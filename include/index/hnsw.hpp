#pragma once

#include "node.hpp"
#include "distance.hpp"
#include "../common/config.hpp"
#include "../storage/mmap_handler.hpp"
#include "../common/spinlock.hpp"
#include "../storage/metadata_handler.hpp"
#include <queue>
#include <vector>
#include <random>
#include <cmath>
#include <algorithm>
#include <omp.h>
#include <mutex>
#include <memory>

namespace nanodb {

    class HNSW {
    public:
        // -----------------------------------------------------------------------
        // Constructor
        // metric: distance function to use for all comparisons in this index.
        //         Must be consistent across insert and search calls.
        // -----------------------------------------------------------------------
        HNSW(MMapHandler& storage,
             const std::string& meta_path = "data/metadata.bin",
             DistanceMetric metric = DistanceMetric::L2)
            : storage_(storage), metric_(metric)
        {
            metadata_storage_.open_file(meta_path);

            std::random_device rd;
            rng_.seed(rd());

            size_t current_count = 0;

            if (storage_.get_size() == 0) {
                entry_point_id_ = -1;
                current_max_layer_ = -1;
                element_count_ = 0;
                current_count = 0;
            } else {
                entry_point_id_ = 0;
                current_max_layer_ = 0;
                element_count_ = storage_.get_size() / sizeof(Node);
                current_count = element_count_;
            }

            node_locks_.reserve(current_count + 10000);
            for (size_t i = 0; i < current_count + 10000; ++i) {
                node_locks_.push_back(std::make_unique<SpinLock>());
            }
        }

        // -----------------------------------------------------------------------
        // Insert a vector with an integer ID and optional metadata string.
        // Thread-safe: multiple threads may call insert() concurrently.
        // -----------------------------------------------------------------------
        void insert(const std::vector<float>& vec_data, id_t id, const std::string& metadata = "") {
            int level = get_random_level();
            Node new_node(id, level, vec_data);

            // Expand storage if needed (double-checked locking)
            size_t offset = (size_t)id * sizeof(Node);
            if (offset + sizeof(Node) > storage_.get_size()) {
                std::lock_guard<std::mutex> lock(global_resize_lock_);
                if (offset + sizeof(Node) > storage_.get_size()) {
                    storage_.resize(storage_.get_size() + 10 * 1024 * 1024);
                    if (id >= node_locks_.size()) {
                        size_t target_size = id + 10000;
                        node_locks_.reserve(target_size);
                        for (size_t i = node_locks_.size(); i < target_size; ++i) {
                            node_locks_.push_back(std::make_unique<SpinLock>());
                        }
                    }
                }
            }

            Node* node_ptr = get_node(id);
            *node_ptr = new_node;

            // Handle first element
            if (entry_point_id_ == -1) {
                std::lock_guard<std::mutex> lock(init_lock_);
                if (entry_point_id_ == -1) {
                    entry_point_id_ = id;
                    current_max_layer_ = level;
                    #pragma omp atomic
                    element_count_++;
                    if (!metadata.empty()) metadata_storage_.save_metadata(id, metadata);
                    return;
                }
            }

            // Greedy search from entry point down to node's level
            id_t curr_obj = entry_point_id_;
            float dist = compute_distance(node_ptr->vector, get_node(curr_obj)->vector,
                                          config::VECTOR_DIM, metric_);

            for (int l = current_max_layer_; l > level; l--) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    Node* curr_node = get_node(curr_obj);
                    for (int i = 0; i < curr_node->neighbor_counts[l]; i++) {
                        id_t n_id = curr_node->neighbors[l][i];
                        float d = compute_distance(node_ptr->vector, get_node(n_id)->vector,
                                                   config::VECTOR_DIM, metric_);
                        if (d < dist) { dist = d; curr_obj = n_id; changed = true; }
                    }
                }
            }

            // Connect neighbors at each layer
            for (int l = std::min(level, current_max_layer_); l >= 0; l--) {
                std::priority_queue<Result> candidates =
                    search_layer(curr_obj, node_ptr->vector, config::EF_CONSTRUCTION, l);

                std::vector<id_t> selected_neighbors;
                while (!candidates.empty() && selected_neighbors.size() < (size_t)config::M) {
                    selected_neighbors.push_back(candidates.top().id);
                    candidates.pop();
                }

                for (id_t neighbor_id : selected_neighbors) {
                    add_link(id, neighbor_id, l);
                    add_link(neighbor_id, id, l);
                }

                if (!selected_neighbors.empty()) curr_obj = selected_neighbors[0];
            }

            if (level > current_max_layer_) {
                entry_point_id_ = id;
                current_max_layer_ = level;
            }

            #pragma omp atomic
            element_count_++;

            if (!metadata.empty()) {
                metadata_storage_.save_metadata(id, metadata);
            }
        }

        // -----------------------------------------------------------------------
        // Search for the k nearest neighbors of a query vector.
        // Deleted (tombstoned) nodes are silently skipped.
        // -----------------------------------------------------------------------
        std::vector<Result> search(const std::vector<float>& query, int k) {
            if (entry_point_id_ == -1) return {};

            id_t curr_obj = entry_point_id_;
            float dist = compute_distance(query.data(), get_node(curr_obj)->vector,
                                          config::VECTOR_DIM, metric_);

            for (int l = current_max_layer_; l > 0; l--) {
                bool changed = true;
                while (changed) {
                    changed = false;
                    Node* curr_node = get_node(curr_obj);
                    for (int i = 0; i < curr_node->neighbor_counts[l]; i++) {
                        id_t n_id = curr_node->neighbors[l][i];
                        float d = compute_distance(query.data(), get_node(n_id)->vector,
                                                   config::VECTOR_DIM, metric_);
                        if (d < dist) { dist = d; curr_obj = n_id; changed = true; }
                    }
                }
            }

            int ef_search = std::max(100, k);
            std::priority_queue<Result> top_candidates =
                search_layer(curr_obj, query.data(), ef_search, 0);

            std::vector<Result> results;
            while (!top_candidates.empty()) {
                Result r = top_candidates.top();
                top_candidates.pop();
                // Skip tombstoned nodes
                Node* n = get_node(r.id);
                if (n->is_deleted) continue;
                r.metadata = metadata_storage_.get_metadata(r.id);
                results.push_back(r);
            }
            std::reverse(results.begin(), results.end());
            if (results.size() > (size_t)k) results.resize(k);

            return results;
        }

        // -----------------------------------------------------------------------
        // Lazy deletion (tombstoning).
        //
        // Design tradeoff: True graph repair on deletion would require re-linking
        // all neighbors of the deleted node — O(M * ef_construction) work per
        // deletion, and it must be done under locks, serializing concurrent inserts.
        // Most production systems (including FAISS IVF) use lazy deletion instead:
        //   - O(1) deletion cost
        //   - Deleted nodes remain in the graph structure but are filtered at query time
        //   - Recall degrades slightly as the fraction of deleted nodes grows
        //   - Periodic "compaction" (rebuild) is the standard remedy
        // -----------------------------------------------------------------------
        void delete_vector(id_t id) {
            size_t offset = (size_t)id * sizeof(Node);
            if (offset + sizeof(Node) > storage_.get_size()) return; // ID out of range
            Node* node = get_node(id);
            node->is_deleted = true;
        }

        // Helper: retrieve metadata string for a given ID
        std::string get_metadata(id_t id) {
            return metadata_storage_.get_metadata(id);
        }

        // Helper: check if a node has been deleted
        bool is_deleted(id_t id) {
            size_t offset = (size_t)id * sizeof(Node);
            if (offset + sizeof(Node) > storage_.get_size()) return false;
            return get_node(id)->is_deleted;
        }

        // Helper: current number of live (non-deleted) elements
        size_t size() const { return element_count_; }

        // Helper: the distance metric this index was built with
        DistanceMetric metric() const { return metric_; }

    private:
        MMapHandler& storage_;
        MetadataHandler metadata_storage_;
        DistanceMetric metric_;
        id_t entry_point_id_ = -1;
        int current_max_layer_ = -1;
        size_t element_count_ = 0;
        std::mt19937 rng_;
        std::mutex init_lock_;

        std::vector<std::unique_ptr<SpinLock>> node_locks_;
        std::mutex global_resize_lock_;

        Node* get_node(id_t id) {
            return reinterpret_cast<Node*>((char*)storage_.get_data() + (size_t)id * sizeof(Node));
        }

        int get_random_level() {
            std::uniform_real_distribution<double> dist(0.0, 1.0);
            double r = dist(rng_);
            int level = 0;
            while (r < 0.03 && level < config::M) {
                level++;
                r = dist(rng_);
            }
            return level;
        }

        // Core beam-search within a single HNSW layer.
        // Tombstoned nodes are skipped during neighbor expansion.
        std::priority_queue<Result> search_layer(id_t entry_point, const float* query_vec,
                                                  int ef, int layer) {
            std::vector<bool> visited(std::max((size_t)entry_point, element_count_) + 2000, false);
            std::priority_queue<Result, std::vector<Result>, std::greater<Result>> candidates;
            std::priority_queue<Result> found_results;

            float d = compute_distance(query_vec, get_node(entry_point)->vector,
                                       config::VECTOR_DIM, metric_);
            Result start_node = {entry_point, d};
            candidates.push(start_node);
            // Only add to found_results if not deleted
            if (!get_node(entry_point)->is_deleted) found_results.push(start_node);
            if (entry_point < visited.size()) visited[entry_point] = true;

            while (!candidates.empty()) {
                Result curr = candidates.top();
                candidates.pop();

                if (!found_results.empty() &&
                    curr.distance > found_results.top().distance &&
                    found_results.size() >= (size_t)ef) break;

                Node* curr_node = get_node(curr.id);
                for (int i = 0; i < curr_node->neighbor_counts[layer]; i++) {
                    id_t neighbor_id = curr_node->neighbors[layer][i];
                    if (neighbor_id >= visited.size() || visited[neighbor_id]) continue;
                    visited[neighbor_id] = true;

                    float dist = compute_distance(query_vec, get_node(neighbor_id)->vector,
                                                  config::VECTOR_DIM, metric_);
                    if (found_results.size() < (size_t)ef || dist < found_results.top().distance) {
                        candidates.push({neighbor_id, dist});
                        // Only add live nodes to results
                        if (!get_node(neighbor_id)->is_deleted) {
                            found_results.push({neighbor_id, dist});
                            if (found_results.size() > (size_t)ef) found_results.pop();
                        }
                    }
                }
            }
            return found_results;
        }

        void add_link(id_t src, id_t dest, int layer) {
            if (src >= node_locks_.size()) return;
            node_locks_[src]->lock();

            Node* node = get_node(src);
            int count = node->neighbor_counts[layer];
            int max_conn = (layer == 0) ? config::M_MAX0 : config::M;

            if (count < max_conn) {
                node->neighbors[layer][count] = dest;
                node->neighbor_counts[layer]++;
            } else {
                // Replace the farthest neighbor if the new one is closer
                float dest_dist = compute_distance(node->vector, get_node(dest)->vector,
                                                   config::VECTOR_DIM, metric_);
                float max_d = -1.0f;
                int max_idx = -1;

                for (int i = 0; i < count; ++i) {
                    float d = compute_distance(node->vector, get_node(node->neighbors[layer][i])->vector,
                                               config::VECTOR_DIM, metric_);
                    if (d > max_d) { max_d = d; max_idx = i; }
                }

                if (dest_dist < max_d && max_idx != -1) {
                    node->neighbors[layer][max_idx] = dest;
                }
            }

            node_locks_[src]->unlock();
        }
    };

} // namespace nanodb
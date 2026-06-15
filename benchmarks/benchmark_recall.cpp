#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <set>
#include <filesystem>

#include "../include/config/constants.hpp"
#include "../include/config/types.hpp"
#include "../include/storage/memory_map.hpp"
#include "../include/index/hnsw.hpp"
#include "../include/index/distance.hpp"

// ============================================================================
// NanoDB Recall vs. Latency Benchmark
//
// Sweeps ef_search from 10 to 500 and measures:
//   - recall@10: fraction of true top-10 neighbors found
//   - mean query latency (microseconds)
//
// Output is CSV-formatted for easy plotting in Python/Excel:
//   ef_search, recall@10, latency_us
//
// Use a real dataset (SIFT-1M, GloVe-100) for publication-quality results.
// This benchmark uses synthetic random data as a reproducible baseline.
// ============================================================================

using namespace nanodb;
using namespace std;
namespace fs = std::filesystem;

static vector<float> make_random_vector(mt19937& rng, size_t dim) {
    uniform_real_distribution<float> dist(0.0f, 1.0f);
    vector<float> v(dim);
    for (auto& x : v) x = dist(rng);
    return v;
}

// Brute-force exact top-k for ground truth
static vector<id_t> brute_force_knn(const vector<float>& query,
                                      const vector<vector<float>>& dataset,
                                      int k) {
    vector<pair<float, id_t>> dists;
    dists.reserve(dataset.size());
    for (size_t i = 0; i < dataset.size(); ++i) {
        float d = l2_distance(query.data(), dataset[i].data(), query.size());
        dists.push_back({d, (id_t)i});
    }
    sort(dists.begin(), dists.end());
    vector<id_t> result;
    for (int i = 0; i < k && i < (int)dists.size(); ++i)
        result.push_back(dists[i].second);
    return result;
}

int main() {
    const int NUM_VECTORS  = 100000;
    const int NUM_QUERIES  = 100;
    const int K            = 10;
    const size_t DIM       = config::VECTOR_DIM;

    cout << "# NanoDB Recall vs. Latency Benchmark\n";
    cout << "# Vectors: " << NUM_VECTORS << "  |  Queries: " << NUM_QUERIES
         << "  |  Dim: " << DIM << "d  |  k=" << K << "\n";
    cout << "ef_search,recall@" << K << ",latency_us\n";

    // Build dataset
    mt19937 rng(42);
    vector<vector<float>> dataset(NUM_VECTORS);
    for (auto& v : dataset) v = make_random_vector(rng, DIM);

    vector<vector<float>> queries(NUM_QUERIES);
    for (auto& q : queries) q = make_random_vector(rng, DIM);

    // Compute ground truth
    vector<vector<id_t>> ground_truth(NUM_QUERIES);
    for (int i = 0; i < NUM_QUERIES; ++i)
        ground_truth[i] = brute_force_knn(queries[i], dataset, K);

    // Build index
    fs::create_directories("data");
    string db_path   = "data/recall_bench.ndb";
    string meta_path = "data/recall_bench.bin";
    fs::remove(db_path);
    fs::remove(meta_path);

    MMapHandler storage;
    storage.open_file(db_path, 200 * 1024 * 1024); // 200MB for 100k vectors
    HNSW index(storage, meta_path, DistanceMetric::L2);

    for (int i = 0; i < NUM_VECTORS; ++i)
        index.insert(dataset[i], (id_t)i);

    // Sweep ef_search
    for (int ef : {10, 20, 40, 60, 80, 100, 150, 200, 300, 500}) {
        double total_recall = 0.0;
        double total_latency_us = 0.0;

        for (int q = 0; q < NUM_QUERIES; ++q) {
            auto t0 = chrono::high_resolution_clock::now();

            // Temporarily override ef by passing it as k (ef = max(ef, k) in search)
            // We expose ef_search by passing ef as k and relying on the internal max(100,k)
            // For a proper sweep we call search with k=ef and take top-K from results
            auto results = index.search(queries[q], ef);

            auto t1 = chrono::high_resolution_clock::now();
            double us = chrono::duration<double, micro>(t1 - t0).count();
            total_latency_us += us;

            // Compute recall@K
            set<id_t> gt_set(ground_truth[q].begin(), ground_truth[q].end());
            int hits = 0;
            for (int r = 0; r < K && r < (int)results.size(); ++r) {
                if (gt_set.count(results[r].id)) ++hits;
            }
            total_recall += (double)hits / K;
        }

        double mean_recall  = total_recall / NUM_QUERIES;
        double mean_latency = total_latency_us / NUM_QUERIES;

        cout << ef << ","
             << fixed << setprecision(4) << mean_recall << ","
             << fixed << setprecision(2) << mean_latency << "\n";
    }

    storage.close_file();
    fs::remove(db_path);
    fs::remove(meta_path);

    return 0;
}

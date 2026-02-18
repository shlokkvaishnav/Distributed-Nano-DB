#pragma once

#include "../common/types.hpp"
#include "../common/config.hpp"
#include <cstring>
#include <vector>

namespace nanodb {

    // Max layers for HNSW graph (4 is sufficient for ~1M vectors)
    constexpr int MAX_LAYERS = 4;

    // --- Node Structure ---
    // alignas(32) ensures the struct starts on a 32-byte boundary in memory.
    // This allows AVX2 to use aligned load instructions (vmovaps) which are faster.
    struct alignas(32) Node {
        
        // Header
        id_t id;            // External Identifier
        int max_layer;      // Highest layer this node participates in
        bool is_deleted;    // Tombstone flag — set by delete_vector(), filtered in search()


        // Vector Data
        // Stored inline for locality (no pointer chasing)
        val_t vector[config::VECTOR_DIM];


        // Graph Connectivity
        // Neighbors for each layer. 
        // We statically allocate M_MAX0 slots for all layers to keep the struct POD (Plain Old Data)
        // for easy serialization to disk.
        id_t neighbors[MAX_LAYERS][config::M_MAX0];
        int neighbor_counts[MAX_LAYERS];


        // Constructors
        Node() = default; // Needed for casting raw memory

        Node(id_t external_id, int level, const std::vector<float>& vec_data) 
            : id(external_id), max_layer(level), is_deleted(false) {
            
            // Safe copy of vector data
            size_t copy_size = (vec_data.size() > config::VECTOR_DIM) ? config::VECTOR_DIM : vec_data.size();
            std::memcpy(vector, vec_data.data(), copy_size * sizeof(float));

            // Initialize neighbor lists to empty (-1)
            std::memset(neighbor_counts, 0, sizeof(neighbor_counts));
            std::memset(neighbors, -1, sizeof(neighbors));
        }
    };

} // namespace nanodb
#pragma once

#include <cstddef>

namespace nanodb {

    namespace config {

        // Data Dimensions
        // Vector dimension (Keep multiple of 8 for AVX2 optimization)
        constexpr size_t VECTOR_DIM = 128; 

        // Soft limit for pre-allocation estimates
        constexpr size_t MAX_ELEMENTS = 100000; 


        // HNSW Algorithm Hyperparameters
        // Max bidirectional links per element (Range: 12-48). Higher = better recall, more RAM.
        constexpr int M = 16; 

        // Max connections in Layer 0 (Bottom layer needs higher density)
        constexpr int M_MAX0 = M * 2; 

        // Size of candidate list during insertion (Higher = better quality, slower build)
        constexpr int EF_CONSTRUCTION = 200; 

        
        // System Settings
        constexpr char DB_FILE_PATH[] = "data/index.ndb";
        constexpr size_t PAGE_SIZE = 4096; // Standard 4KB page alignment

    } // namespace config

} // namespace nanodb
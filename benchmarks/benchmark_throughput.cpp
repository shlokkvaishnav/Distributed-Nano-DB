#include <iostream>
#include <vector>
#include <random>
#include <chrono>
#include <iomanip>
#include <string>
#include <filesystem>

#include "../include/config/constants.hpp"
#include "../include/config/types.hpp"
#include "../include/storage/memory_map.hpp"
#include "../include/index/hnsw.hpp"

// ============================================================================
// NanoDB Throughput Benchmark
//
// Hardware used (fill in before publishing):
//   CPU:        [e.g. Intel Core i7-12700H, 14 cores, 20 threads]
//   Cache:      [e.g. L1 48KB, L2 1.25MB, L3 24MB]
//   RAM:        [e.g. 32GB DDR5-4800]
//   OS:         [e.g. Windows 11 / Ubuntu 22.04]
//
// Why is the multi-thread speedup sublinear (e.g. 2.88x with 8 threads)?
//   1. Lock contention: the global_resize_lock_ serializes storage expansions.
//      With 8 threads all inserting simultaneously, resize events become a
//      bottleneck even though they are rare.
//   2. Memory bandwidth saturation: each insert writes a full Node (~2KB for
//      128d float32 + neighbor arrays). 8 threads writing concurrently saturate
//      the memory bus before all CPU cores are fully utilized.
//   3. HNSW graph structure: inserting a node requires reading its neighbors'
//      vectors to compute distances. With many concurrent inserts, these reads
//      create cache thrashing across threads.
// ============================================================================

using namespace nanodb;
using namespace std;
namespace fs = std::filesystem;

static vector<float> make_random_vector(mt19937& rng) {
    uniform_real_distribution<float> dist(0.0f, 1.0f);
    vector<float> v(config::VECTOR_DIM);
    for (auto& x : v) x = dist(rng);
    return v;
}

static void run_benchmark(int num_threads, int num_vectors,
                           const vector<vector<float>>& dataset) {
    // Use temp files per thread count to avoid state bleed
    string db_path  = "data/bench_" + to_string(num_threads) + "t.ndb";
    string meta_path = "data/bench_" + to_string(num_threads) + "t.bin";

    // Remove stale files
    fs::remove(db_path);
    fs::remove(meta_path);

    MMapHandler storage;
    storage.open_file(db_path, 200ULL * 1024 * 1024); // 200MB (enough for 100k vectors)
    HNSW index(storage, meta_path, DistanceMetric::L2);

    auto start = chrono::high_resolution_clock::now();

    #pragma omp parallel for num_threads(num_threads) schedule(dynamic, 64)
    for (int i = 0; i < num_vectors; ++i) {
        index.insert(dataset[i], (id_t)i);
    }

    auto end = chrono::high_resolution_clock::now();
    double elapsed = chrono::duration<double>(end - start).count();
    double tps = num_vectors / elapsed;

    cout << "  " << setw(8) << num_threads
         << "  " << setw(10) << fixed << setprecision(3) << elapsed << "s"
         << "  " << setw(12) << fixed << setprecision(0) << tps << " TPS"
         << "\n";

    storage.close_file();
    fs::remove(db_path);
    fs::remove(meta_path);
}

int main() {
    const int NUM_VECTORS = 100000;

    cout << "============================================================\n";
    cout << "  NanoDB Throughput Benchmark\n";
    cout << "  Vectors: " << NUM_VECTORS << "  |  Dim: " << config::VECTOR_DIM << "d (float32)\n";
    cout << "============================================================\n";
    cout << "  Threads    Elapsed        Throughput\n";
    cout << "  -------    -------        ----------\n";

    // Pre-generate dataset (not counted in benchmark time)
    mt19937 rng(42);
    vector<vector<float>> dataset(NUM_VECTORS);
    for (auto& v : dataset) v = make_random_vector(rng);

    fs::create_directories("data");

    // Capture single-thread baseline for speedup calculation
    double baseline_tps = 0.0;

    for (int threads : {1, 2, 4, 8}) {
        // Redirect to capture TPS — simpler: just run and print
        run_benchmark(threads, NUM_VECTORS, dataset);
    }

    cout << "\nNote: Sublinear speedup is expected — see file header for explanation.\n";
    return 0;
}

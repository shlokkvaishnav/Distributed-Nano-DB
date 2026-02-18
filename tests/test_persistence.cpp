#include <iostream>
#include <vector>
#include <random>
#include <cassert>
#include <filesystem>
#include <string>

#include "../include/common/config.hpp"
#include "../include/common/types.hpp"
#include "../include/storage/mmap_handler.hpp"
#include "../include/core/hnsw.hpp"

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT_TRUE(cond, msg) \
    do { \
        ++tests_run; \
        if (!(cond)) { \
            std::cerr << "[FAIL] " << msg << "\n"; \
            ++tests_failed; \
        } else { \
            std::cout << "[PASS] " << msg << "\n"; \
        } \
    } while(0)

using namespace nanodb;
namespace fs = std::filesystem;

static std::vector<float> make_random_vec(std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> v(config::VECTOR_DIM);
    for (auto& x : v) x = dist(rng);
    return v;
}

// ---------------------------------------------------------------------------
// Test: Save → Close → Reopen → Search round-trip
// ---------------------------------------------------------------------------
void test_persistence_round_trip() {
    std::cout << "\n--- Test: Persistence Round-Trip ---\n";

    const std::string db_path   = "data/test_persist.ndb";
    const std::string meta_path = "data/test_persist.bin";
    fs::remove(db_path);
    fs::remove(meta_path);
    fs::create_directories("data");

    const int N = 300;
    std::mt19937 rng(99);
    std::vector<std::vector<float>> vecs(N);

    // Phase 1: Insert and close
    {
        MMapHandler storage;
        storage.open_file(db_path, 50 * 1024 * 1024);
        HNSW index(storage, meta_path, DistanceMetric::L2);

        for (int i = 0; i < N; ++i) {
            vecs[i] = make_random_vec(rng);
            index.insert(vecs[i], (id_t)i, "item_" + std::to_string(i));
        }

        storage.close_file();
        std::cout << "  Phase 1: inserted " << N << " vectors and closed.\n";
    }

    // Phase 2: Reopen and verify
    {
        MMapHandler storage;
        storage.open_file(db_path, 50 * 1024 * 1024);
        HNSW index(storage, meta_path, DistanceMetric::L2);

        std::cout << "  Phase 2: reopened index.\n";

        // Search for a few known vectors
        int hits = 0;
        for (int i = 0; i < 20; ++i) {
            auto results = index.search(vecs[i], 1);
            if (!results.empty() && results[0].id == (id_t)i) ++hits;
        }
        ASSERT_TRUE(hits >= 15, "Persistence: recall >= 75% after reload (20 queries)");

        // Verify metadata survived
        std::string meta = index.get_metadata(5);
        ASSERT_TRUE(meta == "item_5", "Persistence: metadata 'item_5' survives reload");

        storage.close_file();
    }

    fs::remove(db_path);
    fs::remove(meta_path);
}

// ---------------------------------------------------------------------------
// Test: Deletion survives across reopen (tombstone is in mmap'd file)
// ---------------------------------------------------------------------------
void test_deletion_persistence() {
    std::cout << "\n--- Test: Deletion Persistence ---\n";

    const std::string db_path   = "data/test_delpersist.ndb";
    const std::string meta_path = "data/test_delpersist.bin";
    fs::remove(db_path);
    fs::remove(meta_path);
    fs::create_directories("data");

    std::mt19937 rng(77);
    std::vector<std::vector<float>> vecs(100);

    // Phase 1: Insert, delete node 0, close
    {
        MMapHandler storage;
        storage.open_file(db_path, 50 * 1024 * 1024);
        HNSW index(storage, meta_path);

        for (int i = 0; i < 100; ++i) {
            vecs[i] = make_random_vec(rng);
            index.insert(vecs[i], (id_t)i);
        }
        index.delete_vector(0);
        storage.close_file();
    }

    // Phase 2: Reopen — node 0 should still be marked deleted
    {
        MMapHandler storage;
        storage.open_file(db_path, 50 * 1024 * 1024);
        HNSW index(storage, meta_path);

        ASSERT_TRUE(index.is_deleted(0), "Deletion persistence: is_deleted(0) true after reload");

        auto results = index.search(vecs[0], 10);
        bool found = false;
        for (auto& r : results) if (r.id == 0) { found = true; break; }
        ASSERT_TRUE(!found, "Deletion persistence: deleted node absent in search after reload");

        storage.close_file();
    }

    fs::remove(db_path);
    fs::remove(meta_path);
}

int main() {
    std::cout << "=== Persistence Tests ===\n";

    test_persistence_round_trip();
    test_deletion_persistence();

    std::cout << "\n=== Results: " << (tests_run - tests_failed) << "/" << tests_run << " passed ===\n";
    return tests_failed > 0 ? 1 : 0;
}

#include <iostream>
#include <vector>
#include <random>
#include <cassert>
#include <cmath>
#include <set>
#include <filesystem>
#include <string>

#include "../include/config/constants.hpp"
#include "../include/config/types.hpp"
#include "../include/storage/memory_map.hpp"
#include "../include/index/hnsw.hpp"

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

static std::vector<float> make_vec(size_t dim, float val) {
    return std::vector<float>(dim, val);
}

static std::vector<float> make_random_vec(std::mt19937& rng, size_t dim) {
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    std::vector<float> v(dim);
    for (auto& x : v) x = dist(rng);
    return v;
}

// Helper: create a fresh HNSW index in a temp file
struct TempIndex {
    MMapHandler storage;
    std::string db_path;
    std::string meta_path;
    HNSW* index = nullptr;

    TempIndex(const std::string& tag, DistanceMetric m = DistanceMetric::L2) {
        db_path   = "data/test_hnsw_" + tag + ".ndb";
        meta_path = "data/test_hnsw_" + tag + ".bin";
        fs::remove(db_path);
        fs::remove(meta_path);
        fs::create_directories("data");
        storage.open_file(db_path, 50 * 1024 * 1024);
        index = new HNSW(storage, meta_path, m);
    }

    ~TempIndex() {
        delete index;
        storage.close_file();
        fs::remove(db_path);
        fs::remove(meta_path);
    }
};

// ---------------------------------------------------------------------------
// Test 1: Insert and exact-match search (L2)
// ---------------------------------------------------------------------------
void test_insert_and_search_l2() {
    std::cout << "\n--- Test: Insert & Search (L2) ---\n";
    TempIndex t("l2");

    std::mt19937 rng(1);
    const int N = 300;
    std::vector<std::vector<float>> vecs(N);
    for (int i = 0; i < N; ++i) {
        vecs[i] = make_random_vec(rng, config::VECTOR_DIM);
        t.index->insert(vecs[i], (id_t)i, "item_" + std::to_string(i));
    }

    // Search for exact vectors — should find themselves in top-3
    int hits = 0;
    for (int i = 0; i < 30; ++i) {
        auto results = t.index->search(vecs[i], 3);
        for (const auto& r : results) {
            if (r.id == (id_t)i) { ++hits; break; }
        }
    }
    ASSERT_TRUE(hits >= 20, "L2 recall@3 >= 67% (30 queries)");
}

// ---------------------------------------------------------------------------
// Test 2: Insert and exact-match search (Cosine)
// ---------------------------------------------------------------------------
void test_insert_and_search_cosine() {
    std::cout << "\n--- Test: Insert & Search (Cosine) ---\n";
    TempIndex t("cosine", DistanceMetric::Cosine);

    std::mt19937 rng(2);
    const int N = 300;
    std::vector<std::vector<float>> vecs(N);
    for (int i = 0; i < N; ++i) {
        vecs[i] = make_random_vec(rng, config::VECTOR_DIM);
        t.index->insert(vecs[i], (id_t)i);
    }

    int hits = 0;
    for (int i = 0; i < 30; ++i) {
        auto results = t.index->search(vecs[i], 1);
        if (!results.empty() && results[0].id == (id_t)i) ++hits;
    }
    ASSERT_TRUE(hits >= 25, "Cosine exact-match recall >= 83% (30 queries)");
}

// ---------------------------------------------------------------------------
// Test 3: Insert and exact-match search (InnerProduct)
// ---------------------------------------------------------------------------
void test_insert_and_search_ip() {
    std::cout << "\n--- Test: Insert & Search (InnerProduct) ---\n";
    TempIndex t("ip", DistanceMetric::InnerProduct);

    std::mt19937 rng(3);
    const int N = 300;
    std::vector<std::vector<float>> vecs(N);
    for (int i = 0; i < N; ++i) {
        vecs[i] = make_random_vec(rng, config::VECTOR_DIM);
        t.index->insert(vecs[i], (id_t)i);
    }

    int hits = 0;
    for (int i = 0; i < 30; ++i) {
        auto results = t.index->search(vecs[i], 1);
        if (!results.empty() && results[0].id == (id_t)i) ++hits;
    }
    ASSERT_TRUE(hits >= 20, "InnerProduct exact-match recall >= 67% (30 queries)");
}

// ---------------------------------------------------------------------------
// Test 4: Lazy deletion — deleted node must not appear in results
// ---------------------------------------------------------------------------
void test_deletion() {
    std::cout << "\n--- Test: Lazy Deletion ---\n";
    TempIndex t("del");

    const int N = 200;
    std::mt19937 rng(4);
    std::vector<std::vector<float>> vecs(N);
    for (int i = 0; i < N; ++i) {
        vecs[i] = make_random_vec(rng, config::VECTOR_DIM);
        t.index->insert(vecs[i], (id_t)i);
    }

    // Confirm node 0 appears in results before deletion
    auto before = t.index->search(vecs[0], 5);
    bool found_before = false;
    for (auto& r : before) if (r.id == 0) { found_before = true; break; }
    ASSERT_TRUE(found_before, "Deletion: node 0 found before deletion");

    // Delete node 0
    t.index->delete_vector(0);
    ASSERT_TRUE(t.index->is_deleted(0), "Deletion: is_deleted(0) returns true");

    // Node 0 must not appear in results after deletion
    auto after = t.index->search(vecs[0], 10);
    bool found_after = false;
    for (auto& r : after) if (r.id == 0) { found_after = true; break; }
    ASSERT_TRUE(!found_after, "Deletion: node 0 absent after deletion");
}

// ---------------------------------------------------------------------------
// Test 5: Metadata round-trip
// ---------------------------------------------------------------------------
void test_metadata() {
    std::cout << "\n--- Test: Metadata ---\n";
    TempIndex t("meta");

    std::vector<float> v(config::VECTOR_DIM, 0.5f);
    t.index->insert(v, 42, "my_image.jpg");

    std::string meta = t.index->get_metadata(42);
    ASSERT_TRUE(meta == "my_image.jpg", "Metadata: retrieved string matches inserted string");
}

// ---------------------------------------------------------------------------
// Test 6: Empty index search returns empty
// ---------------------------------------------------------------------------
void test_empty_search() {
    std::cout << "\n--- Test: Empty Index Search ---\n";
    TempIndex t("empty");
    std::vector<float> q(config::VECTOR_DIM, 0.0f);
    auto results = t.index->search(q, 5);
    ASSERT_TRUE(results.empty(), "Empty index: search returns empty vector");
}

int main() {
    std::cout << "=== HNSW Tests ===\n";

    test_insert_and_search_l2();
    test_insert_and_search_cosine();
    test_insert_and_search_ip();
    test_deletion();
    test_metadata();
    test_empty_search();

    std::cout << "\n=== Results: " << (tests_run - tests_failed) << "/" << tests_run << " passed ===\n";
    return tests_failed > 0 ? 1 : 0;
}

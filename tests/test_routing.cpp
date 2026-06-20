#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <unordered_map>
#include "../cluster/routing.hpp"

using namespace nanodb::cluster;

void test_fnv1a_64_deterministic() {
    std::string key1 = "test_key_1";
    std::string key2 = "test_key_2";
    
    uint64_t hash1_a = fnv1a_64(key1);
    uint64_t hash1_b = fnv1a_64(key1);
    assert(hash1_a == hash1_b);
    
    uint64_t hash2 = fnv1a_64(key2);
    assert(hash1_a != hash2);
    std::cout << "test_fnv1a_64_deterministic passed.\n";
}

void test_distribution() {
    int num_shards = 3;
    std::unordered_map<size_t, int> shard_counts;
    int num_keys = 10000;
    
    for (int i = 0; i < num_keys; ++i) {
        std::string key = "user_id_" + std::to_string(i);
        size_t shard = route_shard(key, num_shards);
        shard_counts[shard]++;
    }
    
    for (int i = 0; i < num_shards; ++i) {
        // For a well-distributed hash, 10000 items over 3 shards should be ~3333 each
        double ratio = (double)shard_counts[i] / num_keys;
        assert(ratio > 0.30 && ratio < 0.36);
    }
    std::cout << "test_distribution passed.\n";
}

int main() {
    test_fnv1a_64_deterministic();
    test_distribution();
    std::cout << "All routing tests passed.\n";
    return 0;
}

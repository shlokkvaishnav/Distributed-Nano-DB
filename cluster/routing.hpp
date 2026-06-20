#pragma once
#include <string>
#include <cstdint>

namespace nanodb {
namespace cluster {

// FNV-1a 64-bit. Fixed and deterministic across processes, compilers, and
// runs. std::hash<std::string> is explicitly NOT used here — it's only
// guaranteed consistent within a single process, and every coordinator
// instance must route the same external_id to the same shard.
inline uint64_t fnv1a_64(const std::string& s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// Phase 1 routing: plain modulo over a fixed shard count. No virtual nodes,
// no rebalancing -- adding or removing a shard reshuffles almost every key.
// This is intentional and temporary. Phase 2 (consistent hashing) replaces
// this function but reuses fnv1a_64 for the hash ring.
inline size_t route_shard(const std::string& external_id, size_t num_shards) {
    return fnv1a_64(external_id) % num_shards;
}

} // namespace cluster
} // namespace nanodb

#pragma once
#include <string>
#include <vector>
#include <algorithm>
#include <cstdint>
#include "routing.hpp"

namespace nanodb {
namespace cluster {

// Number of virtual nodes per shard. Verified empirically (Phase 2 plan,
// Section 1) to keep load imbalance under ~6% across 3-10 shards and
// migration cost on shard add/remove within a fraction of a percent of the
// theoretical minimum (1/N).
constexpr int VIRTUAL_NODES_PER_SHARD = 200;

// Virtual-node hash points come from chaining fnv1a_64 V times starting
// from hash(shard_id), NOT from hashing a concatenated string like
// "shard_id#v" -- that approach was tested and produced badly clustered,
// poorly balanced rings because FNV-1a gives a difference in only the
// last byte of input just one round of mixing. Chaining forces full
// mixing at every step.
inline uint64_t chained_vnode_point(int shard_id, int vnode_index) {
    uint64_t h = fnv1a_64(std::to_string(shard_id));
    for (int i = 0; i <= vnode_index; ++i) {
        h = fnv1a_64(std::to_string(h));
    }
    return h;
}

class HashRing {
public:
    HashRing() = default;
    explicit HashRing(const std::vector<int>& shard_ids) { build(shard_ids); }

    void build(const std::vector<int>& shard_ids) {
        ring_.clear();
        ring_.reserve(shard_ids.size() * VIRTUAL_NODES_PER_SHARD);
        for (int sid : shard_ids) {
            for (int v = 0; v < VIRTUAL_NODES_PER_SHARD; ++v) {
                ring_.push_back({chained_vnode_point(sid, v), sid});
            }
        }
        std::sort(ring_.begin(), ring_.end());
    }

    // Returns the shard_id that owns external_id under the current ring.
    // Precondition: build() has been called with at least one shard id.
    int route(const std::string& external_id) const {
        uint64_t h = fnv1a_64(external_id);
        auto it = std::lower_bound(
            ring_.begin(), ring_.end(), h,
            [](const std::pair<uint64_t, int>& point, uint64_t target) {
                return point.first < target;
            });
        if (it == ring_.end()) it = ring_.begin();
        return it->second;
    }

    bool empty() const { return ring_.empty(); }
    size_t point_count() const { return ring_.size(); }

private:
    std::vector<std::pair<uint64_t, int>> ring_;
};

} // namespace cluster
} // namespace nanodb

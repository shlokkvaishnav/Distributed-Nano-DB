#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <cstdio>
#include <unistd.h>
#include <atomic>
#include "json.hpp"

namespace nanodb {
namespace cluster {

struct ShardEndpoint {
    int shard_id;
    int replica_id;
    std::string host;
    int port;
    bool is_primary;
    std::string address() const { return host + ":" + std::to_string(port); }
};

inline std::vector<ShardEndpoint> load_cluster_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("cannot open cluster config: " + path);
    }
    nlohmann::json j;
    f >> j;
    std::vector<ShardEndpoint> shards;
    for (const auto& s : j.at("shards")) {
        ShardEndpoint ep;
        ep.shard_id = s.at("shard_id").get<int>();
        ep.replica_id = s.value("replica_id", 0);
        ep.host = s.at("host").get<std::string>();
        ep.port = s.at("port").get<int>();
        ep.is_primary = s.value("primary", true);
        shards.push_back(ep);
    }
    if (shards.empty()) throw std::runtime_error("cluster config has zero shards");
    return shards;
}

inline void save_cluster_config(const std::string& path, const std::vector<ShardEndpoint>& shards) {
    nlohmann::json j;
    j["shards"] = nlohmann::json::array();
    for (const auto& ep : shards) {
        j["shards"].push_back({
            {"shard_id", ep.shard_id},
            {"replica_id", ep.replica_id},
            {"host", ep.host},
            {"port", ep.port},
            {"primary", ep.is_primary}
        });
    }
    // Multiple coordinators commonly share this same path, each calling
    // save_cluster_config() after every rebalance/failover. Writing
    // directly to `path` with std::ofstream truncates it the instant the
    // file is opened, so a concurrent load_cluster_config() can observe a
    // truncated/invalid file mid-write and throw -- which is fatal at
    // coordinator startup. Write to a temp file first, then atomically
    // rename() over the real path, so readers only ever see either the
    // fully-old or fully-new file, never a partial one.
    //
    // The temp filename must be unique per writer/call, not just "per
    // path": if two concurrent writers shared one temp filename, the
    // second writer's open(..., trunc) would truncate the *first* writer's
    // still-in-progress temp file (same inode, same path), interleaving
    // both writes into one file before either side renames -- which would
    // then get renamed into place fully intact but containing garbage.
    // The rename() syscall itself is atomic; the file content it points to
    // also has to be untouched by any other writer, which a shared temp
    // name doesn't guarantee.
    static std::atomic<unsigned long> s_write_counter{0};
    const unsigned long seq = s_write_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string tmp_path = path + "." + std::to_string(::getpid()) + "." +
                                  std::to_string(seq) + ".tmp";
    {
        std::ofstream f(tmp_path, std::ios::trunc);
        if (!f.is_open()) {
            throw std::runtime_error("cannot write cluster config temp file: " + tmp_path);
        }
        f << j.dump(2);
        f.flush();
        if (!f.good()) {
            throw std::runtime_error("error writing cluster config temp file: " + tmp_path);
        }
    }
    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        throw std::runtime_error("cannot atomically rename cluster config into place: " + path);
    }
}

} // namespace cluster
} // namespace nanodb

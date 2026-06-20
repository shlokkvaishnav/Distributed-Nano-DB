#pragma once
#include <string>
#include <vector>
#include <fstream>
#include <stdexcept>
#include "json.hpp"

namespace nanodb {
namespace cluster {

struct ShardEndpoint {
    int shard_id;
    std::string host;
    int port;
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
        ep.host = s.at("host").get<std::string>();
        ep.port = s.at("port").get<int>();
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
            {"host", ep.host},
            {"port", ep.port}
        });
    }
    std::ofstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("cannot write cluster config: " + path);
    }
    f << j.dump(2);
}

} // namespace cluster
} // namespace nanodb

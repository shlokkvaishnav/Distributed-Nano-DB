#include <iostream>
#include <csignal>
#include <cstdlib>
#include <future>
#include <vector>
#include <algorithm>
#include <chrono>

#include "httplib.h"
#include "json.hpp"
#include <grpcpp/grpcpp.h>

#include "nanodb_cluster.grpc.pb.h"
#include "cluster_config.hpp"
#include "routing.hpp"

using json = nlohmann::json;
using namespace nanodb::cluster;

static httplib::Server* g_server = nullptr;
static constexpr int RPC_TIMEOUT_MS = 800;

void signal_handler(int) {
    if (g_server) g_server->stop();
}

struct ShardClient {
    int shard_id;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ShardService::Stub> stub;
};

int main() {
    const char* config_env = std::getenv("NANODB_CLUSTER_CONFIG");
    std::string config_path = config_env ? config_env : "cluster.json";

    const char* port_env = std::getenv("NANODB_HTTP_PORT");
    int http_port = port_env ? std::atoi(port_env) : 8080;

    std::vector<ShardEndpoint> endpoints;
    try {
        endpoints = load_cluster_config(config_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }

    std::vector<ShardClient> shards;
    for (const auto& ep : endpoints) {
        ShardClient sc;
        sc.shard_id = ep.shard_id;
        sc.channel = grpc::CreateChannel(ep.address(), grpc::InsecureChannelCredentials());
        sc.stub = ShardService::NewStub(sc.channel);
        shards.push_back(std::move(sc));
    }
    std::cout << "[Coordinator] Loaded " << shards.size() << " shard(s) from "
              << config_path << std::endl;

    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server.Post("/vectors", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            if (!body.contains("id") || !body.contains("vector")) {
                res.status = 400;
                res.set_content(R"({"error":"missing required fields: id, vector"})", "application/json");
                return;
            }
            std::string external_id = body["id"].is_string()
                ? body["id"].get<std::string>()
                : std::to_string(body["id"].get<long long>());
            size_t idx = route_shard(external_id, shards.size());

            InsertRequest grpc_req;
            grpc_req.set_external_id(external_id);
            for (const auto& v : body["vector"]) grpc_req.add_vector(v.get<float>());
            grpc_req.set_metadata(body.value("metadata", ""));

            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
            InsertResponse grpc_res;
            grpc::Status status = shards[idx].stub->Insert(&ctx, grpc_req, &grpc_res);

            if (!status.ok() || !grpc_res.ok()) {
                res.status = 502;
                json err = {{"error", "shard " + std::to_string(shards[idx].shard_id) +
                                       " insert failed: " +
                                       (status.ok() ? grpc_res.error() : status.error_message())}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.status = 201;
            json ok = {{"status", "ok"}, {"id", external_id}, {"shard", shards[idx].shard_id}};
            res.set_content(ok.dump(), "application/json");
        } catch (const json::exception& e) {
            res.status = 400;
            res.set_content(std::string(R"({"error":"invalid JSON: )") + e.what() + R"("})", "application/json");
        }
    });

    server.Post("/search", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto body = json::parse(req.body);
            if (!body.contains("vector") || !body.contains("k")) {
                res.status = 400;
                res.set_content(R"({"error":"missing required fields: vector, k"})", "application/json");
                return;
            }
            int k = body["k"].get<int>();
            std::vector<float> vec;
            for (const auto& v : body["vector"]) vec.push_back(v.get<float>());

            std::vector<std::future<std::pair<int, SearchResponse>>> futures;
            for (auto& sc : shards) {
                futures.push_back(std::async(std::launch::async, [&sc, vec, k]() {
                    SearchRequest grpc_req;
                    for (float f : vec) grpc_req.add_vector(f);
                    grpc_req.set_k(k);
                    grpc::ClientContext ctx;
                    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
                    SearchResponse grpc_res;
                    grpc::Status status = sc.stub->Search(&ctx, grpc_req, &grpc_res);
                    if (!status.ok()) grpc_res.set_ok(false);
                    return std::make_pair(sc.shard_id, grpc_res);
                }));
            }

            std::vector<json> merged;
            std::vector<int> unavailable;
            for (auto& fut : futures) {
                auto [shard_id, grpc_res] = fut.get();
                if (!grpc_res.ok()) { unavailable.push_back(shard_id); continue; }
                for (const auto& r : grpc_res.results()) {
                    merged.push_back({{"id", r.external_id()}, {"distance", r.distance()}, {"metadata", r.metadata()}});
                }
            }
            std::sort(merged.begin(), merged.end(), [](const json& a, const json& b) {
                return a["distance"].get<float>() < b["distance"].get<float>();
            });
            if (merged.size() > (size_t)k) merged.resize(k);

            json response = {{"results", merged}};
            if (!unavailable.empty()) {
                response["degraded"] = true;
                response["unavailable_shards"] = unavailable;
            }
            res.set_content(response.dump(), "application/json");
        } catch (const json::exception& e) {
            res.status = 400;
            res.set_content(std::string(R"({"error":"invalid JSON: )") + e.what() + R"("})", "application/json");
        }
    });

    server.Delete(R"(/vectors/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
        std::string external_id = req.matches[1];
        size_t idx = route_shard(external_id, shards.size());
        DeleteRequest grpc_req;
        grpc_req.set_external_id(external_id);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
        DeleteResponse grpc_res;
        grpc::Status status = shards[idx].stub->Delete(&ctx, grpc_req, &grpc_res);
        if (!status.ok() || !grpc_res.ok()) {
            res.status = 404;
            res.set_content(R"({"error":"not found or shard unreachable"})", "application/json");
            return;
        }
        res.set_content(R"({"status":"ok","id":")" + external_id + "\"}", "application/json");
    });

    server.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        json per_shard = json::array();
        uint64_t total = 0;
        std::vector<int> unavailable;
        for (auto& sc : shards) {
            StatsRequest grpc_req;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
            StatsResponse grpc_res;
            grpc::Status status = sc.stub->Stats(&ctx, grpc_req, &grpc_res);
            if (!status.ok()) { unavailable.push_back(sc.shard_id); continue; }
            per_shard.push_back({{"shard_id", sc.shard_id}, {"element_count", grpc_res.element_count()}});
            total += grpc_res.element_count();
        }
        json response = {{"total_element_count", total}, {"shards", per_shard}, {"num_shards", shards.size()}};
        if (!unavailable.empty()) {
            response["degraded"] = true;
            response["unavailable_shards"] = unavailable;
        }
        res.set_content(response.dump(), "application/json");
    });

    std::cout << "[Coordinator] Listening on 0.0.0.0:" << http_port << std::endl;
    server.listen("0.0.0.0", http_port);
    std::cout << "[Coordinator] Stopped." << std::endl;
    return 0;
}

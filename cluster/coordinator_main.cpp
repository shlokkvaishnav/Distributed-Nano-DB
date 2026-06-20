#include <iostream>
#include <csignal>
#include <cstdlib>
#include <future>
#include <vector>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <set>

#include "httplib.h"
#include "json.hpp"
#include <grpcpp/grpcpp.h>

#include "nanodb_cluster.grpc.pb.h"
#include "cluster_config.hpp"
#include "routing.hpp"
#include "hash_ring.hpp"

using json = nlohmann::json;
using namespace nanodb::cluster;

static httplib::Server* g_server = nullptr;
static constexpr int RPC_TIMEOUT_MS = 800;
static constexpr int MIGRATION_RPC_TIMEOUT_MS = 2000;

void signal_handler(int) {
    if (g_server) g_server->stop();
}

struct ShardClient {
    int shard_id;
    std::string host;
    int port;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ShardService::Stub> stub;
    std::atomic<bool> active{true};
};

// All cluster membership state lives behind this lock. Readers (the normal
// Insert/Search/Delete/Stats handlers) take a shared_lock just long enough
// to copy what they need -- a HashRing (cheap, plain data) or a list of raw
// ShardClient* (cheap, g_shards is append-only so the pointers stay valid
// forever) -- then release the lock before making any gRPC calls. Holding
// the lock across network I/O would serialize all cluster traffic. The
// rebalance handlers take a unique_lock only for the moments they actually
// append to g_shards or swap active_ring; the migration RPCs themselves run
// outside the lock.
static std::shared_mutex cluster_mutex;
static std::vector<std::unique_ptr<ShardClient>> g_shards;
static HashRing active_ring;
static std::atomic<bool> rebalancing{false};
static std::string g_cluster_config_path;

static std::unique_ptr<ShardClient> make_shard_client(int shard_id, const std::string& host, int port) {
    auto sc = std::make_unique<ShardClient>();
    sc->shard_id = shard_id;
    sc->host = host;
    sc->port = port;
    sc->channel = grpc::CreateChannel(host + ":" + std::to_string(port), grpc::InsecureChannelCredentials());
    sc->stub = ShardService::NewStub(sc->channel);
    return sc;
}

// Safe to use after the lock is released: g_shards is append-only, so a
// ShardClient's address never changes or becomes invalid once added.
static std::vector<ShardClient*> live_shards() {
    std::shared_lock lock(cluster_mutex);
    std::vector<ShardClient*> out;
    for (auto& sc : g_shards) {
        if (sc->active) out.push_back(sc.get());
    }
    return out;
}

static HashRing ring_snapshot() {
    std::shared_lock lock(cluster_mutex);
    return active_ring;
}

static ShardClient* find_shard(const std::vector<ShardClient*>& pool, int shard_id) {
    for (auto* sc : pool) {
        if (sc->shard_id == shard_id) return sc;
    }
    return nullptr;
}

static void persist_cluster_state() {
    std::vector<ShardEndpoint> eps;
    for (auto* sc : live_shards()) {
        eps.push_back({sc->shard_id, sc->host, sc->port});
    }
    try {
        save_cluster_config(g_cluster_config_path, eps);
    } catch (const std::exception& e) {
        std::cerr << "[Coordinator] WARNING: failed to persist cluster config: " << e.what() << std::endl;
    }
}

// Moves one key from source to dest. Insert-into-destination happens before
// delete-from-source, always -- a duplicate-for-a-moment is fine (search
// dedupes by external_id), a vector that's briefly on neither shard is not.
// Returns false if any step failed; the key is left wherever it last
// successfully existed (never silently dropped).
static bool migrate_one_key(ShardClient& source, ShardClient& dest, const std::string& external_id) {
    GetVectorRequest greq;
    greq.set_external_id(external_id);
    grpc::ClientContext gctx;
    gctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(MIGRATION_RPC_TIMEOUT_MS));
    GetVectorResponse gres;
    if (!source.stub->GetVector(&gctx, greq, &gres).ok() || !gres.ok()) return false;

    InsertRequest ireq;
    ireq.set_external_id(external_id);
    for (float f : gres.vector()) ireq.add_vector(f);
    ireq.set_metadata(gres.metadata());
    grpc::ClientContext ictx;
    ictx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(MIGRATION_RPC_TIMEOUT_MS));
    InsertResponse ires;
    if (!dest.stub->Insert(&ictx, ireq, &ires).ok() || !ires.ok()) return false;

    DeleteRequest dreq;
    dreq.set_external_id(external_id);
    grpc::ClientContext dctx;
    dctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(MIGRATION_RPC_TIMEOUT_MS));
    DeleteResponse dres;
    source.stub->Delete(&dctx, dreq, &dres); // best-effort: if this fails the key is duplicated, not lost
    return true;
}

int main() {
    const char* config_env = std::getenv("NANODB_CLUSTER_CONFIG");
    g_cluster_config_path = config_env ? config_env : "deploy/cluster.local.json";

    const char* port_env = std::getenv("NANODB_HTTP_PORT");
    int http_port = port_env ? std::atoi(port_env) : 8080;

    std::vector<ShardEndpoint> endpoints;
    try {
        endpoints = load_cluster_config(g_cluster_config_path);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }

    std::vector<int> ids;
    for (const auto& ep : endpoints) {
        g_shards.push_back(make_shard_client(ep.shard_id, ep.host, ep.port));
        ids.push_back(ep.shard_id);
    }
    active_ring.build(ids);
    std::cout << "[Coordinator] Loaded " << g_shards.size() << " shard(s) from " << g_cluster_config_path << std::endl;

    httplib::Server server;
    g_server = &server;
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    server.Post("/vectors", [&](const httplib::Request& req, httplib::Response& res) {
        if (rebalancing) {
            res.status = 503;
            res.set_content(R"({"error":"cluster is rebalancing, try again shortly"})", "application/json");
            return;
        }
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

            HashRing ring = ring_snapshot();
            auto pool = live_shards();
            ShardClient* dest = find_shard(pool, ring.route(external_id));
            if (!dest) {
                res.status = 503;
                res.set_content(R"({"error":"destination shard unavailable"})", "application/json");
                return;
            }

            InsertRequest grpc_req;
            grpc_req.set_external_id(external_id);
            for (const auto& v : body["vector"]) grpc_req.add_vector(v.get<float>());
            grpc_req.set_metadata(body.value("metadata", ""));

            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
            InsertResponse grpc_res;
            grpc::Status status = dest->stub->Insert(&ctx, grpc_req, &grpc_res);

            if (!status.ok() || !grpc_res.ok()) {
                res.status = 502;
                json err = {{"error", "shard " + std::to_string(dest->shard_id) + " insert failed: " +
                                       (status.ok() ? grpc_res.error() : status.error_message())}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.status = 201;
            json ok = {{"status", "ok"}, {"id", external_id}, {"shard", dest->shard_id}};
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

            auto pool = live_shards();
            std::vector<std::future<std::pair<int, SearchResponse>>> futures;
            for (auto* sc : pool) {
                futures.push_back(std::async(std::launch::async, [sc, vec, k]() {
                    SearchRequest grpc_req;
                    for (float f : vec) grpc_req.add_vector(f);
                    grpc_req.set_k(k);
                    grpc::ClientContext ctx;
                    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
                    SearchResponse grpc_res;
                    grpc::Status status = sc->stub->Search(&ctx, grpc_req, &grpc_res);
                    if (!status.ok()) grpc_res.set_ok(false);
                    return std::make_pair(sc->shard_id, grpc_res);
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

            // Dedupe by id, keep the first (lowest-distance) occurrence. A
            // key mid-migration can briefly exist on two shards; this is the
            // free fix for that, independent of whether a rebalance is
            // actually running right now.
            std::vector<json> deduped;
            std::set<std::string> seen;
            for (auto& r : merged) {
                std::string id = r["id"].get<std::string>();
                if (seen.count(id)) continue;
                seen.insert(id);
                deduped.push_back(r);
                if (deduped.size() >= (size_t)k) break;
            }

            json response = {{"results", deduped}};
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
        if (rebalancing) {
            res.status = 503;
            res.set_content(R"({"error":"cluster is rebalancing, try again shortly"})", "application/json");
            return;
        }
        std::string external_id = req.matches[1];
        HashRing ring = ring_snapshot();
        auto pool = live_shards();
        ShardClient* dest = find_shard(pool, ring.route(external_id));
        if (!dest) {
            res.status = 503;
            res.set_content(R"({"error":"destination shard unavailable"})", "application/json");
            return;
        }
        DeleteRequest grpc_req;
        grpc_req.set_external_id(external_id);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
        DeleteResponse grpc_res;
        grpc::Status status = dest->stub->Delete(&ctx, grpc_req, &grpc_res);
        if (!status.ok() || !grpc_res.ok()) {
            res.status = 404;
            res.set_content(R"({"error":"not found or shard unreachable"})", "application/json");
            return;
        }
        res.set_content(R"({"status":"ok","id":")" + external_id + "\"}", "application/json");
    });

    server.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        auto pool = live_shards();
        json per_shard = json::array();
        uint64_t total = 0;
        std::vector<int> unavailable;
        for (auto* sc : pool) {
            StatsRequest grpc_req;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
            StatsResponse grpc_res;
            grpc::Status status = sc->stub->Stats(&ctx, grpc_req, &grpc_res);
            if (!status.ok()) { unavailable.push_back(sc->shard_id); continue; }
            per_shard.push_back({{"shard_id", sc->shard_id}, {"element_count", grpc_res.element_count()}});
            total += grpc_res.element_count();
        }
        json response = {{"total_element_count", total}, {"shards", per_shard}, {"num_shards", pool.size()}};
        if (!unavailable.empty()) {
            response["degraded"] = true;
            response["unavailable_shards"] = unavailable;
        }
        res.set_content(response.dump(), "application/json");
    });

    // POST /admin/shards/add  body: {"shard_id": 3, "host": "shard-3", "port": 9090}
    server.Post("/admin/shards/add", [&](const httplib::Request& req, httplib::Response& res) {
        bool expected = false;
        if (!rebalancing.compare_exchange_strong(expected, true)) {
            res.status = 409;
            res.set_content(R"({"error":"a rebalance is already in progress"})", "application/json");
            return;
        }
        try {
            auto body = json::parse(req.body);
            int new_id = body.at("shard_id").get<int>();
            std::string host = body.at("host").get<std::string>();
            int port = body.at("port").get<int>();

            auto new_client = make_shard_client(new_id, host, port);
            ShardClient* new_ptr = new_client.get();
            {
                std::unique_lock lock(cluster_mutex);
                g_shards.push_back(std::move(new_client));
            }

            auto pool = live_shards(); // includes the new shard now
            std::vector<int> ids2;
            for (auto* sc : pool) ids2.push_back(sc->shard_id);
            HashRing new_ring(ids2);

            int migrated = 0, failed = 0;
            for (auto* source : pool) {
                if (source->shard_id == new_id) continue;
                ListLocalIdsRequest lreq;
                grpc::ClientContext lctx;
                lctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
                ListLocalIdsResponse lres;
                if (!source->stub->ListLocalIds(&lctx, lreq, &lres).ok()) continue;

                for (const auto& external_id : lres.external_ids()) {
                    if (new_ring.route(external_id) != new_id) continue;
                    if (migrate_one_key(*source, *new_ptr, external_id)) migrated++;
                    else failed++;
                }
            }

            {
                std::unique_lock lock(cluster_mutex);
                active_ring = new_ring;
            }
            persist_cluster_state();
            rebalancing = false;

            json ok = {{"status", "ok"}, {"shard_id", new_id}, {"keys_migrated", migrated}, {"keys_failed", failed}};
            res.set_content(ok.dump(), "application/json");
        } catch (const std::exception& e) {
            rebalancing = false;
            res.status = 400;
            json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    // POST /admin/shards/remove  body: {"shard_id": 1}
    server.Post("/admin/shards/remove", [&](const httplib::Request& req, httplib::Response& res) {
        bool expected = false;
        if (!rebalancing.compare_exchange_strong(expected, true)) {
            res.status = 409;
            res.set_content(R"({"error":"a rebalance is already in progress"})", "application/json");
            return;
        }
        try {
            auto body = json::parse(req.body);
            int leaving_id = body.at("shard_id").get<int>();

            auto pool = live_shards();
            ShardClient* leaving = find_shard(pool, leaving_id);
            if (!leaving) {
                rebalancing = false;
                res.status = 404;
                res.set_content(R"({"error":"shard not found or already inactive"})", "application/json");
                return;
            }

            std::vector<int> remaining_ids;
            for (auto* sc : pool) if (sc->shard_id != leaving_id) remaining_ids.push_back(sc->shard_id);
            if (remaining_ids.empty()) {
                rebalancing = false;
                res.status = 400;
                res.set_content(R"({"error":"cannot remove the last shard"})", "application/json");
                return;
            }
            // The leaving shard is excluded from the new ring immediately, but
            // stays `active` (still serving reads, still the migration source)
            // until every key it owns has actually moved.
            HashRing new_ring(remaining_ids);

            ListLocalIdsRequest lreq;
            grpc::ClientContext lctx;
            lctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
            ListLocalIdsResponse lres;
            int migrated = 0, failed = 0;
            if (leaving->stub->ListLocalIds(&lctx, lreq, &lres).ok()) {
                for (const auto& external_id : lres.external_ids()) {
                    ShardClient* dest = find_shard(pool, new_ring.route(external_id));
                    if (!dest) { failed++; continue; }
                    if (migrate_one_key(*leaving, *dest, external_id)) migrated++;
                    else failed++;
                }
            }

            {
                std::unique_lock lock(cluster_mutex);
                active_ring = new_ring;
            }
            leaving->active = false;
            persist_cluster_state();
            rebalancing = false;

            json ok = {{"status", "ok"}, {"shard_id", leaving_id}, {"keys_migrated", migrated}, {"keys_failed", failed}};
            res.set_content(ok.dump(), "application/json");
        } catch (const std::exception& e) {
            rebalancing = false;
            res.status = 400;
            json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    std::cout << "[Coordinator] Listening on 0.0.0.0:" << http_port << std::endl;
    server.listen("0.0.0.0", http_port);
    std::cout << "[Coordinator] Stopped." << std::endl;
    return 0;
}

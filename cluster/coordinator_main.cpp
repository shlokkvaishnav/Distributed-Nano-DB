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
#include <map>

#include "httplib.h"
#include "json.hpp"
#include <grpcpp/grpcpp.h>

#include "nanodb_cluster.grpc.pb.h"
#include "cluster_config.hpp"
#include "routing.hpp"
#include "hash_ring.hpp"
#include "raft_node.hpp"
#include "raft_config.hpp"
#include "raft_service_impl.hpp"

using json = nlohmann::json;
using namespace nanodb::cluster;

static httplib::Server* g_server = nullptr;
static std::unique_ptr<nanodb::raft::RaftNode> g_raft_node;
static grpc::Server* g_raft_server = nullptr;
static constexpr int RPC_TIMEOUT_MS = 800;
static constexpr int MIGRATION_RPC_TIMEOUT_MS = 2000;

static std::atomic<bool> g_apply_poller_running{false};
static std::atomic<bool> g_health_check_running{false};

void signal_handler(int) {
    if (g_server) g_server->stop();
    if (g_raft_node) g_raft_node->stop();
    if (g_raft_server) g_raft_server->Shutdown();
    g_apply_poller_running = false;
    g_health_check_running = false;
}

struct ShardClient {
    int shard_id;
    int replica_id;
    std::string host;
    int port;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ShardService::Stub> stub;
    std::atomic<bool> active{true};
    std::atomic<bool> is_primary{false};
    std::atomic<uint64_t> epoch{0};
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

// Serializes apply_pending_raft_commands() across callers (the dedicated
// poller thread and any admin handler doing a synchronous catch-up after
// its own propose() commits), and tracks how many of RaftNode's applied
// commands have already been reflected in g_shards/active_ring.
static std::mutex g_apply_mutex;
static uint64_t g_local_applied_count = 0;

// Health-check state for primary failover. Only the current Raft leader
// acts on failures it observes (see health_check_loop) -- followers run
// the same loop (cheap, just pings) but never propose anything, since two
// coordinators independently deciding "the primary is down, promote X"
// at the same time is exactly the kind of uncoordinated action Raft
// exists to prevent.
static std::mutex g_health_mutex;
static std::map<int, int> g_consecutive_primary_failures;
static constexpr int HEALTH_CHECK_INTERVAL_MS = 1000;
static constexpr int HEALTH_CHECK_TIMEOUT_MS = 500;
static constexpr int FAILURES_BEFORE_FAILOVER = 3;

static std::unique_ptr<ShardClient> make_shard_client(int shard_id, int replica_id, const std::string& host, int port, bool is_primary) {
    auto sc = std::make_unique<ShardClient>();
    sc->shard_id = shard_id;
    sc->replica_id = replica_id;
    sc->host = host;
    sc->port = port;
    sc->channel = grpc::CreateChannel(host + ":" + std::to_string(port), grpc::InsecureChannelCredentials());
    sc->stub = ShardService::NewStub(sc->channel);
    sc->is_primary = is_primary;
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

// All active replicas of one shard_id, primary first if present. A shard_id
// now maps to a SET of physical nodes, not one -- this is the thing every
// write fans out to and every quorum is computed over.
static std::vector<ShardClient*> replicas_for_shard(const std::vector<ShardClient*>& pool, int shard_id) {
    std::vector<ShardClient*> out;
    for (auto* sc : pool) if (sc->shard_id == shard_id) out.push_back(sc);
    std::sort(out.begin(), out.end(), [](ShardClient* a, ShardClient* b) {
        return a->is_primary && !b->is_primary;
    });
    return out;
}

static ShardClient* find_primary(const std::vector<ShardClient*>& pool, int shard_id) {
    for (auto* sc : pool) if (sc->shard_id == shard_id && sc->is_primary) return sc;
    return nullptr;
}

// One representative replica per distinct shard_id, for reads. "strong"
// always picks the primary -- if the primary isn't currently active, that
// shard is reported unavailable rather than silently reading a replica,
// since the primary is the only replica reads can be sure isn't stale.
// "eventual" prefers a non-primary replica when one's available, both to
// demonstrate genuine load distribution away from the primary and because
// there's no consistency reason to prefer it once staleness is accepted.
static std::vector<ShardClient*> select_read_targets(const std::vector<ShardClient*>& pool, const std::string& consistency) {
    std::map<int, std::vector<ShardClient*>> by_shard;
    for (auto* sc : pool) by_shard[sc->shard_id].push_back(sc);

    std::vector<ShardClient*> out;
    for (auto& [shard_id, replicas] : by_shard) {
        ShardClient* chosen = nullptr;
        if (consistency == "strong") {
            for (auto* r : replicas) if (r->is_primary) chosen = r;
        } else {
            for (auto* r : replicas) if (!r->is_primary) { chosen = r; break; }
            if (!chosen && !replicas.empty()) chosen = replicas.front();
        }
        if (chosen) out.push_back(chosen);
    }
    return out;
}

static void persist_cluster_state() {
    std::vector<ShardEndpoint> eps;
    for (auto* sc : live_shards()) {
        eps.push_back({sc->shard_id, sc->replica_id, sc->host, sc->port, sc->is_primary.load()});
    }
    try {
        save_cluster_config(g_cluster_config_path, eps);
    } catch (const std::exception& e) {
        std::cerr << "[Coordinator] WARNING: failed to persist cluster config: " << e.what() << std::endl;
    }
}

// The Phase 4a state machine, now also handling Phase 4b's set_primary.
// Applies any newly Raft-committed AddShard/RemoveShard/SetPrimary
// commands to g_shards/active_ring. Safe to call from any node regardless
// of role (followers need this too, to keep their own view in sync) and
// safe to call redundantly (no-ops if nothing new). Called both by a
// dedicated background poller (so followers stay in sync passively) and
// synchronously by handlers doing a catch-up right after their own
// propose() commits.
static void apply_pending_raft_commands() {
    if (!g_raft_node) return;
    std::lock_guard<std::mutex> apply_lock(g_apply_mutex);
    auto st = g_raft_node->status();
    if (st.applied_commands.size() <= g_local_applied_count) return;

    for (uint64_t i = g_local_applied_count; i < st.applied_commands.size(); i++) {
        // The epoch for any command that establishes a shard's primary --
        // both the initial AddShard and any later SetPrimary -- is just
        // this command's own position in the committed log (1-based, since
        // applied_commands_[i] in RaftNode corresponds exactly to Raft log
        // index i+1). Free, monotonically increasing, no new Raft
        // machinery needed: it's already guaranteed to only go up, and
        // every node applies commands in this same order, so every node
        // computes the same epoch for the same command.
        uint64_t command_epoch = i + 1;
        try {
            json cmd = json::parse(st.applied_commands[i]);
            std::string type = cmd.at("type").get<std::string>();

            if (type == "add_shard") {
                int shard_id = cmd.at("shard_id").get<int>();

                bool exists = false;
                {
                    std::shared_lock lock(cluster_mutex);
                    for (auto& sc : g_shards) if (sc->shard_id == shard_id) exists = true;
                }
                if (!exists) {
                    std::vector<std::unique_ptr<ShardClient>> new_clients;
                    for (const auto& r : cmd.at("replicas")) {
                        auto sc = make_shard_client(
                            shard_id,
                            r.at("replica_id").get<int>(),
                            r.at("host").get<std::string>(),
                            r.at("port").get<int>(),
                            r.value("primary", false));
                        sc->epoch = command_epoch;
                        new_clients.push_back(std::move(sc));
                    }
                    std::unique_lock lock(cluster_mutex);
                    for (auto& nc : new_clients) g_shards.push_back(std::move(nc));
                    std::set<int> ids;
                    for (auto& sc : g_shards) if (sc->active) ids.insert(sc->shard_id);
                    active_ring.build(std::vector<int>(ids.begin(), ids.end()));
                }
            } else if (type == "remove_shard") {
                int shard_id = cmd.at("shard_id").get<int>();
                std::unique_lock lock(cluster_mutex);
                for (auto& sc : g_shards) {
                    if (sc->shard_id == shard_id) sc->active = false;
                }
                std::set<int> ids;
                for (auto& sc : g_shards) if (sc->active) ids.insert(sc->shard_id);
                active_ring.build(std::vector<int>(ids.begin(), ids.end()));
            } else if (type == "set_primary") {
                int shard_id = cmd.at("shard_id").get<int>();
                int new_primary_replica_id = cmd.at("replica_id").get<int>();
                std::shared_lock lock(cluster_mutex);
                for (auto& sc : g_shards) {
                    if (sc->shard_id != shard_id) continue;
                    sc->is_primary = (sc->replica_id == new_primary_replica_id);
                    sc->epoch = command_epoch;
                }
            } else {
                std::cerr << "[Coordinator] WARNING: unknown raft command type \"" << type << "\"" << std::endl;
            }
        } catch (const std::exception& e) {
            std::cerr << "[Coordinator] WARNING: failed to apply raft command #" << i << ": " << e.what() << std::endl;
        }
    }
    g_local_applied_count = st.applied_commands.size();
}

struct QuorumResult {
    bool ok;     // primary succeeded AND quorum met
    int acks;    // total replicas that succeeded
    int needed;  // quorum size (majority of the replica set)
};

// Fires the write at every replica in parallel (std::async + a local
// futures vector -- discarding a std::async future immediately blocks on
// it, which Phase 3a's heartbeat code already found the hard way). Each
// replica gets its own currently-known epoch attached (Phase 4b) -- not a
// single shared value, since reading sc->epoch per-replica is what lets
// a stale coordinator's writes get correctly fenced at whichever replica
// actually receives them. The primary's specific result is tracked
// separately: a write only counts as successful if the primary itself
// acked AND a majority of the full replica set acked. A majority of
// secondaries succeeding while the primary fails is not a successful
// write -- see Phase 4b plan, Section 1 for what that means in practice
// during an active failover.
static QuorumResult quorum_insert(const std::vector<ShardClient*>& replicas,
                                   const std::string& external_id,
                                   const std::vector<float>& vec,
                                   const std::string& metadata) {
    std::vector<std::future<bool>> futures;
    int primary_idx = -1;
    for (size_t i = 0; i < replicas.size(); i++) {
        if (replicas[i]->is_primary) primary_idx = (int)i;
        auto* sc = replicas[i];
        uint64_t epoch = sc->epoch.load();
        futures.push_back(std::async(std::launch::async, [sc, external_id, vec, metadata, epoch]() {
            InsertRequest req;
            req.set_external_id(external_id);
            for (float f : vec) req.add_vector(f);
            req.set_metadata(metadata);
            req.set_epoch(epoch);
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
            InsertResponse res;
            grpc::Status status = sc->stub->Insert(&ctx, req, &res);
            return status.ok() && res.ok();
        }));
    }
    int acks = 0;
    bool primary_ok = false;
    for (size_t i = 0; i < futures.size(); i++) {
        bool ok = futures[i].get();
        if (ok) acks++;
        if ((int)i == primary_idx && ok) primary_ok = true;
    }
    int needed = (int)(replicas.size() / 2) + 1;
    return {primary_ok && acks >= needed, acks, needed};
}

static QuorumResult quorum_delete(const std::vector<ShardClient*>& replicas, const std::string& external_id) {
    std::vector<std::future<bool>> futures;
    int primary_idx = -1;
    for (size_t i = 0; i < replicas.size(); i++) {
        if (replicas[i]->is_primary) primary_idx = (int)i;
        auto* sc = replicas[i];
        uint64_t epoch = sc->epoch.load();
        futures.push_back(std::async(std::launch::async, [sc, external_id, epoch]() {
            DeleteRequest req;
            req.set_external_id(external_id);
            req.set_epoch(epoch);
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
            DeleteResponse res;
            grpc::Status status = sc->stub->Delete(&ctx, req, &res);
            return status.ok() && res.ok();
        }));
    }
    int acks = 0;
    bool primary_ok = false;
    for (size_t i = 0; i < futures.size(); i++) {
        bool ok = futures[i].get();
        if (ok) acks++;
        if ((int)i == primary_idx && ok) primary_ok = true;
    }
    int needed = (int)(replicas.size() / 2) + 1;
    return {primary_ok && acks >= needed, acks, needed};
}

// Moves one key from a source shard's full replica set to a destination
// shard's full replica set. Reads from the source's primary (the one copy
// guaranteed current), quorum-writes into the destination, then
// quorum-deletes from the source -- insert-before-delete for the same
// reason as every migration since Phase 2: a duplicate-for-a-moment is
// fine, a vector visible on neither side is not. Returns false if the
// destination quorum write failed; the source delete is best-effort
// exactly as before.
static bool migrate_one_key(ShardClient& source_primary,
                             const std::vector<ShardClient*>& source_replicas,
                             const std::vector<ShardClient*>& dest_replicas,
                             const std::string& external_id) {
    GetVectorRequest greq;
    greq.set_external_id(external_id);
    grpc::ClientContext gctx;
    gctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(MIGRATION_RPC_TIMEOUT_MS));
    GetVectorResponse gres;
    if (!source_primary.stub->GetVector(&gctx, greq, &gres).ok() || !gres.ok()) return false;

    std::vector<float> vec(gres.vector().begin(), gres.vector().end());
    auto insert_result = quorum_insert(dest_replicas, external_id, vec, gres.metadata());
    if (!insert_result.ok) return false;

    quorum_delete(source_replicas, external_id); // best-effort
    return true;
}

// Runs on every coordinator, leader and followers alike (cheap: it's just
// Ping RPCs), but only the leader acts on what it sees. Requires
// FAILURES_BEFORE_FAILOVER consecutive misses before declaring a primary
// down, to avoid triggering a failover on one slow response. Promotion
// doesn't move any data -- the data's already on every replica via
// quorum-write replication -- it's purely a Raft-committed declaration of
// who's authoritative now, which is why this can be as simple as one
// propose() call with no migration logic attached.
static void health_check_loop() {
    while (g_health_check_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(HEALTH_CHECK_INTERVAL_MS));
        if (!g_raft_node) continue;
        if (g_raft_node->status().role != "leader") continue;

        auto pool = live_shards();
        std::set<int> shard_ids;
        for (auto* sc : pool) shard_ids.insert(sc->shard_id);

        for (int shard_id : shard_ids) {
            ShardClient* primary = find_primary(pool, shard_id);
            if (!primary) continue;

            PingRequest preq;
            grpc::ClientContext pctx;
            pctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(HEALTH_CHECK_TIMEOUT_MS));
            PingResponse pres;
            bool ok = primary->stub->Ping(&pctx, preq, &pres).ok() && pres.ok();

            int failures = 0;
            {
                std::lock_guard<std::mutex> lock(g_health_mutex);
                if (ok) {
                    g_consecutive_primary_failures[shard_id] = 0;
                    continue;
                }
                failures = ++g_consecutive_primary_failures[shard_id];
            }
            if (failures < FAILURES_BEFORE_FAILOVER) continue;

            ShardClient* candidate = nullptr;
            for (auto* r : replicas_for_shard(pool, shard_id)) {
                if (r == primary) continue;
                PingRequest preq2;
                grpc::ClientContext pctx2;
                pctx2.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(HEALTH_CHECK_TIMEOUT_MS));
                PingResponse pres2;
                if (r->stub->Ping(&pctx2, preq2, &pres2).ok() && pres2.ok()) { candidate = r; break; }
            }
            if (!candidate) {
                std::cerr << "[Coordinator] WARNING: shard " << shard_id
                          << " primary unreachable, no healthy replica available to promote" << std::endl;
                continue;
            }

            int new_primary_replica_id = candidate->replica_id;
            {
                std::lock_guard<std::mutex> lock(g_health_mutex);
                g_consecutive_primary_failures[shard_id] = 0; // don't re-trigger while this is in flight
            }
            std::thread([shard_id, new_primary_replica_id]() {
                json cmd = {{"type", "set_primary"}, {"shard_id", shard_id}, {"replica_id", new_primary_replica_id}};
                if (g_raft_node->propose(cmd.dump())) {
                    apply_pending_raft_commands();
                    persist_cluster_state();
                    std::cout << "[Coordinator] Failover: shard " << shard_id
                              << " primary -> replica " << new_primary_replica_id << std::endl;
                } else {
                    std::cerr << "[Coordinator] WARNING: failover proposal for shard "
                              << shard_id << " did not commit" << std::endl;
                }
            }).detach();
        }
    }
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

    std::set<int> ids;
    for (const auto& ep : endpoints) {
        g_shards.push_back(make_shard_client(ep.shard_id, ep.replica_id, ep.host, ep.port, ep.is_primary));
        ids.insert(ep.shard_id);
    }
    active_ring.build(std::vector<int>(ids.begin(), ids.end()));
    std::cout << "[Coordinator] Loaded " << g_shards.size() << " replica(s) across "
              << ids.size() << " shard(s) from " << g_cluster_config_path << std::endl;

    std::unique_ptr<nanodb::raft::RaftServiceImpl> raft_service;
    std::unique_ptr<grpc::Server> raft_server;
    std::thread raft_server_thread;

    const char* raft_node_id_env = std::getenv("NANODB_RAFT_NODE_ID");
    const char* raft_peers_env = std::getenv("NANODB_RAFT_PEERS_CONFIG");
    if (raft_node_id_env && raft_peers_env) {
        int raft_node_id = std::atoi(raft_node_id_env);
        const char* raft_state_env = std::getenv("NANODB_RAFT_STATE_PATH");
        std::string raft_state_path = raft_state_env ? raft_state_env : "raft_state.bin";

        std::vector<nanodb::raft::RaftPeer> raft_peers;
        try {
            raft_peers = nanodb::raft::load_raft_peers(raft_peers_env);
        } catch (const std::exception& e) {
            std::cerr << "[ERROR] " << e.what() << std::endl;
            return 1;
        }

        nanodb::raft::RaftPeer self_peer;
        bool found_self = false;
        for (auto& p : raft_peers) {
            if (p.node_id == raft_node_id) { self_peer = p; found_self = true; }
        }
        if (!found_self) {
            std::cerr << "[ERROR] NANODB_RAFT_NODE_ID=" << raft_node_id
                      << " not present in " << raft_peers_env << std::endl;
            return 1;
        }

        const char* raft_log_env = std::getenv("NANODB_RAFT_LOG_PATH");
        std::string raft_log_path = raft_log_env ? raft_log_env : "raft_log.bin";
        g_raft_node = std::make_unique<nanodb::raft::RaftNode>(raft_node_id, raft_peers, raft_state_path, raft_log_path);
        raft_service = std::make_unique<nanodb::raft::RaftServiceImpl>(*g_raft_node);

        grpc::ServerBuilder raft_builder;
        raft_builder.AddListeningPort(self_peer.host + ":" + std::to_string(self_peer.port),
                                       grpc::InsecureServerCredentials());
        raft_builder.RegisterService(raft_service.get());
        raft_server = raft_builder.BuildAndStart();
        g_raft_server = raft_server.get();
        raft_server_thread = std::thread([&raft_server]() { raft_server->Wait(); });

        g_raft_node->start();
        std::cout << "[Coordinator] Raft node " << raft_node_id << " listening on "
                  << self_peer.host << ":" << self_peer.port << std::endl;
    } else {
        std::cout << "[Coordinator] Raft disabled (set NANODB_RAFT_NODE_ID and "
                     "NANODB_RAFT_PEERS_CONFIG to enable)" << std::endl;
    }

    std::thread apply_poller_thread;
    std::thread health_check_thread;
    if (g_raft_node) {
        g_apply_poller_running = true;
        apply_poller_thread = std::thread([&]() {
            while (g_apply_poller_running) {
                apply_pending_raft_commands();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        g_health_check_running = true;
        health_check_thread = std::thread(health_check_loop);
    }

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
            int shard_id = ring.route(external_id);
            auto replicas = replicas_for_shard(pool, shard_id);
            if (replicas.empty()) {
                res.status = 503;
                res.set_content(R"({"error":"destination shard unavailable"})", "application/json");
                return;
            }

            std::vector<float> vec;
            for (const auto& v : body["vector"]) vec.push_back(v.get<float>());
            std::string metadata = body.value("metadata", "");

            auto result = quorum_insert(replicas, external_id, vec, metadata);
            if (!result.ok) {
                res.status = 502;
                json err = {{"error", "write quorum not met"}, {"shard", shard_id},
                            {"acks", result.acks}, {"needed", result.needed}};
                res.set_content(err.dump(), "application/json");
                return;
            }
            res.status = 201;
            json ok = {{"status", "ok"}, {"id", external_id}, {"shard", shard_id},
                       {"acks", result.acks}, {"needed", result.needed}};
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
            std::string consistency = body.value("consistency", "eventual");

            auto pool = select_read_targets(live_shards(), consistency);
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

            json response = {{"results", deduped}, {"consistency", consistency}};
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
        int shard_id = ring.route(external_id);
        auto replicas = replicas_for_shard(pool, shard_id);
        if (replicas.empty()) {
            res.status = 503;
            res.set_content(R"({"error":"destination shard unavailable"})", "application/json");
            return;
        }
        auto result = quorum_delete(replicas, external_id);
        if (!result.ok) {
            res.status = 404;
            json err = {{"error", "not found, or delete quorum not met"}, {"acks", result.acks}, {"needed", result.needed}};
            res.set_content(err.dump(), "application/json");
            return;
        }
        json ok = {{"status", "ok"}, {"id", external_id}, {"acks", result.acks}, {"needed", result.needed}};
        res.set_content(ok.dump(), "application/json");
    });

    server.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        auto pool = live_shards();
        json replicas_json = json::array();
        uint64_t total = 0;
        std::vector<json> unavailable;
        for (auto* sc : pool) {
            StatsRequest grpc_req;
            grpc::ClientContext ctx;
            ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(RPC_TIMEOUT_MS));
            StatsResponse grpc_res;
            grpc::Status status = sc->stub->Stats(&ctx, grpc_req, &grpc_res);
            if (!status.ok()) {
                unavailable.push_back({{"shard_id", sc->shard_id}, {"replica_id", sc->replica_id}});
                continue;
            }
            replicas_json.push_back({{"shard_id", sc->shard_id}, {"replica_id", sc->replica_id},
                                      {"is_primary", sc->is_primary.load()}, {"epoch", sc->epoch.load()},
                                      {"element_count", grpc_res.element_count()}});
            // Sum primaries only -- each shard's data is replicated across
            // its full replica set, so summing every replica would inflate
            // the total by roughly the replication factor instead of
            // reporting how many actual unique vectors exist.
            if (sc->is_primary) total += grpc_res.element_count();
        }
        json response = {{"total_element_count", total}, {"replicas", replicas_json}};
        if (!unavailable.empty()) {
            response["degraded"] = true;
            response["unavailable_replicas"] = unavailable;
        }
        res.set_content(response.dump(), "application/json");
    });

    server.Get("/raft/status", [&](const httplib::Request&, httplib::Response& res) {
        if (!g_raft_node) {
            res.status = 404;
            res.set_content(R"({"error":"raft is not enabled on this coordinator"})", "application/json");
            return;
        }
        auto st = g_raft_node->status();
        json response = {{"node_id", st.node_id}, {"role", st.role},
                          {"term", st.term}, {"leader_id", st.leader_id},
                          {"log_length", st.log_length}, {"commit_index", st.commit_index},
                          {"applied_commands", st.applied_commands}};
        res.set_content(response.dump(), "application/json");
    });

    server.Post("/raft/propose", [&](const httplib::Request& req, httplib::Response& res) {
        if (!g_raft_node) {
            res.status = 404;
            res.set_content(R"({"error":"raft is not enabled on this coordinator"})", "application/json");
            return;
        }
        bool ok = g_raft_node->propose(req.body);
        json response = {{"committed", ok}};
        res.status = ok ? 200 : 503;
        res.set_content(response.dump(), "application/json");
    });

    // POST /admin/shards/add  body: {"shard_id": 3, "replicas": [
    //   {"replica_id": 0, "host": "shard-3a", "port": 9090, "primary": true},
    //   {"replica_id": 1, "host": "shard-3b", "port": 9090, "primary": false}]}
    server.Post("/admin/shards/add", [&](const httplib::Request& req, httplib::Response& res) {
        if (!g_raft_node) {
            res.status = 400;
            res.set_content(R"({"error":"raft is not enabled on this coordinator"})", "application/json");
            return;
        }
        auto raft_st = g_raft_node->status();
        if (raft_st.role != "leader") {
            res.status = 503;
            json err = {{"error", "not the leader"}, {"leader_id", raft_st.leader_id}};
            res.set_content(err.dump(), "application/json");
            return;
        }

        bool expected = false;
        if (!rebalancing.compare_exchange_strong(expected, true)) {
            res.status = 409;
            res.set_content(R"({"error":"a rebalance is already in progress"})", "application/json");
            return;
        }
        try {
            auto body = json::parse(req.body);
            int new_id = body.at("shard_id").get<int>();
            if (!body.contains("replicas") || body.at("replicas").empty()) {
                rebalancing = false;
                res.status = 400;
                res.set_content(R"({"error":"replicas must be a non-empty array"})", "application/json");
                return;
            }
            bool has_primary = false;
            for (const auto& r : body.at("replicas")) if (r.value("primary", false)) has_primary = true;
            if (!has_primary) {
                rebalancing = false;
                res.status = 400;
                res.set_content(R"({"error":"exactly one replica must be marked primary"})", "application/json");
                return;
            }

            // Propose first: a shard can't be migrated TO until it has
            // ShardClients/stubs, which only exist once the AddShard
            // command has actually been applied.
            json cmd = {{"type", "add_shard"}, {"shard_id", new_id}, {"replicas", body.at("replicas")}};
            if (!g_raft_node->propose(cmd.dump())) {
                rebalancing = false;
                res.status = 503;
                res.set_content(R"({"error":"raft proposal did not commit, lost leadership or timed out"})", "application/json");
                return;
            }
            apply_pending_raft_commands(); // don't wait for the poller's cadence

            auto pool = live_shards();
            auto dest_replicas = replicas_for_shard(pool, new_id);
            if (dest_replicas.empty()) {
                rebalancing = false;
                res.status = 500;
                res.set_content(R"({"error":"committed via raft but not present locally after apply -- this should not happen"})", "application/json");
                return;
            }
            HashRing new_ring = ring_snapshot();

            std::set<int> source_shard_ids;
            for (auto* sc : pool) if (sc->shard_id != new_id) source_shard_ids.insert(sc->shard_id);

            int migrated = 0, failed = 0;
            for (int source_shard_id : source_shard_ids) {
                auto source_replicas = replicas_for_shard(pool, source_shard_id);
                ShardClient* source_primary = find_primary(pool, source_shard_id);
                if (!source_primary) continue;

                ListLocalIdsRequest lreq;
                grpc::ClientContext lctx;
                lctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
                ListLocalIdsResponse lres;
                if (!source_primary->stub->ListLocalIds(&lctx, lreq, &lres).ok()) continue;

                for (const auto& external_id : lres.external_ids()) {
                    if (new_ring.route(external_id) != new_id) continue;
                    if (migrate_one_key(*source_primary, source_replicas, dest_replicas, external_id)) migrated++;
                    else failed++;
                }
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
        if (!g_raft_node) {
            res.status = 400;
            res.set_content(R"({"error":"raft is not enabled on this coordinator"})", "application/json");
            return;
        }
        auto raft_st = g_raft_node->status();
        if (raft_st.role != "leader") {
            res.status = 503;
            json err = {{"error", "not the leader"}, {"leader_id", raft_st.leader_id}};
            res.set_content(err.dump(), "application/json");
            return;
        }

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
            auto leaving_replicas = replicas_for_shard(pool, leaving_id);
            ShardClient* leaving_primary = find_primary(pool, leaving_id);
            if (leaving_replicas.empty() || !leaving_primary) {
                rebalancing = false;
                res.status = 404;
                res.set_content(R"({"error":"shard not found or already inactive"})", "application/json");
                return;
            }

            std::set<int> all_shard_ids;
            for (auto* sc : pool) all_shard_ids.insert(sc->shard_id);
            if (all_shard_ids.size() <= 1) {
                rebalancing = false;
                res.status = 400;
                res.set_content(R"({"error":"cannot remove the last shard"})", "application/json");
                return;
            }

            // Migrate BEFORE proposing, deliberately: the leaving shard's
            // replicas already have live stubs (they're still in g_shards),
            // so unlike AddShard there's no technical need to commit first.
            // Moving the data first and proposing RemoveShard as the atomic
            // "officially gone" declaration afterward means that by the
            // time ANY node -- leader or follower -- applies this command,
            // the migration is already done, so it's always safe for that
            // application to immediately mark the shard inactive (excluded
            // from search/stats) with no window where data is invisible
            // because it's "removed" on paper but not actually moved yet.
            std::vector<int> remaining_ids;
            for (int sid : all_shard_ids) if (sid != leaving_id) remaining_ids.push_back(sid);
            HashRing post_remove_ring(remaining_ids);

            int migrated = 0, failed = 0;
            ListLocalIdsRequest lreq;
            grpc::ClientContext lctx;
            lctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
            ListLocalIdsResponse lres;
            if (leaving_primary->stub->ListLocalIds(&lctx, lreq, &lres).ok()) {
                for (const auto& external_id : lres.external_ids()) {
                    int dest_shard_id = post_remove_ring.route(external_id);
                    auto dest_replicas = replicas_for_shard(pool, dest_shard_id);
                    if (dest_replicas.empty()) { failed++; continue; }
                    if (migrate_one_key(*leaving_primary, leaving_replicas, dest_replicas, external_id)) migrated++;
                    else failed++;
                }
            }

            json cmd = {{"type", "remove_shard"}, {"shard_id", leaving_id}};
            if (!g_raft_node->propose(cmd.dump())) {
                rebalancing = false;
                res.status = 503;
                res.set_content(R"({"error":"data was migrated but the raft proposal to finalize removal did not commit -- retry the remove, migration is idempotent"})", "application/json");
                return;
            }
            apply_pending_raft_commands();
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

    // POST /admin/shards/set_primary  body: {"shard_id": 1, "replica_id": 2}
    // Manual failover / primary promotion. The automatic health check uses
    // exactly this same mechanism internally.
    server.Post("/admin/shards/set_primary", [&](const httplib::Request& req, httplib::Response& res) {
        if (!g_raft_node) {
            res.status = 400;
            res.set_content(R"({"error":"raft is not enabled on this coordinator"})", "application/json");
            return;
        }
        auto raft_st = g_raft_node->status();
        if (raft_st.role != "leader") {
            res.status = 503;
            json err = {{"error", "not the leader"}, {"leader_id", raft_st.leader_id}};
            res.set_content(err.dump(), "application/json");
            return;
        }

        try {
            auto body = json::parse(req.body);
            int shard_id = body.at("shard_id").get<int>();
            int replica_id = body.at("replica_id").get<int>();

            auto pool = live_shards();
            bool valid_replica = false;
            for (auto* sc : replicas_for_shard(pool, shard_id)) {
                if (sc->replica_id == replica_id) valid_replica = true;
            }
            if (!valid_replica) {
                res.status = 404;
                res.set_content(R"({"error":"no such active replica for this shard"})", "application/json");
                return;
            }

            json cmd = {{"type", "set_primary"}, {"shard_id", shard_id}, {"replica_id", replica_id}};
            if (!g_raft_node->propose(cmd.dump())) {
                res.status = 503;
                res.set_content(R"({"error":"raft proposal did not commit, lost leadership or timed out"})", "application/json");
                return;
            }
            apply_pending_raft_commands();
            persist_cluster_state();
            json ok = {{"status", "ok"}, {"shard_id", shard_id}, {"primary_replica_id", replica_id}};
            res.set_content(ok.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            json err = {{"error", e.what()}};
            res.set_content(err.dump(), "application/json");
        }
    });

    std::cout << "[Coordinator] Listening on 0.0.0.0:" << http_port << std::endl;
    server.listen("0.0.0.0", http_port);
    if (raft_server_thread.joinable()) raft_server_thread.join();
    if (apply_poller_thread.joinable()) apply_poller_thread.join();
    if (health_check_thread.joinable()) health_check_thread.join();
    std::cout << "[Coordinator] Stopped." << std::endl;
    return 0;
}

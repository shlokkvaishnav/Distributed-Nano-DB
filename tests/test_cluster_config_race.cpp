// tests/test_cluster_config_race.cpp
//
// Permanent regression guard for the Phase 5 cluster_config.hpp fix.
//
// Multiple coordinators share one NANODB_CLUSTER_CONFIG path and each
// calls save_cluster_config() after every rebalance/failover. The original
// implementation wrote directly to the path with a plain std::ofstream,
// which truncates the file the instant it's opened -- a concurrent
// load_cluster_config() could observe a truncated/invalid file mid-write
// and throw, which is fatal at coordinator startup.
//
// This test hammers one shared config file with 4 writer threads and 4
// reader threads with no in-process synchronization between them (mirroring
// multiple independent coordinator processes sharing only the filesystem),
// and fails (non-zero exit, ctest reports FAILED) if a single read fails.
// Pre-fix, this fails essentially immediately and overwhelmingly (~96% of
// reads failed in the isolated proof run that motivated this fix). Kept
// short (1s) to stay fast in routine ctest runs while still issuing tens
// of thousands of reads -- plenty to catch a regression decisively.

#include "../cluster/cluster_config.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <thread>
#include <vector>

using namespace nanodb::cluster;

namespace {

constexpr int kDurationSeconds = 1;
constexpr int kWriterThreads = 4;
constexpr int kReaderThreads = 4;
constexpr const char* kConfigPath = "test_cluster_config_race.json";

std::atomic<bool> g_stop{false};
std::atomic<long> g_total_reads{0};
std::atomic<long> g_read_failures{0};
std::atomic<long> g_total_writes{0};

std::vector<ShardEndpoint> make_config(int variant) {
    std::vector<ShardEndpoint> shards;
    for (int s = 0; s < 2; ++s) {
        for (int r = 0; r < 3; ++r) {
            ShardEndpoint ep;
            ep.shard_id = s;
            ep.replica_id = r;
            ep.host = "127.0.0.1";
            ep.port = 9000 + s * 10 + r + (variant % 5);
            ep.is_primary = (r == 0);
            shards.push_back(ep);
        }
    }
    return shards;
}

void writer_loop() {
    int variant = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        save_cluster_config(kConfigPath, make_config(variant++));
        g_total_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

void reader_loop() {
    while (!g_stop.load(std::memory_order_relaxed)) {
        g_total_reads.fetch_add(1, std::memory_order_relaxed);
        try {
            auto eps = load_cluster_config(kConfigPath);
            if (eps.empty()) g_read_failures.fetch_add(1, std::memory_order_relaxed);
        } catch (const std::exception&) {
            g_read_failures.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace

int main() {
    save_cluster_config(kConfigPath, make_config(0));

    std::vector<std::thread> threads;
    for (int i = 0; i < kWriterThreads; ++i) threads.emplace_back(writer_loop);
    for (int i = 0; i < kReaderThreads; ++i) threads.emplace_back(reader_loop);

    std::this_thread::sleep_for(std::chrono::seconds(kDurationSeconds));
    g_stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    std::remove(kConfigPath);

    long reads = g_total_reads.load();
    long fails = g_read_failures.load();
    long writes = g_total_writes.load();

    std::cout << "[ClusterConfigRace] writes=" << writes << " reads=" << reads
              << " read_failures=" << fails << std::endl;

    if (fails > 0) {
        std::cerr << "[ClusterConfigRace] FAILED: " << fails << "/" << reads
                  << " concurrent reads failed -- save_cluster_config is not "
                     "atomic with respect to concurrent load_cluster_config "
                     "calls. See cluster/cluster_config.hpp." << std::endl;
        return 1;
    }
    return 0;
}

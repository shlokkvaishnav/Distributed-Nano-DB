#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <mutex>
#include <utility>
#include <cstdint>

namespace nanodb {
namespace cluster {

// Maps a client-supplied external_id to a per-shard dense local_id, so the
// HNSW engine (which uses the id as a direct file offset, see Section 2 of
// the plan) never sees a sparse global ID. On-disk layout mirrors
// MetadataHandler: an append-only file of [uint32_t length][bytes] records,
// one per local_id, in order. Startup replays the file to rebuild both
// directions of the map.
class IdMapStore {
public:
    IdMapStore() = default;

    void open_file(const std::string& filepath) {
        filepath_ = filepath;
        file_stream_.open(filepath, std::ios::in | std::ios::out | std::ios::binary);
        if (!file_stream_.is_open()) {
            std::ofstream create(filepath);
            create.close();
            file_stream_.open(filepath, std::ios::in | std::ios::out | std::ios::binary);
        }
        rebuild_index();
    }

    void close_file() {
        if (file_stream_.is_open()) file_stream_.close();
    }

    // Returns {local_id, was_newly_assigned}.
    std::pair<uint32_t, bool> assign(const std::string& external_id) {
        std::lock_guard<std::mutex> lock(lock_);
        auto it = forward_.find(external_id);
        if (it != forward_.end()) return {it->second, false};
        uint32_t local_id = static_cast<uint32_t>(reverse_.size());
        persist(external_id);
        forward_[external_id] = local_id;
        reverse_.push_back(external_id);
        return {local_id, true};
    }

    bool lookup(const std::string& external_id, uint32_t& out_local_id) {
        std::lock_guard<std::mutex> lock(lock_);
        auto it = forward_.find(external_id);
        if (it == forward_.end()) return false;
        out_local_id = it->second;
        return true;
    }

    std::string reverse_lookup(uint32_t local_id) {
        std::lock_guard<std::mutex> lock(lock_);
        if (local_id >= reverse_.size()) return "";
        return reverse_[local_id];
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(lock_);
        return reverse_.size();
    }

    // Returns a copy of every external_id currently mapped on this shard, in
    // local_id order. Used by the Phase 2 rebalancer to discover what a
    // shard owns when deciding what needs to migrate.
    std::vector<std::string> list_all_external_ids() {
        std::lock_guard<std::mutex> lock(lock_);
        return reverse_;
    }

private:
    std::string filepath_;
    std::fstream file_stream_;
    std::unordered_map<std::string, uint32_t> forward_;
    std::vector<std::string> reverse_;
    std::mutex lock_;

    void persist(const std::string& external_id) {
        file_stream_.clear();
        file_stream_.seekp(0, std::ios::end);
        uint32_t len = static_cast<uint32_t>(external_id.size());
        file_stream_.write(reinterpret_cast<char*>(&len), sizeof(uint32_t));
        file_stream_.write(external_id.data(), len);
        file_stream_.flush();
    }

    void rebuild_index() {
        file_stream_.clear();
        file_stream_.seekg(0, std::ios::beg);
        if (file_stream_.peek() == EOF) return;
        while (file_stream_.peek() != EOF) {
            uint32_t len;
            if (!file_stream_.read(reinterpret_cast<char*>(&len), sizeof(uint32_t))) break;
            std::string external_id(len, '\0');
            if (!file_stream_.read(&external_id[0], len)) break;
            uint32_t local_id = static_cast<uint32_t>(reverse_.size());
            forward_[external_id] = local_id;
            reverse_.push_back(external_id);
        }
        file_stream_.clear();
    }
};

} // namespace cluster
} // namespace nanodb

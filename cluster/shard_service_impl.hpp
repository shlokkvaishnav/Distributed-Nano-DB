#pragma once
#include "nanodb_cluster.grpc.pb.h"
#include "../include/index/hnsw.hpp"
#include "id_map_store.hpp"
#include <grpcpp/grpcpp.h>

namespace nanodb {
namespace cluster {

class ShardServiceImpl final : public ShardService::Service {
public:
    ShardServiceImpl(HNSW& index, IdMapStore& id_map, const std::string& shard_id)
        : index_(index), id_map_(id_map), shard_id_(shard_id) {}

    grpc::Status Insert(grpc::ServerContext*, const InsertRequest* request,
                         InsertResponse* response) override {
        if (request->vector_size() != (int)config::VECTOR_DIM) {
            response->set_ok(false);
            response->set_error("vector dimension mismatch, expected " +
                                 std::to_string(config::VECTOR_DIM));
            return grpc::Status::OK;
        }
        std::vector<float> vec(request->vector().begin(), request->vector().end());
        auto [local_id, is_new] = id_map_.assign(request->external_id());
        (void)is_new;
        index_.insert(vec, local_id, request->metadata());
        response->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status Search(grpc::ServerContext*, const SearchRequest* request,
                         SearchResponse* response) override {
        if (request->vector_size() != (int)config::VECTOR_DIM) {
            response->set_ok(false);
            response->set_error("vector dimension mismatch, expected " +
                                 std::to_string(config::VECTOR_DIM));
            return grpc::Status::OK;
        }
        std::vector<float> vec(request->vector().begin(), request->vector().end());
        auto results = index_.search(vec, request->k());
        for (const auto& r : results) {
            auto* out = response->add_results();
            out->set_external_id(id_map_.reverse_lookup(r.id));
            out->set_distance(r.distance);
            out->set_metadata(r.metadata);
        }
        response->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status Delete(grpc::ServerContext*, const DeleteRequest* request,
                         DeleteResponse* response) override {
        uint32_t local_id;
        if (!id_map_.lookup(request->external_id(), local_id)) {
            response->set_ok(false);
            response->set_error("external_id not found on this shard");
            return grpc::Status::OK;
        }
        index_.delete_vector(local_id);
        response->set_ok(true);
        return grpc::Status::OK;
    }

    grpc::Status Stats(grpc::ServerContext*, const StatsRequest*,
                        StatsResponse* response) override {
        response->set_element_count(index_.size());
        response->set_vector_dim((uint32_t)config::VECTOR_DIM);
        response->set_shard_id(shard_id_);
        return grpc::Status::OK;
    }

    grpc::Status Ping(grpc::ServerContext*, const PingRequest*,
                       PingResponse* response) override {
        response->set_ok(true);
        response->set_shard_id(shard_id_);
        return grpc::Status::OK;
    }

private:
    HNSW& index_;
    IdMapStore& id_map_;
    std::string shard_id_;
};

} // namespace cluster
} // namespace nanodb

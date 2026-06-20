#include <iostream>
#include <csignal>
#include <cstdlib>
#include <grpcpp/grpcpp.h>

#include "../include/config/constants.hpp"
#include "../include/storage/memory_map.hpp"
#include "../include/index/hnsw.hpp"
#include "id_map_store.hpp"
#include "shard_service_impl.hpp"

using namespace nanodb;

static std::unique_ptr<grpc::Server> g_server;

void signal_handler(int) {
    if (g_server) g_server->Shutdown();
}

int main() {
    const char* shard_id_env = std::getenv("NANODB_SHARD_ID");
    std::string shard_id = shard_id_env ? shard_id_env : "0";

    const char* port_env = std::getenv("NANODB_GRPC_PORT");
    int port = port_env ? std::atoi(port_env) : 9090;

    const char* data_dir_env = std::getenv("NANODB_DATA_DIR");
    std::string data_dir = data_dir_env ? data_dir_env : "data";

    std::string db_path = data_dir + "/index.ndb";
    std::string meta_path = data_dir + "/metadata.bin";
    std::string id_map_path = data_dir + "/id_map.bin";

    MMapHandler storage;
    try {
        storage.open_file(db_path, 100 * 1024 * 1024);
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to open storage: " << e.what() << std::endl;
        return 1;
    }

    HNSW index(storage, meta_path);

    cluster::IdMapStore id_map;
    id_map.open_file(id_map_path);

    cluster::ShardServiceImpl service(index, id_map, shard_id);

    std::string server_address = "0.0.0.0:" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    g_server = builder.BuildAndStart();

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "[ShardNode " << shard_id << "] Listening on " << server_address << std::endl;
    std::cout << "[ShardNode " << shard_id << "] " << id_map.size()
              << " id mappings, " << index.size() << " live vectors loaded" << std::endl;

    g_server->Wait();

    storage.close_file();
    id_map.close_file();
    std::cout << "[ShardNode " << shard_id << "] Stopped." << std::endl;
    return 0;
}

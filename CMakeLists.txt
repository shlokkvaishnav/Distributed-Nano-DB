cmake_minimum_required(VERSION 3.10)
project(NanoDB VERSION 1.0.0 LANGUAGES CXX)

# Build options
option(NANODB_BUILD_SERVER "Build the REST API server" ON)
option(NANODB_BUILD_PYTHON "Build the Python bindings" ON)

# Settings
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Optimization Flags (AVX2)
if(MSVC)
    add_compile_options(/O2 /arch:AVX2 /openmp)
else()
    add_compile_options(-O3 -march=native -mavx2 -mfma -pthread)
endif()

find_package(OpenMP REQUIRED)

# Create the Core Library
add_library(nano_core OBJECT
    src/index/distance.cpp
    src/storage/memory_map.cpp
)
target_include_directories(nano_core PUBLIC include)

# Build the C++ Executable (CLI demo)
add_executable(nano_db src/main.cpp)
target_link_libraries(nano_db PRIVATE nano_core OpenMP::OpenMP_CXX)

# Build the Python Module
if(NANODB_BUILD_PYTHON)
    add_subdirectory(extern/pybind11)
    pybind11_add_module(nanodb_py src/python_bindings.cpp)
    target_link_libraries(nanodb_py PRIVATE nano_core OpenMP::OpenMP_CXX)
    set_target_properties(nanodb_py PROPERTIES OUTPUT_NAME "nanodb")
endif()

# Build the REST API Server
if(NANODB_BUILD_SERVER)
    add_executable(nano_server src/server.cpp)
    target_include_directories(nano_server PRIVATE
        ${CMAKE_SOURCE_DIR}/extern/httplib
        ${CMAKE_SOURCE_DIR}/include/third_party
    )
    target_link_libraries(nano_server PRIVATE nano_core OpenMP::OpenMP_CXX)
    if(WIN32)
        target_link_libraries(nano_server PRIVATE ws2_32)
    endif()
endif()

# ---- Distributed Cluster (gRPC) ----
option(NANODB_BUILD_CLUSTER "Build the distributed cluster (coordinator + shard node)" ON)

if(NANODB_BUILD_CLUSTER)
    find_package(Protobuf REQUIRED)
    find_package(gRPC CONFIG REQUIRED)

    set(NANODB_PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/nanodb_cluster.proto)
    set(NANODB_PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${NANODB_PROTO_GEN_DIR})

    get_target_property(GRPC_CPP_PLUGIN_LOCATION gRPC::grpc_cpp_plugin LOCATION)

    add_custom_command(
        OUTPUT
            "${NANODB_PROTO_GEN_DIR}/nanodb_cluster.pb.cc"
            "${NANODB_PROTO_GEN_DIR}/nanodb_cluster.pb.h"
            "${NANODB_PROTO_GEN_DIR}/nanodb_cluster.grpc.pb.cc"
            "${NANODB_PROTO_GEN_DIR}/nanodb_cluster.grpc.pb.h"
        COMMAND protobuf::protoc
        ARGS --grpc_out "${NANODB_PROTO_GEN_DIR}"
             --cpp_out "${NANODB_PROTO_GEN_DIR}"
             -I "${CMAKE_SOURCE_DIR}/proto"
             --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN_LOCATION}"
             "${NANODB_PROTO_FILE}"
        DEPENDS "${NANODB_PROTO_FILE}"
    )

    add_library(nanodb_proto OBJECT
        ${NANODB_PROTO_GEN_DIR}/nanodb_cluster.pb.cc
        ${NANODB_PROTO_GEN_DIR}/nanodb_cluster.grpc.pb.cc
    )
    target_include_directories(nanodb_proto PUBLIC ${NANODB_PROTO_GEN_DIR})
    target_link_libraries(nanodb_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

    add_executable(nano_shard_node cluster/shard_node_main.cpp)
    target_include_directories(nano_shard_node PRIVATE
        ${CMAKE_SOURCE_DIR}/extern/httplib
        ${CMAKE_SOURCE_DIR}/include/third_party
    )
    target_link_libraries(nano_shard_node PRIVATE nano_core nanodb_proto OpenMP::OpenMP_CXX)

    add_executable(nano_coordinator cluster/coordinator_main.cpp)
    target_include_directories(nano_coordinator PRIVATE
        ${CMAKE_SOURCE_DIR}/extern/httplib
        ${CMAKE_SOURCE_DIR}/include/third_party
    )
    target_link_libraries(nano_coordinator PRIVATE nano_core nanodb_proto OpenMP::OpenMP_CXX)
endif()

# ---- Benchmarks ----
add_executable(benchmark_throughput benchmarks/benchmark_throughput.cpp)
target_link_libraries(benchmark_throughput PRIVATE nano_core OpenMP::OpenMP_CXX)

add_executable(benchmark_recall benchmarks/benchmark_recall.cpp)
target_link_libraries(benchmark_recall PRIVATE nano_core OpenMP::OpenMP_CXX)

# ---- Tests ----
enable_testing()
add_subdirectory(tests)
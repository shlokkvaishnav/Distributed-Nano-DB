# tests/CMakeLists.txt

# Test: Distance Functions
add_executable(test_distance test_distance.cpp)
target_link_libraries(test_distance PRIVATE nano_core)
add_test(NAME DistanceFunctions COMMAND test_distance)

# Test: HNSW Insert / Search / Delete
add_executable(test_hnsw test_hnsw.cpp)
target_link_libraries(test_hnsw PRIVATE nano_core OpenMP::OpenMP_CXX)
add_test(NAME HNSWGraph COMMAND test_hnsw)

# Test: Persistence (save/reload round-trip)
add_executable(test_persistence test_persistence.cpp)
target_link_libraries(test_persistence PRIVATE nano_core OpenMP::OpenMP_CXX)
add_test(NAME Persistence COMMAND test_persistence)

# Test: Routing (fnv1a_64 distribution)
add_executable(test_routing test_routing.cpp)
add_test(NAME Routing COMMAND test_routing)

# Test: IdMapStore (assign, lookup, persistence)
add_executable(test_id_map_store test_id_map_store.cpp)
add_test(NAME IdMapStore COMMAND test_id_map_store)

# Test: HashRing (Phase 2 consistent hashing)
add_executable(test_hash_ring test_hash_ring.cpp)
add_test(NAME HashRing COMMAND test_hash_ring)

# Test: Raft commit-index safety (Figure 8 protection from the paper)
if(NANODB_BUILD_CLUSTER)
    add_executable(test_raft_commit_safety test_raft_commit_safety.cpp)
    target_link_libraries(test_raft_commit_safety PRIVATE nanodb_raft_proto)
    add_test(NAME RaftCommitSafety COMMAND test_raft_commit_safety)
endif()

# Test: cluster_config.hpp atomic-write race (Phase 5) -- permanent
# regression guard for the non-atomic save_cluster_config() bug found via
# the chaos harness; see cluster/cluster_config.hpp.
if(NANODB_BUILD_CLUSTER)
    add_executable(test_cluster_config_race test_cluster_config_race.cpp)
    target_include_directories(test_cluster_config_race PRIVATE
        ${CMAKE_SOURCE_DIR}/include/third_party)
    add_test(NAME ClusterConfigRace COMMAND test_cluster_config_race)
endif()
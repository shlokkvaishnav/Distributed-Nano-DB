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
cmake_minimum_required(VERSION 3.10)
project(NanoDB VERSION 1.0.0 LANGUAGES CXX)

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
    src/core/distance.cpp
    src/storage/mmap_handler.cpp
)
target_include_directories(nano_core PUBLIC include)

# Build the C++ Executable (CLI demo)
add_executable(nano_db src/main.cpp)
target_link_libraries(nano_db PRIVATE nano_core OpenMP::OpenMP_CXX)

# Build the Python Module
add_subdirectory(extern/pybind11)
pybind11_add_module(nanodb_py src/python_bindings.cpp)
target_link_libraries(nanodb_py PRIVATE nano_core OpenMP::OpenMP_CXX)
set_target_properties(nanodb_py PROPERTIES OUTPUT_NAME "nanodb")

# ---- Benchmarks ----
add_executable(benchmark_throughput benchmarks/benchmark_throughput.cpp)
target_link_libraries(benchmark_throughput PRIVATE nano_core OpenMP::OpenMP_CXX)

add_executable(benchmark_recall benchmarks/benchmark_recall.cpp)
target_link_libraries(benchmark_recall PRIVATE nano_core OpenMP::OpenMP_CXX)

# ---- Tests ----
enable_testing()
add_subdirectory(tests)
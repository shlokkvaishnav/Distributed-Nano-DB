<div align="center">

# ⚡ NanoDB

[![C++17](https://img.shields.io/badge/C%2B%2B-17-orange?style=flat-square&logo=cplusplus)](https://en.cppreference.com/w/cpp/17)
[![Build](https://img.shields.io/github/actions/workflow/status/shlokkvaishnav/nano-db/ci.yml?style=flat-square&label=build)](https://github.com/shlokkvaishnav/nano-db/actions)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20Windows-lightgrey?style=flat-square)]()
[![Docker](https://img.shields.io/badge/docker-ready-2496ED?style=flat-square&logo=docker&logoColor=white)](https://github.com/shlokkvaishnav/nano-db/pkgs/container/nano-db)

> A persistent vector search engine built from scratch in C++17.

</div>

NanoDB started as an experiment: what does it take to build a real vector database without wrapping an existing library? The answer is about 3,000 lines of C++ — a custom HNSW graph, a memory-mapped storage engine, SIMD-accelerated distance computations, stripe-lock concurrency, and a REST API that ships as a single Docker container.

It sits between FAISS (fast, no persistence, no API) and Milvus (full-featured, heavyweight). If you need a persistent, embeddable vector search engine with zero external dependencies and a clean HTTP interface, NanoDB is that.

## Live

[![Docker](https://img.shields.io/badge/docker%20run-ghcr.io%2Fshlokkvaishnav%2Fnano--db-2496ED?style=flat-square&logo=docker&logoColor=white)](https://github.com/shlokkvaishnav/nano-db/pkgs/container/nano-db)

Built and maintained by **[Shlok Vaishnav](https://github.com/shlokkvaishnav)**.

## Table of Contents

1. [Why NanoDB](#why-nanodb)
2. [Features](#features)
3. [Quick Start](#quick-start)
4. [REST API](#rest-api)
5. [Python Bindings](#python-bindings)
6. [Architecture](#architecture)
7. [Performance](#performance)
8. [Building from Source](#building-from-source)
9. [File Tree](#file-tree)
10. [Contributing](#contributing)
11. [License](#license)

---

## Why NanoDB

| | FAISS | Milvus | **NanoDB** |
|---|:---:|:---:|:---:|
| Persistent storage | ❌ | ✅ | ✅ |
| REST API | ❌ | ✅ | ✅ |
| Datasets larger than RAM | ❌ | ✅ | ✅ |
| External dependencies | None | etcd, MinIO, Kafka | **None** |
| Single binary / Docker | ❌ | ❌ | ✅ |
| Python bindings | ✅ | ✅ | ✅ |

## Features

| Area | What was built |
|------|----------------|
| Storage Engine | Memory-mapped files for zero-copy access and datasets larger than RAM |
| HNSW Index | Custom O(log N) graph built from scratch — no FAISS wrapper |
| SIMD Acceleration | AVX2 hand-vectorized L2, Cosine, and Inner Product — 4–8x faster than scalar |
| Concurrency | Stripe SpinLocks per node; 6,500+ TPS at 8 threads with no global insert lock |
| Scalar Quantization | int8 SQ with per-dimension calibration — 4x memory reduction, ~1–5% recall loss |
| Lazy Deletion | O(1) tombstone marking; deleted nodes filtered at query time |
| REST API | 4 endpoints via cpp-httplib; ships as a Docker container |
| Python Bindings | pybind11 zero-copy bindings for `HNSW`, `MMapHandler`, `ScalarQuantizer` |
| CI | GitHub Actions: build + ctest on every push and pull request |

## Quick Start

**Docker — recommended:**
```bash
docker run -p 8080:8080 -v nanodb-data:/data ghcr.io/shlokkvaishnav/nano-db
```

**docker-compose:**
```bash
git clone --recurse-submodules https://github.com/shlokkvaishnav/nano-db.git
cd nano-db && docker-compose up
```

See [Building from Source](#building-from-source) if you need to build without Docker.

## REST API

All endpoints accept and return `application/json`. Default port: `8080`.

| Method | Path | Request body | Response |
|--------|------|-------------|----------|
| `POST` | `/vectors` | `{"id": int, "vector": [float...], "metadata": string}` | `201` |
| `POST` | `/search` | `{"vector": [float...], "k": int}` | `200` + results array |
| `DELETE` | `/vectors/:id` | — | `200` |
| `GET` | `/stats` | — | `200` + count, dim, metric |

**Environment variables:**

| Variable | Default | Description |
|----------|---------|-------------|
| `NANODB_PORT` | `8080` | HTTP server port |
| `NANODB_DATA_DIR` | `data` | Directory for `index.ndb` and `meta.bin` |

## Python Bindings

```python
import nanodb

storage = nanodb.MMapHandler()
storage.open_file("data/index.ndb", 50 * 1024 * 1024)
index = nanodb.HNSW(storage, "data/meta.bin", nanodb.DistanceMetric.Cosine)

index.insert([0.1] * 128, id=1, metadata="doc_001")
results = index.search(query=[0.1] * 128, k=5)
index.delete_vector(1)

storage.close_file()
```

**Distance metrics:**

| Metric | Use case |
|--------|---------|
| `L2` | General-purpose ANN |
| `Cosine` | BERT, OpenAI, sentence-transformers |
| `InnerProduct` | Recommendation systems, pre-normalized vectors |

## Architecture

```
+----------------------------------------------------------+
|  REST API            cpp-httplib · JSON · :8080          |
+----------------------------------------------------------+
|  HNSW Index          O(log N) search · 3 metrics         |
|                      AVX2 SIMD · Stripe SpinLocks        |
|                      Lazy tombstone deletion             |
+---------------------------+------------------------------+
|  MMap Storage Engine      |  Metadata Store             |
|  mmap · zero-copy         |  Append-only log            |
|  datasets > RAM           |  In-memory offset index     |
+---------------------------+------------------------------+
```

**Storage engine** — Uses `mmap()` instead of `fread()`. Pages load lazily on first access. No kernel-to-user copy. Enables searching a 100GB index on an 8GB machine.

**Offset-based addressing** — Every node reference is stored as a relative byte offset, not a raw pointer. The index file is fully relocatable with zero deserialization cost on load.

**Stripe SpinLocks** — One `SpinLock` per node. Inserting node A only locks A; other threads modify unrelated nodes simultaneously. Beats `std::mutex` for ~100ns critical sections.

For full technical depth, see [docs/INTERNALS.md](docs/INTERNALS.md).

## Performance

> Run `./benchmark_throughput` and `./benchmark_recall` to reproduce on your hardware.

| Metric | Single-threaded | 8 threads |
|--------|:-:|:-:|
| Insert throughput | ~2,200 TPS | **~6,500 TPS** |
| Multi-thread speedup | 1.0x | 2.88x |
| Search latency | ~0.15 ms | ~0.15 ms |
| Recall@10 | >= 95% | >= 95% |

The 2.88x ceiling (not 8x) comes from `global_resize_lock_` serializing rare storage expansion events and memory bandwidth saturation at ~2KB per insert. FAISS and Milvus hit the same wall.

## Building from Source

**Requirements:** GCC or Clang with C++17 support, CMake 3.10+, CPU with AVX2 (Intel Haswell 2013+ / AMD Ryzen 2017+).

**Linux / macOS:**
```bash
git clone --recurse-submodules https://github.com/shlokkvaishnav/nano-db.git
cd nano-db
cmake -B build -DCMAKE_BUILD_TYPE=Release -DNANODB_BUILD_PYTHON=OFF
cmake --build build -j$(nproc)
cd build && ctest --output-on-failure
./nano_server
```

**Windows:**
```bash
cmake -B build
cmake --build build --config Release
ctest -C Release --output-on-failure
.\build\Release\nano_server.exe
```

To build Python bindings, pass `-DNANODB_BUILD_PYTHON=ON` to cmake.

## File Tree

```
nano-db/
  include/
    config/          constants.hpp, types.hpp, utils.hpp
    concurrency/     spinlock.hpp
    index/           hnsw.hpp, graph_node.hpp, distance.hpp, quantizer.hpp
    storage/         memory_map.hpp, metadata_store.hpp, serializer.hpp
  src/
    index/           distance.cpp (AVX2 implementations)
    storage/         memory_map.cpp (mmap for Linux + Windows)
    server.cpp       REST API server
    python_bindings.cpp
    main.cpp         CLI demo
  tests/             Distance, HNSW, persistence tests
  benchmarks/        Throughput and recall benchmarks
  docs/              INTERNALS.md (full technical deep-dive)
  extern/httplib/    cpp-httplib submodule (header-only)
  Dockerfile         Multi-stage: gcc:13 build -> debian:bookworm-slim runtime
  docker-compose.yml Single service, named volume for data/
  .github/workflows/ ci.yml — build + ctest on push/PR
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE) — free to use, modify, and distribute.

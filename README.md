# ⚡ NanoDB: High-Performance Vector Search Engine

> A high-throughput, persistent Vector Database built from scratch in C++17 with Python bindings.

![C++](https://img.shields.io/badge/Language-C%2B%2B17-blue)
![Python](https://img.shields.io/badge/Bindings-Python%203.11%2B-yellow)
![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux-lightgrey)
![Status](https://img.shields.io/badge/Status-Functional-brightgreen)

**NanoDB** is a lightweight vector search engine designed to handle high-dimensional embedding vectors (e.g., 128d, 768d). Unlike wrapper libraries, NanoDB implements a custom **HNSW (Hierarchical Navigable Small World)** graph from scratch with disk-backed persistence.

It bridges the gap between raw algorithms (like FAISS) and full-scale databases (like Milvus) by offering a persistent, mmap-based storage engine without external dependencies.

---

## 🚀 Key Engineering Features

### 1. Hybrid Storage Engine
* **Vector Storage:** Uses **Memory Mapped Files (mmap)** to handle datasets larger than physical RAM. The OS page cache manages memory, allowing instant load times (Zero-Copy).
* **Metadata Storage:** Implements an **Append-Only Log** with an in-memory offset index to store variable-length strings (filenames, JSON labels) alongside vectors.

### 2. High-Performance Indexing
* **HNSW Graph:** Logarithmic time complexity $O(\log N)$ for searching millions of vectors.
* **SIMD Acceleration:** All three distance metrics are hand-optimized using **AVX2 Intrinsics**, achieving 4x–8x speedups over standard loops.

### 3. Multiple Distance Metrics
* **L2 (Euclidean):** Default. Squared distance, sqrt skipped for speed. General-purpose ANN.
* **Cosine:** `1 - cosine_similarity`. Standard for NLP embedding pipelines (BERT, OpenAI embeddings, sentence transformers).
* **Inner Product:** `-dot(a, b)`. Used in recommendation systems and with pre-normalized vectors (equivalent to cosine on unit vectors, but faster).

### 4. Lazy Deletion
* **Tombstone-based deletion:** `delete_vector(id)` marks a node as deleted in O(1). Deleted nodes are filtered at query time with no recall impact on live nodes.
* **Tradeoff:** The node remains in the graph structure (edges still traverse it). Recall degrades slightly as the fraction of deleted nodes grows. Periodic index rebuilds are the standard remedy — the same approach used by FAISS IVF.

### 5. Scalar Quantization (int8)
* **4x memory reduction:** 128d float32 = 512 bytes → 128d int8 = 128 bytes per vector.
* **ScalarQuantizer:** Trains per-dimension min/max, quantizes to `[-127, 127]`, and provides integer L2 distance for fast pre-filtering.
* Typical recall loss: **1–5%** at equivalent `ef_search` settings.

### 6. Concurrency & Locking
* **Fine-Grained Locking:** Replaces global mutexes with a **Stripe of Atomic SpinLocks**, minimizing contention.
* **Parallel Insertion:** Thread-safe architecture allows **6,500+ TPS** (Transactions Per Second) with 8+ concurrent threads.

---

## 📊 Performance Benchmarks

> **Hardware:** *(Fill in before publishing — see template below)*
> ```
> CPU:   [e.g. Intel Core i7-12700H, 14 cores, 20 threads, 2.3–4.7 GHz]
> Cache: [e.g. L1 48KB/core, L2 1.25MB/core, L3 24MB shared]
> RAM:   [e.g. 32GB DDR5-4800 dual-channel]
> OS:    [e.g. Windows 11 22H2 / Ubuntu 22.04 LTS]
> ```
> Run `benchmark_throughput` and `benchmark_recall` to reproduce these numbers on your hardware.

*Dimensions: 128d (Float32)*

| Metric | Single-Threaded | Multi-Threaded (8 Threads) |
| :--- | :--- | :--- |
| **Throughput (Insert)** | ~2,200 TPS | **~6,500 TPS** |
| **Speedup** | 1.0x | **2.88x** |
| **Search Latency** | ~0.15 ms | ~0.15 ms |

### Why is the 8-thread speedup sublinear (2.88x, not 8x)?

This is expected and worth understanding:

1. **Lock contention on resize:** The `global_resize_lock_` serializes storage expansion events. With 8 threads inserting simultaneously, these rare-but-blocking events become a bottleneck.
2. **Memory bandwidth saturation:** Each insert writes a full `Node` struct (~2KB for 128d float32 + neighbor arrays). 8 concurrent threads saturate the memory bus before all CPU cores are fully utilized.
3. **Cache thrashing:** Inserting a node requires reading its neighbors' vectors to compute distances. Concurrent inserts from different threads cause cache line invalidations across cores.

This is the same reason FAISS and Milvus cap parallel build speedup at 3–5x on commodity hardware.

---

## 🛠️ Installation & Build

NanoDB uses **CMake** for cross-platform build management.

### Prerequisites
* C++17 Compiler (MSVC, GCC, or Clang)
* CMake 3.10+
* Python 3.x (for bindings)
* CPU with AVX2 support (Intel Haswell 2013+ / AMD Ryzen 2017+)

### Windows Build
```powershell
git clone https://github.com/shlokkvaishnav/nano-db.git
cd nano-db
mkdir build && cd build
cmake ..
cmake --build . --config Release

# Run tests
ctest -C Release --output-on-failure

# Run benchmarks
.\Release\benchmark_throughput.exe
.\Release\benchmark_recall.exe > recall_results.csv
```

### Linux/Mac Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
ctest --output-on-failure
./benchmark_throughput
./benchmark_recall > recall_results.csv
```

---

## 💻 Usage (Python)

NanoDB provides native Python bindings (`pybind11`) for easy integration with AI pipelines.

```python
import sys
sys.path.insert(0, 'build/Release')  # or 'build' on Linux
import nanodb
import random

# 1. Initialize DB
storage = nanodb.MMapHandler()
storage.open_file("data/index.ndb", 50 * 1024 * 1024)  # 50MB pre-allocation

# Choose your distance metric:
#   nanodb.DistanceMetric.L2           — general-purpose (default)
#   nanodb.DistanceMetric.Cosine       — NLP embeddings
#   nanodb.DistanceMetric.InnerProduct — recommendation systems
index = nanodb.HNSW(storage, "data/meta.bin", nanodb.DistanceMetric.Cosine)

# 2. Insert vectors
vector = [random.random() for _ in range(128)]
index.insert(vector, id=1, metadata="cat_photo.jpg")

# 3. Search
results = index.search(query=vector, k=5)
for res in results:
    print(f"ID: {res.id}  Distance: {res.distance:.4f}  Meta: {res.metadata}")

# 4. Delete (lazy tombstone)
index.delete_vector(1)
# Node 1 will no longer appear in search results

# 5. Scalar Quantization (4x memory reduction)
sq = nanodb.ScalarQuantizer()
dataset = [[random.random() for _ in range(128)] for _ in range(1000)]
sq.train(dataset)
quantized = sq.quantize(vector)  # list of int8 values
approx = sq.dequantize(quantized, 128)  # approximate float reconstruction

storage.close_file()
```

---

## 🧠 System Architecture

### 1. The "MMap" Storage Engine (Zero-Copy)

Most databases read files using `fread`, which copies data from Disk → Kernel Buffer → User RAM. This is slow and consumes physical memory immediately.

**NanoDB uses Memory Mapped Files (mmap):**

* **Lazy Loading:** The OS maps the file into the process's virtual address space but only loads physical **Pages (4KB)** when they are actually accessed.
* **Huge Datasets:** This allows NanoDB to search a **100GB dataset on a machine with only 8GB of RAM**, relying on the OS page cache for memory management.

### 2. Offset-Based Addressing (The "Pointer" Solution)

A major challenge in C++ database design is that standard pointers (`Node*`) store **absolute memory addresses** (e.g., `0x7fff5b...`). If you save these to disk and reload them, the OS will likely load the file at a different address, making the pointers invalid.

**The Solution:**
Instead of absolute pointers, NanoDB uses **Relative Offsets** (e.g., "Node B is 1024 bytes from the start of the file").

* **Portability:** The database file is "relocatable." It works instantly regardless of where it is loaded in memory.
* **Zero Serialization:** We don't need to parse or convert data when opening the DB. We just map the file and start reading.

### 3. The Index (HNSW)

The graph is constructed with layers. Search starts at the sparse top layer (Layer L) and zooms in to the dense bottom layer (Layer 0), using the **Offset Manager** to traverse links between nodes.

### 4. Why Stripe Locks Instead of a Single Mutex?

A single global mutex would serialize every insert, making multi-threaded insertion equivalent to single-threaded. NanoDB assigns each node its own `SpinLock` (stored in a pre-allocated vector). When adding a link from node A to node B, only node A's lock is held — other threads can simultaneously modify unrelated nodes.

**SpinLock vs `std::mutex`:** For short, predictable critical sections (updating a neighbor list takes ~100ns), spinning is faster than the OS context switch overhead of `std::mutex` (~1–5µs on Linux).

---

## 📜 License

MIT License. Free to use and modify.
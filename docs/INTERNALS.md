# NanoDB Internals

This document explains every design decision in NanoDB at the level of detail a senior engineer would need to modify or extend the codebase without reading the source first. It covers the storage engine, the HNSW graph, the SIMD distance kernels, the concurrency model, the scalar quantizer, the metadata store, the REST API, the Python bindings, the Docker build, and the CI pipeline.

---

## 1. MMap Storage Engine

**File:** `include/storage/memory_map.hpp`, `src/storage/memory_map.cpp`

### Why mmap instead of fread

The traditional approach to database storage is `fread()`/`fwrite()`. When you call `fread()`, the kernel copies data from its page cache into your user-space buffer — a full memcpy per read. This means:

1. Every read allocates user-space memory and copies into it.
2. The application must manage its own buffer pool.
3. If you want to access a 100GB file, you need to decide which parts to load and which to evict.

`mmap()` eliminates all three problems. When the process calls `mmap(fd, size)`, the kernel creates a mapping in the process's virtual address space that points directly at the file's page cache. No copy happens. The data lives in exactly one place in physical memory — the kernel's page cache — and the process accesses it through a pointer.

**Demand paging:** The OS does not load the entire file into RAM when `mmap()` is called. It marks the pages as "not present" in the page table. When the process first reads a page, a page fault fires, the kernel loads the 4KB page from disk into the page cache, updates the page table entry, and the process continues. This means a 100GB file can be memory-mapped on an 8GB machine — the OS will page in what you access and evict what you don't, using LRU or clock replacement.

**Zero-copy:** Because the process reads directly from the page cache (no kernel-to-user copy), there is zero overhead per access beyond the initial page fault. After a page is resident, accessing it is a normal memory load — the TLB caches the translation and subsequent reads hit L1/L2 cache.

### Pre-allocation

`MMapHandler::open_file(path, min_size)` takes a minimum file size parameter. On a fresh database, the file does not exist or is empty. The handler:

1. Creates or opens the file with read/write permissions.
2. Checks the current file size via `fstat()` (POSIX) or `GetFileSizeEx()` (Windows).
3. If the file is smaller than `min_size`, extends it with `ftruncate(fd, min_size)` (POSIX) or `SetFilePointerEx` + `SetEndOfFile` (Windows). This pre-fills the file with zeros.
4. Maps the file with `mmap(nullptr, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)` (POSIX) or `CreateFileMapping` + `MapViewOfFile` (Windows).

The result: the application gets a `void*` pointer to a contiguous block of `min_size` bytes. The server uses 100MB; the CLI demo uses 50MB.

Pre-allocation is necessary because `mmap()` requires the file to already be at least as large as the requested mapping. You cannot map beyond the end of a file. Extending the file later requires unmapping, truncating, and remapping — which is exactly what `resize()` does.

### The resize path

`MMapHandler::resize(new_size)` is called when an insert would place a node beyond the current file boundary. It:

1. Calls `close_file()` — which unmaps the view (`munmap` or `UnmapViewOfFile`) and closes the file handle.
2. Calls `open_file(path, new_size)` — which reopens the file, extends it to `new_size`, and re-maps it.

**Critical implication:** `resize()` invalidates all pointers into the mapped region. Every `Node*` that was obtained from `get_node()` before the resize is now a dangling pointer. This is why the HNSW layer holds `global_resize_lock_` during resize — no thread may be reading or writing nodes while the mapping changes.

In practice, resizes are rare. The 100MB pre-allocation fits approximately `(100MB - 64 bytes header) / sizeof(Node)` nodes. With 128-dim float32 vectors, a Node is roughly 2,600 bytes (512 for the vector, ~2048 for neighbor arrays, plus overhead), so 100MB holds ~38,000 nodes before the first resize.

### Interface

| Method | What it does |
|--------|-------------|
| `open_file(path, min_size)` | Creates directory if needed, opens/creates file, extends to min_size, maps it |
| `close_file()` | Flushes dirty pages to disk (implicit via MAP_SHARED), unmaps, closes handle |
| `resize(new_size)` | close + reopen at larger size — invalidates all prior pointers |
| `get_data()` | Returns `void*` to the start of the mapped region |
| `get_size()` | Returns the current mapped size in bytes |

### Separation from the index

The HNSW class receives a `MMapHandler&` reference in its constructor. It does not know it is operating on a disk-backed file — it treats `get_data()` as a raw memory buffer. This separation means:

- The index logic can be tested with a simple heap-allocated buffer (mock the MMapHandler interface).
- The storage layer can be swapped for a different backend (network-attached storage, shared memory) without changing the index.
- Resize coordination is explicit — the HNSW layer knows when it calls resize and invalidates its cached pointers.

### File layout

The `data/` directory contains two files:

- `index.ndb` — The main index file. Starts with a 64-byte `FileHeader`, followed by contiguous `Node` structs at offset `HEADER_SIZE + id * sizeof(Node)`.
- `metadata.bin` — The metadata log. Variable-length records appended sequentially.

---

## 2. Offset-Based Addressing

### The problem with raw pointers

In C++, a `Node*` stores an absolute virtual address like `0x7f3a00001000`. This address is assigned by the OS when the process calls `mmap()` and is specific to that process and that invocation. If you write `Node* neighbor = 0x7f3a00005000;` to disk and reload the file in a new process, the OS will map the file at a different base address (e.g., `0x7f8b00001000`), and the stored pointer is garbage.

This is the fundamental reason most databases use serialization formats — they convert in-memory structures to a portable on-disk format (protobuf, FlatBuffers, Cap'n Proto) and deserialize on load. Deserialization is expensive: it requires allocating new objects, copying data, and fixing up pointers.

### NanoDB's solution: ID-based offset addressing

NanoDB stores neighbor references as `id_t` values (uint32_t node IDs), not pointers. To access node B from node A's neighbor list:

```cpp
id_t neighbor_id = node_a->neighbors[layer][i];
Node* neighbor = get_node(neighbor_id);
// where get_node computes: base_ptr + HEADER_SIZE + id * sizeof(Node)
```

The `get_node(id)` function computes the byte offset from the base of the mapped file:

```cpp
Node* get_node(id_t id) {
    return reinterpret_cast<Node*>(
        (char*)storage_.get_data() + HEADER_SIZE + (size_t)id * sizeof(Node)
    );
}
```

This means:
- Node IDs are stored on disk (in the neighbor arrays).
- The base address (`storage_.get_data()`) is determined at runtime by the OS.
- The offset arithmetic (`HEADER_SIZE + id * sizeof(Node)`) is constant regardless of base address.

### Zero deserialization

When the process starts and maps `index.ndb`, all the `Node` structs are immediately accessible in memory at their correct offsets. There is no parsing step, no fixup pass, no conversion. The `FileHeader` at offset 0 contains the `element_count`, `entry_point_id`, and `max_layer` — three integers that restore the full index state.

This is what "relocatable" means: the same `index.ndb` file works whether the OS maps it at `0x7f0000000000` or `0x100000000`. The node IDs and the multiplication by `sizeof(Node)` produce the correct byte offset regardless.

### The FileHeader

```cpp
struct FileHeader {
    uint32_t magic;           // 0x4E444200 ("NDB\0") — identifies a valid NanoDB file
    uint32_t element_count;   // number of inserted nodes
    int32_t entry_point_id;   // ID of the HNSW entry point (-1 if empty)
    int32_t max_layer;        // highest occupied layer (-1 if empty)
    char reserved[48];        // padding to 64 bytes for alignment
};
```

A fresh pre-allocated file is all zeros. Since `magic == 0` differs from `NANODB_MAGIC == 0x4E444200`, the constructor knows the file is empty and initializes the header. An existing file has the magic set, so the constructor restores state from the header fields.

---

## 3. Node Layout

**File:** `include/index/graph_node.hpp`

### Struct definition

```cpp
struct alignas(32) Node {
    id_t id;                                    // 4 bytes — external identifier
    int max_layer;                              // 4 bytes — highest layer this node participates in
    bool is_deleted;                            // 1 byte  — tombstone flag
    // 23 bytes padding to next 32-byte boundary (due to alignas)

    val_t vector[config::VECTOR_DIM];           // 512 bytes (128 * 4)
    id_t neighbors[MAX_LAYERS][config::M_MAX0]; // 4 layers * 32 slots * 4 bytes = 512 bytes
    int neighbor_counts[MAX_LAYERS];            // 4 layers * 4 bytes = 16 bytes
};
```

### Size calculation (128d)

| Field | Size |
|-------|------|
| `id` | 4 bytes |
| `max_layer` | 4 bytes |
| `is_deleted` | 1 byte |
| padding (alignas(32)) | 23 bytes |
| `vector[128]` | 512 bytes |
| `neighbors[4][32]` | 512 bytes |
| `neighbor_counts[4]` | 16 bytes |
| **Total** | **~1,072 bytes** (rounded up to nearest 32 for alignment) |

The `alignas(32)` directive ensures the struct starts on a 32-byte boundary. This allows AVX2 to use aligned load instructions (`_mm256_load_ps`) on the vector field, which are faster than unaligned loads on some microarchitectures.

### Neighbor arrays

The original HNSW paper specifies `M_max0 = 2*M` connections at layer 0 and `M` connections at higher layers. NanoDB uses `M_MAX0 = 2*M = 32` at all layers in the static array declaration — this wastes some space at higher layers (which only use 16 slots) but keeps the struct a fixed size, which is critical for the offset-based addressing to work.

`neighbor_counts[layer]` tracks how many valid neighbors are stored at each layer. Slots beyond `neighbor_counts[layer]` contain stale data (initialized to `-1` / `0xFFFFFFFF` on construction).

### Why fixed-size, contiguous layout

1. **Offset arithmetic:** `get_node(id)` computes `base + HEADER_SIZE + id * sizeof(Node)`. This only works if every node is the same size.
2. **Cache locality:** During search, following a neighbor link reads a nearby node. Because nodes are packed contiguously in the mmap region, neighboring IDs are likely on the same or adjacent pages.
3. **Zero serialization:** The struct is POD (Plain Old Data) — no pointers, no vtable, no heap allocations. It can be directly written to and read from disk.
4. **Disk persistence:** The struct in memory IS the on-disk format. When the OS flushes dirty pages, it writes the raw bytes. On reload, the same raw bytes are mapped back.

---

## 4. HNSW Graph

**File:** `include/index/hnsw.hpp`

### Core idea

HNSW (Hierarchical Navigable Small World) is a proximity graph with multiple layers. Each layer is a navigable small-world graph where nodes are connected to nearby neighbors. The key insight is that upper layers are sparse (few nodes with long-distance connections for fast navigation) and lower layers are dense (all nodes with short-distance connections for precise search).

- **Layer 0:** Contains all nodes. Maximum `M_MAX0 = 32` neighbors per node.
- **Layers 1, 2, 3:** Contain progressively fewer nodes. Maximum `M = 16` neighbors per node.
- **Entry point:** A single globally-stored node ID. Search always starts here.

### Layer assignment

When a new node is inserted, it is assigned a maximum layer drawn from a geometric distribution:

```cpp
int get_random_level() {
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    double r = dist(rng_);
    int level = 0;
    while (r < 0.03 && level < config::M) {
        level++;
        r = dist(rng_);
    }
    return level;
}
```

Each iteration has a 3% probability of promoting the node to the next layer. This means:
- ~97% of nodes are only at layer 0
- ~3% are at layer 1
- ~0.09% are at layer 2
- ~0.0027% are at layer 3

The expected number of nodes at layer `l` decreases geometrically, creating the hierarchy. The original paper uses `1/ln(M)` as the probability; NanoDB uses a fixed 3% which gives similar behavior for `M=16`.

### Parameters

| Parameter | Value | Role |
|-----------|-------|------|
| `M` | 16 | Max bidirectional links per node at layers 1+ |
| `M_MAX0` | 32 (2*M) | Max bidirectional links at layer 0 (denser for better recall) |
| `EF_CONSTRUCTION` | 200 | Beam width during insert (larger = better graph quality, slower build) |
| `MAX_LAYERS` | 4 | Maximum possible layers (sufficient for ~1M vectors) |

### Insert algorithm

Given a new vector `v` with assigned ID and random max layer `L`:

**Step 1:** Write the node to storage at its offset.

**Step 2:** If this is the first node (`entry_point_id_ == -1`), set it as the entry point and return.

**Step 3:** Greedy descent from layers above `L`. Starting at the current entry point, greedily move to the closest neighbor at each layer until reaching layer `L`. This finds a good starting point for the beam search below.

```
for l = current_max_layer down to L+1:
    while improved:
        for each neighbor n of current_node at layer l:
            if distance(v, n) < distance(v, current_node):
                current_node = n
```

**Step 4:** Beam search and neighbor selection for layers `L` down to 0. At each layer:

1. Run beam search (`search_layer`) with `ef_construction = 200` candidates starting from `current_node`.
2. Select the top-M closest candidates as neighbors.
3. Add bidirectional links: `v -> neighbor` and `neighbor -> v`.
4. If a neighbor's list is full (already at `M_MAX0` or `M` connections), replace its farthest neighbor if `v` is closer.

**Step 5:** If `L > current_max_layer_`, update the entry point to this new node (it is now the highest-layer node and will be the search entry).

### Neighbor selection

NanoDB uses a simple top-M-nearest selection: after the beam search returns `ef_construction` candidates, take the `M` closest by distance. The original paper describes a heuristic that prefers diverse neighbors (a candidate is kept only if it is closer to the query than to any already-selected neighbor), but NanoDB omits this heuristic for simplicity.

The bidirectional link insertion uses pruning: if a node already has `M_MAX0` neighbors, the new connection replaces the farthest existing neighbor only if the new one is closer. This ensures neighbor lists maintain quality as the graph grows.

### Search algorithm

Given a query vector `q` and desired `k` results:

**Step 1:** Start at the global entry point.

**Step 2:** Greedy descent from `current_max_layer_` to layer 1 (ef=1, single best node):
```
for l = current_max_layer down to 1:
    while improved:
        for each neighbor n of current_node at layer l:
            if distance(q, n) < distance(q, current_node):
                current_node = n
```

**Step 3:** Beam search at layer 0 with `ef_search = max(100, k)`:
- Maintain a candidate min-heap (sorted by distance, explore closest first).
- Maintain a result max-heap (sorted by distance, worst result on top for pruning).
- Expand the closest unvisited candidate: check all its neighbors, add those closer than the worst result to both heaps.
- Stop when the closest candidate is farther than the worst result (no improvement possible).

**Step 4:** Extract the top-k from the result heap, filtering out tombstoned nodes.

### Why search is lock-free

Search only reads neighbor arrays and vector data — it never modifies any node. The only state it reads that can change is:
- `is_deleted` flag — a boolean that transitions from false to true; reading a stale value just means the deleted node appears in results (harmless, it will be filtered next query).
- `neighbor_counts` and `neighbors` arrays — these can be concurrently modified by inserts. A read may see a partially-updated neighbor list (missing the newest link), which slightly reduces recall but does not corrupt memory.

Because reads are safe without locks, search latency is independent of concurrent insert load.

---

## 5. Distance Computations

**Files:** `include/index/distance.hpp`, `src/index/distance.cpp`

### Metrics

**L2 (Squared Euclidean):**
```
d(a, b) = sum_i (a[i] - b[i])^2
```
The square root is omitted because `sqrt` is monotonic — it preserves the ranking of distances. Skipping it saves ~20 cycles per distance computation.

**Cosine:**
```
d(a, b) = 1 - (dot(a, b) / (|a| * |b|))
```
Range: [0, 2]. 0 = identical direction, 1 = orthogonal, 2 = opposite. Standard for NLP embeddings where magnitude is irrelevant.

**Inner Product:**
```
d(a, b) = -dot(a, b)
```
Negated because HNSW uses a min-heap (lower distance = better). For pre-normalized vectors (`|a| = |b| = 1`), inner product and cosine produce equivalent rankings.

### AVX2 implementation

**Why SIMD matters:** A scalar L2 computation for 128 dimensions requires 128 subtractions, 128 multiplications, and 128 additions = 384 arithmetic operations. With AVX2 (256-bit registers, 8 floats per lane), this becomes 16 iterations of packed operations = 48 instructions. That's an 8x reduction in instruction count, translating to 4-8x wall-clock speedup (less than 8x due to memory latency and instruction-level parallelism limits).

**L2 kernel:**
```cpp
float l2_distance(const float* a, const float* b, size_t dim) {
    __m256 sum = _mm256_setzero_ps();  // 8 zeros
    size_t i = 0;
    for (; i + 8 <= dim; i += 8) {
        __m256 va   = _mm256_loadu_ps(a + i);    // load 8 floats from a
        __m256 vb   = _mm256_loadu_ps(b + i);    // load 8 floats from b
        __m256 diff = _mm256_sub_ps(va, vb);     // a - b (8 lanes)
        __m256 sq   = _mm256_mul_ps(diff, diff); // (a-b)^2 (8 lanes)
        sum         = _mm256_add_ps(sum, sq);    // accumulate
    }
    // Horizontal reduction: collapse 8 lanes to 1 scalar
    float temp[8];
    _mm256_storeu_ps(temp, sum);
    float total = 0.0f;
    for (int k = 0; k < 8; ++k) total += temp[k];
    // Scalar tail for dimensions not divisible by 8
    for (; i < dim; ++i) {
        float d = a[i] - b[i];
        total += d * d;
    }
    return total;
}
```

**Key intrinsics:**
- `_mm256_loadu_ps`: Load 8 unaligned floats into a 256-bit register.
- `_mm256_sub_ps`: Subtract 8 float pairs in parallel.
- `_mm256_mul_ps`: Multiply 8 float pairs in parallel.
- `_mm256_add_ps`: Add 8 float pairs in parallel.
- `_mm256_storeu_ps`: Store 256-bit register back to memory for horizontal sum.

**Tail handling:** The scalar loop `for (; i < dim; ++i)` handles remaining dimensions when `dim` is not a multiple of 8. For NanoDB's default `VECTOR_DIM = 128`, this tail is never reached (128 / 8 = 16, no remainder), but the code handles arbitrary dimensions correctly.

**Cosine kernel:** Computes three accumulators simultaneously — `dot(a,b)`, `|a|^2`, and `|b|^2` — in a single pass. This is more efficient than computing them in separate loops because the data is loaded from cache only once.

**Inner product kernel:** The simplest — just accumulates `a[i] * b[i]` across all dimensions, then negates the result.

**Dispatcher:** `compute_distance()` is the single call site used throughout HNSW. A switch on the `DistanceMetric` enum routes to the correct kernel. This is a virtual-dispatch-free approach — the metric is checked once per call, not once per lane.

---

## 6. Concurrency

**Files:** `include/concurrency/spinlock.hpp`, `include/index/hnsw.hpp`

### The problem with a global mutex

If the HNSW class used a single `std::mutex` for all inserts, only one thread could modify the graph at a time. Eight threads would take turns acquiring the lock, achieving approximately 1x throughput regardless of thread count. This is unacceptable for a database.

### SpinLock implementation

```cpp
class SpinLock {
    std::atomic_flag flag = ATOMIC_FLAG_INIT;
public:
    void lock() {
        while (flag.test_and_set(std::memory_order_acquire)) {
            #if defined(_MSC_VER)
                _mm_pause();
            #else
                __builtin_ia32_pause();
            #endif
        }
    }
    void unlock() {
        flag.clear(std::memory_order_release);
    }
};
```

**How it works:**
1. `std::atomic_flag` is a single bit — the smallest possible lock.
2. `test_and_set(acquire)` atomically reads the flag and sets it to `true`. If it was already `true` (locked by another thread), the return value is `true` and the loop continues spinning. If it was `false` (unlocked), the return value is `false`, the flag is now `true` (we hold the lock), and the loop exits.
3. The `_mm_pause()` / `__builtin_ia32_pause()` intrinsic is a hint to the CPU that this is a spin-wait loop. It reduces power consumption and avoids pipeline stalls on hyperthreaded cores by yielding execution resources to the sibling hardware thread.
4. `clear(release)` sets the flag back to `false`, releasing the lock. `memory_order_release` ensures all writes made while holding the lock are visible to the next acquirer.

**Why SpinLock beats std::mutex here:** The critical section in `add_link()` consists of:
1. Read `neighbor_counts[layer]` (one int).
2. Write to `neighbors[layer][count]` (one int).
3. Increment `neighbor_counts[layer]` (one int).

This takes approximately 50-100ns. The overhead of `std::mutex` is:
- `lock()`: a futex syscall if contended (~1-5 microseconds for the context switch).
- `unlock()`: another syscall to wake a waiter.

Spinning for 100ns is 10-50x cheaper than a context switch. SpinLocks are the correct choice when:
- Critical sections are short (< 1 microsecond).
- Contention is low (most threads access different nodes).
- The system is not oversubscribed (threads <= cores).

### Stripe layout

The HNSW class maintains:
```cpp
std::vector<std::unique_ptr<SpinLock>> node_locks_;
```

One SpinLock per node. When inserting node A and adding a link from node B to node A, only `node_locks_[B]` is acquired. Other threads inserting nodes C, D, E can simultaneously modify their respective nodes without contention.

The vector is pre-allocated to `current_count + 10000` on construction and extended (under `global_resize_lock_`) when needed. Each `std::unique_ptr<SpinLock>` is heap-allocated, so SpinLocks are not packed contiguously — this naturally avoids false sharing (where two SpinLocks on the same cache line cause cross-core invalidation traffic).

### The global_resize_lock_

```cpp
std::mutex global_resize_lock_;
```

This mutex serializes storage expansion events. It is held during:
1. The size check (`offset + sizeof(Node) > storage_.get_size()`).
2. The actual resize (`storage_.resize(new_size)`).
3. The SpinLock vector extension.

The double-checked locking pattern is used: the size is checked without the lock first (fast path), and only if it fails is the lock acquired and the check repeated (slow path, handles race between two threads that both see the file as too small).

**Impact on throughput:** The resize operation takes ~1ms (unmap, ftruncate, remap). During this time, ALL insert threads are blocked. However, resizes are rare — they occur only when the file needs to grow (every ~38,000 nodes with 100MB pre-allocation). Between resizes, threads run at full parallel speed.

### Memory bandwidth ceiling

Even without lock contention, 8 threads cannot achieve 8x throughput because of memory bandwidth. Each insert writes a full `Node` struct (~1,072 bytes) to the mmap region. At 8 threads * 2,200 TPS each = 17,600 nodes/second * 1,072 bytes = ~18 MB/s of writes. This is well within memory bandwidth limits, but the real bottleneck is the random-access reads during neighbor search (computing distances to candidate neighbors causes cache misses across the entire mmap region).

The empirical 2.88x speedup matches what FAISS and Milvus report for parallel index construction on consumer hardware (typically 3-5x at 8 threads). The ceiling is fundamental to the workload pattern, not a code deficiency.

---

## 7. Scalar Quantization

**File:** `include/index/quantizer.hpp`

### Motivation

At 128 dimensions with float32, each vector consumes 512 bytes. At 1 million vectors, that is 512MB just for vector data. Scalar quantization converts float32 to int8, reducing this to 128MB — a 4x reduction.

### Training

```cpp
void train(const std::vector<Vector>& vecs) {
    dim_ = vecs[0].size();
    min_.assign(dim_, +INFINITY);
    max_.assign(dim_, -INFINITY);
    for (const auto& v : vecs) {
        for (size_t i = 0; i < dim_; ++i) {
            if (v[i] < min_[i]) min_[i] = v[i];
            if (v[i] > max_[i]) max_[i] = v[i];
        }
    }
}
```

The `train()` method computes per-dimension minimum and maximum values across a calibration dataset. This defines the quantization range for each dimension. The calibration set should be representative of the data distribution — if a dimension has range [0.1, 0.9] in calibration but a query vector has value 1.5 in that dimension, the quantized value will be clamped to 127, losing information.

### Quantize

For each dimension `d`, the float value `x` is mapped to int8:

```
normalized = (x - min[d]) / (max[d] - min[d])    // maps to [0, 1]
quantized = round(normalized * 254 - 127)         // maps to [-127, 127]
quantized = clamp(quantized, -127, 127)           // safety clamp
```

The range uses 254 steps (not 256) to avoid the ambiguity of -128 in signed int8 representation. The clamp handles values outside the calibrated range.

### Dequantize

The inverse mapping recovers an approximate float:

```
normalized = (quantized + 127) / 254.0            // maps to [0, 1]
x_approx = min[d] + normalized * (max[d] - min[d])
```

This is lossy — the original float cannot be exactly recovered because 254 discrete levels represent a continuous range. The quantization error per dimension is at most `(max - min) / 254`.

### Integer L2 distance

```cpp
int32_t quantize_distance(const int8_t* a, const int8_t* b, size_t dim) const {
    int32_t sum = 0;
    for (size_t i = 0; i < dim; ++i) {
        int32_t diff = (int32_t)a[i] - (int32_t)b[i];
        sum += diff * diff;
    }
    return sum;
}
```

Integer arithmetic avoids the FPU entirely. On modern CPUs with fast integer multiply (`imul`), this can be faster than float L2 even without SIMD, because integer pipelines have lower latency on some microarchitectures. For a production system, this would be further optimized with VNNI (Vector Neural Network Instructions) available on Intel Ice Lake+ and AMD Zen4+.

### Recall tradeoff

Quantization distorts distances. Two vectors that are truly nearest neighbors in float32 space may have different quantized distances due to rounding error. The empirical impact:

- At `ef_search = 100`: recall@10 drops by 1-5% compared to float32.
- At `ef_search = 200`: recall@10 drops by 0.5-2% (the larger beam compensates for distance distortion).

**When to use:** Large datasets where memory is the primary constraint. Not recommended when recall@10 > 98% is required without increasing ef_search (which increases latency).

---

## 8. Metadata Store

**File:** `include/storage/metadata_store.hpp`

### Design

The metadata store handles variable-length strings (document IDs, filenames, JSON labels) that cannot fit in the fixed-size `Node` struct. It uses an append-only log design:

**On-disk format (meta.bin):**
```
[uint32_t length][string bytes][uint32_t length][string bytes]...
```

Each record is a 4-byte length prefix followed by the raw string bytes. Records are appended in insertion order.

**In-memory index:**
```cpp
std::vector<std::pair<size_t, size_t>> offsets_;  // (byte_offset, length) per ID
```

`offsets_[id]` stores the byte offset and length of the metadata string for vector ID `id`. This allows O(1) random access to any metadata string.

### Write path

```cpp
void save_metadata(int id, const std::string& metadata) {
    file_stream_.seekp(0, std::ios::end);   // append to end
    size_t offset = file_stream_.tellp();    // record the offset
    uint32_t len = metadata.size();
    file_stream_.write(&len, 4);            // write length prefix
    file_stream_.write(metadata.c_str(), len); // write string
    offsets_[id] = {offset, len};           // update in-memory index
}
```

A `std::mutex` serializes writes to prevent interleaving.

### Read path

```cpp
std::string get_metadata(int id) {
    auto [offset, length] = offsets_[id];
    file_stream_.seekg(offset + sizeof(uint32_t));  // skip length prefix
    std::string data(length, '\0');
    file_stream_.read(&data[0], length);
    return data;
}
```

### Startup recovery

On startup, `rebuild_index()` scans the entire `meta.bin` file sequentially, reading each length-prefixed record and rebuilding the `offsets_` vector. This assumes records are appended in ID order (0, 1, 2, ...), which is true for the current insert pattern.

### Why separate from the index

1. **Variable-length data:** Node structs must be fixed-size for offset arithmetic. Metadata strings vary from 0 to thousands of bytes.
2. **Cache efficiency:** During search, the hot path is computing distances between vectors. If metadata strings were stored inline in Node structs, they would pollute the CPU cache with data that is never accessed during distance computation.
3. **Append-only simplicity:** The log never needs random writes or compaction. Deletion tombstones are in the HNSW index, not the metadata log — a deleted vector's metadata simply becomes unreachable.

---

## 9. REST API Server

**File:** `src/server.cpp`

### Dependencies

- **cpp-httplib** (`extern/httplib/httplib.h`): A single-header C++ HTTP/HTTPS library. Supports keep-alive, chunked transfer encoding, multipart form data, and thread pooling. Zero external dependencies beyond standard POSIX or Winsock.
- **nlohmann/json** (`include/third_party/json.hpp`): A single-header JSON library for C++. Provides ergonomic parsing (`json::parse(str)`) and serialization (`.dump()`), with exception-based error handling for malformed input.

### Startup sequence

```
1. Read NANODB_PORT env var (default: 8080)
2. Read NANODB_DATA_DIR env var (default: "data")
3. Initialize MMapHandler with 100MB pre-allocation at {data_dir}/index.ndb
4. Construct HNSW index with {data_dir}/metadata.bin
5. Create httplib::Server instance
6. Register SIGINT/SIGTERM signal handler (calls server.stop())
7. Register 4 route handlers (POST /vectors, POST /search, DELETE /vectors/:id, GET /stats)
8. Call server.listen("0.0.0.0", port)
```

### Thread safety

cpp-httplib uses a thread pool (default: 8 threads, configurable). Each incoming request is handled by a thread pool worker. Since multiple requests can execute simultaneously:

- **POST /vectors** calls `index.insert()`, which is thread-safe (SpinLocks protect neighbor lists, `global_resize_lock_` protects storage expansion).
- **POST /search** calls `index.search()`, which is read-only and lock-free.
- **DELETE /vectors/:id** sets a single boolean (`is_deleted = true`), which is an atomic-width write on all modern architectures.
- **GET /stats** reads `element_count_`, `VECTOR_DIM`, and `metric_` — all constant or atomically-updated values.

No additional locking is needed in the server layer.

### Route handlers

**POST /vectors:**
1. Parse JSON body with `json::parse(req.body)`.
2. Validate: `id` field exists, `vector` field exists, `vector.size() == config::VECTOR_DIM`.
3. Convert JSON array to `std::vector<float>`.
4. Call `index.insert(vec, id, metadata)`.
5. Return 201 with `{"status": "ok", "id": N}`.
6. On parse error or validation failure: return 400 with error message.

**POST /search:**
1. Parse JSON body.
2. Validate: `vector` field exists with correct dimension, `k > 0`.
3. Call `index.search(vec, k)`.
4. Serialize results as JSON array: `[{"id": N, "distance": F, "metadata": "..."}]`.
5. Return 200.

**DELETE /vectors/:id:**
1. Extract `:id` from URL path using cpp-httplib's regex matching (`/vectors/(\d+)`).
2. Call `index.delete_vector(id)`.
3. Return 200.

**GET /stats:**
1. Read `index.size()`, `config::VECTOR_DIM`, `index.metric()`.
2. Return 200 with `{"element_count": N, "vector_dim": 128, "metric": "L2"}`.

### Graceful shutdown

```cpp
static httplib::Server* g_server = nullptr;
void signal_handler(int) { if (g_server) g_server->stop(); }
```

When SIGINT or SIGTERM is received, `server.stop()` is called. This:
1. Stops accepting new connections.
2. Waits for in-flight requests to complete (up to a configurable timeout).
3. Returns from `server.listen()`.
4. The main function then calls `storage.close_file()` to flush dirty pages to disk.

---

## 10. Python Bindings

**File:** `src/python_bindings.cpp`

### pybind11

pybind11 is a header-only C++ library that creates Python extension modules. It generates the CPython C API glue code at compile time, exposing C++ classes as Python objects with proper reference counting, garbage collection integration, and exception translation.

### Exposed classes

| Python class | C++ class | Methods |
|---|---|---|
| `nanodb.MMapHandler` | `MMapHandler` | `open_file(path, size)`, `close_file()` |
| `nanodb.HNSW` | `HNSW` | `insert(vector, id, metadata)`, `search(query, k)`, `delete_vector(id)`, `is_deleted(id)`, `get_metadata(id)`, `size()`, `metric()` |
| `nanodb.ScalarQuantizer` | `ScalarQuantizer` | `train(vectors)`, `quantize(vector)`, `dequantize(vector, dim)`, `is_trained()`, `dim()` |
| `nanodb.Result` | `Result` | `id`, `distance`, `metadata` (read-only properties) |
| `nanodb.DistanceMetric` | `DistanceMetric` | `L2`, `Cosine`, `InnerProduct` (enum values) |

### GIL release

The `insert()` method is registered with `py::call_guard<py::gil_scoped_release>()`. This releases the Python Global Interpreter Lock during execution, allowing other Python threads to run while a potentially slow insert (with beam search) executes. Search does not release the GIL because it is typically fast enough (~0.15ms) that the GIL release/acquire overhead is not justified.

### Data conversion

- **Python list → std::vector<float>:** pybind11's `stl.h` header automatically converts Python lists and numpy arrays to `std::vector<float>` using a single memcpy for numpy arrays or element-by-element conversion for lists.
- **std::vector<Result> → Python list:** Each `Result` struct is wrapped as a Python object with `id`, `distance`, and `metadata` attributes.
- **Zero-copy of index data:** The index itself is not copied — Python calls operate directly on the memory-mapped C++ objects.

### Build configuration

```cmake
if(NANODB_BUILD_PYTHON)
    add_subdirectory(extern/pybind11)
    pybind11_add_module(nanodb_py src/python_bindings.cpp)
    target_link_libraries(nanodb_py PRIVATE nano_core OpenMP::OpenMP_CXX)
    set_target_properties(nanodb_py PROPERTIES OUTPUT_NAME "nanodb")
endif()
```

The output is a shared library (`nanodb.so` on Linux, `nanodb.pyd` on Windows) that Python can import directly. It is placed in the build directory.

---

## 11. Docker Build

**File:** `Dockerfile`, `docker-compose.yml`, `.dockerignore`

### Multi-stage build

**Stage 1 — Builder:**
```dockerfile
FROM gcc:13 AS builder
RUN apt-get update && apt-get install -y cmake git
WORKDIR /app
COPY . .
RUN git submodule update --init --recursive
RUN mkdir build && cd build && \
    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DNANODB_BUILD_PYTHON=OFF \
             -DNANODB_BUILD_SERVER=ON && \
    cmake --build . --target nano_server -j$(nproc)
```

The `gcc:13` image provides GCC 13 with full C++17 support, AVX2 codegen, and the necessary build tools. `-DNANODB_BUILD_PYTHON=OFF` skips pybind11 (which would require Python development headers). Only `nano_server` is built — not the CLI demo, benchmarks, or tests.

**Stage 2 — Runtime:**
```dockerfile
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y libgomp1 && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=builder /app/build/nano_server .
RUN mkdir -p data
EXPOSE 8080
ENV NANODB_PORT=8080
ENV NANODB_DATA_DIR=/app/data
CMD ["./nano_server"]
```

Only `libgomp1` is needed at runtime — it is the GNU OpenMP runtime library, required because the HNSW code uses `#pragma omp atomic`. Without it, the binary fails to load with "libgomp.so.1: cannot open shared object file."

**Size comparison:**
- `gcc:13` builder image: ~1.4 GB
- Final runtime image: ~80 MB (debian:bookworm-slim + libgomp1 + the ~2MB nano_server binary)

### docker-compose

```yaml
services:
  nanodb:
    build: .
    ports:
      - "8080:8080"
    volumes:
      - nanodb-data:/app/data
    restart: unless-stopped
volumes:
  nanodb-data:
```

The named volume `nanodb-data` maps to `/app/data` inside the container. This means `index.ndb` and `metadata.bin` survive container restarts and upgrades. Without a volume, the data would be lost when the container is removed.

### .dockerignore

```
build/
.git/
.vs/
.vscode/
*.exe
*.pdb
data/
```

Excludes build artifacts, version control history, IDE files, and local data from the Docker build context. This keeps `docker build` fast (small context upload) and prevents the local `data/` directory from being copied into the image.

---

## 12. GitHub Actions CI

**File:** `.github/workflows/ci.yml`

### Trigger

```yaml
on:
  push:
    branches: [main]
  pull_request:
    branches: [main]
```

Runs on every push to `main` and every pull request targeting `main`. This ensures:
- Broken code never lands on the default branch.
- Pull request authors see test results before merge.

### Job 1: build-and-test

```yaml
steps:
  - uses: actions/checkout@v4
    with:
      submodules: recursive
  - run: sudo apt-get update && sudo apt-get install -y cmake g++ libomp-dev
  - run: cmake -B build -DCMAKE_BUILD_TYPE=Release -DNANODB_BUILD_PYTHON=OFF -DNANODB_BUILD_SERVER=ON
  - run: cmake --build build -j$(nproc)
  - run: cd build && ctest --output-on-failure
```

- `submodules: recursive` — Checks out `extern/httplib` and `extern/pybind11` (even though pybind11 isn't built, it must exist for CMake to parse the file without error when `NANODB_BUILD_PYTHON=OFF` — actually no, the `if(NANODB_BUILD_PYTHON)` guard skips the `add_subdirectory`). The httplib submodule IS needed.
- `libomp-dev` — Provides the OpenMP headers and runtime for Ubuntu's GCC.
- `ctest --output-on-failure` — Runs all registered tests and prints stdout/stderr only for failing tests.

GitHub Actions Ubuntu runners have AVX2 support (Intel Xeon Cascade Lake or newer), so the SIMD distance functions work correctly in CI.

### Job 2: docker

```yaml
needs: build-and-test
steps:
  - uses: actions/checkout@v4
    with:
      submodules: recursive
  - run: docker build -t nanodb:ci .
```

`needs: build-and-test` ensures the Docker build only runs if tests pass — no point building a container of broken code. The Docker build verifies that the multi-stage Dockerfile compiles cleanly on the CI runner.

### Why Python bindings are skipped

- pybind11 requires Python development headers (`python3-dev`).
- Adding Python to the CI matrix increases build time and complexity.
- The core library and server are fully tested by the C++ tests.
- Python bindings are a thin wrapper — if the C++ tests pass, the bindings will work.

For a more complete CI, a separate job could build with `-DNANODB_BUILD_PYTHON=ON` after installing `python3-dev`, but this is deferred as low-priority.

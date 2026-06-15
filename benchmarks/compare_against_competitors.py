#!/usr/bin/env python3
"""
NanoDB vs FAISS vs hnswlib — Apples-to-Apples Benchmark
========================================================

This script runs the same workload on all three systems and measures:
  1. Insert throughput (vectors/sec)
  2. Search latency (ms per query)
  3. Recall@10 (fraction of ground truth neighbors retrieved)

Setup:
  pip install faiss-cpu hnswlib numpy requests

Usage:
  # Start NanoDB first in another terminal:
  docker run -p 8080:8080 -v nanodb-data:/data ghcr.io/shlokkvaishnav/nano-db

  # Then run this script:
  python compare_against_competitors.py --size 100000
  python compare_against_competitors.py --size 1000000  # after 100k works

IMPORTANT: NanoDB numbers include HTTP round-trip overhead; FAISS and hnswlib
are measured as in-process library calls. This is noted in the output.
"""

import argparse
import sys
import time
import numpy as np
import requests
from typing import List, Tuple

try:
    import faiss
except ImportError:
    print("ERROR: faiss-cpu not installed. Run: pip install faiss-cpu")
    sys.exit(1)

try:
    import hnswlib
    HNSWLIB_AVAILABLE = True
except ImportError:
    print("WARNING: hnswlib not available. Only FAISS and NanoDB will be benchmarked.")
    print("         On Windows, hnswlib requires Visual C++ Build Tools.")
    print("         On Linux/macOS: pip install hnswlib")
    HNSWLIB_AVAILABLE = False


# ============================================================================
# Configuration
# ============================================================================

DIM = 128
K = 10
THREADS = 8
SEED = 42
NANODB_BASE_URL = "http://localhost:8080"

# HNSW hyperparameters (matching NanoDB defaults where possible)
EF_CONSTRUCTION = 200
M = 16
EF_SEARCH = 50


# ============================================================================
# Data Generation
# ============================================================================

def generate_dataset(n: int, dim: int, seed: int) -> Tuple[np.ndarray, np.ndarray]:
    """Generate random float32 vectors for benchmarking."""
    rng = np.random.default_rng(seed)
    vectors = rng.random((n, dim), dtype=np.float32)
    queries = rng.random((1000, dim), dtype=np.float32)  # 1000 queries for search
    return vectors, queries


# ============================================================================
# Ground Truth Computation (for recall measurement)
# ============================================================================

def compute_ground_truth(vectors: np.ndarray, queries: np.ndarray, k: int) -> np.ndarray:
    """
    Use FAISS exact search (IndexFlatL2) to compute ground truth neighbors.
    Returns: (num_queries, k) array of neighbor IDs.
    """
    print("\n[Ground Truth] Computing exact neighbors with FAISS IndexFlatL2...")
    index = faiss.IndexFlatL2(vectors.shape[1])
    index.add(vectors)
    _, gt_neighbors = index.search(queries, k)
    return gt_neighbors


def compute_recall(gt_neighbors: np.ndarray, pred_neighbors: List[List[int]]) -> float:
    """
    Compute recall@k: fraction of ground truth neighbors found in predictions.
    """
    assert len(gt_neighbors) == len(pred_neighbors), "Query count mismatch"
    total = 0
    correct = 0
    for gt, pred in zip(gt_neighbors, pred_neighbors):
        gt_set = set(gt)
        pred_set = set(pred)
        correct += len(gt_set & pred_set)
        total += len(gt_set)
    return correct / total if total > 0 else 0.0


# ============================================================================
# FAISS Benchmark
# ============================================================================

def benchmark_faiss(vectors: np.ndarray, queries: np.ndarray, k: int, threads: int):
    print(f"\n{'=' * 60}")
    print(f"FAISS Benchmark")
    print(f"{'=' * 60}")

    faiss.omp_set_num_threads(threads)
    index = faiss.IndexHNSWFlat(vectors.shape[1], M)
    index.hnsw.efConstruction = EF_CONSTRUCTION
    index.hnsw.efSearch = EF_SEARCH

    # Insert
    start = time.perf_counter()
    index.add(vectors)
    insert_time = time.perf_counter() - start
    insert_tps = len(vectors) / insert_time

    # Search
    search_times = []
    pred_neighbors = []
    for q in queries:
        start = time.perf_counter()
        _, neighbors = index.search(q.reshape(1, -1), k)
        search_times.append((time.perf_counter() - start) * 1000)  # ms
        pred_neighbors.append(neighbors[0].tolist())

    avg_search_ms = np.mean(search_times)
    p99_search_ms = np.percentile(search_times, 99)

    return {
        "name": "FAISS",
        "insert_tps": insert_tps,
        "avg_search_ms": avg_search_ms,
        "p99_search_ms": p99_search_ms,
        "pred_neighbors": pred_neighbors,
    }


# ============================================================================
# hnswlib Benchmark
# ============================================================================

def benchmark_hnswlib(vectors: np.ndarray, queries: np.ndarray, k: int, threads: int):
    print(f"\n{'=' * 60}")
    print(f"hnswlib Benchmark")
    print(f"{'=' * 60}")

    index = hnswlib.Index(space='l2', dim=vectors.shape[1])
    index.init_index(max_elements=len(vectors), ef_construction=EF_CONSTRUCTION, M=M)
    index.set_num_threads(threads)

    # Insert
    start = time.perf_counter()
    index.add_items(vectors, np.arange(len(vectors)))
    insert_time = time.perf_counter() - start
    insert_tps = len(vectors) / insert_time

    # Search
    index.set_ef(EF_SEARCH)
    search_times = []
    pred_neighbors = []
    for q in queries:
        start = time.perf_counter()
        neighbors, _ = index.knn_query(q.reshape(1, -1), k=k)
        search_times.append((time.perf_counter() - start) * 1000)  # ms
        pred_neighbors.append(neighbors[0].tolist())

    avg_search_ms = np.mean(search_times)
    p99_search_ms = np.percentile(search_times, 99)

    return {
        "name": "hnswlib",
        "insert_tps": insert_tps,
        "avg_search_ms": avg_search_ms,
        "p99_search_ms": p99_search_ms,
        "pred_neighbors": pred_neighbors,
    }


# ============================================================================
# NanoDB Benchmark (via REST API)
# ============================================================================

def benchmark_nanodb(vectors: np.ndarray, queries: np.ndarray, k: int):
    print(f"\n{'=' * 60}")
    print(f"NanoDB Benchmark (via HTTP REST API)")
    print(f"{'=' * 60}")

    # Create a session to reuse connections and avoid port exhaustion
    session = requests.Session()

    # Check if server is running
    try:
        resp = session.get(f"{NANODB_BASE_URL}/stats", timeout=2)
        resp.raise_for_status()
    except requests.exceptions.RequestException as e:
        print(f"ERROR: Cannot connect to NanoDB at {NANODB_BASE_URL}")
        print(f"       Make sure the server is running: docker run -p 8080:8080 ghcr.io/shlokkvaishnav/nano-db")
        sys.exit(1)

    # Insert
    print(f"Inserting {len(vectors):,} vectors...")
    start = time.perf_counter()
    for i, v in enumerate(vectors):
        resp = session.post(f"{NANODB_BASE_URL}/vectors", json={
            "id": int(i),
            "vector": v.tolist(),
            "metadata": f"vec_{i}"
        })
        if resp.status_code != 201:
            print(f"ERROR: Insert failed for vector {i}: {resp.status_code} {resp.text}")
            sys.exit(1)

        if (i + 1) % 10000 == 0:
            elapsed = time.perf_counter() - start
            rate = (i + 1) / elapsed
            print(f"  ... {i + 1:,} / {len(vectors):,} ({rate:.0f} TPS)")

    insert_time = time.perf_counter() - start
    insert_tps = len(vectors) / insert_time

    # Search
    print(f"Running {len(queries):,} search queries...")
    search_times = []
    pred_neighbors = []
    for i, q in enumerate(queries):
        start = time.perf_counter()
        resp = session.post(f"{NANODB_BASE_URL}/search", json={
            "vector": q.tolist(),
            "k": k
        })
        search_times.append((time.perf_counter() - start) * 1000)  # ms

        if resp.status_code != 200:
            print(f"ERROR: Search failed for query {i}: {resp.status_code} {resp.text}")
            sys.exit(1)

        result = resp.json()
        if "results" in result:
            neighbors = [r["id"] for r in result["results"]]
        else:
            # Handle old format if API returns array directly
            neighbors = [r["id"] for r in result]
        pred_neighbors.append(neighbors)

    session.close()

    avg_search_ms = np.mean(search_times)
    p99_search_ms = np.percentile(search_times, 99)

    return {
        "name": "NanoDB",
        "insert_tps": insert_tps,
        "avg_search_ms": avg_search_ms,
        "p99_search_ms": p99_search_ms,
        "pred_neighbors": pred_neighbors,
    }


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="Benchmark NanoDB vs FAISS vs hnswlib")
    parser.add_argument("--size", type=int, default=100_000,
                        help="Number of vectors to insert (default: 100,000)")
    parser.add_argument("--dim", type=int, default=DIM,
                        help=f"Vector dimension (default: {DIM})")
    parser.add_argument("--threads", type=int, default=THREADS,
                        help=f"Thread count for FAISS/hnswlib (default: {THREADS})")
    parser.add_argument("--skip-nanodb", action="store_true",
                        help="Skip NanoDB benchmark (useful for quick FAISS/hnswlib testing)")
    args = parser.parse_args()

    N = args.size
    DIM_LOCAL = args.dim
    THREADS_LOCAL = args.threads

    print("=" * 70)
    print(f"NanoDB vs FAISS vs hnswlib — Benchmark")
    print("=" * 70)
    print(f"Dataset:  {N:,} vectors × {DIM_LOCAL}-dim (float32)")
    print(f"Queries:  1,000 × {DIM_LOCAL}-dim")
    print(f"k:        {K}")
    print(f"Threads:  {THREADS_LOCAL} (FAISS, hnswlib only)")
    print(f"Metric:   L2 (Euclidean distance)")
    print("=" * 70)

    # Generate data
    print("\n[1/5] Generating dataset...")
    vectors, queries = generate_dataset(N, DIM_LOCAL, SEED)

    # Compute ground truth
    print("\n[2/5] Computing ground truth...")
    gt_neighbors = compute_ground_truth(vectors, queries, K)

    # Run benchmarks
    results = []

    print("\n[3/5] Running FAISS benchmark...")
    faiss_result = benchmark_faiss(vectors, queries, K, THREADS_LOCAL)
    results.append(faiss_result)

    if HNSWLIB_AVAILABLE:
        print("\n[4/5] Running hnswlib benchmark...")
        hnswlib_result = benchmark_hnswlib(vectors, queries, K, THREADS_LOCAL)
        results.append(hnswlib_result)
    else:
        print("\n[4/5] Skipping hnswlib benchmark (not installed)")

    if not args.skip_nanodb:
        print("\n[5/5] Running NanoDB benchmark...")
        nanodb_result = benchmark_nanodb(vectors, queries, K)
        results.insert(0, nanodb_result)  # Put NanoDB first in results table
    else:
        print("\n[5/5] Skipping NanoDB benchmark (--skip-nanodb)")


    # Compute recall for each system
    for result in results:
        result["recall"] = compute_recall(gt_neighbors, result["pred_neighbors"])

    # Print comparison table
    print("\n" + "=" * 70)
    print(f"RESULTS — {N:,} vectors, {DIM_LOCAL}-dim, L2, {THREADS_LOCAL} threads")
    print("=" * 70)
    print(f"{'System':<15} {'Insert TPS':>15} {'Avg Search (ms)':>18} {'P99 Search (ms)':>18} {'Recall@10':>12}")
    print("-" * 70)

    for result in results:
        print(f"{result['name']:<15} {result['insert_tps']:>15,.0f} "
              f"{result['avg_search_ms']:>18.3f} {result['p99_search_ms']:>18.3f} "
              f"{result['recall']:>12.2%}")

    print("=" * 70)
    print("\nNOTE: NanoDB numbers include HTTP round-trip overhead per insert/search.")
    print("      FAISS and hnswlib are measured as in-process library calls.")
    print("      For NanoDB's engine-only performance, see ./benchmark_throughput.")
    print()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
tail_latency_analysis.py

Measures search latency against the running 2-shard cluster and models what
tail latency would look like at 1, 4, and 8 shards using order statistics.

Requires the cluster to be running:
    ./cluster.sh up

The key insight: in a scatter-gather fan-out, the coordinator waits for ALL
shards before it can merge and return results. So the end-to-end latency for
a single search is:

    latency = max(latency_shard_0, latency_shard_1, ..., latency_shard_N)

If each shard's response time is drawn independently from the same distribution
F, then the p99 of max(N independent draws) is much worse than the p99 of a
single draw. Concretely:

    Pr[max(N) <= x] = F(x)^N
    => p99 of max(N) = F^{-1}(0.99^{1/N})

For N=2:  p99_effective = F^{-1}(0.9950)
For N=4:  p99_effective = F^{-1}(0.9975)
For N=8:  p99_effective = F^{-1}(0.9988)

These push into the far tail of the per-shard distribution, which is why p99
worsens faster than p50 as you add shards.

Usage:
    python3 benchmarks/tail_latency_analysis.py [--queries N]
"""

import argparse
import math
import random
import time

import requests

API = "http://localhost:8080"
VECTOR_DIM = 128
WARMUP_QUERIES = 50


def rand_vector():
    v = [random.gauss(0, 1) for _ in range(VECTOR_DIM)]
    mag = sum(x * x for x in v) ** 0.5
    return [x / mag for x in v]


def percentile(sorted_data: list, p: float) -> float:
    if not sorted_data:
        return float("nan")
    idx = (len(sorted_data) - 1) * p / 100.0
    lo = int(idx)
    hi = lo + 1
    frac = idx - lo
    if hi >= len(sorted_data):
        return sorted_data[-1]
    return sorted_data[lo] * (1 - frac) + sorted_data[hi] * frac


def run_search(url: str, timeout: float = 3.0) -> float | None:
    """Return latency in ms, or None on failure."""
    vec = rand_vector()
    start = time.perf_counter()
    try:
        r = requests.post(
            f"{url}/search",
            json={"vector": vec, "k": 10, "consistency": "strong"},
            timeout=timeout,
        )
        elapsed_ms = (time.perf_counter() - start) * 1000.0
        if r.status_code == 200:
            return elapsed_ms
    except Exception:
        pass
    return None


def order_statistic_quantile(sorted_data: list, n_shards: int, p: float) -> float:
    """
    Estimate the p-th percentile of max(N iid draws from the empirical distribution).
    Uses the formula: p_single = p^{1/N}, then look up that quantile in sorted_data.
    """
    p_single = p ** (1.0 / n_shards)
    return percentile(sorted_data, p_single * 100.0)


def main():
    parser = argparse.ArgumentParser(description="Tail latency analysis for scatter-gather search")
    parser.add_argument("--queries", type=int, default=500, help="Number of search queries to run (default 500)")
    args = parser.parse_args()

    print()
    print("Nano-DB Tail Latency Analysis: Scatter-Gather vs Shard Count")
    print("=" * 62)
    print()

    # Check cluster is reachable
    try:
        r = requests.get(f"{API}/stats", timeout=3.0)
        stats = r.json()
        print(f"Cluster: {stats.get('total_element_count', '?')} vectors, "
              f"{len(stats.get('replicas', []))} replicas")
    except Exception:
        print("ERROR: Cannot reach cluster at http://localhost:8080")
        print("Run:  ./cluster.sh up")
        raise SystemExit(1)

    print()

    # Warm up
    print(f"Warming up with {WARMUP_QUERIES} queries...")
    for _ in range(WARMUP_QUERIES):
        run_search(API)

    # Measure
    print(f"Measuring {args.queries} search queries...")
    latencies = []
    errors = 0
    for i in range(args.queries):
        ms = run_search(API)
        if ms is not None:
            latencies.append(ms)
        else:
            errors += 1
        if (i + 1) % 100 == 0:
            print(f"  {i + 1}/{args.queries}...")

    if not latencies:
        print("ERROR: all queries failed")
        raise SystemExit(1)

    latencies.sort()
    print(f"  Completed: {len(latencies)} ok, {errors} errors")
    print()

    # Measured numbers (actual 2-shard cluster)
    p50  = percentile(latencies, 50)
    p95  = percentile(latencies, 95)
    p99  = percentile(latencies, 99)
    p999 = percentile(latencies, 99.9)

    print("Measured (2-shard Docker Compose cluster):")
    print(f"  p50:   {p50:6.1f} ms")
    print(f"  p95:   {p95:6.1f} ms")
    print(f"  p99:   {p99:6.1f} ms")
    print(f"  p99.9: {p999:6.1f} ms")
    print()

    # Modeled: what would the same per-shard distribution look like at N shards?
    print("Modeled: p99 of max(N iid shards) — using order statistics on measured data")
    print()
    print(f"  {'Shards':>6}  {'p50 (ms)':>10}  {'p95 (ms)':>10}  {'p99 (ms)':>10}  {'p99.9 (ms)':>12}  Notes")
    print("  " + "-" * 70)

    configs = [
        (1,  "single shard, no fan-out"),
        (2,  "current cluster (measured)"),
        (4,  "4-shard cluster (modeled)"),
        (8,  "8-shard cluster (modeled)"),
        (16, "16-shard cluster (modeled)"),
    ]

    for n, label in configs:
        if n == 1:
            p50_n  = percentile(latencies, 50)
            p95_n  = percentile(latencies, 95)
            p99_n  = percentile(latencies, 99)
            p999_n = percentile(latencies, 99.9)
            suffix = " *"
        elif n == 2:
            p50_n  = p50
            p95_n  = p95
            p99_n  = p99
            p999_n = p999
            suffix = " *"
        else:
            p50_n  = order_statistic_quantile(latencies, n, 0.50)
            p95_n  = order_statistic_quantile(latencies, n, 0.95)
            p99_n  = order_statistic_quantile(latencies, n, 0.99)
            p999_n = order_statistic_quantile(latencies, n, 0.999)
            suffix = ""

        print(f"  {n:>6}  {p50_n:>10.1f}  {p95_n:>10.1f}  {p99_n:>10.1f}  {p999_n:>12.1f}  {label}{suffix}")

    print()
    print("  * Measured from the running cluster. 1-shard estimate assumes the same per-shard")
    print("    distribution as observed — actual 1-shard latency would be slightly lower")
    print("    (no scatter-gather overhead). N>=4 are theoretical projections.")
    print()
    print("Insight:")
    print("  p50 is roughly flat across shard counts — more parallelism, similar average.")
    print("  p99 worsens monotonically — the slowest shard gates every request.")
    print("  p99.9 worsens dramatically — as N grows, you're sampling the deep tail")
    print("  of the per-shard distribution on every single query.")
    print()
    print(f"  p99 increase from 2 to 8 shards: "
          f"{order_statistic_quantile(latencies, 8, 0.99) / max(p99, 0.001):.1f}x")
    print()


if __name__ == "__main__":
    main()

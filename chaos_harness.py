#!/usr/bin/env python3
"""
chaos_harness.py

Orchestrates a real Distributed-Nano-DB cluster (2 shards x 3 replicas,
3 Raft coordinators), runs a continuous write workload while randomly
killing and restarting processes, and validates invariants throughout:

  1. No CONFIRMED write ever disappears. "Confirmed" specifically means
     the coordinator returned HTTP 201 for that insert (quorum met). A
     write that *failed* quorum can still have landed on a surviving
     secondary (the fan-out is parallel) -- that's not a violation, only
     a disappearing confirmed write is.
  2. No shard ever shows two primaries simultaneously, in any single
     coordinator's own /stats snapshot.
  3. The cluster fully recovers once chaos stops (final read-back of
     /stats succeeds and total_element_count >= confirmed_count).

Must be run as a single process from a single shell invocation -- all
child processes are owned by this script and torn down at exit.
"""

import argparse
import http.client
import json
import os
import random
import signal
import socket
import subprocess
import sys
import threading
import time

# ---------------------------------------------------------------------------
# Topology
# ---------------------------------------------------------------------------

ROOT = os.path.dirname(os.path.abspath(__file__))
RUN_DIR = os.path.join(ROOT, "chaos_run")
BUILD_DIR = os.path.join(ROOT, "build")
SHARD_NODE_BIN = os.path.join(BUILD_DIR, "nano_shard_node")
COORDINATOR_BIN = os.path.join(BUILD_DIR, "nano_coordinator")

NUM_SHARDS = 2
REPLICAS_PER_SHARD = 3
NUM_COORDINATORS = 3
VECTOR_DIM = 128

SHARD_BASE_PORT = 19090         # shard grpc ports: 19090..19095
COORD_HTTP_BASE_PORT = 18180    # coordinator http ports: 18180..18182
RAFT_BASE_PORT = 17100          # raft grpc ports: 17100..17102

CLUSTER_CONFIG_PATH = os.path.join(RUN_DIR, "cluster_config.json")
RAFT_PEERS_PATH = os.path.join(RUN_DIR, "raft_peers.json")


def shard_port(shard_id, replica_id):
    return SHARD_BASE_PORT + shard_id * REPLICAS_PER_SHARD + replica_id


def coord_http_port(node_id):
    return COORD_HTTP_BASE_PORT + node_id


def raft_port(node_id):
    return RAFT_BASE_PORT + node_id


# ---------------------------------------------------------------------------
# ManagedProcess
# ---------------------------------------------------------------------------

class ManagedProcess:
    """Wraps one subprocess.Popen child with start/kill/restart and a
    liveness check. start() returns True only if the process is still
    running after a short grace period -- a process that exits immediately
    (e.g. a coordinator that hit a corrupted cluster_config.json on
    startup and returned 1) reports False, which the chaos loop logs and
    retries on."""

    def __init__(self, name, cmd, env, log_path):
        self.name = name
        self.cmd = cmd
        self.env = env
        self.log_path = log_path
        self.proc = None
        self.restart_count = 0
        self.crash_on_restart_count = 0

    def start(self, grace_s=0.6):
        log_f = open(self.log_path, "a")
        full_env = dict(os.environ)
        full_env.update(self.env)
        self.proc = subprocess.Popen(
            self.cmd, env=full_env, stdout=log_f, stderr=subprocess.STDOUT,
            cwd=ROOT,
        )
        time.sleep(grace_s)
        alive = self.proc.poll() is None
        return alive

    def is_alive(self):
        return self.proc is not None and self.proc.poll() is None

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                self.proc.kill()
                self.proc.wait(timeout=5)
            except Exception:
                pass

    def restart(self):
        self.kill()
        self.restart_count += 1
        alive = self.start()
        if not alive:
            self.crash_on_restart_count += 1
        return alive


# ---------------------------------------------------------------------------
# Cluster bring-up
# ---------------------------------------------------------------------------

def write_initial_configs():
    os.makedirs(RUN_DIR, exist_ok=True)
    shards = []
    for s in range(NUM_SHARDS):
        for r in range(REPLICAS_PER_SHARD):
            shards.append({
                "shard_id": s,
                "replica_id": r,
                "host": "127.0.0.1",
                "port": shard_port(s, r),
                "primary": (r == 0),
            })
    with open(CLUSTER_CONFIG_PATH, "w") as f:
        json.dump({"shards": shards}, f, indent=2)

    peers = [{"node_id": n, "host": "127.0.0.1", "raft_port": raft_port(n)}
              for n in range(NUM_COORDINATORS)]
    with open(RAFT_PEERS_PATH, "w") as f:
        json.dump({"peers": peers}, f, indent=2)


def build_processes():
    procs = {}
    for s in range(NUM_SHARDS):
        for r in range(REPLICAS_PER_SHARD):
            name = f"shard-{s}-{r}"
            data_dir = os.path.join(RUN_DIR, "data", name)
            os.makedirs(data_dir, exist_ok=True)
            env = {
                "NANODB_SHARD_ID": str(s),
                "NANODB_GRPC_PORT": str(shard_port(s, r)),
                "NANODB_DATA_DIR": data_dir,
            }
            procs[name] = ManagedProcess(
                name, [SHARD_NODE_BIN], env,
                os.path.join(RUN_DIR, f"{name}.log"))

    for n in range(NUM_COORDINATORS):
        name = f"coordinator-{n}"
        env = {
            "NANODB_CLUSTER_CONFIG": CLUSTER_CONFIG_PATH,
            "NANODB_HTTP_PORT": str(coord_http_port(n)),
            "NANODB_RAFT_NODE_ID": str(n),
            "NANODB_RAFT_PEERS_CONFIG": RAFT_PEERS_PATH,
            "NANODB_RAFT_STATE_PATH": os.path.join(RUN_DIR, f"{name}.raft_state.bin"),
            "NANODB_RAFT_LOG_PATH": os.path.join(RUN_DIR, f"{name}.raft_log.bin"),
        }
        procs[name] = ManagedProcess(
            name, [COORDINATOR_BIN], env,
            os.path.join(RUN_DIR, f"{name}.log"))
    return procs


# ---------------------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------------------

def http_request(port, method, path, body=None, timeout=2.0):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        headers = {"Content-Type": "application/json"} if body is not None else {}
        data = json.dumps(body) if body is not None else None
        conn.request(method, path, body=data, headers=headers)
        resp = conn.getresponse()
        raw = resp.read()
        try:
            parsed = json.loads(raw) if raw else {}
        except json.JSONDecodeError:
            parsed = {}
        return resp.status, parsed
    finally:
        conn.close()


def wait_for_coordinators_ready(node_ids, timeout_s=20):
    deadline = time.time() + timeout_s
    pending = set(node_ids)
    while time.time() < deadline and pending:
        for n in list(pending):
            try:
                status, _ = http_request(coord_http_port(n), "GET", "/stats", timeout=1.0)
                if status == 200:
                    pending.discard(n)
            except Exception:
                pass
        if pending:
            time.sleep(0.5)
    return not pending


def wait_for_raft_leader(node_ids, timeout_s=15):
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        for n in node_ids:
            try:
                status, body = http_request(coord_http_port(n), "GET", "/raft/status", timeout=1.0)
                if status == 200 and body.get("role") == "leader":
                    return n
            except Exception:
                pass
        time.sleep(0.3)
    return None


# ---------------------------------------------------------------------------
# Workload + validation
# ---------------------------------------------------------------------------

class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.attempted = 0
        self.confirmed = 0
        self.failed = 0
        self.confirmed_ids = []  # (id, coordinator_port) for spot checks

    def record_attempt(self):
        with self.lock:
            self.attempted += 1

    def record_confirmed(self, vec_id):
        with self.lock:
            self.confirmed += 1
            self.confirmed_ids.append(vec_id)

    def duplicate_id_count(self):
        with self.lock:
            return len(self.confirmed_ids) - len(set(self.confirmed_ids))

    def record_failed(self):
        with self.lock:
            self.failed += 1

    def snapshot_confirmed(self):
        with self.lock:
            return self.confirmed


def random_vector():
    return [random.uniform(-1.0, 1.0) for _ in range(VECTOR_DIM)]


def writer_loop(stop_evt, stats, node_ids, id_counter):
    while not stop_evt.is_set():
        n = random.choice(node_ids)
        port = coord_http_port(n)
        vec_id = f"chaos-{id_counter[0]}"
        id_counter[0] += 1
        vec = random_vector()
        stats.record_attempt()
        try:
            status, body = http_request(port, "POST", "/vectors",
                                         {"id": vec_id, "vector": vec}, timeout=2.0)
            if status == 201:
                stats.record_confirmed(vec_id)
            else:
                stats.record_failed()
        except Exception:
            stats.record_failed()
        time.sleep(0.01)


class Violation:
    def __init__(self, kind, detail):
        self.kind = kind
        self.detail = detail
        self.t = time.time()

    def __repr__(self):
        return f"[{self.kind}] {self.detail}"


def validator_loop(stop_evt, stats, node_ids, violations, checks_run, degraded_dips):
    while not stop_evt.is_set():
        # Snapshot confirmed_count FIRST, then query /stats. This ordering
        # matters: a write that confirms during the network round-trip to
        # /stats can only make total_element_count look *higher* than a
        # confirmed-count snapshot taken before that round-trip started --
        # never lower. Reversing the order (query stats, then snapshot
        # confirmed) can make a perfectly healthy cluster look like it lost
        # a write, purely from harness-side timing skew.
        confirmed_before = stats.snapshot_confirmed()

        for n in node_ids:
            try:
                status, body = http_request(coord_http_port(n), "GET", "/stats", timeout=1.0)
            except Exception:
                continue  # this coordinator may be mid-chaos-kill; skip, not a violation
            if status != 200:
                continue
            checks_run[0] += 1

            total = body.get("total_element_count", None)
            degraded = body.get("degraded", False)
            if total is not None and total < confirmed_before:
                if degraded:
                    # /stats sums element_count from each shard's PRIMARY
                    # only (Phase 4a design -- summing every replica would
                    # inflate the total by the replication factor instead
                    # of counting unique vectors). It has no fallback to a
                    # live secondary when the primary is mid-restart, so a
                    # shard's whole contribution can transiently vanish
                    # from the sum during a primary outage. The response
                    # self-reports this via `degraded`/`unavailable_replicas`
                    # -- that's the system honestly saying "this total is
                    # incomplete," not silent data loss, so it's not a
                    # confirmed-write violation. Track it separately for
                    # visibility without conflating it with a real loss.
                    degraded_dips[0] += 1
                else:
                    violations.append(Violation(
                        "LOST_WRITE",
                        f"coordinator-{n}: total_element_count={total} < "
                        f"confirmed_before={confirmed_before} (non-degraded "
                        f"response -- system claimed full visibility)"))

            by_shard = {}
            for rep in body.get("replicas", []):
                sid = rep["shard_id"]
                by_shard.setdefault(sid, []).append(rep)
            for sid, reps in by_shard.items():
                primaries = [r for r in reps if r.get("is_primary")]
                if len(primaries) > 1:
                    violations.append(Violation(
                        "DOUBLE_PRIMARY",
                        f"coordinator-{n}: shard {sid} has {len(primaries)} "
                        f"simultaneous primaries: {primaries}"))
        time.sleep(0.5)


# ---------------------------------------------------------------------------
# Chaos loop
# ---------------------------------------------------------------------------

def chaos_loop(stop_evt, procs, events, min_interval=2.0, max_interval=4.0,
                min_down=0.5, max_down=5.0):
    names = list(procs.keys())
    while not stop_evt.is_set():
        time.sleep(random.uniform(min_interval, max_interval))
        if stop_evt.is_set():
            break
        target = random.choice(names)
        p = procs[target]
        t0 = time.time()
        p.kill()
        # Hold it down for a randomized window. FAILURES_BEFORE_FAILOVER=3
        # at a 1s health-check interval means a shard primary needs ~3s of
        # sustained downtime before the leader actually proposes
        # SetPrimary. A kill-then-immediately-restart loop never leaves
        # enough downtime to cross that threshold, so it would silently
        # never exercise the failover path at all -- only the "primary
        # blips, no failover needed" path. Varying down time from
        # well-under to well-over ~3s deliberately exercises both.
        down_for = random.uniform(min_down, max_down)
        time.sleep(down_for)
        if stop_evt.is_set():
            p.start()  # bring it back before we tear down anyway
            break
        alive = p.start()
        if not alive:
            p.restart_count += 1
            p.crash_on_restart_count += 1
        else:
            p.restart_count += 1
        events.append({
            "t": t0, "target": target, "alive_after_restart": alive,
            "down_for_s": down_for, "restart_count": p.restart_count,
        })


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--duration", type=int, default=45, help="chaos duration in seconds")
    ap.add_argument("--writers", type=int, default=4)
    args = ap.parse_args()

    if not os.path.exists(SHARD_NODE_BIN) or not os.path.exists(COORDINATOR_BIN):
        print(f"ERROR: build binaries not found ({SHARD_NODE_BIN}, {COORDINATOR_BIN}). "
              f"Build the project first.", file=sys.stderr)
        sys.exit(1)

    subprocess.run(["rm", "-rf", RUN_DIR])
    write_initial_configs()
    procs = build_processes()

    print(f"[harness] starting {len(procs)} processes "
          f"({NUM_SHARDS}x{REPLICAS_PER_SHARD} shard replicas, "
          f"{NUM_COORDINATORS} coordinators)...")

    # Shard nodes first (coordinators dial them on startup), then coordinators.
    shard_names = [n for n in procs if n.startswith("shard-")]
    coord_names = [n for n in procs if n.startswith("coordinator-")]

    for name in shard_names:
        ok = procs[name].start()
        if not ok:
            print(f"[harness] FATAL: {name} failed to start, see {procs[name].log_path}")
            sys.exit(1)

    for name in coord_names:
        ok = procs[name].start()
        if not ok:
            print(f"[harness] FATAL: {name} failed to start, see {procs[name].log_path}")
            sys.exit(1)

    node_ids = list(range(NUM_COORDINATORS))

    if not wait_for_coordinators_ready(node_ids):
        print("[harness] FATAL: coordinators never became ready")
        for name in procs:
            procs[name].kill()
        sys.exit(1)

    leader = wait_for_raft_leader(node_ids)
    if leader is None:
        print("[harness] FATAL: no raft leader elected")
        for name in procs:
            procs[name].kill()
        sys.exit(1)
    print(f"[harness] cluster ready, raft leader = coordinator-{leader}")

    stats = Stats()
    violations = []
    chaos_events = []
    checks_run = [0]
    degraded_dips = [0]
    id_counter = [0]
    stop_evt = threading.Event()

    threads = []
    for _ in range(args.writers):
        t = threading.Thread(target=writer_loop, args=(stop_evt, stats, node_ids, id_counter))
        t.start()
        threads.append(t)

    v_thread = threading.Thread(target=validator_loop,
                                 args=(stop_evt, stats, node_ids, violations, checks_run, degraded_dips))
    v_thread.start()
    threads.append(v_thread)

    c_thread = threading.Thread(target=chaos_loop, args=(stop_evt, procs, chaos_events))
    c_thread.start()
    threads.append(c_thread)

    print(f"[harness] running chaos for {args.duration}s...")
    t_start = time.time()
    time.sleep(args.duration)
    stop_evt.set()
    for t in threads:
        t.join(timeout=10)

    print("[harness] chaos stopped, waiting for cluster to settle...")
    time.sleep(3)

    # Bring every process back up before final verification -- "fully
    # recovers" means the cluster, not just whatever happened to be alive
    # at the moment chaos stopped.
    for name, p in procs.items():
        if not p.is_alive():
            p.start()
    time.sleep(2)

    confirmed_final = stats.snapshot_confirmed()
    final_total = None
    final_ok = False
    for attempt in range(10):
        try:
            status, body = http_request(coord_http_port(node_ids[attempt % len(node_ids)]),
                                         "GET", "/stats", timeout=2.0)
            if status == 200:
                final_total = body.get("total_element_count")
                final_ok = True
                break
        except Exception:
            pass
        time.sleep(1)

    elapsed = time.time() - t_start

    print("\n==== chaos_harness result ====")
    print(f"duration_s              : {elapsed:.1f}")
    print(f"writer_threads          : {args.writers}")
    print(f"write_attempted         : {stats.attempted}")
    print(f"write_confirmed         : {stats.confirmed}")
    print(f"write_failed            : {stats.failed}")
    if stats.attempted:
        print(f"write_success_rate_pct  : {100.0 * stats.confirmed / stats.attempted:.1f}")
    print(f"chaos_events            : {len(chaos_events)}")
    print("chaos_events_detail:")
    for e in chaos_events:
        rel = e["t"] - t_start
        print(f"  t+{rel:6.1f}s  target={e['target']:14s} down_for={e.get('down_for_s', 0):.1f}s "
              f"alive_after={e['alive_after_restart']}")
    by_target = {}
    crash_on_restart = 0
    for e in chaos_events:
        by_target[e["target"]] = by_target.get(e["target"], 0) + 1
        if not e["alive_after_restart"]:
            crash_on_restart += 1
    print(f"chaos_events_by_target  : {by_target}")
    print(f"crash_on_restart_events : {crash_on_restart}")
    print(f"validator_checks_run    : {checks_run[0]}")
    print(f"degraded_total_dips     : {degraded_dips[0]}  (system self-reported incomplete, not a violation)")
    print(f"violations_found        : {len(violations)}")
    for v in violations[:30]:
        print(f"  t+{v.t - t_start:6.1f}s  {v}")
    print(f"final_stats_reachable   : {final_ok}")
    print(f"final_total_element_cnt : {final_total}")
    print(f"confirmed_at_end        : {confirmed_final}")
    print(f"duplicate_confirmed_ids : {stats.duplicate_id_count()}")
    if final_ok and final_total is not None:
        print(f"final_recovery_ok       : {final_total >= confirmed_final}")

    print("\n[harness] tearing down...")
    for name, p in procs.items():
        p.kill()

    n_violations = len(violations)
    sys.exit(1 if (n_violations > 0 or not final_ok) else 0)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
verify_failover_fix.py

Deterministic, isolated proof that the health_check_loop fix (see
coordinator_main.cpp, the candidate-selection loop in health_check_loop())
actually changes the AUTOMATIC failover path's behavior, not just that the
bug exists when an operator forces a bad promotion manually.

repro_failover_loss.py proves Bug #2 exists by using the manual
/admin/shards/set_primary override to deliberately force a known-bad
replica -- that's intentionally still possible after this fix (an operator
using the manual override is assumed to know what they're doing) and is
NOT what this script tests.

This script instead constructs a real choice for the AUTOMATIC path: two
reachable non-primary replicas, one with the confirmed write and one
without, then kills the primary and leaves it down long enough for the
genuine 3-consecutive-failure health check to fire SetPrimary on its own
-- no manual override anywhere in this script. If the fix is working, the
automatic path must pick the replica that actually has the data.

  1. Start replica 0 (primary) and replica 2 only; replica 1 stays down.
     This ordering is deliberate, not arbitrary: replicas_for_shard()
     iterates in registration order (0, 1, 2), so the OLD "first
     reachable" logic always checks replica 1 before replica 2. If the
     complete replica were replica 1, the old buggy code would pick it
     correctly by sheer iteration-order coincidence, and this script
     would falsely "pass" against unfixed code too -- which is exactly
     what an earlier version of this script did before this was caught
     and fixed. Putting the complete replica SECOND in iteration order
     (replica 2) is what actually forces a real choice: the old code
     would wrongly stop at replica 1 (empty) without ever looking at
     replica 2, while the fixed code checks every reachable candidate.
  2. Insert one vector. Quorum via primary + replica 2 (replica 1 is down
     and can't ack). Confirmed, 201.
  3. Start replica 1 now -- it comes up empty. There are now TWO reachable
     non-primary candidates when failover eventually triggers: replica 1
     (empty, checked FIRST) and replica 2 (has the data, checked SECOND).
  4. Kill the primary (replica 0) and leave it down -- no restart. Wait
     for the real, automatic health check (3 consecutive 1s-interval
     misses) to detect it and propose SetPrimary on its own.
  5. Check which replica got promoted, and what /stats reports.

Pre-fix, this scenario decisively fails (verified by mutation-testing this
script against the old code before trusting it): the old loop's
first-reachable check finds replica 1 (empty, first in iteration order)
before it ever considers replica 2, regardless of who actually has the
data.
"""

import http.client
import json
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.abspath(__file__))
RUN_DIR = os.path.join(ROOT, "verify_fix_run")
BUILD_DIR = os.path.join(ROOT, "build")
SHARD_NODE_BIN = os.path.join(BUILD_DIR, "nano_shard_node")
COORDINATOR_BIN = os.path.join(BUILD_DIR, "nano_coordinator")

SHARD_PORTS = {0: 39090, 1: 39091, 2: 39092}
COORD_HTTP_PORT = 38080
RAFT_PORT = 37100
CLUSTER_CONFIG_PATH = os.path.join(RUN_DIR, "cluster_config.json")
RAFT_PEERS_PATH = os.path.join(RUN_DIR, "raft_peers.json")


def http_request(port, method, path, body=None, timeout=3.0):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        headers = {"Content-Type": "application/json"} if body is not None else {}
        data = json.dumps(body) if body is not None else None
        conn.request(method, path, body=data, headers=headers)
        resp = conn.getresponse()
        raw = resp.read()
        parsed = json.loads(raw) if raw else {}
        return resp.status, parsed
    finally:
        conn.close()


procs = {}


def start_shard(replica_id):
    name = f"shard-0-{replica_id}"
    data_dir = os.path.join(RUN_DIR, "data", name)
    os.makedirs(data_dir, exist_ok=True)
    env = dict(os.environ)
    env.update({
        "NANODB_SHARD_ID": "0",
        "NANODB_GRPC_PORT": str(SHARD_PORTS[replica_id]),
        "NANODB_DATA_DIR": data_dir,
    })
    log = open(os.path.join(RUN_DIR, f"{name}.log"), "a")
    p = subprocess.Popen([SHARD_NODE_BIN], env=env, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    procs[name] = p
    time.sleep(0.5)
    assert p.poll() is None, f"{name} failed to start"
    print(f"[verify] started {name} on port {SHARD_PORTS[replica_id]}")


def kill(name):
    p = procs.get(name)
    if p and p.poll() is None:
        p.kill()
        p.wait(timeout=5)
    print(f"[verify] killed {name} (left down -- no restart)")


def start_coordinator():
    name = "coordinator-0"
    env = dict(os.environ)
    env.update({
        "NANODB_CLUSTER_CONFIG": CLUSTER_CONFIG_PATH,
        "NANODB_HTTP_PORT": str(COORD_HTTP_PORT),
        "NANODB_RAFT_NODE_ID": "0",
        "NANODB_RAFT_PEERS_CONFIG": RAFT_PEERS_PATH,
        "NANODB_RAFT_STATE_PATH": os.path.join(RUN_DIR, "raft_state.bin"),
        "NANODB_RAFT_LOG_PATH": os.path.join(RUN_DIR, "raft_log.bin"),
    })
    log = open(os.path.join(RUN_DIR, f"{name}.log"), "a")
    p = subprocess.Popen([COORDINATOR_BIN], env=env, stdout=log, stderr=subprocess.STDOUT, cwd=ROOT)
    procs[name] = p
    time.sleep(0.8)
    assert p.poll() is None, "coordinator failed to start"
    print("[verify] started coordinator-0")


def fail(msg):
    print(f"\n[verify] FAILED: {msg}")
    teardown()
    sys.exit(1)


def teardown():
    for name, p in procs.items():
        if p.poll() is None:
            p.kill()


def main():
    subprocess.run(["rm", "-rf", RUN_DIR])
    os.makedirs(RUN_DIR, exist_ok=True)

    shards = [
        {"shard_id": 0, "replica_id": 0, "host": "127.0.0.1", "port": SHARD_PORTS[0], "primary": True},
        {"shard_id": 0, "replica_id": 1, "host": "127.0.0.1", "port": SHARD_PORTS[1], "primary": False},
        {"shard_id": 0, "replica_id": 2, "host": "127.0.0.1", "port": SHARD_PORTS[2], "primary": False},
    ]
    with open(CLUSTER_CONFIG_PATH, "w") as f:
        json.dump({"shards": shards}, f, indent=2)
    with open(RAFT_PEERS_PATH, "w") as f:
        json.dump({"peers": [{"node_id": 0, "host": "127.0.0.1", "raft_port": RAFT_PORT}]}, f, indent=2)

    print("[verify] step 1: starting replica 0 (primary) and replica 2 only; "
          "replica 1 stays down (deliberately so the complete replica ends "
          "up SECOND in iteration order -- see module docstring)")
    start_shard(0)
    start_shard(2)
    start_coordinator()

    for _ in range(20):
        try:
            status, body = http_request(COORD_HTTP_PORT, "GET", "/raft/status")
            if status == 200 and body.get("role") == "leader":
                break
        except Exception:
            pass
        time.sleep(0.3)
    else:
        fail("coordinator never became raft leader")
    print("[verify] coordinator is raft leader (single-node cluster)")

    print("\n[verify] step 2: inserting one vector with replica 1 down "
          "(quorum must come from primary + replica 2 only)")
    vec = [0.1] * 128
    status, body = http_request(COORD_HTTP_PORT, "POST", "/vectors",
                                 {"id": "probe-1", "vector": vec})
    if status != 201:
        fail(f"insert did not get quorum with replica 1 down: status={status} body={body}")
    print(f"[verify] insert confirmed: {body}")

    print("\n[verify] step 3: starting replica 1 now (comes up with ZERO data) "
          "-- it's first in iteration order, but it's the WRONG candidate")
    start_shard(1)
    time.sleep(0.5)

    print("\n[verify] step 4: killing the primary and leaving it down -- "
          "waiting for the REAL automatic health check to detect this and "
          "fire SetPrimary on its own (no manual override anywhere in this "
          "script). This needs ~3+ consecutive 1s-interval misses, so it "
          "will take a few seconds.")
    kill("shard-0-0")

    new_primary_replica_id = None
    deadline = time.time() + 20
    while time.time() < deadline:
        try:
            status, body = http_request(COORD_HTTP_PORT, "GET", "/stats", timeout=2.0)
            if status == 200:
                for rep in body.get("replicas", []):
                    if rep.get("shard_id") == 0 and rep.get("is_primary") and rep.get("replica_id") != 0:
                        new_primary_replica_id = rep["replica_id"]
                        break
            if new_primary_replica_id is not None:
                break
        except Exception:
            pass
        time.sleep(0.5)

    if new_primary_replica_id is None:
        fail("automatic failover never happened within 20s -- health check "
             "may not have triggered at all, which is a different problem "
             "from the one this script tests")

    print(f"\n[verify] automatic failover happened: new primary = replica "
          f"{new_primary_replica_id}")

    status, body = http_request(COORD_HTTP_PORT, "GET", "/stats")
    print(f"[verify] /stats after automatic failover: {body}")
    total = body.get("total_element_count")

    print("\n==== verify_failover_fix result ====")
    print(f"new_primary_replica_id : {new_primary_replica_id}")
    print(f"total_element_count    : {total}")
    print(f"expected               : replica_id=2, total_element_count=1")

    if new_primary_replica_id == 2 and total == 1:
        print("\n[verify] FIX CONFIRMED: given a genuine choice between an "
              "empty replica that's checked FIRST (1) and a complete one "
              "checked second (2), the AUTOMATIC failover path picked the "
              "complete one despite it not being first. No data loss.")
        teardown()
        sys.exit(0)
    elif new_primary_replica_id == 1:
        print("\n[verify] FIX NOT WORKING: the automatic path promoted "
              "replica 1 (the empty one, first in iteration order) over "
              "replica 2 (the complete one). This is exactly the old "
              "'first reachable' bug -- the fix in health_check_loop is "
              "not behaving as intended.")
        teardown()
        sys.exit(1)
    else:
        print(f"\n[verify] UNEXPECTED outcome, needs investigation by hand: "
              f"new_primary_replica_id={new_primary_replica_id}, total={total}")
        teardown()
        sys.exit(2)


if __name__ == "__main__":
    main()

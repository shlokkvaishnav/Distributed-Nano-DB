#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FLAGS="-f $SCRIPT_DIR/deploy/docker-compose.cluster.yml -f $SCRIPT_DIR/deploy/docker-compose.monitoring.yml"
API="http://localhost:8080"

usage() {
    echo "Usage: ./cluster.sh <command>"
    echo ""
    echo "  up      Start cluster + monitoring stack (waits for Raft leader election)"
    echo "  down    Stop everything"
    echo "  status  Print cluster stats and Raft role of each coordinator"
    echo "  chaos   Run fault-injection harness for 60s (requires built binaries)"
    exit 1
}

cmd_up() {
    echo "Starting Nano-DB cluster (9 containers)..."
    docker compose $COMPOSE_FLAGS up -d --build

    echo "Waiting for Raft leader election..."
    local elapsed=0
    while [ $elapsed -lt 60 ]; do
        local role
        role=$(curl -sf "$API/raft/status" 2>/dev/null \
               | python3 -c "import sys,json; print(json.load(sys.stdin).get('role',''))" 2>/dev/null \
               || true)
        if [ "$role" = "leader" ]; then
            printf "\r  Elapsed: %ds — leader elected.\n" "$elapsed"
            echo ""
            echo "Cluster ready."
            echo ""
            echo "  API:     $API"
            echo "  Grafana: http://localhost:3000  (admin / nanodb)"
            echo ""
            echo "Next steps:"
            echo "  python3 scripts/demo_chaos.py     # kill the leader, watch zero writes drop"
            echo "  ./cluster.sh chaos                # 60s of random kills + invariant check"
            echo "  ./cluster.sh down                 # tear everything down"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
        printf "\r  Elapsed: %ds..." "$elapsed"
    done
    printf "\n"
    echo "ERROR: leader not elected after 60s."
    echo "Check logs: docker compose $COMPOSE_FLAGS logs coordinator-0"
    exit 1
}

cmd_down() {
    echo "Stopping Nano-DB cluster..."
    docker compose $COMPOSE_FLAGS down
    echo "Done."
}

cmd_status() {
    echo "=== Cluster Stats ==="
    curl -sf "$API/stats" | python3 -m json.tool || echo "(not reachable)"
    echo ""
    echo "=== Raft Status (all coordinators) ==="
    for port in 8080 8081 8082; do
        local label="coordinator-$((port - 8080))"
        local out
        out=$(curl -sf "http://localhost:$port/raft/status" 2>/dev/null \
              | python3 -c "import sys,json; d=json.load(sys.stdin); print(f\"  node_id={d['node_id']}  role={d['role']}  term={d['term']}  commit_index={d['commit_index']}\")" \
              2>/dev/null || echo "  (no response)")
        echo "$label: $out"
    done
}

cmd_chaos() {
    local bin="$SCRIPT_DIR/build/nano_coordinator"
    if [ ! -f "$bin" ]; then
        echo "ERROR: build/nano_coordinator not found."
        echo ""
        echo "Build the cluster binaries first:"
        echo "  mkdir -p build && cd build"
        echo "  cmake .. -DCMAKE_BUILD_TYPE=Release -DNANODB_BUILD_CLUSTER=ON"
        echo "  cmake --build . -j\$(nproc)"
        exit 1
    fi
    echo "Running chaos harness for 60 seconds (standalone cluster on ports 18180-18182)..."
    echo ""
    cd "$SCRIPT_DIR"
    python3 chaos_harness.py --duration 60
}

case "${1:-}" in
    up)     cmd_up ;;
    down)   cmd_down ;;
    status) cmd_status ;;
    chaos)  cmd_chaos ;;
    *)      usage ;;
esac

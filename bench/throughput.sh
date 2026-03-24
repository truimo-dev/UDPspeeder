#!/bin/bash
# bench/throughput.sh — Measure end-to-end UDP tunnel throughput
#
# Usage: ./bench/throughput.sh <speederv2_binary> [options]
#   --duration N     seconds per iteration (default: 5)
#   --fec X:Y        FEC parameter (default: disabled)
#   --disable-fec    explicitly disable FEC
#   --iterations N   number of runs, reports median (default: 3)
#   --json           output JSON for github-action-benchmark

set -euo pipefail

BINARY=""
DURATION=10
FEC_ARGS="--disable-fec"
FEC_LABEL="no-fec"
ITERATIONS=5
JSON=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --duration) DURATION="$2"; shift 2 ;;
        --fec) FEC_ARGS="-f $2"; FEC_LABEL="fec-${2//:/-}"; shift 2 ;;
        --disable-fec) FEC_ARGS="--disable-fec"; FEC_LABEL="no-fec"; shift ;;
        --iterations) ITERATIONS="$2"; shift 2 ;;
        --json) JSON=1; shift ;;
        -*) echo "Unknown option: $1" >&2; exit 1 ;;
        *) BINARY="$1"; shift ;;
    esac
done

if [[ -z "$BINARY" ]]; then
    echo "Usage: $0 <speederv2_binary> [--duration N] [--fec X:Y] [--json]" >&2
    exit 1
fi

if [[ ! -x "$BINARY" ]]; then
    echo "Error: $BINARY is not executable" >&2
    exit 1
fi

PORT_TUNNEL=20000
PORT_APP=20001
PORT_CLIENT=20002

# Kill any leftover processes from previous runs
kill_tunnel() {
    local pids
    pids=$(jobs -p 2>/dev/null) || true
    if [[ -n "$pids" ]]; then
        kill $pids 2>/dev/null || true
        wait $pids 2>/dev/null || true
    fi
}
trap kill_tunnel EXIT

run_once() {
    local tmpfile
    tmpfile=$(mktemp)
    kill_tunnel

    # UDP receiver: writes "bytes elapsed" to tmpfile, exits after 2s of no data
    python3 -c "
import socket, time, sys
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('127.0.0.1', $PORT_APP))
sock.settimeout(2)
total = 0
start = None
try:
    while True:
        data = sock.recv(65535)
        if start is None:
            start = time.monotonic()
        total += len(data)
except socket.timeout:
    pass
elapsed = time.monotonic() - start if start else 0
result = f'{total} {elapsed:.6f}'
sys.stdout.write(result + '\n')
sys.stdout.flush()
" > "$tmpfile" 2>/dev/null &
    local recv_pid=$!

    # Start tunnel
    $BINARY -s -l 127.0.0.1:$PORT_TUNNEL -r 127.0.0.1:$PORT_APP $FEC_ARGS --log-level 0 >/dev/null 2>&1 &
    local server_pid=$!

    $BINARY -c -l 127.0.0.1:$PORT_CLIENT -r 127.0.0.1:$PORT_TUNNEL $FEC_ARGS --log-level 0 >/dev/null 2>&1 &
    local client_pid=$!

    sleep 1

    # UDP sender: blasts 1400-byte packets for DURATION seconds
    python3 -c "
import socket, time
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
payload = b'\x00' * 1400
end = time.monotonic() + $DURATION
while time.monotonic() < end:
    try:
        sock.sendto(payload, ('127.0.0.1', $PORT_CLIENT))
    except OSError:
        pass
"
    echo "  sender done, waiting for receiver..." >&2

    # Wait for receiver to exit naturally (2s socket timeout after last packet)
    wait $recv_pid 2>/dev/null || true

    # Kill tunnel processes
    kill $server_pid $client_pid 2>/dev/null || true
    wait $server_pid $client_pid 2>/dev/null || true

    # Parse result
    local result bytes elapsed
    result=$(cat "$tmpfile")
    rm -f "$tmpfile"

    bytes=$(echo "$result" | awk '{print $1}')
    elapsed=$(echo "$result" | awk '{print $2}')

    echo "  received $bytes bytes in ${elapsed}s" >&2

    if [[ -z "$bytes" || "$bytes" == "0" ]]; then
        echo "0.0"
        return
    fi

    python3 -c "print(f'{$bytes / $elapsed / 1e6 * 8:.1f}')"
}

# Warmup run (discarded) — primes tunnel, caches, socket buffers
echo "  Warmup run..." >&2
run_once > /dev/null

# Run iterations and compute median
results=()
for i in $(seq 1 "$ITERATIONS"); do
    echo "  Run $i/$ITERATIONS..." >&2
    mbps=$(run_once)
    results+=("$mbps")
    echo "  → $mbps Mbps" >&2
done

IFS=$'\n' sorted=($(printf '%s\n' "${results[@]}" | sort -n)); unset IFS
median_idx=$(( ITERATIONS / 2 ))
median=${sorted[$median_idx]}
median=${median:-0.0}

if [[ $JSON -eq 1 ]]; then
    printf '{"name": "throughput/%s", "unit": "Mbps", "value": %s}\n' "$FEC_LABEL" "$median"
else
    echo "Throughput ($FEC_LABEL): $median Mbps  [runs: ${results[*]}]"
fi

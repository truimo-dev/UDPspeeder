#!/bin/sh
# Profile UDPspeeder on target hardware.
# Usage: ./profile.sh [results_dir]
#
# Expects bench_udpspeeder_static and test_udpspeeder_static in the
# same directory as this script (or current directory).
# Outputs results to results_dir (default: ./profile_results/).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" 2>/dev/null && pwd)" || SCRIPT_DIR="."
RESULTS_DIR="${1:-./profile_results}"
mkdir -p "$RESULTS_DIR"

# Find binaries: same dir as script, then cwd
find_bin() {
    if [ -x "$SCRIPT_DIR/$1" ]; then echo "$SCRIPT_DIR/$1"
    elif [ -x "./$1" ]; then echo "./$1"
    else echo ""; fi
}

BENCH_BIN="$(find_bin bench_udpspeeder_static)"
TEST_BIN="$(find_bin test_udpspeeder_static)"

if [ -z "$BENCH_BIN" ]; then
    echo "ERROR: bench_udpspeeder_static not found" >&2
    exit 1
fi

echo "=== UDPspeeder Profiling ==="
echo "Date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "Host: $(hostname 2>/dev/null || echo unknown)"
echo ""

# --- System info ---
INFO="$RESULTS_DIR/system_info.txt"
{
    echo "hostname: $(hostname 2>/dev/null || echo unknown)"
    echo "date: $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
    echo "uname: $(uname -a)"
    echo "arch: $(uname -m)"
    echo ""

    if [ -f /proc/cpuinfo ]; then
        echo "--- /proc/cpuinfo (first core) ---"
        # Print until first blank line (= first core only)
        sed '/^$/q' /proc/cpuinfo
        echo ""

        # Core count
        cores=$(grep -c '^processor' /proc/cpuinfo 2>/dev/null || echo "?")
        echo "core_count: $cores"
        echo ""
    fi

    # CPU frequency if available
    if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
        echo "--- cpufreq ---"
        for f in scaling_cur_freq scaling_min_freq scaling_max_freq scaling_governor; do
            p="/sys/devices/system/cpu/cpu0/cpufreq/$f"
            [ -f "$p" ] && echo "$f: $(cat "$p")"
        done
        echo ""
    fi
} > "$INFO" 2>&1
echo "System info:  $INFO"

# --- Tests ---
if [ -n "$TEST_BIN" ]; then
    echo ""
    echo "--- Running tests ---"
    TEST_LOG="$RESULTS_DIR/test_output.txt"
    if "$TEST_BIN" > "$TEST_LOG" 2>&1; then
        echo "Tests: PASSED"
    else
        echo "Tests: FAILED (see $TEST_LOG)" >&2
        cat "$TEST_LOG"
        exit 1
    fi
else
    echo "WARNING: test_udpspeeder_static not found, skipping tests" >&2
fi

# --- Benchmarks ---
echo ""
echo "--- Running benchmarks ---"

# Human-readable output (tee to both console and file)
BENCH_LOG="$RESULTS_DIR/bench_output.txt"
"$BENCH_BIN" 2>&1 | tee "$BENCH_LOG"

# JSON output for machine consumption
BENCH_JSON="$RESULTS_DIR/bench_results.json"
"$BENCH_BIN" --json 2>/dev/null
if [ -f bench_results.json ]; then
    mv bench_results.json "$BENCH_JSON"
    echo ""
    echo "JSON results: $BENCH_JSON"
fi

echo ""
echo "=== Done ==="
echo "Results in:   $RESULTS_DIR/"
ls -la "$RESULTS_DIR/"

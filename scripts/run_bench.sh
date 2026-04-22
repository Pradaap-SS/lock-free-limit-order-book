#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
RESULTS_DIR="$ROOT/docs/benchmarks"
BENCH="$BUILD_DIR/latency_bench"

if [[ ! -f "$BENCH" ]]; then
    echo "latency_bench not found. Run ./scripts/build.sh first."
    exit 1
fi

mkdir -p "$RESULTS_DIR"

TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
OUT_JSON="$RESULTS_DIR/bench_${TIMESTAMP}.json"
OUT_CSV="$RESULTS_DIR/bench_${TIMESTAMP}.csv"

echo "=== Running benchmarks ==="

# macOS: try to boost priority (may need sudo)
if [[ "$(uname)" == "Darwin" ]]; then
    echo "Tip: run with 'sudo nice -n -20 $BENCH' for more stable results"
fi

"$BENCH" \
    --benchmark_format=json \
    --benchmark_out="$OUT_JSON" \
    --benchmark_repetitions=5 \
    --benchmark_report_aggregates_only=true

echo ""
echo "Results saved to: $OUT_JSON"

# Convert to CSV for spreadsheet / matplotlib
python3 - "$OUT_JSON" "$OUT_CSV" <<'PYEOF'
import json, csv, sys

with open(sys.argv[1]) as f:
    data = json.load(f)

rows = []
for b in data.get("benchmarks", []):
    rows.append({
        "name":      b["name"],
        "iters":     b.get("iterations", ""),
        "real_ns":   b.get("real_time", ""),
        "cpu_ns":    b.get("cpu_time", ""),
        "items/sec": b.get("items_per_second", ""),
    })

with open(sys.argv[2], "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=["name","iters","real_ns","cpu_ns","items/sec"])
    w.writeheader()
    w.writerows(rows)

print(f"CSV: {sys.argv[2]}")
PYEOF

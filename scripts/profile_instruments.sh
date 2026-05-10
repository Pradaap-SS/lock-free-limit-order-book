#!/usr/bin/env bash
# Profile the order book binary using macOS Instruments (xctrace).
#
# Produces a .trace bundle you can open in Instruments.app for:
#   - Flamegraph (Time Profiler)
#   - Signpost timeline (matches SIGNPOST_BEGIN/END in order_book.h)
#   - System call / memory allocation tracing
#
# Usage:
#   ./scripts/profile_instruments.sh            # default: Time Profiler, 10s
#   ./scripts/profile_instruments.sh alloc      # allocation tracing
#   ./scripts/profile_instruments.sh signpost   # os_signpost timeline

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
BINARY="$BUILD/order_book"
OUT_DIR="$ROOT/docs/profiles"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

MODE="${1:-time}"   # time | alloc | signpost

if [[ ! -f "$BINARY" ]]; then
    echo "Binary not found: $BINARY"
    echo "Run: ./scripts/build.sh Release"
    exit 1
fi

mkdir -p "$OUT_DIR"

case "$MODE" in
  alloc)
    TEMPLATE="Allocations"
    TRACE="$OUT_DIR/alloc_${TIMESTAMP}.trace"
    ;;
  signpost)
    TEMPLATE="os_signpost"
    TRACE="$OUT_DIR/signpost_${TIMESTAMP}.trace"
    ;;
  *)
    TEMPLATE="Time Profiler"
    TRACE="$OUT_DIR/time_profiler_${TIMESTAMP}.trace"
    ;;
esac

echo "=== Instruments Profiling ==="
echo "Binary:   $BINARY"
echo "Template: $TEMPLATE"
echo "Output:   $TRACE"
echo ""

# xctrace is the command-line interface to Instruments (Xcode 12+)
if ! command -v xctrace &>/dev/null; then
    echo "xctrace not found. Install Xcode (not just Command Line Tools):"
    echo "  xcode-select --install   # CLT only — won't have xctrace"
    echo "  Open App Store → Xcode   # Full Xcode includes xctrace"
    echo ""
    echo "Alternatively, open Instruments.app manually:"
    echo "  open -a Instruments"
    echo "  File → New → Time Profiler → Choose target: $BINARY"
    exit 1
fi

echo "Recording for 15 seconds..."
xctrace record \
    --template "$TEMPLATE" \
    --output "$TRACE" \
    --time-limit 15s \
    --launch -- "$BINARY"

echo ""
echo "Trace saved: $TRACE"
echo ""
echo "Open in Instruments:"
echo "  open '$TRACE'"
echo ""
echo "=== Reading the Flamegraph ==="
echo "  - Wide bars = hot functions (most time spent here)"
echo "  - Look for unexpectedly wide bars in match() or add_order()"
echo "  - Click a frame to see source-level annotation"
echo ""
echo "=== Flamegraph Tips ==="
echo "  1. Filter by 'order_book' to hide OS/runtime frames"
echo "  2. Enable 'Heaviest Stack Trace' in the bottom panel"
echo "  3. Use the signpost timeline to correlate with add_order intervals"
echo ""

# Also export a text summary via xctrace export (if supported)
if xctrace help export &>/dev/null 2>&1; then
    SUMMARY="$OUT_DIR/summary_${TIMESTAMP}.txt"
    xctrace export --input "$TRACE" --xpath \
        '//trace-toc/run/data/table[@schema="time-profile"]' \
        > "$SUMMARY" 2>/dev/null || true
    [[ -s "$SUMMARY" ]] && echo "Text summary: $SUMMARY"
fi

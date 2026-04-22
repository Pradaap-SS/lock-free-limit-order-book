#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
BUILD_TYPE="${1:-Release}"

echo "=== Building Lock-Free Order Book ($BUILD_TYPE) ==="

# Install dependencies if missing
if ! command -v cmake &>/dev/null; then
    echo "Installing cmake..."
    brew install cmake
fi

for pkg in google-benchmark googletest; do
    if ! brew list "$pkg" &>/dev/null; then
        echo "Installing $pkg..."
        brew install "$pkg"
    fi
done

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

cmake "$ROOT" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

make -j"$(sysctl -n hw.ncpu)"

echo ""
echo "=== Build complete ==="
echo "Binaries:"
for bin in order_book latency_bench order_book_tests; do
    [[ -f "$BUILD_DIR/$bin" ]] && echo "  $BUILD_DIR/$bin"
done

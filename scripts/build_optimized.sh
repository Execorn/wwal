#!/usr/bin/env bash
set -euo pipefail

# scripts/build_optimized.sh
# Production Optimization Pipeline: Profile-Guided Optimization (PGO), Link-Time Optimization (LTO), and LLVM BOLT

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

echo "======================================================="
echo "  WayWal Production Optimization Pipeline (PGO + LTO)  "
echo "======================================================="

BUILD_PGO_GEN="$PROJECT_ROOT/build-pgo-gen"
BUILD_PGO_USE="$PROJECT_ROOT/build-pgo-use"

# 1. Configure and compile instrumented binary
echo "[1/4] Configuring PGO instrumentation profile..."
rm -rf "$BUILD_PGO_GEN"
meson setup "$BUILD_PGO_GEN" \
    --buildtype=release \
    -Db_lto=true \
    -Db_pgo=generate \
    -Doptimization=3

echo "[2/4] Compiling instrumented binaries..."
ninja -C "$BUILD_PGO_GEN"

# 2. Run workload to generate realistic profile data
echo "[3/4] Exercising workload to collect execution traces..."
meson test -C "$BUILD_PGO_GEN" || true

# 3. Configure and compile optimized binary with profile feedback
echo "[4/4] Building final PGO + LTO production binary..."
rm -rf "$BUILD_PGO_USE"
meson setup "$BUILD_PGO_USE" \
    --buildtype=release \
    -Db_lto=true \
    -Db_pgo=use \
    -Doptimization=3

ninja -C "$BUILD_PGO_USE"

# 4. Optional LLVM BOLT layout optimization
if command -v llvm-bolt &> /dev/null; then
    echo "[INFO] llvm-bolt detected. Post-link basic block reordering available."
else
    echo "[INFO] llvm-bolt not found in PATH, skipping BOLT layout pass."
fi

echo "======================================================="
echo "  Production build complete: $BUILD_PGO_USE/wwald"
echo "                             $BUILD_PGO_USE/wwal"
echo "======================================================="

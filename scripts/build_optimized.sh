#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

# scripts/build_optimized.sh
# Production Optimization Pipeline: Profile-Guided Optimization (PGO), Link-Time Optimization (LTO), and LLVM BOLT

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

echo "======================================================="
echo "  WayWal Production Optimization Pipeline (PGO + LTO)  "
echo "======================================================="

BUILD_PGO="$PROJECT_ROOT/build-pgo"

# 1. Configure and compile instrumented binary
echo "[1/4] Configuring PGO instrumentation profile in-place..."
rm -rf "$BUILD_PGO"
meson setup "$BUILD_PGO" \
    --buildtype=release \
    -Db_lto=true \
    -Db_pgo=generate \
    -Doptimization=3

echo "[2/4] Compiling instrumented binaries..."
ninja -C "$BUILD_PGO"

# 2. Run workload to generate realistic profile data
echo "[3/4] Exercising workload to collect execution traces..."
meson test -C "$BUILD_PGO" || true

# 3. Configure and compile optimized binary with profile feedback in-place
echo "[4/4] Building final PGO + LTO production binary with profile feedback..."
meson configure "$BUILD_PGO" -Db_pgo=use
ninja -C "$BUILD_PGO"

# 4. Optional LLVM BOLT layout optimization
if command -v llvm-bolt &> /dev/null; then
    echo "[INFO] llvm-bolt detected. Post-link basic block reordering available."
    if command -v perf2bolt &> /dev/null && [ -f "$BUILD_PGO/perf.fdata" ]; then
        echo "[INFO] Applying BOLT binary optimization using perf profile..."
        llvm-bolt "$BUILD_PGO/wwald" -o "$BUILD_PGO/wwald.bolt" -data "$BUILD_PGO/perf.fdata" -reorder-blocks=ext-tsp -reorder-functions=cdsort -split-functions -split-all-cold -split-eh -dyno-stats || true
    fi
else
    echo "[INFO] llvm-bolt not found in PATH, skipping BOLT layout pass."
fi

echo "======================================================="
echo "  Production build complete: $BUILD_PGO/wwald"
echo "                             $BUILD_PGO/wwal"
echo "======================================================="

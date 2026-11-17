#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

# Production Optimization Pipeline: Profile-Guided Optimization (PGO), LTO, and optional LLVM BOLT.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

usage() {
    cat <<'EOF'
Usage: scripts/build_optimized.sh [OPTIONS]

Options:
  --build-dir <dir>    Build directory for PGO+LTO artifacts (default: build-pgo)
  --output-dir <dir>   Optional directory to copy final optimized binaries into
  --jobs <n>           Parallel compile/test jobs (overrides WWAL_BUILD_JOBS)
  --sem <path>         Optional governor/semaphore wrapper (overrides WWAL_BUILD_SEM)
  --skip-bench         Skip extra benchmark workloads during PGO training (WWAL_PGO_SKIP_BENCH=1)
  --clean              Remove transient PGO/BOLT profile files after build (WWAL_PGO_CLEAN=1)
  --clean-build        Remove the entire PGO build directory after staging --output-dir
  --clean-only         Remove the PGO build directory and exit
  -h, --help           Show this help message

Environment variables:
  WWAL_BUILD_PGO_DIR     Build directory path (default: <repo>/build-pgo)
  WWAL_BUILD_JOBS        Compile job count (default: auto-sized from CPU and available RAM)
  WWAL_TEST_JOBS         Test runner concurrency (default: min(WWAL_BUILD_JOBS, 2))
  WWAL_BUILD_SEM         Optional command wrapper prepended to meson/ninja/test invocations
  WWAL_PGO_SKIP_BENCH    Set to 1 to skip extra bench_transitions workload (default: 0)
  WWAL_PGO_BENCH_FRAMES  Frame count per effect for bench_transitions training (default: 20)
  WWAL_BOLT_FDATA        Pre-recorded perf.fdata path for llvm-bolt
  WWAL_OUTPUT_DIR        Destination directory for final optimized binaries
  WWAL_PGO_CLEAN         Set to 1 to clean transient profile/BOLT files after build
  WWAL_PGO_CLEAN_BUILD   Set to 1 to remove build dir after copying to WWAL_OUTPUT_DIR
  QT_QPA_PLATFORM        Qt platform plugin (default: offscreen)
  LIBVA_DRIVER_NAME      VA-API driver name (default: dummy)
EOF
}

default_build_jobs() {
    local cpus avail_kb mem_jobs jobs
    cpus="$(nproc 2>/dev/null || echo 2)"
    avail_kb="$(awk '/^MemAvailable:/ {print $2; exit}' /proc/meminfo 2>/dev/null || echo 4194304)"
    mem_jobs=$((avail_kb / 2097152))
    if (( mem_jobs < 1 )); then
        mem_jobs=1
    fi
    jobs="$cpus"
    if (( mem_jobs < jobs )); then
        jobs="$mem_jobs"
    fi
    if (( jobs > 4 )); then
        jobs=4
    fi
    if (( jobs < 1 )); then
        jobs=1
    fi
    echo "$jobs"
}

BUILD_PGO="${WWAL_BUILD_PGO_DIR:-$PROJECT_ROOT/build-pgo}"
OUTPUT_DIR="${WWAL_OUTPUT_DIR:-}"
WWAL_BUILD_JOBS="${WWAL_BUILD_JOBS:-$(default_build_jobs)}"
WWAL_BUILD_SEM="${WWAL_BUILD_SEM:-}"
WWAL_PGO_SKIP_BENCH="${WWAL_PGO_SKIP_BENCH:-0}"
WWAL_PGO_BENCH_FRAMES="${WWAL_PGO_BENCH_FRAMES:-20}"
WWAL_PGO_CLEAN="${WWAL_PGO_CLEAN:-0}"
WWAL_PGO_CLEAN_BUILD="${WWAL_PGO_CLEAN_BUILD:-0}"
CLEAN_ONLY=0

while (($# > 0)); do
    case "$1" in
        --build-dir)
            [[ $# -ge 2 ]] || { echo "[ERROR] --build-dir requires an argument" >&2; exit 2; }
            BUILD_PGO="$2"
            shift 2
            ;;
        --output-dir)
            [[ $# -ge 2 ]] || { echo "[ERROR] --output-dir requires an argument" >&2; exit 2; }
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --jobs)
            [[ $# -ge 2 ]] || { echo "[ERROR] --jobs requires an argument" >&2; exit 2; }
            WWAL_BUILD_JOBS="$2"
            shift 2
            ;;
        --sem)
            [[ $# -ge 2 ]] || { echo "[ERROR] --sem requires an argument" >&2; exit 2; }
            WWAL_BUILD_SEM="$2"
            shift 2
            ;;
        --skip-bench)
            WWAL_PGO_SKIP_BENCH=1
            shift
            ;;
        --clean)
            WWAL_PGO_CLEAN=1
            shift
            ;;
        --clean-build)
            WWAL_PGO_CLEAN_BUILD=1
            shift
            ;;
        --clean-only)
            CLEAN_ONLY=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [[ "$BUILD_PGO" != /* ]]; then
    BUILD_PGO="$PROJECT_ROOT/$BUILD_PGO"
fi
if [[ -n "$OUTPUT_DIR" && "$OUTPUT_DIR" != /* ]]; then
    OUTPUT_DIR="$PROJECT_ROOT/$OUTPUT_DIR"
fi

if [[ "$CLEAN_ONLY" == "1" ]]; then
    echo "[CLEAN] Removing $BUILD_PGO..."
    rm -rf "$BUILD_PGO"
    exit 0
fi

if ! [[ "$WWAL_BUILD_JOBS" =~ ^[1-9][0-9]*$ ]]; then
    echo "[ERROR] WWAL_BUILD_JOBS must be a positive integer (got: '$WWAL_BUILD_JOBS')" >&2
    exit 2
fi

if [[ -z "${WWAL_TEST_JOBS:-}" ]]; then
    WWAL_TEST_JOBS="$WWAL_BUILD_JOBS"
    if (( WWAL_TEST_JOBS > 2 )); then
        WWAL_TEST_JOBS=2
    fi
fi

if [[ -n "$WWAL_BUILD_SEM" && ! -x "$WWAL_BUILD_SEM" ]]; then
    echo "[ERROR] WWAL_BUILD_SEM is not executable: $WWAL_BUILD_SEM" >&2
    exit 2
fi

run_governed() {
    if [[ -n "$WWAL_BUILD_SEM" ]]; then
        "$WWAL_BUILD_SEM" "$@"
    else
        "$@"
    fi
}

export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-dummy}"
# Disable strict timing asserts during instrumented PGO generation to avoid false timeouts.
unset WWAL_STRICT_PERF_ASSERTS

PGO_TMP_ROOT=""
cleanup() {
    if [[ -n "$PGO_TMP_ROOT" && -d "$PGO_TMP_ROOT" ]]; then
        rm -rf "$PGO_TMP_ROOT"
    fi
}
trap cleanup EXIT

echo "======================================================="
echo "  WayWal Production Optimization Pipeline (PGO + LTO)  "
echo "======================================================="
echo "  Build dir:  $BUILD_PGO"
echo "  Jobs:       compile=$WWAL_BUILD_JOBS test=$WWAL_TEST_JOBS"
echo "  Skip bench: $WWAL_PGO_SKIP_BENCH"

echo "[1/5] Configuring PGO instrumentation profile in-place..."
rm -rf "$BUILD_PGO"
run_governed meson setup "$BUILD_PGO" \
    --buildtype=release \
    -Db_lto=true \
    -Db_pgo=generate \
    -Doptimization=3 \
    -Dtests=true \
    -Dstudio=enabled

echo "[2/5] Compiling instrumented binaries..."
run_governed meson compile -C "$BUILD_PGO" -j "$WWAL_BUILD_JOBS"

echo "[3/5] Exercising headless test suite and workloads to collect PGO profiles..."
PGO_TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/wwal_pgo_env.XXXXXX")"
mkdir -p "$PGO_TMP_ROOT/runtime" "$PGO_TMP_ROOT/cache" "$PGO_TMP_ROOT/config" "$PGO_TMP_ROOT/state" "$PGO_TMP_ROOT/wallpapers"
chmod 700 "$PGO_TMP_ROOT/runtime"

export XDG_RUNTIME_DIR="$PGO_TMP_ROOT/runtime"
export XDG_CACHE_HOME="$PGO_TMP_ROOT/cache"
export XDG_CONFIG_HOME="$PGO_TMP_ROOT/config"
export XDG_STATE_HOME="$PGO_TMP_ROOT/state"
export WWAL_CACHE_DIR="$PGO_TMP_ROOT/cache/wwal"
export WWAL_STATE_DIR="$PGO_TMP_ROOT/state/wwal"
export LLVM_PROFILE_FILE="$BUILD_PGO/pgo-%p-%m.profraw"

if ! run_governed meson test -C "$BUILD_PGO" --num-processes "$WWAL_TEST_JOBS" --print-errorlogs; then
    echo "[ERROR] PGO training test suite failed; aborting so unprofiled binary is not emitted." >&2
    exit 1
fi

if [[ -f "$PROJECT_ROOT/tests/test_pattern.png" ]]; then
    cp "$PROJECT_ROOT/tests/test_pattern.png" "$PGO_TMP_ROOT/wallpapers/pattern.png"
fi
if [[ -f "$PROJECT_ROOT/contrib/shaders/plasma.frag" ]]; then
    cp "$PROJECT_ROOT/contrib/shaders/plasma.frag" "$PGO_TMP_ROOT/wallpapers/plasma.frag"
fi

"$BUILD_PGO/wwald" --version >/dev/null
"$BUILD_PGO/wwald" --help >/dev/null
"$BUILD_PGO/wwal" --version >/dev/null
"$BUILD_PGO/wwal" --help >/dev/null
run_governed "$BUILD_PGO/wwal" pick --dir "$PGO_TMP_ROOT/wallpapers" --menu json >/dev/null

if [[ -x "$BUILD_PGO/wwal-studio" ]]; then
    run_governed "$BUILD_PGO/wwal-studio" --headless-check >/dev/null
    if "$BUILD_PGO/wwal-studio" --help 2>&1 | grep -q -- "--realize-all-panels"; then
        run_governed "$BUILD_PGO/wwal-studio" --headless-check --realize-all-panels >/dev/null
    fi
fi

if [[ "$WWAL_PGO_SKIP_BENCH" == "0" ]]; then
    if [[ -x "$BUILD_PGO/bench_transitions" ]]; then
        echo "[3/5] Running bench_transitions (${WWAL_PGO_BENCH_FRAMES} frames) for PGO hot-loop feedback..."
        run_governed "$BUILD_PGO/bench_transitions" "$WWAL_PGO_BENCH_FRAMES" >/dev/null
    fi
else
    echo "[3/5] Skipping extra bench_transitions training (WWAL_PGO_SKIP_BENCH=$WWAL_PGO_SKIP_BENCH)."
fi

if compgen -G "$BUILD_PGO/pgo-*.profraw" >/dev/null; then
    if ! command -v llvm-profdata >/dev/null 2>&1; then
        echo "[ERROR] Found .profraw PGO files but llvm-profdata is not installed in PATH." >&2
        exit 1
    fi
    llvm-profdata merge -output="$BUILD_PGO/default.profdata" "$BUILD_PGO"/pgo-*.profraw
fi
unset LLVM_PROFILE_FILE

echo "[4/5] Building final PGO + LTO production binaries with profile feedback..."
CONFIGURE_ARGS=(-Db_pgo=use)
if command -v llvm-bolt >/dev/null 2>&1; then
    # Preserve ELF relocations required by llvm-bolt basic-block and function splitting passes.
    CONFIGURE_ARGS+=(-Dc_link_args=-Wl,--emit-relocs -Dcpp_link_args=-Wl,--emit-relocs)
fi
run_governed meson configure "$BUILD_PGO" "${CONFIGURE_ARGS[@]}"
run_governed meson compile -C "$BUILD_PGO" -j "$WWAL_BUILD_JOBS"

echo "[5/5] Checking LLVM BOLT post-link optimization..."
if command -v llvm-bolt >/dev/null 2>&1; then
    BOLT_HELP="$(llvm-bolt --help 2>&1 || true)"
    BOLT_FLAGS=(-reorder-blocks=ext-tsp -split-functions -split-all-cold -split-eh -dyno-stats)
    if grep -q "cdsort" <<<"$BOLT_HELP"; then
        BOLT_FLAGS+=(-reorder-functions=cdsort)
    elif grep -q "hfsort+" <<<"$BOLT_HELP"; then
        BOLT_FLAGS+=(-reorder-functions=hfsort+)
    fi

    FDATA_PATH="${WWAL_BOLT_FDATA:-$BUILD_PGO/perf.fdata}"
    if [[ -n "${WWAL_BOLT_FDATA:-}" && ! -f "$FDATA_PATH" ]]; then
        echo "[ERROR] WWAL_BOLT_FDATA does not exist: $FDATA_PATH" >&2
        exit 1
    fi

    if [[ ! -f "$FDATA_PATH" ]] && command -v perf >/dev/null 2>&1 && command -v perf2bolt >/dev/null 2>&1; then
        echo "[INFO] Collecting perf trace for BOLT profile conversion..."
        if perf record -e cycles:u -j any,u -o "$BUILD_PGO/perf.data" -- "$BUILD_PGO/wwald" --help >/dev/null 2>&1; then
            perf2bolt "$BUILD_PGO/wwald" -p "$BUILD_PGO/perf.data" -o "$FDATA_PATH"
        elif perf record -e cycles:u -o "$BUILD_PGO/perf.data" -- "$BUILD_PGO/wwald" --help >/dev/null 2>&1; then
            perf2bolt -nl "$BUILD_PGO/wwald" -p "$BUILD_PGO/perf.data" -o "$FDATA_PATH"
        else
            echo "[INFO] Hardware perf sampling unavailable on this host; trying BOLT instrumentation fallback."
        fi
    fi

    if [[ ! -f "$FDATA_PATH" ]] && grep -q -- "-instrument" <<<"$BOLT_HELP"; then
        echo "[INFO] Instrumenting wwald with llvm-bolt to generate deterministic BOLT profile..."
        llvm-bolt "$BUILD_PGO/wwald" -instrument -instrumentation-file="$FDATA_PATH" -o "$BUILD_PGO/wwald.bolt.inst"
        "$BUILD_PGO/wwald.bolt.inst" --version >/dev/null
        "$BUILD_PGO/wwald.bolt.inst" --help >/dev/null
        rm -f "$BUILD_PGO/wwald.bolt.inst"
    fi

    BOLT_DATA_ARGS=()
    if [[ -s "$FDATA_PATH" ]]; then
        BOLT_DATA_ARGS+=(-data="$FDATA_PATH")
    fi

    echo "[INFO] Running llvm-bolt on $BUILD_PGO/wwald..."
    llvm-bolt "$BUILD_PGO/wwald" -o "$BUILD_PGO/wwald.bolt" "${BOLT_DATA_ARGS[@]}" "${BOLT_FLAGS[@]}"
    if ! "$BUILD_PGO/wwald.bolt" --version >/dev/null; then
        echo "[ERROR] BOLT-optimized wwald failed post-optimization verification!" >&2
        exit 1
    fi
    mv -f "$BUILD_PGO/wwald.bolt" "$BUILD_PGO/wwald"
else
    echo "[INFO] llvm-bolt not found in PATH, skipping BOLT layout pass."
fi

"$BUILD_PGO/wwald" --version >/dev/null
"$BUILD_PGO/wwal" --version >/dev/null

if [[ -n "$OUTPUT_DIR" ]]; then
    mkdir -p "$OUTPUT_DIR"
    install -m 0755 "$BUILD_PGO/wwald" "$OUTPUT_DIR/wwald"
    install -m 0755 "$BUILD_PGO/wwal" "$OUTPUT_DIR/wwal"
    if [[ -x "$BUILD_PGO/wwal-studio" ]]; then
        install -m 0755 "$BUILD_PGO/wwal-studio" "$OUTPUT_DIR/wwal-studio"
    fi
fi

if [[ "$WWAL_PGO_CLEAN" == "1" ]]; then
    echo "[CLEAN] Removing transient PGO/BOLT profile artifacts in $BUILD_PGO..."
    rm -f "$BUILD_PGO"/pgo-*.profraw "$BUILD_PGO/default.profdata" "$BUILD_PGO/perf.data" "$BUILD_PGO/perf.fdata" "$BUILD_PGO/wwald.bolt.inst"
fi

if [[ "$WWAL_PGO_CLEAN_BUILD" == "1" ]]; then
    if [[ -z "$OUTPUT_DIR" ]]; then
        echo "[ERROR] --clean-build requires --output-dir (or WWAL_OUTPUT_DIR) so binaries are preserved." >&2
        exit 2
    fi
    echo "[CLEAN] Removing PGO build directory $BUILD_PGO..."
    rm -rf "$BUILD_PGO"
fi

echo "======================================================="
if [[ -n "$OUTPUT_DIR" ]]; then
    echo "  Production build complete: $OUTPUT_DIR/wwald"
    echo "                             $OUTPUT_DIR/wwal"
    if [[ -x "$OUTPUT_DIR/wwal-studio" ]]; then
        echo "                             $OUTPUT_DIR/wwal-studio"
    fi
else
    echo "  Production build complete: $BUILD_PGO/wwald"
    echo "                             $BUILD_PGO/wwal"
    if [[ -x "$BUILD_PGO/wwal-studio" ]]; then
        echo "                             $BUILD_PGO/wwal-studio"
    fi
fi
echo "======================================================="

#!/usr/bin/env bash
set -euo pipefail

NAMESPACE="test_integ_$$"
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../build" && pwd)"

echo "=== Running WayWal Integration Test ==="
echo "Build directory: ${BUILD_DIR}"
echo "Namespace: ${NAMESPACE}"

# Start daemon in background
echo "[1] Starting wwald daemon..."
"${BUILD_DIR}/wwald" --namespace "${NAMESPACE}" -v &
DAEMON_PID=$!

trap 'kill -9 ${DAEMON_PID} 2>/dev/null || true' EXIT

SOCK_PATH="${XDG_RUNTIME_DIR:-/tmp}/${WAYLAND_DISPLAY:-wayland-0}-wwald.${NAMESPACE}.sock"

# Wait for socket to become ready
for i in {1..40}; do
    if [ -S "${SOCK_PATH}" ]; then
        break
    fi
    sleep 0.05
done

if [ ! -S "${SOCK_PATH}" ]; then
    echo "Error: Daemon did not create socket at ${SOCK_PATH}"
    exit 1
fi

# 1. Ping
echo "[2] Testing ping..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" ping

# 2. Query
echo "[3] Testing query..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query

# 3. Clear (green)
echo "[4] Testing clear to green (#00ff00)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" clear 00ff00

# 4. Clear (blue with alpha)
echo "[5] Testing clear to blue (#0000ff80)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" clear 0000ff80

# 5. Image test (instant)
echo "[6] Testing image display (instant)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type none

# 6. Image transition tests (fade, wipe, wave, grow, noise)
echo "[7] Testing fade transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type fade --transition-duration 0.05

echo "[8] Testing wipe transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type wipe --transition-duration 0.05 --transition-angle 45

echo "[9] Testing wave transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type wave --transition-duration 0.05

echo "[10] Testing grow transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type grow --transition-duration 0.05

echo "[11] Testing noise transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type noise --transition-duration 0.05

# 7. Hardware Video wallpaper playback tests
VIDEO_PATH="$(dirname "${BASH_SOURCE[0]}")/test_video.mp4"
if [ -f "${VIDEO_PATH}" ]; then
    echo "[12] Testing hardware video wallpaper playback (VA-API)..."
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" video "${VIDEO_PATH}" --loop 0 --speed 1.0
    sleep 0.1

    echo "[13] Testing video pause..."
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" pause
    sleep 0.05

    echo "[14] Testing video unpause..."
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" unpause
    sleep 0.05

    echo "[15] Testing video toggle..."
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" toggle
    sleep 0.05
fi

# 8. Terminate daemon
echo "[16] Testing kill command..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" kill

# Wait for daemon process to terminate
wait "${DAEMON_PID}" || true
trap - EXIT

echo "=== Integration Test PASSED Successfully! ==="

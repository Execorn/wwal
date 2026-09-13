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

echo "[12] Testing crosszoom transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type crosszoom --transition-duration 0.05

echo "[13] Testing slide transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type slide --transition-duration 0.05 --transition-angle 90

echo "[14] Testing glitch transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type glitch --transition-duration 0.05

echo "[15] Testing burn transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type burn --transition-duration 0.05

echo "[16] Testing ripple transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type ripple --transition-duration 0.05 --transition-pos 0.5,0.5

echo "[17] Testing pixelate transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type pixelate --transition-duration 0.05

echo "[18] Testing doom transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type doom --transition-duration 0.05

echo "[19] Testing swirl transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type swirl --transition-duration 0.05

echo "[20] Testing cube transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type cube --transition-duration 0.05

echo "[21] Testing luma transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type luma --transition-duration 0.05

echo "[22] Testing light_leak transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type light_leak --transition-duration 0.05

echo "[23] Testing page_curl transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type page_curl --transition-duration 0.05

echo "[24] Testing positional aliases (cursor, center, top-left)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type grow --transition-duration 0.05 --transition-pos cursor
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type ripple --transition-duration 0.05 --transition-pos center
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type wave --transition-duration 0.05 --transition-pos top-left

echo "[25] Testing custom compute shader transition..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type custom --transition-shader "$(dirname "${BASH_SOURCE[0]}")/custom_test.comp" --transition-duration 0.05

echo "[26] Testing staggered synchronization mode and 10-bit scanout flag..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type fade --transition-duration 0.05 --sync-mode staggered --stagger-delay 50 --10bit

echo "[27] Testing slideshow daemon engine and runtime controls..."
TMP_SLIDESHOW_DIR=$(mktemp -d /tmp/wwal_integ_slideshow_XXXXXX)
cp "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" "${TMP_SLIDESHOW_DIR}/slide1.png"
cp "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" "${TMP_SLIDESHOW_DIR}/slide2.png"

"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow "${TMP_SLIDESHOW_DIR}" --interval 2 --shuffle --transition-type fade --transition-duration 0.05
sleep 0.1

echo "[28] Testing slideshow pause..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow pause
sleep 0.05

echo "[29] Testing slideshow resume..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow resume
sleep 0.05

echo "[30] Testing slideshow next/prev..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow next
sleep 0.05
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow prev
sleep 0.05

echo "[31] Testing slideshow toggle and stop..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow toggle
sleep 0.05
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow stop
sleep 0.05

rm -rf "${TMP_SLIDESHOW_DIR}"

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

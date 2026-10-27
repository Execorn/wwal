#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

NAMESPACE="test_integ_$$"
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../build" && pwd)"
TMP_INTEG_STATE="$(mktemp -d /tmp/wwal_integ_state_XXXXXX)"
TMP_INTEG_CACHE="$(mktemp -d /tmp/wwal_integ_cache_XXXXXX)"
export WWAL_STATE_DIR="${TMP_INTEG_STATE}"
export WWAL_CACHE_DIR="${TMP_INTEG_CACHE}"

echo "=== Running WayWal Integration Test ==="
echo "Build directory: ${BUILD_DIR}"
echo "Namespace: ${NAMESPACE}"

# Verify standalone CLI commands that do not require an active compositor
echo "[0] Testing standalone wwal CLI commands (--version, --help, theme)..."
"${BUILD_DIR}/wwal" --version
"${BUILD_DIR}/wwal" --help >/dev/null
TMP_THEME_STATE=$(mktemp -d /tmp/wwal_integ_theme_state_XXXXXX)
TMP_THEME_CACHE=$(mktemp -d /tmp/wwal_integ_theme_cache_XXXXXX)
WWAL_STATE_DIR="${TMP_THEME_STATE}" WWAL_CACHE_DIR="${TMP_THEME_CACHE}" \
    "${BUILD_DIR}/wwal" theme "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png"
rm -rf "${TMP_THEME_STATE}" "${TMP_THEME_CACHE}"

WL_SOCK="${XDG_RUNTIME_DIR:-/tmp}/${WAYLAND_DISPLAY:-wayland-0}"
if [ ! -S "${WL_SOCK}" ]; then
    echo "SKIP: No live Wayland compositor socket at ${WL_SOCK} (headless CI environment)."
    rm -rf "${TMP_INTEG_STATE}" "${TMP_INTEG_CACHE}"
    exit 77
fi

# Start daemon in background
echo "[1] Starting wwald daemon..."
"${BUILD_DIR}/wwald" --namespace "${NAMESPACE}" -v &
DAEMON_PID=$!

trap 'kill -9 ${DAEMON_PID} 2>/dev/null || true; rm -rf "${TMP_INTEG_STATE}" "${TMP_INTEG_CACHE}"' EXIT

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

echo "[26] Testing staggered synchronization mode, custom bezier, fill-color, and 10-bit scanout flag..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type fade --transition-duration 0.05 --transition-bezier 0.42,0.0,0.58,1.0 --fill-color 1a2b3c --scaling-mode fit --sync-mode staggered --stagger-delay 50 --10bit

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

echo "[32] Testing wwal stats telemetry..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" stats

echo "[33] Testing wwal theme standalone extraction..."
TMP_THEME_STATE=$(mktemp -d /tmp/wwal_integ_theme_state_XXXXXX)
TMP_THEME_CACHE=$(mktemp -d /tmp/wwal_integ_theme_cache_XXXXXX)
WWAL_STATE_DIR="${TMP_THEME_STATE}" WWAL_CACHE_DIR="${TMP_THEME_CACHE}" \
    "${BUILD_DIR}/wwal" theme "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png"
rm -rf "${TMP_THEME_STATE}" "${TMP_THEME_CACHE}"

echo "[34] Testing swww compatibility flags (--resize, --fill-color, --filter)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --resize crop --fill-color 1e1e2e --filter Lanczos3 --transition-type left --transition-pos tr --transition-duration 0.05

echo "[34b] Testing --scaling-mode span and live cross-mode GPU transitions (Image <-> Shader)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --scaling-mode span --transition-type fade --transition-duration 0.05
TMP_INTEG_SHADER="${TMP_INTEG_CACHE}/integ_deriv_span.frag"
cat > "${TMP_INTEG_SHADER}" << 'EOF'
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    float edge = fwidth(uv.x) + abs(dFdx(uv.x)) + abs(dFdy(uv.y));
    fragColor = vec4(uv.x, uv.y, clamp(edge * 100.0, 0.0, 1.0), 1.0);
}
EOF
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" shader "${TMP_INTEG_SHADER}" --scaling-mode span --transition-type wipe --transition-duration 0.05
sleep 0.08
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type grow --transition-duration 0.05
sleep 0.08

echo "[34c] Testing per-output multi-engine concurrency (shader, video, slideshow isolation)..."
mapfile -t OUT_NAMES < <("${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | awk '/^  - /{print $2}')
if [ "${#OUT_NAMES[@]}" -ge 2 ]; then
    OUT_A="${OUT_NAMES[0]}"
    OUT_B="${OUT_NAMES[1]}"
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" shader "${TMP_INTEG_SHADER}" -o "${OUT_A}" --fps 60 --transition-type fade --transition-duration 0.05
    if [ -f "${VIDEO_PATH}" ]; then
        "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" video "${VIDEO_PATH}" -o "${OUT_A}" --loop 0
        "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" video "${VIDEO_PATH}" -o "${OUT_B}" --loop 0
        sleep 0.1
        "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" -o "${OUT_A}" --transition-type fade --transition-duration 0.05
        sleep 0.1
        STATS_OUT=$("${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" stats)
        echo "${STATS_OUT}" | grep -q "  - ${OUT_A}:.*engine=static"
        echo "${STATS_OUT}" | grep -q "  - ${OUT_B}:.*engine=video"
    fi
fi

echo "[35] Testing wwal restore..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" restore

echo "[35b] Testing JSON IPC inspection, live uniform control, audio ducking & event subscription..."
STATUS_JSON=$("${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" status)
echo "${STATUS_JSON}" | grep -q '"pid":'
echo "${STATUS_JSON}" | grep -q '"outputs":'
echo "${STATUS_JSON}" | grep -q '"scale":'
echo "${STATUS_JSON}" | grep -q '"paused":'
echo "${STATUS_JSON}" | grep -q '"asset":'
echo "${STATUS_JSON}" | grep -q '"mpris":'
echo "${STATUS_JSON}" | grep -q '"playing":'
echo "${STATUS_JSON}" | grep -q '"length_us":'
echo "${STATUS_JSON}" | grep -q '"audio":'
echo "${STATUS_JSON}" | grep -q '"external_active":'
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query --json | grep -q '"outputs":'
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" stats --json | grep -q '"outputs":'
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" audio --volume 0.5 --mute-on-audio --theme-from-media
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" status | grep -q '"volume":0.500'

SUB_LOG="${TMP_INTEG_CACHE}/sub_events.jsonl"
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" subscribe > "${SUB_LOG}" &
SUB_PID=$!
sleep 0.05
grep -q '"event":"initial"' "${SUB_LOG}"

"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" shader "${TMP_INTEG_SHADER}" --fps 60 --transition-type none
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" set-uniform u_Speed 2.5
sleep 0.05
kill "${SUB_PID}" 2>/dev/null || true
wait "${SUB_PID}" 2>/dev/null || true
grep -q '"event":"wallpaper_changed"' "${SUB_LOG}"
grep -q '"event":"uniform_changed"' "${SUB_LOG}"

# 8. Terminate daemon
echo "[36] Testing kill command..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" kill

# Wait for daemon process to terminate
wait "${DAEMON_PID}" || true
rm -rf "${TMP_INTEG_STATE}" "${TMP_INTEG_CACHE}"
trap - EXIT

echo "=== Integration Test PASSED Successfully! ==="

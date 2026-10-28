#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

NAMESPACE="test_integ_$$"
BUILD_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../build" && pwd)"
ORIG_HOME="${HOME:-}"
TMP_INTEG_STATE="$(mktemp -d /tmp/wwal_integ_state_XXXXXX)"
TMP_INTEG_CACHE="$(mktemp -d /tmp/wwal_integ_cache_XXXXXX)"
mkdir -p "${TMP_INTEG_STATE}/home" "${TMP_INTEG_STATE}/config"
if [ -n "${ORIG_HOME}" ] && [ -d "${ORIG_HOME}/.nv" ]; then
    ln -s "${ORIG_HOME}/.nv" "${TMP_INTEG_STATE}/home/.nv"
fi
export HOME="${TMP_INTEG_STATE}/home"
export XDG_STATE_HOME="${TMP_INTEG_STATE}"
export XDG_CACHE_HOME="${TMP_INTEG_CACHE}"
export XDG_CONFIG_HOME="${TMP_INTEG_STATE}/config"
export WWAL_STATE_DIR="${TMP_INTEG_STATE}"
export WWAL_CACHE_DIR="${TMP_INTEG_CACHE}"
export WWAL_NAMESPACE_PREFIX="${NAMESPACE}"

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
for i in {1..200}; do
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
QUERY_OUT=$("${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query)
echo "${QUERY_OUT}"
echo "${QUERY_OUT}" | grep -q "currently displaying:"

# 2b. Idempotent daemon startup check (must exit 0 when namespace is already active)
echo "[3b] Testing idempotent wwald startup on active namespace..."
"${BUILD_DIR}/wwald" --namespace "${NAMESPACE}" -q

# 3. Clear (green)
echo "[4] Testing clear to green (#00ff00)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" clear 00ff00
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: color: #00ff00"

# 4. Clear (blue with alpha) and solid color via wwal img
echo "[5] Testing clear to blue (#0000ff80) and wwal img 0x1e1e2e..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" clear 0000ff80
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img 0x1e1e2e
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: color: #1e1e2e"

# 5. Image test (instant)
echo "[6] Testing image display (instant)..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type none
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*test_pattern.png"

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

echo "[33] Testing wwal theme standalone extraction, --source-color-index, --seed-color, & [template.*] engine..."
TMP_THEME_STATE=$(mktemp -d /tmp/wwal_integ_theme_state_XXXXXX)
TMP_THEME_CACHE=$(mktemp -d /tmp/wwal_integ_theme_cache_XXXXXX)
TMP_THEME_CFG=$(mktemp -d /tmp/wwal_integ_theme_cfg_XXXXXX)
mkdir -p "${TMP_THEME_CFG}/wwal"
cat > "${TMP_THEME_CFG}/test_tpl.in" << 'EOF'
primary={{colors.primary.default.hex}}
primary_light={{colors.primary.light.hex}}
source={{colors.source_color.default.hex}}
lighter={{ (colors.primary.default | lighten(0.15)).hex }}
rgb_alpha={{ colors.primary.default.rgb | alpha(0.8) }}
hsl_comp={{ #ff0000 | complementary | hex }}
pywal_c0={color0.strip}
image={{image}}
EOF
cat > "${TMP_THEME_CFG}/wwal/config.toml" << EOF
[theme]
enabled = true
pywal_compat = false

[template.integ]
input_path = "${TMP_THEME_CFG}/test_tpl.in"
output_path = "${TMP_THEME_CACHE}/rendered_tpl.out"
post_hook = "touch ${TMP_THEME_CACHE}/tpl_hook_ok"
EOF
WWAL_STATE_DIR="${TMP_THEME_STATE}" WWAL_CACHE_DIR="${TMP_THEME_CACHE}" XDG_CACHE_HOME="${TMP_THEME_CACHE}" XDG_CONFIG_HOME="${TMP_THEME_CFG}" \
    "${BUILD_DIR}/wwal" theme "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --source-color-index 1
if [ -f "${TMP_THEME_CACHE}/wal/colors.json" ]; then
    echo "FAIL: pywal_compat = false still exported wal/colors.json"
    exit 1
fi
sed -i 's/pywal_compat = false/pywal_compat = true/' "${TMP_THEME_CFG}/wwal/config.toml"
WWAL_STATE_DIR="${TMP_THEME_STATE}" WWAL_CACHE_DIR="${TMP_THEME_CACHE}" XDG_CACHE_HOME="${TMP_THEME_CACHE}" XDG_CONFIG_HOME="${TMP_THEME_CFG}" \
    "${BUILD_DIR}/wwal" theme --seed-color "#ff5500"
grep -q "primary=#" "${TMP_THEME_CACHE}/rendered_tpl.out"
grep -q "primary_light=#" "${TMP_THEME_CACHE}/rendered_tpl.out"
grep -q "source=#ff5500" "${TMP_THEME_CACHE}/rendered_tpl.out"
grep -q "lighter=#" "${TMP_THEME_CACHE}/rendered_tpl.out"
grep -q "rgb_alpha=rgba(" "${TMP_THEME_CACHE}/rendered_tpl.out"
grep -q ", 0.80)" "${TMP_THEME_CACHE}/rendered_tpl.out"
grep -q "hsl_comp=#00ffff" "${TMP_THEME_CACHE}/rendered_tpl.out"
for _ in {1..20}; do
    [ -f "${TMP_THEME_CACHE}/tpl_hook_ok" ] && break
    sleep 0.01
done
test -f "${TMP_THEME_CACHE}/tpl_hook_ok"
test -f "${TMP_THEME_CACHE}/wal/colors.json"
test -f "${TMP_THEME_CACHE}/wal/colors"
test -f "${TMP_THEME_CACHE}/wal/sequences"
test -f "${TMP_THEME_CACHE}/wal/colors.sh"
test -f "${TMP_THEME_CACHE}/wal/colors.css"
rm -rf "${TMP_THEME_STATE}" "${TMP_THEME_CACHE}" "${TMP_THEME_CFG}"

echo "[34] Testing swww/awww v0.12.1 compatibility flags (--resize, --crop-gravity, --invert-y, --transition-pos, --no-cache, query -o)..."
# First set a known image with caching enabled so state-${NAMESPACE}.bin records test_pattern.png
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type none
grep -aq "test_pattern.png" "${WWAL_STATE_DIR}/state-${NAMESPACE}.bin"
# Copy test_pattern.png to a temporary path and apply with --no-cache; state-${NAMESPACE}.bin must NOT record nocache_wall.png
TMP_NOCACHE_IMG="${TMP_INTEG_CACHE}/nocache_wall.png"
cp "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" "${TMP_NOCACHE_IMG}"
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "${TMP_NOCACHE_IMG}" --resize crop --crop-gravity NE --invert-y --fill-color 1e1e2e --filter Lanczos3 --transition-type outer --transition-pos "75%, 25%" --transition-duration 0.05 --no-cache
if grep -aq "nocache_wall.png" "${WWAL_STATE_DIR}/state-${NAMESPACE}.bin"; then
    echo "FAIL: --no-cache overwrote state-${NAMESPACE}.bin with nocache_wall.png"
    exit 1
fi
# Verify animated solid-color transition via wwal img 0x1e1e2e + restore
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img 0x1e1e2e --transition-type grow --transition-duration 0.05
sleep 0.08
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: color: #1e1e2e"
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" restore
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: color: #1e1e2e"
# Verify query -o filtering
FILTERED_NONE=$("${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query -o "NONEXISTENT_OUTPUT_XYZ")
if [ -n "${FILTERED_NONE}" ]; then
    echo "FAIL: query -o NONEXISTENT_OUTPUT_XYZ returned non-empty output: ${FILTERED_NONE}"
    exit 1
fi

echo "[34a] Testing automatic daemon spawning on fresh namespace via query, multi-namespace stdin broadcast (-a), & clear-cache..."
AUTOSPAWN_NS="${NAMESPACE}_autospawn"
AUTOSPAWN_OUT=$("${BUILD_DIR}/wwal" --namespace "${AUTOSPAWN_NS}" query)
echo "${AUTOSPAWN_OUT}" | grep -q "currently displaying:"
cat "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" | "${BUILD_DIR}/wwal" img - -a --transition-type none --no-cache
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: <stdin>"
"${BUILD_DIR}/wwal" --namespace "${AUTOSPAWN_NS}" query | grep -q "currently displaying: image: <stdin>"
"${BUILD_DIR}/wwal" --namespace "${AUTOSPAWN_NS}" kill
mkdir -p "${WWAL_CACHE_DIR}/thumbs"
touch "${WWAL_CACHE_DIR}/thumbs/dummy.bgra"
"${BUILD_DIR}/wwal" clear-cache
if [ -f "${WWAL_CACHE_DIR}/thumbs/dummy.bgra" ]; then
    echo "FAIL: clear-cache did not remove thumbs/dummy.bgra"
    exit 1
fi

echo "[34b] Testing --scaling-mode span, universal wwal img auto-dispatch (.frag, .mp4), and cross-mode GPU transitions..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --scaling-mode span --crop-gravity NW --transition-type fade --transition-duration 0.05
TMP_INTEG_SHADER="${TMP_INTEG_CACHE}/integ_deriv_span.frag"
cat > "${TMP_INTEG_SHADER}" << 'EOF'
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = fragCoord / iResolution.xy;
    float edge = fwidth(uv.x) + abs(dFdx(uv.x)) + abs(dFdy(uv.y));
    fragColor = vec4(uv.x, uv.y, clamp(edge * 100.0, 0.0, 1.0), 1.0);
}
EOF
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "${TMP_INTEG_SHADER}" --scaling-mode span --transition-type wipe --transition-duration 0.05
sleep 0.08
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*integ_deriv_span.frag"
if [ -f "${VIDEO_PATH}" ]; then
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "${VIDEO_PATH}"
    sleep 0.08
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*test_video.mp4"
fi
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" --transition-type grow --transition-duration 0.05
sleep 0.08

echo "[34c] Testing per-output multi-engine concurrency (shader, video, slideshow isolation)..."
mapfile -t OUT_NAMES < <("${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | awk -F': ' '{print $2}')
if [ "${#OUT_NAMES[@]}" -ge 1 ]; then
    FIRST_OUT="${OUT_NAMES[0]}"
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query -o "${FIRST_OUT}" | grep -q ": ${FIRST_OUT}:"
    if "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow "$(dirname "${BASH_SOURCE[0]}")" -o "NONEXISTENT_OUTPUT_XYZ" 2>/dev/null; then
        echo "FAIL: slideshow targeting NONEXISTENT_OUTPUT_XYZ unexpectedly succeeded"
        exit 1
    fi
    if [ -f "${VIDEO_PATH}" ]; then
        if "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" video "${VIDEO_PATH}" -o "NONEXISTENT_OUTPUT_XYZ" 2>/dev/null; then
            echo "FAIL: video targeting NONEXISTENT_OUTPUT_XYZ unexpectedly succeeded"
            exit 1
        fi
    fi
fi
if [ "${#OUT_NAMES[@]}" -ge 2 ]; then
    OUT_A="${OUT_NAMES[0]}"
    OUT_B="${OUT_NAMES[1]}"
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow "$(dirname "${BASH_SOURCE[0]}")" -o "${OUT_A},${OUT_B}" -d 5m --no-recursive --transition-type none
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow stop -o "${OUT_A}"
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow stop -o "${OUT_A}"
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" next -o "${OUT_B}"
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" slideshow stop
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
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*test_pattern.png"

echo "[35a] Testing ecosystem cache sync (current_wallpaper, current_wallpaper.png), workshop sync-previews, & wwal pick..."
if [ ! -L "${WWAL_CACHE_DIR}/current_wallpaper" ] || [ ! -f "${WWAL_CACHE_DIR}/current_wallpaper.png" ]; then
    echo "FAIL: ecosystem cache files current_wallpaper / current_wallpaper.png missing in ${WWAL_CACHE_DIR}"
    exit 1
fi
PICK_DIR="${TMP_INTEG_CACHE}/picker_walls"
mkdir -p "${PICK_DIR}/sub1" "${PICK_DIR}/sub2"
cp "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" "${PICK_DIR}/alpha_wall.png"
cp "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" "${PICK_DIR}/sub1/dup_wall.png"
cp "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" "${PICK_DIR}/sub2/dup_wall.png"
cp "${TMP_INTEG_SHADER}" "${PICK_DIR}/beta_shader.frag"

MOCK_WS_ROOT="${TMP_INTEG_CACHE}/mock_workshop"
mkdir -p "${MOCK_WS_ROOT}/777888999"
cat > "${MOCK_WS_ROOT}/777888999/project.json" << 'EOF'
{"title":"Integ Workshop: Scene","type":"image","file":"preview.png","preview":"preview.png"}
EOF
cp "$(dirname "${BASH_SOURCE[0]}")/test_pattern.png" "${MOCK_WS_ROOT}/777888999/preview.png"
WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" "${BUILD_DIR}/wwal" workshop sync-previews "${PICK_DIR}/workshop" | grep -q "Synced 1 Workshop preview"
test -L "${PICK_DIR}/workshop/Integ Workshop Scene [777888999].png"
WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" "${BUILD_DIR}/wwal" theme "${MOCK_WS_ROOT}/777888999" | grep -q "Material You"

PICK_STDOUT_FILE="${TMP_INTEG_CACHE}/pick_stdout.bin"
WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" "${BUILD_DIR}/wwal" pick --menu stdout --dir "${PICK_DIR}" > "${PICK_STDOUT_FILE}"
grep -aq "alpha_wall.png" "${PICK_STDOUT_FILE}"
grep -aq "beta_shader.frag" "${PICK_STDOUT_FILE}"
grep -aq "dup_wall \[image - sub1/dup_wall.png\]" "${PICK_STDOUT_FILE}"
grep -aq "dup_wall \[image - sub2/dup_wall.png\]" "${PICK_STDOUT_FILE}"
grep -aq "Integ Workshop: Scene \[workshop\]" "${PICK_STDOUT_FILE}"

PICK_JSON=$(WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" "${BUILD_DIR}/wwal" pick --menu json --dir "${PICK_DIR}")
echo "${PICK_JSON}" | grep -q '"type":"image"'
echo "${PICK_JSON}" | grep -q '"type":"shader"'
echo "${PICK_JSON}" | grep -q '"type":"workshop"'
compgen -G "${WWAL_CACHE_DIR}/thumbs/*.png" >/dev/null

MOCK_BIN_DIR="${TMP_INTEG_CACHE}/mock_bin"
mkdir -p "${MOCK_BIN_DIR}"
cat > "${MOCK_BIN_DIR}/dmenu" << 'EOF'
#!/usr/bin/env bash
grep -m 1 "sub2/dup_wall.png"
EOF
chmod +x "${MOCK_BIN_DIR}/dmenu"
ROFI_INFO="${PICK_DIR}/alpha_wall.png" PATH="${MOCK_BIN_DIR}:${PATH}" WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" \
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" pick --menu dmenu --dir "${PICK_DIR}" --transition-type none
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*sub2/dup_wall.png"

cat > "${MOCK_BIN_DIR}/dmenu" << 'EOF'
#!/usr/bin/env bash
grep -m 1 "beta_shader"
EOF
chmod +x "${MOCK_BIN_DIR}/dmenu"
PATH="${MOCK_BIN_DIR}:${PATH}" WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" \
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" pick --menu dmenu --dir "${PICK_DIR}" --transition-type none
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*beta_shader.frag"
readlink "${WWAL_CACHE_DIR}/current_wallpaper" | grep -q "beta_shader.frag"

# Verify --dir isolation (does not leak global WWAL_WORKSHOP_DIR items when --dir points to sub1)
SUB1_JSON=$(WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" "${BUILD_DIR}/wwal" pick --menu json --dir "${PICK_DIR}/sub1")
if echo "${SUB1_JSON}" | grep -q "777888999"; then
    echo "FAIL: wwal pick --dir leaked global Workshop item outside --dir"
    exit 1
fi

# Verify walker receives plain-text lines without NUL/US metadata
WALKER_CAPTURE="${TMP_INTEG_CACHE}/walker_stdin.bin"
cat > "${MOCK_BIN_DIR}/walker" << EOF
#!/usr/bin/env bash
cat > "${WALKER_CAPTURE}"
grep -m 1 "alpha_wall" "${WALKER_CAPTURE}"
EOF
chmod +x "${MOCK_BIN_DIR}/walker"
PATH="${MOCK_BIN_DIR}:${PATH}" "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" pick --menu walker --dir "${PICK_DIR}" --transition-type none
if grep -Paq '\x00|\x1f' "${WALKER_CAPTURE}"; then
    echo "FAIL: walker menu stdin contained NUL or 0x1F metadata bytes"
    exit 1
fi

# Verify fuzzel receives icon\x1f<path>.png without info\x1f
FUZZEL_CAPTURE="${TMP_INTEG_CACHE}/fuzzel_stdin.bin"
cat > "${MOCK_BIN_DIR}/fuzzel" << EOF
#!/usr/bin/env bash
cat > "${FUZZEL_CAPTURE}"
printf "alpha_wall [image]\n"
EOF
chmod +x "${MOCK_BIN_DIR}/fuzzel"
PATH="${MOCK_BIN_DIR}:${PATH}" "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" pick --menu fuzzel --dir "${PICK_DIR}" --transition-type none
if grep -aq $'info\x1f' "${FUZZEL_CAPTURE}"; then
    echo "FAIL: fuzzel menu stdin contained unsupported info\\x1f metadata"
    exit 1
fi
grep -aq $'icon\x1f' "${FUZZEL_CAPTURE}"

# Verify WAYWAL_REQ_CLEAR updates current_wallpaper.png (64x64 solid-color PNG) and current_wallpaper symlink
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" clear 2a3b4cff
test -f "${WWAL_CACHE_DIR}/current_wallpaper.png"
readlink "${WWAL_CACHE_DIR}/current_wallpaper" | grep -q "current_wallpaper.png"

# Verify --no-cache privacy bypass across shader, workshop, and video
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" img "${PICK_DIR}/alpha_wall.png" --transition-type none
readlink "${WWAL_CACHE_DIR}/current_wallpaper" | grep -q "alpha_wall.png"

PRIVATE_SHADER="${TMP_INTEG_CACHE}/private_nocache.frag"
cp "${PICK_DIR}/beta_shader.frag" "${PRIVATE_SHADER}"
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" shader "${PRIVATE_SHADER}" --no-cache --transition-type none
readlink "${WWAL_CACHE_DIR}/current_wallpaper" | grep -q "alpha_wall.png"
if grep -aq "private_nocache.frag" "${WWAL_STATE_DIR}/state-${NAMESPACE}.bin"; then
    echo "FAIL: shader --no-cache leaked private_nocache.frag into state-${NAMESPACE}.bin"
    exit 1
fi

mkdir -p "${MOCK_WS_ROOT}/999111222"
cp "${PICK_DIR}/alpha_wall.png" "${MOCK_WS_ROOT}/999111222/preview.png"
cat > "${MOCK_WS_ROOT}/999111222/project.json" << 'EOF'
{
  "title": "Private NoCache Workshop",
  "type": "image",
  "file": "preview.png",
  "preview": "preview.png"
}
EOF
WWAL_WORKSHOP_DIR="${MOCK_WS_ROOT}" "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" workshop 999111222 --no-cache --transition-type none
readlink "${WWAL_CACHE_DIR}/current_wallpaper" | grep -q "alpha_wall.png"
if grep -aq "999111222" "${WWAL_STATE_DIR}/state-${NAMESPACE}.bin"; then
    echo "FAIL: workshop --no-cache leaked 999111222 into state-${NAMESPACE}.bin"
    exit 1
fi

if [ -f "${VIDEO_PATH}" ]; then
    PRIVATE_VIDEO="${TMP_INTEG_CACHE}/private_nocache.mp4"
    cp "${VIDEO_PATH}" "${PRIVATE_VIDEO}"
    "${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" video "${PRIVATE_VIDEO}" --no-cache
    readlink "${WWAL_CACHE_DIR}/current_wallpaper" | grep -q "alpha_wall.png"
    if grep -aq "private_nocache.mp4" "${WWAL_STATE_DIR}/state-${NAMESPACE}.bin"; then
        echo "FAIL: video --no-cache leaked private_nocache.mp4 into state-${NAMESPACE}.bin"
        exit 1
    fi
fi

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

echo "[35c] Testing curated built-in shader presets (shader list, shader preset, theme --img, top-level preset)..."
SHADER_LIST_OUT=$("${BUILD_DIR}/wwal" shader list)
echo "${SHADER_LIST_OUT}" | grep -q "Built-in Procedural Shader Presets (10):"
echo "${SHADER_LIST_OUT}" | grep -q "aurora"
echo "${SHADER_LIST_OUT}" | grep -q "plasma_orb"
SHADER_LIST_JSON=$("${BUILD_DIR}/wwal" shader list --json)
echo "${SHADER_LIST_JSON}" | grep -q '"name":"aurora"'
echo "${SHADER_LIST_JSON}" | grep -q '"author":"WayWal"'
echo "${SHADER_LIST_JSON}" | grep -q '"is_builtin":true'
"${BUILD_DIR}/wwal" theme --img aurora | grep -q "Material You Palette"
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" shader preset lava_lamp --transition-type none
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*plasma_orb.frag"
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" aurora --transition-type none
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" query | grep -q "currently displaying: image: .*aurora.frag"

# 8. Terminate daemon
echo "[36] Testing kill command..."
"${BUILD_DIR}/wwal" --namespace "${NAMESPACE}" kill

# Wait for daemon process to terminate
wait "${DAEMON_PID}" || true
rm -rf "${TMP_INTEG_STATE}" "${TMP_INTEG_CACHE}"
trap - EXIT

echo "=== Integration Test PASSED Successfully! ==="

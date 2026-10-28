#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Empirical benchmark harness for WayWal (wwald / wwal).
Uses an isolated socket namespace (-n bench_<pid>) so it never interferes with
or terminates a running desktop wwald instance.

Measures:
  1. Binary sizes, zero-overhead DT_NEEDED audit, and idle daemon VmRSS/VmHWM/PSS & VRAM
  2. 4K (3840x2160) 18-transition GPU compute & AVX2 CPU fallback throughput + live daemon load
  3. Dual-mode (dark + light) OKLab/HCT palette extraction + 7-filter template engine latency
  4. Unified `wwal pick --menu json` discovery & thumbnail cache latency (cold vs. warm)
  5. Zero-Watt occluded/paused 0.00% CPU verification & lockscreen 1280x720 PNG export latency
"""

from __future__ import annotations

import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from typing import Any

import psutil

WAYWAL_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BUILD_DIR = os.environ.get("WWAL_BUILD_DIR", f"{WAYWAL_DIR}/build")
WAYWAL_DAEMON = os.environ.get("WWAL_DAEMON_BIN", f"{BUILD_DIR}/wwald")
WAYWAL_CLIENT = os.environ.get("WWAL_CLIENT_BIN", f"{BUILD_DIR}/wwal")
BENCH_TRANSITIONS = os.environ.get("WWAL_BENCH_BIN", f"{BUILD_DIR}/bench_transitions")

DEFAULT_IMG = f"{WAYWAL_DIR}/tests/test_pattern.png"
IMG1 = os.environ.get("WWAL_BENCH_IMG1", DEFAULT_IMG)
IMG2 = os.environ.get("WWAL_BENCH_IMG2", DEFAULT_IMG)

_GPU_BUSY_PATHS = sorted(glob.glob("/sys/class/drm/card*/device/gpu_busy_percent"))
_VRAM_USED_PATHS = sorted(glob.glob("/sys/class/drm/card*/device/mem_info_vram_used"))
_HAS_NVIDIA_SMI = shutil.which("nvidia-smi") is not None

FORBIDDEN_DT_NEEDED = (
    "libavcodec",
    "libavformat",
    "libavutil",
    "libswscale",
    "libwebp",
    "libavif",
    "libjxl",
    "librsvg",
    "libpipewire",
)


def get_gpu_busy() -> int:
    for path in _GPU_BUSY_PATHS:
        try:
            with open(path, "r", encoding="utf-8") as f:
                return int(f.read().strip())
        except Exception:
            continue
    if _HAS_NVIDIA_SMI:
        try:
            out = subprocess.check_output(
                ["nvidia-smi", "--query-gpu=utilization.gpu", "--format=csv,noheader,nounits"],
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=0.5,
            )
            return int(out.strip().splitlines()[0])
        except Exception:
            pass
    return 0


def get_vram_mb() -> float:
    for path in _VRAM_USED_PATHS:
        try:
            with open(path, "r", encoding="utf-8") as f:
                return int(f.read().strip()) / (1024 * 1024)
        except Exception:
            continue
    if _HAS_NVIDIA_SMI:
        try:
            out = subprocess.check_output(
                ["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=0.5,
            )
            return float(out.strip().splitlines()[0])
        except Exception:
            pass
    return 0.0


def read_proc_status_kb(pid: int) -> dict[str, int]:
    info: dict[str, int] = {"VmRSS": 0, "VmHWM": 0, "Pss": 0}
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    info["VmRSS"] = int(line.split()[1])
                elif line.startswith("VmHWM:"):
                    info["VmHWM"] = int(line.split()[1])
    except Exception:
        pass
    try:
        with open(f"/proc/{pid}/smaps_rollup", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("Pss:"):
                    info["Pss"] = int(line.split()[1])
                    break
    except Exception:
        info["Pss"] = info["VmRSS"]
    return info


def get_proc_stats(proc: psutil.Process) -> dict[str, float] | None:
    try:
        mem = proc.memory_info()
        cpu_times = proc.cpu_times()
        status_kb = read_proc_status_kb(proc.pid)
        vmrss_mb = status_kb["VmRSS"] / 1024.0 if status_kb["VmRSS"] > 0 else mem.rss / (1024 * 1024)
        vmhwm_mb = status_kb["VmHWM"] / 1024.0 if status_kb["VmHWM"] > 0 else vmrss_mb
        pss_mb = status_kb["Pss"] / 1024.0 if status_kb["Pss"] > 0 else vmrss_mb
        return {
            "rss_mb": vmrss_mb,
            "hwm_mb": vmhwm_mb,
            "pss_mb": pss_mb,
            "vms_mb": mem.vms / (1024 * 1024),
            "cpu_user_s": float(cpu_times.user),
            "cpu_sys_s": float(cpu_times.system),
            "cpu_total_s": float(cpu_times.user + cpu_times.system),
        }
    except Exception:
        return None


def audit_dt_needed(binary_path: str) -> tuple[list[str], list[str]]:
    if not os.path.exists(binary_path) or shutil.which("readelf") is None:
        return [], []
    out = subprocess.check_output(["readelf", "-d", binary_path], text=True, stderr=subprocess.DEVNULL)
    needed: list[str] = []
    for line in out.splitlines():
        if "(NEEDED)" in line and "[" in line and "]" in line:
            lib = line.split("[", 1)[1].split("]", 1)[0]
            needed.append(lib)
    violations = [lib for lib in needed if any(lib.startswith(bad) for bad in FORBIDDEN_DT_NEEDED)]
    return needed, violations


def query_daemon_stats(namespace: str, env: dict[str, str]) -> dict[str, Any]:
    try:
        out = subprocess.check_output(
            [WAYWAL_CLIENT, "-n", namespace, "stats", "--json"],
            env=env,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2.0,
        )
        return json.loads(out)
    except Exception:
        return {}


def benchmark_pick_cache() -> None:
    print("\n[4] UNIFIED PICKER (`wwal pick --menu json`) DISCOVERY & CACHE LATENCY:")
    if not os.path.exists(WAYWAL_CLIENT):
        print("  [SKIP] Client binary not found.")
        return

    with tempfile.TemporaryDirectory(prefix="wwal_bench_pick_") as tmp_root:
        wall_dir = os.path.join(tmp_root, "wallpapers")
        cache_dir = os.path.join(tmp_root, "cache")
        os.makedirs(wall_dir, exist_ok=True)
        os.makedirs(cache_dir, exist_ok=True)

        if os.path.exists(DEFAULT_IMG):
            for i in range(10):
                shutil.copyfile(DEFAULT_IMG, os.path.join(wall_dir, f"wall_{i:02d}.png"))

        test_vid = os.path.join(WAYWAL_DIR, "tests", "test_video.mp4")
        if os.path.exists(test_vid):
            shutil.copyfile(test_vid, os.path.join(wall_dir, "sample_loop.mp4"))

        for sh in ("plasma.frag", "audio_spectrum.frag"):
            sh_path = os.path.join(WAYWAL_DIR, "contrib", "shaders", sh)
            if os.path.exists(sh_path):
                shutil.copyfile(sh_path, os.path.join(wall_dir, sh))

        ws_dir = os.path.join(wall_dir, "2841902341")
        os.makedirs(ws_dir, exist_ok=True)
        with open(os.path.join(ws_dir, "project.json"), "w", encoding="utf-8") as f:
            json.dump({"title": "Cyberpunk Neon Scene", "type": "scene", "file": "scene.json"}, f)
        if os.path.exists(DEFAULT_IMG):
            shutil.copyfile(DEFAULT_IMG, os.path.join(ws_dir, "preview.png"))

        env = os.environ.copy()
        env["WWAL_CACHE_DIR"] = cache_dir
        env["WWAL_WORKSHOP_DIR"] = wall_dir

        t0 = time.perf_counter()
        subprocess.run(
            [WAYWAL_CLIENT, "pick", "--dir", wall_dir, "--menu", "json"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
            timeout=5.0,
        )
        cold_ms = (time.perf_counter() - t0) * 1000.0

        warm_samples: list[float] = []
        for _ in range(20):
            tw0 = time.perf_counter()
            subprocess.run(
                [WAYWAL_CLIENT, "pick", "--dir", wall_dir, "--menu", "json"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
                timeout=5.0,
            )
            warm_samples.append((time.perf_counter() - tw0) * 1000.0)

        warm_samples.sort()
        warm_mean = sum(warm_samples) / len(warm_samples)
        warm_min = warm_samples[0]
        warm_p99 = warm_samples[-1]
        print(f"  Cold Cache (14 mixed items + thumbnail gen): {cold_ms:.2f} ms")
        print(
            f"  Warm Cache (20 iters): Mean {warm_mean:.2f} ms | "
            f"Min {warm_min:.2f} ms | Max {warm_p99:.2f} ms"
        )


def get_stripped_size_kb(binary_path: str) -> tuple[float, float]:
    if not os.path.exists(binary_path):
        return 0.0, 0.0
    raw_kb = os.path.getsize(binary_path) / 1024.0
    if shutil.which("strip") is None:
        return raw_kb, raw_kb
    with tempfile.NamedTemporaryFile(prefix="wwal_strip_", delete=True) as tmp:
        shutil.copyfile(binary_path, tmp.name)
        subprocess.run(
            ["strip", "--strip-unneeded", tmp.name],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
        stripped_kb = os.path.getsize(tmp.name) / 1024.0
    return stripped_kb, raw_kb


def run_bench() -> None:
    print("=========================================================================")
    print("  WayWal (wwald / wwal) Empirical Benchmark Harness                      ")
    print("=========================================================================")

    d_strip_kb, d_raw_kb = get_stripped_size_kb(WAYWAL_DAEMON)
    c_strip_kb, c_raw_kb = get_stripped_size_kb(WAYWAL_CLIENT)
    needed, violations = audit_dt_needed(WAYWAL_DAEMON)

    print("\n[1] BINARY SIZES & ZERO-OVERHEAD DT_NEEDED AUDIT:")
    print(f"  Daemon (wwald): {d_strip_kb:.1f} KB stripped ({d_raw_kb:.1f} KB unstripped)")
    print(f"  Client (wwal):  {c_strip_kb:.1f} KB stripped ({c_raw_kb:.1f} KB unstripped)")
    print(f"  Total Stripped Footprint: {(d_strip_kb + c_strip_kb):.1f} KB")
    print(
        f"  wwald DT_NEEDED ({len(needed)} core libs, {len(violations)} heavy violations): "
        f"{'PASS' if not violations else 'FAIL: ' + ', '.join(violations)}"
    )

    frames_arg = "60"
    for idx, arg in enumerate(sys.argv[1:], start=1):
        if arg == "--frames" and idx < len(sys.argv) - 1:
            frames_arg = sys.argv[idx + 1]
        elif arg.startswith("--frames="):
            frames_arg = arg.split("=", 1)[1]

    if os.path.exists(BENCH_TRANSITIONS):
        print("\n[2 & 3] IN-PROCESS 4K TRANSITIONS, DUAL-MODE PALETTE & LOCKSCREEN PNG BENCHMARK:")
        sys.stdout.flush()
        env_omp = os.environ.copy()
        env_omp.setdefault("OMP_NUM_THREADS", "4")
        subprocess.run([BENCH_TRANSITIONS, frames_arg], env=env_omp, check=True)

    benchmark_pick_cache()

    print("\n[5] ISOLATED LIVE DAEMON RSS/VRAM, TRANSITION & ZERO-WATT OCCLUSION BENCHMARK:")
    if not os.environ.get("WAYLAND_DISPLAY"):
        print("  [SKIP] WAYLAND_DISPLAY not set; skipping live Wayland daemon session test.")
        return

    namespace = f"bench_{os.getpid()}"
    with tempfile.TemporaryDirectory(prefix="wwal_bench_state_") as state_root:
        env = os.environ.copy()
        env["WWAL_CACHE_DIR"] = os.path.join(state_root, "cache")
        env["WWAL_STATE_DIR"] = os.path.join(state_root, "state")
        os.makedirs(env["WWAL_CACHE_DIR"], exist_ok=True)
        os.makedirs(env["WWAL_STATE_DIR"], exist_ok=True)

        vram_before_mb = get_vram_mb()
        p = subprocess.Popen(
            [WAYWAL_DAEMON, "-n", namespace, "--no-daemon"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(0.5)
            if p.poll() is not None:
                print("  [SKIP] Isolated wwald exited early (Wayland compositor unavailable).")
                return

            ps_proc = psutil.Process(p.pid)
            subprocess.run(
                [WAYWAL_CLIENT, "-n", namespace, "img", IMG1, "--transition-type", "none"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                timeout=5.0,
            )
            time.sleep(0.4)

            idle_t0 = get_proc_stats(ps_proc)
            time.sleep(2.0)
            idle_t1 = get_proc_stats(ps_proc)
            vram_after_mb = get_vram_mb()
            d_stats = query_daemon_stats(namespace, env)

            if idle_t0 and idle_t1:
                idle_cpu_pct = ((idle_t1["cpu_total_s"] - idle_t0["cpu_total_s"]) / 2.0) * 100.0
                outputs = d_stats.get("outputs", [])
                bo_count = int(d_stats.get("vram_bo_count", len(outputs) * 3))
                pool_bytes = int(
                    d_stats.get(
                        "vram_pool_bytes",
                        sum(int(o.get("width", 0)) * int(o.get("height", 0)) * 4 * 3 for o in outputs),
                    )
                )
                vram_pool_mb = pool_bytes / (1024 * 1024)
                vram_delta_mb = max(0.0, vram_after_mb - vram_before_mb)
                print(
                    f"  Isolated Daemon (-n {namespace}): "
                    f"VmRSS: {idle_t1['rss_mb']:.2f} MB | PSS: {idle_t1['pss_mb']:.2f} MB | "
                    f"VmHWM: {idle_t1['hwm_mb']:.2f} MB | Idle CPU: {idle_cpu_pct:.2f}%"
                )
                print(
                    f"  VRAM Pool: {vram_pool_mb:.2f} MB "
                    f"(BO count: {bo_count}, Outputs: {len(outputs)}, "
                    f"DMA-BUF: {d_stats.get('dmabuf_scanout', False)}, GPU Delta: {vram_delta_mb:.2f} MB)"
                )

            transitions = ["fade", "wipe", "wave", "grow"]
            curr_img, next_img = IMG2, IMG1
            for t in transitions:
                dur = 0.6
                t_start = get_proc_stats(ps_proc)
                gpu_samples: list[int] = []
                t0 = time.perf_counter()

                subprocess.run(
                    [
                        WAYWAL_CLIENT,
                        "-n",
                        namespace,
                        "img",
                        curr_img,
                        "--transition-type",
                        t,
                        "--transition-duration",
                        str(dur),
                        "--transition-fps",
                        "60",
                    ],
                    env=env,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                    check=False,
                    timeout=5.0,
                )

                sample_deadline = t0 + dur + 0.1
                while time.perf_counter() < sample_deadline:
                    gpu_samples.append(get_gpu_busy())
                    time.sleep(0.04)

                t_end = get_proc_stats(ps_proc)
                if t_start and t_end:
                    cpu_ms = (t_end["cpu_total_s"] - t_start["cpu_total_s"]) * 1000.0
                    avg_gpu = sum(gpu_samples) / len(gpu_samples) if gpu_samples else 0.0
                    print(
                        f"  Transition '{t}': Duration: {dur}s | Daemon CPU: {cpu_ms:.1f} ms | "
                        f"VmRSS: {t_end['rss_mb']:.2f} MB | Avg GPU: {avg_gpu:.1f}%"
                    )
                curr_img, next_img = next_img, curr_img
                time.sleep(0.2)

            # Verify Zero-Watt pause / occlusion 0.00% CPU
            subprocess.run(
                [WAYWAL_CLIENT, "-n", namespace, "pause"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                timeout=5.0,
            )
            time.sleep(0.2)
            occ_t0 = get_proc_stats(ps_proc)
            time.sleep(2.0)
            occ_t1 = get_proc_stats(ps_proc)
            if occ_t0 and occ_t1:
                occ_cpu_pct = ((occ_t1["cpu_total_s"] - occ_t0["cpu_total_s"]) / 2.0) * 100.0
                print(
                    f"  Zero-Watt Occluded / Paused State (2.0s window): "
                    f"CPU: {occ_cpu_pct:.2f}% (Target: 0.00%)"
                )
        finally:
            if p.poll() is None:
                try:
                    subprocess.run(
                        [WAYWAL_CLIENT, "-n", namespace, "kill"],
                        env=env,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        check=False,
                        timeout=2.0,
                    )
                except subprocess.TimeoutExpired:
                    pass
            try:
                p.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait(timeout=2.0)


if __name__ == "__main__":
    run_bench()

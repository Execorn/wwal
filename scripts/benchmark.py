#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Runtime transition and idle resource benchmark for WayWal (wwald / wwal).
Measures binary sizes, idle RSS and CPU usage, and per-transition daemon CPU/GPU load.
"""

import os
import time
import subprocess
import psutil

WAYWAL_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
WAYWAL_DAEMON = os.environ.get("WWAL_DAEMON_BIN", f"{WAYWAL_DIR}/build/wwald")
WAYWAL_CLIENT = os.environ.get("WWAL_CLIENT_BIN", f"{WAYWAL_DIR}/build/wwal")

DEFAULT_IMG = f"{WAYWAL_DIR}/tests/test_pattern.png"
IMG1 = os.environ.get("WWAL_BENCH_IMG1", DEFAULT_IMG)
IMG2 = os.environ.get("WWAL_BENCH_IMG2", DEFAULT_IMG)

SYSFS_GPU_BUSY = "/sys/class/drm/card1/device/gpu_busy_percent"
SYSFS_VRAM_USED = "/sys/class/drm/card1/device/mem_info_vram_used"


def get_gpu_busy():
    try:
        with open(SYSFS_GPU_BUSY, "r", encoding="utf-8") as f:
            return int(f.read().strip())
    except Exception:
        return 0


def get_vram_mb():
    try:
        with open(SYSFS_VRAM_USED, "r", encoding="utf-8") as f:
            return int(f.read().strip()) / (1024 * 1024)
    except Exception:
        return 0.0


def get_proc_stats(proc):
    try:
        mem = proc.memory_info()
        cpu_times = proc.cpu_times()
        return {
            "rss_mb": mem.rss / (1024 * 1024),
            "vms_mb": mem.vms / (1024 * 1024),
            "cpu_user_s": cpu_times.user,
            "cpu_sys_s": cpu_times.system,
            "cpu_total_s": cpu_times.user + cpu_times.system,
        }
    except Exception:
        return None


def run_bench():
    print("=================================================================")
    print("  WayWal (wwald / wwal) Live Transition & Resource Benchmark     ")
    print("=================================================================")

    waywal_d_size = os.path.getsize(WAYWAL_DAEMON) / 1024 if os.path.exists(WAYWAL_DAEMON) else 0
    waywal_c_size = os.path.getsize(WAYWAL_CLIENT) / 1024 if os.path.exists(WAYWAL_CLIENT) else 0

    print("\n[1] BINARY SIZES:")
    print(f"  Daemon (wwald): {waywal_d_size:.1f} KB")
    print(f"  Client (wwal):  {waywal_c_size:.1f} KB")

    transitions = ["fade", "wipe", "wave", "grow"]

    print("\n[2] BENCHMARKING WayWal...")
    p = subprocess.Popen([WAYWAL_DAEMON], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    ps_proc = psutil.Process(p.pid)

    idle_start_cpu = get_proc_stats(ps_proc)["cpu_total_s"]
    idle_vram_start = get_vram_mb()
    time.sleep(2.0)
    idle_stats = get_proc_stats(ps_proc)
    idle_cpu_delta = (idle_stats["cpu_total_s"] - idle_start_cpu) / 2.0
    waywal_idle = {
        "rss_mb": idle_stats["rss_mb"],
        "cpu_usage_pct": idle_cpu_delta * 100.0,
        "vram_mb": get_vram_mb() - idle_vram_start,
    }
    print(f"  Idle RSS: {waywal_idle['rss_mb']:.2f} MB | Idle CPU: {waywal_idle['cpu_usage_pct']:.2f}%")

    subprocess.run([WAYWAL_CLIENT, "img", IMG1, "--transition-type", "none"], check=True)
    time.sleep(0.5)

    curr_img = IMG2
    next_img = IMG1

    for t in transitions:
        dur = 0.6
        t_start_stats = get_proc_stats(ps_proc)
        gpu_samples = []
        t0 = time.perf_counter()

        subprocess.run(
            [
                WAYWAL_CLIENT,
                "img",
                curr_img,
                "--transition-type",
                t,
                "--transition-duration",
                str(dur),
                "--transition-fps",
                "60",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )

        sample_deadline = t0 + dur + 0.1
        while time.perf_counter() < sample_deadline:
            gpu_samples.append(get_gpu_busy())
            time.sleep(0.04)

        t_end_stats = get_proc_stats(ps_proc)

        cpu_time = (t_end_stats["cpu_total_s"] - t_start_stats["cpu_total_s"]) * 1000.0
        avg_gpu = sum(gpu_samples) / len(gpu_samples) if gpu_samples else 0
        peak_rss = t_end_stats["rss_mb"]

        print(
            f"  Transition '{t}': Duration: {dur}s | Daemon CPU: {cpu_time:.1f}ms | "
            f"Peak RSS: {peak_rss:.2f}MB | Avg GPU: {avg_gpu:.1f}%"
        )

        curr_img, next_img = next_img, curr_img
        time.sleep(0.2)

    subprocess.run([WAYWAL_CLIENT, "kill"], check=True)
    p.wait(timeout=2)


if __name__ == "__main__":
    run_bench()

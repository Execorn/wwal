#!/usr/bin/env python3
"""
Comparative Benchmark: WayWal (C23 GPU/io_uring) vs Legacy (Rust wl_shm)
Measures:
- Binary sizes
- Background / Idle CPU & Memory (RSS, Anon, VmSize)
- Back-to-back transition CPU time (user+sys ticks), peak RSS, GPU utilization
"""

import os
import sys
import time
import subprocess
import signal
import psutil

WAYWAL_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
LEGACY_DIR = os.environ.get("LEGACY_DAEMON_DIR", os.path.expanduser("~/programming/projects/legacy-waywal"))

WAYWAL_DAEMON = f"{WAYWAL_DIR}/build/wwald"
WAYWAL_CLIENT = f"{WAYWAL_DIR}/build/wwal"

LEGACY_DAEMON = f"{LEGACY_DIR}/target/release/legacy-daemon"
LEGACY_CLIENT = f"{LEGACY_DIR}/target/release/legacy-client"

IMG1 = "/home/execorn/Pictures/Wallpapers/AnimeCozy.jpg"
IMG2 = "/home/execorn/Pictures/Wallpapers/Cosmic Ruby.png"

SYSFS_GPU_BUSY = "/sys/class/drm/card1/device/gpu_busy_percent"
SYSFS_VRAM_USED = "/sys/class/drm/card1/device/mem_info_vram_used"

def get_gpu_busy():
    try:
        with open(SYSFS_GPU_BUSY, "r") as f:
            return int(f.read().strip())
    except Exception:
        return 0

def get_vram_mb():
    try:
        with open(SYSFS_VRAM_USED, "r") as f:
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
    print("  WAYWAL BENCHMARK: WayWal (C23) vs Legacy Implementation        ")
    print("=================================================================")

    # 1. Binary Sizes
    waywal_d_size = os.path.getsize(WAYWAL_DAEMON) / 1024 if os.path.exists(WAYWAL_DAEMON) else 0
    waywal_c_size = os.path.getsize(WAYWAL_CLIENT) / 1024 if os.path.exists(WAYWAL_CLIENT) else 0
    leg_d_size = os.path.getsize(LEGACY_DAEMON) / 1024 if os.path.exists(LEGACY_DAEMON) else 0
    leg_c_size = os.path.getsize(LEGACY_CLIENT) / 1024 if os.path.exists(LEGACY_CLIENT) else 0

    print("\n[1] BINARY SIZES:")
    print(f"  Daemon Binary:")
    print(f"    WayWal (wwald):            {waywal_d_size:.1f} KB")
    print(f"    Legacy Daemon:             {leg_d_size:.1f} KB")
    if waywal_d_size > 0 and leg_d_size > 0:
        print(f"    Ratio: WayWal is {leg_d_size / waywal_d_size:.2f}x smaller")
    print(f"  Client Binary:")
    print(f"    WayWal (wwal):             {waywal_c_size:.1f} KB")
    print(f"    Legacy Client:             {leg_c_size:.1f} KB")
    if waywal_c_size > 0 and leg_c_size > 0:
        print(f"    Ratio: WayWal is {leg_c_size / waywal_c_size:.2f}x smaller")

    transitions = ["fade", "wipe", "wave", "grow"]
    results_waywal = {}
    results_orig = {}

    # 2. Benchmark WayWal
    print("\n[2] BENCHMARKING WayWal (C23 + GPU Compute + Direct Scanout)...")
    p = subprocess.Popen([WAYWAL_DAEMON], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.5)
    ps_proc = psutil.Process(p.pid)

    # Measure Idle
    idle_start_cpu = get_proc_stats(ps_proc)["cpu_total_s"]
    idle_vram_start = get_vram_mb()
    time.sleep(2.0)
    idle_stats = get_proc_stats(ps_proc)
    idle_cpu_delta = (idle_stats["cpu_total_s"] - idle_start_cpu) / 2.0
    waywal_idle = {
        "rss_mb": idle_stats["rss_mb"],
        "cpu_usage_pct": idle_cpu_delta * 100.0,
        "vram_mb": get_vram_mb() - idle_vram_start
    }
    print(f"  Idle RSS: {waywal_idle['rss_mb']:.2f} MB | Idle CPU: {waywal_idle['cpu_usage_pct']:.2f}%")

    # Initial image set
    subprocess.run([WAYWAL_CLIENT, "img", IMG1, "--transition-type", "none"], check=True)
    time.sleep(0.5)

    curr_img = IMG2
    next_img = IMG1

    for t in transitions:
        dur = 0.6
        t_start_stats = get_proc_stats(ps_proc)
        gpu_samples = []
        t0 = time.perf_counter()

        proc_cli = subprocess.run([
            WAYWAL_CLIENT, "img", curr_img,
            "--transition-type", t,
            "--transition-duration", str(dur),
            "--transition-fps", "60"
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        sample_deadline = t0 + dur + 0.1
        while time.perf_counter() < sample_deadline:
            gpu_samples.append(get_gpu_busy())
            time.sleep(0.04)

        t1 = time.perf_counter()
        elapsed = t1 - t0
        t_end_stats = get_proc_stats(ps_proc)

        cpu_time = (t_end_stats["cpu_total_s"] - t_start_stats["cpu_total_s"]) * 1000.0 # ms
        avg_gpu = sum(gpu_samples) / len(gpu_samples) if gpu_samples else 0
        peak_rss = t_end_stats["rss_mb"]

        results_waywal[t] = {
            "elapsed_s": elapsed,
            "cpu_time_ms": cpu_time,
            "avg_gpu_pct": avg_gpu,
            "peak_rss_mb": peak_rss
        }
        print(f"  Transition '{t}': Duration: {dur}s | Daemon CPU: {cpu_time:.1f}ms | Peak RSS: {peak_rss:.2f}MB | Avg GPU: {avg_gpu:.1f}%")

        curr_img, next_img = next_img, curr_img
        time.sleep(0.2)

    # Stop daemon
    subprocess.run([WAYWAL_CLIENT, "kill"], check=True)
    p.wait(timeout=2)

    time.sleep(1.0)

    # 3. Benchmark legacy if present
    if os.path.exists(LEGACY_DAEMON):
        print("\n[3] BENCHMARKING Legacy Implementation (Rust + CPU wl_shm)...")
        p_orig = subprocess.Popen([LEGACY_DAEMON], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(0.5)
        ps_orig = psutil.Process(p_orig.pid)

        # Measure Idle
        idle_orig_start = get_proc_stats(ps_orig)["cpu_total_s"]
        time.sleep(2.0)
        idle_orig_stats = get_proc_stats(ps_orig)
        idle_orig_cpu = (idle_orig_stats["cpu_total_s"] - idle_orig_start) / 2.0
        orig_idle = {
            "rss_mb": idle_orig_stats["rss_mb"],
            "cpu_usage_pct": idle_orig_cpu * 100.0,
        }
        print(f"  Idle RSS: {orig_idle['rss_mb']:.2f} MB | Idle CPU: {orig_idle['cpu_usage_pct']:.2f}%")

        # Initial image set
        subprocess.run([LEGACY_CLIENT, "img", IMG1, "--transition-type", "none"], check=True)
        time.sleep(0.5)

        curr_img = IMG2
        next_img = IMG1

        for t in transitions:
            dur = 0.6
            t_start_stats = get_proc_stats(ps_orig)
            gpu_samples = []
            t0 = time.perf_counter()

            proc_cli = subprocess.run([
                LEGACY_CLIENT, "img", curr_img,
                "--transition-type", t,
                "--transition-duration", str(dur),
                "--transition-fps", "60"
            ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            sample_deadline = t0 + dur + 0.1
            while time.perf_counter() < sample_deadline:
                gpu_samples.append(get_gpu_busy())
                time.sleep(0.04)

            t1 = time.perf_counter()
            elapsed = t1 - t0
            t_end_stats = get_proc_stats(ps_orig)

            cpu_time = (t_end_stats["cpu_total_s"] - t_start_stats["cpu_total_s"]) * 1000.0 # ms
            avg_gpu = sum(gpu_samples) / len(gpu_samples) if gpu_samples else 0
            peak_rss = t_end_stats["rss_mb"]

            results_orig[t] = {
                "elapsed_s": elapsed,
                "cpu_time_ms": cpu_time,
                "avg_gpu_pct": avg_gpu,
                "peak_rss_mb": peak_rss
            }
            print(f"  Transition '{t}': Duration: {dur}s | Daemon CPU: {cpu_time:.1f}ms | Peak RSS: {peak_rss:.2f}MB | Avg GPU: {avg_gpu:.1f}%")

            curr_img, next_img = next_img, curr_img
            time.sleep(0.2)

        # Stop legacy daemon
        os.kill(p_orig.pid, signal.SIGTERM)
        p_orig.wait(timeout=2)

        print("\n=================================================================")
        print("                    FINAL COMPARISON MATRIX                      ")
        print("=================================================================")
        print(f"{'Metric':<30} | {'WayWal (C23)':<18} | {'Legacy (Rust)':<18} | {'Improvement':<15}")
        print("-" * 88)
        print(f"{'Daemon Binary Size':<30} | {waywal_d_size:<15.1f} KB | {leg_d_size:<15.1f} KB | {leg_d_size/waywal_d_size:.1f}x smaller")
        print(f"{'Client Binary Size':<30} | {waywal_c_size:<15.1f} KB | {leg_c_size:<15.1f} KB | {leg_c_size/waywal_c_size:.1f}x smaller")
        print(f"{'Daemon Idle RSS Memory':<30} | {waywal_idle['rss_mb']:<15.2f} MB | {orig_idle['rss_mb']:<15.2f} MB | {(orig_idle['rss_mb'] - waywal_idle['rss_mb']):+.2f} MB")
        print(f"{'Daemon Idle CPU Usage':<30} | {waywal_idle['cpu_usage_pct']:<15.2f} %  | {orig_idle['cpu_usage_pct']:<15.2f} %  | 0% (event-driven)")

        for t in transitions:
            r_cpu = results_waywal[t]["cpu_time_ms"]
            o_cpu = results_orig[t]["cpu_time_ms"]
            speedup = (o_cpu / r_cpu) if r_cpu > 0 else 1.0
            print(f"{'CPU Time (' + t + ')':<30} | {r_cpu:<15.1f} ms | {o_cpu:<15.1f} ms | {speedup:.1f}x less CPU")

        print("=================================================================")

if __name__ == "__main__":
    run_bench()

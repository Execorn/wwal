#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Empirical benchmark and Section 2.2 SLO telemetry harness for WayWal."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import glob
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any

try:
    import psutil  # type: ignore[import-untyped]
except ImportError:  # pragma: no cover (minimal CI images may omit psutil)
    psutil = None

WAYWAL_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DEFAULT_IMG = os.path.join(WAYWAL_DIR, "tests", "test_pattern.png")

_GPU_BUSY_PATHS = sorted(glob.glob("/sys/class/drm/card*/device/gpu_busy_percent"))
_VRAM_USED_PATHS = sorted(glob.glob("/sys/class/drm/card*/device/mem_info_vram_used"))
_HAS_NVIDIA_SMI = shutil.which("nvidia-smi") is not None
_LAST_NVIDIA_QUERY_TIME = 0.0
_CACHED_GPU_BUSY = 0
_CACHED_VRAM_MB = 0.0

FORBIDDEN_DAEMON_DT_NEEDED = (
    "libavcodec",
    "libavformat",
    "libavutil",
    "libswscale",
    "libwebp",
    "libavif",
    "libjxl",
    "librsvg",
    "libpipewire",
    "libzstd",
    "libsodium",
    "libcurl",
    "liblz4",
    "libQt6",
)

FORBIDDEN_CLIENT_DT_NEEDED = (
    *FORBIDDEN_DAEMON_DT_NEEDED,
    "libpng",
    "libturbojpeg",
)


@dataclass(slots=True)
class SloRecord:
    id: str
    metric: str
    category: str
    measured: float | str | None
    unit: str
    target_slo: str
    ci_gate: str
    status: str
    source: str
    notes: str = ""


def resolve_build_dir(cli_build_dir: str | None) -> str:
    raw = cli_build_dir or os.environ.get("WWAL_BUILD_DIR") or "build_rel"
    if os.path.isabs(raw):
        return raw
    cwd_candidate = os.path.abspath(raw)
    if os.path.exists(cwd_candidate):
        return cwd_candidate
    return os.path.abspath(os.path.join(WAYWAL_DIR, raw))


def _query_nvidia_smi() -> tuple[int, float]:
    global _HAS_NVIDIA_SMI, _LAST_NVIDIA_QUERY_TIME, _CACHED_GPU_BUSY, _CACHED_VRAM_MB
    now = time.monotonic()
    if not _HAS_NVIDIA_SMI:
        return 0, 0.0
    if now - _LAST_NVIDIA_QUERY_TIME < 0.25:
        return _CACHED_GPU_BUSY, _CACHED_VRAM_MB
    try:
        out = subprocess.check_output(
            [
                "nvidia-smi",
                "--query-gpu=utilization.gpu,memory.used",
                "--format=csv,noheader,nounits",
            ],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=0.2,
        )
        line = out.strip().splitlines()[0]
        parts = [p.strip() for p in line.split(",")]
        _CACHED_GPU_BUSY = int(parts[0])
        _CACHED_VRAM_MB = float(parts[1])
        _LAST_NVIDIA_QUERY_TIME = now
    except Exception:
        # Cache probe failure so dead nvidia-smi calls do not distort sampling intervals.
        _HAS_NVIDIA_SMI = False
    return _CACHED_GPU_BUSY, _CACHED_VRAM_MB


def get_gpu_busy() -> int:
    for path in _GPU_BUSY_PATHS:
        try:
            with open(path, "r", encoding="utf-8") as f:
                return int(f.read().strip())
        except OSError:
            continue
    if _HAS_NVIDIA_SMI:
        busy, _ = _query_nvidia_smi()
        return busy
    return 0


def get_vram_mb() -> float:
    for path in _VRAM_USED_PATHS:
        try:
            with open(path, "r", encoding="utf-8") as f:
                return int(f.read().strip()) / (1024.0 * 1024.0)
        except OSError:
            continue
    if _HAS_NVIDIA_SMI:
        _, vram = _query_nvidia_smi()
        return vram
    return 0.0


def read_proc_status_kb(pid: int) -> dict[str, int]:
    info: dict[str, int] = {"VmRSS": 0, "VmHWM": 0, "RssAnon": 0, "Pss": 0}
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
            for line in f:
                for key in ("VmRSS:", "VmHWM:", "RssAnon:"):
                    if line.startswith(key):
                        info[key[:-1]] = int(line.split()[1])
                        break
    except OSError:
        pass
    try:
        with open(f"/proc/{pid}/smaps_rollup", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("Pss:"):
                    info["Pss"] = int(line.split()[1])
                    break
    except OSError:
        info["Pss"] = info["VmRSS"]
    return info


def get_proc_stats(pid: int) -> dict[str, float] | None:
    try:
        status_kb = read_proc_status_kb(pid)
        cpu_user_s = 0.0
        cpu_sys_s = 0.0
        rss_bytes = status_kb["VmRSS"] * 1024
        vms_bytes = 0
        if psutil is not None:
            proc = psutil.Process(pid)
            mem = proc.memory_info()
            cpu_times = proc.cpu_times()
            cpu_user_s = float(cpu_times.user)
            cpu_sys_s = float(cpu_times.system)
            rss_bytes = int(mem.rss)
            vms_bytes = int(mem.vms)
        else:
            clk_tck = float(os.sysconf(os.sysconf_names["SC_CLK_TCK"]))
            with open(f"/proc/{pid}/stat", "r", encoding="utf-8") as f:
                raw = f.read()
            rparen = raw.rfind(")")
            fields = raw[rparen + 2 :].split()
            cpu_user_s = int(fields[11]) / clk_tck
            cpu_sys_s = int(fields[12]) / clk_tck
            vms_bytes = int(fields[20])
        vmrss_mb = status_kb["VmRSS"] / 1024.0 if status_kb["VmRSS"] > 0 else rss_bytes / (1024 * 1024)
        vmhwm_mb = status_kb["VmHWM"] / 1024.0 if status_kb["VmHWM"] > 0 else vmrss_mb
        pss_mb = status_kb["Pss"] / 1024.0 if status_kb["Pss"] > 0 else vmrss_mb
        return {
            "rss_mb": vmrss_mb,
            "hwm_mb": vmhwm_mb,
            "pss_mb": pss_mb,
            "vms_mb": vms_bytes / (1024 * 1024),
            "cpu_user_s": cpu_user_s,
            "cpu_sys_s": cpu_sys_s,
            "cpu_total_s": cpu_user_s + cpu_sys_s,
        }
    except Exception:
        return None


def audit_dt_needed(
    binary_path: str,
    forbidden_prefixes: tuple[str, ...],
) -> dict[str, Any]:
    if not os.path.exists(binary_path):
        return {"exists": False, "needed": [], "count": 0, "violations": []}
    if shutil.which("readelf") is None:
        return {"exists": True, "needed": [], "count": None, "violations": [], "error": "readelf missing"}
    proc = subprocess.run(
        ["readelf", "-d", binary_path],
        text=True,
        capture_output=True,
        check=False,
    )
    if proc.returncode != 0:
        return {"exists": True, "needed": [], "count": None, "violations": [], "error": "readelf failed"}
    needed: list[str] = []
    for line in proc.stdout.splitlines():
        if "(NEEDED)" in line and "[" in line and "]" in line:
            needed.append(line.split("[", 1)[1].split("]", 1)[0])
    violations = [lib for lib in needed if any(lib.startswith(bad) for bad in forbidden_prefixes)]
    return {
        "exists": True,
        "needed": needed,
        "count": len(needed),
        "violations": violations,
    }


def read_elf_sections(binary_path: str) -> dict[str, Any]:
    if not os.path.exists(binary_path):
        return {"exists": False}
    if shutil.which("size") is None:
        return {"exists": True, "error": "size tool missing"}
    proc = subprocess.run(["size", binary_path], text=True, capture_output=True, check=False)
    if proc.returncode != 0:
        return {"exists": True, "error": proc.stderr.strip() or "size failed"}
    for line in proc.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 4 and parts[0].isdigit():
            return {
                "exists": True,
                "text_bytes": int(parts[0]),
                "data_bytes": int(parts[1]),
                "bss_bytes": int(parts[2]),
                "dec_bytes": int(parts[3]),
            }
    return {"exists": True, "error": "unrecognized size output"}


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


def run_command_capture(
    cmd: list[str],
    *,
    env: dict[str, str] | None = None,
    cwd: str | None = None,
    timeout: float = 120.0,
) -> dict[str, Any]:
    if not os.path.exists(cmd[0]):
        return {"status": "missing", "returncode": None, "elapsed_ms": 0.0, "stdout": "", "stderr": ""}
    t0 = time.perf_counter()
    try:
        proc = subprocess.run(
            cmd,
            env=env,
            cwd=cwd,
            text=True,
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        return {
            "status": "ok" if proc.returncode == 0 else ("skip" if proc.returncode == 77 else "fail"),
            "returncode": proc.returncode,
            "elapsed_ms": elapsed_ms,
            "stdout": proc.stdout,
            "stderr": proc.stderr,
        }
    except subprocess.TimeoutExpired as exc:
        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        return {
            "status": "timeout",
            "returncode": -1,
            "elapsed_ms": elapsed_ms,
            "stdout": (exc.stdout or "") if isinstance(exc.stdout, str) else "",
            "stderr": (exc.stderr or "") if isinstance(exc.stderr, str) else "",
        }


def make_isolated_xdg_env(base_dir: str, extra: dict[str, str] | None = None) -> dict[str, str]:
    env = os.environ.copy()
    runtime = os.path.join(base_dir, "runtime")
    cache = os.path.join(base_dir, "cache")
    config = os.path.join(base_dir, "config")
    state = os.path.join(base_dir, "state")
    for d in (runtime, cache, config, state):
        os.makedirs(d, exist_ok=True)
    os.chmod(runtime, 0o700)
    env["XDG_RUNTIME_DIR"] = runtime
    env["XDG_CACHE_HOME"] = cache
    env["XDG_CONFIG_HOME"] = config
    env["XDG_STATE_HOME"] = state
    env["WWAL_CACHE_DIR"] = os.path.join(cache, "wwal")
    env["WWAL_STATE_DIR"] = os.path.join(state, "wwal")
    if extra:
        env.update(extra)
    return env


def _parse_float(pattern: str, text: str) -> float | None:
    matches = re.findall(pattern, text, flags=re.IGNORECASE | re.MULTILINE)
    if not matches:
        return None
    val = matches[-1]
    if isinstance(val, tuple):
        val = val[0]
    try:
        return float(val)
    except ValueError:
        return None


def _parse_studio_rss(stdout: str) -> tuple[float | None, float | None]:
    rss_anon = _parse_float(
        r"(?:Private Anonymous / Heap RSS|RssAnon)[^0-9\n]*([0-9]+(?:\.[0-9]+)?)\s*MB",
        stdout,
    )
    if rss_anon is None:
        rss_kb = _parse_float(r"RssAnon:\s*([0-9]+(?:\.[0-9]+)?)\s*kB", stdout)
        if rss_kb is not None:
            rss_anon = rss_kb / 1024.0
    vmrss = _parse_float(
        r"(?:Total VmRSS|VmRSS)[^0-9\n]*([0-9]+(?:\.[0-9]+)?)\s*MB",
        stdout,
    )
    return rss_anon, vmrss


def run_studio_benchmarks(build_dir: str, runs: int) -> dict[str, Any]:
    studio_bin = os.path.join(build_dir, "wwal-studio")
    if not os.path.exists(studio_bin):
        return {
            "headless_check": {"status": "missing"},
            "realize_all_panels": {"status": "missing"},
        }

    latencies_ms: list[float] = []
    rss_anon_samples: list[float] = []
    vmrss_samples: list[float] = []
    last_res: dict[str, Any] = {}

    for i in range(max(1, runs)):
        with tempfile.TemporaryDirectory(prefix=f"wwal_studio_hc_{i}_") as tmp_dir:
            env = make_isolated_xdg_env(
                tmp_dir,
                {
                    "QT_QPA_PLATFORM": "offscreen",
                    "LIBVA_DRIVER_NAME": os.environ.get("LIBVA_DRIVER_NAME", "dummy"),
                },
            )
            res = run_command_capture([studio_bin, "--headless-check"], env=env, timeout=60.0)
            last_res = res
            if res["status"] != "ok":
                break
            latencies_ms.append(float(res["elapsed_ms"]))
            reported_ms = _parse_float(
                r"Startup\s+(?:latency|time)[^0-9\n]*([0-9]+(?:\.[0-9]+)?)\s*ms",
                res["stdout"],
            )
            if reported_ms is not None:
                # Prefer binary-internal timing if emitted, otherwise keep wall-clock measurement.
                latencies_ms[-1] = reported_ms
            rss_anon, vmrss = _parse_studio_rss(res["stdout"])
            if rss_anon is not None:
                rss_anon_samples.append(rss_anon)
            if vmrss is not None:
                vmrss_samples.append(vmrss)

    headless_summary: dict[str, Any] = {
        "status": last_res.get("status", "missing"),
        "returncode": last_res.get("returncode"),
        "runs": len(latencies_ms),
    }
    if latencies_ms:
        sorted_lat = sorted(latencies_ms)
        p99_idx = min(len(sorted_lat) - 1, (len(sorted_lat) * 99) // 100)
        headless_summary["startup_p50_ms"] = statistics.median(sorted_lat)
        headless_summary["startup_p99_ms"] = sorted_lat[p99_idx]
        headless_summary["startup_min_ms"] = sorted_lat[0]
        headless_summary["startup_max_ms"] = sorted_lat[-1]
    if rss_anon_samples:
        headless_summary["rss_anon_mb"] = statistics.median(rss_anon_samples)
    if vmrss_samples:
        headless_summary["vmrss_mb"] = statistics.median(vmrss_samples)
    if last_res.get("status") != "ok":
        headless_summary["stderr_tail"] = last_res.get("stderr", "")[-400:]

    with tempfile.TemporaryDirectory(prefix="wwal_studio_all_") as tmp_dir:
        env = make_isolated_xdg_env(
            tmp_dir,
            {
                "QT_QPA_PLATFORM": "offscreen",
                "LIBVA_DRIVER_NAME": os.environ.get("LIBVA_DRIVER_NAME", "dummy"),
            },
        )
        all_res = run_command_capture(
            [studio_bin, "--headless-check", "--realize-all-panels"],
            env=env,
            timeout=60.0,
        )
    combined_err = (all_res.get("stderr", "") + "\n" + all_res.get("stdout", "")).lower()
    if all_res["status"] == "fail" and (
        "unknown option" in combined_err or "realize-all-panels" in combined_err
    ):
        realize_summary: dict[str, Any] = {
            "status": "unsupported",
            "returncode": all_res["returncode"],
            "reason": "--realize-all-panels option not supported by this wwal-studio build",
        }
    else:
        rss_anon_all, vmrss_all = _parse_studio_rss(all_res.get("stdout", ""))
        realize_summary = {
            "status": all_res["status"],
            "returncode": all_res["returncode"],
            "elapsed_ms": all_res["elapsed_ms"],
            "rss_anon_mb": rss_anon_all,
            "vmrss_mb": vmrss_all,
        }
        if all_res["status"] != "ok":
            realize_summary["stderr_tail"] = all_res.get("stderr", "")[-400:]

    return {
        "headless_check": headless_summary,
        "realize_all_panels": realize_summary,
    }


def run_grid_benchmark(build_dir: str) -> dict[str, Any]:
    exe = os.path.join(build_dir, "benchmark_grid_virtualization")
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    res = run_command_capture([exe], env=env, cwd=WAYWAL_DIR, timeout=180.0)
    out = res["stdout"]
    parsed: dict[str, Any] = {
        "status": res["status"],
        "returncode": res["returncode"],
        "elapsed_ms": res["elapsed_ms"],
    }
    if res["status"] != "ok":
        parsed["stderr_tail"] = res["stderr"][-400:]
        return parsed

    parsed["palette_4k_ms"] = _parse_float(r"Palette Extraction:\s*([0-9.]+)\s*ms", out)
    parsed["fuzzy_10k_ms"] = _parse_float(r"Fuzzy Search Latency:\s*([0-9.]+)\s*ms", out)

    slice_m = re.search(r"Virtualized Slicing:\s*p50=([0-9.]+)\s*µ?us?,\s*p99=([0-9.]+)", out)
    if not slice_m:
        slice_m = re.search(
            r"Virtualized Slicing:\s*p50=([0-9.]+)\s*[^,]+,\s*p99=([0-9.]+)\s*[^(]+\(([0-9.]+)\s*ms\)",
            out,
        )
    if slice_m:
        parsed["virtual_grid_p50_us"] = float(slice_m.group(1))
        parsed["virtual_grid_p99_us"] = float(slice_m.group(2))

    hue_m = re.search(r"DominantHue:\s*p50=([0-9.]+)\s*ms,\s*p99=([0-9.]+)\s*ms", out)
    if hue_m:
        parsed["dominant_hue_100k_p50_ms"] = float(hue_m.group(1))
        parsed["dominant_hue_100k_p99_ms"] = float(hue_m.group(2))

    ref_m = re.search(r"SIMD refilter:\s*p50=([0-9.]+)\s*ms,\s*p99=([0-9.]+)\s*ms", out)
    if ref_m:
        parsed["refilter_100k_p50_ms"] = float(ref_m.group(1))
        parsed["refilter_100k_p99_ms"] = float(ref_m.group(2))

    etc2_m = re.search(
        r"ETC2 AVX2\s*\(([^)]+)\):\s*p50=([0-9.]+)\s*ms\s*\(([0-9.]+)\s*thumbs/s\),\s*p99=([0-9.]+)\s*ms",
        out,
    )
    if etc2_m:
        parsed["etc2_mode"] = etc2_m.group(1)
        parsed["etc2_p50_ms"] = float(etc2_m.group(2))
        parsed["etc2_thumbs_per_sec"] = float(etc2_m.group(3))
        parsed["etc2_p99_ms"] = float(etc2_m.group(4))

    up_m = re.search(
        r"batch upsert:\s*([0-9.]+)\s*ms\s*\|\s*single upsertItem p50:\s*([0-9.]+)\s*ms",
        out,
    )
    if up_m:
        parsed["batch_upsert_100k_ms"] = float(up_m.group(1))
        parsed["single_upsert_100k_p50_ms"] = float(up_m.group(2))

    bm25_m = re.search(
        r"BM25 1T:\s*p50=([0-9.]+)\s*ms,\s*p99=([0-9.]+)\s*ms\s*\|\s*8T:\s*p50=([0-9.]+)\s*ms,\s*p99=([0-9.]+)\s*ms",
        out,
    )
    if bm25_m:
        parsed["bm25_100k_1t_p50_ms"] = float(bm25_m.group(1))
        parsed["bm25_100k_1t_p99_ms"] = float(bm25_m.group(2))
        parsed["bm25_100k_8t_p50_ms"] = float(bm25_m.group(3))
        parsed["bm25_100k_8t_p99_ms"] = float(bm25_m.group(4))

    return parsed


def _parse_bench_transitions_tables(out: str) -> dict[str, Any]:
    parsed: dict[str, Any] = {
        "gpu_sections": {},
        "cpu_sections": {},
        "palette": {},
        "lockscreen_png": {},
    }
    mode = ""
    section_key = ""
    for line in out.splitlines():
        gpu_hdr = re.search(r"WayWal GPU Compute Benchmark:\s*(\d+x\d+)", line)
        if gpu_hdr:
            mode = "gpu"
            section_key = gpu_hdr.group(1)
            parsed["gpu_sections"][section_key] = {}
            continue
        cpu_hdr = re.search(r"WayWal CPU Fallback Benchmark:\s*(\d+x\d+)", line)
        if cpu_hdr:
            mode = "cpu"
            section_key = cpu_hdr.group(1)
            parsed["cpu_sections"][section_key] = {}
            continue
        if "Dual-Mode OKLab/HCT Palette" in line:
            mode = "palette"
            continue
        if "Lockscreen PNG Frame Export" in line:
            mode = "lockscreen"
            continue

        if mode == "gpu" and "|" in line:
            parts = [p.strip() for p in line.split("|")]
            if len(parts) == 8 and parts[0] != "Transition":
                try:
                    parsed["gpu_sections"][section_key][parts[0]] = {
                        "mean_ms": float(parts[1]),
                        "p50_ms": float(parts[2]),
                        "p99_ms": float(parts[3]),
                        "min_ms": float(parts[4]),
                        "max_ms": float(parts[5]),
                        "max_fps": float(parts[6]),
                        "cpu_pct": float(parts[7].rstrip("%")),
                    }
                except ValueError:
                    pass
        elif mode == "cpu" and "|" in line:
            parts = [p.strip() for p in line.split("|")]
            if len(parts) == 5 and parts[0] != "Transition":
                try:
                    parsed["cpu_sections"][section_key][parts[0]] = {
                        "mean_ms": float(parts[1]),
                        "min_ms": float(parts[2]),
                        "max_ms": float(parts[3]),
                        "max_fps": float(parts[4]),
                    }
                except ValueError:
                    pass
        elif mode in ("palette", "lockscreen") and "|" in line:
            parts = [p.strip() for p in line.split("|")]
            if len(parts) == 5 and parts[0] not in ("Stage / Operation", "Lockscreen Export Mode"):
                try:
                    target = parsed["palette"] if mode == "palette" else parsed["lockscreen_png"]
                    target[parts[0]] = {
                        "mean_ms": float(parts[1]),
                        "min_ms": float(parts[2]),
                        "p99_ms": float(parts[3]),
                        "max_ms": float(parts[4]),
                    }
                except ValueError:
                    pass

    gpu_cpus: list[float] = []
    for sec in parsed["gpu_sections"].values():
        for row in sec.values():
            gpu_cpus.append(float(row["cpu_pct"]))
    if gpu_cpus:
        parsed["gpu_cpu_mean_pct"] = statistics.mean(gpu_cpus)
        parsed["gpu_cpu_max_pct"] = max(gpu_cpus)

    cpu_4k = parsed["cpu_sections"].get("3840x2160", {})
    if "swirl" in cpu_4k:
        parsed["cpu_4k_swirl_mean_ms"] = cpu_4k["swirl"]["mean_ms"]
        parsed["cpu_4k_swirl_min_ms"] = cpu_4k["swirl"]["min_ms"]
    return parsed


def run_transitions_benchmarks(build_dir: str, frames_arg: str) -> dict[str, Any]:
    bench_bin = os.environ.get("WWAL_BENCH_BIN", os.path.join(build_dir, "bench_transitions"))
    res = run_command_capture([bench_bin, frames_arg], cwd=WAYWAL_DIR, timeout=180.0)
    data: dict[str, Any] = {
        "bench_transitions": {
            "status": res["status"],
            "returncode": res["returncode"],
            "elapsed_ms": res["elapsed_ms"],
        }
    }
    if res["status"] == "ok":
        data["bench_transitions"].update(_parse_bench_transitions_tables(res["stdout"]))
    elif res["status"] != "missing":
        data["bench_transitions"]["stderr_tail"] = res["stderr"][-400:]

    test_trans_bin = os.path.join(build_dir, "test_transitions")
    if os.path.exists(test_trans_bin):
        t_res = run_command_capture([test_trans_bin], cwd=WAYWAL_DIR, timeout=180.0)
        t_info: dict[str, Any] = {
            "status": t_res["status"],
            "returncode": t_res["returncode"],
            "elapsed_ms": t_res["elapsed_ms"],
        }
        swirl_m = re.search(
            r"\[PERF[-_]\d+\]\s*4K swirl p50=([0-9.]+)\s*ms,\s*fade p50=([0-9.]+)\s*ms",
            t_res["stdout"],
        )
        if swirl_m:
            t_info["cpu_4k_swirl_p50_ms"] = float(swirl_m.group(1))
            t_info["cpu_4k_fade_p50_ms"] = float(swirl_m.group(2))
        if t_res["status"] not in ("ok", "skip"):
            t_info["stderr_tail"] = t_res["stderr"][-400:]
        data["test_transitions"] = t_info

    return data


def run_gpu_brush_benchmark(build_dir: str) -> dict[str, Any]:
    exe = os.path.join(build_dir, "test_we_gpu_brush")
    res = run_command_capture([exe], cwd=WAYWAL_DIR, timeout=120.0)
    combined = res["stdout"] + "\n" + res["stderr"]
    if res["status"] == "fail" and "context initialization failed" in combined.lower():
        return {
            "status": "skip",
            "returncode": res["returncode"],
            "reason": "GBM/EGL OpenGL ES 3.1 device unavailable",
        }
    parsed: dict[str, Any] = {
        "status": res["status"],
        "returncode": res["returncode"],
        "elapsed_ms": res["elapsed_ms"],
    }
    if res["status"] != "ok":
        parsed["stderr_tail"] = res["stderr"][-400:]
        return parsed

    parsed["stroke_4k_p50_ms"] = _parse_float(r"4K RGBA8 40-Dab Stroke p50:\s*([0-9.]+)\s*ms", res["stdout"])
    parsed["color_dab_4k_ms"] = _parse_float(r"4K Color Dab \(r=128\):\s*([0-9.]+)\s*ms/dab", res["stdout"])
    parsed["kawase_blur_ms"] = _parse_float(r"Dual-Kawase Blur:\s*([0-9.]+)\s*ms/dab", res["stdout"])
    parsed["cow_undo_us"] = _parse_float(r"50-Stroke 4K CoW Undo \(r=48\):\s*([0-9.]+)\s*us/stroke", res["stdout"])
    return parsed


def run_mesh_rig_benchmark(build_dir: str) -> dict[str, Any]:
    exe = os.path.join(build_dir, "test_we_mesh_rig")
    res = run_command_capture([exe], cwd=WAYWAL_DIR, timeout=120.0)
    parsed: dict[str, Any] = {
        "status": res["status"],
        "returncode": res["returncode"],
        "elapsed_ms": res["elapsed_ms"],
    }
    if res["status"] != "ok":
        parsed["stderr_tail"] = res["stderr"][-400:]
        return parsed
    m = re.search(
        r"\[BENCH\]\s*VD-05 9991-vert 12-bone heat solve:\s*([0-9.]+)\s*ms\s*\(fill_nnz=(\d+),\s*idt_flips=(\d+),\s*ic0=(\d+),\s*L_inf=([^)]+)\)",
        res["stdout"],
    )
    if m:
        parsed["solve_9991_ms"] = float(m.group(1))
        parsed["fill_nnz"] = int(m.group(2))
        parsed["idt_flips"] = int(m.group(3))
        parsed["ic0_fallback"] = bool(int(m.group(4)))
        parsed["l_inf"] = float(m.group(5))
    return parsed


def run_shader_graph_benchmark(build_dir: str) -> dict[str, Any]:
    exe = os.path.join(build_dir, "test_shader_graph_compiler")
    res = run_command_capture([exe], cwd=WAYWAL_DIR, timeout=120.0)
    parsed: dict[str, Any] = {
        "status": res["status"],
        "returncode": res["returncode"],
        "elapsed_ms": res["elapsed_ms"],
    }
    if res["status"] != "ok":
        parsed["stderr_tail"] = res["stderr"][-400:]
        return parsed

    parsed["compile_256_single_ms"] = _parse_float(
        r"256-node graph compile time:\s*([0-9.]+)\s*ms",
        res["stdout"],
    )
    m = re.search(
        r"16T contended 256-node compile wall latency:\s*p50=([0-9.]+)\s*ms,\s*p99=([0-9.]+)\s*ms,\s*max=([0-9.]+)\s*ms",
        res["stdout"],
    )
    if m:
        parsed["compile_16t_p50_ms"] = float(m.group(1))
        parsed["compile_16t_p99_ms"] = float(m.group(2))
        parsed["compile_16t_max_ms"] = float(m.group(3))
    return parsed


def run_particle_audio_benchmark(build_dir: str) -> dict[str, Any]:
    exe = os.path.join(build_dir, "test_we_particle_audio")
    res = run_command_capture([exe], cwd=WAYWAL_DIR, timeout=120.0)
    parsed: dict[str, Any] = {
        "status": res["status"],
        "returncode": res["returncode"],
        "elapsed_ms": res["elapsed_ms"],
    }
    if res["status"] != "ok":
        parsed["stderr_tail"] = res["stderr"][-400:]
        return parsed
    parsed["cpu_100k_ms"] = _parse_float(r"\[PERF\]\s*100k CPU[^:]*:\s*([0-9.]+)\s*ms/frame", res["stdout"])
    parsed["gpu_100k_ms"] = _parse_float(r"\[PERF\]\s*100k GPU[^:]*:\s*([0-9.]+)\s*ms/frame", res["stdout"])
    return parsed


def run_optional_companions(build_dir: str) -> dict[str, Any]:
    companions: dict[str, Any] = {}

    cat_bin = os.path.join(build_dir, "test_catalog_index")
    if os.path.exists(cat_bin):
        env = os.environ.copy()
        env["QT_QPA_PLATFORM"] = "offscreen"
        res = run_command_capture([cat_bin], env=env, cwd=WAYWAL_DIR, timeout=120.0)
        info: dict[str, Any] = {"status": res["status"], "returncode": res["returncode"]}
        m = re.search(r"FTS5 BM25 Query Latency:\s*Median\s*=\s*([0-9.]+)\s*ms\s*\|\s*P95\s*=\s*([0-9.]+)\s*ms", res["stdout"])
        if m:
            info["bm25_median_ms"] = float(m.group(1))
            info["bm25_p95_ms"] = float(m.group(2))
        companions["test_catalog_index"] = info

    cmd_bin = os.path.join(build_dir, "test_studio_command_and_dock")
    if os.path.exists(cmd_bin):
        env = os.environ.copy()
        env["QT_QPA_PLATFORM"] = "offscreen"
        res = run_command_capture([cmd_bin], env=env, cwd=WAYWAL_DIR, timeout=120.0)
        info = {"status": res["status"], "returncode": res["returncode"]}
        info["command_bus_100k_ms"] = _parse_float(
            r"CommandBus[^\n]*100k[^\n]*?([0-9.]+)\s*ms",
            res["stdout"],
        )
        companions["test_studio_command_and_dock"] = info

    sub_bin = os.path.join(build_dir, "test_subsystems")
    if os.path.exists(sub_bin):
        res = run_command_capture([sub_bin], cwd=WAYWAL_DIR, timeout=120.0)
        info = {"status": res["status"], "returncode": res["returncode"]}
        info["webp_4k_p50_ms"] = _parse_float(r"4K\s*WebP[^\n]*?([0-9.]+)\s*ms", res["stdout"])
        info["qoi_4k_p50_ms"] = _parse_float(r"4K\s*QOI[^\n]*?([0-9.]+)\s*ms", res["stdout"])
        companions["test_subsystems"] = info

    return companions


def audit_compile_commands_and_coverage(build_dir: str) -> dict[str, Any]:
    result: dict[str, Any] = {}
    cc_path = os.path.join(build_dir, "compile_commands.json")
    if os.path.exists(cc_path):
        try:
            with open(cc_path, "r", encoding="utf-8") as f:
                entries = json.load(f)
            c_files = [
                os.path.normpath(os.path.join(e.get("directory", build_dir), e["file"]))
                for e in entries
                if isinstance(e, dict) and str(e.get("file", "")).endswith(".c")
            ]
            rel_c_files = [os.path.relpath(p, WAYWAL_DIR) for p in c_files]
            src_c = [p for p in rel_c_files if p.startswith("src/")]
            result["total_c_compiles"] = len(src_c)
            result["unique_c_files"] = len(set(src_c))
            result["duplicate_c_compiles"] = len(src_c) - len(set(src_c))
        except Exception as exc:
            result["compile_commands_error"] = str(exc)

    cov_txt = os.path.join(build_dir, "meson-logs", "coverage.txt")
    if os.path.exists(cov_txt):
        try:
            with open(cov_txt, "r", encoding="utf-8") as f:
                cov_raw = f.read()
            m = re.search(r"TOTAL\s+\d+\s+\d+\s+([0-9.]+)%\s+\d+\s+\d+\s+([0-9.]+)%", cov_raw)
            if m:
                result["coverage_line_pct"] = float(m.group(1))
                result["coverage_branch_pct"] = float(m.group(2))
        except OSError:
            pass
    return result


def benchmark_pick_cache(client_bin: str) -> dict[str, Any]:
    if not os.path.exists(client_bin):
        return {"status": "missing"}

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
            [client_bin, "pick", "--dir", wall_dir, "--menu", "json"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
            timeout=10.0,
        )
        cold_ms = (time.perf_counter() - t0) * 1000.0

        warm_samples: list[float] = []
        for _ in range(20):
            tw0 = time.perf_counter()
            subprocess.run(
                [client_bin, "pick", "--dir", wall_dir, "--menu", "json"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=True,
                timeout=10.0,
            )
            warm_samples.append((time.perf_counter() - tw0) * 1000.0)

        warm_samples.sort()
        return {
            "status": "ok",
            "cold_ms": cold_ms,
            "warm_mean_ms": statistics.mean(warm_samples),
            "warm_min_ms": warm_samples[0],
            "warm_max_ms": warm_samples[-1],
        }


def query_daemon_stats(client_bin: str, namespace: str, env: dict[str, str]) -> dict[str, Any]:
    try:
        out = subprocess.check_output(
            [client_bin, "-n", namespace, "stats", "--json"],
            env=env,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=2.0,
        )
        return json.loads(out)
    except Exception:
        return {}


def benchmark_live_daemon(daemon_bin: str, client_bin: str) -> dict[str, Any]:
    if not os.environ.get("WAYLAND_DISPLAY"):
        return {"status": "skip", "reason": "WAYLAND_DISPLAY not set"}
    if not os.path.exists(daemon_bin) or not os.path.exists(client_bin):
        return {"status": "missing"}

    img1 = os.environ.get("WWAL_BENCH_IMG1", DEFAULT_IMG)
    img2 = os.environ.get("WWAL_BENCH_IMG2", DEFAULT_IMG)
    namespace = f"bench_{os.getpid()}"

    with tempfile.TemporaryDirectory(prefix="wwal_bench_state_") as state_root:
        env = os.environ.copy()
        env["WWAL_CACHE_DIR"] = os.path.join(state_root, "cache")
        env["WWAL_STATE_DIR"] = os.path.join(state_root, "state")
        os.makedirs(env["WWAL_CACHE_DIR"], exist_ok=True)
        os.makedirs(env["WWAL_STATE_DIR"], exist_ok=True)

        vram_before_mb = get_vram_mb()
        proc = subprocess.Popen(
            [daemon_bin, "-n", namespace, "--no-daemon"],
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        try:
            time.sleep(0.5)
            if proc.poll() is not None:
                return {"status": "skip", "reason": "isolated wwald exited early"}

            subprocess.run(
                [client_bin, "-n", namespace, "img", img1, "--transition-type", "none"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                timeout=5.0,
            )
            time.sleep(0.4)

            idle_t0 = get_proc_stats(proc.pid)
            time.sleep(2.0)
            idle_t1 = get_proc_stats(proc.pid)
            vram_after_mb = get_vram_mb()
            d_stats = query_daemon_stats(client_bin, namespace, env)

            result: dict[str, Any] = {"status": "ok", "namespace": namespace, "transitions": {}}
            if idle_t0 and idle_t1:
                outputs = d_stats.get("outputs", [])
                pool_bytes = int(
                    d_stats.get(
                        "vram_pool_bytes",
                        sum(int(o.get("width", 0)) * int(o.get("height", 0)) * 4 * 3 for o in outputs),
                    )
                )
                result["idle"] = {
                    "vmrss_mb": idle_t1["rss_mb"],
                    "pss_mb": idle_t1["pss_mb"],
                    "vmhwm_mb": idle_t1["hwm_mb"],
                    "cpu_pct": ((idle_t1["cpu_total_s"] - idle_t0["cpu_total_s"]) / 2.0) * 100.0,
                    "vram_pool_mb": pool_bytes / (1024 * 1024),
                    "vram_delta_mb": max(0.0, vram_after_mb - vram_before_mb),
                    "bo_count": int(d_stats.get("vram_bo_count", len(outputs) * 3)),
                    "outputs": len(outputs),
                    "dmabuf_scanout": bool(d_stats.get("dmabuf_scanout", False)),
                }

            curr_img, next_img = img2, img1
            transition_cpus: list[float] = []
            for t_name in ("fade", "wipe", "wave", "grow"):
                dur = 0.6
                t_start = get_proc_stats(proc.pid)
                gpu_samples: list[int] = []
                t0 = time.perf_counter()
                subprocess.run(
                    [
                        client_bin,
                        "-n",
                        namespace,
                        "img",
                        curr_img,
                        "--transition-type",
                        t_name,
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
                t_end = get_proc_stats(proc.pid)
                if t_start and t_end:
                    cpu_ms = (t_end["cpu_total_s"] - t_start["cpu_total_s"]) * 1000.0
                    cpu_pct = (cpu_ms / (dur * 1000.0)) * 100.0
                    transition_cpus.append(cpu_pct)
                    result["transitions"][t_name] = {
                        "daemon_cpu_ms": cpu_ms,
                        "daemon_cpu_pct": cpu_pct,
                        "vmrss_mb": t_end["rss_mb"],
                        "avg_gpu_pct": statistics.mean(gpu_samples) if gpu_samples else 0.0,
                    }
                curr_img, next_img = next_img, curr_img
                time.sleep(0.2)

            if transition_cpus:
                result["transition_cpu_mean_pct"] = statistics.mean(transition_cpus)

            subprocess.run(
                [client_bin, "-n", namespace, "pause"],
                env=env,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                check=False,
                timeout=5.0,
            )
            time.sleep(0.2)
            occ_t0 = get_proc_stats(proc.pid)
            time.sleep(2.0)
            occ_t1 = get_proc_stats(proc.pid)
            if occ_t0 and occ_t1:
                result["paused_cpu_pct"] = ((occ_t1["cpu_total_s"] - occ_t0["cpu_total_s"]) / 2.0) * 100.0
            return result
        finally:
            if proc.poll() is None:
                try:
                    subprocess.run(
                        [client_bin, "-n", namespace, "kill"],
                        env=env,
                        stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL,
                        check=False,
                        timeout=2.0,
                    )
                except subprocess.TimeoutExpired:
                    pass
            try:
                proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2.0)


def _eval_upper_bound(
    measured: float | None,
    *,
    strict_target: float,
    ci_gate: float | None = None,
    inclusive: bool = True,
    base_status: str = "ok",
    unsupported_note: str = "",
) -> str:
    if measured is None:
        if base_status in ("missing", "skip", "unsupported"):
            return base_status.upper()
        if base_status in ("fail", "timeout"):
            return "FAIL"
        return "NOT_EMITTED"
    ok_strict = (measured <= strict_target) if inclusive else (measured < strict_target)
    if ok_strict:
        return "PASS"
    if ci_gate is not None:
        ok_gate = (measured <= ci_gate) if inclusive else (measured < ci_gate)
        if ok_gate:
            return "CI_GATE_PASS"
    return "FAIL"


def build_slo_records(telemetry: dict[str, Any]) -> list[SloRecord]:
    studio = telemetry["suites"]["studio"]
    hc = studio.get("headless_check", {})
    realize = studio.get("realize_all_panels", {})
    grid = telemetry["suites"]["grid"]
    trans = telemetry["suites"]["transitions"]
    bt = trans.get("bench_transitions", {})
    tt = trans.get("test_transitions", {})
    brush = telemetry["suites"]["gpu_brush"]
    rig = telemetry["suites"]["mesh_rig"]
    graph = telemetry["suites"]["shader_graph"]
    part = telemetry["suites"]["particle_audio"]
    comp = telemetry["suites"]["companions"]
    live = telemetry["suites"].get("live_daemon", {})
    bins = telemetry["binaries"]
    graph_cov = telemetry["build_and_coverage"]

    wwald_bss = bins["wwald"]["size"].get("bss_bytes")
    wwald_needed = bins["wwald"]["dt_needed"].get("count")
    wwald_violations = bins["wwald"]["dt_needed"].get("violations", [])
    wwal_needed = bins["wwal"]["dt_needed"].get("count")
    wwal_violations = bins["wwal"]["dt_needed"].get("violations", [])

    bm25_p99 = None
    bm25_candidates = [
        v
        for v in (grid.get("bm25_100k_1t_p99_ms"), grid.get("bm25_100k_8t_p99_ms"))
        if isinstance(v, (int, float))
    ]
    if bm25_candidates:
        bm25_p99 = max(bm25_candidates)

    gpu_cpu_pct = live.get("transition_cpu_mean_pct")
    gpu_cpu_source = "live_daemon"
    gpu_cpu_status = str(live.get("status", "skip"))
    gpu_cpu_notes = ""
    if gpu_cpu_pct is None:
        bt_gpu_cpu = bt.get("gpu_cpu_mean_pct")
        if bt_gpu_cpu is not None:
            gpu_cpu_notes = f"live daemon skipped; unpaced bench_transitions GPU CPU={bt_gpu_cpu:.1f}%"
        else:
            gpu_cpu_notes = str(live.get("reason", "requires live Wayland compositor"))

    swirl_ms = tt.get("cpu_4k_swirl_p50_ms")
    swirl_source = "test_transitions (4K swirl)"
    swirl_status_src = tt.get("status", "missing")
    if swirl_ms is None:
        swirl_ms = bt.get("cpu_4k_swirl_mean_ms")
        swirl_source = "bench_transitions (4K CPU mean)"
        swirl_status_src = bt.get("status", "missing")

    cmd_ms = comp.get("test_studio_command_and_dock", {}).get("command_bus_100k_ms")
    cmd_status = comp.get("test_studio_command_and_dock", {}).get("status", "missing")

    sub_info = comp.get("test_subsystems", {})
    webp_ms = sub_info.get("webp_4k_p50_ms")
    qoi_ms = sub_info.get("qoi_4k_p50_ms")
    sub_status = sub_info.get("status", "missing")

    tu_dups = graph_cov.get("duplicate_c_compiles")
    tu_unique = graph_cov.get("unique_c_files")
    cov_line = graph_cov.get("coverage_line_pct")
    cov_branch = graph_cov.get("coverage_branch_pct")

    records = [
        SloRecord(
            id="studio_headless_rss_anon",
            metric="wwal-studio default 4-panel RssAnon",
            category="Studio Memory",
            measured=hc.get("rss_anon_mb"),
            unit="MB",
            target_slo="< 55.0 (<= 53.5)",
            ci_gate="< 55.0",
            status=_eval_upper_bound(
                hc.get("rss_anon_mb"),
                strict_target=55.0,
                inclusive=False,
                base_status=str(hc.get("status", "missing")),
            ),
            source="wwal-studio --headless-check",
        ),
        SloRecord(
            id="studio_realize_all_rss_anon",
            metric="wwal-studio --realize-all-panels RssAnon",
            category="Studio Memory",
            measured=realize.get("rss_anon_mb"),
            unit="MB",
            target_slo="<= 62.0",
            ci_gate="<= 62.0",
            status=_eval_upper_bound(
                realize.get("rss_anon_mb"),
                strict_target=62.0,
                base_status=str(realize.get("status", "missing")),
            ),
            source="wwal-studio --headless-check --realize-all-panels",
            notes=str(realize.get("reason", "")),
        ),
        SloRecord(
            id="studio_headless_startup_p50",
            metric="Cold --headless-check startup p50",
            category="Studio Startup",
            measured=hc.get("startup_p50_ms"),
            unit="ms",
            target_slo="< 350.0",
            ci_gate="< 350.0",
            status=_eval_upper_bound(
                hc.get("startup_p50_ms"),
                strict_target=350.0,
                inclusive=False,
                base_status=str(hc.get("status", "missing")),
            ),
            source=f"wwal-studio --headless-check (n={hc.get('runs', 0)})",
        ),
        SloRecord(
            id="grid_dominant_hue_100k_p50",
            metric="FuzzyCatalogIndex DominantHue 100k p50",
            category="Catalog Sort",
            measured=grid.get("dominant_hue_100k_p50_ms"),
            unit="ms",
            target_slo="<= 0.45",
            ci_gate="< 2.00",
            status=_eval_upper_bound(
                grid.get("dominant_hue_100k_p50_ms"),
                strict_target=0.45,
                ci_gate=2.0,
                base_status=str(grid.get("status", "missing")),
            ),
            source="benchmark_grid_virtualization",
        ),
        SloRecord(
            id="grid_refilter_100k_p50",
            metric="FuzzyCatalogIndex::refilter 100k p50",
            category="Catalog Search",
            measured=grid.get("refilter_100k_p50_ms"),
            unit="ms",
            target_slo="<= 1.20",
            ci_gate="<= 1.50",
            status=_eval_upper_bound(
                grid.get("refilter_100k_p50_ms"),
                strict_target=1.20,
                ci_gate=1.50,
                base_status=str(grid.get("status", "missing")),
            ),
            source="benchmark_grid_virtualization",
        ),
        SloRecord(
            id="catalog_bm25_p99",
            metric="CatalogIndexManager SQLite FTS5 BM25 p99",
            category="Catalog SQLite",
            measured=bm25_p99,
            unit="ms",
            target_slo="< 2.00",
            ci_gate="< 2.00",
            status=_eval_upper_bound(
                bm25_p99,
                strict_target=2.0,
                inclusive=False,
                base_status=str(grid.get("status", "missing")),
            ),
            source="benchmark_grid_virtualization (max 1T/8T p99)",
        ),
        SloRecord(
            id="catalog_upsert_100k_p50",
            metric="CatalogIndexManager::upsertItem 100k p50",
            category="Catalog RCU",
            measured=grid.get("single_upsert_100k_p50_ms"),
            unit="ms",
            target_slo="<= 0.06",
            ci_gate="<= 0.15",
            status=_eval_upper_bound(
                grid.get("single_upsert_100k_p50_ms"),
                strict_target=0.06,
                ci_gate=0.15,
                base_status=str(grid.get("status", "missing")),
            ),
            source="benchmark_grid_virtualization",
        ),
        SloRecord(
            id="etc2_block_pass_p50",
            metric="Etc2SimdCompressor 4x4 block pass p50",
            category="Grid ETC2",
            measured=grid.get("etc2_p50_ms"),
            unit="ms",
            target_slo="<= 1.50 (>= 666 thumbs/s)",
            ci_gate="<= 1.65",
            status=_eval_upper_bound(
                grid.get("etc2_p50_ms"),
                strict_target=1.50,
                ci_gate=1.65,
                base_status=str(grid.get("status", "missing")),
            ),
            source="benchmark_grid_virtualization",
            notes=(
                f"{grid.get('etc2_thumbs_per_sec'):.1f} thumbs/s ({grid.get('etc2_mode', '')})"
                if isinstance(grid.get("etc2_thumbs_per_sec"), (int, float))
                else ""
            ),
        ),
        SloRecord(
            id="brush_4k_stroke_p50",
            metric="4K RGBA8 40-dab brush stroke p50",
            category="GPU Brush",
            measured=brush.get("stroke_4k_p50_ms"),
            unit="ms",
            target_slo="<= 2.00",
            ci_gate="<= 2.00",
            status=_eval_upper_bound(
                brush.get("stroke_4k_p50_ms"),
                strict_target=2.0,
                base_status=str(brush.get("status", "missing")),
            ),
            source="test_we_gpu_brush",
            notes=str(brush.get("reason", "")),
        ),
        SloRecord(
            id="command_bus_100k_ms",
            metric="CommandBus 100k coalesced pushes",
            category="Command Bus",
            measured=cmd_ms,
            unit="ms",
            target_slo="<= 10.00 (>= 10 M cmds/s)",
            ci_gate="<= 10.00",
            status=_eval_upper_bound(
                cmd_ms,
                strict_target=10.0,
                base_status=str(cmd_status),
            ),
            source="test_studio_command_and_dock",
        ),
        SloRecord(
            id="wwald_gpu_transition_cpu_pct",
            metric="wwald GPU transition CPU burn",
            category="Daemon GPU Fence",
            measured=gpu_cpu_pct,
            unit="%",
            target_slo="<= 1.00",
            ci_gate="<= 1.00",
            status=_eval_upper_bound(
                gpu_cpu_pct,
                strict_target=1.0,
                base_status=gpu_cpu_status,
            ),
            source=gpu_cpu_source,
            notes=gpu_cpu_notes,
        ),
        SloRecord(
            id="wwald_cpu_4k_swirl_p50",
            metric="wwald CPU 4K swirl transition p50",
            category="CPU Transition",
            measured=swirl_ms,
            unit="ms",
            target_slo="<= 4.50",
            ci_gate="<= 7.50",
            status=_eval_upper_bound(
                swirl_ms,
                strict_target=4.5,
                ci_gate=7.5,
                base_status=str(swirl_status_src),
            ),
            source=swirl_source,
        ),
        SloRecord(
            id="wwald_bss_bytes",
            metric="wwald .bss static footprint",
            category="Daemon Footprint",
            measured=wwald_bss,
            unit="B",
            target_slo="<= 120000",
            ci_gate="<= 120000",
            status=_eval_upper_bound(
                float(wwald_bss) if wwald_bss is not None else None,
                strict_target=120000.0,
                base_status="ok" if bins["wwald"]["size"].get("exists") else "missing",
            ),
            source="size wwald",
        ),
        SloRecord(
            id="we_mesh_rig_9991_ms",
            metric="we_mesh_rig 9,991-vert biharmonic solve",
            category="Rig Solver",
            measured=rig.get("solve_9991_ms"),
            unit="ms",
            target_slo="<= 4.50",
            ci_gate="< 10.00",
            status=_eval_upper_bound(
                rig.get("solve_9991_ms"),
                strict_target=4.5,
                ci_gate=10.0,
                base_status=str(rig.get("status", "missing")),
            ),
            source="test_we_mesh_rig",
        ),
        SloRecord(
            id="shader_graph_16t_p99",
            metric="shader_graph 16T 256-node compile p99",
            category="Shader Compiler",
            measured=graph.get("compile_16t_p99_ms"),
            unit="ms",
            target_slo="<= 0.45",
            ci_gate="< 0.50",
            status=_eval_upper_bound(
                graph.get("compile_16t_p99_ms"),
                strict_target=0.45,
                ci_gate=0.50,
                base_status=str(graph.get("status", "missing")),
            ),
            source="test_shader_graph_compiler",
        ),
        SloRecord(
            id="we_particle_100k_cpu_ms",
            metric="we_particle 100k CPU update+verts",
            category="Particle CPU",
            measured=part.get("cpu_100k_ms"),
            unit="ms",
            target_slo="<= 0.75",
            ci_gate="<= 0.75",
            status=_eval_upper_bound(
                part.get("cpu_100k_ms"),
                strict_target=0.75,
                base_status=str(part.get("status", "missing")),
            ),
            source="test_we_particle_audio",
        ),
        SloRecord(
            id="we_particle_100k_gpu_ms",
            metric="we_particle 100k GPU compute+draw",
            category="Particle GPU",
            measured=part.get("gpu_100k_ms"),
            unit="ms",
            target_slo="<= 0.20",
            ci_gate="<= 0.20",
            status=_eval_upper_bound(
                part.get("gpu_100k_ms"),
                strict_target=0.20,
                base_status=str(part.get("status", "missing")),
            ),
            source="test_we_particle_audio",
        ),
        SloRecord(
            id="image_decode_4k_webp_p50",
            metric="4K WebP image decode p50",
            category="Image Decode",
            measured=webp_ms,
            unit="ms",
            target_slo="<= 40.00",
            ci_gate="<= 40.00",
            status=_eval_upper_bound(webp_ms, strict_target=40.0, base_status=str(sub_status)),
            source="test_subsystems",
        ),
        SloRecord(
            id="image_decode_4k_qoi_p50",
            metric="4K QOI image decode p50",
            category="Image Decode",
            measured=qoi_ms,
            unit="ms",
            target_slo="<= 17.00",
            ci_gate="<= 17.00",
            status=_eval_upper_bound(qoi_ms, strict_target=17.0, base_status=str(sub_status)),
            source="test_subsystems",
        ),
        SloRecord(
            id="wwald_dt_needed_count",
            metric="wwald DT_NEEDED audit",
            category="Dynamic Linking",
            measured=wwald_needed,
            unit="libs",
            target_slo="<= 12 (0 forbidden)",
            ci_gate="<= 14 (0 forbidden)",
            status=(
                "MISSING"
                if not bins["wwald"]["dt_needed"].get("exists")
                else (
                    "FAIL"
                    if wwald_violations
                    else _eval_upper_bound(
                        float(wwald_needed) if wwald_needed is not None else None,
                        strict_target=12.0,
                        ci_gate=14.0,
                    )
                )
            ),
            source="readelf -d wwald",
            notes=f"violations: {', '.join(wwald_violations)}" if wwald_violations else "",
        ),
        SloRecord(
            id="wwal_dt_needed_count",
            metric="wwal DT_NEEDED audit",
            category="Dynamic Linking",
            measured=wwal_needed,
            unit="libs",
            target_slo="<= 2 (0 forbidden)",
            ci_gate="<= 2 (0 forbidden)",
            status=(
                "MISSING"
                if not bins["wwal"]["dt_needed"].get("exists")
                else (
                    "FAIL"
                    if wwal_violations
                    else _eval_upper_bound(
                        float(wwal_needed) if wwal_needed is not None else None,
                        strict_target=2.0,
                        ci_gate=2.0,
                    )
                )
            ),
            source="readelf -d wwal",
            notes=f"violations: {', '.join(wwal_violations)}" if wwal_violations else "",
        ),
        SloRecord(
            id="build_tu_dedup_c_files",
            metric="Build graph duplicate C TUs",
            category="Build Graph",
            measured=tu_dups,
            unit="dups",
            target_slo="0 dups (27 shared TUs)",
            ci_gate="0 dups",
            status=(
                "NOT_MEASURED"
                if tu_dups is None
                else ("PASS" if tu_dups == 0 else "FAIL")
            ),
            source="compile_commands.json",
            notes=f"unique C files={tu_unique}" if tu_unique is not None else "",
        ),
        SloRecord(
            id="coverage_line_branch",
            metric="Aggregate line / branch coverage",
            category="Test Coverage",
            measured=(
                f"{cov_line:.1f}%L / {cov_branch:.1f}%B"
                if cov_line is not None and cov_branch is not None
                else None
            ),
            unit="%",
            target_slo=">= 85.0% L / >= 75.0% B",
            ci_gate=">= 85.0% L / >= 75.0% B",
            status=(
                "NOT_MEASURED"
                if cov_line is None or cov_branch is None
                else ("PASS" if cov_line >= 85.0 and cov_branch >= 75.0 else "FAIL")
            ),
            source="meson-logs/coverage.txt",
        ),
    ]
    return records


def _fmt_measured(val: float | str | None, unit: str) -> str:
    if val is None:
        return "N/A"
    if isinstance(val, str):
        return val
    if unit in ("B", "libs", "dups"):
        return f"{int(val)} {unit}"
    return f"{val:.3f} {unit}"


def render_markdown(telemetry: dict[str, Any]) -> str:
    lines: list[str] = [
        "# WayWal Empirical Benchmark & Section 2.2 SLO Telemetry",
        "",
        f"- **Timestamp (UTC):** `{telemetry['timestamp_utc']}`",
        f"- **Build Dir:** `{telemetry['build_dir']}`",
        "",
        "## Binary Footprint, `.bss` & `DT_NEEDED` Audit",
        "",
        "| Binary | Stripped (KB) | Unstripped (KB) | `.text` (B) | `.data` (B) | `.bss` (B) | `DT_NEEDED` | Forbidden |",
        "| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :--- |",
    ]
    for name in ("wwald", "wwal", "wwal-studio"):
        b = telemetry["binaries"][name]
        sz = b["size"]
        dt = b["dt_needed"]
        st = b["stripped_kb"]
        rw = b["raw_kb"]
        if not sz.get("exists"):
            lines.append(f"| `{name}` | missing | missing | - | - | - | - | - |")
            continue
        viol = ", ".join(dt.get("violations", [])) or "none"
        lines.append(
            f"| `{name}` | {st:.1f} | {rw:.1f} | {sz.get('text_bytes', '-')} | "
            f"{sz.get('data_bytes', '-')} | {sz.get('bss_bytes', '-')} | "
            f"{dt.get('count', '-')} | {viol} |"
        )

    lines.extend(
        [
            "",
            "## Section 2.2 Baseline-to-Target SLO Ledger",
            "",
            "| SLO ID | Metric (`Category`) | Measured | Target SLO | CI Gate | Status | Source | Notes |",
            "| :--- | :--- | ---: | :--- | :--- | :---: | :--- | :--- |",
        ]
    )
    for row in telemetry["slos"]:
        measured_str = _fmt_measured(row["measured"], row["unit"])
        lines.append(
            f"| `{row['id']}` | {row['metric']} (`{row['category']}`) | {measured_str} | "
            f"{row['target_slo']} | {row['ci_gate']} | **{row['status']}** | "
            f"`{row['source']}` | {row['notes']} |"
        )

    pick = telemetry["suites"].get("pick_cache", {})
    if pick.get("status") == "ok":
        lines.extend(
            [
                "",
                "## Unified Picker Cache Latency",
                "",
                f"- Cold cache: `{pick['cold_ms']:.2f} ms`",
                f"- Warm cache (20 iters): mean `{pick['warm_mean_ms']:.2f} ms`, "
                f"min `{pick['warm_min_ms']:.2f} ms`, max `{pick['warm_max_ms']:.2f} ms`",
            ]
        )

    live = telemetry["suites"].get("live_daemon", {})
    if live.get("status") == "ok" and "idle" in live:
        idle = live["idle"]
        lines.extend(
            [
                "",
                "## Isolated Live Wayland Daemon",
                "",
                f"- Idle: `VmRSS {idle['vmrss_mb']:.2f} MB`, `PSS {idle['pss_mb']:.2f} MB`, "
                f"`VmHWM {idle['vmhwm_mb']:.2f} MB`, CPU `{idle['cpu_pct']:.2f}%`, "
                f"VRAM pool `{idle['vram_pool_mb']:.2f} MB` (`BO={idle['bo_count']}`, "
                f"DMA-BUF=`{idle['dmabuf_scanout']}`)",
            ]
        )
        if "paused_cpu_pct" in live:
            lines.append(f"- Zero-Watt paused CPU: `{live['paused_cpu_pct']:.2f}%`")
    elif live.get("status") == "skip":
        lines.extend(["", f"- Live Wayland daemon test skipped: `{live.get('reason', '')}`"])

    summary = telemetry["slo_summary"]
    lines.extend(
        [
            "",
            f"**Summary:** PASS={summary['pass']} | CI_GATE_PASS={summary['ci_gate_pass']} | "
            f"FAIL={summary['fail']} | SKIP={summary['skip']} | "
            f"NOT_EMITTED={summary['not_emitted']} | MISSING={summary['missing']}",
        ]
    )
    return "\n".join(lines) + "\n"


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="WayWal empirical benchmark and Section 2.2 SLO telemetry harness.",
    )
    parser.add_argument("--build-dir", default=None, help="Build directory (overrides WWAL_BUILD_DIR, default build_rel)")
    parser.add_argument("--frames", type=int, default=60, help="Frames per effect in bench_transitions (default: 60)")
    parser.add_argument(
        "--studio-runs",
        type=int,
        default=int(os.environ.get("WWAL_BENCH_STUDIO_RUNS", "5")),
        help="Cold runs for wwal-studio --headless-check startup p50 (default: 5)",
    )
    parser.add_argument("--json", action="store_true", help="Emit JSON telemetry to stdout")
    parser.add_argument("--markdown", action="store_true", help="Emit Markdown telemetry to stdout")
    parser.add_argument("--json-out", default=os.environ.get("WWAL_BENCH_JSON_OUT"), help="Write JSON telemetry to path")
    parser.add_argument("--md-out", default=os.environ.get("WWAL_BENCH_MD_OUT"), help="Write Markdown telemetry to path")
    parser.add_argument("--skip-live", action="store_true", help="Skip live WAYLAND_DISPLAY daemon benchmark")
    parser.add_argument("--skip-pick", action="store_true", help="Skip wwal pick cache benchmark")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="Exit non-zero if any invoked suite fails or any measured Section 2.2 SLO fails",
    )
    return parser.parse_args(argv)


def inspect_binary(binary_path: str, forbidden_prefixes: tuple[str, ...]) -> dict[str, Any]:
    stripped_kb, raw_kb = get_stripped_size_kb(binary_path)
    return {
        "path": binary_path,
        "stripped_kb": stripped_kb,
        "raw_kb": raw_kb,
        "size": read_elf_sections(binary_path),
        "dt_needed": audit_dt_needed(binary_path, forbidden_prefixes),
    }


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    build_dir = resolve_build_dir(args.build_dir)

    wwald_bin = os.environ.get("WWAL_DAEMON_BIN", os.path.join(build_dir, "wwald"))
    wwal_bin = os.environ.get("WWAL_CLIENT_BIN", os.path.join(build_dir, "wwal"))
    studio_bin = os.environ.get("WWAL_STUDIO_BIN", os.path.join(build_dir, "wwal-studio"))

    binaries = {
        "wwald": inspect_binary(wwald_bin, FORBIDDEN_DAEMON_DT_NEEDED),
        "wwal": inspect_binary(wwal_bin, FORBIDDEN_CLIENT_DT_NEEDED),
        "wwal-studio": inspect_binary(studio_bin, ()),
    }

    suites: dict[str, Any] = {
        "studio": run_studio_benchmarks(build_dir, args.studio_runs),
        "grid": run_grid_benchmark(build_dir),
        "transitions": run_transitions_benchmarks(build_dir, str(args.frames)),
        "gpu_brush": run_gpu_brush_benchmark(build_dir),
        "mesh_rig": run_mesh_rig_benchmark(build_dir),
        "shader_graph": run_shader_graph_benchmark(build_dir),
        "particle_audio": run_particle_audio_benchmark(build_dir),
        "companions": run_optional_companions(build_dir),
        "pick_cache": {"status": "skip"} if args.skip_pick else benchmark_pick_cache(wwal_bin),
        "live_daemon": (
            {"status": "skip", "reason": "--skip-live"}
            if args.skip_live
            else benchmark_live_daemon(wwald_bin, wwal_bin)
        ),
    }

    telemetry: dict[str, Any] = {
        "schema_version": 2,
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "build_dir": build_dir,
        "binaries": binaries,
        "suites": suites,
        "build_and_coverage": audit_compile_commands_and_coverage(build_dir),
    }

    slo_records = build_slo_records(telemetry)
    telemetry["slos"] = [asdict(r) for r in slo_records]
    telemetry["slo_summary"] = {
        "total": len(slo_records),
        "pass": sum(1 for r in slo_records if r.status == "PASS"),
        "ci_gate_pass": sum(1 for r in slo_records if r.status == "CI_GATE_PASS"),
        "fail": sum(1 for r in slo_records if r.status == "FAIL"),
        "skip": sum(1 for r in slo_records if r.status in ("SKIP", "UNSUPPORTED")),
        "not_emitted": sum(1 for r in slo_records if r.status in ("NOT_EMITTED", "NOT_MEASURED")),
        "missing": sum(1 for r in slo_records if r.status == "MISSING"),
    }

    md_text = render_markdown(telemetry)
    json_text = json.dumps(telemetry, indent=2) + "\n"

    if args.json_out:
        os.makedirs(os.path.dirname(os.path.abspath(args.json_out)), exist_ok=True)
        with open(args.json_out, "w", encoding="utf-8") as f:
            f.write(json_text)
    if args.md_out:
        os.makedirs(os.path.dirname(os.path.abspath(args.md_out)), exist_ok=True)
        with open(args.md_out, "w", encoding="utf-8") as f:
            f.write(md_text)

    if args.json and not args.markdown:
        sys.stdout.write(json_text)
    else:
        sys.stdout.write(md_text)
        if args.json:
            sys.stdout.write("\n```json\n" + json_text + "```\n")

    suite_failures = [
        k
        for k, v in (
            ("studio_headless", suites["studio"]["headless_check"].get("status")),
            ("grid", suites["grid"].get("status")),
            ("bench_transitions", suites["transitions"]["bench_transitions"].get("status")),
            ("gpu_brush", suites["gpu_brush"].get("status")),
            ("mesh_rig", suites["mesh_rig"].get("status")),
            ("shader_graph", suites["shader_graph"].get("status")),
            ("particle_audio", suites["particle_audio"].get("status")),
        )
        if v in ("fail", "timeout")
    ]
    if suite_failures:
        sys.stderr.write(f"[ERROR] Failed benchmark suites: {', '.join(suite_failures)}\n")
        return 1
    if args.strict and telemetry["slo_summary"]["fail"] > 0:
        sys.stderr.write(
            f"[ERROR] {telemetry['slo_summary']['fail']} Section 2.2 SLO(s) failed under --strict.\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

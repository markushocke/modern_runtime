#!/usr/bin/env python3
"""Run independent scheduler benchmark processes and retain failures.

One output directory is created per experiment. No measurement is silently
retried; a nonzero exit, timeout or malformed result remains in runs.csv.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

WORKLOADS = ("pool_dispatch", "pool_multi_producer", "pool_single_flight")
METRICS = ("ops_per_second", "p50_ns", "p95_ns", "p99_ns", "p999_ns")
FIELDS = ("framework", "run", "workload", "workers", "producers", "operations",
          "returncode", "valid", "elapsed_wall_s", "error",
          "cpu_freq_mhz_before", "cpu_freq_mhz_after")

def positive(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("expected positive integer")
    return number

def git(*args):
    try:
        return subprocess.check_output(["git", *args], text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def merged_dev_sha():
    # A benchmark checkout need not have a local dev branch.
    # The latest dev->benchmark sync is a merge; its second parent is the
    # exact source dev revision in this benchmark snapshot.
    parents = git("log", "--first-parent", "--merges", "-1", "--format=%P")
    if parents and len(parents.split()) >= 2:
        return parents.split()[1]
    return git("rev-parse", "--verify", "refs/remotes/origin/dev") or git(
        "rev-parse", "--verify", "refs/heads/dev")

def cpu_frequency_mhz():
    # A best-effort pre/post-run snapshot, not a frequency trace.
    values = []
    for path in Path("/sys/devices/system/cpu/cpufreq").glob("policy*/scaling_cur_freq"):
        try:
            values.append(int(path.read_text().strip()))
        except (OSError, ValueError):
            pass
    return round(sum(values) / len(values) / 1000.0, 1) if values else None


def cpu_governors():
    data = {}
    for path in Path("/sys/devices/system/cpu/cpufreq").glob("policy*/scaling_governor"):
        try:
            data[path.parent.name] = path.read_text().strip()
        except OSError:
            pass
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", action="append", required=True, metavar="NAME=PATH")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runs", type=positive, default=10)
    parser.add_argument("--operations", type=positive, default=100000)
    parser.add_argument("--producers", type=positive, default=4)
    parser.add_argument("--workers", type=positive, nargs="+", default=[1, 2, 4, 8, 16])
    parser.add_argument("--workloads", nargs="+", choices=WORKLOADS, default=list(WORKLOADS))
    parser.add_argument("--timeout", type=positive, default=120)
    parser.add_argument("--taskset", help="Optional Linux CPU list, e.g. 2-7")
    parser.add_argument("--cmake-cache", type=Path,
                        help="CMakeCache.txt used for this benchmark build")
    opts = parser.parse_args()

    binaries = []
    for item in opts.binary:
        name, separator, path = item.partition("=")
        if not separator or not name or not path:
            parser.error("binary must be NAME=PATH")
        path = Path(path).resolve()
        if not path.is_file():
            parser.error("missing executable: " + str(path))
        binaries.append((name, path))
    if len({name for name, _ in binaries}) != len(binaries):
        parser.error("duplicate framework labels")
    if opts.taskset and not shutil.which("taskset"):
        parser.error("--taskset requires the taskset executable")

    output = opts.output
    if opts.cmake_cache and not opts.cmake_cache.is_file():
        parser.error("missing --cmake-cache: " + str(opts.cmake_cache))
    output.mkdir(parents=True, exist_ok=False)
    cache_bytes = opts.cmake_cache.read_bytes() if opts.cmake_cache else b""
    cache_items = {}
    if cache_bytes:
        for line in cache_bytes.decode("utf-8", errors="replace").splitlines():
            if line.startswith(("CMAKE_CXX_COMPILER:", "CMAKE_BUILD_TYPE:",
                                "CMAKE_CXX_FLAGS_RELEASE:", "TBB_DIR:")):
                key, _, value = line.partition("=")
                cache_items[key] = value
    try:
        cpu_model = next(line.split(":", 1)[1].strip()
                         for line in Path("/proc/cpuinfo").read_text().splitlines()
                         if line.startswith("model name"))
    except (OSError, StopIteration):
        cpu_model = platform.processor()
    metadata = {
        "git_head": git("rev-parse", "HEAD"),
        "git_branch": git("branch", "--show-current"),
        "git_dev_sha": merged_dev_sha(),
        "git_status": git("status", "--porcelain"),
        "platform": platform.platform(),
        "python": sys.version,
        "cpu": cpu_model,
        "cmake_cache_sha256": hashlib.sha256(cache_bytes).hexdigest() if cache_bytes else None,
        "cmake_configuration": cache_items,
        "affinity": sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None,
        "cpu_governors": cpu_governors(),
        "cpu_frequency_mhz_initial": cpu_frequency_mhz(),
        "workers": opts.workers,
        "workloads": opts.workloads,
        "producers": opts.producers,
        "operations": opts.operations,
        "runs": opts.runs,
        "timeout_seconds": opts.timeout,
        "taskset": opts.taskset,
        "binaries": {name: {"path": str(path),
                            "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                     for name, path in binaries}
    }
    (output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    failures = 0
    with (output / "runs.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(FIELDS) + list(METRICS))
        writer.writeheader()
        for run in range(1, opts.runs + 1):
            # Rotate scheduler order to avoid systematically favoring the first.
            rotated = binaries[(run - 1) % len(binaries):] + binaries[:(run - 1) % len(binaries)]
            for workers in opts.workers:
                for workload in opts.workloads:
                    for name, binary in rotated:
                        stem = f"{name}-t{workers}-r{run}-{workload}"
                        command = [str(binary), "--workers", str(workers),
                                   "--producers", str(opts.producers),
                                   "--operations", str(opts.operations),
                                   "--workload", workload]
                        if opts.taskset:
                            command = ["taskset", "-c", opts.taskset] + command
                        record = dict(framework=name, run=run, workload=workload,
                                      workers=workers, producers=opts.producers,
                                      operations=opts.operations,
                                      returncode=0, valid=0, error="")
                        record["cpu_freq_mhz_before"] = cpu_frequency_mhz()
                        began = time.monotonic()
                        stdout_path = output / (stem + ".csv")
                        stderr_path = output / (stem + ".stderr")
                        with stdout_path.open("w") as out, stderr_path.open("w") as err:
                            try:
                                finished = subprocess.run(command, stdout=out, stderr=err,
                                                          timeout=opts.timeout, check=False)
                                record["returncode"] = finished.returncode
                            except subprocess.TimeoutExpired:
                                record["returncode"] = 124
                                record["error"] = "timeout"
                        record["elapsed_wall_s"] = round(time.monotonic() - began, 6)
                        record["cpu_freq_mhz_after"] = cpu_frequency_mhz()
                        if record["returncode"] == 0:
                            try:
                                with stdout_path.open(newline="") as data:
                                    measurements = list(csv.DictReader(data))
                                if len(measurements) != 1:
                                    raise ValueError("expected one CSV measurement")
                                measured = measurements[0]
                                if not (measured["framework"] == name and
                                        measured["workload"] == workload and
                                        int(measured["workers"]) == workers and
                                        int(measured["producers"]) == opts.producers and
                                        int(measured["operations"]) == opts.operations and
                                        measured["verified"] == "1"):
                                    raise ValueError("result metadata / verification mismatch")
                                for metric in METRICS:
                                    value = float(measured[metric])
                                    if not (value >= 0 and value < float("inf")):
                                        raise ValueError("invalid metric: " + metric)
                                    record[metric] = value
                                record["valid"] = 1
                            except (OSError, KeyError, ValueError) as exception:
                                record["error"] = str(exception)
                        else:
                            record["error"] = record["error"] or "nonzero exit"
                        if not record["valid"]:
                            failures += 1
                        writer.writerow(record)
                        stream.flush()
                        print(f"{stem}: {'OK' if record['valid'] else 'FAIL'}", flush=True)
    print(f"Completed: {opts.runs * len(opts.workers) * len(opts.workloads) * len(binaries)} processes; "
          f"failures: {failures}")
    return 0 if failures == 0 else 2

if __name__ == "__main__":
    sys.exit(main())

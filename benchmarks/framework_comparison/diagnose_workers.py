#!/usr/bin/env python3
"""Opt-in scheduler loop diagnostics; separate from uninstrumented benchmarks."""
import csv
import os
from pathlib import Path
import re
import subprocess
import sys

FIELDS = (
    "scans", "local_hits", "injected_hits", "steal_hits", "steal_misses",
    "victim_probes", "pending_retries", "park_prepares", "park_cancels",
    "park_waits", "park_returns", "tasks",
)
CONFIGS = (
    ("atomic", "compare_modern_chase_lev_park"),
    ("futex", "compare_modern_chase_lev_futex"),
    ("pooled_atomic", "compare_modern_chase_lev_park_pooled"),
)
WORKLOADS = (
    "pool_worker_fanout", "pool_mixed_compute", "pool_staged_pipeline",
)


def main():
    root = Path(__file__).resolve().parents[2]
    build = Path(os.environ.get(
        "WORKER_DIAG_BUILD_DIR", "build/framework-comparison/cmake"
    ))
    if not build.is_absolute():
        build = root / build
    output = Path(os.environ.get(
        "WORKER_DIAG_OUTPUT", "build/framework-comparison/worker-diagnostics.csv"
    ))
    if not output.is_absolute():
        output = root / output
    workers = int(os.environ.get("WORKER_DIAG_WORKERS", "6"))
    operations = int(os.environ.get("WORKER_DIAG_OPERATIONS", "100000"))
    repeats = int(os.environ.get("WORKER_DIAG_RUNS", "3"))
    if not 1 <= workers <= 256 or not 1 <= operations <= 1_000_000 or not 1 <= repeats <= 100:
        raise ValueError("invalid workers/operations/runs")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as stream:
        columns = [
            "workload", "variant", "workers", "operations", "repeat",
            *FIELDS, "scans_per_task", "victim_probes_per_task",
            "steal_miss_per_task", "pending_retries_per_task",
            "park_waits_per_task",
        ]
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        for workload in WORKLOADS:
            for name, target in CONFIGS:
                binary = build / target
                if not binary.is_file():
                    raise FileNotFoundError(
                        f"Missing {binary}; build the diagnostic targets first"
                    )
                for run in range(repeats):
                    command = [
                        str(binary), "--workers", str(workers),
                        "--operations", str(operations),
                        "--workload", workload, "--profile-only",
                    ]
                    env = os.environ.copy()
                    env["MODERN_RUNTIME_WORKER_DIAGNOSTICS"] = "1"
                    process = subprocess.run(
                        command, capture_output=True, text=True, env=env,
                        timeout=120, check=False,
                    )
                    if process.returncode:
                        raise RuntimeError(
                            f"{name}/{workload} failed ({process.returncode}):\n"
                            + process.stderr[-3000:]
                        )
                    records = re.findall(
                        r"^\[modern-runtime-worker-diagnostics\] (.*)$",
                        process.stderr,
                        flags=re.MULTILINE,
                    )
                    if len(records) != 1:
                        raise RuntimeError(
                            f"Expected exactly one diagnostics line for {name}/{workload}:\n"
                            + process.stderr[-3000:]
                        )
                    values = dict(re.findall(r"([a-z_]+)=(\d+)", records[0]))
                    if any(field not in values for field in FIELDS):
                        raise RuntimeError(f"Incomplete diagnostics: {records[0]}")
                    counts = {field: int(values[field]) for field in FIELDS}
                    expected_tasks = operations * (3 if workload == "pool_staged_pipeline" else 1)
                    expected_tasks += 256  # warmup executes before measurement
                    if workload == "pool_worker_fanout":
                        expected_tasks += 1  # root task creates the leaves
                    if counts["tasks"] != expected_tasks:
                        raise RuntimeError(
                            f"Task mismatch {name}/{workload}: "
                            f"{counts['tasks']} != {expected_tasks}"
                        )
                    row = {
                        "workload": workload, "variant": name,
                        "workers": workers, "operations": operations,
                        "repeat": run, **counts,
                    }
                    for field in (
                        "scans", "victim_probes", "steal_misses",
                        "pending_retries", "park_waits",
                    ):
                        key = "steal_miss_per_task" if field == "steal_misses" else f"{field}_per_task"
                        row[key] = f"{counts[field] / counts['tasks']:.6f}"
                    writer.writerow(row)
                    print(
                        f"{workload:22s} {name:14s} run={run+1} "
                        f"scans/task={row['scans_per_task']} "
                        f"probes/task={row['victim_probes_per_task']} "
                        f"parks/task={row['park_waits_per_task']}",
                        flush=True,
                    )
    print(f"Worker diagnostics: {output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, RuntimeError, FileNotFoundError, subprocess.TimeoutExpired) as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(1)

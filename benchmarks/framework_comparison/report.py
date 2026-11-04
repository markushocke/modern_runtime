#!/usr/bin/env python3
"""Aggregate verified runs. Failed executions remain visible in the report."""
import csv
from collections import defaultdict
from pathlib import Path
import statistics
import sys

METRICS = ("ops_per_second", "p50_ns", "p95_ns", "p99_ns", "p999_ns")

def fmt(value):
    return f"{value:,.2f}" if value is not None else "n/a"

def main(folder):
    source = Path(folder)
    with (source / "runs.csv").open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise SystemExit("no runs.csv measurements")
    groups = defaultdict(list)
    for row in rows:
        groups[(row["workload"], int(row["workers"]), row["framework"])].append(row)
    summary = []
    for (workload, workers, framework), records in sorted(groups.items()):
        valid = [record for record in records if record["valid"] == "1"
                 and record["returncode"] == "0"]
        line = {"workload": workload, "workers": workers, "framework": framework,
                "successful_runs": len(valid), "failed_runs": len(records) - len(valid)}
        for name in METRICS:
            numbers = [float(record[name]) for record in valid]
            line[name + "_median"] = statistics.median(numbers) if numbers else None
            line[name + "_min"] = min(numbers) if numbers else None
            line[name + "_max"] = max(numbers) if numbers else None
            line[name + "_cv_percent"] = (
                100 * statistics.stdev(numbers) / statistics.mean(numbers)
                if len(numbers) > 1 and statistics.mean(numbers) else None)
        summary.append(line)
    fields = ("workload", "workers", "framework", "successful_runs", "failed_runs") + tuple(
        name + suffix for name in METRICS for suffix in
        ("_median", "_min", "_max", "_cv_percent"))
    with (source / "summary.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(summary)

    total_fails = sum(line["failed_runs"] for line in summary)
    out = [
        "# Scheduler framework comparison",
        "",
        f"Runs: {len(rows)}; invalid/failed: {total_fails}.",
        "",
        "Numbers use **per-process medians**, with independent process min–max. "
        "The p99 is enqueue-to-start (including backlog), not isolated wakeup latency.",
        "",
        "A ratio above 1.0 means that framework is faster than modern_runtime for the "
        "same workload and worker count. Ratios are omitted for incomplete groups.",
        "",
        "| Workload | Workers | Framework | Valid / total | Throughput ops/s "
        "(median [min–max]) | p99 µs | Run CV % | vs modern_runtime |",
        "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |"
    ]
    reference = {(line["workload"], line["workers"]): line for line in summary
                 if line["framework"] == "modern_runtime"}
    for line in summary:
        reference_row = reference.get((line["workload"], line["workers"]))
        reference_throughput = reference_row["ops_per_second_median"] if reference_row else None
        throughput = line["ops_per_second_median"]
        if (reference_throughput and throughput and
                reference_row["failed_runs"] == 0 and line["failed_runs"] == 0):
            ratio = f"{throughput / reference_throughput:.2f}x"
        else:
            ratio = "n/a"
        full = line["successful_runs"] + line["failed_runs"]
        window = (f"{fmt(throughput)} [{fmt(line['ops_per_second_min'])}"
                  f"–{fmt(line['ops_per_second_max'])}]")
        p99 = (f"{line['p99_ns_median'] / 1000:.2f}"
               if line["p99_ns_median"] is not None else "n/a")
        cv = fmt(line["ops_per_second_cv_percent"])
        out.append(f"| {line['workload']} | {line['workers']} | {line['framework']} | "
                   f"{line['successful_runs']}/{full} | {window} | {p99} | {cv} | {ratio} |")
    out += [
        "",
        "## Interpretation limits",
        "",
        "- Do not claim superiority if run variation is large, checks fail, or "
        "the compared API semantics differ.",
        "- For stdexec, detached sender construction includes its operation-state "
        "overhead. It is not a direct low-level enqueue comparison.",
        "- 16 workers on a host with fewer than 16 physical cores measures oversubscription.",
        "- The local configuration, binary hashes, CPU affinity and source revisions "
        "are recorded in metadata.json.",
        "- Exact machine/compiler and dependency configuration must match before "
        "comparing across separate runs.",
        "",
        "## Files",
        "",
        "See runs.csv for **all** outcomes, summary.csv for numeric aggregates, "
        "and each *.stderr for individual failures.",
        ""
    ]
    report = source / "report.md"
    report.write_text("\n".join(out))
    print("Report:", report)
    if total_fails:
        print("WARNING: failed/invalid runs present:", total_fails)

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: report.py OUTPUT_DIRECTORY")
    main(sys.argv[1])

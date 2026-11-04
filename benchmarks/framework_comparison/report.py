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
    by_run = {}
    for row in rows:
        groups[(row["workload"], int(row["workers"]), row["framework"])].append(row)
        key = (row["workload"], int(row["workers"]), row["run"], row["framework"])
        if key in by_run:
            raise SystemExit("duplicate benchmark run entry: " + repr(key))
        by_run[key] = row
    summary = []
    for (workload, workers, framework), records in sorted(groups.items()):
        valid = [record for record in records if record["valid"] == "1"
                 and record["returncode"] == "0"]
        line = {"workload": workload, "workers": workers, "framework": framework,
                "successful_runs": len(valid), "failed_runs": len(records) - len(valid)}
        # Pair candidates with modern_runtime from the same repeat/workload/worker
        # configuration. Neither failed run is allowed into the ratio.
        ratios = []
        for row in valid:
            baseline = by_run.get((workload, workers, row["run"], "modern_runtime"))
            if not baseline or baseline["valid"] != "1" or baseline["returncode"] != "0":
                continue
            base_speed = float(baseline["ops_per_second"])
            if base_speed > 0:
                ratios.append(float(row["ops_per_second"]) / base_speed)
        line["paired_runs"] = len(ratios)
        line["paired_ratio_median"] = statistics.median(ratios) if ratios else None
        line["paired_wins"] = sum(ratio > 1.0 for ratio in ratios)
        for name in METRICS:
            numbers = [float(record[name]) for record in valid]
            line[name + "_median"] = statistics.median(numbers) if numbers else None
            line[name + "_min"] = min(numbers) if numbers else None
            line[name + "_max"] = max(numbers) if numbers else None
            line[name + "_cv_percent"] = (
                100 * statistics.stdev(numbers) / statistics.mean(numbers)
                if len(numbers) > 1 and statistics.mean(numbers) else None)
        summary.append(line)
    fields = ("workload", "workers", "framework", "successful_runs", "failed_runs",
              "paired_runs", "paired_ratio_median", "paired_wins") + tuple(
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
        "Paired ratios compare each framework with modern_runtime in the same "
        "measurement round. A value above 1.0 means the competitor was faster. "
        "Missing/failed pairs are omitted from the ratio but remain counted as failures.",
        "",
        "| Workload | Workers | Framework | Valid / total | Throughput ops/s "
        "(median [min–max]) | p99 µs | Run CV % | Paired median ratio | Wins / pairs |",
        "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
    ]
    high_variance = []
    for line in summary:
        throughput = line["ops_per_second_median"]
        if line["paired_runs"] and line["failed_runs"] == 0:
            ratio = f"{line['paired_ratio_median']:.2f}x"
            wins = f"{line['paired_wins']}/{line['paired_runs']}"
        else:
            ratio = "n/a"
            wins = "n/a"
        cv = line["ops_per_second_cv_percent"]
        if cv is not None and cv >= 25:
            high_variance.append((line["workload"], line["workers"], line["framework"], cv))
        full = line["successful_runs"] + line["failed_runs"]
        window = (f"{fmt(throughput)} [{fmt(line['ops_per_second_min'])}"
                  f"–{fmt(line['ops_per_second_max'])}]")
        p99 = (f"{line['p99_ns_median'] / 1000:.2f}"
               if line["p99_ns_median"] is not None else "n/a")
        cv_label = fmt(cv)
        out.append(f"| {line['workload']} | {line['workers']} | {line['framework']} | "
                   f"{line['successful_runs']}/{full} | {window} | {p99} | "
                   f"{cv_label} | {ratio} | {wins} |")
    out += ["", "## Stability warnings", ""]
    if high_variance:
        out.append(f"**{len(high_variance)} configurations** have throughput "
                   "CV >= 25%. Treat speed ratios as exploratory until runs "
                   "are stable and repeat on a quiet pinned host.")
        for workload, workers, framework, cv in high_variance:
            out.append(f"- {workload}, {workers} workers, {framework}: {cv:.1f}% CV")
    else:
        out.append("No throughput configurations exceed the 25% CV warning threshold.")
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
        "- Same-round pairing reduces run-order bias but is not simultaneous. "
        "A median ratio and wins are descriptive, not statistical significance.",
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

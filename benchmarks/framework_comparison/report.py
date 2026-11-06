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
        "p99 is enqueue-to-start (including backlog) for dispatch/fanout/mixed work; "
        "for pool_staged_pipeline it is end-to-end from submission to stage-three completion.",
        "",
        "Paired ratios compare each framework with modern_runtime in the same "
        "measurement round. A value above 1.0 means the competitor was faster. "
        "Missing/failed pairs are omitted from the ratio but remain counted as failures.",
        "",
    ]
    present = {row["framework"] for row in summary}
    if not {"taskflow", "onetbb"} <= present:
        out.extend([
            "**Backend diagnostic ONLY:** the result omits Taskflow and/or oneTBB. "
            "Do not call this a complete framework comparison.",
            "",
        ])
    # Paired A/B: EXACT same Chase-Lev queues, only the worker parking policy
    # differs. Invalid runs on either side never enter the paired estimate.
    out.extend([
        "## Idle parking A/B: Chase-Lev with vs without per-task semaphore",
        "",
        "This section compares identical queues with different wakeup policies.",
        "A ratio above 1.0 means idle parking was faster than the semaphore.",
        "Context switches must be checked independently in perf stat/time -v.",
        "",
        "| Workload | Workers | Paired runs | Parking / semaphore throughput | "
        "Parking wins | Parking / semaphore p99 |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ])
    configs = sorted({(row["workload"], int(row["workers"])) for row in rows})
    for workload, workers in configs:
        speed_ratios = []
        tail_ratios = []
        for repeat in sorted({row["run"] for row in rows
                              if row["workload"] == workload
                              and int(row["workers"]) == workers}):
            park = by_run.get((workload, workers, repeat, "modern_chase_lev_park"))
            base = by_run.get((workload, workers, repeat, "modern_chase_lev"))
            if not park or not base or any(
                r["valid"] != "1" or r["returncode"] != "0"
                for r in (park, base)
            ):
                continue
            throughput = float(base["ops_per_second"])
            p99 = float(base["p99_ns"])
            if throughput > 0:
                speed_ratios.append(float(park["ops_per_second"]) / throughput)
            if p99 > 0:
                tail_ratios.append(float(park["p99_ns"]) / p99)
        ratio = f"{statistics.median(speed_ratios):.2f}x" if speed_ratios else "n/a"
        wins = f"{sum(x > 1 for x in speed_ratios)}/{len(speed_ratios)}" if speed_ratios else "n/a"
        p99ratio = f"{statistics.median(tail_ratios):.2f}x" if tail_ratios else "n/a"
        out.append(f"| {workload} | {workers} | {len(speed_ratios)} | "
                   f"{ratio} | {wins} | {p99ratio} |")
    out.extend([
        "",
        "p99 < 1.0x indicates lower latency with idle parking. Paired ratios "
        "remain exploratory if the independent run CV is high.",
        "",
    ])

    # Isolate the kernel sleep implementation from all queue/parking logic:
    # direct Linux futex vs libstdc++ atomic::wait.
    out.extend([
        "## Direct futex vs atomic-wait idle parking (same Chase-Lev queues)",
        "",
        "Ratio above 1.0x: direct futex achieves higher throughput. Values below",
        "1.0x for p99 mean direct futex has lower tail latency.",
        "",
        "| Workload | Workers | Pairs | Futex / atomic throughput | "
        "Futex wins | Futex / atomic p99 |",
        "| --- | ---: | ---: | ---: | ---: | ---: |",
    ])
    for workload, workers in configs:
        speeds, tails = [], []
        for repeat in sorted({row["run"] for row in rows
                              if row["workload"] == workload
                              and int(row["workers"]) == workers}):
            futex = by_run.get((workload, workers, repeat, "modern_chase_lev_futex"))
            atomic = by_run.get((workload, workers, repeat, "modern_chase_lev_park"))
            if not futex or not atomic or any(
                r["valid"] != "1" or r["returncode"] != "0"
                for r in (futex, atomic)
            ):
                continue
            speed = float(atomic["ops_per_second"])
            tail = float(atomic["p99_ns"])
            if speed > 0:
                speeds.append(float(futex["ops_per_second"]) / speed)
            if tail > 0:
                tails.append(float(futex["p99_ns"]) / tail)
        label = f"{statistics.median(speeds):.2f}x" if speeds else "n/a"
        wins = f"{sum(x > 1 for x in speeds)}/{len(speeds)}" if speeds else "n/a"
        p99 = f"{statistics.median(tails):.2f}x" if tails else "n/a"
        out.append(f"| {workload} | {workers} | {len(speeds)} | "
                   f"{label} | {wins} | {p99} |")
    out.extend(["", "Ratios are from matching rounds; large CV limits conclusions.", ""])

    # Product-relevant cases come first; raw dispatch stays below as a
    # diagnostic. Show competitors even if the runtime-only mode omitted them.
    out.extend([
        "## Real-use-case scorecard (4, 6, 8, 12 and 16 workers)",
        "",
        "| Workload | Workers | Framework | Valid | Ops/s median | "
        "Paired vs direct | Wins/pairs | p99 µs | Run CV % |",
        "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    practical = ("pool_worker_fanout", "pool_mixed_compute", "pool_staged_pipeline")
    for workload in practical:
        for workers in (4, 6, 8, 12, 16):
            for row in summary:
                if row["workload"] != workload or row["workers"] != workers:
                    continue
                if row["paired_runs"] and row["failed_runs"] == 0:
                    ratio = f"{row['paired_ratio_median']:.2f}x"
                    wins = ("reference" if row["framework"] == "modern_runtime" else
                            f"{row['paired_wins']}/{row['paired_runs']}")
                else:
                    ratio, wins = "n/a", "n/a"
                p99 = row["p99_ns_median"]
                cv = row["ops_per_second_cv_percent"]
                out.append(
                    f"| {workload} | {workers} | {row['framework']} | "
                    f"{row['successful_runs']}/"
                    f"{row['successful_runs'] + row['failed_runs']} | "
                    f"{fmt(row['ops_per_second_median'])} | {ratio} | {wins} | "
                    f"{p99 / 1000:.2f} | {fmt(cv)} |"
                    if p99 is not None else
                    f"| {workload} | {workers} | {row['framework']} | "
                    f"{row['successful_runs']}/"
                    f"{row['successful_runs'] + row['failed_runs']} | "
                    f"{fmt(row['ops_per_second_median'])} | {ratio} | {wins} | n/a | {fmt(cv)} |"
                )
    out.extend([
        "",
        "**Note:** fanout measures locally spawned tasks; mixed_compute includes "
        "the identical deterministic CPU body for all frameworks. The staged pipeline "
        "uses three dependent task submissions per completed item and reports "
        "logical items/s rather than scheduled tasks/s. Interpret paired "
        "ratios only when run CV and platform conditions are acceptable.",
        "",
        "## Full matrix (including low-level diagnostics)",
        "",
        "| Workload | Workers | Framework | Valid / total | Throughput ops/s "
        "(median [min–max]) | p99 µs | Run CV % | Paired median ratio | Wins / pairs |",
        "| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |"
    ])
    high_variance = []
    for line in summary:
        throughput = line["ops_per_second_median"]
        if line["paired_runs"] and line["failed_runs"] == 0:
            ratio = f"{line['paired_ratio_median']:.2f}x"
            wins = ("reference" if line["framework"] == "modern_runtime" else
                    f"{line['paired_wins']}/{line['paired_runs']}")
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

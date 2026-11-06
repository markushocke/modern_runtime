#!/usr/bin/env python3
"""Summarize perf self-overhead samples; inspect call stacks for causality."""
from collections import defaultdict
from pathlib import Path
import re
import sys

ROW = re.compile(r"^\s*(\d+(?:\.\d+)?)%\s+(.+)$")
SYMBOL = re.compile(r"\[[^\]]+\]\s*(.*)$")

def category(symbol):
    text = symbol.lower()
    if "emit(" in text or "std::__introsort_loop" in text or "std::__final_insertion_sort" in text:
        return "Benchmark post-processing"
    if "move_only_function::model" in text or "std::_function_handler" in text:
        return "Callable execution/type-erasure"
    if any(x in text for x in ("pthread_mutex", "mutex", "futex", "lll_lock")):
        return "Mutex/futex"
    if any(x in text for x in ("sem_wait", "sem_post", "semaphore", "sched_yield", "nanosleep")):
        return "Waiting/wakeup"
    if any(x in text for x in ("malloc", "operator new", "operator delete",
                               "allocate", "deallocate", "memory_resource",
                               "_int_free", "cfree", "free@", "get_default_resource")):
        return "Allocator / memory management"
    if any(x in text for x in ("bounded_mpmc", "chase_lev", "compare_exchange",
                               "atomic", "steal_task", "pop_local", "push_owner")):
        return "Queue/atomics"
    if any(x in text for x in ("thread_pool", "scheduler", "modern::",
                               "taskflow", "onetbb", "tbb::", "tf::")):
        return "Scheduler/runtime"
    return "Other / unclassified"

def symbols(path):
    records = []
    for line in path.read_text(errors="replace").splitlines():
        match = ROW.match(line)
        if not match:
            continue
        share = float(match.group(1))
        rest = match.group(2)
        mark = SYMBOL.search(rest)
        symbol = mark.group(1).strip() if mark else rest.strip()
        if symbol:
            records.append((share, symbol))
    return sorted(records, reverse=True)

def perf_counters(path):
    """Parse perf stat -x, with both comma-decimal and C locale output."""
    values = {}
    if not path.exists():
        return values
    for line in path.read_text(errors="replace").splitlines():
        line = line.strip()
        if ",msec,task-clock," in line:
            raw = line.split(",msec,task-clock,", 1)[0]
            try:
                values["task-clock"] = float(raw.replace(",", "."))
            except ValueError:
                pass
            continue
        match = re.match(
            r"^([0-9]+),,(cycles|instructions|context-switches|cache-misses),",
            line,
        )
        if match:
            values[match.group(2)] = int(match.group(1))
    return values


def context_switch_breakdown(path):
    """GNU time -v is a separate process from perf stat/record."""
    counts = {}
    if not path.exists():
        return counts
    for line in path.read_text(errors="replace").splitlines():
        match = re.match(
            r"^\s*(Voluntary|Involuntary) context switches:\s*([0-9]+)",
            line,
        )
        if match:
            counts[match.group(1).lower()] = int(match.group(2))
    return counts


def run_metadata(root):
    content = root / "metadata.txt"
    if not content.exists():
        return {}
    return dict(
        line.strip().split("=", 1)
        for line in content.read_text(errors="replace").splitlines()
        if "=" in line
    )


def main(root):
    files = sorted(root.glob("*.perf.report.txt"))
    if not files:
        raise SystemExit("No perf reports found in " + str(root))
    lines = [
        "# Scheduler profiling: hotspot inspection",
        "",
        "Separate symbolized Release runs; samples are self-overhead, not",
        "inclusive costs. Classification is heuristic, not a wall-time",
        "breakdown. This profile-only mode omits post-run benchmark sorting",
        "and output validation; the normal throughput runner must validate",
        "tasks separately. Inspect the associated",
        "perf.data call graphs and stat.csv hardware counters before editing.",
        "These are NOT the uninstrumented throughput benchmark results.",
        "",
    ]
    meta = run_metadata(root)
    operations = int(meta.get("operations", "0"))
    lines.extend([
        "## CPU counters and context switches",
        "",
        "Independent runs: perf stat counters and GNU time -v switch",
        "breakdowns are NOT simultaneous. Values per logical item use",
        "the requested operations count (pipeline: three tasks per item).",
        "",
        "| Workload | Backend | Workers | CS/item (perf) | Voluntary/item (time) | "
        "Involuntary/item (time) | Instructions/item | CPU task-clock ms |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ])
    by_workload = {}
    for path in files:
        stem = path.name.removesuffix(".perf.report.txt")
        if "-" not in stem or "-w" not in stem:
            continue
        workload, remainder = stem.split("-", 1)
        backend, workers = remainder.rsplit("-w", 1)
        counters = perf_counters(root / (stem + ".stat.csv"))
        switches = context_switch_breakdown(root / (stem + ".time.txt"))
        def each(value):
            return f"{value / operations:.4f}" if operations and value is not None else "n/a"
        cs = counters.get("context-switches")
        by_workload[(workload, workers, backend)] = cs
        time_ms = counters.get("task-clock")
        ms_label = f"{time_ms:.1f}" if time_ms is not None else "n/a"
        lines.append(
            f"| {workload} | {backend} | {workers} | "
            f"{each(cs)} | {each(switches.get('voluntary'))} | "
            f"{each(switches.get('involuntary'))} | "
            f"{each(counters.get('instructions'))} | {ms_label} |"
        )
    lines.extend([
        "",
        "### Parking / semaphore context-switch ratio (same Chase-Lev queues)",
        "",
        "| Workload | Workers | Context-switch ratio |",
        "| --- | ---: | ---: |",
    ])
    for (workload, workers, backend), old_cs in sorted(by_workload.items()):
        if backend != "modern_chase_lev":
            continue
        parked_cs = by_workload.get((workload, workers, "modern_chase_lev_park"))
        ratio = f"{parked_cs / old_cs:.4f}x" if old_cs and parked_cs is not None else "n/a"
        lines.append(f"| {workload} | {workers} | {ratio} |")
    lines.extend([
        "",
        "Values below 1.0x indicate fewer context switches. Every variant",
        "was recorded in a separate process; ratios are exploratory.",
        "",
        "## CPU sample hotspots",
        "",
    ])

    for path in files:
        name = path.name.removesuffix(".perf.report.txt")
        lines.extend(["## " + name, ""])
        result = symbols(path)
        if not result:
            lines.extend(["No parsable symbols: inspect raw report/stderr.", ""])
            continue
        lines.extend([
            "| Self % | Category (heuristic) | Sampled symbol |",
            "| ---: | --- | --- |",
        ])
        for share, symbol in result[:15]:
            safe = symbol.replace("|", "\\|")
            lines.append(f"| {share:.2f} | {category(symbol)} | {safe[:150]} |")
        groups = defaultdict(float)
        for share, symbol in result:
            groups[category(symbol)] += share
        lines.extend(["", "### Attribution among visible symbols", ""])
        for key, value in sorted(groups.items(), key=lambda item: -item[1]):
            lines.append(f"- {key}: {value:.1f}%")
        lines.extend([
            "",
            "Verify this hypothesis in call graphs and hardware counters.",
            "CPU frequency, system load, stack unwinding and inlining can",
            "change both the ranking and the number of identified symbols.",
            "",
        ])
    (root / "hotspots.md").write_text("\n".join(lines) + "\n")
    print("Wrote", root / "hotspots.md")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: profile_report.py OUTPUT_DIRECTORY")
    main(Path(sys.argv[1]))

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
    if any(x in text for x in ("pthread_mutex", "mutex", "futex", "lll_lock")):
        return "Mutex/futex"
    if any(x in text for x in ("sem_wait", "sem_post", "semaphore", "sched_yield", "nanosleep")):
        return "Waiting/wakeup"
    if any(x in text for x in ("malloc", "operator new", "operator delete",
                               "allocate", "deallocate", "memory_resource")):
        return "Allocation/PMR"
    if any(x in text for x in ("bounded_mpmc", "chase_lev", "compare_exchange",
                               "atomic", "steal_task", "pop_local", "push_owner")):
        return "Queue/atomics"
    if any(x in text for x in ("thread_pool", "scheduler", "modern::",
                               "taskflow", "onetbb", "tbb::", "tf::")):
        return "Scheduler/runtime"
    return "Other (possibly task body)"

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

def main(root):
    files = sorted(root.glob("*.perf.report.txt"))
    if not files:
        raise SystemExit("No perf reports found in " + str(root))
    lines = [
        "# Scheduler profiling: hotspot inspection",
        "",
        "Separate symbolized Release runs; samples are self-overhead, not",
        "inclusive costs. Categories are heuristics. Inspect the associated",
        "perf.data call graphs and stat.csv hardware counters before editing.",
        "These are NOT the uninstrumented throughput benchmark results.",
        "",
    ]
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

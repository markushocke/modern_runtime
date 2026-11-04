#!/usr/bin/env python3
"""Render final KPI tables from the generated summaries (no dependencies)."""
import csv
from pathlib import Path
import sys

source = Path(sys.argv[1])
output = Path(sys.argv[2])
rows = list(csv.DictReader(source.open()))
index = {(r['version'], r['workload'], r['threads'], r['pmr_mode']): r for r in rows}
workloads = ['pool_single_flight', 'pool_dispatch', 'pool_multi_producer', 'task_submit', 'task_roundtrip']
workers = ['1', '2', '4', '8', '16']
versions = ['cv', 'corrected', 'optimized']

def row(version, workload, workers, mode='default'):
    result = index[version, workload, workers, mode]
    if result['successful_runs'] != '5' or result['failed_runs'] != '0':
        raise ValueError(f'incomplete comparison: {version} {workload} {workers} {mode}')
    return result

def value(r, metric, scale=1):
    return float(r[metric + '_median']) / scale

def span(r, metric, scale=1, digits=2):
    vals = [float(r[metric + '_' + s]) / scale for s in ['median', 'min', 'max']]
    return f'{vals[0]:.{digits}f} [{vals[1]:.{digits}f}–{vals[2]:.{digits}f}]'

lines = ['## Final measured results', '',
    'Five runs per cell, 100,000 operations/run. Values are **median [min–max]**.',
    '`cv` = `88a81d3`; `corrected` = `e23269f`; `optimized` adds only the submit',
    'capture reduction. Timing below uses PMR instrumentation **disabled**.', '',
    '### Completion throughput (million operations/s)', '',
    '| Workload | Workers | CV | Corrected | Optimized | Optimized / CV |',
    '| --- | ---: | ---: | ---: | ---: | ---: |']
for w in workloads:
    for t in workers:
        rr = [row(v, w, t) for v in versions]
        cells = [span(r, 'ops_per_second', 1e6, 3) for r in rr]
        ratio = value(rr[2], 'ops_per_second') / value(rr[0], 'ops_per_second')
        lines.append(f'| {w} | {t} | ' + ' | '.join(cells) + f' | {ratio:.2f}× |')
lines += ['', 'Ratios of run medians are descriptive, not confidence bounds. Read the',
    'ranges and the full coefficients of variation in `final-summary.csv` before',
    'interpreting a ratio as a repeatable speedup.', '',
    '### Latency at 8 and 16 workers (microseconds)', '',
    'Median of per-run percentiles. Full p50/p95/p99/p99.9 ranges for every worker',
    'count and version are in the input summary CSV.', '',
    '| Workload | Workers | CV p99.9 [range] | Optimized p50 / p95 / p99 | Optimized p99.9 [range] |',
    '| --- | ---: | ---: | ---: | ---: |']
for w in workloads:
    for t in ['8', '16']:
        a, b = row('cv', w, t), row('optimized', w, t)
        pp = ' / '.join(f'{value(b, m, 1000):.2f}' for m in ['p50_ns', 'p95_ns', 'p99_ns'])
        lines.append(f"| {w} | {t} | {span(a, 'p999_ns', 1000)} | {pp} | {span(b, 'p999_ns', 1000)} |")
lines += ['', '### PMR allocations (separate instrumented runs)', '',
    'Only allocations through the supplied PMR are counted; global heap',
    'allocations are excluded. Queue block churn makes these counts vary with',
    'worker placement and stealing even for sequential tasks.', '',
    '| Workload | Workers | Corrected allocations/op | Optimized allocations/op | Corrected bytes/op | Optimized bytes/op |',
    '| --- | ---: | ---: | ---: | ---: | ---: |']
for w in ['task_submit', 'task_roundtrip']:
    for t in workers:
        a, b = row('corrected', w, t, 'counting'), row('optimized', w, t, 'counting')
        cells = [span(a, 'pmr_allocs_per_op', digits=3), span(b, 'pmr_allocs_per_op', digits=3),
                 span(a, 'pmr_bytes_per_op', digits=1), span(b, 'pmr_bytes_per_op', digits=1)]
        lines.append(f'| {w} | {t} | ' + ' | '.join(cells) + ' |')
output.write_text('\n'.join(lines) + '\n')

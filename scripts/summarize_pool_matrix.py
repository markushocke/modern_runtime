#!/usr/bin/env python3
"""Merge raw KPI outputs with exit status and summarize successful runs.
Usage: summarize_pool_matrix.py MATRIX_DIRECTORY OUTPUT_PREFIX
"""
import csv
from collections import defaultdict
from pathlib import Path
import statistics
import sys

source, prefix = Path(sys.argv[1]), Path(sys.argv[2])
rows = list(csv.DictReader((source / 'runs.csv').open()))
metrics = ['ops_per_second', 'p50_ns', 'p95_ns', 'p99_ns', 'p999_ns',
           'pmr_allocs_per_op', 'pmr_bytes_per_op']
for row in rows:
    stem = '{version}-t{threads}-r{run}-{workload}-{pmr_mode}'.format(**row)
    measurements = list(csv.DictReader((source / (stem + '.csv')).open()))
    for metric in metrics:
        row[metric] = measurements[0][metric] if measurements else ''
    row['error'] = (source / (stem + '.stderr')).read_text().strip()
prefix.parent.mkdir(parents=True, exist_ok=True)
with prefix.with_suffix('.csv').open('w') as out:
    writer = csv.DictWriter(out, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
keys = ['version', 'workload', 'threads', 'pmr_mode']
groups = defaultdict(list)
for row in rows:
    groups[tuple(row[key] for key in keys)].append(row)
fields = keys + ['successful_runs', 'failed_runs']
fields += [f'{metric}_{stat}' for metric in metrics for stat in ['median', 'min', 'max', 'cv_percent']]
with prefix.with_name(prefix.name + '-summary.csv').open('w') as out:
    writer = csv.DictWriter(out, fieldnames=fields)
    writer.writeheader()
    for key, group in sorted(groups.items()):
        successful = [r for r in group if r['returncode'] == '0' and r['ops_per_second']]
        result = dict(zip(keys, key), successful_runs=len(successful), failed_runs=len(group)-len(successful))
        for metric in metrics:
            values = [float(r[metric]) for r in successful]
            if values:
                mean = statistics.mean(values)
                for name, value in [('median', statistics.median(values)), ('min', min(values)),
                    ('max', max(values)), ('cv_percent', 100*statistics.stdev(values)/mean if len(values)>1 and mean else 0)]:
                    result[f'{metric}_{name}'] = round(value, 3)
        writer.writerow(result)

#!/usr/bin/env python3
"""Run unchanged KPI executables sequentially, alternating version order.

Example: python3 scripts/run_pool_matrix.py --output build/matrix \
  --binary cv=build/performance/cv/modern_runtime_benchmark \
  --binary fixed=build/kpi/modern_runtime_benchmark
Failed runs remain in runs.csv; they are never retried or silently omitted.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary', action='append', required=True, help='label=path')
p.add_argument('--output', type=Path, required=True)
p.add_argument('--runs', type=int, default=5)
p.add_argument('--operations', type=int, default=10000)
p.add_argument('--workers', nargs='+', type=int, default=[1, 2, 4, 8, 16])
p.add_argument('--workloads', nargs='+', default=['pool_single_flight', 'pool_dispatch',
    'pool_multi_producer', 'task_submit', 'task_roundtrip'])
a = p.parse_args()
binaries = [(label, str(Path(path).resolve())) for label, path in
            (entry.split('=', 1) for entry in a.binary)]
a.output.mkdir(parents=True, exist_ok=False)
metadata = dict(platform=platform.platform(), affinity=sorted(os.sched_getaffinity(0)),
                operations=a.operations, runs=a.runs, workers=a.workers,
                workloads=a.workloads, producers=4,
                binaries={label: dict(path=path, sha256=hashlib.sha256(Path(path).read_bytes()).hexdigest())
                          for label, path in binaries})
(a.output / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
fields = ['version', 'run', 'workload', 'threads', 'pmr_mode', 'returncode',
          'wall_seconds', 'voluntary_switches', 'involuntary_switches', 'user_seconds', 'system_seconds']
with (a.output / 'runs.csv').open('w') as output:
    writer = csv.DictWriter(output, fieldnames=fields)
    writer.writeheader()
    for run in range(1, a.runs + 1):
        for workers in a.workers:
            for workload in a.workloads:
                for pmr in ['default', 'counting']:
                    for label, binary in binaries[::1 if run % 2 else -1]:
                        stem = f'{label}-t{workers}-r{run}-{workload}-{pmr}'
                        command = [binary, '--threads', str(workers), '--producers', '4',
                                   '--operations', str(a.operations), '--workload', workload,
                                   '--pmr', pmr]
                        before = resource.getrusage(resource.RUSAGE_CHILDREN)
                        start = time.monotonic()
                        with (a.output / (stem + '.csv')).open('w') as out, (a.output / (stem + '.stderr')).open('w') as err:
                            try:
                                code = subprocess.run(command, stdout=out, stderr=err, timeout=45).returncode
                            except subprocess.TimeoutExpired:
                                code = 124
                        after = resource.getrusage(resource.RUSAGE_CHILDREN)
                        writer.writerow(dict(version=label, run=run, workload=workload,
                            threads=workers, pmr_mode=pmr, returncode=code,
                            wall_seconds=time.monotonic()-start,
                            voluntary_switches=after.ru_nvcsw-before.ru_nvcsw,
                            involuntary_switches=after.ru_nivcsw-before.ru_nivcsw,
                            user_seconds=after.ru_utime-before.ru_utime,
                            system_seconds=after.ru_stime-before.ru_stime))
                        output.flush()
                        print(stem, code, flush=True)

#!/usr/bin/env bash
# Controlled A/B measurement: 32 vs 48 byte inline callable storage.
# Run from any directory with: bash scripts/run_kpi_ab.sh
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

for size in 32 48; do
  echo "== Configure and build SBO-${size} =="
  cmake --preset "kpi-sbo${size}"
  cmake --build --preset "kpi-sbo${size}" -j8
  ctest --test-dir "build/kpi-sbo${size}" --output-on-failure
done

out="kpi-ab"
mkdir -p "$out"
workloads=(pool_dispatch pool_single_flight pool_multi_producer task_submit task_roundtrip)
workers=(1 4 16)
modes=(counting default)

# Alternate build order to reduce CPU frequency and thermal-order bias.
for run in 1 2 3 4 5; do
  if (( run % 2 == 0 )); then
    sizes=(48 32)
  else
    sizes=(32 48)
  fi
  for t in "${workers[@]}"; do
    for workload in "${workloads[@]}"; do
      for pmr in "${modes[@]}"; do
        for size in "${sizes[@]}"; do
          output="$out/sbo${size}-t${t}-r${run}-${workload}-${pmr}.csv"
          echo "SBO=${size} threads=${t} run=${run} ${workload} PMR=${pmr}"
          ./build/kpi-sbo${size}/modern_runtime_benchmark \
            --threads "$t" \
            --producers 4 \
            --operations 10000 \
            --timer-samples 128 \
            --workload "$workload" \
            --pmr "$pmr" > "$output"
        done
      done
    done
  done
done

zip -q -j -FS kpi-ab.zip "$out"/*.csv
echo "Finished: kpi-ab.zip"

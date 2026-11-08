#!/usr/bin/env bash
# Focused uninstrumented A/B: only pending-gated vs unchanged atomic parking.
set -euo pipefail
repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

: "${GATE_BUILD_DIR:=build/framework-comparison/cmake}"
: "${GATE_OUTPUT:=build/framework-comparison/gate-$(date -u +%Y%m%dT%H%M%SZ)}"
: "${GATE_WORKERS:=4 6}"
: "${GATE_WORKLOADS:=pool_worker_fanout pool_mixed_compute pool_staged_pipeline}"
: "${GATE_OPERATIONS:=100000}"
: "${GATE_RUNS:=20}"
: "${GATE_PRODUCERS:=4}"
: "${GATE_BUILD_JOBS:=8}"

compiler="${GATE_CXX:-$(command -v clang++ || true)}"
if [[ -z "$compiler" ]]; then
  echo "clang++ required (C++23 named modules)" >&2
  exit 2
fi
read -r -a workers <<< "$GATE_WORKERS"
read -r -a workloads <<< "$GATE_WORKLOADS"

cmake -S benchmarks/framework_comparison -B "$GATE_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$compiler" \
  -DCOMPARE_WITH_TASKFLOW=OFF -DCOMPARE_WITH_ONETBB=OFF \
  -DCOMPARE_WITH_STDEXEC=OFF -DCOMPARE_WITH_ALLOCATORS=OFF \
  -DCOMPARE_FETCH_DEPS=OFF
cmake --build "$GATE_BUILD_DIR" --parallel "$GATE_BUILD_JOBS" \
  --target compare_modern_chase_lev_park compare_modern_chase_lev_park_gated

args=(--output "$GATE_OUTPUT" --cmake-cache "$GATE_BUILD_DIR/CMakeCache.txt" \
  --runs "$GATE_RUNS" --operations "$GATE_OPERATIONS" \
  --producers "$GATE_PRODUCERS" \
  --workers "${workers[@]}" --workloads "${workloads[@]}" \
  --binary "modern_chase_lev_park=$GATE_BUILD_DIR/compare_modern_chase_lev_park" \
  --binary "modern_chase_lev_park_gated=$GATE_BUILD_DIR/compare_modern_chase_lev_park_gated")
if [[ -n "${GATE_TASKSET:-}" ]]; then args+=(--taskset "$GATE_TASKSET"); fi

# Diagnostic counters would contaminate throughput / p99.
unset MODERN_RUNTIME_WORKER_DIAGNOSTICS

set +e
python3 benchmarks/framework_comparison/run.py "${args[@]}"
result=$?
set -e
python3 benchmarks/framework_comparison/report.py "$GATE_OUTPUT"
echo "Gate A/B report: $GATE_OUTPUT/report.md"
exit "$result"

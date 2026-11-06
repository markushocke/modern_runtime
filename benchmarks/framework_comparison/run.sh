#!/usr/bin/env bash
set -euo pipefail

repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"

: "${COMPARE_BUILD_JOBS:=8}"
: "${COMPARE_RUNS:=10}"
: "${COMPARE_OPERATIONS:=100000}"
: "${COMPARE_PRODUCERS:=4}"
: "${COMPARE_WORKERS:=1 2 4 8 16}"
: "${COMPARE_WORKLOADS:=pool_worker_fanout pool_mixed_compute pool_staged_pipeline pool_dispatch pool_multi_producer pool_single_flight}"
: "${COMPARE_STDEXEC:=0}"
: "${COMPARE_ONLY_RUNTIME:=0}"
: "${COMPARE_OUTPUT:=build/framework-comparison/results-$(date -u +%Y%m%dT%H%M%SZ)}"
: "${COMPARE_BUILD_DIR:=build/framework-comparison/cmake}"

compiler="${COMPARE_CXX:-$(command -v clang++ || true)}"
if [[ -z "$compiler" ]]; then
  echo "ERROR: clang++ is required for modern_runtime's C++23 modules" >&2
  exit 1
fi
read -r -a workers <<< "$COMPARE_WORKERS"
read -r -a workloads <<< "$COMPARE_WORKLOADS"
if [[ "$COMPARE_ONLY_RUNTIME" == "1" && "$COMPARE_STDEXEC" == "1" ]]; then
  echo "ERROR: choose either COMPARE_ONLY_RUNTIME=1 or COMPARE_STDEXEC=1" >&2
  exit 2
fi
compare_external=ON
if [[ "$COMPARE_ONLY_RUNTIME" == "1" ]]; then
  compare_external=OFF
  echo "WARNING: runtime-only diagnostics; Taskflow and oneTBB will NOT be measured." >&2
else
  echo "Full framework comparison: three runtime backends, Taskflow, oneTBB." >&2
fi

cmake -S benchmarks/framework_comparison -B "$COMPARE_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$compiler" \
  -DCOMPARE_WITH_TASKFLOW="$compare_external" \
  -DCOMPARE_WITH_ONETBB="$compare_external" \
  -DCOMPARE_WITH_STDEXEC="$COMPARE_STDEXEC" \
  -DCOMPARE_FETCH_DEPS=ON

targets=(compare_modern compare_modern_injection compare_modern_chase_lev)
if [[ "$COMPARE_ONLY_RUNTIME" != "1" ]]; then
  targets+=(compare_taskflow_binary compare_onetbb_binary)
fi
if [[ "$COMPARE_STDEXEC" == "1" ]]; then
  targets+=(compare_stdexec_binary)
fi
cmake --build "$COMPARE_BUILD_DIR" --parallel "$COMPARE_BUILD_JOBS" \
  --target "${targets[@]}"

args=(--output "$COMPARE_OUTPUT" --cmake-cache "$COMPARE_BUILD_DIR/CMakeCache.txt" --runs "$COMPARE_RUNS" \
      --operations "$COMPARE_OPERATIONS" --producers "$COMPARE_PRODUCERS" \
      --workers "${workers[@]}" --workloads "${workloads[@]}" \
      --binary "modern_runtime=$COMPARE_BUILD_DIR/compare_modern" \
      --binary "modern_injection=$COMPARE_BUILD_DIR/compare_modern_injection" \
      --binary "modern_chase_lev=$COMPARE_BUILD_DIR/compare_modern_chase_lev")
if [[ "$COMPARE_ONLY_RUNTIME" != "1" ]]; then
  args+=(--binary "taskflow=$COMPARE_BUILD_DIR/compare_taskflow_binary"
        --binary "onetbb=$COMPARE_BUILD_DIR/compare_onetbb_binary")
fi
if [[ "$COMPARE_STDEXEC" == "1" ]]; then
  args+=(--binary "stdexec=$COMPARE_BUILD_DIR/compare_stdexec_binary")
fi
if [[ -n "${COMPARE_TASKSET:-}" ]]; then
  args+=(--taskset "$COMPARE_TASKSET")
fi

set +e
python3 benchmarks/framework_comparison/run.py "${args[@]}"
runner_status=$?
set -e

# Always create a report, including partial results and failed runs.
python3 benchmarks/framework_comparison/report.py "$COMPARE_OUTPUT"
echo "Benchmark report: $COMPARE_OUTPUT/report.md"
exit "$runner_status"

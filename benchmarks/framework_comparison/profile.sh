#!/usr/bin/env bash
# perf profiling is separate from the uninstrumented throughput benchmark.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

: "${PROFILE_CXX:=${COMPARE_CXX:-$(command -v clang++ || true)}}"
: "${PROFILE_BUILD_DIR:=build/framework-comparison/profile-cmake}"
: "${PROFILE_OUTPUT:=build/framework-comparison/profile-$(date -u +%Y%m%dT%H%M%SZ)}"
: "${PROFILE_WORKERS:=4}"
: "${PROFILE_PRODUCERS:=4}"
: "${PROFILE_OPERATIONS:=1000000}"
: "${PROFILE_WORKLOADS:=pool_worker_fanout pool_mixed_compute pool_staged_pipeline}"
: "${PROFILE_INCLUDE_COMPETITORS:=1}"
: "${PROFILE_ALLOCATORS:=1}"
: "${PROFILE_BUILD_JOBS:=8}"
: "${PROFILE_FREQUENCY:=499}"
: "${PROFILE_SAMPLE_EVENT:=cycles:u}"

if [[ -z "$PROFILE_CXX" ]]; then
  echo "ERROR: clang++ is required (set PROFILE_CXX)." >&2; exit 2
fi
if ! command -v perf >/dev/null 2>&1; then
  echo "ERROR: Linux perf is not installed." >&2; exit 2
fi
if [[ -n "${PROFILE_TASKSET:-}" ]] && ! command -v taskset >/dev/null 2>&1; then
  echo "ERROR: PROFILE_TASKSET requires taskset." >&2; exit 2
fi
if ! perf stat -e task-clock -- true >/dev/null 2>&1; then
  echo "ERROR: perf lacks permissions. Check perf_event_paranoid and your kernel tools." >&2
  exit 2
fi
if [[ -e "$PROFILE_OUTPUT" ]]; then
  echo "ERROR: profile output exists: $PROFILE_OUTPUT" >&2; exit 2
fi
mkdir -p "$PROFILE_OUTPUT"
read -r -a workloads <<< "$PROFILE_WORKLOADS"

compare_external=ON
if [[ "$PROFILE_INCLUDE_COMPETITORS" != 1 ]]; then compare_external=OFF; fi
# Never mutate the normal Release build: sample a separate symbolized build.
cmake -S benchmarks/framework_comparison -B "$PROFILE_BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="$PROFILE_CXX" \
  -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG -g -fno-omit-frame-pointer" \
  -DCOMPARE_WITH_TASKFLOW="$compare_external" \
  -DCOMPARE_WITH_ONETBB="$compare_external" \
  -DCOMPARE_WITH_STDEXEC=OFF \
  -DCOMPARE_WITH_ALLOCATORS="$([[ "$PROFILE_ALLOCATORS" == "1" ]] && echo ON || echo OFF)"

names=(modern_runtime modern_injection modern_chase_lev modern_chase_lev_park modern_chase_lev_park_pooled)
targets=(compare_modern compare_modern_injection compare_modern_chase_lev compare_modern_chase_lev_park compare_modern_chase_lev_park_pooled)
if [[ "$PROFILE_ALLOCATORS" == "1" ]]; then
  names+=(modern_chase_lev_park_mimalloc modern_chase_lev_park_snmalloc)
  targets+=(compare_modern_chase_lev_park_mimalloc compare_modern_chase_lev_park_snmalloc)
fi
if [[ "$(uname -s)" == "Linux" ]]; then
  names+=(modern_chase_lev_futex)
  targets+=(compare_modern_chase_lev_futex)
fi
if [[ "$PROFILE_INCLUDE_COMPETITORS" == 1 ]]; then
  names+=(taskflow onetbb)
  targets+=(compare_taskflow_binary compare_onetbb_binary)
fi
cmake --build "$PROFILE_BUILD_DIR" --parallel "$PROFILE_BUILD_JOBS" --target "${targets[@]}"

{
  printf 'git_head=%s\n' "$(git rev-parse HEAD)"
  printf 'git_status=%s\n' "$(git status --porcelain | tr '\n' ' ')"
  printf 'kernel=%s\n' "$(uname -r)"
  printf 'cpu=%s\n' "$(grep -m1 'model name' /proc/cpuinfo || true)"
  printf 'compiler=%s\n' "$PROFILE_CXX"
  printf 'workers=%s\noperations=%s\n' "$PROFILE_WORKERS" "$PROFILE_OPERATIONS"
  printf 'taskset=%s\n' "${PROFILE_TASKSET:-none}"
  printf 'perf_event_paranoid=%s\n' "$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
  printf 'cxx_flags=-O3 -DNDEBUG -g -fno-omit-frame-pointer\n'
  printf 'sample_event=%s\n' "$PROFILE_SAMPLE_EVENT"
  printf 'profile_only=1 (post-run validation performed only in normal benchmark)\n'
} > "$PROFILE_OUTPUT/metadata.txt"
if command -v lscpu >/dev/null 2>&1; then
  lscpu -e=CPU,CORE,SOCKET > "$PROFILE_OUTPUT/cpu_topology.txt" || true
fi

for workload in "${workloads[@]}"; do
  for i in "${!names[@]}"; do
    name="${names[$i]}"
    binary="$PROFILE_BUILD_DIR/${targets[$i]}"
    stem="$PROFILE_OUTPUT/${workload}-${name}-w${PROFILE_WORKERS}"
    command=("$binary" --workers "$PROFILE_WORKERS" \
      --producers "$PROFILE_PRODUCERS" --operations "$PROFILE_OPERATIONS" \
      --workload "$workload" --profile-only)
    if [[ -n "${PROFILE_TASKSET:-}" ]]; then
      command=(taskset -c "$PROFILE_TASKSET" "${command[@]}")
    fi
    echo "Profiling: $workload / $name"

    # Hardware counters and sampling are deliberately separate processes.
    perf stat -x, -e task-clock,cycles,instructions,context-switches,cache-misses \
      -o "${stem}.stat.csv" -- "${command[@]}" \
      > "${stem}.stat.stdout" 2> "${stem}.stat.stderr"

    # Default cycles:u restricts sampling to user-space. Kernel symbols
    # often are intentionally hidden via kptr_restrict on developer machines.
    # Override PROFILE_SAMPLE_EVENT=cycles only if kernel profiling is needed
    # and the host is explicitly configured to resolve kernel addresses.
    perf record -e "$PROFILE_SAMPLE_EVENT" -F "$PROFILE_FREQUENCY" -g --call-graph fp \
      -o "${stem}.perf.data" -- "${command[@]}" \
      > "${stem}.record.stdout" 2> "${stem}.record.stderr"

    perf report --stdio --no-children --sort=symbol -n \
      --percent-limit=0.25 -i "${stem}.perf.data" \
      > "${stem}.perf.report.txt"

    # A third, independent run splits voluntary from involuntary context
    # switches. Never treat this as a simultaneous perf-stat measurement.
    if [[ -x /usr/bin/time ]]; then
      /usr/bin/time -v -o "${stem}.time.txt" -- "${command[@]}" \
        > "${stem}.time.stdout" 2> "${stem}.time.stderr"
    fi
  done
done

python3 benchmarks/framework_comparison/profile_report.py "$PROFILE_OUTPUT"
echo "Profiler summary: $PROFILE_OUTPUT/hotspots.md"
echo "Call graphs: perf report -i <file.perf.data>"

#!/usr/bin/env bash
# Merge new runtime work from dev into the benchmark branch without changing dev.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
if [[ "$(git branch --show-current)" != "bench/framework-comparison" ]]; then
  echo "ERROR: switch to bench/framework-comparison before synchronizing" >&2
  exit 2
fi
if [[ -n "$(git status --porcelain)" ]]; then
  echo "ERROR: working tree is not clean; commit/stash changes first" >&2
  exit 2
fi
git fetch origin dev
git merge --no-ff --no-edit origin/dev
echo "Benchmark branch now contains the fetched dev changes."
echo "Review the merge and push the branch explicitly when ready."

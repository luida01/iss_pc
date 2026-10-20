#!/usr/bin/env bash
# Runs the guest-threads bench and saves the output as evidence.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")/.." && pwd)
repo=$(dirname "$here")
cd "$here"
stamp=$(date +%Y%m%d-%H%M%S)
out="$repo/evidence/${stamp}-guest-threads-bench"
mkdir -p "$out"
bash tests/run_guest_threads.sh "$@" > "$out/bench.txt" 2>&1 || true
echo "$out"

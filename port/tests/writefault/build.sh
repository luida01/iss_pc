#!/usr/bin/env bash
# tlg: the write fault bench of the GPU page tracker (see test_write_fault.cpp).
# Builds it in a private directory and runs the modes that matter.
#   bash port/tests/writefault/build.sh          # build + run all the modes
#   bash port/tests/writefault/build.sh --build  # build only
set -euo pipefail
cd -- "$(dirname -- "$0")"
root=$(cd ../../.. && pwd)
export PATH="/c/msys64/clang64/bin:/c/msys64/usr/bin:$PATH"
export MSYSTEM=CLANG64
out="${root}/port/out/gpu-pf/writefault"
mkdir -p "$out"
clang++ -std=c++20 -O2 -g -Wall -Wextra -Werror -static -o "$out/test_write_fault.exe" \
    test_write_fault.cpp -lpthread
if [[ "${1:-}" == "--build" ]]; then
    echo "Built $out/test_write_fault.exe"
    exit 0
fi
exe="$out/test_write_fault.exe"
for mode in none page window merged state guard; do
    "$exe" --mode="$mode" --threads=8 --seconds=3
done

#!/usr/bin/env bash
# tlg: the measurements of the write fault bench (test_write_fault.cpp).
# Each mode is run a few times and the median of the per fault cost is kept: the machine
# shares its cores with the game session and with the other agents, so a single run says
# little.
#
#   bash port/tests/writefault/run.sh [threads]
#
# The modes:
#   none    the writes without any protection (the reference)
#   page    the fault leaves the faulting page accessible (one VirtualProtect of 4 KiB)
#   window  the fault leaves the page and a 256 KiB window accessible: what the port does
#   merged  the same, in one call: the change of the tracker
#   state   like window, but the pages that are already accessible are left alone
#   guard   the pages carry PAGE_GUARD: the kernel drops the guard, the handler protects
#           nothing (the proposal of the report)
set -euo pipefail
cd -- "$(dirname -- "$0")"
root=$(cd ../../.. && pwd)
export PATH="/c/msys64/clang64/bin:/c/msys64/usr/bin:$PATH"
export MSYSTEM=CLANG64
out="${root}/port/out/gpu-pf/writefault"
exe="${out}/test_write_fault.exe"
if [[ ! -x $exe ]]; then
    bash build.sh --build
fi
threads="${1:-8}"
reps="${2:-3}"
# The median of the per fault cost of each mode.
median() {
    printf '%s\n' "$@" | sort -g | awk '{a[NR]=$1} END {print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}'
}
for mode in none page window merged state guard; do
    costs=()
    handlers=()
    for _ in $(seq "$reps"); do
        out_line=$("$exe" --mode="$mode" --threads="$threads" --seconds=3)
        costs+=("$(printf '%s' "$out_line" | sed -n 's/.*mean end-to-end \([0-9.]*\) us.*/\1/p')")
        handlers+=("$(printf '%s' "$out_line" | sed -n 's/.*handler \([0-9.]*\) us,.*/\1/p')")
    done
    cost=$(median "${costs[@]}")
    handler=$(median "${handlers[@]}")
    printf '%-8s threads=%-3s end-to-end %8s us/fault   handler %8s us/fault\n' \
        "$mode" "$threads" "${cost:-n/a}" "${handler:-n/a}"
done

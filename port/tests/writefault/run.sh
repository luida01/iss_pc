#!/usr/bin/env bash
# tlg: the measurements of the write fault bench (test_write_fault.cpp).
# Each mode is run a few times and the median of the per fault cost is kept: the machine
# shares its cores with the game session and with the other agents, so a single run says
# little.
#
#   bash port/tests/writefault/run.sh [threads] [reps]
#
# The modes:
#   none    the writes without any protection (the reference)
#   page    the fault leaves the faulting page accessible (one VirtualProtect of 4 KiB)
#   window  the fault leaves the page and a 256 KiB window accessible: what the port does
#   merged  the same, in one call: the change of the tracker
#   state   like window, but the pages that are already accessible are left alone
#   guard   the pages carry PAGE_GUARD: the kernel drops the guard, the handler protects
#           nothing (the proposal of the report)
#   guardwin the pages carry PAGE_GUARD and the handler still leaves the window accessible:
#           the port's cycle with the guard (the only viable form of the proposal)
#   protect the cost of each protection change (the mode that says why the guard is cheaper)
#   correct the model of the tracker's cycle: does the GPU ever read stale data?
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
for mode in none page window merged state guard guardwin; do
    costs=()
    handlers=()
    stalls=()
    for _ in $(seq "$reps"); do
        out_line=$("$exe" --mode="$mode" --threads="$threads" --seconds=3)
        costs+=("$(printf '%s' "$out_line" | sed -n 's/.*mean end-to-end \([0-9.]*\) us.*/\1/p')")
        handlers+=("$(printf '%s' "$out_line" | sed -n 's/.*handler \([0-9.]*\) us,.*/\1/p')")
        stalls+=("$(printf '%s' "$out_line" | sed -n 's/.*total stall \([0-9.]*\) ms.*/\1/p')")
    done
    cost=$(median "${costs[@]}")
    handler=$(median "${handlers[@]}")
    stall=$(median "${stalls[@]}")
    printf '%-9s threads=%-3s end-to-end %8s us/fault   handler %8s us/fault   stall %8s ms/s\n' \
        "$mode" "$threads" "${cost:-n/a}" "${handler:-n/a}" "${stall:-n/a}"
done
# The same with the work the port's handler does besides protecting (the drain and the
# invalidations, ~48 us): the guard does not remove it.
echo "--- with the port's per fault work (48 us):"
for mode in window merged guardwin guard; do
    costs=()
    stalls=()
    for _ in $(seq "$reps"); do
        out_line=$("$exe" --mode="$mode" --threads="$threads" --seconds=3 --port-cost-us=48)
        costs+=("$(printf '%s' "$out_line" | sed -n 's/.*mean end-to-end \([0-9.]*\) us.*/\1/p')")
        stalls+=("$(printf '%s' "$out_line" | sed -n 's/.*total stall \([0-9.]*\) ms.*/\1/p')")
    done
    cost=$(median "${costs[@]}")
    stall=$(median "${stalls[@]}")
    printf '%-9s threads=%-3s end-to-end %8s us/fault   stall %8s ms/s\n' \
        "$mode" "$threads" "${cost:-n/a}" "${stall:-n/a}"
done
# The protection changes and the correctness model.
"$exe" --mode=protect
"$exe" --mode=correct --read-first=1
"$exe" --mode=correct --read-first=0

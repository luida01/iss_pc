#!/usr/bin/env bash
# Builds and runs the guest memory benches (tests/test_guest_tlb*.c): what page size
# the guest's pool uses on this machine, whether 2 MiB pages can be obtained here, how
# far the TLB reaches, what a protection change costs and what the pagefile backing
# costs when the working set is trimmed.
#
# They measure the host only (no game, no GPU, no Vulkan), so they can run next to a
# session. They are kept out of port/gpu/CMakeLists.txt because that file belongs to the
# renderer/VRAM agents; when they can add it, the targets are:
#
#   add_executable(guest-tlb-test EXCLUDE_FROM_ALL ../tests/test_guest_tlb.c)
#   target_link_libraries(guest-tlb-test PRIVATE psapi onecore)
#
# Usage: bash port/tests/run_guest_tlb.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
  MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    # CreateFileMapping2/MapViewOfFile3 (the pagefile-backed section the runtime uses)
    # are in the onecore API set.
    libs=(-lpsapi -lonecore)
    ext=exe
    ;;
  *)
    libs=()
    ext=
    ;;
esac
mkdir -p out
# test_guest_tlb.c: the main bench. test_guest_tlb3.c: why the section looked 10x more
# expensive. test_guest_tlb4.c: what creating the pool costs.
for bench in test_guest_tlb test_guest_tlb3 test_guest_tlb4; do
  "${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc \
    "tests/$bench.c" "${libs[@]}" -o "out/$bench.$ext"
done
echo "== test_guest_tlb =="
"out/test_guest_tlb.$ext"
echo
echo "== test_guest_tlb3 (the section against plain memory) =="
"out/test_guest_tlb3.$ext"
echo
echo "== test_guest_tlb4 (creating the pool) =="
"out/test_guest_tlb4.$ext"

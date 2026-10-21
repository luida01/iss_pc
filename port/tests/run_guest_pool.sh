#!/usr/bin/env bash
# Builds and runs the guest pool test (tests/test_guest_pool.c): the pool's creation
# and the 2 MiB aligned geometry of the large-page route (TLG_HUGE_PAGES).
#
# The test links the real port/src/runtime_memory.c with stubs of the runtime
# services it uses, so it needs no game, no GPU and no Vulkan.
#
# Usage:
#   bash port/tests/run_guest_pool.sh                 # the normal pool
#   TLG_HUGE_PAGES=align bash port/tests/run_guest_pool.sh
#   TLG_HUGE_PAGES=1 bash port/tests/run_guest_pool.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
  MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    # CreateFileMapping2/MapViewOfFile3 (the pagefile-backed section the runtime uses)
    # are in the onecore API set.
    libs=(-lonecore)
    out=out/test_guest_pool.exe
    ;;
  *)
    libs=()
    out=out/test_guest_pool
    ;;
esac
mkdir -p out
# The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR.
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc \
  tests/test_guest_pool.c src/runtime_memory.c "${libs[@]}" \
  -Wl,--disable-high-entropy-va -o "$out"
"$out"

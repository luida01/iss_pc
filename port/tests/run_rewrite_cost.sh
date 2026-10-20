#!/usr/bin/env bash
# Builds and runs the cost bench of the guest rewrites (tests/test_rewrite_cost.c): what the
# Windows red-zone protection and the ISA emulation stubs cost per execution.
#
# It links only the loader files it measures (isa_rewrite.c, isa_emulate.c, cpu_check.c), so it
# needs no game, no GPU and no Vulkan. It is kept out of port/gpu/CMakeLists.txt because that
# file belongs to the renderer/VRAM agents; when they can add it, the target is:
#
#   add_executable(rewrite-cost-test EXCLUDE_FROM_ALL ../tests/test_rewrite_cost.c
#       ../src/isa_rewrite.c ../src/isa_emulate.c ../src/cpu_check.c)
#   target_include_directories(rewrite-cost-test PRIVATE .. ../src)
#
# The A/B: the same bench with the hit counters of the ISA stubs on and off (TLG_ISA_STATS), and
# with the counters of the red-zone stubs on and off (TLG_REDZONE_STATS).
#
# Usage: bash port/tests/run_rewrite_cost.sh [calls]
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
  MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    extra=(src/compat_win.c)
    libs=(-lbcrypt)
    out=out/rewrite-cost-test.exe
    ;;
  *)
    extra=()
    libs=()
    out=out/rewrite-cost-test
    ;;
esac
mkdir -p out
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc \
  tests/test_rewrite_cost.c src/isa_rewrite.c src/isa_emulate.c src/cpu_check.c "${extra[@]}" \
  "${libs[@]}" -o "$out"
calls=${1:-2000000}
echo "== plain (the counters of the ISA stubs on, the red zone without counters)"
"$out" tests/data/red_zone_image.bin tests/data/red_zone_table.bin "$calls"
echo "== the ISA stubs without their hit counters (TLG_ISA_STATS=0)"
TLG_ISA_STATS=0 "$out" tests/data/red_zone_image.bin tests/data/red_zone_table.bin "$calls"
echo "== the red-zone stubs with their counters (TLG_REDZONE_STATS=1)"
TLG_REDZONE_STATS=1 "$out" tests/data/red_zone_image.bin tests/data/red_zone_table.bin "$calls"

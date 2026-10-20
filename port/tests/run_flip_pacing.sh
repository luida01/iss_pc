#!/usr/bin/env bash
# Builds and runs the bench of the present thread's pacing (tests/test_flip_pacing.cpp).
#
# The bench is a model of the port's present thread (the algorithm of
# port/gpu/shadps4/core/libraries/videoout/driver.cpp, PresentThread): it needs neither the
# game, the GPU nor Vulkan, only a virtual clock.
#
# Usage: bash port/tests/run_flip_pacing.sh [--quick]
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
  MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    out=out/flip-pacing-test.exe
    ;;
  *)
    out=out/flip-pacing-test
    ;;
esac
mkdir -p out
"${CXX:-clang++}" -std=gnu++23 -O2 -g -Wall -Wextra -Werror tests/test_flip_pacing.cpp -o "$out"
"$out" "$@"

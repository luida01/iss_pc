#!/usr/bin/env bash
# Builds and runs the port's synchronization bench (tests/test_sync_latency.c).
#
# The bench links only the runtime files it measures (mutex, condition variable,
# semaphore, threads, kernel) so it does not need the Vulkan library, the game or a
# GPU. It is kept out of port/gpu/CMakeLists.txt because that file belongs to the
# renderer/VRAM agents; when they can add it, the target is:
#
#   add_executable(sync-latency-test EXCLUDE_FROM_ALL ../tests/test_sync_latency.c
#       ../src/runtime_mutex.c ../src/runtime_sema.c ../src/runtime_thread.c
#       ../src/runtime_kernel.c ../src/compat_win.c)
#   target_include_directories(sync-latency-test PRIVATE .. ../src)
#   target_link_libraries(sync-latency-test PRIVATE -pthread)
#
# Usage: bash port/tests/run_sync_latency.sh [--quick]
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
  MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    extra=(src/compat_win.c)
    libs=(-lbcrypt)
    out=out/sync-latency-test.exe
    ;;
  *)
    extra=()
    libs=()
    out=out/sync-latency-test
    ;;
esac
mkdir -p out
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc \
  tests/test_sync_latency.c src/runtime_mutex.c src/runtime_sema.c src/runtime_thread.c \
  src/runtime_kernel.c "${extra[@]}" "${libs[@]}" -o "$out"
"$out" "$@"

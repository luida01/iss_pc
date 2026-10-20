#!/usr/bin/env bash
# Builds and runs the guest-threads bench (tests/test_guest_threads.c): the guest's job
# system on the port's own primitives, the cost of scePthreadYield and the wait map that
# runtime_thread.c now produces.
#
# It links only the runtime files it measures (threads, mutex, condition variable, semaphore,
# rwlock, kernel) so it needs no game, no GPU and no Vulkan. It is kept out of
# port/gpu/CMakeLists.txt because that file belongs to the renderer/VRAM agents; when they
# can add it, the target is:
#
#   add_executable(guest-threads-test EXCLUDE_FROM_ALL ../tests/test_guest_threads.c
#       ../src/runtime_thread.c ../src/runtime_mutex.c ../src/runtime_sema.c
#       ../src/runtime_rwlock.c ../src/runtime_kernel.c ../src/compat_win.c)
#   target_include_directories(guest-threads-test PRIVATE .. ../src)
#   target_link_libraries(guest-threads-test PRIVATE -pthread)
#
# Usage: bash port/tests/run_guest_threads.sh [--quick]
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
case $(uname -s) in
  MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    extra=(src/compat_win.c)
    libs=(-lbcrypt)
    out=out/guest-threads-test.exe
    ;;
  *)
    extra=()
    libs=()
    out=out/guest-threads-test
    ;;
esac
mkdir -p out
"${CC:-clang}" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc \
  tests/test_guest_threads.c src/runtime_thread.c src/runtime_mutex.c src/runtime_sema.c \
  src/runtime_rwlock.c src/runtime_kernel.c "${extra[@]}" "${libs[@]}" -o "$out"
# The A/B of the adaptive spin of the contended guest mutex (TLG_MUTEX_SPIN): the same
# bench with the spin off and with the spin on.
TLG_WAIT_STATS=1 TLG_MUTEX_SPIN=0 "$out" "$@"
TLG_WAIT_STATS=1 TLG_MUTEX_SPIN=40 "$out" "$@"

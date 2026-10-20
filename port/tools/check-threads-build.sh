#!/usr/bin/env bash
# Private compile check of the runtime (the guest-threads agent): every src/runtime*.c and
# src/probe.c, with the project's own flags. It does not touch out/gpu.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
source ./msys2-env.sh
mkdir -p out/threads
# The freetype headers (runtime_gr2_font.c) come from the system or from the local copy.
extra_includes=()
if command -v pkg-config >/dev/null && pkg-config --exists freetype2; then
    read -r -a extra_includes <<< "$(pkg-config --cflags freetype2)"
fi
failed=0
for source in src/runtime*.c src/probe.c; do
    name=$(basename "$source" .c)
    if ! clang -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc "${extra_includes[@]}" \
         -DTLG_BUILD_COMMIT='"threads-check"' -c "$source" -o "out/threads/$name.o" 2>"out/threads/$name.err"; then
        echo "FAILED: $source"
        head -20 "out/threads/$name.err"
        failed=1
    fi
done
[[ $failed -eq 0 ]] && echo "all the runtime files compile"
exit $failed

#!/usr/bin/env bash
# Private build of the whole port (the guest-threads agent): it uses its own build
# directory so it does not touch out/gpu (the renderer/VRAM agents' build).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
source ./msys2-env.sh
for tool in clang cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64." >&2; exit 1; }
done
cmake -S gpu -B out/gpu-threads -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTLG_PGO=off \
    -DTLG_LTO=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ >/dev/null
ninja -C out/gpu-threads tlg-probe tlg-gpu-capabilities tlg-play
echo "private build done: out/gpu-threads"

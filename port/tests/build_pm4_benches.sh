#!/usr/bin/env bash
# tlg: builds the benches and the test of the PM4 decode (the shader recompiler's part).
#
#   bash port/tests/build_pm4_benches.sh [out dir]
#
# The benches do not need the GPU library, the Vulkan driver or the game: they work on
# the shader cache of a real session (.meta files) and on buffers in memory. The build
# uses the MSYS2 CLANG64 toolchain, the same one the port is built with.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
port=$(dirname -- "$here")
out=${1:-"$port/out/gpu-pm4"}
mkdir -p "$out"

clang=${CLANGXX:-/c/msys64/clang64/bin/clang++.exe}
if [[ ! -x $clang ]]; then
    clang=$(command -v clang++)
fi
inc=(-I "$port/gpu/shim" -I "$port/gpu/shadps4" -I "$port/gpu/third_party/gcn/include"
     -I "$port/gpu/third_party/magic_enum/include")
flags=(-O2 -std=c++23)

echo "Building the PM4 decode benches in $out"
"$clang" "${flags[@]}" "${inc[@]}" -o "$out/srt-bench.exe" "$here/srt_bench.cpp"
"$clang" "${flags[@]}" "${inc[@]}" -o "$out/bininfo-bench.exe" "$here/bininfo_bench.cpp"
"$clang" "${flags[@]}" "${inc[@]}" -o "$out/spec-bench.exe" "$here/spec_bench.cpp" \
    "$port/gpu/shadps4/shader_recompiler/frontend/fetch_shader.cpp" \
    "$port/gpu/shadps4/shader_recompiler/frontend/decode.cpp" \
    "$port/gpu/shadps4/shader_recompiler/frontend/format.cpp" \
    "$port/gpu/shim/common/assert.cpp" "$port/gpu/shim/common/logging/log.cpp" -lfmt
"$clang" "${flags[@]}" "${inc[@]}" -o "$out/test-pm4-decode.exe" "$here/test_pm4_decode.cpp" \
    "$port/gpu/shim/common/assert.cpp" "$port/gpu/shim/common/logging/log.cpp" -lfmt
echo "Done: srt-bench, bininfo-bench, spec-bench, test-pm4-decode"

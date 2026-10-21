#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# tlg: cross-check the wave64 census of a run against the SPIR-V the run left in the shader
# cache.
#
# The port's census (shader_recompiler/ir/passes/lower_wave64_pass.cpp) says, for every
# wide compute shader, how many ReadFirstLane it uses and how many of them are in
# divergent flow (where the port does not lower them: the LDS exchange needs a workgroup
# barrier, and a barrier in non uniform control flow is undefined in Vulkan).
#
# The shader cache holds the SPIR-V the run compiled (userdata/cache/<serial>/0x<hash>_0.spv).
# A lowered ReadFirstLane becomes an LDS exchange: the host's
# OpGroupNonUniformBroadcastFirst writes each half's slot, a workgroup barrier orders the
# writes and the whole wave reads the even half's slot. A ReadFirstLane that was NOT
# lowered stays a plain OpGroupNonUniformBroadcastFirst, which on a 32-lane host gives the
# upper half of the wave the value of its own lane 32.
#
# The two must agree: the number of broadcasts of a shader is the number of lowered
# ReadFirstLane plus the number of unlowered ones. This script says when they do not (a
# shader that the census counts as lowered but whose SPIR-V has no exchange, or the other
# way round).
#
# Usage: python spv_rfl_census.py <shader cache dir> [run.log] [--spirv-dis <path>]
import argparse
import re
import subprocess
import sys
from pathlib import Path

BROADCAST = re.compile(r"OpGroupNonUniformBroadcastFirst")
WORKGROUP = re.compile(r"OpTypePointer\s+Workgroup")
BARRIER = re.compile(r"OpControlBarrier|OpMemoryBarrier")
CENSUS = re.compile(
    r"wave64 compute shader (0x[0-9a-f]+) \((\d+) threads[^)]*\): (.*?)(?:\n|$)")


def disassemble(spirv_dis: str, path: Path) -> str:
    try:
        out = subprocess.run([spirv_dis, str(path)], capture_output=True, text=True)
    except OSError as exc:
        print(f"cannot run {spirv_dis}: {exc}")
        return ""
    return out.stdout


def analyse(text: str):
    """(broadcasts, has the LDS exchange)."""
    broadcasts = len(BROADCAST.findall(text))
    exchange = bool(WORKGROUP.search(text)) and bool(BARRIER.search(text))
    return broadcasts, exchange


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path, help="the shader cache directory")
    parser.add_argument("log", type=Path, nargs="?", help="the run log")
    parser.add_argument("--spirv-dis", default="spirv-dis", help="the SPIR-V disassembler")
    args = parser.parse_args()

    shaders = sorted(args.cache.glob("*.spv"))
    print(f"cached shaders: {len(shaders)}")
    with_broadcast = 0
    for path in shaders:
        text = disassemble(args.spirv_dis, path)
        if not text:
            continue
        broadcasts, _ = analyse(text)
        if broadcasts:
            with_broadcast += 1
    print(f"  with OpGroupNonUniformBroadcastFirst (a wave64 ReadFirstLane): {with_broadcast}")

    if not args.log:
        return 0

    text = args.log.read_text(encoding="utf-8", errors="replace")
    agree = disagree = missing = 0
    print("\nagainst the run's census:")
    for match in CENSUS.finditer(text):
        h = match.group(1)
        census = match.group(3)
        divergent = re.search(r"(\d+) ReadFirstLane in divergent flow", census)
        total = re.search(r"ReadFirstLane (\d+)", census)
        n_div = int(divergent.group(1)) if divergent else 0
        n_tot = int(total.group(1)) if total else 0
        path = args.cache / f"0x{int(h, 16):016x}_0.spv"
        if not path.exists():
            missing += 1
            print(f"  {h}: not in the cache")
            continue
        dis = disassemble(args.spirv_dis, path)
        broadcasts, _ = analyse(dis)
        # The exchange emits one host broadcast per lowered ReadFirstLane.
        unlowered = broadcasts - (n_tot - n_div)
        ok = unlowered == n_div
        agree += ok
        disagree += not ok
        print(f"  {h}: census {n_div}/{n_tot} not lowered; {broadcasts} broadcast(s) in the "
              f"SPIR-V -> {unlowered} unlowered {'(agrees)' if ok else '(DISAGREES)'}")
    print(f"  {agree} agree, {disagree} disagree, {missing} not in the cache")
    return 0


if __name__ == "__main__":
    sys.exit(main())

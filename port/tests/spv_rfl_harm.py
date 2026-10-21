#!/usr/bin/env python3
"""Which cached shaders have a wave64 ReadFirstLane that the port left as a host broadcast
AND whose value depends on the lane?

The port lowers a wave64 ReadFirstLane with an LDS exchange: the host's
OpGroupNonUniformBroadcastFirst writes each half's slot, a workgroup barrier orders the
writes and the whole wave reads the even half's slot. That needs a workgroup barrier, and a
barrier in non uniform control flow is undefined in Vulkan, so the port leaves the
ReadFirstLane as it is when it sits in divergent flow
(shader_recompiler/ir/passes/lower_wave64_pass.cpp).

Left as it is, on a 32 lane host the upper half of the wave reads its own lane 32 instead
of the wave's first active lane. That only matters when the broadcast's SOURCE can differ
between lanes: the shader loads it from a constant address, the broadcast is a no-op and
nothing is lost; when the source is a value the lanes computed (a loop carried Phi, an
atomic, a ballot), the upper half of the wave gets a different value and the shader writes
something else.

This script lists the shaders of the cache in the second case: the ones where the missing
exchange can change the result.

Usage: spv_rfl_harm.py <shader cache dir> [--spirv-dis <path>]
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

DEF_RE = re.compile(r"^\s*(%\S+) = (Op\S+)(.*)$")
BCAST_RE = re.compile(r"OpGroupNonUniformBroadcastFirst")
# The instructions whose result depends on the lane.
LANE_VARYING = {
    "OpPhi",             # a value the lanes computed and carried (a loop or a branch)
    "OpAtomicIAdd",
    "OpAtomicISub",
    "OpAtomicSMin",
    "OpAtomicUMin",
    "OpAtomicSMax",
    "OpAtomicUMax",
    "OpAtomicAnd",
    "OpAtomicOr",
    "OpAtomicXor",
    "OpAtomicExchange",
    "OpGroupNonUniformBallot",
    "OpGroupNonUniformElect",
    "OpGroupNonUniformInclusiveScan",
    "OpGroupNonUniformExclusiveScan",
    "OpReadClockKHR",
}
# The values that are the same in every lane: a load from a constant address, an
# immediate, a constant.
UNIFORM_HINT = re.compile(r"buf\d+_dword_off")


def disassemble(spirv_dis: str, path: Path) -> str:
    out = subprocess.run([spirv_dis, str(path)], capture_output=True, text=True)
    return out.stdout


def analyse(text: str):
    """(broadcasts in the LDS exchange, broadcasts left as they are)."""
    defs = {}
    order = []
    for line in text.splitlines():
        m = DEF_RE.match(line)
        if m:
            defs[m.group(1)] = (m.group(2), m.group(3))
            order.append(m.group(1))
    broadcasts = [name for name in order
                  if defs[name][0] == "OpGroupNonUniformBroadcastFirst"]
    if not broadcasts:
        return [], []
    # The exchange: a broadcast whose result is stored into a Workgroup variable.
    stored = set()
    for line in text.splitlines():
        m = re.match(r"^\s*OpStore (%\S+) (%\S+)", line)
        if not m:
            continue
        pointer, value = m.group(1), m.group(2)
        entry = defs.get(pointer)
        if entry and "Workgroup" in entry[1]:
            stored.add(value)
    exchanged = [b for b in broadcasts if b in stored]
    left = [b for b in broadcasts if b not in stored]
    return exchanged, left


def source_lane_dependent(text: str, name: str) -> bool:
    """Whether the value the broadcast takes can differ between the lanes.

    The strong signals are the values the lanes computed: a Phi (a value carried through a
    loop or a branch), an atomic, a ballot. A load is not a signal by itself: its address
    decides, and the walk follows the pointer, so a load from a constant address (the
    game's flat user data at a fixed offset) ends at a constant and is neutral.
    """
    defs = {}
    for line in text.splitlines():
        m = DEF_RE.match(line)
        if m:
            defs[m.group(1)] = (m.group(2), m.group(3))

    def walk(value, seen, depth=0):
        if depth > 60 or value in seen:
            return False
        seen.add(value)
        entry = defs.get(value)
        if entry is None:
            return False
        opcode, args = entry
        if opcode in LANE_VARYING:
            return True
        for dep in re.findall(r"%\S+", args):
            if walk(dep, seen, depth + 1):
                return True
        return False

    entry = defs.get(name)
    if entry is None:
        return False
    ids = re.findall(r"%\S+", entry[1])
    if not ids:
        return False
    return walk(ids[-1], set())


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("cache", type=Path)
    parser.add_argument("--spirv-dis", default="spirv-dis")
    args = parser.parse_args()

    harmful = []
    total = 0
    for path in sorted(args.cache.glob("*.spv")):
        text = disassemble(args.spirv_dis, path)
        if not text:
            continue
        exchanged, left = analyse(text)
        if not left:
            continue
        total += 1
        if any(source_lane_dependent(text, name) for name in left):
            harmful.append((path.stem, len(left), len(exchanged)))
    print(f"cached shaders: {len(list(args.cache.glob('*.spv')))}")
    print(f"  with a ReadFirstLane left as a host broadcast: {total}")
    print(f"  of those, with a source that depends on the lane: {len(harmful)}")
    for name, left, exchanged in harmful:
        print(f"    {name}: {left} broadcast(s) left as they are ({exchanged} exchanged)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

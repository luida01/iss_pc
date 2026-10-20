"""Which of the red-zone functions the port rewrites are the hot ones, and what they cost.

The rewrite table (out/isa-rewrite.bin) says which instructions are protected and where the stub
of every group is; the linked image (out/boot-linked.bin) says which function every address
belongs to (the .eh_frame_hdr search tables, scripts/red_zone.py). This tool joins both:

* the 196 red-zone functions with the number of protected instructions each one carries (the
  cost the rewrite pays every time the function runs that far), and
* the same functions ranked by how many times those instructions ran, when the counters the
  loader writes (TLG_REDZONE_STATS=<file>) are given.

The cost per protected instruction is measured by tests/test_rewrite_cost.c (a direct stub:
about 0.4 ns, a stub reached through a short-jump relay: about 1.2 ns); the estimate below uses
those numbers and says how much of a core the rewrite takes at that rate.

Usage:
  python3 tests/red_zone_hot.py --table out/isa-rewrite.bin --image out/boot-linked.bin
  python3 tests/red_zone_hot.py --table out/isa-rewrite.bin --image out/boot-linked.bin \
      --hits out/redzone-hits.txt
"""
import argparse
import bisect
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))

import isa_rewrite  # noqa: E402
import red_zone  # noqa: E402

# Measured by tests/test_rewrite_cost.c on the i5-13420H (nanoseconds per protected instruction).
DIRECT_NS = 0.4
RELAY_NS = 1.2


def read_groups(table):
    """(site, stub, bytes, protected, patches) of every group of the red-zone section.

    Every patch is (offset, length, new bytes)."""
    magic, size, digest, count, unsupported, rz_bytes = isa_rewrite.HEADER.unpack_from(table, 0)
    if magic != b'G3ISA1\0\0':
        raise ValueError('not a G3ISA1 table')
    if not rz_bytes:
        return [], {}, 0
    section = table[isa_rewrite.HEADER.size + isa_rewrite.RECORD.size * count:]
    head = red_zone.SECTION.unpack_from(section, 0)
    if head[0] != red_zone.MAGIC:
        raise ValueError('not a G3RZONE1 section')
    stats = dict(zip(red_zone.STATS_FIELDS, red_zone.STATS.unpack_from(section, red_zone.SECTION.size)))
    at = red_zone.SECTION.size + red_zone.STATS.size + head[2]
    groups = []
    for _ in range(head[3]):
        site, stub, stub_bytes, protected, patches = red_zone.GROUP.unpack_from(section, at)
        at += red_zone.GROUP.size
        entries = []
        for _ in range(patches):
            offset, length = red_zone.PATCH.unpack_from(section, at)
            at += red_zone.PATCH.size
            entries.append((offset, length, section[at + length:at + 2 * length]))
            at += 2 * length
        groups.append((site, stub, stub_bytes, protected, entries))
    return groups, stats, head[1]   # the offset of the stub code in the image


def check_groups(groups, code_offset, code_bytes):
    """The loader counts every protected instruction with a trampoline between the site's jmp and
    the stub. This checks the assumption the loader makes on every group: the patch that holds the
    site starts with a jmp to the stub (or with a short jmp to a relay whose jmp near is in another
    patch of the same group)."""
    direct = relay = broken = 0
    for site, stub, stub_bytes, protected, patches in groups:
        holding = [p for p in patches if p[0] <= site < p[0] + p[1]]
        if len(holding) != 1:
            broken += 1
            continue
        offset, length, new = holding[0]
        if new[0] == 0xE9:
            target = offset + 5 + struct.unpack_from('<i', new, 1)[0]
            if not code_offset <= target < code_offset + code_bytes:
                broken += 1
                continue
            direct += 1
        elif new[0] == 0xEB:
            where = site + 2 + struct.unpack_from('<b', new, 1)[0]
            found = False
            for other_offset, other_length, other in patches:
                if other_offset <= where < other_offset + other_length and where - other_offset + 5 <= other_length \
                        and other[where - other_offset] == 0xE9:
                    target = where + 5 + struct.unpack_from('<i', other, where - other_offset + 1)[0]
                    found = code_offset <= target < code_offset + code_bytes
                    break
            if found:
                relay += 1
            else:
                broken += 1
        else:
            broken += 1
    return direct, relay, broken


def read_sites(table):
    """The ISA sites of the table: (offset, kind, mode)."""
    magic, size, digest, count, unsupported, rz_bytes = isa_rewrite.HEADER.unpack_from(table, 0)
    sites = []
    at = isa_rewrite.HEADER.size
    for _ in range(count):
        record = isa_rewrite.RECORD.unpack_from(table, at)
        at += isa_rewrite.RECORD.size
        sites.append(record)
    return sites


def read_counts(path):
    """The counters the loader wrote: {address: count}."""
    counts = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2:
            counts[int(parts[0], 16)] = int(parts[1])
    return counts


def function_of(starts, ends, address):
    """The function that contains `address`, or None."""
    i = bisect.bisect_right(starts, address) - 1
    if i < 0:
        return None
    return (starts[i], ends[i]) if address < ends[i] else None


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--table', type=Path, default=ROOT / 'out/isa-rewrite.bin')
    p.add_argument('--image', type=Path, default=ROOT / 'out/boot-linked.bin')
    p.add_argument('--hits', type=Path, help='the counters the loader wrote (TLG_REDZONE_STATS)')
    p.add_argument('--isa-hits', type=Path, help='the counters of the ISA sites (TLG_ISA_STATS)')
    p.add_argument('--top', type=int, default=25)
    a = p.parse_args(argv)
    table = a.table.read_bytes()
    groups, stats, code_offset = read_groups(table)
    if not groups:
        print('the table has no red-zone section')
        return 1
    direct, relay, broken = check_groups(groups, code_offset, stats.get('code_bytes', 0) or 1 << 30)
    print(f'groups: {direct} direct stubs, {relay} through a short-jump relay, {broken} the loader cannot count')
    segments, image = isa_rewrite.read_boot_linked(a.image.read_bytes())
    functions = red_zone.find_functions(image, segments)
    starts = [f[0] for f in functions]
    ends = [f[1] for f in functions]
    print(f'Red zone: {stats["protected"]} instructions protected in {stats["groups"]} patches over '
          f'{stats["red_zone_functions"]} red-zone functions of {stats["functions"]} '
          f'({stats["conservative_functions"]} analysed conservatively); '
          f'{stats["short_relays"]} through short-jump relays, {stats["skipped_unrelocatable"]} unrelocatable')
    per_function = {}
    outside = 0
    for site, stub, stub_bytes, protected, patches in groups:
        owner = function_of(starts, ends, site)
        if owner is None:
            outside += 1
            continue
        entry = per_function.setdefault(owner, {'groups': 0, 'instructions': 0, 'sites': [], 'relays': 0})
        entry['groups'] += 1
        entry['instructions'] += protected
        entry['sites'].append(site)
        if len(patches) > 1:
            entry['relays'] += 1
    if outside:
        print(f'{outside} groups are outside every function of the .eh_frame_hdr tables')
    ranked = sorted(per_function.items(), key=lambda kv: (-kv[1]['instructions'], kv[0][0]))
    print(f'\n{len(ranked)} functions carry protected instructions:')
    print(f'  {"function":>18} {"groups":>7} {"protected":>10} {"relays":>7}  cost per full pass')
    for (start, end), entry in ranked[:a.top]:
        cost = entry['instructions'] * DIRECT_NS + entry['relays'] * (RELAY_NS - DIRECT_NS)
        print(f'  0x{start:08x}-0x{end:08x} {entry["groups"]:7d} {entry["instructions"]:10d} '
              f'{entry["relays"]:7d}  {cost:8.1f} ns')
    total_instructions = sum(e['instructions'] for e in per_function.values())
    total_relays = sum(e['relays'] for e in per_function.values())
    print(f'\nall of them: {total_instructions} protected instructions, {total_relays} relays; a function that '
          f'runs all of them pays about '
          f'{(total_instructions * DIRECT_NS + total_relays * (RELAY_NS - DIRECT_NS)) / 1000:.1f} us')
    if not a.hits:
        print('\n(run the game with TLG_REDZONE_STATS=<file> to get the counters and pass --hits)')
    else:
        hits = read_counts(a.hits)
        per_function_hits = {}
        for site, count in hits.items():
            owner = function_of(starts, ends, site)
            if owner is None:
                continue
            entry = per_function_hits.setdefault(owner, {'hits': 0, 'sites': 0})
            entry['hits'] += count
            if count:
                entry['sites'] += 1
        total_hits = sum(hits.values())
        ranked_hits = sorted(per_function_hits.items(), key=lambda kv: (-kv[1]['hits'], kv[0][0]))
        print(f'\n{len(hits)} protected instructions counted, {total_hits} executions in the run')
        print(f'  {"function":>18} {"executions":>12} {"sites":>6}  estimated cost')
        for (start, end), entry in ranked_hits[:a.top]:
            print(f'  0x{start:08x}-0x{end:08x} {entry["hits"]:12d} {entry["sites"]:6d}')
        # The cost estimate: the measured nanoseconds per protected instruction.
        relays = set()
        for owner, entry in per_function.items():
            if entry['relays']:
                relays.update(entry['sites'])
        cost_ns = sum(count * (RELAY_NS if site in relays else DIRECT_NS) for site, count in hits.items())
        print(f'\nestimated cost of the red-zone rewrite: {cost_ns / 1e6:.1f} ms of CPU '
              f'({cost_ns / 1e7:.2f}% of one core per second of run)')
    if a.isa_hits:
        isa_counts = read_counts(a.isa_hits)
        sites = read_sites(table)
        kinds = {1: 'movbe', 2: 'movbe', 3: 'lzcnt', 4: 'tzcnt', 5: 'andn', 6: 'bextr', 7: 'blsi',
                 8: 'blsmsk', 9: 'blsr', 10: 'vcvtph2ps', 11: 'vcvtps2ph', 12: 'extrq', 13: 'extrq',
                 14: 'insertq', 15: 'insertq', 16: 'movntss', 17: 'movntsd'}
        total_isa = sum(isa_counts.values())
        print(f'\nISA: {total_isa} emulations over {len(sites)} sites')
        print(f'  {"site":>12} {"kind":>8} {"executions":>12}  function')
        hot = sorted(((count, site) for site, count in isa_counts.items() if count), reverse=True)
        for count, site in hot[:a.top]:
            record = next((r for r in sites if r[0] == site), None)
            kind = kinds.get(record[2], '?') if record else '?'
            owner = function_of(starts, ends, site)
            where = f'0x{owner[0]:08x}-0x{owner[1]:08x}' if owner else '(outside every function)'
            print(f'  0x{site:08x} {kind:>8} {count:12d}  {where}')
        # The emulation stub costs about 11 ns (measured by tests/test_rewrite_cost.c).
        print(f'\nestimated cost of the ISA emulation: {total_isa * 11 / 1e6:.1f} ms of CPU '
              f'({total_isa * 11 / 1e7:.2f}% of one core per second of run)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

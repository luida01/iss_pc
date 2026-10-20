#!/usr/bin/env python3
"""Reads a shader cache .meta file and prints the tail layout (the SRT walker).

The .meta file is written by Vulkan::RegisterShaderMeta (vk_pipeline_serialization.cpp):
    u32 meta_version, u32 binary_version, u64 perm_hash, u64 perm_idx
    StageSpecialization::Serialize
    Info::Serialize:
        InfoPersistent (raw bytes)
        flattened_ud_buf (u64 count, count * u32)
        PersistentSrtInfo (raw bytes) + walker machine code

The SRT block is at the end, so the tail can be read without the specification:
the walker size is inside PersistentSrtInfo, right before the code.
"""
import struct
import sys
from pathlib import Path


def parse_tail(data: bytes):
    """The SRT block and the walker code at the end of a .meta file.

    The block (PersistentSrtInfo) is written before the walker code, and the file can
    end with a few padding bytes, so the block is at
    size - padding - 24 - walker_size. The walker always ends with "pop rdi; ret",
    which tells the two apart.
    """
    size = len(data)
    for p in range(size - 24, max(0, size - 65536), -1):
        ws = struct.unpack_from('<Q', data, p + 8)[0]
        if not 0 <= ws < 1 << 20:
            continue
        end = p + 24 + ws
        if end > size or size - end > 8:
            continue
        flat_size = struct.unpack_from('<I', data, p + 16)[0]
        if flat_size < 16 or flat_size > 4096:
            continue
        if ws >= 2 and data[end - 2:end] != b'\x5f\xc3':
            continue
        # Before the SRT block: the flattened user data (u64 count + count * u32) and the
        # InfoPersistent block, whose size this script does not know.
        count = struct.unpack_from('<Q', data, p - 8)[0] if p >= 8 else None
        return {
            'srt_pos': p,
            'walker_size': ws,
            'flattened_bufsize_dw': flat_size,
            'code': data[p + 24:end],
            'flat_count': count,
        }
    return None


def main() -> int:
    if len(sys.argv) < 2:
        print('usage: srt_meta_probe.py <cache dir> [limit]', file=sys.stderr)
        return 2
    cache = Path(sys.argv[1])
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 5
    metas = sorted(cache.glob('*.meta'))
    print(f'{len(metas)} .meta files in {cache}')
    shown = 0
    total_walker = 0
    with_walker = 0
    without = 0
    entries = []
    for m in metas:
        data = m.read_bytes()
        t = parse_tail(data)
        if t is None:
            print(f'{m.name}: tail not understood')
            continue
        if t['walker_size']:
            total_walker += t['walker_size']
            with_walker += 1
            entries.append(t['flattened_bufsize_dw'] - 16)
        else:
            without += 1
        if shown < limit:
            shown += 1
            print(f'{m.name}: walker {t["walker_size"]} bytes, '
                  f'flattened_bufsize_dw {t["flattened_bufsize_dw"]}, '
                  f'entries {t["flattened_bufsize_dw"] - 16}')
            print('   code:', t['code'][:48].hex(' '))
    if entries:
        entries.sort()
        n = len(entries)
        print(f'\n{with_walker}/{len(metas)} shaders have a walker ({without} without); '
              f'{total_walker} bytes of walker code in total')
        print(f'entries per walk: min {entries[0]}, median {entries[n // 2]}, '
              f'mean {sum(entries) / n:.1f}, max {entries[-1]}')
    return 0


if __name__ == '__main__':
    sys.exit(main())

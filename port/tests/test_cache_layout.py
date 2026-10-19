# SPDX-License-Identifier: GPL-2.0-or-later
"""The layout of the texture cache's hot data: the bind path reads it for every bound texture.

The C++ part (tests/test_cache_layout.cpp) measures the layout of the real structures and walks
a pool of images the way the bind path does; it is compiled and run here.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import paths  # noqa: F401  (puts scripts/ on sys.path)
from paths import ROOT

GPU_DIR = ROOT / 'gpu' / 'shadps4'
TEST_SOURCE = ROOT / 'tests' / 'test_cache_layout.cpp'
HEADERS = (GPU_DIR / 'video_core' / 'texture_cache' / 'image.h',
           GPU_DIR / 'video_core' / 'texture_cache' / 'image_info.h')


def compiler():
    """A C++ compiler, or None. MSYS2's clang on Windows, the system one elsewhere."""
    found = shutil.which(os.environ.get('CXX', '') or 'none')
    if found:
        return found
    for name in ('clang++', 'g++', 'c++'):
        found = shutil.which(name)
        if found:
            return found
    if os.name == 'nt':
        for root in (os.environ.get('MSYS2_ROOT', r'C:\msys64'), r'C:\msys64'):
            candidate = Path(root) / 'clang64' / 'bin' / 'clang++.exe'
            if candidate.exists():
                return str(candidate)
    return None


def includes():
    """The include paths the video core is built with (the shim's logging comes first: it is
    the one that does not need spdlog)."""
    gpu = ROOT / 'gpu'
    return ['-I', str(gpu / 'shim'), '-I', str(GPU_DIR),
            '-I', str(gpu / 'third_party' / 'vulkan-headers' / 'include'),
            '-I', str(gpu / 'third_party' / 'gcn' / 'include'),
            '-I', str(gpu / 'third_party' / 'half' / 'include')]


def run_env():
    """The environment for the test binary: on Windows the MSYS2 runtime DLLs (the port's
    toolchain) have to be in the PATH or the binary does not start (0xC0000135)."""
    env = dict(os.environ)
    if os.name == 'nt':
        for root in (os.environ.get('MSYS2_ROOT', r'C:\msys64'), r'C:\msys64'):
            runtime = Path(root) / 'clang64' / 'bin'
            if runtime.exists():
                env['PATH'] = f'{runtime}{os.pathsep}{env.get("PATH", "")}'
                break
    return env


class LayoutTest(unittest.TestCase):
    def test_the_layout_is_measured_and_the_bind_path_reads_one_line(self):
        cxx = compiler()
        if not cxx:
            self.skipTest('no C++ compiler found')
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'cache-layout-test'
            command = [cxx, '-std=gnu++23', '-O2', '-UNDEBUG', *includes(),
                       str(TEST_SOURCE), '-o', str(binary)]
            build = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
            self.assertEqual(build.returncode, 0, build.stdout.decode('utf-8', 'replace'))
            run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 timeout=120, env=run_env())
            output = run.stdout.decode('utf-8', 'replace')
            self.assertEqual(run.returncode, 0, output)
            # The number the test prints: the fields the bind path reads are in one cache line.
            self.assertIn('the bind path touches 1 cache lines', output)

    def test_the_hot_fields_are_declared_first(self):
        # The guard against a reordering that puts them apart again: the fields the bind path
        # reads come before the big cold ones (the mip layout, the metadata addresses).
        info = HEADERS[1].read_text(encoding='utf-8')
        self.assertLess(info.index('ImageProperties props'), info.index('mips_layout'))
        self.assertLess(info.index('vk::Format pixel_format'), info.index('mips_layout'))
        self.assertLess(info.index('SubresourceExtent resources'), info.index('mips_layout'))
        image = HEADERS[0].read_text(encoding='utf-8')
        self.assertLess(image.index('ImageFlagBits flags'), image.index('ImageInfo info;'))
        self.assertLess(image.index('BackingImage* backing{};'), image.index('ImageInfo info;'))


if __name__ == '__main__':
    unittest.main()

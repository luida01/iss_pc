# SPDX-License-Identifier: GPL-2.0-or-later
"""The texture cache's VRAM budget: the policy (C++), the launcher's plumbing and the presets.

The C++ part (tests/test_gc_budget.cpp) is compiled and run here, so the policy the port uses
is the one this test checks. The rest is the launcher: the advanced switches that reach the
game, the budget the launch sends, and the two 4 GB presets.
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

sys.path.insert(0, str(ROOT / 'launcher'))
import advanced_table  # noqa: E402
import backports_table  # noqa: E402
import infamous_run  # noqa: E402
import presets_vram  # noqa: E402

GPU_DIR = ROOT / 'gpu' / 'shadps4'
TEST_SOURCE = ROOT / 'tests' / 'test_gc_budget.cpp'


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


class PolicyTest(unittest.TestCase):
    """The C++ policy: the collector's share of the budget and the working set it keeps."""

    def test_policy_compiles_and_passes(self):
        cxx = compiler()
        if not cxx:
            self.skipTest('no C++ compiler found')
        with tempfile.TemporaryDirectory() as folder:
            binary = Path(folder) / 'gc-budget-test'
            command = [cxx, '-std=gnu++23', '-O1', '-UNDEBUG', '-I', str(GPU_DIR),
                       str(TEST_SOURCE), '-o', str(binary)]
            build = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
            self.assertEqual(build.returncode, 0, build.stdout.decode('utf-8', 'replace'))
            run = subprocess.run([str(binary)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout.decode('utf-8', 'replace'))


class AdvancedRowsTest(unittest.TestCase):
    """The launcher's advanced screen: the switches the collector reads."""

    def rows(self):
        return {item.env: item for item in backports_table.ADVANCED}

    def test_the_gc_rows_are_there(self):
        rows = self.rows()
        self.assertIn('TLG_GC_POLICY', rows)
        self.assertIn('TLG_GC_FLOOR_SHARE', rows)
        self.assertEqual(rows['TLG_GC_POLICY'].default, '1')     # the new rule is the default
        self.assertEqual(rows['TLG_GC_FLOOR_SHARE'].default, '25')
        self.assertEqual(rows['TLG_GC_FLOOR_SHARE'].lo, 5)
        self.assertEqual(rows['TLG_GC_FLOOR_SHARE'].hi, 100)

    def test_the_names_exist_in_the_sources(self):
        text = '\n'.join(path.read_text(encoding='utf-8', errors='replace')
                         for path in (ROOT / 'gpu').rglob('*')
                         if path.suffix in ('.c', '.cpp', '.h', '.hpp') and path.is_file())
        for name in ('TLG_GC_POLICY', 'TLG_GC_FLOOR_SHARE'):
            self.assertIn(f'"{name}"', text, name)

    def test_the_defaults_match_the_policy(self):
        # The C++ defaults of the collector (gc_budget.h) and the launcher's rows.
        header = (GPU_DIR / 'video_core' / 'texture_cache' / 'gc_budget.h').read_text(encoding='utf-8')
        self.assertIn('DefaultFloorShare = 25', header)
        self.assertIn('DefaultHardShare = 93', header)
        self.assertIn('TLG_GC_POLICY', (GPU_DIR / 'video_core' / 'texture_cache' /
                                        'texture_cache.cpp').read_text(encoding='utf-8'))


class BudgetPlumbingTest(unittest.TestCase):
    """The launcher's budget reaches the game (the probe copies ISS_* to TLG_*)."""

    def test_the_budget_is_sent(self):
        settings = dict(infamous_run.DEFAULT_SETTINGS, vram_budget_mb=2300)
        env = infamous_run.build_env(settings)
        self.assertEqual(env['ISS_GC_BUDGET_MB'], '2300')

    def test_the_curve_leaves_room_on_a_small_card(self):
        # A 4 GB card gives 4096 MB and the rest (the desktop, the driver and the buffer cache's
        # resident guest memory, ~600 MB measured) needs what is left: 3500 MB, the same a 6 GB
        # card gets. Lowering it only makes sense if the rest of the port goes down too.
        self.assertEqual(infamous_run.suggested_budget_mb(4096), 3500)
        self.assertEqual(infamous_run.suggested_budget_mb(6 * 1024), 3500)
        self.assertEqual(infamous_run.suggested_budget_mb(8 * 1024), 5000)
        # The budget never goes past 90% of the card.
        self.assertLessEqual(infamous_run.suggested_budget_mb(4096), 4096 * 0.9)


class PresetsTest(unittest.TestCase):
    """The two presets of the launcher."""

    def test_the_presets_exist_with_the_measured_values(self):
        balanced, detail = presets_vram.BALANCED_4GB, presets_vram.DETAIL
        self.assertEqual(balanced['id'], 'balanced_4gb')
        self.assertEqual(detail['id'], 'detail')
        # The measured working point of a 4 GB card: the card gives 4096 MB and the rest (the
        # desktop, the driver and the buffer cache's resident guest memory) needs ~600.
        self.assertEqual(balanced['settings']['vram_budget_mb'], 3500)
        self.assertEqual(detail['settings']['vram_budget_mb'], 3500)
        # The precise GPU reads: the GPU particles need them (the launcher's own note).
        for preset in (balanced, detail):
            self.assertEqual(preset['settings']['gpu_readbacks'], '2')
            # The game's own draw distance: a shorter one would drop the distant particles.
            self.assertEqual(preset['items']['menu:lod_scale'], '1')
            self.assertEqual(preset['items']['menu:far_clip'], '10000000')
        # Detail sharpens the textures; the balanced one keeps the game's own bias.
        self.assertEqual(detail['items']['menu:mip_lod_bias'], '-0.5')
        self.assertEqual(balanced['items']['menu:mip_lod_bias'], '0')

    def test_the_balanced_budget_is_the_launchers_own_rule(self):
        self.assertEqual(presets_vram.BALANCED_4GB['settings']['vram_budget_mb'],
                         infamous_run.suggested_budget_mb(4096))
    def test_the_presets_are_registered_in_the_launcher(self):
        from qt import gfxlogic
        ids = [preset_id for preset_id, _, _ in gfxlogic.PRESETS]
        self.assertIn('balanced_4gb', ids)
        self.assertIn('detail', ids)
        for preset in presets_vram.VRAM_PRESETS:
            self.assertIn(preset['id'], gfxlogic.FULL_PRESETS)
            # Every item the preset sets is a known one.
            for item_id in preset['items']:
                self.assertTrue(any(item.id == item_id for item in backports_table.ITEMS), item_id)
            # The values are inside the range of their item (a value equal to the item's default
            # is "off the list" and normalizes to None).
            for item_id, value in preset['items'].items():
                item = next(i for i in backports_table.ITEMS if i.id == item_id)
                normalized = backports_table.normalize(item, value)
                self.assertIn(normalized, (None, value), item_id)


if __name__ == '__main__':
    unittest.main()

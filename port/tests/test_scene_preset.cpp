// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the resolution preset of the scene (scene_resolution.h): the table of the divisors
// and the size the scene is rendered at. The preset is the resolution dial of the port
// (Native AA, Quality, Balanced, Performance, Ultra Performance) and it is the only place
// where the divisors live.
//
// No game and no Vulkan device: the preset is a table of numbers.
#include <cassert>
#include <cstdio>

#include "video_core/renderer_vulkan/scene_resolution.h"

using Vulkan::SceneResolution::ForPreset;
using Vulkan::SceneResolution::PresetScale;
using Vulkan::SceneResolution::Size;

namespace {

void TestScales() {
    // The divisors the menu labels promise: 1.0 native, 1.5 quality, 1.7 balanced,
    // 2.0 performance, 3.0 ultra performance.
    assert(PresetScale(0) == 1.0);
    assert(PresetScale(1) == 1.5);
    assert(PresetScale(2) == 1.7);
    assert(PresetScale(3) == 2.0);
    assert(PresetScale(4) == 3.0);
    // Out of range: the same as the nearest preset (the menu never sends them).
    assert(PresetScale(-1) == 1.0);
    assert(PresetScale(5) == 3.0);
}

void TestSizes() {
    const Size output{1920, 1080};
    // Native AA: the scene is rendered at the guest's own size.
    assert((ForPreset(0, output) == Size{1920, 1080}));
    // Quality: 1920/1.5 = 1280, 1080/1.5 = 720 (44 % of the pixels).
    assert((ForPreset(1, output) == Size{1280, 720}));
    // Balanced: 1920/1.7 = 1129.4 -> 1130, 1080/1.7 = 635.3 -> 636.
    assert((ForPreset(2, output) == Size{1130, 636}));
    // Performance: half the size.
    assert((ForPreset(3, output) == Size{960, 540}));
    // Ultra performance: a third.
    assert((ForPreset(4, output) == Size{640, 360}));
    // The sizes are even (half-resolution effects need it).
    for (int preset = 0; preset < 5; ++preset) {
        const auto size = ForPreset(preset, output);
        assert((size.width % 2) == 0);
        assert((size.height % 2) == 0);
    }
    // The default output is the guest's own size.
    assert((ForPreset(0) == Size{1920, 1080}));
    assert((ForPreset(4) == Size{640, 360}));
}

} // namespace

int main() {
    TestScales();
    TestSizes();
    std::printf("scene preset test: ok\n");
    return 0;
}

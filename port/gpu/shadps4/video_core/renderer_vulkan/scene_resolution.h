// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>

namespace Vulkan::SceneResolution {
struct Size {
    uint32_t width = 1920, height = 1080;
    bool operator==(const Size&) const = default;
};
/// The resolution divisor of a preset: the scene is rendered at output / divisor and the
/// game upsamples it. This is the one table of the preset scales (the copy in
/// tlg_settings.cpp, PresetScale, has no callers and should go: the renderer must not have
/// two of them).
inline constexpr double PresetScale(int preset) {
    constexpr double scales[5] = {1.0, 1.5, 1.7, 2.0, 3.0};
    return scales[preset < 0 ? 0 : (preset > 4 ? 4 : preset)];
}
inline Size ForPreset(int preset, Size output = {}) {
    // Even dimensions for half-resolution effects; match the preset labels exactly.
    const double scale = PresetScale(preset);
    return {uint32_t(std::max(1l, std::lround(output.width / scale / 2)) * 2),
            uint32_t(std::max(1l, std::lround(output.height / scale / 2)) * 2)};
}
constexpr uint32_t Pack(Size size) { return size.width | (size.height << 16); }
constexpr Size Unpack(uint32_t packed) {
    return packed ? Size{packed & 65535u, packed >> 16} : Size{};
}
// A proxy and the guest-size image hold the same logical surface. Native shader reads
// need a resolve after proxy writes; native writes invalidate the cached proxy.
struct Coherence {
    bool valid = false, dirty = false;
    void ProxyWrite() { valid = dirty = true; }
    void CopiedToProxy() { valid = true; dirty = false; }
    void Resolved() { dirty = false; }
    void NativeWrite() { valid = dirty = false; }
};
} // namespace Vulkan::SceneResolution

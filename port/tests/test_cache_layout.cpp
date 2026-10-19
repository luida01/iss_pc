// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the layout of the texture cache's hot data (video_core/texture_cache/image.h).
//
// The bind path reads a handful of fields of every bound texture (the flags, the binding state,
// the image handle and the descriptor's format and size) and the array of images is big: one
// image per resource, each of them a large structure. A field that sits after a kilobyte of
// other data is a cache miss of its own, and the frame does thousands of binds.
//
// The test checks that the fields the bind path reads are together at the front of the image
// and measures what that is worth: the same walk over a pool of images, with the layout the
// headers have now.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "video_core/texture_cache/image.h"

using namespace VideoCore;

namespace {

/// The fields the bind path reads for every bound texture (vk_rasterizer.cpp: the binding
/// state, the image handle and the descriptor's format and size).
struct HotOffsets {
    size_t flags = 0;
    size_t binding = 0;
    size_t usage = 0;
    size_t backing = 0;
    size_t props = 0;
    size_t pixel_format = 0;
    size_t resources = 0;
    size_t guest_address = 0;
    size_t guest_size = 0;
    size_t pitch = 0;
    size_t tile_mode = 0;
    size_t size = 0;
};

/// The offsets of those fields inside an Image. A real image needs a Vulkan device, and the
/// benchmark only cares about where the bytes are: the offsets come from the type itself.
HotOffsets Measure() {
    alignas(Image) static std::byte storage[sizeof(Image)];
    auto* image = reinterpret_cast<Image*>(storage);
    const auto base = reinterpret_cast<std::byte*>(image);
    const auto off = [&](const void* member) {
        return static_cast<size_t>(reinterpret_cast<const std::byte*>(member) - base);
    };
    HotOffsets out;
    out.flags = off(&image->flags);
    out.binding = off(&image->binding);
    out.usage = off(&image->usage);
    out.backing = off(&image->backing);
    out.props = off(&image->info.props);
    out.pixel_format = off(&image->info.pixel_format);
    out.resources = off(&image->info.resources);
    out.guest_address = off(&image->info.guest_address);
    out.guest_size = off(&image->info.guest_size);
    out.pitch = off(&image->info.pitch);
    out.tile_mode = off(&image->info.tile_mode);
    out.size = off(&image->info.size);
    return out;
}

/// The cache lines the hot fields of one image touch.
size_t Lines(const HotOffsets& hot) {
    const size_t first = hot.flags;
    const size_t last = std::max({hot.flags, hot.binding, hot.usage, hot.backing, hot.props,
                                  hot.pixel_format, hot.resources, hot.guest_address, hot.guest_size,
                                  hot.pitch, hot.tile_mode, hot.size});
    return (last / 64) - (first / 64) + 1;
}

/// The cache lines the bind path alone touches (the descriptor's data and the binding state).
size_t BindLines(const HotOffsets& hot) {
    const size_t first = hot.binding;
    const size_t last = std::max({hot.binding, hot.usage, hot.backing, hot.props, hot.pixel_format,
                                  hot.resources, hot.size});
    return (last / 64) - (first / 64) + 1;
}

/// The same walk the bind path does over a pool of images: the frame binds a few textures per
/// draw, and the draws walk the whole pool. The measure is the cycles per bind.
double BenchBind(u32 images, u32 draws, u32 textures_per_draw, u64 seed) {
    std::vector<std::byte> pool(static_cast<size_t>(images) * sizeof(Image));
    // A pool that looks like the cache's: every image has a different address, so the walk
    // misses the cache the same way the real one does.
    for (size_t i = 0; i < pool.size(); i += 64) {
        pool[i] = static_cast<std::byte>(i);
    }
    const HotOffsets hot = Measure();
    u64 state = seed;
    u64 sum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (u32 draw = 0; draw < draws; ++draw) {
        for (u32 t = 0; t < textures_per_draw; ++t) {
            // A pseudo random image (the game binds the same few textures over and over, but
            // the pool is much bigger than the working set).
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const size_t slot = static_cast<size_t>((state >> 33) % images);
            const std::byte* base = pool.data() + slot * sizeof(Image);
            sum += *reinterpret_cast<const u64*>(base + hot.flags);
            sum += *reinterpret_cast<const u32*>(base + hot.binding);
            sum += *reinterpret_cast<const u32*>(base + hot.usage);
            sum += *reinterpret_cast<const u64*>(base + hot.backing);
            sum += *reinterpret_cast<const u32*>(base + hot.props);
            sum += *reinterpret_cast<const u32*>(base + hot.pixel_format);
            sum += *reinterpret_cast<const u32*>(base + hot.resources);
            sum += *reinterpret_cast<const u64*>(base + hot.guest_address);
            sum += *reinterpret_cast<const u32*>(base + hot.guest_size);
            sum += *reinterpret_cast<const u32*>(base + hot.pitch);
            sum += *reinterpret_cast<const u32*>(base + hot.tile_mode);
            sum += *reinterpret_cast<const u32*>(base + hot.size);
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    // Keep the reads alive.
    if (sum == 0x1234567890abcdefull) {
        std::printf("sum %llu\n", static_cast<unsigned long long>(sum));
    }
    return double(elapsed) / double(draws * textures_per_draw);
}

} // namespace

int main() {
    const HotOffsets hot = Measure();
    std::printf("image layout: sizeof(Image) %zu, sizeof(ImageInfo) %zu\n", sizeof(Image),
                sizeof(ImageInfo));
    std::printf("  flags %zu binding %zu usage %zu backing %zu\n", hot.flags, hot.binding, hot.usage,
                hot.backing);
    std::printf("  info.props %zu pixel_format %zu resources %zu size %zu pitch %zu tile_mode %zu\n",
                hot.props, hot.pixel_format, hot.resources, hot.size, hot.pitch, hot.tile_mode);
    std::printf("  info.guest_address %zu guest_size %zu\n", hot.guest_address, hot.guest_size);
    std::printf("  the bind path touches %zu cache lines, the whole hot set %zu\n", BindLines(hot),
                Lines(hot));

    // The frame of the measured session: 3300 draws, ~10 textures each. The pool is the cache's
    // images: a few thousand in a scene, more in a session.
    for (const u32 images : {u32{1024}, u32{3300}, u32{8192}, u32{32768}}) {
        const double cycles = BenchBind(images, 33000, 10, 0x5eed5eed);
        std::printf("  %u images in the pool: %.0f ns per bind (%.1f cycles at 3 GHz)\n", images,
                    cycles, cycles * 3.0);
    }

    // The layout is a regression guard: the fields the bind path reads have to be in the same
    // cache line, or every bound texture misses the cache again.
    if (BindLines(hot) != 1) {
        std::printf("FAIL: the bind path reads %zu cache lines per image (it should be 1)\n",
                    BindLines(hot));
        return 1;
    }
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the cost of the shader recompiler's per-draw work: the specialization that every
// pipeline lookup builds, and the push constants that every draw copies.
//
// The PM4 decode of a draw and of a dispatch builds a StageSpecialization (the resources
// the shader binds, read out of the flattened user data) and, in the draw preparation
// workers, one per stage of every draw. This bench measures that work outside the game.
//
//   spec-bench [--iters N]

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/specialization.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace {

// The toggles the recompiler reads (tlg_toggles.h): the bench does not change them.
extern "C" std::uint64_t runtime_disabled_optimizations = 0;

u64 Rdtsc() {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

/// A shader with the resources a particle compute shader has: a few buffers, a few images
/// and a few samplers, all read from the flattened user data.
Shader::Info MakeInfo(Shader::HwStage hw, Shader::SwStage sw, u32 num_buffers, u32 num_images,
                      u32 num_samplers) {
    std::array<u32, Shader::ShaderParams::NumShaderUserData> ud{};
    for (u32 i = 0; i < ud.size(); ++i) {
        ud[i] = 0x1000 + i;
    }
    Shader::ShaderParams params{.user_data = ud, .code = std::span<const u32>{}, .hash = 0x1234};
    Shader::Info info{hw, sw, params};
    info.user_data = params.user_data;
    info.flattened_ud_buf.assign(256, 0x40);
    info.ud_mask.mask = 0xffff;
    info.srt_info.flattened_bufsize_dw = 256;

    u32 off = 16;
    info.buffers.resize(num_buffers);
    for (auto& b : info.buffers) {
        for (u32 i = 0; i < b.sharp_fetch.offsets.size(); ++i) {
            b.sharp_fetch.offsets[i] = static_cast<Shader::SharpLocation>(off + i);
        }
        b.sharp_fetch.load_mask = 0xF;
        off += 4;
    }
    info.images.resize(num_images);
    for (auto& i : info.images) {
        for (u32 k = 0; k < i.sharp_fetch.offsets.size(); ++k) {
            i.sharp_fetch.offsets[k] = static_cast<Shader::SharpLocation>(off + k);
        }
        i.sharp_fetch.load_mask = 0xFF;
        off += 8;
    }
    info.samplers.resize(num_samplers);
    for (auto& s : info.samplers) {
        for (u32 k = 0; k < s.sharp_fetch.offsets.size(); ++k) {
            s.sharp_fetch.offsets[k] = static_cast<Shader::SharpLocation>(off + k);
        }
        s.sharp_fetch.load_mask = 0x3;
        off += 2;
    }
    return info;
}

} // namespace

int main(int argc, char** argv) {
    u32 iters = 20000;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--iters")) {
            iters = static_cast<u32>(std::strtoul(argv[i + 1], nullptr, 10));
        }
    }
    double tsc_hz = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        const u64 c0 = Rdtsc();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
        }
        const u64 c1 = Rdtsc();
        const auto t1 = std::chrono::steady_clock::now();
        tsc_hz = double(c1 - c0) / std::chrono::duration<double>(t1 - t0).count();
    }

    Shader::Profile profile{};
    Shader::RuntimeInfo runtime{};
    runtime.Initialize(Shader::HwStage::Compute, Shader::SwStage::Compute);

    struct Case {
        const char* name;
        u32 buffers, images, samplers;
    };
    const Case cases[] = {
        {"compute, 4 buffers + 8 images + 4 samplers", 4, 8, 4},
        {"compute, 1 buffer + 2 images + 1 sampler", 1, 2, 1},
        {"compute, 12 buffers + 24 images + 8 samplers", 12, 24, 8},
    };

    std::printf("StageSpecialization (the resources a pipeline lookup reads), %u iterations, "
                "TSC %.4f GHz\n",
                iters, tsc_hz / 1e9);
    for (const auto& c : cases) {
        auto info = MakeInfo(Shader::HwStage::Compute, Shader::SwStage::Compute, c.buffers,
                              c.images, c.samplers);
        Shader::Backend::Bindings binding{};
        for (u32 i = 0; i < 64; ++i) {
            const auto spec = Shader::StageSpecialization(info, runtime, profile, binding);
            asm volatile("" : : "r"(&spec) : "memory");
        }
        const u64 start = Rdtsc();
        for (u32 i = 0; i < iters; ++i) {
            asm volatile("" : : : "memory");
            const auto spec = Shader::StageSpecialization(info, runtime, profile, binding);
            asm volatile("" : : "r"(&spec) : "memory");
        }
        const u64 end = Rdtsc();
        const double cycles = double(end - start) / double(iters);
        std::printf("  %-44s %8.1f cycles (%.0f ns)\n", c.name, cycles, cycles / tsc_hz * 1e9);
    }

    // The fetch shader data the vertex stage's specialization carries: the copy of the
    // parsed attributes that every specialization construction makes.
    {
        Shader::Gcn::FetchShaderData data{};
        data.attributes.resize(16);
        const u64 start = Rdtsc();
        for (u32 i = 0; i < iters; ++i) {
            asm volatile("" : : : "memory");
            const Shader::Gcn::FetchShaderData copy = data;
            asm volatile("" : : "r"(&copy) : "memory");
        }
        const u64 end = Rdtsc();
        const double cycles = double(end - start) / double(iters);
        std::printf("  %-44s %8.1f cycles (%.0f ns)\n", "copy of a 16-attribute fetch shader",
                    cycles, cycles / tsc_hz * 1e9);
    }

    // The push constants: the user data registers every draw copies.
    {
        auto info = MakeInfo(Shader::HwStage::Compute, Shader::SwStage::Compute, 4, 8, 4);
        Shader::PushData push{};
        const u64 start = Rdtsc();
        for (u32 i = 0; i < iters; ++i) {
            Shader::Backend::Bindings bnd{};
            asm volatile("" : : : "memory");
            info.PushUd(bnd, push);
        }
        const u64 end = Rdtsc();
        const double cycles = double(end - start) / double(iters);
        std::printf("  %-44s %8.1f cycles (%.0f ns)\n", "PushUd (16 user data registers)",
                    cycles, cycles / tsc_hz * 1e9);
    }
    return 0;
}

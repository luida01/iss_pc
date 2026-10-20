// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: the cost of the shader lookup the PM4 decode does for every stage of every draw
// and every dispatch (AmdGpu::TryGetParams -> FindBinaryInfo).
//
// FindBinaryInfo looks for the OrbShdr block that describes the shader code. When the
// code starts with the 0xBEEB03FF marker the block is found at once; otherwise the whole
// 64 KiB window is scanned, a word at a time. The port's own bench (srt-bench) measures
// the SRT walker; this one measures the scan, which is the same shape of work in the same
// place: the PM4 decode of a draw or a dispatch.
//
//   bininfo-bench [--iters N]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shader_recompiler/program_lookup.h"
#include "video_core/amdgpu/regs_shader.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace {

u64 Rdtsc() {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

/// The code of a shader as the guest has it in memory: the instructions, then the OrbShdr
/// block that describes them (the port looks for it right after the code).
std::vector<u32> MakeCode(bool with_marker, size_t code_dwords, size_t offset_dwords) {
    std::vector<u32> code(offset_dwords + code_dwords + 16, 0);
    if (with_marker) {
        code[offset_dwords] = 0xBEEB03FF;
        code[offset_dwords + 1] = static_cast<u32>(code_dwords);
    }
    // The OrbShdr block: the signature, then the length of the code it describes.
    auto* info = reinterpret_cast<AmdGpu::BinaryInfo*>(code.data() + offset_dwords + code_dwords);
    info->signature = AmdGpu::BinaryInfo::signature_ref;
    info->version = 1;
    info->length = static_cast<u32>(code_dwords * sizeof(u32));
    info->shader_hash = 0x1234567890abcdefull;
    return code;
}

} // namespace

int main(int argc, char** argv) {
    u32 iters = 2000;
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

    struct Case {
        const char* name;
        std::vector<u32> code;
        bool found;
    };
    std::vector<Case> cases;
    cases.push_back({"marker + block right after (fast path)", MakeCode(true, 64, 0), false});
    cases.push_back({"no marker, block right after (scan of 64 dwords)", MakeCode(false, 64, 0),
                     false});
    cases.push_back({"no marker, block 4 KiB later", MakeCode(false, 64, 1024), false});
    cases.push_back({"no marker, no block (the whole 64 KiB window)", MakeCode(false, 64, 0),
                     false});
    // The last one has no valid block at all: the scan runs to the end of the window.
    cases.back().code.assign(AmdGpu::BinaryInfoSearchLimit + 16, 0);

    std::printf("FindBinaryInfo (the shader lookup of the PM4 decode), %u iterations, "
                "TSC %.4f GHz\n",
                iters, tsc_hz / 1e9);
    for (auto& c : cases) {
        // Warm up.
        for (u32 i = 0; i < 32; ++i) {
            (void)AmdGpu::FindBinaryInfo(c.code.data());
        }
        const u64 start = Rdtsc();
        u32 found = 0;
        for (u32 i = 0; i < iters; ++i) {
            // The lookup is a pure function of the code: without this barrier the
            // compiler hoists it out of the loop and measures nothing.
            asm volatile("" : : : "memory");
            found += AmdGpu::FindBinaryInfo(c.code.data()) != nullptr ? 1 : 0;
        }
        const u64 end = Rdtsc();
        const double cycles = double(end - start) / double(iters);
        std::printf("  %-46s %8.1f cycles (%.0f ns) found %u\n", c.name, cycles,
                    cycles / tsc_hz * 1e9, found);
    }

    // The same lookups through the memo of the shader recompiler (program_lookup.h).
    std::printf("\nShader::ProgramLookup (the same lookups, memoized by code address)\n");
    for (auto& c : cases) {
        for (u32 i = 0; i < 32; ++i) {
            (void)Shader::ProgramLookup::Get(c.code.data());
        }
        const u64 start = Rdtsc();
        u32 found = 0;
        for (u32 i = 0; i < iters; ++i) {
            asm volatile("" : : : "memory");
            found += Shader::ProgramLookup::Get(c.code.data()).valid ? 1 : 0;
        }
        const u64 end = Rdtsc();
        const double cycles = double(end - start) / double(iters);
        std::printf("  %-46s %8.1f cycles (%.0f ns) found %u\n", c.name, cycles,
                    cycles / tsc_hz * 1e9, found);
    }
    return 0;
}

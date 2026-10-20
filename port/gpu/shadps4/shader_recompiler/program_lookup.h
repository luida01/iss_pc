// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include "common/types.h"
#include "shader_recompiler/params.h"
#include "video_core/amdgpu/regs_shader.h"

namespace Shader {

/// The parameters of a shader program in the guest's memory: its code and the hash the
/// game gave it.
///
/// Finding them means looking for the block that describes the code (the OrbShdr block)
/// in the guest's memory. The port scans for it word by word, for up to 64 KiB
/// (AmdGpu::FindBinaryInfo), and the PM4 decode pays that scan for every stage of every
/// draw and every dispatch; the draw preparation workers pay it again for the same
/// programs. The bench in port/tests/bininfo_bench.cpp measures the scan: 87 cycles when
/// the block is right after the code, 1.023 when it is 4 KiB away, and 15.262 when it is
/// not there at all (the worst case: the guest is still writing the shader).
///
/// The result is memoized by the address of the code and revalidated against the block
/// and against the first words of the code, the same way the fetch shader cache does
/// (frontend/fetch_shader.cpp). A guest that rewrites a shader at the same address
/// rewrites its block too, and the memo misses then.
struct ProgramParams {
    std::span<const u32> code;
    u64 hash{};
    bool valid{};
};

class ProgramLookup {
public:
    /// The number of words of the code the memo keeps to tell a rewritten shader from the
    /// same one. The block carries the game's own hash of the code, so this is only a
    /// second line of defence.
    static constexpr u32 PrefixWords = 16;

    struct Entry {
        u32 block_offset_dw = 0; ///< where the block is, counted from the code
        u32 code_dwords = 0;    ///< the length of the code, in words
        u64 hash = 0;           ///< the hash the game gave the shader
        std::vector<u32> prefix;
    };

    /// The parameters of the program whose code is at `code`, or an invalid result when
    /// the guest has not finished writing it (the port then skips the draw).
    [[nodiscard]] static ProgramParams Get(const u32* code) {
        if (!code) {
            return {};
        }
        auto& cache = Cache();
        if (const auto it = cache.find(code); it != cache.end() && Validate(code, it->second)) {
            ++hits();
            return {std::span<const u32>{code, it->second.code_dwords}, it->second.hash, true};
        }
        return Scan(code, cache);
    }

    /// The number of lookups that hit the memo (diagnostics).
    [[nodiscard]] static u64 Hits() {
        return hits();
    }
    [[nodiscard]] static u64 Misses() {
        return misses();
    }

private:
    [[nodiscard]] static bool Validate(const u32* code, const Entry& entry) {
        const auto* block = reinterpret_cast<const AmdGpu::BinaryInfo*>(code + entry.block_offset_dw);
        if (!block->Valid() || block->length != entry.code_dwords * sizeof(u32) ||
            block->shader_hash != entry.hash) {
            return false;
        }
        if (entry.prefix.size() > entry.code_dwords) {
            return false;
        }
        return std::memcmp(code, entry.prefix.data(), entry.prefix.size() * sizeof(u32)) == 0;
    }

    [[nodiscard]] static ProgramParams Scan(const u32* code,
                                            std::unordered_map<const u32*, Entry>& cache) {
        const auto* block = AmdGpu::FindBinaryInfo(code);
        if (!block) {
            cache.erase(code);
            ++misses();
            return {};
        }
        Entry entry;
        entry.block_offset_dw = static_cast<u32>(reinterpret_cast<const u32*>(block) - code);
        entry.code_dwords = block->length / sizeof(u32);
        entry.hash = block->shader_hash;
        entry.prefix.assign(code, code + std::min<u32>(PrefixWords, entry.code_dwords));
        if (cache.size() >= 4096) {
            cache.clear();
        }
        cache[code] = entry;
        ++misses();
        return {std::span<const u32>{code, entry.code_dwords}, entry.hash, true};
    }

    /// The memo is per thread, like the fetch shader cache: the workers, the GPU command
    /// thread and the compiler threads each have their own, and no lock is paid.
    [[nodiscard]] static std::unordered_map<const u32*, Entry>& Cache() {
        thread_local std::unordered_map<const u32*, Entry> cache;
        return cache;
    }
    [[nodiscard]] static u64& hits() {
        thread_local u64 count = 0;
        return count;
    }
    [[nodiscard]] static u64& misses() {
        thread_local u64 count = 0;
        return count;
    }
};

/// The parameters of the shader program of a stage register, or nothing when its code has
/// no valid block yet (the guest is still writing it, and the draw has to be skipped).
///
/// This is what the PM4 decode calls for every stage of every draw and every dispatch
/// (AmdGpu::TryGetParams does the same, with the scan this memo avoids).
template <typename Sh>
[[nodiscard]] std::optional<ShaderParams> LookupParams(const Sh& sh) {
    const auto* code = sh.template Address<const u32*>();
    const auto found = ProgramLookup::Get(code);
    if (!found.valid) {
        AmdGpu::LogMissingBinaryInfo(code);
        return std::nullopt;
    }
    return ShaderParams{
        .user_data = std::span<const u32, ShaderParams::NumShaderUserData>{sh.user_data},
        .code = found.code,
        .hash = found.hash,
    };
}

} // namespace Shader

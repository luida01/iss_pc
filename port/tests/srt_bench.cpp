// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: cost of the SRT walker (the "prepared data verification" of the PM4 decode).
//
// The SRT walker is the machine code that the shader recompiler generates to copy the
// resource table (the sharps) of a shader out of the guest's memory into the flattened
// user data. The GPU command thread runs it once per stage of every draw it decodes, to
// verify that the flattened data the draw preparation workers computed still matches what
// the guest's memory holds (docs/analysis/pm4-decode-cost.md).
//
// The shader cache of a real session (.meta files) holds the walker of every shader the
// game used, so this bench measures the real code with the real shapes, outside the game:
//
//   srt-bench <cache dir> [--limit N] [--iters N] [--hot]
//
// It reports, per shader: the number of guest memory reads the walker does (counted from
// its machine code), the number of entries it copies, and the cycles it takes.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <x86intrin.h>
#endif

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

// The walker's signature (Shader::PFN_SrtWalker). The recompiler declares it with the
// System V ABI on every platform, so the registers are rdi (user data) and rsi (flat dst).
#if defined(__x86_64__)
using PFN_SrtWalker = void(__attribute__((sysv_abi)) *)(const u32* user_data, u32* flat_dst);
#else
using PFN_SrtWalker = void (*)(const u32* user_data, u32* flat_dst);
#endif

constexpr u32 NumUserDataRegs = 16;

struct Meta {
    std::vector<u8> code;
    u32 flat_size_dw = 0;   ///< flattened_bufsize_dw: the user data registers plus the entries
    u32 entries = 0;        ///< the entries the walker copies (flat_size_dw - the user data)
    u32 reads = 0;          ///< guest memory reads the walker does (4 or 8 bytes each)
    u32 pointer_reads = 0;  ///< of those, the ones that chase a pointer
    u32 sgpr_base = 0;      ///< the user data register the SRT pointer comes from
    u32 stores = 0;         ///< writes to the flattened buffer
    bool has_walker = false;
};

/// Executable memory for the walker (the recompiler's own code generator lives in the
/// library; here the code comes from the cache file).
class CodeBuffer {
public:
    explicit CodeBuffer(const std::vector<u8>& code) {
#if defined(_WIN32)
        ptr_ = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (ptr_) {
            std::memcpy(ptr_, code.data(), code.size());
        }
#else
        ptr_ = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr_ != MAP_FAILED) {
            std::memcpy(ptr_, code.data(), code.size());
        } else {
            ptr_ = nullptr;
        }
#endif
    }
    ~CodeBuffer() {
#if defined(_WIN32)
        if (ptr_) {
            VirtualFree(ptr_, 0, MEM_RELEASE);
        }
#else
        if (ptr_) {
            munmap(ptr_, code.size());
        }
#endif
    }
    CodeBuffer(const CodeBuffer&) = delete;
    CodeBuffer& operator=(const CodeBuffer&) = delete;
    [[nodiscard]] auto* data() const { return ptr_; }
    [[nodiscard]] auto fn() const { return reinterpret_cast<PFN_SrtWalker>(ptr_); }

private:
    void* ptr_ = nullptr;
};

/// Reads the tail of a .meta file: the SRT block and the walker code.
///
/// The file is written by Vulkan::RegisterShaderMeta: the specification, then the
/// InfoPersistent block, the flattened user data, the PersistentSrtInfo block and, at the
/// very end, the walker's machine code. The size of the code is inside the SRT block, so
/// the tail can be found without parsing the specification.
bool ParseMeta(const std::vector<u8>& file, Meta& out) {
    const size_t size = file.size();
    // PersistentSrtInfo: u64 walker_func, u64 walker_func_size, u32 flattened_bufsize_dw, pad.
    constexpr size_t SrtBlockSize = 24;
    if (size < SrtBlockSize) {
        return false;
    }
    // The block is written before the walker code, and the file can end with a few
    // padding bytes, so the block is at size - padding - SrtBlockSize - walker_size.
    // The walker always ends with "pop rdi; ret", which tells the two apart.
    for (size_t p = size - SrtBlockSize + 1; p-- > 0;) {
        u64 walker_size = 0;
        std::memcpy(&walker_size, file.data() + p + 8, sizeof(walker_size));
        if (walker_size > (1u << 20)) {
            continue;
        }
        const size_t end = p + SrtBlockSize + walker_size;
        if (end > size || size - end > 8) {
            continue;
        }
        u32 flat_size = 0;
        std::memcpy(&flat_size, file.data() + p + 16, sizeof(flat_size));
        if (flat_size < NumUserDataRegs || flat_size > 4096) {
            continue;
        }
        if (walker_size >= 2 && (file[end - 2] != 0x5f || file[end - 1] != 0xc3)) {
            continue; // not the end of a walker
        }
        out.flat_size_dw = flat_size;
        out.entries = flat_size > NumUserDataRegs ? flat_size - NumUserDataRegs : 0;
        out.has_walker = walker_size != 0;
        if (walker_size) {
            out.code.assign(file.begin() + p + SrtBlockSize, file.begin() + end);
        }
        return true;
    }
    return false;
}

/// Counts the guest memory reads of the walker from its machine code.
///
/// The recompiler generates a very regular shape (ir/passes/flatten_extended_userdata_pass.cpp):
///   push rdi ; mov rdi, [rdi + off]   (the SRT pointer: a user data read or a nested one)
///   and rdi, 0xffffffffffff
///   mov r10d, [rdi + off] ; mov [rsi + off], r10d   (one entry)
/// and the same again for every nested level.
void CountReads(const std::vector<u8>& code, Meta& out) {
    for (size_t i = 0; i + 3 < code.size(); ++i) {
        const u8 modrm = code[i + 2];
        const u8 mod = (modrm >> 6) & 3;
        const u8 reg = (modrm >> 3) & 7;
        const u8 rm = modrm & 7;
        if (code[i] == 0x44 && code[i + 1] == 0x8b && reg == 2 && rm == 7) {
            // mov r10d, [rdi + off]: a 4 byte read of a sharp.
            ++out.reads;
            ++out.stores;
        }
        if (code[i] == 0x48 && code[i + 1] == 0x8b && reg == 7 && rm == 7) {
            // mov rdi, [rdi + off]: a pointer read (the SRT pointer, or a nested one).
            ++out.reads;
            ++out.pointer_reads;
            // The first one reads the user data: the displacement is the register.
            if (out.pointer_reads == 1) {
                if (mod == 1) {
                    out.sgpr_base = static_cast<u32>(static_cast<int8_t>(code[i + 3])) / 4;
                } else if (mod == 2 && i + 7 < code.size()) {
                    std::int32_t disp = 0;
                    std::memcpy(&disp, code.data() + i + 3, sizeof(disp));
                    out.sgpr_base = static_cast<u32>(disp) / 4;
                }
            }
        }
    }
}

/// The guest's memory the walker reads. The real SRT is a small structure in the guest's
/// heap: the sharps of the resources are in it and the nested tables are in other small
/// buffers. Two models: "hot" (everything in one page, the best case) and "spread" (the
/// tables in a 4 MiB arena, the pessimistic case).
class GuestMemory {
public:
    GuestMemory(size_t size, bool hot) : size_{size} {
        auto* bytes = static_cast<u8*>(std::calloc(size, 1));
        data_ = bytes;
        if (!hot) {
            // Every 8 byte slot points to another 64 byte slot of the same arena.
            const size_t slots = size / 64;
            for (size_t i = 0; i < slots; ++i) {
                const u64 target = reinterpret_cast<u64>(data_) + 64 * ((i * 7 + 1) % slots);
                std::memcpy(data_ + i * 8, &target, sizeof(target));
            }
        } else {
            // Every slot points to the base: the whole walk stays in the first cache lines.
            for (size_t i = 0; i < size / 8; ++i) {
                const u64 target = reinterpret_cast<u64>(data_);
                std::memcpy(data_ + i * 8, &target, sizeof(target));
            }
        }
    }
    ~GuestMemory() { std::free(data_); }
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;
    [[nodiscard]] auto* data() const { return data_; }
    [[nodiscard]] size_t size() const { return size_; }

private:
    u8* data_ = nullptr;
    size_t size_ = 0;
};

u64 Rdtsc() {
#if defined(__x86_64__) || defined(_M_X64)
    return __builtin_ia32_rdtsc();
#else
    return 0;
#endif
}

struct Stats {
    u64 n = 0;
    double sum = 0;
    double min = 1e30;
    double max = 0;
    void Add(double v) {
        ++n;
        sum += v;
        min = std::min(min, v);
        max = std::max(max, v);
    }
    [[nodiscard]] double mean() const { return n ? sum / double(n) : 0; }
};

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <cache dir> [--limit N] [--iters N] [--spread]\n", argv[0]);
        return 2;
    }
    const std::filesystem::path cache{argv[1]};
    size_t limit = SIZE_MAX;
    u32 iters = 2000;
    bool hot = true;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) {
            limit = std::strtoull(argv[++i], nullptr, 10);
        } else if (!std::strcmp(argv[i], "--iters") && i + 1 < argc) {
            iters = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (!std::strcmp(argv[i], "--spread")) {
            hot = false;
        }
    }

    std::vector<std::filesystem::path> metas;
    for (const auto& entry : std::filesystem::directory_iterator(cache)) {
        if (entry.path().extension() == ".meta") {
            metas.push_back(entry.path());
        }
    }
    std::sort(metas.begin(), metas.end());
    if (metas.size() > limit) {
        metas.resize(limit);
    }
    if (metas.empty()) {
        std::fprintf(stderr, "no .meta files in %s\n", cache.string().c_str());
        return 1;
    }

    // The TSC rate, to turn the cycles into nanoseconds.
    double tsc_hz = 0;
    {
        const auto t0 = std::chrono::steady_clock::now();
        const u64 c0 = Rdtsc();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(200)) {
            // busy
        }
        const u64 c1 = Rdtsc();
        const auto t1 = std::chrono::steady_clock::now();
        tsc_hz = double(c1 - c0) / std::chrono::duration<double>(t1 - t0).count();
    }

    GuestMemory guest{static_cast<size_t>(hot ? 4096 : 4 * 1024 * 1024), hot};
    std::vector<u32> user_data(32, 0);
    std::vector<u32> flat(256, 0);

    // Calibration: the cost of the call itself (an empty function with the same
    // signature), so the numbers below can be read as the walk's own cost.
    {
        const auto empty = +[](const u32*, u32*) __attribute__((sysv_abi)) {};
        const u64 start = Rdtsc();
        for (u32 i = 0; i < 100000; ++i) {
            empty(user_data.data(), flat.data());
        }
        const u64 end = Rdtsc();
        std::printf("call overhead: %.2f cycles\n", double(end - start) / 100000.0);
    }

    Stats per_call;
    Stats per_entry;
    u64 with_walker = 0;
    u64 total_reads = 0;
    u64 total_entries = 0;
    u64 total_bytes = 0;
    std::vector<u32> entry_hist(512, 0);
    u64 no_walker = 0;
    u64 parse_failed = 0;
    u64 walkers_that_wrote = 0;
    u32 sgpr_hist[32]{};

    for (const auto& path : metas) {
        std::ifstream in(path, std::ios::binary);
        const std::vector<u8> file{std::istreambuf_iterator<char>(in), {}};
        Meta meta;
        if (!ParseMeta(file, meta)) {
            if (std::getenv("SRT_DEBUG")) {
                std::fprintf(stderr, "%s: size %zu, last 24:", path.filename().string().c_str(),
                             file.size());
                for (size_t k = file.size() > 24 ? file.size() - 24 : 0; k < file.size(); ++k) {
                    std::fprintf(stderr, " %02x", file[k]);
                }
                std::fprintf(stderr, "\n");
            }
            ++parse_failed;
            continue;
        }
        if (!meta.has_walker) {
            ++no_walker;
            continue;
        }
        ++with_walker;
        CountReads(meta.code, meta);
        total_reads += meta.reads;
        total_entries += meta.entries;
        total_bytes += meta.code.size();
        if (meta.entries < entry_hist.size()) {
            ++entry_hist[meta.entries];
        }
        if (meta.sgpr_base < 32) {
            ++sgpr_hist[meta.sgpr_base];
        }

        CodeBuffer code{meta.code};
        if (!code.data()) {
            continue;
        }
        // The SRT pointer comes from the user data: every register pair points at the
        // guest's memory, so the walk works whichever register the shader uses.
        const u64 base = reinterpret_cast<u64>(guest.data());
        for (u32 i = 0; i + 1 < user_data.size(); i += 2) {
            user_data[i] = static_cast<u32>(base);
            user_data[i + 1] = static_cast<u32>(base >> 32);
        }
        const auto fn = code.fn();
        // Warm up (the code is in the i-cache, the data in the d-cache).
        for (u32 i = 0; i < 64; ++i) {
            fn(user_data.data(), flat.data());
        }
        // Sanity: the walker must have written the entries of the flattened buffer.
        bool wrote = false;
        for (u32 i = NumUserDataRegs; i < meta.flat_size_dw && i < flat.size(); ++i) {
            wrote |= flat[i] != 0;
        }
        if (wrote) {
            ++walkers_that_wrote;
        }
        const u64 start = Rdtsc();
        for (u32 i = 0; i < iters; ++i) {
            fn(user_data.data(), flat.data());
        }
        const u64 end = Rdtsc();
        const double cycles = double(end - start) / double(iters);
        per_call.Add(cycles);
        if (meta.entries) {
            per_entry.Add(cycles / double(meta.entries));
        }
    }

    std::printf("SRT walker bench (%s model, %u iterations each, TSC %.4f GHz)\n",
                hot ? "hot" : "spread", iters, tsc_hz / 1e9);
    std::printf("  shaders with a walker: %llu (without: %llu, unreadable: %llu)\n",
                static_cast<unsigned long long>(with_walker),
                static_cast<unsigned long long>(no_walker),
                static_cast<unsigned long long>(parse_failed));
    std::printf("  walker code: %llu bytes in total, %.0f bytes per shader\n",
                static_cast<unsigned long long>(total_bytes),
                with_walker ? double(total_bytes) / double(with_walker) : 0.0);
    std::printf("  entries per walk: %llu in total, %.1f per shader\n",
                static_cast<unsigned long long>(total_entries),
                with_walker ? double(total_entries) / double(with_walker) : 0.0);
    std::printf("  guest reads per walk: %.1f (%.1f of them pointer chases)\n",
                with_walker ? double(total_reads) / double(with_walker) : 0.0,
                with_walker ? double(total_reads - total_entries) / double(with_walker) : 0.0);
    std::printf("  walkers that wrote the flattened buffer: %llu of %llu\n",
                static_cast<unsigned long long>(walkers_that_wrote),
                static_cast<unsigned long long>(with_walker));
    std::printf("  cycles per walk: mean %.1f, min %.1f, max %.1f\n", per_call.mean(),
                per_call.n ? per_call.min : 0.0, per_call.max);
    std::printf("  cycles per entry: mean %.2f\n", per_entry.mean());
    std::printf("  nanoseconds per walk: mean %.1f\n", per_call.mean() / tsc_hz * 1e9);
    std::printf("  entries histogram:");
    for (size_t i = 0; i < entry_hist.size(); ++i) {
        if (entry_hist[i]) {
            std::printf(" %zu:%u", i, entry_hist[i]);
        }
    }
    std::printf("\n  SRT register (user data) histogram:");
    for (u32 i = 0; i < 32; ++i) {
        if (sgpr_hist[i]) {
            std::printf(" %u:%u", i, sgpr_hist[i]);
        }
    }
    std::printf("\n");
    return 0;
}

// SPDX-License-Identifier: GPL-2.0-or-later
// tlg: test of the two pieces the PM4 decode of a draw and a dispatch use from the
// shader recompiler: the SRT walker (the verification of the prepared data) and the
// memoized lookup of a shader program (the scan of the guest's memory).
//
//   test-pm4-decode
//
// It needs no game and no Vulkan device: the walker is a few bytes of machine code
// built here, and the program lookup works on a buffer in memory.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shader_recompiler/info.h"
#include "shader_recompiler/params.h"
#include "shader_recompiler/program_lookup.h"
#include "shader_recompiler/srt_profile.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

#if defined(__x86_64__) || defined(_M_X64)
using PFN_SrtWalker = void(__attribute__((sysv_abi)) *)(const u32* user_data, u32* flat_dst);
#else
using PFN_SrtWalker = void (*)(const u32* user_data, u32* flat_dst);
#endif

/// A walker like the recompiler generates: it takes the address of the resource table
/// from the user data (the register 12, as every shader of this game does) and copies
/// `num_entries` words of it into the flattened user data, starting at word 16.
std::vector<u8> MakeWalkerCode(u32 num_entries) {
    std::vector<u8> code;
    // mov rdi, [rdi + 0x30] (the SRT pointer of the user data register 12)
    code.insert(code.end(), {0x48, 0x8b, 0x7f, 0x30});
    // mov r10, 0xffffffffffff ; and rdi, r10 (the guest's addresses are 48 bits)
    code.insert(code.end(), {0x49, 0xba, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00});
    code.insert(code.end(), {0x4c, 0x21, 0xd7});
    for (u32 i = 0; i < num_entries; ++i) {
        code.insert(code.end(), {0x44, 0x8b, 0x57, static_cast<u8>(i * 4)});  // mov r10d, [rdi+off]
        code.insert(code.end(),
                    {0x44, 0x89, 0x56, static_cast<u8>(0x40 + i * 4)});  // mov [rsi+off], r10d
    }
    code.push_back(0xc3);  // ret
    return code;
}

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
    [[nodiscard]] auto fn() const { return reinterpret_cast<PFN_SrtWalker>(ptr_); }

private:
    void* ptr_ = nullptr;
};

/// The code of a shader with the block that describes it, like the guest has it.
std::vector<u32> MakeProgram(bool with_marker, u32 code_dwords, u32 block_offset_dwords) {
    std::vector<u32> code(block_offset_dwords + code_dwords + 16, 0);
    if (with_marker) {
        code[block_offset_dwords] = 0xBEEB03FF;
        code[block_offset_dwords + 1] = code_dwords;
    }
    auto* block = reinterpret_cast<AmdGpu::BinaryInfo*>(code.data() + block_offset_dwords + code_dwords);
    block->signature = AmdGpu::BinaryInfo::signature_ref;
    block->version = 1;
    block->length = code_dwords * sizeof(u32);
    block->shader_hash = 0xfeedfacecafebeefull;
    return code;
}

} // namespace

int main() {
    // The profile of the walker is off unless the environment says so: the test turns it
    // on explicitly.
    Shader::SrtProfile::SetEnabled(true);

    // 1. The SRT walker: the walk copies the resource table into the flattened user data,
    // and the profile counts it.
    {
        constexpr u32 kEntries = 8;
        CodeBuffer code{MakeWalkerCode(kEntries)};
        Check(code.fn() != nullptr, "the walker code is executable");

        std::array<u32, Shader::ShaderParams::NumShaderUserData> ud{};
        std::vector<u32> guest(64, 0x11223344);
        for (u32 i = 0; i + 1 < ud.size(); i += 2) {
            const u64 base = reinterpret_cast<u64>(guest.data());
            ud[i] = static_cast<u32>(base);
            ud[i + 1] = static_cast<u32>(base >> 32);
        }
        // The SRT pointer of the register 12 (the walker reads it from there).
        const u64 base = reinterpret_cast<u64>(guest.data());
        ud[12] = static_cast<u32>(base);
        ud[13] = static_cast<u32>(base >> 32);
        Shader::ShaderParams params{.user_data = ud, .code = std::span<const u32>{}, .hash = 1};
        Shader::Info info{Shader::HwStage::Compute, Shader::SwStage::Compute, params};
        info.user_data = params.user_data;
        info.srt_info.walker_func = code.fn();
        info.srt_info.flattened_bufsize_dw = Shader::NUM_USER_DATA_REGS + kEntries;

        const auto walks = Shader::SrtProfile::Get().walks.load();
        const auto entries = Shader::SrtProfile::Get().entries.load();
        info.RefreshFlatBuf();
        Check(info.flattened_ud_buf.size() == Shader::NUM_USER_DATA_REGS + kEntries,
              "the flattened buffer has room for the entries");
        Check(info.flattened_ud_buf[Shader::NUM_USER_DATA_REGS] == guest[0],
              "the walker copied the first entry");
        Check(info.flattened_ud_buf[Shader::NUM_USER_DATA_REGS + kEntries - 1] ==
                  guest[kEntries - 1],
              "the walker copied the last entry");
        Check(Shader::SrtProfile::Get().walks.load() == walks + 1, "the profile counted the walk");
        Check(Shader::SrtProfile::Get().entries.load() == entries + kEntries,
              "the profile counted the entries");
    }

    // 2. The program lookup: the same program is found again without scanning the
    // guest's memory, and a program the guest rewrote is looked up again.
    {
        auto program = MakeProgram(false, 64, 0);
        const auto first = Shader::ProgramLookup::Get(program.data());
        Check(first.valid, "the program is found");
        Check(first.hash == 0xfeedfacecafebeefull, "the hash is the one of the block");
        Check(first.code.size() == 64, "the code is the length of the block");

        const auto second = Shader::ProgramLookup::Get(program.data());
        Check(second.valid && second.hash == first.hash, "the memo returns the same program");

        // The guest rewrites the shader at the same address: the block changes too.
        program[0] = 0x12345678;
        auto* block = reinterpret_cast<AmdGpu::BinaryInfo*>(program.data() + 64);
        block->shader_hash = 0x1111222233334444ull;
        const auto third = Shader::ProgramLookup::Get(program.data());
        Check(third.valid && third.hash == 0x1111222233334444ull,
              "a rewritten program is looked up again");
    }

    // 3. The program lookup of a program whose block is not there yet: the guest is
    // still writing it, and the draw is skipped.
    {
        std::vector<u32> program(AmdGpu::BinaryInfoSearchLimit + 16, 0);
        const auto found = Shader::ProgramLookup::Get(program.data());
        Check(!found.valid, "a program without a block is not valid");
    }

    if (failures) {
        std::printf("%d checks failed\n", failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}

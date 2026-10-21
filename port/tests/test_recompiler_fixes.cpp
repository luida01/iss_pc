// SPDX-License-Identifier: GPL-2.0-or-later
// Recompiler fixes ported from the SotC native port (recompiler_toggles.h): LDS barriers for
// partial-wave and multi-wave compute workgroups, and S_WQM_B64. No window, no game.
// The switches are read once per process, so run it with their other values too:
//   out/gpu/recompiler-fixes-test [dir]
//   TLG_LDS_SMALL=0 TLG_LDS_MULTI=0 TLG_WQM=0 out/gpu/recompiler-fixes-test
// With [dir], the multi-wave compute shader is written there as SPIR-V (for spirv-val).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include "common/backports.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler_toggles.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs_shader.h"

using namespace Shader;

static int failures = 0;

// The environment switches of the recompiler are read once per process. The MSYS2/MinGW build
// has no POSIX setenv; _putenv_s is its equivalent (an empty value removes the variable).
static void SetEnv(const char* name, const char* value, bool overwrite) {
#ifdef _WIN32
    if (!overwrite && std::getenv(name)) {
        return;
    }
    _putenv_s(name, value);
#else
    setenv(name, value, overwrite ? 1 : 0);
#endif
}

static void UnsetEnv(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

static void Check(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    failures += ok ? 0 : 1;
}

static u32 Count(const IR::Block& block, IR::Opcode opcode) {
    u32 count = 0;
    for (const IR::Inst& inst : block.Instructions()) {
        count += inst.GetOpcode() == opcode ? 1 : 0;
    }
    return count;
}

struct BarrierResult {
    u32 workgroup;
    u32 subgroup;
};

// One block: an LDS write, then a read of it (another lane's slot on the guest), then a write.
static BarrierResult RunBarrierPass(u32 threads, const char* spirv_path = nullptr,
                                    u32 host_subgroup = 32) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 256;
    runtime.hw.cs.workgroup_size = {threads, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    ir.WriteShared(32, ir.Imm32(1u), ir.Imm32(0u));
    const IR::U32 value{ir.LoadShared(32, false, ir.Imm32(4u))};
    ir.WriteShared(32, value, ir.Imm32(8u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.needs_lds_barriers = true;
    profile.subgroup_size = host_subgroup;
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Optimization::SharedMemoryBarrierPass(program, runtime, profile);
    const BarrierResult result{Count(block, IR::Opcode::Barrier),
                               Count(block, IR::Opcode::SubgroupBarrier)};
    if (spirv_path) {
        Optimization::CollectShaderInfoPass(program, profile); // declares the LDS types
        Backend::Bindings bindings{};
        const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        std::ofstream out(spirv_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        Check(bool(out) && !code.empty(), "multi-wave compute shader emits SPIR-V");
    }
    return result;
}

// The append pattern of a compute shader: the wave reserves a slot with an atomic in its first
// lane and broadcasts the base with a ReadFirstLane, so every lane writes at base + its index.
// The guest wave64 has 64 lanes; on a 32-wide host it is two host subgroups of 32, and the host's
// broadcast (OpGroupNonUniformBroadcastFirst) is per subgroup: the upper half of the wave read
// its own lane 32, which reserved nothing, and wrote its items over the lower half's.
struct AppendPattern {
    static constexpr u32 kWaveLanes = 64;
    static constexpr u32 kHostLanes = 32;

    // What a lane holds before the broadcast: the lane that reserved the slot holds the base,
    // the others hold 0 (they never reserved anything).
    static u32 Reserved(u32 lane, u32 base) {
        return lane == 0 ? base : 0;
    }

    // The base the lane sees after the ReadFirstLane, for the three forms of the shader.
    enum class Form {
        Reference,  // the PS4 wave64: the first active lane of the wave
        HostPerHalf,// the host's per subgroup broadcast: the upper half reads its own lane 32
        Exchange,   // the LDS exchange of the wave64 lowering: the whole wave reads lane 0
    };

    static u32 Base(Form form, u32 lane, u32 base) {
        switch (form) {
        case Form::Reference:
        case Form::Exchange:
            return base;
        case Form::HostPerHalf:
            // The host broadcast is per subgroup: the lower half reads lane 0's value (the
            // base) and the upper half reads lane 32's value (nothing: it reserved no slot).
            return Reserved(lane < kHostLanes ? 0 : kHostLanes, base);
        }
        return 0;
    }

    // The slot the lane writes, and the slots the wave touches as a set.
    static u32 Slot(Form form, u32 lane, u32 base) {
        return Base(form, lane, base) + lane;
    }
    static u32 Slots(Form form, u32 base) {
        std::set<u32> slots;
        for (u32 lane = 0; lane < kWaveLanes; ++lane) {
            slots.insert(Slot(form, lane, base));
        }
        return static_cast<u32>(slots.size());
    }
    // The slots of the wave's own list (the ones the wave reserved, base..base+63).
    static bool WritesInsideTheList(Form form, u32 base) {
        for (u32 lane = 0; lane < kWaveLanes; ++lane) {
            const u32 slot = Slot(form, lane, base);
            if (slot < base || slot >= base + kWaveLanes) {
                return false;
            }
        }
        return true;
    }
};

// The shader the wave64 lowering produces for the append pattern, and what it computes.
struct ReadFirstLaneForm {
    u32 broadcasts{};      // ReadFirstLane instructions in the shader (the host broadcast)
    u32 barriers{};        // workgroup barriers the lowering added
    u32 exchanges{};       // LDS loads of the exchange
    u32 writes{};          // LDS writes (the test's own one plus the exchange's)
    bool even_half_slot{}; // the load reads the even half's slot (bit 2 of the address cleared)
};

// Builds the append pattern in a 64-thread compute shader (in divergent flow when asked) and
// runs the wave64 lowering on it.
static ReadFirstLaneForm RunReadFirstLaneForm(bool varying, bool divergent, u32 threads = 64,
                                                const char* spirv_path = nullptr) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 256;
    runtime.hw.cs.workgroup_size = {threads, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block hoist(pool);
    IR::Block body(pool);
    IR::Block merge(pool);
    // The block that branches to the divergent region: the condition (a branch on the lane
    // id) and, when it is known there, the value the ReadFirstLane broadcasts.
    IR::IREmitter hoist_ir(hoist);
    const IR::U32 tid = hoist_ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U1 cond{
        hoist_ir.INotEqual(hoist_ir.BitwiseAnd(tid, hoist_ir.Imm32(1u)), hoist_ir.Imm32(0u))};
    // The divergent region.
    IR::IREmitter ir(body);
    // The atomic's result: only the lane that reserves the slot holds it.
    const IR::U32 reserved = IR::U32{ir.SharedAtomicIAdd(ir.Imm32(0u), ir.Imm32(1u), false)};
    const IR::U32 source =
        varying ? reserved : IR::U32{ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0)};
    const IR::U32 base = ir.ReadFirstLane(source);
    ir.WriteShared(32, base, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&hoist);
    program.blocks.push_back(&body);
    program.post_order_blocks.push_back(&hoist);
    program.post_order_blocks.push_back(&body);
    if (divergent) {
        // A branch on the lane id: the ReadFirstLane inside it is in divergent flow.
        program.blocks.push_back(&merge);
        program.post_order_blocks.push_back(&merge);
        program.syntax_list.push_back({.data = {.block = &hoist},
                                       .type = IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back(
            {.data = {.if_node = {.cond = cond, .body = &body, .merge = &merge}},
             .type = IR::AbstractSyntaxNode::Type::If});
        program.syntax_list.push_back({.data = {.block = &body},
                                       .type = IR::AbstractSyntaxNode::Type::Block});
        program.syntax_list.push_back({.data = {.end_if = {.merge = &merge}},
                                       .type = IR::AbstractSyntaxNode::Type::EndIf});
        program.syntax_list.push_back({.data = {.block = &merge},
                                       .type = IR::AbstractSyntaxNode::Type::Block});
    } else {
        program.syntax_list.push_back({.data = {.block = &body},
                                       .type = IR::AbstractSyntaxNode::Type::Block});
    }
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = 32;
    profile.supported_spirv = 0x00010600; // the group operations need SPIR-V 1.3
    profile.support_int64 = true;
    Optimization::LowerWave64BallotPass(program, runtime, profile);

    ReadFirstLaneForm form{};
    form.broadcasts = Count(body, IR::Opcode::ReadFirstLane);
    form.barriers = Count(body, IR::Opcode::Barrier);
    form.writes = Count(body, IR::Opcode::WriteSharedU32);
    for (const IR::Inst& inst : body.Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::LoadSharedU32) {
            continue;
        }
        ++form.exchanges;
        // The exchange reads the even half's slot: the slot address with bit 2 cleared, so both
        // host halves of the wave read the same slot (the wave's first lane).
        const IR::Value address = inst.Arg(0);
        const IR::Inst* op = address.IsImmediate() ? nullptr : address.Inst();
        form.even_half_slot = form.even_half_slot ||
                              (op && op->GetOpcode() == IR::Opcode::BitwiseAnd32 &&
                               op->Arg(1).IsImmediate() && op->Arg(1).U32() == ~7u);
    }
    if (spirv_path) {
        // The SPIR-V of the lowered shader (for spirv-val).
        Optimization::CollectShaderInfoPass(program, profile);
        Backend::Bindings bindings{};
        const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        std::ofstream out(spirv_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        Check(bool(out) && !code.empty(), "the lowered wave64 shader emits SPIR-V");
    }
    return form;
}

// tlg: the ReadFirstLane in divergent flow, whose exchange is hoisted to the block that
// branches to the divergent region (uniform flow, where a workgroup barrier is allowed).
//
// The value the branch broadcasts has to be known before the branch: when it is not (a loop
// carried value, an atomic inside the region), the instruction stays a host broadcast and
// the upper half of the wave reads its own lane 32.
struct DivergentForm {
    u32 broadcasts{}; // the ReadFirstLane instructions left in the shader
    u32 barriers{};   // the barriers of the exchange
    u32 in_hoist{};   // the barriers in the block that branches to the divergent region
    u32 loads{};      // the LDS loads of the exchange
    u32 writes{};     // the LDS writes of the exchange
    u32 shuffles{};   // the shuffles (the value of the first lane of the subgroup)
    u32 ballots{};    // the ballots (the lanes of the subgroup with the exec mask set)
};

static DivergentForm RunDivergentReadFirstLane(bool value_before_branch,
                                                const char* spirv_path = nullptr,
                                                u32 threads = 64, u32 size_y = 1) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 256;
    runtime.hw.cs.workgroup_size = {threads, size_y, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block hoist(pool);
    IR::Block body(pool);
    IR::Block merge(pool);
    // The block that branches to the divergent region: the condition and, when it is known
    // before the branch, the value the ReadFirstLane broadcasts.
    IR::IREmitter hoist_ir(hoist);
    const IR::U32 tid = hoist_ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U1 cond{
        hoist_ir.INotEqual(hoist_ir.BitwiseAnd(tid, hoist_ir.Imm32(1u)), hoist_ir.Imm32(0u))};
    // The value the branch broadcasts has to be known before the branch, and it has to be
    // able to differ between the lanes (a uniform value needs no exchange).
    const IR::U32 source_before = IR::U32{tid};
    // The divergent region.
    IR::IREmitter ir(body);
    // The value the branch broadcasts: known before the branch, or only inside the region.
    const IR::U32 source =
        value_before_branch ? source_before
                            : IR::U32{ir.SharedAtomicIAdd(ir.Imm32(0u), ir.Imm32(1u), false)};
    const IR::U32 base = IR::U32{ir.ReadFirstLane(source)};
    ir.WriteShared(32, base, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&hoist);
    program.blocks.push_back(&body);
    program.blocks.push_back(&merge);
    program.post_order_blocks.push_back(&hoist);
    program.post_order_blocks.push_back(&body);
    program.post_order_blocks.push_back(&merge);
    program.syntax_list.push_back({.data = {.block = &hoist},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back(
        {.data = {.if_node = {.cond = cond, .body = &body, .merge = &merge}},
         .type = IR::AbstractSyntaxNode::Type::If});
    program.syntax_list.push_back({.data = {.block = &body},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.data = {.end_if = {.merge = &merge}},
                                   .type = IR::AbstractSyntaxNode::Type::EndIf});
    program.syntax_list.push_back({.data = {.block = &merge},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = 32;
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Optimization::LowerWave64BallotPass(program, runtime, profile);
    DivergentForm form{};
    form.broadcasts = Count(body, IR::Opcode::ReadFirstLane);
    form.barriers = Count(body, IR::Opcode::Barrier) + Count(hoist, IR::Opcode::Barrier);
    form.in_hoist = Count(hoist, IR::Opcode::Barrier);
    form.loads = Count(hoist, IR::Opcode::LoadSharedU32) + Count(body, IR::Opcode::LoadSharedU32);
    form.writes = Count(hoist, IR::Opcode::WriteSharedU32) + Count(body, IR::Opcode::WriteSharedU32);
    form.shuffles = Count(hoist, IR::Opcode::Shuffle) + Count(body, IR::Opcode::Shuffle);
    form.ballots = Count(hoist, IR::Opcode::Ballot) + Count(body, IR::Opcode::Ballot);
    if (spirv_path) {
        Optimization::CollectShaderInfoPass(program, profile);
        Backend::Bindings bindings{};
        const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
        std::ofstream out(spirv_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        Check(bool(out) && !code.empty(), "the lowered divergent wave64 shader emits SPIR-V");
    }
    return form;
}

// tlg: the model of the exchange: the value every lane of the wave sees is the value of the
// first lane of the wave whose condition holds (the reference is the PS4's wave64). The
// condition of the test is the parity of the lane id, so the even lanes are active and the
// first active lane of the wave is the lane 0.
struct DivergentModel {
    static u32 Reference(bool varying) {
        return varying ? 100u : 7u;
    }
    /// The value the host's per-subgroup broadcast gives to a lane: the first active lane of
    /// its own half (the upper half reads its own lane 32).
    static u32 HostPerHalf(u32 lane, bool varying) {
        const u32 first = (lane / 32) * 32;
        return varying ? 100u + first : 7u;
    }
};

// tlg: a shader that reads the exec mask (s_mov v, exec) cannot be translated exactly
// (Vulkan has no exec mask) and used to kill the port with an assertion in the emitter
// ("EmitGetExec: Unreachable code"). The emitter now assumes every lane is active and says
// so in the log: the shader must still be emitted and be valid SPIR-V.
static bool ExecMaskShaderEmits(const char* spirv_path = nullptr) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 64;
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const IR::U1 exec = ir.GetExec();
    ir.WriteShared(32, IR::U32{ir.Select(exec, ir.Imm32(1u), ir.Imm32(0u))}, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = 32;
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    Optimization::CollectShaderInfoPass(program, profile);
    Backend::Bindings bindings{};
    const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    if (code.empty()) {
        return false;
    }
    if (spirv_path) {
        std::ofstream out(spirv_path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
        Check(bool(out), "the shader with the exec mask writes its SPIR-V");
    }
    return true;
}

// A 64-thread compute block with one ReadFirstLane (of a lane-varying value when `varying`) and
// no ballot: lowered for wave64 (two workgroup barriers) only with TLG_RFL_ALL.
static u32 ReadFirstLaneBarriers(bool varying) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 0;
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const IR::U32 source = varying ? IR::U32{ir.SharedAtomicIAdd(ir.Imm32(0u), ir.Imm32(1u), false)}
                                   : ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
    const IR::U32 first = ir.ReadFirstLane(source);
    ir.WriteShared(32, first, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = 32;
    Optimization::LowerWave64BallotPass(program, runtime, profile);
    return Count(block, IR::Opcode::Barrier);
}

// A 64-thread compute block with one ballot in uniform flow, on a 32-wide host: both 32-lane
// halves must read their wave's pair of LDS slots, so the 64-bit load's address is the slot
// address with bit 2 cleared (the odd half used to read its own slot and the next wave's).
static bool BallotReadsSlotPair() {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.shared_memory_size = 0;
    runtime.hw.cs.workgroup_size = {64, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const IR::U32 x = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U64 mask = ir.Ballot(ir.INotEqual(ir.BitwiseAnd(x, ir.Imm32(1u)), ir.Imm32(0u)));
    const IR::U32 low{ir.CompositeExtract(ir.UnpackUint2x32(mask), 0)};
    ir.WriteShared(32, low, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = 32;
    Optimization::LowerWave64BallotPass(program, runtime, profile);
    u32 loads = 0;
    bool paired = true;
    for (const IR::Inst& inst : block.Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::LoadSharedU64) {
            continue;
        }
        ++loads;
        const IR::Value address = inst.Arg(0);
        const IR::Inst* op = address.IsImmediate() ? nullptr : address.Inst();
        paired = paired && op && op->GetOpcode() == IR::Opcode::BitwiseAnd32 &&
                 op->Arg(1).IsImmediate() && op->Arg(1).U32() == ~7u;
    }
    return loads == 1 && paired;
}

static u32 WqmQuadBroadcasts(Gcn::OperandField dst, Gcn::OperandField src) {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    Profile profile{};
    Gcn::Translator translator(info, runtime, profile);
    Gcn::GcnInst inst{};
    inst.opcode = Gcn::Opcode::S_WQM_B64;
    inst.encoding = Gcn::InstEncoding::SOP1;
    inst.category = Gcn::InstCategory::ScalarALU;
    inst.length = 4;
    inst.src_count = 1;
    inst.dst_count = 1;
    inst.src[0].field = src;
    inst.dst[0].field = dst;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    translator.Translate(&block, 0, IR::Condition::True, std::span<const Gcn::GcnInst>{&inst, 1});
    const u32 broadcasts = Count(block, IR::Opcode::QuadBroadcast);
    if (broadcasts) {
        // One ballot reads the EXEC source (GetSrc64), one builds the quad mask.
        Check(Count(block, IR::Opcode::Ballot) == 2, "s_wqm_b64 ballots the quad mask");
        Check(Count(block, IR::Opcode::SetScc) == 1, "s_wqm_b64 sets SCC");
        Check(Count(block, IR::Opcode::SetVccLo) == 1, "s_wqm_b64 vcc, exec writes VCC");
    }
    return broadcasts;
}

// BP04: V_RCP_CLAMP_F32 / V_RSQ_CLAMP_F32 / V_LOG_CLAMP_F32 clamp to +-FLT_MAX (FPClamp32).
static u32 ClampOpClamps(Gcn::Opcode opcode) {
    Info info{};
    info.hw_stage = HwStage::Fragment;
    info.sw_stage = SwStage::Fragment;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Fragment, SwStage::Fragment);
    Profile profile{};
    Gcn::Translator translator(info, runtime, profile);
    Gcn::GcnInst inst{};
    inst.opcode = opcode;
    inst.encoding = Gcn::InstEncoding::VOP1;
    inst.category = Gcn::InstCategory::VectorALU;
    inst.length = 4;
    inst.src_count = 1;
    inst.dst_count = 1;
    inst.src[0].field = Gcn::OperandField::VectorGPR;
    inst.dst[0].field = Gcn::OperandField::VectorGPR;
    inst.src[0].code = 1;
    inst.dst[0].code = 2;
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    translator.Translate(&block, 0, IR::Condition::True, std::span<const Gcn::GcnInst>{&inst, 1});
    return Count(block, IR::Opcode::FPClamp32);
}

// BP06: in a 32-thread workgroup a ReadLane of lane 32 (past the populated lanes) after a
// reduction step becomes the reduction identity; without the backport it is left alone.
static u32 ReadLanePastWorkgroup(u32 host_subgroup) {
    Info info{};
    info.hw_stage = HwStage::Compute;
    info.sw_stage = SwStage::Compute;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Compute, SwStage::Compute);
    runtime.hw.cs.workgroup_size = {32, 1, 1};
    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    const IR::U32 x = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
    const IR::U32 step{ir.IAdd(x, ir.ShuffleXor(x, ir.Imm32(16u)))};
    const IR::U32 half = ir.ReadLane(step, ir.Imm32(32u));
    ir.WriteShared(32, half, ir.Imm32(0u));
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.post_order_blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block},
                                   .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});
    Profile profile{};
    profile.subgroup_size = host_subgroup;
    Optimization::LowerWave64BallotPass(program, runtime, profile);
    return Count(block, IR::Opcode::ReadLane);
}

int main(int argc, char** argv) {
    // Exercise the new paths unless the caller chose (no game serial here: the default needs one).
    SetEnv("TLG_LDS_SMALL", "1", false);
    SetEnv("TLG_LDS_MULTI", "1", false);
    SetEnv("TLG_LDS_READLANE", "1", false);
    SetEnv("TLG_LANE_DIVERGENCE", "1", false);
    SetEnv("TLG_BP06", "1", false); // default off: exercise it unless the caller chose
    const bool small = Toggles::SmallGroupLdsBarriers();
    const bool multi = Toggles::MultiWaveLdsBarriers();
    const bool wqm = Toggles::WholeQuadMode();
    std::printf("TLG_LDS_SMALL=%d TLG_LDS_MULTI=%d TLG_WQM=%d TLG_RFL_ALL=%d\n", small, multi, wqm,
                Toggles::ReadFirstLaneAll() ? 1 : 0);

    const BarrierResult wave = RunBarrierPass(64);
    Check(wave.workgroup == 3 && wave.subgroup == 0, "64 threads: workgroup barriers (unchanged)");
    const BarrierResult partial = RunBarrierPass(16);
    Check(partial.workgroup == (small ? 3u : 0u) && partial.subgroup == 0,
          "16 threads: workgroup barriers with TLG_LDS_SMALL");
    std::string spirv;
    if (argc > 1) {
        std::filesystem::create_directories(argv[1]);
        spirv = (std::filesystem::path{argv[1]} / "lds-multi-wave.spv").string();
    }
    // 32-wide host: every guest wave spans two host subgroups, so workgroup barriers.
    const BarrierResult several = RunBarrierPass(256, spirv.empty() ? nullptr : spirv.c_str());
    Check(several.workgroup == (multi ? 3u : 0u) && several.subgroup == 0,
          "256 threads, 32-wide host: workgroup barriers with TLG_LDS_MULTI");
    const BarrierResult odd = RunBarrierPass(96);
    Check(odd.workgroup == (multi ? 3u : 0u) && odd.subgroup == 0,
          "96 threads, 32-wide host: workgroup barriers with TLG_LDS_MULTI (was none)");
    const BarrierResult small48 = RunBarrierPass(48);
    Check(small48.workgroup == (small ? 3u : 0u) && small48.subgroup == 0,
          "48 threads: workgroup barriers with TLG_LDS_SMALL");
    // 64-wide host: each guest wave (the partial last one too) is one host subgroup.
    const BarrierResult wide = RunBarrierPass(256, nullptr, 64);
    Check(wide.workgroup == 0 && wide.subgroup == (multi ? 3u : 0u),
          "256 threads, 64-wide host: subgroup barriers with TLG_LDS_MULTI");
    const BarrierResult wide_odd = RunBarrierPass(96, nullptr, 64);
    Check(wide_odd.workgroup == 0 && wide_odd.subgroup == (multi ? 3u : 0u),
          "96 threads, 64-wide host: subgroup barriers with TLG_LDS_MULTI");
    const BarrierResult unknown = RunBarrierPass(192, nullptr, 0);
    Check(unknown.workgroup == (multi ? 3u : 0u) && unknown.subgroup == 0,
          "192 threads, unknown host subgroup: workgroup barriers");

    {
        Common::ObjectPool<IR::Inst> pool;
        IR::Block block(pool);
        IR::IREmitter ir(block);
        const IR::U32 tid = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        const IR::U32 index = ir.GetAttributeU32(IR::Attribute::LocalInvocationIndex);
        const IR::U32 lt_mask = ir.GetAttributeU32(IR::Attribute::SubgroupLtMask, 0);
        const IR::U32 wg = ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
        const IR::U32 old = ir.SharedAtomicIAdd(ir.Imm32(0u), ir.Imm32(1u), false);
        const bool lane = Toggles::LaneDivergence();
        Check(Optimization::IsLaneVaryingSource(*tid.Inst()), "LocalInvocationId varies per lane");
        Check(Optimization::IsLaneVaryingSource(*index.Inst()) == lane,
              "LocalInvocationIndex varies per lane with TLG_LANE_DIVERGENCE");
        Check(Optimization::IsLaneVaryingSource(*lt_mask.Inst()) == lane,
              "the mbcnt lane mask varies per lane with TLG_LANE_DIVERGENCE");
        Check(Optimization::IsLaneVaryingSource(*old.Inst()) == lane,
              "an atomic's old value varies per lane with TLG_LANE_DIVERGENCE");
        Check(!Optimization::IsLaneVaryingSource(*wg.Inst()), "WorkgroupId is uniform");
    }

    {
        // Binary info: found right after the code, rejected when its length runs past it.
        alignas(256) static u32 code[64]{};
        code[0] = 0xBF810000; // s_endpgm
        auto* info = reinterpret_cast<AmdGpu::BinaryInfo*>(&code[8]);
        std::memcpy(info->signature.data(), AmdGpu::BinaryInfo::signature_ref.data(),
                    info->signature.size());
        info->length = 8 * sizeof(u32);
        info->shader_hash = 0x1234;
        Check(AmdGpu::FindBinaryInfo(code) == info, "binary info right after the code is found");
        info->length = 40 * sizeof(u32);
        Check(AmdGpu::FindBinaryInfo(code) == nullptr,
              "binary info whose code would run past it is rejected");
        info->length = 0;
        Check(AmdGpu::FindBinaryInfo(code) == nullptr, "binary info with no code is rejected");
    }

    const bool rfl_all = Toggles::ReadFirstLaneAll();
    Check(ReadFirstLaneBarriers(true) == (rfl_all ? 2u : 0u),
          "ReadFirstLane of a lane-varying value is lowered for wave64 with TLG_RFL_ALL");
    Check(ReadFirstLaneBarriers(false) == 0, "ReadFirstLane of a uniform value is left alone");
    Check(BallotReadsSlotPair(), "wave64 ballot on a 32-wide host: both halves read the slot pair");

    // The append pattern (the wave reserves a slot with an atomic and broadcasts the base): on
    // a 32-wide host the whole wave has to read the wave's first lane. Without the exchange the
    // host broadcast is per subgroup and the upper half of the wave reads its own lane 32.
    const ReadFirstLaneForm append = RunReadFirstLaneForm(true, false);
    // The exchange is a LDS write, two workgroup barriers and a LDS load (the host broadcast of
    // the exchange stays in the shader: it is the even half's first active lane).
    Check(append.exchanges == (rfl_all ? 1u : 0u) && append.barriers == (rfl_all ? 2u : 0u) &&
              append.writes == (rfl_all ? 2u : 1u),
          "append pattern: the ReadFirstLane is exchanged through LDS (TLG_RFL_ALL)");
    Check(!rfl_all || append.even_half_slot,
          "append pattern: the exchange reads the even half's slot (the wave's first lane)");
    Check(AppendPattern::Base(AppendPattern::Form::Reference, 0, 100) == 100 &&
              AppendPattern::Base(AppendPattern::Form::Exchange, 63, 100) == 100 &&
              AppendPattern::Base(AppendPattern::Form::HostPerHalf, 63, 100) == 0,
          "append pattern: only the per subgroup broadcast loses the upper half of the wave");
    Check(AppendPattern::Slots(AppendPattern::Form::Reference, 100) == 64 &&
              AppendPattern::Slots(AppendPattern::Form::Exchange, 100) == 64 &&
              !AppendPattern::WritesInsideTheList(AppendPattern::Form::HostPerHalf, 100),
          "append pattern: the per subgroup broadcast writes the upper half outside the list");
    // A ReadFirstLane in divergent flow is exchanged too now: the exchange stays where the
    // instruction is (the barrier in non uniform control flow) or is hoisted to the block
    // that branches to the divergent region when the value is known there.
    const ReadFirstLaneForm divergent = RunReadFirstLaneForm(true, true);
    Check(rfl_all ? (divergent.broadcasts == 0 && divergent.barriers == 2)
                  : (divergent.broadcasts == 1 && divergent.barriers == 0),
          "append pattern in divergent flow: the ReadFirstLane is exchanged too (TLG_RFL_ALL)");
    // A ReadFirstLane in divergent flow is now exchanged too, with the exchange hoisted to
    // the block that branches to the divergent region (uniform flow, where the barrier is
    // allowed). The value it broadcasts has to be known before the branch.
    const DivergentForm divergent_hoisted = RunDivergentReadFirstLane(true);
    Check(rfl_all ? (divergent_hoisted.broadcasts == 0 && divergent_hoisted.in_hoist == 2 &&
                     divergent_hoisted.loads == 4 && divergent_hoisted.writes == 3 &&
                     divergent_hoisted.shuffles == 1 && divergent_hoisted.ballots == 1)
                  : (divergent_hoisted.broadcasts == 1 && divergent_hoisted.barriers == 0),
          "ReadFirstLane in divergent flow: the exchange is hoisted to the block that branches "
          "to the divergent region (TLG_RFL_ALL)");
    // The value is only known inside the divergent region (an atomic there): the exchange
    // cannot be hoisted, so it stays where the instruction is, with its barrier in non
    // uniform control flow.
    const DivergentForm divergent_inner = RunDivergentReadFirstLane(false);
    Check(rfl_all ? (divergent_inner.broadcasts == 0 && divergent_inner.barriers == 2 &&
                     divergent_inner.in_hoist == 0 && divergent_inner.loads == 4)
                  : (divergent_inner.broadcasts == 1 && divergent_inner.barriers == 0),
          "ReadFirstLane whose value is only known inside the divergent region is exchanged in "
          "place");
    // tlg: the workgroup of the game that crashed the port: 256 threads (16x16x1), four waves
    // of 64. The exchange has to give each wave its own slots, and the SPIR-V has to be
    // valid: the mask of the region is the condition of the branch (the emitter cannot read
    // the exec mask, and asking for it took the game down).
    for (const auto& shape : {std::pair<u32, u32>{64, 1}, {256, 16}}) {
        const DivergentForm big = RunDivergentReadFirstLane(false, nullptr, shape.first, shape.second);
        Check(rfl_all ? (big.broadcasts == 0 && big.barriers == 2 && big.loads == 4)
                      : (big.broadcasts == 1 && big.barriers == 0),
              "ReadFirstLane in divergent flow in a workgroup of 256 threads (16x16x1): the "
              "exchange is emitted and the shader is valid");
        if (!spirv.empty()) {
            const auto path =
                (std::filesystem::path{spirv}.parent_path() /
                 (shape.first == 64 ? "wave64-rfl-divergent-64.spv" : "wave64-rfl-divergent-256.spv"))
                    .string();
            RunDivergentReadFirstLane(false, path.c_str(), shape.first, shape.second);
        }
    }
    // The model: the exchange gives every lane the value of the first lane of the wave with
    // the condition set, and the per subgroup broadcast gives the upper half its own lane 32.
    Check(DivergentModel::Reference(true) == 100 && DivergentModel::HostPerHalf(63, true) == 132,
          "divergent exchange: only the per subgroup broadcast loses the upper half of the wave");
    if (!spirv.empty()) {
        // The SPIR-V of the lowered divergent shader (for spirv-val).
        const auto path = (std::filesystem::path{spirv}.parent_path() / "wave64-rfl-divergent.spv")
                              .string();
        RunDivergentReadFirstLane(true, path.c_str());
    }
    // Two waves in the workgroup: each wave has its own slot pair.
    const ReadFirstLaneForm two_waves = RunReadFirstLaneForm(true, false, 128);
    Check(two_waves.exchanges == (rfl_all ? 1u : 0u) && two_waves.barriers == (rfl_all ? 2u : 0u) &&
              (!rfl_all || two_waves.even_half_slot),
          "append pattern in a 128-thread workgroup: one exchange, the wave's own slot pair");
    if (!spirv.empty()) {
        // The SPIR-V of the lowered shader, next to the multi-wave one (for spirv-val).
        const auto path = (std::filesystem::path{spirv}.parent_path() / "wave64-rfl.spv").string();
        RunReadFirstLaneForm(true, false, 64, path.c_str());
    }

    Check(WqmQuadBroadcasts(Gcn::OperandField::VccLo, Gcn::OperandField::ExecLo) ==
              (wqm ? 4u : 0u),
          "s_wqm_b64 vcc, exec grows the mask to quads with TLG_WQM");
    Check(WqmQuadBroadcasts(Gcn::OperandField::ExecLo, Gcn::OperandField::ExecLo) == 0,
          "s_wqm_b64 exec, exec stays a no-op");
    {
        using Common::Backports::Id;
        const bool bp04 = Common::Backports::On(Id::ClampOpsPlain);
        const u32 want = bp04 ? 1u : 0u;
        Check(ClampOpClamps(Gcn::Opcode::V_RCP_CLAMP_F32) == want, "BP04: v_rcp_clamp_f32 clamps");
        Check(ClampOpClamps(Gcn::Opcode::V_RSQ_CLAMP_F32) == want, "BP04: v_rsq_clamp_f32 clamps");
        Check(ClampOpClamps(Gcn::Opcode::V_LOG_CLAMP_F32) == want, "BP04: v_log_clamp_f32 clamps");
        Check(ClampOpClamps(Gcn::Opcode::V_RSQ_F32) == 0, "BP04: v_rsq_f32 stays unclamped");
        const bool bp06 = Common::Backports::On(Id::MaximalReconvergence);
        // 32-lane host: the read is replaced with BP06; 64-lane host: likewise (it used to return
        // before touching the shader).
        Check(ReadLanePastWorkgroup(32) == (bp06 ? 0u : 1u),
              "BP06: readlane 32 in a 32-thread workgroup, 32-wide host");
        Check(ReadLanePastWorkgroup(64) == (bp06 ? 0u : 1u),
              "BP06: readlane 32 in a 32-thread workgroup, 64-wide host");
    }
    Check(ExecMaskShaderEmits(),
          "a shader that reads the exec mask is emitted instead of killing the port");
    if (!spirv.empty()) {
        const auto path =
            (std::filesystem::path{spirv}.parent_path() / "wave64-rfl-exec.spv").string();
        ExecMaskShaderEmits(path.c_str());
    }
    Check(Toggles::ShaderBinarySalt() ==
              ((small ? 1u : 0u) | (multi ? 2u : 0u) | (Toggles::LdsUniformReadLane() ? 4u : 0u) |
               (wqm ? 0u : 8u) | (Toggles::LaneDivergence() ? 16u : 0u) |
               (Toggles::ReadFirstLaneAll() ? 32u : 0u) |
               (Toggles::ReadFirstLaneDivergent() ? 64u : 0u)),
          "shader cache salt follows the switches");
    {
        // Backport switches: tlg.ini choices apply unless the environment sets the entry; only the
        // live ones can be switched while the game runs.
        using Common::Backports::Id;
        UnsetEnv("TLG_BP10");
        Common::Backports::ApplyConfig(static_cast<std::size_t>(Id::VertexIndexBarriers), false);
        Check(!Common::Backports::On(Id::VertexIndexBarriers), "ini choice turns BP10 off");
        SetEnv("TLG_BP10", "1", true);
        Common::Backports::ApplyConfig(static_cast<std::size_t>(Id::VertexIndexBarriers), true);
        Check(!Common::Backports::On(Id::VertexIndexBarriers), "environment variable wins over the ini (state kept)");
        UnsetEnv("TLG_BP10");
        Check(Common::Backports::SetLive(Id::VertexIndexBarriers, true) &&
                  Common::Backports::On(Id::VertexIndexBarriers),
              "a live backport can be switched in game");
        const bool before = Common::Backports::On(Id::ClampOpsPlain);
        Check(!Common::Backports::SetLive(Id::ClampOpsPlain, !before) &&
                  Common::Backports::On(Id::ClampOpsPlain) == before,
              "a shader-changing backport refuses a live switch");
    }
    std::printf("%s\n", failures ? "recompiler fixes: FAILED" : "recompiler fixes: all ok");
    return failures ? 1 : 0;
}

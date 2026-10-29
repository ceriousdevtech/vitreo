// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — the SSE4.1/4.2 forms lowered through a C++ helper into
// exec_sse4_ops: PCMPISTRI/M, PCMPESTRI/M, PHMINPOSUW, DPPS, DPPD and
// MPSADBW.
//
// One family of compile_sse's legacy SSE opcode space, split out of
// ops/sse.cpp (ADR-0006 step 3) as pure code motion: the case arms are
// verbatim. compile_sse (ops/sse.cpp) owns the dispatch chain and the
// shared end-of-instruction tail; see the declaration in jit_compiler.hpp
// for the nullopt / false / true contract.

#include "sse_internal.hpp"

namespace vitreo::cpu::jit {

// Intel SDM Vol. 2 PCMPISTRI/PCMPISTRM/PCMPESTRI/PCMPESTRM: packed string
// compare. Aggregation, implicit/explicit lengths, ECX/XMM0, and the
// non-standard CF/ZF/SF/OF/AF/PF writes live in exec_sse4_ops. PCMPESTR*
// reads EAX/EDX as lengths, so a memory EA is parked in RBX, never RAX.
// packed: kind[1:0] dst_xmm[5:2] imm8[15:8] is_mem[16]
extern "C" void jit_pcmpxstrx(CpuState* cpu, GuestMemory* mem,
                              u64 src2, u32 packed) noexcept {
    if (cpu == nullptr || mem == nullptr) {
        return;
    }

    static constexpr Opcode kOps[4] = {
        Opcode::PCMPISTRI_XMM_XMMM128_IMM8,
        Opcode::PCMPISTRM_XMM_XMMM128_IMM8,
        Opcode::PCMPESTRI_XMM_XMMM128_IMM8,
        Opcode::PCMPESTRM_XMM_XMMM128_IMM8,
    };

    const u32 kind = packed & 3u;
    const u8 dst_xmm = static_cast<u8>((packed >> 2) & 15u);
    const u8 imm = static_cast<u8>(packed >> 8);
    const bool is_mem = (packed & (1u << 16)) != 0;
    const bool from_vex = (packed & (1u << 17)) != 0;

    DecodedInstruction di{};
    di.opcode = kOps[kind];
    di.operands[0] = Operand::make_reg(OperandType::Reg128, dst_xmm);
    di.operands[2] = Operand::make_imm(OperandType::Imm8, imm);
    di.operand_count = 3;

    constexpr u8 kEaReg = static_cast<u8>(RegIndex::RBX);
    u64 saved_rbx = 0;
    if (is_mem) {
        saved_rbx = cpu->gpr.get64(kEaReg);
        cpu->gpr.set64(kEaReg, src2);
        MemoryOperand mop{};
        mop.base = kEaReg;
        di.operands[1] = Operand::make_mem(OperandType::Mem128, mop);
    } else {
        di.operands[1] = Operand::make_reg(OperandType::Reg128,
                                           static_cast<u8>(src2 & 15u));
    }

    ExecContext ctx{*cpu, *mem};
    ctx.vex_origin = from_vex;
    const ExecResult result = ::vitreo::cpu::ops::exec_sse4_ops(ctx, di);
    // PCMPxSTRx writes CF/ZF/SF/OF/AF/PF. exec_sse4_ops stores them in
    // rflags; drop the prior lazy op so a later Jcc does not revive it.
    cpu->set_flag_op(CpuState::FlagOp::None);

    if (is_mem) {
        cpu->gpr.set64(kEaReg, saved_rbx);
    }

    if (result == ExecResult::Exception && !cpu->pending_raise.active) {
        auto& pending = cpu->pending_raise;
        pending.active = true;
        pending.from_helper = true;
        pending.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION
        pending.flags = 0;
        pending.num_args = 2;
        pending.args[0] = 0;
        pending.args[1] = is_mem ? src2 : 0;
    }
}

// Intel SDM Vol. 2 PHMINPOSUW / DPPS / DPPD / MPSADBW. Lane selection,
// tree reduction, and SAD windows live in exec_sse4_ops. Memory EA is
// parked in RBX, never RAX.
// packed: kind[1:0] dst_xmm[5:2] imm8[15:8] is_mem[16]
// kinds: 0 PHMINPOSUW, 1 DPPS, 2 DPPD, 3 MPSADBW
extern "C" void jit_sse41_op(CpuState* cpu, GuestMemory* mem,
                             u64 src2, u32 packed) noexcept {
    if (cpu == nullptr || mem == nullptr) {
        return;
    }

    static constexpr Opcode kOps[4] = {
        Opcode::PHMINPOSUW_XMM_XMMM128,
        Opcode::DPPS_XMM_XMMM128_IMM8,
        Opcode::DPPD_XMM_XMMM128_IMM8,
        Opcode::MPSADBW_XMM_XMMM128_IMM8,
    };

    const u32 kind = packed & 3u;
    const u8 dst_xmm = static_cast<u8>((packed >> 2) & 15u);
    const u8 imm = static_cast<u8>(packed >> 8);
    const bool is_mem = (packed & (1u << 16)) != 0;
    const bool from_vex = (packed & (1u << 17)) != 0;

    DecodedInstruction di{};
    di.opcode = kOps[kind];
    di.operands[0] = Operand::make_reg(OperandType::Reg128, dst_xmm);
    if (kind == 0) {
        di.operand_count = 2;
    } else {
        di.operands[2] = Operand::make_imm(OperandType::Imm8, imm);
        di.operand_count = 3;
    }

    constexpr u8 kEaReg = static_cast<u8>(RegIndex::RBX);
    u64 saved_rbx = 0;
    if (is_mem) {
        saved_rbx = cpu->gpr.get64(kEaReg);
        cpu->gpr.set64(kEaReg, src2);
        MemoryOperand mop{};
        mop.base = kEaReg;
        di.operands[1] = Operand::make_mem(OperandType::Mem128, mop);
    } else {
        di.operands[1] = Operand::make_reg(OperandType::Reg128,
                                           static_cast<u8>(src2 & 15u));
    }

    ExecContext ctx{*cpu, *mem};
    ctx.vex_origin = from_vex;
    const ExecResult result = ::vitreo::cpu::ops::exec_sse4_ops(ctx, di);
    // PHMINPOSUW / DPPS / DPPD / MPSADBW do not write EFLAGS. Clearing
    // flag_op here used to drop a live SUB/CMP so `SUB; DPPS; Jcc` took
    // stale ZF and could spin.

    if (is_mem) {
        cpu->gpr.set64(kEaReg, saved_rbx);
    }

    if (result == ExecResult::Exception && !cpu->pending_raise.active) {
        auto& pending = cpu->pending_raise;
        pending.active = true;
        pending.from_helper = true;
        pending.code = 0xC0000005u;  // STATUS_ACCESS_VIOLATION
        pending.flags = 0;
        pending.num_args = 2;
        pending.args[0] = 0;
        pending.args[1] = is_mem ? src2 : 0;
    }
}

std::optional<bool> JitCompiler::compile_sse4_helper_ops(
    const DecodedInstruction& instr, u64 instr_rip, u64 next_rip) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1; (void)instr_rip;

    switch (instr.opcode) {

    // Intel SDM Vol. 2 PCMPISTRI/M / PCMPESTRI/M: helper → exec_sse4_ops.
    case Opcode::PCMPISTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPISTRM_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRI_XMM_XMMM128_IMM8:
    case Opcode::PCMPESTRM_XMM_XMMM128_IMM8: {
        if (instr.operand_count < 3) {
            return false;
        }
        u32 kind = 0;
        switch (instr.opcode) {
        case Opcode::PCMPISTRM_XMM_XMMM128_IMM8: kind = 1; break;
        case Opcode::PCMPESTRI_XMM_XMMM128_IMM8: kind = 2; break;
        case Opcode::PCMPESTRM_XMM_XMMM128_IMM8: kind = 3; break;
        default: break;
        }
        const u8 dst_xmm = op0.reg & 15;
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        u32 packed = kind | (static_cast<u32>(dst_xmm) << 2)
                     | (static_cast<u32>(imm) << 8);

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_memory_operand(op1.type)) {
            packed |= (1u << 16);
            // Bit 17 carries the guest encoding's VEX-ness across the helper
            // boundary. Without it the helper rebuilds a LEGACY-shaped
            // DecodedInstruction from a fixed opcode table, the interpreter
            // inside computes `aligned_form = true`, and a VEX form that
            // architecturally permits a misaligned operand raises #GP. Same
            // class of defect as the VPXOR fault that terminates a launcher, one layer
            // further out: there the bridge cleared prefixes.vex, here the
            // helper never had it.
            if (instr.prefixes.vex || m_current_original_vex)
                packed |= (1u << 17);
            emit_update_rip(instr_rip);
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(2, REG_SCRATCH4));
        } else if (is_register_operand(op1.type)) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit_mov_imm32(2, op1.reg & 15);
        } else {
            return false;
        }
        emit_mov_imm32(3, packed);
        EMIT_SSE_MEM_HELPER_CALL(jit_pcmpxstrx);
        emit_exit_if_pending_raise();
        break;
    }

    // Intel SDM Vol. 2 PHMINPOSUW / DPPS / DPPD / MPSADBW: helper → exec_sse4_ops.
    case Opcode::PHMINPOSUW_XMM_XMMM128:
    case Opcode::DPPS_XMM_XMMM128_IMM8:
    case Opcode::DPPD_XMM_XMMM128_IMM8:
    case Opcode::MPSADBW_XMM_XMMM128_IMM8: {
        u32 kind = 0;
        switch (instr.opcode) {
        case Opcode::DPPS_XMM_XMMM128_IMM8: kind = 1; break;
        case Opcode::DPPD_XMM_XMMM128_IMM8: kind = 2; break;
        case Opcode::MPSADBW_XMM_XMMM128_IMM8: kind = 3; break;
        default: break;
        }
        if (kind == 0) {
            if (instr.operand_count < 2) {
                return false;
            }
        } else if (instr.operand_count < 3) {
            return false;
        }
        const u8 dst_xmm = op0.reg & 15;
        const u8 imm = (kind == 0)
                           ? 0
                           : static_cast<u8>(instr.operands[2].imm);
        u32 packed = kind | (static_cast<u32>(dst_xmm) << 2)
                     | (static_cast<u32>(imm) << 8);

        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_MATERIALIZE >> 3));
        emit_helper_blr();

        if (is_memory_operand(op1.type)) {
            packed |= (1u << 16);
            // Bit 17 carries the guest encoding's VEX-ness across the helper
            // boundary. Without it the helper rebuilds a LEGACY-shaped
            // DecodedInstruction from a fixed opcode table, the interpreter
            // inside computes `aligned_form = true`, and a VEX form that
            // architecturally permits a misaligned operand raises #GP. Same
            // class of defect as the VPXOR fault that terminates a launcher, one layer
            // further out: there the bridge cleared prefixes.vex, here the
            // helper never had it.
            if (instr.prefixes.vex || m_current_original_vex)
                packed |= (1u << 17);
            emit_update_rip(instr_rip);
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit(arm64::mov_x(2, REG_SCRATCH4));
        } else if (is_register_operand(op1.type)) {
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
            emit_mov_imm32(2, op1.reg & 15);
        } else {
            return false;
        }
        emit_mov_imm32(3, packed);
        EMIT_SSE_MEM_HELPER_CALL(jit_sse41_op);
        emit_exit_if_pending_raise();
        break;
    }

    default: return std::nullopt;
    }

    return true;
}

} // namespace vitreo::cpu::jit

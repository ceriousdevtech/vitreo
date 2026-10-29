// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — Native ARM64 codegen for the hot MMX opcodes.
//
// These opcodes dominate x86-64 decompression (an LZMA-style range decoder:
// MOVD load, PMULUDQ multiply, PSRLQ shift, extract). Measured against a
// launcher self-update extraction, MOVD/MOVQ/PMULUDQ/PSRLQ/PSLLQ account for ~99.9% of
// all interpreter-delegate fallbacks. None of them had a JIT stencil in either
// mode, so every one round-tripped through decode + Interpreter::step.
//
// MMX semantics do not depend on operand-size (no REX effect): MMn is always
// the low 64 bits of the x87 register st[n]. The one subtlety is that writing
// an MM register also sets st[n]'s exponent/sign halfword to 0xFFFF (the "MMX
// in use" tag) — see FpuState::set_mm. emit_store_mm replicates both writes so
// x87/MMX tag state matches the interpreter exactly.
//
// Memory operands use 64-bit-safe addressing (emit_compute_ea64 + the u64
// memory helpers), so this file is wired into the 64-bit dispatch path. The
// register/immediate forms touch no guest memory and are pure register ops.

#include "../jit_helpers.hpp"

namespace vitreo::cpu::jit {

namespace {
inline bool is_mmreg(const Operand& o) noexcept {
    return o.type == OperandType::RegMM;
}
inline bool is_gpr32(const Operand& o) noexcept {
    return o.type == OperandType::Reg32;
}
inline bool is_mem_op(const Operand& o) noexcept {
    return is_memory_operand(o.type);
}
// stencils.hpp ZIP1/ZIP2 are Q=1 (16B/8H/4S). MMX is 64-bit; after fmov the
// high 64 of V is zero, so Q=1 ZIP2 would unpack zeros. Clear Q for 8B/4H/2S.
constexpr u32 zip_mm64(u32 enc) noexcept { return enc & ~0x40000000u; }
} // namespace

// exponent_sign must sit exactly 8 bytes past the mantissa inside Float80, so
// emit_store_mm can reach the tag with a fixed +8 offset from the mantissa
// address it already computed.
static_assert(offsetof(Float80, exponent_sign) - offsetof(Float80, mantissa) == 8,
              "MMX tag halfword must be 8 bytes past the mantissa");

void JitCompiler::emit_load_mm(u8 dst_arm_reg, u8 mm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    const u32 off = mm_mantissa_off(mm_reg);
    // Per-register offsets aren't 8-aligned (Float80 stride is 10), so form
    // the address then do an unaligned 64-bit load (fine on Apple Silicon).
    emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, off));
    emit(arm64::ldr_x_uoff(dst_arm_reg, REG_SCRATCH3, 0));
}

void JitCompiler::emit_store_mm(u8 src_arm_reg, u8 mm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    const u32 off = mm_mantissa_off(mm_reg);
    emit(arm64::add_x_imm(REG_SCRATCH3, REG_CPU_STATE, off));   // X3 = &mantissa
    emit(arm64::str_x_uoff(src_arm_reg, REG_SCRATCH3, 0));      // mantissa = src
    // Set the x87 "MMX in use" tag: exponent_sign = 0xFFFF, at mantissa+8.
    // strh_uoff scales the immediate by 2, so #8 bytes == imm12 4.
    emit(arm64::movz_w(REG_EA_TMP, 0xFFFF));
    emit(arm64::strh_uoff(REG_EA_TMP, REG_SCRATCH3, 4));
}

// Compute the effective address of `memop` into X1 (full 64-bit, zero-extended),
// mode-aware. In 32-bit mode the address is 32-bit and a guest fault must report
// this instruction's RIP, so set RIP first (the 64-bit dispatcher already emits
// that update before calling us, so only do it here for 32-bit).
#define MMX_EA_TO_X1(memop)                                                   \
    do {                                                                      \
        if (m_block_is_64bit) {                                               \
            emit_compute_ea64(REG_SCRATCH4, (memop), next_rip);              \
            emit(arm64::mov_x(1, REG_SCRATCH4));                             \
        } else {                                                             \
            emit_update_rip(instr_rip);                                      \
            emit_compute_ea(REG_SCRATCH4, (memop), next_rip);               \
            emit(arm64::mov_w(1, REG_SCRATCH4));  /* zero-extend to X1 */    \
        }                                                                    \
    } while (0)

// Load 64 bits from the guest EA of `memop` into X(dst_x).
#define MMX_LOAD_MEM64(memop, dst_x)                                          \
    do {                                                                      \
        if (m_block_is_64bit) {                                               \
            emit_compute_ea64(REG_SCRATCH4, (memop), next_rip);               \
        } else {                                                              \
            emit_update_rip(instr_rip);                                       \
            emit_compute_ea(REG_SCRATCH4, (memop), next_rip);                  \
        }                                                                     \
        emit_guest_load64((dst_x), REG_SCRATCH4);                             \
    } while (0)

// Load 16 bits from the guest EA, zero-extended to 64, into X(dst_x).
#define MMX_LOAD_MEM16_ZX(memop, dst_x)                                       \
    do {                                                                      \
        MMX_EA_TO_X1(memop);                                                  \
        emit(arm64::mov_x(0, REG_JIT_CTX));                                   \
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_READ16 >> 3));    \
        emit_helper_blr();                                                    \
        emit_exit_if_pending_raise();                                         \
        emit(arm64::mov_w(0, 0));   /* zero-extend u32 return to 64 bits */   \
        if ((dst_x) != 0) emit(arm64::mov_x((dst_x), 0));                     \
    } while (0)

// Load 32 bits from the guest EA, zero-extended to 64, into X(dst_x).
#define MMX_LOAD_MEM32_ZX(memop, dst_x)                                       \
    do {                                                                      \
        MMX_EA_TO_X1(memop);                                                  \
        emit(arm64::mov_x(0, REG_JIT_CTX));                                   \
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_READ32 >> 3));    \
        emit_helper_blr();                                                    \
        emit_exit_if_pending_raise();                                         \
        emit(arm64::mov_w(0, 0));   /* zero-extend u32 return to 64 bits */   \
        if ((dst_x) != 0) emit(arm64::mov_x((dst_x), 0));                     \
    } while (0)

// Store the low 32 bits in W(src_w) to the guest EA of `memop` (write32).
#define MMX_STORE_MEM32(memop, src_w)                                         \
    do {                                                                      \
        emit(arm64::mov_w(17, (src_w)));                                      \
        MMX_EA_TO_X1(memop);                                                  \
        emit(arm64::mov_w(2, 17));                                            \
        emit(arm64::mov_x(0, REG_JIT_CTX));                                   \
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_WRITE32 >> 3));   \
        emit_helper_blr();                                                    \
        emit_exit_if_pending_raise();                                         \
    } while (0)

// Store the 64 bits in X(src_x) to the guest EA of `memop`.
#define MMX_STORE_MEM64(memop, src_x)                                         \
    do {                                                                      \
        emit(arm64::mov_x(17, (src_x)));                                      \
        if (m_block_is_64bit) {                                               \
            emit_compute_ea64(REG_SCRATCH4, (memop), next_rip);               \
        } else {                                                              \
            emit_update_rip(instr_rip);                                       \
            emit_compute_ea(REG_SCRATCH4, (memop), next_rip);                  \
        }                                                                     \
        emit_guest_store64(17, REG_SCRATCH4);                                 \
    } while (0)

bool JitCompiler::compile_mmx(const DecodedInstruction& instr,
                              u64 instr_rip, u64 next_rip,
                              bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- MOVQ mm, mm/m64 — 64-bit move into MM ----
    case Opcode::MOVQ_MM_MMM64: {
        if (!is_mmreg(op0)) return false;
        if (is_mmreg(op1)) {
            emit_load_mm(REG_SCRATCH0, op1.reg);
        } else if (is_mem_op(op1)) {
            MMX_LOAD_MEM64(op1.mem, REG_SCRATCH0);
        } else {
            return false;
        }
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- MOVQ mm/m64, mm — 64-bit move out of MM ----
    case Opcode::MOVQ_MMM64_MM: {
        if (!is_mmreg(op1)) return false;
        emit_load_mm(REG_SCRATCH0, op1.reg);
        if (is_mmreg(op0)) {
            emit_store_mm(REG_SCRATCH0, op0.reg);
        } else if (is_mem_op(op0)) {
            MMX_STORE_MEM64(op0.mem, REG_SCRATCH0);
        } else {
            return false;
        }
        break;
    }

    // ---- MOVD mm, r/m32 — load dword, zero-extend into MM ----
    case Opcode::MOVD_MM_RM32: {
        if (!is_mmreg(op0)) return false;
        if (is_gpr32(op1)) {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);  // W-load zero-extends
        } else if (is_mem_op(op1)) {
            MMX_LOAD_MEM32_ZX(op1.mem, REG_SCRATCH0);
        } else {
            return false;
        }
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- MOVD r/m32, mm — store low dword of MM ----
    case Opcode::MOVD_RM32_MM: {
        if (!is_mmreg(op1)) return false;
        emit_load_mm(REG_SCRATCH0, op1.reg);         // X0 = mm; low 32 = W0
        if (is_gpr32(op0)) {
            emit_store_gpr32(REG_SCRATCH0, op0.reg); // writing r32 zeroes upper
        } else if (is_mem_op(op0)) {
            MMX_STORE_MEM32(op0.mem, REG_SCRATCH0);
        } else {
            return false;
        }
        break;
    }

    // ---- PMULUDQ mm, mm/m64 — (u32)dst * (u32)src -> 64-bit MM ----
    case Opcode::PMULUDQ_MM_MMM64: {
        if (!is_mmreg(op0)) return false;
        if (is_mmreg(op1)) {
            emit_load_mm(REG_SCRATCH0, op0.reg);      // X0 = dst
            emit_load_mm(REG_SCRATCH1, op1.reg);      // X1 = src
        } else if (is_mem_op(op1)) {
            // The read64 helper clobbers X0-X17, so load src via the helper
            // first, park it in X1, then reload dst from the register file.
            MMX_LOAD_MEM64(op1.mem, REG_SCRATCH1);    // X1 = src
            emit_load_mm(REG_SCRATCH0, op0.reg);      // X0 = dst
        } else {
            return false;
        }
        // Only the low dword of each operand participates; UMULL takes W regs.
        emit(arm64::umull(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- PSRLQ mm, imm8 / PSLLQ mm, imm8 — 64-bit logical shift by imm ----
    case Opcode::PSRLQ_MM_IMM8:
    case Opcode::PSLLQ_MM_IMM8: {
        if (!is_mmreg(op0)) return false;
        const u8 sh = static_cast<u8>(op1.imm & 0xFF);
        emit_load_mm(REG_SCRATCH0, op0.reg);
        if (sh > 63) {
            // x86: a shift count > 63 yields 0 (not an ARM64-style masked shift).
            emit(arm64::movz_w(REG_SCRATCH0, 0));     // X0 = 0 (upper cleared)
        } else if (sh != 0) {
            if (instr.opcode == Opcode::PSRLQ_MM_IMM8)
                emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH0, sh));
            else
                emit(arm64::lsl_x_imm(REG_SCRATCH0, REG_SCRATCH0, sh));
        }
        // sh == 0 is a no-op on the value, but still writes MM (and its tag).
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    // ---- Packed MMX integer ops (Intel SDM Vol. 2). MM is the low 64 bits
    // of st[n]; emit_store_mm writes mantissa and exponent_sign = 0xFFFF. ----

#define MMX_LOAD_DST_SRC()                                                    \
    do {                                                                      \
        if (!is_mmreg(op0)) return false;                                     \
        if (is_mmreg(op1)) {                                                  \
            emit_load_mm(REG_SCRATCH0, op0.reg);                              \
            emit_load_mm(REG_SCRATCH1, op1.reg);                              \
        } else if (is_mem_op(op1)) {                                          \
            MMX_LOAD_MEM64(op1.mem, REG_SCRATCH1);                            \
            emit_load_mm(REG_SCRATCH0, op0.reg);                              \
        } else {                                                              \
            return false;                                                     \
        }                                                                     \
    } while (0)

#define MMX_NEON_BINOP(ENC)                                                   \
    do {                                                                      \
        MMX_LOAD_DST_SRC();                                                   \
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));                          \
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));                          \
        emit(ENC);                                                            \
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));                          \
        emit_store_mm(REG_SCRATCH0, op0.reg);                                 \
    } while (0)

    case Opcode::PADDB_MM_MMM64:
        MMX_NEON_BINOP(arm64::add_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PADDW_MM_MMM64:
        MMX_NEON_BINOP(arm64::add_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PADDD_MM_MMM64:
        MMX_NEON_BINOP(arm64::add_v(0, 0, 1, arm64::VS_S));
        break;
    case Opcode::PADDQ_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::add_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSUBB_MM_MMM64:
        MMX_NEON_BINOP(arm64::sub_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PSUBW_MM_MMM64:
        MMX_NEON_BINOP(arm64::sub_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PSUBD_MM_MMM64:
        MMX_NEON_BINOP(arm64::sub_v(0, 0, 1, arm64::VS_S));
        break;

    case Opcode::PMULLW_MM_MMM64:
        MMX_NEON_BINOP(arm64::mul_v(0, 0, 1, arm64::VS_H));
        break;

    case Opcode::PAND_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::and_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }
    case Opcode::PANDN_MM_MMM64: {
        // Intel SDM Vol. 2 PANDN: DEST = NOT(DEST) AND SRC.
        MMX_LOAD_DST_SRC();
        emit(arm64::bic_x(REG_SCRATCH0, REG_SCRATCH1, REG_SCRATCH0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }
    case Opcode::POR_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::orr_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }
    case Opcode::PXOR_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::eor_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PCMPEQB_MM_MMM64:
        MMX_NEON_BINOP(arm64::cmeq_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PCMPEQW_MM_MMM64:
        MMX_NEON_BINOP(arm64::cmeq_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PCMPEQD_MM_MMM64:
        MMX_NEON_BINOP(arm64::cmeq_v(0, 0, 1, arm64::VS_S));
        break;
    case Opcode::PCMPGTB_MM_MMM64:
        MMX_NEON_BINOP(arm64::cmgt_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PCMPGTW_MM_MMM64:
        MMX_NEON_BINOP(arm64::cmgt_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PCMPGTD_MM_MMM64:
        MMX_NEON_BINOP(arm64::cmgt_v(0, 0, 1, arm64::VS_S));
        break;

    case Opcode::PUNPCKLBW_MM_MMM64:
        MMX_NEON_BINOP(zip_mm64(arm64::zip1_v(0, 0, 1, arm64::VS_B)));
        break;
    case Opcode::PUNPCKLWD_MM_MMM64:
        MMX_NEON_BINOP(zip_mm64(arm64::zip1_v(0, 0, 1, arm64::VS_H)));
        break;
    case Opcode::PUNPCKLDQ_MM_MMM64:
        MMX_NEON_BINOP(zip_mm64(arm64::zip1_v(0, 0, 1, arm64::VS_S)));
        break;
    case Opcode::PUNPCKHBW_MM_MMM64:
        MMX_NEON_BINOP(zip_mm64(arm64::zip2_v(0, 0, 1, arm64::VS_B)));
        break;
    case Opcode::PUNPCKHWD_MM_MMM64:
        MMX_NEON_BINOP(zip_mm64(arm64::zip2_v(0, 0, 1, arm64::VS_H)));
        break;
    case Opcode::PUNPCKHDQ_MM_MMM64:
        MMX_NEON_BINOP(zip_mm64(arm64::zip2_v(0, 0, 1, arm64::VS_S)));
        break;
    // MMX is only 64 bits, so the sole qword is both "high" and "low".
    // Match interpreter: PUNPCKHQDQ mm, mm/m64 → dst ← src (same as MOVQ).
    case Opcode::PUNPCKHQDQ_MM_MMM64: {
        if (!is_mmreg(op0)) return false;
        if (is_mmreg(op1)) {
            emit_load_mm(REG_SCRATCH0, op1.reg);
        } else if (is_mem_op(op1)) {
            MMX_LOAD_MEM64(op1.mem, REG_SCRATCH0);
        } else {
            return false;
        }
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PACKUSWB_MM_MMM64: {
        // Signed words → unsigned saturated bytes; dest words in low 4 bytes,
        // src words in high 4. SQXTUN Vd.8B, Vn.8H matches Intel SDM Vol. 2.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::ins_v_elem(0, 1, 1, 0, arm64::VS_D));
        emit(arm64::sqxtun_v(0, 0, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSRLW_MM_IMM8:
    case Opcode::PSRAW_MM_IMM8:
    case Opcode::PSLLW_MM_IMM8: {
        // Intel SDM Vol. 2: count > 15 → 0 (logical) or sign-fill (PSRAW).
        if (!is_mmreg(op0)) return false;
        const u8 sh = static_cast<u8>(op1.imm & 0xFF);
        const bool arith = instr.opcode == Opcode::PSRAW_MM_IMM8;
        const bool left = instr.opcode == Opcode::PSLLW_MM_IMM8;
        emit_load_mm(REG_SCRATCH0, op0.reg);
        if (left) {
            if (sh >= 16) {
                emit(arm64::movz_w(REG_SCRATCH0, 0));
            } else if (sh != 0) {
                emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
                emit(arm64::shl_v_imm(0, 0, sh, arm64::VS_H));
                emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
            }
        } else if (sh >= 16) {
            if (arith) {
                emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
                emit(arm64::sshr_v_imm(0, 0, 16, arm64::VS_H));
                emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
            } else {
                emit(arm64::movz_w(REG_SCRATCH0, 0));
            }
        } else if (sh != 0) {
            emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
            if (arith)
                emit(arm64::sshr_v_imm(0, 0, sh, arm64::VS_H));
            else
                emit(arm64::ushr_v_imm(0, 0, sh, arm64::VS_H));
            emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        }
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSRLD_MM_IMM8:
    case Opcode::PSRAD_MM_IMM8:
    case Opcode::PSLLD_MM_IMM8: {
        if (!is_mmreg(op0)) return false;
        const u8 sh = static_cast<u8>(op1.imm & 0xFF);
        const bool arith = instr.opcode == Opcode::PSRAD_MM_IMM8;
        const bool left = instr.opcode == Opcode::PSLLD_MM_IMM8;
        emit_load_mm(REG_SCRATCH0, op0.reg);
        if (left) {
            if (sh >= 32) {
                emit(arm64::movz_w(REG_SCRATCH0, 0));
            } else if (sh != 0) {
                emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
                emit(arm64::shl_v_imm(0, 0, sh, arm64::VS_S));
                emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
            }
        } else if (sh >= 32) {
            if (arith) {
                emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
                emit(arm64::sshr_v_imm(0, 0, 32, arm64::VS_S));
                emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
            } else {
                emit(arm64::movz_w(REG_SCRATCH0, 0));
            }
        } else if (sh != 0) {
            emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
            if (arith)
                emit(arm64::sshr_v_imm(0, 0, sh, arm64::VS_S));
            else
                emit(arm64::ushr_v_imm(0, 0, sh, arm64::VS_S));
            emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        }
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSLLQ_MM_MMM64:
    case Opcode::PSRLQ_MM_MMM64: {
        // Count is the full 64-bit source; count > 63 yields 0 (not ARM mask).
        MMX_LOAD_DST_SRC();
        if (instr.opcode == Opcode::PSLLQ_MM_MMM64)
            emit(arm64::lslv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        else
            emit(arm64::lsrv_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::cmp_x_imm(REG_SCRATCH1, 64));
        emit(arm64::csel_x(REG_SCRATCH0, /*XZR*/ 31, REG_SCRATCH0, arm64::CS));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSRLW_MM_MMM64:
    case Opcode::PSRAW_MM_MMM64:
    case Opcode::PSLLW_MM_MMM64:
    case Opcode::PSRLD_MM_MMM64:
    case Opcode::PSRAD_MM_MMM64:
    case Opcode::PSLLD_MM_MMM64: {
        // Full 64-bit count; clamp to lane width so USHL/SSHL match SDM
        // (count > width → 0 logical / sign-fill arithmetic).
        MMX_LOAD_DST_SRC();
        const bool word =
            instr.opcode == Opcode::PSRLW_MM_MMM64 ||
            instr.opcode == Opcode::PSRAW_MM_MMM64 ||
            instr.opcode == Opcode::PSLLW_MM_MMM64;
        const bool arith =
            instr.opcode == Opcode::PSRAW_MM_MMM64 ||
            instr.opcode == Opcode::PSRAD_MM_MMM64;
        const bool left =
            instr.opcode == Opcode::PSLLW_MM_MMM64 ||
            instr.opcode == Opcode::PSLLD_MM_MMM64;
        const u32 width = word ? 16u : 32u;
        const auto vsz = word ? arm64::VS_H : arm64::VS_S;
        emit(arm64::movz_w(REG_SCRATCH2, static_cast<u16>(width)));
        emit(arm64::cmp_x_imm(REG_SCRATCH1, width));
        emit(arm64::csel_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1,
                           arm64::CS));
        if (!left)
            emit(arm64::neg_w(REG_SCRATCH2, REG_SCRATCH2));
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::dup_v_gpr(1, REG_SCRATCH2, vsz));
        if (arith)
            emit(arm64::sshl_v(0, 0, 1, vsz));
        else
            emit(arm64::ushl_v(0, 0, 1, vsz));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PMAXSW_MM_MMM64:
        MMX_NEON_BINOP(arm64::smax_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PMINSW_MM_MMM64:
        MMX_NEON_BINOP(arm64::smin_v(0, 0, 1, arm64::VS_H));
        break;

    case Opcode::PADDUSB_MM_MMM64:
        MMX_NEON_BINOP(arm64::uqadd_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PADDUSW_MM_MMM64:
        MMX_NEON_BINOP(arm64::uqadd_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PSUBUSB_MM_MMM64:
        MMX_NEON_BINOP(arm64::uqsub_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PSUBUSW_MM_MMM64:
        MMX_NEON_BINOP(arm64::uqsub_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PADDSB_MM_MMM64:
        MMX_NEON_BINOP(arm64::sqadd_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PADDSW_MM_MMM64:
        MMX_NEON_BINOP(arm64::sqadd_v(0, 0, 1, arm64::VS_H));
        break;
    case Opcode::PSUBSB_MM_MMM64:
        MMX_NEON_BINOP(arm64::sqsub_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PSUBSW_MM_MMM64:
        MMX_NEON_BINOP(arm64::sqsub_v(0, 0, 1, arm64::VS_H));
        break;

    case Opcode::PMINUB_MM_MMM64:
        MMX_NEON_BINOP(arm64::umin_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PMAXUB_MM_MMM64:
        MMX_NEON_BINOP(arm64::umax_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PAVGB_MM_MMM64:
        MMX_NEON_BINOP(arm64::urhadd_v(0, 0, 1, arm64::VS_B));
        break;
    case Opcode::PAVGW_MM_MMM64:
        MMX_NEON_BINOP(arm64::urhadd_v(0, 0, 1, arm64::VS_H));
        break;

    case Opcode::PSUBQ_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::sub_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PMULHW_MM_MMM64: {
        // Signed word products → high 16 bits. SMULL.4S + UZP2.8H.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::smull_v(2, 0, 1, arm64::VS_H));
        emit(arm64::uzp2_v(0, 2, 2, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }
    case Opcode::PMULHUW_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::umull_v(2, 0, 1, arm64::VS_H));
        emit(arm64::uzp2_v(0, 2, 2, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSADBW_MM_MMM64: {
        // Abs-diff each byte, sum into DEST[15:0]; clear DEST[63:16].
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::uabd_v(0, 0, 1, arm64::VS_B));
        emit(arm64::uaddlv_16b(0, 0));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PACKSSDW_MM_MMM64: {
        // Signed dwords → signed saturated words; dest in low two, src in high.
        // SQXTN Vd.4H, Vn.4S — Intel SDM Vol. 2 PACKSSDW.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::ins_v_elem(0, 1, 1, 0, arm64::VS_D));
        emit(arm64::sqxtn_v(0, 0, arm64::VS_S));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PACKSSWB_MM_MMM64: {
        // Signed words → signed saturated bytes; dest in low 4, src in high 4.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::ins_v_elem(0, 1, 1, 0, arm64::VS_D));
        emit(arm64::sqxtn_v(0, 0, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PMADDWD_MM_MMM64: {
        // Adjacent signed-word products, wrapping dword add. Intel SDM Vol. 2:
        // 0x8000*0x8000 twice yields 0x80000000. SMULL.4S + ADDP.4S matches.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::smull_v(2, 0, 1, arm64::VS_H));
        emit(arm64::addp_v(0, 2, 2, arm64::VS_S));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSHUFW_MM_MMM64_IMM8: {
        // DEST word i := SRC word imm8[2i+1:2i]. Intel SDM Vol. 2 PSHUFW.
        if (!is_mmreg(op0)) return false;
        if (is_mmreg(op1)) {
            emit_load_mm(REG_SCRATCH0, op1.reg);
        } else if (is_mem_op(op1)) {
            MMX_LOAD_MEM64(op1.mem, REG_SCRATCH0);
        } else {
            return false;
        }
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        u64 idx = 0;
        for (int lane = 0; lane < 4; ++lane) {
            const u32 sel = (static_cast<u32>(imm) >> (lane * 2)) & 3u;
            idx |= static_cast<u64>(sel * 2u) << (lane * 16);
            idx |= static_cast<u64>(sel * 2u + 1u) << (lane * 16 + 8);
        }
        emit_mov_imm64(REG_SCRATCH1, static_cast<i64>(idx));
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::tbl_v16b_1(0, 0, 1));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PMOVMSKB_R32_MM: {
        // r32 := sign bits of the 8 MM bytes; MM is not rewritten.
        if (!is_gpr32(op0) || !is_mmreg(op1)) return false;
        emit_load_mm(REG_SCRATCH0, op1.reg);
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::ushr_v_imm(0, 0, 7, arm64::VS_B));
        emit(arm64::umov_w_b(REG_SCRATCH0, 0, 0));
        for (u8 lane = 1; lane < 8; ++lane) {
            emit(arm64::umov_w_b(REG_SCRATCH1, 0, lane));
            emit(arm64::orr_w_lsl(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1,
                                  lane));
        }
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PINSRW_MM_R32M16_IMM8: {
        // SEL := imm8[1:0]; insert low 16 bits of r32/m16. Intel SDM Vol. 2.
        if (!is_mmreg(op0)) return false;
        if (is_gpr32(op1)) {
            emit_load_mm(REG_SCRATCH0, op0.reg);
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else if (is_mem_op(op1)) {
            MMX_LOAD_MEM16_ZX(op1.mem, REG_SCRATCH1);
            emit_load_mm(REG_SCRATCH0, op0.reg);
        } else {
            return false;
        }
        const u8 sel = static_cast<u8>(instr.operands[2].imm) & 3u;
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::ins_v_gpr(0, sel, REG_SCRATCH1, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PEXTRW_R32_MM_IMM8: {
        // SEL := imm8[1:0]; zero-extend the word into r32. MM is not rewritten.
        if (!is_gpr32(op0) || !is_mmreg(op1)) return false;
        const u8 sel = static_cast<u8>(instr.operands[2].imm) & 3u;
        emit_load_mm(REG_SCRATCH0, op1.reg);
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::umov_w_h(REG_SCRATCH0, 0, sel));
        emit_store_gpr32(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSHUFB_MM_MMM64: {
        // 64-bit PSHUFB: bit7 of each control byte zeros dest; else index[2:0].
        // AND 0x87 then TBL: bit7 → 0x80..0x87 (out of range → 0).
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::movi_v16b(2, 0x87));
        emit(arm64::and_v16b(1, 1, 2));
        emit(arm64::tbl_v16b_1(0, 0, 1));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PHADDW_MM_MMM64: {
        // Q=0 ADDP.4H: [d0+d1, d2+d3, s0+s1, s2+s3]. Q=1 would pull zeros.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(zip_mm64(arm64::addp_v(0, 0, 1, arm64::VS_H)));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PHADDD_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(zip_mm64(arm64::addp_v(0, 0, 1, arm64::VS_S)));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PHADDSW_MM_MMM64: {
        // Q=0 UZP packs even/odd words; SQADD saturates. Intel SDM Vol. 2.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(zip_mm64(arm64::uzp1_v(2, 0, 1, arm64::VS_H)));
        emit(zip_mm64(arm64::uzp2_v(0, 0, 1, arm64::VS_H)));
        emit(arm64::sqadd_v(0, 2, 0, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PHSUBW_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(zip_mm64(arm64::uzp1_v(2, 0, 1, arm64::VS_H)));
        emit(zip_mm64(arm64::uzp2_v(0, 0, 1, arm64::VS_H)));
        emit(arm64::sub_v(0, 2, 0, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PHSUBD_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(zip_mm64(arm64::uzp1_v(2, 0, 1, arm64::VS_S)));
        emit(zip_mm64(arm64::uzp2_v(0, 0, 1, arm64::VS_S)));
        emit(arm64::sub_v(0, 2, 0, arm64::VS_S));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PHSUBSW_MM_MMM64: {
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(zip_mm64(arm64::uzp1_v(2, 0, 1, arm64::VS_H)));
        emit(zip_mm64(arm64::uzp2_v(0, 0, 1, arm64::VS_H)));
        emit(arm64::sqsub_v(0, 2, 0, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PABSB_MM_MMM64:
    case Opcode::PABSW_MM_MMM64:
    case Opcode::PABSD_MM_MMM64: {
        // DEST := abs(SRC); INT_MIN stays INT_MIN (no saturate). Intel SDM.
        if (!is_mmreg(op0)) return false;
        if (is_mmreg(op1)) {
            emit_load_mm(REG_SCRATCH0, op1.reg);
        } else if (is_mem_op(op1)) {
            MMX_LOAD_MEM64(op1.mem, REG_SCRATCH0);
        } else {
            return false;
        }
        const auto vsz = instr.opcode == Opcode::PABSB_MM_MMM64 ? arm64::VS_B
                       : instr.opcode == Opcode::PABSW_MM_MMM64 ? arm64::VS_H
                                                                : arm64::VS_S;
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::abs_v(0, 0, vsz));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PSIGNB_MM_MMM64:
    case Opcode::PSIGNW_MM_MMM64:
    case Opcode::PSIGND_MM_MMM64: {
        // src<0 → negate dst; src==0 → zero; else keep dst. Intel SDM Vol. 2.
        MMX_LOAD_DST_SRC();
        const auto vsz = instr.opcode == Opcode::PSIGNB_MM_MMM64 ? arm64::VS_B
                       : instr.opcode == Opcode::PSIGNW_MM_MMM64 ? arm64::VS_H
                                                                : arm64::VS_S;
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::cmlt_v_zero(2, 1, vsz));
        emit(arm64::cmeq_v_zero(3, 1, vsz));
        emit(arm64::neg_v(4, 0, vsz));
        emit(arm64::bsl_v16b(2, 4, 0));
        emit(arm64::bic_v16b(0, 2, 3));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PMADDUBSW_MM_MMM64: {
        // Unsigned dest bytes × signed src bytes; adjacent pairs; sat i16.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::uxtl_v(2, 0, arm64::VS_H));
        emit(arm64::sxtl_v(3, 1, arm64::VS_H));
        emit(arm64::mul_v(2, 2, 3, arm64::VS_H));
        emit(arm64::saddlp_v(2, 2, arm64::VS_H));
        emit(arm64::sqxtn_v(0, 2, arm64::VS_S));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PMULHRSW_MM_MMM64: {
        // ((i32)a*(i32)b + 0x4000) >> 15. Exact vs SQRDMULH INT16_MIN corner.
        MMX_LOAD_DST_SRC();
        emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
        emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
        emit(arm64::smull_v(2, 0, 1, arm64::VS_H));
        emit(arm64::movz_w(REG_SCRATCH2, 0x4000));
        emit(arm64::dup_v_gpr(3, REG_SCRATCH2, arm64::VS_S));
        emit(arm64::add_v(2, 2, 3, arm64::VS_S));
        emit(arm64::sshr_v_imm(2, 2, 15, arm64::VS_S));
        emit(arm64::xtn_v(0, 2, arm64::VS_H));
        emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

    case Opcode::PALIGNR_MM_MMM64_IMM8: {
        // CONCAT(DEST, SRC) >> (imm8*8), take low 8. imm8 >= 16 → 0.
        MMX_LOAD_DST_SRC();
        const u8 imm = static_cast<u8>(instr.operands[2].imm);
        if (imm >= 16) {
            emit(arm64::movz_w(REG_SCRATCH0, 0));
        } else {
            emit(arm64::fmov_d_from_x(0, REG_SCRATCH0));
            emit(arm64::fmov_d_from_x(1, REG_SCRATCH1));
            emit(arm64::ins_v_elem(1, 1, 0, 0, arm64::VS_D));
            emit(arm64::eor_v16b(2, 2, 2));
            emit(arm64::ext_v16b(0, 1, 2, imm));
            emit(arm64::fmov_x_from_d(REG_SCRATCH0, 0));
        }
        emit_store_mm(REG_SCRATCH0, op0.reg);
        break;
    }

#undef MMX_NEON_BINOP
#undef MMX_LOAD_DST_SRC

    // Intel SDM Vol. 2 MASKMOVQ: store mm1 bytes to DS:[EDI]/[RDI] where the
    // MSB of the corresponding mm2 byte is 1. EDI/RDI is not incremented.
    // write_mm / emit_store_mm still runs so the dest enters MMX state even
    // when the mask is all zeros. Address-size / segment prefixes fall back
    // to the interpreter (maskmov_ds_edi_address).
    case Opcode::MASKMOVQ_MM_MM: {
        if (!is_mmreg(op0) || !is_mmreg(op1)) return false;
        if (instr.prefixes.address_size) return false;
        if (instr.prefixes.segment != SegReg::Default) return false;

        if (!m_block_is_64bit) emit_update_rip(instr_rip);
        emit_load_mm(REG_SCRATCH0, op0.reg);
        emit_store_mm(REG_SCRATCH0, op0.reg);

        const u8 rdi = static_cast<u8>(RegIndex::RDI);
        for (int lane = 0; lane < 8; ++lane) {
            emit_load_mm(REG_SCRATCH1, op1.reg);
            const u8 msb = static_cast<u8>(lane * 8 + 7);
            const u32 tbz_idx = static_cast<u32>(m_code_buf.size());
            emit(0);
            emit_load_mm(REG_SCRATCH0, op0.reg);
            if (lane != 0) {
                emit(arm64::lsr_x_imm(REG_SCRATCH0, REG_SCRATCH0,
                                      static_cast<u8>(lane * 8)));
            }
            if (m_block_is_64bit)
                emit_load_gpr64(REG_SCRATCH2, rdi);
            else
                emit_load_gpr32(REG_SCRATCH2, rdi);
            if (lane != 0) {
                emit(arm64::add_x_imm(REG_SCRATCH2, REG_SCRATCH2,
                                      static_cast<u32>(lane)));
            }
            emit_guest_store8(REG_SCRATCH0, REG_SCRATCH2);
            const u32 skip = static_cast<u32>(m_code_buf.size());
            m_code_buf[tbz_idx] = arm64::tbz(
                REG_SCRATCH1, msb,
                static_cast<i32>(skip - tbz_idx) * 4);
        }
        break;
    }

    default:
        return false;
    }

    // Every MMX instruction here enters MMX state (tag word all-Valid, TOP 0).
    // Single exit: all the successful cases break out of the switch to reach
    // this point. This MUST precede the is_last tail below -- that tail emits
    // the block epilogue, so anything after it is unreachable code.
    emit_mmx_enter();

    // Shared tail (mirrors compile_64bit): MMX ops never branch, so on the
    // last instruction of the block we commit the loop counter, advance RIP,
    // and return to the dispatcher. Without this the block runs off its end.
    if (is_last) {
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE,
                               jit_iters_offset() >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

} // namespace vitreo::cpu::jit

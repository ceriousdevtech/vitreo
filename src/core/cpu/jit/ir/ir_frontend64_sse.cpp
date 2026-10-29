// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file ir_frontend64_sse.cpp
/// @brief Long-mode SSE register and memory forms (M13a/M13b)
///
/// Split out of ir_frontend.cpp by pure code motion; each function is one
/// self-contained section of `build_long_mode_insn`, which calls them in
/// their original order (ir_frontend64.cpp).

#include "ir_frontend_internal.hpp"

namespace vitreo::cpu::jit::ir {

FamilyVerdict try_long_mode_sse(
    IrBuilder& builder, const DecodedInstruction& insn, u64 next_rip) {
    InsnVerdict verdict{false, DeclineReason::UnsupportedOpcode};
    const auto& px = insn.prefixes;
    // Long-mode SSE, register-register forms (M13a).
    //
    // Two shapes, and between them 21 of Appendix C's identifiers:
    //
    //   128-bit BITWISE  -- XORPS/XORPD/PXOR, ANDPS/ANDPD/PAND, ORPS/ORPD/POR.
    //       The single-precision, double-precision and integer spellings
    //       compute the same 128 bits; they differ only in the exception class
    //       a MISALIGNED memory operand would raise, which a register-register
    //       form cannot reach. So one lowering serves all three.
    //   128-bit COPY     -- MOVAPS/MOVAPD/MOVDQA/MOVDQU/MOVUPS/MOVUPD, both
    //       directions. Aligned and unaligned differ only in that same
    //       alignment check, and between registers there is no address.
    //
    // Register-register only. A 128-bit memory operand additionally has to
    // model the alignment #GP for the aligned spellings, and would put a
    // vector value across a faultable access -- which the backend refuses,
    // because V0-V7 are caller-saved and the fault handler saves no vector
    // state at all.
    {
        // `VITREO_JIT_IR_SSE` bit 3 drops this whole tranche without giving up
        // Tier 1 -- the same escape hatch the 32-bit SSE forms have. When an
        // SSE lowering is suspected of breaking a guest, "which one" is the
        // first question and a game launch is the measurement.
        const bool sse64 = sse_form_enabled(8);
        Op vbitwise = Op::VXor;   // overwritten below; `have_bitwise` gates it
        bool have_bitwise = false;

        switch (insn.opcode) {
        case Opcode::XORPS_XMM_XMMM128:
        case Opcode::XORPD_XMM_XMMM128:
        case Opcode::PXOR_XMM_XMMM128:
            vbitwise = Op::VXor; have_bitwise = true; break;
        case Opcode::ANDPS_XMM_XMMM128:
        case Opcode::ANDPD_XMM_XMMM128:
        case Opcode::PAND_XMM_XMMM128:
            vbitwise = Op::VAnd; have_bitwise = true; break;
        case Opcode::ORPS_XMM_XMMM128:
        case Opcode::ORPD_XMM_XMMM128:
        case Opcode::POR_XMM_XMMM128:
            vbitwise = Op::VOr;  have_bitwise = true; break;
        default: break;
        }
        if (have_bitwise && sse64 && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 2) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            const auto& src = insn.operands[1];
            if (is_xmm(dst) && dst.reg < 16 &&
                (is_xmm(src) ? src.reg < 16 : src.type == OperandType::Mem128)) {
                ValueId c = kInvalidValue;
                if (is_xmm(src)) {
                    c = builder.xmm_read(static_cast<u8>(src.reg));
                } else {
                    // Every SSE packed bitwise form requires a 16-byte aligned
                    // memory operand -- there is no unaligned spelling of them
                    // the way there is for the copies. Measured, not read off
                    // the manual: tools/oracle_probe/sse_align_probe.c.
                    const ValueId address =
                        build_long_mode_ea(builder, src.mem, next_rip,
                                           px.address_size);
                    if (address == kInvalidValue) return verdict;
                    c = builder.load_aligned128(address, kMemFaultState);
                }
                const ValueId a = builder.xmm_read(static_cast<u8>(dst.reg));
                builder.xmm_write(static_cast<u8>(dst.reg),
                                  builder.binary(vbitwise, Type::V128, a, c));
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        // PACKSSWB / PACKSSDW / PACKUSWB: narrow both sources one element
        // size with saturation, destination into the LOW half and source into
        // the high half. `vec_elem` is the SOURCE size, which is what the
        // narrowing instruction takes.
        //
        // PACKUSWB saturates a SIGNED source into an UNSIGNED destination --
        // SQXTUN, not UQXTN. Getting that wrong is invisible for any input
        // whose elements are already in range.
        struct PackForm { Op op; u8 src_elem; };
        PackForm packf{Op::VPackS, 1};
        bool have_pack = false;
        switch (insn.opcode) {
        case Opcode::PACKSSWB_XMM_XMMM128:
            packf = {Op::VPackS, 1}; have_pack = true; break;
        case Opcode::PACKSSDW_XMM_XMMM128:
            packf = {Op::VPackS, 2}; have_pack = true; break;
        case Opcode::PACKUSWB_XMM_XMMM128:
            packf = {Op::VPackU, 1}; have_pack = true; break;
        default: break;
        }
        if (have_pack && sse64 && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 2) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            const auto& src = insn.operands[1];
            if (is_xmm(dst) && dst.reg < 16 &&
                (is_xmm(src) ? src.reg < 16 : src.type == OperandType::Mem128)) {
                ValueId c = kInvalidValue;
                if (is_xmm(src)) {
                    c = builder.xmm_read(static_cast<u8>(src.reg));
                } else {
                    const ValueId address =
                        build_long_mode_ea(builder, src.mem, next_rip,
                                           px.address_size);
                    if (address == kInvalidValue) return verdict;
                    c = builder.load_aligned128(address, kMemFaultState);
                }
                const ValueId a = builder.xmm_read(static_cast<u8>(dst.reg));
                builder.xmm_write(static_cast<u8>(dst.reg),
                                  builder.vec_binary(packf.op, packf.src_elem, a, c));
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        // Element-wise shifts by a compile-time count.
        //
        // x86 does NOT mask the count: a count at or past the element width
        // shifts every bit out, so the result is all zeros for the logical
        // forms and all sign bits for the arithmetic one. ARM64's shift-by-
        // immediate encodings cannot express those counts at all, so the
        // frontend resolves them here rather than handing the emitter a
        // count it would have to truncate.
        struct VShiftForm { Op op; u8 elem; };
        VShiftForm vshift{Op::VShlImm, 1};
        bool have_vshift = false;
        switch (insn.opcode) {
        case Opcode::PSLLW_XMM_IMM8: vshift = {Op::VShlImm, 1}; have_vshift = true; break;
        case Opcode::PSRLW_XMM_IMM8: vshift = {Op::VShrImm, 1}; have_vshift = true; break;
        case Opcode::PSRAW_XMM_IMM8: vshift = {Op::VSarImm, 1}; have_vshift = true; break;
        case Opcode::PSLLD_XMM_IMM8: vshift = {Op::VShlImm, 2}; have_vshift = true; break;
        case Opcode::PSRLD_XMM_IMM8: vshift = {Op::VShrImm, 2}; have_vshift = true; break;
        case Opcode::PSRAD_XMM_IMM8: vshift = {Op::VSarImm, 2}; have_vshift = true; break;
        case Opcode::PSLLQ_XMM_IMM8: vshift = {Op::VShlImm, 3}; have_vshift = true; break;
        case Opcode::PSRLQ_XMM_IMM8: vshift = {Op::VShrImm, 3}; have_vshift = true; break;
        default: break;
        }
        if (have_vshift && sse64 && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 2 &&
            is_immediate_operand(insn.operands[1].type)) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            if (is_xmm(dst) && dst.reg < 16) {
                const u32 bits = 8u << vshift.elem;
                const u64 count = static_cast<u64>(insn.operands[1].imm) & 0xFFull;
                const ValueId a = builder.xmm_read(static_cast<u8>(dst.reg));
                ValueId result = kInvalidValue;
                if (count == 0) {
                    result = a;                       // no bits move
                } else if (count >= bits) {
                    result = vshift.op == Op::VSarImm
                        ? builder.vec_shift_imm(Op::VSarImm, vshift.elem, a,
                                                static_cast<u8>(bits - 1))
                        : builder.vec_binary(Op::VXor, vshift.elem, a, a);
                } else {
                    result = builder.vec_shift_imm(vshift.op, vshift.elem, a,
                                                   static_cast<u8>(count));
                }
                builder.xmm_write(static_cast<u8>(dst.reg), result);
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        // Element-wise packed integer forms. The guest opcode names both the
        // operation and the element width; the width becomes a field on the
        // op rather than eight more opcodes, so the lowering picks a NEON size
        // selector instead of repeating itself four times.
        //
        // PCMPGT* is SIGNED and PMINUB/PMAXUB are UNSIGNED while PMINSW/
        // PMAXSW are signed -- x86 spells those differences in the mnemonic
        // because they are different operations, and so does the IR.
        struct PackedInt { Op op; u8 elem; };
        PackedInt packed{Op::VAdd, 2};
        bool have_packed = false;
        const auto set_packed = [&](Op op, u8 elem) {
            packed = {op, elem};
            have_packed = true;
        };
        switch (insn.opcode) {
        case Opcode::PADDB_XMM_XMMM128: set_packed(Op::VAdd, 0); break;
        case Opcode::PADDW_XMM_XMMM128: set_packed(Op::VAdd, 1); break;
        case Opcode::PADDD_XMM_XMMM128: set_packed(Op::VAdd, 2); break;
        case Opcode::PADDQ_XMM_XMMM128: set_packed(Op::VAdd, 3); break;
        case Opcode::PSUBB_XMM_XMMM128: set_packed(Op::VSub, 0); break;
        case Opcode::PSUBW_XMM_XMMM128: set_packed(Op::VSub, 1); break;
        case Opcode::PSUBD_XMM_XMMM128: set_packed(Op::VSub, 2); break;
        case Opcode::PSUBQ_XMM_XMMM128: set_packed(Op::VSub, 3); break;
        case Opcode::PCMPEQB_XMM_XMMM128: set_packed(Op::VCmpEq, 0); break;
        case Opcode::PCMPEQW_XMM_XMMM128: set_packed(Op::VCmpEq, 1); break;
        case Opcode::PCMPEQD_XMM_XMMM128: set_packed(Op::VCmpEq, 2); break;
        case Opcode::PCMPGTB_XMM_XMMM128: set_packed(Op::VCmpGt, 0); break;
        case Opcode::PCMPGTW_XMM_XMMM128: set_packed(Op::VCmpGt, 1); break;
        case Opcode::PCMPGTD_XMM_XMMM128: set_packed(Op::VCmpGt, 2); break;
        case Opcode::PMINUB_XMM_XMMM128: set_packed(Op::VMinU, 0); break;
        case Opcode::PMAXUB_XMM_XMMM128: set_packed(Op::VMaxU, 0); break;
        case Opcode::PMINSW_XMM_XMMM128: set_packed(Op::VMinS, 1); break;
        case Opcode::PMAXSW_XMM_XMMM128: set_packed(Op::VMaxS, 1); break;
        case Opcode::PMULLW_XMM_XMMM128: set_packed(Op::VMulLo, 1); break;
        case Opcode::PAVGB_XMM_XMMM128:  set_packed(Op::VAvgU, 0); break;
        case Opcode::PAVGW_XMM_XMMM128:  set_packed(Op::VAvgU, 1); break;
        // SSE4.1 widened the min/max family to the signed byte, the unsigned
        // word and both dword spellings. ARM64 has had all of them since the
        // base instruction set, so these are the same SMIN/SMAX/UMIN/UMAX the
        // SSE2 pair already lower to, at a different element size -- no new IR
        // op, only a new width. (There is no 64-bit spelling to add: x86 has
        // none outside AVX512, and ARM64 has no 2D form either.)
        case Opcode::PMINSB_XMM_XMMM128: set_packed(Op::VMinS, 0); break;
        case Opcode::PMINSD_XMM_XMMM128: set_packed(Op::VMinS, 2); break;
        case Opcode::PMAXSB_XMM_XMMM128: set_packed(Op::VMaxS, 0); break;
        case Opcode::PMAXSD_XMM_XMMM128: set_packed(Op::VMaxS, 2); break;
        case Opcode::PMINUW_XMM_XMMM128: set_packed(Op::VMinU, 1); break;
        case Opcode::PMINUD_XMM_XMMM128: set_packed(Op::VMinU, 2); break;
        case Opcode::PMAXUW_XMM_XMMM128: set_packed(Op::VMaxU, 1); break;
        case Opcode::PMAXUD_XMM_XMMM128: set_packed(Op::VMaxU, 2); break;
        // PMULLD is PMULLW one element size up. PMULLQ does not exist here for
        // the same reason: ARM64 MUL has no 2D form.
        case Opcode::PMULLD_XMM_XMMM128: set_packed(Op::VMulLo, 2); break;
        // SSE4.1/4.2 added the 64-bit element compares. CMEQ and CMGT, unlike
        // the min/max family, DO have a 2D form.
        case Opcode::PCMPEQQ_XMM_XMMM128: set_packed(Op::VCmpEq, 3); break;
        case Opcode::PCMPGTQ_XMM_XMMM128: set_packed(Op::VCmpGt, 3); break;
        // Saturating add and subtract. x86 clamps each element to its own
        // signed or unsigned limits; ARM64 SQADD/UQADD/SQSUB/UQSUB clamp
        // identically, including the asymmetric signed minimum.
        case Opcode::PADDSB_XMM_XMMM128:  set_packed(Op::VQAddS, 0); break;
        case Opcode::PADDSW_XMM_XMMM128:  set_packed(Op::VQAddS, 1); break;
        case Opcode::PADDUSB_XMM_XMMM128: set_packed(Op::VQAddU, 0); break;
        case Opcode::PADDUSW_XMM_XMMM128: set_packed(Op::VQAddU, 1); break;
        case Opcode::PSUBSB_XMM_XMMM128:  set_packed(Op::VQSubS, 0); break;
        case Opcode::PSUBSW_XMM_XMMM128:  set_packed(Op::VQSubS, 1); break;
        case Opcode::PSUBUSB_XMM_XMMM128: set_packed(Op::VQSubU, 0); break;
        case Opcode::PSUBUSW_XMM_XMMM128: set_packed(Op::VQSubU, 1); break;
        // PUNPCK*: x86 interleaves the DESTINATION's elements with the
        // source's, destination first -- which is exactly ARM64 ZIP1/ZIP2's
        // operand order, so no swap is needed.
        case Opcode::PUNPCKLBW_XMM_XMMM128: set_packed(Op::VZipLo, 0); break;
        case Opcode::PUNPCKLWD_XMM_XMMM128: set_packed(Op::VZipLo, 1); break;
        case Opcode::PUNPCKLDQ_XMM_XMMM128: set_packed(Op::VZipLo, 2); break;
        case Opcode::PUNPCKLQDQ_XMM_XMMM128: set_packed(Op::VZipLo, 3); break;
        case Opcode::PUNPCKHBW_XMM_XMMM128: set_packed(Op::VZipHi, 0); break;
        case Opcode::PUNPCKHWD_XMM_XMMM128: set_packed(Op::VZipHi, 1); break;
        case Opcode::PUNPCKHDQ_XMM_XMMM128: set_packed(Op::VZipHi, 2); break;
        case Opcode::PUNPCKHQDQ_XMM_XMMM128: set_packed(Op::VZipHi, 3); break;
        default: break;
        }
        if (have_packed && sse64 && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 2) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            const auto& src = insn.operands[1];
            if (is_xmm(dst) && dst.reg < 16 &&
                (is_xmm(src) ? src.reg < 16 : src.type == OperandType::Mem128)) {
                ValueId c = kInvalidValue;
                if (is_xmm(src)) {
                    c = builder.xmm_read(static_cast<u8>(src.reg));
                } else {
                    const ValueId address =
                        build_long_mode_ea(builder, src.mem, next_rip,
                                           px.address_size);
                    if (address == kInvalidValue) return verdict;
                    c = builder.load_aligned128(address, kMemFaultState);
                }
                const ValueId a = builder.xmm_read(static_cast<u8>(dst.reg));
                builder.xmm_write(static_cast<u8>(dst.reg),
                                  builder.vec_binary(packed.op, packed.elem, a, c));
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        // PABSB/PABSW/PABSD (SSSE3). Unary: unlike every form above, the
        // destination is not also a source, so it is read only to be written.
        // The one case worth stating is the most negative element -- x86
        // defines PABS on it as itself (0x80 stays 0x80), and ARM64 ABS wraps
        // to the same value rather than saturating, so no fixup is needed.
        u8 abs_elem = 0;
        bool have_abs = false;
        switch (insn.opcode) {
        case Opcode::PABSB_XMM_XMMM128: abs_elem = 0; have_abs = true; break;
        case Opcode::PABSW_XMM_XMMM128: abs_elem = 1; have_abs = true; break;
        case Opcode::PABSD_XMM_XMMM128: abs_elem = 2; have_abs = true; break;
        default: break;
        }
        if (have_abs && sse64 && !px.lock && !px.rep && !px.repne &&
            !px.vex && !px.evex && insn.operand_count >= 2) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            const auto& src = insn.operands[1];
            if (is_xmm(dst) && dst.reg < 16 &&
                (is_xmm(src) ? src.reg < 16 : src.type == OperandType::Mem128)) {
                ValueId c = kInvalidValue;
                if (is_xmm(src)) {
                    c = builder.xmm_read(static_cast<u8>(src.reg));
                } else {
                    const ValueId address =
                        build_long_mode_ea(builder, src.mem, next_rip,
                                           px.address_size);
                    if (address == kInvalidValue) return verdict;
                    c = builder.load_aligned128(address, kMemFaultState);
                }
                builder.xmm_write(static_cast<u8>(dst.reg),
                                  builder.vec_unary(Op::VAbs, abs_elem, c));
                return {true, DeclineReason::None};
            }
            return verdict;
        }

        bool vcopy = false;
        bool aligned16 = false;
        // F3 is a MANDATORY prefix on MOVDQU, not a repeat: the decoder has
        // already folded it into a distinct enumerator, so `px.rep` here is a
        // decoding artifact with no semantics of its own. Exempted for exactly
        // those two opcodes, at the one place that knows which they are --
        // rejecting it turned every MOVDQU into `unsupported-prefix`.
        bool mandatory_rep = false;
        switch (insn.opcode) {
        case Opcode::MOVDQU_XMM_XMMM128: case Opcode::MOVDQU_XMMM128_XMM:
            vcopy = true; mandatory_rep = true; break;
        case Opcode::MOVAPS_XMM_XMMM128: case Opcode::MOVAPS_XMMM128_XMM:
        case Opcode::MOVAPD_XMM_XMMM128: case Opcode::MOVAPD_XMMM128_XMM:
        case Opcode::MOVDQA_XMM_XMMM128: case Opcode::MOVDQA_XMMM128_XMM:
            vcopy = true; aligned16 = true; break;
        case Opcode::MOVUPS_XMM_XMMM128: case Opcode::MOVUPS_XMMM128_XMM:
        case Opcode::MOVUPD_XMM_XMMM128: case Opcode::MOVUPD_XMMM128_XMM:
            vcopy = true; break;
        default: break;
        }
        if (vcopy && sse64 && !px.lock && (!px.rep || mandatory_rep) &&
            !px.repne && !px.vex && !px.evex && insn.operand_count >= 2) {
            verdict.reason = DeclineReason::UnsupportedOperand;
            const auto& dst = insn.operands[0];
            const auto& src = insn.operands[1];
            if (is_xmm(dst) && is_xmm(src) && dst.reg < 16 && src.reg < 16) {
                builder.xmm_write(static_cast<u8>(dst.reg),
                                  builder.xmm_read(static_cast<u8>(src.reg)));
                return {true, DeclineReason::None};
            }
            // MEMORY forms (M13b). The vector is PRODUCED by the load or
            // CONSUMED by the store, never staged across an access, so the
            // backend's rule that a V128 value must not be live across a
            // may-fault op holds without relaxing it -- and the access itself
            // stays one LDR Q / STR Q at the site.
            //
            // The aligned spellings raise #GP off a multiple of 16 (Intel SDM
            // Vol. 2 MOVAPS/MOVAPD/MOVDQA; `require_align16` in the
            // reference). The unaligned spellings of the same copy do not, and
            // that is the only thing separating them.
            if (is_xmm(dst) && src.type == OperandType::Mem128 && dst.reg < 16) {
                const ValueId address =
                    build_long_mode_ea(builder, src.mem, next_rip, px.address_size);
                if (address == kInvalidValue) return verdict;
                builder.xmm_write(
                    static_cast<u8>(dst.reg),
                    aligned16 ? builder.load_aligned128(address, kMemFaultState)
                              : builder.load(Type::V128, address, kMemFaultState));
                return {true, DeclineReason::None};
            }
            if (dst.type == OperandType::Mem128 && is_xmm(src) && src.reg < 16) {
                const ValueId address =
                    build_long_mode_ea(builder, dst.mem, next_rip, px.address_size);
                if (address == kInvalidValue) return verdict;
                const ValueId value = builder.xmm_read(static_cast<u8>(src.reg));
                if (aligned16)
                    builder.store_aligned128(address, value, kMemFaultState);
                else
                    builder.store(Type::V128, address, value, kMemFaultState);
                return {true, DeclineReason::None};
            }
            return verdict;
        }
    }
    return std::nullopt;
}

}  // namespace vitreo::cpu::jit::ir

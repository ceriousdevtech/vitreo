// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: the carry-chain ALU. ADC/SBB r/m64
// and ADC/SBB r/m32.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_adc_sbb64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    // Publish the six arithmetic flags after a native ADCS/SBCS X0,X4,X1.
    // ADCS leaves x86 CF/ZF/SF/OF directly in ARM C/Z/N/V; SBCS leaves the
    // same except that ARM C is NOT borrow, so x86 CF is `carry_cond` = CC.
    // PF and AF have no NZCV representation, so compute those two from the
    // result while the other four are copied with CSET. Every instruction
    // below is non-flag-setting, which keeps NZCV live for an adjacent
    // ADC/SBB/Jcc. AF is bit 4 of dst^src^result for both (SDM ADC/SBB).
    // Inputs: X4=original destination, X1=source, X0=result.
    auto emit_adc64_rflags = [&](u8 carry_cond = arm64::CS) {
        const u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit_mov_imm32(REG_SCRATCH2, 0x8D5u);
        emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));

        emit(arm64::cset_w(REG_SCRATCH2, carry_cond));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::cset_w(REG_SCRATCH2, arm64::EQ));
        emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 6));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::cset_w(REG_SCRATCH2, arm64::MI));
        emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 7));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::cset_w(REG_SCRATCH2, arm64::VS));
        emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 11));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));

        // AF = bit 4 of original_dst XOR source XOR result.
        emit(arm64::eor_x(REG_SCRATCH2, REG_SCRATCH4, REG_SCRATCH1));
        emit(arm64::eor_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH0));
        emit(arm64::ubfx_x(REG_SCRATCH2, REG_SCRATCH2, 4, 1));
        emit(arm64::lsl_x_imm(REG_SCRATCH2, REG_SCRATCH2, 4));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));

        // PF = even parity of result[7:0], using the same nibble lookup as
        // the other eager flag publishers.
        emit(arm64::mov_w(REG_SCRATCH2, REG_SCRATCH0));
        emit(arm64::lsr_w_imm(REG_SCRATCH4, REG_SCRATCH2, 4));
        emit(arm64::eor_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit_mov_imm32(REG_SCRATCH4, 0xFu);
        emit(arm64::and_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH4));
        emit_mov_imm32(REG_SCRATCH4, 0x9669u);
        emit(arm64::lsrv_w(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));
        emit(arm64::ubfx_w(REG_SCRATCH4, REG_SCRATCH4, 0, 1));
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_SCRATCH4, 2));
        emit(arm64::orr_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4));

        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off >> 3));
        emit(arm64::strb_uoff(31, REG_CPU_STATE, flag_op_offset()));
        emit_clear_cf_overlay();
    };

    switch (instr.opcode) {

    // ADC r/m64 forms. Keep the carry chain in generated AArch64 instead of
    // crossing a C++ helper boundary for every limb.
    case Opcode::ADC_RM64_IMM8:
    case Opcode::ADC_RM64_IMM32:
    case Opcode::ADC_RAX_IMM32: {
        emit_lazy_cf_to_call_tmp();
        if (is_mem(op0)) {
            emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH3);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        }
        emit_mov_imm64(REG_SCRATCH1, sx_imm64(op1));
        emit(arm64::mov_x(REG_SCRATCH4, REG_SCRATCH0));
        emit(arm64::cmp_w_imm(REG_CALL_TMP, 1));
        emit(arm64::adcs_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_adc64_rflags();
        if (is_mem(op0)) {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
            emit_guest_store64(REG_CALL_TMP, REG_SCRATCH3);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            m_flags.nzcv = NzcvState::Add;
        }
        break;
    }

    // SBB r/m32 forms — Intel SDM Vol. 2, "SBB". Capture the incoming lazy
    // CF before any checked memory call, subtract SRC+CF modulo 32 bits, and
    // retain SRC plus carry-in in the established Sbb32 lazy-flag layout.
    // 32-bit ADC in a 64-bit block. The 64-bit forms (ADC_RM64_*) and the
    // 32-bit SBB forms below were both natively lowered, but the 32-bit ADC
    // forms were not, so every `adc r/m32, imm8` in a 64-bit guest fell
    // through to the interpreter delegate: measured at 128,000,000 delegated
    // executions on an ADC/SBB carry chain, 99.95% of all instructions
    // executed there. Mirrors the SBB case; only the two ALU ops and the
    // recorded FlagOp differ. flag_src keeps the Adc32/Sbb32 encoding
    // (cf_in << 32) | src documented on CpuState::FlagOp.
    case Opcode::ADC_EAX_IMM32:
    case Opcode::ADC_RM32_IMM32:
    case Opcode::ADC_RM32_IMM8:
    case Opcode::ADC_RM32_R32:
    case Opcode::ADC_R32_RM32: {
        const bool dst_is_mem = is_mem(op0);
        const bool src_is_mem = is_mem(op1);

        // Chain fast path: when the immediately preceding instruction left
        // Add/Sub-kind NZCV live, the carry is ALREADY in PSTATE with a known
        // sense (Add: C == CF, Sub: C == !CF — the same invariant the lazy-CF
        // fast paths rely on), so the whole capture-dispatch is one CSET, the
        // ALU op is a single flag-setting ADCS, and NZCV comes out
        // Add-kind live again — which is what lets an
        // `add; adc; adc; ...` chain run at a handful of instructions per
        // limb instead of re-deriving the carry through the FlagOp dispatch
        // every time. A sense mismatch costs exactly one CFINV (FEAT_FlagM).
        //
        // Overlay safety is by construction: CLC/STC/CMC never publish an
        // NzcvState, so m_flags.incoming_nzcv != None proves the previous
        // instruction was the flag producer itself (or a preserving MOV), and
        // that producer's record already retired any overlay.
        {
            const u8 dst_pin = (!dst_is_mem && is_register_operand(op0.type))
                                   ? pinned_gpr32(op0.reg)
                                   : u8{0xFF};
            const bool in_add = m_flags.incoming_nzcv == NzcvState::Add;
            const bool in_sub = m_flags.incoming_nzcv == NzcvState::Sub;
            if (dst_pin != 0xFF && !src_is_mem && (in_add || in_sub)) {
                // Source value first, from its own pin or as an immediate —
                // captured before the ALU op so the lazy record sees the
                // pre-op source even when src == dst.
                u8 src_w = REG_SCRATCH1;
                if (is_imm(op1)) {
                    const u32 imm = static_cast<u32>(static_cast<i32>(op1.imm));
                    if (imm == 0) {
                        src_w = 31;  // WZR: `adc r, 0` is the common limb
                    } else {
                        emit_mov_imm32(REG_SCRATCH1, imm);
                    }
                } else {
                    const u8 sp = pinned_gpr32(op1.reg);
                    if (sp != 0xFF && op1.reg != op0.reg) {
                        src_w = sp;
                    } else {
                        emit_load_gpr32(REG_SCRATCH1, op1.reg);
                    }
                }
                // CF (x86 sense) for the lazy record: CS under Add-kind,
                // CC under Sub-kind.
                emit(arm64::cset_w(17, in_add ? arm64::CS : arm64::CC));
                // // ADCS consumes ARM C as carry-in: equals CF under
                // Add-kind; needs one inversion under Sub-kind.
                if (in_sub) {
                    emit(arm64::cfinv());
                }
                emit(arm64::adcs_w(dst_pin, dst_pin, src_w));
                if (!m_omit_lazy_current) {
                    emit(arm64::lsl_x_imm(REG_SCRATCH4, 17, 32));
                    if (src_w != 31) {
                        // flag_src = CF << 32 | the 32-bit source. A pinned
                        // source (X23-X28, or a tier-2 pool host) holds the
                        // whole 64-bit register: its upper half must not
                        // reach the CF field, so the record takes W1, the
                        // zero-extended operand.
                        if (src_w != REG_SCRATCH1) {
                            emit(arm64::mov_w(REG_SCRATCH1, src_w));
                        }
                        emit(arm64::orr_x(REG_SCRATCH4, REG_SCRATCH4,
                                          REG_SCRATCH1));
                    }
                    emit(arm64::str_x_uoff(REG_SCRATCH4, REG_CPU_STATE,
                                           flag_src_offset() >> 3));
                    emit(arm64::str_x_uoff(dst_pin, REG_CPU_STATE,
                                           flag_dst_offset() >> 3));
                    emit_mov_imm32(REG_SCRATCH3,
                                   static_cast<u32>(CpuState::FlagOp::Adc32));
                    emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                          flag_op_offset()));
                    emit_clear_cf_overlay();
                }
                m_flags.nzcv = NzcvState::Add;
                break;
            }
        }

        // Was an unconditional CAPTURE_CF helper call. The shared emitter
        // takes live NZCV or a known carry source when it can and only calls
        // the helper otherwise -- measured at ~136M helper calls on an ADC/SBB
        // carry chain before this, because a 64-bit block never got the fast
        // paths the 32-bit path in ops/arithmetic.cpp already had.
        emit_lazy_cf_to_call_tmp();

        // The only memory-source encoding has a register destination. Load
        // that source first so the checked helper cannot clobber the LHS.
        if (src_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32_64(REG_SCRATCH1, REG_SCRATCH4);
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            if (dst_is_mem) {
                emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
                emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH4);
            } else {
                emit_load_gpr32(REG_SCRATCH0, op0.reg);
            }
            if (is_imm(op1)) {
                emit_mov_imm32(REG_SCRATCH1,
                               static_cast<u32>(static_cast<i32>(op1.imm)));
            } else {
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
            }
        }

        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::add_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_CALL_TMP, 32));
        emit(arm64::orr_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1));
        emit(arm64::str_x_uoff(REG_SCRATCH4, REG_CPU_STATE,
                               flag_src_offset() >> 3));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                               flag_dst_offset() >> 3));
        emit_mov_imm32(REG_SCRATCH3,
                       static_cast<u32>(CpuState::FlagOp::Adc32));
        emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                              flag_op_offset()));
        emit_clear_cf_overlay();

        if (dst_is_mem) {
            emit(arm64::mov_w(17, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_store32_64(17, REG_SCRATCH4);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::SBB_EAX_IMM32:
    case Opcode::SBB_RM32_IMM32:
    case Opcode::SBB_RM32_IMM8:
    case Opcode::SBB_RM32_R32:
    case Opcode::SBB_R32_RM32: {
        const bool dst_is_mem = is_mem(op0);
        const bool src_is_mem = is_mem(op1);

        // Chain fast path: when the immediately preceding instruction left
        // Add/Sub-kind NZCV live, the carry is ALREADY in PSTATE with a known
        // sense (Add: C == CF, Sub: C == !CF — the same invariant the lazy-CF
        // fast paths rely on), so the whole capture-dispatch is one CSET, the
        // ALU op is a single flag-setting SBCS, and NZCV comes out
        // Sub-kind live again — which is what lets an
        // `add; adc; adc; ...` chain run at a handful of instructions per
        // limb instead of re-deriving the carry through the FlagOp dispatch
        // every time. A sense mismatch costs exactly one CFINV (FEAT_FlagM).
        //
        // Overlay safety is by construction: CLC/STC/CMC never publish an
        // NzcvState, so m_flags.incoming_nzcv != None proves the previous
        // instruction was the flag producer itself (or a preserving MOV), and
        // that producer's record already retired any overlay.
        {
            const u8 dst_pin = (!dst_is_mem && is_register_operand(op0.type))
                                   ? pinned_gpr32(op0.reg)
                                   : u8{0xFF};
            const bool in_add = m_flags.incoming_nzcv == NzcvState::Add;
            const bool in_sub = m_flags.incoming_nzcv == NzcvState::Sub;
            if (dst_pin != 0xFF && !src_is_mem && (in_add || in_sub)) {
                // Source value first, from its own pin or as an immediate —
                // captured before the ALU op so the lazy record sees the
                // pre-op source even when src == dst.
                u8 src_w = REG_SCRATCH1;
                if (is_imm(op1)) {
                    const u32 imm = static_cast<u32>(static_cast<i32>(op1.imm));
                    if (imm == 0) {
                        src_w = 31;  // WZR: `adc r, 0` is the common limb
                    } else {
                        emit_mov_imm32(REG_SCRATCH1, imm);
                    }
                } else {
                    const u8 sp = pinned_gpr32(op1.reg);
                    if (sp != 0xFF && op1.reg != op0.reg) {
                        src_w = sp;
                    } else {
                        emit_load_gpr32(REG_SCRATCH1, op1.reg);
                    }
                }
                // CF (x86 sense) for the lazy record: CS under Add-kind,
                // CC under Sub-kind.
                emit(arm64::cset_w(17, in_add ? arm64::CS : arm64::CC));
                // // SBCS consumes ARM C as NOT-borrow: already right
                // under Sub-kind; needs one inversion under Add-kind.
                if (in_add) {
                    emit(arm64::cfinv());
                }
                emit(arm64::sbcs_w(dst_pin, dst_pin, src_w));
                if (!m_omit_lazy_current) {
                    emit(arm64::lsl_x_imm(REG_SCRATCH4, 17, 32));
                    if (src_w != 31) {
                        // flag_src = CF << 32 | the 32-bit source. A pinned
                        // source (X23-X28, or a tier-2 pool host) holds the
                        // whole 64-bit register: its upper half must not
                        // reach the CF field, so the record takes W1, the
                        // zero-extended operand.
                        if (src_w != REG_SCRATCH1) {
                            emit(arm64::mov_w(REG_SCRATCH1, src_w));
                        }
                        emit(arm64::orr_x(REG_SCRATCH4, REG_SCRATCH4,
                                          REG_SCRATCH1));
                    }
                    emit(arm64::str_x_uoff(REG_SCRATCH4, REG_CPU_STATE,
                                           flag_src_offset() >> 3));
                    emit(arm64::str_x_uoff(dst_pin, REG_CPU_STATE,
                                           flag_dst_offset() >> 3));
                    emit_mov_imm32(REG_SCRATCH3,
                                   static_cast<u32>(CpuState::FlagOp::Sbb32));
                    emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                          flag_op_offset()));
                    emit_clear_cf_overlay();
                }
                m_flags.nzcv = NzcvState::Sub;
                break;
            }
        }

        // Was an unconditional CAPTURE_CF helper call. The shared emitter
        // takes live NZCV or a known carry source when it can and only calls
        // the helper otherwise -- measured at ~136M helper calls on an ADC/SBB
        // carry chain before this, because a 64-bit block never got the fast
        // paths the 32-bit path in ops/arithmetic.cpp already had.
        emit_lazy_cf_to_call_tmp();

        // The only memory-source encoding has a register destination. Load
        // that source first so the checked helper cannot clobber the LHS.
        if (src_is_mem) {
            emit_compute_ea64(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            emit_guest_load32_64(REG_SCRATCH1, REG_SCRATCH4);
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            if (dst_is_mem) {
                emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
                emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH4);
            } else {
                emit_load_gpr32(REG_SCRATCH0, op0.reg);
            }
            if (is_imm(op1)) {
                emit_mov_imm32(REG_SCRATCH1,
                               static_cast<u32>(static_cast<i32>(op1.imm)));
            } else {
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
            }
        }

        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit(arm64::sub_w(REG_SCRATCH0, REG_SCRATCH0, REG_CALL_TMP));
        emit(arm64::lsl_x_imm(REG_SCRATCH4, REG_CALL_TMP, 32));
        emit(arm64::orr_x(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH1));
        emit(arm64::str_x_uoff(REG_SCRATCH4, REG_CPU_STATE,
                               flag_src_offset() >> 3));
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE,
                               flag_dst_offset() >> 3));
        emit_mov_imm32(REG_SCRATCH3,
                       static_cast<u32>(CpuState::FlagOp::Sbb32));
        emit(arm64::strb_uoff(REG_SCRATCH3, REG_CPU_STATE,
                              flag_op_offset()));
        emit_clear_cf_overlay();

        if (dst_is_mem) {
            emit(arm64::mov_w(17, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_store32_64(17, REG_SCRATCH4);
        } else {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    // ADC r/m64, r64 / ADC r64, r/m64 — native carry-chain lowering.
    case Opcode::ADC_RM64_R64:
    case Opcode::ADC_R64_RM64: {
        emit_lazy_cf_to_call_tmp();
        if (is_mem(op1)) {
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op1);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH3);
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        } else if (is_mem(op0)) {
            emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH3);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
        } else {
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
            emit_load_gpr64(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH4, REG_SCRATCH0));
        emit(arm64::cmp_w_imm(REG_CALL_TMP, 1));
        emit(arm64::adcs_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_adc64_rflags();
        if (is_mem(op0)) {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
            emit_guest_store64(REG_CALL_TMP, REG_SCRATCH3);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            m_flags.nzcv = NzcvState::Add;
        }
        break;
    }

    // SBB r/m64 forms — native borrow-chain lowering, the mirror of ADC.
    // Intel SDM Vol. 2B SBB: DEST := DEST - (SRC + CF). SBCS computes
    // Rn - Rm - NOT(C), so C is loaded with NOT CF first (0 - CF borrows
    // exactly when CF is set) and x86 CF comes back as NOT C.
    case Opcode::SBB_RM64_R64:
    case Opcode::SBB_R64_RM64:
    case Opcode::SBB_RM64_IMM32:
    case Opcode::SBB_RM64_IMM8:
    case Opcode::SBB_RAX_IMM32: {
        emit_lazy_cf_to_call_tmp();
        if (is_mem(op1)) {
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op1);
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH3);
            emit_load_gpr64(REG_SCRATCH0, op0.reg);
        } else {
            if (is_mem(op0)) {
                emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
                emit_guest_load64(REG_SCRATCH0, REG_SCRATCH3);
            } else {
                emit_load_gpr64(REG_SCRATCH0, op0.reg);
            }
            if (is_imm(op1))
                emit_mov_imm64(REG_SCRATCH1, sx_imm64(op1));
            else
                emit_load_gpr64(REG_SCRATCH1, op1.reg);
        }
        emit(arm64::mov_x(REG_SCRATCH4, REG_SCRATCH0));
        emit(arm64::subs_w(31, 31, REG_CALL_TMP));
        emit(arm64::sbcs_x(REG_SCRATCH0, REG_SCRATCH0, REG_SCRATCH1));
        emit_adc64_rflags(arm64::CC);
        if (is_mem(op0)) {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH3, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH3, instr, op0);
            emit_guest_store64(REG_CALL_TMP, REG_SCRATCH3);
        } else {
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
            m_flags.nzcv = NzcvState::Sub;
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit

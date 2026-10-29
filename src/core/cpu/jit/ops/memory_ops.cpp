// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — the compile_memory_ops and compile_locked dispatchers, with
// the ADX, MOVBE, XLAT and LOCK BT/BTS/BTR/BTC arms. Their opcode families
// live in ops/memory_ops_{xchg,cond,bt,bitscan,bmi}.cpp and
// ops/locked_{rmw32,rmw64,narrow}.cpp; the native LOCK RMW emitter is in
// ops/native_lock.cpp.

#include "../jit_helpers.hpp"
#include "core/cpu/lock_prefix.hpp"

#include <atomic>
#include <optional>

namespace vitreo::cpu::jit {

namespace {
std::atomic<u32> g_unsupported_lock_diag_logs{0};
}

bool JitCompiler::compile_memory_ops(const DecodedInstruction& instr,
                                      u64 instr_rip, u64 next_rip,
                                      bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];

    switch (instr.opcode) {

    // ---- ADX: ADCX / ADOX ------------------------------------------------
    //
    // Measured on real x86 silicon (Windows; Rosetta #UDs both, so it cannot
    // adjudicate them): ADCX writes ONLY CF and ADOX writes ONLY OF. SF, ZF,
    // AF, PF and the other of CF/OF are preserved -- including ZF when the
    // result is zero. That is the cf_overlay / of_overlay contract exactly, so
    // this lowering writes one overlay and deliberately leaves any pending
    // FlagOp installed to keep owning the flags it preserves. Retiring the
    // FlagOp here would silently destroy them.
    case Opcode::ADCX_R32_RM32:
    case Opcode::ADCX_R64_RM64:
    case Opcode::ADOX_R32_RM32:
    case Opcode::ADOX_R64_RM64: {
        // Diagnostic: VITREO_JIT_NO_NATIVE_ADX=1 hands ADCX/ADOX to the
        // interpreter delegate instead of lowering them natively. Separates a
        // wrong JIT lowering from wrong shared semantics without needing real
        // silicon to adjudicate.
        static const bool no_native_adx =
            std::getenv("VITREO_JIT_NO_NATIVE_ADX") != nullptr;
        if (no_native_adx) return false;
        const bool w64 = instr.opcode == Opcode::ADCX_R64_RM64 ||
                         instr.opcode == Opcode::ADOX_R64_RM64;
        const bool is_of = instr.opcode == Opcode::ADOX_R32_RM32 ||
                           instr.opcode == Opcode::ADOX_R64_RM64;

        // Carry-in must land in REG_CALL_TMP (X21, callee-saved) *before* the
        // operand loads: a non-linear load can call a helper, and only a
        // callee-saved register survives that. This is the same ordering the
        // ADC lowering uses.
        if (is_of) {
            // There is no lazy_of() accessor and OF needs the full
            // computation for most FlagOps, so settle and read RFLAGS bit 11.
            emit(arm64::mov_x(0, REG_CPU_STATE));
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                                   JIT_CTX_OFF_MATERIALIZE >> 3));
            emit_helper_blr();
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE,
                                   cpu_rflags_offset() / 8));
            emit(arm64::lsr_x_imm(REG_CALL_TMP, REG_SCRATCH3, 11));
            emit(arm64::movz_w(REG_SCRATCH4, 1));
            emit(arm64::and_w(REG_CALL_TMP, REG_CALL_TMP, REG_SCRATCH4));
        } else {
            emit_lazy_cf_to_call_tmp();
        }

        if (is_register_operand(op1.type)) {
            if (w64) emit_load_gpr64(REG_SCRATCH1, op1.reg);
            else emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else if (m_block_is_64bit) {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            if (w64) emit_guest_load64(REG_SCRATCH1, REG_SCRATCH2);
            else emit_guest_load32_64(REG_SCRATCH1, REG_SCRATCH2);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            emit_guest_load32(REG_SCRATCH1, REG_SCRATCH2);
        }
        if (w64) emit_load_gpr64(REG_SCRATCH0, op0.reg);
        else emit_load_gpr32(REG_SCRATCH0, op0.reg);

        if (!w64) {
            // Zero-extend both halves and add in 64 bits; bit 32 is the
            // carry-out. MOV Wd, Wn zero-extends into the X register.
            emit(arm64::mov_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::mov_w(REG_SCRATCH1, REG_SCRATCH1));
            emit(arm64::add_x(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::add_x(REG_SCRATCH3, REG_SCRATCH3, REG_CALL_TMP));
            emit(arm64::lsr_x_imm(REG_SCRATCH4, REG_SCRATCH3, 32));
            emit_store_gpr32(REG_SCRATCH3, op0.reg);
        } else {
            // Two adds, either of which can carry; they cannot both carry in a
            // way that loses information, so OR the two carry-outs.
            emit(arm64::adds_x(REG_SCRATCH3, REG_SCRATCH0, REG_SCRATCH1));
            emit(arm64::cset_w(REG_SCRATCH4, arm64::CS));
            emit(arm64::adds_x(REG_SCRATCH3, REG_SCRATCH3, REG_CALL_TMP));
            emit(arm64::cset_w(REG_SCRATCH2, arm64::CS));
            emit(arm64::orr_w(REG_SCRATCH4, REG_SCRATCH4, REG_SCRATCH2));
            emit_store_gpr64(REG_SCRATCH3, op0.reg);
        }

        // Publish the one flag this instruction owns, in RFLAGS and as an
        // overlay (1 = forced 0, 2 = forced 1) so a pending FlagOp keeps
        // answering for everything else.
        const u32 rf_off = cpu_rflags_offset();
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit_mov_imm32(REG_SCRATCH2, is_of ? 0x800u : 0x1u);
        emit(arm64::bic_x(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH2));
        emit(arm64::orr_x_lsl(REG_SCRATCH3, REG_SCRATCH3, REG_SCRATCH4,
                              is_of ? 11 : 0));
        emit(arm64::str_x_uoff(REG_SCRATCH3, REG_CPU_STATE, rf_off / 8));
        emit(arm64::add_w_imm(REG_SCRATCH2, REG_SCRATCH4, 1));
        emit(arm64::strb_uoff(REG_SCRATCH2, REG_CPU_STATE,
                              is_of ? of_overlay_offset() : cf_overlay_offset()));
        break;
    }

    // ---- MOVBE (byte-swapped load/store) ---------------------------------
    //
    // Intel SDM Vol. 2 MOVBE: move data after swapping byte order. No flags.
    // ARM64 REV is the whole operation; the 16-bit form has no REV16 encoder
    // here, so a 32-bit REV puts the swapped halfword in the high half and a
    // logical shift brings it back down.
    case Opcode::MOVBE_R16_M16:
    case Opcode::MOVBE_R32_M32:
    case Opcode::MOVBE_R64_M64: {
        const bool w64 = instr.opcode == Opcode::MOVBE_R64_M64;
        const bool w16 = instr.opcode == Opcode::MOVBE_R16_M16;
        if (!is_memory_operand(op1.type)) return false;  // reg source is #UD

        if (m_block_is_64bit) {
            emit_compute_ea64(REG_SCRATCH2, op1.mem, next_rip);
            if (w64) emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
            else if (w16) emit_guest_load16_64(REG_SCRATCH0, REG_SCRATCH2);
            else emit_guest_load32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_compute_ea(REG_SCRATCH2, op1.mem, next_rip);
            if (w16) emit_guest_load16(REG_SCRATCH0, REG_SCRATCH2);
            else emit_guest_load32(REG_SCRATCH0, REG_SCRATCH2);
        }

        if (w64) {
            emit(arm64::rev_x(REG_SCRATCH0, REG_SCRATCH0));
            emit_store_gpr64(REG_SCRATCH0, op0.reg);
        } else if (w16) {
            emit(arm64::rev_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH0, 16));
            // 16-bit destination: only the low half is written, no extension.
            emit_store_gpr16(REG_SCRATCH0, op0.reg);
        } else {
            emit(arm64::rev_w(REG_SCRATCH0, REG_SCRATCH0));
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
        }
        break;
    }

    case Opcode::MOVBE_M16_R16:
    case Opcode::MOVBE_M32_R32:
    case Opcode::MOVBE_M64_R64: {
        const bool w64 = instr.opcode == Opcode::MOVBE_M64_R64;
        const bool w16 = instr.opcode == Opcode::MOVBE_M16_R16;
        if (!is_memory_operand(op0.type)) return false;

        if (w64) {
            emit_load_gpr64(REG_SCRATCH0, op1.reg);
            emit(arm64::rev_x(REG_SCRATCH0, REG_SCRATCH0));
        } else if (w16) {
            emit_load_gpr16(REG_SCRATCH0, op1.reg);
            emit(arm64::rev_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::lsr_w_imm(REG_SCRATCH0, REG_SCRATCH0, 16));
        } else {
            emit_load_gpr32(REG_SCRATCH0, op1.reg);
            emit(arm64::rev_w(REG_SCRATCH0, REG_SCRATCH0));
        }

        if (m_block_is_64bit) {
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);
            if (w64) emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);
            else if (w16) emit_guest_store16_64(REG_SCRATCH0, REG_SCRATCH2);
            else emit_guest_store32_64(REG_SCRATCH0, REG_SCRATCH2);
        } else {
            emit_compute_ea(REG_SCRATCH2, op0.mem, next_rip);
            if (w16) emit_guest_store16(REG_SCRATCH0, REG_SCRATCH2);
            else emit_guest_store32(REG_SCRATCH0, REG_SCRATCH2);
        }
        break;
    }

    // Intel SDM Vol. 2 XLAT/XLATB: AL := byte at DS:[(E/R)BX + unsigned AL].
    // Address-size override (16-bit BX / 32-bit EBX-in-long-mode) stays on the
    // interpreter delegate — PE32 and default long-mode forms are the hot path.
    case Opcode::XLAT: {
        if (instr.prefixes.address_size) return false;
        constexpr u8 rax = static_cast<u8>(RegIndex::RAX);
        constexpr u8 rbx = static_cast<u8>(RegIndex::RBX);
        if (m_block_is_64bit) {
            emit_load_gpr64(REG_SCRATCH2, rbx);
            emit_load_gpr8(REG_SCRATCH0, rax, /*has_rex=*/true);
            emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::add_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH0));
            const SegReg seg = instr.prefixes.segment;
            if (seg == SegReg::FS || seg == SegReg::GS) {
                const u32 base_off = seg_base_offset(seg);
                emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                        base_off >> 3));
                emit(arm64::add_x(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            }
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            emit_store_gpr8(REG_SCRATCH0, rax, /*has_rex=*/true);
        } else {
            emit_load_gpr32(REG_SCRATCH2, rbx);
            emit_load_gpr8(REG_SCRATCH0, rax, /*has_rex=*/false);
            emit(arm64::uxtb_w(REG_SCRATCH0, REG_SCRATCH0));
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH0));
            const SegReg seg = instr.prefixes.segment == SegReg::Default
                ? SegReg::DS
                : instr.prefixes.segment;
            const u32 base_off = seg_base_offset(seg);
            emit(arm64::ldr_x_uoff(REG_SCRATCH1, REG_CPU_STATE,
                                    base_off >> 3));
            emit(arm64::add_w(REG_SCRATCH2, REG_SCRATCH2, REG_SCRATCH1));
            emit_guest_load8(REG_SCRATCH0, REG_SCRATCH2);
            emit_store_gpr8(REG_SCRATCH0, rax, /*has_rex=*/false);
        }
        break;
    }

    default: {
        // Every other opcode belongs to one family, ops/memory_ops_*.cpp.
        // Their case labels are disjoint (this was one switch before the
        // split), so the order of this chain cannot be observed.
        std::optional<bool> lowered = compile_memory_ops_xchg(instr, next_rip);
        if (!lowered) lowered = compile_memory_ops_cond(instr, next_rip);
        if (!lowered) lowered = compile_memory_ops_bt(instr, next_rip);
        if (!lowered) lowered = compile_memory_ops_bitscan(instr, next_rip);
        if (!lowered) lowered = compile_memory_ops_bmi(instr, next_rip);
        if (!lowered.has_value() || !*lowered) return false;
        break;
    }
    }

    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

// ============================================================================
// LOCK-prefixed instruction compilation
// ============================================================================

bool JitCompiler::compile_locked(const DecodedInstruction& instr,
                                  u64 instr_rip, u64 next_rip,
                                  bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    const auto emit_locked_ea = [&](u8 dst_arm_reg, const MemoryOperand& mem) {
        if (m_block_is_64bit) {
            emit_compute_ea64(dst_arm_reg, mem, next_rip);
        } else {
            emit_compute_ea(dst_arm_reg, mem, next_rip);
        }
    };

    // Diagnostic-only attribution for atomic anomalies. Locked helpers are
    // otherwise free to leave RIP untouched until the block exit, but a
    // helper-side diagnostic needs the exact guest instruction rather than
    // whichever memory operation most recently published CpuState::rip.
    if (std::getenv("VITREO_LOCK_DIAG") != nullptr) {
        emit_update_rip(instr_rip);
    }

    switch (instr.opcode) {

    case Opcode::BT_RM64_R64:
    case Opcode::BTS_RM64_R64:
    case Opcode::BTR_RM64_R64:
    case Opcode::BTC_RM64_R64:
    case Opcode::BT_RM64_IMM8:
    case Opcode::BTS_RM64_IMM8:
    case Opcode::BTR_RM64_IMM8:
    case Opcode::BTC_RM64_IMM8:
    case Opcode::BT_RM32_R32:
    case Opcode::BTS_RM32_R32:
    case Opcode::BTR_RM32_R32:
    case Opcode::BTC_RM32_R32:
    case Opcode::BT_RM32_IMM8:
    case Opcode::BTS_RM32_IMM8:
    case Opcode::BTR_RM32_IMM8:
    case Opcode::BTC_RM32_IMM8:
    case Opcode::BT_RM16_R16:
    case Opcode::BTS_RM16_R16:
    case Opcode::BTR_RM16_R16:
    case Opcode::BTC_RM16_R16:
    case Opcode::BT_RM16_IMM8:
    case Opcode::BTS_RM16_IMM8:
    case Opcode::BTR_RM16_IMM8:
    case Opcode::BTC_RM16_IMM8: {
        // Intel SDM Vol. 2 LOCK: BTS/BTR/BTC mem forms; BT under LOCK matches
        // the interpreter (process mutex + CF-only). Bit-string EA is inside
        // the helper (addr + signed_index/8).
        //
        // All three operand widths share ONE helper family because the memory
        // bit-string form is operand-size independent: the helper touches the
        // single byte at base + (index >> 3), bit index & 7, for 16, 32 and 64
        // alike. `exec_bt` in the interpreter does exactly this and reduces
        // modulo the operand size only on the REGISTER bit-base form, which
        // never reaches compile_locked (LOCK on a register destination is #UD).
        //
        // The only width-dependent term is how the bit index is sign-extended,
        // and the interpreter pins it: an imm8 offset is unsigned 0..255 at
        // every width, while a register offset is signed at ITS OWN operand
        // width (i16 / i32 / i64). Sign-extending is not cosmetic — a negative
        // index must address a byte BELOW the operand, and zero-extending a
        // 32-bit index instead sends it ~4 GiB above it.
        //
        // Before this arm existed, the 32- and 16-bit forms fell through to the
        // per-instruction interpreter delegate, whose dispatcher-level
        // LockPrefixGuard carries no operand information and therefore takes the
        // PROCESS-WIDE exclusive mutex. `lock bts dword ptr [x], reg` is the
        // ordinary bitmap/spinlock primitive, so that is the same defect class
        // as XCHG_RM32_R32 above.
        if (is_register_operand(op0.type)) return false;
        const bool is_reg_idx = !is_immediate_operand(op1.type);
        if (is_reg_idx) {
            switch (operand_size(op1.type)) {
            case 8:
                emit_load_gpr64(REG_SCRATCH1, op1.reg);
                emit(arm64::mov_x(10, REG_SCRATCH1));
                break;
            case 2:
                emit_load_gpr16(REG_SCRATCH1, op1.reg);
                emit(arm64::sxth_x(10, REG_SCRATCH1));
                break;
            default:
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
                emit(arm64::sxtw_x(10, REG_SCRATCH1));
                break;
            }
        } else {
            // Imm8 bit index is unsigned 0..255 (interpreter contract).
            emit_mov_imm64(10, static_cast<i64>(op1.imm & 0xFF));
        }
        emit_locked_ea(REG_SCRATCH2, op0.mem);
        emit(arm64::mov_x(0, REG_CPU_STATE));
        emit(arm64::ldr_x_uoff(1, REG_JIT_CTX, JIT_CTX_OFF_MEMORY >> 3));
        emit(arm64::mov_x(2, REG_SCRATCH2));
        emit(arm64::mov_x(3, 10));
        void* helper = reinterpret_cast<void*>(&jit_lock_bt64);
        switch (instr.opcode) {
        case Opcode::BTS_RM64_R64:
        case Opcode::BTS_RM64_IMM8:
        case Opcode::BTS_RM32_R32:
        case Opcode::BTS_RM32_IMM8:
        case Opcode::BTS_RM16_R16:
        case Opcode::BTS_RM16_IMM8:
            helper = reinterpret_cast<void*>(&jit_lock_bts64);
            break;
        case Opcode::BTR_RM64_R64:
        case Opcode::BTR_RM64_IMM8:
        case Opcode::BTR_RM32_R32:
        case Opcode::BTR_RM32_IMM8:
        case Opcode::BTR_RM16_R16:
        case Opcode::BTR_RM16_IMM8:
            helper = reinterpret_cast<void*>(&jit_lock_btr64);
            break;
        case Opcode::BTC_RM64_R64:
        case Opcode::BTC_RM64_IMM8:
        case Opcode::BTC_RM32_R32:
        case Opcode::BTC_RM32_IMM8:
        case Opcode::BTC_RM16_R16:
        case Opcode::BTC_RM16_IMM8:
            helper = reinterpret_cast<void*>(&jit_lock_btc64);
            break;
        default:
            break;
        }
        emit_host_addr_imm64(16, static_cast<i64>(reinterpret_cast<uintptr_t>(helper)));
        emit_helper_blr();
        break;
    }

    default: {
        // The other LOCK forms belong to one family, ops/locked_*.cpp. Their
        // case labels are disjoint (this was one switch before the split),
        // so the order of this chain cannot be observed.
        std::optional<bool> lowered = compile_locked_rmw32(instr, next_rip);
        if (!lowered) lowered = compile_locked_rmw64(instr, next_rip);
        if (!lowered) lowered = compile_locked_narrow(instr, next_rip);
        if (lowered.has_value()) {
            if (!*lowered) return false;
            break;
        }
        static const bool lock_diag =
            (std::getenv("VITREO_LOCK_DIAG") != nullptr);
        const u32 diag_slot = lock_diag
            ? g_unsupported_lock_diag_logs.fetch_add(
                  1, std::memory_order_relaxed)
            : 64u;
        if (diag_slot < 64u) {
            std::fprintf(
                stderr,
                "[lock-diag] unsupported-jit-lock rip=0x%llx opcode=%s(%u)\n",
                static_cast<unsigned long long>(instr_rip),
                opcode_name(instr.opcode),
                static_cast<unsigned>(instr.opcode));
        }
        return false;
    }
    }

    // LOCK helpers raise STATUS_ACCESS_VIOLATION on unmapped dest. Exit before
    // later instructions in this block observe stale GPRs (Intel SDM Vol. 2
    // LOCK: the RMW is one instruction; a #PF must not fall through).
    emit_exit_if_pending_raise();

    if (is_last) {
        u32 ji_off = jit_iters_offset();
        emit(arm64::str_w_uoff(REG_LOOP_CTR, REG_CPU_STATE, ji_off >> 2));
        emit_update_rip(next_rip);
        emit_epilogue(JitExecResult::Continue);
    }
    return true;
}

} // namespace vitreo::cpu::jit

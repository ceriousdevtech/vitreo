// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — 64-bit block lowering: the binary ALU. ADD/SUB/AND/OR/XOR
// r/m64, and the 32-bit-operand ADD/SUB/AND/OR/XOR forms inside a 64-bit
// block.
//
// One family of compile_64bit's opcode space, split out of ops/ops64.cpp
// (ADR-0006 step 3) as pure code motion: the case arms are verbatim.
// compile_64bit (ops/ops64.cpp) owns the dispatch chain; see the
// declaration in jit_compiler.hpp for the nullopt / false / true contract.

#include "ops64_internal.hpp"

namespace vitreo::cpu::jit {

namespace {
// Launch-time diagnostic switches, not guest environment variables. Re-reading
// environ while compiling every ALU instruction serializes compiler threads
// on libc's environment lock. Preserve the switches without that hot-path work.
const bool no_inplace_alu = std::getenv("VITREO_JIT_NO_INPLACE_ALU") != nullptr;
const bool no_adds_imm = std::getenv("VITREO_JIT_NO_ADDS_IMM") != nullptr;
}  // namespace

// ============================================================================
// Helpers — 64-bit binary ALU pattern (sets full Add64/Sub64/Logic64 flags)
// ============================================================================
//
// Common shape:
//   dst = op0   (R/M64), src = op1   (R/M64 or imm32 sign-extended to 64).
//   load original dst into X0 (full 64-bit)
//   load src into X1 (full 64-bit; imm sign-extended via emit_mov_imm64)
//   X0 = X0 <alu_op> X1
//   set lazy flags(flag_op, src=X1, dst=X0)
//   store X0 back to dst (R or M)
//
// CMP/TEST are like the binary ALUs but skip the store.
// For LOGIC ops (AND/OR/XOR/TEST), flag_src is irrelevant — only the result
// matters — but we pass it through for uniformity.

// ============================================================================
// Binary ALU template — 64-bit
// ============================================================================
#define EMIT_BINARY_ALU64(alu_emit, flag_op)                                 \
    do {                                                                      \
        bool dst_is_mem = is_mem(op0);                                        \
        bool src_is_mem = is_mem(op1);                                        \
        bool src_is_imm = is_imm(op1);                                        \
        if (src_is_mem) {                                                     \
            emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);               \
            emit_guest_load64(REG_SCRATCH1, REG_SCRATCH3);                    \
            emit_load_gpr64(REG_SCRATCH0, op0.reg);                           \
        } else if (dst_is_mem) {                                              \
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);               \
            emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);                    \
            if (src_is_imm)                                                   \
                emit_mov_imm64(REG_SCRATCH1, sx_imm64(op1));                  \
            else                                                              \
                emit_load_gpr64(REG_SCRATCH1, op1.reg);                       \
        } else {                                                              \
            emit_load_gpr64(REG_SCRATCH0, op0.reg);                           \
            if (src_is_imm)                                                   \
                emit_mov_imm64(REG_SCRATCH1, sx_imm64(op1));                  \
            else                                                              \
                emit_load_gpr64(REG_SCRATCH1, op1.reg);                       \
        }                                                                     \
        alu_emit;                                                             \
        emit_set_lazy_flags64(flag_op, REG_SCRATCH1, REG_SCRATCH0);           \
        if (dst_is_mem) {                                                     \
            emit_compute_ea64(REG_SCRATCH2, op0.mem, next_rip);               \
            emit_guest_store64(REG_SCRATCH0, REG_SCRATCH2);                   \
        } else {                                                              \
            emit_store_gpr64(REG_SCRATCH0, op0.reg);                          \
        }                                                                     \
    } while (0)

// In-place variant of EMIT_BINARY_ALU64 for a pinned register destination:
// the pin IS the guest register, so the copy-out / copy-back staging is pure
// overhead (`add rax, 1` cost 4 ARM instructions, three of them a serialized
// chain, for what ARM64 does in one or two). A memory source is loaded into
// the scratch first; the destination pin is untouched by that access.
// Everything else falls through to the generic macro unchanged.
//
// The source may be read from its own pin only when it is not also the
// destination: emit_set_lazy_flags64 names the source REGISTER, and for
// `add rax, rax` the pin would already hold the result by the time the lazy
// operands are stored.
#define EMIT_BINARY_ALU64_MAYBE_INPLACE(alu_insn, flag_op)                    \
    do {                                                                      \
        const u8 ip_dst = is_mem(op0) ? u8{0xFF} : pinned_gpr64(op0.reg);     \
        if (ip_dst != 0xFF && !no_inplace_alu) {                              \
            u8 ip_src = REG_SCRATCH1;                                         \
            if (is_register_operand(op1.type) && op1.reg != op0.reg) {        \
                const u8 sp = pinned_gpr64(op1.reg);                          \
                if (sp != 0xFF) ip_src = sp;                                  \
            }                                                                 \
            if (is_mem(op1)) {                                                \
                /* Pins survive the access's slow path (emit_helper_blr), */  \
                /* so only the loaded source needs a scratch. */              \
                emit_compute_ea64(REG_SCRATCH3, op1.mem, next_rip);           \
                emit_guest_load64(REG_SCRATCH1, REG_SCRATCH3);                \
            } else if (ip_src == REG_SCRATCH1) {                              \
                if (is_imm(op1))                                              \
                    emit_mov_imm64(REG_SCRATCH1, sx_imm64(op1));              \
                else                                                          \
                    emit_load_gpr64(REG_SCRATCH1, op1.reg);                   \
            }                                                                 \
            emit(arm64::alu_insn(ip_dst, ip_dst, ip_src));                    \
            /* emit_store_gpr64 side effect the in-place write must keep. */  \
            if (op0.reg == static_cast<u8>(RegIndex::RSP))                    \
                m_esp_frame_live = false;                                     \
            emit_set_lazy_flags64(flag_op, ip_src, ip_dst);                   \
        } else {                                                              \
            EMIT_BINARY_ALU64(                                                \
                emit(arm64::alu_insn(REG_SCRATCH0, REG_SCRATCH0,              \
                                     REG_SCRATCH1)),                          \
                flag_op);                                                     \
        }                                                                     \
    } while (0)

// nullopt means "not one of mine", so compile_64bit tries the next family.
// `break` means what it meant before the split: run the end-of-block tail
// and report success.
std::optional<bool> JitCompiler::compile_arithmetic64(
    const DecodedInstruction& instr, u64 next_rip, bool is_last) {
    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    (void)op0; (void)op1;

    switch (instr.opcode) {

    // ========================================================================
    // ALU binary ops
    // ========================================================================

    // The flag-setting ARM64 forms (ADDS/SUBS/ANDS) compute the same result as
    // their plain counterparts, so recording NZCV costs nothing and lets a
    // terminal Jcc fuse into a CSET instead of calling a cond helper that
    // rebuilds the flag from lazy state. A memory destination stores after the
    // ALU op, and the store's bounds check uses SUBS, so NZCV does not survive
    // it — those keep m_flags.nzcv clear and fall back to the helper.
    case Opcode::ADD_RM64_R64:
    case Opcode::ADD_R64_RM64:
    case Opcode::ADD_RM64_IMM32:
    case Opcode::ADD_RM64_IMM8:
    case Opcode::ADD_RAX_IMM32: {
        EMIT_BINARY_ALU64_MAYBE_INPLACE(
            adds_x, CpuState::FlagOp::Add64);
        if (!is_mem(op0)) m_flags.nzcv = NzcvState::Add;
        break;
    }

    case Opcode::SUB_RM64_R64:
    case Opcode::SUB_R64_RM64:
    case Opcode::SUB_RM64_IMM32:
    case Opcode::SUB_RM64_IMM8:
    case Opcode::SUB_RAX_IMM32: {
        EMIT_BINARY_ALU64_MAYBE_INPLACE(
            subs_x, CpuState::FlagOp::Sub64);
        if (!is_mem(op0)) m_flags.nzcv = NzcvState::Sub;
        break;
    }

    case Opcode::AND_RM64_R64:
    case Opcode::AND_R64_RM64:
    case Opcode::AND_RM64_IMM32:
    case Opcode::AND_RM64_IMM8:
    case Opcode::AND_RAX_IMM32: {
        EMIT_BINARY_ALU64_MAYBE_INPLACE(
            ands_x, CpuState::FlagOp::And64);
        if (!is_mem(op0)) m_flags.nzcv = NzcvState::And;
        break;
    }

    case Opcode::OR_RM64_R64:
    case Opcode::OR_R64_RM64:
    case Opcode::OR_RM64_IMM32:
    case Opcode::OR_RM64_IMM8:
    case Opcode::OR_RAX_IMM32: {
        EMIT_BINARY_ALU64_MAYBE_INPLACE(
            orr_x, CpuState::FlagOp::Or64);
        break;
    }

    case Opcode::XOR_RM64_R64:
    case Opcode::XOR_R64_RM64:
    case Opcode::XOR_RM64_IMM32:
    case Opcode::XOR_RM64_IMM8:
    case Opcode::XOR_RAX_IMM32: {
        EMIT_BINARY_ALU64_MAYBE_INPLACE(
            eor_x, CpuState::FlagOp::Xor64);
        break;
    }

    // ========================================================================
    // 32-bit-operand ops inside a 64-bit block (register-only forms).
    //
    // In x64 mode, instructions without a REX.W prefix operate on 32-bit
    // operands but zero-extend results to the full 64-bit GPR. ARM64's
    // 32-bit ops (W register) zero-extend automatically, and
    // emit_store_gpr32 clears the upper 32 bits of the GPR slot, so the
    // semantics line up.
    //
    // Memory forms compute the full long-mode EA. Low-address mappings reuse
    // the cached inline access path; >4 GB addresses use checked helpers.
    // ========================================================================

    // 32-bit ALU binary (ADD/SUB/AND/OR/XOR) — register-only forms.
    case Opcode::ADD_RM32_R32:
    case Opcode::ADD_R32_RM32:
    case Opcode::ADD_RM32_IMM32:
    case Opcode::ADD_RM32_IMM8:
    case Opcode::ADD_EAX_IMM32:
    case Opcode::SUB_RM32_R32:
    case Opcode::SUB_R32_RM32:
    case Opcode::SUB_RM32_IMM32:
    case Opcode::SUB_RM32_IMM8:
    case Opcode::SUB_EAX_IMM32:
    case Opcode::AND_RM32_R32:
    case Opcode::AND_R32_RM32:
    case Opcode::AND_RM32_IMM32:
    case Opcode::AND_RM32_IMM8:
    case Opcode::AND_EAX_IMM32:
    case Opcode::OR_RM32_R32:
    case Opcode::OR_R32_RM32:
    case Opcode::OR_RM32_IMM32:
    case Opcode::OR_RM32_IMM8:
    case Opcode::OR_EAX_IMM32:
    case Opcode::XOR_RM32_R32:
    case Opcode::XOR_R32_RM32:
    case Opcode::XOR_RM32_IMM32:
    case Opcode::XOR_RM32_IMM8:
    case Opcode::XOR_EAX_IMM32: {
        auto emit_alu32 = [&](u8 dst, u8 src)
            -> std::pair<CpuState::FlagOp, NzcvState> {
            switch (instr.opcode) {
            case Opcode::ADD_RM32_R32: case Opcode::ADD_R32_RM32:
            case Opcode::ADD_RM32_IMM32: case Opcode::ADD_RM32_IMM8:
            case Opcode::ADD_EAX_IMM32:
                emit(arm64::adds_w(dst, dst, src));
                return {CpuState::FlagOp::Add32, NzcvState::Add};
            case Opcode::SUB_RM32_R32: case Opcode::SUB_R32_RM32:
            case Opcode::SUB_RM32_IMM32: case Opcode::SUB_RM32_IMM8:
            case Opcode::SUB_EAX_IMM32:
                emit(arm64::subs_w(dst, dst, src));
                return {CpuState::FlagOp::Sub32, NzcvState::Sub};
            case Opcode::AND_RM32_R32: case Opcode::AND_R32_RM32:
            case Opcode::AND_RM32_IMM32: case Opcode::AND_RM32_IMM8:
            case Opcode::AND_EAX_IMM32:
                emit(arm64::ands_w(dst, dst, src));
                return {CpuState::FlagOp::And32, NzcvState::And};
            case Opcode::OR_RM32_R32: case Opcode::OR_R32_RM32:
            case Opcode::OR_RM32_IMM32: case Opcode::OR_RM32_IMM8:
            case Opcode::OR_EAX_IMM32:
                emit(arm64::orr_w(dst, dst, src));
                return {CpuState::FlagOp::Or32, NzcvState::None};
            default:
                emit(arm64::eor_w(dst, dst, src));
                return {CpuState::FlagOp::Xor32, NzcvState::None};
            }
        };

        auto emit_source32 = [&] {
            if (is_imm(op1)) {
                emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm));
            } else {
                emit_load_gpr32(REG_SCRATCH1, op1.reg);
            }
        };

        // Ordinary non-LOCK memory RMW with an immediate/register source.
        // A published x64 write slot now proves readable+writable, unwatched,
        // non-executable and code-free. Probe it once, then use the same host
        // mapping for LDR -> ALU -> STR. Any miss takes the unchanged checked
        // read/arithmetic/write sequence below.
        // Same idea for the guest-base model, which otherwise emits a COMPLETE
        // guard twice for one address -- bounds, check byte and base add for
        // the load half, then all of it again for the store half. `sub
        // [mem],1` was the one bench kernel still slower than the region path
        // for exactly this reason. Guard once, then LDR -> ALU -> STR against
        // the same host address.
        //
        // Hardware protection is required: without it the check byte carries
        // only kLinearCheckHoldsCode / kLinearCheckWatched and says nothing
        // about permission, so a single byte test would not be a complete
        // guard. That configuration keeps the two-guard path below.
        if (is_mem(op0) && !is_mem(op1) && m_cfg.linear64 && m_cfg.linear64_hw_protect &&
            m_cfg.linear64_perm_guard) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit(arm64::mov_x(9, REG_SCRATCH4));
            // Both ends against the reservation in one NZCV-free branch.
            emit(arm64::add_x_imm(17, 9, 3));
            emit(arm64::orr_x(8, 9, 17));
            emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
            const usize l_outside_pos = m_code_buf.size();
            emit(0);
            // The WHOLE byte: this is a store, so inexact protection, a page
            // holding translated code and a watched page all have to divert.
            emit(arm64::ubfx_x(4, 17,
                               static_cast<u8>(kLinearHostPageShift),
                               static_cast<u8>(m_cfg.linear64_address_bits -
                                               kLinearHostPageShift)));
            emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(3), 4));
            const usize l_check_pos = m_code_buf.size();
            emit(0);
            // x17 stops being the last-byte address here and becomes the host
            // address; the check-byte index above already consumed it.
            emit(arm64::add_x(17, REG_LINEAR_BASE, 9));
            // Ordered accesses fault on a 16-byte granule straddle, exactly as
            // in the region path above.
            emit(arm64::ubfx_x(8, 17, 0, 4));
            emit(arm64::cmp_x_imm(8, 12));
            const usize l_granule_pos = m_code_buf.size();
            emit(0);

            emit_acquire_load(REG_SCRATCH0, 17, 4);
            emit_source32();
            const auto [l_fop, l_nzcv] =
                emit_alu32(REG_SCRATCH0, REG_SCRATCH1);
            emit_release_store(REG_SCRATCH0, 17, 4);
            emit_set_lazy_flags(l_fop, REG_SCRATCH1, REG_SCRATCH0);
            m_flags.nzcv = l_nzcv;
            const usize l_done_branch = m_code_buf.size();
            emit(0);

            const usize l_slow_pos = m_code_buf.size();
            const auto l_disp = [&](usize pos) {
                return static_cast<i32>(l_slow_pos - pos) * 4;
            };
            m_code_buf[l_outside_pos] = arm64::cbnz_x(8, l_disp(l_outside_pos));
            m_code_buf[l_check_pos] = arm64::cbnz_x(3, l_disp(l_check_pos));
            m_code_buf[l_granule_pos] = arm64::b_cond(arm64::HI,
                                                      l_disp(l_granule_pos));

            // Unchanged checked sequence: two full guards, but only when one of
            // the three tests above actually diverted.
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
            emit_source32();
            const auto [l_sfop, l_snzcv] =
                emit_alu32(REG_SCRATCH0, REG_SCRATCH1);
            emit_set_lazy_flags(l_sfop, REG_SCRATCH1, REG_SCRATCH0);
            emit(arm64::mov_w(17, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            m_flags.hold_nzcv = (l_snzcv != NzcvState::None);
            emit_guest_store32(17, REG_SCRATCH4);
            m_flags.hold_nzcv = false;
            m_flags.nzcv = l_snzcv;

            const usize l_done_pos = m_code_buf.size();
            m_code_buf[l_done_branch] = arm64::b_imm(
                static_cast<i32>(l_done_pos - l_done_branch) * 4);
            break;
        }

        if (is_mem(op0) && !is_mem(op1) && !m_cfg.linear64) {
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit(arm64::mov_x(9, REG_SCRATCH4));
            emit(arm64::ubfx_x(17, 9,
                               static_cast<u8>(X64_SLOT_GRANULE_SHIFT),
                               static_cast<u8>(X64_SLOT_INDEX_BITS)));
            emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                   JIT_CTX_OFF_X64_WSLOTS >> 3));
            emit(arm64::add_x_lsl(3, 3, 17, 5));
            emit(arm64::ldp_x_off(4, 8, 3, 0));
            emit(arm64::subs_x(4, 9, 4));
            const usize below_pos = m_code_buf.size();
            emit(0);
            emit(arm64::adds_x_imm(17, 9, 3));
            const usize overflow_pos = m_code_buf.size();
            emit(0);
            emit(arm64::subs_x(31, 17, 8));
            const usize above_pos = m_code_buf.size();
            emit(0);
            emit(arm64::ldr_x_uoff(3, 3, 2));
            // Ordered AArch64 scalar accesses carry x86-TSO ordering without
            // the three full DMBs required by the plain LDR/STR lowering.
            // Apple Silicon permits unaligned ordered accesses unless they
            // cross a 16-byte granule, so send offsets 13..15 through the
            // existing fenced checked path below.
            emit(arm64::add_x(17, 3, 4));
            emit(arm64::ubfx_x(8, 17, 0, 4));
            emit(arm64::cmp_x_imm(8, 12));
            const usize granule_cross_pos = m_code_buf.size();
            emit(0);
            emit_acquire_load(REG_SCRATCH0, 17, 4);
            emit_source32();
            const auto [fast_fop, fast_nzcv] =
                emit_alu32(REG_SCRATCH0, REG_SCRATCH1);
            emit_release_store(REG_SCRATCH0, 17, 4);
            emit_set_lazy_flags(fast_fop, REG_SCRATCH1, REG_SCRATCH0);
            m_flags.nzcv = fast_nzcv;
            const usize done_branch = m_code_buf.size();
            emit(0);

            const usize slow_pos = m_code_buf.size();
            const auto slow_disp = [&](usize pos) {
                return static_cast<i32>(slow_pos - pos) * 4;
            };
            m_code_buf[below_pos] = arm64::b_cond(arm64::CC,
                                                  slow_disp(below_pos));
            m_code_buf[overflow_pos] = arm64::b_cond(
                arm64::CS, slow_disp(overflow_pos));
            m_code_buf[above_pos] = arm64::b_cond(arm64::CS,
                                                  slow_disp(above_pos));
            m_code_buf[granule_cross_pos] = arm64::b_cond(
                arm64::HI, slow_disp(granule_cross_pos));

            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            emit_guest_load32(REG_SCRATCH0, REG_SCRATCH4);
            emit_source32();
            const auto [slow_fop, slow_nzcv] =
                emit_alu32(REG_SCRATCH0, REG_SCRATCH1);
            emit_set_lazy_flags(slow_fop, REG_SCRATCH1, REG_SCRATCH0);
            emit(arm64::mov_w(17, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            m_flags.hold_nzcv = (slow_nzcv != NzcvState::None);
            emit_guest_store32(17, REG_SCRATCH4);
            m_flags.hold_nzcv = false;
            m_flags.nzcv = slow_nzcv;

            const usize done_pos = m_code_buf.size();
            m_code_buf[done_branch] = arm64::b_imm(
                static_cast<i32>(done_pos - done_branch) * 4);
            break;
        }

        // Read a 32-bit memory operand into `dst` through the inline region
        // fast path, which falls back to the read32 helper on a miss.
        auto read32_to = [&](u8 dst, const Operand& o) {
            emit_compute_ea64(REG_SCRATCH4, o.mem, next_rip);
            emit_guest_load32(dst, REG_SCRATCH4);
        };
        // In-place on the pinned destination. The generic path below copies
        // the pin into W0, operates there, and copies back, so `add eax, 3`
        // costs four ARM instructions where one or two suffice. Only the
        // register-destination, non-memory-source shape qualifies: a memory
        // operand has to reach a scratch anyway.
        const u8 dst_pin = is_register_operand(op0.type)
                               ? pinned_gpr32(op0.reg)
                               : u8{0xFF};
        // XOR r32,r32 / SUB r32,r32 on one register: the result is 0 whatever
        // the register held (Intel SDM Vol. 2 XOR, SUB), so neither operand
        // has to be read. The record names the zero result as its source too,
        // which is exact: logic flags derive from the result alone, and SUB's
        // are those of 0 - 0. SUBS keeps the host NZCV claim (Z=1, C=1 is "no
        // borrow"); EOR writes none.
        if ((instr.opcode == Opcode::XOR_RM32_R32 ||
             instr.opcode == Opcode::XOR_R32_RM32 ||
             instr.opcode == Opcode::SUB_RM32_R32 ||
             instr.opcode == Opcode::SUB_R32_RM32) &&
            is_register_operand(op0.type) && is_register_operand(op1.type) &&
            op0.reg == op1.reg && !no_inplace_alu) {
            const bool is_sub = instr.opcode == Opcode::SUB_RM32_R32 ||
                                instr.opcode == Opcode::SUB_R32_RM32;
            const auto fop = is_sub ? CpuState::FlagOp::Sub32
                                    : CpuState::FlagOp::Xor32;
            if (dst_pin != 0xFF) {
                emit(is_sub ? arm64::subs_w(dst_pin, dst_pin, dst_pin)
                            : arm64::eor_w(dst_pin, dst_pin, dst_pin));
                if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                    m_esp_frame_live = false;
                }
                emit_set_lazy_flags(fop, dst_pin, dst_pin);
            } else {
                if (is_sub) emit(arm64::subs_w(31, 31, 31));
                emit_store_gpr32(31, op0.reg);
                emit_set_lazy_flags(fop, 31, 31);
            }
            m_flags.nzcv = is_sub ? NzcvState::Sub : NzcvState::None;
            break;
        }
        if (dst_pin != 0xFF && !is_mem(op1) &&
            !no_inplace_alu) {
            // A decoded adjacent self-TEST overwrites every architectural
            // ADD/SUB flag before any observer. Keep the result N/Z in PSTATE for
            // that TEST/Jcc fold, but do not materialize an immediate source
            // that no lazy record can consume. For a negative imm8, ADD x,-k
            // and SUBS x,#k have the same modulo result, N/Z, carry/no-borrow,
            // and signed overflow. TEST overwrites those flags immediately and
            // publishes its own lazy PF/CF/OF state before any observer.
            const bool adjacent_self_test_imm8 =
                (instr.opcode == Opcode::ADD_RM32_IMM8 ||
                 instr.opcode == Opcode::SUB_RM32_IMM8) &&
                is_imm(op1) &&
                m_omit_lazy_current &&
                m_next_self_test_gpr == op0.reg &&
                !no_adds_imm;
            if (adjacent_self_test_imm8) {
                const i64 immediate = static_cast<i64>(op1.imm);
                if (immediate >= -128 && immediate <= 127) {
                    const bool arm_add =
                        (instr.opcode == Opcode::ADD_RM32_IMM8) ==
                        (immediate >= 0);
                    const u32 magnitude = static_cast<u32>(
                        immediate >= 0 ? immediate : -immediate);
                    if (arm_add) {
                        emit(arm64::adds_w_imm(
                            dst_pin, dst_pin, magnitude));
                    } else {
                        emit(arm64::subs_w_imm(
                            dst_pin, dst_pin, magnitude));
                    }
                    if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                        m_esp_frame_live = false;
                    }
                    m_pending_alu32_nz_result = dst_pin;
                    m_flags.nzcv = NzcvState::NzOnly;
                    break;
                }
            }
            const bool add_reg =
                instr.opcode == Opcode::ADD_RM32_R32 ||
                instr.opcode == Opcode::ADD_R32_RM32;
            const bool sub_reg =
                instr.opcode == Opcode::SUB_RM32_R32 ||
                instr.opcode == Opcode::SUB_R32_RM32;
            if ((add_reg || sub_reg) && is_register_operand(op1.type) &&
                op1.reg == op0.reg && m_omit_lazy_current &&
                m_next_self_test_gpr == op0.reg) {
                if (add_reg) {
                    emit(arm64::adds_w(dst_pin, dst_pin, dst_pin));
                } else {
                    emit(arm64::subs_w(dst_pin, dst_pin, dst_pin));
                }
                if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                    m_esp_frame_live = false;
                }
                m_pending_alu32_nz_result = dst_pin;
                m_flags.nzcv = NzcvState::NzOnly;
                break;
            }
            // The lazy-flag record names a register that must still hold the
            // SOURCE when the (possibly cold-stub) store is emitted. Reading
            // op1's own pin is therefore only safe while it is not also the
            // destination: `add eax, eax` would otherwise record the result as
            // its own source and materialise the wrong flags.
            u8 src_arm = REG_SCRATCH1;
            if (is_register_operand(op1.type) && op1.reg != op0.reg) {
                const u8 src_pin = pinned_gpr32(op1.reg);
                if (src_pin != 0xFF) src_arm = src_pin;
            }
            if (src_arm == REG_SCRATCH1) emit_source32();

            const auto [fop_ip, nzcv_ip] = emit_alu32(dst_pin, src_arm);
            // emit_store_gpr32 would have done this; the writeback is gone but
            // the invalidation it implies is not.
            if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                m_esp_frame_live = false;
            }
            emit_set_lazy_flags(fop_ip, src_arm, dst_pin);
            m_flags.nzcv = nzcv_ip;
            break;
        }

        // Memory SOURCE into a register destination: read the operand first,
        // so nothing has to be held across the access, then operate on the
        // destination where it lives -- in place when pinned, since pins
        // survive the access's slow path (emit_helper_blr). Staging the
        // destination through W0 and X21 around the access cost four
        // instructions per `add eax, [rcx+4]`.
        if (is_register_operand(op0.type) && is_mem(op1) && !no_inplace_alu) {
            read32_to(REG_SCRATCH1, op1);
            const u8 mdst = dst_pin != 0xFF ? dst_pin : u8{REG_SCRATCH0};
            if (mdst == REG_SCRATCH0) emit_load_gpr32(REG_SCRATCH0, op0.reg);
            const auto [fop_m, nzcv_m] = emit_alu32(mdst, REG_SCRATCH1);
            emit_set_lazy_flags(fop_m, REG_SCRATCH1, mdst);
            if (mdst == REG_SCRATCH0) {
                emit_store_gpr32(REG_SCRATCH0, op0.reg);
            } else if (op0.reg == static_cast<u8>(RegIndex::RSP)) {
                m_esp_frame_live = false;
            }
            m_flags.nzcv = nzcv_m;
            break;
        }

        // Load op0 into W0.
        if (is_register_operand(op0.type)) {
            emit_load_gpr32(REG_SCRATCH0, op0.reg);
        } else {
            read32_to(REG_SCRATCH0, op0);
        }
        // Load op1 into W1, stashing W0 across BLR when needed.
        if (is_imm(op1)) {
            emit_mov_imm32(REG_SCRATCH1, static_cast<u32>(op1.imm));
        } else if (is_register_operand(op1.type)) {
            emit_load_gpr32(REG_SCRATCH1, op1.reg);
        } else {
            emit(arm64::mov_x(REG_CALL_TMP, REG_SCRATCH0));
            read32_to(REG_SCRATCH1, op1);
            emit(arm64::mov_w(REG_SCRATCH0, REG_CALL_TMP));
        }

        const auto [fop, nzcv] =
            emit_alu32(REG_SCRATCH0, REG_SCRATCH1);
        emit_set_lazy_flags(fop, REG_SCRATCH1, REG_SCRATCH0);

        // Store result back to op0.
        if (is_register_operand(op0.type)) {
            emit_store_gpr32(REG_SCRATCH0, op0.reg);
            m_flags.nzcv = nzcv;
        } else {
            emit(arm64::mov_w(17, REG_SCRATCH0));
            emit_compute_ea64(REG_SCRATCH4, op0.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op0);
            // Intel SDM SUB/ADD/AND: flags come from the arithmetic result,
            // not from the following memory writeback. Preserve the native
            // NZCV across the checked store so an adjacent Jcc can consume it.
            m_flags.hold_nzcv = (nzcv != NzcvState::None);
            emit_guest_store32(17, REG_SCRATCH4);
            m_flags.hold_nzcv = false;
            m_flags.nzcv = nzcv;
        }
        break;
    }

    default: return std::nullopt;
    }

    finish_op64_block(next_rip, is_last);
    return true;
}

}  // namespace vitreo::cpu::jit

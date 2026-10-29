// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — direct ARMv8 AES lowering for x86 AES-NI.

#include "../jit_helpers.hpp"

/// @file direct_armv8_aes.cpp
/// @brief AESENC / AESENCLAST / AESDEC / AESDECLAST / AESIMC lowered inline
///        as ARMv8 FEAT_AES instructions instead of dispatching `jit_sse_op`.
///
/// Motivation. An x64 SSE-helper histogram (`VITREO_JIT_SSE_HIST=1`)
/// showed 16,777,216 `jit_sse_op` dispatches in ~90s, of which op_id 244
/// (AESDEC, register form) was 15,481,162 and op_id 246 (AESDECLAST, register
/// form) was 1,190,856 — 99.4% of all remaining helper traffic. Every one of
/// those paid a BLR, the pin spill/reload around it, and a walk down
/// `jit_sse_op`'s op_id chain, to perform work the host does in two
/// instructions.

namespace vitreo::cpu::jit {

// ===========================================================================
// DERIVATION — x86 AES-NI expressed in ARMv8 FEAT_AES
// ===========================================================================
//
// Sources: Intel SDM Vol. 2 (AESENC / AESENCLAST / AESDEC / AESDECLAST /
// AESIMC "Operation" sections) and the ARM ARM (AESE / AESD / AESMC / AESIMC
// pseudocode). Nothing here is taken from another emulator.
//
// ---- What x86 specifies -------------------------------------------------
//
//   AESENC xmm1, xmm2/m128            AESDEC xmm1, xmm2/m128
//     STATE := xmm1                     STATE := xmm1
//     RK    := xmm2/m128                RK    := xmm2/m128
//     STATE := ShiftRows(STATE)         STATE := InvShiftRows(STATE)
//     STATE := SubBytes(STATE)          STATE := InvSubBytes(STATE)
//     STATE := MixColumns(STATE)        STATE := InvMixColumns(STATE)
//     DEST  := STATE XOR RK             DEST  := STATE XOR RK
//
//   AESENCLAST / AESDECLAST are the same with the (Inv)MixColumns line
//   deleted.
//
//   AESIMC xmm1, xmm2/m128
//     DEST := InvMixColumns(SRC)        — the destination is NOT an input.
//
//   Note the shape: on x86 the round key is XORed LAST, and the destination
//   register supplies the state.
//
// ---- What ARMv8 specifies ------------------------------------------------
//
//   AESE  Vd.16B, Vn.16B
//     Vd := AESSubBytes(AESShiftRows(Vd EOR Vn))
//   AESD  Vd.16B, Vn.16B
//     Vd := AESInvSubBytes(AESInvShiftRows(Vd EOR Vn))
//   AESMC  Vd.16B, Vn.16B := AESMixColumns(Vn)
//   AESIMC Vd.16B, Vn.16B := AESInvMixColumns(Vn)
//
//   Note the shape: on ARM the round key is XORed FIRST, folded into AESE/AESD
//   (that is what makes the ARM instruction match FIPS-197's round structure,
//   where AddRoundKey opens the round), and MixColumns is a separate step.
//
// ---- Reconciling the two -------------------------------------------------
//
// Two differences must be handled, and only two:
//
//   (1) The XOR position. x86 XORs the key after the S-box/permute stage; ARM
//       XORs it before. They are NOT interchangeable — SubBytes is non-linear,
//       so SubBytes(x EOR k) != SubBytes(x) EOR k. The fix is to neutralise
//       ARM's built-in AddRoundKey by passing a ZERO key register, and then
//       perform x86's trailing XOR explicitly with EOR Vd,Vd,Vkey.
//
//       With Vn = 0:  AESE Vd, 0 = AESSubBytes(AESShiftRows(Vd)).
//
//   (2) The stage order. x86 AESENC lists ShiftRows before SubBytes; ARM AESE
//       also applies ShiftRows before SubBytes. (Even had they differed it
//       would not matter: ShiftRows is a pure byte permutation and SubBytes is
//       an independent per-byte substitution, so the two commute.) The same
//       holds for InvShiftRows/InvSubBytes in AESD. So the ARM pair is exactly
//       x86's first two lines, no reordering required.
//
//   MixColumns is not folded into ARM's AESE, so x86's third line becomes a
//   separate AESMC (AESIMC for the decrypt direction). That the ARM sequence
//   applies AESMC to the AESE result and x86 applies MixColumns to the
//   SubBytes result is the same composition read left to right.
//
// ---- Resulting sequences (V0 = state, V1 = key/src, V2 = zero) -----------
//
//   AESENC dst, src        AESDEC dst, src
//     V0 = xmm[dst]          V0 = xmm[dst]
//     V1 = src               V1 = src
//     EOR   V2,V2,V2         EOR   V2,V2,V2
//     AESE  V0,V2            AESD  V0,V2
//     AESMC V0,V0            AESIMC V0,V0
//     EOR   V0,V0,V1         EOR   V0,V0,V1
//     xmm[dst] = V0          xmm[dst] = V0
//
//   AESENCLAST / AESDECLAST: identical minus the AESMC / AESIMC line.
//
//   AESIMC dst, src
//     V1 = src ; AESIMC V0,V1 ; xmm[dst] = V0
//   (the destination is write-only here, so it is never loaded.)
//
// ---- Why this is safe ----------------------------------------------------
//
// * Both operands are read into V registers BEFORE the first write, so
//   `AESENC xmm0, xmm0` (dst == src) keeps x86's read-both-then-write order.
// * None of AESE/AESD/AESMC/AESIMC/EOR reads or writes NZCV, and AES-NI
//   defines no EFLAGS effect, so the surrounding lazy-flag bookkeeping is
//   untouched. There is no helper call, hence no pin spill and no NZCV spill.
// * The memory-source forms take a validated 128-bit guest load that delivers
//   into a V register and owns its own fault path; the destination XMM slot is
//   not written until after the load has succeeded, matching the interpreter,
//   which returns without touching DEST when either half of the read faults.
// * AESKEYGENASSIST is deliberately NOT lowered here — see the note at the
//   bottom of this file.
//
// ===========================================================================

namespace {

/// Per-opcode plan: which ARM AES primitive, whether a (Inv)MixColumns step
/// follows, and whether the destination is an input at all.
struct AesPlan {
    bool     valid{false};
    bool     decrypt{false};    ///< AESD instead of AESE
    bool     mix{false};        ///< AESMC / AESIMC after the AESE / AESD
    bool     imc_only{false};   ///< AESIMC: dst is write-only, no key XOR
};

AesPlan plan_for(Opcode opcode) noexcept {
    switch (opcode) {
    case Opcode::AESENC_XMM_XMMM128:     return {true, false, true,  false};
    case Opcode::AESENCLAST_XMM_XMMM128: return {true, false, false, false};
    case Opcode::AESDEC_XMM_XMMM128:     return {true, true,  true,  false};
    case Opcode::AESDECLAST_XMM_XMMM128: return {true, true,  false, false};
    case Opcode::AESIMC_XMM_XMMM128:     return {true, false, false, true};
    default:                             return {};
    }
}

// ARM V registers used by the lowering. V0-V7 are caller-saved, so nothing
// here may stay live across a helper call; the only call that can occur is
// inside the memory-source load, and that runs before V0/V2 are established.
constexpr u8 kVState = 0;
constexpr u8 kVKey   = 1;
constexpr u8 kVZero  = 2;

} // namespace

bool JitCompiler::emit_aes_direct(const DecodedInstruction& instr,
                                  u64 next_rip) {
    const AesPlan plan = plan_for(instr.opcode);
    if (!plan.valid) return false;

    // Host gate + VITREO_JIT_NO_AES_DIRECT opt-out (the control arm of the
    // before/after measurement, and the bisect against the helper).
    if (!host_supports_aes()) return false;

    // These opcodes have no VEX.256 form in the decoder, so a lane split must
    // never reach them; decline rather than silently rewrite the low lane.
    if (m_vec_lane_split) return false;

    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    const u8 dst_xmm = static_cast<u8>(op0.reg & 15);
    const u32 dst_off = xmm_u64_off(dst_xmm, 0);
    if (!fits_ldr_q_offset(dst_off)) return false;

    const usize mark = m_code_buf.size();

    // ---- Stage the source (round key, or the AESIMC input) into V1. -------
    if (is_register_operand(op1.type)) {
        const u8 src_xmm = static_cast<u8>(op1.reg & 15);
        const u32 src_off = xmm_u64_off(src_xmm, 0);
        if (!fits_ldr_q_offset(src_off)) return false;
        emit(arm64::ldr_q_uoff(kVKey, REG_CPU_STATE, src_off >> 4));
    } else {
        // A validated 128-bit guest load that lands in a V register. Only the
        // paths below deliver that; the remaining memory configurations (a
        // 32-bit block on non-linear memory) keep the checked helper, whose
        // fault behavior is already correct. The load runs FIRST because its
        // cold stub may call a helper, and a helper clobbers V0-V7.
        if (m_block_is_64bit) {
            if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned) {
                emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                emit_linear64_load128(kVKey, REG_SCRATCH4);
            } else {
                emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
                emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
                emit_x64_region_load128(kVKey, REG_SCRATCH4);
            }
        } else if (m_cfg.linear_jit && m_memory.is_linear()) {
            emit_compute_ea(REG_SCRATCH4, op1.mem, next_rip);
            emit_m128_align_check_if_required(REG_SCRATCH4, instr, op1);
            // Same shape as emit_sse_load_mem128_fast's 32-bit linear arm:
            // zero-extend the 32-bit EA onto the pinned linear base and read
            // 16 bytes; a bad address faults into the recoverable handler.
            emit(arm64::mov_w(9, REG_SCRATCH4));
            const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE
                                                 : REG_SCRATCH3;
            if (!m_linear_base_pinned) {
                emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                       JIT_CTX_OFF_LINEAR_BASE >> 3));
            }
            emit(arm64::add_x_uxtw(REG_SCRATCH3, base, 9));
            emit(arm64::ldr_q_uoff(kVKey, REG_SCRATCH3, 0));
            // Acquire: the guest load happened, so the fence follows it.
            emit_vector_tso_fence();
        } else {
            m_code_buf.resize(mark);
            return false;
        }
    }

    // ---- AESIMC: DEST := InvMixColumns(SRC); DEST is not an input. -------
    if (plan.imc_only) {
        emit(arm64::aesimc(kVState, kVKey));
        emit(arm64::str_q_uoff(kVState, REG_CPU_STATE, dst_off >> 4));
        return true;
    }

    // ---- The four round forms. -------------------------------------------
    emit(arm64::ldr_q_uoff(kVState, REG_CPU_STATE, dst_off >> 4));
    emit(arm64::eor_v16b(kVZero, kVZero, kVZero));      // neutral round key
    if (plan.decrypt) {
        emit(arm64::aesd(kVState, kVZero));             // InvSubBytes∘InvShiftRows
        if (plan.mix) emit(arm64::aesimc(kVState, kVState));
    } else {
        emit(arm64::aese(kVState, kVZero));             // SubBytes∘ShiftRows
        if (plan.mix) emit(arm64::aesmc(kVState, kVState));
    }
    emit(arm64::eor_v16b(kVState, kVState, kVKey));     // x86's trailing XOR
    emit(arm64::str_q_uoff(kVState, REG_CPU_STATE, dst_off >> 4));
    return true;
}

// ---------------------------------------------------------------------------
// AESKEYGENASSIST stays on the helper — deliberately.
//
// Intel SDM Vol. 2 AESKEYGENASSIST: the result is
//   DEST[31:0]   := SubWord(SRC[63:32])
//   DEST[63:32]  := RotWord(SubWord(SRC[63:32])) XOR RCON
//   DEST[95:64]  := SubWord(SRC[127:96])
//   DEST[127:96] := RotWord(SubWord(SRC[127:96])) XOR RCON
//
// ARMv8 has no SubWord/RotWord primitive. The only way to reach the AES S-box
// is AESE, which additionally applies ShiftRows to the whole 128-bit state, so
// an emulation costs a pre-permute (to place the two source dwords where
// ShiftRows will deposit them), an AESE against a zero key, a post-permute,
// a rotate and an RCON XOR — roughly a dozen instructions plus two constant
// pool loads, against a helper call that is already a straight-line table
// lookup.
//
// More decisively: AESKEYGENASSIST runs once per key schedule, not once per
// block. It is absent from the x64 helper histogram entirely, while AESDEC
// alone accounts for 15.5M dispatches. Lowering it natively would add a
// non-obvious permutation derivation to the crypto path for no measurable
// gain, so it keeps its existing checked helper.
// ---------------------------------------------------------------------------

} // namespace vitreo::cpu::jit

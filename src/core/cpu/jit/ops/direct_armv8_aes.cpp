// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo JIT — direct ARMv8 AES lowering for x86 AES-NI.

#include "../jit_helpers.hpp"

#include <sys/sysctl.h>

#include <cstddef>

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
//   into a V register and owns its own fault path (emit_crypto_src128, shared
//   with the SHA / PCLMULQDQ / AESKEYGENASSIST lowerings); the destination XMM
//   slot is not written until after the load has succeeded, matching the
//   interpreter, which returns without touching DEST when either half of the
//   read faults. The four memory configurations are linear64, the x64 region
//   table, 32-bit linear, and (WS3) 32-bit non-linear memory, where the two
//   8-byte halves go through the checked READ64 path into the JIT-private
//   jit_vec_scratch slot and only then into a V register: a fault on the
//   second half leaves every architectural register untouched.
// * VEX.128 (VAESENC/VAESENCLAST/VAESDEC/VAESDECLAST/VAESIMC, Intel SDM Vol.
//   2 and §2.3.5): SRC1 is VEX.vvvv instead of DEST, and DEST[MAXVL-1:128] is
//   zeroed after the store. All three operands are read before the first
//   store, so `vaesenc xmm1, xmm2, xmm1` and `vaesimc xmm1, xmm1` are exact.
//   VEX m128 operands have no alignment requirement.
// * AESKEYGENASSIST is lowered by emit_aes_keygen_direct at the bottom of
//   this file (its own derivation).
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

// ---- WS3 host gates (declared in stencils_arm64_crypto.hpp) ----
namespace {
bool host_arm_feature(const char* key) noexcept {
    int value = 0;
    std::size_t size = sizeof(value);
    if (::sysctlbyname(key, &value, &size, nullptr, 0) != 0) return false;
    return value != 0;
}
} // namespace

bool host_supports_sha1() noexcept {
    static const bool s = host_arm_feature("hw.optional.arm.FEAT_SHA1");
    return s;
}
bool host_supports_sha256() noexcept {
    static const bool s = host_arm_feature("hw.optional.arm.FEAT_SHA256");
    return s;
}
bool host_supports_pmull() noexcept {
    static const bool s = host_arm_feature("hw.optional.arm.FEAT_PMULL");
    return s;
}

bool JitCompiler::emit_crypto_src128(u8 vreg, const DecodedInstruction& instr,
                                     const Operand& src, u64 next_rip) {
    if (is_register_operand(src.type)) {
        const u32 src_off = xmm_u64_off(static_cast<u8>(src.reg & 15), 0);
        if (!fits_ldr_q_offset(src_off)) return false;
        emit(arm64::ldr_q_uoff(vreg, REG_CPU_STATE, src_off >> 4));
        return true;
    }

    // A validated 128-bit guest load that lands in a V register. The load
    // runs FIRST because its cold stub may call a helper, and a helper
    // clobbers V0-V7.
    if (m_block_is_64bit) {
        emit_compute_ea(REG_SCRATCH4, src.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, src);
        if (m_cfg.linear64 && m_cfg.linear64_vec128 && m_linear_base_pinned) {
            emit_linear64_load128(vreg, REG_SCRATCH4);
        } else {
            emit_x64_region_load128(vreg, REG_SCRATCH4);
        }
        return true;
    }
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        emit_compute_ea(REG_SCRATCH4, src.mem, next_rip);
        emit_m128_align_check_if_required(REG_SCRATCH4, instr, src);
        // Same shape as emit_sse_load_mem128_fast's 32-bit linear arm:
        // zero-extend the 32-bit EA onto the pinned linear base and read
        // 16 bytes; a bad address faults into the recoverable handler.
        emit(arm64::mov_w(9, REG_SCRATCH4));
        const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE : REG_SCRATCH3;
        if (!m_linear_base_pinned) {
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                   JIT_CTX_OFF_LINEAR_BASE >> 3));
        }
        emit(arm64::add_x_uxtw(REG_SCRATCH3, base, 9));
        emit(arm64::ldr_q_uoff(vreg, REG_SCRATCH3, 0));
        // Acquire: the guest load happened, so the fence follows it.
        emit_vector_tso_fence();
        return true;
    }

    // WS3: 32-bit block on non-linear memory. Two checked 8-byte reads
    // (emit_guest_load64 -> READ64 helper, which raises the access violation
    // and exits the block) staged into the JIT-private jit_vec_scratch slot,
    // the EA parked in its last qword across the helper calls. Only after
    // both halves succeeded does anything reach a V register, so a fault on
    // either half leaves DEST and YMM[255:128] untouched (the interpreter's
    // read_m128 order: low half, then high half, then the write).
    if (!ws3_crypto_direct_enabled()) return false;
    const u32 scratch = jit_vec_scratch_off();
    if (!fits_ldr_q_offset(scratch) || !fits_ldr_x_offset(scratch + 24)) {
        return false;
    }
    emit_compute_ea(REG_SCRATCH4, src.mem, next_rip);
    emit_m128_align_check_if_required(REG_SCRATCH4, instr, src);
    emit(arm64::str_x_uoff(REG_SCRATCH4, REG_CPU_STATE, (scratch + 24) >> 3));
    for (u32 half = 0; half < 16; half += 8) {
        emit(arm64::ldr_x_uoff(REG_SCRATCH2, REG_CPU_STATE, (scratch + 24) >> 3));
        // 32-bit add: the high half's EA+8 wraps mod 2^32 like any 32-bit
        // effective address (an unaligned VEX m128 at 0xFFFFFFFC).
        if (half != 0) emit(arm64::add_w_imm(REG_SCRATCH2, REG_SCRATCH2, half));
        emit_guest_load64(REG_SCRATCH0, REG_SCRATCH2);
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, (scratch + half) >> 3));
    }
    emit(arm64::ldr_q_uoff(vreg, REG_CPU_STATE, scratch >> 4));
    return true;
}

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

    const bool vex = instr.prefixes.vex;
    if (vex && (instr.prefixes.vex_l || !ws3_crypto_direct_enabled())) {
        return false;
    }

    const auto& op0 = instr.operands[0];
    const auto& op1 = instr.operands[1];
    const u8 dst_xmm = static_cast<u8>(op0.reg & 15);
    const u32 dst_off = xmm_u64_off(dst_xmm, 0);
    // SRC1: DEST for the legacy form, VEX.vvvv for the VEX.128 form.
    const u8 src1_xmm = vex ? static_cast<u8>(instr.prefixes.vex_vvvv & 15)
                            : dst_xmm;
    const u32 src1_off = xmm_u64_off(src1_xmm, 0);
    const u32 dhi_off = ymm_high_u64_off(dst_xmm, 0);
    if (!fits_ldr_q_offset(dst_off) || !fits_ldr_q_offset(src1_off) ||
        (vex && !fits_ldr_q_offset(dhi_off))) {
        return false;
    }

    const usize mark = m_code_buf.size();

    // ---- Stage the source (round key, or the AESIMC input) into V1. -------
    if (!emit_crypto_src128(kVKey, instr, op1, next_rip)) {
        m_code_buf.resize(mark);
        return false;
    }

    if (plan.imc_only) {
        // ---- AESIMC: DEST := InvMixColumns(SRC); DEST is not an input. ----
        emit(arm64::aesimc(kVState, kVKey));
    } else {
        // ---- The four round forms. ----------------------------------------
        emit(arm64::ldr_q_uoff(kVState, REG_CPU_STATE, src1_off >> 4));
        emit(arm64::eor_v16b(kVZero, kVZero, kVZero));  // neutral round key
        if (plan.decrypt) {
            emit(arm64::aesd(kVState, kVZero));         // InvSubBytes∘InvShiftRows
            if (plan.mix) emit(arm64::aesimc(kVState, kVState));
        } else {
            emit(arm64::aese(kVState, kVZero));         // SubBytes∘ShiftRows
            if (plan.mix) emit(arm64::aesmc(kVState, kVState));
        }
        emit(arm64::eor_v16b(kVState, kVState, kVKey)); // x86's trailing XOR
    }
    emit(arm64::str_q_uoff(kVState, REG_CPU_STATE, dst_off >> 4));
    if (vex) {
        // VEX.128: DEST[MAXVL-1:128] := 0.
        emit(arm64::eor_v16b(5, 5, 5));
        emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
    }
    return true;
}

// ---------------------------------------------------------------------------
// AESKEYGENASSIST xmm1, xmm2/m128, imm8 — AESE with a zero key, TBL, RCON.
//
// Intel SDM Vol. 2 AESKEYGENASSIST (X1 = SRC[63:32], X3 = SRC[127:96],
// RCON = ZeroExtend(imm8), RotWord(x) = x ROR 8):
//   DEST[31:0]   := SubWord(X1)
//   DEST[63:32]  := RotWord(SubWord(X1)) XOR RCON
//   DEST[95:64]  := SubWord(X3)
//   DEST[127:96] := RotWord(SubWord(X3)) XOR RCON
// So DEST byte k = S(SRC byte m(k)) with
//   m = [4,5,6,7, 5,6,7,4, 12,13,14,15, 13,14,15,12]
// and RCON XORed into bytes 4 and 12.
//
// ARM AESE V1, Vzero computes V1[i] = S(SRC[sr(i)]), where ShiftRows output
// byte i (column c = i/4, row r = i%4) reads input byte ((c+r) mod 4)*4 + r.
// Its inverse p(j) = ((j/4 - j%4) mod 4)*4 + j%4 says which AESE output byte
// holds S(SRC[j]). The TBL index for DEST byte k is therefore p(m(k)):
//   04 01 0E 0B  01 0E 0B 04  0C 09 06 03  09 06 03 0C
// (lo qword 0x040B0E010B0E0104, hi qword 0x0C0306090306090C). The S-box is the
// FIPS-197 one in both ISAs, so no table is carried here.
// ---------------------------------------------------------------------------
bool JitCompiler::emit_aes_keygen_direct(const DecodedInstruction& instr,
                                         u64 next_rip) {
    if (instr.opcode != Opcode::AESKEYGENASSIST_XMM_XMMM128_IMM8) return false;
    if (!host_supports_aes()) return false;
    if (m_vec_lane_split) return false;
    const bool vex = instr.prefixes.vex;
    if (vex && instr.prefixes.vex_l) return false;

    const u8 dst_xmm = static_cast<u8>(instr.operands[0].reg & 15);
    const u32 dst_off = xmm_u64_off(dst_xmm, 0);
    const u32 dhi_off = ymm_high_u64_off(dst_xmm, 0);
    if (!fits_ldr_q_offset(dst_off) || (vex && !fits_ldr_q_offset(dhi_off))) {
        return false;
    }
    const u8 imm8 = static_cast<u8>(instr.operands[2].imm);

    const usize mark = m_code_buf.size();
    if (!emit_crypto_src128(kVKey, instr, instr.operands[1], next_rip)) {
        m_code_buf.resize(mark);
        return false;
    }
    emit(arm64::eor_v16b(kVZero, kVZero, kVZero));
    emit(arm64::aese(kVKey, kVZero));                 // V1 = S(ShiftRows(SRC))
    emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(0x040B0E010B0E0104ULL));
    emit_mov_imm64(REG_SCRATCH1, static_cast<i64>(0x0C0306090306090CULL));
    emit(arm64::ins_v_gpr(3, 0, REG_SCRATCH0, arm64::VS_D));
    emit(arm64::ins_v_gpr(3, 1, REG_SCRATCH1, arm64::VS_D));
    emit(arm64::tbl_v16b_1(kVState, kVKey, 3));
    if (imm8 != 0) {
        emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(u64{imm8} << 32));
        emit(arm64::dup_v_gpr(4, REG_SCRATCH0, arm64::VS_D));
        emit(arm64::eor_v16b(kVState, kVState, 4));
    }
    emit(arm64::str_q_uoff(kVState, REG_CPU_STATE, dst_off >> 4));
    if (vex) {
        emit(arm64::eor_v16b(5, 5, 5));
        emit(arm64::str_q_uoff(5, REG_CPU_STATE, dhi_off >> 4));
    }
    return true;
}

} // namespace vitreo::cpu::jit

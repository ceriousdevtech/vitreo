// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_helpers.hpp
/// @brief Shared constants, offset helpers, and macros for JIT compilation.
///
/// Extracted from jit_compiler.cpp to allow ops/ files to share the
/// same register definitions, JitContext field offsets, and CpuState
/// offset accessors used during ARM64 code emission.

#include "jit_compiler.hpp"
#include "stencils.hpp"

namespace vitreo::cpu::jit {

// ============================================================================
// ARM64 register assignments
// ============================================================================

namespace jit_detail {

constexpr u8 REG_CPU_STATE = 19;   // X19 = CpuState*
constexpr u8 REG_JIT_CTX = 20;    // X20 = JitContext*
constexpr u8 REG_SCRATCH0 = 0;    // W0/X0 — first scratch
constexpr u8 REG_SCRATCH1 = 1;    // W1/X1
constexpr u8 REG_SCRATCH2 = 2;    // W2/X2
constexpr u8 REG_SCRATCH3 = 3;    // W3/X3
constexpr u8 REG_SCRATCH4 = 4;    // W4/X4
constexpr u8 REG_EA_TMP   = 5;    // W5 — dedicated temp for emit_compute_ea
// X22 is callee-saved, so helper calls cannot corrupt the block iteration
// count. Using caller-saved W6 here made profiles and instruction-count based
// scheduling depend on arbitrary helper scratch values.
constexpr u8 REG_LOOP_CTR = 22;   // W22/X22 — JIT loop iteration counter
constexpr u8 REG_CALL_TMP = 21;   // W21/X21 — callee-saved temp for indirect calls
// 32-bit ESP/EBP pins. X8/X9 are the dedicated guest-address scratches in
// emit_guest_load/store, so these live in unused caller-saved X12/X13 and
// are reloaded by emit_helper_blr().
constexpr u8 REG_PIN_ESP = 12;
constexpr u8 REG_PIN_EBP = 13;
// 32-bit linear reservation base. Caller-saved; emit_helper_blr reloads it.
constexpr u8 REG_LINEAR_BASE = 14;
// Host address linear_base + zext(ESP). Caller-saved; killed on helper BLR
// and on any write to ESP.
constexpr u8 REG_ESP_FRAME = 15;
// Address of write_watch::g_published_map for linear64 blocks. The 32-bit
// ESP-frame optimization above is unreachable in 64-bit blocks, so X15 can
// retain this live publication slot across linked native block entries.
// x15 pins the base of GuestMemory::linear_check_page_map(). It previously
// pinned the process-global write-watch published-map slot; that test is now
// kLinearCheckWatched, one bit of the byte this map holds, so the register pays
// for itself on every access instead of only while a watch is live.
constexpr u8 REG_LINEAR64_CHECK_MAP = 15;

// JitContext field offsets
constexpr u32 JIT_CTX_OFF_MEMORY    = 0;
constexpr u32 JIT_CTX_OFF_READ32    = 8;
constexpr u32 JIT_CTX_OFF_WRITE32   = 16;
constexpr u32 JIT_CTX_OFF_EVAL_COND = 24;
constexpr u32 JIT_CTX_OFF_READ8     = 32;
constexpr u32 JIT_CTX_OFF_WRITE8    = 40;
constexpr u32 JIT_CTX_OFF_READ16    = 48;
constexpr u32 JIT_CTX_OFF_WRITE16   = 56;
constexpr u32 JIT_CTX_OFF_REGION_BASE = 64;
constexpr u32 JIT_CTX_OFF_REGION_END  = 68;
constexpr u32 JIT_CTX_OFF_REGION_HOST = 72;
constexpr u32 JIT_CTX_OFF_REGION2_BASE = 80;
constexpr u32 JIT_CTX_OFF_REGION2_END  = 84;
constexpr u32 JIT_CTX_OFF_REGION2_HOST = 88;
constexpr u32 JIT_CTX_OFF_REP_MOVSD  = 96;
constexpr u32 JIT_CTX_OFF_REP_STOSD  = 104;
constexpr u32 JIT_CTX_OFF_MATERIALIZE = 112;
constexpr u32 JIT_CTX_OFF_CMPXCHG8B  = 120;
constexpr u32 JIT_CTX_OFF_FPU_OP    = 128;
constexpr u32 JIT_CTX_OFF_SSE_OP    = 136;
constexpr u32 JIT_CTX_OFF_CMPXCHG32 = 144;
constexpr u32 JIT_CTX_OFF_SHLD32   = 152;
constexpr u32 JIT_CTX_OFF_READ64   = 160;
constexpr u32 JIT_CTX_OFF_WRITE64  = 168;
constexpr u32 JIT_CTX_OFF_REP_MOVSB = 176;
constexpr u32 JIT_CTX_OFF_LOCK_INC32     = 184;
constexpr u32 JIT_CTX_OFF_LOCK_DEC32     = 192;
constexpr u32 JIT_CTX_OFF_LOCK_XADD32    = 200;
constexpr u32 JIT_CTX_OFF_LOCK_CMPXCHG32 = 208;
constexpr u32 JIT_CTX_OFF_LOCK_CMPXCHG8B = 216;
constexpr u32 JIT_CTX_OFF_FUSED_MEMCPY   = 224;
constexpr u32 JIT_CTX_OFF_SHRD32         = 232;
constexpr u32 JIT_CTX_OFF_INTERP_STEP    = 240;
constexpr u32 JIT_CTX_OFF_REP_STOSB      = 248;
constexpr u32 JIT_CTX_OFF_REP_STOSW      = 256;
constexpr u32 JIT_CTX_OFF_REP_MOVSW      = 264;
constexpr u32 JIT_CTX_OFF_REPE_CMPSB     = 272;
constexpr u32 JIT_CTX_OFF_REPNE_SCASB    = 280;
constexpr u32 JIT_CTX_OFF_L1_BASE        = 288;  // Stage B1: BlockCache::m_l1 base
constexpr u32 JIT_CTX_OFF_COND_FNS       = 320;  // u32(*)(CpuState*) [16] — specialized cc helpers
constexpr u32 JIT_CTX_OFF_ATOMIC_XCHG32  = 616;
constexpr u32 JIT_CTX_OFF_LOCK_OR32      = 624;
constexpr u32 JIT_CTX_OFF_LOCK_AND32     = 632;
constexpr u32 JIT_CTX_OFF_LOCK_ADD32     = 640;
constexpr u32 JIT_CTX_OFF_LOCK_SUB32     = 648;
/// LOCK CMPXCHG r/m8, r8 — a UI-thread work-flag byte.
constexpr u32 JIT_CTX_OFF_LOCK_CMPXCHG8  = 656;
constexpr u32 JIT_CTX_OFF_LOCK_OR16      = 664;
constexpr u32 JIT_CTX_OFF_LOCK_AND16     = 672;
constexpr u32 JIT_CTX_OFF_FPU_PEEK_M32   = 680;
constexpr u32 JIT_CTX_OFF_FPU_POP        = 688;
// Inline store fast-path regions. Only ever published when the region is
// writable and free of compiled code, so emitted stores need no permission
// test and no SMC notification. See JitContext for the full invariant.
constexpr u32 JIT_CTX_OFF_WREGION_BASE   = 448;
constexpr u32 JIT_CTX_OFF_WREGION_END    = 452;
constexpr u32 JIT_CTX_OFF_WREGION_HOST   = 456;
constexpr u32 JIT_CTX_OFF_WREGION2_BASE  = 464;
constexpr u32 JIT_CTX_OFF_WREGION2_END   = 468;
constexpr u32 JIT_CTX_OFF_WREGION2_HOST  = 472;
constexpr u32 JIT_CTX_OFF_LINEAR_BASE    = 480;  // u8* — guest-base host pointer
constexpr u32 JIT_CTX_OFF_CODE_SPAN_LO   = 488;  // u32 — compiled-code span low
constexpr u32 JIT_CTX_OFF_CODE_SPAN_HI   = 492;  // u32 — compiled-code span high
constexpr u32 JIT_CTX_OFF_CAPTURE_CF     = 496;  // void (*)(CpuState*)
constexpr u32 JIT_CTX_OFF_ZERO_FILL_LOOP = 504;  // void (*)(CpuState*, ...)
constexpr u32 JIT_CTX_OFF_INTERP_DECODED = 512;  // void (*)(CpuState*, GuestMemory*, DecodedInstruction*, u64)
constexpr u32 JIT_CTX_OFF_X64_REGION_BASE = 520;
constexpr u32 JIT_CTX_OFF_X64_REGION_END  = 528;
constexpr u32 JIT_CTX_OFF_X64_REGION_HOST = 536;
constexpr u32 JIT_CTX_OFF_X64_REGION2_BASE = 544;
constexpr u32 JIT_CTX_OFF_X64_REGION2_END  = 552;
constexpr u32 JIT_CTX_OFF_X64_REGION2_HOST = 560;
constexpr u32 JIT_CTX_OFF_X64_READ16_REGION_BASE = 568;
constexpr u32 JIT_CTX_OFF_X64_READ16_REGION_END  = 576;
constexpr u32 JIT_CTX_OFF_X64_READ16_REGION_HOST = 584;
constexpr u32 JIT_CTX_OFF_X64_READ16_REGION2_BASE = 592;
constexpr u32 JIT_CTX_OFF_X64_READ16_REGION2_END  = 600;
constexpr u32 JIT_CTX_OFF_X64_READ16_REGION2_HOST = 608;
// Long-mode inline store region — writable and code-free by construction.
constexpr u32 JIT_CTX_OFF_X64_WREGION_BASE = 696;
constexpr u32 JIT_CTX_OFF_X64_WREGION_END  = 704;
constexpr u32 JIT_CTX_OFF_X64_WREGION_HOST = 712;
constexpr u32 JIT_CTX_OFF_WRITE_WATCH_PAGES = 720;
constexpr u32 JIT_CTX_OFF_REPE_CMPSW      = 728;
constexpr u32 JIT_CTX_OFF_REPE_CMPSD      = 736;
constexpr u32 JIT_CTX_OFF_REPNE_CMPSW     = 744;
constexpr u32 JIT_CTX_OFF_REPNE_CMPSD     = 752;
constexpr u32 JIT_CTX_OFF_REPE_SCASW      = 760;
constexpr u32 JIT_CTX_OFF_REPE_SCASD      = 768;
constexpr u32 JIT_CTX_OFF_REPNE_SCASW     = 776;
constexpr u32 JIT_CTX_OFF_REPNE_SCASD     = 784;
constexpr u32 JIT_CTX_OFF_REPE_SCASB      = 792;
constexpr u32 JIT_CTX_OFF_REPNE_CMPSB     = 800;
constexpr u32 JIT_CTX_OFF_PDEP64          = 808;
constexpr u32 JIT_CTX_OFF_PEXT64          = 816;

// Maximum loop iterations before forcing JIT exit. String helpers chunk
// REP MOVS/STOS at this size. In-block Jcc loop-backs use the tighter
// MAX_IN_BLOCK_LOOP_ITERS so x87-heavy loops return to the dispatcher.
constexpr u32 MAX_JIT_LOOP_ITERS = 4095;
/// Native in-block `Jcc` loop-back cap. Safety valve so a tight loop still
/// returns to the dispatcher for the 16 ms GUI pump — not a per-MCU budget.
/// `movz_w` encodes a 16-bit immediate; 65535 lets a decompress/load loop
/// run ~2000× longer than the old JPEG-oriented cap of 32.
constexpr u32 MAX_IN_BLOCK_LOOP_ITERS = 65535;

// Offsets into CpuState — gpr is the first field: GeneralRegs64 with r[16]
constexpr u32 OFF_GPR_BASE = 0;

inline u32 gpr_off(u8 x86_idx) noexcept {
    return OFF_GPR_BASE + static_cast<u32>(x86_idx) * 8;
}

inline u32 rip_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, rip));
}

inline u32 flag_op_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, flag_op));
}

/// Offset of the aligned word holding {pad, flag_op, cf_overlay, of_overlay}.
/// Publishing a lazy-flag record writes all four with ONE store instead of a
/// STRB plus a STRH; see the layout note on CpuState for why the store count,
/// not the instruction count, is what is being minimised here.
inline u32 flag_word_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, flag_word_pad));
}

/// The value that word takes for a full flag writer publishing `op`: the op
/// byte in its lane, both overlays cleared, pad zero. Little-endian, and the
/// static_asserts at the use site pin every lane.
inline u32 flag_word_value(CpuState::FlagOp op) noexcept {
    return static_cast<u32>(op) << 8;
}

inline u32 cf_overlay_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, cf_overlay));
}

inline u32 of_overlay_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, of_overlay));
}

inline u32 nzcv_kind_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, nzcv_kind));
}

inline u32 nzcv_bits_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, nzcv_bits));
}

inline u32 flag_src_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, flag_src));
}

inline u32 flag_dst_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, flag_dst));
}

// The lazy-flag record is published in two stores, and both need a property
// of this layout rather than merely a reachable offset. See the note on
// CpuState: the store COUNT is what costs, priced at ~9.9% of sc_blocksize
// bsz1 per store (jit-refactor/FLAG-PUBLICATION.md §3).
static_assert(offsetof(CpuState, flag_dst) ==
                  offsetof(CpuState, flag_src) + sizeof(u64),
              "flag_src/flag_dst must be adjacent so one STP publishes both");
static_assert((offsetof(CpuState, flag_src) & 7) == 0,
              "flag_src must be 8-aligned for STP");
static_assert((offsetof(CpuState, flag_src) & 15) == 0,
              "flag_src must be 16-aligned so a 32-bit publisher can STR Q "
              "the {flag_src, flag_dst} pair without a second GPR scratch");
static_assert(offsetof(CpuState, flag_src) / sizeof(u64) <= 63,
              "flag_src must stay within STP's signed imm7 (scaled by 8) of "
              "the CpuState base -- keep the lazy-flag block ahead of fpu/sse");
static_assert((offsetof(CpuState, flag_src) >> 4) < 4096,
              "flag_src must fit STR Q unsigned offset");
static_assert((offsetof(CpuState, flag_word_pad) & 3) == 0,
              "the {pad, flag_op, cf_overlay, of_overlay} word must be "
              "4-aligned so one STR W publishes it");
static_assert(offsetof(CpuState, nzcv_kind) < 4096,
              "nzcv_kind must fit LDRB/STRB unsigned offset");
static_assert(offsetof(CpuState, nzcv_kind) > offsetof(CpuState, of_overlay) + 1,
              "nzcv_kind must NOT share the published flag word, or every "
              "publication would silently clear the carried-NZCV fast path");
static_assert((offsetof(CpuState, nzcv_bits) & 3) == 0,
              "nzcv_bits must be 4-byte aligned for STR W");
static_assert((offsetof(CpuState, nzcv_bits) >> 2) < 4096,
              "nzcv_bits must fit ldr_w_uoff / str_w_uoff");

inline u32 cpu_rflags_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, flags));
}

inline u32 jit_iters_offset() noexcept {
    return static_cast<u32>(offsetof(CpuState, jit_iters));
}

inline u32 sp_offset() noexcept {
    return gpr_off(static_cast<u8>(RegIndex::RSP));
}

inline u32 seg_base_offset(SegReg seg) noexcept {
    switch (seg) {
    case SegReg::ES: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, es_desc));
    case SegReg::CS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, cs_desc));
    case SegReg::SS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, ss_desc));
    case SegReg::DS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, ds_desc));
    case SegReg::FS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, fs_desc));
    case SegReg::GS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, gs_desc));
    default: return 0;
    }
}

/// Byte offset of a segment SELECTOR within CpuState.
///
/// Distinct from `seg_base_offset`: the selector is the 16-bit value a
/// `PUSH FS` stores, while the base is the cached descriptor field an address
/// calculation adds. Pushing the base would store a linear address where the
/// guest expects a selector.
inline u32 seg_selector_offset(SegReg seg) noexcept {
    switch (seg) {
    case SegReg::ES: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, es));
    case SegReg::CS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, cs));
    case SegReg::SS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, ss));
    case SegReg::DS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, ds));
    case SegReg::FS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, fs));
    case SegReg::GS: return static_cast<u32>(offsetof(CpuState, seg) + offsetof(SegmentRegs, gs));
    default: return 0;
    }
}

/// Byte offset of MMX register MMn's 64-bit mantissa within CpuState.
/// MMX aliases the x87 stack: MMn == the low 64 bits (mantissa) of st[n].
/// Float80 is packed to 10 bytes, so these offsets are NOT 8-aligned for
/// odd n — callers must form the address with ADD + an unaligned load/store
/// rather than a scaled uoff load.
inline u32 mm_mantissa_off(u8 mm_idx) noexcept {
    return static_cast<u32>(offsetof(CpuState, fpu)
        + offsetof(FpuState, st)
        + static_cast<u32>(mm_idx & 7) * sizeof(Float80)
        + offsetof(Float80, mantissa));
}

/// Byte offset of MMX register MMn's x87 exponent/sign halfword. Writing an
/// MM register sets this to 0xFFFF (the "MMX in use" tag) — see
/// FpuState::set_mm; any stencil that writes MMn MUST replicate that or the
/// x87/MMX tag state diverges from the interpreter.
inline u32 mm_tag_off(u8 mm_idx) noexcept {
    return static_cast<u32>(offsetof(CpuState, fpu)
        + offsetof(FpuState, st)
        + static_cast<u32>(mm_idx & 7) * sizeof(Float80)
        + offsetof(Float80, exponent_sign));
}

/// Byte offset of the JIT's private 16-byte staging slot (SseState::
/// jit_vec_scratch). Used only by emit_x64_region_load128/store128 when the
/// region probe misses and the access must be split into two 8-byte helper
/// calls; not architectural guest state.
inline u32 jit_vec_scratch_off() noexcept {
    return static_cast<u32>(offsetof(CpuState, sse)
                            + offsetof(SseState, jit_vec_scratch));
}

inline u32 xmm_u64_off(u8 xmm_idx, u8 lane) noexcept {
    return static_cast<u32>(offsetof(CpuState, sse)
        + offsetof(SseState, xmm)
        + static_cast<u32>(xmm_idx & 15) * sizeof(XmmReg)
        + static_cast<u32>(lane & 1) * sizeof(u64));
}

/// Byte offset of YMM[idx][127:0] stored in AvxState::ymm_high (Intel SDM
/// Vol. 1: YMM = XMM || ymm_high).
inline u32 ymm_high_u64_off(u8 xmm_idx, u8 lane) noexcept {
    return static_cast<u32>(offsetof(CpuState, avx)
        + offsetof(AvxState, ymm_high)
        + static_cast<u32>(xmm_idx & 15) * sizeof(XmmReg)
        + static_cast<u32>(lane & 1) * sizeof(u64));
}

inline bool fits_uimm12(u32 val) noexcept { return val < 4096; }

// Every REG_CPU_STATE-relative immediate the JIT emits -- `ADD Xd, X19, #off`
// and the byte-scaled LDRB/STRB [X19, #off] -- assumes a field offset below
// 4096, which holds for every field exactly while the whole CpuState fits the
// 12-bit field. The arm64 encoders abort on an overflow at emission; this
// moves that failure to compile time for the struct as a whole.
static_assert(sizeof(CpuState) <= 4096,
              "CpuState outgrew the ARM64 12-bit immediate: REG_CPU_STATE-"
              "relative ADD #imm12 / LDRB [X19, #imm12] sites need a split");

// REG_JIT_CTX is already past 4 KiB (4360 bytes on 2026-10-11), so its sites
// rely on the scale instead: 16380 = 4095 * 4 is the largest offset the
// smallest-scaled form in use (LDR/STR W, imm12 * 4) can reach, and X/Q reach
// further. Byte and halfword loads and `ADD Xd, X20, #off` reach only 4095, so
// every such REG_JIT_CTX site must keep its explicit range guard (the RSB and
// sticky-ring ADDs in stage_b1_inline_block_link.cpp do).
static_assert(sizeof(JitContext) <= 16380,
              "JitContext outgrew the reach of LDR/STR W [X20, #imm12*4]");

inline bool fits_ldr_w_offset(u32 byte_offset) noexcept {
    return (byte_offset & 3) == 0 && (byte_offset >> 2) < 4096;
}

inline bool fits_ldr_x_offset(u32 byte_offset) noexcept {
    return (byte_offset & 7) == 0 && (byte_offset >> 3) < 4096;
}

/// Quad-word (128-bit) LDR/STR Qt uimm12 is scaled by 16.
inline bool fits_ldr_q_offset(u32 byte_offset) noexcept {
    return (byte_offset & 15) == 0 && (byte_offset >> 4) < 4096;
}

/// Map a Jcc opcode to its x86 condition code (0x0–0xF).
inline u8 jcc_condition_code(Opcode op) noexcept {
    switch (op) {
    case Opcode::JO_REL8: case Opcode::JO_REL32: return 0x0;
    case Opcode::JNO_REL8: case Opcode::JNO_REL32: return 0x1;
    case Opcode::JB_REL8: case Opcode::JB_REL32: return 0x2;
    case Opcode::JNB_REL8: case Opcode::JNB_REL32: return 0x3;
    case Opcode::JZ_REL8: case Opcode::JZ_REL32: return 0x4;
    case Opcode::JNZ_REL8: case Opcode::JNZ_REL32: return 0x5;
    case Opcode::JBE_REL8: case Opcode::JBE_REL32: return 0x6;
    case Opcode::JNBE_REL8: case Opcode::JNBE_REL32: return 0x7;
    case Opcode::JS_REL8: case Opcode::JS_REL32: return 0x8;
    case Opcode::JNS_REL8: case Opcode::JNS_REL32: return 0x9;
    case Opcode::JP_REL8: case Opcode::JP_REL32: return 0xA;
    case Opcode::JNP_REL8: case Opcode::JNP_REL32: return 0xB;
    case Opcode::JL_REL8: case Opcode::JL_REL32: return 0xC;
    case Opcode::JNL_REL8: case Opcode::JNL_REL32: return 0xD;
    case Opcode::JLE_REL8: case Opcode::JLE_REL32: return 0xE;
    case Opcode::JNLE_REL8: case Opcode::JNLE_REL32: return 0xF;
    default: return 0xFF;
    }
}

} // namespace jit_detail

// Pull into enclosing namespace for convenient use in ops/ files.
using namespace jit_detail;

/// Install a main-thread GUI pump invoked from long-running JIT helpers
/// (x87). `fn` may be null. Thread-local; nest-safe via the caller.
void jit_install_gui_pump(void (*fn)()) noexcept;
void jit_poll_gui_pump_throttled() noexcept;

// ============================================================================
// SSE helper histogram controls (VITREO_JIT_SSE_HIST)
// ============================================================================

/// The environment variable sets the initial state; these override it so a
/// caller can sample a known window instead of the whole process.
void jit_set_sse_helper_histogram_enabled(bool enabled) noexcept;
void jit_reset_sse_helper_histogram() noexcept;

/// Print the [jit-av] access-violation dump recorded by the last helper raise
/// on this thread, if any, and disarm it.
///
/// The dump is deferred rather than printed inside the helper because
/// emit_memory_helper_blr() leaves RAX/RCX/RDX/RBX/RSI/RDI in their
/// AAPCS64-callee-saved pins across a pure memory helper. CpuState becomes
/// authoritative for all sixteen GPRs only after the raise-exit stub stores
/// the pins, so JitCompiler::execute() calls this once the block has returned.
void jit_flush_pending_av_dump();

/// Raw details of the last hardware fault that `linear_fault` turned into a
/// guest access violation on this thread.
///
/// The signal handler cannot report, and by the time the run loop declares an
/// exception unhandled the guest context has already been rewritten from the
/// faulting site's metadata. Without the host PC and whether that metadata was
/// found, a report cannot say whether the guest RIP and registers it prints
/// are attributed or merely stale.
struct LinearFaultDetail {
    u64 host_pc;        ///< Interrupted host PC inside the JIT block.
    u64 host_addr;      ///< Faulting host address as delivered by the signal.
    u64 window_base;    ///< Reservation window the address was resolved against.
    u64 guest_addr;     ///< host_addr - window_base.
    u64 guest_rip;      ///< Guest RIP attributed to host_pc, 0 if unattributed.
    bool have_metadata; ///< Whether a JIT site described host_pc.
    bool is_write;
    bool valid;
};

/// Fetch the last recorded linear fault detail for this thread.
[[nodiscard]] bool jit_last_linear_fault(LinearFaultDetail& out);

/// Defined in jit_compiler.cpp: revalidate this thread's memory translation
/// caches before a scalar memory helper resolves its access.
void jit_revalidate_memory_caches(JitContext* ctx) noexcept;

// ============================================================================
// Extern "C" helper function declarations (defined in jit_helpers.cpp)
// ============================================================================

extern "C" {
u32 jit_mem_read32(JitContext* ctx, GuestAddr addr);
void jit_mem_write32(JitContext* ctx, GuestAddr addr, u32 val);
u64 jit_mem_read64(JitContext* ctx, GuestAddr addr);
void jit_mem_write64(JitContext* ctx, GuestAddr addr, u64 val);
void jit_mem_write_span(JitContext* ctx, GuestAddr addr, const u8* lo,
                        const u8* hi, u64 size);
u32 jit_mem_read8(JitContext* ctx, GuestAddr addr);
void jit_mem_write8(JitContext* ctx, GuestAddr addr, u32 val);
u32 jit_mem_read16(JitContext* ctx, GuestAddr addr);
void jit_mem_write16(JitContext* ctx, GuestAddr addr, u32 val);
void jit_materialize_flags(CpuState* cpu);

/// Fold the SSE class's pending sticky exception bits into CpuState::sse.mxcsr.
/// Called by the STMXCSR and FXSAVE lowerings — the points where MXCSR becomes
/// guest-visible. See core/math/host_fenv.hpp for why the flags are lazy.
void jit_publish_mxcsr(CpuState* cpu);

/// Drop the SSE class's pending flags (LDMXCSR supersedes them).
void jit_discard_mxcsr_exceptions(CpuState* cpu);
u64 jit_adc64(CpuState* cpu, u64 dst, u64 src);
u64 jit_sbb64(CpuState* cpu, u64 dst, u64 src);
/// MUL r/m64 — RDX:RAX := RAX * src; CF=OF=(high != 0). Host __int128 path;
/// the in-block UMULH lowering miscomputed products under concurrent guest
/// threads (RSA verify flakes).
void jit_mul_rm64(CpuState* cpu, u64 src);
/// DIV r/m64 — unsigned RDX:RAX / src → RAX rem RDX. Returns 0 on success;
/// non-zero if #DE (zero divisor or quotient does not fit in 64 bits). Does
/// not commit RAX/RDX on failure.
int jit_div_rm64(CpuState* cpu, u64 src);
/// IDIV r/m64 — signed RDX:RAX / src → RAX rem RDX. Same return contract.
int jit_idiv_rm64(CpuState* cpu, u64 src);
/// Raise pending STATUS_INTEGER_DIVIDE_BY_ZERO at instr_rip (#DE).
void jit_raise_divide_error(CpuState* cpu, u64 instr_rip);
/// NT_TIB.StackLimit (GS:[0x10]) probe before a CALL return-address push.
void jit_require_stack_limit(CpuState* cpu, GuestMemory* memory,
                             u64 next_rsp) noexcept;
/// CVTSI2SS xmm, r64 — write float32(src) into xmm lane 0; upper 96 bits kept.
void jit_cvtsi2ss_r64(CpuState* cpu, u32 xmm_dst, u64 src);
/// CVTSI2SD xmm, r32 — write float64(src) into xmm low qword; high qword kept.
void jit_cvtsi2sd_r32(CpuState* cpu, u32 xmm_dst, u32 src);
/// CVTSI2SD xmm, r64 — write float64(src) into xmm low qword; high qword kept.
void jit_cvtsi2sd_r64(CpuState* cpu, u32 xmm_dst, u64 src);
u32 jit_capture_cf(CpuState* cpu);
u32 jit_zero_fill_loop(CpuState* cpu, GuestMemory* mem,
                       u32 dst_reg, u32 count_reg,
                       u32 branch_rip, u32 fallthrough_rip);
void jit_rep_movsd(CpuState* cpu, GuestMemory* mem);
void jit_rep_movsb(CpuState* cpu, GuestMemory* mem);
void jit_rep_movsw(CpuState* cpu, GuestMemory* mem);
void jit_rep_movsq(CpuState* cpu, GuestMemory* mem);
void jit_rep_stosd(CpuState* cpu, GuestMemory* mem);
void jit_rep_stosb(CpuState* cpu, GuestMemory* mem);
void jit_rep_stosw(CpuState* cpu, GuestMemory* mem);
void jit_rep_stosq(CpuState* cpu, GuestMemory* mem);
void jit_rep_lodsb(CpuState* cpu, GuestMemory* mem);
void jit_rep_lodsw(CpuState* cpu, GuestMemory* mem);
void jit_rep_lodsd(CpuState* cpu, GuestMemory* mem);
void jit_repe_cmpsb(CpuState* cpu, GuestMemory* mem);
void jit_repe_cmpsw(CpuState* cpu, GuestMemory* mem);
void jit_repe_cmpsd(CpuState* cpu, GuestMemory* mem);
void jit_repe_cmpsq(CpuState* cpu, GuestMemory* mem);
void jit_repne_cmpsb(CpuState* cpu, GuestMemory* mem);

/// PDEP / PEXT (Intel SDM Vol. 2). No ARM64 equivalent exists, so these are
/// called rather than open-coded. Neither touches EFLAGS. The 32-bit forms
/// pass zero-extended values and take the low 32 bits of the result.
u64 jit_pdep64(u64 src, u64 mask);
u64 jit_pext64(u64 src, u64 mask);
void jit_repne_cmpsw(CpuState* cpu, GuestMemory* mem);
void jit_repne_cmpsd(CpuState* cpu, GuestMemory* mem);
void jit_repne_cmpsq(CpuState* cpu, GuestMemory* mem);
void jit_repne_scasb(CpuState* cpu, GuestMemory* mem);
void jit_repe_scasb(CpuState* cpu, GuestMemory* mem);
void jit_repe_scasw(CpuState* cpu, GuestMemory* mem);
void jit_repe_scasd(CpuState* cpu, GuestMemory* mem);
void jit_repe_scasq(CpuState* cpu, GuestMemory* mem);
void jit_repne_scasw(CpuState* cpu, GuestMemory* mem);
void jit_repne_scasd(CpuState* cpu, GuestMemory* mem);
void jit_repne_scasq(CpuState* cpu, GuestMemory* mem);
u32 jit_eval_condition(CpuState* cpu, u32 cond_code);
// Specialized per-cc helpers (see cond_fns[] in JitContext).
u32 jit_cond_o   (CpuState* cpu);
u32 jit_cond_no  (CpuState* cpu);
u32 jit_cond_b   (CpuState* cpu);
u32 jit_cond_nb  (CpuState* cpu);
u32 jit_cond_z   (CpuState* cpu);
u32 jit_cond_nz  (CpuState* cpu);
u32 jit_cond_be  (CpuState* cpu);
u32 jit_cond_nbe (CpuState* cpu);
u32 jit_cond_s   (CpuState* cpu);
u32 jit_cond_ns  (CpuState* cpu);
u32 jit_cond_p   (CpuState* cpu);
u32 jit_cond_np  (CpuState* cpu);
u32 jit_cond_l   (CpuState* cpu);
u32 jit_cond_nl  (CpuState* cpu);
u32 jit_cond_le  (CpuState* cpu);
u32 jit_cond_nle (CpuState* cpu);
void jit_cmpxchg8b(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_fpu_op(CpuState* cpu, GuestMemory* mem, u32 opcode_id,
                u64 addr);
JitFpuStoreResult jit_fpu_peek_m32(CpuState* cpu);
JitFpuStoreResult jit_fpu_peek_m64(CpuState* cpu);
JitFpuStoreResult jit_fpu_peek_int(CpuState* cpu, u32 packed);
void jit_fpu_pop(CpuState* cpu);
void jit_fpu_math(CpuState* cpu, u32 opcode);
// Host-FPU bridge (core/math/host_fenv.hpp). The native lowerings run x87 and
// SSE arithmetic on the host unit, so the host is where the guest's rounding
// mode has to be installed and where its sticky exception flags accumulate.
// The interpreter calls math::apply_x87_rounding / harvest / clear directly;
// a JIT stencil cannot, so it calls these.
//
// jit_fpu_harvest: fold the host's accumulated exceptions into fpu.status_word
//   (FNSTENV / FNSAVE -- the environment reads, which are cold).
// jit_fpu_clear_host: drop them without reporting (FNCLEX / FNINIT).
// jit_fpu_apply_rounding: point the host unit at fpu.control_word's RC field
//   (FLDCW / FLDENV / FRSTOR / FNINIT). Memoised, so a redundant call is free.
void jit_fpu_harvest(CpuState* cpu);
void jit_fpu_clear_host(CpuState* cpu);
void jit_fpu_apply_rounding(CpuState* cpu);
void jit_fpu_apply_loaded_control_word(CpuState* cpu);
void jit_fpu_deliver_pending(CpuState* cpu);
void jit_sse_apply_rounding(CpuState* cpu);
void jit_fnsave(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_frstor(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_sse_cvt(CpuState* cpu, GuestMemory* mem, u32 kind, u32 dst, u64 src);
u32 jit_shld32(CpuState* cpu, u32 dest, u32 src, u32 count);
u32 jit_shrd32(CpuState* cpu, u32 dest, u32 src, u32 count);
void jit_sse_op(CpuState* cpu, GuestMemory* mem,
                u32 op_id, u32 xmm_dst, u64 addr_or_src);
void jit_cmpxchg32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_val);
void jit_atomic_xchg32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_idx);
void jit_atomic_xchg64(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_idx);
void jit_lock_inc32(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_dec32(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_xadd32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_idx);
void jit_lock_cmpxchg32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_val);
void jit_lock_cmpxchg8b(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_cmpxchg16b(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_cmpxchg8(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_val);
void jit_lock_or32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 mask);
void jit_lock_and32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 mask);
void jit_lock_add32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 addend);
void jit_lock_sub32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 subtrahend);
// 32-bit twins of the r/m64 forms below. Their absence, not any semantic
// difference, is why LOCK XOR/NOT/NEG r/m32 reached the interpreter delegate
// and took the process-wide exclusive mutex. Immediate BLR addresses, so no
// JitContext slots are added.
void jit_lock_xor32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 mask);
void jit_lock_not32(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_neg32(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_or16(CpuState* cpu, GuestMemory* mem, u64 addr, u32 mask);
void jit_lock_and16(CpuState* cpu, GuestMemory* mem, u64 addr, u32 mask);
// LOCK r/m8 / r/m16 (jit_helpers_lock_narrow.cpp). jit_lock_alu8/16 take
// the source value in W3 and an operation selector in W4 (see compile_locked).
// The r8 helpers take a register code: bits 3:0 the register, bit 8 set for
// a legacy high-byte register (AH/CH/DH/BH).
void jit_lock_alu8(CpuState* cpu, GuestMemory* mem, u64 addr, u32 operand, u32 op);
void jit_lock_alu16(CpuState* cpu, GuestMemory* mem, u64 addr, u32 operand, u32 op);
void jit_lock_xadd8(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_code);
void jit_lock_xadd16(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_idx);
void jit_atomic_xchg8(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_code);
void jit_atomic_xchg16(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_idx);
void jit_lock_cmpxchg16(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_val);
// LOCK r/m64 helpers (Intel SDM Vol. 2 LOCK) — immediate BLR addresses, no
// extra JitContext slots (same pattern as jit_lock_cmpxchg16b).
void jit_lock_inc64(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_dec64(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_not64(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_neg64(CpuState* cpu, GuestMemory* mem, u64 addr);
void jit_lock_xadd64(CpuState* cpu, GuestMemory* mem, u64 addr, u32 reg_idx);
void jit_lock_cmpxchg64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 reg_val);
void jit_lock_or64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 mask);
void jit_lock_and64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 mask);
void jit_lock_xor64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 mask);
void jit_lock_add64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 addend);
void jit_lock_sub64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 subtrahend);
void jit_lock_adc64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 addend);
void jit_lock_sbb64(CpuState* cpu, GuestMemory* mem, u64 addr, u64 subtrahend);
void jit_lock_adc32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 addend);
void jit_lock_sbb32(CpuState* cpu, GuestMemory* mem, u64 addr, u32 subtrahend);
// LOCK BT/BTS/BTR/BTC r/m64 (Intel SDM Vol. 2: BTS/BTR/BTC + LOCK; BT
// matches interpreter under the process lock mutex). Bit-string addressing:
// byte = addr + (signed_index >> 3), bit = index & 7.
void jit_lock_bt64(CpuState* cpu, GuestMemory* mem, u64 addr, i64 bit_index);
void jit_lock_bts64(CpuState* cpu, GuestMemory* mem, u64 addr, i64 bit_index);
void jit_lock_btr64(CpuState* cpu, GuestMemory* mem, u64 addr, i64 bit_index);
void jit_lock_btc64(CpuState* cpu, GuestMemory* mem, u64 addr, i64 bit_index);
void jit_fused_memcpy(CpuState* cpu, GuestMemory* mem,
                       u32 src_gva, u32 dst_gva, u32 byte_count,
                       u64 xmm_pack, u32 xmm_count);
// Delegate a single instruction to the interpreter. The helper sets
// cpu->rip = instr_rip, calls Interpreter::step() (which decodes and
// executes one instruction and advances rip on Continue), and returns.
// Used as the generic fallback for any opcode that lacks a native stencil.
void jit_interpret_step(CpuState* cpu, GuestMemory* mem, u64 instr_rip);
// Fast variant: execute one already-decoded instruction (no re-decode). The
// JIT passes a stable pointer to the block's DecodedInstruction plus its rip.
void jit_interpret_decoded(CpuState* cpu, GuestMemory* mem,
                            const DecodedInstruction* instr, u64 instr_rip);
}

} // namespace vitreo::cpu::jit

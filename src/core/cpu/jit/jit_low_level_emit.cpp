// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_low_level_emit.cpp
/// @brief JitContext layout checks and low-level ARM64 emission helpers
///
/// Split out of jit_compiler.cpp by pure code motion; see that file for
/// the JIT register convention.

#include "jit_crash_breadcrumbs.hpp"

#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_symbol_map.hpp"
#include "jit_split_layout.hpp"
#include "ir/jit_ir.hpp"
#include "stencils.hpp"
#include "jit_coverage.hpp"
#include "jit_stats.hpp"
#include "core/cpu/decode/guest_mapped_reader.hpp"
#include "flag_liveness.hpp"
#include "jit_helpers.hpp"
#include "idiom_detector.hpp"
#include "linear_fault.hpp"
#include "hot_block_dump.hpp"
#include "../../memory/jit_region_registry.hpp"
#include "core/cpu/lock_prefix.hpp"
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/math/host_fenv.hpp"
#include "core/base/perf_window.hpp"
#include "core/cpu/decode/opcodes.hpp"  // inlinable perf_window_active_fast()
#include <vitreo/diagnostics.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#include <malloc/malloc.h>

namespace vitreo::cpu::jit {

using namespace jit_detail;

// Verify JitContext field offsets match the constants used in emitted ARM64
// DIAGNOSTIC: the perf window mirrors these enums as fixed-size histograms
// but must not include this header (core/base does not depend on core/cpu),
// so the bridge is asserted here.
static_assert(static_cast<u32>(JitExitReason::Count) <=
                  diagnostics::kPerfJitExitReasonCount,
              "perf window exit-reason histogram too small for JitExitReason");
static_assert(static_cast<u32>(JitExecResult::StaleCode) <
                  diagnostics::kPerfJitExecResultCount,
              "perf window result histogram too small for JitExecResult");

static_assert(offsetof(JitContext, region_base) == 64, "JitContext offset mismatch");
static_assert(offsetof(JitContext, region_end) == 68, "JitContext offset mismatch");
static_assert(offsetof(JitContext, region_host_base) == 72, "JitContext offset mismatch");
static_assert(offsetof(JitContext, region2_base) == 80, "JitContext offset mismatch");
static_assert(offsetof(JitContext, region2_end) == 84, "JitContext offset mismatch");
static_assert(offsetof(JitContext, region2_host_base) == 88, "JitContext offset mismatch");
static_assert(offsetof(JitContext, rep_movsd) == 96, "JitContext offset mismatch");
static_assert(offsetof(JitContext, rep_stosd) == 104, "JitContext offset mismatch");
static_assert(offsetof(JitContext, materialize_flags) == 112, "JitContext offset mismatch");
static_assert(offsetof(JitContext, cmpxchg8b) == 120, "JitContext offset mismatch");
static_assert(offsetof(JitContext, fpu_op) == 128, "JitContext offset mismatch");
static_assert(offsetof(JitContext, sse_op) == 136, "JitContext offset mismatch");
static_assert(offsetof(JitContext, cmpxchg32) == 144, "JitContext offset mismatch");
static_assert(offsetof(JitContext, shld32) == 152, "JitContext offset mismatch");
static_assert(offsetof(JitContext, rep_movsb) == 176, "JitContext offset mismatch");
static_assert(offsetof(JitContext, fused_memcpy) == 224, "JitContext offset mismatch");
static_assert(offsetof(JitContext, shrd32) == 232, "JitContext offset mismatch");
static_assert(offsetof(JitContext, interpret_step) == 240, "JitContext offset mismatch");
static_assert(offsetof(JitContext, rep_stosb) == 248, "JitContext offset mismatch");
static_assert(offsetof(JitContext, rep_stosw) == 256, "JitContext offset mismatch");
static_assert(offsetof(JitContext, rep_movsw) == 264, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repe_cmpsb) == 272, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repne_scasb) == 280, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, write_watch_pages) == 720, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, code_page_map) == JIT_CTX_OFF_CODE_PAGE_MAP,
                  "JitContext code_page_map offset mismatch");
    static_assert(offsetof(JitContext, code_chunk_pages) ==
                      JIT_CTX_OFF_CODE_CHUNK_PAGES,
                  "JitContext code_chunk_pages offset mismatch");
    static_assert(offsetof(JitContext, repe_cmpsw) == 728, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repe_cmpsd) == 736, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repne_cmpsw) == 744, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repne_cmpsd) == 752, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repe_scasw) == 760, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repe_scasd) == 768, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repne_scasw) == 776, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repne_scasd) == 784, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repe_scasb) == 792, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, repne_cmpsb) == 800, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, cond_fns) == 320, "JitContext offset mismatch");
static_assert(offsetof(JitContext, wregion_base) == 448, "JitContext offset mismatch");
static_assert(offsetof(JitContext, wregion_end) == 452, "JitContext offset mismatch");
static_assert(offsetof(JitContext, wregion_host_base) == 456, "JitContext offset mismatch");
static_assert(offsetof(JitContext, wregion2_base) == 464, "JitContext offset mismatch");
static_assert(offsetof(JitContext, wregion2_end) == 468, "JitContext offset mismatch");
static_assert(offsetof(JitContext, wregion2_host_base) == 472, "JitContext offset mismatch");
static_assert(offsetof(JitContext, linear_base) == 480, "JitContext offset mismatch");
static_assert(offsetof(JitContext, code_span_lo) == 488, "JitContext offset mismatch");
static_assert(offsetof(JitContext, code_span_hi) == 492, "JitContext offset mismatch");
static_assert(offsetof(JitContext, capture_cf) == 496, "JitContext offset mismatch");
static_assert(offsetof(JitContext, zero_fill_loop) == 504, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_region_base) == 520, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_region_end) == 528, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_region_host_base) == 536, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_region2_base) == 544, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_region2_end) == 552, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_region2_host_base) == 560, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_read16_region_base) == 568, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_read16_region_end) == 576, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_read16_region_host_base) == 584, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_read16_region2_base) == 592, "JitContext offset mismatch");
static_assert(offsetof(JitContext, x64_read16_region2_end) == 600, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, x64_read16_region2_host_base) == 608, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, atomic_xchg32) == 616, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, lock_or32) == 624, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, lock_and32) == 632, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, lock_add32) == 640, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, lock_sub32) == 648, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, lock_cmpxchg8) == 656, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, lock_or16) == 664, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, lock_and16) == 672, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, fpu_peek_m32) == 680, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, fpu_pop) == 688, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, x64_wregion_base) == 696, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, x64_wregion_end) == 704, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, x64_wregion_host_base) == 712, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, write_watch_pages) == 720, "JitContext offset mismatch");
    static_assert(offsetof(JitContext, code_page_map) == JIT_CTX_OFF_CODE_PAGE_MAP,
                  "JitContext code_page_map offset mismatch");

// ============================================================================
// Low-level emission
// ============================================================================

void JitCompiler::emit(u32 insn) {
    m_code_buf.push_back(insn);
}

void JitCompiler::emit_load_gpr32_mem(u8 arm_reg, u8 x86_reg) {
    u32 off = gpr_off(x86_reg);
    if (fits_ldr_w_offset(off)) {
        emit(arm64::ldr_w_uoff(arm_reg, REG_CPU_STATE, off >> 2));
    } else {
        emit_mov_imm32(REG_SCRATCH4, off);
        emit(arm64::ldr_w_reg(arm_reg, REG_CPU_STATE, REG_SCRATCH4));
    }
}

void JitCompiler::emit_store_gpr32_mem(u8 arm_reg, u8 x86_reg) {
    u32 off = gpr_off(x86_reg);
    // A 32-bit write zero-extends into the 64-bit slot (SDM Vol. 1 3.4.1.1):
    // the value and WZR as one pair when the slot is in STP's reach.
    if (off % 4u == 0 && off / 4u <= 63u) {
        emit(arm64::stp_w_off(arm_reg, 31, REG_CPU_STATE,
                              static_cast<i32>(off / 4u)));
        return;
    }
    if (fits_ldr_w_offset(off)) {
        emit(arm64::str_w_uoff(arm_reg, REG_CPU_STATE, off >> 2));
    } else {
        emit_mov_imm32(REG_SCRATCH4, off);
        emit(arm64::str_w_reg(arm_reg, REG_CPU_STATE, REG_SCRATCH4));
    }
    u32 hi_off = off + 4;
    if (fits_ldr_w_offset(hi_off)) {
        emit(arm64::str_w_uoff(31, REG_CPU_STATE, hi_off >> 2));
    }
}

void JitCompiler::emit_load_gpr32(u8 arm_reg, u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (m_pins_live && x86_reg < 16 && m_pin_arm[x86_reg] != 0xFF) {
        const u8 pin = m_pin_arm[x86_reg];
        if (arm_reg != pin) emit(arm64::mov_w(arm_reg, pin));
        return;
    }
    emit_load_gpr32_mem(arm_reg, x86_reg);
}

void JitCompiler::emit_merge_gpr_narrow(u8 value_reg, u8 x86_reg, u8 lsb,
                                        u8 width) {
    // A PARTIAL write is still a write, and it must carry the same bookkeeping
    // as `emit_store_gpr32` below.
    //
    // Tier 1 originally emitted the `BFI` itself, straight at the pin, and so
    // skipped the one line that matters here: `m_esp_frame_live`. X15 caches
    // `linear_base + ESP` for stack accesses and is documented as killed on
    // any write to ESP. A 16-bit `mov sp, bx` updated the ESP pin and left
    // that cache live, and the next stack access dereferenced a stale base —
    // a host SIGSEGV at a small address, from generated code, with the linear
    // fault handler correctly declining to recover it because the address was
    // nowhere near the guest reservation.
    //
    // The lesson is the placement, not the line: a lowering that writes a
    // guest register through its own instruction sequence inherits none of
    // the invalidation the register-write primitives own. So the narrow write
    // is a primitive HERE, beside the full-width one, and anything added to
    // that one is inherited rather than re-remembered.
    if (x86_reg == static_cast<u8>(RegIndex::RSP)) m_esp_frame_live = false;
    // The insert must be the X form in a 64-bit block. `BFI Wd` zeroes bits
    // 63:32 of the destination, which is exactly what a narrow x86 write must
    // NOT do: `mov al, 5` leaves RAX[63:8] alone. In a 32-bit block the pin
    // holds 32 bits and the W form is correct.
    const bool wide = m_block_is_64bit;
    if (m_pins_live && x86_reg < 16 && m_pin_arm[x86_reg] != 0xFF) {
        emit(wide ? arm64::bfi_x(m_pin_arm[x86_reg], value_reg, lsb, width)
                  : arm64::bfi_w(m_pin_arm[x86_reg], value_reg, lsb, width));
        return;
    }
    // No pin: read-modify-write the CpuState slot. REG_SCRATCH4 is safe as the
    // merge temp because a GPR's offset always fits the scaled immediate, so
    // `emit_store_gpr32_mem` does not need it for an address.
    if (wide) {
        emit_load_gpr64_mem(REG_SCRATCH4, x86_reg);
        emit(arm64::bfi_x(REG_SCRATCH4, value_reg, lsb, width));
        emit_store_gpr64_mem(REG_SCRATCH4, x86_reg);
        return;
    }
    emit_load_gpr32(REG_SCRATCH4, x86_reg);
    emit(arm64::bfi_w(REG_SCRATCH4, value_reg, lsb, width));
    emit_store_gpr32_mem(REG_SCRATCH4, x86_reg);
}

void JitCompiler::emit_store_gpr32(u8 arm_reg, u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (x86_reg == static_cast<u8>(RegIndex::RSP)) m_esp_frame_live = false;
    if (m_pins_live && x86_reg < 16 && m_pin_arm[x86_reg] != 0xFF) {
        const u8 pin = m_pin_arm[x86_reg];
        if (arm_reg != pin) emit(arm64::mov_w(pin, arm_reg));
        return;
    }
    emit_store_gpr32_mem(arm_reg, x86_reg);
}

void JitCompiler::emit_load_pins() {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (!m_pins_live) return;
    if (!m_block_is_64bit) {
        // GeneralRegs64::r[0..7] are contiguous u64 slots. Pair-loading the
        // slots halves the PE32 dispatcher/helper reload sequence from eight
        // instructions to four. The upper halves may enter the host X pins,
        // but every Mode32 pin consumer and writeback is explicitly W-width;
        // CpuState remains authoritative for guest bits 63:32.
        static_assert(offsetof(CpuState, gpr) == 0);
        static_assert(sizeof(CpuState{}.gpr.r[0]) == 8);
        emit(arm64::ldp_x_off(m_pin_arm[0], m_pin_arm[1], REG_CPU_STATE, 0));
        emit(arm64::ldp_x_off(m_pin_arm[2], m_pin_arm[3], REG_CPU_STATE, 2));
        emit(arm64::ldp_x_off(m_pin_arm[4], m_pin_arm[5], REG_CPU_STATE, 4));
        emit(arm64::ldp_x_off(m_pin_arm[6], m_pin_arm[7], REG_CPU_STATE, 6));
        return;
    }
    // Pair-load guest r[0..7] whenever all eight low GPRs are pinned. The
    // host register names follow the selected layout (static or dynamic);
    // CpuState slots stay guest-ordered, so LDP is valid for any pairing.
    if (m_pin_arm[0] != 0xFF && m_pin_arm[1] != 0xFF &&
        m_pin_arm[2] != 0xFF && m_pin_arm[3] != 0xFF &&
        m_pin_arm[4] != 0xFF && m_pin_arm[5] != 0xFF &&
        m_pin_arm[6] != 0xFF && m_pin_arm[7] != 0xFF) {
        static_assert(offsetof(CpuState, gpr) == 0);
        emit(arm64::ldp_x_off(m_pin_arm[0], m_pin_arm[1], REG_CPU_STATE, 0));
        emit(arm64::ldp_x_off(m_pin_arm[2], m_pin_arm[3], REG_CPU_STATE, 2));
        emit(arm64::ldp_x_off(m_pin_arm[4], m_pin_arm[5], REG_CPU_STATE, 4));
        emit(arm64::ldp_x_off(m_pin_arm[6], m_pin_arm[7], REG_CPU_STATE, 6));
        return;
    }
    for (u8 x = 0; x < 16; ++x) {
        if (m_pin_arm[x] == 0xFF) continue;
        emit_load_gpr64_mem(m_pin_arm[x], x);
    }
}

void JitCompiler::emit_store_pins() {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (!m_pins_live) return;
    // 64-bit blocks: the pin is authoritative for all 64 bits (every write
    // went through it; 32-bit writes zero-extended, the x86-64 rule), so the
    // contiguous r[0..7] slots pair-store exactly like the load side.
    if (m_block_is_64bit &&
        m_pin_arm[0] != 0xFF && m_pin_arm[1] != 0xFF &&
        m_pin_arm[2] != 0xFF && m_pin_arm[3] != 0xFF &&
        m_pin_arm[4] != 0xFF && m_pin_arm[5] != 0xFF &&
        m_pin_arm[6] != 0xFF && m_pin_arm[7] != 0xFF) {
        emit(arm64::stp_x_off(m_pin_arm[0], m_pin_arm[1], REG_CPU_STATE, 0));
        emit(arm64::stp_x_off(m_pin_arm[2], m_pin_arm[3], REG_CPU_STATE, 2));
        emit(arm64::stp_x_off(m_pin_arm[4], m_pin_arm[5], REG_CPU_STATE, 4));
        emit(arm64::stp_x_off(m_pin_arm[6], m_pin_arm[7], REG_CPU_STATE, 6));
        return;
    }
    for (u8 x = 0; x < 16; ++x) {
        if (m_pin_arm[x] == 0xFF) continue;
        if (m_block_is_64bit) {
            emit_store_gpr64_mem(m_pin_arm[x], x);
            continue;
        }
        // Store only the live 32-bit view. Zero-extending here would smash
        // bits 32-63 (and, after FNSTSW/SAHF-style STRH, bits 16-31) that
        // live in CpuState but were never in the pin.
        u32 off = gpr_off(x);
        if (fits_ldr_w_offset(off)) {
            emit(arm64::str_w_uoff(m_pin_arm[x], REG_CPU_STATE, off >> 2));
        } else {
            emit_mov_imm32(REG_SCRATCH4, off);
            emit(arm64::str_w_reg(m_pin_arm[x], REG_CPU_STATE, REG_SCRATCH4));
        }
    }
}

void JitCompiler::emit_load_gpr_ptr(u8 arm_reg, u8 x86_reg) {
    if (m_block_is_64bit) {
        emit_load_gpr64(arm_reg, x86_reg);
    } else {
        emit_load_gpr32(arm_reg, x86_reg);
    }
}

void JitCompiler::emit_store_gpr_ptr(u8 arm_reg, u8 x86_reg) {
    if (m_block_is_64bit) {
        emit_store_gpr64(arm_reg, x86_reg);
    } else {
        emit_store_gpr32(arm_reg, x86_reg);
    }
}

void JitCompiler::emit_pin_spill_one(u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (!m_pins_live || x86_reg >= 16 || m_pin_arm[x86_reg] == 0xFF) return;
    if (m_block_is_64bit) {
        emit_store_gpr64_mem(m_pin_arm[x86_reg], x86_reg);
        return;
    }
    // Raw 32-bit store, deliberately NOT emit_store_gpr32_mem: that applies
    // x86-64's zero-extension and would blank bits 63:32, which a 32-bit
    // block keeps in CpuState and never had in the pin. Same reasoning as
    // emit_store_pins.
    const u32 off = gpr_off(x86_reg);
    if (fits_ldr_w_offset(off)) {
        emit(arm64::str_w_uoff(m_pin_arm[x86_reg], REG_CPU_STATE, off >> 2));
    } else {
        emit_mov_imm32(REG_SCRATCH4, off);
        emit(arm64::str_w_reg(m_pin_arm[x86_reg], REG_CPU_STATE, REG_SCRATCH4));
    }
}

void JitCompiler::emit_pin_reload_one(u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (!m_pins_live || x86_reg >= 16 || m_pin_arm[x86_reg] == 0xFF) return;
    if (m_block_is_64bit) {
        emit_load_gpr64_mem(m_pin_arm[x86_reg], x86_reg);
    } else {
        emit_load_gpr32_mem(m_pin_arm[x86_reg], x86_reg);
    }
}

void JitCompiler::emit_load_linear_base() {
    emit(arm64::ldr_x_uoff(REG_LINEAR_BASE, REG_JIT_CTX,
                            JIT_CTX_OFF_LINEAR_BASE >> 3));
}

void JitCompiler::emit_load_linear64_check_map() {
    // Under the JIT view X15 is the view's base and the map is reached only
    // through emit_linear64_check_map_base.
    emit(arm64::ldr_x_uoff(REG_LINEAR64_CHECK_MAP, REG_JIT_CTX,
                           (m_cfg.linear64_jit_view
                                ? JIT_CTX_OFF_JIT_VIEW_BASE
                                : JIT_CTX_OFF_LINEAR_CHECK_MAP) >> 3));
}

u8 JitCompiler::emit_linear64_check_map_base(u8 scratch) {
    if (!m_cfg.linear64_jit_view) return REG_LINEAR64_CHECK_MAP;
    emit(arm64::ldr_x_uoff(scratch, REG_JIT_CTX,
                           JIT_CTX_OFF_LINEAR_CHECK_MAP >> 3));
    return scratch;
}

void JitCompiler::emit_helper_blr() {
    AttrScope attr_scope(*this, EmitAttribution::Helper);
    m_flags.helper_blr_this_insn = true;
    m_vregs_clobbered = true;
    if (m_pins_live) emit_store_pins();
    emit(0xD63F0200u); // BLR X16
    emit_probe_poison_vregs();
    if (m_pins_live) emit_load_pins();
    if (m_linear_base_pinned) emit_load_linear_base();
    if (m_block_is_64bit && m_cfg.linear64 && m_linear_base_pinned) {
        emit_load_linear64_check_map();
    }
    m_esp_frame_live = false;
}

void JitCompiler::emit_semantic_helper_blr() {
    AttrScope attr_scope(*this, EmitAttribution::Helper);
    m_flags.semantic_helper_this_insn = true;
    emit_helper_blr();
}

void JitCompiler::emit_memory_helper_blr() {
    AttrScope attr_scope(*this, EmitAttribution::Helper);
    m_vregs_clobbered = true;
    // Only the caller-saved pins need saving: RSP->X12 and RBP->X13 are in
    // AAPCS64's temporary class (X9-X15), while RAX/RCX/RDX/RBX/RSI/RDI live
    // in X23-X28 which the callee is required to preserve. See the header for
    // why this must not be used for the interpreter delegate.
    //
    // emit_pin_spill_one/emit_pin_reload_one carry the per-bitness width rule
    // (64-bit block: the pin owns all 64 bits; 32-bit block: a raw 32-bit
    // store, deliberately not the zero-extending helper, because bits 63:32
    // live only in CpuState). Do not open-code these stores, and never batch
    // them with STP -- that is correct in a 64-bit block and silently wrong in
    // a 32-bit one.
    constexpr u8 kRsp = 4;
    constexpr u8 kRbp = 5;
    if (m_pins_live) {
        emit_pin_spill_one(kRsp);
        emit_pin_spill_one(kRbp);
    }
    emit(0xD63F0200u); // BLR X16
    emit_probe_poison_vregs();
    if (m_pins_live) {
        emit_pin_reload_one(kRsp);
        emit_pin_reload_one(kRbp);
    }
    if (m_linear_base_pinned) emit_load_linear_base();
    if (m_block_is_64bit && m_cfg.linear64 && m_linear_base_pinned) {
        emit_load_linear64_check_map();
    }
    m_esp_frame_live = false;
}

void JitCompiler::emit_load_gpr8(u8 arm_reg, u8 x86_reg, bool has_rex) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    // One instruction either way: extract the byte from the pin, or load it
    // from its CpuState slot (little-endian: AH..BH are byte 1 of RAX..RBX).
    {
        const bool high = x86_reg >= 4 && x86_reg <= 7 && !has_rex;
        const u8 parent = high ? static_cast<u8>(x86_reg - 4) : x86_reg;
        const u8 pin = (m_pins_live && parent < 16) ? m_pin_arm[parent] : u8{0xFF};
        if (pin != 0xFF) {
            // UBFX Wd, Wpin, #8, #8 / UXTB Wd, Wpin
            emit((high ? 0x53083C00u : 0x53001C00u) |
                 (static_cast<u32>(pin) << 5) | static_cast<u32>(arm_reg));
            return;
        }
        if (parent < 16) {
            const u32 off = gpr_off(parent) + (high ? 1u : 0u);
            if (off <= 4095u) {
                emit(arm64::ldrb_uoff(arm_reg, REG_CPU_STATE, off));
                return;
            }
        }
    }
    if (x86_reg >= 4 && x86_reg <= 7 && !has_rex) {
        // Legacy high byte register (AH=4, CH=5, DH=6, BH=7) — no REX.
        // Load parent register (reg - 4), extract bits [15:8].
        emit_load_gpr32(arm_reg, x86_reg - 4);
        // UBFX Wd, Wn, #8, #8 = UBFM Wd, Wn, #8, #15
        emit(0x53083C00u | (static_cast<u32>(arm_reg) << 5)
                         | static_cast<u32>(arm_reg));
    } else {
        // Low byte register: AL/CL/DL/BL (0-3),
        // SPL/BPL/SIL/DIL (4-7 with REX), R8B-R15B (8-15).
        emit_load_gpr32(arm_reg, x86_reg);
        // UXTB: UBFM Wd, Wn, #0, #7
        emit(0x53001C00u | (static_cast<u32>(arm_reg) << 5)
                         | static_cast<u32>(arm_reg));
    }
}

void JitCompiler::emit_store_gpr8(u8 arm_reg, u8 x86_reg, bool has_rex) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    // Insert the byte into the pin itself, or store it into its CpuState
    // slot: one instruction, every other bit preserved (Intel SDM Vol. 1
    // 3.4.1.1 -- an 8-bit write leaves the rest of the register). RSP's
    // byte forms keep the generic path for its frame-cache bookkeeping.
    {
        const bool high = x86_reg >= 4 && x86_reg <= 7 && !has_rex;
        const u8 parent = high ? static_cast<u8>(x86_reg - 4) : x86_reg;
        const u8 pin = (m_pins_live && parent < 16) ? m_pin_arm[parent] : u8{0xFF};
        if (parent != static_cast<u8>(RegIndex::RSP) && parent < 16) {
            if (pin != 0xFF && pin != arm_reg) {
                // BFI Xpin, Xarm, #lsb, #8 (W form in a 32-bit block, whose
                // pins carry bits 31:0 only).
                const u32 word = m_block_is_64bit
                    ? (high ? 0xB3781C00u : 0xB3401C00u)
                    : (high ? 0x33181C00u : 0x33001C00u);
                emit(word | (static_cast<u32>(arm_reg) << 5) |
                     static_cast<u32>(pin));
                return;
            }
            if (pin == 0xFF) {
                const u32 off = gpr_off(parent) + (high ? 1u : 0u);
                if (off <= 4095u) {
                    emit(arm64::strb_uoff(arm_reg, REG_CPU_STATE, off));
                    return;
                }
            }
        }
    }
    if (x86_reg >= 4 && x86_reg <= 7 && !has_rex) {
        // Legacy high byte register (AH=4, CH=5, DH=6, BH=7) — no REX.
        u8 parent_reg = x86_reg - 4;
        if (m_block_is_64bit) {
            emit_load_gpr64(REG_SCRATCH4, parent_reg);
            // BFI X4, Xarm, #8, #8 = BFM X4, Xarm, #56, #7
            emit(0xB3781C00u | (static_cast<u32>(arm_reg) << 5)
                             | static_cast<u32>(REG_SCRATCH4));
            emit_store_gpr64(REG_SCRATCH4, parent_reg);
        } else {
            emit_load_gpr32(REG_SCRATCH4, parent_reg);
            // BFI W4, Warm, #8, #8 = BFM W4, Warm, #24, #7
            emit(0x33181C00u | (static_cast<u32>(arm_reg) << 5)
                             | static_cast<u32>(REG_SCRATCH4));
            emit_store_gpr32(REG_SCRATCH4, parent_reg);
        }
    } else if (m_block_is_64bit) {
        // Low byte: preserve bits 8..63 (Intel SDM Vol. 2, SETCC r/m8).
        emit_load_gpr64(REG_SCRATCH4, x86_reg);
        // BFI X4, Xarm, #0, #8 = BFM X4, Xarm, #0, #7
        emit(0xB3400000u | (static_cast<u32>(arm_reg) << 5)
                         | static_cast<u32>(REG_SCRATCH4)
                         | (7u << 10));
        emit_store_gpr64(REG_SCRATCH4, x86_reg);
    } else {
        emit_load_gpr32(REG_SCRATCH4, x86_reg);
        // BFI W4, Warm, #0, #8
        emit(0x33000000u | (static_cast<u32>(arm_reg) << 5)
                         | static_cast<u32>(REG_SCRATCH4)
                         | (7u << 10));
        emit_store_gpr32(REG_SCRATCH4, x86_reg);
    }
}

void JitCompiler::emit_load_gpr16(u8 arm_reg, u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    if (x86_reg < 16) {
        const u8 pin = m_pins_live ? m_pin_arm[x86_reg] : u8{0xFF};
        if (pin != 0xFF) {
            emit(arm64::uxth_w(arm_reg, pin));
            return;
        }
        const u32 off = gpr_off(x86_reg);
        if (off <= 8190u && (off & 1u) == 0) {
            emit(arm64::ldrh_uoff(arm_reg, REG_CPU_STATE, off >> 1));
            return;
        }
    }
    // Load 32-bit GPR then zero-extend to 16 bits via UXTH
    emit_load_gpr32(arm_reg, x86_reg);
    emit(arm64::uxth_w(arm_reg, arm_reg));
}

void JitCompiler::emit_store_gpr16(u8 arm_reg, u8 x86_reg) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    // A 16-bit write is a partial-register update. In long mode it preserves
    // bits 16..63; the 32-bit BFI/store sequence would incorrectly clear the
    // upper half through emit_store_gpr32's x86-64 zero-extension semantics.
    if (m_block_is_64bit) {
        emit_load_gpr64(REG_SCRATCH4, x86_reg);
        // BFI X4, Xarm, #0, #16 = BFM X4, Xarm, #0, #15.
        emit(0xB3400000u | (static_cast<u32>(arm_reg) << 5)
                         | static_cast<u32>(REG_SCRATCH4)
                         | (15u << 10));
        emit_store_gpr64(REG_SCRATCH4, x86_reg);
    } else {
        emit_load_gpr32(REG_SCRATCH4, x86_reg);
        // BFI W4, Warm, #0, #16 = BFM W4, Warm, #0, #15.
        emit(0x33000000u | (static_cast<u32>(arm_reg) << 5)
                         | static_cast<u32>(REG_SCRATCH4)
                         | (15u << 10));
        emit_store_gpr32(REG_SCRATCH4, x86_reg);
    }
}

void JitCompiler::emit_mov_imm32(u8 arm_reg, u32 value) {
    // One instruction whenever a 16-bit half is all zeros or all ones (MOVZ
    // or MOVN, either half); small negative immediates are the common case.
    // None of MOVZ/MOVN/MOVK touches NZCV, so callers may materialise a
    // constant between a flag producer and its consumer.
    const u16 lo = static_cast<u16>(value & 0xFFFF);
    const u16 hi = static_cast<u16>(value >> 16);
    if (hi == 0) {
        emit(arm64::movz_w(arm_reg, lo));
    } else if (lo == 0) {
        emit(arm64::movz_w_shift(arm_reg, hi, 16));
    } else if (hi == 0xFFFF) {
        emit(arm64::movn_w(arm_reg, static_cast<u16>(~lo), 0));
    } else if (lo == 0xFFFF) {
        emit(arm64::movn_w(arm_reg, static_cast<u16>(~hi), 16));
    } else {
        emit(arm64::movz_w(arm_reg, lo));
        emit(arm64::movk_w(arm_reg, hi, 16));
    }
}

void JitCompiler::emit_add_x_uimm24(u8 d, u8 n, u64 value) {
    const arm64::AddUimm24 seq = arm64::add_x_uimm24(d, n, value);
    for (u32 i = 0; i < seq.count; ++i) emit(seq.word[i]);
}

void JitCompiler::emit_update_rip(u64 new_rip) {
    AttrScope attr_scope(*this, EmitAttribution::Rip);
    u32 off = rip_offset();
    // Materialize the full guest RIP directly.  Building it as two W-register
    // halves plus ORR costs an extra instruction for ordinary x64 image
    // addresses (for example 0x0000000140000000).  This helper already omits
    // zero halfwords, preserving the exact u64 while shortening every x64
    // memory-fault RIP publication on the hot path.
    m_rip_reg_start = m_code_buf.size();
    emit_mov_imm64(REG_SCRATCH0, static_cast<i64>(new_rip));
    if (fits_ldr_x_offset(off)) {
        emit(arm64::str_x_uoff(REG_SCRATCH0, REG_CPU_STATE, off >> 3));
    }
    // REG_SCRATCH0 now holds exactly `new_rip`. A block tail emitted right
    // here can branch straight to a shared linked-tail stub instead of
    // rematerializing the target; see emit_linked_tail_continue.
    m_rip_reg_value = new_rip;
    m_rip_reg_pos = m_code_buf.size();
}

void JitCompiler::emit_publish_rip_for_raising_helper(u8 scratch) {
    AttrScope attr_scope(*this, EmitAttribution::Rip);
    // A fault is precise (Intel SDM Vol. 3A 6.6): the exception names the
    // faulting instruction. A linked hop or an earlier instruction leaves
    // CpuState.rip anywhere, and the software-raise flag rebuild in execute()
    // looks its plan up by this RIP too.
    emit_mov_imm64(scratch, static_cast<i64>(m_diagnostic_instruction_rip));
    emit(arm64::str_x_uoff(scratch, REG_CPU_STATE, rip_offset() >> 3));
}

} // namespace vitreo::cpu::jit

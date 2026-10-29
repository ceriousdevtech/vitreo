// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_access_emit.cpp
/// @brief Guest-access ordering (x86-TSO) and store primitives
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

void JitCompiler::emit_tso_barrier() {
    if (m_cfg.strict_tso) {
        // DMB ISH. This intentionally over-orders while diagnosing x86 TSO
        // publication/observation defects on Apple ARM.
        emit(0xD5033BBFu);
    }
}

void JitCompiler::emit_tso_store_tail_barrier() {
    // A 32-bit guest store used to be fenced on BOTH sides. Only the leading
    // fence carries x86-TSO:
    //
    //   load  -> [LDR, DMB]        store -> [DMB, STR]
    //
    //   load  -> load    ordered by the first load's trailing DMB
    //   load  -> store   ordered by either barrier
    //   store -> store   ordered by the SECOND store's leading DMB
    //   store -> load    NOT ordered -- and x86 does not order it either
    //                    (SDM Vol. 3A 9.2.2: "loads may be reordered with
    //                    older stores to different locations"). That is the
    //                    store buffer, and it is the entire difference
    //                    between TSO and sequential consistency.
    //
    // So the trailing DMB bought exactly one edge, and that edge is one x86
    // does not have. Dropping it halves the barrier cost of every 32-bit
    // store, PUSH, CALL and RET.
    if (m_cfg.strict_tso && m_cfg.tso_store_tail_fence) {
        emit(0xD5033BBFu);
    }
}

// ---------------------------------------------------------------------------
// x86-TSO ordered guest accesses (TsoMode::Ordered)
// ---------------------------------------------------------------------------
//
// x86-TSO in one line: loads are not reordered with loads, stores are not
// reordered with stores, and stores are not reordered with OLDER loads. Only
// store->load may be reordered, which is the store buffer.
//
// The mapping is therefore acquire-load / release-store per access:
//
//   guest load  -> LDAPR (FEAT_LRCPC) or LDAR
//   guest store -> STLR
//
// LDAPR is preferred where the hardware has it because RCpc is precisely the
// weaker acquire that x86 needs: LDAR would additionally restore store->load
// ordering, which TSO does not require and which costs. That is also why this
// is not the same thing as the DMB in Fence mode -- a full barrier orders
// everything in both directions.
//
// These forms have NO offset addressing mode: the operand is [Xn] only. The
// caller must therefore materialize the complete host address into a single
// X register first, which is what emit_linear_addr_x is for.

bool JitCompiler::tso_ordered() const noexcept {
    return m_cfg.tso_mode == TsoMode::Ordered;
}


void JitCompiler::emit_acquire_load(u8 dst, u8 addr_x, int width) {
    const bool rcpc = m_cfg.tso_use_ldapr;
    switch (width) {
        case 1:
            emit(rcpc ? arm64::ldapr_b(dst, addr_x)
                      : arm64::ldar_b(dst, addr_x));
            return;
        case 2:
            emit(rcpc ? arm64::ldapr_h(dst, addr_x)
                      : arm64::ldar_h(dst, addr_x));
            return;
        case 8:
            emit(rcpc ? arm64::ldapr_x(dst, addr_x)
                      : arm64::ldar_x(dst, addr_x));
            return;
        default:
            emit(rcpc ? arm64::ldapr_w(dst, addr_x)
                      : arm64::ldar_w(dst, addr_x));
            return;
    }
}

void JitCompiler::emit_release_store(u8 src, u8 addr_x, int width) {
    switch (width) {
        case 1: emit(arm64::stlr_b(src, addr_x)); return;
        case 2: emit(arm64::stlr_h(src, addr_x)); return;
        case 8: emit(arm64::stlr_x(src, addr_x)); return;
        default: emit(arm64::stlr_w(src, addr_x)); return;
    }
}

void JitCompiler::emit_x64_tso_load(u8 dst, u8 addr_x, u8 scratch_x,
                                    int width) {
    const auto emit_plain = [&] {
        switch (width) {
            case 1: emit(arm64::ldrb_uoff(dst, addr_x, 0)); return;
            case 2: emit(arm64::ldrh_uoff(dst, addr_x, 0)); return;
            case 8: emit(arm64::ldr_x_uoff(dst, addr_x, 0)); return;
            default: emit(arm64::ldr_w_uoff(dst, addr_x, 0)); return;
        }
    };
    if (m_cfg.tso_mode == TsoMode::Off) {
        emit_plain();
        return;
    }
    if (width == 1) {
        emit_acquire_load(dst, addr_x, width);
        return;
    }

    if (!m_cfg.tso_fault_patch) {
        emit(arm64::ubfx_x(scratch_x, addr_x, 0, 4));
        emit(arm64::cmp_x_imm(scratch_x, static_cast<u32>(16 - width)));
        const usize crossing_pos = m_code_buf.size();
        emit(0);
        emit_acquire_load(dst, addr_x, width);
        const usize done_branch = m_code_buf.size();
        emit(0);
        const usize crossing = m_code_buf.size();
        emit_plain();
        emit(0xD5033BBFu);
        const usize done = m_code_buf.size();
        m_code_buf[crossing_pos] = arm64::b_cond(
            arm64::HI, static_cast<i32>(crossing - crossing_pos) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        return;
    }

    // Two-word fault-patchable site. Same-granule accesses execute only the
    // ordered load plus a harmless marker. An alignment SIGBUS converts the
    // pair, outside signal context, to [plain load, DMB ISH] and retries it.
    emit_acquire_load(dst, addr_x, width);
    emit(kTsoPatchMarker);
}

void JitCompiler::emit_x64_tso_store(u8 src, u8 addr_x, u8 scratch_x,
                                     int width) {
    const auto emit_plain = [&] {
        switch (width) {
            case 1: emit(arm64::strb_uoff(src, addr_x, 0)); return;
            case 2: emit(arm64::strh_uoff(src, addr_x, 0)); return;
            case 8: emit(arm64::str_x_uoff(src, addr_x, 0)); return;
            default: emit(arm64::str_w_uoff(src, addr_x, 0)); return;
        }
    };
    if (m_cfg.tso_mode == TsoMode::Off) {
        emit_plain();
        return;
    }
    if (width == 1) {
        emit_release_store(src, addr_x, width);
        return;
    }

    if (!m_cfg.tso_fault_patch) {
        emit(arm64::ubfx_x(scratch_x, addr_x, 0, 4));
        emit(arm64::cmp_x_imm(scratch_x, static_cast<u32>(16 - width)));
        const usize crossing_pos = m_code_buf.size();
        emit(0);
        emit_release_store(src, addr_x, width);
        const usize done_branch = m_code_buf.size();
        emit(0);
        const usize crossing = m_code_buf.size();
        emit(0xD5033BBFu);
        emit_plain();
        const usize done = m_code_buf.size();
        m_code_buf[crossing_pos] = arm64::b_cond(
            arm64::HI, static_cast<i32>(crossing - crossing_pos) * 4);
        m_code_buf[done_branch] = arm64::b_imm(
            static_cast<i32>(done - done_branch) * 4);
        return;
    }

    // Publish order for the patched form is marker->DMB first, then
    // STLR->plain store, so no concurrent executor can observe an unordered
    // intermediate pair.
    emit(kTsoPatchMarker);
    emit_release_store(src, addr_x, width);
}

u8 JitCompiler::emit_addr_x_add(u8 base_x, u8 addr_x, u8 scratch_x) {
    // Long-mode variant of emit_linear_addr_x: the guest address is already a
    // full 64-bit value, so no zero-extension. Also safe when scratch ==
    // addr_x.
    emit(arm64::add_x(scratch_x, base_x, addr_x));
    return scratch_x;
}

void JitCompiler::emit_region_inline_load(u8 dst, u8 base_x, u8 off_w,
                                          int width) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (tso_ordered()) {
        // The offset register is the last use of both operands on this path --
        // the next region probe reloads base and recomputes the offset -- so
        // it can carry the folded host address.
        const u8 addr_x = emit_linear_addr_x(base_x, off_w, off_w);
        emit_x64_tso_load(dst, addr_x, 8, width);
        return;
    }
    switch (width) {
        case 1: emit(arm64::ldrb_reg(dst, base_x, off_w)); return;
        case 2: emit(arm64::ldrh_reg(dst, base_x, off_w)); return;
        case 8: emit(arm64::ldr_x_reg(dst, base_x, off_w)); return;
        default: emit(arm64::ldr_w_reg(dst, base_x, off_w)); return;
    }
}


bool JitCompiler::tso_vector() const noexcept {
    // Deliberately NOT gated on tso_ordered(). 128-bit accesses have no
    // ordered form at any width, so a fence is the only lowering they ever
    // get -- which means Fence mode and Ordered mode want exactly the same
    // thing here. Gating this on tso_ordered() left vector guest accesses
    // unordered whenever vector TSO is off, which is the shipping default.
    // Only Off means no ordering.
    return m_cfg.tso_vector && m_cfg.tso_mode != TsoMode::Off;
}

void JitCompiler::emit_vector_tso_fence() {
    // ARM64 has no 128-bit acquire/release form -- LDAR/STLR top out at 64
    // bits -- so vector guest accesses can only be ordered with a full DMB
    // ISH. VITREO_JIT_TSO_VECTOR=0 is an explicit performance/bisection
    // relaxation; the default preserves x86 TSO.
    //
    // Ordering, not atomicity: x86 does not guarantee that a 16-byte MOVDQA
    // is a single atomic access either, so splitting is not the concern --
    // reordering is.
    if (tso_vector()) emit(0xD5033BBFu);
}

void JitCompiler::emit_mark_write_watch(u8 addr_wreg, u32 last_byte) {
    AttrScope attr_scope(*this, EmitAttribution::Guard);
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                            JIT_CTX_OFF_WRITE_WATCH_PAGES >> 3));
    emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_SCRATCH3, 0));
    const u32 watch_cbz = static_cast<u32>(m_code_buf.size());
    emit(0);
    emit(arm64::lsr_w_imm(4, addr_wreg, 12));
    emit(arm64::movz_w(5, 1));
    emit(arm64::strb_uxtw(5, REG_SCRATCH3, 4));
    if (last_byte != 0) {
        emit(arm64::add_w_imm(6, addr_wreg, last_byte));
        emit(arm64::lsr_w_imm(6, 6, 12));
        emit(arm64::strb_uxtw(5, REG_SCRATCH3, 6));
    }
    m_code_buf[watch_cbz] = arm64::cbz_x(
        REG_SCRATCH3,
        static_cast<i32>(m_code_buf.size() - watch_cbz) * 4);
}


bool JitCompiler::linear32_store_probe_halfword() noexcept {
    // Default OFF: exact dual-index probe (no neighbour false positives).
    // Opt in for the conservative LDRH A/B only.
    static const bool on =
        environment_flag_enabled("VITREO_JIT_STORE_PROBE_HALFWORD", false);
    return on;
}

bool JitCompiler::linear32_store_is_nzcv_clean() const noexcept {
    return !m_block_is_64bit && m_cfg.linear_jit && m_cfg.linear32_check_map &&
           !m_store_ring_stores && m_memory.is_linear();
}

bool JitCompiler::linear64_store_is_nzcv_clean() const noexcept {
    // The two conditions below are exactly the ones every caller uses to
    // choose `emit_linear64_store` over `emit_x64_region_store`; keeping the
    // same test here is what makes "clean" and "took the clean lowering" the
    // same fact rather than two that can drift apart.
    return m_block_is_64bit && m_cfg.linear64 && m_linear_base_pinned &&
           !m_store_ring_stores;
}





// VITREO_STORE_RING records guest stores inside the write helpers, but the
// linear and write-region fast paths store inline and never call one, so a ring
// run would miss exactly the stores it is meant to explain. A preserving
// recorder observes inline scalar and vector stores in both bitnesses.
// Diagnostic-only and slow by construction: retain the actual memory lowering
// because changing the memory model is not a neutral diagnostic.
extern "C" void vitreo_store_ring_trampoline();

extern "C" void vitreo_store_ring_trampoline16();

void JitCompiler::emit_store_ring_record_vec(u8 addr_arm_reg) {
    // Capture the address before staging arguments, including an address
    // held in an argument register. The caller staged the halves in X10/X11.
    emit(arm64::sub_x_imm(31, 31, 48));
    emit(arm64::stp_x_off(addr_arm_reg, 30, 31, 0));
    emit(arm64::stp_x_off(9, 16, 31, 2));
    emit(arm64::str_x_uoff(17, 31, 4));
    emit(m_block_is_64bit ? arm64::ldr_x_uoff(9, 31, 0)
                         : arm64::ldr_w_uoff(9, 31, 0));
    note_unrelocatable_host_bake();
    emit_mov_imm64(16, static_cast<i64>(m_diagnostic_instruction_rip));
    emit_mov_imm64(17, static_cast<i64>(reinterpret_cast<uintptr_t>(
                           &vitreo_store_ring_trampoline16)));
    emit(0xD63F0000u | (17u << 5));
    emit(arm64::ldr_x_uoff(30, 31, 1));
    emit(arm64::ldp_x_off(9, 16, 31, 2));
    emit(arm64::ldr_x_uoff(17, 31, 4));
    emit(arm64::add_x_imm(31, 31, 48));
}

void JitCompiler::emit_store_ring_record_preloaded(u8 addr_arm_reg, u32 width) {
    (void)emit_store_ring_helper(10, addr_arm_reg, width);
}

bool JitCompiler::emit_store_ring_helper(u8 src_arm_reg, u8 addr_arm_reg,
                                         u32 width) {
    if (!m_store_ring_stores) return false;
    // Stage both inputs before modifying any argument register; restore the
    // store lowering's complete scratch set and LR after the recorder.
    emit(arm64::sub_x_imm(31, 31, 64));
    emit(arm64::stp_x_off(src_arm_reg, addr_arm_reg, 31, 0));
    emit(arm64::stp_x_off(9, 10, 31, 2));
    emit(arm64::stp_x_off(11, 16, 31, 4));
    emit(arm64::str_x_uoff(30, 31, 6));
    emit(arm64::str_x_uoff(17, 31, 7));
    emit(m_block_is_64bit ? arm64::ldr_x_uoff(9, 31, 1)
                         : arm64::ldr_w_uoff(9, 31, 2));
    emit(width == 8 ? arm64::ldr_x_uoff(10, 31, 0)
                    : arm64::ldr_w_uoff(10, 31, 0));
    emit(arm64::movz_w(11, static_cast<u16>(width)));
    note_unrelocatable_host_bake();
    emit_mov_imm64(16, static_cast<i64>(m_diagnostic_instruction_rip));
    emit_mov_imm64(17, static_cast<i64>(reinterpret_cast<uintptr_t>(
                           &vitreo_store_ring_trampoline)));
    emit(0xD63F0000u | (17u << 5));
    emit(arm64::ldp_x_off(9, 10, 31, 2));
    emit(arm64::ldp_x_off(11, 16, 31, 4));
    emit(arm64::ldr_x_uoff(30, 31, 6));
    emit(arm64::ldr_x_uoff(17, 31, 7));
    emit(arm64::add_x_imm(31, 31, 64));
    return false;
}





















u8 JitCompiler::emit_addr32(u8 scratch, const MemoryOperand& mem, u64 rip_val,
                            bool include_segment_base) {
    AttrScope attr_scope(*this, EmitAttribution::Address);
    if (m_block_is_64bit) {
        emit_compute_ea64(scratch, mem, rip_val, include_segment_base);
        return scratch;
    }
    // [reg] with no index/disp is the pin itself. Copying it to scratch
    // just to LDR [linear, scratch] wastes a MOV on every [ebx]/[edx].
    if (!mem.is_rip_relative() && !mem.is_disp_only() && mem.has_base() &&
        !mem.has_index() && mem.disp == 0 && m_pins_live && mem.base < 8 &&
        m_pin_arm[mem.base] != 0xFF &&
        (!include_segment_base || mem.segment == SegReg::Default)) {
        return m_pin_arm[mem.base];
    }
    emit_compute_ea(scratch, mem, rip_val, include_segment_base);
    return scratch;
}

/// `VITREO_JIT_ESP_FRAME=1` re-enables the ESP-frame fast path. Default off.
///
/// X15 caches `linear_base + ESP` so an `[esp+disp]` access is one LDR. The
/// cache is tracked by a COMPILE-TIME flag (`m_esp_frame_live`) describing a
/// RUNTIME register, and that model is not airtight: one guest dies with a
/// host SIGSEGV (`LDR W0,[X15,#44]` with X15 a small integer), and 32-bit
/// /GS frames (Intel SDM Vol. 2 INT 29 / `__fastfail(2)` →
/// `STATUS_STACK_BUFFER_OVERRUN`) smash the cookie at `[esp+size-8]` when
/// the frame is on. `VITREO_JIT_ESP_FRAME=0` (now the default) clears both.
///
/// The knob stays so a later pin/X15 fix can be A/B'd without another
/// compiler patch.
[[nodiscard]] static bool esp_frame_enabled() noexcept {
    static const bool on = [] {
        const char* v = std::getenv("VITREO_JIT_ESP_FRAME");
        return v != nullptr && v[0] == '1' && v[1] == '\0';
    }();
    return on;
}

bool JitCompiler::try_esp_disp_load32(u8 dst_arm_reg, const MemoryOperand& mem) {
    if (!esp_frame_enabled()) return false;
    if (m_block_is_64bit || !m_pins_live || !m_linear_base_pinned) return false;
    if (mem.is_rip_relative() || mem.is_disp_only() || !mem.has_base() ||
        mem.has_index() || mem.segment != SegReg::Default) {
        return false;
    }
    if (mem.base != static_cast<u8>(RegIndex::RSP)) return false;
    if (mem.disp < 0 || (mem.disp & 3) != 0 || mem.disp > 16380) return false;
    // Always rebuild X15. `m_esp_frame_live` is compile-time and does not
    // follow internal-Jcc CFG: `je skip; mov eax,[esp+4]; skip: mov ecx,[esp+8]`
    // compiled the establisher only on the fall-through, then a taken JE
    // loaded through a dead X15. Intel SDM Vol. 2 MOV r32, r/m32: [esp+disp]
    // uses the current ESP. A stale frame smashes 32-bit /GS cookies
    // (__fastfail(2) / STATUS_STACK_BUFFER_OVERRUN) and is the same
    // "consumer without establisher" host SIGSEGV one guest hit.
    emit(arm64::add_x_uxtw(REG_ESP_FRAME, REG_LINEAR_BASE, REG_PIN_ESP));
    m_esp_frame_live = true;
    // Under Ordered, emit_tso_barrier() is a no-op (strict_tso=false). A plain
    // LDR [X15,#disp] then left stack locals unordered while the matching
    // store path already used STLR — classic spin-flag asymmetry. Fold the
    // host address and use the same fault-patchable LDAPR site as other
    // Ordered 32-bit loads.
    if (tso_ordered()) {
        u8 addr_x = REG_ESP_FRAME;
        if (mem.disp != 0) {
            addr_x = REG_SCRATCH4;
            if (fits_uimm12(static_cast<u32>(mem.disp))) {
                emit(arm64::add_x_imm(REG_SCRATCH4, REG_ESP_FRAME,
                                      static_cast<u32>(mem.disp)));
            } else {
                emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(mem.disp));
                emit(arm64::add_x(REG_SCRATCH4, REG_ESP_FRAME, REG_SCRATCH3));
            }
        }
        emit_x64_tso_load(dst_arm_reg, addr_x, 8, 4);
        return true;
    }
    emit(arm64::ldr_w_uoff(dst_arm_reg, REG_ESP_FRAME,
                           static_cast<u32>(mem.disp) >> 2));
    emit_tso_barrier();
    return true;
}

bool JitCompiler::try_esp_disp_store32(u8 src_arm_reg, const MemoryOperand& mem) {
    if (!esp_frame_enabled()) return false;
    if (m_block_is_64bit || !m_pins_live || !m_linear_base_pinned) return false;
    if (!m_cfg.linear_jit || !m_memory.is_linear()) return false;
    if (mem.is_rip_relative() || mem.is_disp_only() || !mem.has_base() ||
        mem.has_index() || mem.segment != SegReg::Default) {
        return false;
    }
    if (mem.base != static_cast<u8>(RegIndex::RSP)) return false;
    if (mem.disp < 0 || (mem.disp & 3) != 0 || mem.disp > 16380) return false;

    // See try_esp_disp_load32: do not skip this ADD because a prior instruction
    // in linear order already set `m_esp_frame_live`.
    emit(arm64::add_x_uxtw(REG_ESP_FRAME, REG_LINEAR_BASE, REG_PIN_ESP));
    m_esp_frame_live = true;

    // Guest EA for the SMC/span probe and write-watch (ESP can alias code).
    const u8 guest_ea = REG_SCRATCH2;
    if (mem.disp == 0) {
        emit(arm64::mov_w(guest_ea, REG_PIN_ESP));
    } else if (fits_uimm12(static_cast<u32>(mem.disp))) {
        emit(arm64::add_w_imm(guest_ea, REG_PIN_ESP,
                              static_cast<u32>(mem.disp)));
    } else {
        // LDR frame offset allows up to 16380; ADD imm12 only to 4095.
        emit(arm64::mov_w(guest_ea, REG_PIN_ESP));
        emit_mov_imm32(REG_SCRATCH3, static_cast<u32>(mem.disp));
        emit(arm64::add_w(guest_ea, guest_ea, REG_SCRATCH3));
    }

    const bool hold = should_hold_nzcv();
    // See emit_guest_store32: the check-byte lowering is NZCV-clean, so the
    // flag save moves into its cold stub.
    const bool nzcv_clean = linear32_store_is_nzcv_clean() ||
                           linear64_store_is_nzcv_clean();
    if (hold && !nzcv_clean) emit_nzcv_spill();
    emit_tso_barrier();
    diagnostics::perf_add_linear_mem_emit(true);
    m_flags.stub_spill_nzcv = hold && nzcv_clean;
    emit_linear_store(src_arm_reg, guest_ea, 4, mem.disp);
    m_flags.stub_spill_nzcv = false;
    emit_tso_store_tail_barrier();
    if (hold && !nzcv_clean) emit_nzcv_fill();
    return true;
}

} // namespace vitreo::cpu::jit

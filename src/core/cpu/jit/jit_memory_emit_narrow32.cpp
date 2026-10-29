// Sub-dword guest accesses of the Tier-0 memory lowering: the 8- and
// 16-bit guest loads and stores of 32-bit blocks. Each forwards to the
// x64 paths when the block is 64-bit.
//
// Split out of jit_memory_emit.cpp as pure code motion: every line below
// the includes is verbatim from that file, whose header has the history.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>
#include "jit_compiler.hpp"
#include "jit_emit_internal.hpp"
#include "jit_symbol_map.hpp"
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
#include "../../memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/base/perf_window.hpp"  // inlinable perf_window_active_fast()
#include <vitreo/diagnostics.hpp>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <array>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

namespace vitreo::cpu::jit {

void JitCompiler::emit_guest_load8(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (m_block_is_64bit) {
        if (m_cfg.linear64 && m_linear_base_pinned) {
            emit_linear64_load(dst_arm_reg, addr_arm_reg, 1);
        } else {
            emit_x64_region_load(dst_arm_reg, addr_arm_reg, 1);
        }
        return;
    }
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        u8 d = (dst_arm_reg != 0) ? dst_arm_reg : 0;
        if (m_linear_base_pinned) {
            emit_linear_load8(d, addr_arm_reg);
            emit_tso_barrier();
        } else if (tso_ordered()) {
            emit(arm64::mov_w(9, addr_arm_reg));
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
            const u8 addr_x = emit_linear_addr_x(REG_SCRATCH3, 9, 9);
            emit_x64_tso_load(d, addr_x, 8, 1);
        } else {
            emit(arm64::mov_w(9, addr_arm_reg));
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
            emit(arm64::ldrb_uxtw(d, REG_SCRATCH3, 9));
            emit_tso_barrier();
        }
        return;
    }
    u8 dst = (dst_arm_reg != 0) ? dst_arm_reg : 0;

    emit(arm64::mov_w(9, addr_arm_reg));

    // Region 1
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r1_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_END >> 2));
    emit(arm64::subs_w(31, 9, 4));
    u32 r1_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_HOST >> 3));
    emit_region_inline_load(dst, 3, 4, 1);
    u32 r1_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Region 2
    u32 r2_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r2_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_END >> 2));
    emit(arm64::subs_w(31, 9, 4));
    u32 r2_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_HOST >> 3));
    emit_region_inline_load(dst, 3, 4, 1);
    u32 r2_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Slow path
    u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit_publish_rip_for_raising_helper(3);  // X3: dead on the miss path
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::mov_w(1, 9));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_READ8 >> 3));
    emit_helper_blr();
    emit_exit_if_pending_raise();
    if (dst_arm_reg != 0) {
        emit(arm64::mov_w(dst_arm_reg, 0));
    }
    if (tso_ordered()) {
        emit(0xD5033BBFu);  // DMB ISH — helper acquire (fast path is LDAPR)
    }

    u32 done_idx = static_cast<u32>(m_code_buf.size());
    emit_tso_barrier();
    m_code_buf[r1_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(r2_idx - r1_blo_idx) * 4);
    m_code_buf[r1_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(r2_idx - r1_bhs_idx) * 4);
    m_code_buf[r1_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r1_done_idx) * 4);
    m_code_buf[r2_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(slow_idx - r2_blo_idx) * 4);
    m_code_buf[r2_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(slow_idx - r2_bhs_idx) * 4);
    m_code_buf[r2_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r2_done_idx) * 4);
}
// See emit_guest_store32 for why this uses the wregion slots rather than the
// region slots the load path uses. A single byte cannot straddle the region
// end, so the upper-bound test compares the address itself.
void JitCompiler::emit_guest_store8(u8 src_arm_reg, u8 addr_arm_reg) {
    publish_pending_stub_store();
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    const bool hold = should_hold_nzcv();
    // The check-byte linear store below is NZCV-clean end to end (LDR/UBFX/
    // LDRB/ORR/CBNZ/STR), so a store standing between an x86 flag producer and
    // its consumer no longer pays an inline MRS/STR + LDR/MSR. Its cold stub
    // saves and restores the flags around the helper instead.
    const bool nzcv_clean = linear32_store_is_nzcv_clean() ||
                           linear64_store_is_nzcv_clean();
    if (hold && !nzcv_clean) emit_nzcv_spill();
    (void)emit_store_ring_helper(src_arm_reg, addr_arm_reg, 1);
    if (m_block_is_64bit) {
        if (m_cfg.linear64 && m_linear_base_pinned) {
            m_flags.stub_spill_nzcv = hold && nzcv_clean;
            emit_linear64_store(src_arm_reg, addr_arm_reg, 1);
            m_flags.stub_spill_nzcv = false;
        } else {
            emit_x64_region_store(src_arm_reg, addr_arm_reg, 1);
        }
        if (hold && !nzcv_clean) emit_nzcv_fill();
        return;
    }
    emit_tso_barrier();
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        m_flags.stub_spill_nzcv = hold && nzcv_clean;
        emit_linear_store(src_arm_reg, addr_arm_reg, 1);
        m_flags.stub_spill_nzcv = false;
        emit_tso_store_tail_barrier();
        if (hold && !nzcv_clean) emit_nzcv_fill();
        return;
    }
    emit(arm64::mov_w(9, addr_arm_reg));
    emit(arm64::mov_w(10, src_arm_reg));

    // Write-region 1
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r1_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION_END >> 2));
    emit(arm64::subs_w(31, 9, 4));
    u32 r1_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION_HOST >> 3));
    emit_region_inline_store(10, 3, 4, 1);
    u32 r1_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Write-region 2
    u32 r2_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION2_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r2_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION2_END >> 2));
    emit(arm64::subs_w(31, 9, 4));
    u32 r2_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION2_HOST >> 3));
    emit_region_inline_store(10, 3, 4, 1);
    u32 r2_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Slow path
    u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit_publish_rip_for_raising_helper(3);  // X3: dead on the miss path
    if (tso_ordered()) {
        emit(0xD5033BBFu);  // DMB ISH — helper release (fast path is STLR)
    }
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::mov_w(1, 9));
    emit(arm64::mov_w(2, 10));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_WRITE8 >> 3));
    emit_helper_blr();
    emit_exit_if_pending_raise();

    u32 done_idx = static_cast<u32>(m_code_buf.size());
    emit_tso_store_tail_barrier();
    if (hold) emit_nzcv_fill();
    m_code_buf[r1_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(r2_idx - r1_blo_idx) * 4);
    m_code_buf[r1_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(r2_idx - r1_bhs_idx) * 4);
    m_code_buf[r1_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r1_done_idx) * 4);
    m_code_buf[r2_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(slow_idx - r2_blo_idx) * 4);
    m_code_buf[r2_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(slow_idx - r2_bhs_idx) * 4);
    m_code_buf[r2_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r2_done_idx) * 4);
}
void JitCompiler::emit_guest_load16(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (m_block_is_64bit) {
        if (m_cfg.linear64 && m_linear_base_pinned) {
            emit_linear64_load(dst_arm_reg, addr_arm_reg, 2);
        } else {
            emit_guest_load16_64(dst_arm_reg, addr_arm_reg);
        }
        return;
    }
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        u8 d = (dst_arm_reg != 0) ? dst_arm_reg : 0;
        if (m_linear_base_pinned) {
            emit_linear_load16(d, addr_arm_reg);
            emit_tso_barrier();
        } else if (tso_ordered()) {
            emit(arm64::mov_w(9, addr_arm_reg));
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
            const u8 addr_x = emit_linear_addr_x(REG_SCRATCH3, 9, 9);
            emit_x64_tso_load(d, addr_x, 8, 2);
        } else {
            emit(arm64::mov_w(9, addr_arm_reg));
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
            emit(arm64::ldrh_uxtw(d, REG_SCRATCH3, 9));
            emit_tso_barrier();
        }
        return;
    }
    u8 dst = (dst_arm_reg != 0) ? dst_arm_reg : 0;

    emit(arm64::mov_w(9, addr_arm_reg));

    // Region 1
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r1_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_END >> 2));
    emit(arm64::add_x_imm(8, 9, 1));                            // W8 = addr + 1 (last byte)
    emit(arm64::subs_w(31, 8, 4));
    u32 r1_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_HOST >> 3));
    emit_region_inline_load(dst, 3, 4, 2);
    u32 r1_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Region 2
    u32 r2_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r2_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_END >> 2));
    emit(arm64::add_x_imm(8, 9, 1));                            // W8 = addr + 1 (last byte)
    emit(arm64::subs_w(31, 8, 4));
    u32 r2_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_HOST >> 3));
    emit_region_inline_load(dst, 3, 4, 2);
    u32 r2_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Slow path
    u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit_publish_rip_for_raising_helper(3);  // X3: dead on the miss path
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::mov_w(1, 9));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_READ16 >> 3));
    emit_helper_blr();
    emit_exit_if_pending_raise();
    if (dst_arm_reg != 0) {
        emit(arm64::mov_w(dst_arm_reg, 0));
    }
    if (tso_ordered()) {
        emit(0xD5033BBFu);  // DMB ISH — helper acquire (fast path is LDAPR)
    }

    u32 done_idx = static_cast<u32>(m_code_buf.size());
    emit_tso_barrier();
    m_code_buf[r1_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(r2_idx - r1_blo_idx) * 4);
    m_code_buf[r1_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(r2_idx - r1_bhs_idx) * 4);
    m_code_buf[r1_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r1_done_idx) * 4);
    m_code_buf[r2_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(slow_idx - r2_blo_idx) * 4);
    m_code_buf[r2_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(slow_idx - r2_bhs_idx) * 4);
    m_code_buf[r2_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r2_done_idx) * 4);
}
// Inline two-region 16-bit store. Same design as emit_guest_store32 (see its
// comment): the wregion slots only ever hold writable, code-free regions, so
// the store needs no permission test and no SMC notification. jit_mem_write16
// showed up as the hot memory op on the decompression compute threads, and
// each call was paying the helper + notify_executable_write cost this avoids.
void JitCompiler::emit_guest_store16(u8 src_arm_reg, u8 addr_arm_reg) {
    publish_pending_stub_store();
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    const bool hold = should_hold_nzcv();
    // The check-byte linear store below is NZCV-clean end to end (LDR/UBFX/
    // LDRB/ORR/CBNZ/STR), so a store standing between an x86 flag producer and
    // its consumer no longer pays an inline MRS/STR + LDR/MSR. Its cold stub
    // saves and restores the flags around the helper instead.
    const bool nzcv_clean = linear32_store_is_nzcv_clean() ||
                           linear64_store_is_nzcv_clean();
    if (hold && !nzcv_clean) emit_nzcv_spill();
    (void)emit_store_ring_helper(src_arm_reg, addr_arm_reg, 2);
    if (m_block_is_64bit) {
        if (m_cfg.linear64 && m_linear_base_pinned) {
            m_flags.stub_spill_nzcv = hold && nzcv_clean;
            emit_linear64_store(src_arm_reg, addr_arm_reg, 2);
            m_flags.stub_spill_nzcv = false;
        } else {
            emit_x64_region_store(src_arm_reg, addr_arm_reg, 2);
        }
        if (hold && !nzcv_clean) emit_nzcv_fill();
        return;
    }
    emit_tso_barrier();
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        m_flags.stub_spill_nzcv = hold && nzcv_clean;
        emit_linear_store(src_arm_reg, addr_arm_reg, 2);
        m_flags.stub_spill_nzcv = false;
        emit_tso_store_tail_barrier();
        if (hold && !nzcv_clean) emit_nzcv_fill();
        return;
    }
    emit(arm64::mov_w(9, addr_arm_reg));                         // W9 = addr
    emit(arm64::mov_w(10, src_arm_reg));                         // W10 = value

    // Write-region 1 bounds check
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r1_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION_END >> 2));
    emit(arm64::add_x_imm(8, 9, 1));                            // X8 = addr + 1 (last byte)
    // 64-bit compare so a store straddling the 4 GB boundary misses cleanly.
    emit(arm64::subs_x(31, 8, 4));
    u32 r1_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION_HOST >> 3));
    emit_region_inline_store(10, 3, 4, 2);
    u32 r1_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Write-region 2 bounds check
    u32 r2_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION2_BASE >> 2));
    emit(arm64::subs_w(31, 9, 3));
    u32 r2_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION2_END >> 2));
    emit(arm64::add_x_imm(8, 9, 1));                            // X8 = addr + 1 (last byte)
    emit(arm64::subs_x(31, 8, 4));
    u32 r2_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION2_HOST >> 3));
    emit_region_inline_store(10, 3, 4, 2);
    u32 r2_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Slow path: BLR to write16(JitContext*, addr, val)
    u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit_publish_rip_for_raising_helper(3);  // X3: dead on the miss path
    if (tso_ordered()) {
        emit(0xD5033BBFu);  // DMB ISH — helper release (fast path is STLR)
    }
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::mov_w(1, 9));
    emit(arm64::mov_w(2, 10));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_WRITE16 >> 3));
    emit_helper_blr();
    emit_exit_if_pending_raise();

    u32 done_idx = static_cast<u32>(m_code_buf.size());
    emit_tso_store_tail_barrier();
    if (hold) emit_nzcv_fill();
    m_code_buf[r1_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(r2_idx - r1_blo_idx) * 4);
    m_code_buf[r1_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(r2_idx - r1_bhs_idx) * 4);
    m_code_buf[r1_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r1_done_idx) * 4);
    m_code_buf[r2_blo_idx]  = arm64::b_cond(arm64::CC,
                                static_cast<i32>(slow_idx - r2_blo_idx) * 4);
    m_code_buf[r2_bhs_idx]  = arm64::b_cond(arm64::CS,
                                static_cast<i32>(slow_idx - r2_bhs_idx) * 4);
    m_code_buf[r2_done_idx] = arm64::b_imm(
                                static_cast<i32>(done_idx - r2_done_idx) * 4);
}

}  // namespace vitreo::cpu::jit

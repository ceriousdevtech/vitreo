// Guest memory access lowering for the Tier-0 stencil compiler.
//
// Extracted verbatim from jit_compiler.cpp (ADR-0006, step 3). That file was
// 8,896 lines and 209 methods on one class; this is the largest cohesive unit
// inside it -- every guest load and store, both linear paths, the x64 region
// cache, effective-address computation, and the cold stubs those queue.
//
// A MOVE, not a redesign: `JitEmissionGolden` is byte-identical across all 44
// snippets before and after. Nothing here was rewritten, reordered or
// reformatted, so this file's history is `git log --follow` on the ranges it
// came from.
//
// The split was chosen by cohesion rather than by line count. These 32
// definitions talk to each other and to the memory model; they barely touch
// the block/flag/link machinery that remains in jit_compiler.cpp.
//
// It was later split again, as pure code motion, toward the 500-line file
// guideline (.docs/architecture.md). This file keeps the 32-bit guest
// load/store entry points and effective-address formation; the rest of
// the lowering lives in its siblings:
//   jit_memory_emit_linear32.cpp        32-bit guest-base primitives and
//                                       the cold linear-store flush
//   jit_memory_emit_narrow32.cpp        8/16-bit accesses of 32-bit blocks
//   jit_memory_emit_guest64.cpp         x64 scalar paths, x64 region cache
//   jit_memory_emit_linear64_guard.cpp  linear64 guards and checked forms
//   jit_memory_emit_vec128.cpp          128-bit accesses
//   jit_memory_emit_vec256.cpp          256-bit accesses
//   jit_memory_emit_vec_cold_stubs.cpp  cold bodies of vector accesses
//   jit_memory_emit_cold_stubs.cpp      the block-tail cold memory stub flush
//   jit_memory_emit_process_stubs.cpp   process-wide helper page and stubs
//   jit_memory_emit_internal.hpp        what more than one part shares

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

void JitCompiler::emit_guest_load32(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    // Intel SDM Vol. 1 §3.3: in 64-bit mode the effective address is 64-bit
    // unless address-size 67H is used. The 32-bit linear/region paths zext
    // through W registers and wrap 0x14000xxxx onto 0x4000xxxx.
    if (m_block_is_64bit) {
        emit_guest_load32_64(dst_arm_reg, addr_arm_reg);
        return;
    }
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        diagnostics::perf_add_linear_mem_emit(true);
        u8 d = (dst_arm_reg != 0) ? dst_arm_reg : 0;
        if (m_linear_base_pinned) {
            emit_linear_load32(d, addr_arm_reg);
            emit_tso_barrier();
        } else if (tso_ordered()) {
            // Same Ordered contract as the pinned path: LDAPR/STLR need a
            // folded host address and a fault-patchable site. A plain LDR
            // plus emit_tso_barrier() is a no-op under Ordered and drops
            // x86-TSO for every non-pinned 32-bit load.
            emit(arm64::mov_w(9, addr_arm_reg));
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
            const u8 addr_x = emit_linear_addr_x(REG_SCRATCH3, 9, 9);
            emit_x64_tso_load(d, addr_x, 8, 4);
        } else {
            emit(arm64::mov_w(9, addr_arm_reg));
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
            emit(arm64::ldr_w_uxtw(d, REG_SCRATCH3, 9));
            emit_tso_barrier();
        }
        return;
    }
    diagnostics::perf_add_linear_mem_emit(false);
    // Dual-region inline fast path: check region1 (stack), then region2
    // (dynamically updated by slow-path helpers), then BLR slow path.
    u8 dst = (dst_arm_reg != 0) ? dst_arm_reg : 0;

    emit(arm64::mov_w(9, addr_arm_reg));                         // W9 = addr

    // Region 1 bounds check
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_BASE >> 2));      // W3 = region_base
    emit(arm64::subs_w(31, 9, 3));                               // CMP W9, W3
    u32 r1_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);                                                      // placeholder B.LO r2

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_END >> 2));       // W4 = region_end
    emit(arm64::add_x_imm(8, 9, 3));                            // W8 = addr + 3 (last byte)
    emit(arm64::subs_w(31, 8, 4));                               // CMP W8, W4
    u32 r1_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);                                                      // placeholder B.HS r2

    // Fast path 1: inline load from region1
    emit(arm64::sub_w(4, 9, 3));                                 // W4 = addr - base
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION_HOST >> 3));      // X3 = host_base
    emit_region_inline_load(dst, 3, 4, 4);                       // Wdst = *(u32*)(host + off)
    u32 r1_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);                                                      // placeholder B done

    // Region 2 bounds check
    u32 r2_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_BASE >> 2));     // W3 = region2_base
    emit(arm64::subs_w(31, 9, 3));                               // CMP W9, W3
    u32 r2_blo_idx = static_cast<u32>(m_code_buf.size());
    emit(0);                                                      // placeholder B.LO slow

    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_END >> 2));      // W4 = region2_end
    emit(arm64::add_x_imm(8, 9, 3));                            // W8 = addr + 3 (last byte)
    emit(arm64::subs_w(31, 8, 4));                               // CMP W8, W4
    u32 r2_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);                                                      // placeholder B.HS slow

    // Fast path 2: inline load from region2
    emit(arm64::sub_w(4, 9, 3));                                 // W4 = addr - base2
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_REGION2_HOST >> 3));     // X3 = host_base2
    emit_region_inline_load(dst, 3, 4, 4);                       // Wdst = *(u32*)(host2 + off)
    u32 r2_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);                                                      // placeholder B done

    // Slow path: BLR to C++ read32(JitContext*, addr)
    u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit_publish_rip_for_raising_helper(3);  // X3: dead on the miss path
    emit(arm64::mov_x(0, REG_JIT_CTX));                          // X0 = jit_ctx
    emit(arm64::mov_w(1, 9));                                    // W1 = addr
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                            JIT_CTX_OFF_READ32 >> 3));           // X16 = read32
    emit_helper_blr();                                           // BLR X16
    emit_exit_if_pending_raise();
    if (dst_arm_reg != 0) {
        emit(arm64::mov_w(dst_arm_reg, 0));                      // Wdst = W0
    }
    if (tso_ordered()) {
        // Fast path already used LDAPR; the C++ helper is unordered. Fence
        // still gets its acquire from emit_tso_barrier at the merge.
        emit(0xD5033BBFu);  // DMB ISH
    }

    u32 done_idx = static_cast<u32>(m_code_buf.size());
    emit_tso_barrier();
    // Patch branches
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

void JitCompiler::emit_guest_store32(u8 src_arm_reg, u8 addr_arm_reg) {
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
    (void)emit_store_ring_helper(src_arm_reg, addr_arm_reg, 4);
    if (m_block_is_64bit) {
        // Inline store over the full-width write region; a miss calls the same
        // write32 helper this used to call unconditionally.
        if (m_cfg.linear64 && m_linear_base_pinned) {
            m_flags.stub_spill_nzcv = hold && nzcv_clean;
            emit_linear64_store(src_arm_reg, addr_arm_reg, 4);
            m_flags.stub_spill_nzcv = false;
        } else {
            emit_x64_region_store(src_arm_reg, addr_arm_reg, 4);
        }
        if (hold && !nzcv_clean) emit_nzcv_fill();
        return;
    }
    emit_tso_barrier();
    if (m_cfg.linear_jit && m_memory.is_linear()) {
        diagnostics::perf_add_linear_mem_emit(true);
        m_flags.stub_spill_nzcv = hold && nzcv_clean;
        emit_linear_store(src_arm_reg, addr_arm_reg, 4);
        m_flags.stub_spill_nzcv = false;
        emit_tso_store_tail_barrier();
        if (hold && !nzcv_clean) emit_nzcv_fill();
        return;
    }
    diagnostics::perf_add_linear_mem_emit(false);
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
    emit(arm64::add_x_imm(8, 9, 3));                            // X8 = addr + 3 (last byte)
    // 64-bit compare: X8 cannot wrap, so a store straddling the 4 GB boundary
    // misses rather than aliasing to a low address that passes a 32-bit test.
    emit(arm64::subs_x(31, 8, 4));
    u32 r1_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Fast path 1: inline store to wregion1
    emit(arm64::sub_w(4, 9, 3));                                 // W4 = offset
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION_HOST >> 3));     // X3 = host_base
    emit_region_inline_store(10, 3, 4, 4);                       // store
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
    emit(arm64::add_x_imm(8, 9, 3));                            // X8 = addr + 3 (last byte)
    emit(arm64::subs_x(31, 8, 4));
    u32 r2_bhs_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Fast path 2: inline store to wregion2
    emit(arm64::sub_w(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_WREGION2_HOST >> 3));
    emit_region_inline_store(10, 3, 4, 4);
    u32 r2_done_idx = static_cast<u32>(m_code_buf.size());
    emit(0);

    // Slow path: BLR to write32(JitContext*, addr, val)
    u32 slow_idx = static_cast<u32>(m_code_buf.size());
    emit_publish_rip_for_raising_helper(3);  // X3: dead on the miss path
    if (tso_ordered()) {
        // Fast path already used STLR; leading emit_tso_barrier is a no-op
        // under Ordered. Release before the unordered C++ helper.
        emit(0xD5033BBFu);  // DMB ISH
    }
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::mov_w(1, 9));
    emit(arm64::mov_w(2, 10));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX,
                            JIT_CTX_OFF_WRITE32 >> 3));
    emit_helper_blr();
    emit_exit_if_pending_raise();

    u32 done_idx = static_cast<u32>(m_code_buf.size());
    emit_tso_store_tail_barrier();
    if (hold) emit_nzcv_fill();
    // Patch branches
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

void JitCompiler::emit_compute_ea(u8 dst_arm_reg, const MemoryOperand& mem,
                                   u64 rip_val, bool include_segment_base) {
    AttrScope attr_scope(*this, EmitAttribution::Address);
    // SSE / FPU / string lowerings call this helper from both bitnesses. In
    // long mode the address is a full canonical u64 (RIP-relative, R8-R15
    // bases, 64-bit displ); truncating through the 32-bit path below would
    // silently wrap high addresses and is exactly how a "just enable
    // compile_sse for x64" change would go wrong. Forward instead.
    if (m_block_is_64bit) {
        emit_compute_ea64(dst_arm_reg, mem, rip_val, include_segment_base);
        return;
    }

    bool have_value = false;

    // REG_EA_TMP is this function's internal scratch. If the caller asked for
    // the result in that same register, using it for the index/displacement
    // would destroy the partially-built address — e.g. [esp+esi*4+0x3c] would
    // compute esi*4+esi*4+0x3c instead of esp+esi*4+0x3c. Pick a temp that
    // cannot alias the destination instead.
    const u8 ea_tmp = (dst_arm_reg == REG_EA_TMP) ? REG_SCRATCH4 : REG_EA_TMP;

    // Pinned ESP/EBP + disp (no index): add the displacement to the pin
    // instead of reloading the GPR from CpuState.
    if (!mem.is_rip_relative() && !mem.is_disp_only() && mem.has_base() &&
        !mem.has_index() && m_pins_live && mem.base < 8 &&
        m_pin_arm[mem.base] != 0xFF) {
        const u8 pin = m_pin_arm[mem.base];
        if (mem.disp == 0) {
            if (dst_arm_reg != pin) emit(arm64::mov_w(dst_arm_reg, pin));
        } else if (mem.disp > 0 && fits_uimm12(static_cast<u32>(mem.disp))) {
            emit(arm64::add_w_imm(dst_arm_reg, pin, static_cast<u32>(mem.disp)));
        } else if (mem.disp < 0 && fits_uimm12(static_cast<u32>(-mem.disp))) {
            emit(arm64::sub_w_imm(dst_arm_reg, pin,
                                   static_cast<u32>(-mem.disp)));
        } else {
            emit(arm64::mov_w(dst_arm_reg, pin));
            emit_mov_imm32(ea_tmp, static_cast<u32>(mem.disp));
            emit(arm64::add_w(dst_arm_reg, dst_arm_reg, ea_tmp));
        }
        if (include_segment_base && mem.segment != SegReg::Default) {
            u32 base_off = seg_base_offset(mem.segment);
            emit(arm64::ldr_x_uoff(ea_tmp, REG_CPU_STATE, base_off >> 3));
            emit(arm64::add_w(dst_arm_reg, dst_arm_reg, ea_tmp));
        }
        return;
    }

    // Pinned base+index: ADD Wd, Wbase, Windex, LSL #scale. Dest must not
    // alias a pin (callers use scratch). [esp+edx*8+disp] is the walk form.
    if (!mem.is_rip_relative() && !mem.is_disp_only() && mem.has_base() &&
        mem.has_index() && m_pins_live && mem.base < 8 && mem.index < 8 &&
        m_pin_arm[mem.base] != 0xFF && m_pin_arm[mem.index] != 0xFF) {
        const u8 pin_b = m_pin_arm[mem.base];
        const u8 pin_i = m_pin_arm[mem.index];
        if (dst_arm_reg != pin_b && dst_arm_reg != pin_i) {
            u8 shift = 0;
            if (mem.scale == 2) shift = 1;
            else if (mem.scale == 4) shift = 2;
            else if (mem.scale == 8) shift = 3;
            if (shift != 0) {
                emit(arm64::add_w_lsl(dst_arm_reg, pin_b, pin_i, shift));
            } else {
                emit(arm64::add_w(dst_arm_reg, pin_b, pin_i));
            }
            if (mem.disp > 0 && fits_uimm12(static_cast<u32>(mem.disp))) {
                emit(arm64::add_w_imm(dst_arm_reg, dst_arm_reg,
                                       static_cast<u32>(mem.disp)));
            } else if (mem.disp < 0 &&
                       fits_uimm12(static_cast<u32>(-mem.disp))) {
                emit(arm64::sub_w_imm(dst_arm_reg, dst_arm_reg,
                                       static_cast<u32>(-mem.disp)));
            } else if (mem.disp != 0) {
                emit_mov_imm32(ea_tmp, static_cast<u32>(mem.disp));
                emit(arm64::add_w(dst_arm_reg, dst_arm_reg, ea_tmp));
            }
            if (include_segment_base && mem.segment != SegReg::Default) {
                u32 base_off = seg_base_offset(mem.segment);
                emit(arm64::ldr_x_uoff(ea_tmp, REG_CPU_STATE, base_off >> 3));
                emit(arm64::add_w(dst_arm_reg, dst_arm_reg, ea_tmp));
            }
            return;
        }
    }

    if (mem.is_rip_relative()) {
        emit_mov_imm32(dst_arm_reg, static_cast<u32>(rip_val + mem.disp));
        have_value = true;
    } else if (mem.is_disp_only()) {
        emit_mov_imm32(dst_arm_reg, static_cast<u32>(mem.disp));
        have_value = true;
    } else {
        if (mem.has_base()) {
            emit_load_gpr32(dst_arm_reg, mem.base);
            have_value = true;
        }
        if (mem.has_index()) {
            emit_load_gpr32(ea_tmp, mem.index);
            if (mem.scale > 1) {
                u8 shift = 0;
                if (mem.scale == 2) shift = 1;
                else if (mem.scale == 4) shift = 2;
                else if (mem.scale == 8) shift = 3;
                emit(arm64::lsl_w_imm(ea_tmp, ea_tmp, shift));
            }
            if (have_value) {
                emit(arm64::add_w(dst_arm_reg, dst_arm_reg, ea_tmp));
            } else {
                emit(arm64::mov_w(dst_arm_reg, ea_tmp));
                have_value = true;
            }
        }
        if (mem.disp != 0) {
            if (!have_value) {
                emit_mov_imm32(dst_arm_reg, static_cast<u32>(mem.disp));
                have_value = true;
            } else if (mem.disp > 0 && fits_uimm12(static_cast<u32>(mem.disp))) {
                emit(arm64::add_x_imm(dst_arm_reg, dst_arm_reg,
                                       static_cast<u32>(mem.disp)));
            } else if (mem.disp < 0 && fits_uimm12(static_cast<u32>(-mem.disp))) {
                emit(arm64::sub_x_imm(dst_arm_reg, dst_arm_reg,
                                       static_cast<u32>(-mem.disp)));
            } else {
                emit_mov_imm32(ea_tmp, static_cast<u32>(mem.disp));
                emit(arm64::add_w(dst_arm_reg, dst_arm_reg, ea_tmp));
            }
        }
    }

    if (!have_value) {
        emit(arm64::movz_w(dst_arm_reg, 0));
    }

    // Add segment base for FS:/GS: overrides (TLS etc.) when requested.
    if (include_segment_base && mem.segment != SegReg::Default) {
        u32 base_off = seg_base_offset(mem.segment);
        // base is u64; load it, then 32-bit add (truncates to 32 bits)
        emit(arm64::ldr_x_uoff(ea_tmp, REG_CPU_STATE, base_off >> 3));
        emit(arm64::add_w(dst_arm_reg, dst_arm_reg, ea_tmp));
    }
}

}  // namespace vitreo::cpu::jit

// The x64 scalar guest-access paths of the Tier-0 memory lowering: the
// 16/32-bit x64 entry points, the linear64 load/store fast paths and
// the x64 region-cache load/store.
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
#include "jit_memory_emit_internal.hpp"

namespace vitreo::cpu::jit {

// Long-mode narrow loads always use the full-width cache, including for guest
// addresses below 4 GB. The legacy cache is re-seeded with code and stack
// regions on dispatcher entry, so using it for x64 heap traffic made a hot
// extraction buffer miss again after every linked-chain exit. The full-width
// read16-specific slots are helper-managed and persist until the memory
// generation changes. Other-width reads cannot evict them.
void JitCompiler::emit_guest_load16_64(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (m_cfg.linear64 && m_linear_base_pinned) {
        emit_linear64_load(dst_arm_reg, addr_arm_reg, 2);
        return;
    }
    m_flags.load_clobbered_nzcv = true;  // bounds check below is a SUBS
    emit(arm64::mov_x(9, addr_arm_reg));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_X64_READ16_REGION_BASE >> 3));
    emit(arm64::subs_x(31, 9, 3));
    const usize below_cache_pos = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_X64_READ16_REGION_END >> 3));
    emit(arm64::add_x_imm(8, 9, 1));
    emit(arm64::subs_x(31, 8, 4));
    const usize above_cache_pos = m_code_buf.size();
    emit(0);
    emit(arm64::sub_x(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_X64_READ16_REGION_HOST >> 3));
    emit(arm64::add_x(17, 3, 4));
    emit_x64_tso_load(dst_arm_reg, 17, 8, 2);
    const usize cache_done_pos = m_code_buf.size();
    emit(0);

    const usize cache2_pos = m_code_buf.size();
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_X64_READ16_REGION2_BASE >> 3));
    emit(arm64::subs_x(31, 9, 3));
    const usize below_cache2_pos = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(4, REG_JIT_CTX,
                            JIT_CTX_OFF_X64_READ16_REGION2_END >> 3));
    emit(arm64::add_x_imm(8, 9, 1));
    emit(arm64::subs_x(31, 8, 4));
    const usize above_cache2_pos = m_code_buf.size();
    emit(0);
    emit(arm64::sub_x(4, 9, 3));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                            JIT_CTX_OFF_X64_READ16_REGION2_HOST >> 3));
    emit(arm64::add_x(17, 3, 4));
    emit_x64_tso_load(dst_arm_reg, 17, 8, 2);
    const usize cache2_done_pos = m_code_buf.size();
    emit(0);

    const usize slow_pos = m_code_buf.size();
    // The cache probes reuse X3/X4, which may also be the caller's address
    // register. X9 has held the full effective address since entry.
    emit(arm64::mov_x(1, 9));
    emit(arm64::mov_x(0, REG_JIT_CTX));
    emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_READ16 >> 3));
    emit_helper_blr();
    emit_exit_if_pending_raise();
    if (dst_arm_reg != 0)
        emit(arm64::mov_w(dst_arm_reg, 0));
    if (m_cfg.tso_mode != TsoMode::Off)
        emit(0xD5033BBFu);  // DMB ISH — acquire helper fallback

    const usize done_pos = m_code_buf.size();
    m_code_buf[below_cache_pos] = arm64::b_cond(
        arm64::CC, static_cast<i32>(cache2_pos - below_cache_pos) * 4);
    m_code_buf[above_cache_pos] = arm64::b_cond(
        arm64::CS, static_cast<i32>(cache2_pos - above_cache_pos) * 4);
    m_code_buf[cache_done_pos] = arm64::b_imm(
        static_cast<i32>(done_pos - cache_done_pos) * 4);
    m_code_buf[below_cache2_pos] = arm64::b_cond(
        arm64::CC, static_cast<i32>(slow_pos - below_cache2_pos) * 4);
    m_code_buf[above_cache2_pos] = arm64::b_cond(
        arm64::CS, static_cast<i32>(slow_pos - above_cache2_pos) * 4);
    m_code_buf[cache2_done_pos] = arm64::b_imm(
        static_cast<i32>(done_pos - cache2_done_pos) * 4);
}
// The x64 store region validates the whole 64-bit address, so these no longer
// need to split low-32 addresses out to the legacy 32-bit fast path.
void JitCompiler::emit_guest_store16_64(u8 src_arm_reg, u8 addr_arm_reg) {
    publish_pending_stub_store();
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    const bool hold = should_hold_nzcv();
    const bool nzcv_clean = linear64_store_is_nzcv_clean();
    if (hold && !nzcv_clean) emit_nzcv_spill();
    (void)emit_store_ring_helper(src_arm_reg, addr_arm_reg, 2);
    if (m_cfg.linear64 && m_linear_base_pinned) {
        m_flags.stub_spill_nzcv = hold && nzcv_clean;
        emit_linear64_store(src_arm_reg, addr_arm_reg, 2);
        m_flags.stub_spill_nzcv = false;
    } else {
        emit_x64_region_store(src_arm_reg, addr_arm_reg, 2);
    }
    if (hold && !nzcv_clean) emit_nzcv_fill();
}
void JitCompiler::emit_guest_load32_64(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (m_cfg.linear64 && m_linear_base_pinned) {
        emit_linear64_load(dst_arm_reg, addr_arm_reg, 4);
    } else {
        emit_x64_region_load(dst_arm_reg, addr_arm_reg, 4);
    }
}

void JitCompiler::emit_linear64_load(u8 dst_arm_reg, u8 addr_arm_reg,
                                     int width) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    // Blueprint section 19's memory fast-path counter. Instrumented here, in
    // the LEAF emitters, rather than at the emit_guest_*_64 dispatch: the leaves
    // are also called directly (the SSE and sub-word paths reach them without
    // passing through emit_guest_load32_64), so counting at the dispatch would
    // miss those. None of the eight leaves calls another, so this cannot double
    // count.
    diagnostics::perf_add_linear_mem_emit(true);

    const u32 helper_off = (width == 1) ? JIT_CTX_OFF_READ8
                         : (width == 2) ? JIT_CTX_OFF_READ16
                         : (width == 4) ? JIT_CTX_OFF_READ32
                                        : JIT_CTX_OFF_READ64;

    const bool access_proven = m_stack_entry_load_proof;

    // Every inline path below is NZCV-clean; only the cold stub's helper call
    // destroys the flags, so the stub saves them when guest flags are live
    // -- the same contract the linear64 stores keep. That is what lets a
    // load sit between an x86 flag producer and its reader.
    struct SpillScope {
        bool& flag;
        bool saved;
        ~SpillScope() { flag = saved; }
    } spill_scope{m_flags.stub_spill_nzcv, m_flags.stub_spill_nzcv};
    // Only for a claim made inside this block: a linked predecessor's carried
    // NZCV alone (pred_nzcv_live) would put the save in nearly every load
    // stub, so that claim is given up across the load instead, as before.
    // VITREO_JIT_NO_LOAD_NZCV_CLAIM=1 restores the old rule (a load ends
    // every claim) on one switch, for bisecting.
    static const bool no_load_claim =
        std::getenv("VITREO_JIT_NO_LOAD_NZCV_CLAIM") != nullptr;
    const bool in_block_claim = !no_load_claim &&
                                (m_flags.hold_nzcv ||
                                 m_flags.nzcv != NzcvState::None ||
                                 m_flags.incoming_nzcv != NzcvState::None);
    m_flags.stub_spill_nzcv = spill_scope.saved || in_block_claim;
    if (!m_flags.stub_spill_nzcv) m_flags.load_clobbered_nzcv = true;

    if (!access_proven && m_cfg.linear64_hw_protect &&
        !g_env_no_single_page_load) {
        emit_linear64_load_checked(dst_arm_reg, addr_arm_reg, width,
                                   helper_off);
        return;
    }

    // Use the incoming EA on the hot path when the guard will not clobber
    // it. X3/X4/X8/X17 are the check scratches, so those addresses still
    // move to X9 first. A pinned [reg] stays in the pin; the trampoline
    // materialises X9 for the helper ABI.
    const bool ea_is_scratch = addr_arm_reg == 3 || addr_arm_reg == 4 ||
                               addr_arm_reg == 8 || addr_arm_reg == 17;
    const u8 ea = (!m_cfg.linear64_hw_protect || ea_is_scratch)
                      ? u8{9} : addr_arm_reg;
    if (ea == 9 && addr_arm_reg != 9) emit(arm64::mov_x(9, addr_arm_reg));
    const u8 last_arm = (width > 1) ? 17 : ea;
    const usize overflow_pos = static_cast<usize>(-1);
    if (width > 1 && !access_proven) {
        // ORR of first and last address, then one shift, tests BOTH ends
        // against the reservation in a single branch and needs no carry check:
        // an EA within `width` of 2^64 wraps `last` down, but its own high bits
        // are set, so the OR still rejects it. Plain ADD/ORR/LSR also leave
        // NZCV alone, which is what lets a store between an x86 flag producer
        // and its consumer avoid an MRS/MSR spill of the guest flags.
        emit(arm64::add_x_imm(last_arm, ea, static_cast<u32>(width - 1)));
        emit(arm64::orr_x(8, ea, last_arm));
        emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
    } else {
        if (!access_proven) {
            emit(arm64::lsr_x_imm(8, last_arm, m_cfg.linear64_address_bits));
        }
    }
    usize outside_pos = static_cast<usize>(-1);
    if (!access_proven) {
        outside_pos = m_code_buf.size();
        emit(0);
    }

    usize protection_generation_pos = static_cast<usize>(-1);
    usize permission_below_pos = static_cast<usize>(-1);
    usize permission_above_pos = static_cast<usize>(-1);
    usize mixed_page_pos = static_cast<usize>(-1);
    usize host_page_cross_pos = static_cast<usize>(-1);
    // A load only has to ask about protection, so it tests the byte only when
    // hardware enforcement is what states that protection.
    if (m_cfg.linear64_hw_protect && !access_proven) {
        if (width > 1 && !g_env_no_single_page_load) {
            // Scalar widths cannot skip a 16 KiB host page. Route the rare
            // crossing to the checked helper, then the hot path needs only
            // the last (and therefore sole) page's mixed-protection byte.
            emit(arm64::eor_x(8, ea, last_arm));
            // TBNZ's imm14 reach is too short for widened blocks with many
            // cold stubs. Extract the crossing bit without touching NZCV,
            // then use CBNZ's imm19 displacement to reach the helper.
            emit(arm64::ubfx_x(8, 8,
                               static_cast<u8>(kLinearHostPageShift), 1));
            host_page_cross_pos = m_code_buf.size();
            emit(0);  // CBNZ X8,cold
            const u8 index_width = static_cast<u8>(
                m_cfg.linear64_address_bits - kLinearHostPageShift);
            emit(arm64::ubfx_x(4, last_arm,
                               static_cast<u8>(kLinearHostPageShift),
                               index_width));
            emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(3), 4));
            mixed_page_pos = m_code_buf.size();
            emit(0);
        } else {
            emit_linear64_check_page_test(ea, last_arm, width, mixed_page_pos);
        }
    }
    emit_linear64_permission_guard(false, last_arm, protection_generation_pos,
                                   permission_below_pos, permission_above_pos);

    emit(arm64::add_x(17, REG_LINEAR_BASE, ea));
    emit_x64_tso_load(dst_arm_reg, 17, 8, width);

    if (access_proven) {
        m_esp_frame_live = false;
        return;
    }

    ColdMemStub stub;
    stub.above_branch_pos = access_proven
                                ? mixed_page_pos
                                : outside_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = helper_off;
    stub.width = width;
    stub.dst_arm = dst_arm_reg;
    stub.single_upper_branch = true;
    stub.range_proven = access_proven;
    stub.overflow_branch_pos = overflow_pos;
    stub.permission_below_branch_pos = permission_below_pos;
    stub.permission_above_branch_pos = permission_above_pos;
    stub.protection_generation_branch_pos = protection_generation_pos;
    stub.mixed_page_branch_pos = mixed_page_pos;
    stub.host_page_cross_branch_pos = host_page_cross_pos;
    stub.addr_arm = ea == 9 ? u8{9} : addr_arm_reg;
    stub.spill_nzcv = m_flags.stub_spill_nzcv;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}
void JitCompiler::emit_linear64_store(u8 src_arm_reg, u8 addr_arm_reg,
                                      int width) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(true);

    const u32 helper_off = (width == 1) ? JIT_CTX_OFF_WRITE8
                         : (width == 2) ? JIT_CTX_OFF_WRITE16
                         : (width == 4) ? JIT_CTX_OFF_WRITE32
                                        : JIT_CTX_OFF_WRITE64;

    // Intel SDM Vol. 2 MOV DEST := SRC: the store writes the computed EA.
    if (m_cfg.linear64_hw_protect) {
        emit_linear64_store_checked(src_arm_reg, addr_arm_reg, width,
                                    helper_off);
        return;
    }

    // Preserve both operands across the guards and into the cold helper.
    // This is a parallel copy to X10:value/X9:address.  The common case needs
    // two moves; only the X9 <- X10 / X10 <- X9 cycle needs X17.
    u8 store_arm = 10;
    u8 last_arm = 17;
    if (addr_arm_reg != 10) {
        if (src_arm_reg != 10) emit(arm64::mov_x(10, src_arm_reg));
        if (addr_arm_reg != 9) emit(arm64::mov_x(9, addr_arm_reg));
    } else if (src_arm_reg != 9) {
        emit(arm64::mov_x(9, addr_arm_reg));
        if (src_arm_reg != 10) emit(arm64::mov_x(10, src_arm_reg));
    } else {
        // Preserve the value in X17 and reuse the old address register X10
        // for the last-byte bound.  The cold stub marshals from store_arm.
        emit(arm64::mov_x(17, src_arm_reg));
        emit(arm64::mov_x(9, addr_arm_reg));
        store_arm = 17;
        last_arm = (width > 1) ? 10 : 9;
    }

    // Validate both ends against the reservation in one branch. ORR of the
    // first and last address subsumes the u64 wrap the old carry check caught
    // (an EA within `width` of 2^64 has its own high bits set), and ADD/ORR/LSR
    // leave NZCV alone, so a store between an x86 flag producer and its
    // consumer no longer forces an MRS/MSR spill of the guest flags.
    const usize overflow_pos = static_cast<usize>(-1);
    if (width > 1) {
        emit(arm64::add_x_imm(last_arm, 9, static_cast<u32>(width - 1)));
        emit(arm64::orr_x(8, 9, last_arm));
        emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
    } else {
        if (last_arm != 9) emit(arm64::mov_x(last_arm, 9));
        emit(arm64::lsr_x_imm(8, last_arm, m_cfg.linear64_address_bits));
    }
    const usize outside_pos = m_code_buf.size();
    emit(0);

    usize protection_generation_pos = static_cast<usize>(-1);
    usize permission_below_pos = static_cast<usize>(-1);
    usize permission_above_pos = static_cast<usize>(-1);
    usize mixed_page_pos = static_cast<usize>(-1);
    // Always, under every protection model: this one byte is what tells a
    // direct store that the host page holds translated code, and it replaced
    // the two acquire-loaded code-span compares this path used to pay per
    // store. Under hardware protection the same byte also answers permission.
    emit_linear64_check_page_test(9, last_arm, width, mixed_page_pos);
    emit_linear64_permission_guard(true, last_arm, protection_generation_pos,
                                   permission_below_pos, permission_above_pos);

    // The write-watch probe that used to live here is gone. It loaded a
    // PROCESS-GLOBAL slot, so one live MEM_WRITE_WATCH allocation anywhere sent
    // every store in the process through the helper -- and a guest GC holds
    // such an allocation for long stretches. kLinearCheckWatched states the
    // same obligation per host page, in the byte the store already loaded, and
    // is the correct precision: GetWriteWatch only reports pages inside the
    // watched allocation.
    const usize watch_pos = static_cast<usize>(-1);

    // The code-span compare that used to live here is gone. It re-derived, per
    // store and through two ACQUIRE loads, a fact that changes only when a new
    // code page is first translated -- and it was coarse, rejecting the whole
    // [lo,hi) envelope. kLinearCheckHoldsCode states the same fact per host
    // page, is published by retain_jit_code_page before the translation is
    // reachable, and is read by the byte test above at no additional cost.
    const usize smc_pos = static_cast<usize>(-1);
    emit(arm64::add_x(8, REG_LINEAR_BASE, 9));
    emit_x64_tso_store(store_arm, 8, 4, width);

    ColdMemStub stub;
    stub.above_branch_pos = outside_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = helper_off;
    stub.width = width;
    stub.store_arm = store_arm;
    stub.is_store = true;
    stub.single_upper_branch = true;
    stub.overflow_branch_pos = overflow_pos;
    stub.permission_below_branch_pos = permission_below_pos;
    stub.permission_above_branch_pos = permission_above_pos;
    stub.protection_generation_branch_pos = protection_generation_pos;
    stub.mixed_page_branch_pos = mixed_page_pos;
    stub.spill_nzcv = m_flags.stub_spill_nzcv;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);
    stub.watch_branch_pos = watch_pos;
    stub.smc_branch_pos = smc_pos;
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}
void JitCompiler::emit_x64_region_load(u8 dst_arm_reg, u8 addr_arm_reg,
                                       int width) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(false);
    m_flags.load_clobbered_nzcv = true;  // bounds check is a SUBS

    const u32 helper_off = (width == 1) ? JIT_CTX_OFF_READ8
                         : (width == 4) ? JIT_CTX_OFF_READ32
                                        : JIT_CTX_OFF_READ64;
    // The last byte touched must stay inside the cached region: comparing the
    // first byte only would let a 4/8-byte access run past `end` into an
    // unrelated mapping. X4/X8 already hold slot.base/slot.end from the
    // paired slot-header load below.
    const auto emit_upper_bound_check = [&] {
        if (width > 1) {
            emit(arm64::add_x_imm(10, 9, static_cast<u32>(width - 1)));
            emit(arm64::subs_x(31, 10, 8));
        } else {
            emit(arm64::subs_x(31, 9, 8));
        }
    };
    const auto emit_inline_load = [&] {
        emit(arm64::add_x(17, 3, 4));
        emit_x64_tso_load(dst_arm_reg, 17, 8, width);
    };

    // X3 = &slots[(addr >> granule) & (count-1)], then bounds-check the entry.
    emit(arm64::mov_x(9, addr_arm_reg));
    emit(arm64::ubfx_x(4, 9, static_cast<u8>(X64_SLOT_GRANULE_SHIFT),
                       static_cast<u8>(X64_SLOT_INDEX_BITS)));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_X64_RSLOTS >> 3));
    emit(arm64::add_x_lsl(3, 3, 4, 5));
    emit(arm64::ldp_x_off(4, 8, 3, 0));     // X4/X8 = slot.base/slot.end
    // Keep addr-base in X4 while setting the same carry flags used by the
    // lower-bound branch. The old path discarded this result into XZR and
    // recomputed the identical subtraction after the upper-bound check.
    emit(arm64::subs_x(4, 9, 4));
    const usize below_cache_pos = m_code_buf.size();
    emit(0);
    emit_upper_bound_check();
    const usize above_cache_pos = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(3, 3, 2));       // X3 = slot.host
    emit_inline_load();

    // The fast path FALLS THROUGH to the merge point from here; the helper
    // marshal, BLR and raise check are emitted in the block tail by
    // emit_flush_cold_mem_stubs() and branch back to this index. That deletes
    // the `B .done` the fast path used to pay on every hit, and keeps the
    // never-executed slow path out of the hot instruction stream.
    ColdMemStub stub;
    stub.below_branch_pos = below_cache_pos;
    stub.above_branch_pos = above_cache_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = helper_off;
    stub.width = width;
    stub.dst_arm = dst_arm_reg;
    stub.is_store = false;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);
    push_cold_mem_stub(stub);
    // The tail's BLR still invalidates any cached ESP frame, exactly as the
    // inline emit_helper_blr() used to do at this point.
    m_esp_frame_live = false;
}
void JitCompiler::emit_x64_region_store(u8 src_arm_reg, u8 addr_arm_reg,
                                        int width) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(false);

    const u32 helper_off = (width == 1) ? JIT_CTX_OFF_WRITE8
                         : (width == 2) ? JIT_CTX_OFF_WRITE16
                         : (width == 4) ? JIT_CTX_OFF_WRITE32
                                        : JIT_CTX_OFF_WRITE64;
    // Marshal the address/value pair without paying for a temporary on the
    // common path. Only X9 -> X10 with the address already in X10 is a true
    // register swap; every other pairing can be ordered in two moves.
    if (src_arm_reg == 9 && addr_arm_reg == 10) {
        emit(arm64::mov_x(17, src_arm_reg));
        emit(arm64::mov_x(9, addr_arm_reg));
        emit(arm64::mov_x(10, 17));
    } else if (addr_arm_reg == 10) {
        emit(arm64::mov_x(9, addr_arm_reg));
        emit(arm64::mov_x(10, src_arm_reg));
    } else {
        emit(arm64::mov_x(10, src_arm_reg));
        emit(arm64::mov_x(9, addr_arm_reg));
    }

    // The value is safe in X10 from here, so X17 doubles as the index and
    // upper-bound temporary. X3 = &slots[index], X4 = slot.base.
    emit(arm64::ubfx_x(17, 9, static_cast<u8>(X64_SLOT_GRANULE_SHIFT),
                       static_cast<u8>(X64_SLOT_INDEX_BITS)));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_X64_WSLOTS >> 3));
    emit(arm64::add_x_lsl(3, 3, 17, 5));
    emit(arm64::ldp_x_off(4, 8, 3, 0));     // X4/X8 = slot.base/slot.end
    emit(arm64::subs_x(4, 9, 4));           // X4 = addr - base, set bounds flags
    const usize below_pos = m_code_buf.size();
    emit(0);
    if (width > 1) {
        emit(arm64::add_x_imm(17, 9, static_cast<u32>(width - 1)));
        emit(arm64::subs_x(31, 17, 8));
    } else {
        emit(arm64::subs_x(31, 9, 8));
    }
    const usize above_pos = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(3, 3, 2));   // X3 = slot.host
    emit(arm64::add_x(8, 3, 4));
    emit_x64_tso_store(10, 8, 17, width);

    // Fast path falls through; slow path goes to the tail. The store value is
    // live in X10 across the probe and stays live into the cold block, which
    // is entered directly from the probe branch with registers intact.
    ColdMemStub stub;
    stub.below_branch_pos = below_pos;
    stub.above_branch_pos = above_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = helper_off;
    stub.width = width;
    stub.dst_arm = 0;
    stub.is_store = true;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}

void JitCompiler::emit_guest_store32_64(u8 src_arm_reg, u8 addr_arm_reg) {
    publish_pending_stub_store();
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    const bool hold = should_hold_nzcv();
    const bool nzcv_clean = linear64_store_is_nzcv_clean();
    if (hold && !nzcv_clean) emit_nzcv_spill();
    (void)emit_store_ring_helper(src_arm_reg, addr_arm_reg, 4);
    if (m_cfg.linear64 && m_linear_base_pinned) {
        m_flags.stub_spill_nzcv = hold && nzcv_clean;
        emit_linear64_store(src_arm_reg, addr_arm_reg, 4);
        m_flags.stub_spill_nzcv = false;
    } else {
        emit_x64_region_store(src_arm_reg, addr_arm_reg, 4);
    }
    if (hold && !nzcv_clean) emit_nzcv_fill();
}

}  // namespace vitreo::cpu::jit

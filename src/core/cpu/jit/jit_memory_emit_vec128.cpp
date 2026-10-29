// 128-bit guest accesses of the Tier-0 memory lowering: the checked
// linear64 form, the linear64 load/store and the x64 region-cache
// load/store.
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

// The hardware-protected 128-bit access, guarded inline in the scalar shape
// (emit_linear64_load_checked): one CBNZ for "outside the reservation or
// crossing a 16 KiB host page", then the single host page's check byte, then
// the access. Ten words and no taken branch, where the shared out-of-line
// guard it replaced executed ~17 words across a BLR and a RET (that opt-in
// was removed 2026-10-11). A mixed page takes the stub's sub-page entry, which still performs the
// direct access when every guest page it touches allows it. The EA stays
// where the lowering formed it unless that is a guard, stub or NZCV-spill
// scratch (X3/X8/X11/X16/X17); the stub moves it to the helper ABI's X9.
void JitCompiler::emit_linear64_vec128_checked(u8 vec_reg, bool is_store,
                                               u8 addr_arm_reg, bool aligned16) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    u8 ea = addr_arm_reg;
    if (ea == 3 || ea == 8 || ea == 11 || ea == 16 || ea == 17) {
        // MOV Xd,Xn -- never MOV Wd,Wn (jit64_ea_truncation_bug_family).
        emit(arm64::mov_x(9, ea));
        ea = 9;
    }
    const u8 bits = m_cfg.linear64_address_bits;
    const u8 index_width = static_cast<u8>(bits - kLinearHostPageShift);
    ColdMemStub stub;
    if (linear64_lean_site_eligible()) {
        // JIT-view lean site (ColdMemStub::lean_variant), vector form: one
        // NZCV-free CBNZ for "outside the window or crossing a host page",
        // then the access straight off the view. The crossing test stays
        // inline, unlike the scalar form: a plain LDR/STR Q has no alignment
        // fault, and a store that crosses into a denied page could write its
        // first part before faulting. One page per access means a refused
        // access performed none of itself.
        if (is_store) emit_vector_tso_fence();
        emit_linear64_lean_vector_guard(stub, ea, 16, aligned16);
        if (is_store) {
            emit(arm64::str_q_reg(vec_reg, REG_LINEAR64_CHECK_MAP, ea));
        } else {
            emit(arm64::ldr_q_reg(vec_reg, REG_LINEAR64_CHECK_MAP, ea));
            emit_vector_tso_fence();
        }
    } else if (m_cfg.linear64_range_by_fault) {
        // A 16-byte-aligned EA cannot cross a host page; any other one tests
        // the crossing alone (the reservation is the map load's business).
        if (!aligned16) {
            // UBFX + CBNZ, not TBNZ #14: the stub of a large block can sit
            // beyond TBNZ's +/-32 KiB (a browser child aborted on exactly that,
            // see WidenedLoadsUseLongRangeCrossingBranches); CBNZ reaches
            // +/-1 MiB.
            emit(arm64::add_x_imm(17, ea, 15));
            emit(arm64::eor_x(8, ea, 17));
            emit(arm64::ubfx_x(8, 8, static_cast<u8>(kLinearHostPageShift), 1));
            stub.host_page_cross_branch_pos = m_code_buf.size();
            emit(0);  // CBNZ X8, .stub
        }
        emit_linear64_first_byte_index(ea, stub);
    } else {
        if (aligned16) {
            // A 16-byte-aligned EA below the reservation's end keeps all 16
            // bytes inside it and inside one host page.
            emit(arm64::lsr_x_imm(8, ea, bits));
        } else {
            emit(arm64::add_x_imm(17, ea, 15));
            emit(arm64::eor_x(8, ea, 17));
            emit(arm64::ubfx_x(8, 8, static_cast<u8>(kLinearHostPageShift), 1));
            emit(arm64::orr_x_lsr(8, 8, ea, bits));
        }
        stub.above_branch_pos = m_code_buf.size();
        emit(0);  // CBNZ X8, .stub
        emit(arm64::ubfx_x(3, ea, static_cast<u8>(kLinearHostPageShift),
                           index_width));
    }
    if (!stub.lean_variant) {
        emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(16), 3));
        const usize mixed_page_pos = m_code_buf.size();
        emit(0);  // TBNZ W3, #MixedRead (load) / CBNZ X3 (store), .stub
        if (is_store) {
            emit_vector_tso_fence();
            emit(arm64::str_q_reg(vec_reg, REG_LINEAR_BASE, ea));
        } else {
            emit(arm64::ldr_q_reg(vec_reg, REG_LINEAR_BASE, ea));
            emit_vector_tso_fence();
        }
        stub.mixed_page_branch_pos = mixed_page_pos;
    }
    if (stub.range_variant) {
        stub.above_branch_pos = stub.mixed_page_branch_pos;  // names the access
        stub.range_proven = true;
    }
    stub.return_pos = m_code_buf.size();
    stub.helper_off = is_store ? JIT_CTX_OFF_WRITE64 : JIT_CTX_OFF_READ64;
    stub.width = 16;
    stub.dst_arm = vec_reg;
    stub.is_store = is_store;
    stub.is_vec128 = true;
    stub.single_upper_branch = true;
    stub.addr_arm = ea;
    stub.spill_nzcv = m_flags.stub_spill_nzcv;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}

// 128-bit guest-base load. Same contract as emit_linear64_load, widened to the
// 16 bytes an SSE/AVX lane touches: validate the LAST byte (addr+15) so an
// access straddling the top of the reservation cannot reach host memory, then
// one LDR Q off the pinned base. An out-of-window or wrapping address takes the
// shared vec128 cold stub, which splits into two checked 8-byte helper calls.
void JitCompiler::emit_linear64_load128(u8 vec_reg, u8 addr_arm_reg,
                                        bool aligned16) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(true);

    if (m_cfg.linear64_hw_protect) {
        emit_linear64_vec128_checked(vec_reg, false, addr_arm_reg, aligned16);
        return;
    }
    // MOV Xd,Xn -- never MOV Wd,Wn. Narrowing the EA here is the recorded
    // jit64_ea_truncation_bug_family shape, silent below 4 GiB.
    emit(arm64::mov_x(9, addr_arm_reg));
    // Software-protection model only from here: hardware protection always
    // takes emit_linear64_vec128_checked above.
    // See emit_linear64_load: ORR of both ends replaces the carry check and
    // leaves NZCV untouched.
    const usize overflow_pos = static_cast<usize>(-1);
    emit(arm64::add_x_imm(17, 9, 15));
    emit(arm64::orr_x(8, 9, 17));
    emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
    const usize outside_pos = m_code_buf.size();
    emit(0);

    usize protection_generation_pos = static_cast<usize>(-1);
    usize permission_below_pos = static_cast<usize>(-1);
    usize permission_above_pos = static_cast<usize>(-1);
    usize mixed_page_pos = static_cast<usize>(-1);
    emit_linear64_permission_guard(false, 17, protection_generation_pos,
                                   permission_below_pos, permission_above_pos);

    emit(arm64::ldr_q_reg(vec_reg, REG_LINEAR_BASE, 9));
    emit_vector_tso_fence();

    ColdMemStub stub;
    stub.above_branch_pos = outside_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = JIT_CTX_OFF_READ64;
    stub.width = 16;
    stub.dst_arm = vec_reg;
    stub.is_store = false;
    stub.is_vec128 = true;
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
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}
// 128-bit guest-base store. Carries the same write-watch and SMC obligations as
// emit_linear64_store: an active MEM_WRITE_WATCH allocation or an address
// intersecting the compiled-code envelope takes the checked helper, which is
// the only path that records the full 64-bit guest address.
void JitCompiler::emit_linear64_store128(u8 vec_reg, u8 addr_arm_reg,
                                         bool aligned16) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(true);

    // The value stays in vec_reg -- nothing below touches the vector file --
    // so only the address needs staging into X9 for the cold stub.
    if (m_cfg.linear64_hw_protect) {
        emit_linear64_vec128_checked(vec_reg, true, addr_arm_reg, aligned16);
        return;
    }
    emit(arm64::mov_x(9, addr_arm_reg));
    // See emit_linear64_load: ORR of both ends replaces the carry check and
    // leaves NZCV untouched.
    const usize overflow_pos = static_cast<usize>(-1);
    emit(arm64::add_x_imm(17, 9, 15));
    emit(arm64::orr_x(8, 9, 17));
    emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
    const usize outside_pos = m_code_buf.size();
    emit(0);

    usize protection_generation_pos = static_cast<usize>(-1);
    usize permission_below_pos = static_cast<usize>(-1);
    usize permission_above_pos = static_cast<usize>(-1);
    usize mixed_page_pos = static_cast<usize>(-1);
    // kLinearCheckHoldsCode carries the SMC obligation the two acquire-loaded
    // code-span compares used to carry here; see emit_linear64_store.
    emit_linear64_check_page_test(9, 17, 16, mixed_page_pos);
    emit_linear64_permission_guard(true, 17, protection_generation_pos,
                                   permission_below_pos, permission_above_pos);

    // See emit_linear64_store: the global write-watch probe is now
    // kLinearCheckWatched in the per-host-page check byte.
    const usize watch_pos = static_cast<usize>(-1);

    const usize smc_pos = static_cast<usize>(-1);
    emit_vector_tso_fence();
    emit(arm64::str_q_reg(vec_reg, REG_LINEAR_BASE, 9));

    ColdMemStub stub;
    stub.above_branch_pos = outside_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = JIT_CTX_OFF_WRITE64;
    stub.width = 16;
    stub.dst_arm = vec_reg;
    stub.is_store = true;
    stub.is_vec128 = true;
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

// 128-bit validated guest load. One region probe, one LDR Q. See the header
// for the contract; the shape deliberately mirrors emit_x64_region_load so the
// two stay reviewable side by side.
void JitCompiler::emit_x64_region_load128(u8 vec_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(false);

    // X3 = &slots[(addr >> granule) & (count-1)], then bounds-check the entry.
    // MOV Xd,Xn -- never MOV Wd,Wn: narrowing here is the recorded
    // jit64_ea_truncation_bug_family shape, silent below 4 GiB.
    emit(arm64::mov_x(9, addr_arm_reg));
    emit(arm64::ubfx_x(4, 9, static_cast<u8>(X64_SLOT_GRANULE_SHIFT),
                       static_cast<u8>(X64_SLOT_INDEX_BITS)));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_X64_RSLOTS >> 3));
    emit(arm64::add_x_lsl(3, 3, 4, 5));
    emit(arm64::ldr_x_uoff(4, 3, 0));       // X4 = slot.base
    emit(arm64::subs_x(31, 9, 4));
    const usize below_pos = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(8, 3, 1));       // X8 = slot.end
    // The LAST byte touched is addr+15. Comparing the first byte only would
    // let a 16-byte access run past `end` into an unrelated mapping.
    emit(arm64::add_x_imm(10, 9, 15));
    emit(arm64::subs_x(31, 10, 8));
    const usize above_pos = m_code_buf.size();
    emit(0);
    emit(arm64::sub_x(4, 9, 4));            // X4 = addr - base
    emit(arm64::ldr_x_uoff(3, 3, 2));       // X3 = slot.host
    emit(arm64::ldr_q_reg(vec_reg, 3, 4));  // the whole 16 bytes, once
    emit_vector_tso_fence();

    ColdMemStub stub;
    stub.below_branch_pos = below_pos;
    stub.above_branch_pos = above_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = JIT_CTX_OFF_READ64;
    stub.width = 16;
    stub.dst_arm = vec_reg;
    stub.is_store = false;
    stub.is_vec128 = true;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}
void JitCompiler::emit_x64_region_store128(u8 vec_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(false);

    emit(arm64::mov_x(9, addr_arm_reg));
    emit(arm64::ubfx_x(17, 9, static_cast<u8>(X64_SLOT_GRANULE_SHIFT),
                       static_cast<u8>(X64_SLOT_INDEX_BITS)));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_X64_WSLOTS >> 3));
    emit(arm64::add_x_lsl(3, 3, 17, 5));
    emit(arm64::ldr_x_uoff(4, 3, 0));
    emit(arm64::subs_x(31, 9, 4));
    const usize below_pos = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(8, 3, 1));
    emit(arm64::add_x_imm(17, 9, 15));
    emit(arm64::subs_x(31, 17, 8));
    const usize above_pos = m_code_buf.size();
    emit(0);
    emit(arm64::sub_x(4, 9, 4));
    emit(arm64::ldr_x_uoff(3, 3, 2));
    emit_vector_tso_fence();
    emit(arm64::str_q_reg(vec_reg, 3, 4));

    // Unlike the GPR store, the value is NOT parked in X10: it stays in
    // vec_reg, which no part of the probe touches. The cold stub therefore
    // reads it straight out of vec_reg.
    ColdMemStub stub;
    stub.below_branch_pos = below_pos;
    stub.above_branch_pos = above_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = JIT_CTX_OFF_WRITE64;
    stub.width = 16;
    stub.dst_arm = vec_reg;
    stub.is_store = true;
    stub.is_vec128 = true;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}

}  // namespace vitreo::cpu::jit

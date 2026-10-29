// 256-bit guest accesses of the Tier-0 memory lowering: the linear64
// load and store.
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

// 256-bit guest-base load: the 128-bit shape with a 32-byte guard and an LDP.
void JitCompiler::emit_linear64_load256(u8 vec_lo, u8 vec_hi, u8 addr_arm_reg,
                                        u32 slot_lo, u32 slot_hi) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(true);
    emit(arm64::mov_x(9, addr_arm_reg));
    if (m_cfg.linear64_hw_protect && linear64_lean_site_eligible()) {
        // JIT-view lean site, 256-bit form: the vector guard, then LDP Q off
        // the view (see emit_linear64_vec128_checked).
        ColdMemStub stub;
        emit_linear64_lean_vector_guard(stub, 9, 32, /*aligned=*/false);
        emit(arm64::add_x(17, REG_LINEAR64_CHECK_MAP, 9));
        emit(arm64::ldp_q_off(vec_lo, vec_hi, 17, 0));
        emit_vector_tso_fence();
        stub.return_pos = m_code_buf.size();
        stub.helper_off = JIT_CTX_OFF_READ64;
        stub.width = 32;
        stub.dst_arm = vec_lo;
        stub.dst_arm2 = vec_hi;
        stub.vec_slot_lo = slot_lo;
        stub.vec_slot_hi = slot_hi;
        stub.is_store = false;
        stub.is_vec256 = true;
        stub.single_upper_branch = true;
        stub.addr_arm = 9;
        stub.spill_nzcv = m_flags.stub_spill_nzcv;
        stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
        stub.nzcv_kind_on_raise = nzcv_kind_byte(
            m_flags.nzcv != NzcvState::None ? m_flags.nzcv
                                            : m_flags.incoming_nzcv);
        push_cold_mem_stub(stub);
        m_esp_frame_live = false;
        return;
    }
    if (m_cfg.linear64_hw_protect &&
        materialize_linear64_hot_helper(false, 32) != nullptr) {
        const u32 slot = linear64_hot_wide_slot(false, 32);
        emit(arm64::ldr_x_uoff(
            16, REG_JIT_CTX, (JIT_CTX_OFF_LINEAR64_HOT_MEM_WIDE >> 3) + slot));
        emit(0xD63F0200u);
        const usize fail_pos = m_code_buf.size();
        emit(0);
        emit(arm64::add_x(17, REG_LINEAR_BASE, 9));
        emit(arm64::ldp_q_off(vec_lo, vec_hi, 17, 0));
        emit_vector_tso_fence();
        ColdMemStub stub;
        stub.above_branch_pos = fail_pos;
        stub.return_pos = m_code_buf.size();
        stub.helper_off = JIT_CTX_OFF_READ64;
        stub.width = 32;
        stub.dst_arm = vec_lo;
        stub.dst_arm2 = vec_hi;
        stub.vec_slot_lo = slot_lo;
        stub.vec_slot_hi = slot_hi;
        stub.is_store = false;
        stub.is_vec256 = true;
        stub.single_upper_branch = true;
        stub.hot_helper_fail = true;
        stub.addr_arm = 9;
        stub.spill_nzcv = m_flags.stub_spill_nzcv;
        stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
        stub.nzcv_kind_on_raise = nzcv_kind_byte(
            m_flags.nzcv != NzcvState::None ? m_flags.nzcv
                                            : m_flags.incoming_nzcv);
        push_cold_mem_stub(stub);
        m_esp_frame_live = false;
        return;
    }
    const usize overflow_pos = static_cast<usize>(-1);
    emit(arm64::add_x_imm(17, 9, 31));
    emit(arm64::orr_x(8, 9, 17));
    emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
    const usize outside_pos = m_code_buf.size();
    emit(0);
    usize protection_generation_pos = static_cast<usize>(-1);
    usize permission_below_pos = static_cast<usize>(-1);
    usize permission_above_pos = static_cast<usize>(-1);
    usize mixed_page_pos = static_cast<usize>(-1);
    if (m_cfg.linear64_hw_protect) {
        emit_linear64_check_page_test(9, 17, 32, mixed_page_pos);
    }
    emit_linear64_permission_guard(false, 17, protection_generation_pos,
                                   permission_below_pos, permission_above_pos);
    emit(arm64::add_x(17, REG_LINEAR_BASE, 9));
    emit(arm64::ldp_q_off(vec_lo, vec_hi, 17, 0));
    emit_vector_tso_fence();
    ColdMemStub stub;
    stub.above_branch_pos = outside_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = JIT_CTX_OFF_READ64;
    stub.width = 32;
    stub.dst_arm = vec_lo;
    stub.dst_arm2 = vec_hi;
    stub.vec_slot_lo = slot_lo;
    stub.vec_slot_hi = slot_hi;
    stub.is_store = false;
    stub.is_vec256 = true;
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

// 256-bit guest-base store: the 128-bit shape with a 32-byte guard and an STP.
// Same write-watch and SMC obligations, carried by the check byte.
void JitCompiler::emit_linear64_store256(u8 vec_lo, u8 vec_hi, u8 addr_arm_reg,
                                         u32 slot_lo, u32 slot_hi) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    diagnostics::perf_add_linear_mem_emit(true);
    emit(arm64::mov_x(9, addr_arm_reg));
    if (m_cfg.linear64_hw_protect && linear64_lean_site_eligible()) {
        // JIT-view lean site, 256-bit form: fence, the vector guard, then
        // STP Q off the view (see emit_linear64_vec128_checked).
        ColdMemStub stub;
        emit_vector_tso_fence();
        emit_linear64_lean_vector_guard(stub, 9, 32, /*aligned=*/false);
        emit(arm64::add_x(17, REG_LINEAR64_CHECK_MAP, 9));
        emit(arm64::stp_q_off(vec_lo, vec_hi, 17, 0));
        stub.return_pos = m_code_buf.size();
        stub.helper_off = JIT_CTX_OFF_WRITE64;
        stub.width = 32;
        stub.dst_arm = vec_lo;
        stub.dst_arm2 = vec_hi;
        stub.vec_slot_lo = slot_lo;
        stub.vec_slot_hi = slot_hi;
        stub.is_store = true;
        stub.is_vec256 = true;
        stub.single_upper_branch = true;
        stub.addr_arm = 9;
        stub.spill_nzcv = m_flags.stub_spill_nzcv;
        stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
        stub.nzcv_kind_on_raise = nzcv_kind_byte(
            m_flags.nzcv != NzcvState::None ? m_flags.nzcv
                                            : m_flags.incoming_nzcv);
        push_cold_mem_stub(stub);
        m_esp_frame_live = false;
        return;
    }
    if (m_cfg.linear64_hw_protect &&
        materialize_linear64_hot_helper(true, 32) != nullptr) {
        const u32 slot = linear64_hot_wide_slot(true, 32);
        emit(arm64::ldr_x_uoff(
            16, REG_JIT_CTX, (JIT_CTX_OFF_LINEAR64_HOT_MEM_WIDE >> 3) + slot));
        emit(0xD63F0200u);
        const usize fail_pos = m_code_buf.size();
        emit(0);
        emit_vector_tso_fence();
        emit(arm64::add_x(17, REG_LINEAR_BASE, 9));
        emit(arm64::stp_q_off(vec_lo, vec_hi, 17, 0));
        ColdMemStub stub;
        stub.above_branch_pos = fail_pos;
        stub.return_pos = m_code_buf.size();
        stub.helper_off = JIT_CTX_OFF_WRITE64;
        stub.width = 32;
        stub.dst_arm = vec_lo;
        stub.dst_arm2 = vec_hi;
        stub.vec_slot_lo = slot_lo;
        stub.vec_slot_hi = slot_hi;
        stub.is_store = true;
        stub.is_vec256 = true;
        stub.single_upper_branch = true;
        stub.hot_helper_fail = true;
        stub.addr_arm = 9;
        stub.spill_nzcv = m_flags.stub_spill_nzcv;
        stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
        stub.nzcv_kind_on_raise = nzcv_kind_byte(
            m_flags.nzcv != NzcvState::None ? m_flags.nzcv
                                            : m_flags.incoming_nzcv);
        push_cold_mem_stub(stub);
        m_esp_frame_live = false;
        return;
    }
    const usize overflow_pos = static_cast<usize>(-1);
    emit(arm64::add_x_imm(17, 9, 31));
    emit(arm64::orr_x(8, 9, 17));
    emit(arm64::lsr_x_imm(8, 8, m_cfg.linear64_address_bits));
    const usize outside_pos = m_code_buf.size();
    emit(0);
    usize protection_generation_pos = static_cast<usize>(-1);
    usize permission_below_pos = static_cast<usize>(-1);
    usize permission_above_pos = static_cast<usize>(-1);
    usize mixed_page_pos = static_cast<usize>(-1);
    emit_linear64_check_page_test(9, 17, 32, mixed_page_pos);
    emit_linear64_permission_guard(true, 17, protection_generation_pos,
                                   permission_below_pos, permission_above_pos);
    const usize watch_pos = static_cast<usize>(-1);
    const usize smc_pos = static_cast<usize>(-1);
    emit_vector_tso_fence();
    emit(arm64::add_x(17, REG_LINEAR_BASE, 9));
    emit(arm64::stp_q_off(vec_lo, vec_hi, 17, 0));
    ColdMemStub stub;
    stub.above_branch_pos = outside_pos;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = JIT_CTX_OFF_WRITE64;
    stub.width = 32;
    stub.dst_arm = vec_lo;
    stub.dst_arm2 = vec_hi;
    stub.vec_slot_lo = slot_lo;
    stub.vec_slot_hi = slot_hi;
    stub.is_store = true;
    stub.is_vec256 = true;
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

}  // namespace vitreo::cpu::jit

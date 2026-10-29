// The linear64 permission and guard machinery of the Tier-0 memory
// lowering: the check-page test, the permission guard, the first-byte
// index, the JIT-view lean site and the checked scalar load/store.
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

// Guest permission re-validation on top of the linear64 reservation, shared by
// every linear64 emitter so the policy means one thing everywhere. Entered with
// the full 64-bit EA in X9 and the last touched byte in `last_arm`; clobbers
// X3/X4.
//
// Two shapes, selected by whether GuestMemory hardware-enforces protection
// inside the reservation (see GuestMemory::enable_linear_hardware_protection):
//
//  * HARDWARE (m_cfg.linear64_hw_protect). A 16 KiB host page whose 4 KiB guest
//    pages all agree carries exactly the guest's protection, so the access
//    needs no software check at all -- the page enforces it and linear_fault
//    turns the fault into STATUS_ACCESS_VIOLATION. Only a MIXED host page,
//    where no single host protection is right, still has to be checked, and
//    one byte load off the mixed map says which: four instructions, and only
//    the map's bytes are consulted rather than the whole slot table.
//    The protect-generation check disappears with it: there is no longer a
//    cached permission decision to go stale. mprotect under the region lock IS
//    the shootdown, and the mixed byte is published before the hardware is
//    relaxed and cleared only after it is tightened, so both transitions fail
//    closed.
//
//  * SOFTWARE (the legacy model, and any reservation without hardware
//    enforcement). The reservation carries only committed->RW, so it proves an
//    address is mapped and never that the guest may touch it. Every access
//    re-checks a read/write permission slot AND the protect generation:
//    eleven instructions plus an LDAR, on every load and every store.
void JitCompiler::emit_linear64_check_page_test(u8 first_arm, u8 last_arm,
                                                int width,
                                                usize& check_page_pos) {
    // The map base is pinned in REG_LINEAR64_CHECK_MAP, so this costs a UBFX
    // and a byte load -- no JitContext reload, and one fewer dependent load on
    // the address-computation chain of every access.
    //
    // The EA is already known to be inside the power-of-two reservation (the
    // caller's shift+CBNZ ran first), so the index cannot leave the map.
    const u8 index_width = static_cast<u8>(m_cfg.linear64_address_bits -
                                           kLinearHostPageShift);
    if (width > 1) {
        // The operand may straddle a host-page boundary.  Protection applies
        // to every byte of the x86 operand, so either endpoint being mixed
        // must take the helper. X8 is dead after the reservation bound test.
        emit(arm64::ubfx_x(4, first_arm,
                           static_cast<u8>(kLinearHostPageShift), index_width));
        emit(arm64::ldrb_reg(8, emit_linear64_check_map_base(8), 4));
    }
    emit(arm64::ubfx_x(4, last_arm,
                       static_cast<u8>(kLinearHostPageShift), index_width));
    emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(3), 4));
    if (width > 1) emit(arm64::orr_x(3, 3, 8));
    check_page_pos = m_code_buf.size();
    emit(0);
}
void JitCompiler::emit_linear64_permission_guard(bool is_store, u8 last_arm,
                                                 usize& protection_generation_pos,
                                                 usize& permission_below_pos,
                                                 usize& permission_above_pos) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    // Hardware protection makes the check byte the whole permission answer,
    // and the byte test is emitted by the caller either way (a store needs it
    // for kLinearCheckHoldsCode regardless of the protection model).
    if (m_cfg.linear64_hw_protect) return;
    if (!m_cfg.linear64_perm_guard) return;

    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                           JIT_CTX_OFF_PROTECT_SEQUENCE_LIVE >> 3));
    emit(arm64::ldar_x(3, 3));
    emit(arm64::ldr_x_uoff(4, REG_JIT_CTX,
                           JIT_CTX_OFF_PROTECT_SEQUENCE_SEEN >> 3));
    emit(arm64::subs_x(31, 3, 4));
    protection_generation_pos = m_code_buf.size();
    emit(0);

    emit(arm64::ubfx_x(4, 9, static_cast<u8>(X64_SLOT_GRANULE_SHIFT),
                       static_cast<u8>(X64_SLOT_INDEX_BITS)));
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                           is_store ? (JIT_CTX_OFF_X64_WSLOTS >> 3)
                                    : (JIT_CTX_OFF_X64_RSLOTS >> 3)));
    emit(arm64::add_x_lsl(3, 3, 4, 5));
    emit(arm64::ldr_x_uoff(4, 3, 0));
    emit(arm64::subs_x(31, 9, 4));
    permission_below_pos = m_code_buf.size();
    emit(0);
    emit(arm64::ldr_x_uoff(4, 3, 1));
    emit(arm64::subs_x(31, last_arm, 4));
    permission_above_pos = m_code_buf.size();
    emit(0);
}
// The default linear64 scalar load: hardware-enforced protection, no entry
// proof. One CBNZ rejects both an EA outside the reservation and an access
// that crosses a 16 KiB host page, then the single page's check byte answers
// the remaining question (MixedRead). Exactness is unchanged from the
// separate tests it replaces:
//
//   * the reservation test may look at the FIRST byte only, because the
//     reservation ends on a host-page boundary (2^address_bits), so an access
//     that starts inside and ends outside necessarily crosses a host page and
//     is already rejected by the crossing bit;
//   * with no crossing, first and last byte share one host page, so that
//     page's byte is the whole software-protection answer.
//
// X3, X8 and X17 are the only scratches, so an EA in X4 (emit_compute_ea64's
// default scratch) is consumed in place rather than copied to X9; the stub
// copies it for the helper ABI. A compile-time-constant EA (RIP-relative,
// moffs, disp-only) decides the reservation and crossing tests statically:
// inside and not crossing keeps only the check-byte test, anything else
// branches straight to the checked helper.
// The map index for a guard that tests the access's FIRST byte. Under
// range-by-fault it is `LSR X3, Xea, #14`, unmasked, and no reservation test
// is emitted: the check map ends at the VA limit, so the LDRB that follows
// faults for every EA the reservation does not cover and the fault handler
// sends the site to its range variant (linear_fault.hpp). Otherwise it is the
// reservation test (LSR X8 + CBNZ X8 -> stub) and the masked index.
void JitCompiler::emit_linear64_first_byte_index(u8 ea, ColdMemStub& stub) {
    if (m_cfg.linear64_range_by_fault) {
        emit(arm64::lsr_x_imm(3, ea, static_cast<u8>(kLinearHostPageShift)));
        stub.range_variant = true;
        return;
    }
    const u8 bits = m_cfg.linear64_address_bits;
    emit(arm64::lsr_x_imm(8, ea, bits));
    stub.above_branch_pos = m_code_buf.size();
    emit(0);  // CBNZ X8, .stub
    emit(arm64::ubfx_x(3, ea, static_cast<u8>(kLinearHostPageShift),
                       static_cast<u8>(bits - kLinearHostPageShift)));
}

// A JIT-view lean site (JitConfig::linear64_jit_view, ColdMemStub::
// lean_variant): every scalar access of an x64 block whose ordered form is
// one instruction. The hardware of the enforcing view is then the whole check,
// and a refusal sends the site to its checked variant (linear_fault.cpp).
bool JitCompiler::linear64_lean_site_eligible() const noexcept {
    return m_cfg.linear64_jit_view && m_block_is_64bit &&
           m_cfg.tso_mode != TsoMode::Off && m_cfg.tso_crossing_fault;
}

// The head of a lean site: the window test (or, for a compile-time-constant
// EA proven inside the window, the never-taken CBNZ XZR that names the
// variant) and the JIT-view address in X17. False -- nothing emitted -- for a
// constant EA outside the window or crossing a host page, which the caller
// sends to its helper unconditionally, as before.
//
// Correct for any EA: one inside the window either reaches the page through
// the view or faults on it, one outside never reaches the ADD. The view ends
// on a host-page boundary followed by a guard, so an access that starts
// inside and ends outside faults too (its granule crossing faults first).
bool JitCompiler::emit_linear64_lean_head(ColdMemStub& stub, u8 ea,
                                          bool have_const, u64 const_ea,
                                          int width) {
    const u8 bits = m_cfg.linear64_address_bits;
    if (have_const) {
        const u64 last = const_ea + static_cast<u64>(width - 1);
        const bool inside = (const_ea >> bits) == 0 && last >= const_ea &&
                            (last >> bits) == 0;
        const bool crosses = ((const_ea ^ last) >> kLinearHostPageShift) != 0;
        if (!inside || crosses) return false;
        stub.lean_window_is_locator = true;
    } else {
        emit(arm64::lsr_x_imm(16, ea, bits));
    }
    stub.lean_window_pos = m_code_buf.size();
    emit(0);  // CBNZ X16 | XZR, V -- bound when V is laid out
    stub.lean_variant = true;
    // The window CBNZ belongs to this instruction's host interval, which is
    // what precise-RIP attribution needs; nothing else is bound to it.
    stub.above_branch_pos = stub.lean_window_pos;
    stub.range_proven = true;
    emit(arm64::add_x(17, REG_LINEAR64_CHECK_MAP, ea));  // X15: the JIT view
    return true;
}

// The guard of a vector lean site: `CBNZ X16, V` taken for an EA outside the
// window or an access of @p width bytes crossing a host page (an aligned
// 16-byte access cannot cross one). X16/X17 are scratch; NZCV is untouched.
void JitCompiler::emit_linear64_lean_vector_guard(ColdMemStub& stub, u8 ea,
                                                  int width, bool aligned) {
    const u8 bits = m_cfg.linear64_address_bits;
    if (aligned) {
        emit(arm64::lsr_x_imm(16, ea, bits));
    } else {
        emit(arm64::add_x_imm(17, ea, static_cast<u32>(width - 1)));
        emit(arm64::eor_x(16, ea, 17));
        emit(arm64::ubfx_x(16, 16, static_cast<u8>(kLinearHostPageShift), 1));
        emit(arm64::orr_x_lsr(16, 16, ea, bits));
    }
    stub.lean_window_pos = m_code_buf.size();
    emit(0);  // CBNZ X16, V -- bound when V is laid out
    stub.lean_variant = true;
    stub.above_branch_pos = stub.lean_window_pos;
    stub.range_proven = true;
}

void JitCompiler::emit_linear64_load_checked(u8 dst_arm_reg, u8 addr_arm_reg,
                                             int width, u32 helper_off) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    u64 const_ea = 0;
    const bool have_const = take_const_ea(addr_arm_reg, const_ea);
    const bool lean = linear64_lean_site_eligible();
    const bool ea_is_scratch =
        addr_arm_reg == 3 || addr_arm_reg == 8 || addr_arm_reg == 17 ||
        (lean && addr_arm_reg == 16);
    const u8 ea = ea_is_scratch ? u8{9} : addr_arm_reg;
    if (ea != addr_arm_reg) emit(arm64::mov_x(9, addr_arm_reg));

    const u8 bits = m_cfg.linear64_address_bits;
    const u8 index_width = static_cast<u8>(bits - kLinearHostPageShift);

    // The ordered access's alignment fault stands in for the crossing test
    // (linear_fault.hpp, kTsoCrossingVariantWords). Single bytes never cross.
    const bool crossing_by_fault = width > 1 && m_block_is_64bit &&
                                   m_cfg.tso_mode != TsoMode::Off &&
                                   m_cfg.tso_crossing_fault;

    ColdMemStub stub;
    stub.helper_off = helper_off;
    stub.width = width;
    stub.dst_arm = dst_arm_reg;
    stub.single_upper_branch = true;
    stub.addr_arm = ea;
    stub.spill_nzcv = m_flags.stub_spill_nzcv;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);

    if (lean && emit_linear64_lean_head(stub, ea, have_const, const_ea, width)) {
        emit_acquire_load(dst_arm_reg, 17, width);
        stub.tso_crossing_variant = width > 1;
        stub.return_pos = m_code_buf.size();
        push_cold_mem_stub(stub);
        m_esp_frame_live = false;
        return;
    }

    if (have_const) {
        const u64 last = const_ea + static_cast<u64>(width - 1);
        const bool inside = (const_ea >> bits) == 0 && last >= const_ea &&
                            (last >> bits) == 0;
        const bool crosses = ((const_ea ^ last) >> kLinearHostPageShift) != 0;
        if (!inside || crosses) {
            // Decided at compile time: this access always needs the checked
            // helper (a high-DLL global, a page-straddling constant).
            const usize branch = m_code_buf.size();
            emit(0);  // B .stub
            stub.above_branch_pos = branch;
            stub.smc_branch_pos = branch;
            stub.range_proven = true;
            stub.return_pos = m_code_buf.size();
            push_cold_mem_stub(stub);
            m_esp_frame_live = false;
            return;
        }
        emit_mov_imm32(3, static_cast<u32>(const_ea >> kLinearHostPageShift));
    } else {
        if (width == 1 || crossing_by_fault) {
            // A crossing-by-fault site tests its FIRST byte only: an access
            // whose last byte leaves the page, or the reservation, crosses a
            // 16-byte granule and so never reaches memory from the fast path.
            emit_linear64_first_byte_index(ea, stub);
        } else {
            emit(arm64::add_x_imm(17, ea, static_cast<u32>(width - 1)));
            emit(arm64::eor_x(8, ea, 17));
            emit(arm64::ubfx_x(8, 8, static_cast<u8>(kLinearHostPageShift), 1));
            emit(arm64::orr_x_lsr(8, 8, ea, bits));
            stub.above_branch_pos = m_code_buf.size();
            emit(0);  // CBNZ X8, .stub
            emit(arm64::ubfx_x(3, ea, static_cast<u8>(kLinearHostPageShift),
                               index_width));
        }
    }
    emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(16), 3));
    const usize mixed_page_pos = m_code_buf.size();
    emit(0);  // TBNZ W3, #MixedRead, .stub
    stub.mixed_page_branch_pos = mixed_page_pos;
    if (have_const || stub.range_variant) {
        // No reservation branch exists; the check-byte branch names the
        // access for precise-RIP attribution.
        stub.above_branch_pos = mixed_page_pos;
        stub.range_proven = true;
    }

    emit(arm64::add_x(17, REG_LINEAR_BASE, ea));
    if (crossing_by_fault) {
        // The marker-free ordered site (linear_fault.hpp): exactly
        // [check-byte branch, ADD, ordered load], nothing between them.
        emit_acquire_load(dst_arm_reg, 17, width);
        stub.tso_crossing_variant = true;
    } else {
        emit_x64_tso_load(dst_arm_reg, 17, 8, width);
    }

    stub.return_pos = m_code_buf.size();
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}

// The default linear64 scalar store: the store twin of
// emit_linear64_load_checked. Same combined reservation/crossing CBNZ (exact
// for the same two reasons), then the single host page's whole check byte --
// a store asks every reason (MixedProt, HoldsCode, Watched), not just the
// read bit. A crossing store therefore always takes the helper, which is what
// lets one byte stand for both pages.
//
// Neither operand is copied on the hot path unless it sits in a guard scratch
// (X3, X8, X11, X17) or, for the value, X4 (the non-patching TSO store's
// alignment scratch). The cold stub moves them into the helper ABI's X9/X10
// with a cycle-safe parallel move.
void JitCompiler::emit_linear64_store_checked(u8 src_arm_reg, u8 addr_arm_reg,
                                              int width, u32 helper_off) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    u64 const_ea = 0;
    const bool have_const = take_const_ea(addr_arm_reg, const_ea);
    const bool lean = linear64_lean_site_eligible();
    const auto guard_scratch = [lean](u8 r) {
        return r == 3 || r == 8 || r == 11 || r == 17 || (lean && r == 16);
    };
    u8 ea = addr_arm_reg;
    u8 value = src_arm_reg;
    if (guard_scratch(ea) || guard_scratch(value) || value == 4) {
        // Copy both into the helper ABI registers, cycle-safe.
        if (addr_arm_reg == 10 && src_arm_reg == 9) {
            emit(arm64::mov_x(17, 9));
            emit(arm64::mov_x(9, 10));
            emit(arm64::mov_x(10, 17));
        } else if (src_arm_reg == 9) {
            emit(arm64::mov_x(10, 9));
            if (addr_arm_reg != 9) emit(arm64::mov_x(9, addr_arm_reg));
        } else {
            if (addr_arm_reg != 9) emit(arm64::mov_x(9, addr_arm_reg));
            if (src_arm_reg != 10) emit(arm64::mov_x(10, src_arm_reg));
        }
        ea = 9;
        value = 10;
    }

    const u8 bits = m_cfg.linear64_address_bits;
    const u8 index_width = static_cast<u8>(bits - kLinearHostPageShift);

    const bool crossing_by_fault = width > 1 && m_block_is_64bit &&
                                   m_cfg.tso_mode != TsoMode::Off &&
                                   m_cfg.tso_crossing_fault;

    ColdMemStub stub;
    stub.helper_off = helper_off;
    stub.width = width;
    stub.store_arm = value;
    stub.addr_arm = ea;
    stub.is_store = true;
    stub.single_upper_branch = true;
    stub.spill_nzcv = m_flags.stub_spill_nzcv;
    stub.restore_nzcv_on_raise = m_flags.nzcv_hold_depth != 0;
    stub.nzcv_kind_on_raise = nzcv_kind_byte(
        m_flags.nzcv != NzcvState::None ? m_flags.nzcv : m_flags.incoming_nzcv);

    if (lean && emit_linear64_lean_head(stub, ea, have_const, const_ea, width)) {
        // A release store that faults -- alignment or protection -- has
        // written nothing, so a refused store is never partially performed.
        emit_release_store(value, 17, width);
        stub.tso_crossing_variant = width > 1;
        stub.return_pos = m_code_buf.size();
        push_cold_mem_stub(stub);
        m_esp_frame_live = false;
        return;
    }

    if (have_const) {
        const u64 last = const_ea + static_cast<u64>(width - 1);
        const bool inside = (const_ea >> bits) == 0 && last >= const_ea &&
                            (last >> bits) == 0;
        const bool crosses = ((const_ea ^ last) >> kLinearHostPageShift) != 0;
        if (!inside || crosses) {
            const usize branch = m_code_buf.size();
            emit(0);  // B .stub
            stub.above_branch_pos = branch;
            stub.smc_branch_pos = branch;
            stub.range_proven = true;
            stub.return_pos = m_code_buf.size();
            push_cold_mem_stub(stub);
            m_esp_frame_live = false;
            return;
        }
        emit_mov_imm32(3, static_cast<u32>(const_ea >> kLinearHostPageShift));
    } else {
        if (width == 1 || crossing_by_fault) {
            emit_linear64_first_byte_index(ea, stub);
        } else {
            emit(arm64::add_x_imm(17, ea, static_cast<u32>(width - 1)));
            emit(arm64::eor_x(8, ea, 17));
            emit(arm64::ubfx_x(8, 8, static_cast<u8>(kLinearHostPageShift), 1));
            emit(arm64::orr_x_lsr(8, 8, ea, bits));
            stub.above_branch_pos = m_code_buf.size();
            emit(0);  // CBNZ X8, .stub
            emit(arm64::ubfx_x(3, ea, static_cast<u8>(kLinearHostPageShift),
                               index_width));
        }
    }
    emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(16), 3));
    const usize check_pos = m_code_buf.size();
    emit(0);  // CBNZ X3, .stub (every reason applies to a store)
    stub.mixed_page_branch_pos = check_pos;
    if (have_const || stub.range_variant) {
        stub.above_branch_pos = check_pos;
        stub.range_proven = true;
    }

    emit(arm64::add_x(8, REG_LINEAR_BASE, ea));
    if (crossing_by_fault) {
        // A release store that alignment-faults has not written anything, so
        // a crossing store is never partially performed (x86 performs none of
        // a faulting store).
        emit_release_store(value, 8, width);
        stub.tso_crossing_variant = true;
    } else {
        emit_x64_tso_store(value, 8, 4, width);
    }

    stub.return_pos = m_code_buf.size();
    push_cold_mem_stub(stub);
    m_esp_frame_live = false;
}

}  // namespace vitreo::cpu::jit

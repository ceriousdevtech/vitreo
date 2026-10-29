// Cold bodies for 128/256-bit guest accesses whose probe missed, emitted
// by emit_flush_cold_mem_stubs (jit_memory_emit_cold_stubs.cpp).
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

// Cold body for a 128-bit access whose region probe missed: two 8-byte helper
// calls, staged through SseState::jit_vec_scratch.
//
// Everything here follows from one fact: NOTHING survives a helper call except
// memory. The helper clobbers every caller-saved GPR (X0-X18, so X9 and the EA
// with it), and every callee-saved register is already spoken for -- X12/X13
// and X23-X28 are pins, X19/X20 are CpuState*/JitContext*, X22 is the loop
// counter, and X21 is reserved (fpu.cpp parks an 80-bit mantissa in it across
// a guest load whose slow path runs through here).
//
// So the scratch slot is used twice over. First it carries the two ADDRESSES,
// written while X9 is still live; each is reloaded into X1 just before its
// call. Then, on the load path, each returned half overwrites its own address
// slot -- safe because an address is dead the moment its call has been issued
// -- so that after both calls the slot holds the full 16 bytes and a single
// LDR Q lands it in vec_reg.
void JitCompiler::emit_cold_mem_stub_vec128(const ColdMemStub& s) {
    emit_cold_mem_stub_vec128_half(s, s.dst_arm);
}

void JitCompiler::emit_cold_mem_stub_vec256(const ColdMemStub& s) {
    // X9 does not survive a helper call; X21 (REG_CALL_TMP) is callee-saved
    // and nothing of this instruction's hot path is live in it (the 256-bit
    // lowering parks nothing there, unlike the split form it replaces).
    // Four 8-byte helper pieces, each staged through the CpuState slot it
    // belongs to. No V register is live across any call: see ColdMemStub.
    emit(arm64::mov_x(REG_CALL_TMP, 9));
    const u32 piece_off[4] = {s.vec_slot_lo, s.vec_slot_lo + 8,
                              s.vec_slot_hi, s.vec_slot_hi + 8};
    if (s.is_store) {
        // Release: fence, then the store (the inline path's fence is behind
        // the stub's return; see the 128-bit form). ONE validated 32-byte
        // write (jit_mem_write_span): a store denied anywhere -- its second
        // host page, say -- must perform none of itself, which four 8-byte
        // helper writes cannot promise.
        emit_vector_tso_fence();
        emit(arm64::mov_x(1, REG_CALL_TMP));
        emit_mov_imm32(2, s.vec_slot_lo);
        emit(arm64::add_x(2, REG_CPU_STATE, 2));
        emit_mov_imm32(3, s.vec_slot_hi);
        emit(arm64::add_x(3, REG_CPU_STATE, 3));
        emit(arm64::movz_x(4, 32, 0));
        emit(arm64::mov_x(0, REG_JIT_CTX));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_WRITE_SPAN >> 3));
        emit_memory_helper_blr();
        emit_exit_if_pending_raise(s.spill_nzcv || s.restore_nzcv_on_raise,
                                   s.nzcv_kind_on_raise);
        return;
    }
    for (u32 i = 0; i < 4; ++i) {
        emit(arm64::add_x_imm(1, REG_CALL_TMP, i * 8u));
        emit(arm64::mov_x(0, REG_JIT_CTX));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, s.helper_off >> 3));
        emit_memory_helper_blr();
        emit_exit_if_pending_raise(s.spill_nzcv || s.restore_nzcv_on_raise,
                                   s.nzcv_kind_on_raise);
        emit(arm64::str_x_uoff(0, REG_CPU_STATE, piece_off[i] >> 3));
    }
    // Acquire: fence after the guest loads; then the hot path's own
    // STR Q of dst_arm/dst_arm2 re-stores what is already in the slots.
    emit_vector_tso_fence();
    emit(arm64::ldr_q_uoff(s.dst_arm, REG_CPU_STATE, s.vec_slot_lo >> 4));
    emit(arm64::ldr_q_uoff(s.dst_arm2, REG_CPU_STATE, s.vec_slot_hi >> 4));
}

void JitCompiler::emit_cold_mem_stub_vec128_half(const ColdMemStub& s, u8 vreg) {
    const u32 scratch = jit_vec_scratch_off();
    // alignas(16) makes the offset a multiple of 16, which LDR/STR Q's scaled
    // immediate requires; the two 8-byte accesses need it 8-aligned. Tripwire,
    // not policy -- the offsets are small, fixed and checked once per emit.
    if ((scratch & 15u) != 0 || (scratch >> 4) >= 4096u ||
        ((scratch + 24) >> 3) >= 4096u) {
        std::fprintf(stderr,
                     "[jit] FATAL: jit_vec_scratch offset %u is misaligned or "
                     "outside the scaled load/store immediate range\n",
                     scratch);
        std::abort();
    }
    const u32 val_q  = scratch >> 4;          // [scratch+0 .. +15], LDR/STR Q
    const u32 val_lo = scratch >> 3;          // [scratch+0]
    const u32 val_hi = (scratch + 8) >> 3;    // [scratch+8]
    const u32 adr_lo = (scratch + 16) >> 3;   // [scratch+16]
    const u32 adr_hi = (scratch + 24) >> 3;   // [scratch+24]

    // Park both addresses while X9 is still live. X9 does not survive the
    // first call -- it is caller-saved -- and there is no spare callee-saved
    // register to hold it in (X12/X13/X23-X28 are pins, X19/X20 are the state
    // pointers, X22 is the loop counter, X21 is reserved for fpu.cpp's
    // in-flight mantissa).
    emit(arm64::str_x_uoff(9, REG_CPU_STATE, adr_lo));
    emit(arm64::add_x_imm(10, 9, 8));
    emit(arm64::str_x_uoff(10, REG_CPU_STATE, adr_hi));

    if (s.is_store) {
        // The inline path this stub replaces carries its own fence, and the
        // stub returns AFTER it, so the ordering has to be re-established
        // here or a probe miss silently drops it. Release: fence, then store.
        emit_vector_tso_fence();
        // One STR Q stages the value; then ONE validated 16-byte write
        // (jit_mem_write_span). Two 8-byte helper writes performed the first
        // half of a store whose second half is denied -- x86 performs none
        // of a faulting store (Intel SDM Vol. 3A 6.15).
        emit(arm64::str_q_uoff(vreg, REG_CPU_STATE, val_q));
        emit(arm64::ldr_x_uoff(1, REG_CPU_STATE, adr_lo));
        emit_mov_imm32(2, scratch);
        emit(arm64::add_x(2, REG_CPU_STATE, 2));
        emit(arm64::movz_x(3, 0, 0));
        emit(arm64::movz_x(4, 16, 0));
        emit(arm64::mov_x(0, REG_JIT_CTX));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_WRITE_SPAN >> 3));
        emit_memory_helper_blr();
        emit_exit_if_pending_raise(s.spill_nzcv || s.restore_nzcv_on_raise,
                                   s.nzcv_kind_on_raise);
        return;
    }

    // Load: each returned half is stored straight into its slot, so after both
    // calls the value area holds all 16 bytes and one LDR Q lands it.
    for (int half = 0; half < 2; ++half) {
        emit(arm64::ldr_x_uoff(1, REG_CPU_STATE,
                               half == 0 ? adr_lo : adr_hi));
        emit(arm64::mov_x(0, REG_JIT_CTX));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, s.helper_off >> 3));
        emit_memory_helper_blr();
        emit_exit_if_pending_raise(s.spill_nzcv ||
                                       s.restore_nzcv_on_raise,
                                   s.nzcv_kind_on_raise);
        emit(arm64::str_x_uoff(0, REG_CPU_STATE,
                               half == 0 ? val_lo : val_hi));
    }
    // Acquire: the guest load has happened, so fence after it. Same reason as
    // the store side -- the return lands past the inline path's own fence.
    emit_vector_tso_fence();
    emit(arm64::ldr_q_uoff(vreg, REG_CPU_STATE, val_q));
}

}  // namespace vitreo::cpu::jit

// The 32-bit guest-base (linear) primitives of the Tier-0 memory
// lowering: the linear loads, the address/inline-store helpers, the
// check-byte linear store (fast path and its checked form) and the
// block-tail flush of its cold stubs.
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

void JitCompiler::emit_linear_load32(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    u8 idx = addr_arm_reg;
    if (idx == REG_LINEAR_BASE) {
        emit(arm64::mov_w(9, addr_arm_reg));
        idx = 9;
    }
    if (tso_ordered()) {
        // Acquire form: no offset addressing mode, so fold the base and the
        // zero-extended guest address into X9 first. X9 is already this
        // path's guest-address scratch. Use the fault-patchable two-word
        // site (same as x64) so a 16-byte granule straddle backpatches
        // instead of becoming a guest AV under Ordered.
        const u8 addr_x = emit_linear_addr_x(REG_LINEAR_BASE, idx, 9);
        emit_x64_tso_load(dst_arm_reg, addr_x, 8, 4);
        return;
    }
    emit(arm64::ldr_w_uxtw(dst_arm_reg, REG_LINEAR_BASE, idx));
}
void JitCompiler::emit_linear_load16(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    u8 idx = addr_arm_reg;
    if (idx == REG_LINEAR_BASE) {
        emit(arm64::mov_w(9, addr_arm_reg));
        idx = 9;
    }
    if (tso_ordered()) {
        const u8 addr_x = emit_linear_addr_x(REG_LINEAR_BASE, idx, 9);
        emit_x64_tso_load(dst_arm_reg, addr_x, 8, 2);
        return;
    }
    emit(arm64::ldrh_uxtw(dst_arm_reg, REG_LINEAR_BASE, idx));
}
void JitCompiler::emit_linear_load8(u8 dst_arm_reg, u8 addr_arm_reg) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    u8 idx = addr_arm_reg;
    if (idx == REG_LINEAR_BASE) {
        emit(arm64::mov_w(9, addr_arm_reg));
        idx = 9;
    }
    if (tso_ordered()) {
        const u8 addr_x = emit_linear_addr_x(REG_LINEAR_BASE, idx, 9);
        emit_x64_tso_load(dst_arm_reg, addr_x, 8, 1);
        return;
    }
    emit(arm64::ldrb_uxtw(dst_arm_reg, REG_LINEAR_BASE, idx));
}

u8 JitCompiler::emit_linear_addr_x(u8 base_x, u8 addr_w, u8 scratch_x) {
    // X<scratch> = X<base> + zext32(W<addr>). Writing the scratch after both
    // sources are read makes scratch == addr_w safe, which matters because
    // the guest-address scratch is reused as the address temp on the load
    // paths.
    emit(arm64::add_x_uxtw(scratch_x, base_x, addr_w));
    return scratch_x;
}
void JitCompiler::emit_region_inline_store(u8 src, u8 base_x, u8 off_w,
                                           int width) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (tso_ordered()) {
        const u8 addr_x = emit_linear_addr_x(base_x, off_w, off_w);
        emit_x64_tso_store(src, addr_x, 8, width);
        return;
    }
    switch (width) {
        case 1: emit(arm64::strb_reg(src, base_x, off_w)); return;
        case 2: emit(arm64::strh_reg(src, base_x, off_w)); return;
        case 8: emit(arm64::str_x_reg(src, base_x, off_w)); return;
        default: emit(arm64::str_w_reg(src, base_x, off_w)); return;
    }
}

// Check-byte form of the 32-bit guest-base store. One LDR of the map base, one
// UBFX + LDRB per touched host page, one CBNZ, one store.
//
// It replaces, in one byte:
//
//   * the [code_span_lo, code_span_hi) compare (2 LDR + 2 SUBS + 2 B.cond),
//   * the 2 MiB chunk table probe (LDR + CBZ + LSR + LSL + LDR + CBZ),
//   * the 4 KiB code-page map probe (LDR + CBZ + LSR + LDRB + CBNZ),
//   * and the inline write-watch mark (2 LDR + CBZ + LSR + MOVZ + STRB),
//
// each of them re-deriving per store a fact that changes only when a page
// first holds a translation or first joins a MEM_WRITE_WATCH allocation.
// kLinearCheckHoldsCode and kLinearCheckWatched state those two facts per
// 16 KiB host page, and the cold helper does the write, the SMC notify AND the
// write-watch record, so a set bit is handled by not storing inline at all.
//
// Precision moves in the safe direction on both counts: HoldsCode is published
// before a translation is reachable and cleared only after every record of the
// page holding code is gone, and Watched covers whole host pages even where a
// watched allocation only partly fills them (over-report, never under-report --
// docs: getwritewatch_gc_landmine).
//
// Guest page protection is deliberately NOT part of this: the 32-bit
// reservation keeps every committed host page RW and records the guest flag for
// VirtualQuery only (see GuestMemory::map_region), exactly as the legacy probe
// this replaces did. An uncommitted page is PROT_NONE and faults into the
// recoverable-fault handler either way.
// W10 -> [linear_base + zext(W9)], `width` bytes. The one place the 32-bit
// guest-base store actually writes memory: emitted inline on the fast path and
// again in the warm half of the cold stub, so the two cannot drift.
void JitCompiler::emit_linear_store_body(int width) {
    const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE : REG_SCRATCH3;
    if (!m_linear_base_pinned) {
        emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                JIT_CTX_OFF_LINEAR_BASE >> 3));
    }
    if (tso_ordered()) {
        // Release form: no offset addressing mode, so fold the base and the
        // zero-extended guest address first. Fault-patchable site — see
        // emit_linear_load32.
        const u8 addr_x = emit_linear_addr_x(base, 9, REG_SCRATCH4);
        emit_x64_tso_store(10, addr_x, 8, width);
        return;
    }
    if (width == 1)      emit(arm64::strb_uxtw(10, base, 9));
    else if (width == 2) emit(arm64::strh_uxtw(10, base, 9));
    else if (width == 8) emit(arm64::str_x_uxtw(10, base, 9));
    else                 emit(arm64::str_w_uxtw(10, base, 9));
}
void JitCompiler::emit_linear_store_checked(u8 src_arm_reg, u8 addr_arm_reg,
                                            int width, i32 esp_frame_disp) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    const u32 helper_off = (width == 1) ? JIT_CTX_OFF_WRITE8
                         : (width == 2) ? JIT_CTX_OFF_WRITE16
                         : (width == 4) ? JIT_CTX_OFF_WRITE32
                                        : JIT_CTX_OFF_WRITE64;
    // W9 = address, W10/X10 = value: the dedicated memory scratches, live into
    // the cold stub. The probe below only clobbers X3-X7.
    if (addr_arm_reg != 9) emit(arm64::mov_w(9, addr_arm_reg));
    if (width == 8) emit(arm64::mov_x(10, src_arm_reg));
    else            emit(arm64::mov_w(10, src_arm_reg));

    // X3 = check map base. Not pinned for 32-bit blocks: x15 already holds the
    // ESP frame there, which the x64 path (where x15 pins this map) has no use
    // for.
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_LINEAR_CHECK_MAP >> 3));
    const u8 index_bits = m_cfg.linear32_check_index_bits;
    emit(arm64::ubfx_x(4, 9, static_cast<u8>(kLinearHostPageShift),
                       index_bits));
    // Probe map[i]. Width-1 stays a single LDRB. Wider stores also OR the
    // endpoint's check byte (exact dual-index). That eliminates the LDRH
    // neighbour-only false positives (91.7% of a 32-bit helper's writes) without
    // a same-page skip branch: equal indices make the ORR a no-op.
    //
    // VITREO_JIT_STORE_PROBE_HALFWORD=1 restores the conservative LDRH for
    // browser-guest A/B; it requires kCheckMapPadBytes of slack past the last
    // entry. Do not "fix" halfword false positives in the cold stub with a
    // CMP against a threshold past the 12-bit immediate: cmp_w_imm used to
    // mask it, which corrupted guest state under Fence, and now refuses it
    // (arm64::uimm12 aborts emission).
    if (width > 1 && linear32_store_probe_halfword()) {
        emit(arm64::ldrh_reg(5, 3, 4));
    } else {
        emit(arm64::ldrb_reg(5, 3, 4));
        if (width > 1) {
            // Always probe the endpoint index. When the access stays inside
            // one host page the two UBFX results are equal, so the ORR is a
            // no-op and there is no neighbour false positive. Skipping the
            // second LDRB with EOR+CBZ looked cheaper but corrupted the guest
            // under Fence (EIP=0 / 0xc000001d ~130M insn); keep the dual
            // probe NZCV-clean (no SUBS).
            emit(arm64::add_w_imm(6, 9, static_cast<u32>(width - 1)));
            emit(arm64::ubfx_x(6, 6, static_cast<u8>(kLinearHostPageShift),
                               index_bits));
            emit(arm64::ldrb_reg(7, 3, 6));
            emit(arm64::orr_x(5, 5, 7));
        }
    }
    const u32 check_cbnz = static_cast<u32>(m_code_buf.size());
    emit(0);  // CBNZ W5, .cold  (patched by the stub flush)

    if (esp_frame_disp >= 0 && width == 4 && m_esp_frame_live) {
        if (tso_ordered()) {
            // Stack stores are ordered too. A guest that hands another thread
            // a pointer to a local has published stack memory, and nothing
            // here can prove it did not. The release form has no offset, so
            // the disp (up to 16380 here) is folded in first.
            emit_add_x_uimm24(REG_SCRATCH4, REG_ESP_FRAME,
                              static_cast<u32>(esp_frame_disp));
            emit_x64_tso_store(10, REG_SCRATCH4, 8, 4);
        } else {
            emit(arm64::str_w_uoff(10, REG_ESP_FRAME,
                                   static_cast<u32>(esp_frame_disp) >> 2));
        }
    } else {
        emit_linear_store_body(width);
    }
    // Mark dirty when any MEM_WRITE_WATCH region is live. The check-map cold
    // path already marks on the Watched warm arm; doing the same on the fast
    // path (map byte == 0) restores the legacy over-report safety net. A
    // false-negative Watched bit would otherwise under-report and free live
    // GC objects (exact dual-index made that reachable: halfword neighbour
    // false positives had been routing those stores through record_store).
    // CBZ on a null published map keeps the unwatched case to one load.
    emit_mark_write_watch(9, width > 1 ? static_cast<u32>(width - 1) : 0);

    ColdLinearStoreStub stub;
    stub.page_branch_pos = check_cbnz;
    stub.return_pos = m_code_buf.size();
    stub.helper_off = helper_off;
    stub.width = width;
    stub.spill_nzcv = m_flags.stub_spill_nzcv;
    stub.checked = true;
    stub.replay = m_ir_stub_replay;
    stub.esp_frame_live = m_esp_frame_live;
    m_cold_linear_store_stubs.push_back(stub);
}
void JitCompiler::emit_linear_store(u8 src_arm_reg, u8 addr_arm_reg, int width,
                                    i32 esp_frame_disp) {
    AttrScope attr_scope(*this, EmitAttribution::MemFast);
    if (m_cfg.linear32_check_map) {
        emit_linear_store_checked(src_arm_reg, addr_arm_reg, width,
                                  esp_frame_disp);
        return;
    }
    const u32 helper_off = (width == 1) ? JIT_CTX_OFF_WRITE8
                         : (width == 2) ? JIT_CTX_OFF_WRITE16
                         : (width == 4) ? JIT_CTX_OFF_WRITE32
                                        : JIT_CTX_OFF_WRITE64;
    emit(arm64::mov_w(9, addr_arm_reg));
    if (width == 8) emit(arm64::mov_x(10, src_arm_reg));
    else            emit(arm64::mov_w(10, src_arm_reg));

    // if (store is entirely outside [code_span_lo, code_span_hi)) goto inline.
    // Heap/package / high stack: load span_hi first so addr >= hi skips the
    // span_lo LDR. Empty span (hi==0) treats every address as outside (B.HS).
    emit(arm64::ldr_w_uoff(4, REG_JIT_CTX, JIT_CTX_OFF_CODE_SPAN_HI >> 2));
    emit(arm64::subs_w(31, 9, 4));
    const u32 span_hs = static_cast<u32>(m_code_buf.size());
    emit(0);
    emit(arm64::ldr_w_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_CODE_SPAN_LO >> 2));
    if (width > 1) {
        emit(arm64::add_w_imm(6, 9, static_cast<u32>(width - 1)));
        emit(arm64::subs_w(31, 6, 3));
    } else {
        emit(arm64::subs_w(31, 9, 3));
    }
    const u32 span_lo = static_cast<u32>(m_code_buf.size());
    emit(0);

    // if (!chunk_map || chunk[addr >> 21] == 0) goto inline.
    // 2 MiB chunks stay empty for heap even when the process-wide span
    // covers a low image and a high DLL.
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_CODE_CHUNK_PAGES >> 3));
    const u32 chunk_map_cbz = static_cast<u32>(m_code_buf.size());
    emit(0);
    emit(arm64::lsr_w_imm(4, 9, 21));
    emit(arm64::lsl_w_imm(7, 4, 2));
    emit(arm64::ldr_w_uxtw(5, 3, 7));
    const u32 chunk_cbz = static_cast<u32>(m_code_buf.size());
    emit(0);
    u32 chunk2_cbz = 0;
    u32 same_chunk_beq = 0;
    if (width > 1) {
        emit(arm64::add_w_imm(6, 9, static_cast<u32>(width - 1)));
        emit(arm64::lsr_w_imm(6, 6, 21));
        emit(arm64::subs_w(31, 6, 4));
        same_chunk_beq = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit(arm64::lsl_w_imm(7, 6, 2));
        emit(arm64::ldr_w_uxtw(5, 3, 7));
        chunk2_cbz = static_cast<u32>(m_code_buf.size());
        emit(0);
    }

    // if (!code_page_map) goto inline;
    const u32 page_map_idx = static_cast<u32>(m_code_buf.size());
    emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_CODE_PAGE_MAP >> 3));
    const u32 map_cbz = static_cast<u32>(m_code_buf.size());
    emit(0);
    // if (map[addr >> 12]) goto helper;
    emit(arm64::lsr_w_imm(4, 9, 12));
    emit(arm64::ldrb_uxtw(5, 3, 4));
    const u32 page_cbnz = static_cast<u32>(m_code_buf.size());
    emit(0);
    u32 last_cbnz = 0;
    u32 same_page_beq = 0;
    if (width > 1) {
        emit(arm64::add_w_imm(6, 9, static_cast<u32>(width - 1)));
        emit(arm64::lsr_w_imm(6, 6, 12));
        emit(arm64::subs_w(31, 6, 4));
        same_page_beq = static_cast<u32>(m_code_buf.size());
        emit(0);
        emit(arm64::ldrb_uxtw(5, 3, 6));
        last_cbnz = static_cast<u32>(m_code_buf.size());
        emit(0);
    }
    // Unmarked page(s) fall straight through to the inline store. The code
    // page case (helper writes + notifies SMC, value in W10, addr in W9) is
    // emitted in the block tail by emit_flush_cold_linear_store_stubs() and
    // branches back to the merge label below. W9/W10 stay live into the stub:
    // the probe above only clobbers W3-W7.
    //
    // Inline store at linear_base + zext(addr), or [esp_frame, #disp].
    const u32 inline_idx = static_cast<u32>(m_code_buf.size());
    if (esp_frame_disp >= 0 && width == 4 && m_esp_frame_live) {
        if (tso_ordered()) {
            // Stack stores are ordered too. A guest that hands another
            // thread a pointer to a local has published stack memory, and
            // nothing here can prove it did not. The release form has no
            // offset, so the disp (up to 16380 here) is folded in first.
            emit_add_x_uimm24(REG_SCRATCH4, REG_ESP_FRAME,
                              static_cast<u32>(esp_frame_disp));
            emit_x64_tso_store(10, REG_SCRATCH4, 8, 4);
        } else {
            emit(arm64::str_w_uoff(10, REG_ESP_FRAME,
                                   static_cast<u32>(esp_frame_disp) >> 2));
        }
    } else {
        const u8 base = m_linear_base_pinned ? REG_LINEAR_BASE : REG_SCRATCH3;
        if (!m_linear_base_pinned) {
            emit(arm64::ldr_x_uoff(REG_SCRATCH3, REG_JIT_CTX,
                                    JIT_CTX_OFF_LINEAR_BASE >> 3));
        }
        if (tso_ordered()) {
            // Release form: no offset addressing mode. X4 held span/chunk
            // temporaries that are dead by this point, so it is free to
            // carry the folded host address.
            const u8 addr_x = emit_linear_addr_x(base, 9, REG_SCRATCH4);
            emit_x64_tso_store(10, addr_x, 8, width);
        }
        else if (width == 1) emit(arm64::strb_uxtw(10, base, 9));
        else if (width == 2) emit(arm64::strh_uxtw(10, base, 9));
        else if (width == 8) emit(arm64::str_x_uxtw(10, base, 9));
        else                 emit(arm64::str_w_uxtw(10, base, 9));
    }
    emit_mark_write_watch(9, width > 1 ? static_cast<u32>(width - 1) : 0);

    const u32 done_idx = static_cast<u32>(m_code_buf.size());
    m_code_buf[span_hs] = arm64::b_cond(
        arm64::CS, static_cast<i32>(inline_idx - span_hs) * 4);
    m_code_buf[span_lo] = arm64::b_cond(
        arm64::CC, static_cast<i32>(inline_idx - span_lo) * 4);
    m_code_buf[chunk_map_cbz] = arm64::cbz_x(
        3, static_cast<i32>(inline_idx - chunk_map_cbz) * 4);
    m_code_buf[chunk_cbz] = arm64::cbz_w(
        5, static_cast<i32>(inline_idx - chunk_cbz) * 4);
    if (width > 1) {
        m_code_buf[same_chunk_beq] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(page_map_idx - same_chunk_beq) * 4);
        m_code_buf[chunk2_cbz] = arm64::cbz_w(
            5, static_cast<i32>(inline_idx - chunk2_cbz) * 4);
    }
    m_code_buf[map_cbz] = arm64::cbz_x(
        3, static_cast<i32>(inline_idx - map_cbz) * 4);
    if (width > 1) {
        m_code_buf[same_page_beq] = arm64::b_cond(
            arm64::EQ, static_cast<i32>(inline_idx - same_page_beq) * 4);
    }
    // page_cbnz / last_cbnz are patched by
    // emit_flush_cold_linear_store_stubs() once the tail address is known.
    ColdLinearStoreStub stub;
    stub.page_branch_pos = page_cbnz;
    if (width > 1) stub.last_page_branch_pos = last_cbnz;
    stub.return_pos = done_idx;
    stub.helper_off = helper_off;
    stub.width = width;
    stub.replay = m_ir_stub_replay;
    stub.esp_frame_live = m_esp_frame_live;
    m_cold_linear_store_stubs.push_back(stub);
}
void JitCompiler::emit_flush_cold_linear_store_stubs() {
    AttrScope attr_scope(*this, EmitAttribution::ColdMem);
    // Index-based for the same reason as emit_flush_cold_mem_stubs():
    // emit_exit_if_pending_raise() appends to m_raise_branch_pos while this
    // runs.
    for (usize i = 0; i < m_cold_linear_store_stubs.size(); ++i) {
        const ColdLinearStoreStub s = m_cold_linear_store_stubs[i];
        const usize dest = m_code_buf.size();
        // Capture the live guest flags at stub ENTRY: the only way in is a
        // CBNZ on a register, so PSTATE still holds what the fast path was
        // carrying. The fast path itself is NZCV-clean, which is why the save
        // is paid here and not around every inline store.
        if (s.spill_nzcv) {
            emit(arm64::mrs_nzcv(11));
            emit(arm64::str_x_uoff(11, 31, 1));
        }
        m_code_buf[s.page_branch_pos] = arm64::cbnz_w(
            5, cold_branch_disp(s.page_branch_pos, dest, 19,
                                "linear store stub entry"));
        if (s.last_page_branch_pos != static_cast<usize>(-1)) {
            m_code_buf[s.last_page_branch_pos] = arm64::cbnz_w(
                5, cold_branch_disp(s.last_page_branch_pos, dest, 19,
                                    "linear store stub entry"));
        }
        // Warm half: a host page that is ONLY write-watched needs the store
        // recorded, not translated code invalidated. Calling the C++ helper for
        // that would make every store into a guest GC's heap pay a probe and a
        // call -- and a collector holds a MEM_WRITE_WATCH allocation live for
        // long stretches, so that is not a rare page. The store and the dirty
        // mark are the same instructions the legacy inline path emitted, moved
        // off the fast path.
        //
        // The test is equality on the full probe result, not a bit test: any
        // OTHER reason a page is marked (kLinearCheckHoldsCode today, anything
        // published later) must reach the helper. Fail closed.
        //
        // W5 is the OR of touched check bytes. Equality to Watched means
        // every touched page is watch-only (no HoldsCode). Halfword probes
        // can still produce neighbour-only false positives
        // (map[i]==0, map[i+1]!=0 → W5 = map[i+1]<<8); those are NOT equal
        // to Watched and take the helper. Exact-when-cheap (default) never
        // loads the neighbour without a real straddle, so that case is gone
        // from the hot path. Do not special-case neighbour-only in this stub:
        // a prior rewrite used cmp_w_imm against a 16 KiB threshold. The
        // encoder then masked it to imm12 and corrupted guest state under
        // Fence; it now aborts emission instead.
        usize warm_ne_pos = static_cast<usize>(-1);
        if (s.checked) {
            emit(arm64::subs_w_imm(31, 5, kLinearCheckWatched));
            warm_ne_pos = m_code_buf.size();
            emit(0);  // B.NE .helper
            emit_linear_store_body(s.width);
            emit_mark_write_watch(9, s.width > 1
                                         ? static_cast<u32>(s.width - 1)
                                         : 0);
            if (s.spill_nzcv) {
                emit(arm64::ldr_x_uoff(11, 31, 1));
                emit(arm64::msr_nzcv(11));
            }
            const usize warm_back = m_code_buf.size();
            emit(arm64::b_imm(cold_branch_disp(warm_back, s.return_pos, 26,
                                               "linear store warm return")));
            m_code_buf[warm_ne_pos] = arm64::b_cond(
                arm64::NE,
                cold_branch_disp(warm_ne_pos, m_code_buf.size(), 19,
                                 "linear store helper entry"));
        }
        // Republish a flag record the Tier 1 recovery pass elided, BEFORE the
        // helper can raise.
        //
        // Only the hardware fault handler rebuilds an elided record from the
        // recovery map. A write this helper refuses raises in software through
        // `emit_exit_if_pending_raise`, which the map never reaches — so
        // without this the guest would see the flags of whatever ran before
        // the elided definition. Measured before it was fixed: EFLAGS 0x2
        // against the interpreter's 0x57.
        //
        // Placed before the helper argument setup, and using W0/W1 as
        // temporaries, precisely because those are overwritten by that setup
        // and clobbered by the BLR regardless. W9 (address) and W10 (value)
        // are the helper's operands and must survive, so nothing here touches
        // them. The warm write-watch path above returns without calling the
        // helper and cannot raise, so it needs no replay.
        if (s.replay.active) {
            u8 src = s.replay.src_reg;
            if (src == 0xFF) {
                emit_mov_imm32(REG_SCRATCH0, s.replay.src_imm);
                src = REG_SCRATCH0;
            }
            u8 dst = s.replay.dst_reg;
            if (dst == 0xFF) {
                emit_mov_imm32(REG_SCRATCH1, s.replay.dst_imm);
                dst = REG_SCRATCH1;
            }
            if (s.replay.recompute) {
                // `dst` is the first x86 operand; the record wants the result.
                const auto op = static_cast<CpuState::FlagOp>(s.replay.flag_op);
                switch (op) {
                    case CpuState::FlagOp::Add32:
                        emit(arm64::add_w(REG_SCRATCH1, dst, src)); break;
                    case CpuState::FlagOp::Sub32:
                        emit(arm64::sub_w(REG_SCRATCH1, dst, src)); break;
                    case CpuState::FlagOp::And32:
                        emit(arm64::and_w(REG_SCRATCH1, dst, src)); break;
                    case CpuState::FlagOp::Or32:
                        emit(arm64::orr_w(REG_SCRATCH1, dst, src)); break;
                    case CpuState::FlagOp::Xor32:
                        emit(arm64::eor_w(REG_SCRATCH1, dst, src)); break;
                    default:
                        // The planner only ever offers those five; anything
                        // else would publish a wrong record, so publish none.
                        goto replay_done;
                }
                dst = REG_SCRATCH1;
            }
            emit_lazy_flag_store_raw(
                static_cast<CpuState::FlagOp>(s.replay.flag_op), src, dst);
        }
    replay_done:

        // The helper performs the write, the SMC notify AND the write-watch
        // mark, so this returns past the inline store's own mark rather than
        // to it.
        emit(arm64::mov_x(0, REG_JIT_CTX));
        emit(arm64::mov_w(1, 9));
        if (s.width == 8) emit(arm64::mov_x(2, 10));
        else              emit(arm64::mov_w(2, 10));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, s.helper_off >> 3));
        emit_helper_blr();
        emit_exit_if_pending_raise(s.spill_nzcv);
        // Re-establish the ESP-frame cache before rejoining the hot path.
        //
        // X15 caches `linear_base + ESP` and is caller-saved, so the helper
        // call above destroys it — but only on THIS path, which the compiler
        // emits out of line at flush time and therefore cannot see in its
        // linear order. `m_esp_frame_live` stayed true, later `[esp+disp]`
        // accesses in the hot path kept emitting a bare `LDR [X15,#disp]`, and
        // whichever execution took the stub then read a dead register.
        //
        // Restoring it here rather than clearing the flag is deliberate:
        // clearing would be another thing every future cold-stub author has to
        // remember, and it would cost the optimization on the whole rest of the
        // block. `emit_helper_blr` has already reloaded the pins and the linear
        // base, so X12 and X14 are valid, and this costs one ADD on a path that
        // just made a function call.
        if (s.esp_frame_live) {
            emit(arm64::add_x_uxtw(REG_ESP_FRAME, REG_LINEAR_BASE,
                                   REG_PIN_ESP));
        }
        // Hand the fast path's flags back before rejoining it.
        if (s.spill_nzcv) {
            emit(arm64::ldr_x_uoff(11, 31, 1));
            emit(arm64::msr_nzcv(11));
        }
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(cold_branch_disp(back, s.return_pos, 26,
                                           "linear store stub return")));
    }
    m_cold_linear_store_stubs.clear();
}

}  // namespace vitreo::cpu::jit

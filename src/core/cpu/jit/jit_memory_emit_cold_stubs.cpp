// The block-tail cold memory stub flush of the Tier-0 memory lowering:
// emit_flush_cold_mem_stubs, including the lazy publication of the
// process-wide scalar helpers (jit_memory_emit_process_stubs.cpp owns
// the helper page).
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

namespace {
// Fixed at image startup: subprocess A/B uses the same actual emitter and
// cannot mix helper policies in the process-wide publication table.
const bool g_env_scalar_lr_eager =
    std::getenv("VITREO_JIT_SCALAR_LR_EAGER") != nullptr;
// VITREO_JIT_SUBPAGE_STUB=0: every mixed-page access takes the helper (A/B
// and bisection).
const bool g_env_subpage_stub = [] {
    const char* v = std::getenv("VITREO_JIT_SUBPAGE_STUB");
    return v == nullptr || v[0] != '0';
}();

ScalarMemHelperKey scalar_mem_helper_key(u32 helper_off, int width, bool is_store,
                                         bool pins_live, bool is_64, u8 tso,
                                         bool linear_base_pinned, bool jit_view) {
    return ScalarMemHelperKey{
        helper_off,
        width,
        static_cast<u8>(is_store ? 1 : 0),
        static_cast<u8>(pins_live ? 1 : 0),
        static_cast<u8>(is_64 ? 1 : 0),
        tso,
        static_cast<u8>(linear_base_pinned ? 1 : 0),
        static_cast<u8>(jit_view ? 1 : 0),
    };
}
}  // namespace

void* JitCompiler::adopt_scalar_mem_helper(u32 helper_off, int width, bool is_store) {
    const ScalarMemHelperKey key = scalar_mem_helper_key(
        helper_off, width, is_store, m_pins_live, m_block_is_64bit,
        static_cast<u8>(m_cfg.tso_mode), m_linear_base_pinned, m_cfg.linear64_jit_view);
    std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
    void* existing = lookup_scalar_mem_helper(key);
    if (existing != nullptr) {
        m_jit_ctx.linear64_cold_mem[linear64_cold_mem_slot(is_store, width)] = existing;
    }
    return existing;
}

void JitCompiler::emit_flush_cold_mem_stubs() {
    AttrScope attr_scope(*this, EmitAttribution::ColdMem);
    // The shared scalar helpers' ABI (unless jit_cold_stub_legacy()): the
    // site leaves the instruction's RIP in X8 for the helper's first word to
    // publish, and every return leaves CpuState.pending_raise.active in W16,
    // so the site's raise check is the CBNZ alone.
    constexpr u32 kRaiseActiveOffset = static_cast<u32>(
        offsetof(CpuState, pending_raise) +
        offsetof(CpuState::PendingRaiseException, active));
    static_assert(kRaiseActiveOffset < 4096);
    auto emit_shared_raise_flag = [&] {
        emit(arm64::ldrb_uoff(16, REG_CPU_STATE, kRaiseActiveOffset));
    };
    // Index-based: emit_exit_if_pending_raise() appends to m_raise_branch_pos
    // while this runs. It does not append to m_cold_mem_stubs, but iterating
    // by index keeps that assumption from becoming a dangling reference if it
    // ever changes.
    // `mixed_dest`: where the check-byte branch goes, when that is not `dest`
    // (the sub-page entry, emit_subpage_entry).
    auto patch_branches = [&](const ColdMemStub& s, usize dest,
                              usize mixed_dest = static_cast<usize>(-1)) {
        if (s.permission_below_branch_pos != static_cast<usize>(-1)) {
            m_code_buf[s.permission_below_branch_pos] = arm64::b_cond(
                arm64::CC,
                cold_branch_disp(s.permission_below_branch_pos, dest, 19,
                                 "mem permission stub entry"));
        }
        if (s.permission_above_branch_pos != static_cast<usize>(-1)) {
            m_code_buf[s.permission_above_branch_pos] = arm64::b_cond(
                arm64::CS,
                cold_branch_disp(s.permission_above_branch_pos, dest, 19,
                                 "mem permission stub entry"));
        }
        if (s.mixed_page_branch_pos != static_cast<usize>(-1)) {
            // A store asks the whole check byte: every reason a host page needs
            // software handling -- inexact protection, or holding translated
            // code -- applies to writing it. A load asks only the protection
            // bit, so it still reads a page that merely holds code directly.
            // TBNZ carries only imm14, and a displacement past its field would
            // decline the whole block (cold_branch_disp). A block long enough
            // to overflow it must still compile, so fall back to
            // the imm19 CBNZ: testing the whole byte is always SAFE, merely
            // conservative (a load from a page that only holds code takes the
            // helper). Correctness never depends on which form is chosen.
            const usize mdest =
                mixed_dest != static_cast<usize>(-1) ? mixed_dest : dest;
            const i64 words = static_cast<i64>(mdest) -
                              static_cast<i64>(s.mixed_page_branch_pos);
            const bool bit_test_reaches =
                !s.is_store && words >= -(i64{1} << 13) &&
                words < (i64{1} << 13);
            // A LOAD asks the read-specific bit; a STORE asks the general
            // mixed bit. The overwhelmingly common mixed page disagrees only
            // about WRITABILITY (a PE's 4 KiB SectionAlignment putting .rdata
            // beside .data in one 16 KiB host page), and gating loads on the
            // write-oriented bit sent 686,080,048 harmless reads to the helper
            // on sc_blocksize rung 1 -- 100% of that workload's helper reads.
            // `bit_test_reaches` is already `!s.is_store`, so the single-bit
            // form is a LOAD's alone; a store takes the CBNZ below and so asks
            // the whole byte, which is what a store should ask.
            m_code_buf[s.mixed_page_branch_pos] =
                bit_test_reaches
                    ? arm64::tbnz(
                          3, kLinearCheckMixedReadBit,
                          cold_branch_disp(s.mixed_page_branch_pos, mdest, 14,
                                           "mem check-page stub entry"))
                    : arm64::cbnz_x(
                          3, cold_branch_disp(s.mixed_page_branch_pos, mdest, 19,
                                              "mem check-page stub entry"));
        }
        if (s.host_page_cross_branch_pos != static_cast<usize>(-1)) {
            m_code_buf[s.host_page_cross_branch_pos] = arm64::cbnz_x(
                8, cold_branch_disp(s.host_page_cross_branch_pos, dest, 19,
                                    "mem host-page crossing stub entry"));
        }

        if (s.protection_generation_branch_pos != static_cast<usize>(-1)) {
            m_code_buf[s.protection_generation_branch_pos] = arm64::b_cond(
                arm64::NE,
                cold_branch_disp(s.protection_generation_branch_pos, dest, 19,
                                 "mem protection generation stub entry"));
        }
        if (!s.single_upper_branch) {
            m_code_buf[s.below_branch_pos] = arm64::b_cond(
                arm64::CC,
                cold_branch_disp(s.below_branch_pos, dest, 19,
                                 "mem stub entry"));
        }
        if (s.single_upper_branch) {
            if (s.overflow_branch_pos != static_cast<usize>(-1)) {
                m_code_buf[s.overflow_branch_pos] = arm64::b_cond(
                    arm64::CS,
                    cold_branch_disp(s.overflow_branch_pos, dest, 19,
                                     "mem stub entry"));
            }
            if (!s.range_proven) {
                m_code_buf[s.above_branch_pos] = s.hot_helper_fail
                    ? arm64::cbnz_w(
                          16, cold_branch_disp(s.above_branch_pos, dest, 19,
                                               "hot helper fail"))
                    : arm64::cbnz_x(
                          8, cold_branch_disp(s.above_branch_pos, dest, 19,
                                              "mem stub entry"));
            }
            if (s.watch_branch_pos != static_cast<usize>(-1)) {
                m_code_buf[s.watch_branch_pos] = arm64::cbnz_x(
                    3, cold_branch_disp(s.watch_branch_pos, dest, 19,
                                        "mem stub entry"));
            }
            if (s.smc_branch_pos != static_cast<usize>(-1)) {
                m_code_buf[s.smc_branch_pos] = arm64::b_imm(
                    cold_branch_disp(s.smc_branch_pos, dest, 26,
                                     "mem stub entry"));
            }
        } else {
            m_code_buf[s.above_branch_pos] = arm64::b_cond(
                arm64::CS,
                cold_branch_disp(s.above_branch_pos, dest, 19,
                                 "mem stub entry"));
        }
    };

    // `helper_stores_rip`: the site calls a shared helper whose first word
    // stores X8 to CpuState.rip (emit_shared_entry_rip), so the site only
    // forms the RIP -- or reloads the published one when it has no owner.
    auto emit_rip_and_nzcv = [&](const ColdMemStub& s,
                                 bool helper_stores_rip = false) {
        // Checked helpers raise pending AVs directly and never enter the Mach
        // fault handler, so move precise-RIP publication from every x64 hot
        // memory path to the cold stub that can actually need it.
        if (u32 insn = 0;
            m_block_is_64bit && owning_insn(s.above_branch_pos, insn)) {
            // Linked hops may leave CpuState.rip at a predecessor, so even
            // the first instruction must publish its address. X0 may still
            // hold the effective address. The normal RIP publisher uses X0,
            // before the trampoline copies the address to X9, and would turn
            // this into a load from the instruction stream. X8 is the
            // range-check scratch and is dead on every cold edge. Preserve
            // all operand homes and NZCV while publishing precise RIP.
            emit_mov_imm64(8, static_cast<i64>(m_insn_rips[insn]));
            if (!helper_stores_rip) {
                emit(arm64::str_x_uoff(8, REG_CPU_STATE, rip_offset() >> 3));
            }
        } else if (helper_stores_rip && m_block_is_64bit) {
            emit(arm64::ldr_x_uoff(8, REG_CPU_STATE, rip_offset() >> 3));
        }
        // Capture the live guest flags at stub ENTRY: every branch that
        // reaches here is a CBNZ/TBNZ on a register, so PSTATE still holds
        // exactly what the fast path was carrying.
        if (s.spill_nzcv) {
            emit(arm64::mrs_nzcv(11));
            emit(arm64::str_x_uoff(11, 31, 1));
        }
    };

    // A linear64 scalar access whose host page is MIXED: decide it here, from
    // the per-guest-page bits GuestMemory::linear_subpage_map() publishes out
    // of the region table, and on success perform exactly the direct access
    // the fast path would have -- instead of marshalling into the C++ helper.
    // Measured on a 64-bit title: ~750k accesses/s reach mixed pages, nearly
    // all to live guest pages of a host page whose other 4 KiB is free (a
    // VirtualAlloc block's tail).
    //
    // Entered only from the check-byte branch, which leaves the page's check
    // byte in X3; the reservation/crossing branch keeps the ordinary entry,
    // so the access here never crosses a host page. A store whose page also
    // holds translated code or a write watch, a denied guest page, or a
    // missing map all fall through to the ordinary entry and its helper.
    // X3/X8/X17 are the fast path's guard scratches; every instruction is
    // flag-neutral, so live guest NZCV needs no spill. The RIP is published
    // first: a concurrent VirtualFree can still make the direct access fault.
    // Returns the entry, or npos when the site has no such path.
    auto emit_subpage_entry = [&](const ColdMemStub& s) -> usize {
        constexpr usize npos = static_cast<usize>(-1);
        const bool enabled = g_env_subpage_stub;
        const bool scalar =
            !s.is_vec128 && !s.is_vec256 &&
            (s.width == 1 || s.width == 2 || s.width == 4 || s.width == 8);
        const bool vec128 = s.is_vec128 && s.width == 16 && s.addr_arm != 3 &&
                            s.addr_arm != 8 && s.addr_arm != 17;
        if (!enabled || !m_cfg.linear64 || !m_cfg.linear64_hw_protect ||
            m_jit_ctx.linear_subpage_map == nullptr ||
            s.mixed_page_branch_pos == npos || !s.single_upper_branch ||
            s.hot_helper_fail || !(scalar || vec128)) {
            return npos;
        }
        static_assert(kLinearCheckHoldsCode == 2u && kLinearCheckWatched == 4u,
                      "the store test below extracts bits 1-2");
        const usize entry = m_code_buf.size();
        if (u32 insn = 0;
            m_block_is_64bit && owning_insn(s.above_branch_pos, insn)) {
            emit_mov_imm64(8, static_cast<i64>(m_insn_rips[insn]));
            emit(arm64::str_x_uoff(8, REG_CPU_STATE, rip_offset() >> 3));
        }
        std::vector<std::pair<usize, u32>> fails;  // (pos, kind) -> ordinary entry
        const auto fail_cbnz = [&](u8 reg) { fails.push_back({m_code_buf.size(), 0u | reg}); emit(0); };
        const auto fail_tbz0 = [&](u8 reg) { fails.push_back({m_code_buf.size(), 0x100u | reg}); emit(0); };
        const u8 ea = s.addr_arm;
        if (s.is_store) {
            emit(arm64::ubfx_w(8, 3, 1, 2));  // HoldsCode | Watched
            fail_cbnz(8);
        }
        if (s.width > 1 && s.tso_crossing_variant) {
            // A crossing-by-fault site has no crossing test of its own before
            // the check-byte branch, so a multi-byte access arriving here may
            // leave the host page: that one takes the helper.
            emit(arm64::add_x_imm(8, ea, static_cast<u32>(s.width - 1)));
            emit(arm64::eor_x(8, 8, ea));
            fails.push_back({m_code_buf.size(), 0x200u | 8u});
            emit(0);  // TBNZ X8, #14 -> ordinary
        }
        const u8 index_width =
            static_cast<u8>(m_cfg.linear64_address_bits - kLinearHostPageShift);
        emit(arm64::ldr_x_uoff(17, REG_JIT_CTX,
                               JIT_CTX_OFF_LINEAR_SUBPAGE_MAP >> 3));
        emit(arm64::ubfx_x(8, ea, static_cast<u8>(kLinearHostPageShift),
                           index_width));
        emit(arm64::ldrb_reg(8, 17, 8));
        if (s.is_store) emit(arm64::lsr_w_imm(8, 8, 4));  // the write bits
        // Every guest page the access touches must allow it: its first byte's
        // and, for a wider access, its last byte's (at most two).
        emit(arm64::ubfx_x(3, ea, static_cast<u8>(kLinearGuestPageShift), 2));
        emit(arm64::lsrv_w(3, 8, 3));
        fail_tbz0(3);
        if (s.width > 1) {
            emit(arm64::add_x_imm(17, ea, static_cast<u32>(s.width - 1)));
            emit(arm64::ubfx_x(17, 17, static_cast<u8>(kLinearGuestPageShift), 2));
            emit(arm64::lsrv_w(17, 8, 17));
            fail_tbz0(17);
        }
        if (vec128) {
            // The fast path's own access; dst_arm names the vector register.
            if (s.is_store) {
                emit_vector_tso_fence();
                emit(arm64::str_q_reg(s.dst_arm, REG_LINEAR_BASE, ea));
            } else {
                emit(arm64::ldr_q_reg(s.dst_arm, REG_LINEAR_BASE, ea));
                emit_vector_tso_fence();
            }
        } else if (s.is_store) {
            emit(arm64::add_x(8, REG_LINEAR_BASE, ea));
            emit_x64_tso_store(s.store_arm, 8, 4, s.width);
        } else {
            emit(arm64::add_x(17, REG_LINEAR_BASE, ea));
            emit_x64_tso_load(s.dst_arm, 17, 8, s.width);
        }
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(cold_branch_disp(back, s.return_pos, 26,
                                           "mem sub-page stub return")));
        const usize ordinary = m_code_buf.size();
        for (const auto& [pos, kind] : fails) {
            const u8 reg = static_cast<u8>(kind & 0xFFu);
            m_code_buf[pos] =
                (kind & 0x200u) != 0
                    ? arm64::tbnz(reg, static_cast<u8>(kLinearHostPageShift),
                                  cold_branch_disp(pos, ordinary, 14,
                                                   "mem sub-page crossing"))
                : (kind & 0x100u) != 0
                    ? arm64::tbz(reg, 0, cold_branch_disp(pos, ordinary, 14,
                                                          "mem sub-page denied"))
                    : arm64::cbnz_w(reg, cold_branch_disp(pos, ordinary, 19,
                                                          "mem sub-page reason"));
        }
        return entry;
    };

    // The range variant of a range-by-fault site (linear_fault.hpp,
    // kRangeVariantWords): what the fault handler rewrites the site's
    // unmasked `LSR X3, Xea, #14` to branch to once its map load has faulted,
    // i.e. once the site has seen an EA the reservation does not cover. The
    // reservation test (outside -> the ordinary entry and its helper), the
    // masked index, the check-byte load, and back to the site's check-byte
    // branch. Laid out immediately before the crossing variant, or before the
    // check-byte branch's target when there is none. Returns its CBNZ, which
    // the caller points at the ordinary entry, or npos.
    auto emit_range_variant = [&](const ColdMemStub& s) -> usize {
        constexpr usize npos = static_cast<usize>(-1);
        if (!s.range_variant) return npos;
        const usize start = m_code_buf.size();
        const u8 bits = m_cfg.linear64_address_bits;
        emit(arm64::lsr_x_imm(8, s.addr_arm, bits));
        const usize outside = m_code_buf.size();
        emit(0);  // CBNZ X8 -> ordinary entry
        emit(arm64::ubfx_x(3, s.addr_arm, static_cast<u8>(kLinearHostPageShift),
                           static_cast<u8>(bits - kLinearHostPageShift)));
        // X15 is the check map here: range variants exist only under
        // linear64_range_by_fault, which the JIT view turns off (its X15 is
        // the view, see JitConfig::linear64_jit_view).
        emit(arm64::ldrb_reg(3, REG_LINEAR64_CHECK_MAP, 3));
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(cold_branch_disp(back, s.mixed_page_branch_pos, 26,
                                           "mem range variant return")));
        assert(m_code_buf.size() - start == kRangeVariantWords &&
               "the fault handler locates the variant by its fixed length");
        (void)start;
        return outside;
    };
    auto patch_range_variant = [&](usize outside, usize dest) {
        if (outside == static_cast<usize>(-1)) return;
        m_code_buf[outside] = arm64::cbnz_x(
            8, cold_branch_disp(outside, dest, 19, "mem range variant outside"));
    };

    // The crossing variant of a crossing-by-fault site (linear_fault.hpp,
    // kTsoCrossingVariantWords): the fault handler rewrites the site's
    // ordered access to a B here once that access alignment-faults, i.e.
    // crossed a 16-byte granule. It is laid out IMMEDIATELY before the
    // check-byte branch's target -- the caller emits the sub-page entry (or
    // the ordinary entry) next -- which is how the handler finds it. Entered
    // with the host address already formed (X17 for a load, X8 for a store),
    // the EA in addr_arm and live NZCV in PSTATE; X3 is dead. Returns the
    // position of its crossing branch, which the caller points at the
    // ordinary entry, or npos when the site has no variant.
    auto emit_crossing_variant = [&](const ColdMemStub& s) -> usize {
        constexpr usize npos = static_cast<usize>(-1);
        if (!s.tso_crossing_variant) return npos;
        const usize start = m_code_buf.size();
        const u8 ea = s.addr_arm;
        // A host-page crossing (which includes leaving the reservation) takes
        // the checked helper, which decides both pages exactly.
        emit(arm64::add_x_imm(3, ea, static_cast<u32>(s.width - 1)));
        emit(arm64::eor_x(3, 3, ea));
        const usize crossing = m_code_buf.size();
        emit(0);  // TBNZ X3, #14 -> ordinary entry
        // Precise RIP: the plain access below can still fault (an unaligned
        // access to an unmapped page reports BUS_ADRALN here, which is what
        // sent it here). The variant is its instruction's second host
        // interval (pushed below), or, legacy, publishes the RIP itself in a
        // fixed four MOVZ/MOVK and a STR.
        u32 owner = 0;
        const bool owned = owning_insn(s.above_branch_pos, owner);
        if (jit_cold_stub_legacy()) {
            const u64 rip = owned ? m_insn_rips[owner] : 0;
            emit(arm64::movz_x(3, static_cast<u16>(rip), 0));
            emit(arm64::movk_x(3, static_cast<u16>(rip >> 16), 16));
            emit(arm64::movk_x(3, static_cast<u16>(rip >> 32), 32));
            emit(arm64::movk_x(3, static_cast<u16>(rip >> 48), 48));
            emit(arm64::str_x_uoff(3, REG_CPU_STATE, rip_offset() >> 3));
        }
        // The fenced plain access: what the two-word site patches itself to.
        constexpr u32 kDmbIsh = 0xD5033BBFu;
        if (s.is_store) {
            emit(kDmbIsh);
            switch (s.width) {
                case 2: emit(arm64::strh_uoff(s.store_arm, 8, 0)); break;
                case 4: emit(arm64::str_w_uoff(s.store_arm, 8, 0)); break;
                default: emit(arm64::str_x_uoff(s.store_arm, 8, 0)); break;
            }
        } else {
            switch (s.width) {
                case 2: emit(arm64::ldrh_uoff(s.dst_arm, 17, 0)); break;
                case 4: emit(arm64::ldr_w_uoff(s.dst_arm, 17, 0)); break;
                default: emit(arm64::ldr_x_uoff(s.dst_arm, 17, 0)); break;
            }
            emit(kDmbIsh);
        }
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(cold_branch_disp(back, s.return_pos, 26,
                                           "mem crossing variant return")));
        assert(m_code_buf.size() - start == tso_crossing_variant_words() &&
               "the fault handler locates the variant by its fixed length");
        if (!jit_cold_stub_legacy() && owned) {
            m_lean_variant_ranges.push_back({start, m_code_buf.size(), owner});
        }
        return crossing;
    };
    auto patch_crossing_variant = [&](usize crossing, usize dest) {
        if (crossing == static_cast<usize>(-1)) return;
        m_code_buf[crossing] = arm64::tbnz(
            3, static_cast<u8>(kLinearHostPageShift),
            cold_branch_disp(crossing, dest, 14, "mem crossing variant"));
    };

    // The checked variant V of a JIT-view lean site (ColdMemStub::
    // lean_variant): reached by the site's window CBNZ for an EA outside the
    // window, and by the B the fault handler rewrites the site's head to once
    // the enforcing view refused it. Exactly the site's pre-view fast path,
    // through the HOST view:
    //
    //     LSR X8, Xea, #bits ; CBNZ X8 -> ordinary entry
    //     UBFX X3, Xea, #14  ; LDR X16, [ctx, map] ; LDRB W3, [X16, X3]
    //     TBNZ W3, #MixedRead | CBNZ X3 -> sub-page / ordinary entry
    //     ADD X17|X8, X14, Xea ; LDAPR|STLR Rt, [X17|X8]
    //     B return_pos
    //
    // so its last three accesses words are the marker-free ordered site of
    // linear_fault.hpp: the crossing variant laid out next (immediately
    // before the check-byte branch's target) is found from it, and must
    // return into V's final B (crossing_view). Every word is NZCV-neutral.
    // Laid out FIRST among the stub's pieces; mutates @p s so the stub's
    // check-byte branch is V's.
    struct LeanVariant {
        usize entry = static_cast<usize>(-1);
        usize outside = static_cast<usize>(-1);
        usize back = static_cast<usize>(-1);
    };
    auto emit_lean_variant = [&](ColdMemStub& s) -> LeanVariant {
        LeanVariant v;
        if (!s.lean_variant) return v;
        const u8 ea = s.addr_arm;
        const u8 bits = m_cfg.linear64_address_bits;
        v.entry = m_code_buf.size();
        if (!m_cold_lazy_late) {
            m_code_buf[s.lean_window_pos] = arm64::cbnz_x(
                s.lean_window_is_locator ? u8{31} : u8{16},
                cold_branch_disp(s.lean_window_pos, v.entry, 19,
                                 "lean site variant"));
        }
        if (s.is_vec128 || s.is_vec256) {
            // A crossing vector access takes the ordinary entry too: the
            // helper decides both pages before writing either.
            emit(arm64::add_x_imm(17, ea, static_cast<u32>(s.width - 1)));
            emit(arm64::eor_x(8, ea, 17));
            emit(arm64::ubfx_x(8, 8, static_cast<u8>(kLinearHostPageShift), 1));
            emit(arm64::orr_x_lsr(8, 8, ea, bits));
        } else {
            emit(arm64::lsr_x_imm(8, ea, bits));
        }
        v.outside = m_code_buf.size();
        emit(0);  // CBNZ X8 -> ordinary entry
        emit(arm64::ubfx_x(3, ea, static_cast<u8>(kLinearHostPageShift),
                           static_cast<u8>(bits - kLinearHostPageShift)));
        emit(arm64::ldrb_reg(3, emit_linear64_check_map_base(16), 3));
        s.mixed_page_branch_pos = m_code_buf.size();
        emit(0);  // check byte -> sub-page / ordinary entry (patch_branches)
        if (s.is_vec128) {
            if (s.is_store) {
                emit_vector_tso_fence();
                emit(arm64::str_q_reg(s.dst_arm, REG_LINEAR_BASE, ea));
            } else {
                emit(arm64::ldr_q_reg(s.dst_arm, REG_LINEAR_BASE, ea));
                emit_vector_tso_fence();
            }
        } else if (s.is_vec256) {
            emit(arm64::add_x(17, REG_LINEAR_BASE, ea));
            if (s.is_store) {
                emit_vector_tso_fence();
                emit(arm64::stp_q_off(s.dst_arm, s.dst_arm2, 17, 0));
            } else {
                emit(arm64::ldp_q_off(s.dst_arm, s.dst_arm2, 17, 0));
                emit_vector_tso_fence();
            }
        } else if (s.is_store) {
            emit(arm64::add_x(8, REG_LINEAR_BASE, ea));
            emit_release_store(s.store_arm, 8, s.width);
        } else {
            emit(arm64::add_x(17, REG_LINEAR_BASE, ea));
            emit_acquire_load(s.dst_arm, 17, s.width);
        }
        v.back = m_code_buf.size();
        emit(arm64::b_imm(cold_branch_disp(v.back, s.return_pos, 26,
                                           "lean variant return")));
        if (u32 insn = 0; owning_insn(s.lean_window_pos, insn)) {
            m_lean_variant_ranges.push_back({v.entry, m_code_buf.size(), insn});
        }
        return v;
    };
    // A lean stub's crossing variant returns into V (after V's ordered
    // access), which is where the handler's "returns to site + 4" looks.
    auto crossing_view = [](const ColdMemStub& s, const LeanVariant& v) {
        ColdMemStub c = s;
        if (v.back != static_cast<usize>(-1)) c.return_pos = v.back;
        return c;
    };
    auto patch_lean_variant = [&](const LeanVariant& v, usize dest) {
        if (v.outside == static_cast<usize>(-1)) return;
        m_code_buf[v.outside] = arm64::cbnz_x(
            8, cold_branch_disp(v.outside, dest, 19, "lean variant outside"));
    };

    auto emit_ea_to_x9 = [&](const ColdMemStub& s) {
        if (s.addr_arm != 9) emit(arm64::mov_x(9, s.addr_arm));
    };

    // try_region + helper. `load_dst` is the cached-hit / helper-return home.
    // When `shared` the site already placed the store value in X10, the
    // helper return stays in X0, and the caller emits raise / dest / unspill.
    auto emit_scalar_core = [&](const ColdMemStub& s, u8 load_dst, bool shared) {
        usize cached_access_done = static_cast<usize>(-1);
        const bool try_region = s.single_upper_branch && !m_cfg.no_jitcache;
        const u8 helper_store_arm = (try_region || shared) ? 10 : s.store_arm;
        if (try_region) {
            if (!shared && s.is_store && s.store_arm != 10)
                emit(arm64::mov_x(10, s.store_arm));
            emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                                   JIT_CTX_OFF_PROTECT_SEQUENCE_LIVE >> 3));
            emit(arm64::ldar_x(3, 3));
            emit(arm64::ldr_x_uoff(4, REG_JIT_CTX,
                                   JIT_CTX_OFF_PROTECT_SEQUENCE_SEEN >> 3));
            emit(arm64::subs_x(31, 3, 4));
            const usize stale = m_code_buf.size(); emit(0);
            usize stale_code = static_cast<usize>(-1);
            usize stale_watch = static_cast<usize>(-1);
            if (s.is_store) {
                emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_CODE_GENERATION_LIVE >> 3));
                emit(arm64::ldar_x(3, 3));
                emit(arm64::ldr_x_uoff(4, REG_JIT_CTX, JIT_CTX_OFF_WREGION_CODE_GEN >> 3));
                emit(arm64::subs_x(31, 3, 4));
                stale_code = m_code_buf.size(); emit(0);
                emit(arm64::ldr_x_uoff(3, REG_JIT_CTX, JIT_CTX_OFF_WATCH_GENERATION_LIVE >> 3));
                emit(arm64::ldar_w(3, 3));
                emit(arm64::ldr_w_uoff(4, REG_JIT_CTX, JIT_CTX_OFF_WREGION_WATCH_GEN >> 2));
                emit(arm64::subs_w(31, 3, 4));
                stale_watch = m_code_buf.size(); emit(0);
            }
            emit(arm64::ubfx_x(4, 9, static_cast<u8>(X64_SLOT_GRANULE_SHIFT),
                               static_cast<u8>(X64_SLOT_INDEX_BITS)));
            emit(arm64::ldr_x_uoff(3, REG_JIT_CTX,
                (s.is_store ? JIT_CTX_OFF_X64_WSLOTS : JIT_CTX_OFF_X64_RSLOTS) >> 3));
            emit(arm64::add_x_lsl(3, 3, 4, 5));
            emit(arm64::ldp_x_off(4, 8, 3, 0));
            emit(arm64::subs_x(4, 9, 4));
            const usize below = m_code_buf.size(); emit(0);
            // Subtract before comparing width so near-UINT64_MAX addresses
            // cannot wrap the last-byte check back into a cached region.
            emit(arm64::subs_x(8, 8, 9));
            const usize above = m_code_buf.size(); emit(0);
            emit(arm64::subs_x_imm(31, 8, static_cast<u32>(s.width)));
            const usize short_range = m_code_buf.size(); emit(0);
            emit(arm64::ldr_x_uoff(3, 3, 2));
            emit(arm64::add_x(17, 3, 4));
            // X3's region-base value is dead after forming X17. Keep the
            // shared caller LR in this scratch register across the access:
            // the ordered-fault retry trampoline preserves X3 but uses X30
            // as its retry PC. Restoring X30 also preserves our private ABI.
            if (shared && !g_env_scalar_lr_eager) emit(arm64::mov_x(3, 30));
            if (s.is_store) emit_x64_tso_store(10, 17, 8, s.width);
            else emit_x64_tso_load(load_dst, 17, 8, s.width);
            if (shared && !g_env_scalar_lr_eager) {
                emit(arm64::mov_x(30, 3));
                if (!jit_cold_stub_legacy()) emit_shared_raise_flag();
                emit(arm64::ret());
            } else {
                cached_access_done = m_code_buf.size(); emit(0);
            }
            const usize helper = m_code_buf.size();
            if (s.is_store) {
                for (usize branch : {stale_code, stale_watch}) {
                    m_code_buf[branch] = arm64::b_cond(arm64::NE,
                        cold_branch_disp(branch, helper, 19, "linear store proof changed"));
                }
            }
            for (auto [branch, condition] : {
                     std::pair{stale, arm64::NE}, std::pair{below, arm64::CC},
                     std::pair{above, arm64::CC}, std::pair{short_range, arm64::CC}}) {
                m_code_buf[branch] = arm64::b_cond(condition,
                    cold_branch_disp(branch, helper, 19, "linear region miss"));
            }
        }

        // Only the C++ miss path needs a memory-backed LR save. Cache hits
        // above return through the register preserved by fault retries.
        if (shared && !g_env_scalar_lr_eager)
            emit(arm64::str_x_uoff(30, REG_JIT_CTX, JIT_CTX_OFF_COLD_MEM_LR >> 3));

        // Entered with the full 64-bit EA in X9 (and the store value in X10);
        // X3/X4/X8/X10 are dead on the fast path, so the marshal may clobber
        // them freely. X21 is deliberately untouched.
        emit(arm64::mov_x(1, 9));
        if (s.is_store && m_cfg.tso_mode != TsoMode::Off) {
            // The inline access carries ordering in STLR, but this stub
            // returns past it. A pre-helper DMB gives the helper store the
            // same release edge; store->load remains reorderable as x86 TSO
            // permits.
            emit(0xD5033BBFu);  // DMB ISH
        }
        if (s.is_store) {
            if (s.width == 8) {
                emit(arm64::mov_x(2, helper_store_arm));
            } else {
                emit(arm64::mov_w(2, helper_store_arm));
            }
        }
        emit(arm64::mov_x(0, REG_JIT_CTX));
        emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, s.helper_off >> 3));
        emit_memory_helper_blr();
        if (shared) {
            if (!s.is_store && m_cfg.tso_mode != TsoMode::Off)
                emit(0xD5033BBFu);  // DMB ISH — acquire
            if (cached_access_done != static_cast<usize>(-1)) {
                m_code_buf[cached_access_done] = arm64::b_imm(cold_branch_disp(
                    cached_access_done, m_code_buf.size(), 26,
                    "linear region hit"));
            }
            return;
        }
        emit_exit_if_pending_raise(s.spill_nzcv ||
                                       s.restore_nzcv_on_raise,
                                   s.nzcv_kind_on_raise);
        if (!s.is_store) {
            // AAPCS64 u32 returns do not zero Xn[63:32]. Always UXTW,
            // including dest==W0 (`mov w0,w0`). read64 returns a full X.
            if (s.width == 8) {
                emit(arm64::mov_x(s.dst_arm, 0));
            } else {
                emit(arm64::mov_w(s.dst_arm, 0));
            }
            if (m_cfg.tso_mode != TsoMode::Off)
                emit(0xD5033BBFu);  // DMB ISH — acquire
        }
        if (cached_access_done != static_cast<usize>(-1)) {
            m_code_buf[cached_access_done] = arm64::b_imm(cold_branch_disp(
                cached_access_done, m_code_buf.size(), 26, "linear region hit"));
        }
        if (s.spill_nzcv) {
            emit(arm64::ldr_x_uoff(11, 31, 1));
            emit(arm64::msr_nzcv(11));
        }
        emit_xmm_cache_reload(s.xmm_cache_live);
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(
            cold_branch_disp(back, s.return_pos, 26, "mem stub return")));
    };

    auto emit_site_tail = [&](const ColdMemStub& s, bool flag_in_w16 = false) {
        emit_exit_if_pending_raise(s.spill_nzcv || s.restore_nzcv_on_raise,
                                   s.nzcv_kind_on_raise, flag_in_w16);
        if (!s.is_store) {
            if (s.width == 8) emit(arm64::mov_x(s.dst_arm, 0));
            else emit(arm64::mov_w(s.dst_arm, 0));
        }
        if (s.spill_nzcv) {
            emit(arm64::ldr_x_uoff(11, 31, 1));
            emit(arm64::msr_nzcv(11));
        }
        emit_xmm_cache_reload(s.xmm_cache_live);
        const usize back = m_code_buf.size();
        emit(arm64::b_imm(
            cold_branch_disp(back, s.return_pos, 26, "mem stub return")));
    };

    // The sub-page decision of emit_subpage_entry, once per helper instead of
    // once per site. A site enters its helper from every guard branch, not
    // only the check-byte one, so the preconditions that branch implied are
    // tested here: the EA inside the hardware-protected window
    // (JitContext::linear64_window_bytes, 0 when there is none), no 16 KiB
    // host-page crossing, and a check byte that names a mixed page (the map
    // is only meaningful for one) -- and, for a store, no translated code or
    // write watch. Then every 4 KiB guest page the access touches must allow
    // it, and the access is exactly the one the fast path would have made.
    // Anything else falls through to the region probe and the checked helper.
    //
    // Entered with the EA in X9 and a store's value in X10; a load returns
    // its value in X0. X3/X8/X17 are scratch and NZCV is clobbered (every
    // site's helper path already destroys it). X30 is the site's return
    // address throughout, which is how the fault handler attributes a fault
    // of the direct access to the calling block (linear_fault.cpp); it is
    // parked in X3 across the access because the ordered-access patch
    // trampoline uses X30 as its retry PC.
    auto emit_shared_subpage = [&](const ColdMemStub& proto) {
        const int w = proto.width;
        if (!g_env_subpage_stub || !m_linear_base_pinned ||
            !(w == 1 || w == 2 || w == 4 || w == 8)) {
            return;
        }
        static_assert(kLinearCheckHoldsCode == 2u && kLinearCheckWatched == 4u,
                      "the store test below extracts bits 1-2");
        static_assert(kLinearCheckMixedProtBit == 0 && kLinearCheckMixedReadBit == 3,
                      "the mixed test below names bits 0 and 3");
        enum Kind : u8 { kBhs, kTbnz, kTbz, kCbnzW, kCbzX };
        struct Fix { usize pos; Kind kind; u8 reg; u8 bit; };
        std::vector<Fix> skips;
        const auto skip = [&](Kind kind, u8 reg = 0, u8 bit = 0) {
            skips.push_back({m_code_buf.size(), kind, reg, bit});
            emit(0);
        };
        emit(arm64::ldr_x_uoff(8, REG_JIT_CTX,
                               JIT_CTX_OFF_LINEAR64_WINDOW_BYTES >> 3));
        emit(arm64::subs_x(31, 9, 8));
        skip(kBhs);
        if (w > 1) {
            emit(arm64::add_x_imm(8, 9, static_cast<u32>(w - 1)));
            emit(arm64::eor_x(8, 8, 9));
            skip(kTbnz, 8, static_cast<u8>(kLinearHostPageShift));
        }
        emit(arm64::ldr_x_uoff(17, REG_JIT_CTX, JIT_CTX_OFF_LINEAR_CHECK_MAP >> 3));
        skip(kCbzX, 17);
        emit(arm64::lsr_x_imm(3, 9, static_cast<u8>(kLinearHostPageShift)));
        emit(arm64::ldrb_reg(3, 17, 3));
        if (proto.is_store) {
            emit(arm64::ubfx_w(8, 3, 1, 2));  // HoldsCode | Watched
            skip(kCbnzW, 8);
        }
        const usize mixed = m_code_buf.size();
        emit(0);  // TBNZ W3, #MixedProt, .map
        skip(kTbz, 3, kLinearCheckMixedReadBit);
        const usize map = m_code_buf.size();
        m_code_buf[mixed] = arm64::tbnz(3, kLinearCheckMixedProtBit,
                                        static_cast<i64>(map - mixed) * 4);
        emit(arm64::ldr_x_uoff(17, REG_JIT_CTX,
                               JIT_CTX_OFF_LINEAR_SUBPAGE_MAP >> 3));
        skip(kCbzX, 17);
        emit(arm64::lsr_x_imm(8, 9, static_cast<u8>(kLinearHostPageShift)));
        emit(arm64::ldrb_reg(8, 17, 8));
        if (proto.is_store) emit(arm64::lsr_w_imm(8, 8, 4));  // the write bits
        // Its first byte's guest page and, for a wider access, its last's.
        emit(arm64::ubfx_x(3, 9, static_cast<u8>(kLinearGuestPageShift), 2));
        emit(arm64::lsrv_w(3, 8, 3));
        skip(kTbz, 3, 0);
        if (w > 1) {
            emit(arm64::add_x_imm(17, 9, static_cast<u32>(w - 1)));
            emit(arm64::ubfx_x(17, 17, static_cast<u8>(kLinearGuestPageShift), 2));
            emit(arm64::lsrv_w(17, 8, 17));
            skip(kTbz, 17, 0);
        }
        emit(arm64::mov_x(3, 30));
        emit(arm64::add_x(17, REG_LINEAR_BASE, 9));
        if (proto.is_store) {
            emit_x64_tso_store(10, 17, 8, w);
        } else {
            emit_x64_tso_load(0, 17, 8, w);
        }
        emit(arm64::mov_x(30, 3));
        emit_shared_raise_flag();
        emit(arm64::ret());
        const usize out = m_code_buf.size();
        for (const Fix& f : skips) {
            const i64 bytes = static_cast<i64>(out - f.pos) * 4;
            switch (f.kind) {
            case kBhs: m_code_buf[f.pos] = arm64::b_cond(arm64::CS, bytes); break;
            case kTbnz: m_code_buf[f.pos] = arm64::tbnz(f.reg, f.bit, bytes); break;
            case kTbz: m_code_buf[f.pos] = arm64::tbz(f.reg, f.bit, bytes); break;
            case kCbnzW: m_code_buf[f.pos] = arm64::cbnz_w(f.reg, bytes); break;
            case kCbzX: m_code_buf[f.pos] = arm64::cbz_x(f.reg, bytes); break;
            }
        }
    };

    auto materialize_process_helper = [&](const ColdMemStub& proto) -> void* {
        ts_note_mem_helper(proto.helper_off, proto.width, proto.is_store);
        const ScalarMemHelperKey key = scalar_mem_helper_key(
            proto.helper_off, proto.width, proto.is_store, m_pins_live, m_block_is_64bit,
            static_cast<u8>(m_cfg.tso_mode), m_linear_base_pinned, m_cfg.linear64_jit_view);
        std::lock_guard<std::mutex> lock(g_scalar_mem_helper_mu());
        if (void* existing = lookup_scalar_mem_helper(key)) {
            m_jit_ctx.linear64_cold_mem[linear64_cold_mem_slot(
                proto.is_store, proto.width)] = existing;
            return existing;
        }

        SharedEmissionScope shared_scope(*this);
        std::vector<u32> saved;
        saved.swap(m_code_buf);
        const bool saved_esp = m_esp_frame_live;
        if (!jit_cold_stub_legacy() && m_block_is_64bit) {
            emit(arm64::str_x_uoff(8, REG_CPU_STATE, rip_offset() >> 3));
        }
        if (g_env_scalar_lr_eager)
            emit(arm64::str_x_uoff(30, REG_JIT_CTX, JIT_CTX_OFF_COLD_MEM_LR >> 3));
        if (!jit_cold_stub_legacy()) emit_shared_subpage(proto);
        emit_scalar_core(proto, 0, true);
        emit(arm64::ldr_x_uoff(30, REG_JIT_CTX, JIT_CTX_OFF_COLD_MEM_LR >> 3));
        if (!jit_cold_stub_legacy()) emit_shared_raise_flag();
        emit(arm64::ret());
        m_esp_frame_live = saved_esp;
        std::vector<u32> words = std::move(m_code_buf);
        m_code_buf = std::move(saved);

        const usize bytes = words.size() * sizeof(u32);
        if (g_scalar_mem_helper_page == nullptr && !map_helper_page()) {
            return nullptr;
        }
        const usize aligned = (g_scalar_mem_helper_used + 15u) & ~usize{15};
        if (aligned + bytes > kScalarMemHelperPage) return nullptr;
        void* dest = static_cast<u8*>(g_scalar_mem_helper_page) + aligned;
        pthread_jit_write_protect_np(0);
        std::memcpy(dest, words.data(), bytes);
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(dest, bytes);
        g_scalar_mem_helper_used = aligned + bytes;
        record_mem_helper(dest, words,
                          proto.is_store ? "scalar-cold-store" : "scalar-cold-load",
                          proto.width);
        g_scalar_mem_helpers.push_back({key, dest});
        g_mem_helper_generation.fetch_add(1, std::memory_order_release);
        m_jit_ctx.linear64_cold_mem[linear64_cold_mem_slot(
            proto.is_store, proto.width)] = dest;
        fill_live_jit_contexts_locked();
        return dest;
    };

    const usize n = m_cold_mem_stubs.size();
    std::vector<int> group(n, -1);
    std::vector<u64> group_keys;
    std::vector<int> group_rep;
    auto shareable = [](const ColdMemStub& s) {
        return !s.is_vec128 && !s.is_vec256;
    };
    auto make_key = [&](const ColdMemStub& s) -> u64 {
        const bool try_region = s.single_upper_branch && !m_cfg.no_jitcache;
        return (u64{s.helper_off} << 32) |
               (u64{static_cast<u32>(s.width)} << 16) |
               (s.is_store ? 1ull : 0) | (try_region ? 2ull : 0);
    };
    for (usize i = 0; i < n; ++i) {
        const ColdMemStub& s = m_cold_mem_stubs[i];
        if (!shareable(s)) continue;
        const u64 key = make_key(s);
        int g = -1;
        for (int gi = 0; gi < static_cast<int>(group_keys.size()); ++gi) {
            if (group_keys[static_cast<usize>(gi)] == key) {
                g = gi;
                break;
            }
        }
        if (g < 0) {
            g = static_cast<int>(group_keys.size());
            group_keys.push_back(key);
            group_rep.push_back(static_cast<int>(i));
        }
        group[i] = g;
    }
    std::vector<int> group_count(group_keys.size(), 0);
    for (int g : group) {
        if (g >= 0) ++group_count[static_cast<usize>(g)];
    }

    // Helper ABI: EA in X9, store value in X10. A site may leave either in
    // any register outside the guard scratches, including each other's
    // target, so the copy is a parallel move: value out of X9 before X9 is
    // overwritten, and the X9<->X10 swap through X17 (dead on every cold edge:
    // it held the last-byte address or the host address).
    auto emit_store_operands_to_abi = [&](const ColdMemStub& s) {
        const u8 a = s.addr_arm;
        const u8 v = s.store_arm;
        if (a == 10 && v == 9) {
            emit(arm64::mov_x(17, 9));
            emit(arm64::mov_x(9, 10));
            emit(arm64::mov_x(10, 17));
            return;
        }
        if (v == 9) {
            emit(arm64::mov_x(10, 9));
            if (a != 9) emit(arm64::mov_x(9, a));
            return;
        }
        if (a != 9) emit(arm64::mov_x(9, a));
        if (v != 10) emit(arm64::mov_x(10, v));
    };

    using FP = ColdTailFootprint;
    usize fp_mark = m_code_buf.size();
    const auto fp_note = [&](FP::Piece piece) {
        if (!m_cold_lazy_late) {
            note_cold_tail_words(piece, m_code_buf.size() - fp_mark);
        }
        fp_mark = m_code_buf.size();
    };
    if (!m_cold_lazy_late) {
        note_cold_tail_words(FP::MemSites, m_cold_mem_stubs.size());
    }

    // A lazy site (cold_lazy_enabled): its slow path is a pure function of
    // the record below and the block's configuration, so it is emitted on
    // first take by cold_lazy_emit -- through this same flush -- instead of
    // here. Only the shared-ABI trampoline form qualifies, and only when
    // its owning instruction's interval names it (the materialized stub
    // takes that instruction's published RIP and flag-recovery descriptor).
    constexpr usize npos = static_cast<usize>(-1);
    usize lazy_entry = npos;
    auto direct_owner = [&](usize pos) {
        for (u32 i = 0; i < m_insn_pos_count; ++i) {
            if (pos >= m_insn_arm_pos[i] && pos < m_insn_arm_end[i]) return true;
        }
        return false;
    };
    auto fits_i16 = [](i64 words) { return words > -32768 && words < 32768; };
    auto lazy_eligible = [&](const ColdMemStub& s) {
        if (m_cold_eager_variant || m_cold_lazy_late || !cold_lazy_enabled() ||
            jit_cold_stub_legacy() ||
            !m_cfg.linear64 || !m_block_is_64bit || s.is_vec128 ||
            s.is_vec256 || !s.single_upper_branch) {
            return false;
        }
        if (!(s.width == 1 || s.width == 2 || s.width == 4 || s.width == 8) ||
            s.helper_off != helper_off_for_width(s.is_store, s.width)) {
            return false;
        }
        // The record holds hot offsets as signed 16-bit word deltas; a block
        // this size, placed contiguously or in one split window, keeps every
        // one of them in range.
        if (m_code_buf.size() >= 16000) return false;
        if (s.addr_arm > 31 || s.store_arm > 31 || s.dst_arm > 31 ||
            m_cfg.linear64_address_bits >= 64) {
            return false;
        }
        if (s.lean_variant) {
            if (s.lean_window_pos == npos || s.above_branch_pos != s.lean_window_pos) {
                return false;
            }
        } else if (s.lean_window_pos != npos) {
            return false;
        }
        if (s.above_branch_pos >= s.return_pos || !direct_owner(s.above_branch_pos) ||
            !fits_i16(static_cast<i64>(s.return_pos - s.above_branch_pos))) {
            return false;
        }
        if (s.mixed_page_branch_pos != npos &&
            (s.mixed_page_branch_pos >= s.return_pos ||
             !fits_i16(static_cast<i64>(s.return_pos - s.mixed_page_branch_pos)))) {
            return false;
        }
        return true;
    };
    auto emit_lazy_slot = [&](const ColdMemStub& s) {
        const usize mark = m_code_buf.size();
        if (lazy_entry == npos) {
            lazy_entry = m_code_buf.size();
            emit(arm64::ldr_x_uoff(16, REG_JIT_CTX, JIT_CTX_OFF_COLD_LAZY_ENTRY >> 3));
            emit(0xD61F0200u);  // BR X16
        }
        const usize slot = m_code_buf.size();
        emit(arm64::bl_imm(cold_branch_disp(slot, lazy_entry, 26, "lazy cold entry")));
        // The record: written after placement (cold_lazy_write_records).
        // Zero (UDF) until then, so the split relocator sees no branch.
        for (u32 k = 0; k < kColdLazyRecordWords; ++k) emit(0);
        if (s.lean_variant) {
            m_code_buf[s.lean_window_pos] = arm64::cbnz_x(
                s.lean_window_is_locator ? u8{31} : u8{16},
                cold_branch_disp(s.lean_window_pos, slot, 19, "lean site variant"));
        } else {
            patch_branches(s, slot);
        }
        // The raise entry the stub will join: what emit_site_tail decides.
        const bool restore = s.spill_nzcv || s.restore_nzcv_on_raise ||
                             m_flags.nzcv_hold_depth != 0;
        u8 kind = s.nzcv_kind_on_raise;
        if (restore && kind == 0xFF) {
            const NzcvState state = m_flags.nzcv != NzcvState::None
                ? m_flags.nzcv : m_flags.incoming_nzcv;
            kind = nzcv_kind_byte(state);
        }
        if (restore) {
            m_raise_lazy_restore[kind] = true;
        } else {
            m_raise_lazy_plain = true;
        }
        ColdLazySite site{};
        site.slot_pos = slot;
        site.return_pos = s.return_pos;
        site.mixed_pos = s.mixed_page_branch_pos;
        site.guard_pos = s.above_branch_pos;
        site.raise_kind = restore ? kind : kColdLazyPlainRaise;
        using R = ColdLazyRecord;
        site.packed = static_cast<u32>(s.width) |
                      (s.is_store ? R::kStore : 0u) |
                      (s.spill_nzcv ? R::kSpillNzcv : 0u) |
                      (restore && !s.spill_nzcv ? R::kRestoreNzcv : 0u) |
                      (s.range_variant ? R::kRangeVariant : 0u) |
                      (s.tso_crossing_variant ? R::kCrossingVariant : 0u) |
                      (s.lean_variant ? R::kLeanVariant : 0u) |
                      (s.lean_window_is_locator ? R::kLeanLocator : 0u) |
                      (u32{s.addr_arm} << R::kAddrShift) |
                      (u32{s.store_arm} << R::kStoreRegShift) |
                      (u32{s.dst_arm} << R::kDstShift) |
                      (u32{restore ? kind : u8{0}} << R::kKindShift) |
                      (m_pins_live ? R::kPinsLive : 0u) |
                      (m_linear_base_pinned ? R::kLinearBasePinned : 0u) |
                      (m_cfg.linear64_jit_view ? R::kJitView : 0u);
        site.config = u32{s.xmm_cache_live} |
                      (static_cast<u32>(m_cfg.tso_mode) & 3u) << 16 |
                      (u32{m_cfg.linear64_address_bits} & 63u) << 18;
        m_cold_lazy_sites.push_back(site);
        note_cold_tail_words(FP::MemLazy, m_code_buf.size() - mark);
        fp_mark = m_code_buf.size();
    };

    auto emit_trampoline = [&](ColdMemStub s, void* process_helper, u32 ordinal) {
        fp_mark = m_code_buf.size();
        const LeanVariant lean = emit_lean_variant(s);
        fp_note(FP::MemLean);
        const usize outside = emit_range_variant(s);
        fp_note(FP::MemRange);
        const usize crossing = emit_crossing_variant(crossing_view(s, lean));
        fp_note(FP::MemCrossing);
        // Shared ABI: the helper decides the sub-page case itself
        // (emit_shared_subpage), publishes the RIP the site leaves in X8,
        // and returns the raise flag in W16.
        const bool shared_abi = !jit_cold_stub_legacy();
        const usize sub_entry = shared_abi ? static_cast<usize>(-1)
                                           : emit_subpage_entry(s);
        fp_note(FP::MemSubpage);
        const usize dest = m_code_buf.size();
        emit_cold_entry_counter(ordinal);
        emit_rip_and_nzcv(s, shared_abi);
        if (s.is_store) {
            emit_store_operands_to_abi(s);
        } else {
            emit_ea_to_x9(s);
        }
        fp_note(FP::MemEntry);
        if (!m_cold_lazy_late) {
            patch_branches(s, dest, sub_entry);
        } else if (s.lean_variant) {
            // Late emission leaves the hot site alone, but V's own
            // check-byte branch (emit_lean_variant) is part of the stub.
            ColdMemStub own{};
            own.is_store = s.is_store;
            own.mixed_page_branch_pos = s.mixed_page_branch_pos;
            patch_branches(own, dest, sub_entry);
        }
        patch_crossing_variant(crossing, dest);
        patch_range_variant(outside, dest);
        patch_lean_variant(lean, dest);
        if (process_helper != nullptr) {
            if (!m_pins_live) {
                // Pinless helpers have a different marshal ABI. A consumer
                // context's shared slots describe the pinned ABI, so retain
                // this exact immutable helper rather than loading that slot.
                note_unrelocatable_host_bake();
                emit_mov_imm64(16, reinterpret_cast<u64>(process_helper));
            } else {
                emit(arm64::ldr_x_uoff(
                    16, REG_JIT_CTX,
                    (JIT_CTX_OFF_LINEAR64_COLD_MEM >> 3) +
                        linear64_cold_mem_slot(s.is_store, s.width)));
            }
            emit(0xD63F0200u);  // BLR X16
            emit_probe_poison_vregs();
        }
        fp_note(FP::MemCall);
        emit_site_tail(s, shared_abi);
        fp_note(FP::MemTail);
        return dest;
    };

    for (usize i = 0; i < n; ++i) {
        ColdMemStub s = m_cold_mem_stubs[i];
        const bool try_region = s.single_upper_branch && !m_cfg.no_jitcache;
        if (s.is_vec128 || s.is_vec256) {
            fp_mark = m_code_buf.size();
            const LeanVariant lean = emit_lean_variant(s);
            fp_note(FP::MemLean);
            const usize outside = emit_range_variant(s);
            fp_note(FP::MemRange);
            const usize sub_entry = emit_subpage_entry(s);
            fp_note(FP::MemSubpage);
            const usize dest = m_code_buf.size();
            emit_cold_entry_counter(m_cold_mem_ordinal_base + static_cast<u32>(i));
            emit_rip_and_nzcv(s);
            patch_branches(s, dest, sub_entry);
            patch_range_variant(outside, dest);
            patch_lean_variant(lean, dest);
            // The bodies take the EA in X9 (the inline 128-bit guard may have
            // left it in the register the lowering formed it in).
            if (s.addr_arm != 9) emit(arm64::mov_x(9, s.addr_arm));
            if (s.is_vec256) emit_cold_mem_stub_vec256(s);
            else emit_cold_mem_stub_vec128(s);
            if (s.spill_nzcv) {
                emit(arm64::ldr_x_uoff(11, 31, 1));
                emit(arm64::msr_nzcv(11));
            }
            emit_xmm_cache_reload(s.xmm_cache_live);
            const usize back128 = m_code_buf.size();
            emit(arm64::b_imm(cold_branch_disp(back128, s.return_pos, 26,
                                               "mem stub return")));
            fp_note(FP::MemVector);
            continue;
        }
        if (try_region) {
            void* helper = materialize_process_helper(s);
            if (helper != nullptr) {
                if (lazy_eligible(s)) {
                    emit_lazy_slot(s);
                    continue;
                }
                (void)emit_trampoline(
                    s, helper, m_cold_mem_ordinal_base + static_cast<u32>(i));
                continue;
            }
        }
        if (group[i] >= 0 &&
            group_count[static_cast<usize>(group[i])] >= 2) {
            continue;
        }
        fp_mark = m_code_buf.size();
        const LeanVariant lean = emit_lean_variant(s);
        fp_note(FP::MemLean);
        const usize outside = emit_range_variant(s);
        fp_note(FP::MemRange);
        const usize crossing = emit_crossing_variant(crossing_view(s, lean));
        fp_note(FP::MemCrossing);
        const usize sub_entry = emit_subpage_entry(s);
        fp_note(FP::MemSubpage);
        const usize dest = m_code_buf.size();
        emit_cold_entry_counter(m_cold_mem_ordinal_base + static_cast<u32>(i));
        emit_rip_and_nzcv(s);
        // The core reads the EA from X9 and the value from store_arm, so a
        // store's operands go through the same parallel move as the
        // trampoline and the core is told where they now are.
        ColdMemStub core = s;
        if (s.is_store) {
            emit_store_operands_to_abi(s);
            core.addr_arm = 9;
            core.store_arm = 10;
        } else {
            emit_ea_to_x9(s);
        }
        patch_branches(s, dest, sub_entry);
        patch_crossing_variant(crossing, dest);
        patch_range_variant(outside, dest);
        patch_lean_variant(lean, dest);
        fp_note(FP::MemEntry);
        emit_scalar_core(core, s.dst_arm, false);
        fp_note(FP::MemCore);
    }

    // Region-path (no try_region) same-kind accesses still share one body
    // inside the block. linear64 try_region sites already left above.
    for (int g = 0; g < static_cast<int>(group_keys.size()); ++g) {
        if (group_count[static_cast<usize>(g)] < 2) continue;
        const ColdMemStub& proto =
            m_cold_mem_stubs[static_cast<usize>(group_rep[static_cast<usize>(g)])];
        if (proto.single_upper_branch && !m_cfg.no_jitcache) continue;
        std::vector<usize> bl_pos;
        for (usize i = 0; i < n; ++i) {
            if (group[i] != g) continue;
            ColdMemStub s = m_cold_mem_stubs[i];
            fp_mark = m_code_buf.size();
            const LeanVariant lean = emit_lean_variant(s);
            fp_note(FP::MemLean);
            const usize outside = emit_range_variant(s);
            fp_note(FP::MemRange);
            const usize crossing = emit_crossing_variant(crossing_view(s, lean));
            fp_note(FP::MemCrossing);
            const usize sub_entry = emit_subpage_entry(s);
            fp_note(FP::MemSubpage);
            const usize dest = m_code_buf.size();
            emit_cold_entry_counter(m_cold_mem_ordinal_base + static_cast<u32>(i));
            emit_rip_and_nzcv(s);
            if (s.is_store) {
                emit_store_operands_to_abi(s);
            } else {
                emit_ea_to_x9(s);
            }
            patch_branches(s, dest, sub_entry);
            patch_crossing_variant(crossing, dest);
            patch_range_variant(outside, dest);
            patch_lean_variant(lean, dest);
            fp_note(FP::MemEntry);
            bl_pos.push_back(m_code_buf.size());
            emit(0);
            fp_note(FP::MemCall);
            emit_site_tail(s);
            fp_note(FP::MemTail);
        }
        const usize shared = m_code_buf.size();
        if (g_env_scalar_lr_eager)
            emit(arm64::str_x_uoff(30, REG_JIT_CTX, JIT_CTX_OFF_COLD_MEM_LR >> 3));
        emit_scalar_core(proto, 0, true);
        emit(arm64::ldr_x_uoff(30, REG_JIT_CTX, JIT_CTX_OFF_COLD_MEM_LR >> 3));
        emit(arm64::ret());
        note_cold_tail_words(FP::MemSharedBody, m_code_buf.size() - shared);
        for (usize p : bl_pos) {
            m_code_buf[p] = arm64::bl_imm(
                cold_branch_disp(p, shared, 26, "shared mem helper"));
        }
    }
    m_cold_mem_stubs.clear();
}

}  // namespace vitreo::cpu::jit

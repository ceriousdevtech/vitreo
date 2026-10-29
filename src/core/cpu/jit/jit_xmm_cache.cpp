// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file jit_xmm_cache.cpp
/// @brief The JIT XMM register cache
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

namespace {
// VITREO_JIT_XMM_CACHE_STATS=1: MEASUREMENT PROBE, not an optimisation.
// Compile-time, unweighted: per opcode, how often an instruction was
// cache-aware, and how many live cache entries a non-aware one dropped
// (helper call / VEX / written operand). Names the next lowering to make
// cache-aware. Dumped to stderr every 2^16 noted vector instructions.
struct XmmCacheStats {
    std::atomic<u64> aware[opcode_count()]{};
    std::atomic<u64> dropped[opcode_count()]{};
    std::atomic<u64> clobber[opcode_count()]{};
    std::atomic<u64> noted{0};
    std::atomic<u64> source_hit{0};   // xmm_cache_source found it cached
    std::atomic<u64> source_miss{0};  // ... and had to load it
};
XmmCacheStats* xmm_cache_stats() {
    static XmmCacheStats* const stats =
        std::getenv("VITREO_JIT_XMM_CACHE_STATS") != nullptr
            ? new XmmCacheStats()
            : nullptr;
    return stats;
}
void xmm_cache_stats_dump(XmmCacheStats& st) {
    struct Row { u64 key; usize op; };
    std::vector<Row> rows;
    for (usize op = 0; op < opcode_count(); ++op) {
        const u64 a = st.aware[op].load(std::memory_order_relaxed);
        const u64 d = st.dropped[op].load(std::memory_order_relaxed);
        const u64 c = st.clobber[op].load(std::memory_order_relaxed);
        if (a + d + c != 0) rows.push_back({d + c, op});
    }
    std::sort(rows.begin(), rows.end(),
              [](const Row& x, const Row& y) { return x.key > y.key; });
    std::fprintf(stderr,
                 "[jit-xmm-stats] noted=%llu source hit=%llu miss=%llu "
                 "top droppers:\n",
                 static_cast<unsigned long long>(st.noted.load()),
                 static_cast<unsigned long long>(st.source_hit.load()),
                 static_cast<unsigned long long>(st.source_miss.load()));
    for (usize i = 0; i < rows.size() && i < 40; ++i) {
        const usize op = rows[i].op;
        std::fprintf(stderr,
                     "[jit-xmm-stats]   %-28s aware=%llu dropped=%llu "
                     "clobbered=%llu\n",
                     opcode_name(static_cast<Opcode>(op)),
                     static_cast<unsigned long long>(st.aware[op].load()),
                     static_cast<unsigned long long>(st.dropped[op].load()),
                     static_cast<unsigned long long>(st.clobber[op].load()));
    }
}
}  // namespace

bool JitCompiler::xmm_cache_on() const noexcept {
    // The store-ring recorder (a diagnostic) calls C through a trampoline
    // on the hot path of every store, which may clobber V16-V31.
    return m_cfg.xmm_cache && m_block_is_64bit && !m_ir_emit_active &&
           !m_vec_lane_split && !m_vec_hi_lane && !m_current_original_vex &&
           !m_flushing_cold_arms && m_hot_end_pos == 0 && !m_store_ring_stores;
}

u8 JitCompiler::xmm_cache_source(u8 xmm) {
    const u8 v = static_cast<u8>(kXmmCacheBase + xmm);
    if (XmmCacheStats* st = xmm_cache_stats(); st != nullptr) {
        ((m_xmm_cache & (1u << xmm)) != 0 ? st->source_hit : st->source_miss)
            .fetch_add(1, std::memory_order_relaxed);
    }
    if ((m_xmm_cache & (1u << xmm)) == 0) {
        emit(arm64::ldr_q_uoff(v, REG_CPU_STATE, xmm_u64_off(xmm, 0) >> 4));
        m_xmm_cache = static_cast<u16>(m_xmm_cache | (1u << xmm));
    }
    return v;
}

void JitCompiler::emit_xmm_cache_reload(u16 mask) {
    AttrScope attr_scope(*this, EmitAttribution::GprState);
    for (u8 n = 0; n < kXmmCacheCount; ++n) {
        if ((mask & (1u << n)) == 0) continue;
        emit(arm64::ldr_q_uoff(static_cast<u8>(kXmmCacheBase + n),
                               REG_CPU_STATE, xmm_u64_off(n, 0) >> 4));
    }
}


void JitCompiler::xmm_cache_note_instruction(
        const DecodedInstruction& instr) noexcept {
    if (XmmCacheStats* st = xmm_cache_stats(); st != nullptr &&
        static_cast<usize>(instr.opcode) < opcode_count()) {
        const usize op = static_cast<usize>(instr.opcode);
        if (m_xmm_cache_aware && !m_vregs_clobbered) {
            st->aware[op].fetch_add(1, std::memory_order_relaxed);
        }
        if (m_xmm_cache_aware || m_vregs_clobbered || instr.prefixes.vex) {
            if ((st->noted.fetch_add(1, std::memory_order_relaxed) &
                 ((1u << 16) - 1)) == 0) {
                xmm_cache_stats_dump(*st);
            }
        }
        if (m_vregs_clobbered && m_xmm_cache != 0) {
            st->clobber[op].fetch_add(
                static_cast<u64>(__builtin_popcount(m_xmm_cache)),
                std::memory_order_relaxed);
        }
    }
    if (m_xmm_cache == 0) return;
    if (m_vregs_clobbered) {
        m_xmm_cache = 0;
        return;
    }
    if (m_xmm_cache_aware) return;
    const u16 before = m_xmm_cache;
    xmm_cache_drop_written(instr);
    if (XmmCacheStats* st = xmm_cache_stats(); st != nullptr &&
        before != m_xmm_cache &&
        static_cast<usize>(instr.opcode) < opcode_count()) {
        st->dropped[static_cast<usize>(instr.opcode)].fetch_add(
            static_cast<u64>(__builtin_popcount(before & ~m_xmm_cache)),
            std::memory_order_relaxed);
    }
}

void JitCompiler::xmm_cache_drop_written(
        const DecodedInstruction& instr) noexcept {
    m_xmm_cache = static_cast<u16>(m_xmm_cache & ~xmm_cache_written_mask(instr));
}

u16 JitCompiler::xmm_cache_written_mask(
        const DecodedInstruction& instr) noexcept {
    // VEX and EVEX forms can write a register named outside the operand list
    // (VEX.vvvv, the is4 immediate), and the state-restore instructions name
    // none: all of them.
    if (instr.prefixes.vex || instr.prefixes.evex) return 0xFFFF;
    switch (instr.opcode) {
    case Opcode::FXRSTOR_M512:
    case Opcode::XRSTOR_M:
    case Opcode::VZEROALL:
        return 0xFFFF;
    default:
        break;
    }
    // A legacy-encoded instruction writes at most its first operand (Intel
    // SDM Vol. 2 operand order: destination first) -- none for the
    // read-only compares below -- plus XMM0 for the two string compares
    // whose mask result is implicit (PCMPESTRM/PCMPISTRM).
    // IsaMatrixExec.LegacyFormsWriteOnlyTheXmmsTheCacheDrops checks this
    // against the interpreter on every executable form.
    u16 mask = 0;
    // The compares that only set EFLAGS (COMISS/UCOMISS/COMISD/UCOMISD,
    // PTEST) and the index forms of the string compares (PCMPESTRI/
    // PCMPISTRI write ECX) read their first operand; they write no XMM.
    const bool reads_only = sse_writes_only_flags(instr.opcode) ||
        instr.opcode == Opcode::PCMPESTRI_XMM_XMMM128_IMM8 ||
        instr.opcode == Opcode::PCMPISTRI_XMM_XMMM128_IMM8;
    if (!reads_only && instr.operand_count > 0 &&
        instr.operands[0].type == OperandType::Reg128) {
        mask = static_cast<u16>(1u << (instr.operands[0].reg & 15));
    }
    if (instr.opcode == Opcode::PCMPESTRM_XMM_XMMM128_IMM8 ||
        instr.opcode == Opcode::PCMPISTRM_XMM_XMMM128_IMM8) {
        mask = static_cast<u16>(mask | 1u);
    }
    return mask;
}

u32 JitCompiler::xmm_rebind_v01(u32 enc, u8 d, u8 v0, u8 v1) noexcept {
    const u8 rd = static_cast<u8>(enc & 31u);
    const u8 rn = static_cast<u8>((enc >> 5) & 31u);
    const u8 rm = static_cast<u8>((enc >> 16) & 31u);
    assert(rd == 0 && rn <= 1 && rm <= 1 &&
           "xmm_rebind_v01 expects a three-register NEON word on V0/V1");
    (void)rd;
    const auto map = [&](u8 r) -> u32 { return r == 0 ? v0 : v1; };
    return (enc & ~(0x1Fu | (0x1Fu << 5) | (0x1Fu << 16))) |
           static_cast<u32>(d) | (map(rn) << 5) | (map(rm) << 16);
}

bool JitCompiler::xmm_cache_rr(u32 enc, u8 d, u8 s1, u8 s2) {
    if (!xmm_cache_fits(d, s1, s2)) return false;
    const u8 v1 = xmm_cache_source(s1);
    const u8 v2 = xmm_cache_source(s2);
    const u8 vd = static_cast<u8>(kXmmCacheBase + d);
    emit(xmm_rebind_v01(enc, vd, v1, v2));
    emit(arm64::str_q_uoff(vd, REG_CPU_STATE, xmm_u64_off(d, 0) >> 4));
    xmm_cache_publish(d);
    m_xmm_cache_aware = true;
    return true;
}

void JitCompiler::xmm_cache_rm(u32 enc, u8 d, u8 s1) {
    assert(xmm_cache_fits(d, s1));
    const u8 v1 = xmm_cache_source(s1);
    const u8 vd = static_cast<u8>(kXmmCacheBase + d);
    emit(xmm_rebind_v01(enc, vd, v1, 1));
    emit(arm64::str_q_uoff(vd, REG_CPU_STATE, xmm_u64_off(d, 0) >> 4));
    xmm_cache_publish(d);
    m_xmm_cache_aware = true;
}

bool JitCompiler::emit_xmm_cache_copy(u8 d, u8 s) {
    if (!xmm_cache_fits(d, s)) return false;
    const u8 vd = static_cast<u8>(kXmmCacheBase + d);
    if ((m_xmm_cache & (1u << s)) != 0) {
        if (s != d) {
            emit(arm64::mov_v16b(vd, static_cast<u8>(kXmmCacheBase + s)));
        }
    } else {
        emit(arm64::ldr_q_uoff(vd, REG_CPU_STATE, xmm_u64_off(s, 0) >> 4));
    }
    emit(arm64::str_q_uoff(vd, REG_CPU_STATE, xmm_u64_off(d, 0) >> 4));
    xmm_cache_publish(d);
    m_xmm_cache_aware = true;
    return true;
}

bool JitCompiler::emit_xmm_cache_binop(const DecodedInstruction& instr,
                                       const Operand& rm, u64 next_rip,
                                       u32 enc, u8 d, u8 s1,
                                       bool xor_zero_idiom) {
    if (is_register_operand(rm.type)) {
        const u8 s2 = static_cast<u8>(rm.reg & 15);
        if (!xmm_cache_fits(d, s1, s2)) return false;
        if (xor_zero_idiom && s2 == s1) {
            // A register XORed with itself is zero whatever it holds (Intel
            // SDM Vol. 2 PXOR/XORPS/XORPD), so the idiom reads no source.
            const u8 vd = static_cast<u8>(kXmmCacheBase + d);
            emit(arm64::eor_v16b(vd, vd, vd));
            emit(arm64::str_q_uoff(vd, REG_CPU_STATE, xmm_u64_off(d, 0) >> 4));
            xmm_cache_publish(d);
            m_xmm_cache_aware = true;
            return true;
        }
        return xmm_cache_rr(enc, d, s1, s2);
    }
    if (!is_memory_operand(rm.type) || !xmm_cache_fits(d, s1) ||
        !m_block_is_64bit || !m_cfg.linear64 || !m_cfg.linear64_vec128 ||
        !m_linear_base_pinned) {
        return false;
    }
    // Legacy SSE needs a 16-byte-aligned m128 on these forms, measured on
    // silicon (tools/oracle_probe/sse_align_probe.c); VEX forms do not.
    // The VEX.128 bridge clears prefixes.vex, hence m_current_original_vex.
    const bool aligned = !instr.prefixes.vex && !m_current_original_vex;
    const u8 ea = emit_ea64(REG_SCRATCH2, rm.mem, next_rip);
    if (aligned) emit_align_check(ea, false);
    emit_linear64_load128(1, ea, aligned);
    xmm_cache_rm(enc, d, s1);
    return true;
}

bool JitCompiler::emit_xmm_cache_store_lane0(u8 d, u8 s, bool qword) {
    if (!xmm_cache_fits(d, s) || (m_xmm_cache & (1u << s)) == 0) return false;
    const u8 vs = static_cast<u8>(kXmmCacheBase + s);
    const u32 off_d = xmm_u64_off(d, 0);
    emit(qword ? arm64::str_d_uoff(vs, REG_CPU_STATE, off_d >> 3)
               : arm64::str_s_uoff(vs, REG_CPU_STATE, off_d >> 2));
    if (d != s) m_xmm_cache = static_cast<u16>(m_xmm_cache & ~(1u << d));
    m_xmm_cache_aware = true;
    return true;
}

void JitCompiler::push_cold_mem_stub(ColdMemStub stub) {
    u16 live = m_xmm_cache;
    if ((stub.is_vec128 || stub.is_vec256) && !stub.is_store &&
        stub.dst_arm >= kXmmCacheBase &&
        stub.dst_arm < kXmmCacheBase + kXmmCacheCount) {
        // The stub's own destination is what it is loading: never reload
        // the stale CpuState copy over it.
        live = static_cast<u16>(live & ~(1u << (stub.dst_arm - kXmmCacheBase)));
    }
    stub.xmm_cache_live = live;
    m_cold_mem_stubs.push_back(stub);
}

void JitCompiler::emit_probe_poison_vregs() {
    if (!m_cfg.probe_poison_vregs) return;
    for (u8 v = kXmmCacheBase; v < kXmmCacheBase + kXmmCacheCount; ++v) {
        emit(arm64::movi_v16b(v, 0xA5));
    }
}

} // namespace vitreo::cpu::jit

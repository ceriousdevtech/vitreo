// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom bulk helper. See loop_idiom_runtime.hpp.
//
// Exactness is by construction: the helper runs m whole iterations where
// iterations 1..m+1 are all known to continue and every element they touch
// is accessible, then returns to the loop head. The translated body then
// runs at least one full iteration and the exit (or faulting) iteration, so
// every temp register, XMM register, flag, the exit RIP and any exception
// (Intel SDM Vol. 3 6.15 #PF/#GP; STATUS_ACCESS_VIOLATION and
// STATUS_GUARD_PAGE_VIOLATION records) come from the ordinary path. The
// helper itself never raises.

#include "core/cpu/jit/loop_idiom_runtime.hpp"

#include "core/cpu/jit/jit_helpers.hpp"
#include "core/cpu/jit/loop_idiom_desc.hpp"
#include "core/cpu/jit/loop_idiom_gate.hpp"
#include "core/cpu/jit/loop_idiom_mem.hpp"
#include "core/cpu/jit/loop_idiom_thresholds.hpp"
#include "core/cpu/jit/loop_idiom_trip.hpp"
#include "core/memory/mem_watch.hpp"
#include "core/memory/write_watch_traps.hpp"

#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <cstring>

namespace vitreo::cpu::jit {

namespace {

using namespace loop_idiom;

u64 reg_value(const CpuState* cpu, u8 reg, bool is64) {
    return is64 ? cpu->gpr.get64(reg) : cpu->gpr.get32(reg);
}

/// Value bytes of an invariant operand, little-endian, 16 bytes.
void value_bytes(const CpuState* cpu, ValueSrc src, u8 reg, bool high8, i32 imm, u8 out[16]) {
    std::memset(out, 0, 16);
    switch (src) {
    case ValueSrc::Imm: {
        const i64 v = imm;
        std::memcpy(out, &v, 8);
        std::memset(out + 8, v < 0 ? 0xFF : 0x00, 8);
        return;
    }
    case ValueSrc::Gpr: {
        u64 v = cpu->gpr.get64(reg);
        if (high8) v = (v >> 8) & 0xFF;
        std::memcpy(out, &v, 8);
        return;
    }
    case ValueSrc::Xmm:
        std::memcpy(out, cpu->sse.xmm[reg & 15].u8_, 16);
        return;
    case ValueSrc::None:
        return;
    }
}

ElemPred make_pred(const CpuState* cpu, const DataExit& x, u32 w) {
    ElemPred p;
    p.op = x.op;
    p.exit_if_eq = x.exit_if_eq;
    if (x.op == DataOp::AnyLaneEq) {
        p.lane = 1u << x.cmp_log2;
        value_bytes(cpu, ValueSrc::Xmm, x.val_reg, false, 0, p.value);
        return p;
    }
    if (x.op == DataOp::StreamEq) return p;
    const u32 cw = 1u << x.cmp_log2;
    u8 v[16];
    value_bytes(cpu, x.val, x.val_reg, x.val_high8, x.val_imm, v);
    if (x.val == ValueSrc::Gpr && x.val_high8) {
        // AH..BH is one byte wide.
        std::memset(v + 1, 0, 15);
    }
    p.cmp_bytes = std::min(cw, w);
    std::memcpy(p.value, v, p.cmp_bytes);
    // The element is zero-extended to the compare width (MOVZX temp): it can
    // only equal a value whose bytes above the element are zero.
    for (u32 i = w; i < cw; ++i) {
        if (v[i] != 0) p.never_eq = true;
    }
    return p;
}

/// Decoded descriptors, per thread, direct-mapped by the six words. The
/// words are compile-time constants of one block, so a hot loop hits on
/// every call; a miss decodes (unpack) and replaces the entry. Equality is
/// on all six words, so a hit is exactly the descriptor unpack would return.
struct DescMemo {
    Words words{};
    Desc desc{};
    bool valid{false};
};
constexpr u32 kDescMemoSlots = 8;
thread_local DescMemo t_desc_memo[kDescMemoSlots];

const Desc* lookup_desc(const Words& w) {
    u64 h = w[0] ^ w[1] ^ w[2] ^ w[3] ^ w[4] ^ w[5];
    h ^= h >> 29;
    h *= 0x9E3779B97F4A7C15ull;
    DescMemo& e = t_desc_memo[h >> 61];
    if (e.valid && e.words == w) return &e.desc;
    const std::optional<Desc> d = unpack(w);
    if (!d) return nullptr;
    e.words = w;
    e.desc = *d;
    e.valid = true;
    return &e.desc;
}

}  // namespace

extern "C" u32 jit_loop_idiom(CpuState* cpu, GuestMemory* mem, u64 w0, u64 w1,
                              u64 w2, u64 w3, u64 w4, u64 w5) {
    if (cpu == nullptr || mem == nullptr) return 0;
    const Desc* od = lookup_desc(Words{w0, w1, w2, w3, w4, w5});
    if (od == nullptr) return 0;
    const Desc& d = *od;
    const bool is64 = d.is64;
    const u64 amask = is64 ? ~u64{0} : 0xFFFFFFFFull;
    const u32 w = d.elem_bytes();
    // Per-call budget (C15): at most max_iters_per_call(w) iterations, so
    // the dispatcher still runs between bounded host bursts.
    const u64 limit = max_iters_per_call(w);

    // 1. Induction exit: closed form.
    u64 W = limit;
    u64 n_exit = ~u64{0};  // first exiting iteration, if known
    if (d.has_ind_exit) {
        const IndExit& e = d.ind_exit;
        if (e.ind >= d.n_ind) return 0;
        const Induction& ind = d.ind[e.ind];
        const u32 width = e.cmp64 ? 64 : 32;
        const u64 r0 = reg_value(cpu, ind.reg, ind.wide);
        const u64 v1 = r0 + static_cast<u64>(static_cast<i64>(e.phase) * ind.step);
        u64 bound;
        if (e.bound == ValueSrc::Imm) {
            bound = static_cast<u64>(static_cast<i64>(e.bound_imm));
        } else {
            bound = reg_value(cpu, e.bound_reg, e.cmp64);
        }
        const TripResult t = first_induction_exit(e.cont, e.ind_is_lhs, v1, ind.step,
                                                  width, bound, limit);
        if (t.kind == TripResult::Kind::Exit) {
            n_exit = t.k;
            W = std::min(W, t.k);
        } else {
            W = std::min(W, t.k);
        }
    }
    if (W < 2) return 0;

    // 2. Streams: element 1 addresses and the accessible prefix.
    const u8 ns = d.n_streams();
    u64 addr1[3]{};
    Span span[3]{};
    for (u8 s = 0; s < ns; ++s) {
        const Stream& st = d.streams[s];
        u64 a = static_cast<u64>(static_cast<i64>(st.disp_eff));
        if (st.base != kNoReg) a += reg_value(cpu, st.base, is64);
        if (st.index != kNoReg) a += reg_value(cpu, st.index, is64) << st.scale_log2;
        addr1[s] = a & amask;
        if (st.align16 && (addr1[s] & 15u) != 0) return 0;  // body raises #GP
    }
    for (u8 s = 0; s < ns; ++s) {
        const bool store = d.has_store && s == d.n_loads;
        span[s] = safe_span(*mem, addr1[s], d.dir_down, w, W, store, is64);
        W = std::min(W, span[s].elems);
        if (W < 2) return 0;
    }

    const bool is_copy = d.kind == Kind::Copy;
    const u8 dst = d.n_loads;
    if (is_copy && !copy_hazard_free(addr1[dst], addr1[0], d.dir_down, w, W)) return 0;

    // Iterations allowed in bulk if no data exit fires: leave the exit
    // iteration and the one before it to the body.
    u64 m_cap = (n_exit <= W) ? (n_exit >= 2 ? n_exit - 2 : 0) : W - 1;
    // min_iters is a whole-loop trip count; the bulk is that less the two
    // iterations the body keeps.
    const u32 mi = min_iters(d.kind, d.elem_log2);
    const u32 floor = thresholds_bypassed() ? 1u : std::max(1u, mi > 2 ? mi - 2 : 1u);
    if (m_cap < floor) return 0;

    // 3. Data exits over the window [1, m_cap + 1] (element indices 0..m_cap).
    u64 m = m_cap;
    const u64 window = m_cap + 1;
    // The exit iteration is known to follow the bulk directly (induction
    // exit within the window, or a data exit found in it): the body only has
    // the tail left, so the block continues into it instead of re-dispatching.
    bool tail_only = n_exit <= W;
    ElemPred preds[2];
    for (u8 k = 0; k < d.n_data; ++k) preds[k] = make_pred(cpu, d.data[k], w);

    if (is_copy && d.n_data > 0 && d.ordered) {
        // Fused: loads stay in loop order with the stores between them.
        if (d.data[0].stream != 0 || d.n_data != 1) return 0;
        struct Ctx {
            GuestMemory* mem;
            u64 guest1;
            const u8* host1;
            u32 w;
        } ctx{mem, addr1[dst], span[dst].host, w};
        PageHook hook;
        hook.ctx = &ctx;
        hook.fn = [](void* p, const u8* host_elem) {
            auto* c = static_cast<Ctx*>(p);
            const u64 g = c->guest1 + static_cast<u64>(host_elem - c->host1);
            // Records write-watch for the page before it is written.
            (void)c->mem->host_ptr_store(static_cast<GuestAddr>(g), c->w);
            vitreo::mem_watch_check(static_cast<GuestAddr>(g), c->w, 0, "jit.loop_idiom.copy");
        };
        m = bulk_copy_until(span[dst].host, span[0].host, d.dir_down, w, m_cap, preds[0], hook);
        if (m == 0) return 0;
        if (m < m_cap) tail_only = true;  // stopped one before the terminator
    } else {
        u64 first_exit = window;  // 0-based element index
        if (d.kind == Kind::Compare) {
            const DataExit* se = nullptr;
            const ElemPred* term = nullptr;
            u8 term_stream = 0;
            for (u8 k = 0; k < d.n_data; ++k) {
                if (d.data[k].op == DataOp::StreamEq) {
                    se = &d.data[k];
                } else {
                    term = &preds[k];
                    term_stream = d.data[k].stream;
                }
            }
            if (se == nullptr) return 0;
            first_exit = bulk_compare(span[0].host, span[1].host, d.dir_down, w, window,
                                      se->exit_if_eq, term, term_stream, d.ordered);
        } else {
            for (u8 k = 0; k < d.n_data; ++k) {
                const u8 s = d.data[k].stream;
                if (s >= d.n_loads) return 0;
                const u64 hit = bulk_scan(span[s].host, d.dir_down, w,
                                          std::min(window, first_exit), preds[k], d.ordered);
                first_exit = std::min(first_exit, hit);
            }
        }
        if (first_exit < window) {
            // Element index i exits in iteration i + 1.
            m = first_exit >= 1 ? first_exit - 1 : 0;
            tail_only = true;
        }
        if (m < floor) return 0;

        // 4. The bulk.
        if (d.kind == Kind::Fill) {
            u8 v[16];
            value_bytes(cpu, d.fill.src, d.fill.reg, d.fill.high8, d.fill.imm, v);
            u8* host = span[dst].host;
            const u64 lo = d.dir_down ? addr1[dst] - (m - 1) * w : addr1[dst];
            // safe_span proved [lo, lo + m*w) writable and not executable;
            // record write-watch as host_ptr_store would, before the write.
            const u8* h = d.dir_down ? host - (m - 1) * w : host;
            write_watch::record_store(static_cast<GuestAddr>(lo), static_cast<usize>(m * w));
            vitreo::mem_watch_check(static_cast<GuestAddr>(lo), m * w, 0, "jit.loop_idiom.fill");
            bulk_fill(host, d.dir_down, w, m, v, d.ordered);
            if (store_ring_enabled()) mem_watch_bytes(static_cast<GuestAddr>(lo), h, m * w, "jit.loop_idiom.fill");
            diagnostics::perf_add_fused_memset(m * w);
        } else if (is_copy) {
            const u64 lo = d.dir_down ? addr1[dst] - (m - 1) * w : addr1[dst];
            const u8* h = d.dir_down ? span[dst].host - (m - 1) * w : span[dst].host;
            write_watch::record_store(static_cast<GuestAddr>(lo), static_cast<usize>(m * w));
            vitreo::mem_watch_check(static_cast<GuestAddr>(lo), m * w, 0, "jit.loop_idiom.copy");
            bulk_copy(span[dst].host, span[0].host, d.dir_down, w, m, d.ordered);
            if (store_ring_enabled()) mem_watch_bytes(static_cast<GuestAddr>(lo), h, m * w, "jit.loop_idiom.copy");
            diagnostics::perf_add_fused_memcpy(m * w);
        }
    }
    if (is_copy && d.n_data > 0 && d.ordered) {
        const u64 lo = d.dir_down ? addr1[dst] - (m - 1) * w : addr1[dst];
        const u8* h = d.dir_down ? span[dst].host - (m - 1) * w : span[dst].host;
        if (store_ring_enabled()) {
            mem_watch_bytes(static_cast<GuestAddr>(lo), h, m * w, "jit.loop_idiom.copy");
        }
        diagnostics::perf_add_fused_memcpy(m * w);
    }

    // 5. Commit the inductions (C1: a 32-bit write zero-extends in 64-bit
    // mode; in 32-bit mode only the architectural 32 bits change).
    for (u8 i = 0; i < d.n_ind; ++i) {
        const Induction& ind = d.ind[i];
        const u64 delta = static_cast<u64>(static_cast<i64>(ind.step)) * m;
        if (ind.wide) {
            cpu->gpr.set64(ind.reg, cpu->gpr.get64(ind.reg) + delta);
        } else if (is64) {
            cpu->gpr.set32(ind.reg, static_cast<u32>(cpu->gpr.get32(ind.reg) + delta));
        } else {
            const u64 old = cpu->gpr.get64(ind.reg);
            cpu->gpr.set64(ind.reg, (old & ~u64{0xFFFFFFFF}) |
                                        static_cast<u32>(static_cast<u32>(old) + delta));
        }
    }
    cpu->jit_iters = static_cast<u32>(m);
    cpu->rip = d.loop_rip;
    g_bulk_runs.fetch_add(1, std::memory_order_relaxed);
    return tail_only ? kRanTail : kRanRedispatch;
}

}  // namespace vitreo::cpu::jit

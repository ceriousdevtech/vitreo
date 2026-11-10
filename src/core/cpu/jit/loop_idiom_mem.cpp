// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom memory primitives. See loop_idiom_mem.hpp.
//
// Ordering (Intel SDM Vol. 3 8.2.2): x86-TSO keeps loads in order with
// loads, stores in order with stores, and stores after older loads. When the
// JIT lowers these accesses ordered (JitConfig::tso_mode != Off, and for
// 16-byte elements tso_vector), the bulk path issues them in loop order with
// acquire loads and release stores. Coalescing element-aligned accesses into
// one aligned 8-byte access keeps every element single-copy atomic (Vol. 3
// 8.1.1) and only removes interleavings, which TSO permits. Plain mode
// (tso_mode == Off) uses memset / memmove / memchr.

#include "core/cpu/jit/loop_idiom_mem.hpp"

#include "core/memory/guard_page_traps.hpp"

#include <algorithm>
#include <cstring>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace vitreo::cpu::jit::loop_idiom {

namespace {

constexpr u64 kGuestPage = 4096;

inline bool elem_aligned(const void* p, u32 w) noexcept {
    const uintptr_t a = reinterpret_cast<uintptr_t>(p);
    return (a & ((w < 8 ? w : 8u) - 1u)) == 0;
}

inline void load_acq(const u8* p, u32 w, u8* out) noexcept {
    if (!elem_aligned(p, w)) {
        // A misaligned element has no acquire instruction; order it with a
        // load barrier after a plain access (DMB ISHLD), as TSO needs.
        std::memcpy(out, p, w);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        return;
    }
    switch (w) {
    case 1: out[0] = __atomic_load_n(p, __ATOMIC_ACQUIRE); return;
    case 2: {
        const u16 v = __atomic_load_n(reinterpret_cast<const u16*>(p), __ATOMIC_ACQUIRE);
        std::memcpy(out, &v, 2);
        return;
    }
    case 4: {
        const u32 v = __atomic_load_n(reinterpret_cast<const u32*>(p), __ATOMIC_ACQUIRE);
        std::memcpy(out, &v, 4);
        return;
    }
    case 8: {
        const u64 v = __atomic_load_n(reinterpret_cast<const u64*>(p), __ATOMIC_ACQUIRE);
        std::memcpy(out, &v, 8);
        return;
    }
    default: {
        // 16 bytes: x86 gives no 16-byte single-copy atomicity; two ordered
        // 8-byte halves in address order.
        const u64 lo = __atomic_load_n(reinterpret_cast<const u64*>(p), __ATOMIC_ACQUIRE);
        const u64 hi = __atomic_load_n(reinterpret_cast<const u64*>(p + 8), __ATOMIC_ACQUIRE);
        std::memcpy(out, &lo, 8);
        std::memcpy(out + 8, &hi, 8);
        return;
    }
    }
}

inline void store_rel(u8* p, u32 w, const u8* in) noexcept {
    if (!elem_aligned(p, w)) {
        // Release fence (DMB ISH) orders this store after every earlier
        // access, as STLR would for an aligned one.
        __atomic_thread_fence(__ATOMIC_RELEASE);
        std::memcpy(p, in, w);
        return;
    }
    switch (w) {
    case 1: __atomic_store_n(p, in[0], __ATOMIC_RELEASE); return;
    case 2: {
        u16 v;
        std::memcpy(&v, in, 2);
        __atomic_store_n(reinterpret_cast<u16*>(p), v, __ATOMIC_RELEASE);
        return;
    }
    case 4: {
        u32 v;
        std::memcpy(&v, in, 4);
        __atomic_store_n(reinterpret_cast<u32*>(p), v, __ATOMIC_RELEASE);
        return;
    }
    case 8: {
        u64 v;
        std::memcpy(&v, in, 8);
        __atomic_store_n(reinterpret_cast<u64*>(p), v, __ATOMIC_RELEASE);
        return;
    }
    default: {
        u64 lo, hi;
        std::memcpy(&lo, in, 8);
        std::memcpy(&hi, in + 8, 8);
        __atomic_store_n(reinterpret_cast<u64*>(p), lo, __ATOMIC_RELEASE);
        __atomic_store_n(reinterpret_cast<u64*>(p + 8), hi, __ATOMIC_RELEASE);
        return;
    }
    }
}

inline i64 stride_of(bool down, u32 w) noexcept {
    return down ? -static_cast<i64>(w) : static_cast<i64>(w);
}

inline bool aligned8(const void* p) noexcept {
    return (reinterpret_cast<uintptr_t>(p) & 7u) == 0;
}

}  // namespace

// ---------------------------------------------------------------------------

Span safe_span(GuestMemory& mem, u64 addr1, bool down, u32 w, u64 limit,
               bool store, bool is64) {
    Span out;
    if (limit == 0 || w == 0) return out;
    // 32-bit linear addresses wrap modulo 2^32 (Intel SDM Vol. 1 3.7.5); the
    // bulk path never wraps, it stops short and leaves the wrap to the body.
    u64 cap = limit;
    if (!is64) {
        if (addr1 > 0xFFFFFFFFull || addr1 + w > 0x100000000ull) return out;
        cap = std::min(cap, down ? addr1 / w + 1 : (0x100000000ull - addr1) / w);
    } else {
        if (addr1 > ~u64{0} - w) return out;
        cap = std::min(cap, down ? addr1 / w + 1 : (~u64{0} - addr1) / w);
    }
    if (cap == 0) return out;

    const GuestMemory::AccessInfo info = mem.probe_access(static_cast<GuestAddr>(addr1), w);
    if (info.host_ptr == nullptr || !info.readable) return out;
    if (store && (!info.writable || info.executable)) return out;
    if (info.region_end <= addr1 || info.region_base > addr1) return out;
    const u64 avail = down ? (addr1 + w - info.region_base) / w
                           : (info.region_end - addr1) / w;
    u64 elems = std::min(cap, avail);
    if (elems == 0) return out;

    auto range = [&](u64 n, u64& lo, u64& len) {
        lo = down ? addr1 - (n - 1) * w : addr1;
        len = n * w;
    };

    // PAGE_GUARD pages are host PROT_NONE while armed but report the base
    // protection: stop before the first armed page in loop order, so the
    // translated body takes the STATUS_GUARD_PAGE_VIOLATION one-shot.
    if (guard_page::any_armed()) {
        u64 lo, len;
        range(elems, lo, len);
        if (!down) {
            for (u64 page = lo & ~(kGuestPage - 1); page < lo + len; page += kGuestPage) {
                if (guard_page::is_armed(static_cast<GuestAddr>(page))) {
                    elems = page <= addr1 ? 0 : (page - addr1) / w;
                    break;
                }
            }
        } else {
            const u64 top = addr1 + w;
            for (u64 page = (top - 1) & ~(kGuestPage - 1);; page -= kGuestPage) {
                if (guard_page::is_armed(static_cast<GuestAddr>(page))) {
                    const u64 page_end = page + kGuestPage;
                    elems = page_end >= top ? 0 : (top - page_end) / w;
                    break;
                }
                if (page <= lo || page < kGuestPage) break;
            }
        }
        if (elems == 0) return out;
    }

    if (store) {
        // Self-modifying code must stay instruction-granular (SDM Vol. 3
        // 8.1.3): a page holding a compiled block, or a code alias, is left
        // to the per-instruction stores that drive SMC invalidation.
        u64 lo, len;
        range(elems, lo, len);
        if (mem.overlaps_code_alias(static_cast<GuestAddr>(lo), static_cast<usize>(len)) ||
            mem.jit_code_pages_overlap(static_cast<GuestAddr>(lo), static_cast<usize>(len))) {
            return out;
        }
    }
    out.host = info.host_ptr;
    out.elems = elems;
    return out;
}

// ---------------------------------------------------------------------------

bool ElemPred::eq(const u8* e) const noexcept {
    if (op == DataOp::AnyLaneEq) {
#if defined(__aarch64__)
        const uint8x16_t a = vld1q_u8(e);
        const uint8x16_t b = vld1q_u8(value);
        uint8x16_t c;
        switch (lane) {
        case 1: c = vceqq_u8(a, b); break;
        case 2: c = vreinterpretq_u8_u16(vceqq_u16(vreinterpretq_u16_u8(a),
                                                   vreinterpretq_u16_u8(b))); break;
        default: c = vreinterpretq_u8_u32(vceqq_u32(vreinterpretq_u32_u8(a),
                                                    vreinterpretq_u32_u8(b))); break;
        }
        return vmaxvq_u8(c) != 0;
#else
        for (u32 j = 0; j < 16; j += lane) {
            if (std::memcmp(e + j, value + j, lane) == 0) return true;
        }
        return false;
#endif
    }
    if (never_eq) return false;
    switch (cmp_bytes) {
    case 1: return e[0] == value[0];
    case 2: { u16 a, b; std::memcpy(&a, e, 2); std::memcpy(&b, value, 2); return a == b; }
    case 4: { u32 a, b; std::memcpy(&a, e, 4); std::memcpy(&b, value, 4); return a == b; }
    case 8: { u64 a, b; std::memcpy(&a, e, 8); std::memcpy(&b, value, 8); return a == b; }
    default: return std::memcmp(e, value, cmp_bytes) == 0;
    }
}

// ---------------------------------------------------------------------------

void bulk_fill(u8* first, bool down, u32 w, u64 elems, const u8 value[16], bool ordered) {
    if (elems == 0) return;
    const i64 st = stride_of(down, w);
    if (!ordered) {
        u8* lo = down ? first - (elems - 1) * w : first;
        if (w == 1) {
            std::memset(lo, value[0], elems);
            return;
        }
        // Every element holds the same bytes, so the order of a plain fill
        // is not observable in the result.
        switch (w) {
        case 2: { u16 v; std::memcpy(&v, value, 2);
                  for (u64 i = 0; i < elems; ++i) std::memcpy(lo + i * 2, &v, 2); return; }
        case 4: { u32 v; std::memcpy(&v, value, 4);
                  for (u64 i = 0; i < elems; ++i) std::memcpy(lo + i * 4, &v, 4); return; }
        case 8: { u64 v; std::memcpy(&v, value, 8);
                  for (u64 i = 0; i < elems; ++i) std::memcpy(lo + i * 8, &v, 8); return; }
        default:
            for (u64 i = 0; i < elems; ++i) std::memcpy(lo + i * 16, value, 16);
            return;
        }
    }
    // Ordered: release stores in loop order.
    if (!down && w <= 8) {
        u64 i = 0;
        u8* p = first;
        while (i < elems && !aligned8(p)) {
            store_rel(p, w, value);
            p += w;
            ++i;
        }
        // Element-aligned elements tile an aligned granule with the same
        // bytes whatever the granule's position.
        u8 g[8];
        for (u32 j = 0; j < 8; j += w) std::memcpy(g + j, value, w);
        u64 gv;
        std::memcpy(&gv, g, 8);
        const u32 per = 8 / w;
        while (elems - i >= per) {
            __atomic_store_n(reinterpret_cast<u64*>(p), gv, __ATOMIC_RELEASE);
            p += 8;
            i += per;
        }
        while (i < elems) {
            store_rel(p, w, value);
            p += w;
            ++i;
        }
        return;
    }
    u8* p = first;
    for (u64 i = 0; i < elems; ++i, p += st) store_rel(p, w, value);
}

void bulk_copy(u8* dst_first, const u8* src_first, bool down, u32 w, u64 elems,
               bool ordered) {
    if (elems == 0) return;
    const i64 st = stride_of(down, w);
    if (!ordered) {
        // With copy_hazard_free, the sequential element loop and memmove
        // agree byte for byte.
        u8* dlo = down ? dst_first - (elems - 1) * w : dst_first;
        const u8* slo = down ? src_first - (elems - 1) * w : src_first;
        std::memmove(dlo, slo, elems * w);
        return;
    }
    u8 buf[16];
    if (!down && w <= 8 &&
        ((reinterpret_cast<uintptr_t>(dst_first) ^ reinterpret_cast<uintptr_t>(src_first)) & 7u) == 0) {
        u64 i = 0;
        u8* d = dst_first;
        const u8* s = src_first;
        while (i < elems && !aligned8(d)) {
            load_acq(s, w, buf);
            store_rel(d, w, buf);
            d += w; s += w; ++i;
        }
        const u32 per = 8 / w;
        while (elems - i >= per) {
            const u64 v = __atomic_load_n(reinterpret_cast<const u64*>(s), __ATOMIC_ACQUIRE);
            __atomic_store_n(reinterpret_cast<u64*>(d), v, __ATOMIC_RELEASE);
            d += 8; s += 8; i += per;
        }
        while (i < elems) {
            load_acq(s, w, buf);
            store_rel(d, w, buf);
            d += w; s += w; ++i;
        }
        return;
    }
    u8* d = dst_first;
    const u8* s = src_first;
    for (u64 i = 0; i < elems; ++i, d += st, s += st) {
        load_acq(s, w, buf);
        store_rel(d, w, buf);
    }
}

u64 bulk_scan(const u8* first, bool down, u32 w, u64 elems, const ElemPred& pred,
              bool ordered) {
    if (elems == 0) return 0;
    const i64 st = stride_of(down, w);
    if (!ordered) {
        if (!down && w == 1 && pred.op == DataOp::ValueEq && pred.exit_if_eq &&
            pred.cmp_bytes == 1 && !pred.never_eq) {
            const void* hit = std::memchr(first, pred.value[0], elems);
            return hit ? static_cast<u64>(static_cast<const u8*>(hit) - first) : elems;
        }
        const u8* p = first;
        for (u64 i = 0; i < elems; ++i, p += st) {
            if (pred.exits(p)) return i;
        }
        return elems;
    }
    u8 buf[16];
    if (!down && w <= 8) {
        u64 i = 0;
        const u8* p = first;
        while (i < elems && !aligned8(p)) {
            load_acq(p, w, buf);
            if (pred.exits(buf)) return i;
            p += w; ++i;
        }
        const u32 per = 8 / w;
        while (elems - i >= per) {
            const u64 v = __atomic_load_n(reinterpret_cast<const u64*>(p), __ATOMIC_ACQUIRE);
            u8 g[8];
            std::memcpy(g, &v, 8);
            for (u32 j = 0; j < per; ++j) {
                if (pred.exits(g + j * w)) return i + j;
            }
            p += 8; i += per;
        }
        while (i < elems) {
            load_acq(p, w, buf);
            if (pred.exits(buf)) return i;
            p += w; ++i;
        }
        return elems;
    }
    const u8* p = first;
    for (u64 i = 0; i < elems; ++i, p += st) {
        load_acq(p, w, buf);
        if (pred.exits(buf)) return i;
    }
    return elems;
}

u64 bulk_compare(const u8* a, const u8* b, bool down, u32 w, u64 elems, bool exit_if_eq,
                 const ElemPred* term, u8 term_stream, bool ordered) {
    const i64 st = stride_of(down, w);
    if (!ordered && !down && !exit_if_eq && term == nullptr) {
        // First mismatch: compare 8 bytes at a time, then locate the element.
        const u64 bytes = elems * w;
        u64 off = 0;
        while (bytes - off >= 8) {
            u64 x, y;
            std::memcpy(&x, a + off, 8);
            std::memcpy(&y, b + off, 8);
            if (x != y) break;
            off += 8;
        }
        for (u64 i = off / w; i < elems; ++i) {
            if (std::memcmp(a + i * w, b + i * w, w) != 0) return i;
        }
        return elems;
    }
    if (ordered && term == nullptr && w <= 8 && elem_aligned(a, w) && elem_aligned(b, w)) {
        // TSO: one acquire load per element, streams interleaved in body
        // order (a[i], b[i], a[i+1], ...); every element of both streams
        // stays w-aligned because the stride is +-w.
        auto run = [&](auto tag) -> u64 {
            using T = decltype(tag);
            const T* pa = reinterpret_cast<const T*>(a);
            const T* pb = reinterpret_cast<const T*>(b);
            const i64 step = down ? -1 : 1;
            for (u64 i = 0; i < elems; ++i, pa += step, pb += step) {
                const T x = __atomic_load_n(pa, __ATOMIC_ACQUIRE);
                const T y = __atomic_load_n(pb, __ATOMIC_ACQUIRE);
                if ((x == y) == exit_if_eq) return i;
            }
            return elems;
        };
        switch (w) {
        case 1: return run(u8{});
        case 2: return run(u16{});
        case 4: return run(u32{});
        case 8: return run(u64{});
        default: break;
        }
    }
    u8 ba[16], bb[16];
    for (u64 i = 0; i < elems; ++i, a += st, b += st) {
        const u8* ea = a;
        const u8* eb = b;
        if (ordered) {
            // Body order: the first stream's element is loaded first.
            load_acq(a, w, ba);
            load_acq(b, w, bb);
            ea = ba;
            eb = bb;
        }
        const bool eq = std::memcmp(ea, eb, w) == 0;
        if (eq == exit_if_eq) return i;
        if (term && term->exits(term_stream == 0 ? ea : eb)) return i;
    }
    return elems;
}

u64 bulk_copy_until(u8* dst_first, const u8* src_first, bool down, u32 w, u64 max_stored,
                    const ElemPred& term, PageHook before_page) {
    const i64 st = stride_of(down, w);
    u8 prev[16];
    u8 cur[16];
    u64 stored = 0;
    uintptr_t last_page = 0;
    const u8* s = src_first;
    u8* d = dst_first;
    for (u64 i = 0; i <= max_stored; ++i, s += st) {
        load_acq(s, w, cur);
        if (term.exits(cur)) break;
        if (i >= 1) {
            // TSO lets this load pass the older store of element i-1.
            const uintptr_t page = reinterpret_cast<uintptr_t>(d) & ~uintptr_t{kGuestPage - 1};
            if (before_page.fn != nullptr && (i == 1 || page != last_page)) {
                before_page.fn(before_page.ctx, d);
                last_page = page;
            }
            store_rel(d, w, prev);
            d += st;
            stored = i;
        }
        std::memcpy(prev, cur, w);
    }
    return stored;
}

bool copy_hazard_free(u64 dst1, u64 src1, bool down, u32 w, u64 iters) noexcept {
    if (iters == 0) return true;
    const u64 span = iters * w;
    const u64 dlo = down ? dst1 - (iters - 1) * w : dst1;
    const u64 slo = down ? src1 - (iters - 1) * w : src1;
    const bool disjoint = dlo + span <= slo || slo + span <= dlo;
    if (disjoint) return true;
    // Equal strides: a write in iteration j can reach the read of a later
    // iteration only when the destination runs ahead of the source.
    return down ? dst1 >= src1 : dst1 <= src1;
}

}  // namespace vitreo::cpu::jit::loop_idiom

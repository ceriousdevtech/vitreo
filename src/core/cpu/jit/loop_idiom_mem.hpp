// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// WS2 loop-idiom memory primitives: the accessible prefix of a guest stream,
// and the plain / TSO-ordered bulk fill, copy, scan and compare that run the
// whole iterations of a recognised loop on the host.
//
// Element k (1-based) of a stream is at addr1 + (k - 1) * stride, stride =
// +w or -w. The host bytes of element 1 start at `Span::host`; element k's
// at host + (k - 1) * stride (a span never leaves one uniformly-protected,
// host-contiguous guest range).

#pragma once

#include "core/cpu/jit/loop_idiom_desc.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::cpu::jit::loop_idiom {

struct Span {
    u8* host{nullptr};
    u64 elems{0};
};

/// Largest elems <= limit such that elements 1..elems of the stream lie in
/// one host-contiguous guest range that is readable (or, for `store`,
/// writable, not executable, not a code alias and holding no compiled code),
/// contains no armed PAGE_GUARD page, and does not wrap (32-bit) or overflow.
[[nodiscard]] Span safe_span(GuestMemory& mem, u64 addr1, bool down, u32 w,
                             u64 limit, bool store, bool is64);

/// Exit predicate on one element.
struct ElemPred {
    DataOp op{DataOp::ValueEq};
    bool exit_if_eq{true};
    u32 cmp_bytes{0};   ///< ValueEq: bytes compared (min(element, compare width))
    bool never_eq{false};
    u32 lane{1};        ///< AnyLaneEq lane bytes
    alignas(16) u8 value[16]{};

    [[nodiscard]] bool eq(const u8* e) const noexcept;
    [[nodiscard]] bool exits(const u8* e) const noexcept { return eq(e) == exit_if_eq; }
};

/// Element bytes `elems` of a stream, element 1 first.
void bulk_fill(u8* first, bool down, u32 w, u64 elems, const u8 value[16], bool ordered);
/// Sequential element copy (caller has proven copy_hazard_free).
void bulk_copy(u8* dst_first, const u8* src_first, bool down, u32 w, u64 elems,
               bool ordered);
/// 0-based index of the first element whose predicate says "exit", else elems.
[[nodiscard]] u64 bulk_scan(const u8* first, bool down, u32 w, u64 elems,
                            const ElemPred& pred, bool ordered);
/// 0-based index of the first element where an exit fires: elements equal /
/// differ (per `exit_if_eq`) between the two streams, or `term` (if non-null)
/// fires on stream `term_stream`'s element; else elems.
[[nodiscard]] u64 bulk_compare(const u8* a, const u8* b, bool down, u32 w, u64 elems,
                               bool exit_if_eq, const ElemPred* term, u8 term_stream,
                               bool ordered);
/// Fused ordered copy with a terminator on the source: loads element i,
/// stops if it exits, else stores element i-1. Returns the number of elements
/// stored, which is (first exit index - 1), or `max_stored` when no element
/// in 0..max_stored exits. Reads at most max_stored + 1 elements.
/// `before_page` runs before the first store into each 4 KiB page (with the
/// host pointer of the element about to be stored), so write-watch pages are
/// recorded before they are written.
struct PageHook {
    void* ctx{nullptr};
    void (*fn)(void* ctx, const u8* host_elem){nullptr};
};
[[nodiscard]] u64 bulk_copy_until(u8* dst_first, const u8* src_first, bool down, u32 w,
                                  u64 max_stored, const ElemPred& term,
                                  PageHook before_page = {});

/// True when copying `iters` elements one by one in loop order gives the
/// same result as bulk_copy: no element written can be one read later.
[[nodiscard]] bool copy_hazard_free(u64 dst1, u64 src1, bool down, u32 w,
                                    u64 iters) noexcept;

}  // namespace vitreo::cpu::jit::loop_idiom

#pragma once

// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// Private to the linear_fault*.cpp translation units (not installed, not
// a SOURCES entry). linear_fault.cpp was one file whose tables and
// instruction-word site recognizers were file-local. When it was split by
// cohesion -- the signal handler and its installation (linear_fault.cpp),
// site backpatching and its trampoline (linear_fault_patch.cpp), the
// registries (linear_fault_registry.cpp) and the precise-RIP map
// (linear_fault_rip_map.cpp) -- the names below gained callers in more
// than one part.
//
// The tables are only declared here. The handler reads what the registry
// and the installer write, so each must stay ONE object process-wide: it
// is defined once, in the file named beside it, instead of being copied
// into an anonymous namespace per file. Their types are in the named
// namespace for the same reason; a type from an anonymous namespace would
// give every translation unit its own object.
//
// The site recognizers are stateless and shared by the handler and the
// patcher. They stay in an anonymous namespace, as in the original file,
// so each of those translation units keeps its own inlinable copy;
// `inline` keeps a unit that does not call one of them free of
// -Wunused-function.

#include <type_traits>
#include <new>
#include "linear_fault.hpp"
#include "jit_compiler.hpp"
#include "jit_helpers.hpp"
#include "stencils.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <pthread.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <unistd.h>
#include <mach/mach_vm.h>
#include <mach/mach.h>
#include <mach/vm_param.h>

namespace vitreo::cpu::jit {

// Registered reservation windows. Written under a mutex; read lock-free in the
// signal handler (async-signal-safe: plain loads of trivially-copyable slots).
struct Window { std::atomic<u8*> base{nullptr}; std::atomic<usize> size{0}; };
constexpr int kMaxWindows = 8;
// Defined in linear_fault_registry.cpp.
extern Window g_windows[kMaxWindows];
extern Window g_views[kMaxWindows];

// Defined in linear_fault.cpp.
extern std::atomic<void*> g_trampoline;
extern std::atomic<bool> g_installed;

// Defined in linear_fault_registry.cpp.
extern std::atomic<uintptr_t> g_shared_helper_lo;
extern std::atomic<uintptr_t> g_shared_helper_hi;

// Exact executable JIT arena ownership. Registration publishes a fresh,
// immutable snapshot; the signal handler performs only a lock-free pointer
// load and plain reads. Old snapshots are never rewritten because a handler
// may still hold one. Arena reclamation publishes a snapshot without the old
// range before unmapping it; old snapshots remain valid static storage.
struct JitCodeRange { uintptr_t lo; uintptr_t hi; };
constexpr usize kMaxJitCodeRanges = 64;
struct JitCodeRangeSnapshot {
    usize count{0};
    JitCodeRange ranges[kMaxJitCodeRanges]{};
};
// Defined in linear_fault_registry.cpp.
extern std::atomic<const JitCodeRangeSnapshot*> g_jit_ranges;

// The preservation trampoline the handler redirects a faulting site to.
// Defined in linear_fault_patch.cpp.
extern "C" void vitreo_jit_tso_patch_trampoline();

namespace {

[[nodiscard]] inline i64 sign_extend(u32 value, unsigned bits) noexcept {
    const u32 shift = 32u - bits;
    return static_cast<i64>(static_cast<i32>(value << shift) >> shift);
}

// A lazy cold slot (linear_fault.hpp, ColdLazyRecord) at @p t. Pending: its
// `BL lazy_entry`, whose stub (and variants) do not exist yet. Materialized:
// its `B stub`, @p stub then the entry an eager check-byte branch would
// target. No eager entry begins with a B or a BL, so neither shape is
// mistaken for one.
enum class LazySlot { None, Pending, Materialized };
inline LazySlot lazy_slot(uintptr_t t, uintptr_t& stub) noexcept {
    if (!jit_code_range_contains(t) ||
        !jit_code_range_contains(t + 4u * ColdLazyRecord::kWords)) {
        return LazySlot::None;
    }
    u32 w = 0;
    __builtin_memcpy(&w, reinterpret_cast<const void*>(t), 4);
    if ((w & 0xFC000000u) == 0x14000000u) {
        stub = t + static_cast<uintptr_t>(sign_extend(w & 0x3FFFFFFu, 26) * 4);
        return jit_code_range_contains(stub) ? LazySlot::Materialized
                                             : LazySlot::None;
    }
    if ((w & 0xFC000000u) != 0x94000000u) return LazySlot::None;
    const uintptr_t entry =
        t + static_cast<uintptr_t>(sign_extend(w & 0x3FFFFFFu, 26) * 4);
    if (!jit_code_range_contains(entry) || !jit_code_range_contains(entry + 4)) {
        return LazySlot::None;
    }
    u32 ldr = 0, br = 0;
    __builtin_memcpy(&ldr, reinterpret_cast<const void*>(entry), 4);
    __builtin_memcpy(&br, reinterpret_cast<const void*>(entry + 4), 4);
    return ldr == arm64::ldr_x_uoff(16, 20, JIT_CTX_OFF_COLD_LAZY_ENTRY >> 3) &&
                   br == 0xD61F0200u
               ? LazySlot::Pending
               : LazySlot::None;
}

// Is the lazy slot at @p t (pending or materialized) the one of the
// crossing-by-fault access at @p site? Its record returns to site + 4.
inline bool lazy_slot_crossing_of(uintptr_t t, uintptr_t site) noexcept {
    uintptr_t stub = 0;
    if (lazy_slot(t, stub) == LazySlot::None) return false;
    u32 w0 = 0, w2 = 0;
    __builtin_memcpy(&w0, reinterpret_cast<const void*>(t + 4), 4);
    __builtin_memcpy(&w2, reinterpret_cast<const void*>(t + 12), 4);
    return (w2 & ColdLazyRecord::kCrossingVariant) != 0 &&
           (w2 & ColdLazyRecord::kLeanVariant) == 0 &&
           t + static_cast<uintptr_t>(ColdLazyRecord::lo(w0) * 4) == site + 4;
}

// Is the lazy slot at @p t the one of the range-by-fault site at @p site?
// Its record's check-byte branch is site + 8.
inline bool lazy_slot_range_of(uintptr_t t, uintptr_t site) noexcept {
    uintptr_t stub = 0;
    if (lazy_slot(t, stub) == LazySlot::None) return false;
    u32 w0 = 0, w1 = 0, w2 = 0;
    __builtin_memcpy(&w0, reinterpret_cast<const void*>(t + 4), 4);
    __builtin_memcpy(&w1, reinterpret_cast<const void*>(t + 8), 4);
    __builtin_memcpy(&w2, reinterpret_cast<const void*>(t + 12), 4);
    if ((w2 & ColdLazyRecord::kRangeVariant) == 0 ||
        !ColdLazyRecord::has_mixed(w1)) {
        return false;
    }
    const uintptr_t ret = t + static_cast<uintptr_t>(ColdLazyRecord::lo(w0) * 4);
    return ret + static_cast<uintptr_t>(ColdLazyRecord::lo(w1) * 4) == site + 8;
}

// Is @p variant the start of a crossing variant whose final B returns to
// @p site + 4 (linear_fault.hpp, kTsoCrossingVariantWords)?
inline bool crossing_variant_returns_to(uintptr_t variant, uintptr_t site) noexcept {
    const uintptr_t last = variant + 4u * (tso_crossing_variant_words() - 1u);
    if (!jit_code_range_contains(variant) || !jit_code_range_contains(last)) {
        return false;
    }
    u32 b = 0;
    __builtin_memcpy(&b, reinterpret_cast<const void*>(last), sizeof(b));
    if ((b & 0xFC000000u) != 0x14000000u) return false;
    return last + static_cast<uintptr_t>(sign_extend(b & 0x3FFFFFFu, 26) * 4) ==
           site + 4;
}

// The marker-free ordered site of linear_fault.hpp: @p pc holds an
// LDAPR/LDAR/STLR whose address register was formed by the ADD before it from
// the linear base, behind a check-byte branch on X3 whose target is preceded
// by this site's crossing variant. Returns the variant.
inline bool crossing_variant_site(uintptr_t pc, uintptr_t& variant) noexcept {
    if (pc < 8 || !jit_code_range_contains(pc - 8)) return false;
    u32 insn = 0, add = 0, branch = 0;
    __builtin_memcpy(&insn, reinterpret_cast<const void*>(pc), 4);
    __builtin_memcpy(&add, reinterpret_cast<const void*>(pc - 4), 4);
    __builtin_memcpy(&branch, reinterpret_cast<const void*>(pc - 8), 4);
    const u32 family = insn & 0x3FFFFC00u;
    if (family != 0x38BFC000u && family != 0x08DFFC00u &&
        family != 0x089FFC00u) {
        return false;
    }
    // ADD Xa, X14, Xm (shifted register, LSL #0), Xa the access's base.
    const u32 base = (insn >> 5) & 31u;
    if ((add & 0xFFE0FFFFu) != (0x8B000000u | (14u << 5) | base)) return false;
    i64 displacement = 0;
    if ((branch & 31u) != 3u) return false;
    if ((branch & 0x7F000000u) == 0x37000000u) {         // TBNZ
        displacement = sign_extend((branch >> 5) & 0x3FFFu, 14) * 4;
    } else if ((branch & 0x7F000000u) == 0x35000000u) {  // CBNZ
        displacement = sign_extend((branch >> 5) & 0x7FFFFu, 19) * 4;
    } else {
        return false;
    }
    uintptr_t target = pc - 8 + static_cast<uintptr_t>(displacement);
    // A lazy site: before its stub exists the slot itself is the variant
    // (taking it materializes the stub and re-points the site); after, the
    // slot's B names the entry the variant precedes.
    uintptr_t stub = 0;
    switch (lazy_slot(target, stub)) {
        case LazySlot::Pending:
            if (!lazy_slot_crossing_of(target, pc)) return false;
            variant = target;
            return true;
        case LazySlot::Materialized: target = stub; break;
        case LazySlot::None: break;
    }
    const uintptr_t candidate = target - 4u * tso_crossing_variant_words();
    if (!crossing_variant_returns_to(candidate, pc)) return false;
    variant = candidate;
    return true;
}

// The site after its patch: `B variant`, with that variant returning here,
// or `B slot` to its lazy slot.
inline bool patched_crossing_variant_site(uintptr_t pc) noexcept {
    u32 b = 0;
    __builtin_memcpy(&b, reinterpret_cast<const void*>(pc), sizeof(b));
    if ((b & 0xFC000000u) != 0x14000000u) return false;
    const uintptr_t variant =
        pc + static_cast<uintptr_t>(sign_extend(b & 0x3FFFFFFu, 26) * 4);
    return crossing_variant_returns_to(variant, pc) ||
           lazy_slot_crossing_of(variant, pc);
}

// The range-by-fault site of linear_fault.hpp at @p site:
// [LSR X3, Xn, #14][LDRB W3, [X15, X3]][TBNZ W3 | CBNZ X3 -> T]. Its range
// variant sits immediately before the site's crossing variant -- present when
// the word before T is that variant's final `B site + 20`, the ordered access
// being at site + 16 -- or else immediately before T, and ends in
// `B site + 8`, the check-byte branch. Returns the variant.
inline bool range_variant_site(uintptr_t site, uintptr_t& variant) noexcept {
    if (!jit_code_range_contains(site) || !jit_code_range_contains(site + 8)) {
        return false;
    }
    u32 lsr = 0, ldrb = 0, branch = 0;
    __builtin_memcpy(&lsr, reinterpret_cast<const void*>(site), 4);
    __builtin_memcpy(&ldrb, reinterpret_cast<const void*>(site + 4), 4);
    __builtin_memcpy(&branch, reinterpret_cast<const void*>(site + 8), 4);
    constexpr u32 kRnMask = ~(u32{0x1F} << 5);
    if ((lsr & kRnMask) != (arm64::lsr_x_imm(3, 0, 14) & kRnMask)) return false;
    if (ldrb != arm64::ldrb_reg(3, 15, 3)) return false;
    if ((branch & 31u) != 3u) return false;
    i64 displacement = 0;
    if ((branch & 0x7F000000u) == 0x37000000u) {         // TBNZ
        displacement = sign_extend((branch >> 5) & 0x3FFFu, 14) * 4;
    } else if ((branch & 0x7F000000u) == 0x35000000u) {  // CBNZ
        displacement = sign_extend((branch >> 5) & 0x7FFFFu, 19) * 4;
    } else {
        return false;
    }
    uintptr_t target = site + 8 + static_cast<uintptr_t>(displacement);
    uintptr_t stub = 0;
    switch (lazy_slot(target, stub)) {
        case LazySlot::Pending:
            if (!lazy_slot_range_of(target, site)) return false;
            variant = target;
            return true;
        case LazySlot::Materialized: target = stub; break;
        case LazySlot::None: break;
    }
    uintptr_t candidate = target - 4u * kRangeVariantWords;
    if (crossing_variant_returns_to(target - 4u * tso_crossing_variant_words(),
                                    site + 16)) {
        candidate -= 4u * tso_crossing_variant_words();
    }
    const uintptr_t last = candidate + 4u * (kRangeVariantWords - 1u);
    if (!jit_code_range_contains(candidate) || !jit_code_range_contains(last)) {
        return false;
    }
    u32 b = 0;
    __builtin_memcpy(&b, reinterpret_cast<const void*>(last), sizeof(b));
    if ((b & 0xFC000000u) != 0x14000000u ||
        last + static_cast<uintptr_t>(sign_extend(b & 0x3FFFFFFu, 26) * 4) !=
            site + 8) {
        return false;
    }
    variant = candidate;
    return true;
}

// The range-by-fault site after its patch: `B variant` in place of the LSR.
inline bool patched_range_variant_site(uintptr_t site) noexcept {
    if (!jit_code_range_contains(site)) return false;
    u32 b = 0, ldrb = 0;
    __builtin_memcpy(&b, reinterpret_cast<const void*>(site), 4);
    __builtin_memcpy(&ldrb, reinterpret_cast<const void*>(site + 4), 4);
    if ((b & 0xFC000000u) != 0x14000000u || ldrb != arm64::ldrb_reg(3, 15, 3)) {
        return false;
    }
    const uintptr_t variant =
        site + static_cast<uintptr_t>(sign_extend(b & 0x3FFFFFFu, 26) * 4);
    if (lazy_slot_range_of(variant, site)) return true;
    const uintptr_t last = variant + 4u * (kRangeVariantWords - 1u);
    if (!jit_code_range_contains(variant) || !jit_code_range_contains(last)) {
        return false;
    }
    u32 back = 0;
    __builtin_memcpy(&back, reinterpret_cast<const void*>(last), 4);
    return (back & 0xFC000000u) == 0x14000000u &&
           last + static_cast<uintptr_t>(sign_extend(back & 0x3FFFFFFu, 26) * 4) ==
               site + 8;
}

// A JIT-view lean site (linear_fault.hpp, JitConfig::linear64_jit_view): its
// guard `CBNZ X16, V` (`CBNZ XZR, V` for a constant EA), then either
//
//     ADD X17, X15, Xm ; LDAPR | LDAR | STLR Rt, [X17]   (scalar)
//     ADD X17, X15, Xm ; LDP | STP Qa, Qb, [X17]         (256-bit)
//     LDR | STR Q Rt, [X15, Xm]                          (128-bit)
//
// with V the site's checked variant. X15 is the JIT view's base and is used
// this way by lean sites only, so the shapes are unambiguous. The fault
// handler rewrites the GUARD to `B V`: a thread already past it still faults
// at the access and must be sent back to it, so a guard that is already
// `B V` is recognized too.
enum class LeanAccess { None, ViaX17, Direct };

inline LeanAccess lean_access_kind(u32 w) noexcept {
    const u32 family = w & 0x3FFFFC00u;
    if ((family == 0x38BFC000u || family == 0x08DFFC00u ||
         family == 0x089FFC00u) &&
        ((w >> 5) & 31u) == 17u) {
        return LeanAccess::ViaX17;
    }
    constexpr u32 kPairMask = ~(0x1Fu | (0x1Fu << 10));  // any Rt, Rt2
    if ((w & kPairMask) == (arm64::ldp_q_off(0, 0, 17, 0) & kPairMask) ||
        (w & kPairMask) == (arm64::stp_q_off(0, 0, 17, 0) & kPairMask)) {
        return LeanAccess::ViaX17;
    }
    constexpr u32 kRegMask = 0xFFE0FFE0u;  // any Rt, Rm
    if ((w & kRegMask) == (arm64::ldr_q_reg(0, 15, 0) & kRegMask) ||
        (w & kRegMask) == (arm64::str_q_reg(0, 15, 0) & kRegMask)) {
        return LeanAccess::Direct;
    }
    return LeanAccess::None;
}

// The lean site whose access is at @p pc: its guard word and V.
inline bool lean_site(uintptr_t pc, uintptr_t& guard, uintptr_t& variant) noexcept {
    if (pc < 8 || !jit_code_range_contains(pc - 8)) return false;
    u32 insn = 0;
    __builtin_memcpy(&insn, reinterpret_cast<const void*>(pc), 4);
    const LeanAccess kind = lean_access_kind(insn);
    if (kind == LeanAccess::None) return false;
    if (kind == LeanAccess::ViaX17) {
        u32 add = 0;
        __builtin_memcpy(&add, reinterpret_cast<const void*>(pc - 4), 4);
        if ((add & 0xFFE0FFFFu) != (0x8B000000u | (15u << 5) | 17u)) return false;
        guard = pc - 8;
    } else {
        guard = pc - 4;
    }
    u32 w = 0;
    __builtin_memcpy(&w, reinterpret_cast<const void*>(guard), 4);
    uintptr_t target = 0;
    if ((w & 0xFF000000u) == 0xB5000000u &&                // CBNZ (64-bit)
        ((w & 31u) == 16u || (w & 31u) == 31u)) {
        target = guard + static_cast<uintptr_t>(
                             sign_extend((w >> 5) & 0x7FFFFu, 19) * 4);
    } else if ((w & 0xFC000000u) == 0x14000000u) {         // already B V
        target = guard + static_cast<uintptr_t>(
                             sign_extend(w & 0x3FFFFFFu, 26) * 4);
    } else {
        return false;
    }
    if (!jit_code_range_contains(target)) return false;
    variant = target;
    return true;
}

// The patcher's view: @p guard is an UNPATCHED lean guard. Returns V.
inline bool lean_guard(uintptr_t guard, uintptr_t& variant) noexcept {
    if (!jit_code_range_contains(guard)) return false;
    u32 w = 0;
    __builtin_memcpy(&w, reinterpret_cast<const void*>(guard), 4);
    if ((w & 0xFF000000u) != 0xB5000000u ||
        ((w & 31u) != 16u && (w & 31u) != 31u)) {
        return false;
    }
    uintptr_t g = 0;
    for (const uintptr_t access : {guard + 4, guard + 8}) {
        if (jit_code_range_contains(access) &&
            lean_site(access, g, variant) && g == guard) {
            return true;
        }
    }
    return false;
}

// True when @p addr lies in a registered JIT view or one of its guards.
inline bool jit_view_envelope_contains(uintptr_t addr) noexcept {
    for (int i = 0; i < kMaxWindows; ++i) {
        u8* const b = g_views[i].base.load(std::memory_order_acquire);
        if (b == nullptr) continue;
        const usize size = g_views[i].size.load(std::memory_order_acquire);
        const uintptr_t lo = reinterpret_cast<uintptr_t>(b) - kLinearJitViewGuardBytes;
        const uintptr_t hi =
            reinterpret_cast<uintptr_t>(b) + size + kLinearJitViewGuardBytes;
        if (addr >= lo && addr < hi) return true;
    }
    return false;
}

inline bool ordered_tso_site(uintptr_t pc, uintptr_t& site) noexcept {
    u32 insn = 0;
    __builtin_memcpy(&insn, reinterpret_cast<const void*>(pc), sizeof(insn));
    const u32 family = insn & 0x3FFFFC00u;
    const bool load = family == 0x38BFC000u || family == 0x08DFFC00u;
    const bool store = family == 0x089FFC00u;
    // The marker-free form first: what follows its load is the next guest
    // instruction's code, which may itself begin with a DMB, and must not be
    // mistaken for a two-word site's fence.
    if (uintptr_t variant = 0; crossing_variant_site(pc, variant)) {
        site = pc;
        return true;
    }
    // The neighbour word identifies the two-word site in BOTH of its fault-
    // reachable states. vitreo_jit_patch_tso_site rewrites the pair in two
    // separate CASes (marker -> DMB first, then the ordered access -> plain),
    // and only the fully patched pair stops faulting — so a SECOND thread
    // executing the same hot site inside that window takes its alignment
    // SIGBUS against [DMB, STLR]. Accepting only the marker sent exactly those
    // faults down the guest-ACCESS_VIOLATION path: measured on the GPU and
    // network-service processes crash-looping with 0xc0000005 at stable hot
    // sites (every faulting address straddled a 16-byte granule), which is
    // what the recorded 73s-vs-41s launcher startup regression actually was. A
    // mid-patch redirect is harmless: the trampoline's patch call finds
    // nothing left to CAS and the retry either re-faults (patcher still in
    // its window) or executes the completed pair.
    constexpr u32 kDmbIsh = 0xD5033BBFu;
    const auto is_site_word = [](u32 w) {
        return w == kTsoPatchMarker || w == kDmbIsh;
    };
    if (load && jit_code_range_contains(pc + 4)) {
        u32 marker = 0;
        __builtin_memcpy(&marker, reinterpret_cast<const void*>(pc + 4), 4);
        if (is_site_word(marker)) { site = pc; return true; }
    }
    if (store && pc >= 4 && jit_code_range_contains(pc - 4)) {
        u32 marker = 0;
        __builtin_memcpy(&marker, reinterpret_cast<const void*>(pc - 4), 4);
        if (is_site_word(marker)) { site = pc - 4; return true; }
    }
    return false;
}

} // namespace

} // namespace vitreo::cpu::jit

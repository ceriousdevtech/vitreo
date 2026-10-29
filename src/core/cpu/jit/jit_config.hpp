// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file jit_config.hpp
/// @brief Everything the JIT reads from the environment, in one place.
///
/// These 25 fields were members of JitCompiler, scattered across 1,200 lines
/// of a 135-member class and indistinguishable from the code arena, the pin
/// layout and the per-block flag state. They are none of those things: each is
/// read once during construction and never written again, and together they
/// decide which lowerings the compiler is allowed to emit at all.
///
/// Grouping them buys two concrete things:
///
///   1. A performance A/B stops being "set an environment variable and hope
///      it reached the right process" and becomes "construct with a different
///      config". The knobs are the experiment surface for this subsystem, and
///      they were not previously nameable as a set.
///   2. The configuration can be PRINTED. `describe()` renders the whole set,
///      so a measurement can record what it actually measured. Several of
///      these interact -- linear64 depends on hardware protection, TSO mode
///      forces strict ordering, a skip range disables native compilation for
///      part of the address space -- and a number taken without knowing all of
///      them is a number nobody can reproduce.
///
/// What is deliberately NOT here: the recompile counters and the churn
/// interpret set. Those look adjacent in the old header and are mutable
/// runtime state, not configuration.

#include <vitreo/common.hpp>

#include <string>
#include <vector>

namespace vitreo::cpu::jit {

// How guest memory accesses preserve x86 Total Store Order on ARM64.
//
// x86 guarantees TSO; ARM64 does not. A guest access translated to a
// plain LDR/STR may be reordered in ways no x86 core would permit, which
// silently corrupts lock-free guest code. Apple Silicon has a hardware
// TSO mode (ACTLR_EL1.EnTSO) but it is EL1-only and macOS grants it to
// Rosetta alone, so the ordering has to come from the instructions we
// emit. The ordering is an ISA requirement, not a per-title workaround.
enum class TsoMode : u8 {
    // No ordering. Fastest, and wrong for any multi-threaded guest.
    // VITREO_JIT_NO_TSO=1 — for measuring the cost of the other two and
    // for bisecting whether a bug is memory-model related.
    Off,
    // Ordering carried in the access instruction: LDAPR/LDAR for loads,
    // STLR for stores. One instruction per access, ordering only what
    // TSO requires.
    //
    // Shipping default when the fault-patch handler installs (fail closed
    // to Fence otherwise). Opt out with VITREO_JIT_TSO_ORDERED=0 or
    // VITREO_JIT_STRICT_TSO=1. 32-bit paths emit the same two-word
    // fault-patchable sites as x64 for 16-byte granule straddles.
    Ordered,
    // A full DMB ISH around legacy accesses. Long-mode scalar owners use
    // guarded LDAPR/STLR even in this mode and reserve DMB for granule
    // crossings and helper misses. The legacy lowering vastly over-orders
    // (it also
    // restores store->load ordering, which x86 does NOT require) and
    // costs roughly an order of magnitude. Opt-in via STRICT_TSO /
    // TSO_ORDERED=0 for bisection and as the reference oracle: if a bug
    // reproduces under Ordered but not under Fence, the Ordered lowering
    // is missing something. Fence has no alignment constraint: it leaves
    // the access a plain LDR/STR and orders it with a barrier.
    Fence,
};

/// Read once at construction, never written again.
struct JitConfig {
    // When true, force all JIT memory accesses through slow path
    // (set by VITREO_NO_JITCACHE=1 env var, for debugging).
    bool no_jitcache{false};

    // When true, emit the linear (guest-base) memory path: a 32-bit guest
    // access is a single UXTW instruction off linear_base, with bad accesses
    // recovered by the signal handler. Requires GuestMemory::is_linear() and
    // is default-on; VITREO_JIT_LINEAR=0 opts out. See
    // .docs/guest-base-memory-mapping.md.
    bool linear_jit{false};

    /// The 32-bit guest-base store may ask the per-host-page check byte
    /// instead of walking the code span / chunk / page maps and marking the
    /// write-watch dirty byte inline. True only when the reservation actually
    /// carries a check map that covers the whole 32-bit address space, since
    /// the emitted index is a bare UBFX with no bound test. Without it the
    /// legacy multi-map probe is still emitted.
    bool linear32_check_map{false};

    /// UBFX width for the check-map index: log2(reservation) - host page
    /// shift, so the index cannot leave the map.
    u8 linear32_check_index_bits{0};

    // Opt-in x64 load-only guest-base path. The reservation is power-of-two;
    // addresses outside it branch to the existing checked helper path.
    bool linear64{false};

    u8 linear64_address_bits{0};

    bool native_ud2_enabled{false};

    bool forward_join_state_enabled{false};

    // Experimental native guest CALL continuation ABI. Compilation feature,
    // not a runtime toggle: its value is captured by the compiler and is part
    // of the process-wide memo identity. Default OFF until CALL/RET lowering
    // can enter and leave the landing-pad frame as one complete protocol.
    bool native_callret_enabled{false};

    // Hot/cold split placement (jit_split_layout.hpp): a block's cold region
    // (memory slow paths, align / raise / UD2 stubs) is copied out of line
    // into the far end of a 32 KiB arena window, so the hot bodies a thread
    // compiles sit densely together. The block is laid out hot-first -- its
    // mid-block Jcc exits and shared continue exits move ahead of the cold
    // stubs -- every crossing branch is re-targeted, and a block the
    // relocator cannot place is placed contiguously. On unless
    // VITREO_JIT_SPLIT_COLD=0 (see the constructor).
    bool split_cold{false};

    // Guest-permission re-validation on top of the linear64 reservation.
    // REQUIRED for correctness in both of its shapes; which one is emitted
    // depends on m_linear64_hw_protect (see emit_linear64_permission_guard).
    //
    // Where GuestMemory hardware-enforces protection, a uniform 16 KiB host
    // page states the guest's 4 KiB protection exactly and the guard shrinks
    // to one byte load off the mixed-page map plus a CBNZ -- only a host page
    // whose guest pages disagree still needs the software check.
    //
    // Where it does not (VITREO_JIT_LINEAR64_HWPROT=0, a non-16 KiB host page,
    // or a failed map allocation), the reservation carries only
    // committed->RW, so it proves an address is mapped and never that the
    // guest may touch it, and the full eleven-instruction read/write slot
    // probe plus the LDAR-ed protect-generation compare has to run on every
    // access. That cost was the whole linear64 deficit: measured 2026-08-24,
    // software guard store 0.81x / load 0.94x against the region cache;
    // hardware enforcement 1.12x / 1.09x.
    //
    // VITREO_JIT_LINEAR64_PERMGUARD=0 removes whichever shape applies, for
    // measurement ONLY -- it is never a shippable mode.
    bool linear64_perm_guard{true};

    /// Bisection lever: VITREO_JIT_NO_LINEAR64_VEC128=1 keeps the 128-bit
    /// SSE/AVX memory forms on their split-scalar/checked path instead of
    /// emit_linear64_{load,store}128. Diagnostic only; default off.
    bool linear64_vec128{true};

    TsoMode tso_mode{TsoMode::Fence};

    // Cached host_supports_lrcpc(). Resolved once per compiler rather than
    // per emitted access: the gate reads an env var uncached by design, and
    // this sits in the guest-access emit path.
    bool tso_use_ldapr{false};

    // True when the process-wide exact-site fault handler is installed. A
    // failed installation keeps the pre-check diamond rather than exposing
    // x86-legal granule crossings to an unrecoverable host signal.
    bool tso_fault_patch{false};

    // With tso_fault_patch: a linear64 x64 scalar access of width 2..8 drops
    // its host-page crossing test and its patch marker, and takes the
    // ordered access's own alignment fault on a crossing instead
    // (linear_fault.hpp, kTsoCrossingVariantWords). Default on;
    // VITREO_JIT_TSO_CROSSING_FAULT=0 restores the inline test for A/B.
    bool tso_crossing_fault{false};

    // Hardware-protected linear64 with the check map ending at the VA limit
    // (GuestMemory::linear_check_map_ends_at_va_limit): the per-access
    // reservation test is dropped. The guard indexes the map with an
    // UNMASKED `EA >> 14`, so an EA at or above the reservation faults on the
    // map load and the fault handler redirects that site, once, to its range
    // variant (linear_fault.hpp, kRangeVariantWords). Default on;
    // VITREO_JIT_RANGE_FAULT=0 keeps the inline test.
    bool linear64_range_by_fault{false};

    // Dense translator Phase A ("Lever 2"): an x64 linear block keeps
    // GuestMemory::linear_jit_view_base() in X15 instead of the check map
    // (whose remaining probes reload it from JitContext), and reaches guest
    // memory through that protection-ENFORCING view. Requires hardware
    // protection, ordered crossing-by-fault and a registered view; turns
    // linear64_range_by_fault off, since that guard indexes X15 as the map.
    // Default off; VITREO_JIT_VIEW=1 enables it for A/B.
    bool linear64_jit_view{false};

    // Block-local write-through XMM register cache in V16-V30
    // (JitCompiler::m_xmm_cache). VITREO_JIT_XMM_CACHE=0 disables it.
    bool xmm_cache{false};
    // VITREO_JIT_PROBE_POISON_VREGS=1: TEST PROBE, not an optimisation.
    // Overwrite V16-V30 after every C++ helper call, as an AAPCS64 callee
    // may, so a differential test sees any code that relies on them
    // surviving one (the XMM cache must have reloaded or dropped them).
    bool probe_poison_vregs{false};

    // Order 128-bit guest accesses too. Default OFF: x86 grants no 16-byte
    // single-copy atomicity, so the decoration is contract-free cost (see the
    // constructor). VITREO_JIT_TSO_VECTOR=1 restores it for bisection.
    bool tso_vector{false};

    // True while m_tso_mode == Fence. Retained because the DMB call sites
    // read it directly.
    bool strict_tso{false};

    // Emit the SECOND (trailing) DMB on a 32-bit guest store in Fence mode.
    // Default OFF: x86-TSO needs store->store and older-load->store ordering,
    // both of which the LEADING barrier already supplies, and the only edge
    // the trailing barrier adds is store->load -- the one reordering x86
    // explicitly permits (SDM Vol. 3A 9.2.2/9.2.3). See emit_tso_barrier().
    // VITREO_JIT_TSO_STORE_TAIL=1 restores it for bisection.
    bool tso_store_tail_fence{false};

    // Block skip list: addresses that should NOT be JIT-compiled (for debugging).
    // Set via VITREO_JIT_SKIP=0x00404e86,0x00404f6f,...
    std::vector<GuestAddr> jit_skip;

    // Range-based skip: skip all blocks with addr >= m_jit_skip_above
    // Set via VITREO_JIT_SKIP_ABOVE=0x00406800
    GuestAddr jit_skip_above{0};

    // Range-based skip: skip all blocks with addr < m_jit_skip_below
    // Set via VITREO_JIT_SKIP_BELOW=0x00406800
    GuestAddr jit_skip_below{0};

    // Closed-open intervals [from, to): interpret only those guest ranges.
    // Set via VITREO_JIT_SKIP_RANGE=0x0042b000-0x0042d000[,0x00482800-0x00484200...]
    static constexpr usize kMaxJitSkipRanges = 8;
    GuestAddr jit_skip_range_from[kMaxJitSkipRanges]{};

    GuestAddr jit_skip_range_to[kMaxJitSkipRanges]{};

    u8 jit_skip_range_count{0};

    // True when GuestMemory hardware-enforces guest protection inside the
    // reservation, which is what lets a direct access skip the permission
    // probe entirely on a uniform host page.
    bool linear64_hw_protect{false};

    /// Render the whole configuration, one `key=value` per line. A measurement
    /// that does not record this is a measurement nobody can reproduce.
    [[nodiscard]] std::string describe() const;
};

}  // namespace vitreo::cpu::jit

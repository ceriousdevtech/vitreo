// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: guest memory model selection (identity, 32-bit linear, x64 linear window)

#include "run_internal.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "core/cpu/jit/linear_fault.hpp"
#include "core/memory/early_low_reservation.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/memory/x64_high_arena.hpp"
#include "core/pe/parser/pe_parser.hpp"

#include <cstdio>

namespace vitreo::cli {

void configure_guest_memory_model(
    GuestMemory& memory,
    cpu::jit::LinearReservationRegistration& linear_fault_registration,
    const pe::ParsedPe& pe) {
#if defined(VITREO_ARCH_X86_64)
    // Identity mapping (host address == guest address) for the native x86_64
    // backend: natively executed guest code emits absolute addresses that the
    // CPU consumes directly, so the guest must be mapped where it believes it
    // is. Translated backends can add a base; this one cannot.
    // VITREO_NO_NATIVE32=1 falls back to the translated linear model.
    if (!environment_flag_enabled("VITREO_NO_NATIVE32", false)) {
        if (memory.enable_identity_mapping()) {
            // Record what the host process already owns BEFORE any guest
            // mapping. The scan cannot distinguish a host occupant from a
            // region Vitreo mapped itself, so it is only truthful against a
            // pristine address space — and the guest heap/stack/image all get
            // placed out of these ranges from here on.
            // Prefer the early reservation's holes when we have them: they
            // were measured before the host could allocate anything low, so
            // they are the true host footprint. Re-scanning here would instead
            // see our own reservation and the guest's own committed memory.
            usize occupied = 0;
            if (vitreo::memory::low_reservation_active()) {
                for (const auto& [lo, hi] :
                         vitreo::memory::low_reservation_holes()) {
                    memory.add_reserved_hole(lo, hi);
                    ++occupied;
                }
            } else {
                occupied = memory.capture_host_occupancy_below_4gb();
            }
            // Then claim everything that is left, before any host allocation
            // (a lazily-loaded dylib, a new pthread stack) can land in the
            // guest's address space.
            // Only reserve here if the early claim did not already happen.
            // Running both makes the second pass fail on every range the first
            // already owns, and mark the guest's own space as host-owned.
            const usize claimed =
                vitreo::memory::low_reservation_active()
                    ? vitreo::memory::reserve_low_guest_space()  // idempotent
                    : memory.reserve_identity_space_below_4gb();
            std::fprintf(stderr,
                         "[native32] identity mapping ENABLED "
                         "(%zu host ranges below 4GB reserved, "
                         "%llu MiB claimed for the guest)\n",
                         occupied,
                         static_cast<unsigned long long>(claimed >> 20));
            for (const auto& [lo, hi] : memory.reserved_holes()) {
                std::fprintf(stderr,
                             "[native32] reserved 0x%llx-0x%llx (%llu KiB)\n",
                             static_cast<unsigned long long>(lo),
                             static_cast<unsigned long long>(hi),
                             static_cast<unsigned long long>(hi - lo) / 1024);
            }
        } else {
            std::fprintf(stderr,
                         "[native32] identity mapping unavailable; "
                         "using the translated memory model\n");
        }
    }
#endif

    // Production guest-base (linear) memory model for 32-bit guests. It must be
    // enabled before any region is mapped. VITREO_JIT_LINEAR=0 selects the
    // scattered region model for compatibility bisection. Identity mapping is
    // a linear model with a base of zero and excludes this one.
    if (!memory.is_identity() && !pe.is_64bit &&
        environment_flag_enabled("VITREO_JIT_LINEAR", true)) {
        constexpr usize k4GiB = usize{4} * 1024 * 1024 * 1024;
        if (memory.enable_linear_reservation(k4GiB)) {
            // Apply real host protections to guest pages, as the x64 model
            // already does. Without it every committed page is mapped RW and
            // the JIT's inline linear store — which tests only the check byte,
            // and the check byte carries no protection — writes straight
            // through a read-only guest page that the interpreter refuses.
            // Measured divergence, 2026-09-03.
            //
            // DEFAULT OFF. Enabling it is a correctness improvement in
            // principle — without it the JIT's inline store writes straight
            // through a read-only guest page that the interpreter refuses —
            // but it demonstrably makes a real guest worse.
            //
            // Measured 2026-09-03: with it ON the guest takes
            // three unhandled access violations whose faulting addresses are
            // 0x311c309d, 0x311c3101, 0x311c3165 — a HOST pointer
            // (linear_base 0x300000000 plus a guest offset) walking through
            // guest memory — plus two "callback stack not writable" failures.
            // With it OFF there is one AV and the guest gets further, reaching
            // CreateCubeTexture. Disabling the ESP-frame cache does not change
            // it, so X15 is not the leak.
            //
            // No guest was ever observed depending on the permissive
            // behaviour, so this trade is: a theoretical improvement with no
            // known beneficiary against a measured regression in a real title.
            // The machinery, the knob and the tests stay; only the default
            // moves. VITREO_JIT_LINEAR32_HWPROT=1 re-enables it, which is how
            // to reproduce the leak.
            if (environment_flag_enabled("VITREO_JIT_LINEAR32_HWPROT", false)) {
                if (memory.enable_linear_hardware_protection()) {
                    std::fprintf(stderr,
                                 "[linear] guest protection is HARDWARE "
                                 "enforced inside the reservation\n");
                } else {
                    std::fprintf(stderr,
                                 "[linear] WARNING: hardware protection could "
                                 "not be enabled; guest page protection is "
                                 "NOT enforced on the JIT store fast path\n");
                }
            }
            if (linear_fault_registration.acquire(memory.linear_base(), k4GiB)) {
                std::fprintf(stderr, "[linear] guest-base memory ENABLED "
                             "(base=%p, 4 GiB)\n",
                             static_cast<void*>(memory.linear_base()));
            } else {
                std::fprintf(stderr,
                             "[linear] recoverable fault registration failed; "
                             "using checked JIT memory paths\n");
            }
        } else {
            std::fprintf(stderr, "[linear] reservation failed; using scattered model\n");
        }
    }

    // Production scalar guest-base model for x64. A power-of-two reservation
    // lets the JIT reject an out-of-window EA with a shift+branch and perform
    // an in-window scalar load or guarded store directly from X14+EA; guest
    // protection is carried by the reservation in hardware.
    // Unset selects the hardware-enforced fast path when the host supports it.
    // VITREO_JIT_LINEAR64=0 selects the scattered region model for
    // compatibility bisection; explicit =1 also permits the slower software-
    // guarded linear model when hardware protection is unavailable.
    if (!memory.is_identity() && pe.is_64bit &&
        cpu::jit::linear64_reservation_requested()) {
        usize reserved = 0;
        // VITREO_JIT_VIEW=1 (dense translator Phase A): the JIT view maps the
        // window a second time, and two 2^46 windows do not fit in the host's
        // user address space, so the window starts at 2^45. The guest-visible
        // address space is unchanged either way (see below).
        const bool want_jit_view =
            environment_flag_enabled("VITREO_JIT_VIEW", false);
        for (const usize candidate : {
                 usize{1} << 46, usize{1} << 45, usize{1} << 44,
                 usize{1} << 42, usize{1} << 40, usize{1} << 39}) {
            if (want_jit_view && candidate > kLinearJitViewMaxWindow) continue;
            if (!want_jit_view && candidate == (usize{1} << 45)) continue;
            // The JIT view mirrors the window, so it leaves no host VA for
            // the extension above the core.
            if (memory.enable_linear_reservation(candidate,
                                                 /*extend_to_va_limit=*/!want_jit_view)) {
                reserved = candidate;
                break;
            }
        }
        if (reserved != 0) {
            // Deliberately NOT set_maximum_user_address(reserved - 1): the
            // guest-visible Windows x64 user space stays 128 TiB
            // (lpMaximumApplicationAddress 0x7FFF'FFFEFFFF). The reservation
            // is `reserved` (the power-of-two core) plus the extension up to
            // the check map at the VA limit, memory.linear_size() in all.
            // Allocations above that fall back to the scattered model per
            // GuestMemory::linear_backs(), and the JIT bounds check sends
            // every access to them through the checked helper.
            //
            // Vitreo's OWN high arenas -- where it relocates DLLs, its thunk
            // gateways and its synthetic system modules -- are placed inside
            // the window instead. Windows assigns no contract to where a
            // relocated DLL loads, and outside the window every access to a
            // DLL's globals, vtables and constants took the checked helper:
            // on a 64-bit title's main thread that was ~11% of busy time
            // (2026-10-05). The earlier capped-VA experiment that also moved
            // DLLs to 0x3ffa... was blamed for a browser-guest wedge, but that crash
            // loop was later traced to linear64 JIT wrong-results
            // (.docs/design-linear64-memory.md, 2026-08-27 addenda), not to the
            // address shape. VITREO_X64_HIGH_ARENA_IN_WINDOW=0 restores the
            // host-probed 0x7FFA placement.
            if (environment_flag_enabled("VITREO_X64_HIGH_ARENA_IN_WINDOW",
                                         true)) {
                ::vitreo::memory::set_x64_high_arena_window(reserved);
            }
            // Make the reservation carry the guest's protections in hardware.
            // Without this the reservation only proves an address is mapped,
            // and every direct access has to re-check permission in software.
            // VITREO_JIT_LINEAR64_HWPROT=0 keeps the software-only model for
            // bisection.
            if (environment_flag_enabled("VITREO_JIT_LINEAR64_HWPROT", true)) {
                if (memory.enable_linear_hardware_protection()) {
                    std::fprintf(stderr,
                                 "[linear64] guest protection is HARDWARE "
                                 "enforced inside the reservation\n");
                    // Before any mapping, like the two calls above.
                    if (want_jit_view && !memory.enable_linear_jit_view()) {
                        std::fprintf(stderr,
                                     "[linear64] JIT view unavailable; "
                                     "check-map probes stay\n");
                    }
                } else {
                    if (cpu::jit::linear64_fast_path_requested(false)) {
                        std::fprintf(stderr,
                                     "[linear64] hardware protection unavailable; "
                                     "explicit software permission guard enabled\n");
                    } else {
                        std::fprintf(stderr,
                                     "[linear64] hardware protection unavailable; "
                                     "using region JIT\n");
                    }
                }
            } else if (cpu::jit::linear64_fast_path_requested(false)) {
                std::fprintf(stderr,
                             "[linear64] hardware protection explicitly disabled; "
                             "software permission guard enabled\n");
            } else {
                std::fprintf(stderr,
                             "[linear64] hardware protection explicitly disabled; "
                             "using region JIT\n");
            }
            if (linear_fault_registration.acquire(memory.linear_base(),
                                                  memory.linear_size())) {
                if (memory.linear_jit_view_base() != nullptr) {
                    if (linear_fault_registration.acquire_jit_view(
                            memory.linear_jit_view_base(), reserved)) {
                        std::fprintf(stderr,
                                     "[linear64] JIT view at %p (window 2^%d)\n",
                                     static_cast<void*>(
                                         memory.linear_jit_view_base()),
                                     __builtin_ctzll(reserved));
                    } else {
                        // Unregistered, the JIT cannot use it
                        // (linear_jit_view_registered): checked paths stay.
                        std::fprintf(stderr,
                                     "[linear64] JIT view registration failed; "
                                     "check-map probes stay\n");
                    }
                }
                if (cpu::jit::linear64_fast_path_requested(
                        memory.linear_hardware_protection())) {
                    std::fprintf(stderr,
                                 "[linear64] scalar memory fast path ENABLED "
                                 "(base=%p, window=%llu GiB, core=%llu GiB)\n",
                                 static_cast<void*>(memory.linear_base()),
                                 static_cast<unsigned long long>(
                                     memory.linear_size() >> 30),
                                 static_cast<unsigned long long>(reserved >> 30));
                } else {
                    std::fprintf(stderr,
                                 "[linear64] reservation registered; "
                                 "region JIT active\n");
                }
            } else {
                std::fprintf(stderr,
                             "[linear64] recoverable fault registration failed; "
                             "using checked JIT memory paths\n");
            }
        } else {
            std::fprintf(stderr,
                         "[linear64] reservation failed; using region cache\n");
        }
    }
}

} // namespace vitreo::cli

// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - shared CPUID synthesis

#pragma once

/// @file cpuid_synth.hpp
/// @brief The CPU identity every backend must present to the guest.
///
/// The interpreter synthesises a curated processor rather than exposing the
/// host's. Native execution runs the real `CPUID` instruction unless it is
/// intercepted, which hands the guest the host CPU instead — so the same guest
/// takes different code paths on the two backends and the Intel machine stops
/// being a controlled comparison against Apple Silicon.
///
/// Measured: guests size their memory pools from this. Natively it asked for
/// 1.69GB across seven blocks of 128-271MB and exhausted its 2GB non-large-
/// address-aware window; interpreted, the same build asked for 0.14GB with a
/// 31.5MB maximum and loaded.
///
/// Both paths call this, so they cannot drift.

#include <vitreo/types.hpp>

namespace vitreo::cpu::ops {

/// Evaluate CPUID for @p leaf / @p subleaf (EAX / ECX on entry) on the
/// logical processor the calling thread is running on.
/// Feature leaves describe the execution mode's implemented ISA contract;
/// identity, cache, topology and clock leaves describe the host machine
/// (platform::host_hardware()); unknown leaves return all zeros, as the
/// hardware does.
void synth_cpuid(u32 leaf, u32 subleaf, bool long_mode,
                 u32& eax, u32& ebx, u32& ecx, u32& edx) noexcept;

/// synth_cpuid as executed on logical processor @p processor: the per-processor
/// leaves (APIC IDs in 1/0xB/0x1F, the core's caches in 4/0x80000006) follow it.
void synth_cpuid_on(u32 processor, u32 leaf, u32 subleaf, bool long_mode,
                    u32& eax, u32& ebx, u32& ecx, u32& edx) noexcept;

} // namespace vitreo::cpu::ops

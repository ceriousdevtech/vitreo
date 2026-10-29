// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - CPUID interception for native x86_64 guest execution

#pragma once

/// @file cpuid_intercept.hpp
/// @brief Makes natively-executed guests see Vitreo's CPU, not the host's.
///
/// x86 `CPUID` is unprivileged and cannot be trapped from user mode — macOS
/// exposes no equivalent of Linux's `ARCH_SET_CPUID`. So the instruction is
/// rewritten in the guest's own code: `0F A2` (CPUID) becomes `0F 0B` (UD2),
/// which is the same two bytes, so nothing shifts and no relocation changes.
/// The Mach exception bridge catches the resulting EXC_BAD_INSTRUCTION,
/// evaluates the leaf with the shared synthesis, writes EAX/EBX/ECX/EDX into
/// the thread state and steps RIP past the two bytes.
///
/// **Why the guest must not see the host CPU.** Under the interpreter it sees
/// a curated processor; natively it would see whatever the Mac has. A guest
/// that probes the CPU then runs different code on each backend, which defeats
/// the Intel machine's entire purpose as a controlled comparison. Measured:
/// one guest asked for 1.69GB natively and 0.14GB interpreted, exhausting its
/// 2GB window in the first case.
///
/// **The scan is instruction-boundary aware.** A raw search for the byte pair
/// `0F A2` would also hit immediates, displacements and data embedded in an
/// executable section, and patching one of those corrupts the guest. The scan
/// therefore decodes forward with Vitreo's own decoder and only rewrites an
/// instruction the decoder identifies as CPUID.

#include <vitreo/types.hpp>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::cpu::x64 {

/// Rewrite every CPUID in [base, base+size) to UD2 and record the sites.
/// @return number of instructions patched.
usize intercept_cpuid_in_range(GuestMemory& memory, u32 base, u32 size);

/// True when @p addr is a recorded patch site, i.e. the UD2 there stands in
/// for a CPUID rather than being a genuine illegal instruction. The exception
/// bridge must not treat an unrecorded UD2 as ours.
[[nodiscard]] bool is_cpuid_patch_site(u32 addr) noexcept;

/// Number of sites patched so far. Diagnostics.
[[nodiscard]] usize cpuid_patch_count() noexcept;

} // namespace vitreo::cpu::x64

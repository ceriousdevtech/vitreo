// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - early claim on the 32-bit guest address space

#pragma once

/// @file early_low_reservation.hpp
/// @brief Claims the low 4GB for the guest before the host can take it.
///
/// Only meaningful on the native x86_64 backend, where the guest address IS
/// the host address and a 32-bit guest must live below 4GB.
///
/// **Why this has to run first.** Linking with `-pagezero_size` (required, or
/// nothing below 4GB is mappable at all) also makes the low 4GB ordinary
/// allocatable space for the *host*. dyld, malloc arenas, thread stacks and
/// Metal then help themselves to it during startup. Reserving after that is
/// too late: measured on the target machine, by the time the runtime reached
/// its guest setup the host had taken 0x100000-0x563000 and
/// 0x800000-0x2e76000 -- 43MB straddling exactly where 32-bit Windows images
/// live. A 32-bit image wants 0x400000 plus 13.7MB and has no relocation
/// table, so it cannot be placed at all.
///
/// A main EXE having no `.reloc` is normal, not defective: Windows guarantees
/// a fresh process's preferred base, so linkers routinely strip relocations
/// from executables. An Intel backend that cannot honour the preferred base
/// therefore cannot run a large fraction of real software, and no amount of
/// relocation cleverness fixes it. The address space has to be claimed first.
///
/// This is invoked from a constructor with priority 101. The `__GUEST`
/// placeholder already stopped malloc taking 0x400000; this call drops that
/// mapping and claims everything else still free. Calling it again from
/// main() is idempotent.

#include <vitreo/types.hpp>

#include <utility>
#include <vector>

namespace vitreo::memory {

/// Claim [kLowReserveStart, 4GB) for the guest, PROT_NONE.
///
/// Non-destructive: uses `mach_vm_allocate(VM_FLAGS_FIXED)`, which fails on an
/// occupied range rather than evicting its owner the way `mmap(MAP_FIXED)`
/// would. Whatever cannot be claimed is recorded as a hole.
///
/// Safe to call on any architecture; a no-op unless the native x86_64 backend
/// is compiled in. Idempotent.
///
/// @return bytes successfully claimed for the guest.
usize reserve_low_guest_space();

/// True once reserve_low_guest_space() has run and claimed anything.
[[nodiscard]] bool low_reservation_active() noexcept;

/// Ranges the host already owned, as sorted, non-overlapping [lo, hi).
/// These are authoritative: they were measured at the earliest possible
/// moment, so nothing that appears low afterwards is a genuine host claim --
/// it is the guest's own memory being committed inside our reservation.
[[nodiscard]] const std::vector<std::pair<GuestAddr, GuestAddr>>&
low_reservation_holes() noexcept;

/// The Mach-O `__GUEST` zerofill (`guest_reserve.s`) occupies this window
/// from process start so libmalloc cannot. `reserve_low_guest_space()` drops
/// it and re-claims the range PROT_NONE for the guest. Must match the
/// `.zerofill` size and `-Wl,-segaddr,__GUEST`.
inline constexpr GuestAddr kGuestReserveLo   = 0x100000;
inline constexpr usize     kGuestReserveSize = 0x7FF00000;

/// The first page is left unclaimed so a guest null dereference still faults
/// instead of hitting reserved memory.
inline constexpr GuestAddr kLowReserveStart = 0x10000;

/// `mach_vm_allocate` user_tag for guest identity reservations.
///
/// The kernel's default tag is `VM_MEMORY_MALLOC`. libmalloc also plants
/// `PROT_NONE` guard pages inside its tiny/small zones with *its* tags. A
/// walk that treats every `PROT_NONE` mapping as ours will hand those guards
/// to the guest; the next `VirtualAlloc` commit then `memset`s them and
/// SIGSEGVs. This tag is `VM_MEMORY_APPLICATION_SPECIFIC_1` (240).
inline constexpr unsigned kGuestIdentityVmTag = 240;

} // namespace vitreo::memory

// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: segment management

#pragma once

/// @file x64_segments.hpp
/// @brief LDT segment allocation for the native x86_64 backend.
///
/// Two unrelated-looking problems on macOS/x86_64 turn out to have the same
/// answer, and it is not the obvious one.
///
/// **Placing the guest TEB in GS.** Win64 code reads its TEB through GS
/// constantly (`mov rax, gs:[0x30]`), while macOS uses GS for pthread TLS.
/// Neither `thread_set_state(x86_THREAD_FULL_STATE64)` nor `wrgsbase` is
/// available: the first returns KERN_INVALID_ARGUMENT even on a suspended
/// thread, and the second raises #UD because macOS never sets CR4.FSGSBASE --
/// despite the CPU advertising `RDWRFSGS` in `machdep.cpu.leaf7_features`.
///
/// **Executing 32-bit guest code** (compatibility mode) needs a code segment
/// with L=0/D=1, which long mode cannot synthesise on its own.
///
/// Both are solved by `i386_set_ldt`, which is unrestricted on macOS 13 with
/// SIP enabled. In 64-bit mode DS/ES/SS/CS bases are ignored but FS and GS
/// bases are not, and a `MOV` to GS takes its base from the descriptor -- so
/// a segment whose base is the TEB makes `gs:[0x30]` resolve correctly, at
/// the cost of one instruction and no syscall.
///
/// Verified on the target hardware; see tools/intel-spikes/.

#include <vitreo/types.hpp>
#include <vitreo/result.hpp>

namespace vitreo::cpu::x64 {

/// Descriptor bases are 32 bits wide, so anything reached through a segment
/// must live below 4GB. GS-addressed guest TEBs therefore have a placement
/// constraint, not merely a preference.
inline constexpr u64 kSegmentBaseLimit = 0x100000000ULL;

/// Process-wide LDT allocations.
///
/// The LDT is per-process and `i386_set_ldt` entries are a finite shared
/// resource, so allocation is centralised rather than done ad hoc per thread.
class SegmentTable {
public:
    /// Allocate a flat 4GB data segment based at @p base, for loading into GS.
    ///
    /// @param base Host/guest address of the TEB. Must be < kSegmentBaseLimit.
    /// @return The selector, or Error::InvalidArgument if @p base is out of
    ///         range, Error::NotSupported if the kernel refused the descriptor.
    [[nodiscard]] static VResult<u16> allocate_data_segment(u64 base);

    /// Allocate the 32-bit code + data segment pair used to enter
    /// compatibility mode. Both are flat (base 0, 4GB limit).
    ///
    /// The data segment is not optional. Long mode leaves DS effectively null,
    /// which is harmless because 64-bit addressing ignores it; the instant
    /// compatibility mode begins every memory reference goes through DS again
    /// and a null DS raises #GP.
    [[nodiscard]] static VoidResult allocate_compat32_pair(u16* out_code_sel,
                                                           u16* out_data_sel);

    /// Number of descriptors handed out. Diagnostics only.
    [[nodiscard]] static usize allocated_count() noexcept;
};

// ---------------------------------------------------------------------------
// Segment register access
// ---------------------------------------------------------------------------

[[nodiscard]] u16 current_gs() noexcept;
[[nodiscard]] u16 current_ds() noexcept;
[[nodiscard]] u16 current_es() noexcept;
[[nodiscard]] u16 current_fs() noexcept;

void set_gs(u16 selector) noexcept;
void set_ds(u16 selector) noexcept;
void set_es(u16 selector) noexcept;
void set_fs(u16 selector) noexcept;

/// Point GS at a guest TEB segment.
///
/// ONE WAY ONLY. Loading a guest segment into GS is not reversible on macOS,
/// so this is deliberately not an RAII scope guard.
///
/// The host's GS selector is 0x0000 -- a null selector -- and its effective
/// base is set by the kernel through IA32_GS_BASE, not through any descriptor.
/// Reloading the selector (even the host's own, even as a no-op) resets the
/// base to the descriptor's, which is 0. That destroys the pthread TLS pointer
/// every libc call depends on, and there is no way to put it back: `wrgsbase`
/// raises #UD because macOS never sets CR4.FSGSBASE, and
/// `thread_set_state(x86_THREAD_FULL_STATE64)` returns KERN_INVALID_ARGUMENT.
/// Measured directly -- see tools/intel-spikes/gs_restore_spike.c.
///
/// Consequently a thread that has entered guest GS **can never safely run host
/// libc again**: not printf, not malloc, not errno, not a signal handler. The
/// failure is delayed and misleading -- it surfaces as a fault at a small
/// absolute address inside some unrelated libc call, long after the swap.
///
/// This is an unresolved design fork for the Win64 native path, not a settled
/// mechanism.
void enter_guest_gs(u16 guest_gs) noexcept;

/// Point FS at the guest TEB carrier used by native Win64 execution.
/// Unlike macOS's pthread GS base, an LDT-derived FS base is preserved across
/// kernel transitions on the supported Intel target.
void enter_guest_fs(u16 guest_fs) noexcept;

} // namespace vitreo::cpu::x64

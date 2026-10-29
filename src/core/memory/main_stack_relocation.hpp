// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - move the main thread stack out of the 32-bit guest address space

#pragma once

/// @file main_stack_relocation.hpp
/// @brief Relocates the main thread stack above 4GB on the Intel backend.
///
/// **The problem.** Under identity mapping a 32-bit guest must live below 4GB,
/// at the addresses its image actually names. macOS places the main thread
/// stack at 0x800000-0x1800000 once `__PAGEZERO` is shrunk -- 16MB sitting
/// directly on top of where 32-bit Windows images load. A classic
/// `0x400000`-based image can therefore be at most 4MB before it collides,
/// which excludes essentially every real 32-bit image. One measured image is
/// 13.7MB, and others are similar.
///
/// Rebasing the image is not an option: a main EXE normally has no `.reloc`,
/// because Windows guarantees the preferred base in a fresh process, so
/// linkers strip relocations from executables as a matter of course.
///
/// **Why the linker cannot do it.** `-stack_addr` is rejected outright for
/// modern executables. `-stack_size` is accepted and silently changes nothing:
/// measured, the reserved region stayed at exactly 0x800000-0x1800000
/// regardless of the size requested.
///
/// **So the process moves its own stack.** At the very top of `main()`, before
/// anything meaningful exists on the stack, switch RSP to a fresh high
/// allocation and unmap the original. This is a well-worn technique in
/// emulators for exactly this reason, and it is safe *only* as the first thing
/// main does: every pointer into the old stack dies with it.

#include <vitreo/types.hpp>

namespace vitreo::memory {

/// Run @p body on a stack allocated above 4GB, having released the original.
///
/// Never returns to the caller: the old stack is gone by the time @p body
/// runs, so there is nothing to return to. Exits the process with @p body's
/// return value.
///
/// On ARM64, or if relocation is not possible, @p body is simply called on the
/// existing stack -- the guest address space is translated there, so nothing
/// is competing for it.
[[noreturn]] void run_main_on_high_stack(int argc, char** argv,
                                         int (*body)(int, char**));

/// Size of the relocated stack. Matches the macOS default so nothing that
/// worked before runs out of room.
inline constexpr usize kHighStackSize = 8u << 20;

/// Guard page below the new stack, so an overflow faults rather than running
/// into whatever precedes the allocation.
inline constexpr usize kPageGuard = 16u << 10;

} // namespace vitreo::memory

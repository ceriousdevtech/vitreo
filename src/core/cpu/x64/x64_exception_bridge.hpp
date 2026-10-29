// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Mach exception bridge for native x86_64 guest execution

#pragma once

/// @file x64_exception_bridge.hpp
/// @brief Catch CPU exceptions taken in 32-bit compatibility mode.
///
/// POSIX signals cannot be used: while guest segments are loaded, the kernel
/// cannot deliver a signal into the thread and kills the process instead.
/// Mach exception ports run on a dedicated handler thread, so the faulting
/// thread's segment state is irrelevant to delivery.
///
/// A guest fault (CS is the compatibility-mode selector) is converted into
/// a Windows NTSTATUS, the thread is redirected to the trampoline's exit32
/// stub, and Compat32Executor::run() returns Compat32Exit::Fault.
/// A host fault (CS is the 64-bit kernel selector) is not handled -- the
/// reply is KERN_FAILURE so the process crashes honestly.

#include "core/cpu/x64/compat32.hpp"

#include <vitreo/result.hpp>

namespace vitreo::cpu::x64 {

class X64ExceptionBridge {
public:
    /// Spawn the handler thread and allocate the receive port. Idempotent.
    [[nodiscard]] static VoidResult start();

    /// Bind the executor that belongs to the calling thread.
    ///
    /// The Mach handler runs on a different thread and cannot use
    /// `for_current_thread()`. A guest fault looks up the faulting thread's
    /// executor here. Pass nullptr to unbind this thread.
    static void bind_executor(Compat32Executor* exec) noexcept;

    /// Route this thread's CPU exceptions to the process handler port.
    /// Each host thread that enters 32-bit guest code must call this once.
    [[nodiscard]] static VoidResult install_on_current_thread();

    /// Register a native64 guest thread and its TEB.
    ///
    /// Two jobs. The handler thread has no other way to find the transfer
    /// block, and a native64 fault is uninterpretable without it: the faulting
    /// registers alone cannot distinguish "the register file was loaded
    /// wrong" from "the guest ran and then jumped somewhere bad".
    ///
    /// It also marks the thread as one that CANNOT receive a signal. Guest
    /// threads block the synchronous fault signals, because delivering one
    /// nulls GS irrecoverably; the cost is that a fault this bridge declines
    /// would otherwise be retried by the kernel forever. Declining on a
    /// registered thread therefore terminates deliberately instead.
    static void register_native64_thread(u32 mach_port, u64 teb_base,
                                        u16 fs_selector) noexcept;
    static void unregister_native64_thread(u32 mach_port) noexcept;

    /// Compatibility-mode CS selector. Shared by every executor (process-wide
    /// LDT pair). 0 if none has been bound yet.
    [[nodiscard]] static u16 guest_cs() noexcept;
};

} // namespace vitreo::cpu::x64

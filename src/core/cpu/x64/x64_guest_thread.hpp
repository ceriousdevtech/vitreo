// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: GS-swapped guest execution threads

#pragma once

/// @file x64_guest_thread.hpp
/// @brief A thread that owns guest GS, and the libc-free handoff it uses to
///        reach host code.
///
/// The constraint being worked around: Win64 code reads its TEB through GS,
/// macOS x86_64 keeps pthread TLS there, and the swap is **one way**. Loading
/// a guest selector into GS destroys the host's TLS pointer and nothing can
/// put it back -- `wrgsbase` raises #UD and `thread_set_state(FULL_STATE64)`
/// returns KERN_INVALID_ARGUMENT. Guarded by
/// `X64SegmentsTest.HostGsBaseCannotBeRestored`.
///
/// So a thread does not swap back. It stops being a host thread. Guest code
/// runs on threads that own guest GS permanently, and every time such a thread
/// needs host C++ -- a WinAPI thunk, a fault to dispatch -- it hands the work
/// to a paired host thread through `GuestHostHop` and waits. The host thread
/// still has its TLS, so the entire existing thunk and marshalling layer runs
/// there unchanged, which is what keeps one implementation of each Windows API
/// serving both backends.
///
/// **Everything reached from a guest-side body must be libc-free.** Not
/// malloc, not printf, not errno, not a C++ exception, not a thread_local, not
/// a signal handler. The failure mode is not a clean crash: it is a fault at a
/// small absolute address inside unrelated libc code, arbitrarily far from the
/// call that broke the rule.

#include <vitreo/result.hpp>
#include <vitreo/types.hpp>

#include <atomic>

namespace vitreo::cpu::x64 {

/// Rendezvous between a GS-swapped guest thread and its host servicer.
///
/// Deliberately built from nothing but `std::atomic` on a plain object: atomic
/// loads and stores are instructions the compiler emits inline, with no call
/// into libc and no TLS access, so the guest side of this handshake is legal
/// after the GS swap. A mutex or condition variable would not be.
///
/// Each side parks with Darwin's raw ulock syscall while the other owns the
/// handoff. Calling the libc wrapper is not legal after the segment swap: its
/// error path writes errno through host TLS, which the guest no longer owns.
class GuestHostHop {
public:
    /// Guest side: publish a service request and block until the host has
    /// finished it. Libc-free.
    void guest_request() noexcept;

    /// Guest side: publish that the guest body has run to completion. Releases
    /// a host thread parked in host_wait(). Libc-free.
    void guest_finish() noexcept;

    /// Outcome of a host-side wait.
    enum class Wait { Request, Finished, TimedOut };

    /// Host side: block until the guest asks for service or finishes.
    ///
    /// @param timeout_ns 0 waits forever. A bounded wait matters because a
    ///        guest thread cannot be signalled -- signals are blocked on it
    ///        for its whole life -- so a wedged guest is otherwise invisible
    ///        and unrecoverable.
    [[nodiscard]] Wait host_wait(u64 timeout_ns = 0) noexcept;

    /// Host side: release the guest thread waiting in guest_request().
    void host_complete() noexcept;

    /// Requests serviced so far. Diagnostics and tests.
    [[nodiscard]] u64 serviced() const noexcept {
        return m_serviced.load(std::memory_order_relaxed);
    }

private:
    enum : u32 { kIdle = 0, kRequested = 1, kFinished = 2 };
    std::atomic<u32> m_state{kIdle};
    std::atomic<u64> m_serviced{0};
};

/// Body run on the guest thread after GS has been swapped. Libc-free.
using GuestTebBody = void (*)(GuestHostHop& hop, void* user);

/// A thread whose GS holds @p teb_base for the rest of its life.
///
/// start() performs every step that still needs libc -- descriptor
/// allocation, signal masking, thread creation -- before the swap, then hands
/// control to @p body. The thread never returns to host code and never runs a
/// pthread cleanup handler; it terminates through a raw syscall.
///
/// @param teb_base Guest TEB address. Must be below `kSegmentBaseLimit`:
///                 LDT descriptor bases are 32 bits wide.
class GuestTebThread {
public:
    GuestTebThread() = default;
    GuestTebThread(const GuestTebThread&) = delete;
    GuestTebThread& operator=(const GuestTebThread&) = delete;

    /// Create the thread and run @p body on it under guest GS.
    /// Returns Error::InvalidArgument if @p teb_base is out of segment range,
    /// Error::NotSupported if the kernel refused the descriptor, and
    /// Error::OutOfMemory if the thread could not be created.
    [[nodiscard]] VoidResult start(u64 teb_base, GuestTebBody body, void* user);

    /// Service requests until the guest body finishes. Runs on the calling
    /// (host) thread, which keeps its TLS and may use libc freely.
    ///
    /// @param servicer  Invoked once per guest request.
    /// @param timeout_ns Per-wait budget; 0 waits forever.
    /// @return false if a wait timed out, leaving the guest thread wedged.
    template <typename Fn>
    bool serve(Fn&& servicer, u64 timeout_ns = 0) {
        for (;;) {
            switch (m_hop.host_wait(timeout_ns)) {
                case GuestHostHop::Wait::Request:
                    servicer();
                    m_hop.host_complete();
                    break;
                case GuestHostHop::Wait::Finished: return true;
                case GuestHostHop::Wait::TimedOut: return false;
            }
        }
    }

    [[nodiscard]] GuestHostHop& hop() noexcept { return m_hop; }

    /// Block until the guest thread has left all guest code.
    ///
    /// serve() returns as soon as the guest *publishes* that its body
    /// finished, which is several instructions before the thread is actually
    /// gone. Anything that tears down the TEB -- and the TEB is where the
    /// thread's own transfer block lives -- must wait for this instead, or it
    /// unmaps memory a live thread is still reading. Past this point the
    /// thread touches nothing but its own pthread stack on the way to the
    /// terminate syscall.
    ///
    /// @return false if the thread did not exit within the timeout.
    [[nodiscard]] bool wait_until_exited(u64 timeout_ns = 1'000'000'000ULL) noexcept;

    /// Selector installed in the guest thread's GS. Diagnostics and tests.
    [[nodiscard]] u16 teb_fs_selector() const noexcept { return m_fs; }

    /// Mach port of the guest thread, captured before the GS swap.
    /// A fault report naming a different port is a fault on a different
    /// thread, which no amount of staring at the transfer block will explain.
    [[nodiscard]] u32 mach_port() const noexcept {
        return m_mach_port.load(std::memory_order_acquire);
    }

private:
    GuestHostHop m_hop{};
    std::atomic<bool> m_exited{false};
    std::atomic<u32> m_mach_port{0};
    u16 m_fs{0};
};

} // namespace vitreo::cpu::x64

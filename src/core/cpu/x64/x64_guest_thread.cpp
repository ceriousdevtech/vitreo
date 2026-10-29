// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - x86_64 native backend: GS-swapped guest execution threads

#include "core/cpu/x64/x64_guest_thread.hpp"

#include "core/cpu/x64/x64_segments.hpp"
#include "core/cpu/x64/x64_exception_bridge.hpp"

#include <mach/mach.h>
#include <pthread.h>
#include <chrono>
#include <csignal>
#include <cstdio>

namespace vitreo::cpu::x64 {

namespace {

/// CPU hint for a spin-wait. Not a libc call -- one instruction.
inline void cpu_relax() noexcept { __asm__ volatile("pause" ::: "memory"); }

/// Darwin's 32-bit compare-and-wait ulock operation. These are raw syscalls,
/// not the libsystem wrappers: a raced wait normally returns EBUSY, and the
/// wrapper's errno path would touch host TLS through the guest's segment.
///
/// On x86_64 Darwin the fourth syscall argument is moved from the C ABI's RCX
/// to R10 before `syscall` (which itself clobbers RCX). The public kernel
/// contract assigns 515/516 to ulock_wait/ulock_wake and operation 1 to a
/// 32-bit compare-and-wait.
constexpr u64 kDarwinBsdSyscallBase = 0x2000000ULL;
constexpr u64 kUlockWaitSyscall = kDarwinBsdSyscallBase + 515;
constexpr u64 kUlockWakeSyscall = kDarwinBsdSyscallBase + 516;
constexpr u32 kUlockCompareAndWait = 1;

inline void raw_ulock_wait(std::atomic<u32>* address, u32 expected,
                           u32 timeout_us = 0) noexcept {
    register u64 number __asm__("rax") = kUlockWaitSyscall;
    register u64 syscall_timeout __asm__("r10") = timeout_us;
    __asm__ volatile(
        "syscall"
        : "+a"(number)
        : "D"(static_cast<u64>(kUlockCompareAndWait)),
          "S"(address),
          "d"(static_cast<u64>(expected)),
          "r"(syscall_timeout)
        : "rcx", "r11", "cc", "memory");
}

inline void raw_ulock_wake(std::atomic<u32>* address) noexcept {
    register u64 number __asm__("rax") = kUlockWakeSyscall;
    __asm__ volatile(
        "syscall"
        : "+a"(number)
        : "D"(static_cast<u64>(kUlockCompareAndWait)),
          "S"(address),
          "d"(0ULL)
        : "rcx", "r11", "cc", "memory");
}

/// Terminate the calling thread without running any pthread cleanup.
///
/// A GS-swapped thread cannot return from its start routine: pthread's exit
/// path walks TLS destructors and touches the TSD block through GS, which no
/// longer points at it. `__bsdthread_terminate` is the syscall underneath
/// pthread_exit; invoked directly it drops the thread with no userspace
/// cleanup. Passing a null stack means "do not free the stack", so the thread
/// stack leaks -- correct here, because pthread's own bookkeeping for this
/// thread is unreachable anyway.
///
/// If the syscall returns (it should not), park rather than fall back into
/// host code that would immediately fault on TLS.
[[noreturn]] void terminate_guest_thread() noexcept {
    constexpr long kBsdThreadTerminate = 0x2000000 + 361;
    __asm__ volatile(
        "movq $0, %%rdi\n\t"
        "movq $0, %%rsi\n\t"
        "movq $0, %%rdx\n\t"
        "movq $0, %%rcx\n\t"
        "movq %0, %%rax\n\t"
        "syscall\n\t"
        :
        : "i"(kBsdThreadTerminate)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory");
    for (;;) cpu_relax();
}

struct ThreadStart {
    GuestHostHop* hop;
    GuestTebBody body;
    void* user;
    std::atomic<bool>* exited;
    std::atomic<u32>* mach_port;
    u64 teb;
    u16 fs;
};

/// Runs on the new thread. Everything before enter_guest_gs() may use libc;
/// nothing after it may.
void* guest_thread_main(void* arg) {
    // Copy the launch record out of the caller's frame before the swap: after
    // it, even reading through a pointer is fine, but the caller may have
    // returned and reused its stack.
    auto* record = static_cast<ThreadStart*>(arg);
    ThreadStart start = *record;
    delete record;  // still a host thread here; after the swap this is illegal

    start.mach_port->store(static_cast<u32>(mach_thread_self()),
                           std::memory_order_release);

    // Route CPU exceptions to the Mach handler port. Mach exception delivery
    // runs on a separate thread and does not depend on this one's segment
    // state or signal mask, which is the only reason a fault here is
    // observable at all.
    (void)X64ExceptionBridge::install_on_current_thread();
    X64ExceptionBridge::register_native64_thread(
        static_cast<u32>(mach_thread_self()), start.teb, start.fs);

    // Block EVERY blockable signal, including the synchronous fault ones.
    //
    // This is not defensive tidiness, it is the load-bearing rule of the whole
    // design: delivering any signal to this thread resets GS to the null
    // selector, and per x64_segments.hpp nothing can restore the base. The
    // damage is silent -- with GS null the entry stub reads its register file
    // out of low memory instead of the TEB, loads all zeros, and jumps to
    // address 0, which looks exactly like the guest dereferencing a null
    // pointer.
    //
    // Mach exceptions are delivered BEFORE signals and do not depend on the
    // signal mask, so the bridge installed above remains the sole handler.
    // Blocking a hardware fault signal would normally make the kernel retry
    // the faulting instruction forever; that is why the bridge terminates
    // deliberately when it declines a fault on a registered guest thread,
    // rather than returning false into an invisible spin.
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, nullptr);

    // Point of no return: from here the thread is not a host thread.
    enter_guest_fs(start.fs);

    start.body(*start.hop, start.user);
    start.hop->guest_finish();

    // Last observable act before leaving. A waiter released here knows the
    // thread will touch nothing but its own pthread stack from now on, so the
    // TEB -- which holds the transfer block -- is safe to tear down.
    start.exited->store(true, std::memory_order_release);

    terminate_guest_thread();
}

} // namespace

// ---------------------------------------------------------------------------
// GuestHostHop
// ---------------------------------------------------------------------------

void GuestHostHop::guest_request() noexcept {
    m_state.store(kRequested, std::memory_order_release);
    raw_ulock_wake(&m_state);
    while (m_state.load(std::memory_order_acquire) == kRequested) {
        raw_ulock_wait(&m_state, kRequested);
    }
}

void GuestHostHop::guest_finish() noexcept {
    m_state.store(kFinished, std::memory_order_release);
    raw_ulock_wake(&m_state);
}

GuestHostHop::Wait GuestHostHop::host_wait(u64 timeout_ns) noexcept {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        const u32 s = m_state.load(std::memory_order_acquire);
        if (s == kRequested) return Wait::Request;
        if (s == kFinished) return Wait::Finished;
        u32 wait_us = 0;
        if (timeout_ns != 0) {
            const auto elapsed = std::chrono::duration_cast<
                std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - start).count();
            if (static_cast<u64>(elapsed) >= timeout_ns) return Wait::TimedOut;
            const u64 remaining_ns = timeout_ns - static_cast<u64>(elapsed);
            const u64 remaining_us = remaining_ns / 1'000ULL +
                                     (remaining_ns % 1'000ULL != 0);
            wait_us = remaining_us > 0xFFFF'FFFFULL
                ? 0xFFFF'FFFFU
                : static_cast<u32>(remaining_us);
        }
        raw_ulock_wait(&m_state, kIdle, wait_us);
    }
}

void GuestHostHop::host_complete() noexcept {
    m_serviced.fetch_add(1, std::memory_order_relaxed);
    m_state.store(kIdle, std::memory_order_release);
    raw_ulock_wake(&m_state);
}

// ---------------------------------------------------------------------------
// GuestTebThread
// ---------------------------------------------------------------------------

VoidResult GuestTebThread::start(u64 teb_base, GuestTebBody body, void* user) {
    if (body == nullptr) return VoidResult::err(Error::InvalidArgument);

    auto sel = SegmentTable::allocate_data_segment(teb_base);
    if (sel.is_err()) return VoidResult::err(sel.error());
    m_fs = sel.value();

    // Owned by the new thread, which copies and frees it while it is still a
    // host thread. A stack object here would race with the caller returning.
    auto* record =
        new ThreadStart{&m_hop, body, user, &m_exited, &m_mach_port,
                        teb_base, m_fs};

    pthread_t tid{};
    if (pthread_create(&tid, nullptr, &guest_thread_main, record) != 0) {
        delete record;
        std::fprintf(stderr, "[native64] guest thread creation failed\n");
        return VoidResult::err(Error::OutOfMemory);
    }
    pthread_detach(tid);
    return VoidResult::ok();
}

bool GuestTebThread::wait_until_exited(u64 timeout_ns) noexcept {
    const auto start = std::chrono::steady_clock::now();
    while (!m_exited.load(std::memory_order_acquire)) {
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count();
        if (static_cast<u64>(elapsed) >= timeout_ns) return false;
        cpu_relax();
    }
    return true;
}

} // namespace vitreo::cpu::x64

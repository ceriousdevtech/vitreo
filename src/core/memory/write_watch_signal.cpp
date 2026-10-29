// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS

#include "core/memory/write_watch_signal.hpp"

#include "core/memory/write_watch_traps.hpp"
#include "core/memory/guard_page_traps.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>

namespace vitreo::memory {

namespace {

struct sigaction g_prev_segv;
struct sigaction g_prev_bus;
std::atomic<bool> g_installed{false};

void chain_to_prev(int sig, siginfo_t* info, void* uc) {
    const struct sigaction& prev = (sig == SIGBUS) ? g_prev_bus : g_prev_segv;
    if ((prev.sa_flags & SA_SIGINFO) != 0) {
        if (prev.sa_sigaction != nullptr) {
            prev.sa_sigaction(sig, info, uc);
            return;
        }
    } else if (prev.sa_handler == SIG_IGN) {
        return;
    } else if (prev.sa_handler != nullptr && prev.sa_handler != SIG_DFL) {
        prev.sa_handler(sig);
        return;
    }
    // No usable previous handler — restore default and re-raise so we crash.
    signal(sig, SIG_DFL);
    raise(sig);
}

// Async-signal-safe: handle_fault only touches atomics, a static byte map and
// mprotect. Returning without touching the context retries the faulting store,
// which now finds the page writable.
void write_watch_signal_handler(int sig, siginfo_t* info, void* uc) {
    const auto fault = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t base = write_watch::host_base();
    if (fault >= base && fault - base < 0x100000000ull) {
        const auto guest = static_cast<GuestAddr>(fault - base);
        // PAGE_GUARD before write-watch: clear the one-shot, restore the base
        // protection, then retry. Guest SEH for STATUS_GUARD_PAGE_VIOLATION is
        // raised on the JIT linear-fault path (which sees the same armed bit);
        // host C++ retries here after the clear (kernel-style trip).
        if (guard_page::handle_fault(guest)) {
            return;
        }
        if (write_watch::hardware_traps() && write_watch::handle_fault(guest)) {
            return;
        }
    }
    chain_to_prev(sig, info, uc);
}

} // namespace

void install_write_watch_signal_handler() {
    bool expected = false;
    if (!g_installed.compare_exchange_strong(expected, true)) {
        return;
    }
    struct sigaction sa {};
    sa.sa_sigaction = write_watch_signal_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_prev_segv);
    sigaction(SIGBUS, &sa, &g_prev_bus);
}

} // namespace vitreo::memory

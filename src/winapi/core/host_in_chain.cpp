#include "winapi/core/host_in_chain.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

namespace vitreo::winapi::host_in_chain {

namespace detail {

std::atomic<int> g_enabled{-1};
std::atomic<int> g_force{0};

bool decide_enabled() noexcept {
    const char* value = std::getenv("VITREO_L3K");
    const bool requested = value == nullptr || std::strcmp(value, "0") != 0;
    const char* diagnostic = requested ? full_path_diagnostic() : nullptr;
    const bool on = requested && diagnostic == nullptr;
    int expected = -1;
    if (g_enabled.compare_exchange_strong(expected, on ? 1 : 0,
                                          std::memory_order_relaxed) &&
        !on) {
        // Off is not the shipping configuration: say so where a measurement
        // looks for proof that its env reached the guest.
        if (!requested) {
            std::fprintf(stderr,
                         "[l3k] VITREO_L3K=0: in-chain kernel32 exports OFF\n");
        } else {
            std::fprintf(stderr,
                         "[l3k] in-chain kernel32 exports off: %s\n",
                         diagnostic);
        }
    }
    return g_enabled.load(std::memory_order_relaxed) != 0;
}

}  // namespace detail

const char* full_path_diagnostic() noexcept {
    // The full path's own per-call diagnostics for registered handlers: the
    // slow-thunk histogram, the TEB last-error trace and the bad-caller
    // trace all live in dispatch_slow_api_thunk. With one of them on, an
    // in-chain-only entry would be declined on every call (the serve
    // refuses it so they keep seeing every call), so none is built and the
    // calls pay no stub call-out at all.
    for (const char* name : {"VITREO_SLOW_THUNK_HIST", "VITREO_TEB_TRACE",
                             "VITREO_TRACE_BAD_CALLER"}) {
        if (std::getenv(name) != nullptr) return name;
    }
    return nullptr;
}

void note_first_in_chain_call(u32 thread_id, const char* export_name,
                              bool is_64bit) noexcept {
    static std::atomic<bool> announced{false};
    if (announced.load(std::memory_order_relaxed) ||
        announced.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr,
                 "[l3k] in-chain kernel32 exports on: first call T%u %s x%d\n",
                 thread_id, export_name != nullptr ? export_name : "?",
                 is_64bit ? 64 : 86);
}

}  // namespace vitreo::winapi::host_in_chain

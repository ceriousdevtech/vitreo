#include "winapi/core/com_direct.hpp"

#include <vitreo/ab_cycle.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vitreo::winapi::com_direct {

namespace detail {

namespace {

[[nodiscard]] Config read_config() noexcept {
    Config config;
    const char* value = std::getenv("VITREO_D3D11_COM_DIRECT");
    config.enabled = value == nullptr || std::strcmp(value, "0") != 0;
    config.cycled = diagnostics::ab_detail::period_seconds() != 0;
    if (!config.enabled) {
        // Off is not the shipping configuration: say so where a measurement
        // looks for proof that its env reached the guest.
        std::fprintf(stderr,
                     "[wp-r1] VITREO_D3D11_COM_DIRECT=0: direct COM crossing "
                     "and D3D11 per-call cuts OFF\n");
    }
    return config;
}

}  // namespace

const Config g_config = read_config();
std::atomic<int> g_force{0};

bool cycled_on() noexcept {
    thread_local diagnostics::AbCycleSampler sampler;
    return !diagnostics::ab_cycle_off_sampled(sampler, "wp-r1");
}

}  // namespace detail

void note_first_direct_crossing(u32 thread_id, u32 slot,
                                bool is_64bit) noexcept {
    static std::atomic<bool> announced{false};
    if (announced.load(std::memory_order_relaxed) ||
        announced.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    std::fprintf(stderr,
                 "[wp-r1] direct COM crossing on: first call T%u slot=%u x%d\n",
                 thread_id, slot, is_64bit ? 64 : 86);
}

}  // namespace vitreo::winapi::com_direct

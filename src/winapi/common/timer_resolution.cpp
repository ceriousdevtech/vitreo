#include "winapi/common/timer_resolution.hpp"

#include <atomic>
#include <map>
#include <mutex>

namespace vitreo::winapi {
namespace {

struct ResolutionState {
    std::mutex mutex;
    /// period -> outstanding request count. Ordered so begin() is the current
    /// effective resolution without a scan.
    std::map<u32, u32> requests;
    /// Mirror of the effective value so readers on hot paths (Sleep, waits)
    /// never take the lock.
    std::atomic<u32> effective{TIMER_PERIOD_DEFAULT};
};

ResolutionState& state() {
    static ResolutionState s;
    return s;
}

/// Caller must hold the lock.
void republish_effective_locked(ResolutionState& s) {
    s.effective.store(
        s.requests.empty() ? TIMER_PERIOD_DEFAULT : s.requests.begin()->first,
        std::memory_order_relaxed);
}

} // namespace

bool timer_resolution_acquire(u32 period) {
    if (period < TIMER_PERIOD_MIN || period > TIMER_PERIOD_MAX) return false;
    ResolutionState& s = state();
    std::lock_guard<std::mutex> guard(s.mutex);
    ++s.requests[period];
    republish_effective_locked(s);
    return true;
}

bool timer_resolution_release(u32 period) {
    if (period < TIMER_PERIOD_MIN || period > TIMER_PERIOD_MAX) return false;
    ResolutionState& s = state();
    std::lock_guard<std::mutex> guard(s.mutex);
    auto it = s.requests.find(period);
    if (it == s.requests.end()) return false;  // unbalanced end
    if (--it->second == 0) s.requests.erase(it);
    republish_effective_locked(s);
    return true;
}

u32 timer_resolution_effective() noexcept {
    return state().effective.load(std::memory_order_relaxed);
}

bool timer_resolution_is_high() noexcept {
    return timer_resolution_effective() < TIMER_PERIOD_DEFAULT;
}

void timer_resolution_reset_for_testing() noexcept {
    ResolutionState& s = state();
    std::lock_guard<std::mutex> guard(s.mutex);
    s.requests.clear();
    republish_effective_locked(s);
}

} // namespace vitreo::winapi

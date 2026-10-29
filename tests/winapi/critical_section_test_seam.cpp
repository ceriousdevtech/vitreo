/// @file critical_section_test_seam.cpp
/// @brief TEST-BUILD-ONLY: CriticalSectionManager::leave_with_released_hook_for_test.
///
/// Compiled into the test binaries only, never into vitreo_winapi. It
/// instantiates the accelerated Leave (critical_section_leave.hpp) with the
/// released hook; the shipping leave() instantiates the same template without
/// it, so the release build has no seam code on its Leave path.

#include <winapi/kernel32/synchronization/critical_section_leave.hpp>

namespace vitreo::winapi {

void CriticalSectionManager::leave_with_released_hook_for_test(GuestAddr cs_addr,
                                                               ReleasedHook hook) {
    if (cs_accel_enabled()) {
        if (std::atomic<u32>* word = lock_word(cs_addr)) {
            leave_accel</*kReleasedHook=*/true>(cs_addr, *word, hook);
            return;
        }
    }
    // The legacy (host-mutex) path has no releasing store of its own to hook.
    leave(cs_addr);
}

} // namespace vitreo::winapi

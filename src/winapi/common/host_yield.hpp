#pragma once

/// @file host_yield.hpp
/// @brief The host yield behind SwitchToThread and Sleep(0).
///
/// Learn: Sleep(0) makes the thread "relinquish the remainder of its time
/// slice but remain ready", and SwitchToThread yields only to a thread that is
/// ready on the calling processor; neither changes the caller's priority, so
/// with nothing else ready the thread simply continues.
///
/// std::this_thread::yield() is not that on macOS: it is sched_yield, i.e.
/// swtch_pri(0), which DEPRESSES the caller to the lowest priority until it is
/// next scheduled. Any runnable thread on the processor, of any priority and
/// any process, then runs first. A game's render thread busy-waits on worker
/// tasks through SwitchToThread; with swtch_pri, 77% of its preemptions in a
/// System Trace came directly after its own yield, handing the core to an
/// unrelated browser process while the frame waited.
///
/// Mach thread_switch with SWITCH_OPTION_NONE and no handoff thread yields to
/// ready threads of equal or higher priority and keeps the caller's priority.

#include <thread>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_switch.h>
#endif

namespace vitreo::winapi {

inline void host_yield_timeslice() noexcept {
#if defined(__APPLE__)
    (void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
#else
    std::this_thread::yield();
#endif
}

}  // namespace vitreo::winapi

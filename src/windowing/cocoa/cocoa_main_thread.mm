// SPDX-License-Identifier: LGPL-2.1-only

/// @file cocoa_main_thread.mm
/// @brief Getting work onto the AppKit main thread, and waking it.
///
/// AppKit may only be driven from the main thread, while guest threads call
/// into CocoaBridge from anywhere. These functions queue a block for the
/// main thread and, where the caller needs the result, wait for it; a main
/// thread that is itself waiting drains the same queue. The second half
/// wakes a main thread parked in a guest wait when something is signalled.
///
/// Split out of cocoa_bridge.mm, which was 7,624 lines. These are CocoaBridge
/// methods defined in a separate translation unit; the class is unchanged.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CATransaction.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAShapeLayer.h>
#import <objc/runtime.h>
#include "windowing/model/message_box_layout.hpp"
#include "windowing/model/diag_flags.hpp"
#include "windowing/cocoa/objc_helpers.h"
#include "windowing/cocoa/cocoa_control_delegates.h"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include "windowing/messages/input_latency.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/cocoa/window_presentation.hpp"
#include "windowing/cocoa/app_icon_presentation.hpp"
#include "graphics/common/perf_diag.hpp"
#include "graphics/common/surface_presenter.hpp"
#include <string>
#include <cstring>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cmath>
#include <limits>
#include <algorithm>
#include <CoreFoundation/CFRunLoop.h>
#include <Block.h>
#include <deque>
#include <mutex>
#include <atomic>
#include <CoreGraphics/CGEventSource.h>
#include <libproc.h>
#include <cerrno>
#include <signal.h>
#include <mach/mach_time.h>
#include "windowing/cocoa/cocoa_bridge_internal.h"

namespace vitreo::windowing {

static thread_local std::function<void()> t_native_wait_dispatch;

CocoaBridge::NativeWaitScope::NativeWaitScope(std::function<void()> dispatch)
    : m_previous(std::move(t_native_wait_dispatch)) {
    t_native_wait_dispatch = std::move(dispatch);
}

CocoaBridge::NativeWaitScope::~NativeWaitScope() {
    t_native_wait_dispatch = std::move(m_previous);
}

// Main-thread work from other threads, in the order it was asked for.
//
// A CFRunLoopPerformBlock only runs when the main run loop reaches it, and a
// main thread that is itself WAITING -- inside a cross-thread SendMessage to a
// guest thread, pumping events until the reply comes -- may never get there:
// with an event always queued, nextEventMatchingMask returns without running
// blocks. If the thread it waits for needs the main thread to answer
// (SetCursor, a window query), neither can proceed. Measured on a D3D11
// title: a focus change had the main thread nested in WM_ACTIVATE sends for
// seconds while the game thread sat in SetCursor waiting for it.
//
// Windows' rule is that a thread waiting for a SendMessage reply still
// services what is sent to it. The equivalent here: every block for the main
// thread joins this queue, a perform-block drains it as before, and the main
// thread's own waits (CocoaBridge::wait_events) drain it too.
namespace {

std::mutex& main_work_mutex() {
    static std::mutex* mutex = new std::mutex();
    return *mutex;
}

std::deque<dispatch_block_t>& main_work() {
    static auto* queue = new std::deque<dispatch_block_t>();
    return *queue;
}

void vtr_enqueue_main_work(dispatch_block_t block) {
    // This translation unit is built without ARC: the copy is ours, released
    // after it runs.
    dispatch_block_t copy = Block_copy(block);
    {
        std::lock_guard<std::mutex> lock(main_work_mutex());
        main_work().push_back(copy);
    }
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, ^{
        vtr_drain_main_work();
    });
    CFRunLoopWakeUp(CFRunLoopGetMain());
}

}  // namespace

void vtr_drain_main_work() {
    if (![NSThread isMainThread]) return;
    for (;;) {
        dispatch_block_t next = nullptr;
        {
            std::lock_guard<std::mutex> lock(main_work_mutex());
            if (main_work().empty()) return;
            next = main_work().front();
            main_work().pop_front();
        }
        // Popped before it runs, so a block that waits (and drains) itself
        // cannot run twice.
        next();
        Block_release(next);
    }
}

void vtr_run_on_main_sync(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
        return;
    }
    // VITREO_MAIN_HOPS=1: count and time the SYNCHRONOUS main-thread round
    // trips, reported per second. A worker that takes one of these parks until
    // the main thread turns its run loop, so these are latency, not CPU -- and
    // a profile that shows the tree 95% "parked in kernel" cannot distinguish
    // an idle thread from one queued behind the main thread. This is the
    // instrument that can.
    static const bool hops_on = std::getenv("VITREO_MAIN_HOPS") != nullptr;
    struct HopTimer {
        bool on;
        std::chrono::steady_clock::time_point t0;
        explicit HopTimer(bool enabled) : on(enabled) {
            if (on) t0 = std::chrono::steady_clock::now();
        }
        ~HopTimer() {
            if (!on) return;
            static std::mutex mu;
            static u64 count = 0;
            static double total_ms = 0.0;
            static double worst_ms = 0.0;
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(now - t0).count();
            std::lock_guard<std::mutex> lock(mu);
            ++count;
            total_ms += ms;
            worst_ms = std::max(worst_ms, ms);
            const double since =
                std::chrono::duration<double>(now - last).count();
            if (since >= 1.0) {
                std::fprintf(stderr,
                    "[main-hops] %.0f/s  mean %.2f ms  worst %.1f ms  "
                    "blocked %.0f ms/s\n",
                    count / since, total_ms / count, worst_ms,
                    total_ms / since);
                count = 0; total_ms = 0.0; worst_ms = 0.0; last = now;
            }
        }
    } hop_timer{hops_on};
    // Use CFRunLoopPerformBlock + semaphore instead of dispatch_sync so the
    // block executes even when the main thread is inside the live-resize
    // tracking run loop (NSEventTrackingRunLoopMode).  dispatch_sync targets
    // the main dispatch queue which only drains in kCFRunLoopDefaultMode,
    // so it would block the guest thread for the entire resize drag.
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    vtr_enqueue_main_work(^{
        block();
        dispatch_semaphore_signal(sem);
    });
    if (t_native_wait_dispatch) {
        // Native focus/hit-test notifications can synchronously send back
        // to this guest thread. Service those sends here, on its own CPU,
        // rather than deadlocking against the AppKit block we are awaiting.
        while (dispatch_semaphore_wait(sem,
                dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC)) != 0) {
            t_native_wait_dispatch();
        }
    } else {
        dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    }
    // This translation unit is built without ARC, so the +1 that
    // dispatch_semaphore_create returns is ours to release. Dropping it on the
    // floor leaks the Mach semaphore behind the object, and Mach ports cost
    // kernel memory that never appears in RSS.
    dispatch_release(sem);
}

/// Variant of vtr_run_on_main_sync that bails out when @p bailout_flag
/// becomes true.  This is critical for pump_events / wait_events: when the
/// user clicks a window border, [NSApp sendEvent:] enters AppKit's tracking
/// loop and blocks until the mouse is released.  That call lives inside the
/// dispatched block, so the semaphore is never signaled.  By polling with a
/// short timeout and checking the live-resize flag we can unblock the guest
/// thread so it processes WM_SIZE/WM_PAINT posted by windowDidResize.
///
/// @p bailout_flag2 is an optional second escape (process teardown): a worker
/// blocked here must not wait forever while the main thread joins workers.
void vtr_run_on_main_sync_bailout(
        dispatch_block_t block,
        const std::atomic<bool>& bailout_flag,
        const std::atomic<bool>* bailout_flag2) {
    if ([NSThread isMainThread]) {
        block();
        return;
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    vtr_enqueue_main_work(^{
        block();
        dispatch_semaphore_signal(sem);
    });

    // Poll with 16 ms timeout; if the live-resize flag becomes true it means
    // [NSApp sendEvent:] entered a tracking loop inside our block.  Return
    // immediately so the caller (get_message) can process resize messages.
    while (dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC)) != 0) {
        if (t_native_wait_dispatch) t_native_wait_dispatch();
        if (bailout_flag.load(std::memory_order_relaxed)) {
            // Block still running; it will finish when tracking ends. The
            // copied block holds its own reference, so releasing ours here is
            // safe and keeps the bailout path from leaking a Mach semaphore.
            dispatch_release(sem);
            return;
        }
        if (bailout_flag2 != nullptr &&
            bailout_flag2->load(std::memory_order_acquire)) {
            dispatch_release(sem);
            return;
        }
    }
    dispatch_release(sem);
}

void vtr_run_on_main_async(dispatch_block_t block) {
    if ([NSThread isMainThread]) {
        block();
        return;
    }
    // Use CFRunLoopPerformBlock with kCFRunLoopCommonModes so blocks execute
    // even when the main thread is in the live-resize tracking run loop
    // (NSEventTrackingRunLoopMode).  dispatch_async only drains in
    // kCFRunLoopDefaultMode and would stall until the mouse is released.
    // Through the same queue as the synchronous hops, so an async block and a
    // later sync one still run in the order they were asked for.
    vtr_enqueue_main_work(block);
}

void CocoaBridge::defer_presentation(std::function<void()> callback) {
    // A timer on a host queue can wake the common-mode main-work queue even
    // while the guest is inside a native wait. No guest messages are dispatched.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC),
                   dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        vtr_enqueue_main_work(^{ callback(); });
    });
}

void CocoaBridge::service_main_runloop(u32 timeout_ms) noexcept {
    if (![NSThread isMainThread]) {
        return;
    }
    const CFTimeInterval seconds =
        static_cast<CFTimeInterval>(timeout_ms) / 1000.0;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
}

namespace {
// One wake event in flight at a time: the pump clears the flag when it
// dequeues the event, so a burst of signals costs one NSEvent, not N.
std::atomic<bool> g_main_wake_posted{false};
// Only a PARKED main thread needs the event. A running main thread re-probes
// its objects before it parks, and the park is armed before that probe, so a
// signal is either seen by the probe or posted here; never lost. Without this
// gate every signal in the process (thousands per second from the guest
// UI thread)
// would bounce the main thread through a pump.
std::atomic<bool> g_main_parked{false};
// Native event dequeue is not acknowledgment by the guest wait. Preserve a
// signal across intervening pumps until that wait disarms or re-probes.
std::atomic<bool> g_main_wake_pending{false};
} // namespace

std::atomic<u64> g_gui_wake_posted{0};
std::atomic<u64> g_gui_wake_suppressed_flag{0};
std::atomic<u64> g_gui_wake_not_parked{0};
std::atomic<u64> g_gui_wake_consumed{0};

void CocoaBridge::set_main_thread_parked(bool parked) noexcept {
    if (parked) g_main_wake_pending.store(false, std::memory_order_seq_cst);
    g_main_parked.store(parked, std::memory_order_seq_cst);
}

bool vtr_main_wake_pending() noexcept {
    return g_main_parked.load(std::memory_order_seq_cst) &&
           g_main_wake_pending.load(std::memory_order_seq_cst);
}

bool vtr_consume_main_wake_event(void* ns_event) noexcept {
    NSEvent* event = (__bridge NSEvent*)ns_event;
    if ([event type] != NSEventTypeApplicationDefined ||
        [event subtype] != kVitreoMainWakeSubtype) {
        return false;
    }
    g_main_wake_posted.store(false, std::memory_order_release);
    g_gui_wake_consumed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void CocoaBridge::wake_main_runloop() noexcept {
    CFRunLoopWakeUp(CFRunLoopGetMain());
    if (NSApp == nil) return;
    if (!g_main_parked.load(std::memory_order_seq_cst) &&
        !vtr_main_event_wait_active()) {
        g_gui_wake_not_parked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_main_wake_pending.store(true, std::memory_order_seq_cst);
    if (g_main_wake_posted.exchange(true, std::memory_order_acq_rel)) {
        g_gui_wake_suppressed_flag.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_gui_wake_posted.fetch_add(1, std::memory_order_relaxed);
    @autoreleasepool {
        NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                           location:NSZeroPoint
                                      modifierFlags:0
                                          timestamp:0
                                       windowNumber:0
                                            context:nil
                                            subtype:kVitreoMainWakeSubtype
                                              data1:0
                                              data2:0];
        if (wake != nil) {
            [NSApp postEvent:wake atStart:YES];
        } else {
            g_main_wake_posted.store(false, std::memory_order_release);
        }
    }
}

} // namespace vitreo::windowing

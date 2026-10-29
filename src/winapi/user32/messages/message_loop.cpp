/// @file message_loop.cpp
/// @brief GetMessageW/PeekMessageW/DispatchMessageW/TranslateMessage (WIN32-053)

#include "message_loop.hpp"
#include "winapi/user32/messages/ansi_char_message.hpp"
#include "winapi/user32/messages/message_time.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "core/memory/guest_memory.hpp"
#include <cstdlib>
#include "windowing/messages/input_latency.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>    // fprintf
#include <thread>    // this_thread::sleep_for
#include <pthread.h>

namespace vitreo::winapi {

static void trace_input_message(const char* api, const windowing::MSG& msg) {
    // VITREO_INPUT_LATENCY: the queue half — how long this message sat between
    // MessageQueue::post_impl and the guest asking for it. Buffered and
    // summarised at exit rather than one unbuffered stderr line per message:
    // a mouse-move flood written that way slows the guest enough to hide the
    // very stalls the probe exists to find.
    windowing::input_latency::note_delivered(msg);

    static const bool input_diag = std::getenv("VITREO_INPUT_DIAG") != nullptr;
    if (!input_diag) return;
    if (msg.message < windowing::WM::KEYDOWN || msg.message > windowing::WM::CHAR_MSG) {
        if (msg.message != windowing::WM::INPUT &&
            msg.message != windowing::WM::MOUSEMOVE &&
            msg.message != windowing::WM::LBUTTONDOWN &&
            msg.message != windowing::WM::LBUTTONUP &&
            msg.message != windowing::WM::RBUTTONDOWN &&
            msg.message != windowing::WM::RBUTTONUP) {
            return;
        }
    }
    std::fprintf(stderr,
                 "[input] %s hwnd=0x%x msg=0x%x wparam=0x%llx lparam=0x%llx "
                 "client=(%d,%d) screen=(%d,%d)\n",
                 api, msg.hwnd, msg.message,
                 static_cast<unsigned long long>(msg.wParam),
                 static_cast<unsigned long long>(msg.lParam),
                 static_cast<i16>(msg.lParam & 0xffff),
                 static_cast<i16>((msg.lParam >> 16) & 0xffff),
                 msg.pt.x, msg.pt.y);
}

static inline bool message_loop_diag_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

// =============================================================================
// Helpers
// =============================================================================

/// Fill MSG.pt with the cursor position in screen (desktop) coordinates.
/// In real Win32 the OS fills this at message-generation time; mouse messages
/// (WM_MOUSEFIRST..WM_MOUSELAST) carry client coordinates in lParam, so we
/// translate to desktop coordinates using the HWND's origin (same basis that
/// MapWindowPoints uses).
static void fill_msg_cursor_pt(windowing::HwndManager& mgr,
                               User32ExtManager* user32_ext,
                               windowing::MSG& msg) {
    constexpr u32 WM_MOUSEFIRST = 0x200;
    constexpr u32 WM_MOUSELAST  = 0x20d;
    if (msg.message == 0x020a || msg.message == 0x020e) {
        // Wheel lParam is already in screen coordinates, including negatives.
        msg.pt = {static_cast<i16>(msg.lParam & 0xffff),
                  static_cast<i16>((msg.lParam >> 16) & 0xffff)};
        return;
    }
    if (msg.message < WM_MOUSEFIRST || msg.message > WM_MOUSELAST) return;

    // Extract client coords from lParam (signed 16-bit each)
    i32 clientX = static_cast<i16>(msg.lParam & 0xFFFF);
    i32 clientY = static_cast<i16>((msg.lParam >> 16) & 0xFFFF);

    // Walk the HWND parent chain to compute the origin in desktop space
    windowing::Point origin{0, 0};
    if (user32_ext == nullptr ||
        !user32_ext->is_virtual_fullscreen_window(msg.hwnd)) {
        windowing::HWND cur = msg.hwnd;
        while (cur != windowing::HWND_DESKTOP && cur != windowing::NULL_HWND) {
            const auto* w = mgr.find(cur);
            if (!w) break;
            origin.x += w->rect.left;
            origin.y += w->rect.top;
            cur = w->parent;
        }
    }

    msg.pt.x = clientX + origin.x;
    msg.pt.y = clientY + origin.y;
}

// =============================================================================
// MessageLoopManager
// =============================================================================

MessageLoopManager::MessageLoopManager(
    windowing::HwndManager& hwnd_mgr,
    windowing::MessageQueue& queue,
    windowing::CocoaBridge& bridge
)
    : m_hwnd_mgr{hwnd_mgr}
    , m_queue{queue}
    , m_bridge{bridge}
{
    m_bridge.set_message_queue(m_queue);
    // Lets a post wake only the thread that owns the message's window.
    m_queue.set_hwnd_owner_resolver(
        [this](windowing::HWND hwnd) -> std::optional<u32> {
            return m_hwnd_mgr.owner_thread_id(hwnd);
        });
}

void MessageLoopManager::set_wndproc_callback(WndProcCallback callback) {
    m_wndproc_callback = std::move(callback);
}

// -----------------------------------------------------------------------------
// get_message
// -----------------------------------------------------------------------------

thread_local bool MessageLoopManager::m_sent_ended_peek = false;

bool MessageLoopManager::synthesized_passes_filter(
    windowing::HWND msg_hwnd, u32 message, windowing::HWND filter_hwnd,
    u32 min, u32 max) noexcept {
    // (HWND)-1 asks for thread messages only — messages posted with a null
    // window. A synthesized window message is never one of those.
    if (filter_hwnd == static_cast<windowing::HWND>(-1)) return false;
    if (filter_hwnd != 0 && filter_hwnd != msg_hwnd) return false;
    // Both bounds zero means no range filter.
    if (min == 0 && max == 0) return true;
    // An inverted range still yields WM_PAINT and WM_TIMER, unlike posted
    // messages (s2msg_queue_probe inverted_range_paint / _timer).
    if (min > max) return true;
    return message >= min && message <= max;
}

bool MessageLoopManager::retrieve_queued(windowing::MSG& msg, u32 tid,
                                         windowing::HWND filter_hwnd, u32 min,
                                         u32 max, u32 remove_flags) {
    const bool removed = (remove_flags & windowing::PM::REMOVE) != 0;
    for (;;) {
        bool sent_ran = false;
        if (!m_queue.peek_for_thread(msg, tid, m_hwnd_mgr, filter_hwnd, min,
                                     max, remove_flags, &sent_ran)) {
            // Under PM_QS_* a call that ran sent messages returns FALSE
            // without looking further (not even at WM_PAINT).
            if (sent_ran && (remove_flags >> windowing::PM::QS_SHIFT) != 0) {
                m_sent_ended_peek = true;
            }
            return false;
        }
        fill_msg_cursor_pt(m_hwnd_mgr, m_user32_ext_mgr, msg);
        // WH_KEYBOARD / WH_MOUSE see hardware input first; a nonzero return
        // discards it and retrieval moves on (Learn KeyboardProc/MouseProc).
        if (msg.hardware_input && m_user32_ext_mgr != nullptr &&
            m_user32_ext_mgr->dispatch_input_hooks(msg, removed)) {
            if (!removed) {
                (void)m_queue.remove_exact(msg.hwnd, msg.message, msg.lParam);
            }
            continue;
        }
        finish_retrieved_message(msg, removed);
        return true;
    }
}

bool MessageLoopManager::retrieve_synthesized(windowing::MSG& msg, u32 tid,
                                              windowing::HWND filter_hwnd,
                                              u32 min, u32 max,
                                              u32 remove_flags) {
    // After everything queued: WM_PAINT, then WM_TIMER (Learn PeekMessage
    // remarks; s2msg_queue_probe "order"). PM_QS_* in the high word limits
    // the classes (PM_QS_POSTMESSAGE includes QS_TIMER).
    const u32 qs = remove_flags >> windowing::PM::QS_SHIFT;
    const bool remove = (remove_flags & windowing::PM::REMOVE) != 0;
    if (m_paint_mgr && (qs == 0 || (qs & windowing::QS::PAINT) != 0)) {
        const u64 paint_hwnd = m_paint_mgr->find_window_needing_paint(
            tid, static_cast<u64>(filter_hwnd));
        if (paint_hwnd != 0 &&
            synthesized_passes_filter(static_cast<windowing::HWND>(paint_hwnd),
                                      windowing::WM::PAINT, filter_hwnd, min,
                                      max)) {
            // WM_PAINT stays pending until the update region is validated,
            // so PM_REMOVE has nothing to remove.
            msg = {};
            msg.hwnd = static_cast<windowing::HWND>(paint_hwnd);
            msg.message = windowing::WM::PAINT;
            finish_retrieved_message(msg, remove);
            VITREO_TRACE("[msgloop] synthesized WM_PAINT for hwnd=0x%llx\n",
                         static_cast<unsigned long long>(paint_hwnd));
            return true;
        }
    }
    if (m_user32_ext_mgr && (qs == 0 || (qs & windowing::QS::TIMER) != 0) &&
        synthesized_passes_filter(filter_hwnd, windowing::WM::TIMER,
                                  filter_hwnd, min, max)) {
        windowing::HWND timer_hwnd = 0;
        u64 timer_id = 0;
        GuestAddr timer_callback = 0;
        // PM_NOREMOVE (and WaitMessage) observe a due timer without
        // rearming it; the retrieving call gets the same WM_TIMER.
        if (m_user32_ext_mgr->find_expired_timer(timer_hwnd, timer_id,
                                                 timer_callback, tid, remove,
                                                 filter_hwnd)) {
            msg = make_timer_message(timer_hwnd, timer_id, timer_callback);
            finish_retrieved_message(msg, remove);
            VITREO_TRACE("[msgloop] synthesized WM_TIMER for hwnd=0x%x id=%llu callback=0x%llx\n",
                         timer_hwnd, static_cast<unsigned long long>(timer_id),
                         static_cast<unsigned long long>(timer_callback));
            return true;
        }
    }
    return false;
}

bool MessageLoopManager::get_message(
    windowing::MSG& msg,
    windowing::HWND filter_hwnd,
    u32 min,
    u32 max
) {
    const u32 current_tid = ThreadManager::get_current_thread_id_static();
    if (m_user32_ext_mgr) {
        m_user32_ext_mgr->drain_pending_native_resizes(current_tid);
        m_user32_ext_mgr->drain_pending_win_events(current_tid);
    }

    // A process whose key window USER32 never activated synchronously (the
    // activation path calls note_synchronous_application_activation) still
    // learns it is active at its first GetMessage. Windows *sends*
    // WM_ACTIVATEAPP and WM_SETFOCUS (Learn), so they go to the window
    // procedure here, as a sent message would, and GetMessage keeps looking:
    // it never returns them (s2msg_ansi_msg_probe retrieves only what was
    // posted). Only the window the host actually made key is activated -- a
    // hidden window named as the filter never is.
    if (!m_sent_initial_activate) {
        const windowing::HWND target = m_bridge.active_hwnd();
        const auto* target_window = m_hwnd_mgr.find(target);
        if (target != windowing::NULL_HWND && target_window &&
            target_window->owner_thread_id == current_tid) {
            m_sent_initial_activate = true;
            windowing::MSG sent{};
            sent.hwnd = target;
            sent.message = windowing::WM::ACTIVATEAPP;
            sent.wParam = 1;  // TRUE = activating
            (void)dispatch_message(sent);
            sent.message = windowing::WM::SETFOCUS;
            sent.wParam = 0;  // no previously focused guest window
            (void)dispatch_message(sent);
            VITREO_TRACE("[msgloop] sent initial WM_ACTIVATEAPP/WM_SETFOCUS "
                         "to hwnd=0x%x\n", target);
        }
    }

    // GetMessage is the blocking wait (Learn): it returns when a message is
    // available and otherwise sleeps. It used to find timers and paints by
    // polling -- a 1 ms queue wait plus a main-thread pump every 1 ms, so an
    // idle GUI thread woke ~2000 times a second (about 3% CPU per idle
    // process). Now every source is event-driven:
    //  - posts, sends, WM_QUIT and teardown wake the queue (MessageQueue);
    //  - InvalidateRect, a timer armed from another thread, a queued WinEvent
    //    or tree notification call MessageQueue::notify_thread;
    //  - native input is drained into the queue by the main run loop's
    //    BeforeWaiting observer (CocoaBridge::install_main_runloop_drain), so
    //    the guest pumps AppKit once before it blocks, not on a clock;
    //  - the only timed wake is the next WM_TIMER this call can return.
    // The wait generation is read before the sources are checked, so a wake
    // that lands in between ends the wait instead of being lost.
    constexpr u32 kSafetyCapMs = 1000;   // bound on a wake source missed above
    constexpr u32 kPollPumpMs = 16;      // AppKit pump cadence without the drain
    constexpr u32 kLiveResizeMs = 16;    // live-resize end posts nothing
    const bool host_main = pthread_main_np() != 0;
    bool pumped = false;
    auto last_pump = std::chrono::steady_clock::time_point{};

    for (;;) {
        if (m_user32_ext_mgr) {
            m_user32_ext_mgr->drain_pending_native_resizes(current_tid);
            m_user32_ext_mgr->drain_pending_win_events(current_tid);
        }
        if (m_queue.shutdown_requested()) {
            msg = {};
            msg.message = windowing::WM::QUIT;
            msg.target_thread_id = current_tid;
            return false;
        }
        const u64 generation = m_queue.wake_generation(current_tid);

        if (retrieve_queued(msg, current_tid, filter_hwnd, min, max,
                            windowing::PM::REMOVE)) {
            if (msg.message == windowing::WM::QUIT) {
                if (message_loop_diag_enabled()) {
                    fprintf(stderr, "[quit-diag] GetMessage returning FALSE (WM_QUIT)\n");
                }
                return false;
            }
            trace_input_message("GetMessage", msg);
            return true;
        }

        // Deliver a pending SysTreeView32 user-click selection as a synthesized
        // WM_NOTIFY(TVN_SELCHANGED). pop_tree_notify is atomic-gated so this is
        // a single relaxed load when nothing is pending. impl_GetMessageW (which
        // holds the Marshaler) marshals the NMTREEVIEW into guest scratch and
        // patches msg.lParam after this returns.
        {
            windowing::CocoaBridge::TreeNotify tn;
            if (m_bridge.pop_tree_notify(tn)) {
                m_pending_tree = tn;
                m_has_pending_tree = true;
                msg = {};
                msg.hwnd    = tn.parent;
                msg.message = 0x004E; // WM_NOTIFY
                msg.wParam  = tn.ctrl_id;
                // Sentinel: impl_GetMessageW (which holds the Marshaler) replaces
                // this with the marshaled NMTREEVIEW pointer.
                msg.lParam  = 0xFFFFFFFFFFFFFFFFull;
                finish_retrieved_message(msg);
                if (std::getenv("VITREO_WIN_DIAG"))
                    fprintf(stderr, "[nav] get_message drained -> WM_NOTIFY parent=0x%x ctrl_id=%u hItem=%llu\n",
                            tn.parent, tn.ctrl_id, (unsigned long long)tn.h_item);
                return true;
            }
        }

        // Input AppKit already holds outranks WM_PAINT and WM_TIMER: flush it
        // into the queue once before synthesizing or blocking. During live
        // resize the main thread is inside AppKit's tracking loop and a
        // synchronous pump would stall it; windowDidResize posts
        // WM_SIZE/WM_PAINT to the queue directly.
        const bool app = m_bridge.is_app_launched();
        const bool live_resize = m_bridge.in_live_resize();
        const bool drain_active =
            windowing::CocoaBridge::main_runloop_drain_active();
        const auto now = std::chrono::steady_clock::now();
        if (app && !live_resize &&
            (!pumped || (!drain_active &&
                         now - last_pump >= std::chrono::milliseconds(kPollPumpMs)))) {
            m_bridge.pump_events(m_queue);
            pumped = true;
            last_pump = now;
            continue;
        }

        if (retrieve_synthesized(msg, current_tid, filter_hwnd, min, max,
                                 windowing::PM::REMOVE)) {
            return true;
        }

        u32 wait_ms = kSafetyCapMs;
        if (live_resize) {
            wait_ms = kLiveResizeMs;
        } else if (app && !drain_active) {
            wait_ms = kPollPumpMs;
        }
        if (m_user32_ext_mgr &&
            synthesized_passes_filter(filter_hwnd, windowing::WM::TIMER,
                                      filter_hwnd, min, max)) {
            const u32 due = m_user32_ext_mgr->next_timer_due_ms(current_tid,
                                                                filter_hwnd);
            if (due < wait_ms) wait_ms = due;
        }
        if (wait_ms == 0) continue;

        // WH_FOREGROUNDIDLE: the foreground thread is about to go idle.
        if (m_user32_ext_mgr != nullptr) {
            const auto foreground = m_bridge.active_hwnd();
            if (foreground != windowing::NULL_HWND &&
                m_hwnd_mgr.owner_thread_id(foreground) == current_tid) {
                m_user32_ext_mgr->dispatch_foreground_idle_hook();
            }
        }

        if (host_main && app) {
            // A guest on the AppKit thread must keep the run loop running:
            // park in nextEvent (posts and signals post a wake event).
            m_bridge.wait_events(m_queue, wait_ms);
            continue;
        }
        (void)m_queue.wait_for_thread_wake(current_tid, generation,
                                           std::chrono::milliseconds(wait_ms));
    }
}

// -----------------------------------------------------------------------------
// peek_message
// -----------------------------------------------------------------------------

bool MessageLoopManager::peek_message(
    windowing::MSG& msg,
    windowing::HWND filter_hwnd,
    u32 min,
    u32 max,
    u32 remove_flags
) {
    const u32 current_tid = ThreadManager::get_current_thread_id_static();
    m_sent_ended_peek = false;
    if (m_user32_ext_mgr) {
        m_user32_ext_mgr->drain_pending_native_resizes(current_tid);
        m_user32_ext_mgr->drain_pending_win_events(current_tid);
    }

    // Fast path: check queue before a main-thread roundtrip via pump_events.
    bool got = retrieve_queued(msg, current_tid, filter_hwnd, min, max,
                               remove_flags);
    if (got) {
        if ((remove_flags & windowing::PM::REMOVE) != 0) trace_input_message("PeekMessage", msg);
        return true;
    }

    // During live resize the main thread is inside the AppKit tracking
    // loop.  A sync roundtrip via pump_events would block the tracking loop
    // and starve windowDidResize callbacks.  Skip it — the delegate posts
    // WM_SIZE + WM_PAINT directly into the queue.
    if (!m_bridge.in_live_resize()) {
        // Some apps spin on PeekMessage() while waiting on other work. Pumping
        // native events here keeps the UI responsive even when the guest
        // doesn't call GetMessage(). The dispatch_sync roundtrip is throttled
        // so a tight PeekMessage spin-loop cannot saturate the main thread.
        //
        // Throttle on emptiness, not on a flat 60 Hz. A PeekMessage loop is
        // the path a native event takes into the queue, and a flat 16 ms
        // guard dominates dequeue latency.
        //
        // The guard is still needed: a guest that spins on PeekMessage with
        // nothing arriving would otherwise hammer the main thread with
        // cross-thread hops. But that case is exactly the one where the pump
        // comes back EMPTY. So throttle on emptiness, not on time: while pumps
        // keep producing messages, pump every 1 ms, and fall back to 60 Hz
        // once they stop.
        constexpr auto kIdlePumpInterval = std::chrono::milliseconds(16);
        constexpr auto kActivePumpInterval = std::chrono::milliseconds(1);
        static const bool adaptive =
            std::getenv("VITREO_PEEK_PUMP_FLAT") == nullptr;
        const auto pump_interval = (adaptive && m_peek_pump_productive)
                                       ? kActivePumpInterval
                                       : kIdlePumpInterval;
        auto now = std::chrono::steady_clock::now();
        if (now - m_peek_last_pump >= pump_interval) {
            m_bridge.pump_events(m_queue);
            m_peek_last_pump = now;
            got = retrieve_queued(msg, current_tid, filter_hwnd, min, max,
                                  remove_flags);
            m_peek_pump_productive = got;
            if (got) {
                if ((remove_flags & windowing::PM::REMOVE) != 0) trace_input_message("PeekMessage", msg);
                return true;
            }
        }
    }

    if (m_sent_ended_peek) {
        m_sent_ended_peek = false;
        return false;
    }
    return retrieve_synthesized(msg, current_tid, filter_hwnd, min, max,
                                remove_flags);
}

// -----------------------------------------------------------------------------
// dispatch_message
// -----------------------------------------------------------------------------

u64 MessageLoopManager::dispatch_message(const windowing::MSG& msg) {
    // VITREO_INPUT_LATENCY: charge the guest wndproc for this message. RAII so
    // the early returns below (no window, no wndproc) are still accounted, and
    // so the pairing thread-local set at Peek/GetMessage time is always
    // consumed. note_dispatch_begin returns 0 — and note_dispatch_end then
    // does nothing — unless the probe is on and this is an input message.
    struct DispatchInputTimer {
        const windowing::MSG& msg;
        u64 begin_ns;
        ~DispatchInputTimer() {
            windowing::input_latency::note_dispatch_end(msg, begin_ns);
        }
    } input_timer{msg, windowing::input_latency::note_dispatch_begin(msg)};
    m_last_result = 0;
    m_last_wndproc = 0;

    // Always-on: detect WM_SIZE dispatched from queue (should no longer happen
    // after sync fixes — if it does, it's a stale post we missed)
    if (msg.message == windowing::WM::SIZE) {
        if (message_loop_diag_enabled()) {
            std::fprintf(stderr,
                "[msg-diag] dispatch_message WM_SIZE from queue: hwnd=0x%x lParam=0x%llx\n",
                msg.hwnd, static_cast<unsigned long long>(msg.lParam));
        }
    }

    // Trace WM_COMMAND dispatch (gated)
    if (msg.message == windowing::WM::COMMAND && message_loop_diag_enabled()) {
        std::fprintf(stderr,
            "[btn-diag] dispatch WM_COMMAND: hwnd=0x%x id=%u notify=%u lParam=0x%llx\n",
            msg.hwnd,
            static_cast<unsigned>(msg.wParam & 0xFFFF),
            static_cast<unsigned>((msg.wParam >> 16) & 0xFFFF),
            static_cast<unsigned long long>(msg.lParam));
    }

    VITREO_TRACE("[msgloop] dispatch_message: hwnd=0x%x msg=0x%x wParam=0x%llx lParam=0x%llx\n",
                 msg.hwnd, msg.message,
                 static_cast<unsigned long long>(msg.wParam),
                 static_cast<unsigned long long>(msg.lParam));

    // WM_TIMER with lParam != 0: DispatchMessage calls that TIMERPROC instead
    // of the window procedure (Learn) -- but only a TIMERPROC one of this
    // process's timers registered. A posted WM_TIMER naming any other address
    // is dropped: neither it nor the window procedure runs (s2msg_queue_probe
    // fake_timerproc_msg: DispatchMessage returns 0, nothing is called).
    if (msg.message == windowing::WM::TIMER && msg.lParam != 0) {
        if (m_user32_ext_mgr) {
            GuestAddr callback = static_cast<GuestAddr>(msg.lParam);
            if (!m_user32_ext_mgr->is_registered_timer_proc(callback)) {
                VITREO_TRACE("[msgloop] dispatch_message: WM_TIMER lParam 0x%llx "
                             "is no registered TIMERPROC; dropped\n",
                             static_cast<unsigned long long>(callback));
                return 0;
            }
            VITREO_TRACE("[msgloop] dispatch_message: WM_TIMER with callback 0x%llx\n",
                         static_cast<unsigned long long>(callback));
            m_user32_ext_mgr->invoke_timer_callback(msg.hwnd, msg.wParam, callback);
        }
        // TIMERPROC returns void; DispatchMessage's value is then undefined
        // (it is the procedure's RAX on Windows). 0.
        return 0;
    }

    // Look up the HWND to get its wndproc
    const windowing::WindowData* win = m_hwnd_mgr.find(msg.hwnd);
    if (std::getenv("VITREO_WIN_DIAG")) {
        static u64 s_disp = 0;
        if (s_disp < 80 || (s_disp % 1000) == 0 || msg.message == 0x004E)
            fprintf(stderr, "[disp#%llu] msg=0x%x hwnd=0x%x wParam=0x%llx lParam=0x%llx wndproc=0x%llx\n",
                    (unsigned long long)s_disp, msg.message, msg.hwnd,
                    (unsigned long long)msg.wParam, (unsigned long long)msg.lParam,
                    win ? (unsigned long long)win->wndproc : 0ull);
        ++s_disp;
    }
    if (!win) {
        VITREO_TRACE("[msgloop] dispatch_message: hwnd not found\n");
        return 0;
    }

    // Zero is an internal marker for native system controls, not the absence
    // of a Windows procedure. Use the same callable identity exposed by
    // GetWindowLongPtr so the comctl32 chain can reach its native default tail.
    const GuestAddr wndproc = effective_wndproc(*win);
    m_last_wndproc = wndproc;

    VITREO_TRACE("[msgloop] dispatch_message: wndproc=0x%llx callback=%s\n",
                 static_cast<unsigned long long>(wndproc),
                 m_wndproc_callback ? "yes" : "NO");

    if (wndproc == 0 || !m_wndproc_callback) {
        VITREO_TRACE("[msgloop] dispatch_message: cannot dispatch (no wndproc or callback)\n");
        // Native control (no guest wndproc) — it's painted by Cocoa, so the guest
        // can never service its WM_PAINT. Validate it here so find_window_needing_paint
        // stops re-emitting WM_PAINT for it forever (otherwise a native LISTBOX/EDIT/
        // etc. pegs the message loop with an infinite WM_PAINT storm).
        if (msg.message == windowing::WM::PAINT && m_paint_mgr) {
            m_paint_mgr->validate_rect(static_cast<u64>(msg.hwnd), nullptr);
        }
        return 0;
    }

    // Call the wndproc callback (will be interpreter dispatch in future)
    m_last_result = m_wndproc_callback(
        msg.hwnd, msg.message, msg.wParam, msg.lParam, wndproc
    );

    // Flush the window framebuffer after paint and mouse messages so drawing
    // done directly to a persistent window DC (for example drag feedback) is
    // presented once when the wndproc returns. Flushing every dispatched
    // message is wrong while WM_TIMER fires at ~50 Hz: each flush schedules
    // an AppKit redraw and saturates the main thread. ReleaseDC and EndPaint
    // remain presentation boundaries for drawing performed outside these
    // messages.
    if (m_gdi_mgr && msg.hwnd != 0) {
        const u32 m = msg.message;
        const bool is_paint_msg =
            m == windowing::WM::PAINT      || // 0x000F
            m == 0x0014 /* WM_ERASEBKGND */ ||
            m == 0x0085 /* WM_NCPAINT    */ ||
            m == 0x0088 /* WM_SYNCPAINT  */;
        const bool is_mouse_msg =
            m >= windowing::WM::MOUSEMOVE && m <= 0x020E /* WM_MOUSEHWHEEL */;
        if (is_paint_msg || is_mouse_msg) {
            // Charged separately (VITREO_INPUT_LATENCY): this is Vitreo's own
            // presentation work running inside the guest's dispatch, so it
            // lands in the wndproc interval and would otherwise read as the
            // app being slow.
            const u64 flush_begin =
                windowing::input_latency::note_dispatch_begin(msg);
            m_gdi_mgr->flush_window(static_cast<u64>(msg.hwnd));
            windowing::input_latency::note_flush(msg, flush_begin);
        }
    }

    // After dispatching WM_PAINT, validate the window so it doesn't stay
    // dirty forever.  In real Win32 the app is expected to call BeginPaint
    // (which clears the update region).  If it doesn't, DefWindowProc does
    // BeginPaint+EndPaint as a fallback.  Without this, apps that skip
    // BeginPaint (e.g. some installer stubs) would enter an infinite
    // WM_PAINT loop because needs_paint never clears.
    if (msg.message == windowing::WM::PAINT && m_paint_mgr) {
        m_paint_mgr->validate_rect(static_cast<u64>(msg.hwnd), nullptr);
    }

    return m_last_result;
}


// =============================================================================
// Thunk Implementations
// =============================================================================

// MSG has a different layout on x64 vs x86. Writing/reading a fixed 32-bit
// layout corrupts the loop for x64 GUI apps (e.g. PuTTY): the guest reads
// msg.message from offset 8 (x64) but a 32-bit writer puts it at offset 4, so
// the loop sees garbage message values. These helpers honor the guest bitness.
//   x86 MSG: hwnd@0(4) message@4(4) wParam@8(4)  lParam@12(4) time@16 pt@20
//   x64 MSG: hwnd@0(8) message@8(4) wParam@16(8) lParam@24(8) time@32 pt@36
static bool msg_use_x64_layout(Marshaler& m) {
    return m.cpu_is_64bit();
}

static void write_msg_to_guest(Marshaler& m, GuestAddr p, const windowing::MSG& msg) {
    if (p == 0) return;
    if (msg_use_x64_layout(m)) {
        (void)m.write_u64(p + 0,  static_cast<u64>(msg.hwnd));
        (void)m.write_u32(p + 8,  msg.message);
        (void)m.write_u64(p + 16, msg.wParam);
        (void)m.write_u64(p + 24, msg.lParam);
        (void)m.write_u32(p + 32, msg.time);
        (void)m.write_u32(p + 36, static_cast<u32>(msg.pt.x));
        (void)m.write_u32(p + 40, static_cast<u32>(msg.pt.y));
    } else {
        (void)m.write_u32(p + 0,  static_cast<u32>(msg.hwnd));
        (void)m.write_u32(p + 4,  msg.message);
        (void)m.write_u32(p + 8,  static_cast<u32>(msg.wParam));
        (void)m.write_u32(p + 12, static_cast<u32>(msg.lParam));
        (void)m.write_u32(p + 16, msg.time);
        (void)m.write_u32(p + 20, static_cast<u32>(msg.pt.x));
        (void)m.write_u32(p + 24, static_cast<u32>(msg.pt.y));
    }
}

static windowing::MSG read_msg_from_guest(Marshaler& m, GuestAddr p) {
    windowing::MSG msg{};
    if (p == 0) return msg;
    if (msg_use_x64_layout(m)) {
        auto r0 = m.read_u64(p + 0);  auto r1 = m.read_u32(p + 8);
        auto r2 = m.read_u64(p + 16); auto r3 = m.read_u64(p + 24);
        if (r0.is_ok()) msg.hwnd    = static_cast<windowing::HWND>(r0.value());
        if (r1.is_ok()) msg.message = r1.value();
        if (r2.is_ok()) msg.wParam  = r2.value();
        if (r3.is_ok()) msg.lParam  = r3.value();
    } else {
        auto r0 = m.read_u32(p + 0);  auto r1 = m.read_u32(p + 4);
        auto r2 = m.read_u32(p + 8);  auto r3 = m.read_u32(p + 12);
        if (r0.is_ok()) msg.hwnd    = static_cast<windowing::HWND>(r0.value());
        if (r1.is_ok()) msg.message = r1.value();
        if (r2.is_ok()) msg.wParam  = static_cast<u64>(r2.value());
        if (r3.is_ok()) msg.lParam  = static_cast<u64>(r3.value());
    }
    return msg;
}

bool MessageLoopManager::take_pending_tree_notify(windowing::CocoaBridge::TreeNotify& out) {
    if (!m_has_pending_tree) return false;
    out = m_pending_tree;
    m_has_pending_tree = false;
    return true;
}

// Lazily-mapped guest scratch page for marshaling NMTREEVIEW on the GetMessage
// (user-click) path. Mirrors message_send.cpp's tv_notify_scratch.
static GuestAddr loop_tree_scratch(Marshaler& m) {
    static GuestAddr s = 0;
    if (s == 0) {
        GuestAddr base = m.memory().find_free_gap(static_cast<GuestAddr>(0x3E000000), 4096);
        if (base != 0 &&
            m.memory().map_region(base, 4096, MemoryProtection::ReadWrite).is_ok()) {
            s = base;
        }
    }
    return s;
}

// Marshal an NMTREEVIEW(TVN_SELCHANGED) for a user-click selection into guest
// scratch and return the guest pointer (0 on failure).
static GuestAddr marshal_tree_notify(Marshaler& m, const windowing::CocoaBridge::TreeNotify& tn) {
    GuestAddr nm = loop_tree_scratch(m);
    if (nm == 0) return 0;
    // Notify code matches the parent window's character set. Win32 common
    // controls send TVN_SELCHANGEDW to a Unicode parent (the modern default,
    // i.e. windows registered via RegisterClassW) and TVN_SELCHANGEDA only to
    // ANSI parents. Vitreo doesn't yet track per-window Unicode-ness, and real
    // GUI dialogs (e.g. PuTTY's config box) are Unicode parents that check for
    // the W code (0xFFFFFE6E), so default to W. (A = 0xFFFFFE6F.)
    constexpr u32 TVN_SELCHANGEDW = 0xFFFFFE6Eu;
    constexpr u32 TVN_ITEMEXPANDINGA = 0xFFFFFE6Bu;
    constexpr u32 TVE_EXPAND = 0x0002u;
    constexpr u32 TVE_COLLAPSE = 0x0001u;
    const bool item_expanding = tn.kind != windowing::CocoaBridge::TreeNotify::Kind::Selection;
    const u32 notify_code = item_expanding ? TVN_ITEMEXPANDINGA : TVN_SELCHANGEDW;
    const u32 action = tn.kind == windowing::CocoaBridge::TreeNotify::Kind::Expanding
        ? TVE_EXPAND
        : (tn.kind == windowing::CocoaBridge::TreeNotify::Kind::Collapsing ? TVE_COLLAPSE : 0u);
    for (u32 i = 0; i < 160; i += 8) (void)m.write_u64(nm + i, 0);
    if (std::getenv("VITREO_WIN_DIAG"))
        fprintf(stderr, "[nav] marshal NMTREEVIEW@0x%llx hwndFrom=0x%x "
                "idFrom=%u code=0x%x is64=%d\n",
                static_cast<unsigned long long>(nm), tn.ctrl, tn.ctrl_id,
                notify_code, m.cpu_is_64bit() ? 1 : 0);
    if (m.cpu_is_64bit()) {
        (void)m.write_u64(nm + 0,   static_cast<u64>(tn.ctrl));
        (void)m.write_u64(nm + 8,   static_cast<u64>(tn.ctrl_id));
        (void)m.write_u32(nm + 16, notify_code);
        (void)m.write_u32(nm + 24, action);
        (void)m.write_u32(nm + 88,  0x4u | 0x8u | 0x1u);
        (void)m.write_u64(nm + 96,  tn.h_item);
        (void)m.write_u32(nm + 104, 2u);
        (void)m.write_u64(nm + 136, tn.lparam);
    } else {
        (void)m.write_u32(nm + 0,  static_cast<u32>(tn.ctrl));
        (void)m.write_u32(nm + 4,  tn.ctrl_id);
        (void)m.write_u32(nm + 8, notify_code);
        (void)m.write_u32(nm + 12, action);
        (void)m.write_u32(nm + 56, 0x4u | 0x8u | 0x1u);
        (void)m.write_u32(nm + 60, static_cast<u32>(tn.h_item));
        (void)m.write_u32(nm + 64, 2u);
        (void)m.write_u32(nm + 92, static_cast<u32>(tn.lparam));
    }
    return nm;
}

static GuestAddr marshal_tab_notify(
    Marshaler& m, u64 marker, u64 control_id) {
    windowing::HWND control = windowing::NULL_HWND;
    bool selection_changing = false;
    if (!windowing::CocoaBridge::decode_tab_notify_lparam(
            marker, &control, &selection_changing)) {
        return 0;
    }

    static GuestAddr scratch = 0;
    if (scratch == 0 || !m.memory().is_mapped(scratch, 24)) {
        scratch = m.memory().find_free_gap(
            static_cast<GuestAddr>(0x3E001000), 4096);
        if (scratch == 0 ||
            m.memory().map_region(
                scratch, 4096, MemoryProtection::ReadWrite).is_err()) {
            scratch = 0;
            return 0;
        }
    }

    constexpr u32 TCN_SELCHANGE = 0xFFFFFDD9u;
    constexpr u32 TCN_SELCHANGING = 0xFFFFFDD8u;
    for (u32 offset = 0; offset < 24; offset += 8) {
        (void)m.write_u64(scratch + offset, 0);
    }
    if (m.cpu_is_64bit()) {
        (void)m.write_u64(scratch + 0, static_cast<u64>(control));
        (void)m.write_u64(scratch + 8, control_id);
        (void)m.write_u32(
            scratch + 16,
            selection_changing ? TCN_SELCHANGING : TCN_SELCHANGE);
    } else {
        (void)m.write_u32(scratch + 0, static_cast<u32>(control));
        (void)m.write_u32(scratch + 4, static_cast<u32>(control_id));
        (void)m.write_u32(
            scratch + 8,
            selection_changing ? TCN_SELCHANGING : TCN_SELCHANGE);
    }
    return scratch;
}

void MessageLoopManager::dispatch_get_message_hook(
    GuestAddr msg_ptr, u32 remove_flags) {
    if (m_user32_ext_mgr) {
        m_user32_ext_mgr->dispatch_get_message_hook(msg_ptr, remove_flags);
    }
}

void impl_GetMessageW(Marshaler& m, MessageLoopManager& mlm, bool ansi) {
    GuestAddr msg_ptr = m.arg_ptr(0);
    auto filter_hwnd  = static_cast<windowing::HWND>(m.arg(1));
    auto filter_min   = static_cast<u32>(m.arg(2));
    auto filter_max   = static_cast<u32>(m.arg(3));

    VITREO_TRACE( "[msgloop] GetMessageA/W(hwnd=0x%x min=%u max=%u)\n",
                 filter_hwnd, filter_min, filter_max);

    // Learn: -1 on error, "for example ... hWnd is an invalid window handle"
    // (s2msg_queue_probe getmessage_invalid_hwnd: -1 /
    // ERROR_INVALID_WINDOW_HANDLE).
    if (filter_hwnd != windowing::NULL_HWND &&
        filter_hwnd != static_cast<windowing::HWND>(-1) &&
        !mlm.window_exists(filter_hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0xFFFFFFFFu);
        return;
    }

    windowing::MSG msg{};
    bool result = mlm.get_message(msg, filter_hwnd, filter_min, filter_max);

    // Always-on: trace GetMessage returning FALSE (WM_QUIT). A nested
    // message loop exits on that result.
    if (!result) {
        if (message_loop_diag_enabled()) {
            std::fprintf(stderr,
                "[getmsg-diag] GetMessage returning FALSE (WM_QUIT): hwnd_filter=0x%x\n",
                filter_hwnd);
        }
    }
    // Always-on: trace WM_COMMAND delivery through GetMessage (vs modal loop)
    if (result && msg.message == 0x0111) {
        if (message_loop_diag_enabled()) {
            std::fprintf(stderr,
                "[getmsg-diag] GetMessage returned WM_COMMAND: hwnd=0x%x id=%u\n",
                msg.hwnd, static_cast<unsigned>(msg.wParam & 0xFFFF));
        }
    }

    VITREO_TRACE( "[msgloop] GetMessage returned msg=0x%x hwnd=0x%x\n",
                 msg.message, msg.hwnd);
    static const bool msg_pump_diag = (std::getenv("VITREO_MSG_PUMP_DIAG") != nullptr);
    if (msg_pump_diag) {
        std::fprintf(stderr,
                     "[msgpump-diag] GetMessage tid=%u -> %s hwnd=0x%x msg=0x%x\n",
                     ThreadManager::get_current_thread_id_static(),
                     result ? "TRUE" : "FALSE(QUIT)",
                     msg.hwnd, msg.message);
        std::fflush(stderr);
    }

    // A synthesized treeview WM_NOTIFY (sentinel lParam) needs its NMTREEVIEW
    // marshaled into guest memory now that we hold the Marshaler.
    if (result && msg.message == 0x004E &&
        msg.lParam == 0xFFFFFFFFFFFFFFFFull) {
        windowing::CocoaBridge::TreeNotify tn;
        if (mlm.take_pending_tree_notify(tn)) {
            msg.lParam = static_cast<u64>(marshal_tree_notify(m, tn));
        } else {
            msg.lParam = 0;
        }
    }
    if (result && msg.message == windowing::WM::NOTIFY) {
        windowing::HWND tab_control = windowing::NULL_HWND;
        bool selection_changing = false;
        if (windowing::CocoaBridge::decode_tab_notify_lparam(
                msg.lParam, &tab_control, &selection_changing)) {
            msg.lParam = static_cast<u64>(
                marshal_tab_notify(m, msg.lParam, msg.wParam));
        }
    }

    // Write MSG struct to guest memory (bitness-aware layout). GetMessageA
    // returns character messages in the ANSI code page (ansi_char_message).
    if (ansi && ansi_char_message::is_char_message(msg.message)) {
        msg.wParam = ansi_char_message::to_ansi(msg.wParam, m.cpu_is_64bit());
    }
    write_msg_to_guest(m, msg_ptr, msg);
    // Learn GetMsgProc: GetMessage always removes the retrieved message.
    mlm.dispatch_get_message_hook(msg_ptr, windowing::PM::REMOVE);

    m.set_return(result ? 1u : 0u);
}

void impl_PeekMessageW(Marshaler& m, MessageLoopManager& mlm, bool ansi) {
    GuestAddr msg_ptr = m.arg_ptr(0);
    if (msg_ptr == 0) {
        // s2msg_queue_probe peek_null_msg: FALSE / ERROR_NOACCESS.
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    auto filter_hwnd  = static_cast<windowing::HWND>(m.arg(1));
    auto filter_min   = static_cast<u32>(m.arg(2));
    auto filter_max   = static_cast<u32>(m.arg(3));
    // UINT wRemoveMsg is PeekMessage's fifth argument: an x64 stack slot.
    auto remove_flags = m.arg32(4);
    // s2msg_queue_probe peek_invalid_hwnd: FALSE / ERROR_INVALID_WINDOW_HANDLE.
    if (filter_hwnd != windowing::NULL_HWND &&
        filter_hwnd != static_cast<windowing::HWND>(-1) &&
        !mlm.window_exists(filter_hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0);
        return;
    }

    windowing::MSG msg{};
    bool result = mlm.peek_message(msg, filter_hwnd, filter_min,
                                    filter_max, remove_flags);
    // DIAGNOSTIC: rate-limited caller (guest return address) so we can identify
    // a guest routine that spins on PeekMessage without making progress
    // (VITREO_TRACE), plus the VITREO_MSG_PUMP_DIAG rate limit. Counted only
    // while one of them is on.
    static const bool msg_pump_diag = (std::getenv("VITREO_MSG_PUMP_DIAG") != nullptr);
    static std::atomic<u64> s_peek_calls{0};
    u64 peek_calls = 0;  // this call's 1-based ordinal
    if (::vitreo::detail::g_trace_enabled || msg_pump_diag) {
        peek_calls = s_peek_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    if (::vitreo::detail::g_trace_enabled && ((peek_calls - 1) % 2000) == 0 && m.cpu()) {
        const u32 caller = m.read_u32(static_cast<GuestAddr>(m.cpu()->sp())).value_or(0);
        VITREO_TRACE("[peek] PeekMessage caller(guest)=0x%08x count=%llu result=%s\n",
                     caller, static_cast<unsigned long long>(peek_calls),
                     result ? "TRUE" : "FALSE");
    }
    if (msg_pump_diag && (result || (peek_calls % 500) == 1)) {
        const u32 tid = ThreadManager::get_current_thread_id_static();
        std::fprintf(stderr,
                     "[msgpump-diag] PeekMessage tid=%u -> %s hwnd=0x%x msg=0x%x "
                     "filter=0x%x flags=0x%x (#%llu)\n",
                     tid,
                     result ? "TRUE" : "FALSE",
                     result ? msg.hwnd : 0, result ? msg.message : 0,
                     filter_hwnd, remove_flags,
                     static_cast<unsigned long long>(peek_calls));
        std::fflush(stderr);
    }
    static u32 peek_trace = 0;
    ++peek_trace;
    // Empty peeks dominate slideshow/timer spins; log hits and every 1024th miss.
    if (result || peek_trace == 1 || (peek_trace & 0x3FFu) == 0) {
        VITREO_TRACE( "[peek] PeekMessage(ptr=0x%llx hwnd=0x%x min=0x%x max=0x%x "
                     "flags=0x%x) -> %s msg=0x%x (#%u)\n",
                     static_cast<unsigned long long>(msg_ptr), filter_hwnd,
                     filter_min, filter_max, remove_flags,
                     result ? "TRUE" : "FALSE", result ? msg.message : 0u,
                     peek_trace);
    }

    if (result) {
        if (msg.message == windowing::WM::NOTIFY) {
            windowing::HWND tab_control = windowing::NULL_HWND;
            bool selection_changing = false;
            if (windowing::CocoaBridge::decode_tab_notify_lparam(
                    msg.lParam, &tab_control, &selection_changing)) {
                msg.lParam = static_cast<u64>(
                    marshal_tab_notify(m, msg.lParam, msg.wParam));
            }
        }
        // PeekMessageA returns character messages in the ANSI code page,
        // PM_NOREMOVE included; the queued message keeps its Unicode form.
        if (ansi && ansi_char_message::is_char_message(msg.message)) {
            msg.wParam = ansi_char_message::to_ansi(msg.wParam, m.cpu_is_64bit());
        }
        write_msg_to_guest(m, msg_ptr, msg);
        mlm.dispatch_get_message_hook(msg_ptr, remove_flags);
    }

    m.set_return(result ? 1u : 0u);
}

void impl_DispatchMessageW(Marshaler& m, MessageLoopManager& mlm, bool ansi) {
    GuestAddr msg_ptr = m.arg_ptr(0);
    if (msg_ptr == 0) {
        m.set_return(0);
        return;
    }

    // Read MSG struct from guest memory (bitness-aware layout).
    windowing::MSG msg = read_msg_from_guest(m, msg_ptr);
    // DispatchMessageA hands a Unicode procedure the Unicode form of a
    // character message, DispatchMessageW an ANSI procedure the ANSI form
    // (s2msg_ansi_msg_probe disp_*).
    if (ansi_char_message::is_char_message(msg.message)) {
        const bool unicode_proc = mlm.window_is_unicode(msg.hwnd);
        if (ansi && unicode_proc) {
            msg.wParam = ansi_char_message::to_unicode(msg.wParam);
        } else if (!ansi && !unicode_proc) {
            msg.wParam = ansi_char_message::to_ansi(msg.wParam, m.cpu_is_64bit());
        }
    }

    // Trace guest DispatchMessage for WM_COMMAND (gated)
    if (msg.message == 0x0111 && message_loop_diag_enabled()) {
        std::fprintf(stderr,
            "[disp-diag] guest DispatchMessageW: hwnd=0x%x WM_COMMAND id=%u\n",
            msg.hwnd, static_cast<unsigned>(msg.wParam & 0xFFFF));
    }

    if (mlm.dispatch_native_control(m, msg, ansi)) return;
    u64 result = mlm.dispatch_message(msg);
    m.set_return(result);
}

void impl_TranslateMessage(Marshaler& m, MessageLoopManager& mlm) {
    GuestAddr msg_ptr = m.arg_ptr(0);
    if (msg_ptr == 0) {
        m.set_return(0);
        return;
    }

    // Read MSG struct from guest memory (bitness-aware layout).
    windowing::MSG msg = read_msg_from_guest(m, msg_ptr);

    bool result = mlm.translate_message(msg);
    m.set_return(result ? 1u : 0u);
}


// =============================================================================
// Export Registration
// =============================================================================

DllExports make_message_loop_exports(MessageLoopManager& mlm) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "GetMessageW", 1,
        [&mlm](Marshaler& m) { impl_GetMessageW(m, mlm); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "GetMessageA", 0,
        [&mlm](Marshaler& m) { impl_GetMessageW(m, mlm, /*ansi=*/true); },
        16
    });
    exports.entries.push_back(ExportEntry{
        "PeekMessageW", 2,
        [&mlm](Marshaler& m) { impl_PeekMessageW(m, mlm); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "PeekMessageA", 0,
        [&mlm](Marshaler& m) { impl_PeekMessageW(m, mlm, /*ansi=*/true); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "DispatchMessageW", 3,
        [&mlm](Marshaler& m) { impl_DispatchMessageW(m, mlm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "DispatchMessageA", 0,
        [&mlm](Marshaler& m) { impl_DispatchMessageW(m, mlm, /*ansi=*/true); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "TranslateMessage", 4,
        [&mlm](Marshaler& m) { impl_TranslateMessage(m, mlm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "MsgWaitForMultipleObjects", 5,
        [&mlm](Marshaler& m) { impl_MsgWaitForMultipleObjects(m, mlm); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "MsgWaitForMultipleObjectsEx", 0,
        [&mlm](Marshaler& m) { impl_MsgWaitForMultipleObjectsEx(m, mlm); },
        20
    });
    return exports;
}

} // namespace vitreo::winapi

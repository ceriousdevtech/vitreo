#pragma once
/// @file message_loop.hpp
/// @brief GetMessageW/PeekMessageW/DispatchMessageW/TranslateMessage (WIN32-053)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/cocoa/cocoa_bridge.hpp"
#include <chrono>
#include <functional>

namespace vitreo::winapi { class PaintingManager; }
namespace vitreo::winapi { class User32ExtManager; }
namespace vitreo::winapi { class GdiManager; }
namespace vitreo::winapi { class SyncManager; }

namespace vitreo::winapi {

class Marshaler;

/// Callback for dispatching messages to wndproc (will be interpreter call in future)
/// Args: hwnd, message, wParam, lParam, wndproc_addr
/// Returns: LRESULT
using WndProcCallback = std::function<u64(windowing::HWND, u32, u64, u64, u64)>;

class MessageLoopManager {
public:
    MessageLoopManager(
        windowing::HwndManager& hwnd_mgr,
        windowing::MessageQueue& queue,
        windowing::CocoaBridge& bridge
    );

    /// Set the wndproc callback (for interpreter integration)
    void set_wndproc_callback(WndProcCallback callback);

    /// Set the painting manager (for WM_PAINT synthesis)
    void set_painting_manager(PaintingManager* mgr) { m_paint_mgr = mgr; }

    /// Set the user32 ext manager (for timers and queue-owned window state)
    void set_user32_ext_manager(User32ExtManager* mgr) { m_user32_ext_mgr = mgr; }

    /// Set the GDI manager (for post-dispatch framebuffer flush)
    void set_gdi_manager(GdiManager* mgr) { m_gdi_mgr = mgr; }

    void set_sync_manager(SyncManager* mgr) { m_sync_mgr = mgr; }

    /// Whether @p hwnd's window procedure is Unicode (IsWindowUnicode), which
    /// decides DispatchMessageA/W character translation. Unset: every window
    /// is Unicode -- what IsWindowUnicode answers while USER32 keeps no
    /// per-window charset.
    void set_window_unicode_query(std::function<bool(windowing::HWND)> query) {
        m_window_unicode = std::move(query);
    }
    /// Class procedure for a system control with no guest procedure that
    /// handles its own input (SysLink). Returns true when it handled the
    /// message and set the Marshaler's return value.
    void set_native_control_dispatch(
        std::function<bool(Marshaler&, const windowing::MSG&, bool ansi)> fn) {
        m_native_control_dispatch = std::move(fn);
    }
    [[nodiscard]] bool dispatch_native_control(Marshaler& m, const windowing::MSG& msg,
                                               bool ansi) const {
        // A wired callback owns the complete subclass/native procedure chain.
        // The legacy host shortcut is only a fallback without that dispatcher.
        return !m_wndproc_callback && m_native_control_dispatch &&
               m_native_control_dispatch(m, msg, ansi);
    }
    [[nodiscard]] bool window_is_unicode(windowing::HWND hwnd) const {
        return !m_window_unicode || m_window_unicode(hwnd);
    }

    /// Record that USER32 delivered the process's initial WM_ACTIVATEAPP as a
    /// sent message. This disables the GetMessage fallback without consuming
    /// or dispatching any posted queue entries.
    void note_synchronous_application_activation() noexcept {
        m_sent_initial_activate = true;
    }

    /// GetMessageW. Returns false for WM_QUIT, true otherwise.
    /// Pumps Cocoa events first if queue empty.
    bool get_message(windowing::MSG& msg, windowing::HWND filter_hwnd = 0,
                     u32 min = 0, u32 max = 0);

    /// PeekMessageW. Non-blocking check.
    bool peek_message(windowing::MSG& msg, windowing::HWND filter_hwnd = 0,
                      u32 min = 0, u32 max = 0, u32 remove_flags = 0);

    /// DispatchMessageW. Calls the wndproc for the message's HWND.
    /// Returns the result from wndproc.
    u64 dispatch_message(const windowing::MSG& msg);

    /// TranslateMessage. Translates WM_KEYDOWN/WM_SYSKEYDOWN to WM_CHAR/WM_SYSCHAR.
    bool translate_message(const windowing::MSG& msg);

    /// True when a message Vitreo synthesizes (rather than dequeues) passes
    /// the caller's PeekMessage/GetMessage filters.
    ///
    /// WM_PAINT and WM_TIMER are not queued messages: Windows generates them
    /// on demand from a window's update region and from expired timers. That
    /// generation is still subject to the caller's filters, so a filtered
    /// drain such as `while (PeekMessage(&m, hwnd, min, max, PM_REMOVE))`
    /// terminates. Reporting a synthesized message the filter excludes turns
    /// that loop into an infinite spin.
    [[nodiscard]] static bool synthesized_passes_filter(
        windowing::HWND msg_hwnd, u32 message, windowing::HWND filter_hwnd,
        u32 min, u32 max) noexcept;

    /// Get the last dispatch result (for testing)
    u64 last_dispatch_result() const { return m_last_result; }

    /// Get wndproc address looked up during last dispatch
    u64 last_dispatch_wndproc() const { return m_last_wndproc; }

    /// MsgWaitForMultipleObjects[Ex]. Waits until a message matching the
    /// wake-mask is queued, a handle is signaled, an alertable APC runs
    /// (`alertable`), or dwMilliseconds elapse.
    /// Returns WAIT_OBJECT_0+nCount when a message is ready, WAIT_TIMEOUT
    /// (0x102) on timeout, WAIT_IO_COMPLETION (0xC0) when APCs ran.
    u32 msg_wait_for_multiple_objects(const u32* handles, u32 n_count,
                                      bool wait_all, u32 timeout_ms, u32 wake_mask,
                                      bool alertable = false,
                                      bool input_available = false);

    /// WaitForInputIdle (Learn, nf-winuser-waitforinputidle): 0 when the
    /// target process is idle, WAIT_TIMEOUT, or WAIT_FAILED.
    u32 wait_for_input_idle(u64 process_handle, u32 timeout_ms);

    /// If get_message returned a synthesized treeview WM_NOTIFY marker, take the
    /// pending selection so the caller (impl_GetMessageW) can marshal NMTREEVIEW.
    bool take_pending_tree_notify(windowing::CocoaBridge::TreeNotify& out);

    /// Windows reports keyboard state through GetKeyState as of the message
    /// currently being processed, not the live hardware state. Modifier keys
    /// are recorded as their messages leave the queue so TranslateMessage sees
    /// the chord the application is handling, even if the physical key has
    /// already been released by the time the message is dispatched.
    void note_retrieved_message(const windowing::MSG& msg) noexcept;

    /// Stamp MSG.time if a producer left it zero, then record last-message
    /// state (GetMessageTime / GetKeyState). Used for both queued and
    /// synthesized retrievals. PM_NOREMOVE records time without advancing
    /// keyboard state (removed=false).
    void finish_retrieved_message(windowing::MSG& msg, bool removed = true) noexcept;

    /// True when the given modifier virtual key was down as of the last
    /// retrieved message. Only VK_SHIFT/VK_CONTROL/VK_MENU are tracked.
    [[nodiscard]] bool sync_modifier_down(u32 vk) const noexcept;

    /// Invoke WH_GETMESSAGE hooks for a MSG already written to @p msg_ptr.
    void dispatch_get_message_hook(GuestAddr msg_ptr, u32 remove_flags);

    [[nodiscard]] bool window_exists(windowing::HWND hwnd) const {
        return m_hwnd_mgr.is_valid(hwnd);
    }

    [[nodiscard]] windowing::MessageQueue& message_queue() noexcept {
        return m_queue;
    }
    [[nodiscard]] const windowing::MessageQueue& message_queue() const noexcept {
        return m_queue;
    }

private:
    /// Posted messages, WM_QUIT and input for @p tid (sent messages are
    /// dispatched inside). @p remove_flags: PM_REMOVE | PM_QS_*.
    bool retrieve_queued(windowing::MSG& msg, u32 tid, windowing::HWND filter_hwnd,
                         u32 min, u32 max, u32 remove_flags);
    /// WM_PAINT then WM_TIMER, synthesized once nothing is queued.
    bool retrieve_synthesized(windowing::MSG& msg, u32 tid,
                              windowing::HWND filter_hwnd, u32 min, u32 max,
                              u32 remove_flags);

    u32 m_sync_modifiers{0};  ///< bit 0 shift, bit 1 control, bit 2 alt
    /// retrieve_queued ran sent messages under a PM_QS_* mask: this
    /// PeekMessage returns FALSE. Per calling thread.
    static thread_local bool m_sent_ended_peek;
    /// The character of the last VK_PACKET key-down this thread retrieved.
    static thread_local u16 t_last_packet_char;
    /// GetMessageExtraInfo: the extra information of the message this thread
    /// last retrieved, or what SetMessageExtraInfo stored since.
    static thread_local u64 t_message_extra_info;

public:
    [[nodiscard]] static u64 message_extra_info() noexcept { return t_message_extra_info; }
    /// SetMessageExtraInfo: returns the previous value.
    static u64 exchange_message_extra_info(u64 value) noexcept {
        const u64 previous = t_message_extra_info;
        t_message_extra_info = value;
        return previous;
    }

private:

    windowing::HwndManager& m_hwnd_mgr;
    windowing::MessageQueue& m_queue;
    windowing::CocoaBridge& m_bridge;
    windowing::CocoaBridge::TreeNotify m_pending_tree{};
    bool m_has_pending_tree = false;
    WndProcCallback m_wndproc_callback;
    PaintingManager* m_paint_mgr = nullptr;
    std::function<bool(Marshaler&, const windowing::MSG&, bool)> m_native_control_dispatch;
    User32ExtManager* m_user32_ext_mgr = nullptr;
    GdiManager* m_gdi_mgr = nullptr;
    SyncManager* m_sync_mgr = nullptr;
    u64 m_last_result = 0;
    u64 m_last_wndproc = 0;
    bool m_sent_initial_activate = false;
    std::function<bool(windowing::HWND)> m_window_unicode;
    // Throttle pump_events in peek_message so a guest spin-loop on
    // PeekMessageW doesn't dispatch_sync to the main thread on every
    // iteration.  Mirrors the 16ms cadence used in get_message.
    std::chrono::steady_clock::time_point m_peek_last_pump{};
    /// The previous peek-driven pump_events actually produced a message. While
    /// that holds, the throttle below drops to the same 1 ms GetMessage uses;
    /// it only rises to the 60 Hz guard once pumps start coming back empty.
    bool m_peek_pump_productive{false};
};

// Thunks
/// @param ansi the A entry point: character messages cross the ANSI/Unicode
///        boundary (winapi/user32/messages/ansi_char_message.hpp).
void impl_GetMessageW(Marshaler& m, MessageLoopManager& mlm, bool ansi = false);
void impl_PeekMessageW(Marshaler& m, MessageLoopManager& mlm, bool ansi = false);
void impl_DispatchMessageW(Marshaler& m, MessageLoopManager& mlm, bool ansi = false);
void impl_TranslateMessage(Marshaler& m, MessageLoopManager& mlm);
void impl_MsgWaitForMultipleObjects(Marshaler& m, MessageLoopManager& mlm);
void impl_MsgWaitForMultipleObjectsEx(Marshaler& m, MessageLoopManager& mlm);

[[nodiscard]] DllExports make_message_loop_exports(MessageLoopManager& mlm);

} // namespace vitreo::winapi

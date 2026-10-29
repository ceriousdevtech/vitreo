#pragma once
/// @file show_window.hpp
/// @brief ShowWindow/UpdateWindow/IsWindowVisible/EnableWindow (WIN32-052)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "windowing/contracts/control_host.hpp"
#include "windowing/contracts/window_host.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "windowing/messages/message_queue.hpp"
#include "windowing/messages/message_types.hpp"

namespace vitreo::winapi {

class Marshaler;
class PaintingManager;
class MessageLoopManager;
class MessageSendManager;
class User32ExtManager;

class ShowWindowManager {
public:
    ShowWindowManager(
        windowing::HwndManager& hwnd_mgr,
        windowing::WindowHost& window_host,
        windowing::MessageQueue& queue
    );
    ShowWindowManager(
        windowing::HwndManager& hwnd_mgr,
        windowing::ControlHost& control_host,
        windowing::MessageQueue& queue
    );

    /// ShowWindow. Returns previous visible state.
    bool show_window(windowing::HWND hwnd, int cmd_show);
    /// ShowWindow's return value as Windows reports it (0x18 for a window
    /// that was visible, measured), with its last-error effects.
    u64 show_window_result(windowing::HWND hwnd, int cmd_show);

    /// UpdateWindow. Synchronously dispatches WM_PAINT if window needs repainting.
    bool update_window(windowing::HWND hwnd);

    /// IsWindowVisible. Returns true if window and all ancestors visible.
    bool is_window_visible(windowing::HWND hwnd) const;

    /// Apply the validated WINDOWPLACEMENT state owned by an HWND. Geometry
    /// is the restored outer rectangle in Windows workspace coordinates.
    bool set_window_placement(windowing::HWND hwnd, u32 flags, int show_cmd,
                              windowing::Point minimized_position,
                              windowing::Point maximized_position,
                              windowing::Rect normal_rect);

    /// EnableWindow. Returns previous enabled state.
    bool enable_window(windowing::HWND hwnd, bool enable);

    void set_painting_manager(PaintingManager* mgr) { m_paint_mgr = mgr; }
    void set_message_loop_manager(MessageLoopManager* mgr) { m_msgloop_mgr = mgr; }
    void set_message_send_manager(MessageSendManager* mgr) { m_msgsend_mgr = mgr; }
    void set_user32_ext_manager(User32ExtManager* mgr) { m_user32_ext_mgr = mgr; }
    windowing::HwndManager& hwnd_mgr() { return m_hwnd_mgr; }
    windowing::ControlHost& control_host() { return m_control_host; }
    windowing::MessageQueue& queue() { return m_queue; }

private:
    void send_or_post(windowing::HWND hwnd, u64 wndproc, u32 msg, u64 wParam, u64 lParam);
    void invalidate_visible_descendants(windowing::HWND parent);
    void update_owned_popups_for_owner(windowing::HWND owner, bool show);

    windowing::HwndManager& m_hwnd_mgr;
    windowing::WindowHost* m_window_host;
    windowing::ControlHost& m_control_host;
    windowing::MessageQueue& m_queue;
    PaintingManager* m_paint_mgr{nullptr};
    MessageLoopManager* m_msgloop_mgr{nullptr};
    MessageSendManager* m_msgsend_mgr{nullptr};
    User32ExtManager* m_user32_ext_mgr{nullptr};
};

// Thunks
void impl_ShowWindow(Marshaler& m, ShowWindowManager& sm);
void impl_UpdateWindow(Marshaler& m, ShowWindowManager& sm);
void impl_IsWindowVisible(Marshaler& m, ShowWindowManager& sm);
void impl_EnableWindow(Marshaler& m, ShowWindowManager& sm);

[[nodiscard]] DllExports make_show_window_exports(ShowWindowManager& sm);

} // namespace vitreo::winapi

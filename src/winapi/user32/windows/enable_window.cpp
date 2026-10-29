#include "winapi/user32/windows/show_window.hpp"

namespace vitreo::winapi {

bool ShowWindowManager::enable_window(windowing::HWND hwnd, bool enable) {
    windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
    if (win == nullptr) return false;

    const bool previously_enabled = win->enabled;
    win->enabled = enable;
    (void)m_control_host.set_control_enabled(hwnd, enable);

    if (enable != previously_enabled && win->wndproc != 0) {
        send_or_post(hwnd, win->wndproc, windowing::WM::ENABLE,
                     enable ? 1u : 0u, 0);
    }
    return previously_enabled;
}

} // namespace vitreo::winapi

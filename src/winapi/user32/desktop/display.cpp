// SPDX-License-Identifier: LGPL-2.1-only

/// @file user32_display.cpp
/// @brief Monitors, display topology, and display-mode changes.
///
/// The USER surface that answers "which screen" rather than "which window":
/// which monitor a point or window falls on, what the topology looks like to
/// CCD (QueryDisplayConfig / DisplayConfigGetDeviceInfo), which adapters and
/// modes EnumDisplayDevices and EnumDisplaySettings report, and what
/// ChangeDisplaySettings does about it — a virtual fullscreen window, since
/// Vitreo never changes the host's real display mode.
///
/// Split out of user32_ext.cpp. This region was contiguous and closed: the two
/// anonymous namespaces it carries define 38 helpers, and nothing outside used
/// any of them, nor did anything here need a file-local helper from elsewhere.
/// That is what made it a cut rather than a gather.
///
/// Window stations and desktops were adjacent to this in user32_ext.cpp and
/// came out with it; they now live in user32_station.cpp.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/common/virtual_display_modes.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "vitreo/result.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {

// Learn: MONITORINFO=40; MONITORINFOEXA=72 (CHAR[32]); MONITORINFOEXW=104 (WCHAR[32]).
constexpr u32 kMonitorInfoSize = 40;
constexpr u32 kMonitorInfoExASize = 72;
constexpr u32 kMonitorInfoExWSize = 104;
constexpr u32 kMonitorHandleBase = 0x000D0001u;
constexpr u32 ERROR_INVALID_MONITOR_HANDLE = 1461;
constexpr u32 ERROR_INVALID_FLAGS = 1004;
constexpr u32 MONITOR_DEFAULTTONULL = 0;
constexpr u32 MONITOR_DEFAULTTOPRIMARY = 1;
constexpr u32 MONITOR_DEFAULTTONEAREST = 2;

i64 overlap(const windowing::MonitorInfo& mi, const windowing::Rect& r) {
    const i64 w = std::min<i64>(mi.right, r.right) - std::max<i64>(mi.left, r.left);
    const i64 h = std::min<i64>(mi.bottom, r.bottom) - std::max<i64>(mi.top, r.top);
    return w > 0 && h > 0 ? w * h : 0;
}

u64 distance2(const windowing::MonitorInfo& mi, const windowing::Rect& r) {
    const i64 dx = r.right <= mi.left ? i64{mi.left} - r.right + 1
                 : r.left >= mi.right ? i64{r.left} - mi.right + 1 : 0;
    const i64 dy = r.bottom <= mi.top ? i64{mi.top} - r.bottom + 1
                 : r.top >= mi.bottom ? i64{r.top} - mi.bottom + 1 : 0;
    return static_cast<u64>(dx * dx + dy * dy);
}

} // namespace

// ---------------------------------------------------------------------------
// Monitors
//
// Contract: Microsoft Learn MonitorFromPoint / MonitorFromRect /
// MonitorFromWindow / GetMonitorInfo / EnumDisplayMonitors / MonitorEnumProc.
// Measured on Windows 11 (tools/oracle_probe/s1win_display_probe.c
// "-- monitors", "-- monitorfrom", "-- getmonitorinfo"):
//  * dwFlags above MONITOR_DEFAULTTONEAREST: NULL / ERROR_INVALID_FLAGS;
//  * MonitorFromWindow of an invalid window: NULL / ERROR_INVALID_WINDOW_HANDLE
//    whatever the flags; a NULL window is "on no monitor"; a minimized
//    window counts at its restored rectangle; the desktop is the primary;
//  * GetMonitorInfo accepts exactly MONITORINFO or MONITORINFOEX{A,W}
//    (ERROR_INVALID_PARAMETER otherwise) and fails an unknown HMONITOR with
//    ERROR_INVALID_MONITOR_HANDLE; success leaves the last error alone;
//  * EnumDisplayMonitors returns FALSE when the callback stops it, and with
//    a clip rectangle passes the part of each monitor inside it.
// The primary monitor is the one at (0,0), first in the list.
// ---------------------------------------------------------------------------

std::vector<windowing::MonitorInfo> User32ExtManager::guest_monitors() const {
    // Exclusive / virtual fullscreen (ChangeDisplaySettings, D3D9 exclusive,
    // DirectDraw DDSCL_FULLSCREEN) remaps the guest desktop to one primary
    // monitor covering the chosen mode.
    if (m_virtual_display_width > 0 && m_virtual_display_height > 0) {
        windowing::MonitorInfo mi{};
        mi.left = mi.top = mi.work_left = mi.work_top = 0;
        mi.right = mi.work_right = mi.width = m_virtual_display_width;
        mi.bottom = mi.work_bottom = mi.height = m_virtual_display_height;
        mi.primary = true;
        return {mi};
    }
    auto list = m_host.get_all_monitor_info();
    // Primary first: it is \\.\DISPLAY1 and HMONITOR #0 everywhere (DXGI
    // EnumOutputs, EnumDisplayDevices, CCD source ids).
    std::stable_partition(list.begin(), list.end(),
                          [](const windowing::MonitorInfo& mi) { return mi.primary; });
    return list;
}

u32 User32ExtManager::monitor_from_rect_rule(const windowing::Rect& r, u32 flags,
                                             bool on_no_monitor) const {
    const auto monitors = guest_monitors();
    if (monitors.empty()) return 0;
    if (!on_no_monitor) {
        i64 best = 0;
        usize best_index = 0;
        for (usize i = 0; i < monitors.size(); ++i) {
            const i64 a = overlap(monitors[i], r);
            if (a > best) { best = a; best_index = i; }
        }
        if (best > 0) return kMonitorHandleBase + static_cast<u32>(best_index);
        // A rectangle without area (or a point) is on the monitor that
        // contains its origin.
        for (usize i = 0; i < monitors.size(); ++i) {
            const auto& mi = monitors[i];
            if (r.left >= mi.left && r.left < mi.right && r.top >= mi.top && r.top < mi.bottom) {
                return kMonitorHandleBase + static_cast<u32>(i);
            }
        }
    }
    if (flags == MONITOR_DEFAULTTONULL) return 0;
    if (flags == MONITOR_DEFAULTTOPRIMARY) return kMonitorHandleBase;
    if (flags == MONITOR_DEFAULTTONEAREST) {
        if (on_no_monitor) return kMonitorHandleBase;
        u64 best = UINT64_MAX;
        usize best_index = 0;
        for (usize i = 0; i < monitors.size(); ++i) {
            const u64 d = distance2(monitors[i], r);
            if (d < best) { best = d; best_index = i; }
        }
        return kMonitorHandleBase + static_cast<u32>(best_index);
    }
    return 0;
}

int User32ExtManager::monitor_index(u64 hmonitor) const {
    const auto monitors = guest_monitors();
    if (hmonitor < kMonitorHandleBase) return -1;
    const u64 index = hmonitor - kMonitorHandleBase;
    return index < monitors.size() ? static_cast<int>(index) : -1;
}

void User32ExtManager::get_monitor_info(Marshaler& m, bool wide) {
    const u64 hmon = m.arg(0);
    const GuestAddr info_ptr = m.arg_ptr(1);
    const auto monitors = guest_monitors();
    const int index = monitor_index(hmon);
    if (index < 0) {
        set_last_error(ERROR_INVALID_MONITOR_HANDLE);
        m.set_return(0u);
        return;
    }
    const u32 cb_in = info_ptr ? m_memory.read_u32(info_ptr).value_or(0) : 0;
    const u32 ex_size = wide ? kMonitorInfoExWSize : kMonitorInfoExASize;
    if (info_ptr == 0 || (cb_in != kMonitorInfoSize && cb_in != ex_size)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const auto& chosen = monitors[static_cast<usize>(index)];
    struct {
        u32 cbSize;
        i32 left, top, right, bottom;       // rcMonitor
        i32 wleft, wtop, wright, wbottom;   // rcWork
        u32 dwFlags;
    } mi{};
    mi.cbSize = cb_in;
    mi.left = chosen.left; mi.top = chosen.top;
    mi.right = chosen.right; mi.bottom = chosen.bottom;
    mi.wleft = chosen.work_left; mi.wtop = chosen.work_top;
    mi.wright = chosen.work_right; mi.wbottom = chosen.work_bottom;
    mi.dwFlags = chosen.primary ? 1u : 0u;  // MONITORINFOF_PRIMARY
    if (m_memory.write_bytes(info_ptr, reinterpret_cast<const Byte*>(&mi), sizeof(mi)).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    // MONITORINFOEX.szDevice names the display. GetMonitorInfo of a
    // MONITORINFOEX-sized buffer fills it so EnumDisplayDevicesW can look
    // the monitor up by that name.
    if (cb_in == ex_size) {
        char name[32];
        std::snprintf(name, sizeof(name), "\\\\.\\DISPLAY%d", index + 1);
        std::vector<Byte> device(wide ? 64 : 32, 0);
        for (usize i = 0; name[i] != '\0' && i < 31; ++i) {
            device[wide ? i * 2u : i] = static_cast<Byte>(static_cast<unsigned char>(name[i]));
        }
        (void)m_memory.write_bytes(info_ptr + kMonitorInfoSize, device.data(), device.size());
    }
    VITREO_TRACE("[user32_ext] GetMonitorInfo%c hmon=0x%llx -> (%d,%d)-(%d,%d)%s cb=%u\n",
                 wide ? 'W' : 'A', static_cast<unsigned long long>(hmon), chosen.left,
                 chosen.top, chosen.right, chosen.bottom, chosen.primary ? " primary" : "",
                 cb_in);
    m.set_return(1u);
}

void User32ExtManager::impl_GetMonitorInfoW(Marshaler& m) { get_monitor_info(m, true); }

void User32ExtManager::impl_MonitorFromPoint(Marshaler& m) {
    // HMONITOR MonitorFromPoint(POINT pt, DWORD dwFlags).
    //
    // POINT is an 8-byte struct passed BY VALUE: Win64 packs it into one
    // register (RCX: x low dword, y high dword) and dwFlags follows in RDX;
    // Win32 stdcall spreads it over two stack DWORDs before dwFlags.
    i32 pt_x = 0, pt_y = 0;
    u32 flags = 0;
    if (m.cpu_is_64bit()) {
        const u64 packed = m.arg(0);
        pt_x = static_cast<i32>(static_cast<u32>(packed));
        pt_y = static_cast<i32>(static_cast<u32>(packed >> 32));
        flags = static_cast<u32>(m.arg(1));
    } else {
        pt_x = static_cast<i32>(static_cast<u32>(m.arg(0)));
        pt_y = static_cast<i32>(static_cast<u32>(m.arg(1)));
        flags = static_cast<u32>(m.arg(2));
    }
    if (flags > MONITOR_DEFAULTTONEAREST) {
        set_last_error(ERROR_INVALID_FLAGS);
        m.set_return(0u);
        return;
    }
    const u32 result = monitor_from_rect_rule(windowing::Rect{pt_x, pt_y, pt_x, pt_y}, flags, false);
    VITREO_TRACE("[user32_ext] MonitorFromPoint(%d,%d flags=0x%x) -> 0x%x\n", pt_x, pt_y,
                 flags, result);
    m.set_return(result);
}

void User32ExtManager::impl_MonitorFromRect(Marshaler& m) {
    const GuestAddr lprc = m.arg_ptr(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    if (flags > MONITOR_DEFAULTTONEAREST) {
        set_last_error(ERROR_INVALID_FLAGS);
        m.set_return(0u);
        return;
    }
    windowing::Rect r{};
    if (lprc) {
        r.left = static_cast<i32>(m_memory.read_u32(lprc).value_or(0));
        r.top = static_cast<i32>(m_memory.read_u32(lprc + 4).value_or(0));
        r.right = static_cast<i32>(m_memory.read_u32(lprc + 8).value_or(0));
        r.bottom = static_cast<i32>(m_memory.read_u32(lprc + 12).value_or(0));
    }
    const u32 result = monitor_from_rect_rule(r, flags, false);
    VITREO_TRACE("[user32_ext] MonitorFromRect(flags=0x%x) -> 0x%x\n", flags, result);
    m.set_return(result);
}

void User32ExtManager::impl_MonitorFromWindow(Marshaler& m) {
    const auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    const u32 flags = static_cast<u32>(m.arg(1));
    if (flags > MONITOR_DEFAULTTONEAREST) {
        set_last_error(ERROR_INVALID_FLAGS);
        m.set_return(0u);
        return;
    }
    u32 result = 0;
    if (hwnd == windowing::NULL_HWND) {
        result = monitor_from_rect_rule({}, flags, true);
    } else if (hwnd == windowing::HWND_DESKTOP) {
        result = kMonitorHandleBase;
    } else if (const windowing::WindowData* win = m_hwnd_mgr.find(hwnd)) {
        // A minimized window is judged by its restored rectangle (measured).
        windowing::Rect r = win->minimized ? win->normal_rect : win->rect;
        // Child rectangles are parent-client relative: map to the screen.
        windowing::HWND cur = win->parent;
        for (u32 guard = 0; guard < 256; ++guard) {
            const windowing::WindowData* p = m_hwnd_mgr.find(cur);
            if (!p || cur == windowing::HWND_DESKTOP) break;
            r.left += p->rect.left + p->client_offset.x;
            r.right += p->rect.left + p->client_offset.x;
            r.top += p->rect.top + p->client_offset.y;
            r.bottom += p->rect.top + p->client_offset.y;
            cur = p->parent;
        }
        result = monitor_from_rect_rule(r, flags, false);
    } else if (ipc::hwnd_registry_is_window(static_cast<u32>(hwnd))) {
        // Another process's window: its screen position is not published,
        // only that it exists, so it is placed on the primary monitor.
        result = kMonitorHandleBase;
    } else {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
    }
    VITREO_TRACE("[user32_ext] MonitorFromWindow(hwnd=0x%x, flags=0x%x) -> 0x%x\n", hwnd,
                 flags, result);
    m.set_return(result);
}

void User32ExtManager::impl_EnumDisplayMonitors(Marshaler& m) {
    const u64 hdc = m.arg(0);
    const GuestAddr clip_rect = m.arg_ptr(1);
    const GuestAddr callback = m.arg_ptr(2);
    const u64 dwData = m.arg(3);

    const auto monitors = guest_monitors();
    if (callback == 0 || !m_guest_call) {
        // Windows dereferences the callback (it faults); report the bad
        // parameter instead of pretending to enumerate.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    // The area enumerated, in the coordinates of @p hdc: the whole virtual
    // screen for no DC or a screen DC, the client area for a window DC.
    windowing::Point dc_origin{0, 0};
    bool dc_limited = false;
    windowing::Rect dc_area{};
    if (hdc != 0 && m_gdi_mgr) {
        const auto hwnd = static_cast<windowing::HWND>(m_gdi_mgr->window_from_dc(static_cast<Handle>(hdc)));
        if (const windowing::WindowData* win = m_hwnd_mgr.find(hwnd);
            win != nullptr && hwnd != windowing::HWND_DESKTOP) {
            windowing::Rect client{0, 0, win->client_rect.width(), win->client_rect.height()};
            windowing::Point origin{win->rect.left + win->client_offset.x,
                                    win->rect.top + win->client_offset.y};
            windowing::HWND cur = win->parent;
            for (u32 guard = 0; guard < 256; ++guard) {
                const windowing::WindowData* p = m_hwnd_mgr.find(cur);
                if (!p || cur == windowing::HWND_DESKTOP) break;
                origin.x += p->rect.left + p->client_offset.x;
                origin.y += p->rect.top + p->client_offset.y;
                cur = p->parent;
            }
            dc_origin = origin;
            dc_area = windowing::Rect{origin.x, origin.y, origin.x + client.right,
                                      origin.y + client.bottom};
            dc_limited = true;
        }
    }
    bool have_clip = false;
    windowing::Rect clip{};
    if (clip_rect != 0) {
        clip.left = static_cast<i32>(m_memory.read_u32(clip_rect + 0).value_or(0)) + dc_origin.x;
        clip.top = static_cast<i32>(m_memory.read_u32(clip_rect + 4).value_or(0)) + dc_origin.y;
        clip.right = static_cast<i32>(m_memory.read_u32(clip_rect + 8).value_or(0)) + dc_origin.x;
        clip.bottom = static_cast<i32>(m_memory.read_u32(clip_rect + 12).value_or(0)) + dc_origin.y;
        have_clip = true;
    }

    auto* cpu = m.cpu();
    const u64 old_sp = cpu->sp();
    const u64 rect_sp = (old_sp - 16) & ~0xFull;
    cpu->set_sp(rect_sp);
    const GuestAddr rect_addr = static_cast<GuestAddr>(rect_sp);
    const bool is_64bit = m.cpu_is_64bit();

    bool stopped = false;
    for (usize i = 0; i < monitors.size(); ++i) {
        const auto& mi = monitors[i];
        windowing::Rect r{mi.left, mi.top, mi.right, mi.bottom};
        auto intersect = [&](const windowing::Rect& o) {
            r.left = std::max(r.left, o.left);
            r.top = std::max(r.top, o.top);
            r.right = std::min(r.right, o.right);
            r.bottom = std::min(r.bottom, o.bottom);
        };
        if (dc_limited) intersect(dc_area);
        if (have_clip) intersect(clip);
        if (r.right <= r.left || r.bottom <= r.top) continue;
        (void)m_memory.write_u32(rect_addr + 0, static_cast<u32>(r.left - dc_origin.x));
        (void)m_memory.write_u32(rect_addr + 4, static_cast<u32>(r.top - dc_origin.y));
        (void)m_memory.write_u32(rect_addr + 8, static_cast<u32>(r.right - dc_origin.x));
        (void)m_memory.write_u32(rect_addr + 12, static_cast<u32>(r.bottom - dc_origin.y));
        const u64 args[4] = {kMonitorHandleBase + static_cast<u32>(i), hdc,
                             static_cast<u64>(rect_addr), dwData};
        if (m_guest_call(callback, args, 4, is_64bit) == 0) {
            stopped = true;
            break;
        }
    }
    cpu->set_sp(old_sp);
    m.set_return(stopped ? 0u : 1u);
}

void User32ExtManager::impl_GetMonitorInfoA(Marshaler& m) { get_monitor_info(m, false); }

namespace {

// CCD / QueryDisplayConfig layouts (wingdi.h). DISPLAYCONFIG_PATH_INFO is
// 72 bytes and DISPLAYCONFIG_MODE_INFO is 64 bytes.
constexpr u32 kCcdErrorSuccess = 0;
constexpr u32 kCcdErrorInvalidParameter = 87;
constexpr u32 kCcdErrorInsufficientBuffer = 122;
constexpr u32 kQdcAllPaths = 0x1;
constexpr u32 kQdcOnlyActivePaths = 0x2;
constexpr u32 kQdcDatabaseCurrent = 0x4;
constexpr u32 kQdcIncludeHmd = 0x20;
constexpr u32 kPathInfoBytes = 72;
constexpr u32 kModeInfoBytes = 64;
constexpr u32 kDisplayConfigPathActive = 0x1;
constexpr u32 kDisplayConfigSourceInUse = 0x1;
constexpr u32 kDisplayConfigTargetInUse = 0x1;
constexpr u32 kModeInfoTypeSource = 1;
constexpr u32 kModeInfoTypeTarget = 2;
constexpr u32 kPixelFormat32Bpp = 4;       // DISPLAYCONFIG_PIXELFORMAT_32BPP
constexpr u32 kOutputTechnologyOther = 0xFFFFFFFFu;  // DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER
constexpr u32 kRotationIdentity = 1;
constexpr u32 kScalingIdentity = 1;
constexpr u32 kScanlineProgressive = 1;
constexpr u32 kDeviceInfoGetSourceName = 1;
constexpr u32 kDeviceInfoGetTargetName = 2;
constexpr u32 kDeviceInfoGetTargetPreferredMode = 3;
constexpr u32 kDeviceInfoGetAdapterName = 4;
constexpr u32 kDeviceInfoGetAdvancedColorInfo = 9;
constexpr u32 kDeviceInfoGetSdrWhiteLevel = 11;
constexpr u32 kAdapterLuidLow = 0x5654524Du; // 'VTRM'
constexpr u32 kAdapterLuidHigh = 1u;
constexpr u32 kTopologyExtend = 0x00000004; // DISPLAYCONFIG_TOPOLOGY_EXTEND

[[nodiscard]] bool ccd_flags_supported(u32 flags) noexcept {
    const u32 base = flags & (kQdcAllPaths | kQdcOnlyActivePaths | kQdcDatabaseCurrent);
    // Exactly one of the three mutually exclusive base flags (Learn).
    const bool one_base =
        base == kQdcAllPaths || base == kQdcOnlyActivePaths ||
        base == kQdcDatabaseCurrent;
    if (!one_base) return false;
    // HMD inclusion is only defined with ONLY_ACTIVE_PATHS.
    if ((flags & kQdcIncludeHmd) != 0 && base != kQdcOnlyActivePaths) {
        return false;
    }
    return true;
}

void write_luid(Byte* dst) noexcept {
    std::memcpy(dst, &kAdapterLuidLow, 4);
    std::memcpy(dst + 4, &kAdapterLuidHigh, 4);
}

void write_u32_bytes(Byte* dst, u32 value) noexcept {
    std::memcpy(dst, &value, 4);
}

void write_u64_bytes(Byte* dst, u64 value) noexcept {
    std::memcpy(dst, &value, 8);
}

void fill_path_info(Byte out[kPathInfoBytes], u32 source_id, u32 target_id,
                    u32 source_mode_idx, u32 target_mode_idx, u32 refresh_milli_hz) noexcept {
    std::memset(out, 0, kPathInfoBytes);
    // DISPLAYCONFIG_PATH_SOURCE_INFO
    write_luid(out + 0);
    write_u32_bytes(out + 8, source_id);
    write_u32_bytes(out + 12, source_mode_idx);
    write_u32_bytes(out + 16, kDisplayConfigSourceInUse);
    // DISPLAYCONFIG_PATH_TARGET_INFO at offset 20
    write_luid(out + 20);
    write_u32_bytes(out + 28, target_id);
    write_u32_bytes(out + 32, target_mode_idx);
    write_u32_bytes(out + 36, kOutputTechnologyOther);
    write_u32_bytes(out + 40, kRotationIdentity);
    write_u32_bytes(out + 44, kScalingIdentity);
    write_u32_bytes(out + 48, refresh_milli_hz); // refreshRate.Numerator
    write_u32_bytes(out + 52, 1000);             // refreshRate.Denominator
    write_u32_bytes(out + 56, kScanlineProgressive);
    write_u32_bytes(out + 60, 1); // targetAvailable = TRUE
    write_u32_bytes(out + 64, kDisplayConfigTargetInUse);
    write_u32_bytes(out + 68, kDisplayConfigPathActive);
}

void fill_source_mode(Byte out[kModeInfoBytes], u32 source_id,
                      const windowing::MonitorInfo& mi) noexcept {
    std::memset(out, 0, kModeInfoBytes);
    write_u32_bytes(out + 0, kModeInfoTypeSource);
    write_u32_bytes(out + 4, source_id);
    write_luid(out + 8);
    write_u32_bytes(out + 16, static_cast<u32>(mi.width));
    write_u32_bytes(out + 20, static_cast<u32>(mi.height));
    write_u32_bytes(out + 24, kPixelFormat32Bpp);
    write_u32_bytes(out + 28, static_cast<u32>(mi.left));  // position.x
    write_u32_bytes(out + 32, static_cast<u32>(mi.top));   // position.y
}

u32 refresh_milli_hz(const windowing::MonitorInfo& mi) noexcept {
    return mi.refresh_rate_hz >= 1.0 ? static_cast<u32>(mi.refresh_rate_hz * 1000.0 + 0.5)
                                     : 60000u;
}

void fill_target_mode(Byte out[kModeInfoBytes], u32 target_id,
                      const windowing::MonitorInfo& mi) noexcept {
    std::memset(out, 0, kModeInfoBytes);
    write_u32_bytes(out + 0, kModeInfoTypeTarget);
    write_u32_bytes(out + 4, target_id);
    write_luid(out + 8);
    // DISPLAYCONFIG_TARGET_MODE.targetVideoSignalInfo
    const u32 milli_hz = refresh_milli_hz(mi);
    const u64 pixel_rate =
        static_cast<u64>(mi.width) * static_cast<u64>(mi.height) * milli_hz / 1000ull;
    write_u64_bytes(out + 16, pixel_rate);
    write_u32_bytes(out + 24, milli_hz * static_cast<u32>(mi.height) / 1000u);  // hSync
    write_u32_bytes(out + 28, 1);
    write_u32_bytes(out + 32, milli_hz);  // vSyncFreq
    write_u32_bytes(out + 36, 1000);
    write_u32_bytes(out + 40, static_cast<u32>(mi.width));  // activeSize.cx
    write_u32_bytes(out + 44, static_cast<u32>(mi.height)); // activeSize.cy
    write_u32_bytes(out + 48, static_cast<u32>(mi.width));  // totalSize.cx
    write_u32_bytes(out + 52, static_cast<u32>(mi.height)); // totalSize.cy
    // additionalSignalInfo: videoStandard D3DKMDT_VSS_OTHER (255),
    // vSyncFreqDivider 1 (measured 0x000100FF).
    write_u32_bytes(out + 56, 0x000100FFu);
    write_u32_bytes(out + 60, kScanlineProgressive);
}

/// The monitor's PnP interface path (DisplayConfigGetDeviceInfo
/// GET_TARGET_NAME.monitorDevicePath and EnumDisplayDevices with
/// EDD_GET_DEVICE_INTERFACE_NAME).
std::string monitor_interface_path(usize index) {
    char path[128];
    std::snprintf(path, sizeof(path),
                  "\\\\?\\DISPLAY#VTR%04X#1&%08x&0&UID%zu#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}",
                  static_cast<unsigned>(index), static_cast<unsigned>(0x5654524Du + index), index);
    return path;
}

void write_ascii_as_utf16(GuestMemory& memory, GuestAddr addr, usize wchar_cap,
                          const char* ascii) {
    std::vector<Byte> buf(wchar_cap * 2u, 0);
    for (usize i = 0; i + 1 < wchar_cap && ascii[i] != '\0'; ++i) {
        buf[i * 2u] = static_cast<Byte>(static_cast<unsigned char>(ascii[i]));
    }
    (void)memory.write_bytes(addr, buf.data(), buf.size());
}

} // namespace

// ---------------------------------------------------------------------------
// CCD: GetDisplayConfigBufferSizes / QueryDisplayConfig /
// DisplayConfigGetDeviceInfo / DisplayConfigSetDeviceInfo
//
// Contract: Microsoft Learn (wingdi.h layouts). One active path per guest
// monitor, primary first; each path has a target mode (index 2i) and a
// source mode (2i+1), as Windows orders them. Measured
// (s1win_display_probe "-- ccd"):
//  * exactly one of QDC_ALL_PATHS / QDC_ONLY_ACTIVE_PATHS /
//    QDC_DATABASE_CURRENT, else ERROR_INVALID_PARAMETER (returned, not set);
//  * QueryDisplayConfig with a zero element count is ERROR_INVALID_PARAMETER,
//    a nonzero but short one ERROR_INSUFFICIENT_BUFFER; a NULL array or a
//    NULL / unexpected topology pointer is ERROR_INVALID_PARAMETER (and the
//    NULL cases also set the last error to ERROR_NOACCESS);
//  * DisplayConfigGetDeviceInfo / SetDeviceInfo: a wrong header size or an
//    unknown type is ERROR_INVALID_PARAMETER, an unknown adapter or id
//    ERROR_GEN_FAILURE, a NULL header ERROR_GEN_FAILURE with the last error
//    ERROR_NOACCESS;
//  * source mode pixelFormat 32BPP (4); target outputTechnology OTHER.
// ---------------------------------------------------------------------------

void User32ExtManager::impl_GetDisplayConfigBufferSizes(Marshaler& m) {
    // LONG GetDisplayConfigBufferSizes(UINT32 flags, UINT32* numPathArrayElements,
    //                                  UINT32* numModeInfoArrayElements)
    const u32 flags = static_cast<u32>(m.arg(0));
    const GuestAddr p_paths = m.arg_ptr(1);
    const GuestAddr p_modes = m.arg_ptr(2);
    if (p_paths == 0 || p_modes == 0 || !ccd_flags_supported(flags)) {
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    const u32 n = static_cast<u32>(guest_monitors().size());
    (void)m.write_u32(p_paths, n);
    (void)m.write_u32(p_modes, n * 2u);
    m.set_return(kCcdErrorSuccess);
}

void User32ExtManager::impl_QueryDisplayConfig(Marshaler& m) {
    // LONG QueryDisplayConfig(UINT32 flags, UINT32* numPathArrayElements,
    //   DISPLAYCONFIG_PATH_INFO*, UINT32* numModeInfoArrayElements,
    //   DISPLAYCONFIG_MODE_INFO*, DISPLAYCONFIG_TOPOLOGY_ID*)
    const u32 flags = static_cast<u32>(m.arg(0));
    const GuestAddr p_num_paths = m.arg_ptr(1);
    const GuestAddr path_array = m.arg_ptr(2);
    const GuestAddr p_num_modes = m.arg_ptr(3);
    const GuestAddr mode_array = m.arg_ptr(4);
    const GuestAddr topology_ptr = m.arg_ptr(5);

    if (!ccd_flags_supported(flags) || p_num_paths == 0 || p_num_modes == 0) {
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    const bool want_topology = (flags & kQdcDatabaseCurrent) != 0;
    if (path_array == 0 || mode_array == 0 || (want_topology && topology_ptr == 0)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    if (!want_topology && topology_ptr != 0) {
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }

    const auto monitors = guest_monitors();
    const u32 n = static_cast<u32>(monitors.size());
    const u32 have_paths = m_memory.read_u32(p_num_paths).value_or(0);
    const u32 have_modes = m_memory.read_u32(p_num_modes).value_or(0);
    if (have_paths == 0 || have_modes == 0) {
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    if (have_paths < n || have_modes < n * 2u) {
        m.set_return(kCcdErrorInsufficientBuffer);
        return;
    }

    for (u32 i = 0; i < n; ++i) {
        Byte path[kPathInfoBytes];
        fill_path_info(path, i, i, i * 2u + 1u, i * 2u, refresh_milli_hz(monitors[i]));
        (void)m_memory.write_bytes(path_array + i * kPathInfoBytes, path, kPathInfoBytes);
        Byte target_mode[kModeInfoBytes];
        fill_target_mode(target_mode, i, monitors[i]);
        (void)m_memory.write_bytes(mode_array + (i * 2u) * kModeInfoBytes, target_mode,
                                   kModeInfoBytes);
        Byte source_mode[kModeInfoBytes];
        fill_source_mode(source_mode, i, monitors[i]);
        (void)m_memory.write_bytes(mode_array + (i * 2u + 1u) * kModeInfoBytes, source_mode,
                                   kModeInfoBytes);
    }
    (void)m.write_u32(p_num_paths, n);
    (void)m.write_u32(p_num_modes, n * 2u);
    if (want_topology) {
        (void)m.write_u32(topology_ptr, n > 1 ? kTopologyExtend : 0x1u /* INTERNAL */);
    }
    m.set_return(kCcdErrorSuccess);
}

void User32ExtManager::impl_DisplayConfigGetDeviceInfo(Marshaler& m) {
    // LONG DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER*)
    constexpr u32 kErrorGenFailure = 31;
    const GuestAddr header = m.arg_ptr(0);
    if (header == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(kErrorGenFailure);
        return;
    }
    const u32 type = m_memory.read_u32(header + 0).value_or(0);
    const u32 size = m_memory.read_u32(header + 4).value_or(0);
    const u32 luid_low = m_memory.read_u32(header + 8).value_or(0);
    const u32 luid_high = m_memory.read_u32(header + 12).value_or(0);
    const u32 id = m_memory.read_u32(header + 16).value_or(0);
    const auto monitors = guest_monitors();

    u32 expected = 0;
    switch (type) {
    case kDeviceInfoGetSourceName: expected = 84; break;
    case kDeviceInfoGetTargetName: expected = 420; break;
    case kDeviceInfoGetTargetPreferredMode: expected = 80; break;
    case kDeviceInfoGetAdapterName: expected = 276; break;
    case kDeviceInfoGetAdvancedColorInfo: expected = 32; break;
    case kDeviceInfoGetSdrWhiteLevel: expected = 24; break;
    default:
        VITREO_TRACE("[user32_ext] DisplayConfigGetDeviceInfo type=%u unsupported\n", type);
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    if (size != expected) {
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    if (luid_low != kAdapterLuidLow || luid_high != kAdapterLuidHigh ||
        (type != kDeviceInfoGetAdapterName && id >= monitors.size())) {
        m.set_return(kErrorGenFailure);
        return;
    }
    std::vector<Byte> zeros(size - 20, 0);
    (void)m_memory.write_bytes(header + 20, zeros.data(), zeros.size());
    switch (type) {
    case kDeviceInfoGetAdapterName:
        write_ascii_as_utf16(
            m_memory, header + 20, 128,
            "\\\\?\\ROOT#VITREO_METAL#0000#{1ca05180-a699-450a-9a0c-de4fbe3dfa89}");
        break;
    case kDeviceInfoGetSourceName: {
        char name[32];
        std::snprintf(name, sizeof(name), "\\\\.\\DISPLAY%u", id + 1u);
        write_ascii_as_utf16(m_memory, header + 20, 32, name);
        break;
    }
    case kDeviceInfoGetTargetName: {
        // flags(4) outputTechnology(4) edidManufactureId(2) edidProductCodeId(2)
        // connectorInstance(4) monitorFriendlyDeviceName[64] monitorDevicePath[128].
        // The friendly name is Vitreo's, not from an EDID: friendlyNameForced.
        (void)m.write_u32(header + 20, 0x2u);
        (void)m.write_u32(header + 24, kOutputTechnologyOther);
        char friendly[64];
        std::snprintf(friendly, sizeof(friendly), "Display %u", id + 1u);
        write_ascii_as_utf16(m_memory, header + 36, 64, friendly);
        write_ascii_as_utf16(m_memory, header + 164, 128,
                             monitor_interface_path(id).c_str());
        break;
    }
    case kDeviceInfoGetTargetPreferredMode: {
        // width, height, then DISPLAYCONFIG_TARGET_MODE at 32.
        (void)m.write_u32(header + 20, static_cast<u32>(monitors[id].width));
        (void)m.write_u32(header + 24, static_cast<u32>(monitors[id].height));
        Byte mode[kModeInfoBytes];
        fill_target_mode(mode, id, monitors[id]);
        (void)m_memory.write_bytes(header + 32, mode + 16, 48);
        break;
    }
    case kDeviceInfoGetAdvancedColorInfo:
        // No advanced colour: value 0, colorEncoding RGB, the panel's depth.
        (void)m.write_u32(header + 28, monitors[id].bits_per_color);
        break;
    case kDeviceInfoGetSdrWhiteLevel:
        // Learn: 1000 is the standard 80-nit SDR white level.
        (void)m.write_u32(header + 20, 1000);
        break;
    default:
        break;
    }
    m.set_return(kCcdErrorSuccess);
}

void User32ExtManager::impl_DisplayConfigSetDeviceInfo(Marshaler& m) {
    // LONG DisplayConfigSetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_HEADER*)
    constexpr u32 kErrorGenFailure = 31;
    constexpr u32 kErrorNotSupported = 50;
    const GuestAddr header = m.arg_ptr(0);
    if (header == 0) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(kErrorGenFailure);
        return;
    }
    const u32 type = m_memory.read_u32(header + 0).value_or(0);
    const u32 size = m_memory.read_u32(header + 4).value_or(0);
    // The SET types and their structure sizes (wingdi.h):
    // SET_TARGET_PERSISTENCE 5 (24), SET_SUPPORT_VIRTUAL_RESOLUTION 8 (24),
    // SET_ADVANCED_COLOR_STATE 10 (24), SET_MONITOR_SPECIALIZATION 13 (292).
    u32 expected = 0;
    switch (type) {
    case 5: case 8: case 10: expected = 24; break;
    case 13: expected = 292; break;
    default:
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    if (size != expected) {
        m.set_return(kCcdErrorInvalidParameter);
        return;
    }
    // Changing the host display's topology or colour state is not possible.
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED user32!DisplayConfigSetDeviceInfo: type %u "
                     "(host display state is not changed)\n",
                     type);
    }
    m.set_return(kErrorNotSupported);
}

// ---------------------------------------------------------------------------
// EnumDisplayDevices{A,W}
//
// Contract: Microsoft Learn EnumDisplayDevicesW / DISPLAY_DEVICEW. Measured
// (s1win_display_probe "-- displaydevices"): DISPLAY_DEVICE.cb is not
// validated -- 0, 100 and the ANSI size all succeed, the call writing at most
// cb bytes; success and "no more devices" both leave the last error alone;
// an unknown device name is FALSE; unknown dwFlags bits are ignored;
// EDD_GET_DEVICE_INTERFACE_NAME turns a monitor's DeviceID into its
// \\?\DISPLAY#... interface path, the same one DisplayConfigGetDeviceInfo
// reports as monitorDevicePath. The primary adapter has
// DISPLAY_DEVICE_PRIMARY_DEVICE.
// ---------------------------------------------------------------------------

namespace {

constexpr u32 EDD_GET_DEVICE_INTERFACE_NAME = 0x1;
constexpr u32 DISPLAY_DEVICE_ATTACHED_TO_DESKTOP = 0x1;
constexpr u32 DISPLAY_DEVICE_PRIMARY_DEVICE = 0x4;
constexpr u32 DISPLAY_DEVICE_ACTIVE = 0x1;
constexpr u32 DISPLAY_DEVICE_ATTACHED = 0x2;

/// "\\.\DISPLAYn" -> n-1, or -1.
int display_index_from_name(const std::string& name, usize count) {
    constexpr std::string_view prefix = "\\\\.\\DISPLAY";
    if (name.size() <= prefix.size()) return -1;
    for (usize i = 0; i < prefix.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(name[i])) !=
            std::toupper(static_cast<unsigned char>(prefix[i]))) {
            return -1;
        }
    }
    int n = 0;
    for (usize i = prefix.size(); i < name.size(); ++i) {
        if (name[i] < '0' || name[i] > '9') return -1;
        n = n * 10 + (name[i] - '0');
        if (n > 1000) return -1;
    }
    return n >= 1 && static_cast<usize>(n) <= count ? n - 1 : -1;
}

} // namespace

bool User32ExtManager::read_device_name(Marshaler& m, GuestAddr ptr, bool wide,
                                        std::string& out) {
    if (ptr == 0) return false;
    auto r = wide ? m.read_wide_string(ptr) : m.read_ansi_string(ptr);
    if (r.is_err()) return false;
    out = r.value();
    return true;
}

void User32ExtManager::enum_display_devices(Marshaler& m, bool wide) {
    // BOOL EnumDisplayDevices(LPCTSTR lpDevice, DWORD iDevNum,
    //                         PDISPLAY_DEVICE lpDisplayDevice, DWORD dwFlags)
    const GuestAddr device_ptr = m.arg_ptr(0);
    u32 dev_num = static_cast<u32>(m.arg(1));
    const GuestAddr dd = m.arg_ptr(2);
    const u32 flags = static_cast<u32>(m.arg(3));
    if (dd == 0) {
        // Windows writes through the pointer (it faults); fail instead.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const u32 cb = m_memory.read_u32(dd).value_or(0);
    const auto monitors = guest_monitors();

    // DISPLAY_DEVICE{A,W}: cb, DeviceName[32], DeviceString[128], StateFlags,
    // DeviceID[128], DeviceKey[128].
    const u32 ch = wide ? 2u : 1u;
    const u32 full = 4 + 32 * ch + 128 * ch + 4 + 128 * ch + 128 * ch;  // 840 / 424
    std::vector<u8> image(full, 0);
    auto put_str = [&](u32 off, u32 cap, const std::string& text) {
        for (usize i = 0; i < text.size() && i + 1 < cap; ++i) {
            image[off + i * ch] = static_cast<u8>(text[i]);
        }
    };
    const u32 name_off = 4, string_off = 4 + 32 * ch, state_off = 4 + 160 * ch;
    const u32 id_off = state_off + 4, key_off = id_off + 128 * ch;
    std::memcpy(image.data(), &cb, 4);

    std::string device;
    if (device_ptr == 0) {
        if (dev_num >= monitors.size()) {
            m.set_return(0u);  // no more adapters; last error untouched
            return;
        }
        const auto& identity = display_device_identity::adapter_identity();
        const std::string description = identity.description
            ? *identity.description
            : (m_display_adapter_name_provider ? m_display_adapter_name_provider()
                                               : std::string{"Vitreo Metal Adapter"});
        put_str(name_off, 32, "\\\\.\\DISPLAY" + std::to_string(dev_num + 1));
        put_str(string_off, 128, description);
        u32 state = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP;
        if (monitors[dev_num].primary) state |= DISPLAY_DEVICE_PRIMARY_DEVICE;
        std::memcpy(image.data() + state_off, &state, 4);
        put_str(id_off, 128, display_device_identity::INSTANCE_ID_A);
        put_str(key_off, 128, display_device_identity::DEVICE_KEY_A);
    } else {
        if (!read_device_name(m, device_ptr, wide, device)) {
            m.set_return(0u);
            return;
        }
        int index = display_index_from_name(device, monitors.size());
        if (index < 0 && device.find("ROOT#VITREO_METAL") != std::string::npos) {
            // The CCD adapter path: its monitors are indexed by iDevNum.
            if (dev_num < monitors.size()) {
                index = static_cast<int>(dev_num);
                dev_num = 0;
            }
        }
        // One monitor per display device.
        if (index < 0 || dev_num != 0) {
            m.set_return(0u);
            return;
        }
        put_str(name_off, 32, "\\\\.\\DISPLAY" + std::to_string(index + 1) + "\\Monitor0");
        put_str(string_off, 128, "Generic PnP Monitor");
        const u32 state = DISPLAY_DEVICE_ACTIVE | DISPLAY_DEVICE_ATTACHED;
        std::memcpy(image.data() + state_off, &state, 4);
        char id[128];
        if (flags & EDD_GET_DEVICE_INTERFACE_NAME) {
            std::snprintf(id, sizeof(id), "%s", monitor_interface_path(static_cast<usize>(index)).c_str());
        } else {
            std::snprintf(id, sizeof(id),
                          "MONITOR\\VTR%04X\\{4d36e96e-e325-11ce-bfc1-08002be10318}\\%04d",
                          static_cast<unsigned>(index), index);
        }
        put_str(id_off, 128, id);
        char key[128];
        std::snprintf(key, sizeof(key),
                      "\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\"
                      "{4d36e96e-e325-11ce-bfc1-08002be10318}\\%04d",
                      index);
        put_str(key_off, 128, key);
    }
    const u32 n = std::min(cb, full);
    if (n > 4 && m_memory.write_bytes(dd + 4, image.data() + 4, n - 4).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

void User32ExtManager::impl_EnumDisplayDevicesW(Marshaler& m) { enum_display_devices(m, true); }

void User32ExtManager::apply_virtual_display_mode(windowing::HWND hwnd,
                                                   u32 width, u32 height,
                                                   u32 bits_per_pixel) {
    // width/height 0 restores the physical desktop metrics.
    if (width == 0 || height == 0) {
        const windowing::HWND old_target = m_virtual_fullscreen_hwnd;
        m_virtual_fullscreen_hwnd = windowing::NULL_HWND;
        m_virtual_display_width = 0;
        m_virtual_display_height = 0;
        if (m_gdi_mgr) {
            m_gdi_mgr->set_virtual_screen_size(0, 0);
        }
        i32 phys_w = 1920;
        i32 phys_h = 1080;
        m_host.get_screen_size(phys_w, phys_h);
        if (windowing::WindowData* desktop =
                m_hwnd_mgr.find(windowing::HWND_DESKTOP)) {
            desktop->rect = {0, 0, phys_w, phys_h};
            desktop->client_rect = desktop->rect;
        }
        VITREO_TRACE(
            "[user32_ext] virtual display restore hwnd=0x%x -> %dx%d\n",
            old_target, phys_w, phys_h);
        return;
    }

    m_virtual_fullscreen_hwnd = hwnd;
    m_virtual_display_width = static_cast<i32>(width);
    m_virtual_display_height = static_cast<i32>(height);
    if (m_gdi_mgr) {
        m_gdi_mgr->set_virtual_screen_size(static_cast<i32>(width),
                                           static_cast<i32>(height));
    }
    if (windowing::WindowData* desktop =
            m_hwnd_mgr.find(windowing::HWND_DESKTOP)) {
        desktop->rect = {0, 0, static_cast<i32>(width),
                         static_cast<i32>(height)};
        desktop->client_rect = desktop->rect;
    }
    if (hwnd != windowing::NULL_HWND) {
        size_window_to_display(hwnd, {0, 0, static_cast<i32>(width),
                                      static_cast<i32>(height)});
        broadcast_display_change(bits_per_pixel, width, height);
        post_display_size(hwnd, width, height);
    }
    VITREO_TRACE(
        "[user32_ext] virtual display hwnd=0x%x mode=%ux%ux%u\n",
        hwnd, width, height, bits_per_pixel);
}

void User32ExtManager::size_window_to_display(windowing::HWND hwnd,
                                              const windowing::Rect& rect) {
    if (windowing::WindowData* win = m_hwnd_mgr.find(hwnd)) {
        win->rect = rect;
        win->client_rect = rect;
    }
    // Keep the Cocoa logical frame on the same guest surface as
    // GetClientRect / GetSystemMetrics. set_frame is a no-op for the
    // native borderless window while exclusive fullscreen is on.
    if (m_host.has_native_window(hwnd)) {
        m_host.set_frame(hwnd, rect);
    }
    if (m_gdi_mgr) {
        m_gdi_mgr->ensure_framebuffer_size(hwnd, rect.width(), rect.height());
    }
}

void User32ExtManager::broadcast_display_change(u32 bits_per_pixel, u32 width,
                                                u32 height) {
    constexpr u32 WM_DISPLAYCHANGE = 0x007E;
    const u64 size_param =
        (static_cast<u64>(height & 0xFFFFu) << 16) | (width & 0xFFFFu);
    // WM_DISPLAYCHANGE is a system-sent broadcast, not ordinary queued
    // work. Microsoft documents synchronous delivery to every top-level
    // window, with non-top-level windows receiving posted copies. Keeping
    // this message queued until the application's next Peek/GetMessage
    // lets guest initialization continue against the old display state
    // even though SetDisplayMode has already returned.
    const auto top_levels = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    for (windowing::HWND top : top_levels) {
        const windowing::WindowData* top_data = m_hwnd_mgr.find(top);
        if (m_msgsend_mgr && top_data && top_data->wndproc != 0) {
            (void)m_msgsend_mgr->send_message(
                top, WM_DISPLAYCHANGE, bits_per_pixel, size_param);
        } else {
            m_queue.post(windowing::MSG{
                top, WM_DISPLAYCHANGE, bits_per_pixel, size_param, 0, {}});
        }

        std::vector<windowing::HWND> pending = m_hwnd_mgr.enum_children(top);
        while (!pending.empty()) {
            const windowing::HWND child = pending.back();
            pending.pop_back();
            m_queue.post(windowing::MSG{
                child, WM_DISPLAYCHANGE, bits_per_pixel, size_param, 0, {}});
            auto descendants = m_hwnd_mgr.enum_children(child);
            pending.insert(pending.end(), descendants.begin(),
                           descendants.end());
        }
    }
}

void User32ExtManager::post_display_size(windowing::HWND hwnd, u32 width,
                                         u32 height) {
    const u64 size_param =
        (static_cast<u64>(height & 0xFFFFu) << 16) | (width & 0xFFFFu);
    m_queue.post(windowing::MSG{hwnd, windowing::WM::SIZE, 0, size_param, 0, {}});
    if (m_paint_mgr) {
        m_paint_mgr->invalidate_rect(static_cast<u64>(hwnd), nullptr, false);
    }
}

// ---------------------------------------------------------------------------
// DXGI exclusive full-screen (IDXGISwapChain::SetFullscreenState).
//
// Contract: Learn "DXGI Overview" (Choosing the DXGI Output and Size,
// Switching Modes), "DXGI best practices" (Full-Screen Issues) and
// DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH. A chain that may switch modes
// publishes the chosen mode through the same virtual display mode DirectDraw
// and D3D9 exclusive mode use; one that may not keeps the desktop mode and
// only covers the output. Leaving restores the desktop mode (with its
// WM_DISPLAYCHANGE, which the restore path of apply_virtual_display_mode does
// not send for its legacy callers) and the window's own rectangle.
// ---------------------------------------------------------------------------

bool User32ExtManager::enter_exclusive_display(windowing::HWND hwnd,
                                               const windowing::Rect& output,
                                               u32 width, u32 height,
                                               bool change_mode) {
    if (m_hwnd_mgr.find(hwnd) == nullptr || width == 0 || height == 0) {
        return false;
    }
    if (change_mode) {
        apply_virtual_display_mode(hwnd, width, height, 32);
        return true;
    }
    // Leaving a mode an earlier ResizeTarget selected for a desktop-sized
    // one is a resolution change too.
    if (m_virtual_display_width > 0 && m_virtual_display_height > 0) {
        apply_virtual_display_mode(windowing::NULL_HWND, 0, 0, 0);
        broadcast_display_change(32, static_cast<u32>(output.width()),
                                 static_cast<u32>(output.height()));
    }
    size_window_to_display(hwnd, output);
    post_display_size(hwnd, static_cast<u32>(output.width()),
                      static_cast<u32>(output.height()));
    VITREO_TRACE("[user32_ext] exclusive display hwnd=0x%x covers %d,%d %dx%d "
                 "(desktop mode)\n",
                 hwnd, output.left, output.top, output.width(),
                 output.height());
    return true;
}

void User32ExtManager::leave_exclusive_display(
    windowing::HWND hwnd, bool mode_changed,
    const std::optional<windowing::Rect>& restore) {
    if (mode_changed && m_virtual_display_width > 0 &&
        m_virtual_display_height > 0) {
        apply_virtual_display_mode(windowing::NULL_HWND, 0, 0, 0);
        // The desktop mode as GetSystemMetrics(SM_CXSCREEN) reports it now.
        const windowing::MonitorInfo primary = m_host.get_primary_monitor_info();
        broadcast_display_change(
            32, static_cast<u32>(std::max(0, primary.right - primary.left)),
            static_cast<u32>(std::max(0, primary.bottom - primary.top)));
    }
    if (restore && m_hwnd_mgr.find(hwnd) != nullptr) {
        // SWP_FRAMECHANGED: the exclusive geometry had no non-client area,
        // so the client rectangle is recomputed from the style even when
        // the outer size happens to match.
        constexpr u32 SWP_NOZORDER = 0x0004;
        constexpr u32 SWP_NOACTIVATE = 0x0010;
        constexpr u32 SWP_FRAMECHANGED = 0x0020;
        (void)apply_set_window_pos(hwnd, windowing::NULL_HWND, restore->left,
                                   restore->top, restore->width(),
                                   restore->height(),
                                   SWP_NOZORDER | SWP_NOACTIVATE |
                                       SWP_FRAMECHANGED,
                                   true);
    }
    VITREO_TRACE("[user32_ext] exclusive display left hwnd=0x%x mode_changed=%d\n",
                 hwnd, mode_changed ? 1 : 0);
}

// ---------------------------------------------------------------------------
// EnumDisplaySettings{A,W,ExA,ExW}
//
// Contract: Microsoft Learn EnumDisplaySettingsW / DEVMODEW. Measured
// (s1win_display_probe "-- displaysettings"): the device name must be NULL
// or a display device (anything else is FALSE); DEVMODE.dmSize on entry is
// not validated and on return is Windows' own (188 for DEVMODEW, 124 for
// DEVMODEA), dmSpecVersion DM_SPECVERSION; the current / registry mode
// reports its position, orientation and fixed-output fields
// (dmFields 0x207C00A0), an enumerated mode its orientation (0x007C0080);
// past the last mode is FALSE with the last error untouched; unknown dwFlags
// are ignored.
//
// The modes are the ones Vitreo can honour: the display's own device-pixel
// resolution first, then the standard smaller ones, each 32 bpp. A caller
// that walks this list is choosing a mode for ChangeDisplaySettings, which
// implements them as a virtual full-screen window, so every listed mode is
// genuinely settable. The list is non-empty.
// ---------------------------------------------------------------------------

namespace {

constexpr u32 ENUM_CURRENT_SETTINGS = 0xFFFFFFFFu;
constexpr u32 ENUM_REGISTRY_SETTINGS = 0xFFFFFFFEu;
constexpr u32 DM_POSITION = 0x00000020;
constexpr u32 DM_DISPLAYORIENTATION = 0x00000080;
constexpr u32 DM_BITSPERPEL = 0x00040000;
constexpr u32 DM_PELSWIDTH = 0x00080000;
constexpr u32 DM_PELSHEIGHT = 0x00100000;
constexpr u32 DM_DISPLAYFLAGS = 0x00200000;
constexpr u32 DM_DISPLAYFREQUENCY = 0x00400000;
constexpr u32 DM_DISPLAYFIXEDOUTPUT = 0x20000000;
constexpr u16 DM_SPECVERSION = 0x0401;

struct DevmodeLayout {
    u32 size_off, extra_off, spec_off, fields_off, position_off, orientation_off,
        fixed_output_off, bits_off, width_off, height_off, flags_off, freq_off, windows_size;
};
constexpr DevmodeLayout kDevmodeW{68, 70, 64, 72, 76, 84, 88, 168, 172, 176, 180, 184, 188};
constexpr DevmodeLayout kDevmodeA{36, 38, 32, 40, 44, 52, 56, 104, 108, 112, 116, 120, 124};

u32 refresh_hz(const windowing::MonitorInfo& mi) {
    const double hz = mi.refresh_rate_hz;
    return hz >= 1.0 ? static_cast<u32>(hz + 0.5) : 60u;
}

} // namespace

void User32ExtManager::monitor_mode(usize index, u32& width, u32& height) const {
    const auto monitors = guest_monitors();
    if (m_virtual_display_width > 0 && m_virtual_display_height > 0) {
        width = static_cast<u32>(m_virtual_display_width);
        height = static_cast<u32>(m_virtual_display_height);
        return;
    }
    if (index == 0) {
        // The desktop mode is the primary's backing framebuffer, not Cocoa
        // points -- otherwise Retina panels omit 1920x1080 and native modes.
        i32 w = 1920, h = 1080;
        m_host.get_screen_pixel_size(w, h);
        width = static_cast<u32>(w);
        height = static_cast<u32>(h);
        return;
    }
    const auto& mi = monitors[index];
    width = static_cast<u32>((mi.right - mi.left) * (mi.scale_factor > 0 ? mi.scale_factor : 1.0) + 0.5);
    height = static_cast<u32>((mi.bottom - mi.top) * (mi.scale_factor > 0 ? mi.scale_factor : 1.0) + 0.5);
}

void User32ExtManager::enum_display_settings(Marshaler& m, bool wide) {
    const GuestAddr device_ptr = m.arg_ptr(0);
    const u32 mode_num = static_cast<u32>(m.arg(1));
    const GuestAddr devmode = m.arg_ptr(2);
    if (devmode == 0) {
        // Windows writes through the pointer (it faults); fail instead.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    const auto monitors = guest_monitors();
    if (monitors.empty()) {
        m.set_return(0u);
        return;
    }
    usize index = 0;
    if (device_ptr != 0) {
        std::string name;
        if (!read_device_name(m, device_ptr, wide, name)) {
            m.set_return(0u);
            return;
        }
        const int i = display_index_from_name(name, monitors.size());
        if (i < 0) {
            m.set_return(0u);
            return;
        }
        index = static_cast<usize>(i);
    }

    u32 desk_w = 0, desk_h = 0;
    monitor_mode(index, desk_w, desk_h);
    const bool current = mode_num == ENUM_CURRENT_SETTINGS || mode_num == ENUM_REGISTRY_SETTINGS;
    u32 width = desk_w, height = desk_h;
    if (!current) {
        i32 host_w = static_cast<i32>(desk_w), host_h = static_cast<i32>(desk_h);
        if (index == 0 && m_virtual_display_width > 0) m_host.get_screen_pixel_size(host_w, host_h);
        const auto modes = virtual_display_modes(static_cast<u32>(host_w), static_cast<u32>(host_h));
        if (mode_num >= modes.size()) {
            m.set_return(0u);  // past the end; last error untouched (x64)
            return;
        }
        width = modes[mode_num].width;
        height = modes[mode_num].height;
    }

    const DevmodeLayout& L = wide ? kDevmodeW : kDevmodeA;
    // DEVMODE is an OUT parameter: clear the caller-declared size, and never
    // less than Windows' own size. Unwritten fields must not leak the
    // caller's stack contents, dmPosition included.
    const u32 declared = m_memory.read_u16(devmode + L.size_off).value_or(0);
    const u32 clear = std::clamp<u32>(declared, L.windows_size, 1024u);
    std::vector<Byte> zeros(clear, 0);
    (void)m_memory.write_bytes(devmode, zeros.data(), zeros.size());
    (void)m.write_u16(devmode + L.spec_off, DM_SPECVERSION);
    (void)m.write_u16(devmode + L.size_off, static_cast<u16>(L.windows_size));
    u32 fields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS |
                 DM_DISPLAYFREQUENCY | DM_DISPLAYORIENTATION;
    if (current) {
        fields |= DM_POSITION | DM_DISPLAYFIXEDOUTPUT;
        (void)m.write_u32(devmode + L.position_off, static_cast<u32>(monitors[index].left));
        (void)m.write_u32(devmode + L.position_off + 4, static_cast<u32>(monitors[index].top));
    }
    (void)m.write_u32(devmode + L.fields_off, fields);
    (void)m.write_u32(devmode + L.bits_off, 32);
    (void)m.write_u32(devmode + L.width_off, width);
    (void)m.write_u32(devmode + L.height_off, height);
    (void)m.write_u32(devmode + L.flags_off, 0);
    (void)m.write_u32(devmode + L.freq_off, refresh_hz(monitors[index]));
    VITREO_TRACE("[u32ext] EnumDisplaySettings%s dev=%zu mode=%u -> %ux%u@32\n",
                 wide ? "W" : "A", index, mode_num, width, height);
    m.set_return(1u);
}

void User32ExtManager::impl_EnumDisplaySettingsA(Marshaler& m) { enum_display_settings(m, false); }
void User32ExtManager::impl_EnumDisplaySettingsW(Marshaler& m) { enum_display_settings(m, true); }
void User32ExtManager::impl_EnumDisplaySettingsExA(Marshaler& m) {
    (void)m.arg(3);  // EDS_RAWMODE / EDS_ROTATEDMODE: every mode is raw and upright
    enum_display_settings(m, false);
}
void User32ExtManager::impl_EnumDisplaySettingsExW(Marshaler& m) {
    (void)m.arg(3);
    enum_display_settings(m, true);
}

void User32ExtManager::impl_EnumDisplayDevicesA(Marshaler& m) { enum_display_devices(m, false); }

// ---------------------------------------------------------------------------
// ChangeDisplaySettings{A,W,ExA,ExW}
//
// Contract: Microsoft Learn ChangeDisplaySettingsExW. Measured
// (s1win_display_probe "-- changedisplaysettings"): a NULL mode restores the
// registry mode (DISP_CHANGE_SUCCESSFUL, also with CDS_TEST); unknown flags
// are DISP_CHANGE_BADFLAGS, an unknown device DISP_CHANGE_BADPARAM, a DEVMODE
// too small to hold the mode DISP_CHANGE_BADMODE; a DEVMODE naming no field
// changes nothing and succeeds; CDS_TEST validates without changing anything.
// A mode must be one EnumDisplaySettings lists (the RDP reference session
// accepts anything; a real display driver rejects unsupported modes with
// DISP_CHANGE_BADMODE).
//
// Vitreo never reprograms the host display: a mode is applied to the guest
// as a virtual full-screen desktop on the largest visible top-level window
// (CDS_FULLSCREEN also makes that window full screen), which is what
// GetSystemMetrics, GetMonitorInfo and EnumDisplaySettings then report.
// With no window to present it on, the change fails (DISP_CHANGE_FAILED).
// CDS_UPDATEREGISTRY is not persisted beyond the process.
// ---------------------------------------------------------------------------

i32 User32ExtManager::change_display_settings(Marshaler& m, GuestAddr device_ptr,
                                              GuestAddr devmode, bool wide, u32 flags) {
    constexpr i32 DISP_CHANGE_SUCCESSFUL = 0;
    constexpr i32 DISP_CHANGE_FAILED = -1;
    constexpr i32 DISP_CHANGE_BADMODE = -2;
    constexpr i32 DISP_CHANGE_NOTUPDATED = -3;
    constexpr i32 DISP_CHANGE_BADFLAGS = -4;
    constexpr i32 DISP_CHANGE_BADPARAM = -5;
    constexpr u32 CDS_UPDATEREGISTRY = 0x00000001;
    constexpr u32 CDS_TEST = 0x00000002;
    constexpr u32 CDS_FULLSCREEN = 0x00000004;
    constexpr u32 CDS_NORESET = 0x10000000;
    constexpr u32 kValidFlags = 0x00000001 | 0x00000002 | 0x00000004 | 0x00000008 |
                                0x00000010 | 0x00000020 | 0x00000040 | 0x00000100 |
                                0x00000200 | 0x10000000 | 0x20000000 | 0x40000000;

    if ((flags & ~kValidFlags) != 0) return DISP_CHANGE_BADFLAGS;
    const auto monitors = guest_monitors();
    usize index = 0;
    if (device_ptr != 0) {
        std::string name;
        if (!read_device_name(m, device_ptr, wide, name)) return DISP_CHANGE_BADPARAM;
        const int i = display_index_from_name(name, monitors.size());
        if (i < 0) return DISP_CHANGE_BADPARAM;
        index = static_cast<usize>(i);
    }

    if (devmode == 0) {
        if ((flags & CDS_TEST) == 0) {
            const windowing::HWND old_target = m_virtual_fullscreen_hwnd;
            if (old_target != windowing::NULL_HWND) {
                m_host.set_window_fullscreen(old_target, false);
            }
            apply_virtual_display_mode(windowing::NULL_HWND, 0, 0, 0);
        }
        return DISP_CHANGE_SUCCESSFUL;
    }

    const DevmodeLayout& L = wide ? kDevmodeW : kDevmodeA;
    const u32 size = m.read_u16(devmode + L.size_off).value_or(0);
    if (size < L.height_off + 4) return DISP_CHANGE_BADMODE;
    const u32 fields = m.read_u32(devmode + L.fields_off).value_or(0);
    u32 cur_w = 0, cur_h = 0;
    monitor_mode(index, cur_w, cur_h);
    if (m_virtual_display_width > 0 && index == 0) {
        i32 w = 0, h = 0;
        m_host.get_screen_pixel_size(w, h);
        cur_w = static_cast<u32>(w);
        cur_h = static_cast<u32>(h);
    }
    const u32 width = (fields & DM_PELSWIDTH) ? m.read_u32(devmode + L.width_off).value_or(0) : cur_w;
    const u32 height = (fields & DM_PELSHEIGHT) ? m.read_u32(devmode + L.height_off).value_or(0) : cur_h;
    const u32 bits = (fields & DM_BITSPERPEL) ? m.read_u32(devmode + L.bits_off).value_or(0) : 32;
    const u32 freq = (fields & DM_DISPLAYFREQUENCY) ? m.read_u32(devmode + L.freq_off).value_or(0) : 0;

    bool listed = false;
    for (const auto& mode : virtual_display_modes(cur_w, cur_h)) {
        listed = listed || (mode.width == width && mode.height == height);
    }
    const bool bits_ok = bits == 0 || bits == 8 || bits == 16 || bits == 24 || bits == 32;
    const bool freq_ok = freq <= 1 || freq == 60 || freq == refresh_hz(monitors[index]);
    if (!listed || !bits_ok || !freq_ok) return DISP_CHANGE_BADMODE;
    if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if ((flags & (CDS_UPDATEREGISTRY | CDS_NORESET)) == (CDS_UPDATEREGISTRY | CDS_NORESET)) {
        // Registry-only update: nothing to persist it in.
        return DISP_CHANGE_NOTUPDATED;
    }
    if (index != 0) {
        // Only the primary display can carry the virtual desktop.
        return DISP_CHANGE_FAILED;
    }
    if (width == cur_w && height == cur_h) {
        // The desktop's own mode: leave any virtual mode.
        if (m_virtual_display_width > 0) {
            const windowing::HWND old_target = m_virtual_fullscreen_hwnd;
            if (old_target != windowing::NULL_HWND) m_host.set_window_fullscreen(old_target, false);
            apply_virtual_display_mode(windowing::NULL_HWND, 0, 0, 0);
        }
        return DISP_CHANGE_SUCCESSFUL;
    }

    windowing::HWND target = windowing::NULL_HWND;
    i64 largest_area = -1;
    for (windowing::HWND candidate : m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP)) {
        const windowing::WindowData* win = m_hwnd_mgr.find(candidate);
        if (!win || !win->visible || (win->style & windowing::WS::CHILD) != 0) continue;
        const i64 area = static_cast<i64>(win->rect.width()) * win->rect.height();
        if (area > largest_area) {
            largest_area = area;
            target = candidate;
        }
    }
    if (target == windowing::NULL_HWND) return DISP_CHANGE_FAILED;
    if (flags & CDS_FULLSCREEN) m_host.set_window_fullscreen(target, true);
    apply_virtual_display_mode(target, width, height, bits);
    return DISP_CHANGE_SUCCESSFUL;
}

void User32ExtManager::impl_ChangeDisplaySettingsA(Marshaler& m) {
    // LONG ChangeDisplaySettingsA(DEVMODEA*, DWORD dwFlags)
    const GuestAddr devmode = m.arg_ptr(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    m.set_return(static_cast<u32>(change_display_settings(m, 0, devmode, false, flags)));
}

void User32ExtManager::impl_ChangeDisplaySettingsW(Marshaler& m) {
    const GuestAddr devmode = m.arg_ptr(0);
    const u32 flags = static_cast<u32>(m.arg(1));
    m.set_return(static_cast<u32>(change_display_settings(m, 0, devmode, true, flags)));
}

void User32ExtManager::impl_ChangeDisplaySettingsExA(Marshaler& m) {
    // LONG ChangeDisplaySettingsExA(LPCSTR, DEVMODEA*, HWND, DWORD, LPVOID)
    const GuestAddr device = m.arg_ptr(0);
    const GuestAddr devmode = m.arg_ptr(1);
    const u32 flags = static_cast<u32>(m.arg(3));
    m.set_return(static_cast<u32>(change_display_settings(m, device, devmode, false, flags)));
}

void User32ExtManager::impl_ChangeDisplaySettingsExW(Marshaler& m) {
    const GuestAddr device = m.arg_ptr(0);
    const GuestAddr devmode = m.arg_ptr(1);
    const u32 flags = static_cast<u32>(m.arg(3));
    m.set_return(static_cast<u32>(change_display_settings(m, device, devmode, true, flags)));
}

} // namespace vitreo::winapi

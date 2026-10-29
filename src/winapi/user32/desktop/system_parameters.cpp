// SPDX-License-Identifier: LGPL-2.1-only

/// @file system_parameters.cpp
/// @brief SystemParametersInfoA / SystemParametersInfoW /
/// SystemParametersInfoForDpi.
///
/// Contract: Microsoft Learn SystemParametersInfoW / SystemParametersInfoForDpi
/// (actions and their pvParam structures, winuser.h layouts). What Learn
/// leaves open is measured on Windows 11 by
/// tools/oracle_probe/s1win_display_probe.c ("-- spi", .expected):
///  * uiParam is ignored for NONCLIENTMETRICS, ICONTITLELOGFONT and the
///    accessibility structures; their cbSize must be exact (NONCLIENTMETRICS
///    may be the pre-Vista size), otherwise FALSE with the last error
///    untouched;
///  * a NULL pvParam where one is required is FALSE / ERROR_NOACCESS;
///  * an unknown action is FALSE / ERROR_INVALID_PARAMETER on x64, action 0
///    ERROR_INVALID_SPI_VALUE; the 32-bit (WOW64) side reports
///    ERROR_CALL_NOT_IMPLEMENTED for both;
///  * NONCLIENTMETRICS for an image older than subsystem 6.0 folds the
///    padded border into iBorderWidth (5, padded 0); a Vista+ image gets 1
///    and 4 (s1win_display_probe64v6);
///  * SystemParametersInfoForDpi accepts only the three GET actions above,
///    scales them with MulDiv(value, dpi, 96), treats dpi 0 as 96, and fails
///    anything else with ERROR_INVALID_PARAMETER;
///  * setting STICKYKEYS / TOGGLEKEYS / FILTERKEYS to their current values
///    succeeds.
/// The values are the reference machine's (an RDP session of Windows 11:
/// most UI effects off), except where the host decides: caption heights follow
/// the native-chrome policy (nc_metrics.hpp), font smoothing is standard
/// anti-aliasing (Vitreo's GDI has no ClearType), client-area animation is on.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 SPI_GETBEEP = 0x0001;
constexpr u32 SPI_GETMOUSE = 0x0003;
constexpr u32 SPI_GETKEYBOARDSPEED = 0x000A;
constexpr u32 SPI_GETSCREENSAVETIMEOUT = 0x000E;
constexpr u32 SPI_GETSCREENSAVEACTIVE = 0x0010;
constexpr u32 SPI_GETKEYBOARDDELAY = 0x0016;
constexpr u32 SPI_GETICONTITLELOGFONT = 0x001F;
constexpr u32 SPI_GETMENUDROPALIGNMENT = 0x001B;
constexpr u32 SPI_GETDRAGFULLWINDOWS = 0x0026;
constexpr u32 SPI_GETNONCLIENTMETRICS = 0x0029;
constexpr u32 SPI_GETICONMETRICS = 0x002D;
constexpr u32 SPI_GETWORKAREA = 0x0030;
constexpr u32 SPI_GETFILTERKEYS = 0x0032;
constexpr u32 SPI_SETFILTERKEYS = 0x0033;
constexpr u32 SPI_GETTOGGLEKEYS = 0x0034;
constexpr u32 SPI_SETTOGGLEKEYS = 0x0035;
constexpr u32 SPI_GETMOUSEKEYS = 0x0036;
constexpr u32 SPI_GETSTICKYKEYS = 0x003A;
constexpr u32 SPI_SETSTICKYKEYS = 0x003B;
constexpr u32 SPI_GETHIGHCONTRAST = 0x0042;
constexpr u32 SPI_GETKEYBOARDPREF = 0x0044;
constexpr u32 SPI_GETSCREENREADER = 0x0046;
constexpr u32 SPI_GETANIMATION = 0x0048;
constexpr u32 SPI_GETFONTSMOOTHING = 0x004A;
constexpr u32 SPI_GETMOUSETRAILS = 0x005E;
constexpr u32 SPI_GETSNAPTODEFBUTTON = 0x005F;
constexpr u32 SPI_GETMOUSEHOVERWIDTH = 0x0062;
constexpr u32 SPI_GETMOUSEHOVERHEIGHT = 0x0064;
constexpr u32 SPI_GETMOUSEHOVERTIME = 0x0066;
constexpr u32 SPI_GETWHEELSCROLLLINES = 0x0068;
constexpr u32 SPI_GETMENUSHOWDELAY = 0x006A;
constexpr u32 SPI_GETWHEELSCROLLCHARS = 0x006C;
constexpr u32 SPI_GETSHOWIMEUI = 0x006E;
constexpr u32 SPI_GETMOUSESPEED = 0x0070;
constexpr u32 SPI_GETSCREENSAVERRUNNING = 0x0072;
constexpr u32 SPI_GETDESKWALLPAPER = 0x0073;
constexpr u32 SPI_GETSNAPARRANGE = 0x0082;
constexpr u32 SPI_GETDOCKMOVING = 0x0090;
constexpr u32 SPI_GETACTIVEWINDOWTRACKING = 0x1000;
constexpr u32 SPI_GETMENUANIMATION = 0x1002;
constexpr u32 SPI_GETCOMBOBOXANIMATION = 0x1004;
constexpr u32 SPI_GETLISTBOXSMOOTHSCROLLING = 0x1006;
constexpr u32 SPI_GETGRADIENTCAPTIONS = 0x1008;
constexpr u32 SPI_GETKEYBOARDCUES = 0x100A;
constexpr u32 SPI_GETHOTTRACKING = 0x100E;
constexpr u32 SPI_GETMENUFADE = 0x1012;
constexpr u32 SPI_GETSELECTIONFADE = 0x1014;
constexpr u32 SPI_GETTOOLTIPANIMATION = 0x1016;
constexpr u32 SPI_GETTOOLTIPFADE = 0x1018;
constexpr u32 SPI_GETCURSORSHADOW = 0x101A;
constexpr u32 SPI_GETMOUSESONAR = 0x101C;
constexpr u32 SPI_GETMOUSEVANISH = 0x1020;
constexpr u32 SPI_GETDROPSHADOW = 0x1024;
constexpr u32 SPI_GETUIEFFECTS = 0x103E;
constexpr u32 SPI_GETDISABLEOVERLAPPEDCONTENT = 0x1040;
constexpr u32 SPI_GETCLIENTAREAANIMATION = 0x1042;
constexpr u32 SPI_GETCARETBROWSING = 0x104C;
constexpr u32 SPI_GETSYSTEMLANGUAGEBAR = 0x1050;
constexpr u32 SPI_GETFOREGROUNDLOCKTIMEOUT = 0x2000;
constexpr u32 SPI_GETCARETWIDTH = 0x2006;
constexpr u32 SPI_GETFONTSMOOTHINGTYPE = 0x200A;
constexpr u32 SPI_GETFONTSMOOTHINGCONTRAST = 0x200C;
constexpr u32 SPI_GETFOCUSBORDERWIDTH = 0x200E;
constexpr u32 SPI_GETFOCUSBORDERHEIGHT = 0x2010;
constexpr u32 SPI_GETFONTSMOOTHINGORIENTATION = 0x2012;
constexpr u32 SPI_GETMESSAGEDURATION = 0x2016;
// Learn SystemParametersInfoW: Windows 10+ default is the pointer target.
constexpr u32 SPI_GETMOUSEWHEELROUTING = 0x201C;
constexpr u32 MOUSEWHEEL_ROUTING_MOUSE_POS = 2;

struct DwordSetting {
    u32 action;
    u32 value;
};

/// GET actions whose pvParam receives one BOOL / UINT / DWORD (measured).
constexpr DwordSetting kDwordSettings[] = {
    {SPI_GETBEEP, 1},
    {SPI_GETKEYBOARDSPEED, 31},
    {SPI_GETSCREENSAVETIMEOUT, 0},
    {SPI_GETSCREENSAVEACTIVE, 0},
    {SPI_GETKEYBOARDDELAY, 1},
    {SPI_GETMENUDROPALIGNMENT, 0},
    {SPI_GETDRAGFULLWINDOWS, 1},
    {SPI_GETKEYBOARDPREF, 0},
    {SPI_GETSCREENREADER, 0},
    {SPI_GETFONTSMOOTHING, 1},
    {SPI_GETMOUSETRAILS, 0},
    {SPI_GETSNAPTODEFBUTTON, 0},
    {SPI_GETMOUSEHOVERWIDTH, 4},
    {SPI_GETMOUSEHOVERHEIGHT, 4},
    {SPI_GETMOUSEHOVERTIME, 400},
    {SPI_GETWHEELSCROLLLINES, 3},
    {SPI_GETMENUSHOWDELAY, 400},
    {SPI_GETWHEELSCROLLCHARS, 3},
    {SPI_GETSHOWIMEUI, 1},
    {SPI_GETMOUSESPEED, 10},
    {SPI_GETSCREENSAVERRUNNING, 0},
    {SPI_GETSNAPARRANGE, 1},
    {SPI_GETDOCKMOVING, 1},
    {SPI_GETACTIVEWINDOWTRACKING, 0},
    {SPI_GETMENUANIMATION, 0},
    {SPI_GETCOMBOBOXANIMATION, 0},
    {SPI_GETLISTBOXSMOOTHSCROLLING, 0},
    {SPI_GETGRADIENTCAPTIONS, 1},
    {SPI_GETKEYBOARDCUES, 0},
    {SPI_GETHOTTRACKING, 1},
    {SPI_GETMENUFADE, 0},
    {SPI_GETSELECTIONFADE, 1},
    {SPI_GETTOOLTIPANIMATION, 0},
    {SPI_GETTOOLTIPFADE, 0},
    {SPI_GETCURSORSHADOW, 0},
    {SPI_GETMOUSESONAR, 0},
    {SPI_GETMOUSEVANISH, 1},
    {SPI_GETDROPSHADOW, 1},
    {SPI_GETUIEFFECTS, 0},
    {SPI_GETDISABLEOVERLAPPEDCONTENT, 0},
    {SPI_GETCLIENTAREAANIMATION, 1},     // host compositor animates
    {SPI_GETCARETBROWSING, 0},
    {SPI_GETSYSTEMLANGUAGEBAR, 0},
    {SPI_GETFOREGROUNDLOCKTIMEOUT, 0x7FFFFFFFu},
    {SPI_GETCARETWIDTH, 1},
    {SPI_GETFONTSMOOTHINGTYPE, 1},       // FE_FONTSMOOTHINGSTANDARD (no ClearType)
    {SPI_GETFONTSMOOTHINGCONTRAST, 1200},
    {SPI_GETFOCUSBORDERWIDTH, 1},
    {SPI_GETFOCUSBORDERHEIGHT, 1},
    {SPI_GETFONTSMOOTHINGORIENTATION, 1},
    {SPI_GETMESSAGEDURATION, 5},
    {SPI_GETMOUSEWHEELROUTING, MOUSEWHEEL_ROUTING_MOUSE_POS},
};

/// The accessibility structures: {GET, SET, size, measured default}.
struct AccessibilityStruct {
    u32 get;
    u32 set;
    u32 size;
};
constexpr AccessibilityStruct kAccessibility[] = {
    {SPI_GETSTICKYKEYS, SPI_SETSTICKYKEYS, 8},
    {SPI_GETTOGGLEKEYS, SPI_SETTOGGLEKEYS, 8},
    {SPI_GETFILTERKEYS, SPI_SETFILTERKEYS, 24},
};

i32 scale(i32 value, u32 dpi) {
    const i64 n = static_cast<i64>(value) * static_cast<i64>(dpi);
    return static_cast<i32>(n >= 0 ? (n + 48) / 96 : (n - 48) / 96);
}

/// LOGFONT{A,W} for the Segoe UI 9-point shell font at @p dpi.
void put_logfont(std::vector<u8>& buf, u32 off, bool wide, u32 dpi) {
    const i32 height = -scale(12, dpi);
    const i32 weight = 400;
    std::memcpy(buf.data() + off + 0, &height, 4);
    std::memcpy(buf.data() + off + 16, &weight, 4);
    buf[off + 23] = 1;  // DEFAULT_CHARSET
    static constexpr char kFace[] = "Segoe UI";
    for (u32 i = 0; kFace[i] != '\0'; ++i) {
        if (wide) {
            buf[off + 28 + i * 2] = static_cast<u8>(kFace[i]);
        } else {
            buf[off + 28 + i] = static_cast<u8>(kFace[i]);
        }
    }
}

void log_unimplemented_once(u32 action) {
    static std::mutex mutex;
    static std::set<u32> logged;
    std::lock_guard lock(mutex);
    if (logged.insert(action).second) {
        std::fprintf(stderr,
                     "[vitreo] UNIMPLEMENTED user32!SystemParametersInfo: action 0x%x\n",
                     action);
    }
}

} // namespace

bool User32ExtManager::spi_call(Marshaler& m, bool wide, u32 dpi, bool for_dpi) {
    const u32 action = static_cast<u32>(m.arg(0));
    const u32 ui_param = static_cast<u32>(m.arg(1));
    const GuestAddr pv = m.arg_ptr(2);
    (void)m.arg(3);  // fWinIni: nothing is persisted outside this process
    (void)ui_param;
    const u32 metrics_dpi = dpi == 0 ? 96 : dpi;

    auto fail = [&](u32 error) {
        set_last_error(error);
        return false;
    };
    auto write = [&](const std::vector<u8>& bytes) {
        if (pv == 0 || m_memory.write_bytes(pv, bytes.data(), bytes.size()).is_err()) {
            return fail(error_codes::ERROR_NOACCESS);
        }
        return true;
    };
    auto read_cb = [&]() -> u32 {
        return pv == 0 ? 0u : m_memory.read_u32(pv).value_or(0u);
    };

    if (for_dpi && action != SPI_GETNONCLIENTMETRICS && action != SPI_GETICONTITLELOGFONT &&
        action != SPI_GETICONMETRICS) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }

    bool valid = true;
    switch (action) {
    case SPI_GETNONCLIENTMETRICS: {
        if (pv == 0) return fail(error_codes::ERROR_NOACCESS);
        const u32 lf = wide ? 92u : 60u;
        const u32 full = 44 + 5 * lf;  // 504 (W) / 344 (A), through iPaddedBorderWidth
        const u32 cb = read_cb();
        if (cb != full && cb != full - 4) return false;  // measured: error untouched
        std::vector<u8> buf(cb, 0);
        auto put = [&](u32 off, i32 v) { std::memcpy(buf.data() + off, &v, 4); };
        const bool legacy = !for_dpi && m_legacy_frame_metrics;
        const i32 padded = legacy ? 0 : scale(nc_metrics::kPaddedBorder, metrics_dpi);
        const i32 border = legacy ? nc_metrics::kBorder + nc_metrics::kPaddedBorder
                                  : scale(nc_metrics::kBorder, metrics_dpi);
        const u32 sm_caption = 24 + lf;           // iSmCaptionWidth
        const u32 menu = sm_caption + 8 + lf;     // iMenuWidth
        put(0, static_cast<i32>(cb));
        put(4, border);
        put(8, scale(nc_metrics::kScrollBar, metrics_dpi));
        put(12, scale(nc_metrics::kScrollBar, metrics_dpi));
        put(16, system_metric(30, metrics_dpi, valid));                 // iCaptionWidth
        put(20, system_metric(4, metrics_dpi, valid) - 1);             // iCaptionHeight
        put_logfont(buf, 24, wide, metrics_dpi);                         // lfCaptionFont
        put(sm_caption, system_metric(52, metrics_dpi, valid));        // iSmCaptionWidth
        put(sm_caption + 4, system_metric(51, metrics_dpi, valid) - 1);
        put_logfont(buf, sm_caption + 8, wide, metrics_dpi);            // lfSmCaptionFont
        put(menu, scale(19, metrics_dpi));                               // iMenuWidth
        put(menu + 4, scale(19, metrics_dpi));
        put_logfont(buf, menu + 8, wide, metrics_dpi);                  // lfMenuFont
        put_logfont(buf, menu + 8 + lf, wide, metrics_dpi);             // lfStatusFont
        put_logfont(buf, menu + 8 + 2 * lf, wide, metrics_dpi);         // lfMessageFont
        if (cb == full) put(full - 4, padded);
        return write(buf);
    }
    case SPI_GETICONTITLELOGFONT: {
        std::vector<u8> buf(wide ? 92u : 60u, 0);
        put_logfont(buf, 0, wide, metrics_dpi);
        return write(buf);
    }
    case SPI_GETICONMETRICS: {
        if (pv == 0) return fail(error_codes::ERROR_NOACCESS);
        const u32 size = wide ? 108u : 76u;
        if (read_cb() != size) return false;
        std::vector<u8> buf(size, 0);
        const i32 spacing = scale(75, metrics_dpi);
        const i32 wrap = 1;
        std::memcpy(buf.data() + 0, &size, 4);
        std::memcpy(buf.data() + 4, &spacing, 4);
        std::memcpy(buf.data() + 8, &spacing, 4);
        std::memcpy(buf.data() + 12, &wrap, 4);
        put_logfont(buf, 16, wide, metrics_dpi);
        return write(buf);
    }
    case SPI_GETWORKAREA: {
        const auto mi = m_host.get_primary_monitor_info();
        const bool virtual_mode = m_virtual_display_width > 0 && m_virtual_display_height > 0;
        const i32 rect[4] = {
            virtual_mode ? 0 : mi.work_left, virtual_mode ? 0 : mi.work_top,
            virtual_mode ? m_virtual_display_width : mi.work_right,
            virtual_mode ? m_virtual_display_height : mi.work_bottom};
        std::vector<u8> buf(16);
        std::memcpy(buf.data(), rect, 16);
        return write(buf);
    }
    case SPI_GETMOUSE: {
        const i32 values[3] = {6, 10, 1};
        std::vector<u8> buf(12);
        std::memcpy(buf.data(), values, 12);
        return write(buf);
    }
    case SPI_GETANIMATION: {
        if (pv == 0) return fail(error_codes::ERROR_NOACCESS);
        if (read_cb() != 8) return false;
        const u32 values[2] = {8, 0};  // iMinAnimate off (measured)
        std::vector<u8> buf(8);
        std::memcpy(buf.data(), values, 8);
        return write(buf);
    }
    case SPI_GETHIGHCONTRAST: {
        if (pv == 0) return fail(error_codes::ERROR_NOACCESS);
        const u32 size = m_is_64bit ? 16u : 12u;
        if (read_cb() != size) return false;
        // HIGHCONTRAST: the feature is available but off (measured flags
        // 0x7E: HCF_AVAILABLE | hot-key bits | HCF_INDICATOR); Vitreo has no
        // colour-scheme name to point lpszDefaultScheme at, so it is NULL.
        std::vector<u8> buf(size, 0);
        const u32 hc_flags = 0x7E;
        std::memcpy(buf.data(), &size, 4);
        std::memcpy(buf.data() + 4, &hc_flags, 4);
        return write(buf);
    }
    case SPI_GETDESKWALLPAPER: {
        // No Windows wallpaper on the guest desktop: an empty path.
        std::vector<u8> buf(wide ? 2u : 1u, 0);
        return write(buf);
    }
    case SPI_GETMOUSEKEYS:
        if (pv == 0) return fail(error_codes::ERROR_NOACCESS);
        if (read_cb() != 28) return false;
        {
            std::vector<u8> buf(28, 0);
            const u32 size = 28;
            std::memcpy(buf.data(), &size, 4);
            return write(buf);
        }
    default:
        break;
    }

    for (const auto& a : kAccessibility) {
        if (action != a.get && action != a.set) continue;
        if (pv == 0) return fail(error_codes::ERROR_NOACCESS);
        if (read_cb() != a.size) return false;
        std::lock_guard lock(m_spi_mutex);
        auto it = m_spi_structs.find(a.get);
        if (it == m_spi_structs.end()) {
            std::vector<u8> def(a.size, 0);
            std::memcpy(def.data(), &a.size, 4);
            u32 flags = 0;
            if (a.get == SPI_GETSTICKYKEYS) flags = 0x1FE;
            if (a.get == SPI_GETTOGGLEKEYS) flags = 0x3E;
            if (a.get == SPI_GETFILTERKEYS) {
                flags = 0x7E;
                const u32 timing[4] = {1000, 1000, 500, 0};  // wait, delay, repeat, bounce
                std::memcpy(def.data() + 8, timing, 16);
            }
            std::memcpy(def.data() + 4, &flags, 4);
            it = m_spi_structs.emplace(a.get, std::move(def)).first;
        }
        if (action == a.get) return write(it->second);
        std::vector<u8> incoming(a.size, 0);
        if (m_memory.read_bytes(pv, incoming.data(), incoming.size()).is_err()) {
            return fail(error_codes::ERROR_NOACCESS);
        }
        it->second = std::move(incoming);
        return true;
    }

    for (const auto& s : kDwordSettings) {
        if (s.action != action) continue;
        std::vector<u8> buf(4);
        std::memcpy(buf.data(), &s.value, 4);
        return write(buf);
    }

    // The SET actions, and GET actions outside the measured set, are not
    // modelled: fail as Windows does for an action it does not know.
    if (action != 0) log_unimplemented_once(action);
    if (!m_is_64bit) return fail(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    return fail(action == 0 ? error_codes::ERROR_INVALID_SPI_VALUE
                            : error_codes::ERROR_INVALID_PARAMETER);
}

void User32ExtManager::impl_SystemParametersInfoW(Marshaler& m) {
    m.set_return(spi_call(m, true, 0, false) ? 1u : 0u);
}

void User32ExtManager::impl_SystemParametersInfoA(Marshaler& m) {
    m.set_return(spi_call(m, false, 0, false) ? 1u : 0u);
}

void User32ExtManager::impl_SystemParametersInfoForDpi(Marshaler& m) {
    // BOOL SystemParametersInfoForDpi(UINT, UINT, PVOID, UINT fWinIni, UINT dpi)
    const u32 dpi = static_cast<u32>(m.arg(4));
    m.set_return(spi_call(m, true, dpi, true) ? 1u : 0u);
}

} // namespace vitreo::winapi

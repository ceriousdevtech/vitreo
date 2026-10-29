#include "dwmapi.hpp"

#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <vitreo/virtual_tsc.hpp>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 kEInvalidArg = 0x80070057u;

// MARGINS (uxtheme.h): four signed LONGs describing how far the DWM frame
// extends into the client area on each side.
struct GuestMargins {
    i32 left = 0;
    i32 right = 0;
    i32 top = 0;
    i32 bottom = 0;
};

[[nodiscard]] bool read_margins(Marshaler& m, GuestAddr address,
                                GuestMargins& out) {
    if (address == 0) return false;
    const auto left = m.read_u32(address + 0);
    const auto right = m.read_u32(address + 4);
    const auto top = m.read_u32(address + 8);
    const auto bottom = m.read_u32(address + 12);
    if (left.is_err() || right.is_err() || top.is_err() || bottom.is_err()) {
        return false;
    }
    out.left = static_cast<i32>(left.value());
    out.right = static_cast<i32>(right.value());
    out.top = static_cast<i32>(top.value());
    out.bottom = static_cast<i32>(bottom.value());
    return true;
}

/// Learn DWM_TIMING_INFO: dwmapi.h declares its structures inside
/// pshpack1.h, so the 64-bit DWM_FRAME_COUNT / QPC_TIME members are not
/// padded to 8-byte boundaries and sizeof(DWM_TIMING_INFO) is 292.
constexpr u32 kTimingInfoSize = 292;
constexpr GuestAddr kOffRateRefreshNumerator = 4;
constexpr GuestAddr kOffRateRefreshDenominator = 8;
constexpr GuestAddr kOffQpcRefreshPeriod = 12;
constexpr GuestAddr kOffRateComposeNumerator = 20;
constexpr GuestAddr kOffRateComposeDenominator = 24;
constexpr GuestAddr kOffQpcVBlank = 28;
constexpr GuestAddr kOffCRefresh = 36;
constexpr GuestAddr kOffCDXRefresh = 44;
constexpr GuestAddr kOffQpcCompose = 48;
constexpr GuestAddr kOffCFrame = 56;

constexpr u32 kEHandle = 0x80070006u;
constexpr u32 kEInsufficientBuffer = 0x8007007Au;  // HRESULT_FROM_WIN32(122)
constexpr u32 kErrorInvalidHandle = 6;
constexpr u32 kErrorInvalidParameter = 87;

// DWMWINDOWATTRIBUTE (dwmapi.h).
constexpr u32 kNcRenderingEnabled = 1;
constexpr u32 kNcRenderingPolicy = 2;
constexpr u32 kCaptionButtonBounds = 5;
constexpr u32 kExtendedFrameBounds = 9;
constexpr u32 kCloak = 13;
constexpr u32 kCloaked = 14;
constexpr u32 kVisibleFrameBorderThickness = 37;
constexpr u32 kNcrpDisabled = 1;
constexpr u32 kNcrpEnabled = 2;

/// Attributes DWM only reports: setting one is E_INVALIDARG with
/// ERROR_INVALID_PARAMETER (measured for DWMWA_EXTENDED_FRAME_BOUNDS).
[[nodiscard]] bool get_only_attribute(u32 a) {
    return a == kNcRenderingEnabled || a == kCaptionButtonBounds ||
           a == kExtendedFrameBounds || a == kCloaked ||
           a == kVisibleFrameBorderThickness;
}

/// Settable attributes whose whole effect is on DWM features the host has
/// no counterpart for (live transitions, taskbar thumbnails, Peek, Flip3D,
/// backdrop brushes): accepting them changes nothing observable.
[[nodiscard]] bool accepted_without_host_effect(u32 a) {
    switch (a) {
    case 3:   // DWMWA_TRANSITIONS_FORCEDISABLED
    case 4:   // DWMWA_ALLOW_NCPAINT (the guest owns its non-client painting)
    case 7:   // DWMWA_FORCE_ICONIC_REPRESENTATION
    case 8:   // DWMWA_FLIP3D_POLICY
    case 10:  // DWMWA_HAS_ICONIC_BITMAP
    case 11:  // DWMWA_DISALLOW_PEEK
    case 12:  // DWMWA_EXCLUDED_FROM_PEEK
    case 15:  // DWMWA_FREEZE_REPRESENTATION
    case 16:  // DWMWA_PASSIVE_UPDATE_MODE
    case 17:  // DWMWA_USE_HOSTBACKDROPBRUSH
    case 39:  // measured S_OK on Windows 11 26100; no documented meaning
        return true;
    default:
        return false;
    }
}

/// Settable attributes that change how DWM draws the frame. The host draws
/// its own native frame, so these cannot be honoured.
[[nodiscard]] bool visual_attribute_unsupported(u32 a) {
    switch (a) {
    case 6:   // DWMWA_NONCLIENT_RTL_LAYOUT
    case 19:  // pre-20H1 DWMWA_USE_IMMERSIVE_DARK_MODE
    case 20:  // DWMWA_USE_IMMERSIVE_DARK_MODE
    case 33:  // DWMWA_WINDOW_CORNER_PREFERENCE
    case 34:  // DWMWA_BORDER_COLOR
    case 35:  // DWMWA_CAPTION_COLOR
    case 36:  // DWMWA_TEXT_COLOR
    case 38:  // DWMWA_SYSTEMBACKDROP_TYPE
        return true;
    default:
        return false;
    }
}

struct DwmAttributeState {
    std::mutex mutex;
    std::map<u32, u32> nc_policy;  // hwnd -> DWMNCRENDERINGPOLICY
    std::set<std::pair<u32, u32>> reported;
};

DwmAttributeState& attribute_state() {
    static DwmAttributeState s;
    return s;
}

void report_unimplemented_attribute(u32 attribute, const char* what) {
    auto& st = attribute_state();
    std::lock_guard lock(st.mutex);
    if (!st.reported.insert({attribute, 0}).second) return;
    std::fprintf(stderr,
                 "[vitreo] UNIMPLEMENTED dwmapi!DwmSetWindowAttribute: "
                 "attribute %u (%s)\n", attribute, what);
}

[[nodiscard]] DwmWindowKind classify(const DwmWindowServices& services,
                                     u32 hwnd) {
    if (hwnd == 0) return DwmWindowKind::Invalid;
    return services.classify_window ? services.classify_window(hwnd)
                                    : DwmWindowKind::TopLevel;
}

/// The composition clock must share the guest's QueryPerformanceCounter
/// timebase and units.
[[nodiscard]] u64 qpc_frequency() noexcept {
    return kQpcHz;
}

} // namespace

DllExports create_dwmapi_thunks(DwmClientDrawnFrameFn on_client_drawn,
                               double refresh_rate_hz,
                               DwmWindowServices services) {
    // DWM_TIMING_INFO rates describe the composition monitor, not a fixed
    // 60 Hz virtual display. Millihertz retains fractional display modes.
    const u32 refresh_numerator = std::isfinite(refresh_rate_hz) &&
            refresh_rate_hz >= 1.0 && refresh_rate_hz <= 10000.0
        ? static_cast<u32>(std::llround(refresh_rate_hz * 1000.0)) : 60000u;
    constexpr u32 refresh_denominator = 1000;
    DllExports exports;
    exports.entries.reserve(10);

    // dwmapi.h / Microsoft Learn: HRESULT(DWORD*, BOOL*), WINAPI.
    // No host glass-colorization service is wired yet. Fail explicitly with
    // the correct ABI, leaving both outputs untouched so callers can fall
    // back. A missing-export thunk leaked these two arguments on x86.
    exports.entries.push_back(ExportEntry{
        "DwmGetColorizationColor", 0,
        [](Marshaler& m) {
            static std::once_flag notice;
            std::call_once(notice, [] {
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED dwmapi!DwmGetColorizationColor: "
                    "host glass colorization is unavailable\n");
            });
            m.set_return(0x80004001u); // E_NOTIMPL (winerror.h)
        }, 8
    });

    // HRESULT DwmIsCompositionEnabled(BOOL* pfEnabled)
    exports.entries.push_back(ExportEntry{
        "DwmIsCompositionEnabled", 0,
        [](Marshaler& m) {
            GuestAddr out = m.arg_ptr(0);
            if (out == 0) {
                m.set_return(0x80070057u); // E_INVALIDARG
                return;
            }
            (void)m.write_u32(out, 1);
            m.set_return(0); // S_OK
        },
        4
    });

    // HRESULT DwmEnableComposition(UINT)
    exports.entries.push_back(ExportEntry{
        "DwmEnableComposition", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(0); // S_OK
        },
        4
    });

    // HRESULT DwmSetWindowAttribute(HWND, DWORD, LPCVOID, DWORD)
    // Measured on Windows 11 (s1win_dwm_probe "-- dwm", x64 = x86):
    //  * NULL, unknown or child HWND: E_HANDLE, last error
    //    ERROR_INVALID_HANDLE;
    //  * attribute 0 or past the last one, NULL value, or cbAttribute below
    //    the attribute's size: E_INVALIDARG, last error untouched (a larger
    //    buffer is accepted);
    //  * a get-only attribute: E_INVALIDARG with ERROR_INVALID_PARAMETER;
    //  * success leaves the last error untouched.
    exports.entries.push_back(ExportEntry{
        "DwmSetWindowAttribute", 0,
        [services](Marshaler& m) {
            const auto hwnd = static_cast<u32>(m.arg(0));
            const auto attribute = static_cast<u32>(m.arg32(1));
            const GuestAddr value = m.arg_ptr(2);
            const auto size = static_cast<u32>(m.arg32(3));
            if (classify(services, hwnd) != DwmWindowKind::TopLevel) {
                set_last_error(kErrorInvalidHandle);
                m.set_return(kEHandle);
                return;
            }
            if (attribute == 0 || attribute > 39) {
                m.set_return(kEInvalidArg);
                return;
            }
            if (get_only_attribute(attribute)) {
                set_last_error(kErrorInvalidParameter);
                m.set_return(kEInvalidArg);
                return;
            }
            if (value == 0 || size < 4) {
                m.set_return(kEInvalidArg);
                return;
            }
            const u32 v = m.read_u32(value).value_or(0);
            if (attribute == kNcRenderingPolicy) {
                if (v > 3) {  // DWMNCRP_LAST
                    m.set_return(kEInvalidArg);
                    return;
                }
                auto& st = attribute_state();
                std::lock_guard lock(st.mutex);
                st.nc_policy[hwnd] = v;
                m.set_return(0);
                return;
            }
            if (attribute == kCloak) {
                if (v == 0) {  // uncloaking a window that is never cloaked
                    m.set_return(0);
                    return;
                }
                // Hiding a shown window from the screen without changing
                // WS_VISIBLE needs a host visibility state of its own.
                report_unimplemented_attribute(attribute, "DWMWA_CLOAK");
                m.set_return(kEInvalidArg);
                return;
            }
            if (accepted_without_host_effect(attribute)) {
                m.set_return(0);
                return;
            }
            if (visual_attribute_unsupported(attribute)) {
                // Answered like a Windows build that lacks the attribute.
                report_unimplemented_attribute(
                    attribute, "frame appearance of the host-drawn frame");
                m.set_return(kEInvalidArg);
                return;
            }
            m.set_return(kEInvalidArg);
        },
        16
    });

    // HRESULT DwmGetWindowAttribute(HWND, DWORD, PVOID, DWORD)
    // Measured on Windows 11 (s1win_dwm_probe "-- dwm", x64 = x86):
    //  * NULL, unknown or child HWND: E_HANDLE, ERROR_INVALID_HANDLE;
    //  * an unknown attribute or NULL output: E_INVALIDARG, error untouched;
    //  * a set-only attribute: E_INVALIDARG with ERROR_INVALID_PARAMETER;
    //  * a short buffer: RECT attributes HRESULT_FROM_WIN32(
    //    ERROR_INSUFFICIENT_BUFFER), DWORD attributes E_INVALIDARG (the last
    //    error varied between runs: 0 or 122); a larger buffer is accepted;
    //  * DWMWA_EXTENDED_FRAME_BOUNDS is the window rectangle without the
    //    invisible resize borders; DWMWA_NCRENDERING_ENABLED is FALSE for a
    //    frameless popup and after DWMNCRP_DISABLED; DWMWA_CLOAKED is 0;
    //    DWMWA_VISIBLE_FRAME_BORDER_THICKNESS is 1.
    exports.entries.push_back(ExportEntry{
        "DwmGetWindowAttribute", 0,
        [services](Marshaler& m) {
            const auto hwnd = static_cast<u32>(m.arg(0));
            const auto attribute = static_cast<u32>(m.arg32(1));
            const GuestAddr out = m.arg_ptr(2);
            const auto size = static_cast<u32>(m.arg32(3));
            if (classify(services, hwnd) != DwmWindowKind::TopLevel) {
                set_last_error(kErrorInvalidHandle);
                m.set_return(kEHandle);
                return;
            }
            const bool known = attribute == kNcRenderingEnabled ||
                               attribute == kCaptionButtonBounds ||
                               attribute == kExtendedFrameBounds ||
                               attribute == kCloaked ||
                               attribute == kVisibleFrameBorderThickness;
            if (attribute == 0 || attribute > 39 || out == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            if (!known) {
                // Settable-only attributes (measured for
                // DWMWA_NCRENDERING_POLICY).
                set_last_error(kErrorInvalidParameter);
                m.set_return(kEInvalidArg);
                return;
            }
            const bool rect_attribute = attribute == kCaptionButtonBounds ||
                                        attribute == kExtendedFrameBounds;
            if (size < (rect_attribute ? 16u : 4u)) {
                m.set_return(rect_attribute ? kEInsufficientBuffer : kEInvalidArg);
                return;
            }
            switch (attribute) {
            case kExtendedFrameBounds: {
                i32 rect[4] = {};
                if (!services.frame_bounds || !services.frame_bounds(hwnd, rect)) {
                    set_last_error(kErrorInvalidHandle);
                    m.set_return(kEHandle);
                    return;
                }
                for (u32 i = 0; i < 4; ++i) {
                    (void)m.write_u32(out + i * 4, static_cast<u32>(rect[i]));
                }
                break;
            }
            case kNcRenderingEnabled: {
                u32 policy = 0;  // DWMNCRP_USEWINDOWSTYLE
                {
                    auto& st = attribute_state();
                    std::lock_guard lock(st.mutex);
                    const auto it = st.nc_policy.find(hwnd);
                    if (it != st.nc_policy.end()) policy = it->second;
                }
                const bool framed = services.has_frame ? services.has_frame(hwnd) : true;
                const bool enabled = policy == kNcrpEnabled ||
                                     (policy != kNcrpDisabled && framed);
                (void)m.write_u32(out, enabled ? 1u : 0u);
                break;
            }
            case kCloaked:
                (void)m.write_u32(out, 0);  // nothing is ever cloaked
                break;
            case kVisibleFrameBorderThickness:
                (void)m.write_u32(out, 1);
                break;
            case kCaptionButtonBounds:
            default: {
                // The caption buttons belong to the host's native frame,
                // not to a DWM caption laid out in window coordinates.
                auto& st = attribute_state();
                std::lock_guard lock(st.mutex);
                if (st.reported.insert({attribute, 1}).second) {
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED dwmapi!DwmGetWindowAttribute: "
                                 "DWMWA_CAPTION_BUTTON_BOUNDS\n");
                }
                m.set_return(kEInvalidArg);
                return;
            }
            }
            m.set_return(0);
        },
        16
    });

    // HRESULT DwmExtendFrameIntoClientArea(HWND, const MARGINS*)
    // Learn: negative margins create a "sheet of glass" with no window border;
    // non-zero margins extend the frame into the client (custom title bars).
    // Either case means the guest paints the caption area — the host must not
    // keep a separate native title bar above that chrome.
    exports.entries.push_back(ExportEntry{
        "DwmExtendFrameIntoClientArea", 0,
        [on_client_drawn, services](Marshaler& m) {
            // Measured (s1win_dwm_probe): a NULL or unknown HWND is
            // E_INVALIDARG with ERROR_INVALID_WINDOW_HANDLE, a child window
            // E_INVALIDARG with the error untouched; success sets the last
            // error to 0.
            const auto hwnd = static_cast<u32>(m.arg(0));
            const GuestAddr margins_addr = m.arg_ptr(1);
            GuestMargins margins{};
            const DwmWindowKind kind = classify(services, hwnd);
            if (kind == DwmWindowKind::Invalid) {
                set_last_error(1400);  // ERROR_INVALID_WINDOW_HANDLE
                m.set_return(kEInvalidArg);
                return;
            }
            if (kind == DwmWindowKind::Child ||
                !read_margins(m, margins_addr, margins)) {
                m.set_return(kEInvalidArg);
                return;
            }
            const bool client_drawn = margins.left != 0 || margins.right != 0 ||
                                      margins.top != 0 || margins.bottom != 0;
            if (on_client_drawn) {
                on_client_drawn(hwnd, client_drawn);
            }
            VITREO_TRACE(
                "[dwm] DwmExtendFrameIntoClientArea hwnd=0x%x "
                "margins=(%d,%d,%d,%d) client_drawn=%d\n",
                hwnd, margins.left, margins.right, margins.top, margins.bottom,
                client_drawn ? 1 : 0);
            set_last_error(0);
            m.set_return(0); // S_OK
        },
        8
    });

    // HRESULT DwmFlush(void)
    exports.entries.push_back(ExportEntry{
        "DwmFlush", 0,
        [](Marshaler& m) {
            m.set_return(0); // S_OK
        },
        0
    });

    // HRESULT DwmGetCompositionTimingInfo(HWND hwnd, DWM_TIMING_INFO* pTimingInfo)
    // A missing-export stub with stdcall_bytes=0 corrupts the stack. The
    // caller then terminates with 0xc0000409 (STATUS_STACK_BUFFER_OVERRUN).
    exports.entries.push_back(ExportEntry{
        "DwmGetCompositionTimingInfo", 0,
        [refresh_numerator](Marshaler& m) {
            // Learn: since Windows 8.1 hwnd must be NULL (system-wide
            // timing). Measured (s1win_dwm_probe): a window handle is
            // E_INVALIDARG with the last error set to 0; a wrong cbSize is
            // 0x88980090. (One run reported last error 203 after success and
            // another 0: an internal side effect, not part of the contract.)
            const auto hwnd = m.arg(0);
            const GuestAddr info = m.arg_ptr(1);
            if (hwnd != 0) {
                set_last_error(0);
                m.set_return(kEInvalidArg);
                return;
            }
            if (info == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            const u32 cb_size = m.read_u32(info).value_or(0);
            if (cb_size != kTimingInfoSize) {
                VITREO_TRACE(
                    "[dwm] DwmGetCompositionTimingInfo cbSize=%u "
                    "(expected %u) -> 0x88980090\n",
                    cb_size, kTimingInfoSize);
                m.set_return(0x88980090u);
                return;
            }

            const std::vector<Byte> zeroed(kTimingInfoSize, 0);
            if (!m.memory().write_bytes(info, zeroed.data(),
                                        zeroed.size()).is_ok()) {
                m.set_return(kEInvalidArg);
                return;
            }

            const u64 frequency = qpc_frequency();
            const u64 refresh_period =
                frequency * refresh_denominator / refresh_numerator;
            const u64 now = virtual_qpc_ticks();
            // Report the most recent vertical blank and the refresh counts
            // derived from it, so a caller can phase-lock to composition.
            const u64 refresh_count =
                refresh_period != 0 ? now / refresh_period : 0;
            const u64 qpc_vblank = refresh_count * refresh_period;

            const bool written =
                m.write_u32(info, cb_size).is_ok() &&
                m.write_u32(info + kOffRateRefreshNumerator, refresh_numerator)
                    .is_ok() &&
                m.write_u32(info + kOffRateRefreshDenominator, refresh_denominator).is_ok() &&
                m.write_u64(info + kOffQpcRefreshPeriod, refresh_period)
                    .is_ok() &&
                m.write_u32(info + kOffRateComposeNumerator, refresh_numerator)
                    .is_ok() &&
                m.write_u32(info + kOffRateComposeDenominator, refresh_denominator).is_ok() &&
                m.write_u64(info + kOffQpcVBlank, qpc_vblank).is_ok() &&
                m.write_u64(info + kOffCRefresh, refresh_count).is_ok() &&
                m.write_u32(info + kOffCDXRefresh,
                            static_cast<u32>(refresh_count)).is_ok() &&
                m.write_u64(info + kOffQpcCompose, qpc_vblank).is_ok() &&
                m.write_u64(info + kOffCFrame, refresh_count).is_ok();
            if (!written) {
                m.set_return(kEInvalidArg);
                return;
            }
            m.set_return(0); // S_OK
        },
        8
    });

    // HRESULT DwmEnableBlurBehindWindow(HWND, const DWM_BLURBEHIND*)
    // DWM_BLURBEHIND (dwmapi.h): DWORD dwFlags; BOOL fEnable; HRGN hRgnBlur;
    // BOOL fTransitionOnMaximized -- hRgnBlur at +8 in both bitnesses, the
    // last BOOL at +12 (x86) / +16 (x64). Flags: DWM_BB_ENABLE 1,
    // DWM_BB_BLURREGION 2, DWM_BB_TRANSITIONONMAXIMIZED 4.
    //
    // Measured on Windows 11 in an interactive session (composition on;
    // tools/oracle_probe/gfx_media_exports_probe.expected):
    //   NULL / non-window HWND, NULL struct, child window, dwFlags 0 or with
    //   bits outside 0x7 -> E_INVALIDARG; DWM_BB_BLURREGION with a handle that
    //   is not a region -> HRESULT_FROM_WIN32(ERROR_BAD_FORMAT); else S_OK.
    //   (Without composition, i.e. a session-0 process, the checks that pass
    //   give DWM_E_COMPOSITIONDISABLED; Vitreo always reports composition.)
    exports.entries.push_back(ExportEntry{
        "DwmEnableBlurBehindWindow", 0,
        [services](Marshaler& m) {
            constexpr u32 kBbEnable = 1, kBbBlurRegion = 2, kBbAll = 7;
            constexpr u32 kEBadFormat = 0x8007000Bu;
            const auto hwnd = static_cast<u32>(m.arg(0));
            const GuestAddr bb = m.arg_ptr(1);
            DwmWindowKind kind = hwnd == 0 ? DwmWindowKind::Invalid
                                           : DwmWindowKind::TopLevel;
            if (hwnd != 0 && services.classify_window) {
                kind = services.classify_window(hwnd);
            }
            if (kind != DwmWindowKind::TopLevel || bb == 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            const auto flags = m.read_u32(bb);
            const auto enable = m.read_u32(bb + 4);
            const auto region = m.cpu_is_64bit() ? m.read_u64(bb + 8)
                : m.read_u32(bb + 8).map([](u32 v) { return u64{v}; });
            if (flags.is_err() || enable.is_err() || region.is_err()) {
                m.set_return(kEInvalidArg);
                return;
            }
            if (flags.value() == 0 || (flags.value() & ~kBbAll) != 0) {
                m.set_return(kEInvalidArg);
                return;
            }
            if ((flags.value() & kBbBlurRegion) != 0 && region.value() != 0 &&
                (!services.is_region || !services.is_region(region.value()))) {
                m.set_return(kEBadFormat);
                return;
            }
            if ((flags.value() & kBbEnable) != 0 && enable.value() != 0) {
                // The compositor effect (DWM honours the client area's alpha
                // behind the blur region) has no host-side equivalent yet.
                static bool logged = false;
                if (!logged) {
                    logged = true;
                    std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED dwmapi!DwmEnableBlurBehindWindow: "
                        "blur-behind alpha composition is not applied to the "
                        "host window\n");
                }
            }
            m.set_return(0); // S_OK
        },
        8
    });

    // BOOL DwmDefWindowProc(HWND, UINT, WPARAM, LPARAM, LRESULT*)
    exports.entries.push_back(ExportEntry{
        "DwmDefWindowProc", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            (void)m.arg(2);
            (void)m.arg(3);
            // No DWM caption buttons live in any client area (the host draws
            // its own frame), so DWM never handles the message; measured:
            // FALSE with *plResult left untouched.
            (void)m.arg_ptr(4);
            m.set_return(0); // FALSE = not handled
        },
        20
    });

    // BOOL InitNetworkAddressControl(void)
    // Some apps probe this symbol via a module handle they already have.
    exports.entries.push_back(ExportEntry{
        "InitNetworkAddressControl", 0,
        [](Marshaler& m) {
            m.set_return(1); // TRUE
        },
        0
    });

    return exports;
}

} // namespace vitreo::winapi

#include "winapi/user32/windows/windowpos_layout.hpp"
/// @file window.cpp
/// @brief CreateWindowExW/DestroyWindow implementation (WIN32-051)

#include "window.hpp"
#include "sdk_creation_layouts.hpp"
#include "winapi/user32/ansi_text.hpp"
#include <mutex>
#include "winapi/user32/painting/painting.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/user32_errors.hpp"
#include "winapi/user32/classes/system_classes.hpp"
#include "winapi/user32/windows/min_track.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/cocoa/window_presentation.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <optional>

namespace vitreo::winapi {

static inline bool diag_logs_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_ENABLE_DIAG_LOGS") != nullptr);
    return enabled;
}

static void note_first_top_level_window(windowing::HWND hwnd, u32 style) noexcept {
    if (hwnd == windowing::NULL_HWND) {
        return;
    }
    if ((style & windowing::WS::CHILD) != 0) {
        return;
    }
    diagnostics::phase_mark(diagnostics::LoadPhase::FirstWindow);
}

// Defined in user32_ext.cpp. Forward-declared rather than included: this file
// carries its own file-local `utf8_to_wstring` and `diag_logs_enabled`, and
// pulling in user32_internal.hpp would collide with both.
std::string wstring_to_utf8(const std::wstring& s);
[[nodiscard]] u64 effective_wndproc(const windowing::WindowData& win) noexcept;

static std::wstring utf8_to_wstring(const std::string& s) {
    std::wstring result;
    result.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            result.push_back(static_cast<wchar_t>(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x1F) << 6) |
                         (static_cast<unsigned char>(s[i + 1]) & 0x3F));
            result.push_back(wc);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            wchar_t wc = static_cast<wchar_t>(((c & 0x0F) << 12) |
                         ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                         (static_cast<unsigned char>(s[i + 2]) & 0x3F));
            result.push_back(wc);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            uint32_t cp = (static_cast<uint32_t>(c & 0x07) << 18) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          static_cast<uint32_t>(static_cast<unsigned char>(s[i + 3]) & 0x3F);
            result.push_back(static_cast<wchar_t>(cp));
            i += 4;
        } else {
            // CP1252 fallback for Win32 ANSI bytes (® = 0xAE), not '?'
            if (c >= 0xA0) {
                result.push_back(static_cast<wchar_t>(c));
            } else if (c >= 0x80) {
                static constexpr wchar_t kCp1252_80_9F[32] = {
                    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
                    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
                    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
                    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
                };
                result.push_back(kCp1252_80_9F[c - 0x80]);
            } else {
                result.push_back(static_cast<wchar_t>(c));
            }
            ++i;
        }
    }
    return result;
}

namespace {

[[nodiscard]] std::wstring to_lower_copy(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t ch : s) {
        out.push_back(static_cast<wchar_t>(std::towlower(ch)));
    }
    return out;
}

[[nodiscard]] std::optional<u16> builtin_control_ordinal(std::wstring_view class_name) {
    if (class_name.empty()) {
        return std::nullopt;
    }

    std::wstring lower = to_lower_copy(class_name);
    if (lower == L"button") { return 0x0080; }
    if (lower == L"edit") { return 0x0081; }
    if (lower == L"static") { return 0x0082; }
    if (lower == L"listbox") { return 0x0083; }
    if (lower == L"scrollbar") { return 0x0084; }
    if (lower == L"combobox") { return 0x0085; }
    // Common controls (comctl32) — ordinal 0 tells the bridge to match by name
    if (lower == L"msctls_progress32") { return 0; }
    if (lower == L"syslistview32") { return 0; }
    if (lower == L"systabcontrol32") { return 0; }
    if (lower == L"msctls_trackbar32") { return 0; }
    if (lower == L"msctls_statusbar32") { return 0; }
    if (lower == L"toolbarwindow32") { return 0; }
    if (lower == L"rebarwindow32") { return 0; }
    if (lower == L"tooltips_class32") { return 0; }
    if (lower == L"systreeview32") { return 0; }
    if (lower == L"sysheader32") { return 0; }
    // SysLink (WC_LINK), the comctl32 hyperlink control enabled by ICC_LINK.
    // Its text carries <a href=...>/<a id=...> markup marking the link spans.
    // A caller that cannot create one treats it as fatal: WiX's
    // ThemeLoadControls aborts the whole theme on the NULL HWND, so every
    // other control goes unregistered and the window comes up empty.
    if (lower == L"syslink") { return 0; }
    // Registered by shell32!InitNetworkAddressControl on Windows. Vitreo's
    // native bridge presents it as an edit-like named control.
    if (lower == L"msctls_netaddress") { return 0; }
    // RichEdit (riched20 / msftedit) — ordinal 0 → Cocoa maps by class name.
    if (lower == L"richedit20a" || lower == L"richedit20w" ||
        lower == L"richedit" || lower == L"richedit50w") {
        return 0;
    }
    return std::nullopt;
}

} // namespace

// =============================================================================
// Non-client area helpers
// =============================================================================

/// The Win32 non-client overhead (caption, menu bar, frame), from the one
/// definition AdjustWindowRectEx and DefWindowProc's WM_NCCALCSIZE use
/// (nc_metrics.hpp). The Cocoa content rect is the outer size minus this.
static void compute_nc_insets(u32 style, u32 ex_style, bool has_menu,
                              i32& nc_width, i32& nc_height) {
    const auto in = nc_metrics::insets(style, ex_style, has_menu, false);
    nc_width = in.left + in.right;
    nc_height = in.top + in.bottom;
}

// =============================================================================
// WindowManager
// =============================================================================

WindowManager::WindowManager(
    windowing::HwndManager& hwnd_mgr,
    WindowClassManager& class_mgr,
    windowing::CocoaBridge& bridge,
    windowing::MessageQueue& queue
)
    : m_hwnd_mgr{hwnd_mgr}
    , m_class_mgr{class_mgr}
    , m_bridge{bridge}
    , m_queue{queue}
{}

// -----------------------------------------------------------------------------
// create_window
// -----------------------------------------------------------------------------

bool WindowManager::initialize_system_control(Marshaler& m, windowing::HWND hwnd,
                                               std::wstring_view base_class,
                                               u64 create_struct, bool ansi) {
    auto* window = m_hwnd_mgr.find(hwnd);
    if (!window || !create_struct) return false;
    if (!window->native_control_class.empty())
        return window->native_control_class == base_class && m_bridge.has_control_view(hwnd);
    // A window OF the system class (CreateWindowEx "Static", "Button", ...)
    // got its native control at creation; its WM_NCCREATE through the class
    // procedure finds it initialized. Only a superclass, whose creation built
    // an uninitialized pixel view, is rebound below.
    if (to_lower_copy(window->class_name) == to_lower_copy(base_class) &&
        m_bridge.has_control_view(hwnd)) {
        return true;
    }
    const auto ordinal = builtin_control_ordinal(base_class);
    // The stock native child controls have real backing/state, as do
    // SysLink (control_syslink.cpp: markup, LM_*, NM_CLICK) and
    // msctls_progress32 (every PBM_*; handle_control_message). Other base
    // class creation paths must not be acknowledged without initialization.
    std::wstring lower_base(base_class);
    for (auto& ch : lower_base) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    const bool named_with_contract =
        lower_base == L"syslink" || lower_base == L"msctls_progress32";
    if (!ordinal || (*ordinal == 0 && !named_with_contract) ||
        !(window->style & windowing::WS::CHILD)) {
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!SystemControlProc: superclass base initialization for this control/presentation\n");
        });
        return false;
    }
    using Create32 = sdk_creation::CREATESTRUCTW<u32>;
    using Create64 = sdk_creation::CREATESTRUCTW<u64>;
    const usize name_offset = m.cpu_is_64bit() ? offsetof(Create64, lpszName) : offsetof(Create32, lpszName);
    GuestAddr name = 0;
    if (m.cpu_is_64bit()) {
        auto value = m.read_u64(create_struct + name_offset);
        if (!value.is_ok()) return false;
        name = value.value();
    } else {
        auto value = m.read_u32(create_struct + name_offset);
        if (!value.is_ok()) return false;
        name = value.value();
    }
    std::wstring text;
    if (name) {
        auto value = ansi ? m.read_ansi_string(name) : m.read_wide_string(name);
        if (!value.is_ok()) return false;
        if (ansi && !codepage::is_valid_code_page(codepage::ansi_code_page())) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!SystemControlProc: ANSI creation code page\n");
            });
            return false;
        }
        text = ansi ? ansi_text::to_wide(value.value()) : utf8_to_wstring(value.value());
    }
    // Before NCCREATE the custom class has only an uninitialized pixel view.
    // Replace that host backing; HWND identity, class bytes and guest proc stay.
    const auto data = *window;
    if (!m_bridge.rebind_child_control(hwnd, data, *ordinal, std::wstring(base_class), text)) return false;
    window = m_hwnd_mgr.find(hwnd);
    if (!window) return false;
    window->native_control_class = std::wstring(base_class);
    window->title = text;
    return true;
}

windowing::HWND WindowManager::create_window(
    u32 ex_style,
    std::wstring_view class_name,
    std::wstring_view window_name,
    u32 style,
    i32 x, i32 y,
    i32 width, i32 height,
    windowing::HWND parent,
    u64 menu,
    u64 hinstance,
    u64 param,
    GuestAddr cls_guest_ptr,
    GuestAddr name_guest_ptr,
    Marshaler* m_marshaler,
    bool ansi_api
) {
    namespace ue = user32_errors;
    constexpr u32 kWsExDlgModalFrame = 0x00000001u;
    constexpr u32 kWsExMdiChild      = 0x00000040u;
    constexpr u32 kWsExToolWindow    = 0x00000080u;
    constexpr u32 kWsExWindowEdge    = 0x00000100u;
    constexpr u32 kWsExStaticEdge    = 0x00020000u;
    constexpr u32 kWsExInternal800   = 0x00000800u;
    // Measured bit by bit (s1win_style_probe "-- exstyle bits"): these
    // extended-style bits make CreateWindowEx fail with
    // ERROR_INVALID_PARAMETER.
    constexpr u32 kWsExInvalidAtCreate = 0xF5808000u;

    // 1. Class lookup (Learn, "How the System Locates a Window Class"): the
    // local classes of hInstance, then global classes, then system classes.
    // A class atom argument reaches here as "#nnnnn"; an application atom
    // names the class registered under it.
    if (class_name.empty()) {
        // Measured (create_null_class): ERROR_INVALID_PARAMETER.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return windowing::NULL_HWND;
    }
    std::wstring lookup_name(class_name);
    {
        ATOM integer = 0;
        if (lookup_name.size() > 1 && lookup_name[0] == L'#' &&
            !WindowClassManager::parse_integer_atom(lookup_name, integer)) {
            u32 value = 0;
            bool digits = true;
            for (usize i = 1; i < lookup_name.size(); ++i) {
                const wchar_t c = lookup_name[i];
                if (c < L'0' || c > L'9') { digits = false; break; }
                value = value * 10 + static_cast<u32>(c - L'0');
                if (value > 0xFFFF) { digits = false; break; }
            }
            if (digits && value >= 0xC000) {
                const std::wstring registered =
                    m_class_mgr.atom_name(static_cast<ATOM>(value));
                if (!registered.empty()) lookup_name = registered;
            }
        }
    }
    const WindowClassData* cls = m_class_mgr.find_for_create(lookup_name, hinstance);
    std::optional<u16> builtin_ord = std::nullopt;
    if (cls) {
        class_name = cls->class_name;
        // The system control classes are presented by native controls; a
        // window of one keeps the native control path (no guest procedure).
        if (cls->system) builtin_ord = builtin_control_ordinal(cls->class_name);
    } else {
        // A system class the process unregistered (UnregisterClass(name,
        // NULL)) is gone: CreateWindowEx fails like for any unknown class
        // (measured, s1win_class_probe create_button_after).
        builtin_ord = find_system_class(class_name) == nullptr
            ? builtin_control_ordinal(class_name)
            : std::nullopt;
        if (!builtin_ord) {
            // Measured (create_unregistered_class, create_bad_atom,
            // create_local_class_other_instance): ERROR_CANNOT_FIND_WND_CLASS.
            set_last_error(ue::ERROR_CANNOT_FIND_WND_CLASS);
            return windowing::NULL_HWND;
        }
        // Controls Vitreo hosts natively without a system-class record
        // (RichEdit, msctls_netaddress) keep that path.
    }

    // 2. Style validation and parent/owner resolution (before CW_USEDEFAULT —
    // message-only keeps 0x0).
    if ((ex_style & kWsExInvalidAtCreate) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return windowing::NULL_HWND;
    }
    // Learn: HWND_MESSAGE creates a message-only window (no desktop owner,
    // no visible chrome). Callers PostMessage to it as a thread-queue wakeup.
    const bool message_only = (parent == windowing::HWND_MESSAGE);
    // CREATESTRUCT.hwndParent is the caller's hWndParent (owner or parent).
    const windowing::HWND requested_parent = parent;
    bool is_child = (style & windowing::WS::CHILD) != 0;
    auto window_exists = [this](windowing::HWND h) {
        return m_hwnd_mgr.find(h) != nullptr ||
               ipc::hwnd_registry_is_window(static_cast<u32>(h));
    };
    windowing::HWND owner = windowing::NULL_HWND;
    if (message_only) {
        // Message-only windows are neither children nor owned top-level windows.
        is_child = false;
        owner = windowing::NULL_HWND;
        // Keep parent == HWND_MESSAGE so GetParent / diagnostics see the contract.
    } else if (is_child) {
        if (parent == windowing::NULL_HWND) {
            // Measured (create_child_no_parent): ERROR_TLW_WITH_WSCHILD.
            set_last_error(ue::ERROR_TLW_WITH_WSCHILD);
            return windowing::NULL_HWND;
        }
        if (parent != windowing::HWND_DESKTOP && !window_exists(parent)) {
            set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
            return windowing::NULL_HWND;
        }
    } else {
        if (parent != windowing::NULL_HWND && parent != windowing::HWND_DESKTOP) {
            if (!window_exists(parent)) {
                // Measured (create_popup_invalid_owner).
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                return windowing::NULL_HWND;
            }
            // For top-level popups/overlapped windows, hwndParent is the
            // owner. An owner is always a top-level window: a child passed as
            // hwndParent makes its top-level ancestor the owner (Learn: Window
            // Features, "Owned Windows"; measured create_owner_is_child).
            owner = parent;
            for (u32 guard = 0; guard < 64; ++guard) {
                const windowing::WindowData* o = m_hwnd_mgr.find(owner);
                if (!o || (o->style & windowing::WS::CHILD) == 0 ||
                    o->parent == windowing::HWND_DESKTOP ||
                    o->parent == windowing::NULL_HWND) {
                    break;
                }
                owner = o->parent;
            }
        }
        parent = windowing::HWND_DESKTOP;
        // A top-level window's hMenu must be a menu (measured,
        // create_popup_bad_menu: ERROR_INVALID_MENU_HANDLE).
        if (menu != 0 && m_menu_validator && !m_menu_validator(menu)) {
            set_last_error(error_codes::ERROR_INVALID_MENU_HANDLE);
            return windowing::NULL_HWND;
        }
    }
    // Learn (Window Features, "Layered Windows"): "Beginning with Windows 8,
    // WS_EX_LAYERED can be used with child windows"; an application gets that
    // behaviour by declaring Windows 8 or later in its manifest. Otherwise a
    // layered child fails without setting an error (measured with an
    // unmanifested probe, s1win_style_probe "c 4xxxxxxx 08080020").
    // WS_POPUP | WS_CHILD counts as a pop-up here ("c c0000000 08080020"
    // succeeds).
    if (is_child && (style & windowing::WS::POPUP) == 0 &&
        (ex_style & 0x00080000u) != 0 && !m_child_layered_allowed) {
        return windowing::NULL_HWND;
    }
    if ((ex_style & kWsExMdiChild) != 0) {
        // WS_EX_MDICHILD needs an MDI client parent. Vitreo has no MDI client
        // window, so no parent qualifies: a top-level window fails with
        // ERROR_INVALID_WINDOW_HANDLE and a child fails without setting an
        // error (measured, s1win_style_probe bit 6 / "c 4xxxxxxx 00000040").
        if (!is_child) set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        return windowing::NULL_HWND;
    }

    // The styles USER stores (measured against every style/ex-style pair in
    // s1win_style_probe "-- create styles"): an overlapped window always gets
    // WS_CAPTION and WS_EX_WINDOWEDGE; every top-level window gets
    // WS_CLIPSIBLINGS; otherwise WS_EX_WINDOWEDGE is set exactly when the
    // window has WS_EX_DLGMODALFRAME, or a dialog/sizing frame without
    // WS_EX_STATICEDGE. The CREATESTRUCT keeps the caller's dwStyle; its
    // dwExStyle has the frame-derived WS_EX_WINDOWEDGE but not the one every
    // overlapped window gets (s1win_msgseq2_probe create_*: 0x08000000 for a
    // plain overlapped window, 0x08000100 for a sizing-frame pop-up).
    const u32 requested_style = style;
    u32 requested_ex_style = ex_style;
    {
        const bool frame_edge = (ex_style & kWsExDlgModalFrame) != 0 ||
            ((style & (windowing::WS::DLGFRAME | windowing::WS::THICKFRAME)) != 0 &&
             (ex_style & kWsExStaticEdge) == 0);
        requested_ex_style = frame_edge ? (ex_style | kWsExWindowEdge)
                                        : (ex_style & ~kWsExWindowEdge);
    }
    {
        const bool top_level = !is_child ||
            (style & windowing::WS::POPUP) != 0;
        const bool overlapped =
            (style & (windowing::WS::POPUP | windowing::WS::CHILD)) == 0;
        if (overlapped) style |= windowing::WS::CAPTION;
        if (top_level) style |= windowing::WS::CLIPSIBLINGS;
        if ((ex_style & kWsExInternal800) != 0) {
            // Measured: bit 0x800 is stored as WS_EX_TOOLWINDOW.
            ex_style = (ex_style & ~kWsExInternal800) | kWsExToolWindow;
        }
        const bool edge = overlapped || (ex_style & kWsExDlgModalFrame) != 0 ||
            ((style & (windowing::WS::DLGFRAME | windowing::WS::THICKFRAME)) != 0 &&
             (ex_style & kWsExStaticEdge) == 0);
        ex_style = edge ? (ex_style | kWsExWindowEdge) : (ex_style & ~kWsExWindowEdge);
    }

    // 3. Resolve CW_USEDEFAULT. Do not coerce width/height 0 → 640×480 for
    // message-only windows: CreateWindow(..., 0, 0, 0, 0, HWND_MESSAGE)
    // is a zero-sized message target on purpose.
    if (message_only) {
        if (x == CW_USEDEFAULT) { x = 0; }
        if (y == CW_USEDEFAULT) { y = 0; }
        if (width == CW_USEDEFAULT) { width = 0; }
        if (height == CW_USEDEFAULT) { height = 0; }
    } else {
        const bool overlapped =
            !is_child && (style & windowing::WS::POPUP) == 0;
        resolve_defaults(x, y, width, height, overlapped);
    }

    // 4. Build WindowData
    windowing::WindowData data{};
    data.style      = style;
    data.ex_style   = ex_style;
    data.title      = std::wstring(window_name);
    data.parent     = parent;
    data.owner      = owner;
    data.is_message_only = message_only;
    data.owner_thread_id = ThreadManager::get_current_thread_id_static();
    data.wndproc    = (cls && !builtin_ord) ? cls->wndproc : 0;
    data.class_atom = cls ? cls->atom : 0;
    data.class_id   = cls ? cls->id : 0;
    data.class_style = cls ? cls->style : 0;
    data.hinstance  = hinstance;
    // Extra window memory: cbWndExtra bytes, zero-initialised (Learn: About
    // Window Classes, "Extra Window Memory").
    data.wnd_extra_size = cls ? static_cast<u32>(std::max(0, cls->wnd_extra)) : 0;
    data.extra_bytes.assign(data.wnd_extra_size, 0);
    // IsWindowUnicode: the class charset; a system class takes the charset of
    // the CreateWindowEx call (measured, s1win_class_probe
    // button_isunicode / buttona_isunicode / ansi_class_createw_isunicode).
    data.unicode = (cls && !cls->system) ? cls->unicode : !ansi_api;
    // Vitreo's control code identifies the six USER controls by their
    // dialog-template class ordinal (0x0080 Button ... 0x0085 ComboBox), as
    // template-created controls carry it; GCW_ATOM still reports the class's
    // own atom through class_id.
    if (builtin_ord && *builtin_ord >= 0x0080) data.class_atom = *builtin_ord;
    data.class_name = cls && !cls->class_name.empty()
                          ? cls->class_name
                          : std::wstring(class_name);
    // Normalize common CreateWindow aliases (BUTTON/button) to the system
    // class name GetClassName returns.
    if (!cls) {
        auto upper = data.class_name;
        for (auto& ch : upper) {
            if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
        }
        if (upper == L"BUTTON") data.class_name = L"Button";
        else if (upper == L"EDIT") data.class_name = L"Edit";
        else if (upper == L"STATIC") data.class_name = L"Static";
        else if (upper == L"LISTBOX") data.class_name = L"ListBox";
        else if (upper == L"SCROLLBAR") data.class_name = L"ScrollBar";
        else if (upper == L"COMBOBOX") data.class_name = L"ComboBox";
    }
    // CreateWindowEx creates the window hidden and shows a WS_VISIBLE one
    // with ShowWindow after WM_CREATE: GetWindowLong(GWL_STYLE) during
    // WM_NCCREATE / WM_CREATE has no WS_VISIBLE, while CREATESTRUCT.style
    // keeps the requested bits (measured, s1win_msgseq2_probe
    // create_overlapped_plain_visible).
    const bool create_visible = (style & windowing::WS::VISIBLE) != 0;
    if (create_visible && m_ops.show) {
        data.style &= ~windowing::WS::VISIBLE;
    }
    data.visible    = (data.style & windowing::WS::VISIBLE) != 0;
    data.enabled    = (style & windowing::WS::DISABLED) == 0;
    // For child windows, hMenu is the control ID. It belongs in GWL_ID only —
    // GWL_USERDATA starts at 0 on Windows and changes only via SetWindowLong.
    // Seeding user_data with the control ID makes GetWindowLong(GWL_USERDATA)
    // return a small integer for every child, so a caller that stores a
    // pointer there and tests it for NULL sees a non-NULL value it never
    // wrote.
    if (is_child) {
        data.ctrl_id   = static_cast<u32>(menu);
    } else if (menu != 0 || (cls && !cls->menu_name.empty())) {
        // Top-level window with an explicit menu or a WNDCLASS lpszMenuName.
        // Microsoft Learn (CreateWindowExW): NULL hMenu uses the class menu.
        // Budget Win32 menu-bar chrome before WM_NCCREATE so GetClientRect /
        // WM_NCCALCSIZE match a window that already owns its menu bar.
        data.has_menu = true;
    }

    // Build rects.  For top-level windows, macOS handles the title bar,
    // borders and (when present) the menu bar natively.  Subtract the full
    // Win32 non-client overhead so the Cocoa content rect matches the
    // Win32 client area — otherwise the window is too large and shows
    // dead space at the bottom / right.
    i32 nc_w = 0, nc_h = 0;
    i32 cw = width;
    i32 ch = height;
    if (!message_only) {
        const auto chrome = windowing::make_presentation_recipe(
                                windowing::make_presentation_signals(
                                    style, ex_style, false, false,
                                    owner != windowing::NULL_HWND, false,
                                    false))
                                .chrome;
        if (chrome == windowing::PresentationChrome::Native) {
            compute_nc_insets(style, ex_style, data.has_menu, nc_w, nc_h);
        }
        cw = width  - nc_w;
        ch = height - nc_h;
        if (cw < 0) cw = 0;
        if (ch < 0) ch = 0;
    }
    // rect stored in WindowData is the Win32 outer rect (for GetWindowRect).
    data.rect        = windowing::Rect{x, y, x + width, y + height};
    // client_rect is the usable area (for GetClientRect / painting) and
    // also determines the Cocoa window content size.
    data.client_rect = windowing::Rect{0, 0, cw, ch};
    {
        const auto in = nc_w != 0 || nc_h != 0
                            ? nc_metrics::insets(style, ex_style, data.has_menu, false)
                            : nc_metrics::Insets{};
        data.client_offset = windowing::Point{in.left, in.top};
    }

    // Internal host/model controls can be created without a guest dispatcher.
    // A real guest procedure cannot silently accept creation without running.
    if (data.wndproc != 0 && !m_wndproc_callback) {
        static std::once_flag unavailable;
        std::call_once(unavailable, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED user32!CreateWindowEx: synchronous guest dispatcher unavailable\n");
        });
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return windowing::NULL_HWND;
    }

    // 5. Allocate HWND
    windowing::HWND hwnd = m_hwnd_mgr.create(data);
    // Measured (winevent_sequence_probe): every window, child or top-level,
    // raises EVENT_OBJECT_CREATE (OBJID_WINDOW, CHILDID_SELF) when created.
    constexpr u32 EVENT_OBJECT_CREATE = 0x8000u;
    raise_win_event(EVENT_OBJECT_CREATE, hwnd);

    // AppKit has no useful notion of a zero-extent surface, so the native
    // backing is given a minimal one.  The Win32 rects above keep the extent
    // the caller asked for, since GetClientRect on a zero-sized window reports
    // zero on Windows.
    windowing::WindowData native = data;
    if (native.client_rect.right < 1) native.client_rect.right = 1;
    if (native.client_rect.bottom < 1) native.client_rect.bottom = 1;

    // 6. Create native backing:
    // - Message-only windows have no NSWindow.
    // - Top-level windows get an NSWindow.
    // - Child windows (controls) get an NSView.
    if (message_only) {
        if (diag_logs_enabled()) {
            std::fprintf(stderr,
                         "[winshow] create_window hwnd=0x%x message_only=1 "
                         "tid=%u style=0x%x size=%dx%d\n",
                         hwnd, data.owner_thread_id, style, width, height);
            std::fflush(stderr);
        }
    } else if (!is_child) {
        // Create the native backing hidden. CreateWindowEx must finish
        // WM_NCCREATE and WM_CREATE before WS_VISIBLE causes WM_SHOWWINDOW and
        // activation. Passing data.visible through here lets AppKit make the
        // window key while native creation is still on the stack, reversing
        // that observable USER32 order and queuing activation too early.
        windowing::WindowData native_data = native;
        native_data.visible = false;
        if (const auto* cls = m_class_mgr.find_by_atom(data.class_atom);
            cls != nullptr && m_paint_mgr != nullptr) {
            native_data.erase_color = m_paint_mgr->brush_color(cls->hbr_background);
        }
        m_bridge.create_window(hwnd, native_data);
    } else {
        u16 ordinal = builtin_ord.value_or(0);
        std::wstring cname;
        if (ordinal == 0) {
            cname = std::wstring(class_name);
        }
        m_bridge.add_child_view(parent, hwnd, native, ordinal, cname,
                                data.title);

        // Apply a default body font to runtime-created controls. Without
        // this, controls use the macOS system font (13pt), which is too
        // large. 11pt matches the dialog-template path (8pt Win32 × 1.375).
        m_bridge.set_control_font(hwnd, 11.0f, 400, false);
    }

    // Capsule HWND table so peer host processes can IsWindow/GetDC.
    // Publish the class and title too: Win32 FindWindow is system-wide, and a
    // guest process waiting for a PEER process's window polls until it appears.
    // Without these the peer's window is invisible to the query and the wait
    // never ends. Correctness, not throughput -- FindWindow is called tens of
    // times over a whole application launch, not in any hot path.
    ipc::hwnd_registry_publish(static_cast<u32>(hwnd), cw, ch,
                               data.owner_thread_id,
                               wstring_to_utf8(data.class_name),
                               wstring_to_utf8(data.title));
    (void)ipc::hwnd_registry_set_parent(static_cast<u32>(hwnd),
                                        static_cast<u32>(data.parent));

    // 6b. Auto-load class menu before WM_NCCREATE / WM_CREATE.
    // Microsoft Learn (CreateWindowExW): NULL hMenu on an overlapped/pop-up
    // window selects the class menu. On Windows that menu is attached before
    // creation messages, so GetMenu during WM_CREATE returns a valid HMENU.
    if (!is_child && menu == 0 && cls && !cls->menu_name.empty()) {
        if (m_menu_load_callback) {
            m_menu_load_callback(hwnd, cls->menu_name);
        }
    }

    // SetWindowLongPtr(GWLP_WNDPROC) replaces the procedure for subsequent
    // messages, including messages sent reentrantly during CreateWindowEx.
    // Resolve it for every dispatch, never from the pre-callback data copy.
    // https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwindowlongptrw
    const auto current_wndproc = [&]() -> u64 {
        const auto* live = m_hwnd_mgr.find(hwnd);
        return live ? effective_wndproc(*live) : 0;
    };
    u64 create_wndproc = current_wndproc();

    // 6c. WM_GETMINMAXINFO comes first for an overlapped or sizable window,
    // before WM_NCCREATE, and its tracking size bounds the created window
    // (measured, s1win_msgseq_probe create_overlapped_hidden and
    // s1win_api_probe create_overlapped_tiny_mintrack: 10x10 becomes 136x39).
    const bool sends_minmax =
        !message_only &&
        ((style & (windowing::WS::POPUP | windowing::WS::CHILD)) == 0 ||
         (style & windowing::WS::THICKFRAME) != 0);
    if (sends_minmax && m_wndproc_callback && create_wndproc != 0 && m_marshaler &&
        m_createstruct_scratch != 0) {
        const GuestAddr mmi = m_createstruct_scratch + 0x200;
        const windowing::WindowData* desktop = m_hwnd_mgr.find(windowing::HWND_DESKTOP);
        const i32 screen_w = desktop ? desktop->rect.width() : 1920;
        const i32 screen_h = desktop ? desktop->rect.height() : 1080;
        i32 min_track_w = 0, min_track_h = 0;
        default_min_track_size(style, ex_style, min_track_w, min_track_h);
        const i32 defaults[10] = {0, 0, screen_w, screen_h, 0, 0,
                                  min_track_w, min_track_h,
                                  screen_w + 12, screen_h + 12};
        for (u32 i = 0; i < 10; ++i) {
            (void)m_marshaler->write_u32(mmi + i * 4, static_cast<u32>(defaults[i]));
        }
        (void)m_wndproc_callback(hwnd, static_cast<u32>(windowing::WM::GETMINMAXINFO), 0,
                                 mmi, create_wndproc);
        const i32 min_w = static_cast<i32>(m_marshaler->read_u32(mmi + 24).value_or(0));
        const i32 min_h = static_cast<i32>(m_marshaler->read_u32(mmi + 28).value_or(0));
        const i32 max_w = static_cast<i32>(m_marshaler->read_u32(mmi + 32).value_or(0));
        const i32 max_h = static_cast<i32>(m_marshaler->read_u32(mmi + 36).value_or(0));
        i32 w = data.rect.width();
        i32 h = data.rect.height();
        if (min_w > 0 && w < min_w) w = min_w;
        if (min_h > 0 && h < min_h) h = min_h;
        if (max_w > 0 && w > max_w) w = max_w;
        if (max_h > 0 && h > max_h) h = max_h;
        if (w != data.rect.width() || h != data.rect.height()) {
            data.rect.right = data.rect.left + w;
            data.rect.bottom = data.rect.top + h;
            data.client_rect = windowing::Rect{0, 0, std::max(0, w - nc_w), std::max(0, h - nc_h)};
            if (windowing::WindowData* live = m_hwnd_mgr.find(hwnd)) {
                live->rect = data.rect;
                live->normal_rect = data.rect;
                live->client_rect = data.client_rect;
            }
        }
    }

    // 7. Build CREATESTRUCT in guest scratch memory and send WM_NCCREATE.
    // The scratch is shared by every CreateWindowEx on the thread; a window
    // procedure that creates another window from WM_NCCREATE/WM_CREATE (the
    // usual way children are made) must get its own CREATESTRUCT back
    // afterwards, as a stack-allocated one would be on Windows.
    struct CreateScratchFrame {
        Marshaler* marshaler;
        GuestAddr address;
        std::array<u8, 0x240> bytes{};
        bool saved{false};
        CreateScratchFrame(Marshaler* m, GuestAddr a) : marshaler(m), address(a) {
            if (marshaler && address) {
                saved = marshaler->memory().read_bytes(address, bytes.data(), bytes.size()).is_ok();
            }
        }
        ~CreateScratchFrame() {
            if (saved) (void)marshaler->memory().write_bytes(address, bytes.data(), bytes.size());
        }
    } create_scratch_frame(m_marshaler, m_createstruct_scratch);
    GuestAddr cs_addr = 0;
    if (m_marshaler) {
        cs_addr = write_createstruct(m_marshaler, requested_ex_style, std::wstring(class_name),
                                      std::wstring(window_name), requested_style,
                                      x, y, width, height, requested_parent, menu,
                                      hinstance, param, cls_guest_ptr, name_guest_ptr);
    }

    create_wndproc = current_wndproc();
    if (m_wndproc_callback && create_wndproc != 0) {
        u64 result = m_wndproc_callback(hwnd, static_cast<u32>(windowing::WM::NCCREATE), 0, cs_addr, create_wndproc);
        if (result == 0) {
            // WM_NCCREATE returned FALSE - abort window creation
            m_bridge.destroy_window(hwnd);
            m_hwnd_mgr.destroy(hwnd);
            ipc::hwnd_registry_unpublish(static_cast<u32>(hwnd));
            return windowing::NULL_HWND;
        }
    }

    create_wndproc = current_wndproc();

    // Windows determines the initial client area after WM_NCCREATE and before
    // WM_CREATE with WM_NCCALCSIZE(wParam = FALSE): lParam is one RECT, the
    // proposed window rectangle in SCREEN coordinates, which the procedure
    // turns into the client rectangle (measured, s1win_msgseq_probe /
    // s1win_msgseq2_probe create_*: "WM_NCCALCSIZE w=0", a child's rectangle
    // offset by its parent's client origin). The guest may call DefWindowProc
    // for the standard frame or leave it unchanged to make the whole window
    // client area.
    if (!message_only && m_marshaler && m_wndproc_callback &&
        create_wndproc != 0 && m_createstruct_scratch != 0) {
        const GuestAddr params = m_createstruct_scratch + 0x100;
        windowing::Point screen{0, 0};
        if (is_child) {
            windowing::HWND cur = parent;
            for (u32 guard = 0; guard < 256; ++guard) {
                const windowing::WindowData* p = m_hwnd_mgr.find(cur);
                if (!p) break;
                screen.x += p->rect.left + p->client_offset.x;
                screen.y += p->rect.top + p->client_offset.y;
                if (p->parent == windowing::HWND_DESKTOP || p->parent == windowing::NULL_HWND) break;
                cur = p->parent;
            }
        }
        const windowing::Rect proposed{
            screen.x + data.rect.left, screen.y + data.rect.top,
            screen.x + data.rect.right, screen.y + data.rect.bottom};
        (void)m_marshaler->write_u32(params + 0, static_cast<u32>(proposed.left));
        (void)m_marshaler->write_u32(params + 4, static_cast<u32>(proposed.top));
        (void)m_marshaler->write_u32(params + 8, static_cast<u32>(proposed.right));
        (void)m_marshaler->write_u32(params + 12, static_cast<u32>(proposed.bottom));

        (void)m_wndproc_callback(
            hwnd, static_cast<u32>(windowing::WM::NCCALCSIZE),
            0, params, create_wndproc);
        const auto left = m_marshaler->read_u32(params + 0);
        const auto top = m_marshaler->read_u32(params + 4);
        const auto right = m_marshaler->read_u32(params + 8);
        const auto bottom = m_marshaler->read_u32(params + 12);
        if (left.is_ok() && top.is_ok() && right.is_ok() && bottom.is_ok()) {
            const windowing::Rect client{
                static_cast<i32>(left.value()),
                static_cast<i32>(top.value()),
                static_cast<i32>(right.value()),
                static_cast<i32>(bottom.value())};
            if (client.right >= client.left && client.bottom >= client.top) {
                if (windowing::WindowData* live = m_hwnd_mgr.find(hwnd)) {
                    live->client_offset = {
                        client.left - proposed.left,
                        client.top - proposed.top};
                    // Learn: an explicit 0×0 CreateWindow stays 0×0 after
                    // WM_NCCALCSIZE. Coercing to 1×1 lied about GetClientRect
                    // while Cocoa keeps a separate min native backing.
                    live->client_rect = {
                        0, 0, client.width(), client.height()};
                    live->client_drawn_frame = windowing::nccalcsize_guest_owns_frame(
                        proposed, client, live->style);
                    if (!is_child) {
                        m_bridge.set_client_drawn_frame(
                            hwnd, live->client_drawn_frame);
                        m_bridge.set_frame(hwnd, windowing::Rect{
                            data.rect.left, data.rect.top,
                            data.rect.left + live->client_rect.width(),
                            data.rect.top + live->client_rect.height()});
                    }
                    ipc::hwnd_registry_publish(
                        static_cast<u32>(hwnd), live->client_rect.width(),
                        live->client_rect.height(), live->owner_thread_id,
                        wstring_to_utf8(live->class_name),
                        wstring_to_utf8(live->title));
                }
            }
        }
    }

    // 8. Send WM_CREATE synchronously (per Win32 spec). A window created from
    // WM_NCCREATE or WM_NCCALCSIZE reused the scratch; rewrite ours.
    if (m_marshaler) {
        cs_addr = write_createstruct(m_marshaler, requested_ex_style, std::wstring(class_name),
                                     std::wstring(window_name), requested_style,
                                     x, y, width, height, requested_parent, menu,
                                     hinstance, param, cls_guest_ptr, name_guest_ptr);
    }
    create_wndproc = current_wndproc();
    if (m_wndproc_callback && create_wndproc != 0) {
        u64 result = m_wndproc_callback(hwnd, static_cast<u32>(windowing::WM::CREATE), 0, cs_addr, create_wndproc);
        const bool rejected = m_marshaler && !m_marshaler->cpu_is_64bit()
            ? static_cast<i32>(result) == -1 : static_cast<i64>(result) == -1;
        if (rejected) {
            // WM_CREATE returned -1 - abort window creation. Measured
            // (s1win_api_probe create_create_minus1): the last error is
            // ERROR_SUCCESS afterwards.
            m_bridge.destroy_window(hwnd);
            m_hwnd_mgr.destroy(hwnd);
            ipc::hwnd_registry_unpublish(static_cast<u32>(hwnd));
            set_last_error(0);
            return windowing::NULL_HWND;
        }
    }

    // 9. After WM_CREATE (measured, s1win_msgseq2_probe create_* and
    //    s1win_msgseq_probe create_child_visible):
    //    a. a pop-up or child window gets WM_SIZE and WM_MOVE at once; an
    //       overlapped window's wait for its first ShowWindow;
    //    b. WS_MINIMIZE / WS_MAXIMIZE are applied through SetWindowPos;
    //    c. WM_PARENTNOTIFY(WM_CREATE) goes up the parent chain;
    //    d. WS_VISIBLE becomes ShowWindow(SW_SHOW).
    if (m_ops.show) {
        auto live_proc = [&]() -> u64 {
            const windowing::WindowData* live = m_hwnd_mgr.find(hwnd);
            return live ? effective_wndproc(*live) : 0;
        };
        auto send = [&](u32 msg, u64 wParam, u64 lParam) {
            const u64 proc = live_proc();
            if (m_wndproc_callback && proc != 0) {
                (void)m_wndproc_callback(hwnd, msg, wParam, lParam, proc);
            }
        };
        if (!message_only) {
            const bool overlapped_window =
                !is_child && (style & windowing::WS::POPUP) == 0;
            if (overlapped_window) {
                if (windowing::WindowData* live = m_hwnd_mgr.find(hwnd)) {
                    live->pending_size_move = true;
                }
            } else if (const windowing::WindowData* live = m_hwnd_mgr.find(hwnd)) {
                const windowing::Rect client = live->client_rect;
                const i32 mx = live->rect.left + live->client_offset.x;
                const i32 my = live->rect.top + live->client_offset.y;
                send(static_cast<u32>(windowing::WM::SIZE), 0,
                     static_cast<u64>((client.width() & 0xFFFF) |
                                      ((client.height() & 0xFFFF) << 16)));
                if (!m_hwnd_mgr.find(hwnd)) return windowing::NULL_HWND;
                send(static_cast<u32>(windowing::WM::MOVE), 0,
                     static_cast<u64>((mx & 0xFFFF) | ((my & 0xFFFF) << 16)));
                if (!m_hwnd_mgr.find(hwnd)) return windowing::NULL_HWND;
            }
            if (m_ops.initial_state &&
                (style & (windowing::WS::MINIMIZE | windowing::WS::MAXIMIZE)) != 0) {
                m_ops.initial_state(hwnd, (style & windowing::WS::MINIMIZE) != 0);
                if (!m_hwnd_mgr.find(hwnd)) return windowing::NULL_HWND;
            }
        }
        send_parent_notify(hwnd, static_cast<u32>(windowing::WM::CREATE));
        if (!m_hwnd_mgr.find(hwnd)) return windowing::NULL_HWND;
        if (create_visible) {
            (void)m_ops.show(hwnd, windowing::SW::SHOW);
            if (!m_hwnd_mgr.find(hwnd)) return windowing::NULL_HWND;
        }
    } else if (data.visible) {
        // Without the window-position engine (unit tests of this class
        // alone): the WS_VISIBLE notifications directly.
        const u64 show_wndproc = current_wndproc();
        if (m_wndproc_callback && show_wndproc != 0) {
            (void)m_wndproc_callback(
                hwnd, static_cast<u32>(windowing::WM::SHOWWINDOW),
                1, 0, show_wndproc);
        }

        {
            constexpr u32 SWP_NOZORDER = 0x0004;
            constexpr u32 SWP_NOACTIVATE = 0x0010;
            constexpr u32 SWP_SHOWWINDOW = 0x0040;

            windowing::Rect placed = data.rect;
            windowing::Point client_origin = data.client_offset;
            windowing::Rect client = data.client_rect;
            if (const windowing::WindowData* live = m_hwnd_mgr.find(hwnd)) {
                placed = live->rect;
                client_origin = live->client_offset;
                client = live->client_rect;
            }
            const u32 swp_flags = SWP_SHOWWINDOW | SWP_NOZORDER |
                                  (is_child ? SWP_NOACTIVATE : 0u);

            const u64 changed_wndproc = current_wndproc();
            auto notify = [&](u32 msg, u64 wParam, u64 lParam) {
                if (m_wndproc_callback && changed_wndproc != 0) {
                    (void)m_wndproc_callback(hwnd, msg, wParam, lParam,
                                             changed_wndproc);
                    return;
                }
            };
            if (m_marshaler && m_createstruct_scratch != 0) {
                const GuestAddr wp = m_createstruct_scratch + 0x1C0;
                WindowPosLayout{m_marshaler->cpu_is_64bit()}.write(
                    m_marshaler->memory(), wp, hwnd, 0, placed.left, placed.top,
                    placed.width(), placed.height(), swp_flags);
                notify(static_cast<u32>(windowing::WM::WINDOWPOSCHANGED), 0, wp);
            }
            if (!m_wndproc_callback || changed_wndproc == 0) {
                notify(static_cast<u32>(windowing::WM::SIZE), 0,
                       static_cast<u64>((client.width() & 0xFFFF) |
                                        ((client.height() & 0xFFFF) << 16)));
                notify(static_cast<u32>(windowing::WM::MOVE), 0,
                       static_cast<u64>(((placed.left + client_origin.x) & 0xFFFF) |
                                        (((placed.top + client_origin.y) & 0xFFFF)
                                         << 16)));
            }
        }
        if (m_paint_mgr) {
            m_paint_mgr->invalidate_rect(
                static_cast<u64>(hwnd), nullptr, true);
        }
        if (!is_child) {
            if (m_activation_callback) {
                m_activation_callback(hwnd);
            }
            m_bridge.set_visible(hwnd, true);
        }
    }

    if (!is_child && m_lifecycle_callback) {
        m_lifecycle_callback(true, hwnd);
    }

    return hwnd;
}


// -----------------------------------------------------------------------------
// resolve_defaults
// -----------------------------------------------------------------------------

void WindowManager::resolve_defaults(i32& x, i32& y, i32& w, i32& h,
                                     bool overlapped) const {
    if (overlapped) {
        // Microsoft Learn (CreateWindowExW): a CW_USEDEFAULT x selects the
        // default upper-left corner and ignores y; a CW_USEDEFAULT nWidth
        // selects a default width and height and ignores nHeight. Honouring
        // an explicit extent next to a defaulted one produces a degenerate
        // window Windows never creates — (CW_USEDEFAULT, 0, CW_USEDEFAULT, 0)
        // would become a 640×0 sliver.
        if (x == CW_USEDEFAULT) { x = 100; y = 100; }
        else if (y == CW_USEDEFAULT) { y = 100; }
        if (w == CW_USEDEFAULT) { w = 640; h = 480; }
        else if (h == CW_USEDEFAULT) { h = 480; }
    } else {
        // Microsoft Learn (CreateWindowExW): CW_USEDEFAULT is valid only for
        // overlapped windows; for a pop-up or child window the position and
        // size parameters are set to zero instead.
        if (x == CW_USEDEFAULT) { x = 0; }
        if (y == CW_USEDEFAULT) { y = 0; }
        if (w == CW_USEDEFAULT) { w = 0; }
        if (h == CW_USEDEFAULT) { h = 0; }
    }
    // An explicit zero extent is a real zero-sized window, not a request for a
    // default. The owner sizes it later. Only a negative extent is meaningless.
    if (w < 0) { w = 0; }
    if (h < 0) { h = 0; }
}


// =============================================================================
// Thunk Implementations
// =============================================================================

void impl_CreateWindowExW(Marshaler& m, WindowManager& wm) {
    // CreateWindowExW has 12 args (PE32 stdcall, 48 bytes on stack):
    //   dwExStyle(0), lpClassName(1), lpWindowName(2), dwStyle(3),
    //   X(4), Y(5), nWidth(6), nHeight(7),
    //   hWndParent(8), hMenu(9), hInstance(10), lpParam(11)
    auto ex_style     = static_cast<u32>(m.arg(0));
    GuestAddr cls_ptr = m.arg_ptr(1);
    GuestAddr name_ptr = m.arg_ptr(2);
    auto style        = static_cast<u32>(m.arg(3));
    auto x            = static_cast<i32>(m.arg(4));
    auto y            = static_cast<i32>(m.arg(5));
    auto w            = static_cast<i32>(m.arg(6));
    auto h            = static_cast<i32>(m.arg(7));
    auto parent       = static_cast<windowing::HWND>(m.arg(8));
    auto menu         = m.arg(9);
    auto hinstance    = m.arg(10);
    auto param        = m.arg(11);

    // Read class name (can be string or ATOM)
    std::wstring class_name;
    if (cls_ptr != 0 && cls_ptr < 0xFFFF) {
        // It's an ATOM, not a string pointer. Common built-in control atoms
        // match the dialog template ordinals.
        u16 atom = static_cast<u16>(cls_ptr & 0xFFFF);
        switch (atom) {
            // MS Learn / GetClassName system class strings (mixed case).
            case 0x0080: class_name = L"Button"; break;
            case 0x0081: class_name = L"Edit"; break;
            case 0x0082: class_name = L"Static"; break;
            case 0x0083: class_name = L"ListBox"; break;
            case 0x0084: class_name = L"ScrollBar"; break;
            case 0x0085: class_name = L"ComboBox"; break;
            default:
                class_name = L"#" + std::to_wstring(atom);
                break;
        }
    } else if (cls_ptr != 0) {
        auto r = m.read_wide_string(cls_ptr);
        if (r.is_ok()) {
            class_name = utf8_to_wstring(r.value());
        }
    }

    // Read window name
    std::wstring window_name;
    if (name_ptr != 0) {
        auto r = m.read_wide_string(name_ptr);
        if (r.is_ok()) {
            window_name = utf8_to_wstring(r.value());
        }
    }

    // Pass marshaler into create_window so CREATESTRUCT can be written
    windowing::HWND hwnd = wm.create_window(
        ex_style, class_name, window_name, style,
        x, y, w, h, parent, menu, hinstance, param,
        cls_ptr, name_ptr, &m
    );

    // Use narrow strings for fprintf (avoids %ls locale issues on macOS)
    std::string cls_u8(class_name.begin(), class_name.end());
    std::string ttl_u8(window_name.begin(), window_name.end());
    // ex_style matters as much as style: WS_EX_LAYERED (0x00080000) decides
    // whether the native window must be non-opaque so per-pixel alpha from
    // UpdateLayeredWindow is honoured instead of compositing onto white.
    VITREO_TRACE( "[hwnd-trace] CreateWindowExW -> hwnd=0x%x class='%s' title='%s' "
                 "style=0x%x ex_style=0x%x parent=0x%x menu=%llu x=%d y=%d w=%d h=%d\n",
                 hwnd, cls_u8.c_str(), ttl_u8.c_str(),
                 style, ex_style, parent,
                 static_cast<unsigned long long>(menu), x, y, w, h);
    if (std::getenv("VITREO_WIN_DIAG")) {
        std::fprintf(stderr,
            "[hwnd] CreateWindowExW hwnd=0x%x class='%s' title='%s' "
            "style=0x%x ex=0x%x parent=0x%x x=%d y=%d w=%d h=%d\n",
            hwnd, cls_u8.c_str(), ttl_u8.c_str(), style, ex_style, parent,
            x, y, w, h);
    }

    // Diagnostic: log child control creation for page-blank debugging
    if ((style & windowing::WS::CHILD) && parent > 0x10000 && diag_logs_enabled()) {
        std::fprintf(stderr,
            "[ctrl-diag] CreateWindowExW: hwnd=0x%x class='%s' parent=0x%x "
            "x=%d y=%d w=%d h=%d\n",
            hwnd, cls_u8.c_str(), parent, x, y, w, h);
    }

    note_first_top_level_window(hwnd, style);
    m.set_return(static_cast<u64>(hwnd));
}

void impl_DestroyWindow(Marshaler& m, WindowManager& wm) {
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (hwnd == windowing::NULL_HWND) {
        VITREO_TRACE( "[hwnd-trace] DestroyWindow hwnd=NULL -> FALSE\n");
        m.set_return(0u);
        return;
    }
    VITREO_TRACE( "[hwnd-trace] DestroyWindow hwnd=0x%x\n", hwnd);
    if (std::getenv("VITREO_WIN_DIAG"))
        fprintf(stderr, "[destroy] DestroyWindow hwnd=0x%x\n", hwnd);
    bool result = wm.destroy_window(hwnd);
    m.set_return(result ? 1u : 0u);
}

// =============================================================================
// ANSI Wrapper
// =============================================================================

void impl_CreateWindowExA(Marshaler& m, WindowManager& wm) {
    // Same layout as CreateWindowExW but string pointers are ANSI
    auto ex_style     = static_cast<u32>(m.arg(0));
    GuestAddr cls_ptr = m.arg_ptr(1);
    GuestAddr name_ptr = m.arg_ptr(2);
    auto style        = static_cast<u32>(m.arg(3));
    auto x            = static_cast<i32>(m.arg(4));
    auto y            = static_cast<i32>(m.arg(5));
    auto w            = static_cast<i32>(m.arg(6));
    auto h            = static_cast<i32>(m.arg(7));
    auto parent       = static_cast<windowing::HWND>(m.arg(8));
    auto menu         = m.arg(9);
    auto hinstance    = m.arg(10);
    auto param        = m.arg(11);

    // Read class name (can be string or ATOM)
    std::wstring class_name;
    if (cls_ptr != 0 && cls_ptr < 0xFFFF) {
        u16 atom = static_cast<u16>(cls_ptr & 0xFFFF);
        switch (atom) {
            // MS Learn / GetClassName system class strings (mixed case).
            case 0x0080: class_name = L"Button"; break;
            case 0x0081: class_name = L"Edit"; break;
            case 0x0082: class_name = L"Static"; break;
            case 0x0083: class_name = L"ListBox"; break;
            case 0x0084: class_name = L"ScrollBar"; break;
            case 0x0085: class_name = L"ComboBox"; break;
            default:
                class_name = L"#" + std::to_wstring(atom);
                break;
        }
    } else if (cls_ptr != 0) {
        auto r = m.read_ansi_string(cls_ptr);
        if (r.is_ok()) {
            class_name = utf8_to_wstring(r.value());
        }
    }

    // Read window name (ANSI)
    std::wstring window_name;
    if (name_ptr != 0) {
        auto r = m.read_ansi_string(name_ptr);
        if (r.is_ok()) {
            window_name = utf8_to_wstring(r.value());
        }
    }

    windowing::HWND hwnd = wm.create_window(
        ex_style, class_name, window_name, style,
        x, y, w, h, parent, menu, hinstance, param,
        cls_ptr, name_ptr, &m, /*ansi_api=*/true
    );

    VITREO_TRACE( "[hwnd-trace] CreateWindowExA -> hwnd=0x%x class='%ls' title='%ls' "
                 "style=0x%x parent=0x%x menu=%llu\n",
                 hwnd, class_name.c_str(), window_name.c_str(),
                 style, parent, static_cast<unsigned long long>(menu));

    if (std::getenv("VITREO_WIN_DIAG")) {
        std::string cls(class_name.begin(), class_name.end());
        std::string ttl(window_name.begin(), window_name.end());
        std::fprintf(stderr, "[ctl] CreateWindowExA hwnd=0x%x class='%s' title='%s' "
                     "pos=(%d,%d %dx%d) style=0x%x parent=0x%x id=%llu\n",
                     hwnd, cls.c_str(), ttl.c_str(), x, y, w, h, style, parent,
                     static_cast<unsigned long long>(menu));
    }

    note_first_top_level_window(hwnd, style);
    m.set_return(static_cast<u64>(hwnd));
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_window_exports(WindowManager& wm) {
    DllExports exports;
    exports.entries.push_back(ExportEntry{
        "CreateWindowExW", 1,
        [&wm](Marshaler& m) { impl_CreateWindowExW(m, wm); },
        48
    });
    exports.entries.push_back(ExportEntry{
        "CreateWindowExA", 0,
        [&wm](Marshaler& m) { impl_CreateWindowExA(m, wm); },
        48
    });
    exports.entries.push_back(ExportEntry{
        "DestroyWindow", 2,
        [&wm](Marshaler& m) { impl_DestroyWindow(m, wm); },
        4
    });
    return exports;
}

} // namespace vitreo::winapi

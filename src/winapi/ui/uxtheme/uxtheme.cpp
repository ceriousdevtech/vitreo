/// @file uxtheme.cpp
/// @brief uxtheme.dll: theme handles, window themes, system metrics and
///        the export table.
///
/// Vitreo's visual style keeps Windows 11 Aero's metrics (theme_data.hpp,
/// measured by tools/oracle_probe/uxtheme_aero_probe.c) and draws macOS
/// artwork (theme_render.cpp). Ordinals and stdcall sizes come from the
/// Windows 11 uxtheme.dll export table and the SDK's uxtheme.lib; every
/// result below is the one recorded in
/// tests/oracle_data/uxtheme_aero_probe{32,64}.expected.

#include "winapi/ui/uxtheme/uxtheme_internal.hpp"

#include "windowing/contracts/theme_host.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/user32/painting/sys_colors.hpp"

#include <array>
#include <cstdio>
#include <set>

namespace vitreo::winapi {

namespace uxtheme {

// --- helpers --------------------------------------------------------------

std::optional<std::u16string> read_wide(Marshaler& m, GuestAddr addr, usize max) {
    if (addr == 0) return std::nullopt;
    std::u16string s;
    for (usize i = 0; i < max; ++i) {
        const auto c = m.read_u16(addr + i * 2);
        if (c.is_err()) return std::nullopt;
        if (c.value() == 0) return s;
        s.push_back(static_cast<char16_t>(c.value()));
    }
    return s;
}

bool write_wide(Marshaler& m, GuestAddr addr, u32 cch, std::u16string_view s) {
    if (addr == 0 || cch == 0) return false;
    const usize n = std::min<usize>(s.size(), cch - 1);
    for (usize i = 0; i < n; ++i) (void)m.write_u16(addr + i * 2, s[i]);
    (void)m.write_u16(addr + n * 2, 0);
    return n == s.size();
}

bool read_rect(Marshaler& m, GuestAddr addr, i32 r[4]) {
    if (addr == 0) return false;
    for (int i = 0; i < 4; ++i) {
        const auto v = m.read_u32(addr + static_cast<GuestAddr>(i) * 4);
        if (v.is_err()) return false;
        r[i] = static_cast<i32>(v.value());
    }
    return true;
}

bool write_rect(Marshaler& m, GuestAddr addr, const i32 r[4]) {
    if (addr == 0) return false;
    for (int i = 0; i < 4; ++i) (void)m.write_u32(addr + static_cast<GuestAddr>(i) * 4, static_cast<u32>(r[i]));
    return true;
}

void unimplemented_once(const char* what) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard lock(mu);
    if (seen.insert(what).second) std::fprintf(stderr, "[vitreo] UNIMPLEMENTED uxtheme!%s\n", what);
}

// --- module -----------------------------------------------------------------

std::optional<ThemeObject> UxThemeModule::theme(u64 handle) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_open.find(handle);
    if (it == m_open.end() || it->second.refs == 0) return std::nullopt;
    return it->second;
}

bool UxThemeModule::window_valid(u32 hwnd) const {
    return hwnd != 0 && m_services.hwnd_mgr != nullptr && m_services.hwnd_mgr->is_valid(hwnd);
}

WindowThemeState& UxThemeModule::window_state(u32 hwnd) { return m_windows[hwnd]; }

windowing::ThemePalette UxThemeModule::palette(bool dark) const {
    const auto appearance = dark ? windowing::ThemeAppearance::Dark : windowing::ThemeAppearance::Light;
    windowing::ThemePalette p;
    if (m_services.host != nullptr && m_services.host->theme_palette(appearance, p)) return p;
    return default_palette(appearance);
}

namespace {

/// The classes Windows' Aero theme defines that this renderer does not draw
/// (vssym32.h): asking for one gets "no theme data", logged once.
bool known_undrawn_class(std::u16string_view cls) {
    static constexpr std::u16string_view kOthers[] = {
        u"AEROWIZARD", u"BUTTONSTYLE", u"CLOCK", u"COMBOBOXSTYLE", u"COMMUNICATIONS", u"CONTROLPANEL",
        u"DATEPICKER", u"DRAGDROP", u"EDITSTYLE", u"EMPTYMARKUP", u"EXPLORERBAR", u"FLYOUT", u"GLOBALS",
        u"LINK", u"LISTBOX", u"LISTVIEWSTYLE", u"MENUBAND", u"MENUSTYLE", u"MONTHCAL", u"NAVIGATION", u"PAGE",
        u"SEARCHEDITBOX", u"STARTPANEL", u"STATIC", u"TASKBAND", u"TASKBAR", u"TASKDIALOG", u"TASKDIALOGSTYLE",
        u"TEXTSELECTIONGRIPPER", u"TEXTSTYLE", u"TOOLBARSTYLE", u"TRAYNOTIFY", u"TREEVIEWSTYLE", u"USERTILE",
        u"ANIMATIONS"};
    for (auto k : kOthers) {
        if (k.size() != cls.size()) continue;
        bool eq = true;
        for (usize i = 0; i < k.size() && eq; ++i) {
            char16_t c = cls[i];
            if (c >= u'a' && c <= u'z') c = static_cast<char16_t>(c - 32);
            eq = c == k[i];
        }
        if (eq) return true;
    }
    return false;
}

std::string narrow(std::u16string_view s) {
    std::string out;
    for (char16_t c : s) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return out;
}

}  // namespace

u64 UxThemeModule::open(u32 hwnd, std::optional<std::u16string_view> class_list, u32& last_error, u32 dpi) {
    constexpr u32 STAP_ALLOW_CONTROLS = 2;
    if (!class_list) {
        last_error = E_POINTER;
        return 0;
    }
    std::u16string list(*class_list);
    std::optional<std::u16string> window_subapp;
    const bool use_window = window_valid(hwnd);
    if (use_window) {
        std::lock_guard lock(m_mutex);
        const auto it = m_windows.find(hwnd);
        if (it != m_windows.end()) {
            // SetWindowTheme: an empty id list (or an empty sub-app with an
            // empty list) turns theming off for the window (measured
            // setwindowtheme empty_empty / explorer_empty: NULL, 0x578).
            if (it->second.id_list) {
                if (it->second.id_list->empty()) {
                    it->second.theme = 0;
                    last_error = ERROR_INVALID_WINDOW_HANDLE;
                    return 0;
                }
                list = *it->second.id_list;
            }
            if (it->second.subapp && !it->second.subapp->empty()) window_subapp = it->second.subapp;
        }
    }
    if (list.empty()) {
        last_error = E_INVALIDARG;
        return 0;
    }
    i32 found = -1;
    bool dark = false;
    if ((app_properties & STAP_ALLOW_CONTROLS) != 0) {
        usize start = 0;
        while (start <= list.size() && found < 0) {
            const usize end = std::min(list.find(u';', start), list.size());
            const std::u16string_view token(list.data() + start, end - start);
            start = end + 1;
            if (token.empty()) continue;
            std::u16string_view subapp, cls = token;
            if (const usize sep = token.find(u"::"); sep != std::u16string_view::npos) {
                subapp = token.substr(0, sep);
                cls = token.substr(sep + 2);
            } else if (window_subapp) {
                subapp = *window_subapp;
            }
            const Variant v = find_variant(subapp, cls);
            if (v.valid()) {
                found = v.index;
                const auto sub = v.record().subapp;
                dark = sub.size() >= 9 && sub.substr(0, 9) == u"DarkMode_";
            } else if (known_undrawn_class(cls)) {
                unimplemented_once(("OpenThemeData: visual-style class " + narrow(cls) + " is not drawn").c_str());
            }
        }
    }
    std::lock_guard lock(m_mutex);
    if (found < 0) {
        if (use_window) m_windows[hwnd].theme = 0;
        last_error = E_PROP_ID_UNSUPPORTED;
        return 0;
    }
    if (dpi == 0) dpi = 96;
    u64& h = m_handles[{found, dpi}];
    if (h == 0) {
        h = m_next_handle;
        m_next_handle += 0x10u;
    }
    auto& obj = m_open[h];
    obj.variant = Variant{found};
    obj.dark = dark;
    obj.dpi = dpi;
    obj.refs += 1;
    if (use_window) m_windows[hwnd].theme = h;
    last_error = 0;
    return h;
}

u32 UxThemeModule::close(u64 handle) {
    std::lock_guard lock(m_mutex);
    const auto it = m_open.find(handle);
    if (it == m_open.end() || it->second.refs == 0) return E_HANDLE;
    it->second.refs -= 1;
    return S_OK;
}

}  // namespace uxtheme

using namespace uxtheme;

namespace {

/// OpenThemeData / OpenThemeDataEx / OpenThemeDataForDpi share one body.
void open_theme(Marshaler& m, UxThemeModule& mod, u32 hwnd, GuestAddr list_ptr, u32 dpi = 96) {
    std::optional<std::u16string> list = read_wide(m, list_ptr);
    u32 err = 0;
    const u64 h = mod.open(hwnd, list ? std::optional<std::u16string_view>(*list) : std::nullopt, err, dpi);
    set_last_error(err);
    m.set_return(h);
}

/// GetThemeSysSize(hTheme, iSizeId) with a theme: the theme's own sizes
/// (theme [SysMetrics]), measured "syssize button".
std::optional<i32> theme_sys_size(i32 id) {
    switch (id) {
    case 2: case 3: return 17;                      // SM_CXVSCROLL, SM_CYHSCROLL
    case 30: case 31: case 52: case 53: return 22;  // SM_CX/CYSIZE, SM_CX/CYSMSIZE
    case 54: case 55: return 19;                    // SM_CX/CYMENUSIZE
    case 32: return 1;                              // SM_CXFRAME (TMT_SIZINGBORDERWIDTH)
    case 92: return 4;                              // SM_CXPADDEDBORDER
    default: return std::nullopt;
    }
}

}  // namespace

DllExports create_uxtheme_thunks(windowing::HwndManager* hwnd_mgr) {
    UxThemeServices services;
    services.hwnd_mgr = hwnd_mgr;
    return create_uxtheme_thunks(services);
}

DllExports create_uxtheme_thunks(const UxThemeServices& services) {
    auto* mod = new UxThemeModule(services);  // lives as long as the process' export table
    DllExports exports;
    exports.entries.reserve(96);
    auto add = [&](const char* name, u16 ordinal, u16 stdcall, std::function<void(Marshaler&)> fn) {
        exports.entries.push_back(ExportEntry{name, ordinal, std::move(fn), stdcall});
    };

    // HTHEME OpenThemeData(HWND, LPCWSTR)
    add("OpenThemeData", 193, 8, [mod](Marshaler& m) {
        open_theme(m, *mod, static_cast<u32>(m.arg(0)), m.arg_ptr(1));
    });
    // HTHEME OpenThemeDataEx(HWND, LPCWSTR, DWORD dwFlags): OTD_FORCE_RECT_SIZING
    // and OTD_NONCLIENT open the same theme (measured openex flags 0..4).
    add("OpenThemeDataEx", 61, 12, [mod](Marshaler& m) {
        (void)m.arg(2);
        open_theme(m, *mod, static_cast<u32>(m.arg(0)), m.arg_ptr(1));
    });
    // HTHEME OpenThemeDataForDpi(HWND, LPCWSTR, UINT dpi). Aero reports the
    // same metrics at every DPI it accepts (measured 0..192) and refuses a
    // DPI beyond the largest scale factor (1000: NULL, ERROR_INVALID_PARAMETER).
    add("OpenThemeDataForDpi", 194, 12, [mod](Marshaler& m) {
        const u32 dpi = static_cast<u32>(m.arg(2));
        if (dpi > 480) {
            set_last_error(ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        open_theme(m, *mod, static_cast<u32>(m.arg(0)), m.arg_ptr(1), dpi);
    });
    // HRESULT CloseThemeData(HTHEME): the last error is untouched.
    add("CloseThemeData", 51, 4, [mod](Marshaler& m) { m.set_return(mod->close(m.arg(0))); });

    // HTHEME GetWindowTheme(HWND): the theme last opened for the window.
    add("GetWindowTheme", 185, 4, [mod](Marshaler& m) {
        const u32 hwnd = static_cast<u32>(m.arg(0));
        if (!mod->window_valid(hwnd)) {
            set_last_error(E_HANDLE);
            m.set_return(0);
            return;
        }
        auto lock = mod->lock();
        m.set_return(mod->window_state(hwnd).theme);
    });

    // HRESULT SetWindowTheme(HWND, LPCWSTR pszSubAppName, LPCWSTR pszSubIdList)
    add("SetWindowTheme", 196, 12, [mod](Marshaler& m) {
        constexpr u32 WM_THEMECHANGED = 0x031A;
        const u32 hwnd = static_cast<u32>(m.arg(0));
        if (!mod->window_valid(hwnd)) {
            m.set_return(E_HANDLE);
            return;
        }
        auto app = read_wide(m, m.arg_ptr(1));
        auto ids = read_wide(m, m.arg_ptr(2));
        {
            auto lock = mod->lock();
            auto& st = mod->window_state(hwnd);
            st.subapp = std::move(app);
            st.id_list = std::move(ids);
            st.theme = 0;
        }
        // Learn: the window receives WM_THEMECHANGED (measured: one, sent
        // synchronously before SetWindowTheme returns).
        if (mod->services().send_message) (void)mod->services().send_message(hwnd, WM_THEMECHANGED, 0, 0);
        set_last_error(0);
        m.set_return(S_OK);
    });

    // BOOL IsThemeActive(void) / IsAppThemed(void) / IsCompositionActive(void)
    add("IsThemeActive", 189, 0, [](Marshaler& m) {
        set_last_error(0);
        m.set_return(1);
    });
    add("IsAppThemed", 187, 0, [](Marshaler& m) {
        set_last_error(0);
        m.set_return(1);
    });
    add("IsCompositionActive", 188, 0, [](Marshaler& m) { m.set_return(1); });

    // DWORD GetThemeAppProperties(void) / void SetThemeAppProperties(DWORD).
    // Without STAP_ALLOW_CONTROLS OpenThemeData opens nothing (measured appprops).
    add("GetThemeAppProperties", 153, 0, [mod](Marshaler& m) { m.set_return(mod->app_properties); });
    add("SetThemeAppProperties", 195, 4, [mod](Marshaler& m) {
        mod->app_properties = static_cast<u32>(m.arg(0));
    });

    // HRESULT EnableThemeDialogTexture(HWND, DWORD): ETDT_DISABLE wins over
    // ETDT_ENABLE; flags with neither leave the state (measured dlgtexture).
    add("EnableThemeDialogTexture", 89, 8, [mod](Marshaler& m) {
        constexpr u32 ETDT_DISABLE = 1, ETDT_ENABLE = 2;
        const u32 hwnd = static_cast<u32>(m.arg(0));
        const u32 flags = static_cast<u32>(m.arg(1));
        if (!mod->window_valid(hwnd)) {
            set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(HR_INVALID_WINDOW_HANDLE);
            return;
        }
        auto lock = mod->lock();
        auto& st = mod->window_state(hwnd);
        if (flags & ETDT_DISABLE) st.dialog_texture = false;
        else if (flags & ETDT_ENABLE) st.dialog_texture = true;
        m.set_return(S_OK);
    });
    // BOOL IsThemeDialogTextureEnabled(HWND)
    add("IsThemeDialogTextureEnabled", 191, 4, [mod](Marshaler& m) {
        const u32 hwnd = static_cast<u32>(m.arg(0));
        if (!mod->window_valid(hwnd)) {
            m.set_return(0);
            return;
        }
        auto lock = mod->lock();
        m.set_return(mod->window_state(hwnd).dialog_texture ? 1 : 0);
    });

    // HRESULT EnableTheming(BOOL). Turning visual styles off is a per-user
    // system setting Vitreo does not change.
    add("EnableTheming", 103, 4, [](Marshaler& m) {
        if (m.arg(0) != 0) {
            m.set_return(S_OK);
            return;
        }
        unimplemented_once("EnableTheming: FALSE (turning visual styles off for the user)");
        m.set_return(E_NOTIMPL);
    });

    // HRESULT GetCurrentThemeName(LPWSTR, int, LPWSTR, int, LPWSTR, int).
    // Each buffer is filled in turn; one too small gets a truncated string
    // and ends the call with ERROR_INSUFFICIENT_BUFFER (measured).
    add("GetCurrentThemeName", 150, 24, [](Marshaler& m) {
        static constexpr std::u16string_view kValues[3] = {u"C:\\WINDOWS\\resources\\themes\\Aero\\Aero.msstyles",
                                                           u"NormalColor", u"NormalSize"};
        for (int i = 0; i < 3; ++i) {
            const GuestAddr buf = m.arg_ptr(static_cast<usize>(i * 2));
            const i32 cch = static_cast<i32>(m.arg(static_cast<usize>(i * 2 + 1)));
            if (buf == 0 || cch <= 0) continue;
            if (!write_wide(m, buf, static_cast<u32>(cch), kValues[i])) {
                m.set_return(HR_INSUFFICIENT_BUFFER);
                return;
            }
        }
        m.set_return(S_OK);
    });

    // HRESULT GetThemeDocumentationProperty(LPCWSTR pszThemeName, LPCWSTR pszPropertyName, LPWSTR, int)
    add("GetThemeDocumentationProperty", 160, 16, [](Marshaler& m) {
        constexpr u32 HR_FILE_NOT_FOUND = 0x80070002u, HR_RESOURCE_NAME_NOT_FOUND = 0x80070648u;
        const auto file = read_wide(m, m.arg_ptr(0));
        auto lower = [](std::u16string s) {
            for (auto& c : s) if (c >= u'A' && c <= u'Z') c = static_cast<char16_t>(c + 32);
            return s;
        };
        if (!file || lower(*file) != u"c:\\windows\\resources\\themes\\aero\\aero.msstyles") {
            set_last_error(2);
            m.set_return(HR_FILE_NOT_FOUND);
            return;
        }
        // The documentation strings are the theme file's own text, which
        // Vitreo does not ship.
        unimplemented_once("GetThemeDocumentationProperty: theme documentation strings");
        set_last_error(0x715);
        m.set_return(HR_RESOURCE_NAME_NOT_FOUND);
    });

    // COLORREF GetThemeSysColor(HTHEME, int): GetSysColor with or without a
    // theme; a bad handle is 0 with the last error E_HANDLE.
    add("GetThemeSysColor", 175, 8, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        if (h != 0 && !mod->theme(h)) {
            set_last_error(E_HANDLE);
            m.set_return(0);
            return;
        }
        set_last_error(0);
        m.set_return(sys_colors::get(static_cast<u32>(m.arg(1))));
    });
    // HBRUSH GetThemeSysColorBrush(HTHEME, int)
    add("GetThemeSysColorBrush", 176, 8, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        if (h != 0 && !mod->theme(h)) {
            set_last_error(E_HANDLE);
            m.set_return(0);
            return;
        }
        const u32 index = static_cast<u32>(m.arg(1));
        m.set_return(index < sys_colors::kCount ? (sys_colors::kSysBrushBase | index) : 0);
    });
    // int GetThemeSysSize(HTHEME, int iSizeId). Without a theme the caption
    // and scroll-bar sizes still come from the theme; other ids are
    // GetSystemMetrics. With a theme, ids the theme has no size for are 0
    // with E_INVALIDARG. A bad id is 0 with ERROR_INVALID_PARAMETER.
    add("GetThemeSysSize", 179, 8, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        const i32 id = static_cast<i32>(m.arg(1));
        if (h != 0 && !mod->theme(h)) {
            set_last_error(E_HANDLE);
            m.set_return(0);
            return;
        }
        if (id < 0 || id > 0x2003) {
            set_last_error(ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const auto size = theme_sys_size(id);
        if (h != 0) {
            if (!size) {
                set_last_error(E_INVALIDARG);
                m.set_return(0);
                return;
            }
            set_last_error(0);
            m.set_return(static_cast<u32>(*size));
            return;
        }
        set_last_error(0);
        if (size && id != 32 && id != 92) {
            m.set_return(static_cast<u32>(*size));
            return;
        }
        const i32 v = mod->services().system_metric ? mod->services().system_metric(id) : 0;
        m.set_return(static_cast<u32>(v));
    });
    // BOOL GetThemeSysBool(HTHEME, int): TMT_FLATMENUS is TRUE, every other id FALSE.
    add("GetThemeSysBool", 174, 8, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        if (h != 0 && !mod->theme(h)) {
            set_last_error(E_HANDLE);
            m.set_return(0);
            return;
        }
        set_last_error(0);
        m.set_return(static_cast<i32>(m.arg(1)) == tmt::FLATMENUS ? 1 : 0);
    });
    // HRESULT GetThemeSysInt(HTHEME, int, int*): needs a theme; TMT_MINCOLORDEPTH = 15.
    add("GetThemeSysInt", 178, 12, [mod](Marshaler& m) {
        if (!mod->theme(m.arg(0))) {
            m.set_return(E_HANDLE);
            return;
        }
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        if (static_cast<i32>(m.arg(1)) != tmt::MINCOLORDEPTH) {
            m.set_return(E_INVALIDARG);
            return;
        }
        (void)m.write_u32(out, 15);
        m.set_return(S_OK);
    });
    // HRESULT GetThemeSysFont(HTHEME, int, LOGFONTW*)
    add("GetThemeSysFont", 177, 12, [mod](Marshaler& m) {
        const u64 h = m.arg(0);
        if (h != 0 && !mod->theme(h)) {
            m.set_return(E_HANDLE);
            return;
        }
        const i32 id = static_cast<i32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        if (id < tmt::CAPTIONFONT || id > tmt::BODYFONT) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (id > tmt::ICONTITLEFONT) {
            // The heading and body fonts: a theme has none (zeroed LOGFONT);
            // without a theme the buffer is left alone (measured sysfont).
            if (h != 0)
                for (u32 i = 0; i < 92; i += 4) (void)m.write_u32(out + i, 0);
            m.set_return(S_OK);
            return;
        }
        // NONCLIENTMETRICS fonts: Segoe UI, 9 pt (-12 px), FW_NORMAL, DEFAULT_CHARSET.
        for (u32 i = 0; i < 92; i += 4) (void)m.write_u32(out + i, 0);
        (void)m.write_u32(out, static_cast<u32>(-12));
        (void)m.write_u32(out + 16, 400);
        (void)m.write_u8(out + 23, 1);
        (void)write_wide(m, out + 28, 32, u"Segoe UI");
        m.set_return(S_OK);
    });
    // HRESULT GetThemeSysString(HTHEME, int, LPWSTR, int)
    add("GetThemeSysString", 180, 16, [mod](Marshaler& m) {
        if (!mod->theme(m.arg(0))) {
            m.set_return(E_HANDLE);
            return;
        }
        const i32 id = static_cast<i32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        const i32 cch = static_cast<i32>(m.arg(3));
        if (out == 0) {
            m.set_return(E_POINTER);
            return;
        }
        if (id < tmt::CSSNAME || id > tmt::ALIAS) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (id == tmt::LASTUPDATED || id == tmt::ALIAS) {
            (void)write_wide(m, out, static_cast<u32>(std::max(cch, 0)), u"");
            m.set_return(S_OK);
            return;
        }
        // TMT_CSSNAME / TMT_XMLNAME are the theme file's own names.
        unimplemented_once("GetThemeSysString: TMT_CSSNAME/TMT_XMLNAME theme-file strings");
        m.set_return(E_NOTIMPL);
    });

    // BOOL BeginPanningFeedback(HWND) / UpdatePanningFeedback(HWND, LONG, LONG, BOOL)
    // / EndPanningFeedback(HWND, BOOL): the Windows boundary-feedback window
    // translation. macOS gives scroll feedback with its own elastic scroll.
    add("BeginPanningFeedback", 12, 4, [](Marshaler& m) {
        unimplemented_once("BeginPanningFeedback: boundary feedback window translation");
        m.set_return(0);
    });
    add("UpdatePanningFeedback", 199, 16, [](Marshaler& m) {
        unimplemented_once("UpdatePanningFeedback: boundary feedback window translation");
        m.set_return(0);
    });
    add("EndPanningFeedback", 134, 8, [](Marshaler& m) {
        unimplemented_once("EndPanningFeedback: boundary feedback window translation");
        m.set_return(0);
    });

    // BOOL ThemeInitApiHook(DWORD, PVOID): uxtheme's private user32 hook
    // installation, which only user32 calls.
    add("ThemeInitApiHook", 198, 8, [](Marshaler& m) { m.set_return(0); });

    // COM entry points: uxtheme registers no classes.
    add("DllCanUnloadNow", 52, 0, [](Marshaler& m) { m.set_return(S_FALSE); });
    add("DllGetClassObject", 54, 12, [](Marshaler& m) {
        if (const GuestAddr ppv = m.arg_ptr(2); ppv != 0) (void)m.write_ptr(ppv, 0);
        m.set_return(0x80040111u);  // CLASS_E_CLASSNOTAVAILABLE
    });
    add("DllGetActivationFactory", 53, 8, [](Marshaler& m) {
        if (const GuestAddr out = m.arg_ptr(1); out != 0) (void)m.write_ptr(out, 0);
        m.set_return(0x80040111u);
    });

    // Undocumented immersive-colour exports (not in uxtheme.lib).
    add("GetImmersiveColorFromColorSetEx", 95, 16, [](Marshaler& m) {
        unimplemented_once("GetImmersiveColorFromColorSetEx: immersive colour sets");
        m.set_return(0);
    });
    add("GetImmersiveUserColorSetPreference", 98, 8, [](Marshaler& m) {
        unimplemented_once("GetImmersiveUserColorSetPreference: immersive colour sets");
        m.set_return(0);
    });
    add("GetUserColorPreference", 120, 8, [](Marshaler& m) {
        unimplemented_once("GetUserColorPreference: immersive colour sets");
        m.set_return(E_NOTIMPL);
    });
    add("GetColorFromPreference", 121, 16, [](Marshaler& m) {
        unimplemented_once("GetColorFromPreference: immersive colour sets");
        m.set_return(0);
    });

    // HRESULT SetWindowThemeAttribute(HWND, WINDOWTHEMEATTRIBUTETYPE, PVOID, DWORD)
    add("SetWindowThemeAttribute", 197, 16, [mod](Marshaler& m) {
        constexpr u32 WTA_NONCLIENT = 1, WTNCA_VALIDBITS = 0xF, WTA_OPTIONS_SIZE = 8;
        const u32 hwnd = static_cast<u32>(m.arg(0));
        const u32 attribute = static_cast<u32>(m.arg(1));
        const GuestAddr options = m.arg_ptr(2);
        const u32 size = static_cast<u32>(m.arg(3));
        auto* hwnd_mgr = mod->services().hwnd_mgr;
        if (!mod->window_valid(hwnd)) {
            // Measured: E_FAIL with ERROR_INVALID_WINDOW_HANDLE.
            set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            m.set_return(E_FAIL);
            return;
        }
        if (attribute != WTA_NONCLIENT || options == 0 || size != WTA_OPTIONS_SIZE) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto flags = m.read_u32(options);
        const auto mask = m.read_u32(options + 4);
        if (flags.is_err() || mask.is_err()) {
            m.set_return(E_INVALIDARG);
            return;
        }
        if (!hwnd_mgr->set_theme_nonclient_attributes(hwnd, mask.value() & WTNCA_VALIDBITS,
                                                      flags.value() & WTNCA_VALIDBITS)) {
            m.set_return(E_HANDLE);
            return;
        }
        m.set_return(S_OK);
    });

    add_property_exports(exports, *mod);
    add_draw_exports(exports, *mod);
    add_buffered_paint_exports(exports, *mod);
    return exports;
}

}  // namespace vitreo::winapi

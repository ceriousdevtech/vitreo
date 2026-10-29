#pragma once

/// @file uxtheme_internal.hpp
/// @brief State shared by uxtheme's export files: theme handles, the
///        per-window theme state and the guest-memory helpers.

#include "winapi/ui/uxtheme/uxtheme.hpp"
#include "winapi/ui/uxtheme/theme_data.hpp"
#include "winapi/ui/uxtheme/theme_render.hpp"

#include "winapi/core/marshaler.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace vitreo::winapi::uxtheme {

inline constexpr u32 S_OK = 0x00000000u;
inline constexpr u32 S_FALSE = 0x00000001u;
inline constexpr u32 E_NOTIMPL = 0x80004001u;
inline constexpr u32 E_POINTER = 0x80004003u;
inline constexpr u32 E_FAIL = 0x80004005u;
inline constexpr u32 E_UNEXPECTED = 0x8000FFFFu;
inline constexpr u32 E_HANDLE = 0x80070006u;
inline constexpr u32 E_INVALIDARG = 0x80070057u;
inline constexpr u32 E_PROP_ID_UNSUPPORTED = 0x80070490u;
inline constexpr u32 HR_INSUFFICIENT_BUFFER = 0x8007007Au;
inline constexpr u32 HR_INVALID_WINDOW_HANDLE = 0x80070578u;
inline constexpr u32 ERROR_INVALID_PARAMETER = 87;
inline constexpr u32 ERROR_INVALID_WINDOW_HANDLE = 1400;

/// HRESULT_FROM_WIN32.
[[nodiscard]] inline u32 hresult_from_win32(u32 e) { return e == 0 ? 0 : ((e & 0xFFFFu) | 0x80070000u); }

/// One open theme: a measured class (with sub-app) and its appearance.
struct ThemeObject {
    Variant variant;
    bool dark = false;
    u32 dpi = 96;  ///< OpenThemeDataForDpi (96 otherwise)
    u32 refs = 0;
};

/// SetWindowTheme / OpenThemeData / EnableThemeDialogTexture state of a window.
struct WindowThemeState {
    std::optional<std::u16string> subapp;   ///< pszSubAppName (nullopt = NULL)
    std::optional<std::u16string> id_list;  ///< pszSubIdList (nullopt = NULL)
    u64 theme = 0;                          ///< GetWindowTheme
    bool dialog_texture = false;            ///< IsThemeDialogTextureEnabled
};

class UxThemeModule {
public:
    explicit UxThemeModule(UxThemeServices services) : m_services(std::move(services)) {}

    [[nodiscard]] const UxThemeServices& services() const { return m_services; }

    /// The theme a handle names, nullptr when it is not an open theme.
    [[nodiscard]] std::optional<ThemeObject> theme(u64 handle) const;
    /// OpenThemeData*: the handle (0 on failure) and the last error to set.
    u64 open(u32 hwnd, std::optional<std::u16string_view> class_list, u32& last_error, u32 dpi = 96);
    u32 close(u64 handle);

    [[nodiscard]] bool window_valid(u32 hwnd) const;
    WindowThemeState& window_state(u32 hwnd);  ///< caller holds lock()
    [[nodiscard]] std::unique_lock<std::mutex> lock() const { return std::unique_lock(m_mutex); }

    /// The palette a theme draws with.
    [[nodiscard]] windowing::ThemePalette palette(bool dark) const;

    u32 app_properties = 3;  ///< STAP_ALLOW_NONCLIENT | STAP_ALLOW_CONTROLS (measured default)

private:
    UxThemeServices m_services;
    mutable std::mutex m_mutex;
    /// One handle per (class variant, DPI): opening the same class again
    /// answers the same handle (measured "samehandle").
    std::map<std::pair<i32, u32>, u64> m_handles;
    std::unordered_map<u64, ThemeObject> m_open;
    u64 m_next_handle = 0xA000u;
    std::unordered_map<u32, WindowThemeState> m_windows;
};

// --- guest memory -------------------------------------------------------

/// A NUL-terminated UTF-16 string at @p addr (at most @p max units).
[[nodiscard]] std::optional<std::u16string> read_wide(Marshaler& m, GuestAddr addr, usize max = 0x8000);
/// Write @p s into a @p cch-unit buffer. Truncates (still terminated) when
/// it does not fit; returns false when it was truncated.
bool write_wide(Marshaler& m, GuestAddr addr, u32 cch, std::u16string_view s);
[[nodiscard]] bool read_rect(Marshaler& m, GuestAddr addr, i32 r[4]);
bool write_rect(Marshaler& m, GuestAddr addr, const i32 r[4]);

/// One-time "[vitreo] UNIMPLEMENTED uxtheme!<what>" line.
void unimplemented_once(const char* what);

/// Exports defined by each file.
void add_property_exports(DllExports& exports, UxThemeModule& mod);
void add_draw_exports(DllExports& exports, UxThemeModule& mod);
void add_buffered_paint_exports(DllExports& exports, UxThemeModule& mod);

/// The COLORREF a theme reports for a colour property (AppKit's colour for
/// that role, composited onto the part's background).
[[nodiscard]] u32 theme_colorref(const ThemeObject& theme, const windowing::ThemePalette& palette, i32 part,
                                 i32 state, i32 prop_id);

/// The background-relative pixels of a part, rendered at @p width x
/// @p height for 96-DPI drawing.
bool render_theme_part(const UxThemeModule& mod, const ThemeObject& theme, i32 part, i32 state, i32 width,
                       i32 height, const PartDrawOptions& options, PartCanvas& out);

}  // namespace vitreo::winapi::uxtheme

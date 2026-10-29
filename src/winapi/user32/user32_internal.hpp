#pragma once
/// @file user32_internal.hpp
/// @brief Shared surface between user32_ext.cpp and user32_ext_registration.cpp.
///
/// Private to user32's extended surface. Everything here was file-local to
/// user32_ext.cpp before its export table was split out; this header exists so
/// the two halves can still see each other, not to publish them.
///
/// Seven helpers appear out of the roughly sixty that were file-local, because
/// only seven are reached from inside a registration lambda. Those lambdas carry
/// real work rather than forwarding to a manager method — resource loading,
/// coordinate mapping, and key translation done inline at the registration site.

#include "capsule/vfs/vfs.hpp"
#include "windowing/model/hwnd_manager.hpp"

#include <vitreo/common.hpp>
#include <vitreo/result.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

struct DllExports;
class GlobalLocalManager;

/// The DDEML client exports (desktop/dde.cpp). @p global_local supplies the
/// guest memory behind DDE data objects; it is read at call time because the
/// user32 manager is wired after its exports are registered.
void add_dde_exports(DllExports& exports, GlobalLocalManager* const& global_local);

/// Expand a client rectangle in guest memory to the window rectangle implied by
/// `style`. Backs AdjustWindowRect / AdjustWindowRectEx.
[[nodiscard]] bool adjust_window_rect(GuestMemory& memory, GuestAddr rp, u32 style,
                                      bool has_menu, u32 ex_style);

/// Windows registered with AddClipboardFormatListener (WM_CLIPBOARDUPDATE).
[[nodiscard]] std::vector<u32> clipboard_listener_snapshot();

/// MapVirtualKey: translate between virtual-key codes, scan codes, and chars.
[[nodiscard]] u32 map_virtual_key(u32 uCode, u32 uMapType) noexcept;

/// ToUnicodeEx's translation (US layout, kbd_layout.hpp) into @p out (up to
/// @p capacity units); returns ToUnicodeEx's value. Shared with
/// TranslateMessage, which Windows implements on top of it.
[[nodiscard]] i32 to_unicode_us(u32 vk, u32 scan_code, const u8* key_state,
                                u16* out, i32 capacity, u32 flags) noexcept;

/// Widen a UTF-8 string to the UTF-16 the Windows W exports expect.
[[nodiscard]] std::wstring utf8_to_wstring(const std::string& s);
/// Inverse of `utf8_to_wstring`. DEFINED IN dialogs/dialog_helpers.cpp, which
/// already had the only real implementation; declared here so callers outside
/// the dialog code do not each re-derive one. There were four copies of this
/// function in user32 before this declaration existed.
[[nodiscard]] std::string wstring_to_utf8(const std::wstring& s);

/// Read an entire capsule file into memory.
[[nodiscard]] Result<std::vector<u8>, capsule::VfsError> read_all_bytes(
    capsule::Vfs& vfs,
    std::string_view win_path);

/// Where a mapped module's .rsrc directory lives.
struct ResourceDirInfo {
    GuestAddr image_base = 0;
    u32 rsrc_rva = 0;
    u32 rsrc_size = 0;
};

/// Locate the resource directory of a module already mapped into guest memory.
[[nodiscard]] std::optional<ResourceDirInfo> try_get_resource_dir_from_module(
    const GuestMemory& memory,
    GuestAddr module_base);

/// Wrap an RT_BITMAP DIB in a BITMAPFILEHEADER so it forms a complete BMP file.
/// RT_BITMAP resources omit the file header; NSImage requires it.
[[nodiscard]] std::vector<u8> dib_to_bmp_file(const u8* dib, u32 dib_size);

/// Screen position of a window's client-area top-left.
[[nodiscard]] windowing::Point client_origin_to_desktop(
    windowing::HwndManager& mgr,
    windowing::HWND hwnd);

/// VITREO_ENABLE_DIAG_LOGS gating. Defined in user32_ext.cpp.
[[nodiscard]] bool diag_logs_enabled() noexcept;

/// Fallback identity for the Cocoa-backed native-control wndproc, used only
/// until the real thunk address is known. It is NOT callable — a guest that
/// subclasses a control calls the value it gets back, so prefer
/// native_control_wndproc_address(), which returns a real thunk once user32
/// has been registered.
constexpr u64 kNativeControlWndProc = 0x7000FF10ull;

/// Record the callable thunk that implements the native-control wndproc.
void set_native_control_wndproc_address(u64 address) noexcept;

/// The address handed to guests for GWL_WNDPROC on a native control. A real,
/// callable thunk once registered; kNativeControlWndProc before that.
[[nodiscard]] u64 native_control_wndproc_address() noexcept;

[[nodiscard]] bool is_native_host_control(const windowing::WindowData& win) noexcept;
[[nodiscard]] u64 effective_wndproc(const windowing::WindowData& win) noexcept;
[[nodiscard]] bool is_native_control_wndproc(u64 proc) noexcept;

/// Property-atom/string key used by SetProp/GetProp/RemoveProp.
[[nodiscard]] std::string make_prop_key(class Marshaler& m, GuestAddr str_or_atom, bool ansi);

/// Parse an unsigned env override, or return fallback.
[[nodiscard]] u64 user32_env_u64_or(const char* name, u64 fallback);

/// IsChar* predicates (char_apis.cpp): which GetStringTypeW CT_CTYPE1 bits
/// make the character qualify. Alphanumeric is either C1_ALPHA or C1_DIGIT.
inline constexpr u16 kIsCharAlpha = 0x0100;                 // C1_ALPHA
inline constexpr u16 kIsCharAlphaNumeric = 0x0100 | 0x0004; // C1_ALPHA|C1_DIGIT
inline constexpr u16 kIsCharUpper = 0x0001;                 // C1_UPPER
inline constexpr u16 kIsCharLower = 0x0002;                 // C1_LOWER
/// 1 when the low WCHAR of @p arg has any of @p c1_mask, else 0.
[[nodiscard]] u32 user32_char_class_w(u64 arg, u16 c1_mask) noexcept;
/// The A form: the low byte converted through CP_ACP first.
[[nodiscard]] u32 user32_char_class_a(u64 arg, u16 c1_mask) noexcept;

} // namespace vitreo::winapi

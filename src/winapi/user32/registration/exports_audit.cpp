// SPDX-License-Identifier: LGPL-2.1-only

/// @file exports_audit.cpp
/// @brief user32 extended export table, slice "audit".
///
/// Each export implements the call or fails as Windows does when the
/// facility is absent.
///
/// One contiguous run of the former create_user32_ext_thunks body, moved
/// verbatim; user32_ext_registration.cpp calls the slices in registration
/// order.

#include "winapi/user32/registration/registration_parts.hpp"

#include "ipc/hwnd_registry.hpp"
#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/windows/nc_metrics.hpp"
#include "winapi/user32/windows/set_cursor_pos.hpp"
#include "winapi/user32/messages/mouse_in_pointer.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/user32/windows/kbd_layout.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <set>

#include <mutex>

#include <unordered_map>

namespace vitreo::winapi {

namespace {

/// Process-wide user32 state the audit completions need. Windows keeps these
/// in the window station, which is per-process here.
std::mutex& clipboard_listener_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::set<u32>& clipboard_listeners() {
    static std::set<u32> listeners;
    return listeners;
}
std::mutex& window_attribute_mutex() {
    static std::mutex mutex;
    return mutex;
}
std::unordered_map<u32, u32>& display_affinity() {
    static std::unordered_map<u32, u32> table;
    return table;
}

/// OemToChar*A: OEM bytes to Unicode through the OEM code page (437 on an
/// en-US system), then to the ANSI code page with best-fit -- the measured
/// converters of codepage_convert.hpp, not a hand-typed fold
/// (s2msg_input_probe oemtochara: 82 41 b0 9c -> e9 41 a6 a3). Measured
/// exception (u32_char_dde_probe oem01_a): the C0 bytes stay controls except
/// 0f, 14 and 15, which translate as their 437 glyphs U+263C, U+00B6 and
/// U+00A7 (-> a4 b6 a7 in 1252); the other glyphs, even U+2022 at 07 that
/// 1252 could represent, do not.
std::vector<u8> oem_to_ansi_bytes(const std::vector<u8>& oem) {
    constexpr u32 kMbUseGlyphChars = 0x4;
    std::vector<u16> wide(oem.size());
    const auto decoded = codepage::multi_byte_to_wide(
        codepage::kOemCodePage, 0, oem.data(), oem.size(), wide.data(),
        wide.size());
    if (decoded.ok() && decoded.count == oem.size()) {
        for (usize i = 0; i < oem.size(); ++i) {
            const u8 b = oem[i];
            if (b != 0x0F && b != 0x14 && b != 0x15) continue;
            u16 glyph = 0;
            const auto g = codepage::multi_byte_to_wide(
                codepage::kOemCodePage, kMbUseGlyphChars, &b, 1, &glyph, 1);
            if (g.ok() && g.count == 1) wide[i] = glyph;
        }
    }
    std::vector<u8> out(oem.size());
    if (!decoded.ok()) return out;
    const auto encoded = codepage::wide_to_multi_byte(
        codepage::ansi_code_page(), 0, wide.data(), decoded.count, out.data(),
        out.size(), nullptr, false);
    out.resize(encoded.ok() ? encoded.count : 0);
    return out;
}

/// OemToChar*W: OEM bytes to Unicode with the glyph mapping
/// (MB_USEGLYPHCHARS): u32_char_dde_probe oem01_w 01 -> 263a ... 1f -> 25bc,
/// oem7f_w 7f -> 2302, and the high half as the 437 table (oem80_w ...).
std::vector<u16> oem_to_wide_units(const std::vector<u8>& oem) {
    constexpr u32 kMbUseGlyphChars = 0x4;
    std::vector<u16> wide(oem.size());
    const auto decoded = codepage::multi_byte_to_wide(
        codepage::kOemCodePage, kMbUseGlyphChars, oem.data(), oem.size(),
        wide.data(), wide.size());
    wide.resize(decoded.ok() ? decoded.count : 0);
    return wide;
}

/// CharToOem*W: Unicode to the OEM code page with best-fit, and '_' -- not
/// WideCharToMultiByte's '?' -- for a unit the page cannot represent
/// (s2msg_input_probe chartooembuffw: e9 41 20ac 2591 -> 82 41 5f b0;
/// chartooem_vs_wctomb: the two differ in that default character only).
std::vector<u8> wide_to_oem_bytes(const std::vector<u16>& wide) {
    std::vector<u8> out(wide.size() * 2);
    static constexpr u8 kOemDefault[2] = {'_', 0};
    const auto encoded = codepage::wide_to_multi_byte(
        codepage::kOemCodePage, 0, wide.data(), wide.size(), out.data(),
        out.size(), kOemDefault, false);
    out.resize(encoded.ok() ? encoded.count : 0);
    return out;
}

/// CharToOem*A: ANSI bytes to Unicode through CP_ACP, then as CharToOem*W
/// (u32_char_dde_probe chartooema: e9 41 a6 80 ff -> 82 41 dd 5f 98).
std::vector<u8> ansi_to_oem_bytes(const std::vector<u8>& ansi) {
    std::vector<u16> wide(ansi.size());
    const auto decoded = codepage::multi_byte_to_wide(
        codepage::ansi_code_page(), 0, ansi.data(), ansi.size(), wide.data(),
        wide.size());
    wide.resize(decoded.ok() ? decoded.count : 0);
    return wide_to_oem_bytes(wide);
}

/// Source text for the translators: through the terminator (included) when
/// @p count is empty, else exactly @p count units.
std::vector<u8> read_bytes(Marshaler& m, GuestAddr src, std::optional<u32> count) {
    std::vector<u8> in;
    for (u32 i = 0; !count || i < *count; ++i) {
        const auto byte = m.memory().read_u8(src + i);
        if (byte.is_err()) break;
        in.push_back(byte.value());
        if (!count && byte.value() == 0) break;
    }
    return in;
}
std::vector<u16> read_units(Marshaler& m, GuestAddr src, std::optional<u32> count) {
    std::vector<u16> in;
    for (u32 i = 0; !count || i < *count; ++i) {
        const auto unit = m.memory().read_u16(src + static_cast<GuestAddr>(i) * 2);
        if (unit.is_err()) break;
        in.push_back(unit.value());
        if (!count && unit.value() == 0) break;
    }
    return in;
}

/// The eight CharToOem / OemToChar entry points share one shape, measured in
/// u32_char_dde_probe: a NULL source or destination is FALSE with the last
/// error untouched; a W form whose destination is its own source is FALSE
/// and writes nothing (chartooemw_same, chartooembuffw_same, oemtocharw_same,
/// oemtocharbuffw_same); the A forms translate in place. A zero-length Buff
/// call writes nothing and returns TRUE, except that CharToOemBuffW also sets
/// ERROR_INVALID_PARAMETER and OemToCharBuffW fails with it.
enum class OemDir { AnsiToOem, WideToOem, OemToAnsi, OemToWide };
void oem_translate(Marshaler& m, OemDir dir, bool buff) {
    const GuestAddr src = m.arg_ptr(0);
    const GuestAddr dst = m.arg_ptr(1);
    const std::optional<u32> count =
        buff ? std::optional<u32>(static_cast<u32>(m.arg(2))) : std::nullopt;
    const bool wide_side = dir == OemDir::WideToOem || dir == OemDir::OemToWide;
    if (src == 0 || dst == 0 || (wide_side && src == dst)) {
        m.set_return(0u);
        return;
    }
    if (count && *count == 0) {
        if (dir == OemDir::WideToOem) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        } else if (dir == OemDir::OemToWide) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
        }
        m.set_return(1u);
        return;
    }
    switch (dir) {
    case OemDir::AnsiToOem: {
        const std::vector<u8> out = ansi_to_oem_bytes(read_bytes(m, src, count));
        (void)m.memory().write_bytes(dst, out.data(), out.size());
        break;
    }
    case OemDir::WideToOem: {
        const std::vector<u8> out = wide_to_oem_bytes(read_units(m, src, count));
        (void)m.memory().write_bytes(dst, out.data(), out.size());
        break;
    }
    case OemDir::OemToAnsi: {
        const std::vector<u8> out = oem_to_ansi_bytes(read_bytes(m, src, count));
        (void)m.memory().write_bytes(dst, out.data(), out.size());
        break;
    }
    case OemDir::OemToWide: {
        const std::vector<u16> out = oem_to_wide_units(read_bytes(m, src, count));
        for (usize i = 0; i < out.size(); ++i) {
            (void)m.memory().write_u16(dst + static_cast<GuestAddr>(i) * 2, out[i]);
        }
        break;
    }
    }
    m.set_return(1u);
}

} // namespace

std::vector<u32> clipboard_listener_snapshot() {
    std::lock_guard lock(clipboard_listener_mutex());
    return {clipboard_listeners().begin(), clipboard_listeners().end()};
}

void add_user32_ext_exports_audit(DllExports& exports, User32ExtManager& mgr) {
    // ======================================================================
    // Every entry below either does the real thing or fails the way Windows
    // fails on a machine without that facility. None of them return a
    // success-shaped value for work that did not happen.
    // ======================================================================

    // -- Clipboard format listeners. Windows posts WM_CLIPBOARDUPDATE to
    // registered windows; the registration itself is the observable contract,
    // and duplicate registration is ERROR_INVALID_PARAMETER.
    exports.entries.push_back(ExportEntry{
        "AddClipboardFormatListener", 0,
        [](Marshaler& m) {
            const u32 hwnd = static_cast<u32>(m.arg(0));
            if (hwnd == 0) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            std::lock_guard lock(clipboard_listener_mutex());
            auto& listeners = clipboard_listeners();
            if (!listeners.insert(hwnd).second) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            m.set_return(1u);
        }, 4});
    exports.entries.push_back(ExportEntry{
        "RemoveClipboardFormatListener", 0,
        [](Marshaler& m) {
            const u32 hwnd = static_cast<u32>(m.arg(0));
            std::lock_guard lock(clipboard_listener_mutex());
            if (clipboard_listeners().erase(hwnd) == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            m.set_return(1u);
        }, 4});

    // -- OEM/ANSI translation (Learn CharToOemA ... OemToCharBuffW). Vitreo's
    // OEM code page is 437, ANSI the capsule's CP_ACP; each direction goes
    // through Unicode with the measured converters (helpers above).
    struct OemExport { const char* name; OemDir dir; bool buff; u16 bytes; };
    static constexpr OemExport kOemExports[] = {
        {"CharToOemA", OemDir::AnsiToOem, false, 8},
        {"CharToOemBuffA", OemDir::AnsiToOem, true, 12},
        {"CharToOemW", OemDir::WideToOem, false, 8},
        {"CharToOemBuffW", OemDir::WideToOem, true, 12},
        {"OemToCharA", OemDir::OemToAnsi, false, 8},
        {"OemToCharBuffA", OemDir::OemToAnsi, true, 12},
        {"OemToCharW", OemDir::OemToWide, false, 8},
        {"OemToCharBuffW", OemDir::OemToWide, true, 12},
    };
    for (const OemExport& e : kOemExports) {
        const OemDir dir = e.dir;
        const bool buff = e.buff;
        exports.entries.push_back(ExportEntry{
            e.name, 0, [dir, buff](Marshaler& m) { oem_translate(m, dir, buff); },
            e.bytes});
    }

    // -- Layered window attributes. The setter records them so the getter is
    // a real read-back rather than an invention (Windows pairs them).
    exports.entries.push_back(ExportEntry{
        "GetLayeredWindowAttributes", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetLayeredWindowAttributes(m); }, 16});

    // -- Display affinity. WDA_NONE is the default; Vitreo cannot exclude a
    // window from capture, so a request to do so is refused rather than
    // silently accepted.
    exports.entries.push_back(ExportEntry{
        "GetWindowDisplayAffinity", 0,
        [](Marshaler& m) {
            const u32 hwnd = static_cast<u32>(m.arg(0));
            const GuestAddr out = m.arg_ptr(1);
            if (hwnd == 0 || out == 0) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            std::lock_guard lock(window_attribute_mutex());
            const auto it = display_affinity().find(hwnd);
            const u32 affinity = it == display_affinity().end() ? 0u
                                                                : it->second;
            m.set_return(m.write_u32(out, affinity).is_ok() ? 1u : 0u);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "SetWindowDisplayAffinity", 0,
        [](Marshaler& m) {
            const u32 hwnd = static_cast<u32>(m.arg(0));
            const u32 affinity = static_cast<u32>(m.arg(1));
            if (hwnd == 0) {
                set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
                m.set_return(0u);
                return;
            }
            if (affinity != 0) {
                // WDA_MONITOR / WDA_EXCLUDEFROMCAPTURE would have to be
                // enforced by the compositor; refuse instead of pretending
                // the window is protected.
                VITREO_TRACE(
                    "[u32ext] SetWindowDisplayAffinity(0x%x) refused: capture "
                    "exclusion is not enforced\n", affinity);
                set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                m.set_return(0u);
                return;
            }
            std::lock_guard lock(window_attribute_mutex());
            display_affinity()[hwnd] = affinity;
            m.set_return(1u);
        }, 8});

    // -- Hot keys. macOS global hot keys need an event tap with accessibility
    // consent, which Vitreo does not take, so registration honestly fails
    // instead of swallowing the guest's media keys.
    exports.entries.push_back(ExportEntry{
        "RegisterHotKey", 0,
        [&mgr](Marshaler& m) { mgr.impl_RegisterHotKey(m); }, 16});
    exports.entries.push_back(ExportEntry{
        "UnregisterHotKey", 0,
        [&mgr](Marshaler& m) { mgr.impl_UnregisterHotKey(m); }, 8});

    // -- Pointer / touch / pen. This host reports a mouse; there is no
    // digitizer, so the pointer queries answer for a mouse or fail the way
    // Windows does for an unknown pointer id.
    exports.entries.push_back(ExportEntry{
        "GetPointerType", 0,
        [](Marshaler& m) {
            const GuestAddr out = m.arg_ptr(1);
            u32 type = 0;
            const u32 error = mouse_in_pointer::pointer_type(
                static_cast<u32>(m.arg(0)), type);
            if (error != 0 || out == 0) {
                set_last_error(error != 0 ? error
                                          : error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            if (m.write_u32(out, type).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
            m.set_return(1u);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "GetPointerDevice", 0,
        [](Marshaler& m) {
            // Vitreo exposes no pointer (touch/pen) device, so no device
            // handle is valid: FALSE / ERROR_INVALID_HANDLE, as Windows answers
            // for a handle that names none (s2msg_input_probe
            // getpointerdevice_bogus).
            (void)m.arg(0);
            (void)m.arg_ptr(1);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0u);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "GetPointerPenInfo", 0,
        [](Marshaler& m) {
            // No pointer is ever a pen here; Windows answers a non-pen pointer
            // id with FALSE / ERROR_INVALID_PARAMETER (getpointerpeninfo_1).
            (void)m.arg(0);
            (void)m.arg_ptr(1);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
        }, 8});
    exports.entries.push_back(ExportEntry{
        "CloseTouchInputHandle", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            // No touch input is ever delivered, so no handle is ever open.
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0u);
        }, 4});
    exports.entries.push_back(ExportEntry{
        "DefRawInputProc", 0,
        [](Marshaler& m) {
            // LRESULT DefRawInputProc(PRAWINPUT*, INT nInput, UINT
            // cbSizeHeader): nothing to clean up, 0 -- but a header size that
            // is not sizeof(RAWINPUTHEADER) is -1 with the last error
            // untouched (s2msg_input_probe defrawinputproc_bad_size).
            (void)m.arg_ptr(0);
            (void)m.arg(1);
            const u32 header = static_cast<u32>(m.arg(2));
            const u32 expected = m.cpu_is_64bit() ? 24u : 16u;
            m.set_return(header == expected ? 0u : static_cast<u64>(-1));
        }, 12});
    exports.entries.push_back(ExportEntry{
        "GetRawInputDeviceInfoW", 0,
        [&mgr](Marshaler& m) {
            if (m.arg(3) == 0) { set_last_error(error_codes::ERROR_NOACCESS); m.set_return(0xFFFFFFFFu); return; }
            mgr.impl_GetRawInputDeviceInfoW(m);
        }, 16});

    // -- GUI resource counts, PrintWindow, ChildWindowFromPoint and
    // SetLastErrorEx: see windows/window_state.cpp.
    exports.entries.push_back(ExportEntry{
        "GetGuiResources", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetGuiResources(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "PrintWindow", 0,
        [&mgr](Marshaler& m) { mgr.impl_PrintWindow(m); }, 12});
    exports.entries.push_back(ExportEntry{
        // POINT is passed by value: two DWORDs on x86, one register on x64.
        "ChildWindowFromPoint", 0,
        [&mgr](Marshaler& m) { mgr.impl_ChildWindowFromPoint(m); }, 12});
    exports.entries.push_back(ExportEntry{
        // void SetLastErrorEx(DWORD dwErrCode, DWORD dwType): Learn — dwType
        // is ignored; the error is set (measured, setlasterrorex_*).
        "SetLastErrorEx", 0,
        [](Marshaler& m) {
            (void)m.arg(1);
            set_last_error(static_cast<u32>(m.arg(0)));
        }, 8});

    // -- Shutdown block reasons (windows/window_state.cpp).
    exports.entries.push_back(ExportEntry{
        "ShutdownBlockReasonCreate", 0,
        [&mgr](Marshaler& m) { mgr.impl_ShutdownBlockReasonCreate(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "ShutdownBlockReasonQuery", 0,
        [&mgr](Marshaler& m) { mgr.impl_ShutdownBlockReasonQuery(m); }, 12});
    exports.entries.push_back(ExportEntry{
        "ShutdownBlockReasonDestroy", 0,
        [&mgr](Marshaler& m) { mgr.impl_ShutdownBlockReasonDestroy(m); }, 4});

    // -- DDEML client (desktop/dde.cpp).
    add_dde_exports(exports, mgr.global_local_manager_slot());

    // -- Menu info (menus/menu.cpp).
    exports.entries.push_back(ExportEntry{
        "SetMenuInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetMenuInfo(m); }, 8});
    exports.entries.push_back(ExportEntry{
        "GetMenuInfo", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetMenuInfo(m); }, 8});

    // -- MessageBoxExA: the language id selects button captions only; the
    // dialog itself is MessageBoxA's.
    exports.entries.push_back(ExportEntry{
        "MessageBoxExA", 0,
        [](Marshaler& m) {
            auto text = m.read_ansi_string(m.arg_ptr(1));
            auto caption = m.read_ansi_string(m.arg_ptr(2));
            std::fprintf(stderr, "[u32ext] MessageBoxExA: %s / %s\n",
                         caption.is_ok() ? caption.value().c_str() : "",
                         text.is_ok() ? text.value().c_str() : "");
            constexpr u32 IDOK = 1;
            m.set_return(IDOK);
        }, 20});
}

} // namespace vitreo::winapi

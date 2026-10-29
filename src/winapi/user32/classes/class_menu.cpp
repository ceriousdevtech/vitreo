// SPDX-License-Identifier: LGPL-2.1-only

/// @file class_menu.cpp
/// @brief Auto-load class menu from lpszMenuName.
///
/// Closed cut from user32_ext.cpp — verified with plan_extraction.py.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/user32/user32_internal.hpp"
#include "winapi/common/display_device_identity.hpp"
#include "winapi/user32/painting/painting.hpp"
#include "winapi/user32/messages/message_loop.hpp"
#include "winapi/user32/messages/message_send.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/hwnd_registry.hpp"
#include "windowing/resources/dialog_template.hpp"
#include "windowing/resources/resource_walker.hpp"
#include "winapi/user32/classes/window_class.hpp"
#include "winapi/user32/windows/window.hpp"
#include "winapi/user32/dialogs/dialog.hpp"
#include "winapi/user32/windows/show_window.hpp"
#include "winapi/user32/resources/resources.hpp"
#include "core/pe/parser/pe_structures.hpp"
#include "vitreo/result.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Auto-load class menu from lpszMenuName
// =============================================================================


void User32ExtManager::impl_SetCapture(Marshaler& m) {
    // HWND SetCapture(HWND hWnd)
    //
    // Learn: the window then receives every mouse message until capture is
    // released, wherever the pointer goes. The event pump does the routing, so
    // it has to be told; recording the handle here alone left capture inert.
    //
    // s2msg_input_probe: an invalid window fails with
    // ERROR_INVALID_WINDOW_HANDLE; success sets the last error to 0 and
    // returns the previous capture of the caller's queue; the window losing
    // capture receives WM_CAPTURECHANGED with lParam = the new capture
    // window (setcapture_second_prev_first: capturechanged=1,
    // lparam_is_second=1). SetCapture(NULL) releases.
    auto hwnd = static_cast<windowing::HWND>(m.arg(0));
    if (hwnd != windowing::NULL_HWND && !m_hwnd_mgr.is_valid(hwnd)) {
        set_last_error(error_codes::ERROR_INVALID_WINDOW_HANDLE);
        m.set_return(0u);
        return;
    }
    const windowing::HWND old = m_hwnd_mgr.get_capture();
    const windowing::HWND prev = window_in_caller_queue(old) ? old : 0;
    if (hwnd == windowing::NULL_HWND) {
        m_hwnd_mgr.release_capture();
    } else {
        (void)m_hwnd_mgr.set_capture(hwnd);
    }
    m_host.set_mouse_capture(hwnd);
    constexpr u32 WM_CAPTURECHANGED = 0x0215;
    if (old != windowing::NULL_HWND && old != hwnd && m_msgsend_mgr != nullptr &&
        m_hwnd_mgr.is_valid(old)) {
        (void)m_msgsend_mgr->send_message(old, WM_CAPTURECHANGED, 0,
                                          static_cast<u64>(hwnd));
    }
    // Setting capture clears the last error (setcapture_prev_none err=0);
    // SetCapture(NULL) with nothing captured leaves it (setcapture_null).
    if (hwnd != windowing::NULL_HWND || old != windowing::NULL_HWND) set_last_error(0);
    VITREO_TRACE("[u32ext] SetCapture(0x%x) -> prev=0x%x\n", hwnd, prev);
    m.set_return(prev);
}

void User32ExtManager::impl_WinHelpW(Marshaler& m) {
    // BOOL WinHelpW(HWND hWndMain, LPCWSTR lpszHelp, UINT uCommand, ULONG_PTR dwData)
    (void)m.arg(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    m.set_return(1); // TRUE — no-op
}


// Accelerator table flags (ACCEL::fVirt).
namespace accel_flags {
constexpr u16 VIRTKEY  = 0x01;
constexpr u16 SHIFT    = 0x04;
constexpr u16 CONTROL  = 0x08;
constexpr u16 ALT      = 0x10;
constexpr u16 LAST     = 0x80;  // terminates the resource table
} // namespace accel_flags

void User32ExtManager::resolve_module_resources(u32 hInstance, GuestAddr& base_out,
                                                u32& rva_out, u32& size_out) const {
    base_out = m_rsrc_image_base;
    rva_out  = m_rsrc_rva;
    size_out = m_rsrc_size;
    if (hInstance == 0 || static_cast<GuestAddr>(hInstance) == m_rsrc_image_base) {
        return;
    }
    u32 rva = 0;
    u32 size = 0;
    if (m_module_mgr &&
        m_module_mgr->resource_directory_for_base(static_cast<GuestAddr>(hInstance),
                                                  rva, size)) {
        base_out = static_cast<GuestAddr>(hInstance);
        rva_out  = rva;
        size_out = size;
    }
}

u32 User32ExtManager::load_accelerator_table(Marshaler& m, u32 hInstance,
                                             u32 lpTableName, bool wide) {
    GuestAddr rsrc_base = 0;
    u32 rsrc_rva = 0;
    u32 rsrc_size = 0;
    resolve_module_resources(hInstance, rsrc_base, rsrc_rva, rsrc_size);
    if (rsrc_rva == 0 || rsrc_size == 0) return 0;

    // RT_ACCELERATOR resources may be named by integer id or by string.
    windowing::ResourceEntry entry{};
    if ((lpTableName >> 16) == 0) {
        entry = windowing::find_resource(m_memory, rsrc_base, rsrc_rva,
                                         rsrc_size, windowing::RT::ACCELERATOR,
                                         static_cast<u16>(lpTableName));
    } else {
        auto r = wide ? m.read_wide_string(static_cast<GuestAddr>(lpTableName))
                      : m.read_ansi_string(static_cast<GuestAddr>(lpTableName));
        if (r.is_err()) return 0;
        std::u16string name;
        for (char c : r.value()) {
            name.push_back(static_cast<char16_t>(
                static_cast<unsigned char>(c >= 'a' && c <= 'z' ? c - 32 : c)));
        }
        entry = windowing::find_resource_by_string_name(
            m_memory, rsrc_base, rsrc_rva, rsrc_size,
            windowing::RT::ACCELERATOR, name.c_str(),
            static_cast<u16>(name.size()));
    }
    if (!entry.data || entry.size < 8) {
        VITREO_TRACE("[u32ext] LoadAccelerators: RT_ACCELERATOR name=0x%x not found"
                     " (rsrc base=0x%llx rva=0x%x size=%u)\n",
                     lpTableName,
                     static_cast<unsigned long long>(rsrc_base),
                     rsrc_rva, rsrc_size);
        return 0;
    }

    // Resource layout is a run of 8-byte ACCELTABLEENTRY records
    // { WORD fFlags; WORD wAnsi; WORD wId; WORD padding; }; the final record
    // carries the 0x80 end marker in fFlags.
    std::vector<AcceleratorEntry> table;
    const u8* p = entry.data;
    for (u32 offset = 0; offset + 8 <= entry.size; offset += 8) {
        const u16 flags = static_cast<u16>(p[offset] | (p[offset + 1] << 8));
        const u16 key   = static_cast<u16>(p[offset + 2] | (p[offset + 3] << 8));
        const u16 cmd   = static_cast<u16>(p[offset + 4] | (p[offset + 5] << 8));
        table.push_back(AcceleratorEntry{
            static_cast<u16>(flags & ~accel_flags::LAST), key, cmd});
        if (flags & accel_flags::LAST) break;
    }
    if (table.empty()) return 0;

    const u32 handle = m_next_accel_handle++;
    if (std::getenv("VITREO_INPUT_DIAG")) {
        for (const auto& e : table) {
            std::fprintf(stderr,
                         "[input]   accel key=0x%02x flags=0x%02x cmd=%u\n",
                         e.key, e.flags, e.cmd);
        }
    }
    m_accel_tables.emplace(handle, std::move(table));
    VITREO_TRACE("[u32ext] LoadAccelerators: table=0x%x entries=%zu -> haccel=0x%x\n",
                 lpTableName, m_accel_tables[handle].size(), handle);
    return handle;
}

bool User32ExtManager::translate_accelerator(u32 hwnd, u32 haccel,
                                             const windowing::MSG& msg) {
    // Windows only translates keyboard messages.
    const bool is_key = msg.message == windowing::WM::KEYDOWN ||
                        msg.message == windowing::WM::SYSKEYDOWN;
    const bool is_char = msg.message == windowing::WM::CHAR_MSG ||
                         msg.message == windowing::WM::SYSCHAR;
    if (!is_key && !is_char) return false;

    auto it = m_accel_tables.find(haccel);
    if (it == m_accel_tables.end()) return false;

    constexpr i16 DOWN = static_cast<i16>(0x8000);
    const bool shift = (thread_key_state(0x10) & DOWN) != 0;
    const bool ctrl  = (thread_key_state(0x11) & DOWN) != 0;
    const bool alt   = (thread_key_state(0x12) & DOWN) != 0;

    for (const auto& e : it->second) {
        const bool virt = (e.flags & accel_flags::VIRTKEY) != 0;
        // A virtual-key accelerator matches WM_KEYDOWN; a character
        // accelerator matches WM_CHAR.
        if (virt != is_key) continue;
        if (e.key != static_cast<u16>(msg.wParam)) continue;
        if (virt) {
            if (((e.flags & accel_flags::SHIFT) != 0)   != shift) continue;
            if (((e.flags & accel_flags::CONTROL) != 0) != ctrl)  continue;
            if (((e.flags & accel_flags::ALT) != 0)     != alt)   continue;
        }

        // Deliver WM_COMMAND with the accelerator notification code (1).
        windowing::MSG command{};
        command.hwnd    = static_cast<windowing::HWND>(hwnd);
        command.message = windowing::WM::COMMAND;
        command.wParam  = (static_cast<u64>(1) << 16) | e.cmd;
        command.lParam  = 0;
        m_queue.post(command);
        VITREO_TRACE("[u32ext] TranslateAccelerator: vk=0x%llx shift=%d ctrl=%d alt=%d"
                     " -> WM_COMMAND id=%u\n",
                     static_cast<unsigned long long>(msg.wParam),
                     shift ? 1 : 0, ctrl ? 1 : 0, alt ? 1 : 0, e.cmd);
        return true;
    }
    if (std::getenv("VITREO_INPUT_DIAG") && is_key) {
        std::fprintf(stderr,
                     "[input] TranslateAccelerator: no match vk=0x%02llx"
                     " shift=%d ctrl=%d alt=%d (haccel=0x%x)\n",
                     static_cast<unsigned long long>(msg.wParam),
                     shift ? 1 : 0, ctrl ? 1 : 0, alt ? 1 : 0, haccel);
    }
    return false;
}

void User32ExtManager::impl_TranslateAcceleratorW(Marshaler& m) {
    // int TranslateAcceleratorW(HWND hWnd, HACCEL hAccTable, LPMSG lpMsg)
    const auto hwnd   = static_cast<u32>(m.arg(0));
    const auto haccel = static_cast<u32>(m.arg(1));
    const GuestAddr msg_ptr = m.arg_ptr(2);
    if (haccel == 0 || msg_ptr == 0) {
        m.set_return(0);
        return;
    }

    // MSG layout differs between x86 and x64; read the fields this needs.
    const bool wide_ptr = m.cpu_is_64bit();
    windowing::MSG msg{};
    if (wide_ptr) {
        msg.hwnd    = static_cast<windowing::HWND>(m.read_u64(msg_ptr).value_or(0));
        msg.message = static_cast<u32>(m.read_u32(msg_ptr + 8).value_or(0));
        msg.wParam  = m.read_u64(msg_ptr + 16).value_or(0);
        msg.lParam  = m.read_u64(msg_ptr + 24).value_or(0);
    } else {
        msg.hwnd    = static_cast<windowing::HWND>(m.read_u32(msg_ptr).value_or(0));
        msg.message = m.read_u32(msg_ptr + 4).value_or(0);
        msg.wParam  = m.read_u32(msg_ptr + 8).value_or(0);
        msg.lParam  = m.read_u32(msg_ptr + 12).value_or(0);
    }
    m.set_return(translate_accelerator(hwnd, haccel, msg) ? 1u : 0u);
}

void User32ExtManager::impl_LoadAcceleratorsW(Marshaler& m) {
    (void)m.arg(0);  // hInstance: resources come from the main module
    const u32 handle = load_accelerator_table(m, static_cast<u32>(m.arg(0)),
                                          static_cast<u32>(m.arg(1)), true);
    if (handle == 0) {
        VITREO_TRACE("[u32ext] LoadAcceleratorsW: no RT_ACCELERATOR resource -> NULL\n");
        set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
    }
    m.set_return(handle);
}

void User32ExtManager::impl_LoadAcceleratorsA(Marshaler& m) {
    VITREO_TRACE("[u32ext] LoadAcceleratorsA hInstance=0x%llx name=0x%llx\n",
                 static_cast<unsigned long long>(m.arg(0)),
                 static_cast<unsigned long long>(m.arg(1)));
    const u32 handle = load_accelerator_table(m, static_cast<u32>(m.arg(0)),
                                          static_cast<u32>(m.arg(1)), false);
    if (handle == 0) {
        VITREO_TRACE("[u32ext] LoadAcceleratorsA: no RT_ACCELERATOR resource -> NULL\n");
        set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
    }
    m.set_return(handle);
}

} // namespace vitreo::winapi

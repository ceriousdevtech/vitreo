// SPDX-License-Identifier: LGPL-2.1-only

/// @file menu_accel.cpp
/// @brief Remaining Menu and Accelerator Table APIs.
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

#include <algorithm>
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

// ----- Menu APIs -----







void User32ExtManager::impl_ShowOwnedPopups(Marshaler& m) {
    u32 hwnd = static_cast<u32>(m.arg(0));
    u32 fShow = static_cast<u32>(m.arg(1));
    auto top = m_hwnd_mgr.enum_children(windowing::HWND_DESKTOP);
    for (windowing::HWND candidate : top) {
        windowing::WindowData* wd = m_hwnd_mgr.find(candidate);
        if (!wd || wd->owner != hwnd) {
            continue;
        }

        if (fShow) {
            if (wd->hidden_by_owner) {
                wd->hidden_by_owner = false;
                wd->visible = true;
                m_host.set_visible(candidate, true);

                windowing::MSG msg{};
                msg.hwnd = candidate;
                msg.message = windowing::WM::SHOWWINDOW;
                msg.wParam = 1;
                m_queue.post(msg);
            }
        } else if (wd->visible) {
            wd->hidden_by_owner = true;
            wd->visible = false;
            m_host.set_visible(candidate, false);

            windowing::MSG msg{};
            msg.hwnd = candidate;
            msg.message = windowing::WM::SHOWWINDOW;
            msg.wParam = 0;
            m_queue.post(msg);
        }
    }

    VITREO_TRACE("[u32ext] ShowOwnedPopups hwnd=0x%x show=%u -> TRUE\n", hwnd, fShow);
    m.set_return(1u);
}











// ----- Accelerator Table APIs -----

void User32ExtManager::impl_CreateAcceleratorTableW(Marshaler& m) {
    // HACCEL CreateAcceleratorTable(LPACCEL paccel, int cAccel)
    // ACCEL is { BYTE fVirt; WORD key; WORD cmd; } with WORD alignment (6 bytes).
    const GuestAddr entries = m.arg_ptr(0);
    const i32 count = static_cast<i32>(m.arg(1));
    if (entries == 0 || count <= 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    std::vector<AcceleratorEntry> table;
    table.reserve(static_cast<usize>(count));
    for (i32 i = 0; i < count; ++i) {
        const GuestAddr row = entries + static_cast<u32>(i) * 6u;
        const u8 fvirt = m.read_u8(row).value_or(0);
        const u16 key = static_cast<u16>(m.read_u16(row + 2).value_or(0));
        const u16 cmd = static_cast<u16>(m.read_u16(row + 4).value_or(0));
        table.push_back(AcceleratorEntry{fvirt, key, cmd});
    }

    const u32 handle = m_next_accel_handle++;
    m_accel_tables.emplace(handle, std::move(table));
    VITREO_TRACE("[u32ext] CreateAcceleratorTable -> 0x%x entries=%d\n",
                 handle, count);
    m.set_return(handle);
}

void User32ExtManager::impl_CopyAcceleratorTableW(Marshaler& m) {
    // int CopyAcceleratorTable(HACCEL hAccelSrc, LPACCEL lpAccelDst, int cAccelEntries)
    const u32 haccel = static_cast<u32>(m.arg(0));
    const GuestAddr dest = m.arg_ptr(1);
    const i32 max_entries = static_cast<i32>(m.arg(2));
    const auto it = m_accel_tables.find(haccel);
    if (it == m_accel_tables.end()) {
        m.set_return(0);
        return;
    }
    const auto& table = it->second;
    if (dest == 0) {
        m.set_return(static_cast<u32>(table.size()));
        return;
    }
    if (max_entries <= 0) {
        m.set_return(0);
        return;
    }
    const u32 copy = std::min(static_cast<u32>(max_entries),
                              static_cast<u32>(table.size()));
    for (u32 i = 0; i < copy; ++i) {
        const GuestAddr row = dest + i * 6u;
        (void)m.write_u8(row, static_cast<u8>(table[i].flags));
        (void)m.write_u8(row + 1, 0);
        (void)m.write_u16(row + 2, table[i].key);
        (void)m.write_u16(row + 4, table[i].cmd);
    }
    VITREO_TRACE("[u32ext] CopyAcceleratorTable h=0x%x dest=0x%llx -> %u\n",
                 haccel, static_cast<unsigned long long>(dest), copy);
    m.set_return(copy);
}

void User32ExtManager::impl_DestroyAcceleratorTable(Marshaler& m) {
    u32 hAccel = static_cast<u32>(m.arg(0));
    const bool found = m_accel_tables.erase(hAccel) != 0;
    VITREO_TRACE("[u32ext] DestroyAcceleratorTable 0x%x -> %s\n",
                 hAccel, found ? "TRUE" : "FALSE");
    m.set_return(found ? 1u : 0u);
}

} // namespace vitreo::winapi

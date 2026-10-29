// SPDX-License-Identifier: LGPL-2.1-only

/// @file char_nav.cpp
/// @brief CharNext / CharPrev navigation.
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

// libicucore general category; the same classification GetStringTypeW uses
// for C3_NONSPACING.
extern "C" int8_t u_charType(int32_t c);

namespace vitreo::winapi {

// ---------- Character navigation ----------
//
// Pointers are guest addresses and are returned whole: an x64 guest's
// strings live above 4 GiB (image .rdata at 0x14000xxxx), and the old
// 32-bit return pointed CharNextW callers at unrelated memory
// (s2msg_input_probe charnextw_* under Vitreo).

namespace {
constexpr int8_t kNonSpacingMark = 6;   // U_NON_SPACING_MARK
constexpr int8_t kEnclosingMark = 7;    // U_ENCLOSING_MARK

bool is_nonspacing(u16 unit) noexcept {
    const int8_t cat = u_charType(static_cast<int32_t>(unit));
    return cat == kNonSpacingMark || cat == kEnclosingMark;
}
} // namespace

void User32ExtManager::impl_CharNextA(Marshaler& m) {
    // CP_ACP is single-byte (1252) in Vitreo: one byte, or none at the NUL.
    const GuestAddr ptr = m.arg_ptr(0);
    if (ptr == 0) {
        m.set_return(0);
        return;
    }
    auto ch = m_memory.read_u8(ptr);
    m.set_return(ch.is_ok() && ch.value() != 0 ? ptr + 1 : ptr);
}

void User32ExtManager::impl_CharNextW(Marshaler& m) {
    // Learn CharNextW: steps over a character together with the combining
    // (non-spacing) marks that follow it; a surrogate is one unit
    // (s2msg_input_probe charnextw_combining=2, charnextw_surrogate=4,
    // charnextw_at_end=6).
    const GuestAddr ptr = m.arg_ptr(0);
    if (ptr == 0) {
        m.set_return(0);
        return;
    }
    auto ch = m_memory.read_u16(ptr);
    if (!ch.is_ok() || ch.value() == 0) {
        m.set_return(ptr);
        return;
    }
    GuestAddr next = ptr + 2;
    for (;;) {
        auto unit = m_memory.read_u16(next);
        if (!unit.is_ok() || unit.value() == 0 || !is_nonspacing(unit.value())) break;
        next += 2;
    }
    m.set_return(next);
}

void User32ExtManager::impl_CharNextExA(Marshaler& m) {
    // LPSTR CharNextExA(WORD CodePage, LPCSTR lpCurrentChar, DWORD dwFlags)
    // Learn: CP_ACP/OEM/MAC select the code page for lead-byte checks.
    // Vitreo's ACP path is single-byte (same as CharNextA); DBCS lead-byte
    // stepping is not implemented for other code pages yet.
    (void)m.arg(0);
    (void)m.arg(2);
    const GuestAddr ptr = m.arg_ptr(1);
    if (ptr == 0) {
        m.set_return(0);
        return;
    }
    auto ch = m_memory.read_u8(ptr);
    m.set_return(ch.is_ok() && ch.value() != 0 ? ptr + 1 : ptr);
}

void User32ExtManager::impl_CharPrevA(Marshaler& m) {
    // LPSTR CharPrevA(LPCSTR lpszStart, LPCSTR lpszCurrent)
    // Single-byte (CP_ACP) path matching CharNextA: step back one byte, or
    // return lpszStart when already at the start.
    const GuestAddr start = m.arg_ptr(0);
    const GuestAddr current = m.arg_ptr(1);
    m.set_return(current > start ? current - 1 : start);
}

void User32ExtManager::impl_CharPrevW(Marshaler& m) {
    // The start of the character (base plus combining marks) before
    // lpszCurrent; lpszStart when there is none (charprevw_start=0,
    // charprevw_after_combining=0, charprevw_after_surrogate=4).
    const GuestAddr start = m.arg_ptr(0);
    const GuestAddr current = m.arg_ptr(1);
    if (current <= start) {
        m.set_return(start);
        return;
    }
    GuestAddr prev = current - 2;
    while (prev > start) {
        auto unit = m_memory.read_u16(prev);
        if (!unit.is_ok() || !is_nonspacing(unit.value())) break;
        prev -= 2;
    }
    m.set_return(prev);
}

} // namespace vitreo::winapi

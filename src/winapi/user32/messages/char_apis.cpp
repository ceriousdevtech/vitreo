// SPDX-License-Identifier: LGPL-2.1-only

/// @file char_apis.cpp
/// @brief Character APIs.
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
#include "winapi/kernel32/locale/codepage_convert.hpp"
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

// ----- Character APIs -----

void User32ExtManager::impl_CharUpperBuffW(Marshaler& m) {
    GuestAddr lpsz = m.arg_ptr(0);
    u32 cchLength = static_cast<u32>(m.arg(1));
    for (u32 i = 0; i < cchLength; ++i) {
        u16 ch = m.read_u16(lpsz + i * 2).value_or(0);
        if (ch >= u'a' && ch <= u'z') {
            (void)m.write_u16(lpsz + i * 2, static_cast<u16>(ch - u'a' + u'A'));
        }
    }
    VITREO_TRACE("[u32ext] CharUpperBuffW len=%u -> %u\n", cchLength, cchLength);
    m.set_return(cchLength);
}

u32 user32_char_class_w(u64 arg, u16 c1_mask) noexcept {
    const u16 ch = static_cast<u16>(arg);
    return (codepage::ctype1_flags(ch) & c1_mask) != 0 ? 1u : 0u;
}

u32 user32_char_class_a(u64 arg, u16 c1_mask) noexcept {
    const u8 byte = static_cast<u8>(arg);
    u16 wide = 0;
    const auto r = codepage::multi_byte_to_wide(codepage::ansi_code_page(), 0,
                                                &byte, 1, &wide, 1);
    if (!r.ok() || r.count != 1) return 0u;
    return user32_char_class_w(wide, c1_mask);
}

void User32ExtManager::impl_IsCharLowerW(Marshaler& m) {
    m.set_return(user32_char_class_w(m.arg(0), kIsCharLower));
}

} // namespace vitreo::winapi

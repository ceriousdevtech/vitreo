// SPDX-License-Identifier: LGPL-2.1-only

/// @file mdi.cpp
/// @brief MDI APIs.
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

// ----- MDI APIs -----

void User32ExtManager::impl_DefFrameProcW(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2);
    (void)m.arg(3); (void)m.arg(4);
    VITREO_TRACE("[u32ext] DefFrameProcW -> 0\n");
    m.set_return(0u);
}

void User32ExtManager::impl_DefMDIChildProcW(Marshaler& m) {
    (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
    VITREO_TRACE("[u32ext] DefMDIChildProcW -> 0\n");
    m.set_return(0u);
}

void User32ExtManager::impl_TranslateMDISysAccel(Marshaler& m) {
    (void)m.arg(0); (void)m.arg_ptr(1);
    VITREO_TRACE("[u32ext] TranslateMDISysAccel -> FALSE\n");
    m.set_return(0u);
}

} // namespace vitreo::winapi

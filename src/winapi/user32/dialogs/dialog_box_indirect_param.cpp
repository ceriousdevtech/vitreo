// SPDX-License-Identifier: LGPL-2.1-only

/// @file dialog_box_indirect_param.cpp
/// @brief DialogBoxIndirectParamA/W. Split out of the former mixed USER32 stubs file
///        as verbatim code motion.

#include "winapi/user32/user32_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/user32/dialogs/dialog.hpp"

namespace vitreo::winapi {

void User32ExtManager::impl_DialogBoxIndirectParamA(Marshaler& m) {
    // DLGTEMPLATE and DLGTEMPLATEEX store their strings as UTF-16 for both
    // entry points, so the ANSI and Unicode functions share the same parser.
    impl_DialogBoxIndirectParamW(m);
}

void User32ExtManager::impl_DialogBoxIndirectParamW(Marshaler& m) {
    const u32 hinstance = static_cast<u32>(m.arg(0));
    const GuestAddr template_ptr = m.arg_ptr(1);
    const auto parent = static_cast<windowing::HWND>(m.arg(2));
    const u64 dlgproc = m.arg(3);
    const u64 init_param = m.arg(4);

    if (template_ptr == 0 || dlgproc == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        VITREO_TRACE(
            "[u32ext] DialogBoxIndirectParam template=0x%llx "
            "dlgproc=0x%llx -> -1 (invalid parameter)\n",
            static_cast<unsigned long long>(template_ptr),
            static_cast<unsigned long long>(dlgproc));
        m.set_return(static_cast<u64>(-1));
        return;
    }

    if (m_dialog_mgr == nullptr) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        VITREO_TRACE(
            "[u32ext] DialogBoxIndirectParam template=0x%llx -> -1 "
            "(dialog manager unavailable)\n",
            static_cast<unsigned long long>(template_ptr));
        m.set_return(static_cast<u64>(-1));
        return;
    }

    m_dialog_mgr->set_creation_hook(
        [this](windowing::HWND hwnd, u32 style) {
            return dispatch_cbt_create_hook(hwnd, style);
        });

    VITREO_TRACE(
        "[u32ext] DialogBoxIndirectParam hinst=0x%x template=0x%llx "
        "parent=0x%x dlgproc=0x%llx init=0x%llx\n",
        hinstance,
        static_cast<unsigned long long>(template_ptr),
        parent,
        static_cast<unsigned long long>(dlgproc),
        static_cast<unsigned long long>(init_param));

    const i32 result = m_dialog_mgr->dialog_box(
        hinstance,
        static_cast<u64>(template_ptr),
        parent,
        dlgproc,
        init_param,
        true);
    VITREO_TRACE("[u32ext] DialogBoxIndirectParam -> %d\n", result);
    m.set_return(static_cast<u64>(static_cast<i64>(result)));
}

} // namespace vitreo::winapi

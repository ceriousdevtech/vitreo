/// @file credui.cpp
/// @brief CredUIPromptForWindowsCredentialsW/A — no credential-provider UI.

#include "credui.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi {
namespace {

constexpr u32 CREDUIWIN_IN_CRED_ONLY = 0x20;

void cred_ui_prompt_for_windows_credentials(Marshaler& m) {
    const GuestAddr auth_package = m.arg_ptr(2);
    const GuestAddr in_auth_buffer = m.arg_ptr(3);
    const GuestAddr out_auth_buffer = m.arg_ptr(5);
    const GuestAddr out_auth_buffer_size = m.arg_ptr(6);
    const u32 flags = static_cast<u32>(m.arg(8));

    if (auth_package == 0 || out_auth_buffer == 0 || out_auth_buffer_size == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    // Learn: CREDUIWIN_IN_CRED_ONLY with a null pvInAuthBuffer fails.
    if ((flags & CREDUIWIN_IN_CRED_ONLY) != 0 && in_auth_buffer == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }

    // Learn: ERROR_SUCCESS on submit; ERROR_CANCELLED if the user cancels.
    // Vitreo has no credential-provider UI, so cancel honestly and do not
    // invent a packed credential BLOB.
    (void)m.write_ptr(out_auth_buffer, 0);
    (void)m.write_u32(out_auth_buffer_size, 0);
    m.set_return(error_codes::ERROR_CANCELLED);
}

}  // namespace

DllExports create_credui_thunks() {
    DllExports exports;
    exports.entries.push_back(
        {"CredUIPromptForWindowsCredentialsW", 0,
         [](Marshaler& m) { cred_ui_prompt_for_windows_credentials(m); }, 36});
    exports.entries.push_back(
        {"CredUIPromptForWindowsCredentialsA", 0,
         [](Marshaler& m) { cred_ui_prompt_for_windows_credentials(m); }, 36});
    return exports;
}

}  // namespace vitreo::winapi

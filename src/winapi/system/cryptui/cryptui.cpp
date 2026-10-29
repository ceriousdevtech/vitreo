/// @file cryptui.cpp
/// @brief CryptUIDlgViewCertificateA/W — no certificate-viewer dialog.

#include "cryptui.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

namespace vitreo::winapi {
namespace {

// Learn: https://learn.microsoft.com/en-us/windows/win32/api/cryptuiapi/nf-cryptuiapi-cryptuidlgviewcertificatew
// Success is TRUE; failure is FALSE and GetLastError. Vitreo has no
// certificate-viewer UI, so a non-null view struct is treated as a dismissed
// dialog (ERROR_CANCELLED). Properties are never modified.
void cryptui_dlg_view_certificate(Marshaler& m) {
    const GuestAddr view_info = m.arg_ptr(0);
    const GuestAddr properties_changed = m.arg_ptr(1);

    if (view_info == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    if (properties_changed != 0) {
        (void)m.write_u32(properties_changed, 0);
    }

    set_last_error(error_codes::ERROR_CANCELLED);
    m.set_return(0);
}

}  // namespace

DllExports create_cryptui_thunks() {
    DllExports exports;
    exports.entries.push_back(
        {"CryptUIDlgViewCertificateW", 0,
         [](Marshaler& m) { cryptui_dlg_view_certificate(m); }, 8});
    exports.entries.push_back(
        {"CryptUIDlgViewCertificateA", 0,
         [](Marshaler& m) { cryptui_dlg_view_certificate(m); }, 8});
    // CryptUIDlgCertMgr — the certificate manager is a shell dialog; there is
    // no UI to show, and returning TRUE would claim the user had managed
    // certificates.
    exports.entries.push_back(ExportEntry{
        "CryptUIDlgCertMgr", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            set_last_error(50u);  // ERROR_NOT_SUPPORTED
            m.set_return(0u);
        }, 4});

    return exports;
}

}  // namespace vitreo::winapi

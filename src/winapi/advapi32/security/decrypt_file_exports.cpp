// SPDX-License-Identifier: LGPL-2.1-only

/// @file decrypt_file_exports.cpp
/// @brief advapi32!DecryptFileA/W.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "capsule/vfs/vfs.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <string>
#include <utility>

namespace vitreo::winapi {

/// Registers DecryptFileA/W.
void add_decrypt_file_exports(DllExports& exports, capsule::Vfs* vfs) {
    // BOOL DecryptFileA/W(LPCTSTR lpFileName, DWORD dwReserved)
    // Microsoft Learn: succeeds (nonzero) when the file is not encrypted.
    // Vitreo has no EFS; unencrypted capsule files therefore succeed. Missing
    // paths still fail with ERROR_FILE_NOT_FOUND when a VFS is wired.
    auto decrypt_file = [vfs](Marshaler& m, bool ansi) {
        const GuestAddr path_ptr = m.arg_ptr(0);
        const u32 reserved = static_cast<u32>(m.arg(1));
        if (path_ptr == 0 || reserved != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        if (vfs != nullptr) {
            std::string path;
            if (ansi) {
                auto r = m.read_ansi_string(path_ptr);
                if (r.is_err()) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                path = std::move(r.value());
            } else {
                auto r = m.read_wide_string(path_ptr);
                if (r.is_err()) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                path = std::move(r.value());
            }
            if (vfs->stat(path).is_err()) {
                set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
                m.set_return(0);
                return;
            }
        }
        m.set_return(1); // TRUE — not encrypted / decrypt no-op
    };
    exports.entries.push_back(
        {"DecryptFileA", 0,
         [decrypt_file](Marshaler& m) { decrypt_file(m, true); }, 8});
    exports.entries.push_back(
        {"DecryptFileW", 0,
         [decrypt_file](Marshaler& m) { decrypt_file(m, false); }, 8});
}

} // namespace vitreo::winapi

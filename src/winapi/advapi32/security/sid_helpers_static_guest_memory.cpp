// SPDX-License-Identifier: LGPL-2.1-only

/// @file security.cpp
/// @brief advapi32 security stubs implementation (WIN32-062)

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "winapi/common/elevation.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "capsule/vfs/vfs.hpp"
#include "ipc/ipc_gate.hpp"

#include <array>
#include <cstdio>
#include <cctype>
#include <cwctype>
#include <cstring>
#include <stdlib.h> // arc4random_buf
#include <unistd.h>
#include <pwd.h>
#include <cerrno>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <set>
#include <variant>
#include <CommonCrypto/CommonDigest.h>

#ifdef __APPLE__
#include <os/log.h>
#include <sys/xattr.h>
#endif

/// @file sid_helpers_static_guest_memory.cpp
/// @brief SID helpers (static, guest-memory based)
///
/// Closed cut from security.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// SID helpers (static, guest-memory based)
// =============================================================================

GuestAddr SecurityStubs::get_sid_identifier_authority(GuestAddr sid) {
    return sid + 2;  // offset 2 = IdentifierAuthority[6]
}

u32 SecurityStubs::get_length_sid(Marshaler& m, GuestAddr sid) {
    auto sub_count = m.read_u8(sid + 1);
    if (sub_count.is_err()) return 0;
    return 8 + 4 * static_cast<u32>(sub_count.value());
}

u32 SecurityStubs::get_sid_length_required(u8 sub_auth_count) {
    return 8 + 4 * static_cast<u32>(sub_auth_count);
}

u32 SecurityStubs::is_valid_sid(Marshaler& m, GuestAddr sid) {
    auto revision = m.read_u8(sid);
    if (revision.is_err()) return 0;
    auto sub_count = m.read_u8(sid + 1);
    if (sub_count.is_err()) return 0;
    return (revision.value() == 1 && sub_count.value() <= 15) ? 1 : 0;
}

u32 SecurityStubs::copy_sid(Marshaler& m, u32 dest_len, GuestAddr dest,
                            GuestAddr src) {
    // Returns ERROR_SUCCESS or the Win32 error CopySid reports. The whole
    // source is read before anything is written, so a failure never leaves a
    // partial SID in the destination.
    auto sub_count = m.read_u8(src + 1);
    if (src == 0 || sub_count.is_err()) return error_codes::ERROR_NOACCESS;
    const u32 sid_len = 8 + 4 * static_cast<u32>(sub_count.value());
    if (dest_len < sid_len) return error_codes::ERROR_INSUFFICIENT_BUFFER;
    std::array<u8, 8 + 4 * 255> bytes{};
    if (m.memory().read_bytes(src, bytes.data(), sid_len).is_err()) {
        return error_codes::ERROR_NOACCESS;
    }
    if (dest == 0 || m.memory().write_bytes(dest, bytes.data(), sid_len).is_err()) {
        return error_codes::ERROR_NOACCESS;
    }
    return error_codes::ERROR_SUCCESS;
}

GuestAddr SecurityStubs::get_sid_sub_authority(GuestAddr sid, u32 index) {
    return sid + 8 + index * 4;
}

GuestAddr SecurityStubs::get_sid_sub_authority_count(GuestAddr sid) {
    return sid + 1;
}

u32 SecurityStubs::initialize_sid(Marshaler& m, GuestAddr sid,
                                  GuestAddr authority_ptr, u8 sub_auth_count) {
    // Write Revision = 1
    auto r = m.write_u8(sid, 1);
    if (r.is_err()) return 0;
    // Write SubAuthorityCount
    r = m.write_u8(sid + 1, sub_auth_count);
    if (r.is_err()) return 0;
    // Copy 6-byte IdentifierAuthority from authority_ptr
    for (u32 i = 0; i < 6; ++i) {
        auto b = m.read_u8(authority_ptr + i);
        if (b.is_err()) return 0;
        r = m.write_u8(sid + 2 + i, b.value());
        if (r.is_err()) return 0;
    }
    // Zero out sub-authorities
    for (u32 i = 0; i < sub_auth_count; ++i) {
        auto w = m.write_u32(sid + 8 + i * 4, 0);
        if (w.is_err()) return 0;
    }
    return 1; // TRUE
}

u32 SecurityStubs::equal_sid(Marshaler& m, GuestAddr sid1, GuestAddr sid2) {
    // Read sub-authority counts
    auto c1 = m.read_u8(sid1 + 1);
    auto c2 = m.read_u8(sid2 + 1);
    if (c1.is_err() || c2.is_err()) return 0;
    if (c1.value() != c2.value()) return 0;
    u32 len = 8 + 4 * static_cast<u32>(c1.value());
    for (u32 i = 0; i < len; ++i) {
        auto b1 = m.read_u8(sid1 + i);
        auto b2 = m.read_u8(sid2 + i);
        if (b1.is_err() || b2.is_err()) return 0;
        if (b1.value() != b2.value()) return 0;
    }
    return 1; // TRUE
}

} // namespace vitreo::winapi

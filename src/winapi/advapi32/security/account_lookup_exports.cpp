// SPDX-License-Identifier: LGPL-2.1-only

/// @file account_lookup_exports.cpp
/// @brief advapi32 account and privilege lookups: LookupPrivilegeValueA/W,
/// LookupPrivilegeNameA/W, LookupPrivilegeDisplayNameW, LookupAccountSidW/NameW,
/// GetCurrentHwProfileW and the SID accessors (GetSidIdentifierAuthority ..
/// EqualSid).
///
/// The two functions are separate because the set-file-security,
/// event-source and descriptor-layout exports sit between them in the
/// registration order (see security_registration.cpp); regrouping them would
/// move thunk addresses.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <cctype>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

/// BOOL LookupPrivilegeValue{A,W}(LPCTSTR lpSystemName, LPCTSTR lpName,
/// PLUID lpLuid) — stdcall 12. tools/oracle_probe/advapi_wksid_probe:
///  - an unknown name: FALSE, ERROR_NO_SUCH_PRIVILEGE, *lpLuid zeroed;
///  - NULL or "" system: success with last error ERROR_SUCCESS; another name
///    for this machine (".", "\\.", "localhost") succeeds with last error
///    ERROR_IO_PENDING (the LSA RPC leaves it; probe lpv_system);
///  - a remote system: FALSE, RPC_S_SERVER_UNAVAILABLE, *lpLuid untouched.
void lookup_privilege_value(Marshaler& m, SecurityStubs& stubs, bool wide) {
    constexpr u32 ERROR_NO_SUCH_PRIVILEGE = 1313;
    constexpr u32 ERROR_IO_PENDING = 997;
    constexpr u32 RPC_S_SERVER_UNAVAILABLE = 1722;
    const GuestAddr system = m.arg_ptr(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const GuestAddr luid_ptr = m.arg_ptr(2);
    if (luid_ptr == 0 || name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    u32 success_error = error_codes::ERROR_SUCCESS;
    if (system != 0) {
        auto sys = wide ? m.read_wide_string(system) : m.read_ansi_string(system);
        const std::string text = sys.is_ok() ? sys.value() : std::string();
        if (!text.empty()) {
            const std::wstring wtext(text.begin(), text.end());
            std::string lower;
            for (char c : text) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (!SecurityStubs::is_local_system_name(wtext) && lower != "localhost" &&
                lower != "\\\\localhost") {
                set_last_error(RPC_S_SERVER_UNAVAILABLE);
                m.set_return(0);
                return;
            }
            success_error = ERROR_IO_PENDING;
        }
    }
    auto name_res = wide ? m.read_wide_string(name_ptr) : m.read_ansi_string(name_ptr);
    std::wstring name_w;
    if (name_res.is_ok()) {
        for (char ch : name_res.value()) {
            name_w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
        }
    }
    u64 luid = 0;
    if (name_res.is_err() || !stubs.lookup_privilege_value_w(name_w, luid)) {
        (void)m.write_u32(luid_ptr, 0);
        (void)m.write_u32(luid_ptr + 4, 0);
        set_last_error(ERROR_NO_SUCH_PRIVILEGE);
        m.set_return(0);
        return;
    }
    (void)m.write_u32(luid_ptr, static_cast<u32>(luid & 0xFFFFFFFFu));
    (void)m.write_u32(luid_ptr + 4, static_cast<u32>(luid >> 32));
    set_last_error(success_error);
    m.set_return(1);
}

} // namespace

/// Registers LookupPrivilegeValueW, LookupAccountSidW/NameW,
/// GetCurrentHwProfileW, the SID accessors (GetSidIdentifierAuthority ..
/// EqualSid), LookupPrivilegeNameA/W and LookupPrivilegeDisplayNameW.
void add_account_lookup_exports(DllExports& exports, SecurityStubs& stubs) {
    // BOOL LookupPrivilegeValueW(LPCWSTR, LPCWSTR, PLUID)
    exports.entries.push_back({
        "LookupPrivilegeValueW", 6,
        [&stubs](Marshaler& m) { lookup_privilege_value(m, stubs, true); },
        12
    });

    // BOOL LookupAccountSidW(LPCWSTR, PSID, LPWSTR, LPDWORD, LPWSTR, LPDWORD, PSID_NAME_USE)
    exports.entries.push_back({
        "LookupAccountSidW", 7,
        [&stubs](Marshaler& m) {
            GuestAddr sid_ptr = m.arg_ptr(1);
            GuestAddr name_buf = m.arg_ptr(2);
            // Only this machine can be asked; a remote lpSystemName is an
            // unreachable RPC server (advapi_token_probe las_remote: 0x6ba).
            if (const GuestAddr system = m.arg_ptr(0); system != 0) {
                std::wstring system_name;
                for (GuestAddr p = system; system_name.size() < 260; p += 2) {
                    const u16 ch = m.read_u16(p).value_or(0);
                    if (ch == 0) break;
                    system_name.push_back(static_cast<wchar_t>(ch));
                }
                if (!SecurityStubs::is_local_system_name(system_name)) {
                    set_last_error(1722u /*RPC_S_SERVER_UNAVAILABLE*/);
                    m.set_return(0);
                    return;
                }
            }
            GuestAddr name_len_ptr = m.arg_ptr(3);
            GuestAddr domain_buf = m.arg_ptr(4);
            GuestAddr domain_len_ptr = m.arg_ptr(5);
            GuestAddr use_ptr = m.arg_ptr(6);

            if (sid_ptr == 0 || name_len_ptr == 0 || domain_len_ptr == 0 ||
                use_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            if (SecurityStubs::is_valid_sid(m, sid_ptr) == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            u32 name_len = m.read_u32(name_len_ptr).is_ok() ? m.read_u32(name_len_ptr).value() : 0;
            u32 domain_len = m.read_u32(domain_len_ptr).is_ok() ? m.read_u32(domain_len_ptr).value() : 0;

            const u32 sid_len = SecurityStubs::get_length_sid(m, sid_ptr);
            std::vector<u8> sid(sid_len);
            if (sid_len == 0 ||
                m.memory().read_bytes(
                    sid_ptr, reinterpret_cast<Byte*>(sid.data()), sid_len)
                    .is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            std::wstring name;
            std::wstring domain;
            u32 use = 0;
            if (!stubs.lookup_account_sid_w(sid, name, domain, use)) {
                set_last_error(error_codes::ERROR_NONE_MAPPED);
                m.set_return(0);
                return;
            }

            u32 need_name = static_cast<u32>(name.size() + 1);
            u32 need_domain = static_cast<u32>(domain.size() + 1);

            if (name_buf == 0 || domain_buf == 0 || name_len < need_name || domain_len < need_domain) {
                (void)m.write_u32(name_len_ptr, need_name);
                (void)m.write_u32(domain_len_ptr, need_domain);
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }

            for (u32 i = 0; i < static_cast<u32>(name.size()); ++i) {
                (void)m.write_u16(name_buf + i * 2, static_cast<u16>(name[i]));
            }
            (void)m.write_u16(name_buf + static_cast<u32>(name.size()) * 2, 0);
            for (u32 i = 0; i < static_cast<u32>(domain.size()); ++i) {
                (void)m.write_u16(domain_buf + i * 2, static_cast<u16>(domain[i]));
            }
            (void)m.write_u16(domain_buf + static_cast<u32>(domain.size()) * 2, 0);

            (void)m.write_u32(name_len_ptr, static_cast<u32>(name.size()));
            (void)m.write_u32(domain_len_ptr, static_cast<u32>(domain.size()));
            (void)m.write_u32(use_ptr, use);

            m.set_return(1);
        },
        28
    });

    // BOOL LookupAccountNameW(LPCWSTR, LPCWSTR, PSID, LPDWORD, LPWSTR,
    //                         LPDWORD, PSID_NAME_USE)
    exports.entries.push_back({
        "LookupAccountNameW", 0,
        [&stubs](Marshaler& m) {
            const GuestAddr system_ptr = m.arg_ptr(0);
            const GuestAddr account_ptr = m.arg_ptr(1);
            const GuestAddr sid_buffer = m.arg_ptr(2);
            const GuestAddr sid_size_ptr = m.arg_ptr(3);
            const GuestAddr domain_buffer = m.arg_ptr(4);
            const GuestAddr domain_size_ptr = m.arg_ptr(5);
            const GuestAddr use_ptr = m.arg_ptr(6);

            if (account_ptr == 0 || sid_size_ptr == 0 ||
                domain_size_ptr == 0 || use_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const auto account_result = m.read_wide_string(account_ptr);
            const auto system_result = system_ptr == 0
                ? VResult<std::string>::ok({})
                : m.read_wide_string(system_ptr);
            const auto sid_size_result = m.read_u32(sid_size_ptr);
            const auto domain_size_result = m.read_u32(domain_size_ptr);
            if (account_result.is_err() || system_result.is_err() ||
                sid_size_result.is_err() || domain_size_result.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const u32 sid_capacity = sid_size_result.value();
            const u32 domain_capacity = domain_size_result.value();
            if ((sid_buffer == 0 && sid_capacity != 0) ||
                (domain_buffer == 0 && domain_capacity != 0)) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            const std::wstring system(system_result.value().begin(),
                                      system_result.value().end());
            const std::wstring account(account_result.value().begin(),
                                       account_result.value().end());
            std::vector<u8> sid;
            std::wstring domain;
            u32 use = 0;
            if (!stubs.lookup_account_name_w(
                    system, account, sid, domain, use)) {
                VITREO_TRACE(
                    "[advapi32] LookupAccountNameW account=\"%s\" "
                    "system=\"%s\" -> ERROR_NONE_MAPPED\n",
                    account_result.value().c_str(),
                    system_result.value().c_str());
                set_last_error(error_codes::ERROR_NONE_MAPPED);
                m.set_return(0);
                return;
            }

            const u32 required_sid = static_cast<u32>(sid.size());
            const u32 required_domain =
                static_cast<u32>(domain.size() + 1);
            if (sid_buffer == 0 || sid_capacity < required_sid ||
                domain_buffer == 0 ||
                domain_capacity < required_domain) {
                (void)m.write_u32(sid_size_ptr, required_sid);
                (void)m.write_u32(domain_size_ptr, required_domain);
                VITREO_TRACE(
                    "[advapi32] LookupAccountNameW account=\"%s\" "
                    "size query sid=%u domain=%u\n",
                    account_result.value().c_str(), required_sid,
                    required_domain);
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }

            for (u32 i = 0; i < required_sid; ++i) {
                if (m.write_u8(sid_buffer + i, sid[i]).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
            for (u32 i = 0; i < static_cast<u32>(domain.size()); ++i) {
                if (m.write_u16(domain_buffer + i * 2,
                                static_cast<u16>(domain[i])).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
            if (m.write_u16(
                    domain_buffer + static_cast<u32>(domain.size()) * 2,
                    0).is_err() ||
                m.write_u32(sid_size_ptr, required_sid).is_err() ||
                m.write_u32(domain_size_ptr,
                            static_cast<u32>(domain.size())).is_err() ||
                m.write_u32(use_ptr, use).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }

            VITREO_TRACE(
                "[advapi32] LookupAccountNameW account=\"%s\" -> "
                "sid_bytes=%u domain=\"VITREO\" use=%u\n",
                account_result.value().c_str(), required_sid, use);
            m.set_return(1);
        },
        28
    });

    // BOOL GetCurrentHwProfileW(LPHW_PROFILE_INFOW)
    exports.entries.push_back({
        "GetCurrentHwProfileW", 0,
        [](Marshaler& m) {
            constexpr u32 DOCKINFO_UNDOCKED = 0x00000001;
            constexpr usize HW_PROFILE_GUIDLEN = 39;
            constexpr usize MAX_PROFILE_LEN = 80;
            constexpr std::u16string_view profile_guid =
                u"{8F5A2C2E-6A80-4A3B-A39E-56495452554D}";
            constexpr std::u16string_view profile_name =
                u"Vitreo Hardware Profile";
            const GuestAddr info = m.arg_ptr(0);
            if (info == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            constexpr usize structure_bytes =
                sizeof(u32) +
                HW_PROFILE_GUIDLEN * sizeof(u16) +
                MAX_PROFILE_LEN * sizeof(u16);
            for (usize i = 0; i < structure_bytes; ++i) {
                if (m.write_u8(info + i, 0).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
            if (m.write_u32(info, DOCKINFO_UNDOCKED).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            const GuestAddr guid = info + sizeof(u32);
            const GuestAddr name =
                guid + HW_PROFILE_GUIDLEN * sizeof(u16);
            for (usize i = 0; i < profile_guid.size(); ++i) {
                (void)m.write_u16(guid + i * sizeof(u16), profile_guid[i]);
            }
            for (usize i = 0; i < profile_name.size(); ++i) {
                (void)m.write_u16(name + i * sizeof(u16), profile_name[i]);
            }

            VITREO_TRACE(
                "[advapi32] GetCurrentHwProfileW -> "
                "dock=UNDOCKED guid={8F5A2C2E-6A80-4A3B-A39E-56495452554D}\n");
            m.set_return(1);
        },
        4
    });

    // -- SID & privilege thunks (ordinals 8-17) --

    exports.entries.push_back(
        {"GetSidIdentifierAuthority", 8, [](Marshaler& m) {
            GuestAddr sid = m.arg_ptr(0);
            GuestAddr result = SecurityStubs::get_sid_identifier_authority(sid);
            m.set_return(static_cast<u64>(result));
        }, 4});

    exports.entries.push_back(
        {"GetLengthSid", 9, [](Marshaler& m) {
            GuestAddr sid = m.arg_ptr(0);
            u32 len = SecurityStubs::get_length_sid(m, sid);
            m.set_return(static_cast<u64>(len));
        }, 4});

    // DWORD GetSidLengthRequired(UCHAR nSubAuthorityCount)
    // The count is passed in a full argument slot but has UCHAR semantics.
    exports.entries.push_back(
        {"GetSidLengthRequired", 0, [](Marshaler& m) {
            const u8 count = static_cast<u8>(m.arg(0));
            m.set_return(SecurityStubs::get_sid_length_required(count));
        }, 4});

    exports.entries.push_back(
        {"IsValidSid", 10, [](Marshaler& m) {
            GuestAddr sid = m.arg_ptr(0);
            u32 valid = SecurityStubs::is_valid_sid(m, sid);
            m.set_return(static_cast<u64>(valid));
        }, 4});

    exports.entries.push_back(
        {"CopySid", 11, [](Marshaler& m) {
            u32 dest_len = static_cast<u32>(m.arg(0));
            GuestAddr dest = m.arg_ptr(1);
            GuestAddr src = m.arg_ptr(2);
            const u32 error = SecurityStubs::copy_sid(m, dest_len, dest, src);
            // A short buffer is ERROR_INSUFFICIENT_BUFFER (advapi_token_probe
            // copysid_short); an unreadable source or unwritable destination
            // is ERROR_NOACCESS. Success leaves the last error untouched
            // (last_error_on_success_advapi32_probe.expected).
            if (error != error_codes::ERROR_SUCCESS) set_last_error(error);
            m.set_return(error == error_codes::ERROR_SUCCESS ? 1u : 0u);
        }, 12});

    exports.entries.push_back(
        {"GetSidSubAuthority", 12, [](Marshaler& m) {
            GuestAddr sid = m.arg_ptr(0);
            u32 index = static_cast<u32>(m.arg(1));
            GuestAddr result = SecurityStubs::get_sid_sub_authority(sid, index);
            m.set_return(static_cast<u64>(result));
        }, 8});

    exports.entries.push_back(
        {"GetSidSubAuthorityCount", 13, [](Marshaler& m) {
            GuestAddr sid = m.arg_ptr(0);
            GuestAddr result = SecurityStubs::get_sid_sub_authority_count(sid);
            m.set_return(static_cast<u64>(result));
        }, 4});

    exports.entries.push_back(
        {"InitializeSid", 14, [](Marshaler& m) {
            GuestAddr sid = m.arg_ptr(0);
            GuestAddr authority = m.arg_ptr(1);
            u8 count = static_cast<u8>(m.arg(2));
            u32 ok = SecurityStubs::initialize_sid(m, sid, authority, count);
            m.set_return(static_cast<u64>(ok));
        }, 12});

    exports.entries.push_back(
        {"EqualSid", 15, [](Marshaler& m) {
            GuestAddr sid1 = m.arg_ptr(0);
            GuestAddr sid2 = m.arg_ptr(1);
            u32 eq = SecurityStubs::equal_sid(m, sid1, sid2);
            m.set_return(static_cast<u64>(eq));
        }, 8});

    // BOOL LookupPrivilegeName{A,W}(LPCTSTR lpSystemName, PLUID lpLuid,
    //   LPTSTR lpName, LPDWORD cchName) — stdcall 16.
    // tools/oracle_probe/advapi_wksid_probe lpn*: an unknown LUID (or a
    // non-zero HighPart) -> ERROR_NO_SUCH_PRIVILEGE with cchName untouched; a
    // buffer shorter than name + NUL -> ERROR_INSUFFICIENT_BUFFER and cchName
    // = name + NUL; success -> cchName = name length, last error ERROR_SUCCESS.
    auto lookup_privilege_name = [](Marshaler& m, bool wide) {
        constexpr u32 ERROR_NO_SUCH_PRIVILEGE = 1313;
        const GuestAddr luid_ptr = m.arg_ptr(1);
        const GuestAddr name_buf = m.arg_ptr(2);
        const GuestAddr size_ptr = m.arg_ptr(3);
        if (luid_ptr == 0 || size_ptr == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        const u64 luid = m.read_u64(luid_ptr).value_or(0);
        const std::string name = SecurityStubs::privilege_name(luid);
        if (name.empty()) {
            set_last_error(ERROR_NO_SUCH_PRIVILEGE);
            m.set_return(0);
            return;
        }
        const u32 capacity = m.read_u32(size_ptr).value_or(0);
        const u32 needed = static_cast<u32>(name.size() + 1);
        if (name_buf == 0 || capacity < needed) {
            (void)m.write_u32(size_ptr, needed);
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        for (u32 i = 0; i < needed; ++i) {
            const u8 c = i < name.size() ? static_cast<u8>(name[i]) : 0;
            if (wide) (void)m.write_u16(name_buf + i * 2, c);
            else (void)m.write_u8(name_buf + i, c);
        }
        (void)m.write_u32(size_ptr, needed - 1);
        set_last_error(error_codes::ERROR_SUCCESS);
        m.set_return(1);
    };
    exports.entries.push_back({"LookupPrivilegeNameA", 0,
        [lookup_privilege_name](Marshaler& m) { lookup_privilege_name(m, false); }, 16});
    exports.entries.push_back({"LookupPrivilegeNameW", 16,
        [lookup_privilege_name](Marshaler& m) { lookup_privilege_name(m, true); }, 16});

    exports.entries.push_back(
        {"LookupPrivilegeDisplayNameW", 17, [](Marshaler& m) {
            // Args: system(0), name(1), display_buf(2), size_ptr(3)
            (void)m.arg_ptr(0); // system name, ignored
            (void)m.arg_ptr(1); // privilege name, ignored
            GuestAddr display_buf = m.arg_ptr(2);
            GuestAddr size_ptr = m.arg_ptr(3);
            const char* display = "Bypass traverse checking";
            u32 len = 25; // strlen + 1 for null
            if (display_buf == 0) {
                (void)m.write_u32(size_ptr, len);
                m.set_return(0);
                return;
            }
            for (u32 i = 0; i < len; ++i) {
                (void)m.write_u16(display_buf + i * 2,
                                  static_cast<u16>(static_cast<unsigned char>(display[i])));
            }
            (void)m.write_u32(size_ptr, len - 1);
            m.set_return(1); // TRUE
        }, 16});
}

/// Registers LookupPrivilegeValueA.
void add_lookup_privilege_value_a_export(DllExports& exports,
                                         SecurityStubs& stubs) {
    // SystemFunction036 (RtlGenRandom) is registered once, further below.

    // -----------------------------------------------------------------
    // Service Control Manager APIs
    // -----------------------------------------------------------------

    // -----------------------------------------------------------------
    // Security / Token / SID APIs
    // -----------------------------------------------------------------

    // BOOL LookupPrivilegeValueA(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid)
    // 3 args, stdcall_bytes=12
    exports.entries.push_back(
        {"LookupPrivilegeValueA", 32, [&stubs](Marshaler& m) {
            lookup_privilege_value(m, stubs, false);
        }, 12});
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only

/// @file token_exports.cpp
/// @brief advapi32 token exports: GetUserNameW/A, OpenProcessToken,
/// GetTokenInformation and AdjustTokenPrivileges.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "ipc/ipc_gate.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace vitreo::winapi {

/// Registers GetUserNameW/A, OpenProcessToken, GetTokenInformation,
/// AdjustTokenPrivileges.
void add_token_exports(DllExports& exports, SecurityStubs& stubs) {
    // BOOL GetUserNameW(LPWSTR lpBuffer, LPDWORD pcbBuffer)
    exports.entries.push_back({
        "GetUserNameW", 1,
        [&stubs](Marshaler& m) {
            GuestAddr buf = m.arg_ptr(0);
            GuestAddr size_ptr = m.arg_ptr(1);
            u32 size = 0;
            if (size_ptr == 0 || m.read_u32(size_ptr).is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            size = m.read_u32(size_ptr).value();

            std::wstring name;
            u32 size_inout = size;
            bool ok = stubs.get_user_name_w(name, size_inout);
            (void)m.write_u32(size_ptr, size_inout);
            if (!ok || buf == 0) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }

            // Write UTF-16LE (host wchar_t is UTF-32 on macOS; but our name is ASCII)
            for (u32 i = 0; i < static_cast<u32>(name.size()); ++i) {
                (void)m.write_u16(buf + i * 2, static_cast<u16>(name[i]));
            }
            (void)m.write_u16(buf + static_cast<u32>(name.size()) * 2, 0);
            // Measured on Windows 11: success leaves the last error untouched
            // (the first call in a process leaks a 0 from a lazy load; the
            // repeat call keeps the sentinel:
            // last_error_on_success_advapi32_probe.expected).
            m.set_return(1);
        },
        8
    });

    // BOOL GetUserNameA(LPSTR lpBuffer, LPDWORD pcbBuffer)
    exports.entries.push_back({
        "GetUserNameA", 2,
        [&stubs](Marshaler& m) {
            GuestAddr buf = m.arg_ptr(0);
            GuestAddr size_ptr = m.arg_ptr(1);
            auto size_r = m.read_u32(size_ptr);
            if (size_ptr == 0 || size_r.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            u32 size_inout = size_r.value();

            std::string name;
            bool ok = stubs.get_user_name_a(name, size_inout);
            (void)m.write_u32(size_ptr, size_inout);
            if (!ok || buf == 0) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            for (u32 i = 0; i < static_cast<u32>(name.size()); ++i) {
                (void)m.write_u8(buf + i, static_cast<u8>(name[i]));
            }
            (void)m.write_u8(buf + static_cast<u32>(name.size()), 0);
            m.set_return(1);
        },
        8
    });

    // BOOL OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle)
    exports.entries.push_back({
        "OpenProcessToken", 3,
        [&stubs](Marshaler& m) {
            Handle process = static_cast<Handle>(m.arg(0));
            u32 access = static_cast<u32>(m.arg(1));
            GuestAddr out = m.arg_ptr(2);
            if (out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            Handle token{};
            u32 error = error_codes::ERROR_SUCCESS;
            // A guest 0xFFFFFFFF includes rights no token has (SYNCHRONIZE,
            // 0x200): ERROR_ACCESS_DENIED like probe opt_access 0x10000200.
            // It must not reach open_process_token's in-process
            // HandleAccess::All sentinel.
            bool ok = false;
            if (access == HandleAccess::All) {
                error = error_codes::ERROR_ACCESS_DENIED;
            } else {
                ok = stubs.open_process_token(process, access, token, error);
            }
            if (ok) {
                (void)m.write_ptr(out, token); // PHANDLE
                // Success must not clear LastError. A caller that first opens
                // the thread token (ERROR_NO_TOKEN), then the process token,
                // branches on the stale code: ERROR_NO_TOKEN continues into
                // GetTokenInformation, while ERROR_SUCCESS is treated as
                // failure and returns FALSE.
                VITREO_TRACE(
                    "[advapi32] OpenProcessToken(proc=0x%x access=0x%x) "
                    "-> 0x%x elevated=%u\n",
                    static_cast<unsigned>(process), access,
                    static_cast<unsigned>(token),
                    stubs.is_elevated() ? 1u : 0u);
                m.set_return(1);
            } else {
                // x86 (WOW64) stores NULL on failure; x64 leaves the variable
                // alone (advapi_token2_probe opt_access, both .expected).
                if (!m.cpu_is_64bit()) (void)m.write_ptr(out, 0);
                set_last_error(error);
                VITREO_TRACE(
                    "[advapi32] OpenProcessToken(proc=0x%x) -> FALSE\n",
                    static_cast<unsigned>(process));
                m.set_return(0);
            }
        },
        12
    });

    // BOOL GetTokenInformation(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD)
    exports.entries.push_back({
        "GetTokenInformation", 4,
        [&stubs](Marshaler& m) {
            Handle token = static_cast<Handle>(m.arg(0));
            u32 info_class = static_cast<u32>(m.arg(1));
            GuestAddr out_buf = m.arg_ptr(2);
            u32 out_len = static_cast<u32>(m.arg(3));
            GuestAddr ret_len_ptr = m.arg_ptr(4);

            const bool is64 = m.cpu_is_64bit();
            if (!stubs.is_token_handle(token)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            // Learn: the handle needs TOKEN_QUERY (TokenSource:
            // TOKEN_QUERY_SOURCE); advapi_token_probe gti_noquery -> 5.
            constexpr u32 TOKEN_QUERY = 0x0008, TOKEN_QUERY_SOURCE = 0x0010;
            if (stubs.is_real_token_handle(token) &&
                !stubs.token_has_access(
                    token, info_class == 7 ? TOKEN_QUERY_SOURCE : TOKEN_QUERY)) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                m.set_return(0);
                return;
            }
            std::vector<u8> data;
            u32 needed = 0;
            bool ok = stubs.get_token_information(token, info_class, data, needed, is64);
            if (ret_len_ptr != 0) {
                (void)m.write_u32(ret_len_ptr, needed);
            }
            if (!ok) {
                VITREO_TRACE(
                    "[advapi32] GetTokenInformation(h=0x%x class=%u) "
                    "-> FALSE needed=%u elevated=%u\n",
                    static_cast<unsigned>(token), info_class, needed,
                    stubs.is_elevated() ? 1u : 0u);
                set_last_error(stubs.is_token_handle(token)
                                   ? error_codes::ERROR_INVALID_PARAMETER
                                   : error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            // TokenLinkedToken / TokenElevation require the exact structure
            // size: any other length is ERROR_BAD_LENGTH with ReturnLength
            // set (probe elev_8, gti_null class 0x13/0x14).
            if ((info_class == 19 || info_class == 20) &&
                (out_buf == 0 || out_len != needed)) {
                set_last_error(24u /*ERROR_BAD_LENGTH*/);
                m.set_return(0);
                return;
            }
            if (info_class == 19) {
                // Only a split (elevated) token has a linked token; an
                // unsplit one gives ERROR_NO_SUCH_LOGON_SESSION (probe
                // linked_exact).
                if (!stubs.token_is_elevated(token)) {
                    set_last_error(1312u /*ERROR_NO_SUCH_LOGON_SESSION*/);
                    m.set_return(0);
                    return;
                }
                Handle linked = 0;
                u32 error = 0;
                if (!stubs.open_linked_token(token, linked, error)) {
                    set_last_error(error);
                    m.set_return(0);
                    return;
                }
                (void)m.write_ptr(out_buf, linked);
                m.set_return(1);
                return;
            }
            if (out_buf == 0 || out_len < needed) {
                VITREO_TRACE(
                    "[advapi32] GetTokenInformation(h=0x%x class=%u) "
                    "-> INSUFFICIENT_BUFFER need=%u have=%u\n",
                    static_cast<unsigned>(token), info_class, needed,
                    out_len);
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            if (!data.empty()) {
                auto wr = m.memory().write_bytes(out_buf,
                                                 reinterpret_cast<const Byte*>(data.data()),
                                                 data.size());
                if (wr.is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
            }
            // Fix up SID pointers: stored as relative offsets, convert to
            // absolute at guest pointer width.
            if (info_class == 1 || info_class == 4 || info_class == 5 ||
                info_class == 25) {
                auto rel = m.read_u32(out_buf);
                if (rel.is_ok() && rel.value() != 0) {
                    if (is64) {
                        (void)m.write_u64(out_buf, out_buf + rel.value());
                    } else {
                        (void)m.write_u32(out_buf, out_buf + rel.value());
                    }
                }
            } else if (info_class == 2 || info_class == 11 ||
                       info_class == 28) {
                const u32 count = m.read_u32(out_buf).value_or(0);
                const u32 entries_offset = is64 ? 8u : 4u;
                const u32 entry_size = is64 ? 16u : 8u;
                for (u32 index = 0; index < count; ++index) {
                    const GuestAddr pointer_field =
                        out_buf + entries_offset + index * entry_size;
                    const u32 relative =
                        m.read_u32(pointer_field).value_or(0);
                    if (relative == 0) continue;
                    if (is64) {
                        (void)m.write_u64(pointer_field,
                                          out_buf + relative);
                    } else {
                        (void)m.write_u32(pointer_field,
                                          out_buf + relative);
                    }
                }
            } else if (info_class == 13) {
                // TOKEN_GROUPS_AND_PRIVILEGES: three array pointers, each
                // array of SID_AND_ATTRIBUTES holding relative SID offsets.
                const u32 ptr = is64 ? 8u : 4u;
                const u32 entry_size = is64 ? 16u : 8u;
                auto rebase = [&](GuestAddr field) {
                    const u32 rel = m.read_u32(field).value_or(0);
                    if (rel != 0) (void)m.write_ptr(field, out_buf + rel);
                    return rel;
                };
                const u32 counts[2] = {m.read_u32(out_buf).value_or(0),
                                       m.read_u32(out_buf + 8 + ptr).value_or(0)};
                const GuestAddr fields[2] = {out_buf + 8, out_buf + 16 + ptr};
                for (int list = 0; list < 2; ++list) {
                    const u32 array = rebase(fields[list]);
                    for (u32 i = 0; array != 0 && i < counts[list]; ++i) {
                        (void)rebase(out_buf + array + i * entry_size);
                    }
                }
                (void)rebase(out_buf + 24 + 2 * ptr);
            } else if (info_class == 6) {
                const u32 relative = m.read_u32(out_buf).value_or(0);
                if (relative != 0) {
                    if (is64) {
                        (void)m.write_u64(out_buf, out_buf + relative);
                    } else {
                        (void)m.write_u32(
                            out_buf, static_cast<u32>(out_buf + relative));
                    }
                }
            } else if (info_class == 39) {
                // TokenSecurityAttributes: rewrite buffer-relative offsets
                // in TOKEN_SECURITY_ATTRIBUTES_INFORMATION / V1 attrs /
                // UNICODE_STRING Buffer fields to absolute guest pointers.
                const u32 pointer_size = is64 ? 8u : 4u;
                const u32 unicode_string_size = is64 ? 16u : 8u;
                const u32 buffer_field = is64 ? 8u : 4u;
                const u32 attr_size =
                    unicode_string_size + 2u + 2u + 4u + 4u +
                    (pointer_size == 8u ? 4u : 0u) + pointer_size;
                const u32 count = m.read_u32(out_buf + 4).value_or(0);
                const u32 attrs_rel = m.read_u32(out_buf + 8).value_or(0);
                if (count != 0 && attrs_rel != 0) {
                    if (is64) {
                        (void)m.write_u64(out_buf + 8, out_buf + attrs_rel);
                    } else {
                        (void)m.write_u32(out_buf + 8, out_buf + attrs_rel);
                    }
                    for (u32 index = 0; index < count; ++index) {
                        const GuestAddr attr =
                            out_buf + attrs_rel + index * attr_size;
                        const u32 name_rel =
                            m.read_u32(attr + buffer_field).value_or(0);
                        if (name_rel != 0) {
                            if (is64) {
                                (void)m.write_u64(attr + buffer_field,
                                                  out_buf + name_rel);
                            } else {
                                (void)m.write_u32(attr + buffer_field,
                                                  out_buf + name_rel);
                            }
                        }
                        const GuestAddr values_ptr_field =
                            attr + unicode_string_size + 12 +
                            (pointer_size == 8u ? 4u : 0u);
                        const u32 values_rel =
                            m.read_u32(values_ptr_field).value_or(0);
                        if (values_rel == 0) continue;
                        if (is64) {
                            (void)m.write_u64(values_ptr_field,
                                              out_buf + values_rel);
                        } else {
                            (void)m.write_u32(values_ptr_field,
                                              out_buf + values_rel);
                        }
                        const GuestAddr value_ustr = out_buf + values_rel;
                        const u32 value_rel =
                            m.read_u32(value_ustr + buffer_field)
                                .value_or(0);
                        if (value_rel != 0) {
                            if (is64) {
                                (void)m.write_u64(
                                    value_ustr + buffer_field,
                                    out_buf + value_rel);
                            } else {
                                (void)m.write_u32(
                                    value_ustr + buffer_field,
                                    out_buf + value_rel);
                            }
                        }
                    }
                }
            }
            if (info_class == 2 && !data.empty()) {
                u32 group_count = 0;
                std::memcpy(&group_count, data.data(), sizeof(group_count));
                VITREO_TRACE(
                    "[advapi32] GetTokenInformation(h=0x%x TokenGroups) "
                    "-> TRUE groups=%u needed=%u elevated=%u\n",
                    static_cast<unsigned>(token), group_count, needed,
                    stubs.is_elevated() ? 1u : 0u);
            } else {
                VITREO_TRACE(
                    "[advapi32] GetTokenInformation(h=0x%x class=%u) "
                    "-> TRUE needed=%u elevated=%u\n",
                    static_cast<unsigned>(token), info_class, needed,
                    stubs.is_elevated() ? 1u : 0u);
            }
            m.set_return(1);
        },
        20
    });

    // BOOL AdjustTokenPrivileges(...)
    exports.entries.push_back({
        "AdjustTokenPrivileges", 5,
        [&stubs](Marshaler& m) {
            const Handle token = static_cast<Handle>(m.arg(0));
            const bool disable_all = m.arg(1) != 0;
            const GuestAddr new_state = m.arg_ptr(2);
            const GuestAddr previous_state = m.arg_ptr(4);
            const GuestAddr return_length = m.arg_ptr(5);
            if (!stubs.is_token_handle(token)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            // Learn: TOKEN_ADJUST_PRIVILEGES, plus TOKEN_QUERY when
            // PreviousState is requested (advapi_token_probe dup_adjust).
            constexpr u32 TOKEN_QUERY = 0x0008, TOKEN_ADJUST_PRIVILEGES = 0x0020;
            if (!stubs.is_real_token_handle(token) ||
                !stubs.token_has_access(
                    token, TOKEN_ADJUST_PRIVILEGES |
                               (previous_state != 0 ? TOKEN_QUERY : 0u))) {
                set_last_error(error_codes::ERROR_ACCESS_DENIED);
                m.set_return(0);
                return;
            }

            std::vector<SecurityStubs::TokenPrivilege> requested;
            if (!disable_all) {
                if (new_state == 0) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                const u32 count = m.read_u32(new_state).value_or(0xFFFFFFFFu);
                if (count > 4096) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                requested.reserve(count);
                for (u32 index = 0; index < count; ++index) {
                    const GuestAddr entry = new_state + 4u + index * 12u;
                    const auto luid = m.read_u64(entry);
                    const auto attributes = m.read_u32(entry + 8u);
                    if (luid.is_err() || attributes.is_err()) {
                        set_last_error(error_codes::ERROR_NOACCESS);
                        m.set_return(0);
                        return;
                    }
                    requested.push_back(
                        {luid.value(), attributes.value()});
                }
            }

            std::vector<SecurityStubs::TokenPrivilege> previous;
            bool all_assigned = true;
            if (!stubs.describe_privilege_adjustment(
                    token, disable_all, requested, previous, all_assigned)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }

            const u32 needed =
                4u + static_cast<u32>(previous.size()) * 12u;
            if (return_length != 0) {
                (void)m.write_u32(return_length, needed);
            }
            if (previous_state != 0) {
                const u32 buffer_length = static_cast<u32>(m.arg(3));
                if (buffer_length < needed) {
                    set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                    m.set_return(0);
                    return;
                }
                if (m.write_u32(previous_state,
                                static_cast<u32>(previous.size())).is_err()) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                for (u32 index = 0; index < previous.size(); ++index) {
                    const GuestAddr entry = previous_state + 4u + index * 12u;
                    if (m.write_u64(entry, previous[index].luid).is_err() ||
                        m.write_u32(entry + 8u, previous[index].attributes)
                            .is_err()) {
                        set_last_error(error_codes::ERROR_NOACCESS);
                        m.set_return(0);
                        return;
                    }
                }
            }

            if (!stubs.adjust_token_privileges(
                    token, disable_all, requested, all_assigned)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            ipc::ipc_log(
                "AdjustTokenPrivileges h=0x%x disable_all=%u count=%zu "
                "-> TRUE all_assigned=%u",
                static_cast<unsigned>(token), disable_all ? 1u : 0u,
                requested.size(), all_assigned ? 1u : 0u);
            set_last_error(all_assigned
                ? error_codes::ERROR_SUCCESS
                : error_codes::ERROR_NOT_ALL_ASSIGNED);
            m.set_return(1);
        },
        24
    });
}

} // namespace vitreo::winapi

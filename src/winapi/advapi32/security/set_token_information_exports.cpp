// SPDX-License-Identifier: LGPL-2.1-only

/// @file set_token_information_exports.cpp
/// @brief advapi32!SetTokenInformation.
///
/// Registration order is ABI-visible: thunk addresses follow entries[] order
/// and the by-name table keeps the last of any duplicate names. The call order
/// lives in security_registration.cpp; this file only supplies registrations.

#include "ipc/ipc_gate.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/advapi32/security/security_exports.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <utility>
#include <vector>

namespace vitreo::winapi {

/// Registers SetTokenInformation.
void add_set_token_information_exports(DllExports& exports,
                                       SecurityStubs& stubs) {
    // BOOL SetTokenInformation(HANDLE,TOKEN_INFORMATION_CLASS,LPVOID,DWORD)
    // 4 args, 16 bytes
    exports.entries.push_back({"SetTokenInformation", 0,
        [&stubs](Marshaler& m) {
            const Handle token = static_cast<Handle>(m.arg(0));
            const u32 info_class = static_cast<u32>(m.arg(1));
            const GuestAddr info = m.arg_ptr(2);
            const u32 info_length = static_cast<u32>(m.arg(3));
            if (!stubs.is_token_handle(token)) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            if (info == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }

            if (info_class == 6) { // TokenDefaultDacl
                const u32 pointer_size = m.cpu_is_64bit() ? 8u : 4u;
                if (info_length < pointer_size) {
                    set_last_error(error_codes::ERROR_BAD_LENGTH);
                    m.set_return(0);
                    return;
                }
                const GuestAddr acl = m.cpu_is_64bit()
                    ? m.read_u64(info).value_or(0)
                    : m.read_u32(info).value_or(0);
                if (acl == 0) {
                    set_last_error(error_codes::ERROR_INVALID_ACL);
                    m.set_return(0);
                    return;
                }
                const u16 acl_size = m.read_u16(acl + 2).value_or(0);
                if (acl_size < 8 || acl_size > 65535) {
                    set_last_error(error_codes::ERROR_INVALID_ACL);
                    m.set_return(0);
                    return;
                }
                std::vector<u8> bytes(acl_size);
                if (m.memory().read_bytes(
                        acl, bytes.data(), bytes.size()).is_err() ||
                    !stubs.set_token_default_dacl(
                        token, std::move(bytes))) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                ipc::ipc_log(
                    "SetTokenInformation TokenDefaultDacl h=0x%x "
                    "acl_size=%u -> TRUE",
                    static_cast<unsigned>(token), acl_size);
                m.set_return(1);
                return;
            }

            if (info_class == 25) { // TokenIntegrityLevel
                const u32 structure_size = m.cpu_is_64bit() ? 16u : 8u;
                if (info_length < structure_size) {
                    set_last_error(error_codes::ERROR_BAD_LENGTH);
                    m.set_return(0);
                    return;
                }
                const GuestAddr sid = m.cpu_is_64bit()
                    ? m.read_u64(info).value_or(0)
                    : m.read_u32(info).value_or(0);
                if (sid == 0 ||
                    SecurityStubs::is_valid_sid(m, sid) == 0) {
                    set_last_error(error_codes::ERROR_INVALID_SID);
                    m.set_return(0);
                    return;
                }
                const u8 subauthority_count =
                    m.read_u8(sid + 1).value_or(0);
                if (subauthority_count == 0) {
                    set_last_error(error_codes::ERROR_INVALID_SID);
                    m.set_return(0);
                    return;
                }
                const GuestAddr rid_address =
                    sid + 8u +
                    static_cast<u64>(subauthority_count - 1u) * 4u;
                const auto rid = m.read_u32(rid_address);
                if (rid.is_err() ||
                    !stubs.set_token_integrity_level(
                        token, rid.value())) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                ipc::ipc_log(
                    "SetTokenInformation TokenIntegrityLevel h=0x%x "
                    "rid=0x%x -> TRUE",
                    static_cast<unsigned>(token), rid.value());
                set_last_error(error_codes::ERROR_SUCCESS);
                m.set_return(1);
                return;
            }

            if (info_class == 39) { // TokenSecurityAttributes
                // TOKEN_SECURITY_ATTRIBUTES_AND_OPERATION_INFORMATION.
                // Callers add one string-typed V1 attribute with OPERATION_ADD.
                constexpr u32 OPERATION_REPLACE_ALL = 1;
                constexpr u32 OPERATION_ADD = 2;
                constexpr u32 OPERATION_REPLACE = 4;
                constexpr u16 kTypeString = 3;
                const bool is64 = m.cpu_is_64bit();
                const u32 pointer_size = is64 ? 8u : 4u;
                if (info_length < 2u * pointer_size) {
                    set_last_error(error_codes::ERROR_BAD_LENGTH);
                    m.set_return(0);
                    return;
                }
                const GuestAddr attrs_info = is64
                    ? m.read_u64(info).value_or(0)
                    : m.read_u32(info).value_or(0);
                const GuestAddr operations = is64
                    ? m.read_u64(info + pointer_size).value_or(0)
                    : m.read_u32(info + pointer_size).value_or(0);
                if (attrs_info == 0 || operations == 0) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                const u16 version = m.read_u16(attrs_info).value_or(0);
                const u32 attr_count =
                    m.read_u32(attrs_info + 4).value_or(0);
                const GuestAddr attr_array = is64
                    ? m.read_u64(attrs_info + 8).value_or(0)
                    : m.read_u32(attrs_info + 8).value_or(0);
                if (version != 1 || attr_count == 0 || attr_array == 0 ||
                    attr_count > 64) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0);
                    return;
                }
                const u32 unicode_string_size = is64 ? 16u : 8u;
                const u32 buffer_field = is64 ? 8u : 4u;
                const u32 attr_size =
                    unicode_string_size + 2u + 2u + 4u + 4u +
                    (pointer_size == 8u ? 4u : 0u) + pointer_size;
                std::vector<SecurityStubs::SecurityAttribute> parsed;
                parsed.reserve(attr_count);
                bool replace_all = false;
                for (u32 index = 0; index < attr_count; ++index) {
                    const u32 op =
                        m.read_u32(operations + index * 4u).value_or(0);
                    if (op == OPERATION_REPLACE_ALL) {
                        replace_all = true;
                    } else if (op != OPERATION_ADD &&
                               op != OPERATION_REPLACE) {
                        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                        m.set_return(0);
                        return;
                    }
                    const GuestAddr attr = attr_array + index * attr_size;
                    const u16 name_bytes = m.read_u16(attr).value_or(0);
                    const GuestAddr name_buf = is64
                        ? m.read_u64(attr + buffer_field).value_or(0)
                        : m.read_u32(attr + buffer_field).value_or(0);
                    const u16 value_type = m.read_u16(
                        attr + unicode_string_size).value_or(0);
                    const u32 flags = m.read_u32(
                        attr + unicode_string_size + 4).value_or(0);
                    const u32 value_count = m.read_u32(
                        attr + unicode_string_size + 8).value_or(0);
                    const GuestAddr values_ptr_field =
                        attr + unicode_string_size + 12 +
                        (pointer_size == 8u ? 4u : 0u);
                    const GuestAddr values = is64
                        ? m.read_u64(values_ptr_field).value_or(0)
                        : m.read_u32(values_ptr_field).value_or(0);
                    if (name_buf == 0 || (name_bytes & 1u) != 0 ||
                        value_type != kTypeString || value_count != 1 ||
                        values == 0) {
                        set_last_error(
                            error_codes::ERROR_INVALID_PARAMETER);
                        m.set_return(0);
                        return;
                    }
                    const u16 value_bytes =
                        m.read_u16(values).value_or(0);
                    const GuestAddr value_buf = is64
                        ? m.read_u64(values + buffer_field).value_or(0)
                        : m.read_u32(values + buffer_field).value_or(0);
                    if (value_buf == 0 || (value_bytes & 1u) != 0) {
                        set_last_error(
                            error_codes::ERROR_INVALID_PARAMETER);
                        m.set_return(0);
                        return;
                    }
                    SecurityStubs::SecurityAttribute item;
                    item.flags = flags;
                    item.value_type = value_type;
                    item.name.assign(name_bytes / 2u, L'\0');
                    for (u32 i = 0; i < name_bytes / 2u; ++i) {
                        item.name[i] = static_cast<wchar_t>(
                            m.read_u16(name_buf + i * 2u).value_or(0));
                    }
                    item.value.assign(value_bytes / 2u, L'\0');
                    for (u32 i = 0; i < value_bytes / 2u; ++i) {
                        item.value[i] = static_cast<wchar_t>(
                            m.read_u16(value_buf + i * 2u).value_or(0));
                    }
                    parsed.push_back(std::move(item));
                }
                if (!stubs.set_token_security_attributes(
                        token, std::move(parsed), replace_all)) {
                    set_last_error(error_codes::ERROR_NOACCESS);
                    m.set_return(0);
                    return;
                }
                ipc::ipc_log(
                    "SetTokenInformation TokenSecurityAttributes h=0x%x "
                    "count=%u replace_all=%d -> TRUE",
                    static_cast<unsigned>(token), attr_count,
                    replace_all ? 1 : 0);
                set_last_error(error_codes::ERROR_SUCCESS);
                m.set_return(1);
                return;
            }

            // TokenVirtualizationAllowed (23) / TokenVirtualizationEnabled (24).
            // Capsule policy has no file virtualization; GetTokenInformation
            // always reports 0. Accept disabling (0); refuse enabling.
            if (info_class == 23 || info_class == 24) {
                if (info_length < 4) {
                    set_last_error(error_codes::ERROR_BAD_LENGTH);
                    m.set_return(0);
                    return;
                }
                const u32 value = m.read_u32(info).value_or(1);
                if (value != 0) {
                    set_last_error(error_codes::ERROR_NOT_SUPPORTED);
                    m.set_return(0);
                    return;
                }
                ipc::ipc_log(
                    "SetTokenInformation TokenVirtualization%s h=0x%x "
                    "-> TRUE (disabled)",
                    info_class == 23 ? "Allowed" : "Enabled",
                    static_cast<unsigned>(token));
                m.set_return(1);
                return;
            }

            ipc::ipc_log(
                "SetTokenInformation h=0x%x class=%u len=%u -> "
                "ERROR_NOT_SUPPORTED",
                static_cast<unsigned>(token), info_class, info_length);
            set_last_error(error_codes::ERROR_NOT_SUPPORTED);
            m.set_return(0);
        }, 16});
}

} // namespace vitreo::winapi

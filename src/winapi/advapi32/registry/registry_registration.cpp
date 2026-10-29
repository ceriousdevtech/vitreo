// SPDX-License-Identifier: LGPL-2.1-only

/// @file registry_registration.cpp
/// @brief Export table for advapi32's registry surface.
///
/// Composition only: maps export names to RegistryManager handlers and records
/// each one's stdcall byte count. Every registry API exists in an A and a W
/// form, and the A entries widen their arguments here before calling the same
/// implementation, which is why this table is not purely forwarding.
///
/// Split out of registry.cpp, which was 2,426 lines and half this table.
///
/// The export inventory this produces is verified byte-for-byte by
/// tools/architecture/check_export_inventory.sh.
///
/// VITREO_LARGE_FILE_OK: declarative ABI table — one entry per export, each
/// naming a handler and its stdcall byte count. Plan section 4.1 exempts these
/// because length here tracks how many registry exports advapi32 implements,
/// not how much logic one file carries.

#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/advapi32/registry/registry_internal.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/core/marshaler.hpp"
#include <algorithm>
#include <atomic>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>
#include <variant>

namespace vitreo::winapi {

namespace {

/// RegEnumValueW / RegEnumValueA. Every rule below is measured on Windows 11,
/// identical on x86 and x64 (tools/oracle_probe/s4_registry_probe.expected).
///  - lpValueName or lpcchValueName NULL, or lpData without lpcbData:
///    ERROR_INVALID_PARAMETER, nothing written (enumw_name_null,
///    enumw_data_noptr_size).
///  - unknown key / past the end: ERROR_INVALID_HANDLE / ERROR_NO_MORE_ITEMS;
///    the W form still stores REG_NONE in *lpType, the A form leaves it
///    (enumw_bad_key, enumw_past_end, enuma_all_5).
///  - name buffer too small (it needs room for the NUL): ERROR_MORE_DATA,
///    *lpcchValueName unchanged, *lpType and the required *lpcbData stored,
///    no data copied. W writes no name; A copies cch-1 characters plus a NUL
///    (enumw_name_exact, enumw_both_small, enuma_name_exact).
///  - data buffer too small: ERROR_MORE_DATA, required size in *lpcbData,
///    type stored, data untouched. W still returns the name and its length;
///    A returns neither (enumw_data_small, enuma_data_small).
void reg_enum_value(RegistryThunks& thunks, Marshaler& m, bool wide) {
    const u64 hkey = m.arg(0);
    const u32 index = m.arg32(1);
    const GuestAddr name_ptr = m.arg_ptr(2);
    const GuestAddr namelen_ptr = m.arg_ptr(3);
    const GuestAddr type_ptr = m.arg_ptr(5);
    const GuestAddr data_ptr = m.arg_ptr(6);
    const GuestAddr datalen_ptr = m.arg_ptr(7);
    if (name_ptr == 0 || namelen_ptr == 0 || (data_ptr != 0 && datalen_ptr == 0)) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    std::wstring name;
    capsule::RegValue value;
    const u32 err = thunks.reg_enum_value_entry(hkey, index, name, value);
    if (err != ERROR_SUCCESS) {
        if (wide && type_ptr != 0) (void)m.write_u32(type_ptr, 0);
        m.set_return(err);
        return;
    }
    const u32 type = RegistryThunks::reg_type_to_win32(value);
    const u32 required = wide ? RegistryThunks::serialise_value(value, nullptr, 0)
                              : RegistryThunks::serialise_value_ansi(value, nullptr, 0);
    const u32 cch = m.read_u32(namelen_ptr).value_or(0);
    const u32 name_len = static_cast<u32>(name.size());
    if (type_ptr != 0) (void)m.write_u32(type_ptr, type);
    if (cch <= name_len) {
        if (!wide && cch != 0) {
            for (u32 i = 0; i + 1 < cch; ++i) {
                const wchar_t ch = name[i];
                (void)m.write_u8(name_ptr + i, ch <= 0xff ? static_cast<u8>(ch) : '?');
            }
            (void)m.write_u8(name_ptr + cch - 1, 0);
        }
        if (datalen_ptr != 0) (void)m.write_u32(datalen_ptr, required);
        m.set_return(ERROR_MORE_DATA);
        return;
    }
    auto write_name = [&]() {
        for (u32 i = 0; i < name_len; ++i) {
            const wchar_t ch = name[i];
            if (wide) {
                (void)m.write_u16(name_ptr + i * 2, static_cast<u16>(ch));
            } else {
                (void)m.write_u8(name_ptr + i, ch <= 0xff ? static_cast<u8>(ch) : '?');
            }
        }
        if (wide) {
            (void)m.write_u16(name_ptr + name_len * 2, 0);
        } else {
            (void)m.write_u8(name_ptr + name_len, 0);
        }
        (void)m.write_u32(namelen_ptr, name_len);
    };
    if (data_ptr != 0) {
        const u32 avail = m.read_u32(datalen_ptr).value_or(0);
        if (avail < required) {
            if (wide) write_name();
            (void)m.write_u32(datalen_ptr, required);
            m.set_return(ERROR_MORE_DATA);
            return;
        }
        std::vector<u8> bytes(required);
        if (required != 0) {
            (void)(wide ? RegistryThunks::serialise_value(value, bytes.data(), required)
                        : RegistryThunks::serialise_value_ansi(value, bytes.data(), required));
            (void)m.memory().write_bytes(data_ptr,
                reinterpret_cast<const Byte*>(bytes.data()), required);
        }
    }
    write_name();
    if (datalen_ptr != 0) (void)m.write_u32(datalen_ptr, required);
    m.set_return(ERROR_SUCCESS);
}

/// RegQueryInfoKeyW / RegQueryInfoKeyA (s4_registry_probe qikw / qika).
/// Keys carry no class string: *lpcchClass becomes 0 and a non-empty class
/// buffer gets its NUL; lpClass without lpcchClass is ERROR_INVALID_PARAMETER;
/// an unknown key is ERROR_INVALID_HANDLE with *lpcSubKeys zeroed.
void reg_query_info_key(RegistryThunks& thunks, Marshaler& m, bool wide) {
    const u64 hkey = m.arg(0);
    const GuestAddr class_ptr = m.arg_ptr(1);
    const GuestAddr class_len_ptr = m.arg_ptr(2);
    GuestAddr outs[8];
    for (usize i = 0; i < 8; ++i) outs[i] = m.arg_ptr(4 + i);
    // outs: 0 lpcSubKeys, 1 lpcbMaxSubKeyLen, 2 lpcbMaxClassLen, 3 lpcValues,
    //       4 lpcbMaxValueNameLen, 5 lpcbMaxValueLen,
    //       6 lpcbSecurityDescriptor, 7 lpftLastWriteTime
    if (class_ptr != 0 && class_len_ptr == 0) {
        m.set_return(ERROR_INVALID_PARAMETER);
        return;
    }
    u32 num_sub_keys = 0, max_subkey_len = 0, num_values = 0;
    u32 max_value_len = 0, max_data_len = 0;
    const u32 err = thunks.reg_query_info_key_a(
        hkey, &num_sub_keys, &max_subkey_len, &num_values, &max_value_len,
        &max_data_len);
    if (err != ERROR_SUCCESS) {
        if (outs[0] != 0) (void)m.write_u32(outs[0], 0);
        m.set_return(err);
        return;
    }
    if (class_len_ptr != 0) {
        const u32 cch = m.read_u32(class_len_ptr).value_or(0);
        if (class_ptr != 0 && cch != 0) {
            if (wide) {
                (void)m.write_u16(class_ptr, 0);
            } else {
                (void)m.write_u8(class_ptr, 0);
            }
        }
        (void)m.write_u32(class_len_ptr, 0);
    }
    const u32 values[6] = {num_sub_keys, max_subkey_len, 0u, num_values,
                           max_value_len, max_data_len};
    for (usize i = 0; i < 6; ++i) {
        if (outs[i] != 0) (void)m.write_u32(outs[i], values[i]);
    }
    if (outs[6] != 0 || outs[7] != 0) {
        // Windows reports the key's security-descriptor size and last-write
        // time; the capsule registry models neither yet.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED advapi32!RegQueryInfoKey: "
                "lpcbSecurityDescriptor / lpftLastWriteTime (reported as 0)\n");
        }
        if (outs[6] != 0) (void)m.write_u32(outs[6], 0);
        if (outs[7] != 0) {
            (void)m.write_u32(outs[7], 0);
            (void)m.write_u32(outs[7] + 4, 0);
        }
    }
    m.set_return(ERROR_SUCCESS);
}

} // namespace


// =============================================================================
// DLL Export Factory
// =============================================================================

DllExports create_advapi32_registry_thunks(RegistryThunks& thunks) {
    DllExports exports;

    auto reg_connect_registry = [&thunks](Marshaler& m, bool ansi) {
        const GuestAddr machine = m.arg_ptr(0);
        const u64 hkey = m.arg(1);
        const GuestAddr result_ptr = m.arg_ptr(2);
        if (result_ptr == 0) {
            m.set_return(ERROR_INVALID_PARAMETER);
            return;
        }
        if (machine != 0) {
            VITREO_TRACE(
                "[registry] RegConnectRegistry%c(remote) -> "
                "ERROR_CALL_NOT_IMPLEMENTED\n",
                ansi ? 'A' : 'W');
            m.set_return(ERROR_CALL_NOT_IMPLEMENTED);
            return;
        }
        u64 result_handle = 0;
        const u32 error =
            thunks.reg_connect_registry(hkey, &result_handle);
        if (error == ERROR_SUCCESS) {
            write_guest_hkey(m, result_ptr, result_handle);
        }
        VITREO_TRACE(
            "[registry] RegConnectRegistry%c(local, 0x%llx) -> %u "
            "handle=0x%llx\n",
            ansi ? 'A' : 'W', static_cast<unsigned long long>(hkey), error,
            static_cast<unsigned long long>(result_handle));
        m.set_return(error);
    };
    exports.entries.push_back(ExportEntry{
        "RegConnectRegistryA", 0,
        [reg_connect_registry](Marshaler& m) {
            reg_connect_registry(m, true);
        },
        12});
    exports.entries.push_back(ExportEntry{
        "RegConnectRegistryW", 0,
        [reg_connect_registry](Marshaler& m) {
            reg_connect_registry(m, false);
        },
        12});

    // -------------------------------------------------------------------------
    // RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions,
    //               REGSAM samDesired, PHKEY phkResult)
    //   5 args, stdcall_bytes=20
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegOpenKeyExW", 1,
        [&thunks](Marshaler& m) {
            RegistryWindowTimer window_timer;
            u64 hkey     = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            u32 options  = static_cast<u32>(m.arg(2));
            u32 sam      = static_cast<u32>(m.arg(3));
            GuestAddr result_ptr = m.arg_ptr(4);

            std::wstring subkey = read_guest_wstring(m.memory(), subkey_ptr);
            u64 result_handle = 0;
            u32 err = thunks.reg_open_key_ex_w(hkey, subkey, options, sam,
                                               &result_handle);
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            VITREO_TRACE( "[registry] RegOpenKeyExW(0x%llx, \"%ls\") -> %u\n",
                         hkey, subkey.c_str(), err);
            log_registry_open("RegOpenKeyExW", hkey, subkey, err);
            m.set_return(err);
        },
        20
    });

    // -------------------------------------------------------------------------
    // RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions,
    //               REGSAM samDesired, PHKEY phkResult)
    //   5 args, stdcall_bytes=20
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegOpenKeyExA", 2,
        [&thunks](Marshaler& m) {
            RegistryWindowTimer window_timer;
            u64 hkey     = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            u32 options  = static_cast<u32>(m.arg(2));
            u32 sam      = static_cast<u32>(m.arg(3));
            GuestAddr result_ptr = m.arg_ptr(4);

            std::string subkey_a = read_guest_astring(m.memory(), subkey_ptr);
            u64 result_handle = 0;
            u32 err = thunks.reg_open_key_ex_a(hkey, subkey_a, options, sam,
                                               &result_handle);
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            if (subkey_a.find("Internal") != std::string::npos) {
                std::fprintf(stderr,
                    "[registry] RegOpenKeyExA(0x%llx, \"%s\") -> %u handle=0x%llx\n",
                    static_cast<unsigned long long>(hkey),
                    subkey_a.c_str(), err,
                    static_cast<unsigned long long>(result_handle));
            }
            VITREO_TRACE( "[registry] RegOpenKeyExA(0x%llx, \"%s\") -> %u\n",
                         hkey, subkey_a.c_str(), err);
            log_registry_open("RegOpenKeyExA", hkey, ansi_to_wide(subkey_a), err);
            m.set_return(err);
        },
        20
    });

    // -------------------------------------------------------------------------
    // RegCreateKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD Reserved,
    //                 LPWSTR lpClass, DWORD dwOptions, REGSAM samDesired,
    //                 LPSECURITY_ATTRIBUTES lpSA, PHKEY phkResult,
    //                 LPDWORD lpdwDisposition)
    //   9 args, stdcall_bytes=36
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegCreateKeyExW", 3,
        [&thunks](Marshaler& m) {
            u64 hkey            = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            // args 2-4 and 6 ignored (reserved, class, options, security);
            // arg 5 (samDesired) carries the KEY_WOW64_* registry view.
            GuestAddr result_ptr = m.arg_ptr(7);
            GuestAddr disp_ptr   = m.arg_ptr(8);

            std::wstring subkey = read_guest_wstring(m.memory(), subkey_ptr);
            u64 result_handle = 0;
            u32 disposition = 0;
            u32 err = thunks.reg_create_key_ex_w(hkey, subkey,
                                                  &result_handle, &disposition,
                                                  static_cast<u32>(m.arg(5)));
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            if (err == ERROR_SUCCESS && disp_ptr != 0) {
                (void)m.write_u32(disp_ptr, disposition);
            }
            VITREO_TRACE( "[registry] RegCreateKeyExW(0x%llx, \"%ls\") -> %u handle=0x%llx disp=%u\n",
                         (unsigned long long)hkey, subkey.c_str(), err,
                         (unsigned long long)result_handle, disposition);
            m.set_return(err);
        },
        36
    });

    // -------------------------------------------------------------------------
    // RegCreateKeyTransactedW(HKEY, LPCWSTR lpSubKey, DWORD Reserved,
    //   LPWSTR lpClass, DWORD dwOptions, REGSAM samDesired,
    //   LPSECURITY_ATTRIBUTES, PHKEY phkResult, LPDWORD lpdwDisposition,
    //   HANDLE hTransaction, PVOID pExtendedParemeter)
    //   11 args, stdcall_bytes=44.
    // The key becomes part of hTransaction (a KTM transaction). Vitreo has no
    // Kernel Transaction Manager: ktmw32!CreateTransaction always fails, so
    // no valid transaction handle can exist and every call is refused with
    // ERROR_INVALID_HANDLE rather than creating a key outside a transaction.
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegCreateKeyTransactedW", 0,
        [](Marshaler& m) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                std::fprintf(stderr,
                    "[vitreo] UNIMPLEMENTED advapi32!RegCreateKeyTransactedW: "
                    "no Kernel Transaction Manager (transaction 0x%llx)\n",
                    static_cast<unsigned long long>(m.arg(9)));
            }
            if (const GuestAddr result = m.arg_ptr(7); result != 0) {
                (void)m.write_ptr(result, 0);
            }
            m.set_return(6u /*ERROR_INVALID_HANDLE*/);
        },
        44
    });

    // -------------------------------------------------------------------------
    // RegQueryValueExW(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved,
    //                  LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
    //   6 args, stdcall_bytes=24
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegQueryValueExW", 4,
        [&thunks](Marshaler& m) {
            RegistryWindowTimer window_timer;
            u64 hkey             = m.arg(0);
            GuestAddr name_ptr   = m.arg_ptr(1);
            // arg(2) = lpReserved, ignored
            GuestAddr type_ptr   = m.arg_ptr(3);
            GuestAddr data_ptr   = m.arg_ptr(4);
            GuestAddr size_ptr   = m.arg_ptr(5);

            std::wstring value_name = read_guest_wstring(m.memory(), name_ptr);

            // Read current buffer size from guest
            u32 buf_size = 0;
            if (size_ptr != 0) {
                auto sr = m.read_u32(size_ptr);
                if (sr.is_ok()) buf_size = sr.value();
            }

            const bool data_requested = data_ptr != 0;

            // Preserve the distinction between lpData == NULL (a size-only
            // query) and lpData != NULL with a zero-byte buffer. The latter
            // must reach the registry implementation as a provided buffer so
            // it can return ERROR_MORE_DATA and the required size without
            // dereferencing the guest pointer.
            std::vector<u8> host_buf(
                data_requested ? std::max<u32>(buf_size, 1u) : 0u);
            u32 type_out = 0;
            u32 data_size = buf_size;

            u32 err = thunks.reg_query_value_ex_w(
                hkey, value_name,
                &type_out,
                data_requested ? host_buf.data() : nullptr,
                &data_size);

            // Write back type
            if (type_ptr != 0) {
                (void)m.write_u32(type_ptr, type_out);
            }

            // Write back data — fail honestly if the guest buffer is not writable.
            if (err == ERROR_SUCCESS && data_requested && data_size > 0) {
                u32 copy_len = (data_size <= buf_size) ? data_size : buf_size;
                if (m.memory().write_bytes(
                        data_ptr,
                        reinterpret_cast<const Byte*>(host_buf.data()),
                        copy_len)
                        .is_err()) {
                    err = ERROR_NOACCESS;
                }
            }

            // Write back size
            if (size_ptr != 0) {
                (void)m.write_u32(size_ptr, data_size);
            }

            VITREO_TRACE( "[registry] RegQueryValueExW(0x%llx, \"%ls\") -> err=%u type=%u size=%u\n",
                         (unsigned long long)hkey, value_name.c_str(), err, type_out, data_size);
            log_registry_query("RegQueryValueExW", hkey, value_name, err,
                               type_out, data_size);
            m.set_return(err);
        },
        24
    });

    // -------------------------------------------------------------------------
    // RegGetValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValue,
    //              DWORD dwFlags, LPDWORD pdwType, PVOID pvData,
    //              LPDWORD pcbData)
    //   7 args, stdcall_bytes=28
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegGetValueW", 0,
        [&thunks](Marshaler& m) {
            constexpr u32 RRF_ZEROONFAILURE = 0x20000000u;
            const u64 hkey = m.arg(0);
            const std::wstring subkey = read_guest_wstring(m.memory(), m.arg_ptr(1));
            const std::wstring value_name = read_guest_wstring(m.memory(), m.arg_ptr(2));
            const u32 flags = static_cast<u32>(m.arg(3));
            const GuestAddr type_ptr = m.arg_ptr(4);
            const GuestAddr data_ptr = m.arg_ptr(5);
            const GuestAddr size_ptr = m.arg_ptr(6);

            u32 capacity = 0;
            if (size_ptr != 0) capacity = m.read_u32(size_ptr).value_or(0);
            std::vector<u8> data(capacity);
            u32 type = 0;
            u32 size = capacity;
            u32 err = thunks.reg_get_value_w(
                hkey, subkey, value_name, flags, &type,
                data_ptr != 0 ? data.data() : nullptr,
                size_ptr != 0 ? &size : nullptr);

            if (type_ptr != 0) (void)m.write_u32(type_ptr, type);
            if (size_ptr != 0) (void)m.write_u32(size_ptr, size);
            if (err == ERROR_SUCCESS && data_ptr != 0 && size != 0) {
                (void)m.memory().write_bytes(
                    data_ptr, reinterpret_cast<const Byte*>(data.data()), size);
            } else if (err != ERROR_SUCCESS && data_ptr != 0 &&
                       (flags & RRF_ZEROONFAILURE) != 0) {
                for (u32 i = 0; i < capacity; ++i) (void)m.write_u8(data_ptr + i, 0);
            }

            VITREO_TRACE("[registry] RegGetValueW(0x%llx, \"%ls\", \"%ls\", flags=0x%x) -> err=%u type=%u size=%u\n",
                         static_cast<unsigned long long>(hkey), subkey.c_str(),
                         value_name.c_str(), flags, err, type, size);
            log_registry_query("RegGetValueW", hkey, value_name, err, type,
                               size);
            m.set_return(err);
        },
        28
    });

    // -------------------------------------------------------------------------
    // RegGetValueA(HKEY hKey, LPCSTR lpSubKey, LPCSTR lpValue,
    //              DWORD dwFlags, LPDWORD pdwType, PVOID pvData,
    //              LPDWORD pcbData)
    //   7 args, stdcall_bytes=28
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegGetValueA", 0,
        [&thunks](Marshaler& m) {
            constexpr u32 RRF_ZEROONFAILURE = 0x20000000u;
            const u64 hkey = m.arg(0);
            const std::string subkey =
                read_guest_astring(m.memory(), m.arg_ptr(1));
            const std::string value_name =
                read_guest_astring(m.memory(), m.arg_ptr(2));
            const u32 flags = static_cast<u32>(m.arg(3));
            const GuestAddr type_ptr = m.arg_ptr(4);
            const GuestAddr data_ptr = m.arg_ptr(5);
            const GuestAddr size_ptr = m.arg_ptr(6);

            u32 capacity = 0;
            if (size_ptr != 0) capacity = m.read_u32(size_ptr).value_or(0);
            std::vector<u8> data(capacity);
            u32 type = 0;
            u32 size = capacity;
            const u32 err = thunks.reg_get_value_a(
                hkey, subkey, value_name, flags, &type,
                data_ptr != 0 ? data.data() : nullptr,
                size_ptr != 0 ? &size : nullptr);

            if (type_ptr != 0) (void)m.write_u32(type_ptr, type);
            if (size_ptr != 0) (void)m.write_u32(size_ptr, size);
            if (err == ERROR_SUCCESS && data_ptr != 0 && size != 0) {
                (void)m.memory().write_bytes(
                    data_ptr, reinterpret_cast<const Byte*>(data.data()), size);
            } else if (err != ERROR_SUCCESS && data_ptr != 0 &&
                       (flags & RRF_ZEROONFAILURE) != 0) {
                for (u32 i = 0; i < capacity; ++i) {
                    (void)m.write_u8(data_ptr + i, 0);
                }
            }

            VITREO_TRACE(
                "[registry] RegGetValueA(0x%llx, \"%s\", \"%s\", "
                "flags=0x%x) -> err=%u type=%u size=%u\n",
                static_cast<unsigned long long>(hkey), subkey.c_str(),
                value_name.c_str(), flags, err, type, size);
            log_registry_query("RegGetValueA", hkey, ansi_to_wide(value_name),
                               err, type, size);
            m.set_return(err);
        },
        28
    });

    // -------------------------------------------------------------------------
    // RegQueryValueExA(HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved,
    //                  LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
    //   6 args, stdcall_bytes=24
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegQueryValueExA", 5,
        [&thunks](Marshaler& m) {
            RegistryWindowTimer window_timer;
            u64 hkey             = m.arg(0);
            GuestAddr name_ptr   = m.arg_ptr(1);
            GuestAddr type_ptr   = m.arg_ptr(3);
            GuestAddr data_ptr   = m.arg_ptr(4);
            GuestAddr size_ptr   = m.arg_ptr(5);

            std::string value_name = read_guest_astring(m.memory(), name_ptr);

            u32 buf_size = 0;
            if (size_ptr != 0) {
                auto sr = m.read_u32(size_ptr);
                if (sr.is_ok()) buf_size = sr.value();
            }

            std::vector<u8> host_buf(buf_size);
            u32 type_out = 0;
            u32 data_size = buf_size;

            constexpr GuestAddr kMinUserDataPtr = 0x10000;
            const bool data_requested = data_ptr != 0;
            if (data_requested && data_ptr < kMinUserDataPtr) {
                log_registry_query("RegQueryValueExA", hkey,
                                   ansi_to_wide(value_name),
                                   ERROR_NOACCESS, 0, buf_size);
                m.set_return(ERROR_NOACCESS);
                return;
            }

            u32 err = thunks.reg_query_value_ex_a(
                hkey, value_name,
                &type_out,
                data_requested ? host_buf.data() : nullptr,
                &data_size);

            if (value_name == "Debug Memory" || value_name == "Protect Memory") {
                u32 dword = 0;
                if (err == ERROR_SUCCESS && data_size >= 4 && data_requested) {
                    std::memcpy(&dword, host_buf.data(), sizeof(dword));
                }
                std::fprintf(stderr,
                    "[registry] RegQueryValueExA(\"%s\") -> err=%u type=%u "
                    "size=%u dword=%u\n",
                    value_name.c_str(), err, type_out, data_size, dword);
            }

            if (type_ptr != 0) {
                (void)m.write_u32(type_ptr, type_out);
            }
            if (err == ERROR_SUCCESS && data_requested && data_size > 0) {
                u32 copy_len = (data_size <= buf_size) ? data_size : buf_size;
                if (m.memory().write_bytes(
                        data_ptr,
                        reinterpret_cast<const Byte*>(host_buf.data()),
                        copy_len)
                        .is_err()) {
                    err = ERROR_NOACCESS;
                }
            }
            if (size_ptr != 0) {
                (void)m.write_u32(size_ptr, data_size);
            }

            VITREO_TRACE(
                "[registry] RegQueryValueExA(0x%llx, \"%s\") -> "
                "err=%u type=%u size=%u\n",
                static_cast<unsigned long long>(hkey), value_name.c_str(),
                err, type_out, data_size);
            log_registry_query("RegQueryValueExA", hkey,
                               ansi_to_wide(value_name), err, type_out,
                               data_size);
            m.set_return(err);
        },
        24
    });

    // -------------------------------------------------------------------------
    // RegSetValueExW(HKEY hKey, LPCWSTR lpValueName, DWORD Reserved,
    //                DWORD dwType, const BYTE* lpData, DWORD cbData)
    //   6 args, stdcall_bytes=24
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegSetValueExW", 6,
        [&thunks](Marshaler& m) {
            u64 hkey             = m.arg(0);
            GuestAddr name_ptr   = m.arg_ptr(1);
            // arg(2) = reserved
            u32 type             = static_cast<u32>(m.arg(3));
            GuestAddr data_ptr   = m.arg_ptr(4);
            u32 data_size        = static_cast<u32>(m.arg(5));

            std::wstring value_name = read_guest_wstring(m.memory(), name_ptr);

            // Read data from guest memory
            std::vector<u8> host_data(data_size);
            if (data_ptr != 0 && data_size > 0) {
                (void)m.memory().read_bytes(data_ptr,
                    reinterpret_cast<Byte*>(host_data.data()), data_size);
            }

            u32 err = thunks.reg_set_value_ex_w(
                hkey, value_name, type, host_data.data(), data_size);
            m.set_return(err);
        },
        24
    });

    // -------------------------------------------------------------------------
    // RegDeleteValueW(HKEY hKey, LPCWSTR lpValueName)
    //   2 args, stdcall_bytes=8
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegDeleteValueW", 7,
        [&thunks](Marshaler& m) {
            u64 hkey           = m.arg(0);
            GuestAddr name_ptr = m.arg_ptr(1);
            std::wstring value_name = read_guest_wstring(m.memory(), name_ptr);
            u32 err = thunks.reg_delete_value_w(hkey, value_name);
            m.set_return(err);
        },
        8
    });

    // -------------------------------------------------------------------------
    // RegSetKeyValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValueName,
    //                 DWORD dwType, LPCVOID lpData, DWORD cbData)
    //   6 args, stdcall_bytes=24
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegSetKeyValueW", 0,
        [&thunks](Marshaler& m) {
            u64 hkey             = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            GuestAddr name_ptr   = m.arg_ptr(2);
            u32 type             = static_cast<u32>(m.arg(3));
            GuestAddr data_ptr   = m.arg_ptr(4);
            u32 data_size        = static_cast<u32>(m.arg(5));

            std::wstring subkey = read_guest_wstring(m.memory(), subkey_ptr);
            std::wstring value_name = read_guest_wstring(m.memory(), name_ptr);

            std::vector<u8> host_data(data_size);
            if (data_ptr != 0 && data_size > 0) {
                (void)m.memory().read_bytes(data_ptr,
                    reinterpret_cast<Byte*>(host_data.data()), data_size);
            }

            u32 err = thunks.reg_set_key_value_w(
                hkey, subkey, value_name, type,
                host_data.empty() ? nullptr : host_data.data(), data_size);
            VITREO_TRACE("[registry] RegSetKeyValueW(0x%llx, \"%ls\", \"%ls\", type=%u, size=%u) -> %u\n",
                         static_cast<unsigned long long>(hkey),
                         subkey.c_str(), value_name.c_str(), type, data_size, err);
            m.set_return(err);
        },
        24
    });

    // -------------------------------------------------------------------------
    // RegDeleteKeyValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValueName)
    //   3 args, stdcall_bytes=12
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegDeleteKeyValueW", 0,
        [&thunks](Marshaler& m) {
            u64 hkey             = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            GuestAddr name_ptr   = m.arg_ptr(2);
            std::wstring subkey = read_guest_wstring(m.memory(), subkey_ptr);
            std::wstring value_name = read_guest_wstring(m.memory(), name_ptr);
            u32 err = thunks.reg_delete_key_value_w(hkey, subkey, value_name);
            VITREO_TRACE("[registry] RegDeleteKeyValueW(0x%llx, \"%ls\", \"%ls\") -> %u\n",
                         static_cast<unsigned long long>(hkey),
                         subkey.c_str(), value_name.c_str(), err);
            m.set_return(err);
        },
        12
    });

    // -------------------------------------------------------------------------
    // RegCloseKey(HKEY hKey)
    //   1 arg, stdcall_bytes=4
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegCloseKey", 8,
        [&thunks](Marshaler& m) {
            u64 hkey = m.arg(0);
            u32 err = thunks.reg_close_key(hkey);
            m.set_return(err);
        },
        4
    });

    // -------------------------------------------------------------------------
    // RegNotifyChangeKeyValue(HKEY, BOOL, DWORD, HANDLE, BOOL)
    //   5 args, stdcall_bytes=20
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegNotifyChangeKeyValue", 0,
        [&thunks](Marshaler& m) {
            const u64 hkey = m.arg(0);
            // BOOL/DWORD parameters are 32-bit: only the low half of the x64
            // register or stack slot is defined (fAsynchronous is on the
            // stack at index 4).
            const bool watch_subtree = m.arg32(1) != 0;
            const u32 filter = m.arg32(2);
            const Handle event = static_cast<Handle>(m.arg(3));
            const bool asynchronous = m.arg32(4) != 0;
            const u32 err = thunks.reg_notify_change_key_value(
                hkey, watch_subtree, filter, event, asynchronous);
            m.set_return(err);
        },
        20
    });

    // -------------------------------------------------------------------------
    // RegFlushKey(HKEY hKey)
    //   1 arg, stdcall_bytes=4
    //
    // The capsule registry is an in-memory store that is serialized to the
    // capsule's registry directory before child launch and when the process
    // exits. Every value written by RegSetValueEx is therefore already visible
    // to any subsequent in-process query. The remaining difference from
    // Windows is durability: Vitreo does not write the hive to disk at the
    // point of RegFlushKey itself, so changes are lost if the host process is
    // killed before either persistence boundary. Validating the handle keeps
    // the current supported contract honest.
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegFlushKey", 0,
        [&thunks](Marshaler& m) {
            u64 hkey = m.arg(0);
            if (!thunks.is_valid_key(hkey)) {
                VITREO_TRACE("[advapi32] RegFlushKey hkey=0x%llx -> "
                             "ERROR_INVALID_HANDLE\n",
                             static_cast<unsigned long long>(hkey));
                m.set_return(ERROR_INVALID_HANDLE);
                return;
            }
            VITREO_TRACE("[advapi32] RegFlushKey hkey=0x%llx -> ERROR_SUCCESS "
                         "(capsule hive persists before child launch/exit)\n",
                         static_cast<unsigned long long>(hkey));
            m.set_return(ERROR_SUCCESS);
        },
        4
    });

    // -------------------------------------------------------------------------
    // RegEnumKeyExW(HKEY hKey, DWORD dwIndex, LPWSTR lpName,
    //               LPDWORD lpcchName, LPDWORD lpReserved,
    //               LPWSTR lpClass, LPDWORD lpcchClass,
    //               PFILETIME lpftLastWriteTime)
    //   8 args, stdcall_bytes=32
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegEnumKeyExW", 9,
        [&thunks](Marshaler& m) {
            u64 hkey              = m.arg(0);
            u32 index             = static_cast<u32>(m.arg(1));
            GuestAddr name_ptr    = m.arg_ptr(2);
            GuestAddr namelen_ptr = m.arg_ptr(3);

            std::wstring name_out;
            u32 err = thunks.reg_enum_key_ex_w(hkey, index, name_out);

            if (err == ERROR_SUCCESS) {
                // lpcchName is required; Windows rejects a NULL one rather
                // than guessing a buffer size.
                if (namelen_ptr == 0) {
                    m.set_return(ERROR_INVALID_PARAMETER);
                    return;
                }
                auto nr = m.read_u32(namelen_ptr);
                const u32 max_chars = nr.is_ok() ? nr.value() : 0;

                // Callers probe with a deliberately small buffer and grow on
                // ERROR_MORE_DATA. Verified against Windows: when the name
                // does not fit, the buffer and *lpcchName are both left
                // untouched and the call fails. Truncating and reporting
                // success instead hands back a one-character key name that
                // the caller then uses as if it were real.
                if (name_out.size() + 1 > max_chars) {
                    m.set_return(ERROR_MORE_DATA);
                    return;
                }
                if (name_ptr != 0) {
                    write_guest_wstring(m.memory(), name_ptr, name_out,
                                        max_chars);
                }
                // On success lpcchName is the length excluding the NUL.
                (void)m.write_u32(namelen_ptr,
                                  static_cast<u32>(name_out.size()));
            }
            m.set_return(err);
        },
        32
    });

    // -------------------------------------------------------------------------
    // RegEnumValueW(HKEY hKey, DWORD dwIndex, LPWSTR lpValueName,
    //               LPDWORD lpcchValueName, LPDWORD lpReserved,
    //               LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
    //   8 args, stdcall_bytes=32
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegEnumValueW", 10,
        [&thunks](Marshaler& m) { reg_enum_value(thunks, m, /*wide=*/true); },
        32
    });

    // -------------------------------------------------------------------------
    // RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved,
    //                 LPSTR lpClass, DWORD dwOptions, REGSAM samDesired,
    //                 LPSECURITY_ATTRIBUTES lpSA, PHKEY phkResult,
    //                 LPDWORD lpdwDisposition)
    //   9 args, stdcall_bytes=36
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegCreateKeyExA", 13,
        [&thunks](Marshaler& m) {
            u64 hkey            = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            // args 2-4 and 6 ignored (reserved, class, options, security);
            // arg 5 (samDesired) carries the KEY_WOW64_* registry view.
            GuestAddr result_ptr = m.arg_ptr(7);
            GuestAddr disp_ptr   = m.arg_ptr(8);

            std::string subkey = read_guest_astring(m.memory(), subkey_ptr);
            u64 result_handle = 0;
            u32 disposition = 0;
            u32 err = thunks.reg_create_key_ex_a(hkey, subkey,
                                                  &result_handle, &disposition,
                                                  static_cast<u32>(m.arg(5)));
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            if (err == ERROR_SUCCESS && disp_ptr != 0) {
                (void)m.write_u32(disp_ptr, disposition);
            }
            VITREO_TRACE( "[registry] RegCreateKeyExA(0x%llx, \"%s\") -> %u\n",
                         hkey, subkey.c_str(), err);
            m.set_return(err);
        },
        36
    });

    // -------------------------------------------------------------------------
    // RegOpenKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult)
    //   3 args, stdcall_bytes=12
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegOpenKeyA", 14,
        [&thunks](Marshaler& m) {
            u64 hkey     = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            GuestAddr result_ptr = m.arg_ptr(2);

            std::string subkey_a = read_guest_astring(m.memory(), subkey_ptr);
            u64 result_handle = 0;
            u32 err = thunks.reg_open_key_a(hkey, subkey_a, &result_handle);
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            VITREO_TRACE( "[registry] RegOpenKeyA(0x%llx, \"%s\") -> %u\n",
                         hkey, subkey_a.c_str(), err);
            m.set_return(err);
        },
        12
    });

    // -------------------------------------------------------------------------
    // RegSetValueExA(HKEY hKey, LPCSTR lpValueName, DWORD Reserved,
    //                DWORD dwType, const BYTE* lpData, DWORD cbData)
    //   6 args, stdcall_bytes=24
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegSetValueExA", 15,
        [&thunks](Marshaler& m) {
            u64 hkey             = m.arg(0);
            GuestAddr name_ptr   = m.arg_ptr(1);
            // arg(2) = reserved
            u32 type             = static_cast<u32>(m.arg(3));
            GuestAddr data_ptr   = m.arg_ptr(4);
            u32 data_size        = static_cast<u32>(m.arg(5));

            std::string value_name = read_guest_astring(m.memory(), name_ptr);

            // Read data from guest memory
            std::vector<u8> host_data(data_size);
            if (data_ptr != 0 && data_size > 0) {
                (void)m.memory().read_bytes(data_ptr,
                    reinterpret_cast<Byte*>(host_data.data()), data_size);
            }

            u32 err = thunks.reg_set_value_ex_a(
                hkey, value_name, type, host_data.data(), data_size);
            VITREO_TRACE( "[registry] RegSetValueExA(0x%llx, \"%s\", type=%u, size=%u) -> %u\n",
                         hkey, value_name.c_str(), type, data_size, err);
            m.set_return(err);
        },
        24
    });

    // -------------------------------------------------------------------------
    // RegDeleteKeyW(HKEY hKey, LPCWSTR lpSubKey)
    //   2 args, stdcall_bytes=8
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegDeleteKeyW", 11,
        [&thunks](Marshaler& m) {
            // Real advapi32 returns ERROR_INVALID_PARAMETER (87) when the
            // root HKEY is NULL.
            if (m.arg(0) == 0) {
                m.set_return(static_cast<u64>(87u));
                return;
            }
            std::wstring subkey;
            if (GuestAddr p = m.arg_ptr(1); p != 0) {
                auto r = m.read_wide_string(p);
                if (r.is_ok()) subkey = ansi_to_wide(r.value());
            }
            u32 err = thunks.reg_delete_key(m.arg(0), subkey);
            VITREO_TRACE("[registry] RegDeleteKeyW(0x%llx, \"%ls\") -> %u\n",
                         static_cast<unsigned long long>(m.arg(0)),
                         subkey.c_str(), err);
            m.set_return(err);
        },
        8
    });

    // -------------------------------------------------------------------------
    // RegDeleteKeyExW(HKEY hKey, LPCWSTR lpSubKey, REGSAM samDesired,
    //                 DWORD Reserved)
    //   4 args, stdcall_bytes=16
    //
    // Resolved by GetProcAddress. Callers treat a missing export as fatal.
    // The supported contract deletes the key; it is not a feature-probe
    // success stub.
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegDeleteKeyExW", 0,
        [&thunks](Marshaler& m) {
            if (m.arg(0) == 0 || m.arg_ptr(1) == 0) {
                m.set_return(static_cast<u64>(ERROR_INVALID_PARAMETER));
                return;
            }

            std::wstring subkey;
            auto read = m.read_wide_string(m.arg_ptr(1));
            if (read.is_err()) {
                m.set_return(static_cast<u64>(ERROR_INVALID_PARAMETER));
                return;
            }
            subkey = ansi_to_wide(read.value());

            const u32 sam_desired = static_cast<u32>(m.arg(2));
            const u32 reserved = static_cast<u32>(m.arg(3));
            const u32 err = thunks.reg_delete_key_ex(
                m.arg(0), subkey, sam_desired, reserved);
            VITREO_TRACE(
                "[registry] RegDeleteKeyExW(0x%llx, \"%ls\", view=0x%x, reserved=%u) -> %u\n",
                static_cast<unsigned long long>(m.arg(0)), subkey.c_str(),
                sam_desired, reserved, err);
            m.set_return(err);
        },
        16
    });

    // RegDeleteKeyExA(HKEY, LPCSTR, REGSAM, DWORD) — stdcall 16.
    // Same deletion as RegDeleteKeyExW. The subkey is an ANSI string.
    exports.entries.push_back(ExportEntry{
        "RegDeleteKeyExA", 0,
        [&thunks](Marshaler& m) {
            if (m.arg(0) == 0 || m.arg_ptr(1) == 0) {
                m.set_return(static_cast<u64>(ERROR_INVALID_PARAMETER));
                return;
            }
            auto read = m.read_ansi_string(m.arg_ptr(1));
            if (read.is_err()) {
                m.set_return(static_cast<u64>(ERROR_INVALID_PARAMETER));
                return;
            }
            const u32 sam_desired = static_cast<u32>(m.arg(2));
            const u32 reserved = static_cast<u32>(m.arg(3));
            const u32 err = thunks.reg_delete_key_ex(
                m.arg(0), ansi_to_wide(read.value()), sam_desired, reserved);
            VITREO_TRACE(
                "[registry] RegDeleteKeyExA(0x%llx, \"%s\", view=0x%x, reserved=%u) -> %u\n",
                static_cast<unsigned long long>(m.arg(0)),
                read.value().c_str(), sam_desired, reserved, err);
            m.set_return(err);
        },
        16
    });

    // -------------------------------------------------------------------------
    // RegDeleteKeyA(HKEY hKey, LPCSTR lpSubKey)
    //   2 args, stdcall_bytes=8
    //
    // Previously unregistered, so it fell through to the missing-export stub
    // which pops none of its 8 stdcall bytes and corrupts the guest stack.
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegDeleteKeyA", 0,
        [&thunks](Marshaler& m) {
            if (m.arg(0) == 0) {
                m.set_return(static_cast<u64>(87u));
                return;
            }
            std::wstring subkey;
            if (GuestAddr p = m.arg_ptr(1); p != 0) {
                auto r = m.read_ansi_string(p);
                if (r.is_ok()) subkey = ansi_to_wide(r.value());
            }
            u32 err = thunks.reg_delete_key(m.arg(0), subkey);
            VITREO_TRACE("[registry] RegDeleteKeyA(0x%llx, \"%ls\") -> %u\n",
                         static_cast<unsigned long long>(m.arg(0)),
                         subkey.c_str(), err);
            m.set_return(err);
        },
        8
    });

    // LSTATUS RegDeleteTreeA(HKEY hKey, LPCSTR lpSubKey)
    // https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regdeletetreea
    exports.entries.push_back(ExportEntry{
        "RegDeleteTreeA", 0,
        [&thunks](Marshaler& m) {
            if (m.arg(0) == 0) {
                m.set_return(ERROR_INVALID_HANDLE);
                return;
            }
            std::wstring subkey;
            if (GuestAddr p = m.arg_ptr(1); p != 0) {
                auto r = m.read_ansi_string(p);
                if (r.is_ok()) subkey = ansi_to_wide(r.value());
            }
            m.set_return(thunks.reg_delete_tree(m.arg(0), subkey));
        },
        8
    });
    exports.entries.push_back(ExportEntry{
        "RegDeleteTreeW", 0,
        [&thunks](Marshaler& m) {
            if (m.arg(0) == 0) {
                m.set_return(ERROR_INVALID_HANDLE);
                return;
            }
            std::wstring subkey;
            if (GuestAddr p = m.arg_ptr(1); p != 0) {
                auto r = m.read_wide_string(p);
                if (r.is_ok()) subkey = ansi_to_wide(r.value());
            }
            m.set_return(thunks.reg_delete_tree(m.arg(0), subkey));
        },
        8
    });

    // LONG RegDisableReflectionKey(HKEY hBase)
    // LONG RegEnableReflectionKey(HKEY hBase)
    // LONG RegQueryReflectionKey(HKEY hBase, BOOL* bIsReflectionDisabled)
    // https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regdisablereflectionkey
    // Registry reflection was removed in Windows 7 (Learn, "Registry
    // Reflection"). On Windows 11 all three succeed for any handle, even an
    // invalid or NULL one, and the query always reports reflection disabled
    // (wow64_registry_probe refl_*, x86 and x64 identical).
    exports.entries.push_back(ExportEntry{
        "RegDisableReflectionKey", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(ERROR_SUCCESS);
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "RegEnableReflectionKey", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(ERROR_SUCCESS);
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "RegQueryReflectionKey", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            // A NULL or unwritable output faults inside advapi32 on Windows;
            // report the bad pointer instead of claiming a result.
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0 || m.write_u32(out, 1u /*TRUE*/).is_err()) {
                m.set_return(ERROR_NOACCESS);
                return;
            }
            m.set_return(ERROR_SUCCESS);
        },
        8
    });

    // -------------------------------------------------------------------------
    // RegEnumKeyA(HKEY hKey, DWORD dwIndex, LPSTR lpName, DWORD cchName)
    //   4 args, stdcall_bytes=16
    //
    // The legacy ANSI form of RegEnumKeyEx. It was previously unregistered, so
    // it resolved to the generic missing-export stub, which returns
    // 0xFFFFFFFF and pops none of its 16 stdcall bytes — corrupting the guest
    // stack on every call.
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegEnumKeyA", 0,
        [&thunks](Marshaler& m) {
            u64 hkey           = m.arg(0);
            u32 index          = static_cast<u32>(m.arg(1));
            GuestAddr name_ptr = m.arg_ptr(2);
            u32 cch_name       = static_cast<u32>(m.arg(3));

            std::string name_out;
            u32 err = thunks.reg_enum_key_ex_a(hkey, index, name_out);
            if (err == ERROR_SUCCESS && name_ptr != 0) {
                if (name_out.size() + 1 > cch_name) {
                    err = ERROR_MORE_DATA;
                } else {
                    for (usize i = 0; i < name_out.size(); ++i) {
                        (void)m.memory().write_u8(
                            name_ptr + static_cast<GuestAddr>(i),
                            static_cast<u8>(name_out[i]));
                    }
                    (void)m.memory().write_u8(
                        name_ptr + static_cast<GuestAddr>(name_out.size()), 0);
                }
            }
            VITREO_TRACE("[registry] RegEnumKeyA(0x%llx, %u) -> %u \"%s\"\n",
                         static_cast<unsigned long long>(hkey), index, err,
                         name_out.c_str());
            m.set_return(err);
        },
        16
    });

    // -------------------------------------------------------------------------
    // RegEnumKeyW(HKEY hKey, DWORD dwIndex, LPWSTR lpName, DWORD cchName)
    //   4 args, stdcall_bytes=16
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegEnumKeyW", 12,
        [&thunks](Marshaler& m) {
            u64 hkey            = m.arg(0);
            u32 index           = static_cast<u32>(m.arg(1));
            GuestAddr name_ptr  = m.arg_ptr(2);
            u32 cch_name        = static_cast<u32>(m.arg(3));

            std::wstring name_out;
            u32 err = thunks.reg_enum_key_ex_w(hkey, index, name_out);
            if (err == ERROR_SUCCESS && name_ptr != 0) {
                write_guest_wstring(m.memory(), name_ptr, name_out, cch_name);
            }
            m.set_return(err);
        },
        16
    });

    // RegQueryValueA(hKey, lpSubKey, lpData, lpcbData) → LONG
    exports.entries.push_back(ExportEntry{
        "RegQueryValueA", 0,
        [](Marshaler& m) {
            (void)m.arg(0); (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
            m.set_return(2u); // ERROR_FILE_NOT_FOUND — key not found
        },
        16
    });

    // -------------------------------------------------------------------------
    // RegQueryValueW(HKEY hKey, LPCWSTR lpSubKey, LPWSTR lpData, PLONG lpcbData)
    //   4 args, stdcall_bytes=16
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegQueryValueW", 16,
        [&thunks](Marshaler& m) {
            u64 hkey            = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            GuestAddr data_ptr  = m.arg_ptr(2);
            GuestAddr size_ptr  = m.arg_ptr(3);

            std::wstring subkey = read_guest_wstring(m.memory(), subkey_ptr);

            u32 buf_size = 0;
            if (size_ptr != 0) {
                auto sr = m.read_u32(size_ptr);
                if (sr.is_ok()) buf_size = sr.value();
            }

            std::vector<u8> host_buf(buf_size);
            u32 data_size = buf_size;
            u32 err = thunks.reg_query_value_w(hkey, subkey,
                                               data_ptr != 0 ? host_buf.data() : nullptr,
                                               &data_size);
            if (err == ERROR_SUCCESS && data_ptr != 0 && data_size > 0) {
                u32 copy_len = (data_size <= buf_size) ? data_size : buf_size;
                (void)m.memory().write_bytes(data_ptr,
                    reinterpret_cast<const Byte*>(host_buf.data()), copy_len);
            }
            if (size_ptr != 0) {
                (void)m.write_u32(size_ptr, data_size);
            }
            VITREO_TRACE("[registry] RegQueryValueW(0x%llx, \"%ls\") -> %u\n",
                         (unsigned long long)hkey, subkey.c_str(), err);
            m.set_return(err);
        },
        16
    });

    // -------------------------------------------------------------------------
    // RegEnumKeyExA(HKEY hKey, DWORD dwIndex, LPSTR lpName,
    //               LPDWORD lpcchName, LPDWORD lpReserved,
    //               LPSTR lpClass, LPDWORD lpcchClass,
    //               PFILETIME lpftLastWriteTime)
    //   8 args, stdcall_bytes=32
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegEnumKeyExA", 17,
        [&thunks](Marshaler& m) {
            u64 hkey              = m.arg(0);
            u32 index             = static_cast<u32>(m.arg(1));
            GuestAddr name_ptr    = m.arg_ptr(2);
            GuestAddr namelen_ptr = m.arg_ptr(3);

            std::string name_out;
            u32 err = thunks.reg_enum_key_ex_a(hkey, index, name_out);

            if (err == ERROR_SUCCESS) {
                if (namelen_ptr == 0) {
                    m.set_return(ERROR_INVALID_PARAMETER);
                    return;
                }
                auto nr = m.read_u32(namelen_ptr);
                const u32 max_chars = nr.is_ok() ? nr.value() : 0;
                // Same small-buffer contract as the wide variant: fail with
                // ERROR_MORE_DATA rather than truncating the name.
                if (name_out.size() + 1 > max_chars) {
                    m.set_return(ERROR_MORE_DATA);
                    return;
                }
                const u32 n = static_cast<u32>(name_out.size());
                if (name_ptr != 0) {
                    for (u32 i = 0; i < n; ++i) {
                        (void)m.write_u8(name_ptr + i,
                                         static_cast<u8>(name_out[i]));
                    }
                    (void)m.write_u8(name_ptr + n, 0);
                }
                (void)m.write_u32(namelen_ptr, n);
            }
            m.set_return(err);
        },
        32
    });

    // -------------------------------------------------------------------------
    // RegQueryInfoKeyA(HKEY hKey, LPSTR lpClass, LPDWORD lpcchClass,
    //   LPDWORD lpReserved, LPDWORD lpcSubKeys, LPDWORD lpcbMaxSubKeyLen,
    //   LPDWORD lpcbMaxClassLen, LPDWORD lpcValues, LPDWORD lpcbMaxValueNameLen,
    //   LPDWORD lpcbMaxValueLen, LPDWORD lpcbSecurityDescriptor,
    //   PFILETIME lpftLastWriteTime)
    //   12 args, stdcall_bytes=48
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegQueryInfoKeyA", 18,
        [&thunks](Marshaler& m) { reg_query_info_key(thunks, m, /*wide=*/false); },
        48
    });

    // -------------------------------------------------------------------------
    // RegOpenKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult)
    //   3 args, stdcall_bytes=12
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegOpenKeyW", 19,
        [&thunks](Marshaler& m) {
            u64 hkey            = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            GuestAddr result_ptr = m.arg_ptr(2);

            std::wstring subkey = read_guest_wstring(m.memory(), subkey_ptr);
            u64 result_handle = 0;
            u32 err = thunks.reg_open_key_w(hkey, subkey, &result_handle);
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            VITREO_TRACE("[registry] RegOpenKeyW(0x%llx, \"%ls\") -> %u\n",
                         (unsigned long long)hkey, subkey.c_str(), err);
            m.set_return(err);
        },
        12
    });

    // -------------------------------------------------------------------------
    // RegDeleteValueA(HKEY hKey, LPCSTR lpValueName)
    //   2 args, stdcall_bytes=8
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegDeleteValueA", 0,
        [&thunks](Marshaler& m) {
            u64 hkey           = m.arg(0);
            GuestAddr name_ptr = m.arg_ptr(1);
            std::string value_name_a = read_guest_astring(m.memory(), name_ptr);
            std::wstring value_name  = ansi_to_wide(value_name_a);
            u32 err = thunks.reg_delete_value_w(hkey, value_name);
            VITREO_TRACE("[registry] RegDeleteValueA(0x%llx, \"%s\") -> %u\n",
                         (unsigned long long)hkey, value_name_a.c_str(), err);
            m.set_return(err);
        },
        8
    });

    // -------------------------------------------------------------------------
    // RegQueryInfoKeyW — wide variant (see reg_query_info_key)
    //   12 args, stdcall_bytes=48
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegQueryInfoKeyW", 0,
        [&thunks](Marshaler& m) { reg_query_info_key(thunks, m, /*wide=*/true); },
        48
    });

    // -------------------------------------------------------------------------
    // RegEnumValueA(HKEY hKey, DWORD dwIndex, LPSTR lpValueName,
    //               LPDWORD lpcchValueName, LPDWORD lpReserved,
    //               LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
    //   8 args, stdcall_bytes=32
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegEnumValueA", 0,
        [&thunks](Marshaler& m) { reg_enum_value(thunks, m, /*wide=*/false); },
        32
    });

    // -------------------------------------------------------------------------
    // RegCreateKeyA(HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult)
    //   3 args, stdcall_bytes=12
    // Legacy 16-bit-style API; forward to the Ex variant via thunks.
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegCreateKeyA", 0,
        [&thunks](Marshaler& m) {
            u64 hkey            = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            GuestAddr result_ptr = m.arg_ptr(2);

            std::string subkey_a = read_guest_astring(m.memory(), subkey_ptr);
            std::wstring subkey  = ansi_to_wide(subkey_a);
            u64 result_handle = 0;
            u32 disposition = 0;
            u32 err = thunks.reg_create_key_ex_w(hkey, subkey,
                                                  &result_handle, &disposition);
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            VITREO_TRACE("[registry] RegCreateKeyA(0x%llx, \"%s\") -> %u handle=0x%llx\n",
                         (unsigned long long)hkey, subkey_a.c_str(), err,
                         (unsigned long long)result_handle);
            m.set_return(err);
        },
        12
    });

    // -------------------------------------------------------------------------
    // RegCreateKeyW(HKEY hKey, LPCWSTR lpSubKey, PHKEY phkResult)
    //   3 args, stdcall_bytes=12
    // Learn: "This function is provided only for compatibility with 16-bit
    // versions of Windows" and behaves as RegCreateKeyEx with Reserved 0, no
    // class, REG_OPTION_NON_VOLATILE and MAXIMUM_ALLOWED access; it has no
    // disposition out-parameter, and it returns ERROR_SUCCESS or a nonzero
    // system error code (not a BOOL). A NULL or empty lpSubKey opens hKey
    // itself, which reg_create_key_ex_w already models.
    // -------------------------------------------------------------------------
    exports.entries.push_back(ExportEntry{
        "RegCreateKeyW", 0,
        [&thunks](Marshaler& m) {
            u64 hkey             = m.arg(0);
            GuestAddr subkey_ptr = m.arg_ptr(1);
            GuestAddr result_ptr = m.arg_ptr(2);

            std::wstring subkey = read_guest_wstring(m.memory(), subkey_ptr);
            u64 result_handle = 0;
            u32 disposition = 0;
            u32 err = thunks.reg_create_key_ex_w(hkey, subkey,
                                                  &result_handle, &disposition);
            if (err == ERROR_SUCCESS && result_ptr != 0) {
                write_guest_hkey(m, result_ptr, result_handle);
            }
            VITREO_TRACE("[registry] RegCreateKeyW(0x%llx, \"%ls\") -> %u handle=0x%llx\n",
                         (unsigned long long)hkey, subkey.c_str(), err,
                         (unsigned long long)result_handle);
            m.set_return(err);
        },
        12
    });

    // RegSetKeyValueA — the convenience wrapper: open (creating) the subkey,
    // write the value, close. Windows documents it as exactly that sequence,
    // so it is implemented as that sequence rather than refused.
    exports.entries.push_back(ExportEntry{
        "RegSetKeyValueA", 0,
        [&thunks](Marshaler& m) {
            const u64 hkey = m.arg(0);
            const GuestAddr subkey_ptr = m.arg_ptr(1);
            const GuestAddr name_ptr = m.arg_ptr(2);
            const u32 type = static_cast<u32>(m.arg(3));
            const GuestAddr data_ptr = m.arg_ptr(4);
            const u32 data_size = static_cast<u32>(m.arg(5));

            const std::string subkey =
                subkey_ptr == 0 ? std::string()
                                : read_guest_astring(m.memory(), subkey_ptr);
            const std::string value_name =
                name_ptr == 0 ? std::string()
                              : read_guest_astring(m.memory(), name_ptr);
            std::vector<u8> host_data(data_size);
            if (data_ptr != 0 && data_size > 0) {
                (void)m.memory().read_bytes(
                    data_ptr, reinterpret_cast<Byte*>(host_data.data()),
                    data_size);
            }

            u64 target = hkey;
            bool opened = false;
            if (!subkey.empty()) {
                u64 created = 0;
                u32 disposition = 0;
                const u32 err = thunks.reg_create_key_ex_a(
                    hkey, subkey, &created, &disposition);
                if (err != 0) {
                    m.set_return(err);
                    return;
                }
                target = created;
                opened = true;
            }
            const u32 err = thunks.reg_set_value_ex_a(
                target, value_name, type, host_data.data(), data_size);
            if (opened) (void)thunks.reg_close_key(target);
            VITREO_TRACE(
                "[registry] RegSetKeyValueA(0x%llx, \"%s\", \"%s\", type=%u, "
                "size=%u) -> %u\n",
                static_cast<unsigned long long>(hkey), subkey.c_str(),
                value_name.c_str(), type, data_size, err);
            m.set_return(err);
        },
        24});

    return exports;
}

} // namespace vitreo::winapi

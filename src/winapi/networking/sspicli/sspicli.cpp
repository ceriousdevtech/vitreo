// SPDX-License-Identifier: LGPL-2.1-only
/// @file sspicli.cpp
/// @brief SspiCli.dll — untrusted LSA connection and GetUserNameEx.
///
/// Every value below is measured on Windows 11 (workgroup machine) by
/// tools/oracle_probe/sspi_secur32_probe.c; see its .expected files.

#include "sspicli.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

namespace vitreo::winapi {

namespace {

// NTSTATUS values (ntstatus.h).
constexpr u32 STATUS_SUCCESS              = 0x00000000u;
constexpr u32 STATUS_INVALID_HANDLE       = 0xC0000008u;
constexpr u32 STATUS_MEMORY_NOT_ALLOCATED = 0xC00000A0u;
constexpr u32 STATUS_NOT_SUPPORTED        = 0xC00000BBu;
constexpr u32 STATUS_NO_SUCH_PACKAGE      = 0xC00000FEu;
constexpr u32 STATUS_INVALID_CONNECTION   = 0xC0000140u;
constexpr u32 RPC_NT_SS_IN_NULL_CONTEXT   = 0xC0030004u;
constexpr u32 RPC_NT_NULL_REF_POINTER     = 0xC0030009u;

// EXTENDED_NAME_FORMAT (secext.h): NameUnknown = 0 ... NameSurname = 14.
constexpr u32 kNameSamCompatible = 2;
constexpr u32 kNameFormatLast    = 14;

// GetComputerNameW / GetUserNameW report these (kernel32 SysInfoManager
// "VITREO", advapi32 SecurityStubs::USERNAME "VitUser"); the cross-check test
// SspiGetUserNameExTest.MatchesGetComputerNameAndGetUserName keeps them equal.
constexpr char kSamCompatible[] = "VITREO\\VitUser";

// Live LsaConnectUntrusted handles. Values fit a 32-bit HANDLE.
struct LsaState {
    std::mutex mtx;
    std::unordered_set<u64> handles;
    u64 next{0x4C534101ull};
};
LsaState& lsa() {
    static LsaState s;
    return s;
}
bool lsa_handle_live(u64 h) {
    if (h == 0) return false;
    std::lock_guard<std::mutex> lock(lsa().mtx);
    return lsa().handles.count(h) != 0;
}

// Authentication packages Windows ships but Vitreo's LSA does not implement.
bool is_windows_lsa_package(const std::string& name) {
    static const char* kNames[] = {
        "MICROSOFT_AUTHENTICATION_PACKAGE_V1_0", "Kerberos", "Negotiate",
        "NTLM", "Schannel", "Microsoft Unified Security Protocol Provider",
        "Default TLS SSP", "CloudAP", "pku2u", "WDigest", "TSSSP", "CREDSSP",
        "Negotiate Extender",
    };
    for (const char* n : kNames) {
        if (name.size() != std::char_traits<char>::length(n)) continue;
        bool eq = true;
        for (usize i = 0; i < name.size() && eq; ++i)
            eq = std::tolower(static_cast<unsigned char>(name[i])) ==
                 std::tolower(static_cast<unsigned char>(n[i]));
        if (eq) return true;
    }
    return false;
}

// Read an LSA_STRING { USHORT Length; USHORT MaximumLength; PCHAR Buffer; }.
std::string read_lsa_string(Marshaler& m, GuestAddr s) {
    if (s == 0) return {};
    const u16 len = m.read_u16(s).value_or(0);
    const GuestAddr buf = m.read_ptr(s + (m.cpu_is_64bit() ? 8 : 4)).value_or(0);
    std::string out;
    for (u16 i = 0; i < len && buf != 0; ++i)
        out.push_back(static_cast<char>(m.read_u8(buf + i).value_or(0)));
    return out;
}

// ---------------------------------------------------------------------------
// GetUserNameExW/A (secext.h). BOOLEAN (EXTENDED_NAME_FORMAT, LPTSTR, PULONG)
// ---------------------------------------------------------------------------

void get_user_name_ex(Marshaler& m, bool wide) {
    const u32 format = static_cast<u32>(m.arg(0));
    const GuestAddr buf = m.arg_ptr(1);
    const GuestAddr size_ptr = m.arg_ptr(2);
    if (size_ptr == 0) {
        // Windows faults reading *nSize (probe nullh=5: 0xC0000005).
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    const u32 n_in = m.read_u32(size_ptr).value_or(0);

    // GetUserNameExA reports every failure's size in bytes of the UTF-16
    // name: twice what the wide call reports, including the untouched nSize
    // of a NONE_MAPPED/INVALID_PARAMETER failure (probe: 600 -> 1200).
    auto fail = [&](u32 err, u32 wide_size) {
        (void)m.write_u32(size_ptr, wide ? wide_size : wide_size * 2);
        set_last_error(err);
        m.set_return(0);
    };

    if (format > kNameFormatLast) {
        fail(error_codes::ERROR_INVALID_PARAMETER, n_in);
        return;
    }
    // Learn: "If the user account is not in a domain, only NameSamCompatible
    // is supported." Every other format fails with ERROR_NONE_MAPPED and
    // leaves the buffer untouched.
    if (format != kNameSamCompatible) {
        fail(error_codes::ERROR_NONE_MAPPED, n_in);
        return;
    }

    const u32 len = static_cast<u32>(std::char_traits<char>::length(kSamCompatible));
    if (n_in <= len) {
        // The wide call copies the name when exactly len chars fit (no room
        // for the NUL) and still fails; the ANSI call never writes on failure.
        if (wide && n_in == len && buf != 0) {
            for (u32 i = 0; i < len; ++i)
                (void)m.write_u16(buf + i * 2, static_cast<u8>(kSamCompatible[i]));
        }
        fail(error_codes::ERROR_MORE_DATA, len + 1);
        return;
    }
    if (buf == 0) {
        // Windows faults writing through a NULL buffer that claims room.
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    const auto wr = wide ? m.write_wide_string(buf, kSamCompatible)
                         : m.write_ansi_string(buf, kSamCompatible);
    if (wr.is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    // Success: nSize = characters copied, excluding the NUL; last error is
    // left untouched.
    (void)m.write_u32(size_ptr, len);
    m.set_return(1);
}

// ---------------------------------------------------------------------------
// LSA (ntsecapi.h)
// ---------------------------------------------------------------------------

// NTSTATUS LsaConnectUntrusted(PHANDLE LsaHandle)
void lsa_connect_untrusted(Marshaler& m) {
    const GuestAddr out = m.arg_ptr(0);
    if (out == 0) { m.set_return(RPC_NT_NULL_REF_POINTER); return; }
    u64 h = 0;
    {
        std::lock_guard<std::mutex> lock(lsa().mtx);
        h = lsa().next++;
        lsa().handles.insert(h);
    }
    // HANDLE is pointer-sized: the x64 write covers all 8 bytes.
    if (m.write_ptr(out, h).is_err()) {
        std::lock_guard<std::mutex> lock(lsa().mtx);
        lsa().handles.erase(h);
        m.set_return(RPC_NT_NULL_REF_POINTER);
        return;
    }
    m.set_return(STATUS_SUCCESS);
}

// NTSTATUS LsaLookupAuthenticationPackage(HANDLE, PLSA_STRING, PULONG)
void lsa_lookup_authentication_package(Marshaler& m) {
    const u64 h = m.arg(0);
    const GuestAddr name_ptr = m.arg_ptr(1);
    const GuestAddr id_ptr = m.arg_ptr(2);
    if (!lsa_handle_live(h)) { m.set_return(STATUS_INVALID_CONNECTION); return; }
    const std::string name = read_lsa_string(m, name_ptr);
    if (is_windows_lsa_package(name)) {
        sspi_unimplemented_once("sspicli!LsaLookupAuthenticationPackage: package " + name +
                                " (Vitreo's LSA has no authentication packages)");
    }
    // Windows writes 0xFFFFFFFF for a package it does not have.
    if (id_ptr != 0) (void)m.write_u32(id_ptr, 0xFFFFFFFFu);
    m.set_return(STATUS_NO_SUCH_PACKAGE);
}

// NTSTATUS LsaCallAuthenticationPackage(HANDLE, ULONG, PVOID, ULONG, PVOID*,
//                                       PULONG, PNTSTATUS)
void lsa_call_authentication_package(Marshaler& m) {
    const u64 h = m.arg(0);
    (void)m.arg(1); (void)m.arg(2); (void)m.arg(3);
    const GuestAddr ret = m.arg_ptr(4);
    const GuestAddr ret_len = m.arg_ptr(5);
    const GuestAddr status = m.arg_ptr(6);
    if (!lsa_handle_live(h)) { m.set_return(STATUS_INVALID_CONNECTION); return; }
    // No package id is valid (no packages): Windows' answer for an unknown id
    // clears all three outputs.
    if (ret != 0) (void)m.write_ptr(ret, 0);
    if (ret_len != 0) (void)m.write_u32(ret_len, 0);
    if (status != 0) (void)m.write_u32(status, 0);
    m.set_return(STATUS_NO_SUCH_PACKAGE);
}

// NTSTATUS LsaLogonUser(HANDLE, PLSA_STRING, SECURITY_LOGON_TYPE, ULONG,
//   PVOID, ULONG, PTOKEN_GROUPS, PTOKEN_SOURCE, PVOID*, PULONG, PLUID,
//   PHANDLE, PQUOTA_LIMITS, PNTSTATUS)        -- 14 parameters, 56 bytes x86
void lsa_logon_user(Marshaler& m) {
    const u64 h = m.arg(0);
    for (u32 i = 1; i < 9; ++i) (void)m.arg(i);
    const GuestAddr profile_len = m.arg_ptr(9);
    (void)m.arg(10); (void)m.arg(11); (void)m.arg(12);
    const GuestAddr sub_status = m.arg_ptr(13);
    if (h == 0) { m.set_return(RPC_NT_SS_IN_NULL_CONTEXT); return; }
    if (!lsa_handle_live(h)) { m.set_return(STATUS_INVALID_HANDLE); return; }
    // Every AuthenticationPackage id is unknown to Vitreo's LSA. Windows
    // answers an unknown id with STATUS_NOT_SUPPORTED, zeroes
    // *ProfileBufferLength and *SubStatus and leaves the other outputs.
    if (profile_len != 0) (void)m.write_u32(profile_len, 0);
    if (sub_status != 0) (void)m.write_u32(sub_status, 0);
    m.set_return(STATUS_NOT_SUPPORTED);
}

// NTSTATUS LsaFreeReturnBuffer(PVOID)
void lsa_free_return_buffer(Marshaler& m) {
    const GuestAddr p = m.arg_ptr(0);
    // Vitreo's LSA never hands out a return buffer; NULL is measured.
    m.set_return(p == 0 ? STATUS_MEMORY_NOT_ALLOCATED : STATUS_SUCCESS);
}

// NTSTATUS LsaDeregisterLogonProcess(HANDLE)
void lsa_deregister_logon_process(Marshaler& m) {
    const u64 h = m.arg(0);
    std::lock_guard<std::mutex> lock(lsa().mtx);
    // Windows faults on NULL; an unknown handle is not a connection.
    m.set_return(lsa().handles.erase(h) != 0 ? STATUS_SUCCESS : STATUS_INVALID_HANDLE);
}

// ---------------------------------------------------------------------------
// GetComputerObjectNameW/A (secext.h). BOOLEAN (EXTENDED_NAME_FORMAT, LPTSTR,
// PULONG). Learn: "It cannot be NameUnknown." Every format names the
// computer's directory object, and a workgroup computer has none: measured
// (userenv_computer_object_probe.c, x64 and x86) NameUnknown fails with
// ERROR_INVALID_PARAMETER and every other value -- NameSamCompatible and
// out-of-range ones included -- with ERROR_CANT_ACCESS_DOMAIN_INFO. The
// buffer is never written; W leaves nSize alone, A doubles it, as
// GetUserNameExA does.
// ---------------------------------------------------------------------------

void get_computer_object_name(Marshaler& m, bool wide) {
    const u32 format = static_cast<u32>(m.arg(0));
    (void)m.arg_ptr(1);
    const GuestAddr size_ptr = m.arg_ptr(2);
    if (size_ptr == 0) {
        // As GetUserNameEx: Windows faults reading *nSize.
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    if (!wide) {
        const u32 n_in = m.read_u32(size_ptr).value_or(0);
        (void)m.write_u32(size_ptr, n_in * 2);
    }
    set_last_error(format == 0 ? error_codes::ERROR_INVALID_PARAMETER
                               : error_codes::ERROR_CANT_ACCESS_DOMAIN_INFO);
    m.set_return(0);
}

} // namespace

const char* sspi_sam_compatible_name() { return kSamCompatible; }

void sspi_unimplemented_once(const std::string& what) {
    static std::mutex mtx;
    static std::unordered_set<std::string> seen;
    std::lock_guard<std::mutex> lock(mtx);
    if (!seen.insert(what).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what.c_str());
}

void append_sspicli_forwarded_exports(DllExports& exports) {
    exports.entries.push_back(ExportEntry{"GetUserNameExA", 0,
        [](Marshaler& m) { get_user_name_ex(m, false); }, 12});
    exports.entries.push_back(ExportEntry{"GetUserNameExW", 0,
        [](Marshaler& m) { get_user_name_ex(m, true); }, 12});
    exports.entries.push_back(ExportEntry{"LsaConnectUntrusted", 0,
        [](Marshaler& m) { lsa_connect_untrusted(m); }, 4});
    exports.entries.push_back(ExportEntry{"LsaLookupAuthenticationPackage", 0,
        [](Marshaler& m) { lsa_lookup_authentication_package(m); }, 12});
    exports.entries.push_back(ExportEntry{"LsaCallAuthenticationPackage", 0,
        [](Marshaler& m) { lsa_call_authentication_package(m); }, 28});
    exports.entries.push_back(ExportEntry{"LsaLogonUser", 0,
        [](Marshaler& m) { lsa_logon_user(m); }, 56});
    exports.entries.push_back(ExportEntry{"LsaFreeReturnBuffer", 0,
        [](Marshaler& m) { lsa_free_return_buffer(m); }, 4});
    exports.entries.push_back(ExportEntry{"LsaDeregisterLogonProcess", 0,
        [](Marshaler& m) { lsa_deregister_logon_process(m); }, 4});
}

void append_secur32_computer_object_name_exports(DllExports& exports) {
    exports.entries.push_back(ExportEntry{"GetComputerObjectNameA", 0,
        [](Marshaler& m) { get_computer_object_name(m, false); }, 12});
    exports.entries.push_back(ExportEntry{"GetComputerObjectNameW", 0,
        [](Marshaler& m) { get_computer_object_name(m, true); }, 12});
}

DllExports create_sspicli_thunks(SspiCliManager& /*mgr*/) {
    DllExports exports;
    exports.entries.reserve(8);
    append_sspicli_forwarded_exports(exports);
    return exports;
}

} // namespace vitreo::winapi

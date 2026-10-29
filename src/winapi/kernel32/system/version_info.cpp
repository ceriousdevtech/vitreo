// SPDX-License-Identifier: LGPL-2.1-only

/// @file version_info.cpp
/// @brief VerifyVersionInfo and GetLogicalProcessorInformation*.
///
/// Version and processor-topology queries that CPU-Z and installers probe.
///
/// Closed cut from kernel32_ext.cpp — verified closed with
/// tools/architecture/plan_extraction.py before moving.

#include "winapi/kernel32/system/processor_topology.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/system/ntdll/version_verify.hpp"
#include "winapi/kernel32/system/sysinfo.hpp"
#include "core/pe/os_version.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// =============================================================================
// Version
// =============================================================================

namespace {

// OSVERSIONINFOEXW / OSVERSIONINFOEXA field offsets (winnt.h): the A form
// has a 128-byte szCSDVersion, the W form 128 WCHARs.
struct OsVersionInfoExLayout {
    u32 service_pack_major;
    u32 service_pack_minor;
    u32 suite_mask;
    u32 product_type;
};
constexpr OsVersionInfoExLayout kOsVersionInfoExW{276, 278, 280, 282};
constexpr OsVersionInfoExLayout kOsVersionInfoExA{148, 150, 152, 154};

version_verify::VersionFields read_requested_version(
    Marshaler& m, GuestAddr info, const OsVersionInfoExLayout& layout) {
    version_verify::VersionFields v;
    v.major = m.read_u32(info + 4).value_or(0);
    v.minor = m.read_u32(info + 8).value_or(0);
    v.build = m.read_u32(info + 12).value_or(0);
    v.platform_id = m.read_u32(info + 16).value_or(0);
    v.service_pack_major = m.read_u16(info + layout.service_pack_major).value_or(0);
    v.service_pack_minor = m.read_u16(info + layout.service_pack_minor).value_or(0);
    v.suite_mask = m.read_u16(info + layout.suite_mask).value_or(0);
    v.product_type = m.read_u8(info + layout.product_type).value_or(0);
    return v;
}

}  // namespace

// BOOL VerifyVersionInfo{W,A}(LPOSVERSIONINFOEX lpVersionInformation,
//   DWORD dwTypeMask, DWORDLONG dwlConditionMask)
// Learn: compares against the version the process sees, so a process
// without a Windows 10 manifest is compared against GetVersionEx's 6.2.9200
// (verify_version_info_probe "lie" cases). FALSE + ERROR_OLD_WIN_VERSION
// when the requirements are not met, FALSE + ERROR_BAD_ARGUMENTS when
// RtlVerifyVersionInfo rejects the arguments; success leaves the last
// error alone.
void Kernel32ExtManager::verify_version_info(Marshaler& m, bool wide) {
    const GuestAddr info = m.arg_ptr(0);
    if (info == 0) {
        set_last_error(error_codes::ERROR_BAD_ARGUMENTS);
        m.set_return(0);
        return;
    }
    const u32 type_mask = static_cast<u32>(m.arg(1));
    const u64 condition_mask =
        m.cpu_is_64bit()
            ? m.arg(2)
            : (static_cast<u64>(static_cast<u32>(m.arg(3))) << 32) |
                  static_cast<u32>(m.arg(2));
    OsVersionInfo seen;
    if (m_sysinfo_manager == nullptr ||
        !m_sysinfo_manager->get_version_ex(wide ? 284u : 156u, seen)) {
        // Wiring error: there is no process version to compare against.
        static std::once_flag once;
        std::call_once(once, [] {
            std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!VerifyVersionInfo: "
                                 "no SysInfoManager attached\n");
        });
        set_last_error(error_codes::ERROR_NOT_SUPPORTED);
        m.set_return(0);
        return;
    }
    version_verify::VersionFields current;
    current.major = seen.major_version;
    current.minor = seen.minor_version;
    current.build = seen.build_number;
    current.platform_id = seen.platform_id;
    current.service_pack_major = pe::kOsServicePackMajor;
    current.service_pack_minor = pe::kOsServicePackMinor;
    current.suite_mask = pe::kOsSuiteMask;
    current.product_type = pe::kOsProductType;
    const auto requested = read_requested_version(
        m, info, wide ? kOsVersionInfoExW : kOsVersionInfoExA);
    switch (version_verify::verify(current, requested, type_mask, condition_mask)) {
    case version_verify::Outcome::Satisfied:
        m.set_return(1);
        return;
    case version_verify::Outcome::Mismatch:
        set_last_error(error_codes::ERROR_OLD_WIN_VERSION);
        break;
    case version_verify::Outcome::InvalidParameter:
        set_last_error(error_codes::ERROR_BAD_ARGUMENTS);
        break;
    }
    m.set_return(0);
}

void Kernel32ExtManager::impl_VerifyVersionInfoW(Marshaler& m) {
    verify_version_info(m, /*wide=*/true);
}

void Kernel32ExtManager::impl_VerifyVersionInfoA(Marshaler& m) {
    verify_version_info(m, /*wide=*/false);
}

void Kernel32ExtManager::impl_VerSetConditionMask(Marshaler& m) {
    // ULONGLONG VerSetConditionMask(ULONGLONG ConditionMask, DWORD TypeMask,
    //   BYTE Condition). x86 passes the mask as two stack DWORDs and returns
    // it in EDX:EAX; x64 passes it in RCX. Encoding: version_verify.hpp.
    const bool long_mode = m.cpu_is_64bit();
    const u64 in_mask =
        long_mode ? m.arg(0)
                  : (static_cast<u64>(static_cast<u32>(m.arg(1))) << 32) |
                        static_cast<u32>(m.arg(0));
    const u32 type_mask = static_cast<u32>(m.arg(long_mode ? 1 : 2));
    const u32 condition = static_cast<u32>(m.arg(long_mode ? 2 : 3)) & 0xFFu;
    m.set_return_i64(version_verify::set_condition_mask(
        in_mask, type_mask, condition, long_mode));
}

void Kernel32ExtManager::impl_GetLogicalProcessorInformationEx(Marshaler& m) {
    // BOOL GetLogicalProcessorInformationEx(LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    //   PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Buffer, PDWORD ReturnedLength)
    // Measured (processor_topology_probe "glpiex"): every relationship 0-7 and
    // RelationAll is answered; any other fails with ERROR_GEN_FAILURE. A NULL
    // or short buffer fails with ERROR_INSUFFICIENT_BUFFER and the required
    // length; success writes the length and leaves the last error alone.
    const u32 relationship = static_cast<u32>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const GuestAddr length_at = m.arg_ptr(2);
    VITREO_TRACE("[kernel32_ext] GetLogicalProcessorInformationEx(rel=%u, buf=0x%llx)\n", relationship,
                 static_cast<unsigned long long>(buffer));
    if (length_at == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const auto records = processor_topology::logical_processor_information_ex(relationship, m.cpu_is_64bit());
    if (!records) {
        set_last_error(error_codes::ERROR_GEN_FAILURE);
        m.set_return(0);
        return;
    }
    const auto capacity = m.read_u32(length_at);
    const u32 required = static_cast<u32>(records->size());
    if (buffer == 0 || capacity.is_err() || capacity.value() < required) {
        (void)m.write_u32(length_at, required);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    if (m_memory.write_bytes(buffer, reinterpret_cast<const Byte*>(records->data()), records->size()).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
        return;
    }
    (void)m.write_u32(length_at, required);
    m.set_return(1);
}

} // namespace vitreo::winapi

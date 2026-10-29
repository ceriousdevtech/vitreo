// SPDX-License-Identifier: LGPL-2.1-only

/// @file crt_probes.cpp
/// @brief APIs the MSVC CRT probes via GetProcAddress at startup.
///
/// Closed cut from kernel32_ext.cpp — verified with plan_extraction.py.

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
#include "winapi/kernel32/synchronization/srw_word.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
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

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
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
// APIs probed by MSVC CRT at startup via GetProcAddress
// =============================================================================

void Kernel32ExtManager::impl_AreFileApisANSI(Marshaler& m) {
    // BOOL AreFileApisANSI(void)
    // TRUE while the ANSI file APIs translate through the ANSI code page,
    // FALSE after SetFileApisToOEM. ANSI is the process default.
    m.set_return(m_file_apis_ansi.load(std::memory_order_relaxed) ? 1u : 0u);
}

void Kernel32ExtManager::impl_SetFileApisToANSI(Marshaler& m) {
    // void SetFileApisToANSI(void)
    m_file_apis_ansi.store(true, std::memory_order_relaxed);
    m.set_return(0);
}

void Kernel32ExtManager::impl_SetFileApisToOEM(Marshaler& m) {
    // void SetFileApisToOEM(void)
    //
    // The flag is recorded so AreFileApisANSI answers truthfully; the
    // translation itself is unaffected because Vitreo's ANSI<->UTF-16 path
    // already maps CP_OEMCP onto the same CP-1252 table as CP_ACP (see
    // locale/string_conv.cpp). A guest that flips to OEM and reads the flag
    // back must not be told it is still in ANSI mode.
    m_file_apis_ansi.store(false, std::memory_order_relaxed);
    m.set_return(0);
}

void Kernel32ExtManager::impl_GetCurrentPackageId(Marshaler& m) {
    // LONG GetCurrentPackageId(UINT32* bufferLength, BYTE* buffer)
    // Non-packaged desktop apps return APPMODEL_ERROR_NO_PACKAGE (15700).
    (void)m.arg(0);
    (void)m.arg(1);
    m.set_return(15700u);
}

void Kernel32ExtManager::impl_GetCurrentPackageFullName(Marshaler& m) {
    // LONG GetCurrentPackageFullName(UINT32* packageFullNameLength,
    //                                PWSTR packageFullName)
    // Vitreo does not model MSIX/AppX process identity. Every process is an
    // unpackaged desktop process, for which Learn specifies
    // APPMODEL_ERROR_NO_PACKAGE. The length and optional output buffer are not
    // package data and must not be fabricated on that failure path.
    constexpr u32 APPMODEL_ERROR_NO_PACKAGE = 15700;
    const GuestAddr length = m.arg_ptr(0);
    (void)m.arg_ptr(1);
    if (length == 0 || m_memory.read_u32(length).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    ipc::ipc_log("GetCurrentPackageFullName -> APPMODEL_ERROR_NO_PACKAGE");
    m.set_return(APPMODEL_ERROR_NO_PACKAGE);
}

void Kernel32ExtManager::impl_GetPackageFamilyName(Marshaler& m) {
    // LONG GetPackageFamilyName(HANDLE, UINT32* length, PWSTR name)
    // Learn appmodel.h: unpackaged process → APPMODEL_ERROR_NO_PACKAGE.
    constexpr u32 APPMODEL_ERROR_NO_PACKAGE = 15700;
    (void)m.arg(0);
    const GuestAddr length = m.arg_ptr(1);
    (void)m.arg_ptr(2);
    if (length == 0 || m_memory.read_u32(length).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(APPMODEL_ERROR_NO_PACKAGE);
}

void Kernel32ExtManager::impl_GetPackagePathByFullName(Marshaler& m) {
    // LONG GetPackagePathByFullName(PCWSTR packageFullName, UINT32* pathLength,
    //                               PWSTR path)
    // Learn (appmodel.h): succeeds with ERROR_SUCCESS when the named package
    // exists. Vitreo has no AppX package store, so any well-formed name is
    // absent → ERROR_NOT_FOUND (no fabricated install path).
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr length_ptr = m.arg_ptr(1);
    (void)m.arg_ptr(2);
    if (name_ptr == 0 || length_ptr == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (m_memory.read_u32(length_ptr).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    const auto name = m.read_wide_string(name_ptr, 512);
    if (name.is_err() || name.value().empty()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    ipc::ipc_log("GetPackagePathByFullName name='%s' -> ERROR_NOT_FOUND",
                 name.value().c_str());
    m.set_return(error_codes::ERROR_NOT_FOUND);
}

void Kernel32ExtManager::impl_CompareObjectHandles(Marshaler& m) {
    // BOOL CompareObjectHandles(HANDLE first, HANDLE second)
    // Learn (handleapi.h): TRUE when both refer to the same kernel object.
    const Handle first = static_cast<Handle>(m.arg(0));
    const Handle second = static_cast<Handle>(m.arg(1));
    if (m_handle_table == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (first == second && first != NULL_HANDLE) {
        // Same handle value always names the same object when valid, or for
        // NT pseudo-handles (-1/-2) which are process-scoped constants.
        if (first >= 0xFFFFFFFAu || m_handle_table->is_valid(first)) {
            m.set_return(1);
            return;
        }
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    // Pseudo-handle vs real process/thread: current-process (-1) matches a
    // Process handle whose pid is this host process.
    constexpr Handle kCurrentProcess = static_cast<Handle>(-1);
    constexpr Handle kCurrentThread = static_cast<Handle>(-2);
    auto same_process_pseudo = [&](Handle pseudo, Handle other) -> bool {
        if (pseudo != kCurrentProcess) return false;
        if (m_handle_table->get_type(other) != HandleType::Process) return false;
        auto* proc = static_cast<ipc::ProcessObject*>(
            m_handle_table->get(other, HandleType::Process));
        return proc != nullptr && proc->pid == ::getpid();
    };
    if (same_process_pseudo(first, second) ||
        same_process_pseudo(second, first)) {
        m.set_return(1);
        return;
    }
    if (first == kCurrentThread || second == kCurrentThread) {
        // Thread pseudo-handle equality with a real Thread handle is not
        // modeled yet (no current-thread object identity). Honest miss.
        set_last_error(error_codes::ERROR_NOT_SAME_OBJECT);
        m.set_return(0);
        return;
    }

    const HandleType t1 = m_handle_table->get_type(first);
    const HandleType t2 = m_handle_table->get_type(second);
    if (t1 == HandleType::Invalid || t2 == HandleType::Invalid) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (t1 != t2) {
        set_last_error(error_codes::ERROR_NOT_SAME_OBJECT);
        m.set_return(0);
        return;
    }
    void* o1 = m_handle_table->get(first, t1);
    void* o2 = m_handle_table->get(second, t2);
    if (o1 != nullptr && o1 == o2) {
        m.set_return(1);
        return;
    }
    set_last_error(error_codes::ERROR_NOT_SAME_OBJECT);
    m.set_return(0);
}

void Kernel32ExtManager::impl_GetPackagesByPackageFamily(Marshaler& m) {
    // LONG GetPackagesByPackageFamily(PCWSTR family, UINT32* count,
    //   PWSTR* fullNames, UINT32* bufferLength, WCHAR* buffer)
    // The capsule has no AppX package repository. A valid family therefore
    // has an empty result set for the current user. Preserve the documented
    // two-call output shape by returning zero required entries/characters.
    const GuestAddr family_ptr = m.arg_ptr(0);
    const GuestAddr count_ptr = m.arg_ptr(1);
    (void)m.arg_ptr(2);
    const GuestAddr length_ptr = m.arg_ptr(3);
    (void)m.arg_ptr(4);
    if (family_ptr == 0 || count_ptr == 0 || length_ptr == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    const auto family = m.read_wide_string(family_ptr, 256);
    if (family.is_err() || family.value().empty()) {
        m.set_return(error_codes::ERROR_INVALID_NAME);
        return;
    }
    if (m.write_u32(count_ptr, 0).is_err() ||
        m.write_u32(length_ptr, 0).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    ipc::ipc_log("GetPackagesByPackageFamily family='%s' -> count=0",
                 family.value().c_str());
    m.set_return(error_codes::ERROR_SUCCESS);
}

void Kernel32ExtManager::impl_PackageFamilyNameFromId(Marshaler& m) {
    // LONG PackageFamilyNameFromId(const PACKAGE_ID* id, UINT32* length,
    //                              PWSTR familyName)
    const GuestAddr package_id = m.arg_ptr(0);
    const GuestAddr length_ptr = m.arg_ptr(1);
    const GuestAddr output = m.arg_ptr(2);
    if (package_id == 0 || length_ptr == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }

    // PACKAGE_ID keeps `name` at offset 16 in both ABIs. `publisherId` is the
    // fourth pointer: offset 28 in PE32 and 40 in PE32+.
    const auto name_ptr = m.read_ptr(package_id + 16);
    const auto publisher_id_ptr =
        m.read_ptr(package_id + (m.cpu_is_64bit() ? 40u : 28u));
    if (name_ptr.is_err() || publisher_id_ptr.is_err() ||
        name_ptr.value() == 0 || publisher_id_ptr.value() == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    const auto name = m.read_wide_string(name_ptr.value(), 51);
    const auto publisher_id =
        m.read_wide_string(publisher_id_ptr.value(), 14);
    if (name.is_err() || publisher_id.is_err() || name.value().empty() ||
        publisher_id.value().empty()) {
        m.set_return(error_codes::ERROR_INVALID_NAME);
        return;
    }

    const std::string family = name.value() + "_" + publisher_id.value();
    const u32 required = static_cast<u32>(family.size() + 1);
    const auto capacity = m.read_u32(length_ptr);
    if (capacity.is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (m.write_u32(length_ptr, required).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (output == 0 || capacity.value() < required) {
        m.set_return(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    if (m.write_wide_string(output, family).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(error_codes::ERROR_SUCCESS);
}

void Kernel32ExtManager::impl_PackageNameAndPublisherIdFromFamilyName(
    Marshaler& m) {
    // LONG PackageNameAndPublisherIdFromFamilyName(PCWSTR family,
    //   UINT32* nameLength, PWSTR name, UINT32* publisherLength,
    //   PWSTR publisherId)
    const GuestAddr family_ptr = m.arg_ptr(0);
    const GuestAddr name_length_ptr = m.arg_ptr(1);
    const GuestAddr name_output = m.arg_ptr(2);
    const GuestAddr publisher_length_ptr = m.arg_ptr(3);
    const GuestAddr publisher_output = m.arg_ptr(4);
    if (family_ptr == 0 || name_length_ptr == 0 ||
        publisher_length_ptr == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    const auto family = m.read_wide_string(family_ptr, 256);
    if (family.is_err()) {
        m.set_return(error_codes::ERROR_INVALID_NAME);
        return;
    }
    const usize separator = family.value().rfind('_');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= family.value().size()) {
        m.set_return(error_codes::ERROR_INVALID_NAME);
        return;
    }
    const std::string name = family.value().substr(0, separator);
    const std::string publisher_id = family.value().substr(separator + 1);
    const u32 required_name = static_cast<u32>(name.size() + 1);
    const u32 required_publisher = static_cast<u32>(publisher_id.size() + 1);
    const auto name_capacity = m.read_u32(name_length_ptr);
    const auto publisher_capacity = m.read_u32(publisher_length_ptr);
    if (name_capacity.is_err() || publisher_capacity.is_err() ||
        m.write_u32(name_length_ptr, required_name).is_err() ||
        m.write_u32(publisher_length_ptr, required_publisher).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (name_output == 0 || publisher_output == 0 ||
        name_capacity.value() < required_name ||
        publisher_capacity.value() < required_publisher) {
        m.set_return(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    if (m.write_wide_string(name_output, name).is_err() ||
        m.write_wide_string(publisher_output, publisher_id).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    m.set_return(error_codes::ERROR_SUCCESS);
}

void Kernel32ExtManager::impl_GetSystemTimePreciseAsFileTime(Marshaler& m) {
    // void GetSystemTimePreciseAsFileTime(LPFILETIME lpSystemTimeAsFileTime)
    GuestAddr ft_ptr = m.arg_ptr(0);
    constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;
    auto now = std::chrono::system_clock::now();
    auto se = now.time_since_epoch();
    auto hns = std::chrono::duration_cast<
        std::chrono::duration<u64, std::ratio<1, 10000000>>>(se);
    u64 ft = hns.count() + EPOCH_OFFSET;
    (void)m.write_u64(ft_ptr, ft);
}

void Kernel32ExtManager::impl_SleepConditionVariableCS(Marshaler& m) {
    // BOOL SleepConditionVariableCS(
    //   PCONDITION_VARIABLE ConditionVariable,
    //   PCRITICAL_SECTION   CriticalSection,
    //   DWORD               dwMilliseconds)
    //
    // EnterCriticalSection is owned by CriticalSectionManager (guest layout).
    // Sleeping must release *that* CS — SyncManager's parallel CS map is not
    // the lock the guest holds, and releasing the wrong one deadlocks the
    // UI thread in EnterCriticalSection.
    GuestAddr cv_addr = m.arg_ptr(0);
    GuestAddr cs_addr = m.arg_ptr(1);
    u32 milliseconds = static_cast<u32>(m.arg(2));

    if (m_sync_manager == nullptr) {
        m.set_return(1u);
        return;
    }

    if (m_critical_section_manager != nullptr && cs_addr != 0) {
        srw_word::Word cvw;
        if (srw_word::resolve(m_memory, cv_addr, m.cpu_is_64bit(), cvw)) {
            const u32 tid = ThreadManager::get_current_thread_id_static();
            auto& cs = *m_critical_section_manager;
            if (cs.owning_thread(cs_addr) != tid || cs.recursion_count(cs_addr) != 1) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const bool woken = srw_word::cv_sleep_with(
                cvw, milliseconds, m_sync_manager->shutdown_flag(),
                [&] { cs.leave(cs_addr); }, [&] { cs.enter(cs_addr); });
            if (!woken) set_last_error(error_codes::ERROR_TIMEOUT);
            m.set_return(woken ? 1u : 0u);
            return;
        }
        const bool ok = m_sync_manager->sleep_condition_variable_cs(
            *m_critical_section_manager, cv_addr, cs_addr, milliseconds);
        m.set_return(ok ? 1u : 0u);
        return;
    }

    bool ok = m_sync_manager->sleep_condition_variable_cs(cv_addr, cs_addr, milliseconds);
    m.set_return(ok ? 1u : 0u);
}

void Kernel32ExtManager::impl_GetTempPath2W(Marshaler& m) {
    // DWORD GetTempPath2W(DWORD nBufferLength, LPWSTR lpBuffer)
    // Identical to GetTempPathW — write the capsule temp path.
    u32 buf_len = static_cast<u32>(m.arg(0));
    GuestAddr buf_ptr = m.arg_ptr(1);
    const char* temp = "C:\\Users\\Default\\AppData\\Local\\Temp\\";
    u32 len = static_cast<u32>(std::strlen(temp));
    if (buf_len == 0 || buf_ptr == 0) {
        m.set_return(len + 1);
        return;
    }
    u32 to_write = (len < buf_len) ? len : buf_len - 1;
    write_wide_literal(buf_ptr, temp);
    m.set_return(to_write);
}

void Kernel32ExtManager::impl_AppPolicyGetProcessTerminationMethod(Marshaler& m) {
    // LONG AppPolicyGetProcessTerminationMethod(HANDLE processToken,
    //                                          AppPolicyProcessTerminationMethod* policy);
    // For non-packaged apps, returns S_OK (0) and sets *policy = 0 (normal termination).
    (void)m.arg(0); // processToken
    GuestAddr policy_ptr = m.arg_ptr(1);
    if (policy_ptr != 0) {
        (void)m.write_u32(policy_ptr, 0); // AppPolicyProcessTerminationMethod_ExitProcess
    }
    m.set_return(0u); // S_OK
}

void Kernel32ExtManager::impl_AppPolicyGetThreadInitializationType(
    Marshaler& m) {
    // LONG AppPolicyGetThreadInitializationType(
    //   HANDLE processToken, AppPolicyThreadInitializationType* policy)
    // Unpackaged desktop processes have no automatic WinRT thread
    // initialization policy. Validate that the caller supplied an actual token
    // before publishing AppPolicyThreadInitializationType_None.
    const Handle token = static_cast<Handle>(m.arg(0));
    const GuestAddr policy = m.arg_ptr(1);
    if (token == NULL_HANDLE || policy == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (m_handle_table == nullptr ||
        m_handle_table->get_type(token) != HandleType::Token) {
        m.set_return(error_codes::ERROR_INVALID_HANDLE);
        return;
    }
    if (m.write_u32(policy, 0).is_err()) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    ipc::ipc_log(
        "AppPolicyGetThreadInitializationType token=0x%x -> None", token);
    m.set_return(error_codes::ERROR_SUCCESS);
}

} // namespace vitreo::winapi

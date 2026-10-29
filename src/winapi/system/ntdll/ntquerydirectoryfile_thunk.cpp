// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_ext.cpp
/// @brief ntdll.dll extended function stubs (WIN32-086)

#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/system/ntdll/ntdll_internal.hpp"
#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "capsule/vfs/filesystem.hpp"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/time.h>

/// @file ntquerydirectoryfile_thunk.cpp
/// @brief NtQueryDirectoryFile Thunk
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtQueryDirectoryFile Thunk
// =============================================================================

void ntdll::impl_NtQueryDirectoryFile(Marshaler& m, NtdllExtManager& mgr) {
    // NTSTATUS NtQueryDirectoryFile(
    //   HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
    //   PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
    //   PVOID FileInformation, ULONG Length,
    //   FILE_INFORMATION_CLASS FileInformationClass,
    //   BOOLEAN ReturnSingleEntry, PUNICODE_STRING FileName,
    //   BOOLEAN RestartScan);
    const Handle handle = static_cast<Handle>(m.arg(0));
    const Handle event = static_cast<Handle>(m.arg(1));
    const GuestAddr apc_routine = m.arg_ptr(2);
    const GuestAddr apc_context = m.arg_ptr(3);
    const GuestAddr io_status = m.arg_ptr(4);
    const GuestAddr file_information = m.arg_ptr(5);
    const u32 length = static_cast<u32>(m.arg(6));
    const u32 information_class = static_cast<u32>(m.arg(7));
    // ReturnSingleEntry and RestartScan are BOOLEAN -- one byte. The Win64 ABI
    // only defines the low byte of the argument slot for a sub-register type;
    // the caller may leave anything in the rest, and MSVC does. Testing the
    // full 64-bit slot made both read TRUE on every call, so an enumeration
    // restarted each time and handed back "." forever. The first directory
    // scan never terminated and burned a core indefinitely.
    const bool return_single_entry = (m.arg(8) & 0xFFu) != 0;
    const GuestAddr file_name_ptr = m.arg_ptr(9);
    const bool restart_scan = (m.arg(10) & 0xFFu) != 0;
    const bool is_64bit = m.cpu_is_64bit();
    const usize io_status_size = is_64bit ? 16 : 8;

    const auto write_io_status = [&](u32 status, u32 information) {
        if (io_status == 0) return;
        (void)m.write_u32(io_status, status);
        if (is_64bit) {
            (void)m.memory().write_u64(io_status + 8, information);
        } else {
            (void)m.write_u32(io_status + 4, information);
        }
    };

    if (io_status == 0 || file_information == 0) {
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    if (m.memory().host_ptr_mut(io_status, io_status_size) == nullptr ||
        (length != 0 &&
         m.memory().host_ptr_mut(file_information, length) == nullptr)) {
        m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
        return;
    }

    const bool has_completion_parameters =
        event != NULL_HANDLE || apc_routine != 0;
    if (has_completion_parameters &&
        !mgr.is_asynchronous_file_handle(handle)) {
        write_io_status(nt_status::STATUS_INVALID_PARAMETER, 0);
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    if (apc_context != 0 && apc_routine == 0) {
        // NT permits this only when the file has an associated completion
        // object. NT-created VFS handles do not currently expose one.
        write_io_status(nt_status::STATUS_INVALID_PARAMETER, 0);
        m.set_return(nt_status::STATUS_INVALID_PARAMETER);
        return;
    }
    if (apc_routine != 0) {
        VITREO_TRACE(
            "[ntdll] NtQueryDirectoryFile APC completion unsupported "
            "handle=0x%x routine=0x%llx\n",
            handle, static_cast<unsigned long long>(apc_routine));
        write_io_status(nt_status::STATUS_NOT_IMPLEMENTED, 0);
        m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
        return;
    }

    SyncManager* sync = mgr.sync_manager();
    if (event != NULL_HANDLE) {
        if (sync == nullptr || !sync->reset_event(event)) {
            const u32 status = sync == nullptr
                ? nt_status::STATUS_NOT_IMPLEMENTED
                : nt_status::STATUS_INVALID_HANDLE;
            write_io_status(status, 0);
            m.set_return(status);
            return;
        }
    }

    std::optional<std::vector<u16>> file_name;
    // Learn (ZwQueryDirectoryFile): FileName is an input on EVERY call, and
    // supplying it restarts the enumeration with that expression. Reading it
    // only on the first query dropped the mask from every later call, so the
    // match expression became "everything" and the scan handed back its first
    // entry -- "." -- forever. A guest that probes a directory one name at a
    // time never advanced. A module resolver spun 3.9 million calls
    // on a single handle.
    if (file_name_ptr != 0) {
        const auto length_result = m.read_u16(file_name_ptr);
        const auto maximum_result = m.read_u16(file_name_ptr + 2);
        if (length_result.is_err() || maximum_result.is_err() ||
            (length_result.value() & 1u) != 0 ||
            length_result.value() > maximum_result.value()) {
            write_io_status(nt_status::STATUS_INVALID_PARAMETER, 0);
            m.set_return(nt_status::STATUS_INVALID_PARAMETER);
            return;
        }
        const GuestAddr buffer_address = is_64bit
            ? m.memory().read_u64(file_name_ptr + 8).value_or(0)
            : m.read_u32(file_name_ptr + 4).value_or(0);
        if (length_result.value() != 0 && buffer_address == 0) {
            write_io_status(nt_status::STATUS_INVALID_PARAMETER, 0);
            m.set_return(nt_status::STATUS_INVALID_PARAMETER);
            return;
        }
        file_name.emplace();
        file_name->reserve(length_result.value() / sizeof(u16));
        for (u32 offset = 0; offset < length_result.value();
             offset += sizeof(u16)) {
            const auto unit = m.read_u16(buffer_address + offset);
            if (unit.is_err()) {
                write_io_status(nt_status::STATUS_ACCESS_VIOLATION, 0);
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            file_name->push_back(unit.value());
        }
    }

    std::vector<u8> output(length, 0);
    const auto result = mgr.nt_query_directory_file(
        handle, output, information_class, return_single_entry,
        file_name, restart_scan);
    if (result.information != 0) {
        const auto write_result = m.memory().write_bytes(
            file_information, output.data(), result.information);
        if (write_result.is_err()) {
            write_io_status(nt_status::STATUS_ACCESS_VIOLATION, 0);
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
    }
    write_io_status(result.status, result.information);
    if (event != NULL_HANDLE) {
        (void)sync->set_event(event);
    }
    m.set_return(result.status);
}

} // namespace vitreo::winapi

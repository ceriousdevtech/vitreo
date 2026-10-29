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
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "capsule/vfs/filesystem.hpp"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <vector>
#include <unistd.h>
#include <sys/time.h>

/// @file ntreadfile_thunk.cpp
/// @brief NtReadFile Thunk
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtReadFile Thunk
// =============================================================================

void ntdll::impl_NtReadFile(Marshaler& m, NtdllExtManager& mgr) {
    // NTSTATUS NtReadFile(
    //   HANDLE           FileHandle,         // arg 0
    //   HANDLE           Event,              // arg 1 - ignored
    //   PIO_APC_ROUTINE  ApcRoutine,         // arg 2 - ignored
    //   PVOID            ApcContext,         // arg 3 - ignored
    //   PIO_STATUS_BLOCK IoStatusBlock,      // arg 4 - out
    //   PVOID            Buffer,             // arg 5
    //   ULONG            Length,             // arg 6
    //   PLARGE_INTEGER   ByteOffset,         // arg 7 - optional
    //   PULONG           Key                 // arg 8 - ignored
    // );

    const Handle file_handle        = static_cast<Handle>(m.arg(0));
    const Handle event              = static_cast<Handle>(m.arg(1));
    const GuestAddr apc_routine     = m.arg_ptr(2);
    const GuestAddr apc_context     = m.arg_ptr(3);
    const GuestAddr io_status_ptr   = m.arg_ptr(4);
    const GuestAddr buffer_addr     = m.arg_ptr(5);
    const u32 length                = static_cast<u32>(m.arg(6));
    const GuestAddr byte_offset_ptr = m.arg_ptr(7);
    // arg 8 (Key) ignored

    // Read byte offset if provided
    i64 byte_offset = -1;  // -1 = use current position
    if (byte_offset_ptr != 0) {
        auto lo_res = m.read_u32(byte_offset_ptr);
        auto hi_res = m.read_u32(byte_offset_ptr + 4);
        if (lo_res.is_ok() && hi_res.is_ok()) {
            byte_offset = static_cast<i64>(
                static_cast<u64>(lo_res.value()) |
                (static_cast<u64>(hi_res.value()) << 32)
            );
            // Special value 0xFFFFFFFFFFFFFFFF means use current position
            if (byte_offset == -1LL) {
                byte_offset = -1;
            }
        }
    }

    // Validate buffer
    if (buffer_addr == 0 && length > 0) {
        if (io_status_ptr != 0) {
            (void)m.write_u32(io_status_ptr, nt_status::STATUS_INVALID_PARAMETER);
            (void)m.write_ptr(
                io_status_ptr + (m.cpu_is_64bit() ? 8 : 4), 0);
        }
        m.set_return(static_cast<u64>(nt_status::STATUS_INVALID_PARAMETER));
        return;
    }

    // Read into host buffer, then copy to guest
    std::vector<u8> host_buf(length);
    u32 bytes_read = 0;
    u32 status = mgr.nt_read_file(file_handle, std::span<u8>(host_buf), byte_offset, bytes_read);

    // Copy to guest memory if successful
    if (status == nt_status::STATUS_SUCCESS && bytes_read > 0) {
        Byte* guest_ptr = m.memory().host_ptr_mut(buffer_addr, bytes_read);
        if (guest_ptr) {
            std::memcpy(guest_ptr, host_buf.data(), bytes_read);
        } else {
            auto wr = m.memory().write_bytes(buffer_addr,
                reinterpret_cast<const Byte*>(host_buf.data()), bytes_read);
            if (wr.is_err()) {
                status = nt_status::STATUS_ACCESS_DENIED;
                bytes_read = 0;
            }
        }
    }

    // Write IO_STATUS_BLOCK
    if (io_status_ptr != 0) {
        (void)m.write_u32(io_status_ptr, status);
        (void)m.write_ptr(
            io_status_ptr + (m.cpu_is_64bit() ? 8 : 4), bytes_read);
    }

    // Learn NtReadFile: Event is signaled when the read completes. A caller
    // that WaitForSingleObject's that event (or SleepEx for ApcRoutine) must
    // observe completion — ignoring these leaves the thread parked.
    if (event != 0 && mgr.sync_manager() != nullptr) {
        (void)mgr.sync_manager()->set_event(event);
    }
    if (apc_routine != 0 && mgr.thread_manager() != nullptr) {
        (void)mgr.thread_manager()->queue_nt_io_apc(
            apc_routine, apc_context, io_status_ptr);
    }

    VITREO_TRACE("[ntdll] NtReadFile: handle=0x%x len=%u offset=%lld -> status=0x%x read=%u\n",
                file_handle, length, static_cast<long long>(byte_offset), status, bytes_read);

    m.set_return(static_cast<u64>(status));
}

} // namespace vitreo::winapi

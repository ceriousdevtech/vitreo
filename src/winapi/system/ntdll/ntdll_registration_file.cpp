// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_file.cpp
/// @brief Extended ntdll export table, part 6 of 11: the Nt*/Zw* file calls.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include <cstdio>
#include <vector>

namespace vitreo::winapi {

using namespace ntdll;

void append_file_exports(NtdllExtManager& mgr,
                         DllExports& exports, u16& ordinal) {
    // NtCreateFile (11 args, stdcall_bytes=44)
    exports.entries.push_back(ExportEntry{
        "NtCreateFile", ordinal++,
        [&mgr](Marshaler& m) { impl_NtCreateFile(m, mgr); },
        44
    });

    // NtOpenFile (6 args, stdcall_bytes=24)
    exports.entries.push_back(ExportEntry{
        "NtOpenFile", ordinal++,
        [&mgr](Marshaler& m) { impl_NtOpenFile(m, mgr); },
        24
    });

    // NtQueryDirectoryFile (11 args, stdcall_bytes=44)
    exports.entries.push_back(ExportEntry{
        "NtQueryDirectoryFile", ordinal++,
        [&mgr](Marshaler& m) { impl_NtQueryDirectoryFile(m, mgr); },
        44
    });
    exports.entries.push_back(ExportEntry{
        "ZwQueryDirectoryFile", ordinal++,
        [&mgr](Marshaler& m) { impl_NtQueryDirectoryFile(m, mgr); },
        44
    });

    // NtReadFile (9 args, stdcall_bytes=36)
    exports.entries.push_back(ExportEntry{
        "NtReadFile", ordinal++,
        [&mgr](Marshaler& m) { impl_NtReadFile(m, mgr); },
        36
    });

    // NtWriteFile (9 args, stdcall_bytes=36)
    exports.entries.push_back(ExportEntry{
        "NtWriteFile", ordinal++,
        [&mgr](Marshaler& m) { impl_NtWriteFile(m, mgr); },
        36
    });

    // NtSetInformationFile (5 args, stdcall_bytes=20)
    // NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS)
    exports.entries.push_back(ExportEntry{
        "NtSetInformationFile", ordinal++,
        [&mgr](Marshaler& m) {
            const Handle file_handle   = static_cast<Handle>(m.arg(0));
            const GuestAddr io_status  = m.arg_ptr(1);
            const GuestAddr info_ptr   = m.arg_ptr(2);
            const u32 info_length      = static_cast<u32>(m.arg(3));
            const u32 info_class       = static_cast<u32>(m.arg(4));

            // Read the info buffer from guest memory
            std::vector<u8> buf(info_length);
            if (info_length > 0 && info_ptr != 0) {
                (void)m.memory().read_bytes(info_ptr, buf.data(), info_length);
            }

            u32 status = mgr.nt_set_information_file(file_handle, info_class,
                                                      buf.data(), info_length);

            if (io_status != 0) {
                (void)m.write_u32(io_status, status);
                (void)m.write_ptr(
                    io_status + (m.cpu_is_64bit() ? 8 : 4), 0);
            }

            std::fprintf(stderr, "[ntdll] NtSetInformationFile(handle=0x%x, class=%u, len=%u) -> 0x%x\n",
                         file_handle, info_class, info_length, status);

            m.set_return(static_cast<u64>(status));
        },
        20
    });

    // NtQueryInformationFile (5 args, stdcall_bytes=20)
    // NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS)
    exports.entries.push_back(ExportEntry{
        "NtQueryInformationFile", ordinal++,
        [&mgr](Marshaler& m) {
            const Handle file_handle   = static_cast<Handle>(m.arg(0));
            const GuestAddr io_status  = m.arg_ptr(1);
            const GuestAddr info_ptr   = m.arg_ptr(2);
            const u32 info_length      = static_cast<u32>(m.arg(3));
            const u32 info_class       = static_cast<u32>(m.arg(4));

            // Allocate buffer, call handler, write back to guest
            std::vector<u8> buf(info_length, 0);
            u32 produced = 0;
            u32 status = mgr.nt_query_information_file(file_handle, info_class,
                                                        buf.data(), info_length,
                                                        &produced);

            // STATUS_BUFFER_OVERFLOW is a warning, not an error: the fixed part
            // of the structure is filled in and the caller is expected to use
            // it. libuv's fs__stat_handle relies on exactly that.
            const bool wrote_data = status == nt_status::STATUS_SUCCESS ||
                                    status == nt_status::STATUS_BUFFER_OVERFLOW;
            if (wrote_data && info_length > 0 && info_ptr != 0) {
                (void)m.memory().write_bytes(info_ptr, buf.data(), info_length);
            }

            if (io_status != 0) {
                (void)m.write_u32(io_status, status);
                (void)m.write_ptr(
                    io_status + (m.cpu_is_64bit() ? 8 : 4),
                    wrote_data ? produced : 0);
            }

            std::fprintf(stderr, "[ntdll] NtQueryInformationFile(handle=0x%x, class=%u, len=%u) -> 0x%x\n",
                         file_handle, info_class, info_length, status);

            m.set_return(static_cast<u64>(status));
        },
        20
    });

    // NtDeviceIoControlFile (10 args, stdcall_bytes=40)
    // NTSTATUS NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event,
    //   PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
    //   PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
    //   PVOID InputBuffer, ULONG InputBufferLength,
    //   PVOID OutputBuffer, ULONG OutputBufferLength)
    //
    // libuv's uv_winapi_init() resolves this by name and calls uv_fatal_error
    // when GetProcAddress returns NULL. A missing export is
    // FAST_FAIL_FATAL_APP_EXIT at startup.
    exports.entries.push_back(ExportEntry{
        "NtDeviceIoControlFile", ordinal++,
        [&mgr](Marshaler& m) { ntdll::impl_NtDeviceIoControlFile(m, mgr); },
        40
    });
    exports.entries.push_back(ExportEntry{
        "ZwDeviceIoControlFile", ordinal++,
        [&mgr](Marshaler& m) { ntdll::impl_NtDeviceIoControlFile(m, mgr); },
        40
    });

    // NtQueryVolumeInformationFile (5 args, stdcall_bytes=20)
    // NTSTATUS NtQueryVolumeInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID,
    //                                       ULONG, FS_INFORMATION_CLASS)
    exports.entries.push_back(ExportEntry{
        "NtQueryVolumeInformationFile", ordinal++,
        [&mgr](Marshaler& m) { ntdll::impl_NtQueryVolumeInformationFile(m, mgr); },
        20
    });
    exports.entries.push_back(ExportEntry{
        "ZwQueryVolumeInformationFile", ordinal++,
        [&mgr](Marshaler& m) { ntdll::impl_NtQueryVolumeInformationFile(m, mgr); },
        20
    });
}

} // namespace vitreo::winapi

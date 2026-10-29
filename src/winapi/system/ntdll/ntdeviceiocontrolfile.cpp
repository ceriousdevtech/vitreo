// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdeviceiocontrolfile.cpp
/// @brief NtDeviceIoControlFile and NtQueryVolumeInformationFile.
///
/// Both are resolved by name in libuv's uv_winapi_init(), which calls
/// uv_fatal_error() the moment GetProcAddress returns NULL for one of them.
/// That is a __fastfail(FAST_FAIL_FATAL_APP_EXIT), so the process used to die
/// roughly 30k instructions in, before any of its own code ran, with nothing
/// on screen.

#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/system/ntdll/ntdll_internal.hpp"
#include "winapi/system/ntdll/ntdll.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"

#include <cstdio>

namespace vitreo::winapi {

namespace {

/// Write the IO_STATUS_BLOCK a synchronous NT file call completes.
/// The block is { NTSTATUS Status (union with PVOID Pointer); ULONG_PTR
/// Information; }, so the Information field is pointer-sized and its offset
/// follows the pointer-sized first member.
void write_io_status(Marshaler& m, GuestAddr io_status, u32 status,
                     u64 information) {
    if (io_status == 0) return;
    (void)m.write_u32(io_status, status);
    (void)m.write_ptr(io_status + (m.cpu_is_64bit() ? 8 : 4), information);
}

/// NTSTATUS for a device request the capsule cannot carry out. Windows reports
/// the driver's status here; Vitreo has no driver, and STATUS_NOT_SUPPORTED is
/// the answer callers already handle (libuv maps it to UV_ENOSYS).
constexpr u32 kUnsupportedIoctl = nt_status::STATUS_NOT_SUPPORTED;

} // namespace

void ntdll::impl_NtDeviceIoControlFile(Marshaler& m, NtdllExtManager& mgr) {
    // NTSTATUS NtDeviceIoControlFile(
    //   HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
    //   PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock,
    //   ULONG IoControlCode, PVOID InputBuffer, ULONG InputBufferLength,
    //   PVOID OutputBuffer, ULONG OutputBufferLength)
    const Handle file_handle  = static_cast<Handle>(m.arg(0));
    const Handle event        = static_cast<Handle>(m.arg(1));
    const GuestAddr apc       = m.arg_ptr(2);
    (void)m.arg_ptr(3);                       // ApcContext
    const GuestAddr io_status = m.arg_ptr(4);
    const u32 control_code    = static_cast<u32>(m.arg(5));
    const GuestAddr input     = m.arg_ptr(6);
    const u32 input_length    = static_cast<u32>(m.arg(7));
    const GuestAddr output    = m.arg_ptr(8);
    const u32 output_length   = static_cast<u32>(m.arg(9));

    // A socket handle is a handle to the AFD driver; its requests complete
    // through the Winsock layer (Event, ApcRoutine, completion port).
    if (const auto& sockets = mgr.socket_device_control(); sockets) {
        if (const auto status = sockets(m, file_handle, control_code, input, input_length, output,
                                        output_length, io_status, m.arg_ptr(3), event, apc)) {
            VITREO_TRACE("[ntdll] NtDeviceIoControlFile(socket=0x%x, code=0x%08x, in=%u) -> 0x%x\n",
                         file_handle, control_code, input_length, *status);
            m.set_return(static_cast<u64>(*status));
            return;
        }
    }

    FileManager* files = mgr.file_manager();
    if (files == nullptr) {
        write_io_status(m, io_status, nt_status::STATUS_INVALID_HANDLE, 0);
        m.set_return(nt_status::STATUS_INVALID_HANDLE);
        return;
    }

    // The kernel32 DeviceIoControl path is the same device dispatch; going
    // through it keeps one implementation of every IOCTL Vitreo serves.
    u32 transferred = 0;
    const bool ok = files->device_io_control(
        file_handle, control_code, input, input_length, output, output_length,
        transferred);

    u32 status = nt_status::STATUS_SUCCESS;
    if (!ok) {
        // device_io_control reports a Win32 error; the only distinction a
        // caller acts on is "bad handle" versus "this device cannot do that".
        status = get_last_error() == error_codes::ERROR_INVALID_HANDLE
                     ? nt_status::STATUS_INVALID_HANDLE
                     : kUnsupportedIoctl;
        transferred = 0;
    }

    write_io_status(m, io_status, status, transferred);

    // A synchronous completion still signals the caller's event; the APC is
    // only queued for a pending request, and nothing here ever pends.
    if (status == nt_status::STATUS_SUCCESS && event != NULL_HANDLE &&
        mgr.sync_manager() != nullptr) {
        (void)mgr.sync_manager()->set_event(event);
    }
    (void)apc;

    VITREO_TRACE("[ntdll] NtDeviceIoControlFile(handle=0x%x, code=0x%08x, "
                 "in=%u, out=%u) -> 0x%x bytes=%u\n",
                 file_handle, control_code, input_length, output_length,
                 status, transferred);
    m.set_return(static_cast<u64>(status));
}

void ntdll::impl_NtQueryVolumeInformationFile(Marshaler& m,
                                              NtdllExtManager& mgr) {
    // NTSTATUS NtQueryVolumeInformationFile(HANDLE FileHandle,
    //   PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation, ULONG Length,
    //   FS_INFORMATION_CLASS FsInformationClass)
    const Handle file_handle  = static_cast<Handle>(m.arg(0));
    const GuestAddr io_status = m.arg_ptr(1);
    const GuestAddr info      = m.arg_ptr(2);
    const u32 length          = static_cast<u32>(m.arg(3));
    const u32 info_class      = static_cast<u32>(m.arg(4));

    constexpr u32 FileFsDeviceInformation = 4;

    // FILE_DEVICE_* from winioctl.h. libuv's uv_guess_handle() switches on
    // exactly these to decide whether a descriptor is a file, a pipe or a tty,
    // so reporting one type for every handle would misclassify stdio.
    constexpr u32 FILE_DEVICE_CD_ROM     = 0x00000002;
    constexpr u32 FILE_DEVICE_DISK       = 0x00000007;
    constexpr u32 FILE_DEVICE_NAMED_PIPE = 0x00000011;
    constexpr u32 FILE_DEVICE_UNKNOWN    = 0x00000022;
    constexpr u32 FILE_DEVICE_CONSOLE    = 0x00000050;

    FileManager* files = mgr.file_manager();
    if (files == nullptr) {
        write_io_status(m, io_status, nt_status::STATUS_INVALID_HANDLE, 0);
        m.set_return(nt_status::STATUS_INVALID_HANDLE);
        return;
    }

    if (info_class != FileFsDeviceInformation) {
        // Volume label, size and attribute classes are answered by the
        // kernel32 volume APIs, which take a path rather than a handle.
        // STATUS_NOT_IMPLEMENTED specifically, not STATUS_NOT_SUPPORTED: NT
        // returns it for a filesystem that does not implement the class, and
        // libuv's fs__stat_handle special-cases it (st_dev = 0) while treating
        // every other error status as a failed stat.
        VITREO_TRACE("[ntdll] NtQueryVolumeInformationFile(handle=0x%x, "
                     "class=%u) -> STATUS_NOT_IMPLEMENTED\n",
                     file_handle, info_class);
        write_io_status(m, io_status, nt_status::STATUS_NOT_IMPLEMENTED, 0);
        m.set_return(nt_status::STATUS_NOT_IMPLEMENTED);
        return;
    }

    // FILE_FS_DEVICE_INFORMATION { DEVICE_TYPE DeviceType; ULONG Characteristics; }
    constexpr u32 kDeviceInfoSize = 8;
    if (info == 0 || length < kDeviceInfoSize) {
        write_io_status(m, io_status, nt_status::STATUS_INFO_LENGTH_MISMATCH, 0);
        m.set_return(nt_status::STATUS_INFO_LENGTH_MISMATCH);
        return;
    }

    u32 device_type = 0;
    switch (files->handle_table().get_type(file_handle)) {
        case HandleType::File:          device_type = FILE_DEVICE_DISK; break;
        case HandleType::Pipe:
        case HandleType::Mailslot:      device_type = FILE_DEVICE_NAMED_PIPE; break;
        case HandleType::ConsoleInput:
        case HandleType::ConsoleOutput: device_type = FILE_DEVICE_CONSOLE; break;
        case HandleType::Device:        device_type = FILE_DEVICE_UNKNOWN; break;
        default:
            write_io_status(m, io_status, nt_status::STATUS_INVALID_HANDLE, 0);
            VITREO_TRACE("[ntdll] NtQueryVolumeInformationFile(handle=0x%x) -> "
                         "STATUS_INVALID_HANDLE\n", file_handle);
            m.set_return(nt_status::STATUS_INVALID_HANDLE);
            return;
    }
    (void)FILE_DEVICE_CD_ROM;

    (void)m.write_u32(info, device_type);
    (void)m.write_u32(info + 4, 0);   // Characteristics: no removable media
    write_io_status(m, io_status, nt_status::STATUS_SUCCESS, kDeviceInfoSize);

    VITREO_TRACE("[ntdll] NtQueryVolumeInformationFile(handle=0x%x) -> "
                 "device_type=0x%x\n", file_handle, device_type);
    m.set_return(nt_status::STATUS_SUCCESS);
}

} // namespace vitreo::winapi

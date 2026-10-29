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

/// @file ntcreatefile_thunk.cpp
/// @brief NtCreateFile Thunk
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtCreateFile Thunk
// =============================================================================

/// Structure offsets for OBJECT_ATTRIBUTES (32-bit):
///   Length:               0x00 (u32)
///   RootDirectory:        0x04 (u32 - HANDLE)
///   ObjectName:           0x08 (u32 - PUNICODE_STRING)
///   Attributes:           0x0C (u32)
///   SecurityDescriptor:   0x10 (u32)
///   SecurityQualityOfService: 0x14 (u32)
///
/// UNICODE_STRING (32-bit):
///   Length:        0x00 (u16) - byte count excluding NUL
///   MaximumLength: 0x02 (u16) - byte count including NUL
///   Buffer:        0x04 (u32) - pointer to UTF-16LE string

void ntdll::impl_NtCreateFile(Marshaler& m, NtdllExtManager& mgr) {
    // NTSTATUS NtCreateFile(
    //   PHANDLE            FileHandle,        // arg 0 - out
    //   ACCESS_MASK        DesiredAccess,     // arg 1
    //   POBJECT_ATTRIBUTES ObjectAttributes,  // arg 2
    //   PIO_STATUS_BLOCK   IoStatusBlock,     // arg 3 - out
    //   PLARGE_INTEGER     AllocationSize,    // arg 4 - optional
    //   ULONG              FileAttributes,    // arg 5
    //   ULONG              ShareAccess,       // arg 6
    //   ULONG              CreateDisposition, // arg 7
    //   ULONG              CreateOptions,     // arg 8
    //   PVOID              EaBuffer,          // arg 9 - optional
    //   ULONG              EaLength           // arg 10
    // );

    const GuestAddr handle_ptr      = m.arg_ptr(0);
    const u32 desired_access        = static_cast<u32>(m.arg(1));
    const GuestAddr obj_attr_ptr    = m.arg_ptr(2);
    const GuestAddr io_status_ptr   = m.arg_ptr(3);
    // arg 4 (AllocationSize) - ignored
    const u32 file_attributes       = static_cast<u32>(m.arg(5));
    const u32 share_access          = static_cast<u32>(m.arg(6));
    const u32 create_disposition    = static_cast<u32>(m.arg(7));
    const u32 create_options        = static_cast<u32>(m.arg(8));
    // args 9, 10 (EaBuffer, EaLength) - ignored

    // Validate required pointers
    if (handle_ptr == 0 || obj_attr_ptr == 0 || io_status_ptr == 0) {
        m.set_return(static_cast<u64>(nt_status::STATUS_INVALID_PARAMETER));
        return;
    }

    // OBJECT_ATTRIBUTES and UNICODE_STRING contain native pointers. Their
    // pointer members therefore move and widen in an AMD64 guest; treating
    // every caller as x86 reads RootDirectory as ObjectName and typically
    // turns a valid request into STATUS_OBJECT_NAME_INVALID.
    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr object_name_offset = is_64bit ? 0x10 : 0x08;
    const GuestAddr string_buffer_offset = is_64bit ? 0x08 : 0x04;
    const auto read_guest_ptr = [&](GuestAddr address) -> VResult<u64> {
        if (is_64bit) return m.read_u64(address);
        auto value = m.read_u32(address);
        if (value.is_err()) return VResult<u64>::err(value.error());
        return VResult<u64>::ok(value.value());
    };

    auto unistr_ptr_res = read_guest_ptr(obj_attr_ptr + object_name_offset);
    if (unistr_ptr_res.is_err() || unistr_ptr_res.value() == 0) {
        m.set_return(static_cast<u64>(nt_status::STATUS_OBJECT_NAME_INVALID));
        return;
    }
    const GuestAddr unistr_ptr = unistr_ptr_res.value();

    // Read UNICODE_STRING.Length and its pointer-sized Buffer member.
    auto len_res = m.read_u16(unistr_ptr);
    auto buf_res = read_guest_ptr(unistr_ptr + string_buffer_offset);
    if (len_res.is_err() || buf_res.is_err()) {
        m.set_return(static_cast<u64>(nt_status::STATUS_OBJECT_NAME_INVALID));
        return;
    }

    const u16 path_byte_len = len_res.value();
    const GuestAddr path_buf = buf_res.value();

    if (path_buf == 0 || path_byte_len == 0) {
        m.set_return(static_cast<u64>(nt_status::STATUS_OBJECT_NAME_INVALID));
        return;
    }

    // Read the wide string path from guest memory
    const u32 char_count = path_byte_len / 2;
    std::string path;
    path.reserve(char_count);

    for (u32 i = 0; i < char_count; ++i) {
        auto ch_res = m.read_u16(path_buf + i * 2);
        if (ch_res.is_err()) {
            m.set_return(static_cast<u64>(nt_status::STATUS_OBJECT_NAME_INVALID));
            return;
        }
        u16 wc = ch_res.value();
        // Simple UTF-16 to UTF-8 conversion (BMP only)
        if (wc < 0x80) {
            path.push_back(static_cast<char>(wc));
        } else if (wc < 0x800) {
            path.push_back(static_cast<char>(0xC0 | (wc >> 6)));
            path.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            path.push_back(static_cast<char>(0xE0 | (wc >> 12)));
            path.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            path.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }

    // Strip NT path prefixes:
    // \??\C:\...  -> C:\...
    // \Device\HarddiskVolume1\... -> C:\... (approximate)
    // \\?\C:\... -> C:\...
    std::string_view vfs_path = path;
    if (vfs_path.starts_with("\\??\\")) {
        vfs_path.remove_prefix(4);
    } else if (vfs_path.starts_with("\\\\?\\")) {
        vfs_path.remove_prefix(4);
    } else if (vfs_path.starts_with("\\Device\\")) {
        // Find the next backslash after \Device\X
        auto pos = vfs_path.find('\\', 8);
        if (pos != std::string_view::npos) {
            vfs_path.remove_prefix(pos);
            // Now vfs_path is like \path\to\file, prepend C:
            std::string fixed_path = "C:";
            fixed_path += vfs_path;
            path = std::move(fixed_path);
            vfs_path = path;
        }
    }

    VITREO_TRACE("[ntdll] impl_NtCreateFile: path='%.*s' access=0x%x disp=%u opts=0x%x\n",
                static_cast<int>(vfs_path.size()), vfs_path.data(),
                desired_access, create_disposition, create_options);

    // Call the manager to do the actual work
    auto result = mgr.nt_create_file(
        vfs_path,
        desired_access,
        file_attributes,
        share_access,
        create_disposition,
        create_options
    );

    // Write FileHandle (output)
    if (handle_ptr != 0) {
        (void)m.write_ptr(handle_ptr, result.handle); // PHANDLE
    }

    // IO_STATUS_BLOCK.Information is ULONG_PTR: +8 on x64, +4 on x86.
    (void)m.write_u32(io_status_ptr, result.status);
    if (is_64bit) {
        (void)m.write_u64(io_status_ptr + 8, result.information);
    } else {
        (void)m.write_u32(io_status_ptr + 4, result.information);
    }

    m.set_return(static_cast<u64>(result.status));
}

} // namespace vitreo::winapi

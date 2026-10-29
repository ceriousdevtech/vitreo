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

/// @file ntopenfile_thunk.cpp
/// @brief NtOpenFile Thunk
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtOpenFile Thunk
// =============================================================================

void ntdll::impl_NtOpenFile(Marshaler& m, NtdllExtManager& mgr) {
    // NTSTATUS NtOpenFile(
    //   PHANDLE            FileHandle,        // arg 0 - out
    //   ACCESS_MASK        DesiredAccess,     // arg 1
    //   POBJECT_ATTRIBUTES ObjectAttributes,  // arg 2
    //   PIO_STATUS_BLOCK   IoStatusBlock,     // arg 3 - out
    //   ULONG              ShareAccess,       // arg 4
    //   ULONG              OpenOptions        // arg 5
    // );

    const GuestAddr handle_ptr      = m.arg_ptr(0);
    const u32 desired_access        = static_cast<u32>(m.arg(1));
    const GuestAddr obj_attr_ptr    = m.arg_ptr(2);
    const GuestAddr io_status_ptr   = m.arg_ptr(3);
    const u32 share_access          = static_cast<u32>(m.arg(4));
    const u32 open_options          = static_cast<u32>(m.arg(5));

    // Validate required pointers
    if (handle_ptr == 0 || obj_attr_ptr == 0) {
        m.set_return(static_cast<u64>(nt_status::STATUS_INVALID_PARAMETER));
        return;
    }

    // OBJECT_ATTRIBUTES and UNICODE_STRING hold native pointers, so in an
    // AMD64 guest ObjectName is at +0x10 and Buffer at +0x08 (x86: +0x08 and
    // +0x04), as impl_NtCreateFile reads them. Reading the x86 layout for
    // every caller failed each 64-bit NtOpenFile with
    // STATUS_OBJECT_NAME_INVALID (tools/oracle_probe/nt_open_file_probe).
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

    // UNICODE_STRING.Length and its pointer-sized Buffer member.
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

    // Strip NT path prefixes
    std::string_view vfs_path = path;
    if (vfs_path.starts_with("\\??\\")) {
        vfs_path.remove_prefix(4);
    } else if (vfs_path.starts_with("\\\\?\\")) {
        vfs_path.remove_prefix(4);
    } else if (vfs_path.starts_with("\\Device\\")) {
        auto pos = vfs_path.find('\\', 8);
        if (pos != std::string_view::npos) {
            vfs_path.remove_prefix(pos);
            std::string fixed_path = "C:";
            fixed_path += vfs_path;
            path = std::move(fixed_path);
            vfs_path = path;
        }
    }

    VITREO_TRACE("[ntdll] impl_NtOpenFile: path='%.*s' access=0x%x opts=0x%x\n",
                static_cast<int>(vfs_path.size()), vfs_path.data(),
                desired_access, open_options);

    // NtOpenFile uses FILE_OPEN disposition (open existing only)
    auto result = mgr.nt_create_file(
        vfs_path,
        desired_access,
        0,  // file_attributes - ignored
        share_access,
        nt_file::FILE_OPEN,  // Always FILE_OPEN for NtOpenFile
        open_options
    );

    // Write FileHandle (output)
    if (handle_ptr != 0) {
        (void)m.write_ptr(handle_ptr, result.handle); // PHANDLE
    }

    // IO_STATUS_BLOCK.Information follows the pointer-sized union: +8 on
    // x64, +4 on x86.
    if (io_status_ptr != 0) {
        (void)m.write_u32(io_status_ptr, result.status);
        (void)m.write_ptr(
            io_status_ptr + (m.cpu_is_64bit() ? 8 : 4),
            result.information);
    }

    m.set_return(static_cast<u64>(result.status));
}

} // namespace vitreo::winapi

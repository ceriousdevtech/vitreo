// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_ext.cpp
/// @brief ntdll.dll extended function stubs (WIN32-086)

#include "winapi/common/object_access.hpp"
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

/// @file ntqueryinformationfile.cpp
/// @brief NtQueryInformationFile
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtQueryInformationFile
// =============================================================================

// FILE_INFORMATION_CLASS values for queries
static constexpr u32 FileBasicInformation          = 4;
static constexpr u32 FileStandardInformation       = 5;
static constexpr u32 FileInternalInformation       = 6;
static constexpr u32 FileAccessInformation         = 8;
static constexpr u32 FileQueryPositionInformation  = 14;
static constexpr u32 FileModeInformation           = 16;
static constexpr u32 FileAllInformation            = 18;
static constexpr u32 FileAttributeTagInformation   = 35;

namespace {

/// Windows never reports an attribute set of zero; a file with no other
/// attribute is FILE_ATTRIBUTE_NORMAL.
constexpr u32 kFileAttributeNormal = 0x80;
constexpr u32 kFileAttributeDirectory = 0x10;

[[nodiscard]] u32 win32_attributes(const capsule::FileInfo& info) {
    const u32 attrs = static_cast<u32>(info.attributes);
    return attrs == 0 ? kFileAttributeNormal : attrs;
}

void store_u64(u8* dst, u64 value) { std::memcpy(dst, &value, 8); }
void store_u32(u8* dst, u32 value) { std::memcpy(dst, &value, 4); }

/// FILE_BASIC_INFORMATION, 40 bytes:
///   CreationTime, LastAccessTime, LastWriteTime, ChangeTime, FileAttributes.
void fill_basic(u8* dst, const capsule::FileInfo& info) {
    std::memset(dst, 0, 40);
    store_u64(dst + 0,  info.creation_time);
    store_u64(dst + 8,  info.last_access_time);
    store_u64(dst + 16, info.last_write_time);
    // ChangeTime is the metadata-change time; the VFS does not track it apart
    // from the write time, which is the closer of the two answers.
    store_u64(dst + 24, info.last_write_time);
    store_u32(dst + 32, win32_attributes(info));
}

/// FILE_STANDARD_INFORMATION, 24 bytes:
///   AllocationSize, EndOfFile, NumberOfLinks, DeletePending, Directory.
void fill_standard(u8* dst, const capsule::FileInfo& info) {
    std::memset(dst, 0, 24);
    store_u64(dst + 0, info.size);   // AllocationSize
    store_u64(dst + 8, info.size);   // EndOfFile
    store_u32(dst + 16, info.link_count == 0 ? 1u : info.link_count);
    dst[20] = 0;                     // DeletePending
    // Directory must be truthful: libuv turns this into S_IFDIR, and a
    // directory reported as a regular file makes fs.statSync().isDirectory()
    // false for every directory in the guest.
    dst[21] = (win32_attributes(info) & kFileAttributeDirectory) != 0 ? 1 : 0;
}

} // namespace

u32 NtdllExtManager::nt_query_information_file(Handle handle, u32 info_class,
                                                u8* info_buffer, u32 info_length,
                                                u32* bytes_written) {
    if (bytes_written != nullptr) *bytes_written = 0;
    if (!has_file_support()) {
        return nt_status::STATUS_NOT_IMPLEMENTED;
    }

    // FILE_ACCESS_INFORMATION is a property of the HANDLE, not of the object
    // behind it, so it is answerable for a pipe, a console or a socket — none
    // of which have a VFS stream. A caller opens the ConPTY pipe and queries
    // it here. STATUS_INVALID_HANDLE makes the pipe look unusable and
    // abandons the spawn, so the child never reaches CreateProcess.
    if (info_class == FileAccessInformation) {
        if (info_buffer == nullptr) return nt_status::STATUS_INVALID_PARAMETER;
        if (info_length < 4) return nt_status::STATUS_INFO_LENGTH_MISMATCH;
        HandleTable* handles = handle_table();
        if (handles == nullptr ||
            handles->get_type(handle) == HandleType::Invalid) {
            return nt_status::STATUS_INVALID_HANDLE;
        }
        // The granted mask never carries GENERIC_* bits: Windows maps them
        // through the file object's generic mapping, the same value
        // NtQueryObject reports (winnt.h FILE_GENERIC_*;
        // tools/oracle_probe/file_access_info_probe, x86 = x64: a CreatePipe
        // read end reports 0x120189, a GENERIC_READ duplicate 0x120089).
        // libuv's uv_pipe_open marks a pipe readable only when FILE_READ_DATA
        // is set here, so an unmapped GENERIC_READ made every read ENOTCONN.
        const u32 granted = object_access::file_granted_access(
            handles->get_access(handle), HandleAccess::All);
        store_u32(info_buffer, granted);
        if (bytes_written != nullptr) *bytes_written = 4;
        VITREO_TRACE("[ntdll] NtQueryInformationFile(Access): handle=0x%x "
                     "access=0x%x\n", handle, granted);
        return nt_status::STATUS_SUCCESS;
    }

    // A handle opened on a directory (or on a volume root such as "\\?\C:\")
    // has no VFS stream behind it, so it is not in the VFS handle map at all.
    // Falling straight to STATUS_INVALID_HANDLE there made every stat of a
    // directory fail; the handle's path still identifies the object.
    if (info_class == FileModeInformation && m_file_manager != nullptr) {
        if (const auto mode = m_file_manager->pipe_file_mode(handle)) {
            if (info_buffer == nullptr) return nt_status::STATUS_INVALID_PARAMETER;
            if (info_length < 4) return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            store_u32(info_buffer, *mode);
            if (bytes_written != nullptr) *bytes_written = 4;
            return nt_status::STATUS_SUCCESS;
        }
    }
    const auto mapped_handle = vfs_handle_for(handle);
    capsule::FileInfo info{};
    bool have_info = false;
    if (mapped_handle.has_value()) {
        auto stat = m_vfs->stat_handle(*mapped_handle);
        if (stat.is_ok()) {
            info = stat.value();
            have_info = true;
        }
    }
    if (!have_info) {
        // The path the handle was opened on, from this manager's own table
        // first and kernel32's second, is the other way to reach the object.
        std::string path = nt_path_for_handle(handle);
        if (path.empty() && m_file_manager != nullptr) {
            path = m_file_manager->path_for_handle(handle);
        }
        if (!path.empty()) {
            auto stat = m_vfs->stat(path);
            if (stat.is_ok()) {
                info = stat.value();
                have_info = true;
            }
        }
    }
    if (!have_info && mapped_handle.has_value()) {
        // A filesystem that does not implement stat on an open handle still
        // gives the size by seeking, which is what this used to do for every
        // query. Losing that would fail queries that used to succeed.
        auto cur = m_vfs->seek(*mapped_handle, 0, capsule::SeekOrigin::Current);
        auto end = m_vfs->seek(*mapped_handle, 0, capsule::SeekOrigin::End);
        if (cur.is_ok() && end.is_ok()) {
            (void)m_vfs->seek(*mapped_handle, static_cast<i64>(cur.value()),
                              capsule::SeekOrigin::Begin);
            info = capsule::FileInfo{};
            info.size = end.value();
            info.attributes = capsule::FileAttributes::Normal;
            have_info = true;
        }
    }
    if (!have_info) {
        VITREO_TRACE("[ntdll] NtQueryInformationFile: handle=0x%x class=%u has "
                     "no VFS object\n", handle, info_class);
        return nt_status::STATUS_INVALID_HANDLE;
    }

    const auto current_offset = [&]() -> u64 {
        if (!mapped_handle.has_value()) return 0;
        auto pos = m_vfs->seek(*mapped_handle, 0, capsule::SeekOrigin::Current);
        return pos.is_ok() ? pos.value() : 0;
    };

    if (info_buffer == nullptr) {
        return nt_status::STATUS_INVALID_PARAMETER;
    }

    switch (info_class) {
        case FileStandardInformation: {
            if (info_length < 24) return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            fill_standard(info_buffer, info);
            if (bytes_written != nullptr) *bytes_written = 24;
            VITREO_TRACE("[ntdll] NtQueryInformationFile(Standard): handle=0x%x "
                         "size=%llu dir=%u\n", handle,
                         static_cast<unsigned long long>(info.size),
                         info_buffer[21]);
            return nt_status::STATUS_SUCCESS;
        }

        case FileBasicInformation: {
            if (info_length < 40) return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            fill_basic(info_buffer, info);
            if (bytes_written != nullptr) *bytes_written = 40;
            return nt_status::STATUS_SUCCESS;
        }

        case FileInternalInformation: {
            // FILE_INTERNAL_INFORMATION: LARGE_INTEGER IndexNumber.
            if (info_length < 8) return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            store_u64(info_buffer, info.file_id);
            if (bytes_written != nullptr) *bytes_written = 8;
            return nt_status::STATUS_SUCCESS;
        }

        case FileAttributeTagInformation: {
            // FILE_ATTRIBUTE_TAG_INFORMATION: FileAttributes, ReparseTag.
            if (info_length < 8) return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            store_u32(info_buffer + 0, win32_attributes(info));
            store_u32(info_buffer + 4, 0);   // nothing in a capsule is a reparse point
            if (bytes_written != nullptr) *bytes_written = 8;
            return nt_status::STATUS_SUCCESS;
        }

        case FileQueryPositionInformation: {
            // FILE_POSITION_INFORMATION (8 bytes): CurrentByteOffset.
            if (info_length < 8) return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            store_u64(info_buffer, current_offset());
            if (bytes_written != nullptr) *bytes_written = 8;
            return nt_status::STATUS_SUCCESS;
        }

        case FileAllInformation: {
            // FILE_ALL_INFORMATION concatenates eight sub-structures and ends
            // with a variable-length name, so 104 bytes is the fixed part with
            // room for nothing of the name. libuv's fs__stat_handle passes
            // exactly that and treats the resulting STATUS_BUFFER_OVERFLOW as
            // success — which is why the fixed part must still be filled in.
            //   0   BasicInformation      40
            //   40  StandardInformation   24
            //   64  InternalInformation    8
            //   72  EaInformation          4
            //   76  AccessInformation      4
            //   80  PositionInformation    8
            //   88  ModeInformation        4
            //   92  AlignmentInformation   4
            //   96  NameInformation        4 + name
            constexpr u32 kFixedSize = 100;   // through FileNameLength
            if (info_length < kFixedSize) {
                return nt_status::STATUS_INFO_LENGTH_MISMATCH;
            }
            std::memset(info_buffer, 0, info_length);
            fill_basic(info_buffer + 0, info);
            fill_standard(info_buffer + 40, info);
            store_u64(info_buffer + 64, info.file_id);        // IndexNumber
            store_u32(info_buffer + 72, 0);                   // EaSize
            store_u32(info_buffer + 76, 0);                   // AccessFlags
            store_u64(info_buffer + 80, current_offset());
            store_u32(info_buffer + 88, 0);                   // Mode
            store_u32(info_buffer + 92, 0);                   // AlignmentRequirement

            // NameInformation: the NT path of the object, in UTF-16 units.
            std::string path = nt_path_for_handle(handle);
            if (path.empty() && m_file_manager != nullptr) {
                path = m_file_manager->path_for_handle(handle);
            }
            // The name NT reports is volume-relative, so "C:\dir\f" is "\dir\f".
            if (path.size() >= 2 && path[1] == ':') path = path.substr(2);
            const u32 name_bytes = static_cast<u32>(path.size() * 2);
            store_u32(info_buffer + 96, name_bytes);

            const u32 room = info_length - kFixedSize;
            const u32 copied = std::min(room, name_bytes) & ~1u;
            for (u32 i = 0; i * 2 < copied; ++i) {
                const u16 unit = static_cast<u16>(
                    static_cast<unsigned char>(path[i]));
                std::memcpy(info_buffer + kFixedSize + i * 2, &unit, 2);
            }
            if (bytes_written != nullptr) *bytes_written = kFixedSize + copied;
            if (copied < name_bytes) {
                return nt_status::STATUS_BUFFER_OVERFLOW;
            }
            return nt_status::STATUS_SUCCESS;
        }

        default:
            VITREO_TRACE("[ntdll] NtQueryInformationFile: unhandled class %u\n", info_class);
            return nt_status::STATUS_NOT_IMPLEMENTED;
    }
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only

/// @file file_ops.cpp
/// @brief Extended file and system operation stubs for kernel32 (7-Zip installer)

#include "winapi/kernel32/filesystem/file_ops.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/short_name_83.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/advapi32/registry/registry.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "capsule/vfs/win32_full_path.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

/// @file file_info_by_handle_class.cpp
/// @brief FILE_INFO_BY_HANDLE_CLASS thunks
///
/// Closed cut from file_ops.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// FILE_INFO_BY_HANDLE_CLASS thunks
// =============================================================================

namespace {

// FILE_INFO_BY_HANDLE_CLASS values used here. Names match Win32 SDK.
enum FileInfoByHandleClass : u32 {
    FileBasicInfo                  = 0,
    FileStandardInfo               = 1,
    FileNameInfo                   = 2,
    FileRenameInfo                 = 3,
    FileDispositionInfo            = 4,
    FileAllocationInfo             = 5,
    FileEndOfFileInfo              = 6,
    FileStreamInfo                 = 7,
    FileCompressionInfo            = 8,
    FileAttributeTagInfo           = 9,
    FileIdBothDirectoryInfo        = 10,
    FileIdBothDirectoryRestartInfo = 11,
    FileIoPriorityHintInfo         = 12,
    FileRemoteProtocolInfo         = 13,
    FileFullDirectoryInfo          = 14,
    FileFullDirectoryRestartInfo   = 15,
    FileStorageInfo                = 16,
    FileAlignmentInfo              = 17,
    FileIdInfo                     = 18,
    FileIdExtdDirectoryInfo        = 19,
    FileIdExtdDirectoryRestartInfo = 20,
    FileRenameInfoEx               = 22,
};

// Helper: write a FILETIME (u64 in 100ns units since 1601) at addr+offset.
inline void write_filetime(GuestMemory& mem, GuestAddr addr, u64 ft) {
    (void)mem.write_u32(addr,     static_cast<u32>(ft & 0xFFFFFFFFu));
    (void)mem.write_u32(addr + 4, static_cast<u32>(ft >> 32));
}

// Convert capsule FileAttributes -> Win32 FILE_ATTRIBUTE_* bits.
// FileAttributes values are already Windows-compatible bit positions.
inline u32 to_win32_attrs(capsule::FileAttributes a) {
    u32 v = static_cast<u32>(a);
    if (v == 0) v = 0x80; // FILE_ATTRIBUTE_NORMAL
    return v;
}

constexpr bool is_directory_info_class(u32 info_class) {
    return info_class == FileIdBothDirectoryInfo ||
           info_class == FileIdBothDirectoryRestartInfo ||
           info_class == FileFullDirectoryInfo ||
           info_class == FileFullDirectoryRestartInfo ||
           info_class == FileIdExtdDirectoryInfo ||
           info_class == FileIdExtdDirectoryRestartInfo;
}

constexpr bool is_directory_restart_class(u32 info_class) {
    return info_class == FileIdBothDirectoryRestartInfo ||
           info_class == FileFullDirectoryRestartInfo ||
           info_class == FileIdExtdDirectoryRestartInfo;
}

constexpr usize directory_info_header_size(u32 info_class) {
    if (info_class == FileIdBothDirectoryInfo ||
        info_class == FileIdBothDirectoryRestartInfo) {
        return 104;
    }
    if (info_class == FileFullDirectoryInfo ||
        info_class == FileFullDirectoryRestartInfo) {
        return 68;
    }
    return 88;
}

void put_u16(std::vector<Byte>& buffer, usize offset, u16 value) {
    buffer[offset] = static_cast<Byte>(value & 0xFFu);
    buffer[offset + 1] = static_cast<Byte>(value >> 8);
}

void put_u32(std::vector<Byte>& buffer, usize offset, u32 value) {
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        buffer[offset + byte] =
            static_cast<Byte>((value >> (byte * 8)) & 0xFFu);
    }
}

void put_u64(std::vector<Byte>& buffer, usize offset, u64 value) {
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        buffer[offset + byte] =
            static_cast<Byte>((value >> (byte * 8)) & 0xFFu);
    }
}

struct EncodedDirectoryEntry {
    const capsule::DirectoryEntry* entry = nullptr;
    std::vector<u16> name;
    std::vector<u16> short_name;
    usize offset = 0;
    usize raw_size = 0;
};

void encode_directory_entry(std::vector<Byte>& buffer,
                            const EncodedDirectoryEntry& encoded,
                            u32 info_class, u32 next_offset) {
    const auto& info = encoded.entry->info;
    const usize base = encoded.offset;
    const u64 allocation_size =
        (info.size + 4095u) & ~static_cast<u64>(4095u);
    const u32 name_bytes =
        static_cast<u32>(encoded.name.size() * sizeof(u16));

    put_u32(buffer, base + 0, next_offset);
    put_u32(buffer, base + 4, 0u); // FileIndex is undefined on stable-ID FSes.
    put_u64(buffer, base + 8, info.creation_time);
    put_u64(buffer, base + 16, info.last_access_time);
    put_u64(buffer, base + 24, info.last_write_time);
    put_u64(buffer, base + 32, info.change_time); // ChangeTime = metadata ctime
    put_u64(buffer, base + 40, info.size);
    put_u64(buffer, base + 48, allocation_size);
    put_u32(buffer, base + 56, to_win32_attrs(info.attributes));
    put_u32(buffer, base + 60, name_bytes);
    put_u32(buffer, base + 64, 0u); // No extended attributes.

    usize name_offset = 68;
    if (info_class == FileIdBothDirectoryInfo ||
        info_class == FileIdBothDirectoryRestartInfo) {
        // FILE_ID_BOTH_DIR_INFO: ShortNameLength (CCHAR), Reserved (CCHAR),
        // ShortName[12], FileId, FileName.
        const u8 short_bytes = static_cast<u8>(
            std::min<usize>(encoded.short_name.size(), 12) * sizeof(u16));
        buffer[base + 68] = short_bytes;
        buffer[base + 69] = 0;
        for (usize i = 0; i < short_bytes / sizeof(u16); ++i) {
            put_u16(buffer, base + 70 + i * sizeof(u16), encoded.short_name[i]);
        }
        put_u64(buffer, base + 96, info.file_id);
        name_offset = 104;
    } else if (info_class == FileIdExtdDirectoryInfo ||
               info_class == FileIdExtdDirectoryRestartInfo) {
        put_u32(buffer, base + 68, 0u); // ReparsePointTag.
        put_u64(buffer, base + 72, info.file_id);
        put_u64(buffer, base + 80, 0u);
        name_offset = 88;
    }

    for (usize index = 0; index < encoded.name.size(); ++index) {
        put_u16(buffer, base + name_offset + index * sizeof(u16),
                encoded.name[index]);
    }
}

} // namespace

void FileOpsManager::impl_GetFileInformationByHandleEx(Marshaler& m) {
    // BOOL GetFileInformationByHandleEx(HANDLE hFile,
    //   FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    //   LPVOID lpFileInformation, DWORD dwBufferSize)
    Handle hFile       = static_cast<Handle>(m.arg(0));
    u32 info_class     = static_cast<u32>(m.arg(1));
    GuestAddr info_ptr = m.arg_ptr(2);
    u32 buf_size       = static_cast<u32>(m.arg(3));

    if (info_ptr == 0 || m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    if (info_class == FileNameInfo) {
        // Learn (FILE_NAME_INFO): the path relative to the volume root
        // (no drive letter), e.g. `\oracle\fil\m\h.txt`. Named pipes keep
        // their earlier spelling; every other open file uses the CreateFile
        // path (files_meta_probe FileNameInfo / _dir / _root).
        std::optional<std::string> name_utf8 =
            m_file_manager->named_pipe_file_name(hFile);
        if (!name_utf8.has_value()) {
            std::string full = m_file_manager->path_for_handle(hFile);
            if (full.empty()) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0u);
                return;
            }
            // Strip "X:" — root `C:\` becomes `\`.
            if (full.size() >= 2 && full[1] == ':') {
                full.erase(0, 2);
            }
            if (full.empty() || full[0] != '\\') {
                full.insert(full.begin(), '\\');
            }
            // Trailing separators other than a lone `\` are dropped so a
            // directory handle opened as `C:\oracle\fil\m\` matches the
            // path without the final slash.
            while (full.size() > 1 && (full.back() == '\\' || full.back() == '/')) {
                full.pop_back();
            }
            name_utf8 = std::move(full);
        }
        const std::vector<u16> wide_name = utf8_to_utf16(*name_utf8);
        const u32 name_bytes =
            static_cast<u32>(wide_name.size() * sizeof(u16));

        // Tiny buffers (< sizeof(DWORD)) fail ERROR_BAD_LENGTH without
        // writing anything (FileNameInfo_tiny). Larger but short buffers
        // write FileNameLength and fail ERROR_MORE_DATA (FileNameInfo_small).
        if (buf_size < sizeof(u32)) {
            set_last_error(error_codes::ERROR_BAD_LENGTH);
            m.set_return(0u);
            return;
        }
        (void)m_memory.write_u32(info_ptr, name_bytes);
        if (buf_size < sizeof(u32) + name_bytes) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            m.set_return(0u);
            return;
        }
        if (!wide_name.empty()) {
            const auto write = m_memory.write_bytes(
                info_ptr + sizeof(u32),
                reinterpret_cast<const Byte*>(wide_name.data()),
                name_bytes);
            if (write.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0u);
                return;
            }
        }
        VITREO_TRACE(
            "[file_ops] GetFileInformationByHandleEx("
            "h=0x%llx, FileNameInfo) \"%s\" -> TRUE\n",
            static_cast<unsigned long long>(hFile),
            name_utf8->c_str());
        m.set_return(1u);
        return;
    }

    capsule::FileHandle vfs_h =
        m_file_manager->vfs_handle_for(hFile);
    if (vfs_h == capsule::INVALID_FILE_HANDLE) {
        VITREO_TRACE("[file_ops] GetFileInformationByHandleEx(h=0x%llx, class=%u)"
                     " -> FALSE (unknown handle)\n",
                     static_cast<unsigned long long>(hFile), info_class);
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }

    if (is_directory_info_class(info_class)) {
        const usize header_size = directory_info_header_size(info_class);
        if (buf_size < header_size) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0u);
            return;
        }

        std::lock_guard<std::mutex> lock(m_directory_enumeration_mutex);
        auto state_it = m_directory_enumerations.find(hFile);
        if (is_directory_restart_class(info_class) ||
            state_it == m_directory_enumerations.end()) {
            auto listing = m_vfs.list_dir_handle(vfs_h);
            if (listing.is_err()) {
                set_last_error(vfs_error_to_win32(listing.error()));
                m.set_return(0u);
                return;
            }
            auto entries = std::move(listing.value());
            // NTFS directory handles enumerate "." and ".." first except on a
            // volume root (files_meta_probe FileFullDirectoryRestartInfo).
            std::string dir_path =
                m_file_manager ? m_file_manager->path_for_handle(hFile)
                               : std::string{};
            const bool is_volume_root =
                dir_path.size() == 3 && dir_path[1] == ':' &&
                (dir_path[2] == '\\' || dir_path[2] == '/');
            if (!is_volume_root) {
                capsule::DirectoryEntry dot;
                capsule::DirectoryEntry dotdot;
                dot.name = ".";
                dotdot.name = "..";
                auto self = m_vfs.stat_handle(vfs_h);
                if (self.is_ok()) {
                    dot.info = self.value();
                    dot.info.attributes =
                        dot.info.attributes | capsule::FileAttributes::Directory;
                } else {
                    dot.info.attributes = capsule::FileAttributes::Directory;
                }
                // Parent times/attrs when available; otherwise reuse self.
                dotdot.info = dot.info;
                if (dir_path.size() > 3) {
                    std::string parent = dir_path;
                    while (!parent.empty() &&
                           (parent.back() == '\\' || parent.back() == '/')) {
                        parent.pop_back();
                    }
                    const auto slash = parent.find_last_of("\\/");
                    if (slash != std::string::npos) {
                        parent.resize(slash == 2 && parent[1] == ':' ? 3
                                                                    : slash);
                        auto parent_info = m_vfs.stat(parent);
                        if (parent_info.is_ok()) {
                            dotdot.info = parent_info.value();
                            dotdot.info.attributes =
                                dotdot.info.attributes |
                                capsule::FileAttributes::Directory;
                        }
                    }
                }
                entries.insert(entries.begin(), std::move(dotdot));
                entries.insert(entries.begin(), std::move(dot));
            }
            auto state = std::make_shared<DirectoryEnumerationState>();
            state->entries = std::move(entries);
            state_it = m_directory_enumerations
                           .insert_or_assign(hFile, std::move(state))
                           .first;
        }

        auto& state = *state_it->second;
        if (state.next_index >= state.entries.size()) {
            set_last_error(error_codes::ERROR_NO_MORE_FILES);
            m.set_return(0u);
            return;
        }

        std::vector<EncodedDirectoryEntry> selected;
        usize next_index = state.next_index;
        while (next_index < state.entries.size()) {
            EncodedDirectoryEntry encoded;
            encoded.entry = &state.entries[next_index];
            encoded.name = utf8_to_utf16(encoded.entry->name);
            encoded.short_name =
                short_name_83::generate(encoded.entry->name);
            encoded.raw_size =
                header_size + encoded.name.size() * sizeof(u16);
            encoded.offset = selected.empty()
                ? 0
                : align_up(selected.back().offset +
                               selected.back().raw_size,
                           static_cast<usize>(8));

            if (encoded.offset + encoded.raw_size >
                static_cast<usize>(buf_size)) {
                break;
            }
            selected.push_back(std::move(encoded));
            ++next_index;
        }

        if (selected.empty()) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0u);
            return;
        }

        const usize used_size =
            selected.back().offset + selected.back().raw_size;
        std::vector<Byte> output(used_size, 0);
        for (usize index = 0; index < selected.size(); ++index) {
            const u32 next_offset = index + 1 < selected.size()
                ? static_cast<u32>(selected[index + 1].offset -
                                   selected[index].offset)
                : 0u;
            encode_directory_entry(output, selected[index], info_class,
                                   next_offset);
        }

        if (m_memory.write_bytes(info_ptr, output.data(), output.size())
                .is_err()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0u);
            return;
        }

        state.next_index = next_index;
        VITREO_TRACE(
            "[file_ops] GetFileInformationByHandleEx("
            "h=0x%llx, class=%u) entries=%zu next=%zu -> TRUE\n",
            static_cast<unsigned long long>(hFile), info_class,
            selected.size(), state.next_index);
        m.set_return(1u);
        return;
    }

    auto info_res = m_vfs.stat_handle(vfs_h);
    if (info_res.is_err()) {
        VITREO_TRACE("[file_ops] GetFileInformationByHandleEx(h=0x%llx, class=%u)"
                     " -> FALSE (stat failed: %s)\n",
                     static_cast<unsigned long long>(hFile), info_class,
                     capsule::vfs_error_to_string(info_res.error()));
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }
    const capsule::FileInfo& info = info_res.value();

    switch (info_class) {
        case FileBasicInfo: {
            // FILE_BASIC_INFO: { LARGE_INTEGER CreationTime, LastAccessTime,
            //   LastWriteTime, ChangeTime; DWORD FileAttributes; } = 40 bytes
            if (buf_size < 40) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            write_filetime(m_memory, info_ptr +  0, info.creation_time);
            write_filetime(m_memory, info_ptr +  8, info.last_access_time);
            write_filetime(m_memory, info_ptr + 16, info.last_write_time);
            write_filetime(m_memory, info_ptr + 24, info.change_time); // ChangeTime
            (void)m_memory.write_u32(info_ptr + 32, to_win32_attrs(info.attributes));
            (void)m_memory.write_u32(info_ptr + 36, 0u); // padding
            VITREO_TRACE("[file_ops] GetFileInformationByHandleEx(h=0x%llx, FileBasicInfo)"
                         " size=%llu attrs=0x%x -> TRUE\n",
                         static_cast<unsigned long long>(hFile),
                         static_cast<unsigned long long>(info.size),
                         to_win32_attrs(info.attributes));
            m.set_return(1u);
            return;
        }
        case FileStandardInfo: {
            // FILE_STANDARD_INFO: { LARGE_INTEGER AllocationSize, EndOfFile;
            //   DWORD NumberOfLinks; BOOLEAN DeletePending, Directory; } = 24 bytes
            if (buf_size < 24) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            // AllocationSize: round up to nearest 4 KiB, like NTFS clusters
            u64 alloc = (info.size + 4095u) & ~static_cast<u64>(4095u);
            (void)m_memory.write_u32(info_ptr +  0, static_cast<u32>(alloc & 0xFFFFFFFFu));
            (void)m_memory.write_u32(info_ptr +  4, static_cast<u32>(alloc >> 32));
            (void)m_memory.write_u32(info_ptr +  8, static_cast<u32>(info.size & 0xFFFFFFFFu));
            (void)m_memory.write_u32(info_ptr + 12, static_cast<u32>(info.size >> 32));
            (void)m_memory.write_u32(info_ptr + 16, info.link_count); // NumberOfLinks
            const bool is_dir = capsule::has_attribute(
                info.attributes, capsule::FileAttributes::Directory);
            const std::string path = m_file_manager->path_for_handle(hFile);
            const bool delete_pending =
                !path.empty() && m_file_manager->is_delete_pending(path);
            (void)m_memory.write_u8(info_ptr + 20, delete_pending ? 1u : 0u);
            (void)m_memory.write_u8(info_ptr + 21, is_dir ? 1u : 0u);   // Directory
            (void)m_memory.write_u8(info_ptr + 22, 0u);                 // padding
            (void)m_memory.write_u8(info_ptr + 23, 0u);                 // padding
            VITREO_TRACE("[file_ops] GetFileInformationByHandleEx(h=0x%llx, FileStandardInfo)"
                         " size=%llu links=%u delete_pending=%d dir=%d -> TRUE\n",
                         static_cast<unsigned long long>(hFile),
                         static_cast<unsigned long long>(info.size),
                         info.link_count,
                         delete_pending ? 1 : 0,
                         is_dir ? 1 : 0);
            m.set_return(1u);
            return;
        }
        case FileAttributeTagInfo: {
            // FILE_ATTRIBUTE_TAG_INFO: { DWORD FileAttributes; DWORD ReparseTag; } = 8
            if (buf_size < 8) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            (void)m_memory.write_u32(info_ptr + 0, to_win32_attrs(info.attributes));
            (void)m_memory.write_u32(info_ptr + 4, 0u); // ReparseTag
            m.set_return(1u);
            return;
        }
        case FileStreamInfo: {
            // FILE_STREAM_INFO: NextEntryOffset, StreamNameLength, StreamSize,
            // StreamAllocationSize, StreamName[]. Local files report the
            // unnamed ::$DATA stream (files_meta_probe FileStreamInfo).
            static constexpr u16 kDataStream[] = {
                u':', u':', u'$', u'D', u'A', u'T', u'A'};
            constexpr u32 kNameBytes =
                static_cast<u32>(sizeof(kDataStream));
            constexpr u32 kHeader = 4u + 4u + 8u + 8u;
            const u32 need = kHeader + kNameBytes;
            if (buf_size < need) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            u64 alloc = (info.size + 4095u) & ~static_cast<u64>(4095u);
            (void)m_memory.write_u32(info_ptr + 0, 0u); // NextEntryOffset
            (void)m_memory.write_u32(info_ptr + 4, kNameBytes);
            (void)m_memory.write_u32(
                info_ptr + 8, static_cast<u32>(info.size & 0xFFFFFFFFu));
            (void)m_memory.write_u32(
                info_ptr + 12, static_cast<u32>(info.size >> 32));
            (void)m_memory.write_u32(
                info_ptr + 16, static_cast<u32>(alloc & 0xFFFFFFFFu));
            (void)m_memory.write_u32(
                info_ptr + 20, static_cast<u32>(alloc >> 32));
            for (u32 i = 0; i < kNameBytes / 2u; ++i) {
                (void)m_memory.write_u16(
                    info_ptr + kHeader + i * 2u, kDataStream[i]);
            }
            m.set_return(1u);
            return;
        }
        case FileCompressionInfo: {
            // FILE_COMPRESSION_INFO / FILE_COMPRESSION_INFORMATION = 16 bytes.
            // Uncompressed local files: format NONE, size = EndOfFile.
            if (buf_size < 16) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            (void)m_memory.write_u32(
                info_ptr + 0, static_cast<u32>(info.size & 0xFFFFFFFFu));
            (void)m_memory.write_u32(
                info_ptr + 4, static_cast<u32>(info.size >> 32));
            (void)m_memory.write_u16(info_ptr + 8, 0u); // COMPRESSION_FORMAT_NONE
            (void)m_memory.write_u8(info_ptr + 10, 0u);
            (void)m_memory.write_u8(info_ptr + 11, 0u);
            (void)m_memory.write_u8(info_ptr + 12, 0u);
            (void)m_memory.write_u8(info_ptr + 13, 0u);
            (void)m_memory.write_u8(info_ptr + 14, 0u);
            (void)m_memory.write_u8(info_ptr + 15, 0u);
            m.set_return(1u);
            return;
        }
        case FileStorageInfo: {
            // FILE_STORAGE_INFO = 7 * ULONG. LogicalBytesPerSector must be
            // nonzero (files_meta_probe FileStorageInfo).
            if (buf_size < 28) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            constexpr u32 kSector = 512u;
            (void)m_memory.write_u32(info_ptr + 0, kSector);
            (void)m_memory.write_u32(info_ptr + 4, kSector);
            (void)m_memory.write_u32(info_ptr + 8, kSector);
            (void)m_memory.write_u32(info_ptr + 12, kSector);
            (void)m_memory.write_u32(info_ptr + 16, 0u); // Flags
            (void)m_memory.write_u32(info_ptr + 20, 0u);
            (void)m_memory.write_u32(info_ptr + 24, 0u);
            m.set_return(1u);
            return;
        }
        case FileAlignmentInfo: {
            // FILE_ALIGNMENT_INFO: AlignmentRequirement. Buffered local
            // files are byte-aligned (0); probe checks value < 512.
            if (buf_size < 4) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            (void)m_memory.write_u32(info_ptr, 0u);
            m.set_return(1u);
            return;
        }
        case FileIdInfo: {
            // FILE_ID_INFO: ULONGLONG VolumeSerialNumber; FILE_ID_128 FileId.
            // Serial low dword matches GetVolumeInformationW / BHFI;
            // Identifier[0..3] matches BHFI.nFileIndexLow (files_meta_probe).
            if (buf_size < 24) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            const u64 serial = info.volume_id;
            (void)m_memory.write_u32(
                info_ptr + 0, static_cast<u32>(serial & 0xFFFFFFFFu));
            (void)m_memory.write_u32(
                info_ptr + 4, static_cast<u32>(serial >> 32));
            // FILE_ID_128: low 64 bits = file_id (LE), high 64 zero.
            (void)m_memory.write_u32(
                info_ptr + 8, static_cast<u32>(info.file_id & 0xFFFFFFFFu));
            (void)m_memory.write_u32(
                info_ptr + 12, static_cast<u32>(info.file_id >> 32));
            (void)m_memory.write_u32(info_ptr + 16, 0u);
            (void)m_memory.write_u32(info_ptr + 20, 0u);
            m.set_return(1u);
            return;
        }
        default:
            // FileRemoteProtocolInfo (13) and unknown classes fail
            // ERROR_INVALID_PARAMETER on local files (files_meta_probe).
            VITREO_TRACE("[file_ops] GetFileInformationByHandleEx(h=0x%llx, class=%u)"
                         " -> FALSE (class not implemented)\n",
                         static_cast<unsigned long long>(hFile), info_class);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
    }
}

void FileOpsManager::impl_SetFileInformationByHandle(Marshaler& m) {
    // BOOL SetFileInformationByHandle(HANDLE hFile,
    //   FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    //   LPVOID lpFileInformation, DWORD dwBufferSize)
    Handle hFile       = static_cast<Handle>(m.arg(0));
    u32 info_class     = static_cast<u32>(m.arg(1));
    GuestAddr info_ptr = m.arg_ptr(2);
    u32 buf_size       = static_cast<u32>(m.arg(3));

    if (info_ptr == 0 || m_file_manager == nullptr) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    capsule::FileHandle vfs_h =
        m_file_manager->vfs_handle_for(hFile);
    if (vfs_h == capsule::INVALID_FILE_HANDLE) {
        VITREO_TRACE("[file_ops] SetFileInformationByHandle(h=0x%llx, class=%u)"
                     " -> FALSE (unknown handle)\n",
                     static_cast<unsigned long long>(hFile), info_class);
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0u);
        return;
    }

    auto read_u64 = [&](GuestAddr a) -> u64 {
        auto lo = m_memory.read_u32(a);
        auto hi = m_memory.read_u32(a + 4);
        if (lo.is_err() || hi.is_err()) return 0;
        return (static_cast<u64>(hi.value()) << 32) | lo.value();
    };

    switch (info_class) {
        case FileBasicInfo: {
            if (buf_size < 40) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            constexpr u64 EPOCH_OFFSET = 116444736000000000ULL;
            auto ft_to_unix = [&](u64 ft) -> i64 {
                if (ft == 0 || ft == 0xFFFFFFFFFFFFFFFFULL) return -1;
                if (ft < EPOCH_OFFSET) return 0;
                return static_cast<i64>((ft - EPOCH_OFFSET) / 10000000ULL);
            };
            u64 ftAccess = read_u64(info_ptr +  8);
            u64 ftWrite  = read_u64(info_ptr + 16);
            i64 atime = ft_to_unix(ftAccess);
            i64 mtime = ft_to_unix(ftWrite);
            auto r = m_vfs.set_file_times(vfs_h, atime, mtime);
            VITREO_TRACE("[file_ops] SetFileInformationByHandle(FileBasicInfo)"
                         " atime=%lld mtime=%lld -> %s\n",
                         static_cast<long long>(atime),
                         static_cast<long long>(mtime),
                         r.is_ok() ? "TRUE" : "TRUE (apply failed)");
            m.set_return(1u);
            return;
        }
        case FileDispositionInfo: {
            // FILE_DISPOSITION_INFO: { BOOLEAN DeleteFile; } = 1 byte
            if (buf_size < 1) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            auto del = m_memory.read_u8(info_ptr);
            bool delete_on_close = del.is_ok() && del.value() != 0;
            // Record the disposition on the handle; the object is removed when
            // its last handle closes. Reporting success without arming the
            // delete leaves callers that re-check the path spinning forever on
            // an object that never disappears.
            const bool armed =
                m_file_manager->set_delete_disposition(hFile, delete_on_close);
            VITREO_TRACE("[file_ops] SetFileInformationByHandle(FileDispositionInfo)"
                         " delete=%d -> %s\n", delete_on_close ? 1 : 0,
                         armed ? "TRUE" : "FALSE (untracked handle)");
            if (!armed) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0u);
                return;
            }
            m.set_return(1u);
            return;
        }
        case FileAllocationInfo: {
            // FILE_ALLOCATION_INFO: { LARGE_INTEGER AllocationSize; } = 8
            // Allocation hint only — no truncation. Accept silently.
            (void)buf_size;
            VITREO_TRACE("[file_ops] SetFileInformationByHandle(FileAllocationInfo) -> TRUE\n");
            m.set_return(1u);
            return;
        }
        case FileEndOfFileInfo: {
            // FILE_END_OF_FILE_INFO: { LARGE_INTEGER EndOfFile; } = 8
            if (buf_size < 8) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0u);
                return;
            }
            u64 new_size = read_u64(info_ptr);
            auto r = m_vfs.truncate(vfs_h, new_size);
            bool ok = r.is_ok();
            VITREO_TRACE("[file_ops] SetFileInformationByHandle(FileEndOfFileInfo)"
                         " size=%llu -> %s\n",
                         static_cast<unsigned long long>(new_size),
                         ok ? "TRUE" : "FALSE");
            if (!ok) set_last_error(error_codes::ERROR_DISK_FULL);
            m.set_return(ok ? 1u : 0u);
            return;
        }
        case FileRenameInfo:
        case FileRenameInfoEx: {
            // FILE_RENAME_INFO (winbase.h): union { BOOLEAN ReplaceIfExists;
            // DWORD Flags; } at 0, HANDLE RootDirectory, DWORD FileNameLength
            // (bytes), WCHAR FileName[]. x64: root at 8, length at 16, name at
            // 20. x86: root at 4, length at 8, name at 12 — reading the x64
            // layout on x86 turned every 32-bit handle rename into garbage.
            // FileRenameInfoEx uses the same layout; bit 0 is REPLACE_IF_EXISTS.
            const bool x64 = m.cpu_is_64bit();
            const u32 root_off = x64 ? 8u : 4u;
            const u32 len_off = x64 ? 16u : 8u;
            const u32 name_off = x64 ? 20u : 12u;
            if (buf_size < name_off) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            auto flags_r = m_memory.read_u32(info_ptr);
            auto root_r = x64 ? m_memory.read_u64(info_ptr + root_off)
                              : m_memory.read_u32(info_ptr + root_off).map(
                                    [](u32 v) { return static_cast<u64>(v); });
            auto len_r = m_memory.read_u32(info_ptr + len_off);
            if (flags_r.is_err() || root_r.is_err() || len_r.is_err()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            const u32 flags = flags_r.value();
            const u64 root = root_r.value();
            const u32 name_bytes = len_r.value();
            if (name_bytes < 2 || buf_size < name_off + name_bytes) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            std::string name;
            for (u32 i = 0; i + 1 < name_bytes; i += 2) {
                auto cu = m_memory.read_u16(info_ptr + name_off + i);
                if (cu.is_err() || cu.value() == 0) break;
                const u16 c = cu.value();
                // A surrogate pair is one code point (4 UTF-8 bytes).
                if (c >= 0xD800 && c < 0xDC00 && i + 3 < name_bytes) {
                    auto lo = m_memory.read_u16(info_ptr + name_off + i + 2);
                    if (lo.is_ok() && lo.value() >= 0xDC00 && lo.value() < 0xE000) {
                        const u32 cp = 0x10000u + ((static_cast<u32>(c) - 0xD800u) << 10) +
                                       (static_cast<u32>(lo.value()) - 0xDC00u);
                        name.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        name.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                        name.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        name.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        i += 2;
                        continue;
                    }
                }
                if (c < 0x80) {
                    name.push_back(static_cast<char>(c));
                } else if (c < 0x800) {
                    name.push_back(static_cast<char>(0xC0 | (c >> 6)));
                    name.push_back(static_cast<char>(0x80 | (c & 0x3F)));
                } else {
                    name.push_back(static_cast<char>(0xE0 | (c >> 12)));
                    name.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
                    name.push_back(static_cast<char>(0x80 | (c & 0x3F)));
                }
            }
            if (name.empty()) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0u);
                return;
            }
            const bool absolute = name.size() >= 2 && name[1] == ':';
            const bool unc = name.size() >= 2 &&
                             (name[0] == '\\' || name[0] == '/') &&
                             (name[1] == '\\' || name[1] == '/');
            std::string dest = name;
            if (!absolute && !unc && root != 0) {
                const std::string root_path =
                    m_file_manager->path_for_handle(static_cast<Handle>(root));
                if (root_path.empty()) {
                    set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                    m.set_return(0u);
                    return;
                }
                dest = root_path;
                if (dest.back() != '\\' && dest.back() != '/') dest.push_back('\\');
                dest += name;
            }
            const bool replace = info_class == FileRenameInfo
                                     ? (flags & 0xFFu) != 0
                                     : (flags & 1u) != 0;
            auto renamed = m_vfs.rename_open(vfs_h, dest, replace);
            if (renamed.is_err()) {
                set_last_error(vfs_error_to_win32(renamed.error()));
                m.set_return(0u);
                return;
            }
            m_file_manager->note_renamed(hFile, dest);
            m.set_return(1u);
            return;
        }
        case FileNameInfo:
        case FileStandardInfo:
        case FileAttributeTagInfo:
        default:
            // Rename/path info would require resolving the new path from the
            // FILE_RENAME_INFO struct (unaligned wide string). Not yet
            // implemented — return FALSE so the caller falls back to a path-
            // based MoveFileExW path instead of believing the rename succeeded.
            VITREO_TRACE("[file_ops] SetFileInformationByHandle(h=0x%llx, class=%u)"
                         " -> FALSE (class not implemented)\n",
                         static_cast<unsigned long long>(hFile), info_class);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0u);
            return;
    }
}

/// MOVEFILE_DELAY_UNTIL_REBOOT (Learn MoveFileEx; layout measured by
/// tools/oracle_probe/movefile_delay_probe.c on Windows 11): appends to the
/// REG_MULTI_SZ HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager
/// \\PendingFileRenameOperations the pair
///   "*1\\??\\<full source>" ("*2" when the source does not exist) and
///   "" (delete) or "*1\\??\\<full target>" ("*1!\\??\\..." with
///   MOVEFILE_REPLACE_EXISTING).
/// Paths are made full like GetFullPathName. The call succeeds without
/// touching the files; the last error becomes 0 except for a delete of a
/// source that does not exist, which leaves it alone (measured).
bool FileOpsManager::record_pending_rename(const std::string& src_utf8, const std::string* dst_utf8, bool replace) {
    if (m_registry == nullptr) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return false;
    }
    const std::string cwd = m_vfs.cwd();
    const auto src_full = capsule::win32_full_path(src_utf8, cwd);
    std::optional<capsule::Win32FullPath> dst_full;
    if (dst_utf8 != nullptr) dst_full = capsule::win32_full_path(*dst_utf8, cwd);
    if (!src_full || (dst_utf8 != nullptr && !dst_full)) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return false;
    }
    const bool src_exists = m_vfs.stat(src_full->path).is_ok();
    std::vector<u16> added;
    auto append = [&](std::string_view text) {
        const auto w = utf8_to_utf16(text);
        added.insert(added.end(), w.begin(), w.end());
    };
    append(src_exists ? "*1\\??\\" : "*2\\??\\");
    append(src_full->path);
    added.push_back(0);
    if (dst_full) {
        append(replace ? "*1!\\??\\" : "*1\\??\\");
        append(dst_full->path);
    }
    added.push_back(0);

    constexpr u64 kHkeyLocalMachine = 0x80000002;
    constexpr u32 kRegMultiSz = 7;
    const std::wstring key_path = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    const std::wstring value_name = L"PendingFileRenameOperations";
    u64 key = 0;
    u32 disposition = 0;
    if (const u32 rc = m_registry->reg_create_key_ex_w(kHkeyLocalMachine, key_path, &key, &disposition); rc != 0) {
        set_last_error(rc);
        return false;
    }
    // The existing strings, without the list's final empty string.
    std::vector<u16> value;
    u32 type = 0, size = 0;
    if (m_registry->reg_query_value_ex_w(key, value_name, &type, nullptr, &size) == 0 && size >= 2 &&
        type == kRegMultiSz) {
        std::vector<u8> bytes(size);
        if (m_registry->reg_query_value_ex_w(key, value_name, &type, bytes.data(), &size) == 0) {
            for (u32 i = 0; i + 1 < size; i += 2) value.push_back(static_cast<u16>(bytes[i] | (bytes[i + 1] << 8)));
            // Drop the list's terminating empty string only.
            if (value.size() >= 2 && value.back() == 0 && value[value.size() - 2] == 0) value.pop_back();
            if (value.size() == 1 && value[0] == 0) value.clear();
        }
    }
    value.insert(value.end(), added.begin(), added.end());
    value.push_back(0);
    std::vector<u8> out;
    out.reserve(value.size() * 2);
    for (const u16 c : value) { out.push_back(static_cast<u8>(c)); out.push_back(static_cast<u8>(c >> 8)); }
    const u32 rc = m_registry->reg_set_value_ex_w(key, value_name, kRegMultiSz, out.data(), static_cast<u32>(out.size()));
    (void)m_registry->reg_close_key(key);
    if (rc != 0) {
        set_last_error(rc);
        return false;
    }
    if (dst_full || src_exists) set_last_error(0);
    return true;
}

void FileOpsManager::impl_MoveFileExW(Marshaler& m) { move_file_ex_common(m, true); }

// MoveFileExA is the same contract with ANSI names (s4_movefile_probe a_*:
// NULL names are ERROR_PATH_NOT_FOUND, REPLACE_EXISTING and COPY_ALLOWED are
// honoured, unknown flag bits are ignored). It used to be a separate,
// flag-blind implementation that always refused an existing destination.
void FileOpsManager::impl_MoveFileExA(Marshaler& m) { move_file_ex_common(m, false); }

void FileOpsManager::move_file_ex_common(Marshaler& m, bool wide) {
    // BOOL MoveFileEx(LPCTSTR lpExistingFileName, LPCTSTR lpNewFileName, DWORD dwFlags)
    // Contract from Learn + files_ns_probe MoveFileExW_* + s4_movefile_probe.
    const GuestAddr src_ptr = m.arg_ptr(0);
    const GuestAddr dst_ptr = m.arg_ptr(1);
    const u32 flags = static_cast<u32>(m.arg(2));
    constexpr u32 kReplaceExisting = 0x1;
    constexpr u32 kCopyAllowed = 0x2;
    constexpr u32 kDelayUntilReboot = 0x4;

    // NULL source (with or without dest) -> ERROR_PATH_NOT_FOUND.
    if (src_ptr == 0) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }

    // COPY_ALLOWED | DELAY_UNTIL_REBOOT is an invalid combination.
    if ((flags & kCopyAllowed) != 0 && (flags & kDelayUntilReboot) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    auto src_res = wide ? m.read_wide_string(src_ptr) : m.read_ansi_string(src_ptr);
    // An empty name is ERROR_PATH_NOT_FOUND like a NULL one (w_empty_empty).
    if (src_res.is_err() || src_res.value().empty()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }

    // Learn MOVEFILE_DELAY_UNTIL_REBOOT: the operation is recorded in
    // PendingFileRenameOperations and nothing moves now. A NULL new name is
    // a delete at restart (movefile_delay_probe).
    if ((flags & kDelayUntilReboot) != 0 && dst_ptr == 0) {
        const std::string src_utf8 = wide ? src_res.value() : capsule::ansi_to_utf8(src_res.value());
        m.set_return(record_pending_rename(src_utf8, nullptr, false) ? 1u : 0u);
        return;
    }

    // NULL destination without DELAY is not a delete — Windows fails
    // ERROR_PATH_NOT_FOUND and leaves the source alone.
    if (dst_ptr == 0) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        VITREO_TRACE("[file_ops] MoveFileExW(%s, NULL, 0x%x) -> FALSE "
                     "(PATH_NOT_FOUND)\n",
                     src_res.value().c_str(), flags);
        m.set_return(0);
        return;
    }

    auto dst_res = wide ? m.read_wide_string(dst_ptr) : m.read_ansi_string(dst_ptr);
    if (dst_res.is_err() || dst_res.value().empty()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        m.set_return(0);
        return;
    }

    // DELAY with a destination records the rename for the restart.
    if ((flags & kDelayUntilReboot) != 0) {
        const std::string src_utf8 = wide ? src_res.value() : capsule::ansi_to_utf8(src_res.value());
        const std::string dst_utf8 = wide ? dst_res.value() : capsule::ansi_to_utf8(dst_res.value());
        m.set_return(record_pending_rename(src_utf8, &dst_utf8, (flags & kReplaceExisting) != 0) ? 1u : 0u);
        return;
    }

    const std::string& src = src_res.value();
    const std::string& dst = dst_res.value();

    auto dst_info = m_vfs.stat(dst);
    // A destination that names the source itself — the same path, or a
    // case-only rename on a case-insensitive volume — is a rename, not a
    // replace: Windows succeeds with or without REPLACE_EXISTING and keeps
    // the file (s4_movefile_probe a_case_only_replace / a_same_path_*).
    // Treating it as an existing destination removed the source.
    if (dst_info.is_ok()) {
        if (auto src_same = m_vfs.stat(src); src_same.is_ok() &&
            src_same.value().file_id != 0 &&
            src_same.value().file_id == dst_info.value().file_id &&
            src_same.value().volume_id == dst_info.value().volume_id) {
            dst_info = Result<capsule::FileInfo, capsule::VfsError>::err(
                capsule::VfsError::FileNotFound);
        }
    }
    if (dst_info.is_ok()) {
        const bool dst_dir = capsule::has_attribute(
            dst_info.value().attributes, capsule::FileAttributes::Directory);
        const bool dst_ro = capsule::has_attribute(
            dst_info.value().attributes, capsule::FileAttributes::ReadOnly);
        if ((flags & kReplaceExisting) == 0) {
            set_last_error(error_codes::ERROR_ALREADY_EXISTS);
            m.set_return(0);
            return;
        }
        // REPLACE_EXISTING onto a directory or a read-only file fails
        // ACCESS_DENIED and leaves the destination (files_ns_probe
        // MoveFileExW_replace_onto_dir / _replace_readonly_dest /
        // _dir_replace_existing_dir).
        if (dst_dir || dst_ro) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            m.set_return(0);
            return;
        }
        // Remove the destination file, then rename. If the source is
        // missing, the remove must NOT have already happened — check
        // source first.
        auto src_info = m_vfs.stat(src);
        if (src_info.is_err()) {
            set_last_error(vfs_error_to_win32(src_info.error()));
            m.set_return(0);
            return;
        }
        auto rem = m_vfs.remove(dst);
        if (rem.is_err()) {
            set_last_error(vfs_error_to_win32(rem.error()));
            m.set_return(0);
            return;
        }
    }

    auto r = m_vfs.rename(src, dst);
    bool ok = r.is_ok();
    if (!ok) {
        using capsule::VfsError;
        auto err = r.error();
        if (err == VfsError::PathAlreadyExists) {
            set_last_error(error_codes::ERROR_ALREADY_EXISTS);
        } else if (err == VfsError::FileNotFound) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        } else if (err == VfsError::PathNotFound) {
            set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        } else if (err == VfsError::NotSupported ||
                   err == VfsError::NotSameDevice) {
            // Learn: without COPY_ALLOWED -> ERROR_NOT_SAME_DEVICE; with it,
            // simulate the move via CopyFile + DeleteFile.
            if ((flags & kCopyAllowed) == 0) {
                set_last_error(error_codes::ERROR_NOT_SAME_DEVICE);
            } else {
                auto src_info = m_vfs.stat(src);
                if (src_info.is_err()) {
                    set_last_error(vfs_error_to_win32(src_info.error()));
                } else if (capsule::has_attribute(
                               src_info.value().attributes,
                               capsule::FileAttributes::Directory)) {
                    // Directory cross-volume copy is a different API family.
                    set_last_error(error_codes::ERROR_ACCESS_DENIED);
                } else {
                    auto src_h = m_vfs.open(
                        src, capsule::AccessMode::Read,
                        capsule::ShareMode::Read,
                        capsule::CreationDisposition::OpenExisting);
                    if (src_h.is_err()) {
                        set_last_error(vfs_error_to_win32(src_h.error()));
                    } else {
                        std::vector<u8> buf(64 * 1024);
                        std::vector<u8> contents;
                        bool read_ok = true;
                        for (;;) {
                            auto nr = m_vfs.read(
                                src_h.value(), std::span<u8>(buf));
                            if (nr.is_err()) {
                                read_ok = false;
                                set_last_error(vfs_error_to_win32(nr.error()));
                                break;
                            }
                            if (nr.value() == 0) break;
                            contents.insert(
                                contents.end(), buf.begin(),
                                buf.begin() +
                                    static_cast<std::ptrdiff_t>(nr.value()));
                        }
                        (void)m_vfs.close(src_h.value());
                        if (read_ok) {
                            auto dst_h = m_vfs.open(
                                dst, capsule::AccessMode::Write,
                                capsule::ShareMode::None,
                                capsule::CreationDisposition::CreateAlways);
                            if (dst_h.is_err()) {
                                set_last_error(
                                    vfs_error_to_win32(dst_h.error()));
                            } else {
                                bool write_ok = true;
                                if (!contents.empty()) {
                                    auto nw = m_vfs.write(
                                        dst_h.value(),
                                        std::span<const u8>(contents));
                                    if (nw.is_err() ||
                                        nw.value() != contents.size()) {
                                        write_ok = false;
                                    }
                                }
                                (void)m_vfs.close(dst_h.value());
                                if (!write_ok) {
                                    (void)m_vfs.remove(dst);
                                    set_last_error(
                                        error_codes::ERROR_ACCESS_DENIED);
                                } else {
                                    u32 attrs = static_cast<u32>(
                                        src_info.value().attributes);
                                    attrs |= static_cast<u32>(
                                        capsule::FileAttributes::Archive);
                                    (void)m_vfs.set_attributes(dst, attrs);
                                    auto rem = m_vfs.remove(src);
                                    if (rem.is_err()) {
                                        set_last_error(
                                            vfs_error_to_win32(rem.error()));
                                    } else {
                                        ok = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        } else {
            set_last_error(vfs_error_to_win32(err));
        }
    }
    VITREO_TRACE("[file_ops] MoveFileExW(%s, %s, 0x%x) -> %s\n",
                 src.c_str(), dst.c_str(), flags, ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1 : 0);
}

} // namespace vitreo::winapi

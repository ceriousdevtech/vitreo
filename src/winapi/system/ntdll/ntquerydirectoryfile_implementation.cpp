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

/// @file ntquerydirectoryfile_implementation.cpp
/// @brief NtQueryDirectoryFile Implementation
///
/// Closed cut from ntdll_ext.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// NtQueryDirectoryFile Implementation
// =============================================================================

namespace {

struct NtDirectoryLayout {
    usize header_size;
    usize name_offset;
    usize alignment;
};

[[nodiscard]] std::optional<NtDirectoryLayout> nt_directory_layout(
    u32 information_class) {
    switch (information_class) {
        case nt_file::FileDirectoryInformation:
            return NtDirectoryLayout{64, 64, 8};
        case nt_file::FileFullDirectoryInformation:
            return NtDirectoryLayout{68, 68, 8};
        case nt_file::FileBothDirectoryInformation:
            return NtDirectoryLayout{94, 94, 8};
        case nt_file::FileNamesInformation:
            return NtDirectoryLayout{12, 12, 4};
        case nt_file::FileIdBothDirectoryInformation:
            return NtDirectoryLayout{104, 104, 8};
        case nt_file::FileIdFullDirectoryInformation:
            return NtDirectoryLayout{80, 80, 8};
        default:
            return std::nullopt;
    }
}

[[nodiscard]] usize align_directory_offset(usize value, usize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

void nt_dir_put_u16(std::span<u8> buffer, usize offset, u16 value) {
    buffer[offset] = static_cast<u8>(value);
    buffer[offset + 1] = static_cast<u8>(value >> 8);
}

void nt_dir_put_u32(std::span<u8> buffer, usize offset, u32 value) {
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        buffer[offset + byte] = static_cast<u8>(value >> (byte * 8));
    }
}

void nt_dir_put_u64(std::span<u8> buffer, usize offset, u64 value) {
    for (usize byte = 0; byte < sizeof(value); ++byte) {
        buffer[offset + byte] = static_cast<u8>(value >> (byte * 8));
    }
}

[[nodiscard]] u16 fold_directory_char(u16 value) {
    return static_cast<u16>(
        std::towupper(static_cast<std::wint_t>(value)));
}

[[nodiscard]] bool nt_directory_name_matches(
    const std::optional<std::vector<u16>>& expression,
    const std::vector<u16>& name) {
    if (!expression.has_value()) return true;

    // Windows treats the ubiquitous DOS expression "*.*" as matching names
    // without a period too. Collapse it to the equivalent all-names scan.
    const std::vector<u16>* pattern = &*expression;
    static const std::vector<u16> all_names{u'*'};
    static const std::vector<u16> star_dot_star{u'*', u'.', u'*'};
    if (*pattern == star_dot_star) {
        pattern = &all_names;
    }

    usize pattern_index = 0;
    usize name_index = 0;
    std::optional<usize> star_pattern;
    usize star_name = 0;
    while (name_index < name.size()) {
        if (pattern_index < pattern->size() &&
            (*pattern)[pattern_index] == u'*') {
            star_pattern = pattern_index++;
            star_name = name_index;
            continue;
        }
        if (pattern_index < pattern->size() &&
            ((*pattern)[pattern_index] == u'?' ||
             fold_directory_char((*pattern)[pattern_index]) ==
                 fold_directory_char(name[name_index]))) {
            ++pattern_index;
            ++name_index;
            continue;
        }
        if (star_pattern.has_value()) {
            pattern_index = *star_pattern + 1;
            name_index = ++star_name;
            continue;
        }
        return false;
    }
    while (pattern_index < pattern->size() &&
           (*pattern)[pattern_index] == u'*') {
        ++pattern_index;
    }
    return pattern_index == pattern->size();
}

void encode_nt_directory_entry(
    std::span<u8> output,
    usize base,
    const capsule::DirectoryEntry& entry,
    const std::vector<u16>& wide_name,
    u32 information_class,
    u32 next_entry_offset,
    usize name_bytes_to_copy) {
    const u32 full_name_bytes =
        static_cast<u32>(wide_name.size() * sizeof(u16));
    nt_dir_put_u32(output, base, next_entry_offset);
    nt_dir_put_u32(output, base + 4, 0); // FileIndex is filesystem-defined.

    if (information_class == nt_file::FileNamesInformation) {
        nt_dir_put_u32(output, base + 8, full_name_bytes);
    } else {
        const auto& info = entry.info;
        const u64 allocation_size =
            (info.size + 4095u) & ~static_cast<u64>(4095u);
        nt_dir_put_u64(output, base + 8, info.creation_time);
        nt_dir_put_u64(output, base + 16, info.last_access_time);
        nt_dir_put_u64(output, base + 24, info.last_write_time);
        nt_dir_put_u64(output, base + 32, info.last_write_time);
        nt_dir_put_u64(output, base + 40, info.size);
        nt_dir_put_u64(output, base + 48, allocation_size);
        u32 attributes = static_cast<u32>(info.attributes);
        if (attributes == 0) attributes = 0x80; // FILE_ATTRIBUTE_NORMAL
        nt_dir_put_u32(output, base + 56, attributes);
        nt_dir_put_u32(output, base + 60, full_name_bytes);

        if (information_class == nt_file::FileFullDirectoryInformation ||
            information_class == nt_file::FileBothDirectoryInformation ||
            information_class == nt_file::FileIdBothDirectoryInformation ||
            information_class == nt_file::FileIdFullDirectoryInformation) {
            nt_dir_put_u32(output, base + 64, 0); // No extended attributes.
        }
        if (information_class == nt_file::FileIdBothDirectoryInformation) {
            // ShortNameLength and ShortName[12] remain zero.
            nt_dir_put_u64(output, base + 96, info.file_id);
        } else if (information_class ==
                   nt_file::FileIdFullDirectoryInformation) {
            nt_dir_put_u64(output, base + 72, info.file_id);
        }
    }

    const usize name_offset =
        nt_directory_layout(information_class)->name_offset;
    for (usize byte = 0; byte < name_bytes_to_copy; ++byte) {
        const u16 unit = wide_name[byte / sizeof(u16)];
        output[base + name_offset + byte] = static_cast<u8>(
            unit >> ((byte % sizeof(u16)) * 8));
    }
}

} // namespace

bool NtdllExtManager::directory_query_started(Handle handle) const {
    std::lock_guard<std::mutex> lock(m_file_state_mutex);
    return m_directory_queries.contains(handle);
}

bool NtdllExtManager::is_asynchronous_file_handle(Handle handle) const {
    std::lock_guard<std::mutex> lock(m_file_state_mutex);
    return m_vfs_handles.contains(handle) &&
           !m_synchronous_file_handles.contains(handle);
}

NtdllExtManager::QueryDirectoryResult
NtdllExtManager::nt_query_directory_file(
    Handle handle,
    std::span<u8> output,
    u32 information_class,
    bool return_single_entry,
    const std::optional<std::vector<u16>>& file_name,
    bool restart_scan) {
    QueryDirectoryResult result{};
    const auto layout = nt_directory_layout(information_class);
    if (!layout.has_value()) {
        result.status = nt_status::STATUS_INVALID_INFO_CLASS;
        return result;
    }
    if (!has_file_support() ||
        m_handles->get_type(handle) != HandleType::File) {
        result.status = nt_status::STATUS_INVALID_HANDLE;
        return result;
    }
    std::fill(output.begin(), output.end(), 0);
    std::lock_guard<std::mutex> lock(m_file_state_mutex);
    capsule::FileHandle vfs_handle = capsule::INVALID_FILE_HANDLE;
    const auto vfs_found = m_vfs_handles.find(handle);
    if (vfs_found != m_vfs_handles.end()) {
        vfs_handle = vfs_found->second;
    } else if (m_file_manager != nullptr) {
        vfs_handle = m_file_manager->vfs_handle_for(handle);
    }
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) {
        result.status = nt_status::STATUS_INVALID_HANDLE;
        return result;
    }
    auto state_found = m_directory_queries.find(handle);
    const bool first_query = state_found == m_directory_queries.end();
    if (first_query) {
        auto listing = m_vfs->list_dir_handle(vfs_handle);
        if (listing.is_err()) {
            result.status = vfs_error_to_ntstatus(listing.error());
            return result;
        }

        // "." and ".." lead the enumeration, as they do on NTFS. They are
        // collected unfiltered; the mask is applied below, once, for whichever
        // expression this call carries.
        DirectoryQueryState state;
        auto directory_info = m_vfs->stat_handle(vfs_handle);
        if (directory_info.is_ok()) {
            for (const char* dot_name : {".", ".."}) {
                capsule::DirectoryEntry dot;
                dot.name = dot_name;
                dot.info = directory_info.value();
                dot.info.attributes = dot.info.attributes |
                    capsule::FileAttributes::Directory;
                state.all_entries.push_back(std::move(dot));
            }
        }
        for (auto& entry : listing.value()) {
            state.all_entries.push_back(std::move(entry));
        }
        state_found = m_directory_queries
                          .emplace(handle, std::move(state))
                          .first;
    }

    auto& state = state_found->second;
    // Learn (ZwQueryDirectoryFile): FileName is honoured on EVERY call, not
    // just the first — supplying it restarts the enumeration using that
    // expression. Caching the first call's mask made a directory answer every
    // later lookup with the first match it had already found, so a guest that
    // probes a directory one name at a time (ReturnSingleEntry plus a fresh
    // mask, which is how a module resolver asks "is this name here?") got the
    // same entry back forever. A module loader issued 3.9 million
    // identical calls on one handle.
    const bool expression_changed =
        first_query ||
        (file_name.has_value() && file_name != state.expression);
    if (expression_changed) {
        state.expression = file_name;
        state.entries.clear();
        for (const auto& entry : state.all_entries) {
            const auto wide_name = utf8_to_utf16(entry.name);
            if (nt_directory_name_matches(state.expression, wide_name)) {
                state.entries.push_back(entry);
            }
        }
        state.next_index = 0;
    } else if (restart_scan) {
        state.next_index = 0;
    }
    if (output.size() < layout->header_size) {
        result.status = nt_status::STATUS_BUFFER_TOO_SMALL;
        return result;
    }
    if (state.next_index >= state.entries.size()) {
        result.status = first_query
            ? nt_status::STATUS_NO_SUCH_FILE
            : nt_status::STATUS_NO_MORE_FILES;
        return result;
    }

    const auto encode_name = [&](usize index) {
        return utf8_to_utf16(state.entries[index].name);
    };
    std::vector<std::vector<u16>> names;
    std::vector<usize> offsets;
    usize candidate_index = state.next_index;
    usize next_offset = 0;
    while (candidate_index < state.entries.size()) {
        std::vector<u16> name = encode_name(candidate_index);
        const usize raw_size =
            layout->header_size + name.size() * sizeof(u16);
        if (next_offset + raw_size > output.size()) break;
        offsets.push_back(next_offset);
        names.push_back(std::move(name));
        ++candidate_index;
        if (return_single_entry) break;
        next_offset = align_directory_offset(
            next_offset + raw_size, layout->alignment);
    }

    if (names.empty()) {
        if (first_query) {
            const auto name = encode_name(state.next_index);
            const usize available_name_bytes =
                output.size() - layout->header_size;
            const usize copied_name_bytes = std::min(
                available_name_bytes, name.size() * sizeof(u16));
            encode_nt_directory_entry(
                output, 0, state.entries[state.next_index], name,
                information_class, 0, copied_name_bytes);
            ++state.next_index;
            result.status = nt_status::STATUS_BUFFER_OVERFLOW;
            result.information = static_cast<u32>(
                layout->header_size + copied_name_bytes);
            return result;
        }
        // Microsoft documents this retry signal for a later call whose next
        // complete structure cannot fit: success with Information == 0.
        result.status = nt_status::STATUS_SUCCESS;
        return result;
    }

    for (usize index = 0; index < names.size(); ++index) {
        const u32 entry_delta = index + 1 < names.size()
            ? static_cast<u32>(offsets[index + 1] - offsets[index])
            : 0;
        encode_nt_directory_entry(
            output, offsets[index], state.entries[state.next_index + index],
            names[index], information_class, entry_delta,
            names[index].size() * sizeof(u16));
    }
    const usize used = offsets.back() + layout->header_size +
                       names.back().size() * sizeof(u16);
    state.next_index += names.size();
    result.information = static_cast<u32>(used);
    const auto narrow = [](const std::vector<u16>& w) {
        std::string out;
        for (const u16 unit : w) out.push_back(static_cast<char>(unit & 0xFF));
        return out;
    };
    VITREO_TRACE(
        "[ntdll] NtQueryDirectoryFile handle=0x%x class=%u entries=%zu "
        "bytes=%u next=%zu/%zu restart=%d single=%d mask='%s' first='%s'\n",
        handle, information_class, names.size(), result.information,
        state.next_index, state.entries.size(), restart_scan ? 1 : 0,
        return_single_entry ? 1 : 0,
        state.expression.has_value() ? narrow(*state.expression).c_str() : "<none>",
        narrow(names.front()).c_str());
    return result;
}

} // namespace vitreo::winapi

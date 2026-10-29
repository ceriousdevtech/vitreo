#include "iso_filesystem.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <limits>

namespace vitreo::capsule {
namespace {

std::atomic<FileHandle> next_iso_handle{0x40000000u};

u32 read_le32(const u8* data) {
    return static_cast<u32>(data[0]) |
           (static_cast<u32>(data[1]) << 8) |
           (static_cast<u32>(data[2]) << 16) |
           (static_cast<u32>(data[3]) << 24);
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

void append_utf8(std::string& out, u32 code_point) {
    if (code_point < 0x80) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point < 0x800) {
        out.push_back(static_cast<char>(0xC0u | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else if (code_point < 0x10000) {
        out.push_back(static_cast<char>(0xE0u | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (code_point & 0x3Fu)));
    }
}

/// Joliet stores names as big-endian UCS-2. Guest-visible names are UTF-8, so
/// they are converted here rather than at every call site.
std::string ucs2be_to_utf8(std::span<const u8> bytes) {
    std::string out;
    out.reserve(bytes.size());
    for (usize i = 0; i + 1 < bytes.size(); i += 2) {
        u32 unit = (static_cast<u32>(bytes[i]) << 8) | bytes[i + 1];
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
            const u32 low = (static_cast<u32>(bytes[i + 2]) << 8) | bytes[i + 3];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
                i += 2;
            }
        }
        if (unit == 0) break;
        append_utf8(out, unit);
    }
    return out;
}

std::string strip_version_suffix(std::string name) {
    const auto version = name.find(';');
    if (version != std::string::npos) {
        name.resize(version);
    }
    // A file with no extension is recorded as "NAME." on plain ISO9660.
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    return name;
}

std::string iso_name(std::span<const u8> bytes, bool joliet) {
    if (joliet) {
        return strip_version_suffix(ucs2be_to_utf8(bytes));
    }
    return strip_version_suffix(
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

std::string trim_iso_field(std::span<const u8> bytes, bool joliet = false) {
    std::string value = joliet
        ? ucs2be_to_utf8(bytes)
        : std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    return value;
}

/// A supplementary volume descriptor is Joliet when its escape sequences select
/// one of the three UCS-2 levels: %/@, %/C, or %/E.
bool is_joliet_escape(std::span<const u8> escapes) {
    for (usize i = 0; i + 2 < escapes.size(); ++i) {
        if (escapes[i] == '%' && escapes[i + 1] == '/' &&
            (escapes[i + 2] == '@' || escapes[i + 2] == 'C' ||
             escapes[i + 2] == 'E')) {
            return true;
        }
    }
    return false;
}

} // namespace

IsoFileSystem::IsoFileSystem(std::string image_path)
    : image_path_(std::move(image_path)) {
    valid_ = initialize();
}

bool IsoFileSystem::initialize() {
    namespace fs = std::filesystem;
    fs::path source(image_path_);
    std::string extension = lowercase(source.extension().string());

    if (extension == ".cue") {
        std::ifstream cue(source);
        if (!cue) {
            error_message_ = "Unable to open CUE file";
            return false;
        }

        std::string referenced_file;
        bool mode1_2352 = false;
        u32 track_count = 0;
        std::string line;
        while (std::getline(cue, line)) {
            const std::string lowered = lowercase(line);
            const auto file_pos = lowercase(line).find("file ");
            if (file_pos != std::string::npos && referenced_file.empty()) {
                const auto first_quote = line.find('"', file_pos);
                const auto second_quote = first_quote == std::string::npos
                    ? std::string::npos : line.find('"', first_quote + 1);
                if (first_quote != std::string::npos && second_quote != std::string::npos) {
                    referenced_file = line.substr(first_quote + 1, second_quote - first_quote - 1);
                }
            }
            if (lowered.find("track ") != std::string::npos) {
                ++track_count;
            }
            if (lowered.find("track 01 mode1/2352") != std::string::npos) {
                mode1_2352 = true;
            }
        }
        if (referenced_file.empty() || !mode1_2352 || track_count != 1) {
            error_message_ = "Only single-track MODE1/2352 CUE images are supported";
            return false;
        }
        image_path_ = (source.parent_path() / referenced_file).lexically_normal().string();
        physical_sector_size_ = 2352;
        payload_offset_ = 16;
    }

    image_.open(image_path_, std::ios::binary);
    if (!image_) {
        error_message_ = "Unable to open image data";
        return false;
    }
    if (extension != ".cue" && !configure_image_layout()) {
        return false;
    }

    // Scan the whole descriptor set rather than stopping at the primary: a disc
    // mastered with long filenames also carries a Joliet supplementary
    // descriptor, and its directory tree is the one worth using.
    std::array<u8, 2048> descriptor{};
    std::array<u8, 2048> joliet_descriptor{};
    bool found_primary = false;
    for (u32 sector = 16; sector < 64; ++sector) {
        std::array<u8, 2048> current{};
        if (!read_logical(static_cast<u64>(sector) * 2048, current)) {
            error_message_ = "Unable to read ISO9660 volume descriptors";
            return false;
        }
        if (std::memcmp(current.data() + 1, "CD001", 5) != 0) {
            continue;
        }
        if (current[0] == 1 && !found_primary) {
            descriptor = current;
            found_primary = true;
        } else if (current[0] == 2 && !joliet_ &&
                   is_joliet_escape(
                       std::span<const u8>(current).subspan(88, 32))) {
            joliet_descriptor = current;
            joliet_ = true;
        } else if (current[0] == 255) {
            break;
        }
    }
    if (!found_primary) {
        error_message_ = "ISO9660 primary volume descriptor not found";
        return false;
    }
    if (joliet_) {
        descriptor = joliet_descriptor;
    }

    volume_info_.label =
        trim_iso_field(std::span<const u8>(descriptor).subspan(40, 32), joliet_);
    volume_info_.filesystem_name = "CDFS";
    volume_info_.max_component_length = 110;
    volume_info_.filesystem_flags = 0x00080001u;
    volume_info_.sectors_per_cluster = 1;
    volume_info_.bytes_per_sector = 2048;
    volume_info_.free_clusters = 0;
    volume_info_.total_clusters = read_le32(descriptor.data() + 80);

    const u8 record_length = descriptor[156];
    if (record_length < 34) {
        error_message_ = "Invalid ISO9660 root directory record";
        return false;
    }
    const u32 root_extent = read_le32(descriptor.data() + 158);
    const u32 root_size = read_le32(descriptor.data() + 166);

    Entry root;
    root.path = "";
    root.extent = root_extent;
    root.size = root_size;
    root.directory = true;
    entries_.emplace("", std::move(root));
    if (!parse_directory("", root_extent, root_size, 0)) {
        if (error_message_.empty()) {
            error_message_ = "Invalid ISO9660 directory tree";
        }
        return false;
    }
    return true;
}

bool IsoFileSystem::volume_info(VolumeInfo& info) const noexcept {
    if (!valid_) {
        return false;
    }
    info = volume_info_;
    return true;
}

Result<usize, VfsError> IsoFileSystem::read_volume(u64 offset,
                                                   std::span<u8> buffer) {
    if (!valid_) {
        return Result<usize, VfsError>::err(VfsError::IoError);
    }
    if (buffer.empty()) {
        return Result<usize, VfsError>::ok(0);
    }
    std::lock_guard lock(mutex_);
    if (!read_logical(offset, buffer)) {
        return Result<usize, VfsError>::err(VfsError::ReadError);
    }
    return Result<usize, VfsError>::ok(buffer.size());
}

bool IsoFileSystem::configure_image_layout() {
    auto has_signature = [&](u32 sector_size, u32 payload_offset) {
        image_.clear();
        image_.seekg(static_cast<std::streamoff>(16ull * sector_size + payload_offset + 1));
        char signature[5]{};
        image_.read(signature, sizeof(signature));
        return image_.gcount() == static_cast<std::streamsize>(sizeof(signature)) &&
               std::memcmp(signature, "CD001", sizeof(signature)) == 0;
    };

    if (has_signature(2048, 0)) {
        physical_sector_size_ = 2048;
        payload_offset_ = 0;
        return true;
    }
    if (has_signature(2352, 16)) {
        physical_sector_size_ = 2352;
        payload_offset_ = 16;
        return true;
    }
    error_message_ = "Image is not an ISO9660 or MODE1/2352 data image";
    return false;
}

bool IsoFileSystem::read_logical(u64 offset, std::span<u8> buffer) {
    usize copied = 0;
    while (copied < buffer.size()) {
        const u64 logical = offset + copied;
        const u64 sector = logical / 2048;
        const usize in_sector = static_cast<usize>(logical % 2048);
        const usize chunk = std::min(buffer.size() - copied, 2048u - in_sector);
        const u64 physical = sector * physical_sector_size_ + payload_offset_ + in_sector;

        image_.clear();
        image_.seekg(static_cast<std::streamoff>(physical));
        if (!image_) {
            return false;
        }
        image_.read(reinterpret_cast<char*>(buffer.data() + copied),
                    static_cast<std::streamsize>(chunk));
        if (image_.gcount() != static_cast<std::streamsize>(chunk)) {
            return false;
        }
        copied += chunk;
    }
    return true;
}

bool IsoFileSystem::parse_directory(const std::string& path, u32 extent, u32 size, u32 depth) {
    if (depth > 64 || size > 64u * 1024u * 1024u) {
        error_message_ = "ISO9660 directory nesting or size limit exceeded";
        return false;
    }

    std::vector<u8> data(size);
    if (!read_logical(static_cast<u64>(extent) * 2048, data)) {
        return false;
    }

    usize offset = 0;
    while (offset < data.size()) {
        const u8 length = data[offset];
        if (length == 0) {
            offset = ((offset / 2048) + 1) * 2048;
            continue;
        }
        if (length < 34 || offset + length > data.size()) {
            return false;
        }

        const u8 name_length = data[offset + 32];
        if (33u + name_length > length) {
            return false;
        }
        const u8* name_data = data.data() + offset + 33;
        if (!(name_length == 1 && (name_data[0] == 0 || name_data[0] == 1))) {
            std::string name =
                iso_name(std::span<const u8>(name_data, name_length), joliet_);
            if (!name.empty()) {
                const std::string child_path = path.empty() ? name : path + "/" + name;
                const std::string key = normalize_path(child_path);
                Entry entry;
                entry.name = name;
                entry.path = key;
                entry.extent = read_le32(data.data() + offset + 2);
                entry.size = read_le32(data.data() + offset + 10);
                entry.directory = (data[offset + 25] & 0x02u) != 0;

                auto [it, inserted] = entries_.emplace(key, std::move(entry));
                if (inserted) {
                    entries_[normalize_path(path)].children.push_back(key);
                    if (it->second.directory &&
                        !parse_directory(key, it->second.extent, it->second.size, depth + 1)) {
                        return false;
                    }
                }
            }
        }
        offset += length;
    }
    return true;
}

std::string IsoFileSystem::normalize_path(std::string_view path) const {
    std::string normalized;
    normalized.reserve(path.size());
    for (char ch : path) {
        char value = ch == '\\' ? '/' : ch;
        if (value == '/' && (normalized.empty() || normalized.back() == '/')) {
            continue;
        }
        normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(value))));
    }
    while (!normalized.empty() && normalized.front() == '/') normalized.erase(normalized.begin());
    while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
    const auto version = normalized.find(';');
    if (version != std::string::npos) normalized.resize(version);
    return normalized;
}

const IsoFileSystem::Entry* IsoFileSystem::find_entry(std::string_view path) const {
    auto it = entries_.find(normalize_path(path));
    return it == entries_.end() ? nullptr : &it->second;
}

FileInfo IsoFileSystem::entry_info(const Entry& entry) {
    FileInfo info;
    info.size = entry.size;
    info.attributes = FileAttributes::ReadOnly |
        (entry.directory ? FileAttributes::Directory : FileAttributes::Archive);
    // ISO entries do not expose a POSIX inode through this backend. Use the
    // normalized on-disc path as the stable per-volume identity instead.
    constexpr u64 fnv_offset = 14695981039346656037ULL;
    constexpr u64 fnv_prime = 1099511628211ULL;
    info.volume_id = 0x49534f39363630ULL; // "ISO9660"
    info.file_id = fnv_offset;
    for (unsigned char ch : entry.path) {
        info.file_id ^= ch;
        info.file_id *= fnv_prime;
    }
    return info;
}

Result<FileHandle, VfsError> IsoFileSystem::open(
    std::string_view path, AccessMode access, ShareMode,
    CreationDisposition disposition) {
    std::lock_guard lock(mutex_);
    if (!valid_) return Result<FileHandle, VfsError>::err(VfsError::IoError);
    if (has_access(access, AccessMode::Write) || has_access(access, AccessMode::Delete) ||
        disposition != CreationDisposition::OpenExisting) {
        return Result<FileHandle, VfsError>::err(VfsError::AccessDenied);
    }
    const Entry* entry = find_entry(path);
    if (!entry) return Result<FileHandle, VfsError>::err(VfsError::PathNotFound);
    if (entry->directory) return Result<FileHandle, VfsError>::err(VfsError::NotAFile);
    const FileHandle handle = next_iso_handle.fetch_add(1, std::memory_order_relaxed);
    open_files_[handle] = OpenState{entry->path, 0};
    return Result<FileHandle, VfsError>::ok(handle);
}

Result<void, VfsError> IsoFileSystem::close(FileHandle handle) {
    std::lock_guard lock(mutex_);
    if (open_files_.erase(handle) == 0) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }
    return Result<void, VfsError>::ok();
}

Result<usize, VfsError> IsoFileSystem::read(FileHandle handle, std::span<u8> buffer) {
    std::lock_guard lock(mutex_);
    auto open = open_files_.find(handle);
    if (open == open_files_.end()) return Result<usize, VfsError>::err(VfsError::InvalidHandle);
    const Entry* entry = find_entry(open->second.path);
    if (!entry) return Result<usize, VfsError>::err(VfsError::InvalidHandle);
    const u64 remaining = entry->size > open->second.position
        ? entry->size - open->second.position : 0;
    const usize count = static_cast<usize>(std::min<u64>(buffer.size(), remaining));
    if (count != 0 && !read_logical(static_cast<u64>(entry->extent) * 2048 + open->second.position,
                                    buffer.first(count))) {
        return Result<usize, VfsError>::err(VfsError::ReadError);
    }
    open->second.position += count;
    return Result<usize, VfsError>::ok(count);
}

Result<usize, VfsError> IsoFileSystem::write(FileHandle, std::span<const u8>) {
    return Result<usize, VfsError>::err(VfsError::AccessDenied);
}

Result<u64, VfsError> IsoFileSystem::seek(FileHandle handle, i64 offset, SeekOrigin origin) {
    std::lock_guard lock(mutex_);
    auto open = open_files_.find(handle);
    if (open == open_files_.end()) return Result<u64, VfsError>::err(VfsError::InvalidHandle);
    const Entry* entry = find_entry(open->second.path);
    if (!entry) return Result<u64, VfsError>::err(VfsError::InvalidHandle);

    i64 base = 0;
    if (origin == SeekOrigin::Current) base = static_cast<i64>(open->second.position);
    if (origin == SeekOrigin::End) base = static_cast<i64>(entry->size);
    if ((offset < 0 && base < -offset) ||
        (offset > 0 && base > std::numeric_limits<i64>::max() - offset)) {
        return Result<u64, VfsError>::err(VfsError::SeekError);
    }
    const i64 position = base + offset;
    if (position < 0) return Result<u64, VfsError>::err(VfsError::SeekError);
    open->second.position = static_cast<u64>(position);
    return Result<u64, VfsError>::ok(open->second.position);
}

Result<FileInfo, VfsError> IsoFileSystem::stat(std::string_view path) {
    std::lock_guard lock(mutex_);
    const Entry* entry = find_entry(path);
    if (!entry) return Result<FileInfo, VfsError>::err(VfsError::PathNotFound);
    return Result<FileInfo, VfsError>::ok(entry_info(*entry));
}

Result<FileInfo, VfsError> IsoFileSystem::stat_handle(FileHandle handle) {
    std::lock_guard lock(mutex_);
    auto open = open_files_.find(handle);
    if (open == open_files_.end()) return Result<FileInfo, VfsError>::err(VfsError::InvalidHandle);
    const Entry* entry = find_entry(open->second.path);
    if (!entry) return Result<FileInfo, VfsError>::err(VfsError::InvalidHandle);
    return Result<FileInfo, VfsError>::ok(entry_info(*entry));
}

Result<std::string, VfsError> IsoFileSystem::final_path(FileHandle handle) {
    std::lock_guard lock(mutex_);
    auto open = open_files_.find(handle);
    if (open == open_files_.end()) return Result<std::string, VfsError>::err(VfsError::InvalidHandle);
    // Entry keys are lower-cased; each ancestor key is a prefix of the
    // child's, and its Entry keeps the name as recorded on the disc.
    const std::string key = normalize_path(open->second.path);
    std::string spelled;
    usize cursor = 0;
    while (cursor < key.size()) {
        usize end = key.find('/', cursor);
        if (end == std::string::npos) end = key.size();
        auto it = entries_.find(key.substr(0, end));
        if (it == entries_.end()) return Result<std::string, VfsError>::err(VfsError::InvalidHandle);
        if (!spelled.empty()) spelled.push_back('/');
        spelled += it->second.name;
        cursor = end + 1;
    }
    return Result<std::string, VfsError>::ok(std::move(spelled));
}

Result<std::vector<DirectoryEntry>, VfsError> IsoFileSystem::list_dir(std::string_view path) {
    std::lock_guard lock(mutex_);
    const Entry* entry = find_entry(path);
    if (!entry) return Result<std::vector<DirectoryEntry>, VfsError>::err(VfsError::PathNotFound);
    if (!entry->directory) return Result<std::vector<DirectoryEntry>, VfsError>::err(VfsError::NotADirectory);
    std::vector<DirectoryEntry> result;
    result.reserve(entry->children.size());
    for (const std::string& child_path : entry->children) {
        const Entry& child = entries_.at(child_path);
        result.push_back(DirectoryEntry{child.name, entry_info(child)});
    }
    return Result<std::vector<DirectoryEntry>, VfsError>::ok(std::move(result));
}

Result<void, VfsError> IsoFileSystem::create_dir(std::string_view) {
    return Result<void, VfsError>::err(VfsError::AccessDenied);
}
Result<void, VfsError> IsoFileSystem::remove(std::string_view) {
    return Result<void, VfsError>::err(VfsError::AccessDenied);
}
Result<void, VfsError> IsoFileSystem::rename(std::string_view, std::string_view) {
    return Result<void, VfsError>::err(VfsError::AccessDenied);
}

} // namespace vitreo::capsule

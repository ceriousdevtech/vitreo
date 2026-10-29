/// @file vfs.cpp
/// @brief Virtual Filesystem implementation

#include "vfs.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "capsule/vfs/win32_full_path.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace vitreo::capsule {

// =============================================================================
// Path Utilities Implementation
// =============================================================================

ParsedPath parse_windows_path(std::string_view path) noexcept {
    ParsedPath result;
    
    if (path.empty()) {
        return result;
    }
    
    // Strip \\?\ extended-length path prefix (and \\?\UNC\ for UNC paths)
    if (path.size() >= 4 && path[0] == '\\' && path[1] == '\\' &&
        path[2] == '?' && path[3] == '\\') {
        path = path.substr(4);
        if (path.empty()) {
            return result;
        }
    }
    // A Win32 device path to a drive, "\\.\C:\foo", names C:\foo too
    // (wow64_fsredir_probe nt-global reaches the file); other \\.\ names
    // (pipes, devices) stay with their own handlers.
    if (path.size() >= 6 && path[0] == '\\' && path[1] == '\\' && path[2] == '.' &&
        path[3] == '\\' && is_valid_drive_letter(path[4]) && path[5] == ':') {
        path = path.substr(4);
    }

    usize pos = 0;
    
    // Check for drive letter (e.g., "C:" or "C:\")
    if (path.size() >= 2 && path[1] == ':' && is_valid_drive_letter(path[0])) {
        result.drive_letter = normalize_drive_letter(path[0]);
        pos = 2;
        
        // Check if separator follows drive letter -> absolute path
        if (pos < path.size() && (path[pos] == '\\' || path[pos] == '/')) {
            result.is_absolute = true;
            pos++;
        } else {
            // C:foo — drive-relative path (not absolute)
            result.is_absolute = false;
        }
    } else if (!path.empty() && (path[0] == '\\' || path[0] == '/')) {
        // Absolute path without drive letter
        result.is_absolute = true;
        pos = 1;
    }
    
    // Extract and normalize the remaining path
    if (pos < path.size()) {
        result.relative_path = normalize_path_separators(path.substr(pos));
    }
    
    // Remove trailing slashes
    while (!result.relative_path.empty() && result.relative_path.back() == '/') {
        result.relative_path.pop_back();
    }
    
    return result;
}

std::string normalize_path_separators(std::string_view path) noexcept {
    std::string result;
    result.reserve(path.size());
    
    bool last_was_slash = false;
    
    for (char c : path) {
        if (c == '\\' || c == '/') {
            if (!last_was_slash) {
                result.push_back('/');
                last_was_slash = true;
            }
        } else {
            result.push_back(c);
            last_was_slash = false;
        }
    }
    
    return result;
}

// =============================================================================
// Vfs Implementation
// =============================================================================

std::string Vfs::generated_path_key(std::string_view path) const {
    // Keyed by the file the path reaches: in a 32-bit process System32 and
    // SysWOW64 are one directory only while redirection is on.
    return generated_key_of_viewed(viewed_path(path));
}

std::string Vfs::generated_key_of_viewed(std::string_view viewed) const {
    auto parsed = parse_windows_path(viewed);
    const char drive = parsed.drive_letter ? parsed.drive_letter : default_drive_;
    std::string key;
    key.reserve(parsed.relative_path.size() + 2);
    key.push_back(normalize_drive_letter(drive));
    key.push_back(':');
    for (char c : parsed.relative_path) {
        key.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(c))));
    }
    return key;
}

std::string Vfs::generated_spelling(std::string_view path) const {
    const auto parsed = parse_windows_path(viewed_path(path));
    const char drive = parsed.drive_letter ? parsed.drive_letter : default_drive_;
    std::string spelled{normalize_drive_letter(drive), ':', '\\'};
    std::string_view rel = parsed.relative_path;
    while (!rel.empty() && rel.front() == '/') rel.remove_prefix(1);
    for (char c : rel) spelled.push_back(c == '/' ? '\\' : c);
    while (spelled.size() > 3 && spelled.back() == '\\') spelled.pop_back();
    return spelled;
}

Result<void, VfsError> Vfs::publish_generated_file(
    std::string_view path,
    std::shared_ptr<const std::vector<u8>> bytes
) {
    if (!bytes || path.empty()) {
        return Result<void, VfsError>::err(VfsError::InvalidArgument);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto parsed = parse_windows_path(path);
    if (!parsed.drive_letter || parsed.relative_path.empty()) {
        return Result<void, VfsError>::err(VfsError::InvalidPath);
    }
    publish_generated_locked(path, std::move(bytes));
    return Result<void, VfsError>::ok();
}

void Vfs::publish_generated_locked(
    std::string_view path, std::shared_ptr<const std::vector<u8>> bytes) {
    const std::string key = generated_path_key(path);
    generated_files_[key] = std::move(bytes);
    generated_names_[key] = generated_spelling(path);
}

void Vfs::set_generated_file_source(GeneratedFileSource source,
                                    GeneratedListing listing) {
    std::lock_guard<std::mutex> lock(mutex_);
    generated_source_ = std::move(source);
    generated_listing_ = std::move(listing);
}

bool Vfs::is_generated_file(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) return false;
    auto [fs, relative_path] = std::move(resolve_result.value());
    const auto host = fs->stat(relative_path);
    if (host.is_ok() || (host.error() != VfsError::FileNotFound &&
                         host.error() != VfsError::PathNotFound)) {
        return false;
    }
    return generated_file_locked(path) != nullptr;
}

std::shared_ptr<const std::vector<u8>> Vfs::generated_file_locked(
    std::string_view path) {
    if (const auto it = generated_files_.find(generated_path_key(path));
        it != generated_files_.end()) {
        return it->second;
    }
    if (!generated_source_) return nullptr;
    const auto parsed = parse_windows_path(path);
    if (!parsed.drive_letter || parsed.relative_path.empty()) return nullptr;
    std::string spelled = generated_spelling(path);
    auto bytes = generated_source_(spelled);
    if (!bytes) return nullptr;
    // The file's name is the spelling its directory lists, whatever case the
    // caller used (FindFirstFile and GetFinalPathNameByHandle report it).
    if (const auto sep = spelled.rfind('\\');
        generated_listing_ && sep != std::string::npos) {
        const std::string leaf = spelled.substr(sep + 1);
        for (const auto& [name, size] :
             generated_listing_(std::string_view(spelled).substr(0, sep))) {
            (void)size;
            if (name.size() == leaf.size() &&
                std::equal(name.begin(), name.end(), leaf.begin(),
                           [](char a, char b) {
                               return std::tolower(static_cast<unsigned char>(a)) ==
                                      std::tolower(static_cast<unsigned char>(b));
                           })) {
                spelled.replace(sep + 1, std::string::npos, name);
                break;
            }
        }
    }
    // @p spelled already names the file the view reaches (Sysnative's is
    // System32's): key it as it is, not viewed a second time.
    const std::string key = generated_key_of_viewed(spelled);
    generated_files_[key] = bytes;
    generated_names_[key] = spelled;
    return bytes;
}

FileInfo Vfs::generated_file_info(u64 size) noexcept {
    // Measured (builtin_dll_file_probe): a system DLL is FILE_ATTRIBUTE_ARCHIVE
    // alone -- not read-only, not system -- with real, nonzero times. These
    // files are Vitreo's 10.0.19041 modules, a build Microsoft shipped on
    // 2019-12-07; one fixed time for all keeps every enumeration stable.
    constexpr FileTime k2019_12_07 = 132211504000000000ull;  // 00:00 UTC
    FileInfo info{};
    info.size = size;
    info.attributes = FileAttributes::Archive;
    info.creation_time = k2019_12_07;
    info.last_access_time = k2019_12_07;
    info.last_write_time = k2019_12_07;
    info.change_time = k2019_12_07;
    return info;
}

Result<void, VfsError> Vfs::mount_drive(char letter, std::unique_ptr<IFileSystem> fs) {
    if (!is_valid_drive_letter(letter)) {
        return Result<void, VfsError>::err(VfsError::InvalidDriveLetter);
    }
    
    if (!fs) {
        return Result<void, VfsError>::err(VfsError::InvalidArgument);
    }
    
    char normalized = normalize_drive_letter(letter);
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (drives_.find(normalized) != drives_.end()) {
        return Result<void, VfsError>::err(VfsError::DriveAlreadyMounted);
    }
    
    drives_[normalized] = std::move(fs);
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> Vfs::unmount_drive(char letter) {
    if (!is_valid_drive_letter(letter)) {
        return Result<void, VfsError>::err(VfsError::InvalidDriveLetter);
    }
    
    char normalized = normalize_drive_letter(letter);
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = drives_.find(normalized);
    if (it == drives_.end()) {
        return Result<void, VfsError>::err(VfsError::DriveNotMounted);
    }
    
    // Remove any handles pointing to this drive
    auto handle_it = handle_to_drive_.begin();
    while (handle_it != handle_to_drive_.end()) {
        if (handle_it->second == normalized) {
            handle_it = handle_to_drive_.erase(handle_it);
        } else {
            ++handle_it;
        }
    }
    
    drives_.erase(it);
    return Result<void, VfsError>::ok();
}

bool Vfs::is_drive_mounted(char letter) const noexcept {
    if (!is_valid_drive_letter(letter)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return drives_.find(normalize_drive_letter(letter)) != drives_.end();
}

std::vector<char> Vfs::mounted_drives() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<char> result;
    result.reserve(drives_.size());
    
    for (const auto& [letter, _] : drives_) {
        result.push_back(letter);
    }
    
    std::sort(result.begin(), result.end());
    return result;
}

void Vfs::set_default_drive(char letter) noexcept {
    if (is_valid_drive_letter(letter)) {
        std::lock_guard<std::mutex> lock(mutex_);
        default_drive_ = normalize_drive_letter(letter);
    }
}

void Vfs::set_cwd(const std::string& cwd) {
    std::lock_guard<std::mutex> lock(mutex_);
    cwd_ = cwd;
}

std::string Vfs::cwd() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cwd_;
}

namespace {

thread_local bool t_wow64_redirection_disabled = false;

[[nodiscard]] bool ieq_prefix(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) ==
                      std::tolower(static_cast<unsigned char>(b));
           });
}

/// @p rest names @p dir or something below it.
[[nodiscard]] bool names_dir(std::string_view rest, std::string_view dir) {
    return ieq_prefix(rest, dir) && (rest.size() == dir.size() || rest[dir.size()] == '\\');
}

}  // namespace

void Vfs::set_thread_redirection_disabled(bool disabled) noexcept {
    t_wow64_redirection_disabled = disabled;
}

bool Vfs::thread_redirection_disabled() noexcept { return t_wow64_redirection_disabled; }

/// A path not anchored at a drive or root is reached through the current
/// directory, whose own redirection decides it (no exemptions apply).
bool Vfs::is_cwd_relative(std::string_view utf8_path) noexcept {
    if (utf8_path.empty()) return false;
    const bool drive_rooted = utf8_path.size() >= 2 && utf8_path[1] == ':' &&
                              (utf8_path.size() == 2 || utf8_path[2] == '\\' || utf8_path[2] == '/');
    return !drive_rooted && utf8_path[0] != '\\' && utf8_path[0] != '/';
}

std::string Vfs::viewed_path(std::string_view path) const {
    if (!wow64_view_) return std::string(path);
    const std::string utf8_path = guest_path_to_host_utf8(path);
    const auto full = win32_full_path(utf8_path, cwd_);
    if (!full.has_value()) return std::string(path);
    return wow64_redirect(full->path, is_cwd_relative(utf8_path));
}

std::string Vfs::wow64_redirect(std::string full, bool relative) const {
    if (!wow64_view_) return full;
    // \\?\ and \\.\ forms are redirected too (probe long-prefix, nt-global).
    usize at = 0;
    if (full.size() >= 4 && full[0] == '\\' && full[1] == '\\' && (full[2] == '?' || full[2] == '.') &&
        full[3] == '\\') {
        at = 4;
    }
    std::string_view rest = std::string_view(full).substr(at);
    constexpr std::string_view kWindows = "C:\\Windows\\";
    if (!ieq_prefix(rest, kWindows)) return full;
    rest.remove_prefix(kWindows.size());
    const usize dir_at = at + kWindows.size();
    // Sysnative is the native System32, whatever the thread's switch says.
    if (names_dir(rest, "Sysnative")) {
        full.replace(dir_at, 9, "System32");
        return full;
    }
    if (t_wow64_redirection_disabled || !names_dir(rest, "System32")) return full;
    if (!relative) {
        std::string_view sub = rest.substr(8);
        if (!sub.empty()) sub.remove_prefix(1);
        for (std::string_view exempt : {"catroot", "catroot2", "drivers\\etc", "driverstore", "logfiles", "spool"}) {
            if (names_dir(sub, exempt)) return full;
        }
    }
    full.replace(dir_at, 8, "SysWOW64");
    return full;
}

Result<std::pair<IFileSystem*, std::string>, VfsError> Vfs::resolve_path(std::string_view path) {
    // *A callers hand CP1252 bytes (® = 0xAE). Host FS needs UTF-8.
    const std::string utf8_path = guest_path_to_host_utf8(path);

    // Empty CreateFile name -> ERROR_PATH_NOT_FOUND (files_core_probe
    // CreateFileW_empty_name), not INVALID_NAME from win32_full_path.
    if (utf8_path.empty()) {
        return Result<std::pair<IFileSystem*, std::string>, VfsError>::err(
            VfsError::PathNotFound);
    }

    // Learn (Naming Files) + X3: Win32 paths are normalized lexically before
    // lookup — `C:\a\missing\..\b.txt` names `C:\a\b.txt`, `C:\..\x` stays
    // under the drive root, trailing dots/spaces on the last component are
    // stripped (files_ns_probe gfp_* / CreateFile open of the same shapes).
    const auto full = win32_full_path(utf8_path, cwd_);
    if (!full.has_value()) {
        return Result<std::pair<IFileSystem*, std::string>, VfsError>::err(
            VfsError::InvalidPath);
    }

    auto parsed = parse_windows_path(wow64_redirect(full->path, is_cwd_relative(utf8_path)));

    char drive = parsed.drive_letter;
    if (drive == 0) {
        drive = default_drive_;
    }

    std::string resolved_path = std::move(parsed.relative_path);

    // Find the filesystem
    auto it = drives_.find(drive);
    if (it == drives_.end()) {
        return Result<std::pair<IFileSystem*, std::string>, VfsError>::err(
            VfsError::DriveNotMounted
        );
    }

    return Result<std::pair<IFileSystem*, std::string>, VfsError>::ok(
        std::make_pair(it->second.get(), std::move(resolved_path))
    );
}

Result<IFileSystem*, VfsError> Vfs::get_fs_for_handle(FileHandle handle) {
    auto it = handle_to_drive_.find(handle);
    if (it == handle_to_drive_.end()) {
        return Result<IFileSystem*, VfsError>::err(VfsError::InvalidHandle);
    }
    
    auto drive_it = drives_.find(it->second);
    if (drive_it == drives_.end()) {
        return Result<IFileSystem*, VfsError>::err(VfsError::DriveNotMounted);
    }
    
    return Result<IFileSystem*, VfsError>::ok(drive_it->second.get());
}

// =============================================================================
// File Operations
// =============================================================================

Result<FileHandle, VfsError> Vfs::open(
    std::string_view path,
    AccessMode access,
    ShareMode share,
    CreationDisposition disposition
) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) {
        return Result<FileHandle, VfsError>::err(resolve_result.error());
    }
    
    auto [fs, relative_path] = std::move(resolve_result.value());
    // A creating disposition would make a real file that shadows a generated
    // one, which Windows refuses for a protected system file
    // (builtin_dll_file_probe): consult the generated file first whenever
    // the mount does not hold one.
    std::shared_ptr<const std::vector<u8>> generated;
    if (disposition != CreationDisposition::OpenExisting &&
        disposition != CreationDisposition::TruncateExisting) {
        const auto host = fs->stat(relative_path);
        if (host.is_err() && (host.error() == VfsError::FileNotFound ||
                              host.error() == VfsError::PathNotFound)) {
            generated = generated_file_locked(path);
        }
    }
    auto open_result =
        generated ? Result<FileHandle, VfsError>::err(VfsError::FileNotFound)
                  : fs->open(relative_path, access, share, disposition);

    if (open_result.is_err()) {
        const auto error = open_result.error();
        if (error != VfsError::FileNotFound &&
            error != VfsError::PathNotFound) {
            return Result<FileHandle, VfsError>::err(error);
        }
        if (!generated) generated = generated_file_locked(path);
        if (!generated) {
            return Result<FileHandle, VfsError>::err(error);
        }
        if (disposition == CreationDisposition::CreateNew) {
            return Result<FileHandle, VfsError>::err(
                VfsError::PathAlreadyExists);
        }
        // A generated file is a protected system file: nothing may write
        // its data, delete it, or change its metadata or security
        // (ERROR_ACCESS_DENIED for each, builtin_dll_file_probe).
        if (disposition == CreationDisposition::CreateAlways ||
            disposition == CreationDisposition::TruncateExisting ||
            has_access(access, AccessMode::Write) ||
            has_access(access, AccessMode::Delete) ||
            has_access(access, AccessMode::WriteMetadata)) {
            return Result<FileHandle, VfsError>::err(VfsError::AccessDenied);
        }
        while (next_generated_handle_ == INVALID_FILE_HANDLE ||
               handle_to_drive_.contains(next_generated_handle_) ||
               generated_handles_.contains(next_generated_handle_)) {
            --next_generated_handle_;
        }
        const FileHandle handle = next_generated_handle_--;
        const auto name = generated_names_.find(generated_path_key(path));
        generated_handles_.emplace(
            handle, GeneratedOpenFile{std::move(generated), 0,
                                      name != generated_names_.end()
                                          ? name->second : std::string{}});
        return Result<FileHandle, VfsError>::ok(handle);
    }
    
    FileHandle handle = open_result.value();
    
    // Parse path again to get drive letter for handle mapping
    auto parsed = parse_windows_path(path);
    char drive = parsed.drive_letter ? parsed.drive_letter : default_drive_;
    handle_to_drive_[handle] = drive;
    
    return Result<FileHandle, VfsError>::ok(handle);
}

Result<void, VfsError> Vfs::close(FileHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.erase(handle) != 0) {
        return Result<void, VfsError>::ok();
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<void, VfsError>::err(fs_result.error());
    }
    
    auto close_result = fs_result.value()->close(handle);
    
    if (close_result.is_ok()) {
        handle_to_drive_.erase(handle);
    }
    
    return close_result;
}

Result<int, VfsError> Vfs::duplicate_native_fd(FileHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.contains(handle)) {
        return Result<int, VfsError>::err(VfsError::NotSupported);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<int, VfsError>::err(fs_result.error());
    }
    return fs_result.value()->duplicate_native_fd(handle);
}

Result<FileHandle, VfsError> Vfs::adopt_native_fd(
    int fd,
    AccessMode access,
    ShareMode share,
    std::string_view path,
    char drive) {
    if (fd < 0) {
        return Result<FileHandle, VfsError>::err(VfsError::InvalidArgument);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    char target = drive ? normalize_drive_letter(drive) : default_drive_;
    auto it = drives_.find(target);
    if (it == drives_.end()) {
        return Result<FileHandle, VfsError>::err(VfsError::DriveNotMounted);
    }
    auto adopted = it->second->adopt_native_fd(fd, access, share, path);
    if (adopted.is_err()) {
        return Result<FileHandle, VfsError>::err(adopted.error());
    }
    handle_to_drive_[adopted.value()] = target;
    return adopted;
}

Result<usize, VfsError> Vfs::read(FileHandle handle, std::span<u8> buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = generated_handles_.find(handle);
        it != generated_handles_.end()) {
        const u64 remaining = it->second.bytes->size() -
            std::min<u64>(it->second.position, it->second.bytes->size());
        const usize count = std::min<u64>(remaining, buffer.size());
        if (count != 0) {
            std::copy_n(it->second.bytes->data() + it->second.position,
                        count, buffer.data());
            it->second.position += count;
        }
        return Result<usize, VfsError>::ok(count);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<usize, VfsError>::err(fs_result.error());
    }
    
    return fs_result.value()->read(handle, buffer);
}

Result<usize, VfsError> Vfs::write(FileHandle handle, std::span<const u8> data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.contains(handle)) {
        return Result<usize, VfsError>::err(VfsError::AccessDenied);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<usize, VfsError>::err(fs_result.error());
    }
    
    return fs_result.value()->write(handle, data);
}

Result<u64, VfsError> Vfs::seek(FileHandle handle, i64 offset, SeekOrigin origin) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = generated_handles_.find(handle);
        it != generated_handles_.end()) {
        u64 base = 0;
        if (origin == SeekOrigin::Current) {
            base = it->second.position;
        } else if (origin == SeekOrigin::End) {
            base = it->second.bytes->size();
        }
        u64 position = base;
        if (offset < 0) {
            const u64 magnitude = static_cast<u64>(-(offset + 1)) + 1;
            if (magnitude > base) {
                return Result<u64, VfsError>::err(VfsError::InvalidArgument);
            }
            position -= magnitude;
        } else {
            const u64 delta = static_cast<u64>(offset);
            if (delta > std::numeric_limits<u64>::max() - base) {
                return Result<u64, VfsError>::err(VfsError::InvalidArgument);
            }
            position += delta;
        }
        it->second.position = position;
        return Result<u64, VfsError>::ok(it->second.position);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<u64, VfsError>::err(fs_result.error());
    }
    
    return fs_result.value()->seek(handle, offset, origin);
}

Result<void, VfsError> Vfs::truncate(FileHandle handle, u64 new_size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.contains(handle)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<void, VfsError>::err(fs_result.error());
    }
    
    return fs_result.value()->truncate(handle, new_size);
}

Result<void, VfsError> Vfs::flush(FileHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.contains(handle)) {
        return Result<void, VfsError>::ok();
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<void, VfsError>::err(fs_result.error());
    }
    
    return fs_result.value()->flush(handle);
}

Result<void, VfsError> Vfs::set_file_times(
    FileHandle handle,
    i64 atime_unix_seconds,
    i64 mtime_unix_seconds
) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.contains(handle)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<void, VfsError>::err(fs_result.error());
    }
    return fs_result.value()->set_file_times(
        handle, atime_unix_seconds, mtime_unix_seconds);
}

Result<FileInfo, VfsError> Vfs::stat(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) {
        return Result<FileInfo, VfsError>::err(resolve_result.error());
    }
    
    auto [fs, relative_path] = std::move(resolve_result.value());
    auto result = fs->stat(relative_path);
    if (result.is_ok() || (result.error() != VfsError::FileNotFound &&
                           result.error() != VfsError::PathNotFound)) {
        return result;
    }
    const auto generated = generated_file_locked(path);
    if (!generated) return result;
    return Result<FileInfo, VfsError>::ok(generated_file_info(generated->size()));
}

Result<std::string, VfsError> Vfs::final_path_for_handle(FileHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = generated_handles_.find(handle); it != generated_handles_.end()) {
        if (it->second.final_path.empty()) {
            return Result<std::string, VfsError>::err(VfsError::NotSupported);
        }
        return Result<std::string, VfsError>::ok(it->second.final_path);
    }
    auto drive_it = handle_to_drive_.find(handle);
    if (drive_it == handle_to_drive_.end()) {
        return Result<std::string, VfsError>::err(VfsError::InvalidHandle);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<std::string, VfsError>::err(fs_result.error());
    }
    auto rel = fs_result.value()->final_path(handle);
    if (rel.is_err()) return Result<std::string, VfsError>::err(rel.error());
    std::string guest{normalize_drive_letter(drive_it->second), ':', '\\'};
    for (char c : rel.value()) guest.push_back(c == '/' ? '\\' : c);
    return Result<std::string, VfsError>::ok(std::move(guest));
}

Result<FileInfo, VfsError> Vfs::lstat(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) {
        return Result<FileInfo, VfsError>::err(resolve_result.error());
    }
    auto [fs, relative_path] = std::move(resolve_result.value());
    auto result = fs->lstat(relative_path);
    if (result.is_ok() || (result.error() != VfsError::FileNotFound &&
                           result.error() != VfsError::PathNotFound)) {
        return result;
    }
    const auto generated = generated_file_locked(path);
    if (!generated) return result;
    return Result<FileInfo, VfsError>::ok(generated_file_info(generated->size()));
}

Result<FileInfo, VfsError> Vfs::stat_handle(FileHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = generated_handles_.find(handle);
        it != generated_handles_.end()) {
        return Result<FileInfo, VfsError>::ok(
            generated_file_info(it->second.bytes->size()));
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<FileInfo, VfsError>::err(fs_result.error());
    }
    return fs_result.value()->stat_handle(handle);
}

Result<void, VfsError> Vfs::set_attributes(std::string_view path, u32 attributes) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) {
        return Result<void, VfsError>::err(resolve_result.error());
    }
    auto [fs, rel] = resolve_result.value();
    auto result = fs->set_attributes(rel, attributes);
    if (result.is_err() && (result.error() == VfsError::FileNotFound ||
                            result.error() == VfsError::PathNotFound) &&
        generated_file_locked(path)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    return result;
}

Result<void, VfsError> Vfs::set_handle_attributes(FileHandle handle, u32 attributes) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.contains(handle)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) return Result<void, VfsError>::err(fs_result.error());
    return fs_result.value()->set_handle_attributes(handle, attributes);
}

Result<void, VfsError> Vfs::set_file_times_ft(FileHandle handle, const u64* creation,
                                              const u64* access, const u64* write) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (generated_handles_.contains(handle)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) return Result<void, VfsError>::err(fs_result.error());
    return fs_result.value()->set_file_times_ft(handle, creation, access, write);
}

Result<void, VfsError> Vfs::lock_range(FileHandle handle, u64 offset, u64 length,
                                       bool exclusive) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) return Result<void, VfsError>::err(fs_result.error());
    return fs_result.value()->lock_range(handle, offset, length, exclusive);
}

Result<void, VfsError> Vfs::unlock_range(FileHandle handle, u64 offset, u64 length) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) return Result<void, VfsError>::err(fs_result.error());
    return fs_result.value()->unlock_range(handle, offset, length);
}

Result<std::vector<DirectoryEntry>, VfsError> Vfs::list_dir(std::string_view path) {
    std::unique_lock<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) {
        return Result<std::vector<DirectoryEntry>, VfsError>::err(resolve_result.error());
    }
    
    auto [fs, relative_path] = std::move(resolve_result.value());
    // Directory enumeration can issue thousands of host metadata queries.
    // Retain the resolved mount, but do not block unrelated file I/O while
    // the backend enumerates. Unmount/remount must not destroy or redirect
    // an enumeration already in progress.
    std::shared_ptr<IFileSystem> mount;
    for (const auto& [letter, candidate] : drives_) {
        if (candidate.get() == fs) {
            mount = candidate;
            break;
        }
    }
    lock.unlock();
    auto listed = mount->list_dir(relative_path);
    if (listed.is_err()) return listed;

    // Generated files of this directory the mount does not hold: those the
    // generated listing names (built-in DLLs in System32, loaded or not,
    // spelled as the source spells them), then any other published one. A
    // real file of the same name wins.
    lock.lock();
    auto entries = std::move(listed.value());
    std::unordered_set<std::string> seen;
    const auto lower = [](std::string_view name) {
        std::string out(name);
        for (char& c : out) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
    };
    for (const auto& entry : entries) seen.insert(lower(entry.name));
    const auto add = [&](std::string_view name, u64 size) {
        if (name.empty() || !seen.insert(lower(name)).second) return;
        entries.push_back(DirectoryEntry{std::string(name),
                                         generated_file_info(size)});
    };
    if (generated_listing_) {
        for (const auto& [name, size] :
             generated_listing_(generated_spelling(path))) {
            add(name, size);
        }
    }
    std::string dir_key = generated_path_key(path);
    while (!dir_key.empty() && (dir_key.back() == '/' || dir_key.back() == '\\')) {
        dir_key.pop_back();
    }
    for (const auto& [key, bytes] : generated_files_) {
        const auto sep = key.find_last_of("/\\");
        if (sep == std::string::npos || key.compare(0, sep, dir_key) != 0 ||
            sep != dir_key.size()) {
            continue;
        }
        const auto spelled = generated_names_.find(key);
        const std::string& full =
            spelled != generated_names_.end() ? spelled->second : key;
        const auto leaf = full.find_last_of("/\\");
        add(leaf == std::string::npos ? std::string_view(full)
                                      : std::string_view(full).substr(leaf + 1),
            bytes->size());
    }
    return Result<std::vector<DirectoryEntry>, VfsError>::ok(std::move(entries));
}

Result<std::vector<DirectoryEntry>, VfsError>
Vfs::list_dir_handle(FileHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<std::vector<DirectoryEntry>, VfsError>::err(
            fs_result.error());
    }
    return fs_result.value()->list_dir_handle(handle);
}

Result<std::pair<IFileSystem*, std::string>, VfsError>
Vfs::resolve_components(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    return resolve_path(path);
}

Result<std::string, VfsError> Vfs::resolve_host_path(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolved = resolve_path(path);
    if (resolved.is_err()) {
        return Result<std::string, VfsError>::err(resolved.error());
    }

    auto [fs, rel] = std::move(resolved.value());
    return fs->resolve_host_path(rel);
}

Result<void, VfsError> Vfs::create_dir(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) {
        return Result<void, VfsError>::err(resolve_result.error());
    }
    
    auto [fs, relative_path] = std::move(resolve_result.value());
    return fs->create_dir(relative_path);
}

Result<void, VfsError> Vfs::remove(std::string_view path) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto resolve_result = resolve_path(path);
    if (resolve_result.is_err()) {
        return Result<void, VfsError>::err(resolve_result.error());
    }
    
    auto [fs, relative_path] = std::move(resolve_result.value());
    auto result = fs->remove(relative_path);
    // A generated file is a protected system file (builtin_dll_file_probe:
    // DELETE access is ERROR_ACCESS_DENIED).
    if (result.is_err() && (result.error() == VfsError::FileNotFound ||
                            result.error() == VfsError::PathNotFound) &&
        generated_file_locked(path)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    return result;
}

Result<void, VfsError> Vfs::rename(std::string_view old_path, std::string_view new_path) {
    // Same CP1252-to-UTF-8 normalization as open/stat. MoveFileA of a path
    // that contains a CP1252 byte (® = 0xAE) must find the UTF-8 host path
    // CreateFileA wrote. Raw 0xAE is invalid UTF-8 and yields ENOENT.
    const std::string old_utf8 = guest_path_to_host_utf8(old_path);
    const std::string new_utf8 = guest_path_to_host_utf8(new_path);

    std::lock_guard<std::mutex> lock(mutex_);

    // Learn (Naming Files) + X5: relative / drive-relative names resolve
    // against the process CWD (files_ns_probe MoveFileW_relative /
    // MoveFileW_drive_relative). win32_full_path matches GetFullPathNameW.
    const auto old_full = win32_full_path(old_utf8, cwd_);
    const auto new_full = win32_full_path(new_utf8, cwd_);
    if (!old_full.has_value() || !new_full.has_value()) {
        return Result<void, VfsError>::err(VfsError::InvalidPath);
    }

    auto old_parsed = parse_windows_path(old_full->path);
    auto new_parsed = parse_windows_path(new_full->path);

    char old_drive = old_parsed.drive_letter ? old_parsed.drive_letter : default_drive_;
    char new_drive = new_parsed.drive_letter ? new_parsed.drive_letter : default_drive_;
    
    if (old_drive != new_drive) {
        // This is the rename primitive, without copy/delete fallback. Learn
        // MoveFileExW requires COPY_ALLOWED for a cross-volume file move;
        // its API owner performs that fallback. Preserve the cross-volume
        // error here rather than reporting an unsupported operation.
        return Result<void, VfsError>::err(VfsError::NotSameDevice);
    }
    
    auto it = drives_.find(old_drive);
    if (it == drives_.end()) {
        return Result<void, VfsError>::err(VfsError::DriveNotMounted);
    }

    // A generated file is a protected system file at either end of a move
    // (builtin_dll_file_probe: DELETE / write access is ERROR_ACCESS_DENIED).
    const auto generated_end = [&](std::string_view full,
                                   std::string_view relative) {
        const auto host = it->second->stat(relative);
        return host.is_err() &&
               (host.error() == VfsError::FileNotFound ||
                host.error() == VfsError::PathNotFound) &&
               generated_file_locked(full) != nullptr;
    };
    if (generated_end(old_full->path, old_parsed.relative_path) ||
        generated_end(new_full->path, new_parsed.relative_path)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }

    return it->second->rename(old_parsed.relative_path, new_parsed.relative_path);
}

Result<void, VfsError> Vfs::rename_open(
    FileHandle handle,
    std::string_view new_path,
    bool replace_existing
) {
    const std::string new_utf8 = guest_path_to_host_utf8(new_path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto fs_result = get_fs_for_handle(handle);
    if (fs_result.is_err()) {
        return Result<void, VfsError>::err(fs_result.error());
    }
    auto drive_it = handle_to_drive_.find(handle);
    if (drive_it == handle_to_drive_.end()) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }
    const auto full = win32_full_path(new_utf8, cwd_);
    if (!full.has_value()) {
        return Result<void, VfsError>::err(VfsError::InvalidPath);
    }
    auto parsed = parse_windows_path(full->path);
    const char new_drive = parsed.drive_letter ? parsed.drive_letter : default_drive_;
    if (new_drive != drive_it->second) {
        return Result<void, VfsError>::err(VfsError::NotSameDevice);
    }
    return fs_result.value()->rename_open(
        handle, parsed.relative_path, replace_existing);
}

Result<void, VfsError> Vfs::create_hard_link(
    std::string_view new_path,
    std::string_view existing_path
) {
    const std::string new_utf8 = guest_path_to_host_utf8(new_path);
    const std::string existing_utf8 = guest_path_to_host_utf8(existing_path);
    auto new_parsed = parse_windows_path(new_utf8);
    auto existing_parsed = parse_windows_path(existing_utf8);

    std::lock_guard<std::mutex> lock(mutex_);
    const char new_drive = new_parsed.drive_letter
        ? new_parsed.drive_letter : default_drive_;
    const char existing_drive = existing_parsed.drive_letter
        ? existing_parsed.drive_letter : default_drive_;
    if (new_drive != existing_drive) {
        return Result<void, VfsError>::err(VfsError::NotSameDevice);
    }

    auto it = drives_.find(new_drive);
    if (it == drives_.end()) {
        return Result<void, VfsError>::err(VfsError::DriveNotMounted);
    }
    return it->second->create_hard_link(
        new_parsed.relative_path, existing_parsed.relative_path);
}

} // namespace vitreo::capsule

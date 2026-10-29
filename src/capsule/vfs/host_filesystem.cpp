/// @file host_filesystem.cpp
/// @brief Host filesystem implementation using POSIX APIs

#include "host_filesystem.hpp"

#include "win_path.hpp"
#include "share_access.hpp"
#include "platform/host_volume_space.hpp"

#include <cerrno>
#include <optional>
#include <chrono>
#include <cstring>
#include <ctime>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/attr.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>

namespace vitreo::capsule {

namespace {

[[nodiscard]] bool io_stats_enabled() noexcept {
    static const bool enabled = (std::getenv("VITREO_IO_STATS") != nullptr);
    return enabled;
}

/// Existence check that does not follow the final symlink, so a mapped drive
/// entry counts as present even when its target is unreachable.
[[nodiscard]] bool path_exists(const std::string& path) noexcept {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

/// Volumes that report case-insensitivity already match every spelling. ISO9660
/// and UDF media answer EINVAL rather than a value, so an unanswered query is
/// treated as case-sensitive and the caller searches the directory.
[[nodiscard]] bool directory_is_case_sensitive(const std::string& dir) noexcept {
    return ::pathconf(dir.c_str(), _PC_CASE_SENSITIVE) != 0;
}

[[nodiscard]] std::optional<std::string> find_case_insensitive_child(
    const std::string& dir,
    std::string_view name) {
    DIR* stream = ::opendir(dir.c_str());
    if (stream == nullptr) {
        return std::nullopt;
    }
    std::optional<std::string> match;
    while (const dirent* entry = ::readdir(stream)) {
        if (case_insensitive_equals(entry->d_name, name)) {
            match.emplace(entry->d_name);
            break;
        }
    }
    ::closedir(stream);
    return match;
}

struct IoStats {
    std::atomic<u64> read_calls{0};
    std::atomic<u64> write_calls{0};
    std::atomic<u64> flush_calls{0};

    std::atomic<u64> read_bytes{0};
    std::atomic<u64> write_bytes{0};

    std::atomic<u64> read_ns{0};
    std::atomic<u64> write_ns{0};
    std::atomic<u64> flush_ns{0};
};

IoStats g_io_stats;
constexpr u64 kLiveReportStepBytes = 8ull * 1024ull * 1024ull;
std::atomic<u64> g_next_live_report_write_bytes{kLiveReportStepBytes};

inline u64 elapsed_ns(std::chrono::steady_clock::time_point start,
                      std::chrono::steady_clock::time_point end) noexcept {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count());
}

void append_io_stats_file_line(const char* line) {
    FILE* fp = std::fopen("/tmp/vitreo_iostats.log", "a");
    if (!fp) {
        return;
    }
    std::fputs(line, fp);
    std::fputc('\n', fp);
    std::fclose(fp);
}

void print_io_stats_if_enabled() {
    if (!io_stats_enabled()) {
        return;
    }

    const u64 reads = g_io_stats.read_calls.load(std::memory_order_relaxed);
    const u64 writes = g_io_stats.write_calls.load(std::memory_order_relaxed);
    const u64 flushes = g_io_stats.flush_calls.load(std::memory_order_relaxed);
    const u64 read_b = g_io_stats.read_bytes.load(std::memory_order_relaxed);
    const u64 write_b = g_io_stats.write_bytes.load(std::memory_order_relaxed);
    const u64 read_t = g_io_stats.read_ns.load(std::memory_order_relaxed);
    const u64 write_t = g_io_stats.write_ns.load(std::memory_order_relaxed);
    const u64 flush_t = g_io_stats.flush_ns.load(std::memory_order_relaxed);

    if (reads == 0 && writes == 0 && flushes == 0) {
        return;
    }

    char line[512];
    std::snprintf(
        line, sizeof(line),
        "[vfs-io] read_calls=%llu read_bytes=%llu read_ms=%.3f "
        "write_calls=%llu write_bytes=%llu write_ms=%.3f "
        "flush_calls=%llu flush_ms=%.3f",
        static_cast<unsigned long long>(reads),
        static_cast<unsigned long long>(read_b),
        static_cast<double>(read_t) / 1e6,
        static_cast<unsigned long long>(writes),
        static_cast<unsigned long long>(write_b),
        static_cast<double>(write_t) / 1e6,
        static_cast<unsigned long long>(flushes),
        static_cast<double>(flush_t) / 1e6);
    std::fprintf(stderr, "%s\n", line);
    append_io_stats_file_line(line);
}

void maybe_print_live_io_stats() {
    if (!io_stats_enabled()) {
        return;
    }

    const u64 write_b = g_io_stats.write_bytes.load(std::memory_order_relaxed);
    u64 next = g_next_live_report_write_bytes.load(std::memory_order_relaxed);
    if (write_b < next) {
        return;
    }

    // Move threshold forward in fixed steps; only one thread reports a step.
    while (write_b >= next) {
        const u64 desired = next + kLiveReportStepBytes;
        if (g_next_live_report_write_bytes.compare_exchange_weak(
                next, desired, std::memory_order_relaxed, std::memory_order_relaxed)) {
            const u64 reads = g_io_stats.read_calls.load(std::memory_order_relaxed);
            const u64 writes = g_io_stats.write_calls.load(std::memory_order_relaxed);
            const u64 flushes = g_io_stats.flush_calls.load(std::memory_order_relaxed);
            const u64 read_b = g_io_stats.read_bytes.load(std::memory_order_relaxed);
            const u64 read_t = g_io_stats.read_ns.load(std::memory_order_relaxed);
            const u64 write_t = g_io_stats.write_ns.load(std::memory_order_relaxed);
            const u64 flush_t = g_io_stats.flush_ns.load(std::memory_order_relaxed);

            char line[512];
            std::snprintf(
                line, sizeof(line),
                "[vfs-io-live] read_calls=%llu read_bytes=%llu read_ms=%.3f "
                "write_calls=%llu write_bytes=%llu write_ms=%.3f "
                "flush_calls=%llu flush_ms=%.3f",
                static_cast<unsigned long long>(reads),
                static_cast<unsigned long long>(read_b),
                static_cast<double>(read_t) / 1e6,
                static_cast<unsigned long long>(writes),
                static_cast<unsigned long long>(write_b),
                static_cast<double>(write_t) / 1e6,
                static_cast<unsigned long long>(flushes),
                static_cast<double>(flush_t) / 1e6);
            std::fprintf(stderr, "%s\n", line);
            append_io_stats_file_line(line);
            return;
        }
    }
}

} // namespace

// =============================================================================
// FileTime Utilities
// =============================================================================

/// Convert Unix time_t to Windows FILETIME
/// FILETIME is 100-nanosecond intervals since January 1, 1601
/// Unix time is seconds since January 1, 1970
/// Difference is 11644473600 seconds
static FileTime unix_time_to_filetime(time_t t) noexcept {
    constexpr i64 UNIX_TO_FILETIME_OFFSET = 11644473600LL;
    constexpr i64 FILETIME_TICKS_PER_SECOND = 10000000LL;
    
    return static_cast<FileTime>(
        (static_cast<i64>(t) + UNIX_TO_FILETIME_OFFSET) * FILETIME_TICKS_PER_SECOND
    );
}

FileTime current_file_time() noexcept {
    return unix_time_to_filetime(std::time(nullptr));
}

// =============================================================================
// HostFileSystem Implementation
// =============================================================================

HostFileSystem::HostFileSystem(std::string root_path) 
    : root_(std::move(root_path)) {
    // Ensure root path doesn't end with a slash (unless it's just "/")
    while (root_.size() > 1 && root_.back() == '/') {
        root_.pop_back();
    }

    if (io_stats_enabled()) {
        std::fprintf(stderr,
            "[vfs-io] enabled live_step_mib=%llu\n",
            static_cast<unsigned long long>(kLiveReportStepBytes / (1024ull * 1024ull)));
        // Fresh file per run so users can always inspect deterministic output.
        FILE* fp = std::fopen("/tmp/vitreo_iostats.log", "w");
        if (fp) {
            std::fprintf(fp,
                "[vfs-io] enabled live_step_mib=%llu\n",
                static_cast<unsigned long long>(kLiveReportStepBytes / (1024ull * 1024ull)));
            std::fclose(fp);
        }
    }
}

HostFileSystem::~HostFileSystem() {
    // Close all open files
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [handle, state] : open_files_) {
        if (state && state->fd >= 0) {
            ::close(state->fd);
            state->fd = -1;
        }
    }
    open_files_.clear();
    print_io_stats_if_enabled();
}

std::string HostFileSystem::to_host_path(std::string_view relative) const {
    if (relative.empty()) {
        return root_;
    }
    
    // Skip leading slashes in relative path
    while (!relative.empty() && relative.front() == '/') {
        relative.remove_prefix(1);
    }
    
    if (relative.empty()) {
        return root_;
    }
    
    std::string direct = root_ + "/" + std::string(relative);
    if (path_exists(direct)) {
        return direct;
    }
    return resolve_case_insensitively(direct);
}

std::string HostFileSystem::resolve_case_insensitively(
    const std::string& host_path) const {
    // A miss inside a case-insensitive directory is a genuine miss: no other
    // spelling could have matched, so skip the per-component walk. Guests probe
    // for absent files constantly, and that keeps the common case at one extra
    // stat rather than one per path component.
    if (const usize parent_end = host_path.rfind('/');
        parent_end != std::string::npos && parent_end >= root_.size()) {
        const std::string parent = host_path.substr(0, parent_end);
        if (path_exists(parent) && !directory_is_case_sensitive(parent)) {
            return host_path;
        }
    }

    std::string resolved = root_;
    usize cursor = root_.size();
    while (cursor < host_path.size()) {
        while (cursor < host_path.size() && host_path[cursor] == '/') {
            ++cursor;
        }
        if (cursor >= host_path.size()) {
            break;
        }

        const usize end = host_path.find('/', cursor);
        const usize length =
            (end == std::string::npos) ? std::string::npos : end - cursor;
        const std::string_view component =
            std::string_view(host_path).substr(cursor, length);

        std::string candidate = resolved + "/" + std::string(component);
        if (!path_exists(candidate)) {
            // Stop rewriting as soon as a component cannot be matched: the
            // remainder names something that does not exist yet, and a creating
            // caller must get the spelling it asked for.
            if (!directory_is_case_sensitive(resolved)) {
                return resolved + "/" + host_path.substr(cursor);
            }
            const auto match = find_case_insensitive_child(resolved, component);
            if (!match.has_value()) {
                return resolved + "/" + host_path.substr(cursor);
            }
            candidate = resolved + "/" + *match;
        }

        resolved = std::move(candidate);
        if (end == std::string::npos) {
            break;
        }
        cursor = end;
    }
    return resolved;
}

std::shared_ptr<OpenFileState> HostFileSystem::get_open_file(FileHandle handle) {
    auto it = open_files_.find(handle);
    if (it == open_files_.end()) {
        return {};
    }
    return it->second;
}

FileHandle HostFileSystem::allocate_handle() {
    return next_handle_++;
}

VfsError HostFileSystem::errno_to_vfs_error_for(int err,
                                                 const std::string& host_path) {
    if (err != ENOENT) return errno_to_vfs_error(err);
    // ENOENT covers both "leaf missing" and "a directory along the way is
    // missing". Windows separates them, so decide by looking at the parent.
    std::error_code ec;
    const auto parent = std::filesystem::path(host_path).parent_path();
    // An empty parent means the path had no directory component, so the leaf
    // is what is missing. is_directory() reports failures through `ec`; a
    // query error is not evidence the parent is absent, so only a definitive
    // "not a directory" answer downgrades this to PathNotFound.
    if (parent.empty()) return VfsError::FileNotFound;
    if (std::filesystem::is_directory(parent, ec)) return VfsError::FileNotFound;

    // Not a directory. is_directory() reports an absent path by returning
    // false *and* setting ec to no_such_file_or_directory — that is the
    // missing-parent case, and it must stay PathNotFound. Any other error
    // means the query itself failed and we cannot tell, so prefer the more
    // specific leaf-missing answer over claiming the whole path is gone.
    if (ec && ec != std::errc::no_such_file_or_directory) {
        return VfsError::FileNotFound;
    }
    return VfsError::PathNotFound;
}

VfsError HostFileSystem::errno_to_vfs_error(int err) {
    switch (err) {
        // Callers with a path should prefer errno_to_vfs_error_for(), which
        // can tell a missing leaf from a missing directory. Without a path the
        // stricter PATH_NOT_FOUND is the safe answer.
        case ENOENT:        return VfsError::PathNotFound;
        case EEXIST:        return VfsError::PathAlreadyExists;
        case EACCES:        return VfsError::AccessDenied;
        case EPERM:         return VfsError::AccessDenied;
        case ENOTDIR:       return VfsError::NotADirectory;
        case EISDIR:        return VfsError::NotAFile;
        case ENOTEMPTY:     return VfsError::DirectoryNotEmpty;
        case EMFILE:        return VfsError::TooManyOpenFiles;
        case ENFILE:        return VfsError::TooManyOpenFiles;
        case ENOSPC:        return VfsError::DiskFull;
        case ENOMEM:        return VfsError::OutOfMemory;
        case EINVAL:        return VfsError::InvalidArgument;
        case EBADF:         return VfsError::InvalidHandle;
        case EIO:           return VfsError::IoError;
#ifdef EILSEQ
        case EILSEQ:        return VfsError::InvalidArgument;
#endif
        default:            return VfsError::InternalError;
    }
}

namespace {

// Win32 attributes a host file cannot express natively live in an extended
// attribute. READONLY on a FILE is the host write permission (so host-level
// opens enforce it); on a directory it is advisory on Windows too (files can
// still be created in it: files_meta_probe create_in_readonly_dir), so it is
// stored only.
constexpr const char* kDosAttrXattr = "com.vitreo.dosattrib";
constexpr u32 kAttrReadOnly = 0x1, kAttrHidden = 0x2, kAttrSystem = 0x4,
              kAttrDirectory = 0x10, kAttrArchive = 0x20, kAttrNormal = 0x80,
              kAttrTemporary = 0x100, kAttrOffline = 0x1000,
              kAttrNotIndexed = 0x2000;
// Learn (SetFileAttributesW): the settable set. DIRECTORY / COMPRESSED /
// ENCRYPTED etc. are ignored (files_meta_probe SetFileAttributesW_directory_bit
// and _compressed read back NORMAL).
constexpr u32 kSettable = kAttrReadOnly | kAttrHidden | kAttrSystem |
                          kAttrArchive | kAttrNormal | kAttrTemporary |
                          kAttrOffline | kAttrNotIndexed;

std::optional<u32> read_dos_attrs(const std::string& host_path, int fd) {
    u32 v = 0;
    const ssize_t n = fd >= 0
        ? ::fgetxattr(fd, kDosAttrXattr, &v, sizeof v, 0, 0)
        : ::getxattr(host_path.c_str(), kDosAttrXattr, &v, sizeof v, 0, 0);
    if (n != static_cast<ssize_t>(sizeof v)) return std::nullopt;
    return v;
}

/// Normalize a requested word the way SetFileAttributes stores it: only
/// settable bits; NORMAL only when alone; nothing at all reads as NORMAL
/// (files_meta_probe SetFileAttributesW_zero / _normal_plus_hidden).
u32 normalize_settable(u32 attrs) {
    u32 v = attrs & kSettable;
    if ((v & ~kAttrNormal) != 0) v &= ~kAttrNormal;
    if (v == 0) v = kAttrNormal;
    return v;
}

bool write_dos_attrs(const std::string& host_path, int fd, u32 v) {
    const int r = fd >= 0
        ? ::fsetxattr(fd, kDosAttrXattr, &v, sizeof v, 0, 0)
        : ::setxattr(host_path.c_str(), kDosAttrXattr, &v, sizeof v, 0, 0);
    return r == 0;
}

/// Apply a normalized word to a host object: xattr + (files) write bits.
bool apply_dos_attrs(const std::string& host_path, int fd, const struct stat& st,
                     u32 attrs) {
    const u32 v = normalize_settable(attrs);
    const bool is_dir = S_ISDIR(st.st_mode);
    mode_t mode = st.st_mode & 07777;
    if (v & kAttrReadOnly) {
        mode &= ~static_cast<mode_t>(S_IWUSR | S_IWGRP | S_IWOTH);
    } else {
        mode |= S_IWUSR;
    }
    auto apply_mode = [&]() {
        return (fd >= 0 ? ::fchmod(fd, mode) : ::chmod(host_path.c_str(), mode)) == 0;
    };
    // The attribute xattr cannot be rewritten while the host file is still
    // read-only, so clearing READONLY together with HIDDEN/SYSTEM must make
    // the file writable first; otherwise the old HIDDEN bit survived
    // SetFileAttributes(FILE_ATTRIBUTE_NORMAL) (s4_copyfile_probe cfw_dst_hidden).
    const bool clearing_readonly =
        !is_dir && (v & kAttrReadOnly) == 0 && (st.st_mode & S_IWUSR) == 0;
    if (clearing_readonly && !apply_mode()) return false;
    const bool stored = write_dos_attrs(host_path, fd, v);
    if (!is_dir && !clearing_readonly && !apply_mode()) return false;
    // A volume without extended attributes can still hold the bits that the
    // mode expresses (READONLY) or that are the default (ARCHIVE / NORMAL).
    return stored || (v & ~(kAttrReadOnly | kAttrArchive | kAttrNormal)) == 0;
}

FileTime timespec_to_filetime(const struct timespec& ts) {
    constexpr i64 kOffset = 11644473600LL;
    return static_cast<FileTime>((static_cast<i64>(ts.tv_sec) + kOffset) * 10000000LL +
                                 ts.tv_nsec / 100);
}

struct timespec filetime_to_timespec(u64 ft) {
    constexpr i64 kOffset = 11644473600LL * 10000000LL;
    const i64 rel = static_cast<i64>(ft) - kOffset;
    i64 sec = rel / 10000000LL;
    i64 rem = rel % 10000000LL;
    if (rem < 0) { rem += 10000000LL; sec -= 1; }
    struct timespec ts{};
    ts.tv_sec = static_cast<time_t>(sec);
    ts.tv_nsec = static_cast<long>(rem * 100);
    return ts;
}

bool host_is_readonly(const std::string& host_path, const struct stat& st) {
    if (!S_ISDIR(st.st_mode)) return (st.st_mode & S_IWUSR) == 0;
    const auto v = read_dos_attrs(host_path, -1);
    return v && (*v & kAttrReadOnly) != 0;
}

}  // namespace

FileInfo HostFileSystem::stat_to_file_info(const struct stat& st,
                                           std::optional<u32> stored) {
    FileInfo info;
    info.size = static_cast<u64>(st.st_size);
    info.volume_id = static_cast<u64>(st.st_dev);
    info.file_id = static_cast<u64>(st.st_ino);
    info.link_count = static_cast<u32>(st.st_nlink);

    // Attribute word (Learn: File Attribute Constants). A file Windows
    // created carries ARCHIVE (files_meta_probe GetFileAttributesW_new_file
    // = 0x20); NORMAL is valid only alone.
    u32 a = 0;
    if (S_ISDIR(st.st_mode)) {
        a = kAttrDirectory;
        if (stored) a |= (*stored & kSettable & ~kAttrNormal);
    } else {
        a = stored ? (*stored & kSettable) : kAttrArchive;
        a &= ~kAttrReadOnly;
        if (!(st.st_mode & S_IWUSR)) a |= kAttrReadOnly;
        if ((a & ~kAttrNormal) != 0) a &= ~kAttrNormal;
        if (a == 0) a = kAttrNormal;
    }
    info.attributes = static_cast<FileAttributes>(a);

    // FILETIME carries 100 ns units; keep the host's sub-second part.
    info.creation_time = timespec_to_filetime(st.st_birthtimespec);
    info.last_access_time = timespec_to_filetime(st.st_atimespec);
    info.last_write_time = timespec_to_filetime(st.st_mtimespec);
    info.change_time = timespec_to_filetime(st.st_ctimespec);
    return info;
}

Result<void, VfsError> HostFileSystem::set_attributes(std::string_view path,
                                                      u32 attributes) {
    const std::string host_path = to_host_path(path);
    struct stat st{};
    if (::stat(host_path.c_str(), &st) < 0) {
        return Result<void, VfsError>::err(errno_to_vfs_error_for(errno, host_path));
    }
    if (!apply_dos_attrs(host_path, -1, st, attributes)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::set_handle_attributes(FileHandle handle,
                                                             u32 attributes) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) return Result<void, VfsError>::err(VfsError::InvalidHandle);
    std::lock_guard<std::mutex> state_lock(state->mutex);
    struct stat st{};
    if (state->fd < 0 || ::fstat(state->fd, &st) < 0) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }
    if (!apply_dos_attrs(std::string(), state->fd, st, attributes)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::set_file_times_ft(FileHandle handle,
                                                         const u64* creation,
                                                         const u64* access,
                                                         const u64* write) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) return Result<void, VfsError>::err(VfsError::InvalidHandle);
    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) return Result<void, VfsError>::err(VfsError::InvalidHandle);

    constexpr u64 kSuspend = ~0ull;
    // Learn (SetFileTime): a zero FILETIME, like a NULL pointer, leaves the
    // time unchanged (files_meta_probe SetFileTime_zero_nochange); all ones
    // stops I/O through this handle from moving it (write_after_write).
    struct stat now{};
    (void)::fstat(state->fd, &now);
    struct timespec times[2];
    times[0].tv_nsec = UTIME_OMIT;
    times[1].tv_nsec = UTIME_OMIT;
    if (access != nullptr && *access != 0) {
        if (*access == kSuspend) {
            state->hold_atime = true;
            state->held_atime = now.st_atimespec;
        } else {
            times[0] = filetime_to_timespec(*access);
            state->held_atime = times[0];
        }
    }
    if (write != nullptr && *write != 0) {
        if (*write == kSuspend) {
            state->hold_mtime = true;
            state->held_mtime = now.st_mtimespec;
        } else {
            times[1] = filetime_to_timespec(*write);
            state->held_mtime = times[1];
        }
    }
    if ((times[0].tv_nsec != UTIME_OMIT || times[1].tv_nsec != UTIME_OMIT) &&
        ::futimens(state->fd, times) < 0) {
        return Result<void, VfsError>::err(errno_to_vfs_error(errno));
    }
    if (creation != nullptr && *creation != 0 && *creation != kSuspend) {
        // The creation (birth) time is settable on APFS/HFS+ through
        // setattrlist; it is the FILETIME the guest asked for, pre-1970
        // included (files_meta_probe SetFileTime_all).
        struct attrlist al{};
        al.bitmapcount = ATTR_BIT_MAP_COUNT;
        al.commonattr = ATTR_CMN_CRTIME;
        struct timespec crtime = filetime_to_timespec(*creation);
        if (::fsetattrlist(state->fd, &al, &crtime, sizeof crtime, 0) < 0) {
            return Result<void, VfsError>::err(errno_to_vfs_error(errno));
        }
    }
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::lock_range(FileHandle handle, u64 offset,
                                                  u64 length, bool exclusive) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state || state->fd < 0) return Result<void, VfsError>::err(VfsError::InvalidHandle);
    const auto r = range_lock(state->dev, state->ino,
                              reinterpret_cast<u64>(state.get()), offset, length,
                              exclusive);
    if (r != RangeLockResult::Ok) {
        return Result<void, VfsError>::err(VfsError::LockViolation);
    }
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::unlock_range(FileHandle handle, u64 offset,
                                                    u64 length) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state || state->fd < 0) return Result<void, VfsError>::err(VfsError::InvalidHandle);
    const auto r = range_unlock(state->dev, state->ino,
                                reinterpret_cast<u64>(state.get()), offset, length);
    if (r != RangeLockResult::Ok) {
        return Result<void, VfsError>::err(VfsError::NotLocked);
    }
    return Result<void, VfsError>::ok();
}

// =============================================================================
// File Operations
// =============================================================================

namespace {

// Windows enforces a handle's share mode kernel-wide: DeleteFile fails with
// ERROR_SHARING_VIOLATION while any handle in any process has the file open
// without FILE_SHARE_DELETE, or has it mapped. Guest processes are separate
// host processes here, so the enforcement must live in something the host
// kernel shares between them. flock(2) is that primitive: the lock belongs
// to the open file description, so it follows dup()'d descriptors (a section
// mapped over the file keeps it alive), it is released when the last
// descriptor closes — including on SIGKILL — and it conflicts across
// descriptions inside one process as well as between processes.
//
// A handle opened without delete sharing holds a shared lock; DeleteFile
// probes with a non-blocking exclusive lock and reports a sharing violation
// when the probe is refused.
void hold_no_delete_lock(int fd, ShareMode share) noexcept {
    if ((static_cast<u32>(share) & static_cast<u32>(ShareMode::Delete)) != 0) {
        return;
    }
    // Best effort: a refused shared lock means a DeleteFile probe is in
    // flight on this very file, which is about to disappear anyway.
    (void)::flock(fd, LOCK_SH | LOCK_NB);
}

bool other_handles_deny_delete(const std::string& host_path) noexcept {
    const int fd = ::open(host_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return false;
    const bool denied =
        ::flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
    ::close(fd);
    return denied;
}

} // namespace

Result<FileHandle, VfsError> HostFileSystem::open(
    std::string_view path,
    AccessMode access,
    ShareMode share,
    CreationDisposition disposition
) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::string host_path = to_host_path(path);
    
    // Build open flags
    int flags = 0;
    
    // Access mode.  NOTE: has_access() is a bitwise-ANY test and ReadWrite is
    // (Read | Write), so has_access(access, ReadWrite) is true even for a
    // read-only request — using it here would force O_RDWR and fail (EACCES)
    // on read-only files such as CD-ROM media.  Test the individual bits.
    const bool want_write = has_access(access, AccessMode::Write);
    const bool want_read  = has_access(access, AccessMode::Read);
    if (want_write && want_read) {
        flags |= O_RDWR;
    } else if (want_write) {
        flags |= O_WRONLY;
    } else {
        flags |= O_RDONLY;
    }
    
    // Creation disposition
    switch (disposition) {
        case CreationDisposition::CreateNew:
            flags |= O_CREAT | O_EXCL;
            break;
        case CreationDisposition::CreateAlways:
            flags |= O_CREAT | O_TRUNC;
            break;
        case CreationDisposition::OpenExisting:
            // No additional flags
            break;
        case CreationDisposition::OpenAlways:
            flags |= O_CREAT;
            break;
        case CreationDisposition::TruncateExisting:
            flags |= O_TRUNC;
            break;
    }
    
    // Share access (Learn: CreateFileW dwShareMode) is arbitrated BEFORE
    // the open can truncate anything: a CREATE_ALWAYS / TRUNCATE_EXISTING
    // that loses a sharing check must leave the data intact.
    const u32 share_access_bits =
        ((has_access(access, AccessMode::Read) ||
          has_access(access, AccessMode::Execute)) ? share_bits::Read : 0u) |
        (has_access(access, AccessMode::Write) ? share_bits::Write : 0u) |
        (has_access(access, AccessMode::Delete) ? share_bits::Delete : 0u);
    const u32 share_allow = static_cast<u32>(share) & share_bits::All;
    ShareToken token = 0;
    u64 token_dev = 0, token_ino = 0;
    if (share_access_bits != 0) {
        struct stat pre{};
        if (::stat(host_path.c_str(), &pre) == 0) {
            auto t = share_access_acquire(static_cast<u64>(pre.st_dev),
                                          static_cast<u64>(pre.st_ino),
                                          share_access_bits, share_allow);
            if (!t) {
                return Result<FileHandle, VfsError>::err(
                    VfsError::SharingViolation);
            }
            token = *t;
            token_dev = static_cast<u64>(pre.st_dev);
            token_ino = static_cast<u64>(pre.st_ino);
        }
    }

    // Open the file
    int fd = ::open(host_path.c_str(), flags, 0644);
    if (fd < 0 && errno == EISDIR &&
        disposition == CreationDisposition::OpenExisting) {
        // A directory handle is metadata-only at the host boundary. POSIX
        // rejects O_WRONLY/O_RDWR directory opens with EISDIR, while Win32
        // CreateFile permits requested directory access when the caller uses
        // FILE_FLAG_BACKUP_SEMANTICS. Open a read-only descriptor here and
        // preserve the original Windows desired-access mask in FileManager's
        // process handle; the Win32 layer remains responsible for allowing or
        // rejecting the directory handle.
        fd = ::open(host_path.c_str(), O_RDONLY);
    }
    // A missing parent directory is not "a valid path to a writable
    // location" (Learn: CreateFileW CREATE_NEW/OPEN_ALWAYS rows): Windows
    // fails ERROR_PATH_NOT_FOUND and creates nothing
    // (files_core_probe: CreateFileW_missing_parent / parent_not_created).
    if (fd < 0) {
        const int open_errno = errno;
        share_access_release(token);
        return Result<FileHandle, VfsError>::err(
            errno_to_vfs_error_for(open_errno, host_path));
    }
    
    // Check if it's a directory
    struct stat st;
    if (::fstat(fd, &st) < 0) {
        int err = errno;
        ::close(fd);
        share_access_release(token);
        return Result<FileHandle, VfsError>::err(errno_to_vfs_error(err));
    }
    if (share_access_bits != 0 &&
        (token == 0 || token_dev != static_cast<u64>(st.st_dev) ||
         token_ino != static_cast<u64>(st.st_ino))) {
        // Newly created (or replaced between the stat and the open): record
        // against the object actually opened.
        share_access_release(token);
        token = 0;
        auto t = share_access_acquire(static_cast<u64>(st.st_dev),
                                      static_cast<u64>(st.st_ino),
                                      share_access_bits, share_allow);
        if (!t) {
            ::close(fd);
            return Result<FileHandle, VfsError>::err(VfsError::SharingViolation);
        }
        token = *t;
    }
    
    FileHandle handle = allocate_handle();
    
    auto state = std::make_shared<OpenFileState>();
    state->fd = fd;
    state->path = std::string(path);
    state->access = access;
    state->share = share;
    state->is_directory = S_ISDIR(st.st_mode);
    state->dev = static_cast<u64>(st.st_dev);
    state->ino = static_cast<u64>(st.st_ino);
    state->share_token = token;
    if (!state->is_directory) hold_no_delete_lock(fd, share);

    open_files_[handle] = std::move(state);
    
    return Result<FileHandle, VfsError>::ok(handle);
}

Result<void, VfsError> HostFileSystem::close(FileHandle handle) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = open_files_.find(handle);
        if (it == open_files_.end()) {
            return Result<void, VfsError>::err(VfsError::InvalidHandle);
        }
        state = std::move(it->second);
        open_files_.erase(it);
    }

    if (!state) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd >= 0) {
        ::close(state->fd);
        state->fd = -1;
    }
    share_access_release(state->share_token);
    state->share_token = 0;
    range_unlock_all(reinterpret_cast<u64>(state.get()));

    return Result<void, VfsError>::ok();
}

Result<int, VfsError> HostFileSystem::duplicate_native_fd(FileHandle handle) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<int, VfsError>::err(VfsError::InvalidHandle);
    }
    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) {
        return Result<int, VfsError>::err(VfsError::InvalidHandle);
    }
    // The descriptor's own offset is stale while seek() is in-memory only, and
    // a dup() shares that offset with the caller, so materialize it first.
    if (!state->native_position &&
        ::lseek(state->fd, static_cast<off_t>(state->position), SEEK_SET) < 0) {
        return Result<int, VfsError>::err(errno_to_vfs_error(errno));
    }
    const int duped = ::dup(state->fd);
    if (duped < 0) {
        return Result<int, VfsError>::err(errno_to_vfs_error(errno));
    }
    state->native_position = true;
    return Result<int, VfsError>::ok(duped);
}

Result<FileHandle, VfsError> HostFileSystem::adopt_native_fd(
    int fd,
    AccessMode access,
    ShareMode share,
    std::string_view path) {
    if (fd < 0) {
        return Result<FileHandle, VfsError>::err(VfsError::InvalidArgument);
    }
    struct stat st{};
    if (::fstat(fd, &st) < 0) {
        return Result<FileHandle, VfsError>::err(errno_to_vfs_error(errno));
    }

    FileHandle handle = allocate_handle();
    auto state = std::make_shared<OpenFileState>();
    state->fd = fd;
    state->path = std::string(path);
    state->access = access;
    state->share = share;
    state->is_directory = S_ISDIR(st.st_mode);
    if (!state->is_directory && S_ISREG(st.st_mode)) {
        hold_no_delete_lock(fd, share);
    }
    // An adopted descriptor may already be positioned (an inherited pipe or a
    // partially consumed file), so seed the tracked pointer from it rather
    // than assuming 0. Not seekable (a pipe) simply means position 0.
    const off_t adopted_pos = ::lseek(fd, 0, SEEK_CUR);
    state->position = adopted_pos > 0 ? static_cast<u64>(adopted_pos) : 0;
    state->native_position = true;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        open_files_[handle] = std::move(state);
    }
    return Result<FileHandle, VfsError>::ok(handle);
}

Result<usize, VfsError> HostFileSystem::read(FileHandle handle, std::span<u8> buffer) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<usize, VfsError>::err(VfsError::InvalidHandle);
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) {
        return Result<usize, VfsError>::err(VfsError::InvalidHandle);
    }
    // FILE_EXECUTE-only opens (SEC_IMAGE) still need to read PE bytes when
    // materializing a section. Treat Execute as sufficient for read, matching
    // Windows image-section semantics.
    if (!has_access(state->access, AccessMode::Read) &&
        !has_access(state->access, AccessMode::Execute)) {
        return Result<usize, VfsError>::err(VfsError::AccessDenied);
    }
    if (state->native_position) {
        const off_t position = ::lseek(state->fd, 0, SEEK_CUR);
        if (position >= 0) state->position = static_cast<u64>(position);
    }

    // Learn (ReadFile): a read overlapping a range another handle locked
    // fails ERROR_LOCK_VIOLATION without moving the pointer
    // (files_core_probe ReadFile_h2_locked_region).
    if (!range_io_allowed(state->dev, state->ino,
                          reinterpret_cast<u64>(state.get()), state->position,
                          buffer.size(), /*write=*/false)) {
        return Result<usize, VfsError>::err(VfsError::LockViolation);
    }

    const bool stats = io_stats_enabled();
    auto start = std::chrono::steady_clock::time_point{};
    if (stats) {
        start = std::chrono::steady_clock::now();
    }
    ssize_t bytes_read = state->native_position
        ? ::read(state->fd, buffer.data(), buffer.size())
        : ::pread(state->fd, buffer.data(), buffer.size(),
                  static_cast<off_t>(state->position));
    if (stats) {
        g_io_stats.read_calls.fetch_add(1, std::memory_order_relaxed);
        g_io_stats.read_ns.fetch_add(
            elapsed_ns(start, std::chrono::steady_clock::now()),
            std::memory_order_relaxed);
        if (bytes_read > 0) {
            g_io_stats.read_bytes.fetch_add(static_cast<u64>(bytes_read),
                                            std::memory_order_relaxed);
        }
    }
    if (bytes_read < 0) {
        return Result<usize, VfsError>::err(errno_to_vfs_error(errno));
    }

    if (state->hold_atime) {
        const struct timespec t[2] = {state->held_atime, {0, UTIME_OMIT}};
        (void)::futimens(state->fd, t);
    }
    // pread() does not move the descriptor, so the file object's pointer is
    // advanced here to keep the Windows read-advances-the-pointer contract.
    state->position += static_cast<u64>(bytes_read);
    return Result<usize, VfsError>::ok(static_cast<usize>(bytes_read));
}

Result<usize, VfsError> HostFileSystem::write(FileHandle handle, std::span<const u8> data) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<usize, VfsError>::err(VfsError::InvalidHandle);
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) {
        return Result<usize, VfsError>::err(VfsError::InvalidHandle);
    }
    if (!has_access(state->access, AccessMode::Write)) {
        return Result<usize, VfsError>::err(VfsError::AccessDenied);
    }
    if (state->native_position) {
        const off_t position = ::lseek(state->fd, 0, SEEK_CUR);
        if (position >= 0) state->position = static_cast<u64>(position);
    }

    // Learn (WriteFile): fails when the range overlaps a region locked by
    // another handle, or any shared lock (files_core_probe
    // WriteFile_h2_locked_region).
    if (!range_io_allowed(state->dev, state->ino,
                          reinterpret_cast<u64>(state.get()), state->position,
                          data.size(), /*write=*/true)) {
        return Result<usize, VfsError>::err(VfsError::LockViolation);
    }

    const bool stats = io_stats_enabled();
    auto start = std::chrono::steady_clock::time_point{};
    if (stats) {
        start = std::chrono::steady_clock::now();
    }

    // Loop to handle short writes (POSIX write() may not write the full
    // buffer in a single call, especially for large buffers).
    const u8* ptr = data.data();
    usize remaining = data.size();
    usize total_written = 0;
    while (remaining > 0) {
        ssize_t bytes_written =
            state->native_position ? ::write(state->fd, ptr, remaining)
                : ::pwrite(state->fd, ptr, remaining,
                           static_cast<off_t>(state->position + total_written));
        if (bytes_written < 0) {
            if (errno == EINTR) continue;  // Interrupted — retry
            // A disk-file write that cannot complete fails (ENOSPC ->
            // ERROR_DISK_FULL); it is not reported as a short success.
            return Result<usize, VfsError>::err(errno_to_vfs_error(errno));
        }
        total_written += static_cast<usize>(bytes_written);
        ptr += bytes_written;
        remaining -= static_cast<usize>(bytes_written);
    }
    
    if (stats) {
        g_io_stats.write_calls.fetch_add(1, std::memory_order_relaxed);
        g_io_stats.write_ns.fetch_add(
            elapsed_ns(start, std::chrono::steady_clock::now()),
            std::memory_order_relaxed);
        if (total_written > 0) {
            g_io_stats.write_bytes.fetch_add(static_cast<u64>(total_written),
                                             std::memory_order_relaxed);
            maybe_print_live_io_stats();
        }
    }

    if (state->hold_mtime || state->hold_atime) {
        struct timespec t[2] = {{0, UTIME_OMIT}, {0, UTIME_OMIT}};
        if (state->hold_atime) t[0] = state->held_atime;
        if (state->hold_mtime) t[1] = state->held_mtime;
        (void)::futimens(state->fd, t);
    }
    // As in read(): pwrite() leaves the descriptor where it was.
    state->position += static_cast<u64>(total_written);
    return Result<usize, VfsError>::ok(total_written);
}

Result<u64, VfsError> HostFileSystem::seek(FileHandle handle, i64 offset, SeekOrigin origin) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<u64, VfsError>::err(VfsError::InvalidHandle);
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) {
        return Result<u64, VfsError>::err(VfsError::InvalidHandle);
    }
    if (state->native_position) {
        int whence;
        switch (origin) {
            case SeekOrigin::Begin: whence = SEEK_SET; break;
            case SeekOrigin::Current: whence = SEEK_CUR; break;
            case SeekOrigin::End: whence = SEEK_END; break;
            default: return Result<u64, VfsError>::err(VfsError::InvalidArgument);
        }
        const off_t target = ::lseek(state->fd, static_cast<off_t>(offset), whence);
        if (target < 0) return Result<u64, VfsError>::err(errno_to_vfs_error(errno));
        state->position = static_cast<u64>(target);
        return Result<u64, VfsError>::ok(state->position);
    }
    
    // Windows resolves the file pointer entirely inside the file object, so
    // only SeekOrigin::End needs to ask the OS anything (for the size).
    i64 base = 0;
    switch (origin) {
        case SeekOrigin::Begin:
            base = 0;
            break;
        case SeekOrigin::Current:
            base = static_cast<i64>(state->position);
            break;
        case SeekOrigin::End: {
            struct stat st{};
            if (::fstat(state->fd, &st) < 0) {
                return Result<u64, VfsError>::err(errno_to_vfs_error(errno));
            }
            base = static_cast<i64>(st.st_size);
            break;
        }
        default:
            return Result<u64, VfsError>::err(VfsError::InvalidArgument);
    }

    const i64 target = base + offset;
    if (target < 0) {
        // Windows reports a seek to a negative offset as ERROR_NEGATIVE_SEEK;
        // callers map InvalidArgument onto that.
        return Result<u64, VfsError>::err(VfsError::InvalidArgument);
    }

    // Seeking past EOF is legal on Windows and does not extend the file, so
    // there is deliberately no size clamp here.
    state->position = static_cast<u64>(target);
    return Result<u64, VfsError>::ok(state->position);
}

Result<void, VfsError> HostFileSystem::truncate(FileHandle handle, u64 new_size) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }
    
    if (::ftruncate(state->fd, static_cast<off_t>(new_size)) < 0) {
        return Result<void, VfsError>::err(errno_to_vfs_error(errno));
    }
    
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::flush(FileHandle handle) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }

    // Win32 installers call FlushFileBuffers after every file copy. A host
    // fsync per call dominates capsule install time on APFS. The default
    // acknowledges success without a durable sync. Opt in with
    // VITREO_FSYNC=1 when durability matters.
    static const bool do_fsync = std::getenv("VITREO_FSYNC") != nullptr;
    if (!do_fsync) {
        return Result<void, VfsError>::ok();
    }

    const bool stats = io_stats_enabled();
    auto start = std::chrono::steady_clock::time_point{};
    if (stats) {
        start = std::chrono::steady_clock::now();
    }

    if (::fsync(state->fd) < 0) {
        if (stats) {
            g_io_stats.flush_calls.fetch_add(1, std::memory_order_relaxed);
            g_io_stats.flush_ns.fetch_add(
                elapsed_ns(start, std::chrono::steady_clock::now()),
                std::memory_order_relaxed);
        }
        return Result<void, VfsError>::err(errno_to_vfs_error(errno));
    }

    if (stats) {
        g_io_stats.flush_calls.fetch_add(1, std::memory_order_relaxed);
        g_io_stats.flush_ns.fetch_add(
            elapsed_ns(start, std::chrono::steady_clock::now()),
            std::memory_order_relaxed);
    }
    
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::set_file_times(
    FileHandle handle,
    i64 atime_unix_seconds,
    i64 mtime_unix_seconds
) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    if (state->fd < 0) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }

    struct timespec times[2];
    if (atime_unix_seconds < 0) {
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
    } else {
        times[0].tv_sec = static_cast<time_t>(atime_unix_seconds);
        times[0].tv_nsec = 0;
    }
    if (mtime_unix_seconds < 0) {
        times[1].tv_sec = 0;
        times[1].tv_nsec = UTIME_OMIT;
    } else {
        times[1].tv_sec = static_cast<time_t>(mtime_unix_seconds);
        times[1].tv_nsec = 0;
    }

    if (::futimens(state->fd, times) < 0) {
        return Result<void, VfsError>::err(errno_to_vfs_error(errno));
    }
    return Result<void, VfsError>::ok();
}

bool HostFileSystem::volume_info(VolumeInfo& info) const noexcept {
    info = VolumeInfo{};
    info.filesystem_name = "NTFS";
    info.max_component_length = 255;
    info.filesystem_flags =
        0x1u |  // FILE_CASE_SENSITIVE_SEARCH
        0x2u |  // FILE_CASE_PRESERVED_NAMES
        0x4u |  // FILE_UNICODE_ON_DISK
        0x8u;   // FILE_PERSISTENT_ACLS

    // Space is the host volume's, purgeable space included (see
    // platform/host_volume_space.hpp). A volume that cannot be queried
    // leaves the geometry zero, which tells GetDiskFreeSpace to use host
    // totals.
    platform::HostVolumeSpace space;
    if (!platform::host_volume_space(root_.c_str(), space)) {
        return true;
    }
    const u64 unit = space.allocation_unit;
    if (unit >= 512 && unit % 512 == 0 && unit / 512 <= 0xFFFFFFFFull) {
        info.bytes_per_sector = 512;
        info.sectors_per_cluster = static_cast<u32>(unit / 512);
    } else {
        info.bytes_per_sector = static_cast<u32>(std::min<u64>(unit, 0xFFFFFFFFull));
        info.sectors_per_cluster = 1;
    }
    const auto clamp32 = [](u64 v) {
        return static_cast<u32>(std::min<u64>(v, 0xFFFFFFFFull));
    };
    info.free_clusters = clamp32(space.available_bytes / unit);
    info.total_clusters = clamp32(space.total_bytes / unit);
    info.total_bytes = space.total_bytes;
    info.free_bytes = space.free_bytes;
    info.available_bytes = space.available_bytes;
    return true;
}

Result<FileInfo, VfsError> HostFileSystem::stat(std::string_view path) {
    std::string host_path = to_host_path(path);
    
    struct stat st;
    if (::stat(host_path.c_str(), &st) < 0) {
        return Result<FileInfo, VfsError>::err(
            errno_to_vfs_error_for(errno, host_path));
    }

    return Result<FileInfo, VfsError>::ok(
        stat_to_file_info(st, read_dos_attrs(host_path, -1)));
}

Result<FileInfo, VfsError> HostFileSystem::lstat(std::string_view path) {
    std::string host_path = to_host_path(path);
    struct stat st;
    if (::lstat(host_path.c_str(), &st) < 0) {
        return Result<FileInfo, VfsError>::err(
            errno_to_vfs_error_for(errno, host_path));
    }
    FileInfo info = stat_to_file_info(st, read_dos_attrs(host_path, -1));
    if (S_ISLNK(st.st_mode)) {
        // Learn (FindFirstFileW): find data describes the link, not the target.
        // Directory symlinks carry DIRECTORY|REPARSE_POINT; file symlinks
        // ARCHIVE|REPARSE_POINT; size is 0 (files_meta / symlink_find probe).
        info.attributes = info.attributes | FileAttributes::ReparsePoint;
        info.size = 0;
        struct stat target{};
        if (::stat(host_path.c_str(), &target) == 0 && S_ISDIR(target.st_mode)) {
            info.attributes =
                info.attributes | FileAttributes::Directory;
            info.attributes = static_cast<FileAttributes>(
                static_cast<u32>(info.attributes) &
                ~static_cast<u32>(FileAttributes::Archive |
                                  FileAttributes::Normal));
        } else {
            info.attributes = static_cast<FileAttributes>(
                (static_cast<u32>(info.attributes) &
                 ~static_cast<u32>(FileAttributes::Directory)) |
                static_cast<u32>(FileAttributes::Archive));
            info.attributes = static_cast<FileAttributes>(
                static_cast<u32>(info.attributes) &
                ~static_cast<u32>(FileAttributes::Normal));
        }
    }
    return Result<FileInfo, VfsError>::ok(info);
}

Result<FileInfo, VfsError> HostFileSystem::stat_handle(FileHandle handle) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = open_files_.find(handle);
        if (it == open_files_.end()) {
            return Result<FileInfo, VfsError>::err(VfsError::InvalidHandle);
        }
        state = it->second;
    }

    std::lock_guard<std::mutex> state_lock(state->mutex);
    struct stat st;
    if (::fstat(state->fd, &st) < 0) {
        return Result<FileInfo, VfsError>::err(errno_to_vfs_error(errno));
    }
    return Result<FileInfo, VfsError>::ok(
        stat_to_file_info(st, read_dos_attrs(std::string(), state->fd)));
}

Result<std::vector<DirectoryEntry>, VfsError> HostFileSystem::list_dir(std::string_view path) {
    std::string host_path = to_host_path(path);
    
    DIR* dir = ::opendir(host_path.c_str());
    if (!dir) {
        // For enumeration the leaf *is* the directory, so its absence is a
        // missing path, not a missing file — matching FindFirstFile, which
        // reports ERROR_PATH_NOT_FOUND for a directory that does not exist.
        return Result<std::vector<DirectoryEntry>, VfsError>::err(
            errno_to_vfs_error(errno)
        );
    }
    
    std::vector<DirectoryEntry> entries;
    
    struct dirent* entry;
    while ((entry = ::readdir(dir)) != nullptr) {
        // Skip . and ..
        if (std::strcmp(entry->d_name, ".") == 0 || 
            std::strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        DirectoryEntry dir_entry;
        dir_entry.name = entry->d_name;

        // FindFirstFileW describes the symlink itself (REPARSE_POINT), not the
        // target (Windows symlink_find: attrs 0x410 / 0x420, size 0).
        std::string guest_entry(path);
        if (!guest_entry.empty() && guest_entry.back() != '\\' &&
            guest_entry.back() != '/') {
            // Host-relative paths use '/'. A backslash is a literal name
            // character here, so joining with one makes every child lstat
            // miss and FindFirstFileExW reports an empty directory.
            guest_entry.push_back('/');
        }
        guest_entry += entry->d_name;
        auto info_res = lstat(guest_entry);
        if (info_res.is_err()) {
            continue;
        }
        dir_entry.info = info_res.value();
        entries.push_back(std::move(dir_entry));
    }
    
    ::closedir(dir);
    
    return Result<std::vector<DirectoryEntry>, VfsError>::ok(std::move(entries));
}

Result<std::vector<DirectoryEntry>, VfsError>
HostFileSystem::list_dir_handle(FileHandle handle) {
    auto state = get_open_file(handle);
    if (!state) {
        return Result<std::vector<DirectoryEntry>, VfsError>::err(
            VfsError::InvalidHandle);
    }

    std::string path;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->is_directory) {
            return Result<std::vector<DirectoryEntry>, VfsError>::err(
                VfsError::NotADirectory);
        }
        path = state->path;
    }
    return list_dir(path);
}

Result<void, VfsError> HostFileSystem::create_dir(std::string_view path) {
    std::string host_path = to_host_path(path);

    // Learn (CreateDirectoryW): "this function will only create the final
    // directory in the path"; a missing intermediate directory fails
    // ERROR_PATH_NOT_FOUND (files_ns_probe: CreateDirectoryW_missing_parent).
    if (::mkdir(host_path.c_str(), 0755) < 0) {
        return Result<void, VfsError>::err(
            errno_to_vfs_error_for(errno, host_path));
    }

    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::remove(std::string_view path) {
    std::string host_path = to_host_path(path);
    static const bool diag = std::getenv("VITREO_FILE_DIAG") != nullptr;

    // Check if it's a directory or file (lstat: unlink a symlink itself).
    struct stat st;
    if (::lstat(host_path.c_str(), &st) < 0) {
        // Capture errno immediately: errno_to_vfs_error_for() calls into
        // std::filesystem, which may clobber it.
        const int stat_errno = errno;
        const VfsError mapped = errno_to_vfs_error_for(stat_errno, host_path);
        if (diag) {
            std::error_code ec;
            const auto parent = std::filesystem::path(host_path).parent_path();
            const bool parent_is_dir =
                !parent.empty() && std::filesystem::is_directory(parent, ec);
            std::fprintf(stderr,
                "[vfsdiag] remove guest='%.*s' host='%s' stat=fail errno=%d(%s) "
                "parent='%s' parent_is_dir=%d parent_ec=%d vfs_err=%s\n",
                static_cast<int>(path.size()), path.data(), host_path.c_str(),
                stat_errno, std::strerror(stat_errno),
                parent.c_str(), parent_is_dir ? 1 : 0, ec.value(),
                vfs_error_to_string(mapped));
        }
        return Result<void, VfsError>::err(mapped);
    }

    // Learn (DeleteFile): "If the file is a read-only file, the function
    // fails with ERROR_ACCESS_DENIED"; a READONLY directory likewise refuses
    // RemoveDirectory (files_ns_probe DeleteFileW_readonly,
    // RemoveDirectoryW_readonly_dir). POSIX would unlink both.
    if (host_is_readonly(host_path, st)) {
        return Result<void, VfsError>::err(VfsError::AccessDenied);
    }

    // DeleteFile/RemoveDirectory open the object for DELETE with full
    // sharing; any open that denies delete sharing refuses it
    // (files_core_probe writer_then_DeleteFileW, files_ns_probe
    // RemoveDirectoryW_current_dir).
    if (!share_access_would_admit(static_cast<u64>(st.st_dev),
                                  static_cast<u64>(st.st_ino),
                                  share_bits::Delete, share_bits::All)) {
        return Result<void, VfsError>::err(VfsError::SharingViolation);
    }

    int result;
    if (S_ISDIR(st.st_mode)) {
        result = ::rmdir(host_path.c_str());
    } else {
        // Learn (DeleteFile): fails while other handles have the file open
        // for normal I/O or as a memory-mapped file without
        // FILE_SHARE_DELETE. Unlinking anyway would let a second guest
        // process replace a file the first one is still mapping.
        if (other_handles_deny_delete(host_path)) {
            if (diag) {
                std::fprintf(stderr,
                    "[vfsdiag] remove guest='%.*s' host='%s' -> sharing "
                    "violation (open elsewhere without delete sharing)\n",
                    static_cast<int>(path.size()), path.data(),
                    host_path.c_str());
            }
            return Result<void, VfsError>::err(VfsError::SharingViolation);
        }
        result = ::unlink(host_path.c_str());
    }

    if (result < 0) {
        const int op_errno = errno;
        const VfsError mapped = errno_to_vfs_error_for(op_errno, host_path);
        if (diag) {
            std::fprintf(stderr,
                "[vfsdiag] remove guest='%.*s' host='%s' op=%s errno=%d(%s) "
                "vfs_err=%s\n",
                static_cast<int>(path.size()), path.data(), host_path.c_str(),
                S_ISDIR(st.st_mode) ? "rmdir" : "unlink",
                op_errno, std::strerror(op_errno), vfs_error_to_string(mapped));
        }
        return Result<void, VfsError>::err(mapped);
    }

    if (diag) {
        std::fprintf(stderr, "[vfsdiag] remove guest='%.*s' host='%s' -> ok\n",
                     static_cast<int>(path.size()), path.data(), host_path.c_str());
    }
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::rename(
    std::string_view old_path,
    std::string_view new_path
) {
    std::string old_host_path = to_host_path(old_path);
    std::string new_host_path = to_host_path(new_path);

    // MoveFile opens the source (and a replaced destination) for DELETE with
    // full sharing (files_core_probe writer_then_MoveFileW,
    // files_ns_probe MoveFileW_open_without_share_delete).
    for (const std::string* p : {&old_host_path, &new_host_path}) {
        struct stat st{};
        if (::lstat(p->c_str(), &st) == 0 &&
            !share_access_would_admit(static_cast<u64>(st.st_dev),
                                      static_cast<u64>(st.st_ino),
                                      share_bits::Delete, share_bits::All)) {
            return Result<void, VfsError>::err(VfsError::SharingViolation);
        }
    }

    // Learn (MoveFileW): "The new name must not already exist." POSIX
    // rename(2) would replace a file destination; refuse that with
    // PathAlreadyExists so MoveFileW fails ERROR_ALREADY_EXISTS. A
    // case-only rename on a case-insensitive volume (same inode) is still
    // allowed (files_ns_probe MoveFileW_case_only).
    struct stat old_st{};
    struct stat new_st{};
    const bool old_ok = ::lstat(old_host_path.c_str(), &old_st) == 0;
    const bool new_ok = ::lstat(new_host_path.c_str(), &new_st) == 0;
    if (new_ok) {
        if (!old_ok || old_st.st_dev != new_st.st_dev ||
            old_st.st_ino != new_st.st_ino) {
            return Result<void, VfsError>::err(VfsError::PathAlreadyExists);
        }
    }

    if (::rename(old_host_path.c_str(), new_host_path.c_str()) < 0) {
        const int rename_errno = errno;
        if (rename_errno == ENOENT) {
            // Classify against the side that is actually missing so
            // MoveFileW_missing_source_dir -> PATH_NOT_FOUND and
            // MoveFileW_missing_dest_dir -> PATH_NOT_FOUND.
            struct stat probe{};
            const std::string& classify =
                (::lstat(old_host_path.c_str(), &probe) != 0)
                    ? old_host_path
                    : new_host_path;
            return Result<void, VfsError>::err(
                errno_to_vfs_error_for(rename_errno, classify));
        }
        return Result<void, VfsError>::err(
            errno_to_vfs_error_for(rename_errno, old_host_path));
    }
    
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::rename_open(
    FileHandle handle,
    std::string_view new_relative,
    bool replace_existing
) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) {
        return Result<void, VfsError>::err(VfsError::InvalidHandle);
    }

    std::string old_rel;
    ShareToken token = 0;
    u32 access_bits = 0;
    u32 share_allow = 0;
    {
        std::lock_guard<std::mutex> sl(state->mutex);
        old_rel = state->path;
        token = state->share_token;
        access_bits =
            ((has_access(state->access, AccessMode::Read) ||
              has_access(state->access, AccessMode::Execute))
                 ? share_bits::Read : 0u) |
            (has_access(state->access, AccessMode::Write) ? share_bits::Write : 0u) |
            (has_access(state->access, AccessMode::Delete) ? share_bits::Delete : 0u);
        share_allow = static_cast<u32>(state->share) & share_bits::All;
        state->share_token = 0;
    }
    if (old_rel == new_relative) {
        std::lock_guard<std::mutex> sl(state->mutex);
        state->share_token = token;
        return Result<void, VfsError>::ok();
    }

    // This handle is the one performing the rename, so its own share mode
    // must not collide with the DELETE check inside rename/unlink.
    share_access_release(token);
    token = 0;

    auto restore = [&](const std::string& rel) {
        if (access_bits == 0) return;
        struct stat st{};
        if (::lstat(to_host_path(rel).c_str(), &st) != 0) return;
        auto t = share_access_acquire(static_cast<u64>(st.st_dev),
                                      static_cast<u64>(st.st_ino),
                                      access_bits, share_allow);
        std::lock_guard<std::mutex> sl(state->mutex);
        if (t) {
            state->share_token = *t;
            state->dev = static_cast<u64>(st.st_dev);
            state->ino = static_cast<u64>(st.st_ino);
        }
    };

    const std::string old_host = to_host_path(old_rel);
    const std::string new_host = to_host_path(new_relative);
    struct stat new_st{};
    if (::lstat(new_host.c_str(), &new_st) == 0) {
        struct stat old_st{};
        const bool old_ok = ::lstat(old_host.c_str(), &old_st) == 0;
        const bool same = old_ok && old_st.st_dev == new_st.st_dev &&
                          old_st.st_ino == new_st.st_ino;
        if (!same) {
            if (!replace_existing || S_ISDIR(new_st.st_mode) ||
                host_is_readonly(new_host, new_st)) {
                restore(old_rel);
                return Result<void, VfsError>::err(
                    replace_existing ? VfsError::AccessDenied
                                     : VfsError::PathAlreadyExists);
            }
            if (!share_access_would_admit(static_cast<u64>(new_st.st_dev),
                                          static_cast<u64>(new_st.st_ino),
                                          share_bits::Delete, share_bits::All)) {
                restore(old_rel);
                return Result<void, VfsError>::err(VfsError::SharingViolation);
            }
            if (::unlink(new_host.c_str()) < 0) {
                restore(old_rel);
                return Result<void, VfsError>::err(
                    errno_to_vfs_error_for(errno, new_host));
            }
        }
    }

    if (::rename(old_host.c_str(), new_host.c_str()) < 0) {
        const int rename_errno = errno;
        restore(old_rel);
        return Result<void, VfsError>::err(
            errno_to_vfs_error_for(rename_errno, old_host));
    }

    {
        std::lock_guard<std::mutex> sl(state->mutex);
        state->path = std::string(new_relative);
    }
    restore(std::string(new_relative));
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> HostFileSystem::create_hard_link(
    std::string_view new_path,
    std::string_view existing_path
) {
    const std::string new_host_path = to_host_path(new_path);
    const std::string existing_host_path = to_host_path(existing_path);
    if (::link(existing_host_path.c_str(), new_host_path.c_str()) < 0) {
        const int link_errno = errno;
        if (link_errno == EXDEV) {
            return Result<void, VfsError>::err(VfsError::NotSameDevice);
        }
        return Result<void, VfsError>::err(
            errno_to_vfs_error_for(link_errno, existing_host_path));
    }
    return Result<void, VfsError>::ok();
}

Result<std::string, VfsError> HostFileSystem::resolve_host_path(
    std::string_view path
) const {
    return Result<std::string, VfsError>::ok(to_host_path(path));
}

Result<std::string, VfsError> HostFileSystem::final_path(FileHandle handle) {
    std::shared_ptr<OpenFileState> state;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state = get_open_file(handle);
    }
    if (!state) return Result<std::string, VfsError>::err(VfsError::InvalidHandle);

    // F_GETPATH names the vnode the descriptor holds, so it already has the
    // properties the final path needs: the stored spelling of every component
    // (a case-insensitive open of "mixedcase.TXT" reports "MixedCase.txt"),
    // no dot segments, links resolved, and the current name after a rename.
    char host[MAXPATHLEN] = {};
    {
        std::lock_guard<std::mutex> state_lock(state->mutex);
        if (state->fd < 0) {
            return Result<std::string, VfsError>::err(VfsError::InvalidHandle);
        }
        if (::fcntl(state->fd, F_GETPATH, host) != 0) {
            return Result<std::string, VfsError>::err(errno_to_vfs_error(errno));
        }
    }

    // The root is compared in the same resolved form F_GETPATH returns
    // ("/private/tmp", not "/tmp").
    char root_buf[MAXPATHLEN] = {};
    const std::string root =
        ::realpath(root_.c_str(), root_buf) != nullptr ? std::string(root_buf) : root_;
    const std::string_view full(host);
    if (full == root) return Result<std::string, VfsError>::ok(std::string{});
    if (full.size() <= root.size() + 1 ||
        full.compare(0, root.size(), root) != 0 || full[root.size()] != '/') {
        return Result<std::string, VfsError>::err(VfsError::PathNotFound);
    }
    return Result<std::string, VfsError>::ok(std::string(full.substr(root.size() + 1)));
}

} // namespace vitreo::capsule

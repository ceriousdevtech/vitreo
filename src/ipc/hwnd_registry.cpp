// SPDX-License-Identifier: LGPL-2.1-only
/// @file hwnd_registry.cpp

#include "ipc/hwnd_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include <csignal>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vitreo::ipc {
namespace {

namespace fs = std::filesystem;

constexpr u32 kMagic = 0x56544857; // "VTHW"
constexpr u32 kVersion = 3;

struct HwndRecord {
    u32 magic;
    u32 version;
    u32 hwnd;
    u32 owner_host_pid;
    u32 owner_guest_thread_id;
    i32 client_w;
    i32 client_h;
    u32 reserved;
    // Class and title, UTF-8, NUL-terminated and NUL-padded.
    //
    // FindWindow is SYSTEM-WIDE on Windows (Microsoft Learn): it searches
    // every top-level window on the desktop, not the caller's process. Vitreo
    // runs each guest process as its own host process, so a class/title query
    // that only consulted the local HwndManager could never see a peer's
    // window -- and a program waiting for another process's window POLLS,
    // which turns a miss into a spin rather than an error.
    //
    // Fixed-size so the record stays a POD written with one write() and read
    // with one read(); a truncated name simply fails to match, which is the
    // safe direction.
    char class_name[64];
    char title[128];
};

std::string capsule_root() {
    const char* scope = ::getenv("VITREO_IPC_SCOPE");
    if (scope == nullptr || scope[0] == '\0') return {};
    return scope;
}

std::string registry_directory() {
    const std::string root = capsule_root();
    if (root.empty()) return {};
    return root + "/runtime/hwnds";
}

std::string record_path(u32 hwnd) {
    const std::string dir = registry_directory();
    if (dir.empty() || hwnd == 0) return {};
    return dir + "/" + std::to_string(hwnd);
}

std::string parent_path(u32 hwnd) {
    const std::string path = record_path(hwnd);
    return path.empty() ? std::string{} : path + ".parent";
}

// Visibility lives beside the record rather than inside it, exactly as the
// parent HWND does. The record is rewritten wholesale on every move/resize;
// a separate file cannot be clobbered by a publish that only knows geometry.
std::string visible_path(u32 hwnd) {
    const std::string path = record_path(hwnd);
    return path.empty() ? std::string{} : path + ".visible";
}

// Absent file means "no publisher has said otherwise" -> visible, which keeps
// every window that predates this channel behaving as it did.
bool read_visible(u32 hwnd) {
    const std::string path = visible_path(hwnd);
    if (path.empty()) return true;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return true;
    u32 value = 1;
    const bool read_ok =
        ::pread(fd, &value, sizeof(value), 0) ==
        static_cast<ssize_t>(sizeof(value));
    ::close(fd);
    return read_ok ? value != 0 : true;
}

std::string present_host_path(u32 hwnd) {
    const std::string path = record_path(hwnd);
    return path.empty() ? std::string{} : path + ".presenthost";
}

// Absent file means nobody hosts a peer view for this HWND.
u32 read_present_host(u32 hwnd) {
    const std::string path = present_host_path(hwnd);
    if (path.empty()) return 0;
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    u32 value = 0;
    const bool read_ok =
        ::pread(fd, &value, sizeof(value), 0) ==
        static_cast<ssize_t>(sizeof(value));
    ::close(fd);
    return read_ok ? value : 0;
}

std::string shell_properties_path(u32 hwnd) {
    const std::string path = record_path(hwnd);
    return path.empty() ? std::string{} : path + ".shellprops";
}

// Without an IPC scope (isolated tests, a lone process) the values live here.
std::mutex& local_shell_properties_lock() {
    static std::mutex lock;
    return lock;
}

std::unordered_map<u32, std::vector<WindowShellProperty>>& local_shell_properties() {
    static std::unordered_map<u32, std::vector<WindowShellProperty>> values;
    return values;
}

// Sidecar layout: u32 count, then per value 16 bytes FMTID, u32 PID,
// u32 UTF-16 code units and the code units.
bool parse_shell_properties(const std::string& bytes,
                            std::vector<WindowShellProperty>& out) {
    out.clear();
    if (bytes.empty()) return true;
    usize at = 0;
    const auto take = [&](void* dst, usize n) {
        if (bytes.size() - at < n) return false;
        std::memcpy(dst, bytes.data() + at, n);
        at += n;
        return true;
    };
    u32 count = 0;
    if (!take(&count, sizeof(count))) return false;
    for (u32 i = 0; i < count; ++i) {
        WindowShellProperty property;
        u32 units = 0;
        if (!take(property.fmtid.data(), property.fmtid.size()) ||
            !take(&property.pid, sizeof(property.pid)) ||
            !take(&units, sizeof(units)) ||
            bytes.size() - at < static_cast<usize>(units) * 2) {
            return false;
        }
        property.text.resize(units);
        (void)take(property.text.data(), static_cast<usize>(units) * 2);
        out.push_back(std::move(property));
    }
    return at == bytes.size();
}

std::string serialize_shell_properties(const std::vector<WindowShellProperty>& values) {
    std::string bytes;
    const auto put = [&](const void* src, usize n) {
        bytes.append(static_cast<const char*>(src), n);
    };
    const u32 count = static_cast<u32>(values.size());
    put(&count, sizeof(count));
    for (const auto& property : values) {
        const u32 units = static_cast<u32>(property.text.size());
        put(property.fmtid.data(), property.fmtid.size());
        put(&property.pid, sizeof(property.pid));
        put(&units, sizeof(units));
        put(property.text.data(), static_cast<usize>(units) * 2);
    }
    return bytes;
}

bool read_fd_fully(int fd, std::string& bytes) {
    bytes.clear();
    char buffer[4096];
    off_t offset = 0;
    for (;;) {
        const ssize_t n = ::pread(fd, buffer, sizeof(buffer), offset);
        if (n < 0) return false;
        if (n == 0) return true;
        bytes.append(buffer, static_cast<usize>(n));
        offset += n;
    }
}

u32 read_parent(u32 hwnd) {
    u32 parent_hwnd = 0;
    const std::string parent = parent_path(hwnd);
    if (parent.empty()) return 0;
    const int parent_fd = ::open(parent.c_str(), O_RDONLY);
    if (parent_fd >= 0) {
        if (::pread(parent_fd, &parent_hwnd, sizeof(parent_hwnd), 0) !=
            static_cast<ssize_t>(sizeof(parent_hwnd))) {
            parent_hwnd = 0;
        }
        ::close(parent_fd);
    }
    return parent_hwnd;
}

std::string allocator_path() {
    const std::string dir = registry_directory();
    if (dir.empty()) return {};
    return dir + "/next-hwnd";
}

bool process_is_live(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) == 0) return true;
    return errno == EPERM;
}

// -----------------------------------------------------------------------------
// Change detection.
//
// Every query used to touch the filesystem: lookup opened and read the record
// plus three sidecars, and FindWindow listed the whole directory and stat'ed
// every entry (13,835 of them in a launcher capsule). Measured on a launcher's
// message loop: 161 stat + 40 getdirentries samples in 5 s, ~4% of a core,
// for answers that had not changed.
//
// A reader must still observe every change made by any process: windows
// created, destroyed, retitled, reparented, shown or hidden by a peer. Two
// signals, both read in one place (observe_locked):
//
//  * The directory's mtime/ctime. A record is published by writing `.tmp`
//    and renaming it over the name, and removed by unlink; both change the
//    directory timestamps, and either way the name maps to a NEW inode, so a
//    record is "unchanged" exactly when its inode, mtime and size are.
//  * The `generation` file, a counter every writer in this file increments
//    AFTER its data write. It exists because the sidecars (.parent, .visible,
//    .presenthost) are rewritten IN PLACE with the same 4-byte size: neither
//    the directory nor (inside one timestamp tick) the file's own mtime says
//    anything changed. It also closes the same-tick hole for renames.
//
// Either signal differing invalidates; nothing is trusted past that point
// without re-reading it. Steady state costs one stat and one 8-byte pread per
// query; a change costs one re-read of the affected record (lookup) or one
// readdir pass that opens only entries with unseen inodes (FindWindow).
// -----------------------------------------------------------------------------

std::string generation_path(const std::string& dir) {
    return dir + "/generation";
}

// Called by every writer after its data is on disk. Best-effort: a failure
// here degrades to the directory-timestamp signal alone.
void bump_generation() {
    const std::string dir = registry_directory();
    if (dir.empty()) return;
    const int fd = ::open(generation_path(dir).c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return;
    if (::flock(fd, LOCK_EX) == 0) {
        u64 value = 0;
        if (::pread(fd, &value, sizeof(value), 0) !=
            static_cast<ssize_t>(sizeof(value))) {
            value = 0;
        }
        ++value;
        (void)(::pwrite(fd, &value, sizeof(value), 0) ==
               static_cast<ssize_t>(sizeof(value)));
        (void)::flock(fd, LOCK_UN);
    }
    ::close(fd);
}

struct Stamp {
    // Generation mode: the counter file exists and is authoritative (every
    // writer in this file bumps it). Otherwise the registry was written only
    // by a build without the counter, and the directory timestamps are the
    // only signal (renames and unlinks; not in-place sidecar rewrites).
    bool by_generation = false;
    bool dir_exists = false;
    struct timespec mtime {};
    struct timespec ctime {};
    u64 generation = 0;
    bool operator==(const Stamp& o) const {
        return by_generation == o.by_generation &&
               dir_exists == o.dir_exists &&
               mtime.tv_sec == o.mtime.tv_sec &&
               mtime.tv_nsec == o.mtime.tv_nsec &&
               ctime.tv_sec == o.ctime.tv_sec &&
               ctime.tv_nsec == o.ctime.tv_nsec &&
               generation == o.generation;
    }
};

struct CachedWindow {
    // Record file identity the parse below came from.
    ino_t ino = 0;
    struct timespec mtime {};
    off_t size = 0;
    bool present = false;      // a valid v3 record exists for this HWND
    HwndRecord record{};
    u64 record_epoch = 0;      // cache epoch the record state is valid for
    // Sidecars, re-read whenever the epoch moves (they change in place).
    u32 parent_hwnd = 0;
    bool visible = true;
    u32 present_host_pid = 0;
    u64 sidecar_epoch = 0;
};

struct Cache {
    std::mutex lock;
    std::string dir;           // directory the cache describes
    // The generation file, held open so the steady-state check is two
    // fd-based syscalls (fstat + pread) with no path lookup. fstat's
    // st_nlink == 0 says the file was unlinked (registry wiped) and the fd
    // must be reopened.
    int gen_fd = -1;
    Stamp stamp;
    u64 epoch = 0;             // advances on every observed change
    u64 index_epoch = 0;       // epoch the full directory index is valid for
    std::unordered_map<u32, CachedWindow> windows;
    std::vector<u32> index;    // sorted HWNDs with a present record
};

Cache& cache() {
    static Cache* c = new Cache();  // immortal: queried during teardown
    return *c;
}

// Refresh the stamp; returns the current epoch. Caller holds cache().lock.
u64 observe_locked(Cache& c, const std::string& dir) {
    if (dir != c.dir) {
        c.dir = dir;
        c.stamp = Stamp{};
        c.windows.clear();
        c.index.clear();
        c.index_epoch = 0;
        if (c.gen_fd >= 0) ::close(c.gen_fd);
        c.gen_fd = -1;
        ++c.epoch;
    }
    Stamp now;
    struct stat st {};
    if (c.gen_fd < 0) {
        c.gen_fd = ::open(generation_path(dir).c_str(), O_RDONLY | O_CLOEXEC);
    }
    if (c.gen_fd >= 0) {
        if (::fstat(c.gen_fd, &st) != 0 || st.st_nlink == 0) {
            ::close(c.gen_fd);
            c.gen_fd = -1;
        } else if (::pread(c.gen_fd, &now.generation, sizeof(now.generation),
                           0) == static_cast<ssize_t>(sizeof(now.generation))) {
            now.by_generation = true;
            now.dir_exists = true;
            now.mtime = st.st_mtimespec;
        }
    }
    if (!now.by_generation && ::stat(dir.c_str(), &st) == 0) {
        now.dir_exists = true;
        now.mtime = st.st_mtimespec;
        now.ctime = st.st_ctimespec;
    }
    if (!(now == c.stamp)) {
        c.stamp = now;
        ++c.epoch;
    }
    return c.epoch;
}

bool parse_record(const std::string& path, HwndRecord& record) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    const ssize_t n = ::pread(fd, &record, sizeof(record), 0);
    ::close(fd);
    return n == static_cast<ssize_t>(sizeof(record)) &&
           record.magic == kMagic && record.version == kVersion &&
           record.owner_host_pid != 0;
}

// Bring `w` up to date with the record file for `hwnd` at this epoch, reusing
// the parse when the file is the same inode/mtime/size. Caller holds the lock.
void refresh_record_locked(Cache& c, u32 hwnd, CachedWindow& w) {
    const std::string path = record_path(hwnd);
    struct stat st {};
    if (path.empty() || ::stat(path.c_str(), &st) != 0) {
        w.present = false;
        w.ino = 0;
        w.record_epoch = c.epoch;
        return;
    }
    const bool same = w.ino == st.st_ino &&
                      w.mtime.tv_sec == st.st_mtimespec.tv_sec &&
                      w.mtime.tv_nsec == st.st_mtimespec.tv_nsec &&
                      w.size == st.st_size && w.ino != 0;
    if (!same) {
        w.ino = st.st_ino;
        w.mtime = st.st_mtimespec;
        w.size = st.st_size;
        w.present = st.st_size == static_cast<off_t>(sizeof(HwndRecord)) &&
                    parse_record(path, w.record) && w.record.hwnd == hwnd;
    }
    w.record_epoch = c.epoch;
}

void refresh_sidecars_locked(Cache& c, u32 hwnd, CachedWindow& w) {
    w.parent_hwnd = read_parent(hwnd);
    w.visible = read_visible(hwnd);
    w.present_host_pid = read_present_host(hwnd);
    w.sidecar_epoch = c.epoch;
}

void remove_window_files(u32 hwnd) {
    std::error_code rm_ec;
    fs::remove(record_path(hwnd), rm_ec);
    fs::remove(parent_path(hwnd), rm_ec);
    fs::remove(visible_path(hwnd), rm_ec);
    fs::remove(present_host_path(hwnd), rm_ec);
    fs::remove(shell_properties_path(hwnd), rm_ec);
}

// One readdir pass. Records are named by decimal HWND with no extension;
// `.parent`, `.tmp` and friends live beside them and are skipped by name.
// Only an entry whose inode is not the one already parsed is opened; the
// rest of the 13k-record launcher capsule costs a directory read and nothing
// else. Caller holds the lock.
void rebuild_index_locked(Cache& c, const std::string& dir) {
    if (c.index_epoch == c.epoch) return;
    std::unordered_map<u32, CachedWindow> fresh;
    fresh.reserve(c.windows.size());
    c.index.clear();
    DIR* d = ::opendir(dir.c_str());
    if (d != nullptr) {
        while (struct dirent* e = ::readdir(d)) {
            const char* name = e->d_name;
            if (name[0] == '\0') continue;
            bool numeric = true;
            for (const char* p = name; *p; ++p) {
                if (*p < '0' || *p > '9') { numeric = false; break; }
            }
            if (!numeric) continue;
            const unsigned long value = std::strtoul(name, nullptr, 10);
            if (value == 0 || value > 0xFFFFFFFFul) continue;
            const u32 hwnd = static_cast<u32>(value);
            CachedWindow w;
            auto it = c.windows.find(hwnd);
            if (it != c.windows.end()) w = it->second;
            if (w.ino != static_cast<ino_t>(e->d_ino) || w.ino == 0) {
                refresh_record_locked(c, hwnd, w);
            } else {
                w.record_epoch = c.epoch;
            }
            if (w.present) c.index.push_back(hwnd);
            fresh.emplace(hwnd, w);
        }
        ::closedir(d);
    }
    // HWNDs looked up but never listed (absent records) keep their negative
    // entry; everything that was listed is replaced by the fresh state.
    for (auto& [hwnd, w] : c.windows) {
        if (fresh.find(hwnd) == fresh.end() && !w.present) {
            fresh.emplace(hwnd, w);
        }
    }
    c.windows.swap(fresh);
    std::sort(c.index.begin(), c.index.end());
    c.index_epoch = c.epoch;
}

// The entry for `hwnd` at the current epoch, or nullptr when there is no
// live record (a dead owner's files are reaped). Caller holds the lock.
CachedWindow* resolve_locked(Cache& c, u32 hwnd) {
    if (hwnd == 0) return nullptr;
    CachedWindow& w = c.windows[hwnd];
    if (w.record_epoch != c.epoch) refresh_record_locked(c, hwnd, w);
    if (!w.present) return nullptr;
    if (!process_is_live(static_cast<pid_t>(w.record.owner_host_pid))) {
        // A dead owner's window does not exist, exactly as on Windows once
        // the process is gone. Reap it rather than leaving it for a scan that
        // may never come: this is what keeps the directory from growing
        // without bound across runs.
        remove_window_files(hwnd);
        w.present = false;
        w.ino = 0;
        return nullptr;
    }
    if (w.sidecar_epoch != c.epoch) refresh_sidecars_locked(c, hwnd, w);
    return &w;
}

HwndRegistryEntry entry_from(const CachedWindow& w) {
    return HwndRegistryEntry{
        w.record.hwnd,
        w.record.owner_host_pid,
        w.record.owner_guest_thread_id,
        w.parent_hwnd,
        w.record.client_w,
        w.record.client_h,
        w.visible,
        w.present_host_pid,
    };
}

} // namespace

u32 hwnd_registry_allocate_handle() {
    constexpr u32 kFirstHwnd = 0x00010000u;
    constexpr u32 kLastHwnd = 0x7FFFFFFEu;

    const std::string dir = registry_directory();
    const std::string path = allocator_path();
    if (dir.empty() || path.empty()) return 0;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return 0;

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return 0;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return 0;
    }

    u32 next = kFirstHwnd;
    const ssize_t read_count = ::pread(fd, &next, sizeof(next), 0);
    if (read_count != static_cast<ssize_t>(sizeof(next)) ||
        next < kFirstHwnd || next > kLastHwnd) {
        next = kFirstHwnd;
    }

    // The counter survives process crashes. Skip any live record left by a
    // process that predates the allocator file or by a wrapped counter.
    while (next <= kLastHwnd && hwnd_registry_lookup(next).has_value()) {
        ++next;
    }
    const u32 allocated = next <= kLastHwnd ? next : 0;
    if (allocated != 0) {
        ++next;
        if (::pwrite(fd, &next, sizeof(next), 0) !=
                static_cast<ssize_t>(sizeof(next)) ||
            ::ftruncate(fd, static_cast<off_t>(sizeof(next))) != 0) {
            next = 0;
        }
    }

    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    return next != 0 ? allocated : 0;
}

namespace {

std::string tid_allocator_path() {
    const std::string root = capsule_root();
    if (root.empty()) return {};
    return root + "/runtime/next-tid";
}

} // namespace

u32 allocate_system_thread_id() {
    // One capsule-wide namespace for primary and worker thread identities.
    constexpr u32 kFirstTid = 0x01000000u;
    constexpr u32 kLastTid = 0x7FFFFFFEu;

    const std::string path = tid_allocator_path();
    if (path.empty()) return 0;

    const std::string dir = path.substr(0, path.rfind('/'));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return 0;

    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return 0;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return 0;
    }

    u32 next = kFirstTid;
    const ssize_t read_count = ::pread(fd, &next, sizeof(next), 0);
    if (read_count != 0 &&
        (read_count != static_cast<ssize_t>(sizeof(next)) ||
         next < kFirstTid || next > kLastTid)) {
        // Exhaustion or a damaged counter must fail, never recycle live IDs.
        (void)::flock(fd, LOCK_UN);
        ::close(fd);
        return 0;
    }

    const u32 allocated = next <= kLastTid ? next : 0;
    if (allocated != 0) {
        ++next;
        if (::pwrite(fd, &next, sizeof(next), 0) !=
                static_cast<ssize_t>(sizeof(next)) ||
            ::ftruncate(fd, static_cast<off_t>(sizeof(next))) != 0) {
            next = 0;
        }
    }

    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    return next != 0 ? allocated : 0;
}

namespace {

void copy_fixed(char* dst, usize cap, std::string_view src) {
    const usize n = src.size() < cap - 1 ? src.size() : cap - 1;
    std::memcpy(dst, src.data(), n);
    std::memset(dst + n, 0, cap - n);
}

bool iequals_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

}  // namespace

void hwnd_registry_publish(u32 hwnd, i32 client_w, i32 client_h,
                           u32 owner_guest_thread_id,
                           std::string_view class_name,
                           std::string_view title) {
    if (hwnd == 0) return;
    const std::string dir = registry_directory();
    if (dir.empty()) return;

    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        VITREO_TRACE("[hwnd_registry] create failed: path=%s error=%s\n",
                     dir.c_str(), ec.message().c_str());
        return;
    }

    const u32 published_tid = owner_guest_thread_id;

    const std::string final_path = record_path(hwnd);
    const std::string temp_path = final_path + ".tmp";
    HwndRecord record{
        kMagic,
        kVersion,
        hwnd,
        static_cast<u32>(::getpid()),
        published_tid,
        client_w > 0 ? client_w : 0,
        client_h > 0 ? client_h : 0,
        0,
        {},
        {},
    };
    // Publish is called again on every move/resize, and most of those callers
    // know the geometry but not the class or title. Treat an empty field as
    // "unchanged" and carry the existing record's value forward, so a resize
    // cannot erase the identity a FindWindow query needs. Without this, a
    // window's class survived only until its first WM_WINDOWPOSCHANGED.
    HwndRecord previous{};
    bool have_previous = false;
    {
        std::ifstream in(final_path, std::ios::binary);
        if (in) {
            in.read(reinterpret_cast<char*>(&previous), sizeof(previous));
            have_previous = in.good() && previous.magic == kMagic &&
                            previous.version == kVersion;
        }
    }
    if (class_name.empty() && have_previous) {
        std::memcpy(record.class_name, previous.class_name,
                    sizeof(record.class_name));
    } else {
        copy_fixed(record.class_name, sizeof(record.class_name), class_name);
    }
    if (title.empty() && have_previous) {
        std::memcpy(record.title, previous.title, sizeof(record.title));
    } else {
        copy_fixed(record.title, sizeof(record.title), title);
    }

    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(&record), sizeof(record));
        out.close();
        if (!out) {
            fs::remove(temp_path, ec);
            return;
        }
    }

    fs::rename(temp_path, final_path, ec);
    if (ec) {
        fs::remove(temp_path, ec);
        return;
    }
    bump_generation();

    VITREO_TRACE("[hwnd_registry] publish hwnd=0x%x owner=%u tid=%u "
                 "client=%dx%d\n", hwnd, record.owner_host_pid,
                 record.owner_guest_thread_id, record.client_w,
                 record.client_h);
}

void hwnd_registry_set_present_host(u32 hwnd, u32 host_pid) {
    const std::string path = present_host_path(hwnd);
    if (path.empty()) return;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return;
    }
    (void)(::pwrite(fd, &host_pid, sizeof(host_pid), 0) ==
               static_cast<ssize_t>(sizeof(host_pid)) &&
           ::ftruncate(fd, static_cast<off_t>(sizeof(host_pid))) == 0);
    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    bump_generation();
    VITREO_TRACE("[hwnd_registry] SetPresentHost hwnd=0x%x pid=%u\n", hwnd,
                 host_pid);
}

void hwnd_registry_set_visible(u32 hwnd, bool visible) {
    const std::string path = visible_path(hwnd);
    if (path.empty()) return;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return;
    }
    const u32 value = visible ? 1u : 0u;
    (void)(::pwrite(fd, &value, sizeof(value), 0) ==
               static_cast<ssize_t>(sizeof(value)) &&
           ::ftruncate(fd, static_cast<off_t>(sizeof(value))) == 0);
    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    bump_generation();
    VITREO_TRACE("[hwnd_registry] SetVisible hwnd=0x%x visible=%d\n", hwnd,
                 visible ? 1 : 0);
}

std::vector<WindowShellProperty> hwnd_registry_shell_properties(u32 hwnd) {
    std::vector<WindowShellProperty> values;
    if (hwnd == 0) return values;
    const std::string path = shell_properties_path(hwnd);
    if (path.empty()) {
        std::lock_guard lock(local_shell_properties_lock());
        const auto found = local_shell_properties().find(hwnd);
        if (found != local_shell_properties().end()) values = found->second;
        return values;
    }
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return values;
    std::string bytes;
    if (::flock(fd, LOCK_SH) == 0) {
        if (!read_fd_fully(fd, bytes) || !parse_shell_properties(bytes, values)) {
            values.clear();
        }
        (void)::flock(fd, LOCK_UN);
    }
    ::close(fd);
    return values;
}

bool hwnd_registry_update_shell_properties(
    u32 hwnd, const std::function<void(std::vector<WindowShellProperty>&)>& edit) {
    if (hwnd == 0) return false;
    const std::string path = shell_properties_path(hwnd);
    if (path.empty()) {
        std::lock_guard lock(local_shell_properties_lock());
        auto& values = local_shell_properties()[hwnd];
        edit(values);
        if (values.empty()) local_shell_properties().erase(hwnd);
        return true;
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return false;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return false;
    }
    std::string bytes;
    std::vector<WindowShellProperty> values;
    bool ok = read_fd_fully(fd, bytes) && parse_shell_properties(bytes, values);
    if (ok) {
        edit(values);
        bytes = values.empty() ? std::string{} : serialize_shell_properties(values);
        ok = ::ftruncate(fd, 0) == 0 &&
             (bytes.empty() ||
              ::pwrite(fd, bytes.data(), bytes.size(), 0) ==
                  static_cast<ssize_t>(bytes.size()));
    }
    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    VITREO_TRACE("[hwnd_registry] SetShellProperties hwnd=0x%x count=%zu ok=%d\n",
                 hwnd, values.size(), ok ? 1 : 0);
    return ok;
}

void hwnd_registry_unpublish(u32 hwnd) {
    {
        std::lock_guard lock(local_shell_properties_lock());
        local_shell_properties().erase(hwnd);
    }
    const std::string path = record_path(hwnd);
    if (path.empty()) return;
    remove_window_files(hwnd);
    bump_generation();
}

std::optional<HwndRegistryEntry> hwnd_registry_lookup(u32 hwnd) {
    const std::string dir = registry_directory();
    if (dir.empty() || hwnd == 0) return std::nullopt;
    Cache& c = cache();
    std::lock_guard<std::mutex> lk(c.lock);
    (void)observe_locked(c, dir);
    const CachedWindow* w = resolve_locked(c, hwnd);
    if (w == nullptr) return std::nullopt;
    return entry_from(*w);
}

std::optional<u32> hwnd_registry_set_parent(u32 hwnd, u32 parent_hwnd) {
    if (!hwnd_registry_lookup(hwnd).has_value()) return std::nullopt;
    const std::string path = parent_path(hwnd);
    if (path.empty()) return std::nullopt;
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd < 0) return std::nullopt;
    if (::flock(fd, LOCK_EX) != 0) {
        ::close(fd);
        return std::nullopt;
    }

    u32 previous = 0;
    if (::pread(fd, &previous, sizeof(previous), 0) !=
        static_cast<ssize_t>(sizeof(previous))) {
        previous = 0;
    }
    const bool written =
        ::pwrite(fd, &parent_hwnd, sizeof(parent_hwnd), 0) ==
            static_cast<ssize_t>(sizeof(parent_hwnd)) &&
        ::ftruncate(fd, static_cast<off_t>(sizeof(parent_hwnd))) == 0;
    (void)::flock(fd, LOCK_UN);
    ::close(fd);
    if (!written) return std::nullopt;
    bump_generation();
    VITREO_TRACE("[hwnd_registry] SetParent hwnd=0x%x parent=0x%x "
                 "previous=0x%x\n", hwnd, parent_hwnd, previous);
    return previous;
}

std::vector<u32> hwnd_registry_live_peer_owners() {
    std::vector<u32> owners;
    const std::string dir = registry_directory();
    if (dir.empty()) return owners;
    Cache& c = cache();
    std::lock_guard<std::mutex> lk(c.lock);
    (void)observe_locked(c, dir);
    rebuild_index_locked(c, dir);
    const u32 self = static_cast<u32>(::getpid());
    // c.index is not modified by resolve_locked, so index-based iteration is
    // safe without copying the (possibly 13k-entry) vector per call.
    for (usize i = 0; i < c.index.size(); ++i) {
        const u32 hwnd = c.index[i];
        auto it = c.windows.find(hwnd);
        if (it == c.windows.end() || !it->second.present) continue;
        const u32 pid = it->second.record.owner_host_pid;
        if (pid == self) continue;
        if (std::find(owners.begin(), owners.end(), pid) != owners.end()) {
            continue;
        }
        if (!process_is_live(static_cast<pid_t>(pid))) continue;
        owners.push_back(pid);
    }
    return owners;
}

std::optional<HwndRegistryEntry> hwnd_registry_find_window(
    std::string_view class_name, std::string_view title) {
    const std::string dir = registry_directory();
    if (dir.empty()) return std::nullopt;
    // Win32 returns NULL for FindWindow(NULL, NULL).
    if (class_name.empty() && title.empty()) return std::nullopt;
    Cache& c = cache();
    std::lock_guard<std::mutex> lk(c.lock);
    (void)observe_locked(c, dir);
    rebuild_index_locked(c, dir);

    // Win32 returns "a" matching window, not a defined one, when several
    // match. Deterministic order (ascending HWND) beats directory order so a
    // capsule with two candidates behaves the same way twice.
    for (usize i = 0; i < c.index.size(); ++i) {
        const u32 hwnd = c.index[i];
        auto it = c.windows.find(hwnd);
        if (it == c.windows.end() || !it->second.present) continue;
        const HwndRecord& record = it->second.record;
        const std::string_view rec_class{record.class_name};
        const std::string_view rec_title{record.title};
        if (!class_name.empty() && !iequals_ascii(rec_class, class_name)) {
            continue;
        }
        if (!title.empty() && !iequals_ascii(rec_title, title)) continue;
        // resolve_locked applies the liveness rule (and reaps a dead owner).
        if (const CachedWindow* w = resolve_locked(c, hwnd)) {
            return entry_from(*w);
        }
    }
    return std::nullopt;
}

std::vector<HwndRegistryEntry> hwnd_registry_enumerate() {
    std::vector<HwndRegistryEntry> out;
    const std::string dir = registry_directory();
    if (dir.empty()) return out;
    Cache& c = cache();
    std::lock_guard<std::mutex> lk(c.lock);
    (void)observe_locked(c, dir);
    rebuild_index_locked(c, dir);
    for (usize i = 0; i < c.index.size(); ++i) {
        const u32 hwnd = c.index[i];
        // resolve_locked validates the record and reaps a dead owner's entry.
        if (const CachedWindow* w = resolve_locked(c, hwnd)) {
            out.push_back(entry_from(*w));
        }
    }
    return out;
}

bool hwnd_registry_is_window(u32 hwnd) {
    return hwnd_registry_lookup(hwnd).has_value();
}

} // namespace vitreo::ipc

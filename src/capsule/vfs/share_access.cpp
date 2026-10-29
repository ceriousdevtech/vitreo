/// @file share_access.cpp
/// @brief Cross-process Windows share-access and byte-range lock arbitration.
/// See share_access.hpp for the contract and the sidecar-lock design.

#include "capsule/vfs/share_access.hpp"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace vitreo::capsule {
namespace {

// Sidecar byte layout.
//   0..2  "some open in this process has read / write / delete access"
//   3..5  "some open in this process denies read / write / delete sharing"
//   64    gate: held exclusively for the duration of check+record
//   kRangeBase + off: projection of LockFile ranges
constexpr off_t kGateByte = 64;
constexpr off_t kRangeBase = 4096;

struct RangeLock {
    u64 owner;
    u64 offset;
    u64 length;
    bool exclusive;
};

struct Entry {
    int fd = -1;
    u32 access_counts[3] = {0, 0, 0};
    u32 deny_counts[3] = {0, 0, 0};
    std::vector<RangeLock> ranges;
    [[nodiscard]] bool idle() const {
        for (int b = 0; b < 3; ++b) {
            if (access_counts[b] != 0 || deny_counts[b] != 0) return false;
        }
        return ranges.empty();
    }
};

struct Key {
    u64 dev;
    u64 ino;
    bool operator<(const Key& o) const {
        return dev != o.dev ? dev < o.dev : ino < o.ino;
    }
};

struct TokenInfo {
    Key key;
    u32 access;
    u32 deny;
};

std::mutex g_mutex;
std::map<Key, Entry>& entries() {
    static auto* m = new std::map<Key, Entry>();
    return *m;
}
std::map<ShareToken, TokenInfo>& tokens() {
    static auto* m = new std::map<ShareToken, TokenInfo>();
    return *m;
}
ShareToken g_next_token = 1;

const std::string& sidecar_dir() {
    static const std::string dir = [] {
        std::string base;
        char buf[1024];
        const size_t n = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
        if (n > 0 && n < sizeof buf) {
            base = buf;
        } else {
            base = "/tmp/";
        }
        if (!base.empty() && base.back() != '/') base.push_back('/');
        base += "vitreo-share-access";
        (void)::mkdir(base.c_str(), 0700);
        return base;
    }();
    return dir;
}

std::string sidecar_path(const Key& k) {
    char name[64];
    std::snprintf(name, sizeof name, "/%llx_%llx",
                  static_cast<unsigned long long>(k.dev),
                  static_cast<unsigned long long>(k.ino));
    return sidecar_dir() + name;
}

bool set_lock(int fd, short type, off_t start, off_t len, bool wait) {
    struct flock fl{};
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    for (;;) {
        if (::fcntl(fd, wait ? F_SETLKW : F_SETLK, &fl) == 0) return true;
        if (errno != EINTR) return false;
    }
}

/// True when ANOTHER process holds a lock on [start, start+len) that
/// conflicts with @p type.
bool other_process_holds(int fd, short type, off_t start, off_t len) {
    struct flock fl{};
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    fl.l_start = start;
    fl.l_len = len;
    if (::fcntl(fd, F_GETLK, &fl) != 0) return false;
    return fl.l_type != F_UNLCK;
}

/// Open (or reuse) the sidecar for @p k and take the cross-process gate.
/// Returns nullptr when no sidecar can be opened; arbitration then degrades
/// to this process only.
Entry* gate_enter(const Key& k) {
    Entry& e = entries()[k];
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (e.fd < 0) {
            e.fd = ::open(sidecar_path(k).c_str(),
                          O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (e.fd < 0) return &e;
        }
        if (!set_lock(e.fd, F_WRLCK, kGateByte, 1, true)) return &e;
        // A peer may have unlinked the sidecar between our open and the
        // gate; locks on an orphaned inode would be invisible to everyone
        // else, so reopen by name.
        struct stat st{};
        if (::fstat(e.fd, &st) == 0 && st.st_nlink > 0) return &e;
        if (!e.idle()) return &e;  // we hold state on it; keep using it
        ::close(e.fd);
        e.fd = -1;
    }
    return &e;
}

void gate_leave(const Key& k, Entry& e) {
    if (e.fd >= 0) (void)set_lock(e.fd, F_UNLCK, kGateByte, 1, false);
    if (e.idle()) {
        if (e.fd >= 0) {
            // Last local user: remove the sidecar if no other process holds
            // anything on it (we still hold the gate, so nobody is between
            // their open and their gate).
            (void)set_lock(e.fd, F_WRLCK, kGateByte, 1, true);
            if (!other_process_holds(e.fd, F_WRLCK, 0, kGateByte) &&
                !other_process_holds(e.fd, F_WRLCK, kGateByte + 1, 0)) {
                (void)::unlink(sidecar_path(k).c_str());
            }
            ::close(e.fd);
        }
        entries().erase(k);
    }
}

bool conflicts(const Entry& e, u32 access, u32 deny) {
    for (int b = 0; b < 3; ++b) {
        const u32 bit = 1u << b;
        if ((access & bit) && e.deny_counts[b] != 0) return true;
        if ((deny & bit) && e.access_counts[b] != 0) return true;
        if (e.fd >= 0) {
            if ((access & bit) && other_process_holds(e.fd, F_WRLCK, 3 + b, 1)) return true;
            if ((deny & bit) && other_process_holds(e.fd, F_WRLCK, b, 1)) return true;
        }
    }
    return false;
}

void add_bits(Entry& e, u32 access, u32 deny) {
    for (int b = 0; b < 3; ++b) {
        const u32 bit = 1u << b;
        if ((access & bit) && e.access_counts[b]++ == 0 && e.fd >= 0) {
            (void)set_lock(e.fd, F_RDLCK, b, 1, false);
        }
        if ((deny & bit) && e.deny_counts[b]++ == 0 && e.fd >= 0) {
            (void)set_lock(e.fd, F_RDLCK, 3 + b, 1, false);
        }
    }
}

void remove_bits(Entry& e, u32 access, u32 deny) {
    for (int b = 0; b < 3; ++b) {
        const u32 bit = 1u << b;
        if ((access & bit) && e.access_counts[b] != 0 &&
            --e.access_counts[b] == 0 && e.fd >= 0) {
            (void)set_lock(e.fd, F_UNLCK, b, 1, false);
        }
        if ((deny & bit) && e.deny_counts[b] != 0 &&
            --e.deny_counts[b] == 0 && e.fd >= 0) {
            (void)set_lock(e.fd, F_UNLCK, 3 + b, 1, false);
        }
    }
}

// --- byte ranges -------------------------------------------------------------

/// Project a guest range onto the sidecar, clamped to what off_t can name.
/// Returns false for an empty range (zero-length locks never conflict).
bool project(u64 offset, u64 length, off_t& start, off_t& len) {
    if (length == 0) return false;
    constexpr u64 kMax = static_cast<u64>(std::numeric_limits<off_t>::max());
    const u64 room = kMax - static_cast<u64>(kRangeBase);
    if (offset >= room) offset = room - 1;
    u64 end = (length > room - offset) ? room : offset + length;
    start = kRangeBase + static_cast<off_t>(offset);
    len = static_cast<off_t>(end - offset);
    return len > 0;
}

bool overlaps(u64 a_off, u64 a_len, u64 b_off, u64 b_len) {
    if (a_len == 0 || b_len == 0) return false;
    const u64 a_end = (a_len > ~0ull - a_off) ? ~0ull : a_off + a_len;
    const u64 b_end = (b_len > ~0ull - b_off) ? ~0ull : b_off + b_len;
    return a_off < b_end && b_off < a_end;
}

/// Re-project every local range overlapping [offset, offset+length) after an
/// unlock: POSIX record locks of one process coalesce, so the F_UNLCK just
/// issued may have removed bytes another local handle still holds.
void reproject(Entry& e, u64 offset, u64 length) {
    if (e.fd < 0) return;
    for (const RangeLock& r : e.ranges) {
        if (!overlaps(r.offset, r.length, offset, length)) continue;
        off_t s, l;
        if (project(r.offset, r.length, s, l)) {
            (void)set_lock(e.fd, r.exclusive ? F_WRLCK : F_RDLCK, s, l, false);
        }
    }
}

}  // namespace

std::optional<ShareToken> share_access_acquire(u64 dev, u64 ino, u32 access,
                                               u32 share) {
    access &= share_bits::All;
    if (access == 0) return ShareToken{0};
    const u32 deny = ~share & share_bits::All;
    const Key k{dev, ino};
    std::lock_guard lock(g_mutex);
    Entry* e = gate_enter(k);
    if (conflicts(*e, access, deny)) {
        gate_leave(k, *e);
        return std::nullopt;
    }
    add_bits(*e, access, deny);
    gate_leave(k, *e);
    const ShareToken t = g_next_token++;
    tokens()[t] = TokenInfo{k, access, deny};
    return t;
}

bool share_access_would_admit(u64 dev, u64 ino, u32 access, u32 share) {
    access &= share_bits::All;
    if (access == 0) return true;
    const u32 deny = ~share & share_bits::All;
    const Key k{dev, ino};
    std::lock_guard lock(g_mutex);
    Entry* e = gate_enter(k);
    const bool ok = !conflicts(*e, access, deny);
    gate_leave(k, *e);
    return ok;
}

void share_access_release(ShareToken token) {
    if (token == 0) return;
    std::lock_guard lock(g_mutex);
    auto it = tokens().find(token);
    if (it == tokens().end()) return;
    const TokenInfo info = it->second;
    tokens().erase(it);
    auto eit = entries().find(info.key);
    if (eit == entries().end()) return;
    Entry& e = eit->second;
    remove_bits(e, info.access, info.deny);
    if (e.idle()) {
        Entry* g = gate_enter(info.key);
        gate_leave(info.key, *g);
    }
}

RangeLockResult range_lock(u64 dev, u64 ino, u64 owner, u64 offset,
                           u64 length, bool exclusive) {
    const Key k{dev, ino};
    std::lock_guard lock(g_mutex);
    Entry* e = gate_enter(k);
    // Learn (LockFile/LockFileEx): an exclusive lock cannot overlap any
    // existing lock — including one the same handle holds (files_core_probe
    // LockFile_h1_overlap_self); shared locks may overlap shared locks.
    for (const RangeLock& r : e->ranges) {
        if (overlaps(r.offset, r.length, offset, length) &&
            (exclusive || r.exclusive)) {
            gate_leave(k, *e);
            return RangeLockResult::Conflict;
        }
    }
    off_t s = 0, l = 0;
    const bool projected = project(offset, length, s, l);
    if (projected && e->fd >= 0 &&
        other_process_holds(e->fd, exclusive ? F_WRLCK : F_RDLCK, s, l)) {
        gate_leave(k, *e);
        return RangeLockResult::Conflict;
    }
    e->ranges.push_back(RangeLock{owner, offset, length, exclusive});
    if (projected && e->fd >= 0) {
        (void)set_lock(e->fd, exclusive ? F_WRLCK : F_RDLCK, s, l, false);
    }
    gate_leave(k, *e);
    return RangeLockResult::Ok;
}

RangeLockResult range_unlock(u64 dev, u64 ino, u64 owner, u64 offset,
                             u64 length) {
    const Key k{dev, ino};
    std::lock_guard lock(g_mutex);
    auto eit = entries().find(k);
    if (eit == entries().end()) return RangeLockResult::NotLocked;
    Entry& e = eit->second;
    // Learn (UnlockFile): the region must correspond exactly to a locked
    // region of this handle; otherwise ERROR_NOT_LOCKED (files_core_probe
    // UnlockFile_partial / UnlockFile_other_handles_lock).
    for (auto it = e.ranges.begin(); it != e.ranges.end(); ++it) {
        if (it->owner == owner && it->offset == offset && it->length == length) {
            e.ranges.erase(it);
            off_t s, l;
            if (e.fd >= 0 && project(offset, length, s, l)) {
                (void)set_lock(e.fd, F_UNLCK, s, l, false);
                reproject(e, offset, length);
            }
            if (e.idle()) {
                Entry* g = gate_enter(k);
                gate_leave(k, *g);
            }
            return RangeLockResult::Ok;
        }
    }
    return RangeLockResult::NotLocked;
}

void range_unlock_all(u64 owner) {
    std::lock_guard lock(g_mutex);
    std::vector<Key> idle;
    for (auto& [k, e] : entries()) {
        bool touched = false;
        for (auto it = e.ranges.begin(); it != e.ranges.end();) {
            if (it->owner == owner) {
                off_t s, l;
                if (e.fd >= 0 && project(it->offset, it->length, s, l)) {
                    (void)set_lock(e.fd, F_UNLCK, s, l, false);
                }
                const u64 off = it->offset, len = it->length;
                it = e.ranges.erase(it);
                reproject(e, off, len);
                touched = true;
            } else {
                ++it;
            }
        }
        if (touched && e.idle()) idle.push_back(k);
    }
    for (const Key& k : idle) {
        Entry* g = gate_enter(k);
        gate_leave(k, *g);
    }
}

bool range_io_allowed(u64 dev, u64 ino, u64 owner, u64 offset, u64 length,
                      bool write) {
    if (length == 0) return true;
    std::lock_guard lock(g_mutex);
    auto eit = entries().find(Key{dev, ino});
    if (eit != entries().end()) {
        for (const RangeLock& r : eit->second.ranges) {
            if (!overlaps(r.offset, r.length, offset, length)) continue;
            // Learn (LockFileEx): an exclusive lock denies all other
            // handles; a shared lock denies every writer, the owner included.
            if (r.exclusive && r.owner != owner) return false;
            if (!r.exclusive && write) return false;
        }
    }
    // Other processes: their projected locks. An entry exists only while
    // this process uses the sidecar, so open it transiently otherwise.
    const Key k{dev, ino};
    auto it = entries().find(k);
    int fd = (it != entries().end()) ? it->second.fd : -1;
    bool transient = false;
    if (fd < 0) {
        fd = ::open(sidecar_path(k).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) return true;  // no sidecar: nobody has ever locked it
        transient = true;
    }
    off_t s, l;
    bool allowed = true;
    if (project(offset, length, s, l)) {
        allowed = !other_process_holds(fd, write ? F_WRLCK : F_RDLCK, s, l);
    }
    if (transient) ::close(fd);
    return allowed;
}

}  // namespace vitreo::capsule

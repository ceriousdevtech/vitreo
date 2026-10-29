// SPDX-License-Identifier: LGPL-2.1-only
/// @file shared_memory.cpp

#include "ipc/shared_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"

#include <array>
#include <dirent.h>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <libproc.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <unistd.h>

namespace vitreo::ipc {

namespace {

constexpr u32 kLifetimeMagic = 0x56544d4cu; // "VTML"
constexpr u32 kLifetimeVersion = 2;
constexpr usize kLifetimeSlotCount = 64;

struct LifetimeSlot {
    i32 pid;
    u32 references;
};

struct LifetimeRecord {
    u32 magic;
    u32 version;
    // POSIX shm implementations may report a page-rounded st_size after the
    // object has been mapped. Windows section objects retain the exact
    // MaximumSize supplied by their creator, and openers must observe that
    // same logical size. Keep it beside the lifetime data instead of deriving
    // guest-visible capacity from host allocation geometry.
    u64 logical_size;
    std::array<LifetimeSlot, kLifetimeSlotCount> slots;
};

// Keep the version-2 lifetime prefix intact. Older live mappings have no
// descriptor and cannot be imported as Windows sections without guessing.
struct SectionRecord {
    LifetimeRecord lifetime;
    u32 descriptor_version;
    u32 protection;
};
constexpr u32 kSectionDescriptorVersion = 1;

class ScopeLock {
public:
    ScopeLock() {
        const u64 scope_hash = fnv1a64(ipc_scope_key());
        char path[96];
        std::snprintf(path, sizeof(path), "/tmp/vtmlock_%u_%016llx",
                      static_cast<unsigned>(::getuid()),
                      static_cast<unsigned long long>(scope_hash));
        m_fd = ::open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (m_fd < 0) return;
        while (::flock(m_fd, LOCK_EX) != 0) {
            if (errno == EINTR) continue;
            ::close(m_fd);
            m_fd = -1;
            return;
        }
        m_locked = true;
    }

    ~ScopeLock() {
        if (m_fd < 0) return;
        if (m_locked) (void)::flock(m_fd, LOCK_UN);
        (void)::close(m_fd);
    }

    ScopeLock(const ScopeLock&) = delete;
    ScopeLock& operator=(const ScopeLock&) = delete;

    [[nodiscard]] bool locked() const noexcept { return m_locked; }

private:
    int m_fd{-1};
    bool m_locked{false};
};

[[nodiscard]] bool process_is_live(i32 pid) {
    if (pid <= 0) return false;
    // Darwin: kill(pid, 0) succeeds for zombies. Windows closes a process's
    // handles at termination, so a named mutex must not stay visible after
    // ExitProcess just because the host has not waitpid'd the worker yet.
    proc_bsdinfo info {};
    const int n = ::proc_pidinfo(static_cast<pid_t>(pid), PROC_PIDTBSDINFO, 0,
                                 &info, static_cast<int>(sizeof(info)));
    if (n == static_cast<int>(sizeof(info))) {
        return info.pbi_status != SZOMB;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) return false;
    return errno == EPERM;
}

void log_lifetime_failure(const char* operation,
                          const std::string& posix_name,
                          int error = errno) {
    ipc_log("SharedMemory lifetime %s name='%s' errno=%d",
            operation, posix_name.c_str(), error);
}

void initialize_lifetime_record(LifetimeRecord& record) {
    std::memset(&record, 0, sizeof(record));
    record.magic = kLifetimeMagic;
    record.version = kLifetimeVersion;
}

void prune_dead_references(LifetimeRecord& record) {
    for (auto& slot : record.slots) {
        if (slot.references == 0) {
            slot.pid = 0;
            continue;
        }
        if (!process_is_live(slot.pid)) {
            slot = {};
        }
    }
}

[[nodiscard]] u64 total_references(const LifetimeRecord& record) {
    u64 total = 0;
    for (const auto& slot : record.slots) {
        total += slot.references;
    }
    return total;
}

// Registry of the objects this scope has created: one file per live object,
// named after its data shm (without the leading '/'), holding the lifetime
// shm's name. POSIX shm cannot be enumerated, so this is the only way a later
// process can find objects whose last referencing process was killed. It
// lives in /tmp beside the scope lock: both survive exactly as long as the
// shm objects do (until reboot).
[[nodiscard]] std::string registry_dir() {
    const u64 scope_hash = fnv1a64(ipc_scope_key());
    char path[96];
    std::snprintf(path, sizeof(path), "/tmp/vtmreg_%u_%016llx",
                  static_cast<unsigned>(::getuid()),
                  static_cast<unsigned long long>(scope_hash));
    return path;
}

[[nodiscard]] std::string registry_entry(const std::string& dir,
                                         const std::string& posix_name) {
    return dir + "/" + posix_name.substr(1);
}

// Caller holds the ScopeLock.
void register_object(const std::string& posix_name,
                     const std::string& lifetime_name) {
    const std::string dir = registry_dir();
    if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
        log_lifetime_failure("registry-mkdir", posix_name);
        return;
    }
    const std::string entry = registry_entry(dir, posix_name);
    const int fd = ::open(entry.c_str(),
                          O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        log_lifetime_failure("registry-open", posix_name);
        return;
    }
    const ssize_t want = static_cast<ssize_t>(lifetime_name.size());
    if (::write(fd, lifetime_name.data(), lifetime_name.size()) != want) {
        log_lifetime_failure("registry-write", posix_name);
    }
    (void)::close(fd);
}

// Caller holds the ScopeLock. Removes both host names and the record.
void retire_object(const std::string& posix_name,
                   const std::string& lifetime_name) {
    (void)::shm_unlink(posix_name.c_str());
    (void)::shm_unlink(lifetime_name.c_str());
    (void)::unlink(registry_entry(registry_dir(), posix_name).c_str());
}

// Caller holds the ScopeLock. True when the object named by @p entry_name
// had no live referencing process and was removed.
[[nodiscard]] bool reap_entry(const std::string& dir,
                              const std::string& entry_name) {
    const std::string posix_name = "/" + entry_name;
    const std::string entry = dir + "/" + entry_name;
    std::string lifetime_name;
    {
        const int fd = ::open(entry.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        char buf[64];
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        (void)::close(fd);
        if (n <= 1 || buf[0] != '/') return false;
        lifetime_name.assign(buf, static_cast<usize>(n));
    }

    const int lifetime_fd = ::shm_open(lifetime_name.c_str(), O_RDWR, 0600);
    if (lifetime_fd < 0) {
        // No lifetime record means no process can hold a reference.
        const bool removed = ::shm_unlink(posix_name.c_str()) == 0;
        (void)::unlink(entry.c_str());
        return removed;
    }
    bool dead = false;
    struct stat st {};
    if (::fstat(lifetime_fd, &st) == 0 &&
        st.st_size >= static_cast<off_t>(sizeof(LifetimeRecord))) {
        void* map = ::mmap(nullptr, sizeof(LifetimeRecord),
                           PROT_READ | PROT_WRITE, MAP_SHARED, lifetime_fd, 0);
        if (map != MAP_FAILED) {
            auto* record = static_cast<LifetimeRecord*>(map);
            if (record->magic == kLifetimeMagic &&
                record->version == kLifetimeVersion) {
                prune_dead_references(*record);
                dead = total_references(*record) == 0;
            }
            (void)::munmap(map, sizeof(LifetimeRecord));
        }
    }
    (void)::close(lifetime_fd);
    if (!dead) return false;
    retire_object(posix_name, lifetime_name);
    return true;
}

// Caller holds the ScopeLock.
[[nodiscard]] usize reap_scope_locked() {
    const std::string dir = registry_dir();
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return 0;
    std::vector<std::string> names;
    while (const dirent* e = ::readdir(d)) {
        if (std::strncmp(e->d_name, "vtm", 3) == 0) names.emplace_back(e->d_name);
    }
    (void)::closedir(d);
    usize removed = 0;
    for (const auto& name : names) {
        if (reap_entry(dir, name)) ++removed;
    }
    if (removed != 0) {
        ipc_log("SharedMemory reaped %zu object(s) left by terminated "
                "processes in scope", removed);
    }
    return removed;
}

// Caller holds the ScopeLock. Sweeps each scope once per host process.
void reap_scope_once_locked() {
    static std::mutex mu;
    static std::unordered_set<std::string> swept;
    {
        std::lock_guard<std::mutex> guard(mu);
        if (!swept.insert(ipc_scope_key()).second) return;
    }
    (void)reap_scope_locked();
}

[[nodiscard]] bool add_reference(LifetimeRecord& record, i32 pid) {
    LifetimeSlot* empty = nullptr;
    for (auto& slot : record.slots) {
        if (slot.references != 0 && slot.pid == pid) {
            if (slot.references == std::numeric_limits<u32>::max()) {
                return false;
            }
            ++slot.references;
            return true;
        }
        if (empty == nullptr && slot.references == 0) empty = &slot;
    }
    if (empty == nullptr) return false;
    empty->pid = pid;
    empty->references = 1;
    return true;
}

void remove_reference(LifetimeRecord& record, i32 pid) {
    for (auto& slot : record.slots) {
        if (slot.references == 0 || slot.pid != pid) continue;
        --slot.references;
        if (slot.references == 0) slot = {};
        return;
    }
}

} // namespace

std::shared_ptr<SharedMemory> SharedMemory::create_or_open(
    const std::string& name, u64 size, bool create) {
    return create_impl(name, size, create, std::nullopt);
}

std::shared_ptr<SharedMemory> SharedMemory::create_section(
    const std::string& name, u64 size, bool create, u32 protection) {
    return create_impl(name, size, create, protection);
}

std::optional<u32> SharedMemory::section_protection() const noexcept {
    if (m_lifetime_map_len < sizeof(SectionRecord)) return std::nullopt;
    const auto* record = static_cast<const SectionRecord*>(m_lifetime_map);
    if (record->descriptor_version != kSectionDescriptorVersion) return std::nullopt;
    return record->protection;
}

std::shared_ptr<SharedMemory> SharedMemory::create_impl(
    const std::string& win_name, u64 size, bool create,
    std::optional<u32> protection) {
    const std::string pname = posix_shm_name(win_name);
    const std::string lifetime_name =
        posix_shm_name("lifetime:" + win_name);
    ScopeLock scope_lock;
    if (!scope_lock.locked()) {
        log_lifetime_failure("lock", pname);
        return nullptr;
    }
    reap_scope_once_locked();

    int lifetime_fd =
        ::shm_open(lifetime_name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    const bool lifetime_created = lifetime_fd >= 0;
    if (lifetime_fd < 0 && errno == EEXIST) {
        lifetime_fd = ::shm_open(lifetime_name.c_str(), O_RDWR, 0600);
    }
    if (lifetime_fd < 0) {
        log_lifetime_failure("open-metadata", lifetime_name);
        return nullptr;
    }

    if (lifetime_created &&
        ::ftruncate(lifetime_fd,
                    static_cast<off_t>(sizeof(SectionRecord))) != 0) {
        (void)::close(lifetime_fd);
        (void)::shm_unlink(lifetime_name.c_str());
        log_lifetime_failure("size-metadata", lifetime_name);
        return nullptr;
    }

    struct stat lifetime_stat {};
    if (::fstat(lifetime_fd, &lifetime_stat) != 0 ||
        lifetime_stat.st_size <
            static_cast<off_t>(sizeof(SectionRecord))) {
        if (::ftruncate(lifetime_fd,
                        static_cast<off_t>(sizeof(SectionRecord))) != 0) {
            log_lifetime_failure("resize-metadata", lifetime_name);
            (void)::close(lifetime_fd);
            return nullptr;
        }
    }

    void* lifetime_map =
        ::mmap(nullptr, sizeof(SectionRecord), PROT_READ | PROT_WRITE,
               MAP_SHARED, lifetime_fd, 0);
    if (lifetime_map == MAP_FAILED) {
        log_lifetime_failure("map-metadata", lifetime_name);
        (void)::close(lifetime_fd);
        return nullptr;
    }
    auto* lifetime = static_cast<LifetimeRecord*>(lifetime_map);
    if (lifetime_created || lifetime->magic != kLifetimeMagic ||
        lifetime->version != kLifetimeVersion) {
        initialize_lifetime_record(*lifetime);
    }
    prune_dead_references(*lifetime);

    const bool has_live_references = total_references(*lifetime) != 0;
    int fd = -1;
    bool creator = false;
    if (!has_live_references) {
        // A creator that was killed could not run its destructor. With no live
        // references, the POSIX object is stale and Windows would no longer
        // expose its name.
        (void)::shm_unlink(pname.c_str());
        if (!create) {
            (void)::munmap(lifetime_map, sizeof(SectionRecord));
            (void)::close(lifetime_fd);
            (void)::shm_unlink(lifetime_name.c_str());
            log_lifetime_failure("open-no-live-refs", pname, 0);
            return nullptr;
        }
        fd = ::shm_open(pname.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        creator = fd >= 0;
    } else {
        fd = ::shm_open(pname.c_str(), O_RDWR, 0600);
    }
    if (fd < 0) {
        log_lifetime_failure(
            has_live_references ? "open-data" : "create-data", pname);
        (void)::munmap(lifetime_map, sizeof(SectionRecord));
        (void)::close(lifetime_fd);
        if (!has_live_references) {
            (void)::shm_unlink(lifetime_name.c_str());
        }
        return nullptr;
    }

    if (creator) {
        if (::ftruncate(fd, static_cast<off_t>(size)) != 0) {
            log_lifetime_failure("size-data", pname);
            (void)::close(fd);
            (void)::shm_unlink(pname.c_str());
            (void)::munmap(lifetime_map, sizeof(SectionRecord));
            (void)::close(lifetime_fd);
            (void)::shm_unlink(lifetime_name.c_str());
            return nullptr;
        }
        lifetime->logical_size = size;
        auto* section = static_cast<SectionRecord*>(lifetime_map);
        section->descriptor_version = protection ? kSectionDescriptorVersion : 0;
        section->protection = protection.value_or(0);
    } else {
        // Attach: adopt the creator's Windows-visible logical size. On Darwin,
        // fstat may expose the page-rounded POSIX backing size after mmap;
        // using that value makes two endpoints derive different ring-buffer
        // capacities from the same named section.
        struct stat st {};
        if (::fstat(fd, &st) == 0 && st.st_size > 0) {
            const u64 host_size = static_cast<u64>(st.st_size);
            size = lifetime->logical_size != 0 &&
                           lifetime->logical_size <= host_size
                       ? lifetime->logical_size
                       : host_size;
        }
    }

    if (!add_reference(*lifetime, static_cast<i32>(::getpid()))) {
        log_lifetime_failure("reference-table-full", lifetime_name, 0);
        (void)::close(fd);
        if (creator) (void)::shm_unlink(pname.c_str());
        (void)::munmap(lifetime_map, sizeof(SectionRecord));
        (void)::close(lifetime_fd);
        if (!has_live_references) {
            (void)::shm_unlink(lifetime_name.c_str());
        }
        return nullptr;
    }

    // Record the object while it is live so that, if every referencing
    // process is killed, a later process can still find and remove it.
    // Openers find the creator's record already in place.
    if (creator) register_object(pname, lifetime_name);

    auto sm = std::shared_ptr<SharedMemory>(new SharedMemory());
    sm->m_fd = fd;
    sm->m_lifetime_fd = lifetime_fd;
    sm->m_size = size;
    sm->m_creator = creator;
    sm->m_lifetime_map = lifetime_map;
    sm->m_lifetime_map_len = sizeof(SectionRecord);
    sm->m_posix_name = pname;
    sm->m_lifetime_posix_name = lifetime_name;
    return sm;
}

SharedMemory::~SharedMemory() {
    if (m_lifetime_map != nullptr && m_lifetime_fd >= 0) {
        ScopeLock scope_lock;
        if (scope_lock.locked()) {
            auto* lifetime =
                static_cast<LifetimeRecord*>(m_lifetime_map);
            if (lifetime->magic == kLifetimeMagic &&
                lifetime->version == kLifetimeVersion) {
                prune_dead_references(*lifetime);
                remove_reference(*lifetime,
                                 static_cast<i32>(::getpid()));
                if (total_references(*lifetime) == 0) {
                    retire_object(m_posix_name, m_lifetime_posix_name);
                }
            }
        }
        (void)::munmap(m_lifetime_map, m_lifetime_map_len);
        (void)::close(m_lifetime_fd);
    }
    if (m_fd >= 0) {
        (void)::close(m_fd);
    }
}

usize SharedMemory::reap_stale_objects() {
    ScopeLock scope_lock;
    if (!scope_lock.locked()) return 0;
    return reap_scope_locked();
}

void* SharedMemory::map_view(u64 offset, usize len) {
    void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED,
                     m_fd, static_cast<off_t>(offset));
    return (p == MAP_FAILED) ? nullptr : p;
}

void SharedMemory::unmap_view(void* p, usize len) {
    if (p != nullptr) {
        ::munmap(p, len);
    }
}

} // namespace vitreo::ipc

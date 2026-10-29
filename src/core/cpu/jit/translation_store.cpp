// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

/// @file translation_store.cpp
/// @brief Persistent translation store: record format, MAC, fingerprint,
///        directory lifecycle, index and writer. See translation_store.hpp.

#include "translation_store.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/loader.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

extern char** environ;

namespace vitreo::cpu::jit {

namespace {

// ---------------------------------------------------------------------------
// Hashing
// ---------------------------------------------------------------------------

inline u64 load64(const u8* p) noexcept {
    u64 v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

u64 fnv1a64(const void* data, usize size, u64 h = 14695981039346656037ull) noexcept {
    const auto* b = static_cast<const u8*>(data);
    for (usize i = 0; i < size; ++i) {
        h ^= b[i];
        h *= 1099511628211ull;
    }
    return h;
}

inline u64 rotl64(u64 x, int b) noexcept { return (x << b) | (x >> (64 - b)); }

// SipHash-2-4, 64-bit output: Aumasson & Bernstein, "SipHash: a fast
// short-input PRF" (INDOCRYPT 2012), section 2.
u64 siphash24_impl(const u8 key[16], const u8* in, usize len) noexcept {
    const u64 k0 = load64(key);
    const u64 k1 = load64(key + 8);
    u64 v0 = 0x736f6d6570736575ull ^ k0;
    u64 v1 = 0x646f72616e646f6dull ^ k1;
    u64 v2 = 0x6c7967656e657261ull ^ k0;
    u64 v3 = 0x7465646279746573ull ^ k1;
    const auto round = [&] {
        v0 += v1; v1 = rotl64(v1, 13); v1 ^= v0; v0 = rotl64(v0, 32);
        v2 += v3; v3 = rotl64(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotl64(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotl64(v1, 17); v1 ^= v2; v2 = rotl64(v2, 32);
    };
    const usize end = len - (len % 8);
    for (usize i = 0; i < end; i += 8) {
        const u64 m = load64(in + i);
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    }
    u64 b = static_cast<u64>(len) << 56;
    for (usize i = 0; i < (len & 7); ++i) {
        b |= static_cast<u64>(in[end + i]) << (8 * i);
    }
    v3 ^= b;
    round();
    round();
    v0 ^= b;
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// ---------------------------------------------------------------------------
// Environment snapshot (design §4.1): every VITREO_* variable except those
// that differ per process or per launch and do not reach the emitter.
// ---------------------------------------------------------------------------

bool env_excluded(std::string_view name) noexcept {
    static constexpr std::string_view kExact[] = {
        "VITREO_LOG_DIR",
        "VITREO_IPC_SCOPE",
        "VITREO_SCOPED_ENV",
        "VITREO_SCOPED_ENV_REEXEC",
        "VITREO_PROCESS_MITIGATION_POLICY",
    };
    static constexpr std::string_view kPrefix[] = {
        "VITREO_MACOS_",
        "VITREO_STARTUP",
        "VITREO_PRIMARY_THREAD_",
        "VITREO_TRANSLATION_STORE",
    };
    for (std::string_view e : kExact) {
        if (name == e) return true;
    }
    for (std::string_view p : kPrefix) {
        if (name.starts_with(p)) return true;
    }
    return false;
}

std::vector<std::string> collect_vitreo_env() {
    std::vector<std::string> lines;
    for (char** e = environ; e != nullptr && *e != nullptr; ++e) {
        const std::string_view s(*e);
        if (!s.starts_with("VITREO_")) continue;
        const std::string_view name = s.substr(0, s.find('='));
        if (env_excluded(name)) continue;
        lines.emplace_back(s);
    }
    std::sort(lines.begin(), lines.end());
    return lines;
}

// What knobs latched at static initialization saw.
const std::vector<std::string> g_env_at_start = collect_vitreo_env();

// ---------------------------------------------------------------------------
// The host image whose helper addresses records relocate.
// ---------------------------------------------------------------------------

struct HostImage {
    bool ok{false};
    u64 base{0};  ///< Mach header address: relocation origin
    u64 lo{0};
    u64 hi{0};
    u8 uuid[16]{};
};

const HostImage& host_image() noexcept {
    static const HostImage image = [] {
        HostImage out;
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&TranslationStore::active), &info) == 0 ||
            info.dli_fbase == nullptr) {
            return out;
        }
        const auto* mh = static_cast<const mach_header_64*>(info.dli_fbase);
        if (mh->magic != MH_MAGIC_64) return out;
        const auto* cmd = reinterpret_cast<const u8*>(mh + 1);
        u64 text_vmaddr = 0;
        u64 vm_lo = ~u64{0};
        u64 vm_hi = 0;
        bool have_uuid = false;
        bool have_text = false;
        for (u32 i = 0; i < mh->ncmds; ++i) {
            const auto* lc = reinterpret_cast<const load_command*>(cmd);
            if (lc->cmd == LC_UUID) {
                std::memcpy(out.uuid, reinterpret_cast<const uuid_command*>(lc)->uuid, 16);
                have_uuid = true;
            } else if (lc->cmd == LC_SEGMENT_64) {
                const auto* seg = reinterpret_cast<const segment_command_64*>(lc);
                if (std::strncmp(seg->segname, "__PAGEZERO", 16) != 0) {
                    vm_lo = std::min<u64>(vm_lo, seg->vmaddr);
                    vm_hi = std::max<u64>(vm_hi, seg->vmaddr + seg->vmsize);
                }
                if (std::strncmp(seg->segname, "__TEXT", 16) == 0) {
                    text_vmaddr = seg->vmaddr;
                    have_text = true;
                }
            }
            cmd += lc->cmdsize;
        }
        if (!have_uuid || !have_text || vm_lo >= vm_hi) return out;
        const u64 slide = reinterpret_cast<u64>(mh) - text_vmaddr;
        out.base = reinterpret_cast<u64>(mh);
        out.lo = vm_lo + slide;
        out.hi = vm_hi + slide;
        out.ok = true;
        return out;
    }();
    return image;
}

// ---------------------------------------------------------------------------
// File formats (little-endian, this host only)
// ---------------------------------------------------------------------------

constexpr u32 kRecordMagic = 0x31525354u;  // "TSR1"
constexpr u32 kFormatVersion = 6;
constexpr usize kMaxRecordBytes = 1u << 20;
/// Compaction merges dead segments smaller than this into segments of at
/// most this size; full ones are left alone, so a store at its cap is not
/// rewritten every few launches and LRU eviction keeps a useful grain.
constexpr u64 kCompactFullBytes = 64ull << 20;
/// Dead small segments needed before a compaction runs.
constexpr u32 kDefaultCompactMin = 4;
/// How long the first compile of a process waits for the index. Measured
/// (M4 Pro, jit_bench kern64+app64, 432 processes): uncompacted, 432
/// segments / 37.7k records / 43 MB took 18-140 ms, while a whole guest's
/// compiles took ~1.5 ms, so a background index always lost; compacted, the
/// same workload is 3 segments / 3.7k records / 4 MB, ready in ~0.9 ms
/// (~0.24 us per record). A store at the 2 GB cap would exceed the budget;
/// the process then runs on, and its early records are checked against the
/// index before they are written.
constexpr u32 kDefaultIndexWaitMs = 100;

struct RecordHeader {
    u32 magic;
    u32 total_len;     ///< whole record, multiple of 8
    u64 mac;           ///< SipHash-2-4 over bytes [16, total_len)
    u64 tag;           ///< index tag of `key`
    u32 header_check;  ///< FNV of the header with mac and this field zero
    u32 payload_len;   ///< encoded StoredEmission bytes
    StoredKey key;
    // then guest bytes (key.guest_size, padded to 8), then the payload
    // (padded to 8)
};
static_assert(sizeof(RecordHeader) == 112);

constexpr char kSegmentMagic[8] = {'V', 'T', 'S', 'S', 'E', 'G', '0', '1'};
struct SegmentHeader {
    char magic[8];
    u32 version;
    u32 header_len;
    u8 fingerprint[16];
    u64 producer_host_base;
    u64 created_ms;
    u32 pid;
    u32 reserved;
    u64 mac;  ///< SipHash-2-4 over the 56 bytes before it
};
static_assert(sizeof(SegmentHeader) == 64);

constexpr usize pad8(usize n) noexcept { return (n + 7) & ~usize{7}; }

u64 key_tag(const StoredKey& k) noexcept { return fnv1a64(&k, sizeof k) | 1u; }

u32 record_header_check(const RecordHeader& h) noexcept {
    RecordHeader c = h;
    c.mac = 0;
    c.header_check = 0;
    return static_cast<u32>(fnv1a64(&c, sizeof c));
}

u64 realtime_ms() noexcept {
    timeval tv{};
    gettimeofday(&tv, nullptr);
    return static_cast<u64>(tv.tv_sec) * 1000u + static_cast<u64>(tv.tv_usec) / 1000u;
}

bool read_exact(int fd, void* out, usize n) noexcept {
    auto* p = static_cast<u8*>(out);
    while (n != 0) {
        const ssize_t r = ::read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= static_cast<usize>(r);
    }
    return true;
}

bool write_exact(int fd, const void* data, usize n) noexcept {
    const auto* p = static_cast<const u8*>(data);
    while (n != 0) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= static_cast<usize>(w);
    }
    return true;
}

bool make_private_dir(const std::string& path) noexcept {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) return false;
    struct stat sb{};
    if (::lstat(path.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode) || sb.st_uid != ::getuid()) {
        return false;
    }
    if ((sb.st_mode & 0777) != 0700) (void)::chmod(path.c_str(), 0700);
    return true;
}

std::string hex(const u8* b, usize n) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (usize i = 0; i < n; ++i) {
        s.push_back(kDigits[b[i] >> 4]);
        s.push_back(kDigits[b[i] & 15]);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Process-wide state
// ---------------------------------------------------------------------------

struct Segment {
    int fd{-1};
    const u8* base{nullptr};
    usize size{0};
    u64 producer_base{0};
    std::string path;
    mutable std::atomic<bool> touched{false};
};

struct Index {
    struct Slot {
        u64 tag{0};
        const u8* rec{nullptr};
        const Segment* seg{nullptr};
    };
    std::vector<Slot> slots;
    usize mask{0};
    usize records{0};
};

struct State {
    std::mutex mutex;  ///< activation and the writer
    std::string root;
    std::string dir;
    std::string fingerprint_hex;
    u8 mac_key[16]{};
    u8 fingerprint[16]{};
    int live_fd{-1};

    // writer
    int seg_fd{-1};
    std::string seg_path;
    std::vector<u8> buffer;
    /// Records offered before the index was ready, each checked against it
    /// before it is written (flush_locked).
    std::vector<std::vector<u8>> unchecked;
    u64 unchecked_bytes{0};
    u64 written{0};
    u64 existing_bytes{0};
    u64 cap_bytes{0};
    std::atomic<bool> producer_off{false};

    // reader
    std::vector<std::unique_ptr<Segment>> segments;
    std::atomic<const Index*> index{nullptr};
    std::atomic<bool> index_done{false};
    std::mutex index_mutex;
    std::condition_variable index_cv;
    std::atomic<bool> gc_done{false};
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> poisoned{false};
    u32 verify_every{256};
    u32 compact_min{kDefaultCompactMin};
    u32 index_wait_ms{kDefaultIndexWaitMs};
    std::atomic<u64> hits{0};

    std::shared_mutex seen_mutex;
    std::unordered_set<u64> seen;
};

State* g_state = new State();
std::atomic<int> g_active{-1};  // -1 undecided, 0 inactive, 1 active
std::mutex g_activation_mutex;
std::atomic<u32> g_index_delay_ms_for_test{0};

void compact_segments(State& s);

/// Append one authenticated record for (key, guest bytes, payload) to @p out.
void append_record(const u8 mac_key[16], const StoredKey& key, const u8* guest_bytes,
                   const std::vector<u8>& payload, std::vector<u8>& out) {
    const usize total = sizeof(RecordHeader) + pad8(key.guest_size) + pad8(payload.size());
    const usize at = out.size();
    out.resize(at + total, 0);
    u8* rec = out.data() + at;
    RecordHeader h{};
    h.magic = kRecordMagic;
    h.total_len = static_cast<u32>(total);
    h.tag = key_tag(key);
    h.payload_len = static_cast<u32>(payload.size());
    h.key = key;
    h.header_check = record_header_check(h);
    std::memcpy(rec, &h, sizeof h);
    std::memcpy(rec + sizeof h, guest_bytes, key.guest_size);
    std::memcpy(rec + sizeof h + pad8(key.guest_size), payload.data(), payload.size());
    const u64 mac = siphash24_impl(mac_key, rec + 16, total - 16);
    std::memcpy(rec + offsetof(RecordHeader, mac), &mac, sizeof mac);
}

/// An authentic record for @p key whose stored bytes equal @p guest_bytes.
const u8* find_valid_record(const State& s, const Index& index, const StoredKey& key,
                            const u8* guest_bytes) noexcept {
    const u64 tag = key_tag(key);
    for (usize i = static_cast<usize>(tag) & index.mask;; i = (i + 1) & index.mask) {
        const Index::Slot& slot = index.slots[i];
        if (slot.rec == nullptr) return nullptr;
        if (slot.tag != tag) continue;
        RecordHeader h;
        std::memcpy(&h, slot.rec, sizeof h);
        if (std::memcmp(&h.key, &key, sizeof key) != 0) continue;
        if (std::memcmp(slot.rec + sizeof(RecordHeader), guest_bytes, key.guest_size) != 0) {
            continue;
        }
        if (h.mac != siphash24_impl(s.mac_key, slot.rec + 16, h.total_len - 16)) continue;
        return slot.rec;
    }
}

u32 env_u32(const char* name, u32 fallback, u32 lo, u32 hi) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') return fallback;
    char* end = nullptr;
    const unsigned long n = std::strtoul(v, &end, 10);
    if (end != v && *end == '\0' && n >= lo && n <= hi) return static_cast<u32>(n);
    std::fprintf(stderr, "[tstore] %s=%s IGNORED: expected %u..%u\n", name, v, lo, hi);
    return fallback;
}

std::string choose_root() {
    if (const char* forced = std::getenv("VITREO_TRANSLATION_STORE_DIR");
        forced != nullptr && forced[0] != '\0') {
        return forced;
    }
    const char* scope = std::getenv("VITREO_IPC_SCOPE");
    if (scope == nullptr || scope[0] == '\0') return {};
    return std::string(scope) + "/cache/jit-store";
}

void host_cpu_identity(std::string& out) {
    const auto add = [&](const char* name) {
        u64 value = 0;
        usize size = sizeof value;
        if (sysctlbyname(name, &value, &size, nullptr, 0) != 0) value = ~u64{0};
        out += name;
        out += '=';
        out += std::to_string(value);
        out += '\n';
    };
    add("hw.cpufamily");
    add("hw.cpusubfamily");
    add("hw.optional.arm.FEAT_LSE");
    add("hw.optional.arm.FEAT_LSE2");
    add("hw.optional.arm.FEAT_LRCPC");
    add("hw.optional.arm.FEAT_LRCPC2");
    add("hw.optional.arm.FEAT_FlagM");
    add("hw.optional.arm.FEAT_FlagM2");
    add("hw.optional.arm.FEAT_AFP");
    add("hw.optional.arm.FEAT_FRINTTS");
    add("hw.optional.arm.FEAT_RPRES");
    add("hw.optional.arm.FEAT_CRC32");
    add("hw.optional.arm.FEAT_AES");
    add("hw.optional.arm.FEAT_SHA256");
    add("hw.optional.arm.FEAT_PMULL");
}

void compute_fingerprint(State& s, std::string& env_names) {
    const HostImage& img = host_image();
    std::string material = "vitreo-translation-store/1\n";
    material.append(reinterpret_cast<const char*>(img.uuid), 16);
    material += '\n';
    host_cpu_identity(material);
    const auto add_env = [&](const char* tag, const std::vector<std::string>& lines) {
        material += tag;
        for (const std::string& line : lines) {
            material += line;
            material += '\n';
            const std::string name = line.substr(0, line.find('='));
            if (env_names.find(name) == std::string::npos) {
                if (!env_names.empty()) env_names += ',';
                env_names += name;
            }
        }
    };
    add_env("env@start\n", g_env_at_start);
    add_env("env@jit\n", collect_vitreo_env());
    static constexpr u8 kFpKey0[16] = {'v', 'i', 't', 'r', 'u', 'm', '-', 't',
                                       's', 't', 'o', 'r', 'e', '-', 'f', '0'};
    static constexpr u8 kFpKey1[16] = {'v', 'i', 't', 'r', 'u', 'm', '-', 't',
                                       's', 't', 'o', 'r', 'e', '-', 'f', '1'};
    const u64 a = siphash24_impl(kFpKey0, reinterpret_cast<const u8*>(material.data()),
                                 material.size());
    const u64 b = siphash24_impl(kFpKey1, reinterpret_cast<const u8*>(material.data()),
                                 material.size());
    std::memcpy(s.fingerprint, &a, 8);
    std::memcpy(s.fingerprint + 8, &b, 8);
    s.fingerprint_hex = hex(s.fingerprint, 16);
}

bool load_or_create_key(State& s) {
    const std::string path = s.root + "/key";
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            struct stat sb{};
            const bool ok = ::fstat(fd, &sb) == 0 && sb.st_size == 16 &&
                            sb.st_uid == ::getuid() && read_exact(fd, s.mac_key, 16);
            ::close(fd);
            if (!ok) {
                std::fprintf(stderr, "[tstore] key file %s is not a 16-byte file of this user\n",
                             path.c_str());
            }
            return ok;
        }
        u8 fresh[16];
        arc4random_buf(fresh, sizeof fresh);
        char tmp[64];
        std::snprintf(tmp, sizeof tmp, "/key.tmp.%d.%08x", static_cast<int>(::getpid()),
                      arc4random());
        const std::string tmp_path = s.root + tmp;
        const int out = ::open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (out < 0) return false;
        (void)::fchmod(out, 0600);
        const bool written = write_exact(out, fresh, sizeof fresh) && ::fsync(out) == 0;
        ::close(out);
        // link() never replaces: a racing creator's key wins and is re-read.
        if (written) (void)::link(tmp_path.c_str(), path.c_str());
        ::unlink(tmp_path.c_str());
        if (!written) return false;
    }
    return false;
}

struct DirEntryInfo {
    std::string path;
    std::string name;
    u64 size{0};
    i64 mtime{0};
    bool is_dir{false};
};

std::vector<DirEntryInfo> list_dir(const std::string& dir) {
    std::vector<DirEntryInfo> out;
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) return out;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        DirEntryInfo info;
        info.name = e->d_name;
        info.path = dir + "/" + info.name;
        struct stat sb{};
        if (::lstat(info.path.c_str(), &sb) != 0) continue;
        info.size = static_cast<u64>(sb.st_size);
        info.mtime = static_cast<i64>(sb.st_mtimespec.tv_sec);
        info.is_dir = S_ISDIR(sb.st_mode);
        out.push_back(std::move(info));
    }
    ::closedir(d);
    return out;
}

bool is_segment_name(const std::string& n) {
    return n.starts_with("seg-") && n.ends_with(".tss");
}

bool is_fingerprint_name(const std::string& n) {
    if (n.size() != 32) return false;
    return std::all_of(n.begin(), n.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

/// Eviction (design §3b, bounded): keep this fingerprint's directory under
/// its cap by deleting the least recently used segments no live process is
/// writing, and keep at most one directory of another (dead) fingerprint.
void collect_garbage(State& s) {
    const std::string gc_path = s.root + "/gc.lock";
    const int gc = ::open(gc_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (gc < 0) return;
    if (::flock(gc, LOCK_EX | LOCK_NB) != 0) {
        ::close(gc);
        return;
    }
    std::vector<DirEntryInfo> dead;
    for (DirEntryInfo& e : list_dir(s.root)) {
        if (!e.is_dir || !is_fingerprint_name(e.name) || e.name == s.fingerprint_hex) continue;
        const int lock = ::open((e.path + "/live.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC,
                                0600);
        if (lock < 0) continue;
        if (::flock(lock, LOCK_EX | LOCK_NB) == 0) dead.push_back(e);
        ::close(lock);
    }
    std::sort(dead.begin(), dead.end(),
              [](const DirEntryInfo& a, const DirEntryInfo& b) { return a.mtime > b.mtime; });
    for (usize i = 1; i < dead.size(); ++i) {
        std::error_code ec;
        std::filesystem::remove_all(dead[i].path, ec);
        std::fprintf(stderr, "[tstore] evicted store of another build: %s\n",
                     dead[i].name.c_str());
    }

    std::vector<DirEntryInfo> segs;
    u64 total = 0;
    for (DirEntryInfo& e : list_dir(s.dir)) {
        if (e.is_dir || !is_segment_name(e.name)) continue;
        total += e.size;
        segs.push_back(std::move(e));
    }
    if (total > s.cap_bytes) {
        std::sort(segs.begin(), segs.end(),
                  [](const DirEntryInfo& a, const DirEntryInfo& b) { return a.mtime < b.mtime; });
        const u64 target = s.cap_bytes / 4 * 3;
        u64 evicted = 0;
        for (const DirEntryInfo& e : segs) {
            if (total <= target) break;
            const int fd = ::open(e.path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
                ::unlink(e.path.c_str());
                total -= e.size;
                evicted += e.size;
            }
            ::close(fd);
        }
        std::fprintf(stderr, "[tstore] evicted %llu MB of least recently used segments\n",
                     static_cast<unsigned long long>(evicted >> 20));
    }
    // A compaction that died before its rename left a temporary no one
    // holds locked.
    for (const DirEntryInfo& e : list_dir(s.dir)) {
        if (e.is_dir || !e.name.starts_with("compact-") || !e.name.ends_with(".tmp")) continue;
        const int fd = ::open(e.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) ::unlink(e.path.c_str());
        ::close(fd);
    }
    if (!s.poisoned.load(std::memory_order_acquire)) compact_segments(s);
    total = 0;
    for (const DirEntryInfo& e : list_dir(s.dir)) {
        if (!e.is_dir && is_segment_name(e.name)) total += e.size;
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.existing_bytes = total;
    }
    ::close(gc);
}

void build_index(State& s) {
    const auto t0 = std::chrono::steady_clock::now();
    u64 records = 0;
    u64 bytes = 0;
    u64 torn = 0;
    u64 foreign = 0;
    struct Found {
        u64 tag;
        const u8* rec;
        const Segment* seg;
    };
    std::vector<Found> found;
    std::string own_segment;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        own_segment = s.seg_path;
    }
    u64 dir_bytes = 0;
    for (const DirEntryInfo& e : list_dir(s.dir)) {
        if (e.is_dir || !is_segment_name(e.name) || e.path == own_segment) continue;
        dir_bytes += e.size;
        if (e.size < sizeof(SegmentHeader)) continue;
        const int fd = ::open(e.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        struct stat sb{};
        if (::fstat(fd, &sb) != 0 || sb.st_size < static_cast<off_t>(sizeof(SegmentHeader))) {
            ::close(fd);
            continue;
        }
        const usize size = static_cast<usize>(sb.st_size);
        void* map = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            ::close(fd);
            continue;
        }
        const auto* base = static_cast<const u8*>(map);
        SegmentHeader sh;
        std::memcpy(&sh, base, sizeof sh);
        const bool header_ok =
            std::memcmp(sh.magic, kSegmentMagic, 8) == 0 && sh.version == kFormatVersion &&
            sh.header_len == sizeof(SegmentHeader) &&
            std::memcmp(sh.fingerprint, s.fingerprint, 16) == 0 &&
            sh.mac == siphash24_impl(s.mac_key, base, offsetof(SegmentHeader, mac));
        if (!header_ok) {
            ++foreign;
            ::munmap(map, size);
            ::close(fd);
            continue;
        }
        auto seg = std::make_unique<Segment>();
        seg->fd = fd;
        seg->base = base;
        seg->size = size;
        seg->producer_base = sh.producer_host_base;
        seg->path = e.path;
        usize off = sizeof(SegmentHeader);
        while (off + sizeof(RecordHeader) <= size) {
            RecordHeader h;
            std::memcpy(&h, base + off, sizeof h);
            const usize need = sizeof(RecordHeader) + pad8(h.key.guest_size) + pad8(h.payload_len);
            if (h.magic != kRecordMagic || h.total_len != need || h.total_len > kMaxRecordBytes ||
                off + h.total_len > size || h.header_check != record_header_check(h) ||
                h.tag != key_tag(h.key)) {
                ++torn;
                break;
            }
            found.push_back(Found{h.tag, base + off, seg.get()});
            ++records;
            bytes += h.total_len;
            off += h.total_len;
        }
        s.segments.push_back(std::move(seg));
    }
    auto* index = new Index();
    usize slots = 1024;
    while (slots < found.size() * 2) slots <<= 1;
    index->slots.resize(slots);
    index->mask = slots - 1;
    index->records = found.size();
    {
        // The cap holds before eviction has measured the directory.
        std::lock_guard<std::mutex> lock(s.mutex);
        s.existing_bytes = dir_bytes;
    }
    for (const Found& f : found) {
        usize i = static_cast<usize>(f.tag) & index->mask;
        while (index->slots[i].rec != nullptr) i = (i + 1) & index->mask;
        index->slots[i] = Index::Slot{f.tag, f.rec, f.seg};
    }
    s.index.store(index, std::memory_order_release);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::fprintf(stderr,
                 "[tstore] index pid=%d segments=%zu records=%llu bytes=%lluMB torn_tails=%llu "
                 "foreign_segments=%llu ms=%lld\n",
                 static_cast<int>(::getpid()), s.segments.size(),
                 static_cast<unsigned long long>(records),
                 static_cast<unsigned long long>(bytes >> 20),
                 static_cast<unsigned long long>(torn),
                 static_cast<unsigned long long>(foreign), static_cast<long long>(ms));
}

bool open_segment_locked(State& s) {
    char name[96];
    std::snprintf(name, sizeof name, "/seg-%llu-%d-%08x.tss",
                  static_cast<unsigned long long>(realtime_ms()), static_cast<int>(::getpid()),
                  arc4random());
    s.seg_path = s.dir + name;
    const int fd = ::open(s.seg_path.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    (void)::fchmod(fd, 0600);
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return false;
    }
    SegmentHeader sh{};
    std::memcpy(sh.magic, kSegmentMagic, 8);
    sh.version = kFormatVersion;
    sh.header_len = sizeof(SegmentHeader);
    std::memcpy(sh.fingerprint, s.fingerprint, 16);
    sh.producer_host_base = host_image().base;
    sh.created_ms = realtime_ms();
    sh.pid = static_cast<u32>(::getpid());
    sh.mac = siphash24_impl(s.mac_key, reinterpret_cast<const u8*>(&sh),
                            offsetof(SegmentHeader, mac));
    if (!write_exact(fd, &sh, sizeof sh)) {
        ::close(fd);
        return false;
    }
    s.seg_fd = fd;
    s.written += sizeof sh;
    return true;
}

/// Write buffered records. Records offered before the index was ready are
/// dropped when the index holds an authentic copy; with no index yet they
/// wait, unless @p final (process end), when they are written and a later
/// compaction removes any duplicate.
void flush_locked(State& s, bool final) {
    if (!s.unchecked.empty()) {
        const Index* index = s.index.load(std::memory_order_acquire);
        if (index != nullptr || final) {
            for (const std::vector<u8>& rec : s.unchecked) {
                RecordHeader h;
                std::memcpy(&h, rec.data(), sizeof h);
                if (index != nullptr &&
                    find_valid_record(s, *index, h.key, rec.data() + sizeof h) != nullptr) {
                    tstats::note(tstats::Event::SkipDuplicate);
                    continue;
                }
                s.buffer.insert(s.buffer.end(), rec.begin(), rec.end());
            }
            s.unchecked.clear();
            s.unchecked_bytes = 0;
        }
    }
    if (s.buffer.empty() || s.producer_off.load(std::memory_order_relaxed)) {
        s.buffer.clear();
        return;
    }
    if (s.seg_fd < 0 && !open_segment_locked(s)) {
        std::fprintf(stderr, "[tstore] cannot create a segment in %s (%s); not storing\n",
                     s.dir.c_str(), std::strerror(errno));
        s.producer_off.store(true, std::memory_order_relaxed);
        s.buffer.clear();
        return;
    }
    if (!write_exact(s.seg_fd, s.buffer.data(), s.buffer.size())) {
        std::fprintf(stderr, "[tstore] segment write failed (%s); not storing\n",
                     std::strerror(errno));
        s.producer_off.store(true, std::memory_order_relaxed);
    }
    s.written += s.buffer.size();
    s.buffer.clear();
}

void check_poison(State& s) {
    if (s.poisoned.load(std::memory_order_relaxed)) return;
    struct stat sb{};
    if (::stat((s.dir + "/poison").c_str(), &sb) == 0) {
        s.poisoned.store(true, std::memory_order_release);
        std::fprintf(stderr,
                     "[tstore] POISONED: %s/poison exists (a shadow verification mismatch); "
                     "no stored translation is adopted or written\n",
                     s.dir.c_str());
    }
}

void worker_main(State* s) {
    if (const u32 delay = g_index_delay_ms_for_test.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    }
    // Index first: the activating compile waits for it. Eviction and
    // compaction only unlink files; segments mapped here stay readable.
    build_index(*s);
    {
        std::lock_guard<std::mutex> lock(s->index_mutex);
        s->index_done.store(true, std::memory_order_release);
    }
    s->index_cv.notify_all();
    collect_garbage(*s);
    s->gc_done.store(true, std::memory_order_release);
    while (!s->stop.load(std::memory_order_acquire)) {
        for (int i = 0; i < 10 && !s->stop.load(std::memory_order_acquire); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            flush_locked(*s, false);
        }
        check_poison(*s);
    }
}

bool activate() {
    State& s = *g_state;
    if (translation_store_mode() != TranslationStoreMode::Warm) return false;
    const HostImage& img = host_image();
    if (!img.ok) {
        std::fprintf(stderr, "[tstore] inactive: host image identity unavailable\n");
        return false;
    }
    s.root = choose_root();
    if (s.root.empty()) {
        std::fprintf(stderr,
                     "[tstore] inactive: no capsule (VITREO_IPC_SCOPE unset) and no "
                     "VITREO_TRANSLATION_STORE_DIR\n");
        return false;
    }
    if (const char* scope = std::getenv("VITREO_IPC_SCOPE"); scope != nullptr && scope[0] != '\0') {
        const std::string drives = std::string(scope) + "/drives";
        if (s.root.starts_with(drives)) {
            std::fprintf(stderr,
                         "[tstore] inactive: %s is inside the capsule's guest drives\n",
                         s.root.c_str());
            return false;
        }
    }
    {
        std::error_code ec;
        const std::filesystem::path parent = std::filesystem::path(s.root).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    }
    if (!make_private_dir(s.root) || !load_or_create_key(s)) {
        std::fprintf(stderr, "[tstore] inactive: cannot own %s\n", s.root.c_str());
        return false;
    }
    std::string env_names;
    compute_fingerprint(s, env_names);
    s.dir = s.root + "/" + s.fingerprint_hex;
    if (!make_private_dir(s.dir)) {
        std::fprintf(stderr, "[tstore] inactive: cannot own %s\n", s.dir.c_str());
        return false;
    }
    s.live_fd = ::open((s.dir + "/live.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (s.live_fd < 0 || ::flock(s.live_fd, LOCK_SH) != 0) {
        std::fprintf(stderr, "[tstore] inactive: cannot lock %s/live.lock\n", s.dir.c_str());
        return false;
    }
    u64 cap_mb = 2048;
    if (const char* v = std::getenv("VITREO_TRANSLATION_STORE_MAX_MB"); v != nullptr && *v) {
        char* end = nullptr;
        const unsigned long long n = std::strtoull(v, &end, 10);
        if (end != v && *end == '\0' && n > 0) cap_mb = n;
    }
    s.cap_bytes = cap_mb << 20;
    if (const char* v = std::getenv("VITREO_TRANSLATION_STORE_VERIFY"); v != nullptr && *v) {
        if (std::strcmp(v, "all") == 0) {
            s.verify_every = 1;
        } else {
            char* end = nullptr;
            const unsigned long n = std::strtoul(v, &end, 10);
            if (end != v && *end == '\0' && n >= 1 && n <= (1u << 20)) {
                s.verify_every = static_cast<u32>(n);
            } else {
                std::fprintf(stderr,
                             "[tstore] VITREO_TRANSLATION_STORE_VERIFY=%s IGNORED: expected "
                             "all or 1..1048576\n",
                             v);
            }
        }
    }
    s.compact_min = env_u32("VITREO_TRANSLATION_STORE_COMPACT_MIN", kDefaultCompactMin, 2, 1u << 16);
    s.index_wait_ms =
        env_u32("VITREO_TRANSLATION_STORE_INDEX_WAIT_MS", kDefaultIndexWaitMs, 0, 10000);
    check_poison(s);
    std::fprintf(stderr,
                 "[tstore] active pid=%d dir=%s cap=%lluMB verify=1/%u host_base=0x%llx "
                 "env=[%s]\n",
                 static_cast<int>(::getpid()), s.dir.c_str(),
                 static_cast<unsigned long long>(cap_mb), s.verify_every,
                 static_cast<unsigned long long>(img.base), env_names.c_str());
    s.stop.store(false, std::memory_order_relaxed);
    s.worker = std::thread(worker_main, &s);
    tstats::set_shutdown_hook([] { TranslationStore::flush(); });
    {
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_lock<std::mutex> lock(s.index_mutex);
        const bool ready = s.index_cv.wait_for(
            lock, std::chrono::milliseconds(s.index_wait_ms),
            [&] { return s.index_done.load(std::memory_order_acquire); });
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        std::fprintf(stderr, "[tstore] index %s after %lld us (budget %u ms)\n",
                     ready ? "ready" : "NOT ready", static_cast<long long>(us),
                     s.index_wait_ms);
    }
    return true;
}

/// Re-encode every relocation from the image at @p producer_base to the same
/// image at @p target_base (this process's slide, or a compacted segment's).
bool relocate(StoredEmission& e, u64 producer_base, u64 target_base) noexcept {
    const HostImage& img = host_image();
    for (StoredHostReloc& r : e.relocs) {
        if (r.words == 0 || r.words > 4 || r.reg > 30 ||
            static_cast<usize>(r.word) + r.words > e.words.size()) {
            return false;
        }
        u32 tmp[4];
        if (encode_mov_imm64(r.reg, r.value, tmp) != r.words ||
            std::memcmp(tmp, e.words.data() + r.word, r.words * sizeof(u32)) != 0) {
            return false;
        }
        if (r.value < producer_base || r.value - producer_base >= img.hi - img.base) {
            return false;
        }
        const u64 moved = target_base + (r.value - producer_base);
        if (encode_mov_imm64(r.reg, moved, tmp) != r.words) return false;
        std::memcpy(e.words.data() + r.word, tmp, r.words * sizeof(u32));
        r.value = moved;
    }
    return true;
}

/// A MOVZ/MOVN X run starting at @p i: its register, value and length.
bool mov_run_at(const std::vector<u32>& w, usize i, u8& reg, u64& value, usize& len) noexcept {
    const u32 first = w[i];
    const bool movz = (first & 0xFF800000u) == 0xD2800000u;
    const bool movn = (first & 0xFF800000u) == 0x92800000u;
    if (!movz && !movn) return false;
    reg = static_cast<u8>(first & 31u);
    const u32 shift = ((first >> 21) & 3u) * 16u;
    const u64 imm = static_cast<u64>((first >> 5) & 0xFFFFu) << shift;
    value = movz ? imm : ~imm;
    usize j = i + 1;
    while (j < w.size() && (w[j] & 0xFF800000u) == 0xF2800000u && (w[j] & 31u) == reg) {
        const u32 s = ((w[j] >> 21) & 3u) * 16u;
        value = (value & ~(u64{0xFFFF} << s)) |
                (static_cast<u64>((w[j] >> 5) & 0xFFFFu) << s);
        ++j;
    }
    len = j - i;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// StoredEmission
// ---------------------------------------------------------------------------

void StoredEmission::clear() noexcept {
    words.clear();
    insn_arm_pos.clear();
    insn_arm_end.clear();
    insn_rips.clear();
    insn_flag_recovery.clear();
    insn_rsp_undo.clear();
    stencil_flag_recovery.clear();
    lean_ranges.clear();
    cold_lazy.clear();
    links.clear();
    relocs.clear();
    process_stubs = 0;
    mem_helpers.clear();
    hot_end_pos = link_entry_pos = call_entry_pos = insn_pos_count = 0;
    raise_entry_plain_pos = 0;
    raise_entry_restore_pos.fill(0);
    pin_arm.fill(0);
    pin_layout = 0;
    native_insns = 0;
    terminal_opcode = 0;
    stack_entry_load_proof = false;
    instruction_count = 0;
    ends_with_branch = false;
}

namespace {

// Every struct serialized field by field: a new field fails these asserts
// until it is added to put/get and to the equality below.
static_assert(sizeof(JitFlagRecovery) == 32, "serialize the new JitFlagRecovery field");
static_assert(sizeof(StencilFlagRecovery) == 64, "serialize the new StencilFlagRecovery field");
static_assert(sizeof(JitCompileMemo::DirectLinkMeta) == 16, "serialize the new DirectLinkMeta field");
static_assert(sizeof(StoredEmission::LeanRange) == 24);
static_assert(sizeof(StoredEmission::ColdLazy) == 48);
static_assert(sizeof(StoredMemHelper) == 8, "serialize the new StoredMemHelper field");

bool flag_recovery_equal(const JitFlagRecovery& a, const JitFlagRecovery& b) noexcept {
    return a.flag_op == b.flag_op && a.src_reg == b.src_reg && a.dst_reg == b.dst_reg &&
           a.width == b.width && a.mode == b.mode && a.src_imm == b.src_imm &&
           a.dst_imm == b.dst_imm && a.rsp_undo == b.rsp_undo;
}

bool stencil_recovery_equal(const StencilFlagRecovery& a, const StencilFlagRecovery& b) noexcept {
    return a.flag_op == b.flag_op && a.recompute_dst == b.recompute_dst &&
           a.src_is_imm == b.src_is_imm && a.src_reg == b.src_reg && a.src_imm == b.src_imm &&
           a.dst_is_imm == b.dst_is_imm && a.dst_reg == b.dst_reg && a.dst_imm == b.dst_imm &&
           a.def_rip == b.def_rip && a.first_rip == b.first_rip && a.last_rip == b.last_rip &&
           a.width == b.width && a.merge_cf == b.merge_cf;
}

struct Writer {
    std::vector<u8>& out;
    template <typename T>
    void put(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const usize at = out.size();
        out.resize(at + sizeof v);
        std::memcpy(out.data() + at, &v, sizeof v);
    }
    void put_bool(bool v) { put<u8>(v ? 1 : 0); }
};

struct Reader {
    const u8* p;
    const u8* end;
    bool ok{true};
    template <typename T>
    T get() {
        T v{};
        if (static_cast<usize>(end - p) < sizeof v) {
            ok = false;
            return v;
        }
        std::memcpy(&v, p, sizeof v);
        p += sizeof v;
        return v;
    }
    bool get_bool() {
        const u8 v = get<u8>();
        if (v > 1) ok = false;
        return v != 0;
    }
    u32 count(usize max) {
        const u32 n = get<u32>();
        if (n > max) ok = false;
        return ok ? n : 0;
    }
};

constexpr usize kMaxWords = 1u << 16;
constexpr usize kMaxInsnSites = 4096;
constexpr usize kMaxSmall = 1u << 14;

}  // namespace

void encode_stored_emission(const StoredEmission& e, std::vector<u8>& out) {
    Writer w{out};
    w.put<u32>(static_cast<u32>(e.words.size()));
    for (u32 v : e.words) w.put(v);
    w.put(e.hot_end_pos);
    w.put(e.link_entry_pos);
    w.put(e.call_entry_pos);
    w.put(e.insn_pos_count);
    for (u32 i = 0; i < e.insn_pos_count; ++i) {
        w.put(e.insn_arm_pos[i]);
        w.put(e.insn_arm_end[i]);
        w.put(e.insn_rips[i]);
        const JitFlagRecovery& f = e.insn_flag_recovery[i];
        w.put(f.flag_op);
        w.put(f.src_reg);
        w.put(f.dst_reg);
        w.put(f.width);
        w.put(f.mode);
        w.put(f.src_imm);
        w.put(f.dst_imm);
        w.put(f.rsp_undo);
        w.put(e.insn_rsp_undo[i]);
    }
    w.put<u32>(static_cast<u32>(e.stencil_flag_recovery.size()));
    for (const StencilFlagRecovery& p : e.stencil_flag_recovery) {
        w.put(p.flag_op);
        w.put_bool(p.recompute_dst);
        w.put_bool(p.src_is_imm);
        w.put(p.src_reg);
        w.put(p.src_imm);
        w.put_bool(p.dst_is_imm);
        w.put(p.dst_reg);
        w.put(p.dst_imm);
        w.put(p.def_rip);
        w.put(p.first_rip);
        w.put(p.last_rip);
        w.put(p.width);
        w.put_bool(p.merge_cf);
    }
    w.put<u32>(static_cast<u32>(e.lean_ranges.size()));
    for (const auto& r : e.lean_ranges) {
        w.put(r.begin);
        w.put(r.end);
        w.put(r.owner);
    }
    w.put<u32>(static_cast<u32>(e.cold_lazy.size()));
    for (const auto& c : e.cold_lazy) {
        w.put(c.slot_pos);
        w.put(c.return_pos);
        w.put(c.mixed_pos);
        w.put(c.guard_pos);
        w.put(c.raise_kind);
        w.put(c.packed);
        w.put(c.config);
    }
    w.put(e.raise_entry_plain_pos);
    for (u64 v : e.raise_entry_restore_pos) w.put(v);
    w.put<u32>(static_cast<u32>(e.links.size()));
    for (const auto& l : e.links) {
        w.put(l.patch_offset);
        w.put(l.gen_offset);
        w.put(l.target);
    }
    for (u8 v : e.pin_arm) w.put(v);
    w.put(e.pin_layout);
    w.put(e.native_insns);
    w.put(e.terminal_opcode);
    w.put_bool(e.stack_entry_load_proof);
    w.put(e.instruction_count);
    w.put_bool(e.ends_with_branch);
    w.put<u32>(static_cast<u32>(e.relocs.size()));
    for (const auto& r : e.relocs) {
        w.put(r.word);
        w.put(r.words);
        w.put(r.reg);
        w.put(r.value);
    }
    w.put(e.process_stubs);
    w.put<u32>(static_cast<u32>(e.mem_helpers.size()));
    for (const auto& m : e.mem_helpers) {
        w.put(m.helper_off);
        w.put(m.width);
        w.put(m.is_store);
    }
}

bool decode_stored_emission(const u8* data, usize size, StoredEmission& e) {
    e.clear();
    Reader r{data, data + size};
    const u32 nwords = r.count(kMaxWords);
    e.words.resize(nwords);
    for (u32 i = 0; i < nwords && r.ok; ++i) e.words[i] = r.get<u32>();
    e.hot_end_pos = r.get<u32>();
    e.link_entry_pos = r.get<u32>();
    e.call_entry_pos = r.get<u32>();
    e.insn_pos_count = r.count(kMaxInsnSites);
    const u32 n = e.insn_pos_count;
    e.insn_arm_pos.resize(n);
    e.insn_arm_end.resize(n);
    e.insn_rips.resize(n);
    e.insn_flag_recovery.resize(n);
    e.insn_rsp_undo.resize(n);
    for (u32 i = 0; i < n && r.ok; ++i) {
        e.insn_arm_pos[i] = r.get<u32>();
        e.insn_arm_end[i] = r.get<u32>();
        e.insn_rips[i] = r.get<u64>();
        JitFlagRecovery& f = e.insn_flag_recovery[i];
        f.flag_op = r.get<u8>();
        f.src_reg = r.get<u8>();
        f.dst_reg = r.get<u8>();
        f.width = r.get<u8>();
        f.mode = r.get<u8>();
        f.src_imm = r.get<u64>();
        f.dst_imm = r.get<u64>();
        f.rsp_undo = r.get<i8>();
        e.insn_rsp_undo[i] = r.get<i8>();
    }
    const u32 nplans = r.count(kMaxSmall);
    e.stencil_flag_recovery.resize(nplans);
    for (u32 i = 0; i < nplans && r.ok; ++i) {
        StencilFlagRecovery& p = e.stencil_flag_recovery[i];
        p.flag_op = r.get<u8>();
        p.recompute_dst = r.get_bool();
        p.src_is_imm = r.get_bool();
        p.src_reg = r.get<u8>();
        p.src_imm = r.get<u64>();
        p.dst_is_imm = r.get_bool();
        p.dst_reg = r.get<u8>();
        p.dst_imm = r.get<u64>();
        p.def_rip = r.get<u64>();
        p.first_rip = r.get<u64>();
        p.last_rip = r.get<u64>();
        p.width = r.get<u8>();
        p.merge_cf = r.get_bool();
    }
    const u32 nlean = r.count(kMaxSmall);
    e.lean_ranges.resize(nlean);
    for (u32 i = 0; i < nlean && r.ok; ++i) {
        e.lean_ranges[i].begin = r.get<u64>();
        e.lean_ranges[i].end = r.get<u64>();
        e.lean_ranges[i].owner = r.get<u32>();
    }
    const u32 nlazy = r.count(kMaxSmall);
    e.cold_lazy.resize(nlazy);
    for (u32 i = 0; i < nlazy && r.ok; ++i) {
        auto& c = e.cold_lazy[i];
        c.slot_pos = r.get<u64>();
        c.return_pos = r.get<u64>();
        c.mixed_pos = r.get<u64>();
        c.guard_pos = r.get<u64>();
        c.raise_kind = r.get<u8>();
        c.packed = r.get<u32>();
        c.config = r.get<u32>();
    }
    e.raise_entry_plain_pos = r.get<u64>();
    for (u64& v : e.raise_entry_restore_pos) v = r.get<u64>();
    const u32 nlinks = r.count(kMaxSmall);
    e.links.resize(nlinks);
    for (u32 i = 0; i < nlinks && r.ok; ++i) {
        e.links[i].patch_offset = r.get<u32>();
        e.links[i].gen_offset = r.get<u32>();
        e.links[i].target = r.get<u64>();
    }
    for (u8& v : e.pin_arm) v = r.get<u8>();
    e.pin_layout = r.get<u64>();
    e.native_insns = r.get<u32>();
    e.terminal_opcode = r.get<u16>();
    e.stack_entry_load_proof = r.get_bool();
    e.instruction_count = r.get<u32>();
    e.ends_with_branch = r.get_bool();
    const u32 nrelocs = r.count(kMaxSmall);
    e.relocs.resize(nrelocs);
    for (u32 i = 0; i < nrelocs && r.ok; ++i) {
        e.relocs[i].word = r.get<u32>();
        e.relocs[i].words = r.get<u8>();
        e.relocs[i].reg = r.get<u8>();
        e.relocs[i].value = r.get<u64>();
    }
    e.process_stubs = r.get<u32>();
    const u32 nhelpers = r.count(kMaxSmall);
    e.mem_helpers.resize(nhelpers);
    for (u32 i = 0; i < nhelpers && r.ok; ++i) {
        e.mem_helpers[i].helper_off = r.get<u32>();
        e.mem_helpers[i].width = r.get<u8>();
        e.mem_helpers[i].is_store = r.get<u8>();
    }
    return r.ok && r.p == r.end;
}

bool stored_emission_equal(const StoredEmission& a, const StoredEmission& b) noexcept {
    if (a.words != b.words || a.hot_end_pos != b.hot_end_pos ||
        a.link_entry_pos != b.link_entry_pos || a.call_entry_pos != b.call_entry_pos ||
        a.insn_pos_count != b.insn_pos_count || a.insn_arm_pos != b.insn_arm_pos ||
        a.insn_arm_end != b.insn_arm_end || a.insn_rips != b.insn_rips ||
        a.insn_rsp_undo != b.insn_rsp_undo || a.lean_ranges != b.lean_ranges ||
        a.cold_lazy != b.cold_lazy || a.raise_entry_plain_pos != b.raise_entry_plain_pos ||
        a.raise_entry_restore_pos != b.raise_entry_restore_pos || a.pin_arm != b.pin_arm ||
        a.pin_layout != b.pin_layout || a.native_insns != b.native_insns ||
        a.terminal_opcode != b.terminal_opcode ||
        a.stack_entry_load_proof != b.stack_entry_load_proof ||
        a.instruction_count != b.instruction_count ||
        a.ends_with_branch != b.ends_with_branch || a.relocs != b.relocs ||
        a.process_stubs != b.process_stubs || a.mem_helpers != b.mem_helpers ||
        a.insn_flag_recovery.size() != b.insn_flag_recovery.size() ||
        a.stencil_flag_recovery.size() != b.stencil_flag_recovery.size() ||
        a.links.size() != b.links.size()) {
        return false;
    }
    for (usize i = 0; i < a.insn_flag_recovery.size(); ++i) {
        if (!flag_recovery_equal(a.insn_flag_recovery[i], b.insn_flag_recovery[i])) return false;
    }
    for (usize i = 0; i < a.stencil_flag_recovery.size(); ++i) {
        if (!stencil_recovery_equal(a.stencil_flag_recovery[i], b.stencil_flag_recovery[i])) {
            return false;
        }
    }
    for (usize i = 0; i < a.links.size(); ++i) {
        if (a.links[i].patch_offset != b.links[i].patch_offset ||
            a.links[i].gen_offset != b.links[i].gen_offset ||
            a.links[i].target != b.links[i].target) {
            return false;
        }
    }
    return true;
}

bool stored_emission_well_formed(const StoredEmission& e) noexcept {
    constexpr u64 npos = ~u64{0};
    const usize n = e.words.size();
    const u64 bytes = static_cast<u64>(n) * sizeof(u32);
    if (n == 0 || e.hot_end_pos > n || e.link_entry_pos >= n) return false;
    if (e.call_entry_pos != ~u32{0} && e.call_entry_pos >= n) return false;
    if (e.insn_pos_count > 192 || e.insn_arm_pos.size() != e.insn_pos_count ||
        e.insn_arm_end.size() != e.insn_pos_count || e.insn_rips.size() != e.insn_pos_count ||
        e.insn_flag_recovery.size() != e.insn_pos_count ||
        e.insn_rsp_undo.size() != e.insn_pos_count) {
        return false;
    }
    for (u32 i = 0; i < e.insn_pos_count; ++i) {
        if (e.insn_arm_pos[i] > n || e.insn_arm_end[i] > n) return false;
    }
    for (const auto& r : e.lean_ranges) {
        if (r.begin > n || r.end > n || r.owner >= e.insn_pos_count) return false;
    }
    for (const auto& c : e.cold_lazy) {
        // The record occupies the four words after the slot.
        if (c.slot_pos + 5 > n || c.return_pos >= n || c.guard_pos >= n ||
            (c.mixed_pos != npos && c.mixed_pos >= n)) {
            return false;
        }
        const u64 raise = c.raise_kind == 0xFF ? e.raise_entry_plain_pos
                          : c.raise_kind < 5   ? e.raise_entry_restore_pos[c.raise_kind]
                                               : npos;
        if (raise == npos || raise >= n) return false;
    }
    for (const auto& l : e.links) {
        if (l.patch_offset % 4 != 0 || l.gen_offset % 4 != 0 || l.patch_offset >= bytes ||
            l.gen_offset >= bytes) {
            return false;
        }
    }
    for (const auto& r : e.relocs) {
        if (r.words == 0 || r.words > 4 || static_cast<u64>(r.word) + r.words > n) return false;
    }
    if ((e.process_stubs & ~stored_stub::kAll) != 0) return false;
    for (const auto& m : e.mem_helpers) {
        if ((m.width != 1 && m.width != 2 && m.width != 4 && m.width != 8) || m.is_store > 1) {
            return false;
        }
    }
    return true;
}

u32 encode_mov_imm64(u8 reg, u64 v, u32 out[4]) noexcept {
    // Arm ARM C6.2: MOVZ (sf=1) 0xD2800000, MOVN (sf=1) 0x92800000, MOVK
    // (sf=1) 0xF2800000; hw in bits 22:21, imm16 in bits 20:5, Rd in 4:0.
    const auto movz = [](u8 rd, u16 imm, u32 shift) {
        return 0xD2800000u | ((shift / 16) << 21) | (static_cast<u32>(imm) << 5) | rd;
    };
    const auto movn = [](u8 rd, u16 imm, u32 shift) {
        return 0x92800000u | ((shift / 16) << 21) | (static_cast<u32>(imm) << 5) | rd;
    };
    const auto movk = [](u8 rd, u16 imm, u32 shift) {
        return 0xF2800000u | ((shift / 16) << 21) | (static_cast<u32>(imm) << 5) | rd;
    };
    unsigned zero_lanes = 0, ones_lanes = 0;
    for (u32 shift = 0; shift < 64; shift += 16) {
        const u16 lane = static_cast<u16>(v >> shift);
        zero_lanes += lane == 0;
        ones_lanes += lane == 0xffff;
    }
    const bool inverted = ones_lanes > zero_lanes;
    const u16 fill = inverted ? u16{0xffff} : u16{0};
    u32 count = 0;
    for (u32 shift = 0; shift < 64; shift += 16) {
        const u16 lane = static_cast<u16>(v >> shift);
        if (lane == fill) continue;
        if (count != 0) {
            out[count++] = movk(reg, lane, shift);
        } else {
            out[count++] = inverted ? movn(reg, static_cast<u16>(~lane), shift)
                                    : movz(reg, lane, shift);
        }
    }
    if (count == 0) out[count++] = inverted ? movn(reg, 0, 0) : movz(reg, 0, 0);
    return count;
}

// ---------------------------------------------------------------------------
// Compaction
// ---------------------------------------------------------------------------

namespace {

struct CompactSource {
    std::string path;
    int fd{-1};
    const u8* base{nullptr};
    usize size{0};
    i64 mtime{0};
};

/// Write @p body (records) as a new segment of this process's host base:
/// a locked temporary, fsync, then rename into the segment namespace so a
/// reader never sees a partial segment.
bool write_compacted_segment(State& s, const std::vector<u8>& body) {
    char name[96];
    std::snprintf(name, sizeof name, "/compact-%d-%08x.tmp", static_cast<int>(::getpid()),
                  arc4random());
    const std::string tmp = s.dir + name;
    const int fd =
        ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    bool ok = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
    SegmentHeader sh{};
    std::memcpy(sh.magic, kSegmentMagic, 8);
    sh.version = kFormatVersion;
    sh.header_len = sizeof(SegmentHeader);
    std::memcpy(sh.fingerprint, s.fingerprint, 16);
    sh.producer_host_base = host_image().base;
    sh.created_ms = realtime_ms();
    sh.pid = static_cast<u32>(::getpid());
    sh.mac = siphash24_impl(s.mac_key, reinterpret_cast<const u8*>(&sh),
                            offsetof(SegmentHeader, mac));
    ok = ok && write_exact(fd, &sh, sizeof sh) && write_exact(fd, body.data(), body.size()) &&
         ::fsync(fd) == 0;
    std::snprintf(name, sizeof name, "/seg-%llu-%d-%08x.tss",
                  static_cast<unsigned long long>(realtime_ms()), static_cast<int>(::getpid()),
                  arc4random());
    ok = ok && ::rename(tmp.c_str(), (s.dir + name).c_str()) == 0;
    if (!ok) ::unlink(tmp.c_str());
    ::close(fd);
    return ok;
}

}  // namespace

namespace {

/// Merge the dead segments smaller than kCompactFullBytes into as few
/// segments as possible, one copy of each (key, guest bytes): every record
/// re-authenticated and re-checked as lookup would, relocated to this
/// process's host base (a segment has one producer base), and re-MACed.
/// Runs under gc.lock; a source is only read and unlinked while this process
/// holds its flock, which a live writer never releases.
void compact_segments(State& s) {
    std::string own;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        own = s.seg_path;
    }
    std::vector<CompactSource> sources;
    for (DirEntryInfo& e : list_dir(s.dir)) {
        if (e.is_dir || !is_segment_name(e.name) || e.path == own ||
            e.size >= kCompactFullBytes || e.size < sizeof(SegmentHeader)) {
            continue;
        }
        const int fd = ::open(e.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            continue;
        }
        sources.push_back(CompactSource{std::move(e.path), fd, nullptr, 0, e.mtime});
    }
    const auto release = [&] {
        for (CompactSource& src : sources) {
            if (src.base != nullptr) ::munmap(const_cast<u8*>(src.base), src.size);
            ::close(src.fd);
        }
    };
    if (sources.size() < s.compact_min) {
        release();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    std::sort(sources.begin(), sources.end(),
              [](const CompactSource& a, const CompactSource& b) { return a.mtime < b.mtime; });
    const HostImage& img = host_image();
    std::unordered_map<u64, std::vector<const u8*>> kept;  // tag -> source records
    std::vector<u8> body;
    std::vector<u8> payload;
    StoredEmission e;
    u64 records = 0, duplicates = 0, dropped = 0, outputs = 0;
    bool ok = true;
    std::vector<const CompactSource*> merged;
    for (CompactSource& src : sources) {
        struct stat sb{};
        if (::fstat(src.fd, &sb) != 0 || sb.st_size < static_cast<off_t>(sizeof(SegmentHeader))) {
            continue;
        }
        src.size = static_cast<usize>(sb.st_size);
        void* map = ::mmap(nullptr, src.size, PROT_READ, MAP_SHARED, src.fd, 0);
        if (map == MAP_FAILED) continue;
        src.base = static_cast<const u8*>(map);
        SegmentHeader sh;
        std::memcpy(&sh, src.base, sizeof sh);
        if (std::memcmp(sh.magic, kSegmentMagic, 8) != 0 || sh.version != kFormatVersion ||
            sh.header_len != sizeof(SegmentHeader) ||
            std::memcmp(sh.fingerprint, s.fingerprint, 16) != 0 ||
            sh.mac != siphash24_impl(s.mac_key, src.base, offsetof(SegmentHeader, mac))) {
            continue;  // not ours to rewrite: left in place
        }
        merged.push_back(&src);
        usize off = sizeof(SegmentHeader);
        while (off + sizeof(RecordHeader) <= src.size) {
            const u8* rec = src.base + off;
            RecordHeader h;
            std::memcpy(&h, rec, sizeof h);
            const usize need =
                sizeof(RecordHeader) + pad8(h.key.guest_size) + pad8(h.payload_len);
            if (h.magic != kRecordMagic || h.total_len != need || h.total_len > kMaxRecordBytes ||
                off + h.total_len > src.size || h.header_check != record_header_check(h) ||
                h.tag != key_tag(h.key)) {
                break;  // torn tail
            }
            off += h.total_len;
            ++records;
            const u8* bytes = rec + sizeof(RecordHeader);
            if (h.mac != siphash24_impl(s.mac_key, rec + 16, h.total_len - 16)) {
                ++dropped;
                continue;
            }
            std::vector<const u8*>& same_tag = kept[h.tag];
            const bool dup = std::any_of(same_tag.begin(), same_tag.end(), [&](const u8* k) {
                RecordHeader kh;
                std::memcpy(&kh, k, sizeof kh);
                return std::memcmp(&kh.key, &h.key, sizeof h.key) == 0 &&
                       std::memcmp(k + sizeof(RecordHeader), bytes, h.key.guest_size) == 0;
            });
            if (dup) {
                ++duplicates;
                continue;
            }
            if (!decode_stored_emission(bytes + pad8(h.key.guest_size), h.payload_len, e) ||
                !stored_emission_well_formed(e) || !relocate(e, sh.producer_host_base, img.base)) {
                ++dropped;
                continue;
            }
            same_tag.push_back(rec);
            payload.clear();
            encode_stored_emission(e, payload);
            append_record(s.mac_key, h.key, bytes, payload, body);
            // A full output is never merged again.
            if (body.size() + sizeof(SegmentHeader) >= kCompactFullBytes) {
                ok = ok && write_compacted_segment(s, body);
                ++outputs;
                body.clear();
            }
        }
    }
    if (ok && !body.empty()) {
        ok = write_compacted_segment(s, body);
        ++outputs;
    }
    // Only after every output is in place: a crash before this leaves
    // duplicates, which the next compaction removes, never a lost record.
    if (ok) {
        for (const CompactSource* src : merged) ::unlink(src->path.c_str());
    }
    release();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::fprintf(stderr,
                 "[tstore] compacted %zu segments into %llu: records=%llu duplicates=%llu "
                 "dropped=%llu%s ms=%lld\n",
                 merged.size(), static_cast<unsigned long long>(outputs),
                 static_cast<unsigned long long>(records),
                 static_cast<unsigned long long>(duplicates),
                 static_cast<unsigned long long>(dropped), ok ? "" : " (FAILED, sources kept)",
                 static_cast<long long>(ms));
}

}  // namespace

// ---------------------------------------------------------------------------
// TranslationStore
// ---------------------------------------------------------------------------

bool TranslationStore::active() noexcept {
    int a = g_active.load(std::memory_order_acquire);
    if (a >= 0) return a == 1;
    std::lock_guard<std::mutex> lock(g_activation_mutex);
    a = g_active.load(std::memory_order_acquire);
    if (a < 0) {
        a = activate() ? 1 : 0;
        g_active.store(a, std::memory_order_release);
    }
    return a == 1;
}

bool TranslationStore::producing() noexcept {
    if (!active()) return false;
    State& s = *g_state;
    if (s.producer_off.load(std::memory_order_relaxed) ||
        s.poisoned.load(std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

bool TranslationStore::lookup(const StoredKey& key, const u8* guest_bytes,
                              StoredEmission& out, bool& verify) {
    verify = false;
    State& s = *g_state;
    tstats::note(tstats::Event::Lookup);
    if (s.poisoned.load(std::memory_order_acquire)) return false;
    const Index* index = s.index.load(std::memory_order_acquire);
    if (index == nullptr) {
        tstats::note(tstats::Event::MissIndexNotReady);
        return false;
    }
    const u64 tag = key_tag(key);
    for (usize i = static_cast<usize>(tag) & index->mask;; i = (i + 1) & index->mask) {
        const Index::Slot& slot = index->slots[i];
        if (slot.rec == nullptr) break;
        if (slot.tag != tag) continue;
        RecordHeader h;
        std::memcpy(&h, slot.rec, sizeof h);
        if (std::memcmp(&h.key, &key, sizeof key) != 0) continue;
        if (h.mac != siphash24_impl(s.mac_key, slot.rec + 16, h.total_len - 16)) {
            tstats::note(tstats::Event::RejectMac);
            continue;
        }
        const u8* bytes = slot.rec + sizeof(RecordHeader);
        if (std::memcmp(bytes, guest_bytes, key.guest_size) != 0) {
            tstats::note(tstats::Event::RejectBytes);
            continue;
        }
        const u8* payload = bytes + pad8(key.guest_size);
        if (!decode_stored_emission(payload, h.payload_len, out)) {
            tstats::note(tstats::Event::RejectDecode);
            continue;
        }
        if (!stored_emission_well_formed(out)) {
            tstats::note(tstats::Event::RejectShape);
            continue;
        }
        if (!relocate(out, slot.seg->producer_base, host_image().base)) {
            tstats::note(tstats::Event::RejectReloc);
            continue;
        }
        if (!slot.seg->touched.exchange(true, std::memory_order_relaxed)) {
            // Least-recently-used eviction reads segment mtimes.
            (void)::futimes(slot.seg->fd, nullptr);
        }
        const u64 n = s.hits.fetch_add(1, std::memory_order_relaxed);
        verify = (n % s.verify_every) == s.verify_every - 1;
        tstats::note(tstats::Event::Hit);
        return true;
    }
    tstats::note(tstats::Event::MissNoRecord);
    return false;
}

void TranslationStore::offer(const StoredKey& key, const u8* guest_bytes,
                             const StoredEmission& e) {
    if (!producing()) return;
    State& s = *g_state;
    const HostImage& img = host_image();
    for (const StoredHostReloc& r : e.relocs) {
        u32 tmp[4];
        if (r.words == 0 || r.words > 4 || static_cast<usize>(r.word) + r.words > e.words.size() ||
            r.value < img.lo || r.value >= img.hi ||
            encode_mov_imm64(r.reg, r.value, tmp) != r.words ||
            std::memcmp(tmp, e.words.data() + r.word, r.words * sizeof(u32)) != 0) {
            tstats::note(tstats::Event::SkipUnrelocatable);
            return;
        }
    }
    // Backstop for a future emitter that bakes a host address without
    // recording it: no MOV run outside the recorded ones may assemble an
    // address inside the host image.
    usize next_reloc = 0;
    for (usize i = 0; i < e.words.size();) {
        u8 reg = 0;
        u64 value = 0;
        usize len = 0;
        if (!mov_run_at(e.words, i, reg, value, len)) {
            ++i;
            continue;
        }
        while (next_reloc < e.relocs.size() && e.relocs[next_reloc].word < i) ++next_reloc;
        const bool recorded = next_reloc < e.relocs.size() && e.relocs[next_reloc].word == i;
        if (!recorded && value >= img.lo && value < img.hi) {
            static std::atomic<u32> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                Dl_info info{};
                const bool named = dladdr(reinterpret_cast<const void*>(value), &info) != 0 &&
                                   info.dli_sname != nullptr;
                std::fprintf(stderr,
                             "[tstore] not storing block 0x%llx: word %zu bakes host address "
                             "0x%llx (%s) without a relocation\n",
                             static_cast<unsigned long long>(key.start_addr), i,
                             static_cast<unsigned long long>(value),
                             named ? info.dli_sname : "?");
            }
            tstats::note(tstats::Event::SkipUnrecordedHostAddr);
            return;
        }
        i += len;
    }

    std::vector<u8> payload;
    payload.reserve(e.words.size() * 4 + 1024);
    encode_stored_emission(e, payload);
    const usize total = sizeof(RecordHeader) + pad8(key.guest_size) + pad8(payload.size());
    if (total > kMaxRecordBytes) return;

    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.existing_bytes + s.written + s.buffer.size() + s.unchecked_bytes + total >
        s.cap_bytes) {
        tstats::note(tstats::Event::SkipFull);
        return;
    }
    // A lookup that ran before the index was ready, or before another
    // process's records were in it, missed a record the store may hold.
    if (const Index* index = s.index.load(std::memory_order_acquire); index != nullptr) {
        if (find_valid_record(s, *index, key, guest_bytes) != nullptr) {
            tstats::note(tstats::Event::SkipDuplicate);
            return;
        }
        append_record(s.mac_key, key, guest_bytes, payload, s.buffer);
    } else {
        std::vector<u8> rec;
        append_record(s.mac_key, key, guest_bytes, payload, rec);
        s.unchecked_bytes += rec.size();
        s.unchecked.push_back(std::move(rec));
    }
    tstats::note(tstats::Event::Produced);
    tstats::note(tstats::Event::ProducedBytes, total);
    tstats::note(tstats::Event::ProducedRelocs, e.relocs.size());
    if (s.buffer.size() >= (256u << 10)) flush_locked(s, false);
}

void TranslationStore::note_verification(const StoredKey& key, bool match) {
    if (match) {
        tstats::note(tstats::Event::VerifyMatch);
        return;
    }
    tstats::note(tstats::Event::VerifyMismatch);
    State& s = *g_state;
    s.poisoned.store(true, std::memory_order_release);
    std::fprintf(stderr,
                 "[tstore] SHADOW VERIFICATION MISMATCH block=0x%llx size=%u is64=%d: a stored "
                 "translation differs from a fresh one. Poisoning %s; the fresh code runs.\n",
                 static_cast<unsigned long long>(key.start_addr), key.guest_size,
                 (key.flags & StoredKey::Is64) != 0 ? 1 : 0, s.dir.c_str());
    const int fd = ::open((s.dir + "/poison").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) {
        char line[160];
        const int n = std::snprintf(line, sizeof line, "block=0x%llx size=%u pid=%d\n",
                                    static_cast<unsigned long long>(key.start_addr),
                                    key.guest_size, static_cast<int>(::getpid()));
        (void)write_exact(fd, line, static_cast<usize>(n));
        ::close(fd);
    }
}

bool TranslationStore::seen(u64 start_addr) noexcept {
    State& s = *g_state;
    std::shared_lock lock(s.seen_mutex);
    return s.seen.count(start_addr) != 0;
}

void TranslationStore::mark_seen(u64 start_addr) noexcept {
    State& s = *g_state;
    std::unique_lock lock(s.seen_mutex);
    s.seen.insert(start_addr);
}

u64 TranslationStore::config_hash(const JitConfig& c) noexcept {
    static_assert(sizeof(JitConfig) == 200,
                  "JitConfig changed: hash the new field in config_hash()");
    std::vector<u8> m;
    Writer w{m};
    w.put_bool(c.no_jitcache);
    w.put_bool(c.linear_jit);
    w.put_bool(c.linear32_check_map);
    w.put(c.linear32_check_index_bits);
    w.put_bool(c.linear64);
    w.put(c.linear64_address_bits);
    w.put_bool(c.native_ud2_enabled);
    w.put_bool(c.forward_join_state_enabled);
    w.put_bool(c.native_callret_enabled);
    w.put_bool(c.split_cold);
    w.put_bool(c.linear64_perm_guard);
    w.put_bool(c.linear64_vec128);
    w.put(static_cast<u8>(c.tso_mode));
    w.put_bool(c.tso_use_ldapr);
    w.put_bool(c.tso_fault_patch);
    w.put_bool(c.tso_crossing_fault);
    w.put_bool(c.linear64_range_by_fault);
    w.put_bool(c.linear64_jit_view);
    w.put_bool(c.xmm_cache);
    w.put_bool(c.probe_poison_vregs);
    w.put_bool(c.tso_vector);
    w.put_bool(c.strict_tso);
    w.put_bool(c.tso_store_tail_fence);
    w.put<u32>(static_cast<u32>(c.jit_skip.size()));
    for (GuestAddr a : c.jit_skip) w.put<u64>(a);
    w.put<u64>(c.jit_skip_above);
    w.put<u64>(c.jit_skip_below);
    for (usize i = 0; i < JitConfig::kMaxJitSkipRanges; ++i) {
        w.put<u64>(c.jit_skip_range_from[i]);
        w.put<u64>(c.jit_skip_range_to[i]);
    }
    w.put(c.jit_skip_range_count);
    w.put_bool(c.linear64_hw_protect);
    return fnv1a64(m.data(), m.size());
}

std::string TranslationStore::directory() {
    return active() ? g_state->dir : std::string{};
}

void TranslationStore::flush() {
    if (g_active.load(std::memory_order_acquire) != 1) return;
    State& s = *g_state;
    std::lock_guard<std::mutex> lock(s.mutex);
    flush_locked(s, true);
}

void TranslationStore::reset_for_test() {
    std::lock_guard<std::mutex> activation(g_activation_mutex);
    State& s = *g_state;
    if (g_active.load(std::memory_order_acquire) == 1) {
        s.stop.store(true, std::memory_order_release);
        if (s.worker.joinable()) s.worker.join();
        std::lock_guard<std::mutex> lock(s.mutex);
        flush_locked(s, true);
    }
    if (s.seg_fd >= 0) ::close(s.seg_fd);
    if (s.live_fd >= 0) ::close(s.live_fd);
    // Readers may still hold record pointers in tests that misbehave; the old
    // mappings and index are intentionally leaked.
    auto* fresh = new State();
    g_state = fresh;
    g_active.store(-1, std::memory_order_release);
}

void TranslationStore::wait_index_for_test() {
    if (!active()) return;
    while (!g_state->index_done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void TranslationStore::wait_gc_for_test() {
    if (!active()) return;
    while (!g_state->gc_done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

usize TranslationStore::indexed_records_for_test() {
    if (!active()) return 0;
    const Index* index = g_state->index.load(std::memory_order_acquire);
    return index != nullptr ? index->records : 0;
}

void TranslationStore::set_index_delay_for_test(u32 ms) noexcept {
    g_index_delay_ms_for_test.store(ms, std::memory_order_relaxed);
}

void TranslationStore::host_image_range(u64& lo, u64& hi) noexcept {
    lo = host_image().lo;
    hi = host_image().hi;
}

u64 TranslationStore::siphash24(const u8 key[16], const void* data, usize size) noexcept {
    return siphash24_impl(key, static_cast<const u8*>(data), size);
}

}  // namespace vitreo::cpu::jit

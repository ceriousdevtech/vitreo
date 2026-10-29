// SPDX-License-Identifier: LGPL-2.1-only

/// @file credentials.cpp
/// @brief Credential Manager store (see credentials.hpp).

#include "winapi/advapi32/security/credentials.hpp"

#include "winapi/advapi32/security/security.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

namespace vitreo::winapi {

namespace {

// wincred.h
constexpr u32 CRED_TYPE_GENERIC = 1;
constexpr u32 CRED_TYPE_DOMAIN_PASSWORD = 2;
constexpr u32 CRED_TYPE_DOMAIN_CERTIFICATE = 3;
constexpr u32 CRED_TYPE_DOMAIN_EXTENDED = 6;
constexpr u32 CRED_TYPE_MAXIMUM = 7;               // valid types are 1..6
constexpr u32 CRED_PERSIST_SESSION = 1;
constexpr u32 CRED_PERSIST_ENTERPRISE = 3;
constexpr u32 CRED_PRESERVE_CREDENTIAL_BLOB = 0x1;
constexpr u32 CRED_CACHE_TARGET_INFORMATION = 0x1;
constexpr u32 CRED_FLAGS_VALID_FLAGS = 0xF01F;     // wincred.h CRED_FLAGS_VALID_FLAGS
constexpr u32 CRED_MAX_CREDENTIAL_BLOB_SIZE = 5 * 512;
constexpr u32 CRED_MAX_ATTRIBUTES = 64;
constexpr u32 CRED_MAX_VALUE_SIZE = 256;

constexpr u32 ERROR_NOT_FOUND = 1168;
constexpr u32 ERROR_INVALID_FLAGS = 1004;
constexpr u32 ERROR_BAD_USERNAME = 2202;
/// A blob over CRED_MAX_CREDENTIAL_BLOB_SIZE fails in the RPC layer
/// (probe write_bigblob: 0x6f7 RPC_X_BAD_STUB_DATA).
constexpr u32 RPC_X_BAD_STUB_DATA = 1783;

struct Attribute {
    std::u16string keyword;
    u32 flags{0};
    std::vector<u8> value;
};

struct Credential {
    u32 flags{0};
    u32 type{0};
    std::u16string target;
    std::u16string comment;
    u64 last_written{0};
    std::vector<u8> blob;
    u32 persist{0};
    std::vector<Attribute> attributes;
    std::u16string alias;
    std::u16string user;
    bool has_comment{false}, has_alias{false}, has_user{false};
};

bool domain_type(u32 type) {
    return type >= CRED_TYPE_DOMAIN_PASSWORD && type != 5 /*GENERIC_CERTIFICATE*/;
}

/// Domain target names are server/domain names; a '/' cannot appear
/// (probe read_wrongtype: ERROR_INVALID_PARAMETER).
bool valid_target(const std::u16string& target, u32 type) {
    if (target.empty() || target.size() > 32767) return false;
    if (domain_type(type)) {
        return target.size() <= 337 &&
               std::find(target.begin(), target.end(), u'/') == target.end();
    }
    return true;
}

bool same_target(const std::u16string& a, const std::u16string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

// ---- serialisation ---------------------------------------------------------

void put_u32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void put_u64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void put_bytes(std::vector<u8>& out, const u8* data, usize n) {
    put_u32(out, static_cast<u32>(n));
    out.insert(out.end(), data, data + n);
}
void put_str(std::vector<u8>& out, const std::u16string& s) {
    put_bytes(out, reinterpret_cast<const u8*>(s.data()), s.size() * 2);
}

struct Reader {
    const std::vector<u8>& in;
    usize pos{0};
    bool ok{true};
    u32 u32v() {
        if (pos + 4 > in.size()) { ok = false; return 0; }
        u32 v = 0;
        std::memcpy(&v, in.data() + pos, 4);
        pos += 4;
        return v;
    }
    u64 u64v() {
        if (pos + 8 > in.size()) { ok = false; return 0; }
        u64 v = 0;
        std::memcpy(&v, in.data() + pos, 8);
        pos += 8;
        return v;
    }
    std::vector<u8> bytes() {
        const u32 n = u32v();
        if (!ok || pos + n > in.size()) { ok = false; return {}; }
        std::vector<u8> v(in.begin() + pos, in.begin() + pos + n);
        pos += n;
        return v;
    }
    std::u16string str() {
        const auto b = bytes();
        std::u16string s(b.size() / 2, u'\0');
        if (!s.empty()) std::memcpy(s.data(), b.data(), s.size() * 2);
        return s;
    }
};

std::vector<u8> serialise(const std::vector<Credential>& list) {
    std::vector<u8> out;
    put_u32(out, 0x44455243u); // "CRED"
    put_u32(out, static_cast<u32>(list.size()));
    for (const auto& c : list) {
        put_u32(out, c.flags);
        put_u32(out, c.type);
        put_str(out, c.target);
        put_u32(out, (c.has_comment ? 1u : 0u) | (c.has_alias ? 2u : 0u) |
                         (c.has_user ? 4u : 0u));
        put_str(out, c.comment);
        put_u64(out, c.last_written);
        put_bytes(out, c.blob.data(), c.blob.size());
        put_u32(out, c.persist);
        put_u32(out, static_cast<u32>(c.attributes.size()));
        for (const auto& a : c.attributes) {
            put_str(out, a.keyword);
            put_u32(out, a.flags);
            put_bytes(out, a.value.data(), a.value.size());
        }
        put_str(out, c.alias);
        put_str(out, c.user);
    }
    return out;
}

std::vector<Credential> deserialise(const std::vector<u8>& in) {
    std::vector<Credential> list;
    Reader r{in};
    if (r.u32v() != 0x44455243u) return list;
    const u32 count = r.u32v();
    for (u32 i = 0; r.ok && i < count; ++i) {
        Credential c;
        c.flags = r.u32v();
        c.type = r.u32v();
        c.target = r.str();
        const u32 present = r.u32v();
        c.has_comment = (present & 1u) != 0;
        c.has_alias = (present & 2u) != 0;
        c.has_user = (present & 4u) != 0;
        c.comment = r.str();
        c.last_written = r.u64v();
        c.blob = r.bytes();
        c.persist = r.u32v();
        const u32 attrs = r.u32v();
        for (u32 a = 0; r.ok && a < attrs && a <= CRED_MAX_ATTRIBUTES; ++a) {
            Attribute at;
            at.keyword = r.str();
            at.flags = r.u32v();
            at.value = r.bytes();
            c.attributes.push_back(std::move(at));
        }
        c.alias = r.str();
        c.user = r.str();
        if (r.ok) list.push_back(std::move(c));
    }
    return list;
}

// ---- the store ---------------------------------------------------------------

struct Store {
    std::mutex mutex;
    std::string root;                   ///< empty: memory only
    std::vector<Credential> memory_session, memory_persistent;
};

Store& store() {
    static auto* s = new Store();
    return *s;
}

/// Logon-session identity: the host boot. Session credentials written before
/// the last restart are not visible afterwards.
std::string session_file_name() {
    timeval boot{};
    size_t len = sizeof(boot);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (::sysctl(mib, 2, &boot, &len, nullptr, 0) != 0) boot.tv_sec = 0;
    return "session-" + std::to_string(static_cast<long long>(boot.tv_sec)) + ".bin";
}

/// Run `fn(session, persistent)` on the store's two lists under the process
/// mutex and an exclusive host file lock (other capsule processes share the
/// files), writing them back when `fn` returns true.
template <typename Fn>
auto with_lists(Fn&& fn) {
    auto& s = store();
    std::lock_guard lock(s.mutex);
    if (s.root.empty()) {
        return fn(s.memory_session, s.memory_persistent);
    }
    std::error_code ec;
    std::filesystem::create_directories(s.root, ec);
    const std::string lock_path = s.root + "/credentials.lock";
    const int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (lock_fd >= 0) (void)::flock(lock_fd, LOCK_EX);
    auto load = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        return deserialise(bytes);
    };
    const std::string session_path = s.root + "/" + session_file_name();
    const std::string persistent_path = s.root + "/persisted.bin";
    auto session = load(session_path);
    auto persistent = load(persistent_path);
    auto result = fn(session, persistent);
    if (result.second) {
        auto save = [](const std::string& path, const std::vector<Credential>& list) {
            const std::string tmp = path + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                const auto bytes = serialise(list);
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            }
            ::chmod(tmp.c_str(), 0600);
            std::error_code e;
            std::filesystem::rename(tmp, path, e);
        };
        save(session_path, session);
        save(persistent_path, persistent);
    }
    if (lock_fd >= 0) {
        (void)::flock(lock_fd, LOCK_UN);
        ::close(lock_fd);
    }
    return result;
}

std::optional<std::u16string> read_wstr(Marshaler& m, GuestAddr p) {
    if (p == 0) return std::nullopt;
    std::u16string s;
    for (GuestAddr a = p; s.size() <= 32768; a += 2) {
        auto ch = m.read_u16(a);
        if (ch.is_err()) return std::nullopt;
        if (ch.value() == 0) return s;
        s.push_back(static_cast<char16_t>(ch.value()));
    }
    return std::nullopt;
}

u64 now_filetime() {
    const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
    const u64 ticks = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count() / 100);
    return ticks + 116444736000000000ull;
}

/// CREDENTIALW / CREDENTIAL_ATTRIBUTEW layouts (wincred.h).
struct Layout {
    u32 ptr, target, comment, last_written, blob_size, blob, persist,
        attr_count, attrs, alias, user, size, attr_size;
};
Layout layout(bool is64) {
    if (is64) return {8, 8, 16, 24, 32, 40, 48, 52, 56, 64, 72, 80, 24};
    return {4, 8, 12, 16, 24, 28, 32, 36, 40, 44, 48, 52, 16};
}

void cred_write(Marshaler& m) {
    const GuestAddr cred = m.arg_ptr(0);
    const u32 flags = m.arg32(1);
    const Layout L = layout(m.cpu_is_64bit());
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    if (cred == 0 || (flags & ~CRED_PRESERVE_CREDENTIAL_BLOB) != 0) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    Credential c;
    c.flags = m.read_u32(cred).value_or(0);
    c.type = m.read_u32(cred + 4).value_or(0);
    const GuestAddr target_p = static_cast<GuestAddr>(m.read_ptr(cred + L.target).value_or(0));
    const GuestAddr comment_p = static_cast<GuestAddr>(m.read_ptr(cred + L.comment).value_or(0));
    const u32 blob_size = m.read_u32(cred + L.blob_size).value_or(0);
    const GuestAddr blob_p = static_cast<GuestAddr>(m.read_ptr(cred + L.blob).value_or(0));
    c.persist = m.read_u32(cred + L.persist).value_or(0);
    const u32 attr_count = m.read_u32(cred + L.attr_count).value_or(0);
    const GuestAddr attrs_p = static_cast<GuestAddr>(m.read_ptr(cred + L.attrs).value_or(0));
    const GuestAddr alias_p = static_cast<GuestAddr>(m.read_ptr(cred + L.alias).value_or(0));
    const GuestAddr user_p = static_cast<GuestAddr>(m.read_ptr(cred + L.user).value_or(0));

    // Validation order measured on Windows (probe write_*).
    if ((c.flags & ~CRED_FLAGS_VALID_FLAGS) != 0) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    if (c.type == 0 || c.type >= CRED_TYPE_MAXIMUM) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    auto target = read_wstr(m, target_p);
    if (!target || !valid_target(*target, c.type)) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    c.target = *target;
    if (c.persist < CRED_PERSIST_SESSION || c.persist > CRED_PERSIST_ENTERPRISE) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    if (blob_size > CRED_MAX_CREDENTIAL_BLOB_SIZE) return fail(RPC_X_BAD_STUB_DATA);
    if (attr_count > CRED_MAX_ATTRIBUTES) return fail(error_codes::ERROR_INVALID_PARAMETER);
    if (comment_p != 0) {
        auto s = read_wstr(m, comment_p);
        if (!s) return fail(error_codes::ERROR_INVALID_PARAMETER);
        c.comment = *s;
        c.has_comment = true;
    }
    if (alias_p != 0) {
        auto s = read_wstr(m, alias_p);
        if (!s) return fail(error_codes::ERROR_INVALID_PARAMETER);
        c.alias = *s;
        c.has_alias = true;
    }
    if (user_p != 0) {
        auto s = read_wstr(m, user_p);
        if (!s) return fail(error_codes::ERROR_INVALID_PARAMETER);
        c.user = *s;
        c.has_user = !s->empty();
    }
    // Domain credentials must name the account (probe write_domain_nouser).
    if (domain_type(c.type) && !c.has_user) return fail(ERROR_BAD_USERNAME);
    if ((flags & CRED_PRESERVE_CREDENTIAL_BLOB) == 0 && blob_size != 0) {
        if (blob_p == 0) return fail(error_codes::ERROR_INVALID_PARAMETER);
        c.blob.resize(blob_size);
        if (m.memory().read_bytes(blob_p, c.blob.data(), blob_size).is_err()) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
    }
    for (u32 i = 0; i < attr_count; ++i) {
        const GuestAddr a = attrs_p + i * L.attr_size;
        Attribute at;
        auto kw = read_wstr(m, static_cast<GuestAddr>(m.read_ptr(a).value_or(0)));
        if (!kw || kw->empty() || kw->size() > 256) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
        at.keyword = *kw;
        at.flags = m.read_u32(a + L.ptr).value_or(0);
        const u32 vsize = m.read_u32(a + L.ptr + 4).value_or(0);
        const GuestAddr vptr = static_cast<GuestAddr>(m.read_ptr(a + L.ptr + 8).value_or(0));
        if (vsize > CRED_MAX_VALUE_SIZE || (vsize != 0 && vptr == 0)) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
        at.value.resize(vsize);
        if (vsize != 0 && m.memory().read_bytes(vptr, at.value.data(), vsize).is_err()) {
            return fail(error_codes::ERROR_INVALID_PARAMETER);
        }
        c.attributes.push_back(std::move(at));
    }
    c.last_written = now_filetime();
    const bool preserve = (flags & CRED_PRESERVE_CREDENTIAL_BLOB) != 0;

    const auto [error, written] = with_lists(
        [&](std::vector<Credential>& session, std::vector<Credential>& persistent)
            -> std::pair<u32, bool> {
            std::optional<Credential> existing;
            for (auto* list : {&session, &persistent}) {
                auto it = std::find_if(list->begin(), list->end(), [&](const Credential& e) {
                    return e.type == c.type && same_target(e.target, c.target);
                });
                if (it != list->end()) {
                    existing = *it;
                    list->erase(it);
                }
            }
            if (preserve) {
                // Keep the stored secret; nothing to preserve is ERROR_NOT_FOUND
                // (probe write_preserve_missing).
                if (!existing) return {ERROR_NOT_FOUND, false};
                c.blob = existing->blob;
            }
            (c.persist == CRED_PERSIST_SESSION ? session : persistent).push_back(c);
            return {error_codes::ERROR_SUCCESS, true};
        });
    (void)written;
    if (error != error_codes::ERROR_SUCCESS) return fail(error);
    m.set_return(1);
}

void cred_read(Marshaler& m, SecurityStubs& stubs) {
    const GuestAddr target_p = m.arg_ptr(0);
    const u32 type = m.arg32(1);
    const u32 flags = m.arg32(2);
    const GuestAddr out = m.arg_ptr(3);
    auto fail = [&](u32 error) { set_last_error(error); m.set_return(0); };
    auto target = read_wstr(m, target_p);
    if (!target || out == 0 || type == 0 || type >= CRED_TYPE_MAXIMUM ||
        !valid_target(*target, type)) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    if ((flags & ~CRED_CACHE_TARGET_INFORMATION) != 0) return fail(ERROR_INVALID_FLAGS);
    const auto found = with_lists(
        [&](std::vector<Credential>& session, std::vector<Credential>& persistent)
            -> std::pair<std::optional<Credential>, bool> {
            for (auto* list : {&session, &persistent}) {
                for (const auto& e : *list) {
                    if (e.type == type && same_target(e.target, *target)) return {e, false};
                }
            }
            return {std::nullopt, false};
        }).first;
    if (!found) return fail(ERROR_NOT_FOUND);
    Credential c = *found;
    // Domain secrets are for the security packages only: read back with
    // CredentialBlobSize 0 and a NULL blob (probe read_domain).
    if (c.type == CRED_TYPE_DOMAIN_PASSWORD || c.type == CRED_TYPE_DOMAIN_CERTIFICATE ||
        c.type == CRED_TYPE_DOMAIN_EXTENDED) {
        c.blob.clear();
    }

    // One LocalAlloc block: the structure, the attribute array, then the
    // strings and data it points at (CredFree releases it whole).
    const bool is64 = m.cpu_is_64bit();
    const Layout L = layout(is64);
    std::vector<u8> image(L.size + c.attributes.size() * L.attr_size, 0);
    std::vector<std::pair<u32, u32>> fixups; // {field offset, relative target}
    auto align = [&](u32 a) { while (image.size() % a) image.push_back(0); };
    auto add_str = [&](const std::u16string& s) -> u32 {
        align(2);
        const u32 at = static_cast<u32>(image.size());
        const auto* p = reinterpret_cast<const u8*>(s.data());
        image.insert(image.end(), p, p + s.size() * 2);
        image.push_back(0); image.push_back(0);
        return at;
    };
    auto add_bytes = [&](const std::vector<u8>& b) -> u32 {
        align(8);
        const u32 at = static_cast<u32>(image.size());
        image.insert(image.end(), b.begin(), b.end());
        return at;
    };
    auto put32 = [&](u32 off, u32 v) { std::memcpy(image.data() + off, &v, 4); };
    put32(0, c.flags);
    put32(4, c.type);
    fixups.push_back({L.target, add_str(c.target)});
    if (c.has_comment) fixups.push_back({L.comment, add_str(c.comment)});
    std::memcpy(image.data() + L.last_written, &c.last_written, 8);
    put32(L.blob_size, static_cast<u32>(c.blob.size()));
    if (!c.blob.empty()) fixups.push_back({L.blob, add_bytes(c.blob)});
    put32(L.persist, c.persist);
    put32(L.attr_count, static_cast<u32>(c.attributes.size()));
    if (!c.attributes.empty()) fixups.push_back({L.attrs, L.size});
    for (usize i = 0; i < c.attributes.size(); ++i) {
        const u32 base = L.size + static_cast<u32>(i) * L.attr_size;
        fixups.push_back({base, add_str(c.attributes[i].keyword)});
        put32(base + L.ptr, c.attributes[i].flags);
        put32(base + L.ptr + 4, static_cast<u32>(c.attributes[i].value.size()));
        if (!c.attributes[i].value.empty()) {
            fixups.push_back({base + L.ptr + 8, add_bytes(c.attributes[i].value)});
        }
    }
    if (c.has_alias) fixups.push_back({L.alias, add_str(c.alias)});
    if (c.has_user) fixups.push_back({L.user, add_str(c.user)});

    const GuestAddr block = stubs.alloc_local_memory(static_cast<u32>(image.size()));
    if (block == 0) return fail(error_codes::ERROR_NOT_ENOUGH_MEMORY);
    for (const auto& [field, rel] : fixups) {
        const u64 absolute = block + rel;
        std::memcpy(image.data() + field, &absolute, L.ptr);
    }
    if (m.memory().write_bytes(block, image.data(), image.size()).is_err() ||
        m.write_ptr(out, block).is_err()) {
        stubs.free_local_memory(block);
        return fail(error_codes::ERROR_NOACCESS);
    }
    m.set_return(1);
}

void cred_delete(Marshaler& m) {
    const GuestAddr target_p = m.arg_ptr(0);
    const u32 type = m.arg32(1);
    (void)m.arg32(2); // Flags: reserved
    auto target = read_wstr(m, target_p);
    if (!target || type == 0 || type >= CRED_TYPE_MAXIMUM || !valid_target(*target, type)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const bool removed = with_lists(
        [&](std::vector<Credential>& session, std::vector<Credential>& persistent)
            -> std::pair<bool, bool> {
            for (auto* list : {&session, &persistent}) {
                auto it = std::find_if(list->begin(), list->end(), [&](const Credential& e) {
                    return e.type == type && same_target(e.target, *target);
                });
                if (it != list->end()) {
                    list->erase(it);
                    return {true, true};
                }
            }
            return {false, false};
        }).first;
    if (!removed) {
        set_last_error(ERROR_NOT_FOUND); // probe delete_again
        m.set_return(0);
        return;
    }
    m.set_return(1);
}

} // namespace

void set_credential_store_root(std::string root) {
    std::lock_guard lock(store().mutex);
    store().root = std::move(root);
}

void add_credential_exports(DllExports& exports, SecurityStubs& stubs) {
    // BOOL CredWriteW(PCREDENTIALW Credential, DWORD Flags) — 8 bytes.
    exports.entries.push_back({"CredWriteW", 0, [](Marshaler& m) { cred_write(m); }, 8});
    // BOOL CredReadW(LPCWSTR TargetName, DWORD Type, DWORD Flags,
    //                PCREDENTIALW* Credential) — 16 bytes.
    exports.entries.push_back({"CredReadW", 0,
        [&stubs](Marshaler& m) { cred_read(m, stubs); }, 16});
    // BOOL CredDeleteW(LPCWSTR TargetName, DWORD Type, DWORD Flags) — 12 bytes.
    exports.entries.push_back({"CredDeleteW", 0, [](Marshaler& m) { cred_delete(m); }, 12});
    // VOID CredFree(PVOID Buffer) — 4 bytes; NULL is allowed (probe).
    exports.entries.push_back({"CredFree", 0,
        [&stubs](Marshaler& m) {
            const GuestAddr p = m.arg_ptr(0);
            if (p != 0) stubs.free_local_memory(p);
            m.set_return(0);
        }, 4});
}

} // namespace vitreo::winapi

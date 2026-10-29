// SPDX-License-Identifier: LGPL-2.1-only
/// @file ipc_name.hpp
/// @brief Map a Windows kernel-object name to a host resource name.
///
/// Windows IPC names (GUIDs, "Global\\...") routinely exceed POSIX limits
/// (shm_open/sem_open names are ~31 chars incl. leading '/'; AF_UNIX sun_path
/// is 104). We therefore HASH the (namespace-stripped) name into a short,
/// deterministic host name so that two Vitreo host processes derive the SAME
/// host name from the SAME Windows name. Header-only (no link deps).

#pragma once

#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <string>
#include <unistd.h>

namespace vitreo::ipc {

/// FNV-1a 64-bit hash.
[[nodiscard]] inline std::uint64_t fnv1a64(const std::string& s) noexcept {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

/// Scope all host IPC resources to one capsule. `execute_pe` sets this to the
/// canonical capsule root before any guest-visible IPC object is created, and
/// ProcessSession children inherit it. The per-user fallback keeps standalone
/// IPC unit tests isolated from other macOS users.
[[nodiscard]] inline std::string ipc_scope_key() {
    if (const char* scope = std::getenv("VITREO_IPC_SCOPE");
        scope != nullptr && scope[0] != '\0') {
        return scope;
    }
    return "uid:" + std::to_string(static_cast<unsigned>(::getuid()));
}

[[nodiscard]] inline std::string scoped_ipc_name(
    const std::string& win_name) {
    std::string scoped = ipc_scope_key();
    scoped.push_back('\x1f');
    scoped += win_name;
    return scoped;
}

/// Strip a leading Windows object-namespace prefix: "Global\\", "Local\\",
/// "Session\\N\\". Comparison is case-insensitive. Returns the bare name.
[[nodiscard]] inline std::string strip_ns_prefix(const std::string& name) {
    auto bs = name.find('\\');
    if (bs == std::string::npos) return name;
    std::string head;
    for (std::size_t i = 0; i < bs; ++i) {
        head += static_cast<char>(std::tolower(static_cast<unsigned char>(name[i])));
    }
    if (head == "global" || head == "local") {
        return name.substr(bs + 1);
    }
    if (head == "session") {
        auto bs2 = name.find('\\', bs + 1);
        if (bs2 != std::string::npos) return name.substr(bs2 + 1);
    }
    return name;
}

/// Deterministic POSIX shm/sem name for a Windows object name.
/// "/vtm" + 16 hex = 20 chars (< 31-char macOS limit). Includes the leading
/// '/' required by shm_open/sem_open.
[[nodiscard]] inline std::string posix_shm_name(const std::string& win_name) {
    std::uint64_t h =
        fnv1a64(scoped_ipc_name(strip_ns_prefix(win_name)));
    char buf[32];
    std::snprintf(buf, sizeof(buf), "/vtm%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

/// Deterministic AF_UNIX socket path for a Windows pipe name ("\\.\pipe\X").
/// Both server and client pass the same full name, so hashing the whole string
/// agrees. Kept short to stay under the 104-byte sun_path limit; per-uid.
[[nodiscard]] inline std::string posix_pipe_path(const std::string& win_name) {
    // Win32's extended prefix and device prefix address the same pipe.
    // Canonicalize before hashing so either spelling reaches one listener
    // and its metadata, without changing the capsule scope.
    std::string canonical = win_name;
    if (canonical.size() >= 4 && canonical.compare(0, 4, "\\\\?\\") == 0) {
        canonical[2] = '.';
    }
    std::uint64_t h = fnv1a64(scoped_ipc_name(canonical));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "/tmp/vtmp%u_%016llx",
                  static_cast<unsigned>(::getuid()),
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

} // namespace vitreo::ipc

// SPDX-License-Identifier: LGPL-2.1-only
/// @file ipc_gate.hpp
/// @brief Master gate for cross-process IPC behavior.
///
/// Shared memory, named sync objects, and named pipes are part of the normal
/// Windows process contract and are enabled by default. VITREO_IPC=0 remains a
/// diagnostic escape hatch for isolating IPC behavior. Real ProcessSession
/// child handles and read-only capsule process discovery remain available even
/// when that override is active: CreateProcess must always return the real
/// child identity and a handle that becomes signaled only when that child exits.

#pragma once

#include <fcntl.h>
#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

namespace vitreo::ipc {

/// True unless env VITREO_IPC is exactly "0". Read once and cached.
[[nodiscard]] inline bool ipc_enabled() noexcept {
    static const bool enabled = [] {
        const char* v = std::getenv("VITREO_IPC");
        if (v != nullptr && v[0] == '0' && v[1] == '\0') return false;
        return true;
    }();
    return enabled;
}

/// True only when IPC diagnostics are explicitly requested. IPC itself is a
/// normal runtime facility and must not imply synchronous stderr writes on
/// every operation.
[[nodiscard]] inline bool ipc_logging_enabled() noexcept {
    // Marker file as well as the env var: a guest launched by a store client
    // is several re-execs removed from the shell and does not reliably inherit
    // the environment, so an env-only switch is silently off in exactly the
    // processes whose IPC is in question — and no output then reads as "no IPC
    // happened". touch /tmp/vitreo-ipc-trace to enable.
    static const bool enabled = [] {
        if (::access("/tmp/vitreo-ipc-trace", F_OK) == 0) return true;
        const char* v = std::getenv("VITREO_IPC_TRACE");
        return v != nullptr && !(v[0] == '0' && v[1] == '\0');
    }();
    return enabled;
}

/// Where ipc_log writes. stderr is unreachable for a launcher-spawned guest,
/// so the marker-file mode writes a per-pid file beside the marker.
[[nodiscard]] inline int ipc_log_fd() noexcept {
    static const int fd = [] {
        if (::access("/tmp/vitreo-ipc-trace", F_OK) != 0) return 2;
        char path[128];
        std::snprintf(path, sizeof(path), "/tmp/vitreo-ipc-%d.log",
                      static_cast<int>(::getpid()));
        const int opened = ::open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        return opened >= 0 ? opened : 2;
    }();
    return fd;
}

/// Lightweight opt-in diagnostic for cross-process IPC operations.
/// No-op unless VITREO_IPC_TRACE is set to a value other than "0".
__attribute__((format(printf, 1, 2)))
inline void ipc_log(const char* fmt, ...) {
    if (!ipc_logging_enabled()) return;

    // One write per line: a guest process tree shares this stderr, so a
    // separate write for the prefix, the body, and the newline lets other
    // processes interleave inside a single log line.
    char line[1024];
    const int prefix = std::snprintf(line, sizeof(line), "[ipc pid=%d] ",
                                     static_cast<int>(::getpid()));
    if (prefix < 0) return;

    std::va_list ap;
    va_start(ap, fmt);
    const int body = std::vsnprintf(
        line + prefix, sizeof(line) - static_cast<std::size_t>(prefix) - 1,
        fmt, ap);
    va_end(ap);
    if (body < 0) return;

    std::size_t total = static_cast<std::size_t>(prefix) +
                        std::min<std::size_t>(static_cast<std::size_t>(body),
                                              sizeof(line) -
                                                  static_cast<std::size_t>(
                                                      prefix) - 1);
    line[total++] = '\n';
    // One write() so a line stays intact when the whole process tree shares
    // this destination.
    (void)::write(ipc_log_fd(), line, total);
}

} // namespace vitreo::ipc

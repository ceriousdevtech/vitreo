// SPDX-License-Identifier: LGPL-2.1-only
/// @file unix_pipe.cpp

#include "ipc/unix_pipe.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <mutex>
#include <unordered_map>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/un.h>
#include <unistd.h>

namespace vitreo::ipc {

namespace {
bool set_fd_close_on_exec(int fd, bool enabled) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) return false;
    const int updated = enabled ? (flags | FD_CLOEXEC) : (flags & ~FD_CLOEXEC);
    return ::fcntl(fd, F_SETFD, updated) == 0;
}

void suppress_sigpipe(int fd) {
    int enabled = 1;
    (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE,
                       &enabled, sizeof(enabled));
}

// Fill sockaddr_un for `path`; returns false if too long for sun_path.
bool fill_sun(sockaddr_un& addr, const std::string& path) {
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) return false;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    return true;
}
// Listening sockets are non-blocking: every server instance of a name waits
// on the same socket, so several can wake for one connection; the ones that
// lose the accept() race must see "no connection yet", not block.
void set_fd_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

std::mutex& listener_mutex() {
    static std::mutex m;
    return m;
}

struct ListenerEntry {
    std::weak_ptr<UnixPipe::SharedListener> listener;
    u64 id = 0;
};

/// Socket path -> this process's listening socket for it.
std::unordered_map<std::string, ListenerEntry>& listener_registry() {
    static std::unordered_map<std::string, ListenerEntry> registry;
    return registry;
}

u64 next_listener_id() {
    static std::atomic<u64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

struct UnixPipe::SharedListener {
    int fd = -1;
    std::string path;
    u64 id = 0;
    /// Unlink `path` when the last instance closes. Cleared once the socket
    /// is shared with another process, which may still be serving the name.
    std::atomic<bool> owns_path{true};

    ~SharedListener() {
        if (fd >= 0) ::close(fd);
        std::lock_guard<std::mutex> lock(listener_mutex());
        auto& registry = listener_registry();
        auto it = registry.find(path);
        // Only the listener the name still maps to may remove the name: a
        // replacement bound since then owns it now.
        if (it != registry.end() && it->second.id == id) {
            registry.erase(it);
            if (owns_path.load(std::memory_order_acquire) && !path.empty()) {
                ::unlink(path.c_str());
            }
        }
    }
};

void UnixPipe::release_path_ownership() noexcept {
    if (m_listener) m_listener->owns_path.store(false, std::memory_order_release);
}

std::shared_ptr<UnixPipe> UnixPipe::create_server(const std::string& win_name) {
    const std::string path = posix_pipe_path(win_name);
    // Hold the registry across lookup, unlink and bind so a concurrent
    // instance cannot bind over the socket this one is joining.
    std::lock_guard<std::mutex> registry_lock(listener_mutex());
    if (auto it = listener_registry().find(path); it != listener_registry().end()) {
        if (auto listener = it->second.listener.lock()) {
            auto p = std::shared_ptr<UnixPipe>(new UnixPipe());
            p->m_fd = listener->fd;
            p->m_listen = true;
            p->m_listener = std::move(listener);
            return p;
        }
    }
    sockaddr_un addr;
    if (!fill_sun(addr, path)) {
        ipc_log("named pipe server path too long win_name='%s' path='%s'",
                win_name.c_str(), path.c_str());
        return nullptr;
    }

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        ipc_log("named pipe server socket failed win_name='%s' errno=%d (%s)",
                win_name.c_str(), errno, std::strerror(errno));
        return nullptr;
    }
    (void)set_fd_close_on_exec(fd, true);

    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        ipc_log("named pipe server stale-path unlink failed path='%s' "
                "errno=%d (%s)",
                path.c_str(), errno, std::strerror(errno));
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int bind_error = errno;
        ipc_log("named pipe server bind failed win_name='%s' path='%s' "
                "errno=%d (%s)",
                win_name.c_str(), path.c_str(), bind_error,
                std::strerror(bind_error));
        ::close(fd);
        return nullptr;
    }
    if (::listen(fd, 16) != 0) {
        const int listen_error = errno;
        ipc_log("named pipe server listen failed win_name='%s' path='%s' "
                "errno=%d (%s)",
                win_name.c_str(), path.c_str(), listen_error,
                std::strerror(listen_error));
        ::close(fd);
        ::unlink(path.c_str());
        return nullptr;
    }

    set_fd_nonblocking(fd);
    auto listener = std::make_shared<SharedListener>();
    listener->fd = fd;
    listener->path = path;
    listener->id = next_listener_id();
    listener_registry()[path] = ListenerEntry{listener, listener->id};

    auto p = std::shared_ptr<UnixPipe>(new UnixPipe());
    p->m_fd = fd;
    p->m_listen = true;
    p->m_listener = std::move(listener);
    return p;
}

std::shared_ptr<UnixPipe> UnixPipe::connect_client(const std::string& win_name) {
    const std::string path = posix_pipe_path(win_name);
    sockaddr_un addr;
    if (!fill_sun(addr, path)) return nullptr;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    (void)set_fd_close_on_exec(fd, true);
    suppress_sigpipe(fd);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return nullptr;
    }

    return adopt_connected(fd);
}

std::shared_ptr<UnixPipe> UnixPipe::adopt_server(
    int fd, const std::string& win_name) {
    if (fd < 0) return nullptr;
    const std::string path = posix_pipe_path(win_name);
    std::lock_guard<std::mutex> registry_lock(listener_mutex());
    std::shared_ptr<SharedListener> listener;
    if (auto it = listener_registry().find(path); it != listener_registry().end()) {
        listener = it->second.listener.lock();
    }
    if (listener && listener->fd == fd) {
        // A second inherited instance of the same socket: same descriptor.
    } else {
        // The name's socket, handed over by the process that created it
        // (which gave up ownership of the name when it did). It becomes the
        // name's listener here, so later instances created in this process
        // join it rather than binding over it, and the last one to close
        // removes the name.
        listener = std::make_shared<SharedListener>();
        listener->fd = fd;
        listener->path = path;
        listener->id = next_listener_id();
        listener_registry()[path] = ListenerEntry{listener, listener->id};
        set_fd_nonblocking(fd);
    }
    auto pipe = std::shared_ptr<UnixPipe>(new UnixPipe());
    pipe->m_fd = fd;
    pipe->m_listen = true;
    pipe->m_listener = std::move(listener);
    (void)pipe->set_close_on_exec(true);
    return pipe;
}

std::pair<std::shared_ptr<UnixPipe>, std::shared_ptr<UnixPipe>>
UnixPipe::create_pair() {
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {};
    }
    (void)set_fd_close_on_exec(fds[0], true);
    (void)set_fd_close_on_exec(fds[1], true);
    return {adopt_connected(fds[0]), adopt_connected(fds[1])};
}

std::shared_ptr<UnixPipe> UnixPipe::adopt_connected(int fd) {
    if (fd < 0) return nullptr;
    auto pipe = std::shared_ptr<UnixPipe>(new UnixPipe());
    pipe->m_fd = fd;
    // Darwin AF_UNIX large sends can wait despite MSG_DONTWAIT on a
    // blocking descriptor. OVERLAPPED pipe writes must return under
    // backpressure, so make nonblocking mode an endpoint invariant.
    // The synchronous wrappers below wait explicitly for readiness.
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return nullptr;
    }
    suppress_sigpipe(fd);
    (void)pipe->set_close_on_exec(true);
    return pipe;
}

std::shared_ptr<UnixPipe> UnixPipe::accept(int timeout_ms) {
    if (!m_listen || m_fd < 0) return nullptr;
    for (;;) {
        struct pollfd pfd { m_fd, POLLIN, 0 };
        int pr = ::poll(&pfd, 1, timeout_ms);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) return nullptr;  // timeout or error
        int conn = ::accept(m_fd, nullptr, nullptr);
        if (conn >= 0) return adopt_connected(conn);
        // Another instance of this name took the connection.
        if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ||
             errno == ECONNABORTED) && timeout_ms < 0) {
            continue;
        }
        return nullptr;
    }
}

bool UnixPipe::set_close_on_exec(bool enabled) noexcept {
    return m_fd >= 0 && set_fd_close_on_exec(m_fd, enabled);
}

isize UnixPipe::read(void* buf, usize len, std::atomic<bool>* cancel) {
    if (m_fd < 0) return -1;
    for (;;) {
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return -2; // CancelSynchronousIo
        }
        ssize_t r = ::recv(m_fd, buf, len, 0);
        if (r >= 0) return static_cast<isize>(r);
        if (errno == EINTR) continue;
        if ((errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_readable(-1, cancel)) {
            continue;
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return -2;
        }
        return -1;
    }
}

isize UnixPipe::write(const void* buf, usize len) {
    if (m_fd < 0) return -1;
    const char* p = static_cast<const char*>(buf);
    usize off = 0;
    while (off < len) {
        ssize_t w = ::send(m_fd, p + off, len - off, 0);
        if (w > 0) { off += static_cast<usize>(w); continue; }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            wait_writable(-1)) {
            continue;
        }
        return (off > 0) ? static_cast<isize>(off) : -1;
    }
    return static_cast<isize>(off);
}

isize UnixPipe::write_nonblocking(const void* buf, usize len) {
    if (m_fd < 0) return -1;
    for (;;) {
        const ssize_t written = ::send(m_fd, buf, len, MSG_DONTWAIT);
        if (written >= 0) return static_cast<isize>(written);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

isize UnixPipe::peek_available() {
    if (m_fd < 0) return -1;
    int n = 0;
    if (::ioctl(m_fd, FIONREAD, &n) != 0) return -1;
    if (n == 0) {
        // FIONREAD cannot distinguish an empty connection from EOF. Probe
        // without consuming bytes or blocking so readers can report closure.
        char byte;
        const auto result = ::recv(m_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (result == 0) return -1;
        if (result < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            return -1;
    }
    return static_cast<isize>(n);
}

isize UnixPipe::peek_read(void* buf, usize len) {
    if (m_fd < 0 || buf == nullptr || len == 0) return -1;
    for (;;) {
        const auto result = ::recv(m_fd, buf, len, MSG_PEEK | MSG_DONTWAIT);
        if (result >= 0) return static_cast<isize>(result);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
}

bool UnixPipe::wait_readable(int timeout_ms) {
    return wait_readable(timeout_ms, nullptr);
}

bool UnixPipe::wait_readable(int timeout_ms, std::atomic<bool>* cancel) {
    if (m_fd < 0) return false;
    // Slice long waits so CancelSynchronousIo can interrupt a blocked
    // ReadFile on another guest thread (Learn CancelSynchronousIo).
    constexpr int kSliceMs = 50;
    int remaining = timeout_ms;
    for (;;) {
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            return false;
        }
        int slice = kSliceMs;
        if (remaining >= 0) {
            if (remaining == 0) {
                slice = 0;
            } else if (remaining < kSliceMs) {
                slice = remaining;
            }
        }
        struct pollfd descriptor{m_fd, POLLIN, 0};
        int result = 0;
        do {
            result = ::poll(&descriptor, 1, slice);
        } while (result < 0 && errno == EINTR);
        if (result > 0 &&
            (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            return true;
        }
        if (remaining == 0) return false;
        if (remaining > 0) {
            remaining -= slice;
            if (remaining <= 0) return false;
        }
        // remaining < 0 → block forever in slices
    }
}

bool UnixPipe::wait_writable(int timeout_ms) {
    if (m_fd < 0) return false;
    struct pollfd descriptor{m_fd, POLLOUT, 0};
    int result = 0;
    do {
        result = ::poll(&descriptor, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    return result > 0 &&
           (descriptor.revents & (POLLOUT | POLLHUP | POLLERR)) != 0;
}

int UnixPipe::peer_pid() {
    if (m_fd < 0) return -1;
#ifdef LOCAL_PEERPID
    pid_t pid = 0;
    socklen_t len = sizeof(pid);
    if (::getsockopt(m_fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0) {
        return static_cast<int>(pid);
    }
#endif
    return -1;
}

UnixPipe::~UnixPipe() {
    // A server instance's descriptor belongs to its shared listener, which
    // closes it (and removes the name) when the last instance goes.
    if (m_listener) return;
    if (m_fd >= 0) ::close(m_fd);
}

} // namespace vitreo::ipc

// SPDX-License-Identifier: LGPL-2.1-only
/// @file unix_pipe.hpp
/// @brief Windows named pipes ("\\.\pipe\NAME") over AF_UNIX/SOCK_STREAM, so
///        CreateNamedPipe / ConnectNamedPipe / CreateFile(open) / ReadFile /
///        WriteFile work across the forked Vitreo host processes (Stage 4).

#pragma once

#include <vitreo/common.hpp>

#include <memory>
#include <string>
#include <utility>
#include <atomic>

namespace vitreo::ipc {

/// One pipe endpoint: a listening server socket, or a connected stream (a
/// server-side accepted connection, or a client connection).
class UnixPipe {
public:
    ~UnixPipe();
    UnixPipe(const UnixPipe&) = delete;
    UnixPipe& operator=(const UnixPipe&) = delete;

    /// Server: a new instance of @p win_name. Learn CreateNamedPipe: every
    /// instance of a name is reachable and a client connects to whichever
    /// instance is waiting, so a second instance in this process (including
    /// one inherited from the process that created the name) joins the
    /// existing listening socket instead of binding over it. Binding over it
    /// left the earlier instance's ConnectNamedPipe waiting forever.
    /// nullptr on error.
    [[nodiscard]] static std::shared_ptr<UnixPipe> create_server(const std::string& win_name);

    /// Client: connect to an existing server for @p win_name. nullptr on error
    /// (e.g. no server listening → maps to ERROR_FILE_NOT_FOUND / pipe busy).
    [[nodiscard]] static std::shared_ptr<UnixPipe> connect_client(const std::string& win_name);

    /// Adopt an inherited listening descriptor after a Vitreo relaunch.
    [[nodiscard]] static std::shared_ptr<UnixPipe> adopt_server(
        int fd, const std::string& win_name);

    /// Create two connected stream endpoints for an anonymous Win32 pipe.
    [[nodiscard]] static std::pair<std::shared_ptr<UnixPipe>,
                                   std::shared_ptr<UnixPipe>> create_pair();

    /// Adopt one inherited connected endpoint after a Vitreo relaunch.
    [[nodiscard]] static std::shared_ptr<UnixPipe> adopt_connected(int fd);

    /// Server only: accept one pending connection. @p timeout_ms: 0 = poll,
    /// -1 = block. Returns the connected endpoint, or nullptr on timeout/error.
    [[nodiscard]] std::shared_ptr<UnixPipe> accept(int timeout_ms);

    // Connected-endpoint I/O. read: >0 bytes, 0 = EOF, <0 = error/would-block,
    // -2 = cancelled (when @p cancel is set during a blocking wait).
    [[nodiscard]] isize read(void* buf, usize len,
                             std::atomic<bool>* cancel = nullptr);
    [[nodiscard]] isize write(const void* buf, usize len);
    /// Perform one nonblocking send. Returns transferred bytes, zero when the
    /// socket would block, or -1 for a terminal transport error.
    [[nodiscard]] isize write_nonblocking(const void* buf, usize len);
    [[nodiscard]] isize peek_available();   ///< bytes available, <0 on error
    /// Copy up to @p len bytes into @p buf without consuming them (MSG_PEEK).
    /// Returns bytes copied, 0 if none, or <0 on error.
    [[nodiscard]] isize peek_read(void* buf, usize len);
    [[nodiscard]] bool  wait_readable(int timeout_ms);
    /// Like wait_readable, but returns false early when @p cancel is set.
    [[nodiscard]] bool  wait_readable(int timeout_ms, std::atomic<bool>* cancel);
    [[nodiscard]] bool  wait_writable(int timeout_ms);
    [[nodiscard]] int   peer_pid();         ///< LOCAL_PEERPID, -1 if unknown

    [[nodiscard]] bool is_server() const noexcept { return m_listen; }
    [[nodiscard]] int  fd() const noexcept { return m_fd; }
    [[nodiscard]] bool set_close_on_exec(bool enabled) noexcept;
    /// The listening socket now also lives in another process (inherited or
    /// transferred): this process must not unlink its name on close.
    void release_path_ownership() noexcept;

    /// One listening socket per pipe name per process, shared by every
    /// server instance of that name (defined in unix_pipe.cpp).
    struct SharedListener;

private:
    UnixPipe() = default;

    int         m_fd{-1};
    bool        m_listen{false};
    /// Server instances: the name's shared listening socket. Owns the fd and
    /// the socket path; m_fd aliases it.
    std::shared_ptr<SharedListener> m_listener;
};

} // namespace vitreo::ipc

// SPDX-License-Identifier: LGPL-2.1-only
/// @file remote_memory.cpp

#include "winapi/kernel32/memory/remote_memory.hpp"
#include "winapi/kernel32/shared_resource_handle.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstring>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace vitreo::ipc {
namespace {

constexpr u32 kMagic = 0x56524D45; // "VRME"
constexpr u32 kVersion = 10;
constexpr u64 kMaxTransfer = 1024 * 1024;
constexpr u32 kMaxGuestCallArguments = 32;
/// Window present payloads (RGBA) can exceed the memory-RPC limit. A
/// 350×700×4 frame is about 1 MB, and a desktop surface is larger.
constexpr u64 kMaxPresentTransfer = 16ull * 1024ull * 1024ull;

enum class Operation : u32 {
    Read = 1,
    Write = 2,
    Resume = 3,
    Shutdown = 4,
    VirtualAlloc = 5,
    VirtualFree = 6,
    VirtualProtect = 7,
    ImportHandle = 8,
    VirtualQuery = 9,
    ExportHandle = 10,
    PresentWindowPixels = 11,
    // Close a HANDLE in the peer process. Used to apply DUPLICATE_CLOSE_SOURCE
    // after FROM-remote import so the section/sync object stays alive until the
    // importer has opened shared backing (Windows creates the target handle
    // before closing the source).
    CloseHandle = 12,
    RefreshWindowPixels = 13,
    CallGuest = 14,
    PresentWindowSurface = 15,
    MarshalComInterface = 16,
    SetWindowPos = 17,
    SetParent = 18,
    PostThreadMessage = 19,
    // WM_NCHITTEST against a window owned by the peer. Without this a process
    // that does not own the window under the cursor answers HTCLIENT on its
    // behalf, which reads as "this window has no caption" and disables
    // dragging for every cross-process window.
    HitTestWindow = 20,
    // SendMessage/SendMessageTimeout to a window the peer owns. Request bytes:
    // SendHeader + marshaled payload; response: value = LRESULT,
    // reserved = RemoteSendStatus, then `transferred` copied-back bytes.
    SendWindowMessage = 21,
    // PostMessage to a window the peer owns (Learn: an HWND is valid
    // system-wide). argument1 = hwnd, argument2 = message, address = wParam,
    // size = lParam; posted values are not marshaled (messages carrying
    // pointers are synchronous-only and refused before this point).
    PostWindowMessage = 22,
    // ShowWindow on a window the peer owns: argument1 = hwnd, argument2 =
    // nCmdShow; value = ShowWindow's return.
    ShowWindow = 23,
};

struct SendHeader {
    u64 wparam;
    u64 lparam;
    u32 flags;
    u32 timeout_ms;
    u32 out_capacity;
    u32 pad;
};

struct Request {
    u32 magic;
    u32 version;
    u32 operation;
    u32 argument1;
    u64 address;
    u64 size;
    u32 argument2;
    u32 reserved;
};

struct Response {
    u32 magic;
    u32 version;
    u32 success;
    u32 reserved;
    u64 transferred;
    u64 value;
};

std::string socket_path(pid_t pid) {
    const char* temp_dir = ::getenv("TMPDIR");
    if (temp_dir == nullptr || temp_dir[0] == '\0') temp_dir = "/tmp";
    std::string path(temp_dir);
    if (path.back() != '/') path.push_back('/');
    path += "vitreo-rmem-" +
            std::to_string(static_cast<int>(::getuid())) + "-" +
            std::to_string(static_cast<int>(pid)) + ".sock";
    return path;
}

bool send_all(int fd, const void* data, usize size) {
    const auto* bytes = static_cast<const u8*>(data);
    usize sent = 0;
    while (sent < size) {
        const ssize_t result =
            ::send(fd, bytes + sent, size - sent, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        sent += static_cast<usize>(result);
    }
    return true;
}

bool receive_all(int fd, void* data, usize size) {
    auto* bytes = static_cast<u8*>(data);
    usize received = 0;
    while (received < size) {
        const ssize_t result = ::recv(fd, bytes + received, size - received, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) return false;
        received += static_cast<usize>(result);
    }
    return true;
}

bool send_fd(int sock, int fd_to_send) {
    char byte = 0;
    iovec iov{};
    iov.iov_base = &byte;
    iov.iov_len = 1;
    alignas(cmsghdr) char cmsgbuf[CMSG_SPACE(sizeof(int))];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(fd_to_send));
    for (;;) {
        const ssize_t result = ::sendmsg(sock, &msg, MSG_NOSIGNAL);
        if (result < 0 && errno == EINTR) continue;
        return result == 1;
    }
}

bool receive_fd(int sock, int& out_fd) {
    char byte = 0;
    iovec iov{};
    iov.iov_base = &byte;
    iov.iov_len = 1;
    alignas(cmsghdr) char cmsgbuf[CMSG_SPACE(sizeof(int))];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsgbuf;
    msg.msg_controllen = sizeof(cmsgbuf);
    for (;;) {
        const ssize_t result = ::recvmsg(sock, &msg, 0);
        if (result < 0 && errno == EINTR) continue;
        if (result != 1) return false;
        break;
    }
    out_fd = -1;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type == SCM_RIGHTS &&
            cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
            std::memcpy(&out_fd, CMSG_DATA(cmsg), sizeof(out_fd));
            return out_fd >= 0;
        }
    }
    return false;
}

int connect_once(pid_t pid) {
    const std::string path = socket_path(pid);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
        ::close(fd);
        return -1;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                  sizeof(address)) == 0) {
        return fd;
    }
    ::close(fd);
    return -1;
}

int connect_to(pid_t pid) {
    const std::string path = socket_path(pid);
    // A CREATE_SUSPENDED worker materializes its static PE/module address
    // space before publishing this endpoint. Large vendor DLLs can make that
    // loader phase noticeably slower than native CreateProcess.
    for (u32 attempt = 0; attempt < 3000; ++attempt) {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.size() >= sizeof(address.sun_path)) {
            ::close(fd);
            return -1;
        }
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) == 0) {
            return fd;
        }
        const int error = errno;
        ::close(fd);
        if (error != ENOENT && error != ECONNREFUSED) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}

bool transact(pid_t pid, Operation operation, GuestAddr address,
              u64 operation_size, u32 argument1, u32 argument2,
              std::span<const Byte> input, std::span<Byte> output,
              u64* value = nullptr, u64* transferred = nullptr,
              u32 reserved = 0) {
    const bool returns_bytes =
        operation == Operation::Read || operation == Operation::VirtualQuery ||
        operation == Operation::ExportHandle;
    const bool sends_bytes =
        operation == Operation::Write ||
        operation == Operation::ImportHandle ||
        operation == Operation::PresentWindowPixels ||
        operation == Operation::CallGuest ||
        operation == Operation::MarshalComInterface;
    const u64 size =
        sends_bytes
        ? static_cast<u64>(input.size())
        : returns_bytes
            ? static_cast<u64>(output.size())
            : operation_size;
    const u64 size_limit =
        (operation == Operation::PresentWindowPixels)
            ? kMaxPresentTransfer
            : kMaxTransfer;
    if ((operation == Operation::Read || operation == Operation::Write ||
         operation == Operation::ImportHandle ||
         operation == Operation::VirtualQuery ||
         operation == Operation::ExportHandle ||
         operation == Operation::PresentWindowPixels ||
         operation == Operation::CallGuest ||
         operation == Operation::MarshalComInterface) &&
        size > size_limit) {
        return false;
    }
    const int fd = connect_to(pid);
    if (fd < 0) return false;
    const Request request{
        kMagic, kVersion, static_cast<u32>(operation), argument1,
        address, size, argument2, reserved};
    bool ok = send_all(fd, &request, sizeof(request));
    if (ok && sends_bytes && !input.empty()) {
        ok = send_all(fd, input.data(), input.size());
    }
    Response response{};
    if (ok) ok = receive_all(fd, &response, sizeof(response));
    if (ok && response.magic == kMagic && response.version == kVersion &&
        response.success != 0 && returns_bytes && !output.empty() &&
        response.transferred > 0 && response.transferred <= size) {
        ok = receive_all(fd, output.data(),
                         static_cast<usize>(response.transferred));
    }
    ::close(fd);
    const bool response_ok =
        ok && response.magic == kMagic && response.version == kVersion &&
        response.success != 0 &&
        ((operation != Operation::Read && operation != Operation::Write &&
          operation != Operation::VirtualQuery &&
          operation != Operation::PresentWindowPixels) ||
         response.transferred == size) &&
        (operation != Operation::ExportHandle ||
         (response.transferred > 0 && response.transferred <= size));
    if (response_ok && value != nullptr) *value = response.value;
    if (response_ok && transferred != nullptr) {
        *transferred = response.transferred;
    }
    return response_ok;
}

} // namespace

RemoteMemoryServer::RemoteMemoryServer(
    GuestMemory& memory, winapi::VirtualMemoryManager& virtual_memory,
    winapi::SyncManager* sync_manager,
    winapi::FileMappingManager* file_mapping_manager,
    winapi::HandleTable* handle_table,
    winapi::FileManager* file_manager)
    : m_memory(memory),
      m_virtual_memory(virtual_memory),
      m_sync_manager(sync_manager),
      m_file_mapping_manager(file_mapping_manager),
      m_handle_table(handle_table),
      m_file_manager(file_manager) {}

RemoteMemoryServer::~RemoteMemoryServer() {
    if (m_listener >= 0) {
        (void)transact(
            ::getpid(), Operation::Shutdown, 0, 0, 0, 0, {}, {});
        if (m_thread.joinable()) m_thread.join();
        ::close(m_listener);
        m_listener = -1;
    }
    if (!m_path.empty()) (void)::unlink(m_path.c_str());
}

bool RemoteMemoryServer::start(bool initially_suspended) {
    m_path = socket_path(::getpid());
    (void)::unlink(m_path.c_str());
    m_listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (m_listener < 0) {
        ipc_log("remote GuestMemory socket failed errno=%d (%s)",
                errno, std::strerror(errno));
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (m_path.size() >= sizeof(address.sun_path)) {
        ipc_log("remote GuestMemory socket path too long path='%s'",
                m_path.c_str());
        ::close(m_listener);
        m_listener = -1;
        return false;
    }
    std::memcpy(address.sun_path, m_path.c_str(), m_path.size() + 1);
    if (::bind(m_listener, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) != 0) {
        const int error = errno;
        ipc_log("remote GuestMemory bind failed path='%s' errno=%d (%s)",
                m_path.c_str(), error, std::strerror(error));
        ::close(m_listener);
        m_listener = -1;
        (void)::unlink(m_path.c_str());
        return false;
    }
    if (::chmod(m_path.c_str(), 0600) != 0) {
        const int error = errno;
        ipc_log("remote GuestMemory chmod failed path='%s' errno=%d (%s)",
                m_path.c_str(), error, std::strerror(error));
        ::close(m_listener);
        m_listener = -1;
        (void)::unlink(m_path.c_str());
        return false;
    }
    if (::listen(m_listener, 8) != 0) {
        const int error = errno;
        ipc_log("remote GuestMemory listen failed path='%s' errno=%d (%s)",
                m_path.c_str(), error, std::strerror(error));
        ::close(m_listener);
        m_listener = -1;
        (void)::unlink(m_path.c_str());
        return false;
    }
    m_initially_suspended = initially_suspended;
    m_resumed = !initially_suspended;
    m_thread = std::thread([this] { serve(); });
    ipc_log("remote GuestMemory server ready path='%s' suspended=%d",
            m_path.c_str(), initially_suspended ? 1 : 0);
    return true;
}

void RemoteMemoryServer::wait_for_initial_resume() {
    if (!m_initially_suspended) return;
    ipc_log("primary thread waiting for ResumeThread");
    std::unique_lock lock(m_resume_mutex);
    m_resume_cv.wait(lock, [this] { return m_resumed; });
    ipc_log("primary thread resumed");
}

void RemoteMemoryServer::serve() {
    for (;;) {
        const int client = ::accept(m_listener, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR) continue;
            return;
        }
        Request request{};
        bool ok = receive_all(client, &request, sizeof(request)) &&
                  request.magic == kMagic && request.version == kVersion;
        const auto operation = static_cast<Operation>(request.operation);
        if (ok) {
            const u64 size_limit =
                (operation == Operation::PresentWindowPixels)
                    ? kMaxPresentTransfer
                    : kMaxTransfer;
            if ((operation == Operation::Read ||
                 operation == Operation::Write ||
                 operation == Operation::ImportHandle ||
                 operation == Operation::ExportHandle ||
                 operation == Operation::VirtualQuery ||
                 operation == Operation::PresentWindowPixels ||
                 operation == Operation::CallGuest ||
                 operation == Operation::MarshalComInterface ||
                 operation == Operation::SendWindowMessage) &&
                request.size > size_limit) {
                ok = false;
            }
        }
        if (ok && operation == Operation::SendWindowMessage) {
            // The send blocks on the receiving guest thread (up to its
            // timeout); run it off the accept loop so other RPCs -- including
            // ones the receiver makes back to this process -- still flow.
            std::vector<Byte> in(static_cast<usize>(request.size));
            bool got = request.size >= sizeof(SendHeader) &&
                       receive_all(client, in.data(), in.size());
            std::thread([this, client, request, got, in = std::move(in)] {
                u64 lresult = 0;
                std::vector<Byte> out;
                auto status = RemoteSendStatus::TargetGone;
                bool handled = false;
                if (got && m_window_send_message) {
                    SendHeader h{};
                    std::memcpy(&h, in.data(), sizeof(h));
                    status = m_window_send_message(
                        request.argument1, request.argument2, h.wparam,
                        h.lparam, h.flags, h.timeout_ms,
                        std::span<const Byte>(in).subspan(sizeof(SendHeader)),
                        h.out_capacity, lresult, out);
                    handled = true;
                    if (out.size() > h.out_capacity) out.resize(h.out_capacity);
                }
                const Response response{
                    kMagic, kVersion, handled ? 1u : 0u,
                    static_cast<u32>(status),
                    handled ? static_cast<u64>(out.size()) : 0u, lresult};
                if (send_all(client, &response, sizeof(response)) && handled &&
                    !out.empty()) {
                    (void)send_all(client, out.data(), out.size());
                }
                ::close(client);
            }).detach();
            continue;
        }
        std::vector<Byte> bytes;
        if (operation == Operation::Read || operation == Operation::Write ||
            operation == Operation::ImportHandle ||
            operation == Operation::ExportHandle ||
            operation == Operation::VirtualQuery ||
            operation == Operation::PresentWindowPixels ||
            operation == Operation::CallGuest ||
            operation == Operation::MarshalComInterface) {
            bytes.resize(static_cast<usize>(request.size));
        }
        if (ok && (operation == Operation::Write ||
                   operation == Operation::ImportHandle ||
                   operation == Operation::PresentWindowPixels ||
                   operation == Operation::CallGuest ||
                   operation == Operation::MarshalComInterface) &&
            !bytes.empty()) {
            ok = receive_all(client, bytes.data(), bytes.size());
        }

        u64 response_value = 0;
        u64 response_transferred = 0;
        int export_pipe_fd = -1;
        if (ok && operation == Operation::Read) {
            ok = m_memory.read_bytes(
                request.address, bytes.data(), bytes.size()).is_ok();
            if (ok) response_transferred = request.size;
        } else if (ok && operation == Operation::Write) {
            ok = m_memory.write_bytes(
                request.address, bytes.data(), bytes.size()).is_ok();
            if (ok) response_transferred = request.size;
        } else if (ok && operation == Operation::Resume) {
            {
                std::lock_guard lock(m_resume_mutex);
                m_resumed = true;
            }
            m_resume_cv.notify_all();
        } else if (ok && operation == Operation::VirtualAlloc) {
            response_value = m_virtual_memory.virtual_alloc(
                request.address, request.size,
                request.argument1, request.argument2);
            ok = response_value != 0;
        } else if (ok && operation == Operation::VirtualFree) {
            ok = m_virtual_memory.virtual_free(
                request.address, request.size, request.argument1);
        } else if (ok && operation == Operation::VirtualProtect) {
            u32 old_protect = 0;
            ok = m_virtual_memory.virtual_protect(
                request.address, request.size,
                request.argument1, &old_protect);
            response_value = old_protect;
        } else if (ok && operation == Operation::VirtualQuery) {
            ok = bytes.size() == sizeof(RemoteMemoryBasicInformation);
            if (ok) {
                winapi::VirtualQueryManager manager(
                    m_memory, &m_virtual_memory);
                winapi::MemoryBasicInformation queried{};
                const u64 maximum_address =
                    static_cast<u64>(request.argument1) |
                    (static_cast<u64>(request.argument2) << 32);
                ok = manager.virtual_query(
                         request.address, queried, maximum_address) != 0;
                if (ok) {
                    const RemoteMemoryBasicInformation result{
                        queried.base_address,
                        queried.allocation_base,
                        queried.region_size,
                        queried.allocation_protect,
                        queried.state,
                        queried.protect,
                        queried.type,
                    };
                    std::memcpy(bytes.data(), &result, sizeof(result));
                    response_transferred = sizeof(result);
                }
            }
        } else if (ok && operation == Operation::ImportHandle) {
            const std::string_view descriptor(
                reinterpret_cast<const char*>(bytes.data()), bytes.size());
            u32 handle = 0;
            if (descriptor.starts_with("S,") && m_sync_manager != nullptr) {
                handle = m_sync_manager->import_transferred_handle(descriptor);
            } else if (descriptor.starts_with("M,") &&
                       m_file_mapping_manager != nullptr) {
                handle =
                    m_file_mapping_manager->import_transferred_handle(descriptor);
            } else if (descriptor.starts_with("R,") &&
                       m_handle_table != nullptr) {
                // "R,<access>,<flags>,<pid>" — Process by host pid.
                std::array<std::string_view, 4> fields{};
                usize field_start = 2;
                bool parsed = true;
                for (usize index = 1; index < fields.size(); ++index) {
                    const usize comma = descriptor.find(',', field_start);
                    if (index + 1 == fields.size()) {
                        if (comma != std::string_view::npos) {
                            parsed = false;
                            break;
                        }
                        fields[index] = descriptor.substr(field_start);
                    } else {
                        if (comma == std::string_view::npos) {
                            parsed = false;
                            break;
                        }
                        fields[index] =
                            descriptor.substr(field_start, comma - field_start);
                        field_start = comma + 1;
                    }
                }
                u32 access = 0;
                u32 flags = 0;
                long long pid_value = 0;
                if (parsed) {
                    const auto access_end = std::from_chars(
                        fields[1].data(),
                        fields[1].data() + fields[1].size(), access);
                    const auto flags_end = std::from_chars(
                        fields[2].data(),
                        fields[2].data() + fields[2].size(), flags);
                    const auto pid_end = std::from_chars(
                        fields[3].data(),
                        fields[3].data() + fields[3].size(), pid_value);
                    parsed = access_end.ec == std::errc{} &&
                             access_end.ptr ==
                                 fields[1].data() + fields[1].size() &&
                             flags_end.ec == std::errc{} &&
                             flags_end.ptr ==
                                 fields[2].data() + fields[2].size() &&
                             pid_end.ec == std::errc{} &&
                             pid_end.ptr ==
                                 fields[3].data() + fields[3].size() &&
                             pid_value > 0;
                }
                if (parsed) {
                    ipc::ProcessObject* process =
                        ipc::track_process(static_cast<pid_t>(pid_value));
                    if (process != nullptr) {
                        handle = m_handle_table->allocate(
                            winapi::HandleType::Process, process, flags,
                            access);
                    }
                }
            } else if (descriptor.starts_with("D,") &&
                       m_handle_table != nullptr) {
                // A DXGI shared-resource NT handle.
                handle = winapi::import_shared_resource_transfer(
                    *m_handle_table, descriptor);
            } else if (descriptor.starts_with("P,") ||
                       descriptor.starts_with("F,")) {
                int transferred_fd = -1;
                ok = receive_fd(client, transferred_fd);
                if (ok && m_file_manager != nullptr) {
                    handle = m_file_manager->import_transferred_handle(
                        descriptor, transferred_fd);
                } else {
                    if (transferred_fd >= 0) ::close(transferred_fd);
                    ok = false;
                }
            }
            response_value = handle;
            ok = ok && handle != 0;
        } else if (ok && operation == Operation::ExportHandle) {
            constexpr u32 DUPLICATE_CLOSE_SOURCE = 0x00000001;
            constexpr u32 DUPLICATE_SAME_ACCESS = 0x00000002;
            const auto source = static_cast<winapi::Handle>(request.address);
            const bool same_access =
                (request.argument2 & DUPLICATE_SAME_ACCESS) != 0;
            const bool close_source =
                (request.argument2 & DUPLICATE_CLOSE_SOURCE) != 0;
            const u32 access = same_access
                ? (m_handle_table != nullptr
                       ? m_handle_table->get_access(source)
                       : winapi::HandleAccess::All)
                : request.argument1;
            std::optional<std::string> descriptor;
            int pipe_fd = -1;
            if (m_handle_table != nullptr &&
                m_handle_table->get_type(source) ==
                    winapi::HandleType::Process) {
                auto* process = static_cast<ipc::ProcessObject*>(
                    m_handle_table->get(source, winapi::HandleType::Process));
                if (process != nullptr) {
                    descriptor =
                        "R," + std::to_string(access) + ",0," +
                        std::to_string(
                            static_cast<long long>(process->pid));
                }
            }
            if (!descriptor && m_handle_table != nullptr &&
                m_handle_table->get_type(source) ==
                    winapi::HandleType::SharedResource) {
                descriptor = winapi::prepare_shared_resource_transfer(
                    *m_handle_table, source, access, /*inherit=*/false);
            }
            if (!descriptor && m_sync_manager != nullptr) {
                descriptor = m_sync_manager->prepare_transferred_handle(
                    source, access, /*inheritable=*/false);
            }
            if (!descriptor && m_file_mapping_manager != nullptr) {
                descriptor =
                    m_file_mapping_manager->prepare_transferred_handle(
                        source, access, /*inheritable=*/false);
            }
            if (!descriptor && m_file_manager != nullptr) {
                descriptor = m_file_manager->prepare_transferred_handle(
                    source, access, /*inheritable=*/false, pipe_fd);
            }
            if (!descriptor || descriptor->size() > bytes.size()) {
                if (pipe_fd >= 0) ::close(pipe_fd);
                ok = false;
            } else {
                std::memcpy(bytes.data(), descriptor->data(),
                            descriptor->size());
                response_transferred = descriptor->size();
                export_pipe_fd = pipe_fd;
                // Do not honor close_source here. Closing the last section
                // handle before the peer imports drops SharedMemory lifetime
                // refs and unlinks POSIX shm (especially when the sender never
                // MapViewOfFile'd). Callers apply DUPLICATE_CLOSE_SOURCE via
                // remote_close_handle after a successful local import.
                (void)close_source;
            }
        } else if (ok && operation == Operation::CloseHandle) {
            const auto handle =
                static_cast<winapi::Handle>(request.address);
            if (m_handle_table == nullptr || handle == 0) {
                ok = false;
            } else {
                ok = m_handle_table->close(handle);
            }
        } else if (ok && operation == Operation::PresentWindowPixels) {
            const auto hwnd = request.argument1;
            const i32 width = static_cast<i32>(request.address & 0xffffffffu);
            const i32 height =
                static_cast<i32>((request.address >> 32) & 0xffffffffu);
            const u64 expected =
                (width > 0 && height > 0)
                    ? static_cast<u64>(width) * static_cast<u64>(height) * 4ull
                    : 0ull;
            ok = m_window_present && width > 0 && height > 0 &&
                 expected == request.size &&
                 bytes.size() == static_cast<usize>(expected) &&
                 m_window_present(hwnd, std::span<const Byte>(bytes), width,
                                  height);
            if (ok) response_transferred = request.size;
            VITREO_TRACE(
                "[ipc] PresentWindowPixels hwnd=0x%x %dx%d bytes=%llu -> %s\n",
                hwnd, width, height,
                static_cast<unsigned long long>(request.size),
                ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::PresentWindowSurface) {
            const u32 hwnd = request.argument1;
            const u64 shared_handle = request.address;
            const i32 width = static_cast<i32>(request.size & 0xffffffffu);
            const i32 height =
                static_cast<i32>((request.size >> 32) & 0xffffffffu);
            // Microsoft Learn, IDXGISwapChain::Present: SyncInterval 0
            // presents immediately, 1-4 waits that many vertical blanks.
            // Anything else is invalid, so clamp rather than trust a peer.
            const u32 sync_interval =
                request.argument2 <= 4u ? request.argument2 : 1u;
            const u32 src_x = request.reserved & 0xFFFFu;
            const u32 src_y = (request.reserved >> 16) & 0xFFFFu;
            ok = hwnd != 0 && shared_handle != 0 && width > 0 && height > 0 &&
                 m_window_surface_present &&
                 m_window_surface_present(
                     hwnd, shared_handle, width, height, sync_interval,
                     src_x, src_y);
            VITREO_TRACE(
                "[ipc] PresentWindowSurface hwnd=0x%x handle=0x%llx "
                "%dx%d -> %s\n",
                hwnd, static_cast<unsigned long long>(shared_handle),
                width, height, ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::RefreshWindowPixels) {
            const u32 hwnd = request.argument1;
            ok = hwnd != 0 && m_window_refresh && m_window_refresh(hwnd);
            VITREO_TRACE(
                "[ipc] RefreshWindowPixels hwnd=0x%x -> %s\n", hwnd,
                ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::SetWindowPos) {
            const u32 hwnd = request.argument1;
            const u32 insert_after =
                static_cast<u32>(request.address & 0xffffffffu);
            const i32 x = static_cast<i32>(
                (request.address >> 32) & 0xffffffffu);
            const i32 y = static_cast<i32>(request.size & 0xffffffffu);
            const i32 cx = static_cast<i32>(
                (request.size >> 32) & 0xffffffffu);
            const i32 cy = static_cast<i32>(request.reserved);
            const u32 flags = request.argument2;
            ok = hwnd != 0 && m_window_set_window_pos &&
                 m_window_set_window_pos(hwnd, insert_after, x, y, cx, cy,
                                         flags);
            VITREO_TRACE(
                "[ipc] SetWindowPos hwnd=0x%x after=0x%x "
                "(%d,%d %dx%d) flags=0x%x -> %s\n",
                hwnd, insert_after, x, y, cx, cy, flags,
                ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::ShowWindow) {
            const u32 hwnd = request.argument1;
            const i32 cmd = static_cast<i32>(request.argument2);
            const auto shown = hwnd != 0 && m_window_show ? m_window_show(hwnd, cmd)
                                                          : std::nullopt;
            ok = shown.has_value();
            if (ok) response_value = *shown;
            VITREO_TRACE("[ipc] ShowWindow hwnd=0x%x cmd=%d -> %s 0x%llx\n", hwnd, cmd,
                         ok ? "ok" : "FAIL", static_cast<unsigned long long>(response_value));
        } else if (ok && operation == Operation::SetParent) {
            const u32 child = request.argument1;
            const u32 parent = request.argument2;
            ok = child != 0 && m_window_set_parent &&
                 m_window_set_parent(child, parent);
            VITREO_TRACE("[ipc] SetParent child=0x%x parent=0x%x -> %s\n",
                         child, parent, ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::HitTestWindow) {
            const u32 hwnd = request.argument1;
            const i32 screen_x = static_cast<i32>(
                static_cast<i16>(request.address & 0xffffu));
            const i32 screen_y = static_cast<i32>(
                static_cast<i16>((request.address >> 16) & 0xffffu));
            ok = hwnd != 0 && m_window_hit_test;
            if (ok) {
                response_value = static_cast<u64>(static_cast<u32>(
                    m_window_hit_test(hwnd, screen_x, screen_y)));
            }
            VITREO_TRACE("[ipc] HitTestWindow hwnd=0x%x (%d,%d) -> %llu %s\n",
                         hwnd, screen_x, screen_y,
                         static_cast<unsigned long long>(response_value),
                         ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::PostThreadMessage) {
            const u32 message = request.argument1;
            ok = m_thread_message &&
                 m_thread_message(message, request.address, request.size);
            ipc_log(
                "received PostThreadMessage msg=0x%x wParam=0x%llx "
                "lParam=0x%llx result=%d",
                message, static_cast<unsigned long long>(request.address),
                static_cast<unsigned long long>(request.size), ok ? 1 : 0);
            VITREO_TRACE(
                "[ipc] PostThreadMessage msg=0x%x wParam=0x%llx "
                "lParam=0x%llx -> %s\n",
                message, static_cast<unsigned long long>(request.address),
                static_cast<unsigned long long>(request.size),
                ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::PostWindowMessage) {
            ok = request.argument1 != 0 && m_window_post_message &&
                 m_window_post_message(request.argument1, request.argument2,
                                       request.address, request.size);
            VITREO_TRACE(
                "[ipc] PostWindowMessage hwnd=0x%x msg=0x%x wParam=0x%llx "
                "lParam=0x%llx -> %s\n",
                request.argument1, request.argument2,
                static_cast<unsigned long long>(request.address),
                static_cast<unsigned long long>(request.size),
                ok ? "ok" : "FAIL");
        } else if (ok && operation == Operation::CallGuest) {
            const u32 argument_count = request.argument1;
            ok = m_guest_call && request.address != 0 &&
                 argument_count <= kMaxGuestCallArguments &&
                 request.size ==
                     static_cast<u64>(argument_count) * sizeof(u64) &&
                 bytes.size() == static_cast<usize>(request.size);
            if (ok) {
                std::vector<u64> arguments(argument_count);
                if (!bytes.empty()) {
                    std::memcpy(arguments.data(), bytes.data(), bytes.size());
                }
                const auto result = m_guest_call(
                    request.address, std::span<const u64>(arguments),
                    request.argument2 != 0);
                ok = result.has_value();
                if (ok) response_value = *result;
            }
        } else if (ok && operation == Operation::MarshalComInterface) {
            ok = m_com_interface_marshal && request.argument1 != 0 &&
                 request.address != 0 &&
                 (request.argument2 == 4 || request.argument2 == 8) &&
                 bytes.size() == 16;
            if (ok) {
                response_value = m_com_interface_marshal(
                    request.argument1, request.address, request.argument2,
                    std::span<const Byte>(bytes));
                ok = response_value != 0;
            }
        } else if (ok && operation != Operation::Shutdown) {
            ok = false;
        }

        const Response response{
            kMagic, kVersion, ok ? 1u : 0u, 0,
            ok ? response_transferred : 0u,
            response_value};
        (void)send_all(client, &response, sizeof(response));
        if (ok && response_transferred > 0 &&
            (operation == Operation::Read ||
             operation == Operation::VirtualQuery ||
             operation == Operation::ExportHandle)) {
            (void)send_all(client, bytes.data(),
                           static_cast<usize>(response_transferred));
        }
        if (export_pipe_fd >= 0) {
            if (ok) (void)send_fd(client, export_pipe_fd);
            ::close(export_pipe_fd);
        }
        ::close(client);
        if (operation == Operation::Shutdown) return;
    }
}

bool remote_read_process_memory(
    pid_t pid, GuestAddr address, std::span<Byte> output) {
    return transact(pid, Operation::Read, address, output.size(), 0, 0,
                    {}, output);
}

bool remote_write_process_memory(
    pid_t pid, GuestAddr address, std::span<const Byte> input) {
    return transact(pid, Operation::Write, address, input.size(), 0, 0,
                    input, {});
}

GuestAddr remote_virtual_alloc(
    pid_t pid, GuestAddr address, u64 size, u32 allocation_type, u32 protect) {
    u64 result = 0;
    if (!transact(pid, Operation::VirtualAlloc, address, size,
                  allocation_type, protect, {}, {}, &result)) {
        return 0;
    }
    return result;
}

bool remote_virtual_free(
    pid_t pid, GuestAddr address, u64 size, u32 free_type) {
    return transact(pid, Operation::VirtualFree, address, size,
                    free_type, 0, {}, {});
}

bool remote_virtual_protect(
    pid_t pid, GuestAddr address, u64 size,
    u32 new_protect, u32& old_protect) {
    u64 result = 0;
    if (!transact(pid, Operation::VirtualProtect, address, size,
                  new_protect, 0, {}, {}, &result)) {
        return false;
    }
    old_protect = static_cast<u32>(result);
    return true;
}

bool remote_virtual_query(
    pid_t pid, GuestAddr address, u64 maximum_address,
    RemoteMemoryBasicInformation& information) {
    auto output = std::span<Byte>(
        reinterpret_cast<Byte*>(&information), sizeof(information));
    return transact(
        pid, Operation::VirtualQuery, address, output.size(),
        static_cast<u32>(maximum_address),
        static_cast<u32>(maximum_address >> 32), {}, output);
}

bool remote_resume_primary_thread(pid_t pid) {
    return transact(pid, Operation::Resume, 0, 0, 0, 0, {}, {});
}

bool remote_process_endpoint_available(pid_t pid) {
    if (pid <= 0) return false;
    const int fd = connect_once(pid);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

std::optional<u64> remote_call_guest(
    pid_t pid, GuestAddr function, std::span<const u64> arguments,
    bool caller_elevated) {
    if (pid <= 0 || function == 0 ||
        arguments.size() > kMaxGuestCallArguments) {
        return std::nullopt;
    }
    const auto input = std::span<const Byte>(
        reinterpret_cast<const Byte*>(arguments.data()),
        arguments.size_bytes());
    u64 result = 0;
    if (!transact(pid, Operation::CallGuest, function, input.size(),
                  static_cast<u32>(arguments.size()),
                  caller_elevated ? 1u : 0u, input, {},
                  &result)) {
        return std::nullopt;
    }
    return result;
}

GuestAddr remote_marshal_com_interface(
    pid_t pid, u32 owner_pid, GuestAddr object, u32 owner_pointer_size,
    std::span<const Byte> iid) {
    if (pid <= 0 || owner_pid == 0 || object == 0 || iid.size() != 16 ||
        (owner_pointer_size != 4 && owner_pointer_size != 8)) {
        return 0;
    }
    u64 result = 0;
    if (!transact(pid, Operation::MarshalComInterface, object, iid.size(),
                  owner_pid, owner_pointer_size, iid, {}, &result)) {
        return 0;
    }
    return result;
}

u32 remote_import_handle(
    pid_t pid, std::string_view descriptor, int transferred_fd) {
    // Consume transferred_fd even on early failure so callers can pass ownership.
    struct FdCloser {
        int fd;
        ~FdCloser() {
            if (fd >= 0) ::close(fd);
        }
    } closer{transferred_fd};

    u64 result = 0;
    const auto input = std::span<const Byte>(
        reinterpret_cast<const Byte*>(descriptor.data()), descriptor.size());
    const int fd = connect_to(pid);
    if (fd < 0) return 0;
    const Request request{
        kMagic, kVersion, static_cast<u32>(Operation::ImportHandle), 0,
        0, static_cast<u64>(input.size()), 0, 0};
    bool ok = send_all(fd, &request, sizeof(request));
    if (ok && !input.empty()) {
        ok = send_all(fd, input.data(), input.size());
    }
    if (ok && (descriptor.starts_with("P,") || descriptor.starts_with("F,"))) {
        if (closer.fd < 0) {
            ok = false;
        } else {
            ok = send_fd(fd, closer.fd);
            ::close(closer.fd);
            closer.fd = -1;
        }
    }
    Response response{};
    if (ok) ok = receive_all(fd, &response, sizeof(response));
    ::close(fd);
    if (!ok || response.magic != kMagic || response.version != kVersion ||
        response.success == 0) {
        return 0;
    }
    result = response.value;
    return static_cast<u32>(result);
}

std::optional<std::string> remote_export_handle(
    pid_t pid, u32 source_handle, u32 desired_access, bool same_access,
    bool /*close_source*/, int* out_transferred_fd) {
    constexpr u32 DUPLICATE_SAME_ACCESS = 0x00000002;
    constexpr usize kMaxDescriptor = 2048;
    if (out_transferred_fd != nullptr) *out_transferred_fd = -1;

    // CLOSE_SOURCE is applied by the caller via remote_close_handle after
    // import (see Operation::CloseHandle). Passing it here is ignored so a
    // last-handle anonymous section survives until the importer attaches.
    const u32 options = same_access ? DUPLICATE_SAME_ACCESS : 0u;
    const int fd = connect_to(pid);
    if (fd < 0) return std::nullopt;

    const Request request{
        kMagic, kVersion, static_cast<u32>(Operation::ExportHandle),
        desired_access, static_cast<GuestAddr>(source_handle),
        static_cast<u64>(kMaxDescriptor), options, 0};
    bool ok = send_all(fd, &request, sizeof(request));
    Response response{};
    if (ok) ok = receive_all(fd, &response, sizeof(response));
    std::array<Byte, kMaxDescriptor> buffer{};
    if (ok && response.magic == kMagic && response.version == kVersion &&
        response.success != 0 && response.transferred > 0 &&
        response.transferred <= kMaxDescriptor) {
        ok = receive_all(fd, buffer.data(),
                         static_cast<usize>(response.transferred));
    } else {
        ok = false;
    }
    std::optional<std::string> descriptor;
    if (ok) {
        descriptor = std::string(
            reinterpret_cast<const char*>(buffer.data()),
            static_cast<usize>(response.transferred));
        if (descriptor->starts_with("P,") || descriptor->starts_with("F,")) {
            int transferred = -1;
            if (!receive_fd(fd, transferred)) {
                if (transferred >= 0) ::close(transferred);
                descriptor.reset();
            } else if (out_transferred_fd != nullptr) {
                *out_transferred_fd = transferred;
            } else {
                ::close(transferred);
            }
        }
    }
    ::close(fd);
    return descriptor;
}

bool remote_close_handle(pid_t pid, u32 handle) {
    if (pid <= 0 || handle == 0) return false;
    return transact(pid, Operation::CloseHandle,
                    static_cast<GuestAddr>(handle), 0, 0, 0, {}, {});
}

bool remote_present_window_pixels(
    pid_t owner_host_pid, u32 hwnd, std::span<const Byte> rgba, i32 width,
    i32 height) {
    if (owner_host_pid <= 0 || hwnd == 0 || width <= 0 || height <= 0 ||
        rgba.size() !=
            static_cast<usize>(width) * static_cast<usize>(height) * 4u) {
        return false;
    }
    const GuestAddr dims =
        static_cast<GuestAddr>(static_cast<u32>(width)) |
        (static_cast<GuestAddr>(static_cast<u32>(height)) << 32);
    return transact(owner_host_pid, Operation::PresentWindowPixels, dims,
                    rgba.size(), hwnd, 0, rgba, {});
}

bool remote_present_window_surface(
    pid_t owner_host_pid, u32 hwnd, u64 shared_handle, i32 width, i32 height,
    u32 sync_interval, u32 src_x, u32 src_y) {
    if (owner_host_pid <= 0 || hwnd == 0 || shared_handle == 0 ||
        width <= 0 || height <= 0 || src_x > 0xFFFFu || src_y > 0xFFFFu) {
        return false;
    }
    const u64 dims = static_cast<u64>(static_cast<u32>(width)) |
                     (static_cast<u64>(static_cast<u32>(height)) << 32);
    // argument2 carries the guest's IDXGISwapChain::Present SyncInterval. It
    // has to cross the process boundary because the OWNER holds the layer:
    // dropping it here is what made every hosted child window display-synced
    // regardless of what its guest asked for.
    // `reserved` carries the source origin of the presented rectangle
    // inside the shared texture (16 bits each); `size` is the presented
    // size. Without the origin the owner shows a pooled chain in full.
    const u32 origin = (src_x & 0xFFFFu) | ((src_y & 0xFFFFu) << 16);
    return transact(owner_host_pid, Operation::PresentWindowSurface,
                    shared_handle, dims, hwnd, sync_interval, {}, {},
                    nullptr, nullptr, origin);
}

bool remote_refresh_window_pixels(pid_t owner_host_pid, u32 hwnd) {
    if (owner_host_pid <= 0 || hwnd == 0) return false;
    return transact(owner_host_pid, Operation::RefreshWindowPixels, 0, 0,
                    hwnd, 0, {}, {});
}

bool remote_set_window_pos(pid_t owner_host_pid, u32 hwnd, u32 insert_after,
                           i32 x, i32 y, i32 cx, i32 cy, u32 flags) {
    if (owner_host_pid <= 0 || hwnd == 0) return false;
    const GuestAddr after_x =
        static_cast<GuestAddr>(insert_after) |
        (static_cast<GuestAddr>(static_cast<u32>(x)) << 32);
    const u64 y_cx = static_cast<u64>(static_cast<u32>(y)) |
                     (static_cast<u64>(static_cast<u32>(cx)) << 32);
    return transact(owner_host_pid, Operation::SetWindowPos, after_x, y_cx,
                    hwnd, flags, {}, {}, nullptr, nullptr,
                    static_cast<u32>(cy));
}

bool remote_show_window(pid_t owner_host_pid, u32 hwnd, i32 cmd, u64& result) {
    if (owner_host_pid <= 0 || hwnd == 0) return false;
    u64 value = 0;
    if (!transact(owner_host_pid, Operation::ShowWindow, 0, 0, hwnd, static_cast<u32>(cmd), {},
                  {}, &value)) {
        return false;
    }
    result = value;
    return true;
}

bool remote_set_parent(pid_t owner_host_pid, u32 child, u32 parent) {
    if (owner_host_pid <= 0 || child == 0) return false;
    return transact(owner_host_pid, Operation::SetParent, 0, 0, child, parent,
                    {}, {});
}

bool remote_hit_test_window(pid_t owner_host_pid, u32 hwnd, i32 screen_x,
                            i32 screen_y, i32& hit) {
    if (owner_host_pid <= 0 || hwnd == 0) return false;
    const u64 point = (static_cast<u64>(static_cast<u16>(screen_x))) |
                      (static_cast<u64>(static_cast<u16>(screen_y)) << 16);
    u64 value = 0;
    if (!transact(owner_host_pid, Operation::HitTestWindow, point, 0, hwnd, 0,
                  {}, {}, &value)) {
        return false;
    }
    hit = static_cast<i32>(static_cast<u32>(value));
    return true;
}

bool remote_send_message(pid_t owner_host_pid, u32 hwnd, u32 message,
                         u64 wparam, u64 lparam, u32 flags, u32 timeout_ms,
                         std::span<const Byte> payload, u32 out_capacity,
                         RemoteSendStatus& status, u64& lresult,
                         std::vector<Byte>& out) {
    if (owner_host_pid <= 0 || hwnd == 0) return false;
    if (sizeof(SendHeader) + payload.size() > kMaxTransfer) return false;
    std::vector<Byte> in(sizeof(SendHeader) + payload.size());
    const SendHeader h{wparam, lparam, flags, timeout_ms, out_capacity, 0};
    std::memcpy(in.data(), &h, sizeof(h));
    if (!payload.empty()) {
        std::memcpy(in.data() + sizeof(h), payload.data(), payload.size());
    }
    const int fd = connect_to(owner_host_pid);
    if (fd < 0) return false;
    const Request request{kMagic, kVersion,
                          static_cast<u32>(Operation::SendWindowMessage), hwnd,
                          0, static_cast<u64>(in.size()), message, 0};
    bool ok = send_all(fd, &request, sizeof(request)) &&
              send_all(fd, in.data(), in.size());
    Response response{};
    if (ok) ok = receive_all(fd, &response, sizeof(response));
    ok = ok && response.magic == kMagic && response.version == kVersion &&
         response.success != 0 && response.transferred <= out_capacity;
    out.clear();
    if (ok && response.transferred > 0) {
        out.resize(static_cast<usize>(response.transferred));
        ok = receive_all(fd, out.data(), out.size());
    }
    ::close(fd);
    if (!ok) return false;
    status = static_cast<RemoteSendStatus>(response.reserved);
    lresult = response.value;
    return true;
}

bool remote_post_message(pid_t owner_host_pid, u32 hwnd, u32 message,
                         u64 wparam, u64 lparam) {
    if (owner_host_pid <= 0 || hwnd == 0) return false;
    return transact(owner_host_pid, Operation::PostWindowMessage, wparam,
                    lparam, hwnd, message, {}, {});
}

bool remote_post_thread_message(pid_t owner_host_pid, u32 message, u64 wparam,
                                u64 lparam) {
    if (owner_host_pid <= 0) return false;
    return transact(owner_host_pid, Operation::PostThreadMessage, wparam,
                    lparam, message, 0, {}, {});
}

} // namespace vitreo::ipc

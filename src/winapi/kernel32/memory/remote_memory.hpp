// SPDX-License-Identifier: LGPL-2.1-only
/// @file remote_memory.hpp
/// @brief Process-local GuestMemory RPC used by Windows cross-process memory APIs.

#pragma once

#include <vitreo/common.hpp>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/types.h>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {
class FileMappingManager;
class FileManager;
class SyncManager;
class VirtualMemoryManager;
class HandleTable;
}

namespace vitreo::ipc {

/// Present RGBA pixels into a window owned by this process (cross-process GDI
/// flush from a peer that painted via GetDC/StretchDIBits on our HWND).
using WindowPresentHandler =
    std::function<bool(u32 hwnd, std::span<const Byte> rgba, i32 width,
                       i32 height)>;
/// `sync_interval` is the guest's IDXGISwapChain::Present SyncInterval,
/// forwarded because the layer lives in the OWNER process.
/// width/height: the presented size; src_x/src_y: where that rectangle
/// sits in the shared texture (a composition clip), zero for the whole.
using WindowSurfacePresentHandler =
    std::function<bool(u32 hwnd, u64 shared_handle, i32 width, i32 height,
                       u32 sync_interval, u32 src_x, u32 src_y)>;
using WindowRefreshHandler = std::function<bool(u32 hwnd)>;
/// WM_NCHITTEST on behalf of a peer that does not own the window under the
/// cursor. Returns a Win32 HT* code.
using WindowHitTestHandler =
    std::function<i32(u32 hwnd, i32 screen_x, i32 screen_y)>;
/// Apply SetWindowPos on the process that owns @p hwnd (Learn: HWND is
/// desktop-global; SWP from a peer is sent to the owner thread).
using WindowSetWindowPosHandler = std::function<bool(
    u32 hwnd, u32 insert_after, i32 x, i32 y, i32 cx, i32 cy, u32 flags)>;
/// Apply SetParent on the process that owns the child HWND.
using WindowSetParentHandler = std::function<bool(u32 child, u32 parent)>;
/// ShowWindow on a window this process owns, run on its owner thread
/// (Learn: an HWND is valid system-wide). Returns ShowWindow's result, or
/// nullopt when the window is not (or no longer) valid here.
using WindowShowHandler = std::function<std::optional<u64>(u32 hwnd, i32 cmd)>;
/// Post an HWND-less message to this process's primary guest thread.
using ThreadMessageHandler =
    std::function<bool(u32 message, u64 wparam, u64 lparam)>;

/// PostMessage from a peer to a window this process owns; false when the
/// window is not (or no longer) valid here.
using WindowPostMessageHandler =
    std::function<bool(u32 hwnd, u32 message, u64 wparam, u64 lparam)>;

/// Outcome of a cross-process SendMessage/SendMessageTimeout.
enum class RemoteSendStatus : u32 {
    Completed = 0,
    TimedOut = 1,      ///< uTimeout elapsed or SMTO_ABORTIFHUNG
    TargetGone = 2,    ///< window destroyed (SMTO_ERRORONEXIT / no window)
    Unsupported = 3,   ///< message needs marshaling Vitreo does not do
};
/// Receiver side: deliver `message` to `hwnd` (owned by this process).
/// `payload` carries marshaled pointer data (see user32
/// cross_process_send.hpp); `out` receives data copied back to the sender.
using WindowSendMessageHandler = std::function<RemoteSendStatus(
    u32 hwnd, u32 message, u64 wparam, u64 lparam, u32 flags, u32 timeout_ms,
    std::span<const Byte> payload, u32 out_capacity, u64& lresult,
    std::vector<Byte>& out)>;
using GuestCallHandler = std::function<std::optional<u64>(
    GuestAddr function, std::span<const u64> arguments,
    bool caller_elevated)>;
using ComInterfaceMarshalHandler = std::function<GuestAddr(
    u32 owner_pid, GuestAddr object, u32 pointer_size,
    std::span<const Byte> iid)>;


/// Pointer-width-independent representation of MEMORY_BASIC_INFORMATION used
/// by the process-local memory RPC. The caller serializes this into the ABI
/// layout required by its own guest bitness.
struct RemoteMemoryBasicInformation {
    u64 base_address{0};
    u64 allocation_base{0};
    u64 region_size{0};
    u32 allocation_protect{0};
    u32 state{0};
    u32 protect{0};
    u32 type{0};
};

static_assert(sizeof(RemoteMemoryBasicInformation) == 40);

class RemoteMemoryServer {
public:
    RemoteMemoryServer(
        GuestMemory& memory, winapi::VirtualMemoryManager& virtual_memory,
        winapi::SyncManager* sync_manager = nullptr,
        winapi::FileMappingManager* file_mapping_manager = nullptr,
        winapi::HandleTable* handle_table = nullptr,
        winapi::FileManager* file_manager = nullptr);
    ~RemoteMemoryServer();

    RemoteMemoryServer(const RemoteMemoryServer&) = delete;
    RemoteMemoryServer& operator=(const RemoteMemoryServer&) = delete;

    [[nodiscard]] bool start(bool initially_suspended);
    void wait_for_initial_resume();

    /// Optional: handle PresentWindowPixels RPCs from peer GPU processes.
    void set_window_present_handler(WindowPresentHandler handler) {
        m_window_present = std::move(handler);
    }
    /// Optional: present an IOSurface-backed GPU texture from a peer process.
    void set_window_surface_present_handler(
        WindowSurfacePresentHandler handler) {
        m_window_surface_present = std::move(handler);
    }
    /// Optional: replay the latest presentation for an HWND after a peer
    /// changes its cross-process desktop relationship (for example SetParent).
    void set_window_hit_test_handler(WindowHitTestHandler handler) {
        m_window_hit_test = std::move(handler);
    }
    void set_window_refresh_handler(WindowRefreshHandler handler) {
        m_window_refresh = std::move(handler);
    }
    void set_window_set_window_pos_handler(WindowSetWindowPosHandler handler) {
        m_window_set_window_pos = std::move(handler);
    }
    void set_window_set_parent_handler(WindowSetParentHandler handler) {
        m_window_set_parent = std::move(handler);
    }
    void set_window_show_handler(WindowShowHandler handler) {
        m_window_show = std::move(handler);
    }
    void set_thread_message_handler(ThreadMessageHandler handler) {
        m_thread_message = std::move(handler);
    }
    void set_window_send_message_handler(WindowSendMessageHandler handler) {
        m_window_send_message = std::move(handler);
    }
    void set_window_post_message_handler(WindowPostMessageHandler handler) {
        m_window_post_message = std::move(handler);
    }
    /// Execute a synchronous call on a guest service thread owned by this
    /// process. Guest pointers remain meaningful only to this endpoint's
    /// address space.
    void set_guest_call_handler(GuestCallHandler handler) {
        m_guest_call = std::move(handler);
    }
    /// Materialize a process-local proxy for an interface owned by a peer.
    /// The IID is an opaque 16-byte Windows GUID in guest byte order.
    void set_com_interface_marshal_handler(
        ComInterfaceMarshalHandler handler) {
        m_com_interface_marshal = std::move(handler);
    }

private:
    void serve();

    GuestMemory& m_memory;
    winapi::VirtualMemoryManager& m_virtual_memory;
    winapi::SyncManager* m_sync_manager{nullptr};
    winapi::FileMappingManager* m_file_mapping_manager{nullptr};
    winapi::HandleTable* m_handle_table{nullptr};
    winapi::FileManager* m_file_manager{nullptr};
    WindowPresentHandler m_window_present;
    WindowSurfacePresentHandler m_window_surface_present;
    WindowRefreshHandler m_window_refresh;
    WindowHitTestHandler m_window_hit_test;
    WindowSetWindowPosHandler m_window_set_window_pos;
    WindowSetParentHandler m_window_set_parent;
    WindowShowHandler m_window_show;
    ThreadMessageHandler m_thread_message;
    WindowSendMessageHandler m_window_send_message;
    WindowPostMessageHandler m_window_post_message;
    GuestCallHandler m_guest_call;
    ComInterfaceMarshalHandler m_com_interface_marshal;
    int m_listener{-1};
    std::string m_path;
    std::thread m_thread;
    std::mutex m_resume_mutex;
    std::condition_variable m_resume_cv;
    bool m_initially_suspended{false};
    bool m_resumed{false};
};

[[nodiscard]] bool remote_read_process_memory(
    pid_t pid, GuestAddr address, std::span<Byte> output);
[[nodiscard]] bool remote_write_process_memory(
    pid_t pid, GuestAddr address, std::span<const Byte> input);
[[nodiscard]] GuestAddr remote_virtual_alloc(
    pid_t pid, GuestAddr address, u64 size, u32 allocation_type, u32 protect);
[[nodiscard]] bool remote_virtual_free(
    pid_t pid, GuestAddr address, u64 size, u32 free_type);
[[nodiscard]] bool remote_virtual_protect(
    pid_t pid, GuestAddr address, u64 size, u32 new_protect, u32& old_protect);
[[nodiscard]] bool remote_virtual_query(
    pid_t pid, GuestAddr address, u64 maximum_address,
    RemoteMemoryBasicInformation& information);
[[nodiscard]] bool remote_resume_primary_thread(pid_t pid);
/// True when @p pid has already published its process-local RPC endpoint.
/// Unlike normal CREATE_SUSPENDED operations this probe never waits for a
/// future endpoint to appear.
[[nodiscard]] bool remote_process_endpoint_available(pid_t pid);
/// Execute @p function in @p pid's guest address space using that process's
/// native guest calling convention.
[[nodiscard]] std::optional<u64> remote_call_guest(
    pid_t pid, GuestAddr function, std::span<const u64> arguments,
    bool caller_elevated = false);
/// Ask @p pid to create a local COM proxy for @p object owned by
/// @p owner_pid. The returned pointer is meaningful only in @p pid.
[[nodiscard]] GuestAddr remote_marshal_com_interface(
    pid_t pid, u32 owner_pid, GuestAddr object, u32 owner_pointer_size,
    std::span<const Byte> iid);
/// Ask @p pid to export a descriptor for its local handle @p source_handle so
/// the caller can import it (DuplicateHandle of a remote handle). @p close_source is ignored here — apply
/// DUPLICATE_CLOSE_SOURCE with remote_close_handle after a successful import so
/// SharedMemory lifetime survives last-handle transfers without MapViewOfFile.
/// For pipe descriptors (`P,...`), @p out_transferred_fd receives the host FD
/// delivered via SCM_RIGHTS; the caller must close it if unused / after import.
[[nodiscard]] std::optional<std::string> remote_export_handle(
    pid_t pid, u32 source_handle, u32 desired_access, bool same_access,
    bool close_source, int* out_transferred_fd = nullptr);

/// Close @p handle inside @p pid's handle table (DUPLICATE_CLOSE_SOURCE for
/// FROM-remote DuplicateHandle after the local import has attached).
[[nodiscard]] bool remote_close_handle(pid_t pid, u32 handle);

/// Import a serialized sync/section/pipe object into a peer process and return
/// the HANDLE value allocated there. For pipe descriptors (`P,...`) pass the
/// host FD in @p transferred_fd (sent via SCM_RIGHTS); otherwise leave -1.
/// Ownership of @p transferred_fd is consumed (closed after send).
[[nodiscard]] u32 remote_import_handle(
    pid_t pid, std::string_view descriptor, int transferred_fd = -1);

/// Deliver a window framebuffer to the Vitreo host process that owns @p hwnd.
/// Used when GDI present runs in a peer process.
[[nodiscard]] bool remote_present_window_pixels(
    pid_t owner_host_pid, u32 hwnd, std::span<const Byte> rgba, i32 width,
    i32 height);

/// Present an IOSurface-backed GPU texture into a peer-owned window without
/// transferring framebuffer bytes through the CPU RPC channel.
[[nodiscard]] bool remote_present_window_surface(
    pid_t owner_host_pid, u32 hwnd, u64 shared_handle, i32 width, i32 height,
    u32 sync_interval,
    u32 src_x = 0, u32 src_y = 0);

/// Ask the process that owns @p hwnd to replay its current presented surface.
/// Vitreo guest processes are separate AppKit processes, so cross-process
/// SetParent needs this explicit recomposition notification.
/// Ask the process that owns @p hwnd to hit-test a screen point.
[[nodiscard]] bool remote_hit_test_window(pid_t owner_host_pid, u32 hwnd,
                                          i32 screen_x, i32 screen_y,
                                          i32& hit);

[[nodiscard]] bool remote_refresh_window_pixels(pid_t owner_host_pid,
                                                u32 hwnd);

/// Apply SetWindowPos on the host process that owns @p hwnd.
[[nodiscard]] bool remote_set_window_pos(pid_t owner_host_pid, u32 hwnd,
                                         u32 insert_after, i32 x, i32 y,
                                         i32 cx, i32 cy, u32 flags);

/// Apply SetParent on the host process that owns @p child.
[[nodiscard]] bool remote_set_parent(pid_t owner_host_pid, u32 child,
                                     u32 parent);
/// ShowWindow on a window the peer owns; @p result is ShowWindow's return.
/// False when the peer is unreachable or no longer owns the window.
[[nodiscard]] bool remote_show_window(pid_t owner_host_pid, u32 hwnd, i32 cmd,
                                      u64& result);
/// SendMessage(Timeout) to a window owned by the peer process. Returns false
/// when the peer could not be reached; otherwise `status`/`lresult`/`out`
/// carry the receiver's outcome.
[[nodiscard]] bool remote_send_message(pid_t owner_host_pid, u32 hwnd,
                                       u32 message, u64 wparam, u64 lparam,
                                       u32 flags, u32 timeout_ms,
                                       std::span<const Byte> payload,
                                       u32 out_capacity,
                                       RemoteSendStatus& status, u64& lresult,
                                       std::vector<Byte>& out);

/// PostMessage to a window owned by the peer process. False when the peer
/// could not be reached or the window is not valid there.
[[nodiscard]] bool remote_post_message(pid_t owner_host_pid, u32 hwnd,
                                       u32 message, u64 wparam, u64 lparam);

/// Post an HWND-less message to a peer's advertised primary thread.
[[nodiscard]] bool remote_post_thread_message(pid_t owner_host_pid, u32 message,
                                              u64 wparam, u64 lparam);

} // namespace vitreo::ipc

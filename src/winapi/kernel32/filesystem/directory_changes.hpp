#pragma once
/// @file directory_changes.hpp
/// @brief ReadDirectoryChangesW: per-directory-handle change tracking over
///        macOS FSEvents (file-level events).
///
/// Contract: Microsoft Learn (ReadDirectoryChangesW, FILE_NOTIFY_INFORMATION)
/// plus tools/oracle_probe/read_dir_changes_probe{32,64}.expected (x86 = x64):
///   - Tracking starts with the first call on a handle; changes made while no
///     request is pending are kept (up to the first call's buffer length) and
///     complete the next request at once. More than that, or a request buffer
///     too small for the records, completes with 0 bytes and
///     STATUS_NOTIFY_ENUM_DIR (0x10C, a success: ERROR_NOTIFY_ENUM_DIR).
///   - Records: FILE_ACTION_ADDED 1, REMOVED 2, MODIFIED 3, RENAMED_OLD_NAME
///     4 followed by RENAMED_NEW_NAME 5; names relative to the directory,
///     '\'-separated; NextEntryOffset DWORD-aligned, the last record 0 and
///     unpadded in the byte count. Without bWatchSubtree, changes inside
///     subdirectories are not reported at all.
///   - CancelIo/CancelIoEx complete a pending request with STATUS_CANCELLED;
///     closing the handle completes it with STATUS_NOTIFY_CLEANUP (0x10B), 0
///     bytes.

#include <vitreo/common.hpp>

#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

namespace dirchange {

constexpr u32 FILE_ACTION_ADDED = 1, FILE_ACTION_REMOVED = 2, FILE_ACTION_MODIFIED = 3,
              FILE_ACTION_RENAMED_OLD_NAME = 4, FILE_ACTION_RENAMED_NEW_NAME = 5;
// winnt.h FILE_NOTIFY_CHANGE_*.
constexpr u32 FILE_NOTIFY_CHANGE_FILE_NAME = 0x1, FILE_NOTIFY_CHANGE_DIR_NAME = 0x2,
              FILE_NOTIFY_CHANGE_ATTRIBUTES = 0x4, FILE_NOTIFY_CHANGE_SIZE = 0x8,
              FILE_NOTIFY_CHANGE_LAST_WRITE = 0x10, FILE_NOTIFY_CHANGE_LAST_ACCESS = 0x20,
              FILE_NOTIFY_CHANGE_CREATION = 0x40, FILE_NOTIFY_CHANGE_SECURITY = 0x100;
constexpr u32 kValidFilter = 0x17F;
constexpr u32 STATUS_NOTIFY_CLEANUP = 0x0000010Bu, STATUS_NOTIFY_ENUM_DIR = 0x0000010Cu,
              STATUS_CANCELLED = 0xC0000120u;
constexpr u32 ERROR_NOTIFY_CLEANUP = 745, ERROR_NOTIFY_ENUM_DIR = 1022,
              ERROR_OPERATION_ABORTED = 995;

struct Record {
    u32 action = 0;
    std::string name;   ///< UTF-8, '\'-separated, relative to the directory
    bool operator==(const Record&) const = default;
};

/// One host file event (FSEvents, file-level) in testable form.
struct HostEvent {
    std::string relative;   ///< '/'-separated path below the watched directory
    bool is_directory = false;
    bool created = false, removed = false, renamed = false;
    bool content_modified = false, metadata_modified = false;
    bool exists_now = false;   ///< lstat after the event
    bool born_after_start = true;   ///< birth time at or after tracking began
};

/// Translate a batch of host events into Windows records for @p filter /
/// @p subtree. @p added tracks names already reported as added, so FSEvents'
/// sticky "created" flag on later events does not repeat FILE_ACTION_ADDED.
[[nodiscard]] std::vector<Record> translate(const std::vector<HostEvent>& events,
                                            bool subtree, u32 filter,
                                            std::vector<std::string>& added);

/// Serialize records as FILE_NOTIFY_INFORMATION (UTF-16 names). nullopt when
/// they do not fit in @p capacity bytes.
[[nodiscard]] std::optional<std::vector<u8>> serialize(const std::vector<Record>& records,
                                                       u32 capacity);

}  // namespace dirchange

class DirectoryChangeService {
public:
    struct Completion {
        /// Writes OVERLAPPED Internal/InternalHigh, signals the event and
        /// posts to an associated completion port.
        std::function<void(u32 handle, u32 bytes, GuestAddr overlapped, bool is_64bit,
                           u32 nt_status, u32 win_error)> overlapped;
        /// Queues a LPOVERLAPPED_COMPLETION_ROUTINE APC on @p thread_id.
        std::function<void(u32 thread_id, GuestAddr routine, u32 win_error, u32 bytes,
                           GuestAddr overlapped)> routine;
    };

    struct Request {
        u32 handle = 0;
        std::string host_directory;
        GuestAddr buffer = 0;
        u32 length = 0;
        bool subtree = false;
        u32 filter = 0;
        GuestAddr overlapped = 0;
        GuestAddr routine = 0;
        u32 thread_id = 0;
        bool is_64bit = true;
    };

    DirectoryChangeService(GuestMemory& memory, Completion completion);
    ~DirectoryChangeService();
    DirectoryChangeService(const DirectoryChangeService&) = delete;
    DirectoryChangeService& operator=(const DirectoryChangeService&) = delete;

    /// Queue an asynchronous request (completes now if changes are waiting).
    /// False when host tracking could not start.
    [[nodiscard]] bool start(const Request& request);
    /// Synchronous request: blocks until changes, cancel or close. Returns the
    /// NTSTATUS and the byte count.
    [[nodiscard]] u32 wait(const Request& request, u32& bytes);
    /// CancelIo / CancelIoEx: complete matching pending requests with
    /// STATUS_CANCELLED. @p overlapped 0 = any; @p thread_id 0 = any issuer.
    usize cancel(u32 handle, GuestAddr overlapped, u32 thread_id);
    /// CloseHandle: complete pending requests with STATUS_NOTIFY_CLEANUP and
    /// stop tracking.
    void close(u32 handle);
    /// Feed host events as the FSEvents stream would (tests).
    void deliver(u32 handle, const std::vector<dirchange::HostEvent>& events);

private:
    struct Watch;
    std::shared_ptr<Watch> watch_for(const Request& request, bool& created);
    bool ensure_stream(const std::shared_ptr<Watch>& watch);
    void pump(const std::shared_ptr<Watch>& watch);

    GuestMemory& m_memory;
    Completion m_completion;
    std::mutex m_mutex;
    std::map<u32, std::shared_ptr<Watch>> m_watches;
};

}  // namespace vitreo::winapi

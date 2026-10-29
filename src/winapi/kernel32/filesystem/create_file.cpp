/// @file file.cpp
/// @brief CreateFileW / CreateFileA / ReadFile / WriteFile implementation (WIN32-010/011)

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/filesystem/file_pipe_internal.hpp"
#include "winapi/kernel32/filesystem/file_thunks_internal.hpp"
#include "winapi/kernel32/filesystem/file_profile_internal.hpp"
#include "winapi/kernel32/filesystem/file_path_internal.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/filepos.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/system/virtual_firmware.hpp"
#include "winapi/core/thunk_registry.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "winapi/common/write_sha1_debug.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/ipc_name.hpp"
#include "ipc/unix_pipe.hpp"

#include <algorithm>
#include <vector>
#include <array>
#include <deque>
#include <memory>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/// @file create_file.cpp
/// @brief FileManager::create_file
///
/// Closed method cut from file.cpp.

namespace vitreo::winapi {

bool FileManager::is_character_file(Handle handle) const {
    auto fd = m_vfs.duplicate_native_fd(vfs_handle_for(handle));
    if (fd.is_err()) return false;
    struct stat info{};
    const bool character = ::fstat(fd.value(), &info) == 0 && S_ISCHR(info.st_mode);
    ::close(fd.value());
    return character;
}

u32 FileManager::file_type(Handle handle) const {
    constexpr u32 kUnknown = 0, kDisk = 1, kChar = 2, kPipe = 3;
    if (handle == NULL_HANDLE) return kUnknown;
    switch (m_handles.get_type(handle)) {
        case HandleType::File:
            return is_character_file(handle) ? kChar : kDisk;
        case HandleType::Device:
            // Volume handles (`\\.\X:`) are FILE_TYPE_DISK
            // (files_core_probe GetFileType_volume).
            return kDisk;
        case HandleType::ConsoleInput:
        case HandleType::ConsoleOutput:
            return kChar;
        case HandleType::Pipe:
        case HandleType::Mailslot:
            return kPipe;
        case HandleType::Invalid:
            // Winsock SOCKETs live outside HandleTable; Learn:
            // FILE_TYPE_PIPE covers sockets (GetFileType_socket).
            return socket_handle_is_known(static_cast<u32>(handle)) ? kPipe
                                                                     : kUnknown;
        default:
            return kUnknown;
    }
}

Handle FileManager::adopt_vfs_handle(capsule::FileHandle vfs_handle,
                                     std::string_view path,
                                     u32 desired_access, bool owns_vfs) {
    if (vfs_handle == capsule::INVALID_FILE_HANDLE) return INVALID_HANDLE;
    const Handle handle =
        m_handles.allocate(HandleType::File, nullptr, 0, desired_access);
    if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
        return INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> lock(m_file_path_mutex);
    m_vfs_handles[handle] = vfs_handle;
    if (!owns_vfs) m_unowned_vfs_handles.insert(handle);
    if (!path.empty()) {
        m_handle_paths[handle] = std::string(path);
        m_path_open_counts[std::string(path)] += 1;
    }
    return handle;
}

namespace {
/// FILE_READ_ATTRIBUTES (winnt.h), granted by every CreateFile.
constexpr u32 kFileReadAttributesRight = 0x0080u;
}  // namespace

Handle FileManager::create_file(
    std::string_view path,
    u32 desired_access,
    u32 share_mode,
    u32 creation_disposition,
    u32 flags_and_attrs)
{
    // DOS reserves NUL (including extensions) independently of disk files.
    // Preserve real kernel I/O and descriptor inheritance using the host null
    // device; never create a capsule file named NUL.
    const auto device_name = dos_device_name(path);
    std::string_view leaf = path.substr(path.find_last_of("\\/") == std::string_view::npos
        ? 0 : path.find_last_of("\\/") + 1);
    const auto dot = leaf.find('.');
    const std::string leaf_lower = ascii_lower(leaf);
    // CONIN$ / CONOUT$ / CON — open the process console even when std handles
    // are redirected (process_contract_probe.expected "console state").
    const bool con_device =
        leaf_lower == "conin$" || leaf_lower == "conout$" || leaf_lower == "con" ||
        (device_name && (*device_name == "conin$" || *device_name == "conout$" ||
                         *device_name == "con"));
    if (con_device) {
        if (m_console == nullptr || !m_console->is_attached()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return INVALID_HANDLE;
        }
        const bool want_input =
            leaf_lower == "conin$" ||
            (device_name && *device_name == "conin$") ||
            ((leaf_lower == "con" || (device_name && *device_name == "con")) &&
             (desired_access & 0x80000000u) != 0 &&
             (desired_access & 0x40000000u) == 0);
        const Handle obj = want_input
            ? m_console->console_input_handle()
            : m_console->console_output_handle(/*error_stream=*/false);
        if (obj == NULL_HANDLE) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return INVALID_HANDLE;
        }
        const Handle dup = m_console->duplicate_handle(obj, 0, desired_access);
        if (dup == NULL_HANDLE) {
            return INVALID_HANDLE;
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        return dup;
    }
    const bool null_name = ascii_lower(leaf.substr(0, dot)) == "nul";
    const bool null_device = (device_name && *device_name == "nul") ||
        (!path.starts_with("\\\\?\\") && !path.starts_with("\\\\.\\") && null_name);
    if (null_device) {
        const auto access = to_access_mode(desired_access);
        int mode = O_RDONLY;
        if (desired_access & (0x40000000u | 0x2u | 0x4u))
            mode = (desired_access & (0x80000000u | 0x1u)) ? O_RDWR : O_WRONLY;
        const int fd = ::open("/dev/null", mode | O_CLOEXEC);
        if (fd < 0) {
            set_last_error(error_codes::ERROR_ACCESS_DENIED);
            return INVALID_HANDLE;
        }
        auto adopted = m_vfs.adopt_native_fd(fd, access, capsule::ShareMode::All, "NUL");
        if (adopted.is_err()) {
            ::close(fd);
            set_last_error(vfs_error_to_win32(adopted.error()));
            return INVALID_HANDLE;
        }
        const Handle handle = m_handles.allocate(HandleType::File, nullptr, 0,
                                                 desired_access | kFileReadAttributesRight);
        if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
            (void)m_vfs.close(adopted.value());
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return INVALID_HANDLE;
        }
        {
            std::lock_guard lock(m_file_path_mutex);
            m_vfs_handles[handle] = adopted.value();
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        return handle;
    }

    if (auto device = dos_device_name(path)) {
        // Volume device path "\\.\X:" — opens the mounted drive as a raw
        // volume handle (CreateFile contract). SecuROM and other CD checks
        // open "\\.\D:" / "\\.\E:" then DeviceIoControl the handle.
        if (device->size() == 2 && (*device)[1] == ':' &&
            capsule::is_valid_drive_letter((*device)[0])) {
            const char letter =
                static_cast<char>(std::toupper(static_cast<unsigned char>((*device)[0])));
            if (!m_vfs.is_drive_mounted(letter)) {
                set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
                return INVALID_HANDLE;
            }
            // Volumes are opened with OPEN_EXISTING / OPEN_ALWAYS only.
            if (creation_disposition != OPEN_EXISTING &&
                creation_disposition != OPEN_ALWAYS) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                return INVALID_HANDLE;
            }
            auto* object = new std::string(std::string("vol:") +
                                           static_cast<char>(std::tolower(
                                               static_cast<unsigned char>(letter))));
            const Handle handle = m_handles.allocate(
                HandleType::Device, object, 0, desired_access);
            if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
                delete object;
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return INVALID_HANDLE;
            }
            VITREO_TRACE("[device] CreateFile('%.*s') -> volume '%c:' "
                         "handle=0x%x\n",
                         static_cast<int>(path.size()), path.data(), letter,
                         handle);
            return handle;
        }

        std::lock_guard<std::mutex> lock(m_device_mutex);
        if (m_running_kernel_devices.contains(*device)) {
            auto* object = new std::string(*device);
            const Handle handle = m_handles.allocate(
                HandleType::Device, object, 0, desired_access);
            if (handle == NULL_HANDLE || handle == INVALID_HANDLE) {
                delete object;
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return INVALID_HANDLE;
            }
            VITREO_TRACE("[device] CreateFile('%.*s') -> device '%s' "
                         "handle=0x%x\n", static_cast<int>(path.size()),
                         path.data(), device->c_str(), handle);
            return handle;
        }
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE;
    }

    // Cross-process named pipe client: CreateFile("\\.\pipe\X") connects to a
    // peer process's named-pipe server (AF_UNIX socket).
    if (is_named_pipe_path(path)) {
        const auto config = read_pipe_metadata(path);
        if (!config) {
            ipc::ipc_log(
                "CreateFile pipe '%.*s' -> missing/invalid server metadata",
                static_cast<int>(path.size()), path.data());
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE;
        }
        auto client = ipc::UnixPipe::connect_client(std::string(path));
        if (!client) {
            ipc::ipc_log("CreateFile pipe '%.*s' -> no-server",
                         static_cast<int>(path.size()), path.data());
            VITREO_TRACE("[file] CreateFile named pipe '%.*s' -> no-server\n",
                         static_cast<int>(path.size()), path.data());
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return INVALID_HANDLE;
        }
        auto ep = std::make_shared<PipeEndpoint>();
        ep->readable = true;
        ep->writable = true;
        ep->conn = std::move(client);
        // A client opened by name keeps that name: GetNamedPipeInfo and the
        // inheritance descriptor both report it, and an empty name previously
        // made this endpoint indistinguishable from an anonymous pipe.
        ep->win_name = std::string(path);
        ep->kind = PipeKind::NamedClient;
        ep->pipe_type =
            config->message_type ? PipeDataMode::Message : PipeDataMode::Byte;
        // Windows always opens a local client in byte-read, blocking mode.
        ep->read_mode = PipeDataMode::Byte;
        ep->blocking = true;
        ep->file_mode = (flags_and_attrs & 0x40000000u) ? 0 : 0x20;
        ep->input_buffer_size = config->input_buffer_size;
        ep->output_buffer_size = config->output_buffer_size;
        ep->max_instances = config->max_instances;
        ep->default_timeout_ms = config->default_timeout_ms;
        // Cache the server guest pid for GetNamedPipeServerProcessId and for
        // inheritance when the child later queries the same client handle.
        if (ep->conn) {
            const int peer = ep->conn->peer_pid();
            if (peer > 0) {
                ep->server_process_id = static_cast<u32>(peer);
            }
        }
        // A pipe client is granted what it asked for plus
        // FILE_READ_ATTRIBUTES, like any CreateFile (file_access_info_probe:
        // GENERIC_WRITE -> 0x120196, GENERIC_READ|GENERIC_WRITE -> 0x12019f).
        const Handle client_handle = allocate_pipe_endpoint(
            m_handles, std::move(ep), 0, desired_access | kFileReadAttributesRight);
        if (client_handle == INVALID_HANDLE || client_handle == NULL_HANDLE) {
            // Returning INVALID_HANDLE_VALUE while last-error still reads
            // ERROR_SUCCESS is the one answer no Win32 failure path may give.
            ipc::ipc_log("CreateFile pipe '%.*s' -> handle allocation failed",
                         static_cast<int>(path.size()), path.data());
            set_last_error(error_codes::ERROR_TOO_MANY_OPEN_FILES);
            return INVALID_HANDLE;
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        // Log the client HANDLE so the two ends of a pipe pair (server vs
        // client) can be correlated without guessing allocation order.
        ipc::ipc_log("CreateFile pipe '%.*s' -> CONNECTED h=0x%x",
                     static_cast<int>(path.size()), path.data(),
                     static_cast<unsigned>(client_handle));
        VITREO_TRACE("[file] CreateFile named pipe '%.*s' -> CONNECTED h=0x%x\n",
                     static_cast<int>(path.size()), path.data(),
                     static_cast<unsigned>(client_handle));
        return client_handle;
    }

    // Learn (CreateFileW dwCreationDisposition): "must be one of the
    // following values, which cannot be combined" — anything else fails
    // ERROR_INVALID_PARAMETER (files_core_probe CreateFileW_disposition_0/6).
    if (creation_disposition < CREATE_NEW || creation_disposition > TRUNCATE_EXISTING) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE;
    }
    // TRUNCATE_EXISTING: "The calling process must open the file with the
    // GENERIC_WRITE bit set" — without it Windows fails 87 and leaves the
    // file alone (TRUNCATE_EXISTING_read_only_access).
    constexpr u32 FILE_WRITE_DATA_BIT = 0x0002;
    constexpr u32 GENERIC_ALL_BIT = 0x10000000u;
    constexpr u32 MAXIMUM_ALLOWED_BIT = 0x02000000u;
    if (creation_disposition == TRUNCATE_EXISTING &&
        (desired_access & (GENERIC_WRITE | GENERIC_ALL_BIT | FILE_WRITE_DATA_BIT)) == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE;
    }

    const auto existing = m_vfs.stat(path);
    const bool exists = existing.is_ok();
    const u32 existing_attrs =
        exists ? static_cast<u32>(existing.value().attributes) : 0u;
    const bool existing_is_dir = exists && (existing_attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    // A trailing separator names a directory: on a file it is not found
    // (files_core_probe file_with_trailing_backslash).
    if (!path.empty() && (path.back() == '\\' || path.back() == '/') &&
        exists && !existing_is_dir) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE;
    }
    // Directories open only with FILE_FLAG_BACKUP_SEMANTICS and only through
    // OPEN_EXISTING / OPEN_ALWAYS; everything else is ERROR_ACCESS_DENIED for
    // an object that exists (files_core_probe dir_*).
    if (existing_is_dir &&
        ((flags_and_attrs & FILE_FLAG_BACKUP_SEMANTICS) == 0 ||
         (creation_disposition != OPEN_EXISTING && creation_disposition != OPEN_ALWAYS))) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return INVALID_HANDLE;
    }
    // Learn (CreateFileW, Files): CREATE_ALWAYS on an existing HIDDEN or
    // SYSTEM file fails ERROR_ACCESS_DENIED unless the caller asks for that
    // attribute too (CREATE_ALWAYS_normal_on_hidden / _hidden_on_hidden).
    constexpr u32 kHidden = 0x2, kSystem = 0x4;
    if (exists && !existing_is_dir && creation_disposition == CREATE_ALWAYS &&
        (((existing_attrs & kHidden) && !(flags_and_attrs & kHidden)) ||
         ((existing_attrs & kSystem) && !(flags_and_attrs & kSystem)))) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        return INVALID_HANDLE;
    }

    // Convert Win32 constants to VFS enums. FILE_FLAG_DELETE_ON_CLOSE opens
    // the file for DELETE whatever the caller asked (files_core_probe
    // doc_without_delete_access succeeds), and that DELETE access is what
    // makes later opens without FILE_SHARE_DELETE fail with a sharing
    // violation (doc_reopen_shareRW).
    auto access = to_access_mode(
        (flags_and_attrs & FILE_FLAG_DELETE_ON_CLOSE) != 0
            ? (desired_access | 0x00010000u /*DELETE*/)
            : desired_access);
    auto share  = to_share_mode(share_mode);
    auto disp   = to_disposition(creation_disposition);
    if (existing_is_dir) disp = capsule::CreationDisposition::OpenExisting;
    // MAXIMUM_ALLOWED grants whatever the caller may have: read and write
    // on a writable file (files_core_probe max_allowed_read / _write).
    const bool maximum_allowed = (desired_access & MAXIMUM_ALLOWED_BIT) != 0;
    if (maximum_allowed) {
        access = access | capsule::AccessMode::ReadWrite | capsule::AccessMode::Delete;
    }

    // A delete-pending object still exists on disk but cannot be reopened:
    // Windows fails the open with STATUS_DELETE_PENDING -> ERROR_ACCESS_DENIED
    // until the last handle closes and it goes away. Reporting it as simply
    // absent tells callers the object was already removed, which is a state
    // Windows never presents while a handle is still open.
    if (is_delete_pending(path)) {
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        VITREO_TRACE("[file] CreateFile('%.*s') refused: delete pending\n",
                     static_cast<int>(path.size()), path.data());
        return INVALID_HANDLE;
    }

    // Learn (CreateFile): CREATE_ALWAYS and OPEN_ALWAYS on an existing file
    // succeed with ERROR_ALREADY_EXISTS; every other successful open of a
    // file sets 0 (measured on Windows 11 for CREATE_NEW, CREATE_ALWAYS,
    // OPEN_ALWAYS, OPEN_EXISTING and TRUNCATE_EXISTING:
    // tools/oracle_probe/last_error_on_success_probe.expected).
    const bool reports_existing =
        (creation_disposition == CREATE_ALWAYS ||
         creation_disposition == OPEN_ALWAYS) && exists;

    // Delegate to VFS
    auto result = m_vfs.open(path, access, share, disp);
    if (result.is_err() && maximum_allowed &&
        result.error() == capsule::VfsError::AccessDenied) {
        access = to_access_mode(desired_access & ~MAXIMUM_ALLOWED_BIT) |
                 capsule::AccessMode::Read;
        result = m_vfs.open(path, access, share, disp);
    }
    if (result.is_err()) {
        set_last_error(vfs_error_to_win32(result.error()));
        // CREATE_NEW + PathAlreadyExists is the *expected* path Win32 takes
        // when callers probe whether a file is already present. The Win32
        // contract is to return INVALID_HANDLE + ERROR_FILE_EXISTS, which we
        // do — so this isn't an error condition,
        // just a normal control-flow signal. Suppress the log to avoid
        // spamming stderr when installers retry the same probe in a loop.
        const bool expected_already_exists =
            (creation_disposition == CREATE_NEW &&
             result.error() == capsule::VfsError::PathAlreadyExists);
        if (!expected_already_exists) {
            VITREO_TRACE(
                         "[file] create_file: vfs.open(%.*s) failed: %s (disp=%u access=0x%x share=0x%x)\n",
                         static_cast<int>(path.size()), path.data(),
                         capsule::vfs_error_to_string(result.error()),
                         creation_disposition, desired_access, share_mode);
        }
        return INVALID_HANDLE;
    }

    capsule::FileHandle vfs_handle = result.value();

    // CreateFile only opens a directory when the caller explicitly requests
    // backup semantics. Host open(2) accepts directories as ordinary file
    // descriptors, so enforce the Windows-visible contract before publishing
    // the handle. Otherwise callers can mistake host directory metadata for
    // file contents and issue reads against a handle Windows would reject.
    const auto info = m_vfs.stat_handle(vfs_handle);
    if (info.is_ok() && capsule::has_attribute(
                            info.value().attributes,
                            capsule::FileAttributes::Directory) &&
        (flags_and_attrs & FILE_FLAG_BACKUP_SEMANTICS) == 0) {
        (void)m_vfs.close(vfs_handle);
        set_last_error(error_codes::ERROR_ACCESS_DENIED);
        VITREO_TRACE(
            "[file] CreateFile('%.*s') rejected directory without "
            "FILE_FLAG_BACKUP_SEMANTICS\n",
            static_cast<int>(path.size()), path.data());
        return INVALID_HANDLE;
    }

    // Allocate a Win32 handle pointing at the VFS file handle storage.
    // CreateFile always grants FILE_READ_ATTRIBUTES on top of the request
    // (file_access_info_probe: FILE_READ_DATA|SYNCHRONIZE -> 0x110081).
    Handle win_handle = m_handles.allocate(HandleType::File, nullptr, 0,
                                           desired_access | kFileReadAttributesRight);
    if (win_handle == NULL_HANDLE || win_handle == INVALID_HANDLE) {
        // Roll back VFS open
        (void)m_vfs.close(vfs_handle);
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE;
    }

    // Track the mapping for later cleanup. Concurrent CreateFile/CloseHandle
    // must not race these containers — std::map emplace on m_path_open_counts
    // was host-crashing under multi-threaded open storms.
    {
        std::lock_guard<std::mutex> lock(m_file_path_mutex);
        m_vfs_handles[win_handle] = vfs_handle;

        // Retain the name so a pending-delete disposition can be carried out at
        // close: the VFS deletes by path, and the caller may never name the file
        // again after CreateFile returns.
        m_handle_paths[win_handle] = std::string(path);
        m_path_open_counts[std::string(path)] += 1;
        if ((flags_and_attrs & FILE_FLAG_DELETE_ON_CLOSE) != 0) {
            // The object becomes delete-pending when THIS handle closes, not
            // now: opens that share delete keep succeeding until then
            // (files_core_probe doc_reopen_shareRWD).
            m_delete_on_close_handles.insert(win_handle);
        }
        // FILE_FLAG_OVERLAPPED (0x40000000) — same bit as GENERIC_WRITE but in
        // dwFlagsAndAttributes. Recorded so ReadFile/WriteFile use async
        // semantics (files_core_probe async_ReadFile_*).
        constexpr u32 kFileFlagOverlapped = 0x40000000u;
        if ((flags_and_attrs & kFileFlagOverlapped) != 0) {
            m_overlapped_file_handles.insert(win_handle);
        }
        // FILE_FLAG_NO_BUFFERING (0x20000000): SetFilePointer(Ex) offsets must
        // be sector-aligned (files_core_probe SFP_nobuf_*).
        constexpr u32 kFileFlagNoBuffering = 0x20000000u;
        if ((flags_and_attrs & kFileFlagNoBuffering) != 0) {
            m_no_buffering_file_handles.insert(win_handle);
        }

        // Learn (CreateFile, FILE_APPEND_DATA): granted without FILE_WRITE_DATA,
        // "write operations will not overwrite existing data" — each write goes
        // to the current end of file irrespective of the file pointer. Several
        // processes rely on this to append to one file concurrently.
        constexpr u32 FILE_WRITE_DATA_RIGHT = 0x0002;
        constexpr u32 FILE_APPEND_DATA_RIGHT = 0x0004;
        if ((desired_access & FILE_APPEND_DATA_RIGHT) != 0 &&
            (desired_access & (FILE_WRITE_DATA_RIGHT | GENERIC_WRITE)) == 0) {
            m_append_only_handles.insert(win_handle);
        }
    }

    if (m_append_only_handles.count(win_handle) != 0) {
        // The file pointer starts at the end so a caller that queries the
        // position before its first write sees the append point.
        (void)m_vfs.seek(vfs_handle, 0, capsule::SeekOrigin::End);
    }

    // Attributes supplied at creation apply to a new (or superseded) file,
    // and Windows adds ARCHIVE (files_core_probe create_readonly_hidden ->
    // 0x23, create_plain_attrs -> 0x20); an existing file opened by
    // OPEN_ALWAYS keeps its own (OPEN_ALWAYS_existing_ignores_attrs).
    const bool created_or_superseded =
        !existing_is_dir &&
        (creation_disposition == CREATE_NEW ||
         creation_disposition == CREATE_ALWAYS ||
         (creation_disposition == OPEN_ALWAYS && !exists));
    if (created_or_superseded) {
        constexpr u32 kCreateAttrMask = 0x3FFF & ~FILE_ATTRIBUTE_DIRECTORY;
        const u32 requested = flags_and_attrs & kCreateAttrMask;
        (void)m_vfs.set_handle_attributes(
            vfs_handle, (requested & ~FILE_ATTRIBUTE_NORMAL) | 0x20u /*ARCHIVE*/);
    }

    debug::write_sha1_open(win_handle, path);
    debug::read_sha1_open(win_handle, path);
    debug::watch_open(win_handle, path);

    set_last_error(reports_existing ? error_codes::ERROR_ALREADY_EXISTS
                                    : error_codes::ERROR_SUCCESS);
    return win_handle;
}

void FileManager::apply_template_attributes(Handle created, Handle template_file) {
    const capsule::FileHandle t = vfs_handle_for(template_file);
    const capsule::FileHandle c = vfs_handle_for(created);
    if (t == capsule::INVALID_FILE_HANDLE || c == capsule::INVALID_FILE_HANDLE) return;
    auto info = m_vfs.stat_handle(t);
    if (info.is_err()) return;
    (void)m_vfs.set_handle_attributes(c, static_cast<u32>(info.value().attributes) |
                                             0x20u /*ARCHIVE*/);
}

Handle FileManager::reopen_file(
    Handle original,
    u32 desired_access,
    u32 share_mode,
    u32 flags_and_attrs)
{
    // Learn (ReOpenFile): the original object must have been created by
    // CreateFile. A missing path means this handle is not a tracked file.
    const std::string path = path_for_handle(original);
    if (path.empty()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        return INVALID_HANDLE;
    }
    return create_file(path, desired_access, share_mode, OPEN_EXISTING,
                       flags_and_attrs);
}

bool FileManager::is_append_only(Handle handle) const {
    std::lock_guard<std::mutex> lock(m_file_path_mutex);
    return m_append_only_handles.count(handle) != 0;
}

bool FileManager::is_overlapped_file(Handle handle) const {
    std::lock_guard<std::mutex> lock(m_file_path_mutex);
    return m_overlapped_file_handles.count(handle) != 0;
}

bool FileManager::is_no_buffering_file(Handle handle) const {
    std::lock_guard<std::mutex> lock(m_file_path_mutex);
    return m_no_buffering_file_handles.count(handle) != 0;
}

} // namespace vitreo::winapi

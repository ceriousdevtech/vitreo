// SPDX-License-Identifier: LGPL-2.1-only

/// @file session_misc.cpp
/// @brief PSAPI K32GetModuleFileNameExA, session/power, toolhelp, and debug stubs.
///
/// The middle of the Additional Win32 stubs run: WTS session id,
/// power/execution state, AttachConsole, DebugActiveProcess,
/// Module32First/Next, GetBinaryType, CreateHardLink, and
/// ReadDirectoryChangesW.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/common/object_access.hpp"
#include "winapi/kernel32/filesystem/directory_changes.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/module/module_internal.hpp"
#include "capsule/vfs/path_encoding.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "ipc/process_session.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <filesystem>
#include <limits>
#include <random>
#include <sys/sysctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libproc.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace vitreo::winapi {

// impl_K32GetModuleFileNameExA lives with the rest of the psapi family in
// module/psapi_k32.cpp.

void Kernel32ExtManager::impl_WTSGetActiveConsoleSessionId(Marshaler& m) {
    m.set_return(1u);
}

void Kernel32ExtManager::impl_GetComputerNameExW(Marshaler& m) {
    (void)m.arg(0); // NameType
    GuestAddr buf      = m.arg_ptr(1);
    GuestAddr size_ptr = m.arg_ptr(2);
    if (size_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    auto size_r = m_memory.read_u32(size_ptr);
    u32 cch = size_r.is_ok() ? size_r.value() : 0;
    constexpr const wchar_t kName[] = L"VITREO";
    constexpr u32 kLen = 6; // chars excl. NUL
    if (cch < kLen + 1 || buf == 0) {
        (void)m_memory.write_u32(size_ptr, kLen + 1);
        set_last_error(error_codes::ERROR_MORE_DATA);
        m.set_return(0u);
        return;
    }
    for (u32 i = 0; i < kLen; ++i) {
        (void)m_memory.write_u16(buf + i * 2, static_cast<u16>(kName[i]));
    }
    (void)m_memory.write_u16(buf + kLen * 2, 0);
    (void)m_memory.write_u32(size_ptr, kLen);
    m.set_return(1u);
}

void Kernel32ExtManager::impl_SetThreadExecutionState(Marshaler& m) {
    // Echo "previous state" — caller's request value is acceptable.
    m.set_return(m.arg(0));
}

void Kernel32ExtManager::impl_SetProcessShutdownParameters(Marshaler& m) {
    (void)m.arg(0);
    (void)m.arg(1);
    m.set_return(1u);
}

// -----------------------------------------------------------------------------
// Error mode (SetErrorMode / GetErrorMode / SetThreadErrorMode /
// GetThreadErrorMode)
//
// Windows keeps two error modes: a process-wide one owned by SetErrorMode and
// reported by GetErrorMode, and a per-thread one owned by SetThreadErrorMode
// and reported by GetThreadErrorMode. A thread that has never called
// SetThreadErrorMode reports the process error mode; once it has, its own mode
// takes precedence for that thread and the process mode is left untouched.
//
// Learn documents the settable per-thread bits as SEM_FAILCRITICALERRORS,
// SEM_NOGPFAULTERRORBOX and SEM_NOOPENFILEERRORBOX only.
// SEM_NOALIGNMENTFAULTEXCEPT is explicitly not settable per thread — passing it
// (or any bit outside the documented set) fails with ERROR_INVALID_PARAMETER
// and leaves the thread error mode unchanged.
// -----------------------------------------------------------------------------
namespace {

constexpr u32 kSemFailCriticalErrors = 0x0001u;
constexpr u32 kSemNoGpFaultErrorBox  = 0x0002u;
constexpr u32 kSemNoOpenFileErrorBox = 0x8000u;
/// Bits SetThreadErrorMode accepts. SEM_NOALIGNMENTFAULTEXCEPT (0x0004) is
/// deliberately absent: it is a process-wide setting only.
constexpr u32 kThreadErrorModeValidBits =
    kSemFailCriticalErrors | kSemNoGpFaultErrorBox | kSemNoOpenFileErrorBox;

/// Per-thread override state. API handlers run on the calling guest thread's
/// host thread, so thread_local is the guest thread's storage.
thread_local bool t_thread_error_mode_set = false;
thread_local u32  t_thread_error_mode = 0;

} // namespace

u32 Kernel32ExtManager::current_thread_error_mode() const {
    return t_thread_error_mode_set ? t_thread_error_mode : m_error_mode;
}

void Kernel32ExtManager::impl_GetErrorMode(Marshaler& m) {
    // UINT GetErrorMode(void) — the process error mode; never fails.
    m.set_return(m_error_mode);
}

void Kernel32ExtManager::impl_GetThreadErrorMode(Marshaler& m) {
    // DWORD GetThreadErrorMode(void) — the calling thread's error mode; never
    // fails, and defaults to the process error mode.
    m.set_return(current_thread_error_mode());
}

void Kernel32ExtManager::impl_SetThreadErrorMode(Marshaler& m) {
    // BOOL SetThreadErrorMode(DWORD dwNewMode, LPDWORD lpOldMode)
    const u32 new_mode = static_cast<u32>(m.arg(0));
    const GuestAddr old_mode_ptr = m.arg_ptr(1);

    if ((new_mode & ~kThreadErrorModeValidBits) != 0u) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    const u32 old_mode = current_thread_error_mode();
    if (old_mode_ptr != 0) {
        // lpOldMode is _Out_opt_; when supplied it must be writable. A guest
        // that hands over unwritable memory gets an honest failure rather than
        // a silently dropped store.
        if (!m_memory.write_u32(old_mode_ptr, old_mode).is_ok()) {
            set_last_error(error_codes::ERROR_NOACCESS);
            m.set_return(0u);
            return;
        }
    }

    t_thread_error_mode = new_mode;
    t_thread_error_mode_set = true;
    VITREO_TRACE("[kernel32_ext] SetThreadErrorMode(0x%x) -> old 0x%x\n",
                 new_mode, old_mode);
    m.set_return(1u);
}

void Kernel32ExtManager::impl_GetSystemPowerStatus(Marshaler& m) {
    GuestAddr ps = m.arg_ptr(0);
    if (ps == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    (void)m_memory.write_u8(ps + 0, 1);    // ACLineStatus = AC online
    (void)m_memory.write_u8(ps + 1, 128);  // BatteryFlag = no battery
    (void)m_memory.write_u8(ps + 2, 255);  // BatteryLifePercent = unknown
    (void)m_memory.write_u8(ps + 3, 0);    // SystemStatusFlag / Reserved1
    (void)m_memory.write_u32(ps + 4, 0xFFFFFFFFu); // BatteryLifeTime
    (void)m_memory.write_u32(ps + 8, 0xFFFFFFFFu); // BatteryFullLifeTime
    m.set_return(1u);
}

void Kernel32ExtManager::impl_AttachConsole(Marshaler& m) {
    (void)m.arg(0);
    set_last_error(error_codes::ERROR_INVALID_HANDLE);
    m.set_return(0u);
}

void Kernel32ExtManager::impl_GenerateConsoleCtrlEvent(Marshaler& m) {
    // BOOL GenerateConsoleCtrlEvent(DWORD dwCtrlEvent, DWORD dwProcessGroupId)
    // https://learn.microsoft.com/en-us/windows/console/generateconsolectrlevent
    constexpr u32 kCtrlCEvent = 0;
    constexpr u32 kCtrlBreakEvent = 1;
    const u32 ctrl_event = static_cast<u32>(m.arg(0));
    const u32 process_group = static_cast<u32>(m.arg(1));
    if (ctrl_event != kCtrlCEvent && ctrl_event != kCtrlBreakEvent) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m_console_manager == nullptr || !m_console_manager->is_attached()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    // Learn: CTRL_C with nonzero group succeeds but is not received by that
    // group. ignore-Ctrl+C attribute skips CTRL_C handlers only.
    const bool deliver =
        !(ctrl_event == kCtrlCEvent && process_group != 0) &&
        !(ctrl_event == kCtrlCEvent && m_console_manager->ignore_ctrl_c());
    if (deliver) {
        const auto handlers = m_console_manager->ctrl_handlers(); // LIFO
        bool handled = false;
        if (m_guest_call) {
            for (const GuestAddr routine : handlers) {
                if (routine == 0) continue;
                const u64 args[1] = {ctrl_event};
                const u64 ret =
                    m_guest_call(routine, args, 1, m.cpu_is_64bit());
                if (ret != 0) {
                    handled = true;
                    break;
                }
            }
        }
        if (!handled) {
            // Default console handler calls ExitProcess (Learn).
            static bool logged = false;
            if (!logged) {
                logged = true;
                VITREO_TRACE(
                    "[vitreo] GenerateConsoleCtrlEvent: default ExitProcess "
                    "for CTRL_* (no HandlerRoutine returned TRUE)\n");
            }
            if (m_process_manager != nullptr) {
                m_process_manager->exit_process(0);
            }
        }
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_DebugActiveProcess(Marshaler& m) {
    (void)m.arg(0);
    set_last_error(error_codes::ERROR_NOT_SUPPORTED);
    m.set_return(0u);
}

void Kernel32ExtManager::impl_DebugActiveProcessStop(Marshaler& m) {
    (void)m.arg(0);
    set_last_error(error_codes::ERROR_NOT_SUPPORTED);
    m.set_return(0u);
}

void Kernel32ExtManager::impl_Module32First(Marshaler& m) {
    (void)write_next_module_entry(m, static_cast<u32>(m.arg(0)),
                                  m.arg_ptr(1), false, true);
}

void Kernel32ExtManager::impl_Module32Next(Marshaler& m) {
    (void)write_next_module_entry(m, static_cast<u32>(m.arg(0)),
                                  m.arg_ptr(1), false, false);
}

namespace {

/// SCS_* (Learn GetBinaryTypeW).
constexpr u32 kScs32BitBinary = 0;
constexpr u32 kScs64BitBinary = 6;
constexpr u32 kScsDosBinary = 1;
constexpr u32 kScsPifBinary = 3;
constexpr u32 kScsPosixBinary = 4;

/// Classify @p data as GetBinaryType does. Returns the SCS_* value, or
/// nullopt for ERROR_BAD_EXE_FORMAT. Every rule below is a measured
/// tools/oracle_probe/get_binary_type_probe case (x86 and x64 identical):
///  * an image is a file that would make an image section: "MZ", then
///    "PE\0\0" at e_lfanew with the COFF header and the whole optional
///    header in the file (pe64_cut_after_optional; pe64_cut_mid_optional is
///    still an MS-DOS program). It must be a valid executable image for this
///    host: PE32 i386 -> SCS_32BIT_BINARY, PE32+ AMD64 -> SCS_64BIT_BINARY,
///    the POSIX subsystem -> SCS_POSIX_BINARY; a DLL, a missing
///    EXECUTABLE_IMAGE flag, any other machine (ARM64, ARMNT, IA64), a
///    machine that disagrees with the magic, SizeOfImage 0 or section data
///    past the end of the file is ERROR_BAD_EXE_FORMAT (own_*). The
///    extension does not matter for an image (own_image_as_*).
///  * any other "MZ" file is an MS-DOS program whatever its name
///    (mz_dos_only, ne_*, le/lx_header, mz_dos_named_pif/_bat). "ZM" is not
///    "MZ" here (own_image_zm_signature falls to the name rule).
///  * anything else goes by name: .pif -> SCS_PIF_BINARY, .exe/.com ->
///    SCS_DOS_BINARY (text_named_exe, one_byte_exe), otherwise
///    ERROR_BAD_EXE_FORMAT (text_txt, bat, cmd, scr_text, text_no_extension).
///    An empty file is ERROR_BAD_EXE_FORMAT under any name (empty_*).
std::optional<u32> classify_binary(std::span<const u8> data, std::string_view name) {
    if (data.empty()) return std::nullopt;
    const auto read16 = [&](usize at) -> u16 {
        u16 v = 0;
        std::memcpy(&v, data.data() + at, sizeof v);
        return v;
    };
    const auto read32 = [&](usize at) -> u32 {
        u32 v = 0;
        std::memcpy(&v, data.data() + at, sizeof v);
        return v;
    };
    if (data.size() >= 2 && data[0] == 'M' && data[1] == 'Z') {
        bool image = false;
        if (data.size() >= 0x40) {
            const u64 nt = read32(0x3C);
            if (nt + 24 <= data.size() && std::memcmp(data.data() + nt, "PE\0\0", 4) == 0) {
                const u64 optional_size = read16(static_cast<usize>(nt) + 20);
                image = nt + 24 + optional_size <= data.size();
            }
        }
        if (!image) return kScsDosBinary;
        auto parsed = pe::parse_pe(data);
        if (parsed.is_err()) return std::nullopt;
        const pe::ParsedPe& pe = parsed.value();
        if (pe.size_of_image == 0 || !pe.is_executable() || pe.is_dll()) return std::nullopt;
        for (const auto& section : pe.sections) {
            if (static_cast<u64>(section.raw_offset) + section.raw_size > data.size()) {
                return std::nullopt;
            }
        }
        u32 type = 0;
        if (pe.machine == pe::MachineType::I386 && !pe.is_64bit) {
            type = kScs32BitBinary;
        } else if (pe.machine == pe::MachineType::AMD64 && pe.is_64bit) {
            type = kScs64BitBinary;
        } else {
            return std::nullopt;
        }
        if (pe.subsystem == pe::Subsystem::PosixCui) type = kScsPosixBinary;
        return type;
    }
    const usize slash = name.find_last_of("\\/");
    const std::string_view leaf = slash == std::string_view::npos ? name : name.substr(slash + 1);
    const usize dot = leaf.rfind('.');
    std::string ext;
    if (dot != std::string_view::npos) {
        for (char c : leaf.substr(dot + 1)) {
            ext.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
    }
    if (ext == "pif") return kScsPifBinary;
    if (ext == "exe" || ext == "com") return kScsDosBinary;
    return std::nullopt;
}

} // namespace

void Kernel32ExtManager::get_binary_type_impl(Marshaler& m, bool wide) {
    // BOOL GetBinaryType(LPCTSTR lpApplicationName, LPDWORD lpBinaryType).
    // On failure *lpBinaryType is left as it was and on success the last
    // error is untouched (get_binary_type_probe: type=0xdead / err=0xdead).
    const GuestAddr name_ptr = m.arg_ptr(0);
    const GuestAddr type_ptr = m.arg_ptr(1);
    const auto fail = [&](u32 error) {
        set_last_error(error);
        m.set_return(0u);
    };
    auto name = wide ? m.read_wide_string(name_ptr) : m.read_ansi_string(name_ptr);
    if (name_ptr == 0 || name.is_err()) {
        fail(error_codes::ERROR_NOACCESS);
        return;
    }
    const std::string path = wide ? name.value() : capsule::ansi_to_utf8(name.value());
    // empty_string: ERROR_INVALID_NAME.
    if (path.empty()) {
        fail(error_codes::ERROR_INVALID_NAME);
        return;
    }
    // missing_file / missing_dir: the path errors; directory_named_exe:
    // ERROR_ACCESS_DENIED (opening a directory as a file).
    auto info = m_vfs.stat(path);
    if (info.is_err()) {
        fail(vfs_error_to_win32(info.error()));
        return;
    }
    if (capsule::has_attribute(info.value().attributes, capsule::FileAttributes::Directory)) {
        fail(error_codes::ERROR_ACCESS_DENIED);
        return;
    }
    auto data = module_read_all_bytes(m_vfs, path);
    if (data.is_err()) {
        fail(vfs_error_to_win32(data.error()));
        return;
    }
    const auto type = classify_binary(data.value(), path);
    if (!type) {
        fail(error_codes::ERROR_BAD_EXE_FORMAT);
        return;
    }
    (void)m.write_u32(type_ptr, *type);
    m.set_return(1u);
}

void Kernel32ExtManager::impl_GetBinaryTypeW(Marshaler& m) {
    get_binary_type_impl(m, true);
}

void Kernel32ExtManager::impl_GetBinaryTypeA(Marshaler& m) {
    get_binary_type_impl(m, false);
}

void Kernel32ExtManager::create_hard_link_impl(Marshaler& m, bool wide) {
    // Microsoft Learn: lpFileName names the new directory entry,
    // lpExistingFileName names an existing file, and lpSecurityAttributes is
    // reserved and must be NULL.
    if (m.arg_ptr(0) == 0 || m.arg_ptr(1) == 0 || m.arg_ptr(2) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }
    auto new_name = wide ? m.read_wide_string(m.arg_ptr(0))
                         : m.read_ansi_string(m.arg_ptr(0));
    auto existing_name = wide ? m.read_wide_string(m.arg_ptr(1))
                              : m.read_ansi_string(m.arg_ptr(1));
    if (new_name.is_err() || existing_name.is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0u);
        return;
    }

    auto result = m_vfs.create_hard_link(new_name.value(), existing_name.value());
    if (result.is_err()) {
        set_last_error(vfs_error_to_win32(result.error()));
        m.set_return(0u);
        return;
    }
    m.set_return(1u);
}

void Kernel32ExtManager::impl_CreateHardLinkA(Marshaler& m) {
    create_hard_link_impl(m, false);
}

void Kernel32ExtManager::impl_CreateHardLinkW(Marshaler& m) {
    create_hard_link_impl(m, true);
}

DirectoryChangeService& Kernel32ExtManager::directory_changes() {
    std::call_once(m_directory_changes_once, [this] {
        DirectoryChangeService::Completion completion;
        completion.overlapped = [this](u32 handle, u32 bytes, GuestAddr overlapped,
                                       bool is_64bit, u32 nt_status, u32 win_error) {
            complete_overlapped_file_io(handle, bytes, overlapped, is_64bit, nt_status,
                                        win_error, false);
        };
        completion.routine = [this](u32 thread_id, GuestAddr routine, u32 win_error,
                                    u32 bytes, GuestAddr overlapped) {
            if (m_thread_manager) {
                (void)m_thread_manager->queue_apc3_for_thread(thread_id, routine, win_error,
                                                              bytes, overlapped);
            }
        };
        m_directory_changes =
            std::make_shared<DirectoryChangeService>(m_memory, std::move(completion));
    });
    return *m_directory_changes;
}

void Kernel32ExtManager::on_directory_watch_close(Handle handle) {
    if (m_directory_changes) m_directory_changes->close(handle);
}

void Kernel32ExtManager::impl_ReadDirectoryChangesW(Marshaler& m) {
    // BOOL ReadDirectoryChangesW(HANDLE hDirectory, LPVOID lpBuffer,
    //     DWORD nBufferLength, BOOL bWatchSubtree, DWORD dwNotifyFilter,
    //     LPDWORD lpBytesReturned, LPOVERLAPPED lpOverlapped,
    //     LPOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
    // Errors measured by tools/oracle_probe/read_dir_changes_probe (x86 = x64).
    const Handle handle = static_cast<Handle>(m.arg(0));
    const GuestAddr buffer = m.arg_ptr(1);
    const u32 length = static_cast<u32>(m.arg(2));
    const bool subtree = static_cast<u32>(m.arg(3)) != 0;
    const u32 filter = static_cast<u32>(m.arg(4));
    const GuestAddr bytes_out = m.arg_ptr(5);
    const GuestAddr overlapped = m.arg_ptr(6);
    const GuestAddr routine = m.arg_ptr(7);
    const auto fail = [&](u32 error) {
        set_last_error(error);
        m.set_return(0u);
    };
    if (m_handle_table == nullptr || m_file_manager == nullptr || handle == NULL_HANDLE ||
        handle == INVALID_HANDLE || m_handle_table->get_type(handle) == HandleType::Invalid) {
        return fail(error_codes::ERROR_INVALID_HANDLE);
    }
    if (filter == 0 || (filter & ~dirchange::kValidFilter) != 0) {
        return fail(error_codes::ERROR_INVALID_PARAMETER);
    }
    // A directory opened through CreateFile (FILE_FLAG_BACKUP_SEMANTICS).
    std::string host_directory;
    if (m_handle_table->get_type(handle) == HandleType::File) {
        const std::string guest = m_file_manager->path_for_handle(handle);
        if (!guest.empty()) {
            auto host = m_vfs.resolve_host_path(guest);
            std::error_code ec;
            if (host.is_ok() && std::filesystem::is_directory(host.value(), ec)) {
                host_directory = host.value();
            }
        }
    }
    if (host_directory.empty()) return fail(error_codes::ERROR_INVALID_PARAMETER);
    constexpr u32 FILE_LIST_DIRECTORY = 0x0001;
    const u32 granted =
        object_access::file_granted_access(m_handle_table->get_access(handle), HandleAccess::All);
    if ((granted & FILE_LIST_DIRECTORY) == 0) return fail(error_codes::ERROR_ACCESS_DENIED);
    if (length != 0 && (buffer == 0 || !m_memory.guest_can_write(buffer, length))) {
        return fail(error_codes::ERROR_NOACCESS);
    }

    DirectoryChangeService::Request request;
    request.handle = handle;
    request.host_directory = host_directory;
    request.buffer = buffer;
    request.length = length;
    request.subtree = subtree;
    request.filter = filter;
    request.overlapped = overlapped;
    request.routine = routine;
    request.thread_id = ThreadManager::get_current_thread_id_static();
    request.is_64bit = m.cpu_is_64bit();

    if (overlapped != 0 && m_file_manager->is_overlapped_file(handle)) {
        constexpr u32 STATUS_PENDING = 0x00000103;
        const GuestAddr event_field = overlapped + (request.is_64bit ? 24u : 16u);
        const u64 event = request.is_64bit ? m_memory.read_u64(event_field).value_or(0)
                                           : m_memory.read_u32(event_field).value_or(0);
        if (event != 0 && m_sync_manager) {
            (void)m_sync_manager->reset_event(static_cast<Handle>(event & ~u64{1}));
        }
        (void)(request.is_64bit ? m_memory.write_u64(overlapped, STATUS_PENDING)
                                : m_memory.write_u32(overlapped, STATUS_PENDING));
        if (!directory_changes().start(request)) {
            static std::once_flag once;
            std::call_once(once, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!ReadDirectoryChangesW: "
                                     "host change notifications unavailable\n");
            });
            return fail(error_codes::ERROR_NOT_SUPPORTED);
        }
        // Measured: the overlapped form returns TRUE once queued.
        m.set_return(1u);
        return;
    }

    // Synchronous: the call blocks until changes arrive (measured).
    u32 bytes = 0;
    const u32 status = directory_changes().wait(request, bytes);
    if (status == 0xC0000001u) return fail(error_codes::ERROR_NOT_SUPPORTED);
    if (bytes_out != 0) (void)m_memory.write_u32(bytes_out, bytes);
    if (overlapped != 0) {
        // A non-overlapped handle with an OVERLAPPED completes synchronously
        // and still reports through it.
        complete_overlapped_file_io(handle, bytes, overlapped, request.is_64bit, status,
                                    status == 0 ? 0 : dirchange::ERROR_NOTIFY_ENUM_DIR, true);
    }
    if (status == dirchange::STATUS_NOTIFY_CLEANUP) return fail(dirchange::ERROR_NOTIFY_CLEANUP);
    set_last_error(status == 0 ? error_codes::ERROR_SUCCESS : dirchange::ERROR_NOTIFY_ENUM_DIR);
    m.set_return(1u);
}

namespace {

// Microsoft Learn (FindFirstChangeNotificationW): path must be a full path to
// an existing directory — not relative and not empty.
[[nodiscard]] bool is_absolute_windows_directory_path(
    std::string_view path) noexcept {
    if (path.empty()) return false;
    if (path.size() >= 3 &&
        std::isalpha(static_cast<unsigned char>(path[0])) &&
        path[1] == ':' && (path[2] == '\\' || path[2] == '/')) {
        return true;
    }
    return path.size() >= 2 && path[0] == '\\' && path[1] == '\\';
}

[[nodiscard]] std::string directory_fingerprint(const std::string& host_path,
                                                bool subtree) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string out;
    out.reserve(256);
    auto append_entry = [&](const fs::directory_entry& entry) {
        out.append(entry.path().lexically_relative(host_path).string());
        out.push_back('\0');
        const auto status = entry.status(ec);
        if (ec) {
            ec.clear();
            return;
        }
        out.push_back(fs::is_directory(status) ? 'D' : 'F');
        if (fs::is_regular_file(status)) {
            out.append(std::to_string(
                static_cast<unsigned long long>(entry.file_size(ec))));
            ec.clear();
        }
        out.push_back('\0');
        const auto mtime = entry.last_write_time(ec);
        if (!ec) {
            out.append(std::to_string(
                static_cast<long long>(mtime.time_since_epoch().count())));
        }
        ec.clear();
        out.push_back('\n');
    };

    if (subtree) {
        for (fs::recursive_directory_iterator it(
                 host_path, fs::directory_options::skip_permission_denied, ec),
             end;
             it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            append_entry(*it);
        }
    } else {
        for (fs::directory_iterator it(
                 host_path, fs::directory_options::skip_permission_denied, ec),
             end;
             it != end; it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }
            append_entry(*it);
        }
    }
    return out;
}

}  // namespace

void Kernel32ExtManager::stop_change_notification_watch(
    const std::shared_ptr<ChangeNotificationWatch>& watch) {
    if (!watch) return;
    watch->stop.store(true, std::memory_order_release);
    if (watch->worker.joinable()) {
        if (watch->worker.get_id() == std::this_thread::get_id()) {
            watch->worker.detach();
        } else {
            watch->worker.join();
        }
    }
}

void Kernel32ExtManager::stop_all_change_notification_watches() {
    std::vector<std::shared_ptr<ChangeNotificationWatch>> watches;
    {
        std::lock_guard lock(m_change_watch_mutex);
        watches.reserve(m_change_watches.size());
        for (auto& [handle, watch] : m_change_watches) {
            (void)handle;
            watches.push_back(watch);
        }
        m_change_watches.clear();
    }
    for (auto& watch : watches) {
        stop_change_notification_watch(watch);
        if (m_sync_manager && watch->event_handle != NULL_HANDLE) {
            (void)m_sync_manager->close_handle(watch->event_handle);
            watch->event_handle = NULL_HANDLE;
        }
    }
}

Handle Kernel32ExtManager::start_change_notification(const std::string& path,
                                                     bool watch_subtree,
                                                     u32 notify_filter) {
    if (!m_sync_manager) {
        set_last_error(error_codes::ERROR_INVALID_FUNCTION);
        return INVALID_HANDLE;
    }
    if (!is_absolute_windows_directory_path(path)) {
        set_last_error(error_codes::ERROR_INVALID_NAME);
        return INVALID_HANDLE;
    }
    if (notify_filter == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE;
    }

    u32 attrs = INVALID_FILE_ATTRIBUTES;
    if (m_file_search_manager) {
        attrs = m_file_search_manager->get_file_attributes(path);
    } else {
        auto info = m_vfs.stat(path);
        if (info.is_ok()) {
            attrs = static_cast<u32>(info.value().attributes);
        } else {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        }
    }
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return INVALID_HANDLE;
    }
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        set_last_error(error_codes::ERROR_DIRECTORY);
        return INVALID_HANDLE;
    }

    auto host = m_vfs.resolve_host_path(path);
    if (host.is_err()) {
        set_last_error(error_codes::ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE;
    }

    // The notification object is an internal event; CreateEvent's success
    // code must not leak: FindFirstChangeNotification leaves the last error
    // untouched on success (measured on Windows 11:
    // last_error_on_success_probe.expected).
    const u32 entry_error = get_last_error();
    const Handle event =
        m_sync_manager->create_event(/*manual_reset=*/false,
                                     /*initial_state=*/false, "");
    if (event == NULL_HANDLE || event == INVALID_HANDLE) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE;
    }
    set_last_error(entry_error);

    auto watch = std::make_shared<ChangeNotificationWatch>();
    watch->event_handle = event;
    watch->host_path = host.value();
    watch->watch_subtree = watch_subtree;
    watch->notify_filter = notify_filter;
    watch->armed.store(true, std::memory_order_release);

    {
        std::lock_guard lock(m_change_watch_mutex);
        m_change_watches[event] = watch;
    }

    watch->worker = std::thread([this, watch]() {
        std::string previous =
            directory_fingerprint(watch->host_path, watch->watch_subtree);
        while (!watch->stop.load(std::memory_order_acquire) &&
               !m_shutdown.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (watch->stop.load(std::memory_order_acquire) ||
                m_shutdown.load(std::memory_order_acquire)) {
                break;
            }
            const std::string current =
                directory_fingerprint(watch->host_path, watch->watch_subtree);
            if (current == previous) continue;
            previous = current;
            // Fingerprint covers name/size/write/attribute class changes.
            // Security-descriptor-only changes are not visible on the host FS.
            (void)watch->notify_filter;
            if (watch->armed.exchange(false, std::memory_order_acq_rel)) {
                if (!m_sync_manager ||
                    !m_sync_manager->set_event(watch->event_handle)) {
                    break;
                }
            } else {
                // MSDN FindNextChangeNotification: changes between waits are
                // recorded and immediately satisfy the next wait after FindNext.
                watch->pending.store(true, std::memory_order_release);
            }
        }
    });

    return event;
}

} // namespace vitreo::winapi

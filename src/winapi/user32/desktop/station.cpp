// SPDX-License-Identifier: LGPL-2.1-only

/// @file user32_station.cpp
/// @brief Window stations and desktops — the USER objects a process attaches to.
///
/// Vitreo has no real ones. A guest process runs alone on a Mac, so these track
/// stations and desktops in two maps and answer from there: an open by name
/// succeeds only for a name already registered, a close fails with ERROR_BUSY
/// on the station the process is attached to, and EnumDesktopWindows reports
/// nothing rather than inventing windows for a desktop that does not exist.
///
/// Split from user32_display.cpp when the two together crossed the file-size
/// ratchet. They were adjacent in user32_ext.cpp and share nothing else: one
/// answers "which screen", the other "which session".

#include "winapi/user32/user32_ext.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "windowing/model/hwnd_manager.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/advapi32/security/security_internal.hpp"
#include "ipc/ipc_gate.hpp"
#include "vitreo/result.hpp"
#include "winapi/kernel32/synthetic_handle_registry.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi {

namespace {
// Desktop and window-station handles are USER objects this manager numbers
// itself, but they are handles to the guest: GetHandleInformation reports the
// fInherit / lpsa->bInheritHandle they were opened with, and
// SetHandleInformation changes it (s4_desktop_handle_probe on Windows 11:
// OpenDesktopW/OpenWindowStationW fInherit=TRUE -> flags 1, FALSE -> 0;
// GetThreadDesktop -> 0).
void note_user_handle(u64 handle, bool inherit, bool fresh = true) {
    synthetic_handles::note_user_object(static_cast<Handle>(handle),
                                        inherit ? HandleFlags::Inherit : 0u,
                                        fresh);
}

// SECURITY_ATTRIBUTES.bInheritHandle: offset 8 (x86) / 16 (x64).
bool sa_inherits(Marshaler& m, GuestAddr sa) {
    if (sa == 0) return false;
    return m.read_u32(sa + (m.cpu_is_64bit() ? 16u : 8u)).value_or(0) != 0;
}
} // namespace

void User32ExtManager::impl_GetProcessWindowStation(Marshaler& m) {
    note_user_handle(m_process_window_station, false, /*fresh=*/false);
    m.set_return(m_process_window_station);
}

void User32ExtManager::impl_CreateWindowStationW(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    (void)m.arg(1);
    (void)m.arg(2);
    const bool inherit = sa_inherits(m, m.arg_ptr(3));

    std::string name;
    if (name_ptr != 0) {
        auto name_result = m.read_wide_string(name_ptr);
        if (name_result.is_err()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        name = name_result.value();
    }

    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    const u64 handle = m_next_window_station++;
    if (name.empty()) {
        // MS Learn: NULL name asks the system to generate a name.
        name = "VitreoWindowStation-" + std::to_string(handle);
    }
    m_window_stations.emplace(handle, name);
    // New stations are not the interactive visible station (WSF_VISIBLE).
    m_user_object_dw_flags.emplace(handle, 0u);
    note_user_handle(handle, inherit);
    ipc::ipc_log("CreateWindowStationW name='%s' -> h=0x%llx",
                 name.c_str(),
                 static_cast<unsigned long long>(handle));
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(handle);
}

void User32ExtManager::impl_OpenWindowStationW(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    const bool inherit = m.arg32(1) != 0; // fInherit
    (void)m.arg(2); // dwDesiredAccess
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto name_result = m.read_wide_string(name_ptr);
    if (name_result.is_err() || name_result.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    const u64 handle = open_window_station_by_name(name_result.value());
    if (handle == 0) {
        ipc::ipc_log("OpenWindowStationW name='%s' -> NULL (not found)",
                     name_result.value().c_str());
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0);
        return;
    }
    note_user_handle(handle, inherit);
    ipc::ipc_log("OpenWindowStationW name='%s' -> h=0x%llx",
                 name_result.value().c_str(),
                 static_cast<unsigned long long>(handle));
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(handle);
}

void User32ExtManager::impl_SetProcessWindowStation(Marshaler& m) {
    const u64 handle = m.arg(0);
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    if (m_window_stations.find(handle) == m_window_stations.end()) {
        ipc::ipc_log("SetProcessWindowStation h=0x%llx -> FALSE (invalid)",
                     static_cast<unsigned long long>(handle));
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    m_process_window_station = handle;
    ipc::ipc_log("SetProcessWindowStation h=0x%llx -> TRUE",
                 static_cast<unsigned long long>(handle));
    m.set_return(1);
}

void User32ExtManager::impl_CloseWindowStation(Marshaler& m) {
    const u64 handle = m.arg(0);
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    auto it = m_window_stations.find(handle);
    if (it == m_window_stations.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (handle == m_process_window_station) {
        constexpr u32 ERROR_BUSY = 170;
        set_last_error(ERROR_BUSY);
        m.set_return(0);
        return;
    }
    m_user_object_dw_flags.erase(handle);
    m_window_stations.erase(it);
    (void)synthetic_handles::close(static_cast<Handle>(handle));
    m.set_return(1);
}

void User32ExtManager::impl_GetThreadDesktop(Marshaler& m) {
    (void)m.arg(0);
    note_user_handle(m_thread_desktop, false, /*fresh=*/false);
    m.set_return(m_thread_desktop);
}

void User32ExtManager::impl_CreateDesktopW(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    (void)m.arg_ptr(1);
    (void)m.arg_ptr(2);
    (void)m.arg(3);
    (void)m.arg(4);
    const bool inherit = sa_inherits(m, m.arg_ptr(5));
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto name_result = m.read_wide_string(name_ptr);
    if (name_result.is_err() || name_result.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    const u64 handle = m_next_desktop++;
    m_desktops.emplace(handle, name_result.value());
    m_user_object_dw_flags.emplace(handle, 0u);
    note_user_handle(handle, inherit);
    ipc::ipc_log("CreateDesktopW name='%s' -> h=0x%llx",
                 name_result.value().c_str(),
                 static_cast<unsigned long long>(handle));
    m.set_return(handle);
}

void User32ExtManager::impl_SetThreadDesktop(Marshaler& m) {
    const u64 handle = m.arg(0);
    {
        std::lock_guard<std::mutex> objects(m_user_object_mutex);
        if (m_desktops.find(handle) == m_desktops.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
    }
    // Learn: fails if the calling thread has any windows or hooks; measured
    // (s1win_misc_probe setthreaddesktop_*): ERROR_BUSY, even for the
    // thread's current desktop.
    if (m_hwnd_mgr.thread_has_windows(ThreadManager::get_current_thread_id_static())) {
        constexpr u32 ERROR_BUSY = 170;
        set_last_error(ERROR_BUSY);
        m.set_return(0);
        return;
    }
    m_thread_desktop = handle;
    m.set_return(1);
}

void User32ExtManager::impl_SwitchDesktop(Marshaler& m) {
    const u64 handle = m.arg(0);
    // MS Learn (winuser!SwitchDesktop): make the desktop visible / receive
    // input. Fails for invalid handles. Vitreo has no real desktop switch;
    // accept known desktop handles and record them as the input desktop.
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    if (m_desktops.find(handle) == m_desktops.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    m_input_desktop = handle;
    ipc::ipc_log("SwitchDesktop h=0x%llx -> TRUE",
                 static_cast<unsigned long long>(handle));
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
}

namespace {

[[nodiscard]] u32 self_relative_sd_length(Marshaler& m, GuestAddr sd) {
    constexpr u16 SE_SELF_RELATIVE = 0x8000;
    const auto revision = m.read_u8(sd);
    const auto control_r = m.read_u16(sd + 2);
    if (revision.is_err() || control_r.is_err() || revision.value() != 1) {
        return 0;
    }
    if ((control_r.value() & SE_SELF_RELATIVE) == 0) {
        return 0;
    }
    auto sid_length = [&](GuestAddr sid) -> u32 {
        if (sid == 0) return 0;
        const auto count = m.read_u8(sid + 1);
        if (count.is_err()) return 0;
        return 8u + 4u * static_cast<u32>(count.value());
    };
    auto acl_length = [&](GuestAddr acl) -> u32 {
        if (acl == 0) return 0;
        const auto size = m.read_u16(acl + 2);
        if (size.is_err() || size.value() < 8) return 0;
        return size.value();
    };
    u32 length = 20;
    for (u32 index = 0; index < 4; ++index) {
        const auto offset = m.read_u32(sd + 4 + index * 4);
        if (offset.is_err() || offset.value() == 0) continue;
        const u32 component = index < 2 ? sid_length(sd + offset.value())
                                        : acl_length(sd + offset.value());
        if (component == 0) return 0;
        length = std::max(length, offset.value() + component);
    }
    return length;
}

} // namespace

// Caller holds m_user_object_mutex.
bool User32ExtManager::has_desktop_handle(u64 handle) const {
    return m_desktops.find(handle) != m_desktops.end();
}

// Caller holds m_user_object_mutex.
bool User32ExtManager::has_window_station_handle(u64 handle) const {
    return m_window_stations.find(handle) != m_window_stations.end();
}

void User32ExtManager::apply_startup_desktop(std::string_view spec) {
    if (spec.empty()) return;
    std::lock_guard<std::mutex> objects(m_user_object_mutex);

    std::string station_name;
    std::string desktop_name;
    const auto slash = spec.find('\\');
    if (slash != std::string_view::npos) {
        station_name = std::string(spec.substr(0, slash));
        desktop_name = std::string(spec.substr(slash + 1));
    } else {
        desktop_name = std::string(spec);
    }

    if (!station_name.empty()) {
        u64 handle = open_window_station_by_name(station_name);
        if (handle == 0) {
            handle = m_next_window_station++;
            m_window_stations.emplace(handle, station_name);
            m_user_object_dw_flags.emplace(handle, 0u);
        }
        m_process_window_station = handle;
    }

    if (!desktop_name.empty()) {
        u64 handle = open_desktop_by_name(desktop_name);
        if (handle == 0) {
            handle = m_next_desktop++;
            m_desktops.emplace(handle, desktop_name);
            m_user_object_dw_flags.emplace(handle, 0u);
        }
        m_thread_desktop = handle;
        // Do not SwitchDesktop: alternate sandboxed desktops are not the
        // interactive input desktop.
    }

    ipc::ipc_log("apply_startup_desktop '%.*s' -> winsta=0x%llx desk=0x%llx",
                 static_cast<int>(spec.size()), spec.data(),
                 static_cast<unsigned long long>(m_process_window_station),
                 static_cast<unsigned long long>(m_thread_desktop));
}

void User32ExtManager::impl_GetUserObjectSecurity(Marshaler& m) {
    const u64 handle = m.arg(0);
    (void)m.arg_ptr(1); // pSIRequested — return a full default/stored SD
    const GuestAddr sd_out = m.arg_ptr(2);
    const u32 length = static_cast<u32>(m.arg(3));
    const GuestAddr needed_out = m.arg_ptr(4);

    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    if (!has_desktop_handle(handle) && !has_window_station_handle(handle)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (needed_out == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    std::vector<u8> descriptor;
    auto it = m_user_object_sds.find(handle);
    if (it != m_user_object_sds.end()) {
        descriptor = it->second;
    } else {
        descriptor = make_default_null_dacl_sd(true, true, true);
    }
    descriptor = ensure_sd_has_owner_group(std::move(descriptor));
    const u32 needed = static_cast<u32>(descriptor.size());
    (void)m.write_u32(needed_out, needed);
    if (sd_out == 0 || length < needed) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    if (m.memory().write_bytes(sd_out, descriptor.data(), descriptor.size())
            .is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    ipc::ipc_log("GetUserObjectSecurity h=0x%llx needed=%u -> TRUE",
                 static_cast<unsigned long long>(handle), needed);
    m.set_return(1);
}

void User32ExtManager::impl_SetUserObjectSecurity(Marshaler& m) {
    const u64 handle = m.arg(0);
    (void)m.arg_ptr(1); // pSIRequested
    const GuestAddr sd = m.arg_ptr(2);

    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    if (!has_desktop_handle(handle) && !has_window_station_handle(handle)) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (sd == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    const u32 length = self_relative_sd_length(m, sd);
    if (length < 20) {
        set_last_error(error_codes::ERROR_INVALID_SECURITY_DESCR);
        m.set_return(0);
        return;
    }
    std::vector<u8> bytes(length);
    if (m.memory().read_bytes(sd, bytes.data(), bytes.size()).is_err()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    bytes = ensure_sd_has_owner_group(std::move(bytes));
    m_user_object_sds[handle] = std::move(bytes);
    ipc::ipc_log("SetUserObjectSecurity h=0x%llx len=%u -> TRUE",
                 static_cast<unsigned long long>(handle), length);
    m.set_return(1);
}

void User32ExtManager::impl_GetUserObjectInformationW(Marshaler& m) {
    const u64 handle = m.arg(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    const u32 length = static_cast<u32>(m.arg(3));
    const GuestAddr needed_out = m.arg_ptr(4);

    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    auto station = m_window_stations.find(handle);
    auto desktop = m_desktops.find(handle);
    if (station == m_window_stations.end() && desktop == m_desktops.end()) {
        // Measured (s1win_misc_probe uoi_invalid_handle): FALSE with the
        // last error left as it was.
        m.set_return(0);
        return;
    }

    // MS Learn (winuser!GetUserObjectInformationW): UOI_FLAGS / UOI_IO /
    // UOI_NAME / UOI_TYPE. UOI_FLAGS returns USEROBJECTFLAGS (12 bytes).
    constexpr u32 UOI_FLAGS = 1;
    constexpr u32 UOI_NAME = 2;
    constexpr u32 UOI_TYPE = 3;
    constexpr u32 UOI_IO = 6;
    constexpr u32 WSF_VISIBLE = 0x0001u;

    if (index == UOI_FLAGS) {
        constexpr u32 needed = 12; // BOOL + BOOL + DWORD
        if (needed_out != 0) {
            (void)m.write_u32(needed_out, needed);
        }
        if (info == 0 || length < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        u32 dw_flags = 0;
        auto flags_it = m_user_object_dw_flags.find(handle);
        if (flags_it != m_user_object_dw_flags.end()) {
            dw_flags = flags_it->second;
        } else if (station != m_window_stations.end() &&
                   station->second == "WinSta0") {
            dw_flags = WSF_VISIBLE;
        }
        (void)m.write_u32(info + 0, 0); // fInherit
        (void)m.write_u32(info + 4, 0); // fReserved
        (void)m.write_u32(info + 8, dw_flags);
        ipc::ipc_log(
            "GetUserObjectInformationW UOI_FLAGS h=0x%llx dwFlags=0x%x -> TRUE",
            static_cast<unsigned long long>(handle), dw_flags);
        m.set_return(1);
        return;
    }

    if (index == UOI_IO) {
        // Learn: TRUE iff hObj is the desktop receiving user input.
        if (desktop == m_desktops.end()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        constexpr u32 needed = 4; // BOOL
        if (needed_out != 0) {
            (void)m.write_u32(needed_out, needed);
        }
        if (info == 0 || length < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        bool is_input = (handle == m_input_desktop);
        if (!is_input) {
            auto input_it = m_desktops.find(m_input_desktop);
            if (input_it != m_desktops.end() &&
                input_it->second == desktop->second) {
                is_input = true;
            }
        }
        (void)m.write_u32(info, is_input ? 1u : 0u);
        m.set_return(1);  // success leaves the last error alone (measured)
        return;
    }

    std::string value;
    if (index == UOI_NAME) {
        value = station != m_window_stations.end() ? station->second : desktop->second;
    } else if (index == UOI_TYPE) {
        value = station != m_window_stations.end() ? "WindowStation" : "Desktop";
    } else {
        ipc::ipc_log(
            "GetUserObjectInformationW unsupported index=%u h=0x%llx",
            index, static_cast<unsigned long long>(handle));
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const u32 needed = static_cast<u32>((value.size() + 1) * 2);
    if (needed_out != 0) {
        (void)m.write_u32(needed_out, needed);
    }
    if (info == 0 || length < needed) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    (void)m.write_wide_string(info, value);
    m.set_return(1);
}

void User32ExtManager::impl_GetUserObjectInformationA(Marshaler& m) {
    // ANSI twin of GetUserObjectInformationW: UOI_FLAGS / UOI_IO layout is
    // identical; UOI_NAME / UOI_TYPE return ANSI strings (byte length + NUL).
    const u64 handle = m.arg(0);
    const u32 index = static_cast<u32>(m.arg(1));
    const GuestAddr info = m.arg_ptr(2);
    const u32 length = static_cast<u32>(m.arg(3));
    const GuestAddr needed_out = m.arg_ptr(4);

    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    auto station = m_window_stations.find(handle);
    auto desktop = m_desktops.find(handle);
    if (station == m_window_stations.end() && desktop == m_desktops.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    constexpr u32 UOI_FLAGS = 1;
    constexpr u32 UOI_NAME = 2;
    constexpr u32 UOI_TYPE = 3;
    constexpr u32 UOI_IO = 6;
    constexpr u32 WSF_VISIBLE = 0x0001u;

    if (index == UOI_FLAGS) {
        constexpr u32 needed = 12;
        if (needed_out != 0) {
            (void)m.write_u32(needed_out, needed);
        }
        if (info == 0 || length < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        u32 dw_flags = 0;
        auto flags_it = m_user_object_dw_flags.find(handle);
        if (flags_it != m_user_object_dw_flags.end()) {
            dw_flags = flags_it->second;
        } else if (station != m_window_stations.end() &&
                   station->second == "WinSta0") {
            dw_flags = WSF_VISIBLE;
        }
        (void)m.write_u32(info + 0, 0);
        (void)m.write_u32(info + 4, 0);
        (void)m.write_u32(info + 8, dw_flags);
        m.set_return(1);
        return;
    }

    if (index == UOI_IO) {
        if (desktop == m_desktops.end()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
            return;
        }
        constexpr u32 needed = 4;
        if (needed_out != 0) {
            (void)m.write_u32(needed_out, needed);
        }
        if (info == 0 || length < needed) {
            set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
            m.set_return(0);
            return;
        }
        bool is_input = (handle == m_input_desktop);
        if (!is_input) {
            auto input_it = m_desktops.find(m_input_desktop);
            if (input_it != m_desktops.end() &&
                input_it->second == desktop->second) {
                is_input = true;
            }
        }
        (void)m.write_u32(info, is_input ? 1u : 0u);
        m.set_return(1);
        return;
    }

    std::string value;
    if (index == UOI_NAME) {
        value = station != m_window_stations.end() ? station->second
                                                  : desktop->second;
    } else if (index == UOI_TYPE) {
        value = station != m_window_stations.end() ? "WindowStation" : "Desktop";
    } else {
        static std::atomic_flag reported = ATOMIC_FLAG_INIT;
        if (!reported.test_and_set()) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED user32!GetUserObjectInformationA: "
                "index=%u\n",
                index);
        }
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }

    const u32 needed = static_cast<u32>(value.size() + 1);
    if (needed_out != 0) {
        (void)m.write_u32(needed_out, needed);
    }
    if (info == 0 || length < needed) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        m.set_return(0);
        return;
    }
    (void)m.write_ansi_string(info, value);
    m.set_return(1);
}

void User32ExtManager::impl_OpenInputDesktop(Marshaler& m) {
    // Deliberately in-memory only: desktop identity and handles are USER
    // objects owned by this manager, not native Cocoa windows or workspaces.
    (void)m.arg(0); // dwFlags
    const bool inherit = m.arg32(1) != 0; // fInherit
    (void)m.arg(2); // dwDesiredAccess

    // MS Learn (winuser!OpenInputDesktop): opens the desktop that receives
    // user input — not necessarily the calling thread's current desktop.
    // Prefer the name recorded by SwitchDesktop; fall back to "Default".
    // Return a distinct handle so CloseDesktop can succeed without ERROR_BUSY
    // on the thread's current-desktop association.
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    std::string input_name = "Default";
    auto it = m_desktops.find(m_input_desktop);
    if (it != m_desktops.end()) {
        input_name = it->second;
    }
    const u64 handle = m_next_desktop++;
    m_desktops.emplace(handle, input_name);
    m_user_object_dw_flags.emplace(handle, 0u);
    note_user_handle(handle, inherit);
    ipc::ipc_log("OpenInputDesktop name='%s' -> h=0x%llx",
                 input_name.c_str(),
                 static_cast<unsigned long long>(handle));
    // Success leaves the last error untouched; no dwFlags or access value
    // is refused (s2msg_inputdesktop_probe open_*).
    m.set_return(handle);
}

void User32ExtManager::impl_OpenDesktopA(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto name_result = m.read_ansi_string(name_ptr);
    if (name_result.is_err() || name_result.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    const u64 handle = open_desktop_by_name(name_result.value());
    if (handle == 0) {
        ipc::ipc_log("OpenDesktopA name='%s' -> NULL (not found)",
                     name_result.value().c_str());
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0);
        return;
    }
    note_user_handle(handle, m.arg32(2) != 0); // fInherit
    ipc::ipc_log("OpenDesktopA name='%s' -> h=0x%llx",
                 name_result.value().c_str(),
                 static_cast<unsigned long long>(handle));
    m.set_return(handle);
}

void User32ExtManager::impl_OpenDesktopW(Marshaler& m) {
    const GuestAddr name_ptr = m.arg_ptr(0);
    (void)m.arg(1);
    (void)m.arg(2);
    (void)m.arg(3);
    if (name_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    auto name_result = m.read_wide_string(name_ptr);
    if (name_result.is_err() || name_result.value().empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    const u64 handle = open_desktop_by_name(name_result.value());
    if (handle == 0) {
        ipc::ipc_log("OpenDesktopW name='%s' -> NULL (not found)",
                     name_result.value().c_str());
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        m.set_return(0);
        return;
    }
    note_user_handle(handle, m.arg32(2) != 0); // fInherit
    ipc::ipc_log("OpenDesktopW name='%s' -> h=0x%llx",
                 name_result.value().c_str(),
                 static_cast<unsigned long long>(handle));
    m.set_return(handle);
}

void User32ExtManager::impl_CloseDesktop(Marshaler& m) {
    const u64 handle = m.arg(0);
    std::lock_guard<std::mutex> objects(m_user_object_mutex);
    auto it = m_desktops.find(handle);
    if (it == m_desktops.end()) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }
    if (handle == m_thread_desktop) {
        constexpr u32 ERROR_BUSY = 170;
        set_last_error(ERROR_BUSY);
        m.set_return(0);
        return;
    }
    m_user_object_dw_flags.erase(handle);
    m_desktops.erase(it);
    (void)synthetic_handles::close(static_cast<Handle>(handle));
    m.set_return(1);
}

void User32ExtManager::impl_EnumDesktopWindows(Marshaler& m) {
    // BOOL EnumDesktopWindows(HDESK hDesktop, WNDENUMPROC lpfn, LPARAM)
    // Learn: "If this parameter is NULL, the current desktop is used." Every
    // guest window lives on the thread's desktop, so NULL, the thread's
    // desktop, and any handle naming the same desktop enumerate the top-level
    // windows exactly as EnumWindows does (measured, s1win_api_probe
    // enumdesktopwindows_*). Another desktop holds no Vitreo windows.
    const u64 handle = m.arg(0);
    const GuestAddr enum_proc = m.arg_ptr(1);
    const u64 lparam = m.arg(2);
    bool same_desktop = handle == 0 || handle == m_thread_desktop;
    if (!same_desktop) {
        // Released before the enumeration: WNDENUMPROC is guest code.
        std::lock_guard<std::mutex> objects(m_user_object_mutex);
        auto it = m_desktops.find(handle);
        auto current = m_desktops.find(m_thread_desktop);
        same_desktop = it != m_desktops.end() && current != m_desktops.end() &&
                       it->second == current->second;
    }
    if (same_desktop) {
        impl_EnumWindows_values(m, enum_proc, lparam);
        return;
    }
    VITREO_TRACE("[u32ext] EnumDesktopWindows(0x%llx) -> TRUE (no windows on it)\n",
                 static_cast<unsigned long long>(handle));
    m.set_return(1u);
}

} // namespace vitreo::winapi

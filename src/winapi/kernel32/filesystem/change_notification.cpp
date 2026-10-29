// SPDX-License-Identifier: LGPL-2.1-only

/// @file change_notification.cpp
/// @brief FindFirst/Next/CloseChangeNotification and EnumResourceNames.
///
/// Directory-change wait handles and resource-name enumeration from
/// the tail of the Additional Win32 stubs run.
///
/// Closed cut from kernel32_ext.cpp: nothing these functions define is used
/// elsewhere in that file, and they use nothing file-local from elsewhere —
/// verified in both directions with tools/architecture/plan_extraction.py
/// before moving.

#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/kernel32_ext_internal.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
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
#include "winapi/kernel32/process/vectored_exception.hpp"

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

void Kernel32ExtManager::impl_FindFirstChangeNotificationW(Marshaler& m) {
    // HANDLE FindFirstChangeNotificationW(LPCWSTR lpPathName,
    //   BOOL bWatchSubtree, DWORD dwNotifyFilter)
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/
    // nf-fileapi-findfirstchangenotificationw
    const GuestAddr path_ptr = m.arg_ptr(0);
    const bool watch_subtree = m.arg(1) != 0;
    const u32 notify_filter = static_cast<u32>(m.arg(2));
    if (path_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(INVALID_HANDLE);
        return;
    }
    const std::string path = m.read_wide_string(path_ptr).value_or("");
    const Handle event =
        start_change_notification(path, watch_subtree, notify_filter);
    if (event != INVALID_HANDLE && event != NULL_HANDLE) {
        ipc::ipc_log(
            "FindFirstChangeNotificationW path='%s' subtree=%d filter=0x%x "
            "-> h=0x%x",
            path.c_str(), watch_subtree ? 1 : 0, notify_filter,
            static_cast<unsigned>(event));
    } else {
        ipc::ipc_log(
            "FindFirstChangeNotificationW path='%s' subtree=%d filter=0x%x "
            "-> INVALID_HANDLE gle=0x%x",
            path.c_str(), watch_subtree ? 1 : 0, notify_filter,
            get_last_error());
    }
    m.set_return(event);
}

void Kernel32ExtManager::impl_FindFirstChangeNotificationA(Marshaler& m) {
    // HANDLE FindFirstChangeNotificationA(LPCSTR, BOOL, DWORD)
    const GuestAddr path_ptr = m.arg_ptr(0);
    const bool watch_subtree = m.arg(1) != 0;
    const u32 notify_filter = static_cast<u32>(m.arg(2));
    if (path_ptr == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(INVALID_HANDLE);
        return;
    }
    const std::string path = m.read_ansi_string(path_ptr).value_or("");
    const Handle event =
        start_change_notification(path, watch_subtree, notify_filter);
    if (event != INVALID_HANDLE) {
        ipc::ipc_log(
            "FindFirstChangeNotificationA path='%s' subtree=%d filter=0x%x "
            "-> h=0x%x",
            path.c_str(), watch_subtree ? 1 : 0, notify_filter,
            static_cast<unsigned>(event));
    }
    m.set_return(event);
}

void Kernel32ExtManager::impl_FindNextChangeNotification(Marshaler& m) {
    // BOOL FindNextChangeNotification(HANDLE hChangeHandle)
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/
    // nf-fileapi-findnextchangenotification
    const Handle handle = static_cast<Handle>(m.arg(0));
    std::shared_ptr<ChangeNotificationWatch> watch;
    {
        std::lock_guard lock(m_change_watch_mutex);
        const auto found = m_change_watches.find(handle);
        if (found != m_change_watches.end()) watch = found->second;
    }
    if (!watch || !m_sync_manager) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    const bool had_pending =
        watch->pending.exchange(false, std::memory_order_acq_rel);
    if (had_pending) {
        // Recorded change immediately satisfies the subsequent wait.
        if (!m_sync_manager->set_event(handle)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        watch->armed.store(false, std::memory_order_release);
    } else {
        (void)m_sync_manager->reset_event(handle);
        watch->armed.store(true, std::memory_order_release);
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_FindCloseChangeNotification(Marshaler& m) {
    // BOOL FindCloseChangeNotification(HANDLE hChangeHandle)
    // https://learn.microsoft.com/en-us/windows/win32/api/fileapi/
    // nf-fileapi-findclosechangenotification
    const Handle handle = static_cast<Handle>(m.arg(0));
    std::shared_ptr<ChangeNotificationWatch> watch;
    {
        std::lock_guard lock(m_change_watch_mutex);
        const auto found = m_change_watches.find(handle);
        if (found == m_change_watches.end()) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        watch = found->second;
        m_change_watches.erase(found);
    }
    stop_change_notification_watch(watch);
    if (m_sync_manager && watch->event_handle != NULL_HANDLE) {
        (void)m_sync_manager->close_handle(watch->event_handle);
        watch->event_handle = NULL_HANDLE;
    }
    m.set_return(1);
}

void Kernel32ExtManager::impl_EnumResourceNamesW(Marshaler& m) {
    enum_resource_names_impl(m, /*wide=*/true);
}

void Kernel32ExtManager::impl_EnumResourceNamesA(Marshaler& m) {
    enum_resource_names_impl(m, /*wide=*/false);
}

void Kernel32ExtManager::enum_resource_names_impl(Marshaler& m, bool wide) {
    // BOOL EnumResourceNames(HMODULE hModule, LPCTSTR lpType,
    //                        ENUMRESNAMEPROC lpEnumFunc, LONG_PTR lParam)
    // Learn: HMODULE and LPCTSTR are pointer-width. Truncating to u32 rejects
    // x64 image bases and type-name strings above 4 GiB
    // (EnumResourceNames of a high base returns TYPE_NOT_FOUND).
    const GuestAddr hModule = m.arg_ptr(0);
    const GuestAddr lpType = m.arg_ptr(1);
    const GuestAddr callback = m.arg_ptr(2);
    const u64 lparam = m.arg(3);

    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    GuestAddr module_base = m_rsrc_image_base;
    u32 rsrc_rva = m_rsrc_rva;
    u32 rsrc_size = m_rsrc_size;
    if (hModule != 0 && hModule != m_rsrc_image_base) {
        u32 rva = 0;
        u32 size = 0;
        if (m_module_manager &&
            m_module_manager->resource_directory_for_base(hModule, rva, size)) {
            module_base = hModule;
            rsrc_rva = rva;
            rsrc_size = size;
        } else {
            set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
            m.set_return(0);
            return;
        }
    }

    if (rsrc_rva == 0 || rsrc_size == 0) {
        // A module with NO resource directory reports
        // ERROR_RESOURCE_DATA_NOT_FOUND (1812); TYPE_NOT_FOUND (1813) means
        // the module HAS resources but not this type. Verified against
        // Windows with an EXE carrying no string table.
        set_last_error(error_codes::ERROR_RESOURCE_DATA_NOT_FOUND);
        m.set_return(0);
        return;
    }

    u16 type_id = 0;
    bool type_is_string = false;
    std::u16string type_str;
    // IS_INTRESOURCE: (((ULONG_PTR)r) >> 16) == 0
    if ((lpType >> 16) == 0) {
        type_id = static_cast<u16>(lpType);
    } else {
        type_is_string = true;
        if (wide) {
            auto r = m.read_wide_string(lpType);
            if (r.is_err()) {
                set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
                m.set_return(0);
                return;
            }
            for (char c : r.value()) {
                type_str.push_back(static_cast<char16_t>(
                    static_cast<unsigned char>(
                        c >= 'a' && c <= 'z' ? c - 32 : c)));
            }
        } else {
            auto r = m.read_ansi_string(lpType);
            if (r.is_err()) {
                set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
                m.set_return(0);
                return;
            }
            for (char c : r.value()) {
                type_str.push_back(static_cast<char16_t>(
                    static_cast<unsigned char>(
                        c >= 'a' && c <= 'z' ? c - 32 : c)));
            }
        }
    }

    auto names = windowing::enumerate_resource_names(
        m_memory, module_base, rsrc_rva, rsrc_size, type_id, type_is_string,
        type_str.empty() ? nullptr : type_str.c_str(),
        static_cast<u16>(type_str.size()));
    if (names.empty()) {
        set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
        m.set_return(0);
        return;
    }

    auto ensure_scratch = [this, &m]() -> GuestAddr {
        if (m_enum_resource_scratch != 0) {
            return m_enum_resource_scratch;
        }
        constexpr usize kSize = 0x1000;
        constexpr GuestAddr kMin = 0x5A200000;
        GuestAddr base = m.memory().find_free_gap(kMin, kSize);
        if (base == 0 ||
            m.memory().map_region(base, kSize, MemoryProtection::ReadWrite)
                .is_err()) {
            return 0;
        }
        m_enum_resource_scratch = base;
        return base;
    };

    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr module_arg = hModule != 0 ? hModule : module_base;

    for (const auto& entry : names) {
        GuestAddr name_arg = 0;
        if (entry.is_id) {
            name_arg = static_cast<GuestAddr>(entry.id);
        } else {
            GuestAddr scratch = ensure_scratch();
            if (scratch == 0) {
                set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                m.set_return(0);
                return;
            }
            if (wide) {
                for (usize i = 0; i < entry.name.size(); ++i) {
                    (void)m.memory().write_u16(
                        scratch + static_cast<u64>(i) * 2,
                        static_cast<u16>(entry.name[i]));
                }
                (void)m.memory().write_u16(
                    scratch + static_cast<u64>(entry.name.size()) * 2, 0);
            } else {
                for (usize i = 0; i < entry.name.size(); ++i) {
                    const char16_t ch = entry.name[i];
                    const u8 ansi = ch < 0x100 ? static_cast<u8>(ch) : static_cast<u8>('?');
                    (void)m.memory().write_u8(scratch + i, ansi);
                }
                (void)m.memory().write_u8(scratch + entry.name.size(), 0);
            }
            name_arg = scratch;
        }

        // BOOL CALLBACK EnumResNameProc(HMODULE, LPCTSTR type, LPTSTR name, LONG_PTR)
        u64 args[4] = {
            static_cast<u64>(module_arg),
            static_cast<u64>(lpType),
            static_cast<u64>(name_arg),
            lparam,
        };
        const u64 cb_ret = m_guest_call(callback, args, 4, is_64bit);
        if (cb_ret == 0) {
            // Measured on Windows 11 (last_error_on_success_probe.expected):
            // a callback that stops the enumeration makes it FAIL with
            // ERROR_RESOURCE_ENUM_USER_STOP; a completed enumeration leaves
            // the last error untouched.
            set_last_error(error_codes::ERROR_RESOURCE_ENUM_USER_STOP);
            m.set_return(0);
            return;
        }
    }

    m.set_return(1);
}

void Kernel32ExtManager::enum_resource_languages_impl(Marshaler& m, bool wide) {
    // BOOL EnumResourceLanguages(HMODULE, LPCTSTR type, LPCTSTR name,
    //                            ENUMRESLANGPROC, LONG_PTR)
    const GuestAddr hModule = m.arg_ptr(0);
    const GuestAddr lpType = m.arg_ptr(1);
    const GuestAddr lpName = m.arg_ptr(2);
    const GuestAddr callback = m.arg_ptr(3);
    const u64 lparam = m.arg(4);

    if (callback == 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (!m_guest_call) {
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        m.set_return(0);
        return;
    }

    GuestAddr module_base = m_rsrc_image_base;
    u32 rsrc_rva = m_rsrc_rva;
    u32 rsrc_size = m_rsrc_size;
    if (hModule != 0 && hModule != m_rsrc_image_base) {
        u32 rva = 0;
        u32 size = 0;
        if (m_module_manager &&
            m_module_manager->resource_directory_for_base(hModule, rva, size)) {
            module_base = hModule;
            rsrc_rva = rva;
            rsrc_size = size;
        } else {
            set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
            m.set_return(0);
            return;
        }
    }

    if (rsrc_rva == 0 || rsrc_size == 0) {
        // A module with NO resource directory reports
        // ERROR_RESOURCE_DATA_NOT_FOUND (1812); TYPE_NOT_FOUND (1813) means
        // the module HAS resources but not this type. Verified against
        // Windows with an EXE carrying no string table.
        set_last_error(error_codes::ERROR_RESOURCE_DATA_NOT_FOUND);
        m.set_return(0);
        return;
    }

    auto read_resource_string = [&m, wide](GuestAddr value,
                                           std::u16string& out) -> bool {
        if (wide) {
            auto r = m.read_wide_string(value);
            if (r.is_err()) {
                return false;
            }
            for (char c : r.value()) {
                out.push_back(static_cast<char16_t>(
                    static_cast<unsigned char>(
                        c >= 'a' && c <= 'z' ? c - 32 : c)));
            }
            return true;
        }
        auto r = m.read_ansi_string(value);
        if (r.is_err()) {
            return false;
        }
        for (char c : r.value()) {
            out.push_back(static_cast<char16_t>(
                static_cast<unsigned char>(
                    c >= 'a' && c <= 'z' ? c - 32 : c)));
        }
        return true;
    };

    u16 type_id = 0;
    bool type_is_string = false;
    std::u16string type_str;
    if ((lpType >> 16) == 0) {
        type_id = static_cast<u16>(lpType);
    } else {
        type_is_string = true;
        if (!read_resource_string(lpType, type_str)) {
            set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
            m.set_return(0);
            return;
        }
    }

    u16 name_id = 0;
    bool name_is_string = false;
    std::u16string name_str;
    if ((lpName >> 16) == 0) {
        name_id = static_cast<u16>(lpName);
    } else {
        name_is_string = true;
        if (!read_resource_string(lpName, name_str)) {
            set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
            m.set_return(0);
            return;
        }
    }

    auto found = windowing::enumerate_resource_languages(
        m_memory, module_base, rsrc_rva, rsrc_size, type_id, type_is_string,
        type_str.empty() ? nullptr : type_str.c_str(),
        static_cast<u16>(type_str.size()), name_id, name_is_string,
        name_str.empty() ? nullptr : name_str.c_str(),
        static_cast<u16>(name_str.size()));
    if (found.status == windowing::ResourceLanguageEnum::Status::TypeNotFound) {
        set_last_error(error_codes::ERROR_RESOURCE_TYPE_NOT_FOUND);
        m.set_return(0);
        return;
    }
    if (found.status == windowing::ResourceLanguageEnum::Status::NameNotFound) {
        set_last_error(error_codes::ERROR_RESOURCE_NAME_NOT_FOUND);
        m.set_return(0);
        return;
    }
    if (found.languages.empty()) {
        set_last_error(error_codes::ERROR_RESOURCE_LANG_NOT_FOUND);
        m.set_return(0);
        return;
    }

    const bool is_64bit = m.cpu_is_64bit();
    const GuestAddr module_arg = hModule != 0 ? hModule : module_base;
    for (u16 language : found.languages) {
        // BOOL CALLBACK EnumResLangProc(HMODULE, LPCTSTR, LPCTSTR, WORD, LONG_PTR)
        u64 args[5] = {
            static_cast<u64>(module_arg),
            static_cast<u64>(lpType),
            static_cast<u64>(lpName),
            static_cast<u64>(language),
            lparam,
        };
        const u64 cb_ret = m_guest_call(callback, args, 5, is_64bit);
        if (cb_ret == 0) {
            // Measured on Windows 11 (last_error_on_success_probe.expected):
            // a callback that stops the enumeration makes it FAIL with
            // ERROR_RESOURCE_ENUM_USER_STOP ...
            set_last_error(error_codes::ERROR_RESOURCE_ENUM_USER_STOP);
            m.set_return(0);
            return;
        }
    }

    // ... while a completed enumeration SETS ERROR_SUCCESS (unlike
    // EnumResourceNames, which leaves the last error untouched).
    set_last_error(error_codes::ERROR_SUCCESS);
    m.set_return(1);
}

void Kernel32ExtManager::impl_AddVectoredExceptionHandler(Marshaler& m) {
    // Learn: AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER)
    // First != 0 inserts at the front; First == 0 appends. Failure returns NULL.
    const u32 first = static_cast<u32>(m.arg(0));
    const u64 handler = m.arg(1);
    const u64 handle = add_vectored_exception_handler(first, handler);
    VITREO_TRACE("[k32ext] AddVectoredExceptionHandler(first=%u, handler=0x%llx) -> 0x%llx\n",
                 first,
                 static_cast<unsigned long long>(handler),
                 static_cast<unsigned long long>(handle));
    m.set_return(handle);
}

void Kernel32ExtManager::impl_RemoveVectoredExceptionHandler(Marshaler& m) {
    // Learn: RemoveVectoredExceptionHandler(PVOID Handle) — nonzero on success.
    const u64 handle = m.arg(0);
    const bool removed = remove_vectored_exception_handler(handle);
    VITREO_TRACE("[k32ext] RemoveVectoredExceptionHandler(0x%llx) -> %u\n",
                 static_cast<unsigned long long>(handle), removed ? 1u : 0u);
    m.set_return(removed ? 1u : 0u);
}

} // namespace vitreo::winapi

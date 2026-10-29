// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_cef_wer.cpp
/// @brief Extended kernel32 export table, part 8 of 9: process-query exports and the Wer* family.
///
/// A contiguous slice of the table that create_kernel32_ext_thunks builds
/// (see kernel32_ext_registration.cpp). The slice boundaries follow the
/// table's registration order, which is load-bearing: a name registered
/// twice resolves last-wins, and thunk addresses are assigned in
/// registration order. Do not reorder entries or call the parts out of
/// order.

#include "winapi/kernel32/kernel32_ext_registration_parts.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/advapi32/security/security.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/file_mapping.hpp"
#include "winapi/kernel32/filesystem/file_search.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/process/process.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/kernel32/synchronization/critical_section.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/system/ntdll/ntdll_ext.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/memory/remote_memory.hpp"
#include "core/pe/loader/teb_setup.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "windowing/resources/resource_walker.hpp"

#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win_path.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/thread/restore_context64.hpp"

#include "winapi/kernel32/synchronization/slist.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"

#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <string>
#include <unistd.h>

namespace vitreo::winapi {

namespace {
std::mutex g_wer_metadata_mu;
std::unordered_map<std::string, std::string> g_wer_custom_metadata;
} // namespace

void append_cef_wer_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // ------------------------------------------------------------------------
    // Process-query and Wer* exports: previously stubbed with fail-thunks
    // ------------------------------------------------------------------------

    exports.entries.push_back(ExportEntry{
        "GetThreadPriorityBoost", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            GuestAddr disable_boost_ptr = m.arg_ptr(1);
            if (disable_boost_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            (void)m.memory().write_u32(disable_boost_ptr, 0);
            m.set_return(1);
        },
        8
    });

    exports.entries.push_back(ExportEntry{
        "SetThreadPriorityBoost", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg(1);
            m.set_return(1);
        },
        8
    });

    // Learn processthreadsapi: BOOL SetProcessPriorityBoost(HANDLE, BOOL)
    exports.entries.push_back(ExportEntry{
        "SetProcessPriorityBoost", 0,
        [](Marshaler& m) {
            if (m.arg(0) == 0) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            (void)m.arg(1);
            m.set_return(1);
        },
        8
    });

    // kernel32 forwards the Rtl* function-table APIs to ntdll on Windows, and
    // coreclr imports them FROM KERNEL32 -- registering them only in ntdll
    // left the kernel32 import resolving to the missing-export stub. Same
    // registry as the ntdll copies (ModuleManager), which is where the
    // unwinder looks.
    exports.entries.push_back(ExportEntry{
        "RtlAddFunctionTable", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr table = m.arg_ptr(0);
            const u32 count = static_cast<u32>(m.arg(1));
            const GuestAddr base = static_cast<GuestAddr>(m.arg(2));
            auto* modules = mgr.module_manager();
            if (modules == nullptr) { m.set_return(0); return; }
            m.set_return(
                modules->add_dynamic_function_table(table, count, base) ? 1u : 0u);
        },
        12
    });

    exports.entries.push_back(ExportEntry{
        "RtlInstallFunctionTableCallback", 0,
        [&mgr](Marshaler& m) {
            const GuestAddr identifier = static_cast<GuestAddr>(m.arg(0));
            const GuestAddr base = static_cast<GuestAddr>(m.arg(1));
            const u32 length = static_cast<u32>(m.arg(2));
            const GuestAddr callback = m.arg_ptr(3);
            const GuestAddr context = m.arg_ptr(4);
            (void)m.arg_ptr(5);  // OutOfProcessCallbackDll: no such consumer
            auto* modules = mgr.module_manager();
            if (modules == nullptr) { m.set_return(0); return; }
            const bool ok = modules->add_dynamic_function_table_callback(
                identifier, base, length, callback, context);
            VITREO_TRACE("[seh] RtlInstallFunctionTableCallback(id=0x%llx "
                         "base=0x%llx len=0x%x) -> %d\n",
                         static_cast<unsigned long long>(identifier),
                         static_cast<unsigned long long>(base), length,
                         ok ? 1 : 0);
            m.set_return(ok ? 1u : 0u);
        },
        24
    });

    exports.entries.push_back(ExportEntry{
        "RtlDeleteFunctionTable", 0,
        [&mgr](Marshaler& m) {
            auto* modules = mgr.module_manager();
            if (modules == nullptr) { m.set_return(0); return; }
            m.set_return(modules->remove_dynamic_function_table(
                static_cast<GuestAddr>(m.arg(0))) ? 1u : 0u);
        },
        4
    });

    exports.entries.push_back(ExportEntry{
        "Wow64GetThreadContext", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            (void)m.arg_ptr(1);
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            m.set_return(0);
        },
        8
    });

    // SetDllDirectoryW(LPCWSTR)
    exports.entries.push_back(ExportEntry{
        "SetDllDirectoryW", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetDllDirectoryW(m); },
        4
    });

    // BOOL QueryFullProcessImageNameW(HANDLE, DWORD dwFlags, LPWSTR, PDWORD lpdwSize)
    exports.entries.push_back(ExportEntry{
        "QueryFullProcessImageNameW", 0,
        [&mgr](Marshaler& m) {
            Handle process_handle = static_cast<Handle>(m.arg(0));
            (void)m.arg(1);              // dwFlags  (0 = win32 path, 1 = native)
            GuestAddr buf_ptr = m.arg_ptr(2);
            GuestAddr size_ptr = m.arg_ptr(3);
            if (buf_ptr == 0 || size_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            auto cap_res = m.read_u32(size_ptr);
            u32 cap_chars = cap_res.is_ok() ? cap_res.value() : 0u;
            if (cap_chars == 0) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            auto path = mgr.process_image_path(process_handle);
            if (!path) {
                ipc::ipc_log(
                    "QueryFullProcessImageNameW h=0x%x -> INVALID_HANDLE",
                    static_cast<unsigned>(process_handle));
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            const u32 len = static_cast<u32>(path->size());
            if (len >= cap_chars) {
                (void)m.memory().write_u32(size_ptr, len + 1);
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            (void)m.write_wide_string(buf_ptr, *path);
            (void)m.memory().write_u32(size_ptr, len);
            ipc::ipc_log(
                "QueryFullProcessImageNameW h=0x%x -> '%s'",
                static_cast<unsigned>(process_handle), path->c_str());
            m.set_return(1);
        },
        16
    });

    // BOOL QueryFullProcessImageNameA — same, ANSI buffer.
    exports.entries.push_back(ExportEntry{
        "QueryFullProcessImageNameA", 0,
        [&mgr](Marshaler& m) {
            Handle process_handle = static_cast<Handle>(m.arg(0));
            (void)m.arg(1);
            GuestAddr buf_ptr = m.arg_ptr(2);
            GuestAddr size_ptr = m.arg_ptr(3);
            if (buf_ptr == 0 || size_ptr == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            auto cap_res = m.read_u32(size_ptr);
            u32 cap_chars = cap_res.is_ok() ? cap_res.value() : 0u;
            if (cap_chars == 0) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            auto path = mgr.process_image_path(process_handle);
            if (!path) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0);
                return;
            }
            const u32 len = static_cast<u32>(path->size());
            if (len >= cap_chars) {
                (void)m.memory().write_u32(size_ptr, len + 1);
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            (void)m.write_ansi_string(buf_ptr, *path);
            (void)m.memory().write_u32(size_ptr, len);
            VITREO_TRACE("[k32ext] QueryFullProcessImageNameA(handle=0x%x) -> '%s' (%u chars)\n",
                         process_handle, path->c_str(), len);
            m.set_return(1);
        },
        16
    });

    // BOOL QuirkIsEnabled(REFGUID quirk)
    // Returns FALSE for unknown quirks (no quirk catalog on this host).
    exports.entries.push_back(ExportEntry{
        "QuirkIsEnabled", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            m.set_return(0);
        },
        4
    });

    // HRESULT WerRegisterCustomMetadata(PCWSTR key, PCWSTR value)
    // https://learn.microsoft.com/en-us/windows/win32/api/werapi/nf-werapi-werregistercustommetadata
    exports.entries.push_back(ExportEntry{
        "WerRegisterCustomMetadata", 0,
        [](Marshaler& m) {
            constexpr u32 E_INVALIDARG = 0x80070057u;
            const GuestAddr key_ptr = m.arg_ptr(0);
            const GuestAddr value_ptr = m.arg_ptr(1);
            if (key_ptr == 0 || value_ptr == 0) {
                m.set_return(E_INVALIDARG);
                return;
            }
            auto key = m.read_wide_string(key_ptr, 65);
            auto value = m.read_wide_string(value_ptr, 129);
            if (key.is_err() || value.is_err() ||
                key.value().empty() || key.value().size() > 64 ||
                value.value().size() > 128) {
                m.set_return(E_INVALIDARG);
                return;
            }
            for (unsigned char ch : value.value()) {
                if (ch < 0x20 || ch > 0x7E) {
                    m.set_return(E_INVALIDARG);
                    return;
                }
            }
            {
                std::lock_guard lock(g_wer_metadata_mu);
                g_wer_custom_metadata[key.value()] = value.value();
            }
            m.set_return(0); // S_OK
        },
        8
    });

    // HRESULT WerUnregisterCustomMetadata(PCWSTR key)
    exports.entries.push_back(ExportEntry{
        "WerUnregisterCustomMetadata", 0,
        [](Marshaler& m) {
            constexpr u32 E_INVALIDARG = 0x80070057u;
            constexpr u32 WER_E_NOT_FOUND = 0x80070002u;
            const GuestAddr key_ptr = m.arg_ptr(0);
            if (key_ptr == 0) {
                m.set_return(E_INVALIDARG);
                return;
            }
            auto key = m.read_wide_string(key_ptr, 65);
            if (key.is_err()) {
                m.set_return(E_INVALIDARG);
                return;
            }
            {
                std::lock_guard lock(g_wer_metadata_mu);
                if (g_wer_custom_metadata.erase(key.value()) == 0) {
                    m.set_return(WER_E_NOT_FOUND);
                    return;
                }
            }
            m.set_return(0);
        },
        4
    });

    // HRESULT WerRegisterRuntimeExceptionModule(PCWSTR pwszOutOfProcessCallbackDll, PVOID pContext)
    // Vista+ WER — return S_OK so callers continue.
    exports.entries.push_back(ExportEntry{
        "WerRegisterRuntimeExceptionModule", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            VITREO_TRACE("[k32ext] WerRegisterRuntimeExceptionModule -> S_OK\n");
            m.set_return(0);
        },
        8
    });

    // BOOL WerUnregisterRuntimeExceptionModule — pair (no harm to add).
    exports.entries.push_back(ExportEntry{
        "WerUnregisterRuntimeExceptionModule", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1);
            m.set_return(0);
        },
        8
    });

    // WerReport* (ext-ms-win-wer-reporting): report creation/submit is not
    // hosted on macOS — fail loudly with E_NOTIMPL rather than fake a handle.
    auto wer_report_notimpl = [](Marshaler& m, const char* api) {
        static std::once_flag once;
        std::call_once(once, [api] {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED kernel32!%s: WER report "
                         "submission is not available on this host\n",
                         api);
        });
        m.set_return(0x80004001u); // E_NOTIMPL
    };
    exports.entries.push_back(ExportEntry{
        "WerReportCreate", 0,
        [wer_report_notimpl](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg(1); (void)m.arg_ptr(2);
            (void)m.arg_ptr(3);
            wer_report_notimpl(m, "WerReportCreate");
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WerReportSetParameter", 0,
        [wer_report_notimpl](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg(1); (void)m.arg_ptr(2);
            (void)m.arg_ptr(3);
            wer_report_notimpl(m, "WerReportSetParameter");
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WerReportAddDump", 0,
        [wer_report_notimpl](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg_ptr(1); (void)m.arg(2);
            (void)m.arg_ptr(3); (void)m.arg_ptr(4); (void)m.arg(5);
            (void)m.arg_ptr(6); (void)m.arg_ptr(7); (void)m.arg(8);
            wer_report_notimpl(m, "WerReportAddDump");
        },
        36
    });
    exports.entries.push_back(ExportEntry{
        "WerReportSubmit", 0,
        [wer_report_notimpl](Marshaler& m) {
            (void)m.arg_ptr(0); (void)m.arg(1); (void)m.arg(2);
            (void)m.arg_ptr(3);
            wer_report_notimpl(m, "WerReportSubmit");
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "WerReportCloseHandle", 0,
        [wer_report_notimpl](Marshaler& m) {
            (void)m.arg_ptr(0);
            wer_report_notimpl(m, "WerReportCloseHandle");
        },
        4
    });

    // BOOL GetMachineTypeAttributes(USHORT Machine, MACHINE_ATTRIBUTES* MachineAttributes)
    // Win11 24H2. Report USER_ENABLED for IMAGE_FILE_MACHINE_AMD64 and the
    // current process machine (i386).
    exports.entries.push_back(ExportEntry{
        "GetMachineTypeAttributes", 0,
        [](Marshaler& m) {
            u32 machine = static_cast<u32>(m.arg(0)) & 0xFFFFu;
            GuestAddr out = m.arg_ptr(1);
            if (out == 0) {
                m.set_return(0x80070057u); // E_INVALIDARG
                return;
            }
            // MACHINE_ATTRIBUTES bits: UserEnabled=1, KernelEnabled=2, Wow64Container=4
            u32 attrs = 0;
            if (machine == 0x014C /* IMAGE_FILE_MACHINE_I386  */ ||
                machine == 0x8664 /* IMAGE_FILE_MACHINE_AMD64 */) {
                attrs = 1;
            }
            (void)m.memory().write_u32(out, attrs);
            m.set_return(0); // S_OK
        },
        8
    });

    // BOOL IsValidLocaleName(LPCWSTR lpLocaleName)
    exports.entries.push_back(ExportEntry{
        "IsValidLocaleName", 0,
        [](Marshaler& m) {
            (void)m.arg_ptr(0);
            m.set_return(1);
        },
        4
    });

    // HRESULT SetThreadDescription(HANDLE hThread, PCWSTR lpThreadDescription)
    exports.entries.push_back(ExportEntry{
        "SetThreadDescription", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetThreadDescription(m); },
        8
    });

    // HRESULT GetThreadDescription(HANDLE hThread, PWSTR* ppszThreadDescription)
    exports.entries.push_back(ExportEntry{
        "GetThreadDescription", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetThreadDescription(m); },
        8
    });

    // BOOL GetProcessUserModeExceptionPolicy(LPDWORD lpFlags)
    // BOOL SetProcessUserModeExceptionPolicy(DWORD dwFlags)
    // Win7+ legacy: report no policy bits, allow set.
    exports.entries.push_back(ExportEntry{
        "GetProcessUserModeExceptionPolicy", 0,
        [](Marshaler& m) {
            GuestAddr out = m.arg_ptr(0);
            if (out != 0) (void)m.memory().write_u32(out, 0);
            m.set_return(1);
        },
        4
    });
    exports.entries.push_back(ExportEntry{
        "SetProcessUserModeExceptionPolicy", 0,
        [](Marshaler& m) {
            (void)m.arg(0);
            m.set_return(1);
        },
        4
    });
}

} // namespace vitreo::winapi

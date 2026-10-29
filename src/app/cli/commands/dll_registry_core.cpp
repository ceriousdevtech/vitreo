// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: kernelbase surface collection; kernel32.dll and ntdll.dll

#include "winapi/common/c_specific_handler.hpp"
#include "winapi/kernel32/kernel32_ntdll_forwarded.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "ipc/process_handle.hpp"
#include "winapi/system/ntdll/nt_event.hpp"
#include "winapi/system/ntdll/nt_query_virtual_memory.hpp"
#include "dll_registry_internal.hpp"

#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/text/format_message.hpp"
#include "winapi/kernel32/locale/normalization.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/path/pathcch.hpp"
#include "winapi/kernel32/kernel32_in_chain.hpp"
#include "winapi/system/ntdll/ntdll_system_services.hpp"

#include <cctype>
#include <algorithm>
#include <unistd.h>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo::cli {

/// kernelbase.dll's export names, generated from the real DLL
/// (tools/gen_windows_export_names.py). On Windows 10+ KernelBase holds the
/// implementations that kernel32 -- and parts of advapi32, user32 and shlwapi
/// -- forward to, and code resolves them there directly:
/// GetProcAddress(GetModuleHandle("kernelbase.dll"), "LoadLibraryExW").
static const std::unordered_set<std::string_view>& kernelbase_export_names() {
    static const std::unordered_set<std::string_view> names = {
#include "winapi/kernel32/kernelbase_export_names.inc"
    };
    return names;
}

/// Give KernelBase every entry of @p src that the real KernelBase exports --
/// the same implementation, published under the DLL Windows publishes it in.
/// Across DLLs the first registration wins, so KernelBase's own entries come
/// first. Within @p src the LAST entry of a name is taken: that is the one
/// ThunkRegistry::register_dll binds for the source DLL (by_name is
/// last-wins), so kernel32!X and kernelbase!X are one implementation. Taking
/// the first gave KernelBase stale duplicates (Sleep with a u32 overflow,
/// a UTC-copying TzSpecificLocalTimeToSystemTime, a path-blind
/// GetDiskFreeSpaceExW).
void collect_kernelbase_surface(winapi::DllExports& kernelbase,
                                const winapi::DllExports& src,
                                std::unordered_set<std::string>& seen) {
    const auto& names = kernelbase_export_names();
    std::unordered_map<std::string_view, usize> last_index;
    for (usize i = 0; i < src.entries.size(); ++i) {
        if (!src.entries[i].name.empty()) last_index[src.entries[i].name] = i;
    }
    for (usize i = 0; i < src.entries.size(); ++i) {
        const auto& e = src.entries[i];
        if (e.name.empty() || names.count(e.name) == 0 ||
            last_index[e.name] != i || !seen.insert(e.name).second) {
            continue;
        }
        winapi::ExportEntry copy = e;
        copy.ordinal = 0;  // KernelBase's ordinals are its own; bind by name
        kernelbase.entries.push_back(std::move(copy));
    }
}

namespace {

/// kernel32 exports that Windows forwards to ntdll (dumpbin /exports
/// kernel32.dll: "X (forwarded to NTDLL.Y)"). A forwarder is one function
/// under two names: a guest resolving ntdll!Y directly must reach the same
/// code as one calling kernel32!X.
struct Kernel32NtdllForwarder {
    const char* kernel32;
    const char* ntdll;
    bool x64;
    bool x86;
};
constexpr Kernel32NtdllForwarder kKernel32NtdllForwarders[] = {
#include "winapi/kernel32/kernel32_ntdll_forwarders.inc"
};


/// @p path spelled with each existing component's real name: the file's own
/// name is reported, not the spelling it was opened with
/// (nt_query_virtual_memory_probe name_data_view: "...\system32\shell32.dll"
/// opened, "...\System32\shell32.dll" reported). Components that do not
/// exist keep their spelling.
std::string on_disk_case(capsule::Vfs& vfs, const std::string& path) {
    if (path.size() < 3 || path[1] != ':') return path;
    std::string out = path.substr(0, 3);
    usize start = 3;
    while (start < path.size()) {
        const usize end = path.find_first_of("\\/", start);
        const std::string component =
            path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        std::string chosen = component;
        if (auto entries = vfs.list_dir(out); entries.is_ok()) {
            for (const auto& entry : entries.value()) {
                if (entry.name.size() == component.size() &&
                    std::equal(entry.name.begin(), entry.name.end(), component.begin(),
                               [](char a, char b) {
                                   return std::tolower(static_cast<unsigned char>(a)) ==
                                          std::tolower(static_cast<unsigned char>(b));
                               })) {
                    chosen = entry.name;
                    break;
                }
            }
        }
        out += chosen;
        if (end == std::string::npos) break;
        out += '\\';
        start = end + 1;
    }
    return out;
}

}  // namespace

void register_core_dlls(RuntimeContext& ctx, winapi::DllExports& kernelbase, std::unordered_set<std::string>& kernelbase_seen,
                        bool guest_is_64bit) {
    auto& reg = ctx.thunk_registry;

    // -- kernelbase.dll (registered after the DLLs it shares code with) --
    // Modern Windows keeps the low-level implementations for this memory API
    // family in KernelBase even when the SDK import library exposes some of
    // them through Kernel32.
    {
        winapi::DllExports own;
        merge_exports(own, winapi::make_virtual_protect_exports(ctx.vmm, ctx.vqm));
        merge_exports(own, winapi::create_placeholder_mapping_thunks(
                               ctx.fmm_mgr, ctx.vmm));
        // The PathCch family is a KernelBase export in its own right; the
        // api-ms-win-core-path API set is only a forwarder to it.
        merge_exports(own, winapi::make_pathcch_exports());
        for (auto& e : own.entries) {
            kernelbase_seen.insert(e.name);
            kernelbase.entries.push_back(std::move(e));
        }
    }

    // kernel32 entries that ntdll also exports under their forwarder name.
    std::vector<winapi::ExportEntry> forwarded_to_ntdll;

    // -- kernel32.dll --
    {
        winapi::DllExports k32;
        merge_exports(k32, winapi::make_process_exports(ctx.process_mgr, &ctx.gl_mgr));
        merge_exports(k32, winapi::make_process_handle_exports(ctx.process_mgr, ctx.handle_table));
        merge_exports(k32, winapi::make_console_exports(ctx.console_mgr));
        merge_exports(k32, winapi::make_file_exports(ctx.file_mgr));
        merge_exports(k32, winapi::make_filepos_exports(
                               ctx.file_mgr, &ctx.ntdll_ext_mgr));
        merge_exports(k32, winapi::create_sysinfo_thunks(ctx.sysinfo_mgr));
        merge_exports(k32, winapi::create_critical_section_thunks(ctx.cs_mgr));
        merge_exports(k32, winapi::create_fiber_fls_thunks(ctx.fls_mgr));
        merge_exports(k32, winapi::create_file_mapping_thunks(ctx.fmm_mgr));
        k32.entries.push_back({"K32GetMappedFileNameW", 0,
            [&ctx](winapi::Marshaler& m) { ctx.psapi_mgr.get_mapped_file_name(m, true); }, 16});
        k32.entries.push_back({"K32GetMappedFileNameA", 0,
            [&ctx](winapi::Marshaler& m) { ctx.psapi_mgr.get_mapped_file_name(m, false); }, 16});

        merge_exports(k32, winapi::create_placeholder_mapping_thunks(
                               ctx.fmm_mgr, ctx.vmm));
        merge_exports(k32, winapi::create_file_search_thunks(ctx.fsearch_mgr));
        merge_exports(k32, winapi::create_global_local_thunks(ctx.gl_mgr));
        merge_exports(k32, winapi::make_string_conv_exports());
        merge_exports(k32, winapi::create_time_thunks(ctx.time_mgr));
        merge_exports(k32, winapi::make_module_exports(ctx.module_mgr));
        merge_exports(k32, winapi::make_environment_exports(ctx.env_mgr));
        merge_exports(k32, winapi::make_virtual_protect_exports(ctx.vmm, ctx.vqm));
        merge_exports(k32, winapi::create_heap_ext_thunks(ctx.heap_ext, ctx.heap_mgr));
        merge_exports(k32, winapi::make_tls_exports(ctx.tls_mgr));
        merge_exports(k32, winapi::create_thread_thunks(ctx.thread_mgr));
        winapi::add_threadpool_exports(k32, ctx.threadpool_mgr);
        merge_exports(k32, winapi::create_sync_thunks(ctx.sync_mgr));
        merge_exports(k32, winapi::make_handle_exports(ctx.handle_table));
        merge_exports(k32, winapi::make_error_exports());
        merge_exports(k32, winapi::format_message::make_format_message_exports(
                               ctx.k32_ext_mgr, ctx.gl_mgr, ctx.locale_str_mgr));
        merge_exports(k32, winapi::create_locale_string_thunks(ctx.locale_str_mgr));
        merge_exports(k32, winapi::create_file_ops_thunks(ctx.file_ops_mgr));
        merge_exports(k32, winapi::create_kernel32_ext_thunks(ctx.k32_ext_mgr));
        // VerLanguageNameW/A live on both version.dll and kernel32 (forwarder).
        merge_exports(k32, winapi::create_version_thunks(ctx.ver_mgr));
        // PathCch*: api-ms-win-core-path-l1-1-0.dll forwards to KernelBase on
        // Windows, and Vitreo's API-set resolver sends api-ms-win-core-* here.
        merge_exports(k32, winapi::make_pathcch_exports());
        // NormalizeString, IsNormalizedString, IdnToAscii/Unicode/
        // NameprepUnicode: kernel32 and kernelbase both export them by name.
        merge_exports(k32, winapi::nls::create_normalization_thunks());
        // L3-K: the in-chain class of the hot exports, from their contract
        // (kernel32_in_chain.cpp); before the kernelbase copies are taken.
        winapi::kernel32::apply_in_chain_classes(k32);
        // Forwarded exports no other kernel32 family owns.
        merge_exports(k32, winapi::make_kernel32_ntdll_forwarded_exports(
                               guest_is_64bit, ctx.module_mgr, ctx.k32_ext_mgr));
        // x64 _local_unwind(TargetFrame, TargetIp) is
        // RtlUnwind(TargetFrame, TargetIp, NULL, NULL)
        // (k32_ntdll_misc_forwarder_probe local_unwind).
        if (guest_is_64bit) {
            for (auto it = k32.entries.rbegin(); it != k32.entries.rend(); ++it) {
                if (it->internal || it->name != "RtlUnwind" || !it->handler) continue;
                winapi::ExportEntry copy = *it;
                copy.name = "_local_unwind";
                copy.ordinal = 0;
                copy.direct = {};
                copy.in_chain = winapi::InChainClass::None;
                copy.handler = [unwind = it->handler](winapi::Marshaler& m) {
                    m.cpu()->gpr.r8 = 0;
                    m.cpu()->gpr.r9 = 0;
                    unwind(m);
                };
                k32.entries.push_back(std::move(copy));
                break;
            }
        }
        // RestoreLastError (ntdll RtlRestoreLastWin32Error) is SetLastError's
        // contract (k32_ntdll_forwarder_probe restore_last_error): the same
        // entry under a second name.
        for (auto it = k32.entries.rbegin(); it != k32.entries.rend(); ++it) {
            if (it->internal || it->name != "SetLastError") continue;
            winapi::ExportEntry copy = *it;
            copy.name = "RestoreLastError";
            copy.ordinal = 0;
            k32.entries.push_back(std::move(copy));
            break;
        }
        collect_kernelbase_surface(kernelbase, k32, kernelbase_seen);
        for (const auto& forwarder : kKernel32NtdllForwarders) {
            if (!(guest_is_64bit ? forwarder.x64 : forwarder.x86)) continue;
            // A name registered twice resolves to its last entry.
            for (auto it = k32.entries.rbegin(); it != k32.entries.rend(); ++it) {
                if (it->internal || it->name != forwarder.kernel32) continue;
                winapi::ExportEntry copy = *it;
                copy.name = forwarder.ntdll;
                copy.ordinal = 0;
                forwarded_to_ntdll.push_back(std::move(copy));
                break;
            }
        }
        (void)reg.register_dll("kernel32.dll", std::move(k32));
    }

    // -- Normaliz.dll --
    // Windows' export table is exactly these five, ordinals 1-5 on x86 and
    // x64, each forwarding to kernelbase. Importers bind it by name
    // (libcurl's IdnToAscii) as well as LoadLibrary it.
    {
        auto normaliz = winapi::nls::create_normalization_thunks();
        for (auto& entry : normaliz.entries) {
            entry.ordinal = winapi::nls::normaliz_ordinal(entry.name);
        }
        (void)reg.register_dll("normaliz.dll", std::move(normaliz));
    }

    // -- ntdll.dll --
    {
        winapi::DllExports ntdll;
        merge_exports(ntdll, winapi::make_ntdll_exports(ctx.ntdll_mgr));
        merge_exports(ntdll, winapi::make_ntdll_ext_exports(ctx.ntdll_ext_mgr));
        // The Rtl heap is kernel32's heap (HeapAlloc forwards to
        // RtlAllocateHeap); one HeapManager serves both DLLs.
        merge_exports(ntdll, winapi::create_rtl_heap_thunks(ctx.heap_ext, ctx.heap_mgr));
        merge_exports(ntdll, winapi::ntdll::make_nt_event_exports(ctx.sync_mgr, ctx.handle_table));
        // NtQueryVirtualMemory answers from the same state VirtualQuery and
        // GetMappedFileName read: the wired VirtualQueryManager, the mapped
        // view or module backing an address, and the drive -> NT device map.
        winapi::ntdll::set_query_virtual_memory_host({
            [&ctx](u64 handle) {
                using Target = winapi::ntdll::NtQueryVirtualMemoryHost::Target;
                if (handle == ~u64{0} || handle == 0xFFFFFFFFull) return Target::Current;
                if (handle > static_cast<u64>(std::numeric_limits<winapi::Handle>::max())) {
                    return Target::Invalid;
                }
                const auto* process = ctx.handle_table.get_as<ipc::ProcessObject>(
                    static_cast<winapi::Handle>(handle), winapi::HandleType::Process);
                if (process == nullptr) return Target::Invalid;
                return process->pid == static_cast<pid_t>(::getpid()) ? Target::Current
                                                                     : Target::Other;
            },
            [&ctx](u64 address, winapi::MemoryBasicInformation& info, u64 maximum) {
                return ctx.vqm.virtual_query(address, info, maximum);
            },
            [&ctx](u64 address) {
                auto path = ctx.fmm_mgr.mapped_file_path(static_cast<GuestAddr>(address));
                if (path.empty()) {
                    const auto module = ctx.module_mgr.module_for_address(
                        static_cast<GuestAddr>(address));
                    if (module.info) path = module.info->path;
                }
                return on_disk_case(ctx.vfs, path);
            },
            [&ctx](const std::string& path) {
                return winapi::win32_to_nt_device_path(ctx.vfs, path).value_or(std::string{});
            },
        });

        // x64 ntdll exports the SEH scope-table handler (dumpbin /exports
        // System32\ntdll.dll: ordinal 2346); the x86 one does not
        // (SysWOW64\ntdll.dll). EasyAntiCheat looks it up by name.
        if (guest_is_64bit) {
            ntdll.entries.push_back(winapi::make_c_specific_handler_export());
        }
        merge_exports(ntdll, winapi::make_nt_process_handle_exports(
                                 ctx.process_mgr, ctx.handle_table));
        ntdll.entries.push_back({"RtlRunOnceExecuteOnce", 0,
            [&ctx](winapi::Marshaler& m) { ctx.k32_ext_mgr.impl_RtlRunOnceExecuteOnce(m); }, 16});
        // NTSTATUS NtOpenKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES)
        // NTSTATUS NtOpenKeyEx(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG)
        // We don't implement a registry — fail with STATUS_OBJECT_NAME_NOT_FOUND
        // (0xC0000034) so callers fall back to defaults.
        ntdll.entries.push_back({"NtOpenKey", 0,
            [](winapi::Marshaler& m){
                GuestAddr h = m.arg_ptr(0); (void)m.arg(1); (void)m.arg_ptr(2);
                if (h) (void)m.write_u32(h, 0);
                m.set_return(0xC0000034u);
            }, 12});
        ntdll.entries.push_back({"NtOpenKeyEx", 0,
            [](winapi::Marshaler& m){
                GuestAddr h = m.arg_ptr(0); (void)m.arg(1); (void)m.arg_ptr(2); (void)m.arg(3);
                if (h) (void)m.write_u32(h, 0);
                m.set_return(0xC0000034u);
            }, 16});
        // NTSTATUS NtQueryValueKey(HANDLE, PUNICODE_STRING ValueName,
        //                          KEY_VALUE_INFORMATION_CLASS, PVOID Buffer,
        //                          ULONG Length, PULONG ResultLength)
        // Return STATUS_OBJECT_NAME_NOT_FOUND with ResultLength = 0.
        ntdll.entries.push_back({"NtQueryValueKey", 0,
            [](winapi::Marshaler& m){
                (void)m.arg(0); (void)m.arg_ptr(1); (void)m.arg(2);
                (void)m.arg_ptr(3); (void)m.arg(4);
                GuestAddr rl = m.arg_ptr(5);
                if (rl) (void)m.write_u32(rl, 0);
                m.set_return(0xC0000034u);
            }, 24});
        // Kernel32's public vectored-exception APIs forward to these native
        // Rtl entry points in ntdll.  Publish both names over the same
        // process-wide handler list so callers that resolve ntdll exports
        // directly observe the same registrations and handles.
        ntdll.entries.push_back({"RtlAddVectoredExceptionHandler", 0,
            [&mgr = ctx.k32_ext_mgr](winapi::Marshaler& m) {
                mgr.impl_AddVectoredExceptionHandler(m);
            }, 8});
        ntdll.entries.push_back({"RtlRemoveVectoredExceptionHandler", 0,
            [&mgr = ctx.k32_ext_mgr](winapi::Marshaler& m) {
                mgr.impl_RemoveVectoredExceptionHandler(m);
            }, 4});
        // Forwarder targets ntdll does not implement itself.
        {
            std::unordered_set<std::string> present;
            for (const auto& entry : ntdll.entries) present.insert(entry.name);
            for (auto& entry : forwarded_to_ntdll) {
                if (present.insert(entry.name).second) ntdll.entries.push_back(std::move(entry));
            }
        }
        winapi::add_ntdll_service_twins(ntdll);
        (void)reg.register_dll("ntdll.dll", std::move(ntdll));
    }
}

} // namespace vitreo::cli

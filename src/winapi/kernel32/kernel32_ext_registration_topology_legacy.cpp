// SPDX-License-Identifier: LGPL-2.1-only

/// @file kernel32_ext_registration_topology_legacy.cpp
/// @brief Extended kernel32 export table, part 4 of 9: NUMA topology, CRT probes, legacy file APIs.
///
/// A contiguous slice of the table that create_kernel32_ext_thunks builds
/// (see kernel32_ext_registration.cpp). The slice boundaries follow the
/// table's registration order, which is load-bearing: a name registered
/// twice resolves last-wins, and thunk addresses are assigned in
/// registration order. Do not reorder entries or call the parts out of
/// order.

#include "winapi/kernel32/system/processor_topology.hpp"
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

#include <string>
#include <thread>
#include <algorithm>
#include <span>
#include <vector>
#include <libproc.h>
#include <unistd.h>

namespace vitreo::winapi {

void append_topology_legacy_exports(Kernel32ExtManager& mgr, DllExports& exports) {
    // =====================================================================
    // NUMA topology. Learn (GetNumaProcessorNode et al): a machine with a
    // single node reports node 0 for every processor and a mask covering all
    // of them. Vitreo presents one node, so these are exact rather than
    // approximate.
    // =====================================================================

    exports.entries.push_back(ExportEntry{
        "GetNumaProcessorNode", 0,
        [](Marshaler& m) {
            const GuestAddr node_out = m.arg_ptr(1);
            if (node_out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            (void)m.write_u8(node_out, 0);
            m.set_return(1);
        },
        8});
    exports.entries.push_back(ExportEntry{
        "GetNumaProcessorNodeEx", 0,
        [](Marshaler& m) {
            const GuestAddr node_out = m.arg_ptr(1);
            if (node_out == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            (void)m.write_u16(node_out, 0);
            m.set_return(1);
        },
        16});
    exports.entries.push_back(ExportEntry{
        "GetNumaNodeProcessorMask", 0,
        [](Marshaler& m) {
            const u32 node = static_cast<u32>(m.arg(0)) & 0xFFu;
            const GuestAddr mask_out = m.arg_ptr(1);
            if (mask_out == 0 || node != 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            // PULONGLONG on both ABIs.
            (void)m.write_u64(mask_out, processor_topology::processor_mask());
            m.set_return(1);
        },
        12});
    exports.entries.push_back(ExportEntry{
        "GetNumaNodeProcessorMaskEx", 0,
        [](Marshaler& m) {
            const u32 node = static_cast<u32>(m.arg(0)) & 0xFFFFu;
            const GuestAddr affinity_out = m.arg_ptr(1);
            if (affinity_out == 0 || node != 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            // GROUP_AFFINITY in the caller's ABI (12 bytes on x86, 16 on x64).
            (void)processor_topology::write_group_affinity(
                m, affinity_out, processor_topology::guest_affinity(m, processor_topology::processor_mask()), 0);
            m.set_return(1);
        },
        12});
    // BOOL GetSystemCpuSetInformation(PSYSTEM_CPU_SET_INFORMATION Information,
    //   ULONG BufferLength, PULONG ReturnedLength, HANDLE Process, ULONG Flags)
    // Measured (processor_topology_probe "cpuset"): a NULL or short buffer fails
    // with ERROR_INSUFFICIENT_BUFFER and the required length; success writes
    // the length and leaves the last error alone. No CPU set is allocated to
    // any process, so Process changes no flag.
    exports.entries.push_back(ExportEntry{
        "GetSystemCpuSetInformation", 0,
        [](Marshaler& m) {
            const GuestAddr information = m.arg_ptr(0);
            const u32 capacity = static_cast<u32>(m.arg(1));
            const GuestAddr length_at = m.arg_ptr(2);
            if (length_at == 0) {
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
                m.set_return(0);
                return;
            }
            const std::vector<u8> sets = processor_topology::cpu_set_information();
            const u32 required = static_cast<u32>(sets.size());
            (void)m.write_u32(length_at, required);
            if (information == 0 || capacity < required) {
                set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
                m.set_return(0);
                return;
            }
            if (m.memory().write_bytes(information, reinterpret_cast<const Byte*>(sets.data()), sets.size()).is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                m.set_return(0);
                return;
            }
            m.set_return(1);
        },
        20});
    exports.entries.push_back(ExportEntry{
        "GetThreadIdealProcessorEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetThreadIdealProcessorEx(m); },
        8});
    exports.entries.push_back(ExportEntry{
        "SetThreadIdealProcessorEx", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetThreadIdealProcessorEx(m); },
        12});
    // Learn (GetLargePageMinimum): returns 0 when the processor does not
    // support large pages, which is the honest answer for a guest whose
    // allocator Vitreo backs with ordinary host pages.
    exports.entries.push_back(ExportEntry{
        "GetLargePageMinimum", 0,
        [](Marshaler& m) { m.set_return(0); },
        0});
    // Learn (GetDllDirectoryW): returns the characters copied, or the required
    // buffer size including the null when the buffer is too small. Vitreo has
    // no per-process DLL directory, so the value is the empty string.
    exports.entries.push_back(ExportEntry{
        "GetDllDirectoryW", 0,
        [](Marshaler& m) {
            const u32 chars = static_cast<u32>(m.arg(0));
            const GuestAddr buffer = m.arg_ptr(1);
            if (buffer != 0 && chars >= 1) {
                (void)m.write_u16(buffer, 0);
                // An empty directory returns 0, which Windows tells apart
                // from failure by setting ERROR_SUCCESS (measured on
                // Windows 11: last_error_on_success_probe.expected).
                set_last_error(error_codes::ERROR_SUCCESS);
                m.set_return(0);
                return;
            }
            m.set_return(1);  // room needed for the terminator alone
        },
        8});

    // =====================================================================
    // CRT startup probes (resolved via GetProcAddress at runtime)
    // =====================================================================

    // AreFileApisANSI (0 args)
    exports.entries.push_back(ExportEntry{
        "AreFileApisANSI", 0,
        [&mgr](Marshaler& m) { mgr.impl_AreFileApisANSI(m); },
        0
    });

    // SetFileApisToANSI (0 args)
    exports.entries.push_back(ExportEntry{
        "SetFileApisToANSI", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetFileApisToANSI(m); },
        0
    });

    // SetFileApisToOEM (0 args)
    exports.entries.push_back(ExportEntry{
        "SetFileApisToOEM", 0,
        [&mgr](Marshaler& m) { mgr.impl_SetFileApisToOEM(m); },
        0
    });

    // GetCurrentPackageId (2 args)
    exports.entries.push_back(ExportEntry{
        "GetCurrentPackageId", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentPackageId(m); },
        8
    });

    // GetCurrentPackageFullName (2 args)
    exports.entries.push_back(ExportEntry{
        "GetCurrentPackageFullName", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetCurrentPackageFullName(m); },
        8
    });

    // GetPackageFamilyName (3 args) — Learn appmodel.h
    exports.entries.push_back(ExportEntry{
        "GetPackageFamilyName", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPackageFamilyName(m); },
        12
    });

    // GetPackagePathByFullName (3 args) — Learn appmodel.h
    exports.entries.push_back(ExportEntry{
        "GetPackagePathByFullName", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPackagePathByFullName(m); },
        12
    });

    // CompareObjectHandles (2 args) — also exposed via api-ms-win-core-handle
    // which Vitreo's api-set resolver maps to kernel32.dll.
    exports.entries.push_back(ExportEntry{
        "CompareObjectHandles", 0,
        [&mgr](Marshaler& m) { mgr.impl_CompareObjectHandles(m); },
        8
    });

    // GetPackagesByPackageFamily (5 args)
    exports.entries.push_back(ExportEntry{
        "GetPackagesByPackageFamily", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPackagesByPackageFamily(m); },
        20
    });

    // PackageFamilyNameFromId (3 args)
    exports.entries.push_back(ExportEntry{
        "PackageFamilyNameFromId", 0,
        [&mgr](Marshaler& m) { mgr.impl_PackageFamilyNameFromId(m); },
        12
    });

    // PackageNameAndPublisherIdFromFamilyName (5 args)
    exports.entries.push_back(ExportEntry{
        "PackageNameAndPublisherIdFromFamilyName", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_PackageNameAndPublisherIdFromFamilyName(m);
        },
        20
    });

    // GetSystemTimePreciseAsFileTime (1 arg)
    exports.entries.push_back(ExportEntry{
        "GetSystemTimePreciseAsFileTime", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetSystemTimePreciseAsFileTime(m); },
        4
    });

    // SleepConditionVariableCS (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "SleepConditionVariableCS", 0,
        [&mgr](Marshaler& m) { mgr.impl_SleepConditionVariableCS(m); },
        12
    });

    // GetTempPath2W (2 args)
    exports.entries.push_back(ExportEntry{
        "GetTempPath2W", 0,
        [&mgr](Marshaler& m) {
            // Real Windows accepts (0, NULL) and returns the required
            // buffer size in characters (including terminating NUL).
            // Forward to impl which mirrors GetTempPathW semantics.
            mgr.impl_GetTempPath2W(m);
        },
        8
    });

    // AppPolicyGetProcessTerminationMethod (2 args)
    exports.entries.push_back(ExportEntry{
        "AppPolicyGetProcessTerminationMethod", 0,
        [&mgr](Marshaler& m) { mgr.impl_AppPolicyGetProcessTerminationMethod(m); },
        8
    });

    // AppPolicyGetThreadInitializationType (2 args)
    exports.entries.push_back(ExportEntry{
        "AppPolicyGetThreadInitializationType", 0,
        [&mgr](Marshaler& m) {
            mgr.impl_AppPolicyGetThreadInitializationType(m);
        },
        8
    });

    // ---- Legacy file APIs, string ops, INI profile ----

    // GetPrivateProfileIntA (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "GetPrivateProfileIntA", 0,
        [&mgr](Marshaler& m) { mgr.impl_GetPrivateProfileIntA(m); },
        16
    });

    // WritePrivateProfileStringA (4 args, stdcall_bytes=16)
    exports.entries.push_back(ExportEntry{
        "WritePrivateProfileStringA", 0,
        [&mgr](Marshaler& m) { mgr.impl_WritePrivateProfileStringA(m); },
        16
    });

    // FreeResource (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "FreeResource", 0,
        [](Marshaler& m) { (void)m.arg(0); m.set_return(0u); },
        4
    });

    // _lopen (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "_lopen", 0,
        [&mgr](Marshaler& m) {
            GuestAddr name_ptr = m.arg_ptr(0);
            u32 mode = static_cast<u32>(m.arg(1));
            if (name_ptr == 0) { m.set_return(0xFFFFFFFFu); return; }
            auto r = m.read_ansi_string(name_ptr);
            if (r.is_err()) { m.set_return(0xFFFFFFFFu); return; }
            std::string path = r.value();
            VITREO_TRACE("[k32ext] _lopen('%s', 0x%x)\n", path.c_str(), mode);
            auto am = (mode & 0x02) ? capsule::AccessMode::ReadWrite : capsule::AccessMode::Read;
            auto h = mgr.vfs().open(path, am,
                                    capsule::ShareMode::ReadWrite,
                                    capsule::CreationDisposition::OpenExisting);
            if (h.is_err()) { m.set_return(0xFFFFFFFFu); return; }
            m.set_return(static_cast<u32>(h.value()));
        },
        8
    });

    // _lclose (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "_lclose", 0,
        [&mgr](Marshaler& m) {
            u32 hFile = static_cast<u32>(m.arg(0));
            if (hFile == 0 || hFile == 0xFFFFFFFFu) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0xFFFFFFFFu /* HFILE_ERROR */);
                return;
            }
            (void)mgr.vfs().close(hFile);
            m.set_return(0u); // success
        },
        4
    });

    // _lread (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "_lread", 0,
        [&mgr](Marshaler& m) {
            u32 hFile = static_cast<u32>(m.arg(0));
            GuestAddr buf = m.arg_ptr(1);
            u32 bytes = static_cast<u32>(m.arg(2));
            if (hFile == 0 || hFile == 0xFFFFFFFFu) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0xFFFFFFFFu /* HFILE_ERROR */);
                return;
            }
            VITREO_TRACE("[k32ext] _lread(h=%u, buf=0x%llx, bytes=%u)\n",
                         hFile, (unsigned long long)buf, bytes);
            if (buf == 0 || bytes == 0) { m.set_return(0u); return; }
            std::vector<u8> tmp(bytes);
            auto r = mgr.vfs().read(hFile, std::span<u8>(tmp));
            if (r.is_err()) {
                VITREO_TRACE("[k32ext] _lread → VFS error\n");
                m.set_return(0xFFFFFFFFu); return;
            }
            u32 read_count = static_cast<u32>(r.value());
            (void)mgr.memory().write_bytes(buf, tmp.data(), read_count);
            VITREO_TRACE("[k32ext] _lread → %u bytes\n", read_count);
            m.set_return(read_count);
        },
        12
    });

    // _hread (3 args, stdcall_bytes=12) — same as _lread but returns long
    exports.entries.push_back(ExportEntry{
        "_hread", 0,
        [&mgr](Marshaler& m) {
            u32 hFile = static_cast<u32>(m.arg(0));
            GuestAddr buf = m.arg_ptr(1);
            u32 bytes = static_cast<u32>(m.arg(2));
            if (hFile == 0 || hFile == 0xFFFFFFFFu) {
                set_last_error(error_codes::ERROR_INVALID_HANDLE);
                m.set_return(0xFFFFFFFFu /* HFILE_ERROR */);
                return;
            }
            VITREO_TRACE("[k32ext] _hread(h=%u, buf=0x%llx, bytes=%u)\n",
                         hFile, (unsigned long long)buf, bytes);
            if (buf == 0 || bytes == 0) { m.set_return(0u); return; }
            std::vector<u8> tmp(bytes);
            auto r = mgr.vfs().read(hFile, std::span<u8>(tmp));
            if (r.is_err()) {
                VITREO_TRACE("[k32ext] _hread → VFS error\n");
                m.set_return(static_cast<u64>(static_cast<i32>(-1))); return;
            }
            u32 read_count = static_cast<u32>(r.value());
            (void)mgr.memory().write_bytes(buf, tmp.data(), read_count);
            VITREO_TRACE("[k32ext] _hread → %u bytes\n", read_count);
            m.set_return(read_count);
        },
        12
    });

    // _llseek (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "_llseek", 0,
        [&mgr](Marshaler& m) {
            u32 hFile = static_cast<u32>(m.arg(0));
            i32 offset = static_cast<i32>(m.arg(1));
            u32 origin = static_cast<u32>(m.arg(2));
            VITREO_TRACE("[k32ext] _llseek(h=%u, offset=%d, origin=%u)\n",
                         hFile, offset, origin);
            using SO = capsule::SeekOrigin;
            SO so = (origin == 1) ? SO::Current : (origin == 2) ? SO::End : SO::Begin;
            auto r = mgr.vfs().seek(hFile, static_cast<i64>(offset), so);
            if (r.is_err()) {
                VITREO_TRACE("[k32ext] _llseek → error\n");
                m.set_return(0xFFFFFFFFu); return;
            }
            VITREO_TRACE("[k32ext] _llseek → pos %llu\n", (unsigned long long)r.value());
            m.set_return(static_cast<u32>(r.value()));
        },
        12
    });

    // WinExec (2 args, stdcall_bytes=8) — stub, return 33 (success > 31)
    exports.entries.push_back(ExportEntry{
        "WinExec", 0,
        [](Marshaler& m) {
            if (m.arg(0) == 0) { set_last_error(error_codes::ERROR_INVALID_PARAMETER); m.set_return(0); return; }
            (void)m.arg_ptr(0); (void)m.arg(1);
            VITREO_TRACE("[k32ext] WinExec() → stub 33\n");
            m.set_return(33u);
        },
        8
    });

    // lstrcmpA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcmpA", 0,
        [&mgr](Marshaler& m) {
            // Real Win32 sets last_error=0 on entry (verified via win_oracle_diff).
            set_last_error(error_codes::ERROR_SUCCESS);
            GuestAddr s1 = m.arg_ptr(0);
            GuestAddr s2 = m.arg_ptr(1);
            for (u32 i = 0; ; ++i) {
                auto c1 = mgr.memory().read_u8(s1 + i);
                auto c2 = mgr.memory().read_u8(s2 + i);
                u8 ch1 = c1.is_ok() ? c1.value() : 0;
                u8 ch2 = c2.is_ok() ? c2.value() : 0;
                if (ch1 < ch2) { m.set_return(static_cast<u64>(static_cast<u32>(-1))); return; }
                if (ch1 > ch2) { m.set_return(1u); return; }
                if (ch1 == 0) { m.set_return(0u); return; }
            }
        },
        8
    });

    // GlobalHandle / LocalHandle live with the Global/Local object model
    // (memory/global_local.cpp).

    // OpenFile (3 args, stdcall_bytes=12) — legacy file open
    exports.entries.push_back(ExportEntry{
        "OpenFile", 0,
        [&mgr](Marshaler& m) {
            GuestAddr name_ptr = m.arg_ptr(0);
            GuestAddr ofstruct_ptr = m.arg_ptr(1);
            u32 style = static_cast<u32>(m.arg(2));
            if (name_ptr == 0) {
                VITREO_TRACE("[k32ext] OpenFile(NULL, style=0x%x) → HFILE_ERROR\n", style);
                m.set_return(0xFFFFFFFFu); return;
            }
            auto r = m.read_ansi_string(name_ptr);
            if (r.is_err()) {
                VITREO_TRACE("[k32ext] OpenFile(bad ptr, style=0x%x) → HFILE_ERROR\n", style);
                m.set_return(0xFFFFFFFFu); return;
            }
            VITREO_TRACE("[k32ext] OpenFile('%s', style=0x%x)\n", r.value().c_str(), style);

            // OF_EXIST (0x4000) — just check if file exists, don't open
            if (style & 0x4000) {
                auto stat = mgr.vfs().stat(r.value());
                if (stat.is_err()) { m.set_return(0xFFFFFFFFu); return; }
                if (ofstruct_ptr != 0) {
                    (void)mgr.memory().write_u16(ofstruct_ptr + 2, 0);
                }
                m.set_return(1u); // success (non-HFILE_ERROR)
                return;
            }

            // OF_READ=0, OF_WRITE=1, OF_READWRITE=2
            auto am = (style & 0x02) ? capsule::AccessMode::ReadWrite
                     : (style & 0x01) ? capsule::AccessMode::Write
                     : capsule::AccessMode::Read;
            auto h = mgr.vfs().open(r.value(), am,
                                    capsule::ShareMode::ReadWrite,
                                    capsule::CreationDisposition::OpenExisting);
            if (h.is_err()) { m.set_return(0xFFFFFFFFu); return; }
            // Fill OFSTRUCT.nErrCode = 0 (success)
            if (ofstruct_ptr != 0) {
                (void)mgr.memory().write_u16(ofstruct_ptr + 2, 0); // nErrCode
            }
            m.set_return(static_cast<u32>(h.value()));
        },
        12
    });

    // lstrcpynA (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "lstrcpynA", 0,
        [&mgr](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            i32 max_len = static_cast<i32>(m.arg(2));
            if (max_len <= 0 || dst == 0) { m.set_return(dst); return; }
            i32 i = 0;
            for (; i < max_len - 1; ++i) {
                auto ch = mgr.memory().read_u8(src + static_cast<u32>(i));
                u8 c = ch.is_ok() ? ch.value() : 0;
                (void)mgr.memory().write_u8(dst + static_cast<u32>(i), c);
                if (c == 0) { m.set_return(dst); return; }
            }
            (void)mgr.memory().write_u8(dst + static_cast<u32>(i), 0);
            m.set_return(dst);
        },
        12
    });

    // lstrcatA (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "lstrcatA", 0,
        [&mgr](Marshaler& m) {
            GuestAddr dst = m.arg_ptr(0);
            GuestAddr src = m.arg_ptr(1);
            if (dst == 0) { m.set_return(dst); return; }
            u32 dst_len = mgr.guest_strlen(dst);
            for (u32 i = 0; ; ++i) {
                auto ch = mgr.memory().read_u8(src + i);
                u8 c = ch.is_ok() ? ch.value() : 0;
                (void)mgr.memory().write_u8(dst + dst_len + i, c);
                if (c == 0) break;
            }
            m.set_return(dst);
        },
        8
    });
}

} // namespace vitreo::winapi

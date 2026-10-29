// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_rtl_unwind_version.cpp
/// @brief Extended ntdll export table, part 7 of 11: Rtl context capture,
///        function lookup and unwinding, and the version-info calls.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/thread/context64.hpp"
#include "winapi/kernel32/thread/unwind64.hpp"
#include "winapi/system/ntdll/version_verify.hpp"
#include "core/pe/os_version.hpp"

namespace vitreo::winapi {

using namespace ntdll;

namespace {
constexpr u32 kStatusRevisionMismatch = 0xC0000059u;  // ntstatus.h STATUS_REVISION_MISMATCH
}  // namespace

void append_rtl_unwind_version_exports(NtdllExtManager& mgr,
                                       DllExports& exports, u16& ordinal) {
    // RtlCaptureContext (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "RtlCaptureContext", 0,
        [](Marshaler& m) {
            // This used to discard its argument entirely, leaving the guest's
            // CONTEXT buffer holding whatever was already there. Callers do
            // not check for that -- there is nothing to check, the API returns
            // void and cannot fail on Windows -- so every consumer read stack
            // garbage or zeros as the captured register state.
            //
            // coreclr captures a context this way to start a GC stack walk.
            // A zeroed nonvolatile register slot makes GcInfoDecoder resolve a
            // GC_FRAMEREG_REL live slot against a base of 0, so the collector
            // is handed a root address that is just the frame offset (0x74,
            // say) and faults dereferencing it.
            //
            // kernel32's export already did the right thing; only the ntdll
            // one, which is the copy coreclr actually imports, was empty.
            write_context64(m, m.arg_ptr(0));
        },
        4
    });

    // RtlPcToFileHeader — Microsoft Learn winnt.h; also exported from
    // api-ms-win-core-rtlsupport-l1-1-0.dll (host ntdll). Must return the
    // image that contains PcValue, not the process EXE.
    exports.entries.push_back(ExportEntry{
        "RtlPcToFileHeader", 0,
        [&mgr](Marshaler& m) {
            GuestAddr base = 0;
            if (mgr.module_manager() != nullptr) {
                const auto found = mgr.module_manager()->module_for_address(
                    static_cast<GuestAddr>(m.arg(0)));
                if (found.info != nullptr) {
                    base = found.info->base_address;
                }
            }
            GuestAddr base_out = m.arg_ptr(1);
            if (base_out != 0) {
                (void)m.memory().write_u64(base_out, base);
            }
            m.set_return(base);
        },
        8
    });

    // RtlLookupFunctionEntry / RtlVirtualUnwind — Microsoft Learn winnt.h;
    // also exported from api-ms-win-core-rtlsupport-l1-1-0.dll (host ntdll).
    // No-op stubs made x64 C++ personalities ContinueSearch instead of
    // locating a catch via .pdata.
    exports.entries.push_back(ExportEntry{
        "RtlLookupFunctionEntry", 0,
        [&mgr](Marshaler& m) {
            GuestAddr control_pc = static_cast<GuestAddr>(m.arg(0));
            GuestAddr image_base_out = m.arg_ptr(1);
            GuestAddr image_base = 0;
            GuestAddr runtime_function = 0;
            if (mgr.module_manager() != nullptr) {
                runtime_function = mgr.module_manager()->lookup_runtime_function_entry(
                    control_pc, &image_base);
            }
            if (image_base_out != 0) {
                (void)m.memory().write_u64(image_base_out, image_base);
            }
            m.set_return(runtime_function);
        },
        12
    });

    exports.entries.push_back(ExportEntry{
        "RtlVirtualUnwind", 0,
        [](Marshaler& m) {
            GuestAddr handler_data = m.arg_ptr(5);
            GuestAddr establisher_frame = m.arg_ptr(6);
            GuestAddr exception_handler = 0;
            const bool ok = unwind_context64(
                m,
                m.arg_ptr(1),
                m.arg_ptr(2),
                m.arg_ptr(3),
                m.arg_ptr(4),
                handler_data,
                establisher_frame,
                &exception_handler,
                // HandlerType: only a handler of the requested kind is
                // returned (UNW_FLAG_NHANDLER asks for none).
                static_cast<u32>(m.arg(0)));
            if (!ok) {
                if (handler_data != 0) {
                    (void)m.write_u64(handler_data, 0);
                }
                if (establisher_frame != 0 && m.has_cpu()) {
                    (void)m.write_u64(establisher_frame, m.cpu()->gpr.rsp);
                }
                exception_handler = 0;
            }
            m.set_return(exception_handler);
        },
        32
    });

    // RtlUnwind / RtlUnwindEx — Microsoft Learn winnt.h; also exported from
    // api-ms-win-core-rtlsupport-l1-1-0.dll (host ntdll). On x64 both are the
    // one unwinder in kernel32/thread/unwind64.cpp (RtlUnwind is RtlUnwindEx
    // with no context buffer and no history table), shared with kernel32's
    // exports and the exception dispatcher's unwind pass.
    exports.entries.push_back(ExportEntry{
        "RtlUnwind", 0,
        [&mgr](Marshaler& m) {
            auto* cpu = m.cpu();
            if (!cpu) return;
            if (cpu->is_64bit()) {
                rtl_unwind_export64(m, mgr.module_manager(), mgr.process_manager(),
                                    /*is_ex=*/false);
                return;
            }
            // x86: the dispatcher's registration-chain unwind consumes this.
            cpu->pending_unwind.active = true;
            cpu->pending_unwind.target_ip = static_cast<u32>(m.arg(1));
            cpu->pending_unwind.target_frame = static_cast<u32>(m.arg(0));
            cpu->pending_unwind.return_value = static_cast<u32>(m.arg(3));
            cpu->gpr.rax = static_cast<u32>(m.arg(3));
        },
        16
    });
    exports.entries.push_back(ExportEntry{
        "RtlUnwindEx", 0,
        [&mgr](Marshaler& m) {
            rtl_unwind_export64(m, mgr.module_manager(), mgr.process_manager(),
                                /*is_ex=*/true);
        },
        0
    });

    // RtlVerifyVersionInfo: compares against the running system's real
    // version (no manifest lie; verify_version_info_probe "Rtl=" column).
    // Rules: version_verify.hpp. The last error is never touched.
    exports.entries.push_back(ExportEntry{
        "RtlVerifyVersionInfo", ordinal++,
        [&mgr](Marshaler& m) {
            // NTSTATUS RtlVerifyVersionInfo(PRTL_OSVERSIONINFOEXW VersionInfo,
            //   ULONG TypeMask, ULONGLONG ConditionMask)
            const GuestAddr info = m.arg_ptr(0);
            if (info == 0) {
                m.set_return(static_cast<u64>(nt_status::STATUS_INVALID_PARAMETER));
                return;
            }
            const u32 type_mask = static_cast<u32>(m.arg(1));
            const u64 cond_mask =
                m.cpu_is_64bit()
                    ? m.arg(2)
                    : static_cast<u64>(static_cast<u32>(m.arg(2))) |
                          (static_cast<u64>(static_cast<u32>(m.arg(3))) << 32);
            const auto ver = mgr.get_version();
            version_verify::VersionFields current;
            current.major = ver.major;
            current.minor = ver.minor;
            current.build = ver.build;
            current.platform_id = ver.platform_id;
            current.service_pack_major = pe::kOsServicePackMajor;
            current.service_pack_minor = pe::kOsServicePackMinor;
            current.suite_mask = pe::kOsSuiteMask;
            current.product_type = pe::kOsProductType;
            // RTL_OSVERSIONINFOEXW: wServicePackMajor +276, wServicePackMinor
            // +278, wSuiteMask +280, wProductType +282 (winnt.h).
            version_verify::VersionFields requested;
            requested.major = m.read_u32(info + 4).value_or(0);
            requested.minor = m.read_u32(info + 8).value_or(0);
            requested.build = m.read_u32(info + 12).value_or(0);
            requested.platform_id = m.read_u32(info + 16).value_or(0);
            requested.service_pack_major = m.read_u16(info + 276).value_or(0);
            requested.service_pack_minor = m.read_u16(info + 278).value_or(0);
            requested.suite_mask = m.read_u16(info + 280).value_or(0);
            requested.product_type = m.read_u8(info + 282).value_or(0);
            u32 status = nt_status::STATUS_SUCCESS;
            switch (version_verify::verify(current, requested, type_mask, cond_mask)) {
            case version_verify::Outcome::Satisfied: break;
            case version_verify::Outcome::Mismatch:
                status = kStatusRevisionMismatch;
                break;
            case version_verify::Outcome::InvalidParameter:
                status = nt_status::STATUS_INVALID_PARAMETER;
                break;
            }
            m.set_return(static_cast<u64>(status));
        },
        16
    });

    // VerSetConditionMask: encoding measured by verify_version_info_probe
    // (version_verify.hpp). x86 passes the ULONGLONG as two stack DWORDs and
    // returns it in EDX:EAX; x64 passes it in RCX.
    exports.entries.push_back(ExportEntry{
        "VerSetConditionMask", ordinal++,
        [](Marshaler& m) {
            const bool long_mode = m.cpu_is_64bit();
            const u64 mask =
                long_mode ? m.arg(0)
                          : static_cast<u64>(static_cast<u32>(m.arg(0))) |
                                (static_cast<u64>(static_cast<u32>(m.arg(1))) << 32);
            const u32 type_mask = static_cast<u32>(m.arg(long_mode ? 1 : 2));
            const u32 condition = static_cast<u32>(m.arg(long_mode ? 2 : 3)) & 0xFFu;
            m.set_return_i64(version_verify::set_condition_mask(
                mask, type_mask, condition, long_mode));
        },
        16
    });
}

} // namespace vitreo::winapi

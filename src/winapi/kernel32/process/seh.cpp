// SPDX-License-Identifier: LGPL-2.1-only

/// @file seh.cpp
/// @brief RtlUnwind and RaiseException thunks.
///
/// Structured-exception entry points exposed through kernel32.
/// Dispatch itself lives in the execution runtime.
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
#include "winapi/kernel32/thread/unwind64.hpp"
#include "winapi/kernel32/memory/virtual_query.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <cstdio>
#include "core/cpu/seh/seh32.hpp"
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

// =============================================================================
// SEH / Exception
// =============================================================================

void Kernel32ExtManager::impl_RtlUnwind(Marshaler& m) {
    // void RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
    //                PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
    auto* cpu = m.cpu();
    if (!cpu) return;

    if (cpu->is_64bit()) {
        // Learn: on x64 RtlUnwind is RtlUnwindEx with no context buffer and
        // no history table -- the same unwinder ntdll exports
        // (kernel32/thread/unwind64.cpp), with 64-bit arguments and the
        // caller's ExceptionRecord (STATUS_LONGJUMP from vcruntime's longjmp).
        rtl_unwind_export64(m, m_module_manager, m_process_manager,
                            /*is_ex=*/false);
        return;
    }

    const u32 target_frame = static_cast<u32>(m.arg(0));
    const u32 target_ip    = static_cast<u32>(m.arg(1));
    const u32 erec_ptr     = static_cast<u32>(m.arg(2));
    const u32 return_value = static_cast<u32>(m.arg(3));
    VITREO_TRACE("[kernel32_ext] RtlUnwind(frame=0x%x, ip=0x%x, erec=0x%x, ret=0x%x)\n",
                 target_frame, target_ip, erec_ptr, return_value);
    unwind_seh32_chain(m, target_frame, erec_ptr, return_value);
}

// x86 RtlUnwind, as measured on Windows (tools/oracle_probe/
// rtl_unwind32_probe.{c,expected}) -- Learn documents only the parameters:
//
//  - Every registration from FS:[0] down to, but not including, TargetFrame
//    has its handler called, in chain order, then is unlinked.
//  - The handler gets the caller's ExceptionRecord with EXCEPTION_UNWINDING
//    OR-ed into it IN PLACE, or, when none was given, a local STATUS_UNWIND
//    record whose ExceptionAddress is RtlUnwind's return address.
//  - Its CONTEXT (ContextFlags 0x10007) is RtlUnwind's return state: EIP =
//    return address, ESP = past the four stdcall arguments, EAX = ReturnValue,
//    EBP = the caller's, the other integer registers 0. Its DispatcherContext
//    is non-null and does not name the registration.
//  - During the call FS:[0] is a protector record whose Next is the
//    registration being unwound.
//  - RtlUnwind then RETURNS to its caller -- TargetIp is ignored -- with EAX =
//    ReturnValue, EBX/ESI/EDI/ECX/EDX = 0 and EBP preserved.
//
// This replaces a version that only rewrote FS:[0], so no __finally block and
// no C++ destructor in an intermediate frame ever ran during an unwind.
void Kernel32ExtManager::unwind_seh32_chain(Marshaler& m, u32 target_frame,
                                            u32 erec_ptr, u32 return_value) {
    auto* cpu = m.cpu();
    constexpr u32 kChainEnd = 0xFFFFFFFFu;
    constexpr u32 kExceptionUnwinding = 0x2;
    constexpr u32 kExceptionExitUnwind = 0x4;
    constexpr u32 kStatusUnwind = 0xC0000027u;
    constexpr u32 kStatusInvalidDisposition = 0xC0000026u;
    constexpr u32 kStatusBadStack = 0xC0000028u;
    constexpr u32 kStatusInvalidUnwindTarget = 0xC0000029u;

    const u32 entry_esp = static_cast<u32>(cpu->sp());
    const u32 return_address = m_memory.read_u32(entry_esp).value_or(0);
    const u32 caller_esp = entry_esp + 4 + 16;
    const u32 caller_ebp = static_cast<u32>(cpu->gpr.rbp);
    const GuestAddr teb = cpu->seg.fs_desc.base;
    const u32 stack_base = m_memory.read_u32(teb + 0x04).value_or(0);
    const u32 stack_limit = m_memory.read_u32(teb + 0x08).value_or(0);

    auto raise = [&](u32 code) {
        cpu->pending_raise.active = true;
        cpu->pending_raise.from_helper = false;
        cpu->pending_raise.code = code;
        cpu->pending_raise.flags = 1;  // EXCEPTION_NONCONTINUABLE
        cpu->pending_raise.num_args = 0;
    };

    // Scratch below the caller's frame: CONTEXT, local EXCEPTION_RECORD,
    // DISPATCHER_CONTEXT, protector registration. Handlers run below it.
    const u32 scratch = (entry_esp - 0x400u) & ~0xFu;
    const u32 ctx_addr = scratch;
    const u32 local_erec = ctx_addr + 0x2D0;
    const u32 dispatcher_ctx = local_erec + cpu::EREC32_SIZE;
    const u32 protector_addr = dispatcher_ctx + 0x10;

    cpu::CpuState return_state = *cpu;
    return_state.rip = return_address;
    return_state.gpr.rax = return_value;
    return_state.gpr.rbx = 0;
    return_state.gpr.rcx = 0;
    return_state.gpr.rdx = 0;
    return_state.gpr.rsi = 0;
    return_state.gpr.rdi = 0;
    return_state.gpr.rbp = caller_ebp;
    return_state.set_sp(caller_esp);
    if (cpu::write_context_32(m_memory, ctx_addr, return_state).is_err()) {
        raise(cpu::STATUS_ACCESS_VIOLATION);
        return;
    }

    u32 erec = erec_ptr;
    if (erec == 0) {
        erec = local_erec;
        (void)cpu::write_exception_record_32(m_memory, erec, kStatusUnwind, 0,
                                             return_address);
    }
    auto flags = m_memory.read_u32(erec + cpu::EREC32_FLAGS);
    if (flags.is_err()) {
        raise(cpu::STATUS_ACCESS_VIOLATION);
        return;
    }
    u32 unwind_flags = flags.value() | kExceptionUnwinding;
    if (target_frame == 0) unwind_flags |= kExceptionExitUnwind;
    (void)m_memory.write_u32(erec + cpu::EREC32_FLAGS, unwind_flags);
    (void)m_memory.write_u32(dispatcher_ctx, 0);
    (void)cpu::ensure_seh32_exception_protector(m_memory);

    u32 registration = m_memory.read_u32(teb + 0x00).value_or(kChainEnd);
    while (registration != kChainEnd && registration != target_frame) {
        if (target_frame != 0 && registration > target_frame) {
            raise(kStatusInvalidUnwindTarget);
            return;
        }
        if ((registration & 3u) != 0 || registration < stack_limit ||
            registration + 8 > stack_base) {
            raise(kStatusBadStack);
            return;
        }
        const u32 next = m_memory.read_u32(registration).value_or(kChainEnd);
        const u32 handler = m_memory.read_u32(registration + 4).value_or(0);

        cpu::Seh32NestedExecution protector{};
        (void)cpu::install_seh32_nested_execution(
            m_memory, *cpu, protector_addr, registration, protector);
        const u64 args[4] = {erec, registration, ctx_addr, dispatcher_ctx};
        const u64 saved_sp = cpu->sp();
        cpu->set_sp(scratch);
        const u64 disposition =
            m_guest_call ? m_guest_call(handler, args, 4, false) : ~0ull;
        cpu->set_sp(saved_sp);
        if (protector.installed) {
            (void)cpu::unlink_seh32_nested_execution(m_memory, *cpu, protector);
        }

        if ((disposition & 0xFFFFFFFFu) == cpu::ExceptionCollidedUnwind) {
            std::fprintf(stderr,
                "[vitreo] UNIMPLEMENTED kernel32!RtlUnwind: collided unwind "
                "at registration 0x%08x\n", registration);
            raise(kStatusInvalidDisposition);
            return;
        }
        if ((disposition & 0xFFFFFFFFu) != cpu::ExceptionContinueSearch) {
            raise(kStatusInvalidDisposition);
            return;
        }
        (void)m_memory.write_u32(teb + 0x00, next);
        registration = next;
    }

    if (target_frame != 0 && registration != target_frame) {
        std::fprintf(stderr,
            "[kernel32] RtlUnwind: target frame 0x%08x is not on the "
            "registration chain\n", target_frame);
        raise(kStatusInvalidUnwindTarget);
        return;
    }

    // Return to the caller exactly as measured. The thunk epilogue would put
    // back the entry nonvolatiles, so transfer control ourselves.
    cpu->rip = return_address;
    cpu->set_sp(caller_esp);
    cpu->gpr.rax = return_value;
    cpu->gpr.rbx = 0;
    cpu->gpr.rcx = 0;
    cpu->gpr.rdx = 0;
    cpu->gpr.rsi = 0;
    cpu->gpr.rdi = 0;
    cpu->gpr.rbp = caller_ebp;
    cpu->thunk_control_transfer = true;
}

void Kernel32ExtManager::impl_RaiseException(Marshaler& m) {
    // void RaiseException(DWORD dwExceptionCode, DWORD dwExceptionFlags,
    //                     DWORD nNumberOfArguments, const ULONG_PTR* lpArguments)
    u32 code      = static_cast<u32>(m.arg(0));
    u32 flags     = static_cast<u32>(m.arg(1));
    u32 nargs     = static_cast<u32>(m.arg(2));
    GuestAddr args_ptr = static_cast<GuestAddr>(m.arg(3));

    auto* cpu = m.cpu();
    if (!cpu) return;

    u64 caller_ret = 0;
    u64 throw_site_ret = 0;
    u64 throw_caller_ret = 0;
    if (cpu->is_64bit()) {
        auto ret_r = m_memory.read_u64(cpu->sp());
        if (ret_r.is_ok()) caller_ret = ret_r.value();
    } else {
        auto ret_r = m_memory.read_u32(cpu->sp());
        if (ret_r.is_ok()) caller_ret = static_cast<u64>(ret_r.value());
        auto frame1_r = m_memory.read_u32(static_cast<GuestAddr>(cpu->gpr.rbp));
        auto throw_site_r = m_memory.read_u32(static_cast<GuestAddr>(cpu->gpr.rbp + 4));
        if (throw_site_r.is_ok()) {
            throw_site_ret = static_cast<u64>(throw_site_r.value());
        }
        if (frame1_r.is_ok() && frame1_r.value() != 0) {
            auto throw_caller_r = m_memory.read_u32(static_cast<GuestAddr>(frame1_r.value() + 4));
            if (throw_caller_r.is_ok()) throw_caller_ret = static_cast<u64>(throw_caller_r.value());
        }
    }

    VITREO_TRACE("[kernel32_ext] RaiseException(code=0x%08x, flags=0x%x, nargs=%u, args=0x%llx, caller_ret=0x%llx, throw_site_ret=0x%llx, throw_caller_ret=0x%llx)\n",
                 code, flags, nargs, static_cast<unsigned long long>(args_ptr),
                 static_cast<unsigned long long>(caller_ret),
                 static_cast<unsigned long long>(throw_site_ret),
                 static_cast<unsigned long long>(throw_caller_ret));

    // VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND / MOD_NOT_FOUND /
    // INVALID_PARAMETER). The CRT delay helper passes one argument: a pointer
    // to a DelayLoadInfo (delayimp.h). On x86 that info is 36 bytes and the
    // argument is a pointer to the pointer, which is what the setup image's
    // helper pushes. Name the DLL and procedure so a missing export is the
    // contract, not an anonymous 0xC06D007F.
    if (!cpu->is_64bit() && args_ptr != 0 && nargs >= 1 &&
        (code == 0xC06D007Fu || code == 0xC06D007Eu || code == 0xC06D0057u)) {
        auto read_cstr = [&](GuestAddr p, char* out, usize cap) {
            out[0] = '\0';
            if (p == 0 || cap == 0) return;
            for (usize i = 0; i + 1 < cap; ++i) {
                const auto b = m_memory.read_u8(p + i);
                if (b.is_err() || b.value() == 0 || b.value() < 32 || b.value() > 126) {
                    out[i] = '\0';
                    return;
                }
                out[i] = static_cast<char>(b.value());
                out[i + 1] = '\0';
            }
        };
        char dll[96] = "?";
        char proc[96] = "?";
        const auto slot = m_memory.read_u32(args_ptr);
        if (slot.is_ok() && slot.value() != 0) {
            const GuestAddr info = slot.value();
            const auto dll_p = m_memory.read_u32(info + 0x0C);
            const auto by_name = m_memory.read_u32(info + 0x10);
            const auto name_p = m_memory.read_u32(info + 0x14);
            if (dll_p.is_ok()) read_cstr(dll_p.value(), dll, sizeof(dll));
            if (by_name.is_ok() && by_name.value() != 0 && name_p.is_ok()) {
                read_cstr(name_p.value(), proc, sizeof(proc));
            } else if (name_p.is_ok()) {
                std::snprintf(proc, sizeof(proc), "ordinal %u",
                              name_p.value() & 0xFFFFu);
            }
        }
        std::fprintf(stderr,
                     "[vitreo] delay-load failure code=0x%08x dll=%s proc=%s\n",
                     code, dll, proc);
    }

    // Learn: Set a thread name in native code — MS_VC_EXCEPTION 0x406D1388
    // with THREADNAME_INFO. Apply the name when present, then fall through and
    // dispatch normally: without a debugger Windows still raises the exception
    // so VEH / __except run (thread_lifecycle_probe RAISE ms_vc_exception
    // veh_saw=1). The CRT's own __except typically swallows it.
    constexpr u32 kMsVcException = 0x406D1388u;
    if (code == kMsVcException && args_ptr != 0 && nargs >= 2) {
        const bool is_64bit = cpu->is_64bit();
        u32 type = 0;
        GuestAddr name_ptr = 0;
        u32 thread_id = 0xFFFFFFFFu;
        if (is_64bit) {
            type = static_cast<u32>(m_memory.read_u64(args_ptr).value_or(0));
            name_ptr = static_cast<GuestAddr>(
                m_memory.read_u64(args_ptr + 8).value_or(0));
            if (nargs >= 3) {
                thread_id = static_cast<u32>(
                    m_memory.read_u64(args_ptr + 16).value_or(0xFFFFFFFFu));
            }
        } else {
            type = m_memory.read_u32(args_ptr).value_or(0);
            name_ptr = m_memory.read_u32(args_ptr + 4).value_or(0);
            if (nargs >= 3) {
                thread_id = m_memory.read_u32(args_ptr + 8).value_or(0xFFFFFFFFu);
            }
        }
        if (type == 0x1000u && name_ptr != 0 && m_thread_manager != nullptr) {
            std::string ansi;
            for (u32 i = 0; i < 63; ++i) {
                auto ch = m_memory.read_u8(name_ptr + i);
                if (ch.is_err() || ch.value() == 0) {
                    break;
                }
                ansi.push_back(static_cast<char>(ch.value()));
            }
            std::u16string wide;
            wide.reserve(ansi.size());
            for (unsigned char c : ansi) {
                wide.push_back(static_cast<char16_t>(c));
            }
            // Stores the description and, when the target is the calling
            // thread, names the host thread (the same path SetThreadDescription
            // takes: one UTF-16 -> UTF-8 conversion, one length limit).
            (void)m_thread_manager->set_thread_description_for_id(
                thread_id, std::move(wide));
            VITREO_TRACE("[kernel32_ext] SetThreadName tid=%u \"%s\"\n",
                         thread_id, ansi.c_str());
            static const bool prio_diag =
                std::getenv("VITREO_THREAD_PRIORITY_DIAG") != nullptr;
            if (prio_diag) {
                std::fprintf(stderr, "[thread-prio] name tid=%u caller=%u \"%s\"\n",
                             thread_id,
                             m_thread_manager->get_current_thread_id(),
                             ansi.c_str());
            }
        }
        // Fall through to pending_raise — do not return early.
    }

    // Store the exception info so the execution loop can dispatch SEH
    cpu->pending_raise.active = true;
    cpu->pending_raise.code = code;
    // Only EXCEPTION_NONCONTINUABLE survives from dwExceptionFlags; x64 marks
    // the record EXCEPTION_SOFTWARE_ORIGINATE (0x80), x86 does not
    // (raise_flags_probe: flags 0 and 0x40 give 0x80 on x64, 0 on x86).
    cpu->pending_raise.flags = (flags & 0x1u) | (cpu->is_64bit() ? 0x80u : 0u);
    // Learn: nNumberOfArguments is ignored when lpArguments is NULL
    // (thread_lifecycle_probe RAISE null_args_count3 params=0).
    cpu->pending_raise.num_args =
        (args_ptr == 0) ? 0u : ((nargs > 15) ? 15u : nargs);

    if (args_ptr != 0 && cpu->pending_raise.num_args > 0) {
        const bool is_64bit = cpu->is_64bit();
        for (u32 i = 0; i < cpu->pending_raise.num_args; ++i) {
            if (is_64bit) {
                auto arg_r = m_memory.read_u64(args_ptr + i * 8);
                cpu->pending_raise.args[i] = arg_r.is_ok() ? arg_r.value() : 0;
            } else {
                auto arg_r = m_memory.read_u32(args_ptr + i * 4);
                cpu->pending_raise.args[i] = arg_r.is_ok() ? arg_r.value() : 0;
            }
        }
    }

    if (code == 0xE06D7363u) {
        std::fprintf(stderr,
            "[kernel32_ext] RaiseException C++ EH nargs=%u "
            "args[0]=0x%llx args[1]=0x%llx args[2]=0x%llx args[3]=0x%llx\n",
            cpu->pending_raise.num_args,
            static_cast<unsigned long long>(cpu->pending_raise.args[0]),
            static_cast<unsigned long long>(cpu->pending_raise.args[1]),
            static_cast<unsigned long long>(cpu->pending_raise.args[2]),
            static_cast<unsigned long long>(cpu->pending_raise.args[3]));
    }

    // The MSVC delay-load helper raises these exceptions with one argument:
    // a pointer to the public DelayLoadInfo structure from delayimp.h.
    // Decode it only when explicitly requested so a missing procedure can be
    // diagnosed without a high-volume instruction/API trace. Keep the layouts
    // explicit because DelayLoadInfo contains guest pointers and therefore has
    // different offsets in PE32 and PE32+.
    constexpr u32 VCPP_EXCEPTION_MOD_NOT_FOUND = 0xC06D007Eu;
    constexpr u32 VCPP_EXCEPTION_PROC_NOT_FOUND = 0xC06D007Fu;
    static const bool delayload_diag =
        std::getenv("VITREO_DELAYLOAD_DIAG") != nullptr;
    if ((code == VCPP_EXCEPTION_MOD_NOT_FOUND ||
         code == VCPP_EXCEPTION_PROC_NOT_FOUND) &&
        cpu->pending_raise.num_args >= 1 &&
        delayload_diag) {
        const bool is_64bit = cpu->is_64bit();
        const GuestAddr info =
            static_cast<GuestAddr>(cpu->pending_raise.args[0]);
        const GuestAddr pidd_offset = is_64bit ? 8 : 4;
        const GuestAddr ppfn_offset = is_64bit ? 16 : 8;
        const GuestAddr dll_offset = is_64bit ? 24 : 12;
        const GuestAddr import_by_name_offset = is_64bit ? 32 : 16;
        const GuestAddr proc_offset = is_64bit ? 40 : 20;
        const GuestAddr hmod_offset = is_64bit ? 48 : 24;
        const GuestAddr pfn_offset = is_64bit ? 56 : 28;
        const GuestAddr last_error_offset = is_64bit ? 64 : 32;

        auto read_guest_ptr = [&](GuestAddr address) -> u64 {
            if (is_64bit) {
                auto value = m_memory.read_u64(address);
                return value.is_ok() ? value.value() : 0;
            }
            auto value = m_memory.read_u32(address);
            return value.is_ok() ? static_cast<u64>(value.value()) : 0;
        };
        auto read_guest_ansi = [&](GuestAddr address) -> std::string {
            if (address == 0) return {};
            auto value = m.read_ansi_string(address, 260);
            return value.is_ok() ? value.value() : std::string{"<unreadable>"};
        };

        const auto cb_r = m_memory.read_u32(info);
        const auto import_by_name_r =
            m_memory.read_u32(info + import_by_name_offset);
        const auto last_error_r =
            m_memory.read_u32(info + last_error_offset);
        const u64 dll_ptr = read_guest_ptr(info + dll_offset);
        const u64 proc_value = read_guest_ptr(info + proc_offset);
        const bool import_by_name =
            import_by_name_r.is_ok() && import_by_name_r.value() != 0;
        const std::string dll =
            read_guest_ansi(static_cast<GuestAddr>(dll_ptr));
        const std::string proc =
            import_by_name
                ? read_guest_ansi(static_cast<GuestAddr>(proc_value))
                : std::string{};

        std::fprintf(
            stderr,
            "[delay_load] code=0x%08x mode=%s info=0x%llx cb=%u "
            "dll=\"%s\" proc=%s%s%s last_error=%u "
            "pidd=0x%llx ppfn=0x%llx hmod=0x%llx pfn=0x%llx "
            "caller=0x%llx throw_site=0x%llx\n",
            code, is_64bit ? "x64" : "x86",
            static_cast<unsigned long long>(info),
            cb_r.is_ok() ? cb_r.value() : 0,
            dll.empty() ? "<unavailable>" : dll.c_str(),
            import_by_name ? "\"" : "ordinal=",
            import_by_name ? proc.c_str()
                           : std::to_string(
                                 static_cast<u32>(proc_value)).c_str(),
            import_by_name ? "\"" : "",
            last_error_r.is_ok() ? last_error_r.value() : 0,
            static_cast<unsigned long long>(
                read_guest_ptr(info + pidd_offset)),
            static_cast<unsigned long long>(
                read_guest_ptr(info + ppfn_offset)),
            static_cast<unsigned long long>(
                read_guest_ptr(info + hmod_offset)),
            static_cast<unsigned long long>(
                read_guest_ptr(info + pfn_offset)),
            static_cast<unsigned long long>(caller_ret),
            static_cast<unsigned long long>(throw_site_ret));
    }

    // RaiseException code 0xE0000008 carries a requested size in args[0].
    // It means an allocation contract returned failure where Windows would
    // have succeeded. Always log it (low volume; the process is about to die)
    // plus the recent hostcall ring so the failing API is identifiable.
    if (code == 0xE0000008u) {
        std::fprintf(stderr,
            "[kernel32_ext] RaiseException OOM (kOomExceptionCode) size=0x%llx"
            " caller_ret=0x%llx nargs=%u\n",
            static_cast<unsigned long long>(
                cpu->pending_raise.num_args ? cpu->pending_raise.args[0] : 0),
            static_cast<unsigned long long>(caller_ret), nargs);
        // RSP-scan backtrace: stack qwords that point just past a CALL
        // instruction are likely return addresses. Frame pointers are
        // omitted, so an RBP chain is useless here.
        if (cpu->is_64bit()) {
            u64 rsp = cpu->sp();
            int found = 0;
            for (u64 off = 0; off < 0x1000 && found < 24; off += 8) {
                auto v = m_memory.read_u64(rsp + off);
                if (!v.is_ok()) break;
                u64 a = v.value();
                if (a < 0x10000 || a >= 0x0000800000000000ull) continue;
                auto b5 = m_memory.read_u8(a - 5);  // E8 rel32
                auto b6 = m_memory.read_u8(a - 6);  // FF 15 [rip+d32]
                auto b6b = m_memory.read_u8(a - 5); // second byte of FF 15
                auto b2 = m_memory.read_u8(a - 2);  // FF Dx call reg
                bool is_ret =
                    (b5.is_ok() && b5.value() == 0xE8) ||
                    (b6.is_ok() && b6.value() == 0xFF && b6b.is_ok() &&
                     (b6b.value() == 0x15 || (b6b.value() & 0xF8) == 0x90)) ||
                    (b2.is_ok() && b2.value() == 0xFF);
                if (is_ret) {
                    std::fprintf(stderr, "[oom-bt] rsp+0x%03llx ret=0x%llx\n",
                                 static_cast<unsigned long long>(off),
                                 static_cast<unsigned long long>(a));
                    ++found;
                }
            }
        }
        // The exception argument is useless for the cppgc path: blink's
        // ReportOOMErrorInMainThread calls TerminateBecauseOutOfMemory(0), so
        // the size that cppgc actually asked for only exists in the caller's
        // still-live frame (ObjectAllocator::OutOfLineAllocateImpl keeps it in
        // a callee-saved register, which the OOM reporting path spills). Dump
        // the registers and a raw stack window so it can be read out.
        if (cpu->is_64bit()) {
            std::fprintf(stderr,
                "[oom-ctx] rax=0x%llx rcx=0x%llx rdx=0x%llx rbx=0x%llx "
                "rsi=0x%llx rdi=0x%llx rbp=0x%llx rsp=0x%llx\n",
                (unsigned long long)cpu->gpr.rax,
                (unsigned long long)cpu->gpr.rcx,
                (unsigned long long)cpu->gpr.rdx,
                (unsigned long long)cpu->gpr.rbx,
                (unsigned long long)cpu->gpr.rsi,
                (unsigned long long)cpu->gpr.rdi,
                (unsigned long long)cpu->gpr.rbp,
                (unsigned long long)cpu->gpr.rsp);
            std::fprintf(stderr,
                "[oom-ctx] r8=0x%llx r9=0x%llx r10=0x%llx r11=0x%llx "
                "r12=0x%llx r13=0x%llx r14=0x%llx r15=0x%llx\n",
                (unsigned long long)cpu->gpr.r8,
                (unsigned long long)cpu->gpr.r9,
                (unsigned long long)cpu->gpr.r10,
                (unsigned long long)cpu->gpr.r11,
                (unsigned long long)cpu->gpr.r12,
                (unsigned long long)cpu->gpr.r13,
                (unsigned long long)cpu->gpr.r14,
                (unsigned long long)cpu->gpr.r15);
            const u64 rsp = cpu->sp();
            for (u64 off = 0; off < 0x800; off += 0x20) {
                u64 word[4];
                bool ok = true;
                for (int i = 0; i < 4; ++i) {
                    auto v = m_memory.read_u64(rsp + off + 8u * i);
                    if (!v.is_ok()) { ok = false; break; }
                    word[i] = v.value();
                }
                if (!ok) break;
                std::fprintf(stderr,
                    "[oom-stack] rsp+0x%03llx %016llx %016llx %016llx "
                    "%016llx\n",
                    (unsigned long long)off,
                    (unsigned long long)word[0], (unsigned long long)word[1],
                    (unsigned long long)word[2], (unsigned long long)word[3]);
            }
        }
        // The size is a *string's* characters, not a computed length (see the
        // AASM display-copy OOM), so the object that owns it is what matters.
        // Dump every plausible guest pointer in the frames that are still live
        // — the callee-saved registers plus the stack window holding the copy
        // loop's frame — far enough to cover a short-string buffer.
        if (cpu->is_64bit()) {
            const auto dump_object = [this](const char* what, u64 addr) {
                if (addr < 0x10000 || addr >= 0x0000800000000000ull) return;
                for (u64 off = 0; off < 0x40; off += 0x10) {
                    u64 lo, hi;
                    auto a = m_memory.read_u64(addr + off);
                    auto b = m_memory.read_u64(addr + off + 8);
                    if (!a.is_ok() || !b.is_ok()) return;
                    lo = a.value();
                    hi = b.value();
                    char text[17];
                    for (int i = 0; i < 16; ++i) {
                        const u64 w = i < 8 ? lo : hi;
                        const unsigned c =
                            static_cast<unsigned>((w >> (8 * (i % 8))) & 0xFF);
                        text[i] = (c >= 0x20 && c < 0x7F)
                                      ? static_cast<char>(c) : '.';
                    }
                    text[16] = '\0';
                    std::fprintf(stderr,
                        "[oom-obj] %s+0x%02llx %016llx %016llx |%s|\n",
                        what, (unsigned long long)off,
                        (unsigned long long)lo, (unsigned long long)hi, text);
                }
            };
            dump_object("rbx", cpu->gpr.rbx);
            dump_object("r12", cpu->gpr.r12);
            dump_object("r15", cpu->gpr.r15);
            const u64 rsp = cpu->sp();
            for (u64 off = 0x300; off < 0x340; off += 8) {
                auto v = m_memory.read_u64(rsp + off);
                if (!v.is_ok()) break;
                char label[24];
                std::snprintf(label, sizeof(label), "[rsp+0x%03llx]",
                              (unsigned long long)off);
                dump_object(label, v.value());
            }
        }
        vitreo::diagnostics::dump_hostcalls("guest-oom");
    }

    // Debug: dump MSVC C++ exception type info
    if (code == 0xe06d7363u && nargs >= 3) {
        u32 magic = static_cast<u32>(cpu->pending_raise.args[0]);
        u32 obj   = static_cast<u32>(cpu->pending_raise.args[1]);
        u32 ti    = static_cast<u32>(cpu->pending_raise.args[2]);
        VITREO_TRACE("[cxx_eh] magic=0x%08x obj=0x%08x ThrowInfo=0x%08x\n", magic, obj, ti);
        static const bool cxx_eh_diag =
            std::getenv("VITREO_CXX_EH_DIAG") != nullptr;
        static std::atomic_bool first_cxx_exception_logged{false};
        const bool log_first =
            cxx_eh_diag && !first_cxx_exception_logged.exchange(true);
        if (log_first) {
            std::fprintf(stderr,
                "[cxx_eh] first exception magic=0x%08x obj=0x%08x "
                "ThrowInfo=0x%08x caller=0x%llx\n",
                magic, obj, ti,
                static_cast<unsigned long long>(caller_ret));
        }
        // Try to read type name from ThrowInfo -> CatchableTypeArray -> CatchableType -> TypeDescriptor
        if (ti != 0) {
            auto cta_r = m_memory.read_u32(ti + 12); // ThrowInfo.pCatchableTypeArray
            if (cta_r.is_ok() && cta_r.value() != 0) {
                auto n_r = m_memory.read_u32(cta_r.value());
                auto ct0_r = m_memory.read_u32(cta_r.value() + 4);
                if (n_r.is_ok() && ct0_r.is_ok() && ct0_r.value() != 0) {
                    auto td_r = m_memory.read_u32(ct0_r.value() + 4); // CatchableType.pType
                    if (td_r.is_ok() && td_r.value() != 0) {
                        // TypeDescriptor: offset 8 = mangled name (null-terminated)
                        char name[128] = {};
                        for (int i = 0; i < 127; ++i) {
                            auto ch = m_memory.read_u8(td_r.value() + 8 + i);
                            if (ch.is_err() || ch.value() == 0) break;
                            name[i] = static_cast<char>(ch.value());
                        }
                        VITREO_TRACE("[cxx_eh] Exception type: %s\n", name);
                        if (log_first) {
                            std::fprintf(stderr,
                                "[cxx_eh] first exception type=%s\n", name);
                        }
                    }
                }
            }
        }
    }
}

} // namespace vitreo::winapi

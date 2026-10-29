// SPDX-License-Identifier: LGPL-2.1-only

/// @file ntdll_registration_thread_control.cpp
/// @brief Extended ntdll export table, part 9 of 11: NtSuspendThread,
///        NtResumeThread, NtSystemDebugControl and NtCreateThreadEx.
///
/// A contiguous slice of the table that make_ntdll_ext_exports builds
/// (see ntdll_registration.cpp). The slice boundaries follow the table's
/// registration order, which is load-bearing: ordinals come from the
/// running `ordinal++` counter and ModuleManager derives the service
/// veneers from entry order. Do not reorder entries or call the parts out
/// of order.

#include "winapi/system/ntdll/ntdll_registration_parts.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include <cstdio>
#include <cstdlib>

namespace vitreo::winapi {

using namespace ntdll;

void append_thread_control_exports(NtdllExtManager& mgr,
                                   DllExports& exports, u16& ordinal) {
    // NtSuspendThread / NtResumeThread are the native primitives behind
    // kernel32 SuspendThread / ResumeThread.  Both take the thread handle and
    // an optional PULONG that receives the *previous* suspend count, and
    // return an NTSTATUS rather than the Win32 DWORD count.  They share the
    // cooperative suspend machinery used by the Win32 entry points, so a
    // native caller genuinely stops the target thread.
    constexpr u32 kStatusSuspendCountExceeded = 0xC000004Au;
    constexpr u32 kStatusThreadIsTerminating = 0xC000004Bu;
    auto nt_suspend_thread = [&mgr](Marshaler& m) {
        const Handle thread = static_cast<Handle>(m.arg(0));
        const GuestAddr previous_ptr = m.arg_ptr(1);
        auto* threads = mgr.thread_manager();
        if (threads == nullptr) {
            m.set_return(nt_status::STATUS_INVALID_HANDLE);
            return;
        }
        auto outcome = ThreadManager::SuspendResult::Ok;
        const u32 previous = threads->suspend_thread(thread, &outcome);
        if (outcome != ThreadManager::SuspendResult::Ok) {
            VITREO_TRACE("[ntdll] NtSuspendThread(0x%x) -> %s\n",
                         static_cast<unsigned>(thread),
                         outcome == ThreadManager::SuspendResult::Terminating
                             ? "STATUS_THREAD_IS_TERMINATING"
                             : (outcome ==
                                ThreadManager::SuspendResult::CountExceeded
                                    ? "STATUS_SUSPEND_COUNT_EXCEEDED"
                                    : "STATUS_INVALID_HANDLE"));
            m.set_return(
                outcome == ThreadManager::SuspendResult::Terminating
                    ? kStatusThreadIsTerminating
                    : (outcome == ThreadManager::SuspendResult::CountExceeded
                           ? kStatusSuspendCountExceeded
                           : nt_status::STATUS_INVALID_HANDLE));
            return;
        }
        VITREO_TRACE("[ntdll] NtSuspendThread(0x%x) -> prev=%u\n",
                     static_cast<unsigned>(thread), previous);
        if (previous_ptr != 0 &&
            m.memory().write_u32(previous_ptr, previous).is_err()) {
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        m.set_return(nt_status::STATUS_SUCCESS);
    };
    exports.entries.push_back(ExportEntry{
        "NtSuspendThread", ordinal++, nt_suspend_thread, 8});

    auto nt_resume_thread = [&mgr](Marshaler& m) {
        const Handle thread = static_cast<Handle>(m.arg(0));
        const GuestAddr previous_ptr = m.arg_ptr(1);
        auto* threads = mgr.thread_manager();
        if (threads == nullptr) {
            m.set_return(nt_status::STATUS_INVALID_HANDLE);
            return;
        }
        const u32 previous = threads->resume_thread(thread);
        if (previous == ~0u) {
            m.set_return(nt_status::STATUS_INVALID_HANDLE);
            return;
        }
        VITREO_TRACE("[ntdll] NtResumeThread(0x%x) -> prev=%u\n",
                     static_cast<unsigned>(thread), previous);
        if (previous_ptr != 0 &&
            m.memory().write_u32(previous_ptr, previous).is_err()) {
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        m.set_return(nt_status::STATUS_SUCCESS);
    };
    exports.entries.push_back(ExportEntry{
        "NtResumeThread", ordinal++, nt_resume_thread, 8});

    // NtSystemDebugControl is the six-argument native system-debugging
    // service.  User-mode ntdll publishes both spellings at the same system
    // service; Vitreo does not expose a kernel debugger, so calls fail
    // explicitly instead of fabricating debug state.
    add_unsupported_service(exports, ordinal, "NtSystemDebugControl", 24);
    add_unsupported_service(exports, ordinal, "ZwSystemDebugControl", 24);

    // NtCreateThreadEx is the native primitive used by CreateThread and
    // RtlCreateUserThread. Vitreo currently owns only the current process, so
    // remote-process creation and PS_ATTRIBUTE_LIST outputs remain explicitly
    // unsupported instead of being silently discarded.
    auto nt_create_thread_ex = [&mgr](Marshaler& m) {
        constexpr u32 kNativeCreateSuspended = 0x00000001;
        constexpr u32 kNativeSkipThreadAttach = 0x00000002;
        constexpr u32 kNativeHideFromDebugger = 0x00000004;
        constexpr u32 kSupportedNativeFlags =
            kNativeCreateSuspended | kNativeSkipThreadAttach |
            kNativeHideFromDebugger;
        constexpr u32 kObjInherit = 0x00000002;
        constexpr u64 kCurrentProcess64 = ~u64{0};
        constexpr u64 kCurrentProcess32 = 0xFFFFFFFFu;
        const GuestAddr thread_handle_ptr = m.arg_ptr(0);
        const u32 desired_access = static_cast<u32>(m.arg(1));
        const GuestAddr object_attributes = m.arg_ptr(2);
        const u64 process = m.arg(3);
        const GuestAddr start = m.arg_ptr(4);
        const GuestAddr argument = m.arg_ptr(5);
        const u32 create_flags = static_cast<u32>(m.arg(6));
        const u64 zero_bits = m.arg(7);
        const u64 stack_commit = m.arg(8);
        const u64 stack_reserve = m.arg(9);
        const GuestAddr attribute_list = m.arg_ptr(10);
        auto* threads = mgr.thread_manager();
        auto* handles = mgr.handle_table();

        if (std::getenv("VITREO_THREAD_DIAG") != nullptr) {
            std::fprintf(
                stderr,
                "[thread-diag] NtCreateThreadEx out=0x%llx access=0x%x "
                "attrs=0x%llx process=0x%llx start=0x%llx arg=0x%llx "
                "flags=0x%x zero=0x%llx commit=0x%llx reserve=0x%llx "
                "psattrs=0x%llx\n",
                static_cast<unsigned long long>(thread_handle_ptr),
                desired_access,
                static_cast<unsigned long long>(object_attributes),
                static_cast<unsigned long long>(process),
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(argument), create_flags,
                static_cast<unsigned long long>(zero_bits),
                static_cast<unsigned long long>(stack_commit),
                static_cast<unsigned long long>(stack_reserve),
                static_cast<unsigned long long>(attribute_list));
        }

        if (thread_handle_ptr == 0) {
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        const auto cleared = m.cpu_is_64bit()
            ? m.write_u64(thread_handle_ptr, 0)
            : m.write_u32(thread_handle_ptr, 0);
        if (cleared.is_err()) {
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }
        if ((process != kCurrentProcess64 && process != kCurrentProcess32) ||
            threads == nullptr || handles == nullptr) {
            m.set_return(nt_status::STATUS_INVALID_HANDLE);
            return;
        }
        if (start == 0) {
            m.set_return(nt_status::STATUS_INVALID_PARAMETER);
            return;
        }
        if ((create_flags & ~kSupportedNativeFlags) != 0 ||
            zero_bits != 0 || attribute_list != 0) {
            m.set_return(nt_status::STATUS_NOT_SUPPORTED);
            return;
        }

        u32 handle_flags = 0;
        if (object_attributes != 0) {
            const auto length = m.read_u32(object_attributes);
            const GuestAddr attributes_offset = m.cpu_is_64bit() ? 24 : 12;
            const u32 expected_length = m.cpu_is_64bit() ? 48 : 24;
            const auto attributes = m.read_u32(
                object_attributes + attributes_offset);
            if (length.is_err() || attributes.is_err()) {
                m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
                return;
            }
            if (length.value() != expected_length) {
                m.set_return(nt_status::STATUS_INVALID_PARAMETER);
                return;
            }
            if ((attributes.value() & kObjInherit) != 0) {
                handle_flags |= HandleFlags::Inherit;
            }
        }

        const u32 win32_flags = (create_flags & kNativeCreateSuspended) != 0
            ? CREATE_SUSPENDED
            : 0;
        u32 tid = 0;
        // SizeOfStackCommit and MaximumStackSize are separate here; folding
        // them into CreateThread's single dwStackSize read the reserve as a
        // commit and rounded it to 1 MB.
        const Handle thread = threads->create_thread(
            0, start, argument, win32_flags, &tid, 0, 0,
            ThreadStackSizes{stack_reserve, stack_commit});
        if (thread == INVALID_HANDLE || thread == NULL_HANDLE) {
            m.set_return(nt_status::STATUS_NO_MEMORY);
            return;
        }
        (void)handles->set_flags(
            thread, HandleFlags::Inherit, handle_flags);
        if ((create_flags & kNativeHideFromDebugger) != 0) {
            (void)threads->set_thread_hidden_from_debugger(thread);
        }
        // Set before the thread can launch: it decides whether the thread's
        // start runs DLL/TLS attach notifications at all.
        if ((create_flags & kNativeSkipThreadAttach) != 0) {
            if (auto* guest_thread = threads->get_thread(tid)) {
                guest_thread->skip_thread_attach = true;
            }
        }
        const auto wrote = m.cpu_is_64bit()
            ? m.write_u64(thread_handle_ptr, thread)
            : m.write_u32(thread_handle_ptr, thread);
        if (wrote.is_err()) {
            (void)handles->close(thread);
            m.set_return(nt_status::STATUS_ACCESS_VIOLATION);
            return;
        }

        if ((create_flags & kNativeCreateSuspended) == 0) {
            if (auto* guest_thread = threads->get_thread(tid)) {
                guest_thread->launched = true;
            }
            threads->run_async(tid, start, argument);
        }
        VITREO_TRACE(
            "[ntdll] NtCreateThreadEx start=0x%llx argument=0x%llx "
            "flags=0x%x stack=0x%llx tid=%u -> handle=0x%x\n",
            static_cast<unsigned long long>(start),
            static_cast<unsigned long long>(argument), create_flags,
            static_cast<unsigned long long>(stack_reserve), tid, thread);
        (void)desired_access;
        m.set_return(nt_status::STATUS_SUCCESS);
    };
    exports.entries.push_back(ExportEntry{
        "NtCreateThreadEx", ordinal++, nt_create_thread_ex, 44});
    exports.entries.push_back(ExportEntry{
        "ZwCreateThreadEx", ordinal++, nt_create_thread_ex, 44});
}

} // namespace vitreo::winapi

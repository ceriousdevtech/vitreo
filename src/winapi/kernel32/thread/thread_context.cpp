// SPDX-License-Identifier: LGPL-2.1-only

/// @file thread_context.cpp
/// @brief See thread_context.hpp.

#include "winapi/kernel32/thread/thread_context.hpp"

#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/state/context32_record.hpp"
#include "core/cpu/state/context64_record.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"

#include <cstdio>

namespace vitreo::winapi {
namespace {

constexpr u32 STATUS_SUCCESS = 0x00000000;
constexpr u32 STATUS_DATATYPE_MISALIGNMENT = 0x80000002;
constexpr u32 STATUS_INVALID_HANDLE = 0xC0000008;
constexpr u32 STATUS_ACCESS_VIOLATION = 0xC0000005;
constexpr u32 STATUS_ACCESS_DENIED = 0xC0000022;
constexpr u32 STATUS_NOT_SUPPORTED = 0xC00000BB;

constexpr GuestAddr kContext64EFlags = 0x44;  // winnt.h CONTEXT (AMD64)
constexpr u16 kWindowsInitialFpuControlWord = 0x027F;
constexpr u32 kWindowsInitialMxcsr = 0x1F80;

constexpr u64 kCurrentThread64 = ~u64{1};
constexpr u64 kCurrentThread32 = 0xFFFFFFFEu;

struct Target {
    u32 tid = 0;
    GuestThread* thread = nullptr;
};

Target resolve(ThreadManager* threads, u64 handle, u32 required_access,
               u32& error_out) {
    Target t;
    error_out = 0;
    if (threads == nullptr) {
        error_out = error_codes::ERROR_INVALID_HANDLE;
        return t;
    }
    // Access and existence: same path SuspendThread uses (pseudo-handle,
    // THREAD_* rights). Measured GETCTX_NO_ACCESS / SETPRIO_NO_ACCESS.
    const u32 err = threads->thread_handle_error(
        static_cast<Handle>(handle), required_access);
    if (err != 0) {
        error_out = err;
        return t;
    }
    if (handle == kCurrentThread64 || handle == kCurrentThread32) {
        t.tid = ThreadManager::get_current_thread_id_static();
        t.thread = threads->get_thread(t.tid);
    } else {
        t.thread = threads->get_thread_by_handle(static_cast<Handle>(handle));
        if (t.thread != nullptr) t.tid = t.thread->thread_id;
    }
    if (t.thread == nullptr || t.tid == 0 || t.thread->terminated) {
        error_out = error_codes::ERROR_INVALID_HANDLE;
        return Target{};
    }
    return t;
}

/// The target's register file: its pre-launch seed when it has not started
/// (CREATE_SUSPENDED), otherwise its parked snapshot or live state.
bool read_state(const Target& t, cpu::CpuState& out) {
    if (!t.thread->launched) {
        out = t.thread->cpu;
        out.materialize_lazy_flags();
        return true;
    }
    return ThreadSuspendRegistry::instance().capture(t.tid, out);
}

bool write_state(const Target& t, const cpu::CpuState& in) {
    if (!t.thread->launched) {
        // The thread starts at exactly this context when resumed
        // (thread_lifecycle_probe SUSPENDED_IP_REDIRECT / START_REG_REDIRECT).
        t.thread->cpu = in;
        t.thread->context_set = true;
        return true;
    }
    return ThreadSuspendRegistry::instance().apply(t.tid, in);
}

u32 check_record(Marshaler& m, GuestAddr context) {
    if (context == 0) return STATUS_ACCESS_VIOLATION;
    // CONTEXT is DECLSPEC_ALIGN(16) on x64; x86 needs only DWORD alignment.
    const GuestAddr alignment = m.cpu_is_64bit() ? 16 : 4;
    if ((context & (alignment - 1)) != 0) return STATUS_DATATYPE_MISALIGNMENT;
    return STATUS_SUCCESS;
}

} // namespace

void seed_windows_initial_fpu(cpu::CpuState& state) {
    state.fpu.control_word = kWindowsInitialFpuControlWord;
    state.sse.mxcsr = kWindowsInitialMxcsr;
}

u32 nt_get_context_thread(Marshaler& m, ThreadManager* threads, u64 handle,
                          GuestAddr context) {
    u32 access_err = 0;
    const Target t = resolve(threads, handle, ThreadManager::THREAD_GET_CONTEXT,
                             access_err);
    if (t.thread == nullptr) {
        return access_err == error_codes::ERROR_ACCESS_DENIED
                   ? STATUS_ACCESS_DENIED
                   : STATUS_INVALID_HANDLE;
    }
    if (const u32 s = check_record(m, context); s != STATUS_SUCCESS) return s;

    cpu::CpuState state{};
    if (!read_state(t, state)) return STATUS_INVALID_HANDLE;

    if (m.cpu_is_64bit()) {
        const auto flags = m.memory().read_u32(context + cpu::kContext64FlagsOffset);
        if (!flags.is_ok()) return STATUS_ACCESS_VIOLATION;
        if (cpu::store_context64(m.memory(), context, state, flags.value()) !=
            cpu::Context64Status::Ok) {
            return STATUS_ACCESS_VIOLATION;
        }
        // EFlags is reported as the thread holds it (plus IF): a thread that
        // has not run reports 0x200, one given EFlags by SetThreadContext
        // reads back exactly the sanitised value (thread_lifecycle_probe
        // SUSPENDED_START / EFLAGS). store_context64 forces bit 1, which is
        // right for captured and exception contexts but not here.
        if ((flags.value() & 0x1u) != 0 &&
            !m.memory().write_u32(context + kContext64EFlags,
                                  static_cast<u32>(state.flags.rflags) | 0x200u).is_ok()) {
            return STATUS_ACCESS_VIOLATION;
        }
        return STATUS_SUCCESS;
    }
    // x86 (WOW64): measured by thread_context32_probe.
    const auto flags = m.memory().read_u32(context);
    if (!flags.is_ok()) return STATUS_ACCESS_VIOLATION;
    return cpu::store_context32(m.memory(), context, state, flags.value()) ==
                   cpu::Context32Status::Ok
               ? STATUS_SUCCESS
               : STATUS_ACCESS_VIOLATION;
}

u32 nt_set_context_thread(Marshaler& m, ThreadManager* threads, u64 handle,
                          GuestAddr context) {
    u32 access_err = 0;
    const Target t = resolve(threads, handle, ThreadManager::THREAD_SET_CONTEXT,
                             access_err);
    if (t.thread == nullptr) {
        return access_err == error_codes::ERROR_ACCESS_DENIED
                   ? STATUS_ACCESS_DENIED
                   : STATUS_INVALID_HANDLE;
    }
    if (const u32 s = check_record(m, context); s != STATUS_SUCCESS) return s;

    // Start from the current register file so groups the record does not
    // select keep their values.
    cpu::CpuState state{};
    if (!read_state(t, state)) return STATUS_INVALID_HANDLE;

    if (m.cpu_is_64bit()) {
        switch (cpu::load_context64(m.memory(), context, state,
                                    /*restoring_current_thread=*/false)) {
            case cpu::Context64Status::Ok:
                // Learn SetThreadContext: privileged EFlags bits "are silently
                // set to the correct value". Measured mask (x64 and WOW64):
                // (value & 0x00210FD5) | 0x200.
                if ((m.memory().read_u32(context + cpu::kContext64FlagsOffset)
                         .value_or(0) & 0x1u) != 0) {
                    state.flags.rflags =
                        (m.memory().read_u32(context + kContext64EFlags).value_or(0) &
                         0x00210FD5u) | 0x200u;
                }
                break;
            case cpu::Context64Status::DebugRegistersUnsupported:
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED kernel32!SetThreadContext: DR7 "
                             "arms a hardware breakpoint; no debug-register support\n");
                return STATUS_NOT_SUPPORTED;
            case cpu::Context64Status::Fault:
                return STATUS_ACCESS_VIOLATION;
        }
    } else {
        switch (cpu::load_context32(m.memory(), context, state)) {
            case cpu::Context32Status::Ok:
                break;
            case cpu::Context32Status::DebugRegistersUnsupported:
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED kernel32!SetThreadContext: DR7 "
                             "arms a hardware breakpoint; no debug-register support\n");
                return STATUS_NOT_SUPPORTED;
            case cpu::Context32Status::Fault:
                return STATUS_ACCESS_VIOLATION;
        }
    }
    return write_state(t, state) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
}

u32 thread_context_status_to_win32(u32 status) {
    switch (status) {
        case STATUS_SUCCESS:               return 0;
        case STATUS_INVALID_HANDLE:        return 6;    // ERROR_INVALID_HANDLE
        case STATUS_ACCESS_DENIED:         return 5;    // ERROR_ACCESS_DENIED
        case STATUS_DATATYPE_MISALIGNMENT: return 998;  // ERROR_NOACCESS (measured)
        case STATUS_ACCESS_VIOLATION:      return 998;  // ERROR_NOACCESS
        case STATUS_NOT_SUPPORTED:         return 50;   // ERROR_NOT_SUPPORTED
        default:                           return 317;  // ERROR_MR_MID_NOT_FOUND
    }
}

} // namespace vitreo::winapi

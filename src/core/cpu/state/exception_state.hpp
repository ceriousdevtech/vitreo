// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include "vitreo/types.hpp"

namespace vitreo::cpu {

/// Standalone exception state extracted from CpuState.
/// Contains pending software exceptions and unwind requests
/// set by thunks and consumed by the execution loop.
struct ExceptionState {
    struct PendingRaise {
        bool active = false;
        // When true, the raise originated from a JIT slow-path helper
        // (e.g. unmapped guest memory access) rather than from a Win32
        // RaiseException thunk. The SEH dispatcher must NOT simulate a
        // RET-from-thunk in this case — cpu.rip / cpu.sp already reflect
        // the faulting instruction.
        bool from_helper = false;
        u32 code = 0;
        u32 flags = 0;
        u32 num_args = 0;
        u64 args[15] = {};
        void clear() noexcept { active = false; from_helper = false; code = 0; flags = 0; num_args = 0; }
    };

    struct PendingUnwind {
        bool active = false;
        u64 target_ip = 0;
        u64 target_frame = 0;
        u64 return_value = 0;
        // x86 RtlUnwind transfers to TargetIp as though its stdcall had
        // completed in the target function. Preserve that continuation state;
        // the SEH registration record is not itself a machine stack pointer.
        u64 resume_sp = 0;
        u64 resume_bp = 0;
        /// EXCEPTION_RECORD the unwinder was handed (RtlUnwindEx arg 3). The
        /// language handler matches this pointer to recognise its own
        /// exception; the dispatcher's own record is a different object.
        u64 exception_record = 0;
        /// x64: RSP of RtlUnwindEx's caller. Everything at or above it is still
        /// live when the unwind completes, so a frame consolidation (Learn,
        /// RtlRestoreContext) must build its frame below it.
        u64 caller_sp = 0;
        /// x64: RtlUnwindEx was handed a NULL ExceptionRecord. Whoever goes on
        /// with the unwind synthesises STATUS_UNWIND with this ExceptionAddress
        /// (RtlUnwindEx's return address); the record the first walk built
        /// lived in a frame that is gone.
        u64 null_record_address = 0;
        /// x64: the frames of the current guest-callback level, from
        /// RtlUnwindEx's caller up to the host-callback sentinel, have already
        /// been unwound (their language handlers ran). The level must leave
        /// without walking them again; run_guest_callback clears this when it
        /// returns, so the level that called the host code goes on from its
        /// own thunk's caller (winapi/kernel32/thread/unwind64.hpp).
        bool level_walked = false;
        void clear() noexcept {
            active = false;
            target_ip = 0;
            target_frame = 0;
            return_value = 0;
            resume_sp = 0;
            resume_bp = 0;
            exception_record = 0;
            caller_sp = 0;
            null_record_address = 0;
            level_walked = false;
        }
    };

    PendingRaise pending_raise;
    PendingUnwind pending_unwind;

    void clear() noexcept {
        pending_raise.clear();
        pending_unwind.clear();
    }
};

} // namespace vitreo::cpu

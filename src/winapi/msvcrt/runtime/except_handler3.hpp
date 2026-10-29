// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file except_handler3.hpp
/// @brief MSVC `_except_handler3` personality for x86 `__try/__except`.

#include "vitreo/common.hpp"
#include "core/memory/guest_memory.hpp"

#include <functional>

namespace vitreo::cpu {
struct CpuState;
}

namespace vitreo::winapi {

/// Result of walking one EH3 registration's scopetable.
struct Eh3DispatchResult {
    bool handled{false};              ///< filter returned EXECUTE_HANDLER
    bool continue_execution{false};   ///< filter returned CONTINUE_EXECUTION
    u32  handler_addr{0};             ///< `__except` block
    i32  enclosing_level{-1};
    i32  handled_level{-1};
};

/// Call a compiler-generated EH3 filter with the parent function's EBP.
/// Returns the filter's EAX (`EXCEPTION_EXECUTE_HANDLER` / `_SEARCH` / `_CONTINUE_EXECUTION`).
using Eh3FilterCallback = std::function<i32(GuestAddr filter, u32 parent_ebp)>;

/// Walk the EH3 scopetable at `establisher_frame` and evaluate filters.
/// Does not transfer control; the caller patches CONTEXT / FS:[0] on `handled`.
Eh3DispatchResult dispatch_except_handler3_32(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 exception_record_addr,
    const Eh3FilterCallback& call_filter);

/// Write `EXCEPTION_POINTERS` for GetExceptionInformation() in an EH3 filter.
///
/// MSVC stores a pointer at `[ebp-20]` (`establisher_frame - 4`) to an
/// `EXCEPTION_POINTERS` value. That struct must not occupy `[ebp-24]`:
/// EH3 functions keep the saved ESP for `__except` (`mov esp, [ebp-18h]`)
/// in that slot. `pointers_storage` is the 8-byte guest location that
/// holds `{ExceptionRecord, ContextRecord}`.
void install_eh3_exception_pointers(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 exception_record_addr,
    u32 context_record_addr,
    u32 pointers_storage);

/// Resume at the `__except` block: parent EBP, handler EIP, trylevel updated.
void patch_context_for_eh3_handler(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 context_record_addr,
    const Eh3DispatchResult& result);

/// Unlink SEH frames above this registration (simplified RtlUnwind target).
void eh3_unlink_frames_above(cpu::CpuState& cpu, GuestMemory& memory,
                             u32 establisher_frame);

} // namespace vitreo::winapi

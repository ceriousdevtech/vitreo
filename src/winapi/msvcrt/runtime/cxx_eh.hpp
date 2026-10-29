// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file cxx_eh.hpp
/// @brief MSVC C++ exception-handling personality routine support.

#include "vitreo/common.hpp"
#include "core/memory/guest_memory.hpp"

#include <optional>
#include <string>

namespace vitreo::winapi {

/// Result of dispatching a C++ exception against one function's FuncInfo.
/// When `match` is true the caller should install the catch parameter and
/// patch the dispatcher's CONTEXT record so execution resumes at
/// `handler_addr` (returning ExceptionContinueExecution from the SEH
/// personality function).
struct CxxDispatchResult {
    bool match{false};
    u32  handler_addr{0};         ///< addressOfHandler from matched HandlerType
    i32  disp_catch_obj{0};       ///< Frame offset (from EBP) to catch param slot
    u32  adjectives{0};           ///< HandlerType.adjectives (HT_IS_REFERENCE, etc.)
    i32  try_high{0};             ///< For state update after match
    i32  try_low{0};
    u32  catchable_type_addr{0};  ///< Matched CatchableType*
    u32  throw_object_addr{0};    ///< Pointer to throw object (from exception record)
    u32  catchable_size{0};       ///< Size in bytes for catch-by-value copies
    u32  catchable_copy_fn{0};    ///< Copy ctor (currently unused — TODO)
    i32  catchable_pmd_mdisp{0};  ///< this-displacement for base-class match
};

/// Walk FuncInfo / TryBlockMap / HandlerArray to find a catch that matches
/// the C++ exception described by `exception_record_addr`.  Returns the
/// match details on success; `match == false` means no matching catch was
/// found in this function (caller should return ExceptionContinueSearch).
///
/// `func_info_addr` comes from the __ehhandler thunk (it passes FuncInfo*
/// in EAX before jumping to __CxxFrameHandler3 — read it from cpu->gpr.rax).
/// `establisher_frame` is the EH registration record on the guest stack.
CxxDispatchResult dispatch_cxx_exception_32(
    GuestMemory& memory,
    u32 func_info_addr,
    u32 establisher_frame,
    u32 exception_record_addr,
    u32 context_record_addr);

/// Diagnostic: render the throw described by `exception_record_addr` as
/// "obj=.. ti=.. types=[mangled, ...]". Empty when the record is not an MSVC
/// C++ throw. For operator diagnostics only; never affects dispatch.
[[nodiscard]] std::string describe_cxx_throw_32(
    GuestMemory& memory,
    u32 exception_record_addr);

/// Diagnostic: render one function's catch clauses as
/// "state=N tries=[low..high -> mangled, ...]". Empty when `func_info_addr`
/// is not a readable FuncInfo. For operator diagnostics only.
[[nodiscard]] std::string describe_cxx_frame_catches_32(
    GuestMemory& memory,
    u32 func_info_addr,
    u32 establisher_frame);

/// Write the catch parameter (pointer or copied object) into the function's
/// frame slot at `establisher_frame+12 + disp_catch_obj`.  Must be called
/// before returning to the dispatcher with ExceptionContinueExecution.
bool install_catch_param_32(
    GuestMemory& memory,
    const CxxDispatchResult& r,
    u32 establisher_frame);

/// Return the stack top that Vitreo's x86 callback runner must use before
/// invoking an MSVC catch funclet. The callback runner pushes a synthetic
/// four-byte return address, while the funclet expects to enter with ESP equal
/// to the epilogue stack saved at `establisher_frame - 4`.
///
/// Returns no value when the saved stack is absent, unreadable, or cannot be
/// advanced without overflowing the x86 address space.
std::optional<u32> catch_funclet_callback_stack_top_32(
    GuestMemory& memory,
    u32 establisher_frame);

/// Return the stack the catch funclet must *enter* with.
///
/// MSVC runs a catch block from inside the exception dispatcher, whose frame
/// sits **below** the throwing function's. That is what keeps the exception
/// object alive: it is a temporary in the throwing frame, a catch-by-reference
/// binds straight to it, and nothing may reuse that memory until the catch
/// body finishes. Entering the funclet at the catch function's own (already
/// unwound) stack level instead hands every function the catch body calls the
/// object's bytes to scribble on — the catch parameter then dereferences a
/// vftable pointer some unrelated callee overwrote.
///
/// `throw_site_sp` is ESP as captured when the exception was raised, i.e. the
/// deepest point of the live stack. Returns no value when it is unusable (not
/// below the catch frame's epilogue stack, or zero), so the caller can fall
/// back to `catch_funclet_callback_stack_top_32`.
std::optional<u32> catch_funclet_entry_stack_32(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 throw_site_sp);

/// Patch the SEH dispatcher's CONTEXT record for a matched catch. The caller
/// normally invokes the catch funclet first and passes the continuation address
/// it returned; passing zero retains the legacy direct-handler fallback.
void patch_context_for_catch_32(
    GuestMemory& memory,
    const CxxDispatchResult& r,
    u32 establisher_frame,
    u32 context_record_addr,
    u32 continuation_addr = 0);

// =============================================================================
// Rethrow (`throw;`)
//
// MSVC compiles a bare `throw;` to _CxxThrowException(nullptr, nullptr): a
// C++ exception whose object and ThrowInfo are both NULL, meaning "raise the
// exception this catch block is handling again". Without the surrounding
// bookkeeping there is no type to match, so the rethrow finds no handler and
// terminates the process — even though an outer catch was willing to take it.
//
// The exception object is a temporary in the *throwing* frame, which is below
// the catch block's frame and gets overwritten as the catch runs. So the
// object is snapshotted when the catch is entered and re-materialised in a
// stable buffer if a rethrow actually happens.
// =============================================================================

/// Guest buffer holding re-materialised rethrow objects.
constexpr GuestAddr CXX_RETHROW_SCRATCH_BASE = 0x5EE00000;
constexpr u32       CXX_RETHROW_SCRATCH_SIZE = 0x10000; // 64 KB

/// Record the exception a catch block is about to handle, snapshotting its
/// object bytes. Call immediately before invoking the catch funclet.
void push_active_cxx_exception(
    GuestMemory& memory,
    u32 exception_record_addr);

/// Discard the innermost active exception. Call once the catch funclet has
/// returned, successfully or not.
void pop_active_cxx_exception();

/// If `params` describe a bare `throw;`, replace the NULL object and
/// ThrowInfo with the exception currently being handled on this thread.
/// Returns true when a substitution was made.
///
/// `code` and `num_params` are the raised exception's; `params` is the
/// ExceptionInformation array and is modified in place.
[[nodiscard]] bool resolve_cxx_rethrow_32(
    GuestMemory& memory,
    u32 code,
    u32 num_params,
    u32* params);

// Learn: https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/uncaught-exception
// `__uncaught_exception` is true from throw until the matching catch is
// initialized. `__uncaught_exceptions` is the C++17 count of those in flight.
void note_cxx_exception_thrown();
void note_cxx_exception_caught();
[[nodiscard]] int cxx_uncaught_exceptions() noexcept;
[[nodiscard]] bool cxx_uncaught_exception() noexcept;

} // namespace vitreo::winapi

#pragma once
/// @file unwind64.hpp
/// @brief x64 RtlUnwindEx: the one frame walk behind the exception
///        dispatcher's unwind pass and the RtlUnwind / RtlUnwindEx exports.
///
/// Microsoft Learn, "x64 exception handling" (RtlUnwindEx, RtlVirtualUnwind,
/// UNWIND_INFO, DISPATCHER_CONTEXT): starting from a context, each frame is
/// virtually unwound; a frame whose UNWIND_INFO declares UNW_FLAG_UHANDLER has
/// its language handler called with EXCEPTION_UNWINDING (EXCEPTION_EXIT_UNWIND
/// when TargetFrame is NULL, EXCEPTION_TARGET_UNWIND on the frame whose
/// establisher is TargetFrame), until the establisher frame is TargetFrame.
/// That frame's context then resumes with Rip = TargetIp and Rax =
/// ReturnValue through RtlRestoreContext(Context, ExceptionRecord), which
/// applies the STATUS_LONGJUMP jump buffer or runs the
/// STATUS_UNWIND_CONSOLIDATE callback (restore_context64.hpp).
///
/// Vitreo-specific: guest frames above a host call that ran a guest callback
/// are not on the walk's stack -- the callback's frames end in the
/// host-callback sentinel. A walk that reaches it stops there
/// (Unwind64Stop::CallbackBoundary). The RtlUnwindEx export then leaves
/// cpu.pending_unwind for whoever owns the next level: the exception
/// dispatcher (a language handler called RtlUnwindEx during dispatch), or the
/// run loop the host call returns to, which goes on with
/// continue_pending_unwind64 from that host call's caller.

#include <vitreo/common.hpp>

#include <functional>
#include <optional>

namespace vitreo::winapi {

class Marshaler;
class ModuleManager;
class ProcessManager;

/// ntstatus.h.
inline constexpr u32 STATUS_UNWIND_CODE = 0xC0000027u;
inline constexpr u32 STATUS_BAD_STACK_CODE = 0xC0000028u;
inline constexpr u32 STATUS_INVALID_DISPOSITION_CODE = 0xC0000026u;

/// winnt.h EXCEPTION_RECORD::ExceptionFlags.
inline constexpr u32 EXCEPTION_NONCONTINUABLE_BIT = 0x01u;
inline constexpr u32 EXCEPTION_UNWINDING_BIT = 0x02u;
inline constexpr u32 EXCEPTION_EXIT_UNWIND_BIT = 0x04u;
inline constexpr u32 EXCEPTION_TARGET_UNWIND_BIT = 0x20u;
inline constexpr u32 EXCEPTION_COLLIDED_UNWIND_BIT = 0x40u;

/// Guest storage the walk uses. Every address is caller-owned and must stay
/// valid (and not be overwritten by the handlers it calls) for the walk.
struct Unwind64Scratch {
    /// CONTEXT (0x4D0): on entry the first frame's context; virtually
    /// unwound in place, one frame at a time.
    GuestAddr cursor = 0;
    /// CONTEXT (0x4D0, 16-aligned): the frame being unwound, before its own
    /// virtual unwind. The language handler's ContextRecord and
    /// DISPATCHER_CONTEXT.ContextRecord; at TargetReached, the context to
    /// restore.
    GuestAddr frame = 0;
    /// DISPATCHER_CONTEXT (0x50).
    GuestAddr dispatcher = 0;
    /// RtlVirtualUnwind's HandlerData / EstablisherFrame outputs (u64 each).
    GuestAddr handler_data_out = 0;
    GuestAddr establisher_out = 0;
    /// DISPATCHER_CONTEXT.HistoryTable (an UNWIND_HISTORY_TABLE, never NULL
    /// on Windows).
    GuestAddr history_table = 0;
    /// EXCEPTION_RECORD (0x98) for a STATUS_UNWIND the walk synthesises when a
    /// collided unwind was started with a NULL record.
    GuestAddr local_record = 0;
};

/// The unwind being performed. A collided unwind replaces it mid-walk.
struct Unwind64Target {
    u64 frame = 0;            ///< TargetFrame; 0 = exit unwind
    u64 ip = 0;               ///< TargetIp
    GuestAddr record = 0;     ///< EXCEPTION_RECORD, never 0 here
    u64 return_value = 0;     ///< ReturnValue
    /// Lowest guest stack address still live, 0 = none beyond the caller's.
    /// A collided unwind sets it to the RSP of the nested RtlUnwindEx: its
    /// record and everything its caller still owns sit above it, so the
    /// handlers the walk calls afterwards (and a consolidation frame) must
    /// run below it.
    u64 stack_floor = 0;
};

enum class Unwind64Stop {
    /// Scratch.frame is TargetFrame's context with Rip = TargetIp (unless the
    /// record is STATUS_UNWIND_CONSOLIDATE) and Rax = ReturnValue, ready for
    /// RtlRestoreContext(frame, target.record).
    TargetReached,
    /// The next frame is the host-callback sentinel: the rest of the stack
    /// belongs to an enclosing level.
    CallbackBoundary,
    /// No frame left (end of the thread's stack, or EstablisherFrame reached
    /// StackBase) and TargetFrame was not found: an exit unwind completed, or
    /// the target is not on the stack.
    EndOfStack,
    /// STATUS_BAD_STACK: an establisher frame outside the stack, misaligned,
    /// or past TargetFrame.
    BadStack,
    /// STATUS_INVALID_DISPOSITION: a handler returned something other than
    /// ExceptionContinueSearch / ExceptionCollidedUnwind.
    InvalidDisposition,
    /// A handler did not come back normally (a nested raise left for the
    /// outer loop, or the process ended); the CPU is the handler's.
    Interrupted,
};

/// Calls language handler @p handler with the four EXCEPTION_ROUTINE
/// arguments on the walking thread. nullopt when the call did not come back
/// normally (Unwind64Stop::Interrupted). A call that comes back with
/// cpu.pending_unwind active is a nested RtlUnwindEx: the walk treats it as
/// a collided unwind.
using Unwind64HandlerCall =
    std::function<std::optional<u64>(GuestAddr handler, const u64* args)>;

/// The frame walk. @p start_is_return_address: the cursor's Rip is a return
/// address (RtlUnwindEx's caller), so its function is looked up at Rip - 1;
/// false when it is the point an exception was raised at.
[[nodiscard]] Unwind64Stop walk_unwind64(Marshaler& m, ModuleManager& modules,
                                         const Unwind64Scratch& scratch,
                                         Unwind64Target& target,
                                         bool start_is_return_address,
                                         const Unwind64HandlerCall& call_handler);

/// Writes the STATUS_UNWIND record RtlUnwindEx synthesises for a NULL
/// ExceptionRecord: no flags, no parameters, ExceptionAddress = @p address.
void write_status_unwind_record64(Marshaler& m, GuestAddr record, u64 address);

enum class RtlUnwind64Result {
    Transferred,   ///< CPU positioned at the target (thunk_control_transfer set)
    Deferred,      ///< cpu.pending_unwind left for the enclosing level
    Raised,        ///< cpu.pending_raise: STATUS_BAD_STACK / INVALID_DISPOSITION / ACCESS_VIOLATION
    Terminated,    ///< second-chance end of an exit unwind / missing target
    Interrupted,   ///< a handler raised or the process ended; CPU is the handler's
};

/// What RtlUnwindEx was asked to do.
struct RtlUnwind64Request {
    u64 target_frame = 0;
    u64 target_ip = 0;
    GuestAddr record = 0;              ///< 0: synthesise STATUS_UNWIND
    u64 return_value = 0;
    GuestAddr original_context = 0;    ///< RtlUnwindEx OriginalContext, or 0
    GuestAddr history_table = 0;       ///< RtlUnwindEx HistoryTable, or 0
    /// ExceptionAddress of a synthesised STATUS_UNWIND; 0 = RtlUnwindEx's
    /// return address.
    u64 null_record_address = 0;
    /// Lowest stack address still live from an earlier level of this unwind
    /// (pending_unwind.caller_sp when it is continued): the record may live
    /// just above it, so the walk's scratch and handlers stay below it.
    u64 stack_floor = 0;
};

/// RtlUnwindEx for the x64 CPU attached to @p m, positioned at a thunk entry:
/// RSP points at the return address into the caller the walk starts from.
/// Language handlers run through the SEH guest call (c_specific_handler.hpp).
[[nodiscard]] RtlUnwind64Result rtl_unwind_ex64(Marshaler& m,
                                                ModuleManager* modules,
                                                ProcessManager* processes,
                                                const RtlUnwind64Request& request);

/// The x64 RtlUnwind (4 arguments) / RtlUnwindEx (6 arguments) exports of
/// ntdll and kernel32. On x64, RtlUnwind is RtlUnwindEx with no context
/// buffer and no history table (Learn).
void rtl_unwind_export64(Marshaler& m, ModuleManager* modules,
                         ProcessManager* processes, bool is_ex);

/// A run loop saw a thunk come back with cpu.pending_unwind active and not
/// level_walked: the host call it made ran a guest callback whose unwind
/// crossed the callback boundary. Go on with that unwind from the thunk's
/// caller. Clears pending_unwind before walking.
[[nodiscard]] RtlUnwind64Result continue_pending_unwind64(
    Marshaler& m, ModuleManager* modules, ProcessManager* processes);

} // namespace vitreo::winapi

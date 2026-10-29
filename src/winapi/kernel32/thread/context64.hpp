#pragma once
/// @file context64.hpp
/// @brief x64 CONTEXT record access and UNWIND_INFO interpretation.

#include <vitreo/common.hpp>

namespace vitreo::cpu {
struct CpuState;
}

namespace vitreo::winapi {

class Marshaler;

/// Capture the current guest CPU state into a guest CONTEXT record.
///
/// Writes the integer and control registers at their documented AMD64 CONTEXT
/// offsets. This is what backs RtlCaptureContext and the state the exception
/// dispatcher hands to guest code.
///
/// Does nothing when @p ctx_addr is null or no CPU is attached to @p m.
void write_context64(Marshaler& m, GuestAddr ctx_addr);

/// Capture an arbitrary guest CPU state into the register groups requested by
/// an AMD64 CONTEXT record's input ContextFlags.
///
/// Unlike RtlCaptureContext, this preserves unrequested fields and is suitable
/// for GetThreadContext/NtGetContextThread. Returns false when the record is
/// unreadable, is not an AMD64 CONTEXT, or a selected field is unwritable.
[[nodiscard]] bool capture_context64_to(Marshaler& m, GuestAddr ctx_addr,
                                        const cpu::CpuState& source);

/// Restore the selected integer/control state from a guest AMD64 CONTEXT.
///
/// ContextFlags controls which register groups are consumed. The CPU state is
/// updated atomically only after every selected field is readable.
[[nodiscard]] bool restore_context64(Marshaler& m, GuestAddr ctx_addr);

/// Restore selected AMD64 CONTEXT groups into an arbitrary guest CPU state.
/// Used by SetThreadContext/NtSetContextThread for a target other than the
/// currently executing thunk CPU.
[[nodiscard]] bool restore_context64_to(Marshaler& m, GuestAddr ctx_addr,
                                        cpu::CpuState& target);

/// Walk one frame up, per the x64 exception-handling ABI.
///
/// Reads the RUNTIME_FUNCTION at @p function_entry_addr and interprets its
/// UNWIND_INFO codes to reconstruct the caller's register state, updating the
/// CONTEXT at @p ctx_addr in place. Backs RtlVirtualUnwind.
///
/// @param image_base              Base of the module owning the function.
/// @param control_pc              PC being unwound from; selects which prologue
///                                operations have executed and so must be undone.
/// @param function_entry_addr     Guest RUNTIME_FUNCTION for that PC.
/// @param ctx_addr                CONTEXT to update in place.
/// @param handler_data_addr       Receives the handler data pointer, if any.
/// @param establisher_frame_addr  Receives the establisher frame.
/// @param handler_routine_out     Receives the language handler, or 0 when the
///                                unwind info declares none.
/// @param handler_type            RtlVirtualUnwind's HandlerType: the
///                                UNW_FLAG_EHANDLER / UNW_FLAG_UHANDLER kinds
///                                to report; a handler of another kind is
///                                reported as 0 (and so is its data).
/// @return false if the unwind info is unreadable or malformed.
[[nodiscard]] bool unwind_context64(Marshaler& m,
                                    GuestAddr image_base,
                                    GuestAddr control_pc,
                                    GuestAddr function_entry_addr,
                                    GuestAddr ctx_addr,
                                    GuestAddr handler_data_addr,
                                    GuestAddr establisher_frame_addr,
                                    GuestAddr* handler_routine_out,
                                    u32 handler_type = 0x3);

} // namespace vitreo::winapi

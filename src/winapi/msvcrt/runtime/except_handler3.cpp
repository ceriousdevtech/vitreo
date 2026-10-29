// SPDX-License-Identifier: LGPL-2.1-only
/// @file except_handler3.cpp
/// @brief MSVC `_except_handler3` — x86 `__try/__except` personality.
///
/// Microsoft Learn (`_except_handler3`): the CRT uses the registration's
/// scope table to find a filter/handler for the current exception. If a
/// handler is selected, control is transferred into it and this function
/// does not return; otherwise it returns ContinueSearch.
///
/// Compiler-generated x86 layout (MSVC EH3 prologue):
///   push ebp / mov ebp, esp
///   push -1                  ; TryLevel
///   push offset ScopeTable
///   push offset _except_handler3
///   push fs:[0]              ; Next
///   mov  fs:[0], esp         ; EstablisherFrame
///   sub  esp, 8              ; ExceptionPointers slot
///
/// EstablisherFrame:
///   +0  Next, +4 Handler, +8 ScopeTable*, +12 TryLevel (i32, -1 = none)
/// ScopeTable entry (12 bytes):
///   +0  EnclosingLevel (i32), +4 Filter (0 = __finally), +8 Handler

#include "winapi/msvcrt/runtime/except_handler3.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "core/cpu/state/cpu_state.hpp"

namespace vitreo::winapi {
namespace {

constexpr i32 k_trylevel_none = -1;
constexpr u32 k_exception_unwinding = 0x2;
constexpr u32 k_exception_exit_unwind = 0x4;
constexpr i32 k_exception_continue_execution = -1;
constexpr i32 k_exception_continue_search = 0;
constexpr i32 k_exception_execute_handler = 1;

constexpr u32 k_reg_scopetable = 8;
constexpr u32 k_reg_trylevel = 12;
constexpr u32 k_scope_stride = 12;

} // namespace

void install_eh3_exception_pointers(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 exception_record_addr,
    u32 context_record_addr,
    u32 pointers_storage)
{
    // Microsoft Learn (try-except / GetExceptionInformation): the filter
    // loads a PEXCEPTION_POINTERS from [ebp-20]. Do not materialize that
    // struct at [ebp-24] — that slot is the EH3 saved ESP.
    if (establisher_frame < 4 || pointers_storage == 0) return;
    (void)memory.write_u32(pointers_storage + 0, exception_record_addr);
    (void)memory.write_u32(pointers_storage + 4, context_record_addr);
    (void)memory.write_u32(establisher_frame - 4, pointers_storage);
}

void patch_context_for_eh3_handler(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 context_record_addr,
    const Eh3DispatchResult& result)
{
    const u32 parent_ebp = establisher_frame + 16;
    (void)memory.write_u32(context_record_addr + cpu::CTX32_EIP, result.handler_addr);
    (void)memory.write_u32(context_record_addr + cpu::CTX32_EBP, parent_ebp);
    (void)memory.write_u32(establisher_frame + k_reg_trylevel,
                           static_cast<u32>(result.enclosing_level));
}

void eh3_unlink_frames_above(cpu::CpuState& cpu, GuestMemory& memory,
                             u32 establisher_frame)
{
    const GuestAddr fs_base = cpu.seg.fs_desc.base;
    if (fs_base == 0) return;
    (void)memory.write_u32(fs_base + 0, establisher_frame);
}

Eh3DispatchResult dispatch_except_handler3_32(
    GuestMemory& memory,
    u32 establisher_frame,
    u32 exception_record_addr,
    const Eh3FilterCallback& call_filter)
{
    Eh3DispatchResult out{};
    if (establisher_frame == 0 || exception_record_addr == 0) return out;

    auto flags_r = memory.read_u32(exception_record_addr + cpu::EREC32_FLAGS);
    if (flags_r.is_err()) return out;
    const u32 flags = flags_r.value();
    if ((flags & (k_exception_unwinding | k_exception_exit_unwind)) != 0) {
        return out;
    }

    auto table_r = memory.read_u32(establisher_frame + k_reg_scopetable);
    auto level_r = memory.read_u32(establisher_frame + k_reg_trylevel);
    if (table_r.is_err() || level_r.is_err()) return out;
    const u32 scopetable = table_r.value();
    i32 level = static_cast<i32>(level_r.value());
    if (scopetable == 0 || level < 0) return out;

    const u32 parent_ebp = establisher_frame + 16;
    int guard = 0;
    while (level != k_trylevel_none && guard++ < 256) {
        const u32 entry = scopetable + static_cast<u32>(level) * k_scope_stride;
        auto enc_r = memory.read_u32(entry + 0);
        auto filter_r = memory.read_u32(entry + 4);
        auto handler_r = memory.read_u32(entry + 8);
        if (enc_r.is_err() || filter_r.is_err() || handler_r.is_err()) break;

        const i32 enclosing = static_cast<i32>(enc_r.value());
        const u32 filter = filter_r.value();
        const u32 handler = handler_r.value();
        if (filter != 0 && call_filter) {
            const i32 fr = call_filter(filter, parent_ebp);
            if (fr == k_exception_continue_execution) {
                out.continue_execution = true;
                return out;
            }
            if (fr == k_exception_execute_handler) {
                out.handled = true;
                out.handler_addr = handler;
                out.enclosing_level = enclosing;
                out.handled_level = level;
                return out;
            }
            (void)k_exception_continue_search;
        }
        level = enclosing;
    }
    return out;
}

} // namespace vitreo::winapi

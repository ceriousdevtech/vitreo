#pragma once
/// @file exception_dispatch32.hpp
/// @brief 32-bit SEH and C++ exception handling on the guest stack.
///
/// The x86 counterpart to exception_dispatch.hpp. Where 64-bit Windows keeps
/// unwind information in the module's `.pdata`/`.xdata`, 32-bit Windows keeps a
/// linked list of `EXCEPTION_REGISTRATION_RECORD`s on the thread stack, rooted
/// at `fs:[0]`. Dispatching means walking that chain and calling each frame's
/// handler, which runs as *guest code* — so every call here re-enters the
/// guest and returns with whatever state it left behind.
///
/// The C++ half sits on top of the same mechanism. MSVC's `__CxxFrameHandler`
/// is an ordinary SEH handler whose registration carries a `FuncInfo`
/// describing try blocks and catch handlers, so entering a catch means
/// recognising that handler, reading its tables, and transferring control into
/// the catch body with the right frame.

#include <vitreo/common.hpp>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
namespace winapi {
class ThunkRegistry;
class ProcessManager;
struct CxxDispatchResult;
} // namespace winapi
} // namespace vitreo

namespace vitreo::cli {

/// Reject handler addresses that cannot be real code.
///
/// The registration chain lives on the guest stack and is routinely corrupt or
/// uninitialised — trap sentinels (0, 1, 0xa, 0xff) and NULL-page addresses
/// show up constantly. Calling one would fault in the guest for no reason.
[[nodiscard]] bool seh_handler_looks_valid(GuestMemory& memory, u64 handler_addr);

struct Seh32FrameHandlerResult {
    u64 disposition;
    // A nonreturning handler has already restored its continuation's CPU
    // state. Dispatcher scratch may now overlap that continuation's stack.
    bool transferred;
};

/// Call one frame's SEH handler as guest code and report how it left.
///
/// @param scratch_esp Stack pointer to run the handler on. The faulting
///        thread's own stack may be exhausted or mid-unwind, so the caller
///        supplies somewhere safe.
[[nodiscard]] Seh32FrameHandlerResult invoke_seh32_frame_handler(cpu::CpuState& cpu,
                                             GuestMemory& memory,
                                             winapi::ThunkRegistry& thunk_registry,
                                             winapi::ProcessManager& process_mgr,
                                             u32 handler_addr,
                                             u32 establisher_frame,
                                             u32 erec_addr,
                                             u32 ctx_addr,
                                             u32 disp_ctx_addr,
                                             u32 scratch_esp);

/// Apply an unwind a handler requested, moving the CPU to the target frame.
///
/// A handler that unwinds does not return normally — it records where control
/// should resume. This consumes `cpu.pending_unwind` and performs the transfer.
///
/// @return false when no unwind is pending, leaving @p cpu untouched.
[[nodiscard]] bool apply_pending_unwind_32(cpu::CpuState& cpu, GuestMemory& memory,
                                           u32 ctx_addr, const char* trace_prefix);

/// Recognise MSVC's `__CxxFrameHandler` and return its `FuncInfo` address.
///
/// Identified by the prologue pattern rather than by symbol, since guest
/// binaries carry no symbols. Returns 0 when the handler is not a C++ one.
[[nodiscard]] u32 detect_cxx_ehhandler_funcinfo(GuestMemory& memory, u64 handler_addr);

/// True when the `__ehhandler` stub at @p handler_addr tail-jumps (directly,
/// or through an import slot or synthetic-module veneer) into a Vitreo-hosted
/// `__CxxFrameHandler` -- msvcrt.dll, which is part of Windows and so is
/// Vitreo's to implement. A personality that is guest code (the real
/// vcruntime140, or a statically linked CRT) must run as itself, exactly as
/// on Windows, so only the hosted one is dispatched natively.
[[nodiscard]] bool cxx_ehhandler_uses_host_personality(
    GuestMemory& memory, const winapi::ThunkRegistry& thunk_registry,
    u64 handler_addr);

/// Transfer control into a C++ catch handler.
///
/// @param result Which catch was selected, from the FuncInfo tables.
/// @return false when the transfer could not be set up, leaving the caller to
///         continue the search or treat the exception as unhandled.
[[nodiscard]] bool enter_cxx_catch_32(cpu::CpuState& cpu, GuestMemory& memory,
                                      winapi::ThunkRegistry& thunk_registry,
                                      winapi::ProcessManager& process_mgr,
                                      const winapi::CxxDispatchResult& result,
                                      u32 frame_addr, u32 context_addr,
                                      u32 exception_record_addr,
                                      const char* trace_prefix);

/// True while a C++ catch funclet owns the CPU after a rethrow reached an
/// outer catch. The callback path checks it to know that restoring its saved
/// registers would undo a transfer the handler already made.
[[nodiscard]] bool cxx_catch_transfer_active() noexcept;

/// Run the process-wide vectored exception handlers for a 32-bit exception.
///
/// Learn: VEH is *first-chance* — Windows calls it after the debugger and
/// before any frame handler on the `fs:[0]` chain, and it runs whether or not
/// the thread has a single registration record. The 64-bit dispatcher already
/// did this; the 32-bit path did not, so on a 32-bit guest every handler
/// registered through AddVectoredExceptionHandler was silently never called.
///
/// Self-contained on purpose: it builds its own EXCEPTION_RECORD, CONTEXT and
/// EXCEPTION_POINTERS on scratch below the current stack pointer, so each of
/// the three 32-bit dispatch sites needs only one call ahead of its chain walk.
///
/// @return true when a handler returned EXCEPTION_CONTINUE_EXECUTION, in which
///         case @p cpu has been reloaded from the (possibly handler-modified)
///         CONTEXT and execution must resume rather than continue the search.
[[nodiscard]] bool invoke_vectored_handlers_32(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 raise_code, u32 raise_flags, u32 raise_nargs, const u32* raise_args,
    u32 exception_address);

/// Invoke the guest `SetUnhandledExceptionFilter` callback, if any.
///
/// Windows calls this after every frame handler returns ContinueSearch.
/// `EXCEPTION_CONTINUE_EXECUTION` (0xFFFFFFFF) resumes from the (possibly
/// modified) CONTEXT. Other returns leave the exception unhandled.
[[nodiscard]] bool invoke_unhandled_exception_filter_32(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 erec_addr,
    u32 ctx_addr,
    u32 scratch_esp);

/// Dispatch resumes from the CONTEXT at @p ctx_addr: run the vectored
/// continue handlers on it, then load it into @p cpu. Windows runs them
/// whenever dispatch continues execution -- after a vectored handler, a
/// frame-based handler or the top-level filter -- in list order on the same
/// CONTEXT, until one returns EXCEPTION_CONTINUE_EXECUTION
/// (tools/oracle_probe/vectored_continue_probe). The EXCEPTION_POINTERS go
/// below @p scratch_esp. False, with the CPU left as the handler left it,
/// when a handler raised a nested exception or ended the process.
bool resume_from_context_32(
    cpu::CpuState& cpu,
    GuestMemory& memory,
    winapi::ThunkRegistry& thunk_registry,
    winapi::ProcessManager& process_mgr,
    u32 erec_addr,
    u32 ctx_addr,
    u32 scratch_esp);

} // namespace vitreo::cli

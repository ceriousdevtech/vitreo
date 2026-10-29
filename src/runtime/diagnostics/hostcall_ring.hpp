#pragma once
/// @file hostcall_ring.hpp
/// @brief Ring buffer of recent guest→host API calls, for post-mortem dumps.
///
/// When a guest dies somewhere unhelpful — a spin, an access violation, an
/// abrupt exit — the useful question is what it was doing just before. This
/// records the last N hostcalls with caller module and RVA, arguments, return
/// values, and `GetLastError` on either side, so a crash can be read backwards.
///
/// Opt-in: capture is off unless `VITREO_HOSTCALL_RING` is set, and
/// `hostcall_record()` is the only entry point that costs anything when it is.
///
/// The dump itself is published to layers below the CLI through
/// `vitreo::diagnostics::set_hostcall_dump` — the CPU, the WinAPI thunks, and
/// IPC all notice conditions worth dumping but cannot link the runtime that
/// owns this ring.

#include <vitreo/common.hpp>
#include <string_view>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
namespace winapi { class ThunkRegistry; class ModuleManager; }
} // namespace vitreo

namespace vitreo::cli {

/// Whether any capture is configured. False unless one of the
/// VITREO_HOSTCALL_* variables is set, in which case the loop can skip the
/// per-call bookkeeping entirely.
[[nodiscard]] bool hostcall_capture_enabled();

/// One recorded hostcall.
///
/// Callers fill this in two stages around the call — `hostcall_fill_pre`
/// before, `hostcall_fill_post` after — then hand it to `hostcall_record`.
/// It is a value type so the caller can keep it on the stack across a call
/// that may not return.
struct HostcallEntry {
    u64 seq{0};
    u32 tid{0};
    u8  is_64bit{0};
    u64 caller_rip{0};
    u64 esp_before{0};
    u64 ret_addr{0};
    u64 args[8]{};
    u64 eax_in{0};
    u64 eax_out{0};
    u64 ebx_in{0};
    u64 ecx_in{0};
    u64 edx_in{0};
    u32 gle_before{0};
    u32 gle_after{0};
    u32 caller_rva{0};
    char caller_mod[64]{};
    char name[64]{};
    u8  arg_preview[2][64]{};
    u8  arg_preview_len[2]{};
    u64 out_preview_addr{0};
    u8  out_preview[64]{};
    u8  out_preview_len{0};
    // GetQueuedCompletionStatus outputs: byte count, key, OVERLAPPED pointer.
    // A readable value of zero differs from an unreadable output address.
    u64 completion_outputs[3]{};
    u8 completion_readable{0};
    bool completion_snapshot{false};
};

/// Exact, case-insensitive API leaves separated by commas; empty tokens ignored.
/// An absent/empty selector retains the historical all-API behavior.
bool hostcall_detail_matches(std::string_view name, std::string_view selector);

/// Read only the three IOCP output scalars; never changes guest API state.
void hostcall_capture_completion_outputs(HostcallEntry& e, GuestMemory& memory);

/// Capture the pre-call half: caller, arguments, register inputs, and
/// best-effort previews of the first two pointer arguments.
/// @param modules Resolves the caller's module and RVA. May be null during
///        early bootstrap, before the module manager is wired.
void hostcall_fill_pre(HostcallEntry& e, const cpu::CpuState& cpu,
                       GuestMemory& memory, const winapi::ThunkRegistry& registry,
                       winapi::ModuleManager* modules);

/// Capture the post-call half: return value and resulting `GetLastError`.
void hostcall_fill_post(HostcallEntry& e, const cpu::CpuState& cpu,
                        GuestMemory& memory);

/// Commit a filled entry to the ring, subject to the configured filters.
void hostcall_record(const HostcallEntry& e);

/// Print the ring to the diagnostic stream, tagged with what prompted it.
void hostcall_dump(const char* reason);

} // namespace vitreo::cli

/// The dump, as a C symbol so it can be installed as a hook by layers that do
/// not link the runtime owning the ring. See vitreo::diagnostics.
extern "C" void vitreo_hostcall_dump(const char* reason);

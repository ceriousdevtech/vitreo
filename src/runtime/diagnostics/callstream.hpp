// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file callstream.hpp
/// @brief Canonical whole-program API call recorder.
///
/// Per-call value comparison -- "call this API, check the return" -- is blind
/// to everything that makes a real program work: the ORDER effects happen in,
/// state that accumulates across calls, interaction between subsystems, and
/// any API whose job is to configure something rather than return a value.
/// That blindness is measured, not theoretical: a probe suite scored 0/99 on
/// `_controlfp` while the FPU underneath was set to trap on ordinary
/// arithmetic (see [[crt-x87-mask-bits-reversed]]), because it read the
/// setting back instead of exercising what the setting controls.
///
/// This records the whole program instead: every import call, in order, with
/// canonicalised arguments and results. Compare streams from runtime runs
/// to locate the first observable divergence for further investigation.
///
/// Capture is generic: the thunk address yields the function name via
/// ThunkRegistry::resolve_name() and the argument size via
/// resolve_stdcall_bytes(), so no per-function schema is needed and every
/// export is covered automatically, including ones nobody has thought to test.
///
/// Enabled by `VITREO_CALLSTREAM=<path>`; costs nothing when unset.

#include <vitreo/common.hpp>

#include <atomic>
#include <string>

namespace vitreo {
class GuestMemory;
namespace cpu { struct CpuState; }
}

namespace vitreo::diagnostics {

namespace detail {

/// Tri-state cache of the knob: -1 unknown, 0 off, 1 on. Namespace scope, not
/// a function-local static: on Apple a function-local static costs a
/// `_tlv_get_addr` for its guard. The gate is queried on every guest->host
/// thunk dispatch, so the disabled path must not even be a cross-TU call —
/// `stream()` (which did the check) was a top symbol in a live browser-guest
/// profile with the knob unset.
extern std::atomic<int> g_callstream_state;

/// One-time open of the stream. Out of line; never inlined into the gate.
[[nodiscard]] bool callstream_init_slow() noexcept;

}  // namespace detail

/// True when VITREO_CALLSTREAM names an output file. Checked once.
[[nodiscard]] inline bool callstream_enabled() noexcept {
    const int seen =
        detail::g_callstream_state.load(std::memory_order_relaxed);
    if (seen >= 0) return seen != 0;
    return detail::callstream_init_slow();
}

/// True when recording should leave Vitreo's fast thunk paths in place.
///
/// Recording normally disables them so no call can bypass a handler. But that
/// slows the guest a lot, and a game whose behaviour depends on how fast it
/// runs will then be measured in a state the instrument created. Setting
/// VITREO_CALLSTREAM_KEEP_FASTPATH=1 restores the fast paths: kernel32 hot
/// calls go missing from the stream, but everything else (the whole CRT
/// included) is still recorded, at close to normal speed.
[[nodiscard]] bool callstream_keep_fastpath() noexcept;

/// Tell the recorder where the guest image lives, so pointers into it can be
/// reported as image-relative rather than as absolute addresses that shift
/// between runs and between hosts.
void callstream_set_image_range(u64 base, u64 size) noexcept;

/// Record one completed call.
///
/// @param name    "dll.Function" as ThunkRegistry reports it.
/// @param tid     Guest thread id; emitted as a first-seen ordinal.
/// @param args    Raw argument words read from the guest stack.
/// @param argc    How many of them (stdcall_bytes / 4).
/// @param retval  The value the handler returned.
/// @param memory  Used to classify pointer arguments and read short strings.
/// @param caller Guest return address, reported image-relative. Without it a
///        hot function is a number with no owner; with it the guest call site
///        is identifiable and comparable against a reference trace.
void callstream_record(const std::string& name, u32 tid,
                       const u64* args, u32 argc, u64 retval,
                       GuestMemory& memory, const cpu::CpuState& cpu,
                       u64 caller);

/// Flush and close. Safe to call more than once.
void callstream_close() noexcept;

} // namespace vitreo::diagnostics

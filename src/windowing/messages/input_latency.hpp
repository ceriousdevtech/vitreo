// SPDX-License-Identifier: LGPL-2.1-only

/// @file input_latency.hpp
/// @brief How long a mouse or key press waits, and how long the guest takes on it.
///
/// `VITREO_INPUT_LATENCY=<path>` (or `=1` for stderr) turns this on. Each input
/// message contributes two intervals:
///
///   queue  — MessageQueue::post_impl → the PeekMessage/GetMessage that removed
///            it. This is Vitreo's own responsiveness: how long an event sat
///            before the guest was willing to look at it.
///   proc   — DispatchMessage entry → the guest wndproc returning. This is the
///            app's own work, charged to the guest, and it is where a slow JIT
///            or a blocking API call shows up.
///
/// Both are recorded in nanoseconds and reported as a per-message-id percentile
/// table when the process exits. The per-message log line is opt-in on top
/// (`VITREO_INPUT_LATENCY_TRACE=1`) because a mouse-move flood writing a line
/// each is exactly the kind of probe that slows the guest enough to hide the
/// thing being measured — the tooltip stall was invisible under VITREO_TRACE
/// for that reason. The summary path costs two clock reads and an array append.
///
/// The peek→dispatch handoff cannot ride on MSG: DispatchMessage reads the MSG
/// back out of guest memory, and MSG::host_post_ns is host-side only. The
/// timestamp is parked in a thread-local between the two instead, which is
/// sound because a Win32 pump peeks and dispatches on one thread.

#pragma once

#include "vitreo/types.hpp"

#include <cstdio>
#include "windowing/messages/message_types.hpp"

namespace vitreo::windowing::input_latency {

/// CLOCK_MONOTONIC nanoseconds. Cheap enough to call unconditionally.
[[nodiscard]] u64 now_ns() noexcept;

/// True when VITREO_INPUT_LATENCY names a destination. Read once.
[[nodiscard]] bool enabled() noexcept;

/// A user ACTION (button, wheel, key -- not a mouse move) was just dequeued.
/// Starts the click-to-pixels clock; the present path stops it. This is the
/// only end-to-end number here, and the one a user actually experiences:
/// everything else measures a single leg of the pipeline.
void note_action_event() noexcept;

/// Nanoseconds since the oldest unanswered action event, or 0 if none is
/// outstanding. Clears the pending mark. Called from the present path.
[[nodiscard]] u64 take_pending_action_age() noexcept;

/// How long a macOS event sat between AppKit timestamping it and Vitreo
/// dequeuing it in pump_events. This is the part of input latency that happens
/// BEFORE MessageQueue::post_impl, so the queue/proc pair below cannot see it —
/// and it is exactly where a pump interval lands.
void note_event_age(u64 age_ns) noexcept;

/// True for the messages this probe accounts: keyboard (WM_KEYFIRST..
/// WM_KEYLAST) and mouse (WM_MOUSEFIRST..WM_MOUSELAST).
[[nodiscard]] bool is_input_message(u32 message) noexcept;

/// A PeekMessage/GetMessage removed `msg` from the queue. Records the queue
/// wait and parks the arrival time for the DispatchMessage that follows.
void note_delivered(const MSG& msg) noexcept;

/// DispatchMessage is about to run the guest wndproc for `msg`. Returns the
/// start timestamp to hand back to note_dispatch_end, or 0 when this message
/// is not being accounted.
[[nodiscard]] u64 note_dispatch_begin(const MSG& msg) noexcept;

/// Vitreo's post-dispatch framebuffer flush for `msg` finished. `begin_ns` is
/// what note_dispatch_begin gave before the flush. Charged separately because
/// it runs INSIDE the dispatch interval: it is Vitreo's presentation work, not
/// the app's, and without this split it reads as a slow guest wndproc.
void note_flush(const MSG& msg, u64 begin_ns) noexcept;

/// The guest wndproc returned. `begin_ns` is what note_dispatch_begin gave.
void note_dispatch_end(const MSG& msg, u64 begin_ns) noexcept;

/// Write the percentile table to `out`. Safe to call repeatedly — registered
/// as a hang-dump section so `kill -USR1 <pid>` prints it on demand. That is
/// the only way to get it out of a GUI guest: such a process parks in its
/// message loop and is killed, so std::atexit never runs.
void report_to(std::FILE* out) noexcept;

/// Write the percentile table to the destination VITREO_INPUT_LATENCY named.
/// Registered with std::atexit; prints at most once.
void report() noexcept;

} // namespace vitreo::windowing::input_latency

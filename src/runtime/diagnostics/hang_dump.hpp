// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

/// @file hang_dump.hpp
/// @brief On-demand guest-thread position dump for a process that is stuck.
///
/// A hung guest looks identical from the outside whether one thread is spinning
/// in a loop that never exits, every thread is waiting on a lock nobody holds,
/// or the run is simply slow. Telling those apart needs the guest RIP of each
/// thread, which host tools cannot supply: `sample` sees only JIT arena
/// addresses, and lldb cannot always attach. Without this, the only way to
/// place a stall was to restart the run under an instruction-count probe and
/// hope to reach the same state again — which for a game means replaying every
/// menu click.
///
/// Sending SIGUSR1 (or SIGINFO, i.e. Ctrl-T on a terminal) prints every
/// published guest thread's RIP/SP/BP/GPRs three times a short interval apart,
/// plus a short stack sample and BP walk when guest memory is bound. A single
/// request distinguishes a thread pinned at one RIP from one that is moving,
/// and shows the caller without an instruction-count restart. Threads are
/// never stopped to do this, so each line is a best-effort read; see
/// ThreadSuspendRegistry::sample_threads.

#include "vitreo/common.hpp"

#include <cstdio>
#include <functional>
#include <string>

namespace vitreo::diagnostics {

/// Install the SIGUSR1/SIGINFO handlers and start the thread that services
/// them. Idempotent; safe to call before any guest thread exists.
void install_hang_dump();

/// Register a subsystem dumper run as part of every hang dump.
///
/// A thread list says a lock is being waited on; it cannot say whether that
/// lock is actually held. Distinguishing a genuine deadlock cycle (the owner
/// is itself blocked) from a lost wakeup (the lock is free and the sleeper was
/// never notified) needs the lock's own words, so the subsystem that owns them
/// contributes them here.
void register_hang_dump_section(void (*fn)(std::FILE*));

/// Install a resolver that names a guest code address.
///
/// Every thread parked in a Win32 call sits at an address inside the thunk
/// region, and those addresses are pure hex here: telling "everyone is waiting
/// on an event" apart from "everyone is inside the same lock" otherwise means
/// hand-correlating each RIP against a registry dump from the same run. The
/// layer that owns the thunk registry installs the lookup; diagnostics only
/// calls it. Return an empty string for an address that names nothing.
void set_hang_dump_symbolizer(std::function<std::string(u64)> fn);

}  // namespace vitreo::diagnostics

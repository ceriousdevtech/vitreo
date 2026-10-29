// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS
//
// POSIX recovery for MEM_WRITE_WATCH protection faults.
//
// Hardware write-watch arms clean guest pages PROT_READ (see
// write_watch_traps.hpp). A guest thread's fault is delivered to the Mach
// exception bridge, but a HOST thread that writes guest memory without going
// through record_store -- and any code path that misses it -- lands on the
// ordinary signal handler instead. Without this the arming would turn a
// legitimate host write into a fatal SIGSEGV.

#pragma once

namespace vitreo::memory {

/// Install the SIGSEGV/SIGBUS write-watch recovery handler once, chaining to
/// whatever was installed before (the fatal crash dumper). Idempotent.
void install_write_watch_signal_handler();

} // namespace vitreo::memory

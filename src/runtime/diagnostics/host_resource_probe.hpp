// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include <cstdio>

namespace vitreo::diagnostics {

/// Host-side resource accounting for a live guest process.
///
/// Two host resources are invisible to the tools normally reached for:
///   * Mach ports live in the task's IPC space and cost kernel memory only,
///     so they never appear in RSS or in any `vmmap`/`heap` output.
///   * Large host `malloc` blocks have no attribution unless something records
///     a backtrace at the allocating call, and `MallocStackLogging` does not
///     survive in this process.
///
/// Both are read from inside the process, so neither needs `task_for_pid` and
/// therefore neither needs root.
///
/// Installing the probe wires a section into the SIGUSR1 hang dump. The Mach
/// port census is always available; the large-allocation recorder is gated on
/// `VITREO_LARGE_ALLOC_PROBE=1` because it hooks every host malloc/free.
/// `VITREO_LARGE_ALLOC_BYTES` overrides the recording threshold (default 32MB).
void install_host_resource_probe();

/// Write the census directly. Exposed for tests and for callers that want the
/// numbers without raising a signal.
void dump_host_resources(std::FILE* out);

}  // namespace vitreo::diagnostics

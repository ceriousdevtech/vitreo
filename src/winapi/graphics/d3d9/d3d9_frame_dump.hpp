// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS
//
// On-demand capture of the frame the guest is about to present.
//
// A host screenshot is not a substitute: macOS strips windows from a capture
// taken without Screen Recording rights, and a shell-script app bundle does
// not reliably own the permission of the `screencapture` child it spawns. This
// reads the swap chain's own back buffer, so it is pixel-exact, needs no TCC
// grant, and shows what Vitreo rendered rather than what the compositor did
// with it.

#pragma once

#include <vitreo/common.hpp>

namespace vitreo::graphics {
class BackendSwapChain;
}

namespace vitreo::winapi {

/// Arm the SIGUSR2 handler once, if VITREO_FRAME_DUMP names an output path.
/// No-op otherwise, so an unconfigured run pays nothing.
void d3d9_frame_dump_init();

/// True when a dump was requested and not yet taken. Cheap enough to test on
/// every Present.
[[nodiscard]] bool d3d9_frame_dump_pending() noexcept;

/// Read @p chain's back buffer and write it as a BMP. Clears the pending flag
/// whether or not it succeeds, so a failing capture cannot stall every frame.
void d3d9_frame_dump_write(graphics::BackendSwapChain& chain);

} // namespace vitreo::winapi

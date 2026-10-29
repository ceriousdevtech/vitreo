// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#pragma once

#include <vitreo/common.hpp>

namespace vitreo::diagnostics {

/// Last line of defence against freezing the host.
///
/// The memory floor (core/memory/host_memory_floor.hpp) refuses new GPU
/// resources and bulk commits, but it cannot stop a guest from TOUCHING
/// memory it committed while the host still had room. macOS then neither
/// fails nor kills anything: it compresses and swaps until WindowServer stops
/// responding, and the machine needs a forced power-off (measured three times
/// on a world load, 24 GB Mac, no jetsam event, nothing logged).
///
/// The brake polls host-available memory every 250 ms. When it falls below
/// half the floor AND this process's physical footprint exceeds a quarter of
/// RAM -- it is the consumer, not a bystander -- it logs why and terminates
/// the process. VITREO_MEMORY_FLOOR_MB=0 disables it with the floor.
void install_memory_brake();

/// The decision, separated for tests: terminate when the host is below
/// half the floor and this process holds more than a quarter of RAM.
[[nodiscard]] bool memory_brake_should_fire(u64 host_available, u64 floor,
                                            u64 footprint,
                                            u64 physical) noexcept;

}  // namespace vitreo::diagnostics

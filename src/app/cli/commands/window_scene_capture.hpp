// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "winapi/graphics/gdi32/gdi32.hpp"
namespace vitreo::cli {
struct RuntimeContext;
[[nodiscard]] std::optional<std::vector<Byte>> capture_local_window_scene(RuntimeContext& ctx);
[[nodiscard]] winapi::GdiManager::SystemRegionState capture_system_region_scene(RuntimeContext& ctx);
}

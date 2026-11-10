// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include "hwnd_manager.hpp"
#include <array>
#include <optional>
#include <span>
#include <string>

namespace vitreo::windowing {
// Owned transport values, never guest pointers or persistent duplicate state.
struct WindowScene {
    struct Shape {u64 hwnd;std::array<i32,4> bounds;std::vector<std::array<i32,4>> parts;};
    u32 owner_host_pid{0},owner_guest_pid{0};
    u64 generation{0};
    std::string capsule_scope;
    std::vector<HwndManager::Geometry> windows;
    std::vector<Shape> shapes;
    std::vector<std::pair<u64,bool>> layers;
};
inline constexpr usize kWindowSceneMaxBytes=1024*1024;
[[nodiscard]] std::optional<std::vector<Byte>> encode_window_scene(const WindowScene& scene);
[[nodiscard]] std::optional<WindowScene> decode_window_scene(std::span<const Byte> bytes);
}

// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service group: windowing & UI

#pragma once

#include <vitreo/types.hpp>

namespace vitreo::windowing {
class HwndManager;
class MessageQueue;
class CocoaBridge;
} // namespace vitreo::windowing

namespace vitreo::winapi {
class WindowClassManager;
class WindowManager;
class PaintingManager;
class GdiManager;
} // namespace vitreo::winapi

namespace vitreo {

struct UIServices {
    static constexpr u32 kServiceId = 5;

    windowing::HwndManager&     hwnd_mgr;
    windowing::MessageQueue&    msg_queue;
    windowing::CocoaBridge&     cocoa_bridge;
    winapi::WindowClassManager& wclass_mgr;
    winapi::WindowManager&      window_mgr;
    winapi::PaintingManager&    paint_mgr;
    winapi::GdiManager&         gdi_mgr;
};

} // namespace vitreo

// SPDX-License-Identifier: LGPL-2.1-only

/// @file active_backend.cpp
/// @brief Storage for the process's active graphics backend.

#include "graphics/contracts/backend_device.hpp"

#include <atomic>

namespace vitreo::graphics {
namespace {

/// Installed once during startup and read from guest threads answering
/// adapter-identity calls, so it is atomic rather than lock-guarded — a
/// reader must never block on the installer.
std::atomic<GraphicsBackendDevice*> g_active_device{nullptr};

} // namespace

GraphicsBackendDevice* active_backend_device() noexcept {
    return g_active_device.load(std::memory_order_acquire);
}

void set_active_backend_device(GraphicsBackendDevice* device) noexcept {
    g_active_device.store(device, std::memory_order_release);
}

} // namespace vitreo::graphics

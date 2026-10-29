// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_foreign.cpp
/// @brief Cross-process and foreign-window DXGI presentation: the owner
///        side of a shared window surface and the foreign present
///        worker queue.
///        Split out of d3d11_swapchain.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_swapchain_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/common/virtual_display_modes.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

bool D3D11Manager::present_shared_window_surface(
    u64 hwnd, u64 shared_handle, u32 width, u32 height, u32 sync_interval,
    u32 src_x, u32 src_y) {
    if (hwnd == 0 || shared_handle == 0 || width == 0 || height == 0 ||
        !m_foreign_surface_view_resolver) {
        d3d11_diag(
            "present_shared_window_surface reject hwnd=0x%llx handle=0x%llx "
            "%ux%u resolver=%d",
            static_cast<unsigned long long>(hwnd),
            static_cast<unsigned long long>(shared_handle), width, height,
            m_foreign_surface_view_resolver ? 1 : 0);
        return false;
    }
    void* native_view = m_foreign_surface_view_resolver(hwnd);
    if (native_view == nullptr) {
        d3d11_diag(
            "present_shared_window_surface no native view hwnd=0x%llx "
            "handle=0x%llx",
            static_cast<unsigned long long>(hwnd),
            static_cast<unsigned long long>(shared_handle));
        return false;
    }

    auto& surface = m_external_surfaces[hwnd];
    const bool rebuild = surface.width != width || surface.height != height ||
                         surface.native_view != native_view ||
                         !surface.swap_chain;
    if (rebuild) {
        graphics::BackendSwapChainDesc desc{};
        desc.native_view = native_view;
        desc.width = width;
        desc.height = height;
        desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
        desc.buffer_count = 2;
        // Microsoft Learn, IDXGISwapChain::Present: SyncInterval 0 presents
        // immediately; 1-4 wait that many vertical blanks. This is the
        // originating guest's value, forwarded across the process boundary
        // because the layer lives here in the owner. It used to be hardcoded
        // true, so a child that asked to present immediately was display-synced
        // anyway -- and each such child then blocked its producer for a refresh
        // interval. `present_texture` below re-applies it every frame, since a
        // swap chain may change interval between presents.
        desc.vsync = sync_interval != 0;
        // Owner-side blit of a DComp IOSurface: honour per-pixel alpha.
        desc.premultiplied_alpha = true;
        auto swap_chain = m_backend.create_swap_chain(desc);
        if (!swap_chain || !swap_chain->valid()) {
            d3d11_diag(
                "present_shared_window_surface create_swap_chain failed "
                "hwnd=0x%llx handle=0x%llx %ux%u",
                static_cast<unsigned long long>(hwnd),
                static_cast<unsigned long long>(shared_handle), width, height);
            m_external_surfaces.erase(hwnd);
            return false;
        }
        surface.width = width;
        surface.height = height;
        surface.native_view = native_view;
        surface.textures.clear();
        surface.swap_chain = std::move(swap_chain);
    }

    auto texture = surface.textures.find(shared_handle);
    if (texture == surface.textures.end()) {
        auto imported = m_backend.open_shared_texture_2d(shared_handle);
        if (!imported || !imported->valid()) {
            d3d11_diag(
                "present_shared_window_surface open_shared failed "
                "hwnd=0x%llx handle=0x%llx %ux%u",
                static_cast<unsigned long long>(hwnd),
                static_cast<unsigned long long>(shared_handle), width, height);
            return false;
        }
        // The presented rectangle must lie inside the shared texture; it is
        // the whole texture unless a composition clip scoped it.
        if (imported->desc().width < src_x + width ||
            imported->desc().height < src_y + height ||
            imported->desc().pixel_format != graphics::PixelFormat::BGRA8Unorm) {
            d3d11_diag(
                "present_shared_window_surface texture mismatch hwnd=0x%llx "
                "handle=0x%llx expected=%ux%u BGRA8 got=%ux%u fmt=%u",
                static_cast<unsigned long long>(hwnd),
                static_cast<unsigned long long>(shared_handle), width, height,
                imported->desc().width, imported->desc().height,
                static_cast<unsigned>(imported->desc().pixel_format));
            return false;
        }
        texture = surface.textures.emplace(shared_handle, std::move(imported)).first;
    }
    const bool whole = src_x == 0 && src_y == 0 &&
                       texture->second->desc().width == width &&
                       texture->second->desc().height == height;
    const bool presented = whole
        ? surface.swap_chain->present_texture(texture->second.get(),
                                              sync_interval, 0)
        : surface.swap_chain->present_texture_region(
              texture->second.get(),
              graphics::BackendSwapChain::PresentRegion{src_x, src_y, width, height},
              sync_interval, 0);
    if (!presented) {
        d3d11_diag(
            "present_shared_window_surface present_texture failed "
            "hwnd=0x%llx handle=0x%llx %ux%u",
            static_cast<unsigned long long>(hwnd),
            static_cast<unsigned long long>(shared_handle), width, height);
        return false;
    }

    // The producer can recycle this flip buffer as soon as the synchronous
    // IPC reply arrives. Cross-process Metal queues have no implicit ordering,
    // so acknowledge only after this submission's IOSurface -> staging copy
    // has stopped reading the shared allocation. Do not wait for queue-wide
    // idle: the drawable blit reads staging, and unrelated later GPU work must
    // not serialize every foreign-window frame.
    auto completion = m_backend.flush_completion();
    const bool consumed = completion
        ? completion->wait()
        : m_backend.wait_idle();
    if (!consumed) {
        d3d11_diag(
            "present_shared_window_surface GPU consume wait failed "
            "hwnd=0x%llx handle=0x%llx",
            static_cast<unsigned long long>(hwnd),
            static_cast<unsigned long long>(shared_handle));
        return false;
    }
    return true;
}

void D3D11Manager::enqueue_foreign_surface_present(
    u64 hwnd, u64 shared_handle, u32 width, u32 height, u32 sync_interval,
    u32 src_x, u32 src_y) {
    auto completion = m_backend.flush_completion();
    std::lock_guard<std::mutex> lock(m_foreign_present_mutex);
    if (const auto old = m_pending_foreign_surfaces.find(hwnd);
        old != m_pending_foreign_surfaces.end()) {
        // SyncInterval 0 permits a newer flip-model frame to supersede a
        // queued frame. The old IOSurface was never handed to the owner, so
        // it no longer needs transport-side reuse protection.
        m_foreign_surfaces_in_flight.erase(old->second.shared_handle);
    }
    m_pending_foreign_surfaces[hwnd] = PendingForeignSurface{
        .hwnd = hwnd,
        .shared_handle = shared_handle,
        .width = width,
        .height = height,
        .src_x = src_x,
        .src_y = src_y,
        .sync_interval = sync_interval,
        .completion = std::move(completion),
    };
    m_foreign_surfaces_in_flight.insert(shared_handle);
    if (!m_foreign_present_worker.joinable()) {
        m_foreign_present_worker = std::jthread(
            [this](std::stop_token stop) {
                run_foreign_present_worker(stop);
            });
    }
    m_foreign_present_cv.notify_one();
}

void D3D11Manager::wait_for_foreign_surface_reuse(u64 shared_handle) {
    std::unique_lock<std::mutex> lock(m_foreign_present_mutex);
    m_foreign_present_cv.wait(lock, [this, shared_handle] {
        return !m_foreign_surfaces_in_flight.contains(shared_handle);
    });
}

void D3D11Manager::run_foreign_present_worker(std::stop_token stop) {
    std::stop_callback wake(stop, [this] {
        m_foreign_present_cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(m_foreign_present_mutex);
    while (!stop.stop_requested()) {
        m_foreign_present_cv.wait(lock, [&] {
            return stop.stop_requested() ||
                   !m_pending_foreign_surfaces.empty();
        });
        if (stop.stop_requested()) {
            return;
        }
        auto pending = std::move(m_pending_foreign_surfaces);
        m_pending_foreign_surfaces.clear();
        lock.unlock();
        // Command-queue order is per-process. Wait only for the submission
        // captured at Present — later GPU work must not stall this frame
        // (Microsoft Learn: SyncInterval 0 queues and returns).
        for (auto& [hwnd, frame] : pending) {
            bool synchronized = true;
            if (frame.completion) {
                synchronized = frame.completion->wait();
            } else {
                synchronized = m_backend.wait_idle();
            }
            if (!synchronized) {
                d3d11_diag("Present foreign surface wait failed");
            } else if (m_window_surface_presenter) {
                const bool presented = m_window_surface_presenter(
                    frame.hwnd, frame.shared_handle, frame.width,
                    frame.height, frame.sync_interval, frame.src_x,
                    frame.src_y);
                d3d11_diag(
                    "Present foreign surface HWND=0x%llx handle=0x%llx "
                    "%ux%u sync=ok -> %s",
                    static_cast<unsigned long long>(hwnd),
                    static_cast<unsigned long long>(frame.shared_handle),
                    frame.width, frame.height,
                    presented ? "ok" : "failed");
            }
            lock.lock();
            m_foreign_surfaces_in_flight.erase(frame.shared_handle);
            m_foreign_present_cv.notify_all();
            lock.unlock();
        }
        lock.lock();
        m_foreign_present_cv.notify_all();
    }
}

} // namespace vitreo::winapi

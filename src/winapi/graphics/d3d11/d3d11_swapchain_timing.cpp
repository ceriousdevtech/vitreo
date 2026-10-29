// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_timing.cpp
/// @brief DXGI swap-chain frame pacing and timing: the frame-latency
///        waitable object and GetFrameStatistics.
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

bool D3D11Manager::ensure_swap_chain_latency_object(SwapChain& chain) {
    if (chain.latency) return true;
    if (!m_sync_services.create_semaphore) {
        DXGI_UNIMPLEMENTED_ONCE(
            "dxgi!IDXGISwapChain2::GetFrameLatencyWaitableObject: no kernel "
            "object services -> NULL");
        return false;
    }
    // One frame per unit of latency is available at once; Present
    // retirements refill it up to the latency (measured).
    const u64 handle = m_sync_services.create_semaphore(
        static_cast<i32>(chain.maximum_frame_latency),
        static_cast<i32>(kDxgiMaxFrameLatency));
    if (handle == 0) return false;
    chain.latency = std::make_shared<SwapChain::LatencyObject>();
    chain.latency->handle = handle;
    chain.latency->cap = chain.maximum_frame_latency;
    return true;
}

void D3D11Manager::close_swap_chain_latency_object(SwapChain& chain) {
    if (!chain.latency) return;
    u64 handle = 0;
    {
        std::lock_guard<std::mutex> lock(chain.latency->mutex);
        chain.latency->alive = false;
        handle = chain.latency->handle;
        chain.latency->handle = 0;
    }
    if (handle != 0 && m_sync_services.close_handle) {
        m_sync_services.close_handle(handle);
    }
    chain.latency.reset();
}

u32 D3D11Manager::write_frame_statistics(const SwapChain& chain,
                                         GuestAddr stats) {
    // DXGI_FRAME_STATISTICS: three UINTs, then two LARGE_INTEGERs at 16 and
    // 24 on both bitnesses (8-byte aligned). Measured: the structure is
    // written (zeroed) even when the call fails.
    const std::array<u8, 32> zero{};
    if (m_memory.write_bytes(stats, zero.data(), zero.size()).is_err()) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (!dxgi_flip_effect(chain.desc.swap_effect)) {
        // Learn: no statistics for a windowed bitblt chain (measured
        // DXGI_ERROR_UNSUPPORTED).
        return d3d11::DXGI_ERROR_UNSUPPORTED;
    }
    if (chain.present_count == 0 || chain.window_missing) {
        // Nothing has reached the screen: the statistics are disjoint.
        return d3d11::DXGI_ERROR_FRAME_STATISTICS_DISJOINT;
    }
    // Frames are composited at the next vertical blank after Present; the
    // vblank grid is the display's refresh on the virtual QPC timeline, the
    // same one DwmGetCompositionTimingInfo reports.
    const u64 period = qpc_refresh_period();
    const u64 now = virtual_qpc_ticks();
    const u64 sync_refresh = now / period;
    const u64 present_refresh = std::min(
        sync_refresh, (chain.last_present_qpc + period - 1) / period);
    const bool written =
        m_memory.write_u32(stats + 0, chain.present_count).is_ok() &&
        m_memory.write_u32(stats + 4, static_cast<u32>(present_refresh))
            .is_ok() &&
        m_memory.write_u32(stats + 8, static_cast<u32>(sync_refresh))
            .is_ok() &&
        m_memory.write_u64(stats + 16, sync_refresh * period).is_ok();
    return written ? d3d11::S_OK : DXGI_ERROR_INVALID_CALL;
}

u64 D3D11Manager::qpc_refresh_period() const {
    double hz = 0.0;
    for (const auto& output : m_dxgi.output_infos()) {
        hz = std::max(hz, output.refresh_rate_hz);
    }
    if (!std::isfinite(hz) || hz < 1.0) hz = 60.0;
    const u64 period = static_cast<u64>(static_cast<double>(kQpcHz) / hz);
    return period == 0 ? 1 : period;
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_media.cpp
/// @brief The IDXGISwapChainMedia facet vtable
///        (impl_SwapChainMediaMethod).
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

void D3D11Manager::impl_SwapChainMediaMethod(Marshaler& m, usize slot) {
    // IDXGISwapChainMedia is a facet of the chain: one reference count and
    // one IUnknown identity (the chain's).
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr chain_address =
        self >= kSwapChainMediaFacet ? self - kSwapChainMediaFacet : 0;
    auto chain = m_swap_chains.find(chain_address);
    if (chain == m_swap_chains.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid)) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        GuestAddr result = 0;
        if (guid_equal(iid, IID_IDXGISWAPCHAINMEDIA)) {
            result = self;
        } else if (guid_equal(iid, IID_IUNKNOWN) ||
                   guid_equal(iid, IID_IDXGIOBJECT) ||
                   guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) ||
                   guid_equal(iid, IID_IDXGISWAPCHAIN) ||
                   guid_equal(iid, IID_IDXGISWAPCHAIN1) ||
                   guid_equal(iid, IID_IDXGISWAPCHAIN2) ||
                   guid_equal(iid, IID_IDXGISWAPCHAIN3) ||
                   guid_equal(iid, IID_IDXGISWAPCHAIN4)) {
            result = chain_address;
        }
        if (result == 0) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        ++chain->second.ref_count;
        (void)m.write_ptr(object_out, result);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(++chain->second.ref_count);
        return;
    case 2:
        m.set_return(release_swap_chain(chain_address));
        return;
    case 3: {
        // GetFrameStatisticsMedia(DXGI_FRAME_STATISTICS_MEDIA*): the frame
        // statistics (same rules as IDXGISwapChain::GetFrameStatistics) plus
        // CompositionMode COMPOSED (every Vitreo frame is composited by Core
        // Animation) and ApprovedPresentDuration 0 (no custom duration).
        const GuestAddr stats = m.arg_ptr(1);
        if (stats == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const u32 result = write_frame_statistics(chain->second, stats);
        (void)m_memory.write_u32(stats + 32, 0);  // _COMPOSED
        (void)m_memory.write_u32(stats + 36, 0);
        m.set_return(result);
        return;
    }
    case 4: {
        // SetPresentDuration(Duration): 0 is "no custom duration". The
        // presenter follows the display's own refresh, so
        // CheckPresentDurationSupport offers no other duration and asking
        // for one is refused.
        const u32 duration = static_cast<u32>(m.arg(1));
        m.set_return(duration == 0 ? d3d11::S_OK : DXGI_ERROR_INVALID_CALL);
        return;
    }
    case 5: {
        // CheckPresentDurationSupport(Desired, pSmaller, pLarger): Learn --
        // an output without custom refresh rates reports (0, 0).
        const GuestAddr smaller = m.arg_ptr(2);
        const GuestAddr larger = m.arg_ptr(3);
        if (smaller == 0 || larger == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m_memory.write_u32(smaller, 0);
        (void)m_memory.write_u32(larger, 0);
        m.set_return(d3d11::S_OK);
        return;
    }
    default:
        m.set_return(d3d11::E_NOTIMPL);
        return;
    }
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_swapchain_methods.cpp
/// @brief The IDXGISwapChain .. IDXGISwapChain4 vtable dispatch
///        (impl_SwapChainMethod).
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

void D3D11Manager::impl_SwapChainMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto swap_chain = m_swap_chains.find(self);
    if (swap_chain == m_swap_chains.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    SwapChain& chain = swap_chain->second;
    const bool flip = dxgi_flip_effect(chain.desc.swap_effect);
    const bool waitable =
        (chain.desc.flags & kSwapChainFlagFrameLatencyWaitableObject) != 0;
    switch (slot) {
    case 0: {
        const GuestAddr object_out = m.arg_ptr(2);
        if (object_out == 0) {
            m.set_return(d3d11::E_POINTER);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        // The chain implements IDXGISwapChain4 and every interface below it
        // (measured: IDXGISwapChain2/3/4 answer on bitblt and flip chains).
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_IDXGIOBJECT) &&
             !guid_equal(iid, IID_IDXGIDEVICESUBOBJECT) &&
             !guid_equal(iid, IID_IDXGISWAPCHAIN) &&
             !guid_equal(iid, IID_IDXGISWAPCHAIN1) &&
             !guid_equal(iid, IID_IDXGISWAPCHAIN2) &&
             !guid_equal(iid, IID_IDXGISWAPCHAIN3) &&
             !guid_equal(iid, IID_IDXGISWAPCHAIN4) &&
             !guid_equal(iid, IID_IDXGISWAPCHAINMEDIA))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        const GuestAddr result = guid_equal(iid, IID_IDXGISWAPCHAINMEDIA)
            ? self + kSwapChainMediaFacet
            : self;
        ++chain.ref_count;
        if (m.write_ptr(object_out, result).is_err()) {
            --chain.ref_count;
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 1:
        m.set_return(++chain.ref_count);
        return;
    case 2:
        m.set_return(release_swap_chain(self));
        return;
    case 3: {
        const u32 result = set_private_data(m, chain.private_data);
        d3d11_diag("IDXGISwapChain::SetPrivateData chain=0x%llx -> 0x%x",
                   static_cast<unsigned long long>(self), result);
        m.set_return(result);
        return;
    }
    case 4:
        // SetPrivateDataInterface: the IDXGIObject contract every object
        // here follows (set_private_data_interface).
        m.set_return(set_private_data_interface(m, chain.private_data));
        return;
    case 5: {
        const u32 result = get_private_data(m, chain.private_data);
        d3d11_diag("IDXGISwapChain::GetPrivateData chain=0x%llx -> 0x%x",
                   static_cast<unsigned long long>(self), result);
        m.set_return(result);
        return;
    }
    case 6: {
        // GetParent(riid, ppParent): the factory that created the chain
        // (measured: the very factory, and IDXGIAdapter is E_NOINTERFACE).
        const GuestAddr parent_out = m.arg_ptr(2);
        if (parent_out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        if (chain.factory == 0) {
            (void)m.write_ptr(parent_out, 0);
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        m.set_return(m_dxgi.query_object_interface(
            chain.factory, m.arg_ptr(1), parent_out));
        return;
    }
    case 7: {
        auto device = m_devices.find(chain.device_owner);
        if (device == m_devices.end()) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        m.set_return(query_interface(m, device->second.device_interface));
        return;
    }
    case 8: {
        const u32 sync_interval = static_cast<u32>(m.arg(1));
        const u32 flags = static_cast<u32>(m.arg(2));
        const u32 result = present_swap_chain(self, sync_interval, flags);
        d3d11_diag(
            "IDXGISwapChain::Present chain=0x%llx sync=%u flags=0x%x "
            "-> 0x%08x count=%u",
            static_cast<unsigned long long>(self), sync_interval, flags,
            result, m_swap_chains.at(self).present_count);
        m.set_return(result);
        return;
    }
    case 9: {
        // GetBuffer(Buffer, riid, ppSurface). Measured: a NULL ppSurface is
        // INVALID_CALL; an index past the chain's readable buffers is
        // INVALID_CALL (DISCARD / FLIP_DISCARD expose only buffer 0; the
        // sequential effects expose 1..BufferCount-1 read-only); an IID the
        // texture does not implement is E_NOINTERFACE. *ppSurface is NULLed
        // on every failure after the pointer check.
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr object_out = m.arg_ptr(3);
        if (object_out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m.write_ptr(object_out, 0);
        if (chain.buffers_lost ||
            (index != 0 && (!dxgi_sequential_effect(chain.desc.swap_effect) ||
                            index >= chain.desc.buffer_count))) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const GuestAddr texture_address =
            index == 0 ? chain.back_buffer : swap_chain_index_buffer(self, index);
        auto texture = m_textures.find(texture_address);
        GuidBytes iid{};
        if (texture == m_textures.end() || !read_guid(m.arg_ptr(2), iid)) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const GuestAddr result = texture_interface_for(texture_address, iid);
        if (result == 0) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        // The application's reference counts on the chain (retain_object).
        (void)retain_object(texture_address);
        if (m.write_ptr(object_out, result).is_err()) {
            (void)release_object(texture_address);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 10: {
        // SetFullscreenState(Fullscreen, pTarget). Learn documents S_OK,
        // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE and
        // DXGI_STATUS_MODE_CHANGE_IN_PROGRESS; a FRAME_LATENCY_WAITABLE_OBJECT
        // chain refuses the call outright (measured, TRUE and FALSE).
        const bool fullscreen = m.arg(1) != 0;
        const GuestAddr target = m.arg_ptr(2);
        if (waitable || chain.composition) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        // "DXGI_STATUS_MODE_CHANGE_IN_PROGRESS is returned if a
        // fullscreen/windowed mode transition is occurring when this API is
        // called" (from a handler of the transition's own messages, or
        // another thread).
        if (m_fullscreen_transition) {
            m.set_return(d3d11::DXGI_STATUS_MODE_CHANGE_IN_PROGRESS);
            return;
        }
        relinquish_fullscreen_if_inactive(self);
        if (fullscreen) {
            const u32 result = enter_exclusive_fullscreen(self, target);
            d3d11_diag("IDXGISwapChain::SetFullscreenState(TRUE, 0x%llx) "
                       "chain=0x%llx -> 0x%08x",
                       static_cast<unsigned long long>(target),
                       static_cast<unsigned long long>(self), result);
            m.set_return(result);
            return;
        }
        // Learn: with FALSE pTarget "must" be NULL; measured
        // (dxgi_exclusive_fullscreen_probe) an output is accepted, S_OK.
        //
        // The guest goes windowed either way. The host Space is left only
        // when the application is foreground: a FALSE issued while it is not
        // (a guest answering its own fullscreen-lost with a windowed reset)
        // must not pull the window out of the Space the user Cmd-Tabbed
        // away from -- that leaves a black window covering the other app.
        // The Space is reused by the guest's re-entry on return.
        {
            const auto& active = m_window_services.application_active;
            leave_exclusive_fullscreen(self, !active || active());
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 11: {
        // GetFullscreenState(pFullscreen, ppTarget), both optional: TRUE and
        // the output (its reference count incremented) while full-screen,
        // else FALSE and NULL.
        const GuestAddr fullscreen_out = m.arg_ptr(1);
        const GuestAddr output_out = m.arg_ptr(2);
        relinquish_fullscreen_if_inactive(self);
        auto current = m_swap_chains.find(self);
        if (current == m_swap_chains.end()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const bool is_fullscreen = current->second.fullscreen;
        const GuestAddr output = current->second.fullscreen_output;
        if (fullscreen_out != 0) {
            (void)m.write_u32(fullscreen_out, is_fullscreen ? 1u : 0u);
        }
        if (output_out != 0) {
            const bool hand_out =
                is_fullscreen && output != 0 && m_dxgi.retain_object(output);
            if (m.write_ptr(output_out, hand_out ? output : 0).is_err() &&
                hand_out) {
                m_dxgi.release_dxgi_object(output);
            }
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 12:
        // GetDesc(NULL) is E_INVALIDARG (measured).
        m.set_return(write_swap_chain_desc(m.arg_ptr(1), chain.desc)
                         ? d3d11::S_OK
                         : d3d11::E_INVALIDARG);
        return;
    case 13: {
        // ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags)
        const u32 result = resize_swap_chain_buffers(
            self, static_cast<u32>(m.arg(1)), static_cast<u32>(m.arg(2)),
            static_cast<u32>(m.arg(3)), static_cast<u32>(m.arg(4)),
            static_cast<u32>(m.arg(5)));
        m.set_return(result);
        return;
    }
    case 14: {
        // ResizeTarget(const DXGI_MODE_DESC*): a windowed chain resizes its
        // window so the CLIENT area is Width x Height (measured 320x240), the
        // window system clamping what it must (0x0 gave the minimum size).
        const GuestAddr mode = m.arg_ptr(1);
        if (mode == 0 || chain.composition || chain.window_missing) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const auto width = m_memory.read_u32(mode + 0);
        const auto height = m_memory.read_u32(mode + 4);
        if (width.is_err() || height.is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        // Learn ResizeTarget: DXGI_STATUS_MODE_CHANGE_IN_PROGRESS during a
        // full-screen / windowed transition.
        if (m_fullscreen_transition) {
            m.set_return(d3d11::DXGI_STATUS_MODE_CHANGE_IN_PROGRESS);
            return;
        }
        const u64 output_window = chain.desc.output_window;
        relinquish_fullscreen_if_inactive(self);
        if (const auto current = m_swap_chains.find(self);
            current != m_swap_chains.end() && current->second.fullscreen) {
            // "Changes the display mode on the target output when the swap
            // chain is in full-screen mode."
            m.set_return(resize_exclusive_target(self, width.value(),
                                                 height.value()));
            return;
        }
        if (!m_window_services.resize_client) {
            DXGI_UNIMPLEMENTED_ONCE(
                "dxgi!IDXGISwapChain::ResizeTarget: no window services -> "
                "DXGI_ERROR_INVALID_CALL");
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const bool resized = m_window_services.resize_client(
            output_window, width.value(), height.value());
        d3d11_diag("IDXGISwapChain::ResizeTarget chain=0x%llx %ux%u -> %s",
                   static_cast<unsigned long long>(self), width.value(),
                   height.value(), resized ? "ok" : "failed");
        m.set_return(resized ? d3d11::S_OK : DXGI_ERROR_INVALID_CALL);
        return;
    }
    case 15: {
        // GetContainingOutput(ppOutput): the output holding most of the
        // window (Learn). NULL is INVALID_CALL; with no output the result is
        // DXGI_ERROR_UNSUPPORTED and *ppOutput NULL (measured on an adapter
        // without outputs).
        const GuestAddr output_out = m.arg_ptr(1);
        if (output_out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m.write_ptr(output_out, 0);
        auto device = m_devices.find(chain.device_owner);
        std::optional<std::array<i32, 4>> rect;
        if (!chain.composition && !chain.window_missing &&
            m_window_services.window_rect) {
            rect = m_window_services.window_rect(chain.desc.output_window);
        }
        const GuestAddr output =
            device != m_devices.end() && rect
                ? m_dxgi.create_containing_output(device->second.adapter,
                                                  (*rect)[0], (*rect)[1],
                                                  (*rect)[2], (*rect)[3])
                : 0;
        if (output == 0) {
            m.set_return(d3d11::DXGI_ERROR_UNSUPPORTED);
            return;
        }
        if (m.write_ptr(output_out, output).is_err()) {
            m_dxgi.release_dxgi_object(output);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 16: {
        // GetFrameStatistics(pStats). NULL is INVALID_CALL. Learn: a windowed
        // bitblt chain has no statistics (measured DXGI_ERROR_UNSUPPORTED,
        // the structure zeroed); a flip chain reports them.
        const GuestAddr stats = m.arg_ptr(1);
        if (stats == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(write_frame_statistics(chain, stats));
        return;
    }
    case 17: {
        const GuestAddr count_out = m.arg_ptr(1);
        if (count_out == 0 ||
            m.write_u32(count_out, chain.present_count).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 18: {
        // Learn IDXGISwapChain1::GetDesc1 → DXGI_SWAP_CHAIN_DESC1: the DESC1
        // Scaling and AlphaMode as created (a legacy chain: STRETCH, IGNORE).
        const GuestAddr address = m.arg_ptr(1);
        if (address == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const auto& desc = chain.desc;
        const auto write32 = [&](usize offset, u32 value) {
            return m_memory.write_u32(address + offset, value).is_ok();
        };
        if (!(write32(0, desc.width) && write32(4, desc.height) &&
              write32(8, desc.format) && write32(12, 0) && // Stereo
              write32(16, desc.sample_count) &&
              write32(20, desc.sample_quality) &&
              write32(24, desc.buffer_usage) &&
              write32(28, desc.buffer_count) &&
              write32(32, desc.desc1_scaling) &&
              write32(36, desc.swap_effect) &&
              write32(40, desc.alpha_mode) &&
              write32(44, desc.flags))) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 19: {
        // GetFullscreenDesc
        const GuestAddr address = m.arg_ptr(1);
        if (address == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const auto& desc = chain.desc;
        if (m_memory.write_u32(address + 0, desc.refresh_numerator).is_err() ||
            m_memory.write_u32(address + 4, desc.refresh_denominator)
                .is_err() ||
            m_memory.write_u32(address + 8, desc.scanline_ordering).is_err() ||
            m_memory.write_u32(address + 12, desc.scaling).is_err() ||
            m_memory.write_u32(address + 16, chain.fullscreen ? 0u : 1u)
                .is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 20: {
        // GetHwnd: a composition chain has none -> INVALID_CALL with the
        // HWND NULLed (measured).
        const GuestAddr hwnd_out = m.arg_ptr(1);
        if (hwnd_out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        if (chain.composition) {
            (void)m.write_ptr(hwnd_out, 0);
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(m.write_ptr(hwnd_out, chain.desc.output_window).is_ok()
                         ? d3d11::S_OK
                         : DXGI_ERROR_INVALID_CALL);
        return;
    }
    case 21: {
        // GetCoreWindow(refiid, ppUnk): an HWND or composition chain has no
        // CoreWindow -> INVALID_CALL with *ppUnk NULLed (measured).
        const GuestAddr unknown_out = m.arg_ptr(2);
        if (unknown_out != 0) (void)m.write_ptr(unknown_out, 0);
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    case 22: {
        // Present1(SyncInterval, PresentFlags, pPresentParameters).
        const u32 sync_interval = static_cast<u32>(m.arg(1));
        const u32 flags = static_cast<u32>(m.arg(2));
        const u32 params_ok = validate_present_parameters(chain, m.arg_ptr(3));
        const u32 result = params_ok != d3d11::S_OK
            ? params_ok
            : present_swap_chain(self, sync_interval, flags);
        d3d11_diag(
            "IDXGISwapChain1::Present1 chain=0x%llx sync=%u flags=0x%x "
            "-> 0x%08x count=%u",
            static_cast<unsigned long long>(self), sync_interval, flags,
            result, m_swap_chains.at(self).present_count);
        m.set_return(result);
        return;
    }
    case 23:
        // IsTemporaryMonoSupported: no stereo chains exist.
        m.set_return(0);
        return;
    case 24: {
        // GetRestrictToOutput: the output given at creation, AddRef'd.
        const GuestAddr output_out = m.arg_ptr(1);
        if (output_out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        GuestAddr output = chain.restrict_output;
        if (output != 0 && !m_dxgi.retain_object(output)) output = 0;
        (void)m.write_ptr(output_out, output);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 25: {
        // SetBackgroundColor(const DXGI_RGBA*): NULL is INVALID_CALL.
        const GuestAddr color = m.arg_ptr(1);
        std::array<float, 4> value{};
        if (color == 0 ||
            m_memory.read_bytes(color, reinterpret_cast<Byte*>(value.data()),
                                sizeof(value)).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.background_color = value;
        apply_swap_chain_presentation_state(chain);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 26: {
        const GuestAddr color = m.arg_ptr(1);
        if (color == 0 ||
            m_memory.write_bytes(
                color,
                reinterpret_cast<const Byte*>(chain.background_color.data()),
                sizeof(chain.background_color)).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 27: {
        // SetRotation(DXGI_MODE_ROTATION): flip-model chains only; values
        // IDENTITY..ROTATE270 (UNSPECIFIED and out-of-range values are
        // INVALID_CALL, measured).
        const u32 rotation = static_cast<u32>(m.arg(1));
        if (!flip || rotation < kDxgiRotationIdentity ||
            rotation > kDxgiRotationRotate270) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.rotation = rotation;
        apply_swap_chain_presentation_state(chain);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 28: {
        const GuestAddr rotation_out = m.arg_ptr(1);
        if (!flip || rotation_out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(m.write_u32(rotation_out, chain.rotation).is_ok()
                         ? d3d11::S_OK
                         : DXGI_ERROR_INVALID_CALL);
        return;
    }
    case 29: {
        // IDXGISwapChain2::SetSourceSize(Width, Height). Flip-model and
        // composition chains; neither may exceed the buffer (INVALID_CALL,
        // measured; zero is accepted and presents nothing).
        const u32 width = static_cast<u32>(m.arg(1));
        const u32 height = static_cast<u32>(m.arg(2));
        if (!flip || width > chain.desc.width || height > chain.desc.height) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.source_width = width;
        chain.source_height = height;
        m.set_return(d3d11::S_OK);
        return;
    }
    case 30: {
        const GuestAddr width_out = m.arg_ptr(1);
        const GuestAddr height_out = m.arg_ptr(2);
        if (width_out == 0 || height_out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m.write_u32(width_out, chain.source_width);
        (void)m.write_u32(height_out, chain.source_height);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 31: {
        // SetMaximumFrameLatency(MaxLatency): FRAME_LATENCY_WAITABLE_OBJECT
        // chains only; 0 and values past the limit are INVALID_CALL
        // (measured). Raising the latency frees that many more frames on the
        // waitable object at once (measured: 1 -> 3 made two more waits
        // succeed); lowering it frees none.
        const u32 latency = static_cast<u32>(m.arg(1));
        if (!waitable || latency == 0 || latency > kDxgiMaxFrameLatency) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        const u32 previous = chain.maximum_frame_latency;
        chain.maximum_frame_latency = latency;
        if (chain.latency) {
            std::lock_guard<std::mutex> lock(chain.latency->mutex);
            chain.latency->cap = latency;
            if (latency > previous && m_sync_services.release_semaphore) {
                m_sync_services.release_semaphore(
                    chain.latency->handle,
                    static_cast<i32>(latency - previous),
                    static_cast<i32>(latency));
            }
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 32: {
        const GuestAddr latency_out = m.arg_ptr(1);
        if (!waitable || latency_out == 0) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        (void)m.write_u32(latency_out, chain.maximum_frame_latency);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 33: {
        // GetFrameLatencyWaitableObject(): NULL unless the chain was created
        // with FRAME_LATENCY_WAITABLE_OBJECT; otherwise a NEW handle each call
        // (measured: two calls differ) to one semaphore that holds the
        // latency's worth of frames and gains one per retired Present.
        if (!waitable || !ensure_swap_chain_latency_object(chain)) {
            m.set_return(0);
            return;
        }
        const u64 handle = m_sync_services.duplicate_handle
            ? m_sync_services.duplicate_handle(chain.latency->handle)
            : 0;
        m.set_return(handle);
        return;
    }
    case 34: {
        // SetMatrixTransform(const DXGI_MATRIX_3X2_F*): composition chains
        // only; a NULL matrix or one with rotation / skew terms is
        // INVALID_CALL and leaves the transform unchanged (measured; Learn
        // says E_INVALIDARG).
        const GuestAddr matrix = m.arg_ptr(1);
        std::array<float, 6> value{};
        if (!chain.composition || matrix == 0 ||
            m_memory.read_bytes(matrix, reinterpret_cast<Byte*>(value.data()),
                                sizeof(value)).is_err() ||
            value[1] != 0.0f || value[2] != 0.0f) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        chain.matrix_transform = value;
        (void)apply_composition_layer(chain);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 35: {
        const GuestAddr matrix = m.arg_ptr(1);
        if (!chain.composition || matrix == 0 ||
            m_memory.write_bytes(
                matrix,
                reinterpret_cast<const Byte*>(chain.matrix_transform.data()),
                sizeof(chain.matrix_transform)).is_err()) {
            m.set_return(DXGI_ERROR_INVALID_CALL);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    case 36:
        // IDXGISwapChain3::GetCurrentBackBufferIndex
        m.set_return(chain.current_back_buffer_index);
        return;
    case 37: {
        // CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport): NULL is
        // E_INVALIDARG; support is DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG
        // _PRESENT for a colour space the presenter shows correctly.
        const u32 color_space = static_cast<u32>(m.arg(1));
        const GuestAddr support_out = m.arg_ptr(2);
        if (support_out == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const bool supported = swap_chain_color_space_supported(chain,
                                                                color_space);
        m.set_return(m.write_u32(support_out, supported ? 1u : 0u).is_ok()
                         ? d3d11::S_OK
                         : d3d11::E_INVALIDARG);
        return;
    }
    case 38: {
        // SetColorSpace1(ColorSpace): E_INVALIDARG unless supported.
        const u32 color_space = static_cast<u32>(m.arg(1));
        if (!swap_chain_color_space_supported(chain, color_space)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        chain.color_space = color_space;
        apply_swap_chain_presentation_state(chain);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 39:
        // ResizeBuffers1 exists for D3D12 command-queue chains; a D3D11 chain
        // answers INVALID_CALL whatever the arguments (measured).
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    case 40: {
        // IDXGISwapChain4::SetHDRMetaData(Type, Size, pMetaData). NONE clears
        // it; HDR10 needs exactly a DXGI_HDR_METADATA_HDR10; a wrong size, a
        // NULL pointer or an unknown type is E_INVALIDARG (measured).
        const u32 type = static_cast<u32>(m.arg(1));
        const u32 size = static_cast<u32>(m.arg(2));
        const GuestAddr data = m.arg_ptr(3);
        if (type == kDxgiHdrMetadataNone) {
            chain.hdr_metadata_type = type;
            chain.hdr_metadata.clear();
            if (chain.backend) (void)chain.backend->set_hdr10_metadata({});
            m.set_return(d3d11::S_OK);
            return;
        }
        const u32 expected = type == kDxgiHdrMetadataHdr10
            ? kDxgiHdrMetadataHdr10Size
            : type == kDxgiHdrMetadataHdr10Plus ? kDxgiHdrMetadataHdr10PlusSize
                                                : 0u;
        if (expected == 0 || size != expected || data == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        std::vector<u8> bytes(size);
        if (m_memory.read_bytes(data, bytes.data(), bytes.size()).is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        chain.hdr_metadata_type = type;
        chain.hdr_metadata = bytes;
        if (type == kDxgiHdrMetadataHdr10) {
            apply_swap_chain_presentation_state(chain);
        } else {
            DXGI_UNIMPLEMENTED_ONCE(
                "dxgi!IDXGISwapChain4::SetHDRMetaData: HDR10+ dynamic "
                "metadata is stored but not passed to the display");
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    default:
        unsupported(m, "IDXGISwapChain", slot);
        return;
    }
}

} // namespace vitreo::winapi

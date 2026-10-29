// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_resources.cpp
/// @brief IDirect3DSwapChain9, IDirect3DSurface9, IDirect3DTexture9,
///        IDirect3DVertexBuffer9, and IDirect3DIndexBuffer9.
///
/// Locking is the contract that matters most here: D3D9 hands the application
/// a raw pointer into resource memory. Vitreo maps guest-visible memory for
/// the resource's whole lifetime and pushes the locked range into the shared
/// backend on unlock, so pixels and vertices the guest writes are the ones the
/// GPU receives.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_frame_dump.hpp"

#include "core/memory/guest_memory.hpp"
#include <vitreo/diagnostics.hpp>
#include "winapi/core/host_gui_pump.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/// @file idirect3dswapchain9.cpp
/// @brief IDirect3DSwapChain9
///
/// Closed cut from d3d9_resources.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// IDirect3DSwapChain9
// =============================================================================

void D3D9Manager::dispatch_swap_chain(Marshaler& m, usize slot, u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::SwapChain)) return;

    ComObject* entry = find(self, D3D9InterfaceId::SwapChain);
    ComObject* owner = entry != nullptr
                           ? find(entry->device, D3D9InterfaceId::Device)
                           : nullptr;
    if (owner == nullptr || !owner->device_state) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    DeviceState& state = *owner->device_state;
    SwapChainState* additional =
        entry->swap_chain_state ? entry->swap_chain_state.get() : nullptr;

    // Implicit chain (no SwapChainState) presents through DeviceState; chains
    // from CreateAdditionalSwapChain own their backend + back buffer.
    graphics::BackendSwapChain* backend =
        additional != nullptr ? additional->swap_chain.get()
                              : state.swap_chain.get();
    const u32 back_buffer = additional != nullptr
                                ? additional->back_buffer_surface
                                : state.back_buffer_surface;
    const auto& present_parameters =
        additional != nullptr ? additional->present_parameters
                              : state.present_parameters;
    const u32 present_interval =
        additional != nullptr
            ? additional->present_field(PP_PRESENTATION_INTERVAL)
            : state.present_field(PP_PRESENTATION_INTERVAL);

    switch (slot) {
    case 3: { // Present
        // Same contract as IDirect3DDevice9::Present: refuse while inside a
        // scene, flush the active pass, then flip. Callers GetSwapChain(0)
        // and present through this path. Skipping the flush left draws
        // stranded in an uncommitted pass, so the drawable never showed new
        // frames after the device Present path was no longer used.
        if (state.lost) {
            VITREO_TRACE("[d3d9] IDirect3DSwapChain9::Present -> DEVICELOST "
                         "(device already lost)\n");
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        if (state.in_scene) {
            VITREO_TRACE("[d3d9] IDirect3DSwapChain9::Present -> INVALIDCALL "
                         "(inside BeginScene/EndScene)\n");
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (backend == nullptr || !backend->valid()) {
            state.lost = true;
            VITREO_TRACE("[d3d9] IDirect3DSwapChain9::Present -> DEVICELOST "
                         "(no swap chain)\n");
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        const auto present_start = std::chrono::steady_clock::now();
        flush_render_pass(state);
        if (d3d9_frame_dump_pending()) d3d9_frame_dump_write(*backend);
        const auto submit_start = std::chrono::steady_clock::now();
        // Same throttle as IDirect3DDevice9::Present; see
        // effective_present_sync in d3d9_device_present.cpp for why
        // IMMEDIATE is honoured as INTERVAL_ONE.
        constexpr u32 kImmediate = 0x80000000u;
        static const bool unthrottled =
            std::getenv("VITREO_D3D9_UNTHROTTLED") != nullptr;
        const u32 sync = (present_interval == kImmediate && unthrottled) ? 0u : 1u;
        // Same reasoning as IDirect3DDevice9::Present: drop the manager lock
        // across the blocking submit so other guest threads are not queued
        // behind a sleeping owner.
        bool present_ok = false;
        {
            D3D9Manager::UnlockDuringBlockingCall unlocked;
            present_ok = backend->present(sync, 0);
        }
        if (!present_ok) {
            state.lost = true;
            VITREO_TRACE("[d3d9] IDirect3DSwapChain9::Present failed; device "
                         "0x%08x is lost\n",
                         entry->device);
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        static const bool perf_diag =
            std::getenv("VITREO_GRAPHICS_PERF_DIAG") != nullptr;
        if (perf_diag) {
            const auto present_us = std::chrono::duration_cast<
                std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                          present_start)
                                        .count();
            if (present_us >= 25000) {
                std::fprintf(
                    stderr,
                    "[graphics-perf] d3d9 swapchain present %.3f ms\n",
                    static_cast<double>(present_us) / 1000.0);
            }
        }
        static u32 swap_present_count = 0;
        ++swap_present_count;
        if (swap_present_count == 1 || (swap_present_count & 0xFFu) == 0) {
            VITREO_TRACE("[d3d9] SwapChain Present ok #%u device=0x%08x\n",
                         swap_present_count, entry->device);
        }
        diagnostics::phase_note_present();
        {
            const auto now = std::chrono::steady_clock::now();
            const u64 ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - present_start)
                    .count());
            const u64 submit_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - submit_start)
                    .count());
            diagnostics::phase_add_present_wall(ns);
            diagnostics::phase_add_present_submit_wall(submit_ns);
            diagnostics::perf_add_graphics_ns(ns);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 4: // GetFrontBufferData
        // Learn: IDirect3DSwapChain9::GetFrontBufferData(pDestSurface).
        // Same helper as IDirect3DDevice9::GetFrontBufferData; this chain's
        // presentable back buffer is the readback source.
        get_front_buffer_data(m, state, backend, back_buffer,
                              static_cast<u32>(m.arg(1)));
        return;
    case 5: { // GetBackBuffer
        const u32 index = static_cast<u32>(m.arg(1));
        const u32 type = static_cast<u32>(m.arg(2));
        const GuestAddr out = m.arg_ptr(3);
        if (out != 0) (void)m.write_u32(out, 0);
        if (index != 0 || type != D3DBACKBUFFER_TYPE_MONO || out == 0 ||
            back_buffer == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(back_buffer);
        if (m.write_u32(out, back_buffer).is_err()) {
            (void)release(back_buffer);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 6: // GetRasterStatus
        // Learn: IDirect3DSwapChain9::GetRasterStatus(pRasterStatus).
        get_raster_status(m, 0, m.arg_ptr(1));
        return;
    case 7: { // GetDisplayMode
        const GuestAddr out = m.arg_ptr(1);
        std::array<u32, 4> mode{};
        if (out == 0 || !current_display_mode(mode) ||
            m.memory().write_bytes(
                out, reinterpret_cast<const Byte*>(mode.data()),
                D3DDISPLAYMODE_SIZE).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 8: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(entry->device);
        if (m.write_u32(out, entry->device).is_err()) {
            (void)release(entry->device);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 9: { // GetPresentParameters
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 ||
            m.memory().write_bytes(
                out, present_parameters.data(),
                present_parameters.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi

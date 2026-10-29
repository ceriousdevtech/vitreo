// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device_present.cpp
/// @brief IDirect3DDevice9 swap chains, Reset and Present.
///
/// Vtable slots 13-18 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_frame_dump.hpp"
#include "winapi/graphics/d3d9/d3d9_device_internal.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

namespace vitreo::winapi {

namespace {

// D3DPRESENT_INTERVAL_IMMEDIATE asks to present without waiting for vblank.
// On real hardware that is nearly free: the flip is queued and the caller
// returns. Vitreo's present is not free -- each one stages a full-screen copy
// of the back buffer and blits it to a drawable -- so an IMMEDIATE loop runs
// as fast as the GPU can absorb those copies and blocks the guest thread on
// backpressure for the difference.
//
// Measured: 168,523 presents in 195s (~850/s) costing 90s,
// which was 62% of all Vitreo work and ~46% of the render thread's wall. On a
// 60Hz display roughly fourteen of every fifteen of those frames are never
// shown, so the work is invisible by construction, and the core it burns is
// one the loader needs.
//
// Honouring IMMEDIATE as INTERVAL_ONE trades a frame rate nobody can see for
// that core. It reuses the vsync path that already exists and is exercised by
// every INTERVAL_ONE title, rather than adding a new present-skip -- skipping
// was tried before and flickered, because it dropped Draw/Clear with the frame.
// Set VITREO_D3D9_UNTHROTTLED=1 to get the old behaviour (benchmarks, or any
// title that genuinely wants uncapped presentation).
[[nodiscard]] u32 effective_present_sync(u32 interval) noexcept {
    constexpr u32 kImmediate = 0x80000000u;
    if (interval != kImmediate) return 1u;
    static const bool unthrottled =
        std::getenv("VITREO_D3D9_UNTHROTTLED") != nullptr;
    return unthrottled ? 0u : 1u;
}

} // namespace

/// IDirect3DDevice9 vtable slots: CreateAdditionalSwapChain, GetSwapChain,
/// GetNumberOfSwapChains, Reset, Present and GetBackBuffer.
void D3D9Manager::dispatch_device_swap_chain(Marshaler& m, usize slot,
                                             u32 self, DeviceState& state) {
    switch (slot) {
    case 13: { // CreateAdditionalSwapChain
        // Learn: windowed multi-view chains only; one fullscreen chain per
        // device (the implicit one). pPresentationParameters is in/out.
        const GuestAddr parameters_in = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        if (parameters_in == 0 || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.lost) {
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        if (state.present_field(PP_WINDOWED) == 0) {
            VITREO_TRACE("[d3d9] CreateAdditionalSwapChain -> INVALIDCALL "
                         "(device is exclusive fullscreen)\n");
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m_backend == nullptr || !m_backend->available()) {
            m.set_return(D3DERR_NOTAVAILABLE);
            return;
        }

        std::array<u8, D3DPRESENT_PARAMETERS_SIZE> parameters{};
        if (m.memory().read_bytes(parameters_in, parameters.data(),
                                  parameters.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const auto read_pp = [&](usize offset) -> u32 {
            u32 value = 0;
            std::memcpy(&value, parameters.data() + offset, sizeof(value));
            return value;
        };
        const auto write_pp = [&](usize offset, u32 value) {
            std::memcpy(parameters.data() + offset, &value, sizeof(value));
        };

        if (read_pp(PP_WINDOWED) == 0) {
            // Learn: any given device can support only one full-screen chain.
            VITREO_TRACE("[d3d9] CreateAdditionalSwapChain -> INVALIDCALL "
                         "(additional chain must be windowed)\n");
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }

        u32 width = read_pp(PP_BACK_BUFFER_WIDTH);
        u32 height = read_pp(PP_BACK_BUFFER_HEIGHT);
        u32 format = read_pp(PP_BACK_BUFFER_FORMAT);
        u32 window = read_pp(PP_DEVICE_WINDOW);
        if (window == 0) window = state.focus_window;
        resolve_present_back_buffer(true, window, width, height, format);
        if (width == 0 || height == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const graphics::PixelFormat backend_format =
            d3d9_backend_format(format);
        if (backend_format == graphics::PixelFormat::Unknown) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }

        if (window == 0 || !m_presentation_view_resolver) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        void* native_view = m_presentation_view_resolver(window);
        if (native_view == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }

        u32 buffer_count = read_pp(PP_BACK_BUFFER_COUNT);
        if (buffer_count == 0) buffer_count = 1;
        write_pp(PP_BACK_BUFFER_WIDTH, width);
        write_pp(PP_BACK_BUFFER_HEIGHT, height);
        write_pp(PP_BACK_BUFFER_FORMAT, format);
        write_pp(PP_BACK_BUFFER_COUNT, buffer_count);
        write_pp(PP_DEVICE_WINDOW, window);

        graphics::BackendSwapChainDesc desc{};
        desc.native_view = native_view;
        desc.width = width;
        desc.height = height;
        desc.pixel_format = backend_format;
        desc.buffer_count = std::clamp(buffer_count + 1u, 2u, 3u);
        desc.vsync = read_pp(PP_PRESENTATION_INTERVAL) != 0x80000000u;
        desc.depth_stencil = true;
        const u32 multi_sample = read_pp(PP_MULTI_SAMPLE_TYPE);
        const u32 multi_quality = read_pp(PP_MULTI_SAMPLE_QUALITY);
        if (multi_sample != D3DMULTISAMPLE_NONE) {
            if (!d3d9_multisample_supported(m_backend, multi_sample) ||
                (read_pp(PP_FLAGS) & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) !=
                    0 ||
                read_pp(PP_SWAP_EFFECT) != D3DSWAPEFFECT_DISCARD) {
                m.set_return(D3DERR_NOTAVAILABLE);
                return;
            }
            desc.sample_count = d3d9_raster_sample_count(
                multi_sample, multi_quality, m_backend);
        }

        auto backend_chain = m_backend->create_swap_chain(desc);
        if (!backend_chain || !backend_chain->valid()) {
            m.set_return(D3DERR_NOTAVAILABLE);
            return;
        }

        const u32 chain = alloc_com_object(D3D9InterfaceId::SwapChain, self);
        if (chain == 0) {
            m.set_return(E_OUTOFMEMORY_D3D);
            return;
        }
        ComObject* chain_entry = find(chain, D3D9InterfaceId::SwapChain);
        if (chain_entry == nullptr) {
            destroy_object(chain);
            m.set_return(E_OUTOFMEMORY_D3D);
            return;
        }
        auto chain_state = std::make_unique<SwapChainState>();
        chain_state->present_parameters = parameters;
        chain_state->swap_chain = std::move(backend_chain);
        chain_state->back_buffer_surface = create_surface_object(
            self, width, height, format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
            static_cast<int>(SurfaceState::Kind::BackBuffer), 0, 0, 0,
            multi_sample);
        if (chain_state->back_buffer_surface == 0) {
            destroy_object(chain);
            m.set_return(D3DERR_OUTOFVIDEOMEMORY);
            return;
        }
        if (ComObject* back = find(chain_state->back_buffer_surface,
                                   D3D9InterfaceId::Surface);
            back != nullptr && back->surface) {
            back->surface->swap_chain_color =
                chain_state->swap_chain->color_target();
        }
        if (read_pp(PP_ENABLE_AUTO_DEPTH_STENCIL) != 0) {
            const u32 depth_format = read_pp(PP_AUTO_DEPTH_STENCIL_FORMAT);
            chain_state->depth_stencil_surface = create_surface_object(
                self, width, height, depth_format, D3DUSAGE_DEPTHSTENCIL,
                D3DPOOL_DEFAULT,
                static_cast<int>(SurfaceState::Kind::DepthStencil), 0, 0, 0,
                multi_sample);
            if (chain_state->depth_stencil_surface == 0) {
                destroy_object(chain_state->back_buffer_surface);
                destroy_object(chain);
                m.set_return(D3DERR_OUTOFVIDEOMEMORY);
                return;
            }
        }
        chain_entry->swap_chain_state = std::move(chain_state);
        state.additional_swap_chains.push_back(chain);

        if (m.memory().write_bytes(parameters_in, parameters.data(),
                                   parameters.size()).is_err() ||
            m.write_u32(out, chain).is_err()) {
            // Roll back the published chain on a guest write failure.
            state.additional_swap_chains.pop_back();
            destroy_object(chain);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VITREO_TRACE("[d3d9] CreateAdditionalSwapChain -> 0x%08x %ux%u "
                     "(chains=%zu)\n",
                     chain, width, height,
                     state.additional_swap_chains.size() + 1u);
        m.set_return(D3D_OK);
        return;
    }

    case 14: { // GetSwapChain
        // Learn: ordinals address CreateDevice implicit chains only (see
        // GetNumberOfSwapChains). Additional chains are returned from
        // CreateAdditionalSwapChain and are not GetSwapChain-enumerable.
        const u32 index = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        if (out == 0 || index != 0 || state.implicit_swap_chain == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 chain = state.implicit_swap_chain;
        (void)add_ref(chain);
        if (m.write_u32(out, chain).is_err()) {
            (void)release(chain);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 15: // GetNumberOfSwapChains
        // Learn: returns the number of implicit swap chains created by
        // CreateDevice — not chains from CreateAdditionalSwapChain.
        m.set_return(1u);
        return;

    case 16: { // Reset
        const GuestAddr parameters_in = m.arg_ptr(1);
        if (parameters_in == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.in_scene) {
            // Windows rejects Reset between BeginScene and EndScene.
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Learn: callers must Release additional swap chains before Reset.
        if (!state.additional_swap_chains.empty()) {
            VITREO_TRACE("[d3d9] Reset -> INVALIDCALL "
                         "(%zu additional swap chain(s) still held)\n",
                         state.additional_swap_chains.size());
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u8, D3DPRESENT_PARAMETERS_SIZE> parameters{};
        if (m.memory().read_bytes(parameters_in, parameters.data(),
                                  parameters.size()).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }

        // Recorded work cannot outlive the swap chain it targets.
        d3d9_cs_drain(true);
        flush_render_pass(state);
        state.pipelines.clear();
        state.draw_reuse = {};
        state.vs_const_dirty = true;
        state.ps_const_dirty = true;
        state.ff_uniform_dirty = true;
        state.rt_depth_scratch.clear();

        const u32 old_swap_chain = state.implicit_swap_chain;
        const u32 old_back_buffer = state.back_buffer_surface;
        const u32 old_depth = state.depth_stencil_surface;
        state.implicit_swap_chain = 0;
        state.back_buffer_surface = 0;
        state.depth_stencil_surface = 0;
        state.render_target_surface = 0;
        for (usize i = 1; i < state.render_target_surfaces.size(); ++i) {
            if (state.render_target_surfaces[i] != 0) {
                (void)release(state.render_target_surfaces[i]);
            }
        }
        state.render_target_surfaces = {};
        state.swap_chain.reset();
        for (u32 child : {old_swap_chain, old_back_buffer, old_depth}) {
            if (child != 0) destroy_object(child);
        }

        // Learn (SetCursorProperties): the cursor does not survive Reset.
        if (state.cursor_surface != 0) {
            const u32 cursor = state.cursor_surface;
            state.cursor_surface = 0;
            (void)release(cursor);
        }
        state.cursor_hotspot_x = 0;
        state.cursor_hotspot_y = 0;
        state.cursor_x = 0;
        state.cursor_y = 0;
        if (state.cursor_visible) {
            state.cursor_visible = false;
            if (m_cursor_visible_cb) m_cursor_visible_cb(false);
        }

        state.present_parameters = parameters;
        if (!build_presentation(self)) {
            // Windows leaves the device in the lost state when Reset fails,
            // and the application must call Reset again.
            state.lost = true;
            std::fprintf(stderr,
                         "[d3d9] Reset failed; device 0x%08x is lost\n", self);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.memory()
                .write_bytes(parameters_in, state.present_parameters.data(),
                             state.present_parameters.size())
                .is_err()) {
            state.lost = true;
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        reset_device_state(self);
        std::fprintf(stderr, "[d3d9] Reset device=0x%08x -> %ux%u windowed=%u\n",
                     self, state.back_buffer_width(), state.back_buffer_height(),
                     state.present_field(PP_WINDOWED));
        apply_exclusive_fullscreen(state);
        m.set_return(D3D_OK);
        return;
    }

    case 17: { // Present
        if (state.lost) {
            VITREO_TRACE("[d3d9] Present -> DEVICELOST (device already lost)\n");
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        if (state.in_scene) {
            // Present is only legal outside a BeginScene/EndScene pair.
            VITREO_TRACE("[d3d9] Present -> INVALIDCALL "
                         "(inside BeginScene/EndScene)\n");
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (!state.swap_chain || !state.swap_chain->valid()) {
            state.lost = true;
            VITREO_TRACE("[d3d9] Present -> DEVICELOST (no swap chain)\n");
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        if (d3d9_sequence_diag_enabled() && state.diagnostic_frame <= 8) {
            std::fprintf(stderr,
                         "[d3d9-sequence] frame=%llu present draws=%u\n",
                         static_cast<unsigned long long>(
                             state.diagnostic_frame),
                         state.diagnostic_draw);
        }
        const auto present_start = std::chrono::steady_clock::now();
        flush_render_pass(state);
        // After the render pass is flushed, before the drawable blit: this is
        // the only point where the back buffer holds the finished frame and is
        // still readable.
        if (d3d9_frame_dump_pending()) {
            d3d9_frame_dump_write(*state.swap_chain);
        }
        const auto submit_start = std::chrono::steady_clock::now();
        const u32 interval = state.present_field(PP_PRESENTATION_INTERVAL);
        // Everything the blocking call needs, captured BEFORE the lock goes:
        // nothing inside the guard may touch manager state.
        graphics::BackendSwapChain* const chain = state.swap_chain.get();
        const u32 sync = effective_present_sync(interval);

        bool present_ok = false;
        {
            // present() ends in MetalSubmitQueue::submit(), which sleeps on
            // vblank/backpressure. Holding the single D3D9 lock across that
            // sleep is what pinned the guest to ~0.8 cores: every other thread
            // touching D3D9 queued behind an owner that was not running.
            D3D9Manager::UnlockDuringBlockingCall unlocked;
            present_ok = chain->present(sync, 0);
        }
        if (!present_ok) {
            state.lost = true;
            VITREO_TRACE("[d3d9] Present failed; device 0x%08x is lost\n",
                         self);
            m.set_return(D3DERR_DEVICELOST);
            return;
        }
        static const bool perf_diag =
            std::getenv("VITREO_GRAPHICS_PERF_DIAG") != nullptr;
        if (perf_diag) {
            static auto previous_present_start = present_start;
            const auto frame_gap_us = std::chrono::duration_cast<
                std::chrono::microseconds>(present_start - previous_present_start)
                                          .count();
            previous_present_start = present_start;
            if (frame_gap_us >= 25000) {
                std::fprintf(stderr,
                             "[graphics-perf] d3d9 inter-present gap %.3f ms\n",
                             static_cast<double>(frame_gap_us) / 1000.0);
            }
            const auto present_us = std::chrono::duration_cast<
                std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                          present_start)
                                        .count();
            if (present_us >= 25000) {
                std::fprintf(stderr,
                             "[graphics-perf] d3d9 device present %.3f ms\n",
                             static_cast<double>(present_us) / 1000.0);
            }
        }
        // Rate-limit: TRACE once, then every 256th Present for bring-up.
        static u32 present_count = 0;
        ++present_count;
        if (present_count == 1 || (present_count & 0xFFu) == 0) {
            VITREO_TRACE("[d3d9] Present ok #%u device=0x%08x\n", present_count,
                         self);
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

    case 18: { // GetBackBuffer
        // Learn: iSwapChain is the same CreateDevice-implicit ordinal as
        // GetSwapChain / GetNumberOfSwapChains (multihead NumberOfAdaptersInGroup),
        // not CreateAdditionalSwapChain. Additional chains return their back
        // buffer via IDirect3DSwapChain9::GetBackBuffer on the COM pointer
        // from CreateAdditionalSwapChain.
        const u32 swap_chain = static_cast<u32>(m.arg(1));
        const u32 back_buffer = static_cast<u32>(m.arg(2));
        const u32 type = static_cast<u32>(m.arg(3));
        const GuestAddr out = m.arg_ptr(4);
        if (out != 0) (void)m.write_u32(out, 0);
        if (swap_chain != 0 || back_buffer != 0 ||
            type != D3DBACKBUFFER_TYPE_MONO || out == 0 ||
            state.back_buffer_surface == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(state.back_buffer_surface);
        if (m.write_u32(out, state.back_buffer_surface).is_err()) {
            (void)release(state.back_buffer_surface);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_device.cpp
/// @brief IDirect3DDevice9 creation, presentation setup and Reset state.
///
/// Presentation goes through the shared `graphics::GraphicsBackendDevice`
/// contract, the same one the Vulkan and OpenGL frontends use. Nothing here
/// talks to Metal directly.

#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_device_internal.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

namespace vitreo::winapi {

namespace {

constexpr u32 D3DCREATE_FPU_PRESERVE = 0x00000002;
constexpr u32 D3DCREATE_HARDWARE_VERTEXPROCESSING = 0x00000040;
constexpr u32 D3DCREATE_SOFTWARE_VERTEXPROCESSING = 0x00000020;
constexpr u32 D3DCREATE_MIXED_VERTEXPROCESSING = 0x00000080;

} // namespace

// =============================================================================
// Device creation
// =============================================================================

void D3D9Manager::create_device(Marshaler& m, u32 factory) {
    const u32 adapter = static_cast<u32>(m.arg(1));
    const u32 device_type = static_cast<u32>(m.arg(2));
    const u32 focus_window = static_cast<u32>(m.arg(3));
    const u32 behavior_flags = static_cast<u32>(m.arg(4));
    const GuestAddr present_parameters = m.arg_ptr(5);
    const GuestAddr device_out = m.arg_ptr(6);

    if (device_out != 0) (void)m.write_u32(device_out, 0);
    if (!valid_adapter(adapter, device_type) || present_parameters == 0 ||
        device_out == 0) {
        VITREO_TRACE("[d3d9] CreateDevice -> D3DERR_INVALIDCALL "
                     "(adapter=%u type=%u pp=0x%llx out=0x%llx)\n",
                     adapter, device_type,
                     static_cast<unsigned long long>(present_parameters),
                     static_cast<unsigned long long>(device_out));
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    // Exactly one vertex-processing mode must be requested.
    constexpr u32 vertex_processing_mask =
        D3DCREATE_HARDWARE_VERTEXPROCESSING |
        D3DCREATE_SOFTWARE_VERTEXPROCESSING |
        D3DCREATE_MIXED_VERTEXPROCESSING;
    const u32 requested = behavior_flags & vertex_processing_mask;
    if (requested == 0 || (requested & (requested - 1)) != 0) {
        VITREO_TRACE("[d3d9] CreateDevice -> D3DERR_INVALIDCALL "
                     "(behavior flags 0x%08x select %s vertex processing "
                     "mode)\n",
                     behavior_flags, requested == 0 ? "no" : "more than one");
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    std::array<u8, D3DPRESENT_PARAMETERS_SIZE> parameters{};
    if (m.memory().read_bytes(present_parameters, parameters.data(),
                              parameters.size()).is_err()) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    const u32 device = alloc_com_object(D3D9InterfaceId::Device, 0);
    if (device == 0) {
        VITREO_TRACE("[d3d9] CreateDevice -> E_OUTOFMEMORY "
                     "(guest COM storage unavailable)\n");
        m.set_return(E_OUTOFMEMORY_D3D);
        return;
    }

    ComObject* entry = find(device, D3D9InterfaceId::Device);
    entry->device = device;
    entry->device_state = std::make_unique<DeviceState>();
    DeviceState& state = *entry->device_state;
    state.adapter = adapter;
    state.device_type = device_type;
    state.focus_window = focus_window;
    state.behavior_flags = behavior_flags;
    state.parent_d3d9 = factory;
    state.present_parameters = parameters;

    // Learn IDirect3D9::CreateDevice / D3DCREATE_FPU_PRESERVE: unless the
    // caller asks to preserve it, Direct3D switches the x87 unit to single
    // precision and leaves it that way for the calling thread. Vitreo honours
    // the precision-control field, so setting it here would faithfully match
    // Windows arithmetic.
    //
    // DISABLED, deliberately. The JIT's native x87 arithmetic computes in
    // binary64 and cannot express a 24-bit significand, so it falls back to a
    // C helper for every operation whenever PC selects single precision (see
    // compile_fpu's pc_single_branch). Setting PC here therefore pushes a
    // D3D9 guest's entire x87 workload onto the helper path from CreateDevice
    // onward. Measured as a hard performance cliff: the guest reads as locked
    // up.
    //
    // Re-enable only together with a native PC=24 rounding sequence in
    // jit/ops/fpu.cpp, so honouring the control word does not cost the fast
    // path. Until then, matching Windows here is not worth making the guest
    // unusable.
    (void)D3DCREATE_FPU_PRESERVE;
    // GetAvailableTextureMem is an estimate rounded to the nearest MiB
    // (Microsoft Learn). Texture backing lives in host Metal memory, so
    // report a fixed MiB-aligned pool — same rationale as DirectDraw
    // GetAvailableVidMem. Leaving this at zero makes callers treat the
    // adapter as having no texture budget.
    constexpr u32 kAvailableTextureMemBytes = 512u * 1024u * 1024u;
    state.available_texture_mem = kAvailableTextureMemBytes;

    if (!build_presentation(device)) {
        destroy_object(device);
        // No presentable surface means no usable device. Windows reports
        // D3DERR_NOTAVAILABLE for a device the adapter cannot provide.
        VITREO_TRACE("[d3d9] CreateDevice -> D3DERR_NOTAVAILABLE "
                     "(shared backend produced no presentable swap chain)\n");
        m.set_return(D3DERR_NOTAVAILABLE);
        return;
    }

    // Learn: pPresentationParameters is in/out — zeros and D3DFMT_UNKNOWN
    // are filled with the resolved back-buffer size/format.
    if (m.memory()
            .write_bytes(present_parameters, state.present_parameters.data(),
                         state.present_parameters.size())
            .is_err()) {
        destroy_object(device);
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    reset_device_state(device);

    // The device holds a reference to the factory that created it.
    (void)add_ref(factory);

    if (m.write_u32(device_out, device).is_err()) {
        destroy_object(device);
        m.set_return(E_POINTER_D3D);
        return;
    }

    std::fprintf(stderr,
                 "[d3d9] CreateDevice -> device=0x%08x %ux%u fmt=%u "
                 "windowed=%u\n",
                 device, state.back_buffer_width(), state.back_buffer_height(),
                 state.present_field(PP_BACK_BUFFER_FORMAT),
                 state.present_field(PP_WINDOWED));
    diagnostics::phase_mark(diagnostics::LoadPhase::FirstDevice);
    apply_exclusive_fullscreen(state);
    m.set_return(D3D_OK);
}

bool D3D9Manager::build_presentation(u32 device) {
    ComObject* entry = find(device, D3D9InterfaceId::Device);
    if (entry == nullptr || !entry->device_state) return false;
    DeviceState& state = *entry->device_state;

    if (m_backend == nullptr || !m_backend->available()) return false;

    // A zero width/height in windowed mode means "use the device window's
    // client area"; D3DFMT_UNKNOWN means "use the current display mode format".
    u32 width = state.back_buffer_width();
    u32 height = state.back_buffer_height();
    u32 format = state.present_field(PP_BACK_BUFFER_FORMAT);
    u32 window = state.present_field(PP_DEVICE_WINDOW);
    if (window == 0) window = state.focus_window;
    resolve_present_back_buffer(state.present_field(PP_WINDOWED) != 0, window,
                                width, height, format);
    if (width == 0 || height == 0) {
        VITREO_TRACE("[d3d9] presentation: no back buffer size available\n");
        return false;
    }

    const graphics::PixelFormat backend_format = d3d9_backend_format(format);
    if (backend_format == graphics::PixelFormat::Unknown) {
        VITREO_TRACE("[d3d9] presentation: D3DFORMAT %u has no shared-core "
                     "pixel format\n", format);
        return false;
    }

    if (window == 0) {
        VITREO_TRACE("[d3d9] presentation: neither hDeviceWindow nor "
                     "hFocusWindow names a window\n");
        return false;
    }
    if (!m_presentation_view_resolver) {
        VITREO_TRACE("[d3d9] presentation: no presentation view resolver "
                     "wired\n");
        return false;
    }
    void* native_view = m_presentation_view_resolver(window);
    if (native_view == nullptr) {
        VITREO_TRACE("[d3d9] presentation: HWND 0x%08x has no native "
                     "presentation view\n", window);
        return false;
    }

    u32 buffer_count = state.present_field(PP_BACK_BUFFER_COUNT);
    if (buffer_count == 0) buffer_count = 1;

    graphics::BackendSwapChainDesc desc{};
    desc.native_view = native_view;
    desc.width = width;
    desc.height = height;
    desc.pixel_format = backend_format;
    // D3D9 counts back buffers; the shared contract counts total buffers.
    desc.buffer_count = std::clamp(buffer_count + 1u, 2u, 3u);
    // D3DPRESENT_INTERVAL_IMMEDIATE is 0x80000000; anything else waits.
    desc.vsync = state.present_field(PP_PRESENTATION_INTERVAL) != 0x80000000u;
    const u32 multi_sample = state.present_field(PP_MULTI_SAMPLE_TYPE);
    const u32 multi_quality = state.present_field(PP_MULTI_SAMPLE_QUALITY);
    const u32 present_flags = state.present_field(PP_FLAGS);
    const u32 swap_effect = state.present_field(PP_SWAP_EFFECT);
    if (multi_sample != D3DMULTISAMPLE_NONE) {
        if (!d3d9_multisample_supported(m_backend, multi_sample) ||
            (present_flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0 ||
            swap_effect != D3DSWAPEFFECT_DISCARD) {
            VITREO_TRACE("[d3d9] presentation: MultiSampleType %u is not "
                         "available (flags=0x%x swap=%u)\n",
                         multi_sample, present_flags, swap_effect);
            return false;
        }
        desc.sample_count =
            d3d9_raster_sample_count(multi_sample, multi_quality, m_backend);
        VITREO_TRACE("[d3d9] presentation: MSAA type=%u quality=%u "
                     "samples=%u\n",
                     multi_sample, multi_quality, desc.sample_count);
    }
    // Always allocate a Metal depth attachment on the swap-chain pass.
    // Many titles create the device with EnableAutoDepthStencil=FALSE and
    // bind depth later via CreateDepthStencilSurface/SetDepthStencilSurface.
    // Without a depth attachment, those draws are rejected when ZENABLE is
    // on. The D3D9 depth-surface *object* is still only created when auto
    // depth is requested (or when the app creates one explicitly).
    desc.depth_stencil = true;

    state.swap_chain = m_backend->create_swap_chain(desc);
    if (!state.swap_chain || !state.swap_chain->valid()) {
        state.swap_chain.reset();
        VITREO_TRACE("[d3d9] presentation: shared backend rejected a "
                     "%ux%u swap chain\n", width, height);
        return false;
    }

    // Write back the resolved values: Windows fills in the fields the caller
    // left as "choose for me", and applications read them afterwards.
    state.set_present_field(PP_BACK_BUFFER_WIDTH, width);
    state.set_present_field(PP_BACK_BUFFER_HEIGHT, height);
    state.set_present_field(PP_BACK_BUFFER_FORMAT, format);
    state.set_present_field(PP_BACK_BUFFER_COUNT, buffer_count);

    state.back_buffer_surface = create_surface_object(
        device, width, height, format, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
        static_cast<int>(SurfaceState::Kind::BackBuffer), 0, 0, 0,
        multi_sample);
    if (state.back_buffer_surface == 0) {
        state.swap_chain.reset();
        return false;
    }
    if (ComObject* back = find(state.back_buffer_surface,
                               D3D9InterfaceId::Surface);
        back != nullptr && back->surface) {
        back->surface->swap_chain_color = state.swap_chain->color_target();
    }
    state.render_target_surface = state.back_buffer_surface;
    state.render_target_surfaces[0] = state.back_buffer_surface;

    if (state.present_field(PP_ENABLE_AUTO_DEPTH_STENCIL) != 0) {
        const u32 depth_format =
            state.present_field(PP_AUTO_DEPTH_STENCIL_FORMAT);
        state.depth_stencil_surface = create_surface_object(
            device, width, height, depth_format, D3DUSAGE_DEPTHSTENCIL,
            D3DPOOL_DEFAULT,
            static_cast<int>(SurfaceState::Kind::DepthStencil), 0, 0, 0,
            multi_sample);
        if (state.depth_stencil_surface == 0) {
            VITREO_TRACE("[d3d9] presentation: automatic depth stencil "
                         "format %u could not be created\n", depth_format);
            state.swap_chain.reset();
            return false;
        }
    }

    state.implicit_swap_chain =
        alloc_com_object(D3D9InterfaceId::SwapChain, device);
    if (state.implicit_swap_chain == 0) {
        state.swap_chain.reset();
        return false;
    }

    state.lost = false;
    return true;
}

void D3D9Manager::reset_device_state(u32 device) {
    ComObject* entry = find(device, D3D9InterfaceId::Device);
    if (entry == nullptr || !entry->device_state) return;
    DeviceState& state = *entry->device_state;

    state.render_states.clear();
    state.transforms.clear();
    for (auto& stage : state.stage_states) stage.clear();
    for (auto& sampler : state.sampler_states) sampler.clear();
    for (auto& sampler : state.vertex_sampler_states) sampler.clear();
    state.textures.fill(0);
    for (u32& texture : state.vertex_textures) {
        const u32 previous = texture;
        texture = 0;
        if (previous != 0) (void)release(previous);
    }
    state.streams.fill(DeviceState::StreamSource{});
    state.indices = 0;
    if (state.vertex_declaration != 0) {
        (void)release(state.vertex_declaration);
        state.vertex_declaration = 0;
    }
    state.fvf = 0;
    state.in_scene = false;
    state.lights.clear();
    state.light_enabled.clear();
    state.clip_planes.fill({0.0f, 0.0f, 0.0f, 0.0f});
    state.clip_status_union = 0;
    state.clip_status_intersection = 0xFFFFFFFFu;
    state.material.fill(0);
    state.vs_const_f.fill(0.0f);
    state.ps_const_f.fill(0.0f);
    state.vs_const_i.fill(0);
    state.ps_const_i.fill(0);
    state.vs_const_b.fill(0);
    state.ps_const_b.fill(0);
    state.vertex_shader = 0;
    state.pixel_shader = 0;
    state.software_vertex_processing =
        (state.behavior_flags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0 ? 1
                                                                          : 0;
    state.npatch_mode = f32_bits(0.0f);
    state.texture_palettes.clear();
    state.current_texture_palette = 0;
    state.texture_palette_serial = 1;

    // The viewport and scissor cover the whole back buffer after creation and
    // after every successful Reset.
    const u32 width = state.back_buffer_width();
    const u32 height = state.back_buffer_height();
    state.viewport = {0, 0, width, height, f32_bits(0.0f), f32_bits(1.0f)};
    state.scissor = {0, 0, static_cast<i32>(width), static_cast<i32>(height)};

    // Learn: GetGammaRamp before Set returns a defined ramp. Identity maps
    // channel index i to (i << 8) | i. Reset restores the same default.
    d3d9_fill_identity_gamma_ramp(state.gamma_ramp);
    state.gamma_ramp_flags = 0;
    if (m_backend != nullptr) {
        m_backend->set_present_gamma_ramp(nullptr);
    }
    state.depth_pending_clear = false;
    state.depth_pending_clear_back_buffer = false;
    state.invalidate_draw_pipeline_reuse();
}

} // namespace vitreo::winapi

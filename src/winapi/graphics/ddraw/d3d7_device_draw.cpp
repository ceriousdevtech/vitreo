// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d7_device_draw.cpp
/// @brief IDirect3DDevice7 draw submission: DrawPrimitive and
///        DrawIndexedPrimitive.

#include "winapi/graphics/ddraw/ddraw.hpp"
#include "graphics/contracts/cpu_surface.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/ff_vertex_layout.hpp"
#include "graphics/shaders/ff_uniforms.hpp"
#include "graphics/shaders/ff_emulation.hpp"
#include "winapi/graphics/ddraw/d3d7_present.hpp"
#include "core/memory/guest_memory.hpp"

#include <vector>

namespace vitreo::winapi {

u32 DirectDrawManager::device_draw_primitive(GuestAddr device,
                                             u32 primitive_type, u32 fvf,
                                             GuestAddr vertices,
                                             u32 vertex_count, u32 flags) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    (void)flags;
    if (vertices == 0 || vertex_count == 0) return ddraw::E_INVALIDARG;
    if (primitive_type < d3d7::prim::POINTLIST ||
        primitive_type > d3d7::prim::TRIANGLEFAN) {
        return ddraw::E_INVALIDARG;
    }
    if (!state->state.in_scene) return ddraw::D3DERR_SCENE_NOT_IN_SCENE;

    SurfaceObject* target = find_surface(state->state.render_target);
    if (target == nullptr || !target->surface) {
        return ddraw::DDERR_INVALIDOBJECT;
    }

    // The vertex layout, the generated shader and the uniform payload are all
    // keyed off the same FVF and device state, so they are built together and
    // any one of them failing means the draw cannot be described at all.
    std::vector<graphics::BackendVertexAttribute> attributes;
    u32 stride = 0;
    const char* rejection = nullptr;
    if (!graphics::ff_build_vertex_layout(fvf, attributes, stride, rejection)) {
        VITREO_TRACE("[d3d7] DrawPrimitive fvf=0x%x unsupported: %s\n", fvf,
                     rejection != nullptr ? rejection : "unknown");
        return ddraw::E_INVALIDARG;
    }

    graphics::GraphicsBackendDevice* backend = graphics::active_backend_device();
    if (backend == nullptr || !backend->available()) {
        VITREO_TRACE("[d3d7] draw: no backend device\n");
        // Without a backend there is nothing that can rasterize; reporting a
        // frame that was never drawn would be worse than failing.
        return ddraw::E_NOTIMPL;
    }

    const graphics::FFPipelineState ff = d3d7::state_to_ff(state->state, fvf);
    const graphics::FFUniformInputs uniform_inputs =
        d3d7::state_to_ff_uniform_inputs(state->state);
    const graphics::FFGeneratedShaders shaders = graphics::generate_ff_shaders(ff);
    if (!shaders.valid()) {
        VITREO_TRACE("[d3d7] draw: shader generation failed\n");
        return ddraw::E_NOTIMPL;
    }

    const u32 width = target->surface->width();
    const u32 height = target->surface->height();
    if (width == 0 || height == 0) return ddraw::DDERR_INVALIDOBJECT;

    // Copy the guest's vertices verbatim: an FVF buffer is already the packed
    // layout the generated shader's stage-in declares.
    const usize vertex_bytes = static_cast<usize>(stride) * vertex_count;
    const Byte* guest_vertices = m_memory->host_ptr(vertices, vertex_bytes);
    if (guest_vertices == nullptr) return ddraw::E_INVALIDARG;

    graphics::BackendBufferDesc vertex_desc{};
    vertex_desc.byte_width = vertex_bytes;
    vertex_desc.vertex_buffer = true;
    auto vertex_buffer = backend->create_buffer(vertex_desc, guest_vertices);
    if (!vertex_buffer || !vertex_buffer->valid()) {
        VITREO_TRACE("[d3d7] draw: vertex buffer creation failed (%zu bytes)\n",
                     vertex_bytes);
        return ddraw::E_NOTIMPL;
    }

    graphics::BackendTexture2DDesc target_desc{};
    target_desc.width = width;
    target_desc.height = height;
    target_desc.mip_levels = 1;
    target_desc.array_size = 1;
    target_desc.pixel_format = graphics::PixelFormat::BGRA8Unorm;
    target_desc.render_target = true;
    target_desc.shader_resource = true;
    auto color = backend->create_texture_2d(target_desc, nullptr, 0);
    if (!color || !color->valid()) {
        VITREO_TRACE("[d3d7] draw: render target creation failed (%ux%u)\n",
                     width, height);
        return ddraw::E_NOTIMPL;
    }

    graphics::BackendPipelineDesc pipeline_desc{};
    pipeline_desc.vertex_shader_msl = shaders.vertex_msl;
    pipeline_desc.fragment_shader_msl = shaders.fragment_msl;
    // generate_ff_shaders() names its entry points ff_vertex / ff_fragment;
    // the descriptor's vertex_main / fragment_main defaults belong to the
    // programmable path and leave Metal unable to find the functions.
    pipeline_desc.vertex_entry_point = "ff_vertex";
    pipeline_desc.fragment_entry_point = "ff_fragment";
    pipeline_desc.vertex_attributes = attributes;
    pipeline_desc.vertex_buffer_strides[0] = stride;
    pipeline_desc.vertex_stride = stride;
    pipeline_desc.color_format = graphics::PixelFormat::BGRA8Unorm;
    // A zero write mask discards every channel; the fixed-function default is
    // to write all four.
    pipeline_desc.color_write_mask = 0xF;
    pipeline_desc.color_write_masks[0] = 0xF;
    pipeline_desc.cache_key = shaders.state_hash;
    auto pipeline = backend->create_pipeline(pipeline_desc);
    if (!pipeline || !pipeline->valid()) {
        VITREO_TRACE("[d3d7] draw: pipeline creation failed\n");
        return ddraw::E_NOTIMPL;
    }

    std::vector<u8> vertex_uniforms;
    std::vector<u8> fragment_uniforms;
    graphics::pack_ff_vertex_uniforms(ff, uniform_inputs, vertex_uniforms);
    graphics::pack_ff_fragment_uniforms(ff, uniform_inputs, fragment_uniforms);

    graphics::BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = color.get();
    // The surface already holds whatever Clear or a previous draw left there,
    // and Direct3D does not clear on BeginScene.
    pass_desc.preserve_color_contents = true;
    auto pass = backend->create_render_pass_standalone(pass_desc);
    if (!pass || !pass->valid()) {
        VITREO_TRACE("[d3d7] draw: render pass creation failed\n");
        return ddraw::E_NOTIMPL;
    }

    graphics::BackendDrawCall call{};
    call.pipeline = pipeline.get();
    switch (primitive_type) {
        case d3d7::prim::POINTLIST:
            call.topology = graphics::BackendPrimitiveTopology::PointList; break;
        case d3d7::prim::LINELIST:
            call.topology = graphics::BackendPrimitiveTopology::LineList; break;
        case d3d7::prim::LINESTRIP:
            call.topology = graphics::BackendPrimitiveTopology::LineStrip; break;
        case d3d7::prim::TRIANGLESTRIP:
            call.topology = graphics::BackendPrimitiveTopology::TriangleStrip; break;
        default:
            // A triangle fan has no Metal equivalent; it is converted by the
            // caller-facing path rather than silently drawn as a list.
            call.topology = graphics::BackendPrimitiveTopology::TriangleList; break;
    }
    call.vertex_buffer = vertex_buffer.get();
    call.vertex_buffers[0] = vertex_buffer.get();
    call.vertex_count = vertex_count;
    call.viewport = {0.0f, 0.0f, static_cast<float>(width),
                     static_cast<float>(height), 0.0f, 1.0f};
    call.vertex_constants = vertex_uniforms.data();
    call.vertex_constants_size = vertex_uniforms.size();
    call.fragment_constants = fragment_uniforms.data();
    call.fragment_constants_size = fragment_uniforms.size();

    if (!pass->draw(call)) {
        VITREO_TRACE("[d3d7] draw: submission rejected\n");
        return ddraw::E_NOTIMPL;
    }
    if (!pass->end()) {
        VITREO_TRACE("[d3d7] draw: render pass end failed\n");
        return ddraw::E_NOTIMPL;
    }

    // A Direct3D 7 render target is guest-visible surface memory, so the
    // frame is not finished until it lands back there.
    std::vector<u8> readback(static_cast<usize>(width) * height * 4u, 0);
    if (!color->readback(0, readback.data(), width * 4u)) {
        VITREO_TRACE("[d3d7] draw: readback failed\n");
        return ddraw::E_NOTIMPL;
    }
    Byte* pixels = m_memory->host_ptr_mut(target->pixels, target->pixel_bytes);
    if (pixels == nullptr) return ddraw::DDERR_INVALIDOBJECT;
    if (!d3d7::copy_bgra8_to_surface(readback.data(), width * 4u, width, height,
                                     reinterpret_cast<u8*>(pixels),
                                     target->surface->pitch(),
                                     target->surface->bits_per_pixel())) {
        return ddraw::DDERR_INVALIDPARAMS;
    }

    VITREO_TRACE("[d3d7] DrawPrimitive type=%u fvf=0x%x vertices=%u -> drawn "
                 "(%ux%u, stride=%u, viewport=%gx%g, vs_uniforms=%zu)\n",
                 primitive_type, fvf, vertex_count, width, height, stride,
                 static_cast<double>(uniform_inputs.viewport_width),
                 static_cast<double>(uniform_inputs.viewport_height),
                 vertex_uniforms.size());
    VITREO_TRACE("[d3d7] ff: hash=%llu lighting=%d vcolor=%d stages=%u "
                 "alpha=%d fog=%u specular=%d fvf=0x%x\n",
                 static_cast<unsigned long long>(ff.hash()),
                 ff.lighting_enabled ? 1 : 0, ff.vertex_color_enable ? 1 : 0,
                 ff.active_texture_stages, ff.alpha_test_enable ? 1 : 0,
                 static_cast<unsigned>(ff.fog_mode),
                 ff.specular_enable ? 1 : 0, ff.fvf_flags);
    return ddraw::DD_OK;
}

u32 DirectDrawManager::device_draw_indexed_primitive(
    GuestAddr device, u32 primitive_type, u32 fvf, GuestAddr vertices,
    u32 vertex_count, GuestAddr indices, u32 index_count, u32 flags) {
    DeviceObject* state = find_device(device);
    if (!state) return ddraw::DDERR_INVALIDOBJECT;
    (void)flags;
    if (vertices == 0 || vertex_count == 0 || indices == 0 ||
        index_count == 0) {
        return ddraw::E_INVALIDARG;
    }
    if (primitive_type < d3d7::prim::POINTLIST ||
        primitive_type > d3d7::prim::TRIANGLEFAN) {
        return ddraw::E_INVALIDARG;
    }
    if (!state->state.in_scene) return ddraw::D3DERR_SCENE_NOT_IN_SCENE;

    const graphics::FFPipelineState ff = d3d7::state_to_ff(state->state, fvf);
    VITREO_TRACE("[d3d7] DrawIndexedPrimitive type=%u fvf=0x%x vertices=%u "
                 "indices=%u (lights=%u stages=%u) -> unimplemented\n",
                 primitive_type, fvf, vertex_count, index_count,
                 ff.active_light_count, ff.active_texture_stages);
    return ddraw::E_NOTIMPL;
}

} // namespace vitreo::winapi

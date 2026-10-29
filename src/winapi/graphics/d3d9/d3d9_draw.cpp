// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_draw.cpp
/// @brief D3D9 DrawPrimitive/DrawIndexedPrimitive recording.
///
/// Translates the device's D3D9 state store into the shared-core
/// `FFPipelineState`, has `graphics/core/shader/ff_emulation` generate MSL for
/// it, builds a shared-core pipeline, and issues the draw through
/// `BackendRenderPass`. Nothing here is Metal-specific.
///
/// Programmable shaders may be bound on one or both stages. A missing stage
/// is filled with a fixed-function companion that shares the SM3 varying
/// contract (`[[user(colorN)]]` / `[[user(texcoordN)]]`) so the Metal
/// pipeline can link.

#include "winapi/graphics/d3d9/d3d9_draw_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_cs.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_draw_detail;

void D3D9Manager::draw_primitive(Marshaler& m, u32 self, bool indexed) {
    ComObject* entry = find(self, D3D9InterfaceId::Device);
    if (entry == nullptr || !entry->device_state) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    DeviceState& state = *entry->device_state;

    static const bool draw_diag =
        std::getenv("VITREO_D3D9_DRAW_DIAG") != nullptr;

    const auto reject = [&](const char* reason) {
        VITREO_TRACE("[d3d9] Draw%sPrimitive -> E_NOTIMPL (%s)\n",
                     indexed ? "Indexed" : "", reason);
        if (draw_diag) {
            std::fprintf(stderr,
                         "[d3d9-draw-diag] rejected indexed=%u reason=%s "
                         "vs=0x%08x ps=0x%08x fvf=0x%08x decl=0x%08x "
                         "rt=0x%08x\n",
                         indexed ? 1u : 0u, reason, state.vertex_shader,
                         state.pixel_shader, state.fvf,
                         state.vertex_declaration, state.render_target_surface);
            std::fflush(stderr);
        }
        m.set_return(E_NOTIMPL_D3D);
    };

    if (state.lost) {
        m.set_return(D3DERR_DEVICELOST);
        return;
    }
    // Windows requires draws to sit inside a scene.
    if (!state.in_scene) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    diagnostics::phase_mark(diagnostics::LoadPhase::FirstDraw);
    diagnostics::phase_note_draw();
    // D3D9 allows a programmable stage on only one side of the pipeline; the
    // other side stays fixed-function. Pure FF and a full VS+PS pair are the
    // other two legal combinations.
    const bool has_vs = state.vertex_shader != 0;
    const bool has_ps = state.pixel_shader != 0;
    const bool programmable_vs = has_vs;
    const bool programmable_ps = has_ps;
    const bool programmable_pair = has_vs && has_ps;
    const bool fixed_function_only = !has_vs && !has_ps;

    u32 primitive_type = 0;
    u32 first_vertex = 0;
    u32 index_start = 0;
    u32 primitive_count = 0;
    i32 base_vertex = 0;
    if (state.up_override.has_value()) {
        const DeviceState::UpDrawOverride& up = *state.up_override;
        primitive_type = up.primitive_type;
        first_vertex = up.first_vertex;
        index_start = up.index_start;
        primitive_count = up.primitive_count;
        base_vertex = up.base_vertex;
    } else if (indexed) {
        primitive_type = static_cast<u32>(m.arg(1));
        base_vertex = static_cast<i32>(m.arg(2));
        index_start = static_cast<u32>(m.arg(5));
        primitive_count = static_cast<u32>(m.arg(6));
    } else {
        primitive_type = static_cast<u32>(m.arg(1));
        first_vertex = static_cast<u32>(m.arg(2));
        primitive_count = static_cast<u32>(m.arg(3));
    }

    if (primitive_count == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    u32 element_count =
        vertices_for_primitives(primitive_type, primitive_count);
    if (element_count == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    // Learn: FVF describes a single interleaved stream (stream 0). Vertex
    // declarations may pull elements from multiple SetStreamSource slots.
    const DeviceState::StreamSource& stream = state.streams[0];
    ComObject* vertex_entry =
        stream.buffer != 0 ? find(stream.buffer, D3D9InterfaceId::VertexBuffer)
                           : nullptr;
    if (vertex_entry == nullptr || !vertex_entry->buffer ||
        !vertex_entry->buffer->backend) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    ComObject* index_entry = nullptr;
    if (indexed) {
        if (state.indices == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        index_entry = find(state.indices, D3D9InterfaceId::IndexBuffer);
        if (index_entry == nullptr || !index_entry->buffer ||
            !index_entry->buffer->backend) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
    }

    // Metal has no triangle-fan topology. Expand to an indexed triangle list
    // for this draw only, then restore the device's index binding (Draw*UP
    // pattern). Learn: D3DPT_TRIANGLEFAN — triangles (v0,v1,v2), (v0,v2,v3), …
    struct FanIndexRestore {
        D3D9Manager& mgr;
        DeviceState& state;
        u32 saved_indices = 0;
        u32 temp_ib = 0;
        bool armed = false;
        ~FanIndexRestore() {
            if (!armed) return;
            if (state.indices != 0) (void)mgr.release(state.indices);
            state.indices = saved_indices;
            if (temp_ib != 0) (void)mgr.release(temp_ib);
        }
    } fan_restore{*this, state};

    if (primitive_type == D3DPT_TRIANGLEFAN) {
        const u32 fan_index_count = primitive_count + 2;
        std::vector<u32> fan_indices(fan_index_count);
        bool out_32bit = false;
        if (indexed) {
            const bool src_32bit =
                index_entry->buffer->fvf_or_format == D3DFMT_INDEX32;
            out_32bit = src_32bit;
            const u32 src_stride = src_32bit ? 4u : 2u;
            const u64 src_bytes =
                static_cast<u64>(fan_index_count) * src_stride;
            const u64 src_offset = static_cast<u64>(index_start) * src_stride;
            if (index_entry->buffer->storage == 0 ||
                src_offset + src_bytes > index_entry->buffer->length) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            std::vector<u8> raw(static_cast<usize>(src_bytes));
            if (m.memory()
                    .read_bytes(index_entry->buffer->storage +
                                    static_cast<GuestAddr>(src_offset),
                                raw.data(), raw.size())
                    .is_err()) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
            for (u32 i = 0; i < fan_index_count; ++i) {
                if (src_32bit) {
                    u32 value = 0;
                    std::memcpy(&value, raw.data() + i * 4, 4);
                    fan_indices[i] = value;
                } else {
                    u16 value = 0;
                    std::memcpy(&value, raw.data() + i * 2, 2);
                    fan_indices[i] = value;
                }
            }
        } else {
            out_32bit = fan_index_count > 65535u;
            for (u32 i = 0; i < fan_index_count; ++i) {
                fan_indices[i] = i;
            }
            base_vertex = static_cast<i32>(first_vertex);
        }

        const u32 expanded_count = primitive_count * 3;
        const u32 out_stride = out_32bit ? 4u : 2u;
        std::vector<u8> expanded(static_cast<usize>(expanded_count) *
                                 out_stride);
        for (u32 t = 0; t < primitive_count; ++t) {
            const u32 tri[3] = {fan_indices[0], fan_indices[t + 1],
                                fan_indices[t + 2]};
            for (u32 k = 0; k < 3; ++k) {
                if (out_32bit) {
                    std::memcpy(expanded.data() + (t * 3 + k) * 4, &tri[k],
                                4);
                } else {
                    const u16 narrow = static_cast<u16>(tri[k]);
                    std::memcpy(expanded.data() + (t * 3 + k) * 2, &narrow,
                                2);
                }
            }
        }

        const u32 temp_ib = create_host_buffer(
            self, true, static_cast<u32>(expanded.size()),
            out_32bit ? D3DFMT_INDEX32 : D3DFMT_INDEX16, expanded.data());
        if (temp_ib == 0) {
            m.set_return(D3DERR_OUTOFVIDEOMEMORY);
            return;
        }

        fan_restore.saved_indices = state.indices;
        fan_restore.temp_ib = temp_ib;
        state.indices = temp_ib;
        (void)add_ref(temp_ib);
        fan_restore.armed = true;

        index_entry = find(state.indices, D3D9InterfaceId::IndexBuffer);
        if (index_entry == nullptr || !index_entry->buffer ||
            !index_entry->buffer->backend) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        indexed = true;
        primitive_type = D3DPT_TRIANGLELIST;
        index_start = 0;
        element_count = expanded_count;
    }

    if (state.fvf == 0 && state.vertex_declaration == 0) {
        reject("no FVF or vertex declaration is set");
        return;
    }

    D3d9DrawSnapshot snap{};
    snap.manager = this;
    snap.backend = m_backend;
    snap.indexed = indexed;
    snap.primitive_type = primitive_type;
    snap.first_vertex = first_vertex;
    snap.index_start = index_start;
    snap.primitive_count = primitive_count;
    snap.element_count = element_count;
    snap.base_vertex = base_vertex;
    snap.vertex_shader = state.vertex_shader;
    snap.pixel_shader = state.pixel_shader;
    snap.fvf = state.fvf;
    snap.vertex_declaration = state.vertex_declaration;
    snap.render_target_surface = state.render_target_surface;
    snap.render_target_surfaces = state.render_target_surfaces;
    snap.depth_stencil_surface = state.depth_stencil_surface;
    snap.indices = state.indices;
    snap.has_vs = has_vs;
    snap.has_ps = has_ps;
    snap.programmable_vs = programmable_vs;
    snap.programmable_ps = programmable_ps;
    snap.programmable_pair = programmable_pair;
    snap.fixed_function_only = fixed_function_only;
    snap.auto_depth_stencil =
        state.present_field(PP_ENABLE_AUTO_DEPTH_STENCIL) != 0;
    snap.back_buffer_width = state.back_buffer_width();
    snap.back_buffer_height = state.back_buffer_height();
    snap.back_buffer_format = state.present_field(PP_BACK_BUFFER_FORMAT);
    snap.multi_sample_type = state.present_field(PP_MULTI_SAMPLE_TYPE);
    snap.multi_sample_quality = state.present_field(PP_MULTI_SAMPLE_QUALITY);
    snap.viewport = state.viewport;
    snap.scissor = state.scissor;
    snap.tables = state.freeze_draw_tables();
    snap.clip_planes = state.clip_planes;
    snap.texture_ids = state.textures;
    snap.vertex_texture_ids = state.vertex_textures;
    snap.material = state.material;
    snap.diagnostic_frame = state.diagnostic_frame;

    struct RetainGuard {
        D3D9Manager& mgr;
        std::vector<u32>& ids;
        bool dismiss{false};
        ~RetainGuard() {
            if (dismiss) {
                return;
            }
            for (u32 id : ids) {
                (void)mgr.release(id);
            }
            ids.clear();
        }
    } retain_guard{*this, snap.retained};
    const auto retain = [&](u32 id) {
        if (id == 0) {
            return;
        }
        (void)add_ref(id);
        snap.retained.push_back(id);
    };

    std::vector<FvfElement>& elements = snap.elements;
    u32 stride = 0;
    std::array<u32, D3D9_MAX_STREAMS> stream_layout_strides{};
    const char* rejection = "an unsupported vertex format";
    if (state.vertex_declaration != 0) {
        ComObject* decl_entry =
            find(state.vertex_declaration, D3D9InterfaceId::VertexDeclaration);
        if (decl_entry == nullptr || !decl_entry->vertex_declaration) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const VertexDeclarationState& decl = *decl_entry->vertex_declaration;
        elements = decl.elements;
        stream_layout_strides = decl.stream_strides;
        stride = decl.stream_strides[0];
        // DIAGNOSTIC (VITREO_D3D9_STRIDE_DIAG=1): the stride used above is
        // INFERRED from the declaration (max offset+size). D3D9's authority is
        // the Stride passed to SetStreamSource, which may legitimately be
        // larger (padding, interleaved data the shader ignores, or a decl
        // describing only part of the vertex). Where they disagree, every
        // vertex after the first is fetched from the wrong offset.
        {
            static const bool s_diag =
                std::getenv("VITREO_D3D9_STRIDE_DIAG") != nullptr;
            if (s_diag) {
                for (u32 sl = 0; sl < D3D9_MAX_STREAMS; ++sl) {
                    const u32 app = state.streams[sl].stride;
                    const u32 inferred = decl.stream_strides[sl];
                    if (app != 0 && inferred != 0 && app != inferred) {
                        std::fprintf(stderr,
                            "[d3d9-stride] MISMATCH stream=%u app=%u "
                            "inferred=%u\n",
                            sl, app, inferred);
                    }
                }
            }
        }
        if (elements.empty() || stride == 0) {
            reject("the bound vertex declaration has no stream-0 layout");
            return;
        }
        retain(state.vertex_declaration);
    } else if (!enumerate_fvf_elements(state.fvf, elements, stride,
                                       rejection)) {
        reject(rejection);
        return;
    } else {
        stream_layout_strides[0] = stride;
        if (!state.up_override.has_value()) {
            for (u32 slot = 1; slot < D3D9_MAX_STREAMS; ++slot) {
                if (state.streams[slot].buffer != 0) {
                    reject("multiple vertex streams");
                    return;
                }
            }
        }
    }

    std::array<bool, D3D9_MAX_STREAMS> stream_used{};
    for (const FvfElement& element : elements) {
        if (element.stream >= D3D9_MAX_STREAMS ||
            element.stream >= graphics::BACKEND_MAX_VERTEX_BUFFERS) {
            reject("a vertex declaration stream beyond the backend limit");
            return;
        }
        stream_used[element.stream] = true;
    }
    stream_used[0] = true;
    if (state.up_override.has_value()) {
        for (u32 slot = 1; slot < D3D9_MAX_STREAMS; ++slot) {
            if (stream_used[slot]) {
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
    }

    for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
        if (!stream_used[slot]) {
            continue;
        }
        const DeviceState::StreamSource& src = state.streams[slot];
        ComObject* stream_entry =
            src.buffer != 0 ? find(src.buffer, D3D9InterfaceId::VertexBuffer)
                            : nullptr;
        if (stream_entry == nullptr || !stream_entry->buffer ||
            !stream_entry->buffer->backend) {
            VITREO_TRACE("[d3d9] Draw: stream %u used by the vertex "
                         "declaration has no bound vertex buffer\n",
                         slot);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 expected = stream_layout_strides[slot];
        if (expected == 0 ||
            (state.vertex_declaration != 0 ? src.stride < expected
                                           : src.stride != expected)) {
            VITREO_TRACE("[d3d9] Draw: stream %u stride %u does not match the "
                         "%u bytes the %s describes\n",
                         slot, src.stride, expected,
                         state.vertex_declaration != 0 ? "vertex declaration"
                                                       : "FVF");
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        stream_layout_strides[slot] = src.stride;
        snap.streams[slot].buffer = src.buffer;
        snap.streams[slot].offset = src.offset;
        snap.streams[slot].stride = src.stride;
        snap.streams[slot].frequency = src.frequency;
        snap.streams[slot].backend = stream_entry->buffer->backend.get();
        retain(src.buffer);
    }
    snap.stream_layout_strides = stream_layout_strides;
    snap.stream_used = stream_used;

    if (indexed) {
        if (index_entry == nullptr || !index_entry->buffer ||
            !index_entry->buffer->backend) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        snap.index_backend = index_entry->buffer->backend.get();
        snap.index_format = index_entry->buffer->fvf_or_format;
        retain(state.indices);
    }

    if (has_vs) {
        ComObject* vs_entry =
            find(state.vertex_shader, D3D9InterfaceId::VertexShader);
        if (vs_entry == nullptr || !vs_entry->shader) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        snap.vs_translated = &vs_entry->shader->translated;
        retain(state.vertex_shader);
        {
            const graphics::Sm3Shader& vs = *snap.vs_translated;
            const u32 fcount = std::min(vs.float_constant_count,
                                        static_cast<u32>(D3D9_MAX_SHADER_CONST_F));
            const u32 icount = std::min(vs.int_constant_count,
                                        static_cast<u32>(D3D9_MAX_SHADER_CONST_I));
            const u32 bcount = std::min(vs.bool_constant_count,
                                        static_cast<u32>(D3D9_MAX_SHADER_CONST_B));
            snap.vs_const_f.assign(state.vs_const_f.begin(),
                                   state.vs_const_f.begin() + fcount * 4);
            snap.vs_const_i.assign(state.vs_const_i.begin(),
                                   state.vs_const_i.begin() + icount * 4);
            snap.vs_const_b.assign(state.vs_const_b.begin(),
                                   state.vs_const_b.begin() + bcount);
        }
        for (const graphics::Sm3InputElement& input :
             snap.vs_translated->inputs) {
            const u32 usage = static_cast<u32>(input.usage);
            bool match = false;
            for (const FvfElement& element : elements) {
                if (element.usage == usage &&
                    element.usage_index == input.usage_index) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                VITREO_TRACE("[d3d9] Draw: the vertex shader declares usage "
                             "%u index %u, which the bound vertex layout does "
                             "not provide\n",
                             usage, input.usage_index);
                m.set_return(D3DERR_INVALIDCALL);
                return;
            }
        }
    }
    if (has_ps) {
        ComObject* ps_entry =
            find(state.pixel_shader, D3D9InterfaceId::PixelShader);
        if (ps_entry == nullptr || !ps_entry->shader) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        snap.ps_translated = &ps_entry->shader->translated;
        snap.ps_bytecode = ps_entry->shader->bytecode.data();
        snap.ps_bytecode_size = ps_entry->shader->bytecode.size();
        retain(state.pixel_shader);
        {
            const graphics::Sm3Shader& ps = *snap.ps_translated;
            const u32 fcount = std::min(ps.float_constant_count,
                                        static_cast<u32>(D3D9_MAX_SHADER_CONST_F));
            const u32 icount = std::min(ps.int_constant_count,
                                        static_cast<u32>(D3D9_MAX_SHADER_CONST_I));
            const u32 bcount = std::min(ps.bool_constant_count,
                                        static_cast<u32>(D3D9_MAX_SHADER_CONST_B));
            snap.ps_const_f.assign(state.ps_const_f.begin(),
                                   state.ps_const_f.begin() + fcount * 4);
            snap.ps_const_i.assign(state.ps_const_i.begin(),
                                   state.ps_const_i.begin() + icount * 4);
            snap.ps_const_b.assign(state.ps_const_b.begin(),
                                   state.ps_const_b.begin() + bcount);
        }
    }

    snap.rt_color = resolve_render_target_color(
        state.render_target_surfaces[0], state.back_buffer_surface);
    snap.rt_colors[0] = snap.rt_color;
    for (u32 slot = 1; slot < snap.rt_colors.size(); ++slot) {
        const u32 surface = state.render_target_surfaces[slot];
        if (surface == 0) continue;
        snap.rt_colors[slot] =
            resolve_render_target_color(surface, state.back_buffer_surface);
        if (snap.rt_colors[slot] == nullptr) {
            reject("a secondary render target has no shared-core texture");
            return;
        }
    }
    const auto slice_for = [&](u32 surface_id) -> unsigned {
        if (surface_id == 0) return 0;
        ComObject* surface_entry = find(surface_id, D3D9InterfaceId::Surface);
        if (surface_entry == nullptr || !surface_entry->surface) return 0;
        return surface_entry->surface->face;
    };
    snap.rt_color_slices[0] = slice_for(state.render_target_surfaces[0]);
    for (u32 slot = 1; slot < snap.rt_color_slices.size(); ++slot) {
        snap.rt_color_slices[slot] =
            slice_for(state.render_target_surfaces[slot]);
    }
    snap.to_back_buffer = state.render_target_surface == 0 ||
                          state.render_target_surface == state.back_buffer_surface;
    if (!snap.to_back_buffer && snap.rt_color == nullptr) {
        reject("bound render target has no shared-core texture");
        return;
    }

    snap.color_width = state.back_buffer_width();
    snap.color_height = state.back_buffer_height();
    snap.color_format = state.present_field(PP_BACK_BUFFER_FORMAT);
    snap.color_formats[0] = snap.color_format;
    if (!snap.to_back_buffer) {
        ComObject* surface_entry =
            find(state.render_target_surface, D3D9InterfaceId::Surface);
        if (surface_entry && surface_entry->surface) {
            snap.color_width = surface_entry->surface->width;
            snap.color_height = surface_entry->surface->height;
            snap.color_format = surface_entry->surface->format;
            snap.color_formats[0] = snap.color_format;
        }
        retain(state.render_target_surface);
    }
    for (u32 slot = 1; slot < snap.render_target_surfaces.size(); ++slot) {
        const u32 surface = snap.render_target_surfaces[slot];
        if (surface == 0) continue;
        ComObject* surface_entry = find(surface, D3D9InterfaceId::Surface);
        if (surface_entry == nullptr || !surface_entry->surface) {
            reject("a secondary render target surface disappeared");
            return;
        }
        snap.color_formats[slot] = surface_entry->surface->format;
        retain(surface);
    }
    snap.rt_depth = snap.to_back_buffer
                        ? nullptr
                        : resolve_depth_attachment(state, snap.color_width,
                                                   snap.color_height);
    if (state.depth_stencil_surface != 0) {
        retain(state.depth_stencil_surface);
    }

    for (u32 slot = 0; slot < D3D9_MAX_SAMPLERS; ++slot) {
        const u32 tex_id = slot < state.textures.size() ? state.textures[slot] : 0;
        snap.textures[slot].com_id = tex_id;
        if (tex_id == 0) {
            continue;
        }
        ComObject* texture_entry = find(tex_id, D3D9InterfaceId::Texture);
        if (texture_entry == nullptr) {
            texture_entry = find(tex_id, D3D9InterfaceId::CubeTexture);
        }
        if (texture_entry == nullptr) {
            texture_entry = find(tex_id, D3D9InterfaceId::VolumeTexture);
        }
        const bool has_2d = texture_entry != nullptr &&
                            texture_entry->texture &&
                            texture_entry->texture->backend;
        const bool has_3d =
            texture_entry != nullptr && texture_entry->texture &&
            texture_entry->texture->backend_3d &&
            texture_entry->texture->backend_3d->valid();
        if (has_2d) {
            if (d3d9_is_palettized_format(texture_entry->texture->format)) {
                sync_palettized_texture(*texture_entry->texture, state, false);
            }
            snap.textures[slot].tex2d = texture_entry->texture->backend.get();
            snap.textures[slot].width = texture_entry->texture->width;
            snap.textures[slot].height = texture_entry->texture->height;
        } else if (has_3d) {
            snap.textures[slot].tex3d = texture_entry->texture->backend_3d.get();
        } else {
            snap.textures[slot].missing_backing = true;
        }
        retain(tex_id);
    }
    for (u32 slot = 0; slot < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT; ++slot) {
        const u32 tex_id = state.vertex_textures[slot];
        snap.vertex_textures[slot].com_id = tex_id;
        if (tex_id == 0) continue;
        ComObject* texture_entry = find(tex_id, D3D9InterfaceId::Texture);
        if (texture_entry == nullptr) {
            texture_entry = find(tex_id, D3D9InterfaceId::CubeTexture);
        }
        if (texture_entry == nullptr) {
            texture_entry = find(tex_id, D3D9InterfaceId::VolumeTexture);
        }
        const bool has_2d = texture_entry != nullptr &&
                            texture_entry->texture &&
                            texture_entry->texture->backend;
        const bool has_3d = texture_entry != nullptr &&
                            texture_entry->texture &&
                            texture_entry->texture->backend_3d &&
                            texture_entry->texture->backend_3d->valid();
        if (has_2d) {
            if (d3d9_is_palettized_format(texture_entry->texture->format)) {
                sync_palettized_texture(*texture_entry->texture, state, false);
            }
            snap.vertex_textures[slot].tex2d =
                texture_entry->texture->backend.get();
            snap.vertex_textures[slot].width = texture_entry->texture->width;
            snap.vertex_textures[slot].height = texture_entry->texture->height;
        } else if (has_3d) {
            snap.vertex_textures[slot].tex3d =
                texture_entry->texture->backend_3d.get();
        } else {
            snap.vertex_textures[slot].missing_backing = true;
        }
        retain(tex_id);
    }
    snap.white_texture = default_white_texture();

    if (!ensure_draw_render_pass(state, snap)) {
        state.lost = true;
        m.set_return(D3DERR_DEVICELOST);
        return;
    }
    ++state.diagnostic_draw;

    retain_guard.dismiss = true;
    D3d9SubmitQueue::instance().enqueue_draw(std::move(snap));
    m.set_return(D3D_OK);
}

} // namespace vitreo::winapi

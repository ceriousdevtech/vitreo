// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_draw_execute.cpp
/// @brief D3D9 worker-side draw execution: turns a recorded draw snapshot
///        into a shared-core `BackendDrawCall`.
///
/// The uniform layouts packed here must match the generated fixed-function
/// and SM3 shaders; see `D3D9Manager::pack_vertex_uniforms`.

#include "winapi/graphics/d3d9/d3d9_draw_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_cs.hpp"

#include "graphics/shaders/ff_emulation.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_draw_detail;

namespace {

/// Bytes a vertex element occupies, for bounds-checking a declaration against
/// the stride the app bound. Mirrors D3DDECLTYPE sizes.
[[nodiscard]] u32 d3d9_vertex_format_size(graphics::BackendVertexFormat f) {
    using F = graphics::BackendVertexFormat;
    switch (f) {
    case F::Float1: return 4;
    case F::Float2: return 8;
    case F::Float3: return 12;
    case F::Float4: return 16;
    case F::UByte4NormalizedBGRA:
    case F::UByte4Normalized:
    case F::UByte4: return 4;
    case F::Short2:
    case F::Short2Normalized:
    case F::UShort2Normalized:
    case F::Half2: return 4;
    case F::Short4:
    case F::Short4Normalized:
    case F::UShort4Normalized:
    case F::Half4: return 8;
    case F::Dec3N: return 4;
    }
    return 16;
}

[[nodiscard]] graphics::BackendBlendFactor blend_factor(u32 d3d_factor) {
    switch (d3d_factor) {
    case 1:  return graphics::BackendBlendFactor::Zero;
    case 2:  return graphics::BackendBlendFactor::One;
    case 3:  return graphics::BackendBlendFactor::SourceColor;
    case 4:  return graphics::BackendBlendFactor::OneMinusSourceColor;
    case 5:  return graphics::BackendBlendFactor::SourceAlpha;
    case 6:  return graphics::BackendBlendFactor::OneMinusSourceAlpha;
    case 7:  return graphics::BackendBlendFactor::DestinationAlpha;
    case 8:  return graphics::BackendBlendFactor::OneMinusDestinationAlpha;
    case 9:  return graphics::BackendBlendFactor::DestinationColor;
    case 10: return graphics::BackendBlendFactor::OneMinusDestinationColor;
    case 11: return graphics::BackendBlendFactor::SourceAlphaSaturated;
    case 14: return graphics::BackendBlendFactor::BlendColor;
    case 15: return graphics::BackendBlendFactor::OneMinusBlendColor;
    default: return graphics::BackendBlendFactor::One;
    }
}

[[nodiscard]] graphics::BackendBlendOperation blend_operation(u32 d3d_op) {
    switch (d3d_op) {
    case 2: return graphics::BackendBlendOperation::Subtract;
    case 3: return graphics::BackendBlendOperation::ReverseSubtract;
    case 4: return graphics::BackendBlendOperation::Min;
    case 5: return graphics::BackendBlendOperation::Max;
    default: return graphics::BackendBlendOperation::Add;
    }
}

[[nodiscard]] graphics::BackendCompare compare_function(u32 d3d_compare) {
    switch (d3d_compare) {
    case 1: return graphics::BackendCompare::Never;
    case 2: return graphics::BackendCompare::Less;
    case 3: return graphics::BackendCompare::Equal;
    case 4: return graphics::BackendCompare::LessEqual;
    case 5: return graphics::BackendCompare::Greater;
    case 6: return graphics::BackendCompare::NotEqual;
    case 7: return graphics::BackendCompare::GreaterEqual;
    default: return graphics::BackendCompare::Always;
    }
}

[[nodiscard]] graphics::BackendAddressMode address_mode(u32 d3d_address) {
    switch (d3d_address) {
    case 1: return graphics::BackendAddressMode::Wrap;
    case 2: return graphics::BackendAddressMode::Mirror;
    case 3: return graphics::BackendAddressMode::Clamp;
    case 4: return graphics::BackendAddressMode::Border;
    case 5: return graphics::BackendAddressMode::MirrorOnce;
    default: return graphics::BackendAddressMode::Wrap;
    }
}

/// D3DTEXF_POINT is 1, D3DTEXF_LINEAR is 2; anything higher is an anisotropic
/// or gaussian variant that still resolves to linear sampling here.
[[nodiscard]] graphics::BackendFilter texture_filter(u32 d3d_filter) {
    return d3d_filter >= 2 ? graphics::BackendFilter::Linear
                           : graphics::BackendFilter::Nearest;
}

} // namespace

void D3D9Manager::execute_recorded_draw(D3d9DrawSnapshot& snap) {
    if (snap.render_pass == nullptr || snap.backend == nullptr ||
        snap.tables == nullptr) {
        return;
    }

    const bool programmable_vs = snap.programmable_vs;
    const bool programmable_ps = snap.programmable_ps;
    const bool programmable_pair = snap.programmable_pair;
    const bool fixed_function_only = snap.fixed_function_only;
    const graphics::Sm3Shader* vertex_shader = snap.vs_translated;
    const graphics::Sm3Shader* pixel_shader = snap.ps_translated;
    if ((programmable_vs && vertex_shader == nullptr) ||
        (programmable_ps && pixel_shader == nullptr)) {
        return;
    }

    std::vector<graphics::BackendVertexAttribute> attributes;
    graphics::FFPipelineState ff{};
    bool need_ff = fixed_function_only || !programmable_pair;
    u64 shader_hash = 0;

    if (programmable_vs) {
        for (const graphics::Sm3InputElement& input : vertex_shader->inputs) {
            const u32 usage = static_cast<u32>(input.usage);
            const FvfElement* match = nullptr;
            for (const FvfElement& element : snap.elements) {
                if (element.usage == usage &&
                    element.usage_index == input.usage_index) {
                    match = &element;
                    break;
                }
            }
            if (match == nullptr) {
                VITREO_TRACE("[d3d9] Draw: the vertex shader declares usage "
                             "%u index %u, which the bound vertex layout does "
                             "not provide\n",
                             usage, input.usage_index);
                return;
            }
            attributes.push_back({input.register_index, match->offset,
                                  match->format, match->stream});
        }
    } else {
        unsigned location = 0;
        for (const FvfElement& element : snap.elements) {
            attributes.push_back({location++, element.offset, element.format,
                                  element.stream});
        }
    }

    if (need_ff) {
        if (!build_ff_state(snap, ff)) {
            VITREO_TRACE("[d3d9] Draw: the current fixed-function state has "
                         "no shader translation\n");
            return;
        }
        if (snap.fvf == 0 && !snap.elements.empty()) {
            u32 synthesized = 0;
            u32 tex_count = 0;
            for (const FvfElement& element : snap.elements) {
                if (element.usage == 0 && element.usage_index == 0) {
                    synthesized |= 0x002;
                } else if (element.usage == 9) {
                    synthesized |= 0x004;
                } else if (element.usage == 3) {
                    synthesized |= 0x010;
                } else if (element.usage == 4) {
                    synthesized |= 0x020;
                } else if (element.usage == 10) {
                    synthesized |= (element.usage_index == 0) ? 0x040u : 0x080u;
                } else if (element.usage == 5) {
                    tex_count = std::max(tex_count, element.usage_index + 1);
                }
            }
            if ((synthesized & 0x006) == 0) {
                synthesized |= 0x002;
            }
            synthesized |= (tex_count & 0xF) << 8;
            ff.fvf_flags = synthesized;

            // A programmable VS binds declaration elements to the dcl vn
            // register (Learn: Input Register). Those attribute indices must
            // stay on the Metal descriptor. Sequential rebuild is only for
            // generated FF vertex shaders.
            if (!programmable_vs) {
                // Fixed-function declaration shaders use canonical attribute
                // locations, but declaration elements may appear in any order.
                // Preserve usage indices explicitly so POSITION1/NORMAL1 cannot
                // alias their index-0 counterparts during D3DVBF_TWEENING.
                attributes.clear();
                u32 location = 0;
                const auto append_semantic = [&](u32 usage, u32 usage_index) {
                    const FvfElement* element =
                        find_fvf_element(snap.elements, usage, usage_index);
                    if (element == nullptr) return false;
                    attributes.push_back({location++, element->offset,
                                          element->format, element->stream});
                    return true;
                };
                // Learn D3DDECLUSAGE_POSITIONT: transformed screen-space
                // position. The FF rebuild used to require POSITION0 and
                // silently drop the draw when the declaration only had
                // POSITIONT — typical 2D UI quads never reached the RT.
                if ((synthesized & D3DFVF_XYZRHW) != 0) {
                    if (!append_semantic(9, 0)) return;
                } else if (!append_semantic(0, 0)) {
                    return;
                }
                if ((synthesized & D3DFVF_XYZRHW) == 0 && ff.vertex_tweening &&
                    !append_semantic(0, 1)) {
                    return;
                }
                if ((synthesized & D3DFVF_NORMAL) != 0) {
                    if (!append_semantic(3, 0)) return;
                    if (ff.vertex_tweening && !append_semantic(3, 1)) return;
                }
                if ((synthesized & D3DFVF_PSIZE) != 0 &&
                    !append_semantic(4, 0)) return;
                if ((synthesized & D3DFVF_DIFFUSE) != 0 &&
                    !append_semantic(10, 0)) return;
                if ((synthesized & D3DFVF_SPECULAR) != 0 &&
                    !append_semantic(10, 1)) return;
                for (u32 i = 0; i < tex_count; ++i) {
                    if (!append_semantic(5, i)) return;
                }
            }
        }
    }

    const auto state_value = [&](u32 which) {
        const auto found = snap.tables->render_states.find(which);
        return found != snap.tables->render_states.end()
                   ? found->second
                   : d3d9_default_render_state(which, snap.auto_depth_stencil);
    };
    const bool flat_shading = state_value(D3DRS_SHADEMODE) == 1u;
    const bool point_size_output = snap.primitive_type == D3DPT_POINTLIST;
    const u32 clip_plane_mask =
        state_value(D3DRS_CLIPPING) != 0
            ? state_value(D3DRS_CLIPPLANEENABLE) & 0x3Fu
            : 0u;
    const bool clip_planes_enabled = clip_plane_mask != 0;
    const auto state_float = [&](u32 which) {
        const u32 bits = state_value(which);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const float point_minimum =
        std::max(0.0f, state_float(D3DRS_POINTSIZE_MIN));
    const float point_maximum = std::min(
        256.0f, std::max(0.0f, state_float(D3DRS_POINTSIZE_MAX)));
    const float point_size = std::max(
        point_minimum,
        std::min(state_float(D3DRS_POINTSIZE), point_maximum));
    const bool use_vertex_fog_factor =
        programmable_vs && vertex_shader->writes_fog &&
        state_value(D3DRS_FOGENABLE) != 0 &&
        state_value(D3DRS_FOGTABLEMODE) == 0;
    if (use_vertex_fog_factor) {
        ff.fog_mode = graphics::FogMode::Linear;
    }

    const auto sampler_lod_bias = [&](u32 slot, bool vertex) {
        const auto& states = vertex ? snap.tables->vertex_sampler_states[slot]
                                    : snap.tables->sampler_states[slot];
        const auto found = states.find(D3DSAMP_MIPMAPLODBIAS);
        const u32 bits = found != states.end()
                             ? found->second
                             : d3d9_default_sampler_state(
                                   D3DSAMP_MIPMAPLODBIAS);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return std::pair{bits, value};
    };
    std::array<float, graphics::SM3_MAX_SAMPLERS> pixel_lod_biases{};
    std::array<float, graphics::SM3_MAX_SAMPLERS> vertex_lod_biases{};
    for (u32 slot = 0; slot < graphics::SM3_MAX_SAMPLERS; ++slot) {
        pixel_lod_biases[slot] = sampler_lod_bias(slot, false).second;
        if (slot < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT) {
            vertex_lod_biases[slot] = sampler_lod_bias(slot, true).second;
        }
    }

    if (programmable_pair) {
        shader_hash = snap.vertex_shader;
        shader_hash = (shader_hash << 32) ^ snap.pixel_shader;
    } else if (programmable_vs) {
        shader_hash = snap.vertex_shader;
        shader_hash = (shader_hash << 32) ^ ff.hash();
    } else if (programmable_ps) {
        shader_hash = snap.pixel_shader;
        shader_hash = (shader_hash << 32) ^ ff.hash();
    } else {
        shader_hash = ff.hash();
    }

    const auto render_state = [&](u32 which) { return state_value(which); };
    const bool blend_enabled = render_state(D3DRS_ALPHABLENDENABLE) != 0;
    const u32 source_blend_state = render_state(D3DRS_SRCBLEND);
    const u32 destination_blend_state = render_state(D3DRS_DESTBLEND);
    // D3DBLEND_BOTHSRCALPHA and BOTHINVSRCALPHA are legacy source-state
    // shorthands which also override D3DRS_DESTBLEND.  Canonicalize them
    // before creating the backend pipeline so the paired Windows contract is
    // preserved without teaching the shared backend D3D9-only values.
    u32 source_blend = source_blend_state;
    u32 destination_blend = destination_blend_state;
    if (source_blend_state == 12u) { // D3DBLEND_BOTHSRCALPHA
        source_blend = 5u;           // D3DBLEND_SRCALPHA
        destination_blend = 6u;      // D3DBLEND_INVSRCALPHA
    } else if (source_blend_state == 13u) { // D3DBLEND_BOTHINVSRCALPHA
        source_blend = 6u;                 // D3DBLEND_INVSRCALPHA
        destination_blend = 5u;            // D3DBLEND_SRCALPHA
    }
    const u32 color_blend_op = render_state(D3DRS_BLENDOP);
    const bool separate_alpha_blend =
        render_state(D3DRS_SEPARATEALPHABLENDENABLE) != 0;
    const u32 source_blend_alpha = separate_alpha_blend
                                       ? render_state(D3DRS_SRCBLENDALPHA)
                                       : source_blend;
    const u32 destination_blend_alpha =
        separate_alpha_blend ? render_state(D3DRS_DESTBLENDALPHA)
                             : destination_blend;
    const u32 alpha_blend_op = separate_alpha_blend
                                   ? render_state(D3DRS_BLENDOPALPHA)
                                   : color_blend_op;
    const std::array<u32, 4> color_writes{
        render_state(D3DRS_COLORWRITEENABLE),
        render_state(D3DRS_COLORWRITEENABLE1),
        render_state(D3DRS_COLORWRITEENABLE2),
        render_state(D3DRS_COLORWRITEENABLE3)};
    const bool has_depth = snap.to_back_buffer
                               ? snap.depth_stencil_surface != 0
                               : snap.rt_depth != nullptr;

    // VITREO_D3D9_DECL_AUDIT=1: report any declaration element that cannot
    // legally be fetched from the stream it names. Each of these makes the
    // shader read a neighbouring vertex's bytes or past the buffer, which
    // shows up as displaced vertices with correct topology. Reported once
    // per distinct violation so
    // a per-frame offender does not flood the log.
    static const bool decl_audit = std::getenv("VITREO_D3D9_DECL_AUDIT") != nullptr;
    if (decl_audit) {
        static std::set<u64> seen;
        for (const auto& element : snap.elements) {
            const u32 slot = element.stream;
            if (slot >= graphics::BACKEND_MAX_VERTEX_BUFFERS) continue;
            const auto& stream = snap.streams[slot];
            const u32 size = d3d9_vertex_format_size(element.format);
            const char* problem = nullptr;
            if (stream.buffer == 0) {
                problem = "element names an unbound stream";
            } else if (stream.stride == 0) {
                problem = "stream stride is 0";
            } else if (element.offset + size > stream.stride) {
                problem = "element runs past the vertex stride";
            }
            if (problem == nullptr) continue;
            const u64 key = (static_cast<u64>(shader_hash) << 24) ^
                            (static_cast<u64>(element.usage) << 16) ^
                            (static_cast<u64>(element.usage_index) << 12) ^
                            (static_cast<u64>(element.offset) << 4) ^ slot;
            if (!seen.insert(key).second) continue;
            std::fprintf(stderr,
                         "[decl-audit] %s: vs=0x%llx stream=%u usage=%u/%u "
                         "offset=%u size=%u stride=%u\n",
                         problem,
                         static_cast<unsigned long long>(shader_hash), slot,
                         element.usage, element.usage_index, element.offset,
                         size, stream.stride);
        }
    }

    std::array<bool, graphics::BACKEND_MAX_VERTEX_BUFFERS>
        vertex_buffer_per_instance{};
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS>
        vertex_buffer_step_rates{};
    unsigned instance_count = 1;
    constexpr u32 kStreamFreqMask =
        ~(D3DSTREAMSOURCE_INDEXEDDATA | D3DSTREAMSOURCE_INSTANCEDATA);
    for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
        const u32 frequency = snap.streams[slot].frequency;
        if ((frequency & D3DSTREAMSOURCE_INSTANCEDATA) != 0) {
            vertex_buffer_per_instance[slot] = true;
            vertex_buffer_step_rates[slot] =
                std::max(1u, frequency & kStreamFreqMask);
        }
    }
    if (snap.indexed) {
        const u32 frequency0 = snap.streams[0].frequency;
        if ((frequency0 & D3DSTREAMSOURCE_INDEXEDDATA) != 0) {
            const u32 count = frequency0 & kStreamFreqMask;
            if (count != 0) {
                instance_count = count;
            }
        }
    }

    u64 pipeline_key = shader_hash;
    const auto mix = [&pipeline_key](u64 value) {
        pipeline_key ^= value + 0x9E3779B97F4A7C15ULL + (pipeline_key << 6) +
                        (pipeline_key >> 2);
    };
    mix(blend_enabled ? 1u : 0u);
    mix(source_blend);
    mix(destination_blend);
    mix(color_blend_op);
    mix(separate_alpha_blend ? 1u : 0u);
    mix(source_blend_alpha);
    mix(destination_blend_alpha);
    mix(alpha_blend_op);
    for (const u32 color_write : color_writes) {
        mix(color_write);
    }
    mix(snap.color_format);
    for (u32 slot = 1; slot < snap.color_formats.size(); ++slot) {
        mix(snap.color_formats[slot]);
    }
    mix(has_depth ? 1u : 0u);
    const unsigned raster_samples = [&]() -> unsigned {
        if (snap.to_back_buffer) {
            return d3d9_raster_sample_count(snap.multi_sample_type,
                                            snap.multi_sample_quality,
                                            snap.backend);
        }
        if (snap.rt_color != nullptr && snap.rt_color->desc().sample_count > 1) {
            return snap.rt_color->desc().sample_count;
        }
        return 1;
    }();
    mix(raster_samples);
    const bool ps_alpha_test =
        programmable_ps && render_state(D3DRS_ALPHATESTENABLE) != 0;
    const u32 ps_alpha_func = render_state(D3DRS_ALPHAFUNC);
    const u32 ps_alpha_ref = render_state(D3DRS_ALPHAREF);
    if (ps_alpha_test) {
        mix(1u);
        mix(ps_alpha_func);
    }
    // sRGB conversion for a programmable pixel shader: the sampler-state read
    // decode and the render-state write encode both fold into the translated
    // shader, the same way alpha test and fog do.
    // Read the two states from the snapshot rather than from `ff`: a fully
    // programmable vs+ps pair never builds the fixed-function state at all.
    const u32 ps_srgb_texture_mask =
        programmable_ps ? d3d9_srgb_texture_mask(*snap.tables) : 0u;
    const bool ps_srgb_write =
        programmable_ps && state_value(D3DRS_SRGBWRITEENABLE) != 0;
    if (ps_srgb_texture_mask != 0) {
        mix(7u);
        mix(ps_srgb_texture_mask);
    }
    if (ps_srgb_write) {
        mix(8u);
    }
    const bool ps_vertex_fog =
        programmable_ps && programmable_vs &&
        render_state(D3DRS_FOGENABLE) != 0 &&
        render_state(D3DRS_FOGTABLEMODE) == 0 && vertex_shader->writes_fog;
    const bool ps_table_fog =
        programmable_ps && render_state(D3DRS_FOGENABLE) != 0 &&
        render_state(D3DRS_FOGTABLEMODE) != 0;
    if (ps_vertex_fog) {
        mix(2u);
    }
    if (ps_table_fog) {
        mix(6u);
        mix(render_state(D3DRS_FOGTABLEMODE));
        mix(ff.table_fog_w ? 1u : 0u);
    }
    if (programmable_pair && flat_shading) {
        mix(3u);
    }
    if (programmable_pair && point_size_output) {
        mix(4u);
    }
    if (programmable_vs && clip_planes_enabled) {
        mix(5u);
    }
    if (programmable_ps) {
        for (u32 slot = 0; slot < pixel_shader->samplers_used.size(); ++slot) {
            if (pixel_shader->samplers_used[slot]) {
                mix(sampler_lod_bias(slot, false).first);
            }
        }
    }
    if (programmable_vs) {
        for (u32 slot = 0; slot < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT; ++slot) {
            if (vertex_shader->samplers_used[slot]) {
                mix(sampler_lod_bias(slot, true).first);
            }
        }
    }
    for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
        mix(snap.stream_layout_strides[slot]);
        mix(vertex_buffer_per_instance[slot] ? 1u : 0u);
        mix(vertex_buffer_step_rates[slot]);
    }
    for (const graphics::BackendVertexAttribute& attr : attributes) {
        mix(attr.shader_location);
        mix(attr.offset);
        mix(static_cast<u64>(attr.format));
        mix(attr.buffer_index);
    }

    std::array<u32, 8> sm1_stage_ttf{};
    bool sm1_projected = false;
    if (programmable_ps && pixel_shader->major == 1) {
        for (u32 stage = 0; stage < sm1_stage_ttf.size(); ++stage) {
            const auto& map = snap.tables->stage_states[stage];
            const auto found = map.find(D3DTSS_TEXTURETRANSFORMFLAGS);
            sm1_stage_ttf[stage] =
                found != map.end()
                    ? found->second
                    : d3d9_default_texture_stage_state(
                          stage, D3DTSS_TEXTURETRANSFORMFLAGS);
            mix(sm1_stage_ttf[stage]);
            if ((sm1_stage_ttf[stage] & 0x100u) != 0) {
                sm1_projected = true;
            }
        }
    }

    auto& queue = D3d9SubmitQueue::instance();
    graphics::BackendPipeline* pipeline = queue.cached_pipeline(pipeline_key);
    if (pipeline == nullptr || !pipeline->valid()) {
        graphics::BackendPipelineDesc desc{};
        const auto specialized_vs_msl = [&]() -> std::string {
            std::string msl = vertex_shader->msl;
            msl = graphics::apply_sm3_sampler_lod_bias(
                std::move(msl), vertex_lod_biases);
            if (flat_shading) {
                msl = graphics::apply_sm3_flat_shading(std::move(msl));
            }
            if (point_size_output) {
                msl = graphics::apply_sm3_point_size(
                    std::move(msl), vertex_shader->writes_point_size);
            }
            if (clip_planes_enabled) {
                msl = graphics::apply_sm3_clip_planes(std::move(msl));
            }
            return msl;
        };
        auto specialized_ps_msl = [&]() -> std::string {
            std::string msl = pixel_shader->msl;
            if (sm1_projected && snap.ps_bytecode != nullptr &&
                snap.ps_bytecode_size > 0) {
                graphics::Sm3Shader specialized =
                    graphics::translate_sm3_to_msl(
                        snap.ps_bytecode, snap.ps_bytecode_size,
                        sm1_stage_ttf.data(), sm1_stage_ttf.size());
                if (specialized.ok && !specialized.msl.empty()) {
                    msl = std::move(specialized.msl);
                }
            }
            msl = graphics::apply_sm3_sampler_lod_bias(
                std::move(msl), pixel_lod_biases);
            if (ps_table_fog) {
                msl = graphics::apply_sm3_table_fog(
                    std::move(msl), render_state(D3DRS_FOGTABLEMODE),
                    ff.table_fog_w);
            }
            if (ps_alpha_test) {
                msl = graphics::apply_sm3_alpha_test(std::move(msl),
                                                     ps_alpha_func);
            }
            if (ps_vertex_fog) {
                msl = graphics::apply_sm3_fog(std::move(msl), true);
            }
            if (flat_shading) {
                msl = graphics::apply_sm3_flat_shading(std::move(msl));
            }
            if (ps_srgb_texture_mask != 0) {
                msl = graphics::apply_sm3_srgb_texture(std::move(msl),
                                                       ps_srgb_texture_mask);
            }
            // Last: D3D9 converts the value on its way to the render target,
            // after everything else the pixel pipeline does.
            if (ps_srgb_write) {
                msl = graphics::apply_sm3_srgb_write(std::move(msl));
            }
            return msl;
        };
        if (programmable_pair) {
            desc.vertex_shader_msl = specialized_vs_msl();
            desc.fragment_shader_msl = specialized_ps_msl();
            desc.vertex_entry_point = vertex_shader->entry_point;
            desc.fragment_entry_point = pixel_shader->entry_point;
        } else if (programmable_vs) {
            desc.vertex_shader_msl = specialized_vs_msl();
            desc.vertex_entry_point = vertex_shader->entry_point;
            desc.fragment_shader_msl =
                graphics::generate_ff_fragment_for_sm3_varyings(
                    ff, vertex_shader->color_varyings,
                    vertex_shader->texcoord_varyings, use_vertex_fog_factor);
            desc.fragment_entry_point = "ff_fragment";
            if (desc.fragment_shader_msl.empty()) {
                VITREO_TRACE("[d3d9] Draw: the fixed-function fragment "
                             "companion produced no shader\n");
                return;
            }
        } else if (programmable_ps) {
            desc.vertex_shader_msl =
                graphics::generate_ff_vertex_for_sm3_varyings(
                    ff, pixel_shader->color_varyings,
                    pixel_shader->texcoord_varyings);
            desc.vertex_entry_point = "ff_vertex";
            desc.fragment_shader_msl = specialized_ps_msl();
            desc.fragment_entry_point = pixel_shader->entry_point;
            if (desc.vertex_shader_msl.empty()) {
                VITREO_TRACE("[d3d9] Draw: the fixed-function vertex "
                             "companion produced no shader\n");
                return;
            }
        } else {
            const graphics::FFGeneratedShaders shaders =
                graphics::generate_ff_shaders(ff);
            if (!shaders.valid()) {
                VITREO_TRACE("[d3d9] Draw: the fixed-function shader "
                             "generator produced no shader\n");
                return;
            }
            desc.vertex_shader_msl = shaders.vertex_msl;
            desc.fragment_shader_msl = shaders.fragment_msl;
            desc.vertex_entry_point = "ff_vertex";
            desc.fragment_entry_point = "ff_fragment";
        }
        desc.vertex_attributes = attributes;
        desc.vertex_stride = snap.stream_layout_strides[0];
        for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
            desc.vertex_buffer_strides[slot] = snap.stream_layout_strides[slot];
            desc.vertex_buffer_per_instance[slot] =
                vertex_buffer_per_instance[slot];
            desc.vertex_buffer_step_rates[slot] =
                vertex_buffer_step_rates[slot];
        }
        desc.color_format = d3d9_backend_format(snap.color_format);
        for (u32 slot = 0; slot < snap.color_formats.size(); ++slot) {
            if (snap.render_target_surfaces[slot] != 0) {
                desc.color_formats[slot] =
                    d3d9_backend_format(snap.color_formats[slot]);
            }
        }
        desc.depth_attachment = has_depth;
        desc.blend_enabled = blend_enabled;
        desc.source_blend = blend_factor(source_blend);
        desc.destination_blend = blend_factor(destination_blend);
        desc.blend_operation = blend_operation(color_blend_op);
        desc.source_blend_alpha = blend_factor(source_blend_alpha);
        desc.destination_blend_alpha =
            blend_factor(destination_blend_alpha);
        desc.blend_operation_alpha = blend_operation(alpha_blend_op);
        desc.color_write_mask = color_writes[0] & 0xF;
        for (u32 slot = 0; slot < color_writes.size(); ++slot) {
            desc.color_write_masks[slot] = color_writes[slot] & 0xF;
            if (programmable_ps &&
                !pixel_shader->color_outputs_written[slot]) {
                // Learn, Output Color Register: an MRT whose oCn has no
                // shader write must not be updated.  Keep this explicit in
                // the pipeline in addition to omitting the Metal result
                // member for statically absent secondary outputs.
                desc.color_write_masks[slot] = 0;
            }
        }
        desc.color_write_mask = desc.color_write_masks[0];
        desc.sample_count = raster_samples;
        desc.cache_key = pipeline_key;

        auto created = snap.backend->create_pipeline(desc);
        if (!created || !created->valid()) {
            VITREO_TRACE("[d3d9] Draw: the shared backend could not build a "
                         "pipeline for FVF 0x%08x\n",
                         snap.fvf);
            return;
        }
        pipeline = created.get();
        queue.store_pipeline(pipeline_key, std::move(created));
    }

    // execute_recorded_draw runs only on the serial D3D9 command-stream
    // worker. Keep its transient packing buffers on that worker so repeated
    // draws retain capacity instead of allocating two vectors per draw.
    static thread_local std::vector<u8> vertex_uniforms;
    static thread_local std::vector<u8> fragment_uniforms;
    vertex_uniforms.clear();
    fragment_uniforms.clear();
    const auto pack_shader_constants =
        [](const std::vector<float>& floats, u32 float_count,
           const std::vector<i32>& ints, u32 int_count,
           const std::vector<u32>& bools, u32 bool_count, std::vector<u8>& out) {
            const usize float_bytes =
                static_cast<usize>(std::max(float_count, 1u)) * 4 *
                sizeof(float);
            const usize int_bytes =
                static_cast<usize>(int_count) * 4 * sizeof(i32);
            const usize bool_bytes = static_cast<usize>(bool_count) * sizeof(i32);
            const usize used = float_bytes + int_bytes + bool_bytes;
            const usize total = (used + 15u) & ~static_cast<usize>(15u);
            out.assign(total, 0);
            if (!floats.empty()) {
                std::memcpy(out.data(), floats.data(),
                            std::min(float_bytes, floats.size() * sizeof(float)));
            }
            if (int_bytes > 0 && !ints.empty()) {
                std::memcpy(out.data() + float_bytes, ints.data(),
                            std::min(int_bytes, ints.size() * sizeof(i32)));
            }
            if (bool_bytes > 0) {
                i32* dest = reinterpret_cast<i32*>(out.data() + float_bytes +
                                                   int_bytes);
                const u32 n = std::min<u32>(
                    bool_count, static_cast<u32>(bools.size()));
                for (u32 i = 0; i < n; ++i) {
                    dest[i] = bools[i] != 0 ? 1 : 0;
                }
            }
        };
    if (programmable_vs) {
        pack_shader_constants(snap.vs_const_f,
                              vertex_shader->float_constant_count,
                              snap.vs_const_i,
                              vertex_shader->int_constant_count, snap.vs_const_b,
                              vertex_shader->bool_constant_count,
                              vertex_uniforms);
        if (vertex_shader->needs_viewport) {
            const float viewport[4] = {
                static_cast<float>(snap.viewport[2]),
                static_cast<float>(snap.viewport[3]), 0.0f, 0.0f};
            const usize offset = vertex_uniforms.size();
            vertex_uniforms.resize(offset + sizeof(viewport));
            std::memcpy(vertex_uniforms.data() + offset, viewport,
                        sizeof(viewport));
        }
        if (point_size_output) {
            const float params[4] = {point_size, point_minimum, point_maximum,
                                     0.0f};
            const usize offset = vertex_uniforms.size();
            vertex_uniforms.resize(offset + sizeof(params));
            std::memcpy(vertex_uniforms.data() + offset, params,
                        sizeof(params));
        }
        if (clip_planes_enabled) {
            for (u32 i = 0; i < D3D9_MAX_CLIP_PLANES; ++i) {
                const float* plane = (clip_plane_mask & (1u << i)) != 0
                                         ? snap.clip_planes[i].data()
                                         : nullptr;
                const float values[4] = {
                    plane != nullptr ? plane[0] : 0.0f,
                    plane != nullptr ? plane[1] : 0.0f,
                    plane != nullptr ? plane[2] : 0.0f,
                    plane != nullptr ? plane[3] : 0.0f};
                const usize offset = vertex_uniforms.size();
                vertex_uniforms.resize(offset + sizeof(values));
                std::memcpy(vertex_uniforms.data() + offset, values,
                            sizeof(values));
            }
        }
    } else {
        pack_vertex_uniforms(snap, ff, vertex_uniforms);
    }
    if (programmable_ps) {
        pack_shader_constants(snap.ps_const_f,
                              pixel_shader->float_constant_count,
                              snap.ps_const_i,
                              pixel_shader->int_constant_count, snap.ps_const_b,
                              pixel_shader->bool_constant_count,
                              fragment_uniforms);
        if (ps_table_fog) {
            const float params[4] = {
                ff.fog_start, ff.fog_end, ff.fog_density, 0.0f};
            const auto fog = color_to_rgba(render_state(D3DRS_FOGCOLOR));
            usize offset = fragment_uniforms.size();
            fragment_uniforms.resize(offset + sizeof(params));
            std::memcpy(fragment_uniforms.data() + offset, params,
                        sizeof(params));
            offset = fragment_uniforms.size();
            fragment_uniforms.resize(offset + sizeof(fog));
            std::memcpy(fragment_uniforms.data() + offset, fog.data(),
                        sizeof(fog));
        }
        if (ps_alpha_test && ps_alpha_func != 8u && ps_alpha_func != 0u) {
            const float extra[4] = {
                static_cast<float>(ps_alpha_ref & 0xFFu) / 255.0f, 0.0f, 0.0f,
                0.0f};
            const usize offset = fragment_uniforms.size();
            fragment_uniforms.resize(offset + sizeof(extra));
            std::memcpy(fragment_uniforms.data() + offset, extra, sizeof(extra));
        }
        if (ps_vertex_fog) {
            const auto fog = color_to_rgba(render_state(D3DRS_FOGCOLOR));
            const usize offset = fragment_uniforms.size();
            fragment_uniforms.resize(offset + sizeof(fog));
            std::memcpy(fragment_uniforms.data() + offset, fog.data(),
                        sizeof(fog));
        }
    } else {
        pack_fragment_uniforms(snap, ff, fragment_uniforms);
    }

    graphics::BackendDrawCall call{};
    call.pipeline = pipeline;
    switch (snap.primitive_type) {
    case D3DPT_POINTLIST:
        call.topology = graphics::BackendPrimitiveTopology::PointList;
        break;
    case D3DPT_LINELIST:
        call.topology = graphics::BackendPrimitiveTopology::LineList;
        break;
    case D3DPT_LINESTRIP:
        call.topology = graphics::BackendPrimitiveTopology::LineStrip;
        break;
    case D3DPT_TRIANGLESTRIP:
        call.topology = graphics::BackendPrimitiveTopology::TriangleStrip;
        break;
    default:
        call.topology = graphics::BackendPrimitiveTopology::TriangleList;
        break;
    }
    call.vertex_buffer = snap.streams[0].backend;
    call.vertex_buffer_offset = snap.streams[0].offset;
    for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
        if (!snap.stream_used[slot] || snap.streams[slot].backend == nullptr) {
            continue;
        }
        call.vertex_buffers[slot] = snap.streams[slot].backend;
        call.vertex_buffer_offsets[slot] = snap.streams[slot].offset;
    }
    if (snap.indexed) {
        call.index_buffer = snap.index_backend;
        call.index_32bit = snap.index_format == D3DFMT_INDEX32;
        call.index_buffer_offset =
            snap.index_start * (call.index_32bit ? 4u : 2u);
        call.index_count = snap.element_count;
        call.base_vertex = snap.base_vertex;
    } else {
        call.first_vertex = snap.first_vertex;
        call.vertex_count = snap.element_count;
    }
    call.instance_count = instance_count;
    call.vertex_constants = vertex_uniforms.data();
    call.vertex_constants_size = vertex_uniforms.size();
    call.fragment_constants = fragment_uniforms.data();
    call.fragment_constants_size = fragment_uniforms.size();
    call.blend_color = color_to_rgba(render_state(D3DRS_BLENDFACTOR));
    // Direct3D 9 defines both states as floating-point values whose bits are
    // passed through SetRenderState.  The shared backend owns the matching
    // post-clip triangle depth adjustment; points and lines are excluded by
    // the native rasterizer contract.
    call.depth_bias = state_float(D3DRS_DEPTHBIAS);
    call.depth_bias_slope_scale =
        state_float(D3DRS_SLOPESCALEDEPTHBIAS);

    const u32 texture_slots =
        programmable_ps ? graphics::BACKEND_MAX_TEXTURE_SLOTS
                        : std::min(ff.active_texture_stages,
                                   graphics::BACKEND_MAX_TEXTURE_SLOTS);
    for (u32 slot = 0; slot < texture_slots; ++slot) {
        if (programmable_ps && !pixel_shader->samplers_used[slot]) {
            continue;
        }
        const D3d9DrawSnapshot::TextureSnap& tex = snap.textures[slot];
        if (tex.tex2d != nullptr) {
            call.textures[slot] = tex.tex2d;
        } else if (tex.tex3d != nullptr) {
            call.textures_3d[slot] = tex.tex3d;
        } else {
            if (tex.missing_backing) {
                static u32 missing_backing = 0;
                ++missing_backing;
                if (missing_backing <= 8 || (missing_backing & 0x3FFu) == 0) {
                    VITREO_TRACE("[d3d9] Draw: stage %u has texture 0x%08x "
                                 "with no shared-core backing #%u — sampling "
                                 "opaque white\n",
                                 slot, tex.com_id, missing_backing);
                }
            }
            if (snap.white_texture == nullptr) {
                continue;
            }
            call.textures[slot] = snap.white_texture;
        }

        const auto sampler_state = [&](u32 which) {
            const auto& map = snap.tables->sampler_states[slot];
            const auto found = map.find(which);
            return found != map.end() ? found->second
                                      : d3d9_default_sampler_state(which);
        };
        graphics::BackendSamplerDesc& sampler = call.samplers[slot];
        sampler.min_filter = texture_filter(sampler_state(D3DSAMP_MINFILTER));
        sampler.mag_filter = texture_filter(sampler_state(D3DSAMP_MAGFILTER));
        const u32 mip_texf = sampler_state(D3DSAMP_MIPFILTER);
        sampler.mipmaps_enabled = mip_texf != 0;
        sampler.mip_filter = texture_filter(mip_texf);
        sampler.address_u = address_mode(sampler_state(D3DSAMP_ADDRESSU));
        sampler.address_v = address_mode(sampler_state(D3DSAMP_ADDRESSV));
        sampler.address_w = address_mode(sampler_state(D3DSAMP_ADDRESSW));
        sampler.border_color = sampler_state(D3DSAMP_BORDERCOLOR);
        sampler.max_anisotropy =
            std::max(1u, sampler_state(D3DSAMP_MAXANISOTROPY));
        const u32 lod_bias_bits = sampler_state(D3DSAMP_MIPMAPLODBIAS);
        std::memcpy(&sampler.lod_bias, &lod_bias_bits,
                    sizeof(sampler.lod_bias));
        // D3DSAMP_MAXMIPLEVEL names the most *detailed* mip the sampler may
        // use (0 = the whole chain), which is a minimum LOD clamp. Ignoring it
        // samples sharper levels than the guest allowed.
        sampler.lod_min_clamp =
            static_cast<float>(sampler_state(D3DSAMP_MAXMIPLEVEL));
    }
    if (programmable_vs) {
        for (u32 slot = 0;
             slot < D3D9_VERTEX_TEXTURE_SAMPLER_COUNT; ++slot) {
            if (!vertex_shader->samplers_used[slot]) continue;
            const D3d9DrawSnapshot::TextureSnap& tex =
                snap.vertex_textures[slot];
            if (tex.tex2d != nullptr) {
                call.vertex_textures[slot] = tex.tex2d;
            } else if (tex.tex3d != nullptr) {
                call.vertex_textures_3d[slot] = tex.tex3d;
            } else if (snap.white_texture != nullptr) {
                call.vertex_textures[slot] = snap.white_texture;
            }

            const auto sampler_state = [&](u32 which) {
                const auto& map = snap.tables->vertex_sampler_states[slot];
                const auto found = map.find(which);
                return found != map.end()
                           ? found->second
                           : d3d9_default_sampler_state(which);
            };
            graphics::BackendSamplerDesc& sampler =
                call.vertex_samplers[slot];
            sampler.min_filter =
                texture_filter(sampler_state(D3DSAMP_MINFILTER));
            sampler.mag_filter =
                texture_filter(sampler_state(D3DSAMP_MAGFILTER));
            const u32 mip_texf = sampler_state(D3DSAMP_MIPFILTER);
            sampler.mipmaps_enabled = mip_texf != 0;
            sampler.mip_filter = texture_filter(mip_texf);
            sampler.address_u =
                address_mode(sampler_state(D3DSAMP_ADDRESSU));
            sampler.address_v =
                address_mode(sampler_state(D3DSAMP_ADDRESSV));
            sampler.address_w =
                address_mode(sampler_state(D3DSAMP_ADDRESSW));
            sampler.border_color = sampler_state(D3DSAMP_BORDERCOLOR);
            // Learn: anisotropic filtering is not supported for vertex
            // samplers; MAXANISOTROPY is ignored.
            sampler.max_anisotropy = 1;
            const u32 lod_bias_bits =
                sampler_state(D3DSAMP_MIPMAPLODBIAS);
            std::memcpy(&sampler.lod_bias, &lod_bias_bits,
                        sizeof(sampler.lod_bias));
        }
    }

    float min_z = 0.0f;
    float max_z = 1.0f;
    std::memcpy(&min_z, &snap.viewport[4], sizeof(min_z));
    std::memcpy(&max_z, &snap.viewport[5], sizeof(max_z));
    const float vp_x = static_cast<float>(snap.viewport[0]);
    const float vp_y = static_cast<float>(snap.viewport[1]);
    float vp_w = static_cast<float>(snap.viewport[2]);
    float vp_h = static_cast<float>(snap.viewport[3]);
    const float rt_w_f = static_cast<float>(snap.color_width);
    const float rt_h_f = static_cast<float>(snap.color_height);
    if (vp_x + vp_w > rt_w_f) vp_w = std::max(0.0f, rt_w_f - vp_x);
    if (vp_y + vp_h > rt_h_f) vp_h = std::max(0.0f, rt_h_f - vp_y);
    call.viewport = {vp_x, vp_y, vp_w, vp_h, min_z, max_z};
    call.scissor_enabled = render_state(D3DRS_SCISSORTESTENABLE) != 0;
    if (call.scissor_enabled) {
        call.scissor = snap.scissor;
        if (call.scissor[2] > static_cast<i32>(snap.color_width)) {
            call.scissor[2] = static_cast<i32>(snap.color_width);
        }
        if (call.scissor[3] > static_cast<i32>(snap.color_height)) {
            call.scissor[3] = static_cast<i32>(snap.color_height);
        }
    } else {
        call.scissor = {0, 0, static_cast<i32>(snap.color_width),
                        static_cast<i32>(snap.color_height)};
    }

    const auto cull = translate_d3d9_cull_mode(render_state(D3DRS_CULLMODE));
    call.cull_mode = cull.cull_mode;
    call.front_counter_clockwise = cull.front_counter_clockwise;
    call.depth_test_enabled = has_depth && render_state(D3DRS_ZENABLE) != 0;
    call.depth_write_enabled =
        call.depth_test_enabled && render_state(D3DRS_ZWRITEENABLE) != 0;
    call.depth_compare = compare_function(render_state(D3DRS_ZFUNC));

    if (!snap.render_pass->draw(call)) {
        VITREO_TRACE("[d3d9] Draw: the shared backend rejected the draw "
                     "call (fvf=0x%08x elements=%u)\n",
                     snap.fvf, snap.element_count);
    }
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_draw_ff_state.cpp
/// @brief D3D9 fixed-function state translation and uniform packing: FVF to
///        vertex layout, device state to `FFPipelineState`, and the byte
///        layout handed to the generated shaders.

#include "winapi/graphics/d3d9/d3d9_draw_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"
#include "winapi/graphics/d3d9/d3d9_cs.hpp"

#include "graphics/shaders/ff_emulation.hpp"
#include "graphics/shaders/ff_uniforms.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

using namespace d3d9_draw_detail;

namespace {

/// D3DFVF_TEXCOORDSIZE* — two bits per set starting at bit 16.
[[nodiscard]] u32 fvf_texcoord_components(u32 fvf, u32 index) {
    const u32 fmt = (fvf >> (16u + index * 2u)) & 0x3u;
    switch (fmt) {
    case 1:  return 3; // D3DFVF_TEXTUREFORMAT3
    case 2:  return 4; // D3DFVF_TEXTUREFORMAT4
    case 3:  return 1; // D3DFVF_TEXTUREFORMAT1
    default: return 2; // D3DFVF_TEXTUREFORMAT2 (0)
    }
}

[[nodiscard]] graphics::BackendVertexFormat fvf_texcoord_format(u32 components) {
    switch (components) {
    case 1:  return graphics::BackendVertexFormat::Float1;
    case 3:  return graphics::BackendVertexFormat::Float3;
    case 4:  return graphics::BackendVertexFormat::Float4;
    default: return graphics::BackendVertexFormat::Float2;
    }
}

} // namespace

// =============================================================================
// FVF → vertex layout
// =============================================================================

bool D3D9Manager::enumerate_fvf_elements(u32 fvf,
                                         std::vector<FvfElement>& elements,
                                         u32& stride,
                                         const char*& rejection) {
    elements.clear();
    stride = 0;
    if ((fvf & 0x8000u /* D3DFVF_LASTBETA_D3DCOLOR */) != 0) {
        rejection = "D3DFVF_LASTBETA_D3DCOLOR is not supported";
        return false;
    }

    // D3DDECLUSAGE values, which are how a programmable vertex shader names
    // the elements an FVF describes.
    constexpr u32 USAGE_POSITION = 0;
    constexpr u32 USAGE_BLENDWEIGHT = 1;
    constexpr u32 USAGE_BLENDINDICES = 2;
    constexpr u32 USAGE_NORMAL = 3;
    constexpr u32 USAGE_PSIZE = 4;
    constexpr u32 USAGE_TEXCOORD = 5;
    constexpr u32 USAGE_POSITIONT = 9;
    constexpr u32 USAGE_COLOR = 10;

    const u32 position = fvf & D3DFVF_POSITION_MASK;
    if (position == D3DFVF_XYZ ||
        (position >= D3DFVF_XYZB1 && position <= D3DFVF_XYZB5)) {
        elements.push_back({USAGE_POSITION, 0, stride,
                            graphics::BackendVertexFormat::Float3});
        stride += 12;
        if (position >= D3DFVF_XYZB1 && position <= D3DFVF_XYZB5) {
            const u32 beta_count = (position - D3DFVF_XYZB1) / 2u + 1u;
            const bool packed_indices =
                (fvf & D3DFVF_LASTBETA_UBYTE4) != 0;
            if (position == D3DFVF_XYZB5 && !packed_indices) {
                rejection = "D3DFVF_XYZB5 without packed blend indices";
                return false;
            }
            const u32 weight_count = beta_count - (packed_indices ? 1u : 0u);
            const graphics::BackendVertexFormat format =
                weight_count == 1 ? graphics::BackendVertexFormat::Float1
                : weight_count == 2 ? graphics::BackendVertexFormat::Float2
                : weight_count == 3 ? graphics::BackendVertexFormat::Float3
                                    : graphics::BackendVertexFormat::Float4;
            if (weight_count != 0) {
                elements.push_back({USAGE_BLENDWEIGHT, 0, stride, format});
                stride += weight_count * 4u;
            }
            if (packed_indices) {
                elements.push_back({USAGE_BLENDINDICES, 0, stride,
                                    graphics::BackendVertexFormat::UByte4});
                stride += 4u;
            }
        }
    } else if (position == D3DFVF_XYZRHW) {
        // x, y, z, and RHW; the shader maps it out of screen space itself.
        elements.push_back({USAGE_POSITIONT, 0, stride,
                            graphics::BackendVertexFormat::Float4});
        stride += 16;
    } else {
        rejection = "an unsupported D3DFVF position format";
        return false;
    }

    if ((fvf & D3DFVF_NORMAL) != 0) {
        elements.push_back({USAGE_NORMAL, 0, stride,
                            graphics::BackendVertexFormat::Float3});
        stride += 12;
    }
    if ((fvf & D3DFVF_PSIZE) != 0) {
        elements.push_back({USAGE_PSIZE, 0, stride,
                            graphics::BackendVertexFormat::Float1});
        stride += 4;
    }
    if ((fvf & D3DFVF_DIFFUSE) != 0) {
        elements.push_back({USAGE_COLOR, 0, stride,
                            graphics::BackendVertexFormat::UByte4NormalizedBGRA});
        stride += 4;
    }
    if ((fvf & D3DFVF_SPECULAR) != 0) {
        elements.push_back({USAGE_COLOR, 1, stride,
                            graphics::BackendVertexFormat::UByte4NormalizedBGRA});
        stride += 4;
    }

    const u32 texcoord_count =
        (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (texcoord_count > 8) {
        rejection = "more than eight texture coordinate sets";
        return false;
    }
    for (u32 i = 0; i < texcoord_count; ++i) {
        const u32 components = fvf_texcoord_components(fvf, i);
        elements.push_back({USAGE_TEXCOORD, i, stride,
                            fvf_texcoord_format(components)});
        stride += components * 4u;
    }
    return true;
}

bool D3D9Manager::build_vertex_layout(u32 fvf,
                                      std::vector<graphics::BackendVertexAttribute>&
                                          attributes,
                                      u32& stride,
                                      const char*& rejection) {
    std::vector<FvfElement> elements;
    if (!enumerate_fvf_elements(fvf, elements, stride, rejection)) {
        return false;
    }
    attributes.clear();
    // The generated fixed-function shader numbers its attributes in FVF order.
    unsigned location = 0;
    for (const FvfElement& element : elements) {
        attributes.push_back({location++, element.offset, element.format});
    }
    return true;
}

// =============================================================================
// Device state → shared-core fixed-function state
// =============================================================================

bool D3D9Manager::build_ff_state(const D3d9DrawSnapshot& snap,
                                 graphics::FFPipelineState& ff) const {
    if (snap.tables == nullptr) {
        return false;
    }
    const auto render_state = [&](u32 which) {
        const auto found = snap.tables->render_states.find(which);
        if (found != snap.tables->render_states.end()) return found->second;
        return d3d9_default_render_state(which, snap.auto_depth_stencil);
    };
    const auto render_state_float = [&](u32 which) {
        const u32 bits = render_state(which);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    const auto stage_state = [&](u32 stage, u32 which) {
        const auto& map = snap.tables->stage_states[stage];
        const auto found = map.find(which);
        if (found != map.end()) return found->second;
        return d3d9_default_texture_stage_state(stage, which);
    };

    ff.fvf_flags = snap.fvf;
    ff.flat_shading = render_state(D3DRS_SHADEMODE) == 1u;
    ff.point_size_output = snap.primitive_type == D3DPT_POINTLIST;
    ff.point_scale_enabled =
        ff.point_size_output &&
        (ff.fvf_flags & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW &&
        render_state(D3DRS_POINTSCALEENABLE) != 0;
    ff.clip_planes_enabled =
        render_state(D3DRS_CLIPPING) != 0 &&
        (render_state(D3DRS_CLIPPLANEENABLE) & 0x3Fu) != 0;
    ff.vertex_blend_weight_count = 0;
    ff.indexed_vertex_blend = false;
    ff.vertex_tweening = false;
    ff.tween_factor = render_state_float(170 /* D3DRS_TWEENFACTOR */);
    const u32 fvf_position = ff.fvf_flags & D3DFVF_POSITION_MASK;
    const bool packed_blend_indices =
        (ff.fvf_flags & D3DFVF_LASTBETA_UBYTE4) != 0;
    const u32 beta_count =
        fvf_position >= D3DFVF_XYZB1 && fvf_position <= D3DFVF_XYZB5
            ? (fvf_position - D3DFVF_XYZB1) / 2u + 1u : 0u;
    const u32 available_weights =
        beta_count - (packed_blend_indices && beta_count != 0 ? 1u : 0u);
    const u32 requested_weights =
        render_state(151 /* D3DRS_VERTEXBLEND */);
    if (requested_weights == 255u /* D3DVBF_TWEENING */) {
        const bool position0 = find_fvf_element(snap.elements, 0, 0) != nullptr;
        const bool position1 = find_fvf_element(snap.elements, 0, 1) != nullptr;
        const bool normal0 = find_fvf_element(snap.elements, 3, 0) != nullptr;
        const bool normal1 = find_fvf_element(snap.elements, 3, 1) != nullptr;
        if (!position0 || !position1 || normal0 != normal1) {
            return false;
        }
        ff.vertex_tweening = true;
    }
    if (requested_weights == 256u /* D3DVBF_0WEIGHTS */ &&
        packed_blend_indices &&
        render_state(167 /* D3DRS_INDEXEDVERTEXBLENDENABLE */) != 0) {
        ff.indexed_vertex_blend = true;
    }
    if (requested_weights >= 1u && requested_weights <= 3u &&
        available_weights >= requested_weights) {
        ff.vertex_blend_weight_count = requested_weights;
        ff.indexed_vertex_blend =
            packed_blend_indices &&
            render_state(167 /* D3DRS_INDEXEDVERTEXBLENDENABLE */) != 0;
    }
    ff.lighting_enabled = render_state(D3DRS_LIGHTING) != 0;
    ff.normalize_normals = render_state(D3DRS_NORMALIZENORMALS) != 0;
    ff.local_viewer = render_state(D3DRS_LOCALVIEWER) != 0;
    ff.specular_enable = render_state(D3DRS_SPECULARENABLE) != 0;
    ff.vertex_color_enable = render_state(D3DRS_COLORVERTEX) != 0;
    ff.diffuse_material_source = render_state(D3DRS_DIFFUSEMATERIALSOURCE);
    ff.ambient_material_source = render_state(D3DRS_AMBIENTMATERIALSOURCE);
    ff.specular_material_source = render_state(D3DRS_SPECULARMATERIALSOURCE);
    ff.emissive_material_source = render_state(D3DRS_EMISSIVEMATERIALSOURCE);

    const auto ambient = color_to_rgba(render_state(D3DRS_AMBIENT));
    std::copy(ambient.begin(), ambient.end(), ff.global_ambient);

    // D3DMATERIAL9 is four D3DCOLORVALUEs (diffuse, ambient, specular,
    // emissive) followed by the specular power.
    const auto read_material_color = [&](usize offset, float* out) {
        std::memcpy(out, snap.material.data() + offset, sizeof(float) * 4);
    };
    read_material_color(0, ff.material.diffuse);
    read_material_color(16, ff.material.ambient);
    read_material_color(32, ff.material.specular);
    read_material_color(48, ff.material.emissive);
    std::memcpy(&ff.material.power, snap.material.data() + 64, sizeof(float));

    // Only lights that are both set and enabled participate, packed densely
    // because the generated shader indexes them 0..active_light_count-1.
    ff.active_light_count = 0;
    for (const auto& [index, light_bytes] : snap.tables->lights) {
        const auto enabled = snap.tables->light_enabled.find(index);
        if (enabled == snap.tables->light_enabled.end() || !enabled->second) continue;
        if (ff.active_light_count >= ff.lights.size()) break;

        graphics::FFLight& light = ff.lights[ff.active_light_count];
        u32 type = 0;
        std::memcpy(&type, light_bytes.data(), sizeof(type));
        light.type = static_cast<graphics::LightType>(type);
        light.enabled = true;
        std::memcpy(light.diffuse, light_bytes.data() + 4, sizeof(float) * 4);
        std::memcpy(light.specular, light_bytes.data() + 20, sizeof(float) * 4);
        std::memcpy(light.ambient, light_bytes.data() + 36, sizeof(float) * 4);
        std::memcpy(light.position, light_bytes.data() + 52, sizeof(float) * 3);
        std::memcpy(light.direction, light_bytes.data() + 64,
                    sizeof(float) * 3);
        std::memcpy(&light.range, light_bytes.data() + 76, sizeof(float));
        std::memcpy(&light.attenuation[0], light_bytes.data() + 84,
                    sizeof(float));
        std::memcpy(&light.attenuation[1], light_bytes.data() + 88,
                    sizeof(float));
        std::memcpy(&light.attenuation[2], light_bytes.data() + 92,
                    sizeof(float));
        std::memcpy(&light.spot_falloff, light_bytes.data() + 80,
                    sizeof(float));
        std::memcpy(&light.spot_theta, light_bytes.data() + 96,
                    sizeof(float));
        std::memcpy(&light.spot_phi, light_bytes.data() + 100, sizeof(float));
        ++ff.active_light_count;
    }

    ff.active_texture_stages = 0;
    for (u32 stage = 0; stage < D3D9_MAX_TEXTURE_STAGES; ++stage) {
        const u32 color_op = stage_state(stage, D3DTSS_COLOROP);
        if (color_op == 1 /* D3DTOP_DISABLE */) break;
        const u32 color_arg1 = stage_state(stage, D3DTSS_COLORARG1);
        // Microsoft documents a fixed-function cascade termination rule:
        // when COLORARG1 selects D3DTA_TEXTURE and the stage has no texture,
        // that stage and every later stage are not processed. Binding a white
        // fallback and continuing is only accidentally equivalent for a
        // narrow subset of operations.
        if ((color_arg1 & 0xFu) == 2u /* D3DTA_TEXTURE */ &&
            snap.texture_ids[stage] == 0) {
            break;
        }
        graphics::TextureStageState& target = ff.texture_stages[stage];
        target.color_op = static_cast<graphics::TexStageOp>(color_op);
        // Keep D3DTA_COMPLEMENT / D3DTA_ALPHAREPLICATE — stripping to & 0xF
        // made lightmap modulate ignore 1-tex / alpha-replicate args.
        target.color_arg0 = stage_state(stage, D3DTSS_COLORARG0);
        target.color_arg1 = color_arg1;
        target.color_arg2 = stage_state(stage, D3DTSS_COLORARG2);
        target.alpha_op = static_cast<graphics::TexStageOp>(
            stage_state(stage, D3DTSS_ALPHAOP));
        target.alpha_arg0 = stage_state(stage, D3DTSS_ALPHAARG0);
        target.alpha_arg1 = stage_state(stage, D3DTSS_ALPHAARG1);
        target.alpha_arg2 = stage_state(stage, D3DTSS_ALPHAARG2);
        target.result_arg = stage_state(stage, D3DTSS_RESULTARG);
        // Keep TCI_* high bits for FF camera-space texgen.
        target.texcoord_index = stage_state(stage, D3DTSS_TEXCOORDINDEX);
        target.texture_transform_flags =
            stage_state(stage, D3DTSS_TEXTURETRANSFORMFLAGS);
        target.bump_env_mat00 = stage_state(stage, D3DTSS_BUMPENVMAT00);
        target.bump_env_mat01 = stage_state(stage, D3DTSS_BUMPENVMAT01);
        target.bump_env_mat10 = stage_state(stage, D3DTSS_BUMPENVMAT10);
        target.bump_env_mat11 = stage_state(stage, D3DTSS_BUMPENVMAT11);
        target.bump_env_lscale = stage_state(stage, D3DTSS_BUMPENVLSCALE);
        target.bump_env_loffset = stage_state(stage, D3DTSS_BUMPENVLOFFSET);
        ++ff.active_texture_stages;
    }

    // Table fog wins over vertex fog when both are set, matching D3D9.
    ff.fog_mode = graphics::FogMode::None;
    if (render_state(D3DRS_FOGENABLE) != 0) {
        const u32 table_mode = render_state(D3DRS_FOGTABLEMODE);
        const u32 vertex_mode = render_state(D3DRS_FOGVERTEXMODE);
        const u32 mode = table_mode != 0 ? table_mode : vertex_mode;
        if (mode != 0) ff.fog_mode = static_cast<graphics::FogMode>(mode);
        ff.table_fog = table_mode != 0;
        if (ff.table_fog) {
            const auto found = snap.tables->transforms.find(D3DTS_PROJECTION);
            const Matrix& projection =
                found != snap.tables->transforms.end() ? found->second : IDENTITY;
            const bool affine = projection[3] == 0.0f &&
                                projection[7] == 0.0f &&
                                projection[11] == 0.0f &&
                                projection[15] == 1.0f;
            ff.table_fog_w = !affine;
        }
    }
    ff.fog_start = render_state_float(D3DRS_FOGSTART);
    ff.fog_end = render_state_float(D3DRS_FOGEND);
    ff.fog_density = render_state_float(D3DRS_FOGDENSITY);
    ff.range_fog_enable = render_state(D3DRS_RANGEFOGENABLE) != 0;
    const auto fog_color = color_to_rgba(render_state(D3DRS_FOGCOLOR));
    std::copy(fog_color.begin(), fog_color.end(), ff.fog_color);

    ff.alpha_test_enable = render_state(D3DRS_ALPHATESTENABLE) != 0;
    ff.alpha_test_func = render_state(D3DRS_ALPHAFUNC);
    ff.alpha_ref = render_state(D3DRS_ALPHAREF);

    // sRGB conversion is a per-sampler read and a per-draw write; both are
    // states, not opcodes, so nothing else in the pipeline applies them.
    ff.srgb_texture_mask = d3d9_srgb_texture_mask(*snap.tables);
    ff.srgb_write_enable = render_state(D3DRS_SRGBWRITEENABLE) != 0;
    return true;
}

// =============================================================================
// Uniform packing
// =============================================================================
//
// The layouts below must mirror `generate_ff_shaders` field for field: the
// generated MSL declares its uniform structs conditionally on the same state,
// so a mismatch shifts every field after the first difference.

/// Translate the D3D9 state tables into the API-neutral inputs the shared
/// fixed-function uniform packer consumes. Everything D3D9-specific — default
/// render-state values, the transform/sampler/stage maps — stays here; the
/// byte layout belongs to `graphics/shaders/ff_uniforms`.
namespace {

[[nodiscard]] graphics::FFUniformInputs build_ff_uniform_inputs(
    const D3d9DrawSnapshot& snap, const graphics::FFPipelineState& ff) {
    graphics::FFUniformInputs inputs{};

    const auto transform = [&](u32 which) -> Matrix {
        const auto found = snap.tables->transforms.find(which);
        return found != snap.tables->transforms.end() ? found->second : IDENTITY;
    };
    for (u32 i = 0; i < graphics::FF_MAX_WORLD_MATRICES; ++i) {
        inputs.world_matrices[i] = transform(D3DTS_WORLD + i);
    }
    inputs.view = transform(D3DTS_VIEW);
    inputs.projection = transform(D3DTS_PROJECTION);
    for (u32 i = 0; i < graphics::FF_MAX_TEXTURE_STAGES; ++i) {
        inputs.texture_transforms[i] = transform(D3DTS_TEXTURE0 + i);
    }

    inputs.viewport_width = static_cast<float>(snap.viewport[2]);
    inputs.viewport_height = static_cast<float>(snap.viewport[3]);

    const auto render_state = [&](u32 which) {
        const auto found = snap.tables->render_states.find(which);
        if (found != snap.tables->render_states.end()) return found->second;
        return d3d9_default_render_state(which, snap.auto_depth_stencil);
    };
    const auto render_state_float = [&](u32 which) {
        const u32 bits = render_state(which);
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };
    inputs.point_size = render_state_float(D3DRS_POINTSIZE);
    inputs.point_size_min = render_state_float(D3DRS_POINTSIZE_MIN);
    inputs.point_size_max = render_state_float(D3DRS_POINTSIZE_MAX);
    inputs.point_scale_a = render_state_float(D3DRS_POINTSCALE_A);
    inputs.point_scale_b = render_state_float(D3DRS_POINTSCALE_B);
    inputs.point_scale_c = render_state_float(D3DRS_POINTSCALE_C);

    {
        const auto found = snap.tables->render_states.find(D3DRS_CLIPPLANEENABLE);
        inputs.clip_plane_enable_mask =
            found != snap.tables->render_states.end() ? found->second : 0;
    }
    static_assert(D3D9_MAX_CLIP_PLANES == graphics::FF_MAX_CLIP_PLANES,
                  "clip plane counts must agree with the shared packer");
    for (u32 i = 0; i < graphics::FF_MAX_CLIP_PLANES; ++i) {
        inputs.clip_planes[i] = snap.clip_planes[i];
    }

    inputs.texture_factor = color_to_rgba(render_state(D3DRS_TEXTUREFACTOR));
    static_assert(D3D9_MAX_TEXTURE_STAGES == graphics::FF_MAX_TEXTURE_STAGES,
                  "texture stage counts must agree with the shared packer");
    for (u32 stage = 0; stage < graphics::FF_MAX_TEXTURE_STAGES; ++stage) {
        const auto& states = snap.tables->stage_states[stage];
        const auto found = states.find(D3DTSS_CONSTANT);
        const u32 packed = found != states.end()
                               ? found->second
                               : d3d9_default_texture_stage_state(
                                     stage, D3DTSS_CONSTANT);
        inputs.stage_constants[stage] = color_to_rgba(packed);

        const auto& samplers = snap.tables->sampler_states[stage];
        const auto sampler_found = samplers.find(D3DSAMP_MIPMAPLODBIAS);
        const u32 bits = sampler_found != samplers.end()
                             ? sampler_found->second
                             : d3d9_default_sampler_state(D3DSAMP_MIPMAPLODBIAS);
        std::memcpy(&inputs.mipmap_lod_bias[stage], &bits, sizeof(float));
    }

    (void)ff;
    return inputs;
}

} // namespace

void D3D9Manager::pack_vertex_uniforms(const D3d9DrawSnapshot& snap,
                                       const graphics::FFPipelineState& ff,
                                       std::vector<u8>& out) const {
    graphics::pack_ff_vertex_uniforms(ff, build_ff_uniform_inputs(snap, ff),
                                      out);
}

void D3D9Manager::pack_fragment_uniforms(const D3d9DrawSnapshot& snap,
                                         const graphics::FFPipelineState& ff,
                                         std::vector<u8>& out) const {
    graphics::pack_ff_fragment_uniforms(ff, build_ff_uniform_inputs(snap, ff),
                                        out);
}

} // namespace vitreo::winapi

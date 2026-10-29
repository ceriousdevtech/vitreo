// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d7_state.cpp
/// @brief IDirect3DDevice7 fixed-function state → shared FFPipelineState.

#include "d3d7_state.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::d3d7 {

using graphics::FFLight;
using graphics::FFPipelineState;
using graphics::FogMode;
using graphics::TexStageOp;
using graphics::TextureStageState;

namespace {

/// D3DCOLOR is 0xAARRGGBB; the pipeline state carries straight RGBA floats.
std::array<float, 4> color_to_rgba(u32 color) {
    return {static_cast<float>((color >> 16) & 0xFF) / 255.0f,
            static_cast<float>((color >> 8) & 0xFF) / 255.0f,
            static_cast<float>(color & 0xFF) / 255.0f,
            static_cast<float>((color >> 24) & 0xFF) / 255.0f};
}

float bits_to_float(u32 bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

const Mat4& identity_storage() {
    static const Mat4 kIdentity = {1, 0, 0, 0,
                                   0, 1, 0, 0,
                                   0, 0, 1, 0,
                                   0, 0, 0, 1};
    return kIdentity;
}

} // namespace

Mat4 identity_matrix() { return identity_storage(); }

u32 default_render_state(u32 which) {
    switch (which) {
        // A device created against a render target with no attached z-buffer
        // still reports ZENABLE as D3DZB_TRUE; whether depth testing actually
        // happens is decided by the attachment, not by this default.
        case rs::ZENABLE: return 1;             // D3DZB_TRUE
        case rs::FILLMODE: return 3;            // D3DFILL_SOLID
        case rs::SHADEMODE: return 2;           // D3DSHADE_GOURAUD
        case rs::ZWRITEENABLE: return 1;
        case rs::ALPHATESTENABLE: return 0;
        case rs::LASTPIXEL: return 1;
        case rs::SRCBLEND: return 2;            // D3DBLEND_ONE
        case rs::DESTBLEND: return 1;           // D3DBLEND_ZERO
        case rs::CULLMODE: return 3;            // D3DCULL_CCW
        case rs::ZFUNC: return 4;               // D3DCMP_LESSEQUAL
        case rs::ALPHAREF: return 0;
        case rs::ALPHAFUNC: return 8;           // D3DCMP_ALWAYS
        case rs::DITHERENABLE: return 0;
        case rs::ALPHABLENDENABLE: return 0;
        case rs::FOGENABLE: return 0;
        // D3D7 powers up with specular highlights on; D3D8 flipped this
        // default to FALSE, so it cannot be inherited from the D3D9 tables.
        case rs::SPECULARENABLE: return 1;
        case rs::FOGCOLOR: return 0;
        case rs::FOGTABLEMODE: return 0;        // D3DFOG_NONE
        case rs::FOGVERTEXMODE: return 0;       // D3DFOG_NONE
        case rs::FOGSTART: return 0x00000000u;  // 0.0f
        case rs::FOGEND: return 0x3F800000u;    // 1.0f
        case rs::FOGDENSITY: return 0x3F800000u;
        case rs::RANGEFOGENABLE: return 0;
        case rs::EDGEANTIALIAS: return 0;
        case rs::COLORKEYENABLE: return 0;
        case rs::ZBIAS: return 0;
        case rs::STENCILENABLE: return 0;
        case rs::STENCILFAIL:
        case rs::STENCILZFAIL:
        case rs::STENCILPASS: return 1;         // D3DSTENCILOP_KEEP
        case rs::STENCILFUNC: return 8;         // D3DCMP_ALWAYS
        case rs::STENCILREF: return 0;
        case rs::STENCILMASK:
        case rs::STENCILWRITEMASK: return 0xFFFFFFFFu;
        case rs::TEXTUREFACTOR: return 0xFFFFFFFFu;
        case rs::CLIPPING: return 1;
        case rs::LIGHTING: return 1;
        case rs::EXTENTS: return 0;
        case rs::AMBIENT: return 0;
        case rs::COLORVERTEX: return 1;
        case rs::LOCALVIEWER: return 1;
        case rs::NORMALIZENORMALS: return 0;
        case rs::DIFFUSEMATERIALSOURCE: return 1;   // D3DMCS_COLOR1
        case rs::SPECULARMATERIALSOURCE: return 2;  // D3DMCS_COLOR2
        case rs::AMBIENTMATERIALSOURCE: return 0;   // D3DMCS_MATERIAL
        case rs::EMISSIVEMATERIALSOURCE: return 0;  // D3DMCS_MATERIAL
        case rs::VERTEXBLEND: return 0;             // D3DVBLEND_DISABLE
        case rs::CLIPPLANEENABLE: return 0;
        default:
            if (which >= rs::WRAP0 && which <= rs::WRAP7) return 0;
            return 0;
    }
}

u32 default_stage_state(u32 stage, u32 which) {
    switch (which) {
        // Stage 0 powers up modulating the texture against the interpolated
        // diffuse colour, so binding a texture and drawing is sufficient
        // without touching the combiner at all. Every later stage is disabled.
        case tss::COLOROP: return stage == 0 ? 4u : 1u;  // MODULATE : DISABLE
        case tss::ALPHAOP: return stage == 0 ? 2u : 1u;  // SELECTARG1 : DISABLE
        case tss::COLORARG1:
        case tss::ALPHAARG1: return 2;                   // D3DTA_TEXTURE
        case tss::COLORARG2:
        case tss::ALPHAARG2: return 1;                   // D3DTA_CURRENT
        case tss::TEXCOORDINDEX: return stage;
        case tss::ADDRESS:
        case tss::ADDRESSU:
        case tss::ADDRESSV: return 1;                    // D3DTADDRESS_WRAP
        case tss::BORDERCOLOR: return 0;
        case tss::MAGFILTER: return 1;                   // D3DTFG_POINT
        case tss::MINFILTER: return 1;                   // D3DTFN_POINT
        case tss::MIPFILTER: return 1;                   // D3DTFP_NONE
        case tss::MIPMAPLODBIAS: return 0;
        case tss::MAXMIPLEVEL: return 0;
        case tss::MAXANISOTROPY: return 1;
        case tss::TEXTURETRANSFORMFLAGS: return 0;       // D3DTTFF_DISABLE
        default: return 0;
    }
}

u32 DeviceState::render_state(u32 which) const {
    const auto found = render_states.find(which);
    return found != render_states.end() ? found->second
                                        : default_render_state(which);
}

float DeviceState::render_state_float(u32 which) const {
    return bits_to_float(render_state(which));
}

u32 DeviceState::stage_state(u32 stage, u32 which) const {
    if (stage >= MAX_TEXTURE_STAGES) return default_stage_state(stage, which);
    const auto& table = stage_states[stage];
    const auto found = table.find(which);
    return found != table.end() ? found->second
                                : default_stage_state(stage, which);
}

const Mat4& DeviceState::transform(u32 which) const {
    const auto found = transforms.find(which);
    return found != transforms.end() ? found->second : identity_storage();
}

FFPipelineState state_to_ff(const DeviceState& s, u32 fvf) {
    FFPipelineState ff;
    ff.fvf_flags = fvf;

    // ---- rasterisation ---------------------------------------------------
    ff.flat_shading = s.render_state(rs::SHADEMODE) == 1;  // D3DSHADE_FLAT
    // D3D7 has no point-size render state and no point sprites; those arrived
    // with D3D8, so the generator's point path stays off.
    ff.point_size_output = false;
    ff.point_scale_enabled = false;
    ff.clip_planes_enabled = s.render_state(rs::CLIPPLANEENABLE) != 0;

    // D3DVERTEXBLENDFLAGS uses the same DISABLE/1/2/3WEIGHTS encoding as
    // D3D9's D3DVBF_*. Indexed blending and tweening are D3D8 additions.
    const u32 vertex_blend = s.render_state(rs::VERTEXBLEND);
    ff.vertex_blend_weight_count = vertex_blend <= 3 ? vertex_blend : 0;
    ff.indexed_vertex_blend = false;
    ff.vertex_tweening = false;

    // ---- lighting --------------------------------------------------------
    ff.lighting_enabled = s.render_state(rs::LIGHTING) != 0;
    ff.normalize_normals = s.render_state(rs::NORMALIZENORMALS) != 0;
    ff.local_viewer = s.render_state(rs::LOCALVIEWER) != 0;

    // The generator reads lights[0 .. active_light_count), so enabled lights
    // are compacted into the low slots in guest index order. A guest may
    // enable more than the device advertises; the extras are dropped rather
    // than silently displacing lower-indexed ones.
    u32 active_lights = 0;
    for (const auto& [index, slot] : s.lights) {
        (void)index;
        if (!slot.enabled) continue;
        if (active_lights >= MAX_ACTIVE_LIGHTS) break;
        FFLight& out = ff.lights[active_lights];
        out = slot.light;
        out.enabled = true;
        ++active_lights;
    }
    ff.active_light_count = active_lights;

    ff.material = s.material;
    const auto ambient = color_to_rgba(s.render_state(rs::AMBIENT));
    std::copy(ambient.begin(), ambient.end(), ff.global_ambient);

    ff.specular_enable = s.render_state(rs::SPECULARENABLE) != 0;
    ff.vertex_color_enable = s.render_state(rs::COLORVERTEX) != 0;
    ff.diffuse_material_source = s.render_state(rs::DIFFUSEMATERIALSOURCE);
    ff.specular_material_source = s.render_state(rs::SPECULARMATERIALSOURCE);
    ff.ambient_material_source = s.render_state(rs::AMBIENTMATERIALSOURCE);
    ff.emissive_material_source = s.render_state(rs::EMISSIVEMATERIALSOURCE);

    // ---- texture stages --------------------------------------------------
    // D3DTOP_* and D3DTA_* are numerically identical in D3D7 and D3D9, so the
    // combiner description transfers without a remap. A stage only counts as
    // active while the chain above it is unbroken: D3DTOP_DISABLE terminates
    // the cascade for every later stage regardless of what they hold.
    u32 stage_count = 0;
    bool chain_live = true;
    for (u32 stage = 0; stage < MAX_TEXTURE_STAGES; ++stage) {
        TextureStageState& out = ff.texture_stages[stage];
        const u32 color_op = s.stage_state(stage, tss::COLOROP);
        if (!chain_live || color_op == static_cast<u32>(TexStageOp::Disable)) {
            chain_live = false;
            out.color_op = TexStageOp::Disable;
            out.alpha_op = TexStageOp::Disable;
            continue;
        }
        // Microsoft documents a second cascade-termination rule alongside
        // D3DTOP_DISABLE: when COLORARG1 selects D3DTA_TEXTURE and the stage
        // has no texture bound, that stage and every later one go unprocessed.
        // This matters at defaults — D3D7 starts stage 0 at
        // MODULATE(TEXTURE, DIFFUSE), so a device that has never called
        // SetTexture would otherwise report one active stage and sample a
        // texture that does not exist, resolving the whole draw to nothing.
        const u32 color_arg1 = s.stage_state(stage, tss::COLORARG1);
        constexpr u32 kD3dtaSelectMask = 0xFu;
        constexpr u32 kD3dtaTexture = 2u;
        if ((color_arg1 & kD3dtaSelectMask) == kD3dtaTexture &&
            s.textures[stage] == 0) {
            chain_live = false;
            out.color_op = TexStageOp::Disable;
            out.alpha_op = TexStageOp::Disable;
            continue;
        }
        out.color_op = static_cast<TexStageOp>(color_op);
        out.color_arg1 = color_arg1;
        out.color_arg2 = s.stage_state(stage, tss::COLORARG2);
        // D3DTSS_COLORARG0 / ALPHAARG0 / RESULTARG are D3D8 additions, so the
        // triadic ops keep the generator's D3DTA_CURRENT defaults here.
        out.alpha_op =
            static_cast<TexStageOp>(s.stage_state(stage, tss::ALPHAOP));
        out.alpha_arg1 = s.stage_state(stage, tss::ALPHAARG1);
        out.alpha_arg2 = s.stage_state(stage, tss::ALPHAARG2);
        out.texcoord_index = s.stage_state(stage, tss::TEXCOORDINDEX);
        out.texture_transform_flags =
            s.stage_state(stage, tss::TEXTURETRANSFORMFLAGS);
        out.bump_env_mat00 = s.stage_state(stage, tss::BUMPENVMAT00);
        out.bump_env_mat01 = s.stage_state(stage, tss::BUMPENVMAT01);
        out.bump_env_mat10 = s.stage_state(stage, tss::BUMPENVMAT10);
        out.bump_env_mat11 = s.stage_state(stage, tss::BUMPENVMAT11);
        out.bump_env_lscale = s.stage_state(stage, tss::BUMPENVLSCALE);
        out.bump_env_loffset = s.stage_state(stage, tss::BUMPENVLOFFSET);
        ++stage_count;
    }
    ff.active_texture_stages = stage_count;

    // ---- fog -------------------------------------------------------------
    if (s.render_state(rs::FOGENABLE) != 0) {
        // D3DRENDERSTATE_FOGTABLEMODE selects per-pixel fog and takes
        // precedence; FOGVERTEXMODE is the per-vertex fallback.
        const u32 table_mode = s.render_state(rs::FOGTABLEMODE);
        const u32 vertex_mode = s.render_state(rs::FOGVERTEXMODE);
        const u32 mode = table_mode != 0 ? table_mode : vertex_mode;
        ff.fog_mode = mode != 0 ? static_cast<FogMode>(mode) : FogMode::None;
        ff.table_fog = table_mode != 0;
        if (ff.table_fog) {
            // Depth fog against a perspective projection uses reciprocal
            // fragment RHW; an affine projection has no W to divide by.
            const Mat4& projection = s.transform(ts::PROJECTION);
            const bool affine =
                projection[3] == 0.0f && projection[7] == 0.0f &&
                projection[11] == 0.0f && projection[15] == 1.0f;
            ff.table_fog_w = !affine;
        }
    }
    ff.range_fog_enable = s.render_state(rs::RANGEFOGENABLE) != 0;
    ff.fog_start = s.render_state_float(rs::FOGSTART);
    ff.fog_end = s.render_state_float(rs::FOGEND);
    ff.fog_density = s.render_state_float(rs::FOGDENSITY);
    const auto fog_color = color_to_rgba(s.render_state(rs::FOGCOLOR));
    std::copy(fog_color.begin(), fog_color.end(), ff.fog_color);

    // ---- alpha test ------------------------------------------------------
    ff.alpha_test_enable = s.render_state(rs::ALPHATESTENABLE) != 0;
    ff.alpha_test_func = s.render_state(rs::ALPHAFUNC);
    ff.alpha_ref = s.render_state(rs::ALPHAREF);

    return ff;
}

namespace {

/// D3DTS_WORLD is 1 in Direct3D 7 and the matrix palette continues at
/// D3DTS_WORLD1..3 (4..6), which is not the contiguous run D3D9 uses at 256.
constexpr u32 kWorldTransformIds[4] = {ts::WORLD, ts::WORLD1, ts::WORLD2, ts::WORLD3};

} // namespace

graphics::FFUniformInputs state_to_ff_uniform_inputs(const DeviceState& s) {
    graphics::FFUniformInputs inputs;

    for (u32 i = 0; i < 4 && i < graphics::FF_MAX_WORLD_MATRICES; ++i) {
        const Mat4& m = s.transform(kWorldTransformIds[i]);
        std::copy(m.begin(), m.end(), inputs.world_matrices[i].begin());
    }
    {
        const Mat4& view = s.transform(ts::VIEW);
        std::copy(view.begin(), view.end(), inputs.view.begin());
        const Mat4& projection = s.transform(ts::PROJECTION);
        std::copy(projection.begin(), projection.end(),
                  inputs.projection.begin());
    }
    for (u32 i = 0; i < graphics::FF_MAX_TEXTURE_STAGES; ++i) {
        const Mat4& m = s.transform(ts::TEXTURE0 + i);
        std::copy(m.begin(), m.end(), inputs.texture_transforms[i].begin());
    }

    // A pre-transformed (XYZRHW) vertex is mapped through the viewport in the
    // vertex shader, so its dimensions are a uniform rather than pipeline
    // state — resizing must not recompile every shader.
    inputs.viewport_width = static_cast<float>(s.viewport.width);
    inputs.viewport_height = static_cast<float>(s.viewport.height);

    inputs.texture_factor =
        graphics::ff_color_to_rgba(s.render_state(rs::TEXTUREFACTOR));
    inputs.clip_plane_enable_mask = s.render_state(rs::CLIPPLANEENABLE);
    for (u32 i = 0; i < graphics::FF_MAX_CLIP_PLANES &&
                    i < MAX_CLIP_PLANES; ++i) {
        inputs.clip_planes[i] = s.clip_planes[i];
    }

    // Direct3D 7 has no per-stage D3DTSS_CONSTANT and no mipmap LOD bias
    // sampler state; both arrived with D3D9. Their defaults (zero constant,
    // zero bias) are what the generated shader treats as "unused".
    return inputs;
}

} // namespace vitreo::winapi::d3d7

// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ff_emulation.hpp
/// @brief D3D9 fixed-function pipeline → MSL shader generator.
///
/// Given the current D3D9 render state (transforms, lighting, texture stage
/// combiners, fog), generates equivalent vertex + fragment MSL shaders.
/// Generated shaders are cached by state hash to avoid re-generation.

#include "shader_types.hpp"
#include <array>
#include <string>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// D3D9 Fixed-Function State
// =============================================================================

// Flexible Vertex Format flags (D3DFVF_*)
namespace fvf {
    constexpr u32 XYZ           = 0x002;
    constexpr u32 XYZRHW       = 0x004;
    constexpr u32 XYZB1         = 0x006;
    constexpr u32 XYZB2         = 0x008;
    constexpr u32 XYZB3         = 0x00A;
    constexpr u32 XYZB4         = 0x00C;
    constexpr u32 XYZB5         = 0x00E;
    constexpr u32 POSITION_MASK = 0x400E;
    constexpr u32 NORMAL        = 0x010;
    constexpr u32 PSIZE         = 0x020;
    constexpr u32 DIFFUSE       = 0x040;
    constexpr u32 SPECULAR      = 0x080;
    constexpr u32 TEX1          = 0x100;
    constexpr u32 TEX2          = 0x200;
    constexpr u32 TEX3          = 0x300;
    constexpr u32 TEX4          = 0x400;
    constexpr u32 TEXCOUNT_MASK = 0xF00;
    constexpr u32 TEXCOUNT_SHIFT = 8;
    constexpr u32 LASTBETA_UBYTE4 = 0x1000;
} // namespace fvf

// Texture stage combiner operations (D3DTOP_*). Values MUST match MSDN —
// an off-by-one here silently remaps Blend*/AddSmooth and drops AddSigned2x.
enum class TexStageOp : u8 {
    Disable = 1,
    SelectArg1 = 2,
    SelectArg2 = 3,
    Modulate = 4,
    Modulate2x = 5,
    Modulate4x = 6,
    Add = 7,
    AddSigned = 8,
    AddSigned2x = 9,
    Subtract = 10,
    AddSmooth = 11,
    BlendDiffuseAlpha = 12,
    BlendTextureAlpha = 13,
    BlendFactorAlpha = 14,
    BlendTextureAlphaPM = 15,
    BlendCurrentAlpha = 16,
    Premodulate = 17,
    ModulateAlphaAddColor = 18,
    ModulateColorAddAlpha = 19,
    ModulateInvAlphaAddColor = 20,
    ModulateInvColorAddAlpha = 21,
    BumpEnvMap = 22,
    BumpEnvMapLuminance = 23,
    DotProduct3 = 24,
    MultiplyAdd = 25,
    Lerp = 26,
};

// Texture stage combiner argument sources (D3DTA_*)
enum class TexStageArg : u8 {
    Diffuse = 0,
    Current = 1,
    Texture = 2,
    TFactor = 3,
    Specular = 4,
    Temp = 5,
    Constant = 6,
};

struct TextureStageState {
    TexStageOp color_op = TexStageOp::Disable;
    /// Full D3DTA_* value (source in low 4 bits + COMPLEMENT/ALPHAREPLICATE).
    u32 color_arg0 = 1;  // D3DTA_CURRENT (MultiplyAdd / Lerp)
    u32 color_arg1 = 2;  // D3DTA_TEXTURE
    u32 color_arg2 = 1;  // D3DTA_CURRENT
    TexStageOp alpha_op = TexStageOp::Disable;
    u32 alpha_arg0 = 1;
    u32 alpha_arg1 = 2;
    u32 alpha_arg2 = 1;
    /// D3DTSS_RESULTARG: D3DTA_CURRENT (1) or D3DTA_TEMP (5).
    u32 result_arg = 1;
    /// Full D3DTSS_TEXCOORDINDEX (low bits = set, high bits = TCI_*).
    u32 texcoord_index = 0;
    /// D3DTSS_TEXTURETRANSFORMFLAGS (COUNT* | PROJECTED).
    u32 texture_transform_flags = 0;
    /// Raw IEEE-754 bits from D3DTSS_BUMPENVMAT00/01/10/11.
    u32 bump_env_mat00 = 0;
    u32 bump_env_mat01 = 0;
    u32 bump_env_mat10 = 0;
    u32 bump_env_mat11 = 0;
    /// Raw IEEE-754 bits from D3DTSS_BUMPENVLSCALE/BUMPENVLOFFSET.
    u32 bump_env_lscale = 0;
    u32 bump_env_loffset = 0;
};

// Fog mode (D3DFOG_*)
enum class FogMode : u8 {
    None = 0,
    Linear = 3,
    Exp = 1,
    Exp2 = 2,
};

// Light type (D3DLIGHTTYPE)
enum class LightType : u8 {
    Point = 1,
    Spot = 2,
    Directional = 3,
};

// =============================================================================
// Matrix convention (the one place it is defined)
// =============================================================================
//
// An FFMatrix (ff_uniforms.hpp) is a Direct3D matrix: row-major storage and
// row-vector math, p' = p * M, with the translation in elements 12..14
// (Learn, "Transforms (Direct3D 9)", "World Transform (Direct3D 9)", "View
// Transform (Direct3D 9)", "Projection Transform (Direct3D 9)":
// v' = v * World * View * Projection). pack_ff_vertex_uniforms stores
// transpose(M) into MSL's column-major float4x4, so the MSL value equals M
// and EVERY generated site applies it in the row-vector form `p * cb.X`:
//   - position: `p * cb.world_view_proj` (W * V * P), or, blended, the
//     weighted sum of `p * cb.worldN` (world space) then `* cb.view *
//     cb.projection` ("Geometry Blending (Direct3D 9)");
//   - camera space for lighting, fog, point scale and D3DTSS_TCI_*:
//     `p * cb.world * cb.view` ("Camera Space Transformations (Direct3D 9)":
//     V * wvMatrix). Direct3D lighting is evaluated in camera space: the
//     packer moves light positions and directions there with `* View`;
//   - normals: `n * cb.world_inv_transpose` (and the blend matrices'
//     `*_inv_transpose`), packed (append_normal_matrix) as the raw inverse
//     of world * view, whose MSL value is transpose((W V)^-1): the camera-
//     space N * (wvMatrix^-1)^T of the same Learn page;
//   - clip planes: `p * cb.world`, world space (Learn, IDirect3DDevice9::
//     SetClipPlane: fixed-function planes are in world space);
//   - texture transform: `tc * cb.texture_matrix[i]` (fragment stage;
//     "Texture Coordinate Transformations (Direct3D 9)"), then
//     D3DTTFF_PROJECTED divides by the last of the D3DTTFF_COUNTn elements.
// No site uses the column-vector form `cb.X * p`
// (FFEmulationGl.MatrixApplicationConventionIsPinned and the Metal tests in
// test_ff_emulation_d3d_metal.cpp hold this).
//
// An OpenGL caller (position_is_homogeneous, gl_spot_model,
// two_sided_lighting, texgen_plane_mode) passes each GL matrix array
// verbatim: GL's column-major array of the column-vector matrix G, read
// row-major, is transpose(G), the row-vector form of G (gl_backend_draw.cpp,
// to_ff). It passes world = MV and view = I, so camera space is GL's eye
// space and the normal matrix is MV^-1 (GL spec 2.10.3).

// GL 1.1 texture-coordinate generation modes (spec 2.10.4, glTexGen).
enum class FFTexGenMode : u8 {
    None = 0,         ///< the coordinate comes from the vertex (texgen disabled)
    ObjectLinear = 1, ///< g = p . (xo, yo, zo, wo)
    EyeLinear = 2,    ///< g = p' . (xe, ye, ze, we), p' = p M^-1 at glTexGen time
    SphereMap = 3,    ///< s / t only: the spec's reflection-vector formula
};

// GL 1.1 logic operations (spec 4.1.10, Table 4.5), in GL_CLEAR + n order.
// Copy is "no logic op": the fragment colour is written unchanged.
enum class FFLogicOp : u8 {
    Clear = 0,
    And = 1,
    AndReverse = 2,
    Copy = 3,
    AndInverted = 4,
    Noop = 5,
    Xor = 6,
    Or = 7,
    Nor = 8,
    Equiv = 9,
    Invert = 10,
    OrReverse = 11,
    CopyInverted = 12,
    OrInverted = 13,
    Nand = 14,
    Set = 15,
};

/// GL 1.1 line stipple (spec 3.4.2). Only `enabled` changes the generated
/// MSL; factor and pattern are fragment-uniform payload.
struct FFLineStipple {
    bool enabled = false;
    u32 factor = 1;       ///< glLineStipple factor, clamped to [1, 256] when packed
    u16 pattern = 0xFFFF;
};

struct FFLight {
    LightType type = LightType::Directional;
    bool enabled = false;
    float diffuse[4]  = {1, 1, 1, 1};
    float specular[4] = {0, 0, 0, 0};
    float ambient[4]  = {0, 0, 0, 0};
    float position[3] = {0, 0, 1};
    float direction[3]= {0, 0, -1};
    float range = 1e10f;
    float attenuation[3] = {1, 0, 0};  // constant, linear, quadratic
    float spot_falloff = 0;
    float spot_theta = 0;
    float spot_phi = 0;
    /// gl_spot_model only (GL 1.1 spec 2.13.1). `position` + `gl_position_w`
    /// is the light's eye-space homogeneous position (w = 0: directional);
    /// `direction` is the eye-space spot direction.
    float gl_position_w = 1;
    float gl_spot_exponent = 0;   ///< s_rli, [0, 128]
    float gl_spot_cutoff = 180;   ///< c_rli in degrees, [0, 90] or 180 (no spot)
};

struct FFMaterial {
    float diffuse[4]  = {1, 1, 1, 1};
    float ambient[4]  = {1, 1, 1, 1};
    float specular[4] = {0, 0, 0, 0};
    float emissive[4] = {0, 0, 0, 0};
    float power = 0;
};

// Full fixed-function pipeline state for shader generation
struct FFPipelineState {
    u32 fvf_flags = fvf::XYZ;
    /// D3DRS_SHADEMODE == D3DSHADE_FLAT: color varyings use the provoking
    /// vertex instead of interpolation across the primitive.
    bool flat_shading = false;
    /// Point-list pipelines must provide Metal's [[point_size]] output.
    bool point_size_output = false;
    /// Untransformed fixed-function points use the D3D9 camera-distance
    /// attenuation formula before the final point-size clamp.
    bool point_scale_enabled = false;
    /// One or more D3DRS_CLIPPLANEENABLE bits are active. Fixed-function
    /// plane coefficients are evaluated in world space.
    bool clip_planes_enabled = false;
    /// D3DRS_VERTEXBLEND D3DVBF_1/2/3WEIGHTS. These are the explicit beta
    /// count; the following WORLD matrix receives the implicit 1-sum weight.
    u32 vertex_blend_weight_count = 0;
    /// D3DRS_INDEXEDVERTEXBLENDENABLE with D3DFVF_LASTBETA_UBYTE4. Each
    /// weight selects its WORLD palette matrix through the corresponding
    /// packed byte rather than using WORLD0..N consecutively.
    bool indexed_vertex_blend = false;
    /// D3DVBF_TWEENING blends declaration POSITION0/POSITION1 and, when
    /// present, NORMAL0/NORMAL1 by D3DRS_TWEENFACTOR before transformation.
    bool vertex_tweening = false;
    float tween_factor = 0.0f;
    bool lighting_enabled = true;
    /// D3DRS_NORMALIZENORMALS after the world/view normal transform.
    bool normalize_normals = false;
    /// D3DRS_LOCALVIEWER: use the eye position for the specular half-vector;
    /// otherwise Direct3D uses the camera-space infinite-viewer direction.
    bool local_viewer = true;
    u32 active_light_count = 0;
    std::array<FFLight, 8> lights;
    FFMaterial material;
    float global_ambient[4] = {0, 0, 0, 0};

    std::array<TextureStageState, 8> texture_stages;
    u32 active_texture_stages = 0;  // Count of non-disabled stages

    FogMode fog_mode = FogMode::None;
    /// D3DRS_FOGTABLEMODE selects per-pixel depth fog instead of interpolated
    /// fixed-function vertex fog.
    bool table_fog = false;
    /// Perspective projection with WFOG uses reciprocal fragment RHW;
    /// affine projection uses device-space z.
    bool table_fog_w = false;
    /// D3DRS_RANGEFOGENABLE: use eye distance instead of |camera-space Z|.
    bool range_fog_enable = false;
    float fog_start = 0;
    float fog_end = 1;
    float fog_density = 1;
    float fog_color[4] = {0, 0, 0, 0};

    bool specular_enable = false;
    bool vertex_color_enable = true;
    /// D3DMCS_* sources used when `vertex_color_enable` (COLORVERTEX) is set.
    /// Defaults match D3D9: diffuse=COLOR1, ambient/emissive=MATERIAL,
    /// specular=COLOR2.
    u32 diffuse_material_source = 1;
    u32 ambient_material_source = 0;
    u32 specular_material_source = 2;
    u32 emissive_material_source = 0;

    // Alpha test (note: we handle this in fragment shader since Metal
    // doesn't have a fixed-function alpha test like D3D9)
    bool alpha_test_enable = false;
    u32 alpha_test_func = 8;    // D3DCMP_ALWAYS
    u32 alpha_ref = 0;          // 0-255

    /// D3DSAMP_SRGBTEXTURE, bit N per sampler: the texel is gamma-decoded on
    /// read. Ignoring it leaves every sampled surface in gamma space, i.e.
    /// far too bright.
    u32 srgb_texture_mask = 0;
    /// D3DRS_SRGBWRITEENABLE: gamma-encode the result on the way to the
    /// render target. D3D9 converts the shader output, so blending then
    /// operates on encoded values.
    bool srgb_write_enable = false;

    // ---- OpenGL fixed-function extensions -----------------------------------
    // Every field below defaults to the Direct3D behaviour; a state that
    // leaves them all at their defaults generates byte-identical MSL, state
    // hash and uniform bytes (test_ff_emulation_d3d_golden.cpp). All of them
    // follow the row-vector matrix convention above.

    /// The position attribute is float4 object coordinates (x, y, z, w),
    /// passed unchanged into the row-vector transforms (GL spec 2.10: object
    /// coordinates are homogeneous). Clipping then happens in clip space
    /// with the vertex's own w, so w = 0 and mixed-sign primitives clip as
    /// the spec defines. Eye-space positions for lighting, point size and
    /// fog are divided by their w.
    /// Requires a non-pretransformed position without blend weights,
    /// tweening or D3DTSS_TCI_* texture coordinates; otherwise
    /// generate_ff_shaders returns empty (invalid) shaders.
    bool position_is_homogeneous = false;

    /// The GL 1.1 per-light lighting equation (spec 2.13.1) instead of
    /// Direct3D's: for each light, att_i * spot_i * (a_cm a_cli +
    /// (n (.) VP) d_cm d_cli + f_i (n (.) h)^s_rm s_cm s_cli), plus e_cm +
    /// a_cm a_cs, clamped to [0, 1], alpha d_cm.a; the specular term goes into
    /// the primary colour (out.specular is 0). Lighting is evaluated in the
    /// space of `world` (view must be identity: GL lights are already eye
    /// space), the light uniforms are (position xyz + gl_position_w,
    /// direction xyz + gl_spot_exponent, k0 k1 k2 + cos(gl_spot_cutoff) or -2
    /// for 180), the infinite viewer is (0, 0, 1), the local viewer the eye-
    /// space origin, and lighting stays on with no active lights (emission +
    /// global ambient). `light.type` is ignored. (.) is the clamped dot
    /// max(d1 . d2, 0) (eq. 2.2); f_i = 1 when n (.) VP != 0, i.e. when
    /// n . VP > 0; the spot is (PV (.) s)^exponent when PV (.) s >=
    /// cos(cutoff) and 0 otherwise (eq. 2.5), so cutoff 90 with exponent 0
    /// is 1 even behind the light.
    bool gl_spot_model = false;

    /// GL two-sided lighting (spec 2.13.1, LIGHT_MODEL_TWO_SIDE): a back
    /// colour is lit with -n and `back_material` / `back_*_material_source`,
    /// and the fragment stage takes it for back-facing fragments
    /// ([[front_facing]], i.e. the BackendDrawCall's front_counter_clockwise
    /// winding as Metal applies it). Requires gl_spot_model. Points and
    /// lines always take the front colour (spec 2.13.1): a point pipeline
    /// (point_size_output) does so here; a line pipeline cannot be told
    /// apart, so the caller (Task 9) must leave two_sided_lighting off for
    /// lines.
    bool two_sided_lighting = false;
    FFMaterial back_material;
    /// D3DMCS_*-style sources for the back material (glColorMaterial face).
    u32 back_diffuse_material_source = 1;
    u32 back_ambient_material_source = 0;
    u32 back_specular_material_source = 2;
    u32 back_emissive_material_source = 0;

    /// glTexGen mode per texture unit [unit] and coordinate [s, t, r, q].
    /// The generated coordinate replaces that component of the vertex's
    /// texture coordinate set `unit`, before the fragment stage's texture
    /// matrix. A unit with any mode needs texcoord_count() > unit; SphereMap
    /// is valid for s and t only and needs fvf NORMAL. Planes come from
    /// FFUniformInputs::texgen_object_planes / texgen_eye_planes.
    std::array<std::array<FFTexGenMode, 4>, 4> texgen_plane_mode{};

    /// GL line stipple (spec 3.4.2). When enabled the vertex input gains
    /// `float line_distance` as its LAST attribute (after the FVF
    /// attributes, see ff_vertex_layout.hpp for their order), interpolated
    /// linearly in window space; a fragment is kept when bit
    /// (floor(line_distance) / factor) mod 16 of the pattern is 1. The
    /// caller supplies line_distance as the stipple counter s: window-space
    /// length along the line's major axis, in pixels, from the point the
    /// counter was last reset (each glBegin, and each GL_LINES segment).
    FFLineStipple line_stipple;

    /// GL polygon stipple (spec 3.5.2): a fragment at framebuffer pixel
    /// (x, y) is kept when bit (x mod 32) of row (y mod 32) of
    /// FFUniformInputs::polygon_stipple_rows is 1. Rows are in the backend
    /// framebuffer's orientation (row 0 at the top); the caller converts GL's
    /// bottom-up, MSB-first mask. Enable only for filled polygons.
    bool polygon_stipple = false;

    /// GL logic op (spec 4.1.10) applied last in the fragment stage, reading
    /// the destination through framebuffer fetch ([[color(0)]]); needs
    /// GraphicsBackendDevice::supports_framebuffer_fetch(), an 8-bit unorm
    /// colour attachment 0, and blending off. Copy = off.
    FFLogicOp logic_op = FFLogicOp::Copy;

    /// The alpha-test reference is FFUniformInputs::alpha_reference (a
    /// float, clamped to [0, 1] when packed, GL spec 4.1.4) instead of
    /// alpha_ref / 255. Uniform payload only: not part of the hash.
    bool alpha_ref_float = false;

    /// GL lines and points expanded on the CPU into window-aligned quads
    /// (GL 1.1 spec 3.3, 3.4.2; gl_assembler.hpp). The vertex input gains
    /// `float2 window_offset` (after line_distance), a displacement in window
    /// pixels added after the projection: clip xy += window_offset *
    /// (2 / viewport width, 2 / viewport height) * clip w, so a corner lands
    /// window_offset pixels from where its object position projects. The
    /// scale is FFUniformInputs::viewport_width/height, packed as `float4
    /// gl_window_scale` at the end of the vertex uniforms. Requires
    /// position_is_homogeneous.
    bool window_offset = false;

    /// glPolygonOffset (spec 3.5.5) in the fragment stage, which then writes
    /// [[depth(any)]] = clamp(z + depth_offset.x + depth_offset.y *
    /// max(|dz/dx|, |dz/dy|), 0, 1), z the fragment's window depth and
    /// depth_offset a flat per-vertex `float2` attribute (the LAST attribute):
    /// (units * r, factor) for a filled polygon, whose maximum depth slope
    /// (eq. 3.6) the derivatives give exactly; (the whole offset, 0) for a
    /// polygon drawn as CPU-expanded lines or points, whose slope the caller
    /// computed from the polygon. Requires position_is_homogeneous.
    bool depth_offset = false;

    /// Colour attachments 0..color_targets-1 that receive the fragment
    /// colour (GL_FRONT_AND_BACK and GL_LEFT on a double-buffered format
    /// write both buffers, spec 4.2.1). With a logic op each target reads its
    /// own destination ([[color(i)]]) and gets its own result. 1 to 8.
    u32 color_targets = 1;

    /// GL 1.1 texture sampling (spec 3.8.1-3.8.2) in the fragment stage, in
    /// place of the Metal sampler: stage i fetches texels of `tex<i>` with
    /// `read()` and applies the GL rules itself. Its coordinate is
    /// in.texcoord<i> * texture_matrix[i] (row-vector form, so stages need
    /// texture_transform_flags != 0 for the matrix to be packed), then (s/q,
    /// t/q); lambda = log2(rho), rho = max(|d(u,v)/dx|, |d(u,v)/dy|) (eq.
    /// 3.11) with u = 2^n s, v = 2^m t of level 0; magnification when lambda
    /// <= c (c = 0.5 for a LINEAR mag filter with a NEAREST_MIPMAP_* min
    /// filter, else 0); NEAREST takes texel floor(u) (2^n - 1 at s = 1) and
    /// LINEAR the 2x2 square at floor(u - 1/2) with weights frac(u - 1/2);
    /// REPEAT wraps s to its fraction and every index mod the level size;
    /// CLAMP clamps s to [0, 1] and an index outside the level reads the
    /// border colour; the mipmap filters select levels per spec 3.8.2. The
    /// per-stage filters, wrap modes, level-0 log2 sizes, top mipmap level,
    /// dimensionality (1: t ignored) and canonical border colour are
    /// FFUniformInputs::gl_textures[i] (uniform payload). tex_sample is the
    /// texel exactly as stored: the caller stores each base internal format
    /// in a canonical RGBA layout. No sampler is declared. Requires
    /// position_is_homogeneous.
    bool gl_texture_sampling = false;

    /// Hash of the fields that change generated MSL. Uniform payload
    /// (light positions, material colors, fog distances) is excluded.
    [[nodiscard]] u64 hash() const;

    /// Number of texture coordinate sets from FVF.
    [[nodiscard]] u32 texcoord_count() const {
        return (fvf_flags & fvf::TEXCOUNT_MASK) >> fvf::TEXCOUNT_SHIFT;
    }
};

// =============================================================================
// Generated Shader Output
// =============================================================================

struct FFGeneratedShaders {
    std::string vertex_msl;     // Generated vertex shader MSL
    std::string fragment_msl;   // Generated fragment shader MSL
    u64 state_hash;             // Hash of the FFPipelineState used

    [[nodiscard]] bool valid() const {
        return !vertex_msl.empty() && !fragment_msl.empty();
    }
};

// =============================================================================
// API
// =============================================================================

/// Whether any OpenGL field differs from its Direct3D default.
[[nodiscard]] bool ff_uses_gl_fields(const FFPipelineState& state);
/// Whether the vertex uniforms carry texgen_planes (an OBJECT_ or EYE_LINEAR
/// coordinate is in use).
[[nodiscard]] bool ff_texgen_needs_planes(const FFPipelineState& state);
/// Whether the vertex uniforms carry the camera/material/light block.
[[nodiscard]] bool ff_lighting_uniforms(const FFPipelineState& state);

/// Generate MSL vertex + fragment shaders for the given D3D9 fixed-function state.
/// The generated shaders handle: transforms, per-vertex lighting, texture
/// stage combiners, fog, alpha test, and vertex colors.
[[nodiscard]] FFGeneratedShaders generate_ff_shaders(const FFPipelineState& state);

/// Fixed-function fragment shader whose stage-in matches an SM1–3 vertex
/// shader's `Sm3Varyings` (`[[user(colorN)]]` / `[[user(texcoordN)]]`).
[[nodiscard]] std::string generate_ff_fragment_for_sm3_varyings(
    const FFPipelineState& state,
    const std::vector<u32>& color_varyings,
    const std::vector<u32>& texcoord_varyings,
    bool use_vertex_fog_factor = false);

/// Fixed-function vertex shader that emits `Sm3Varyings` matching what a
/// bound SM1–3 pixel shader reads.
[[nodiscard]] std::string generate_ff_vertex_for_sm3_varyings(
    const FFPipelineState& state,
    const std::vector<u32>& color_varyings,
    const std::vector<u32>& texcoord_varyings);

} // namespace vitreo::graphics

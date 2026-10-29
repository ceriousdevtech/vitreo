// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file ff_uniforms.hpp
/// @brief Uniform-buffer packing for the shaders `generate_ff_shaders` emits.
///
/// `generate_ff_shaders()` declares its vertex and fragment uniform structs
/// conditionally on the same `FFPipelineState` that is passed here, so the
/// packing below must mirror the generator field for field: a mismatch shifts
/// every field after the first difference.
///
/// The packing is API-neutral. Every Direct3D fixed-function API (DirectX 7's
/// `IDirect3DDevice7`, Direct3D 8/9's fixed-function path) keeps the same
/// transform/light/material/fog/texture-stage contract, so the caller
/// translates its own state tables into `FFUniformInputs` and this code owns
/// the byte layout.

#include "ff_emulation.hpp"

#include <array>
#include <vector>

namespace vitreo::graphics {

/// A Direct3D matrix in its native row-major, row-vector (`v * M`) form.
using FFMatrix = std::array<float, 16>;

constexpr FFMatrix FF_IDENTITY_MATRIX{1, 0, 0, 0, 0, 1, 0, 0,
                                      0, 0, 1, 0, 0, 0, 0, 1};

/// D3DTS_WORLD..D3DTS_WORLD + 8. The advertised MaxVertexBlendMatrixIndex is
/// 8, hence a nine-entry palette; entry 0 is the plain world matrix.
constexpr u32 FF_MAX_WORLD_MATRICES = 9;

/// Fixed-function user clip planes (D3DMAXUSERCLIPPLANES).
constexpr u32 FF_MAX_CLIP_PLANES = 6;

constexpr u32 FF_MAX_TEXTURE_STAGES = 8;

/// Texture units FFPipelineState::texgen_plane_mode covers.
constexpr u32 FF_MAX_TEXGEN_UNITS = 4;

/// Row-major multiply, matching the order Direct3D composes world * view *
/// projection.
[[nodiscard]] FFMatrix ff_matrix_multiply(const FFMatrix& a, const FFMatrix& b);

/// Packing stores transpose(M) into MSL's column-major float4x4, so the MSL
/// value equals M and the generator's row-vector form `p * cb.X` applies the
/// guest's matrix (ff_emulation.hpp, matrix convention).
[[nodiscard]] FFMatrix ff_matrix_transpose(const FFMatrix& m);

/// Row-major 4x4 inverse. Returns false when the matrix is singular.
[[nodiscard]] bool ff_matrix_invert(const FFMatrix& m, FFMatrix& out);

/// Everything the generated uniform blocks need that is not already carried by
/// `FFPipelineState`. Members are raw Direct3D values; the clamping and
/// derivation the fixed-function pipeline specifies happens during packing.
struct FFUniformInputs {
    /// D3DTS_WORLD..D3DTS_WORLD + 8. Index 0 is D3DTS_WORLD itself and is the
    /// matrix used for the non-blended transform and normal matrix.
    std::array<FFMatrix, FF_MAX_WORLD_MATRICES> world_matrices{};
    FFMatrix view = FF_IDENTITY_MATRIX;
    FFMatrix projection = FF_IDENTITY_MATRIX;
    /// D3DTS_TEXTURE0..7, used only when a stage sets TEXTURETRANSFORMFLAGS.
    std::array<FFMatrix, FF_MAX_TEXTURE_STAGES> texture_transforms{};

    /// Viewport dimensions in pixels, needed to map pre-transformed (XYZRHW)
    /// screen coordinates into clip space and to scale attenuated point
    /// sizes. A uniform rather than part of the pipeline state so resizing
    /// does not recompile every shader.
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;

    /// D3DRS_POINTSIZE / _MIN / _MAX and D3DRS_POINTSCALE_A/B/C, unclamped.
    float point_size = 1.0f;
    float point_size_min = 1.0f;
    float point_size_max = 64.0f;
    float point_scale_a = 1.0f;
    float point_scale_b = 0.0f;
    float point_scale_c = 0.0f;

    /// D3DRS_CLIPPLANEENABLE. Planes whose bit is clear pack as zero.
    u32 clip_plane_enable_mask = 0;
    std::array<std::array<float, 4>, FF_MAX_CLIP_PLANES> clip_planes{};

    /// D3DRS_TEXTUREFACTOR, already unpacked to RGBA in 0..1.
    std::array<float, 4> texture_factor{};
    /// D3DTSS_CONSTANT per stage, unpacked to RGBA in 0..1.
    std::array<std::array<float, 4>, FF_MAX_TEXTURE_STAGES> stage_constants{};
    /// D3DSAMP_MIPMAPLODBIAS per stage as a float.
    std::array<float, FF_MAX_TEXTURE_STAGES> mipmap_lod_bias{};

    // ---- OpenGL fixed-function extensions (ff_emulation.hpp) ----------------
    /// glTexGen planes per [unit][s, t, r, q]: OBJECT_PLANE as given, and
    /// EYE_PLANE as stored by glTexGen (already multiplied by the inverse of
    /// the modelview current at that call, spec 2.10.4). Packed per
    /// FFPipelineState::texgen_plane_mode: the object plane for
    /// ObjectLinear, the eye plane for EyeLinear, zero otherwise.
    using Plane = std::array<float, 4>;
    std::array<std::array<Plane, 4>, FF_MAX_TEXGEN_UNITS> texgen_object_planes{};
    std::array<std::array<Plane, 4>, FF_MAX_TEXGEN_UNITS> texgen_eye_planes{};
    /// Polygon stipple rows in framebuffer orientation (row 0 at the top),
    /// bit k = column k (FFPipelineState::polygon_stipple). GL's initial
    /// pattern is all ones.
    std::array<u32, 32> polygon_stipple_rows{};
    /// FFPipelineState::alpha_ref_float: the alpha-test reference.
    float alpha_reference = 0.0f;
    /// FFPipelineState::gl_texture_sampling: each stage's texture parameters,
    /// packed as `GlTexUnit gl_tex[active_texture_stages]` (uint4 filter,
    /// uint4 size, float4 border) at the end of the fragment block.
    struct GlTextureUnit {
        u32 min_filter = 0x2702;     ///< TEXTURE_MIN_FILTER (GL enumerant)
        u32 mag_filter = 0x2601;     ///< TEXTURE_MAG_FILTER
        u32 wrap_s = 0x2901;         ///< TEXTURE_WRAP_S: CLAMP 0x2900 / REPEAT 0x2901
        u32 wrap_t = 0x2901;         ///< TEXTURE_WRAP_T
        u32 log2_width = 0;          ///< n: level 0 is 2^n wide
        u32 log2_height = 0;         ///< m: level 0 is 2^m high (0 for 1D)
        u32 top_level = 0;           ///< p = max(n, m), the last mipmap array
        u32 dimensions = 2;          ///< 1: a one-dimensional texture (t ignored)
        std::array<float, 4> border{};   ///< border colour in the stored layout
    };
    std::array<GlTextureUnit, FF_MAX_TEXTURE_STAGES> gl_textures{};

    FFUniformInputs() {
        polygon_stipple_rows.fill(0xFFFFFFFFu);
        world_matrices.fill(FF_IDENTITY_MATRIX);
        texture_transforms.fill(FF_IDENTITY_MATRIX);
    }
};

/// Unpack a D3DCOLOR (ARGB) into the RGBA order the generated shaders expect.
[[nodiscard]] std::array<float, 4> ff_color_to_rgba(u32 color);

/// Pack the vertex-stage uniform block for `generate_ff_shaders(state)`.
/// `out` is cleared first.
void pack_ff_vertex_uniforms(const FFPipelineState& state,
                             const FFUniformInputs& inputs,
                             std::vector<u8>& out);

/// Pack the fragment-stage uniform block for `generate_ff_shaders(state)`.
/// `out` is cleared first.
void pack_ff_fragment_uniforms(const FFPipelineState& state,
                               const FFUniformInputs& inputs,
                               std::vector<u8>& out);

} // namespace vitreo::graphics

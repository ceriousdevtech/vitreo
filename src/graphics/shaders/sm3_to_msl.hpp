// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file sm3_to_msl.hpp
/// @brief Direct3D 9 shader model 1–3 bytecode to Metal Shading Language.
///
/// A D3D9 shader is not a DXBC container: `CreateVertexShader` and
/// `CreatePixelShader` take a raw token stream that begins with a version
/// token and ends with `END`. `dxbc_parser` handles the SM4+ container that
/// D3D10 and later use; this handles what D3D9 actually passes.
///
/// The translator refuses anything it cannot express rather than emitting a
/// shader that compiles but computes something else. `failure` names the
/// construct so the caller can log which contract is missing.

#include "shader_types.hpp"

#include <array>
#include <string>
#include <vector>

namespace vitreo::graphics {

/// D3DDECLUSAGE values a vertex shader can declare on an input register.
enum class Sm3Usage : u32 {
    Position = 0,
    BlendWeight = 1,
    BlendIndices = 2,
    Normal = 3,
    PointSize = 4,
    TexCoord = 5,
    Tangent = 6,
    Binormal = 7,
    TessFactor = 8,
    PositionT = 9,
    Color = 10,
    Fog = 11,
    Depth = 12,
    Sample = 13,
};

/// One `dcl_<usage> v#` in a vertex shader: which vertex element the shader
/// expects, and the attribute index it reads it from.
struct Sm3InputElement {
    Sm3Usage usage{Sm3Usage::Position};
    u32 usage_index{0};
    u32 register_index{0};
};

struct Sm3Shader {
    bool ok{false};
    /// Set when `ok` is false: the construct that has no translation.
    std::string failure;

    std::string msl;
    std::string entry_point;
    ShaderStage stage{ShaderStage::Vertex};
    u32 major{0};
    u32 minor{0};

    /// Vertex shader input declarations, in declaration order.
    std::vector<Sm3InputElement> inputs;
    /// Sampler registers the shader reads from.
    std::array<bool, 16> samplers_used{};
    /// Pixel colour outputs that have at least one write instruction.  The
    /// D3D9 draw path uses this to disable writes to statically absent MRT
    /// outputs even when a surface remains bound at that slot.
    std::array<bool, 4> color_outputs_written{};
    /// Colour / texcoord varyings this stage writes (VS) or reads (PS). Used
    /// to build a matching fixed-function companion when only one programmable
    /// stage is bound.
    std::vector<u32> color_varyings;
    std::vector<u32> texcoord_varyings;
    /// A vs_1_x / vs_2_x shader writes the scalar oFog output. The fixed
    /// function pixel pipeline consumes this as an interpolated fog factor.
    bool writes_fog{false};
    /// The vertex shader writes oPts / a D3DDECLUSAGE_PSIZE output.
    bool writes_point_size{false};
    /// One past the highest `c#` the shader reads, so the caller knows how
    /// many float4 constants the uniform buffer must carry.
    u32 float_constant_count{0};
    /// One past the highest runtime `i#` read (excludes `defi` immediates).
    u32 int_constant_count{0};
    /// One past the highest runtime `b#` read (excludes `defb` immediates).
    u32 bool_constant_count{0};
    /// Vertex shader declared `dcl_positiont`. oPos is then screen-space
    /// XYZRHW and the draw path must append viewport size after the c[] file
    /// so the translator can map it into Metal clip space.
    bool needs_viewport{false};
};

/// Translate a D3D9 shader token stream.
///
/// @param tokens      Start of the stream, at the version token.
/// @param token_count Number of 32-bit tokens available.
/// @param stage_ttf   Optional `D3DTSS_TEXTURETRANSFORMFLAGS` per stage
///                    (ps_1_1–1_3 honour PROJECTED at sample time from this
///                    runtime state, not from the texldp opcode bit).
/// @param stage_ttf_count Length of @p stage_ttf (stages beyond are treated as 0).
[[nodiscard]] Sm3Shader translate_sm3_to_msl(
    const u32* tokens, usize token_count,
    const u32* stage_ttf = nullptr, usize stage_ttf_count = 0);

/// Fold D3DRS_ALPHATESTENABLE into a translated pixel shader. Alpha test is
/// a render state, not an opcode, so a `vs+ps` pair otherwise ignores it and
/// leaves the discarded texels (foliage cards, fences, sprites) on screen.
/// `cmp_func` is a D3DCMPFUNC; ALWAYS returns `msl` unchanged. The draw path
/// appends `float4 alpha_test` (ref in `.x`) after the padded c/i/b file.
[[nodiscard]] std::string apply_sm3_alpha_test(std::string msl, u32 cmp_func);

/// Fold D3DSAMP_SRGBTEXTURE into a translated pixel shader. The state is a
/// sampler state, not an opcode, so a translated `texld` otherwise returns the
/// raw gamma-space texel and every surface sampled through it renders far too
/// bright. `sampler_mask` has bit N set for each sampler whose texture must be
/// gamma-decoded on read.
///
/// Deviation from hardware, deliberate and documented: real hardware decodes
/// each texel before filtering, this decodes the filtered result. The two
/// differ only in the weighting of a filtered edge, and expressing the exact
/// order would require an sRGB-typed view of every guest texture format.
[[nodiscard]] std::string apply_sm3_srgb_texture(std::string msl,
                                                 u32 sampler_mask);

/// Fold D3DRS_SRGBWRITEENABLE into a translated pixel shader: the result is
/// gamma-encoded on the way to the render target. Applied to the shader output
/// rather than to the blend result, which is where D3D9 puts it -- blending
/// then operates on encoded values.
[[nodiscard]] std::string apply_sm3_srgb_write(std::string msl);

/// Fold D3DRS_FOGENABLE vertex fog into a translated pixel shader. Fog is
/// applied after the pixel shader, so a `vs+ps` pair otherwise ignores it
/// and leaves distant objects unfogged. The VS must write oFog; the draw
/// path appends `float4 fog_color` after any alpha-test uniform.
[[nodiscard]] std::string apply_sm3_fog(std::string msl, bool vertex_fog);

/// Apply D3D9 pixel/table fog after a translated pixel shader. `fog_mode` is
/// D3DFOG_EXP/EXP2/LINEAR; W fog uses reciprocal fragment RHW.
[[nodiscard]] std::string apply_sm3_table_fog(
    std::string msl, u32 fog_mode, bool w_fog);

/// Fold D3DRS_SHADEMODE=D3DSHADE_FLAT into a translated shader's COLOR
/// varyings. Direct3D 9 applies flat interpolation to D3DDECLUSAGE_COLOR even
/// when both shader stages are programmable.
[[nodiscard]] std::string apply_sm3_flat_shading(std::string msl);

/// Add Metal's required point-size output to a translated vertex shader.
/// `shader_writes_point_size` selects oPts.x; otherwise point_params.x is the
/// clamped D3DRS_POINTSIZE fallback. point_params.yz hold the Windows min/max.
[[nodiscard]] std::string apply_sm3_point_size(
    std::string msl, bool shader_writes_point_size);

/// Add six Metal clip-distance outputs to a translated vertex shader. D3D9
/// programmable clip planes are already expressed in clip space.
[[nodiscard]] std::string apply_sm3_clip_planes(std::string msl);

constexpr u32 SM3_MAX_SAMPLERS = 16;

/// Apply D3D9 sampler MIPMAPLODBIAS to every sample from the corresponding
/// sampler. Explicit texldb/texldl instruction bias is additive.
[[nodiscard]] std::string apply_sm3_sampler_lod_bias(
    std::string msl,
    const std::array<float, SM3_MAX_SAMPLERS>& lod_biases);

/// The largest float constant file a D3D9 shader can address. The generated
/// MSL declares only `float_constant_count` of these, because a full 256-entry
/// file is 4 KiB — the entire inline-constant budget of one draw.
constexpr u32 SM3_MAX_FLOAT_CONSTANT_REGISTERS = 256;
/// D3D9 integer / boolean constant files (Set*ShaderConstantI/B).
constexpr u32 SM3_MAX_INT_CONSTANT_REGISTERS = 16;
constexpr u32 SM3_MAX_BOOL_CONSTANT_REGISTERS = 16;

} // namespace vitreo::graphics

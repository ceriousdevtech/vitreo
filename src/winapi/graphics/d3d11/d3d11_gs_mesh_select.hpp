// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_gs_mesh_select.hpp
/// @brief WS5: which path a D3D11 geometry-shader draw takes -- one in-pass
///        capture plus a Metal mesh draw, or the three-pass emulation
///        (host-offload plan WS5, Task 7).
///
/// The choice is total and static: a pure function of what the geometry
/// shader is (GsMeshStatic, decided at creation), what the draw is
/// (GsDrawFacts), the device, and the kill switch / test force. Everything
/// the mesh path cannot draw bit-identically to the three-pass path returns
/// ThreePass with the reason; the force overrides only the default choice,
/// never a contract reason.

#include "graphics/shaders/msl_mesh_emit.hpp"

#include <vitreo/common.hpp>

#include <array>

namespace vitreo::winapi::gs_mesh {

enum class GsPath : u8 { ThreePass, Mesh };
enum class Force : int { None = 0, Mesh = 1, ThreePass = 2 };

/// Tests and benchmarks only: override the default choice.
void force_for_test(Force) noexcept;
[[nodiscard]] Force forced() noexcept;
/// VITREO_WS5=0 (read once): every GS draw takes the three-pass path.
[[nodiscard]] bool kill_switch_off() noexcept;

/// Decided at CreateGeometryShader.
struct GsMeshStatic {
    bool eligible{false};
    const char* why{"not built"};
    graphics::GsMeshFit fit{};
    graphics::GsMeshLayout layout{};
};

// d3d11.h values (D3D11_BLEND_OP, D3D11_COMPARISON_FUNC, D3D11_STENCIL_OP).
inline constexpr u8 kBlendOpMin = 4;
inline constexpr u8 kBlendOpMax = 5;
inline constexpr u8 kComparisonNever = 1;
inline constexpr u8 kComparisonLess = 2;
inline constexpr u8 kComparisonEqual = 3;
inline constexpr u8 kComparisonLessEqual = 4;
inline constexpr u8 kComparisonGreater = 5;
inline constexpr u8 kComparisonNotEqual = 6;
inline constexpr u8 kComparisonGreaterEqual = 7;
inline constexpr u8 kComparisonAlways = 8;
inline constexpr u8 kStencilOpKeep = 1;

struct RenderTargetFacts {
    bool bound{false};
    u8 write_mask{0};
    bool blend{false};
    u8 op_color{1};
    u8 op_alpha{1};
    /// Blendable and neither float nor snorm: Min/Max give the same bits in
    /// any order (float: -0/+0 and NaN; snorm: two encodings of -1).
    bool minmax_exact{false};
    bool logic_op{false};
};

/// Gathered per draw from the context state.
struct GsDrawFacts {
    bool device_supports_mesh{false};
    bool indirect{false};
    /// Geometry-shader invocations this draw runs (input primitives x draw
    /// instances x the shader's instance count); 0 when not known.
    u64 invocations{0};
    bool statistics_query{false};
    bool stream_output{false};
    bool line_quads{false};
    bool wireframe{false};
    bool ps_reads_builtin_primitive_id{false};
    /// The pixel shader reads SV_RenderTargetArrayIndex / SV_ViewportArrayIndex
    /// and the geometry shader does NOT write that value. One it writes is
    /// the mesh primitive's own [[render_target_array_index]] /
    /// [[viewport_array_index]], which the fragment stage reads back as the
    /// same builtin on either path.
    bool ps_reads_builtin_layer_or_viewport{false};
    bool ps_inputs_covered{true};
    bool ps_uav{false};
    bool ps_writes_coverage{false};
    bool alpha_to_coverage{false};
    std::array<RenderTargetFacts, 8> rts{};
    bool depth_enable{false};
    bool depth_write{false};
    u8 depth_func{kComparisonLess};
    bool stencil_enable{false};
    u8 stencil_write_mask{0};
    /// front fail / depth-fail / pass, then back.
    std::array<u8, 6> stencil_ops{1, 1, 1, 1, 1, 1};
};

/// True only where the final value of every sample does not depend on the
/// order fragments arrive in, bit for bit (Apple documents no rasterization
/// order across mesh threadgroups; D3D11 requires input-primitive order).
[[nodiscard]] bool order_independent(const GsDrawFacts&) noexcept;

/// True when the whole draw is ONE mesh: every invocation fits the shader's
/// threadgroup (GsMeshFit::invocations_per_group). A mesh's primitives are
/// rasterized in primitive-index order, and the mesh lays them out
/// invocation-major -- input primitive, then GS instance, then emit order,
/// which is D3D11's order (Learn, geometry-shader stage: "ordering is
/// respected"; plan G4) -- so such a draw is ordered whatever it blends.
/// The undocumented order is only the one ACROSS threadgroups.
[[nodiscard]] bool single_mesh(const GsMeshStatic&, const GsDrawFacts&) noexcept;

struct GsPathChoice {
    GsPath path;
    const char* why;
};
[[nodiscard]] GsPathChoice select_gs_path(const GsMeshStatic&,
                                          const GsDrawFacts&) noexcept;

}  // namespace vitreo::winapi::gs_mesh

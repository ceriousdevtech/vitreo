// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_draw_dump.hpp
/// @brief /tmp/vitreo-draw-dump: dump the inputs of the draws whose pixel
///        shader takes a named semantic, to the process's Vitreo log.
///
/// DIAGNOSTIC, log only. A guest launched by a store client does not reliably
/// inherit environment variables, so the VITREO_D3D11_DUMP_DRAW family cannot
/// reach it; this is its marker-file form. The file's contents select the
/// draws, as whitespace-separated key=value tokens:
///
///   ps_semantic=WAVE_PARAM_INDEX max=4
///
///   ps_semantic  keep only draws whose bound pixel shader's INPUT signature
///                names this semantic (case-insensitive; `TEXCOORD` matches
///                any index, `TEXCOORD3` that one). Absent: every draw.
///   max          how many draws to dump (default 16).
///
/// The budget restarts whenever the marker is created again or its contents
/// change. Each dumped draw writes `[draw-dump] pid=<pid> draw=<n> <what> ...`
/// lines (d3d11_draw_dump.cpp): the draw call, shaders and targets, the pixel
/// shader's input signature, viewport and scissor, rasterizer, blend and
/// depth-stencil state, input layout and vertex/index buffers, every bound
/// vertex and pixel constant buffer slice with its bytes, and the vertex and
/// pixel shader-resource views.

#include "graphics/shaders/dxbc_parser.hpp"

#include <vitreo/types.hpp>

#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace vitreo::winapi::d3d11 {

inline constexpr u32 kDefaultDrawDumpMax = 16;

struct DrawDumpConfig {
    /// Empty: no semantic filter.
    std::string ps_semantic;
    u32 max_draws{kDefaultDrawDumpMax};
    /// Tokens this parser did not understand, space separated ("" when the
    /// file was clean). They select nothing and are reported once.
    std::string ignored;
};

[[nodiscard]] DrawDumpConfig parse_draw_dump_config(std::string_view text);

/// Whether @p signature names @p semantic: the element's SemanticName equal to
/// it (ASCII case-insensitive), or SemanticName followed by SemanticIndex
/// ("TEXCOORD3" for TEXCOORD, index 3).
[[nodiscard]] bool signature_names_semantic(
    std::span<const graphics::DxbcSignatureElement> signature,
    std::string_view semantic);

/// The dump budget: draws taken since the marker's generation @p generation
/// began. Thread-safe.
class DrawDumpBudget {
public:
    /// Take one draw of @p max under @p generation; false when the budget is
    /// spent. A new generation restarts it.
    [[nodiscard]] bool take(u64 generation, u32 max);
    /// How many were taken under the current generation.
    [[nodiscard]] u32 taken() const;
    /// Whether any is left under @p generation (a cheap pre-check).
    [[nodiscard]] bool has_room(u64 generation, u32 max) const;
    /// Lock-free: the budget of @p generation is spent. A draw after that
    /// costs this one load while the marker stays.
    [[nodiscard]] bool full(u64 generation) const noexcept {
        return m_full_generation.load(std::memory_order_relaxed) == generation;
    }

private:
    mutable std::mutex m_mutex;
    u64 m_generation{~u64{0}};
    u32 m_taken{0};
    std::atomic<u64> m_full_generation{~u64{0}};
};

/// The process-wide budget of the marker.
[[nodiscard]] DrawDumpBudget& draw_dump_budget() noexcept;

}  // namespace vitreo::winapi::d3d11

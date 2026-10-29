// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_pipeline.cpp
/// @brief D3D11 draw execution (execute_draw) and the D3D11-to-backend state
///        translators it uses.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_pipeline_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "winapi/graphics/d3d11/d3d11_draw_memo.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/graphics/common/live_diag.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "winapi/graphics/common/d3d_sampler.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/gs_passthrough.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <vitreo/diagnostics.hpp>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <limits>
#include <map>
#include <tuple>
#include <mutex>
#include <vector>
#include <cctype>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <mach/mach_time.h>

namespace vitreo::winapi {

namespace {

// Learn D3D11_RTV_DIMENSION: TEXTURE3D. d3d11_internal.hpp cannot be included
// here — its unqualified IID constants collide with this file's.
constexpr u32 kD3D11RtvDimensionTexture3D = 8;

// Bounded bring-up diagnostic for draw rejection. Unlike the full D3D11 trace,
// this records only failures and therefore remains usable across a scene
// transition with tens of thousands of successful draws.
void pipeline_reject_diag(const char* format, ...) {
    static const bool enabled = std::getenv("VITREO_D3D11_REJECT_DIAG") != nullptr;
    if (!enabled) return;
    static unsigned emitted = 0;
    flockfile(stderr);
    if (emitted++ >= 96) {
        funlockfile(stderr);
        return;
    }
    std::fprintf(stderr, "[d3d11-reject pid=%d] ", static_cast<int>(::getpid()));
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    funlockfile(stderr);
}

} // namespace

namespace {

/// The bits each component of a UINT render-target format holds (0 for a
/// component the format lacks), for the logic-op variant; false for any
/// other format (D3D11.3 17.7: logic ops are defined on UINT formats only).
bool uint_component_masks(graphics::PixelFormat format,
                          std::array<u32, 4>& masks) {
    using graphics::PixelFormat;
    const auto set = [&masks](u32 r, u32 g, u32 b, u32 a) {
        masks = {r, g, b, a};
        return true;
    };
    switch (format) {
    case PixelFormat::R8Uint: return set(0xFFu, 0, 0, 0);
    case PixelFormat::RG8Uint: return set(0xFFu, 0xFFu, 0, 0);
    case PixelFormat::RGBA8Uint: return set(0xFFu, 0xFFu, 0xFFu, 0xFFu);
    case PixelFormat::R16Uint: return set(0xFFFFu, 0, 0, 0);
    case PixelFormat::RG16Uint: return set(0xFFFFu, 0xFFFFu, 0, 0);
    case PixelFormat::RGBA16Uint:
        return set(0xFFFFu, 0xFFFFu, 0xFFFFu, 0xFFFFu);
    case PixelFormat::R32Uint: return set(~0u, 0, 0, 0);
    case PixelFormat::RG32Uint: return set(~0u, ~0u, 0, 0);
    case PixelFormat::RGBA32Uint: return set(~0u, ~0u, ~0u, ~0u);
    case PixelFormat::RGB10A2Uint: return set(0x3FFu, 0x3FFu, 0x3FFu, 0x3u);
    default: masks = {}; return false;
    }
}

} // namespace

namespace {

[[nodiscard]] graphics::BackendBlendFactor blend_factor_from_d3d11(u32 v) {
    switch (v) {
    case 16: return graphics::BackendBlendFactor::Source1Color;
    case 17: return graphics::BackendBlendFactor::OneMinusSource1Color;
    case 18: return graphics::BackendBlendFactor::Source1Alpha;
    case 19: return graphics::BackendBlendFactor::OneMinusSource1Alpha;
    case 1:
        return graphics::BackendBlendFactor::Zero;
    case 2:
        return graphics::BackendBlendFactor::One;
    case 3:
        return graphics::BackendBlendFactor::SourceColor;
    case 4:
        return graphics::BackendBlendFactor::OneMinusSourceColor;
    case 5:
        return graphics::BackendBlendFactor::SourceAlpha;
    case 6:
        return graphics::BackendBlendFactor::OneMinusSourceAlpha;
    case 7:
        return graphics::BackendBlendFactor::DestinationAlpha;
    case 8:
        return graphics::BackendBlendFactor::OneMinusDestinationAlpha;
    case 9:
        return graphics::BackendBlendFactor::DestinationColor;
    case 10:
        return graphics::BackendBlendFactor::OneMinusDestinationColor;
    case 11:
        return graphics::BackendBlendFactor::SourceAlphaSaturated;
    case 14:
        return graphics::BackendBlendFactor::BlendColor;
    case 15:
        return graphics::BackendBlendFactor::OneMinusBlendColor;
    default:
        return graphics::BackendBlendFactor::One;
    }
}

[[nodiscard]] graphics::BackendBlendOperation blend_operation_from_d3d11(
    u32 value) {
    switch (value) {
    case 2: return graphics::BackendBlendOperation::Subtract;
    case 3: return graphics::BackendBlendOperation::ReverseSubtract;
    case 4: return graphics::BackendBlendOperation::Min;
    case 5: return graphics::BackendBlendOperation::Max;
    default: return graphics::BackendBlendOperation::Add;
    }
}

[[nodiscard]] graphics::BackendPrimitiveTopology topology_from_d3d11(u32 t) {
    switch (t) {
    case 1:
        return graphics::BackendPrimitiveTopology::PointList;
    case 2:
        return graphics::BackendPrimitiveTopology::LineList;
    case 3:
        return graphics::BackendPrimitiveTopology::LineStrip;
    case 5:
        return graphics::BackendPrimitiveTopology::TriangleStrip;
    default:
        return graphics::BackendPrimitiveTopology::TriangleList;
    }
}

/// The primitive class a topology rasterizes as with no geometry shader:
/// adjacency topologies draw only their own lines / triangles (D3D11.3 8.15).
[[nodiscard]] graphics::BackendPrimitiveTopology rasterized_topology(u32 t) {
    switch (t) {
    case 10:
    case 11:
        return graphics::BackendPrimitiveTopology::LineList;
    case 12:
    case 13:
        return graphics::BackendPrimitiveTopology::TriangleList;
    default:
        return topology_from_d3d11(t);
    }
}

[[nodiscard]] bool semantic_names_equal(const std::string& a,
                                        const std::string& b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

[[nodiscard]] graphics::BackendCompare compare_from_d3d11(u32 v) {
    switch (v) {
    case 1:
        return graphics::BackendCompare::Never;
    case 2:
        return graphics::BackendCompare::Less;
    case 3:
        return graphics::BackendCompare::Equal;
    case 4:
        return graphics::BackendCompare::LessEqual;
    case 5:
        return graphics::BackendCompare::Greater;
    case 6:
        return graphics::BackendCompare::NotEqual;
    case 7:
        return graphics::BackendCompare::GreaterEqual;
    case 8:
        return graphics::BackendCompare::Always;
    default:
        return graphics::BackendCompare::LessEqual;
    }
}

[[nodiscard]] graphics::BackendStencilOperation stencil_op_from_d3d11(u32 v) {
    using Op = graphics::BackendStencilOperation;
    switch (v) {
    case 1: return Op::Keep;
    case 2: return Op::Zero;
    case 3: return Op::Replace;
    case 4: return Op::IncrementClamp;
    case 5: return Op::DecrementClamp;
    case 6: return Op::Invert;
    case 7: return Op::IncrementWrap;
    case 8: return Op::DecrementWrap;
    default: return Op::Keep;
    }
}

} // namespace

graphics::BackendBlendFactor d3d11_backend_blend_factor(u32 value) {
    return blend_factor_from_d3d11(value);
}

bool D3D11Manager::draw_compile_may_release_lock(GuestAddr context) {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_D3D11_DRAW_UNLOCK");
        return value == nullptr || std::strcmp(value, "0") != 0;
    }();
    if (!enabled || m_devices.size() != 1) return false;
    const Device* device = find_device(context);
    return device != nullptr && !device->multithread_protected;
}

void D3D11Manager::execute_draw(GuestAddr context, bool indexed,
                                u32 count, u32 start_location,
                                i32 base_vertex, u32 instance_count,
                                u32 first_instance,
                                GuestAddr indirect_args, u32 indirect_offset,
                                DrawVariant* variant,
                                const IndirectDraw& host_indirect) {
    const bool guest_draw = std::exchange(m_draw_may_release_lock, false);
    m_draw_memo_mask = d3d11::draw_memo::active_mask();  // R4
    auto state_it = m_context_states.find(context);
    if (state_it == m_context_states.end()) {
        pipeline_diag("Draw skipped: no context state");
        return;
    }
    // An indirect draw has no CPU-side counts to check: the GPU wrote them and
    // zero is a legal value for it to have written. Only a direct draw can be
    // rejected here.
    // Vitreo's own argument blocks (DrawAuto's, an emulation pass's) come
    // as a backend buffer; a guest's as its DRAWINDIRECT_ARGS buffer.
    const graphics::BackendBuffer* indirect_buffer = nullptr;
    const bool indirect = indirect_args != 0 || host_indirect.active();
    if (host_indirect.active()) {
        indirect_buffer = host_indirect.args;
        indirect_offset = host_indirect.offset;
    } else if (indirect_args != 0) {
        indirect_buffer = resolve_indirect_args(
            indirect_args, indirect_offset,
            indexed ? "DrawIndexedInstancedIndirect" : "DrawInstancedIndirect",
            // D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS: 5 UINTs;
            // D3D11_DRAW_INSTANCED_INDIRECT_ARGS: 4.
            (indexed ? 5u : 4u) * static_cast<u32>(sizeof(u32)));
        if (indirect_buffer == nullptr) return;
    } else if ((variant == nullptr ||
                (variant->kind != DrawVariant::Kind::GeometryOutput &&
                 variant->kind != DrawVariant::Kind::Tessellated)) &&
               (count == 0 || instance_count == 0)) {
        // Pass C of the geometry emulation carries its count in the variant
        // (the whole expansion region), not in `count`.
        pipeline_diag("Draw skipped: zero vertex/instance count");
        static const bool zero_stats = d3d11_skip_stats_enabled();
        if (zero_stats) {
            struct Key {
                GuestAddr vs, ps;
                u32 flags;
                bool operator<(const Key& o) const {
                    return std::tie(vs, ps, flags) < std::tie(o.vs, o.ps, o.flags);
                }
            };
            static std::mutex zmu;
            static auto* hist = new std::map<Key, u64>();
            static auto last = std::chrono::steady_clock::now();
            const auto& st = state_it->second;
            const u32 flags = (indexed ? 1u : 0u) | (count == 0 ? 2u : 0u) |
                              (instance_count == 0 ? 4u : 0u);
            std::lock_guard<std::mutex> lk(zmu);
            ++(*hist)[Key{st.vertex_shader, st.pixel_shader, flags}];
            const auto now = std::chrono::steady_clock::now();
            if (now - last >= std::chrono::seconds(5)) {
                last = now;
                std::vector<std::pair<Key, u64>> rows(hist->begin(), hist->end());
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                    return a.second > b.second;
                });
                for (usize i = 0; i < rows.size() && i < 12; ++i) {
                    std::fprintf(stderr,
                                 "[d3d11-zero] n=%llu indexed=%u zero_count=%u "
                                 "zero_inst=%u vs=0x%llx ps=0x%llx\n",
                                 static_cast<unsigned long long>(rows[i].second),
                                 rows[i].first.flags & 1u,
                                 (rows[i].first.flags >> 1) & 1u,
                                 (rows[i].first.flags >> 2) & 1u,
                                 static_cast<unsigned long long>(rows[i].first.vs),
                                 static_cast<unsigned long long>(rows[i].first.ps));
                }
                hist->clear();
            }
        }
        return;
    }
    auto& state = state_it->second;
    // ---- P0 state census (VITREO_D3D11_STATE_CENSUS=1, DIAGNOSTIC).
    if (d3d11_census::enabled && variant == nullptr) [[unlikely]] {
        census_guest_draw(context, state, indexed, indirect);
    }
    // ---- end P0 state census
    // PIPELINE_STATISTICS counts the guest's draw once, before any emulation
    // pass (capture, geometry expansion, patch draw) re-enters here. A draw
    // that reaches the rasterizer directly also captures its vertex
    // shader's outputs for the clipper counts; a draw that then never
    // reaches the backend leaves them uncounted, which the query reports.
    StatisticsDraw statistics{};
    if (variant == nullptr) {
        count_pipeline_draw(context, indexed, count, start_location,
                            base_vertex, instance_count, indirect_buffer,
                            indirect_offset, statistics);
    }
    struct StatisticsGuard {
        D3D11Manager* self;
        GuestAddr context;
        StatisticsDraw& plan;
        ~StatisticsGuard() {
            if (!plan.active) return;
            self->mark_statistics_unmeasured(
                context, 1u << graphics::STAT_C_PRIMITIVES,
                "CPrimitives of a draw that did not reach the backend");
        }
    } statistics_guard{this, context, statistics};

    const bool is_capture =
        variant != nullptr && variant->kind == DrawVariant::Kind::Capture;
    const bool is_geometry_output =
        variant != nullptr &&
        variant->kind == DrawVariant::Kind::GeometryOutput;
    const bool is_tessellated =
        variant != nullptr && variant->kind == DrawVariant::Kind::Tessellated;
    // Pass C drawing lines as the rectangles D3D rasterizes
    // (execute_line_quads, geometry_expand).
    const bool line_quad_pass =
        is_geometry_output && variant->line_quad_divisor != 0;

    // A bound geometry shader changes what a draw is. A geometry PROGRAM runs
    // as capture -> compute expansion -> draw of the expansion (11.16). A
    // stream-output one streams the draw first (capture + repack) and then
    // rasterizes as an ordinary draw only if its rasterized stream is 0.
    // Both re-enter this function with a DrawVariant for their own draws.
    // Hull and domain stages (11.17) run as capture -> hull kernel -> patch
    // draw. A patch-list topology with no tessellation stages is an invalid
    // draw, and so is either stage without the other: nothing is drawn.
    if (variant == nullptr) {
        // The geometry, hull and domain stages run through their own
        // translations (lowered kernels), which have no R32G32B32
        // typed-buffer variant (rgb32_buffer_srv_mask).
        bool beyond = false;
        const u32 emulated_rgb32 =
            (state.geometry_shader != 0
                 ? rgb32_buffer_srv_mask(state.geometry_shader_resources,
                                         beyond)
                 : 0u) |
            (state.hull_shader != 0
                 ? rgb32_buffer_srv_mask(state.hull_shader_resources, beyond)
                 : 0u) |
            (state.domain_shader != 0
                 ? rgb32_buffer_srv_mask(state.domain_shader_resources,
                                         beyond)
                 : 0u);
        // Their kernels are translated without border twins either: a
        // sampler whose border colour Metal cannot express samples there
        // with a transparent-black border.
        if ((state.geometry_shader != 0 &&
             border_twin_mask(state.geometry_samplers) != 0) ||
            (state.hull_shader != 0 &&
             border_twin_mask(state.hull_samplers) != 0) ||
            (state.domain_shader != 0 &&
             border_twin_mask(state.domain_samplers) != 0)) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d11!Draw: a sampler "
                             "with a BORDER colour other than transparent "
                             "black or opaque white (comparing: a red other "
                             "than 0 or 1) bound to a geometry, hull or "
                             "domain shader samples a transparent-black "
                             "border\n");
            }
        }
        if (emulated_rgb32 != 0 || beyond) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d11!Draw: an R32G32B32 "
                             "typed buffer view bound to a geometry, hull or "
                             "domain shader\n");
            }
            return;
        }
        // Nor a 2D-declared variant of a multisample view of a one-sample
        // texture (single_sample_ms_srv_mask): Metal cannot bind that view
        // to their texture2d_ms.
        bool ms1_beyond = false;
        const u32 emulated_ms1 =
            (state.geometry_shader != 0
                 ? single_sample_ms_srv_mask(state.geometry_shader_resources,
                                             ms1_beyond)
                 : 0u) |
            (state.hull_shader != 0
                 ? single_sample_ms_srv_mask(state.hull_shader_resources,
                                             ms1_beyond)
                 : 0u) |
            (state.domain_shader != 0
                 ? single_sample_ms_srv_mask(state.domain_shader_resources,
                                             ms1_beyond)
                 : 0u);
        if (emulated_ms1 != 0 || ms1_beyond) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d11!Draw: a "
                             "TEXTURE2DMS view of a one-sample texture bound "
                             "to a geometry, hull or domain shader\n");
            }
            return;
        }
    }
    if (variant == nullptr &&
        (state.hull_shader != 0 || state.domain_shader != 0 ||
         (state.primitive_topology >= 33 && state.primitive_topology <= 64))) {
        if (state.hull_shader != 0 || state.domain_shader != 0) {
            execute_tessellation(context, indexed, count, start_location,
                                 base_vertex, instance_count, first_instance,
                                 IndirectDraw{indirect_buffer,
                                              indirect_offset});
        } else {
            pipeline_diag("Draw skipped: a patch-list topology needs hull and "
                          "domain shaders");
        }
        return;
    }
    if (variant == nullptr && state.geometry_shader != 0) {
        const auto gs = m_geometry_shaders.find(state.geometry_shader);
        if (gs != m_geometry_shaders.end() && gs->second.program) {
            execute_geometry(context, indexed, count, start_location,
                             base_vertex, instance_count, first_instance,
                             IndirectDraw{indirect_buffer, indirect_offset});
            return;
        }
        if (!execute_stream_out(context, indexed, count, start_location,
                                base_vertex, instance_count, first_instance,
                                IndirectDraw{indirect_buffer,
                                             indirect_offset})) {
            return;
        }
    }

    // Lines -- or triangles in WIREFRAME fill -- in the quadrilateral or
    // alpha-antialiased mode are drawn as the rectangles D3D rasterizes
    // (execute_line_quads). What it cannot serve (an indirect draw, or one
    // it declines) is drawn aliased, reported below.
    if (variant == nullptr && !indirect &&
        line_rasterization_mode(state) != 0) {
        bool wireframe = false;
        if (const auto rs = m_rasterizer_states.find(state.rasterizer_state);
            state.rasterizer_state != 0 && rs != m_rasterizer_states.end()) {
            wireframe = rs->second.desc.fill_mode == 2u;
        }
        const u32 topology = state.primitive_topology;
        const bool lines = topology == 2 || topology == 3 || topology == 10 ||
                           topology == 11;
        const bool triangles =
            wireframe && (topology == 4 || topology == 5 || topology == 12 ||
                          topology == 13);
        if ((lines || triangles) &&
            execute_line_quads(context, indexed, count, start_location,
                               base_vertex, instance_count, first_instance,
                               line_rasterization_mode(state))) {
            return;
        }
    }
    auto vs = m_vertex_shaders.find(state.vertex_shader);
    auto ps = m_pixel_shaders.find(state.pixel_shader);
    // The capture draw runs the vertex stage only.
    const bool pixel_shader_disabled =
        state.pixel_shader == 0 || is_capture;
    if (vs == m_vertex_shaders.end() || vs->second.msl_source.empty() ||
        (!pixel_shader_disabled &&
         (ps == m_pixel_shaders.end() || ps->second.msl_source.empty()))) {
        pipeline_diag("Draw skipped: required shader MSL missing");
        return;
    }

    std::array<graphics::BackendTexture2D*,
               graphics::BACKEND_MAX_RENDER_TARGETS> color_targets{};
    std::array<graphics::PixelFormat,
               graphics::BACKEND_MAX_RENDER_TARGETS> color_formats{};
    std::array<u32, graphics::BACKEND_MAX_RENDER_TARGETS> color_mip_slices{};
    std::array<u32, graphics::BACKEND_MAX_RENDER_TARGETS> color_array_slices{};
    std::array<u32, graphics::BACKEND_MAX_RENDER_TARGETS> color_array_sizes{
        1, 1, 1, 1, 1, 1, 1, 1};
    std::array<graphics::BackendTexture3D*,
               graphics::BACKEND_MAX_RENDER_TARGETS> color_targets_3d{};
    std::array<u32, graphics::BACKEND_MAX_RENDER_TARGETS> color_depth_planes{};
    // The RTV's DXGI format per slot, for rules the host format cannot
    // express (render_target_has_no_alpha).
    std::array<u32, graphics::BACKEND_MAX_RENDER_TARGETS> color_view_formats{};
    if (is_capture) {
        // Rasterization is off, so nothing reaches this target; it exists
        // because a pass needs one, and a streaming draw commonly binds no
        // render target at all.
        color_targets[0] = variant->target;
        color_formats[0] = graphics::PixelFormat::RGBA8Unorm;
    }
    // Buffer views bound as targets: their proxies are filled from the
    // buffers before the pass and copied back after it.
    std::array<GuestAddr, graphics::BACKEND_MAX_RENDER_TARGETS>
        buffer_target_views{};
    // ---- R4 om-memo (d3d11_draw_memo.hpp): the attachments the bound
    // RTVs and DSV resolve to, keyed by their stamps and the texture-backend
    // generation (a texture's backend object replaced under a live view).
    const bool om_memo =
        !is_capture && memo_on(d3d11::draw_memo::Part::OmMemo);
    const bool om_verify = om_memo && d3d11::draw_memo::verify();
    OmMemo& om = m_om_memo;
    const bool om_keys_match =
        om.valid && om.rt_stamp == state.render_targets.stamp() &&
        om.dsv_stamp == state.depth_stencil_view.stamp() &&
        om.backend_gen == m_texture_backend_gen;
    const bool om_hit = om_memo && !om_verify &&
                        (om_keys_match ||
                         (om.valid && d3d11::draw_memo::ignore_keys()));
    std::array<Texture2D*, graphics::BACKEND_MAX_RENDER_TARGETS> rt_diag{};
    // A render target's diagnostics: who drew into it last, and the
    // (VS, GS, PS) set the rt-dump lists. A memo hit replays them.
    const auto note_rt_diag = [&](Texture2D& texture) {
        ++texture.diag_rt_draws;
        texture.diag_rt_writer = state.pixel_shader;
        texture.diag_rt_writer_vs = state.vertex_shader;
        texture.diag_rt_writer_gs = state.geometry_shader;
        if (m_diag_draw_pairs_armed) [[unlikely]] {
            note_diag_draw_pair(texture, state);
        }
    };
    if (om_hit) {
        ++m_r4_om_hits;
        color_targets = om.color_targets;
        color_formats = om.color_formats;
        color_mip_slices = om.color_mip_slices;
        color_array_slices = om.color_array_slices;
        color_array_sizes = om.color_array_sizes;
        color_targets_3d = om.color_targets_3d;
        color_depth_planes = om.color_depth_planes;
        color_view_formats = om.color_view_formats;
        buffer_target_views = om.buffer_target_views;
        for (Texture2D* tex : om.rt_diag) {
            if (tex != nullptr) note_rt_diag(*tex);
        }
    } else {
        if (om_memo) ++m_r4_om_misses;
        for (u32 slot = 0; slot < state.render_targets.size() &&
                           !is_capture;
             ++slot) {
            if (state.render_targets[slot] == 0) continue;
            auto rtv = m_render_target_views.find(state.render_targets[slot]);
            if (rtv == m_render_target_views.end()) continue;
            if (rtv->second.buffer != 0) {
                // D3D11_RTV_DIMENSION_BUFFER: a NumElements x 1 target
                // (sync_buffer_render_target).
                if (!rtv->second.buffer_proxy || slot >= color_targets.size()) {
                    continue;
                }
                color_targets[slot] = rtv->second.buffer_proxy.get();
                color_formats[slot] = backend_format(rtv->second.format);
                color_view_formats[slot] = rtv->second.format;
                buffer_target_views[slot] = rtv->first;
                continue;
            }
            auto tex = m_textures.find(rtv->second.texture);
            if (tex == m_textures.end()) continue;
            // A Texture3D view (D3D11_TEX3D_RTV) covers the W slices
            // [FirstWSlice, FirstWSlice + WSize) of one mip. A draw renders into
            // FirstWSlice unless its vertex/geometry stage writes
            // SV_RenderTargetArrayIndex, which then selects a slice relative to
            // FirstWSlice (an index outside the view renders slice 0 of it). UE
            // builds its colour-grading LUT this way: one instanced quad per W
            // slice, routed by the index.
            if (rtv->second.view_dimension == kD3D11RtvDimensionTexture3D) {
                if (!tex->second.backend_3d) continue;
                rt_diag[slot] = &tex->second;  // R4 om-memo replays these
                note_rt_diag(tex->second);
                color_targets_3d[slot] = tex->second.backend_3d.get();
                color_depth_planes[slot] = rtv->second.first_array_slice;
                color_formats[slot] = backend_format(rtv->second.format);
                color_view_formats[slot] = rtv->second.format;
                color_mip_slices[slot] = rtv->second.mip_slice;
                color_array_sizes[slot] = std::max(1u, rtv->second.array_size);
                continue;
            }
            if (tex->second.backend) {
                rt_diag[slot] = &tex->second;  // R4 om-memo replays these
                note_rt_diag(tex->second);
                color_formats[slot] = backend_format(rtv->second.format);
                // A chroma view of a planar video texture renders into its
                // half-size chroma plane (Learn DXGI_FORMAT NV12).
                color_targets[slot] =
                    tex->second.backend->view_plane(color_formats[slot]);
                color_view_formats[slot] = rtv->second.format;
                color_mip_slices[slot] = rtv->second.mip_slice;
                color_array_slices[slot] = rtv->second.first_array_slice;
                color_array_sizes[slot] = rtv->second.array_size;
            }
        }
    }
    // ---- end R4 (the DSV half follows)
    // Attachment geometry, whichever resource kind backs the slot. A volume
    // attachment is never multisampled.
    const auto color_slot_width = [&](u32 slot) -> unsigned {
        if (color_targets_3d[slot] != nullptr) {
            return std::max(1u, color_targets_3d[slot]->desc().width >>
                                    color_mip_slices[slot]);
        }
        return std::max(1u, color_targets[slot]->desc().width >>
                                color_mip_slices[slot]);
    };
    const auto color_slot_height = [&](u32 slot) -> unsigned {
        if (color_targets_3d[slot] != nullptr) {
            return std::max(1u, color_targets_3d[slot]->desc().height >>
                                    color_mip_slices[slot]);
        }
        return std::max(1u, color_targets[slot]->desc().height >>
                                color_mip_slices[slot]);
    };
    const auto color_slot_samples = [&](u32 slot) -> unsigned {
        if (color_targets_3d[slot] != nullptr) return 1u;
        return std::max(1u, color_targets[slot]->desc().sample_count);
    };
    const auto color_slot_bound = [&](u32 slot) {
        return color_targets[slot] != nullptr ||
               color_targets_3d[slot] != nullptr;
    };
    graphics::BackendTexture2D* color0 = color_targets[0];
    const bool color0_bound = color_slot_bound(0);
    const graphics::PixelFormat color_format = color_formats[0];

    graphics::BackendTexture2D* depth = nullptr;
    u32 depth_mip_slice = 0;
    u32 depth_array_slice = 0;
    u32 depth_array_size = 1;
    u32 depth_view_flags = 0;
    Texture2D* depth_diag = nullptr;
    if (om_hit) {
        depth_diag = om.depth_diag;
        depth = om.depth;
        depth_mip_slice = om.depth_mip_slice;
        depth_array_slice = om.depth_array_slice;
        depth_array_size = om.depth_array_size;
        depth_view_flags = om.depth_view_flags;
    } else {
        if (state.depth_stencil_view != 0 && !is_capture) {
            auto dsv = m_depth_stencil_views.find(state.depth_stencil_view);
            if (dsv != m_depth_stencil_views.end()) {
                auto tex = m_textures.find(dsv->second.texture);
                if (tex != m_textures.end() && tex->second.backend) {
                    depth = tex->second.backend.get();
                    depth_mip_slice = dsv->second.mip_slice;
                    depth_array_slice = dsv->second.first_array_slice;
                    depth_array_size = dsv->second.array_size;
                    depth_view_flags = dsv->second.flags;
                    depth_diag = &tex->second;
                }
            }
        }
    }
    if (depth_diag != nullptr && m_diag_draw_pairs_armed) [[unlikely]] {
        note_diag_draw_pair(*depth_diag, state);
    }
    // ---- R4 om-memo: keep what this draw resolved (verify: compare first).
    if (om_memo && !om_hit) {
        if (om_verify && om_keys_match &&
            (om.color_targets != color_targets ||
             om.color_formats != color_formats ||
             om.color_mip_slices != color_mip_slices ||
             om.color_array_slices != color_array_slices ||
             om.color_array_sizes != color_array_sizes ||
             om.color_targets_3d != color_targets_3d ||
             om.color_depth_planes != color_depth_planes ||
             om.color_view_formats != color_view_formats ||
             om.buffer_target_views != buffer_target_views ||
             om.depth != depth || om.depth_mip_slice != depth_mip_slice ||
             om.depth_array_slice != depth_array_slice ||
             om.depth_array_size != depth_array_size ||
             om.depth_view_flags != depth_view_flags)) {
            std::fprintf(stderr, "[r4] VERIFY FAILED: the memoised "
                                 "attachments differ from the resolved ones\n");
            std::abort();
        }
        om.valid = true;
        om.rt_stamp = state.render_targets.stamp();
        om.dsv_stamp = state.depth_stencil_view.stamp();
        om.backend_gen = m_texture_backend_gen;
        om.color_targets = color_targets;
        om.color_formats = color_formats;
        om.color_mip_slices = color_mip_slices;
        om.color_array_slices = color_array_slices;
        om.color_array_sizes = color_array_sizes;
        om.color_targets_3d = color_targets_3d;
        om.color_depth_planes = color_depth_planes;
        om.color_view_formats = color_view_formats;
        om.buffer_target_views = buffer_target_views;
        om.rt_diag = rt_diag;
        om.depth_diag = depth_diag;
        om.depth = depth;
        om.depth_mip_slice = depth_mip_slice;
        om.depth_array_slice = depth_array_slice;
        om.depth_array_size = depth_array_size;
        om.depth_view_flags = depth_view_flags;
    }
    if (om_memo &&
        d3d11::draw_memo::count_tick(m_r4_om_hits + m_r4_om_misses)) {
        std::fprintf(stderr, "[r4] om-memo hits=%llu misses=%llu\n",
                     static_cast<unsigned long long>(m_r4_om_hits),
                     static_cast<unsigned long long>(m_r4_om_misses));
    }
    // ---- end R4
    const graphics::BackendTexture2D* primary_target =
        color0 != nullptr ? color0 : depth;
    bool any_color_bound = false;
    for (u32 slot = 0; slot < color_targets.size(); ++slot) {
        any_color_bound |= color_slot_bound(slot);
    }
    // Learn (OMSetRenderTargetsAndUnorderedAccessViews; D3D11.1 "UAV-only
    // rendering", D3D11_RASTERIZER_DESC1::ForcedSampleCount): a draw with no
    // RTV and no DSV still rasterizes. The viewport bounds it, the pixel
    // shader's UAV writes are its output, and it runs at ForcedSampleCount
    // samples (one when that is zero). Dropping these draws left UE's
    // lighting/compositing targets unwritten -- read back as magenta.
    // VITREO_D3D11_NO_ATTACHMENTLESS=1: drop these draws as before (bisection
    // knob while a GPU hang is being isolated).
    static const bool no_attachmentless =
        std::getenv("VITREO_D3D11_NO_ATTACHMENTLESS") != nullptr;
    const bool attachmentless = !no_attachmentless &&
        primary_target == nullptr && !any_color_bound && !is_capture;
    if (primary_target == nullptr && !color0_bound && !attachmentless) {
        pipeline_diag("Draw skipped: no render or depth-stencil target");
        return;
    }
    u32 forced_sample_count = 0;
    if (state.rasterizer_state != 0) {
        const auto rs = m_rasterizer_states.find(state.rasterizer_state);
        if (rs != m_rasterizer_states.end()) {
            forced_sample_count = rs->second.desc.forced_sample_count;
        }
    }
    unsigned attachmentless_samples =
        attachmentless && forced_sample_count != 0 ? forced_sample_count : 1u;
    if (attachmentless && attachmentless_samples > 1 &&
        !m_backend.supports_texture_sample_count(attachmentless_samples)) {
        // D3D11.3 3.5.6.4: UAV-only rasterization at ForcedSampleCount 8 (and
        // 16 at FL 11_1). Metal rasterizes at most at the sample counts the
        // device supports (rasterSampleCount; 8 is refused on Apple GPUs), so
        // this device reports UAVOnlyRenderingForcedSampleCount FALSE, which
        // makes the draw's result undefined (3.5.6.4) -- it is not drawn.
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Draw: UAV-only "
                         "rendering at ForcedSampleCount %u (a sample count "
                         "the GPU cannot rasterize)\n",
                         attachmentless_samples);
        }
        pipeline_diag("Draw skipped: UAV-only ForcedSampleCount %u",
                      attachmentless_samples);
        return;
    }
    // D3D11_VIEWPORT_BOUNDS_MAX is 32767, but the rasterizer never produces a
    // pixel beyond D3D11_REQ_RENDER_TO_BUFFER_WINDOW_WIDTH (16384).
    const auto attachmentless_extent = [&](float origin, float size) {
        const float end = std::ceil(origin + size);
        return static_cast<unsigned>(
            std::clamp(end, 1.0f, 16384.0f));
    };
    bool dual_source = false;
    if (const auto blend = m_blend_states.find(state.blend_state);
        !is_capture && blend != m_blend_states.end() &&
        blend->second.desc.blend_enable[0]) {
        const auto& desc = blend->second.desc;
        for (u32 factor : {desc.src_blend[0], desc.dest_blend[0],
                           desc.src_blend_alpha[0], desc.dest_blend_alpha[0]}) {
            dual_source |= factor >= 16 && factor <= 19;
        }
    }
    if (dual_source) {
        bool extra_targets = false;
        for (u32 slot = 1; slot < color_targets.size(); ++slot) {
            extra_targets |= color_slot_bound(slot);
        }
        if (pixel_shader_disabled || !color0_bound || extra_targets) {
            pipeline_diag("Draw rejected: dual-source blending requires PS and RT0 only");
            return;
        }
        auto& shader = ps->second;
        if (!shader.dual_source_compile_attempted) {
            shader.dual_source_compile_attempted = true;
            if (!compile_shader_to_msl(shader.bytecode.data(), shader.bytecode.size(),
                                       false, shader.dual_source_msl, true)) {
                pipeline_diag("Dual-source pixel shader translation failed");
            }
        }
        if (shader.dual_source_msl.empty()) return;
    }
    // Border colours Metal cannot express take a shader variant that samples
    // through border twins (border_twin_mask), compiled once per mask.
    const u32 ps_twins = pixel_shader_disabled
        ? 0u : sampler_stage_summary(0, state.pixel_samplers).border_twins;
    const u32 vs_twins = is_geometry_output
        ? 0u : sampler_stage_summary(1, state.vertex_samplers).border_twins;
    // R32G32B32 typed buffer views, read through a variant as three R32
    // texels per element (rgb32_buffer_srv_mask). The vertex stage of a
    // geometry-output or tessellated draw is not the guest's VS.
    bool rgb32_unservable = false;
    bool ms1_unservable = false;
    u32 ps_rgb32 = 0;
    u32 vs_rgb32 = 0;
    u32 ps_ms1 = 0;
    u32 vs_ms1 = 0;
    if (!pixel_shader_disabled) {
        const SrvStageSummary& ps_srvs =
            srv_stage_summary(0, state.pixel_shader_resources);
        ps_rgb32 = ps_srvs.rgb32;
        ps_ms1 = ps_srvs.ms1;
        rgb32_unservable = rgb32_unservable || ps_srvs.rgb32_unservable;
        ms1_unservable = ms1_unservable || ps_srvs.ms1_unservable;
    }
    if (!is_geometry_output && !is_tessellated) {
        const SrvStageSummary& vs_srvs =
            srv_stage_summary(1, state.vertex_shader_resources);
        vs_rgb32 = vs_srvs.rgb32;
        vs_ms1 = vs_srvs.ms1;
        rgb32_unservable = rgb32_unservable || vs_srvs.rgb32_unservable;
        ms1_unservable = ms1_unservable || vs_srvs.ms1_unservable;
    }
    if (rgb32_unservable) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Draw: an R32G32B32 "
                         "typed buffer view at t32 or above\n");
        }
        return;
    }
    // Multisample views of one-sample textures, read through a variant
    // declaring them 2D (single_sample_ms_srv_mask).
    if (ms1_unservable) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Draw: a TEXTURE2DMS "
                         "view of a one-sample texture at t32 or above\n");
        }
        return;
    }
    // A line-quad pass's pixel shader: SV_PrimitiveID counts D3D
    // primitives, and an alpha-antialiased line scales o0.a by its coverage
    // (DxbcTranslationOptions::line_quad_primitive_divisor /
    // line_coverage_alpha).
    const u32 ps_line_divisor =
        line_quad_pass && !pixel_shader_disabled ? variant->line_quad_divisor
                                                 : 0u;
    const bool ps_line_coverage = line_quad_pass && !pixel_shader_disabled &&
                                  variant->line_coverage_alpha;
    // MINIMUM_ / MAXIMUM_ samplers: the variant reducing in the shader
    // (minmax_sampler_mask).
    const u32 ps_minmax = pixel_shader_disabled
        ? 0u : sampler_stage_summary(0, state.pixel_samplers).minmax;
    const u32 vs_minmax = is_geometry_output || is_tessellated
        ? 0u : sampler_stage_summary(1, state.vertex_samplers).minmax;
    const u64 ps_srv_key = static_cast<u64>(ps_ms1) |
                           (static_cast<u64>(ps_line_divisor) << 32) |
                           (ps_line_coverage ? (1ull << 40) : 0ull) |
                           (static_cast<u64>(ps_minmax) << 44);
    const unsigned sample_count = attachmentless ? attachmentless_samples
        : color0_bound
        ? color_slot_samples(0)
        : std::max(1u, primary_target->desc().sample_count);
    // D3D11.3 17.17: SampleMask "is always applied, regardless of whether
    // Multisample rendertargets are bound", its first n bits masking an
    // n-sample target's coverage (bit 0 gates a single-sample write). Metal
    // has no pipeline sample mask, so a mask that clears a sample the
    // target has takes a pixel-shader variant writing [[sample_mask]] (or a
    // mask-only fragment function when no pixel shader is bound).
    const u32 target_samples_mask = sample_count >= 32
        ? 0xFFFFFFFFu : ((1u << sample_count) - 1u);
    const bool ps_sample_mask = !is_capture &&
        (state.sample_mask & target_samples_mask) != target_samples_mask;
    // D3D11.3 17.7 output-merger logic op (CreateBlendState1; RT[0]'s desc
    // applies to every target, IndependentBlendEnable being FALSE): Metal
    // has none, so a pixel-shader variant reads each UINT target through
    // framebuffer fetch and writes op(output, target) with blending off.
    // Logic ops exist only on UINT formats; any other target takes the
    // shader's output unchanged (17.7: "undefined rendering otherwise").
    LogicOpVariant logic{};
    if (!is_capture && !pixel_shader_disabled && state.blend_state != 0) {
        const auto blend = m_blend_states.find(state.blend_state);
        if (blend != m_blend_states.end() &&
            blend->second.desc.logic_op_enable[0] != 0) {
            logic.op = blend->second.desc.logic_op[0];
            for (u32 slot = 0; slot < color_formats.size(); ++slot) {
                if (color_slot_bound(slot) &&
                    uint_component_masks(color_formats[slot],
                                         logic.component_masks[slot])) {
                    logic.targets |= 1u << slot;
                }
            }
        }
    }
    if (!pixel_shader_disabled && sample_count > 1 && depth != nullptr &&
        ps->second.early_depth_uav_writes) {
        // The translated shader performs a pixel's UAV side effects in the
        // invocation at its lowest covered sample (Metal runs a fragment
        // function that writes memory once per covered sample). Under
        // [earlydepthstencil] a sample the depth/stencil test rejects is
        // never invoked, and Metal cannot tell the shader which samples
        // passed ([[sample_mask]] stays the raster coverage, even with
        // post_depth_coverage; measured on Apple9) -- so where exactly the
        // lowest covered sample fails, D3D's one invocation (16.10) is lost.
        bool tested = true;  // the default state has DepthEnable TRUE
        if (state.depth_stencil_state != 0) {
            const auto dss = m_depth_stencil_states.find(state.depth_stencil_state);
            if (dss != m_depth_stencil_states.end()) {
                tested = dss->second.desc.depth_enable != 0 ||
                         dss->second.desc.stencil_enable != 0;
            }
        }
        static bool reported = false;
        if (tested && !reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Draw: [earlydepthstencil] "
                         "pixel shader UAV writes on a multisampled target are "
                         "skipped where the depth/stencil test rejects the "
                         "pixel's lowest covered sample\n");
        }
    }
    // PIPELINE_STATISTICS building here: the pixel shader counts its
    // invocations (DxbcTranslationOptions::count_pixel_invocations) into
    // the interval's record. A NULL pixel shader counts nothing (D3D11.3
    // 20.4.7).
    const std::shared_ptr<graphics::BackendBuffer> pixel_statistics =
        !pixel_shader_disabled ? statistics_record_for_draw(context) : nullptr;
    const bool count_pixels = pixel_statistics != nullptr;
    const std::string* twin_fragment = nullptr;
    if (logic.targets != 0) {
        auto& shader = ps->second;
        const u64 base_key = ps_twins | (dual_source ? 0x10000u : 0u) |
                             (ps_sample_mask ? 0x20000u : 0u) |
                             (count_pixels ? 0x40000u : 0u) |
                             (static_cast<u64>(ps_rgb32) << 32);
        std::string key = std::to_string(base_key) + ":" +
                          std::to_string(ps_srv_key) + ":" +
                          std::to_string(logic.op) + ":" +
                          std::to_string(logic.targets);
        for (const auto& masks : logic.component_masks) {
            for (const u32 bits : masks) key += ":" + std::to_string(bits);
        }
        auto found = shader.logic_variants.find(key);
        if (found == shader.logic_variants.end()) {
            std::string msl;
            if (!compile_shader_to_msl(shader.bytecode.data(),
                                       shader.bytecode.size(), false, msl,
                                       dual_source, nullptr, ps_twins,
                                       ps_sample_mask, ps_rgb32, &logic,
                                       count_pixels, ps_ms1,
                                       ps_line_coverage,
                                       ps_line_divisor, ps_minmax)) {
                pipeline_diag("Pixel shader logic-op variant translation "
                              "failed");
            }
            found = shader.logic_variants.emplace(key, std::move(msl)).first;
        }
        if (found->second.empty()) return;
        twin_fragment = &found->second;
    } else if (ps_twins != 0 || ps_rgb32 != 0 || count_pixels ||
        ps_srv_key != 0 || (ps_sample_mask && !pixel_shader_disabled)) {
        auto& shader = ps->second;
        const std::pair<u64, u64> key{
            ps_twins | (dual_source ? 0x10000u : 0u) |
                (ps_sample_mask ? 0x20000u : 0u) |
                (count_pixels ? 0x40000u : 0u) |
                (static_cast<u64>(ps_rgb32) << 32),
            ps_srv_key};
        auto found = shader.border_variants.find(key);
        if (found == shader.border_variants.end()) {
            std::string msl;
            if (!compile_shader_to_msl(shader.bytecode.data(),
                                       shader.bytecode.size(), false, msl,
                                       dual_source, nullptr, ps_twins,
                                       ps_sample_mask, ps_rgb32, nullptr,
                                       count_pixels, ps_ms1,
                                       ps_line_coverage,
                                       ps_line_divisor, ps_minmax)) {
                pipeline_diag("Pixel shader variant translation failed");
            }
            found = shader.border_variants.emplace(key, std::move(msl)).first;
        }
        if (found->second.empty()) return;
        twin_fragment = &found->second;
    }
    // No pixel shader: the mask still limits which samples the draw's depth
    // and stencil writes reach, so a fragment function writes only it.
    static const std::string mask_only_fragment_source =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "struct vitreo_mask_out { uint mask [[sample_mask]]; };\n"
        "fragment vitreo_mask_out main0(constant uint* vitreo_push "
        "[[buffer(0)]]) {\n"
        "    return { vitreo_push[" +
        std::to_string(graphics::kFixedFunctionOffset / 4u + 1u) +
        "u] };\n"
        "}\n";
    const std::string empty_fragment_source;
    const std::string& fragment_source = pixel_shader_disabled
        ? (ps_sample_mask ? mask_only_fragment_source : empty_fragment_source)
        : twin_fragment != nullptr
        ? *twin_fragment
        : (dual_source ? ps->second.dual_source_msl : ps->second.msl_source);
    // Metal needs the drawn primitive class on a pipeline whose vertex stage
    // writes the render-target array index; always supplying it keeps the
    // rule local. A capture pass rasterizes nothing and leaves it open.
    const graphics::BackendPrimitiveClass input_class = is_capture
        ? graphics::BackendPrimitiveClass::Unspecified
        : is_tessellated ? graphics::BackendPrimitiveClass::Triangle
        : graphics::primitive_class_of(is_geometry_output
            ? variant->topology
            : rasterized_topology(state.primitive_topology));
    const unsigned target_width = attachmentless
        ? attachmentless_extent(state.viewports[0][0], state.viewports[0][2])
        : color0_bound
        ? color_slot_width(0)
        : std::max(1u, primary_target->desc().width >> depth_mip_slice);
    const unsigned target_height = attachmentless
        ? attachmentless_extent(state.viewports[0][1], state.viewports[0][3])
        : color0_bound
        ? color_slot_height(0)
        : std::max(1u, primary_target->desc().height >> depth_mip_slice);
    for (u32 slot = 1; slot < color_targets.size(); ++slot) {
        if (!color_slot_bound(slot)) continue;
        if (color_slot_samples(slot) != sample_count ||
            color_slot_width(slot) != target_width ||
            color_slot_height(slot) != target_height ||
            color_array_sizes[slot] != color_array_sizes[0]) {
            pipeline_diag(
                "Draw skipped: MRT slot %u dimensions/sample mismatch", slot);
            return;
        }
    }
    if (depth != nullptr) {
        const unsigned depth_width =
            std::max(1u, depth->desc().width >> depth_mip_slice);
        const unsigned depth_height =
            std::max(1u, depth->desc().height >> depth_mip_slice);
        const unsigned target_array_size =
            color0_bound ? color_array_sizes[0] : depth_array_size;
        if (std::max(1u, depth->desc().sample_count) != sample_count ||
            depth_width != target_width || depth_height != target_height ||
            depth_array_size != target_array_size) {
            pipeline_diag(
                "Draw skipped: depth dimensions/sample mismatch");
            return;
        }
    }

    // ---- R4c vb-memo: the backend buffer behind each IA slot.
    const ResolvedVbs* vb_memo =
        memo_on(d3d11::draw_memo::Part::VbMemo)
            ? &resolved_vertex_buffers(state.vertex_buffers)
            : nullptr;
    // ---- end R4c
    const auto vertex_slot_bound = [&](u32 slot) {
        if (slot >= state.vertex_buffers.size() ||
            state.vertex_buffers[slot] == 0) {
            return false;
        }
        if (vb_memo != nullptr) return vb_memo->buffers[slot] != nullptr;
        const auto found = m_buffers.find(state.vertex_buffers[slot]);
        return found != m_buffers.end() && found->second.backend != nullptr;
    };
    std::vector<graphics::BackendVertexAttribute> attributes_built;
    // R4c: the attributes a draw uses -- the ia-memo's own list on a hit (no
    // per-draw copy), else attributes_built.
    const std::vector<graphics::BackendVertexAttribute>* attributes_in_use =
        &attributes_built;
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS> strides{};
    std::array<bool, graphics::BACKEND_MAX_VERTEX_BUFFERS> per_instance{};
    std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS> step_rates{};
    std::array<bool, graphics::BACKEND_MAX_VERTEX_BUFFERS> constant_slots{};
    // Backend stream -> IA slot. The input layout fixed this at creation
    // (InputLayout::stream_slot): IA slots 8-31 ride on streams the layout's
    // low slots leave free. A stream the layout does not claim is its own
    // slot, which is also the whole mapping when no layout is bound.
    const auto bound_layout = m_input_layouts.find(state.input_layout);
    const InputLayout* input_layout = bound_layout != m_input_layouts.end()
        ? &bound_layout->second : nullptr;
    std::array<u32, graphics::BACKEND_MAX_VERTEX_BUFFERS> stream_slot{};
    for (u32 stream = 0; stream < stream_slot.size(); ++stream) {
        stream_slot[stream] =
            input_layout != nullptr &&
                    input_layout->stream_slot[stream] !=
                        InputLayout::kNoBackendStream
                ? input_layout->stream_slot[stream]
                : stream;
        strides[stream] = state.vertex_buffer_strides[stream_slot[stream]];
    }
    if (state.input_layout != 0) {
        auto layout = m_input_layouts.find(state.input_layout);
        if (layout != m_input_layouts.end()) {
            std::array<unsigned, graphics::BACKEND_MAX_VERTEX_BUFFERS>
                inferred{};
            // ---- R4 ia-memo (d3d11_draw_memo.hpp): what the layout and
            // the vertex shader's input signature make of each element,
            // kept on the layout (immutable) for the shader it last met
            // (VertexShader::serial, never reused).
            const bool ia_memo =
                memo_on(d3d11::draw_memo::Part::IaMemo);
            InputLayout::IaMemo& ia = layout->second.ia_memo;
            const bool ia_verify = ia_memo && d3d11::draw_memo::verify();
            if (ia_memo && !ia_verify && ia.valid &&
                (ia.vs_serial == vs->second.serial ||
                 d3d11::draw_memo::ignore_keys())) {
                ++m_r4_ia_hits;
                if (ia.unservable) {
                    pipeline_diag("Draw skipped: IA slot %u has no backend "
                                  "vertex stream", ia.unservable_slot);
                    return;
                }
                attributes_in_use = &ia.attributes;
                per_instance = ia.per_instance;
                step_rates = ia.step_rates;
                inferred = ia.inferred;
            } else {
                if (ia_memo) ++m_r4_ia_misses;
                unsigned location = 0;
                for (usize i = 0; i < layout->second.elements.size(); ++i) {
                    const auto& el = layout->second.elements[i];
                    unsigned shader_location = location;
                    u8 shader_component_type = 3u;
                    if (!el.semantic_name.empty() &&
                        !vs->second.input_signature.empty()) {
                        bool matched = false;
                        for (const auto& sig : vs->second.input_signature) {
                            if (sig.semantic_index == el.semantic_index &&
                                semantic_names_equal(sig.semantic_name,
                                                     el.semantic_name)) {
                                shader_location = sig.register_num;
                                shader_component_type = sig.component_type;
                                matched = true;
                                break;
                            }
                        }
                        if (!matched) continue;
                    }
                    const u8 stream = el.input_slot < d3d11::VERTEX_BUFFER_SLOTS
                        ? layout->second.backend_stream[el.input_slot]
                        : InputLayout::kNoBackendStream;
                    if (stream == InputLayout::kNoBackendStream) {
                        // Only when a layout feeds the shader from more distinct
                        // IA slots than the backend has streams. The binding is
                        // recorded and reads back; the draw cannot fetch it.
                        static std::once_flag once;
                        std::call_once(once, [&] {
                            std::fprintf(stderr,
                                         "[vitreo] UNIMPLEMENTED d3d11!Draw: input "
                                         "layout reads IA slot %u, but its slots "
                                         "need more than %u backend vertex "
                                         "streams\n",
                                         el.input_slot,
                                         graphics::BACKEND_MAX_VERTEX_BUFFERS);
                        });
                        pipeline_diag("Draw skipped: IA slot %u has no backend "
                                      "vertex stream", el.input_slot);
                        if (ia_memo) {  // R4 ia-memo
                            ia.valid = true;
                            ia.vs_serial = vs->second.serial;
                            ia.unservable = true;
                            ia.unservable_slot = el.input_slot;
                        }
                        return;
                    }
                    graphics::BackendVertexAttribute attr{};
                    attr.shader_location = shader_location;
                    ++location;
                    attr.offset = el.aligned_byte_offset;
                    attr.buffer_index = stream;
                    attr.format = d3d11::vertex_format_from_dxgi(el.format);
                    attr.storage_component_type =
                        d3d11::vertex_storage_component_type(el.format);
                    attr.shader_component_type = shader_component_type == 1u
                        ? graphics::BackendVertexComponentType::UInt
                        : shader_component_type == 2u
                            ? graphics::BackendVertexComponentType::SInt
                            : graphics::BackendVertexComponentType::Float;
                    attributes_built.push_back(attr);
                    if (el.input_slot_class == 1) {
                        // 0 is a real rate: the slot never steps (D3D11.3 8.3.1).
                        per_instance[stream] = true;
                        step_rates[stream] = el.instance_data_step_rate;
                    }
                    const unsigned end =
                        attr.offset + d3d11::vertex_format_stride(attr.format);
                    if (end > inferred[stream]) {
                        inferred[stream] = end;
                    }
                }
                if (ia_memo) {
                    const auto same_attr =
                        [](const graphics::BackendVertexAttribute& a,
                           const graphics::BackendVertexAttribute& b) {
                            return a.shader_location == b.shader_location &&
                                   a.offset == b.offset &&
                                   a.format == b.format &&
                                   a.buffer_index == b.buffer_index &&
                                   a.storage_component_type ==
                                       b.storage_component_type &&
                                   a.shader_component_type ==
                                       b.shader_component_type;
                        };
                    if (ia_verify && ia.valid &&
                        ia.vs_serial == vs->second.serial &&
                        (ia.unservable ||
                         ia.attributes.size() != attributes_built.size() ||
                         !std::equal(attributes_built.begin(),
                                     attributes_built.end(),
                                     ia.attributes.begin(), same_attr) ||
                         ia.per_instance != per_instance ||
                         ia.step_rates != step_rates ||
                         ia.inferred != inferred)) {
                        std::fprintf(stderr,
                                     "[r4] VERIFY FAILED: the memoised input "
                                     "assembly differs from the recomputed "
                                     "one\n");
                        std::abort();
                    }
                    ia.valid = true;
                    ia.vs_serial = vs->second.serial;
                    ia.unservable = false;
                    ia.attributes = attributes_built;
                    ia.per_instance = per_instance;
                    ia.step_rates = step_rates;
                    ia.inferred = inferred;
                }
            }
            if (ia_memo && d3d11::draw_memo::count_tick(m_r4_ia_hits +
                                                         m_r4_ia_misses)) {
                std::fprintf(stderr, "[r4] ia-memo hits=%llu misses=%llu\n",
                             static_cast<unsigned long long>(m_r4_ia_hits),
                             static_cast<unsigned long long>(m_r4_ia_misses));
            }
            // ---- end R4
            for (unsigned slot = 0; slot < strides.size(); ++slot) {
                // A slot the layout reads that the guest bound with stride 0
                // is a constant stream (IASetVertexBuffers accepts 0): every
                // vertex and instance reads the element at the offset.
                // Inferring a stride from the layout made each instance
                // step through the buffer instead -- UE's per-draw instance
                // offset stream then handed every instance another mesh's
                // scene data.
                if (strides[slot] == 0 && inferred[slot] != 0) {
                    constant_slots[slot] = true;
                }
                // A slot the layout reads with no buffer bound is a buffer
                // of size 0 (D3D11.3 8.19.2): every fetch is out of bounds
                // and returns 0. The draw binds zeros there, read as a
                // constant stream (measured: d3d11_a07_ia_probe `B slot1
                // unbound` fetches 0 for every instance).
                if (inferred[slot] != 0 && !vertex_slot_bound(slot)) {
                    constant_slots[slot] = true;
                }
                if (strides[slot] == 0) strides[slot] = inferred[slot];
            }
        }
    }
    if (is_geometry_output || is_tessellated) {
        // Pass C reads every vertex from the expansion buffer by vertex id,
        // and a patch draw's vertex function (the domain shader) reads its
        // control points from the hull kernel's buffers: no input layout, no
        // vertex streams. Matching the guest's layout against the domain
        // shader instead "specialized" a record with no bytecode, and every
        // patch-list draw whose layout had an integer element was skipped
        // (d3d11_a07_ia_probe `D patchlist`).
        attributes_built.clear();
        attributes_in_use = &attributes_built;
        strides.fill(0);
        per_instance.fill(false);
        step_rates.fill(0);
        constant_slots.fill(false);
    }
    if (strides[0] == 0) strides[0] = 16;
    const unsigned stride = strides[0];

    bool blend_enabled = false;
    graphics::BackendBlendFactor src_blend = graphics::BackendBlendFactor::One;
    graphics::BackendBlendFactor dst_blend =
        graphics::BackendBlendFactor::Zero;
    std::array<unsigned, graphics::BACKEND_MAX_RENDER_TARGETS> write_masks{
        0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF};
    std::array<bool, graphics::BACKEND_MAX_RENDER_TARGETS> blend_enables{};
    std::array<graphics::BackendBlendFactor,
               graphics::BACKEND_MAX_RENDER_TARGETS> source_blends{};
    std::array<graphics::BackendBlendFactor,
               graphics::BACKEND_MAX_RENDER_TARGETS> destination_blends{};
    std::array<graphics::BackendBlendOperation,
               graphics::BACKEND_MAX_RENDER_TARGETS> blend_operations{};
    std::array<graphics::BackendBlendFactor,
               graphics::BACKEND_MAX_RENDER_TARGETS> source_blends_alpha{};
    std::array<graphics::BackendBlendFactor,
               graphics::BACKEND_MAX_RENDER_TARGETS> destination_blends_alpha{};
    std::array<graphics::BackendBlendOperation,
               graphics::BACKEND_MAX_RENDER_TARGETS> blend_operations_alpha{};
    bool independent_blend = false;
    bool alpha_to_coverage = false;
    unsigned write_mask = write_masks[0];
    if (state.blend_state != 0) {
        auto blend = m_blend_states.find(state.blend_state);
        if (blend != m_blend_states.end()) {
            independent_blend =
                blend->second.desc.independent_blend_enable != 0;
            // D3D11.3 17.18: from o0.a, and disabled when the pixel shader
            // writes oMask. Metal takes the alpha of the colour(0) output of
            // a float attachment 0; with none bound, the coverage cannot be
            // formed from it.
            alpha_to_coverage =
                blend->second.desc.alpha_to_coverage_enable != 0 &&
                !pixel_shader_disabled && !ps->second.writes_coverage;
            if (alpha_to_coverage &&
                (!color0_bound ||
                 graphics::is_integer_format(color_formats[0]))) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED d3d11!Draw: "
                                 "AlphaToCoverageEnable without a float "
                                 "render target 0\n");
                }
                alpha_to_coverage = false;
            }
            blend_enabled = blend->second.desc.blend_enable[0] != 0;
            src_blend =
                blend_factor_from_d3d11(blend->second.desc.src_blend[0]);
            dst_blend =
                blend_factor_from_d3d11(blend->second.desc.dest_blend[0]);
            write_mask =
                blend->second.desc.render_target_write_mask[0] & 0xFu;
            write_masks.fill(write_mask);
            if (blend->second.desc.independent_blend_enable != 0) {
                for (u32 slot = 0; slot < write_masks.size(); ++slot) {
                    write_masks[slot] =
                        blend->second.desc.render_target_write_mask[slot] & 0xFu;
                }
            }
            for (u32 slot = 0; slot < blend_enables.size(); ++slot) {
                const u32 source_slot = independent_blend ? slot : 0;
                blend_enables[slot] =
                    blend->second.desc.blend_enable[source_slot] != 0;
                source_blends[slot] = blend_factor_from_d3d11(
                    blend->second.desc.src_blend[source_slot]);
                destination_blends[slot] = blend_factor_from_d3d11(
                    blend->second.desc.dest_blend[source_slot]);
                blend_operations[slot] = blend_operation_from_d3d11(
                    blend->second.desc.blend_op[source_slot]);
                source_blends_alpha[slot] = blend_factor_from_d3d11(
                    blend->second.desc.src_blend_alpha[source_slot]);
                destination_blends_alpha[slot] = blend_factor_from_d3d11(
                    blend->second.desc.dest_blend_alpha[source_slot]);
                blend_operations_alpha[slot] = blend_operation_from_d3d11(
                    blend->second.desc.blend_op_alpha[source_slot]);
            }
        }
    }

    // A B8G8R8X8 render target is BGRA8 storage whose fourth byte is not a
    // component: the output merger never writes it, and a blend reads the
    // missing destination alpha as 1 (D3D11.3 functional spec §19.1), so
    // DEST_ALPHA is ONE, INV_DEST_ALPHA and SRC_ALPHA_SAT (min(As, 1 - Ad))
    // are ZERO for the colour channels.
    for (u32 slot = 0; slot < color_view_formats.size(); ++slot) {
        if (!d3d11::render_target_has_no_alpha(color_view_formats[slot])) {
            continue;
        }
        const auto opaque_destination = [](graphics::BackendBlendFactor f) {
            using F = graphics::BackendBlendFactor;
            switch (f) {
            case F::DestinationAlpha: return F::One;
            case F::OneMinusDestinationAlpha: return F::Zero;
            case F::SourceAlphaSaturated: return F::Zero;
            default: return f;
            }
        };
        independent_blend = true;
        write_masks[slot] &= ~0x8u;
        source_blends[slot] = opaque_destination(source_blends[slot]);
        destination_blends[slot] =
            opaque_destination(destination_blends[slot]);
    }

    u64 pipeline_key = 0;
    // ---- R4 key-memo (d3d11_draw_memo.hpp): the key's inputs are gathered
    // first and the key mixed from them only when they differ from the last
    // draw's -- the same inputs give the same key, so this needs no
    // invalidation at all. The mixing is a serial dependency chain over ~150
    // values; gathering and comparing them is not.
    constexpr usize kKeyInputs = 512;
    u64 key_inputs[kKeyInputs];
    usize key_input_count = 0;
    // Past the buffer (no layout reaches it: at most 32 elements), the
    // inputs go on here and the key is mixed without the memo.
    // One thread-local lookup per draw, not per input.
    static thread_local std::vector<u64> key_overflow_storage;
    std::vector<u64>& key_overflow = key_overflow_storage;
    key_overflow.clear();
    const auto mix = [&key_inputs, &key_input_count,
                      &key_overflow](u64 v) __attribute__((always_inline)) {
        if (key_input_count < kKeyInputs) [[likely]] {
            key_inputs[key_input_count] = v;
        } else {
            key_overflow.push_back(v);
        }
        ++key_input_count;
    };
    // A geometry-output pass's vertex source can be a per-draw copy; every
    // non-empty fragment source is shader-owned (or the static mask-only one).
    // The shaders' own sources carry their hash; variants go through the
    // memo. Same values either way: both are std::hash of the source.
    const auto own_hash = [](const std::string& msl, u64& memo) {
        if (memo == 0) memo = std::hash<std::string>{}(msl);
        return memo;
    };
    mix(is_geometry_output ? msl_hash(*variant->vertex_msl, false)
                           : own_hash(vs->second.msl_source,
                                      vs->second.msl_source_hash));
    mix(!pixel_shader_disabled && &fragment_source == &ps->second.msl_source
            ? own_hash(ps->second.msl_source, ps->second.msl_source_hash)
            : msl_hash(fragment_source, !fragment_source.empty()));
    mix(is_capture ? (variant->capture_relative ? 0x5C4A8u : 0x5C4A7u)
        : is_geometry_output ? 0x6E0A7u
                             : 0u);
    // The vertex shader also captures its outputs (PIPELINE_STATISTICS).
    const bool statistics_capture = statistics.active;
    mix(statistics_capture ? 0x57A7C0u : 0u);
    mix(0xB0DE0000u | vs_twins);
    mix(0x3B320000ull | (static_cast<u64>(vs_rgb32) << 32));
    mix(0x5A510000ull | (static_cast<u64>(vs_ms1) << 32));
    mix(0x313A0000ull | (static_cast<u64>(vs_minmax) << 32));
    mix(blend_enabled ? 1u : 0u);
    mix(independent_blend ? 0x1B1u : 0u);
    mix(static_cast<u64>(src_blend));
    mix(static_cast<u64>(dst_blend));
    mix(write_mask);
    for (u32 slot = 0; slot < color_formats.size(); ++slot) {
        mix(static_cast<u64>(color_formats[slot]));
        mix(write_masks[slot]);
        mix(blend_enables[slot] ? 1u : 0u);
        mix(static_cast<u64>(source_blends[slot]));
        mix(static_cast<u64>(destination_blends[slot]));
        mix(static_cast<u64>(blend_operations[slot]));
        mix(static_cast<u64>(source_blends_alpha[slot]));
        mix(static_cast<u64>(destination_blends_alpha[slot]));
        mix(static_cast<u64>(blend_operations_alpha[slot]));
    }
    mix(sample_count);
    mix(attachmentless ? 0xA77AC4u : 0u);
    mix(0xC1A55000u | static_cast<u64>(input_class));
    if (is_tessellated) {
        mix(0x7E550000u | (static_cast<u64>(variant->tess_partition) << 8) |
            (variant->tess_clockwise ? 1u : 0u));
        mix(variant->tess_max_factor);
    }
    mix(depth != nullptr ? 1u : 0u);
    const graphics::PixelFormat depth_format =
        depth != nullptr ? depth->desc().pixel_format
                         : graphics::PixelFormat::Unknown;
    mix(0xDF000000u | static_cast<u64>(depth_format));
    mix(alpha_to_coverage ? 0xA2C1u : 0u);
    mix(stride);
    for (unsigned slot = 1; slot < strides.size(); ++slot) {
        mix(strides[slot]);
    }
    for (unsigned slot = 0; slot < strides.size(); ++slot) {
        mix(per_instance[slot] ? 1u : 0u);
        mix(step_rates[slot]);
        mix(constant_slots[slot] ? 0xC0u : 0u);
    }
    for (const auto& attr : *attributes_in_use) {
        mix(attr.shader_location);
        mix(attr.offset);
        mix(static_cast<u64>(attr.format));
        mix(attr.buffer_index);
        mix(static_cast<u64>(attr.storage_component_type));
        mix(static_cast<u64>(attr.shader_component_type));
    }

    {
        const auto mix_key = [&] {
            u64 key = 0;
            const auto one = [&key](u64 v) {
                key ^= v + 0x9E3779B97F4A7C15ULL + (key << 6) + (key >> 2);
            };
            for (usize i = 0; i < key_input_count && i < kKeyInputs; ++i) {
                one(key_inputs[i]);
            }
            for (const u64 v : key_overflow) one(v);
            return key;
        };
        const bool fits = key_input_count <= kKeyInputs;
        const bool key_memo =
            fits && memo_on(d3d11::draw_memo::Part::KeyMemo);
        KeyMemo& km = m_key_memo;
        const bool same =
            key_memo && km.valid && km.count == key_input_count &&
            (d3d11::draw_memo::ignore_keys() ||
             std::memcmp(km.inputs.data(), key_inputs,
                         key_input_count * sizeof(u64)) == 0);
        if (same) {
            ++m_r4_key_hits;
            pipeline_key = km.key;
            if (d3d11::draw_memo::verify() &&
                !d3d11::draw_memo::ignore_keys() && mix_key() != km.key) {
                std::fprintf(stderr, "[r4] VERIFY FAILED: the memoised "
                                     "pipeline key differs\n");
                std::abort();
            }
        } else {
            pipeline_key = mix_key();
            if (key_memo) {
                ++m_r4_key_misses;
                km.valid = true;
                km.count = key_input_count;
                std::memcpy(km.inputs.data(), key_inputs,
                            key_input_count * sizeof(u64));
                km.key = pipeline_key;
            }
        }
        if (key_memo &&
            d3d11::draw_memo::count_tick(m_r4_key_hits + m_r4_key_misses)) {
            std::fprintf(stderr, "[r4] key-memo hits=%llu misses=%llu\n",
                         static_cast<unsigned long long>(m_r4_key_hits),
                         static_cast<unsigned long long>(m_r4_key_misses));
        }
    }
    // ---- end R4
    graphics::BackendPipeline* pipeline = nullptr;
    const auto cached = state.pipelines.find(pipeline_key);
    if (cached != state.pipelines.end()) {
        pipeline = cached->second.get();
    } else {
        std::string vertex_source = is_geometry_output ? *variant->vertex_msl
                                                   : vs->second.msl_source;
        graphics::DxbcTranslationOptions translation_options{};
        bool specialize_vertex_inputs = false;
        for (const auto& attr : *attributes_in_use) {
            // R10G10B10A2_UINT: fetched normalized, restored in the shader.
            if (attr.format == graphics::BackendVertexFormat::UInt1010102 &&
                attr.shader_location < 32) {
                translation_options.vertex_input_unpack_uint1010102 |=
                    1u << attr.shader_location;
                specialize_vertex_inputs = true;
                continue;
            }
            if (attr.storage_component_type == attr.shader_component_type ||
                attr.shader_location >=
                    translation_options.vertex_input_component_types.size()) {
                continue;
            }
            specialize_vertex_inputs = true;
            translation_options
                .vertex_input_component_types[attr.shader_location] =
                attr.storage_component_type ==
                        graphics::BackendVertexComponentType::UInt
                    ? 1u
                    : attr.storage_component_type ==
                              graphics::BackendVertexComponentType::SInt
                        ? 2u : 3u;
        }
        // The capture variant is the same translation with one option more:
        // the vertex shader also writes every output into the capture buffer.
        translation_options.capture_vertex_outputs_to_buffer = is_capture;
        translation_options.capture_draw_relative =
            (is_capture && variant->capture_relative) || statistics_capture;
        translation_options.capture_rasterized_vertex_outputs =
            statistics_capture;
        translation_options.border_twin_samplers = vs_twins;
        translation_options.rgb32_buffer_srvs = vs_rgb32;
        translation_options.single_sample_ms_srvs = vs_ms1;
        translation_options.minmax_samplers = vs_minmax;
        translation_options.guard_buffer_bounds = true;
        translation_options.guard_constant_buffers = true;
        translation_options.typed_buffer_bias = true;
        translation_options.fixed_function_push = true;
        if (specialize_vertex_inputs || is_capture || statistics_capture ||
            vs_twins != 0 || vs_rgb32 != 0 || vs_ms1 != 0 || vs_minmax != 0) {
            const auto container = graphics::parse_dxbc(
                vs->second.bytecode.data(), vs->second.bytecode.size());
            // A descriptor-table shader's twins follow the sixteen s# in its
            // sampler table (sync_descriptor_tables).
            if (vs->second.binding_layout.uses_descriptor_tables) {
                translation_options.border_twin_binding_base =
                    d3d11::SAMPLER_SLOTS;
            }
            const auto spirv = graphics::translate_dxbc_to_spirv(
                container, translation_options);
            // The same binding model as the shader's first compile: a vertex
            // shader whose b#/t# exceed the flat windows was compiled with
            // descriptor tables, and a flat recompile here put cb4 at buffer 6
            // on top of t0 ("cannot reserve 'buffer' resource location at
            // index 6"), so the specialised draw silently never ran.
            graphics::MslCompileOptions msl_options = msl_options_for(
                spirv.valid() ? shader_binding_layout(spirv)
                              : d3d11::ShaderBindingLayout{},
                /*is_compute=*/false);
            raise_msl_version_for_typed_uav_atomics(msl_options, spirv);
            const auto compiled = spirv.valid()
                ? graphics::compile_spirv_to_msl(
                      spirv.spirv.data(), spirv.spirv.size(),
                      graphics::ShaderStage::Vertex, msl_options)
                : graphics::CompiledShader{};
            if (!compiled.valid()) {
                pipeline_diag(
                    "Draw skipped: vertex input-layout specialization failed "
                    "(%s)",
                    spirv.valid() ? "SPIR-V to MSL" : spirv.failure.c_str());
                return;
            }
            vertex_source = compiled.msl_text();
        }
        graphics::BackendPipelineDesc desc{};
        desc.vertex_shader_msl = std::move(vertex_source);
        desc.fragment_shader_msl = fragment_source;
        // The vertex function is the VS -- or the DS of a tessellated draw,
        // whose record execute_tessellation swapped in -- unless this is the
        // geometry pass-through, which is generated and does no arithmetic.
        if (!is_geometry_output && vs != m_vertex_shaders.end()) {
            desc.vertex_math_mode = math_mode_for_dxbc(vs->second.bytecode);
            desc.preserve_invariance = dxbc_uses_precise(vs->second.bytecode);
        }
        if (ps != m_pixel_shaders.end()) {
            desc.fragment_math_mode = math_mode_for_dxbc(ps->second.bytecode);
        }
        // SPIRV-Cross renames `main` to `main0` for MSL.
        desc.vertex_entry_point =
            is_geometry_output ? graphics::kGsPassthroughEntryPoint : "main0";
        desc.fragment_entry_point = "main0";
        desc.vertex_attributes = *attributes_in_use;
        desc.vertex_buffer_strides = strides;
        desc.vertex_buffer_per_instance = per_instance;
        desc.vertex_buffer_step_rates = step_rates;
        desc.vertex_buffer_constant = constant_slots;
        desc.vertex_stride = stride;
        desc.color_format = color_format;
        desc.color_formats = color_formats;
        desc.sample_count = sample_count;
        desc.attachmentless = attachmentless;
        desc.input_primitive_class = input_class;
        if (is_tessellated) {
            desc.tessellation = true;
            desc.tessellation_partition = variant->tess_partition;
            desc.tessellation_clockwise = variant->tess_clockwise;
            desc.tessellation_max_factor = variant->tess_max_factor;
        }
        desc.depth_attachment = depth != nullptr;
        desc.depth_format = depth_format;
        desc.alpha_to_coverage = alpha_to_coverage;
        desc.blend_enabled = blend_enabled;
        desc.source_blend = src_blend;
        desc.destination_blend = dst_blend;
        desc.blend_operation = blend_operations[0];
        desc.source_blend_alpha = source_blends_alpha[0];
        desc.destination_blend_alpha = destination_blends_alpha[0];
        desc.blend_operation_alpha = blend_operations_alpha[0];
        desc.independent_blend_enabled = independent_blend;
        desc.blend_enabled_targets = blend_enables;
        desc.source_blends = source_blends;
        desc.destination_blends = destination_blends;
        desc.blend_operations = blend_operations;
        desc.source_blends_alpha = source_blends_alpha;
        desc.destination_blends_alpha = destination_blends_alpha;
        desc.blend_operations_alpha = blend_operations_alpha;
        desc.color_write_mask = write_mask;
        desc.color_write_masks = write_masks;
        desc.rasterization_enabled = !is_capture;
        desc.cache_key = pipeline_key;
        // desc owns copies of everything the compile reads (MetalDevice is
        // thread-safe; its function cache has its own lock), so the lock can
        // go for it. Another thread's create may rehash a map meanwhile:
        // the shader iterators are taken again after.
        std::unique_ptr<graphics::BackendPipeline> created;
        {
            const PureWorkUnlock unlocked(
                *this, guest_draw && draw_compile_may_release_lock(context));
            created = m_backend.create_pipeline(desc);
        }
        vs = m_vertex_shaders.find(state.vertex_shader);
        ps = m_pixel_shaders.find(state.pixel_shader);
        if (!created || !created->valid()) {
            pipeline_diag(
                "Draw skipped: create_pipeline failed attrs=%zu stride=%u",
                attributes_in_use->size(), stride);
            for (const auto& a : *attributes_in_use) {
                pipeline_diag("  attr loc=%u off=%u fmt=%u", a.shader_location,
                              a.offset, static_cast<unsigned>(a.format));
            }
            return;
        }
        pipeline = created.get();
        state.pipelines.emplace(pipeline_key, std::move(created));
    }

    graphics::BackendDrawCall call{};
    // ---- R3c native-state: name prebuilt sampler / depth-stencil states.
    const bool native_states = graphics::delta_record::active(
        graphics::delta_record::Part::NativeState);
    // The states the call names (call.native_states, set by
    // settle_native_states); lives until the draw is recorded.
    graphics::BackendNativeStates native{};
    // ---- end R3c
    // The translated shaders' push-constant block: s# LOD biases, the border
    // colours of s0..s7 for the border twins, then the bounds guard's view
    // sizes -- the layout dxbc_to_spirv declares (kGuardPushBlockBytes).
    struct PushConstants {
        std::array<float, kD3D11SamplerSlotCount> lod_bias{};
        std::array<std::array<float, 4>, 8> border{};
        /// Member 2: bound raw/structured buffer view sizes in words, one per
        /// t# slot then one per u# slot (0 = not a buffer view, no bounds
        /// check). Learn (ByteAddressBuffer / StructuredBuffer): reads past
        /// the view return 0 and writes are dropped; the translator enforces
        /// it from these, so each slot carries its own view's size.
        std::array<u32, graphics::kGuardSizeSrvSlots +
                            graphics::kGuardSizeUavSlots>
            buffer_size_words{};
        /// Member 3: SV_InstanceID bias of a vertex-capture variant.
        u32 instance_id_bias{0};
        /// Member 4: s# with a POINT mip filter (explicit-LOD rounding).
        u32 point_mip_mask{0};
        /// Member 5: the render targets' sample count (`rasterizer`).
        u32 rasterizer_sample_count{0};
        /// Member 6: s# whose bias carries the point-mip tie nudge.
        u32 point_mip_nudge_mask{0};
        /// Members 6-9 (graphics::kFixedFunctionOffset): the vertex stages'
        /// clip-space depth offset, then the pixel shader's SampleMask and
        /// viewport depth range.
        float depth_offset{0.0f};
        u32 sample_mask{0xFFFFFFFFu};
        float depth_min{0.0f};
        float depth_max{1.0f};
        /// Member 10 (graphics::kBorderHighOffset): the border colours of
        /// s8..s15 for the border twins.
        std::array<std::array<float, 4>, 8> border_high{};
        /// Member 12 (graphics::kConstantBufferLimitOffset): the elements
        /// each b# may read (fill_constant_buffer_limits).
        std::array<u32, graphics::kConstantBufferLimitSlots> cb_limits{};
        /// Member 13 (graphics::kMinMaxSamplerOffset): the MINIMUM_ /
        /// MAXIMUM_ samplers' state (fill_minmax_samplers).
        std::array<std::array<u32, 4>, graphics::kMinMaxSamplerSlots>
            minmax_samplers{};
    };
    static_assert(offsetof(PushConstants, buffer_size_words) ==
                  graphics::kGuardSizeTableOffset);
    static_assert(offsetof(PushConstants, instance_id_bias) ==
                  graphics::kInstanceIdBiasOffset);
    static_assert(offsetof(PushConstants, point_mip_mask) ==
                  graphics::kPointMipMaskOffset);
    static_assert(offsetof(PushConstants, rasterizer_sample_count) ==
                  graphics::kRasterizerSampleCountOffset);
    static_assert(offsetof(PushConstants, point_mip_nudge_mask) ==
                  graphics::kPointMipNudgeMaskOffset);
    static_assert(offsetof(PushConstants, depth_offset) ==
                  graphics::kFixedFunctionOffset);
    static_assert(offsetof(PushConstants, border_high) ==
                  graphics::kBorderHighOffset);
    static_assert(offsetof(PushConstants, cb_limits) ==
                  graphics::kConstantBufferLimitOffset);
    static_assert(offsetof(PushConstants, minmax_samplers) ==
                  graphics::kMinMaxSamplerOffset);
    static_assert(sizeof(PushConstants) == graphics::kPushBlockBytes);
    PushConstants fragment_push{};
    PushConstants vertex_push{};
    // D3D11.3 3.5.6.2-3 (D3D11_OPTIONS.MultisampleRTVWithForcedSampleCountOne):
    // ForcedSampleCount 1 on a multisampled target rasterizes the single
    // sample at the pixel centre -- "If a rendered primitive covers any
    // samples in the ForcedSampleCount sample pattern, the pixel is a
    // candidate" -- and a candidate writes every output sample in the
    // SampleMask. The pass puts every Metal sample at the centre; the pixel
    // shader then sees one rasterizer sample (sampleinfo / samplepos on
    // `rasterizer`, SV_Coverage). Measured: d3d11_fl111_probe
    // "forced rt forced=1 samples=4".
    const bool centre_forced_sample =
        !attachmentless && !is_capture && forced_sample_count == 1 &&
        sample_count > 1 && m_backend.supports_programmable_sample_positions();
    // Aliased lines (and WIREFRAME edges) and alpha-antialiased lines are
    // rasterized once per pixel, at its centre, writing every sample of a
    // multisampled target (measured on WARP: the 4x grids of
    // d3d11_view_line_probe [line] "aliased" and "alpha-AA" equal the 1x
    // ones); Metal covers a line per sample. With every sample at the
    // centre each sample sees the one-sample answer. Quadrilateral lines
    // keep the standard pattern: they are ordinary triangles (3.4.5).
    bool per_pixel_lines = false;
    if (!attachmentless && !is_capture && sample_count > 1 &&
        !centre_forced_sample &&
        m_backend.supports_programmable_sample_positions()) {
        if (line_quad_pass) {
            per_pixel_lines = variant->line_coverage_alpha;
        } else if (line_rasterization_mode(state) == 0) {
            bool wireframe = false;
            if (const auto rs =
                    m_rasterizer_states.find(state.rasterizer_state);
                state.rasterizer_state != 0 &&
                rs != m_rasterizer_states.end()) {
                wireframe = rs->second.desc.fill_mode == 2u;
            }
            per_pixel_lines =
                input_class == graphics::BackendPrimitiveClass::Line ||
                (wireframe &&
                 input_class == graphics::BackendPrimitiveClass::Triangle);
        }
    }
    // b# windows (D3D11.3 7.5): the vertex stage is the VS, or the DS whose
    // registers execute_tessellation swapped into the vertex slots.
    const auto push_words = [](PushConstants& push) {
        return std::span<u32>(reinterpret_cast<u32*>(&push),
                              sizeof(PushConstants) / sizeof(u32));
    };
    fill_constant_buffer_limits(push_words(vertex_push),
                                state.vertex_constant_buffers,
                                state.vertex_constant_first,
                                state.vertex_constant_count);
    fill_constant_buffer_limits(push_words(fragment_push),
                                state.pixel_constant_buffers,
                                state.pixel_constant_first,
                                state.pixel_constant_count);
    // sampleinfo / samplepos on `rasterizer` (D3D11.3 22.4.21/22).
    fragment_push.rasterizer_sample_count =
        centre_forced_sample ? 1u : sample_count;
    if (is_capture) vertex_push.instance_id_bias = variant->instance_id_bias;
    // The vertex stage is the VS, or the DS of a tessellated draw (whose
    // bindings execute_tessellation swapped into the vertex slots).
    // The size table's t# entries come from the stage summaries (re-fetched:
    // a nested execute_draw may have rebuilt them since the masks above).
    {
        const auto& ps_sizes =
            srv_stage_summary(0, state.pixel_shader_resources).buffer_size_words;
        std::copy_n(ps_sizes.begin(), graphics::kGuardSizeSrvSlots,
                    fragment_push.buffer_size_words.begin());
        const auto& vs_sizes =
            srv_stage_summary(1, state.vertex_shader_resources).buffer_size_words;
        std::copy_n(vs_sizes.begin(), graphics::kGuardSizeSrvSlots,
                    vertex_push.buffer_size_words.begin());
    }
    for (u32 slot = 0; slot < graphics::kGuardSizeUavSlots &&
                       slot < state.output_merger_unordered_access_views.size();
         ++slot) {
        fragment_push.buffer_size_words[graphics::kGuardSizeSrvSlots + slot] =
            uav_buffer_size_words(
                state.output_merger_unordered_access_views[slot]);
    }
    // The s# part of the push block, from each stage's memoised summary
    // (sampler_stage_summary): border-twin colours, MipLODBias for every
    // sampling instruction of the stage (D3D11.3 7.18.11, 22.4.18/20 -- the
    // translator adds it in the shader, whatever the binding mode, since a
    // Metal sampler has no bias), the point-mip masks and the MINIMUM_ /
    // MAXIMUM_ samplers' state. Fetched here rather than held from above: a
    // nested execute_draw may have rebuilt the memo entry in between.
    const auto apply_samplers = [](PushConstants& push, u32 twins,
                                   const SamplerStageSummary& sum) {
        for (u32 slot = 0; slot < kD3D11SamplerSlotCount; ++slot) {
            if ((twins & (1u << slot)) != 0) {
                const auto& colour = sum.border_colour[slot];
                std::copy(colour.begin(), colour.end(),
                          slot < 8 ? push.border[slot].begin()
                                   : push.border_high[slot - 8].begin());
            }
            push.lod_bias[slot] = sum.lod_bias[slot];
        }
        push.point_mip_mask = sum.point_mip;
        push.minmax_samplers = sum.minmax_entry;
        push.point_mip_nudge_mask = sum.point_mip_nudge;
    };
    apply_samplers(fragment_push, ps_twins,
                   sampler_stage_summary(0, state.pixel_samplers));
    apply_samplers(vertex_push, vs_twins,
                   sampler_stage_summary(1, state.vertex_samplers));
    call.pipeline = pipeline;
    call.blend_color = state.blend_factor;
    call.topology = topology_from_d3d11(state.primitive_topology);
    graphics::BackendBuffer* vb = nullptr;
    if (vb_memo != nullptr) {
        vb = vb_memo->buffers[0];  // R4c vb-memo
    } else if (state.vertex_buffers[0] != 0) {
        auto buf = m_buffers.find(state.vertex_buffers[0]);
        if (buf != m_buffers.end()) vb = buf->second.backend.get();
    }
    // No vertex buffer is a legal draw, not a broken one: Microsoft Learn
    // (SV_VertexID) lets a vertex shader synthesise its vertices from the
    // index alone, with nothing bound to the input assembler. UE5 draws every
    // fullscreen pass that way, so skipping these silently dropped the final
    // composite and left the window black.
    call.vertex_buffer = vb;
    call.vertex_buffer_offset = state.vertex_buffer_offsets[0];
    if (statistics_capture) {
        call.vertex_capture_buffer = statistics.capture.get();
        call.vertex_capture_buffer_offset = 0;
    }
    call.fragment_statistics_buffer = pixel_statistics.get();
    if (is_capture) {
        call.vertex_capture_buffer = variant->buffer;
        call.vertex_capture_buffer_offset = variant->offset;
        // Every vertex must run exactly once, whatever the guest's topology:
        // Metal skips the vertices of an incomplete primitive, and an
        // adjacency topology has no Metal equivalent at all (a 4-vertex
        // LINELIST_ADJ drawn as a triangle list ran three). Nothing is
        // rasterized, so points cost nothing.
        call.topology = graphics::BackendPrimitiveTopology::PointList;
    }
    if (vb != nullptr) {
        call.vertex_buffers[0] = vb;
        call.vertex_buffer_offsets[0] = state.vertex_buffer_offsets[0];
    }
    for (u32 stream = 1; stream < graphics::BACKEND_MAX_VERTEX_BUFFERS;
         ++stream) {
        const u32 slot = stream_slot[stream];
        if (state.vertex_buffers[slot] == 0) continue;
        if (vb_memo != nullptr) {  // R4c vb-memo
            if (vb_memo->buffers[slot] == nullptr) continue;
            call.vertex_buffers[stream] = vb_memo->buffers[slot];
            call.vertex_buffer_offsets[stream] =
                state.vertex_buffer_offsets[slot];
            continue;
        }
        auto buf = m_buffers.find(state.vertex_buffers[slot]);
        if (buf == m_buffers.end() || !buf->second.backend) continue;
        call.vertex_buffers[stream] = buf->second.backend.get();
        call.vertex_buffer_offsets[stream] = state.vertex_buffer_offsets[slot];
    }
    // A declared attribute's unbound stream reads zeros (the pipeline made
    // it a constant stream, see vertex_slot_bound).
    for (const auto& attr : *attributes_in_use) {
        if (attr.buffer_index >= call.vertex_buffers.size() ||
            call.vertex_buffers[attr.buffer_index] != nullptr) {
            continue;
        }
        if (!m_zero_vertex_buffer) {
            // Covers every element: a vertex structure is at most
            // D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES (2048) bytes.
            graphics::BackendBufferDesc zero_desc{};
            zero_desc.byte_width = 4096;
            zero_desc.vertex_buffer = true;
            const std::vector<u8> zeros(zero_desc.byte_width, 0);
            m_zero_vertex_buffer =
                m_backend.create_buffer(zero_desc, zeros.data());
        }
        if (m_zero_vertex_buffer) {
            call.vertex_buffers[attr.buffer_index] = m_zero_vertex_buffer.get();
            call.vertex_buffer_offsets[attr.buffer_index] = 0;
            if (attr.buffer_index == 0) {
                call.vertex_buffer = m_zero_vertex_buffer.get();
                call.vertex_buffer_offset = 0;
            }
        }
    }
    for (const auto& attr : *attributes_in_use) {
        if (attr.buffer_index >= call.vertex_buffers.size() ||
            call.vertex_buffers[attr.buffer_index] == nullptr) {
            pipeline_reject_diag(
                "missing vertex slot=%u layout=0x%llx vs=0x%llx ps=0x%llx "
                "rt=0x%llx state=[0x%llx,0x%llx,0x%llx,0x%llx,"
                "0x%llx,0x%llx,0x%llx,0x%llx]",
                stream_slot[attr.buffer_index],
                static_cast<unsigned long long>(state.input_layout),
                static_cast<unsigned long long>(state.vertex_shader),
                static_cast<unsigned long long>(state.pixel_shader),
                static_cast<unsigned long long>(state.render_targets[0]),
                static_cast<unsigned long long>(state.vertex_buffers[0]),
                static_cast<unsigned long long>(state.vertex_buffers[1]),
                static_cast<unsigned long long>(state.vertex_buffers[2]),
                static_cast<unsigned long long>(state.vertex_buffers[3]),
                static_cast<unsigned long long>(state.vertex_buffers[4]),
                static_cast<unsigned long long>(state.vertex_buffers[5]),
                static_cast<unsigned long long>(state.vertex_buffers[6]),
                static_cast<unsigned long long>(state.vertex_buffers[7]));
            pipeline_diag(
                "Draw skipped: missing vertex buffer for input slot %u",
                stream_slot[attr.buffer_index]);
            return;
        }
    }
    if (indexed) {
        auto ib = m_buffers.find(state.index_buffer);
        if (ib == m_buffers.end() || !ib->second.backend) {
            pipeline_reject_diag(
                "missing index buffer state=0x%llx exists=%u backend=%u",
                static_cast<unsigned long long>(state.index_buffer),
                ib != m_buffers.end() ? 1u : 0u,
                ib != m_buffers.end() && ib->second.backend ? 1u : 0u);
            pipeline_diag("DrawIndexed skipped: no index buffer");
            return;
        }
        call.index_buffer = ib->second.backend.get();
        call.index_32bit = state.index_buffer_format == 42; // R32_UINT
        // A direct draw folds StartIndexLocation into the buffer offset. An
        // indirect one must not: StartIndexLocation is a member of the
        // GPU-written argument block, and the backend passes it through, so
        // folding a CPU-side copy in as well would apply it twice.
        call.index_buffer_offset =
            indirect
                ? state.index_buffer_offset
                : state.index_buffer_offset +
                      start_location * (call.index_32bit ? 4u : 2u);
        call.index_count = count;
        call.base_vertex = base_vertex;
        if (is_capture && variant->capture_indices != nullptr) {
            call.index_buffer = variant->capture_indices;
            call.index_32bit = true;
            call.index_buffer_offset = 0;
            call.index_count = variant->capture_index_count;
        }
    } else {
        call.first_vertex = start_location;
        call.vertex_count = count;
    }
    call.instance_count = instance_count;
    call.first_instance = first_instance;
    if (indirect_buffer != nullptr) {
        call.indirect.buffer = indirect_buffer;
        call.indirect.offset = indirect_offset;
    }

    // A capture pass runs instance k of an instanced draw alone, at the
    // draw's StartInstanceLocation with SV_InstanceID biased by k. Its
    // per-instance elements are StartInstanceLocation + k / step (D3D11.3
    // 8.3.1; a step of 0 never advances), which the slot's offset carries:
    // re-basing the instance instead fetched element + k for every step.
    if (is_capture && variant->instance_id_bias != 0) {
        const u32 k = variant->instance_id_bias;
        for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS; ++slot) {
            if (!per_instance[slot] || constant_slots[slot] ||
                step_rates[slot] == 0 || call.vertex_buffers[slot] == nullptr ||
                call.vertex_buffers[slot] == m_zero_vertex_buffer.get()) {
                continue;
            }
            const u64 shift =
                static_cast<u64>(strides[slot]) * (k / step_rates[slot]);
            call.vertex_buffer_offsets[slot] += static_cast<unsigned>(shift);
            if (slot == 0) call.vertex_buffer_offset += static_cast<unsigned>(shift);
        }
    }

    // Input assembly Metal's own cannot express, for a direct draw whose
    // topology the vertex stage rasterizes (a geometry or tessellation
    // emulation assembles its own primitives from the capture):
    //  - an adjacency topology with no geometry shader draws only the
    //    primitive's own vertices; the adjacent ones exist for a GS (D3D11.3
    //    8.15; measured d3d11_a07_ia_probe `E *_ADJ no GS`);
    //  - the cut value inside a LINELIST / TRIANGLELIST discards the partial
    //    primitive before it and restarts assembly after it (8.12, 8.13;
    //    measured `E idx16 TRIANGLELIST {0,1,2,FFFF,4,5,6}`), where Metal
    //    keeps index positions and drops the primitive holding the cut;
    //  - a vertex fetched past the end of its buffer reads 0 (8.19.2).
    // Both index rewrites become a host 32-bit index stream. A non-indexed
    // draw's stream counts from 0 with StartVertexLocation as the base
    // vertex, so fetch and SV_VertexID are unchanged.
    const bool ia_direct = !is_capture && !is_geometry_output &&
                           !is_tessellated && !indirect;
    const u32 ia_topology = state.primitive_topology;
    const bool ia_adjacency = ia_topology >= 10 && ia_topology <= 13;
    Buffer* ia_index_buffer = nullptr;
    if (indexed && !indirect) {
        const auto found = m_buffers.find(state.index_buffer);
        if (found != m_buffers.end()) ia_index_buffer = &found->second;
    }
    const bool ia_wide = state.index_buffer_format == 42;  // R32_UINT
    const u32 ia_cut = ia_wide ? 0xFFFFFFFFu : 0xFFFFu;
    const u32 ia_first_index =
        state.index_buffer_offset / (ia_wide ? 4u : 2u) + start_location;
    // The index range's CPU copy, when it is true to the GPU's: not while a
    // GPU write is newer, and never for a buffer the GPU can write itself.
    const auto ia_indices_readable = [&]() {
        // D3D11_BIND_STREAM_OUTPUT (0x10) | D3D11_BIND_UNORDERED_ACCESS (0x80).
        constexpr u32 kGpuWritableBinds = 0x10u | 0x80u;
        return ia_index_buffer != nullptr && !ia_index_buffer->gpu_newer &&
               (ia_index_buffer->desc.bind_flags & kGpuWritableBinds) == 0 &&
               (static_cast<u64>(ia_first_index) + count) *
                       (ia_wide ? 4u : 2u) <=
                   ia_index_buffer->shadow.size();
    };
    const auto ia_index_at = [&](u32 i) {
        const u8* at = ia_index_buffer->shadow.data() +
                       static_cast<usize>(ia_first_index + i) *
                           (ia_wide ? 4u : 2u);
        u32 value = 0;
        std::memcpy(&value, at, ia_wide ? 4u : 2u);
        return value;
    };
    // Largest index (cut excluded) and whether the cut occurs, cached per
    // range until the shadow changes.
    const auto ia_scan = [&]() -> const Buffer::IndexScan* {
        if (!ia_indices_readable()) return nullptr;
        auto& scans = ia_index_buffer->index_scans;
        for (const auto& scan : scans) {
            if (scan.version == ia_index_buffer->shadow_version &&
                scan.first == ia_first_index && scan.count == count &&
                scan.wide == ia_wide) {
                return &scan;
            }
        }
        std::erase_if(scans, [&](const Buffer::IndexScan& scan) {
            return scan.version != ia_index_buffer->shadow_version;
        });
        if (scans.size() >= 16) scans.erase(scans.begin());
        Buffer::IndexScan scan{};
        scan.version = ia_index_buffer->shadow_version;
        scan.first = ia_first_index;
        scan.count = count;
        scan.wide = ia_wide;
        for (u32 i = 0; i < count; ++i) {
            const u32 value = ia_index_at(i);
            if (value == ia_cut) {
                scan.has_cut = true;
            } else {
                scan.max_index = std::max(scan.max_index, value);
            }
        }
        scans.push_back(scan);
        return &scans.back();
    };
    if (ia_adjacency && indirect && !is_capture &&
        !is_geometry_output && !is_tessellated) {
        // The GPU writes the counts, so the host cannot pick the
        // primitive's own vertices out of the adjacency stream.
        static bool reported = false;
        if (!reported) {
            reported = true;
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED d3d11!Draw*Indirect: "
                         "adjacency topology without a geometry shader\n");
        }
        pipeline_diag("Draw skipped: indirect adjacency draw without a "
                      "geometry shader");
        return;
    }
    const Buffer::IndexScan* ia_scanned = nullptr;
    std::vector<u32> ia_rewritten;
    bool ia_rewrite = false;
    graphics::BackendPrimitiveTopology ia_rewritten_topology = call.topology;
    if (ia_direct && (ia_adjacency ||
                      (indexed && (ia_topology == 2 || ia_topology == 4)))) {
        if (indexed) ia_scanned = ia_scan();
        const bool need = ia_adjacency ||
                          (ia_scanned != nullptr && ia_scanned->has_cut);
        if (need && indexed && ia_scanned == nullptr) {
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d11!DrawIndexed: "
                             "adjacency or list-cut assembly from a "
                             "GPU-written index buffer\n");
            }
            if (ia_adjacency) {
                pipeline_diag("Draw skipped: adjacency topology without a "
                              "geometry shader needs a CPU-readable index "
                              "buffer");
                return;
            }
        } else if (need) {
            const auto source = [&](u32 i) {
                return indexed ? ia_index_at(i) : i;
            };
            const u32 vpp = ia_topology == 2 || ia_topology == 10 ? 2u
                          : ia_topology == 4 || ia_topology == 12 ? 3u
                          : ia_topology == 11 ? 2u : 3u;
            ia_rewritten_topology =
                vpp == 2 ? graphics::BackendPrimitiveTopology::LineList
                         : graphics::BackendPrimitiveTopology::TriangleList;
            ia_rewritten.reserve(count);
            if (ia_topology == 2 || ia_topology == 4 || ia_topology == 10 ||
                ia_topology == 12) {
                // Lists: collect a primitive's vertices; a cut discards the
                // partial one. With adjacency the list is 4 (lines) or 6
                // (triangles) a primitive and its vertices 1,2 or 0,2,4 are
                // the primitive itself.
                const u32 primitive_width = ia_topology == 10 ? 4u
                                          : ia_topology == 12 ? 6u : vpp;
                std::array<u32, 6> held{};
                u32 filled = 0;
                for (u32 i = 0; i < count; ++i) {
                    const u32 value = source(i);
                    if (indexed && value == ia_cut) {
                        filled = 0;
                        continue;
                    }
                    held[filled++] = value;
                    if (filled < primitive_width) continue;
                    filled = 0;
                    if (ia_topology == 10) {
                        ia_rewritten.insert(ia_rewritten.end(),
                                            {held[1], held[2]});
                    } else if (ia_topology == 12) {
                        ia_rewritten.insert(ia_rewritten.end(),
                                            {held[0], held[2], held[4]});
                    } else {
                        ia_rewritten.insert(ia_rewritten.end(), held.begin(),
                                            held.begin() + vpp);
                    }
                }
            } else {
                // Strips with adjacency, cut into segments by the cut value.
                // LINESTRIP_ADJ primitive j is vertices j..j+3 and draws
                // j+1..j+2; TRIANGLESTRIP_ADJ takes its six from the
                // measured table (graphics::triangle_strip_adjacency_vertex)
                // and draws inputs 0, 2 and 4, already in drawing order.
                u32 begin = 0;
                while (begin < count) {
                    u32 end = begin;
                    while (end < count && !(indexed && source(end) == ia_cut)) {
                        ++end;
                    }
                    const u32 length = end - begin;
                    if (ia_topology == 11) {
                        for (u32 j = 0; j + 4 <= length; ++j) {
                            ia_rewritten.insert(
                                ia_rewritten.end(),
                                {source(begin + j + 1), source(begin + j + 2)});
                        }
                    } else {
                        const u32 triangles =
                            graphics::triangle_strip_adjacency_count(length);
                        for (u32 t = 0; t < triangles; ++t) {
                            for (const u32 k : {0u, 2u, 4u}) {
                                ia_rewritten.push_back(source(
                                    begin +
                                    graphics::triangle_strip_adjacency_vertex(
                                        t, k, triangles)));
                            }
                        }
                    }
                    begin = end + 1;
                }
            }
            ia_rewrite = true;
        }
    }
    std::unique_ptr<graphics::BackendBuffer> ia_index_stream;
    if (ia_rewrite) {
        if (ia_rewritten.empty()) {
            pipeline_diag("Draw skipped: no complete primitive");
            return;
        }
        graphics::BackendBufferDesc stream_desc{};
        stream_desc.byte_width = ia_rewritten.size() * sizeof(u32);
        stream_desc.index_buffer = true;
        ia_index_stream =
            m_backend.create_buffer(stream_desc, ia_rewritten.data());
        if (!ia_index_stream) {
            pipeline_diag("Draw skipped: index stream allocation failed");
            return;
        }
        call.index_buffer = ia_index_stream.get();
        call.index_32bit = true;
        call.index_buffer_offset = 0;
        call.index_count = static_cast<u32>(ia_rewritten.size());
        call.base_vertex = indexed ? base_vertex
                                   : static_cast<i32>(start_location);
        call.first_vertex = 0;
        call.vertex_count = 0;
        call.topology = ia_rewritten_topology;
    }

    // D3D11.3 8.19.2: a fetch past the end of a vertex buffer returns 0 in
    // the format's components (and the missing-component defaults). Metal
    // reads whatever lies beyond, so a slot the draw's vertex or instance
    // range overruns is served from a zero-padded copy of the bound range
    // (measured: d3d11_a07_ia_probe `A draw POINTLIST n=6 start=61`, `A idx32
    // POINTLIST indices past vb end`). The range is CPU-known for a direct
    // draw; an indexed one takes it from the index buffer's CPU copy.
    std::vector<std::unique_ptr<graphics::BackendBuffer>> ia_padded_streams;
    if (!is_geometry_output && !is_tessellated && !indirect) {
        std::array<u64, graphics::BACKEND_MAX_VERTEX_BUFFERS> attr_end{};
        for (const auto& attr : *attributes_in_use) {
            if (attr.buffer_index >= attr_end.size()) continue;
            attr_end[attr.buffer_index] = std::max<u64>(
                attr_end[attr.buffer_index],
                static_cast<u64>(attr.offset) +
                    d3d11::vertex_format_stride(attr.format));
        }
        // Bytes a slot's fetches reach for element @p last; 0 = none.
        const auto reach = [&](u32 slot, i64 last) -> u64 {
            if (last < 0) return 0;
            const u64 stride_bytes =
                constant_slots[slot] ? 0u : strides[slot];
            return stride_bytes * static_cast<u64>(last) + attr_end[slot];
        };
        const auto available = [&](u32 slot) -> u64 {
            const auto guest = m_buffers.find(state.vertex_buffers[slot]);
            if (guest == m_buffers.end()) return ~0ull;
            const u64 width = guest->second.desc.byte_width;
            const u64 offset = call.vertex_buffer_offsets[slot];
            return width > offset ? width - offset : 0u;
        };
        // Largest vertex id the draw fetches; negative = none known. An
        // indexed draw's comes from its index data -- scanned only when the
        // largest index the format can hold would overrun a per-vertex slot.
        i64 max_vertex = -1;
        if (ia_rewrite) {
            max_vertex = static_cast<i64>(*std::max_element(
                             ia_rewritten.begin(), ia_rewritten.end())) +
                         call.base_vertex;
        } else if (!indexed) {
            max_vertex = static_cast<i64>(start_location) + count - 1;
        } else {
            const i64 worst =
                static_cast<i64>(ia_wide ? 0xFFFFFFFEu : 0xFFFEu) + base_vertex;
            bool may_overrun = false;
            for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS;
                 ++slot) {
                if (attr_end[slot] == 0 || per_instance[slot] ||
                    constant_slots[slot] ||
                    call.vertex_buffers[slot] == nullptr ||
                    call.vertex_buffers[slot] == m_zero_vertex_buffer.get() ||
                    slot >= state.vertex_buffers.size()) {
                    continue;
                }
                may_overrun |= reach(slot, worst) > available(slot);
            }
            if (may_overrun) {
                if (const auto* scan =
                        ia_scanned != nullptr ? ia_scanned : ia_scan()) {
                    max_vertex = static_cast<i64>(scan->max_index) + base_vertex;
                } else if (!indirect) {
                    static bool reported = false;
                    if (!reported) {
                        reported = true;
                        std::fprintf(stderr,
                                     "[vitreo] UNIMPLEMENTED d3d11!DrawIndexed: "
                                     "out-of-bounds vertex fetch through a "
                                     "GPU-written index buffer is not "
                                     "zeroed\n");
                    }
                }
            }
        }
        for (u32 slot = 0; slot < graphics::BACKEND_MAX_VERTEX_BUFFERS;
             ++slot) {
            if (attr_end[slot] == 0 || call.vertex_buffers[slot] == nullptr ||
                call.vertex_buffers[slot] == m_zero_vertex_buffer.get() ||
                slot >= state.vertex_buffers.size()) {
                continue;
            }
            const auto guest = m_buffers.find(state.vertex_buffers[slot]);
            if (guest == m_buffers.end()) continue;
            i64 last = -1;
            if (constant_slots[slot]) {
                last = 0;
            } else if (per_instance[slot]) {
                const u32 instances = std::max(1u, instance_count);
                last = static_cast<i64>(first_instance) +
                       (step_rates[slot] == 0
                            ? 0
                            : (instances - 1) / step_rates[slot]);
            } else {
                last = max_vertex;
            }
            if (last < 0) continue;
            const u64 needed = reach(slot, last);
            const u64 offset = call.vertex_buffer_offsets[slot];
            const u64 bound = available(slot);
            if (needed <= bound) continue;
            constexpr u64 kMaxPaddedStream = 256ull << 20;
            if (needed > kMaxPaddedStream) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED d3d11!Draw: "
                                 "out-of-bounds vertex fetch more than 256 MB "
                                 "past a buffer's end is not zeroed\n");
                }
                continue;
            }
            graphics::BackendBufferDesc padded_desc{};
            padded_desc.byte_width = static_cast<usize>((needed + 15u) & ~15ull);
            padded_desc.vertex_buffer = true;
            const std::vector<u8> zeros(padded_desc.byte_width, 0);
            auto padded = m_backend.create_buffer(padded_desc, zeros.data());
            if (!padded) continue;
            if (bound != 0 &&
                !m_backend.copy_buffer_region(padded.get(), 0,
                                              call.vertex_buffers[slot], offset,
                                              bound)) {
                continue;
            }
            note_gpu_use(guest->second);
            call.vertex_buffers[slot] = padded.get();
            call.vertex_buffer_offsets[slot] = 0;
            if (slot == 0) {
                call.vertex_buffer = padded.get();
                call.vertex_buffer_offset = 0;
            }
            ia_padded_streams.push_back(std::move(padded));
        }
    }

    // Which binding mode each stage is in was decided when its shader was
    // compiled, from the registers the shader declares. The two modes want the
    // same Metal buffer indices, so a stage is in exactly one of them -- but the
    // stages are independent, and a flat VS paired with a table-reading PS is
    // both legal and normal, because Metal's index namespace is per stage.
    d3d11::ShaderBindingLayout vs_layout{};
    d3d11::ShaderBindingLayout ps_layout{};
    if (const auto vs = m_vertex_shaders.find(state.vertex_shader);
        vs != m_vertex_shaders.end()) {
        vs_layout = vs->second.binding_layout;
    }
    if (const auto ps = m_pixel_shaders.find(state.pixel_shader);
        ps != m_pixel_shaders.end()) {
        ps_layout = ps->second.binding_layout;
    }
    if (!sync_descriptor_tables(state, d3d11::StageBindings::Vertex, vs_layout,
                                call.vertex_descriptor_tables, vs_twins)) {
        pipeline_diag("Draw skipped: vertex descriptor tables unavailable");
        return;
    }
    if (!sync_descriptor_tables(state, d3d11::StageBindings::Pixel, ps_layout,
                                call.fragment_descriptor_tables, ps_twins)) {
        pipeline_diag("Draw skipped: pixel descriptor tables unavailable");
        return;
    }

    for (u32 i = 0; i < graphics::BACKEND_MAX_CONSTANT_BUFFER_SLOTS; ++i) {
        if (!vs_layout.uses_descriptor_tables &&
            state.vertex_constant_buffers[i] != 0) {
            auto buf = m_buffers.find(state.vertex_constant_buffers[i]);
            if (buf != m_buffers.end()) {
                call.vertex_constant_buffers[i] = buf->second.backend.get();
                // Learn: *SetConstantBuffers1 measures the window in constants
                // of 16 bytes. Zero is the whole buffer, which is what the
                // D3D11.0 entry point always binds.
                call.vertex_constant_buffer_offsets[i] =
                    constant_buffer_bind_offset(state.vertex_constant_buffers[i],
                                                state.vertex_constant_first[i]);
            }
        }
        if (!ps_layout.uses_descriptor_tables &&
            state.pixel_constant_buffers[i] != 0) {
            auto buf = m_buffers.find(state.pixel_constant_buffers[i]);
            if (buf != m_buffers.end()) {
                call.fragment_constant_buffers[i] = buf->second.backend.get();
                call.fragment_constant_buffer_offsets[i] =
                    constant_buffer_bind_offset(state.pixel_constant_buffers[i],
                                                state.pixel_constant_first[i]);
            }
        }
    }

    const auto bind_shader_resource =
        [this](GuestAddr srv_address,
               const graphics::BackendTexture2D*& texture_2d,
               graphics::BackendTextureViewDesc& backend_view,
               const graphics::BackendTexture3D*& texture_3d) {
        resolve_shader_resource(srv_address, texture_2d, backend_view,
                                texture_3d);
    };
    const auto bind_buffer_resource =
        [this](GuestAddr srv_address, const graphics::BackendBuffer*& out_buffer,
               unsigned& out_offset,
               const graphics::BackendTextureBuffer*& out_texture_buffer) {
        resolve_buffer_resource(srv_address, out_buffer, out_offset,
                                out_texture_buffer);
    };
    // Both SRV resolvers leave their outputs untouched for an empty slot, so
    // an empty slot skips them; the sampler resolves regardless (a null
    // sampler slot still binds the default sampler).
    // ---- R4 sampler-memo: each stage's s# as resolve_sampler leaves them,
    // keyed by the sampler array's stamp.
    const bool sampler_memo =
        memo_on(d3d11::draw_memo::Part::SamplerMemo);
    // srv-memo: each stage's flat t# keyed by the SRV array's stamp.
    const bool srv_memo =
        memo_on(d3d11::draw_memo::Part::SrvMemo);
    if (srv_memo) {
        const auto apply = [](const ResolvedSrvs& r, auto& textures,
                              auto& views, auto& textures_3d, auto& buffers,
                              auto& offsets, auto& texture_buffers) {
            for (u32 i = 0; i < ResolvedSrvs::N; ++i) {
                textures[i] = r.textures[i];
                views[i] = r.views[i];
                textures_3d[i] = r.textures_3d[i];
                buffers[i] = r.buffers[i];
                offsets[i] = r.offsets[i];
                texture_buffers[i] = r.texture_buffers[i];
            }
        };
        if (!ps_layout.uses_descriptor_tables) {
            apply(resolved_srvs(0, state.pixel_shader_resources),
                  call.textures, call.texture_views, call.textures_3d,
                  call.fragment_srv_buffers, call.fragment_srv_buffer_offsets,
                  call.fragment_srv_texture_buffers);
        }
        if (!vs_layout.uses_descriptor_tables) {
            apply(resolved_srvs(1, state.vertex_shader_resources),
                  call.vertex_textures, call.vertex_texture_views,
                  call.vertex_textures_3d, call.vertex_srv_buffers,
                  call.vertex_srv_buffer_offsets,
                  call.vertex_srv_texture_buffers);
        }
    }
    if (sampler_memo) {
        if (!ps_layout.uses_descriptor_tables) {
            const ResolvedSamplers& r =
                resolved_samplers(0, state.pixel_samplers, native_states);
            std::copy(r.descs.begin(), r.descs.end(), call.samplers.begin());
            if (native_states) native.samplers = r.natives;
        }
        if (!vs_layout.uses_descriptor_tables) {
            const ResolvedSamplers& r =
                resolved_samplers(1, state.vertex_samplers, native_states);
            std::copy(r.descs.begin(), r.descs.end(),
                      call.vertex_samplers.begin());
            if (native_states) native.vertex_samplers = r.natives;
        }
    }
    // ---- end R4
    for (u32 i = 0; i < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++i) {
        if (!ps_layout.uses_descriptor_tables) {
            if (!srv_memo && state.pixel_shader_resources[i] != 0) {
                bind_shader_resource(
                    state.pixel_shader_resources[i], call.textures[i],
                    call.texture_views[i], call.textures_3d[i]);
                bind_buffer_resource(state.pixel_shader_resources[i],
                                     call.fragment_srv_buffers[i],
                                     call.fragment_srv_buffer_offsets[i],
                                     call.fragment_srv_texture_buffers[i]);
            }
            if (!sampler_memo) {
                (void)resolve_sampler(state.pixel_samplers[i],
                                      call.samplers[i], nullptr,
                                      native_states ? &native.samplers[i]
                                                    : nullptr);  // R3c
            }
        }
        if (!vs_layout.uses_descriptor_tables) {
            if (!srv_memo && state.vertex_shader_resources[i] != 0) {
                bind_shader_resource(
                    state.vertex_shader_resources[i], call.vertex_textures[i],
                    call.vertex_texture_views[i], call.vertex_textures_3d[i]);
                bind_buffer_resource(state.vertex_shader_resources[i],
                                     call.vertex_srv_buffers[i],
                                     call.vertex_srv_buffer_offsets[i],
                                     call.vertex_srv_texture_buffers[i]);
            }
            if (!sampler_memo) {
                (void)resolve_sampler(state.vertex_samplers[i],
                                      call.vertex_samplers[i], nullptr,
                                      native_states
                                          ? &native.vertex_samplers[i]
                                          : nullptr);  // R3c
            }
        }
    }
    for (u32 i = 0; i < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++i) {
        if (!ps_layout.uses_descriptor_tables && call.textures[i] != nullptr &&
            !d3d11::fit_view_to_declaration(ps_layout, false, i,
                                            call.texture_views[i])) {
            call.textures[i] = nullptr;
        }
        if (!vs_layout.uses_descriptor_tables &&
            call.vertex_textures[i] != nullptr &&
            !d3d11::fit_view_to_declaration(vs_layout, false, i,
                                            call.vertex_texture_views[i])) {
            call.vertex_textures[i] = nullptr;
        }
    }
    // Fragment-stage u# registers. Learn
    // (OMSetRenderTargetsAndUnorderedAccessViews): for the graphics pipeline the
    // UAVs share the u# space with the render targets, and UAVStartSlot is the
    // absolute index -- which is how the context stored them, so slot N here is
    // register uN.
    //
    // Unlike b#/t#/s# these stay on the flat path in both binding modes. D3D11.0
    // caps u# at 8 (D3D11_PS_CS_UAV_REGISTER_COUNT), which the flat windows
    // already reach, so a table would buy nothing; and set 3 is deliberately
    // left discrete for a pixel shader so the two coexist.
    for (u32 slot = 0; slot < graphics::BACKEND_MAX_TEXTURE_SLOTS &&
                       slot < state.output_merger_unordered_access_views.size();
         ++slot) {
        const auto view = m_unordered_access_views.find(
            state.output_merger_unordered_access_views[slot]);
        if (view == m_unordered_access_views.end()) continue;
        if (view->second.texture_buffer != nullptr) {
            // RWBuffer<T>: the pixel shader declares a read_write
            // texture_buffer at BACKEND_UAV_TEXTURE_BASE + slot.
            call.fragment_uav_texture_buffers[slot] =
                view->second.texture_buffer.get();
            continue;
        }
        if (view->second.buffer != 0) {
            const auto buffer = m_buffers.find(view->second.buffer);
            if (buffer != m_buffers.end() && buffer->second.backend) {
                call.fragment_uav_buffers[slot] = buffer->second.backend.get();
                call.fragment_uav_buffer_offsets[slot] =
                    view->second.first_element * view->second.element_bytes;
            }
            continue;
        }
        const auto texture = m_textures.find(view->second.texture);
        if (texture == m_textures.end()) continue;
        if (texture->second.volume) {
            // One mip under the view's format (volume_uav_binding).
            graphics::BackendTexture3D* volume =
                volume_uav_binding(view->second, texture->second, slot);
            if (volume == nullptr) continue;
            ++texture->second.diag_uav_binds;
            texture->second.diag_uav_writer = state.pixel_shader;
            texture->second.diag_uav_writer_compute = false;
            call.fragment_uav_textures_3d[slot] = volume;
            continue;
        }
        graphics::BackendTextureViewDesc uav_view = uav_texture_view(
            view->second.view_dimension, view->second.mip_slice,
            view->second.first_array_slice, view->second.array_size,
            backend_format(view->second.format),
            texture->second.backend->desc().pixel_format);
        if (!d3d11::fit_view_to_declaration(ps_layout, true, slot, uav_view)) {
            continue;
        }
        ++texture->second.diag_uav_binds;
        texture->second.diag_uav_writer = state.pixel_shader;
        texture->second.diag_uav_writer_compute = false;
        call.fragment_uav_textures[slot] = texture->second.backend.get();
        call.fragment_uav_texture_views[slot] = uav_view;
    }
    // A capture draw runs no pixel shader, so it moves no counter.
    if (!is_capture &&
        !bind_uav_counters(state.output_merger_unordered_access_views,
                           ps_layout, call.fragment_uav_counters)) {
        pipeline_diag("Draw skipped: a UAV counter is unbound");
        return;
    }

    call.fragment_constants = &fragment_push;
    call.fragment_constants_size = sizeof(fragment_push);
    // The flat model binds s0..s7's twins at Metal samplers 8..15; a
    // descriptor-table shader finds its twins in its sampler table.
    call.fragment_border_twin_mask =
        pixel_shader_disabled || ps->second.binding_layout.uses_descriptor_tables
            ? 0u : (ps_twins & 0xFFu);
    // Every draw: a guarded vertex shader reads its t# view sizes from this
    // block whether or not it samples through border twins. Leaving it
    // unbound handed GPU-scene loads whatever buffer(0) last held.
    call.vertex_constants = &vertex_push;
    call.vertex_constants_size = sizeof(vertex_push);
    call.vertex_border_twin_mask =
        vs->second.binding_layout.uses_descriptor_tables ? 0u
                                                         : (vs_twins & 0xFFu);

    call.viewport = state.viewports[0];
    call.scissor_enabled = false;
    // D3D11's implicit rasterizer state is solid fill with clockwise fronts
    // and back-face culling when no object is bound.
    call.cull_mode = graphics::BackendCullMode::Back;
    const auto& scissor = state.scissor_rects[0];
    call.scissor = {scissor[0], scissor[1], scissor[2], scissor[3]};
    if (!is_capture && (is_geometry_output ? variant->viewport_array
                                           : vs->second.writes_viewport_index)) {
        // The shader picks the viewport; indices past the bound count are
        // disabled (Learn RSSetViewports), which the backend enforces. The
        // scissor array is read only when scissoring is on, and D3D pairs
        // rect i with viewport i.
        call.viewports = state.viewports.data();
        call.scissors = state.scissor_rects.data();
        call.viewport_count = state.viewport_count;
    }

    // Opt-in capture of the frontend-visible draw inputs. This uses the
    // shadow bytes maintained by the D3D11 resource contract, so diagnosing
    // malformed geometry never requires synchronously reading normal GPU
    // buffers or changing submission behavior.
    // Per draw: read the process environment once, not per call.
    static const char* const draw_dump_prefix =
        std::getenv("VITREO_D3D11_DUMP_DRAW");
    if (draw_dump_prefix != nullptr && draw_dump_prefix[0] != '\0') {
        // Optional trigger file keeps the finite capture budget available for
        // a scene reached through guest input instead of consuming it during
        // startup. Absence of the variable preserves the original behavior.
        const char* trigger =
            std::getenv("VITREO_D3D11_DUMP_DRAW_TRIGGER");
        bool trigger_ready = trigger == nullptr || trigger[0] == '\0';
        if (!trigger_ready) {
            if (FILE* file = std::fopen(trigger, "rb")) {
                std::fclose(file);
                trigger_ready = true;
            }
        }
        // Optional: only capture draws whose viewport width matches
        // VITREO_D3D11_DUMP_DRAW_VP_W (viewport width in pixels).
        const char* vp_w_text = std::getenv("VITREO_D3D11_DUMP_DRAW_VP_W");
        const bool vp_match =
            vp_w_text == nullptr || vp_w_text[0] == '\0' ||
            static_cast<int>(state.viewports[0][2] + 0.5f) ==
                static_cast<int>(std::strtol(vp_w_text, nullptr, 0));
        const char* min_base_text =
            std::getenv("VITREO_D3D11_DUMP_DRAW_MIN_BASE");
        const bool base_match =
            min_base_text == nullptr || min_base_text[0] == '\0' ||
            base_vertex >= static_cast<int>(
                std::strtol(min_base_text, nullptr, 0));
        const char* rt_text =
            std::getenv("VITREO_D3D11_DUMP_DRAW_RT");
        const bool rt_match =
            rt_text == nullptr || rt_text[0] == '\0' ||
            state.render_targets[0] == static_cast<GuestAddr>(
                std::strtoull(rt_text, nullptr, 0));
        // VITREO_D3D11_DUMP_DRAW_NO_PS=1 keeps only depth-only draws (a
        // prepass); VITREO_D3D11_DUMP_DRAW_WITH_PS=1 only shaded ones.
        const bool ps_match =
            (std::getenv("VITREO_D3D11_DUMP_DRAW_NO_PS") == nullptr ||
             state.pixel_shader == 0) &&
            (std::getenv("VITREO_D3D11_DUMP_DRAW_WITH_PS") == nullptr ||
             state.pixel_shader != 0) &&
            // VITREO_D3D11_DUMP_DRAW_WITH_RT=1 / _WITH_DSV=1: only draws with
            // a colour target / a depth-stencil view bound (a base pass, not
            // UI, a prepass or an occlusion box).
            (std::getenv("VITREO_D3D11_DUMP_DRAW_WITH_RT") == nullptr ||
             state.render_targets[0] != 0) &&
            (std::getenv("VITREO_D3D11_DUMP_DRAW_WITH_DSV") == nullptr ||
             state.depth_stencil_view != 0) &&
            // VITREO_D3D11_DUMP_DRAW_WITH_STENCIL=1: only draws whose
            // depth-stencil state enables the stencil test (UE's base pass
            // writes lighting-channel stencil; its prepass does not).
            (std::getenv("VITREO_D3D11_DUMP_DRAW_WITH_STENCIL") == nullptr ||
             [&] {
                 const auto dss =
                     m_depth_stencil_states.find(state.depth_stencil_state);
                 return dss != m_depth_stencil_states.end() &&
                        dss->second.desc.stencil_enable != 0;
             }());
        const char* max_text =
            std::getenv("VITREO_D3D11_DUMP_DRAW_MAX");
        const int draw_dump_max =
            max_text == nullptr || max_text[0] == '\0'
                ? 16
                : std::max(0, static_cast<int>(
                                  std::strtol(max_text, nullptr, 0)));
        const bool metadata_only =
            std::getenv("VITREO_D3D11_DUMP_DRAW_META_ONLY") != nullptr;
        static int draw_dump_count = 0;
        if (trigger_ready && vp_match && base_match && rt_match && ps_match &&
            draw_dump_count < draw_dump_max) {
            const int draw_number = ++draw_dump_count;
            char path[768];
            const auto dump_buffer = [&](const char* kind, unsigned slot,
                                         GuestAddr address) {
                if (address == 0) return;
                auto buffer = m_buffers.find(address);
                if (buffer == m_buffers.end()) return;
                std::snprintf(path, sizeof(path), "%s-draw%d-%s%u.bin",
                              draw_dump_prefix, draw_number, kind, slot);
                if (FILE* file = std::fopen(path, "wb")) {
                    std::fwrite(buffer->second.shadow.data(), 1,
                                buffer->second.shadow.size(), file);
                    std::fclose(file);
                }
            };
            if (!metadata_only) {
                for (unsigned slot = 0; slot < state.vertex_buffers.size();
                     ++slot) {
                    dump_buffer("vb", slot, state.vertex_buffers[slot]);
                }
                for (unsigned slot = 0;
                     slot < state.vertex_constant_buffers.size(); ++slot) {
                    dump_buffer("vcb", slot,
                                state.vertex_constant_buffers[slot]);
                }
                for (unsigned slot = 0;
                     slot < state.pixel_constant_buffers.size(); ++slot) {
                    dump_buffer("pcb", slot,
                                state.pixel_constant_buffers[slot]);
                }
                if (indexed) dump_buffer("ib", 0, state.index_buffer);
            }

            // The shaders themselves, so a captured draw can be replayed or
            // disassembled without a second run: vs/ps DXBC by draw number.
            const auto dump_bytecode = [&](const char* kind,
                                           const std::vector<u8>& bytes) {
                if (bytes.empty()) return;
                std::snprintf(path, sizeof(path), "%s-draw%d-%s.dxbc",
                              draw_dump_prefix, draw_number, kind);
                if (FILE* file = std::fopen(path, "wb")) {
                    std::fwrite(bytes.data(), 1, bytes.size(), file);
                    std::fclose(file);
                }
            };
            if (const auto dvs = m_vertex_shaders.find(state.vertex_shader);
                dvs != m_vertex_shaders.end()) {
                dump_bytecode("vs", dvs->second.bytecode);
            }
            if (const auto dps = m_pixel_shaders.find(state.pixel_shader);
                dps != m_pixel_shaders.end()) {
                dump_bytecode("ps", dps->second.bytecode);
            }
            std::snprintf(path, sizeof(path), "%s-draw%d.txt",
                          draw_dump_prefix, draw_number);
            if (FILE* file = std::fopen(path, "wb")) {
                std::fprintf(file,
                             "indexed=%u count=%u start=%u base=%d "
                             "instances=%u first_instance=%u "
                             "topology=%u\n",
                             indexed ? 1u : 0u, count, start_location,
                             base_vertex, instance_count, first_instance,
                             state.primitive_topology);
                // Indirect draws: the counts above are the CPU-side zeros; the
                // real ones are in the args buffer, written by GPU culling.
                // VITREO_D3D11_DUMP_DRAW_INDIRECT=1 reads them back (waits).
                if (indirect_args != 0) {
                    std::fprintf(file, "indirect_args=0x%llx offset=%u",
                                 static_cast<unsigned long long>(indirect_args),
                                 indirect_offset);
                    const auto args = m_buffers.find(indirect_args);
                    if (std::getenv("VITREO_D3D11_DUMP_DRAW_INDIRECT") &&
                        args != m_buffers.end() && args->second.backend) {
                        std::array<u32, 5> words{};
                        m_backend.wait_idle();
                        if (args->second.backend->read(
                                indirect_offset, words.data(),
                                (indexed ? 5u : 4u) * sizeof(u32))) {
                            std::fprintf(file, " gpu_args=%u,%u,%u,%u,%u",
                                         words[0], words[1], words[2],
                                         words[3], words[4]);
                        }
                    }
                    std::fprintf(file, "\n");
                }
                const auto dump_dsv =
                    m_depth_stencil_views.find(state.depth_stencil_view);
                const auto dump_rtv =
                    m_render_target_views.find(state.render_targets[0]);
                std::fprintf(
                    file,
                    "vs=0x%llx ps=0x%llx layout=0x%llx rt0=0x%llx "
                    "dsv=0x%llx rt0_tex=0x%llx dsv_tex=0x%llx dsv_flags=%u "
                    "dsv_mip=%u\n",
                    static_cast<unsigned long long>(state.vertex_shader),
                    static_cast<unsigned long long>(state.pixel_shader),
                    static_cast<unsigned long long>(state.input_layout),
                    static_cast<unsigned long long>(state.render_targets[0]),
                    static_cast<unsigned long long>(state.depth_stencil_view),
                    static_cast<unsigned long long>(
                        dump_rtv != m_render_target_views.end()
                            ? dump_rtv->second.texture : 0),
                    static_cast<unsigned long long>(
                        dump_dsv != m_depth_stencil_views.end()
                            ? dump_dsv->second.texture : 0),
                    dump_dsv != m_depth_stencil_views.end()
                        ? dump_dsv->second.flags : 0u,
                    dump_dsv != m_depth_stencil_views.end()
                        ? dump_dsv->second.mip_slice : 0u);
                const auto depth_state =
                    m_depth_stencil_states.find(state.depth_stencil_state);
                if (depth_state != m_depth_stencil_states.end()) {
                    const auto& depth = depth_state->second.desc;
                    std::fprintf(file,
                        "depth_enable=%u depth_write=%u depth_func=%u "
                        "stencil_enable=%u stencil_ref=%u "
                        "stencil_read=%u stencil_write=%u "
                        "front=%u,%u,%u,%u back=%u,%u,%u,%u\n",
                        depth.depth_enable, depth.depth_write_mask,
                        depth.depth_func, depth.stencil_enable,
                        state.stencil_ref, depth.stencil_read_mask,
                        depth.stencil_write_mask,
                        depth.front_stencil_fail_op,
                        depth.front_stencil_depth_fail_op,
                        depth.front_stencil_pass_op, depth.front_stencil_func,
                        depth.back_stencil_fail_op,
                        depth.back_stencil_depth_fail_op,
                        depth.back_stencil_pass_op, depth.back_stencil_func);
                }
                const auto dump_layout =
                    m_input_layouts.find(state.input_layout);
                if (dump_layout != m_input_layouts.end()) {
                    for (const auto& element : dump_layout->second.elements) {
                        std::fprintf(
                            file,
                            "input=%s%u format=%u slot=%u offset=%u "
                            "class=%u step=%u\n",
                            element.semantic_name.c_str(),
                            element.semantic_index, element.format,
                            element.input_slot, element.aligned_byte_offset,
                            element.input_slot_class,
                            element.instance_data_step_rate);
                    }
                }
                for (unsigned slot = 0; slot < state.vertex_buffers.size();
                     ++slot) {
                    std::fprintf(file,
                                 "vb%u=0x%llx stride=%u offset=%u\n", slot,
                                 static_cast<unsigned long long>(
                                     state.vertex_buffers[slot]),
                                 state.vertex_buffer_strides[slot],
                                 state.vertex_buffer_offsets[slot]);
                }
                std::fprintf(file,
                             "ib=0x%llx format=%u offset=%u\n",
                             static_cast<unsigned long long>(
                                 state.index_buffer),
                             state.index_buffer_format,
                             state.index_buffer_offset);
                for (unsigned slot = 0;
                     slot < state.vertex_constant_buffers.size(); ++slot) {
                    std::fprintf(
                        file, "vcb%u=0x%llx pcb%u=0x%llx\n", slot,
                        static_cast<unsigned long long>(
                            state.vertex_constant_buffers[slot]),
                        slot,
                        static_cast<unsigned long long>(
                            state.pixel_constant_buffers[slot]));
                }
                for (unsigned slot = 0;
                     slot < state.pixel_shader_resources.size(); ++slot) {
                    const GuestAddr srv_address =
                        state.pixel_shader_resources[slot];
                    GuestAddr texture_address = 0;
                    u32 format = 0;
                    u32 most_detailed_mip = 0;
                    u32 mip_levels = 0;
                    const auto srv =
                        m_shader_resource_views.find(srv_address);
                    if (srv != m_shader_resource_views.end()) {
                        texture_address = srv->second.texture;
                        format = srv->second.format;
                        most_detailed_mip = srv->second.most_detailed_mip;
                        mip_levels = srv->second.mip_levels;
                    }
                    // Opt-in GPU readback distinguishes missing source pixels
                    // from a draw/presentation failure. Keep it bounded by the
                    // existing draw capture limit; readback changes timing.
                    const auto texture = m_textures.find(texture_address);
                    if (!metadata_only &&
                        std::getenv("VITREO_D3D11_DUMP_DRAW_TEXTURES") &&
                        texture != m_textures.end() && texture->second.backend &&
                        (format == 28 || format == 87)) {
                        const auto& desc = texture->second.desc;
                        const u32 width = std::max(1u, desc.width >> std::min(most_detailed_mip, 31u));
                        const u32 height = std::max(1u, desc.height >> std::min(most_detailed_mip, 31u));
                        if (width <= 2048 && height <= 2048) {
                            std::vector<u8> pixels(static_cast<usize>(width) * height * 4);
                            m_backend.wait_idle();
                            if (texture->second.backend->readback_slice(
                                    most_detailed_mip, 0, pixels.data(), width * 4)) {
                                char texture_path[768];
                                std::snprintf(texture_path, sizeof(texture_path),
                                    "%s-draw%d-t%u-%ux%u.rgba", draw_dump_prefix,
                                    draw_number, slot, width, height);
                                if (FILE* texture_file = std::fopen(texture_path, "wb")) {
                                    std::fwrite(pixels.data(), 1, pixels.size(), texture_file);
                                    std::fclose(texture_file);
                                }
                            }
                        }
                    }
                    std::fprintf(
                        file,
                        "t%u_srv=0x%llx texture=0x%llx format=%u "
                        "first_mip=%u mips=%u sampler=0x%llx\n",
                        slot,
                        static_cast<unsigned long long>(srv_address),
                        static_cast<unsigned long long>(texture_address),
                        format, most_detailed_mip, mip_levels,
                        static_cast<unsigned long long>(
                            state.pixel_samplers[slot]));
                }
                // Vertex-stage t#: a GPU-scene draw reads its transforms
                // from structured/typed buffer SRVs the CPU never wrote, so
                // VITREO_D3D11_DUMP_DRAW_VS_BUFFERS=1 reads them back from
                // the GPU (after a wait) into -vtN.bin for an exact replay.
                for (unsigned slot = 0;
                     slot < state.vertex_shader_resources.size(); ++slot) {
                    const GuestAddr srv_address =
                        state.vertex_shader_resources[slot];
                    if (srv_address == 0) continue;
                    const auto srv = m_shader_resource_views.find(srv_address);
                    if (srv == m_shader_resource_views.end()) continue;
                    if (srv->second.buffer == 0) {
                        std::fprintf(file,
                                     "vt%u_srv=0x%llx texture=0x%llx format=%u\n",
                                     slot,
                                     static_cast<unsigned long long>(srv_address),
                                     static_cast<unsigned long long>(
                                         srv->second.texture),
                                     srv->second.format);
                        continue;
                    }
                    const auto buffer = m_buffers.find(srv->second.buffer);
                    usize bytes = 0;
                    if (buffer != m_buffers.end() && !metadata_only &&
                        std::getenv("VITREO_D3D11_DUMP_DRAW_VS_BUFFERS")) {
                        std::vector<u8> data(buffer->second.desc.byte_width);
                        bool read = false;
                        if (buffer->second.backend) {
                            m_backend.wait_idle();
                            read = buffer->second.backend->read(
                                0, data.data(), data.size());
                        }
                        if (!read && buffer->second.shadow.size() == data.size()) {
                            data = buffer->second.shadow;
                            read = true;
                        }
                        if (read) {
                            std::snprintf(path, sizeof(path),
                                          "%s-draw%d-vt%u.bin",
                                          draw_dump_prefix, draw_number, slot);
                            if (FILE* out = std::fopen(path, "wb")) {
                                std::fwrite(data.data(), 1, data.size(), out);
                                std::fclose(out);
                                bytes = data.size();
                            }
                        }
                    }
                    std::fprintf(
                        file,
                        "vt%u_srv=0x%llx buffer=0x%llx bytes=%zu "
                        "element_bytes=%u first=%u count=%u format=%u "
                        "typed=%d structured=%d\n",
                        slot, static_cast<unsigned long long>(srv_address),
                        static_cast<unsigned long long>(srv->second.buffer),
                        bytes, srv->second.element_bytes,
                        srv->second.first_element, srv->second.num_elements,
                        srv->second.format,
                        srv->second.texture_buffer != nullptr ? 1 : 0,
                        buffer != m_buffers.end() &&
                                buffer->second.desc.structure_byte_stride != 0
                            ? 1 : 0);
                }
                std::fclose(file);
            }
            pipeline_diag("Dumped draw inputs prefix=%s draw=%d",
                          draw_dump_prefix, draw_number);
        }
    }

    // /tmp/vitreo-draw-dump: the marker-file form of the dump above, to the
    // log (guests launched by a store client do not inherit the environment).
    // One atomic check per draw while the marker is absent.
    if (live_diag::draw_dump_marker().present()) {
        log_draw_dump(state, DrawDumpCall{indexed, count, start_location,
                                          base_vertex, instance_count,
                                          first_instance, indirect_args,
                                          indirect_offset});
    }

    if (pipeline_trace_enabled()) {
    unsigned bound_vertex_streams = 0;
    unsigned bound_vertex_cbs = 0;
    unsigned bound_fragment_cbs = 0;
    unsigned bound_textures = 0;
    unsigned bound_samplers = 0;
    for (const auto* stream : call.vertex_buffers) {
        bound_vertex_streams += stream != nullptr;
    }
    for (const auto* buffer : call.vertex_constant_buffers) {
        bound_vertex_cbs += buffer != nullptr;
    }
    for (const auto* buffer : call.fragment_constant_buffers) {
        bound_fragment_cbs += buffer != nullptr;
    }
    for (u32 slot = 0; slot < graphics::BACKEND_MAX_TEXTURE_SLOTS; ++slot) {
        bound_textures += call.textures[slot] != nullptr;
        bound_samplers += state.pixel_samplers[slot] != 0;
    }
    pipeline_diag(
        "Draw state indexed=%u rt=0x%llx target=%p fmt=%u viewport="
        "(%.1f,%.1f %.1fx%.1f %.3f..%.3f) scissor=%u "
        "streams=%u vcb=%u fcb=%u textures=%u samplers=%u "
        "blend=%u write=0x%x topology=%u tex0=%p srv0=0x%llx "
        "srv0_tex=0x%llx rt0_tex=0x%llx",
        indexed ? 1u : 0u,
        static_cast<unsigned long long>(state.render_targets[0]),
        // The backend object actually rendered into. A flip-model Present
        // rotates this pointer inside the same guest Texture2D, so it is the
        // only way to tell "drew into the presented allocation" from "drew
        // into the one that just rotated out".
        static_cast<const void*>(color_targets[0]),
        static_cast<unsigned>(color_format), call.viewport[0],
        call.viewport[1], call.viewport[2], call.viewport[3],
        call.viewport[4], call.viewport[5],
        call.scissor_enabled ? 1u : 0u, bound_vertex_streams,
        bound_vertex_cbs, bound_fragment_cbs, bound_textures,
        bound_samplers, blend_enabled ? 1u : 0u, write_mask,
        state.primitive_topology,
        // Which texture the draw samples in slot 0. A composite that
        // targets the right allocation can still produce black by sampling
        // one that was never rendered into, and the target alone cannot
        // distinguish those two.
        static_cast<const void*>(call.textures[0]),
        // Guest-side identity of what slot 0 samples, and of the render
        // target. If the composite's SRV names the same guest texture that
        // the scene passes rendered into, then a mismatch in the backend
        // pointers is our view->allocation resolution; if the guest texture
        // differs too, the guest bound something else.
        static_cast<unsigned long long>(state.pixel_shader_resources[0]),
        static_cast<unsigned long long>(
            m_shader_resource_views.count(state.pixel_shader_resources[0])
                ? m_shader_resource_views.at(state.pixel_shader_resources[0])
                      .texture
                : 0u),
        static_cast<unsigned long long>(
            m_render_target_views.count(state.render_targets[0])
                ? m_render_target_views.at(state.render_targets[0]).texture
                : 0u));
    }
    // Discriminator for compositor coverage: the vertex shader multiplies Y
    // by DriverConstants.clipControlOrigin (cb1[3].z). Dump those shadow
    // floats so a black Present can be attributed to zero constant buffers
    // versus a later GPU path, without changing submission behavior.
    if (static_cast<int>(state.viewports[0][2] + 0.5f) == 700) {
        static int vcb_float_dumps = 0;
        if (vcb_float_dumps < 8) {
            ++vcb_float_dumps;
            for (unsigned slot = 0;
                 slot < state.vertex_constant_buffers.size(); ++slot) {
                const GuestAddr address = state.vertex_constant_buffers[slot];
                if (address == 0) continue;
                auto buffer = m_buffers.find(address);
                if (buffer == m_buffers.end() ||
                    buffer->second.shadow.size() < 16) {
                    continue;
                }
                const auto* f = reinterpret_cast<const float*>(
                    buffer->second.shadow.data());
                const usize nfloat =
                    buffer->second.shadow.size() / sizeof(float);
                const usize show = nfloat < 16 ? nfloat : 16;
                char line[512];
                usize used = 0;
                used += static_cast<usize>(std::snprintf(
                    line + used, sizeof(line) - used,
                    "VCB slot=%u bytes=%zu floats:", slot,
                    buffer->second.shadow.size()));
                for (usize i = 0; i < show && used + 24 < sizeof(line); ++i) {
                    used += static_cast<usize>(std::snprintf(
                        line + used, sizeof(line) - used, " %g",
                        static_cast<double>(f[i])));
                }
                if (buffer->second.shadow.size() >= 64) {
                    const float* c3 = f + 12; // cb1[3]
                    used += static_cast<usize>(std::snprintf(
                        line + used, sizeof(line) - used,
                        " | c3=(%g,%g,%g,%g)",
                        static_cast<double>(c3[0]),
                        static_cast<double>(c3[1]),
                        static_cast<double>(c3[2]),
                        static_cast<double>(c3[3])));
                }
                pipeline_diag("%s", line);
            }
            pipeline_diag(
                "SRV mips: tex0_mipmaps_enabled=%u",
                call.samplers[0].mipmaps_enabled ? 1u : 0u);
            // Dump the first few floats of each bound vertex stream so a
            // collapsed compositor quad can be distinguished from a CB bug.
            for (unsigned slot = 0; slot < state.vertex_buffers.size();
                 ++slot) {
                const GuestAddr address = state.vertex_buffers[slot];
                if (address == 0) continue;
                auto buffer = m_buffers.find(address);
                if (buffer == m_buffers.end() ||
                    buffer->second.shadow.size() < 16) {
                    continue;
                }
                const u32 stride = state.vertex_buffer_strides[slot] != 0
                    ? state.vertex_buffer_strides[slot]
                    : 16u;
                const u32 offset = state.vertex_buffer_offsets[slot];
                const auto* base = buffer->second.shadow.data();
                const usize bytes = buffer->second.shadow.size();
                char line[512];
                usize used = static_cast<usize>(std::snprintf(
                    line, sizeof(line),
                    "VB slot=%u stride=%u offset=%u count=%u:", slot, stride,
                    offset, count));
                for (u32 v = 0; v < 4 && v < count; ++v) {
                    const usize at =
                        static_cast<usize>(offset) +
                        static_cast<usize>(v) * stride;
                    if (at + sizeof(float) * 4 > bytes) break;
                    const auto* f =
                        reinterpret_cast<const float*>(base + at);
                    used += static_cast<usize>(std::snprintf(
                        line + used, sizeof(line) - used,
                        " v%u=(%g,%g,%g,%g)", v,
                        static_cast<double>(f[0]),
                        static_cast<double>(f[1]),
                        static_cast<double>(f[2]),
                        static_cast<double>(f[3])));
                }
                pipeline_diag("%s", line);
            }
        }
    }

    if (state.rasterizer_state != 0) {
        auto rs = m_rasterizer_states.find(state.rasterizer_state);
        if (rs != m_rasterizer_states.end()) {
            call.front_counter_clockwise =
                rs->second.desc.front_counter_clockwise != 0;
            call.fill_mode = rs->second.desc.fill_mode == 2u
                                 ? graphics::BackendFillMode::Lines
                                 : graphics::BackendFillMode::Fill;
            call.depth_clip_enabled =
                rs->second.desc.depth_clip_enable != 0;
            call.depth_bias = static_cast<float>(rs->second.desc.depth_bias);
            call.depth_bias_slope_scale =
                rs->second.desc.slope_scaled_depth_bias;
            call.depth_bias_clamp = rs->second.desc.depth_bias_clamp;
            switch (rs->second.desc.cull_mode) {
            case 2:
                call.cull_mode = graphics::BackendCullMode::Front;
                break;
            case 3:
                call.cull_mode = graphics::BackendCullMode::Back;
                break;
            default:
                call.cull_mode = graphics::BackendCullMode::None;
                break;
            }
            // Learn D3D11_RASTERIZER_DESC: lines -- and wireframe edges,
            // which rasterize as lines (D3D11.3 15.13) -- are quadrilaterals
            // with MultisampleEnable, and alpha-antialiased (coverage into
            // alpha, 15.14.1) with AntialiasedLineEnable alone. Metal draws
            // only aliased lines; execute_line_quads and geometry_expand
            // draw the rectangles, and what reaches here in such a mode is
            // what they cannot serve (indirect draws, tessellation output,
            // triangles with adjacency in WIREFRAME).
            const bool draws_lines =
                !line_quad_pass &&
                (input_class == graphics::BackendPrimitiveClass::Line ||
                 (input_class == graphics::BackendPrimitiveClass::Triangle &&
                  rs->second.desc.fill_mode == 2u));
            if (draws_lines && (rs->second.desc.multisample_enable != 0 ||
                                rs->second.desc.antialiased_line_enable != 0)) {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED d3d11!Draw: "
                                 "quadrilateral (MultisampleEnable) or "
                                 "alpha-antialiased (AntialiasedLineEnable) "
                                 "lines of an indirect draw, tessellation "
                                 "output or adjacency WIREFRAME triangles "
                                 "are drawn aliased\n");
                }
            }
            // D3D11.3 15.7: the default (and every unset) scissor rect is
            // the empty {0,0,0,0}, so ScissorEnable with no rect set draws
            // nothing (measured, d3d11_a07_rs_probe `R3 scissor on, no rect
            // set`); RSSetScissorRects(0, ...) left state.scissor_rects zero.
            call.scissor_enabled = rs->second.desc.scissor_enable != 0;
        }
    }
    // Pass C of lines drawn as rectangles (execute_line_quads): they are
    // lines to D3D -- never culled (a wireframe triangle's facing was
    // applied by the vertex shader) and always filled -- and a line takes no
    // depth bias (measured, d3d11_a07_rs_probe `R4 ... line`) while a
    // wireframe edge keeps its triangle's (`R4 ... wireframe`).
    if (line_quad_pass) {
        call.cull_mode = graphics::BackendCullMode::None;
        call.fill_mode = graphics::BackendFillMode::Fill;
        if (variant->line_quad_divisor == 2) {
            call.depth_bias = 0.0f;
            call.depth_bias_slope_scale = 0.0f;
            call.depth_bias_clamp = 0.0f;
        }
    }
    // D3D11.3 15.10, UNORM depth: Bias = DepthBias * r, r the smallest
    // representable step (1/(2^24 - 1) for D24, 1/65535 for D16), for every
    // z. D24_UNORM_S8_UINT lives in Depth32Float_Stencil8 on Apple GPUs and
    // gets Metal's float rule, DepthBias * 2^(exponent(max z) - 23) -- half
    // the bias at z = .25 (measured, d3d11_a07_rs_probe `R4 D24S8`) -- and
    // Metal's own Depth16Unorm constant lands a unit or two off (`R4 D16`).
    // So the constant part moves to the vertex stages as a clip-space depth
    // offset (exact: z_ndc + o for every vertex is window z + o * (MaxDepth -
    // MinDepth)), and Metal keeps the slope part, with its clamp re-based so
    // min/max(Clamp, k + slope) comes out whole. Only triangles are biased
    // (15.10), wireframe included.
    const bool unorm16_depth = depth_format == graphics::PixelFormat::D16Unorm;
    if ((depth_format == graphics::PixelFormat::D24Unorm_S8Uint ||
         unorm16_depth) &&
        (call.depth_bias != 0.0f || call.depth_bias_slope_scale != 0.0f)) {
        const float r = unorm16_depth ? 1.0f / 65535.0f : 1.0f / 16777215.0f;
        const float constant = call.depth_bias * r;
        const float slope = call.depth_bias_slope_scale;
        const float clamp = call.depth_bias_clamp;
        float offset = constant;
        float metal_clamp = 0.0f;
        float metal_slope = slope;
        if (clamp != 0.0f && slope == 0.0f) {
            offset = clamp > 0.0f ? std::min(clamp, constant)
                                  : std::max(clamp, constant);
        } else if (clamp != 0.0f) {
            // min(C, k + s) = k + min(C - k, s) (C > 0; max for C < 0).
            // Metal's clamp is a min only while positive and a max only
            // while negative, so C - k on the wrong side of 0 is exact only
            // when the slope term's sign settles it: s >= 0 >= C - k gives C.
            const float rebased = clamp - constant;
            if (clamp > 0.0f ? rebased > 0.0f : rebased < 0.0f) {
                metal_clamp = rebased;
            } else if (clamp > 0.0f ? slope > 0.0f : slope < 0.0f) {
                offset = clamp;
                metal_slope = 0.0f;
            } else {
                static bool reported = false;
                if (!reported) {
                    reported = true;
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED d3d11!Draw: D24 "
                                 "DepthBias*r beyond DepthBiasClamp with an "
                                 "opposing SlopeScaledDepthBias\n");
                }
                metal_clamp = rebased;
            }
        }
        if (input_class == graphics::BackendPrimitiveClass::Triangle) {
            const float range = state.viewports[0][5] - state.viewports[0][4];
            vertex_push.depth_offset = range != 0.0f ? offset / range : 0.0f;
        }
        call.depth_bias = 0.0f;
        call.depth_bias_slope_scale = metal_slope;
        call.depth_bias_clamp = metal_clamp;
    }
    // The pixel shader's fixed-function inputs: SampleMask (applied by a
    // coverage-writing variant, see ps_sample_mask above) and viewport 0's
    // depth range for the SV_Depth clamp.
    fragment_push.sample_mask = state.sample_mask;
    fragment_push.depth_min = state.viewports[0][4];
    fragment_push.depth_max = state.viewports[0][5];
    if (call.viewports != nullptr && !pixel_shader_disabled &&
        ps->second.writes_depth) {
        // The clamp reads one range; a primitive routed to another viewport
        // with a different one would be clamped to viewport 0's.
        for (u32 i = 1; i < state.viewport_count; ++i) {
            if (state.viewports[i][4] == fragment_push.depth_min &&
                state.viewports[i][5] == fragment_push.depth_max) {
                continue;
            }
            static bool reported = false;
            if (!reported) {
                reported = true;
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d11!Draw: SV_Depth "
                             "clamp with per-viewport depth ranges (viewport "
                             "0's is used)\n");
            }
            break;
        }
    }
    pipeline_diag("Raster state cull=%u front_ccw=%u",
                  static_cast<unsigned>(call.cull_mode),
                  call.front_counter_clockwise ? 1u : 0u);

    // ---- R3c native-state: the bound depth-stencil object, for
    // settle_native_states (null: the NULL default state).
    const DepthStencilState* draw_dss = nullptr;
    // ---- end R3c
    if (state.depth_stencil_state == 0) {
        // Learn OMSetDepthStencilState / D3D11_DEPTH_STENCIL_DESC: NULL binds
        // the default state -- DepthEnable TRUE, DepthWriteMask ALL,
        // DepthFunc LESS, StencilEnable FALSE (masks 0xFF, ops KEEP, func
        // ALWAYS).
        call.depth_test_enabled = true;
        call.depth_write_enabled =
            (depth_view_flags & d3d11::DSV_READ_ONLY_DEPTH) == 0;
        call.depth_compare = graphics::BackendCompare::Less;
        call.stencil_test_enabled = false;
        call.stencil_read_mask = 0xFFu;
        call.stencil_write_mask = 0xFFu;
        const graphics::BackendStencilFaceDesc keep_always{
            graphics::BackendStencilOperation::Keep,
            graphics::BackendStencilOperation::Keep,
            graphics::BackendStencilOperation::Keep,
            graphics::BackendCompare::Always,
        };
        call.front_stencil = keep_always;
        call.back_stencil = keep_always;
    }
    if (state.depth_stencil_state != 0) {
        auto dss = m_depth_stencil_states.find(state.depth_stencil_state);
        if (dss != m_depth_stencil_states.end()) {
            draw_dss = &dss->second;  // R3c
            call.depth_test_enabled = dss->second.desc.depth_enable != 0;
            // D3D11.3 17.9: "When DepthEnable is false, depth test and depth
            // buffer write are not performed, regardless of any other
            // settings."
            call.depth_write_enabled =
                dss->second.desc.depth_enable != 0 &&
                dss->second.desc.depth_write_mask != 0 &&
                (depth_view_flags & d3d11::DSV_READ_ONLY_DEPTH) == 0;
            call.depth_compare =
                compare_from_d3d11(dss->second.desc.depth_func);
            call.stencil_test_enabled =
                dss->second.desc.stencil_enable != 0;
            call.stencil_read_mask = dss->second.desc.stencil_read_mask;
            call.stencil_write_mask =
                (depth_view_flags & d3d11::DSV_READ_ONLY_STENCIL) == 0
                    ? dss->second.desc.stencil_write_mask
                    : 0;
            call.stencil_reference = state.stencil_ref & 0xFFu;
            call.front_stencil = {
                stencil_op_from_d3d11(
                    dss->second.desc.front_stencil_fail_op),
                stencil_op_from_d3d11(
                    dss->second.desc.front_stencil_depth_fail_op),
                stencil_op_from_d3d11(
                    dss->second.desc.front_stencil_pass_op),
                compare_from_d3d11(dss->second.desc.front_stencil_func),
            };
            call.back_stencil = {
                stencil_op_from_d3d11(
                    dss->second.desc.back_stencil_fail_op),
                stencil_op_from_d3d11(
                    dss->second.desc.back_stencil_depth_fail_op),
                stencil_op_from_d3d11(
                    dss->second.desc.back_stencil_pass_op),
                compare_from_d3d11(dss->second.desc.back_stencil_func),
            };
        }
    }
    // Diagnostic: VITREO_D3D11_DUMP_DEPTH_AT_STENCIL_DRAW=<path> writes the
    // depth plane the FIRST stencil-enabled, read-only-depth draw (a deferred
    // base pass) is about to test against: float32 texels after a 16-byte
    // header {w, h, 0, 0}. Measures what the GREATER_EQUAL test sees, not
    // what a later pass left behind.
    static const std::string depth_dump_path = [] {
        const char* path =
            std::getenv("VITREO_D3D11_DUMP_DEPTH_AT_STENCIL_DRAW");
        return std::string(path != nullptr ? path : "");
    }();
    if (!depth_dump_path.empty() && depth != nullptr &&
        call.stencil_test_enabled && call.depth_test_enabled &&
        (depth_view_flags & d3d11::DSV_READ_ONLY_DEPTH) != 0) {
        const char* depth_dump = depth_dump_path.c_str();
        static bool dumped = false;
        // Optional gate: with _TRIGGER set, wait until that file exists so the
        // dump lands in the frame of interest, not the first loading frame.
        bool armed = true;
        if (const char* trigger = std::getenv(
                "VITREO_D3D11_DUMP_DEPTH_AT_STENCIL_DRAW_TRIGGER");
            trigger != nullptr && trigger[0] != '\0') {
            FILE* probe_file = std::fopen(trigger, "rb");
            armed = probe_file != nullptr;
            if (probe_file != nullptr) std::fclose(probe_file);
        }
        if (!dumped && armed) {
            dumped = true;
            const auto& dd = depth->desc();
            std::vector<float> plane(static_cast<usize>(dd.width) * dd.height);
            m_backend.wait_idle();
            const bool ok = depth->readback_depth_stencil_plane(
                depth_mip_slice, depth_array_slice, false, plane.data(),
                dd.width * 4u);
            if (FILE* out = std::fopen(depth_dump, "wb")) {
                const u32 header[4] = {dd.width, dd.height, 0u, 0u};
                std::fwrite(header, sizeof(header), 1, out);
                std::fwrite(plane.data(), 4, plane.size(), out);
                std::fclose(out);
            }
            pipeline_diag("Dumped depth before first stencil draw -> %s (%s)",
                          depth_dump, ok ? "ok" : "readback failed");
        }
    }

    if (is_geometry_output) {
        // Pass C: a list of the expanded vertices, read from the expansion
        // buffer as the pass-through shader's t0, counted by the GPU.
        call.topology = variant->topology;
        call.vertex_buffer = nullptr;
        call.vertex_buffers.fill(nullptr);
        call.index_buffer = nullptr;
        call.index_count = 0;
        call.base_vertex = 0;
        call.first_vertex = 0;
        // Every invocation's whole region; the pass-through culls the slots
        // an invocation did not write.
        call.vertex_count = variant->vertex_count;
        call.instance_count = 1;
        call.first_instance = 0;
        call.indirect.buffer = nullptr;
        call.indirect.offset = 0;
        call.vertex_descriptor_tables.fill(nullptr);
        call.vertex_srv_buffers[0] = variant->buffer;
        call.vertex_srv_buffer_offsets[0] = variant->offset;
        // Dense records (a domain shader's) have no counts to cull by; the
        // shader still declares the binding.
        call.vertex_srv_buffers[1] =
            variant->counts != nullptr ? variant->counts : variant->buffer;
        call.vertex_srv_buffer_offsets[1] =
            variant->counts != nullptr ? variant->counts_offset : 0u;
        if (variant->line_params != nullptr) {
            // A line-quad pass's LineQuadParams.
            call.vertex_srv_buffers[2] = variant->line_params;
            call.vertex_srv_buffer_offsets[2] = 0;
        }
        if (variant->index_buffer != nullptr) {
            // The tessellator's connectivity: 32-bit record indices.
            call.index_buffer = variant->index_buffer;
            call.index_32bit = true;
            call.index_buffer_offset = variant->index_offset;
            call.index_count = variant->vertex_count;
            call.vertex_count = 0;
        }
        if (variant->indirect.active()) {
            call.indirect.buffer = variant->indirect.args;
            call.indirect.offset = variant->indirect.offset;
        }
    }
    if (is_tessellated) {
        // The patch draw: no input assembler at all. The domain shader reads
        // the hull kernel's output buffers at the expansion window.
        call.vertex_buffer = nullptr;
        call.vertex_buffers.fill(nullptr);
        call.index_buffer = nullptr;
        call.index_count = 0;
        call.vertex_count = 0;
        call.indirect.buffer = nullptr;
        call.tessellation_factors = variant->tess_factors;
        call.patch_count = variant->patch_count;
        call.patch_control_points = variant->patch_control_points;
        call.instance_count = 1;
        call.first_instance = 0;
        constexpr unsigned window = graphics::BACKEND_GS_EXPANSION_BUFFER_BASE -
                                    graphics::BACKEND_VERTEX_BUFFER_BASE;
        static_assert(window == 0, "the expansion window is the IA window");
        call.vertex_buffers[1] = variant->tess_control_points;
        call.vertex_buffers[2] = variant->tess_control;
        call.vertex_buffers[4] = variant->tess_patch_constants;
        call.vertex_buffer_offsets.fill(0);
    }
    if (is_capture) {
        // Nothing is rasterized; keep the fixed-function state from asking
        // anything of the 1x1 target.
        call.viewport = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        call.scissor_enabled = false;
        call.depth_test_enabled = false;
        call.depth_write_enabled = false;
        call.stencil_test_enabled = false;
    }

    graphics::BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets = color_targets;
    pass_desc.color_formats = color_formats;
    pass_desc.color_targets_3d = color_targets_3d;
    pass_desc.color_depth_planes = color_depth_planes;
    pass_desc.color_levels = color_mip_slices;
    pass_desc.color_slices = color_array_slices;
    pass_desc.color_array_sizes = color_array_sizes;
    pass_desc.preserve_color_contents = true;
    pass_desc.depth_stencil_target = depth;
    pass_desc.depth_level = depth_mip_slice;
    pass_desc.depth_slice = depth_array_slice;
    pass_desc.depth_array_size = depth_array_size;
    if (attachmentless) {
        pass_desc.attachmentless_width = target_width;
        pass_desc.attachmentless_height = target_height;
        pass_desc.attachmentless_samples = sample_count;
    }
    pass_desc.center_sample_positions = centre_forced_sample ||
                                        per_pixel_lines;
    // OCCLUSION queries building on this context count the samples this
    // draw passes (a capture draw rasterizes nothing).
    if (!is_capture) {
        pass_desc.occlusion_counter = occlusion_counter_for_draw(context);
    }
    // A buffer target's proxy takes the buffer's current contents, so the
    // pass loads (and blends over) what the window holds.
    for (const GuestAddr view : buffer_target_views) {
        if (view == 0) continue;
        if (!sync_buffer_render_target(m_render_target_views.at(view), true)) {
            pipeline_diag("Draw skipped: a buffer render target could not be "
                          "filled");
            return;
        }
    }
    auto pass = m_backend.create_render_pass_standalone(pass_desc);
    if (!pass || !pass->valid()) {
        pipeline_reject_diag(
            "render pass creation failed rt=0x%llx dsv=0x%llx",
            static_cast<unsigned long long>(state.render_targets[0]),
            static_cast<unsigned long long>(state.depth_stencil_view));
        pipeline_diag("Draw skipped: create_render_pass_standalone failed");
        return;
    }
    // ---- P0 state census (VITREO_D3D11_STATE_CENSUS=1, DIAGNOSTIC).
    if (d3d11_census::enabled) [[unlikely]] {
        census_backend_draw(context, variant != nullptr, pipeline_key,
                            pass_desc, call);
    }
    // ---- end P0 state census
    // ---- R3c native-state: last, after every override of the call.
    if (native_states) {
        settle_native_states(call, native, draw_dss,
                             sampler_memo && !d3d11::draw_memo::verify());
    }
    // ---- end R3c
    const bool drew = pass->draw(call);
    const bool ended = pass->end();
    for (const GuestAddr view : buffer_target_views) {
        if (view == 0) continue;
        if (!sync_buffer_render_target(m_render_target_views.at(view),
                                       false)) {
            pipeline_diag("a buffer render target's result could not be "
                          "copied back");
        }
    }
    if (variant != nullptr) variant->submitted = drew && ended;
    // The draw counted its pixel-shader invocations into the statistics
    // record, and the primitives dispatch clips its captured vertices into
    // it: the queries wait for the generation those went into, read now.
    const bool statistics_recorded =
        pixel_statistics != nullptr || statistics.active;
    if (drew && ended) finish_statistics_draw(statistics);
    if (statistics_recorded) note_statistics_recorded(context);
    if (!drew || !ended) {
        // Counted with the skips (the reject line prints only under
        // VITREO_D3D11_REJECT_DIAG).
        d3d11_skip_stat("Draw failed: the backend did not record the draw "
                        "or end its pass");
        pipeline_reject_diag("backend draw failed drew=%u ended=%u rt=0x%llx",
                             drew ? 1u : 0u, ended ? 1u : 0u,
                             static_cast<unsigned long long>(
                                 state.render_targets[0]));
    }
    if (drew && ended) {
        // Count the draw where it actually reaches the backend, matching the
        // D3D9 frontend. Without this the load-phase heartbeat reports
        // draws=0 for every D3D11 guest.
        diagnostics::phase_mark(diagnostics::LoadPhase::FirstDraw);
        diagnostics::phase_note_draw();
        note_immediate_gpu_work(context);
    }
    if (d3d11_skip_stats_enabled()) note_draw_encode_counts();
    pipeline_diag(
        "Draw%s count=%u start=%u base=%d -> %s",
        indexed ? "Indexed" : "", count, start_location, base_vertex,
        (drew && ended) ? "submitted" : "failed");
}

} // namespace vitreo::winapi

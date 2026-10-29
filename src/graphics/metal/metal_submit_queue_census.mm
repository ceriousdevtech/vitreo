// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_census.mm
/// @brief P0 state census, backend half (VITREO_D3D11_STATE_CENSUS=1;
///        DIAGNOSTIC).

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <CoreFoundation/CoreFoundation.h>
#import <pthread.h>

#include <cmath>
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_submit_queue_internal.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_texture.hpp"

#include <vitreo/present_cpu.hpp>
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"

#include <algorithm>
#include <string>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <utility>

namespace vitreo::graphics {

// ---- P0 state census, backend half (VITREO_D3D11_STATE_CENSUS=1).
// DIAGNOSTIC (Track R round 3 design §6.1): per recorded draw, the handles
// the CsDraw retains (release_draw's fields) and which CsDraw fields differ
// from the previous draw recorded on the same thread -- what a delta record
// would carry. Reads the draw, changes nothing. `[r3-census] backend` every
// 2^16 draws per recording thread.
const bool g_draw_census = [] {
    const char* v = std::getenv("VITREO_D3D11_STATE_CENSUS");
    return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
}();

namespace {
/// The delta-record field classes the census counts.
enum CensusField : u32 {
    kCfPipeline, kCfDepthStencil, kCfStencilRef, kCfVertexStreams,
    kCfIndex, kCfVsCb, kCfPsCb, kCfTextures, kCfSamplers, kCfVsTextures,
    kCfVsSamplers, kCfViewport, kCfScissor, kCfViewportArray, kCfRaster,
    kCfBlendColor, kCfPrimitive, kCfVsPush, kCfPsPush, kCfUav, kCfStatistics,
    kCfDescriptorTables, kCfResident, kCfCount
};
constexpr std::array<const char*, kCfCount> kCensusFieldNames{{
    "pipeline", "dss", "stencil_ref", "vb", "ib", "vs_cb", "ps_cb", "tex",
    "samp", "vs_tex", "vs_samp", "viewport", "scissor", "vp_array", "raster",
    "blend_color", "primitive", "vs_push", "ps_push", "uav", "stats",
    "desc_tables", "resident",
}};

struct DrawCensus {
    // The previous draw's fields (no vectors: push bytes are hashed).
    void* pipeline{nullptr};
    void* depth_stencil{nullptr};
    u32 stencil_reference{0};
    std::array<void*, 16> vertex_buffers{};
    std::array<usize, 16> vertex_offsets{};
    std::array<u32, 16> vertex_slots{};
    u32 vertex_stream_count{0};
    void* index_buffer{nullptr};
    usize index_offset{0};
    u32 index_type{0};
    std::array<void*, 16> vs_cbuffer{}, ps_cbuffer{};
    std::array<usize, 16> vs_cbuffer_off{}, ps_cbuffer_off{};
    std::array<void*, 16> textures{}, samplers{}, vertex_textures{},
        vertex_samplers{};
    std::array<float, 6> viewport{};
    std::array<u32, 5> scissor{};
    usize viewport_array{0};
    std::array<u32, 6> raster{};
    std::array<float, 3> bias{};
    std::array<float, 4> blend_color{};
    u32 primitive{0};
    u64 vs_push_hash{0}, ps_push_hash{0};
    std::array<void*, 8> ps_uav_buffer{};
    std::array<usize, 8> ps_uav_buffer_off{};
    std::array<void*, graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS>
        ps_uav_counter{};
    void* ps_statistics{nullptr};
    std::array<void*, 4> vs_tables{}, ps_tables{};
    usize resident{0};
    // The window.
    u64 draws{0};
    u64 retains{0};
    u64 fields{0};        ///< changed slots/fields, summed
    u64 classes{0};       ///< changed field classes, summed
    std::array<u64, kCfCount> class_changed{};
    std::array<u64, kCfCount> slots_changed{};
    std::array<u64, 8> field_hist{};  ///< 0, 1-2, 3-4, 5-8, 9-16, 17-32, 33-64, 65+
    std::vector<void*> samplers_seen;
    std::vector<void*> dss_seen;
};

u64 census_hash(const u8* bytes, usize size) {
    u64 h = 0xcbf29ce484222325ull;
    for (usize i = 0; i < size; ++i) h = (h ^ bytes[i]) * 0x100000001b3ull;
    return h ^ size;
}

template <class A>
u32 census_slots(A& prev, const A& cur) {
    if (std::memcmp(prev.data(), cur.data(), sizeof(cur)) == 0) return 0;
    u32 n = 0;
    for (usize i = 0; i < cur.size(); ++i) {
        if (std::memcmp(&prev[i], &cur[i], sizeof(cur[i])) != 0) ++n;
    }
    prev = cur;
    return n;
}

template <class T>
u32 census_one(T& prev, const T& cur) {
    if (std::memcmp(&prev, &cur, sizeof(cur)) == 0) return 0;
    prev = cur;
    return 1;
}

void census_note_seen(std::vector<void*>& seen, void* object) {
    if (object == nullptr) return;
    if (std::find(seen.begin(), seen.end(), object) == seen.end() &&
        seen.size() < 4096) {
        seen.push_back(object);
    }
}
}  // namespace

[[gnu::noinline, gnu::cold]] void census_record_draw(const CsDraw& d) {
    static thread_local DrawCensus c;
    // Retains: release_draw's fields, one per non-null handle.
    u64 retains = (d.pipeline != nullptr) + (d.depth_stencil != nullptr) +
                  (d.index_buffer != nullptr) + (d.indirect_args != nullptr) +
                  (d.ps_statistics != nullptr);
    const auto count = [](const auto& a) {
        u64 n = 0;
        for (void* p : a) n += p != nullptr;
        return n;
    };
    retains += count(d.vertex_buffers) + count(d.vs_cbuffer) +
               count(d.ps_cbuffer) + count(d.textures) +
               count(d.vertex_textures) + count(d.ps_uav_buffer) +
               count(d.ps_uav_counter) + count(d.vs_descriptor_tables) +
               count(d.ps_descriptor_tables) + d.resident.size();
    c.retains += retains;
    for (void* s : d.samplers) census_note_seen(c.samplers_seen, s);
    for (void* s : d.vertex_samplers) census_note_seen(c.samplers_seen, s);
    census_note_seen(c.dss_seen, d.depth_stencil);

    std::array<u32, kCfCount> changed{};
    changed[kCfPipeline] = census_one(c.pipeline, d.pipeline);
    changed[kCfDepthStencil] = census_one(c.depth_stencil, d.depth_stencil);
    changed[kCfStencilRef] = census_one(c.stencil_reference, d.stencil_reference);
    {
        u32 vb = census_slots(c.vertex_buffers, d.vertex_buffers);
        vb = std::max(vb, census_slots(c.vertex_offsets, d.vertex_offsets));
        vb = std::max(vb, census_slots(c.vertex_slots, d.vertex_slots));
        vb += census_one(c.vertex_stream_count, d.vertex_stream_count);
        changed[kCfVertexStreams] = vb;
    }
    changed[kCfIndex] = std::min(1u, census_one(c.index_buffer, d.index_buffer) +
                                         census_one(c.index_offset, d.index_offset) +
                                         census_one(c.index_type, d.index_type));
    changed[kCfVsCb] = std::max(census_slots(c.vs_cbuffer, d.vs_cbuffer),
                                census_slots(c.vs_cbuffer_off, d.vs_cbuffer_off));
    changed[kCfPsCb] = std::max(census_slots(c.ps_cbuffer, d.ps_cbuffer),
                                census_slots(c.ps_cbuffer_off, d.ps_cbuffer_off));
    changed[kCfTextures] = census_slots(c.textures, d.textures);
    changed[kCfSamplers] = census_slots(c.samplers, d.samplers);
    changed[kCfVsTextures] = census_slots(c.vertex_textures, d.vertex_textures);
    changed[kCfVsSamplers] = census_slots(c.vertex_samplers, d.vertex_samplers);
    changed[kCfViewport] = census_one(c.viewport, d.viewport);
    const std::array<u32, 5> scissor{d.scissor_x, d.scissor_y, d.scissor_w,
                                     d.scissor_h, d.scissor ? 1u : 0u};
    changed[kCfScissor] = census_one(c.scissor, scissor);
    changed[kCfViewportArray] = census_one(c.viewport_array, d.viewports.size());
    const std::array<u32, 6> raster{d.cull_mode, d.fill_mode, d.depth_clip_mode,
                                    d.winding, d.writes_depth_stencil ? 1u : 0u,
                                    d.indexed ? 1u : 0u};
    const std::array<float, 3> bias{d.depth_bias, d.depth_bias_slope_scale,
                                    d.depth_bias_clamp};
    changed[kCfRaster] = std::min(1u, census_one(c.raster, raster) +
                                          census_one(c.bias, bias));
    changed[kCfBlendColor] = census_one(c.blend_color, d.blend_color);
    changed[kCfPrimitive] = census_one(c.primitive, d.primitive);
    const u8* vs = d.push_pending ? d.vs_push : d.vs_const.data();
    const usize vs_n = d.push_pending ? d.vs_push_size : d.vs_const.size();
    const u8* ps = d.push_pending ? d.ps_push : d.ps_const.data();
    const usize ps_n = d.push_pending ? d.ps_push_size : d.ps_const.size();
    changed[kCfVsPush] = census_one(c.vs_push_hash, census_hash(vs, vs_n));
    changed[kCfPsPush] = census_one(c.ps_push_hash, census_hash(ps, ps_n));
    changed[kCfUav] = census_slots(c.ps_uav_buffer, d.ps_uav_buffer) +
                      census_slots(c.ps_uav_buffer_off, d.ps_uav_buffer_off) +
                      census_slots(c.ps_uav_counter, d.ps_uav_counter);
    changed[kCfStatistics] = census_one(c.ps_statistics, d.ps_statistics);
    changed[kCfDescriptorTables] =
        census_slots(c.vs_tables, d.vs_descriptor_tables) +
        census_slots(c.ps_tables, d.ps_descriptor_tables);
    changed[kCfResident] = census_one(c.resident, d.resident.size());
    u64 fields = 0;
    u64 classes = 0;
    for (u32 i = 0; i < kCfCount; ++i) {
        if (changed[i] == 0) continue;
        ++classes;
        fields += changed[i];
        ++c.class_changed[i];
        c.slots_changed[i] += changed[i];
    }
    c.fields += fields;
    c.classes += classes;
    const usize bucket = fields == 0 ? 0 : fields <= 2 ? 1 : fields <= 4 ? 2
                       : fields <= 8 ? 3 : fields <= 16 ? 4 : fields <= 32 ? 5
                       : fields <= 64 ? 6 : 7;
    ++c.field_hist[bucket];
    if ((++c.draws & 0xFFFFu) != 0) return;
    const double n = static_cast<double>(c.draws);
    char line[1400];
    int at = std::snprintf(
        line, sizeof(line),
        "[r3-census] backend draws=%llu retains/draw=%.2f "
        "changed_fields/draw=%.2f changed_classes/draw=%.2f "
        "distinct_samplers=%zu distinct_dss=%zu fields-hist%%(0,1-2,3-4,5-8,"
        "9-16,17-32,33-64,65+)",
        static_cast<unsigned long long>(c.draws), c.retains / n, c.fields / n,
        c.classes / n, c.samplers_seen.size(), c.dss_seen.size());
    for (u64 h : c.field_hist) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " %.1f", 100.0 * h / n);
    }
    at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                        " | class%%(slots/draw)");
    for (u32 i = 0; i < kCfCount; ++i) {
        at += std::snprintf(line + at, sizeof(line) - static_cast<usize>(at),
                            " %s=%.1f(%.2f)", kCensusFieldNames[i],
                            100.0 * c.class_changed[i] / n,
                            c.slots_changed[i] / n);
    }
    std::fprintf(stderr, "%s\n", line);
    // A window per report; the previous draw stays for the next compare.
    c.draws = 0;
    c.retains = 0;
    c.fields = 0;
    c.classes = 0;
    c.class_changed = {};
    c.slots_changed = {};
    c.field_hist = {};
    c.samplers_seen.clear();
    c.dss_seen.clear();
}
// ---- end P0 state census

} // namespace vitreo::graphics

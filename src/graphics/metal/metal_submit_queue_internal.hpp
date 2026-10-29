// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_internal.hpp
/// @brief Shared helpers for the MetalSubmitQueue translation units; private
///        to src/graphics/metal.

#pragma once

#include "graphics/metal/metal_submit_queue.hpp"

#include <atomic>

namespace vitreo::graphics {

// metal_submit_queue_ownership.mm:
// A draw's handles are all id<MTL*> (retain_cs_handle): release and retain
// them through the ObjC runtime directly rather than CFRelease/CFRetain.
void release_draw_handle(void* object);
void retain_draw_handle(void* object);

// The encoder's pass-merge rules, defined with them in
// metal_submit_queue_pass_merge.mm (draw_writes below); pass-fold applies
// the same ones at record time.
bool pass_merge_enabled();
bool gpu_watch_split();
void* texture_root(void* texture);

/// Whether @p draw writes memory another draw could read: pixel-shader UAV
/// buffers, textures and counters, the statistics record, or a u# descriptor
/// table. The b#, t# and s# tables hold read-only views.
inline bool draw_writes(const CsDraw& draw) {
    for (void* buffer : draw.ps_uav_buffer) {
        if (buffer != nullptr) return true;
    }
    for (void* counter : draw.ps_uav_counter) {
        if (counter != nullptr) return true;
    }
    if (draw.ps_statistics != nullptr) return true;
    for (usize i = graphics::BACKEND_UAV_TEXTURE_BASE;
         i < draw.textures.size(); ++i) {
        if (draw.textures[i] != nullptr) return true;
    }
    return draw.vs_descriptor_tables[graphics::BACKEND_DESCRIPTOR_SET_UAV] !=
               nullptr ||
           draw.ps_descriptor_tables[graphics::BACKEND_DESCRIPTOR_SET_UAV] !=
               nullptr;
}

// P0 state census, backend half (metal_submit_queue_census.mm).
extern const bool g_draw_census;
[[gnu::noinline, gnu::cold]] void census_record_draw(const CsDraw& d);

// -- delta-record (delta_record.hpp): the DeltaDraw format --------------------
//
// One DeltaDraw: a DeltaHead, the draw's own parameters (DeltaParams), then
// one entry per field that differs from the draw before it in the stream --
// from the default (unbound) draw for a keyframe, which the encoder applies
// to a reset state. Every entry is an 8-byte DeltaEntry and a payload whose
// size is a multiple of 8.

constexpr u8 kDeltaKeyframe = 1u;

struct DeltaHead {
    u32 bytes{0};    ///< the whole DeltaDraw
    u16 entries{0};
    u8 flags{0};
    u8 pad{0};
    u64 hash{0};     ///< VITREO_METAL_DELTA_VERIFY: cs_draw_state_hash
};
static_assert(sizeof(DeltaHead) == 16);

struct DeltaParams {
    u32 vertex_count{0};
    u32 index_count{0};
    u32 instance_count{1};
    u32 first_vertex{0};
    u32 first_instance{0};
    i32 base_vertex{0};
    void* indirect_args{nullptr};
    u64 indirect_offset{0};
    void* tess_factors{nullptr};
    u64 tess_factors_offset{0};
    u32 patch_count{0};
    u32 patch_control_points{0};
};
static_assert(sizeof(DeltaParams) % 8 == 0);

struct DeltaEntry {
    u16 field{0};
    u16 bytes{0};  ///< payload bytes after this header
    u32 a{0};
};
static_assert(sizeof(DeltaEntry) == 8);

/// Field ids. Slot arrays take one id per slot.
constexpr u16 kFPipeline = 0;
constexpr u16 kFDepth = 1;          ///< depth_stencil, stencil ref, writes
constexpr u16 kFStreamCount = 2;    ///< a = vertex_stream_count
constexpr u16 kFStream = 3;         ///< +16: buffer, offset; a = slot
constexpr u16 kFIndex = kFStream + 16;  ///< buffer, offset; a = type | indexed << 16
constexpr u16 kFVsCb = kFIndex + 1;     ///< +16: buffer, offset
constexpr u16 kFPsCb = kFVsCb + 16;     ///< +16
constexpr u16 kFTex = kFPsCb + 16;      ///< +16: handle
constexpr u16 kFSamp = kFTex + 16;      ///< +16
constexpr u16 kFVTex = kFSamp + 16;     ///< +16
constexpr u16 kFVSamp = kFVTex + 16;    ///< +16
constexpr u16 kFViewport = kFVSamp + 16;
constexpr u16 kFBlend = kFViewport + 1;
constexpr u16 kFScissor = kFBlend + 1;
constexpr u16 kFRaster = kFScissor + 1;
constexpr u16 kFUav = kFRaster + 1;     ///< +8: buffer, offset
constexpr u16 kFCounter = kFUav + 8;    ///< +BACKEND_MAX_UAV_COUNTER_BINDINGS
constexpr u16 kFStats =
    kFCounter + static_cast<u16>(graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS);
constexpr u16 kFVsTable = kFStats + 1;  ///< +4: handle
constexpr u16 kFPsTable = kFVsTable + 4;  ///< +4
constexpr u16 kFResident = kFPsTable + 4;  ///< a = count, then the handles
constexpr u16 kFViewportArray = kFResident + 1;  ///< a = count, viewports, scissors
constexpr u16 kFVsPush = kFViewportArray + 1;  ///< bytes, size
constexpr u16 kFPsPush = kFVsPush + 1;
constexpr u16 kFieldCount = kFPsPush + 1;

// metal_submit_queue_delta_apply.mm:
void reset_draw(CsDraw& draw);

/// Apply one DeltaDraw at @p record to @p state. False when it is not a
/// keyframe and @p valid is not set (nothing to apply it to).
bool apply_delta(const u8* record, CsDraw& state, bool& valid);

/// MetalSubmitQueue::set_delta_mutation_for_test writes it
/// (metal_submit_queue_delta_record.mm).
extern std::atomic<int> g_delta_mutation;

}  // namespace vitreo::graphics

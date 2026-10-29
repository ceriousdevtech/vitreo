// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_delta_record.mm
/// @brief delta-record producer: DeltaDraws written on the recording
///        thread.

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

namespace {
/// The unbound draw a keyframe is relative to and the encoder resets to.
const CsDraw& default_draw() {
    static const CsDraw* empty = new CsDraw();
    return *empty;
}

/// Serialises DeltaDraws into a byte vector.
struct DeltaWriter {
    std::vector<u8>& out;
    std::vector<void*>* retains;  ///< handles carried; null: none taken
    u16 entries{0};

    void raw(const void* data, usize size) {
        const auto* bytes = static_cast<const u8*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }
    void pad8() {
        while ((out.size() & 7u) != 0) out.push_back(0);
    }
    void entry(u16 field, u32 a, const void* payload, usize size) {
        DeltaEntry e{};
        e.field = field;
        e.bytes = static_cast<u16>((size + 7u) & ~usize{7});
        e.a = a;
        raw(&e, sizeof(e));
        if (size != 0) raw(payload, size);
        pad8();
        ++entries;
    }
    void retain(void* handle) {
        if (handle == nullptr || retains == nullptr) return;
        retain_draw_handle(handle);
        retains->push_back(handle);
    }
    void handle(u16 field, void* h, bool owned) {
        entry(field, 0, &h, sizeof(h));
        if (owned) retain(h);
    }
    void handle_offset(u16 field, void* h, u64 offset, u32 a = 0) {
        const struct { void* h; u64 off; } p{h, offset};
        entry(field, a, &p, sizeof(p));
        retain(h);
    }
};

/// The entries of @p cur that differ from @p base, which is the stream's
/// previous draw (or default_draw() for a keyframe). Push blocks are not
/// written here: they are interned under the queue's lock first.
void write_delta_fields(DeltaWriter& w, const CsDraw& cur, const CsDraw& base) {
    if (cur.pipeline != base.pipeline) w.handle(kFPipeline, cur.pipeline, true);
    if (cur.depth_stencil != base.depth_stencil ||
        cur.stencil_reference != base.stencil_reference ||
        cur.writes_depth_stencil != base.writes_depth_stencil) {
        // Depth-stencil states live for the process (MetalPassStateCache),
        // so a draw holds no reference to them.
        const struct { void* h; u32 ref; u32 writes; } p{
            cur.depth_stencil, cur.stencil_reference,
            cur.writes_depth_stencil ? 1u : 0u};
        w.entry(kFDepth, 0, &p, sizeof(p));
    }
    if (cur.vertex_stream_count != base.vertex_stream_count) {
        w.entry(kFStreamCount, cur.vertex_stream_count, nullptr, 0);
    }
    for (u16 i = 0; i < 16; ++i) {
        if (cur.vertex_buffers[i] != base.vertex_buffers[i] ||
            cur.vertex_offsets[i] != base.vertex_offsets[i] ||
            cur.vertex_slots[i] != base.vertex_slots[i]) {
            w.handle_offset(static_cast<u16>(kFStream + i), cur.vertex_buffers[i],
                            cur.vertex_offsets[i], cur.vertex_slots[i]);
        }
    }
    if (cur.index_buffer != base.index_buffer ||
        cur.index_offset != base.index_offset ||
        cur.index_type != base.index_type || cur.indexed != base.indexed) {
        w.handle_offset(kFIndex, cur.index_buffer, cur.index_offset,
                        (cur.index_type & 0xFFFFu) |
                            (cur.indexed ? 0x10000u : 0u));
    }
    for (u16 i = 0; i < 16; ++i) {
        if (cur.vs_cbuffer[i] != base.vs_cbuffer[i] ||
            cur.vs_cbuffer_off[i] != base.vs_cbuffer_off[i]) {
            w.handle_offset(static_cast<u16>(kFVsCb + i), cur.vs_cbuffer[i], cur.vs_cbuffer_off[i]);
        }
        if (cur.ps_cbuffer[i] != base.ps_cbuffer[i] ||
            cur.ps_cbuffer_off[i] != base.ps_cbuffer_off[i]) {
            w.handle_offset(static_cast<u16>(kFPsCb + i), cur.ps_cbuffer[i], cur.ps_cbuffer_off[i]);
        }
        if (cur.textures[i] != base.textures[i]) {
            w.handle(static_cast<u16>(kFTex + i), cur.textures[i], true);
        }
        // Samplers live for the process (MetalPassStateCache): not owned.
        if (cur.samplers[i] != base.samplers[i]) {
            w.handle(static_cast<u16>(kFSamp + i), cur.samplers[i], false);
        }
        if (cur.vertex_textures[i] != base.vertex_textures[i]) {
            w.handle(static_cast<u16>(kFVTex + i), cur.vertex_textures[i], true);
        }
        if (cur.vertex_samplers[i] != base.vertex_samplers[i]) {
            w.handle(static_cast<u16>(kFVSamp + i), cur.vertex_samplers[i], false);
        }
    }
    if (std::memcmp(cur.viewport.data(), base.viewport.data(),
                    sizeof(cur.viewport)) != 0) {
        w.entry(kFViewport, 0, cur.viewport.data(), sizeof(cur.viewport));
    }
    if (std::memcmp(cur.blend_color.data(), base.blend_color.data(),
                    sizeof(cur.blend_color)) != 0) {
        w.entry(kFBlend, 0, cur.blend_color.data(), sizeof(cur.blend_color));
    }
    if (cur.scissor_x != base.scissor_x || cur.scissor_y != base.scissor_y ||
        cur.scissor_w != base.scissor_w || cur.scissor_h != base.scissor_h ||
        cur.scissor != base.scissor) {
        const std::array<u32, 5> p{cur.scissor_x, cur.scissor_y, cur.scissor_w,
                                   cur.scissor_h, cur.scissor ? 1u : 0u};
        w.entry(kFScissor, 0, p.data(), sizeof(p));
    }
    if (cur.cull_mode != base.cull_mode || cur.fill_mode != base.fill_mode ||
        cur.depth_clip_mode != base.depth_clip_mode ||
        cur.winding != base.winding || cur.primitive != base.primitive ||
        std::memcmp(&cur.depth_bias, &base.depth_bias, sizeof(float)) != 0 ||
        std::memcmp(&cur.depth_bias_slope_scale, &base.depth_bias_slope_scale,
                    sizeof(float)) != 0 ||
        std::memcmp(&cur.depth_bias_clamp, &base.depth_bias_clamp,
                    sizeof(float)) != 0) {
        struct {
            u32 cull, fill, clip, winding, primitive;
            float bias, slope, clamp;
        } p{cur.cull_mode,  cur.fill_mode, cur.depth_clip_mode, cur.winding,
            cur.primitive,  cur.depth_bias, cur.depth_bias_slope_scale,
            cur.depth_bias_clamp};
        w.entry(kFRaster, 0, &p, sizeof(p));
    }
    for (u16 i = 0; i < 8; ++i) {
        if (cur.ps_uav_buffer[i] != base.ps_uav_buffer[i] ||
            cur.ps_uav_buffer_off[i] != base.ps_uav_buffer_off[i]) {
            w.handle_offset(static_cast<u16>(kFUav + i), cur.ps_uav_buffer[i],
                            cur.ps_uav_buffer_off[i]);
        }
    }
    for (u16 i = 0; i < graphics::BACKEND_MAX_UAV_COUNTER_BINDINGS; ++i) {
        if (cur.ps_uav_counter[i] != base.ps_uav_counter[i] ||
            cur.ps_uav_counter_off[i] != base.ps_uav_counter_off[i]) {
            w.handle_offset(static_cast<u16>(kFCounter + i), cur.ps_uav_counter[i],
                            cur.ps_uav_counter_off[i]);
        }
    }
    if (cur.ps_statistics != base.ps_statistics ||
        cur.ps_statistics_off != base.ps_statistics_off) {
        w.handle_offset(kFStats, cur.ps_statistics, cur.ps_statistics_off);
    }
    for (u16 i = 0; i < 4; ++i) {
        if (cur.vs_descriptor_tables[i] != base.vs_descriptor_tables[i]) {
            w.handle(static_cast<u16>(kFVsTable + i), cur.vs_descriptor_tables[i], true);
        }
        if (cur.ps_descriptor_tables[i] != base.ps_descriptor_tables[i]) {
            w.handle(static_cast<u16>(kFPsTable + i), cur.ps_descriptor_tables[i], true);
        }
    }
    if (cur.resident != base.resident) {
        w.entry(kFResident, static_cast<u32>(cur.resident.size()),
                cur.resident.data(), cur.resident.size() * sizeof(void*));
        for (void* r : cur.resident) w.retain(r);
    }
    const auto same_vectors = [](const auto& a, const auto& b) {
        return a.size() == b.size() &&
               (a.empty() ||
                std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
    };
    if (!same_vectors(cur.viewports, base.viewports) ||
        !same_vectors(cur.scissors, base.scissors)) {
        // Both arrays are all 16 slots or empty (record_backend_draw).
        const u32 n = static_cast<u32>(cur.viewports.size());
        std::vector<u8> payload(n * (sizeof(cur.viewports[0]) +
                                     sizeof(std::array<u32, 4>)));
        if (n != 0) {
            std::memcpy(payload.data(), cur.viewports.data(),
                        n * sizeof(cur.viewports[0]));
            for (u32 i = 0; i < n; ++i) {
                const std::array<u32, 4> sc =
                    i < cur.scissors.size() ? cur.scissors[i]
                                            : std::array<u32, 4>{};
                std::memcpy(payload.data() + n * sizeof(cur.viewports[0]) +
                                i * sizeof(sc),
                            sc.data(), sizeof(sc));
            }
        }
        w.entry(kFViewportArray, n, payload.data(), payload.size());
    }
}

/// A push entry: @p bytes is the recording arena's interned copy.
void write_push_entry(DeltaWriter& w, u16 field, const u8* bytes, u32 size) {
    const struct { const u8* p; u64 size; } p{bytes, size};
    w.entry(field, 0, &p, sizeof(p));
}

/// Begin a DeltaDraw for @p cur: its head (patched by finish_delta) and its
/// own parameters, whose argument and patch-factor buffers it holds a
/// reference on.
void begin_delta(DeltaWriter& w, const CsDraw& cur, bool keyframe) {
    w.out.clear();
    w.entries = 0;
    DeltaHead head{};
    head.flags = keyframe ? kDeltaKeyframe : 0u;
    w.raw(&head, sizeof(head));
    DeltaParams params{};
    params.vertex_count = cur.vertex_count;
    params.index_count = cur.index_count;
    params.instance_count = cur.instance_count;
    params.first_vertex = cur.first_vertex;
    params.first_instance = cur.first_instance;
    params.base_vertex = cur.base_vertex;
    params.indirect_args = cur.indirect_args;
    params.indirect_offset = cur.indirect_offset;
    params.tess_factors = cur.tess_factors;
    params.tess_factors_offset = cur.tess_factors_offset;
    params.patch_count = cur.patch_count;
    params.patch_control_points = cur.patch_control_points;
    w.raw(&params, sizeof(params));
    w.retain(cur.indirect_args);
    w.retain(cur.tess_factors);
}

void finish_delta(DeltaWriter& w, u64 hash) {
    auto* head = reinterpret_cast<DeltaHead*>(w.out.data());
    head->bytes = static_cast<u32>(w.out.size());
    head->entries = w.entries;
    head->hash = hash;
}
}  // namespace

std::atomic<int> g_delta_mutation{0};

namespace {
/// delta-record: what one recording thread knows of the stream it records
/// into. draws[cur] is the draw being resolved; draws[cur ^ 1] is the last
/// draw this thread recorded, which is the stream's last draw state while
/// `chain` is the queue's token and this thread recorded the last delta.
struct DeltaProducer {
    std::array<CsDraw, 2> draws;
    unsigned cur{0};
    bool valid{false};
    u64 gen{0};
    u64 chain{0};
    /// The interned push blocks the last draw bound.
    std::array<const u8*, 2> push{};
    std::array<u32, 2> push_size{};
    std::vector<u8> bytes;
    std::vector<void*> retains;
};
DeltaProducer& delta_producer() {
    // Immortal: a recording thread may still record during static teardown.
    static thread_local DeltaProducer* producer = new DeltaProducer();
    return *producer;
}
}  // namespace

CsDraw& MetalSubmitQueue::delta_scratch() {
    DeltaProducer& p = delta_producer();
    CsDraw& draw = p.draws[p.cur];
    reset_draw(draw);
    return draw;
}

void MetalSubmitQueue::set_delta_mutation_for_test(
    DeltaMutation mutation) noexcept {
    g_delta_mutation.store(static_cast<int>(mutation),
                           std::memory_order_relaxed);
}

bool MetalSubmitQueue::record_draw_delta(u64 gen, u64 pass_id) {
    DeltaProducer& p = delta_producer();
    CsDraw& cur = p.draws[p.cur];
    const CsDraw& last = p.draws[p.cur ^ 1u];
    // ---- P0 state census (VITREO_D3D11_STATE_CENSUS=1, DIAGNOSTIC).
    if (g_draw_census) [[unlikely]] census_record_draw(cur);
    // ---- end P0 state census
    const auto mutation =
        static_cast<DeltaMutation>(g_delta_mutation.load(std::memory_order_relaxed));
    // A new recording starts with a keyframe: nothing in it binds anything
    // yet, and every reference the stream holds belongs to one recording.
    bool keyframe = !p.valid ||
                    (p.gen != gen && mutation != DeltaMutation::NoRecordingKeyframe);
    p.retains.clear();
    DeltaWriter w{p.bytes, &p.retains};
    const auto write = [&](bool key) {
        begin_delta(w, cur, key);
        write_delta_fields(w, cur, key ? default_draw() : last);
    };
    write(keyframe);
    const bool verify = delta_record::verify();
    const u64 hash = verify ? cs_draw_state_hash(cur) : 0;
    std::unique_lock<std::mutex> lock(m_mu);
    if (m_submit_gen + 1 != gen || !appends_in_place()) {
        // The recording this draw's stamps were taken for has been handed
        // to the encoder (another thread submitted), or the stream now needs
        // the append path. Hold the references taken until the current list
        // is done -- harmless -- and resolve again.
        auto& releases = m_recording_arena.releases;
        releases.insert(releases.end(), p.retains.begin(), p.retains.end());
        p.retains.clear();
        p.valid = false;
        ++m_delta.retries;
        return false;
    }
    // From here the draw joins this recording (or, too large for the
    // arena, record_draw records it and notes the same generation again).
    note_recorded_locked();
    materialize_end_locked();
    if (!keyframe && (p.chain != m_delta_chain || m_delta_producer != &p) &&
        mutation != DeltaMutation::NoChainKeyframe) {
        // Something else wrote draw state into the stream since this
        // thread's last delta (a full draw, a replayed command list, another
        // thread's delta): the encoder's state is not `last`.
        auto& releases = m_recording_arena.releases;
        releases.insert(releases.end(), p.retains.begin(), p.retains.end());
        p.retains.clear();
        keyframe = true;
        write(true);
    }
    // push-intern: the caller's bytes into the recording's arena. A block
    // equal to the stage's previous one shares its copy and its pointer, so
    // an unchanged block is no entry at all.
    const u8* push[2]{nullptr, nullptr};
    const u32 size[2]{cur.vs_push_size, cur.ps_push_size};
    const u8* const src[2]{cur.vs_push, cur.ps_push};
    for (int stage = 0; stage < 2; ++stage) {
        if (size[stage] == 0 || src[stage] == nullptr) continue;
        push[stage] = intern_push_locked(stage, src[stage], size[stage]);
        if (push[stage] == nullptr) {
            // Larger than an arena chunk: the draw carries its own copy, as
            // a full op. The references the delta took stay with the list.
            auto& releases = m_recording_arena.releases;
            releases.insert(releases.end(), p.retains.begin(), p.retains.end());
            p.retains.clear();
            lock.unlock();
            CsDraw full = cur;
            retain_draw(full);
            full.push_pending = true;
            record_draw(std::move(full), pass_id);
            p.valid = false;
            return true;
        }
    }
    for (int stage = 0; stage < 2; ++stage) {
        if (keyframe ? push[stage] != nullptr
                     : (push[stage] != p.push[stage] ||
                        size[stage] != p.push_size[stage])) {
            write_push_entry(w, stage == 0 ? kFVsPush : kFPsPush, push[stage],
                             push[stage] != nullptr ? size[stage] : 0u);
        }
    }
    cur.vs_push = push[0];
    cur.ps_push = push[1];
    cur.vs_push_size = push[0] != nullptr ? size[0] : 0u;
    cur.ps_push_size = push[1] != nullptr ? size[1] : 0u;
    cur.push_pending = false;
    finish_delta(w, hash);
    if (slim_record::count_tick(++m_push_draws)) {
        std::fprintf(
            stderr, "[wpr2] push-intern draws=%llu copied=%llu shared=%llu\n",
            static_cast<unsigned long long>(m_push_draws),
            static_cast<unsigned long long>(m_push_blocks_copied),
            static_cast<unsigned long long>(m_push_blocks_shared));
    }
    // pass-fold, exactly as record_draw does for a full draw.
    if (m_fold.pass_id != 0 && pass_id == m_fold.pass_id) {
        if (m_fold.ended) {
            const bool reopened = m_fold.reopened;
            if (reopened) fold_split_locked(/*end_first=*/false);
            const bool writes = draw_writes(cur);
            append_delta_locked(p.bytes.data(), p.bytes.size(), pass_id);
            if (!reopened) {
                fold_reset_locked();
            } else {
                Op& end = m_recording.emplace_back();
                end.kind = OpKind::EndPass;
                end.pass_id = pass_id;
                m_fold.ended = true;
                m_fold.wrote = writes;
            }
        } else {
            if (m_fold.reopened && fold_draw_needs_split_locked(cur)) {
                fold_split_locked(/*end_first=*/true);
            }
            if (draw_writes(cur)) m_fold.wrote = true;
            append_delta_locked(p.bytes.data(), p.bytes.size(), pass_id);
        }
    } else {
        fold_settle_locked();
        append_delta_locked(p.bytes.data(), p.bytes.size(), pass_id);
    }
    auto& releases = m_recording_arena.releases;
    releases.insert(releases.end(), p.retains.begin(), p.retains.end());
    ++m_delta.draws;
    if (keyframe) ++m_delta.keyframes;
    m_delta.bytes += p.bytes.size();
    m_delta.retains += p.retains.size();
    if (delta_record::count_tick(m_delta.draws)) {
        std::fprintf(stderr,
                     "[r3] delta-record draws=%llu keyframes=%llu runs=%llu "
                     "bytes/draw=%.0f retains/draw=%.2f retries=%llu\n",
                     static_cast<unsigned long long>(m_delta.draws),
                     static_cast<unsigned long long>(m_delta.keyframes),
                     static_cast<unsigned long long>(m_delta.runs),
                     static_cast<double>(m_delta.bytes) /
                         static_cast<double>(m_delta.draws),
                     static_cast<double>(m_delta.retains) /
                         static_cast<double>(m_delta.draws),
                     static_cast<unsigned long long>(m_delta.retries));
    }
    m_delta_producer = &p;
    p.chain = m_delta_chain;
    lock.unlock();
    p.retains.clear();
    p.push[0] = push[0];
    p.push[1] = push[1];
    p.push_size[0] = cur.vs_push_size;
    p.push_size[1] = cur.ps_push_size;
    p.gen = gen;
    p.valid = true;
    p.cur ^= 1u;
    return true;
}

} // namespace vitreo::graphics

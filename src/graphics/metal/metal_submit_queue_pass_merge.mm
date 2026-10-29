// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_pass_merge.mm
/// @brief The encoder's pass-merge rules and their diagnostics.

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

/// set_pass_merge_for_test: 0 the environment, 1 merge, 2 do not.
std::atomic<int> g_pass_merge_for_test{0};
}  // namespace

bool pass_merge_enabled() {
    const int pinned = g_pass_merge_for_test.load(std::memory_order_relaxed);
    if (pinned != 0) return pinned == 1;
    static const bool on = [] {
        const char* text = std::getenv("VITREO_METAL_PASS_MERGE");
        return text == nullptr || text[0] != '0';
    }();
    return on;
}

/// VITREO_GPU_WATCH_SPLIT=1: every pass (and dispatch) ends its command
/// buffer, so a stuck buffer names one pass's draws. No pass merges then.
bool gpu_watch_split() {
    static const bool split = std::getenv("VITREO_GPU_WATCH_SPLIT") != nullptr;
    return split;
}

/// The texture a view was made from, or @p texture itself.
void* texture_root(void* texture) {
    id<MTLTexture> t = (__bridge id<MTLTexture>)texture;
    while (t.parentTexture != nil) t = t.parentTexture;
    return (__bridge void*)t;
}

void MetalSubmitQueue::set_pass_merge_for_test(
    std::optional<bool> merge) noexcept {
    g_pass_merge_for_test.store(!merge ? 0 : *merge ? 1 : 2,
                                std::memory_order_relaxed);
}

bool MetalSubmitQueue::can_continue_pass(const CsBeginPass& next) const {
    if (!pass_merge_enabled() || m_open_pass == 0 || !m_open_pass_ended ||
        m_open_pass_wrote || m_open_begin == nullptr) {
        return false;
    }
    return descriptors_continue(*m_open_begin, next);
}

bool MetalSubmitQueue::descriptors_continue(const CsBeginPass& open,
                                            const CsBeginPass& next) {
    // Continuing is equivalent to ending and re-beginning with Load only if
    // nothing happens to the contents at the boundary: no clear, no discard,
    // no resolve.
    if (!next.preserve || next.clear_color || next.color_clear_mask != 0 ||
        next.clear_depth || next.clear_stencil || next.resolve != nullptr ||
        open.resolve != nullptr) {
        return false;
    }
    if (next.color_count != open.color_count) return false;
    for (u32 i = 0; i < next.color_count && i < next.colors.size(); ++i) {
        if (next.colors[i] != open.colors[i]) return false;
        if (next.colors[i] == nullptr) continue;
        if (next.color_levels[i] != open.color_levels[i] ||
            next.color_slices[i] != open.color_slices[i] ||
            next.color_depth_planes[i] != open.color_depth_planes[i]) {
            return false;
        }
    }
    if (next.depth != open.depth || next.stencil != open.stencil) return false;
    if (next.depth != nullptr && (next.depth_level != open.depth_level ||
                                  next.depth_slice != open.depth_slice)) {
        return false;
    }
    // The visibility buffer is part of the descriptor; the offset is encoder
    // state, set per pass below.
    return next.render_target_array_length ==
               open.render_target_array_length &&
           next.default_width == open.default_width &&
           next.default_height == open.default_height &&
           next.default_samples == open.default_samples &&
           next.center_samples == open.center_samples &&
           next.visibility_buffer == open.visibility_buffer &&
           next.visibility_accumulate == open.visibility_accumulate;
}

const char* MetalSubmitQueue::pass_break_reason(const CsBeginPass& next) const {
    if (!pass_merge_enabled()) return "merge-disabled";
    if (!m_open_pass_ended) return "open-pass-not-ended";
    if (m_open_pass_wrote) return "open-pass-wrote-memory";
    if (m_open_begin == nullptr) return "no-open-begin";
    const CsBeginPass& open = *m_open_begin;
    if (!next.preserve) return "next-discards";
    if (next.clear_color || next.color_clear_mask != 0 || next.clear_depth ||
        next.clear_stencil) {
        return "next-clears";
    }
    if (next.resolve != nullptr || open.resolve != nullptr) return "resolve";
    if (next.color_count != open.color_count) return "color-count";
    for (u32 i = 0; i < next.color_count && i < next.colors.size(); ++i) {
        if (next.colors[i] != open.colors[i]) return "color-target";
        if (next.colors[i] == nullptr) continue;
        if (next.color_levels[i] != open.color_levels[i] ||
            next.color_slices[i] != open.color_slices[i] ||
            next.color_depth_planes[i] != open.color_depth_planes[i]) {
            return "color-subresource";
        }
    }
    if (next.depth != open.depth) return "depth-target";
    if (next.stencil != open.stencil) return "stencil-attachment";
    if (next.depth != nullptr && (next.depth_level != open.depth_level ||
                                  next.depth_slice != open.depth_slice)) {
        return "depth-subresource";
    }
    if (next.render_target_array_length != open.render_target_array_length) {
        return "array-length";
    }
    if (next.default_width != open.default_width ||
        next.default_height != open.default_height ||
        next.default_samples != open.default_samples) {
        return "attachmentless-extent";
    }
    if (next.center_samples != open.center_samples) return "center-samples";
    if (next.visibility_buffer != open.visibility_buffer) {
        return (next.visibility_buffer == nullptr) !=
                       (open.visibility_buffer == nullptr)
                   ? "visibility-on-off"
                   : "visibility-buffer";
    }
    if (next.visibility_accumulate != open.visibility_accumulate) {
        return "visibility-mode";
    }
    return "continuable";
}

const char* MetalSubmitQueue::attachment_read_label(const CsDraw& draw) const {
    // DIAGNOSTIC: which binding of @p draw matched an attachment of the open
    // pass (the first one draw_needs_own_encoder would find), whether that
    // attachment is the depth or a colour target, and whether the draw names
    // the attachment's own view or another view of the same texture.
    if (m_open_begin == nullptr) return "draw-restart:reads-attachment";
    const CsBeginPass& open = *m_open_begin;
    const auto classify = [&](void* texture) -> int {
        if (texture == nullptr) return 0;
        void* const root = texture_root(texture);
        const auto match = [&](void* attachment) -> int {
            if (attachment == nullptr) return 0;
            if (attachment == texture) return 1;
            return texture_root(attachment) == root ? 2 : 0;
        };
        if (const int m = match(open.depth)) return m;               // depth
        for (u32 i = 0; i < open.color_count && i < open.colors.size(); ++i) {
            if (const int m = match(open.colors[i])) return m + 2;   // colour
        }
        return 0;
    };
    static const char* const kLabels[3][5] = {
        {"", "draw-restart:slot/depth/same-view",
         "draw-restart:slot/depth/other-view",
         "draw-restart:slot/color/same-view",
         "draw-restart:slot/color/other-view"},
        {"", "draw-restart:vs-slot/depth/same-view",
         "draw-restart:vs-slot/depth/other-view",
         "draw-restart:vs-slot/color/same-view",
         "draw-restart:vs-slot/color/other-view"},
        {"", "draw-restart:table/depth/same-view",
         "draw-restart:table/depth/other-view",
         "draw-restart:table/color/same-view",
         "draw-restart:table/color/other-view"},
    };
    for (usize i = 0; i < graphics::BACKEND_UAV_TEXTURE_BASE &&
                      i < draw.textures.size();
         ++i) {
        if (const int k = classify(draw.textures[i])) return kLabels[0][k];
    }
    for (void* texture : draw.vertex_textures) {
        if (const int k = classify(texture)) return kLabels[1][k];
    }
    for (void* resource : draw.resident) {
        if (resource == nullptr ||
            ![(__bridge id)resource respondsToSelector:@selector(parentTexture)]) {
            continue;
        }
        if (const int k = classify(resource)) return kLabels[2][k];
    }
    return "draw-restart:reads-attachment";
}

void MetalSubmitQueue::note_pass_break(const Op& op) {
    // DIAGNOSTIC, worker thread only. The open encoder (m_open_pass) is about
    // to be closed by `op`: a BeginPass that cannot continue it, or any op
    // other than a draw, an EndPass or a debug marker.
    if (m_open_pass == 0) return;
    const char* reason = nullptr;
    switch (op.kind) {
    case OpKind::Draw:
        if (op.draw && op.pass_id == m_open_pass && m_open_pass_merged &&
            draw_needs_own_encoder(*op.draw)) {
            reason = draw_writes(*op.draw) ? "draw-restart:writes-memory"
                                           : attachment_read_label(*op.draw);
            break;
        }
        return;
    case OpKind::EndPass:
    case OpKind::DebugMarker:
    // A DrawRun is never recorded under this diagnostic (delta_record.hpp).
    case OpKind::DrawRun:
        return;
    case OpKind::BeginPass:
        if (!op.begin || can_continue_pass(*op.begin)) return;
        reason = pass_break_reason(*op.begin);
        break;
    case OpKind::Resolve: reason = "op:Resolve"; break;
    case OpKind::BlitRegion: reason = "op:BlitRegion"; break;
    case OpKind::CopyBuffer: reason = "op:CopyBuffer"; break;
    case OpKind::FillBuffer: reason = "op:FillBuffer"; break;
    case OpKind::UploadTexture: reason = "op:UploadTexture"; break;
    case OpKind::DownloadTexture: reason = "op:DownloadTexture"; break;
    case OpKind::GenerateMips: reason = "op:GenerateMips"; break;
    case OpKind::Compute: reason = "op:Compute"; break;
    case OpKind::Present: reason = "op:Present"; break;
    case OpKind::SignalFence: reason = "op:SignalFence"; break;
    case OpKind::WaitFence: reason = "op:WaitFence"; break;
    case OpKind::Timestamp: reason = "op:Timestamp"; break;
    case OpKind::TileMapping: reason = "op:TileMapping"; break;
    case OpKind::TiledBarrier: reason = "op:TiledBarrier"; break;
    case OpKind::Exit: reason = "op:Exit"; break;
    }
    if (reason == nullptr) return;
    static std::vector<std::pair<const char*, u64>> counts;
    // The same reasons, for the breaks of a pass with four or more colour
    // targets (a G-buffer): each such break stores and reloads every target.
    static std::vector<std::pair<const char*, u64>> mrt_counts;
    static auto last = std::chrono::steady_clock::now();
    const auto bump = [](std::vector<std::pair<const char*, u64>>& table,
                         const char* label) {
        for (auto& [known, n] : table) {
            if (known == label) {
                ++n;
                return;
            }
        }
        table.emplace_back(label, 1);
    };
    bump(counts, reason);
    if (m_open_begin != nullptr && m_open_begin->color_count >= 4) {
        bump(mrt_counts, reason);
    }
    const auto now = std::chrono::steady_clock::now();
    const double since = std::chrono::duration<double>(now - last).count();
    if (since < 5.0) return;
    std::sort(counts.begin(), counts.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::string line;
    for (const auto& [label, n] : counts) {
        char item[96];
        std::snprintf(item, sizeof(item), " %s=%.0f/s", label,
                      static_cast<double>(n) / since);
        line += item;
    }
    std::fprintf(stderr, "[metal-pass-break]%s\n", line.c_str());
    std::sort(mrt_counts.begin(), mrt_counts.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    line.clear();
    for (const auto& [label, n] : mrt_counts) {
        char item[96];
        std::snprintf(item, sizeof(item), " %s=%.0f/s", label,
                      static_cast<double>(n) / since);
        line += item;
    }
    std::fprintf(stderr, "[metal-pass-break-mrt]%s\n", line.c_str());
    mrt_counts.clear();
    counts.clear();
    last = now;
}

bool MetalSubmitQueue::draw_needs_own_encoder(const CsDraw& draw) const {
    if (draw_writes(draw)) return true;
    // Reading the depth attachment's texture is ordered correctly without a
    // boundary while nothing in this encoder has changed the attachment
    // (m_open_depth_dirty); a colour attachment always needs one.
    void* const depth = m_open_begin != nullptr ? m_open_begin->depth : nullptr;
    void* const depth_root = depth != nullptr ? texture_root(depth) : nullptr;
    const auto attached = [&](void* texture) {
        if (texture == nullptr) return false;
        void* const root = texture_root(texture);
        if (depth != nullptr && (texture == depth || root == depth_root)) {
            return m_open_depth_dirty;
        }
        for (u32 i = 0; i < m_open_attachment_root_count; ++i) {
            if (m_open_attachment_roots[i] == texture ||
                m_open_attachment_roots[i] == root) {
                return true;
            }
        }
        return false;
    };
    for (usize i = 0; i < graphics::BACKEND_UAV_TEXTURE_BASE &&
                      i < draw.textures.size();
         ++i) {
        if (attached(draw.textures[i])) return true;
    }
    for (void* texture : draw.vertex_textures) {
        if (attached(texture)) return true;
    }
    // What a descriptor table reaches, which is where a table-mode shader's
    // t# textures are. The list also holds buffers, which have no parent.
    for (void* resource : draw.resident) {
        if (resource == nullptr) continue;
        if (![(__bridge id)resource respondsToSelector:@selector(parentTexture)]) {
            continue;
        }
        if (attached(resource)) return true;
    }
    return false;
}

bool MetalSubmitQueue::fold_draw_needs_split_locked(const CsDraw& draw) const {
    // draw_needs_own_encoder for a draw in an encoder that holds more than
    // one pass, without the encoder's state: a writing draw, or any read of
    // an attachment or of the texture it is a view of. A depth read counts
    // whether or not the depth is dirty, which only the encoder knows; the
    // split hands that decision back to it.
    if (draw_writes(draw)) return true;
    const auto attached = [&](void* texture) {
        if (texture == nullptr) return false;
        for (u32 i = 0; i < m_fold.root_count; ++i) {
            if (m_fold.roots[i] == texture) return true;
        }
        void* const root = texture_root(texture);
        for (u32 i = 0; i < m_fold.root_count; ++i) {
            if (m_fold.roots[i] == root) return true;
        }
        return false;
    };
    for (usize i = 0; i < graphics::BACKEND_UAV_TEXTURE_BASE &&
                      i < draw.textures.size();
         ++i) {
        if (attached(draw.textures[i])) return true;
    }
    for (void* texture : draw.vertex_textures) {
        if (attached(texture)) return true;
    }
    for (void* resource : draw.resident) {
        if (resource == nullptr) continue;
        if (![(__bridge id)resource respondsToSelector:@selector(parentTexture)]) {
            continue;
        }
        if (attached(resource)) return true;
    }
    return false;
}

} // namespace vitreo::graphics

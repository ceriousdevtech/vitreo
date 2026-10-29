// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_encode_pass.mm
/// @brief Render-pass encoding on the worker: begin, close, reopen and the
///        passes live across lists.

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

void MetalSubmitQueue::encode_begin_pass(const CsBeginPass& pass) {
    @autoreleasepool {
        if (!m_recorder) {
            m_recorder = std::make_unique<MetalCommandRecorder>(
                MetalDevice::instance().command_queue_ptr());
        }
        // begin_render_pass encodes onto the current command buffer. Without
        // this the buffer is nil, the clear encoder is nil, and readback
        // sees uninitialized magenta.
        m_recorder->ensure_begun();
        MTLRenderPassDescriptor* desc =
            [MTLRenderPassDescriptor renderPassDescriptor];
        for (u32 i = 0; i < pass.color_count && i < pass.colors.size(); ++i) {
            if (pass.colors[i] == nullptr) {
                continue;
            }
            desc.colorAttachments[i].texture =
                (__bridge id<MTLTexture>)pass.colors[i];
            desc.colorAttachments[i].level = pass.color_levels[i];
            desc.colorAttachments[i].slice = pass.color_slices[i];
            desc.colorAttachments[i].depthPlane = pass.color_depth_planes[i];
            if (pass.clear_color) {
                desc.colorAttachments[i].loadAction = MTLLoadActionClear;
                desc.colorAttachments[i].clearColor = MTLClearColorMake(
                    pass.color_clear[0], pass.color_clear[1],
                    pass.color_clear[2], pass.color_clear[3]);
            } else if ((pass.color_clear_mask >> i) & 1u) {
                const std::array<float, 4>& c = pass.color_clears[i];
                desc.colorAttachments[i].loadAction = MTLLoadActionClear;
                desc.colorAttachments[i].clearColor =
                    MTLClearColorMake(c[0], c[1], c[2], c[3]);
            } else {
                desc.colorAttachments[i].loadAction =
                    pass.preserve ? MTLLoadActionLoad : MTLLoadActionDontCare;
            }
            if (i == 0 && pass.resolve != nullptr) {
                desc.colorAttachments[i].resolveTexture =
                    (__bridge id<MTLTexture>)pass.resolve;
                desc.colorAttachments[i].storeAction =
                    MTLStoreActionStoreAndMultisampleResolve;
            } else {
                desc.colorAttachments[i].storeAction = MTLStoreActionStore;
            }
        }
        if (pass.render_target_array_length > 1) {
            desc.renderTargetArrayLength = pass.render_target_array_length;
        }
        if (pass.color_count == 0 && pass.depth == nullptr &&
            pass.default_width != 0) {
            desc.renderTargetWidth = pass.default_width;
            desc.renderTargetHeight = pass.default_height;
            desc.defaultRasterSampleCount = pass.default_samples;
        }
        if (pass.center_samples) {
            // One position per sample of the attachments, all at the pixel
            // centre: coverage is then the centre's for every sample.
            id<MTLTexture> first = nil;
            for (u32 i = 0; i < pass.color_count && i < pass.colors.size() &&
                            first == nil;
                 ++i) {
                first = (__bridge id<MTLTexture>)pass.colors[i];
            }
            if (first == nil) first = (__bridge id<MTLTexture>)pass.depth;
            const NSUInteger samples = first != nil ? first.sampleCount : 1;
            if (samples > 1 && samples <= 16) {
                MTLSamplePosition positions[16];
                for (NSUInteger i = 0; i < samples; ++i) {
                    positions[i] = MTLSamplePositionMake(0.5f, 0.5f);
                }
                [desc setSamplePositions:positions count:samples];
            }
        }
        if (pass.depth != nullptr) {
            desc.depthAttachment.texture = (__bridge id<MTLTexture>)pass.depth;
            desc.depthAttachment.level = pass.depth_level;
            desc.depthAttachment.slice = pass.depth_slice;
            desc.depthAttachment.loadAction =
                pass.clear_depth ? MTLLoadActionClear : MTLLoadActionLoad;
            desc.depthAttachment.storeAction = MTLStoreActionStore;
            desc.depthAttachment.clearDepth = pass.depth_clear;
            if (pass.stencil) {
                desc.stencilAttachment.texture =
                    (__bridge id<MTLTexture>)pass.depth;
                desc.stencilAttachment.level = pass.depth_level;
                desc.stencilAttachment.slice = pass.depth_slice;
                desc.stencilAttachment.loadAction =
                    pass.clear_stencil ? MTLLoadActionClear
                                       : MTLLoadActionLoad;
                desc.stencilAttachment.storeAction = MTLStoreActionStore;
                desc.stencilAttachment.clearStencil = pass.stencil_clear;
            }
        }
        if (pass.visibility_buffer != nullptr) {
            // D3D11 OCCLUSION queries: the draws of this pass count the
            // samples that pass depth and stencil into the slot at
            // visibility_offset. Accumulate adds to what earlier passes of
            // the same interval left there; the default (Reset) replaces
            // only the offsets this pass uses, so the macOS 13 path gives
            // every pass a slot of its own.
            desc.visibilityResultBuffer =
                (__bridge id<MTLBuffer>)pass.visibility_buffer;
            if (pass.visibility_accumulate) {
                if (@available(macOS 14.0, *)) {
                    desc.visibilityResultType =
                        MTLVisibilityResultTypeAccumulate;
                }
            }
        }
        m_recorder->begin_render_pass((__bridge void*)desc);
        if (!m_recorder->has_render_encoder()) {
            // Metal gave no encoder: every draw of this pass is dropped (and,
            // with pass-fold, the draws of passes folded into it). Visible
            // otherwise only under VITREO_TRACE ("[submit] draw dropped").
            static std::atomic<bool> warned{false};
            if (!warned.exchange(true, std::memory_order_relaxed)) {
                std::fprintf(stderr,
                             "[metal] WARNING: render encoder creation failed "
                             "(%u colour attachment(s), depth %s): the pass's "
                             "draws are dropped; reported once\n",
                             pass.color_count,
                             pass.depth != nullptr ? "yes" : "no");
            }
        }
        if (pass.visibility_buffer != nullptr) {
            m_recorder->set_visibility_counting(pass.visibility_offset);
        }
    }
}

void MetalSubmitQueue::close_open_pass() {
    if (m_recorder) {
        m_recorder->end_render_pass();
    }
    const bool was_open = m_open_pass != 0;
    m_open_pass = 0;
    m_open_pass_ended = false;
    m_open_pass_merged = false;
    m_open_pass_wrote = false;
    m_open_depth_dirty = false;
    m_open_begin = nullptr;
    m_open_attachment_root_count = 0;
    if (m_open_begin_owned) [[unlikely]] {
        release_begin(*m_open_begin_owned);
        m_open_begin_owned.reset();
    }
    if (was_open) {
        note_encoder_closed();
    }
}

void MetalSubmitQueue::note_live_begin(u64 pass_id, const CsBeginPass& begin) {
    // A pass split at record time (fold_split_locked) is begun again under
    // its own id: the newer descriptor is the one its later draws continue.
    for (LivePass& live : m_live_passes) {
        if (live.pass_id != pass_id) continue;
        if (live.owned) {
            release_begin(*live.owned);
            live.owned.reset();
        }
        live.begin = &begin;
        return;
    }
    m_live_passes.push_back(LivePass{pass_id, &begin, nullptr});
}

void MetalSubmitQueue::note_live_end(u64 pass_id) {
    for (usize i = 0; i < m_live_passes.size(); ++i) {
        if (m_live_passes[i].pass_id != pass_id) continue;
        if (m_live_passes[i].owned) release_begin(*m_live_passes[i].owned);
        m_live_passes[i] = std::move(m_live_passes.back());
        m_live_passes.pop_back();
        return;
    }
}

void MetalSubmitQueue::carry_live_passes() {
    // Every BeginPass is followed by its EndPass (a pass object records it at
    // end() or destruction), so a pass live at the end of a list is one
    // whose EndPass is still to be recorded or submitted. Bound the set all
    // the same: a recorder that never ended a pass must not pin its
    // attachments for the life of the process.
    constexpr usize kMaxLivePasses = 64;
    while (m_live_passes.size() > kMaxLivePasses) {
        static std::atomic<bool> told{false};
        if (!told.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[metal] WARNING: more than %zu render passes begun "
                         "and never ended; the oldest is forgotten (a later "
                         "draw of it is dropped); reported once\n",
                         kMaxLivePasses);
        }
        if (m_live_passes.front().owned) {
            release_begin(*m_live_passes.front().owned);
        }
        m_live_passes.erase(m_live_passes.begin());
    }
    for (LivePass& live : m_live_passes) {
        if (live.owned) continue;
        live.owned = std::make_unique<CsBeginPass>(*live.begin);
        retain_begin(*live.owned);
        live.begin = nullptr;
    }
}

bool MetalSubmitQueue::reopen_pass(u64 pass_id, const CsBeginPass* fallback) {
    const CsBeginPass* source = fallback;
    for (const LivePass& live : m_live_passes) {
        if (live.pass_id == pass_id) {
            source = live.owned ? live.owned.get() : live.begin;
            break;
        }
    }
    if (source == nullptr || !m_recorder) return false;
    // The pass's earlier encoder ran its clears and stored its attachments:
    // this one loads them (the descriptors_continue condition, made true).
    // Copied before the open encoder is closed: @p fallback can be the
    // descriptor that encoder owns.
    auto again = std::make_unique<CsBeginPass>(*source);
    again->preserve = true;
    again->clear_color = false;
    again->color_clear_mask = 0;
    again->clear_depth = false;
    again->clear_stencil = false;
    retain_begin(*again);
    close_open_pass();
    if (again->visibility_buffer != nullptr && !again->visibility_accumulate) {
        // Reset visibility: count into a carry slot, added into the pass's
        // own slot once this list has completed (list_completed).
        @autoreleasepool {
            id<MTLDevice> device =
                (__bridge id<MTLDevice>)MetalDevice::instance().device_ptr();
            id<MTLBuffer> carry =
                device != nil
                    ? [device newBufferWithLength:sizeof(u64)
                                          options:MTLResourceStorageModeShared]
                    : nil;
            if (carry != nil) {
                std::memset([carry contents], 0, sizeof(u64));
                carry.label = @"vitreo.visibility.carry";
                VisibilityCarry entry{};
                entry.carry = (__bridge_retained void*)carry;
                entry.target = again->visibility_buffer;
                entry.offset = again->visibility_offset;
                retain(entry.target);
                m_list_carries.push_back(entry);
                release(again->visibility_buffer);
                again->visibility_buffer = entry.carry;
                again->visibility_offset = 0;
                retain(again->visibility_buffer);
            } else {
                static std::atomic<bool> told{false};
                if (!told.exchange(true, std::memory_order_relaxed)) {
                    std::fprintf(stderr,
                                 "[vitreo] UNIMPLEMENTED d3d11!GetData: "
                                 "OCCLUSION count of a render pass reopened "
                                 "in a second encoder without accumulating "
                                 "visibility (no carry slot): the count "
                                 "before the reopen is lost\n");
                }
            }
        }
    }
    encode_begin_pass(*again);
    if (!m_recorder->has_render_encoder()) {
        release_begin(*again);
        return false;
    }
    m_open_pass = pass_id;
    m_open_begin_owned = std::move(again);
    m_open_begin = m_open_begin_owned.get();
    // Loaded: the attachments hold what memory holds.
    m_open_depth_dirty = false;
    return true;
}

void MetalSubmitQueue::list_completed(u64 gen) {
    if (m_carry_lists.load(std::memory_order_acquire) != 0) [[unlikely]] {
        std::vector<VisibilityCarry> done;
        {
            std::lock_guard<std::mutex> lock(m_carry_mu);
            for (usize i = 0; i < m_carries.size();) {
                if (m_carries[i].first > gen) {
                    ++i;
                    continue;
                }
                done.insert(done.end(), m_carries[i].second.begin(),
                            m_carries[i].second.end());
                m_carries.erase(m_carries.begin() +
                                static_cast<std::ptrdiff_t>(i));
                m_carry_lists.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        // Every command buffer of the list has completed (one queue retires
        // in order, and this is the list's last): both slots are final.
        for (const VisibilityCarry& entry : done) {
            id<MTLBuffer> carry = (__bridge id<MTLBuffer>)entry.carry;
            id<MTLBuffer> target = (__bridge id<MTLBuffer>)entry.target;
            u64 add = 0;
            u64 value = 0;
            std::memcpy(&add, [carry contents], sizeof(add));
            u8* const slot = static_cast<u8*>([target contents]) + entry.offset;
            std::memcpy(&value, slot, sizeof(value));
            value += add;
            std::memcpy(slot, &value, sizeof(value));
            release(entry.carry);
            release(entry.target);
        }
    }
    signal_gpu_gen(gen);
}

} // namespace vitreo::graphics

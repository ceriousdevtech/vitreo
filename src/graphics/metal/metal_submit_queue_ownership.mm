// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_submit_queue_ownership.mm
/// @brief Op reference ownership: retain and release of the handles an
///        op holds.

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

void MetalSubmitQueue::retain(void* object) {
    if (object != nullptr) {
        CFRetain(object);
    }
}

void MetalSubmitQueue::release(void* object) {
    if (object != nullptr) {
        CFRelease(object);
    }
}

// A draw's handles are all id<MTL*> (retain_cs_handle): release and retain
// them through the ObjC runtime directly rather than CFRelease/CFRetain.
void release_draw_handle(void* object) {
    if (object != nullptr) (void)CFBridgingRelease(object);
}
void retain_draw_handle(void* object) {
    if (object != nullptr) (void)CFBridgingRetain((__bridge id)object);
}

void MetalSubmitQueue::release_draw(CsDraw& draw) {
    release_draw_handle(draw.pipeline);
    release_draw_handle(draw.depth_stencil);
    for (void* buffer : draw.vertex_buffers) {
        release_draw_handle(buffer);
    }
    release_draw_handle(draw.index_buffer);
    for (void* buffer : draw.vs_cbuffer) {
        release_draw_handle(buffer);
    }
    for (void* buffer : draw.ps_cbuffer) {
        release_draw_handle(buffer);
    }
    for (void* texture : draw.textures) {
        release_draw_handle(texture);
    }
    // draw.samplers / draw.vertex_samplers: not owned (see retain_draw).
    for (void* texture : draw.vertex_textures) {
        release_draw_handle(texture);
    }
    release_draw_handle(draw.indirect_args);
    for (void* buffer : draw.ps_uav_buffer) {
        release_draw_handle(buffer);
    }
    for (void* buffer : draw.ps_uav_counter) {
        release_draw_handle(buffer);
    }
    release_draw_handle(draw.ps_statistics);
    for (void* table : draw.vs_descriptor_tables) {
        release_draw_handle(table);
    }
    for (void* table : draw.ps_descriptor_tables) {
        release_draw_handle(table);
    }
    for (void* resource : draw.resident) {
        release_draw_handle(resource);
    }
}

void MetalSubmitQueue::release_begin(CsBeginPass& pass) {
    for (void* color : pass.colors) {
        release(color);
    }
    release(pass.depth);
    release(pass.resolve);
    release(pass.visibility_buffer);
}

void MetalSubmitQueue::release_compute(CsCompute& compute) {
    release(compute.pipeline);
    for (void* buffer : compute.buffers) {
        release(buffer);
    }
    for (void* texture : compute.textures) {
        release(texture);
    }
    for (void* sampler : compute.samplers) {
        release(sampler);
    }
    release(compute.indirect_args);
    for (void* table : compute.descriptor_tables) {
        release(table);
    }
    for (void* resource : compute.resident) {
        release(resource);
    }
}

void MetalSubmitQueue::release_op(Op& op) {
    release(op.fence_event);
    if (op.begin) release_begin(*op.begin);
    release(op.blit_src);
    release(op.blit_dst);
    release(op.present_src);
    if (op.draw) release_draw(*op.draw);
    if (op.compute) release_compute(*op.compute);
}

// Mirrors release_draw field for field. A missing retain here means the first
// submission of a twice-submitted list frees something the second still uses.
void MetalSubmitQueue::retain_draw(CsDraw& draw) {
    retain_draw_handle(draw.pipeline);
    retain_draw_handle(draw.depth_stencil);
    for (void* buffer : draw.vertex_buffers) {
        retain_draw_handle(buffer);
    }
    retain_draw_handle(draw.index_buffer);
    for (void* buffer : draw.vs_cbuffer) {
        retain_draw_handle(buffer);
    }
    for (void* buffer : draw.ps_cbuffer) {
        retain_draw_handle(buffer);
    }
    for (void* texture : draw.textures) {
        retain_draw_handle(texture);
    }
    // draw.samplers / draw.vertex_samplers: not owned. Every CsDraw sampler
    // comes from MetalPassStateCache's process-lifetime store.
    for (void* texture : draw.vertex_textures) {
        retain_draw_handle(texture);
    }
    retain_draw_handle(draw.indirect_args);
    for (void* buffer : draw.ps_uav_buffer) {
        retain_draw_handle(buffer);
    }
    for (void* buffer : draw.ps_uav_counter) {
        retain_draw_handle(buffer);
    }
    retain_draw_handle(draw.ps_statistics);
    for (void* table : draw.vs_descriptor_tables) {
        retain_draw_handle(table);
    }
    for (void* table : draw.ps_descriptor_tables) {
        retain_draw_handle(table);
    }
    for (void* resource : draw.resident) {
        retain_draw_handle(resource);
    }
}

void MetalSubmitQueue::retain_begin(CsBeginPass& pass) {
    for (void* color : pass.colors) {
        retain(color);
    }
    retain(pass.depth);
    retain(pass.resolve);
    retain(pass.visibility_buffer);
}

void MetalSubmitQueue::retain_compute(CsCompute& compute) {
    retain(compute.pipeline);
    for (void* buffer : compute.buffers) {
        retain(buffer);
    }
    for (void* texture : compute.textures) {
        retain(texture);
    }
    for (void* sampler : compute.samplers) {
        retain(sampler);
    }
    retain(compute.indirect_args);
    for (void* table : compute.descriptor_tables) {
        retain(table);
    }
    for (void* resource : compute.resident) {
        retain(resource);
    }
}

void MetalSubmitQueue::retain_op(Op& op) {
    retain(op.fence_event);
    if (op.begin) retain_begin(*op.begin);
    retain(op.blit_src);
    retain(op.blit_dst);
    retain(op.present_src);
    if (op.draw) retain_draw(*op.draw);
    if (op.compute) retain_compute(*op.compute);
}

} // namespace vitreo::graphics

// SPDX-License-Identifier: LGPL-2.1-only
#import <Metal/Metal.h>
#import <CoreFoundation/CoreFoundation.h>
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/metal/metal_gpu_watch.hpp"

#include <cstdio>
#include "metal_command.hpp"

#include "metal_submit_queue.hpp"

#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace vitreo::graphics {

MetalCommandRecorder::MetalCommandRecorder(void* command_queue)
    : m_queue(command_queue) {
    VITREO_TRACE("[command] recorder created queue=%p\n", command_queue);
}

MetalCommandRecorder::~MetalCommandRecorder() {
    release_constant_slabs();
    if (m_disabled_depth_stencil) {
        CFRelease(m_disabled_depth_stencil);
        m_disabled_depth_stencil = nullptr;
    }
    if (m_dummy_black_texture) {
        CFRelease(m_dummy_black_texture);
        m_dummy_black_texture = nullptr;
    }
    if (m_default_sampler) {
        CFRelease(m_default_sampler);
        m_default_sampler = nullptr;
    }
    VITREO_TRACE("[command] recorder destroyed\n");
}

// =============================================================================
// Command buffer lifecycle
// =============================================================================

void MetalCommandRecorder::begin() {
    if (m_command_buffer != nullptr) return;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_queue;
    id<MTLCommandBuffer> buf = [queue commandBuffer];
    m_command_buffer = (__bridge_retained void*)buf;
    // Do not recycle constant slabs here. Parks commit a command buffer every
    // few hundred render passes to bound encoder size; resetting the 3-slot
    // ring on each of those begins overwrites bone/UV constants the GPU is
    // still reading. Recycle once per Present instead.
    static u32 begin_trace = 0;
    ++begin_trace;
    if (begin_trace <= 8 || (begin_trace & 0x3FFu) == 0) {
        VITREO_TRACE("[command] begin cmd_buf=%p #%u\n", m_command_buffer,
                     begin_trace);
    }
}

void MetalCommandRecorder::ensure_begun() { begin(); }

// =============================================================================
// Render pass
// =============================================================================

void MetalCommandRecorder::begin_render_pass(void* descriptor) {
    end_active_encoder();
    id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)m_command_buffer;
    MTLRenderPassDescriptor* desc = (__bridge MTLRenderPassDescriptor*)descriptor;
    if (gpu_watch::enabled()) {
        char pass[64];
        std::snprintf(pass, sizeof(pass), "pass%s%lux%lu",
                      desc.colorAttachments[0].texture == nil &&
                              desc.depthAttachment.texture == nil
                          ? "(no-attachment)"
                          : "",
                      static_cast<unsigned long>(
                          desc.colorAttachments[0].texture != nil
                              ? desc.colorAttachments[0].texture.width
                              : desc.renderTargetWidth),
                      static_cast<unsigned long>(
                          desc.colorAttachments[0].texture != nil
                              ? desc.colorAttachments[0].texture.height
                              : desc.renderTargetHeight));
        gpu_watch::note(m_cb_summary, pass);
    }
    id<MTLRenderCommandEncoder> enc = nil;
    if (gpu_profile::enabled()) {
        enc = (__bridge_transfer id<MTLRenderCommandEncoder>)
            gpu_profile::begin_render(m_profile, m_queue, m_command_buffer,
                                      descriptor);
    }
    if (enc == nil) enc = [buf renderCommandEncoderWithDescriptor:desc];
    if (enc == nil) {
        m_render_encoder = nullptr;
        return;
    }
    m_render_encoder = (__bridge_retained void*)enc;
    open_debug_groups(m_render_encoder);
    reset_render_state_cache();
    static u32 pass_trace = 0;
    ++pass_trace;
    if (pass_trace <= 8 || (pass_trace & 0x3FFu) == 0) {
        VITREO_TRACE("[command] begin_render_pass enc=%p #%u\n",
                     m_render_encoder, pass_trace);
    }
}

void MetalCommandRecorder::set_visibility_counting(usize offset) {
    if (m_render_encoder == nullptr) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setVisibilityResultMode:MTLVisibilityResultModeCounting
                          offset:offset];
}

void MetalCommandRecorder::end_render_pass() {
    if (m_render_encoder) {
        close_debug_groups(m_render_encoder);
        id<MTLRenderCommandEncoder> enc =
            (__bridge_transfer id<MTLRenderCommandEncoder>)m_render_encoder;
        [enc endEncoding];
        m_render_encoder = nullptr;
        // Drop the bound-state cache with the encoder. The cache describes
        // what THIS encoder had bound; leaving it set means the next pass can
        // early-return out of a setter it actually needed, and -- worse --
        // makes a stale-encoder bug present as a skip rather than a fault.
        reset_render_state_cache();
        VITREO_TRACE("[command] end_render_pass\n");
    }
}

// =============================================================================
// Render state
// =============================================================================

void MetalCommandRecorder::set_render_pipeline(void* pso) {
    if (m_render_encoder == nullptr) return;
    if (m_render_pipeline_bound && m_render_pipeline == pso) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLRenderPipelineState> state =
        (__bridge id<MTLRenderPipelineState>)pso;
    [enc setRenderPipelineState:state];
    m_render_pipeline = pso;
    if (m_profile) gpu_profile::note_pipeline(m_profile.get(), state.label.UTF8String);
    if (gpu_watch::enabled()) {
        gpu_watch::note(m_cb_summary, state.label.UTF8String);
    }
    m_render_pipeline_bound = true;
}

void MetalCommandRecorder::set_depth_stencil(void* state) {
    if (m_render_encoder == nullptr) return;
    if (state == nullptr) {
        state = disabled_depth_stencil();
        if (state == nullptr) return;
    }
    if (m_depth_stencil_bound && m_depth_stencil == state) return;
    VITREO_TRACE("[command] set_depth_stencil enc=%p state=%p\n",
                 m_render_encoder, state);
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLDepthStencilState> ds = (__bridge id<MTLDepthStencilState>)state;
    [enc setDepthStencilState:ds];
    m_depth_stencil = state;
    m_depth_stencil_bound = true;
}

void* MetalCommandRecorder::disabled_depth_stencil() {
    if (m_disabled_depth_stencil != nullptr) {
        return m_disabled_depth_stencil;
    }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_queue;
    if (queue == nil || queue.device == nil) {
        return nullptr;
    }
    MTLDepthStencilDescriptor* desc = [[MTLDepthStencilDescriptor alloc] init];
    desc.depthCompareFunction = MTLCompareFunctionAlways;
    desc.depthWriteEnabled = NO;
    id<MTLDepthStencilState> ds =
        [queue.device newDepthStencilStateWithDescriptor:desc];
    if (ds == nil) {
        return nullptr;
    }
    m_disabled_depth_stencil = (__bridge_retained void*)ds;
    return m_disabled_depth_stencil;
}

void* MetalCommandRecorder::dummy_black_texture() {
    if (m_dummy_black_texture != nullptr) {
        return m_dummy_black_texture;
    }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_queue;
    if (queue == nil || queue.device == nil) {
        return nullptr;
    }
    MTLTextureDescriptor* desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                     width:1
                                    height:1
                                 mipmapped:NO];
    desc.usage = MTLTextureUsageShaderRead;
    desc.storageMode = MTLStorageModeShared;
    id<MTLTexture> tex = [queue.device newTextureWithDescriptor:desc];
    if (tex == nil) {
        return nullptr;
    }
    const u32 pixel = 0;
    [tex replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
           mipmapLevel:0
             withBytes:&pixel
           bytesPerRow:4];
    m_dummy_black_texture = (__bridge_retained void*)tex;
    return m_dummy_black_texture;
}

void* MetalCommandRecorder::default_sampler() {
    if (m_default_sampler != nullptr) {
        return m_default_sampler;
    }
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)m_queue;
    if (queue == nil || queue.device == nil) {
        return nullptr;
    }
    MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];
    desc.minFilter = MTLSamplerMinMagFilterLinear;
    desc.magFilter = MTLSamplerMinMagFilterLinear;
    desc.mipFilter = MTLSamplerMipFilterNotMipmapped;
    desc.sAddressMode = MTLSamplerAddressModeClampToEdge;
    desc.tAddressMode = MTLSamplerAddressModeClampToEdge;
    desc.rAddressMode = MTLSamplerAddressModeClampToEdge;
    id<MTLSamplerState> sampler =
        [queue.device newSamplerStateWithDescriptor:desc];
    if (sampler == nil) {
        return nullptr;
    }
    m_default_sampler = (__bridge_retained void*)sampler;
    return m_default_sampler;
}

namespace {

constexpr usize kMetalInlineBytesLimit = 4096;
constexpr usize kConstantSlabBytes = 1024 * 1024;

[[nodiscard]] id<MTLBuffer> slab_mtl(void* buffer) {
    return buffer ? (__bridge id<MTLBuffer>)buffer : nil;
}

} // namespace

void MetalCommandRecorder::recycle_constant_slabs_after_present() {
    advance_inline_constant_frame();
}

void MetalCommandRecorder::advance_inline_constant_frame() {
    m_constant_frame = (m_constant_frame + 1u) % kConstantFrames;
    // Only hand these slabs back once the GPU has finished the work that read
    // them. Advancing a Present ring is not evidence of that: at a high frame
    // rate the CPU laps the ring while draws are still in flight, and the
    // memcpy below would then overwrite constants a queued draw has not
    // consumed -- displacing vertices while topology and static geometry stay
    // correct. Still live: leave the slabs marked used, so bind_large_constants
    // grows into a new slab instead of reusing storage the GPU is reading.
    const u64 done = MetalSubmitQueue::instance().completed_generation();
    if (m_constant_slot_gen[m_constant_frame] > done) {
        return;
    }
    m_vs_constant_slab = 0;
    m_ps_constant_slab = 0;
    for (ConstantSlab& slab : m_vs_constant_slabs[m_constant_frame]) {
        slab.used = 0;
    }
    for (ConstantSlab& slab : m_ps_constant_slabs[m_constant_frame]) {
        slab.used = 0;
    }
    m_constant_slot_gen[m_constant_frame] = 0;
}

void MetalCommandRecorder::release_constant_slabs() {
    auto release_frames =
        [](std::array<std::vector<ConstantSlab>, kConstantFrames>& frames) {
            for (auto& frame : frames) {
                for (ConstantSlab& slab : frame) {
                    if (slab.buffer) {
                        CFRelease(slab.buffer);
                        slab.buffer = nullptr;
                    }
                }
                frame.clear();
            }
        };
    release_frames(m_vs_constant_slabs);
    release_frames(m_ps_constant_slabs);
}

namespace {
/// VITREO_GPU_DUMP=1: print the exact bytes the GPU will read for a draw.
/// Every buffer on Apple Silicon here is MTLResourceStorageModeShared, so the
/// CPU sees the same memory the vertex fetch and constant reads see. This is
/// the poor man's Metal frame capture: it answers "did the data survive to
/// the GPU boundary" without Xcode.
[[nodiscard]] bool gpu_dump_enabled() noexcept {
    static const bool on = std::getenv("VITREO_GPU_DUMP") != nullptr;
    return on;
}
void gpu_dump_floats(const char* tag, const void* base, usize byte_offset,
                     unsigned nfloats) {
    const float* f = reinterpret_cast<const float*>(
        static_cast<const u8*>(base) + byte_offset);
    std::fprintf(stderr, "[gpu-dump] %s +%zu:", tag, byte_offset);
    for (unsigned i = 0; i < nfloats; ++i) {
        std::fprintf(stderr, " %.6g", f[i]);
    }
    std::fputc('\n', stderr);
}
} // namespace

void MetalCommandRecorder::set_vertex_buffer(void* buffer, usize offset, u32 index) {
    if (gpu_dump_enabled() && index >= 16 && buffer != nullptr) {
        id<MTLBuffer> b = (__bridge id<MTLBuffer>)buffer;
        if ([b contents] != nullptr) {
            std::fprintf(stderr,
                         "[gpu-dump] vertex stream idx=%u off=%zu len=%zu\n",
                         index, offset, (usize)[b length]);
            // First vertex's first 8 floats, and bytes 80..95 (a 96-byte
            // stride's 6th attribute, blend indices).
            gpu_dump_floats("v0[0..7]", [b contents], offset, 8);
            if ([b length] >= offset + 96) {
                gpu_dump_floats("v0[attr5]", [b contents], offset + 80, 4);
            }
            if ([b length] >= offset + 192) {
                gpu_dump_floats("v1[0..7]", [b contents], offset + 96, 8);
                gpu_dump_floats("v1[attr5]", [b contents], offset + 176, 4);
            }
        }
    }
    if (index < kCachedBufferSlots && m_vertex_buffer_bound[index] &&
        m_vertex_buffers[index] == buffer &&
        m_vertex_buffer_offsets[index] == offset) {
        return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLBuffer> buf = (__bridge id<MTLBuffer>)buffer;
    [enc setVertexBuffer:buf offset:offset atIndex:index];
    if (index < kCachedBufferSlots) {
        m_vertex_buffers[index] = buffer;
        m_vertex_buffer_offsets[index] = offset;
        m_vertex_buffer_bound[index] = true;
    }
}

namespace {

[[nodiscard]] u64 hash_constant_bytes(const void* data, usize length) {
    u64 hash = 14695981039346656037ULL;
    const auto* bytes = static_cast<const u8*>(data);
    usize offset = 0;
    while (offset + sizeof(u64) <= length) {
        u64 word = 0;
        std::memcpy(&word, bytes + offset, sizeof(word));
        hash ^= word;
        hash *= 1099511628211ULL;
        offset += sizeof(u64);
    }
    while (offset < length) {
        hash ^= bytes[offset];
        hash *= 1099511628211ULL;
        ++offset;
    }
    hash ^= length;
    return hash;
}

} // namespace

void MetalCommandRecorder::bind_large_constants(bool vertex, const void* data,
                                                usize length, u32 index) {
    if (m_render_encoder == nullptr || m_queue == nullptr || data == nullptr ||
        length == 0) {
        return;
    }
    if (gpu_dump_enabled() && vertex) {
        std::fprintf(stderr, "[gpu-dump] vs constants len=%zu idx=%u\n",
                     length, index);
        gpu_dump_floats("c0", data, 0, 4);
        gpu_dump_floats("c1", data, 16, 4);
        if (length >= 192u * 16u + 16u) gpu_dump_floats("c188", data, 188u*16u, 4);
        if (length >= 192u * 16u)      gpu_dump_floats("c191", data, 191u*16u, 4);
    }
    bool& bound = vertex ? m_vs_bytes_bound : m_ps_bytes_bound;
    usize& bound_length = vertex ? m_vs_bytes_length : m_ps_bytes_length;
    u64& bound_hash = vertex ? m_vs_bytes_hash : m_ps_bytes_hash;
    const u64 hash = hash_constant_bytes(data, length);
    if (bound && bound_length == length && bound_hash == hash) {
        return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    // Apple documents set*Bytes as at most 4 KiB. Stay strictly under that
    // so a 256-register VS constant file (exactly 4096 bytes, typical of
    // c[a0]/c[aL] palette skinning) takes the bump-allocated slab path
    // rather than risking an encoder error at the limit. Inline copies
    // live in the command stream; slabs must not be recycled until Present.
    if (length < kMetalInlineBytesLimit) {
        if (vertex) {
            [enc setVertexBytes:data length:length atIndex:index];
            if (index < kCachedBufferSlots) m_vertex_buffer_bound[index] = false;
        } else {
            [enc setFragmentBytes:data length:length atIndex:index];
            if (index < kCachedBufferSlots) {
                m_fragment_buffer_bound[index] = false;
            }
        }
        bound = true;
        bound_length = length;
        bound_hash = hash;
        return;
    }

    auto& slabs = vertex ? m_vs_constant_slabs[m_constant_frame]
                         : m_ps_constant_slabs[m_constant_frame];
    u32& slab_index = vertex ? m_vs_constant_slab : m_ps_constant_slab;
    constexpr usize kAlign = 256;
    const usize need = (length + kAlign - 1u) & ~(kAlign - 1u);
    if (slabs.empty()) {
        slab_index = 0;
    }
    auto ensure_slab = [&](u32 which) -> ConstantSlab* {
        if (which < slabs.size()) return &slabs[which];
        id<MTLDevice> device =
            [(__bridge id<MTLCommandQueue>)m_queue device];
        const usize cap = std::max(need, kConstantSlabBytes);
        id<MTLBuffer> buf =
            [device newBufferWithLength:cap
                                options:MTLResourceStorageModeShared];
        if (buf == nil) return nullptr;
        ConstantSlab slab;
        slab.buffer = (__bridge_retained void*)buf;
        slab.capacity = cap;
        slab.used = 0;
        slabs.push_back(slab);
        return &slabs.back();
    };

    ConstantSlab* slab = ensure_slab(slab_index);
    if (slab == nullptr) return;
    usize start = (slab->used + kAlign - 1u) & ~(kAlign - 1u);
    if (start + need > slab->capacity) {
        ++slab_index;
        slab = ensure_slab(slab_index);
        if (slab == nullptr) return;
        start = 0;
    }
    id<MTLBuffer> buf = slab_mtl(slab->buffer);
    if (buf == nil || [buf contents] == nullptr) return;
    std::memcpy(static_cast<u8*>([buf contents]) + start, data, length);
    slab->used = start + need;
    // Pin this frame's slabs until the GPU retires the list being recorded.
    {
        const u64 gen = MetalSubmitQueue::instance().recording_generation();
        if (gen > m_constant_slot_gen[m_constant_frame]) {
            m_constant_slot_gen[m_constant_frame] = gen;
        }
    }
    if (vertex) {
        [enc setVertexBuffer:buf offset:start atIndex:index];
        if (index < kCachedBufferSlots) {
            m_vertex_buffers[index] = slab->buffer;
            m_vertex_buffer_offsets[index] = start;
            m_vertex_buffer_bound[index] = true;
        }
    } else {
        [enc setFragmentBuffer:buf offset:start atIndex:index];
        if (index < kCachedBufferSlots) {
            m_fragment_buffers[index] = slab->buffer;
            m_fragment_buffer_offsets[index] = start;
            m_fragment_buffer_bound[index] = true;
        }
    }
    bound = true;
    bound_length = length;
    bound_hash = hash;
}

void MetalCommandRecorder::set_fragment_buffer(void* buffer, usize offset, u32 index) {
    if (index < kCachedBufferSlots && m_fragment_buffer_bound[index] &&
        m_fragment_buffers[index] == buffer &&
        m_fragment_buffer_offsets[index] == offset) {
        return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLBuffer> buf = (__bridge id<MTLBuffer>)buffer;
    [enc setFragmentBuffer:buf offset:offset atIndex:index];
    if (index < kCachedBufferSlots) {
        m_fragment_buffers[index] = buffer;
        m_fragment_buffer_offsets[index] = offset;
        m_fragment_buffer_bound[index] = true;
    }
}

void MetalCommandRecorder::set_vertex_bytes(const void* data, usize length,
                                            u32 index) {
    bind_large_constants(true, data, length, index);
}

void MetalCommandRecorder::set_fragment_bytes(const void* data, usize length,
                                              u32 index) {
    bind_large_constants(false, data, length, index);
}

void MetalCommandRecorder::set_vertex_texture(void* texture, u32 index) {
    if (texture == nullptr) {
        texture = dummy_black_texture();
        if (texture == nullptr) return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    [enc setVertexTexture:tex atIndex:index];
}

void MetalCommandRecorder::set_fragment_texture(void* texture, u32 index) {
    if (texture == nullptr) {
        texture = dummy_black_texture();
        if (texture == nullptr) return;
    }
    if (index < kCachedTextureSlots && m_fragment_texture_bound[index] &&
        m_fragment_textures[index] == texture) {
        return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    [enc setFragmentTexture:tex atIndex:index];
    if (index < kCachedTextureSlots) {
        m_fragment_textures[index] = texture;
        m_fragment_texture_bound[index] = true;
    }
}

void MetalCommandRecorder::set_vertex_sampler(void* sampler, u32 index) {
    if (sampler == nullptr) {
        sampler = default_sampler();
        if (sampler == nullptr) return;
    }
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLSamplerState> s = (__bridge id<MTLSamplerState>)sampler;
    [enc setVertexSamplerState:s atIndex:index];
}

void MetalCommandRecorder::set_fragment_sampler(void* sampler, u32 index) {
    if (sampler == nullptr) {
        sampler = default_sampler();
        if (sampler == nullptr) return;
    }
    if (index < kCachedTextureSlots && m_fragment_sampler_bound[index] &&
        m_fragment_samplers[index] == sampler) {
        return;
    }
    id<MTLSamplerState> s = (__bridge id<MTLSamplerState>)sampler;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setFragmentSamplerState:s atIndex:index];
    if (index < kCachedTextureSlots) {
        m_fragment_samplers[index] = sampler;
        m_fragment_sampler_bound[index] = true;
    }
}

void MetalCommandRecorder::set_viewport(f64 x, f64 y, f64 w, f64 h,
                                        f64 znear, f64 zfar) {
    const std::array<f64, 6> viewport{x, y, w, h, znear, zfar};
    if (m_viewport_bound && m_viewport == viewport) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    MTLViewport vp = { x, y, w, h, znear, zfar };
    [enc setViewport:vp];
    m_viewport = viewport;
    m_viewport_bound = true;
}

void MetalCommandRecorder::set_scissor(u32 x, u32 y, u32 w, u32 h) {
    const std::array<u32, 4> scissor{x, y, w, h};
    if (m_scissor_bound && m_scissor == scissor) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    MTLScissorRect rect = { x, y, w, h };
    [enc setScissorRect:rect];
    m_scissor = scissor;
    m_scissor_bound = true;
}

void MetalCommandRecorder::draw_patches(void* factors, usize factors_offset,
                                        u32 control_points, u32 patch_count,
                                        u32 instance_count,
                                        u32 first_instance) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setTessellationFactorBuffer:(__bridge id<MTLBuffer>)factors
                              offset:factors_offset
                      instanceStride:0];
    [enc drawPatches:control_points
               patchStart:0
               patchCount:patch_count
         patchIndexBuffer:nil
    patchIndexBufferOffset:0
            instanceCount:std::max(instance_count, 1u)
             baseInstance:first_instance];
}

void MetalCommandRecorder::set_viewports(const std::array<float, 6>* viewports,
                                         const std::array<u32, 4>* scissors,
                                         u32 count) {
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    std::array<MTLViewport, BACKEND_MAX_VIEWPORTS> vps{};
    std::array<MTLScissorRect, BACKEND_MAX_VIEWPORTS> rects{};
    count = std::min<u32>(count, BACKEND_MAX_VIEWPORTS);
    for (u32 i = 0; i < count; ++i) {
        const auto& v = viewports[i];
        vps[i] = {v[0], v[1], v[2], v[3], v[4], v[5]};
        rects[i] = {scissors[i][0], scissors[i][1], scissors[i][2],
                    scissors[i][3]};
    }
    [enc setViewports:vps.data() count:count];
    [enc setScissorRects:rects.data() count:count];
    m_viewport_bound = false;
    m_scissor_bound = false;
}

void MetalCommandRecorder::set_cull_mode(u32 mode) {
    if (m_cull_mode_bound && m_cull_mode == mode) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setCullMode:(MTLCullMode)mode];
    m_cull_mode = mode;
    m_cull_mode_bound = true;
}

void MetalCommandRecorder::set_triangle_fill_mode(u32 mode) {
    if (m_triangle_fill_mode_bound && m_triangle_fill_mode == mode) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setTriangleFillMode:(MTLTriangleFillMode)mode];
    m_triangle_fill_mode = mode;
    m_triangle_fill_mode_bound = true;
}

void MetalCommandRecorder::set_depth_clip_mode(u32 mode) {
    if (m_depth_clip_mode_bound && m_depth_clip_mode == mode) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setDepthClipMode:(MTLDepthClipMode)mode];
    m_depth_clip_mode = mode;
    m_depth_clip_mode_bound = true;
}

void MetalCommandRecorder::set_depth_bias(f32 bias, f32 slope_scale,
                                          f32 clamp) {
    const std::array<f32, 3> value{bias, slope_scale, clamp};
    if (m_depth_bias_bound && m_depth_bias == value) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setDepthBias:bias slopeScale:slope_scale clamp:clamp];
    m_depth_bias = value;
    m_depth_bias_bound = true;
}

void MetalCommandRecorder::set_front_facing(u32 winding) {
    if (m_front_facing_bound && m_front_facing == winding) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setFrontFacingWinding:(MTLWinding)winding];
    m_front_facing = winding;
    m_front_facing_bound = true;
}

void MetalCommandRecorder::reset_render_state_cache() {
    m_render_pipeline_bound = false;
    m_depth_stencil_bound = false;
    m_viewport_bound = false;
    m_scissor_bound = false;
    m_cull_mode_bound = false;
    m_triangle_fill_mode_bound = false;
    m_depth_clip_mode_bound = false;
    m_depth_bias_bound = false;
    m_front_facing_bound = false;
    m_vertex_buffer_bound.fill(false);
    m_fragment_buffer_bound.fill(false);
    m_fragment_texture_bound.fill(false);
    m_fragment_sampler_bound.fill(false);
    m_vs_bytes_bound = false;
    m_ps_bytes_bound = false;
}

void MetalCommandRecorder::set_blend_color(f32 r, f32 g, f32 b, f32 a) {
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setBlendColorRed:r green:g blue:b alpha:a];
}

void MetalCommandRecorder::set_stencil_reference(u32 ref) {
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc setStencilReferenceValue:ref];
}

// =============================================================================
// Draw calls
// =============================================================================

void MetalCommandRecorder::draw(u32 primitive_type, u32 vertex_count, u32 start) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    [enc drawPrimitives:(MTLPrimitiveType)primitive_type
            vertexStart:start
            vertexCount:vertex_count];
}

void MetalCommandRecorder::draw_indexed(u32 primitive_type, u32 index_count,
                                        u32 index_type, void* index_buffer,
                                        usize offset) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)index_buffer;
    [enc drawIndexedPrimitives:(MTLPrimitiveType)primitive_type
                    indexCount:index_count
                     indexType:(MTLIndexType)index_type
                   indexBuffer:ibuf
             indexBufferOffset:offset];
}

namespace {
/// VITREO_GPU_WATCH_SPLIT: also name every draw's size in the command-buffer
/// summary, so a stuck single-pass buffer shows which draw is enormous.
bool split_draw_notes() {
    static const bool on = std::getenv("VITREO_GPU_WATCH_SPLIT") != nullptr;
    return on;
}
}  // namespace

void MetalCommandRecorder::draw_instanced(u32 primitive_type, u32 vertex_count,
                                          u32 instance_count, u32 start_vertex,
                                          u32 start_instance) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    if (split_draw_notes()) {
        char note[64];
        std::snprintf(note, sizeof(note), "d(v%u,i%u,b%u)", vertex_count,
                      instance_count, start_instance);
        gpu_watch::note(m_cb_summary, note);
    }
    [enc drawPrimitives:(MTLPrimitiveType)primitive_type
            vertexStart:start_vertex
            vertexCount:vertex_count
          instanceCount:instance_count
           baseInstance:start_instance];
}

void MetalCommandRecorder::draw_indexed_instanced(u32 primitive_type, u32 index_count,
                                                  u32 index_type, void* index_buffer,
                                                  usize offset, u32 instance_count,
                                                  i32 base_vertex, u32 start_instance) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)index_buffer;
    if (split_draw_notes()) {
        char note[64];
        std::snprintf(note, sizeof(note), "di(n%u,i%u,b%u)", index_count,
                      instance_count, start_instance);
        gpu_watch::note(m_cb_summary, note);
    }
    [enc drawIndexedPrimitives:(MTLPrimitiveType)primitive_type
                    indexCount:index_count
                     indexType:(MTLIndexType)index_type
                   indexBuffer:ibuf
             indexBufferOffset:offset
                 instanceCount:instance_count
                    baseVertex:base_vertex
                  baseInstance:start_instance];
}

// D3D and Metal spell the indirect argument block identically, so the guest's
// buffer is handed to Metal unmodified rather than read back and repacked.
//
//   D3D12_DRAW_ARGUMENTS         {VertexCountPerInstance, InstanceCount,
//                                 StartVertexLocation, StartInstanceLocation}
//   MTLDrawPrimitivesIndirectArguments
//                                {vertexCount, instanceCount, vertexStart,
//                                 baseInstance}
//
//   D3D12_DRAW_INDEXED_ARGUMENTS {IndexCountPerInstance, InstanceCount,
//                                 StartIndexLocation, BaseVertexLocation (INT),
//                                 StartInstanceLocation}
//   MTLDrawIndexedPrimitivesIndirectArguments
//                                {indexCount, instanceCount, indexStart,
//                                 baseVertex (int32), baseInstance}
//
// Same members, same order, same signedness. Learn documents the D3D11 indirect
// buffers as carrying "data that matches the argument list" of the direct call,
// and the D3D12 structs above name that layout explicitly. Reading the counts on
// the CPU would defeat the point of the API: the whole purpose is that the GPU
// wrote them and the CPU never sees them.

void MetalCommandRecorder::draw_instanced_indirect(u32 primitive_type,
                                                   void* args_buffer,
                                                   usize args_offset) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLBuffer> args = (__bridge id<MTLBuffer>)args_buffer;
    if (split_draw_notes()) gpu_watch::note(m_cb_summary, "d-indirect");
    [enc drawPrimitives:(MTLPrimitiveType)primitive_type
          indirectBuffer:args
    indirectBufferOffset:args_offset];
}

void MetalCommandRecorder::draw_indexed_instanced_indirect(
    u32 primitive_type, u32 index_type, void* index_buffer, usize index_offset,
    void* args_buffer, usize args_offset) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    id<MTLBuffer> ibuf = (__bridge id<MTLBuffer>)index_buffer;
    id<MTLBuffer> args = (__bridge id<MTLBuffer>)args_buffer;
    if (split_draw_notes()) gpu_watch::note(m_cb_summary, "di-indirect");
    [enc drawIndexedPrimitives:(MTLPrimitiveType)primitive_type
                    indexType:(MTLIndexType)index_type
                  indexBuffer:ibuf
            indexBufferOffset:index_offset
               indirectBuffer:args
         indirectBufferOffset:args_offset];
}

void MetalCommandRecorder::use_render_resources(
    const std::vector<void*>& resources) {
    if (resources.empty()) return;
    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)m_render_encoder;
    @autoreleasepool {
        for (void* resource : resources) {
            if (resource == nullptr) continue;
            // Both stages, read and write. A table does not know which stages
            // read it or which of its slots are u# rather than t#; that lives on
            // the far side of the shader translator. Declaring the union is
            // correct but conservative -- narrowing it is a later optimisation
            // that needs the shader's declared access to be plumbed through.
            [enc useResource:(__bridge id<MTLResource>)resource
                      usage:MTLResourceUsageRead | MTLResourceUsageWrite
                     stages:MTLRenderStageVertex | MTLRenderStageFragment];
        }
    }
}

void MetalCommandRecorder::use_compute_resources(
    const std::vector<void*>& resources) {
    if (resources.empty()) return;
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    @autoreleasepool {
        for (void* resource : resources) {
            if (resource == nullptr) continue;
            [enc useResource:(__bridge id<MTLResource>)resource
                       usage:MTLResourceUsageRead | MTLResourceUsageWrite];
        }
    }
}

// =============================================================================
// Compute
// =============================================================================

void MetalCommandRecorder::begin_compute() {
    end_active_encoder();
    id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)m_command_buffer;
    id<MTLComputeCommandEncoder> enc = nil;
    if (gpu_profile::enabled()) {
        enc = (__bridge_transfer id<MTLComputeCommandEncoder>)
            gpu_profile::begin_compute(m_profile, m_queue, m_command_buffer);
    }
    if (enc == nil) {
        enc = [buf computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
    }
    m_compute_encoder = (__bridge_retained void*)enc;
    open_debug_groups(m_compute_encoder);
    VITREO_TRACE("[command] begin_compute enc=%p\n", m_compute_encoder);
}

void MetalCommandRecorder::end_compute() {
    if (m_compute_encoder) {
        close_debug_groups(m_compute_encoder);
        id<MTLComputeCommandEncoder> enc =
            (__bridge_transfer id<MTLComputeCommandEncoder>)m_compute_encoder;
        [enc endEncoding];
        m_compute_encoder = nullptr;
        VITREO_TRACE("[command] end_compute\n");
    }
}

void MetalCommandRecorder::set_compute_pipeline(void* pso) {
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    id<MTLComputePipelineState> state =
        (__bridge id<MTLComputePipelineState>)pso;
    [enc setComputePipelineState:state];
    if (m_profile) gpu_profile::note_pipeline(m_profile.get(), state.label.UTF8String);
    if (gpu_watch::enabled()) {
        gpu_watch::note(m_cb_summary, state.label.UTF8String);
    }
}

void MetalCommandRecorder::set_compute_buffer(void* buffer, usize offset, u32 index) {
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    id<MTLBuffer> buf = (__bridge id<MTLBuffer>)buffer;
    [enc setBuffer:buf offset:offset atIndex:index];
}

void MetalCommandRecorder::set_compute_bytes(const void* data, usize length,
                                             u32 index) {
    if (data == nullptr || length == 0) return;
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    [enc setBytes:data length:length atIndex:index];
}

void MetalCommandRecorder::set_compute_texture(void* texture, u32 index) {
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    [enc setTexture:tex atIndex:index];
}

void MetalCommandRecorder::set_compute_sampler(void* sampler, u32 index) {
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    id<MTLSamplerState> state = (__bridge id<MTLSamplerState>)sampler;
    [enc setSamplerState:state atIndex:index];
}

void MetalCommandRecorder::dispatch(u32 gx, u32 gy, u32 gz,
                                    u32 tx, u32 ty, u32 tz) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    MTLSize threadgroups = { gx, gy, gz };
    MTLSize threads_per = { tx, ty, tz };
    [enc dispatchThreadgroups:threadgroups threadsPerThreadgroup:threads_per];
}

void MetalCommandRecorder::dispatch_indirect(void* args_buffer,
                                             usize args_offset,
                                             u32 tx, u32 ty, u32 tz) {
    if (m_profile) gpu_profile::note_work(m_profile.get());
    id<MTLComputeCommandEncoder> enc =
        (__bridge id<MTLComputeCommandEncoder>)m_compute_encoder;
    id<MTLBuffer> args = (__bridge id<MTLBuffer>)args_buffer;
    // Threads-per-threadgroup stays a CPU-side value in both APIs: D3D takes it
    // from the compute shader's numthreads declaration, Metal from this
    // argument. Only the threadgroup *count* comes from the buffer.
    MTLSize threads_per = { tx, ty, tz };
    [enc dispatchThreadgroupsWithIndirectBuffer:args
                          indirectBufferOffset:args_offset
                         threadsPerThreadgroup:threads_per];
}

// =============================================================================
// Timeline events
// =============================================================================

void MetalCommandRecorder::signal_event(void* event, u64 value) {
    if (event == nullptr) return;
    // Metal orders an event against command-buffer boundaries, so the active
    // encoder has to close first or the signal could be observed before the work
    // it is meant to follow.
    end_active_encoder();
    id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)m_command_buffer;
    if (buf == nil) return;
    if (gpu_watch::enabled()) {
        char note[48];
        std::snprintf(note, sizeof(note), "signal:%p=%llu", event,
                      static_cast<unsigned long long>(value));
        gpu_watch::note(m_cb_summary, note);
    }
    [buf encodeSignalEvent:(__bridge id<MTLEvent>)event value:value];
}

void MetalCommandRecorder::wait_for_event(void* event, u64 value) {
    if (event == nullptr) return;
    end_active_encoder();
    id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)m_command_buffer;
    if (buf == nil) return;
    if (gpu_watch::enabled()) {
        char note[48];
        std::snprintf(note, sizeof(note), "WAIT:%p=%llu", event,
                      static_cast<unsigned long long>(value));
        gpu_watch::note(m_cb_summary, note);
        m_cb_waits.push_back({event, value});
    }
    [buf encodeWaitForEvent:(__bridge id<MTLEvent>)event value:value];
}

// =============================================================================
// Blit
// =============================================================================

void MetalCommandRecorder::ensure_blit_encoder() {
    if (!m_blit_encoder) {
        end_active_encoder();
        id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)m_command_buffer;
        id<MTLBlitCommandEncoder> enc = nil;
        if (gpu_profile::enabled()) {
            enc = (__bridge_transfer id<MTLBlitCommandEncoder>)
                gpu_profile::begin_blit(m_profile, m_queue, m_command_buffer);
        }
        if (enc == nil) enc = [buf blitCommandEncoder];
        m_blit_encoder = (__bridge_retained void*)enc;
        open_debug_groups(m_blit_encoder);
        VITREO_TRACE("[command] blit encoder created enc=%p\n", m_blit_encoder);
    }
    if (m_profile) gpu_profile::note_work(m_profile.get());
}

void MetalCommandRecorder::blit_copy_buffer(void* src, usize src_off,
                                            void* dst, usize dst_off,
                                            usize size) {
    ensure_blit_encoder();
    id<MTLBlitCommandEncoder> enc =
        (__bridge id<MTLBlitCommandEncoder>)m_blit_encoder;
    id<MTLBuffer> src_buf = (__bridge id<MTLBuffer>)src;
    id<MTLBuffer> dst_buf = (__bridge id<MTLBuffer>)dst;
    [enc copyFromBuffer:src_buf sourceOffset:src_off
               toBuffer:dst_buf destinationOffset:dst_off size:size];
}

void MetalCommandRecorder::blit_fill_buffer(void* dst, usize offset,
                                            usize size, u8 value) {
    ensure_blit_encoder();
    id<MTLBlitCommandEncoder> enc =
        (__bridge id<MTLBlitCommandEncoder>)m_blit_encoder;
    [enc fillBuffer:(__bridge id<MTLBuffer>)dst
              range:NSMakeRange(offset, size)
              value:value];
}

void MetalCommandRecorder::blit_copy_buffer_to_texture(
    void* src, usize bytes_per_row, usize bytes_per_image, u32 width,
    u32 height, u32 depth, void* dst, u32 level, u32 slice, u32 x, u32 y,
    u32 z, u32 options, usize src_offset) {
    ensure_blit_encoder();
    id<MTLBlitCommandEncoder> enc =
        (__bridge id<MTLBlitCommandEncoder>)m_blit_encoder;
    [enc copyFromBuffer:(__bridge id<MTLBuffer>)src
            sourceOffset:src_offset
       sourceBytesPerRow:bytes_per_row
     sourceBytesPerImage:bytes_per_image
              sourceSize:MTLSizeMake(width, height, depth)
               toTexture:(__bridge id<MTLTexture>)dst
        destinationSlice:slice
        destinationLevel:level
       destinationOrigin:MTLOriginMake(x, y, z)
                 options:static_cast<MTLBlitOption>(options)];
}

void MetalCommandRecorder::blit_copy_texture(void* src, void* dst,
                                             u32 width, u32 height) {
    blit_copy_texture_region(src, 0, 0, dst, 0, 0, width, height);
}

void MetalCommandRecorder::resolve_msaa_texture(void* src, void* dst,
                                                u32 src_level, u32 src_slice,
                                                u32 dst_level, u32 dst_slice) {
    if (src == nullptr || dst == nullptr) return;
    ensure_begun();
    MTLRenderPassDescriptor* pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = (__bridge id<MTLTexture>)src;
    pass.colorAttachments[0].level = src_level;
    pass.colorAttachments[0].slice = src_slice;
    pass.colorAttachments[0].resolveTexture = (__bridge id<MTLTexture>)dst;
    pass.colorAttachments[0].resolveLevel = dst_level;
    pass.colorAttachments[0].resolveSlice = dst_slice;
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    // Keep the MSAA surface. MultisampleResolve-only leaves it undefined,
    // so the next frame's Load is black/magenta and UI alpha-blends into
    // a trail (an options window while the scene composite misses).
    pass.colorAttachments[0].storeAction =
        MTLStoreActionStoreAndMultisampleResolve;
    begin_render_pass((__bridge void*)pass);
    end_render_pass();
}

void MetalCommandRecorder::blit_copy_texture_region(void* src, u32 src_x,
                                                    u32 src_y, void* dst,
                                                    u32 dst_x, u32 dst_y,
                                                    u32 width, u32 height,
                                                    u32 src_level,
                                                    u32 src_slice,
                                                    u32 dst_level,
                                                    u32 dst_slice, u32 depth,
                                                    u32 src_z, u32 dst_z) {
    ensure_blit_encoder();
    id<MTLBlitCommandEncoder> enc =
        (__bridge id<MTLBlitCommandEncoder>)m_blit_encoder;
    id<MTLTexture> src_texture = (__bridge id<MTLTexture>)src;
    id<MTLTexture> dst_texture = (__bridge id<MTLTexture>)dst;
    [enc copyFromTexture:src_texture
             sourceSlice:src_slice
             sourceLevel:src_level
            sourceOrigin:MTLOriginMake(src_x, src_y, src_z)
              sourceSize:MTLSizeMake(width, height, depth == 0 ? 1 : depth)
               toTexture:dst_texture
        destinationSlice:dst_slice
        destinationLevel:dst_level
       destinationOrigin:MTLOriginMake(dst_x, dst_y, dst_z)];
}

void MetalCommandRecorder::blit_copy_texture_to_buffer(
    void* src, u32 level, u32 slice, u32 x, u32 y, u32 z, u32 width,
    u32 height, u32 depth, void* dst, usize bytes_per_row,
    usize bytes_per_image, usize dst_offset, u32 options) {
    ensure_blit_encoder();
    id<MTLBlitCommandEncoder> enc =
        (__bridge id<MTLBlitCommandEncoder>)m_blit_encoder;
    [enc copyFromTexture:(__bridge id<MTLTexture>)src
                 sourceSlice:slice
                 sourceLevel:level
                sourceOrigin:MTLOriginMake(x, y, z)
                  sourceSize:MTLSizeMake(width, height, depth)
                    toBuffer:(__bridge id<MTLBuffer>)dst
           destinationOffset:dst_offset
      destinationBytesPerRow:bytes_per_row
    destinationBytesPerImage:bytes_per_image
                     options:static_cast<MTLBlitOption>(options)];
}

void MetalCommandRecorder::blit_generate_mipmaps(void* texture) {
    ensure_blit_encoder();
    id<MTLBlitCommandEncoder> enc =
        (__bridge id<MTLBlitCommandEncoder>)m_blit_encoder;
    id<MTLTexture> tex = (__bridge id<MTLTexture>)texture;
    [enc generateMipmapsForTexture:tex];
}

// =============================================================================
// Submit
// =============================================================================

void MetalCommandRecorder::commit() {
    void* retained = commit_retained();
    (void)(__bridge_transfer id<MTLCommandBuffer>)retained;
}

void* MetalCommandRecorder::commit_retained() {
    end_active_encoder();
    id<MTLCommandBuffer> buf =
        (__bridge_transfer id<MTLCommandBuffer>)m_command_buffer;
    m_command_buffer = nullptr;
    static const bool k_log_errors =
        std::getenv("VITREO_D3D11_DIAG") != nullptr ||
        std::getenv("VITREO_METAL_ERROR_DIAG") != nullptr;
    if (k_log_errors) {
        [buf addCompletedHandler:^(id<MTLCommandBuffer> completed) {
          if (completed.status == MTLCommandBufferStatusError) {
              std::fprintf(
                  stderr, "[metal] command buffer failed: %s\n",
                  completed.error != nil
                      ? [[completed.error localizedDescription] UTF8String]
                      : "unknown Metal error");
          }
        }];
    }
    gpu_profile::attach((__bridge void*)buf, std::move(m_profile));
    gpu_watch::track((__bridge void*)buf, std::move(m_cb_summary),
                     std::move(m_cb_waits));
    m_cb_summary.clear();
    m_cb_waits.clear();
    [buf commit];
    VITREO_TRACE("[command] committed\n");
    return (__bridge_retained void*)buf;
}

void MetalCommandRecorder::commit_with_handler(void (*on_complete)(u64),
                                               u64 token) {
    end_active_encoder();
    id<MTLCommandBuffer> buf =
        (__bridge_transfer id<MTLCommandBuffer>)m_command_buffer;
    m_command_buffer = nullptr;
    // A command buffer that errors discards every encoder it carries, so the
    // render target silently keeps its previous (often uninitialised)
    // contents. Report it instead of presenting the failure as a blank frame.
    static const bool k_log_errors =
        std::getenv("VITREO_D3D11_DIAG") != nullptr ||
        std::getenv("VITREO_METAL_ERROR_DIAG") != nullptr;
    if (k_log_errors) {
        [buf addCompletedHandler:^(id<MTLCommandBuffer> completed) {
          if (completed.status == MTLCommandBufferStatusError) {
              std::fprintf(
                  stderr, "[metal] command buffer failed: %s\n",
                  completed.error != nil
                      ? [[completed.error localizedDescription] UTF8String]
                      : "unknown Metal error");
          }
        }];
    }
    if (on_complete != nullptr) {
        [buf addCompletedHandler:^(id<MTLCommandBuffer>) {
            on_complete(token);
        }];
    }
    gpu_profile::attach((__bridge void*)buf, std::move(m_profile));
    gpu_watch::track((__bridge void*)buf, std::move(m_cb_summary),
                     std::move(m_cb_waits));
    m_cb_summary.clear();
    m_cb_waits.clear();
    [buf commit];
    VITREO_TRACE("[command] committed with handler\n");
}

void MetalCommandRecorder::commit_with_end_time(
    void (*on_complete)(void*, double), void* context) {
    end_active_encoder();
    id<MTLCommandBuffer> buf =
        (__bridge_transfer id<MTLCommandBuffer>)m_command_buffer;
    m_command_buffer = nullptr;
    [buf addCompletedHandler:^(id<MTLCommandBuffer> completed) {
      on_complete(context, completed.GPUEndTime);
    }];
    gpu_profile::attach((__bridge void*)buf, std::move(m_profile));
    gpu_watch::track((__bridge void*)buf, std::move(m_cb_summary),
                     std::move(m_cb_waits));
    m_cb_summary.clear();
    m_cb_waits.clear();
    [buf commit];
    VITREO_TRACE("[command] committed with end-time handler\n");
}

void MetalCommandRecorder::commit_and_wait() {
    end_active_encoder();
    id<MTLCommandBuffer> buf =
        (__bridge_transfer id<MTLCommandBuffer>)m_command_buffer;
    gpu_profile::attach((__bridge void*)buf, std::move(m_profile));
    gpu_watch::track((__bridge void*)buf, std::move(m_cb_summary),
                     std::move(m_cb_waits));
    m_cb_summary.clear();
    m_cb_waits.clear();
    [buf commit];
    [buf waitUntilCompleted];
    m_command_buffer = nullptr;
    VITREO_TRACE("[command] committed and waited\n");
}

// =============================================================================
// Internal
// =============================================================================

void MetalCommandRecorder::end_active_encoder() {
    if (m_render_encoder) {
        close_debug_groups(m_render_encoder);
        id<MTLRenderCommandEncoder> enc =
            (__bridge_transfer id<MTLRenderCommandEncoder>)m_render_encoder;
        [enc endEncoding];
        m_render_encoder = nullptr;
    }
    if (m_compute_encoder) {
        close_debug_groups(m_compute_encoder);
        id<MTLComputeCommandEncoder> enc =
            (__bridge_transfer id<MTLComputeCommandEncoder>)m_compute_encoder;
        [enc endEncoding];
        m_compute_encoder = nullptr;
    }
    if (m_blit_encoder) {
        close_debug_groups(m_blit_encoder);
        id<MTLBlitCommandEncoder> enc =
            (__bridge_transfer id<MTLBlitCommandEncoder>)m_blit_encoder;
        [enc endEncoding];
        m_blit_encoder = nullptr;
    }
}

// =============================================================================
// Debug markers
// =============================================================================

namespace {

/// A debug label as an NSString; invalid UTF-8 (never produced by the D3D
/// frontend's UTF-16 conversion) becomes an empty label rather than nil.
NSString* debug_label(const std::string& label) {
    NSString* text = [NSString stringWithUTF8String:label.c_str()];
    return text != nil ? text : @"";
}

}  // namespace

void MetalCommandRecorder::open_debug_groups(void* encoder) {
    m_encoder_debug_depth = 0;
    if (encoder == nullptr) return;
    id<MTLCommandEncoder> enc = (__bridge id<MTLCommandEncoder>)encoder;
    for (const std::string& label : m_debug_groups) {
        [enc pushDebugGroup:debug_label(label)];
    }
    m_encoder_debug_depth = m_debug_groups.size();
}

void MetalCommandRecorder::close_debug_groups(void* encoder) {
    if (encoder != nullptr) {
        id<MTLCommandEncoder> enc = (__bridge id<MTLCommandEncoder>)encoder;
        for (usize i = 0; i < m_encoder_debug_depth; ++i) [enc popDebugGroup];
    }
    m_encoder_debug_depth = 0;
}

void MetalCommandRecorder::push_debug_group(const std::string& label) {
    if (m_debug_groups.size() >= kMaxDebugGroups) {
        ++m_debug_overflow;
        return;
    }
    m_debug_groups.push_back(label);
    if (void* encoder = active_encoder()) {
        [(__bridge id<MTLCommandEncoder>)encoder pushDebugGroup:
            debug_label(label)];
        ++m_encoder_debug_depth;
    }
}

void MetalCommandRecorder::pop_debug_group() {
    if (m_debug_overflow > 0) {
        --m_debug_overflow;
        return;
    }
    if (m_debug_groups.empty()) return;
    m_debug_groups.pop_back();
    void* encoder = active_encoder();
    if (encoder != nullptr && m_encoder_debug_depth > 0) {
        [(__bridge id<MTLCommandEncoder>)encoder popDebugGroup];
        --m_encoder_debug_depth;
    }
}

void MetalCommandRecorder::insert_debug_signpost(const std::string& label) {
    if (void* encoder = active_encoder()) {
        [(__bridge id<MTLCommandEncoder>)encoder insertDebugSignpost:
            debug_label(label)];
        return;
    }
    // Between encoders the command buffer takes groups, not signposts.
    ensure_begun();
    id<MTLCommandBuffer> buf = (__bridge id<MTLCommandBuffer>)m_command_buffer;
    [buf pushDebugGroup:debug_label(label)];
    [buf popDebugGroup];
}

} // namespace vitreo::graphics

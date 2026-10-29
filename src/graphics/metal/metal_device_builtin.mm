// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_device_builtin.mm
/// @brief MetalDevice built-in GPU programs: stretch blit, mips, fills, gamma

#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#import <QuartzCore/CAMetalLayer.h>
#include "metal_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_tiled.hpp"
#include "graphics/metal/metal_gpu_profile.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include <vitreo/ab_cycle.hpp>

#include <atomic>
#include <cmath>
#include <chrono>
#include "graphics/metal/metal_texture.hpp"
#include "graphics/metal/metal_shared_resource.hpp"
#include "core/memory/host_memory_floor.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <limits>
#include <memory>
#include <map>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <thread>
#include <deque>
#include <unistd.h>
#include <condition_variable>
#include <vector>

#include "graphics/metal/metal_backend_texture_2d.hpp"
#include "graphics/metal/metal_backend_texture_3d.hpp"

using namespace vitreo;

namespace vitreo::graphics {

bool MetalDevice::ensure_stretch_blit(PixelFormat format,
                                      unsigned sample_count) {
    if (sample_count == 0) sample_count = 1;
    if (m_stretch_vb == nullptr) {
        // NDC quad: +Y is the top of a Metal viewport (D3D9 dest origin).
        constexpr float k_quad[] = {
            -1.0f, 1.0f, 0.0f, 0.0f, 1.0f,  1.0f, 1.0f, 0.0f,
            -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 1.0f,
            1.0f,  1.0f, 1.0f, 0.0f, 1.0f,  -1.0f, 1.0f, 1.0f,
        };
        BackendBufferDesc vb{};
        vb.byte_width = sizeof(k_quad);
        vb.vertex_buffer = true;
        m_stretch_vb = create_buffer(vb, k_quad);
        if (m_stretch_vb == nullptr || !m_stretch_vb->valid()) {
            m_stretch_vb.reset();
            return false;
        }
    }

    const u32 key = (sample_count << 16) | static_cast<u32>(format);
    if (m_stretch_pipelines[key] != nullptr) return true;

    static const char* k_vs = R"(
#include <metal_stdlib>
using namespace metal;
struct VSIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };
struct VSOut { float4 position [[position]]; float2 uv; };
vertex VSOut vertex_main(VSIn in [[stage_in]]) {
    VSOut out;
    out.position = float4(in.pos, 0.0, 1.0);
    out.uv = in.uv;
    return out;
}
)";
    static const char* k_fs = R"(
#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 position [[position]]; float2 uv; };
fragment float4 fragment_main(VSOut in [[stage_in]],
                              texture2d<float> src [[texture(0)]],
                              sampler samp [[sampler(0)]],
                              constant float4& uv_rect [[buffer(0)]]) {
    const float2 uv = mix(uv_rect.xy, uv_rect.zw, in.uv);
    return src.sample(samp, uv);
}
)";

    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = k_vs;
    desc.fragment_shader_msl = k_fs;
    desc.vertex_attributes = {
        {0, 0, BackendVertexFormat::Float2, 0},
        {1, 8, BackendVertexFormat::Float2, 0},
    };
    desc.vertex_buffer_strides[0] = 16;
    desc.vertex_stride = 16;
    desc.color_format = format;
    desc.blend_enabled = false;
    desc.color_write_mask = 0xF;
    desc.sample_count = sample_count;
    desc.cache_key = 0x53545200ull ^ static_cast<u64>(key);
    auto pipeline = create_pipeline(desc);
    if (pipeline == nullptr || !pipeline->valid()) return false;
    m_stretch_pipelines[key] = std::move(pipeline);
    return true;
}

bool MetalDevice::stretch_texture_2d_region(
    BackendTexture2D* dst, unsigned dst_x, unsigned dst_y, unsigned dst_width,
    unsigned dst_height, const BackendTexture2D* src, unsigned src_x,
    unsigned src_y, unsigned src_width, unsigned src_height,
    BackendFilter filter, unsigned dst_slice, unsigned src_slice) {
    if (!available() || dst == nullptr || src == nullptr) return false;
    if (!dst->valid() || !src->valid()) return false;
    if (dst == src) return false;
    if (dst_width == 0 || dst_height == 0 || src_width == 0 ||
        src_height == 0) {
        return false;
    }
    const auto& dd = dst->desc();
    const auto& sd = src->desc();
    if (!dd.render_target || !sd.shader_resource) return false;
    if (dd.pixel_format != sd.pixel_format) return false;
    if (src_x + src_width > sd.width || src_y + src_height > sd.height ||
        dst_x + dst_width > dd.width || dst_y + dst_height > dd.height) {
        return false;
    }
    if (src_slice >= sd.array_size || dst_slice >= dd.array_size) {
        return false;
    }
    const unsigned dst_samples = dd.sample_count > 1 ? dd.sample_count : 1;
    const unsigned src_samples = sd.sample_count > 1 ? sd.sample_count : 1;
    if (src_samples > 1) return false;
    if (!ensure_stretch_blit(dd.pixel_format, dst_samples)) return false;
    const u32 key = (dst_samples << 16) | static_cast<u32>(dd.pixel_format);
    BackendPipeline* pipeline = m_stretch_pipelines[key].get();
    if (pipeline == nullptr) return false;

    const bool full = dst_x == 0 && dst_y == 0 && dst_width == dd.width &&
                      dst_height == dd.height;
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = dst;
    pass_desc.color_slices[0] = dst_slice;
    pass_desc.color_array_sizes[0] = 1;
    pass_desc.preserve_color_contents = !full;
    auto pass = create_render_pass_standalone(pass_desc);
    if (pass == nullptr || !pass->valid()) return false;

    const float su0 = static_cast<float>(src_x) / static_cast<float>(sd.width);
    const float sv0 = static_cast<float>(src_y) / static_cast<float>(sd.height);
    const float su1 =
        static_cast<float>(src_x + src_width) / static_cast<float>(sd.width);
    const float sv1 =
        static_cast<float>(src_y + src_height) / static_cast<float>(sd.height);
    const float uv_rect[4] = {su0, sv0, su1, sv1};

    BackendDrawCall call{};
    call.pipeline = pipeline;
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_buffer = m_stretch_vb.get();
    call.vertex_count = 6;
    call.fragment_constants = uv_rect;
    call.fragment_constants_size = sizeof(uv_rect);
    call.textures[0] = src;
    // Cube / array sources are MTLTextureTypeCube or 2DArray. The stretch
    // shader samples texture2d, so bind a one-slice 2D view of src_slice.
    call.texture_views[0].dimension = BackendTextureViewDimension::Texture2D;
    call.texture_views[0].most_detailed_mip = 0;
    call.texture_views[0].mip_levels = 1;
    call.texture_views[0].first_array_slice = src_slice;
    call.texture_views[0].array_size = 1;
    call.samplers[0].min_filter = filter;
    call.samplers[0].mag_filter = filter;
    call.samplers[0].mip_filter = BackendFilter::Nearest;
    call.samplers[0].mipmaps_enabled = false;
    call.samplers[0].address_u = BackendAddressMode::Clamp;
    call.samplers[0].address_v = BackendAddressMode::Clamp;
    call.samplers[0].address_w = BackendAddressMode::Clamp;
    call.viewport = {static_cast<float>(dst_x), static_cast<float>(dst_y),
                     static_cast<float>(dst_width),
                     static_cast<float>(dst_height), 0.0f, 1.0f};
    call.scissor_enabled = true;
    call.scissor = {static_cast<int>(dst_x), static_cast<int>(dst_y),
                    static_cast<int>(dst_x + dst_width),
                    static_cast<int>(dst_y + dst_height)};
    call.cull_mode = BackendCullMode::None;
    call.depth_test_enabled = false;
    call.depth_write_enabled = false;

    const bool drawn = pass->draw(call);
    const bool ended = pass->end();
    return drawn && ended;
}

bool MetalDevice::generate_mips_window(BackendTexture2D* texture,
                                       unsigned first_mip, unsigned mip_count,
                                       unsigned first_slice,
                                       unsigned slice_count) {
    if (!available() || texture == nullptr || !texture->valid()) return false;
    const auto& d = texture->desc();
    if (mip_count == 0 || first_mip >= d.mip_levels ||
        mip_count > d.mip_levels - first_mip || slice_count == 0 ||
        first_slice >= d.array_size ||
        slice_count > d.array_size - first_slice) {
        return false;
    }
    if (mip_count == 1) return true;  // no level below the view's first
    if (first_mip == 0 && mip_count == d.mip_levels && first_slice == 0 &&
        slice_count == d.array_size) {
        return generate_mips(texture);
    }
    // generateMipmapsForTexture fills every level of what it is given from
    // that texture's level 0, so hand it a view that is exactly the window.
    auto* metal_tex = static_cast<MetalBackendTexture2D*>(texture);
    BackendTextureViewDesc view{};
    view.pixel_format = d.pixel_format;
    view.dimension = BackendTextureViewDimension::Texture2DArray;
    view.most_detailed_mip = first_mip;
    view.mip_levels = mip_count;
    view.first_array_slice = first_slice;
    view.array_size = slice_count;
    void* handle = metal_tex->native_shader_handle(view, true);
    if (handle == nullptr) return false;
    MetalSubmitQueue::instance().record_generate_mips(handle);
    return true;
}

bool MetalDevice::generate_mips(BackendTexture2D* texture) {
    if (!available() || texture == nullptr || !texture->valid()) return false;
    if (texture->desc().mip_levels <= 1) return false;
    const auto* metal_tex = static_cast<MetalBackendTexture2D*>(texture);
    void* handle = metal_tex->native_handle();
    if (handle == nullptr) return false;
    MetalSubmitQueue::instance().record_generate_mips(handle);
    return true;
}

bool MetalDevice::generate_mips(BackendTexture3D* texture) {
    if (!available() || texture == nullptr || !texture->valid()) return false;
    if (texture->desc().mip_levels <= 1) return false;
    const auto* metal_tex = static_cast<MetalBackendTexture3D*>(texture);
    void* handle = metal_tex->native_handle();
    if (handle == nullptr) return false;
    MetalSubmitQueue::instance().record_generate_mips(handle);
    return true;
}

namespace {

constexpr const char* kPresentGammaKernel = R"(
#include <metal_stdlib>
using namespace metal;

kernel void present_gamma(
    texture2d<float, access::read> src [[texture(0)]],
    texture2d<float, access::write> dst [[texture(1)]],
    texture2d<float, access::read> lut [[texture(2)]],
    uint2 gid [[thread_position_in_grid]])
{
    if (gid.x >= src.get_width() || gid.y >= src.get_height()) {
        return;
    }
    const float4 c = src.read(gid);
    const uint r = uint(clamp(c.r, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint g = uint(clamp(c.g, 0.0f, 1.0f) * 255.0f + 0.5f);
    const uint b = uint(clamp(c.b, 0.0f, 1.0f) * 255.0f + 0.5f);
    const float4 lr = lut.read(uint2(r, 0));
    const float4 lg = lut.read(uint2(g, 0));
    const float4 lb = lut.read(uint2(b, 0));
    dst.write(float4(lr.r, lg.g, lb.b, c.a), gid);
}
)";

} // namespace

namespace {

/// fill_texels: one texel (already in the texture's byte layout) written
/// through an unsigned-integer view of the same texel size, so the stored
/// bits are exactly the argument's. FillTexelsArgs mirrors the struct.
constexpr const char* kFillTexelsKernel = R"(
#include <metal_stdlib>
using namespace metal;
struct FillTexelsArgs { uint4 texel; uint4 origin; uint4 extent; };
kernel void fill_texels_2d(texture2d_array<uint, access::write> dst [[texture(0)]],
                           constant FillTexelsArgs& a [[buffer(0)]],
                           uint3 id [[thread_position_in_grid]]) {
    if (id.x >= a.extent.x || id.y >= a.extent.y || id.z >= a.extent.z) return;
    dst.write(a.texel, uint2(a.origin.x + id.x, a.origin.y + id.y),
              a.origin.z + id.z);
}
kernel void fill_texels_3d(texture3d<uint, access::write> dst [[texture(0)]],
                           constant FillTexelsArgs& a [[buffer(0)]],
                           uint3 id [[thread_position_in_grid]]) {
    if (id.x >= a.extent.x || id.y >= a.extent.y || id.z >= a.extent.z) return;
    dst.write(a.texel, a.origin.xyz + id);
}
)";

} // namespace

void* MetalDevice::fill_texels_pipeline(bool volume) {
    std::lock_guard<std::mutex> lock(m_fill_mutex);
    if (!m_fill_tried && available()) {
        m_fill_tried = true;
        @autoreleasepool {
            id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
            NSError* error = nil;
            id<MTLLibrary> library =
                [device newLibraryWithSource:@(kFillTexelsKernel)
                                     options:nil
                                       error:&error];
            if (library == nil) {
                std::fprintf(stderr, "[metal] fill_texels shader failed: %s\n",
                             error != nil
                                 ? [[error localizedDescription] UTF8String]
                                 : "unknown error");
                return nullptr;
            }
            const auto build = [&](NSString* name) -> void* {
                id<MTLFunction> function = [library newFunctionWithName:name];
                if (function == nil) return nullptr;
                NSError* pso_error = nil;
                id<MTLComputePipelineState> state =
                    [device newComputePipelineStateWithFunction:function
                                                          error:&pso_error];
                return state != nil ? (__bridge_retained void*)state : nullptr;
            };
            m_fill_pso_2d = build(@"fill_texels_2d");
            m_fill_pso_3d = build(@"fill_texels_3d");
        }
    }
    return volume ? m_fill_pso_3d : m_fill_pso_2d;
}

void MetalDevice::ensure_present_gamma_resources() {
    if (m_gamma_lut != nullptr && m_gamma_compute_pso != nullptr) {
        return;
    }
    if (!available()) {
        return;
    }
    @autoreleasepool {
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        if (m_gamma_lut == nullptr) {
            MTLTextureDescriptor* td = [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                             width:256
                                            height:1
                                         mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModeShared;
            id<MTLTexture> lut = [device newTextureWithDescriptor:td];
            if (lut != nil) {
                m_gamma_lut = (__bridge_retained void*)lut;
            }
        }
        if (m_gamma_compute_pso == nullptr) {
            NSError* error = nil;
            id<MTLLibrary> library =
                [device newLibraryWithSource:@(kPresentGammaKernel)
                                     options:nil
                                       error:&error];
            if (library == nil) {
                std::fprintf(stderr,
                             "[metal] present gamma shader failed: %s\n",
                             error != nil
                                 ? [[error localizedDescription] UTF8String]
                                 : "unknown error");
                return;
            }
            id<MTLFunction> function =
                [library newFunctionWithName:@"present_gamma"];
            if (function == nil) {
                std::fprintf(stderr,
                             "[metal] present gamma entry point missing\n");
                return;
            }
            id<MTLComputePipelineState> state =
                [device newComputePipelineStateWithFunction:function
                                                      error:&error];
            if (state == nil) {
                std::fprintf(stderr,
                             "[metal] present gamma pipeline failed: %s\n",
                             error != nil
                                 ? [[error localizedDescription] UTF8String]
                                 : "unknown error");
                return;
            }
            m_gamma_compute_pso = (__bridge_retained void*)state;
        }
    }
}

void MetalDevice::set_present_gamma_ramp(const u16* rgb_256x3) {
    if (rgb_256x3 == nullptr) {
        m_gamma_active.store(false, std::memory_order_release);
        return;
    }
    ensure_present_gamma_resources();
    if (m_gamma_lut == nullptr || m_gamma_compute_pso == nullptr) {
        m_gamma_active.store(false, std::memory_order_release);
        return;
    }
    std::array<u8, 256 * 4> rgba{};
    for (u32 i = 0; i < 256; ++i) {
        rgba[i * 4 + 0] = static_cast<u8>(rgb_256x3[i] >> 8);
        rgba[i * 4 + 1] = static_cast<u8>(rgb_256x3[256 + i] >> 8);
        rgba[i * 4 + 2] = static_cast<u8>(rgb_256x3[512 + i] >> 8);
        rgba[i * 4 + 3] = 255;
    }
    @autoreleasepool {
        id<MTLTexture> lut = (__bridge id<MTLTexture>)m_gamma_lut;
        const MTLRegion region = MTLRegionMake2D(0, 0, 256, 1);
        [lut replaceRegion:region
               mipmapLevel:0
                 withBytes:rgba.data()
               bytesPerRow:256 * 4];
    }
    m_gamma_active.store(true, std::memory_order_release);
}

bool MetalDevice::present_gamma_active() const noexcept {
    return m_gamma_active.load(std::memory_order_acquire);
}

void* MetalDevice::present_gamma_lut() const noexcept {
    return m_gamma_lut;
}

void* MetalDevice::present_gamma_compute_pso() const noexcept {
    return m_gamma_compute_pso;
}

} // namespace vitreo::graphics

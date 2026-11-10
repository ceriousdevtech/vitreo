// SPDX-License-Identifier: LGPL-2.1-only

/// @file metal_transcode.mm
/// @brief GPU-timeline transcoding between a guest texel encoding and the
/// storage the backend keeps for it (format_422.hpp: DXGI R8G8_B8G8 /
/// G8R8_G8B8 held as decoded RGBA8).
///
/// Buffer to buffer only: a copy into a texture decodes the guest blocks into
/// a scratch buffer of storage texels that the ordinary blit then uploads; a
/// copy out of one blits into scratch and encodes from there. Both are
/// recorded in command order with the copies around them.

#import <Metal/Metal.h>

#include "graphics/metal/metal_transcode.hpp"

#include "graphics/contracts/format_422.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"

#include <cstdio>
#include <cstring>
#include <mutex>

namespace vitreo::graphics {

namespace {

const char* const kTranscodeSource = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Args {
    uint blocks; uint width; uint height; uint depth;
    uint in_row; uint in_slice; uint out_row; uint out_slice;
    uint in_offset; uint out_offset; uint decode; uint pad0;
    uint r; uint g0; uint b; uint g1;
};
kernel void transcode_422(constant Args& a [[buffer(0)]],
                          const device uchar* src [[buffer(1)]],
                          device uchar* dst [[buffer(2)]],
                          uint3 gid [[thread_position_in_grid]]) {
    if (gid.x >= a.blocks || gid.y >= a.height || gid.z >= a.depth) return;
    const uint px = gid.x * 2u;
    if (a.decode != 0u) {
        const device uchar* blk =
            src + a.in_offset + gid.z * a.in_slice + gid.y * a.in_row + gid.x * 4u;
        device uchar* out =
            dst + a.out_offset + gid.z * a.out_slice + gid.y * a.out_row + px * 4u;
        const uchar r = blk[a.r], g0 = blk[a.g0], b = blk[a.b], g1 = blk[a.g1];
        out[0] = r; out[1] = g0; out[2] = b; out[3] = g1;
        if (px + 1u < a.width) { out[4] = r; out[5] = g1; out[6] = b; out[7] = g0; }
        return;
    }
    const device uchar* in =
        src + a.in_offset + gid.z * a.in_slice + gid.y * a.in_row + px * 4u;
    device uchar* blk =
        dst + a.out_offset + gid.z * a.out_slice + gid.y * a.out_row + gid.x * 4u;
    blk[a.r] = in[0]; blk[a.g0] = in[1]; blk[a.b] = in[2]; blk[a.g1] = in[3];
}
)MSL";

struct Args {
    u32 blocks, width, height, depth;
    u32 in_row, in_slice, out_row, out_slice;
    u32 in_offset, out_offset, decode, pad0;
    u32 r, g0, b, g1;
};

class TranscodePrograms {
public:
    static TranscodePrograms& instance() {
        static TranscodePrograms* programs = new TranscodePrograms();
        return *programs;
    }
    void* pipeline(void* device) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pipeline != nullptr || m_failed) return m_pipeline;
        @autoreleasepool {
            NSError* error = nil;
            id<MTLLibrary> library = [(__bridge id<MTLDevice>)device
                newLibraryWithSource:[NSString stringWithUTF8String:kTranscodeSource]
                             options:nil
                               error:&error];
            id<MTLFunction> fn =
                library != nil ? [library newFunctionWithName:@"transcode_422"] : nil;
            id<MTLComputePipelineState> pso =
                fn != nil ? [(__bridge id<MTLDevice>)device
                                newComputePipelineStateWithFunction:fn
                                                              error:&error]
                          : nil;
            if (pso == nil) {
                m_failed = true;
                std::fprintf(stderr,
                             "[metal] 4:2:2 transcode kernel did not compile: %s\n",
                             error ? error.localizedDescription.UTF8String : "unknown");
                return nullptr;
            }
            m_pipeline = (__bridge_retained void*)pso;
        }
        return m_pipeline;
    }

private:
    std::mutex m_mutex;
    void* m_pipeline{nullptr};
    bool m_failed{false};
};

}  // namespace

bool record_transcode_422(PixelFormat format, bool decode, void* src, usize src_offset,
                          void* dst, usize dst_offset, const Transcode422Region& region) {
    void* device = MetalDevice::instance().device_ptr();
    if (device == nullptr || src == nullptr || dst == nullptr || !is_422_storage(format) ||
        region.width == 0 || region.height == 0 || region.depth == 0) {
        return false;
    }
    const u64 limits[] = {region.in_row, region.in_slice, region.out_row, region.out_slice};
    for (const u64 v : limits) {
        if (v > UINT32_MAX) return false;
    }
    void* pipeline = TranscodePrograms::instance().pipeline(device);
    if (pipeline == nullptr) return false;
    const Block422Layout l = block_422_layout(format);
    Args args{};
    args.blocks = (region.width + 1u) / 2u;
    args.width = region.width;
    args.height = region.height;
    args.depth = region.depth;
    args.in_row = static_cast<u32>(region.in_row);
    args.in_slice = static_cast<u32>(region.in_slice);
    args.out_row = static_cast<u32>(region.out_row);
    args.out_slice = static_cast<u32>(region.out_slice);
    args.decode = decode ? 1u : 0u;
    args.r = l.r;
    args.g0 = l.g0;
    args.b = l.b;
    args.g1 = l.g1;
    CsCompute compute{};
    compute.pipeline = pipeline;
    compute.buffers[1] = src;
    compute.buffer_off[1] = src_offset;
    compute.buffers[2] = dst;
    compute.buffer_off[2] = dst_offset;
    compute.constants.resize(sizeof(args));
    std::memcpy(compute.constants.data(), &args, sizeof(args));
    compute.tx = 8;
    compute.ty = 8;
    compute.tz = 1;
    compute.gx = (args.blocks + 7u) / 8u;
    compute.gy = (region.height + 7u) / 8u;
    compute.gz = region.depth;
    MetalSubmitQueue::instance().record_compute(std::move(compute));
    return true;
}

}  // namespace vitreo::graphics

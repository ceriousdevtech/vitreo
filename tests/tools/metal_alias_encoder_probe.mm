// SPDX-License-Identifier: LGPL-2.1-only
// Standalone synchronization discriminator on macOS:
// clang++ -std=c++20 -fobjc-arc -framework Foundation -framework Metal \
//   tests/tools/metal_alias_encoder_probe.mm -o /tmp/metal-alias-probe
// /tmp/metal-alias-probe
//
// Microsoft Learn, "Memory Aliasing and Data Inheritance": buffers can inherit
// data regardless of transition state; an aliasing barrier orders the accesses.
// This probe shows why tracked heaps alone do not supply that order WITHIN a
// render encoder. The split arm represents our AttachmentBarrier boundary and
// must have zero wrong pixels. The unsplit arm is deliberately unordered; its
// wrong-pixel count is diagnostic, not a portable expected value.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <vector>

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) return 2;
        NSError* error = nil;
        NSString* source = @R"(
#include <metal_stdlib>
using namespace metal;
vertex float4 vs(uint id [[vertex_id]]) {
    return float4(id == 1 ? 3.0 : -1.0, id == 2 ? 3.0 : -1.0, 0, 1);
}
fragment float4 writeA(float4 pos [[position]], device uint* a [[buffer(0)]]) {
    uint at = uint(pos.y) * 512u + uint(pos.x);
    a[at] = at + 0x10001u;
    return float4(0);
}
fragment float4 readB(float4 pos [[position]], device const uint* b [[buffer(0)]]) {
    uint at = 512u * 512u - 1u - (uint(pos.y) * 512u + uint(pos.x));
    return b[at] == at + 0x10001u ? float4(0, 1, 0, 1) : float4(1, 0, 0, 1);
}
)";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        if (!library) { std::puts(error.description.UTF8String); return 3; }
        id<MTLRenderPipelineState> pipelines[2];
        for (int i = 0; i < 2; ++i) {
            auto* descriptor = [MTLRenderPipelineDescriptor new];
            descriptor.vertexFunction = [library newFunctionWithName:@"vs"];
            descriptor.fragmentFunction = [library newFunctionWithName:i ? @"readB" : @"writeA"];
            descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
            pipelines[i] = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
            if (!pipelines[i]) { std::puts(error.description.UTF8String); return 4; }
        }
        auto* heap_descriptor = [MTLHeapDescriptor new];
        heap_descriptor.type = MTLHeapTypePlacement;
        heap_descriptor.storageMode = MTLStorageModePrivate;
        heap_descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
        heap_descriptor.size = [device heapBufferSizeAndAlignWithLength:512 * 512 * 4
                                                               options:MTLResourceStorageModePrivate].size;
        id<MTLHeap> heap = [device newHeapWithDescriptor:heap_descriptor];
        id<MTLBuffer> a = [heap newBufferWithLength:512 * 512 * 4
                                           options:MTLResourceStorageModePrivate offset:0];
        id<MTLBuffer> b = [heap newBufferWithLength:512 * 512 * 4
                                           options:MTLResourceStorageModePrivate offset:0];
        if (!a || !b) return 5;
        auto* texture_descriptor = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                        width:512 height:512 mipmapped:NO];
        texture_descriptor.usage = MTLTextureUsageRenderTarget;
        texture_descriptor.storageMode = MTLStorageModeShared;
        id<MTLTexture> target = [device newTextureWithDescriptor:texture_descriptor];
        id<MTLCommandQueue> queue = [device newCommandQueue];
        std::vector<unsigned char> pixels(512 * 512 * 4);
        for (int trial = 0; trial < 8; ++trial) {
            for (int split = 0; split < 2; ++split) {
                id<MTLCommandBuffer> command = [queue commandBuffer];
                id<MTLBlitCommandEncoder> clear = [command blitCommandEncoder];
                [clear fillBuffer:a range:NSMakeRange(0, a.length) value:0];
                [clear endEncoding];
                auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
                [encoder setRenderPipelineState:pipelines[0]];
                [encoder setFragmentBuffer:a offset:0 atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                if (split) {
                    [encoder endEncoding];
                    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
                    encoder = [command renderCommandEncoderWithDescriptor:pass];
                }
                [encoder setRenderPipelineState:pipelines[1]];
                [encoder setFragmentBuffer:b offset:0 atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [encoder endEncoding];
                [command commit];
                [command waitUntilCompleted];
                if (command.status == MTLCommandBufferStatusError) {
                    std::puts(command.error.description.UTF8String);
                    return 6;
                }
                [target getBytes:pixels.data() bytesPerRow:512 * 4
                      fromRegion:MTLRegionMake2D(0, 0, 512, 512) mipmapLevel:0];
                unsigned wrong = 0;
                for (unsigned at = 0; at < 512 * 512; ++at) {
                    wrong += pixels[at * 4] != 0 || pixels[at * 4 + 1] != 255;
                }
                std::printf("trial%d split%d wrongPixels=%u\n", trial, split, wrong);
                if (split && wrong) return 7;
            }
        }
        return 0;
    }
}

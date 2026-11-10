// SPDX-License-Identifier: LGPL-2.1-only
#import <Metal/Metal.h>
#include <gtest/gtest.h>
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include <array>

using namespace vitreo;
using namespace vitreo::graphics;

@interface VitreoBindingEncoder : NSObject
@property(nonatomic, strong) id<MTLRenderCommandEncoder> target;
@property(nonatomic) NSUInteger textureCalls;
@property(nonatomic) NSUInteger samplerCalls;
@end

@implementation VitreoBindingEncoder
- (id)forwardingTargetForSelector:(SEL)selector {
    return self.target;
}
- (void)setVertexTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index {
    ++_textureCalls;
    [self.target setVertexTexture:texture atIndex:index];
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)sampler
                     atIndex:(NSUInteger)index {
    ++_samplerCalls;
    [self.target setVertexSamplerState:sampler atIndex:index];
}
@end

@interface VitreoBindingBuffer : NSObject
@property(nonatomic, strong) id<MTLCommandBuffer> target;
@property(nonatomic, strong) NSMutableArray<VitreoBindingEncoder*>* encoders;
@end

@implementation VitreoBindingBuffer
- (id)forwardingTargetForSelector:(SEL)selector {
    return self.target;
}
- (id)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)descriptor {
    VitreoBindingEncoder* encoder = [VitreoBindingEncoder new];
    encoder.target = [self.target renderCommandEncoderWithDescriptor:descriptor];
    if (encoder.target == nil) return nil;
    [self.encoders addObject:encoder];
    return encoder;
}
@end

@interface VitreoBindingQueue : NSObject
@property(nonatomic, strong) id<MTLCommandQueue> target;
@property(nonatomic, strong) NSMutableArray<VitreoBindingEncoder*>* encoders;
@end

@implementation VitreoBindingQueue
- (id)forwardingTargetForSelector:(SEL)selector {
    return self.target;
}
- (id)commandBuffer {
    VitreoBindingBuffer* buffer = [VitreoBindingBuffer new];
    buffer.target = [self.target commandBuffer];
    buffer.encoders = self.encoders;
    return buffer;
}
@end

class MetalVertexBindings : public testing::Test {
protected:
    void SetUp() override {
        auto& backend = MetalDevice::instance();
        ASSERT_TRUE(backend.available());
        device = (__bridge id<MTLDevice>)backend.device_ptr();
        queue = [VitreoBindingQueue new];
        queue.target = (__bridge id<MTLCommandQueue>)backend.command_queue_ptr();
        queue.encoders = [NSMutableArray new];
        auto* desc = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                        width:8 height:1 mipmapped:NO];
        desc.storageMode = MTLStorageModeShared;
        desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        output = [device newTextureWithDescriptor:desc];
        ASSERT_NE(output, nil);
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = output;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }

    id<MTLDevice> device;
    VitreoBindingQueue* queue;
    id<MTLTexture> output;
    MTLRenderPassDescriptor* pass;
};

TEST_F(MetalVertexBindings, RepeatedBindingsAreElidedPerSlotAndEncoder) {
    MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:desc];
    ASSERT_NE(sampler, nil);
    MetalCommandRecorder recorder((__bridge void*)queue);
    recorder.begin();
    recorder.begin_render_pass((__bridge void*)pass);
    for (u32 draw = 0; draw < 1024; ++draw) {
        for (u32 slot = 0; slot < 16; ++slot) {
            recorder.set_vertex_texture((__bridge void*)output, slot);
            recorder.set_vertex_sampler((__bridge void*)sampler, slot);
        }
    }
    ASSERT_EQ(queue.encoders.count, 1u);
    EXPECT_EQ(queue.encoders[0].textureCalls, 16u);
    EXPECT_EQ(queue.encoders[0].samplerCalls, 16u);
    recorder.end_render_pass();
    recorder.begin_render_pass((__bridge void*)pass);
    for (u32 slot = 0; slot < 16; ++slot) {
        recorder.set_vertex_texture((__bridge void*)output, slot);
        recorder.set_vertex_sampler((__bridge void*)sampler, slot);
    }
    ASSERT_EQ(queue.encoders.count, 2u);
    EXPECT_EQ(queue.encoders[1].textureCalls, 16u);
    EXPECT_EQ(queue.encoders[1].samplerCalls, 16u);
    // Slots outside the cache must still reach Metal.
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        recorder.set_vertex_texture((__bridge void*)output, 16);
    }
    EXPECT_EQ(queue.encoders[1].textureCalls, 18u);
    EXPECT_EQ(queue.encoders[1].samplerCalls, 16u);
    recorder.commit_and_wait();
}

TEST_F(MetalVertexBindings, ChangedNullAndRestartedBindingsReachTheShader) {
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
         "struct V { float4 position [[position]]; float4 color; };\n"
         "vertex V vs(uint i [[vertex_id]], texture2d<float> t [[texture(0)]],"
         "sampler s [[sampler(0)]]) {\n"
         "float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
         "return {float4(p[i],0,1),t.sample(s,float2(0.5),level(0))}; }\n"
         "fragment float4 ps(V v [[stage_in]]) { return v.color; }\n"
        options:nil error:&error];
    ASSERT_NE(library, nil) << error.localizedDescription.UTF8String;
    MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [library newFunctionWithName:@"vs"];
    pd.fragmentFunction = [library newFunctionWithName:@"ps"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:pd error:&error];
    ASSERT_NE(pipeline, nil) << error.localizedDescription.UTF8String;
    auto* td = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                    width:2 height:1 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    id<MTLTexture> redGreen = [device newTextureWithDescriptor:td];
    id<MTLTexture> blue = [device newTextureWithDescriptor:td];
    ASSERT_NE(redGreen, nil);
    ASSERT_NE(blue, nil);
    const std::array<u8, 8> rg{255, 0, 0, 255, 0, 255, 0, 255};
    const std::array<u8, 8> bb{0, 0, 255, 255, 0, 0, 255, 255};
    [redGreen replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0
                 withBytes:rg.data() bytesPerRow:8];
    [blue replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0
             withBytes:bb.data() bytesPerRow:8];
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    id<MTLSamplerState> nearest = [device newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = MTLSamplerAddressModeRepeat;
    id<MTLSamplerState> linear = [device newSamplerStateWithDescriptor:sd];
    ASSERT_NE(nearest, nil);
    ASSERT_NE(linear, nil);
    MetalCommandRecorder recorder((__bridge void*)queue);
    const auto beginPass = [&] {
        recorder.begin_render_pass((__bridge void*)pass);
        recorder.set_render_pipeline((__bridge void*)pipeline);
        recorder.set_viewport(0, 0, 8, 1, 0, 1);
    };
    const auto draw = [&](u32 pixel, id<MTLTexture> texture,
                          id<MTLSamplerState> sampler) {
        recorder.set_vertex_texture((__bridge void*)texture, 0);
        recorder.set_vertex_sampler((__bridge void*)sampler, 0);
        recorder.set_scissor(pixel, 0, 1, 1);
        recorder.draw(MTLPrimitiveTypeTriangle, 3, 0);
    };
    recorder.begin();
    beginPass();
    draw(0, redGreen, nearest);
    draw(1, redGreen, nearest);
    draw(2, redGreen, linear);
    draw(3, blue, linear);
    draw(4, nil, linear);
    draw(5, redGreen, nil);
    draw(6, redGreen, nearest);
    recorder.end_render_pass();
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    beginPass();
    draw(7, redGreen, nearest);
    recorder.commit_and_wait();
    std::array<u8, 32> pixels{};
    [output getBytes:pixels.data() bytesPerRow:pixels.size()
          fromRegion:MTLRegionMake2D(0, 0, 8, 1) mipmapLevel:0];
    const std::array<std::array<unsigned, 4>, 8> expected{{
        {0, 255, 0, 255}, {0, 255, 0, 255}, {128, 128, 0, 255},
        {0, 0, 255, 255}, {0, 0, 0, 0}, {128, 128, 0, 255},
        {0, 255, 0, 255}, {0, 255, 0, 255}}};
    for (usize pixel = 0; pixel < expected.size(); ++pixel) {
        for (usize channel = 0; channel < 4; ++channel) {
            EXPECT_NEAR(pixels[pixel * 4 + channel], expected[pixel][channel], 1)
                << "pixel " << pixel << " channel " << channel;
        }
    }
    ASSERT_EQ(queue.encoders.count, 2u);
    EXPECT_EQ(queue.encoders[0].textureCalls, 4u);
    EXPECT_EQ(queue.encoders[0].samplerCalls, 4u);
    EXPECT_EQ(queue.encoders[1].textureCalls, 1u);
    EXPECT_EQ(queue.encoders[1].samplerCalls, 1u);
}

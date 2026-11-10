// SPDX-License-Identifier: LGPL-2.1-only
#import <Metal/Metal.h>
#include <gtest/gtest.h>
#include "graphics/metal/metal_command.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include <future>
#include <thread>

using namespace vitreo;
using namespace vitreo::graphics;

TEST(ComputeBatch, SerialRecorderDispatchesPublishBufferWrites) {
    auto& backend = MetalDevice::instance();
    ASSERT_TRUE(backend.available());
    id<MTLDevice> device = (__bridge id<MTLDevice>)backend.device_ptr();
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:@"#include <metal_stdlib>\n"
        "using namespace metal; kernel void twice(device uint* v [[buffer(0)]],"
        "uint i [[thread_position_in_grid]]) { v[i] *= 2u; }"
        options:nil error:&error];
    ASSERT_NE(library, nil) << error.localizedDescription.UTF8String;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:
        [library newFunctionWithName:@"twice"] error:&error];
    ASSERT_NE(pipeline, nil) << error.localizedDescription.UTF8String;
    id<MTLBuffer> values = [device newBufferWithLength:1024 * sizeof(u32)
                                           options:MTLResourceStorageModeShared];
    ASSERT_NE(values, nil);
    auto* numbers = static_cast<u32*>(values.contents);
    for (unsigned i = 0; i < 1024; ++i) numbers[i] = 1;
    MetalCommandRecorder recorder(backend.command_queue_ptr());
    recorder.begin();
    recorder.begin_compute();
    recorder.set_compute_pipeline((__bridge void*)pipeline);
    recorder.set_compute_buffer((__bridge void*)values, 0, 0);
    for (unsigned n = 0; n < 30; ++n) {
        if (n != 0) recorder.memory_barrier_compute();
        recorder.dispatch(16, 1, 1, 64, 1, 1);
    }
    recorder.end_compute();
    recorder.commit_and_wait();
    // Thirty doublings of one, not a result obtained from the implementation.
    for (unsigned i = 0; i < 1024; ++i) EXPECT_EQ(numbers[i], 1073741824u);
}

TEST(ComputeBatch, EmptySubmitDoesNotWaitForGpuListCapacity) {
    auto& backend = MetalDevice::instance();
    ASSERT_TRUE(backend.available());
    auto& queue = MetalSubmitQueue::instance();
    queue.drain();
    id<MTLDevice> device = (__bridge id<MTLDevice>)backend.device_ptr();
    id<MTLSharedEvent> gate = [device newSharedEvent];
    id<MTLBuffer> buffer = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    ASSERT_NE(gate, nil);
    ASSERT_NE(buffer, nil);
    gate.signaledValue = 0;
    std::atomic<bool> released{false};
    std::thread safety([&] {
        for (unsigned n = 0; n < 100; ++n) {
            if (released.load(std::memory_order_acquire)) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        gate.signaledValue = 1;
    });
    std::atomic<unsigned> encoded{0};
    unsigned queued = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    const auto handoff = [&] {
        const unsigned marker = ++queued;
        queue.record_encode([&, marker](void*) {
            encoded.store(marker, std::memory_order_release);
        });
        const u64 generation = queue.submit();
        while (encoded.load(std::memory_order_acquire) < marker &&
               std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return generation;
    };
    queue.record_wait_fence((__bridge void*)gate, 1);
    u64 last = handoff();
    // The CPU marker establishes encoding progress independently of GPU
    // completion (wait_encoded also waits for GPU completion). Saturation
    // below is the GPU bound, not transient pending work.
    unsigned submitted = 0;
    while (!queue.submit_would_block() && submitted < 128 &&
           std::chrono::steady_clock::now() < deadline) {
        queue.record_fill_buffer((__bridge void*)buffer, 0, 4, 0x42);
        last = handoff();
        ++submitted;
    }
    const bool saturated = queue.submit_would_block();
    auto empty = std::async(std::launch::async, [&] { return queue.submit(); });
    const bool returned = empty.wait_for(std::chrono::milliseconds(150)) ==
                          std::future_status::ready;
    const u64 completed_while_blocked = queue.completed_generation();
    // Always unblock before assertions or future destruction, including RED.
    gate.signaledValue = 1;
    released.store(true, std::memory_order_release);
    safety.join();
    const u64 empty_generation = empty.get();
    queue.drain();
    EXPECT_TRUE(saturated);
    EXPECT_TRUE(returned) << "an empty submit waited for unrelated GPU work";
    EXPECT_LT(completed_while_blocked, last);
    EXPECT_EQ(empty_generation, last);
}

TEST(ComputeBatch, QueueAdjacentDispatchesPublishTheirWrites) {
    auto& device = MetalDevice::instance();
    ASSERT_TRUE(device.available());
    BackendComputePipelineDesc pipeline_desc{};
    pipeline_desc.compute_entry_point = "twice";
    pipeline_desc.compute_shader_msl = "#include <metal_stdlib>\nusing namespace metal; "
        "kernel void twice(device uint* values [[buffer(0)]], uint i [[thread_position_in_grid]]) { values[i] *= 2u; }";
    auto pipeline = device.create_compute_pipeline(pipeline_desc);
    ASSERT_TRUE(pipeline && pipeline->valid());
    std::vector<u32> words(1024, 1u);
    BackendBufferDesc buffer_desc{};
    buffer_desc.byte_width = words.size() * sizeof(u32); buffer_desc.unordered_access = true;
    auto buffer = device.create_buffer(buffer_desc, words.data());
    ASSERT_TRUE(buffer && buffer->valid());
    BackendDispatchCall call{};
    call.pipeline = pipeline.get(); call.buffers[0] = buffer.get();
    call.group_count_x = 16; call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 64;
    for (unsigned n = 0; n < 30; ++n) ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    ASSERT_TRUE(buffer->read(0, words.data(), words.size() * sizeof(u32)));
    for (u32 value : words) EXPECT_EQ(value, 1073741824u);
}

#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_backend_buffer.hpp"
#include <array>
#include <cstring>

namespace {
std::unique_ptr<BackendComputePipeline> batch_pipeline(const std::string& body) {
    BackendComputePipelineDesc d{}; d.compute_entry_point = "main0";
    d.compute_shader_msl = "#include <metal_stdlib>\nusing namespace metal;\n" + body;
    static u64 key = 0xCBB00000u; d.cache_key = ++key;
    return MetalDevice::instance().create_compute_pipeline(d);
}
std::unique_ptr<BackendBuffer> batch_buffer(const std::vector<u32>& initial) {
    BackendBufferDesc d{}; d.byte_width = initial.size() * sizeof(u32);
    d.unordered_access = d.shader_resource = true;
    return MetalDevice::instance().create_buffer(d, initial.data());
}
BackendDispatchCall batch_call(const BackendComputePipeline* pipeline, unsigned groups = 1) {
    BackendDispatchCall c{}; c.pipeline = pipeline;
    c.group_count_x = groups; c.group_count_y = c.group_count_z = 1;
    return c;
}
}

TEST(ComputeBatch, QueuePreservesRawWarAndWawAcrossBuffers) {
    auto& d = MetalDevice::instance(); ASSERT_TRUE(d.available());
    auto copy = batch_pipeline("kernel void main0(device uint* dst [[buffer(0)]], const device uint* src [[buffer(1)]], uint i [[thread_position_in_grid]]) { dst[i] = src[i]; }");
    auto twice = batch_pipeline("kernel void main0(device uint* v [[buffer(0)]], uint i [[thread_position_in_grid]]) { v[i] *= 2u; }");
    ASSERT_TRUE(copy && twice);
    std::vector<u32> ones(4096, 1u), zeros(4096, 0u);
    auto a = batch_buffer(ones), b = batch_buffer(zeros), c = batch_buffer(zeros);
    ASSERT_TRUE(a && b && c);
    auto op = batch_call(copy.get(), 4096); op.buffers[0] = b.get(); op.buffers[1] = a.get();
    ASSERT_TRUE(d.dispatch(op)); // B reads the old A, before A is overwritten.
    op = batch_call(twice.get(), 4096); op.buffers[0] = a.get(); ASSERT_TRUE(d.dispatch(op));
    op = batch_call(copy.get(), 4096); op.buffers[0] = c.get(); op.buffers[1] = b.get(); ASSERT_TRUE(d.dispatch(op));
    op = batch_call(twice.get(), 4096); op.buffers[0] = a.get(); ASSERT_TRUE(d.dispatch(op));
    ASSERT_TRUE(d.wait_idle());
    ASSERT_TRUE(a->read(0, zeros.data(), zeros.size() * sizeof(u32)));
    for (u32 value : zeros) EXPECT_EQ(value, 4u);
    ASSERT_TRUE(c->read(0, zeros.data(), zeros.size() * sizeof(u32)));
    for (u32 value : zeros) EXPECT_EQ(value, 1u);
}

TEST(ComputeBatch, QueueOrdersTextureViewAliasesBeforeLaterWrites) {
    auto& d = MetalDevice::instance(); ASSERT_TRUE(d.available());
    auto red = batch_pipeline("kernel void main0(texture2d<float, access::write> dst [[texture(0)]], uint2 p [[thread_position_in_grid]]) { dst.write(float4(1,0,0,1),p); }");
    auto green = batch_pipeline("kernel void main0(texture2d<float, access::write> dst [[texture(0)]], uint2 p [[thread_position_in_grid]]) { dst.write(float4(0,1,0,1),p); }");
    auto copy = batch_pipeline("kernel void main0(texture2d<float, access::read> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]], uint2 p [[thread_position_in_grid]]) { dst.write(src.read(p),p); }");
    ASSERT_TRUE(red && green && copy);
    BackendTexture2DDesc td{}; td.width = td.height = 8; td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm; td.shader_resource = td.unordered_access = true;
    auto source = d.create_texture_2d(td, nullptr, 0), result = d.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(source && result);
    auto op = batch_call(red.get(), 8); op.group_count_y = 8; op.textures[0] = source.get(); ASSERT_TRUE(d.dispatch(op));
    op = batch_call(copy.get(), 8); op.group_count_y = 8; op.textures[0] = source.get(); op.textures[1] = result.get();
    op.texture_views[0].swizzle = BackendViewSwizzle::GBAR; ASSERT_TRUE(d.dispatch(op));
    op = batch_call(green.get(), 8); op.group_count_y = 8; op.textures[0] = source.get(); ASSERT_TRUE(d.dispatch(op));
    std::array<u8, 8 * 8 * 4> bytes{}; ASSERT_TRUE(result->readback(0, bytes.data(), 8 * 4));
    for (usize i = 0; i < bytes.size(); i += 4) {
        EXPECT_EQ(bytes[i], 0u); EXPECT_EQ(bytes[i+1], 0u); EXPECT_EQ(bytes[i+2], 255u); EXPECT_EQ(bytes[i+3], 255u);
    }
}

TEST(ComputeBatch, QueueComputeWrittenIndirectArgumentsAreVisible) {
    auto& d = MetalDevice::instance(); ASSERT_TRUE(d.available());
    auto args_writer = batch_pipeline("kernel void main0(device uint* args [[buffer(0)]]) { args[0]=3; args[1]=1; args[2]=1; }");
    auto writer = batch_pipeline("kernel void main0(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) { out[i]=i+1; }");
    ASSERT_TRUE(args_writer && writer);
    auto args = batch_buffer({0,0,0}), output = batch_buffer({0,0,0}); ASSERT_TRUE(args && output);
    id<MTLBuffer> native_args = (__bridge id<MTLBuffer>)static_cast<const MetalBackendBuffer&>(*args).native_handle();
    id<MTLBuffer> native_output = (__bridge id<MTLBuffer>)static_cast<const MetalBackendBuffer&>(*output).native_handle();
    std::fprintf(stderr, "[backend-indirect] args_hazard=%lu output_hazard=%lu\n",
        static_cast<unsigned long>(native_args.hazardTrackingMode), static_cast<unsigned long>(native_output.hazardTrackingMode));
    auto op = batch_call(args_writer.get()); op.buffers[0] = args.get(); ASSERT_TRUE(d.dispatch(op));
    op = batch_call(writer.get()); op.buffers[0] = output.get(); op.indirect.buffer = args.get(); ASSERT_TRUE(d.dispatch(op));
    ASSERT_TRUE(d.wait_idle()); std::array<u32,3> words{};
    ASSERT_TRUE(output->read(0, words.data(), sizeof(words)));
    EXPECT_EQ(words, (std::array<u32,3>{1,2,3}));
}

TEST(ComputeBatch, QueueClearsUnusedBindingsAndPreservesEffectivePrecedence) {
    auto& backend = MetalDevice::instance(); ASSERT_TRUE(backend.available());
    id<MTLDevice> device = (__bridge id<MTLDevice>)backend.device_ptr();
    const auto pipeline = [&](const std::string& body) -> id<MTLComputePipelineState> {
        NSString* text = [NSString stringWithUTF8String:("#include <metal_stdlib>\nusing namespace metal;\n"+body).c_str()];
        NSError* error = nil; id<MTLLibrary> lib = [device newLibraryWithSource:text options:nil error:&error];
        EXPECT_NE(lib,nil) << error.localizedDescription.UTF8String;
        return lib ? [device newComputePipelineStateWithFunction:[lib newFunctionWithName:@"main0"] error:&error] : nil;
    };
    id<MTLComputePipelineState> inline_reader = pipeline("kernel void main0(constant uint& x [[buffer(0)]], device uint* out [[buffer(14)]]) {out[0]=x;}");
    id<MTLComputePipelineState> direct_reader = pipeline("kernel void main0(const device uint* x [[buffer(0)]], device uint* out [[buffer(14)]]) {out[1]=x[0];}");
    const std::string slot = std::to_string(BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE);
    id<MTLComputePipelineState> table_to_direct = pipeline("kernel void main0(const device uint* x [[buffer("+slot+")]], device uint* out [[buffer(14)]]) {out[3]=x[0];}");
    id<MTLComputePipelineState> noop = pipeline("kernel void main0() {}");
    ASSERT_TRUE(inline_reader && direct_reader && table_to_direct && noop);
    id<MTLBuffer> input = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    id<MTLBuffer> dummy_table = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    *static_cast<u32*>(input.contents)=7; std::memset(output.contents,0,256);
    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
    td.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> texture=[device newTextureWithDescriptor:td];
    id<MTLSamplerState> sampler=[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    auto make = [&](id<MTLComputePipelineState> p) {
        CsCompute op{}; op.pipeline=(__bridge void*)p; op.gx=op.gy=op.gz=1;
        op.buffers[14]=(__bridge void*)output; return op;
    };
    auto& queue=MetalSubmitQueue::instance();
    auto op=make(inline_reader); op.buffers[0]=(__bridge void*)input;
    op.textures[0]=(__bridge void*)texture; op.samplers[0]=(__bridge void*)sampler;
    const u32 eleven=11; op.constants.resize(sizeof(eleven)); std::memcpy(op.constants.data(),&eleven,sizeof(eleven));
    queue.record_compute(std::move(op));
    op=make(direct_reader); op.buffers[0]=(__bridge void*)input; queue.record_compute(std::move(op));
    // Active unbound buffer arguments are rejected by Metal validation even
    // when the kernel checks nullptr. Exercise clearing with an inactive slot.
    op=make(noop); queue.record_compute(std::move(op));
    op=make(noop);
    // descriptor_tables/indirect/resident ownership is transferred by the
    // frontend; ordinary buffer/texture/sampler handles are retained by record_compute.
    op.descriptor_tables[0]=(__bridge_retained void*)dummy_table; queue.record_compute(std::move(op));
    op=make(table_to_direct); op.buffers[BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE]=(__bridge void*)input; queue.record_compute(std::move(op));
    ASSERT_TRUE(backend.wait_idle());
    const auto* words=static_cast<const u32*>(output.contents);
    EXPECT_EQ(words[0],11u); EXPECT_EQ(words[1],7u); EXPECT_EQ(words[2],0u);
    EXPECT_EQ(words[3],7u); EXPECT_EQ(words[4],0u);
}

TEST(ComputeBatch, QueueBudgetInvalidPipelineAndListBoundariesKeepState) {
    auto& d=MetalDevice::instance(); ASSERT_TRUE(d.available());
    auto increment=batch_pipeline("kernel void main0(device uint* out [[buffer(0)]]) {++out[0];}");
    ASSERT_TRUE(increment); auto output=batch_buffer({0}); ASSERT_TRUE(output);
    auto op=batch_call(increment.get()); op.buffers[0]=output.get();
    for (unsigned i=0;i<600;++i) ASSERT_TRUE(d.dispatch(op));
    MetalSubmitQueue::instance().record_compute(CsCompute{}); // invalid pipeline is a boundary
    ASSERT_TRUE(d.dispatch(op)); ASSERT_TRUE(d.wait_idle()); // end-list closes
    ASSERT_TRUE(d.dispatch(op)); ASSERT_TRUE(d.wait_idle());
    u32 value=0; ASSERT_TRUE(output->read(0,&value,sizeof(value))); EXPECT_EQ(value,602u);
}

TEST(ComputeBatch, IndirectArgumentsSurviveBudgetCrossListAndTimestampBoundaries) {
    auto& d=MetalDevice::instance(); ASSERT_TRUE(d.available());
    auto args_writer=batch_pipeline("kernel void main0(device uint* a [[buffer(0)]]) {a[0]=3;a[1]=1;a[2]=1;}");
    auto consumer=batch_pipeline("kernel void main0(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]]) {out[i]=i+1;}");
    auto noop=batch_pipeline("kernel void main0() {}"); ASSERT_TRUE(args_writer && consumer && noop);
    for (unsigned boundary=0;boundary<3;++boundary) {
        SCOPED_TRACE(boundary);
        auto args=batch_buffer({0,0,0}), output=batch_buffer({0,0,0}); ASSERT_TRUE(args && output);
        ASSERT_TRUE(d.wait_idle()); // begin with an empty list and reset logical budget
        if (boundary==0) {
            auto empty=batch_call(noop.get());
            for (unsigned i=0;i<255;++i) ASSERT_TRUE(d.dispatch(empty));
        }
        auto op=batch_call(args_writer.get()); op.buffers[0]=args.get(); ASSERT_TRUE(d.dispatch(op));
        if (boundary==1) d.flush(); // publish producer list; do not wait for GPU
        auto point=boundary==2 ? d.record_timestamp() : decltype(d.record_timestamp()){};
        if (boundary==2) ASSERT_TRUE(point);
        op=batch_call(consumer.get()); op.buffers[0]=output.get(); op.indirect.buffer=args.get(); ASSERT_TRUE(d.dispatch(op));
        ASSERT_TRUE(d.wait_idle());
        std::array<u32,3> words{}; ASSERT_TRUE(output->read(0,words.data(),sizeof(words)));
        EXPECT_EQ(words,(std::array<u32,3>{1,2,3}));
        if (point) EXPECT_TRUE(point->ready());
    }
}

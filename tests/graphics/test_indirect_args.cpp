// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_indirect_args.cpp
/// @brief GPU-sourced draw and dispatch arguments, end to end on real Metal.
///
/// The whole point of an indirect call is that the CPU never learns the counts:
/// the GPU wrote them and the GPU consumes them. A test that only checks "the
/// call returned true" cannot tell a working implementation from one that
/// quietly used the CPU-side counts it was handed.
///
/// So every test here sets the CPU-side counts to values that DISAGREE with the
/// argument buffer, and asserts the result matches the buffer. If the
/// implementation ever falls back to the struct fields, these fail.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

using namespace vitreo::graphics;

namespace {

/// D3D12_DISPATCH_ARGUMENTS / MTLDispatchThreadgroupsIndirectArguments.
struct DispatchArgs {
    std::uint32_t x, y, z;
};

/// D3D12_DRAW_ARGUMENTS / MTLDrawPrimitivesIndirectArguments.
struct DrawArgs {
    std::uint32_t vertex_count;
    std::uint32_t instance_count;
    std::uint32_t start_vertex;
    std::uint32_t start_instance;
};

[[nodiscard]] std::unique_ptr<BackendBuffer> make_args_buffer(
    GraphicsBackendDevice& device, const void* data, std::size_t size) {
    BackendBufferDesc desc{};
    desc.byte_width = size;
    // An indirect-argument buffer is written by the GPU in real use, which in
    // D3D means a UAV. Nothing here depends on that: Metal reads the argument
    // block off any buffer.
    desc.unordered_access = true;
    return device.create_buffer(desc, data);
}

} // namespace

// The dispatch writes one texel per thread at thread_position_in_grid, with one
// thread per threadgroup, so the written region is exactly the threadgroup
// count. The CPU-side counts say 4x4; the buffer says 2x1. A 2x1 written region
// proves the buffer won.
TEST(IndirectArgs, DispatchTakesItsGroupCountFromTheBufferNotTheCall) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.unordered_access = true;
    td.render_target = true; // so the untouched texels have a known value
    auto texture = device.create_texture_2d(td, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->valid());
    ASSERT_TRUE(texture->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(
            texture2d<float, access::write> u0 [[texture(8)]],
            uint3 id [[thread_position_in_grid]]) {
            if (id.x < u0.get_width() && id.y < u0.get_height()) {
                u0.write(float4(1.0, 0.0, 0.0, 1.0), id.xy);
            }
        }
    )";
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    const DispatchArgs args{2, 1, 1};
    auto args_buffer = make_args_buffer(device, &args, sizeof(args));
    ASSERT_NE(args_buffer, nullptr);
    ASSERT_TRUE(args_buffer->valid());

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    // Deliberately wrong. If these are used, the whole 4x4 texture turns red.
    call.group_count_x = 4;
    call.group_count_y = 4;
    call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.textures[8] = texture.get();
    call.indirect.buffer = args_buffer.get();
    call.indirect.offset = 0;
    ASSERT_TRUE(device.dispatch(call));

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(texture->readback(0, pixels.data(), 16));
    for (unsigned y = 0; y < 4; ++y) {
        for (unsigned x = 0; x < 4; ++x) {
            const std::size_t i = (y * 4u + x) * 4u;
            const bool written = x < 2 && y < 1;
            EXPECT_EQ(pixels[i], written ? 255u : 0u)
                << "texel (" << x << "," << y << ") red channel";
        }
    }
}

// Same shape for the graphics pipeline: the buffer says draw zero vertices, the
// call struct says three. A target still holding its clear colour proves the
// vertex count came from the buffer.
TEST(IndirectArgs, DrawTakesItsVertexCountFromTheBufferNotTheCall) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    BackendPipelineDesc pdesc{};
    pdesc.vertex_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float2 position [[attribute(0)]]; };
        struct main0_out { float4 position [[position]]; };
        vertex main0_out main0(main0_in in [[stage_in]]) {
            main0_out out = {};
            out.position = float4(in.position, 0.0, 1.0);
            return out;
        }
    )";
    pdesc.fragment_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 main0() { return float4(0.0, 1.0, 0.0, 1.0); }
    )";
    pdesc.vertex_entry_point = "main0";
    pdesc.fragment_entry_point = "main0";
    pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
    pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
    pdesc.color_format = PixelFormat::BGRA8Unorm;
    pdesc.cache_key = 0x494e4449524543ull;
    auto pipeline = device.create_pipeline(pdesc);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    // A full-target triangle: three vertices covering the whole 4x4 target.
    const float positions[]{-1, -1, 3, -1, -1, 3};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    auto vertices = device.create_buffer(vb, positions);
    ASSERT_NE(vertices, nullptr);

    // Green everywhere when the buffer asks for the triangle, red everywhere
    // when it asks for nothing -- whatever the call struct says. The last case
    // is the one the D3D11 frontend produces: its Draw*Indirect passes ZERO
    // counts, and the backend used to reject that as a bad draw, silently
    // dropping every indirect draw.
    const std::pair<std::uint32_t, std::uint32_t> cases[] = {
        {3u, 3u}, {0u, 3u}, {3u, 0u}};
    for (const auto& [buffer_vertex_count, call_vertex_count] : cases) {
        BackendTexture2DDesc td{};
        td.width = td.height = 4;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = PixelFormat::BGRA8Unorm;
        td.render_target = true;
        auto target = device.create_texture_2d(td, nullptr, 0);
        ASSERT_NE(target, nullptr);

        const DrawArgs args{buffer_vertex_count, 1, 0, 0};
        auto args_buffer = make_args_buffer(device, &args, sizeof(args));
        ASSERT_NE(args_buffer, nullptr);

        // Clear to red first, then preserve: a target still red after the pass
        // is a target the draw never touched.
        ASSERT_TRUE(target->clear_render_target({1.0f, 0.0f, 0.0f, 1.0f}));
        BackendRenderPassDesc pass_desc{};
        pass_desc.color_targets[0] = target.get();
        pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
        pass_desc.preserve_color_contents = true;
        auto pass = device.create_render_pass_standalone(pass_desc);
        ASSERT_NE(pass, nullptr);

        BackendDrawCall call{};
        call.pipeline = pipeline.get();
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = call_vertex_count;  // ignored: the buffer decides
        call.instance_count = call_vertex_count == 0 ? 0u : 1u;
        call.vertex_buffer = vertices.get();
        call.vertex_buffers[0] = vertices.get();
        call.viewport = {0, 0, 4, 4, 0, 1};
        call.indirect.buffer = args_buffer.get();
        call.indirect.offset = 0;
        ASSERT_TRUE(pass->draw(call));
        ASSERT_TRUE(pass->end());

        std::array<std::uint8_t, 64> pixels{};
        ASSERT_TRUE(target->readback(0, pixels.data(), 16));
        const bool drew = buffer_vertex_count != 0;
        for (std::size_t p = 0; p < 16; ++p) {
            // BGRA8: [0]=B, [1]=G, [2]=R.
            EXPECT_EQ(pixels[p * 4 + 1], drew ? 255u : 0u)
                << "pixel " << p << " green, buffer count "
                << buffer_vertex_count;
            EXPECT_EQ(pixels[p * 4 + 2], drew ? 0u : 255u)
                << "pixel " << p << " red, buffer count "
                << buffer_vertex_count;
        }
    }
}

// Learn (DrawIndexedInstancedIndirect / DispatchIndirect): the offset is
// DWORD-aligned. Metal requires the same. A misaligned offset is a caller
// error, and the honest response is to refuse the call — not to round the
// offset, which would silently execute a different argument block.
TEST(IndirectArgs, MisalignedArgumentOffsetIsRefusedRatherThanRounded) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(device uint* out [[buffer(14)]],
                          uint3 id [[thread_position_in_grid]]) {
            out[0] = 1;
        }
    )";
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    const std::array<DispatchArgs, 2> args{DispatchArgs{1, 1, 1},
                                           DispatchArgs{1, 1, 1}};
    auto args_buffer = make_args_buffer(device, args.data(), sizeof(args));
    ASSERT_NE(args_buffer, nullptr);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.indirect.buffer = args_buffer.get();

    call.indirect.offset = 12; // the second block: aligned, in range
    EXPECT_TRUE(device.dispatch(call));

    call.indirect.offset = 2; // not DWORD aligned
    EXPECT_FALSE(device.dispatch(call));
}

// A direct dispatch still rejects a zero group count, but an indirect one must
// not: zero is a value the GPU is entitled to have written, and Metal treats it
// as a dispatch of no threadgroups. Rejecting it would turn a legal
// "nothing to do this frame" into a dropped call.
TEST(IndirectArgs, ZeroGroupCountIsRejectedOnlyForADirectDispatch) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(device uint* out [[buffer(14)]],
                          uint3 id [[thread_position_in_grid]]) {
            out[0] = 1;
        }
    )";
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_NE(pipeline, nullptr);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = 0;
    call.group_count_y = 0;
    call.group_count_z = 0;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    EXPECT_FALSE(device.dispatch(call)) << "direct dispatch of zero groups";

    const DispatchArgs args{0, 0, 0};
    auto args_buffer = make_args_buffer(device, &args, sizeof(args));
    ASSERT_NE(args_buffer, nullptr);
    call.indirect.buffer = args_buffer.get();
    call.indirect.offset = 0;
    EXPECT_TRUE(device.dispatch(call))
        << "indirect dispatch whose buffer happens to hold zero";
}

// D3D11's pixel stage can write through unordered-access views bound by
// OMSetRenderTargetsAndUnorderedAccessViews. Those bindings were recorded by the
// frontend and then dropped: BackendDrawCall had no u# arrays at all, so the
// draw reached Metal with the UAV missing and the shader's writes went nowhere.
//
// This is the backend half: prove a fragment shader's write through a u# texture
// lands in memory, at the slot it was bound to. The slot is deliberately not 0 --
// an implementation that ignored the index would still pass at slot 0.
TEST(FragmentUnorderedAccess, PixelShaderWriteThroughUavSlotReachesMemory) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    constexpr unsigned UAV_SLOT = 3;
    // The MSL index the backend binds a u# texture at, which is what the shader
    // must declare. Taken from the contract, never spelled as a literal.
    const unsigned uav_texture_index = BACKEND_UAV_TEXTURE_BASE + UAV_SLOT;

    BackendPipelineDesc pdesc{};
    pdesc.vertex_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float2 position [[attribute(0)]]; };
        struct main0_out { float4 position [[position]]; };
        vertex main0_out main0(main0_in in [[stage_in]]) {
            main0_out out = {};
            out.position = float4(in.position, 0.0, 1.0);
            return out;
        }
    )";
    pdesc.fragment_shader_msl =
        std::string(R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 main0(
            float4 pos [[position]],
            texture2d<float, access::write> u [[texture()") +
        std::to_string(uav_texture_index) + R"()]]) {
            u.write(float4(1.0, 0.0, 0.0, 1.0), uint2(pos.xy));
            return float4(0.0, 0.0, 1.0, 1.0);
        }
    )";
    pdesc.vertex_entry_point = "main0";
    pdesc.fragment_entry_point = "main0";
    pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
    pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
    pdesc.color_format = PixelFormat::BGRA8Unorm;
    pdesc.cache_key = 0x5541565f50530001ull;
    auto pipeline = device.create_pipeline(pdesc);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid()) << pdesc.fragment_shader_msl;

    const float positions[]{-1, -1, 3, -1, -1, 3};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    auto vertices = device.create_buffer(vb, positions);
    ASSERT_NE(vertices, nullptr);

    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::BGRA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_NE(target, nullptr);

    BackendTexture2DDesc ud{};
    ud.width = ud.height = 4;
    ud.mip_levels = ud.array_size = 1;
    ud.pixel_format = PixelFormat::RGBA8Unorm;
    ud.unordered_access = true;
    ud.render_target = true;  // so the untouched state is known
    auto uav = device.create_texture_2d(ud, nullptr, 0);
    ASSERT_NE(uav, nullptr);
    ASSERT_TRUE(uav->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));

    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);

    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.vertex_buffer = vertices.get();
    call.vertex_buffers[0] = vertices.get();
    call.viewport = {0, 0, 4, 4, 0, 1};
    call.fragment_uav_textures[UAV_SLOT] = uav.get();
    call.fragment_uav_texture_views[UAV_SLOT].dimension =
        BackendTextureViewDimension::Texture2D;
    call.fragment_uav_texture_views[UAV_SLOT].mip_levels = 1;
    call.fragment_uav_texture_views[UAV_SLOT].array_size = 1;
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(uav->readback(0, pixels.data(), 16));
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        EXPECT_EQ(pixels[i], 255u)
            << "texel " << (i / 4) << " red channel: the fragment shader's UAV "
            << "write did not reach memory";
    }
}

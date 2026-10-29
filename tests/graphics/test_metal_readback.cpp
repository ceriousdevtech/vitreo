// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_metal_readback.cpp
/// @brief GPU texture readback must observe draws still sitting on the
///        uncommitted frame recorder (D3D11 CopySubresourceRegion GPU→staging).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/metal/metal_swapchain.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace vitreo::graphics;

TEST(MetalReadback, PipelinePadsFragmentInputsMissingFromVertexStage) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    BackendPipelineDesc desc{};
    desc.vertex_shader_msl = R"(
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
    desc.fragment_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float4 optional [[user(locn8)]]; };
        fragment float4 main0(main0_in in [[stage_in]]) {
            return in.optional;
        }
    )";
    desc.vertex_entry_point = "main0";
    desc.fragment_entry_point = "main0";
    desc.vertex_attributes = {
        {0, 0, BackendVertexFormat::Float2, 0},
    };
    desc.vertex_stride = desc.vertex_buffer_strides[0] = 8;
    desc.color_format = PixelFormat::BGRA8Unorm;
    desc.cache_key = 0x4c494e4b50414438ull;

    auto pipeline = device.create_pipeline(desc);
    ASSERT_NE(pipeline, nullptr);
    EXPECT_TRUE(pipeline->valid());
}

TEST(MetalReadback, ShareableTextureWithoutAnIOSurfaceFormIsMirrored) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    // Learn D3D11_RESOURCE_MISC_SHARED: the flag makes a resource *shareable*,
    // it does not constrain the shape. UE5 tags ordinary render targets with
    // it, so refusing to create one is a fatal RHI error. An IOSurface holds
    // one image, so a mip chain is mirrored through a canonical store
    // (metal_shared_resource.hpp), allocated when it is first exported: until
    // then it has no shared id and costs nothing extra. (This test used to
    // expect such a texture to be created unshared; the backend now shares
    // every shape -- d3d11_share_ers_probe measures Windows doing so.)
    BackendTexture2DDesc shared_chain{};
    shared_chain.width = shared_chain.height = 1000;
    shared_chain.mip_levels = 2;
    shared_chain.pixel_format = PixelFormat::RGBA16Float;
    shared_chain.render_target = true;
    shared_chain.shader_resource = true;
    shared_chain.shareable = true;
    shared_chain.shared_metadata = {1, 2, 3};
    auto chain = device.create_texture_2d(shared_chain, nullptr, 0);
    ASSERT_NE(chain, nullptr);
    EXPECT_TRUE(chain->valid());
    EXPECT_EQ(chain->shared_handle(), 0u);
    EXPECT_TRUE(chain->desc().shareable);
    EXPECT_EQ(chain->shared_metadata(), (std::vector<vitreo::u8>{1, 2, 3}));
    EXPECT_TRUE(chain->clear_render_target({0, 0, 0, 1}));
    const vitreo::u64 id = chain->export_shared();
    EXPECT_NE(id, 0u);
    EXPECT_EQ(chain->shared_handle(), id);

    // A format with no IOSurface form (block-compressed, depth) has no
    // zero-copy form, but shares through a store.
    EXPECT_FALSE(device.supports_shared_texture_2d(PixelFormat::BC1_RGBA));
    EXPECT_FALSE(device.supports_shared_texture_2d(PixelFormat::D32Float));
    EXPECT_TRUE(device.supports_shared_texture(PixelFormat::BC1_RGBA));
    EXPECT_TRUE(device.supports_shared_texture(PixelFormat::D32Float));
    BackendTexture2DDesc shared_bc1{};
    shared_bc1.width = shared_bc1.height = 16;
    shared_bc1.pixel_format = PixelFormat::BC1_RGBA;
    shared_bc1.shader_resource = true;
    shared_bc1.shareable = true;
    auto bc1 = device.create_texture_2d(shared_bc1, nullptr, 0);
    ASSERT_NE(bc1, nullptr);
    EXPECT_TRUE(bc1->valid());
    EXPECT_EQ(bc1->shared_handle(), 0u);
    EXPECT_NE(bc1->export_shared(), 0u);

    // The single image of a D3D11.1 guaranteed-shareable format gets a real
    // shared handle (Learn, "Extended support for shared Texture2D
    // resources": R16G16B16A16_FLOAT is one of them).
    BackendTexture2DDesc shared_half{};
    shared_half.width = shared_half.height = 1000;
    shared_half.pixel_format = PixelFormat::RGBA16Float;
    shared_half.render_target = true;
    shared_half.shader_resource = true;
    shared_half.shareable = true;
    auto half = device.create_texture_2d(shared_half, nullptr, 0);
    ASSERT_NE(half, nullptr);
    ASSERT_TRUE(half->valid());
    EXPECT_NE(half->shared_handle(), 0u);
}

TEST(MetalReadback, SharedTextureOpensWithItsDescriptionAndPixels) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    // Every storage format of the D3D11.1 shareable list and of the
    // ExtendedResourceSharing list (Learn, Direct3D 11.1 features) that this
    // backend has a texture for. An odd width exercises the IOSurface row
    // alignment against the tight rows the caller uploads.
    struct Case {
        PixelFormat format;
        unsigned bytes;
        bool render_target;
    };
    const Case cases[] = {
        {PixelFormat::BGRA8Unorm, 4, true},
        {PixelFormat::BGRA8Unorm_sRGB, 4, true},
        {PixelFormat::RGBA8Unorm, 4, true},
        {PixelFormat::RGBA8Unorm_sRGB, 4, true},
        {PixelFormat::RGB10A2Unorm, 4, true},
        {PixelFormat::RGBA16Float, 8, true},
        {PixelFormat::RGBA32Float, 16, true},
        {PixelFormat::RGBA32Uint, 16, true},
        {PixelFormat::RGBA32Sint, 16, true},
        {PixelFormat::RGBA16Unorm, 8, true},
        {PixelFormat::RGBA16Snorm, 8, true},
        {PixelFormat::RGBA16Uint, 8, true},
        {PixelFormat::RGBA16Sint, 8, true},
        {PixelFormat::RGB10A2Uint, 4, true},
        {PixelFormat::RGBA8Uint, 4, true},
        {PixelFormat::RGBA8Snorm, 4, true},
        {PixelFormat::RGBA8Sint, 4, true},
        {PixelFormat::R32Float, 4, true},
        {PixelFormat::R32Uint, 4, true},
        {PixelFormat::R32Sint, 4, true},
        {PixelFormat::R16Float, 2, true},
        {PixelFormat::R16Unorm, 2, true},
        {PixelFormat::R16Snorm, 2, true},
        {PixelFormat::R16Uint, 2, true},
        {PixelFormat::R16Sint, 2, true},
        {PixelFormat::R8Unorm, 1, true},
        {PixelFormat::R8Snorm, 1, true},
        {PixelFormat::R8Uint, 1, true},
        {PixelFormat::R8Sint, 1, true},
        {PixelFormat::A8Unorm, 1, true},
        {PixelFormat::RG11B10Float, 4, true},
        {PixelFormat::RG8Unorm, 2, true},
        {PixelFormat::RG16Float, 4, true},
    };
    constexpr unsigned kWidth = 13;
    constexpr unsigned kHeight = 7;
    for (const Case& c : cases) {
        SCOPED_TRACE(static_cast<unsigned>(c.format));
        EXPECT_TRUE(device.supports_shared_texture_2d(c.format));
        BackendTexture2DDesc desc{};
        desc.width = kWidth;
        desc.height = kHeight;
        desc.pixel_format = c.format;
        desc.render_target = c.render_target;
        desc.shader_resource = true;
        desc.shareable = true;
        desc.shared_metadata = {0xD3, 0x11, static_cast<std::uint8_t>(c.format)};
        auto creator = device.create_texture_2d(desc, nullptr, 0);
        ASSERT_NE(creator, nullptr);
        ASSERT_TRUE(creator->valid());
        const std::uint64_t handle = creator->shared_handle();
        ASSERT_NE(handle, 0u);
        EXPECT_EQ(creator->shared_metadata(), desc.shared_metadata);

        const unsigned row = kWidth * c.bytes;
        std::vector<std::uint8_t> pattern(row * kHeight);
        for (std::size_t i = 0; i < pattern.size(); ++i) {
            pattern[i] = static_cast<std::uint8_t>((i * 7u + 3u) % 251u);
        }
        ASSERT_TRUE(creator->update(0, pattern.data(), row));

        // Another opening of the same storage: the recorded description and
        // the creator's bytes, whoever reads them.
        auto opened = device.open_shared_texture_2d(handle);
        ASSERT_NE(opened, nullptr);
        ASSERT_TRUE(opened->valid());
        EXPECT_EQ(opened->shared_handle(), handle);
        EXPECT_EQ(opened->desc().width, kWidth);
        EXPECT_EQ(opened->desc().height, kHeight);
        EXPECT_EQ(opened->desc().pixel_format, c.format);
        EXPECT_EQ(opened->desc().render_target, c.render_target);
        EXPECT_TRUE(opened->desc().shader_resource);
        EXPECT_EQ(opened->shared_metadata(), desc.shared_metadata);
        std::vector<std::uint8_t> seen(pattern.size(), 0);
        ASSERT_TRUE(opened->readback(0, seen.data(), row));
        EXPECT_EQ(seen, pattern);

        // A retained reference keeps the storage (and so the bytes) alive
        // after every texture on it is gone -- what an NT shared-resource
        // handle does (Learn IDXGIResource1::CreateSharedHandle; measured:
        // an NT handle opens with the data after the creator released it).
        auto keep = device.retain_shared_texture(handle);
        ASSERT_NE(keep, nullptr);
        creator.reset();
        opened.reset();
        auto reopened = device.open_shared_texture_2d(handle);
        ASSERT_NE(reopened, nullptr);
        std::vector<std::uint8_t> kept(pattern.size(), 0);
        ASSERT_TRUE(reopened->readback(0, kept.data(), row));
        EXPECT_EQ(kept, pattern);
    }
}

TEST(MetalReadback, R11G11B10FloatSupportsUnorderedAccess) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";

    // DXGI_FORMAT_R11G11B10_FLOAT (26) had no case in pixel_format_from_dxgi,
    // so it resolved to PixelFormat::Unknown and CreateTexture2D answered
    // E_INVALIDARG. UE's SkyAtmosphere allocates its transmittance LUT in
    // exactly this shape and treats the failure as fatal:
    //
    //   Size=256x64x1 PF=9 D3DFormat=0x1A NumMips=1
    //   Flags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS
    //   Name:'SkyAtmosphere.TransmittanceLut'
    //
    // Learn (DXGI_FORMAT / Format Support): R11G11B10_FLOAT is a valid 2D
    // texture, shader-resource and unordered-access format. Metal's
    // RG11B10Float is the exact component match, so the whole descriptor has
    // to be creatable -- the UAV flag included, which is what makes this
    // different from merely naming the format.
    // Start from the DXGI value the guest passes, not from the enum: the
    // defect was the missing case in pixel_format_from_dxgi, and naming the
    // enum directly would keep passing if that case were removed again.
    ASSERT_EQ(pixel_format_from_dxgi(26), PixelFormat::RG11B10Float);

    BackendTexture2DDesc desc{};
    desc.width = 256;
    desc.height = 64;
    desc.mip_levels = 1;
    desc.pixel_format = pixel_format_from_dxgi(26);
    desc.shader_resource = true;
    desc.unordered_access = true;
    auto lut = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(lut, nullptr);
    EXPECT_TRUE(lut->valid())
        << "R11G11B10_FLOAT with BIND_UNORDERED_ACCESS must create; UE treats "
           "the failure as a fatal RHI error";
}

TEST(MetalReadback, DiscardBufferContentsSurvivePoolRecycling) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    const std::array<vitreo::u32, 4> initial{1, 2, 3, 4};
    const std::array<vitreo::u32, 4> updated{5, 6, 7, 8};
    const std::array<vitreo::u32, 4> unrelated{9, 10, 11, 12};
    MetalBuffer persistent(device.device_ptr(), sizeof(initial),
                           BufferUsage::Uniform, initial.data());
    ASSERT_NE(persistent.handle(), nullptr);
    void* original = persistent.handle();
    const void* original_contents = persistent.map();
    ASSERT_NE(original_contents, nullptr);
    persistent.note_gpu_bind();
    persistent.update(updated.data(), 0, sizeof(updated),
                      BufferWrite::Discard);
    ASSERT_NE(persistent.handle(), original);
    EXPECT_EQ(std::memcmp(original_contents, initial.data(), sizeof(initial)), 0);

    BackendTexture2DDesc desc{};
    desc.width = desc.height = 4;
    desc.pixel_format = PixelFormat::RGBA8Unorm;
    desc.render_target = true;
    auto target = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->clear_render_target({0, 0, 0, 1}));
    MetalSubmitQueue::instance().drain();
    for (unsigned frame = 0; frame < 3; ++frame) {
        metal_note_presented_buffers();
    }
    MetalBuffer churn(device.device_ptr(), sizeof(initial),
                      BufferUsage::Uniform, initial.data());
    churn.note_gpu_bind();
    churn.update(unrelated.data(), 0, sizeof(unrelated),
                 BufferWrite::Discard);
    ASSERT_NE(persistent.handle(), nullptr);
    const void* contents = persistent.map();
    ASSERT_NE(contents, nullptr);
    EXPECT_EQ(std::memcmp(contents, updated.data(), sizeof(updated)), 0);
    const vitreo::u32 replacement = 42;
    persistent.update(&replacement, sizeof(vitreo::u32),
                      sizeof(replacement), BufferWrite::Ordered);
    const std::array<vitreo::u32, 4> partial{5, 42, 7, 8};
    EXPECT_EQ(std::memcmp(persistent.map(), partial.data(), sizeof(partial)), 0);
}

TEST(MetalReadback, DxbcDualSourceBlendsSecondOutputIntoTargetZero) {
    using namespace vitreo;
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.stage = ShaderStage::Fragment;
    container.output_signature.push_back({"SV_TARGET", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_TARGET", 1, 1, 0xFu, 3});
    const auto word = [&](u32 value) {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    word(0x50); word(19); // ps_5_0, two eight-word movs and ret
    const std::array<std::array<float, 4>, 2> outputs{{
        {0.8f, 0.4f, 0.2f, 0.5f}, {0.25f, 0.5f, 0.75f, 0.25f}}};
    for (u32 output = 0; output < 2; ++output) {
        word(54u | (8u << 24)); word(0x001020F2u); word(output);
        word(0x00004002u); // immediate32 float4
        for (float component : outputs[output]) {
            u32 bits;
            std::memcpy(&bits, &component, sizeof(bits));
            word(bits);
        }
    }
    word(62u | (1u << 24));
    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto mrt = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                        ShaderStage::Fragment, options);
    ASSERT_TRUE(mrt.valid());
    EXPECT_NE(std::string(mrt.msl_text()).find("color(1)"), std::string::npos);
    options.dual_source_blending = true;
    const auto dual = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                         ShaderStage::Fragment, options);
    ASSERT_TRUE(dual.valid());
    EXPECT_NE(std::string(dual.msl_text()).find("index(1)"), std::string::npos);
    EXPECT_EQ(std::string(dual.msl_text()).find("color(1)"), std::string::npos);

    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_NE(target, nullptr);
    const std::array<float, 4> destination{0.2f, 0.4f, 0.6f, 0.8f};
    const float positions[]{-1, -1, 3, -1, -1, 3};
    BackendBufferDesc vb_desc{};
    vb_desc.byte_width = sizeof(positions);
    vb_desc.vertex_buffer = true;
    auto vertices = device.create_buffer(vb_desc, positions);
    ASSERT_NE(vertices, nullptr);
    for (bool alpha_factor : {false, true}) {
        ASSERT_TRUE(target->clear_render_target(destination));
        BackendPipelineDesc pd{};
        pd.vertex_shader_msl = R"(
            #include <metal_stdlib>
            using namespace metal;
            struct VSIn { float2 position [[attribute(0)]]; };
            vertex float4 vertex_main(VSIn in [[stage_in]]) {
                return float4(in.position, 0, 1);
            }
        )";
        pd.fragment_shader_msl = dual.msl_text();
        pd.fragment_entry_point = "main0";
        pd.color_format = PixelFormat::RGBA8Unorm;
        pd.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
        pd.vertex_stride = pd.vertex_buffer_strides[0] = 8;
        pd.blend_enabled = true;
        pd.source_blend = alpha_factor ? BackendBlendFactor::Source1Alpha
                                      : BackendBlendFactor::Source1Color;
        pd.destination_blend = alpha_factor ? BackendBlendFactor::OneMinusSource1Alpha
                                           : BackendBlendFactor::OneMinusSource1Color;
        pd.source_blend_alpha = BackendBlendFactor::Source1Alpha;
        pd.destination_blend_alpha = BackendBlendFactor::OneMinusSource1Alpha;
        pd.cache_key = 0x4455414c53524300ull + alpha_factor;
        auto pipeline = device.create_pipeline(pd);
        ASSERT_NE(pipeline, nullptr);
        ASSERT_TRUE(pipeline->valid()) << dual.msl_text();
        BackendRenderPassDesc pass_desc{};
        pass_desc.color_targets[0] = target.get();
        pass_desc.preserve_color_contents = true;
        auto pass = device.create_render_pass_standalone(pass_desc);
        ASSERT_NE(pass, nullptr);
        BackendDrawCall call{};
        call.pipeline = pipeline.get();
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = 3;
        call.vertex_buffer = vertices.get();
        call.vertex_buffers[0] = vertices.get();
        call.viewport = {0, 0, 4, 4, 0, 1};
        ASSERT_TRUE(pass->draw(call));
        ASSERT_TRUE(pass->end());
        std::array<u8, 64> pixels{};
        ASSERT_TRUE(target->readback(0, pixels.data(), 16));
        for (usize pixel = 0; pixel < 16; ++pixel) {
            for (usize c = 0; c < 4; ++c) {
                const float factor = outputs[1][alpha_factor ? 3 : c];
                const float expected = outputs[0][c] * factor + destination[c] * (1 - factor);
                EXPECT_NEAR(pixels[pixel * 4 + c], expected * 255, 1.5);
            }
        }
    }
}

TEST(MetalReadback, SeesUnpresentedClear) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.render_target = true;
    desc.shader_resource = true;
    auto texture = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->valid());

    ASSERT_TRUE(texture->clear_render_target({1.0f, 0.0f, 0.0f, 1.0f}));

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(texture->readback(0, pixels.data(), 16));
    EXPECT_EQ(pixels[0], 0u);    // B
    EXPECT_EQ(pixels[1], 0u);    // G
    EXPECT_EQ(pixels[2], 255u);  // R
    EXPECT_EQ(pixels[3], 255u);  // A
}

TEST(MetalReadback, PrivateTextureUpdateBlitsThenReadback) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.shader_resource = true;
    desc.cpu_writable = false;
    auto texture = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->valid());

    std::vector<std::uint8_t> src(4u * 4u * 4u);
    for (std::size_t i = 0; i < src.size(); i += 4) {
        src[i] = 32;       // B
        src[i + 1] = 64;   // G
        src[i + 2] = 128;  // R
        src[i + 3] = 255;  // A
    }
    ASSERT_TRUE(texture->update(0, src.data(), 16));

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(texture->readback(0, pixels.data(), 16));
    EXPECT_EQ(pixels[0], 32u);
    EXPECT_EQ(pixels[1], 64u);
    EXPECT_EQ(pixels[2], 128u);
    EXPECT_EQ(pixels[3], 255u);
}

TEST(MetalReadback, ComputeTypedTexture2DUavWriteIsReadable) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc texture_desc{};
    texture_desc.width = 4;
    texture_desc.height = 4;
    texture_desc.mip_levels = 1;
    texture_desc.array_size = 1;
    texture_desc.pixel_format = PixelFormat::RGBA8Unorm;
    texture_desc.unordered_access = true;
    auto texture = device.create_texture_2d(texture_desc, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->valid());

    BackendComputePipelineDesc pipeline_desc{};
    pipeline_desc.compute_entry_point = "main0";
    pipeline_desc.compute_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(
            texture2d<float, access::write> u0 [[texture(8)]],
            uint3 id [[thread_position_in_grid]]) {
            if (id.x < u0.get_width() && id.y < u0.get_height()) {
                u0.write(float4(1.0, 0.0, 1.0, 1.0), id.xy);
            }
        }
    )";
    auto pipeline = device.create_compute_pipeline(pipeline_desc);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = 4;
    call.group_count_y = 4;
    call.group_count_z = 1;
    call.threads_per_group_x = 1;
    call.threads_per_group_y = 1;
    call.threads_per_group_z = 1;
    call.textures[8] = texture.get();
    ASSERT_TRUE(device.dispatch(call));

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(texture->readback(0, pixels.data(), 16));
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        EXPECT_EQ(pixels[i], 255u);      // R
        EXPECT_EQ(pixels[i + 1], 0u);   // G
        EXPECT_EQ(pixels[i + 2], 255u); // B
        EXPECT_EQ(pixels[i + 3], 255u); // A
    }
}

TEST(MetalReadback, ComputeSrvSamplerAndUavNamespacesCoexist) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    const std::array<float, 16> source_pixels{
        0.25f, 0.25f, 0.25f, 0.25f,
        0.25f, 0.25f, 0.25f, 0.25f,
        0.25f, 0.25f, 0.25f, 0.25f,
        0.25f, 0.25f, 0.25f, 0.25f};
    BackendTexture2DDesc source_desc{};
    source_desc.width = 4;
    source_desc.height = 4;
    source_desc.pixel_format = PixelFormat::R32Float;
    source_desc.shader_resource = true;
    auto source = device.create_texture_2d(
        source_desc, source_pixels.data(), 4u * sizeof(float));
    ASSERT_NE(source, nullptr);
    ASSERT_TRUE(source->valid());

    BackendTexture2DDesc output_desc{};
    output_desc.width = 4;
    output_desc.height = 4;
    output_desc.pixel_format = PixelFormat::RGBA8Unorm;
    output_desc.unordered_access = true;
    auto output = device.create_texture_2d(output_desc, nullptr, 0);
    ASSERT_NE(output, nullptr);
    ASSERT_TRUE(output->valid());

    BackendComputePipelineDesc pipeline_desc{};
    pipeline_desc.compute_entry_point = "main0";
    pipeline_desc.compute_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        kernel void main0(
            texture2d<float> t0 [[texture(0)]],
            sampler s0 [[sampler(0)]],
            texture2d<float, access::write> u0 [[texture(8)]],
            uint3 id [[thread_position_in_grid]]) {
            if (id.x < u0.get_width() && id.y < u0.get_height()) {
                float value = t0.sample(s0, (float2(id.xy) + 0.5) / 4.0,
                                        level(0.0)).x;
                u0.write(float4(value, 0.0, 0.0, 1.0), id.xy);
            }
        }
    )";
    auto pipeline = device.create_compute_pipeline(pipeline_desc);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = 4;
    call.group_count_y = 4;
    call.group_count_z = 1;
    call.textures[0] = source.get();
    call.textures[8] = output.get();
    call.sampler_bound[0] = true;
    call.samplers[0].min_filter = BackendFilter::Nearest;
    call.samplers[0].mag_filter = BackendFilter::Nearest;
    call.samplers[0].mipmaps_enabled = false;
    call.samplers[0].address_u = BackendAddressMode::Clamp;
    call.samplers[0].address_v = BackendAddressMode::Clamp;
    ASSERT_TRUE(device.dispatch(call));

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(output->readback(0, pixels.data(), 16));
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        EXPECT_NEAR(pixels[i], 64u, 1u);
        EXPECT_EQ(pixels[i + 1], 0u);
        EXPECT_EQ(pixels[i + 2], 0u);
        EXPECT_EQ(pixels[i + 3], 255u);
    }
}

TEST(MetalReadback, DynamicBlendColorAndSourceAlphaSaturatedReachEncoder) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc target_desc{};
    target_desc.width = 4;
    target_desc.height = 4;
    target_desc.mip_levels = 1;
    target_desc.array_size = 1;
    target_desc.pixel_format = PixelFormat::BGRA8Unorm;
    target_desc.render_target = true;
    auto target = device.create_texture_2d(target_desc, nullptr, 0);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->valid());

    const char* vertex_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VSIn { float2 pos [[attribute(0)]]; };
        struct VSOut { float4 position [[position]]; };
        vertex VSOut vertex_main(VSIn in [[stage_in]]) {
            VSOut out;
            out.position = float4(in.pos, 0.0, 1.0);
            return out;
        }
    )";
    const char* fragment_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 fragment_main() {
            return float4(0.8, 0.4, 0.2, 0.5);
        }
    )";
    const float vertices_data[]{
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    BackendBufferDesc vb_desc{};
    vb_desc.byte_width = sizeof(vertices_data);
    vb_desc.vertex_buffer = true;
    auto vertices = device.create_buffer(vb_desc, vertices_data);
    ASSERT_NE(vertices, nullptr);

    const auto make_pipeline = [&](BackendBlendFactor source,
                                   BackendBlendFactor destination,
                                   std::uint64_t key) {
        BackendPipelineDesc desc{};
        desc.vertex_shader_msl = vertex_msl;
        desc.fragment_shader_msl = fragment_msl;
        desc.vertex_attributes = {
            {0, 0, BackendVertexFormat::Float2, 0},
        };
        desc.vertex_stride = 8;
        desc.vertex_buffer_strides[0] = 8;
        desc.color_format = PixelFormat::BGRA8Unorm;
        desc.blend_enabled = true;
        desc.source_blend = source;
        desc.destination_blend = destination;
        desc.source_blend_alpha = source;
        desc.destination_blend_alpha = destination;
        desc.cache_key = key;
        return device.create_pipeline(desc);
    };
    const auto draw = [&](BackendPipeline* pipeline,
                          const std::array<float, 4>& blend_color) {
        BackendRenderPassDesc pass_desc{};
        pass_desc.color_targets[0] = target.get();
        pass_desc.preserve_color_contents = true;
        auto pass = device.create_render_pass_standalone(pass_desc);
        ASSERT_NE(pass, nullptr);
        BackendDrawCall call{};
        call.pipeline = pipeline;
        call.topology = BackendPrimitiveTopology::TriangleStrip;
        call.vertex_buffer = vertices.get();
        call.vertex_buffers[0] = vertices.get();
        call.vertex_count = 4;
        call.viewport = {0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f};
        call.blend_color = blend_color;
        ASSERT_TRUE(pass->draw(call));
        ASSERT_TRUE(pass->end());
    };

    ASSERT_TRUE(target->clear_render_target({0.2f, 0.4f, 0.6f, 0.8f}));
    auto constant = make_pipeline(BackendBlendFactor::BlendColor,
                                  BackendBlendFactor::OneMinusBlendColor,
                                  0x424c454e44434f4cull);
    ASSERT_NE(constant, nullptr);
    ASSERT_TRUE(constant->valid());
    draw(constant.get(), {0.25f, 0.5f, 0.75f, 0.5f});
    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(target->readback(0, pixels.data(), 16));
    EXPECT_NEAR(pixels[2], 89, 1);   // R = .8*.25 + .2*.75
    EXPECT_NEAR(pixels[1], 102, 1);  // G = .4*.5 + .4*.5
    EXPECT_NEAR(pixels[0], 77, 1);   // B = .2*.75 + .6*.25
    EXPECT_NEAR(pixels[3], 166, 1);  // A = .5*.5 + .8*.5

    ASSERT_TRUE(target->clear_render_target({0.2f, 0.4f, 0.6f, 0.25f}));
    auto saturated = make_pipeline(BackendBlendFactor::SourceAlphaSaturated,
                                   BackendBlendFactor::One,
                                   0x535243414c534154ull);
    ASSERT_NE(saturated, nullptr);
    ASSERT_TRUE(saturated->valid());
    draw(saturated.get(), {1.0f, 1.0f, 1.0f, 1.0f});
    ASSERT_TRUE(target->readback(0, pixels.data(), 16));
    EXPECT_NEAR(pixels[2], 153, 1);  // R = .8*.5 + .2
    EXPECT_NEAR(pixels[1], 153, 1);  // G = .4*.5 + .4
    EXPECT_NEAR(pixels[0], 179, 1);  // B = .2*.5 + .6
    EXPECT_NEAR(pixels[3], 191, 1);  // saturate alpha factor is 1
}

TEST(MetalReadback, CopyTextureRegionVisibleAfterReadback) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.shader_resource = true;
    desc.render_target = true;
    desc.cpu_writable = false;
    auto source = device.create_texture_2d(desc, nullptr, 0);
    auto dest = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(dest, nullptr);
    ASSERT_TRUE(source->valid());
    ASSERT_TRUE(dest->valid());

    std::vector<std::uint8_t> src(4u * 4u * 4u);
    for (std::size_t i = 0; i < src.size(); i += 4) {
        src[i] = 16;
        src[i + 1] = 48;
        src[i + 2] = 96;
        src[i + 3] = 255;
    }
    ASSERT_TRUE(source->update(0, src.data(), 16));
    ASSERT_TRUE(device.copy_texture_2d_region(dest.get(), 0, 0, source.get(),
                                              0, 0, 4, 4));

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(dest->readback(0, pixels.data(), 16));
    EXPECT_EQ(pixels[0], 16u);
    EXPECT_EQ(pixels[1], 48u);
    EXPECT_EQ(pixels[2], 96u);
    EXPECT_EQ(pixels[3], 255u);
}

// Learn D3D11_TEX2D_SRV: MipLevels is the shader-visible mip window.
// SAMPLE_B with a coarse lod bias must not read GENERATE_MIPS levels that
// the SRV did not expose (those levels stay zero until GenerateMips).
TEST(MetalReadback, SrvMipWindowHidesCoarserLevels) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc src_desc{};
    src_desc.width = 8;
    src_desc.height = 8;
    src_desc.mip_levels = 4;
    src_desc.array_size = 1;
    src_desc.pixel_format = PixelFormat::BGRA8Unorm;
    src_desc.shader_resource = true;
    src_desc.cpu_writable = true;
    auto source = device.create_texture_2d(src_desc, nullptr, 0);
    ASSERT_NE(source, nullptr);
    ASSERT_TRUE(source->valid());

    std::vector<std::uint8_t> red(8u * 8u * 4u);
    for (std::size_t i = 0; i < red.size(); i += 4) {
        red[i] = 0;
        red[i + 1] = 0;
        red[i + 2] = 255;
        red[i + 3] = 255;
    }
    ASSERT_TRUE(source->update(0, red.data(), 32));

    std::vector<std::uint8_t> green(4u * 4u * 4u);
    for (std::size_t i = 0; i < green.size(); i += 4) {
        green[i] = 0;
        green[i + 1] = 255;
        green[i + 2] = 0;
        green[i + 3] = 255;
    }
    ASSERT_TRUE(source->update(1, green.data(), 16));

    BackendTexture2DDesc rt_desc{};
    rt_desc.width = 8;
    rt_desc.height = 8;
    rt_desc.mip_levels = 1;
    rt_desc.array_size = 1;
    rt_desc.pixel_format = PixelFormat::BGRA8Unorm;
    rt_desc.render_target = true;
    rt_desc.shader_resource = true;
    auto target = device.create_texture_2d(rt_desc, nullptr, 0);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->valid());
    ASSERT_TRUE(target->clear_render_target({0.0f, 0.0f, 1.0f, 1.0f}));

    const char* vertex_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VSIn {
            float2 pos [[attribute(0)]];
            float2 uv  [[attribute(1)]];
        };
        struct VSOut {
            float4 position [[position]];
            float2 uv;
        };
        vertex VSOut vertex_main(VSIn in [[stage_in]]) {
            VSOut o;
            o.position = float4(in.pos, 0.0, 1.0);
            o.uv = in.uv;
            return o;
        }
    )";
    const char* fragment_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VSOut {
            float4 position [[position]];
            float2 uv;
        };
        fragment float4 fragment_main(VSOut in [[stage_in]],
                                      texture2d<float> tex [[texture(0)]],
                                      sampler samp [[sampler(0)]]) {
            return tex.sample(samp, in.uv, bias(16.0));
        }
    )";

    BackendPipelineDesc pipeline_desc{};
    pipeline_desc.vertex_shader_msl = vertex_msl;
    pipeline_desc.fragment_shader_msl = fragment_msl;
    pipeline_desc.vertex_attributes = {
        {0, 0, BackendVertexFormat::Float2, 0},
        {1, 8, BackendVertexFormat::Float2, 0},
    };
    pipeline_desc.vertex_stride = 16;
    pipeline_desc.vertex_buffer_strides[0] = 16;
    pipeline_desc.color_format = PixelFormat::BGRA8Unorm;
    pipeline_desc.cache_key = 0x5352564d495031ull;
    auto pipeline = device.create_pipeline(pipeline_desc);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    // Fullscreen triangle strip, UV 0..1.
    const float verts[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    BackendBufferDesc vb_desc{};
    vb_desc.byte_width = sizeof(verts);
    vb_desc.vertex_buffer = true;
    auto vertices = device.create_buffer(vb_desc, verts);
    ASSERT_NE(vertices, nullptr);
    ASSERT_TRUE(vertices->valid());

    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    ASSERT_TRUE(pass->valid());

    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleStrip;
    call.vertex_buffer = vertices.get();
    call.vertex_buffers[0] = vertices.get();
    call.vertex_count = 4;
    call.textures[0] = source.get();
    call.samplers[0].mipmaps_enabled = false;
    call.samplers[0].min_filter = BackendFilter::Linear;
    call.samplers[0].mag_filter = BackendFilter::Linear;
    call.viewport = {0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    std::vector<std::uint8_t> pixels(8u * 8u * 4u, 0);
    ASSERT_TRUE(target->readback(0, pixels.data(), 32));
    EXPECT_EQ(pixels[0], 0u);    // B — mip 0 red, not green/blue/black
    EXPECT_EQ(pixels[1], 0u);    // G
    EXPECT_EQ(pixels[2], 255u);  // R
    EXPECT_EQ(pixels[3], 255u);  // A

    // Same shader, full mip chain visible: bias(16) must reach the empty
    // coarsest level instead of staying on the red mip 0.
    ASSERT_TRUE(target->clear_render_target({0.0f, 0.0f, 1.0f, 1.0f}));
    auto full_pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(full_pass, nullptr);
    ASSERT_TRUE(full_pass->valid());
    call.samplers[0].mipmaps_enabled = true;
    call.samplers[0].mip_filter = BackendFilter::Linear;
    ASSERT_TRUE(full_pass->draw(call));
    ASSERT_TRUE(full_pass->end());
    ASSERT_TRUE(target->readback(0, pixels.data(), 32));
    EXPECT_EQ(pixels[2], 0u) << "full chain + SAMPLE_B bias should miss mip 0";
}

TEST(MetalReadback, RenderTargetIsImmediatelyVisibleToFollowingSrvPass) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc texture_desc{};
    texture_desc.width = 8;
    texture_desc.height = 8;
    texture_desc.mip_levels = 1;
    texture_desc.array_size = 1;
    texture_desc.pixel_format = PixelFormat::BGRA8Unorm;
    texture_desc.render_target = true;
    texture_desc.shader_resource = true;
    auto source = device.create_texture_2d(texture_desc, nullptr, 0);
    auto target = device.create_texture_2d(texture_desc, nullptr, 0);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(source->valid());
    ASSERT_TRUE(target->valid());

    const char* vertex_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VSIn {
            float2 pos [[attribute(0)]];
            float2 uv [[attribute(1)]];
        };
        struct VSOut {
            float4 position [[position]];
            float2 uv;
        };
        vertex VSOut vertex_main(VSIn in [[stage_in]]) {
            return {float4(in.pos, 0.0, 1.0), in.uv};
        }
    )";
    const char* solid_fragment_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 fragment_main() {
            return float4(0.1, 0.2, 0.3, 1.0);
        }
    )";
    const char* sample_fragment_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VSOut { float4 position [[position]]; float2 uv; };
        fragment float4 fragment_main(
            VSOut in [[stage_in]], texture2d<float> source [[texture(0)]],
            sampler source_sampler [[sampler(0)]]) {
            return source.sample(source_sampler, in.uv);
        }
    )";

    const float verts[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };
    BackendBufferDesc vb_desc{};
    vb_desc.byte_width = sizeof(verts);
    vb_desc.vertex_buffer = true;
    auto vertices = device.create_buffer(vb_desc, verts);
    ASSERT_NE(vertices, nullptr);
    ASSERT_TRUE(vertices->valid());

    const auto make_pipeline = [&](const char* fragment, std::uint64_t key) {
        BackendPipelineDesc desc{};
        desc.vertex_shader_msl = vertex_msl;
        desc.fragment_shader_msl = fragment;
        desc.vertex_attributes = {
            {0, 0, BackendVertexFormat::Float2, 0},
            {1, 8, BackendVertexFormat::Float2, 0},
        };
        desc.vertex_stride = 16;
        desc.vertex_buffer_strides[0] = 16;
        desc.color_format = PixelFormat::BGRA8Unorm;
        desc.cache_key = key;
        return device.create_pipeline(desc);
    };
    auto solid = make_pipeline(solid_fragment_msl, 0x5254535256533031ull);
    auto sample = make_pipeline(sample_fragment_msl, 0x5254535256533032ull);
    ASSERT_NE(solid, nullptr);
    ASSERT_NE(sample, nullptr);
    ASSERT_TRUE(solid->valid());
    ASSERT_TRUE(sample->valid());

    BackendDrawCall call{};
    call.topology = BackendPrimitiveTopology::TriangleStrip;
    call.vertex_buffer = vertices.get();
    call.vertex_buffers[0] = vertices.get();
    call.vertex_count = 4;
    call.viewport = {0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f};

    BackendRenderPassDesc source_pass_desc{};
    source_pass_desc.color_targets[0] = source.get();
    source_pass_desc.preserve_color_contents = false;
    auto source_pass = device.create_render_pass_standalone(source_pass_desc);
    ASSERT_NE(source_pass, nullptr);
    ASSERT_TRUE(source_pass->valid());
    call.pipeline = solid.get();
    ASSERT_TRUE(source_pass->draw(call));
    ASSERT_TRUE(source_pass->end());

    BackendRenderPassDesc target_pass_desc{};
    target_pass_desc.color_targets[0] = target.get();
    target_pass_desc.preserve_color_contents = false;
    auto target_pass = device.create_render_pass_standalone(target_pass_desc);
    ASSERT_NE(target_pass, nullptr);
    ASSERT_TRUE(target_pass->valid());
    call.pipeline = sample.get();
    call.textures[0] = source.get();
    call.samplers[0].min_filter = BackendFilter::Linear;
    call.samplers[0].mag_filter = BackendFilter::Linear;
    ASSERT_TRUE(target_pass->draw(call));
    ASSERT_TRUE(target_pass->end());

    std::vector<std::uint8_t> pixels(8u * 8u * 4u, 0);
    ASSERT_TRUE(target->readback(0, pixels.data(), 32));
    EXPECT_NEAR(pixels[0], 77, 1);  // B
    EXPECT_NEAR(pixels[1], 51, 1);  // G
    EXPECT_NEAR(pixels[2], 26, 1);  // R
    EXPECT_EQ(pixels[3], 255u);     // A
}

TEST(MetalReadback, CubeTextureArraySizeOneCreatesSixFaces) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc desc{};
    desc.width = 8;
    desc.height = 8;
    desc.mip_levels = 1;
    desc.array_size = 1;
    desc.cube = true;
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.shader_resource = true;
    desc.render_target = true;
    auto texture = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->valid());
    EXPECT_EQ(texture->desc().array_size, 6u);

    std::vector<std::uint8_t> src(8u * 8u * 4u, 0);
    for (std::size_t i = 0; i < src.size(); i += 4) {
        src[i] = 16;
        src[i + 1] = 32;
        src[i + 2] = 64;
        src[i + 3] = 255;
    }
    ASSERT_TRUE(texture->update_slice(0, 5, src.data(), 32));

    std::vector<std::uint8_t> pixels(8u * 8u * 4u, 0);
    ASSERT_TRUE(texture->readback_slice(0, 5, pixels.data(), 32));
    EXPECT_EQ(pixels[0], 16u);
    EXPECT_EQ(pixels[1], 32u);
    EXPECT_EQ(pixels[2], 64u);
    EXPECT_EQ(pixels[3], 255u);
}

TEST(MetalReadback, CubeTextureRenderTargetEdge64And128) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    for (unsigned edge : {64u, 128u}) {
        BackendTexture2DDesc desc{};
        desc.width = edge;
        desc.height = edge;
        desc.mip_levels = 1;
        desc.array_size = 6;
        desc.cube = true;
        desc.pixel_format = PixelFormat::BGRA8Unorm;
        desc.shader_resource = true;
        desc.render_target = true;
        auto texture = device.create_texture_2d(desc, nullptr, 0);
        ASSERT_NE(texture, nullptr) << "edge=" << edge;
        ASSERT_TRUE(texture->valid()) << "edge=" << edge;
        EXPECT_EQ(texture->desc().array_size, 6u);
        EXPECT_EQ(texture->desc().width, edge);
        EXPECT_TRUE(texture->desc().cube);
        EXPECT_TRUE(texture->desc().render_target);
    }
}

TEST(MetalReadback, CubeTextureArraySizeSixCreatesSingleCube) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc desc{};
    desc.width = 8;
    desc.height = 8;
    desc.mip_levels = 1;
    desc.array_size = 6;
    desc.cube = true;
    desc.pixel_format = PixelFormat::BGRA8Unorm;
    desc.shader_resource = true;
    desc.render_target = true;
    auto texture = device.create_texture_2d(desc, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    ASSERT_TRUE(texture->valid());
    EXPECT_EQ(texture->desc().array_size, 6u);
}

/// D3D11 leaves a depth-stencil state bound across draws that target no
/// depth-stencil view; Microsoft Learn (OMSetRenderTargets) specifies that
/// depth and stencil testing simply do not occur when no depth-stencil view
/// is bound, so such a draw must still rasterise. Dropping it instead leaves
/// the render target on whatever it held before -- for a swap-chain back
/// buffer, uninitialised GPU memory.
TEST(MetalRenderPass, DepthTestEnabledWithoutDepthTargetStillDraws) {
    auto& device = MetalDevice::instance();
    if (!device.available()) {
        GTEST_SKIP() << "no Metal device on this machine";
    }

    BackendTexture2DDesc target_desc{};
    target_desc.width = 4;
    target_desc.height = 4;
    target_desc.mip_levels = 1;
    target_desc.array_size = 1;
    target_desc.pixel_format = PixelFormat::BGRA8Unorm;
    target_desc.render_target = true;
    auto target = device.create_texture_2d(target_desc, nullptr, 0);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(target->valid());
    ASSERT_TRUE(target->clear_render_target({0.0f, 0.0f, 0.0f, 1.0f}));

    const char* vertex_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct VSIn { float2 pos [[attribute(0)]]; };
        struct VSOut { float4 position [[position]]; };
        vertex VSOut vertex_main(VSIn in [[stage_in]]) {
            VSOut out;
            out.position = float4(in.pos, 0.0, 1.0);
            return out;
        }
    )";
    const char* fragment_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        fragment float4 fragment_main() {
            return float4(0.0, 1.0, 0.0, 1.0);
        }
    )";

    BackendPipelineDesc pipeline_desc{};
    pipeline_desc.vertex_shader_msl = vertex_msl;
    pipeline_desc.fragment_shader_msl = fragment_msl;
    pipeline_desc.vertex_attributes = {
        {0, 0, BackendVertexFormat::Float2, 0},
    };
    pipeline_desc.vertex_stride = 8;
    pipeline_desc.vertex_buffer_strides[0] = 8;
    pipeline_desc.color_format = PixelFormat::BGRA8Unorm;
    // No depth attachment: matches a pass built from a render-target view
    // with a null depth-stencil view.
    pipeline_desc.depth_attachment = false;
    pipeline_desc.cache_key = 0x4e4f4445505448ull;
    auto pipeline = device.create_pipeline(pipeline_desc);
    ASSERT_NE(pipeline, nullptr);
    ASSERT_TRUE(pipeline->valid());

    const float verts[] = {
        -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f};
    BackendBufferDesc vb_desc{};
    vb_desc.byte_width = sizeof(verts);
    vb_desc.vertex_buffer = true;
    auto vertices = device.create_buffer(vb_desc, verts);
    ASSERT_NE(vertices, nullptr);
    ASSERT_TRUE(vertices->valid());

    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.preserve_color_contents = true;
    pass_desc.depth_stencil_target = nullptr;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    ASSERT_TRUE(pass->valid());

    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleStrip;
    call.vertex_buffer = vertices.get();
    call.vertex_buffers[0] = vertices.get();
    call.vertex_count = 4;
    call.instance_count = 1;
    call.cull_mode = BackendCullMode::None;
    call.viewport = {0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f};
    // The state under test: the guest left depth testing on with no
    // depth-stencil view bound.
    call.depth_test_enabled = true;
    call.depth_write_enabled = false;
    call.depth_compare = BackendCompare::LessEqual;
    EXPECT_TRUE(pass->draw(call));
    EXPECT_TRUE(pass->end());

    std::vector<std::uint8_t> pixels(4u * 4u * 4u, 0);
    ASSERT_TRUE(target->readback(0, pixels.data(), 16));
    EXPECT_EQ(pixels[0], 0u);    // B
    EXPECT_EQ(pixels[1], 255u);  // G
    EXPECT_EQ(pixels[2], 0u);    // R
    EXPECT_EQ(pixels[3], 255u);  // A
}

// CAMetalLayer.pixelFormat throws CAMetalLayerInvalid -- an uncaught
// NSException that aborts the host -- for any format outside its set. A DXGI
// swap chain in R32G32B32A32_FLOAT (MTLPixelFormatRGBA32Float, 125) or a
// depth format did exactly that (2026-09-28 D3D11 audit, area 11). The set
// was measured on macOS 26 by assigning each MTLPixelFormat to a fresh layer.
TEST(MetalSwapchainFormats, LayerFormatSetMatchesCoreAnimation) {
    for (const vitreo::u32 accepted : {80u, 81u, 70u, 71u, 115u, 90u, 94u, 552u,
                               553u, 554u, 555u}) {
        EXPECT_TRUE(metal_layer_supports_pixel_format(accepted)) << accepted;
    }
    for (const vitreo::u32 refused : {125u, 55u, 40u, 260u, 73u, 0u}) {
        EXPECT_FALSE(metal_layer_supports_pixel_format(refused)) << refused;
    }
}

TEST(MetalSwapchainFormats, UnsupportedFormatOrSizeIsRefusedNotThrown) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    // Both are refused before the (here absent) view is touched.
    SwapchainDesc desc{};
    desc.device = device.device_ptr();
    desc.width = 64;
    desc.height = 48;
    desc.pixel_format = 125;  // MTLPixelFormatRGBA32Float
    MetalSwapchain bad_format(desc);
    EXPECT_FALSE(bad_format.valid());
    desc.pixel_format = 80;
    desc.width = desc.height = 20000;  // past the 16384 texture limit
    MetalSwapchain too_big(desc);
    EXPECT_FALSE(too_big.valid());
    // Through the backend contract: an invalid swap chain, no exception.
    BackendSwapChainDesc backend_desc{};
    backend_desc.native_view = reinterpret_cast<void*>(0x1);
    backend_desc.width = 64;
    backend_desc.height = 48;
    backend_desc.pixel_format = PixelFormat::RGBA32Float;
    auto chain = device.create_swap_chain(backend_desc);
    EXPECT_TRUE(chain == nullptr || !chain->valid());
}

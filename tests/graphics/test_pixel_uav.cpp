// SPDX-License-Identifier: LGPL-2.1-only
//
// Pixel-shader UAVs, from real fxc output to real Metal.
//
// Feature level 11_0 gives the pixel stage the u# registers
// (OMSetRenderTargetsAndUnorderedAccessViews): typed, raw and structured
// UAVs, their loads and stores, atomics, and the [earlydepthstencil] global
// flag that decides whether a pixel failing the depth test still writes them.
// Every blob here is fxc output (tests/graphics/pixel_uav_shaders.h); every
// expected value follows from the HLSL in
// tools/oracle_probe/pixel_uav_shaders.hlsl and the fxc listing beside each
// blob, not from running the translator.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "pixel_uav_shaders.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kSentinel = 0xEEEEEEEEu;
constexpr u32 kSide = 4;  // a 4x4 target: 16 fragments, pixel i = y * 4 + x
constexpr u32 kFragments = kSide * kSide;

class PixelUavTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
    }

    static GraphicsBackendDevice& device() { return MetalDevice::instance(); }

    static std::unique_ptr<BackendBuffer> buffer(const std::vector<u32>& words) {
        BackendBufferDesc desc{};
        desc.byte_width = static_cast<unsigned>(words.size() * sizeof(u32));
        desc.shader_resource = true;
        desc.unordered_access = true;
        return device().create_buffer(desc, words.data());
    }

    /// The fragment MSL for one blob, as the D3D11 frontend builds it: flat
    /// bindings, MSL 3.1 when the shader does a typed-UAV atomic.
    static std::string fragment_msl(const unsigned char* bytecode, usize size) {
        const auto container = parse_dxbc(bytecode, size);
        EXPECT_TRUE(container.valid());
        const auto spirv = translate_dxbc_to_spirv(container);
        EXPECT_TRUE(spirv.valid()) << spirv.failure;
        if (!spirv.valid()) return {};
        MslCompileOptions flat{};
        flat.argument_buffers = false;
        if (spirv.uses_typed_uav_atomics && flat.msl_version < 30100u) {
            flat.msl_version = 30100u;
        }
        const auto msl = compile_spirv_to_msl(
            spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Fragment, flat);
        EXPECT_TRUE(msl.valid());
        return msl.valid() ? msl.msl_text() : std::string{};
    }

    /// A full-target triangle at depth @p z, drawn with @p fragment.
    static std::unique_ptr<BackendPipeline> pipeline(const std::string& fragment,
                                                     u32 key,
                                                     bool depth = false) {
        BackendPipelineDesc pdesc{};
        pdesc.vertex_shader_msl = R"(
            #include <metal_stdlib>
            using namespace metal;
            struct main0_in { float2 position [[attribute(0)]]; };
            struct main0_out { float4 position [[position]]; };
            vertex main0_out main0(main0_in in [[stage_in]]) {
                main0_out out = {};
                out.position = float4(in.position, 0.5, 1.0);
                return out;
            }
        )";
        pdesc.fragment_shader_msl = fragment;
        pdesc.vertex_entry_point = "main0";
        pdesc.fragment_entry_point = "main0";
        pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
        pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
        pdesc.color_format = PixelFormat::BGRA8Unorm;
        pdesc.depth_attachment = depth;
        pdesc.cache_key = key;
        auto made = device().create_pipeline(pdesc);
        EXPECT_TRUE(made && made->valid()) << fragment;
        return made;
    }

    struct Frame {
        std::unique_ptr<BackendBuffer> vertices;
        std::unique_ptr<BackendTexture2D> target;
        std::unique_ptr<BackendTexture2D> depth;
    };

    static Frame frame(bool with_depth) {
        static const float positions[]{-1, -1, 3, -1, -1, 3};
        Frame f;
        BackendBufferDesc vb{};
        vb.byte_width = sizeof(positions);
        vb.vertex_buffer = true;
        f.vertices = device().create_buffer(vb, positions);
        BackendTexture2DDesc td{};
        td.width = td.height = kSide;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = PixelFormat::BGRA8Unorm;
        td.render_target = true;
        f.target = device().create_texture_2d(td, nullptr, 0);
        if (with_depth) {
            BackendTexture2DDesc dd{};
            dd.width = dd.height = kSide;
            dd.mip_levels = dd.array_size = 1;
            dd.pixel_format = PixelFormat::D24Unorm_S8Uint;
            dd.depth_stencil = true;
            f.depth = device().create_texture_2d(dd, nullptr, 0);
        }
        return f;
    }

    static void draw(Frame& f, BackendDrawCall& call, float depth_clear = 1.0f) {
        BackendRenderPassDesc pass_desc{};
        pass_desc.color_targets[0] = f.target.get();
        pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
        if (f.depth) {
            pass_desc.depth_stencil_target = f.depth.get();
            pass_desc.clear_depth = true;
            pass_desc.depth_clear_value = depth_clear;
        }
        auto pass = device().create_render_pass_standalone(pass_desc);
        ASSERT_NE(pass, nullptr);
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = 3;
        call.vertex_buffer = f.vertices.get();
        call.vertex_buffers[0] = f.vertices.get();
        call.viewport = {0, 0, static_cast<float>(kSide),
                         static_cast<float>(kSide), 0, 1};
        ASSERT_TRUE(pass->draw(call));
        ASSERT_TRUE(pass->end());
    }

    /// Words of @p source through a compute copy into an R32Uint texture.
    static std::vector<u32> read_words(BackendBuffer* source, u32 count) {
        const std::string msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "kernel void main0(const device uint* words [[buffer(" +
            std::to_string(BACKEND_SRV_BUFFER_BASE) + ")]],\n"
            "    texture2d<uint, access::write> out [[texture(" +
            std::to_string(BACKEND_UAV_TEXTURE_BASE) + ")]],\n"
            "    uint3 id [[thread_position_in_grid]]) {\n"
            "    if (id.x < out.get_width()) out.write(uint4(words[id.x]), "
            "uint2(id.x, 0));\n}\n";
        BackendComputePipelineDesc pd{};
        pd.compute_entry_point = "main0";
        pd.compute_shader_msl = msl;
        pd.cache_key = 0xC0DE7E4Eu;
        auto reader = device().create_compute_pipeline(pd);
        BackendTexture2DDesc td{};
        td.width = count;
        td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = PixelFormat::R32Uint;
        td.unordered_access = true;
        auto texture = device().create_texture_2d(td, nullptr, 0);
        if (!reader || !reader->valid() || !texture) return {};
        BackendDispatchCall call{};
        call.pipeline = reader.get();
        call.group_count_x = count;
        call.group_count_y = call.group_count_z = 1;
        call.buffers[BACKEND_SRV_BUFFER_BASE] = source;
        call.textures[BACKEND_UAV_TEXTURE_BASE] = texture.get();
        if (!device().dispatch(call)) return {};
        std::vector<u32> out(count, 0);
        if (!texture->readback(0, out.data(), count * sizeof(u32))) return {};
        return out;
    }

    static BackendTextureViewDesc whole_2d() {
        BackendTextureViewDesc view{};
        view.dimension = BackendTextureViewDimension::Texture2D;
        view.mip_levels = 1;
        view.array_size = 1;
        return view;
    }
};

// ps_typed_texture: before = image[p]; image[p] = before * 0.5 +
// (0.25, 0.5, 0.75, 1.0) -- ld_uav_typed then store_uav_typed on u1.
TEST_F(PixelUavTest, TypedTextureLoadThenStoreAtEachPixel) {
    const std::string msl = fragment_msl(pixel_uav_ps_typed_texture,
                                         sizeof(pixel_uav_ps_typed_texture));
    ASSERT_FALSE(msl.empty());
    auto pso = pipeline(msl, 0xC0DE0101u);
    ASSERT_TRUE(pso);

    std::vector<float> texels(kFragments * 4);
    for (u32 i = 0; i < kFragments; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            texels[i * 4 + c] = static_cast<float>(i * 4 + c);
        }
    }
    BackendTexture2DDesc td{};
    td.width = td.height = kSide;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA32Float;
    td.unordered_access = true;
    td.shader_resource = true;
    auto image = device().create_texture_2d(td, texels.data(), kSide * 16);
    ASSERT_TRUE(image && image->valid());

    Frame f = frame(false);
    ASSERT_TRUE(f.vertices && f.target);
    BackendDrawCall call{};
    call.pipeline = pso.get();
    call.fragment_uav_textures[1] = image.get();
    call.fragment_uav_texture_views[1] = whole_2d();
    draw(f, call);

    std::vector<float> after(kFragments * 4, -1.0f);
    ASSERT_TRUE(image->readback(0, after.data(), kSide * 16));
    constexpr std::array<float, 4> kAdd{0.25f, 0.5f, 0.75f, 1.0f};
    for (u32 i = 0; i < kFragments; ++i) {
        for (u32 c = 0; c < 4; ++c) {
            EXPECT_EQ(after[i * 4 + c], texels[i * 4 + c] * 0.5f + kAdd[c])
                << "pixel " << i << " component " << c;
        }
    }
}

// ps_typed_buffer on a RWBuffer<uint> at u2: counts[i + 16] = i * 3 + 1,
// InterlockedAdd(counts[0], 1), InterlockedMax(counts[1], i, previous).
TEST_F(PixelUavTest, TypedBufferStoreAndAtomicsThroughATextureBuffer) {
    const std::string msl = fragment_msl(pixel_uav_ps_typed_buffer,
                                         sizeof(pixel_uav_ps_typed_buffer));
    ASSERT_FALSE(msl.empty());
    auto pso = pipeline(msl, 0xC0DE0102u);
    ASSERT_TRUE(pso);

    // Words 0 and 1 start away from zero; word 32 is past every store.
    constexpr u32 kWords = 16 + kFragments + 1;
    std::vector<u32> initial(kWords, kSentinel);
    initial[0] = 100;
    initial[1] = 7;
    auto storage = buffer(initial);
    ASSERT_TRUE(storage);
    auto typed = device().create_texture_buffer(*storage, PixelFormat::R32Uint,
                                                0, kWords,
                                                /*shader_write=*/true);
    ASSERT_TRUE(typed && typed->valid());

    Frame f = frame(false);
    BackendDrawCall call{};
    call.pipeline = pso.get();
    call.fragment_uav_texture_buffers[2] = typed.get();
    draw(f, call);

    const auto words = read_words(storage.get(), kWords);
    ASSERT_EQ(words.size(), kWords);
    EXPECT_EQ(words[0], 100u + kFragments) << "one atomic add per fragment";
    EXPECT_EQ(words[1], kFragments - 1) << "max of 7 and every pixel index";
    for (u32 i = 2; i < 16; ++i) EXPECT_EQ(words[i], kSentinel) << i;
    for (u32 i = 0; i < kFragments; ++i) {
        EXPECT_EQ(words[16 + i], i * 3 + 1) << "pixel " << i;
    }
    EXPECT_EQ(words[kWords - 1], kSentinel);
}

// D3D11.3 16.13: accesses before discard complete; accesses after it
// cannot modify UAVs. Inject discard_nz v0.x (every covered pixel has x>0)
// immediately before/after the fixture's first store, using the same audited
// discard encoding as PixelDiscardUsesExactZeroAndNonzeroTests.
TEST_F(PixelUavTest, DiscardPreservesEarlierStoresAndSuppressesLaterAtomics) {
    for (const u32 mode : {0u, 1u, 2u}) {
        const bool before_store = mode == 1u;
        const bool mixed_lanes = mode == 2u;
        auto container = parse_dxbc(pixel_uav_ps_typed_buffer,
                                    sizeof(pixel_uav_ps_typed_buffer));
        ASSERT_TRUE(container.valid());
        std::vector<u32> tokens(container.bytecode.size() / sizeof(u32));
        std::memcpy(tokens.data(), container.bytecode.data(), container.bytecode.size());
        usize insertion = 0;
        for (usize i = 2; i < tokens.size();) {
            const u32 count = (tokens[i] >> 24) & 0x7fu;
            ASSERT_NE(count, 0u);
            if ((tokens[i] & 0x7ffu) == 164u) { // fxc store_uav_typed
                insertion = before_store ? i : i + count;
                break;
            }
            i += count;
        }
        ASSERT_NE(insertion, 0u);
        if (mixed_lanes) {
            // Replace fxc's final utof with deriv_rtx o0.x, v0.x. The
            // surviving quad lanes must still see d(position.x)/dx = 1
            // when pixel zero has been demoted.
            for (usize i = 2; i < tokens.size();) {
                const u32 count = (tokens[i] >> 24) & 0x7fu;
                if ((tokens[i] & 0x7ffu) == 86u) { // fxc utof
                    ASSERT_EQ(count, 5u);
                    tokens[i] = 11u | (5u << 24); // deriv_rtx
                    tokens[i + 3] = 0x00101006u; // v0.xxxx
                    tokens[i + 4] = 0u;
                }
                i += count;
            }
        }
        tokens.insert(tokens.begin() + static_cast<std::ptrdiff_t>(insertion),
                      {13u | (3u << 24) | (mixed_lanes ? 0u : 0x00040000u),
                       mixed_lanes ? 0x0010000Au : 0x0010100Au, 0u,
                       // A second non-triggering discard never undoes the first.
                       13u | (3u << 24), 0x0010100Au, 0u});
        tokens[1] = static_cast<u32>(tokens.size());
        container.bytecode.resize(tokens.size() * sizeof(u32));
        std::memcpy(container.bytecode.data(), tokens.data(), container.bytecode.size());
        const auto spirv = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(spirv.valid()) << spirv.failure;
        MslCompileOptions options{};
        options.argument_buffers = false;
        options.msl_version = 30100;
        const auto compiled = compile_spirv_to_msl(spirv.spirv.data(),
            spirv.spirv.size(), ShaderStage::Fragment, options);
        ASSERT_TRUE(compiled.valid());
        auto pso = pipeline(compiled.msl_text(), 0xC0DE0110u + mode);
        ASSERT_TRUE(pso && pso->valid());
        constexpr u32 kWords = 16 + kFragments + 1;
        std::vector<u32> initial(kWords, kSentinel);
        initial[0] = 100;
        initial[1] = 7;
        auto storage = buffer(initial);
        ASSERT_TRUE(storage);
        auto typed = device().create_texture_buffer(*storage, PixelFormat::R32Uint,
            0, kWords, /*shader_write=*/true);
        ASSERT_TRUE(typed && typed->valid());
        Frame f = frame(false);
        BackendDrawCall call{};
        call.pipeline = pso.get();
        call.fragment_uav_texture_buffers[2] = typed.get();
        draw(f, call);
        const auto words = read_words(storage.get(), kWords);
        ASSERT_EQ(words.size(), kWords);
        EXPECT_EQ(words[0], mixed_lanes ? 100u + kFragments - 1u : 100u) << "discard suppresses atomic add";
        EXPECT_EQ(words[1], mixed_lanes ? kFragments - 1u : 7u) << "discard suppresses atomic max";
        for (u32 i = 0; i < kFragments; ++i) {
            EXPECT_EQ(words[16 + i], before_store ? kSentinel : i * 3 + 1)
                << "pixel " << i << " before_store=" << before_store;
        }
        EXPECT_EQ(words[kWords - 1], kSentinel);
        std::vector<u8> pixels(kFragments * 4);
        ASSERT_TRUE(f.target->readback(0, pixels.data(), kSide * 4));
        for (u32 i = 0; i < kFragments; ++i) {
            const bool survives = mixed_lanes && i != 0u;
            EXPECT_EQ(pixels[i * 4 + 2], survives ? 255u : 0u)
                << "red derivative / discarded framebuffer at pixel " << i;
            EXPECT_EQ(pixels[i * 4 + 3], survives ? 255u : 0u);
        }
    }
}

// ps_raw_structured: raw.InterlockedAdd(0, 1, slot); raw[(slot + 1) * 4] = i;
// structured[i] = (s.x + i, s.y ^ 0x5a); InterlockedOr(structured[0].y,
// 1 << i).
TEST_F(PixelUavTest, RawAndStructuredLoadsStoresAndAtomics) {
    const std::string msl = fragment_msl(pixel_uav_ps_raw_structured,
                                         sizeof(pixel_uav_ps_raw_structured));
    ASSERT_FALSE(msl.empty());
    auto pso = pipeline(msl, 0xC0DE0103u);
    ASSERT_TRUE(pso);

    constexpr u32 kRawWords = 1 + kFragments + 1;
    auto raw = buffer(std::vector<u32>(kRawWords, 0u));
    std::vector<u32> elements(kFragments * 2);
    for (u32 i = 0; i < kFragments; ++i) {
        elements[i * 2] = 100 + i;
        elements[i * 2 + 1] = 0x700u;
    }
    auto structured = buffer(elements);
    ASSERT_TRUE(raw && structured);

    Frame f = frame(false);
    BackendDrawCall call{};
    call.pipeline = pso.get();
    call.fragment_uav_buffers[3] = raw.get();
    call.fragment_uav_buffers[4] = structured.get();
    draw(f, call);

    const auto raw_words = read_words(raw.get(), kRawWords);
    ASSERT_EQ(raw_words.size(), kRawWords);
    EXPECT_EQ(raw_words[0], kFragments) << "imm_atomic_iadd once per fragment";
    std::vector<u32> slots(raw_words.begin() + 1,
                           raw_words.begin() + 1 + kFragments);
    std::sort(slots.begin(), slots.end());
    for (u32 i = 0; i < kFragments; ++i) {
        EXPECT_EQ(slots[i], i) << "each pixel stored at its own returned slot";
    }
    EXPECT_EQ(raw_words[kRawWords - 1], 0u);

    const auto s = read_words(structured.get(), kFragments * 2);
    ASSERT_EQ(s.size(), kFragments * 2);
    for (u32 i = 1; i < kFragments; ++i) {
        EXPECT_EQ(s[i * 2], 100 + i + i) << "element " << i;
        EXPECT_EQ(s[i * 2 + 1], 0x700u ^ 0x5au) << "element " << i;
    }
    // Element 0's .y races pixel 0's plain store against every pixel's
    // InterlockedOr (no ordering between invocations); pixel 0's own OR comes
    // after its store in program order, so bit 0 is always set.
    EXPECT_EQ(s[0], 100u);
    EXPECT_NE(s[1] & 1u, 0u);
}

// ps_early_coherent: [earlydepthstencil] with InterlockedMax(coverage[p],
// uint(z * 65535)). z is 0.5, so a pixel that runs writes 32767 (ftou
// truncates 32767.5). With the depth buffer cleared to 0.25 and a Less test the
// pixel fails -- and because the test ran EARLY, its UAV write never happens.
TEST_F(PixelUavTest, EarlyDepthStencilSuppressesUavWritesOfFailedPixels) {
    const std::string msl = fragment_msl(pixel_uav_ps_early_coherent,
                                         sizeof(pixel_uav_ps_early_coherent));
    ASSERT_FALSE(msl.empty());
    EXPECT_NE(msl.find("early_fragment_tests"), std::string::npos) << msl;
    auto pso = pipeline(msl, 0xC0DE0104u, /*depth=*/true);
    ASSERT_TRUE(pso);

    for (const auto& [clear, expected] :
         {std::pair{1.0f, 32767u}, std::pair{0.25f, 0u}}) {
        SCOPED_TRACE(clear);
        BackendTexture2DDesc td{};
        td.width = td.height = kSide;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = PixelFormat::R32Uint;
        td.unordered_access = true;
        td.shader_resource = true;
        const std::vector<u32> zeros(kFragments, 0u);
        auto coverage = device().create_texture_2d(td, zeros.data(), kSide * 4);
        ASSERT_TRUE(coverage && coverage->valid());

        Frame f = frame(true);
        ASSERT_TRUE(f.depth);
        BackendDrawCall call{};
        call.pipeline = pso.get();
        call.depth_test_enabled = true;
        call.depth_write_enabled = false;
        call.depth_compare = BackendCompare::Less;
        call.fragment_uav_textures[1] = coverage.get();
        call.fragment_uav_texture_views[1] = whole_2d();
        draw(f, call, clear);

        std::vector<u32> after(kFragments, kSentinel);
        ASSERT_TRUE(coverage->readback(0, after.data(), kSide * 4));
        for (u32 i = 0; i < kFragments; ++i) {
            EXPECT_EQ(after[i], expected) << "pixel " << i;
        }
    }
}

}  // namespace

// D3D11 rasterizes a draw with no RTV and no DSV bound (UAV-only rendering):
// the viewport bounds it and the pixel shader's UAV writes are its only
// output. Metal expresses that as a render pass with no attachments
// (renderTargetWidth/Height, defaultRasterSampleCount) and a pipeline with no
// attachment formats. Same shader as above: one InterlockedAdd per fragment.
TEST_F(PixelUavTest, AttachmentlessPassRunsTheFragmentStage) {
    const std::string msl = fragment_msl(pixel_uav_ps_raw_structured,
                                         sizeof(pixel_uav_ps_raw_structured));
    ASSERT_FALSE(msl.empty());
    BackendPipelineDesc pdesc{};
    pdesc.vertex_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_in { float2 position [[attribute(0)]]; };
        struct main0_out { float4 position [[position]]; };
        vertex main0_out main0(main0_in in [[stage_in]]) {
            main0_out out = {};
            out.position = float4(in.position, 0.5, 1.0);
            return out;
        }
    )";
    pdesc.fragment_shader_msl = msl;
    pdesc.vertex_entry_point = "main0";
    pdesc.fragment_entry_point = "main0";
    pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
    pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
    pdesc.attachmentless = true;  // no colour format, no depth
    pdesc.cache_key = 0xC0DE0A77u;
    auto pso = device().create_pipeline(pdesc);
    ASSERT_TRUE(pso && pso->valid());

    constexpr u32 kRawWords = 1 + kFragments + 1;
    auto raw = buffer(std::vector<u32>(kRawWords, 0u));
    auto structured = buffer(std::vector<u32>(kFragments * 2, 0u));
    ASSERT_TRUE(raw && structured);
    static const float positions[]{-1, -1, 3, -1, -1, 3};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    auto vertices = device().create_buffer(vb, positions);

    BackendRenderPassDesc pass_desc{};
    pass_desc.attachmentless_width = kSide;
    pass_desc.attachmentless_height = kSide;
    pass_desc.attachmentless_samples = 1;
    auto pass = device().create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr) << "a pass with no attachments must be creatable";
    BackendDrawCall call{};
    call.pipeline = pso.get();
    call.fragment_uav_buffers[3] = raw.get();
    call.fragment_uav_buffers[4] = structured.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.vertex_buffer = vertices.get();
    call.vertex_buffers[0] = vertices.get();
    call.viewport = {0, 0, static_cast<float>(kSide),
                     static_cast<float>(kSide), 0, 1};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    const auto raw_words = read_words(raw.get(), kRawWords);
    ASSERT_EQ(raw_words.size(), kRawWords);
    EXPECT_EQ(raw_words[0], kFragments)
        << "every pixel of the 4x4 viewport runs the fragment stage once";
}

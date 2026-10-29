// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_typed_buffer_offset.cpp
/// @brief Typed buffer views at element offsets Metal cannot alias directly.
///
/// Learn (D3D11_BUFFER_SRV / D3D11_BUFFER_UAV): FirstElement is any element,
/// so the view's byte offset is only a multiple of the element size; Metal's
/// linear textures need an aligned base (16 bytes on Apple GPUs). The view is
/// a texture over the buffer's OWN storage from the largest aligned offset at
/// or below FirstElement -- BackendTextureBuffer::element_bias() elements in
/// front of it -- and the translated shader adds the bias to every index
/// (DxbcTranslationOptions::typed_buffer_bias). It used to be an aligned
/// scratch copy refreshed before each draw: a copy recorded inside the
/// draw's render pass, which split every pass that read such a view and
/// could not hold a UAV's writes at all. D3D11 orders every read after every
/// earlier write; a texture over the storage does so by construction.
///
/// Also: the R32G32B32 typed-buffer view and the any-face cube-array view of
/// the D3D11 view contract (area 03), both served through views like these.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/delta_record.hpp"
#include "graphics/contracts/slim_record.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "rgb32_buffer_shaders.h"
#include "typed_buffer_offset_shaders.h"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

/// A push block (kPushBlockBytes, as words) whose size table holds each
/// view's entry -- its element bias, and kTypedBufferAlphaMissing for a
/// format without alpha: t# at its slot, u# at kGuardSizeSrvSlots + slot.
struct BiasBlock {
    std::vector<u32> words =
        std::vector<u32>(kPushBlockBytes / 4, 0u);
    static u32 entry(const BackendTextureBuffer& view, bool has_alpha) {
        return view.element_bias() | (has_alpha ? 0u : kTypedBufferAlphaMissing);
    }
    void srv(u32 slot, const BackendTextureBuffer& view, bool has_alpha) {
        words[kGuardSizeTableOffset / 4 + slot] = entry(view, has_alpha);
    }
    void uav(u32 slot, const BackendTextureBuffer& view, bool has_alpha) {
        words[kGuardSizeTableOffset / 4 + kGuardSizeSrvSlots + slot] =
            entry(view, has_alpha);
    }
};

/// fxc's DXBC through the translator as the D3D11 frontend runs it: bounds
/// guards and typed-buffer biases from the push block, flat bindings.
std::string translate(const unsigned char* blob, usize size,
                      ShaderStage stage, u32 rgb32 = 0) {
    const auto container = parse_dxbc(blob, size);
    EXPECT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.guard_buffer_bounds = true;
    options.typed_buffer_bias = true;
    options.rgb32_buffer_srvs = rgb32;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    if (spirv.uses_typed_uav_atomics) {
        // Texture atomics (MSL 3.1), as the D3D11 frontend raises it.
        if (flat.msl_version < 30100u) flat.msl_version = 30100u;
    }
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(), stage, flat);
    EXPECT_TRUE(msl.valid());
    return msl.valid() ? msl.msl_text() : std::string{};
}

std::unique_ptr<BackendComputePipeline> compute_pipeline(
    const unsigned char* blob, usize size, u64 key) {
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = translate(blob, size, ShaderStage::Compute);
    pd.cache_key = key;
    return MetalDevice::instance().create_compute_pipeline(pd);
}

/// Production records draws with delta-record and pass-fold on: pin them.
class TypedBufferOffsetRecording : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal";
        delta_record::force_all_for_test(delta_record::Force::On);
        slim_record::force_all_for_test(slim_record::Force::On);
    }
    void TearDown() override {
        delta_record::force_all_for_test(delta_record::Force::None);
        slim_record::force_all_for_test(slim_record::Force::None);
    }
};

/// Learn (D3D11_BUFFER_SRV): the element size comes from the format. The
/// bias is the view's byte offset past the largest aligned offset below
/// it, in elements, so it is less than one alignment's worth of elements
/// and the texture's base is aligned: on Apple GPUs (16-byte alignment)
/// FirstElement 270 of R32 is bias 2, 271 bias 3.
void expect_bias_shape(const BackendTextureBuffer& view, u32 first,
                       u32 element_bytes) {
    const u32 bias = view.element_bias();
    EXPECT_LE(bias, first);
    EXPECT_LT(bias * element_bytes, 256u);
    EXPECT_EQ(((first - bias) * element_bytes) % 16u, 0u)
        << "the texture base is not 16-byte aligned";
}

}  // namespace

// A compute shader writes the buffer; a second reads it through a view at
// element 3 (12 bytes: not a 16-byte multiple). The view is a texture over
// the buffer's own storage, element_bias() elements in front of element 3,
// so the read sees the first dispatch's writes.
TEST(TypedBufferOffset, UnalignedViewSeesEarlierGpuWrites) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    constexpr u32 kWords = 64;
    constexpr u32 kFirst = 3;  // 12 bytes: not a 16-byte multiple
    constexpr u32 kCount = 32;

    BackendBufferDesc bd{};
    bd.byte_width = kWords * 4;
    bd.shader_resource = true;
    bd.unordered_access = true;
    const std::vector<u32> zeros(kWords, 0u);
    auto source = device.create_buffer(bd, zeros.data());
    auto out = device.create_buffer(bd, zeros.data());
    ASSERT_TRUE(source && out);
    auto view = device.create_texture_buffer(*source, PixelFormat::R32Uint,
                                             kFirst, kCount, false);
    ASSERT_TRUE(view && view->valid());
    expect_bias_shape(*view, kFirst, 4);
    const u32 bias = view->element_bias();
    EXPECT_NE(bias, 0u) << "element 3 of R32 is not 16-byte aligned";

    const auto kernel = [&](const std::string& body, u64 key) {
        BackendComputePipelineDesc pd{};
        pd.compute_entry_point = "main0";
        pd.compute_shader_msl = body;
        pd.cache_key = key;
        return device.create_compute_pipeline(pd);
    };
    // Writer: words[i] = 1000 + i, entirely on the GPU.
    auto writer = kernel(
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(device uint* words [[buffer(" +
            std::to_string(BACKEND_UAV_BUFFER_BASE) +
            ")]], uint3 id [[thread_position_in_grid]]) {\n"
            "  words[id.x] = 1000u + id.x;\n}\n",
        0x7B0FF5E1u);
    // Reader: out[i] = element i of the view -- texel i + bias -- and the
    // texture's width, which must be bias + kCount.
    auto reader = kernel(
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(texture_buffer<uint> view [[texture(0)]],\n"
        "    device uint* out [[buffer(" +
            std::to_string(BACKEND_UAV_BUFFER_BASE) +
            ")]], uint3 id [[thread_position_in_grid]]) {\n"
            "  out[id.x] = view.read(id.x + " + std::to_string(bias) +
            "u).x;\n  if (id.x == 0) out[63] = view.get_width();\n}\n",
        0x7B0FF5E2u + (static_cast<u64>(bias) << 32));
    ASSERT_TRUE(writer && writer->valid() && reader && reader->valid());

    BackendDispatchCall write{};
    write.pipeline = writer.get();
    write.group_count_x = kWords;
    write.group_count_y = write.group_count_z = 1;
    write.buffers[BACKEND_UAV_BUFFER_BASE] = source.get();
    ASSERT_TRUE(device.dispatch(write));

    BackendDispatchCall read{};
    read.pipeline = reader.get();
    read.group_count_x = kCount;
    read.group_count_y = read.group_count_z = 1;
    read.texture_buffers[0] = view.get();
    read.buffers[BACKEND_UAV_BUFFER_BASE] = out.get();
    ASSERT_TRUE(device.dispatch(read));

    ASSERT_TRUE(device.wait_idle());
    std::vector<u32> got(kWords, 0xEEEEEEEEu);
    ASSERT_TRUE(out->read(0, got.data(), kWords * 4));
    for (u32 i = 0; i < kCount; ++i) {
        EXPECT_EQ(got[i], 1000u + kFirst + i) << "element " << i;
    }
    EXPECT_EQ(got[63], bias + kCount) << "the texture ends where the view ends";
}

// The same unaligned view read by a DRAW's pixel shader. It used to be served
// from a scratch copy refreshed by a copy recorded inside the draw's render
// pass, which ended the pass's encoder (and, before the queue reopened passes,
// dropped the draw). The view is now the buffer's own storage: no copy, the
// pass is never broken, and the draw is rendered.
TEST(TypedBufferOffset, ADrawReadingAnUnalignedViewIsRendered) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    constexpr u32 kFirst = 3;  // 12 bytes: not on the 16-byte alignment
    constexpr unsigned kSide = 4;
    std::vector<u32> words(16);
    for (u32 i = 0; i < words.size(); ++i) words[i] = 10u * i;
    BackendBufferDesc bd{};
    bd.byte_width = static_cast<unsigned>(words.size() * 4);
    bd.shader_resource = true;
    auto source = device.create_buffer(bd, words.data());
    ASSERT_TRUE(source);
    auto view = device.create_texture_buffer(*source, PixelFormat::R32Uint,
                                             kFirst, 4, false);
    ASSERT_TRUE(view && view->valid());
    const u32 bias = view->element_bias();

    BackendTexture2DDesc td{};
    td.width = td.height = kSide;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(target);
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = R"(
        #include <metal_stdlib>
        using namespace metal;
        struct main0_out { float4 position [[position]]; };
        vertex main0_out main0(uint vid [[vertex_id]]) {
            const float2 p[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
            main0_out out = {};
            out.position = float4(p[vid], 0.5, 1.0);
            return out;
        }
    )";
    pd.fragment_shader_msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 main0(texture_buffer<uint> view [[texture(0)]]) {\n"
        "    return float4(float(view.read(" + std::to_string(bias) +
        "u).x) / 255.0, 0.0, 0.0, 1.0);\n}\n";
    pd.vertex_entry_point = "main0";
    pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::RGBA8Unorm;
    pd.cache_key = 0x7B0FF5E4u + (static_cast<u64>(bias) << 32);
    auto pso = device.create_pipeline(pd);
    ASSERT_TRUE(pso && pso->valid());
    ASSERT_TRUE(device.wait_idle());

    auto& queue = MetalSubmitQueue::instance();
    const u64 reopened = queue.draws_reopened();
    const u64 dropped = queue.draws_dropped();
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Unorm;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_TRUE(pass && pass->valid());
    BackendDrawCall call{};
    call.pipeline = pso.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.fragment_srv_texture_buffers[0] = view.get();
    call.viewport = {0, 0, static_cast<float>(kSide),
                     static_cast<float>(kSide), 0, 1};
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());
    ASSERT_TRUE(device.wait_idle());

    std::vector<u32> pixels(kSide * kSide, 0u);
    ASSERT_TRUE(target->readback(0, pixels.data(), kSide * 4));
    for (const u32 pixel : pixels) {
        // R = element kFirst of the buffer = 30, alpha 255.
        EXPECT_EQ(pixel, 0xFF00001Eu);
    }
    EXPECT_EQ(queue.draws_reopened() - reopened, 0u)
        << "something split the draw's pass";
    EXPECT_EQ(queue.draws_dropped() - dropped, 0u);
}

// CSReadViews (fxc) through the translator, two views of one buffer per
// format at FirstElements off the 16-byte alignment -- R32_FLOAT 270 and
// 271, R16_FLOAT 270 and 1, R8G8B8A8_UNORM 270 and 3 -- and
// R32G32B32A32_FLOAT (every element aligned). Threads 0-7 read elements
// 0-7, threads 8-63 past the eight-element views, 64-67 0xFFFFFFFF - k:
// Learn (ld) -- past the view, 0 in the format's components and the default
// (alpha 1) in the missing ones, never the elements in front of the view
// that index + bias would wrap to. GetDimensions (bufinfo) is NumElements.
TEST_F(TypedBufferOffsetRecording, TranslatedReadsHonourFormatAndBounds) {
    auto& device = MetalDevice::instance();
    auto pipeline = compute_pipeline(tbo_cs_read_views,
                                     sizeof(tbo_cs_read_views), 0x7B0FF5F0u);
    ASSERT_TRUE(pipeline && pipeline->valid());
    constexpr u32 kThreads = 68;
    constexpr u32 kViewElements = 8;
    constexpr u32 kBytes = 4352;
    BackendBufferDesc od{};
    od.byte_width = kThreads * 48;
    od.unordered_access = true;
    od.shader_resource = true;
    const std::vector<u8> fill(od.byte_width, 0xEEu);
    auto out = device.create_buffer(od, fill.data());
    ASSERT_TRUE(out);

    struct Case {
        const char* name;
        PixelFormat format;
        u32 element_bytes;
        std::array<u32, 2> firsts;
        bool has_alpha;
    };
    const std::array<Case, 4> cases{{
        {"R32_FLOAT", PixelFormat::R32Float, 4, {270, 271}, false},
        {"R16_FLOAT", PixelFormat::R16Float, 2, {270, 1}, false},
        {"R8G8B8A8_UNORM", PixelFormat::RGBA8Unorm, 4, {270, 3}, true},
        {"R32G32B32A32_FLOAT", PixelFormat::RGBA32Float, 16, {37, 200}, true},
    }};
    for (const Case& test : cases) {
        // Bytes: R32/RGBA32 words w = float(w); R16 halves e = e % 1024
        // (exact); RGBA8 byte k = k & 0xFF.
        std::vector<u8> bytes(kBytes);
        for (u32 k = 0; k < kBytes; ++k) bytes[k] = static_cast<u8>(k);
        if (test.element_bytes != 2 && test.format != PixelFormat::RGBA8Unorm) {
            for (u32 w = 0; w < kBytes / 4; ++w) {
                const float v = static_cast<float>(w);
                std::memcpy(&bytes[w * 4], &v, 4);
            }
        } else if (test.element_bytes == 2) {
            for (u32 e = 0; e < kBytes / 2; ++e) {
                const u32 v = e % 1024;
                u16 half = 0;
                if (v != 0) {
                    u32 exp = 0;
                    while ((v >> (exp + 1)) != 0) ++exp;
                    half = static_cast<u16>(((15 + exp) << 10) |
                                            ((v << (10 - exp)) & 0x3FFu));
                }
                std::memcpy(&bytes[e * 2], &half, 2);
            }
        }
        BackendBufferDesc bd{};
        bd.byte_width = kBytes;
        bd.shader_resource = true;
        auto data = device.create_buffer(bd, bytes.data());
        ASSERT_TRUE(data);
        auto v0 = device.create_texture_buffer(*data, test.format,
                                               test.firsts[0], kViewElements,
                                               false);
        auto v1 = device.create_texture_buffer(*data, test.format,
                                               test.firsts[1], kViewElements,
                                               false);
        ASSERT_TRUE(v0 && v0->valid() && v1 && v1->valid()) << test.name;
        expect_bias_shape(*v0, test.firsts[0], test.element_bytes);
        expect_bias_shape(*v1, test.firsts[1], test.element_bytes);
        BiasBlock push;
        push.srv(0, *v0, test.has_alpha);
        push.srv(1, *v1, test.has_alpha);
        BackendDispatchCall call{};
        call.pipeline = pipeline.get();
        call.group_count_x = kThreads;
        call.group_count_y = call.group_count_z = 1;
        call.texture_buffers[0] = v0.get();
        call.texture_buffers[1] = v1.get();
        call.buffers[BACKEND_UAV_BUFFER_BASE] = out.get();
        call.constants = push.words.data();
        call.constants_size = push.words.size() * 4;
        ASSERT_TRUE(device.dispatch(call));
        ASSERT_TRUE(device.wait_idle());
        std::vector<u32> words(kThreads * 12);
        ASSERT_TRUE(out->read(0, words.data(), words.size() * 4));

        const auto expected = [&](u32 first, u32 i) {
            std::array<float, 4> v{0, 0, 0, test.has_alpha ? 0.0f : 1.0f};
            if (i >= kViewElements) return v;
            const u32 e = first + i;
            if (test.format == PixelFormat::R32Float) {
                v[0] = static_cast<float>(e);
            } else if (test.format == PixelFormat::R16Float) {
                v[0] = static_cast<float>(e % 1024);
            } else if (test.format == PixelFormat::RGBA8Unorm) {
                for (u32 c = 0; c < 4; ++c) {
                    v[c] = static_cast<float>((e * 4 + c) & 0xFF) / 255.0f;
                }
            } else {
                for (u32 c = 0; c < 4; ++c) {
                    v[c] = static_cast<float>(e * 4 + c);
                }
            }
            return v;
        };
        for (u32 t = 0; t < kThreads; ++t) {
            const u32 i = t < 64 ? t : 0xFFFFFFFFu - (t - 64);
            for (u32 view = 0; view < 2; ++view) {
                const auto want = expected(test.firsts[view], i);
                for (u32 c = 0; c < 4; ++c) {
                    float got = 0;
                    std::memcpy(&got, &words[t * 12 + view * 4 + c], 4);
                    EXPECT_NEAR(got, want[c], 1e-6f)
                        << test.name << " t" << view << " thread " << t
                        << " component " << c;
                }
            }
            EXPECT_EQ(words[t * 12 + 8], kViewElements) << test.name;
            EXPECT_EQ(words[t * 12 + 9], kViewElements) << test.name;
        }
        // The two edges by name: index NumElements, and NumElements + bias
        // - 1, the last index a test against the texture's width (bias +
        // NumElements) instead of the view's would let through. Both read
        // exactly (0, 0, 0, default alpha), never buffer contents.
        const BackendTextureBuffer* views[2] = {v0.get(), v1.get()};
        for (u32 view = 0; view < 2; ++view) {
            const u32 bias = views[view]->element_bias();
            for (const u32 i : {kViewElements, kViewElements + bias - 1}) {
                if (i < kViewElements) continue;  // bias 0: one edge only
                for (u32 c = 0; c < 4; ++c) {
                    float got = 0;
                    std::memcpy(&got, &words[i * 12 + view * 4 + c], 4);
                    EXPECT_EQ(got, c == 3 && !test.has_alpha ? 1.0f : 0.0f)
                        << test.name << " t" << view << " bias " << bias
                        << ": ld at index " << i << " component " << c;
                }
            }
        }
    }
}

// CSWriteViews (fxc) through the translator: RWBuffer<float> u1 at
// FirstElement 270 and RWBuffer<uint> u2 at 271 (write=1, both off the
// alignment). ld_uav_typed, store_uav_typed and imm_atomic_iadd reach
// exactly elements first + i; threads past the eight-element views write
// nothing (Learn, store_uav_typed / imm_atomic_iadd), the wrapping ones
// included, so every other element keeps its value; ld_uav_typed past the
// view reads 0. GetDimensions is 8.
TEST_F(TypedBufferOffsetRecording, TranslatedTypedUavsWriteOnlyTheirElements) {
    auto& device = MetalDevice::instance();
    auto pipeline = compute_pipeline(tbo_cs_write_views,
                                     sizeof(tbo_cs_write_views), 0x7B0FF5F1u);
    ASSERT_TRUE(pipeline && pipeline->valid());
    constexpr u32 kThreads = 68;
    constexpr u32 kViewElements = 8;
    constexpr u32 kElements = 1088;
    BackendBufferDesc od{};
    od.byte_width = kThreads * 16;
    od.unordered_access = true;
    od.shader_resource = true;
    const std::vector<u8> fill(od.byte_width, 0xEEu);
    auto dump = device.create_buffer(od, fill.data());
    std::vector<float> floats(kElements);
    std::vector<u32> uints(kElements);
    for (u32 e = 0; e < kElements; ++e) {
        floats[e] = 100000.0f + static_cast<float>(e);
        uints[e] = 7000u + e;
    }
    BackendBufferDesc bd{};
    bd.byte_width = kElements * 4;
    bd.shader_resource = true;
    bd.unordered_access = true;
    auto fbuf = device.create_buffer(bd, floats.data());
    auto ubuf = device.create_buffer(bd, uints.data());
    ASSERT_TRUE(dump && fbuf && ubuf);
    auto fview = device.create_texture_buffer(*fbuf, PixelFormat::R32Float,
                                              270, kViewElements, true);
    auto uview = device.create_texture_buffer(*ubuf, PixelFormat::R32Uint,
                                              271, kViewElements, true);
    ASSERT_TRUE(fview && fview->valid() && uview && uview->valid())
        << "a writable view at an unaligned FirstElement was refused";
    expect_bias_shape(*fview, 270, 4);
    expect_bias_shape(*uview, 271, 4);
    BiasBlock push;
    push.uav(1, *fview, false);  // R32_FLOAT
    push.uav(2, *uview, false);  // R32_UINT
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = kThreads;
    call.group_count_y = call.group_count_z = 1;
    call.buffers[BACKEND_UAV_BUFFER_BASE] = dump.get();
    call.texture_buffers[BACKEND_UAV_TEXTURE_BASE + 1] = fview.get();
    call.texture_buffers[BACKEND_UAV_TEXTURE_BASE + 2] = uview.get();
    call.constants = push.words.data();
    call.constants_size = push.words.size() * 4;
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());

    std::vector<u32> words(kThreads * 4);
    ASSERT_TRUE(dump->read(0, words.data(), words.size() * 4));
    for (u32 t = 0; t < kThreads; ++t) {
        float before = 0;
        std::memcpy(&before, &words[t * 4], 4);
        if (t < kViewElements) {
            EXPECT_EQ(before, 100000.0f + static_cast<float>(270 + t)) << t;
            EXPECT_EQ(words[t * 4 + 1], 7000u + 271 + t) << t;
        } else {
            EXPECT_EQ(before, 0.0f) << "thread " << t << " read past the view";
            // The imm_atomic past the view ran no atomic: it returns 0.
            EXPECT_EQ(words[t * 4 + 1], 0u)
                << "thread " << t << " atomic past the view";
        }
        EXPECT_EQ(words[t * 4 + 2], kViewElements) << t;
        EXPECT_EQ(words[t * 4 + 3], kViewElements) << t;
    }
    std::vector<float> fgot(kElements);
    std::vector<u32> ugot(kElements);
    ASSERT_TRUE(fbuf->read(0, fgot.data(), kElements * 4));
    ASSERT_TRUE(ubuf->read(0, ugot.data(), kElements * 4));
    for (u32 e = 0; e < kElements; ++e) {
        const bool in_f = e >= 270 && e < 270 + kViewElements;
        EXPECT_EQ(fgot[e], in_f ? 1000.0f + static_cast<float>(e - 270)
                                : 100000.0f + static_cast<float>(e))
            << "float element " << e;
        const bool in_u = e >= 271 && e < 271 + kViewElements;
        EXPECT_EQ(ugot[e], 7000u + e + (in_u ? 1u : 0u)) << "uint element " << e;
    }
    // The edges by name, per view: index NumElements and NumElements + bias
    // - 1 (the last a test against the texture's width would pass).
    // ld_uav_typed reads 0, the atomic returns 0, and neither the store nor
    // the atomic changes the buffer element that index would name.
    const u32 fbias = fview->element_bias();
    const u32 ubias = uview->element_bias();
    ASSERT_GE(fbias, 2u) << "FirstElement 270 of R32 is 8 bytes off 16";
    ASSERT_GE(ubias, 2u) << "FirstElement 271 of R32 is 12 bytes off 16";
    for (const u32 i : {kViewElements, kViewElements + fbias - 1}) {
        float before = 0;
        std::memcpy(&before, &words[i * 4], 4);
        EXPECT_EQ(before, 0.0f) << "ld_uav_typed at index " << i;
        EXPECT_EQ(fgot[270 + i], 100000.0f + static_cast<float>(270 + i))
            << "store_uav_typed at index " << i << " wrote element "
            << 270 + i;
    }
    for (const u32 i : {kViewElements, kViewElements + ubias - 1}) {
        EXPECT_EQ(words[i * 4 + 1], 0u) << "imm_atomic_iadd at index " << i;
        EXPECT_EQ(ugot[271 + i], 7000u + 271 + i)
            << "imm_atomic_iadd at index " << i << " wrote element "
            << 271 + i;
    }
}

// Two draws of ONE render pass read the view at FirstElement 270 (byte
// 1080, UE's landscape layout) through PSReadView (fxc): the first with the
// buffer holding A, the second after a DISCARD update to B, then a third
// after an in-place (NO_OVERWRITE-style) update of a disjoint view's
// elements, and a fourth reading four views at different FirstElements
// (PSReadFour). Each draw sees the contents as of its own position in the
// stream (D3D11.3: commands execute in submission order).
TEST_F(TypedBufferOffsetRecording, DrawsOfOnePassSeeTheBufferAsOfEachDraw) {
    auto& device = MetalDevice::instance();
    constexpr unsigned kWidth = 16;
    constexpr unsigned kHeight = 2;
    constexpr u32 kElements = 1088;
    const auto ramp = [](float base) {
        std::vector<float> v(kElements);
        for (u32 e = 0; e < kElements; ++e) v[e] = base + static_cast<float>(e);
        return v;
    };
    const auto a = ramp(100000.0f);
    BackendBufferDesc bd{};
    bd.byte_width = kElements * 4;
    bd.shader_resource = true;
    bd.cpu_writable = true;
    auto data = device.create_buffer(bd, a.data());
    ASSERT_TRUE(data);
    auto view = device.create_texture_buffer(*data, PixelFormat::R32Float, 270,
                                             270, false);
    auto later = device.create_texture_buffer(*data, PixelFormat::R32Float,
                                              541, 8, false);
    ASSERT_TRUE(view && view->valid() && later && later->valid());
    expect_bias_shape(*view, 270, 4);
    expect_bias_shape(*later, 541, 4);
    const std::array<u32, 4> firsts{270, 541, 812, 3};
    std::array<std::unique_ptr<BackendTextureBuffer>, 4> four;
    for (u32 i = 0; i < 4; ++i) {
        four[i] = device.create_texture_buffer(*data, PixelFormat::R32Float,
                                               firsts[i], kWidth, false);
        ASSERT_TRUE(four[i] && four[i]->valid());
    }

    BackendTexture2DDesc td{};
    td.width = kWidth;
    td.height = kHeight;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA32Float;
    td.render_target = true;
    auto target = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(target);
    const auto pipeline = [&](const unsigned char* ps, usize size, u64 key) {
        BackendPipelineDesc pd{};
        pd.vertex_shader_msl =
            translate(tbo_vs_full, sizeof(tbo_vs_full), ShaderStage::Vertex);
        pd.fragment_shader_msl = translate(ps, size, ShaderStage::Fragment);
        pd.vertex_entry_point = "main0";
        pd.fragment_entry_point = "main0";
        pd.color_format = PixelFormat::RGBA32Float;
        pd.cache_key = key;
        return device.create_pipeline(pd);
    };
    auto read_view = pipeline(tbo_ps_read_view, sizeof(tbo_ps_read_view),
                              0x7B0FF5F2u);
    auto read_four = pipeline(tbo_ps_read_four, sizeof(tbo_ps_read_four),
                              0x7B0FF5F3u);
    ASSERT_TRUE(read_view && read_view->valid() && read_four &&
                read_four->valid());

    ASSERT_TRUE(target->clear_render_target({-1, -1, -1, -1}));
    auto& queue = MetalSubmitQueue::instance();
    const u64 reopened = queue.draws_reopened();
    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA32Float;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    ASSERT_TRUE(pass && pass->valid());
    const auto draw = [&](const BackendPipeline* pso,
                          const BackendTextureBuffer* const* views,
                          u32 count, float x, float width) {
        BiasBlock push;
        BackendDrawCall call{};
        call.pipeline = pso;
        call.topology = BackendPrimitiveTopology::TriangleList;
        call.vertex_count = 3;
        for (u32 i = 0; i < count; ++i) {
            call.fragment_srv_texture_buffers[i] = views[i];
            push.srv(i, *views[i], false);  // R32_FLOAT
        }
        call.fragment_constants = push.words.data();
        call.fragment_constants_size = push.words.size() * 4;
        call.viewport = {x, 0, width, static_cast<float>(kHeight), 0, 1};
        return pass->draw(call);
    };
    const BackendTextureBuffer* first_view[1] = {view.get()};
    ASSERT_TRUE(draw(read_view.get(), first_view, 1, 0, 8));
    const auto b = ramp(200000.0f);
    ASSERT_TRUE(data->update(0, b.data(), kElements * 4, BufferWrite::Discard));
    ASSERT_TRUE(draw(read_view.get(), first_view, 1, 8, 8));
    ASSERT_TRUE(pass->end());
    ASSERT_TRUE(device.wait_idle());
    std::vector<std::array<float, 4>> texels(kWidth * kHeight);
    ASSERT_TRUE(target->readback(0, texels.data(), kWidth * 16));
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const float base = x < 8 ? 100000.0f : 200000.0f;
            const auto& t = texels[y * kWidth + x];
            EXPECT_EQ(t[0], base + static_cast<float>(270 + x % 8))
                << "pixel " << x << "," << y;
            EXPECT_EQ(t[1], 0.0f);
            EXPECT_EQ(t[2], 0.0f);
            EXPECT_EQ(t[3], 1.0f);
        }
    }
    EXPECT_EQ(queue.draws_reopened() - reopened, 0u)
        << "something split the pass between the draws";

    // An in-place write of elements no recorded draw reads (D3D11
    // NO_OVERWRITE), a draw of the disjoint view, then all four views.
    const auto c = ramp(300000.0f);
    ASSERT_TRUE(data->update(541 * 4, c.data() + 541, 8 * 4,
                             BufferWrite::NoOverwrite));
    auto second = device.create_render_pass_standalone(pass_desc);
    ASSERT_TRUE(second && second->valid());
    pass = std::move(second);
    const BackendTextureBuffer* later_view[1] = {later.get()};
    ASSERT_TRUE(draw(read_view.get(), later_view, 1, 0, 8));
    ASSERT_TRUE(pass->end());
    ASSERT_TRUE(device.wait_idle());
    ASSERT_TRUE(target->readback(0, texels.data(), kWidth * 16));
    for (u32 x = 0; x < 8; ++x) {
        EXPECT_EQ(texels[x][0], 300000.0f + static_cast<float>(541 + x)) << x;
    }

    auto third = device.create_render_pass_standalone(pass_desc);
    ASSERT_TRUE(third && third->valid());
    pass = std::move(third);
    const BackendTextureBuffer* four_views[4] = {four[0].get(), four[1].get(),
                                                 four[2].get(), four[3].get()};
    ASSERT_TRUE(draw(read_four.get(), four_views, 4, 0, kWidth));
    ASSERT_TRUE(pass->end());
    ASSERT_TRUE(device.wait_idle());
    ASSERT_TRUE(target->readback(0, texels.data(), kWidth * 16));
    for (u32 x = 0; x < kWidth; ++x) {
        for (u32 v = 0; v < 4; ++v) {
            // B everywhere except the eight NO_OVERWRITE elements from 541.
            const u32 e = firsts[v] + x;
            const float base =
                e >= 541 && e < 549 ? 300000.0f : 200000.0f;
            EXPECT_EQ(texels[x][v], base + static_cast<float>(e))
                << "pixel " << x << " view t" << v;
        }
    }
}

// Area 03 W17: R32G32B32_FLOAT/UINT/SINT are required Buffer<> formats (Learn,
// "Format support for Direct3D Feature Level 11.0 hardware") with no Metal
// texel format. The frontend aliases such a view as an R32 texture buffer of
// three texels per element and the translator variant
// (DxbcTranslationOptions::rgb32_buffer_srvs) reads the three back as one
// element with alpha 1, 0 past the view, and GetDimensions / 3. Expected
// words: Windows (WARP) running the same shader,
// tools/oracle_probe/d3d11_a03_rgb32_buffer_probe.expected.
TEST(TypedBufferOffset, Rgb32ViewReadsThreeTexelsPerElement) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    const auto container = parse_dxbc(rgb32_cs_load, sizeof(rgb32_cs_load));
    ASSERT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.rgb32_buffer_srvs = 0x7u;  // t0, t1, t2
    // FLOAT's view starts at texel 3, SINT's at 6: off the alignment, so
    // their biases come from the push block, as the frontend sends them.
    options.typed_buffer_bias = true;
    const auto spirv = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(),
                                          ShaderStage::Compute, flat);
    ASSERT_TRUE(msl.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl = msl.msl_text();
    pd.cache_key = 0x7B0FF5E3u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();

    // The probe's 24 source words: 1.5 + i as floats, then integers.
    std::vector<u32> words(24);
    for (u32 i = 0; i < 24; ++i) {
        if (i < 16) {
            const float f = 1.5f + static_cast<float>(i);
            std::memcpy(&words[i], &f, sizeof(f));
        } else {
            words[i] = (i * 0x01010101u) ^ ((i & 1u) ? 0x80000000u : 0u);
        }
    }
    BackendBufferDesc bd{};
    bd.byte_width = 96;
    bd.shader_resource = true;
    auto source = device.create_buffer(bd, words.data());
    ASSERT_TRUE(source);
    // Views {FirstElement, NumElements}: FLOAT {1, 4} (a 12-byte offset),
    // UINT {0, 5}, SINT {2, 3} -- three R32 texels per element.
    auto view_f = device.create_texture_buffer(*source, PixelFormat::R32Float,
                                               3, 12, false);
    auto view_u = device.create_texture_buffer(*source, PixelFormat::R32Uint,
                                               0, 15, false);
    auto view_s = device.create_texture_buffer(*source, PixelFormat::R32Sint,
                                               6, 9, false);
    ASSERT_TRUE(view_f && view_u && view_s);
    constexpr u32 kThreads = 6;
    BackendBufferDesc od{};
    od.byte_width = kThreads * 64;
    od.unordered_access = true;
    od.shader_resource = true;
    const std::vector<u8> fill(od.byte_width, 0xEEu);
    auto out = device.create_buffer(od, fill.data());
    ASSERT_TRUE(out);

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = kThreads;
    call.group_count_y = call.group_count_z = 1;
    call.texture_buffers[0] = view_f.get();
    call.texture_buffers[1] = view_u.get();
    call.texture_buffers[2] = view_s.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE] = out.get();
    BiasBlock push;
    push.srv(0, *view_f, false);  // R32G32B32: no alpha
    push.srv(1, *view_u, false);
    push.srv(2, *view_s, false);
    call.constants = push.words.data();
    call.constants_size = push.words.size() * 4;
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    std::vector<u32> got(kThreads * 16u);
    ASSERT_TRUE(out->read(0, got.data(), od.byte_width));

    // tools/oracle_probe/d3d11_a03_rgb32_buffer_probe.expected, threads 0-5.
    static const u32 expected[kThreads][16] = {
        {0x40900000, 0x40b00000, 0x40d00000, 0x3f800000, 0x3fc00000,
         0x40200000, 0x40600000, 1, 0x40f00000, 0x41080000, 0x41180000, 1,
         4, 5, 3, 0xc0de},
        {0x40f00000, 0x41080000, 0x41180000, 0x3f800000, 0x40900000,
         0x40b00000, 0x40d00000, 1, 0x41280000, 0x41380000, 0x41480000, 1,
         4, 5, 3, 0xc0de},
        {0x41280000, 0x41380000, 0x41480000, 0x3f800000, 0x40f00000,
         0x41080000, 0x41180000, 1, 0x41580000, 0x41680000, 0x41780000, 1,
         4, 5, 3, 0xc0de},
        {0x41580000, 0x41680000, 0x41780000, 0x3f800000, 0x41280000,
         0x41380000, 0x41480000, 1, 0, 0, 0, 1, 4, 5, 3, 0xc0de},
        {0, 0, 0, 0x3f800000, 0x41580000, 0x41680000, 0x41780000, 1, 0, 0, 0,
         1, 4, 5, 3, 0xc0de},
        {0, 0, 0, 0x3f800000, 0, 0, 0, 1, 0, 0, 0, 1, 4, 5, 3, 0xc0de},
    };
    for (u32 t = 0; t < kThreads; ++t) {
        for (u32 w = 0; w < 16; ++w) {
            EXPECT_EQ(got[t * 16 + w], expected[t][w])
                << "thread " << t << " word " << w;
        }
    }
}

// Area 03 W10: D3D11_TEXCUBE_ARRAY_SRV.First2DArrayFace is "the first 2D
// texture to use" -- any face, not only one that starts a cube (Windows
// accepts {First2DArrayFace 1, NumCubes 1}, d3d11_a03_views_probe). Metal's
// cube-array view takes a face-granular slice range whose length is a
// multiple of six, so the view over faces 1..6 of a two-cube array reads
// face f of its cube from slice 1 + f.
TEST(ShaderViewWindow, CubeArrayViewStartsAtAnyFace) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal";
    BackendTexture2DDesc td{};
    td.width = td.height = 1;
    td.array_size = 12;
    td.cube = true;
    td.pixel_format = PixelFormat::R32Uint;
    td.shader_resource = true;
    auto cubes = device.create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(cubes && cubes->valid());
    for (u32 slice = 0; slice < 12; ++slice) {
        const u32 value = 100u + slice;
        ASSERT_TRUE(cubes->update_slice(0, slice, &value, 4)) << slice;
    }
    BackendComputePipelineDesc pd{};
    pd.compute_entry_point = "main0";
    pd.compute_shader_msl =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void main0(texturecube_array<uint> cubes [[texture(0)]],\n"
        "    device uint* out [[buffer(" +
        std::to_string(BACKEND_UAV_BUFFER_BASE) +
        ")]], uint3 id [[thread_position_in_grid]]) {\n"
        "  out[id.x] = cubes.read(uint2(0, 0), id.x, 0).x;\n}\n";
    pd.cache_key = 0x7B0FF5E4u;
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid());
    BackendBufferDesc od{};
    od.byte_width = 6 * 4;
    od.unordered_access = true;
    od.shader_resource = true;
    const std::vector<u32> fill(6, 0xEEEEEEEEu);
    auto out = device.create_buffer(od, fill.data());
    ASSERT_TRUE(out);
    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = 6;
    call.group_count_y = call.group_count_z = 1;
    call.textures[0] = cubes.get();
    call.texture_views[0].dimension = BackendTextureViewDimension::TextureCubeArray;
    call.texture_views[0].mip_levels = 1;
    call.texture_views[0].first_array_slice = 1;
    call.texture_views[0].array_size = 6;
    call.buffers[BACKEND_UAV_BUFFER_BASE] = out.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    std::vector<u32> got(6);
    ASSERT_TRUE(out->read(0, got.data(), 6 * 4));
    for (u32 face = 0; face < 6; ++face) {
        EXPECT_EQ(got[face], 101u + face) << "face " << face;
    }
}

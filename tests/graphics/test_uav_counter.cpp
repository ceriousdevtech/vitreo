// SPDX-License-Identifier: LGPL-2.1-only
//
// The hidden UAV counter, moved by real fxc output on real Metal.
//
// Learn (imm_atomic_alloc / imm_atomic_consume, sm5 asm): alloc increments the
// counter and returns the ORIGINAL value; consume decrements it and returns
// the NEW value. Append/IncrementCounter compile to the first, Consume/
// DecrementCounter to the second. Each test starts the counter somewhere other
// than zero and sizes its buffer one element past the range the counter should
// reach, so an off-by-one in either direction writes or reads a sentinel.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "uav_counter_shaders.h"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kSentinel = 0xEEEEEEEEu;
constexpr u32 kThreads = 64;

class UavCounterTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!MetalDevice::instance().available()) {
            GTEST_SKIP() << "no Metal device";
        }
    }

    static GraphicsBackendDevice& device() { return MetalDevice::instance(); }

    static std::unique_ptr<BackendBuffer> buffer(const std::vector<u32>& words) {
        BackendBufferDesc desc{};
        desc.byte_width = words.size() * sizeof(u32);
        desc.shader_resource = true;
        desc.unordered_access = true;
        return device().create_buffer(desc, words.data());
    }

    /// Translate @p bytecode and build its kernel; @p registers receives the
    /// counter binding order the translator chose.
    static std::unique_ptr<BackendComputePipeline> kernel(
        const unsigned char* bytecode, usize size, u32 key,
        std::vector<u32>& registers) {
        const auto container = parse_dxbc(bytecode, size);
        EXPECT_TRUE(container.valid());
        const auto spirv = translate_dxbc_to_spirv(container);
        EXPECT_TRUE(spirv.valid()) << spirv.failure;
        if (!spirv.valid()) return nullptr;
        registers = spirv.uav_counter_registers;
        MslCompileOptions flat{};
        flat.argument_buffers = false;
        const auto msl = compile_spirv_to_msl(
            spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Compute, flat);
        EXPECT_TRUE(msl.valid());
        if (!msl.valid()) return nullptr;
        BackendComputePipelineDesc pd{};
        pd.compute_shader_msl = msl.msl_text();
        pd.compute_entry_point = "main0";
        pd.cache_key = key;
        auto pipeline = device().create_compute_pipeline(pd);
        EXPECT_TRUE(pipeline && pipeline->valid()) << msl.msl_text();
        return pipeline;
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
        pd.cache_key = 0xC0DE7E4Du;
        auto pipeline = device().create_compute_pipeline(pd);
        BackendTexture2DDesc td{};
        td.width = count;
        td.height = 1;
        td.mip_levels = td.array_size = 1;
        td.pixel_format = PixelFormat::R32Uint;
        td.unordered_access = true;
        auto texture = device().create_texture_2d(td, nullptr, 0);
        if (!pipeline || !pipeline->valid() || !texture) return {};
        BackendDispatchCall call{};
        call.pipeline = pipeline.get();
        call.group_count_x = count;
        call.group_count_y = call.group_count_z = 1;
        call.buffers[BACKEND_SRV_BUFFER_BASE] = source;
        call.textures[BACKEND_UAV_TEXTURE_BASE] = texture.get();
        if (!device().dispatch(call)) return {};
        std::vector<u32> out(count, 0);
        if (!texture->readback(0, out.data(), count * sizeof(u32))) return {};
        return out;
    }

    static void dispatch(BackendComputePipeline* pipeline,
                         BackendDispatchCall& call) {
        call.pipeline = pipeline;
        call.group_count_x = call.group_count_y = call.group_count_z = 1;
        call.threads_per_group_x = kThreads;
        call.threads_per_group_y = call.threads_per_group_z = 1;
        ASSERT_TRUE(device().dispatch(call));
    }
};

TEST_F(UavCounterTest, AppendWritesAtEachPreIncrementIndex) {
    std::vector<u32> registers;
    auto pipeline = kernel(uav_counter_append_main,
                           sizeof(uav_counter_append_main), 0xC0DE0001u,
                           registers);
    ASSERT_TRUE(pipeline);
    ASSERT_EQ(registers, (std::vector<u32>{1}));

    // Items are {value, tag}; the counter starts at 5, so 64 appends land in
    // items 5..68 and item 69 stays untouched.
    constexpr u32 kFirst = 5, kItems = kFirst + kThreads + 1;
    auto sink = buffer(std::vector<u32>(kItems * 2, kSentinel));
    auto counter = buffer({kFirst});
    ASSERT_TRUE(sink && counter);
    BackendDispatchCall call{};
    call.buffers[BACKEND_UAV_BUFFER_BASE + 1] = sink.get();
    call.buffers[BACKEND_UAV_COUNTER_BUFFER_BASE + 0] = counter.get();
    dispatch(pipeline.get(), call);

    const auto count = read_words(counter.get(), 1);
    ASSERT_EQ(count.size(), 1u);
    EXPECT_EQ(count[0], kFirst + kThreads);
    const auto words = read_words(sink.get(), kItems * 2);
    ASSERT_EQ(words.size(), kItems * 2);
    std::vector<u32> values;
    for (u32 item = 0; item < kItems; ++item) {
        const bool written = item >= kFirst && item < kFirst + kThreads;
        if (!written) {
            EXPECT_EQ(words[item * 2], kSentinel) << "item " << item;
            continue;
        }
        EXPECT_EQ(words[item * 2 + 1], 7u) << "item " << item;
        values.push_back(words[item * 2]);
    }
    std::sort(values.begin(), values.end());
    for (u32 i = 0; i < values.size(); ++i) {
        EXPECT_EQ(values[i], i * 3 + 1) << "every thread's item, once";
    }
}

TEST_F(UavCounterTest, ConsumeReadsAtEachPostDecrementIndex) {
    std::vector<u32> registers;
    auto pipeline = kernel(uav_counter_consume_main,
                           sizeof(uav_counter_consume_main), 0xC0DE0002u,
                           registers);
    ASSERT_TRUE(pipeline);
    ASSERT_EQ(registers, (std::vector<u32>{2}));

    // 64 items, then a poison item 64: a consume that returned the ORIGINAL
    // count would read it and never read item 0.
    std::vector<u32> items;
    for (u32 item = 0; item < kThreads; ++item) {
        items.push_back(100 + item);
        items.push_back(0);
    }
    items.push_back(0xDEADu);
    items.push_back(0);
    auto source = buffer(items);
    auto counter = buffer({kThreads});
    auto consumed = buffer(std::vector<u32>(kThreads, kSentinel));
    ASSERT_TRUE(source && counter && consumed);
    BackendDispatchCall call{};
    call.buffers[BACKEND_UAV_BUFFER_BASE + 0] = consumed.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE + 2] = source.get();
    call.buffers[BACKEND_UAV_COUNTER_BUFFER_BASE + 0] = counter.get();
    dispatch(pipeline.get(), call);

    const auto count = read_words(counter.get(), 1);
    ASSERT_EQ(count.size(), 1u);
    EXPECT_EQ(count[0], 0u);
    auto values = read_words(consumed.get(), kThreads);
    ASSERT_EQ(values.size(), kThreads);
    std::sort(values.begin(), values.end());
    for (u32 i = 0; i < kThreads; ++i) {
        EXPECT_EQ(values[i], 100 + i) << "each item consumed exactly once";
    }
}

TEST_F(UavCounterTest, CountersBindInFirstUseOrderNotByRegister) {
    std::vector<u32> registers;
    auto pipeline = kernel(uav_counter_count_main,
                           sizeof(uav_counter_count_main), 0xC0DE0003u,
                           registers);
    ASSERT_TRUE(pipeline);
    // IncrementCounter on u3 comes first, so u3 is binding 0 and u4 binding 1.
    ASSERT_EQ(registers, (std::vector<u32>{3, 4}));

    // up counts from 2 to 66 writing 1000+id at 2..65; down counts from 66 to
    // 2 writing 2000+id at 65..2. Different starting points and payloads, so a
    // swapped binding shows up in both buffers.
    constexpr u32 kWords = kThreads + 4;
    auto up = buffer(std::vector<u32>(kWords, kSentinel));
    auto down = buffer(std::vector<u32>(kWords, kSentinel));
    auto up_counter = buffer({2});
    auto down_counter = buffer({kThreads + 2});
    ASSERT_TRUE(up && down && up_counter && down_counter);
    BackendDispatchCall call{};
    call.buffers[BACKEND_UAV_BUFFER_BASE + 3] = up.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE + 4] = down.get();
    call.buffers[BACKEND_UAV_COUNTER_BUFFER_BASE + 0] = up_counter.get();
    call.buffers[BACKEND_UAV_COUNTER_BUFFER_BASE + 1] = down_counter.get();
    dispatch(pipeline.get(), call);

    EXPECT_EQ(read_words(up_counter.get(), 1), (std::vector<u32>{kThreads + 2}));
    EXPECT_EQ(read_words(down_counter.get(), 1), (std::vector<u32>{2}));
    for (const auto& [target, base] :
         {std::pair{up.get(), 1000u}, std::pair{down.get(), 2000u}}) {
        const auto words = read_words(target, kWords);
        ASSERT_EQ(words.size(), kWords);
        EXPECT_EQ(words[0], kSentinel);
        EXPECT_EQ(words[1], kSentinel);
        EXPECT_EQ(words[kThreads + 2], kSentinel);
        std::vector<u32> written(words.begin() + 2,
                                 words.begin() + 2 + kThreads);
        std::sort(written.begin(), written.end());
        for (u32 i = 0; i < kThreads; ++i) {
            EXPECT_EQ(written[i], base + i) << "base " << base;
        }
    }
}

TEST_F(UavCounterTest, PixelShaderAppendsOncePerFragment) {
    const auto container =
        parse_dxbc(uav_counter_ps_main, sizeof(uav_counter_ps_main));
    ASSERT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    ASSERT_EQ(spirv.uav_counter_registers, (std::vector<u32>{1}));
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto fragment = compile_spirv_to_msl(
        spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Fragment, flat);
    ASSERT_TRUE(fragment.valid());

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
    pdesc.fragment_shader_msl = fragment.msl_text();
    pdesc.vertex_entry_point = "main0";
    pdesc.fragment_entry_point = "main0";
    pdesc.vertex_attributes = {{0, 0, BackendVertexFormat::Float2, 0}};
    pdesc.vertex_stride = pdesc.vertex_buffer_strides[0] = 8;
    pdesc.color_format = PixelFormat::BGRA8Unorm;
    pdesc.cache_key = 0xC0DE0004u;
    auto pipeline = device().create_pipeline(pdesc);
    ASSERT_TRUE(pipeline && pipeline->valid()) << fragment.msl_text();

    const float positions[]{-1, -1, 3, -1, -1, 3};
    BackendBufferDesc vb{};
    vb.byte_width = sizeof(positions);
    vb.vertex_buffer = true;
    auto vertices = device().create_buffer(vb, positions);
    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::BGRA8Unorm;
    td.render_target = true;
    auto target = device().create_texture_2d(td, nullptr, 0);
    ASSERT_TRUE(vertices && target);

    // 16 fragments append after the 3 the counter starts at; word 19 stays.
    constexpr u32 kStart = 3, kFragments = 16, kWords = kStart + kFragments + 1;
    auto sink = buffer(std::vector<u32>(kWords, kSentinel));
    auto counter = buffer({kStart});
    ASSERT_TRUE(sink && counter);

    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target.get();
    pass_desc.color_formats[0] = PixelFormat::BGRA8Unorm;
    auto pass = device().create_render_pass_standalone(pass_desc);
    ASSERT_NE(pass, nullptr);
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.vertex_buffer = vertices.get();
    call.vertex_buffers[0] = vertices.get();
    call.viewport = {0, 0, 4, 4, 0, 1};
    call.fragment_uav_buffers[1] = sink.get();
    call.fragment_uav_counters[0] = counter.get();
    ASSERT_TRUE(pass->draw(call));
    ASSERT_TRUE(pass->end());

    EXPECT_EQ(read_words(counter.get(), 1),
              (std::vector<u32>{kStart + kFragments}));
    const auto words = read_words(sink.get(), kWords);
    ASSERT_EQ(words.size(), kWords);
    for (u32 i = 0; i < kStart; ++i) EXPECT_EQ(words[i], kSentinel);
    EXPECT_EQ(words[kWords - 1], kSentinel);
    std::vector<u32> pixels(words.begin() + kStart,
                            words.begin() + kStart + kFragments);
    std::sort(pixels.begin(), pixels.end());
    for (u32 i = 0; i < kFragments; ++i) {
        EXPECT_EQ(pixels[i], i) << "each pixel appended exactly once";
    }
}

}  // namespace

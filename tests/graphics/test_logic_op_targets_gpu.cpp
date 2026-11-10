// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_logic_op_targets_gpu.cpp
/// @brief Per-target output-merger logic ops (D3D11.3 17.7,
///        D3D12_RENDER_TARGET_BLEND_DESC::LogicOp).
///
/// g_ps_logic2 writes cb0[0] to o0 and cb0[0].yxwz to o1 (operand token
/// 0x00208B16, swizzle 0xB1). Expected texels are that source, the
/// destination, and the 17.7 truth table, truncated to 8 bits. They are
/// not taken from a Vitreo run.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

namespace fl111 {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../oracle_data/d3d11_fl111_shaders.h"
#pragma clang diagnostic pop
}  // namespace fl111

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

std::string translate(const unsigned char* bytecode, usize size,
                      ShaderStage stage,
                      const DxbcTranslationOptions& options = {},
                      bool framebuffer_fetch = false) {
    const auto container = parse_dxbc(bytecode, size);
    EXPECT_TRUE(container.valid());
    const auto spirv = translate_dxbc_to_spirv(container, options);
    EXPECT_TRUE(spirv.valid()) << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    flat.framebuffer_fetch = framebuffer_fetch;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(), stage, flat);
    EXPECT_TRUE(msl.valid());
    return msl.valid() ? std::string(msl.msl_text()) : std::string{};
}

std::unique_ptr<BackendBuffer> buffer(usize bytes, const void* data, bool cb) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.shader_resource = !cb;
    desc.constant_buffer = cb;
    return MetalDevice::instance().create_buffer(desc, data);
}

struct LogicDraw {
    bool per_target{false};
    u32 shared_op{0};
    std::array<u32, 2> ops{};
    u32 targets{0x3u};
    std::array<u32, 2> write_masks{0xFu, 0xFu};
    bool independent_masks{false};
};

/// Draws g_vs_full + g_ps_logic2 into two RGBA8Uint targets and returns
/// texel 0 of each.
std::array<std::array<u8, 4>, 2> draw_two(const LogicDraw& draw,
                                          const std::array<u8, 4>& dst0,
                                          const std::array<u8, 4>& dst1,
                                          const std::array<u32, 4>& src) {
    auto& device = MetalDevice::instance();
    DxbcTranslationOptions ps_options{};
    ps_options.logic_op = draw.shared_op;
    ps_options.logic_op_per_target = draw.per_target;
    ps_options.logic_ops[0] = draw.ops[0];
    ps_options.logic_ops[1] = draw.ops[1];
    ps_options.logic_op_targets = draw.targets;
    ps_options.logic_op_component_masks[0] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
    ps_options.logic_op_component_masks[1] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
    BackendPipelineDesc pd{};
    pd.vertex_shader_msl = translate(fl111::g_vs_full, sizeof(fl111::g_vs_full),
                                     ShaderStage::Vertex);
    pd.fragment_shader_msl =
        translate(fl111::g_ps_logic2, sizeof(fl111::g_ps_logic2),
                  ShaderStage::Fragment, ps_options, /*framebuffer_fetch=*/true);
    if (pd.fragment_shader_msl.empty()) return {};
    if ((draw.targets & 1u) != 0) {
        EXPECT_NE(pd.fragment_shader_msl.find("[[color(0)]]"), std::string::npos)
            << pd.fragment_shader_msl;
    }
    if ((draw.targets & 2u) != 0) {
        EXPECT_NE(pd.fragment_shader_msl.find("[[color(1)]]"), std::string::npos)
            << pd.fragment_shader_msl;
    }
    pd.vertex_entry_point = pd.fragment_entry_point = "main0";
    pd.color_format = PixelFormat::RGBA8Uint;
    pd.color_formats[0] = PixelFormat::RGBA8Uint;
    pd.color_formats[1] = PixelFormat::RGBA8Uint;
    pd.independent_blend_enabled = draw.independent_masks;
    pd.color_write_masks[0] = draw.write_masks[0];
    pd.color_write_masks[1] = draw.write_masks[1];
    pd.input_primitive_class = BackendPrimitiveClass::Triangle;
    pd.cache_key = 0xF1113000ull ^
                   (static_cast<u64>(draw.shared_op) << 8) ^
                   (static_cast<u64>(draw.ops[0]) << 16) ^
                   (static_cast<u64>(draw.ops[1]) << 24) ^
                   (static_cast<u64>(draw.targets) << 32) ^
                   (static_cast<u64>(draw.write_masks[0]) << 40) ^
                   (static_cast<u64>(draw.write_masks[1]) << 44) ^
                   (draw.per_target ? (1ull << 48) : 0ull) ^
                   (draw.independent_masks ? (1ull << 49) : 0ull);
    auto pipeline = device.create_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << pd.fragment_shader_msl;
    if (!pipeline || !pipeline->valid()) return {};

    const auto fill = [](const std::array<u8, 4>& dst) {
        std::array<u8, 16> texels{};
        for (u32 x = 0; x < 4; ++x) {
            std::memcpy(&texels[x * 4], dst.data(), 4);
        }
        return texels;
    };
    const auto texels0 = fill(dst0);
    const auto texels1 = fill(dst1);
    BackendTexture2DDesc td{};
    td.width = 4;
    td.height = 1;
    td.mip_levels = td.array_size = 1;
    td.pixel_format = PixelFormat::RGBA8Uint;
    td.render_target = true;
    td.shader_resource = true;
    auto target0 = device.create_texture_2d(td, texels0.data(), 16);
    auto target1 = device.create_texture_2d(td, texels1.data(), 16);
    auto cb = buffer(16, src.data(), true);
    EXPECT_TRUE(target0 && target1 && cb);
    if (!target0 || !target1 || !cb) return {};

    BackendRenderPassDesc pass_desc{};
    pass_desc.color_targets[0] = target0.get();
    pass_desc.color_targets[1] = target1.get();
    pass_desc.color_formats[0] = PixelFormat::RGBA8Uint;
    pass_desc.color_formats[1] = PixelFormat::RGBA8Uint;
    pass_desc.preserve_color_contents = true;
    auto pass = device.create_render_pass_standalone(pass_desc);
    EXPECT_TRUE(pass && pass->valid());
    if (!pass || !pass->valid()) return {};

    const std::array<u32, kPushBlockBytes / 4> push{};
    BackendDrawCall call{};
    call.pipeline = pipeline.get();
    call.topology = BackendPrimitiveTopology::TriangleList;
    call.vertex_count = 3;
    call.instance_count = 1;
    call.viewport = {0, 0, 4, 1, 0, 1};
    call.fragment_constant_buffers[0] = cb.get();
    call.fragment_constants = push.data();
    call.fragment_constants_size = sizeof(push);
    call.vertex_constants = push.data();
    call.vertex_constants_size = sizeof(push);
    EXPECT_TRUE(pass->draw(call) && pass->end());

    std::array<std::array<u8, 4>, 2> out{};
    const auto read = [&](BackendTexture2D& target, std::array<u8, 4>& pixel) {
        std::array<u8, 16> raw{};
        EXPECT_TRUE(target.readback_slice(0, 0, raw.data(), 16));
        for (u32 x = 1; x < 4; ++x) {
            EXPECT_TRUE(std::equal(raw.begin(), raw.begin() + 4, &raw[x * 4]))
                << "texel " << x;
        }
        std::memcpy(pixel.data(), raw.data(), 4);
    };
    read(*target0, out[0]);
    read(*target1, out[1]);
    return out;
}

}  // namespace

TEST(LogicOpTargetsGpu, DifferentOpsAndASharedOpMatchTheTruthTable) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_framebuffer_fetch()) {
        GTEST_SKIP() << "no framebuffer fetch: OutputMergerLogicOp is FALSE";
    }
    // 17.7: s = pixel-shader output, d = target contents. RGBA8 keeps the
    // low 8 bits (the spec's ideal truncate; 0x100 becomes 0x00).
    // o0 = src. g_ps_logic2's o1 = src.yxwz.
    // src = (0xAA, 0xAA, 0xF0, 0x00), d = (0xCC, 0x55, 0x0F, 0xCC).
    // o0 XOR: AA^CC, AA^55, F0^0F, 00^CC = 66 FF FF CC
    // o1 = (0xAA, 0xAA, 0x00, 0xF0)
    // o1 XOR: AA^CC, AA^55, 00^0F, F0^CC = 66 FF 0F 3C
    // o1 AND: AA&CC, AA&55, 00&0F, F0&CC = 88 00 00 C0
    const std::array<u8, 4> dst{0xCC, 0x55, 0x0F, 0xCC};
    const std::array<u32, 4> src{0xAAu, 0xAAu, 0xF0u, 0x00u};

    LogicDraw different{};
    different.per_target = true;
    different.ops = {10u, 6u};  // XOR, AND
    EXPECT_EQ(draw_two(different, dst, dst, src),
              (std::array<std::array<u8, 4>, 2>{
                  std::array<u8, 4>{0x66, 0xFF, 0xFF, 0xCC},
                  std::array<u8, 4>{0x88, 0x00, 0x00, 0xC0}}));

    LogicDraw shared{};
    shared.shared_op = 10u;  // XOR on every enabled target
    EXPECT_EQ(draw_two(shared, dst, dst, src),
              (std::array<std::array<u8, 4>, 2>{
                  std::array<u8, 4>{0x66, 0xFF, 0xFF, 0xCC},
                  std::array<u8, 4>{0x66, 0xFF, 0x0F, 0x3C}}));

    // The same XOR stored per target still applies to that target only.
    LogicDraw both_xor{};
    both_xor.per_target = true;
    both_xor.ops = {10u, 10u};
    EXPECT_EQ(draw_two(both_xor, dst, dst, src),
              (std::array<std::array<u8, 4>, 2>{
                  std::array<u8, 4>{0x66, 0xFF, 0xFF, 0xCC},
                  std::array<u8, 4>{0x66, 0xFF, 0x0F, 0x3C}}));

    // Target 1 has no logic op, so it keeps the shader output.
    LogicDraw only0{};
    only0.shared_op = 10u;
    only0.targets = 1u;
    EXPECT_EQ(draw_two(only0, dst, dst, src),
              (std::array<std::array<u8, 4>, 2>{
                  std::array<u8, 4>{0x66, 0xFF, 0xFF, 0xCC},
                  std::array<u8, 4>{0xAA, 0xAA, 0x00, 0xF0}}));
}

TEST(LogicOpTargetsGpu, TruncationAndWriteMaskStayPerTarget) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_framebuffer_fetch()) {
        GTEST_SKIP() << "no framebuffer fetch: OutputMergerLogicOp is FALSE";
    }
    // src = (0x100, 0x1AA, 0xFFFFFFFF, 0x7F). 17.7: 0x100 into an 8-bit
    // component truncates to 0x00 (a clamp would be 0xFF).
    // o0 COPY -> 00 AA FF 7F
    // o1 = (0x1AA, 0x100, 0x7F, 0xFFFFFFFF) ->  AA 00 7F FF
    // o1 AND d (CC 55 0F CC) -> 88 00 0F CC
    const std::array<u8, 4> dst{0xCC, 0x55, 0x0F, 0xCC};
    const std::array<u32, 4> src{0x100u, 0x1AAu, 0xFFFFFFFFu, 0x7Fu};
    LogicDraw truncated{};
    truncated.per_target = true;
    truncated.ops = {2u, 6u};  // COPY, AND
    EXPECT_EQ(draw_two(truncated, dst, dst, src),
              (std::array<std::array<u8, 4>, 2>{
                  std::array<u8, 4>{0x00, 0xAA, 0xFF, 0x7F},
                  std::array<u8, 4>{0x88, 0x00, 0x0F, 0xCC}}));

    // 17.15: the write mask limits the write after the op. Target 0's XOR
    // is 66 FF FF CC; red-only keeps GBA at the destination. Target 1's
    // AND is still the full 88 00 00 C0.
    const std::array<u32, 4> narrow{0xAAu, 0xAAu, 0xF0u, 0x00u};
    LogicDraw masked{};
    masked.per_target = true;
    masked.ops = {10u, 6u};
    masked.independent_masks = true;
    masked.write_masks = {0x1u, 0xFu};
    EXPECT_EQ(draw_two(masked, dst, dst, narrow),
              (std::array<std::array<u8, 4>, 2>{
                  std::array<u8, 4>{0x66, 0x55, 0x0F, 0xCC},
                  std::array<u8, 4>{0x88, 0x00, 0x00, 0xC0}}));
}

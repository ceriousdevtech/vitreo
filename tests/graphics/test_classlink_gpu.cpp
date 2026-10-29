// SPDX-License-Identifier: LGPL-2.1-only

/// @file test_classlink_gpu.cpp
/// @brief Interface calls (D3D11.3 7.19: fcall, this[], uniformly indexed
///        cb#/t#/s#) run on real Metal, against Windows (WARP).
///
/// The shaders are tools/oracle_probe/d3d11_classlink_shaders.h (fxc output
/// of d3d11_classlink.hlsl); every expected line is copied from
/// tools/oracle_probe/d3d11_classlink_probe.expected. Each program is
/// specialised for the probe's instance set: the function table and this[]
/// data of every interface slot, read off fxc's /Fc listing ("Interface
/// slots", "Available Class Instances") for the instance the probe binds
/// there -- a GetClassInstance'd instance's cb/offset/texture/sampler, a
/// CreateClassInstance'd one's arguments (all 0 here).

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace classlink {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../oracle_data/d3d11_classlink_shaders.h"
#pragma clang diagnostic pop
}  // namespace classlink

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

const std::map<std::string, std::vector<std::string>> kExpected = {
    {"multi_a", {
        "multi_a fnv=053f731e removed=0x00000000",
        "multi_a 000: daa66d0b 4d481f35 dca86d2d aa66d0b0 e6ea9f5d 3ef3f1a1 191360bf 6ea9f5d0",
        "multi_a 008: 2b7b932f 26b5be25 538453f1 b7b932f0 6feab71d 187b8b91 8ff54743 feab71d0",
        "multi_a 016: 2b59b50b 0a315655 cc623af5 b59b50b0 e6aead95 f5e32401 06cf2e47 6aead950",
        "multi_a 024: 3abf9d67 e7a4f175 434021a9 abf9d670 7dae9355 e36ac171 81b114cb dae93550",
        "multi_a 032: 3dd9f70b d5209125 be1e08ad dd9f70b0 e56af35d c6d25d31 fa8afbbf 56af35d0",
        "multi_a 040: 26fbed2f be942415 38fbee71 6fbed2f0 6b6a9d1d a659f5a1 736ce2c3 b6a9d1d0",
        "multi_a 048: 2fd9950b 980fc045 afd9d575 fd9950b0 eba6b605 89c18a91 ec46c747 ba6b6050",
        "multi_a 056: 26a7b397 75835f05 26b7bc49 6a7b3970 5aa6afc5 67492fe1 6328b02b aa6afc50",
    }},
    {"multi_b", {
        "multi_b fnv=401fea2e removed=0x00000000",
        "multi_b 000: 00000016 dca86d1c dca86d1c 00000160 000000eb 191360ae 191360ae 00000eb0",
        "multi_b 008: 0000007c 538453e0 538453e0 000007c0 00000031 8ff54732 8ff54732 00000310",
        "multi_b 016: 000000f6 cc623ae4 cc623ae4 00000f60 000000cb 06cf2e36 06cf2e36 00000cb0",
        "multi_b 024: 0000009c 43402198 43402198 000009c0 000000d1 81b114ba 81b114ba 00000d10",
        "multi_b 032: 000000d6 be1e089c be1e089c 00000d60 0000002b fa8afbae fa8afbae 000002b0",
        "multi_b 040: 000000bc 38fbee60 38fbee60 00000bc0 000000f1 736ce2b2 736ce2b2 00000f10",
        "multi_b 048: 00000036 afd9d564 afd9d564 00000360 0000009b ec46c736 ec46c736 000009b0",
        "multi_b 056: 0000006c 26b7bc38 26b7bc38 000006c0 00000101 6328b01a 6328b01a 00001010",
    }},
    {"dyn_1_sel0", {
        "dyn_1_sel0 fnv=a8ccca5a removed=0x00000000",
        "dyn_1_sel0 000: dca86d1c 191360ae 538453e0 8ff54732 cc623ae4 06cf2e36 43402198 81b114ba",
        "dyn_1_sel0 008: be1e089c fa8afbae 38fbee60 736ce2b2 afd9d564 ec46c736 26b7bc38 6328b01a",
    }},
    {"dyn_1_sel1", {
        "dyn_1_sel1 fnv=63ed54e9 removed=0x00000000",
        "dyn_1_sel1 000: 4d481f35 3ef3f1a1 26b5be25 187b8b91 0a315655 f5e32401 e7a4f175 e36ac171",
        "dyn_1_sel1 008: d5209125 c6d25d31 be942415 a659f5a1 980fc045 89c18a91 75835f05 67492fe1",
    }},
    {"dyn_1_sel2", {
        "dyn_1_sel2 fnv=d5ab597e removed=0x00000000",
        "dyn_1_sel2 000: dca86d2d 191360bf 538453f1 8ff54743 cc623af5 06cf2e47 434021a9 81b114cb",
        "dyn_1_sel2 008: be1e08ad fa8afbbf 38fbee71 736ce2c3 afd9d575 ec46c747 26b7bc49 6328b02b",
    }},
    {"dyn_1_sel3", {
        "dyn_1_sel3 fnv=05d329c5 removed=0x00000000",
        "dyn_1_sel3 000: 00001028 0000103c 00001050 0000103c 00001028 0000103c 00001050 0000103c",
        "dyn_1_sel3 008: 00001028 0000103c 00001050 0000103c 00001028 0000103c 00001050 0000103c",
    }},
    {"nested_inc_shl_sel2", {
        "nested_inc_shl_sel2 fnv=645d0a89 removed=0x00000000",
        "nested_inc_shl_sel2 000: 030303c6 10101010 dda36aeb f3678b80 b84fd81c d6ff2730 92f644e1 ba76c0a0",
        "nested_inc_shl_sel2 008: 6d96b872 9dce7e50 423d24d7 614615c0 1ce99208 44ddb170 f78ff98d 28552ee0",
        "nested_inc_shl_sel2 016: d230671e 0baccc90 acd6d413 ef246700 8d8345f4 f2bc1bb0 6829b409 d633bc20",
        "nested_inc_shl_sel2 024: 42ca204a b98b52d0 1d70867f 9d02c940 f21cf560 609a6df0 ccc36135 44120260",
    }},
    {"nested_mod_not_sel1", {
        "nested_mod_not_sel1 fnv=009d87da removed=0x00000000",
        "nested_mod_not_sel1 000: fdfdfd53 fefefefe c1930e25 60c98747 85201bb7 c2900d8c 48b12741 245893f5",
        "nested_mod_not_sel1 008: 0c46309b 8623181a d3d73ded e9eb9ea3 9764496f 4bb224e8 5af55a89 ad7aad11",
        "nested_mod_not_sel1 016: 1e8a66c3 0f453336 e21b73b5 710db98f a1a87c27 d0d43e44 653988d1 329cc43d",
        "nested_mod_not_sel1 024: 28ce950b 94674ad2 ec5fa67d f62fd36b b3ecb2bf 59f65920 777dbf19 bbbedfd9",
    }},
    {"nested_mod_shl_sel0", {
        "nested_mod_shl_sel0 fnv=89577a05 removed=0x00000000",
        "nested_mod_shl_sel0 000: 10101045 10101010 f3678bd5 f3678b80 d6ff2765 d6ff2730 ba76c0f5 ba76c0a0",
        "nested_mod_shl_sel0 008: 9dce7e05 9dce7e50 61461595 614615c0 44ddb125 44ddb170 28552eb5 28552ee0",
        "nested_mod_shl_sel0 016: 0bacccc5 0baccc90 ef246755 ef246700 f2bc1be5 f2bc1bb0 d633bc75 d633bc20",
        "nested_mod_shl_sel0 024: b98b5285 b98b52d0 9d02c915 9d02c940 609a6da5 609a6df0 44120235 44120260",
    }},
};

std::unique_ptr<BackendBuffer> buffer(usize bytes, const void* data,
                                      bool uav = false, bool cb = false) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.shader_resource = !cb;
    desc.unordered_access = uav;
    desc.constant_buffer = cb;
    return MetalDevice::instance().create_buffer(desc, data);
}

u32 fnv(const std::vector<u32>& words, usize count) {
    u32 hash = 2166136261u;
    for (usize i = 0; i < count * 4; ++i) {
        hash ^= reinterpret_cast<const u8*>(words.data())[i];
        hash *= 16777619u;
    }
    return hash;
}

/// Dispatches one group of 64 threads of @p blob specialised for
/// @p bindings with g_sel = @p sel, and prints the output as the probe does.
std::vector<std::string> run(const std::string& name, const unsigned char* blob,
                             usize size,
                             const std::vector<DxbcSlotBinding>& bindings,
                             u32 sel, u32 words_per_thread) {
    auto& device = MetalDevice::instance();
    const auto copy = dxbc_with_slot_bindings(blob, size, bindings);
    const auto spirv =
        translate_dxbc_to_spirv(parse_dxbc(copy.data(), copy.size()));
    EXPECT_TRUE(spirv.valid()) << name << ": " << spirv.failure;
    if (!spirv.valid()) return {};
    MslCompileOptions flat{};
    flat.argument_buffers = false;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                          ShaderStage::Compute, flat);
    EXPECT_TRUE(msl.valid()) << name;
    if (!msl.valid()) return {};
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = std::hash<std::string>{}(pd.compute_shader_msl);
    auto pipeline = device.create_compute_pipeline(pd);
    EXPECT_TRUE(pipeline && pipeline->valid()) << pd.compute_shader_msl;
    if (!pipeline || !pipeline->valid()) return {};

    // The probe's resources (d3d11_classlink_probe.c setup()).
    std::array<u32, 64> in{};
    for (u32 i = 0; i < 64; ++i) {
        in[i] = (i * 0x9E3779B1u) ^ (i << 3) ^ 0x01010101u;
    }
    const std::array<u32, 4> c0{sel, 0, 0, 0};
    const std::array<u32, 12> c1{0x100, 0, 0, 0, 0x200, 0, 0, 0, 0x300, 0, 0, 0};
    const std::array<u32, 24> c2{0x11, 0, 0, 0, 3, 5, 0, 0, 0x22, 0, 0, 0,
                                 4, 0, 0, 0, 0x1000, 0, 0, 0, 0, 0, 0, 0};
    const std::array<u32, 4> c3{0x40, 0, 0, 0};
    const std::array<u32, 8> tab{10, 20, 30, 40, 50, 60, 70, 80};
    const std::array<u32, 4> texels{11, 22, 33, 44};
    auto input = buffer(sizeof(in), in.data());
    const std::vector<u32> fill(256, 0xCDCDCDCDu);
    auto output = buffer(fill.size() * 4, fill.data(), true);
    auto cb0 = buffer(sizeof(c0), c0.data(), false, true);
    auto cb1 = buffer(sizeof(c1), c1.data(), false, true);
    auto cb2 = buffer(sizeof(c2), c2.data(), false, true);
    auto cb3 = buffer(sizeof(c3), c3.data(), false, true);
    auto tab_storage = buffer(sizeof(tab), tab.data());
    EXPECT_TRUE(input && output && cb0 && cb1 && cb2 && cb3 && tab_storage);
    if (!input || !output || !cb0 || !cb1 || !cb2 || !cb3 || !tab_storage) {
        return {};
    }
    auto tab_view = device.create_texture_buffer(*tab_storage,
                                                 PixelFormat::R32Uint, 0, 8);
    BackendTexture2DDesc td{};
    td.width = 4;
    td.height = 1;
    td.pixel_format = PixelFormat::RGBA8Unorm;
    td.shader_resource = true;
    auto texture = device.create_texture_2d(td, texels.data(), 16);
    EXPECT_TRUE(tab_view && texture);
    if (!tab_view || !texture) return {};

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 64;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.buffers[BACKEND_CONSTANT_BUFFER_BASE + 0] = cb0.get();
    call.buffers[BACKEND_CONSTANT_BUFFER_BASE + 1] = cb1.get();
    call.buffers[BACKEND_CONSTANT_BUFFER_BASE + 2] = cb2.get();
    call.buffers[BACKEND_CONSTANT_BUFFER_BASE + 3] = cb3.get();
    call.buffers[BACKEND_SRV_BUFFER_BASE] = input.get();
    call.buffers[BACKEND_UAV_BUFFER_BASE] = output.get();
    call.texture_buffers[1] = tab_view.get();
    call.textures[2] = texture.get();
    call.samplers[0].min_filter = call.samplers[0].mag_filter =
        call.samplers[0].mip_filter = BackendFilter::Nearest;
    call.samplers[0].address_u = call.samplers[0].address_v =
        call.samplers[0].address_w = BackendAddressMode::Clamp;
    call.sampler_bound[0] = true;
    EXPECT_TRUE(device.dispatch(call));
    EXPECT_TRUE(device.wait_idle());
    std::vector<u32> words(256, 0xDDDDDDDDu);
    EXPECT_TRUE(output->read(0, words.data(), words.size() * 4));

    std::vector<std::string> lines;
    char text[160];
    std::snprintf(text, sizeof text, "%s fnv=%08x removed=0x00000000",
                  name.c_str(), fnv(words, 64 * words_per_thread));
    lines.push_back(text);
    for (u32 i = 0; i < 16 * words_per_thread; i += 8) {
        std::string line = name;
        std::snprintf(text, sizeof text, " %03u:", i);
        line += text;
        for (u32 j = 0; j < 8; ++j) {
            std::snprintf(text, sizeof text, " %08x", words[i + j]);
            line += text;
        }
        lines.push_back(line);
    }
    return lines;
}

void expect_case(const std::string& name, const unsigned char* blob, usize size,
                 const std::vector<DxbcSlotBinding>& bindings, u32 sel,
                 u32 words_per_thread) {
    if (!MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
    const auto got = run(name, blob, size, bindings, sel, words_per_thread);
    EXPECT_EQ(got, kExpected.at(name));
}

// this[] of an instance the probe binds, per the listing: {CB, CB offset,
// texture, sampler}, "-" read as 0.
constexpr std::array<u32, 4> kNone{0, 0, 0, 0};
constexpr std::array<u32, 4> kAdd0{2, 0, 0, 0};
constexpr std::array<u32, 4> kMul{2, 1, 0, 0};
constexpr std::array<u32, 4> kAdd1{2, 2, 0, 0};
constexpr std::array<u32, 4> kShl{2, 3, 0, 0};
constexpr std::array<u32, 4> kBuf{2, 4, 1, 0};
constexpr std::array<u32, 4> kSamp{0, 0, 2, 0};
constexpr std::array<u32, 4> kInc{3, 0, 0, 0};

}  // namespace

// cs_multi slots: g_op 0 (tables 0-4 by type SampOp, BufOp, MulK, AddK,
// XorOp), g_ops[4] 1-4 (tables 5-9), g_mod 5 (NotMod 10, ShlMod 11).
TEST(ClassLinkageGpuTest, StaticSlotsCallTheBoundClasses) {
    using namespace classlink;
    expect_case("multi_a", k_cs_multi, sizeof(k_cs_multi),
                {{4, kNone}, {8, kAdd0}, {7, kMul}, {8, kAdd1}, {6, kBuf},
                 {11, kShl}}, 0, 4);
}

TEST(ClassLinkageGpuTest, ClassTexturesAndSamplersComeFromThis) {
    using namespace classlink;
    expect_case("multi_b", k_cs_multi, sizeof(k_cs_multi),
                {{0, kSamp}, {8, kAdd0}, {8, kAdd0}, {8, kAdd0}, {8, kAdd0},
                 {11, kShl}}, 0, 4);
}

// cs_dyn: g_ops[4] at slots 0-3, called through fp0[r0.w + 0].
TEST(ClassLinkageGpuTest, RegisterIndexedInterfaceArraysSwitchPerSlot) {
    using namespace classlink;
    const std::vector<DxbcSlotBinding> set = {
        {3, kAdd0}, {2, kMul}, {3, kAdd1}, {1, kBuf}};
    for (u32 sel = 0; sel < 4; ++sel) {
        expect_case("dyn_1_sel" + std::to_string(sel), k_cs_dyn,
                    sizeof(k_cs_dyn), set, sel, 1);
    }
}

// cs_nested: g_step slot 0 (ModStep 0, IncStep 1), g_inner slot 1 (NotMod 2,
// ShlMod 3); ModStep's method calls g_inner (a nested fcall), and g_step is
// called in a loop.
TEST(ClassLinkageGpuTest, NestedInterfaceCallsAndCallsInLoops) {
    using namespace classlink;
    expect_case("nested_inc_shl_sel2", k_cs_nested, sizeof(k_cs_nested),
                {{1, kInc}, {3, kShl}}, 2, 2);
    expect_case("nested_mod_not_sel1", k_cs_nested, sizeof(k_cs_nested),
                {{0, kNone}, {2, kNone}}, 1, 2);
    expect_case("nested_mod_shl_sel0", k_cs_nested, sizeof(k_cs_nested),
                {{0, kNone}, {3, kShl}}, 0, 2);
}

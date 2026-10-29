// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_to_spirv_oracle_blobs.cpp
/// @brief Tests driven by committed fxc blobs from tools/oracle_probe.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/contracts/backend_device.hpp"

#include "dxbc_to_spirv_test_shaders.h"
#include "dxbc_to_spirv_test_spirv.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <tuple>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;
using namespace vitreo::graphics::dxbc_spirv_test;

// ---------------------------------------------------------------------------
// Area 09b resource queries, from the fxc blobs the Windows probe runs
// (tools/oracle_probe/d3d11_a09b_shaders.h; the probe's .expected holds what
// WARP returns and d3d11_a09b_shader_probe.c the resources behind it).
#include "../oracle_data/d3d11_a09b_shaders.h"

namespace {

std::string translate_blob_to_msl(const unsigned char* blob, usize size,
                                  ShaderStage stage) {
    const auto container = parse_dxbc(blob, size);
    EXPECT_TRUE(container.valid());
    const auto result = translate_dxbc_to_spirv(container);
    EXPECT_TRUE(result.valid()) << result.failure;
    if (!result.valid()) return {};
    MslCompileOptions options{};
    options.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        result.spirv.data(), result.spirv.size(), stage, options);
    EXPECT_TRUE(compiled.valid());
    return compiled.valid() ? std::string(compiled.msl_text()) : std::string();
}

} // namespace

// D3D11.3 22.4.14 resinfo: the float and uint return modes on every texture
// dimension, a TextureCubeArray (z = the cube count), multisampled textures
// (no level of detail) and typed UAVs (view size, one mip). All of these
// were refused before.
TEST(DxbcToSpirvTest, ResinfoCoversEveryReturnModeAndDimension) {
    const std::string f = translate_blob_to_msl(
        a09b_cs_resinfo_f, sizeof(a09b_cs_resinfo_f), ShaderStage::Compute);
    EXPECT_NE(f.find("get_num_mip_levels()"), std::string::npos) << f;
    EXPECT_NE(f.find("float("), std::string::npos) << f;
    const std::string cube = translate_blob_to_msl(
        a09b_cs_resinfo_u_cubearray, sizeof(a09b_cs_resinfo_u_cubearray),
        ShaderStage::Compute);
    EXPECT_NE(cube.find("texturecube_array"), std::string::npos) << cube;
    EXPECT_NE(cube.find("get_array_size()"), std::string::npos) << cube;
    const std::string ms = translate_blob_to_msl(
        a09b_cs_resinfo_ms, sizeof(a09b_cs_resinfo_ms), ShaderStage::Compute);
    EXPECT_NE(ms.find("texture2d_ms"), std::string::npos) << ms;
    EXPECT_NE(ms.find("texture2d_ms_array"), std::string::npos) << ms;
    const std::string uav = translate_blob_to_msl(
        a09b_cs_resinfo_uav, sizeof(a09b_cs_resinfo_uav),
        ShaderStage::Compute);
    EXPECT_NE(uav.find("texture3d<float> u5"), std::string::npos) << uav;
    EXPECT_NE(uav.find("u5.get_depth()"), std::string::npos) << uav;
}

// 22.4.1 bufinfo (opcode 121): typed views report their element count (the
// texture buffer's width), raw views bytes and structured views structures,
// both from the bound view sizes in the push block.
TEST(DxbcToSpirvTest, BufinfoReportsViewElementCounts) {
    const auto container = parse_dxbc(a09b_cs_bufinfo, sizeof(a09b_cs_bufinfo));
    ASSERT_TRUE(container.valid());
    DxbcTranslationOptions options{};
    options.guard_buffer_bounds = true;
    const auto result = translate_dxbc_to_spirv(container, options);
    ASSERT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpImageQuerySize=*/104u));
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpUDiv=*/134u));
    // Without the push block (no bounds guard) raw/structured sizes are not
    // available, and that fails loudly rather than returning 0.
    const auto unguarded = translate_dxbc_to_spirv(container);
    EXPECT_FALSE(unguarded.valid());
    EXPECT_NE(unguarded.failure.find("BUFINFO"), std::string::npos);
}

// 22.4.21/22 sampleinfo / samplepos (111 / 110) on a multisampled t# and on
// `rasterizer`, 22.4.23-25 eval_sample_index / eval_centroid / eval_snapped
// (204 / 205 / 203) and 22.4.26-ish lod (108): all refused before.
TEST(DxbcToSpirvTest, SampleQueriesEvalAndLodTranslate) {
    const std::string samples = translate_blob_to_msl(
        a09b_cs_sampleinfo_only, sizeof(a09b_cs_sampleinfo_only),
        ShaderStage::Compute);
    EXPECT_NE(samples.find("get_num_samples()"), std::string::npos) << samples;
    const std::string positions = translate_blob_to_msl(
        a09b_cs_samplepos, sizeof(a09b_cs_samplepos), ShaderStage::Compute);
    EXPECT_NE(positions.find("get_num_samples()"), std::string::npos);
    const std::string eval = translate_blob_to_msl(
        a09b_ps_eval, sizeof(a09b_ps_eval), ShaderStage::Fragment);
    EXPECT_NE(eval.find("interpolate_at_sample("), std::string::npos) << eval;
    EXPECT_NE(eval.find("interpolate_at_centroid("), std::string::npos);
    EXPECT_NE(eval.find("interpolate_at_offset("), std::string::npos);
    EXPECT_NE(eval.find("vitreo_sampler_lod_biases"), std::string::npos)
        << "rasterizer sample count comes from the push block";
    const std::string lod = translate_blob_to_msl(
        a09b_ps_lod, sizeof(a09b_ps_lod), ShaderStage::Fragment);
    EXPECT_NE(lod.find("calculate_clamped_lod("), std::string::npos) << lod;
    EXPECT_NE(lod.find("calculate_unclamped_lod("), std::string::npos);
}

// 22.12.8/14: uaddc (132) and usubb (133) translate to an add/subtract plus a
// 0/1 carry/borrow compare; fxc never emits them, so the words are built by
// hand as the probe's patch does (tools/oracle_probe/d3d11_a09a_build.sh).
TEST(DxbcToSpirvTest, UaddcAndUsubbProduceValueAndFlag) {
    for (const u32 opcode : {132u, 133u}) {
        DxbcContainer container{};
        container.shader_model_major = 5;
        container.stage = ShaderStage::Compute;
        const u32 program[] = {
            0x00050050u, 18u,          // cs_5_0, 18 DWORDs
            0x02000068u, 2u,           // dcl_temps 2
            0x0400009Bu, 1u, 1u, 1u,   // dcl_thread_group 1, 1, 1
            opcode | (9u << 24),       // uaddc / usubb
            0x00100012u, 0u,           //   dest0 r0.x
            0x00100022u, 0u,           //   dest1 r0.y
            0x0010000Au, 1u,           //   src0 r1.x
            0x0010001Au, 1u,           //   src1 r1.y
            0x0100003Eu,               // ret
        };
        container.bytecode.assign(reinterpret_cast<const u8*>(program),
                                  reinterpret_cast<const u8*>(program) +
                                      sizeof(program));
        const auto result = translate_dxbc_to_spirv(container);
        ASSERT_TRUE(result.valid()) << opcode << ": " << result.failure;
        EXPECT_TRUE(spirv_contains_opcode(
            result.spirv, opcode == 132u ? /*OpIAdd=*/128u : /*OpISub=*/130u));
        EXPECT_TRUE(spirv_contains_opcode(result.spirv,
                                          /*OpULessThan=*/176u));
    }
}

// ---------------------------------------------------------------------------
// Dynamic shader linkage (D3D11.3 7.19): the probe's fxc blobs
// (tools/oracle_probe/d3d11_classlink_shaders.h, d3d11_classlink.hlsl). The
// expected class types, instances and slots are fxc's own /Fc listing of
// cs_multi ("Available Class Types", "Available Class Instances",
// "Interface slots"); d3d11_classlink_probe.expected holds what WARP runs.
namespace classlink_blobs {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "../oracle_data/d3d11_classlink_shaders.h"
#pragma clang diagnostic pop
}  // namespace classlink_blobs

TEST(DxbcToSpirvTest, ClassLinkageChunkMatchesTheFxcListing) {
    const auto container = parse_dxbc(classlink_blobs::k_cs_multi,
                                      sizeof(classlink_blobs::k_cs_multi));
    ASSERT_TRUE(container.valid());
    ASSERT_TRUE(container.has_class_linkage);
    const auto& classes = container.class_linkage;
    // Name, ID (the record index), CB Stride, Texture, Sampler.
    const std::vector<std::tuple<const char*, u32, u32, u32>> types = {
        {"SampOp", 0, 1, 1}, {"BufOp", 1, 1, 0}, {"MulK", 1, 0, 0},
        {"AddK", 1, 0, 0},   {"XorOp", 0, 0, 0}, {"NotMod", 0, 0, 0},
        {"ShlMod", 1, 0, 0}};
    ASSERT_EQ(classes.types.size(), types.size());
    for (usize i = 0; i < types.size(); ++i) {
        EXPECT_EQ(classes.types[i].name, std::get<0>(types[i]));
        EXPECT_EQ(classes.types[i].cb_stride, std::get<1>(types[i]));
        EXPECT_EQ(classes.types[i].texture_count, std::get<2>(types[i]));
        EXPECT_EQ(classes.types[i].sampler_count, std::get<3>(types[i]));
    }
    // Name, Type, CB, CB Offset, Texture, Sampler ("-" = kDxbcClassNone);
    // g_adds[3] is one record of count 3. The data-less g_xor and g_not are
    // not listed.
    constexpr u32 kNone = kDxbcClassNone;
    const std::vector<std::tuple<const char*, u32, u32, u32, u32, u32, u32>>
        instances = {{"g_buf", 1, 1, 2, 4, 1, kNone},
                     {"g_samp", 0, 1, kNone, kNone, 2, 0},
                     {"g_adds", 3, 3, 1, 0, kNone, kNone},
                     {"g_add0", 3, 1, 2, 0, kNone, kNone},
                     {"g_mul", 2, 1, 2, 1, kNone, kNone},
                     {"g_add1", 3, 1, 2, 2, kNone, kNone},
                     {"g_shl", 6, 1, 2, 3, kNone, kNone}};
    ASSERT_EQ(classes.instances.size(), instances.size());
    for (usize i = 0; i < instances.size(); ++i) {
        const auto& got = classes.instances[i];
        EXPECT_EQ(got.name, std::get<0>(instances[i]));
        EXPECT_EQ(got.type, std::get<1>(instances[i])) << got.name;
        EXPECT_EQ(got.count, std::get<2>(instances[i])) << got.name;
        EXPECT_EQ(got.constant_buffer, std::get<3>(instances[i])) << got.name;
        EXPECT_EQ(got.cb_offset, std::get<4>(instances[i])) << got.name;
        EXPECT_EQ(got.texture, std::get<5>(instances[i])) << got.name;
        EXPECT_EQ(got.sampler, std::get<6>(instances[i])) << got.name;
    }
    // "Interface slots, 6 total": slot 0 types 0-4 -> tables 0-4, slots 1-4
    // types 0-4 -> tables 5-9, slot 5 types 5-6 -> tables 10-11.
    EXPECT_EQ(classes.slot_count, 6u);
    ASSERT_EQ(classes.slot_runs.size(), 3u);
    EXPECT_EQ(classes.slot_runs[0].count, 1u);
    EXPECT_EQ(classes.slot_runs[0].type_ids, (std::vector<u32>{0, 1, 2, 3, 4}));
    EXPECT_EQ(classes.slot_runs[0].table_ids,
              (std::vector<u32>{0, 1, 2, 3, 4}));
    EXPECT_EQ(classes.slot_runs[1].count, 4u);
    EXPECT_EQ(classes.slot_runs[1].table_ids,
              (std::vector<u32>{5, 6, 7, 8, 9}));
    EXPECT_EQ(classes.slot_runs[2].count, 1u);
    EXPECT_EQ(classes.slot_runs[2].type_ids, (std::vector<u32>{5, 6}));
    EXPECT_EQ(classes.slot_runs[2].table_ids, (std::vector<u32>{10, 11}));
    // A program without interfaces has no chunk.
    EXPECT_FALSE(parse_dxbc(classlink_blobs::k_cs_plain,
                            sizeof(classlink_blobs::k_cs_plain))
                     .has_class_linkage);
}

TEST(DxbcToSpirvTest, SlotBindingsTravelWithACopyOfTheContainer) {
    const auto& blob = classlink_blobs::k_cs_multi;
    std::vector<DxbcSlotBinding> bindings(6);
    for (u32 slot = 0; slot < 6; ++slot) {
        bindings[slot].table = slot;
        bindings[slot].this_data = {slot, slot + 10, slot + 20, slot + 30};
    }
    const auto copy = dxbc_with_slot_bindings(blob, sizeof(blob), bindings);
    ASSERT_EQ(copy.size(), sizeof(blob) + 4u + 8u + 4u + 6u * 20u);
    const auto original = parse_dxbc(blob, sizeof(blob));
    const auto parsed = parse_dxbc(copy.data(), copy.size());
    ASSERT_TRUE(parsed.valid());
    EXPECT_EQ(parsed.slot_bindings, bindings);
    EXPECT_EQ(parsed.bytecode, original.bytecode);
    EXPECT_EQ(parsed.class_linkage.slot_count, 6u);
    EXPECT_EQ(parsed.resource_bindings.size(),
              original.resource_bindings.size());
    EXPECT_TRUE(original.slot_bindings.empty());
    EXPECT_TRUE(dxbc_with_slot_bindings(blob, 16, bindings).empty());
}

// Without a binding (a program validated at creation) every interface slot
// is unbound and the program still translates -- each function body too;
// with one it translates for exactly that binding, and a table the slot's
// interface does not list, or a binding of the wrong length, is refused.
TEST(DxbcToSpirvTest, InterfaceProgramsTranslateForTheirBinding) {
    using namespace classlink_blobs;
    struct Blob {
        const unsigned char* code;
        usize size;
    };
    const Blob blobs[] = {{k_cs_multi, sizeof(k_cs_multi)},
                          {k_cs_dyn, sizeof(k_cs_dyn)},
                          {k_cs_nested, sizeof(k_cs_nested)},
                          {k_vs_iface, sizeof(k_vs_iface)},
                          {k_ps_iface, sizeof(k_ps_iface)}};
    for (const Blob& blob : blobs) {
        const auto container = parse_dxbc(blob.code, blob.size);
        ASSERT_TRUE(dxbc_uses_class_linkage(container));
        const auto result = translate_dxbc_to_spirv(container);
        EXPECT_TRUE(result.valid()) << result.failure;
    }
    DxbcTranslationOptions geometry{};
    geometry.emulate_geometry_with_compute = true;
    EXPECT_TRUE(translate_dxbc_to_spirv(
                    parse_dxbc(k_gs_iface, sizeof(k_gs_iface)), geometry)
                    .valid());
    // As CreateHullShader / CreateDomainShader lower them.
    DxbcTranslationOptions tessellation{};
    tessellation.emulate_tessellation = true;
    tessellation.domain_to_compute = true;
    for (const Blob& blob : {Blob{k_hs_iface, sizeof(k_hs_iface)},
                             Blob{k_ds_iface, sizeof(k_ds_iface)}}) {
        const auto result = translate_dxbc_to_spirv(
            parse_dxbc(blob.code, blob.size), tessellation);
        EXPECT_TRUE(result.valid()) << result.failure;
    }

    // cs_multi bound as the probe's multi_a: slot 0 XorOp (table 4), slots
    // 1-4 AddK/MulK/AddK/BufOp (tables 8, 7, 8, 6), slot 5 ShlMod (11).
    std::vector<DxbcSlotBinding> bindings = {
        {4, {0, 0, 0, 0}}, {8, {2, 0, 0, 0}}, {7, {2, 1, 0, 0}},
        {8, {2, 2, 0, 0}}, {6, {2, 4, 1, 0}}, {11, {2, 3, 0, 0}}};
    auto copy = dxbc_with_slot_bindings(k_cs_multi, sizeof(k_cs_multi),
                                        bindings);
    auto result = translate_dxbc_to_spirv(parse_dxbc(copy.data(), copy.size()));
    EXPECT_TRUE(result.valid()) << result.failure;
    EXPECT_TRUE(spirv_contains_opcode(result.spirv, /*OpFunctionCall=*/57u));

    bindings[5].table = 3;  // AddK's slot-0 table at the IMod slot
    copy = dxbc_with_slot_bindings(k_cs_multi, sizeof(k_cs_multi), bindings);
    result = translate_dxbc_to_spirv(parse_dxbc(copy.data(), copy.size()));
    EXPECT_FALSE(result.valid());
    EXPECT_NE(result.failure.find("is not a choice of interface slot 5"),
              std::string::npos) << result.failure;

    bindings.resize(5);
    copy = dxbc_with_slot_bindings(k_cs_multi, sizeof(k_cs_multi), bindings);
    result = translate_dxbc_to_spirv(parse_dxbc(copy.data(), copy.size()));
    EXPECT_FALSE(result.valid());
}

// Every committed fxc shader header (all stages, ~300 shaders) translates
// to SPIR-V that
// spirv-val accepts. SPIRV-Cross tolerates modules the specification
// rejects -- a Dim=Buffer image without SampledBuffer / ImageBuffer, a loop
// guard variable missing from the entry point's interface -- so nothing else
// would notice those. Skipped where SPIRV-Tools' spirv-val is not on PATH.
TEST(DxbcToSpirvTest, EveryProbeShaderTranslatesToValidSpirv) {
#ifndef VITREO_SOURCE_DIR
    GTEST_SKIP() << "VITREO_SOURCE_DIR not defined";
#else
    if (std::system("command -v spirv-val >/dev/null 2>&1") != 0) {
        GTEST_SKIP() << "spirv-val not on PATH";
    }
    namespace fs = std::filesystem;
    const fs::path probes = fs::path(VITREO_SOURCE_DIR) / "tests/oracle_data";
    const fs::path scratch =
        fs::temp_directory_path() / "vitreo_spirv_val_probe_shaders";
    fs::create_directories(scratch);
    usize validated = 0;
    std::vector<std::string> invalid;
    for (const auto& entry : fs::directory_iterator(probes)) {
        const std::string file = entry.path().filename().string();
        if (file.size() < 10 ||
            file.compare(file.size() - 10, 10, "_shaders.h") != 0) {
            continue;
        }
        std::ifstream in(entry.path());
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        // `unsigned char NAME[N] = { b, b, ... };` in decimal (fxc /Fh).
        usize at = 0;
        while ((at = text.find("unsigned char ", at)) != std::string::npos) {
            const usize name_end = text.find('[', at);
            const usize open = text.find('{', at);
            const usize close = text.find("};", at);
            if (name_end == std::string::npos || open == std::string::npos ||
                close == std::string::npos || open > close) {
                break;
            }
            const std::string name = text.substr(at + 14, name_end - at - 14);
            std::vector<u8> blob;
            const char* p = text.c_str() + open + 1;
            const char* end = text.c_str() + close;
            while (p < end) {
                char* next = nullptr;
                const unsigned long v = std::strtoul(p, &next, 0);
                if (next == p) { ++p; continue; }
                blob.push_back(static_cast<u8>(v));
                p = next;
            }
            at = close + 2;
            const auto container = parse_dxbc(blob.data(), blob.size());
            if (!container.valid()) continue;
            DxbcTranslationOptions options{};
            options.fixed_function_push = true;
            options.guard_constant_buffers = true;
            // As the D3D11 frontend translates them (d3d11_tessellation.cpp).
            options.emulate_tessellation =
                container.stage == ShaderStage::Hull ||
                container.stage == ShaderStage::Domain;
            const auto spirv = translate_dxbc_to_spirv(container, options);
            if (!spirv.valid()) continue;  // refused shapes are other tests'
            const fs::path spv = scratch / (name + ".spv");
            {
                std::ofstream out(spv, std::ios::binary);
                out.write(reinterpret_cast<const char*>(spirv.spirv.data()),
                          static_cast<std::streamsize>(spirv.spirv.size() * 4));
            }
            const std::string command = "spirv-val --target-env spv1.6 '" +
                                        spv.string() + "' >/dev/null 2>&1";
            ++validated;
            if (std::system(command.c_str()) != 0) {
                invalid.push_back(file + ":" + name);
            }
        }
    }
    EXPECT_GT(validated, 100u);
    for (const auto& name : invalid) ADD_FAILURE() << "spirv-val rejects " << name;
#endif
}

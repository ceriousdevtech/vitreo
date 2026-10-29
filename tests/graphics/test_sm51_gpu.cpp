// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_sm51_gpu.cpp
/// @brief Shader model 5.1 descriptor arrays on a real GPU: every corpus blob
/// compiles to Metal through the descriptor-table (argument-buffer) path,
/// and the execution shader, dispatched through the backend's descriptor
/// tables, writes exactly what WARP12 wrote.
///
/// Blobs: tools/oracle_probe/d3d12_sm51/ (fxc 10.1 on Windows). Expected
/// output: tools/oracle_probe/d3d12_sm51_exec_probe.expected, from
/// d3d12_sm51_exec_probe.c run on WARP12.

#include <gtest/gtest.h>

#include "graphics/contracts/backend_device.hpp"
#include "graphics/metal/metal_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include "../oracle_data/d3d12_sm51/d3d12_sm51_shaders.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

constexpr u32 kRunSlots = 32;

u32 class_set(DxbcRegisterClass cls) {
    switch (cls) {
    case DxbcRegisterClass::ConstantBuffer: return 0;
    case DxbcRegisterClass::ShaderResource: return 1;
    case DxbcRegisterClass::Sampler: return 2;
    case DxbcRegisterClass::UnorderedAccess: return 3;
    }
    return 0;
}

/// One table per class; each (class, space) gets kRunSlots bindings in space
/// order, the last running to the end of the table.
struct PackedLayout {
    std::vector<DxbcRegisterRemap> remap;
    std::array<u32, MSL_DESCRIPTOR_SET_COUNT> slots{};
};

PackedLayout packed_layout(const DxbcContainer& c) {
    std::map<DxbcRegisterClass, std::set<u32>> spaces;
    for (const auto& d : c.binding_declarations) {
        spaces[d.register_class].insert(d.space);
    }
    PackedLayout layout;
    for (const auto& [cls, list] : spaces) {
        u32 k = 0;
        for (const u32 space : list) {
            const bool last = k + 1 == list.size();
            layout.remap.push_back({cls, space, 0u,
                                    last ? kDxbcUnboundedRange : kRunSlots,
                                    class_set(cls), k * kRunSlots});
            ++k;
        }
        // The last run's unbounded arrays end with the table.
        layout.slots[class_set(cls)] = (k + 1) * kRunSlots;
    }
    return layout;
}

MslCompileOptions table_options(const std::array<u32, 4>& slots) {
    MslCompileOptions m{};
    m.argument_buffers = true;
    const MslDescriptorClass classes[4] = {
        MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
        MslDescriptorClass::Sampler, MslDescriptorClass::Buffer};
    for (u32 set = 0; set < 4; ++set) {
        m.argument_buffer_sets[set] = {
            slots[set], BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set,
            classes[set]};
    }
    return m;
}

const Sm51Blob* blob_named(const char* name) {
    for (const auto& blob : kSm51Blobs) {
        if (std::string(blob.name) == name) return &blob;
    }
    return nullptr;
}

/// The lines of d3d12_sm51_exec_probe.expected from the header naming
/// @p shader ("d3d12_sm51_exec_probe: <shader> on WARP12") to the next one.
std::vector<std::string> expected_section(const std::string& shader) {
    std::ifstream file(std::string(VITREO_SOURCE_DIR) +
                       "/tests/oracle_data/d3d12_sm51_exec_probe.expected");
    std::vector<std::string> lines;
    bool inside = false;
    for (std::string line; std::getline(file, line);) {
        if (line.rfind("d3d12_sm51_exec_probe: ", 0) == 0) {
            inside = line.find(" " + shader + " ") != std::string::npos;
        }
        if (inside) lines.push_back(line);
    }
    return lines;
}

std::unique_ptr<BackendBuffer> buffer(const void* data, usize bytes, bool uav,
                                      bool cb = false) {
    BackendBufferDesc desc{};
    desc.byte_width = bytes;
    desc.shader_resource = !cb;
    desc.constant_buffer = cb;
    desc.unordered_access = uav;
    return MetalDevice::instance().create_buffer(desc, data);
}

}  // namespace

// Every blob (the ROV one aside: refused) translates with a remap and its
// MSL -- descriptor arrays as argument-buffer arrays, runtime arrays sized
// by their table -- is accepted by the Metal compiler; the vertex and pixel
// shaders as one pipeline, the rest as compute kernels (the geometry, hull
// and domain shaders lowered as the D3D11 frontend lowers them).
TEST(Sm51Gpu, EveryCorpusShaderCompilesToMetal) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }
    std::string vertex_msl;
    std::vector<std::pair<std::string, std::string>> fragments;
    for (const auto& blob : kSm51Blobs) {
        const std::string name = blob.name;
        if (name == "ps_rov") continue;
        const auto c = parse_dxbc(blob.code, blob.size);
        ASSERT_TRUE(c.valid()) << name;
        const PackedLayout layout = packed_layout(c);
        DxbcTranslationOptions options{};
        options.register_remap = layout.remap;
        options.emulate_geometry_with_compute =
            c.stage == ShaderStage::Geometry;
        options.emulate_tessellation =
            c.stage == ShaderStage::Hull || c.stage == ShaderStage::Domain;
        options.domain_to_compute = c.stage == ShaderStage::Domain;
        const auto spirv = translate_dxbc_to_spirv(c, options);
        ASSERT_TRUE(spirv.valid()) << name << ": " << spirv.failure;
        auto msl_options = table_options(layout.slots);
        if (spirv.uses_typed_uav_atomics) {
            // As the frontends raise it (d3d11_pipeline.cpp): an atomic on a
            // storage image -- here an element of an array -- is native from
            // MSL 3.1, and SPIRV-Cross cannot emulate it for an array.
            auto refused = msl_options;
            refused.msl_version = 30000u;
            EXPECT_FALSE(compile_spirv_to_msl(spirv.spirv.data(),
                                              spirv.spirv.size(), spirv.stage,
                                              refused)
                             .valid())
                << name;
            msl_options.msl_version = 30100u;
        }
        const auto msl = compile_spirv_to_msl(
            spirv.spirv.data(), spirv.spirv.size(), spirv.stage, msl_options);
        ASSERT_TRUE(msl.valid()) << name;
        const std::string text = msl.msl_text();
        if (spirv.stage == ShaderStage::Vertex) {
            vertex_msl = text;
            continue;
        }
        if (spirv.stage == ShaderStage::Fragment) {
            fragments.emplace_back(name, text);
            continue;
        }
        ASSERT_EQ(spirv.stage, ShaderStage::Compute) << name;
        BackendComputePipelineDesc pd{};
        pd.compute_shader_msl = text;
        pd.compute_entry_point = "main0";
        pd.cache_key = std::hash<std::string>{}(text);
        auto pipeline = device.create_compute_pipeline(pd);
        EXPECT_TRUE(pipeline && pipeline->valid()) << name << "\n" << text;
    }
    ASSERT_FALSE(vertex_msl.empty());
    ASSERT_EQ(fragments.size(), 2u);  // ps_arrays, ps_ops
    for (const auto& [name, fragment_msl] : fragments) {
        BackendPipelineDesc desc{};
        desc.vertex_shader_msl = vertex_msl;
        desc.fragment_shader_msl = fragment_msl;
        desc.vertex_entry_point = "main0";
        desc.fragment_entry_point = "main0";
        desc.color_format = PixelFormat::RGBA8Unorm;
        desc.cache_key = std::hash<std::string>{}(vertex_msl + fragment_msl);
        auto pipeline = device.create_pipeline(desc);
        EXPECT_TRUE(pipeline && pipeline->valid())
            << name << "\n--- vertex ---\n" << vertex_msl
            << "\n--- fragment ---\n" << fragment_msl;
    }
}

// cs_exec through the backend's descriptor tables, laid out as a D3D12
// frontend would resolve d3d12_sm51_exec_probe.c's root signature: one table
// per class, spaces packed into it by the remap, the unbounded ranges (rb[]
// in space 2, outr[] in space 1) last and sized by their table.
TEST(Sm51Gpu, ExecutionShaderMatchesWarp) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }
    const Sm51Blob* blob = blob_named("cs_exec");
    ASSERT_NE(blob, nullptr);
    const auto c = parse_dxbc(blob->code, blob->size);
    ASSERT_TRUE(c.valid());

    using C = DxbcRegisterClass;
    DxbcTranslationOptions options{};
    options.register_remap = {
        // b0 space0 (the root constants) -> 0; b1-b3 space1 -> 1..3.
        {C::ConstantBuffer, 0, 0, 1, 0, 0},
        {C::ConstantBuffer, 1, 1, 3, 0, 1},
        // t0-t3 space1 -> 0..3, t2-t3 space3 -> 4..5, t8-t10 space0 ->
        // 6..8, t0.. space2 -> 9.. (to the end of the 12-slot table).
        {C::ShaderResource, 1, 0, 4, 1, 0},
        {C::ShaderResource, 3, 2, 2, 1, 4},
        {C::ShaderResource, 0, 8, 3, 1, 6},
        {C::ShaderResource, 2, 0, kDxbcUnboundedRange, 1, 9},
        // u0-u1 space0 -> 0..1, u0.. space1 -> 2.. (a 4-slot table).
        {C::UnorderedAccess, 0, 0, 2, 3, 0},
        {C::UnorderedAccess, 1, 0, kDxbcUnboundedRange, 3, 2},
    };
    const auto spirv = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    const std::array<u32, 4> slots{4, 12, 0, 4};
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(),
                                          ShaderStage::Compute,
                                          table_options(slots));
    ASSERT_TRUE(msl.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = std::hash<std::string>{}(pd.compute_shader_msl);
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << pd.compute_shader_msl;

    // The probe's inputs (d3d12_sm51_exec_probe.c's header).
    const std::array<u32, 4> root{2u, 1u, 0u, 0u};
    auto root_cb = buffer(root.data(), 256, false, true);
    std::vector<std::unique_ptr<BackendBuffer>> cbs, sbs, rbs, tbs, outs, outr;
    std::vector<std::unique_ptr<BackendTextureBuffer>> tb_views;
    std::vector<std::unique_ptr<BackendTexture2D>> textures;
    for (u32 k = 0; k < 3; ++k) {
        std::array<u32, 64> cb{};
        for (u32 j = 0; j < 2; ++j) {
            for (u32 ch = 0; ch < 4; ++ch) {
                cb[j * 4 + ch] = 0x10000u * (k + 1) + 0x100u * j + ch;
            }
        }
        cbs.push_back(buffer(cb.data(), sizeof(cb), false, true));
    }
    for (u32 k = 0; k < 4; ++k) {
        std::array<u32, 32> sb{};
        for (u32 e = 0; e < 16; ++e) {
            sb[e * 2] = 1000u * (k + 1) + e;
            sb[e * 2 + 1] = 7u * e + k;
        }
        sbs.push_back(buffer(sb.data(), sizeof(sb), false));
    }
    for (u32 k = 0; k < 3; ++k) {
        std::array<u32, 8> rb{};
        for (u32 w = 0; w < 8; ++w) rb[w] = 0x1000u * (k + 1) + 3u * w;
        rbs.push_back(buffer(rb.data(), sizeof(rb), false));
    }
    for (u32 k = 0; k < 2; ++k) {
        std::array<u32, 8> tb{};
        for (u32 e = 0; e < 8; ++e) tb[e] = 500u * (k + 1) + e * e;
        tbs.push_back(buffer(tb.data(), sizeof(tb), false));
        tb_views.push_back(device.create_texture_buffer(
            *tbs.back(), PixelFormat::R32Uint, 0, 8, false));
        ASSERT_TRUE(tb_views.back() && tb_views.back()->valid());
    }
    for (u32 k = 0; k < 3; ++k) {
        std::array<u32, 16> texels{};
        for (u32 y = 0; y < 4; ++y) {
            for (u32 x = 0; x < 4; ++x) {
                texels[y * 4 + x] = 0x100000u * (k + 1) + 16u * y + x;
            }
        }
        BackendTexture2DDesc td{};
        td.width = td.height = 4;
        td.pixel_format = PixelFormat::R32Uint;
        td.shader_resource = true;
        textures.push_back(device.create_texture_2d(td, texels.data(), 16));
        ASSERT_NE(textures.back(), nullptr);
    }
    const std::vector<u32> zeros(256, 0u);
    for (u32 k = 0; k < 2; ++k) {
        outs.push_back(buffer(zeros.data(), 32 * 16, true));
        outr.push_back(buffer(zeros.data(), 256 * 4, true));
    }

    auto cb_table = device.create_descriptor_table(
        {slots[0], BackendDescriptorClass::Buffer});
    auto srv_table = device.create_descriptor_table(
        {slots[1], BackendDescriptorClass::Texture});
    auto uav_table = device.create_descriptor_table(
        {slots[3], BackendDescriptorClass::Buffer});
    ASSERT_TRUE(cb_table && srv_table && uav_table);
    ASSERT_TRUE(cb_table->set_buffer(0, root_cb.get(), 0));
    for (u32 k = 0; k < 3; ++k) {
        ASSERT_TRUE(cb_table->set_buffer(1 + k, cbs[k].get(), 0));
    }
    BackendTextureViewDesc view{};
    view.dimension = BackendTextureViewDimension::Texture2D;
    view.mip_levels = 1;
    view.array_size = 1;
    for (u32 k = 0; k < 4; ++k) {
        ASSERT_TRUE(srv_table->set_buffer(k, sbs[k].get(), 0));
    }
    for (u32 k = 0; k < 2; ++k) {
        ASSERT_TRUE(srv_table->set_texture_buffer(4 + k, tb_views[k].get()));
    }
    for (u32 k = 0; k < 3; ++k) {
        ASSERT_TRUE(srv_table->set_texture(6 + k, textures[k].get(), view));
        ASSERT_TRUE(srv_table->set_buffer(9 + k, rbs[k].get(), 0));
    }
    for (u32 k = 0; k < 2; ++k) {
        ASSERT_TRUE(uav_table->set_buffer(k, outs[k].get(), 0));
        ASSERT_TRUE(uav_table->set_buffer(2 + k, outr[k].get(), 0));
    }

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 64;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.descriptor_tables[0] = cb_table.get();
    call.descriptor_tables[1] = srv_table.get();
    call.descriptor_tables[3] = uav_table.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());

    // Printed as the probe prints it.
    std::vector<std::string> lines{"d3d12_sm51_exec_probe: cs_exec on WARP12"};
    char line[160];
    for (u32 k = 0; k < 2; ++k) {
        std::array<u32, 128> v{};
        ASSERT_TRUE(outs[k]->read(0, v.data(), sizeof(v)));
        for (u32 e = 0; e < 32; ++e) {
            std::snprintf(line, sizeof(line),
                          "outs%u %02u: %08x %08x %08x %08x", k, e, v[e * 4],
                          v[e * 4 + 1], v[e * 4 + 2], v[e * 4 + 3]);
            lines.emplace_back(line);
        }
    }
    for (u32 k = 0; k < 2; ++k) {
        std::array<u32, 256> r{};
        ASSERT_TRUE(outr[k]->read(0, r.data(), sizeof(r)));
        for (u32 w = 0; w < 256; w += 8) {
            std::snprintf(line, sizeof(line),
                          "outr%u %03u: %08x %08x %08x %08x %08x %08x %08x "
                          "%08x",
                          k, w, r[w], r[w + 1], r[w + 2], r[w + 3], r[w + 4],
                          r[w + 5], r[w + 6], r[w + 7]);
            lines.emplace_back(line);
        }
    }

    const std::vector<std::string> expected = expected_section("cs_exec");
    ASSERT_EQ(lines.size(), expected.size());
    for (usize i = 0; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i], expected[i]) << "line " << i + 1;
    }
}

// cs_bias: the per-descriptor sampler state Metal samplers lack, read from
// DxbcTranslationOptions::sampler_state_buffer -- s1 of a sampler array
// carries MipLODBias +1, both carry a POINT mip filter -- matches what
// WARP12 read through the same descriptors (d3d12_sm51_exec_probe.c
// sampler_bias(): SampleLevel of mips 1.0 / 2.0 / 3.0 at LOD 0..1.5, the
// .5 ties rounding up, 7.18.10).
TEST(Sm51Gpu, SamplerStateBufferAppliesBiasAndPointMip) {
    auto& device = MetalDevice::instance();
    if (!device.available()) GTEST_SKIP() << "no Metal device";
    if (!device.supports_descriptor_tables()) {
        GTEST_SKIP() << "no Tier-2 argument buffers";
    }
    const Sm51Blob* blob = blob_named("cs_bias");
    ASSERT_NE(blob, nullptr);
    const auto c = parse_dxbc(blob->code, blob->size);
    using C = DxbcRegisterClass;
    DxbcTranslationOptions options{};
    // s0-s1 space1 -> set 2 slots 4-5 (so the record index is the binding,
    // not the element); the state buffer rides in the CB table's slot 3.
    options.register_remap = {
        {C::ShaderResource, 0, 0, 1, 1, 0},
        {C::UnorderedAccess, 0, 0, 1, 3, 0},
        {C::Sampler, 1, 0, 2, 2, 4},
    };
    options.sampler_state_buffer = {true, 0, 3};
    const auto spirv = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    const std::array<u32, 4> slots{4, 1, 6, 1};
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(),
                                          spirv.spirv.size(),
                                          ShaderStage::Compute,
                                          table_options(slots));
    ASSERT_TRUE(msl.valid());
    BackendComputePipelineDesc pd{};
    pd.compute_shader_msl = msl.msl_text();
    pd.compute_entry_point = "main0";
    pd.cache_key = std::hash<std::string>{}(pd.compute_shader_msl);
    auto pipeline = device.create_compute_pipeline(pd);
    ASSERT_TRUE(pipeline && pipeline->valid()) << pd.compute_shader_msl;

    // Mip m of a 4x4 R32_FLOAT texture holds m + 1.0.
    BackendTexture2DDesc td{};
    td.width = td.height = 4;
    td.mip_levels = 3;
    td.pixel_format = PixelFormat::R32Float;
    td.shader_resource = true;
    auto texture = device.create_texture_2d(td, nullptr, 0);
    ASSERT_NE(texture, nullptr);
    for (u32 mip = 0; mip < 3; ++mip) {
        const u32 size = 4u >> mip;
        const std::vector<float> texels(size * size, float(mip + 1));
        ASSERT_TRUE(texture->update(mip, texels.data(), size * 4));
    }
    const std::vector<float> zeros(16, 0.0f);
    auto output = buffer(zeros.data(), 64, true);
    // The records: {MipLODBias, flags, 0, 0} for bindings 4 and 5.
    std::array<u32, 6 * kSamplerStateRecordWords> records{};
    const float biases[2] = {0.0f, 1.0f};
    for (u32 k = 0; k < 2; ++k) {
        u32 bits = 0;
        std::memcpy(&bits, &biases[k], 4);
        records[(4 + k) * kSamplerStateRecordWords + kSamplerStateBiasWord] =
            bits;
        records[(4 + k) * kSamplerStateRecordWords + kSamplerStateFlagsWord] =
            kSamplerStatePointMip;
    }
    auto state = buffer(records.data(), sizeof(records), false);

    auto cb_table = device.create_descriptor_table(
        {slots[0], BackendDescriptorClass::Buffer});
    auto srv_table = device.create_descriptor_table(
        {slots[1], BackendDescriptorClass::Texture});
    auto sampler_table = device.create_descriptor_table(
        {slots[2], BackendDescriptorClass::Sampler});
    auto uav_table = device.create_descriptor_table(
        {slots[3], BackendDescriptorClass::Buffer});
    ASSERT_TRUE(cb_table && srv_table && sampler_table && uav_table);
    ASSERT_TRUE(cb_table->set_buffer(3, state.get(), 0));
    BackendTextureViewDesc view{};
    view.dimension = BackendTextureViewDimension::Texture2D;
    view.mip_levels = 3;
    view.array_size = 1;
    ASSERT_TRUE(srv_table->set_texture(0, texture.get(), view));
    ASSERT_TRUE(uav_table->set_buffer(0, output.get(), 0));
    for (u32 k = 0; k < 2; ++k) {
        // D3D12_FILTER_MIN_MAG_MIP_POINT, clamp, MinLOD 0, MaxLOD FLT_MAX;
        // the bias is the record's, not the Metal sampler's.
        BackendSamplerDesc sampler{};
        sampler.min_filter = sampler.mag_filter = sampler.mip_filter =
            BackendFilter::Nearest;
        sampler.address_u = sampler.address_v = sampler.address_w =
            BackendAddressMode::Clamp;
        ASSERT_TRUE(sampler_table->set_sampler(4 + k, sampler));
    }

    BackendDispatchCall call{};
    call.pipeline = pipeline.get();
    call.group_count_x = call.group_count_y = call.group_count_z = 1;
    call.threads_per_group_x = 8;
    call.threads_per_group_y = call.threads_per_group_z = 1;
    call.descriptor_tables[0] = cb_table.get();
    call.descriptor_tables[1] = srv_table.get();
    call.descriptor_tables[2] = sampler_table.get();
    call.descriptor_tables[3] = uav_table.get();
    ASSERT_TRUE(device.dispatch(call));
    ASSERT_TRUE(device.wait_idle());
    std::array<float, 16> values{};
    ASSERT_TRUE(output->read(0, values.data(), sizeof(values)));

    std::vector<std::string> lines{"d3d12_sm51_exec_probe: cs_bias on WARP12"};
    char line[96];
    for (u32 k = 0; k < 8; ++k) {
        std::snprintf(line, sizeof(line), "bias t%u s%u lod %.1f: %.1f", k,
                      k & 1u, double((k >> 1) * 0.5f), double(values[k]));
        lines.emplace_back(line);
    }
    EXPECT_EQ(lines, expected_section("cs_bias"));
}

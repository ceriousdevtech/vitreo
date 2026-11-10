// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxil_lower.cpp
/// @brief DXIL -> SM5.1 lowering: every lowered program is a whole, hashed
/// DXBC container the DXBC parser reads -- the declarations dx.resources
/// names (range ID, bounds, space, shape, stride) -- and the DXBC translator
/// compiles to SPIR-V and MSL. Execution against WARP is
/// test_dxil_gpu.cpp.

#include <gtest/gtest.h>

#include <fstream>

#include "graphics/contracts/backend_bindings.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxbc_to_spirv_sm4.hpp"
#include "graphics/shaders/dxil_to_dxbc.hpp"
#include "graphics/shaders/dxil_raytracing.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/dxbc_to_spirv_spv_defs.hpp"
#include "dxil_constant_test_fixture.hpp"
#include "../oracle_data/dxil_readonly/dxil_readonly_shaders.h"
#include "../oracle_data/d3d12_packed_bytes_shaders.h"

#include "../oracle_data/d3d12_sm6/d3d12_sm6_shaders.h"
#include "../oracle_data/d3d12_sm6/d3d12_sm6_stages_shaders.h"
#include "../oracle_data/d3d12_sm6x/d3d12_sm6x_shaders.h"
#include "../oracle_data/d3d12_dxilc/d3d12_dxilc_shaders.h"
#include "../oracle_data/d3d12_vinst/d3d12_vinst_shaders.h"
#include "../oracle_data/d3d12_vinst/d3d12_feedback_shaders.h"
#include "../oracle_data/d3d12_csderiv/d3d12_csderiv_shaders.h"
#include "../oracle_data/d3d12_bary/d3d12_bary_shaders.h"
#include "../oracle_data/d3d12_c4/d3d12_c4_shaders.h"
#include "../oracle_data/d3d12_sm68/d3d12_sm68_shaders.h"
#include "../oracle_data/d3d12_dxr/d3d12_dxr_shaders.h"
#include "../oracle_data/d3d12_dxr_local/d3d12_dxr_local_shaders.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <vector>
#include <cstdlib>
#include <set>
#include <string>

namespace vitreo::graphics {
namespace {

const Sm6Blob& blob(const char* name) {
    for (const auto& b : kSm6Blobs) {
        if (std::string(b.name) == name) return b;
    }
    // The wave / 64-bit / 16-bit corpus (tests/oracle_data/d3d12_sm6x).
    static std::vector<Sm6Blob> extra(std::size(kSm6xBlobs));
    for (usize i = 0; i < std::size(kSm6xBlobs); ++i) {
        if (std::string(kSm6xBlobs[i].name) != name) continue;
        extra[i] = Sm6Blob{kSm6xBlobs[i].name, kSm6xBlobs[i].code, kSm6xBlobs[i].size};
        return extra[i];
    }
    for (const auto& b : kDxilcBlobs) {
        if (std::string(b.name) == name) {
            static thread_local Sm6Blob found_c;
            found_c = Sm6Blob{b.name, b.code, b.size};
            return found_c;
        }
    }
    for (const auto& b : kSm68Blobs) {
        if (std::string(b.name) == name) {
            static thread_local Sm6Blob found_s;
            found_s = Sm6Blob{b.name, b.code, b.size};
            return found_s;
        }
    }
    for (const auto& b : kC4Blobs) {
        if (std::string(b.name) == name) {
            static thread_local Sm6Blob found_c4;
            found_c4 = Sm6Blob{b.name, b.code, b.size};
            return found_c4;
        }
    }
    for (const auto& b : kBaryBlobs) {
        if (std::string(b.name) == name) {
            static thread_local Sm6Blob found_b;
            found_b = Sm6Blob{b.name, b.code, b.size};
            return found_b;
        }
    }
    for (const auto& b : kCsderivBlobs) {
        if (std::string(b.name) == name) {
            static thread_local Sm6Blob found_d;
            found_d = Sm6Blob{b.name, b.code, b.size};
            return found_d;
        }
    }
    // The DXR corpus (tests/oracle_data/d3d12_dxr).
    for (const auto& b : kDxrBlobs) {
        if (std::string(b.name) == name) {
            static thread_local Sm6Blob found_r;
            found_r = Sm6Blob{b.name, b.code, b.size};
            return found_r;
        }
    }
    for (const auto& b : kSm6StageBlobs) {
        if (std::string(b.name) == name) {
            static thread_local Sm6Blob found;
            found = Sm6Blob{b.name, b.code, b.size};
            return found;
        }
    }
    static const Sm6Blob none{"", nullptr, 0};
    return none;
}

DxbcContainer lowered(const char* name) {
    const Sm6Blob& b = blob(name);
    const DxilToDxbcResult r = dxil_to_dxbc(b.code, b.size);
    EXPECT_TRUE(r.ok) << name << ": " << r.failure;
    if (!r.ok) return {};
    EXPECT_TRUE(dxbc_container_verified(r.container.data(), r.container.size())) << name;
    return parse_dxbc(r.container.data(), r.container.size());
}

TEST(DxilLower, ExecShaderDeclaresTheSm51Ranges) {
    // cs_exec.dxil.txt dx.resources: SRV 0 [4 x StructuredBuffer<uint2>] t0
    // space1 stride 8; SRV 1 ByteAddressBuffer[] t0.. space2; SRV 2 [2 x
    // Buffer<uint>] t2 space3; SRV 3 [3 x Texture2D<uint>] t8; UAV 0 [2 x
    // RWStructuredBuffer<uint4>] u0 stride 16; UAV 1 RWByteAddressBuffer[]
    // u0.. space1; CBV 0 Root b0 (8 bytes); CBV 1 [3 x cbarr] b1 space1 (32).
    const DxbcContainer c = lowered("cs_exec");
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(c.stage, ShaderStage::Compute);
    EXPECT_TRUE(c.is_sm51_plus());
    struct Want {
        DxbcRegisterClass cls;
        u32 id, space, lower, upper, opcode, stride, dimension, cb_vectors;
    };
    using C = DxbcRegisterClass;
    const Want want[] = {
        {C::ShaderResource, 0, 1, 0, 3, 162, 8, 0, 0},
        {C::ShaderResource, 1, 2, 0, kDxbcUnboundedRange, 161, 0, 0, 0},
        {C::ShaderResource, 2, 3, 2, 3, 88, 0, 1, 0},
        {C::ShaderResource, 3, 0, 8, 10, 88, 0, 3, 0},
        {C::UnorderedAccess, 0, 0, 0, 1, 158, 16, 0, 0},
        {C::UnorderedAccess, 1, 1, 0, kDxbcUnboundedRange, 157, 0, 0, 0},
        {C::ConstantBuffer, 0, 0, 0, 0, 89, 0, 0, 1},
        {C::ConstantBuffer, 1, 1, 1, 3, 89, 0, 0, 2},
    };
    ASSERT_EQ(c.binding_declarations.size(), std::size(want));
    for (usize i = 0; i < std::size(want); ++i) {
        const auto& d = c.binding_declarations[i];
        EXPECT_EQ(d.register_class, want[i].cls) << i;
        EXPECT_EQ(d.range_id, want[i].id) << i;
        EXPECT_EQ(d.space, want[i].space) << i;
        EXPECT_EQ(d.lower, want[i].lower) << i;
        EXPECT_EQ(d.upper, want[i].upper) << i;
        EXPECT_EQ(d.opcode, want[i].opcode) << i;
        EXPECT_EQ(d.stride, want[i].stride) << i;
        EXPECT_EQ(d.dimension, want[i].dimension) << i;
        if (want[i].cls == C::ConstantBuffer) EXPECT_EQ(d.cb_vectors, want[i].cb_vectors) << i;
    }
}

class DxilLowerCompute : public ::testing::TestWithParam<const char*> {};

TEST_P(DxilLowerCompute, TranslatesToSpirvAndMsl) {
    const DxbcContainer c = lowered(GetParam());
    ASSERT_TRUE(c.valid());
    DxbcTranslationOptions options{};
    // One run per declared range, each at its own 64-slot window, as a
    // descriptor-table translation needs for SM 5.1.
    u32 next[4] = {};
    for (const auto& d : c.binding_declarations) {
        const u32 set = d.register_class == DxbcRegisterClass::ConstantBuffer ? 0
                        : d.register_class == DxbcRegisterClass::ShaderResource ? 1
                        : d.register_class == DxbcRegisterClass::Sampler ? 2 : 3;
        options.register_remap.push_back({d.register_class, d.space, d.lower,
                                          d.unbounded() ? kDxbcUnboundedRange : d.count(), set,
                                          next[set]});
        next[set] += 64;
    }
    // The stage emulations the D3D12 frontend translates with
    // (d3d12_pipeline.cpp): a geometry shader and a hull shader become
    // compute kernels reading a record per input vertex / control point,
    // a domain shader a kernel per tessellated point.
    if (c.stage == ShaderStage::Geometry || c.stage == ShaderStage::Hull) {
        for (const auto& e : c.input_signature) {
            if (e.register_num != 0xFFFFFFFFu &&
                std::find(options.gs_input_record_registers.begin(), options.gs_input_record_registers.end(),
                          e.register_num) == options.gs_input_record_registers.end()) {
                options.gs_input_record_registers.push_back(e.register_num);
            }
        }
    }
    if (c.stage == ShaderStage::Geometry) options.emulate_geometry_with_compute = true;
    if (c.stage == ShaderStage::Hull || c.stage == ShaderStage::Domain) {
        options.emulate_tessellation = true;
        options.domain_to_compute = true;
    }
    // The view sizes bufinfo reads, as a descriptor-table frontend binds them
    // (d3d12_pipeline.cpp configure_descriptor_bindings).
    options.view_size_buffer.enabled = true;
    options.view_size_buffer.srv = options.view_size_buffer.uav = true;
    options.view_size_buffer.srv_binding = next[0]++;
    options.view_size_buffer.uav_binding = next[0]++;
    if (next[2] != 0) {
        // The sampler records a D3D12 descriptor-table translation reads
        // (d3d12_pipeline.cpp): MipLODBias, filter, LOD clamps.
        options.sampler_state_buffer.enabled = true;
        options.sampler_state_buffer.descriptor_set = 0;
        options.sampler_state_buffer.binding = next[0]++;
        options.border_twin_binding_base = next[2];
    }
    const auto spirv = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(spirv.valid()) << GetParam() << ": " << spirv.failure;
    MslCompileOptions msl{};
    msl.argument_buffers = true;
    const MslDescriptorClass classes[4] = {MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
                                           MslDescriptorClass::Sampler, MslDescriptorClass::Texture};
    for (u32 set = 0; set < 4; ++set) {
        msl.argument_buffer_sets[set] = {std::max(next[set], 1u), BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set, classes[set]};
    }
    if (spirv.uses_typed_uav_atomics) msl.msl_version = 30100u;
    // VITREO_DXIL_SPV=<name> writes that program's SPIR-V to <name>.spv
    // (diagnostic, for spirv-val / spirv-dis).
    if (const char* dump = std::getenv("VITREO_DXIL_SPV"); dump != nullptr && std::string(dump) == GetParam()) {
        std::ofstream(std::string(dump) + ".spv", std::ios::binary)
            .write(reinterpret_cast<const char*>(spirv.spirv.data()),
                   static_cast<std::streamsize>(spirv.spirv.size() * sizeof(spirv.spirv[0])));
    }
    const auto compiled = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                               spirv.stage, msl);
    EXPECT_TRUE(compiled.valid()) << GetParam();
    if (std::string(GetParam()).starts_with("ps_bary") && compiled.valid()) {
        // d3d12_bary.hlsl reads GetAttributeAtVertex(id, 0..2) and
        // GetAttributeAtVertex(attr, 2): ISG1 places ID (uint) at v1 and
        // ATTR at v2 (dxc 1.6 listing). Both registers are reported, with
        // three flat copies each at per_vertex_input_location.
        EXPECT_EQ(spirv.per_vertex_inputs, (1u << 1) | (1u << 2)) << GetParam();
        const std::string text = compiled.msl_text();
        for (u32 reg : {1u, 2u}) {
            for (u32 k = 0; k < 3; ++k) {
                const std::string loc = "[[user(locn" + std::to_string(per_vertex_input_location(reg, k));
                EXPECT_NE(text.find(loc), std::string::npos) << GetParam() << " lacks " << loc;
            }
        }
        EXPECT_NE(text.find("locn" + std::to_string(per_vertex_input_location(2, 0)) + "), flat]]"),
                  std::string::npos)
            << GetParam();
    } else {
        // d3d12_c4.hlsl ps_start reads GetAttributeAtVertex(ids, 0..2), IDS
        // at v1.
        EXPECT_EQ(spirv.per_vertex_inputs, std::string(GetParam()) == "ps_start" ? 1u << 1 : 0u) << GetParam();
    }
    // VITREO_DXIL_DUMP=<name> prints that program's MSL (diagnostic).
    if (const char* dump = std::getenv("VITREO_DXIL_DUMP");
        dump != nullptr && std::string(dump) == GetParam()) {
        std::printf("%s\n", compiled.msl_text());
    }
}

INSTANTIATE_TEST_SUITE_P(Corpus, DxilLowerCompute,
                         ::testing::Values("cs_exec", "cs_bias", "cs_alu", "cs_flow", "cs_res",
                                           "cs_alu_65", "vs_tri", "ps_tri", "vs_quad", "ps_tex",
                                           "cs_wave", "cs_int64", "cs_half", "cs_wavex", "cs_wsize16",
                                           "cs_int64x", "cs_halfx", "cs_res16",
                                           // d3d12_sm6_stages.hlsl
                                           "vs_gs", "gs_quads", "ps_data", "vs_cp", "hs_quad", "ds_quad",
                                           "ps_color", "vs_ops", "ps_ops", "cs_tex", "cs_math", "cs_bits",
                                           "cs_dot4", "cs_pack", "cs_three", "gs_dup", "hs_tri", "ds_tri",
                                           // d3d12_dxilc.hlsl
                                           "cs_double", "cs_double2", "cs_double16", "cs_dot2", "cs_wavedbl", "vs_pix",
                                           "ps_helper", "ps_quad", "vs_sig16", "ps_sig16",
                                           // d3d12_csderiv.hlsl (SM 6.6 compute derivatives)
                                           "cs_lin", "cs_grid", "cs_sample", "cs_sample_lin", "cs_cube",
                                           // d3d12_bary.hlsl (SM 6.1 barycentrics)
                                           "vs_bary", "ps_weights", "ps_weights_npc", "ps_bary", "ps_bary_np",
                                           "ps_bary_cen",
                                           // d3d12_c4.hlsl (dxc 1.9: SM 6.7 / 6.8 leftovers)
                                           "vs_prim", "vs_start", "ps_start", "ps_start_flat", "cs_offs", "cs_raw",
                                           // d3d12_sm68.hlsl (dxc 1.9: WaveSize, SampleCmpLevel / Grad / Bias)
                                           "cs_wsize32", "cs_wsize16_64", "cs_cmp",
                                           // d3d12_dxr.hlsl (RayQuery, SM 6.5)
                                           "cs_rq", "ps_rq"),
                         [](const auto& info) { return std::string(info.param); });

TEST(DxilLower, WaveSizeIsReadFromBothTags) {
    // kDxilWaveSizeTag 11 !{i32 N} (SM 6.6) and kDxilRangedWaveSizeTag 23
    // !{min, max, preferred} (SM 6.8), dxc 1.9 (d3d12_sm68.hlsl); the D3D12
    // frontend compares them with WaveLaneCountMin / Max.
    const struct { const char* name; u32 min, max; } want[] = {
        {"cs_wsize32", 32, 32}, {"cs_wsize64", 64, 64}, {"cs_wsize16", 16, 16},
        {"cs_wsize16_64", 16, 64}, {"cs_wsize64_128", 64, 128}, {"cs_wsize4_32_16", 4, 32},
        {"cs_cmp", 0, 0},
    };
    for (const auto& w : want) {
        const Sm6Blob& b = blob(w.name);
        const DxilToDxbcResult r = dxil_to_dxbc(b.code, b.size);
        ASSERT_TRUE(r.ok) << w.name << ": " << r.failure;
        EXPECT_EQ(r.wave_size_min, w.min) << w.name;
        EXPECT_EQ(r.wave_size_max, w.max) << w.name;
    }
}

TEST(DxilLower, ForwardMergeScopesDoNotIntroduceBackedges) {
    // cs_flow.dxil.txt: only labels 18 and 47 have backedges. dxc unrolled
    // the inner x loop. The early return, switch, multiple exits and phi
    // joins must not turn into extra loops in the lowered program. Native
    // FlowShaderMatchesWarp checks these paths against Windows for x86/x64.
    const DxbcContainer c = lowered("cs_flow");
    ASSERT_TRUE(c.valid());
    using dxbc_spirv_detail::Sm4Opcode;
    std::map<Sm4Opcode, u32> counts;
    for (usize at = 8; at + 4 <= c.bytecode.size();) {
        u32 token = 0;
        std::memcpy(&token, c.bytecode.data() + at, sizeof(token));
        const u32 length = (token >> 24) & 0x7Fu;
        ASSERT_NE(length, 0u);
        ASSERT_LE(at + usize{length} * 4u, c.bytecode.size());
        ++counts[static_cast<Sm4Opcode>(token & 0x7FFu)];
        at += usize{length} * 4u;
    }
    EXPECT_EQ(counts[Sm4Opcode::LOOP], 2u);
    EXPECT_EQ(counts[Sm4Opcode::ENDLOOP], 2u);
    EXPECT_GT(counts[Sm4Opcode::SWITCH], 0u);
    EXPECT_EQ(counts[Sm4Opcode::SWITCH], counts[Sm4Opcode::DEFAULT]);
    EXPECT_EQ(counts[Sm4Opcode::SWITCH], counts[Sm4Opcode::ENDSWITCH]);
}

TEST(DxilLower, ImmutableViewLimitsAreLoadedOnceInAStraightLineScope) {
    // Host-owned view sizes are immutable throughout an invocation. This
    // shader has many reads and writes, including per-component OOB paths;
    // a single dominating read of each metadata buffer suffices. Guest UAV
    // stores must not cause the compiler to reload this independent data.
    const auto lowered = dxil_to_dxbc(kPackedBytesNative16, sizeof(kPackedBytesNative16));
    ASSERT_TRUE(lowered.ok) << lowered.failure;
    const auto c = parse_dxbc(lowered.container.data(), lowered.container.size());
    ASSERT_TRUE(c.valid());
    DxbcTranslationOptions options{};
    options.register_remap = {{DxbcRegisterClass::ShaderResource, 0, 0, 1, 1, 0},
                              {DxbcRegisterClass::UnorderedAccess, 0, 0, 1, 3, 0}};
    options.view_size_buffer.enabled = options.view_size_buffer.srv = options.view_size_buffer.uav = true;
    options.view_size_buffer.srv_binding = 0;
    options.view_size_buffer.uav_binding = 1;
    const auto translated = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(translated.valid()) << translated.failure;
    using dxbc_spirv_detail::SpvOp;
    const auto loads = [&](const char* name) {
        const auto& words = translated.spirv;
        u32 root = 0;
        for (usize i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFFu) == static_cast<u32>(SpvOp::OpName) &&
                std::string(reinterpret_cast<const char*>(words.data() + i + 2)) == name)
                root = words[i + 1];
        }
        EXPECT_NE(root, 0u) << name;
        std::set<u32> pointers;
        for (usize i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFFu) == static_cast<u32>(SpvOp::OpAccessChain) && words[i + 3] == root)
                pointers.insert(words[i + 2]);
        }
        u32 count = 0;
        for (usize i = 5; i < words.size(); i += words[i] >> 16) {
            if ((words[i] & 0xFFFFu) == static_cast<u32>(SpvOp::OpLoad) && pointers.contains(words[i + 3])) ++count;
        }
        return count;
    };
    EXPECT_EQ(loads("vitreo_srv_view_sizes"), 1u);
    EXPECT_EQ(loads("vitreo_uav_view_sizes"), 1u);
    MslCompileOptions msl{};
    msl.argument_buffers = true;
    msl.argument_buffer_sets[0] = {2, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE, MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[1] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1, MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[3] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3, MslDescriptorClass::Buffer};
    const auto compiled = compile_spirv_to_msl(translated.spirv.data(), translated.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid());
    const std::string text(compiled.msl_text());
    const auto mentions = [&](const char* name) {
        u32 count = 0;
        for (usize at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) ++count;
        return count;
    };
    // The argument-buffer member declaration and exactly one native read.
    // SPIRV-Cross must not inline the cached load into each expression.
    EXPECT_EQ(mentions("vitreo_srv_view_sizes"), 2u) << text;
    EXPECT_EQ(mentions("vitreo_uav_view_sizes"), 2u) << text;
}

TEST(DxilLower, StatusLoadsAndSamplesLowerToTheStatusForms) {
    using dxbc_spirv_detail::Sm4Opcode;
    // A ResRet status (member 4) that the shader extracts turns the
    // resource op into its tiled-resources _s / _cl_s form, with the
    // status destination after the destination (d3d12_dxilc cs_status /
    // ps_status, dxc 1.6). Sample, SampleBias, SampleGrad and SampleCmp
    // with a status become the _cl_s forms; CheckAccessFullyMapped reads it.
    for (const auto& [name, expected] : std::vector<std::pair<const char*, std::vector<Sm4Opcode>>>{
             {"cs_status", {Sm4Opcode::LD_RAW_S, Sm4Opcode::LD_STRUCTURED_S, Sm4Opcode::LD_S,
                            Sm4Opcode::LD_UAV_TYPED_S, Sm4Opcode::CHECK_ACCESS_FULLY_MAPPED}},
             {"ps_status", {Sm4Opcode::SAMPLE_CL_S, Sm4Opcode::SAMPLE_B_CL_S, Sm4Opcode::SAMPLE_L_S,
                            Sm4Opcode::SAMPLE_D_CL_S, Sm4Opcode::SAMPLE_C_CL_S, Sm4Opcode::SAMPLE_C_LZ_S,
                            Sm4Opcode::GATHER4_S, Sm4Opcode::GATHER4_C_S,
                            Sm4Opcode::CHECK_ACCESS_FULLY_MAPPED}}}) {
        const Sm6Blob& b = blob(name);
        const DxilToDxbcResult r = dxil_to_dxbc(b.code, b.size);
        ASSERT_TRUE(r.ok) << name << ": " << r.failure;
        const DxbcContainer c = parse_dxbc(r.container.data(), r.container.size());
        ASSERT_TRUE(c.valid()) << name;
        std::set<u32> seen;
        std::vector<u32> code(c.bytecode.size() / 4);
        std::memcpy(code.data(), c.bytecode.data(), code.size() * 4);
        for (usize pos = 2; pos < code.size();) {
            const u32 length = (code[pos] >> 24) & 0x7Fu;
            if (length == 0) break;
            seen.insert(code[pos] & 0x7FFu);
            pos += length;
        }
        for (Sm4Opcode o : expected) {
            EXPECT_TRUE(seen.contains(static_cast<u32>(o))) << name << " lacks opcode " << static_cast<u32>(o);
        }
    }
}

TEST(DxilLower, LodClampsLowerToTheClampedStatusForms) {
    // ps_clamp: Sample / SampleBias / SampleGrad with a clamp become
    // sample_cl_s (230), sample_b_cl_s (231) and sample_d_cl_s (232), the
    // tiled-resources forms fxc measured (dxbc_to_spirv_sm4.hpp), with a
    // null status; no plain sample remains.
    const DxbcContainer c = lowered("ps_clamp");
    ASSERT_TRUE(c.valid());
    u32 counts[256] = {};
    const auto& code = c.bytecode;
    for (usize at = 8; at + 4 <= code.size();) {
        u32 token = 0;
        std::memcpy(&token, code.data() + at, 4);
        const u32 length = (token >> 24) & 0x7Fu;
        if (length == 0) break;
        ++counts[token & 0xFFu];
        at += usize{length} * 4u;
    }
    EXPECT_EQ(counts[230], 2u);
    EXPECT_EQ(counts[231], 1u);
    EXPECT_EQ(counts[232], 1u);
    EXPECT_EQ(counts[static_cast<u32>(dxbc_spirv_detail::Sm4Opcode::SAMPLE)], 0u);
}

TEST(DxilLower, HeapIndexedResourcesLowerToTheHeapSpaces) {
    // Shader model 6.6 dynamic resources (CreateHandleFromHeap +
    // AnnotateHandle) lower to unbounded ranges of the heap register spaces
    // (graphics::kDxbcResourceHeapSpace / kDxbcSamplerHeapSpace), which the
    // container's binding declarations report.
    const Sm6Blob& b = blob("cs_dyn");
    const DxilToDxbcResult r = dxil_to_dxbc(b.code, b.size);
    ASSERT_TRUE(r.ok) << r.failure;
    const graphics::DxbcContainer container = graphics::parse_dxbc(r.container.data(), r.container.size());
    ASSERT_FALSE(container.binding_declarations.empty());
    bool heap = false;
    for (const auto& d : container.binding_declarations) {
        if (d.space == graphics::kDxbcResourceHeapSpace || d.space == graphics::kDxbcSamplerHeapSpace) {
            heap = true;
            EXPECT_TRUE(d.unbounded());
        }
    }
    EXPECT_TRUE(heap);
}

/// The extension instructions of a lowered program (dxbc_vitreo_ext.hpp):
/// how many VX_WAVE / VX_I64 / VX_F16 instructions its SHEX holds.
std::array<u32, 3> extension_counts(const char* name) {
    const Sm6Blob& b = blob(name);
    const DxilToDxbcResult r = dxil_to_dxbc(b.code, b.size);
    std::array<u32, 3> n{};
    if (!r.ok) return n;
    const DxbcContainer c = parse_dxbc(r.container.data(), r.container.size());
    const auto& code = c.bytecode;
    const usize words = code.size() / 4;
    for (usize at = 2; at < words;) {
        u32 token = 0;
        std::memcpy(&token, code.data() + at * 4, 4);
        const u32 opcode = token & 0x7FFu, length = (token >> 24) & 0x7Fu;
        if (opcode >= 0x700 && opcode <= 0x702) ++n[opcode - 0x700];
        if (length == 0) break;
        at += length;
    }
    return n;
}

TEST(DxilLower, WaveAndWideOpsBecomeExtensionInstructions) {
    // cs_wave: 13 wave intrinsics (cs_wave.dxil.txt), each one VX_WAVE.
    EXPECT_EQ(extension_counts("cs_wave")[0], 13u);
    // cs_int64: i64 arithmetic is VX_I64, no wave or half instruction.
    const auto i64 = extension_counts("cs_int64");
    EXPECT_GT(i64[1], 10u);
    EXPECT_EQ(i64[0] + i64[2], 0u);
    // cs_half: the half multiply, divide and add each round (VX_F16), as do
    // the two fptruncs and the bitcast to i16.
    EXPECT_GE(extension_counts("cs_half")[2], 6u);
}

// SV_ViewID (DirectX-Specs "View Instancing"; DXIL ViewID, opcode 138):
// dxc 1.6 blobs from winoracle-gpu (tools/oracle_probe/d3d12_vinst). The
// lowering emits VX_VIEWID (0x704) once per read; each translation is for
// one view (DxbcTranslationOptions::view_id, the D3D12 frontend's
// draw-level looping), so views 0 and 3 give different, valid programs.
TEST(DxilLower, ViewIdBecomesTheTranslationsViewInstance) {
    struct Case {
        const char* name;
        const unsigned char* code;
        usize size;
    };
    const Case cases[] = {{"vs_view", k_vs_view, sizeof(k_vs_view)},
                          {"gs_view", k_gs_view, sizeof(k_gs_view)},
                          {"ps_view", k_ps_view, sizeof(k_ps_view)}};
    for (const Case& k : cases) {
        const DxilToDxbcResult r = dxil_to_dxbc(k.code, k.size);
        ASSERT_TRUE(r.ok) << k.name << ": " << r.failure;
        const DxbcContainer c = parse_dxbc(r.container.data(), r.container.size());
        ASSERT_TRUE(c.valid()) << k.name;
        u32 view_ids = 0;
        const usize words = c.bytecode.size() / 4;
        for (usize at = 2; at < words;) {
            u32 token = 0;
            std::memcpy(&token, c.bytecode.data() + at * 4, 4);
            const u32 length = (token >> 24) & 0x7Fu;
            if ((token & 0x7FFu) == 0x704u) ++view_ids;
            if ((token & 0x7ffu) == 53u) {
                u32 block_length = 0;
                ASSERT_LT(at + 1u, words);
                std::memcpy(&block_length, c.bytecode.data() + (at + 1u) * 4, 4);
                ASSERT_GE(block_length, 2u);
                at += block_length;
            } else {
                if (length == 0) break;
                at += length;
            }
        }
        EXPECT_EQ(view_ids, 1u) << k.name;
        DxbcTranslationOptions options{};
        u32 next[4] = {};
        for (const auto& d : c.binding_declarations) {
            const u32 set = d.register_class == DxbcRegisterClass::ConstantBuffer ? 0
                            : d.register_class == DxbcRegisterClass::ShaderResource ? 1
                            : d.register_class == DxbcRegisterClass::Sampler ? 2 : 3;
            options.register_remap.push_back({d.register_class, d.space, d.lower, d.count(), set, next[set]});
            next[set] += 64;
        }
        if (c.stage == ShaderStage::Geometry) {
            options.emulate_geometry_with_compute = true;
            for (const auto& e : c.input_signature) {
                if (e.register_num != 0xFFFFFFFFu &&
                    std::find(options.gs_input_record_registers.begin(),
                              options.gs_input_record_registers.end(),
                              e.register_num) == options.gs_input_record_registers.end()) {
                    options.gs_input_record_registers.push_back(e.register_num);
                }
            }
        }
        options.view_id = 0;
        const auto view0 = translate_dxbc_to_spirv(c, options);
        options.view_id = 3;
        const auto view3 = translate_dxbc_to_spirv(c, options);
        ASSERT_TRUE(view0.valid()) << k.name << ": " << view0.failure;
        ASSERT_TRUE(view3.valid()) << k.name << ": " << view3.failure;
        EXPECT_NE(view0.spirv, view3.spirv) << k.name;
    }
}

// The d3d12_a47 "pipeline" state object (d3d12_dxr.hlsl LIB): one compute
// megakernel that the translator and SPIRV-Cross accept.
TEST(DxilLower, RaytracingPipelineBecomesOneKernel) {
    const Sm6Blob& b = blob("lib");
    ASSERT_NE(b.code, nullptr);
    DxilRtProgramDesc d;
    d.libraries.push_back({b.code, b.size, {}});
    d.hit_groups.push_back({"hg_tri", false, "ah_tri", "ch_tri", ""});
    d.hit_groups.push_back({"hg_sphere", true, "", "ch_sphere", "is_sphere"});
    d.max_recursion = 2;
    d.max_payload_bytes = 20;
    d.max_attribute_bytes = 12;
    using Root = DxilRtProgramDesc::Root;
    Root local;
    local.params.push_back({Root::Constants, 0, 1, 4, 0, {}});
    for (const char* owner : {"ms", "ms_shadow", "hg_tri", "hg_sphere", "cl"}) d.local_roots[owner] = local;
    // The probe's global root signature: SRV t0, UAV u0, b0 x4.
    Root global;
    global.params.push_back({Root::SRV, 0, 0, 0, 0, {}});
    global.params.push_back({Root::UAV, 0, 0, 0, 0, {}});
    global.params.push_back({Root::Constants, 0, 0, 4, 0, {}});
    // Without it the shaders' resources are unbound (d3d12_a48 local_rules:
    // E_INVALIDARG).
    EXPECT_FALSE(dxil_raytracing_validate(d).ok);
    for (const char* e : {"rg", "ms", "ms_shadow", "ch_tri", "ah_tri", "is_sphere", "ch_sphere", "cl"}) {
        d.global_roots[e] = global;
    }
    EXPECT_TRUE(dxil_raytracing_validate(d).ok) << dxil_raytracing_validate(d).failure;
    // A global root that binds b1 as well: bound twice for ms.
    Root twice = global;
    twice.params.push_back({Root::Constants, 0, 1, 4, 0, {}});
    DxilRtProgramDesc both = d;
    both.global_roots["ms"] = twice;
    EXPECT_FALSE(dxil_raytracing_validate(both).ok);
    const DxilRtProgram p = dxil_raytracing_to_dxbc(d);
    ASSERT_TRUE(p.ok) << p.failure;
    EXPECT_EQ(p.indices.size(), 6u);
    const DxbcContainer c = parse_dxbc(p.container.data(), p.container.size());
    ASSERT_TRUE(c.valid());
    DxbcTranslationOptions options{};
    u32 next[4] = {};
    for (const auto& decl : c.binding_declarations) {
        const u32 set = decl.register_class == DxbcRegisterClass::ConstantBuffer ? 0
                        : decl.register_class == DxbcRegisterClass::ShaderResource ? 1
                        : decl.register_class == DxbcRegisterClass::Sampler ? 2 : 3;
        options.register_remap.push_back({decl.register_class, decl.space, decl.lower, decl.count(), set, next[set]});
        next[set] += 1;
    }
    options.view_size_buffer.enabled = true;
    options.view_size_buffer.srv = options.view_size_buffer.uav = true;
    options.view_size_buffer.srv_binding = next[0]++;
    options.view_size_buffer.uav_binding = next[0]++;
    const auto spirv = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions msl{};
    msl.argument_buffers = true;
    const MslDescriptorClass classes[4] = {MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
                                           MslDescriptorClass::Sampler, MslDescriptorClass::Texture};
    for (u32 set = 0; set < 4; ++set) {
        msl.argument_buffer_sets[set] = {std::max(next[set], 1u), BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set, classes[set]};
    }
    const auto compiled = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(), spirv.stage, msl);
    EXPECT_TRUE(compiled.valid());
    if (std::getenv("VITREO_DXIL_DUMP") != nullptr && std::string(std::getenv("VITREO_DXIL_DUMP")) == "lib") {
        std::printf("%s\n", compiled.msl_text());
    }
}

// The subobjects of d3d12_dxr_local.hlsl SUB, as dxc's -Fc listing
// (tests/oracle_data/d3d12_dxr_local/sub.dxil.txt, "Subobjects:") prints them.
TEST(DxilLower, LibrarySubobjectsFromRdat) {
    std::vector<DxilSubobject> s;
    ASSERT_TRUE(dxil_library_subobjects(dxrl_sub, sizeof(dxrl_sub), s));
    ASSERT_EQ(s.size(), 10u);
    const auto find = [&](const char* name) -> const DxilSubobject* {
        for (const auto& x : s) {
            if (x.name == name) return &x;
        }
        return nullptr;
    };
    const DxilSubobject* g = find("sub_global");
    ASSERT_NE(g, nullptr);
    EXPECT_EQ(g->kind, 1u);
    EXPECT_EQ(g->root_signature.size(), 96u);
    ASSERT_NE(find("sub_local"), nullptr);
    EXPECT_EQ(find("sub_local")->kind, 2u);
    EXPECT_EQ(find("sub_local")->root_signature.size(), 48u);
    EXPECT_EQ(find("sub_local_shift")->root_signature.size(), 72u);
    const DxilSubobject* sc = find("sub_shader_config");
    ASSERT_NE(sc, nullptr);
    EXPECT_EQ(sc->kind, 9u);
    EXPECT_EQ(sc->max_payload, 16u);
    EXPECT_EQ(sc->max_attributes, 8u);
    EXPECT_EQ(find("sub_pipeline_config")->kind, 10u);
    EXPECT_EQ(find("sub_pipeline_config")->max_recursion, 1u);
    EXPECT_EQ(find("sub_config")->kind, 0u);
    EXPECT_EQ(find("sub_config")->flags, 4u);  // STATE_OBJECT_FLAG_ALLOW_STATE_OBJECT_ADDITIONS
    const DxilSubobject* box = find("hg_box");
    ASSERT_NE(box, nullptr);
    EXPECT_EQ(box->kind, 11u);
    EXPECT_EQ(box->hit_group_type, 1u);
    EXPECT_EQ(box->any_hit, "");
    EXPECT_EQ(box->closest_hit, "ch_box");
    EXPECT_EQ(box->intersection, "is_box");
    EXPECT_EQ(find("hg_tri")->hit_group_type, 0u);
    EXPECT_EQ(find("hg_tri")->closest_hit, "ch_tri");
    const DxilSubobject* a = find("sub_assoc");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->kind, 8u);
    EXPECT_EQ(a->subobject, "sub_local");
    EXPECT_EQ(a->exports, (std::vector<std::string>{"ms", "hg_tri", "hg_box"}));
    EXPECT_EQ(find("sub_assoc_cl")->exports, (std::vector<std::string>{"cl"}));
    // A library without subobjects has none.
    std::vector<DxilSubobject> none;
    ASSERT_TRUE(dxil_library_subobjects(dxrl_loc, sizeof(dxrl_loc), none));
    EXPECT_TRUE(none.empty());
}

}  // namespace
// WriteSamplerFeedback / Bias / Level / Grad (DXIL 174-177, SM 6.5): dxc
// 1.6 blobs of tools/oracle_probe/d3d12_vinst/d3d12_feedback.hlsl from
// winoracle-gpu. Each write reads its sampler's state through
// VX_SAMPLER_STATE (0x7F0) and sets bits with atomic_or (opcode 170) in the
// map, declared dcl_uav_raw (157); the translation needs the descriptor
// table's sampler_state_buffer and refuses without one.
TEST(DxilLower, SamplerFeedbackWritesBecomeAtomicOrIntoTheMap) {
    struct Case {
        const char* name;
        const unsigned char* code;
        usize size;
        u32 writes;
    };
    const Case cases[] = {{"cs_level", k_cs_level, sizeof(k_cs_level), 2},
                          {"cs_grad", k_cs_grad, sizeof(k_cs_grad), 3},
                          {"cs_array", k_cs_array, sizeof(k_cs_array), 1},
                          {"ps_feedback", k_ps_feedback, sizeof(k_ps_feedback), 5}};
    for (const Case& k : cases) {
        const DxilToDxbcResult r = dxil_to_dxbc(k.code, k.size);
        ASSERT_TRUE(r.ok) << k.name << ": " << r.failure;
        const DxbcContainer c = parse_dxbc(r.container.data(), r.container.size());
        ASSERT_TRUE(c.valid()) << k.name;
        u32 state_reads = 0, atomics = 0, raw_uavs = 0;
        const usize words = c.bytecode.size() / 4;
        for (usize at = 2; at < words;) {
            u32 token = 0;
            std::memcpy(&token, c.bytecode.data() + at * 4, 4);
            const u32 opcode = token & 0x7FFu;
            const u32 length = (token >> 24) & 0x7Fu;
            if (opcode == 0x7F0u) ++state_reads;
            if (opcode == 170u) ++atomics;
            if (opcode == 157u) ++raw_uavs;
            if ((token & 0x7ffu) == 53u) {
                u32 block_length = 0;
                ASSERT_LT(at + 1u, words);
                std::memcpy(&block_length, c.bytecode.data() + (at + 1u) * 4, 4);
                ASSERT_GE(block_length, 2u);
                at += block_length;
            } else {
                if (length == 0) break;
                at += length;
            }
        }
        // Five sampler words and four atomics (one per bilinear corner) per write.
        EXPECT_EQ(state_reads, 5u * k.writes) << k.name;
        EXPECT_EQ(atomics, 4u * k.writes) << k.name;
        EXPECT_GE(raw_uavs, 1u) << k.name;
        DxbcTranslationOptions options{};
        u32 next[4] = {};
        for (const auto& d : c.binding_declarations) {
            const u32 set = d.register_class == DxbcRegisterClass::ConstantBuffer ? 0
                            : d.register_class == DxbcRegisterClass::ShaderResource ? 1
                            : d.register_class == DxbcRegisterClass::Sampler ? 2 : 3;
            options.register_remap.push_back({d.register_class, d.space, d.lower, d.count(), set, next[set]});
            next[set] += 64;
        }
        const auto refused = translate_dxbc_to_spirv(c, options);
        EXPECT_FALSE(refused.valid()) << k.name;
        options.sampler_state_buffer = {true, 0, next[0]};
        const auto spirv = translate_dxbc_to_spirv(c, options);
        EXPECT_TRUE(spirv.valid()) << k.name << ": " << spirv.failure;
    }
}


TEST(DxilLower, ReadonlyGlobalWordsUseOneImmutableNestedTableWithoutCopies) {
    using namespace dxil_constant_test;
    using namespace dxbc_spirv_detail;
    for (u32 count : {4u, 4097u, 65533u}) {
        std::vector<u32> words(count);
        for (u32 i = 0; i < count; ++i) words[i] = 0x7fa12345u ^ i;
        const auto c = readonly_words_fixture(words);
        const auto result = translate_dxbc_to_spirv(c);
        ASSERT_TRUE(result.valid()) << result.failure;
        u32 initialized = 0;
        for (usize at = 5; at < result.spirv.size();) {
            const u32 size = result.spirv[at] >> 16;
            const auto op = static_cast<SpvOp>(result.spirv[at] & 0xffffu);
            ASSERT_GT(size, 0u);
            ASSERT_LE(at + size, result.spirv.size());
            if (op == SpvOp::OpConstantComposite) EXPECT_LE(size, 1027u);
            if (op == SpvOp::OpVariable && size == 5u &&
                result.spirv[at + 3] == static_cast<u32>(SpvStorageClass::Private)) ++initialized;
            at += size;
        }
        EXPECT_EQ(initialized, 1u);
        const auto msl = compile_spirv_to_msl(result.spirv.data(), result.spirv.size(), ShaderStage::Fragment);
        ASSERT_TRUE(msl.valid());
        const std::string text = msl.msl_text();
        EXPECT_EQ(text.find("_array_copy"), std::string::npos);
        EXPECT_NE(text.find("constant uint"), std::string::npos);
        EXPECT_EQ(text.find("thread uint dxil_const"), std::string::npos);
    }
}

TEST(DxilLower, ReadonlyGlobalMetadataRejectsMalformedDeclarationsAndWrites) {
    using namespace dxil_constant_test;
    const u32 token = 53u | (dxbc_vitreo_ext::kReadonlyIndexableDataClass << 11);
    for (const auto& extra : std::vector<std::vector<u32>>{
            {token, 4u, kReadonlySlot, 1u}, // duplicate initializer
            {104u | (2u << 24), 1u, 54u | (6u << 24),
             0x00203012u, kReadonlySlot, 0u, 0x00004001u, 1u}, // write
            {105u | (4u << 24), kReadonlySlot, 4u, 1u}}) { // mutable alias
        auto c = readonly_words_fixture({1u, 2u, 3u, 4u});
        insert_before_body(c, extra, 4u);
        const auto result = translate_dxbc_to_spirv(c);
        EXPECT_FALSE(result.valid());
        EXPECT_NE(result.failure.find("readonly"), std::string::npos) << result.failure;
    }
    for (u32 length : {2u, 3u, 10u}) {
        auto c = readonly_words_fixture({1u, 2u, 3u, 4u});
        c.bytecode.resize(5u * sizeof(u32));
        std::memcpy(c.bytecode.data() + 3u * sizeof(u32), &length, sizeof(length));
        const auto result = translate_dxbc_to_spirv(c);
        EXPECT_FALSE(result.valid());
        EXPECT_EQ(result.failure, "invalid readonly global initializer");
    }
}


TEST(DxilLower, DxcReadonlyGlobalsPreserveWordLayoutAndShareAcrossHullPhases) {
    using namespace dxbc_spirv_detail;
    for (const auto& fixture : dxil_readonly_fixture::blobs) {
        SCOPED_TRACE(fixture.name);
        const auto lower = dxil_to_dxbc(fixture.data, fixture.size);
        ASSERT_TRUE(lower.ok) << lower.failure;
        const auto c = parse_dxbc(lower.container.data(), lower.container.size());
        ASSERT_TRUE(c.valid());
        std::map<u32, std::vector<u32>> initializers;
        std::string failure;
        (void)decode_immediate_constant_buffer(c.bytecode, &failure, &initializers);
        ASSERT_TRUE(failure.empty()) << failure;
        const std::string name = fixture.name;
        EXPECT_EQ(initializers.size(), name == "psnested" ? 1u : 2u);
        std::vector<std::vector<u32>> expected;
        if (name == "ps32") expected = {{0x7fa12345u, 0x80000000u, 0xffffffffu, 0x12345678u},
                                       {0xdeadbeefu, 0x7fc54321u, 0x80000001u, 42u}};
        if (name == "ps16") expected = {{0, 65535, 32768, 4660},
                                       {0, 0x80000000u, 0x3fc00000u, 0xc0000000u}};
        if (name == "ps64") expected = {{0x89abcdefu, 0x01234567u, 0xffffffffu, 0xffffffffu,
                                       0, 0x80000000u, 42, 0},
                                      {0, 0, 0, 0x80000000u, 0, 0x3ff80000u, 0, 0xc0020000u}};
        if (name == "psnested") expected = {{1, 11, 2, 22, 3, 33, 4, 44, 5, 55, 6, 66}};
        if (name == "hs") expected = {{3, 5, 7, 11}, {13, 17, 19, 23}};
        for (const auto& wanted : expected) {
            EXPECT_TRUE(std::any_of(initializers.begin(), initializers.end(),
                [&](const auto& pair) { return pair.second == wanted; }));
        }
        DxbcTranslationOptions options{};
        if (c.stage == ShaderStage::Hull) {
            options.emulate_tessellation = true;
            options.domain_to_compute = true;
            for (const auto& e : c.input_signature) {
                if (e.register_num != 0xffffffffu &&
                    std::find(options.gs_input_record_registers.begin(), options.gs_input_record_registers.end(),
                              e.register_num) == options.gs_input_record_registers.end())
                    options.gs_input_record_registers.push_back(e.register_num);
            }
        }
        const auto result = translate_dxbc_to_spirv(c, options);
        ASSERT_TRUE(result.valid()) << result.failure;
        const auto msl = compile_spirv_to_msl(result.spirv.data(), result.spirv.size(),
            c.stage == ShaderStage::Hull ? ShaderStage::Compute : ShaderStage::Fragment);
        ASSERT_TRUE(msl.valid());
        const std::string text = msl.msl_text();
        EXPECT_EQ(text.find("_array_copy"), std::string::npos);
        EXPECT_NE(text.find("constant uint"), std::string::npos);
    }
}

}  // namespace vitreo::graphics

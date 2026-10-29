// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_dxbc_sm51.cpp
/// @brief Shader model 5.1 (D3D12): binding declarations, the register
/// remap, and descriptor arrays in the translated SPIR-V.
///
/// Every blob is fxc 10.1 output from Windows
/// (tools/oracle_probe/d3d12_sm51/gen_d3d12_sm51.py), and every expected
/// range, space, dimension and flag below is read off that compiler's own
/// /Fc listing checked in beside it (cs_arrays.asm, cs_dims.asm, ...).

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"

#include "../oracle_data/d3d12_sm51/d3d12_sm51_shaders.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace vitreo;
using namespace vitreo::graphics;

namespace {

DxbcContainer parse_blob(const unsigned char* code, unsigned size) {
    return parse_dxbc(code, size);
}

const DxbcBindingDeclaration* find_declaration(const DxbcContainer& c,
                                               DxbcRegisterClass cls,
                                               u32 range_id) {
    for (const auto& d : c.binding_declarations) {
        if (d.register_class == cls && d.range_id == range_id) return &d;
    }
    return nullptr;
}

/// SPIR-V: variable id -> (DescriptorSet, Binding), from OpDecorate.
std::map<u32, std::pair<u32, u32>> descriptor_bindings(
    const std::vector<u32>& spirv) {
    std::map<u32, std::pair<u32, u32>> out;
    for (usize i = 5; i < spirv.size();) {
        const u32 words = spirv[i] >> 16;
        const u32 op = spirv[i] & 0xFFFFu;
        if (words == 0) break;
        if (op == 71u && words >= 4) {  // OpDecorate
            if (spirv[i + 2] == 34u) out[spirv[i + 1]].first = spirv[i + 3];
            if (spirv[i + 2] == 33u) out[spirv[i + 1]].second = spirv[i + 3];
        }
        i += words;
    }
    return out;
}

bool has_capability(const std::vector<u32>& spirv, u32 capability) {
    for (usize i = 5; i < spirv.size();) {
        const u32 words = spirv[i] >> 16;
        if (words == 0) break;
        if ((spirv[i] & 0xFFFFu) == 17u && spirv[i + 1] == capability) {
            return true;
        }
        i += words;
    }
    return false;
}

usize count_opcode(const std::vector<u32>& spirv, u32 opcode) {
    usize n = 0;
    for (usize i = 5; i < spirv.size();) {
        const u32 words = spirv[i] >> 16;
        if (words == 0) break;
        if ((spirv[i] & 0xFFFFu) == opcode) ++n;
        i += words;
    }
    return n;
}

bool has_decoration(const std::vector<u32>& spirv, u32 decoration) {
    for (usize i = 5; i < spirv.size();) {
        const u32 words = spirv[i] >> 16;
        if (words == 0) break;
        if ((spirv[i] & 0xFFFFu) == 71u && spirv[i + 2] == decoration) {
            return true;
        }
        i += words;
    }
    return false;
}

u32 class_set(DxbcRegisterClass cls) {
    switch (cls) {
    case DxbcRegisterClass::ConstantBuffer: return 0;
    case DxbcRegisterClass::ShaderResource: return 1;
    case DxbcRegisterClass::Sampler: return 2;
    case DxbcRegisterClass::UnorderedAccess: return 3;
    }
    return 0;
}

}  // namespace

/// The remap a D3D12 frontend with one table per class would build: every
/// (class, space) the program declares gets `stride` bindings of its class's
/// set in space order, the last of each class running to the end of the
/// table (an unbounded run).
static std::vector<DxbcRegisterRemap> packed_remap(const DxbcContainer& c,
                                            u32 stride = 32) {
    std::map<DxbcRegisterClass, std::set<u32>> spaces;
    for (const auto& d : c.binding_declarations) {
        spaces[d.register_class].insert(d.space);
    }
    std::vector<DxbcRegisterRemap> remap;
    for (const auto& [cls, list] : spaces) {
        u32 k = 0;
        for (const u32 space : list) {
            const bool last = k + 1 == list.size();
            remap.push_back({cls, space, 0u,
                             last ? kDxbcUnboundedRange : stride,
                             class_set(cls), k * stride});
            ++k;
        }
    }
    return remap;
}

static DxbcTranslationOptions sm51_options(const DxbcContainer& c) {
    DxbcTranslationOptions options{};
    options.register_remap = packed_remap(c);
    // As the D3D11 frontend lowers the stages Metal lacks.
    options.emulate_geometry_with_compute = c.stage == ShaderStage::Geometry;
    options.emulate_tessellation =
        c.stage == ShaderStage::Hull || c.stage == ShaderStage::Domain;
    options.domain_to_compute = c.stage == ShaderStage::Domain;
    return options;
}

// cs_arrays.asm:
//   dcl_constantbuffer CB0[3:3][2], immediateIndexed, space=0
//   dcl_constantbuffer CB1[1:4][2], dynamicIndexed, space=1
//   dcl_constantbuffer CB2[0:*][1], dynamicIndexed, space=2
//   dcl_sampler S0[1:2], mode_default, space=0
//   dcl_sampler S1[0:*], mode_default, space=4
//   dcl_resource_texture2d (float,float,float,float) T0[2:9], space=0
//   dcl_resource_structured T1[0:2], 20, space=1
//   dcl_resource_raw T2[4:5], space=1
//   dcl_resource_buffer (float,float,float,float) T3[6:7], space=1
//   dcl_resource_texture2d (float,float,float,float) T4[10:*], space=3
//   dcl_uav_typed_texture2d (float,float,float,float) U0[0:1], space=0
//   dcl_uav_raw U1[2:*], space=5
//   dcl_uav_structured U2[1:1], 20, space=6
TEST(DxbcSm51Test, DeclarationsReflectRangesSpacesAndShapes) {
    const auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    ASSERT_TRUE(c.valid());
    EXPECT_TRUE(c.is_sm51_plus());
    ASSERT_EQ(c.binding_declarations.size(), 13u);
    using C = DxbcRegisterClass;
    struct Want {
        C cls; u32 id, space, lower, upper, opcode;
    };
    const Want want[] = {
        {C::ConstantBuffer, 0, 0, 3, 3, 89},
        {C::ConstantBuffer, 1, 1, 1, 4, 89},
        {C::ConstantBuffer, 2, 2, 0, kDxbcUnboundedRange, 89},
        {C::Sampler, 0, 0, 1, 2, 90},
        {C::Sampler, 1, 4, 0, kDxbcUnboundedRange, 90},
        {C::ShaderResource, 0, 0, 2, 9, 88},
        {C::ShaderResource, 1, 1, 0, 2, 162},
        {C::ShaderResource, 2, 1, 4, 5, 161},
        {C::ShaderResource, 3, 1, 6, 7, 88},
        {C::ShaderResource, 4, 3, 10, kDxbcUnboundedRange, 88},
        {C::UnorderedAccess, 0, 0, 0, 1, 156},
        {C::UnorderedAccess, 1, 5, 2, kDxbcUnboundedRange, 157},
        {C::UnorderedAccess, 2, 6, 1, 1, 158},
    };
    for (const Want& w : want) {
        const auto* d = find_declaration(c, w.cls, w.id);
        ASSERT_NE(d, nullptr) << "range " << w.id;
        EXPECT_EQ(d->space, w.space) << "range " << w.id;
        EXPECT_EQ(d->lower, w.lower) << "range " << w.id;
        EXPECT_EQ(d->upper, w.upper) << "range " << w.id;
        EXPECT_EQ(d->opcode, w.opcode) << "range " << w.id;
    }
    const auto* cb0 = find_declaration(c, C::ConstantBuffer, 0);
    EXPECT_EQ(cb0->cb_vectors, 2u);
    EXPECT_FALSE(cb0->dynamically_indexed);
    EXPECT_EQ(cb0->name, "Plain");
    const auto* cb1 = find_declaration(c, C::ConstantBuffer, 1);
    EXPECT_TRUE(cb1->dynamically_indexed);
    EXPECT_EQ(cb1->count(), 4u);
    EXPECT_TRUE(cb1->is_array());
    EXPECT_EQ(cb1->name, "cbs");
    const auto* t0 = find_declaration(c, C::ShaderResource, 0);
    EXPECT_EQ(t0->dimension, 3u);  // texture2d
    EXPECT_EQ(t0->return_type, 0x5555u);
    EXPECT_EQ(t0->name, "texs");
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 1)->stride, 20u);
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 3)->dimension, 1u);
    EXPECT_TRUE(find_declaration(c, C::ShaderResource, 4)->unbounded());
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 4)->count(),
              kDxbcUnboundedRange);
    EXPECT_EQ(find_declaration(c, C::UnorderedAccess, 2)->stride, 20u);
    EXPECT_FALSE(find_declaration(c, C::UnorderedAccess, 2)->is_array());

    // The 5.1 RDEF record (40 bytes: + Space, uID), listing "Resource
    // Bindings": texu t10,space3 unbounded -> count 0; cbu cb0,space2 ->
    // 4294967295.
    bool texu = false, cbu = false;
    for (const auto& rb : c.resource_bindings) {
        if (rb.name == "texu") {
            texu = true;
            EXPECT_EQ(rb.bind_point, 10u);
            EXPECT_EQ(rb.bind_count, 0u);
            EXPECT_EQ(rb.space, 3u);
            EXPECT_EQ(rb.range_id, 4u);
        }
        if (rb.name == "cbu") {
            cbu = true;
            EXPECT_EQ(rb.bind_count, 0xFFFFFFFFu);
            EXPECT_EQ(rb.space, 2u);
            EXPECT_EQ(rb.range_id, 2u);
        }
    }
    EXPECT_TRUE(texu && cbu);
}

// cs_dims.asm: every dcl_resource dimension (1d 2, 1darray 7, 2darray 8,
// 3d 5, cube 6, cubearray 10, 2dms(0) 4, 2dmsarray(4) 9, 2d 3), the typed
// UAV shapes, `_glc`, `_opc` and mode_comparison.
TEST(DxbcSm51Test, DeclarationFlagsAndDimensions) {
    const auto c = parse_blob(sm51_cs_dims, sizeof(sm51_cs_dims));
    ASSERT_TRUE(c.valid());
    using C = DxbcRegisterClass;
    const u32 dims[] = {2, 7, 8, 5, 6, 10, 4, 9, 3};
    for (u32 id = 0; id < 9; ++id) {
        const auto* d = find_declaration(c, C::ShaderResource, id);
        ASSERT_NE(d, nullptr);
        EXPECT_EQ(d->dimension, dims[id]) << "T" << id;
        EXPECT_EQ(d->lower, id * 2u) << "T" << id;
    }
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 2)->return_type, 0x4444u);
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 3)->return_type, 0x3333u);
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 6)->sample_count, 0u);
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 7)->sample_count, 4u);
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 8)->upper, 18u);
    EXPECT_EQ(find_declaration(c, C::Sampler, 0)->sampler_mode, 1u);
    EXPECT_EQ(find_declaration(c, C::Sampler, 1)->sampler_mode, 0u);
    const u32 uav_dims[] = {8, 5, 1, 2};
    for (u32 id = 0; id < 4; ++id) {
        EXPECT_EQ(find_declaration(c, C::UnorderedAccess, id)->dimension,
                  uav_dims[id]) << "U" << id;
    }
    EXPECT_TRUE(find_declaration(c, C::UnorderedAccess, 3)->globally_coherent);
    EXPECT_FALSE(find_declaration(c, C::UnorderedAccess, 2)->globally_coherent);
    EXPECT_TRUE(find_declaration(c, C::UnorderedAccess, 4)->has_counter);
    EXPECT_EQ(find_declaration(c, C::ConstantBuffer, 0)->cb_vectors, 9u);
    EXPECT_EQ(find_declaration(c, C::ConstantBuffer, 1)->cb_vectors, 8u);

    // ps_arrays.asm: the counter array U1[3:4] carries no `_opc` although
    // the program moves its counters.
    const auto p = parse_blob(sm51_ps_arrays, sizeof(sm51_ps_arrays));
    EXPECT_FALSE(find_declaration(p, C::UnorderedAccess, 0)->rasterizer_ordered);
    EXPECT_FALSE(find_declaration(p, C::UnorderedAccess, 1)->has_counter);

    // ps_rov.asm: dcl_uav_typed_texture2d_rov U0[1:2] -- declared, and
    // refused by the translation (ROVsSupported is not claimed).
    const auto rov = parse_blob(sm51_ps_rov, sizeof(sm51_ps_rov));
    EXPECT_TRUE(find_declaration(rov, C::UnorderedAccess, 0)->rasterizer_ordered);
    const auto refused = translate_dxbc_to_spirv(rov, sm51_options(rov));
    EXPECT_FALSE(refused.valid());
    EXPECT_NE(refused.failure.find("rasterizer-ordered"), std::string::npos)
        << refused.failure;
}

// Before 5.1 a declaration is one register of space 0 named by itself
// (cs_space0_50.asm: dcl_constantbuffer CB2[1], dcl_resource_texture2d t3,
// dcl_resource_structured t5, 4, ...).
TEST(DxbcSm51Test, Sm50DeclarationsAreSingleRegistersOfSpaceZero) {
    const auto c = parse_blob(sm51_cs_space0_50, sizeof(sm51_cs_space0_50));
    ASSERT_TRUE(c.valid());
    EXPECT_FALSE(c.is_sm51_plus());
    using C = DxbcRegisterClass;
    const auto* cb = find_declaration(c, C::ConstantBuffer, 2);
    ASSERT_NE(cb, nullptr);
    EXPECT_EQ(cb->cb_vectors, 1u);
    for (const auto& d : c.binding_declarations) {
        EXPECT_EQ(d.space, 0u);
        EXPECT_EQ(d.lower, d.range_id);
        EXPECT_EQ(d.upper, d.lower);
        EXPECT_FALSE(d.is_array());
    }
    EXPECT_EQ(find_declaration(c, C::ShaderResource, 5)->stride, 4u);
    EXPECT_EQ(find_declaration(c, C::UnorderedAccess, 4)->dimension, 3u);
    EXPECT_EQ(c.binding_declarations.size(), 8u);
}

// No remap: space 0 keeps the per-class sets and binding = register, so a
// 5.1 program without spaces or arrays lands exactly where its 5.0 build
// does; another space is refused rather than folded onto space 0.
TEST(DxbcSm51Test, WithoutRemapSpaceZeroMatchesSm50AndOtherSpacesFail) {
    const auto c51 = parse_blob(sm51_cs_space0_51, sizeof(sm51_cs_space0_51));
    const auto c50 = parse_blob(sm51_cs_space0_50, sizeof(sm51_cs_space0_50));
    const auto r51 = translate_dxbc_to_spirv(c51);
    const auto r50 = translate_dxbc_to_spirv(c50);
    ASSERT_TRUE(r51.valid()) << r51.failure;
    ASSERT_TRUE(r50.valid()) << r50.failure;
    std::multiset<std::pair<u32, u32>> b51, b50;
    for (const auto& [id, sb] : descriptor_bindings(r51.spirv)) b51.insert(sb);
    for (const auto& [id, sb] : descriptor_bindings(r50.spirv)) b50.insert(sb);
    EXPECT_EQ(b51, b50);
    // b2 -> (0, 2), t3 -> (1, 3), s1 -> (2, 1), u2 -> (3, 2), ...
    EXPECT_TRUE(b51.contains({0u, 2u}));
    EXPECT_TRUE(b51.contains({1u, 3u}));
    EXPECT_TRUE(b51.contains({1u, 5u}));
    EXPECT_TRUE(b51.contains({2u, 1u}));
    EXPECT_TRUE(b51.contains({3u, 2u}));
    EXPECT_TRUE(b51.contains({3u, 4u}));
    ASSERT_EQ(r51.bindings.size(), 8u);
    for (const auto& b : r51.bindings) {
        EXPECT_EQ(b.binding, b.declaration.lower);
        EXPECT_EQ(b.descriptor_count, 1u);
    }

    const auto arrays = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    const auto refused = translate_dxbc_to_spirv(arrays);
    EXPECT_FALSE(refused.valid());
    EXPECT_NE(refused.failure.find("needs a register remap"),
              std::string::npos) << refused.failure;
}

// With a remap: each range lands on its run's bindings, arrays are
// descriptor arrays (an unbounded range the remap bounds is a sized one, an
// unbounded run a runtime array), and indexing emits the capabilities and
// NonUniform decorations SPIR-V requires.
TEST(DxbcSm51Test, RemappedRangesBecomeDescriptorArrays) {
    const auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    DxbcTranslationOptions options{};
    using C = DxbcRegisterClass;
    options.register_remap = {
        {C::ConstantBuffer, 0, 3, 1, 0, 0},            // b3 -> set 0 slot 0
        {C::ConstantBuffer, 1, 0, 8, 0, 8},            // b0-7 space1 -> 8..15
        {C::ConstantBuffer, 2, 0, kDxbcUnboundedRange, 0, 16},
        {C::Sampler, 0, 0, 4, 2, 0},
        {C::Sampler, 4, 0, 6, 2, 4},                   // bounds S1[0:*] to 6
        {C::ShaderResource, 0, 0, 10, 1, 0},
        {C::ShaderResource, 1, 0, 8, 1, 10},
        {C::ShaderResource, 3, 10, kDxbcUnboundedRange, 1, 18},
        {C::UnorderedAccess, 0, 0, 2, 3, 0},
        {C::UnorderedAccess, 6, 1, 1, 3, 2},
        {C::UnorderedAccess, 5, 2, kDxbcUnboundedRange, 3, 3},
    };
    const auto r = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(r.valid()) << r.failure;
    const auto binding_of = [&](C cls, u32 id) -> const DxbcResolvedBinding* {
        for (const auto& b : r.bindings) {
            if (b.declaration.register_class == cls &&
                b.declaration.range_id == id) {
                return &b;
            }
        }
        return nullptr;
    };
    struct Want { C cls; u32 id, set, binding, count; };
    const Want want[] = {
        {C::ConstantBuffer, 0, 0, 0, 1},  {C::ConstantBuffer, 1, 0, 9, 4},
        {C::ConstantBuffer, 2, 0, 16, 0}, {C::Sampler, 0, 2, 1, 2},
        {C::Sampler, 1, 2, 4, 6},         {C::ShaderResource, 0, 1, 2, 8},
        {C::ShaderResource, 1, 1, 10, 3}, {C::ShaderResource, 2, 1, 14, 2},
        {C::ShaderResource, 3, 1, 16, 2}, {C::ShaderResource, 4, 1, 18, 0},
        {C::UnorderedAccess, 0, 3, 0, 2}, {C::UnorderedAccess, 1, 3, 3, 0},
        {C::UnorderedAccess, 2, 3, 2, 1},
    };
    for (const Want& w : want) {
        const auto* b = binding_of(w.cls, w.id);
        ASSERT_NE(b, nullptr) << "range " << w.id;
        EXPECT_EQ(b->descriptor_set, w.set) << "range " << w.id;
        EXPECT_EQ(b->binding, w.binding) << "range " << w.id;
        EXPECT_EQ(b->descriptor_count, w.count) << "range " << w.id;
    }
    // Three runtime arrays (CB2, T4, U1) and the capabilities (SPIR-V 1.6
    // Capability: RuntimeDescriptorArray 5302, ShaderNonUniform 5301, the
    // *ArrayDynamicIndexing 28-31 / 5304) and NonUniform (5300).
    EXPECT_EQ(count_opcode(r.spirv, /*OpTypeRuntimeArray=*/29u) >= 3u, true);
    EXPECT_TRUE(has_capability(r.spirv, 5302u));
    EXPECT_TRUE(has_capability(r.spirv, 5301u));
    EXPECT_TRUE(has_capability(r.spirv, 28u));
    EXPECT_TRUE(has_capability(r.spirv, 29u));
    EXPECT_TRUE(has_capability(r.spirv, 30u));
    EXPECT_TRUE(has_capability(r.spirv, 31u));
    EXPECT_TRUE(has_capability(r.spirv, 5304u));
    EXPECT_TRUE(has_decoration(r.spirv, 5300u));
    // Every binding the SPIR-V declares is one the reflection names.
    std::set<std::pair<u32, u32>> reflected;
    for (const auto& b : r.bindings) {
        reflected.insert({b.descriptor_set, b.binding});
    }
    for (const auto& [id, sb] : descriptor_bindings(r.spirv)) {
        if (sb.first > 3u) continue;
        EXPECT_TRUE(reflected.contains(sb))
            << "set " << sb.first << " binding " << sb.second;
    }
}

// A declared array must land on consecutive bindings of one set: two runs
// that continue each other in register and binding serve it, two that do
// not are refused -- as is a register no run covers.
TEST(DxbcSm51Test, RemapRunsMustBeConsecutive) {
    const auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    using C = DxbcRegisterClass;
    auto options = sm51_options(c);
    // T0[2:9] in space 0: split over two runs that continue each other.
    std::erase_if(options.register_remap, [](const DxbcRegisterRemap& e) {
        return e.register_class == C::ShaderResource && e.space == 0u;
    });
    auto split = options;
    split.register_remap.push_back({C::ShaderResource, 0, 0, 5, 1, 200});
    split.register_remap.push_back({C::ShaderResource, 0, 5, 5, 1, 205});
    const auto r = translate_dxbc_to_spirv(c, split);
    ASSERT_TRUE(r.valid()) << r.failure;
    bool found = false;
    for (const auto& b : r.bindings) {
        if (b.declaration.register_class == C::ShaderResource &&
            b.declaration.range_id == 0u) {
            found = true;
            EXPECT_EQ(b.binding, 202u);
            EXPECT_EQ(b.descriptor_count, 8u);
        }
    }
    EXPECT_TRUE(found);

    auto gap = options;
    gap.register_remap.push_back({C::ShaderResource, 0, 0, 5, 1, 200});
    gap.register_remap.push_back({C::ShaderResource, 0, 5, 5, 1, 300});
    const auto refused = translate_dxbc_to_spirv(c, gap);
    EXPECT_FALSE(refused.valid());
    EXPECT_NE(refused.failure.find("t2:9 space 0"), std::string::npos)
        << refused.failure;

    const auto missing = translate_dxbc_to_spirv(c, options);
    EXPECT_FALSE(missing.valid());
    EXPECT_NE(missing.failure.find("is not on consecutive bindings"),
              std::string::npos) << missing.failure;
}

// The options keyed by a D3D11 register have no meaning for a range ID and
// are refused; the push block carries no s# state (MipLODBias) for a
// descriptor-table translation.
TEST(DxbcSm51Test, D3D11RegisterOptionsAreRefused) {
    const auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    auto guarded = sm51_options(c);
    guarded.guard_constant_buffers = true;
    auto result = translate_dxbc_to_spirv(c, guarded);
    EXPECT_FALSE(result.valid());
    EXPECT_NE(result.failure.find("keyed by D3D11 register"), std::string::npos);
    auto twins = sm51_options(c);
    twins.border_twin_samplers = 1u;
    EXPECT_FALSE(translate_dxbc_to_spirv(c, twins).valid());

    const auto plain = translate_dxbc_to_spirv(c, sm51_options(c));
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_EQ(count_opcode(plain.spirv, /*OpTypeStruct=*/30u) > 0u, true);
    // No PushConstant variable (storage class 9) in a compute shader that
    // samples: the D3D11 bias block is not read.
    bool push = false;
    for (usize i = 5; i < plain.spirv.size();) {
        const u32 words = plain.spirv[i] >> 16;
        if (words == 0) break;
        if ((plain.spirv[i] & 0xFFFFu) == 59u && plain.spirv[i + 3] == 9u) {
            push = true;
        }
        i += words;
    }
    EXPECT_FALSE(push);
}

// A counter per element of a UAV array (ps_arrays: imm_atomic_alloc
// U1[r0.y + 3] over RWStructuredBuffer<uint> cntarr[2] : register(u3)):
// consecutive counter bindings, one register each.
TEST(DxbcSm51Test, UavArrayCountersBindPerElement) {
    const auto p = parse_blob(sm51_ps_arrays, sizeof(sm51_ps_arrays));
    const auto r = translate_dxbc_to_spirv(p, sm51_options(p));
    ASSERT_TRUE(r.valid()) << r.failure;
    EXPECT_EQ(r.uav_counter_registers, (std::vector<u32>{3u, 4u}));
    EXPECT_EQ(r.uav_counter_spaces, (std::vector<u32>{0u, 0u}));

    const auto d = parse_blob(sm51_cs_dims, sizeof(sm51_cs_dims));
    const auto rd = translate_dxbc_to_spirv(d, sm51_options(d));
    ASSERT_TRUE(rd.valid()) << rd.failure;
    EXPECT_EQ(rd.uav_counter_registers, (std::vector<u32>{8u}));
}

// Every corpus blob translates with a remap -- all six stages, as the
// frontend lowers them -- to SPIR-V that spirv-val accepts (skipped where
// SPIRV-Tools is not installed).
TEST(DxbcSm51Test, EveryCorpusShaderTranslatesToValidSpirv) {
    const bool validate =
        std::system("command -v spirv-val >/dev/null 2>&1") == 0;
    namespace fs = std::filesystem;
    const fs::path scratch =
        fs::temp_directory_path() / "vitreo_spirv_val_sm51";
    fs::create_directories(scratch);
    for (const auto& blob : kSm51Blobs) {
        if (std::string(blob.name) == "ps_rov") continue;  // refused above
        const auto c = parse_blob(blob.code, blob.size);
        ASSERT_TRUE(c.valid()) << blob.name;
        const auto r = translate_dxbc_to_spirv(c, sm51_options(c));
        ASSERT_TRUE(r.valid()) << blob.name << ": " << r.failure;
        if (!validate) continue;
        const fs::path spv = scratch / (std::string(blob.name) + ".spv");
        {
            std::ofstream out(spv, std::ios::binary);
            out.write(reinterpret_cast<const char*>(r.spirv.data()),
                      static_cast<std::streamsize>(r.spirv.size() * 4));
        }
        const std::string command = "spirv-val --target-env spv1.6 '" +
                                    spv.string() + "' 2>&1";
        EXPECT_EQ(std::system(command.c_str()), 0)
            << "spirv-val rejects " << blob.name;
    }
}

// fxc sizes a dynamically indexed 5.1 constant buffer below what it reads
// (cs_arrays.asm: dcl_constantbuffer CB1[1:4][2] for the 80-byte CBData the
// program indexes as arr[i & 3]), so the translation takes the RDEF size --
// and, stripped of reflection (D3DStripShader), the largest view there is,
// D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT (4096, d3d12.h). An immediately
// indexed buffer keeps its declared size.
TEST(DxbcSm51Test, DynamicallyIndexedConstantBuffersCoverWhatTheyRead) {
    const auto array_lengths = [](const std::vector<u32>& spirv) {
        std::map<u32, u32> constants;
        std::multiset<u32> lengths;
        for (usize i = 5; i < spirv.size();) {
            const u32 words = spirv[i] >> 16;
            if (words == 0) break;
            const u32 op = spirv[i] & 0xFFFFu;
            if (op == 43u && words == 4) constants[spirv[i + 2]] = spirv[i + 3];
            if (op == 28u) {  // OpTypeArray
                const auto found = constants.find(spirv[i + 3]);
                if (found != constants.end()) lengths.insert(found->second);
            }
            i += words;
        }
        return lengths;
    };
    auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    const auto reflected = translate_dxbc_to_spirv(c, sm51_options(c));
    ASSERT_TRUE(reflected.valid()) << reflected.failure;
    // CBData is 80 bytes: 5 vectors, not the declared 2.
    EXPECT_TRUE(array_lengths(reflected.spirv).contains(5u));
    c.resource_bindings.clear();
    c.constant_buffers.clear();
    const auto stripped = translate_dxbc_to_spirv(c, sm51_options(c));
    ASSERT_TRUE(stripped.valid()) << stripped.failure;
    const auto lengths = array_lengths(stripped.spirv);
    EXPECT_TRUE(lengths.contains(4096u));
    EXPECT_TRUE(lengths.contains(2u));  // CB0[3:3][2], immediateIndexed
    EXPECT_FALSE(lengths.contains(5u));
}

// DxbcTranslationOptions::sampler_state_buffer: a descriptor-table
// translation's MipLODBias / POINT-mip records, one per sampler binding --
// so the samplers must share one set -- and never a D3D11 translation's
// (its push block carries that state). The GPU side is
// Sm51Gpu.SamplerStateBufferAppliesBiasAndPointMip.
TEST(DxbcSm51Test, SamplerStateBufferIsOneSetOfDescriptorTableRecords) {
    const auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    auto options = sm51_options(c);
    options.sampler_state_buffer = {true, 0, 200};
    const auto r = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(r.valid()) << r.failure;
    EXPECT_EQ(std::count(r.spirv.begin(), r.spirv.end(), 200u) > 0, true);
    bool declared = false;
    for (const auto& [id, sb] : descriptor_bindings(r.spirv)) {
        declared |= sb == std::pair<u32, u32>{0u, 200u};
    }
    EXPECT_TRUE(declared);

    auto split = options;
    for (auto& entry : split.register_remap) {
        if (entry.register_class == DxbcRegisterClass::Sampler &&
            entry.space == 4u) {
            entry.descriptor_set = 0;
        }
    }
    const auto refused = translate_dxbc_to_spirv(c, split);
    EXPECT_FALSE(refused.valid());
    EXPECT_NE(refused.failure.find("more than one descriptor set"),
              std::string::npos) << refused.failure;

    const auto sm50 = parse_blob(sm51_cs_space0_50, sizeof(sm51_cs_space0_50));
    DxbcTranslationOptions d3d11{};
    d3d11.sampler_state_buffer = {true, 0, 200};
    const auto d3d11_result = translate_dxbc_to_spirv(sm50, d3d11);
    EXPECT_FALSE(d3d11_result.valid());
    EXPECT_NE(d3d11_result.failure.find("push block"), std::string::npos)
        << d3d11_result.failure;
}

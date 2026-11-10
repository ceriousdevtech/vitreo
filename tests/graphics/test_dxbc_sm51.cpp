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

#include "graphics/contracts/backend_device.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"
#include "graphics/shaders/sampler_features.hpp"

#include <atomic>
#include <thread>

#include "../oracle_data/d3d12_sm51/d3d12_sm51_shaders.h"
#include "typed_buffer_offset_shaders.h"

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
    // With the frontend ordering them (DxbcTranslationOptions::
    // rasterizer_ordered_views) the array translates and its resolved
    // binding keeps the flag for the MSL compile's raster order group.
    auto ordered_options = sm51_options(rov);
    ordered_options.rasterizer_ordered_views = true;
    const auto ordered = translate_dxbc_to_spirv(rov, ordered_options);
    ASSERT_TRUE(ordered.valid()) << ordered.failure;
    u32 ordered_bindings = 0;
    for (const auto& binding : ordered.bindings) {
        if (binding.declaration.rasterizer_ordered) ++ordered_bindings;
    }
    EXPECT_EQ(ordered_bindings, 1u);
    // A _rov declaration exists only in a pixel shader (fxc: ps_5_x).
    auto as_compute = rov;
    as_compute.stage = ShaderStage::Compute;
    const auto wrong_stage = translate_dxbc_to_spirv(as_compute, ordered_options);
    EXPECT_FALSE(wrong_stage.valid());
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

// Learn (ByteAddressBuffer / StructuredBuffer): a descriptor-table load or
// store past the view returns 0 / is dropped, from the view's word count.
// cs_arrays loads rbs[i & 1] and stores ruav[i] / suav[i]. The push-block
// guard is refused here; the sizes are one uint per table slot.
TEST(DxbcSm51Test, DescriptorTableRawAndStructuredViewsAreBoundsChecked) {
    const auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    const auto plain = translate_dxbc_to_spirv(c, sm51_options(c));
    ASSERT_TRUE(plain.valid()) << plain.failure;
    const usize unguarded_limits = count_opcode(plain.spirv, /*OpULessThan=*/176u);

    auto options = sm51_options(c);
    options.view_size_buffer.enabled = true;
    options.view_size_buffer.srv = true;
    options.view_size_buffer.srv_set = 0;
    options.view_size_buffer.srv_binding = 96;
    options.view_size_buffer.uav = true;
    options.view_size_buffer.uav_set = 0;
    options.view_size_buffer.uav_binding = 97;
    const auto guarded = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(guarded.valid()) << guarded.failure;
    EXPECT_GT(count_opcode(guarded.spirv, /*OpULessThan=*/176u),
              unguarded_limits);

    MslCompileOptions msl{};
    msl.argument_buffers = true;
    const MslDescriptorClass classes[4] = {
        MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
        MslDescriptorClass::Sampler, MslDescriptorClass::Buffer};
    const u32 slots[4] = {128u, 128u, 128u, 128u};
    for (u32 set = 0; set < 4; ++set) {
        msl.argument_buffer_sets[set] = {
            slots[set], BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set, classes[set]};
    }
    const auto compiled = compile_spirv_to_msl(
        guarded.spirv.data(), guarded.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << "view-size translation did not compile";
    const std::string text(compiled.msl_text());
    EXPECT_NE(text.find("vitreo_srv_view_sizes"), std::string::npos) << text;
    EXPECT_NE(text.find("vitreo_uav_view_sizes"), std::string::npos) << text;
}

// D3D11.3 7.5: a constant-buffer element past the bound view reads 0.
// cs_arrays reads cbs[i & 3], cbu[i] and plain_v. The push-block guard is
// refused for a descriptor table; the vector counts are one uint per slot.
TEST(DxbcSm51Test, DescriptorTableConstantBuffersAreBoundsChecked) {
    const auto c = parse_blob(sm51_cs_arrays, sizeof(sm51_cs_arrays));
    const auto plain = translate_dxbc_to_spirv(c, sm51_options(c));
    ASSERT_TRUE(plain.valid()) << plain.failure;
    const usize unguarded_limits = count_opcode(plain.spirv, /*OpULessThan=*/176u);

    auto options = sm51_options(c);
    options.constant_buffer_limits.enabled = true;
    options.constant_buffer_limits.descriptor_set = 0;
    options.constant_buffer_limits.binding = 98;
    const auto guarded = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(guarded.valid()) << guarded.failure;
    EXPECT_GT(count_opcode(guarded.spirv, /*OpULessThan=*/176u),
              unguarded_limits);

    MslCompileOptions msl{};
    msl.argument_buffers = true;
    const MslDescriptorClass classes[4] = {
        MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
        MslDescriptorClass::Sampler, MslDescriptorClass::Buffer};
    const u32 slots[4] = {128u, 128u, 128u, 128u};
    for (u32 set = 0; set < 4; ++set) {
        msl.argument_buffer_sets[set] = {
            slots[set], BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set, classes[set]};
    }
    const auto compiled = compile_spirv_to_msl(
        guarded.spirv.data(), guarded.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << "constant-buffer limit translation did not compile";
    const std::string text(compiled.msl_text());
    EXPECT_NE(text.find("vitreo_cbv_limits"), std::string::npos) << text;
}

// D3D11.3 22.4.14 / 22.4.1: GetDimensions on a null descriptor is 0 in every
// component, including the mip count. cs_dims queries texture and typed
// buffer dimensions. The flag is per table slot so the image is not loaded
// when the descriptor is null.
TEST(DxbcSm51Test, NullDescriptorsReportZeroDimensions) {
    const auto c = parse_blob(sm51_cs_dims, sizeof(sm51_cs_dims));
    const auto plain = translate_dxbc_to_spirv(c, sm51_options(c));
    ASSERT_TRUE(plain.valid()) << plain.failure;
    const usize plain_merges = count_opcode(plain.spirv, /*OpSelectionMerge=*/247u);

    auto options = sm51_options(c);
    options.null_descriptors.enabled = true;
    options.null_descriptors.srv = true;
    options.null_descriptors.srv_set = 0;
    options.null_descriptors.srv_binding = 96;
    options.null_descriptors.uav = true;
    options.null_descriptors.uav_set = 0;
    options.null_descriptors.uav_binding = 97;
    const auto guarded = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(guarded.valid()) << guarded.failure;
    EXPECT_GT(count_opcode(guarded.spirv, /*OpSelectionMerge=*/247u),
              plain_merges);

    MslCompileOptions msl{};
    msl.argument_buffers = true;
    if (guarded.uses_typed_uav_atomics) msl.msl_version = 30100u;
    const MslDescriptorClass classes[4] = {
        MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
        MslDescriptorClass::Sampler, MslDescriptorClass::Buffer};
    const u32 slots[4] = {128u, 128u, 128u, 128u};
    for (u32 set = 0; set < 4; ++set) {
        msl.argument_buffer_sets[set] = {
            slots[set], BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set, classes[set]};
    }
    const auto compiled = compile_spirv_to_msl(
        guarded.spirv.data(), guarded.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << "null-descriptor translation did not compile";
    const std::string text(compiled.msl_text());
    EXPECT_NE(text.find("vitreo_srv_null"), std::string::npos) << text;
    EXPECT_NE(text.find("vitreo_uav_null"), std::string::npos) << text;
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

// fxc 10.1 tokens from cs_arrays (dcl_resource texture2d float4, ld, and
// the nonuniform ld of an unbounded t#). Range id, lower bound and space
// are the 5.1 operand fields; upper 0xffffffff is the unbounded form.
static std::vector<u32> two_unbounded_srv_shex(u32 second_lower,
                                               u32 second_space) {
    std::vector<u32> words;
    words.push_back(0x00050051u);
    words.push_back(0u);
    words.push_back(0x0100006au);  // dcl_globalFlags
    const auto declare = [&](u32 range_id, u32 lower, u32 space) {
        words.insert(words.end(),
                     {0x07001858u, 0x00307e46u, range_id, lower, 0xffffffffu,
                      0x5555u, space});
    };
    declare(0u, 0u, 0u);
    declare(1u, second_lower, second_space);
    words.insert(words.end(), {0x02000068u, 4u});          // dcl_temps
    words.insert(words.end(), {0x0400009bu, 1u, 1u, 1u});  // dcl_thread_group
    // ld r2.xyzw, l(1, 1, 0, 0), t0[0]
    words.insert(words.end(), {0x0b00002du, 0x001000f2u, 2u, 0x00004002u, 1u,
                               1u, 0u, 0u, 0x00207e46u, 0u, 0u});
    // ld r2.xyzw, l(0, 0, 0, 0), t1[r0.y + second_lower] nonuniform
    words.insert(words.end(),
                 {0x0e00002du, 0x001000f2u, 2u, 0x00004002u, 0u, 0u, 0u, 0u,
                  0x86207e46u, 0x00020001u, 1u, second_lower, 0x0010001au, 0u});
    words.push_back(0x0100003eu);  // ret
    words[1] = static_cast<u32>(words.size());
    return words;
}

static DxbcContainer shex_container(const std::vector<u32>& shex) {
    std::vector<u8> bytes;
    const auto put = [&](u32 value) {
        bytes.push_back(static_cast<u8>(value));
        bytes.push_back(static_cast<u8>(value >> 8));
        bytes.push_back(static_cast<u8>(value >> 16));
        bytes.push_back(static_cast<u8>(value >> 24));
    };
    put(0x43425844u);
    put(0u);
    put(0u);
    put(0u);
    put(0u);
    put(1u);
    const u32 chunk = 36u;
    put(chunk + 8u + static_cast<u32>(shex.size() * 4u));
    put(1u);
    put(chunk);
    put(0x58454853u);  // SHEX
    put(static_cast<u32>(shex.size() * 4u));
    for (const u32 word : shex) put(word);
    return parse_dxbc(bytes.data(), bytes.size());
}

static const DxbcResolvedBinding* binding_of(const DxbcToSpirvResult& result,
                                            DxbcRegisterClass cls, u32 id) {
    for (const auto& binding : result.bindings) {
        if (binding.declaration.register_class == cls &&
            binding.declaration.range_id == id) {
            return &binding;
        }
    }
    return nullptr;
}

static bool spirv_module_is_valid(const std::vector<u32>& spirv,
                                  const char* name) {
    if (std::system("command -v spirv-val >/dev/null 2>&1") != 0) return true;
    namespace fs = std::filesystem;
    const fs::path spv = fs::temp_directory_path() / name;
    {
        std::ofstream out(spv, std::ios::binary);
        out.write(reinterpret_cast<const char*>(spirv.data()),
                  static_cast<std::streamsize>(spirv.size() * 4));
    }
    const std::string command =
        "spirv-val --target-env spv1.6 '" + spv.string() + "' 2>&1";
    return std::system(command.c_str()) == 0;
}

// Two unbounded SRV ranges. The table slot is the binding: different
// register spaces, and the same space at a later base whose run the
// earlier unbounded entry also covers. A dynamic NonUniform index of the
// second range addresses that range's array. spirv-val accepts the module.
TEST(DxbcSm51Test, TwoUnboundedSrvRangesAreDistinctBindings) {
    using C = DxbcRegisterClass;
    const auto spaces = shex_container(two_unbounded_srv_shex(0u, 1u));
    ASSERT_TRUE(spaces.valid());
    ASSERT_TRUE(spaces.is_sm51_plus());
    const auto* space0 = find_declaration(spaces, C::ShaderResource, 0u);
    const auto* space1 = find_declaration(spaces, C::ShaderResource, 1u);
    ASSERT_NE(space0, nullptr);
    ASSERT_NE(space1, nullptr);
    EXPECT_TRUE(space0->unbounded());
    EXPECT_TRUE(space1->unbounded());
    EXPECT_EQ(space0->space, 0u);
    EXPECT_EQ(space1->space, 1u);
    EXPECT_EQ(space0->upper, kDxbcUnboundedRange);
    EXPECT_EQ(space1->upper, kDxbcUnboundedRange);

    // configure_descriptor_bindings' layout: the earlier unbounded range is
    // a sized array of the slots it was given; the later one is the single
    // runtime array, at the next slot. 4 is that assignment, not a register.
    DxbcTranslationOptions options{};
    options.register_remap = {
        {C::ShaderResource, 0u, 0u, 4u, 1u, 0u},
        {C::ShaderResource, 1u, 0u, kDxbcUnboundedRange, 1u, 4u},
    };
    const auto translated = translate_dxbc_to_spirv(spaces, options);
    ASSERT_TRUE(translated.valid()) << translated.failure;
    const auto* first = binding_of(translated, C::ShaderResource, 0u);
    const auto* second = binding_of(translated, C::ShaderResource, 1u);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->descriptor_set, 1u);
    EXPECT_EQ(first->binding, 0u);
    EXPECT_EQ(first->descriptor_count, 4u);
    EXPECT_EQ(second->descriptor_set, 1u);
    EXPECT_EQ(second->binding, 4u);
    EXPECT_EQ(second->descriptor_count, 0u);
    EXPECT_NE(first->binding, second->binding);
    EXPECT_TRUE(has_decoration(translated.spirv, /*NonUniform=*/5300u));
    EXPECT_TRUE(has_capability(translated.spirv, /*ShaderNonUniform=*/5301u));
    EXPECT_TRUE(has_capability(translated.spirv, /*RuntimeDescriptorArray=*/5302u));
    // SPIR-V SampledImageArrayDynamicIndexing / SampledImageArrayNonUniformIndexing.
    EXPECT_TRUE(has_capability(translated.spirv, 29u));
    EXPECT_TRUE(has_capability(translated.spirv, 5307u));
    std::set<std::pair<u32, u32>> spirv_bindings;
    for (const auto& [id, sb] : descriptor_bindings(translated.spirv)) {
        if (sb.first == 1u) spirv_bindings.insert(sb);
    }
    EXPECT_TRUE(spirv_bindings.contains({1u, 0u}));
    EXPECT_TRUE(spirv_bindings.contains({1u, 4u}));
    EXPECT_TRUE(spirv_module_is_valid(translated.spirv,
                                      "vitreo_two_unbounded_srv_spaces.spv"));

    MslCompileOptions msl{};
    msl.argument_buffers = true;
    const MslDescriptorClass classes[4] = {
        MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
        MslDescriptorClass::Sampler, MslDescriptorClass::Buffer};
    // Sized array occupies 0..3; the runtime array starts at 4 and is given
    // the four slots through the end of this table.
    const u32 slots[4] = {0u, 8u, 0u, 0u};
    for (u32 set = 0; set < 4; ++set) {
        msl.argument_buffer_sets[set] = {
            slots[set], BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set, classes[set]};
    }
    const auto compiled = compile_spirv_to_msl(
        translated.spirv.data(), translated.spirv.size(), ShaderStage::Compute,
        msl);
    ASSERT_TRUE(compiled.valid()) << "two unbounded SRV ranges did not compile";

    // Same space, later base. The unbounded run from register 0 also covers
    // register 10; the range still binds at the slot its own run names (0),
    // not at 8 + 10.
    const auto same = shex_container(two_unbounded_srv_shex(10u, 0u));
    ASSERT_TRUE(same.valid());
    EXPECT_EQ(find_declaration(same, C::ShaderResource, 1u)->lower, 10u);
    EXPECT_EQ(find_declaration(same, C::ShaderResource, 1u)->space, 0u);
    DxbcTranslationOptions later{};
    later.register_remap = {
        {C::ShaderResource, 0u, 0u, kDxbcUnboundedRange, 1u, 8u},
        {C::ShaderResource, 0u, 10u, 4u, 1u, 0u},
    };
    const auto later_result = translate_dxbc_to_spirv(same, later);
    ASSERT_TRUE(later_result.valid()) << later_result.failure;
    const auto* low = binding_of(later_result, C::ShaderResource, 0u);
    const auto* high = binding_of(later_result, C::ShaderResource, 1u);
    ASSERT_NE(low, nullptr);
    ASSERT_NE(high, nullptr);
    EXPECT_EQ(low->binding, 8u);
    EXPECT_EQ(low->descriptor_count, 0u);
    EXPECT_EQ(high->binding, 0u);
    EXPECT_EQ(high->descriptor_count, 4u);
    EXPECT_NE(low->binding, high->binding);
    EXPECT_TRUE(has_decoration(later_result.spirv, /*NonUniform=*/5300u));
    EXPECT_TRUE(spirv_module_is_valid(later_result.spirv,
                                      "vitreo_two_unbounded_srv_bases.spv"));
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

// A descriptor-table sample cannot bake a per-s# border or min/max bit: the
// record is three uvec4s (stride 48) and each real sampler has a twin past
// the real slots. cs_bias samples an array at bindings 4-5; the twin array
// is binding 10. Expected layout is the record in dxbc_to_spirv.hpp, not a
// measurement of this translator.
TEST(DxbcSm51Test, SamplerStateRecordCarriesBorderAndMinMax) {
    EXPECT_EQ(kSamplerStateRecordWords, 12u);
    EXPECT_EQ(kSamplerStateBiasWord, 0u);
    EXPECT_EQ(kSamplerStateFlagsWord, 1u);
    EXPECT_EQ(kSamplerStateBorderWord, 4u);
    EXPECT_EQ(kSamplerStateFilterShift, 8u);
    EXPECT_GE(kSamplerStateAnisoShift, kSamplerStateFilterShift + 9u);

    const auto c = parse_blob(sm51_cs_bias, sizeof(sm51_cs_bias));
    using C = DxbcRegisterClass;
    DxbcTranslationOptions options{};
    options.register_remap = {
        {C::ShaderResource, 0, 0, 1, 1, 0},
        {C::UnorderedAccess, 0, 0, 1, 3, 0},
        {C::Sampler, 1, 0, 2, 2, 4},
    };
    options.sampler_state_buffer = {true, 0, 3};
    options.border_twin_binding_base = 6;
    const auto r = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(r.valid()) << r.failure;

    bool twin = false;
    bool stride32 = false;
    const auto bindings = descriptor_bindings(r.spirv);
    for (const auto& [id, sb] : bindings) {
        if (sb == std::pair<u32, u32>{2u, 10u}) twin = true;
    }
    for (usize i = 5; i < r.spirv.size();) {
        const u32 words = r.spirv[i] >> 16;
        if (words == 0) break;
        // OpDecorate ArrayStride 32 (two uvec4s).
        if ((r.spirv[i] & 0xFFFFu) == 71u && words >= 4 && r.spirv[i + 2] == 6u &&
            r.spirv[i + 3] == kSamplerStateRecordWords * 4u) {
            stride32 = true;
        }
        i += words;
    }
    EXPECT_TRUE(twin);
    EXPECT_TRUE(stride32);
    // The sample branches on the record instead of a baked s# mask.
    EXPECT_GT(count_opcode(r.spirv, /*OpSelectionMerge=*/247u), 0u);

    if (std::system("command -v spirv-val >/dev/null 2>&1") == 0) {
        const auto path =
            std::filesystem::temp_directory_path() / "vitreo_s51_16.spv";
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(r.spirv.data()),
                      static_cast<std::streamsize>(r.spirv.size() * 4));
        }
        const std::string command =
            "spirv-val --target-env spv1.6 '" + path.string() + "'";
        EXPECT_EQ(std::system(command.c_str()), 0) << "spirv-val rejects cs_bias";
    }

    MslCompileOptions msl{};
    msl.argument_buffers = true;
    const MslDescriptorClass classes[4] = {
        MslDescriptorClass::Buffer, MslDescriptorClass::Texture,
        MslDescriptorClass::Sampler, MslDescriptorClass::Buffer};
    const u32 slots[4] = {4u, 1u, 12u, 1u};
    for (u32 set = 0; set < 4; ++set) {
        msl.argument_buffer_sets[set] = {
            slots[set], BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + set, classes[set]};
    }
    const auto compiled = compile_spirv_to_msl(
        r.spirv.data(), r.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << compiled.msl_text();
}

// Learn (D3D12_BUFFER_SRV / D3D12_BUFFER_UAV): FirstElement is any element.
// A descriptor table cannot use the push-block bias (keyed by a D3D11
// register). The packed entry is one uint per table slot, and ld / store /
// bufinfo add it. A non-table translation keeps the push block.
TEST(DxbcSm51Test, TypedBufferBiasIsAPerSlotSideBuffer) {
    const auto reads = parse_blob(tbo_cs_read_views, sizeof(tbo_cs_read_views));
    ASSERT_TRUE(reads.valid());
    DxbcTranslationOptions refused{};
    refused.register_remap = {
        {DxbcRegisterClass::ShaderResource, 0, 0, 2, 1, 0},
        {DxbcRegisterClass::UnorderedAccess, 0, 0, 1, 3, 0},
    };
    refused.typed_buffer_bias = true;
    const auto rejected = translate_dxbc_to_spirv(reads, refused);
    EXPECT_FALSE(rejected.valid());
    EXPECT_NE(rejected.failure.find("keyed by D3D11 register"),
              std::string::npos);

    DxbcTranslationOptions table = refused;
    table.typed_buffer_bias = false;
    table.typed_buffer_bias_buffer.enabled = true;
    table.typed_buffer_bias_buffer.srv = true;
    table.typed_buffer_bias_buffer.srv_set = 0;
    table.typed_buffer_bias_buffer.srv_binding = 0;
    const auto translated = translate_dxbc_to_spirv(reads, table);
    ASSERT_TRUE(translated.valid()) << translated.failure;
    const std::string spirv(
        reinterpret_cast<const char*>(translated.spirv.data()),
        translated.spirv.size() * 4);
    EXPECT_NE(spirv.find("vitreo_srv_typed_bias"), std::string::npos);

    DxbcTranslationOptions bare{};
    bare.register_remap = refused.register_remap;
    const auto plain = translate_dxbc_to_spirv(reads, bare);
    ASSERT_TRUE(plain.valid()) << plain.failure;
    const std::string plain_spirv(
        reinterpret_cast<const char*>(plain.spirv.data()),
        plain.spirv.size() * 4);
    EXPECT_EQ(plain_spirv.find("vitreo_srv_typed_bias"), std::string::npos);

    MslCompileOptions msl{};
    msl.argument_buffers = true;
    msl.argument_buffer_sets[0] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE,
                                   MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[1] = {2, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1,
                                   MslDescriptorClass::Texture};
    msl.argument_buffer_sets[3] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3,
                                   MslDescriptorClass::Buffer};
    const auto compiled = compile_spirv_to_msl(
        translated.spirv.data(), translated.spirv.size(), ShaderStage::Compute,
        msl);
    ASSERT_TRUE(compiled.valid());
    EXPECT_NE(std::string(compiled.msl_text()).find("vitreo_srv_typed_bias"),
              std::string::npos);

    const auto writes = parse_blob(tbo_cs_write_views, sizeof(tbo_cs_write_views));
    ASSERT_TRUE(writes.valid());
    DxbcTranslationOptions uav_options{};
    uav_options.register_remap = {
        {DxbcRegisterClass::UnorderedAccess, 0, 0, 3, 3, 0},
    };
    uav_options.typed_buffer_bias_buffer.enabled = true;
    uav_options.typed_buffer_bias_buffer.uav = true;
    uav_options.typed_buffer_bias_buffer.uav_set = 0;
    uav_options.typed_buffer_bias_buffer.uav_binding = 0;
    const auto uav = translate_dxbc_to_spirv(writes, uav_options);
    ASSERT_TRUE(uav.valid()) << uav.failure;
    const std::string uav_spirv(reinterpret_cast<const char*>(uav.spirv.data()),
                                uav.spirv.size() * 4);
    EXPECT_NE(uav_spirv.find("vitreo_uav_typed_bias"), std::string::npos);
    EXPECT_TRUE(uav.uses_typed_uav_atomics);

    if (std::system("command -v spirv-val >/dev/null 2>&1") == 0) {
        const auto path =
            std::filesystem::temp_directory_path() / "vitreo_typed_bias.spv";
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(translated.spirv.data()),
                      static_cast<std::streamsize>(translated.spirv.size() * 4));
        }
        EXPECT_EQ(std::system(("spirv-val --target-env spv1.6 '" + path.string() +
                               "'")
                                  .c_str()),
                  0);
    }
}

namespace {

// fxc 10.1 cs_5_1 (Windows SDK 10.0.26100): Texture2D<uint> GatherRed and
// Texture2D<int> GatherGreen through one sampler, stored raw.
//   dcl_resource_texture2d (uint,uint,uint,uint) T0[0:0], space=0
//   dcl_resource_texture2d (sint,sint,sint,sint) T1[1:1], space=0
//   gather4 r0.x, l(0.5, 0.5, 0, 0), T0[0].xyzw, S0[0].x
//   gather4 r0.y, l(0.25, 0.75, 0, 0), T1[1].xyzw, S0[0].y
static const unsigned char sm51_cs_int_gather[888] = {
    0x44,0x58,0x42,0x43,0xad,0x52,0xd3,0xbe,0x78,0xe9,0x94,0x8b,0xe3,0x62,0x33,0x64,
    0x25,0xb1,0xa0,0x6d,0x01,0x00,0x00,0x00,0x78,0x03,0x00,0x00,0x05,0x00,0x00,0x00,
    0x34,0x00,0x00,0x00,0x58,0x01,0x00,0x00,0x68,0x01,0x00,0x00,0x78,0x01,0x00,0x00,
    0xdc,0x02,0x00,0x00,0x52,0x44,0x45,0x46,0x1c,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x3c,0x00,0x00,0x00,0x01,0x05,0x53,0x43,
    0x00,0x05,0x00,0x00,0xf1,0x00,0x00,0x00,0x13,0x13,0x44,0x25,0x3c,0x00,0x00,0x00,
    0x18,0x00,0x00,0x00,0x28,0x00,0x00,0x00,0x28,0x00,0x00,0x00,0x24,0x00,0x00,0x00,
    0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xdc,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xe1,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,
    0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xe7,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0xed,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x73,0x61,0x6d,0x70,0x00,0x74,0x65,0x78,
    0x5f,0x75,0x00,0x74,0x65,0x78,0x5f,0x73,0x00,0x64,0x73,0x74,0x00,0x4d,0x69,0x63,
    0x72,0x6f,0x73,0x6f,0x66,0x74,0x20,0x28,0x52,0x29,0x20,0x48,0x4c,0x53,0x4c,0x20,
    0x53,0x68,0x61,0x64,0x65,0x72,0x20,0x43,0x6f,0x6d,0x70,0x69,0x6c,0x65,0x72,0x20,
    0x31,0x30,0x2e,0x31,0x00,0xab,0xab,0xab,0x49,0x53,0x47,0x4e,0x08,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x4f,0x53,0x47,0x4e,0x08,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x53,0x48,0x45,0x58,0x5c,0x01,0x00,0x00,
    0x51,0x00,0x05,0x00,0x57,0x00,0x00,0x00,0x6a,0x08,0x00,0x01,0x5a,0x00,0x00,0x06,
    0x46,0x6e,0x30,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x58,0x18,0x00,0x07,0x46,0x7e,0x30,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x44,0x44,0x00,0x00,0x00,0x00,0x00,0x00,
    0x58,0x18,0x00,0x07,0x46,0x7e,0x30,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x33,0x33,0x00,0x00,0x00,0x00,0x00,0x00,0x9d,0x00,0x00,0x06,
    0x46,0xee,0x31,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x5f,0x00,0x00,0x02,0x12,0x00,0x02,0x00,0x68,0x00,0x00,0x02,
    0x01,0x00,0x00,0x00,0x9b,0x00,0x00,0x04,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x6d,0x00,0x00,0x0e,0x12,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x02,0x40,0x00,0x00,0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x46,0x7e,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0a,0x60,0x20,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x6d,0x00,0x00,0x0e,
    0x22,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x02,0x40,0x00,0x00,0x00,0x00,0x80,0x3e,
    0x00,0x00,0x40,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46,0x7e,0x20,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x1a,0x60,0x20,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x29,0x00,0x00,0x06,0x42,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x0a,0x00,0x02,0x00,0x01,0x40,0x00,0x00,0x02,0x00,0x00,0x00,0x1e,0x00,0x00,0x07,
    0x12,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x1a,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x0a,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0xa6,0x00,0x00,0x08,0x12,0xe0,0x21,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x2a,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x0a,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x3e,0x00,0x00,0x01,0x53,0x54,0x41,0x54,
    0x94,0x00,0x00,0x00,0x06,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
};

// fxc 10.1 cs_5_0 of the same shader. The listing's gather4_indexable
// return types are (uint,uint,uint,uint) and (sint,sint,sint,sint).
static const unsigned char sm50_cs_int_gather[804] = {
    0x44,0x58,0x42,0x43,0xad,0x84,0xfb,0x4e,0x11,0x60,0x48,0xbd,0xd8,0xb3,0x08,0x82,
    0x76,0xbd,0x8d,0xca,0x01,0x00,0x00,0x00,0x24,0x03,0x00,0x00,0x05,0x00,0x00,0x00,
    0x34,0x00,0x00,0x00,0x38,0x01,0x00,0x00,0x48,0x01,0x00,0x00,0x58,0x01,0x00,0x00,
    0x88,0x02,0x00,0x00,0x52,0x44,0x45,0x46,0xfc,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0x3c,0x00,0x00,0x00,0x00,0x05,0x53,0x43,
    0x00,0x01,0x00,0x00,0xd1,0x00,0x00,0x00,0x52,0x44,0x31,0x31,0x3c,0x00,0x00,0x00,
    0x18,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x28,0x00,0x00,0x00,0x24,0x00,0x00,0x00,
    0x0c,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xbc,0x00,0x00,0x00,0x03,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xc1,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xc7,0x00,0x00,0x00,0x02,0x00,0x00,0x00,
    0x03,0x00,0x00,0x00,0x04,0x00,0x00,0x00,0xff,0xff,0xff,0xff,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0xcd,0x00,0x00,0x00,0x08,0x00,0x00,0x00,
    0x06,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x73,0x61,0x6d,0x70,0x00,0x74,0x65,0x78,
    0x5f,0x75,0x00,0x74,0x65,0x78,0x5f,0x73,0x00,0x64,0x73,0x74,0x00,0x4d,0x69,0x63,
    0x72,0x6f,0x73,0x6f,0x66,0x74,0x20,0x28,0x52,0x29,0x20,0x48,0x4c,0x53,0x4c,0x20,
    0x53,0x68,0x61,0x64,0x65,0x72,0x20,0x43,0x6f,0x6d,0x70,0x69,0x6c,0x65,0x72,0x20,
    0x31,0x30,0x2e,0x31,0x00,0xab,0xab,0xab,0x49,0x53,0x47,0x4e,0x08,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x4f,0x53,0x47,0x4e,0x08,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x08,0x00,0x00,0x00,0x53,0x48,0x45,0x58,0x28,0x01,0x00,0x00,
    0x50,0x00,0x05,0x00,0x4a,0x00,0x00,0x00,0x6a,0x08,0x00,0x01,0x5a,0x00,0x00,0x03,
    0x00,0x60,0x10,0x00,0x00,0x00,0x00,0x00,0x58,0x18,0x00,0x04,0x00,0x70,0x10,0x00,
    0x00,0x00,0x00,0x00,0x44,0x44,0x00,0x00,0x58,0x18,0x00,0x04,0x00,0x70,0x10,0x00,
    0x01,0x00,0x00,0x00,0x33,0x33,0x00,0x00,0x9d,0x00,0x00,0x03,0x00,0xe0,0x11,0x00,
    0x00,0x00,0x00,0x00,0x5f,0x00,0x00,0x02,0x12,0x00,0x02,0x00,0x68,0x00,0x00,0x02,
    0x01,0x00,0x00,0x00,0x9b,0x00,0x00,0x04,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,0x6d,0x00,0x00,0x8e,0xc2,0x00,0x00,0x80,0x03,0x11,0x11,0x00,
    0x12,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x02,0x40,0x00,0x00,0x00,0x00,0x00,0x3f,
    0x00,0x00,0x00,0x3f,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x46,0x7e,0x10,0x00,
    0x00,0x00,0x00,0x00,0x0a,0x60,0x10,0x00,0x00,0x00,0x00,0x00,0x6d,0x00,0x00,0x8e,
    0xc2,0x00,0x00,0x80,0xc3,0xcc,0x0c,0x00,0x22,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x02,0x40,0x00,0x00,0x00,0x00,0x80,0x3e,0x00,0x00,0x40,0x3f,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x46,0x7e,0x10,0x00,0x01,0x00,0x00,0x00,0x1a,0x60,0x10,0x00,
    0x00,0x00,0x00,0x00,0x1e,0x00,0x00,0x07,0x12,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x1a,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x0a,0x00,0x10,0x00,0x00,0x00,0x00,0x00,
    0x29,0x00,0x00,0x06,0x22,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x0a,0x00,0x02,0x00,
    0x01,0x40,0x00,0x00,0x02,0x00,0x00,0x00,0xa6,0x00,0x00,0x07,0x12,0xe0,0x11,0x00,
    0x00,0x00,0x00,0x00,0x1a,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x0a,0x00,0x10,0x00,
    0x00,0x00,0x00,0x00,0x3e,0x00,0x00,0x01,0x53,0x54,0x41,0x54,0x94,0x00,0x00,0x00,
    0x06,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0x00,0x00,
};

}  // namespace

// D3D11.3 7.18.9 / 7.18.9.1 and gather4 (22.4.2): an integer gather blends
// the float border as lo + (hi - lo) * border. A descriptor-table sampler
// reads that float from its record. Success without the blend means the
// border was ignored. A gather with no stored border is left on the sampler.
// Gather does not take the min/max reduction. The UINT borders a record
// represents are (0,0,0,0), (0,0,0,1) and (1,1,1,1); Metal applies those.
TEST(DxbcSm51Test, IntegerGatherDescriptorTableAppliesStoredBorder) {
    const auto c = parse_blob(sm51_cs_int_gather, sizeof(sm51_cs_int_gather));
    ASSERT_TRUE(c.valid());

    DxbcTranslationOptions table{};
    table.sampler_state_buffer = {true, 0, 0};
    table.border_twin_binding_base = 8;
    const auto r = translate_dxbc_to_spirv(c, table);
    ASSERT_TRUE(r.valid()) << r.failure;

    // Two gather4s, each through the real sampler and its white twin.
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageGather=*/96u), 4u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFSub=*/131u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFMul=*/133u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFAdd=*/129u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertUToF=*/112u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToU=*/109u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertSToF=*/111u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToS=*/110u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageSampleExplicitLod=*/88u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageDrefGather=*/97u), 0u);

    if (std::system("command -v spirv-val >/dev/null 2>&1") == 0) {
        const auto path =
            std::filesystem::temp_directory_path() / "vitreo_s51_int_gather.spv";
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(r.spirv.data()),
                      static_cast<std::streamsize>(r.spirv.size() * 4));
        }
        const std::string command =
            "spirv-val --target-env spv1.6 '" + path.string() + "'";
        EXPECT_EQ(std::system(command.c_str()), 0) << "spirv-val rejects integer gather";
    }

    MslCompileOptions msl{};
    if (r.uses_typed_uav_atomics) msl.msl_version = 30100;
    msl.argument_buffers = true;
    msl.argument_buffer_sets[0] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE,
                                   MslDescriptorClass::Buffer};
    msl.argument_buffer_sets[1] = {2, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 1,
                                   MslDescriptorClass::Texture};
    msl.argument_buffer_sets[2] = {9, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 2,
                                   MslDescriptorClass::Sampler};
    msl.argument_buffer_sets[3] = {1, BACKEND_DESCRIPTOR_TABLE_BUFFER_BASE + 3,
                                   MslDescriptorClass::Buffer};
    const auto compiled = compile_spirv_to_msl(
        r.spirv.data(), r.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << compiled.msl_text();
    const std::string metal(compiled.msl_text());
    const bool weight = metal.find("fma(") != std::string::npos ||
                        metal.find(") * ") != std::string::npos;
    EXPECT_TRUE(weight) << metal;
    EXPECT_NE(metal.find("gather"), std::string::npos) << metal;

    // No record: the gather stays on the sampler. A border the record does
    // not store is not invented, and gather is not a min/max reduction.
    const auto plain = translate_dxbc_to_spirv(c, DxbcTranslationOptions{});
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_EQ(count_opcode(plain.spirv, /*OpImageGather=*/96u), 2u);
    EXPECT_EQ(count_opcode(plain.spirv, /*OpFMul=*/133u), 0u);

    // Shader model 5.0 with no twin: the gather stays on the sampler.
    const auto sm50 = parse_blob(sm50_cs_int_gather, sizeof(sm50_cs_int_gather));
    ASSERT_TRUE(sm50.valid());
    const auto bare = translate_dxbc_to_spirv(sm50, DxbcTranslationOptions{});
    ASSERT_TRUE(bare.valid()) << bare.failure;
    EXPECT_EQ(count_opcode(bare.spirv, /*OpImageGather=*/96u), 2u);
    EXPECT_EQ(count_opcode(bare.spirv, /*OpFMul=*/133u), 0u);
}

// Shader model 5.0 static sampler: the push-block twin holds the same float
// border the sampler record stores. The gather applies lo + (hi - lo) *
// border, or the translation fails. Success without that blend drops the
// border. A min/max sampler flag does not turn gather into a reduction.
// The three UINT borders Metal applies have no twin.
TEST(DxbcSm51Test, IntegerGatherStaticSamplerAppliesBorder) {
    const auto sm50 = parse_blob(sm50_cs_int_gather, sizeof(sm50_cs_int_gather));
    ASSERT_TRUE(sm50.valid());
    DxbcTranslationOptions options{};
    options.border_twin_samplers = 1u;
    options.minmax_samplers = 1u;
    const auto r = translate_dxbc_to_spirv(sm50, options);
    if (!r.valid()) {
        EXPECT_NE(r.failure.find("integer gather"), std::string::npos)
            << r.failure;
        return;
    }

    // Two gather4s, each through the real sampler and its white twin.
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageGather=*/96u), 4u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFSub=*/131u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFMul=*/133u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFAdd=*/129u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertUToF=*/112u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToU=*/109u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertSToF=*/111u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToS=*/110u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageSampleExplicitLod=*/88u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageDrefGather=*/97u), 0u);

    if (std::system("command -v spirv-val >/dev/null 2>&1") == 0) {
        const auto path = std::filesystem::temp_directory_path() /
                          "vitreo_s50_int_gather_border.spv";
        {
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(r.spirv.data()),
                      static_cast<std::streamsize>(r.spirv.size() * 4));
        }
        const std::string command =
            "spirv-val --target-env spv1.6 '" + path.string() + "'";
        EXPECT_EQ(std::system(command.c_str()), 0)
            << "spirv-val rejects static integer gather";
    }

    MslCompileOptions msl{};
    if (r.uses_typed_uav_atomics) msl.msl_version = 30100;
    msl.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        r.spirv.data(), r.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << compiled.msl_text();
    const std::string metal(compiled.msl_text());
    const bool weight = metal.find("fma(") != std::string::npos ||
                        metal.find(") * ") != std::string::npos;
    EXPECT_TRUE(weight) << metal;
    EXPECT_NE(metal.find("gather"), std::string::npos) << metal;
}

namespace {

void append_shader_word(DxbcContainer& container, u32 word) {
    const auto* bytes = reinterpret_cast<const u8*>(&word);
    container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
}

// cs_5_0 integer gather. Token dimensions are the executable declaration:
// TEXTURE1D=2, TEXTURE3D=5, TEXTURECUBE=6, TEXTURECUBEARRAY=10. The
// gather4 operand words are fxc 10.1's from sm50_cs_int_gather. `tail` is
// appended after the sampler (a gather4_c reference). `middle` is inserted
// after the address (a gather4_po offset).
DxbcContainer integer_gather_cs(u32 token_dimension, bool sint_as_well,
                                u32 gather_opcode, u32 gather_length,
                                const std::vector<u32>& middle,
                                const std::vector<u32>& tail) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Compute;
    std::vector<u32> words;
    words.push_back(0x00050050u);
    words.push_back(0);
    words.push_back(155u | (4u << 24));
    words.push_back(1);
    words.push_back(1);
    words.push_back(1);
    words.push_back(104u | (2u << 24));
    words.push_back(1);
    const auto resource = [&](u32 slot, u32 return_type) {
        words.push_back(88u | (token_dimension << 11) | (4u << 24));
        words.push_back(0x00107000u);
        words.push_back(slot);
        words.push_back(return_type);
    };
    resource(0, 0x00004444u);
    if (sint_as_well) resource(1, 0x00003333u);
    words.push_back(90u | (3u << 24));
    words.push_back(0x00106000u);
    words.push_back(0);
    const auto gather = [&](u32 slot, u32 sampler_operand) {
        const u32 before = static_cast<u32>(words.size());
        // Bit 31 marks the opcode extended, as in fxc's gather4 token
        // 0x8e00006d. The next two words are that extension, not operands.
        words.push_back(gather_opcode | (gather_length << 24) | 0x80000000u);
        words.push_back(0x800000c2u);
        words.push_back(0x00111103u);
        words.push_back(slot == 0 ? 0x00100012u : 0x00100022u);
        words.push_back(0);
        words.push_back(0x00004002u);
        if (slot == 0) {
            words.push_back(0x3f000000u);
            words.push_back(0x3f000000u);
        } else {
            words.push_back(0x3e800000u);
            words.push_back(0x3f400000u);
        }
        words.push_back(0);
        words.push_back(0);
        words.insert(words.end(), middle.begin(), middle.end());
        words.push_back(0x00107e46u);
        words.push_back(slot);
        words.push_back(sampler_operand);
        words.push_back(0);
        words.insert(words.end(), tail.begin(), tail.end());
        EXPECT_EQ(words.size() - before, static_cast<usize>(gather_length));
    };
    gather(0, 0x0010600au);
    if (sint_as_well) gather(1, 0x0010601au);
    words.push_back(62u | (1u << 24));
    words[1] = static_cast<u32>(words.size());
    for (u32 word : words) append_shader_word(container, word);
    return container;
}

// OpTypeInt width 32, then OpTypeImage DimCube of that type.
bool spirv_has_integer_cube_image(const std::vector<u32>& spirv, bool arrayed) {
    std::set<u32> ints;
    for (usize i = 5; i < spirv.size();) {
        const u32 words = spirv[i] >> 16;
        const u32 op = spirv[i] & 0xFFFFu;
        if (words == 0) break;
        if (op == 21u && words >= 4u && spirv[i + 2] == 32u) ints.insert(spirv[i + 1]);
        i += words;
    }
    for (usize i = 5; i < spirv.size();) {
        const u32 words = spirv[i] >> 16;
        const u32 op = spirv[i] & 0xFFFFu;
        if (words == 0) break;
        if (op == 25u && words >= 9u && spirv[i + 3] == 3u &&
            spirv[i + 5] == (arrayed ? 1u : 0u) && ints.contains(spirv[i + 2])) {
            return true;
        }
        i += words;
    }
    return false;
}

void expect_spirv_valid(const std::vector<u32>& spirv, const char* name) {
    if (std::system("command -v spirv-val >/dev/null 2>&1") != 0) return;
    const auto path = std::filesystem::temp_directory_path() / name;
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(spirv.data()),
                  static_cast<std::streamsize>(spirv.size() * 4));
    }
    const std::string command =
        "spirv-val --target-env spv1.6 '" + path.string() + "'";
    EXPECT_EQ(std::system(command.c_str()), 0) << name;
}

}  // namespace

// D3D11.3 7.18.9.1 / 7.18.12 and gather4 (sm5): TextureCube gather4 is a
// cube sample, including an integer one. The border blend is the 2D
// integer gather's lo + (hi - lo) * border, round-to-nearest-even. A form
// that cannot be named as a cube fails instead of gathering a 2D image.
// Gather is not a min/max reduction. The three UINT borders Metal applies
// have no twin; this shader's twin is the static-sampler push block.
TEST(DxbcSm51Test, IntegerCubeGatherAppliesBorder) {
    const auto shader = integer_gather_cs(/*TEXTURECUBE=*/6u, true,
                                           /*GATHER4=*/109u, 14u, {}, {});
    ASSERT_TRUE(shader.valid());
    DxbcTranslationOptions options{};
    options.border_twin_samplers = 1u;
    options.minmax_samplers = 1u;
    const auto r = translate_dxbc_to_spirv(shader, options);
    if (!r.valid()) {
        EXPECT_NE(r.failure.find(
                      "integer GATHER4 TextureCube cannot be expressed"),
                  std::string::npos)
            << r.failure;
        // No twin either: still not a 2D gather wearing the cube's name.
        const auto plain =
            translate_dxbc_to_spirv(shader, DxbcTranslationOptions{});
        ASSERT_FALSE(plain.valid());
        EXPECT_NE(plain.failure.find(
                      "integer GATHER4 TextureCube cannot be expressed"),
                  std::string::npos)
            << plain.failure;
        return;
    }

    EXPECT_TRUE(spirv_has_integer_cube_image(r.spirv, false));
    // Two gather4s, each through the real sampler and its white twin.
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageGather=*/96u), 4u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFSub=*/131u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFMul=*/133u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFAdd=*/129u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertUToF=*/112u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToU=*/109u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertSToF=*/111u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToS=*/110u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageSampleExplicitLod=*/88u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageDrefGather=*/97u), 0u);
    expect_spirv_valid(r.spirv, "vitreo_int_cube_gather.spv");

    MslCompileOptions msl{};
    msl.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        r.spirv.data(), r.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << compiled.msl_text();
    const std::string metal(compiled.msl_text());
    const bool weight = metal.find("fma(") != std::string::npos ||
                        metal.find(") * ") != std::string::npos;
    EXPECT_TRUE(weight) << metal;
    EXPECT_NE(metal.find("texturecube<"), std::string::npos) << metal;
    EXPECT_EQ(metal.find("texture2d"), std::string::npos) << metal;
    EXPECT_EQ(metal.find("texturecube_array"), std::string::npos) << metal;
    EXPECT_NE(metal.find("gather"), std::string::npos) << metal;

    // No twin: the gather stays on the sampler. A border the record does
    // not store is not invented.
    const auto plain = translate_dxbc_to_spirv(shader, DxbcTranslationOptions{});
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_TRUE(spirv_has_integer_cube_image(plain.spirv, false));
    EXPECT_EQ(count_opcode(plain.spirv, /*OpImageGather=*/96u), 2u);
    EXPECT_EQ(count_opcode(plain.spirv, /*OpFMul=*/133u), 0u);
    expect_spirv_valid(plain.spirv, "vitreo_int_cube_gather_plain.spv");
}

// TextureCubeArray is the same gather as TextureCube: a cube-array image,
// the border blend when a twin is present, and no 2D stand-in.
TEST(DxbcSm51Test, IntegerCubeArrayGatherAppliesBorder) {
    const auto shader = integer_gather_cs(/*TEXTURECUBEARRAY=*/10u, true,
                                           /*GATHER4=*/109u, 14u, {}, {});
    ASSERT_TRUE(shader.valid());
    DxbcTranslationOptions options{};
    options.border_twin_samplers = 1u;
    options.minmax_samplers = 1u;
    const auto r = translate_dxbc_to_spirv(shader, options);
    if (!r.valid()) {
        EXPECT_NE(r.failure.find(
                      "integer GATHER4 TextureCubeArray cannot be expressed"),
                  std::string::npos)
            << r.failure;
        const auto plain =
            translate_dxbc_to_spirv(shader, DxbcTranslationOptions{});
        ASSERT_FALSE(plain.valid());
        EXPECT_NE(plain.failure.find(
                      "integer GATHER4 TextureCubeArray cannot be expressed"),
                  std::string::npos)
            << plain.failure;
        return;
    }

    EXPECT_TRUE(spirv_has_integer_cube_image(r.spirv, true));
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageGather=*/96u), 4u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFSub=*/131u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFMul=*/133u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpFAdd=*/129u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertUToF=*/112u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToU=*/109u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertSToF=*/111u), 0u);
    EXPECT_GT(count_opcode(r.spirv, /*OpConvertFToS=*/110u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageSampleExplicitLod=*/88u), 0u);
    EXPECT_EQ(count_opcode(r.spirv, /*OpImageDrefGather=*/97u), 0u);
    expect_spirv_valid(r.spirv, "vitreo_int_cube_array_gather.spv");

    MslCompileOptions msl{};
    msl.argument_buffers = false;
    const auto compiled = compile_spirv_to_msl(
        r.spirv.data(), r.spirv.size(), ShaderStage::Compute, msl);
    ASSERT_TRUE(compiled.valid()) << compiled.msl_text();
    const std::string metal(compiled.msl_text());
    const bool weight = metal.find("fma(") != std::string::npos ||
                        metal.find(") * ") != std::string::npos;
    EXPECT_TRUE(weight) << metal;
    EXPECT_NE(metal.find("texturecube_array<"), std::string::npos) << metal;
    EXPECT_EQ(metal.find("texture2d"), std::string::npos) << metal;
    EXPECT_EQ(metal.find("texturecube<"), std::string::npos) << metal;
    EXPECT_NE(metal.find("gather"), std::string::npos) << metal;

    const auto plain = translate_dxbc_to_spirv(shader, DxbcTranslationOptions{});
    ASSERT_TRUE(plain.valid()) << plain.failure;
    EXPECT_TRUE(spirv_has_integer_cube_image(plain.spirv, true));
    EXPECT_EQ(count_opcode(plain.spirv, /*OpImageGather=*/96u), 2u);
    EXPECT_EQ(count_opcode(plain.spirv, /*OpFMul=*/133u), 0u);
    expect_spirv_valid(plain.spirv, "vitreo_int_cube_array_gather_plain.spv");
}

TEST(DxbcSm51Test, IntegerCubeGatherRejectedShapes) {
    const auto comparison = integer_gather_cs(
        6u, false, /*GATHER4_C=*/126u, 16u, {},
        {0x00004001u, 0x3f000000u});
    const auto compared =
        translate_dxbc_to_spirv(comparison, DxbcTranslationOptions{});
    ASSERT_FALSE(compared.valid());
    EXPECT_NE(compared.failure.find(
                  "GATHER4_C: integer textures have no comparison"),
              std::string::npos)
        << compared.failure;

    const auto offset = integer_gather_cs(
        6u, false, /*GATHER4_PO=*/127u, 19u,
        {0x00004002u, 0x3f800000u, 0u, 0u, 0u}, {});
    const auto programmed =
        translate_dxbc_to_spirv(offset, DxbcTranslationOptions{});
    ASSERT_FALSE(programmed.valid());
    EXPECT_NE(programmed.failure.find(
                  "GATHER4_PO: only 2D textures take an offset"),
              std::string::npos)
        << programmed.failure;

    const auto tex1d = integer_gather_cs(/*TEXTURE1D=*/2u, false, 109u, 14u, {}, {});
    const auto one_d = translate_dxbc_to_spirv(tex1d, DxbcTranslationOptions{});
    ASSERT_FALSE(one_d.valid());
    EXPECT_NE(one_d.failure.find("GATHER4 does not support Texture1D"),
              std::string::npos)
        << one_d.failure;

    const auto tex3d = integer_gather_cs(/*TEXTURE3D=*/5u, false, 109u, 14u, {}, {});
    const auto volume = translate_dxbc_to_spirv(tex3d, DxbcTranslationOptions{});
    ASSERT_FALSE(volume.valid());
    EXPECT_NE(volume.failure.find("GATHER4 does not support Texture3D"),
              std::string::npos)
        << volume.failure;
}

// Multiple producers may publish different sampler kinds concurrently. Each
// bit has one first producer and neither producer can erase the other's bit.
TEST(SamplerFeatureTest, ConcurrentEnablesAreMonotonic) {
    const auto before = enabled_sampler_features();
    const std::array<SamplerFeature, 3> features{
        SamplerFeature::MinMax, SamplerFeature::CustomBorder, SamplerFeature::NonNormalized};
    std::array<std::atomic<unsigned>, 3> first{};
    std::vector<std::thread> producers;
    for (unsigned i = 0; i < 9; ++i) {
        producers.emplace_back([&, i] {
            const auto slot = i % features.size();
            if (enable_sampler_feature(features[slot])) first[slot].fetch_add(1);
        });
    }
    for (auto& producer : producers) producer.join();
    unsigned expected = before;
    for (unsigned i = 0; i < features.size(); ++i) {
        const auto bit = static_cast<unsigned>(features[i]);
        EXPECT_EQ(first[i].load(), (before & bit) ? 0u : 1u);
        expected |= bit;
        EXPECT_FALSE(enable_sampler_feature(features[i]));
    }
    EXPECT_EQ(enabled_sampler_features(), expected);
}

TEST(DxbcSm51Test, SamplerFlagsExposeBothFeatureCompileSwitches) {
    const auto c = parse_blob(sm51_cs_bias, sizeof(sm51_cs_bias));
    auto options = sm51_options(c);
    options.sampler_state_buffer = {true, 0, 200};
    const auto spirv = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    const auto msl = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                         ShaderStage::Compute);
    ASSERT_TRUE(msl.valid()) << msl.failure;
    const std::string source = msl.msl_text();
    EXPECT_NE(source.find("#define VITREO_CUSTOM_BORDER 0"), std::string::npos);
    EXPECT_NE(source.find("#define VITREO_NON_NORMALIZED 0"), std::string::npos);
    EXPECT_EQ(source.find("[[function_constant(" + std::to_string(kCustomBorderSpecId) + ")]]"),
              std::string::npos);
    EXPECT_EQ(source.find("[[function_constant(" + std::to_string(kNonNormalizedSpecId) + ")]]"),
              std::string::npos);
}

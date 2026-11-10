// SPDX-License-Identifier: LGPL-2.1-only
//
// D3D12 mesh and amplification shaders (SM 6.5): the DXIL lowering
// (dxil_lower_mesh.inc), the translator (dxbc_to_spirv_mesh.cpp) and the MSL
// mesh compiler (mesh_stage.cpp), against dxc 1.6 output compiled on
// winoracle-gpu (tools/oracle_probe/d3d12_mesh/gen_d3d12_mesh.py). Expected
// state values are the dxc listings' (tests/oracle_data/d3d12_mesh/*.txt):
// the kDxilMSStateTag / kDxilASStateTag entry properties and the signatures.

#include <gtest/gtest.h>

#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"
#include "graphics/shaders/dxil_to_dxbc.hpp"
#include "graphics/shaders/mesh_stage.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include "../oracle_data/d3d12_mesh/d3d12_mesh_shaders.h"
// tools/oracle_probe/d3d12_mesh2 (the a42 probe's shaders, dxc 1.6 on
// winoracle-gpu).
#include "../oracle_data/d3d12_mesh2/d3d12_mesh2_shaders.h"

#include <string>

namespace vitreo::graphics {
namespace {

DxbcContainer lower_blob(const char* name, const unsigned char* code, unsigned size) {
    const DxilToDxbcResult r = dxil_to_dxbc(code, size);
    EXPECT_TRUE(r.ok) << name << ": " << r.failure;
    if (!r.ok) return {};
    EXPECT_TRUE(dxbc_container_verified(r.container.data(), r.container.size())) << name;
    return parse_dxbc(r.container.data(), r.container.size());
}

DxbcContainer lowered(const char* name) {
    for (const auto& b : kMeshBlobs) {
        if (std::string(b.name) == name) return lower_blob(name, b.code, b.size);
    }
    for (const auto& b : kMesh2Blobs) {
        if (std::string(b.name) == name) return lower_blob(name, b.code, b.size);
    }
    ADD_FAILURE() << "no blob " << name;
    return {};
}

struct Expected {
    const char* name;
    u32 kind, x, y, z, topology, vertices, primitives, payload_bytes, payload_words;
};

// ms_payload.dxil.txt: !{!25 = {4,1,1}, i32 4, i32 2, i32 2, i32 40};
// as_rows: !{{8,1,1}, i32 40}; ms_lines: {3,1,1}, 6, 3, 1, 0; ms_big:
// {128,1,1}, 256, 256, 2, 0. Payload words: struct Payload { uint[8]; uint;
// float } is ten 32-bit scalars.
constexpr Expected kStates[] = {
    {"ms_quad", DxbcMeshState::Mesh, 2, 2, 1, 2, 4, 2, 0, 0},
    {"ms_payload", DxbcMeshState::Mesh, 4, 1, 1, 2, 4, 2, 40, 10},
    {"as_rows", DxbcMeshState::Amplification, 8, 1, 1, 0, 0, 0, 40, 10},
    {"ms_lines", DxbcMeshState::Mesh, 3, 1, 1, 1, 6, 3, 0, 0},
    {"ms_big", DxbcMeshState::Mesh, 128, 1, 1, 2, 256, 256, 0, 0},
};

TEST(DxilMesh, LowersToAComputeProgramWithTheMeshState) {
    for (const Expected& e : kStates) {
        const DxbcContainer c = lowered(e.name);
        ASSERT_TRUE(c.valid()) << e.name;
        EXPECT_EQ(c.stage, ShaderStage::Compute) << e.name;
        EXPECT_EQ(c.mesh.kind, e.kind) << e.name;
        EXPECT_EQ(c.mesh.threads[0], e.x) << e.name;
        EXPECT_EQ(c.mesh.threads[1], e.y) << e.name;
        EXPECT_EQ(c.mesh.threads[2], e.z) << e.name;
        EXPECT_EQ(c.mesh.topology, e.topology) << e.name;
        EXPECT_EQ(c.mesh.max_vertices, e.vertices) << e.name;
        EXPECT_EQ(c.mesh.max_primitives, e.primitives) << e.name;
        EXPECT_EQ(c.mesh.payload_bytes, e.payload_bytes) << e.name;
        EXPECT_EQ(c.mesh.payload_words, e.payload_words) << e.name;
    }
    // ms_quad's vertex outputs (OSG1) and primitive outputs (PSG1).
    const DxbcContainer q = lowered("ms_quad");
    ASSERT_EQ(q.output_signature.size(), 3u);
    EXPECT_EQ(q.output_signature[0].system_value_type, 1u);  // SV_Position
    EXPECT_EQ(q.output_signature[2].semantic_name, "TEXCOORD");
    EXPECT_EQ(q.output_signature[2].semantic_index, 5u);
    ASSERT_EQ(q.patch_constant_signature.size(), 2u);
    EXPECT_EQ(q.patch_constant_signature[0].semantic_name, "PRIMCOLOR");
    EXPECT_EQ(q.patch_constant_signature[1].system_value_type, 25u);  // D3D_NAME_CULLPRIMITIVE
}

struct Built {
    MeshStageLayout layout;
    std::string msl;
};

Built build(const char* name, const char* ps, u32 payload_words, bool device_staging = false) {
    Built out;
    const DxbcContainer c = lowered(name);
    if (!c.valid()) return out;
    if (c.mesh.is_mesh()) {
        const DxbcContainer p = ps != nullptr ? lowered(ps) : DxbcContainer{};
        std::string why;
        bool invalid = false;
        EXPECT_TRUE(build_mesh_stage_layout(c, ps != nullptr ? &p.input_signature : nullptr, out.layout, why,
                                            invalid))
            << name << ": " << why;
    } else {
        out.layout = amplification_stage_layout(c);
    }
    out.layout.device_staging = device_staging;
    DxbcTranslationOptions options{};
    options.mesh_layout = &out.layout;
    const auto spirv = translate_dxbc_to_spirv(c, options);
    EXPECT_TRUE(spirv.valid()) << name << ": " << spirv.failure;
    if (!spirv.valid()) return out;
    EXPECT_EQ(spirv.mesh_stage, c.mesh.kind) << name;
    MslCompileOptions msl{};
    msl.mesh_stage = &out.layout;
    msl.mesh_payload_words = payload_words;
    const auto compiled = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(),
                                               c.mesh.is_mesh() ? ShaderStage::Mesh : ShaderStage::Amplification,
                                               msl);
    EXPECT_TRUE(compiled.valid()) << name;
    out.msl = compiled.msl_text();
    return out;
}

TEST(DxilMesh, StagesWhatThePixelShaderReadsAndWritesAMeshFunction) {
    const Built q = build("ms_quad", "ps_color", 0);
    ASSERT_FALSE(q.msl.empty());
    EXPECT_NE(q.msl.find("[[mesh]] void main0(vitreo_ms_mesh vitreo_mesh, "), std::string::npos) << q.msl;
    EXPECT_NE(q.msl.find("metal::mesh<vitreo_ms_vertex, vitreo_ms_primitive, 4, 2, metal::topology::triangle>"),
              std::string::npos);
    // COLOR0 per vertex and PRIMCOLOR per primitive, at the pixel shader's
    // input registers (v1, v2); TEXCOORD5 is read by nothing, not staged.
    ASSERT_EQ(q.layout.fields.size(), 2u);
    EXPECT_FALSE(q.layout.fields[0].per_primitive);
    EXPECT_TRUE(q.layout.fields[1].per_primitive);
    EXPECT_EQ(q.layout.vertex_scalars, 4u + 4u);
    EXPECT_EQ(q.layout.primitive_scalars, 4u + 1u);  // PRIMCOLOR + SV_CullPrimitive
    EXPECT_NE(q.layout.cull_primitive_slot, kMeshNoSlot);
    for (u32 c = 0; c < 4; ++c) EXPECT_EQ(q.layout.vertex_slot[2 * 4 + c], kMeshNoSlot);

    const Built a = build("as_rows", nullptr, 10);
    ASSERT_FALSE(a.msl.empty());
    EXPECT_NE(a.msl.find("[[object]] void main0(object_data vitreo_mesh_payload& vitreo_payload [[payload]], "
                         "mesh_grid_properties vitreo_grid, "),
              std::string::npos)
        << a.msl;
    EXPECT_NE(a.msl.find("set_threadgroups_per_grid"), std::string::npos);

    const Built m = build("ms_payload", "ps_color", 10);
    EXPECT_NE(m.msl.find("const object_data vitreo_mesh_payload& vitreo_payload [[payload]]"), std::string::npos)
        << m.msl;
    // The payload is read where Metal keeps it (mesh_stage_direct_payload):
    // the helper takes object_data, no threadgroup copy is made.
    EXPECT_NE(m.msl.find("const object_data uint (&vitreo_ms_payload)["), std::string::npos)
        << m.msl;
    EXPECT_NE(m.msl.find("vitreo_payload.w"), std::string::npos);
    EXPECT_EQ(m.msl.find("threadgroup uint vitreo_ms_payload["), std::string::npos);
    EXPECT_EQ(m.msl.find("vitreo_ms_payload[i] = vitreo_payload.w[i]"), std::string::npos);

    const Built l = build("ms_lines", nullptr, 0);
    EXPECT_NE(l.msl.find("metal::topology::line"), std::string::npos) << l.msl;
    const Built big = build("ms_big", "ps_vcolor", 0);
    EXPECT_EQ(big.layout.vertex_scalars, 8u);  // SV_Position + COLOR0 of 8 rows
    EXPECT_FALSE(big.msl.empty());
}

TEST(DxilMesh, PayloadsMoveAsTheirByteImage) {
    // d3d12_mesh2.hlsl R16 {bool flag; uint16_t h0, h1; uint tail;} is 12
    // bytes (flag 0, h0 4, h1 6, tail 8: DXIL's data layout), four
    // register-model scalars; R12 {uint a, b, c;} the same 12 bytes. Both
    // move as 3 words, so the pair links (S_OK on the RTX 4080, a42
    // "payload").
    const DxbcContainer as = lowered("as_r16");
    const DxbcContainer r16 = lowered("ms_r16");
    const DxbcContainer r12 = lowered("ms_r12");
    EXPECT_EQ(as.mesh.payload_bytes, 12u);
    EXPECT_EQ(as.mesh.payload_words, 3u);
    EXPECT_EQ(r16.mesh.payload_words, 3u);
    EXPECT_EQ(r12.mesh.payload_words, 3u);
    const std::vector<u32> layout = {0u, 4u | (kDxbcPayloadU16 << 16), 6u | (kDxbcPayloadU16 << 16), 8u};
    EXPECT_EQ(as.mesh.payload_layout, layout);
    // A 32-bit payload keeps the identity layout (no table).
    EXPECT_TRUE(lowered("as_p32").mesh.payload_layout.empty());
    // The object function packs h0 | h1 << 16 into word 1.
    const Built a = build("as_r16", nullptr, 3);
    EXPECT_NE(a.msl.find("& 0xFFFFu) << 16u)"), std::string::npos) << a.msl;
    const Built m = build("ms_r16", "ps_word", 3);
    EXPECT_FALSE(m.msl.empty());
}

TEST(DxilMesh, OutputsPastThreadgroupMemoryAreStagedInDeviceMemory) {
    // ms_bigtg2: 28 KB of groupshared memory (the MS limit) and 32 KB of
    // outputs: 20 + 12 KB staged past Metal's 32 KB of threadgroup memory.
    const Built big = build("ms_bigtg2", "ps_big2", 0);
    EXPECT_GT(mesh_stage_threadgroup_bytes(big.layout, false) + lowered("ms_bigtg2").mesh.groupshared_bytes,
              32768u);
    const Built d = build("ms_bigtg2", "ps_big2", 0, true);
    ASSERT_FALSE(d.msl.empty());
    EXPECT_EQ(mesh_stage_threadgroup_bytes(d.layout, false), 0u);
    EXPECT_NE(d.msl.find("mem_flags::mem_device"), std::string::npos);
    EXPECT_EQ(d.msl.find("threadgroup uint vitreo_ms_vertices["), std::string::npos) << d.msl;
    EXPECT_NE(d.msl.find("device uint* vitreo_ms_vertices = vitreo_ms_scratch + "), std::string::npos) << d.msl;
    EXPECT_NE(d.msl.find("device uint* vitreo_ms_scratch [[buffer("), std::string::npos);
    // 256 vertices x 20 words, 128 primitives x 24, 384 indices.
    EXPECT_EQ(d.layout.device_staging_words(), 256u * 20u + 128u * 24u + 384u + 4u);
    // Behind an amplification shader the payload carries nothing extra (a
    // full 16 KB one has no room): the mesh group takes its slot from the
    // chunk's counter.
    const Built a = build("as_big", nullptr, 3072, true);
    EXPECT_EQ(a.msl.find("vitreo_base"), std::string::npos) << a.msl;
    const Built m = build("ms_bigtg", "ps_big", 3072, true);
    EXPECT_NE(m.msl.find("const uint vitreo_slot = vitreo_slot_shared;"), std::string::npos) << m.msl;
    EXPECT_NE(m.msl.find("vitreo_ms_scratch + vitreo_slot * "), std::string::npos) << m.msl;
    EXPECT_EQ(m.msl.find("vitreo_base"), std::string::npos) << m.msl;
    const Built full = build("ms_full", "ps_big", 4096, true);
    EXPECT_NE(full.msl.find("vitreo_slot_shared"), std::string::npos) << full.msl;
    // ms_bigtg2 reads no SV_GroupID: Vitreo's own group position stays.
    EXPECT_NE(d.msl.find("uint3 vitreo_tg [[threadgroup_position_in_grid]]"), std::string::npos) << d.msl;
}

TEST(DxilMesh, RasterCaptureAppendsSurvivingPrimitives) {
    // Conservative / target-independent rasterization: nothing is drawn,
    // the records go to the capture through one atomic add per group.
    const DxbcContainer c = lowered("ms_cr");
    const DxbcContainer p = lowered("ps_white");
    ASSERT_TRUE(c.valid() && p.valid());
    MeshStageLayout layout;
    std::string why;
    bool invalid = false;
    ASSERT_TRUE(build_mesh_stage_layout(c, &p.input_signature, layout, why, invalid)) << why;
    ASSERT_TRUE(mesh_stage_capture_layout(layout, why)) << why;
    ASSERT_EQ(layout.capture.size(), 1u);  // SV_Position only
    EXPECT_TRUE(layout.capture[0].position);
    DxbcTranslationOptions options{};
    options.mesh_layout = &layout;
    const auto spirv = translate_dxbc_to_spirv(c, options);
    ASSERT_TRUE(spirv.valid()) << spirv.failure;
    MslCompileOptions msl{};
    msl.mesh_stage = &layout;
    const auto compiled = compile_spirv_to_msl(spirv.spirv.data(), spirv.spirv.size(), ShaderStage::Mesh, msl);
    ASSERT_TRUE(compiled.valid());
    const std::string text = compiled.msl_text();
    EXPECT_NE(text.find("vitreo_mesh.set_primitive_count(0u);"), std::string::npos) << text;
    EXPECT_NE(text.find("atomic_fetch_add_explicit((device atomic_uint*)vitreo_cap"), std::string::npos) << text;
    EXPECT_EQ(text.find("vitreo_mesh.set_primitive(p, q);"), std::string::npos) << text;
}

TEST(DxilMesh, LinkageByName) {
    // ps_color reads COLOR0 and PRIMCOLOR, which ms_lines does not write:
    // D3D12 refuses the pair (Signature Linkage).
    const DxbcContainer ms = lowered("ms_lines");
    const DxbcContainer ps = lowered("ps_color");
    MeshStageLayout l;
    std::string why;
    bool invalid = false;
    EXPECT_FALSE(build_mesh_stage_layout(ms, &ps.input_signature, l, why, invalid));
    EXPECT_TRUE(invalid) << why;
}

}  // namespace
}  // namespace vitreo::graphics

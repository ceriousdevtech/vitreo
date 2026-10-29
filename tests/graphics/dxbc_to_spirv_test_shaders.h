// SPDX-License-Identifier: LGPL-2.1-only
/// @file dxbc_to_spirv_test_shaders.h
/// @brief Hand-assembled DXBC builders shared by the test_dxbc_to_spirv*.cpp suites.
#pragma once

#include "graphics/shaders/dxbc_parser.hpp"
#include "graphics/shaders/dxbc_to_spirv.hpp"

#include <cstring>
#include <vector>

namespace vitreo::graphics::dxbc_spirv_test {

/// Create a minimal DXBC container for testing.
/// This creates a DXBC blob with the required chunks for a simple VS that
/// outputs position from input position: `mov o0, v0`.
inline std::vector<u8> create_minimal_vertex_shader_dxbc() {
    std::vector<u8> blob;

    // DXBC header
    // Magic: "DXBC" (0x43425844)
    const u8 magic[] = {0x44, 0x58, 0x42, 0x43};
    blob.insert(blob.end(), magic, magic + 4);

    // Checksum (4 DWORDs) - zeros for test
    for (int i = 0; i < 16; ++i) blob.push_back(0);

    // One (always 1)
    const u8 one[] = {0x01, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), one, one + 4);

    // Total size - will be updated at end
    usize total_size_offset = blob.size();
    blob.insert(blob.end(), 4, 0);

    // Chunk count (3: ISGN, OSGN, SHDR)
    const u8 chunk_count[] = {0x03, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), chunk_count, chunk_count + 4);

    // Chunk offset table (relative to start of blob)
    usize offset_table_start = blob.size();
    blob.insert(blob.end(), 12, 0); // 3 offsets, filled in later

    std::vector<u32> chunk_offsets;

    // === ISGN Chunk (Input Signature) ===
    chunk_offsets.push_back(static_cast<u32>(blob.size()));

    // Chunk header: FourCC "ISGN" (0x4E475349), size
    const u8 isgn_fourcc[] = {0x49, 0x53, 0x47, 0x4E};
    blob.insert(blob.end(), isgn_fourcc, isgn_fourcc + 4);

    usize isgn_size_offset = blob.size();
    blob.insert(blob.end(), 4, 0); // Size filled later

    usize isgn_data_start = blob.size();
    
    // ISGN data: 1 element, offset 8 for signature table
    const u8 isgn_count[] = {0x01, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), isgn_count, isgn_count + 4);
    const u8 isgn_offset[] = {0x08, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), isgn_offset, isgn_offset + 4);
    
    // Element 0: POSITION0 at register 0
    // Name offset relative to ISGN chunk data start (after FourCC+size).
    // Layout: count(4)+unk(4)+element(24) => strings start at 32.
    u32 name_offset = 32;
    const u8 name_off_bytes[] = {
        static_cast<u8>(name_offset & 0xFF),
        static_cast<u8>((name_offset >> 8) & 0xFF),
        static_cast<u8>((name_offset >> 16) & 0xFF),
        static_cast<u8>((name_offset >> 24) & 0xFF)
    };
    blob.insert(blob.end(), name_off_bytes, name_off_bytes + 4);
    
    // Semantic index: 0
    blob.insert(blob.end(), 4, 0);
    
    // System value type: 0 (none)
    blob.insert(blob.end(), 4, 0);
    
    // Component type: 3 (float)
    const u8 comp_type[] = {0x03, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), comp_type, comp_type + 4);
    
    // Register: 0
    blob.insert(blob.end(), 4, 0);
    
    // Component mask: 0x0F (xyzw)
    const u8 comp_mask[] = {0x0F, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), comp_mask, comp_mask + 4);
    
    // Semantic name: "POSITION\0"
    const char pos_name[] = "POSITION";
    blob.insert(blob.end(), pos_name, pos_name + 9);
    
    // Align to 4 bytes
    while ((blob.size() - isgn_data_start) % 4 != 0) blob.push_back(0);
    
    u32 isgn_size = static_cast<u32>(blob.size() - isgn_data_start);
    std::memcpy(&blob[isgn_size_offset], &isgn_size, 4);
    
    // === OSGN Chunk (Output Signature) ===
    chunk_offsets.push_back(static_cast<u32>(blob.size()));
    
    // Chunk header: FourCC "OSGN" (0x4E47534F), size
    const u8 osgn_fourcc[] = {0x4F, 0x53, 0x47, 0x4E};
    blob.insert(blob.end(), osgn_fourcc, osgn_fourcc + 4);
    
    usize osgn_size_offset = blob.size();
    blob.insert(blob.end(), 4, 0);
    
    usize osgn_data_start = blob.size();
    
    // OSGN data: 1 element
    const u8 osgn_count[] = {0x01, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), osgn_count, osgn_count + 4);
    blob.insert(blob.end(), isgn_offset, isgn_offset + 4);
    
    // Element 0: SV_Position at register 0
    blob.insert(blob.end(), name_off_bytes, name_off_bytes + 4);
    blob.insert(blob.end(), 4, 0); // semantic index 0
    blob.insert(blob.end(), 4, 0); // system value type
    blob.insert(blob.end(), comp_type, comp_type + 4);
    blob.insert(blob.end(), 4, 0); // register 0
    blob.insert(blob.end(), comp_mask, comp_mask + 4);
    
    const char sv_pos[] = "SV_Position";
    blob.insert(blob.end(), sv_pos, sv_pos + 12);
    
    while ((blob.size() - osgn_data_start) % 4 != 0) blob.push_back(0);
    
    u32 osgn_size = static_cast<u32>(blob.size() - osgn_data_start);
    std::memcpy(&blob[osgn_size_offset], &osgn_size, 4);
    
    // === SHDR Chunk (Shader Bytecode) ===
    chunk_offsets.push_back(static_cast<u32>(blob.size()));
    
    // Chunk header: FourCC "SHDR" (0x52444853), size
    const u8 shdr_fourcc[] = {0x53, 0x48, 0x44, 0x52};
    blob.insert(blob.end(), shdr_fourcc, shdr_fourcc + 4);
    
    usize shdr_size_offset = blob.size();
    blob.insert(blob.end(), 4, 0);
    
    usize shdr_data_start = blob.size();
    
    // Version token: SM4.0 vertex shader
    // Bits [15:0] = 0x0001 (VS), bits [7:4] = major (4), bits [3:0] = minor (0)
    // Full token: (1 << 16) | (4 << 4) | 0 = 0x00010040
    const u8 version[] = {0x40, 0x00, 0x01, 0x00};
    blob.insert(blob.end(), version, version + 4);
    
    // Length in DWORDs (excluding version token): mov(5)+ret(1)=6
    const u8 length[] = {0x06, 0x00, 0x00, 0x00};
    blob.insert(blob.end(), length, length + 4);
    
    // Instruction: mov o0, v0
    // Opcode token: opcode=54 (MOV), length=5 (opcode+dst+idx+src+idx)
    u32 mov_opcode = 54 | (5 << 24);
    blob.insert(blob.end(), reinterpret_cast<u8*>(&mov_opcode),
                reinterpret_cast<u8*>(&mov_opcode) + 4);
    
    // Destination: output o0, 4-comp mask .xyzw, 1D immediate index
    // type=2 at [19:12], numcomp=3, mask sel=0, mask=0xF, index_dim=1
    u32 dst_operand = (2u << 12) | 3u | (0xFu << 4) | (1u << 20);
    blob.insert(blob.end(), reinterpret_cast<u8*>(&dst_operand),
                reinterpret_cast<u8*>(&dst_operand) + 4);
    u32 dst_index = 0;
    blob.insert(blob.end(), reinterpret_cast<u8*>(&dst_index),
                reinterpret_cast<u8*>(&dst_index) + 4);
    
    // Source: input v0, swizzle .xyzw, 1D immediate index
    u32 src_operand =
        (1u << 12) | 3u | (1u << 2) | (0xE4u << 4) | (1u << 20);
    blob.insert(blob.end(), reinterpret_cast<u8*>(&src_operand),
                reinterpret_cast<u8*>(&src_operand) + 4);
    u32 src_index = 0;
    blob.insert(blob.end(), reinterpret_cast<u8*>(&src_index),
                reinterpret_cast<u8*>(&src_index) + 4);
    
    // Instruction: ret
    u32 ret_opcode = 62 | (1 << 24);
    blob.insert(blob.end(), reinterpret_cast<u8*>(&ret_opcode),
                reinterpret_cast<u8*>(&ret_opcode) + 4);
    
    u32 shdr_size = static_cast<u32>(blob.size() - shdr_data_start);
    std::memcpy(&blob[shdr_size_offset], &shdr_size, 4);
    
    // Update chunk offset table
    std::memcpy(&blob[offset_table_start], chunk_offsets.data(), 12);
    
    // Update total size
    u32 total_size = static_cast<u32>(blob.size());
    std::memcpy(&blob[total_size_offset], &total_size, 4);
    
    return blob;
}

inline DxbcContainer create_binary_vertex_shader(u32 opcode) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.input_signature.push_back({"TEXCOORD", 0, 1, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    const u32 dst =
        (2u << 12) | 3u | (0xFu << 4) | (1u << 20); // o0.xyzw
    const u32 src =
        (1u << 12) | 3u | (1u << 2) | (0xE4u << 4) | (1u << 20);

    append_word(0x00010040u); // vs_4_0
    append_word(10u);         // total DWORD count
    append_word(opcode | (7u << 24));
    append_word(dst);
    append_word(0u);
    append_word(src);
    append_word(0u); // v0.xyzw
    append_word(src);
    append_word(1u); // v1.xyzw
    append_word(62u | (1u << 24)); // ret
    return container;
}

inline DxbcContainer create_unary_vertex_shader(u32 opcode) {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Vertex;
    container.input_signature.push_back({"POSITION", 0, 0, 0xFu, 3});
    container.output_signature.push_back({"SV_Position", 0, 0, 0xFu, 3});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    append_word(0x00010040u); // vs_4_0
    append_word(8u);
    append_word(opcode | (5u << 24));
    append_word(0x001020F2u); append_word(0u); // o0.xyzw
    append_word(0x001010E4u); append_word(0u); // v0.xyzw
    append_word(62u | (1u << 24)); // ret
    return container;
}

inline DxbcContainer create_fxc_point_geometry_shader() {
    DxbcContainer container{};
    container.shader_model_major = 4;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Geometry;
    container.input_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.input_signature.push_back({"COLOR", 0, 1, 0xFu, 3});
    container.output_signature.push_back(
        {"SV_Position", 0, 0, 0xFu, 3, 1u});
    container.output_signature.push_back({"COLOR", 0, 1, 0xFu, 3});

    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };
    // Exact SHDR token stream from Microsoft D3DCompiler 47 gs_4_0 output:
    // point input, PointStream output, maxvertexcount(1), copy both inputs,
    // Append(), RestartStrip(). Full DXBC SHA-256:
    // 29ac21313a5e098a5c4a78403e2fbf6bf0357d17e44ac6f2370fef5b4687c530.
    const u32 words[] = {
        0x00020040u, 0x00000025u,
        0x05000061u, 0x002010F2u, 1u, 0u, 1u,
        0x0400005Fu, 0x002010F2u, 1u, 1u,
        0x0100085Du, // dcl_inputPrimitive point
        0x0100085Cu, // dcl_outputTopology pointlist
        0x04000067u, 0x001020F2u, 0u, 1u,
        0x03000065u, 0x001020F2u, 1u,
        0x0200005Eu, 1u, // dcl_maxOutputVertexCount 1
        0x06000036u, 0x001020F2u, 0u,
        0x00201E46u, 0u, 0u,
        0x06000036u, 0x001020F2u, 1u,
        0x00201E46u, 0u, 1u,
        0x01000013u, // emit
        0x01000009u, // cut
        0x0100003Eu, // ret
    };
    for (const u32 word : words) append_word(word);
    return container;
}

inline DxbcContainer create_synthetic_typed_uav_store_compute_shader(
    u32 resource_dimension = 3u, u32 packed_return_type = 0x00005555u) {
    DxbcContainer container{};
    container.shader_model_major = 5;
    container.shader_model_minor = 0;
    container.stage = ShaderStage::Compute;
    const auto append_word = [&](u32 word) {
        const auto* bytes = reinterpret_cast<const u8*>(&word);
        container.bytecode.insert(container.bytecode.end(), bytes, bytes + 4);
    };

    // Synthetic cs_5_0 token stream assembled strictly from Microsoft's
    // published d3d11TokenizedProgramFormat.hpp ABI. Native FXC proof remains
    // pending; this isolates dcl_uav_typed u0 (Texture2D<float4>) and
    // store_uav_typed u0.xyzw, vThreadID.xy, l(1,0,1,1).
    append_word(0x00050050u); // cs_5_0
    append_word(21u);
    append_word(0x0100086Au); // dcl_globalFlags refactoringAllowed
    append_word(0x0400009Bu); append_word(1u); append_word(1u);
    append_word(1u); // dcl_thread_group 1, 1, 1
    append_word(0x0400009Cu | (resource_dimension << 11));
    append_word(0x0011E000u); append_word(0u); // dcl_uav_typed u0
    append_word(packed_return_type);
    append_word(0x090000A4u); // store_uav_typed
    append_word(0x0011E0F2u); append_word(0u); // u0.xyzw
    append_word(0x00020E46u); // vThreadID.xyzw
    append_word(0x00004002u); // l(1.0, 0.0, 1.0, 1.0)
    append_word(0x3F800000u); append_word(0u);
    append_word(0x3F800000u); append_word(0x3F800000u);
    append_word(0x0100003Eu); // ret
    return container;
}

}  // namespace vitreo::graphics::dxbc_spirv_test

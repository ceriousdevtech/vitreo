// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_parser.cpp
/// @brief DXBC shader bytecode container parser implementation.

#include "dxbc_parser.hpp"
#include <vitreo/common.hpp>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstring>

namespace vitreo::graphics {

namespace {

// DXBC container header: "DXBC" + checksum[4*u32] + one + total_size + chunk_count
constexpr u32 DXBC_MAGIC = 0x43425844; // "DXBC" as little-endian u32

struct DxbcHeader {
    u32 magic;
    u32 checksum[4];
    u32 one;            // Always 1
    u32 total_size;
    u32 chunk_count;
};

struct ChunkHeader {
    u32 fourcc;
    u32 size;
};

// SM4/5 program type (bits 16-31 of first DWORD in SHDR/SHEX)
constexpr u32 D3D10_SB_PIXEL_SHADER    = 0;
constexpr u32 D3D10_SB_VERTEX_SHADER   = 1;
constexpr u32 D3D10_SB_GEOMETRY_SHADER  = 2;
constexpr u32 D3D11_SB_HULL_SHADER     = 3;
constexpr u32 D3D11_SB_DOMAIN_SHADER   = 4;
constexpr u32 D3D11_SB_COMPUTE_SHADER  = 5;

// Safe u32 read
u32 read_u32(const u8* data, usize offset, usize max_size) {
    if (offset + 4 > max_size) return 0;
    u32 value;
    std::memcpy(&value, data + offset, sizeof(u32));
    return value;
}

// Parse Input/Output Signature chunk (ISGN / OSGN, and OSG5 whose elements
// carry a leading stream index).
void parse_signature_chunk(
    const u8* chunk_data, u32 chunk_size,
    std::vector<DxbcSignatureElement>& out, bool with_stream = false,
    bool with_min_precision = false)
{
    if (chunk_size < 8) return;

    u32 element_count = read_u32(chunk_data, 0, chunk_size);
    // u32 unknown = read_u32(chunk_data, 4, chunk_size); // Always 8

    // ISG1-style elements add a trailing min-precision word; nothing here
    // consumes it yet, so it is only stepped over.
    const usize element_size =
        (with_stream ? 28u : 24u) + (with_min_precision ? 4u : 0u);
    usize offset = 8;
    for (u32 i = 0; i < element_count && offset + element_size <= chunk_size;
         ++i) {
        DxbcSignatureElement elem{};
        if (with_stream) {
            elem.stream = read_u32(chunk_data, offset, chunk_size);
            offset += 4;
        }
        u32 name_offset = read_u32(chunk_data, offset, chunk_size);
        elem.semantic_index = read_u32(chunk_data, offset + 4, chunk_size);
        elem.system_value_type =
            read_u32(chunk_data, offset + 8, chunk_size);
        elem.component_type = static_cast<u8>(
            read_u32(chunk_data, offset + 12, chunk_size));
        elem.register_num = read_u32(chunk_data, offset + 16, chunk_size);
        elem.component_mask = static_cast<u8>(
            read_u32(chunk_data, offset + 20, chunk_size) & 0xFF);

        // Read semantic name string
        if (name_offset < chunk_size) {
            auto* str = reinterpret_cast<const char*>(chunk_data + name_offset);
            usize max_len = chunk_size - name_offset;
            usize len = 0;
            while (len < max_len && str[len] != '\0') ++len;
            elem.semantic_name.assign(str, len);
        }

        out.push_back(std::move(elem));
        offset += 24 + (with_min_precision ? 4u : 0u);
    }
}

// Parse RDEF (Resource Definition) chunk
void parse_rdef_chunk(
    const u8* chunk_data, u32 chunk_size,
    std::vector<DxbcResourceBinding>& bindings,
    std::vector<DxbcConstantBuffer>& cbufs)
{
    if (chunk_size < 28) return;

    u32 cbuf_count = read_u32(chunk_data, 0, chunk_size);
    u32 cbuf_offset = read_u32(chunk_data, 4, chunk_size);
    u32 binding_count = read_u32(chunk_data, 8, chunk_size);
    u32 binding_offset = read_u32(chunk_data, 12, chunk_size);

    // Target 5.0+ reflection carries a header at byte 28 -- a magic, then its
    // own record sizes (header, cbuffer, binding, variable, type, member).
    // The binding record is 32 bytes up to 5.0 and 40 in 5.1, which appends
    // {space, range ID}. Measured, fxc 10.1 (tools/oracle_probe/d3d12_sm51,
    // cs_space0 built both ways): cs_5_0 "RD11" (0x31314452) 3c 18 20 28 24
    // 0c, cs_5_1 0x25441313 3c 18 28 28 24 0c. Without the header the record
    // is the 32-byte one.
    constexpr u32 kRd11Magic = 0x31314452;    // "RD11", target 5.0
    constexpr u32 kRd51Magic = 0x25441313;    // target 5.1
    const u32 magic = read_u32(chunk_data, 28, chunk_size);
    const bool rd11 = magic == kRd11Magic || magic == kRd51Magic;
    const u32 binding_stride =
        rd11 ? std::max(32u, read_u32(chunk_data, 40, chunk_size)) : 32u;

    // Parse resource bindings
    usize off = binding_offset;
    for (u32 i = 0; i < binding_count && off + binding_stride <= chunk_size;
         ++i) {
        DxbcResourceBinding rb{};
        u32 name_off = read_u32(chunk_data, off, chunk_size);
        rb.type = read_u32(chunk_data, off + 4, chunk_size);
        rb.return_type = read_u32(chunk_data, off + 8, chunk_size);
        rb.dimension = read_u32(chunk_data, off + 12, chunk_size);
        // skip num_samples at off + 16
        rb.bind_point = read_u32(chunk_data, off + 20, chunk_size);
        rb.bind_count = read_u32(chunk_data, off + 24, chunk_size);
        // off + 28 is D3D_SHADER_INPUT_FLAGS.
        if (binding_stride >= 40u) {
            rb.space = read_u32(chunk_data, off + 32, chunk_size);
            rb.range_id = read_u32(chunk_data, off + 36, chunk_size);
        }

        if (name_off < chunk_size) {
            auto* str = reinterpret_cast<const char*>(chunk_data + name_off);
            usize max_len = chunk_size - name_off;
            usize len = 0;
            while (len < max_len && str[len] != '\0') ++len;
            rb.name.assign(str, len);
        }

        bindings.push_back(std::move(rb));
        off += binding_stride;
    }

    // Parse constant buffer descriptions
    off = cbuf_offset;
    for (u32 i = 0; i < cbuf_count && off + 24 <= chunk_size; ++i) {
        DxbcConstantBuffer cb{};
        u32 name_off = read_u32(chunk_data, off, chunk_size);
        u32 var_count = read_u32(chunk_data, off + 4, chunk_size);
        u32 var_offset = read_u32(chunk_data, off + 8, chunk_size);
        cb.size_bytes = read_u32(chunk_data, off + 12, chunk_size);

        if (name_off < chunk_size) {
            auto* str = reinterpret_cast<const char*>(chunk_data + name_off);
            usize max_len = chunk_size - name_off;
            usize len = 0;
            while (len < max_len && str[len] != '\0') ++len;
            cb.name.assign(str, len);
        }

        // Parse variables
        usize voff = var_offset;
        for (u32 j = 0; j < var_count && voff + 28 <= chunk_size; ++j) {
            DxbcCBufferVariable var{};
            u32 vname_off = read_u32(chunk_data, voff, chunk_size);
            var.offset = read_u32(chunk_data, voff + 4, chunk_size);
            var.size = read_u32(chunk_data, voff + 8, chunk_size);

            if (vname_off < chunk_size) {
                auto* str = reinterpret_cast<const char*>(chunk_data + vname_off);
                usize max_len = chunk_size - vname_off;
                usize len = 0;
                while (len < max_len && str[len] != '\0') ++len;
                var.name.assign(str, len);
            }

            // Type info at voff + 12 points to a type description, but
            // for now we capture offset+size which is sufficient for binding
            cb.variables.push_back(std::move(var));
            voff += 28;
        }

        cbufs.push_back(std::move(cb));
        off += 24;
    }
}

} // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

namespace {

// Parse the IFCE chunk (layout: dxbc_parser.hpp, "Class linkage"). Every
// offset is checked against the chunk; a record out of range ends parsing
// with what was read so far.
void parse_ifce_chunk(const u8* data, u32 size, DxbcClassLinkage& out) {
    const auto word = [&](u32 offset) { return read_u32(data, offset, size); };
    const auto half = [&](u32 offset) -> u32 {
        if (offset + 2u > size) return 0u;
        u16 value = 0;
        std::memcpy(&value, data + offset, sizeof(value));
        return value;
    };
    const auto name_at = [&](u32 offset) {
        std::string name;
        for (u32 i = offset; i < size && data[i] != 0; ++i) {
            name.push_back(static_cast<char>(data[i]));
        }
        return name;
    };
    if (size < 36u) return;
    const u32 instance_count = word(0);
    const u32 type_count = word(4);
    const u32 run_count = word(8);
    out.slot_count = word(12);
    const u32 instances = word(16);
    const u32 types = word(20);
    const u32 runs = word(24);
    for (u32 i = 0; i < type_count && types + i * 12u + 12u <= size; ++i) {
        const u32 at = types + i * 12u;
        DxbcClassType type{};
        type.name = name_at(word(at));
        type.cb_stride = half(at + 6u);
        type.texture_count = half(at + 8u);
        type.sampler_count = half(at + 10u);
        out.types.push_back(std::move(type));
    }
    for (u32 i = 0; i < instance_count && instances + i * 16u + 16u <= size;
         ++i) {
        const u32 at = instances + i * 16u;
        DxbcClassInstance instance{};
        instance.name = name_at(word(at));
        instance.type = half(at + 4u);
        instance.count = half(at + 6u);
        instance.constant_buffer = half(at + 8u);
        instance.cb_offset = half(at + 10u);
        instance.texture = half(at + 12u);
        instance.sampler = half(at + 14u);
        out.instances.push_back(std::move(instance));
    }
    for (u32 i = 0; i < run_count && runs + i * 16u + 16u <= size; ++i) {
        const u32 at = runs + i * 16u;
        DxbcInterfaceSlots run{};
        run.count = word(at);
        const u32 entries = word(at + 4u);
        const u32 type_ids = word(at + 8u);
        const u32 table_ids = word(at + 12u);
        for (u32 k = 0; k < entries && type_ids + k * 2u + 2u <= size &&
                        table_ids + k * 4u + 4u <= size;
             ++k) {
            run.type_ids.push_back(half(type_ids + k * 2u));
            run.table_ids.push_back(word(table_ids + k * 4u));
        }
        out.slot_runs.push_back(std::move(run));
    }
}

// Opcodes of the binding declarations (D3D10/11 tokenized program format;
// the layouts are in dxbc_parser.hpp, "Binding declarations").
constexpr u32 kDclResource = 88;
constexpr u32 kDclConstantBuffer = 89;
constexpr u32 kDclSampler = 90;
constexpr u32 kDclUavTyped = 156;
constexpr u32 kDclUavRaw = 157;
constexpr u32 kDclUavStructured = 158;
constexpr u32 kDclResourceRaw = 161;
constexpr u32 kDclResourceStructured = 162;
constexpr u32 kCustomData = 53;

// The RDEF D3D_SHADER_INPUT_TYPE values (d3dcommon.h, SDK 10.0.26100) of a
// register class: CBUFFER 0; SAMPLER 3; UAV_RWTYPED 4, UAV_RWSTRUCTURED 6,
// UAV_RWBYTEADDRESS 8, UAV_APPEND_STRUCTURED 9, UAV_CONSUME_STRUCTURED 10,
// UAV_RWSTRUCTURED_WITH_COUNTER 11; every other input is a t#.
DxbcRegisterClass rdef_register_class(u32 type) {
    switch (type) {
    case 0: return DxbcRegisterClass::ConstantBuffer;
    case 3: return DxbcRegisterClass::Sampler;
    case 4: case 6: case 8: case 9: case 10: case 11:
        return DxbcRegisterClass::UnorderedAccess;
    default: return DxbcRegisterClass::ShaderResource;
    }
}

// Every binding declaration of the program. A declaration operand is never
// relatively indexed; anything else there means the bytecode is not a
// program this walk understands, and the list is left empty.
void parse_binding_declarations(DxbcContainer& container) {
    const std::vector<u8>& code = container.bytecode;
    const usize words = code.size() / 4u;
    const auto word = [&](usize index) {
        return read_u32(code.data(), index * 4u, code.size());
    };
    const bool sm51 = container.is_sm51_plus();
    std::vector<DxbcBindingDeclaration> out;
    usize at = 2;  // version, length
    while (at < words) {
        const u32 token = word(at);
        const u32 opcode = token & 0x7FFu;
        u32 length = (token >> 24) & 0x7Fu;
        if (opcode == kCustomData) {
            if (at + 1 >= words) break;
            length = word(at + 1);
        }
        if (length == 0 || at + length > words) return;
        const bool declaration =
            opcode == kDclResource || opcode == kDclConstantBuffer ||
            opcode == kDclSampler || opcode == kDclUavTyped ||
            opcode == kDclUavRaw || opcode == kDclUavStructured ||
            opcode == kDclResourceRaw || opcode == kDclResourceStructured;
        if (!declaration) {
            at += length;
            continue;
        }
        const usize end = at + length;
        usize p = at + 1;
        // Extended opcode tokens (bit 31) never precede these operands, but
        // stepping over them costs nothing.
        for (u32 ext = token; (ext >> 31) != 0 && p < end; ++p) ext = word(p);
        if (p >= end) return;
        const u32 operand = word(p++);
        for (u32 ext = operand; (ext >> 31) != 0 && p < end; ++p) {
            ext = word(p);
        }
        const u32 dims = (operand >> 20) & 0x3u;
        u32 index[3] = {0, 0, 0};
        for (u32 d = 0; d < dims; ++d) {
            const u32 repr = (operand >> (22u + 3u * d)) & 0x7u;
            if (repr != 0u || p >= end) return;  // immediate32 only
            index[d] = word(p++);
        }
        const auto next = [&]() -> u32 { return p < end ? word(p++) : 0u; };
        DxbcBindingDeclaration decl{};
        decl.opcode = opcode;
        if (sm51) {
            if (dims != 3u) return;
            decl.range_id = index[0];
            decl.lower = index[1];
            decl.upper = index[2];
        } else {
            if (dims < 1u) return;
            decl.range_id = decl.lower = decl.upper = index[0];
        }
        switch (opcode) {
        case kDclConstantBuffer:
            decl.register_class = DxbcRegisterClass::ConstantBuffer;
            decl.cb_vectors = sm51 ? next() : index[1];
            decl.dynamically_indexed = ((token >> 11) & 1u) != 0;
            break;
        case kDclSampler:
            decl.register_class = DxbcRegisterClass::Sampler;
            decl.sampler_mode = (token >> 11) & 0xFu;
            break;
        case kDclResource:
            decl.register_class = DxbcRegisterClass::ShaderResource;
            decl.dimension = (token >> 11) & 0x1Fu;
            decl.sample_count = (token >> 16) & 0x7Fu;
            decl.return_type = next();
            break;
        case kDclResourceRaw:
            decl.register_class = DxbcRegisterClass::ShaderResource;
            break;
        case kDclResourceStructured:
            decl.register_class = DxbcRegisterClass::ShaderResource;
            decl.stride = next();
            break;
        default:  // the three UAV forms
            decl.register_class = DxbcRegisterClass::UnorderedAccess;
            decl.globally_coherent = ((token >> 16) & 1u) != 0;
            decl.rasterizer_ordered = ((token >> 17) & 1u) != 0;
            if (opcode == kDclUavTyped) {
                decl.dimension = (token >> 11) & 0x1Fu;
                decl.return_type = next();
            } else if (opcode == kDclUavStructured) {
                decl.stride = next();
                decl.has_counter = ((token >> 23) & 1u) != 0;
            }
            break;
        }
        if (sm51) decl.space = next();
        for (const auto& rb : container.resource_bindings) {
            if (rdef_register_class(rb.type) != decl.register_class) continue;
            const bool match = sm51
                ? rb.range_id == decl.range_id && rb.space == decl.space
                : decl.lower >= rb.bind_point &&
                      decl.lower - rb.bind_point < std::max(1u, rb.bind_count);
            if (match) {
                decl.name = rb.name;
                break;
            }
        }
        out.push_back(std::move(decl));
        at = end;
    }
    container.binding_declarations = std::move(out);
}

} // namespace

ShaderStage dxbc_program_type_to_stage(u32 program_type) {
    switch (program_type) {
    case D3D10_SB_PIXEL_SHADER:    return ShaderStage::Fragment;
    case D3D10_SB_VERTEX_SHADER:   return ShaderStage::Vertex;
    case D3D10_SB_GEOMETRY_SHADER: return ShaderStage::Geometry;
    case D3D11_SB_HULL_SHADER:     return ShaderStage::Hull;
    case D3D11_SB_DOMAIN_SHADER:   return ShaderStage::Domain;
    case D3D11_SB_COMPUTE_SHADER:  return ShaderStage::Compute;
    default:                       return ShaderStage::Vertex;
    }
}

DxbcContainer parse_dxbc(const u8* data, usize size) {
    DxbcContainer result{};
    result.shader_model_major = 0;
    result.shader_model_minor = 0;
    result.stage = ShaderStage::Vertex;
    std::memset(result.checksum, 0, sizeof(result.checksum));

    if (!data || size < sizeof(DxbcHeader)) {
        return result;
    }

    // Validate magic
    auto magic = read_u32(data, 0, size);
    if (magic != DXBC_MAGIC) {
        VITREO_TRACE("[shader] DXBC magic mismatch: 0x%08X\n", magic);
        return result;
    }

    // Parse header
    DxbcHeader header;
    std::memcpy(&header, data, sizeof(DxbcHeader));
    std::memcpy(result.checksum, header.checksum, 16);

    if (header.total_size > size) {
        VITREO_TRACE("[shader] DXBC size mismatch: %u > %zu\n",
                     header.total_size, size);
        return result;
    }

    // Read chunk offsets
    usize offset_table = sizeof(DxbcHeader);
    for (u32 i = 0; i < header.chunk_count; ++i) {
        usize table_entry = offset_table + i * sizeof(u32);
        if (table_entry + 4 > size) break;

        u32 chunk_offset = read_u32(data, table_entry, size);
        if (chunk_offset + sizeof(ChunkHeader) > size) continue;

        ChunkHeader ch;
        std::memcpy(&ch, data + chunk_offset, sizeof(ChunkHeader));

        const u8* chunk_data = data + chunk_offset + sizeof(ChunkHeader);
        u32 chunk_size = ch.size;

        if (chunk_offset + sizeof(ChunkHeader) + chunk_size > size) continue;

        auto chunk_type = static_cast<DxbcChunkType>(ch.fourcc);

        switch (chunk_type) {
        case DxbcChunkType::ISGN:
            parse_signature_chunk(chunk_data, chunk_size,
                                  result.input_signature);
            result.has_input_signature = true;
            break;

        case DxbcChunkType::ISG1:
            parse_signature_chunk(chunk_data, chunk_size,
                                  result.input_signature,
                                  /*with_stream=*/true,
                                  /*with_min_precision=*/true);
            result.has_input_signature = true;
            break;

        case DxbcChunkType::OSGN:
            parse_signature_chunk(chunk_data, chunk_size,
                                  result.output_signature);
            break;

        case DxbcChunkType::PCSG:
            parse_signature_chunk(chunk_data, chunk_size,
                                  result.patch_constant_signature);
            break;

        case DxbcChunkType::OSG5:
            parse_signature_chunk(chunk_data, chunk_size,
                                  result.output_signature,
                                  /*with_stream=*/true);
            break;

        case DxbcChunkType::RDEF:
            parse_rdef_chunk(chunk_data, chunk_size,
                             result.resource_bindings, result.constant_buffers);
            break;

        case DxbcChunkType::SHDR:
        case DxbcChunkType::SHEX:
            result.bytecode.assign(chunk_data, chunk_data + chunk_size);
            // Extract shader model version + program type from first token
            if (chunk_size >= 4) {
                u32 version_token = read_u32(chunk_data, 0, chunk_size);
                result.shader_model_major = (version_token >> 4) & 0xF;
                result.shader_model_minor = version_token & 0xF;
                u32 program_type = (version_token >> 16) & 0xFFFF;
                result.stage = dxbc_program_type_to_stage(program_type);
            }
            break;

        case DxbcChunkType::SFI0:
            // One 64-bit flags field (fxc: 0x8 for a u63 declaration, 0x4
            // for a vertex-stage UAV, 0x40 for msad; d3d11_fl111.hlsl).
            if (chunk_size >= 8) {
                u64 flags = 0;
                std::memcpy(&flags, chunk_data, sizeof(flags));
                result.required_features = flags;
            } else if (chunk_size >= 4) {
                result.required_features = read_u32(chunk_data, 0, chunk_size);
            }
            break;

        case DxbcChunkType::IFCE:
            parse_ifce_chunk(chunk_data, chunk_size, result.class_linkage);
            result.has_class_linkage = true;
            break;

        case DxbcChunkType::VSLB: {
            // dxbc_with_slot_bindings: a count, then {table, this.xyzw} per
            // slot.
            const u32 count = read_u32(chunk_data, 0, chunk_size);
            for (u32 slot = 0;
                 slot < count && 4u + (slot + 1u) * 20u <= chunk_size;
                 ++slot) {
                DxbcSlotBinding binding{};
                const u32 at = 4u + slot * 20u;
                binding.table = read_u32(chunk_data, at, chunk_size);
                for (u32 c = 0; c < 4; ++c) {
                    binding.this_data[c] =
                        read_u32(chunk_data, at + 4u + c * 4u, chunk_size);
                }
                result.slot_bindings.push_back(binding);
            }
            break;
        }

        default:
            // Skip unknown chunks
            break;
        }
    }

    // After the loop: the declarations take their names from RDEF, which
    // may follow SHEX in the chunk table.
    if (result.valid() && result.is_sm4_plus()) {
        parse_binding_declarations(result);
    }
    return result;
}

std::vector<u8> dxbc_with_slot_bindings(
    const u8* data, usize size, const std::vector<DxbcSlotBinding>& bindings) {
    if (data == nullptr || size < sizeof(DxbcHeader) ||
        read_u32(data, 0, size) != DXBC_MAGIC) {
        return {};
    }
    DxbcHeader header;
    std::memcpy(&header, data, sizeof(DxbcHeader));
    const usize table_end =
        sizeof(DxbcHeader) + usize{header.chunk_count} * 4u;
    if (header.total_size > size || table_end > header.total_size) return {};
    std::vector<u8> chunk(8u + 4u + bindings.size() * 20u);
    const auto put = [&](usize at, u32 value) {
        std::memcpy(chunk.data() + at, &value, sizeof(value));
    };
    put(0, static_cast<u32>(DxbcChunkType::VSLB));
    put(4, static_cast<u32>(chunk.size() - 8u));
    put(8, static_cast<u32>(bindings.size()));
    for (usize i = 0; i < bindings.size(); ++i) {
        put(12u + i * 20u, bindings[i].table);
        for (u32 c = 0; c < 4; ++c) {
            put(12u + i * 20u + 4u + c * 4u, bindings[i].this_data[c]);
        }
    }
    // Header and offset table, one entry longer; every chunk moves by 4.
    std::vector<u8> out(data, data + table_end);
    out.resize(table_end + 4u);
    const auto patch = [&](usize at, u32 value) {
        std::memcpy(out.data() + at, &value, sizeof(value));
    };
    for (u32 i = 0; i < header.chunk_count; ++i) {
        patch(sizeof(DxbcHeader) + i * 4u,
              read_u32(data, sizeof(DxbcHeader) + i * 4u, size) + 4u);
    }
    out.insert(out.end(), data + table_end, data + header.total_size);
    patch(sizeof(DxbcHeader) + usize{header.chunk_count} * 4u,
          static_cast<u32>(out.size()));
    out.insert(out.end(), chunk.begin(), chunk.end());
    patch(offsetof(DxbcHeader, chunk_count), header.chunk_count + 1u);
    patch(offsetof(DxbcHeader, total_size), static_cast<u32>(out.size()));
    return out;
}

bool detect_d3d9_shader(
    const u8* data, usize size,
    u32& out_major, u32& out_minor, ShaderStage& out_stage)
{
    if (!data || size < 4) return false;

    u32 version_token = read_u32(data, 0, size);
    u16 shader_type = static_cast<u16>(version_token >> 16);
    out_major = (version_token >> 8) & 0xFF;
    out_minor = version_token & 0xFF;

    if (shader_type == 0xFFFE) {
        out_stage = ShaderStage::Vertex;
        return out_major >= 1 && out_major <= 3;
    }
    if (shader_type == 0xFFFF) {
        out_stage = ShaderStage::Fragment;
        return out_major >= 1 && out_major <= 3;
    }

    return false;
}

namespace {

// One MD5 compression round (RFC 1321, section 3.4) over sixteen little-endian
// words.
void md5_transform(u32 state[4], const u32 x[16]) {
    static constexpr u32 k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static constexpr u32 shift[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    u32 a = state[0], b = state[1], c = state[2], d = state[3];
    for (u32 i = 0; i < 64; ++i) {
        u32 f = 0;
        u32 g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const u32 rotated = a + f + k[i] + x[g];
        a = d;
        d = c;
        c = b;
        b = b + ((rotated << shift[i]) | (rotated >> (32 - shift[i])));
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

// The DXBC container hash: MD5's compression function, but the final block
// stores the bit count in its FIRST word and (byte count * 2) | 1 in its last,
// instead of MD5's 64-bit length (DirectXShaderCompiler, DxilHash.cpp,
// ComputeHashRetail).
void dxbc_hash(const u8* data, u32 byte_count, u32 out[4]) {
    static constexpr u8 padding[64] = {0x80};
    const u32 left_over = byte_count & 0x3f;
    const bool two_rows = left_over >= 56;
    const u32 pad_amount = two_rows ? 120 - left_over : 56 - left_over;
    const u32 blocks = (byte_count + pad_amount + 8) >> 6;
    u32 state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    u32 next_end = two_rows ? blocks - 2 : blocks - 1;
    u32 offset = 0;
    for (u32 i = 0; i < blocks; ++i, offset += 64) {
        u32 x[16]{};
        if (i != next_end) {
            std::memcpy(x, data + offset, 64);
        } else if (!two_rows) {
            const u32 remainder = byte_count - offset;
            x[0] = byte_count << 3;
            std::memcpy(reinterpret_cast<u8*>(x) + 4, data + offset, remainder);
            std::memcpy(reinterpret_cast<u8*>(x) + 4 + remainder, padding,
                        pad_amount);
            x[15] = 1 | (byte_count << 1);
        } else if (i == blocks - 2) {
            const u32 remainder = byte_count - offset;
            std::memcpy(x, data + offset, remainder);
            std::memcpy(reinterpret_cast<u8*>(x) + remainder, padding,
                        pad_amount - 56);
            next_end = blocks - 1;
        } else {
            x[0] = byte_count << 3;
            std::memcpy(reinterpret_cast<u8*>(x) + 4,
                        padding + pad_amount - 56, 56);
            x[15] = 1 | (byte_count << 1);
        }
        md5_transform(state, x);
    }
    std::memcpy(out, state, sizeof(state));
}

} // namespace

bool dxbc_container_verified(const u8* data, usize size) {
    if (data == nullptr || size < sizeof(DxbcHeader)) return false;
    DxbcHeader header;
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != DXBC_MAGIC || header.total_size < sizeof(DxbcHeader) ||
        header.total_size > size ||
        header.chunk_count >
            (header.total_size - sizeof(DxbcHeader)) / sizeof(u32)) {
        return false;
    }
    for (u32 i = 0; i < header.chunk_count; ++i) {
        const u32 chunk_offset =
            read_u32(data, sizeof(DxbcHeader) + i * sizeof(u32), size);
        if (static_cast<u64>(chunk_offset) + sizeof(ChunkHeader) >
            header.total_size) {
            return false;
        }
        const u32 chunk_size =
            read_u32(data, chunk_offset + sizeof(u32), size);
        if (static_cast<u64>(chunk_offset) + sizeof(ChunkHeader) + chunk_size >
            header.total_size) {
            return false;
        }
    }
    // The hash covers everything after the magic and the hash itself.
    constexpr u32 kHashedFrom = 20;
    u32 hash[4];
    dxbc_hash(data + kHashedFrom, header.total_size - kHashedFrom, hash);
    return std::memcmp(hash, header.checksum, sizeof(hash)) == 0;
}

void dxbc_container_checksum(const u8* data, u32 total_size, u8 out[16]) {
    constexpr u32 kHashedFrom = 20;
    u32 hash[4]{};
    if (data != nullptr && total_size >= kHashedFrom) {
        dxbc_hash(data + kHashedFrom, total_size - kHashedFrom, hash);
    }
    std::memcpy(out, hash, sizeof(hash));
}

bool dxbc_declares_uavs(const DxbcContainer& container) {
    // Walk the SHDR/SHEX token stream: version, length, then instructions of
    // OpcodeToken0[30:24] tokens each (CUSTOMDATA carries its length in the
    // next token instead). Opcodes 156-158 are dcl_uav_typed, dcl_uav_raw and
    // dcl_uav_structured (Windows SDK d3d11TokenizedProgramFormat.hpp).
    const usize count = container.bytecode.size() / 4u;
    if (count < 2u) return false;
    const auto word = [&](usize i) {
        u32 value = 0;
        std::memcpy(&value, container.bytecode.data() + i * 4u, 4u);
        return value;
    };
    const usize end = std::min<usize>(count, word(1));
    usize i = 2;
    while (i < end) {
        const u32 token = word(i);
        const u32 opcode = token & 0x7FFu;
        usize length = (token >> 24) & 0x7Fu;
        if (opcode == 53u) {
            if (i + 1u >= end) return false;
            length = word(i + 1u);
        }
        if (length == 0u) return false;
        if (opcode == 156u || opcode == 157u || opcode == 158u) return true;
        i += length;
    }
    return false;
}

namespace {

/// Calls @p visit(OpcodeToken0) for every instruction of the SHDR/SHEX token
/// stream (version, length, then OpcodeToken0[30:24]-token instructions;
/// CUSTOMDATA carries its length in the next token). Stops early when
/// @p visit returns true, and returns whether it did.
template <typename Visit>
bool any_instruction(const DxbcContainer& container, Visit visit) {
    const usize count = container.bytecode.size() / 4u;
    if (count < 2u) return false;
    const auto word = [&](usize i) {
        u32 value = 0;
        std::memcpy(&value, container.bytecode.data() + i * 4u, 4u);
        return value;
    };
    const usize end = std::min<usize>(count, word(1));
    usize i = 2;
    while (i < end) {
        const u32 token = word(i);
        const u32 opcode = token & 0x7FFu;
        usize length = (token >> 24) & 0x7Fu;
        if (opcode == 53u) {
            if (i + 1u >= end) return false;
            length = word(i + 1u);
        }
        if (length == 0u) return false;
        if (visit(token)) return true;
        i += length;
    }
    return false;
}

} // namespace

bool dxbc_writes_memory(const DxbcContainer& container) {
    return any_instruction(container, [](u32 token) {
        const u32 opcode = token & 0x7FFu;
        return opcode == 164u || opcode == 166u ||
               (opcode >= 168u && opcode <= 189u);
    });
}

bool dxbc_pixel_runs_per_sample(const DxbcContainer& container) {
    // SV_SampleIndex is D3D_NAME_SAMPLE_INDEX (10) in the input signature.
    for (const auto& element : container.input_signature) {
        if (element.system_value_type == 10u) return true;
    }
    // dcl_input_ps (98), dcl_input_ps_sgv (99), dcl_input_ps_siv (100):
    // interpolation mode in OpcodeToken0 bits 11-14.
    return any_instruction(container, [](u32 token) {
        const u32 opcode = token & 0x7FFu;
        const u32 mode = (token >> 11) & 0xFu;
        return opcode >= 98u && opcode <= 100u && (mode == 6u || mode == 7u);
    });
}

bool dxbc_forces_early_depth_stencil(const DxbcContainer& container) {
    // dcl_globalFlags (106); fxc sets bit 13 for [earlydepthstencil]
    // (0x0100286a beside refactoringAllowed's 0x0100086a).
    return any_instruction(container, [](u32 token) {
        return (token & 0x7FFu) == 106u && (token & (1u << 13)) != 0u;
    });
}

bool dxbc_uses_class_linkage(const DxbcContainer& container) {
    // dcl_function_body 144, dcl_function_table 145, dcl_interface 146 and
    // fcall 120, as fxc's own disassembler names them (the area-09a probe's
    // cs_iface, tools/oracle_probe/d3d11_a09a_alu_shaders.hlsl).
    const usize count = container.bytecode.size() / 4u;
    if (count < 2u) return false;
    const auto word = [&](usize i) {
        u32 value = 0;
        std::memcpy(&value, container.bytecode.data() + i * 4u, 4u);
        return value;
    };
    const usize end = std::min<usize>(count, word(1));
    usize i = 2;
    while (i < end) {
        const u32 token = word(i);
        const u32 opcode = token & 0x7FFu;
        usize length = (token >> 24) & 0x7Fu;
        if (opcode == 53u) {
            if (i + 1u >= end) return false;
            length = word(i + 1u);
        }
        if (length == 0u) return false;
        if (opcode == 120u || (opcode >= 144u && opcode <= 146u)) return true;
        i += length;
    }
    return false;
}

bool dxbc_writes_stencil_ref(const DxbcContainer& container) {
    // fxc names oStencilRef in the output signature with system value 0 and
    // register 0xFFFFFFFF, so the semantic identifies it.
    for (const auto& element : container.output_signature) {
        const std::string& name = element.semantic_name;
        static constexpr char kName[] = "SV_StencilRef";
        if (name.size() != sizeof(kName) - 1u) continue;
        bool equal = true;
        for (usize c = 0; c < name.size() && equal; ++c) {
            equal = std::tolower(static_cast<unsigned char>(name[c])) ==
                    std::tolower(static_cast<unsigned char>(kName[c]));
        }
        if (equal) return true;
    }
    return false;
}

} // namespace vitreo::graphics

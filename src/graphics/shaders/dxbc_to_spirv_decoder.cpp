// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_decoder.cpp
/// @brief DXBC operand decoding and the immediate constant buffer.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

/// The register class a resource operand names (t#, s#, u#, cb#).
DxbcRegisterClass register_class_of(Sm4OperandType type) {
    switch (type) {
    case Sm4OperandType::ConstantBuffer:
        return DxbcRegisterClass::ConstantBuffer;
    case Sm4OperandType::Sampler: return DxbcRegisterClass::Sampler;
    case Sm4OperandType::UnorderedAccessView:
        return DxbcRegisterClass::UnorderedAccess;
    default: return DxbcRegisterClass::ShaderResource;
    }
}

bool DxbcDecoder::decode_operand(Sm4Operand& op) {
    const u32 ordinal = m_operand_depth == 0 ? m_operand_ordinal++ : ~0u;
    ++m_operand_depth;
    const bool decoded = decode_operand_tokens(op);
    --m_operand_depth;
    if (decoded && m_sm51_resource_operands &&
        (op.type == Sm4OperandType::Resource ||
         op.type == Sm4OperandType::Sampler ||
         op.type == Sm4OperandType::UnorderedAccessView ||
         op.type == Sm4OperandType::ConstantBuffer)) {
        // See Sm4Operand::has_register_index. index1's relative register is
        // op.relative_* unless index2 (a constant buffer's element) was
        // relative too and overwrote it, in which case decode_operand_tokens
        // kept index1's in m_index1_relative.
        op.has_register_index = true;
        op.register_index = op.index1;
        op.register_relative = op.index1_relative;
        op.register_relative_type = m_index1_relative.type;
        op.register_relative_register = m_index1_relative.index;
        op.register_relative_component = m_index1_relative.component;
        if (op.type == Sm4OperandType::ConstantBuffer) {
            op.index1 = m_index2.immediate;
            op.index1_relative = m_index2.relative;
            op.relative_type = m_index2.type;
            op.relative_index0 = m_index2.index;
            op.relative_component = m_index2.component;
        } else {
            op.index1 = 0;
            op.index1_relative = false;
        }
    }
    if (decoded && ordinal != ~0u && !m_index0_overrides.empty()) {
        const auto forced = m_index0_overrides.find(ordinal);
        if (forced != m_index0_overrides.end() && op.index0_relative) {
            op.index0 = forced->second;
            op.index0_relative = false;
        }
    }
    return decoded;
}

bool DxbcDecoder::decode_operand_tokens(Sm4Operand& op) {
    if (!has_tokens(1)) return false;

    u32 token = read_token();

    // Microsoft D3D10 Tokenized Program Format OperandToken0:
    // [01:00] NumComponents: 0→0, 1→1, 2→4 components
    switch (token & 0x3u) {
    case 0: op.num_components = 0; break;
    case 1: op.num_components = 1; break;
    case 2: op.num_components = 4; break;
    default: op.num_components = 4; break;
    }

    const u32 selection = (token >> 2) & 0x3u;
    if (op.num_components == 4) {
        if (selection == static_cast<u32>(Sm4ComponentSelection::Mask)) {
            op.write_mask = (token >> 4) & 0xFu;
        } else if (selection ==
                   static_cast<u32>(Sm4ComponentSelection::Swizzle)) {
            op.swizzle = (token >> 4) & 0xFFu;
        } else if (selection ==
                   static_cast<u32>(Sm4ComponentSelection::Select1)) {
            const u32 comp = (token >> 4) & 0x3u;
            op.swizzle = static_cast<u32>(comp * 0x55u); // broadcast
            op.write_mask = 1u << comp;
        }
    } else if (op.num_components == 1) {
        op.write_mask = 0x1u;
        op.swizzle = 0x00u;
    }

    op.type = static_cast<Sm4OperandType>((token >> 12) & 0xFFu);

    // Extended operand token(s) precede index immediates when bit 31 is set.
    u32 ext = token;
    while ((ext >> 31) & 1u) {
        if (!has_tokens(1)) return false;
        ext = read_token();
        // Microsoft D3D10/11 Tokenized Program Format: extended operand type
        // 1 stores D3D10_SB_OPERAND_MODIFIER in bits 13:6. Retain it for
        // instructions whose contracts permit modifiers; min precision is
        // otherwise intentionally ignored.
        if ((ext & 0x3Fu) == 1u) {
            op.modifier = (ext >> 6) & 0xFFu;
            op.nonuniform = ((ext >> 17) & 1u) != 0;
        }
    }

    if (op.type == Sm4OperandType::Immediate32) {
        op.is_immediate = true;
        const u32 count = op.num_components == 0 ? 1u : op.num_components;
        for (u32 i = 0; i < count && i < 4; ++i) {
            if (!has_tokens(1)) return false;
            u32 bits = read_token();
            std::memcpy(&op.immediate[i], &bits, sizeof(float));
        }
        // Splat scalar immediates into unused lanes for easier ALU use.
        for (u32 i = count; i < 4; ++i) {
            op.immediate[i] = op.immediate[0];
        }
        return true;
    }
    if (op.type == Sm4OperandType::Immediate64) {
        // A double immediate, d(x, y) in fxc's listing. MEASURED: the
        // four-component form carries two doubles in four DWORDs (fxc's
        // `dmax r4.xy, r4.xyxy, d(0.0, 0.0)` is 10 DWORDs long), i.e. the
        // .xyzw register-pair layout of 3.1.4, low word first. fxc never
        // emits the one-component form; patched in (d3d11_doubles_probe
        // p_dimm1), two DWORDs follow the token (fxc /dumpbin decodes on)
        // and WARP reads the FIRST DWORD into every component
        // (`dmov r1.xy, <0x3fb999999999999a>` gave 0x9999999a in .x and .y).
        op.is_immediate = true;
        if (op.num_components == 0) return false;
        const u32 count = op.num_components == 1 ? 2u : 4u;
        for (u32 i = 0; i < count; ++i) {
            if (!has_tokens(1)) return false;
            const u32 bits = read_token();
            std::memcpy(&op.immediate[i], &bits, sizeof(float));
        }
        if (count == 2) {
            op.immediate[1] = op.immediate[2] = op.immediate[3] =
                op.immediate[0];
        }
        return true;
    }

    const u32 index_dim = (token >> 20) & 0x3u;
    auto read_imm_index = [&](u32 repr, u32& out) -> bool {
        // D3D10_SB_OPERAND_INDEX_IMMEDIATE32 == 0
        if (repr != 0u) return false;
        if (!has_tokens(1)) return false;
        out = read_token();
        return true;
    };
    const auto read_relative_index = [&](u32 repr, u32& immediate,
                                         bool& is_relative) -> bool {
        if (repr != 2u && repr != 3u) {
            return read_imm_index(repr, immediate);
        }
        if (repr == 3u) {
            if (!has_tokens(1)) return false;
            immediate = read_token();
        }
        Sm4Operand relative;
        if (!decode_operand(relative) || relative.is_immediate ||
            relative.index0_relative || relative.index1_relative) {
            return false;
        }
        is_relative = true;
        op.relative_type = relative.type;
        op.relative_index0 = relative.index0;
        op.relative_component = relative.swizzle & 0x3u;
        return true;
    };
    if (index_dim >= 1) {
        const u32 repr0 = (token >> 22) & 0x7u;
        if (!read_relative_index(repr0, op.index0, op.index0_relative)) {
            return false;
        }
        if (op.index0_relative) {
            op.index0_relative_type = op.relative_type;
            op.index0_relative_register = op.relative_index0;
            op.index0_relative_component = op.relative_component;
        }
    }
    if (index_dim >= 2) {
        const u32 repr1 = (token >> 25) & 0x7u;
        if (!read_relative_index(repr1, op.index1, op.index1_relative)) {
            return false;
        }
        if (m_operand_depth == 1) {
            m_index1_relative = {op.index1, op.index1_relative,
                                 op.relative_type, op.relative_index0,
                                 op.relative_component};
        }
    }
    if (index_dim >= 3) {
        // SM 5.1: a constant buffer's element, which may be relative
        // (CB1[2][r0.x + 1]); a declaration's upper bound otherwise.
        const u32 repr2 = (token >> 28) & 0x7u;
        u32 index2 = 0;
        bool index2_relative = false;
        const Sm4OperandType saved_type = op.relative_type;
        const u32 saved_index = op.relative_index0;
        const u32 saved_component = op.relative_component;
        if ((!m_sm51 && repr2 != 0u) ||
            !read_relative_index(repr2, index2, index2_relative)) {
            return false;
        }
        if (m_operand_depth == 1) {
            m_index2 = {index2, index2_relative, op.relative_type,
                        op.relative_index0, op.relative_component};
        }
        if (!index2_relative) {
            op.relative_type = saved_type;
            op.relative_index0 = saved_index;
            op.relative_component = saved_component;
        }
    } else if (m_operand_depth == 1) {
        m_index2 = {};
    }

    return true;
}

std::vector<std::array<float, 4>> decode_immediate_constant_buffer(
    const std::vector<u8>& bytecode) {
    std::vector<std::array<float, 4>> values;
    DxbcDecoder scan(bytecode.data(), bytecode.size());
    if (!scan.has_tokens(2)) return values;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7ffu;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) return {};
            const u32 block_length = scan.read_token();
            if (block_length < 2u || !scan.has_tokens(block_length - 2u)) {
                return {};
            }
            const u32 custom_class = (token >> 11u) & 0x1fffffu;
            if (custom_class == 3u && ((block_length - 2u) % 4u) == 0u) {
                values.reserve((block_length - 2u) / 4u);
                for (u32 i = 2; i < block_length; i += 4u) {
                    std::array<float, 4> value{};
                    for (u32 lane = 0; lane < 4; ++lane) {
                        const u32 bits = scan.read_token();
                        std::memcpy(&value[lane], &bits, sizeof(bits));
                    }
                    values.push_back(value);
                }
            } else {
                scan.skip_tokens(block_length - 2u);
            }
            continue;
        }
        const u32 length = (token >> 24u) & 0x7fu;
        if (length == 0u || !scan.has_tokens(length - 1u)) return {};
        scan.skip_tokens(length - 1u);
    }
    return values;
}

} // namespace vitreo::graphics::dxbc_spirv_detail

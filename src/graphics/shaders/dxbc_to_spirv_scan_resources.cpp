// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_scan_resources.cpp
/// @brief Resource and register declaration scans of the DXBC program.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

u32 DxbcTranslator::scan_temp_count(usize start_token) const {
    // D3D10_SB_OPCODE_DCL_TEMPS == 104; next DWORD is temp count.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return 0;
    scan.set_position(start_token);
    u32 count = 0;
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opc = token & 0x7FFu;
        if (opc == 62u) break; // RET
        if (opc == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 blen = scan.read_token();
            if (blen < 2) break;
            scan.skip_tokens(blen - 2);
            continue;
        }
        const u32 len = (token >> 24) & 0x7Fu;
        if (opc == 104u) { // DCL_TEMPS
            if (scan.has_tokens(1)) {
                count = scan.read_token();
                if (len > 2) scan.skip_tokens(len - 2);
            }
            continue;
        }
        if (len > 1) scan.skip_tokens(len - 1);
    }
    return count;
}

std::map<u32, u32> DxbcTranslator::scan_indexable_temps(
    usize start_token) const {
    // D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP == 105. The three payload DWORDs
    // declare x# array slot, register count, and component count.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.set_position(start_token);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7ffu;
        if (opcode == 62u) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        const u32 length = (token >> 24) & 0x7fu;
        if (opcode == 105u && length >= 4u && scan.has_tokens(3)) {
            const u32 slot = scan.read_token();
            const u32 count = scan.read_token();
            const u32 components = scan.read_token();
            if (count != 0u && count <= 4096u &&
                components >= 1u && components <= 4u) {
                declarations[slot] = count;
            }
            if (length > 4u) scan.skip_tokens(length - 4u);
            continue;
        }
        if (length > 1u) scan.skip_tokens(length - 1u);
    }
    return declarations;
}

std::map<u32, u32> DxbcTranslator::scan_constant_buffers() const {
    // Microsoft D3D10 tokenized program format opcode 89 is
    // DCL_CONSTANT_BUFFER. Its operand is cb<slot>[<vec4 count>].
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == 62u) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2) break;
            scan.skip_tokens(block_length - 2);
            continue;
        }
        if (opcode == 89u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) &&
                declaration.type == Sm4OperandType::ConstantBuffer) {
                // 5.1: CB#[lower:upper] then the size DWORD (see
                // DxbcBindingDeclaration), keyed by range ID.
                u32 size = declaration.index1;
                if (m_container.is_sm51_plus()) {
                    if (!scan.has_tokens(1)) break;
                    size = scan.read_token();
                }
                declarations[declaration.index0] = std::max(1u, size);
            }
        }
        if (length > 1) {
            const usize consumed = 1 + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

std::tuple<std::map<u32, u32>, std::map<u32, u32>, std::set<u32>>
DxbcTranslator::scan_shader_resources() const {
    // Microsoft tokenized-program opcodes: DCL_RESOURCE=88 and
    // DCL_SAMPLER=90. Resource dimension occupies opcode-token bits 11..15.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> textures;
    std::map<u32, u32> return_tokens;
    std::set<u32> samplers;
    if (!scan.has_tokens(2)) return {textures, return_tokens, samplers};
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == 62u) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2) break;
            scan.skip_tokens(block_length - 2);
            continue;
        }
        if (opcode == 88u || opcode == 90u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration)) {
                if (opcode == 88u &&
                    declaration.type == Sm4OperandType::Resource) {
                    textures[declaration.index0] = (token >> 11) & 0x1Fu;
                    if (!scan.has_tokens(1)) break;
                    return_tokens[declaration.index0] = scan.read_token();
                } else if (opcode == 90u &&
                           declaration.type == Sm4OperandType::Sampler) {
                    samplers.insert(declaration.index0);
                }
            }
        }
        if (length > 1) {
            const usize consumed = 1 + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return {textures, return_tokens, samplers};
}

std::map<u32, u32> DxbcTranslator::scan_structured_srvs() const {
    // Microsoft tokenized-program format: dcl_resource_structured (opcode 162)
    // is the t# resource operand followed by one DWORD holding the structure
    // byte stride. Declarations must be collected before the function body so
    // the storage buffer exists by the time ld_structured references it.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == 62u) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2) break;
            scan.skip_tokens(block_length - 2);
            continue;
        }
        if (opcode == 162u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) &&
                declaration.type == Sm4OperandType::Resource &&
                scan.has_tokens(1)) {
                declarations[declaration.index0] = scan.read_token();
            }
        }
        if (length > 1) {
            const usize consumed = 1 + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

std::map<u32, std::pair<u32, u32>> DxbcTranslator::scan_typed_uavs() const {
    // Microsoft dcl_uav_typed (opcode 156): resource dimension is in
    // OpcodeToken0 bits 11..15, followed by the u# operand and the packed
    // xyzw resource return-type token.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, std::pair<u32, u32>> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (opcode == static_cast<u32>(
                          Sm4Opcode::DCL_UNORDERED_ACCESS_VIEW_TYPED)) {
            Sm4Operand declaration;
            if (!scan.decode_operand(declaration) ||
                declaration.type != Sm4OperandType::UnorderedAccessView ||
                !scan.has_tokens(1)) {
                return {};
            }
            const u32 packed_return = scan.read_token();
            declarations[declaration.index0] = {
                (token >> 11) & 0x1Fu, packed_return};
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

std::set<u32> DxbcTranslator::scan_raw_srvs() const {
    // Windows SDK opcode 161 is dcl_resource_raw t#. A ByteAddressBuffer takes
    // no return type and no stride token: it is addressed in bytes and read
    // as 32-bit words, so the declaration is the resource operand alone.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::set<u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (opcode == 161u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) &&
                declaration.type == Sm4OperandType::Resource) {
                declarations.insert(declaration.index0);
            }
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

std::map<u32, u32> DxbcTranslator::scan_raw_tgsm() const {
    // Windows SDK opcode 159 is dcl_tgsm_raw g#, byteCount.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (opcode == 159u) {
            Sm4Operand declaration;
            if (!scan.decode_operand(declaration) ||
                declaration.type != Sm4OperandType::ThreadGroupSharedMemory ||
                !scan.has_tokens(1)) {
                return {};
            }
            declarations[declaration.index0] = scan.read_token();
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

std::set<u32> DxbcTranslator::scan_raw_uavs() const {
    // Windows SDK opcode 157 is dcl_uav_raw u#. Learn (RWByteAddressBuffer):
    // a raw buffer has no element type and no format — it is addressed by byte
    // offset and read or written as 32-bit words, which is exactly a storage
    // buffer of words. The writable twin of dcl_resource_raw.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::set<u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (opcode == 157u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) &&
                declaration.type == Sm4OperandType::UnorderedAccessView) {
                declarations.insert(declaration.index0);
            }
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

std::vector<u32> DxbcTranslator::scan_uav_counters() const {
    // The whole program, not just the declarations: unlike the scans above
    // this looks for instructions, and a `ret` inside a branch is not the end.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::vector<u32> registers;
    if (!scan.has_tokens(2)) return registers;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (length == 0u) break;
        if (opcode == static_cast<u32>(Sm4Opcode::IMM_ATOMIC_ALLOC) ||
            opcode == static_cast<u32>(Sm4Opcode::IMM_ATOMIC_CONSUME)) {
            Sm4Operand destination, uav;
            if (scan.decode_operand(destination) && scan.decode_operand(uav) &&
                uav.type == Sm4OperandType::UnorderedAccessView &&
                std::find(registers.begin(), registers.end(), uav.index0) ==
                    registers.end()) {
                registers.push_back(uav.index0);
            }
        }
        const usize consumed = 1u + (scan.position() - operands_begin);
        if (consumed < length) scan.skip_tokens(length - consumed);
    }
    return registers;
}

std::map<u32, u32> DxbcTranslator::scan_uav_declaration_tokens() const {
    // Opcodes 156 (dcl_uav_typed), 157 (dcl_uav_raw) and 158
    // (dcl_uav_structured) all name their u# as the first operand.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (length == 0u) break;
        if (opcode == 156u || opcode == 157u || opcode == 158u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) &&
                declaration.type == Sm4OperandType::UnorderedAccessView) {
                declarations[declaration.index0] = token;
            }
        }
        const usize consumed = 1u + (scan.position() - operands_begin);
        if (consumed < length) scan.skip_tokens(length - consumed);
    }
    return declarations;
}

std::map<u32, u32> DxbcTranslator::scan_structured_uavs() const {
    // Windows SDK opcode 158 is dcl_uav_structured u#, structByteStride.
    // Learn (RWStructuredBuffer): the element type is opaque to the ISA, so
    // like its read-only counterpart the buffer is an array of 32-bit words
    // addressed as elementIndex * stride + byteOffset.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (opcode == 158u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) &&
                declaration.type == Sm4OperandType::UnorderedAccessView &&
                scan.has_tokens(1)) {
                declarations[declaration.index0] = scan.read_token();
            }
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

std::map<u32, std::pair<u32, u32>>
DxbcTranslator::scan_structured_tgsm() const {
    // Windows SDK opcode 160 is dcl_tgsm_structured g#, structByteStride,
    // structCount. Groupshared memory has no element type either way, so a
    // structured declaration describes exactly the same word array a raw one
    // does — only the address arithmetic at the use site differs.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, std::pair<u32, u32>> declarations;
    if (!scan.has_tokens(2)) return declarations;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const usize operands_begin = scan.position();
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (opcode == 160u) {
            Sm4Operand declaration;
            if (!scan.decode_operand(declaration) ||
                declaration.type != Sm4OperandType::ThreadGroupSharedMemory ||
                !scan.has_tokens(2)) {
                return {};
            }
            const u32 stride = scan.read_token();
            const u32 count = scan.read_token();
            declarations[declaration.index0] = {stride, count};
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return declarations;
}

} // namespace vitreo::graphics::dxbc_spirv_detail

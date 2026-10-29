// SPDX-License-Identifier: LGPL-2.1-only

/// @file dxbc_to_spirv_scan_stage.cpp
/// @brief Stage-declaration scans of the DXBC program.

#include "graphics/shaders/dxbc_to_spirv_internal.hpp"

namespace vitreo::graphics::dxbc_spirv_detail {

bool DxbcTranslator::scan_geometry_declarations() {
    if (m_result.stage != ShaderStage::Geometry) return true;

    // Microsoft D3D10 tokenized-program declarations:
    // DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY=92, DCL_GS_INPUT_PRIMITIVE=93,
    // DCL_MAX_OUTPUT_VERTEX_COUNT=94. Primitive/topology values occupy the
    // opcode-specific control field beginning at bit 11.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return fail("geometry shader bytecode truncated");
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        if (opcode == 53u) { // CUSTOMDATA has its length in DWORD 1.
            if (!scan.has_tokens(1)) return fail("CUSTOMDATA truncated");
            const u32 block_length = scan.read_token();
            if (block_length < 2u) return fail("CUSTOMDATA invalid length");
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (length == 0u) {
            return fail("geometry shader declaration has zero length");
        }
        if (opcode == 92u) {
            const u32 topology = (token >> 11) & 0x3Fu;
            if (m_gs_output_topology != 0u &&
                m_gs_output_topology != topology) {
                return fail("conflicting geometry output topologies");
            }
            m_gs_output_topology = topology;
        } else if (opcode == 93u) {
            const u32 primitive = (token >> 11) & 0x3Fu;
            if (m_gs_input_primitive != 0u &&
                m_gs_input_primitive != primitive) {
                return fail("conflicting geometry input primitives");
            }
            m_gs_input_primitive = primitive;
        } else if (opcode == 94u) {
            if (length != 2u || !scan.has_tokens(1)) {
                return fail("invalid geometry max-output declaration");
            }
            const u32 count = scan.read_token();
            if (count == 0u) {
                return fail("geometry max-output vertex count is zero");
            }
            if (m_gs_max_output_vertices != 0u &&
                m_gs_max_output_vertices != count) {
                return fail("conflicting geometry max-output declarations");
            }
            m_gs_max_output_vertices = count;
            continue;
        } else if (opcode == 143u) {
            // dcl_stream mN. Stream 0 is the rasterized one; streams 1-3 only
            // ever feed stream output from a geometry shader, not served.
            const usize operands_begin = scan.position();
            Sm4Operand stream;
            if (!scan.decode_operand(stream) ||
                stream.type != Sm4OperandType::Stream) {
                return fail("invalid geometry stream declaration");
            }
            if (stream.index0 >= 4u) {
                return fail("geometry stream m" +
                            std::to_string(stream.index0) +
                            " is out of range");
            }
            m_gs_stream_mask |= 1u << stream.index0;
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
            continue;
        } else if (opcode == 206u) {
            // dcl_gsinstances n (Learn: [instance(n)], 1..32).
            if (length != 2u || !scan.has_tokens(1)) {
                return fail("invalid geometry instance declaration");
            }
            const u32 instances = scan.read_token();
            if (instances == 0u || instances > 32u) {
                return fail("geometry instance count " +
                            std::to_string(instances) + " is out of range");
            }
            m_gs_instance_count = instances;
            continue;
        }
        if (length > 1u) scan.skip_tokens(length - 1u);
    }

    // D3D_PRIMITIVE_POINT=1, LINE=2, TRIANGLE=3, LINE_ADJ=6 (4 vertices),
    // TRIANGLE_ADJ=7 (6 vertices); the adjacency values measured with fxc
    // (tools/oracle_probe/gs_sm5_shaders.hlsl). From here on the member holds
    // the VERTEX COUNT of one input primitive -- the width of v[n][...] --
    // and m_gs_input_adjacency says which kind it is.
    if (m_gs_input_primitive == 6u || m_gs_input_primitive == 7u) {
        m_gs_input_adjacency = true;
        m_gs_input_primitive = m_gs_input_primitive == 6u ? 4u : 6u;
    } else if (m_gs_input_primitive < 1u || m_gs_input_primitive > 3u) {
        return fail("geometry input primitive is not implemented");
    }
    // fxc leaves dcl_outputtopology out of a geometry shader that never
    // emits (measured, tools/oracle_probe/d3d11_pstats.hlsl gs_none:
    // dcl_maxout 3 and ret; Windows creates and runs it). It makes no
    // primitive, so the point list describes it as well as any.
    if (m_gs_output_topology == 0u) m_gs_output_topology = 1u;
    // D3D_PRIMITIVE_TOPOLOGY_POINTLIST=1, LINESTRIP=3,
    // TRIANGLESTRIP=5 are the only GS output topology contracts.
    if (m_gs_output_topology != 1u && m_gs_output_topology != 3u &&
        m_gs_output_topology != 5u) {
        return fail("geometry output topology is not implemented");
    }
    if (m_gs_max_output_vertices == 0u) {
        return fail("geometry max-output declaration is missing");
    }
    if (m_gs_stream_mask == 0u) m_gs_stream_mask = 1u;
    // D3D11.3 13.5.2: "When outputting to multiple streams ... [the output
    // topology] must be set to point".
    if ((m_gs_stream_mask & (m_gs_stream_mask - 1u)) != 0u &&
        m_gs_output_topology != 1u) {
        return fail("geometry shader with several streams and a non-point "
                    "output topology");
    }
    return true;
}

bool DxbcTranslator::scan_tessellation_declarations() {
    if (m_container.stage != ShaderStage::Hull &&
        m_container.stage != ShaderStage::Domain) {
        return true;
    }
    // Measured with fxc (tools/oracle_probe/tessellation_shaders.hlsl):
    // hs_decls 113, control-point / fork / join phase 114 / 115 / 116,
    // dcl_input / output_control_point_count 147 / 148 (count in bits 11+),
    // domain 149, partitioning 150, output primitive 151 (values in bits 11+),
    // dcl_hs_max_tessfactor 152 (an immediate float), fork / join phase
    // instance counts 153 / 154 (Microsoft's tokenized-program format).
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return fail("tessellation shader truncated");
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const usize at = scan.position();
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        const u32 value = (token >> 11) & 0x3Fu;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) return fail("CUSTOMDATA truncated");
            const u32 block_length = scan.read_token();
            if (block_length < 2u) return fail("CUSTOMDATA invalid length");
            scan.set_position(at + block_length);
            continue;
        }
        if (length == 0u) return fail("tessellation: zero-length token");
        if (opcode >= 114u && opcode <= 116u) {
            if (!m_hs_phases.empty()) m_hs_phases.back().end = at;
            HsPhase phase{};
            phase.kind = opcode == 114u   ? HsPhaseKind::ControlPoint
                         : opcode == 115u ? HsPhaseKind::Fork
                                          : HsPhaseKind::Join;
            phase.begin = at + length;
            m_hs_phases.push_back(phase);
        } else if (opcode == 113u) {
            if (!m_hs_phases.empty()) m_hs_phases.back().end = at;
        } else if (opcode == 147u) {
            (m_container.stage == ShaderStage::Hull ? m_hs_in_cp : m_ds_in_cp) =
                value;
        } else if (opcode == 148u) {
            m_hs_out_cp = value;
        } else if (opcode == 149u) {
            m_tess_domain = value;
        } else if (opcode == 150u) {
            m_tess_partitioning = value;
        } else if (opcode == 151u) {
            m_tess_output_primitive = value;
        } else if (opcode == 152u && length >= 2u) {
            const u32 bits = scan.read_token();
            std::memcpy(&m_hs_max_tess_factor, &bits, sizeof(bits));
        } else if ((opcode == 153u || opcode == 154u) && length >= 2u) {
            const u32 count = scan.read_token();
            if (m_hs_phases.empty() || count == 0u || count > 128u) {
                return fail("tessellation: bad phase instance count");
            }
            m_hs_phases.back().instances = count;
        }
        scan.set_position(at + length);
    }
    if (!m_hs_phases.empty()) {
        m_hs_phases.back().end = m_container.bytecode.size() / 4u;
    }
    const auto record_vec4s =
        [](const std::vector<DxbcSignatureElement>& signature) {
            u32 slots = 0;
            for (const auto& e : signature) {
                if (e.register_num != 0xFFFFFFFFu) {
                    slots = std::max(slots, e.register_num + 1u);
                }
            }
            return slots;
        };
    m_result.tess_domain = m_tess_domain;
    m_result.tess_partitioning = m_tess_partitioning;
    m_result.tess_output_primitive = m_tess_output_primitive;
    m_result.hs_input_control_points = m_hs_in_cp;
    // A domain shader reads the hull shader's output control points, and
    // declares how many with its own dcl_input_control_point_count.
    m_result.hs_output_control_points =
        m_container.stage == ShaderStage::Domain ? m_ds_in_cp : m_hs_out_cp;
    m_result.hs_max_tess_factor = m_hs_max_tess_factor;
    m_result.hs_pc_record_vec4s =
        record_vec4s(m_container.patch_constant_signature);
    if (m_container.stage == ShaderStage::Hull) {
        m_result.hs_cp_record_vec4s = record_vec4s(m_container.output_signature);
    }
    if (!m_options.emulate_tessellation) return true;

    // The software tessellator (domain_to_compute) serves every domain and
    // output primitive; Metal's own tessellates triangles and quads into
    // triangles only.
    if (m_tess_domain < 1u || m_tess_domain > 3u) {
        return fail("tessellation: no tessellator domain declared");
    }
    if (m_tess_domain == 1u && !m_options.domain_to_compute) {
        return fail("tessellation: the isoline domain has no Metal "
                    "equivalent (Metal tessellates triangles and quads)");
    }
    if (m_container.stage == ShaderStage::Domain) {
        if (m_ds_in_cp == 0u || m_ds_in_cp > 32u) {
            return fail("domain shader: bad input control point count");
        }
        return true;
    }
    if (m_tess_output_primitive < 1u || m_tess_output_primitive > 4u) {
        return fail("hull shader: unknown output primitive");
    }
    if ((m_tess_output_primitive == 1u || m_tess_output_primitive == 2u) &&
        !m_options.domain_to_compute) {
        return fail("hull shader: point and line tessellator output have no "
                    "Metal equivalent");
    }
    if (m_tess_partitioning < 1u || m_tess_partitioning > 4u) {
        return fail("hull shader: unknown partitioning");
    }
    if (m_hs_in_cp == 0u || m_hs_in_cp > 32u || m_hs_out_cp > 32u) {
        return fail("hull shader: bad control point counts");
    }
    const bool has_cp_phase = std::any_of(
        m_hs_phases.begin(), m_hs_phases.end(),
        [](const HsPhase& p) { return p.kind == HsPhaseKind::ControlPoint; });
    if (!has_cp_phase && m_hs_out_cp > m_hs_in_cp) {
        return fail("hull shader: a pass-through control-point phase with "
                    "more outputs than inputs");
    }
    return true;
}

bool DxbcTranslator::scan_compute_declarations() {
    if (m_result.stage != ShaderStage::Compute) return true;

    // Microsoft dcl_thread_group (opcode 155) appears exactly once in a
    // compute shader and carries its x/y/z literals after OpcodeToken0.
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return fail("compute shader bytecode truncated");
    scan.skip_tokens(2);
    bool found = false;
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) return fail("CUSTOMDATA truncated");
            const u32 block_length = scan.read_token();
            if (block_length < 2u) return fail("CUSTOMDATA invalid length");
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (length == 0u) {
            return fail("compute shader declaration has zero length");
        }
        if (opcode == 155u) {
            if (found || length != 4u || !scan.has_tokens(3)) {
                return fail("invalid or duplicate compute thread-group declaration");
            }
            m_compute_group_size_x = scan.read_token();
            m_compute_group_size_y = scan.read_token();
            m_compute_group_size_z = scan.read_token();
            found = true;
            continue;
        }
        if (length > 1u) scan.skip_tokens(length - 1u);
    }
    const u64 thread_count = static_cast<u64>(m_compute_group_size_x) *
                             m_compute_group_size_y *
                             m_compute_group_size_z;
    if (!found || m_compute_group_size_x == 0u ||
        m_compute_group_size_x > 1024u || m_compute_group_size_y == 0u ||
        m_compute_group_size_y > 1024u || m_compute_group_size_z == 0u ||
        m_compute_group_size_z > 64u || thread_count > 1024u) {
        return fail("compute thread-group dimensions violate cs_5_0 limits");
    }
    return true;
}

bool DxbcTranslator::declares_input_operand(Sm4OperandType type) const {
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return false;
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
        if (opcode == 95u) {  // dcl_input
            Sm4Operand operand;
            if (scan.decode_operand(operand) && operand.type == type) {
                return true;
            }
        }
        const usize consumed = 1u + (scan.position() - operands_begin);
        if (consumed < length) scan.skip_tokens(length - consumed);
    }
    return false;
}

u32 DxbcTranslator::scan_global_flags() const {
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    if (!scan.has_tokens(2)) return 0u;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        if (opcode == static_cast<u32>(Sm4Opcode::RET)) break;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (length == 0u) break;
        if (opcode == 106u) return token;
        if (length > 1u) scan.skip_tokens(length - 1u);
    }
    return 0u;
}

DxbcMathContract math_contract_of(const DxbcContainer& container) {
    // Declarations reuse OpcodeToken0 bits 11-23 for their own controls
    // (dcl_resource's sample count sits in bits 16-22), so only instruction
    // tokens are read for the precise mask (bits 19-22). Declaration opcodes
    // per the SDK token format, as decoded against fxc disassembly
    // (census-shaders.md): customdata 53, dcl_* 88-106, the hull-shader phase
    // markers 113-116, SM5 dcl_* 143-162 and dcl_gsinstances 206.
    const auto is_declaration = [](u32 opcode) {
        return opcode == 53u || (opcode >= 88u && opcode <= 106u) ||
               (opcode >= 113u && opcode <= 116u) ||
               (opcode >= 143u && opcode <= 162u) || opcode == 206u;
    };
    DxbcMathContract contract{};
    DxbcDecoder scan(container.bytecode.data(), container.bytecode.size());
    if (!scan.has_tokens(2)) return contract;
    scan.skip_tokens(2);
    while (scan.has_tokens(1)) {
        const u32 token = scan.read_token();
        const u32 opcode = token & 0x7FFu;
        const u32 length = (token >> 24) & 0x7Fu;
        if (opcode == 53u) {
            if (!scan.has_tokens(1)) break;
            const u32 block_length = scan.read_token();
            if (block_length < 2u) break;
            scan.skip_tokens(block_length - 2u);
            continue;
        }
        if (length == 0u) break;
        if (opcode == 106u) {
            // D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED (measured with fxc:
            // `dcl_globalFlags refactoringAllowed` is 0x0100086a).
            contract.refactoring_allowed = (token & (1u << 11)) != 0u;
        } else if (!is_declaration(opcode) && ((token >> 19) & 0xFu) != 0u) {
            contract.uses_precise = true;
        }
        if (length > 1u) scan.skip_tokens(length - 1u);
    }
    return contract;
}

std::map<u32, u32> DxbcTranslator::scan_input_interpolation() const {
    // Windows SDK d3d10TokenizedProgramFormat.hpp: dcl_input_ps (98),
    // dcl_input_ps_sgv (99) and dcl_input_ps_siv (100) carry the
    // interpolation mode in OpcodeToken0 bits 11-14 -- 1 constant, 2 linear,
    // 3 linear centroid, 4 linear noperspective, 5 noperspective centroid,
    // 6 linear sample, 7 noperspective sample. (Decoded against 137 fxc
    // disassembly listings with no mismatch, census-shaders.md.)
    DxbcDecoder scan(m_container.bytecode.data(), m_container.bytecode.size());
    std::map<u32, u32> modes;
    if (!scan.has_tokens(2)) return modes;
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
        if (opcode >= 98u && opcode <= 100u) {
            Sm4Operand declaration;
            if (scan.decode_operand(declaration) &&
                declaration.type == Sm4OperandType::Input) {
                modes[declaration.index0] = (token >> 11) & 0xFu;
            }
        }
        if (length > 1u) {
            const usize consumed = 1u + (scan.position() - operands_begin);
            if (consumed < length) scan.skip_tokens(length - consumed);
        }
    }
    return modes;
}

} // namespace vitreo::graphics::dxbc_spirv_detail

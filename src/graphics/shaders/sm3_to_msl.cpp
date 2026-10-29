// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl.cpp
/// @brief Direct3D 9 shader model 1–3 bytecode to Metal Shading Language.

#include "sm3_to_msl_internal.hpp"

#include <sstream>
#include <string>

namespace vitreo::graphics {
namespace sm3_msl_detail {

bool Translator::parse_version() {
    if (!has(1)) return fail("an empty token stream");
    const u32 version = take();
    const u32 kind = version >> 16;
    m_result.major = (version >> 8) & 0xFF;
    m_result.minor = version & 0xFF;
    if (kind == 0xFFFE) {
        m_result.stage = ShaderStage::Vertex;
        m_result.entry_point = "sm3_vertex";
    } else if (kind == 0xFFFF) {
        m_result.stage = ShaderStage::Fragment;
        m_result.entry_point = "sm3_fragment";
    } else {
        return fail("a token stream that is not a D3D9 shader");
    }
    if (m_result.major < 1 || m_result.major > 3) {
        return fail("shader model " + std::to_string(m_result.major) + "." +
                    std::to_string(m_result.minor));
    }
    return true;
}

bool Translator::parse_instruction(bool& finished) {
    const u32 token = take();
    const u32 opcode = token & OPCODE_MASK;

    if (opcode == OPCODE_END) {
        finished = true;
        return true;
    }
    if (opcode == OPCODE_COMMENT) {
        const u32 length = (token >> COMMENT_LENGTH_SHIFT) &
                           COMMENT_LENGTH_MASK;
        if (!has(length)) return fail("a truncated comment block");
        m_position += length;
        return true;
    }
    if ((token & PREDICATED_BIT) != 0) {
        // Predicated instructions carry one extra source (the predicate)
        // after the normal operands. Parse the instruction into a side
        // buffer, then wrap it in a predicate guard.
        return parse_predicated_instruction(token, finished);
    }

    return parse_instruction_body(token, finished);
}

bool Translator::parse_predicated_instruction(u32 token, bool& finished) {
    const std::string before = m_body.str();
    if (!parse_instruction_body(token, finished)) return false;
    if (finished) {
        return fail("a predicated END instruction");
    }
    Parameter predicate;
    if (!has(1)) return fail("a truncated predicated instruction");
    predicate = decode_source(take());
    std::string predicate_expression;
    // Any component of the predicate enables the instruction. Per-component
    // write masking would be more precise; this matches the common (p0)
    // whole-instruction form games emit.
    if (!source_expression(predicate, 0xF, predicate_expression)) {
        return false;
    }
    const std::string after = m_body.str();
    if (after.size() < before.size()) {
        return fail("a predicated instruction");
    }
    const std::string added = after.substr(before.size());
    m_body.str(std::string());
    m_body.clear();
    m_body << before;
    m_body << "    if (any(" << predicate_expression << ")) {\n";
    m_body << added;
    m_body << "    }\n";
    return true;
}

Sm3Shader Translator::run() {
    m_result.ok = true;
    if (!parse_version()) return m_result;

    bool finished = false;
    while (!finished) {
        if (!has(1)) {
            (void)fail("a token stream with no END instruction");
            return m_result;
        }
        if (!parse_instruction(finished)) return m_result;
    }

    if (m_flow_depth != 0) {
        (void)fail("an unclosed if block");
        return m_result;
    }
    if (!m_loop_stack.empty()) {
        (void)fail("an unclosed loop or rep block");
        return m_result;
    }
    if (m_current_label.has_value()) {
        (void)fail("an unclosed label block");
        return m_result;
    }
    flush_body();
    for (u32 label : m_labels_called) {
        if (m_labels_defined.count(label) == 0) {
            (void)fail("call to undefined label l" + std::to_string(label));
            return m_result;
        }
    }
    if (m_result.stage == ShaderStage::Vertex && !m_position_written) {
        (void)fail("a vertex shader that never writes a position");
        return m_result;
    }
    if (m_result.stage == ShaderStage::Fragment &&
        m_outputs_written.empty()) {
        // SM1 pixel shaders write the colour through r0 rather than oC0.
        if (m_result.major == 1 && m_temps_used.count(0) != 0) {
            m_body << "    out.color = r0;\n";
            m_outputs_written.insert(0);
            flush_body();
        } else {
            (void)fail("a pixel shader that never writes a colour");
            return m_result;
        }
    }

    m_result.color_varyings.assign(m_colors_written.begin(),
                                   m_colors_written.end());
    m_result.texcoord_varyings.assign(m_texcoords_written.begin(),
                                      m_texcoords_written.end());
    if (m_result.stage == ShaderStage::Fragment) {
        for (u32 output : m_outputs_written) {
            if (output < m_result.color_outputs_written.size()) {
                m_result.color_outputs_written[output] = true;
            }
        }
    }
    m_result.msl = build_msl();
    return m_result;
}

} // namespace sm3_msl_detail

Sm3Shader translate_sm3_to_msl(const u32* tokens, usize token_count,
                               const u32* stage_ttf, usize stage_ttf_count) {
    if (tokens == nullptr || token_count == 0) {
        Sm3Shader result;
        result.failure = "an empty token stream";
        return result;
    }
    sm3_msl_detail::Translator translator(tokens, token_count, stage_ttf,
                                          stage_ttf_count);
    return translator.run();
}

} // namespace vitreo::graphics

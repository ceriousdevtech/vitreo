// SPDX-License-Identifier: LGPL-2.1-only
/// @file sm3_to_msl_subroutines.cpp
/// @brief SM2-3 call, callnz, ret and label subroutines.

#include "sm3_to_msl_internal.hpp"

#include <sstream>
#include <string>

namespace vitreo::graphics::sm3_msl_detail {

void Translator::flush_body() {
    const std::string chunk = m_body.str();
    m_body.str(std::string());
    m_body.clear();
    if (chunk.empty()) return;
    if (m_current_label.has_value()) {
        m_label_bodies[*m_current_label] += chunk;
    } else {
        m_main_body += chunk;
    }
}

std::string Translator::subroutine_param_list() const {
    std::ostringstream ss;
    bool first = true;
    const auto add = [&](const std::string& param) {
        if (!first) ss << ", ";
        first = false;
        ss << param;
    };
    for (u32 temp : m_temps_used) {
        add("thread float4& r" + std::to_string(temp));
    }
    for (u32 texreg : m_texregs_used) {
        add("thread float4& t" + std::to_string(texreg));
    }
    if (m_address_register_used) add("thread int4& a0");
    for (u32 predicate : m_predicates_used) {
        add("thread bool4& p" + std::to_string(predicate));
    }
    // aL is shared so calls inside loop…endloop see the current counter.
    add("thread int& aL");
    if (m_result.stage == ShaderStage::Vertex) {
        add("Sm3VertexIn in");
        add("thread Sm3Varyings& out");
    } else {
        add("Sm3Varyings in");
        add("thread Sm3FragmentOut& out");
    }
    add("constant Sm3Constants& cb");
    for (u32 sampler = 0; sampler < m_result.samplers_used.size();
         ++sampler) {
        if (!m_result.samplers_used[sampler]) continue;
        const u8 dim = m_sampler_dim[sampler];
        const char* texture_ty = "texture2d";
        if (dim == 3) texture_ty = "texturecube";
        else if (dim == 4) texture_ty = "texture3d";
        add(std::string(texture_ty) + "<float> tex" +
            std::to_string(sampler));
        add("sampler samp" + std::to_string(sampler));
    }
    return ss.str();
}

std::string Translator::subroutine_arg_list() const {
    std::ostringstream ss;
    bool first = true;
    const auto add = [&](const std::string& arg) {
        if (!first) ss << ", ";
        first = false;
        ss << arg;
    };
    for (u32 temp : m_temps_used) add("r" + std::to_string(temp));
    for (u32 texreg : m_texregs_used) add("t" + std::to_string(texreg));
    if (m_address_register_used) add("a0");
    for (u32 predicate : m_predicates_used) {
        add("p" + std::to_string(predicate));
    }
    add("aL");
    add("in");
    add("out");
    add("cb");
    for (u32 sampler = 0; sampler < m_result.samplers_used.size();
         ++sampler) {
        if (!m_result.samplers_used[sampler]) continue;
        add("tex" + std::to_string(sampler));
        add("samp" + std::to_string(sampler));
    }
    return ss.str();
}

std::string Translator::expand_calls(std::string body) const {
    const std::string args = subroutine_arg_list();
    for (u32 label : m_labels_called) {
        const std::string needle = "__sm3_call_" + std::to_string(label) + "()";
        const std::string repl =
            "sm3_l" + std::to_string(label) + "(" + args + ")";
        for (;;) {
            const auto pos = body.find(needle);
            if (pos == std::string::npos) break;
            body.replace(pos, needle.size(), repl);
        }
    }
    return body;
}

void Translator::emit_call(u32 label) {
    m_uses_subroutines = true;
    m_labels_called.insert(label);
    m_body << "    __sm3_call_" << label << "();\n";
}

} // namespace vitreo::graphics::sm3_msl_detail

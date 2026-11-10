// SPDX-License-Identifier: LGPL-2.1-only

/// @file msl_minmax_fence.cpp
/// @brief The MIN/MAX reduction switch in translated MSL
///        (graphics/shaders/minmax_reduction.hpp).
///
/// SPIRV-Cross writes the specialization constant as a Metal function
/// constant:
///     constant bool <name>_tmp [[function_constant(<id>)]];
///     constant bool <name> = is_function_constant_defined(<name>_tmp) ? ... ;
/// A function constant is resolved when a function is specialised, after
/// the library compile that is most of a shader's cost, so both lines become
/// the preprocessor switch instead:
///     #ifndef VITREO_MINMAX_REDUCTION
///     #define VITREO_MINMAX_REDUCTION 0
///     #endif
///     #define <name> (VITREO_MINMAX_REDUCTION != 0)
/// and each branch the translator gated on it,
///     <ind>if (<name> && <record flag>)
///     <ind>{
///     <ind>    ... the reduction ...
///     <ind>}
///     <ind>else
///     <ind>{
///     <ind>    ... the ordinary sample ...
///     <ind>}
/// is fenced as `#if VITREO_MINMAX_REDUCTION` from the `if` through the
/// `else`, so with the switch 0 the compiler never sees the reduction and
/// the ordinary arm is a plain block. A branch in any other shape keeps the
/// (constant-false) condition, which is still exact. When every gated
/// branch is fenced, so are the out-of-line reduction helpers only they
/// call.

#include "graphics/shaders/minmax_reduction.hpp"
#include "graphics/shaders/spirv_to_msl.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace vitreo::graphics {

static void fence_sampler_feature(std::string& msl, unsigned spec_id,
                                  const char* macro_name, bool fence_helpers) {
    const std::string id_attr =
        "[[function_constant(" + std::to_string(spec_id) + ")]];";
    const usize attr = msl.find(id_attr);
    if (attr == std::string::npos) return;
    // The two declaration lines.
    const usize decl_start = msl.rfind('\n', attr) + 1;  // npos + 1 == 0
    const usize decl_end = msl.find('\n', attr);
    if (decl_end == std::string::npos) return;
    const usize value_start = decl_end + 1;
    const usize value_end = msl.find('\n', value_start);
    if (value_end == std::string::npos) return;
    const std::string value_line = msl.substr(value_start, value_end - value_start);
    const std::string prefix = "constant bool ";
    if (value_line.rfind(prefix, 0) != 0) return;
    const usize name_end = value_line.find(' ', prefix.size());
    if (name_end == std::string::npos) return;
    const std::string name = value_line.substr(prefix.size(), name_end - prefix.size());
    if (value_line.find("is_function_constant_defined(", name_end) == std::string::npos) {
        return;
    }
    const std::string macro = macro_name;
    const std::string replacement = "#ifndef " + macro + "\n#define " + macro +
                                    " 0\n#endif\n#define " + name + " (" + macro +
                                    " != 0)";
    msl.replace(decl_start, value_end - decl_start, replacement);

    // Fence the gated branches, line by line.
    std::vector<std::string> lines;
    for (usize at = 0; at <= msl.size();) {
        const usize eol = msl.find('\n', at);
        if (eol == std::string::npos) {
            lines.push_back(msl.substr(at));
            break;
        }
        lines.push_back(msl.substr(at, eol - at));
        at = eol + 1;
    }
    const std::string opener = "if (" + name + " && ";
    std::vector<usize> fence_open;   // line index the #if goes before
    std::vector<usize> fence_close;  // line index the #endif goes after
    for (usize i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        const usize indent = line.find_first_not_of(' ');
        if (indent == std::string::npos || line.compare(indent, opener.size(), opener) != 0 ||
            line.back() != ')') {
            continue;
        }
        const std::string pad(indent, ' ');
        if (i + 1 >= lines.size() || lines[i + 1] != pad + "{") continue;
        usize close = i + 2;
        while (close < lines.size() && lines[close] != pad + "}") ++close;
        if (close + 2 >= lines.size() || lines[close + 1] != pad + "else" ||
            lines[close + 2] != pad + "{") {
            continue;
        }
        fence_open.push_back(i);
        fence_close.push_back(close + 1);
    }
    if (fence_open.empty()) return;
    // With every gated branch fenced nothing else calls the out-of-line
    // reductions (emit_minmax_sample_call's helpers): fence their
    // definitions too, from `static __attribute__((noinline))` to the
    // top-level `}` that closes the function.
    usize gates = 0;
    for (const std::string& line : lines) {
        if (line.find(opener) != std::string::npos) ++gates;
    }
    if (fence_helpers && gates == fence_open.size()) {
        for (usize i = 0; i + 1 < lines.size(); ++i) {
            if (lines[i] != "static __attribute__((noinline))" ||
                lines[i + 1].find(" vitreoMinMaxSample") == std::string::npos) {
                continue;
            }
            usize end = i + 2;
            while (end < lines.size() && lines[end] != "}") ++end;
            if (end >= lines.size()) break;
            fence_open.push_back(i);
            fence_close.push_back(end);
            i = end;
        }
    }
    std::sort(fence_open.begin(), fence_open.end());
    std::sort(fence_close.begin(), fence_close.end());
    std::string out;
    out.reserve(msl.size() + fence_open.size() * 48);
    usize next_open = 0;
    usize next_close = 0;
    for (usize i = 0; i < lines.size(); ++i) {
        while (next_open < fence_open.size() && fence_open[next_open] == i) {
            out += "#if " + macro + "\n";
            ++next_open;
        }
        out += lines[i];
        if (i + 1 < lines.size()) out += '\n';
        while (next_close < fence_close.size() && fence_close[next_close] == i) {
            out += "#endif\n";
            ++next_close;
        }
    }
    msl.swap(out);
}

void fence_sampler_features(std::string& msl) {
    fence_sampler_feature(msl, kMinMaxReductionSpecId, kMinMaxReductionMacro, true);
    fence_sampler_feature(msl, kCustomBorderSpecId, kCustomBorderMacro, false);
    fence_sampler_feature(msl, kNonNormalizedSpecId, kNonNormalizedMacro, false);
}

void fence_minmax_reduction(std::string& msl) {
    fence_sampler_features(msl);
}

}  // namespace vitreo::graphics

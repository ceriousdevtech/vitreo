// SPDX-License-Identifier: LGPL-2.1-only
/// @file msl_point_size.hpp
/// @brief The one-pixel point of a vertex function that writes no size.
///
/// D3D11.3 3.4.6: a point rasterizes as a square of width 1, and no D3D10+
/// stage has a point-size output. Metal leaves the size undefined unless the
/// vertex function writes [[point_size]] (MTLPrimitiveType.point), and it
/// refuses a pipeline whose vertex function writes one when the pipeline's
/// inputPrimitiveTopology is a line or triangle class (measured,
/// tools/oracle_probe/metal/metal_restart_point_facts.m). So the size is
/// added only to a pipeline that draws points: an SPIRV-Cross vertex
/// function (`struct main0_out`, `main0_out out = {};`) gains
/// `[[point_size]] = 1.0`.

#pragma once

#include <string>
#include <string_view>

namespace vitreo::graphics {

/// @p vertex with [[point_size]] = 1.0, unchanged when it already writes a
/// size or is not shaped as an SPIRV-Cross vertex function with an output.
[[nodiscard]] inline std::string msl_with_unit_point_size(std::string vertex) {
    if (vertex.find("[[point_size") != std::string::npos) return vertex;
    const std::size_t output_struct = vertex.find("struct main0_out");
    if (output_struct == std::string::npos) return vertex;
    const std::size_t output_end = vertex.find("};", output_struct);
    if (output_end == std::string::npos) return vertex;
    std::size_t declared = std::string::npos;
    std::size_t after = 0;
    for (const std::string_view form : {"main0_out out = {};", "main0_out out;"}) {
        declared = vertex.find(form, output_end);
        if (declared != std::string::npos) {
            after = declared + form.size();
            break;
        }
    }
    if (declared == std::string::npos) return vertex;
    // The assignment first: inserting the member shifts the later offset.
    vertex.insert(after, "\n    out.vitreo_point_size = 1.0;");
    vertex.insert(output_end, "    float vitreo_point_size [[point_size]];\n");
    return vertex;
}

}  // namespace vitreo::graphics

// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file msl_mesh_emit.hpp
/// @brief A D3D11 geometry shader drawn through a Metal mesh pipeline (WS5,
///        host-offload plan WS5): the mesh
///        limits, whether a shader fits them, and the MSL the mesh compiler
///        (spirv_to_msl) writes around the translated GS body.
///
/// The translator lowers the GS exactly as for the three-pass compute
/// expansion, except that the records and per-invocation counts go to
/// threadgroup memory (DxbcTranslationOptions::emulate_geometry_with_mesh).
/// One mesh threadgroup runs K invocations, one thread each; invocation k
/// owns mesh vertex slots [k * vpi, (k + 1) * vpi), the same fixed region it
/// owns of vitreo_gs_out in the compute lowering, and every list primitive
/// whose last vertex is past what its invocation wrote is culled -- the mesh
/// form of pass C's cull of the unwritten tail.

#include "graphics/shaders/gs_passthrough.hpp"

#include <string>
#include <vector>

namespace vitreo::graphics {

// Limits. Apple: WWDC22 session 10162 ("Transform your geometry with Metal
// mesh shaders"), MSL 4.1 Table 2.13 (set_index takes uchar indices here,
// so at most 256 vertices), Metal Feature Set Tables (May 2026).
inline constexpr u32 kMeshMaxVertices = 256;              // WWDC22 10162; MSL Table 2.13
inline constexpr u32 kMeshMaxPrimitives = 512;            // WWDC22 10162
inline constexpr u32 kMeshMaxBytes = 16384;               // WWDC22 10162: metal::mesh <= 16 KB
inline constexpr u32 kMeshPayloadBytes = 16;              // <= 16384 (FST "Maximum payload", fn 11)
inline constexpr u32 kMaxMeshGroupsPerObjectGroup = 1024; // WWDC22; FST Apple7/8 mesh grid
inline constexpr u32 kMaxThreadsPerGroup = 1024;          // FST
inline constexpr u32 kThreadgroupMemoryBytes = 32768;     // FST Apple4+

/// What one shader needs of a mesh threadgroup (gs_mesh_fit).
struct GsMeshFit {
    bool fits{false};
    u32 invocations_per_group{0};  ///< K
    u32 vertex_bytes{0};           ///< estimated sizeof(vertex)
    u32 primitive_bytes{0};        ///< estimated sizeof(primitive)
    u32 mesh_bytes{0};             ///< NV*V + NP*P + NP*indices per primitive
    u32 scratch_bytes{0};          ///< K*vpi*record_vec4s*16 + K*4
    const char* why{nullptr};      ///< set when !fits
};

/// Vertices per output list primitive: 1 point, 2 line, 3 triangle.
[[nodiscard]] constexpr u32 gs_mesh_vertices_per_primitive(u32 output_topology) {
    return output_topology == 1 ? 1u : output_topology == 3 ? 2u : 3u;
}

/// The largest K (invocations per mesh threadgroup) that satisfies every
/// limit at once: K*vpi <= 256, K*vpi/vpp <= 512, mesh bytes <= 16384,
/// threadgroup scratch <= 32768, K <= 1024. `outputs` is the rasterized
/// stream's signature as build_geometry_program builds it.
[[nodiscard]] GsMeshFit gs_mesh_fit(const std::vector<GsPassthroughOutput>& outputs,
                                    u32 output_topology, u32 max_output_vertices);

/// The shape one mesh-lowered shader was compiled for.
struct GsMeshLayout {
    std::vector<GsPassthroughOutput> outputs;  ///< as build_geometry_program
    u32 output_topology{0};                    ///< DXBC 1/3/5
    u32 vertices_per_invocation{0};
    u32 invocations_per_group{0};              ///< K
    u32 record_vec4s{0};
};

inline constexpr const char* kGsMeshObjectEntryPoint = "vitreo_gs_object";
inline constexpr const char* kGsMeshEntryPoint = "main0";

/// The vertex, primitive and payload types, the metal::mesh alias and the
/// object function that launches the mesh grid in chunks of at most
/// kMaxMeshGroupsPerObjectGroup (its object-stage buffer 0 holds the total
/// number of mesh threadgroups). One statement per element.
[[nodiscard]] std::vector<std::string> gs_mesh_preamble(const GsMeshLayout&);
/// The parameters placed before SPIRV-Cross's own entry arguments, ending in
/// ", ".
[[nodiscard]] std::string gs_mesh_entry_parameters(const GsMeshLayout&);
/// The expression for Vitreo.MeshGS InvocationIndex.
[[nodiscard]] std::string gs_mesh_invocation_expression(const GsMeshLayout&);
/// The expression for Vitreo.MeshGS LocalIndex.
[[nodiscard]] std::string gs_mesh_local_expression();
/// The statements for Vitreo.MeshGS Epilogue, given SPIRV-Cross's names for
/// the scratch and counts arrays and the depth-offset expression.
[[nodiscard]] std::vector<std::string> gs_mesh_epilogue(
    const GsMeshLayout&, const std::string& scratch, const std::string& counts,
    const std::string& depth_offset);

}  // namespace vitreo::graphics

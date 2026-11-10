// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file mesh_stage.hpp
/// @brief D3D12 mesh and amplification shaders (SM 6.5; DirectX-Specs
///        "Mesh Shader") drawn through a Metal object + mesh pipeline.
///
/// Path: DXIL -> SM 5.1 COMPUTE program + VXMS chunk + MeshOp extension
/// instructions (dxil_lower_mesh.inc) -> GLCompute SPIR-V whose body runs as
/// a function, followed by a workgroup barrier and a Vitreo.Mesh epilogue
/// marker (dxbc_to_spirv_mesh.cpp) -> MSL, where spirv_to_msl's mesh
/// compiler makes the entry `[[mesh]]` / `[[object]]` and expands the
/// markers with the functions below.
///
/// A mesh shader's outputs may be written piecewise by any thread
/// ("Shared Output Arrays"), and metal::mesh only takes whole vertices
/// and primitives, so the outputs are staged in threadgroup memory as
/// 32-bit scalars and copied into the metal::mesh after the barrier. Only
/// what is consumed is staged: SV_Position, clip / cull distances, the
/// per-primitive system values and each element the pixel shader reads,
/// linked by semantic name and index ("Signature Linkage").

#include "graphics/contracts/backend_bindings.hpp"
#include "graphics/shaders/dxbc_parser.hpp"

#include <array>
#include <string>
#include <vector>

namespace vitreo::graphics {

inline constexpr u16 kMeshNoSlot = 0xFFFF;

/// The extended-instruction import of a lowered mesh / amplification
/// shader. Like Vitreo.MeshGS, only spirv_to_msl's mesh compiler gives it
/// meaning.
inline constexpr const char* kMeshStageExtInstSet = "Vitreo.Mesh";
/// (payload_var, thread index) -> void: copy the object payload into the
/// mesh shader's groupshared copy (the translated body is a helper function,
/// which cannot name the entry's [[payload]] argument). DirectX-Specs counts
/// the payload against the mesh shader's 28k of groupshared memory.
inline constexpr u32 kMeshStagePayloadCopy = 1;
/// (vertices_var, primitives_var, indices_var, counts_var, thread index,
/// depth offset) -> void: write the metal::mesh.
inline constexpr u32 kMeshStageEpilogue = 2;
/// (payload groupshared var, grid_var, thread index) -> void: write the
/// object payload and the mesh grid.
inline constexpr u32 kObjectStageEpilogue = 3;

/// The mesh statistics record: ASInvocations, MSInvocations, MSPrimitives
/// (every primitive a group outputs, culled ones included: OPTIONS12
/// MSPrimitivesPipelineStatisticIncludesCulledPrimitives TRUE) and
/// CInvocations (the primitives not culled, which reach the clipper).
inline constexpr u32 kMeshStatAs = 0, kMeshStatMs = 1, kMeshStatPrimitives = 2,
                     kMeshStatClipper = 3, kMeshStatClipped = 4, kMeshStatCounters = 5;
inline constexpr u32 kMeshStatWords = kMeshStatCounters * 2;
/// Object and mesh stage buffer index of the record: the fragment stage's
/// statistics index, unused by both stages (they bind only the push block
/// at 0 and the descriptor tables).
inline constexpr unsigned kMeshStatisticsBufferIndex = BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX;
/// Device staging (MeshStageLayout::device_staging): the scratch the mesh
/// stage stages its outputs in, one region of device_staging_words() per
/// threadgroup slot, and the uint4 added to SV_GroupID of the stage the
/// grid launches (the mesh stage without an amplification shader, else the
/// object stage) when a dispatch is drawn in chunks. Free mesh / object
/// buffer indices of the GS expansion range (a D3D12 mesh draw binds none).
inline constexpr unsigned kMeshScratchBufferIndex = BACKEND_GS_EXPANSION_BUFFER_BASE + 1;
inline constexpr unsigned kMeshOriginBufferIndex = BACKEND_GS_EXPANSION_BUFFER_BASE + 2;
/// The mesh threadgroups one object threadgroup may launch (Metal Feature
/// Set Tables, mesh grid): a chunk of N object groups has N times this
/// scratch slots, which its amplified mesh groups take from a counter.
inline constexpr u32 kMeshSlotsPerObjectGroup = 1024;
/// Raster capture (MeshStageLayout::raster_capture): the buffer the mesh
/// epilogue appends each surviving primitive's vertex records to, on the
/// mesh stage. Words: [0] the primitives appended (one atomic add per
/// group), [1] the capacity in primitives, [2..3] 0, then from word 4 three
/// records of capture.size() uint4 per primitive (raster_emulation.hpp
/// pass C reads them as consecutive records, limit word 0).
inline constexpr unsigned kMeshCaptureBufferIndex = BACKEND_GS_EXPANSION_BUFFER_BASE + 3;
inline constexpr u32 kMeshCaptureHeaderWords = 4;

/// What one translation of a mesh or amplification shader was built for.
struct MeshStageLayout {
    DxbcMeshState state;
    /// Mesh: staging slot of output register r component c at [r * 4 + c]
    /// (OSGN for vertices, PCSG for primitives); kMeshNoSlot = not staged
    /// (its stores are dropped: nothing downstream reads them).
    std::array<u16, 128> vertex_slot{};
    std::array<u16, 128> primitive_slot{};
    u16 cull_primitive_slot{kMeshNoSlot};
    u32 vertex_scalars{0};
    u32 primitive_scalars{0};
    /// One user varying of the mesh's vertex or primitive struct: the pixel
    /// shader input register it feeds ([[user(locnN)]]), its MSL type (the
    /// D3D_REGISTER_COMPONENT_TYPE of the register's first element: 1 uint,
    /// 2 int, else float) and the staging slot of each component.
    struct Field {
        u32 location{0};
        u8 component_type{3};
        bool per_primitive{false};
        std::array<u16, 4> slot{kMeshNoSlot, kMeshNoSlot, kMeshNoSlot, kMeshNoSlot};
    };
    std::vector<Field> fields;
    std::array<u16, 4> position{kMeshNoSlot, kMeshNoSlot, kMeshNoSlot, kMeshNoSlot};
    /// The PIPELINE_STATISTICS variant: the epilogues add to the record at
    /// kMeshStatisticsBufferIndex (kMeshStat* counters, each a 64-bit
    /// {low, high} pair of 32-bit words).
    bool statistics{false};
    /// The statistics variant's clipper (CPrimitives, kMeshStatClipped):
    /// the rasterizer state's DepthClipEnable.
    bool depth_clip{true};
    std::vector<u16> clip_distance;
    std::vector<u16> cull_distance;
    u16 layer{kMeshNoSlot};
    u16 viewport{kMeshNoSlot};
    /// The staged vertices, primitives and indices do not fit Metal's 32 KB
    /// of threadgroup memory beside the shader's groupshared memory: they
    /// are staged in device memory instead, in the threadgroup's slot of
    /// the scratch at kMeshScratchBufferIndex (mesh_stage_device_staging).
    /// Set on the amplification layout too (its SV_GroupID takes the chunk
    /// origin); an amplified mesh group takes its slot from the chunk's
    /// counter in the scratch (word vitreo_origin.w - 2).
    bool device_staging{false};
    /// Target-independent / conservative rasterization: the epilogue draws
    /// nothing (primitive count 0) and appends the records of every
    /// primitive not culled to the buffer at kMeshCaptureBufferIndex,
    /// which the raster-emulation pass C draws.
    bool raster_capture{false};
    /// One uint4 of a captured vertex record (mesh_stage_capture_layout),
    /// in record order (ascending register_num): a pixel input register
    /// (per-primitive values repeated on the three vertices), SV_Position,
    /// clip distances (system value 2), the render-target array / viewport
    /// index (4 / 5, from the primitive).
    struct CaptureRecord {
        u32 register_num{0};
        u8 component_type{3};
        bool position{false};
        bool per_primitive{false};
        std::array<u16, 4> slot{kMeshNoSlot, kMeshNoSlot, kMeshNoSlot, kMeshNoSlot};
        std::array<u8, 4> system_values{};
    };
    std::vector<CaptureRecord> capture;
    /// The v# registers the pixel shader reads with GetAttributeAtVertex
    /// (DxbcToSpirvResult::per_vertex_inputs): each primitive carries its
    /// vertices' values of the register's field, flat, at
    /// per_vertex_input_location(r, 0..2) in its index order ("the vertex
    /// order of the primitive" is the order the mesh shader's indices name);
    /// a line's third and a register no output feeds read 0.
    u32 per_vertex_registers{0};
    /// View instancing tier 3 (DirectX-Specs "View Instancing": the stages
    /// before the first one reading SV_ViewID run once): kMeshViCapture*.
    /// An amplification capture writes each group's DispatchMesh grid and
    /// payload to its record (mesh_vi_as_record_words) in the buffer at
    /// kMeshViewCaptureBufferIndex and launches no mesh group; a mesh
    /// capture writes each group's staged outputs (mesh_vi_ms_record_words)
    /// there as well as drawing them. Records are indexed by the group's
    /// linear threadgroup index (x fastest), plus, with vi_payload_base, the
    /// base the replayed amplification group passes in the payload word
    /// after the D3D payload.
    /// Pixel input register r fed by vertex AND primitive outputs: the
    /// components (mask) its primitive-fed elements take; they travel in a
    /// primitive field at primitive_split_input_location(r)
    /// (DxbcTranslationOptions::pixel_primitive_split).
    std::array<u8, 32> ps_primitive_split{};
    u32 vi_capture{0};
    bool vi_payload_base{false};
    /// With vi_payload_base: the payload word holding the base (the D3D
    /// payload's word count).
    u32 vi_base_word{0};

    [[nodiscard]] u32 vertices_per_primitive() const { return state.topology == 1 ? 2u : 3u; }
    [[nodiscard]] u32 thread_count() const {
        return state.threads[0] * state.threads[1] * state.threads[2];
    }
    /// Threadgroup bytes the staging takes (with the program's groupshared
    /// memory, what Metal counts against its threadgroup limit).
    [[nodiscard]] u32 staging_bytes() const;
    /// Device staging: the 32-bit words of one threadgroup's scratch slot
    /// (vertices, primitives, indices; each 16-byte aligned).
    [[nodiscard]] u32 device_staging_words() const;
};

/// Builds the layout of a lowered mesh shader `ms` feeding a pixel shader
/// with input signature `ps_inputs` (null: no pixel shader). False with
/// `why` set when the pair cannot link: `invalid` true for what D3D12
/// refuses (a pixel input no mesh output provides), false for what Vitreo
/// does not implement.
[[nodiscard]] bool build_mesh_stage_layout(const DxbcContainer& ms,
                                           const std::vector<DxbcSignatureElement>* ps_inputs,
                                           MeshStageLayout& out, std::string& why, bool& invalid);
/// Raster capture: fills `capture` (and sets raster_capture) for a layout
/// built with its pixel shader. Pass C's own varyings sit at
/// [[user(locn26..31)]], so the registers it adds (position, clip
/// distances, layer, viewport) take free registers below 26. False with
/// `why` when they do not fit or the topology is lines.
[[nodiscard]] bool mesh_stage_capture_layout(MeshStageLayout& layout, std::string& why);
/// An amplification shader's layout (no outputs).
[[nodiscard]] MeshStageLayout amplification_stage_layout(const DxbcContainer& as);

inline constexpr u32 kMeshViCaptureNone = 0, kMeshViCaptureAmplification = 1, kMeshViCaptureMesh = 2;
/// Object and mesh stage buffer index of the view-instancing capture
/// records: the vertex-capture hole, which neither stage binds.
inline constexpr unsigned kMeshViewCaptureBufferIndex = BACKEND_VERTEX_CAPTURE_BUFFER_INDEX;
/// An amplification group's record: its grid x, y, z, the mesh-record base
/// of its first mesh group (filled by the frontend), then the payload.
inline constexpr u32 kMeshViAsHeaderWords = 4;
[[nodiscard]] constexpr u32 mesh_vi_as_record_words(u32 payload_words) {
    return kMeshViAsHeaderWords + payload_words;
}
/// A mesh group's record: vertex count, primitive count, the depth offset's
/// bits, then the staged vertex words, primitive words and index words.
[[nodiscard]] u32 mesh_vi_ms_record_words(const MeshStageLayout&);
/// An object function that replays a captured amplification group: the
/// record at its linear threadgroup index gives the payload (and, with
/// @p base, the record base after it) and the mesh grid.
[[nodiscard]] std::string mesh_vi_object_replay_msl(const MeshStageLayout& as, u32 payload_words, bool base);
/// A mesh function that replays captured mesh groups: group g draws record
/// g through the epilogue of @p ms (statistics off).
[[nodiscard]] std::string mesh_vi_mesh_replay_msl(const MeshStageLayout& ms);

/// The MSL entry points of the two functions.
inline constexpr const char* kMeshStageEntryPoint = "main0";

/// Types placed before the entry: the payload struct (`payload_words`,
/// shared by the object and mesh functions) and, for a mesh shader, the
/// vertex and primitive structs and the metal::mesh alias.
[[nodiscard]] std::vector<std::string> mesh_stage_preamble(const MeshStageLayout&, u32 payload_words);
/// The qualifier and the parameters placed before SPIRV-Cross's own entry
/// arguments (each list ends in ", ").
[[nodiscard]] std::string mesh_stage_qualifier(const MeshStageLayout&);
[[nodiscard]] std::string mesh_stage_parameters(const MeshStageLayout&, u32 payload_words);
/// A mesh shader fed by an amplification shader (@p payload_words != 0)
/// reads the payload where Metal keeps it, the entry's object_data
/// [[payload]], instead of a threadgroup copy: rewrites the translated MSL so
/// the helper takes `const object_data uint (&vitreo_ms_payload)[N]` bound
/// to vitreo_payload.w, and drops the copy and its threadgroup array. The
/// payload then costs none of Metal's 32 KB of threadgroup memory. False
/// (the MSL unchanged) when the expected shape is not there.
[[nodiscard]] bool mesh_stage_direct_payload(std::string& msl, u32 payload_words);
/// An object (amplification) function: every buffer member of its argument
/// buffers becomes the ulong it is (a Metal 3 argument buffer holds a
/// buffer as its 64-bit GPU address) and each use casts it back to the
/// member's pointer type. Measured on an M4 Pro (macOS 26): an object
/// function that loads through a pointer member of an argument buffer and
/// writes the result to the payload or the mesh grid kills Metal's compiler
/// service (newRenderPipelineStateWithMeshDescriptor fails with
/// XPC_ERROR_CONNECTION_INTERRUPTED); the same load through a pointer cast
/// from a ulong compiles. Returns how many members were rewritten.
u32 object_stage_buffer_addresses(std::string& msl);
/// Device staging (MeshStageLayout::device_staging): rewrites the
/// translated mesh MSL so the vertex, primitive and index arrays are
/// pointers into the entry's slot of the scratch instead of threadgroup
/// arrays, and offsets SV_GroupID by the chunk origin when the mesh stage is
/// what the grid launches. False (the MSL unchanged) when the expected shape
/// is not there.
[[nodiscard]] bool mesh_stage_device_staging(std::string& msl, const MeshStageLayout&, u32 payload_words);
/// The object function of a device-staging pipeline: SV_GroupID offset by
/// the chunk origin. False when the expected shape is not there.
[[nodiscard]] bool object_stage_device_staging(std::string& msl);
/// Threadgroup bytes of the staging that stay when the payload is read in
/// place (mesh_stage_direct_payload).
[[nodiscard]] u32 mesh_stage_threadgroup_bytes(const MeshStageLayout&, bool payload_in_place);
[[nodiscard]] std::vector<std::string> mesh_stage_payload_copy(const MeshStageLayout&, u32 payload_words,
                                                             const std::string& copy, const std::string& tid);
/// @p linear: the group's linear threadgroup index (vi_capture only).
[[nodiscard]] std::vector<std::string> mesh_stage_epilogue(
    const MeshStageLayout&, const std::string& vertices, const std::string& primitives,
    const std::string& indices, const std::string& counts, const std::string& tid,
    const std::string& depth_offset, const std::string& linear = {});
[[nodiscard]] std::vector<std::string> object_stage_epilogue(
    const MeshStageLayout&, u32 payload_words, const std::string& payload,
    const std::string& grid, const std::string& tid, const std::string& linear = {});

}  // namespace vitreo::graphics

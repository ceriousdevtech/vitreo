// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file spirv_to_msl.hpp
/// @brief SPIR-V → Metal Shading Language compiler wrapper using SPIRV-Cross.

#include "shader_types.hpp"
#include "shader_reflection.hpp"

#include <array>
#include <utility>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// MSL Compilation Options
// =============================================================================

/// What a descriptor set's slots are declared as in the shader.
///
/// Only affects the padding SPIRV-Cross inserts for slots the shader does not
/// declare. Every class occupies 8 bytes in a Tier-2 argument buffer — a texture
/// and a sampler are each an `MTLResourceID`, a buffer is a 64-bit GPU address —
/// so the class changes what an absent slot is *spelled* as, never where the
/// next slot begins.
enum class MslDescriptorClass : u8 { Texture, Buffer, Sampler };

/// One descriptor set's argument-buffer layout.
///
/// Our DXBC translator assigns one set per D3D register space: 0 = b#,
/// 1 = t#, 2 = s#, 3 = u#. A set with `slot_count == 0` keeps its discrete
/// (flat) `[[buffer(n)]]` / `[[texture(n)]]` bindings.
struct MslArgumentBufferSet {
    u32 slot_count = 0;
    /// Metal buffer index the set's argument buffer binds at.
    u32 metal_buffer_index = 0;
    MslDescriptorClass slot_class = MslDescriptorClass::Texture;
};

inline constexpr usize MSL_DESCRIPTOR_SET_COUNT = 4;

/// OpDecorateString UserSemantic on a fragment shader's SampleMask input: it
/// is D3D's vCoverage, the pixel's raster coverage at pixel and at sample
/// frequency alike (Windows, d3d11_ps_uav_msaa_probe "sample": every
/// sample's invocation reads 0xf on a fully covered 4x pixel) -- which is
/// what Metal's [[sample_mask]] delivers to every per-sample invocation.
/// compile_spirv_to_msl then reads [[sample_mask]] unchanged instead of
/// SPIRV-Cross's sample-rate reduction to the invocation's own sample.
inline constexpr const char* kRasterCoverageSemantic =
    "vitreo.raster_coverage";

// ---- WS5 begin ----
struct GsMeshLayout;  // msl_mesh_emit.hpp
// ---- WS5 end ----

struct MslCompileOptions {
    u32 msl_version = 30000;        // Metal 3.0: MTLResourceID descriptors
    u32 platform = 1;               // 0=iOS, 1=macOS
    /// Emit argument buffers for the sets named in `argument_buffer_sets`.
    ///
    /// Defaults to false: a shader compiled with argument buffers can only be
    /// fed by a backend that binds descriptor tables, so a caller that has not
    /// thought about it must get the flat path. This used to default to true
    /// while all three production call sites overrode it to false, which meant
    /// a new call site would silently have got bindings nothing could satisfy.
    bool argument_buffers = false;
    bool force_native_arrays = true;
    bool pad_fragment_output = false;
    /// Fragment: the output locations the MSL writes (bit n: [[color(n)]]).
    /// An output whose bit is clear is left out of the fragment's return
    /// struct: the pipeline has no colour attachment there (D3D12: an
    /// SV_Target past NumRenderTargets or with an UNKNOWN format, whose
    /// writes are discarded). All bits by default.
    u32 fragment_output_mask = 0xFFFFFFFFu;
    bool dual_source_blending = false; // Fragment locations 0/1 feed RT0 sources 0/1.
    /// SubpassData inputs become Metal framebuffer fetch ([[color(n)]] at
    /// their InputAttachmentIndex): the D3D11 logic-op variant
    /// (DxbcTranslationOptions::logic_op). Apple GPUs only.
    bool framebuffer_fetch = false;
    /// Lower a precise (NoContraction) FAdd/FSub/FMul/Dot to an
    /// always_inline helper whose own contraction and reassociation are
    /// off, instead of SPIRV-Cross's optnone spvFAdd/spvFSub/vitreoFMul.
    /// The rest of the shader still contracts. Exact when the library is
    /// MTLMathModeSafe (no reassociation, INF/NaN kept): Safe alone still
    /// fuses a non-precise a*b+c, which D3D11.3 22.19.2 allows wherever
    /// refactoringAllowed is set. Measured on Apple silicon: a file-scope
    /// `#pragma clang fp contract(off)` is bit-identical to the optnone
    /// helpers and 2.2x faster, but it also refuses that legal FMA.
    bool precise_without_helpers = false;
    /// With `precise_without_helpers`: file-scope
    /// `#pragma clang fp contract(off)` and plain precise operators, so
    /// nothing in the shader fuses. D3D11.3 22.3.2, for a shader that has
    /// a precise op and whose global flags omit refactoringAllowed.
    bool fp_contract_off = false;
    u32 buffer_size_buffer_index = 25; // Metal buffer index for buffer-size auxiliary
    /// Metal buffer index of the SPIR-V push-constant block (the D3D11
    /// frontend's 256-byte inline block: sampler LOD biases, border colours,
    /// buffer view sizes). The host binds it at
    /// BACKEND_INLINE_CONSTANT_BUFFER_INDEX (0) for every stage, so it is
    /// pinned here instead of left to SPIRV-Cross's next-free-index rule,
    /// which put it at buffer(5) behind argument buffers.
    u32 push_constant_buffer_index = 0;
    /// (descriptor set, binding) of every rasterizer-ordered UAV
    /// (DxbcResolvedBinding with `declaration.rasterizer_ordered`, from a
    /// translation with DxbcTranslationOptions::rasterizer_ordered_views).
    /// Each becomes a member of Metal raster order group 0 (MSL 2.0
    /// `[[raster_order_group(0)]]`): accesses to it from fragments that
    /// overlap one pixel happen in primitive submission order, which is the
    /// Learn "Rasterizer Order Views" guarantee. Empty for every D3D11 and
    /// non-ROV compile, which therefore emit exactly what they did before.
    std::vector<std::pair<u32, u32>> raster_ordered_bindings;
    /// Per-set argument-buffer layout, indexed by descriptor set. Ignored unless
    /// `argument_buffers` is set.
    std::array<MslArgumentBufferSet, MSL_DESCRIPTOR_SET_COUNT>
        argument_buffer_sets{};
    /// A D3D12 descriptor-heap window (DxbcTranslationOptions::heap_window):
    /// descriptor set `descriptor_set` (past the four register-space sets)
    /// is one argument buffer at `metal_buffer_index`: every array of the
    /// set starts at id(0) and aliases the others (Metal 3 argument-buffer
    /// aliasing), element k at id(k), so a shader's texture, buffer and
    /// constant-buffer windows all read the one heap. Each array is
    /// registered as one binding of its whole length -- padding a million
    /// slots one by one does not finish. Ignored unless
    /// `argument_buffers` is set; off for every D3D11 compile.
    struct HeapWindow {
        bool enabled = false;
        u32 descriptor_set = 0;
        u32 metal_buffer_index = 0;
    };
    HeapWindow heap_window{};
    // ---- WS5 begin ----
    /// Non-null: the SPIR-V is a mesh-lowered geometry shader
    /// (DxbcTranslationOptions::emulate_geometry_with_mesh), and its
    /// GLCompute entry is written as a [[mesh]] function with the object
    /// function beside it (msl_mesh_emit.hpp). A module without the
    /// Vitreo.MeshGS import, or with another entry model, is refused.
    const GsMeshLayout* mesh{nullptr};
    // ---- WS5 end ----
    /// Non-null: the SPIR-V is a lowered D3D12 mesh or amplification shader
    /// (DxbcTranslationResult::mesh_stage, mesh_stage.hpp), whose GLCompute
    /// entry is written as the [[mesh]] or [[object]] function. A module
    /// without the Vitreo.Mesh import is refused.
    const struct MeshStageLayout* mesh_stage{nullptr};
    /// With mesh_stage: the 32-bit words of the payload the object function
    /// writes and the mesh function reads (0: a mesh function without one).
    u32 mesh_payload_words{0};
};

/// Bytes one descriptor occupies in a Tier-2 argument buffer.
///
/// Metal 3 descriptors are all 8 bytes: `MTLResourceID` for a texture or
/// sampler, `MTLGPUAddress` for a buffer. A table's slot N therefore begins at
/// `N * MSL_DESCRIPTOR_SLOT_BYTES`, which is the layout the host encodes to and
/// `DescriptorTable.ShaderReadsSlotsBeyondTheFlatTextureLimit` proves on a real
/// GPU rather than assuming.
inline constexpr usize MSL_DESCRIPTOR_SLOT_BYTES = 8;

// =============================================================================
// API
// =============================================================================

/// Compile SPIR-V binary to MSL source text.
/// Returns a CompiledShader with MSL source and reflection data.
/// On failure, returns a CompiledShader where valid() == false.
[[nodiscard]] CompiledShader compile_spirv_to_msl(
    const u32* spirv_data,
    usize spirv_word_count,
    ShaderStage stage,
    const MslCompileOptions& options = {});

/// Extract reflection data from a SPIR-V module without compiling to MSL.
[[nodiscard]] ShaderReflection reflect_spirv(
    const u32* spirv_data,
    usize spirv_word_count);

/// True when the generated MSL can express a memory fence that is not also
/// a threadgroup rendezvous (MSL 3.2 atomic_thread_fence, macOS 15+). D3D
/// sync without _t needs one wherever threads may diverge.
[[nodiscard]] bool msl_memory_fence_available() noexcept;

} // namespace vitreo::graphics

// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file shader_types.hpp
/// @brief Common types for the shader compilation pipeline.

#include <vitreo/common.hpp>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// Shader Source Type
// =============================================================================

enum class ShaderSourceType : u8 {
    DXBC,       // D3D9/D3D11 shader bytecode (SM1–5)
    DXIL,       // D3D12 shader bytecode (SM6+, LLVM bitcode)
    GLSL,       // OpenGL GLSL source text
    SPIRV,      // Vulkan SPIR-V binary
    MSL,        // Already compiled Metal Shading Language (passthrough)
};

// =============================================================================
// Shader Stage
// =============================================================================

enum class ShaderStage : u8 {
    Vertex,
    Fragment,   // a.k.a. Pixel
    Compute,
    Geometry,
    Hull,       // a.k.a. Tessellation Control
    Domain,     // a.k.a. Tessellation Evaluation
    Mesh,
    Amplification,
};

// =============================================================================
// Resource Binding
// =============================================================================

enum class BindingType : u8 {
    ConstantBuffer, // CBV / uniform block
    ShaderResource, // SRV / sampled texture / storage buffer (read)
    UnorderedAccess,// UAV / storage buffer (read-write) / storage image
    Sampler,        // Texture sampler
};

struct ResourceBinding {
    BindingType type;
    u32 set;            // Descriptor set (Vulkan) / register space (D3D12) / 0 for D3D9/D3D11
    u32 binding;        // Binding index within the set / register number
    u32 count;          // Array element count (1 for non-array)
    u32 mtl_index;      // Remapped Metal buffer/texture/sampler index
};

// =============================================================================
// Input/Output Semantics
// =============================================================================

enum class SemanticName : u8 {
    Position,
    Normal,
    Texcoord,
    Color,
    Tangent,
    Binormal,
    BlendWeight,
    BlendIndices,
    PointSize,
    Fog,
    Depth,
    Target,         // SV_Target (render target output)
    InstanceId,
    VertexId,
    Unknown,
};

struct ShaderIO {
    SemanticName semantic;
    u8 semantic_index;  // e.g., TEXCOORD0 vs TEXCOORD1
    u8 component_count; // 1–4
    u32 location;       // SPIR-V / Metal attribute location
};

// =============================================================================
// Compiled Shader Output
// =============================================================================

struct CompiledShader {
    std::vector<u8> msl_source;         // MSL source text (null-terminated)
    std::vector<u8> spirv_binary;       // Intermediate SPIR-V (optional, for caching)
    std::vector<ResourceBinding> bindings;
    std::vector<ShaderIO> inputs;
    std::vector<ShaderIO> outputs;
    ShaderStage stage;
    u32 workgroup_size_x;   // Compute only
    u32 workgroup_size_y;
    u32 workgroup_size_z;
    bool uses_push_constants;

    [[nodiscard]] const char* msl_text() const {
        return reinterpret_cast<const char*>(msl_source.data());
    }

    [[nodiscard]] bool valid() const {
        return !msl_source.empty();
    }
};

/// Descriptor set the DXBC translator gives the hidden counters of APPEND and
/// COUNTER UAVs (`imm_atomic_alloc` / `imm_atomic_consume`). Binding k is the
/// k-th counter the shader touches -- DxbcToSpirvResult::uav_counter_registers
/// names its u# -- and spirv_to_msl maps it to
/// BACKEND_UAV_COUNTER_BUFFER_BASE + k.
inline constexpr u32 SPIRV_UAV_COUNTER_DESCRIPTOR_SET = 5;
/// Descriptor set of the PIPELINE_STATISTICS record a counting pixel shader
/// adds to (DxbcTranslationOptions::count_pixel_invocations): binding 0,
/// which spirv_to_msl maps to BACKEND_FRAGMENT_STATISTICS_BUFFER_INDEX.
inline constexpr u32 SPIRV_STATISTICS_DESCRIPTOR_SET = 6;
/// Counters one shader may use: the buffer indices left below Metal's limit.
/// spirv_to_msl.cpp asserts this equals BACKEND_MAX_UAV_COUNTER_BINDINGS.
inline constexpr u32 SPIRV_MAX_UAV_COUNTERS = 5;

} // namespace vitreo::graphics

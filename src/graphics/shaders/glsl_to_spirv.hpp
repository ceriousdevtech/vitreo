// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file glsl_to_spirv.hpp
/// @brief GLSL → SPIR-V compiler wrapper using glslang.

#include "shader_types.hpp"
#include <string>

namespace vitreo::graphics {

// =============================================================================
// GLSL Compilation Options
// =============================================================================

struct GlslCompileOptions {
    u32 default_version = 450;      // Default GLSL version if no #version
    bool vulkan_semantics = false;  // Use Vulkan GLSL semantics (binding=, set=)
    bool auto_map_bindings = true;  // Auto-assign binding locations
    bool auto_map_locations = true; // Auto-assign attribute locations
};

// =============================================================================
// API
// =============================================================================

/// Initialize the glslang library (call once at startup).
void glsl_compiler_init();

/// Shut down the glslang library (call once at exit).
void glsl_compiler_shutdown();

/// Compile GLSL source to SPIR-V binary.
/// @param source       Null-terminated GLSL source text
/// @param stage        Shader stage (Vertex, Fragment, Compute, etc.)
/// @param options      Compilation options
/// @param out_spirv    Output SPIR-V binary (u32 words)
/// @return true on success, false on compilation error
[[nodiscard]] bool compile_glsl_to_spirv(
    const char* source,
    ShaderStage stage,
    const GlslCompileOptions& options,
    std::vector<u32>& out_spirv);

/// Compile GLSL source to SPIR-V with an include preamble prepended.
/// Useful for injecting #define directives or common headers.
[[nodiscard]] bool compile_glsl_to_spirv_with_preamble(
    const char* preamble,
    const char* source,
    ShaderStage stage,
    const GlslCompileOptions& options,
    std::vector<u32>& out_spirv);

} // namespace vitreo::graphics

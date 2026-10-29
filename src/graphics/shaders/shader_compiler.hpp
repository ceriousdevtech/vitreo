// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file shader_compiler.hpp
/// @brief Unified shader compiler facade.
///
/// Accepts shader bytecode/source from any API (DXBC, DXIL, GLSL, SPIR-V)
/// and produces Metal Shading Language. Integrates with the shader cache
/// for fast repeat compilations.

#include "shader_types.hpp"
#include "shader_cache.hpp"
#include "spirv_to_msl.hpp"

#include <memory>

namespace vitreo::graphics {

// =============================================================================
// Shader Compiler
// =============================================================================

class ShaderCompiler {
public:
    /// Create a shader compiler with disk cache at the given path.
    /// @param cache_dir  Cache directory (e.g., ~/.vitreo/shader_cache/)
    explicit ShaderCompiler(const std::string& cache_dir);
    ~ShaderCompiler();

    // Non-copyable, non-movable (contains mutex via ShaderCache)
    ShaderCompiler(const ShaderCompiler&) = delete;
    ShaderCompiler& operator=(const ShaderCompiler&) = delete;
    ShaderCompiler(ShaderCompiler&&) = delete;
    ShaderCompiler& operator=(ShaderCompiler&&) = delete;

    /// Compile a shader from any supported source type to MSL.
    /// @param data        Raw shader bytecode or source text
    /// @param size        Size in bytes
    /// @param source_type Type of input shader
    /// @param stage       Shader stage (required for GLSL/SPIR-V, auto-detected for DXBC)
    /// @param msl_options MSL compilation options
    /// @return Compiled shader; check valid() for success
    [[nodiscard]] CompiledShader compile(
        const u8* data,
        usize size,
        ShaderSourceType source_type,
        ShaderStage stage = ShaderStage::Vertex,
        const MslCompileOptions& msl_options = {});

    /// Get the shader cache for statistics / management.
    [[nodiscard]] ShaderCache& cache() { return m_cache; }

    /// Clear the shader cache.
    void clear_cache() { m_cache.clear(); }

private:
    ShaderCache m_cache;

    [[nodiscard]] CompiledShader compile_dxbc(const u8* data, usize size,
        const MslCompileOptions& msl_options);
    [[nodiscard]] CompiledShader compile_glsl(const u8* data, usize size,
        ShaderStage stage, const MslCompileOptions& msl_options);
    [[nodiscard]] CompiledShader compile_spirv(const u8* data, usize size,
        ShaderStage stage, const MslCompileOptions& msl_options);
};

} // namespace vitreo::graphics

// SPDX-License-Identifier: LGPL-2.1-only

/// @file shader_compiler.cpp
/// @brief Unified shader compiler facade implementation.

#include "shader_compiler.hpp"
#include "dxbc_parser.hpp"
#include "dxbc_to_spirv.hpp"
#include "glsl_to_spirv.hpp"
#include "spirv_to_msl.hpp"

#include <vitreo/common.hpp>

namespace vitreo::graphics {

ShaderCompiler::ShaderCompiler(const std::string& cache_dir)
    : m_cache(cache_dir)
{
    glsl_compiler_init();
}

ShaderCompiler::~ShaderCompiler() {
    glsl_compiler_shutdown();
}

CompiledShader ShaderCompiler::compile(
    const u8* data,
    usize size,
    ShaderSourceType source_type,
    ShaderStage stage,
    const MslCompileOptions& msl_options)
{
    if (!data || size == 0) {
        return {};
    }

    // Check cache
    std::vector<u8> cached_msl;
    if (m_cache.lookup(data, size, cached_msl)) {
        VITREO_TRACE("[shader] Cache hit (%zu bytes MSL)\n", cached_msl.size());
        CompiledShader result{};
        result.msl_source = std::move(cached_msl);
        result.stage = stage;
        return result;
    }

    CompiledShader result{};
    switch (source_type) {
    case ShaderSourceType::DXBC:
        result = compile_dxbc(data, size, msl_options);
        break;
    case ShaderSourceType::DXIL:
        // DXIL (SM6+) requires a separate DXIL→SPIR-V translator.
        // No translator is installed for this format.
        VITREO_TRACE("[shader] DXIL compilation not yet implemented\n");
        break;
    case ShaderSourceType::GLSL:
        result = compile_glsl(data, size, stage, msl_options);
        break;
    case ShaderSourceType::SPIRV:
        result = compile_spirv(data, size, stage, msl_options);
        break;
    case ShaderSourceType::MSL:
        // Passthrough — already MSL
        result.msl_source.assign(data, data + size);
        if (result.msl_source.empty() || result.msl_source.back() != 0) {
            result.msl_source.push_back(0);
        }
        result.stage = stage;
        break;
    }

    // Store in cache
    if (result.valid()) {
        m_cache.store(data, size, result.msl_source);
        VITREO_TRACE("[shader] Compiled %zu bytes → %zu bytes MSL\n",
                     size, result.msl_source.size());
    }

    return result;
}

CompiledShader ShaderCompiler::compile_dxbc(
    const u8* data, usize size,
    const MslCompileOptions& msl_options)
{
    // Try parsing as DXBC container (SM4/5)
    auto container = parse_dxbc(data, size);
    if (container.valid()) {
        VITREO_TRACE("[shader] DXBC SM%u.%u %s shader, %zu bytes bytecode\n",
                     container.shader_model_major, container.shader_model_minor,
                     container.stage == ShaderStage::Vertex ? "VS" : "PS",
                     container.bytecode.size());

        // Translate DXBC → SPIR-V
        auto spirv_result = translate_dxbc_to_spirv(container);
        if (!spirv_result.valid()) {
            VITREO_TRACE("[shader] DXBC→SPIR-V translation failed: %s\n",
                        spirv_result.failure.c_str());
            return {};
        }

        VITREO_TRACE("[shader] DXBC→SPIR-V translation succeeded, %zu words\n",
                    spirv_result.spirv.size());

        // SPIR-V → MSL. An atomic on a typed UAV is spelled with
        // OpImageTexelPointer, and SPIRV-Cross lowers that to a native Metal
        // texture atomic only from MSL 3.1. Below that it emulates the atomic
        // against a side buffer the caller is expected to bind alongside the
        // texture -- which nothing here does, so the MSL would compile and the
        // atomic would land in an unbound allocation. Raise the version for
        // exactly those shaders rather than for every shader.
        auto shader_msl_options = msl_options;
        if (spirv_result.uses_typed_uav_atomics &&
            shader_msl_options.msl_version < 30100) {
            shader_msl_options.msl_version = 30100;
        }
        auto compiled = compile_spirv_to_msl(
            spirv_result.spirv.data(),
            spirv_result.spirv.size(),
            spirv_result.stage,
            shader_msl_options);

        if (!compiled.valid()) {
            VITREO_TRACE("[shader] SPIR-V→MSL compilation failed\n");
            return {};
        }

        // Copy reflection data from DXBC translation
        compiled.spirv_binary.assign(
            reinterpret_cast<const u8*>(spirv_result.spirv.data()),
            reinterpret_cast<const u8*>(spirv_result.spirv.data()) +
            spirv_result.spirv.size() * sizeof(u32));

        return compiled;
    }

    // Try as D3D9 shader (SM1-3, no DXBC container)
    u32 sm_major, sm_minor;
    ShaderStage stage;
    if (detect_d3d9_shader(data, size, sm_major, sm_minor, stage)) {
        VITREO_TRACE("[shader] D3D9 SM%u.%u %s shader\n",
                     sm_major, sm_minor,
                     stage == ShaderStage::Vertex ? "VS" : "PS");

        // TODO: Implement D3D9 SM1-3 → SPIR-V translation.
        // D3D9 assembly format is much simpler than SM4/5.
        VITREO_TRACE("[shader] D3D9→SPIR-V translation not yet implemented\n");
        return {};
    }

    VITREO_TRACE("[shader] Unrecognized DXBC/D3D9 shader format\n");
    return {};
}

CompiledShader ShaderCompiler::compile_glsl(
    const u8* data, usize size,
    ShaderStage stage,
    const MslCompileOptions& msl_options)
{
    // Ensure null-terminated source
    std::string source_str;
    if (data[size - 1] == 0) {
        source_str.assign(reinterpret_cast<const char*>(data), size - 1);
    } else {
        source_str.assign(reinterpret_cast<const char*>(data), size);
    }

    // GLSL → SPIR-V
    GlslCompileOptions glsl_opts;
    glsl_opts.vulkan_semantics = false;
    glsl_opts.auto_map_bindings = true;
    glsl_opts.auto_map_locations = true;

    std::vector<u32> spirv;
    if (!compile_glsl_to_spirv(source_str.c_str(), stage, glsl_opts, spirv)) {
        VITREO_TRACE("[shader] GLSL→SPIR-V compilation failed\n");
        return {};
    }

    // SPIR-V → MSL
    return compile_spirv_to_msl(spirv.data(), spirv.size(), stage, msl_options);
}

CompiledShader ShaderCompiler::compile_spirv(
    const u8* data, usize size,
    ShaderStage stage,
    const MslCompileOptions& msl_options)
{
    if (size % sizeof(u32) != 0) {
        VITREO_TRACE("[shader] SPIR-V size not u32-aligned: %zu\n", size);
        return {};
    }

    auto word_count = size / sizeof(u32);
    auto* words = reinterpret_cast<const u32*>(data);

    // Validate SPIR-V magic
    if (word_count < 5 || words[0] != 0x07230203) {
        VITREO_TRACE("[shader] Invalid SPIR-V magic\n");
        return {};
    }

    return compile_spirv_to_msl(words, word_count, stage, msl_options);
}

} // namespace vitreo::graphics

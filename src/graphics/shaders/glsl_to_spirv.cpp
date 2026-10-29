// SPDX-License-Identifier: LGPL-2.1-only

/// @file glsl_to_spirv.cpp
/// @brief GLSL → SPIR-V compiler implementation using glslang.

#include "glsl_to_spirv.hpp"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <vitreo/common.hpp>

#include <string>

namespace vitreo::graphics {

namespace {

EShLanguage stage_to_glslang(ShaderStage stage) {
    switch (stage) {
    case ShaderStage::Vertex:        return EShLangVertex;
    case ShaderStage::Fragment:      return EShLangFragment;
    case ShaderStage::Compute:       return EShLangCompute;
    case ShaderStage::Geometry:      return EShLangGeometry;
    case ShaderStage::Hull:          return EShLangTessControl;
    case ShaderStage::Domain:        return EShLangTessEvaluation;
    case ShaderStage::Mesh:          return EShLangMesh;
    case ShaderStage::Amplification: return EShLangTask;
    }
    return EShLangVertex;
}

bool compile_impl(
    const char* source_text,
    ShaderStage stage,
    const GlslCompileOptions& options,
    std::vector<u32>& out_spirv)
{
    auto lang = stage_to_glslang(stage);
    glslang::TShader shader(lang);

    const char* sources[] = { source_text };
    shader.setStrings(sources, 1);

    // Configure environment
    auto client = options.vulkan_semantics
        ? glslang::EShClientVulkan
        : glslang::EShClientOpenGL;
    auto target_version = options.vulkan_semantics
        ? glslang::EShTargetVulkan_1_3
        : glslang::EShTargetOpenGL_450;
    shader.setEnvInput(glslang::EShSourceGlsl, lang, client,
                       static_cast<int>(options.default_version));
    shader.setEnvClient(client, target_version);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);

    if (options.auto_map_bindings) {
        shader.setAutoMapBindings(true);
    }
    if (options.auto_map_locations) {
        shader.setAutoMapLocations(true);
    }

    // Parse
    auto* resources = GetDefaultResources();
    if (!shader.parse(resources, static_cast<int>(options.default_version),
                      false, EShMsgDefault)) {
        VITREO_TRACE("[shader] GLSL parse error: %s\n", shader.getInfoLog());
        return false;
    }

    // Link
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        VITREO_TRACE("[shader] GLSL link error: %s\n", program.getInfoLog());
        return false;
    }

    // Generate SPIR-V
    auto* intermediate = program.getIntermediate(lang);
    if (!intermediate) {
        VITREO_TRACE("[shader] No intermediate for stage\n");
        return false;
    }

    glslang::SpvOptions spv_options;
    spv_options.generateDebugInfo = false;
    spv_options.disableOptimizer = false;
    spv_options.optimizeSize = true;
    spv_options.validate = true;

    glslang::GlslangToSpv(*intermediate, out_spirv, &spv_options);

    return !out_spirv.empty();
}

} // anonymous namespace

// =============================================================================
// Public API
// =============================================================================

void glsl_compiler_init() {
    glslang::InitializeProcess();
}

void glsl_compiler_shutdown() {
    glslang::FinalizeProcess();
}

bool compile_glsl_to_spirv(
    const char* source,
    ShaderStage stage,
    const GlslCompileOptions& options,
    std::vector<u32>& out_spirv)
{
    if (!source) return false;
    return compile_impl(source, stage, options, out_spirv);
}

bool compile_glsl_to_spirv_with_preamble(
    const char* preamble,
    const char* source,
    ShaderStage stage,
    const GlslCompileOptions& options,
    std::vector<u32>& out_spirv)
{
    if (!source) return false;

    std::string combined;
    if (preamble) {
        combined = preamble;
        combined += '\n';
    }
    combined += source;
    return compile_impl(combined.c_str(), stage, options, out_spirv);
}

} // namespace vitreo::graphics

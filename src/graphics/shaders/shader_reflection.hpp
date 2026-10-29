// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file shader_reflection.hpp
/// @brief Shader reflection data extracted from SPIR-V modules via SPIRV-Cross.

#include "shader_types.hpp"
#include <string>
#include <vector>

namespace vitreo::graphics {

// =============================================================================
// Reflection Data
// =============================================================================

struct UniformMember {
    std::string name;
    u32 offset;
    u32 size;
};

struct ConstantBufferReflection {
    std::string name;
    u32 binding;
    u32 set;
    u32 size_bytes;
    std::vector<UniformMember> members;
};

struct TextureReflection {
    std::string name;
    u32 binding;
    u32 set;
    u8 dimensions;  // 1D=1, 2D=2, 3D=3, Cube=4
    bool is_array;
    bool is_storage; // UAV / storage image
};

struct SamplerReflection {
    std::string name;
    u32 binding;
    u32 set;
};

struct ShaderReflection {
    std::vector<ConstantBufferReflection> constant_buffers;
    std::vector<TextureReflection> textures;
    std::vector<SamplerReflection> samplers;
    std::vector<ShaderIO> inputs;
    std::vector<ShaderIO> outputs;

    u32 push_constant_size;
    u32 workgroup_size[3];  // Compute only [x,y,z]

    [[nodiscard]] u32 total_resource_count() const {
        return static_cast<u32>(constant_buffers.size()
            + textures.size() + samplers.size());
    }
};

} // namespace vitreo::graphics

// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file gpu_resource.hpp
/// @brief Base class for all GPU-owned resources

#include <vitreo/common.hpp>

namespace vitreo::graphics {

enum class ResourceType : u8 {
    Buffer,
    Texture2D,
    Texture3D,
    TextureCube,
    TextureArray,
    Sampler,
    RenderTarget,
    DepthStencil,
    Shader,
    Pipeline,
};

class GpuResource {
public:
    virtual ~GpuResource() = default;

    u64 handle_id() const { return m_handle_id; }
    ResourceType type() const { return m_type; }

protected:
    explicit GpuResource(ResourceType type)
        : m_type(type), m_handle_id(s_next_id++) {}

private:
    ResourceType m_type;
    u64 m_handle_id;
    static inline u64 s_next_id = 1;
};

} // namespace vitreo::graphics

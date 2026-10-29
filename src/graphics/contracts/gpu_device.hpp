// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Abstract GPU device interface for backend-agnostic rendering

#pragma once

#include <vitreo/types.hpp>
#include <vitreo/result.hpp>
#include <cstddef>

namespace vitreo::graphics {

/// Buffer usage flags
enum class BufferUsage : u32 {
    Vertex   = 1 << 0,
    Index    = 1 << 1,
    Constant = 1 << 2,
    Storage  = 1 << 3,
};

/// Texture format (subset matching D3D9/Metal common formats)
enum class TextureFormat : u32 {
    Unknown = 0,
    RGBA8Unorm,
    BGRA8Unorm,
    R8Unorm,
    R16Float,
    RGBA16Float,
    RGBA32Float,
    D24UnormS8Uint,
    D32Float,
    DXT1,
    DXT3,
    DXT5,
};

/// Texture usage flags
enum class TextureUsage : u32 {
    ShaderRead   = 1 << 0,
    RenderTarget = 1 << 1,
    DepthStencil = 1 << 2,
};

/// Opaque handle types
using GpuBufferHandle  = u64;
using GpuTextureHandle = u64;
using GpuPipelineHandle = u64;
using GpuShaderHandle  = u64;

constexpr GpuBufferHandle   kInvalidBuffer   = 0;
constexpr GpuTextureHandle  kInvalidTexture  = 0;
constexpr GpuPipelineHandle kInvalidPipeline = 0;
constexpr GpuShaderHandle   kInvalidShader   = 0;

/// Buffer creation descriptor
struct BufferDesc {
    usize size = 0;
    BufferUsage usage = BufferUsage::Vertex;
    const void* initial_data = nullptr;
};

/// Texture creation descriptor
struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 1;
    TextureFormat format = TextureFormat::RGBA8Unorm;
    TextureUsage usage = TextureUsage::ShaderRead;
};

/// Pipeline state descriptor (simplified)
struct PipelineDesc {
    GpuShaderHandle vertex_shader = kInvalidShader;
    GpuShaderHandle pixel_shader = kInvalidShader;
    // Additional state (blend, depth, raster) added as needed
};

/// Abstract GPU device interface.
/// Decouples D3D frontends from the Metal backend.
class IGpuDevice {
public:
    virtual ~IGpuDevice() = default;

    // Resource creation
    virtual Result<GpuBufferHandle>   create_buffer(const BufferDesc& desc) = 0;
    virtual Result<GpuTextureHandle>  create_texture(const TextureDesc& desc) = 0;
    virtual Result<GpuPipelineHandle> create_pipeline(const PipelineDesc& desc) = 0;
    virtual Result<GpuShaderHandle>   create_shader(const void* bytecode, usize size) = 0;

    // Resource destruction
    virtual void destroy_buffer(GpuBufferHandle handle) = 0;
    virtual void destroy_texture(GpuTextureHandle handle) = 0;
    virtual void destroy_pipeline(GpuPipelineHandle handle) = 0;
    virtual void destroy_shader(GpuShaderHandle handle) = 0;

    // Buffer operations
    virtual Result<void*> map_buffer(GpuBufferHandle handle) = 0;
    virtual void unmap_buffer(GpuBufferHandle handle) = 0;

    // Texture operations
    virtual Result<void> update_texture(GpuTextureHandle handle,
                                         u32 x, u32 y, u32 width, u32 height,
                                         const void* data, usize row_pitch) = 0;

    // Command submission
    virtual void begin_frame() = 0;
    virtual void end_frame() = 0;
    
    // Rendering commands  
    virtual void set_pipeline(GpuPipelineHandle handle) = 0;
    virtual void set_vertex_buffer(GpuBufferHandle handle, u32 slot, u32 offset, u32 stride) = 0;
    virtual void set_index_buffer(GpuBufferHandle handle, u32 offset, bool is_32bit) = 0;
    virtual void set_texture(GpuTextureHandle handle, u32 slot) = 0;
    virtual void set_constant_buffer(GpuBufferHandle handle, u32 slot) = 0;
    virtual void set_viewport(float x, float y, float width, float height, float min_depth, float max_depth) = 0;
    virtual void set_scissor(u32 x, u32 y, u32 width, u32 height) = 0;
    
    virtual void draw(u32 vertex_count, u32 start_vertex) = 0;
    virtual void draw_indexed(u32 index_count, u32 start_index, u32 base_vertex) = 0;
    
    virtual void present() = 0;
};

// Bitwise operators for flag enums
inline BufferUsage operator|(BufferUsage a, BufferUsage b) {
    return static_cast<BufferUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline bool operator&(BufferUsage a, BufferUsage b) {
    return (static_cast<u32>(a) & static_cast<u32>(b)) != 0;
}
inline TextureUsage operator|(TextureUsage a, TextureUsage b) {
    return static_cast<TextureUsage>(static_cast<u32>(a) | static_cast<u32>(b));
}
inline bool operator&(TextureUsage a, TextureUsage b) {
    return (static_cast<u32>(a) & static_cast<u32>(b)) != 0;
}

} // namespace vitreo::graphics

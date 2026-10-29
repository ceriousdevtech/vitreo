// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include <gtest/gtest.h>
#include "graphics/contracts/gpu_device.hpp"

using namespace vitreo;
using namespace vitreo::graphics;

// ===========================================================================
// BufferUsage flag combinations
// ===========================================================================

TEST(GpuDeviceTest, BufferUsageBitwiseOr) {
    auto combined = BufferUsage::Vertex | BufferUsage::Index;
    EXPECT_TRUE(combined & BufferUsage::Vertex);
    EXPECT_TRUE(combined & BufferUsage::Index);
    EXPECT_FALSE(combined & BufferUsage::Constant);
}

TEST(GpuDeviceTest, BufferUsageAllFlags) {
    auto all = BufferUsage::Vertex | BufferUsage::Index |
               BufferUsage::Constant | BufferUsage::Storage;
    EXPECT_TRUE(all & BufferUsage::Vertex);
    EXPECT_TRUE(all & BufferUsage::Index);
    EXPECT_TRUE(all & BufferUsage::Constant);
    EXPECT_TRUE(all & BufferUsage::Storage);
}

// ===========================================================================
// TextureUsage flag combinations
// ===========================================================================

TEST(GpuDeviceTest, TextureUsageBitwiseOr) {
    auto combined = TextureUsage::ShaderRead | TextureUsage::RenderTarget;
    EXPECT_TRUE(combined & TextureUsage::ShaderRead);
    EXPECT_TRUE(combined & TextureUsage::RenderTarget);
    EXPECT_FALSE(combined & TextureUsage::DepthStencil);
}

// ===========================================================================
// Handle constants
// ===========================================================================

TEST(GpuDeviceTest, InvalidHandlesAreZero) {
    EXPECT_EQ(kInvalidBuffer, 0u);
    EXPECT_EQ(kInvalidTexture, 0u);
    EXPECT_EQ(kInvalidPipeline, 0u);
    EXPECT_EQ(kInvalidShader, 0u);
}

// ===========================================================================
// IGpuDevice can be subclassed (vtable works)
// ===========================================================================

namespace {

/// Minimal mock that implements IGpuDevice to verify the interface is usable.
class MockGpuDevice : public IGpuDevice {
public:
    bool frame_begun = false;
    bool presented   = false;

    Result<GpuBufferHandle>   create_buffer(const BufferDesc&) override { return Result<GpuBufferHandle>::ok(GpuBufferHandle{1}); }
    Result<GpuTextureHandle>  create_texture(const TextureDesc&) override { return Result<GpuTextureHandle>::ok(GpuTextureHandle{2}); }
    Result<GpuPipelineHandle> create_pipeline(const PipelineDesc&) override { return Result<GpuPipelineHandle>::ok(GpuPipelineHandle{3}); }
    Result<GpuShaderHandle>   create_shader(const void*, usize) override { return Result<GpuShaderHandle>::ok(GpuShaderHandle{4}); }

    void destroy_buffer(GpuBufferHandle) override {}
    void destroy_texture(GpuTextureHandle) override {}
    void destroy_pipeline(GpuPipelineHandle) override {}
    void destroy_shader(GpuShaderHandle) override {}

    Result<void*> map_buffer(GpuBufferHandle) override { return Result<void*>::ok(static_cast<void*>(nullptr)); }
    void unmap_buffer(GpuBufferHandle) override {}

    Result<void> update_texture(GpuTextureHandle, u32, u32, u32, u32, const void*, usize) override {
        return Result<void>::ok();
    }

    void begin_frame() override { frame_begun = true; }
    void end_frame() override {}

    void set_pipeline(GpuPipelineHandle) override {}
    void set_vertex_buffer(GpuBufferHandle, u32, u32, u32) override {}
    void set_index_buffer(GpuBufferHandle, u32, bool) override {}
    void set_texture(GpuTextureHandle, u32) override {}
    void set_constant_buffer(GpuBufferHandle, u32) override {}
    void set_viewport(float, float, float, float, float, float) override {}
    void set_scissor(u32, u32, u32, u32) override {}

    void draw(u32, u32) override {}
    void draw_indexed(u32, u32, u32) override {}

    void present() override { presented = true; }
};

} // namespace

TEST(GpuDeviceTest, MockSubclass_CreateBuffer) {
    MockGpuDevice mock;
    IGpuDevice* dev = &mock;
    auto result = dev->create_buffer(BufferDesc{.size = 256, .usage = BufferUsage::Vertex});
    ASSERT_TRUE(result.is_ok());
    EXPECT_EQ(result.value(), 1u);
}

TEST(GpuDeviceTest, MockSubclass_VtableDispatch) {
    MockGpuDevice mock;
    IGpuDevice* dev = &mock;

    EXPECT_FALSE(mock.frame_begun);
    dev->begin_frame();
    EXPECT_TRUE(mock.frame_begun);

    EXPECT_FALSE(mock.presented);
    dev->present();
    EXPECT_TRUE(mock.presented);
}

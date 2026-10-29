/// @file test_gl_pixel_formats_metal.cpp
/// @brief Every OpenGL ICD pixel format has backend targets the real Metal
///        backend can create.
///
/// Kept out of vitreo_winapi_tests, which deliberately does not link the
/// Metal backend (same reason as test_ddraw_d3d7_metal.cpp).

#include <gtest/gtest.h>

#include <graphics/contracts/backend_device.hpp>
#include <graphics/metal/metal_device.hpp>
#include <winapi/graphics/opengl32/gl_pixel_formats.hpp>

using namespace vitreo;
using namespace vitreo::winapi::gl;

TEST(GlPixelFormatsMetal, EveryFormatHasBackendTargets) {
    if (!graphics::MetalDevice::instance().available()) GTEST_SKIP() << "no Metal device";
    graphics::set_active_backend_device(&graphics::MetalDevice::instance());
    graphics::GraphicsBackendDevice* device = graphics::active_backend_device();
    ASSERT_NE(device, nullptr);

    for (usize i = 0; i < gl_pixel_formats().size(); ++i) {
        const GlPixelFormat& f = gl_pixel_formats()[i];
        SCOPED_TRACE(i + 1);
        const GlBackendTargets t = gl_backend_targets(f);

        // Back and front buffers: render target and shader resource.
        graphics::BackendTexture2DDesc color{};
        color.width = 64;
        color.height = 64;
        color.pixel_format = t.color;
        color.render_target = true;
        color.shader_resource = true;
        ASSERT_EQ(t.color, graphics::PixelFormat::BGRA8Unorm);
        EXPECT_NE(device->create_texture_2d(color, nullptr, 0), nullptr);

        // The depth/stencil target, when the format has depth or stencil.
        if (f.depth_bits == 0 && f.stencil_bits == 0) {
            EXPECT_EQ(t.depth_stencil, graphics::PixelFormat::Unknown);
            continue;
        }
        ASSERT_NE(t.depth_stencil, graphics::PixelFormat::Unknown);
        graphics::BackendTexture2DDesc depth{};
        depth.width = 64;
        depth.height = 64;
        depth.pixel_format = t.depth_stencil;
        depth.depth_stencil = true;
        EXPECT_NE(device->create_texture_2d(depth, nullptr, 0), nullptr);
        // Stencil bits need a stencil-carrying target.
        if (f.stencil_bits != 0) EXPECT_EQ(t.depth_stencil, graphics::PixelFormat::D32Float_S8Uint);
    }
    graphics::set_active_backend_device(nullptr);
}

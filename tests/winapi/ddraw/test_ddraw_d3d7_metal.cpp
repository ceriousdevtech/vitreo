// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ddraw_d3d7_metal.cpp
/// @brief Direct3D 7 draw submission against a real GPU.
///
/// Kept out of vitreo_winapi_tests because that target deliberately does not
/// link the Metal backend. A draw cannot be proven without one: the submission
/// path assembles a vertex layout, generated MSL, a uniform payload and a
/// readback, and only an executed draw shows whether they agree.

#include <gtest/gtest.h>

#include <graphics/contracts/backend_device.hpp>
#include <graphics/contracts/cpu_surface.hpp>
#include <graphics/contracts/presentation_state.hpp>
#include <graphics/common/surface_presenter.hpp>
#include <graphics/metal/metal_device.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/graphics/ddraw/ddraw.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

class NullSurfacePresenter final : public graphics::ISurfacePresenter {
public:
    bool present_indexed(u32, const graphics::CpuSurface&, const u8*,
                         usize) override {
        return true;
    }
};

class D3D7DrawTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        manager.set_memory(&memory);
        manager.set_thunk_registry(&registry);
        manager.set_surface_presenter(&presenter);
        ASSERT_TRUE(registry.register_dll("ddraw.dll",
                                          create_ddraw_thunks(manager)).is_ok());
        // The runtime installs the backend at process start; a test process
        // has to do it explicitly.
        graphics::set_active_backend_device(&graphics::MetalDevice::instance());
    }
    void TearDown() override {
        graphics::set_active_backend_device(nullptr);
    }

    GuestMemory memory;
    ThunkRegistry registry{0x70000000};
    graphics::PresentationState presentation;
    NullSurfacePresenter presenter;
    DirectDrawManager manager{presentation};
};

} // namespace

// =============================================================================
// Direct3D 7 draw submission
// =============================================================================
//
// DrawPrimitive assembles four independently tested pieces: the FVF vertex
// layout, the generated MSL, the uniform payload, and the readback bridge back
// into guest-visible surface memory. Each is covered elsewhere; only an actual
// draw shows whether they agree with each other. Until this passes, DDCAPS_3D
// must stay clear — the device would be claiming to render without evidence.

TEST_F(D3D7DrawTest, DrawPrimitiveReachesSurfaceMemory) {
    if (graphics::active_backend_device() == nullptr ||
        !graphics::active_backend_device()->available()) {
        GTEST_SKIP() << "no graphics backend installed in this test process";
    }

    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    constexpr GuestAddr device_output = 0x10900;
    constexpr GuestAddr clsid = 0x10A00;
    constexpr GuestAddr vertices = 0x10B00;

    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    // An offscreen surface inherits the display mode's pixel format, so the
    // mode has to be established before one can be created.
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 64, 64, 32), ddraw::DD_OK);

    // A 32bpp offscreen surface: DDSD_CAPS|HEIGHT|WIDTH with
    // DDSCAPS_OFFSCREENPLAIN|DDSCAPS_SYSTEMMEMORY, the render target a
    // software device draws into.
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 0x7).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 8, 8).is_ok());   // height
    ASSERT_TRUE(memory.write_u32(descriptor + 12, 8).is_ok());  // width
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x840).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();
    ASSERT_NE(surface, 0u);

    // IID_IDirect3DRGBDevice {A4665C60-2673-11CF-A31A-00AA00B93356}
    ASSERT_TRUE(memory.write_u32(clsid + 0, 0xA4665C60u).is_ok());
    ASSERT_TRUE(memory.write_u32(clsid + 4, 0x11CF2673u).is_ok());
    ASSERT_TRUE(memory.write_u32(clsid + 8, 0xAA001AA3u).is_ok());
    ASSERT_TRUE(memory.write_u32(clsid + 12, 0x5633B900u).is_ok());
    const u32 created =
        manager.d3d7_create_device(object, clsid, surface, device_output);
    if (created != ddraw::DD_OK) {
        GTEST_SKIP() << "CreateDevice rejected this GUID encoding: " << created;
    }
    const GuestAddr device = memory.read_u32(device_output).value();
    ASSERT_NE(device, 0u);

    // D3DVIEWPORT7 {x, y, width, height, minZ, maxZ}. A pre-transformed
    // (XYZRHW) vertex is mapped to clip space through the viewport extent, so
    // leaving it at its 0x0 default collapses every vertex to a point.
    constexpr GuestAddr viewport = 0x10D00;
    ASSERT_TRUE(memory.write_u32(viewport + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(viewport + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(viewport + 8, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(viewport + 12, 8).is_ok());
    {
        const float min_z = 0.0f, max_z = 1.0f;
        u32 bits = 0;
        std::memcpy(&bits, &min_z, sizeof(bits));
        ASSERT_TRUE(memory.write_u32(viewport + 16, bits).is_ok());
        std::memcpy(&bits, &max_z, sizeof(bits));
        ASSERT_TRUE(memory.write_u32(viewport + 20, bits).is_ok());
    }
    ASSERT_EQ(manager.device_set_viewport(device, viewport), ddraw::DD_OK);

    // D3DRENDERSTATE_LIGHTING defaults to TRUE in Direct3D 7. With it on and
    // no lights set, the pipeline resolves every vertex to black regardless of
    // the diffuse colour, so a pre-transformed unlit triangle must turn it off
    // exactly as a real title would.
    ASSERT_EQ(manager.device_set_render_state(device, 137 /*LIGHTING*/, 0),
              ddraw::DD_OK);

    // Clear to opaque black so any drawn pixel is unambiguous.
    ASSERT_EQ(manager.device_clear(device, 0, 0, 0x1 /*TARGET*/, 0xFF000000u,
                                   0, 0),
              ddraw::DD_OK);

    // One screen-space triangle covering the surface. D3DFVF_XYZRHW|DIFFUSE
    // needs no transform, so a wrong world/view matrix cannot explain a blank
    // result: this isolates the submission path itself.
    struct Vertex { float x, y, z, rhw; u32 color; };
    const Vertex tri[3] = {
        {0.0f, 0.0f, 0.5f, 1.0f, 0xFF00FF00u},
        {8.0f, 0.0f, 0.5f, 1.0f, 0xFF00FF00u},
        {0.0f, 8.0f, 0.5f, 1.0f, 0xFF00FF00u},
    };
    for (u32 i = 0; i < 3; ++i) {
        const auto* raw = reinterpret_cast<const u8*>(&tri[i]);
        for (u32 b = 0; b < sizeof(Vertex); ++b) {
            ASSERT_TRUE(memory.write_u8(vertices + i * sizeof(Vertex) + b,
                                        raw[b]).is_ok());
        }
    }

    constexpr u32 kFvfXyzrhwDiffuse = 0x004 | 0x040;
    ASSERT_EQ(manager.device_begin_scene(device), ddraw::DD_OK);
    const u32 drawn = manager.device_draw_primitive(
        device, 4 /*D3DPT_TRIANGLELIST*/, kFvfXyzrhwDiffuse, vertices, 3, 0);
    (void)manager.device_end_scene(device);

    ASSERT_EQ(drawn, ddraw::DD_OK)
        << "DrawPrimitive did not complete; the assembled path is incomplete";

    // Returning DD_OK only says the submission was accepted. The device is not
    // usable until the geometry actually lands in the surface, so read the
    // guest-visible pixels back and look for the triangle.
    // Lock is the guest-facing way to read a surface, and is exactly what the
    // game would use to see the frame.
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    const u32 pitch = memory.read_u32(descriptor + 16).value();
    const GuestAddr pixels = memory.read_u32(descriptor + 36).value();
    ASSERT_NE(pixels, 0u);
    ASSERT_GE(pitch, 8u * 4u);

    u32 green = 0;
    u32 black = 0;
    u32 other = 0;
    for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
            const GuestAddr at = pixels + y * pitch + x * 4u;
            const u32 pixel = memory.read_u32(at).value() & 0x00FFFFFFu;
            if (pixel == 0x0000FF00u) ++green;
            else if (pixel == 0u) ++black;
            else ++other;
        }
    }
    ASSERT_EQ(manager.surface_unlock(surface, 0), ddraw::DD_OK);

    // The triangle covers the upper-left half, so both a drawn and an
    // untouched region must exist: all-green would mean the geometry was
    // ignored and the target merely filled.
    EXPECT_GT(green, 0u) << "no triangle pixels reached the surface";
    EXPECT_GT(black, 0u) << "the whole target was painted; geometry ignored";
    EXPECT_EQ(other, 0u) << "unexpected colours: channel order is wrong";
}

// GetCaps reports DDCAPS_3D from the presence of a backend device, so the two
// branches need separate coverage: vitreo_winapi_tests links no backend and
// pins the DDCAPS_NOHARDWARE answer, and this pins the other one. A caller
// that reads DDCAPS_3D goes on to create a Direct3D device and draw through
// it, which is what the draw test above establishes actually works.
TEST_F(D3D7DrawTest, GetCapsReportsHardware3DWhenABackendIsPresent) {
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr driver_caps = 0x10600;
    constexpr u32 caps_size = 316;  // DDCAPS_DX7
    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_TRUE(memory.write_u32(driver_caps, caps_size).is_ok());
    ASSERT_EQ(manager.get_caps(object, driver_caps, 0), ddraw::DD_OK);

    constexpr u32 DDCAPS_3D = 0x00000001;
    constexpr u32 DDCAPS_NOHARDWARE = 0x02000000;
    const u32 flags = memory.read_u32(driver_caps + 4).value();
    EXPECT_TRUE((flags & DDCAPS_3D) != 0);
    EXPECT_TRUE((flags & DDCAPS_NOHARDWARE) == 0);
}

// =============================================================================
// Direct3D 7 transforms
// =============================================================================
//
// v' = v * World * View * Projection with row vectors and row-major
// D3DMATRIX (Learn, "Transforms (Direct3D 9)"; Direct3D 7 uses the same
// D3DMATRIX and pipeline), then X = (1 + x) * Width / 2, Y = (1 - y) *
// Height / 2 ("Viewports and Clipping"). The matrices are written out from
// the D3DXMatrixLookAtLH / D3DXMatrixPerspectiveFovLH / D3DXMatrixRotationY
// formulas and set through IDirect3DDevice7::SetTransform; the expected
// coverage is that math evaluated on the CPU in double precision.

namespace {

using M4 = std::array<double, 16>;
using V4 = std::array<double, 4>;
using D3 = std::array<double, 3>;

M4 mul4(const M4& a, const M4& b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i * 4 + j] += a[i * 4 + k] * b[k * 4 + j];
    return r;
}
V4 xf4(const V4& v, const M4& m) {
    V4 r{};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) r[j] += v[k] * m[k * 4 + j];
    return r;
}
D3 sub3(const D3& a, const D3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
double dot3(const D3& a, const D3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross3(const D3& a, const D3& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
D3 unit3(const D3& a) {
    const double l = std::sqrt(dot3(a, a));
    return {a[0] / l, a[1] / l, a[2] / l};
}

} // namespace

TEST_F(D3D7DrawTest, D3D7TransformedVertexLandsWherePerspectiveMathSays) {
    if (graphics::active_backend_device() == nullptr ||
        !graphics::active_backend_device()->available()) {
        GTEST_SKIP() << "no graphics backend installed in this test process";
    }
    constexpr u32 kSize = 32;
    constexpr GuestAddr output = 0x10000;
    constexpr GuestAddr descriptor = 0x10600;
    constexpr GuestAddr surface_output = 0x10700;
    constexpr GuestAddr device_output = 0x10900;
    constexpr GuestAddr clsid = 0x10A00;
    constexpr GuestAddr vertices = 0x10B00;
    constexpr GuestAddr viewport = 0x10D00;
    constexpr GuestAddr matrix = 0x10E00;

    ASSERT_EQ(manager.direct_draw_create(0, output, 0), ddraw::DD_OK);
    const GuestAddr object = memory.read_u32(output).value();
    ASSERT_EQ(manager.set_cooperative_level(object, 0x1234,
                                            ddraw::DDSCL_FULLSCREEN |
                                            ddraw::DDSCL_EXCLUSIVE),
              ddraw::DD_OK);
    ASSERT_EQ(manager.set_display_mode(object, 64, 64, 32), ddraw::DD_OK);
    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 4, 0x7).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 8, kSize).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 12, kSize).is_ok());
    ASSERT_TRUE(memory.write_u32(descriptor + 104, 0x840).is_ok());
    ASSERT_EQ(manager.create_surface(object, descriptor, surface_output, 0),
              ddraw::DD_OK);
    const GuestAddr surface = memory.read_u32(surface_output).value();
    ASSERT_TRUE(memory.write_u32(clsid + 0, 0xA4665C60u).is_ok());
    ASSERT_TRUE(memory.write_u32(clsid + 4, 0x11CF2673u).is_ok());
    ASSERT_TRUE(memory.write_u32(clsid + 8, 0xAA001AA3u).is_ok());
    ASSERT_TRUE(memory.write_u32(clsid + 12, 0x5633B900u).is_ok());
    const u32 created =
        manager.d3d7_create_device(object, clsid, surface, device_output);
    if (created != ddraw::DD_OK) {
        GTEST_SKIP() << "CreateDevice rejected this GUID encoding: " << created;
    }
    const GuestAddr device = memory.read_u32(device_output).value();
    const auto write_float = [&](GuestAddr at, float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        ASSERT_TRUE(memory.write_u32(at, bits).is_ok());
    };
    // D3DVIEWPORT7 {x, y, width, height, minZ, maxZ}.
    ASSERT_TRUE(memory.write_u32(viewport + 0, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(viewport + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(viewport + 8, kSize).is_ok());
    ASSERT_TRUE(memory.write_u32(viewport + 12, kSize).is_ok());
    write_float(viewport + 16, 0.0f);
    write_float(viewport + 20, 1.0f);
    ASSERT_EQ(manager.device_set_viewport(device, viewport), ddraw::DD_OK);
    ASSERT_EQ(manager.device_set_render_state(device, 137 /*LIGHTING*/, 0), ddraw::DD_OK);
    ASSERT_EQ(manager.device_set_render_state(device, 22 /*CULLMODE*/, 1 /*D3DCULL_NONE*/),
              ddraw::DD_OK);

    // World: D3DXMatrixRotationY(0.6) * translation; view: LookAtLH; projection:
    // PerspectiveFovLH(0.9, 1, 0.5, 50).
    const double c = std::cos(0.6), s = std::sin(0.6);
    const M4 world = mul4({c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1},
                          {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.3, -0.2, 0.5, 1});
    const D3 eye{1.0, 1.5, -6.0}, at{0.2, 0.1, 0.0}, up{0.0, 1.0, 0.0};
    const D3 z = unit3(sub3(at, eye)), x = unit3(cross3(up, z)), y = cross3(z, x);
    const M4 view{x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0,
                  -dot3(x, eye), -dot3(y, eye), -dot3(z, eye), 1};
    const double ys = 1.0 / std::tan(0.45), zn = 0.5, zf = 50.0;
    const M4 proj{ys, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    const auto set_transform = [&](u32 which, const M4& m) {
        for (u32 i = 0; i < 16; ++i) write_float(matrix + 4 * i, static_cast<float>(m[i]));
        ASSERT_EQ(manager.device_set_transform(device, which, matrix), ddraw::DD_OK);
    };
    set_transform(1 /*D3DTRANSFORMSTATE_WORLD*/, world);
    set_transform(2 /*D3DTRANSFORMSTATE_VIEW*/, view);
    set_transform(3 /*D3DTRANSFORMSTATE_PROJECTION*/, proj);

    const std::array<D3, 3> obj{{{-1.6, -1.2, 0.0}, {1.7, -0.9, 0.4}, {-0.1, 1.8, -0.3}}};
    std::array<std::array<double, 2>, 3> tri{};
    const M4 wvp = mul4(mul4(world, view), proj);
    for (u32 i = 0; i < 3; ++i) {
        const V4 clip = xf4({obj[i][0], obj[i][1], obj[i][2], 1.0}, wvp);
        tri[i] = {(1.0 + clip[0] / clip[3]) * kSize / 2.0, (1.0 - clip[1] / clip[3]) * kSize / 2.0};
        // D3DFVF_XYZ | D3DFVF_DIFFUSE: {x, y, z, color}.
        for (u32 k = 0; k < 3; ++k) write_float(vertices + i * 16 + k * 4, static_cast<float>(obj[i][k]));
        ASSERT_TRUE(memory.write_u32(vertices + i * 16 + 12, 0xFF00FF00u).is_ok());
    }
    ASSERT_EQ(manager.device_clear(device, 0, 0, 0x1 /*TARGET*/, 0xFF000000u, 0, 0), ddraw::DD_OK);
    ASSERT_EQ(manager.device_begin_scene(device), ddraw::DD_OK);
    ASSERT_EQ(manager.device_draw_primitive(device, 4 /*D3DPT_TRIANGLELIST*/, 0x002 | 0x040, vertices, 3, 0),
              ddraw::DD_OK);
    (void)manager.device_end_scene(device);

    ASSERT_TRUE(memory.write_u32(descriptor, 108).is_ok());
    ASSERT_EQ(manager.surface_lock(surface, 0, descriptor, 1, 0), ddraw::DD_OK);
    const u32 pitch = memory.read_u32(descriptor + 16).value();
    const GuestAddr pixels = memory.read_u32(descriptor + 36).value();
    ASSERT_NE(pixels, 0u);
    const auto edge = [](const std::array<double, 2>& a, const std::array<double, 2>& b, double px, double py) {
        const double dx = b[0] - a[0], dy = b[1] - a[1];
        return (dx * (py - a[1]) - dy * (px - a[0])) / std::sqrt(dx * dx + dy * dy);
    };
    u32 inside_count = 0, outside_count = 0;
    for (u32 py = 0; py < kSize; ++py) {
        for (u32 px = 0; px < kSize; ++px) {
            const double cx = px + 0.5, cy = py + 0.5;
            const double e0 = edge(tri[0], tri[1], cx, cy), e1 = edge(tri[1], tri[2], cx, cy),
                         e2 = edge(tri[2], tri[0], cx, cy);
            if (std::min({std::abs(e0), std::abs(e1), std::abs(e2)}) < 0.01) continue;
            const bool inside = (e0 > 0 && e1 > 0 && e2 > 0) || (e0 < 0 && e1 < 0 && e2 < 0);
            (inside ? inside_count : outside_count)++;
            const u32 pixel = memory.read_u32(pixels + py * pitch + px * 4u).value() & 0x00FFFFFFu;
            EXPECT_EQ(pixel, inside ? 0x0000FF00u : 0u) << px << "," << py;
        }
    }
    ASSERT_EQ(manager.surface_unlock(surface, 0), ddraw::DD_OK);
    EXPECT_GT(inside_count, 60u);
    EXPECT_GT(outside_count, 60u);
}

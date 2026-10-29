/// @file test_d3d9_fullscreen_cursor.cpp
/// @brief Exclusive fullscreen and the hardware cursor.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, ExclusiveCreateDeviceDrivesFullscreenCallbacks) {
    std::vector<std::string> callback_order;
    u32 fullscreen_hwnd = 0;
    bool fullscreen_on = false;
    u32 mode_hwnd = 0;
    u32 mode_w = 0;
    u32 mode_h = 0;
    u32 mode_bits = 0;
    mgr_.set_fullscreen_callback([&](u32 hwnd, bool enable) {
        callback_order.emplace_back("fullscreen");
        fullscreen_hwnd = hwnd;
        fullscreen_on = enable;
    });
    mgr_.set_display_mode_callback(
        [&](u32 hwnd, u32 width, u32 height, u32 bits) {
            callback_order.emplace_back("display-mode");
            mode_hwnd = hwnd;
            mode_w = width;
            mode_h = height;
            mode_bits = bits;
        });

    const u32 factory = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(factory, 0u);
    std::array<u32, 14> pp{};
    pp[0] = 800;
    pp[1] = 600;
    pp[2] = 22;
    pp[3] = 1;
    pp[6] = 1;
    pp[7] = TEST_HWND;
    pp[8] = 0;  // Windowed = FALSE (exclusive)
    ASSERT_TRUE(memory_.write_bytes(
        PRESENT_ADDR, reinterpret_cast<const Byte*>(pp.data()),
        sizeof(pp)).is_ok());
    ASSERT_EQ(invoke_method(
                  16, {factory, 0, 1, TEST_HWND, 0x00000040,
                       static_cast<u32>(PRESENT_ADDR),
                       static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_TRUE(fullscreen_on);
    EXPECT_EQ(fullscreen_hwnd, TEST_HWND);
    EXPECT_EQ(mode_hwnd, TEST_HWND);
    EXPECT_EQ(mode_w, 800u);
    EXPECT_EQ(mode_h, 600u);
    EXPECT_EQ(mode_bits, 32u);
    ASSERT_EQ(callback_order.size(), 2u);
    EXPECT_EQ(callback_order[0], "display-mode");
    EXPECT_EQ(callback_order[1], "fullscreen");
    mgr_.set_fullscreen_callback({});
    mgr_.set_display_mode_callback({});
}

TEST_F(D3D9ManagerTest, HardwareCursorPropertiesPositionAndShowRoundTrip) {
    // Learn: SetCursorProperties keeps hotspot + A8R8G8B8 bitmap; ShowCursor
    // returns prior visibility; SetCursorPosition updates screen coords.
    // Host show/warp/image are optional callbacks (CocoaBridge in the runtime).
    bool host_visible = true;
    i32 host_x = -1;
    i32 host_y = -1;
    unsigned host_show_calls = 0;
    unsigned host_pos_calls = 0;
    unsigned host_image_calls = 0;
    u32 host_w = 0;
    u32 host_h = 0;
    u32 host_hot_x = 0;
    u32 host_hot_y = 0;
    std::vector<u8> host_rgba;
    bool host_image_ok = true;
    mgr_.set_cursor_visible_callback([&](bool visible) {
        ++host_show_calls;
        host_visible = visible;
    });
    mgr_.set_cursor_position_callback([&](i32 x, i32 y) {
        ++host_pos_calls;
        host_x = x;
        host_y = y;
    });
    mgr_.set_cursor_image_callback(
        [&](u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
            const u8* rgba, usize bytes) {
            ++host_image_calls;
            host_w = width;
            host_h = height;
            host_hot_x = hotspot_x;
            host_hot_y = hotspot_y;
            host_rgba.assign(rgba, rgba + bytes);
            return host_image_ok;
        });

    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    // CreateOffscreenPlainSurface(32, 32, D3DFMT_A8R8G8B8, SYSTEMMEM).
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 32, 32, D3DFMT_A8R8G8B8, 2 /* SYSTEMMEM */,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 cursor_surface = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(cursor_surface, 0u);

    // Paint one A8R8G8B8 pixel (0xAARRGGBB → B,G,R,A) so the host RGBA blob
    // can be checked without requiring AppKit.
    ASSERT_EQ(invoke(D3D9InterfaceId::Surface, 13,
                     {cursor_surface, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      0, 0}),
              D3D_OK);
    const u32 pitch = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    const u32 bits = memory_.read_u32(SCRATCH_ADDR + 0x104).value();
    ASSERT_GE(pitch, 32u * 4u);
    ASSERT_NE(bits, 0u);
    const std::array<u8, 4> bgra{0x11, 0x22, 0x33, 0xFF}; // B,G,R,A
    ASSERT_TRUE(memory_.write_bytes(bits, bgra.data(), bgra.size()).is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::Surface, 14, {cursor_surface}),
              D3D_OK);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 10,
                     {device, 4, 6, cursor_surface}),
              D3D_OK);
    EXPECT_EQ(host_image_calls, 1u);
    EXPECT_EQ(host_w, 32u);
    EXPECT_EQ(host_h, 32u);
    EXPECT_EQ(host_hot_x, 4u);
    EXPECT_EQ(host_hot_y, 6u);
    ASSERT_EQ(host_rgba.size(), 32u * 32u * 4u);
    EXPECT_EQ(host_rgba[0], 0x33); // R
    EXPECT_EQ(host_rgba[1], 0x22); // G
    EXPECT_EQ(host_rgba[2], 0x11); // B
    EXPECT_EQ(host_rgba[3], 0xFF); // A
    // Device holds a ref: guest Release must not destroy the surface.
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 2, {cursor_surface}), 1u);

    // Host rejection must fail the call (no fake success).
    host_image_ok = false;
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 10,
                     {device, 4, 6, cursor_surface}),
              D3DERR_INVALIDCALL);
    host_image_ok = true;

    // Reject non-A8R8G8B8 and non-power-of-two sizes.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 32, 32, 22 /* X8R8G8B8 */, 2,
                      static_cast<u32>(SCRATCH_ADDR + 0x10), 0}),
              D3D_OK);
    const u32 wrong_fmt = memory_.read_u32(SCRATCH_ADDR + 0x10).value();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 10,
                     {device, 0, 0, wrong_fmt}),
              D3DERR_INVALIDCALL);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 30, 32, D3DFMT_A8R8G8B8, 2,
                      static_cast<u32>(SCRATCH_ADDR + 0x20), 0}),
              D3D_OK);
    const u32 wrong_size = memory_.read_u32(SCRATCH_ADDR + 0x20).value();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 10,
                     {device, 0, 0, wrong_size}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 10, {device, 0, 0, 0}),
              D3DERR_INVALIDCALL);

    // Starts hidden; ShowCursor returns previous visibility (BOOL).
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 12, {device, 0}), 0u);
    EXPECT_EQ(host_show_calls, 1u);
    EXPECT_FALSE(host_visible);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 12, {device, 1}), 0u);
    EXPECT_TRUE(host_visible);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 12, {device, 0}), 1u);
    EXPECT_FALSE(host_visible);

    invoke(D3D9InterfaceId::Device, 11, {device, 120, 340, 0});
    EXPECT_EQ(host_pos_calls, 1u);
    EXPECT_EQ(host_x, 120);
    EXPECT_EQ(host_y, 340);

    // Replacing the bitmap releases the previous device-held surface.
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 36,
                     {device, 64, 64, D3DFMT_A8R8G8B8, 2,
                      static_cast<u32>(SCRATCH_ADDR + 0x30), 0}),
              D3D_OK);
    const u32 cursor2 = memory_.read_u32(SCRATCH_ADDR + 0x30).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 10,
                     {device, 1, 2, cursor2}),
              D3D_OK);
    EXPECT_EQ(host_image_calls, 3u); // success, rejected, then replacement
    EXPECT_EQ(host_w, 64u);
    EXPECT_EQ(host_h, 64u);
    EXPECT_EQ(host_hot_x, 1u);
    EXPECT_EQ(host_hot_y, 2u);
    // Prior surface had only the device ref left after the earlier Release.
    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 1, {cursor_surface}), 0u);

    mgr_.set_cursor_visible_callback({});
    mgr_.set_cursor_position_callback({});
    mgr_.set_cursor_image_callback({});
}

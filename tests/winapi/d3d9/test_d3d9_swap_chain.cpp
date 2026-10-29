/// @file test_d3d9_swap_chain.cpp
/// @brief Clear and pass interplay, and the device-owned presentation objects.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <array>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;
using vitreo::winapi::d3d9_test::test_f32_bits;

TEST_F(D3D9ManagerTest, ClearSubmitsRecordedWorkBeforeReplacingTheTarget) {
    const auto [factory, device] = create_device();
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.render_passes_ended, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_TARGET, 0, 0, 0}),
              D3D_OK);
    // Drawn geometry must reach the GPU before the clear replaces it.
    EXPECT_EQ(backend_.record.render_passes_ended, 1u);
    EXPECT_EQ(backend_.record.back_buffer_clears, 1u);
}

TEST_F(D3D9ManagerTest, DepthOnlyClearEndsTheCurrentPassBeforeLaterDraws) {
    const auto [factory, device] = create_device(true);
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    ASSERT_EQ(backend_.record.render_passes_begun, 1u);
    ASSERT_EQ(backend_.record.render_passes_ended, 0u);

    const float clear_value = 0.625f;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_ZBUFFER, 0,
                      test_f32_bits(clear_value), 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.render_passes_ended, 1u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.render_passes_begun, 2u);
    EXPECT_EQ(backend_.record.depth_clearing_passes, 1u);
    EXPECT_FLOAT_EQ(backend_.record.last_depth_clear, clear_value);
}

TEST_F(D3D9ManagerTest, BackBufferDepthClearIsNotConsumedByTextureRtPass) {
    // RCT3 Clears Z on the back buffer, then binds 256²/128² RTs. Those
    // offscreen passes must not steal the pending swap-chain depth clear.
    const auto [factory, device] = create_device(true);
    const u32 vertex_buffer = bind_triangle_stream(device);
    (void)vertex_buffer;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 back_buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(back_buffer, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);

    const float clear_value = 0.25f;
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 43,
                     {device, 0, 0, D3DCLEAR_ZBUFFER, 0,
                      test_f32_bits(clear_value), 0}),
              D3D_OK);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 28,
                     {device, 64, 64, 21, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR + 0x40), 0}),
              D3D_OK);
    const u32 rt = memory_.read_u32(SCRATCH_ADDR + 0x40).value();
    ASSERT_NE(rt, 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, rt}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.standalone_passes, 1u);
    EXPECT_FALSE(backend_.record.last_standalone_cleared_depth);
    EXPECT_EQ(backend_.record.depth_clearing_passes, 0u);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 37, {device, 0, back_buffer}),
              D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81, {device, 4, 0, 1}), D3D_OK);
    EXPECT_EQ(backend_.record.depth_clearing_passes, 1u);
    EXPECT_FLOAT_EQ(backend_.record.last_depth_clear, clear_value);
    (void)invoke(D3D9InterfaceId::Surface, 2, {back_buffer});
}

// =============================================================================
// Device-owned objects
// =============================================================================

TEST_F(D3D9ManagerTest, GetBackBufferReturnsASurfaceDescribingTheSwapChain) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 surface = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(surface, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::Surface, 12,
                     {surface, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    std::array<u32, 8> desc{};
    ASSERT_TRUE(memory_.read_bytes(
        SCRATCH_ADDR + 0x100, reinterpret_cast<Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    EXPECT_EQ(desc[0], 22u); // D3DFMT_X8R8G8B8
    EXPECT_EQ(desc[1], 1u);  // D3DRTYPE_SURFACE
    EXPECT_EQ(desc[6], 640u);
    EXPECT_EQ(desc[7], 480u);

    // Requesting a back buffer the swap chain does not have.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 1, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, GetSwapChainAndItsPresentParametersRoundTrip) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 15, {device}), 1u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 14,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 swap_chain = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(swap_chain, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 9,
                     {swap_chain, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), 640u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), 480u);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 3,
                     {swap_chain, 0, 0, 0, 0, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.presents, 1u);

    // Present while inside a scene must fail (same as Device::Present).
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);  // BeginScene
    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 3,
                     {swap_chain, 0, 0, 0, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 42, {device}), D3D_OK);  // EndScene
    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 3,
                     {swap_chain, 0, 0, 0, 0, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.presents, 2u);
}

TEST_F(D3D9ManagerTest,
       WindowedZeroBackBufferUsesClientAreaNotDisplayMode) {
    constexpr u32 kClientW = 800;
    constexpr u32 kClientH = 600;
    mgr_.set_client_size_resolver(
        [](u32 hwnd, u32& width, u32& height) {
            if (hwnd != TEST_HWND) return false;
            width = kClientW;
            height = kClientH;
            return true;
        });

    const u32 factory = mgr_.direct3d_create9(D3D_SDK_VERSION);
    ASSERT_NE(factory, 0u);
    // Learn: windowed BackBufferWidth/Height == 0 → client area of
    // hDeviceWindow, not the adapter display mode (1920×1080 here).
    write_present_parameters(0, 0);
    EXPECT_EQ(invoke_method(
                  16, {factory, 0, 1, TEST_HWND, 0x00000040,
                       static_cast<u32>(PRESENT_ADDR),
                       static_cast<u32>(DEVICE_OUT_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR).value(), kClientW);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR + 4).value(), kClientH);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.width, kClientW);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.height, kClientH);
    EXPECT_NE(backend_.record.last_swap_chain_desc.width, 1920u);
    EXPECT_NE(backend_.record.last_swap_chain_desc.height, 1080u);

    const u32 device = memory_.read_u32(DEVICE_OUT_ADDR).value();
    ASSERT_NE(device, 0u);
    write_present_parameters(0, 0);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 13,
                     {device, static_cast<u32>(PRESENT_ADDR),
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR).value(), kClientW);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR + 4).value(), kClientH);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.width, kClientW);
    EXPECT_EQ(backend_.record.last_swap_chain_desc.height, kClientH);
}

TEST_F(D3D9ManagerTest, CreateAdditionalSwapChainRoundTripsViaOutPointer) {
    const auto [factory, device] = create_device();
    const u32 swap_chains_before = backend_.record.swap_chains_created;

    // Windowed present params for a second view (same HWND is legal).
    write_present_parameters(320, 240);
    // BackBufferCount == 0 must resolve to 1 (Learn).
    ASSERT_TRUE(memory_.write_u32(PRESENT_ADDR + 12, 0).is_ok());

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 13,
                     {device, static_cast<u32>(PRESENT_ADDR),
                      static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    const u32 additional = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(additional, 0u);
    EXPECT_EQ(backend_.record.swap_chains_created, swap_chains_before + 1u);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR).value(), 320u);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR + 4).value(), 240u);
    EXPECT_EQ(memory_.read_u32(PRESENT_ADDR + 12).value(), 1u);

    // Learn: GetNumberOfSwapChains counts CreateDevice implicits only.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 15, {device}), 1u);
    // Additional chains are not GetSwapChain-enumerable.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 14,
                     {device, 1, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3DERR_INVALIDCALL);
    // Device::GetBackBuffer / GetRasterStatus use the same implicit ordinal —
    // additional chains are reached only via IDirect3DSwapChain9.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 1, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 19,
                     {device, 1, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3DERR_INVALIDCALL);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 9,
                     {additional, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100).value(), 320u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x104).value(), 240u);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 5,
                     {additional, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x80)}),
              D3D_OK);
    const u32 back_buffer = memory_.read_u32(SCRATCH_ADDR + 0x80).value();
    ASSERT_NE(back_buffer, 0u);

    // Implicit Device::GetBackBuffer(0) remains the CreateDevice surface.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 18,
                     {device, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0xC0)}),
              D3D_OK);
    const u32 implicit_bb = memory_.read_u32(SCRATCH_ADDR + 0xC0).value();
    ASSERT_NE(implicit_bb, 0u);
    EXPECT_NE(implicit_bb, back_buffer);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 3,
                     {additional, 0, 0, 0, 0, 0}),
              D3D_OK);
    EXPECT_EQ(backend_.record.presents, 1u);

    // Fullscreen additional chains are rejected (one fullscreen chain / device).
    write_present_parameters(320, 240);
    ASSERT_TRUE(memory_.write_u32(PRESENT_ADDR + 32, 0).is_ok());  // Windowed=FALSE
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 13,
                     {device, static_cast<u32>(PRESENT_ADDR),
                      static_cast<u32>(SCRATCH_ADDR + 0xC0)}),
              D3DERR_INVALIDCALL);

    // Reset refuses while the additional chain is still held.
    write_present_parameters(640, 480);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 16,
                     {device, static_cast<u32>(PRESENT_ADDR)}),
              D3DERR_INVALIDCALL);

    // CreateAdditionalSwapChain handed out one ref; release it.
    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 2, {additional}), 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 15, {device}), 1u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 14,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3D_OK);
}

TEST_F(D3D9ManagerTest, GetRasterStatusReportsHostSyncVBlank) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    // Device::GetRasterStatus — host-synchronous present ⇒ InVBlank, ScanLine 0.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 19,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), 1u);         // InVBlank
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 4).value(), 0u);     // ScanLine

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 19, {device, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 19,
                     {device, 1, static_cast<u32>(SCRATCH_ADDR)}),
              D3DERR_INVALIDCALL);

    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 14,
                     {device, 0, static_cast<u32>(SCRATCH_ADDR + 0x40)}),
              D3D_OK);
    const u32 swap_chain = memory_.read_u32(SCRATCH_ADDR + 0x40).value();
    ASSERT_NE(swap_chain, 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 6,
                     {swap_chain, static_cast<u32>(SCRATCH_ADDR + 0x80)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x80).value(), 1u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x84).value(), 0u);

    EXPECT_EQ(invoke(D3D9InterfaceId::SwapChain, 6, {swap_chain, 0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, GammaRampDefaultsAndRoundTrips) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);

    constexpr GuestAddr kGetAddr = SCRATCH_ADDR;
    constexpr GuestAddr kSetAddr = SCRATCH_ADDR + 0x800;
    constexpr usize kRampBytes = 256u * 3u * sizeof(u16);

    // Get before Set → identity: WORD i = (i << 8) | i for each channel.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 22,
                     {device, 0, static_cast<u32>(kGetAddr)}),
              0u);
    for (u32 channel = 0; channel < 3; ++channel) {
        for (u32 i = 0; i < 256; ++i) {
            const u16 expected = static_cast<u16>((i << 8) | i);
            const GuestAddr slot = kGetAddr + (channel * 256u + i) * 2u;
            EXPECT_EQ(memory_.read_u16(slot).value(), expected)
                << "channel=" << channel << " i=" << i;
        }
    }

    // Null / bad swap-chain index → INVALIDCALL (GetRasterStatus style).
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 22, {device, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 22,
                     {device, 1, static_cast<u32>(kGetAddr)}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 21,
                     {device, 0, 0, 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 21,
                     {device, 1, 0, static_cast<u32>(kSetAddr)}),
              D3DERR_INVALIDCALL);

    // Set a non-identity ramp and read it back.
    std::array<u8, kRampBytes> custom{};
    for (u32 channel = 0; channel < 3; ++channel) {
        for (u32 i = 0; i < 256; ++i) {
            const u16 value =
                static_cast<u16>(0x1000u + channel * 0x100u + i);
            std::memcpy(custom.data() + (channel * 256u + i) * 2u, &value,
                        sizeof(value));
        }
    }
    ASSERT_TRUE(memory_.write_bytes(kSetAddr, custom.data(), custom.size())
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 21,
                     {device, 0, 0 /* D3DSGR_NO_CALIBRATION */,
                      static_cast<u32>(kSetAddr)}),
              0u);

    std::array<u8, kRampBytes> cleared{};
    ASSERT_TRUE(memory_.write_bytes(kGetAddr, cleared.data(), cleared.size())
                    .is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 22,
                     {device, 0, static_cast<u32>(kGetAddr)}),
              0u);
    std::array<u8, kRampBytes> got{};
    ASSERT_TRUE(memory_.read_bytes(kGetAddr, got.data(), got.size()).is_ok());
    EXPECT_EQ(got, custom);
}

TEST_F(D3D9ManagerTest, GetDirect3DReturnsTheCreatingFactory) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 6,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR).value(), factory);
}

TEST_F(D3D9ManagerTest, GetCreationParametersReportsWhatCreateDeviceReceived) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 9,
                     {device, static_cast<u32>(SCRATCH_ADDR)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0).value(), 0u);         // adapter
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 4).value(), 1u);         // HAL
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 8).value(), TEST_HWND);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 12).value(), 0x40u);
}

TEST_F(D3D9ManagerTest, GetAvailableTextureMemReportsNonZeroMiBAlignedBudget) {
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);
    // Slot 4 returns UINT bytes (not HRESULT).
    const u32 bytes = invoke(D3D9InterfaceId::Device, 4, {device});
    EXPECT_GE(bytes, 128u * 1024u * 1024u);
    EXPECT_EQ(bytes % (1024u * 1024u), 0u);
}

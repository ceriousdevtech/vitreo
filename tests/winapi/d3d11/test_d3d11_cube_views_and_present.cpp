// SPDX-License-Identifier: LGPL-2.1-only
//
// Multisample/cube views, factory swap-chain present, foreign surfaces and versioned-device contracts.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, Texture2DMSShaderResourceViewHasNoUnionPayload) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> texture_desc{
        16, 16, 1, 1, 28, 4, 0, 0, 0x8, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(texture_desc.data()),
        sizeof(texture_desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();

    // D3D11_TEX2DMS_SRV is one UnusedField_NothingToDefine word. Measured
    // (d3d11_view_desc_fill_probe): GetDesc echoes whatever the caller put
    // there at creation and zeroes the rest of the union.
    ASSERT_TRUE(memory.write_u32(SRV_DESC, 28).is_ok());
    ASSERT_TRUE(memory.write_u32(SRV_DESC + 4, 6).is_ok());
    ASSERT_TRUE(memory.write_u32(SRV_DESC + 8, 0x22222222u).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    ASSERT_TRUE(memory.write_u32(SRV_DESC + 8, 0xA5A5A5A5u).is_ok());
    ASSERT_TRUE(memory.write_u32(SRV_DESC + 12, 0x5A5A5A5Au).is_ok());
    set_arg(0, view);
    set_arg(1, SRV_DESC);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(SRV_DESC).value(), 28u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 4).value(), 6u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 8).value(), 0x22222222u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 12).value(), 0u);
}

TEST_F(D3D11ManagerTest, Texture2DMSArrayShaderResourceViewRoundTripsWindow) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> texture_desc{
        16, 16, 1, 4, 28, 4, 0, 0, 0x8, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(texture_desc.data()),
        sizeof(texture_desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();

    const std::array<u32, 4> srv_desc{28, 7, 1, 2};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
        sizeof(srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    ASSERT_TRUE(memory.write_u32(SRV_DESC + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SRV_DESC + 12, 0).is_ok());
    set_arg(0, view);
    set_arg(1, SRV_DESC);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(SRV_DESC).value(), 28u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 4).value(), 7u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 8).value(), 1u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 12).value(), 2u);
}

TEST_F(D3D11ManagerTest, CreatesSingleTextureCubeAndCubeView) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        16, 16, 1, 6, 87, 1, 0, 0, 0x8, 0, 0x4,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);
    EXPECT_TRUE(backend.state.last_texture_desc.cube);
    EXPECT_EQ(backend.state.last_texture_desc.array_size, 6u);

    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    set_arg(0, view);
    set_arg(1, SRV_DESC);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(SRV_DESC).value(), 87u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 4).value(), 9u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 12).value(), 1u);
}

TEST_F(D3D11ManagerTest, CreatesMipmappedTextureCubeArrayViewWindow) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        16, 16, 2, 12, 87, 1, 0, 0, 0x8, 0, 0x4,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);
    EXPECT_TRUE(backend.state.last_texture_desc.cube);
    EXPECT_EQ(backend.state.last_texture_desc.mip_levels, 2u);
    EXPECT_EQ(backend.state.last_texture_desc.array_size, 12u);

    // D3D11_TEXCUBE_ARRAY_SRV: select mip 1 and the second cube.
    const std::array<u32, 6> srv_desc{
        87, 10, 1, 1, 6, 1,
    };
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
        sizeof(srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    set_arg(0, view);
    set_arg(1, SRV_DESC);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(SRV_DESC).value(), 87u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 4).value(), 10u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 8).value(), 1u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 12).value(), 1u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 16).value(), 6u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 20).value(), 1u);
}

TEST_F(D3D11ManagerTest, ShaderResourceViewCanExposeFullMipChain) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> texture_desc{
        16, 16, 0, 1, 28, 1, 0, 0, 0x28, 0, 0x1,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(texture_desc.data()),
        sizeof(texture_desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();

    const std::array<u32, 4> srv_desc{
        28, 4, 0, 0xffffffffu,
    };
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
        sizeof(srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, SRV_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(view, 0u);

    set_arg(0, view);
    set_arg(1, SRV_DESC);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 12).value(), 5u);
}

TEST_F(D3D11ManagerTest, FactorySwapChainClearsAndPresentsBackBuffer) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIFACTORY1),
        sizeof(IID_IDXGIFACTORY1)).is_ok());

    set_arg(0, IID_ADDRESS);
    set_arg(1, FACTORY_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    dxgi.impl_CreateDXGIFactory1(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr factory = memory.read_u64(FACTORY_OUT).value();
    ASSERT_NE(factory, 0u);

    const std::array<u32, 11> prefix{
        320, 180, 60, 1, 87, 0, 0, 1, 0, 0x20, 2,
    };
    ASSERT_TRUE(memory.write_bytes(
        SWAP_DESC, reinterpret_cast<const Byte*>(prefix.data()),
        sizeof(prefix)).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 44, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(SWAP_DESC + 48, 0xCAFE).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 56, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 60, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 64, 0).is_ok());

    set_arg(0, factory);
    set_arg(1, device);
    set_arg(2, SWAP_DESC);
    set_arg(3, SWAP_OUT);
    dxgi.impl_CreateSwapChain(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr swap_chain = memory.read_u64(SWAP_OUT).value();
    ASSERT_NE(swap_chain, 0u);
    EXPECT_EQ(backend.state.swap_chain_creations, 1u);

    // IDXGISwapChain inherits IDXGIObject. Private byte data is copied into
    // object-owned storage and supports the standard size-query round trip.
    constexpr std::array<u8, 4> private_value{1, 3, 5, 7};
    ASSERT_TRUE(memory.write_bytes(
        COLOR, private_value.data(), private_value.size()).is_ok());
    set_arg(0, swap_chain);
    set_arg(1, IID_ADDRESS);
    set_arg(2, private_value.size());
    set_arg(3, COLOR);
    d3d11.impl_SwapChainMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);

    ASSERT_TRUE(memory.write_u32(BUFFER_OUT, 0).is_ok());
    set_arg(0, swap_chain);
    set_arg(1, IID_ADDRESS);
    set_arg(2, BUFFER_OUT);
    set_arg(3, 0);
    d3d11.impl_SwapChainMethod(marshaler, 5);
    // A NULL pData is the size query, and it succeeds (measured, a12 [pd]
    // "get size-query (NULL data, size 0) hr=0x00000000 size=4").
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(BUFFER_OUT).value(), private_value.size());

    ASSERT_TRUE(memory.write_u32(BUFFER_OUT, private_value.size()).is_ok());
    set_arg(3, COLOR + 8);
    d3d11.impl_SwapChainMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    std::array<u8, 4> private_copy{};
    ASSERT_TRUE(memory.read_bytes(
        COLOR + 8, private_copy.data(), private_copy.size()).is_ok());
    EXPECT_EQ(private_copy, private_value);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11TEXTURE2D),
        sizeof(IID_ID3D11TEXTURE2D)).is_ok());
    set_arg(0, swap_chain);
    set_arg(1, 0);
    set_arg(2, IID_ADDRESS);
    set_arg(3, BUFFER_OUT);
    d3d11.impl_SwapChainMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr back_buffer = memory.read_u64(BUFFER_OUT).value();
    ASSERT_NE(back_buffer, 0u);

    set_arg(0, device);
    set_arg(1, back_buffer);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);

    const std::array<float, 4> color{0.1f, 0.2f, 0.3f, 1.0f};
    ASSERT_TRUE(memory.write_bytes(
        COLOR, reinterpret_cast<const Byte*>(color.data()),
        sizeof(color)).is_ok());
    set_arg(0, context);
    set_arg(1, memory.read_u64(VIEW_OUT).value());
    set_arg(2, COLOR);
    d3d11.impl_ContextMethod(marshaler, 50);
    EXPECT_EQ(backend.state.texture_clears, 1u);
    EXPECT_EQ(backend.state.texture_creations, 1u);

    set_arg(0, swap_chain);
    set_arg(1, 1);
    set_arg(2, 0);
    d3d11.impl_SwapChainMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.presents, 1u);

    // A foreign HWND cannot own a CAMetalLayer in this host process. The
    // D3D11 frontend must read back the actual back buffer and forward it to
    // the process that owns the HWND, without also presenting the detached
    // local drawable.
    backend.state.readback_rgba.assign(320u * 180u * 4u, 0x5Au);
    std::vector<u8> forwarded;
    d3d11.set_foreign_window_presenter(
        [](u64 hwnd) { return hwnd == 0xCAFE; },
        [&](u64 hwnd, const std::vector<u8>& rgba, u32 width, u32 height) {
            EXPECT_EQ(hwnd, 0xCAFEu);
            EXPECT_EQ(width, 320u);
            EXPECT_EQ(height, 180u);
            forwarded = rgba;
            return true;
        });
    d3d11.impl_SwapChainMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.presents, 1u);
    EXPECT_EQ(backend.state.texture_readbacks, 1u);
    EXPECT_EQ(forwarded, backend.state.readback_rgba);
}

TEST_F(D3D11ManagerTest, SharedForeignSurfacePresentsWithoutCpuReadback) {
    backend.state.wait_idle_ok = true;
    d3d11.set_foreign_surface_view_resolver(
        [](u64 hwnd) -> void* {
            return hwnd == 0xCAFE
                ? reinterpret_cast<void*>(0x1234)
                : nullptr;
        });

    ASSERT_TRUE(d3d11.present_shared_window_surface(
        0xCAFE, 0x51FACE, 348, 646, /*sync_interval=*/1));
    EXPECT_EQ(backend.state.shared_texture_opens, 1u);
    EXPECT_EQ(backend.state.swap_chain_creations, 1u);
    EXPECT_EQ(backend.state.presents, 1u);
    EXPECT_EQ(backend.state.texture_readbacks, 0u);

    // Reuse the imported IOSurface and drawable chain on subsequent frames.
    ASSERT_TRUE(d3d11.present_shared_window_surface(
        0xCAFE, 0x51FACE, 348, 646, /*sync_interval=*/1));
    EXPECT_EQ(backend.state.shared_texture_opens, 1u);
    EXPECT_EQ(backend.state.swap_chain_creations, 1u);
    EXPECT_EQ(backend.state.presents, 2u);
    EXPECT_EQ(backend.state.wait_idles, 2u);

    // A flip-model chain rotates shared back-buffer handles. Import the new
    // IOSurface, but retain the HWND's CAMetalLayer/drawable chain.
    ASSERT_TRUE(d3d11.present_shared_window_surface(
        0xCAFE, 0x52FACE, 348, 646, /*sync_interval=*/1));
    EXPECT_EQ(backend.state.shared_texture_opens, 2u);
    EXPECT_EQ(backend.state.swap_chain_creations, 1u);
    EXPECT_EQ(backend.state.presents, 3u);
    EXPECT_EQ(backend.state.flushes, 3u)
        << "each owner-side IOSurface consume must use a generation-scoped "
           "completion instead of queue-wide idle";
    EXPECT_EQ(backend.state.wait_idles, 3u);
}

TEST_F(D3D11ManagerTest, ForeignFlipModelPreservesAndBackpressuresReuse) {
    // IDXGISwapChain::Present with SyncInterval 0 must return without waiting
    // for the GPU. Cross-process IOSurface publish waits on the command buffer
    // captured at Present, off the caller, not on later queue-wide idle.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIFACTORY1),
        sizeof(IID_IDXGIFACTORY1)).is_ok());

    set_arg(0, IID_ADDRESS);
    set_arg(1, FACTORY_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    dxgi.impl_CreateDXGIFactory1(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr factory = memory.read_u64(FACTORY_OUT).value();
    ASSERT_NE(factory, 0u);

    const std::array<u32, 11> prefix{
        320, 180, 60, 1, 87, 0, 0, 1, 0, 0x20, 2,
    };
    ASSERT_TRUE(memory.write_bytes(
        SWAP_DESC, reinterpret_cast<const Byte*>(prefix.data()),
        sizeof(prefix)).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 44, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(SWAP_DESC + 48, 0xCAFE).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 56, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 60, 3).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 64, 0).is_ok());

    set_arg(0, factory);
    set_arg(1, device);
    set_arg(2, SWAP_DESC);
    set_arg(3, SWAP_OUT);
    dxgi.impl_CreateSwapChain(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr swap_chain = memory.read_u64(SWAP_OUT).value();
    ASSERT_NE(swap_chain, 0u);

    backend.state.force_shared_handle = true;
    backend.state.unique_shared_handles = true;
    backend.state.wait_idle_ok = true;
    backend.state.wait_idle_delay = std::chrono::milliseconds(80);

    std::mutex presented_mutex;
    std::condition_variable presented_cv;
    std::vector<u64> presented_handles;
    bool release_first_present = false;
    d3d11.set_foreign_window_presenter(
        [](u64 hwnd) { return hwnd == 0xCAFE; },
        [](u64, const std::vector<u8>&, u32, u32) { return false; },
        [&](u64 hwnd, u64 shared_handle, u32 width, u32 height,
            u32 sync_interval, u32 src_x, u32 src_y) {
            EXPECT_EQ(hwnd, 0xCAFEu);
            EXPECT_EQ(width, 320u);
            EXPECT_EQ(height, 180u);
            // No composition clip was bound: the whole surface is presented.
            EXPECT_EQ(src_x, 0u);
            EXPECT_EQ(src_y, 0u);
            // The guest's SyncInterval has to reach the OWNER process: that is
            // where the layer lives, so it is the only place that can honour
            // it. This path used to hardcode the value, which meant a child
            // window presenting with interval 0 was display-synced anyway.
            EXPECT_EQ(sync_interval, 0u);
            std::unique_lock<std::mutex> lock(presented_mutex);
            presented_handles.push_back(shared_handle);
            presented_cv.notify_all();
            if (presented_handles.size() == 1) {
                presented_cv.wait(lock, [&] { return release_first_present; });
            }
            return true;
        });

    set_arg(0, swap_chain);
    set_arg(1, 0);
    set_arg(2, 0);
    const auto started = std::chrono::steady_clock::now();
    d3d11.impl_SwapChainMethod(marshaler, 8);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_LT(elapsed, std::chrono::milliseconds(40));
    EXPECT_EQ(backend.state.texture_readbacks, 0u);

    std::unique_lock<std::mutex> lock(presented_mutex);
    ASSERT_TRUE(presented_cv.wait_for(
        lock, std::chrono::seconds(2),
        [&] { return presented_handles.size() == 1; }));
    lock.unlock();

    // The first Present exposes B while the owner consumes A. The second may
    // enqueue B, but it must not expose A for guest rendering until the owner
    // releases A. Otherwise incremental guest draws race the foreign reader.
    std::atomic<bool> second_present_done = false;
    std::thread second_present([&] {
        set_arg(0, swap_chain);
        set_arg(1, 0);
        set_arg(2, 0);
        d3d11.impl_SwapChainMethod(marshaler, 8);
        second_present_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(second_present_done.load(std::memory_order_acquire));

    lock.lock();
    release_first_present = true;
    lock.unlock();
    presented_cv.notify_all();
    second_present.join();
    EXPECT_TRUE(second_present_done.load(std::memory_order_acquire));
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL preserves buffer-zero contents across
    // every Present. Each rotation therefore carries the coherent frame into
    // the next physical allocation before exposing it for incremental draws.
    EXPECT_EQ(backend.state.texture_copies, 2u);

    lock.lock();
    ASSERT_TRUE(presented_cv.wait_for(
        lock, std::chrono::seconds(2),
        [&] { return presented_handles.size() == 2; }));
    EXPECT_NE(presented_handles[0], presented_handles[1]);
    lock.unlock();
    EXPECT_EQ(backend.state.flushes, 2u);
    EXPECT_GE(backend.state.wait_idles, 1u);

    // DXGI_SWAP_EFFECT_FLIP_DISCARD explicitly does not preserve contents.
    // Its rotation must therefore never emit the carry-forward copy required
    // by FLIP_SEQUENTIAL. A window holds one flip-model chain at a time (a
    // second is E_ACCESSDENIED, measured), so the first one goes first.
    set_arg(0, swap_chain);
    d3d11.impl_SwapChainMethod(marshaler, 2);  // Release
    EXPECT_EQ(cpu.gpr.rax, 0u);
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 60, 4).is_ok());
    set_arg(0, factory);
    set_arg(1, device);
    set_arg(2, SWAP_DESC);
    set_arg(3, SWAP_OUT + 8);
    dxgi.impl_CreateSwapChain(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr discard_chain = memory.read_u64(SWAP_OUT + 8).value();
    ASSERT_NE(discard_chain, 0u);
    const u32 copies_before_discard = backend.state.texture_copies;
    set_arg(0, discard_chain);
    set_arg(1, 0);
    set_arg(2, 0);
    d3d11.impl_SwapChainMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.texture_copies, copies_before_discard);
    // The foreign present runs on the presenter worker, and its callback uses
    // this test's locals: let it finish before they go out of scope.
    lock.lock();
    ASSERT_TRUE(presented_cv.wait_for(
        lock, std::chrono::seconds(2),
        [&] { return presented_handles.size() == 3; }));
    lock.unlock();
}

TEST_F(D3D11ManagerTest, ResizeBuffersAcceptsFlagsAndZeroClientSize) {
    // Learn IDXGISwapChain::ResizeBuffers: SwapChainFlags may be nonzero;
    // Width/Height 0 preserve client size (or current size when HWND size
    // is unavailable). Nonzero flags must not force DXGI_ERROR_INVALID_CALL.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIFACTORY1),
        sizeof(IID_IDXGIFACTORY1)).is_ok());

    set_arg(0, IID_ADDRESS);
    set_arg(1, FACTORY_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    dxgi.impl_CreateDXGIFactory1(marshaler);
    ASSERT_EQ(cpu.gpr.rax, dxgi::S_OK);
    const GuestAddr factory = memory.read_u64(FACTORY_OUT).value();

    const std::array<u32, 11> prefix{
        320, 180, 60, 1, 87, 0, 0, 1, 0, 0x20, 2,
    };
    ASSERT_TRUE(memory.write_bytes(
        SWAP_DESC, reinterpret_cast<const Byte*>(prefix.data()),
        sizeof(prefix)).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 44, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(SWAP_DESC + 48, 0xCAFE).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 56, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 60, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(SWAP_DESC + 64, 0).is_ok());

    set_arg(0, factory);
    set_arg(1, device);
    set_arg(2, SWAP_DESC);
    set_arg(3, SWAP_OUT);
    dxgi.impl_CreateSwapChain(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr swap_chain = memory.read_u64(SWAP_OUT).value();

    // DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH = 0x2
    set_arg(0, swap_chain);
    set_arg(1, 0);       // BufferCount 0 = preserve
    set_arg(2, 640);
    set_arg(3, 360);
    set_arg(4, 0);       // DXGI_FORMAT_UNKNOWN = preserve
    set_arg(5, 0x2);
    d3d11.impl_SwapChainMethod(marshaler, 13);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.resizes, 1u);

    set_arg(0, swap_chain);
    set_arg(1, 0);
    set_arg(2, 0);       // Width 0 = preserve / client area
    set_arg(3, 0);       // Height 0
    set_arg(4, 0);
    set_arg(5, 0x2);
    d3d11.impl_SwapChainMethod(marshaler, 13);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.resizes, 2u);
}

TEST_F(D3D11ManagerTest, ExposesNoProfilerAnnotationContract) {
    create_default_device();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_ID3DUSERDEFINEDANNOTATION),
        sizeof(IID_ID3DUSERDEFINEDANNOTATION)).is_ok());
    set_arg(0, context);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_ContextMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr annotation = memory.read_u64(INTERFACE_OUT).value();
    ASSERT_NE(annotation, 0u);

    set_arg(0, annotation);
    d3d11.impl_AnnotationMethod(marshaler, 6);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    d3d11.impl_AnnotationMethod(marshaler, 3);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xFFFFFFFFu);
}

TEST_F(D3D11ManagerTest, ExposesVersionedDeviceAndFrameLatencyContracts) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11DEVICE1),
        sizeof(IID_ID3D11DEVICE1)).is_ok());
    set_arg(0, device);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(INTERFACE_OUT).value(), device);

    set_arg(0, device);
    set_arg(1, BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 43);
    EXPECT_EQ(memory.read_u64(BUFFER_OUT).value(), context);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIDEVICE2),
        sizeof(IID_IDXGIDEVICE2)).is_ok());
    set_arg(0, device);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr dxgi_device = memory.read_u64(INTERFACE_OUT).value();

    set_arg(0, dxgi_device);
    set_arg(1, 5);
    d3d11.impl_DxgiDeviceMethod(marshaler, 12);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    set_arg(0, dxgi_device);
    set_arg(1, LEVEL_OUT);
    d3d11.impl_DxgiDeviceMethod(marshaler, 13);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), 5u);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11DEVICE),
        sizeof(IID_ID3D11DEVICE)).is_ok());
    set_arg(0, device);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(INTERFACE_OUT).value(), device);
}

TEST_F(D3D11ManagerTest, ReportsOnlyImplementedFormatCapabilities) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    set_arg(0, device);
    set_arg(1, 28); // DXGI_FORMAT_R8G8B8A8_UNORM
    set_arg(2, LEVEL_OUT);
    d3d11.impl_DeviceMethod(marshaler, 29);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // Learn D3D11_FORMAT_SUPPORT bits Metal actually provides for RGBA8.
    // ANGLE's GenerateTextureFormatCaps requires SHADER_SAMPLE + MIP +
    // RENDER_TARGET (and BLENDABLE) before advertising GL renderable caps
    // that Skia uses for GrBackendFormat / createCharacterization.
    // MULTISAMPLE_RENDERTARGET is required for TextureCaps.sampleCounts.
    //
    // Corrected by Windows: the answer is the Learn FL 11_0 row, which
    // WARP reproduces ("fmt  28: cfs=00000000/03fef3f3"). The earlier mask
    // omitted the required MIP_AUTOGEN, CPU_LOCKABLE, CAST_WITHIN_BIT_LAYOUT,
    // SHADER_GATHER and BACK_BUFFER_CAST, all of which this device honours.
    constexpr u32 kRgba8Support = 0x03fef3f3u;
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), kRgba8Support);

    // RGBA8_UNORM is in the TypedUAVLoadAdditionalFormats group (Learn,
    // Typed unordered access view (UAV) loads), which this backend reports,
    // so it carries typed load *and* store — a UAV texture binds as Metal
    // access::read_write, which cannot offer one direction without the other.
    ASSERT_TRUE(memory.write_u32(LEVEL_OUT, 28).is_ok());
    set_arg(0, device);
    set_arg(1, 3); // D3D11_FEATURE_FORMAT_SUPPORT2
    set_arg(2, LEVEL_OUT);
    set_arg(3, 8);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT + 4).value() & 0xc0u, 0xc0u);

    set_arg(0, device);
    set_arg(1, 28);
    set_arg(2, 1);
    set_arg(3, LEVEL_OUT);
    d3d11.impl_DeviceMethod(marshaler, 30);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), 1u);

    set_arg(0, device);
    set_arg(1, 28);
    set_arg(2, 4);
    set_arg(3, LEVEL_OUT);
    d3d11.impl_DeviceMethod(marshaler, 30);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(LEVEL_OUT).value(), 1u);

    // Learn D3D11_FEATURE_DATA_THREADING: resource creation is thread-safe
    // here, deferred-context command lists are not.
    set_arg(0, device);
    set_arg(1, 0); // D3D11_FEATURE_THREADING
    set_arg(2, COLOR);
    set_arg(3, 8);
    d3d11.impl_DeviceMethod(marshaler, 33);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(COLOR).value(), 1u);     // ConcurrentCreates
    EXPECT_EQ(memory.read_u32(COLOR + 4).value(), 0u); // CommandLists
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test

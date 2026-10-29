// SPDX-License-Identifier: LGPL-2.1-only
//
// SwapChain2-4, composition, DXGI device services, resource usage and keyed mutex.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_swapchain.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(DxgiSwapChainContractTest, SwapChain2Through4Methods) {
    GuestAddr blt = 0;
    ASSERT_EQ(create(Desc{}, &blt), d3d11::S_OK);
    for (const GuidBytes& iid : {winapi::IID_IDXGISWAPCHAIN2, winapi::IID_IDXGISWAPCHAIN3,
                                 winapi::IID_IDXGISWAPCHAIN4}) {
        write_iid(iid);
        EXPECT_EQ(call(blt, 0, {IID, OUT}), d3d11::S_OK);
        EXPECT_EQ(memory.read_u64(OUT).value(), blt);
        (void)call(blt, 2, {});
    }
    // Source size: bitblt refuses Set; Get reports the buffer.
    EXPECT_EQ(call(blt, 29, {32, 24}), kIC);
    ASSERT_EQ(call(blt, 30, {OUT, OUT2}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 64u);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 48u);
    EXPECT_EQ(call(blt, 30, {0, 0}), kIC);
    // Latency on a chain without FRAME_LATENCY_WAITABLE_OBJECT.
    EXPECT_EQ(call(blt, 31, {2}), kIC);
    EXPECT_EQ(call(blt, 32, {OUT}), kIC);
    EXPECT_EQ(call(blt, 33, {}), 0u);
    // Matrix transform is for composition chains only.
    EXPECT_EQ(call(blt, 34, {SCRATCH}), kIC);
    EXPECT_EQ(call(blt, 35, {SCRATCH}), kIC);
    // Colour spaces of an 8-bit bitblt chain: sRGB only; NULL E_INVALIDARG.
    EXPECT_EQ(call(blt, 37, {0, 0}), d3d11::E_INVALIDARG);
    ASSERT_EQ(call(blt, 37, {0, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);
    ASSERT_EQ(call(blt, 37, {12, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    EXPECT_EQ(call(blt, 38, {12}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(blt, 38, {0}), d3d11::S_OK);
    // ResizeBuffers1 is D3D12's: INVALID_CALL on a D3D11 chain.
    EXPECT_EQ(call(blt, 39, {0, 0, 0, 0, 0, 0, 0}), kIC);
    // SetHDRMetaData: NONE and a full HDR10 block are accepted.
    EXPECT_EQ(call(blt, 40, {0, 0, 0}), d3d11::S_OK);
    EXPECT_EQ(call(blt, 40, {1, 28, SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(call(blt, 40, {1, 0, 0}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(blt, 40, {1, 4, SCRATCH}), d3d11::E_INVALIDARG);
    EXPECT_EQ(call(blt, 40, {7, 0, 0}), d3d11::E_INVALIDARG);
    (void)call(blt, 2, {});

    // A waitable flip chain.
    Desc d;
    d.effect = 4;
    d.count = 3;
    d.flags = 0x40;
    GuestAddr flip = 0;
    ASSERT_EQ(create(d, &flip), d3d11::S_OK);
    ASSERT_EQ(call(flip, 32, {OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u) << "default latency 1";
    const u32 handle = call(flip, 33, {});
    EXPECT_NE(handle, 0u);
    EXPECT_NE(call(flip, 33, {}), handle) << "a new handle each call";
    EXPECT_EQ(semaphore_count, 1) << "one frame available at once";
    semaphore_count = 0;  // the guest waited once
    EXPECT_EQ(call(flip, 31, {3}), d3d11::S_OK);
    EXPECT_EQ(semaphore_count, 2) << "raising 1 -> 3 frees two frames";
    EXPECT_EQ(call(flip, 31, {0}), kIC);
    EXPECT_EQ(call(flip, 31, {17}), d3d11::S_OK);
    EXPECT_EQ(call(flip, 31, {100}), kIC);
    // Source size on a flip chain: within the buffer, zero allowed.
    EXPECT_EQ(call(flip, 29, {32, 24}), d3d11::S_OK);
    EXPECT_EQ(call(flip, 29, {65, 48}), kIC);
    EXPECT_EQ(call(flip, 29, {0, 48}), d3d11::S_OK);
    ASSERT_EQ(call(flip, 30, {OUT, OUT2}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 48u);
    ASSERT_EQ(call(flip, 13, {0, 100, 80, 0, 0x40}), d3d11::S_OK);
    ASSERT_EQ(call(flip, 30, {OUT, OUT2}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 100u) << "reset by ResizeBuffers";
    // Flip colour spaces: sRGB, HDR10 PQ and G22 P2020.
    for (const u32 space : {0u, 12u, 17u}) {
        ASSERT_EQ(call(flip, 37, {space, OUT}), d3d11::S_OK);
        EXPECT_EQ(memory.read_u32(OUT).value(), 1u) << space;
    }
    EXPECT_EQ(call(flip, 38, {12}), d3d11::S_OK);
    EXPECT_EQ(backend.state.last_color_space,
              graphics::BackendColorSpace::Bt2100Pq);
    // GetCurrentBackBufferIndex advances with each flip present.
    EXPECT_EQ(call(flip, 36, {}), 0u);
    ASSERT_EQ(call(flip, 8, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(call(flip, 36, {}), 1u);
    // A WAITABLE chain refuses SetFullscreenState outright.
    EXPECT_EQ(call(flip, 10, {0, 0}), kIC);
    (void)call(flip, 2, {});
    EXPECT_FALSE(closed.empty()) << "the chain closes its semaphore";

    // An FP16 flip chain presents linear scRGB only.
    d.flags = 0;
    d.format = 10;
    d.hwnd = 0xF16;
    ASSERT_EQ(create(d, &flip), d3d11::S_OK);
    ASSERT_EQ(call(flip, 37, {1, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u);
    ASSERT_EQ(call(flip, 37, {0, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 0u);
    (void)call(flip, 2, {});
}

TEST_F(DxgiSwapChainContractTest, CompositionMatrixTransform) {
    // Rules probe [composition]: identity by default; scale / translate
    // (even negative or zero scale) accepted; skew or rotation INVALID_CALL
    // leaving the matrix; NULL INVALID_CALL.
    Desc d;
    d.effect = 3;
    d.count = 2;
    GuestAddr chain = 0;
    ASSERT_EQ(create1(d, 0, 0, true, &chain), d3d11::S_OK);
    ASSERT_EQ(call(chain, 35, {SCRATCH}), d3d11::S_OK);
    std::array<float, 6> m{};
    ASSERT_TRUE(memory.read_bytes(SCRATCH, reinterpret_cast<Byte*>(m.data()),
                                  sizeof(m)).is_ok());
    EXPECT_EQ(m, (std::array<float, 6>{1, 0, 0, 1, 0, 0}));
    const std::array<float, 6> scale{2, 0, 0, 0.5f, 3, 4};
    ASSERT_TRUE(memory.write_bytes(SCRATCH,
                                   reinterpret_cast<const Byte*>(scale.data()),
                                   sizeof(scale)).is_ok());
    EXPECT_EQ(call(chain, 34, {SCRATCH}), d3d11::S_OK);
    const std::array<float, 6> rotate{0, 1, -1, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(SCRATCH,
                                   reinterpret_cast<const Byte*>(rotate.data()),
                                   sizeof(rotate)).is_ok());
    EXPECT_EQ(call(chain, 34, {SCRATCH}), kIC);
    ASSERT_EQ(call(chain, 35, {SCRATCH}), d3d11::S_OK);
    ASSERT_TRUE(memory.read_bytes(SCRATCH, reinterpret_cast<Byte*>(m.data()),
                                  sizeof(m)).is_ok());
    EXPECT_EQ(m, scale);
    EXPECT_EQ(call(chain, 34, {0}), kIC);
    EXPECT_EQ(call(chain, 35, {0}), kIC);
    (void)call(chain, 2, {});
}

TEST_F(DxgiSwapChainContractTest, ParentWindowFullscreenAndStatistics) {
    GuestAddr blt = 0;
    ASSERT_EQ(create(Desc{}, &blt), d3d11::S_OK);
    // GetParent: the creating factory; IDXGIAdapter is E_NOINTERFACE.
    write_iid(IID_IDXGIFACTORY1);
    ASSERT_EQ(call(blt, 6, {IID, OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(OUT).value(), factory);
    // GetCoreWindow: INVALID_CALL, out NULL.
    ASSERT_TRUE(memory.write_u64(OUT, 1).is_ok());
    EXPECT_EQ(call(blt, 21, {IID, OUT}), kIC);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    // Background colour: opaque black by default; NULL INVALID_CALL.
    ASSERT_EQ(call(blt, 26, {SCRATCH}), d3d11::S_OK);
    std::array<float, 4> color{};
    ASSERT_TRUE(memory.read_bytes(SCRATCH,
                                  reinterpret_cast<Byte*>(color.data()),
                                  sizeof(color)).is_ok());
    EXPECT_EQ(color, (std::array<float, 4>{0, 0, 0, 1}));
    EXPECT_EQ(call(blt, 25, {0}), kIC);
    EXPECT_EQ(call(blt, 26, {0}), kIC);
    // Rotation is flip-only.
    EXPECT_EQ(call(blt, 27, {1}), kIC);
    EXPECT_EQ(call(blt, 28, {OUT}), kIC);
    // Fullscreen: FALSE fine, TRUE not available here.
    EXPECT_EQ(call(blt, 10, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(call(blt, 10, {1, 0}), kNotCurrentlyAvailable);
    // Frame statistics: NULL INVALID_CALL; a windowed bitblt chain has none.
    EXPECT_EQ(call(blt, 16, {0}), kIC);
    EXPECT_EQ(call(blt, 16, {SCRATCH}), d3d11::DXGI_ERROR_UNSUPPORTED);
    // GetContainingOutput: NULL INVALID_CALL; no output -> UNSUPPORTED.
    EXPECT_EQ(call(blt, 15, {0}), kIC);
    ASSERT_TRUE(memory.write_u64(OUT, 1).is_ok());
    EXPECT_EQ(call(blt, 15, {OUT}), d3d11::DXGI_ERROR_UNSUPPORTED);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    // ResizeTarget resizes the window's client area; NULL INVALID_CALL.
    const std::array<u32, 2> mode{320, 240};
    ASSERT_TRUE(memory.write_bytes(SCRATCH,
                                   reinterpret_cast<const Byte*>(mode.data()),
                                   sizeof(mode)).is_ok());
    EXPECT_EQ(call(blt, 14, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(resized_hwnd, kWindow);
    EXPECT_EQ(client_w, 320u);
    EXPECT_EQ(client_h, 240u);
    EXPECT_EQ(call(blt, 14, {0}), kIC);
    (void)call(blt, 2, {});
    // A flip chain: rotation round trip, statistics disjoint until the first
    // present, then reported.
    Desc d;
    d.effect = 4;
    d.count = 2;
    GuestAddr flip = 0;
    ASSERT_EQ(create(d, &flip), d3d11::S_OK);
    ASSERT_EQ(call(flip, 28, {OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 1u) << "IDENTITY by default";
    EXPECT_EQ(call(flip, 27, {2}), d3d11::S_OK);
    ASSERT_EQ(call(flip, 28, {OUT}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT).value(), 2u);
    EXPECT_EQ(call(flip, 27, {0}), kIC);
    EXPECT_EQ(call(flip, 27, {5}), kIC);
    EXPECT_EQ(call(flip, 16, {SCRATCH}),
              d3d11::DXGI_ERROR_FRAME_STATISTICS_DISJOINT);
    ASSERT_EQ(call(flip, 8, {0, 0}), d3d11::S_OK);
    EXPECT_EQ(call(flip, 16, {SCRATCH}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 1u) << "PresentCount";
    (void)call(flip, 2, {});
}

TEST_F(DxgiSwapChainContractTest, SwapChainVtableIsSwapChain4Long) {
    // The guest reaches these methods through the vtable: every slot of
    // IDXGISwapChain4 (dxgi1_5.h, 41) must hold a thunk.
    GuestAddr chain = 0;
    ASSERT_EQ(create(Desc{}, &chain), d3d11::S_OK);
    const GuestAddr vtable = memory.read_u64(chain).value();
    for (usize slot = 0; slot < 41; ++slot) {
        EXPECT_NE(memory.read_u64(vtable + slot * 8).value(), 0u) << slot;
    }
    const GuestAddr dxgi_device_vtable = [&] {
        write_iid(winapi::IID_IDXGIDEVICE4);
        set_arg(0, device);
        set_arg(1, IID);
        set_arg(2, OUT);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_DeviceMethod(marshaler, 0);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(memory.read_u64(OUT).value()).value();
    }();
    for (usize slot = 0; slot < 20; ++slot) {
        EXPECT_NE(memory.read_u64(dxgi_device_vtable + slot * 8).value(), 0u)
            << slot;
    }
    (void)call(chain, 2, {});
}

TEST_F(DxgiSwapChainContractTest, DxgiDeviceServicesAsMeasured) {
    // [dxgidevice]: QI IDXGIObject / IDXGIDevice3 / IDXGIDevice4 answer.
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    for (const GuidBytes& iid :
         {winapi::IID_IDXGIOBJECT, winapi::IID_IDXGIDEVICE3, winapi::IID_IDXGIDEVICE4}) {
        write_iid(iid);
        set_arg(0, device);
        set_arg(1, IID);
        set_arg(2, OUT);
        d3d11.impl_DeviceMethod(marshaler, 0);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    }
    const GuestAddr dxgi_device = memory.read_u64(OUT).value();
    const auto device_call = [&](usize slot, std::initializer_list<u64> args) {
        set_arg(0, dxgi_device);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        d3d11.impl_DxgiDeviceMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    // SetMaximumFrameLatency: 0 stored as 0, 17 INVALID_CALL; Get(NULL)
    // INVALID_CALL.
    EXPECT_EQ(device_call(12, {0}), d3d11::S_OK);
    EXPECT_EQ(device_call(13, {OUT2}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0u);
    EXPECT_EQ(device_call(12, {17}), kIC);
    EXPECT_EQ(device_call(13, {0}), kIC);
    // Offer / reclaim a texture's IDXGIResource.
    const std::array<u32, 11> tex{32, 32, 1, 1, 87, 1, 0, 0, 0x28, 0, 0};
    ASSERT_TRUE(memory.write_bytes(TEXTURE_DESC,
                                   reinterpret_cast<const Byte*>(tex.data()),
                                   sizeof(tex)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_TRUE(memory.write_u64(SCRATCH, texture + 0x20).is_ok());
    EXPECT_EQ(device_call(14, {1, SCRATCH, 2}), d3d11::S_OK);
    ASSERT_TRUE(memory.write_u32(OUT2, 7).is_ok());
    EXPECT_EQ(device_call(15, {1, SCRATCH, OUT2}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0u) << "not discarded";
    EXPECT_EQ(device_call(15, {1, SCRATCH, OUT2}), d3d11::E_INVALIDARG)
        << "not offered";
    EXPECT_EQ(device_call(14, {1, SCRATCH, 0}), d3d11::E_INVALIDARG)
        << "priority 0";
    EXPECT_EQ(device_call(18, {1, SCRATCH, 1, 1}), d3d11::S_OK);
    ASSERT_TRUE(memory.write_u32(OUT2, 7).is_ok());
    EXPECT_EQ(device_call(19, {1, SCRATCH, OUT2}), d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0u) << "RESULT_OK";
    // EnqueueSetEvent(NULL) is E_INVALIDARG.
    EXPECT_EQ(device_call(16, {0}), d3d11::E_INVALIDARG);
    // CreateSurface(2): two surfaces.
    const std::array<u32, 5> surface_desc{64, 64, 87, 1, 0};
    ASSERT_TRUE(memory.write_bytes(
        SCRATCH + 0x40, reinterpret_cast<const Byte*>(surface_desc.data()),
        sizeof(surface_desc)).is_ok());
    ASSERT_TRUE(memory.write_u64(SCRATCH + 0x80, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(SCRATCH + 0x88, 0).is_ok());
    EXPECT_EQ(device_call(8, {SCRATCH + 0x40, 2, 0x20, 0, SCRATCH + 0x80}),
              d3d11::S_OK);
    EXPECT_NE(memory.read_u64(SCRATCH + 0x80).value(), 0u);
    EXPECT_NE(memory.read_u64(SCRATCH + 0x88).value(), 0u);
    // QueryResourceResidency under the discrete-class adapter profile
    // (.docs/contracts/dxgi.md): Learn DXGI_RESIDENCY FULLY_RESIDENT (1),
    // "located in video memory", for a resource with GPU storage; a STAGING
    // texture is CPU memory, RESIDENT_IN_SHARED_MEMORY (2). Any interface of
    // the resource names it. (Windows, a UMA adapter, answers 2 for the
    // DEFAULT texture, dxgi_a11_contracts_probe.)
    const std::array<u32, 11> staging_desc{32, 32, 1, 1, 87, 1, 0,
                                           3, 0, 0x20000, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(staging_desc.data()),
        sizeof(staging_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr staging = memory.read_u64(TEXTURE_OUT).value();
    const std::array<u32, 6> buffer_desc{64, 0, 0x1, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        DESC, reinterpret_cast<const Byte*>(buffer_desc.data()),
        sizeof(buffer_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, DESC);
    set_arg(2, 0);
    set_arg(3, OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(OUT).value();
    const std::array<std::pair<GuestAddr, u32>, 7> residency{{
        {texture + 0x20, 1u},                        // IDXGIResource
        {texture, 1u},                               // ID3D11Texture2D
        {memory.read_u64(SCRATCH + 0x80).value(), 1u},  // CreateSurface
        {staging + 0x20, 2u},
        {staging, 2u},
        {buffer, 1u},
        {buffer + 0x20, 1u},
    }};
    for (usize i = 0; i < residency.size(); ++i) {
        ASSERT_TRUE(memory.write_u64(SCRATCH + 0x100 + i * 8,
                                     residency[i].first).is_ok());
        ASSERT_TRUE(memory.write_u32(SCRATCH + 0x200 + i * 4, 0xCDu).is_ok());
    }
    EXPECT_EQ(device_call(9, {SCRATCH + 0x100, SCRATCH + 0x200,
                              residency.size()}),
              d3d11::S_OK);
    for (usize i = 0; i < residency.size(); ++i) {
        EXPECT_EQ(memory.read_u32(SCRATCH + 0x200 + i * 4).value(),
                  residency[i].second) << "entry " << i;
    }
    // An entry that is no resource: E_INVALIDARG, nothing written.
    ASSERT_TRUE(memory.write_u64(SCRATCH + 0x108, device).is_ok());
    for (usize i = 0; i < 2; ++i) {
        ASSERT_TRUE(memory.write_u32(SCRATCH + 0x200 + i * 4, 0xCDu).is_ok());
    }
    EXPECT_EQ(device_call(9, {SCRATCH + 0x100, SCRATCH + 0x200, 2}),
              d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u32(SCRATCH + 0x200).value(), 0xCDu);
    EXPECT_EQ(memory.read_u32(SCRATCH + 0x204).value(), 0xCDu);
    EXPECT_EQ(device_call(9, {0, SCRATCH + 0x200, 1}), d3d11::E_INVALIDARG);
    EXPECT_EQ(device_call(9, {SCRATCH + 0x100, 0, 1}), d3d11::E_INVALIDARG);
}

TEST_F(DxgiSwapChainContractTest, ResourceUsagePriorityAndSharing) {
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto make = [&](std::array<u32, 11> desc) {
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, TEXTURE_OUT);
        d3d11.impl_DeviceMethod(marshaler, 5);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(TEXTURE_OUT).value();
    };
    const auto usage_of = [&](GuestAddr texture) {
        set_arg(0, texture + 0x20);
        set_arg(1, OUT2);
        d3d11.impl_DxgiResourceMethod(marshaler, 9);
        return memory.read_u32(OUT2).value();
    };
    // [resource] GetUsage: UAV|SRV 0x410, STAGING 0x3, DYNAMIC SRV 0x11.
    // desc: w h mips array fmt samples quality usage bind cpu misc
    EXPECT_EQ(usage_of(make({8, 8, 1, 1, 28, 1, 0, 0, 0x88, 0, 0})), 0x410u);
    EXPECT_EQ(usage_of(make({8, 8, 1, 1, 28, 1, 0, 3, 0, 0x20000, 0})), 0x3u);
    EXPECT_EQ(usage_of(make({8, 8, 1, 1, 28, 1, 0, 2, 0x8, 0x10000, 0})),
              0x11u);
    const GuestAddr plain = make({64, 64, 2, 1, 87, 1, 0, 0, 0x28, 0, 0});
    // Default eviction priority DXGI_RESOURCE_PRIORITY_NORMAL.
    set_arg(0, plain + 0x20);
    set_arg(1, OUT2);
    d3d11.impl_DxgiResourceMethod(marshaler, 11);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0x78000000u);
    // CreateSharedHandle on an unshared resource: E_INVALIDARG, handle NULL.
    ASSERT_TRUE(memory.write_u64(OUT, 1).is_ok());
    set_arg(0, plain + 0x20);
    set_arg(1, 0);
    set_arg(2, 0x80000001u);
    set_arg(3, 0);
    set_arg(4, OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 13);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(OUT).value(), 0u);
    // CreateSubresourceSurface(1) of a 2-mip 64x64: a 32x32 surface whose
    // GetResource names subresource 1; index 5 is E_INVALIDARG.
    set_arg(0, plain + 0x20);
    set_arg(1, 1);
    set_arg(2, OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 12);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr surface = memory.read_u64(OUT).value();
    set_arg(0, surface);
    set_arg(1, SCRATCH);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(SCRATCH).value(), 32u);
    EXPECT_EQ(memory.read_u32(SCRATCH + 4).value(), 32u);
    write_iid(winapi::IID_ID3D11TEXTURE2D);
    set_arg(0, surface);
    set_arg(1, IID);
    set_arg(2, OUT2);
    set_arg(3, SCRATCH + 0x20);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 13);
    EXPECT_EQ(memory.read_u64(OUT2).value(), plain);
    EXPECT_EQ(memory.read_u32(SCRATCH + 0x20).value(), 1u);
    release_texture(plain);
    set_arg(0, surface);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 2);
    set_arg(0, plain + 0x20);
    set_arg(1, 5);
    set_arg(2, OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 12);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    // A buffer is an IDXGIResource too (GetUsage 0 for a vertex buffer).
    const std::array<u32, 6> buffer_desc{64, 0, 0x1, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(buffer_desc.data()),
        sizeof(buffer_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    write_iid(winapi::IID_IDXGIRESOURCE);
    set_arg(0, buffer);
    set_arg(1, IID);
    set_arg(2, OUT);
    d3d11.impl_BufferMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer_resource = memory.read_u64(OUT).value();
    set_arg(0, buffer_resource);
    set_arg(1, OUT2);
    ASSERT_TRUE(memory.write_u32(OUT2, 0xFF).is_ok());
    d3d11.impl_DxgiResourceMethod(marshaler, 9);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(OUT2).value(), 0u);
}

TEST_F(DxgiSwapChainContractTest, KeyedMutexIsSharedAcrossOpeningsAndWaits) {
    // [keyedmutex]: the state belongs to the shared surface; a wrong key
    // waits for the timeout; releasing unowned is INVALID_CALL.
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const std::array<u32, 11> desc{16, 16, 1, 1, 87, 1, 0, 0, 0x28, 0, 0x100};
    ASSERT_TRUE(memory.write_bytes(TEXTURE_DESC,
                                   reinterpret_cast<const Byte*>(desc.data()),
                                   sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr original = memory.read_u64(TEXTURE_OUT).value();
    const GuestAddr original_mutex = original + 0x28;
    // Open the same surface a second time by its shared handle.
    set_arg(0, original + 0x20);
    set_arg(1, OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 8);  // GetSharedHandle
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const u64 handle = memory.read_u64(OUT).value();
    ASSERT_NE(handle, 0u);
    write_iid(winapi::IID_ID3D11TEXTURE2D);
    set_arg(0, device);
    set_arg(1, handle);
    set_arg(2, IID);
    set_arg(3, RESOURCE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 28);  // OpenSharedResource
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr opened = memory.read_u64(RESOURCE_OUT).value();
    const GuestAddr opened_mutex = opened + 0x28;
    const auto sync = [&](GuestAddr mutex, usize slot, u64 key, u32 ms) {
        set_arg(0, mutex);
        set_arg(1, key);
        set_arg(2, ms);
        d3d11.impl_KeyedMutexMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(sync(original_mutex, 9, 0, 0), kIC) << "ReleaseSync unowned";
    ASSERT_EQ(sync(original_mutex, 8, 0, 0), d3d11::S_OK);
    EXPECT_EQ(sync(original_mutex, 8, 0, 0), kIC) << "recursive";
    ASSERT_EQ(sync(original_mutex, 9, 7, 0), d3d11::S_OK);
    EXPECT_EQ(sync(opened_mutex, 8, 0, 0), WAIT_TIMEOUT) << "key is 7";
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(sync(opened_mutex, 8, 8, 200), WAIT_TIMEOUT);
    EXPECT_GE(std::chrono::steady_clock::now() - start,
              std::chrono::milliseconds(150));
    EXPECT_EQ(sync(opened_mutex, 8, 7, 0), d3d11::S_OK);
    EXPECT_EQ(sync(original_mutex, 8, 7, 0), WAIT_TIMEOUT)
        << "the opening owns it";
    EXPECT_EQ(sync(opened_mutex, 9, 9, 0), d3d11::S_OK);
    EXPECT_EQ(sync(original_mutex, 8, 9, 0), d3d11::S_OK);
    // The full 64-bit key.
    EXPECT_EQ(sync(original_mutex, 9, 0x100000005ull, 0), d3d11::S_OK);
    EXPECT_EQ(sync(opened_mutex, 8, 5, 0), WAIT_TIMEOUT);
    EXPECT_EQ(sync(opened_mutex, 8, 0x100000005ull, 0), d3d11::S_OK);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test

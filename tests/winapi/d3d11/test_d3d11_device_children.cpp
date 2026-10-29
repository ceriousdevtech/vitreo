// SPDX-License-Identifier: LGPL-2.1-only
//
// Device-child lifetime, multithread facets, private data and the resource facet QueryInterface graph.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

// [ctx] and [dev]: a device child -- the immediate context included -- holds
// ONE device reference while the application holds it, never for the
// runtime's own references. A bound buffer the application released
// answers 0 and drops the device reference; IAGet takes both back.
TEST_F(D3D11ManagerTest, DeviceChildrenHoldTheDeviceOnlyWhileTheAppDoes) {
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto device_count = [&](GuestAddr device) {
        return count_of(d3d11, marshaler, cpu, device,
                        &D3D11Manager::impl_DeviceMethod);
    };
    // Without ppImmediateContext the device count is 1; GetImmediateContext
    // takes it to 2, releasing the context back to 1.
    prepare_create();
    set_arg(9, 0);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr lone = memory.read_u64(DEVICE_OUT).value();
    EXPECT_EQ(device_count(lone), 1u);
    set_arg(0, lone);
    set_arg(1, CONTEXT_OUT);
    d3d11.impl_DeviceMethod(marshaler, 40);  // GetImmediateContext
    const GuestAddr lone_context = memory.read_u64(CONTEXT_OUT).value();
    EXPECT_EQ(device_count(lone), 2u);
    set_arg(0, lone_context);
    d3d11.impl_ContextMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(device_count(lone), 1u);

    // With ppImmediateContext: 2.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    EXPECT_EQ(device_count(device), 2u);

    const std::array<u32, 6> vb_desc{64, 0, 0x1, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(vb_desc.data()),
        sizeof(vb_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    EXPECT_EQ(device_count(device), 3u);  // created: +1

    constexpr GuestAddr BINDING = GPU_BUFFER_INITIAL;
    constexpr GuestAddr STRIDE = GPU_BUFFER_INITIAL + 8;
    constexpr GuestAddr OFFSET = GPU_BUFFER_INITIAL + 12;
    ASSERT_TRUE(memory.write_u64(BINDING, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(STRIDE, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(OFFSET, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDING);
    set_arg(4, STRIDE);
    set_arg(5, OFFSET);
    d3d11.impl_ContextMethod(marshaler, 18);  // IASetVertexBuffers
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, buffer,
                       &D3D11Manager::impl_BufferMethod), 1u);  // bound: 1
    EXPECT_EQ(device_count(device), 3u);  // bound: +1

    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);           // released(ret 0)
    EXPECT_EQ(device_count(device), 2u);  // +0
    // A Release past the application's last reference takes nothing the
    // binding holds.
    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ASSERT_TRUE(memory.write_u64(BINDING, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDING);
    set_arg(4, STRIDE);
    set_arg(5, OFFSET);
    d3d11.impl_ContextMethod(marshaler, 79);  // IAGetVertexBuffers
    EXPECT_EQ(memory.read_u64(BINDING).value(), buffer);
    EXPECT_EQ(device_count(device), 3u);  // IAGet: +1
    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);           // release-of-get(ret 0)
    EXPECT_EQ(device_count(device), 2u);

    ASSERT_TRUE(memory.write_u64(BINDING, 0).is_ok());
    set_arg(0, context);
    set_arg(1, 0);
    set_arg(2, 1);
    set_arg(3, BINDING);
    set_arg(4, STRIDE);
    set_arg(5, OFFSET);
    d3d11.impl_ContextMethod(marshaler, 18);  // unbind
    EXPECT_FALSE(kept_alive_by_runtime(buffer, &D3D11Manager::impl_BufferMethod));
    EXPECT_EQ(device_count(device), 2u);

    // tex+srv: created +2; the texture released under its view -> 0 and the
    // device count drops; GetResource takes both back.
    const std::array<u32, 11> tex_desc{8, 8, 1, 1, 28, 1, 0, 0, 8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(tex_desc.data()),
        sizeof(tex_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    EXPECT_EQ(device_count(device), 4u);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, texture,
                       &D3D11Manager::impl_Texture2DMethod), 1u)
        << "a view does not count on its resource";
    set_arg(0, texture);
    d3d11.impl_Texture2DMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(device_count(device), 3u);
    set_arg(0, view);
    set_arg(1, RESOURCE_OUT);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 7);  // GetResource
    EXPECT_EQ(memory.read_u64(RESOURCE_OUT).value(), texture);
    EXPECT_EQ(device_count(device), 4u);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, texture,
                       &D3D11Manager::impl_Texture2DMethod), 1u);
    set_arg(0, texture);
    d3d11.impl_Texture2DMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    set_arg(0, view);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(device_count(device), 2u);
    EXPECT_FALSE(kept_alive_by_runtime(texture,
                                       &D3D11Manager::impl_Texture2DMethod));

    // [dedup]: a second, identical CreateBlendState is the same object with
    // count 2 and no second device reference.
    std::array<u32, 2 + 8 * 8> blend{};
    blend[2 + 7] = 0x5;  // RenderTarget[0].RenderTargetWriteMask
    constexpr GuestAddr BLEND_DESC = DATA_BASE + 0x1800;
    constexpr GuestAddr BLEND_OUT = DATA_BASE + 0x1900;
    ASSERT_TRUE(memory.write_bytes(
        BLEND_DESC, reinterpret_cast<const Byte*>(blend.data()),
        sizeof(blend)).is_ok());
    GuestAddr blends[2]{};
    for (GuestAddr& made : blends) {
        set_arg(0, device);
        set_arg(1, BLEND_DESC);
        set_arg(2, BLEND_OUT);
        d3d11.impl_DeviceMethod(marshaler, 20);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        made = memory.read_u64(BLEND_OUT).value();
    }
    EXPECT_EQ(blends[0], blends[1]);
    EXPECT_EQ(device_count(device), 3u);
    EXPECT_EQ(count_of(d3d11, marshaler, cpu, blends[0],
                       &D3D11Manager::impl_BlendStateMethod), 2u);
}

// [mt]: ID3D11Multithread / ID3D10Multithread (one IID in the SDK) on the
// device and on the immediate context -- two facets, each QueryInterface'd
// and counted as its owner, one protection flag -- and Enter/Leave is the
// device's own recursive lock. A SINGLETHREADED device refuses both.
TEST_F(D3D11ManagerTest, MultithreadFacetsShareOneFlagAndTheDeviceLock) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto query = [&](GuestAddr self, const GuidBytes& iid,
                           void (D3D11Manager::*method)(Marshaler&, usize)) {
        EXPECT_TRUE(memory.write_bytes(
            IID_ADDRESS, reinterpret_cast<const Byte*>(iid.data()),
            iid.size()).is_ok());
        EXPECT_TRUE(memory.write_u64(INTERFACE_OUT, 0xA5A5).is_ok());
        set_arg(0, self);
        set_arg(1, IID_ADDRESS);
        set_arg(2, INTERFACE_OUT);
        (d3d11.*method)(marshaler, 0);
        return std::pair{static_cast<u32>(cpu.gpr.rax),
                         memory.read_u64(INTERFACE_OUT).value()};
    };
    const auto [device_hr, device_mt] =
        query(device, winapi::IID_ID3D11MULTITHREAD, &D3D11Manager::impl_DeviceMethod);
    const auto [context_hr, context_mt] = query(
        context, winapi::IID_ID3D11MULTITHREAD, &D3D11Manager::impl_ContextMethod);
    ASSERT_EQ(device_hr, d3d11::S_OK);
    ASSERT_EQ(context_hr, d3d11::S_OK);
    EXPECT_NE(device_mt, context_mt);
    EXPECT_EQ(query(device, winapi::IID_ID3D10MULTITHREAD,
                    &D3D11Manager::impl_DeviceMethod).second, device_mt);
    // COM identity: each facet is its owner.
    EXPECT_EQ(query(device_mt, winapi::IID_IUNKNOWN,
                    &D3D11Manager::impl_MultithreadMethod).second, device);
    EXPECT_EQ(query(context_mt, winapi::IID_IUNKNOWN,
                    &D3D11Manager::impl_MultithreadMethod).second, context);
    // Each counts with its owner (measured: device facet AddRef = the
    // device's count; the context facet moves the context's, not the
    // device's).
    const u32 device_before = count_of(d3d11, marshaler, cpu, device,
                                       &D3D11Manager::impl_DeviceMethod);
    set_arg(0, device_mt);
    d3d11.impl_MultithreadMethod(marshaler, 1);
    EXPECT_EQ(cpu.gpr.rax, device_before + 1);
    set_arg(0, device_mt);
    d3d11.impl_MultithreadMethod(marshaler, 2);

    const auto call = [&](GuestAddr self, usize slot, u64 arg = 0) {
        set_arg(0, self);
        set_arg(1, arg);
        d3d11.impl_MultithreadMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(call(device_mt, 6), 0u);     // GetMultithreadProtected
    EXPECT_EQ(call(device_mt, 5, 1), 0u);  // Set(TRUE) returns the old 0
    EXPECT_EQ(call(device_mt, 6), 1u);
    EXPECT_EQ(call(device_mt, 5, 0), 1u);  // Set(FALSE) returns 1
    EXPECT_EQ(call(device_mt, 5, 5), 0u);  // Set(5) ...
    EXPECT_EQ(call(device_mt, 6), 1u);     // ... reads TRUE
    EXPECT_EQ(call(context_mt, 6), 1u);    // one flag for both facets
    EXPECT_EQ(call(context_mt, 5, 0), 1u);
    EXPECT_EQ(call(device_mt, 6), 0u);

    // Enter twice, Leave twice: the device lock is held between, by this
    // thread only, and released after the second Leave.
    const auto held_elsewhere = [&] {
        bool acquired = false;
        std::thread probe([&] {
            acquired = d3d11.api_mutex().try_lock();
            if (acquired) d3d11.api_mutex().unlock();
        });
        probe.join();
        return !acquired;
    };
    (void)call(device_mt, 3);
    (void)call(context_mt, 3);
    EXPECT_TRUE(held_elsewhere());
    (void)call(device_mt, 4);
    EXPECT_TRUE(held_elsewhere());
    (void)call(context_mt, 4);
    EXPECT_FALSE(held_elsewhere());
    // A Leave with no Enter leaves nothing unbalanced.
    (void)call(device_mt, 4);
    EXPECT_FALSE(held_elsewhere());

    // SINGLETHREADED: no critical section to expose (measured E_NOINTERFACE,
    // *ppv NULLed).
    prepare_create();
    set_arg(3, d3d11::CREATE_DEVICE_SINGLETHREADED);
    d3d11.impl_D3D11CreateDevice(marshaler);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const auto [single_hr, single_out] =
        query(memory.read_u64(DEVICE_OUT).value(), winapi::IID_ID3D11MULTITHREAD,
              &D3D11Manager::impl_DeviceMethod);
    EXPECT_EQ(single_hr, d3d11::E_NOINTERFACE);
    EXPECT_EQ(single_out, 0u);
}

// [pd]: the private-data contract, with interface entries keyed by the
// object so every facet sees them, and the immediate context's own store.
TEST_F(D3D11ManagerTest, PrivateDataFollowsTheMeasuredContract) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    constexpr GuestAddr GUID_A = DATA_BASE + 0x1800;
    constexpr GuestAddr SIZE_OUT = DATA_BASE + 0x1820;
    constexpr GuestAddr DATA = DATA_BASE + 0x1830;
    constexpr GuestAddr GUEST_VTABLE = DATA_BASE + 0x1840;
    constexpr GuestAddr GUEST_IFACE = DATA_BASE + 0x1880;
    constexpr GuestAddr GUEST_ADDREF = 0x72000010;
    constexpr GuestAddr GUEST_RELEASE = 0x72000020;
    const std::array<u8, 16> guid{0xA1, 0xA1, 0xA1, 0xA1, 1, 0, 2, 0,
                                  3, 4, 5, 6, 7, 8, 9, 10};
    ASSERT_TRUE(memory.write_bytes(
        GUID_A, reinterpret_cast<const Byte*>(guid.data()), 16).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 8, GUEST_ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_VTABLE + 16, GUEST_RELEASE).is_ok());
    ASSERT_TRUE(memory.write_u64(GUEST_IFACE, GUEST_VTABLE).is_ok());
    std::vector<GuestAddr> calls;
    d3d11.set_guest_function_caller(
        [&calls](GuestAddr function, const u64*, usize) -> u64 {
            calls.push_back(function);
            return 1;
        });

    const std::array<u32, 11> desc{4, 4, 1, 1, 28, 1, 0, 0, 8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(winapi::IID_IDXGIRESOURCE.data()),
        16).is_ok());
    set_arg(0, texture);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_Texture2DMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr resource = memory.read_u64(INTERFACE_OUT).value();

    const auto set = [&](u32 size, GuestAddr data) {
        set_arg(0, texture);
        set_arg(1, GUID_A);
        set_arg(2, size);
        set_arg(3, data);
        d3d11.impl_Texture2DMethod(marshaler, 5);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto get = [&](u32 capacity, GuestAddr data, bool via_facet) {
        EXPECT_TRUE(memory.write_u32(SIZE_OUT, capacity).is_ok());
        set_arg(0, via_facet ? resource : texture);
        set_arg(1, GUID_A);
        set_arg(2, SIZE_OUT);
        set_arg(3, data);
        if (via_facet) {
            d3d11.impl_DxgiResourceMethod(marshaler, 5);
        } else {
            d3d11.impl_Texture2DMethod(marshaler, 4);
        }
        return std::pair{static_cast<u32>(cpu.gpr.rax),
                         memory.read_u32(SIZE_OUT).value()};
    };
    constexpr u32 kNotFound = 0x887A0002u;
    constexpr u32 kMoreData = 0x887A0003u;
    ASSERT_TRUE(memory.write_u32(DATA, 0x11223344u).is_ok());
    EXPECT_EQ(get(4, DATA + 8, false), (std::pair{kNotFound, 0u}));
    EXPECT_EQ(set(4, DATA), d3d11::S_OK);
    // A NULL pData is the size query, and succeeds, whatever *pDataSize held.
    EXPECT_EQ(get(0, 0, false), (std::pair{d3d11::S_OK, 4u}));
    EXPECT_EQ(get(100, 0, false), (std::pair{d3d11::S_OK, 4u}));
    EXPECT_EQ(get(2, DATA + 8, false), (std::pair{kMoreData, 4u}));
    // Set(size 4, NULL) deletes.
    EXPECT_EQ(set(4, 0), d3d11::S_OK);
    EXPECT_EQ(get(4, DATA + 8, false), (std::pair{kNotFound, 0u}));
    // Set(0, non-NULL) stores an empty entry.
    EXPECT_EQ(set(0, DATA), d3d11::S_OK);
    EXPECT_EQ(get(4, DATA + 8, false), (std::pair{d3d11::S_OK, 0u}));
    // Delete, then delete what is not there: S_FALSE.
    EXPECT_EQ(set(0, 0), d3d11::S_OK);
    EXPECT_EQ(set(0, 0), 1u);

    // An interface set through the texture is the IDXGIResource facet's too.
    calls.clear();
    set_arg(0, texture);
    set_arg(1, GUID_A);
    set_arg(2, GUEST_IFACE);
    d3d11.impl_Texture2DMethod(marshaler, 6);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(calls, std::vector<GuestAddr>{GUEST_ADDREF});
    ASSERT_TRUE(memory.write_u64(DATA + 8, 0).is_ok());
    EXPECT_EQ(get(8, DATA + 8, true), (std::pair{d3d11::S_OK, 8u}));
    EXPECT_EQ(memory.read_u64(DATA + 8).value(), GUEST_IFACE);
    // SetPrivateDataInterface(NULL) through the facet releases it and keeps
    // a NULL entry.
    calls.clear();
    set_arg(0, resource);
    set_arg(1, GUID_A);
    set_arg(2, 0);
    d3d11.impl_DxgiResourceMethod(marshaler, 4);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // Two releases: the one GetPrivateData handed out is the caller's to
    // drop; this one is the object's.
    EXPECT_EQ(calls, std::vector<GuestAddr>{GUEST_RELEASE});
    ASSERT_TRUE(memory.write_u64(DATA + 8, 0xA5A5).is_ok());
    EXPECT_EQ(get(8, DATA + 8, false), (std::pair{d3d11::S_OK, 8u}));
    EXPECT_EQ(memory.read_u64(DATA + 8).value(), 0u);

    // Device and immediate context: separate stores.
    set_arg(0, device);
    set_arg(1, GUID_A);
    set_arg(2, 4);
    set_arg(3, DATA);
    d3d11.impl_DeviceMethod(marshaler, 35);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_TRUE(memory.write_u32(SIZE_OUT, 4).is_ok());
    set_arg(0, context);
    set_arg(1, GUID_A);
    set_arg(2, SIZE_OUT);
    set_arg(3, DATA + 8);
    d3d11.impl_ContextMethod(marshaler, 4);
    EXPECT_EQ(cpu.gpr.rax, kNotFound);
}

// [qi] and refmodel [facet]: one QueryInterface graph per resource, answered
// alike from every facet -- a buffer and a Texture1D are DXGI surfaces, a
// Texture3D is a DXGI resource but no surface, the keyed mutex's IUnknown is
// its texture -- and an SRV answers IDXGISurface as a runtime quirk.
TEST_F(D3D11ManagerTest, ResourceFacetsAnswerOneQueryInterfaceGraph) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto query = [&](GuestAddr self, const GuidBytes& iid,
                           void (D3D11Manager::*method)(Marshaler&, usize)) {
        EXPECT_TRUE(memory.write_bytes(
            IID_ADDRESS, reinterpret_cast<const Byte*>(iid.data()),
            iid.size()).is_ok());
        set_arg(0, self);
        set_arg(1, IID_ADDRESS);
        set_arg(2, INTERFACE_OUT);
        (d3d11.*method)(marshaler, 0);
        const GuestAddr out = memory.read_u64(INTERFACE_OUT).value();
        if (cpu.gpr.rax == d3d11::S_OK) {
            // Hand the reference back through the object's own vtable.
            set_arg(0, out);
            (d3d11.*method)(marshaler, 2);
        }
        return out;
    };

    const std::array<u32, 6> vb_desc{64, 0, 0x1, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(vb_desc.data()),
        sizeof(vb_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    const GuestAddr surface = query(buffer, winapi::IID_IDXGISURFACE,
                                    &D3D11Manager::impl_BufferMethod);
    ASSERT_NE(surface, 0u);
    EXPECT_NE(surface, buffer);
    EXPECT_EQ(query(buffer, winapi::IID_IDXGISURFACE2,
                    &D3D11Manager::impl_BufferMethod), surface);
    EXPECT_EQ(query(surface, winapi::IID_IUNKNOWN,
                    &D3D11Manager::impl_DxgiSurfaceMethod), buffer);
    EXPECT_NE(query(surface, winapi::IID_IDXGIRESOURCE1,
                    &D3D11Manager::impl_DxgiSurfaceMethod), 0u);
    // GetDesc: ByteWidth x 1, DXGI_FORMAT_UNKNOWN, one sample.
    constexpr GuestAddr SURFACE_DESC = DATA_BASE + 0x1800;
    set_arg(0, surface);
    set_arg(1, SURFACE_DESC);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 8);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    std::array<u32, 5> words{};
    ASSERT_TRUE(memory.read_bytes(SURFACE_DESC,
                                  reinterpret_cast<Byte*>(words.data()),
                                  sizeof(words)).is_ok());
    EXPECT_EQ(words, (std::array<u32, 5>{64, 1, 0, 1, 0}));
    // Map of a DEFAULT buffer's surface: E_INVALIDARG; no DC.
    set_arg(0, surface);
    set_arg(1, SURFACE_DESC);
    set_arg(2, 1);  // DXGI_MAP_READ
    d3d11.impl_DxgiSurfaceMethod(marshaler, 9);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    set_arg(0, surface);
    set_arg(1, 0);
    set_arg(2, SURFACE_DESC);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 11);
    EXPECT_EQ(cpu.gpr.rax, 0x887A0001u);

    // Texture1D: ID3D11Texture1D, never Texture2D, and a DXGI surface.
    std::array<u32, 8> tex1d_desc{16, 1, 1, 28, 0, 8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(tex1d_desc.data()),
        sizeof(tex1d_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 4);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr tex1d = memory.read_u64(TEXTURE_OUT).value();
    EXPECT_EQ(query(tex1d, winapi::IID_ID3D11TEXTURE1D,
                    &D3D11Manager::impl_Texture2DMethod), tex1d);
    EXPECT_EQ(query(tex1d, winapi::IID_ID3D11TEXTURE2D,
                    &D3D11Manager::impl_Texture2DMethod), 0u);
    EXPECT_NE(query(tex1d, winapi::IID_IDXGISURFACE1,
                    &D3D11Manager::impl_Texture2DMethod), 0u);

    // Texture3D: IDXGIResource / IDXGIResource1, no surface.
    std::array<u32, 9> tex3d_desc{4, 4, 4, 1, 28, 0, 8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(tex3d_desc.data()),
        sizeof(tex3d_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 6);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr tex3d = memory.read_u64(TEXTURE_OUT).value();
    const GuestAddr tex3d_resource = query(tex3d, winapi::IID_IDXGIRESOURCE1,
                                           &D3D11Manager::impl_Texture2DMethod);
    ASSERT_NE(tex3d_resource, 0u);
    EXPECT_EQ(query(tex3d_resource, winapi::IID_ID3D11TEXTURE3D,
                    &D3D11Manager::impl_DxgiResourceMethod), tex3d);
    EXPECT_EQ(query(tex3d, winapi::IID_IDXGISURFACE,
                    &D3D11Manager::impl_Texture2DMethod), 0u);

    // Keyed mutex: QI(IUnknown) of the facet is the texture, and it reaches
    // Texture2D1 and the surfaces.
    const std::array<u32, 11> keyed_desc{16, 16, 1, 1, 87, 1, 0, 0,
                                         0x28, 0, 0x100};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(keyed_desc.data()),
        sizeof(keyed_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr keyed = memory.read_u64(TEXTURE_OUT).value();
    const GuestAddr mutex = query(keyed, winapi::IID_IDXGIKEYEDMUTEX,
                                  &D3D11Manager::impl_Texture2DMethod);
    ASSERT_NE(mutex, 0u);
    EXPECT_EQ(query(mutex, winapi::IID_IUNKNOWN,
                    &D3D11Manager::impl_KeyedMutexMethod), keyed);
    EXPECT_NE(query(mutex, winapi::IID_ID3D11TEXTURE2D1,
                    &D3D11Manager::impl_KeyedMutexMethod), 0u);
    EXPECT_NE(query(mutex, winapi::IID_IDXGISURFACE2,
                    &D3D11Manager::impl_KeyedMutexMethod), 0u);

    // SRV: IDXGISurface (not Surface1, not IDXGIResource) on a facet whose
    // IUnknown is the view; GetParent(IDXGIDevice) is E_NOINTERFACE.
    const std::array<u32, 11> desc{8, 8, 1, 1, 28, 1, 0, 0, 8, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    set_arg(0, device);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr view = memory.read_u64(VIEW_OUT).value();
    const GuestAddr view_surface = query(
        view, winapi::IID_IDXGISURFACE, &D3D11Manager::impl_ShaderResourceViewMethod);
    ASSERT_NE(view_surface, 0u);
    EXPECT_EQ(query(view_surface, winapi::IID_IUNKNOWN,
                    &D3D11Manager::impl_DxgiSurfaceMethod), view);
    EXPECT_EQ(query(view, winapi::IID_IDXGISURFACE1,
                    &D3D11Manager::impl_ShaderResourceViewMethod), 0u);
    EXPECT_EQ(query(view, winapi::IID_IDXGIRESOURCE,
                    &D3D11Manager::impl_ShaderResourceViewMethod), 0u);
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(winapi::IID_IDXGIDEVICE.data()),
        16).is_ok());
    set_arg(0, view_surface);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 6);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_NOINTERFACE);
    set_arg(0, view_surface);
    set_arg(1, SURFACE_DESC);
    d3d11.impl_DxgiSurfaceMethod(marshaler, 8);
    ASSERT_TRUE(memory.read_bytes(SURFACE_DESC,
                                  reinterpret_cast<Byte*>(words.data()),
                                  sizeof(words)).is_ok());
    EXPECT_EQ(words, (std::array<u32, 5>{8, 8, 28, 1, 0}));
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test

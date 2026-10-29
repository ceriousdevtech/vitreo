// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file d3d11_test_sharing.hpp
/// @brief D3D11SharingTest / D3D11ExtendedSharingTest and the sharing HRESULT constants.

#include "d3d11_test_swapchain.hpp"

namespace vitreo::winapi::d3d11_test {

// ---------------------------------------------------------------------------
// Shared resources (d3d11_sharing.cpp). Every expected value is a line of
// tools/oracle_probe/d3d11_share_probe.expected (Windows 11, WARP), named
// next to the assertion. Where this device deliberately answers differently
// (the test backend's default is a backend without extended sharing: a
// shared Texture2D is one image of an ordinary colour format) the assertion
// says so; D3D11ExtendedSharingTest below turns extended sharing on.
// ---------------------------------------------------------------------------

constexpr u32 kShareSFalse = 0x00000001u;
constexpr u32 kShareAccessDenied = 0x80070005u;
constexpr u32 kShareNameExists = 0x887A002Cu;   // DXGI_ERROR_NAME_ALREADY_EXISTS
constexpr u32 kShareNotAvailable = 0x8876086Au; // D3DERR_NOTAVAILABLE
constexpr u32 kShareAbandoned = 0x00000080u;    // WAIT_ABANDONED
constexpr u32 kShareTimeout = 0x00000102u;      // WAIT_TIMEOUT


class D3D11SharingTest : public DxgiSwapChainContractTest {
protected:
    static constexpr GuestAddr NAME = DATA_BASE + 0x1C00;
    static constexpr GuestAddr ATTRIBUTES = DATA_BASE + 0x1D00;

    void SetUp() override {
        DxgiSwapChainContractTest::SetUp();
        // A second device: openings are made there, as the probe's device B.
        create_default_device();
        device_b = memory.read_u64(DEVICE_OUT).value();
        ASSERT_NE(device_b, device);
    }

    u32 dispatch_device(GuestAddr self, usize slot,
                        std::initializer_list<u64> args) {
        set_arg(0, self);
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_DeviceMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 dispatch_resource(GuestAddr texture, usize slot,
                          std::initializer_list<u64> args) {
        set_arg(0, texture + 0x20);  // the IDXGIResource1 facet
        usize index = 1;
        for (const u64 value : args) set_arg(index++, value);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_DxgiResourceMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 sync(GuestAddr texture, usize slot, u64 key, u32 ms) {
        set_arg(0, texture + 0x28);  // the IDXGIKeyedMutex facet
        set_arg(1, key);
        set_arg(2, ms);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_KeyedMutexMethod(marshaler, slot);
        return static_cast<u32>(cpu.gpr.rax);
    }

    /// CreateTexture2D on device A: desc is w h mips array fmt samples
    /// quality usage bind cpu misc.
    GuestAddr make(u32 format, u32 misc, u32 bind = 0x28, u32 mips = 1,
                   u32 array = 1, u32 samples = 1, u32 usage = 0,
                   u32 cpu_access = 0, GuestAddr on = 0) {
        const std::array<u32, 11> desc{16, 8, mips, array, format, samples,
                                       0, usage, bind, cpu_access, misc};
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        EXPECT_TRUE(memory.write_u64(TEXTURE_OUT, 0).is_ok());
        last_hr = dispatch_device(on == 0 ? device : on, 5,
                                  {TEXTURE_DESC, 0, TEXTURE_OUT});
        return memory.read_u64(TEXTURE_OUT).value();
    }

    u32 get_kmt(GuestAddr texture, u64& handle) {
        EXPECT_TRUE(memory.write_u64(OUT, 0x5A5A5A5Au).is_ok());
        const u32 hr = dispatch_resource(texture, 8, {OUT});
        handle = memory.read_u64(OUT).value();
        return hr;
    }
    u32 create_nt(GuestAddr texture, u32 access, u64& handle,
                  GuestAddr name = 0, GuestAddr attributes = 0) {
        EXPECT_TRUE(memory.write_u64(OUT, 0x5A5A5A5Au).is_ok());
        const u32 hr =
            dispatch_resource(texture, 13, {attributes, access, name, OUT});
        handle = memory.read_u64(OUT).value();
        return hr;
    }
    /// OpenSharedResource (slot 28) or OpenSharedResource1 (48) on device B.
    u32 open(usize slot, u64 handle, const GuidBytes& iid, GuestAddr& out,
             bool want_out = true) {
        write_iid(iid);
        EXPECT_TRUE(memory.write_u64(RESOURCE_OUT, 0x7777).is_ok());
        const u32 hr = dispatch_device(device_b, slot,
                                       {handle, IID, want_out ? RESOURCE_OUT : 0});
        out = want_out ? memory.read_u64(RESOURCE_OUT).value() : 0;
        return hr;
    }
    u32 open_by_name(const char16_t* name, u32 access, GuestAddr& out) {
        GuestAddr name_arg = 0;
        if (name != nullptr) {
            std::u16string text(name);
            text.push_back(0);
            EXPECT_TRUE(memory.write_bytes(
                NAME, reinterpret_cast<const Byte*>(text.data()),
                text.size() * sizeof(char16_t)).is_ok());
            name_arg = NAME;
        }
        write_iid(winapi::IID_ID3D11TEXTURE2D);
        EXPECT_TRUE(memory.write_u64(RESOURCE_OUT, 0x7777).is_ok());
        const u32 hr = dispatch_device(device_b, 49,
                                       {name_arg, access, IID, RESOURCE_OUT});
        out = memory.read_u64(RESOURCE_OUT).value();
        return hr;
    }
    GuestAddr write_name(const char16_t* name) {
        std::u16string text(name);
        text.push_back(0);
        EXPECT_TRUE(memory.write_bytes(
            NAME, reinterpret_cast<const Byte*>(text.data()),
            text.size() * sizeof(char16_t)).is_ok());
        return NAME;
    }
    /// ID3D11Texture2D::GetDesc (slot 10).
    std::array<u32, 11> desc_of(GuestAddr texture) {
        set_arg(0, texture);
        set_arg(1, SCRATCH);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_Texture2DMethod(marshaler, 10);
        std::array<u32, 11> desc{};
        EXPECT_TRUE(memory.read_bytes(
            SCRATCH, reinterpret_cast<Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        return desc;
    }
    bool answers(GuestAddr texture, const GuidBytes& iid) {
        write_iid(iid);
        set_arg(0, texture);
        set_arg(1, IID);
        set_arg(2, OUT2);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_Texture2DMethod(marshaler, 0);
        const bool ok = static_cast<u32>(cpu.gpr.rax) == d3d11::S_OK;
        if (ok) release_texture(texture);
        return ok;
    }

    GuestAddr device_b{0};
    u32 last_hr{0};
};

// ---------------------------------------------------------------------------
// ExtendedResourceSharing (the backend shares every shape: the Metal
// backend's canonical stores, metal_shared_resource.hpp). Expected values
// are lines of tools/oracle_probe/d3d11_share_probe.expected,
// d3d11_share_ers_probe.expected and d3d11_nv12_probe.expected.
// ---------------------------------------------------------------------------
class D3D11ExtendedSharingTest : public D3D11SharingTest {
protected:
    void SetUp() override {
        // Before any capability query: the manager reads the backend's
        // sharing capabilities once.
        backend.state.extended_sharing = true;
        D3D11SharingTest::SetUp();
        context_b = memory.read_u64(CONTEXT_OUT).value();
    }
    u32 feature(u32 which, u32 size, u32 word) {
        constexpr GuestAddr DATA = DATA_BASE + 0x1B40;
        for (u32 i = 0; i < size; i += 4) {
            EXPECT_TRUE(memory.write_u32(DATA + i, 0xCCCCCCCCu).is_ok());
        }
        EXPECT_EQ(dispatch_device(device, 33, {which, DATA, size}),
                  d3d11::S_OK);
        return memory.read_u32(DATA + word * 4).value();
    }
    u32 support2(u32 format) {
        constexpr GuestAddr FORMAT2 = DATA_BASE + 0x1B00;
        EXPECT_TRUE(memory.write_u32(FORMAT2, format).is_ok());
        EXPECT_TRUE(memory.write_u32(FORMAT2 + 4, 0).is_ok());
        (void)dispatch_device(device, 33, {3, FORMAT2, 8});
        return memory.read_u32(FORMAT2 + 4).value();
    }
    /// ID3D11DeviceContext::Map (slot 14) on @p on; the mapped pointer and
    /// pitches land in SCRATCH.
    u32 map(GuestAddr on, GuestAddr resource, u32 subresource, u32 type) {
        set_arg(0, on);
        set_arg(1, resource);
        set_arg(2, subresource);
        set_arg(3, type);
        set_arg(4, 0);
        set_arg(5, SCRATCH);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_ContextMethod(marshaler, 14);
        return static_cast<u32>(cpu.gpr.rax);
    }
    void unmap(GuestAddr on, GuestAddr resource, u32 subresource) {
        set_arg(0, on);
        set_arg(1, resource);
        set_arg(2, subresource);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        d3d11.impl_ContextMethod(marshaler, 15);
    }
    GuestAddr make_buffer(u32 bytes, u32 usage, u32 bind, u32 cpu_access,
                          u32 misc) {
        const std::array<u32, 6> desc{bytes, usage, bind, cpu_access, misc,
                                      0};
        EXPECT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        EXPECT_TRUE(memory.write_u64(GPU_BUFFER_OUT, 0).is_ok());
        last_hr =
            dispatch_device(device, 3, {GPU_BUFFER_DESC, 0, GPU_BUFFER_OUT});
        return memory.read_u64(GPU_BUFFER_OUT).value();
    }
    GuestAddr context_b{0};
};

}  // namespace vitreo::winapi::d3d11_test

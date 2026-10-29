// SPDX-License-Identifier: LGPL-2.1-only
//
// Array subresources, initial data, resolves, shared textures, keyed mutex and NT handles.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, MapsTextureArraySubresourcesIndependently) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> desc{
        4, 4, 2, 2, 87, 1, 0, 3, 0, 0x30000, 0,
    };
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

    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, 3); // D3D11_MAP_READ_WRITE
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped0 = memory.read_u64(MAPPED_OUT).value();
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), 16u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(), 64u);

    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 3); // mip 1, array slice 1
    set_arg(3, 3);
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped3 = memory.read_u64(MAPPED_OUT).value();
    EXPECT_NE(mapped0, mapped3);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), 8u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(), 16u);
    ASSERT_TRUE(memory.write_u32(mapped3, 0xA1B2C3D4u).is_ok());

    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 3);
    d3d11.impl_ContextMethod(marshaler, 15);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 3);
    set_arg(3, 1); // D3D11_MAP_READ
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr remapped3 = memory.read_u64(MAPPED_OUT).value();
    EXPECT_EQ(memory.read_u32(remapped3).value(), 0xA1B2C3D4u);
    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 3);
    d3d11.impl_ContextMethod(marshaler, 15);
}

TEST_F(D3D11ManagerTest, CreateTexture2DConsumesEveryInitialSubresource) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> desc{
        4, 4, 2, 2, 87, 1, 0, 0, 0x8, 0, 0,
    };
    constexpr GuestAddr records = DATA_BASE + 0x900;
    constexpr GuestAddr pixels = DATA_BASE + 0xA00;
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    for (u32 index = 0; index < 4; ++index) {
        const u32 dimension = 4u >> (index % 2);
        const u32 pitch = dimension * 4u + 4u;
        std::vector<u8> bytes(
            static_cast<usize>(pitch) * dimension, 0);
        bytes[0] = static_cast<u8>(0x20u + index);
        const GuestAddr data = pixels + index * 0x80u;
        ASSERT_TRUE(memory.write_bytes(
            data, reinterpret_cast<const Byte*>(bytes.data()),
            bytes.size()).is_ok());
        ASSERT_TRUE(memory.write_u64(records + index * 16u, data).is_ok());
        ASSERT_TRUE(memory.write_u32(
            records + index * 16u + 8u, pitch).is_ok());
        ASSERT_TRUE(memory.write_u32(
            records + index * 16u + 12u, 0).is_ok());
    }

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, records);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.texture_subresource_updates,
              (std::vector<std::array<unsigned, 2>>{
                  {0, 0}, {1, 0}, {0, 1}, {1, 1}}));
    EXPECT_EQ(backend.state.texture_subresource_first_bytes,
              (std::vector<u8>{0x20, 0x21, 0x22, 0x23}));
}

TEST_F(D3D11ManagerTest, UpdateSubresourceUploadsBoxedTextureRegion) {
    // D3D11_BOX coordinates are destination texels. pSrcData begins at the
    // box's top-left and SrcRowPitch may include padding after each row.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> desc{
        4, 4, 1, 1, 87, 1, 0, 0, 0x8, 0, 0,
    };
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

    constexpr GuestAddr box_address = DATA_BASE + 0xA00;
    constexpr GuestAddr pixels_address = DATA_BASE + 0xA40;
    const std::array<u32, 6> box{1, 1, 0, 3, 3, 1};
    const std::array<u8, 24> pixels{
        1, 2, 3, 4, 5, 6, 7, 8, 0xEE, 0xEE, 0xEE, 0xEE,
        9, 10, 11, 12, 13, 14, 15, 16, 0xDD, 0xDD, 0xDD, 0xDD,
    };
    ASSERT_TRUE(memory.write_bytes(
        box_address, reinterpret_cast<const Byte*>(box.data()),
        sizeof(box)).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        pixels_address, reinterpret_cast<const Byte*>(pixels.data()),
        pixels.size()).is_ok());

    set_arg(0, context);
    set_arg(1, texture);
    set_arg(2, 0);
    set_arg(3, box_address);
    set_arg(4, pixels_address);
    set_arg(5, 12);
    set_arg(6, 24);
    d3d11.impl_ContextMethod(marshaler, 48);

    EXPECT_EQ(backend.state.texture_updates, 1u);
    EXPECT_EQ(backend.state.last_texture_update_region,
              (std::array<unsigned, 6>{0, 0, 1, 1, 2, 2}));
    EXPECT_EQ(backend.state.last_texture_update_row_pitch, 12u);
    // Learn (UpdateSubresource pitch diagram): pSrcData holds the box and
    // nothing after its last texel, so the last row's padding (0xDD here)
    // is never read -- reading a whole pitch there ran past the caller's
    // buffer (a06 probe U17 on x86 showed foreign bytes).
    std::vector<u8> expected(pixels.begin(), pixels.end());
    std::fill(expected.begin() + 20, expected.end(), u8{0});
    EXPECT_EQ(backend.state.last_texture_update, expected);
}

TEST_F(D3D11ManagerTest, CreateTexture2DAcceptsGenerateMipsMisc) {
    // Learn: GENERATE_MIPS requires BIND_RENDER_TARGET|BIND_SHADER_RESOURCE.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        64, 64, 1, 1, 87, 1, 0, 0, 0x28, 0, 0x1,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_NE(memory.read_u64(TEXTURE_OUT).value(), 0u);
    EXPECT_EQ(backend.state.texture_creations, 1u);
}

TEST_F(D3D11ManagerTest, ResolvesMultisampledRenderTarget) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> msaa_desc{
        64, 32, 1, 1, 87, 4, 0, 0, 0x20, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(msaa_desc.data()),
        sizeof(msaa_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source = memory.read_u64(TEXTURE_OUT).value();
    EXPECT_EQ(backend.state.last_texture_desc.sample_count, 4u);

    const std::array<u32, 11> resolve_desc{
        64, 32, 1, 1, 87, 1, 0, 0, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(resolve_desc.data()),
        sizeof(resolve_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr destination = memory.read_u64(VIEW_OUT).value();

    set_arg(0, context);
    set_arg(1, destination);
    set_arg(2, 0);
    set_arg(3, source);
    set_arg(4, 0);
    set_arg(5, 87);
    d3d11.impl_ContextMethod(marshaler, 57);
    EXPECT_EQ(backend.state.texture_resolves, 1u);
}

TEST_F(D3D11ManagerTest, ResolvesNonzeroMultisampledTextureArraySlice) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> msaa_desc{
        64, 32, 1, 3, 87, 4, 0, 0, 0x20, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(msaa_desc.data()),
        sizeof(msaa_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source = memory.read_u64(TEXTURE_OUT).value();

    const std::array<u32, 11> resolve_desc{
        64, 32, 1, 3, 87, 1, 0, 0, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(resolve_desc.data()),
        sizeof(resolve_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr destination = memory.read_u64(VIEW_OUT).value();

    set_arg(0, context);
    set_arg(1, destination);
    set_arg(2, 1);
    set_arg(3, source);
    set_arg(4, 2);
    set_arg(5, 87);
    d3d11.impl_ContextMethod(marshaler, 57);

    EXPECT_EQ(backend.state.texture_resolves, 1u);
    EXPECT_EQ(backend.state.last_copy_dst_mip, 0u);
    EXPECT_EQ(backend.state.last_copy_dst_slice, 1u);
    EXPECT_EQ(backend.state.last_copy_src_mip, 0u);
    EXPECT_EQ(backend.state.last_copy_src_slice, 2u);
}

TEST_F(D3D11ManagerTest, ResolvesCompatibleTypelessTextureFormatFamily) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // R8G8B8A8_TYPELESS source and destination, resolved through the typed
    // R8G8B8A8_UNORM_SRGB family member.
    const std::array<u32, 11> source_desc{
        16, 16, 1, 1, 27, 4, 0, 0, 0x20, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(source_desc.data()),
        sizeof(source_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr source = memory.read_u64(TEXTURE_OUT).value();
    EXPECT_TRUE(backend.state.last_texture_desc.pixel_format_view);

    auto destination_desc = source_desc;
    destination_desc[5] = 1;
    destination_desc[8] = 0;
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(destination_desc.data()),
        sizeof(destination_desc)).is_ok());
    set_arg(1, TEXTURE_DESC);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr destination = memory.read_u64(VIEW_OUT).value();
    EXPECT_TRUE(backend.state.last_texture_desc.pixel_format_view);

    set_arg(0, context);
    set_arg(1, destination);
    set_arg(2, 0);
    set_arg(3, source);
    set_arg(4, 0);
    set_arg(5, 29);  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
    d3d11.impl_ContextMethod(marshaler, 57);

    EXPECT_EQ(backend.state.texture_resolves, 1u);
    EXPECT_EQ(backend.state.last_resolve_format,
              graphics::PixelFormat::RGBA8Unorm_sRGB);
}

TEST_F(D3D11ManagerTest, CreateTexture2DAcceptsSharedSingleMipTexture) {
    // Learn: D3D11_RESOURCE_MISC_SHARED supports non-mipmapped 2-D textures.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        1, 1, 1, 1, 87, 1, 0, 0, 0x28, 0, 0x2,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_NE(memory.read_u64(TEXTURE_OUT).value(), 0u);
    EXPECT_EQ(backend.state.texture_creations, 1u);
}

TEST_F(D3D11ManagerTest, SharedKeyedMutexQueryInterfaceAndAcquireRelease) {
    // Learn: D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX is 0x100 and mutually
    // exclusive with SHARED. QI(IDXGIKeyedMutex) then AcquireSync(0) /
    // ReleaseSync succeeds on that resource.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        8, 8, 1, 1, 87, 1, 0, 0, 0x28, 0, 0x100,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIKEYEDMUTEX),
        sizeof(IID_IDXGIKEYEDMUTEX)).is_ok());
    set_arg(0, texture);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_Texture2DMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mutex = memory.read_u64(INTERFACE_OUT).value();
    ASSERT_NE(mutex, 0u);
    EXPECT_NE(mutex, texture);

    set_arg(0, mutex);
    set_arg(1, 0);
    set_arg(2, 0xFFFFFFFFu);
    d3d11.impl_KeyedMutexMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    // A recursive AcquireSync is DXGI_ERROR_INVALID_CALL, not E_FAIL
    // (dxgi_a11_contracts_probe64.expected "AcquireSync(0, 0) recursive").
    d3d11.impl_KeyedMutexMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, 0x887A0001u);

    set_arg(1, 1);
    d3d11.impl_KeyedMutexMethod(marshaler, 9);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    const std::array<u32, 11> both{
        8, 8, 1, 1, 87, 1, 0, 0, 0x28, 0, 0x102,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(both.data()),
        sizeof(both)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::E_INVALIDARG);
}

TEST_F(D3D11ManagerTest, SharedNtHandleCreateAndOpenSharedResource1) {
    // Learn: SHARED_KEYEDMUTEX|SHARED_NTHANDLE (0x900) uses
    // IDXGIResource1::CreateSharedHandle + ID3D11Device1::OpenSharedResource1.
    // GetSharedHandle must fail for NT-handle resources.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        16, 16, 1, 1, 87, 1, 0, 0, 0x28, 0, 0x900,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);

    // IDXGIResource1 from dxgi1_2.h. This test used to carry the same wrong
    // Data4 as the frontend constant (BFAD-0D243C75B987), so both agreed on
    // an IID no guest ever passes.
    constexpr Guid IID_RESOURCE1{
        0x30961379u, 0x4609u, 0x4a41u,
        {0x99, 0x8E, 0x54, 0xFE, 0x56, 0x7E, 0xE0, 0xC1}};
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_RESOURCE1),
        sizeof(IID_RESOURCE1)).is_ok());
    set_arg(0, texture);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_Texture2DMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr resource1 = memory.read_u64(INTERFACE_OUT).value();
    ASSERT_NE(resource1, 0u);

    constexpr GuestAddr HANDLE_OUT = DATA_BASE + 0x2A0;
    // Measured (dxgi_a11_contracts_probe64.expected
    // "GetSharedHandle(SHARED|NTHANDLE)"): E_INVALIDARG, handle untouched.
    ASSERT_TRUE(memory.write_u64(HANDLE_OUT, 0x5A5A5A5Au).is_ok());
    set_arg(0, resource1);
    set_arg(1, HANDLE_OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 8); // GetSharedHandle
    EXPECT_EQ(cpu.gpr.rax & 0xffffffffu, d3d11::E_INVALIDARG);
    EXPECT_EQ(memory.read_u64(HANDLE_OUT).value(), 0x5A5A5A5Au);

    set_arg(0, resource1);
    set_arg(1, 0); // pAttributes
    set_arg(2, 0x80000001u); // READ|WRITE
    set_arg(3, 0); // lpName
    set_arg(4, HANDLE_OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 13);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const u64 handle = memory.read_u64(HANDLE_OUT).value();
    ASSERT_NE(handle, 0u);

    // A second CreateSharedHandle succeeds too (measured: "CreateSharedHandle
    // second call hr=0x00000000"); this used to expect INVALID_CALL. Each
    // call makes another NT handle -- a kernel handle of its own, which the
    // old implementation (the storage id as the "handle") could not be.
    d3d11.impl_DxgiResourceMethod(marshaler, 13);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(HANDLE_OUT).value(), 0u);
    EXPECT_NE(memory.read_u64(HANDLE_OUT).value(), handle);
    EXPECT_EQ(handles.get_type(static_cast<Handle>(handle)),
              HandleType::SharedResource);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11TEXTURE2D),
        sizeof(IID_ID3D11TEXTURE2D)).is_ok());
    set_arg(0, device);
    set_arg(1, handle);
    set_arg(2, IID_ADDRESS);
    set_arg(3, RESOURCE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 48); // OpenSharedResource1
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(RESOURCE_OUT).value(), 0u);
}

TEST_F(D3D11ManagerTest, CreateTexture2DPreservesAlphaOnlyFormat) {
    // DXGI_FORMAT_A8_UNORM stores a normalized alpha component, not red.
    // The backend must retain that channel identity for glyph atlas sampling.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        64, 64, 1, 1, 65, 1, 0, 0, 0x8, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);

    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_NE(memory.read_u64(TEXTURE_OUT).value(), 0u);
    ASSERT_EQ(backend.state.texture_creations, 1u);
    EXPECT_EQ(backend.state.last_texture_desc.pixel_format,
              graphics::PixelFormat::A8Unorm);
    EXPECT_FALSE(backend.state.last_texture_desc.cpu_writable);
    const auto format = graphics::format_table(
        backend.state.last_texture_desc.pixel_format);
    EXPECT_EQ(format.mtl_pixel_format, 1u);
    EXPECT_EQ(format.bytes_per_pixel, 1u);
}

TEST_F(D3D11ManagerTest, CreateTexture2DKeepsDefaultBc7GpuOwned) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        256, 256, 9, 1, 98, 1, 0, 0, 0x8, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);

    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_NE(memory.read_u64(TEXTURE_OUT).value(), 0u);
    EXPECT_EQ(backend.state.last_texture_desc.pixel_format,
              graphics::PixelFormat::BC7_RGBAUnorm);
    EXPECT_FALSE(backend.state.last_texture_desc.cpu_writable);
}

TEST_F(D3D11ManagerTest, CreateTexture2DAcceptsZeroMipLevelsAsFullChain) {
    // Microsoft Learn: MipLevels 0 generates a full set of subtextures.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    // 16x16 → 5 mip levels when MipLevels=0.
    const std::array<u32, 11> desc{
        16, 16, 0, 1, 28, 1, 0, 0, 0x28, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);
    EXPECT_EQ(backend.state.texture_creations, 1u);
    EXPECT_EQ(backend.state.last_texture_desc.mip_levels, 5u);
}

TEST_F(D3D11ManagerTest, TexturePublishesDxgiResourceContract) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        64, 64, 1, 1, 87, 1, 0, 0, 0x28, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDXGIRESOURCE),
        sizeof(IID_IDXGIRESOURCE)).is_ok());
    set_arg(0, texture);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_Texture2DMethod(marshaler, 0);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr resource = memory.read_u64(INTERFACE_OUT).value();
    ASSERT_NE(resource, 0u);
    EXPECT_NE(resource, texture);
    const GuestAddr resource_vtable = memory.read_u64(resource).value();
    EXPECT_NE(memory.read_u64(resource_vtable + 11 * 8).value(), 0u);

    set_arg(0, resource);
    set_arg(1, COLOR);
    d3d11.impl_DxgiResourceMethod(marshaler, 9);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u32(COLOR).value(), 0x30u);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_ID3D11DEVICE),
        sizeof(IID_ID3D11DEVICE)).is_ok());
    set_arg(0, resource);
    set_arg(1, IID_ADDRESS);
    set_arg(2, ADAPTER_OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 7);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(ADAPTER_OUT).value(), device);

    ASSERT_TRUE(memory.write_u64(ADAPTER_OUT, 0xFFFFFFFFFFFFFFFFull).is_ok());
    set_arg(0, resource);
    set_arg(1, ADAPTER_OUT);
    d3d11.impl_DxgiResourceMethod(marshaler, 8);
    // GetSharedHandle on an unshared resource is S_OK with a NULL handle
    // (dxgi_a11_contracts_probe64.expected "GetSharedHandle(non-shared)");
    // this used to expect INVALID_CALL.
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(ADAPTER_OUT).value(), 0u);
}

TEST_F(D3D11ManagerTest, CreatesShaderResourceViewForTexture) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const std::array<u32, 11> desc{
        16, 16, 1, 1, 28, 1, 0, 0, 0x8, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
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
    ASSERT_NE(view, 0u);

    set_arg(0, view);
    set_arg(1, SRV_DESC);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 8);
    EXPECT_EQ(memory.read_u32(SRV_DESC).value(), 28u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 4).value(), 4u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 8).value(), 0u);
    EXPECT_EQ(memory.read_u32(SRV_DESC + 12).value(), 1u);

    set_arg(0, view);
    set_arg(1, RESOURCE_OUT);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 7);
    EXPECT_EQ(memory.read_u64(RESOURCE_OUT).value(), texture);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_ID3D11SHADERRESOURCEVIEW),
        sizeof(IID_ID3D11SHADERRESOURCEVIEW)).is_ok());
    set_arg(0, view);
    set_arg(1, IID_ADDRESS);
    set_arg(2, INTERFACE_OUT);
    d3d11.impl_ShaderResourceViewMethod(marshaler, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u64(INTERFACE_OUT).value(), view);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test

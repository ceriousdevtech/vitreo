// SPDX-License-Identifier: LGPL-2.1-only
//
// Texture UAVs, volume copies and the buffer SRV/UAV kinds.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, CreatesTextureUavOverATypelessResource) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr UAV_DESC = TEXTURE_DESC + 0x80;
    constexpr GuestAddr UAV_OUT = VIEW_OUT;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_TEXTURE2D_DESC: 64x64, DXGI_FORMAT_B8G8R8A8_TYPELESS (90),
    // DEFAULT, SHADER_RESOURCE | RENDER_TARGET | UNORDERED_ACCESS, 1 mip.
    const std::array<u32, 11> tex{64, 64, 1, 1, 90, 1, 0, 0,
                                  0x8 | 0x20 | 0x80, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(tex.data()),
        sizeof(tex)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr texture = memory.read_u64(TEXTURE_OUT).value();
    ASSERT_NE(texture, 0u);

    // D3D11_UNORDERED_ACCESS_VIEW_DESC + D3D11_TEX2D_UAV:
    // Format, ViewDimension(TEXTURE2D=4), MipSlice.
    const auto create_uav = [&](u32 format) {
        const std::array<u32, 3> uav{format, 4, 0};
        EXPECT_TRUE(memory.write_bytes(
            UAV_DESC, reinterpret_cast<const Byte*>(uav.data()),
            sizeof(uav)).is_ok());
        EXPECT_TRUE(memory.write_u64(UAV_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, texture);
        set_arg(2, UAV_DESC);
        set_arg(3, UAV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 8);
    };

    // Learn D3D11_UNORDERED_ACCESS_VIEW_DESC: the view of a typeless resource
    // supplies the interpretation, so a typed member of the resource's DXGI
    // family is legal — B8G8R8A8_TYPELESS (90) is viewable as
    // B8G8R8A8_UNORM (87). Not as B8G8R8A8_UNORM_SRGB (91): no sRGB format
    // is a typed UAV format, and the typeless format itself is no view format
    // at all (d3d11_a03_views_probe "uav TYPELESS res as UNORM_SRGB | as
    // TYPELESS"; both expected S_OK here before the oracle measured them).
    create_uav(87);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "typed view of a typeless resource";
    EXPECT_NE(memory.read_u64(UAV_OUT).value(), 0u);
    create_uav(91);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "sRGB UAV";
    create_uav(90);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "the resource format itself";

    // A format from another family is still refused.
    create_uav(28);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "R8G8B8A8_UNORM on a BGRA "
                                                   "typeless resource";
}

TEST_F(D3D11ManagerTest, CreatesBlockCompressedVolumeTextureFromInitialData) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr TEX3D_DESC = TEXTURE_DESC;
    constexpr GuestAddr TEX3D_OUT = TEXTURE_OUT;
    constexpr GuestAddr SUBRESOURCE = DATA_BASE + 0x1000;
    constexpr GuestAddr BLOCKS = DATA_BASE + 0x1100;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // Learn D3D11 block compression: Direct3D 11 supports BC volume textures,
    // which Direct3D 10 did not. A BC1 level is a stack of 4x4 block planes:
    // RowPitch spans one row of blocks (8 bytes each) and DepthPitch one W
    // slice of them, so an 8x8x4 level is 2x2 blocks per slice.
    constexpr u32 BLOCK_BYTES = 8;
    constexpr u32 ROW_PITCH = 2 * BLOCK_BYTES;
    constexpr u32 SLICE_PITCH = ROW_PITCH * 2;
    const std::vector<Byte> blocks(SLICE_PITCH * 4, Byte{0xAB});
    ASSERT_TRUE(memory.write_bytes(
        BLOCKS, blocks.data(), blocks.size()).is_ok());

    // 8x8x4, DXGI_FORMAT_BC1_UNORM (71), DEFAULT, SHADER_RESOURCE, 1 mip.
    const auto create = [&](u32 format, u32 bind, u32 row_pitch,
                            u32 slice_pitch) {
        const std::array<u32, 9> tex{8, 8, 4, 1, format, 0, bind, 0, 0};
        EXPECT_TRUE(memory.write_bytes(
            TEX3D_DESC, reinterpret_cast<const Byte*>(tex.data()),
            sizeof(tex)).is_ok());
        EXPECT_TRUE(memory.write_u64(SUBRESOURCE, BLOCKS).is_ok());
        EXPECT_TRUE(memory.write_u32(SUBRESOURCE + 8, row_pitch).is_ok());
        EXPECT_TRUE(memory.write_u32(SUBRESOURCE + 12, slice_pitch).is_ok());
        EXPECT_TRUE(memory.write_u64(TEX3D_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, TEX3D_DESC);
        set_arg(2, SUBRESOURCE);
        set_arg(3, TEX3D_OUT);
        d3d11.impl_DeviceMethod(marshaler, 6);
    };

    create(71, 0x8, ROW_PITCH, SLICE_PITCH);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "BC1 volume with initial data";
    EXPECT_NE(memory.read_u64(TEX3D_OUT).value(), 0u);
    EXPECT_EQ(backend.state.last_texture_3d_desc.width, 8u);
    EXPECT_EQ(backend.state.last_texture_3d_desc.depth, 4u);

    // DXGI_FORMAT_BC1_TYPELESS (70) is a legal creation format as well: the
    // typed view supplies the interpretation.
    create(70, 0x8, ROW_PITCH, SLICE_PITCH);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "typeless BC1 volume";

    // Corrected by Windows (2026-09-28, a pitch probe): a non-zero pitch
    // smaller than a row, or a slice pitch smaller than a slice, is honoured
    // -- the rows / slices overlap -- for block-compressed volumes too
    // ("BC1 8x8x2 pitch8 (<row 16) slice32" and "pitch16 slice16" S_OK).
    // Only a zero pitch is refused.
    create(71, 0x8, BLOCK_BYTES, SLICE_PITCH);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "row pitch below one row";
    create(71, 0x8, ROW_PITCH, ROW_PITCH);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "depth pitch below a slice";
    create(71, 0x8, 0, SLICE_PITCH);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "zero row pitch";
    create(71, 0x8, ROW_PITCH, 0);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "zero depth pitch";

    // BC formats are not renderable, so a compressed volume cannot be bound
    // as a render target.
    create(71, 0x8 | 0x20, ROW_PITCH, SLICE_PITCH);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "compressed render target";
}

// Learn D3D11_USAGE: a staging resource is how the CPU reads a GPU resource
// back, through CopyResource; CopyResource requires two resources of the same
// type and dimensions. A staging Texture3D was refused at creation and a
// volume CopyResource declined as an "unsupported resource pair", so a
// compute-filled volume (UE's distance fields, LUTs) could never be read
// back -- the replay audit's 3D UAV outputs all read as zero.
TEST_F(D3D11ManagerTest, CopiesVolumeTexturesAndReadsThemBackThroughStaging) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // D3D11_TEXTURE3D_DESC: Width, Height, Depth, MipLevels, Format, Usage,
    // BindFlags, CPUAccessFlags, MiscFlags.
    const auto create_texture_3d = [&](const std::array<u32, 9>& desc) {
        EXPECT_TRUE(memory.write_bytes(
            TEXTURE_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        EXPECT_TRUE(memory.write_u64(TEXTURE_OUT, 0).is_ok());
        set_arg(0, device);
        set_arg(1, TEXTURE_DESC);
        set_arg(2, 0);
        set_arg(3, TEXTURE_OUT);
        d3d11.impl_DeviceMethod(marshaler, 6);
        return memory.read_u64(TEXTURE_OUT).value();
    };
    constexpr u32 kR32Uint = 42;
    constexpr u32 kStaging = 3, kDynamic = 2;
    constexpr u32 kCpuRead = 0x20000, kCpuWrite = 0x10000;

    // Refusals Learn spells out: staging cannot carry bind flags or lack CPU
    // access; dynamic is write-only, input-only and single-subresource.
    EXPECT_EQ(create_texture_3d({4, 4, 2, 1, kR32Uint, kStaging, 0x8, kCpuRead, 0}), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(create_texture_3d({4, 4, 2, 1, kR32Uint, kStaging, 0, 0, 0}), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(create_texture_3d({4, 4, 2, 2, kR32Uint, kDynamic, 0x8, kCpuWrite, 0}), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_EQ(create_texture_3d({4, 4, 2, 1, kR32Uint, kDynamic, 0x8, kCpuRead, 0}), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    EXPECT_NE(create_texture_3d({4, 4, 2, 1, kR32Uint, kDynamic, 0x8, kCpuWrite, 0}), 0u);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    const GuestAddr gpu_a = create_texture_3d({4, 4, 2, 2, kR32Uint, 0, 0x8 | 0x80, 0, 0});
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr gpu_b = create_texture_3d({4, 4, 2, 2, kR32Uint, 0, 0x8, 0, 0});
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr staging = create_texture_3d({4, 4, 2, 2, kR32Uint, kStaging, 0, kCpuRead | kCpuWrite, 0});
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    ASSERT_NE(staging, 0u);

    // GPU -> GPU: one recorded backend copy of the whole volume.
    backend.state.texture_3d_copies = 0;
    set_arg(0, context);
    set_arg(1, gpu_b);
    set_arg(2, gpu_a);
    d3d11.impl_ContextMethod(marshaler, 47);  // CopyResource
    EXPECT_EQ(backend.state.texture_3d_copies, 1u);

    // GPU -> staging, then Map(READ) of mip 1 (2x2x1): RowPitch is one row,
    // DepthPitch one W slice, and the texels are the backend's per-slice
    // pattern, so a copy that skipped a slice or mip would show.
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, gpu_a);
    d3d11.impl_ContextMethod(marshaler, 47);
    for (u32 mip = 0; mip < 2; ++mip) {
        ASSERT_TRUE(memory.write_u64(MAPPED_OUT, 0).is_ok());
        set_arg(0, context);
        set_arg(1, staging);
        set_arg(2, mip);
        set_arg(3, 1);  // D3D11_MAP_READ
        set_arg(4, 0);
        set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);  // Map
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK) << "mip " << mip;
        const GuestAddr data = memory.read_u64(MAPPED_OUT).value();
        ASSERT_NE(data, 0u);
        const u32 side = 4u >> mip;
        const u32 depth = std::max(1u, 2u >> mip);
        const u32 row_pitch = memory.read_u32(MAPPED_OUT + 8).value();
        const u32 depth_pitch = memory.read_u32(MAPPED_OUT + 12).value();
        EXPECT_EQ(row_pitch, side * 4u);
        EXPECT_EQ(depth_pitch, side * side * 4u);
        for (u32 w = 0; w < depth; ++w) {
            for (u32 y = 0; y < side; ++y) {
                for (u32 x = 0; x < side; ++x) {
                    const auto texel = memory.read_u32(
                        data + w * depth_pitch + y * row_pitch + x * 4);
                    ASSERT_TRUE(texel.is_ok());
                    EXPECT_EQ(texel.value(), mip * 1000u + w * 100u + y * 10u + x)
                        << "mip " << mip << " w " << w << " y " << y << " x " << x;
                }
            }
        }
        set_arg(0, context);
        set_arg(1, staging);
        set_arg(2, mip);
        d3d11.impl_ContextMethod(marshaler, 15);  // Unmap
    }

}

TEST_F(D3D11ManagerTest, CreatesTexture3DRenderTargetViewOverWSlices) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr TEX3D_DESC = TEXTURE_DESC;
    constexpr GuestAddr TEX3D_OUT = TEXTURE_OUT;
    constexpr GuestAddr RTV3D_DESC = TEXTURE_DESC + 0x80;
    constexpr GuestAddr RTV3D_OUT = VIEW_OUT;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // 8x8x4, R8G8B8A8_UNORM, DEFAULT, SHADER_RESOURCE | RENDER_TARGET, 3 mips.
    const std::array<u32, 9> tex{8, 8, 4, 3, 28, 0, 0x8 | 0x20, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEX3D_DESC, reinterpret_cast<const Byte*>(tex.data()),
        sizeof(tex)).is_ok());
    set_arg(0, device);
    set_arg(1, TEX3D_DESC);
    set_arg(2, 0);
    set_arg(3, TEX3D_OUT);
    d3d11.impl_DeviceMethod(marshaler, 6);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr volume = memory.read_u64(TEX3D_OUT).value();
    ASSERT_NE(volume, 0u);

    // D3D11_RENDER_TARGET_VIEW_DESC + D3D11_TEX3D_RTV:
    // Format, ViewDimension(TEXTURE3D=8), MipSlice, FirstWSlice, WSize.
    const auto create_rtv = [&](const std::array<u32, 5>& rtv) {
        EXPECT_TRUE(memory.write_bytes(
            RTV3D_DESC, reinterpret_cast<const Byte*>(rtv.data()),
            sizeof(rtv)).is_ok());
        set_arg(0, device);
        set_arg(1, volume);
        set_arg(2, RTV3D_DESC);
        set_arg(3, RTV3D_OUT);
        d3d11.impl_DeviceMethod(marshaler, 9);
    };

    create_rtv({28, 8, 0, 1, 2});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(RTV3D_OUT).value(), 0u);

    // Learn D3D11_TEX3D_RTV: WSize counts W slices of the *selected mip*, and
    // a volume's depth halves per level like every other dimension. Mip 1 of a
    // depth-4 volume has two slices, so slice 2 is out of range there but not
    // at mip 0.
    create_rtv({28, 8, 0, 2, 2});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "mip 0 has four W slices";
    create_rtv({28, 8, 1, 2, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "mip 1 has two W slices";
    create_rtv({28, 8, 1, 0, 2});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    create_rtv({28, 8, 0, 0, 5});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "window past the depth";
    create_rtv({28, 8, 0, 0, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "empty window";
    // WSize of -1 selects every remaining slice.
    create_rtv({28, 8, 0, 1, 0xFFFFFFFFu});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    // A volume resource cannot be viewed through a 2D dimension, and a 2D
    // resource cannot be viewed as a volume.
    create_rtv({28, 4, 0, 0, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "TEXTURE2D on a volume";

    // D3D11_TEXTURE2D_DESC: Width, Height, MipLevels, ArraySize, Format,
    // SampleDesc.Count, SampleDesc.Quality, Usage, BindFlags, CPUAccessFlags,
    // MiscFlags — a different shape from the 3D desc above.
    const std::array<u32, 11> flat{8, 8, 1, 1, 28, 1, 0, 0, 0x20, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        TEX3D_DESC, reinterpret_cast<const Byte*>(flat.data()),
        sizeof(flat)).is_ok());
    set_arg(0, device);
    set_arg(1, TEX3D_DESC);
    set_arg(2, 0);
    set_arg(3, TEXTURE_OUT + 0x10);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr flat_texture = memory.read_u64(TEXTURE_OUT + 0x10).value();
    ASSERT_NE(flat_texture, 0u);
    EXPECT_TRUE(memory.write_bytes(
        RTV3D_DESC,
        reinterpret_cast<const Byte*>(
            std::array<u32, 5>{28, 8, 0, 0, 1}.data()), 20).is_ok());
    set_arg(0, device);
    set_arg(1, flat_texture);
    set_arg(2, RTV3D_DESC);
    set_arg(3, RTV3D_OUT);
    d3d11.impl_DeviceMethod(marshaler, 9);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "TEXTURE3D on a 2D texture";
}

// CreateBuffer's storage is a pure backend allocation. It is made with the
// device lock released (DxgiManager::ApiUnlock), so another thread's draws do
// not queue behind it; the buffer is then recorded under the lock as before.
// The lock is taken here exactly as the guest-entry thunk takes it.
TEST_F(D3D11ManagerTest, CreateBufferAllocatesItsStorageWithTheDeviceLockReleased) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // ByteWidth 256, DEFAULT, bind CONSTANT_BUFFER, no CPU access, no misc.
    const std::array<u32, 6> desc{256, 0, 0x4, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    std::optional<bool> lock_free_during_allocation;
    backend.state.on_create_buffer = [&] {
        bool acquired = false;
        std::thread probe([&] {
            acquired = d3d11.api_mutex().try_lock();
            if (acquired) d3d11.api_mutex().unlock();
        });
        probe.join();
        lock_free_during_allocation = acquired;
    };
    {
        std::scoped_lock lock(d3d11.api_mutex());
        ++d3d11.api_lock_depth();
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        --d3d11.api_lock_depth();
    }
    backend.state.on_create_buffer = nullptr;
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(GPU_BUFFER_OUT).value(), 0u);
    ASSERT_TRUE(lock_free_during_allocation.has_value());
    EXPECT_TRUE(*lock_free_during_allocation)
        << "the storage was allocated with the device lock held";
    EXPECT_EQ(backend.state.last_buffer_desc.byte_width, 256u);
    EXPECT_TRUE(backend.state.last_buffer_desc.constant_buffer);
}

TEST_F(D3D11ManagerTest, ClearsAStructuredBufferUnorderedAccessView) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr UAV_DESC = GPU_BUFFER_DESC + 0x40;
    constexpr GuestAddr UAV_OUT = GPU_BUFFER_OUT + 0x18;
    constexpr GuestAddr CLEAR_VALUES = GPU_BUFFER_DESC + 0x80;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // ByteWidth 32, DEFAULT, bind UAV, no CPU access, MISC_STRUCTURED,
    // stride 4 — eight 32-bit elements.
    const std::array<u32, 6> desc{32, 0, 0x80, 0, 0x40, 4};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(buffer, 0u);

    // A view over elements [2, 6): the clear must not touch the rest.
    const std::array<u32, 5> uav_desc{0, 1, 2, 4, 0};
    ASSERT_TRUE(memory.write_bytes(
        UAV_DESC, reinterpret_cast<const Byte*>(uav_desc.data()),
        sizeof(uav_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, buffer);
    set_arg(2, UAV_DESC);
    set_arg(3, UAV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr uav = memory.read_u64(UAV_OUT).value();
    ASSERT_NE(uav, 0u);

    // Learn (ClearUnorderedAccessViewUint): a structured buffer is written as
    // raw 32-bit words, so Values[0] is replicated across the view window.
    const std::array<u32, 4> values{0xABCD1234u, 0u, 0u, 0u};
    ASSERT_TRUE(memory.write_bytes(
        CLEAR_VALUES, reinterpret_cast<const Byte*>(values.data()),
        sizeof(values)).is_ok());
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    set_arg(0, context);
    set_arg(1, uav);
    set_arg(2, CLEAR_VALUES);
    d3d11.impl_ContextMethod(marshaler, 51);  // ClearUnorderedAccessViewUint

    // The clear is a device-context command: it lands on the GPU timeline as
    // a 16-byte copy into elements 2..5 from a 16-byte pattern buffer, never
    // as a CPU write into the buffer's live storage (which would overtake the
    // dispatches queued before it -- Satisfactory's FFT bloom read a counter
    // "cleared" before the reduction that filled it).
    EXPECT_TRUE(backend.state.last_buffer_update.empty())
        << "the clear must not write the destination from the CPU";
    ASSERT_EQ(backend.state.buffer_region_copies.size(), 1u);
    const auto& copy = backend.state.buffer_region_copies.front();
    EXPECT_EQ(copy.dst_offset, 8u);
    EXPECT_EQ(copy.src_offset, 0u);
    EXPECT_EQ(copy.bytes, 16u);
    EXPECT_EQ(copy.src_byte_width, 16u);
}

TEST_F(D3D11ManagerTest, CreatesRawByteAddressBufferShaderResourceView) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr SRV_OUT = GPU_BUFFER_OUT + 0x38;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // A raw buffer may also be a vertex/UAV resource. StructureByteStride is
    // ignored unless BUFFER_STRUCTURED is set, so retaining a logical stride
    // must not reject this otherwise valid descriptor.
    // ByteWidth, Usage(DEFAULT), Bind(VB|SRV|UAV), CPUAccess,
    // Misc(BUFFER_ALLOW_RAW_VIEWS), Stride.
    const std::array<u32, 6> desc{16, 0, 0x89, 0, 0x20, 16};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(buffer, 0u);

    const auto create_srv = [&](const std::array<u32, 5>& srv_desc) {
        EXPECT_TRUE(memory.write_bytes(
            SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
            sizeof(srv_desc)).is_ok());
        set_arg(0, device);
        set_arg(1, buffer);
        set_arg(2, SRV_DESC);
        set_arg(3, SRV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 7);
    };

    // Learn D3D11_BUFFEREX_SRV: a raw view is DXGI_FORMAT_R32_TYPELESS with
    // BUFFEREX and the RAW flag, and counts its window in 4-byte units.
    create_srv({39, 11, 0, 4, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(SRV_OUT).value(), 0u);
    // A ByteAddressBuffer has no format to decode, so it stays a buffer and
    // no texture view is aliased over it.
    EXPECT_TRUE(backend.state.texture_buffer_creations.empty());

    create_srv({39, 11, 0, 5, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "window past the end";
    create_srv({42, 11, 0, 4, 1});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "raw needs R32_TYPELESS";
    // Flags without RAW make this a typed view, and R32_TYPELESS is not a
    // typed format (d3d11_a03_views_probe "srv buf raw BUFFER R32_TYPELESS").
    // An unknown flag bit alone is accepted: see
    // BufferExShaderResourceViewEchoesUnknownFlags.
    create_srv({39, 11, 0, 4, 2});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "typeless typed view";
}

TEST_F(D3D11ManagerTest, CreatesStructuredBufferWithShaderResourceAndUav) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    constexpr GuestAddr UAV_DESC = GPU_BUFFER_DESC + 0x40;
    constexpr GuestAddr SRV_OUT = GPU_BUFFER_OUT + 0x10;
    constexpr GuestAddr UAV_OUT = GPU_BUFFER_OUT + 0x18;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto create_buffer_desc = [&](const std::array<u32, 6>& desc) {
        EXPECT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
    };

    // The shape UE5's D3D11 RHI asks for: four 4-byte elements addressed as
    // both a StructuredBuffer and an RWStructuredBuffer.
    // ByteWidth, Usage(DEFAULT), Bind(SRV|UAV), CPUAccess, Misc(STRUCTURED),
    // StructureByteStride.
    create_buffer_desc({16, 0, 0x88, 0, 0x40, 4});
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(backend.state.last_buffer_desc.byte_width, 16u);

    // The stride only means something with BUFFER_STRUCTURED -- and without
    // it Windows IGNORES the stride rather than refusing it. Measured on
    // Windows 2026-09-22: stride 4, 3, and a non-tiling 5 on an SRV|UAV
    // buffer, 16 on a constant buffer and 12 on a vertex buffer all return
    // S_OK with no STRUCTURED flag. This assertion used to expect
    // E_INVALIDARG, which would refuse buffers real engines create.
    create_buffer_desc({16, 0, 0x88, 0, 0, 4});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "stride without the flag";
    create_buffer_desc({16, 0, 0x88, 0, 0, 5});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK) << "non-tiling stride, no flag";
    // STRUCTURED with stride 0 is refused (measured, same run).
    create_buffer_desc({16, 0, 0x88, 0, 0x40, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "structured without stride";
    create_buffer_desc({16, 0, 0x88, 0, 0x40, 6});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "stride not a multiple of 4";
    create_buffer_desc({20, 0, 0x88, 0, 0x40, 8});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "stride does not tile width";
    // STRUCTURED and ALLOW_RAW_VIEWS describe different views of the bytes.
    create_buffer_desc({16, 0, 0x88, 0, 0x60, 4});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG) << "structured and raw";

    // D3D11_SHADER_RESOURCE_VIEW_DESC: Format, ViewDimension(BUFFER=1),
    // FirstElement, NumElements.
    const std::array<u32, 4> srv_desc{0, 1, 1, 3};
    ASSERT_TRUE(memory.write_bytes(
        SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
        sizeof(srv_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, buffer);
    set_arg(2, SRV_DESC);
    set_arg(3, SRV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 7);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(SRV_OUT).value(), 0u);

    // D3D11_UNORDERED_ACCESS_VIEW_DESC: Format, ViewDimension(BUFFER=1),
    // FirstElement, NumElements, Flags.
    const std::array<u32, 5> uav_desc{0, 1, 1, 3, 0};
    ASSERT_TRUE(memory.write_bytes(
        UAV_DESC, reinterpret_cast<const Byte*>(uav_desc.data()),
        sizeof(uav_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, buffer);
    set_arg(2, UAV_DESC);
    set_arg(3, UAV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(UAV_OUT).value(), 0u);

    // The element window is bounds-checked against the buffer.
    const std::array<u32, 5> past_end{0, 1, 2, 3, 0};
    ASSERT_TRUE(memory.write_bytes(
        UAV_DESC, reinterpret_cast<const Byte*>(past_end.data()),
        sizeof(past_end)).is_ok());
    set_arg(3, UAV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // A COUNTER UAV over a structured, UNKNOWN-format buffer is served now
    // (11.6, oracle: S_OK), so this used to expect E_NOTIMPL from before the
    // hidden counter existed.
    const std::array<u32, 5> counter{0, 1, 0, 4, 2};
    ASSERT_TRUE(memory.write_bytes(
        UAV_DESC, reinterpret_cast<const Byte*>(counter.data()),
        sizeof(counter)).is_ok());
    set_arg(3, UAV_OUT);
    d3d11.impl_DeviceMethod(marshaler, 8);
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
}

TEST_F(D3D11ManagerTest, CreatesTypedBufferShaderResourceView) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    // Must clear the whole 20-byte BUFFEREX desc at SRV_DESC (DATA_BASE +
    // 0x6A0): the out-parameter is zeroed before the desc is read, so an
    // overlapping address silently erases FirstElement/NumElements/Flags.
    constexpr GuestAddr SRV_OUT = GPU_BUFFER_OUT + 0x38;

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    // The shape UE5's RHI creates for its default typed buffer: no structure
    // stride, no misc flags, viewed through a DXGI format instead.
    // ByteWidth, Usage(DEFAULT), Bind(VERTEX|SRV|UAV), CPUAccess, Misc, Stride.
    const std::array<u32, 6> desc{16, 0, 0x89, 0, 0, 0};
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(buffer, 0u);

    const auto create_srv = [&](const std::array<u32, 5>& srv_desc) {
        EXPECT_TRUE(memory.write_bytes(
            SRV_DESC, reinterpret_cast<const Byte*>(srv_desc.data()),
            sizeof(srv_desc)).is_ok());
        set_arg(0, device);
        set_arg(1, buffer);
        set_arg(2, SRV_DESC);
        set_arg(3, SRV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 7);
    };

    // Format, ViewDimension(BUFFER=1), FirstElement, NumElements, Flags.
    // The view aliases the buffer's storage as a texture of that format, so
    // the backend is told the format and the element window and the hardware
    // does the decode. DXGI_FORMAT_R32_UINT is 4 bytes: four fit exactly.
    create_srv({42, 1, 0, 4, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(SRV_OUT).value(), 0u);
    ASSERT_FALSE(backend.state.texture_buffer_creations.empty());
    EXPECT_EQ(backend.state.texture_buffer_creations.back().pixel_format,
              graphics::PixelFormat::R32Uint);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().num_elements, 4u);
    create_srv({41, 1, 1, 3, 0});  // R32_FLOAT, offset window
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().first_element, 1u);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().num_elements, 3u);

    // The element window is measured in format-sized elements and is
    // bounds-checked: a 16-byte buffer holds four R32 elements, not five.
    create_srv({42, 1, 0, 5, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    create_srv({42, 1, 4, 1, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // A wider format is addressable too, and its element count shrinks
    // accordingly: 16 bytes holds one RGBA32 element.
    create_srv({2, 1, 0, 1, 0});   // DXGI_FORMAT_R32G32B32A32_FLOAT
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    create_srv({2, 1, 0, 2, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // A packed format needs no shader-side unpacking: the texel decode is the
    // sampler hardware's job, which is the whole point of viewing the buffer
    // as a texture.
    create_srv({28, 1, 0, 4, 0});  // DXGI_FORMAT_R8G8B8A8_UNORM
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().pixel_format,
              graphics::PixelFormat::RGBA8Unorm);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().num_elements, 4u);
    create_srv({10, 1, 0, 2, 0});  // DXGI_FORMAT_R16G16B16A16_FLOAT
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // R11G11B10_FLOAT is packed the same way: three floats in 32 bits, decoded
    // by the sampler. It has a host equivalent (Metal RG11B10Float) and so is
    // addressable as a buffer texel; it used to sit in the "no host
    // equivalent" group below only because pixel_format_from_dxgi had no case
    // for it at all.
    create_srv({26, 1, 0, 4, 0});  // DXGI_FORMAT_R11G11B10_FLOAT
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().pixel_format,
              graphics::PixelFormat::RG11B10Float);
    // R16G16B16A16_SNORM is 8 bytes per element, so two fit in this buffer.
    create_srv({13, 1, 0, 2, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(backend.state.texture_buffer_creations.back().pixel_format,
              graphics::PixelFormat::RGBA16Snorm);

    // A block-compressed, depth or video format is not a Buffer<> format
    // (no FORMAT_SUPPORT_BUFFER): Windows refuses each with E_INVALIDARG
    // (d3d11_a03_views_probe "srv buf typed BC1_UNORM / D32_FLOAT"). These
    // expectations said E_NOTIMPL until the oracle measured them.
    create_srv({71, 1, 0, 1, 0});  // DXGI_FORMAT_BC1_UNORM
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    create_srv({40, 1, 0, 4, 0});  // DXGI_FORMAT_D32_FLOAT
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    create_srv({130, 1, 0, 4, 0});  // DXGI_FORMAT_P208
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);

    // Learn D3D11_BUFFEREX_SRV_FLAG_RAW: a raw view is a ByteAddressBuffer.
    // It needs a resource created with ALLOW_RAW_VIEWS and the typeless R32
    // format; this buffer has neither.
    create_srv({42, 11, 0, 4, 1});  // BUFFEREX with RAW, wrong format
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    create_srv({39, 11, 0, 4, 1});  // right format, no ALLOW_RAW_VIEWS
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    // Without the flag, BUFFEREX is the same typed view.
    create_srv({42, 11, 0, 4, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);

    // The u# side of the same buffer takes the identical element rule.
    const auto create_uav = [&](const std::array<u32, 5>& uav_desc) {
        EXPECT_TRUE(memory.write_bytes(
            SRV_DESC, reinterpret_cast<const Byte*>(uav_desc.data()),
            sizeof(uav_desc)).is_ok());
        set_arg(0, device);
        set_arg(1, buffer);
        set_arg(2, SRV_DESC);
        set_arg(3, SRV_OUT);
        d3d11.impl_DeviceMethod(marshaler, 8);
    };
    // Format, ViewDimension(BUFFER=1), FirstElement, NumElements, Flags.
    create_uav({42, 1, 0, 4, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(SRV_OUT).value(), 0u);
    // DXGI_FORMAT_R16G16B16A16_SNORM (13) is a typed member of the RGBA16
    // family. Metal RGBA16Snorm is the host texel, 8 bytes, so two fit.
    create_uav({13, 1, 0, 2, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(graphics::pixel_format_from_dxgi(37),
              graphics::PixelFormat::RG16Snorm);
    EXPECT_EQ(graphics::pixel_format_from_dxgi(58),
              graphics::PixelFormat::R16Snorm);
    create_uav({42, 1, 0, 5, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    create_uav({2, 1, 0, 1, 0});   // R32G32B32A32_FLOAT: one element fits
    EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
    // Block-compressed: not a typed UAV format, E_INVALIDARG on Windows
    // (d3d11_a03_views_probe; this said E_NOTIMPL before the measurement).
    create_uav({71, 1, 0, 1, 0});
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    // RAW asks for a view this buffer cannot carry: a raw view needs
    // ALLOW_RAW_VIEWS and R32_TYPELESS, and format 42 is R32_UINT. Measured on
    // Windows as E_INVALIDARG -- Windows judges the combination before it
    // judges support, so a malformed raw request never reaches the honest gap.
    // This expectation said E_NOTIMPL while Vitreo checked support first.
    create_uav({42, 1, 0, 4, 1});  // D3D11_BUFFER_UAV_FLAG_RAW
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
    // Learn (CopyStructureCount): the hidden counter belongs to "a UAV of a
    // Structured Buffer resource". This buffer is typed, so APPEND is refused
    // for that reason -- not because append is unserved, which is what this
    // expectation said while it was.
    create_uav({42, 1, 0, 4, 2});  // ..._APPEND on a typed buffer
    EXPECT_EQ(cpu.gpr.rax, d3d11::E_INVALIDARG);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test

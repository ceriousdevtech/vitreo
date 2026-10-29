// SPDX-License-Identifier: LGPL-2.1-only
//
// Vertex buffers, dynamic/no-overwrite maps and staging buffer copies.
//
// Split from the former test_d3d11.cpp; shared fixtures live in d3d11_test_*.hpp.

#include <gtest/gtest.h>

#include "d3d11_test_fixture.hpp"

namespace vitreo::winapi::d3d11_test {
namespace {

TEST_F(D3D11ManagerTest, CreatesVertexBufferWithStreamOutputBind) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    // D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_STREAM_OUTPUT is the standard
    // shape for a buffer whose generated vertices can be fed back into the
    // input assembler. The stream-output bind changes permitted pipeline
    // use; it does not require a distinct Metal allocation.
    const std::array<u32, 6> desc{
        1920, 0, 0x1 | 0x10, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 3);

    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_NE(memory.read_u64(GPU_BUFFER_OUT).value(), 0u);
    EXPECT_EQ(backend.state.buffer_creations, 1u);
    EXPECT_TRUE(backend.state.last_buffer_desc.vertex_buffer);
    EXPECT_EQ(backend.state.last_buffer_desc.byte_width, 1920u);
}

TEST_F(D3D11ManagerTest, IASetVertexBuffersRetainsInputSlotFour) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    const std::array<u32, 6> desc{
        64, 0, 0x1, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();

    constexpr GuestAddr binding = GPU_BUFFER_INITIAL;
    constexpr GuestAddr stride = GPU_BUFFER_INITIAL + 8;
    constexpr GuestAddr offset = GPU_BUFFER_INITIAL + 12;
    ASSERT_TRUE(memory.write_u64(binding, buffer).is_ok());
    ASSERT_TRUE(memory.write_u32(stride, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(offset, 4).is_ok());
    set_arg(0, context);
    set_arg(1, 4); // StartSlot
    set_arg(2, 1);
    set_arg(3, binding);
    set_arg(4, stride);
    set_arg(5, offset);
    d3d11.impl_ContextMethod(marshaler, 18);

    // The slot-four binding owns a reference exactly like slots zero to three:
    // the application's last Release answers 0 (measured, a12 [rc] "bound
    // VB") and the binding keeps the buffer alive.
    set_arg(0, buffer);
    d3d11.impl_BufferMethod(marshaler, 2);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_TRUE(kept_alive_by_runtime(buffer, &D3D11Manager::impl_BufferMethod));
}

TEST_F(D3D11ManagerTest, MapsAndUploadsDynamicBuffer) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    const std::array<u32, 6> desc{
        64, 2, 1, 0x10000, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
        sizeof(desc)).is_ok());

    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    ASSERT_NE(buffer, 0u);
    EXPECT_TRUE(backend.state.last_buffer_desc.cpu_writable);

    set_arg(0, context);
    set_arg(1, buffer);
    set_arg(2, 0);
    set_arg(3, 4);
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    ASSERT_NE(mapped, 0u);
    // Measured (d3d11_buffer_map_pitch_probe.expected): a buffer's RowPitch
    // and DepthPitch are its ByteWidth rounded up to 4.
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), 64u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(), 64u);

    std::array<u8, 64> update{};
    for (usize index = 0; index < update.size(); ++index) {
        update[index] = static_cast<u8>(index ^ 0xA5u);
    }
    ASSERT_TRUE(memory.write_bytes(
        mapped, reinterpret_cast<const Byte*>(update.data()),
        update.size()).is_ok());
    set_arg(0, context);
    set_arg(1, buffer);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);
    EXPECT_EQ(backend.state.buffer_updates, 1u);
    EXPECT_EQ(backend.state.last_buffer_update,
              std::vector<u8>(update.begin(), update.end()));
    EXPECT_EQ(backend.state.last_buffer_update_mode,
              graphics::BufferWrite::Discard);

    // Learn: WRITE_DISCARD makes previous contents undefined. A second
    // discard map must not spend bandwidth restoring or zero-initializing the
    // old 64-byte resource before the caller replaces it.
    const std::array<u8, 4> discarded_sentinel{0x31, 0x41, 0x59, 0x26};
    ASSERT_TRUE(memory.write_bytes(
        mapped, reinterpret_cast<const Byte*>(discarded_sentinel.data()),
        discarded_sentinel.size()).is_ok());
    set_arg(0, context);
    set_arg(1, buffer);
    set_arg(2, 0);
    set_arg(3, 4);
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    std::array<u8, 4> discard_probe{};
    ASSERT_TRUE(memory.read_bytes(
        mapped, discard_probe.data(), discard_probe.size()).is_ok());
    EXPECT_EQ(discard_probe, discarded_sentinel);
    ASSERT_TRUE(memory.write_bytes(
        mapped, reinterpret_cast<const Byte*>(update.data()),
        update.size()).is_ok());
    set_arg(0, context);
    set_arg(1, buffer);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    set_arg(0, context);
    set_arg(1, buffer);
    set_arg(2, 0);
    set_arg(3, 5);
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const std::array<u8, 4> patch{0xDE, 0xAD, 0xBE, 0xEF};
    ASSERT_TRUE(memory.write_bytes(
        mapped + 16, reinterpret_cast<const Byte*>(patch.data()),
        patch.size()).is_ok());
    set_arg(0, context);
    set_arg(1, buffer);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);
    EXPECT_EQ(backend.state.buffer_updates, 3u);
    EXPECT_EQ(backend.state.last_buffer_update_offset, 16u);
    EXPECT_EQ(backend.state.last_buffer_update,
              std::vector<u8>(patch.begin(), patch.end()));
    EXPECT_EQ(backend.state.last_buffer_update_mode,
              graphics::BufferWrite::NoOverwrite);
}

TEST_F(D3D11ManagerTest, DISABLED_NoOverwriteUnchangedBufferThroughput) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    const std::array<u32, 6> desc{1024 * 1024, 2, 1, 0x10000, 0, 0};
    ASSERT_TRUE(memory.write_bytes(GPU_BUFFER_DESC,
        reinterpret_cast<const Byte*>(desc.data()), sizeof(desc)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device); set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0); set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    const char* patch_value = std::getenv("VITREO_UNMAP_BENCH_PATCH");
    const usize patch_offset = patch_value ? std::strtoull(patch_value, nullptr, 0) : 0;
    ASSERT_LT(patch_offset, desc[0]);
    const auto start = std::chrono::steady_clock::now();
    constexpr u32 iterations = 4096;
    for (u32 i = 0; i < iterations; ++i) {
        set_arg(0, context); set_arg(1, buffer); set_arg(2, 0);
        set_arg(3, 5); set_arg(4, 0); set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        if (patch_value) {
            ASSERT_TRUE(memory.write_u8(memory.read_u64(MAPPED_OUT).value() +
                                      patch_offset, (i & 1u) ? 0 : 1).is_ok());
        }
        set_arg(0, context); set_arg(1, buffer); set_arg(2, 0);
        d3d11.impl_ContextMethod(marshaler, 15);
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::printf("unmap-bench iterations=%u raw_ns=%lld ns_per_map_unmap=%.3f\n",
                iterations, static_cast<long long>(ns), double(ns) / iterations);
    EXPECT_EQ(backend.state.buffer_updates, patch_value ? iterations : 0u);
}

TEST_F(D3D11ManagerTest, NoOverwriteSkipsEqualSpansAndPreservesExactDirtyRanges) {
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    constexpr u32 size = 1024 * 1024 + 3;
    const std::array<u32, 6> desc{size, 2, 1, 0x10000, 0, 0};
    ASSERT_TRUE(memory.write_bytes(GPU_BUFFER_DESC,
        reinterpret_cast<const Byte*>(desc.data()), sizeof(desc)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr buffer = memory.read_u64(GPU_BUFFER_OUT).value();
    const auto map = [&] {
        set_arg(0, context);
        set_arg(1, buffer);
        set_arg(2, 0);
        set_arg(3, 5);
        set_arg(4, 0);
        set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);
    };
    const auto unmap = [&] {
        set_arg(0, context);
        set_arg(1, buffer);
        set_arg(2, 0);
        d3d11.impl_ContextMethod(marshaler, 15);
    };
    for (u32 iteration = 0; iteration < 128; ++iteration) {
        map();
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        unmap();
    }
    EXPECT_EQ(backend.state.buffer_updates, 0u);
    for (const usize offset : {usize{0}, usize{255}, usize{256},
                               usize{1023}, usize{4095}, usize{4096},
                               usize{8192}, usize{size - 1}}) {
        map();
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
        ASSERT_TRUE(memory.write_u8(mapped + offset, 0x7b).is_ok());
        const auto before = backend.state.buffer_updates;
        unmap();
        EXPECT_EQ(backend.state.buffer_updates, before + 1);
        EXPECT_EQ(backend.state.last_buffer_update_offset, offset);
        EXPECT_EQ(backend.state.last_buffer_update, std::vector<u8>{0x7b});
        EXPECT_EQ(backend.state.last_buffer_update_mode,
              graphics::BufferWrite::NoOverwrite);
        map();
        ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
        unmap();
        EXPECT_EQ(backend.state.buffer_updates, before + 1)
            << "unchanged data was uploaded again after updating the shadow";
    }
    map();
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    ASSERT_TRUE(memory.protect_region(
        mapped + 4096, 4096, MemoryProtection::Read).is_ok());
    ASSERT_TRUE(memory.write_u8(mapped + size - 2, 0x6d).is_ok());
    const auto before = backend.state.buffer_updates;
    unmap();
    EXPECT_EQ(backend.state.buffer_updates, before + 1);
    EXPECT_EQ(backend.state.last_buffer_update_offset, size - 2);
    EXPECT_EQ(backend.state.last_buffer_update, std::vector<u8>{0x6d});
    ASSERT_TRUE(memory.protect_region(
        mapped + 4096, 4096, MemoryProtection::ReadWrite).is_ok());
}

TEST_F(D3D11ManagerTest, CopiesBoxedStagingBufferRegionIntoDefaultBuffer) {
    // Microsoft Learn: D3D11_BOX coordinates are bytes for buffers, and the
    // right coordinate is exclusive. ANGLE uses this staging-to-default path
    // when uploading an index-buffer range.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 6> staging_desc{
        64, 3, 0, 0x30000, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(staging_desc.data()),
        sizeof(staging_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, GPU_BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr staging = memory.read_u64(GPU_BUFFER_OUT).value();

    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    set_arg(3, 2); // D3D11_MAP_WRITE
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    std::array<u8, 64> source{};
    for (usize index = 0; index < source.size(); ++index) {
        source[index] = static_cast<u8>(0x80u + index);
    }
    ASSERT_TRUE(memory.write_bytes(
        mapped, reinterpret_cast<const Byte*>(source.data()),
        source.size()).is_ok());
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    const std::array<u32, 6> default_desc{
        64, 0, 2, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(default_desc.data()),
        sizeof(default_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, GPU_BUFFER_DESC);
    set_arg(2, 0);
    set_arg(3, BUFFER_OUT);
    d3d11.impl_DeviceMethod(marshaler, 3);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr destination = memory.read_u64(BUFFER_OUT).value();

    constexpr GuestAddr box_address = DATA_BASE + 0x700;
    const std::array<u32, 6> box{
        8, 0, 0, 24, 1, 1,
    };
    ASSERT_TRUE(memory.write_bytes(
        box_address, reinterpret_cast<const Byte*>(box.data()),
        sizeof(box)).is_ok());
    set_arg(0, context);
    set_arg(1, destination);
    set_arg(2, 0);
    set_arg(3, 4);
    set_arg(4, 0);
    set_arg(5, 0);
    set_arg(6, staging);
    set_arg(7, 0);
    set_arg(8, box_address);
    const unsigned updates = backend.state.buffer_updates;
    const usize copies = backend.state.buffer_region_copies.size();
    d3d11.impl_ContextMethod(marshaler, 46);

    // A GPU region copy, ordered with the rest of the GPU work: the staging
    // buffer has storage of its own, and the copy must see any GPU writes to
    // its source -- a CPU upload from the source's shadow would not.
    EXPECT_EQ(backend.state.buffer_updates, updates) << "no CPU upload";
    ASSERT_EQ(backend.state.buffer_region_copies.size(), copies + 1);
    const auto& copy = backend.state.buffer_region_copies.back();
    EXPECT_EQ(copy.dst_offset, 4u);
    EXPECT_EQ(copy.src_offset, 8u);
    EXPECT_EQ(copy.bytes, 16u);
    // The staging buffer's CPU writes reached its GPU storage at Unmap, so the
    // copy moves the bytes the guest wrote.
    EXPECT_EQ(backend.state.last_buffer_update,
              std::vector<u8>(source.begin(), source.end()));
}

TEST_F(D3D11ManagerTest, StagingMapReadReturnsWhatTheGpuCopiedIn) {
    // A compute shader's UAV output reaches the CPU as CopyResource into a
    // STAGING buffer, then Map(READ). The copy happens on the GPU, so Map --
    // which waits for the GPU (Learn) -- must read the staging buffer back
    // rather than return its CPU shadow. Oracle: compute_readback.
    create_default_device();
    backend.state.wait_idle_ok = true;
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto create = [&](const std::array<u32, 6>& desc) {
        EXPECT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(GPU_BUFFER_OUT).value();
    };
    // {ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags, Stride}
    const GuestAddr gpu = create({64, 0, 0x8, 0, 0, 0});        // DEFAULT, SRV
    const GuestAddr staging = create({64, 3, 0, 0x20000, 0, 0});  // STAGING
    const auto map_read = [&](u32 flags) {
        set_arg(0, context);
        set_arg(1, staging);
        set_arg(2, 0);
        set_arg(3, 1);  // D3D11_MAP_READ
        set_arg(4, flags);
        set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto unmap = [&] {
        set_arg(0, context);
        set_arg(1, staging);
        set_arg(2, 0);
        d3d11.impl_ContextMethod(marshaler, 15);
    };

    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, gpu);
    d3d11.impl_ContextMethod(marshaler, 47);  // CopyResource
    EXPECT_EQ(backend.state.buffer_copies, 1u) << "the copy runs on the GPU";

    const unsigned waits = backend.state.wait_idles;
    ASSERT_EQ(map_read(0), d3d11::S_OK);
    EXPECT_EQ(backend.state.wait_idles, waits + 1) << "Map waits for the GPU";
    EXPECT_EQ(backend.state.buffer_reads, 1u);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    for (u32 i = 0; i < 64; ++i) {
        EXPECT_EQ(memory.read_u8(mapped + i).value(),
                  static_cast<u8>(0xC0u ^ i)) << "byte " << i;
    }
    unmap();

    // Nothing wrote it since: the CPU copy is current, no second readback.
    // DO_NOT_WAIT is a valid flag for a READ map (Learn: D3D11_MAP_FLAG).
    ASSERT_EQ(map_read(0x100000u), d3d11::S_OK);
    EXPECT_EQ(backend.state.buffer_reads, 1u);
    unmap();
}

// Learn (ID3D11DeviceContext::Map, D3D11_MAP_FLAG): Map waits until the GPU
// is done with the resource, or with D3D11_MAP_FLAG_DO_NOT_WAIT returns
// DXGI_ERROR_WAS_STILL_DRAWING (winerror.h 0x887A000A) while it is not. The
// wait is for the submission that last copied into or out of the buffer --
// not for the whole queue, which serialized UE's render thread with the GPU.
TEST_F(D3D11ManagerTest, StagingMapWaitsOnlyForTheBuffersLastGpuUse) {
    create_default_device();
    backend.state.generations = true;
    backend.state.recording_gen = 5;
    backend.state.completed_gen = 0;
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto create = [&](const std::array<u32, 6>& desc) {
        EXPECT_TRUE(memory.write_bytes(
            GPU_BUFFER_DESC, reinterpret_cast<const Byte*>(desc.data()),
            sizeof(desc)).is_ok());
        set_arg(0, device);
        set_arg(1, GPU_BUFFER_DESC);
        set_arg(2, 0);
        set_arg(3, GPU_BUFFER_OUT);
        d3d11.impl_DeviceMethod(marshaler, 3);
        EXPECT_EQ(cpu.gpr.rax, d3d11::S_OK);
        return memory.read_u64(GPU_BUFFER_OUT).value();
    };
    // {ByteWidth, Usage, BindFlags, CPUAccessFlags, MiscFlags, Stride}
    const GuestAddr gpu = create({64, 0, 0x8, 0, 0, 0});           // DEFAULT
    const GuestAddr readback = create({64, 3, 0, 0x20000, 0, 0});  // READ
    const GuestAddr upload = create({64, 3, 0, 0x10000, 0, 0});    // WRITE
    const auto map = [&](GuestAddr buffer, u32 type, u32 flags) {
        ASSERT_TRUE(memory.write_u64(MAPPED_OUT, 0xDEADBEEFull).is_ok());
        set_arg(0, context);
        set_arg(1, buffer);
        set_arg(2, 0);
        set_arg(3, type);
        set_arg(4, flags);
        set_arg(5, MAPPED_OUT);
        d3d11.impl_ContextMethod(marshaler, 14);
    };
    const auto unmap = [&](GuestAddr buffer) {
        set_arg(0, context);
        set_arg(1, buffer);
        set_arg(2, 0);
        d3d11.impl_ContextMethod(marshaler, 15);
    };
    const auto copy = [&](GuestAddr dst, GuestAddr src) {
        set_arg(0, context);
        set_arg(1, dst);
        set_arg(2, src);
        d3d11.impl_ContextMethod(marshaler, 47);  // CopyResource
    };
    constexpr u32 kRead = 1, kWrite = 2, kDoNotWait = 0x100000u;
    constexpr u32 kWasStillDrawing = 0x887A000Au;

    // The copy joins generation 5; the frame goes on (6 is recording) and
    // the GPU has retired only up to 4.
    copy(readback, gpu);
    backend.state.recording_gen = 6;
    backend.state.completed_gen = 4;
    const unsigned idles = backend.state.wait_idles;
    const unsigned flushes = backend.state.flushes;

    map(readback, kRead, kDoNotWait);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kWasStillDrawing);
    EXPECT_EQ(memory.read_u64(MAPPED_OUT).value(), 0u) << "pData is NULL";
    EXPECT_EQ(backend.state.buffer_reads, 0u);
    EXPECT_TRUE(backend.state.generation_waits.empty()) << "DO_NOT_WAIT";
    EXPECT_EQ(backend.state.flushes, flushes)
        << "generation 5 is already submitted; nothing to hand over";

    map(readback, kRead, 0);
    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    EXPECT_EQ(backend.state.generation_waits, std::vector<u64>{5})
        << "waits for the copy's generation, not for generation 6";
    EXPECT_EQ(backend.state.wait_idles, idles) << "never a whole-queue wait";
    EXPECT_EQ(backend.state.buffer_reads, 1u) << "GPU contents read back";
    unmap(readback);

    // Retired: DO_NOT_WAIT maps without waiting or reading back again.
    map(readback, kRead, kDoNotWait);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    EXPECT_EQ(backend.state.generation_waits.size(), 1u);
    unmap(readback);

    // A copy still in the recording: DO_NOT_WAIT reports it and submits the
    // recording so a polling caller sees it retire.
    copy(readback, gpu);  // joins generation 6, not submitted
    map(readback, kRead, kDoNotWait);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kWasStillDrawing);
    EXPECT_EQ(backend.state.flushes, flushes + 1);
    EXPECT_EQ(backend.state.recording_gen, 7u);
    backend.state.completed_gen = 6;
    map(readback, kRead, kDoNotWait);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    EXPECT_EQ(backend.state.buffer_reads, 2u);
    unmap(readback);

    // A WRITE map waits for GPU work that READS the buffer: its Unmap uploads
    // in place, under a copy still sourcing the old bytes.
    copy(gpu, upload);  // generation 7
    backend.state.recording_gen = 8;
    map(upload, kWrite, kDoNotWait);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), kWasStillDrawing);
    map(upload, kWrite, 0);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), d3d11::S_OK);
    EXPECT_EQ(backend.state.generation_waits, (std::vector<u64>{5, 7}));
    unmap(upload);
    EXPECT_EQ(backend.state.wait_idles, idles);
}

TEST_F(D3D11ManagerTest,
       StagingTextureMapsNativeBytesAndCopiesAcrossGpuBoundary) {
    // Learn: STAGING resources have no bind flags and may be mapped for the
    // CPU access declared at creation. ANGLE uses this exact R8 read/write
    // descriptor for its one-pixel texture allocation path.
    create_default_device();
    const GuestAddr device = memory.read_u64(DEVICE_OUT).value();
    const GuestAddr context = memory.read_u64(CONTEXT_OUT).value();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    const std::array<u32, 11> staging_desc{
        1, 1, 1, 1, 61, 1, 0, 3, 0, 0x30000, 0,
    };
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
    ASSERT_NE(staging, 0u);
    EXPECT_EQ(backend.state.texture_creations, 0u);

    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    set_arg(3, 3); // D3D11_MAP_READ_WRITE
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr mapped = memory.read_u64(MAPPED_OUT).value();
    ASSERT_NE(mapped, 0u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 8).value(), 1u);
    EXPECT_EQ(memory.read_u32(MAPPED_OUT + 12).value(), 1u);
    ASSERT_TRUE(memory.write_u8(mapped, 0xA5u).is_ok());
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);

    // A staging-to-default CopyResource uploads the native R8 byte.
    const std::array<u32, 11> default_desc{
        1, 1, 1, 1, 61, 1, 0, 0, 0, 0, 0,
    };
    ASSERT_TRUE(memory.write_bytes(
        TEXTURE_DESC, reinterpret_cast<const Byte*>(default_desc.data()),
        sizeof(default_desc)).is_ok());
    set_arg(0, device);
    set_arg(1, TEXTURE_DESC);
    set_arg(2, 0);
    set_arg(3, VIEW_OUT);
    d3d11.impl_DeviceMethod(marshaler, 5);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    const GuestAddr gpu_texture = memory.read_u64(VIEW_OUT).value();
    ASSERT_NE(gpu_texture, 0u);
    ASSERT_EQ(backend.state.texture_creations, 1u);

    set_arg(0, context);
    set_arg(1, gpu_texture);
    set_arg(2, staging);
    d3d11.impl_ContextMethod(marshaler, 47);
    ASSERT_EQ(backend.state.texture_updates, 1u);
    EXPECT_EQ(backend.state.last_texture_update_row_pitch, 1u);
    EXPECT_EQ(backend.state.last_texture_update,
              std::vector<u8>({0xA5u}));

    // The reverse copy waits for backend completion and refreshes the staging
    // shadow before Map(READ) exposes it.
    backend.state.native_texture_readback = {0x3Cu};
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, gpu_texture);
    d3d11.impl_ContextMethod(marshaler, 47);
    ASSERT_EQ(backend.state.texture_native_readbacks, 1u);
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    set_arg(3, 1); // D3D11_MAP_READ
    set_arg(4, 0);
    set_arg(5, MAPPED_OUT);
    d3d11.impl_ContextMethod(marshaler, 14);
    ASSERT_EQ(cpu.gpr.rax, d3d11::S_OK);
    EXPECT_EQ(memory.read_u8(mapped).value(), 0x3Cu);
    set_arg(0, context);
    set_arg(1, staging);
    set_arg(2, 0);
    d3d11.impl_ContextMethod(marshaler, 15);
}

}  // namespace
}  // namespace vitreo::winapi::d3d11_test

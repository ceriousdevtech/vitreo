/// @file test_d3d9_buffers.cpp
/// @brief Vertex and index buffers.
///        Part of the D3D9 frontend contract tests (see test_d3d9.cpp).

#include "d3d9_test_fixture.hpp"

#include <algorithm>
#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using vitreo::winapi::d3d9_test::D3D9ManagerTest;

TEST_F(D3D9ManagerTest, CreateVertexBufferSucceedsWhenPreferredLockWindowIsFull) {
    // Learn: CreateVertexBuffer returns E_OUTOFMEMORY only when the process
    // cannot allocate. Occupying the preferred 0x50000000–0x60000000 window
    // must not fail a valid create — lock storage falls back to free user VA.
    ASSERT_TRUE(memory_.map_region(0x50000000ull, 0x10000000ull,
                                   MemoryProtection::ReadWrite)
                    .is_ok());
    const auto [factory, device] = create_device();
    ASSERT_NE(device, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 36864, 0, 0, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    EXPECT_NE(memory_.read_u32(SCRATCH_ADDR).value(), 0u);
}

TEST_F(D3D9ManagerTest, VertexBufferLockGivesGuestMemoryAndUnlockUploadsIt) {
    const auto [factory, device] = create_device();
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 256, 0, 0x142 /* FVF */, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(buffer, 0u);

    // Lock a 16-byte range at offset 32.
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 32, 16, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      0}),
              D3D_OK);
    const u32 pointer = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(pointer, 0u);

    const std::array<u8, 16> payload{1, 2, 3, 4, 5, 6, 7, 8,
                                     9, 10, 11, 12, 13, 14, 15, 16};
    ASSERT_TRUE(memory_.write_bytes(pointer, payload.data(),
                                    payload.size()).is_ok());
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);

    ASSERT_EQ(backend_.record.buffer_updates, 1u);
    EXPECT_EQ(backend_.record.last_buffer_update_offset, 32u);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Ordered);
    ASSERT_EQ(backend_.record.last_buffer_update.size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                           backend_.record.last_buffer_update.begin()));
}

TEST_F(D3D9ManagerTest, VertexBufferUnlockNamesTheLockFlagsOrdering) {
    // Learn D3DLOCK: NOOVERWRITE promises the locked bytes are not in use,
    // so they go in place; DISCARD gets new memory while the GPU keeps the
    // old; without either "the driver must finish using the resource before
    // returning from locking", so work already recorded keeps the old bytes.
    // Both flags at once: renaming honours either promise.
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0x200 /* D3DUSAGE_DYNAMIC */, 0x142, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(buffer, 0u);
    const auto lock_write_unlock = [&](u32 flags, u8 value) {
        ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                         {buffer, 16, 8,
                          static_cast<u32>(SCRATCH_ADDR + 0x100), flags}),
                  D3D_OK);
        const u32 pointer = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
        ASSERT_NE(pointer, 0u);
        const std::array<u8, 8> payload{value, value, value, value,
                                        value, value, value, value};
        ASSERT_TRUE(memory_.write_bytes(pointer, payload.data(),
                                        payload.size()).is_ok());
        ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}),
                  D3D_OK);
    };
    lock_write_unlock(D3DLOCK_NOOVERWRITE, 1);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::NoOverwrite);
    lock_write_unlock(D3DLOCK_DISCARD, 2);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Discard);
    lock_write_unlock(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE, 3);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Discard);
    lock_write_unlock(0, 4);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Ordered);
    EXPECT_EQ(backend_.record.buffer_updates, 4u);
}

TEST_F(D3D9ManagerTest, UnlockThatMayRenameWaitsForQueuedDraws) {
    // The command-stream worker resolves a draw's buffer storage when it
    // records the draw, not when the draw is queued. A write that may rename
    // (DISCARD, or a plain lock) must therefore reach the backend only after
    // every draw queued before it was recorded -- else those draws read the
    // new storage, and the rename reclaims pool slabs a draw is binding.
    // Two holes: Lock(DISCARD | NOOVERWRITE) skipped its drain, and a draw
    // queued between Lock and Unlock was never waited for.
    const auto [factory, device] = create_device();
    ASSERT_NE(bind_triangle_stream(device), 0u);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 41, {device}), D3D_OK);
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 81,
                     {device, 4 /* TRIANGLELIST */, 0, 1}),
              D3D_OK);  // pipeline and pass now exist
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0x200 /* D3DUSAGE_DYNAMIC */, 0x142, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 dynamic = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(dynamic, 0u);
    const auto lock = [&](u32 flags) {
        ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                         {dynamic, 0, 16,
                          static_cast<u32>(SCRATCH_ADDR + 0x100), flags}),
                  D3D_OK);
        const u32 pointer = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
        const std::array<u8, 16> payload{};
        ASSERT_TRUE(memory_.write_bytes(pointer, payload.data(),
                                        payload.size()).is_ok());
    };
    const auto queued_draw = [&] {
        ASSERT_EQ(invoke_queued(D3D9InterfaceId::Device, 81,
                                {device, 4 /* TRIANGLELIST */, 0, 1}),
                  D3D_OK);
    };
    const auto unlock = [&] {
        ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {dynamic}),
                  D3D_OK);
    };
    backend_.record.draw_delay_ms.store(150);

    // A draw queued before Lock(DISCARD | NOOVERWRITE).
    queued_draw();
    lock(D3DLOCK_DISCARD | D3DLOCK_NOOVERWRITE);
    unlock();
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Discard);
    EXPECT_EQ(backend_.record.draws_finished_at_buffer_update, 2u)
        << "a DISCARD reached the backend while a queued draw was recording";
    mgr_.d3d9_cs_flush();
    const unsigned after_discard = backend_.record.draws_finished.load();
    ASSERT_EQ(after_discard, 2u);

    // A draw queued between a plain Lock and its Unlock.
    lock(0);
    queued_draw();
    unlock();
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Ordered);
    EXPECT_EQ(backend_.record.draws_finished_at_buffer_update,
              after_discard + 1)
        << "a plain Unlock did not wait for the draw queued after its Lock";

    backend_.record.draw_delay_ms.store(0);
    mgr_.d3d9_cs_flush();
}

TEST_F(D3D9ManagerTest, VertexBufferRepeatedUnlockPublishesEachLock) {
    // Learn: D3DLOCK_DISCARD may rename GPU storage so earlier draws keep
    // the previous bytes. Each Unlock must still publish that lock's payload.
    // Metal renames into its DISCARD pool, reclaimed on GPU completion.
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 32, 0x200 /* D3DUSAGE_DYNAMIC */, 0x142, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(buffer, 0u);

    const auto lock_write_unlock = [&](const std::array<u8, 16>& payload) {
        ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                         {buffer, 0, 16,
                          static_cast<u32>(SCRATCH_ADDR + 0x100),
                          0x2000 /* D3DLOCK_DISCARD */}),
                  D3D_OK);
        const u32 pointer = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
        ASSERT_NE(pointer, 0u);
        ASSERT_TRUE(memory_.write_bytes(pointer, payload.data(),
                                        payload.size()).is_ok());
        ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}),
                  D3D_OK);
    };

    const std::array<u8, 16> first{1, 2, 3, 4, 5, 6, 7, 8,
                                   9, 10, 11, 12, 13, 14, 15, 16};
    const std::array<u8, 16> second{16, 15, 14, 13, 12, 11, 10, 9,
                                    8, 7, 6, 5, 4, 3, 2, 1};
    lock_write_unlock(first);
    ASSERT_EQ(backend_.record.buffer_updates, 1u);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Discard);
    lock_write_unlock(second);
    ASSERT_EQ(backend_.record.buffer_updates, 2u);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Discard);
    ASSERT_EQ(backend_.record.last_buffer_update.size(), second.size());
    EXPECT_TRUE(std::equal(second.begin(), second.end(),
                           backend_.record.last_buffer_update.begin()));
}

TEST_F(D3D9ManagerTest, VertexBufferDiscardRangeUnlockPublishesOnlyLockedRange) {
    // Learn: D3DLOCK_DISCARD discards the entire buffer; unlocked bytes in
    // the new storage are undefined. Unlock still publishes only the
    // OffsetToLock/SizeToLock range — not a full-buffer snapshot of the
    // previous contents.
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0x200 /* D3DUSAGE_DYNAMIC */, 0x142, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(buffer, 0u);

    std::array<u8, 64> initial{};
    initial.fill(0x11);
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0}),
              D3D_OK);
    const u32 whole = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_TRUE(memory_.write_bytes(whole, initial.data(), initial.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);

    const std::array<u8, 16> payload{1, 2, 3, 4, 5, 6, 7, 8,
                                     9, 10, 11, 12, 13, 14, 15, 16};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 32, 16, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      D3DLOCK_DISCARD}),
              D3D_OK);
    const u32 pointer = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_NE(pointer, 0u);
    ASSERT_TRUE(memory_.write_bytes(pointer, payload.data(), payload.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);

    ASSERT_EQ(backend_.record.buffer_updates, 2u);
    EXPECT_EQ(backend_.record.last_buffer_update_offset, 32u);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Discard);
    ASSERT_EQ(backend_.record.last_buffer_update.size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                           backend_.record.last_buffer_update.begin()));
}

TEST_F(D3D9ManagerTest, VertexBufferRangeUnlockPreservesBytesOutsideLock) {
    // Without DISCARD, a range Unlock publishes only the locked range and
    // leaves guest bytes outside that range unchanged.
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0, 0x142, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(buffer, 0u);

    std::array<u8, 64> initial{};
    initial.fill(0xAA);
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0}),
              D3D_OK);
    const u32 whole = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_TRUE(memory_.write_bytes(whole, initial.data(), initial.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);

    const std::array<u8, 8> payload{1, 2, 3, 4, 5, 6, 7, 8};
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 16, 8, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      0}),
              D3D_OK);
    const u32 pointer = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    ASSERT_TRUE(memory_.write_bytes(pointer, payload.data(), payload.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);

    EXPECT_EQ(backend_.record.last_buffer_update_offset, 16u);
    EXPECT_EQ(backend_.record.last_buffer_update_mode,
              graphics::BufferWrite::Ordered);
    ASSERT_EQ(backend_.record.last_buffer_update.size(), payload.size());
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                           backend_.record.last_buffer_update.begin()));

    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      D3DLOCK_READONLY}),
              D3D_OK);
    const u32 read_ptr = memory_.read_u32(SCRATCH_ADDR + 0x100).value();
    std::array<u8, 64> observed{};
    ASSERT_TRUE(memory_.read_bytes(read_ptr, observed.data(), observed.size())
                    .is_ok());
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);

    EXPECT_TRUE(std::equal(initial.begin(), initial.begin() + 16,
                           observed.begin()));
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(),
                           observed.begin() + 16));
    EXPECT_TRUE(std::equal(initial.begin() + 24, initial.end(),
                           observed.begin() + 24));
}

TEST_F(D3D9ManagerTest, VertexBufferRejectsDoubleLockAndOutOfRangeLock) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();

    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x100), 0}),
              D3DERR_INVALIDCALL);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);
    // Unlock without a lock.
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}),
              D3DERR_INVALIDCALL);
    // Past the end of the buffer.
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 32, 64, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      0}),
              D3DERR_INVALIDCALL);
}

TEST_F(D3D9ManagerTest, ReadOnlyLockDoesNotUploadToTheBackend) {
    const auto [factory, device] = create_device();
    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 26,
                     {device, 64, 0, 0, 0, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 11,
                     {buffer, 0, 0, static_cast<u32>(SCRATCH_ADDR + 0x100),
                      D3DLOCK_READONLY}),
              D3D_OK);
    EXPECT_EQ(invoke(D3D9InterfaceId::VertexBuffer, 12, {buffer}), D3D_OK);
    EXPECT_EQ(backend_.record.buffer_updates, 0u);
}

TEST_F(D3D9ManagerTest, IndexBufferDescribesItselfAndRejectsBadFormats) {
    const auto [factory, device] = create_device();
    // A vertex FVF is not a valid index format.
    EXPECT_EQ(invoke(D3D9InterfaceId::Device, 27,
                     {device, 64, 0, 22, 0, static_cast<u32>(SCRATCH_ADDR), 0}),
              D3DERR_INVALIDCALL);

    ASSERT_EQ(invoke(D3D9InterfaceId::Device, 27,
                     {device, 64, 0, D3DFMT_INDEX16, 0,
                      static_cast<u32>(SCRATCH_ADDR), 0}),
              D3D_OK);
    const u32 buffer = memory_.read_u32(SCRATCH_ADDR).value();
    ASSERT_NE(buffer, 0u);
    EXPECT_EQ(invoke(D3D9InterfaceId::IndexBuffer, 13,
                     {buffer, static_cast<u32>(SCRATCH_ADDR + 0x100)}),
              D3D_OK);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100 + 0).value(),
              D3DFMT_INDEX16);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100 + 4).value(), 7u);
    EXPECT_EQ(memory_.read_u32(SCRATCH_ADDR + 0x100 + 16).value(), 64u);
    // GetType on the resource base.
    EXPECT_EQ(invoke(D3D9InterfaceId::IndexBuffer, 10, {buffer}), 7u);
}

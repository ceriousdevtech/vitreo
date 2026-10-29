/// @file test_file_overlapped_pipe_write.cpp
/// @brief Unit tests for overlapped named-pipe writes: synchronous completion, inline writes, worker reaping and reclamation

#include "kernel32_file_fixture.hpp"

TEST_F(FileConsoleBridgeTest,
       OverlappedNamedPipeWriteCompletesSynchronouslyWhenBufferAcceptsData) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    constexpr u32 kFileFlagOverlapped = 0x40000000u;

    std::mutex completion_mutex;
    bool completed = false;
    bool completed_synchronously = false;
    Handle completed_handle = 0;
    u32 completed_bytes = 0;
    GuestAddr completed_overlapped = 0;
    bool completed_is_64bit = true;
    u32 completed_nt_status = 0xFFFFFFFFu;
    u32 completed_win_error = 0xFFFFFFFFu;

    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vtm_overlapped_write_" +
        std::to_string(::getpid());
    const Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = fm.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING,
        kFileFlagOverlapped);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);

    fm.set_overlapped_completion_callback(
        [&](Handle file, u32 bytes, GuestAddr overlapped, bool is_64bit,
            u32 nt_status, u32 win_error, bool synchronous) {
            std::lock_guard<std::mutex> lock(completion_mutex);
            completed = true;
            completed_handle = file;
            completed_bytes = bytes;
            completed_overlapped = overlapped;
            completed_is_64bit = is_64bit;
            completed_nt_status = nt_status;
            completed_win_error = win_error;
            completed_synchronously = synchronous;
        });

    constexpr GuestAddr kOverlapped = 0x10400;
    constexpr GuestAddr kAsyncCount = 0x10500;
    constexpr GuestAddr kReadBuffer = 0x10600;
    constexpr GuestAddr kReadCount = 0x10700;
    constexpr GuestAddr kStack = 0x10800;
    constexpr std::string_view kPayload = "mojo";
    ASSERT_TRUE(guest_mem.write_bytes(
        kBufAddr, reinterpret_cast<const Byte*>(kPayload.data()),
        kPayload.size()).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(kOverlapped, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(kOverlapped + 4, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(kOverlapped + 16, 0).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(kAsyncCount, 0xFFFFFFFFu).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    ASSERT_TRUE(guest_mem.write_u32(kStack + 4, client).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(kStack + 8, kBufAddr).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(
        kStack + 12, static_cast<u32>(kPayload.size())).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(kStack + 16, kAsyncCount).is_ok());
    ASSERT_TRUE(guest_mem.write_u32(kStack + 20, kOverlapped).is_ok());
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);

    set_last_error(error_codes::ERROR_SUCCESS);
    impl_WriteFile(marshaler, fm);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
    EXPECT_EQ(guest_mem.read_u32(kAsyncCount).value(), kPayload.size());

    ASSERT_TRUE(fm.read_file(
        server, kReadBuffer, static_cast<u32>(kPayload.size()), kReadCount));
    std::array<char, 4> received{};
    ASSERT_TRUE(guest_mem.read_bytes(
        kReadBuffer, reinterpret_cast<Byte*>(received.data()),
        received.size()).is_ok());
    EXPECT_EQ(std::string_view(received.data(), received.size()), kPayload);

    std::unique_lock<std::mutex> completion_lock(completion_mutex);
    EXPECT_TRUE(completed);
    EXPECT_EQ(completed_handle, client);
    EXPECT_EQ(completed_bytes, kPayload.size());
    EXPECT_EQ(completed_overlapped, kOverlapped);
    EXPECT_FALSE(completed_is_64bit);
    EXPECT_EQ(completed_nt_status, 0u);
    EXPECT_EQ(completed_win_error, 0u);
    EXPECT_TRUE(completed_synchronously);
}

TEST_F(FileConsoleBridgeTest,
       OverlappedNamedPipeInlineWritesDoNotCreateCompletionWorkers) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    constexpr u32 kFileFlagOverlapped = 0x40000000u;
    constexpr int kBurst = 32;

    std::mutex completion_mutex;
    int completed = 0;

    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vtm_overlapped_burst_" +
        std::to_string(::getpid());
    const Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = fm.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING,
        kFileFlagOverlapped);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);

    fm.set_overlapped_completion_callback(
        [&](Handle, u32, GuestAddr, bool, u32, u32, bool) {
            std::lock_guard<std::mutex> lock(completion_mutex);
            ++completed;
        });

    ASSERT_TRUE(guest_mem.map_region(
        0x20000, 4096, MemoryProtection::ReadWrite).is_ok());

    constexpr GuestAddr kOvBase = 0x20000;
    constexpr GuestAddr kAsyncCount = 0x10500;
    constexpr GuestAddr kStack = 0x10800;
    constexpr std::string_view kPayload = "mojo";
    ASSERT_TRUE(guest_mem.write_bytes(
        kBufAddr, reinterpret_cast<const Byte*>(kPayload.data()),
        kPayload.size()).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);

    for (int i = 0; i < kBurst; ++i) {
        const GuestAddr overlapped = kOvBase + static_cast<GuestAddr>(i * 32);
        ASSERT_TRUE(guest_mem.write_u32(overlapped, 0).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(overlapped + 4, 0).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(overlapped + 16, 0).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kAsyncCount, 0xFFFFFFFFu).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 4, client).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 8, kBufAddr).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(
            kStack + 12, static_cast<u32>(kPayload.size())).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 16, kAsyncCount).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 20, overlapped).is_ok());
        set_last_error(error_codes::ERROR_SUCCESS);
        impl_WriteFile(marshaler, fm);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
        EXPECT_EQ(guest_mem.read_u32(kAsyncCount).value(), kPayload.size());
    }
    EXPECT_EQ(fm.overlapped_pipe_io_worker_count(), 0u);

    std::unique_lock<std::mutex> completion_lock(completion_mutex);
    EXPECT_EQ(completed, kBurst);
    EXPECT_EQ(fm.overlapped_pipe_io_worker_count(), 0u);
}

TEST(ReapedWorkersTest, FinishedWorkersAreHandedBackForJoin) {
    constexpr int kWorkers = 200;
    std::mutex owner;
    ReapedWorkers workers;
    std::atomic<int> ran{0};
    for (int i = 0; i < kWorkers; ++i) {
        ReapedWorkers::Finished finished;
        std::lock_guard<std::mutex> lock(owner);
        workers.spawn([&](std::stop_token) { ran.fetch_add(1); }, finished);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::size_t retained = 0;
    do {
        ReapedWorkers::Finished finished;
        std::lock_guard<std::mutex> lock(owner);
        workers.take_finished(finished);
        retained = workers.size();
    } while (retained != 0 && std::chrono::steady_clock::now() < deadline);
    EXPECT_EQ(ran.load(), kWorkers);
    EXPECT_EQ(retained, 0u);

    // Teardown stops and hands back a worker that is still running.
    ReapedWorkers::Finished all;
    workers.spawn([](std::stop_token stop) {
        while (!stop.stop_requested()) std::this_thread::yield();
    }, all);
    workers.take_all(all);
    EXPECT_EQ(workers.size(), 0u);
    all.clear();
}

// A pended overlapped pipe write finishes on a host worker. Once the I/O has
// completed nothing of it may stay behind: an unjoined finished worker keeps
// its pthread record (a 32 KiB VM region on macOS), and a Chromium renderer
// pends writes continuously, so retaining one per write grew without bound.
TEST_F(FileConsoleBridgeTest,
       OverlappedNamedPipePendedWriteWorkersAreReclaimed) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    constexpr u32 kFileFlagOverlapped = 0x40000000u;
    constexpr int kWrites = 48;
    constexpr u32 kPayloadSize = 512 * 1024;
    constexpr GuestAddr kPayload = 0x400000;
    constexpr GuestAddr kReadBuf = 0x500000;
    constexpr GuestAddr kOverlapped = 0x10400;
    constexpr GuestAddr kAsyncCount = 0x10500;
    constexpr GuestAddr kReadCount = 0x10700;
    constexpr GuestAddr kStack = 0x10800;

    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    int completed = 0;
    int pended = 0;

    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vtm_overlapped_pended_" + std::to_string(::getpid());
    const Handle server = fm.create_named_pipe_server(pipe);
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = fm.create_file(
        pipe, GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING,
        kFileFlagOverlapped);
    ASSERT_NE(client, INVALID_HANDLE);
    ASSERT_EQ(fm.connect_named_pipe(server, 2000),
              PipeConnectResult::AlreadyConnected);
    fm.set_overlapped_completion_callback(
        [&](Handle, u32, GuestAddr, bool, u32, u32, bool) {
            std::lock_guard<std::mutex> lock(completion_mutex);
            ++completed;
            completion_cv.notify_all();
        });

    ASSERT_TRUE(guest_mem.map_region(
        kPayload, kPayloadSize, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(guest_mem.map_region(
        kReadBuf, kPayloadSize, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    Marshaler marshaler(guest_mem);
    marshaler.attach_cpu(cpu);

    for (int i = 0; i < kWrites; ++i) {
        ASSERT_TRUE(guest_mem.write_u32(kOverlapped, 0).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kOverlapped + 4, 0).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kOverlapped + 16, 0).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 4, client).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 8, kPayload).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 12, kPayloadSize).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 16, kAsyncCount).is_ok());
        ASSERT_TRUE(guest_mem.write_u32(kStack + 20, kOverlapped).is_ok());
        set_last_error(error_codes::ERROR_SUCCESS);
        impl_WriteFile(marshaler, fm);
        if (static_cast<u32>(cpu.gpr.rax) == 0u) {
            ASSERT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);
            ++pended;
        }

        u32 drained = 0;
        while (drained < kPayloadSize) {
            ASSERT_TRUE(fm.read_file(server, kReadBuf, kPayloadSize - drained,
                                     kReadCount));
            const u32 got = guest_mem.read_u32(kReadCount).value();
            ASSERT_GT(got, 0u);
            drained += got;
        }

        std::unique_lock<std::mutex> lock(completion_mutex);
        ASSERT_TRUE(completion_cv.wait_for(
            lock, std::chrono::seconds(5),
            [&] { return completed == i + 1; }));
    }
    ASSERT_GT(pended, kWrites / 2) << "writes larger than the pipe buffer "
                                      "must pend";

    // Pended writes share one write service thread; none is per operation.
    EXPECT_EQ(fm.overlapped_pipe_io_worker_count(), 1u);
}

/// @file test_file_overlapped_pipe_write_service.cpp
/// @brief Unit tests for the overlapped named-pipe write service: ordering, cancel, close, reader close, and the pended-write latency bench

#include "kernel32_file_fixture.hpp"

namespace {

int host_thread_count() {
    struct proc_taskinfo info {};
    const int n = ::proc_pidinfo(::getpid(), PROC_PIDTASKINFO, 0, &info,
                                 sizeof(info));
    return n == static_cast<int>(sizeof(info)) ? info.pti_threadnum : -1;
}

struct PipeWriteCompletion {
    Handle handle;
    u32 bytes;
    GuestAddr overlapped;
    bool is_64bit;
    u32 nt_status;
    u32 win_error;
};

}  // namespace

class OverlappedPipeWriteServiceTest
    : public FileConsoleBridgeTest,
      public ::testing::WithParamInterface<bool> {
protected:
    static constexpr u32 kFileFlagOverlapped = 0x40000000u;
    static constexpr u32 kStatusCancelled = 0xC0000120u;
    static constexpr u32 kStatusPipeBroken = 0xC000014Bu;
    static constexpr GuestAddr kPayload = 0x400000;
    static constexpr GuestAddr kReadBuf = 0x600000;
    static constexpr GuestAddr kReadCount = 0x10700;
    static constexpr GuestAddr kOvBase = 0x30000;
    static constexpr u32 kOvStride = 32;
    static constexpr u32 kRegionSize = 1u << 20;

    void open_pipe(const char* tag) {
        fm = std::make_unique<FileManager>(vfs, handles, guest_mem);
        handles.set_close_callback([this](Handle h, HandleType type,
                                          void* object) {
            return fm != nullptr && fm->on_handle_close(h, type, object);
        });
        const std::string pipe = std::string("\\\\.\\pipe\\vtm_ws_") + tag +
                                 "_" + std::to_string(::getpid());
        server = fm->create_named_pipe_server(pipe);
        ASSERT_NE(server, INVALID_HANDLE);
        client = fm->create_file(pipe, GENERIC_READ | GENERIC_WRITE, 0,
                                 OPEN_EXISTING, kFileFlagOverlapped);
        ASSERT_NE(client, INVALID_HANDLE);
        ASSERT_EQ(fm->connect_named_pipe(server, 2000),
                  PipeConnectResult::AlreadyConnected);
        fm->set_overlapped_completion_callback(
            [this](Handle h, u32 bytes, GuestAddr ov, bool is_64bit,
                   u32 status, u32 error, bool) {
                std::lock_guard<std::mutex> lock(mutex);
                completions.push_back({h, bytes, ov, is_64bit, status, error});
                cv.notify_all();
            });
        ASSERT_TRUE(guest_mem.map_region(kPayload, kRegionSize,
                                         MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(guest_mem.map_region(kReadBuf, kRegionSize,
                                         MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(guest_mem.map_region(kOvBase, 0x10000,
                                         MemoryProtection::ReadWrite).is_ok());
    }

    void TearDown() override {
        if (fm) {
            (void)fm->close_file(client);
            (void)fm->close_file(server);
            fm->shutdown_overlapped_pipe_io();
            fm->clear_overlapped_completion_callback();
            fm.reset();
        }
        FileConsoleBridgeTest::TearDown();
    }

    GuestAddr ov(u32 i) const { return kOvBase + i * kOvStride; }

    OverlappedPipeIoResult write(u32 i, u32 size) {
        u32 written = 0;
        return fm->begin_overlapped_pipe_write(client, kPayload, size, 0,
                                               ov(i), GetParam(), written);
    }

    /// Write until one request pends; returns how many were issued.
    u32 fill_until_pending(u32 size) {
        for (u32 i = 0; i < 64; ++i) {
            if (write(i, size) == OverlappedPipeIoResult::Pending) return i + 1;
        }
        ADD_FAILURE() << "no write pended";
        return 0;
    }

    bool wait_for(usize count) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(10),
                           [&] { return completions.size() >= count; });
    }

    u32 drain_server(u32 bytes) {
        u32 drained = 0;
        while (drained < bytes) {
            const u32 want = std::min(bytes - drained, kRegionSize);
            if (!fm->read_file(server, kReadBuf, want, kReadCount)) break;
            const u32 got = guest_mem.read_u32(kReadCount).value();
            if (got == 0) break;
            drained += got;
        }
        return drained;
    }

    std::unique_ptr<FileManager> fm;
    Handle server{INVALID_HANDLE};
    Handle client{INVALID_HANDLE};
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<PipeWriteCompletion> completions;
};

// 1000 writes queued behind a full pipe complete in issue order with their
// full byte counts, the data reaches the reader in order, and no host thread
// is created per operation.
TEST_P(OverlappedPipeWriteServiceTest, ThousandPendedWritesCompleteInOrder) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    open_pipe("order");
    constexpr u32 kWrites = 1000;
    constexpr u32 kSize = 4096;
    const u32 issued = fill_until_pending(kSize);
    ASSERT_GT(issued, 0u);
    const int threads_after_first_pend = host_thread_count();
    ASSERT_GT(threads_after_first_pend, 0);

    for (u32 i = issued; i < kWrites; ++i) {
        // Each payload is tagged with its index to verify transport order.
        ASSERT_TRUE(guest_mem.write_u32(kPayload, i).is_ok());
        u32 written = 0;
        ASSERT_EQ(fm->begin_overlapped_pipe_write(client, kPayload, kSize, 0,
                                                  ov(i), GetParam(),
                                                  written),
                  OverlappedPipeIoResult::Pending);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);
        const u64 internal = GetParam()
            ? guest_mem.read_u64(ov(i)).value()
            : guest_mem.read_u32(ov(i)).value();
        EXPECT_EQ(internal, 0x103u) << "Internal = STATUS_PENDING";
    }
    EXPECT_EQ(host_thread_count(), threads_after_first_pend);
    EXPECT_EQ(fm->overlapped_pipe_io_worker_count(), 1u);

    std::vector<u8> stream;
    stream.reserve(kWrites * kSize);
    while (stream.size() < kWrites * kSize) {
        ASSERT_TRUE(fm->read_file(server, kReadBuf, kSize, kReadCount));
        const u32 got = guest_mem.read_u32(kReadCount).value();
        ASSERT_GT(got, 0u);
        std::vector<u8> chunk(got);
        ASSERT_TRUE(guest_mem.read_bytes(kReadBuf,
            reinterpret_cast<Byte*>(chunk.data()), got).is_ok());
        stream.insert(stream.end(), chunk.begin(), chunk.end());
    }
    ASSERT_TRUE(wait_for(kWrites - issued + 1));
    EXPECT_EQ(host_thread_count(), threads_after_first_pend);

    for (u32 i = issued; i < kWrites; ++i) {
        u32 tag = 0;
        std::memcpy(&tag, stream.data() + static_cast<usize>(i) * kSize, 4);
        ASSERT_EQ(tag, i) << "write " << i << " reached the pipe out of order";
    }
    std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(completions.size(), kWrites - issued + 1);
    for (usize k = 0; k < completions.size(); ++k) {
        const auto& c = completions[k];
        EXPECT_EQ(c.overlapped, ov(static_cast<u32>(issued - 1 + k)))
            << "completion " << k << " out of issue order";
        EXPECT_EQ(c.handle, client);
        EXPECT_EQ(c.bytes, kSize);
        EXPECT_EQ(c.is_64bit, GetParam());
        EXPECT_EQ(c.nt_status, 0u);
        EXPECT_EQ(c.win_error, 0u);
    }
}

// CancelIoEx on one queued request aborts it with STATUS_CANCELLED /
// ERROR_OPERATION_ABORTED at once, while requests around it still complete
// and the cancelled bytes never reach the reader.
TEST_P(OverlappedPipeWriteServiceTest, CancelWhilePendingAbortsOnlyThatRequest) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    open_pipe("cancel");
    constexpr u32 kSize = 4096;
    const u32 head = fill_until_pending(kSize);
    ASSERT_GT(head, 0u);
    ASSERT_EQ(write(head, kSize), OverlappedPipeIoResult::Pending);
    ASSERT_EQ(write(head + 1, kSize), OverlappedPipeIoResult::Pending);

    EXPECT_EQ(fm->cancel_overlapped_pipe_io(client, ov(head), false), 1u);
    ASSERT_TRUE(wait_for(1));
    {
        std::lock_guard<std::mutex> lock(mutex);
        EXPECT_EQ(completions[0].overlapped, ov(head));
        EXPECT_EQ(completions[0].bytes, 0u);
        EXPECT_EQ(completions[0].nt_status, kStatusCancelled);
        EXPECT_EQ(completions[0].win_error,
                  error_codes::ERROR_OPERATION_ABORTED);
    }
    // Everything except the cancelled request reaches the reader.
    EXPECT_EQ(drain_server(head * kSize + kSize), head * kSize + kSize);
    ASSERT_TRUE(wait_for(3));
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(completions[1].overlapped, ov(head - 1));
    EXPECT_EQ(completions[1].bytes, kSize);
    EXPECT_EQ(completions[2].overlapped, ov(head + 1));
    EXPECT_EQ(completions[2].bytes, kSize);
    EXPECT_EQ(completions[2].nt_status, 0u);
}

// Closing the writing handle cancels its pending writes (IRP_MJ_CLEANUP):
// each completes with STATUS_CANCELLED / ERROR_OPERATION_ABORTED.
TEST_P(OverlappedPipeWriteServiceTest, CloseWhilePendingAbortsRequests) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    open_pipe("close");
    constexpr u32 kSize = 4096;
    const u32 head = fill_until_pending(kSize);
    ASSERT_GT(head, 0u);
    ASSERT_EQ(write(head, kSize), OverlappedPipeIoResult::Pending);
    ASSERT_TRUE(fm->close_file(client));
    ASSERT_TRUE(wait_for(2));
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& c : completions) {
        EXPECT_EQ(c.nt_status, kStatusCancelled);
        EXPECT_EQ(c.win_error, error_codes::ERROR_OPERATION_ABORTED);
        EXPECT_EQ(c.bytes, 0u);
    }
}

// The reader closing its end fails every queued write with
// STATUS_PIPE_BROKEN / ERROR_BROKEN_PIPE.
TEST_P(OverlappedPipeWriteServiceTest, ReaderCloseBreaksPendingWrites) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    open_pipe("broken");
    constexpr u32 kSize = 4096;
    const u32 head = fill_until_pending(kSize);
    ASSERT_GT(head, 0u);
    ASSERT_EQ(write(head, kSize), OverlappedPipeIoResult::Pending);
    ASSERT_TRUE(fm->close_file(server));
    ASSERT_TRUE(wait_for(2));
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& c : completions) {
        EXPECT_EQ(c.handle, client);
        EXPECT_EQ(c.bytes, 0u);
        EXPECT_EQ(c.nt_status, kStatusPipeBroken);
        EXPECT_EQ(c.win_error, error_codes::ERROR_BROKEN_PIPE);
    }
}

INSTANTIATE_TEST_SUITE_P(Bitness, OverlappedPipeWriteServiceTest,
                         ::testing::Values(false, true),
                         [](const auto& info) {
                             return info.param ? "x64" : "x86";
                         });

// Microbenchmark (VITREO_PIPE_WRITE_BENCH=1): host cost of submitting a write
// that pends, and latency from submission to its completion while the reader
// drains the pipe as fast as it can.
TEST_F(FileConsoleBridgeTest, OverlappedNamedPipePendedWriteLatencyBench) {
    if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC=1";
    if (std::getenv("VITREO_PIPE_WRITE_BENCH") == nullptr) {
        GTEST_SKIP() << "set VITREO_PIPE_WRITE_BENCH=1";
    }
    constexpr u32 kFileFlagOverlapped = 0x40000000u;
    constexpr int kIters = 400;
    constexpr u32 kPayloadSize = 64 * 1024;
    constexpr GuestAddr kPayload = 0x400000;
    constexpr GuestAddr kReadBuf = 0x500000;
    constexpr GuestAddr kOverlapped = 0x10400;
    constexpr GuestAddr kReadCount = 0x10700;

    using Clock = std::chrono::steady_clock;
    std::mutex completion_mutex;
    std::condition_variable completion_cv;
    int completed = 0;
    Clock::time_point completed_at;

    FileManager fm(vfs, handles, guest_mem);
    const std::string pipe =
        "\\\\.\\pipe\\vtm_overlapped_bench_" + std::to_string(::getpid());
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
            completed_at = Clock::now();
            ++completed;
            completion_cv.notify_all();
        });
    ASSERT_TRUE(guest_mem.map_region(
        kPayload, kPayloadSize, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(guest_mem.map_region(
        kReadBuf, kPayloadSize, MemoryProtection::ReadWrite).is_ok());

    std::vector<double> submit_us;
    std::vector<double> total_us;
    int sync_completions = 0;
    for (int i = 0; i < kIters; ++i) {
        u32 written = 0;
        const auto start = Clock::now();
        const auto result = fm.begin_overlapped_pipe_write(
            client, kPayload, kPayloadSize, 0, kOverlapped, false, written);
        const auto submitted = Clock::now();
        u32 drained = 0;
        while (drained < kPayloadSize) {
            ASSERT_TRUE(fm.read_file(server, kReadBuf, kPayloadSize - drained,
                                     kReadCount));
            drained += guest_mem.read_u32(kReadCount).value();
        }
        if (result == OverlappedPipeIoResult::Completed) {
            ++sync_completions;
            continue;
        }
        ASSERT_EQ(result, OverlappedPipeIoResult::Pending);
        std::unique_lock<std::mutex> lock(completion_mutex);
        ASSERT_TRUE(completion_cv.wait_for(
            lock, std::chrono::seconds(5),
            [&] { return completed == i + 1 - sync_completions; }));
        submit_us.push_back(
            std::chrono::duration<double, std::micro>(submitted - start)
                .count());
        total_us.push_back(
            std::chrono::duration<double, std::micro>(completed_at - start)
                .count());
    }
    ASSERT_FALSE(submit_us.empty());
    auto pct = [](std::vector<double> v, double p) {
        std::sort(v.begin(), v.end());
        return v[static_cast<usize>(p * static_cast<double>(v.size() - 1))];
    };
    std::printf("[pipe-write-bench] pended=%zu sync=%d submit_us p50=%.1f "
                "p90=%.1f p99=%.1f | submit->complete_us p50=%.1f p90=%.1f "
                "p99=%.1f\n",
                submit_us.size(), sync_completions, pct(submit_us, 0.5),
                pct(submit_us, 0.9), pct(submit_us, 0.99), pct(total_us, 0.5),
                pct(total_us, 0.9), pct(total_us, 0.99));
}

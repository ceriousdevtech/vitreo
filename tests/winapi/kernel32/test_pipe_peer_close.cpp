/// @file test_pipe_peer_close.cpp
/// @brief A pending overlapped pipe read completes when the peer end closes.
///
/// Microsoft Learn, ReadFile "Pipes": when the write end of a pipe has been
/// closed, ReadFile returns FALSE and GetLastError returns ERROR_BROKEN_PIPE.
/// For an outstanding overlapped read that failure is delivered as the
/// operation's completion: OVERLAPPED.Internal holds the NTSTATUS
/// (STATUS_PIPE_BROKEN, ntstatus.h 0xC000014B), hEvent is signaled, and an
/// associated port gets a packet that GetQueuedCompletionStatus returns as
/// FALSE with a non-null *lpOverlapped and the I/O error in GetLastError
/// (Learn, GetQueuedCompletionStatus Remarks). OVERLAPPED field offsets are
/// from minwinbase.h: x86 Internal/InternalHigh/hEvent at 0/4/16, x64 at 0/8/24.
///
/// Every case runs the production path: FileManager's overlapped pipe-read
/// worker over the AF_UNIX backing used for cross-process pipes, delivered
/// through Kernel32ExtManager's real completion routing and API thunks.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <ipc/ipc_gate.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/synchronization/sync.hpp>
#include <winapi/kernel32/thread/thread.hpp>

#include <string>
#include <sys/wait.h>
#include <tuple>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kRegion = 0x40000;
constexpr GuestAddr kStack = kRegion + 0x800;
constexpr GuestAddr kOverlapped = kRegion + 0x1000;
constexpr GuestAddr kBuffer = kRegion + 0x1100;
constexpr GuestAddr kBytesOut = kRegion + 0x1200;
constexpr GuestAddr kKeyOut = kRegion + 0x1208;
constexpr GuestAddr kOverlappedOut = kRegion + 0x1210;
constexpr u32 kBufferSize = 64;
constexpr u64 kCompletionKey = 0x5A17;
constexpr u32 kStatusPipeBroken = 0xC000014Bu;
constexpr u32 kStatusPending = 0x00000103u;
constexpr u32 kFileFlagOverlapped = 0x40000000u;
constexpr u32 kOpenExisting = 3;

/// (x64, server_closes, message_mode, iocp)
using PeerCloseParam = std::tuple<bool, bool, bool, bool>;

class PipePeerCloseTest : public ::testing::TestWithParam<PeerCloseParam> {
protected:
    GuestMemory memory;
    capsule::Vfs vfs;
    HandleTable handles;
    SyncManager sync{handles};
    ThreadManager threads{handles};
    FileManager files{vfs, handles, memory};
    Kernel32ExtManager mgr{memory, vfs};

    bool x64() const { return std::get<0>(GetParam()); }
    bool server_closes() const { return std::get<1>(GetParam()); }
    bool message_mode() const { return std::get<2>(GetParam()); }
    bool iocp() const { return std::get<3>(GetParam()); }

    void SetUp() override {
        if (!ipc::ipc_enabled()) GTEST_SKIP() << "requires VITREO_IPC";
        ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
        handles.set_close_callback([this](Handle h, HandleType type, void* object) {
            return files.on_handle_close(h, type, object);
        });
        mgr.set_handle_table(&handles);
        mgr.set_sync_manager(&sync);
        mgr.set_thread_manager(&threads);
        mgr.set_file_manager(&files);
    }

    void TearDown() override {
        files.shutdown_overlapped_pipe_io();
        files.clear_overlapped_completion_callback();
        handles.set_close_callback(nullptr);
    }

    std::string pipe_name(const char* tag) const {
        return std::string("\\\\.\\pipe\\vitreo_peer_close_") + tag + "_" +
               std::to_string(::getpid()) + "_" +
               ::testing::UnitTest::GetInstance()->current_test_info()->name();
    }

    NamedPipeConfig config() const {
        NamedPipeConfig c;
        c.overlapped = true;
        c.message_type = message_mode();
        c.message_read_mode = message_mode();
        return c;
    }

    u64 call(void (Kernel32ExtManager::*fn)(Marshaler&),
             const std::vector<u64>& args) {
        cpu::CpuState cpu;
        if (x64()) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kStack);
        if (x64()) {
            EXPECT_TRUE(memory.write_u64(kStack, 0xDEADBEEFull).is_ok());
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else EXPECT_TRUE(memory.write_u64(
                    kStack + 0x28 + (i - 4) * 8, args[i]).is_ok());
            }
        } else {
            EXPECT_TRUE(memory.write_u32(kStack, 0xDEADBEEFu).is_ok());
            for (usize i = 0; i < args.size(); ++i) {
                EXPECT_TRUE(memory.write_u32(kStack + 4 + i * 4,
                                             static_cast<u32>(args[i])).is_ok());
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        (mgr.*fn)(m);
        return x64() ? cpu.gpr.rax : static_cast<u32>(cpu.gpr.rax);
    }

    u64 read_ptr(GuestAddr at) const {
        return x64() ? memory.read_u64(at).value_or(~0ull)
                     : memory.read_u32(at).value_or(~0u);
    }
    void write_ptr(GuestAddr at, u64 v) {
        ASSERT_TRUE((x64() ? memory.write_u64(at, v)
                           : memory.write_u32(at, static_cast<u32>(v))).is_ok());
    }
    GuestAddr event_field() const { return kOverlapped + (x64() ? 24 : 16); }
    GuestAddr internal_high_field() const { return kOverlapped + (x64() ? 8 : 4); }

    /// Prepare the reader's delivery route: either associate it with a fresh
    /// port, or give the OVERLAPPED a manual-reset event. Returns the port
    /// (IOCP) or event handle.
    Handle prepare_route(Handle reader) {
        for (GuestAddr a = kOverlapped; a < kOverlapped + 32; a += 4) {
            EXPECT_TRUE(memory.write_u32(a, 0).is_ok());
        }
        if (iocp()) {
            const Handle port = static_cast<Handle>(call(
                &Kernel32ExtManager::impl_CreateIoCompletionPort,
                {x64() ? ~0ull : 0xFFFFFFFFull, 0, 0, 0}));
            EXPECT_NE(port, NULL_HANDLE);
            EXPECT_EQ(call(&Kernel32ExtManager::impl_CreateIoCompletionPort,
                           {reader, port, kCompletionKey, 0}),
                      port);
            return port;
        }
        const Handle event = sync.create_event(true, false, "");
        EXPECT_NE(event, NULL_HANDLE);
        write_ptr(event_field(), event);
        return event;
    }

    void begin_pending_read(Handle reader) {
        u32 count = 0xFFFFFFFFu;
        ASSERT_EQ(files.begin_overlapped_pipe_read(reader, kBuffer, kBufferSize,
                                                   kOverlapped, x64(), count),
                  OverlappedPipeIoResult::Pending);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_IO_PENDING);
        EXPECT_EQ(read_ptr(kOverlapped), kStatusPending);
    }

    /// The completion every route must observe after the peer closed.
    void expect_broken_pipe_completion(Handle route, Handle reader) {
        if (iocp()) {
            write_ptr(kOverlappedOut, 0);
            ASSERT_TRUE(memory.write_u32(kBytesOut, 0xFFFFFFFFu).is_ok());
            set_last_error(0);
            const u64 ok = call(&Kernel32ExtManager::impl_GetQueuedCompletionStatus,
                                {route, kBytesOut, kKeyOut, kOverlappedOut, 5000});
            EXPECT_EQ(ok, 0u);
            EXPECT_EQ(read_ptr(kOverlappedOut), kOverlapped)
                << "no packet: the pending read never completed";
            EXPECT_EQ(get_last_error(), error_codes::ERROR_BROKEN_PIPE);
            EXPECT_EQ(memory.read_u32(kBytesOut).value_or(1), 0u);
            EXPECT_EQ(read_ptr(kKeyOut), kCompletionKey);
        } else {
            ASSERT_EQ(sync.wait_for_single_object(route, 5000), WAIT_OBJECT_0)
                << "hEvent never signaled: the pending read never completed";
            ASSERT_TRUE(memory.write_u32(kBytesOut, 0xFFFFFFFFu).is_ok());
            set_last_error(0);
            // GetOverlappedResult(hFile, lpOverlapped, lpBytes, bWait=FALSE)
            EXPECT_EQ(call(&Kernel32ExtManager::impl_GetOverlappedResult,
                           {reader, kOverlapped, kBytesOut, 0}), 0u);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_BROKEN_PIPE);
            EXPECT_EQ(memory.read_u32(kBytesOut).value_or(1), 0u);
        }
        EXPECT_EQ(read_ptr(kOverlapped), kStatusPipeBroken);
        EXPECT_EQ(read_ptr(internal_high_field()), 0u);
    }

    /// After the peer is gone a new read fails at once (Learn: FALSE +
    /// ERROR_BROKEN_PIPE) and is never queued as a completion.
    void expect_new_read_fails(Handle reader) {
        u32 count = 0xFFFFFFFFu;
        set_last_error(0);
        EXPECT_EQ(files.begin_overlapped_pipe_read(reader, kBuffer, kBufferSize,
                                                   kOverlapped, x64(), count),
                  OverlappedPipeIoResult::Failed);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_BROKEN_PIPE);
        EXPECT_EQ(count, 0u);
        set_last_error(0);
        EXPECT_FALSE(files.read_file(reader, kBuffer, kBufferSize, 0, &count));
        EXPECT_EQ(get_last_error(), error_codes::ERROR_BROKEN_PIPE);
    }
};

TEST_P(PipePeerCloseTest, InProcessPeerCloseCompletesPendingRead) {
    const std::string name = pipe_name("inproc");
    const Handle server = files.create_named_pipe_server(name, false, config());
    ASSERT_NE(server, INVALID_HANDLE);
    const Handle client = files.create_file(name, 0xC0000000u, 0, kOpenExisting,
                                            kFileFlagOverlapped);
    ASSERT_NE(client, INVALID_HANDLE);
    (void)files.connect_named_pipe(server, 1000);

    const Handle reader = server_closes() ? client : server;
    const Handle closer = server_closes() ? server : client;
    const Handle route = prepare_route(reader);
    begin_pending_read(reader);
    ASSERT_TRUE(files.close_file(closer));
    expect_broken_pipe_completion(route, reader);
    expect_new_read_fails(reader);
    EXPECT_TRUE(files.close_file(reader));
}

/// The peer is another host process, as in a Chromium browser/renderer pair:
/// the pending read must complete when that process closes its end or exits.
TEST_P(PipePeerCloseTest, CrossProcessPeerExitCompletesPendingRead) {
    const std::string name = pipe_name("xproc");
    int ready_pipe[2] = {-1, -1};
    int go_pipe[2] = {-1, -1};
    ASSERT_EQ(::pipe(ready_pipe), 0);
    ASSERT_EQ(::pipe(go_pipe), 0);

    // server_closes: the child owns the server and the parent reads on the
    // client (renderer view of the browser going away); otherwise reversed.
    Handle local = INVALID_HANDLE;
    if (!server_closes()) {
        local = files.create_named_pipe_server(name, false, config());
        ASSERT_NE(local, INVALID_HANDLE);
    }
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(go_pipe[1]);
        HandleTable child_handles;
        GuestMemory child_memory;
        FileManager child_files(vfs, child_handles, child_memory);
        Handle remote = INVALID_HANDLE;
        if (server_closes()) {
            NamedPipeConfig c = config();
            remote = child_files.create_named_pipe_server(name, false, c);
            if (remote == INVALID_HANDLE) _exit(20);
            const char listening = 'L';
            if (::write(ready_pipe[1], &listening, 1) != 1) _exit(21);
            const auto connected = child_files.connect_named_pipe(remote, 5000);
            if (connected != PipeConnectResult::Connected &&
                connected != PipeConnectResult::AlreadyConnected) {
                _exit(22);
            }
        } else {
            remote = child_files.create_file(name, 0xC0000000u, 0,
                                             kOpenExisting, kFileFlagOverlapped);
            if (remote == INVALID_HANDLE) _exit(23);
        }
        const char ready = 'R';
        if (::write(ready_pipe[1], &ready, 1) != 1) _exit(24);
        char go = 0;
        if (::read(go_pipe[0], &go, 1) != 1) _exit(25);
        // Exit with the endpoint still open: process teardown is the close.
        _exit(0);
    }
    ::close(ready_pipe[1]);
    ::close(go_pipe[0]);
    char byte = 0;
    if (server_closes()) {
        ASSERT_EQ(::read(ready_pipe[0], &byte, 1), 1);
        ASSERT_EQ(byte, 'L');
        local = files.create_file(name, 0xC0000000u, 0, kOpenExisting,
                                  kFileFlagOverlapped);
        ASSERT_NE(local, INVALID_HANDLE);
    }
    ASSERT_EQ(::read(ready_pipe[0], &byte, 1), 1);
    ASSERT_EQ(byte, 'R');
    if (!server_closes()) (void)files.connect_named_pipe(local, 2000);

    const Handle route = prepare_route(local);
    begin_pending_read(local);
    const char go = 'G';
    ASSERT_EQ(::write(go_pipe[1], &go, 1), 1);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    ::close(ready_pipe[0]);
    ::close(go_pipe[1]);

    expect_broken_pipe_completion(route, local);
    expect_new_read_fails(local);
    EXPECT_TRUE(files.close_file(local));
}

std::string param_name(const ::testing::TestParamInfo<PeerCloseParam>& info) {
    const auto [x64, server_closes, message, iocp] = info.param;
    return std::string(x64 ? "x64" : "x86") +
           (server_closes ? "_ServerCloses" : "_ClientCloses") +
           (message ? "_Message" : "_Byte") + (iocp ? "_Iocp" : "_Event");
}

INSTANTIATE_TEST_SUITE_P(AllRoutes, PipePeerCloseTest,
                         ::testing::Combine(::testing::Bool(), ::testing::Bool(),
                                            ::testing::Bool(), ::testing::Bool()),
                         param_name);

} // namespace

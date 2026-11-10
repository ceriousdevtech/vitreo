// FlushFileBuffers: Windows disk access results, honest unsupported pipe drain,
// and real HostFileSystem flush failure propagation through the VFS boundary.
#include <gtest/gtest.h>
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "../../../tools/oracle_probe/flush_buffers.generated.inc"
#include <filesystem>
#include <fstream>
#include <memory>
#include <unistd.h>
using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::capsule;
namespace {
namespace sdk = flush_buffers_sdk;
class FlushFileSystem : public HostFileSystem {
public:
    using HostFileSystem::HostFileSystem;
    bool fail_flush{};
    unsigned calls{};
    Result<void, VfsError> flush(FileHandle handle) override {
        ++calls;
        if (fail_flush) return Result<void, VfsError>::err(VfsError::AccessDenied);
        return HostFileSystem::flush(handle);
    }
};
class FlushBuffersContract : public ::testing::TestWithParam<bool> {
protected:
    GuestMemory memory;
    Vfs vfs;
    HandleTable handles;
    Kernel32ExtManager manager{memory, vfs};
    std::unique_ptr<FileManager> files;
    FlushFileSystem* fs{};
    std::filesystem::path root;
    void SetUp() override {
        char name[] = "/tmp/vitreo-flush-contract-XXXXXX";
        const auto temp = ::mkdtemp(name); ASSERT_NE(temp, nullptr); root = temp;
        std::ofstream(root / "data.bin", std::ios::binary).write("flush-data", 10);
        auto backing = std::make_unique<FlushFileSystem>(root.string()); fs = backing.get();
        ASSERT_TRUE(vfs.mount_drive('C', std::move(backing)).is_ok());
        ASSERT_TRUE(memory.map_region(0x10000, 0x5000, MemoryProtection::ReadWrite).is_ok());
        files = std::make_unique<FileManager>(vfs, handles, memory); manager.set_file_manager(files.get());
    }
    void TearDown() override { files.reset(); std::filesystem::remove_all(root); }
    Handle open(u32 access) {
        return files->create_file("C:\\data.bin", access,
            sdk::FILE_SHARE_READ | sdk::FILE_SHARE_WRITE, OPEN_EXISTING, 0);
    }
    bool flush(Handle handle) {
        cpu::CpuState state;
        if (GetParam()) { state.init_long_mode(); state.gpr.rcx = handle; state.gpr.rsp = 0x14000; }
        else { state.init_protected_mode(); state.set_sp(0x14000); (void)memory.write_u32(0x14004, handle); }
        Marshaler marshaler(memory); marshaler.attach_cpu(state); set_last_error(0xdeadbeef);
        manager.impl_FlushFileBuffers(marshaler); return state.gpr.rax != 0;
    }
};
TEST_P(FlushBuffersContract, WindowsDiskAccessAndSuccessfulLastError) {
    // Expected access outcomes: flush_buffers_probe{32,64}.expected disk_*.
    for (const u32 access : {sdk::GENERIC_WRITE, sdk::GENERIC_READ | sdk::GENERIC_WRITE, sdk::FILE_APPEND_DATA}) {
        const auto handle = open(access); ASSERT_NE(handle, INVALID_HANDLE);
        const auto calls = fs->calls; EXPECT_TRUE(flush(handle));
        EXPECT_EQ(get_last_error(), 0xdeadbeefu); EXPECT_EQ(fs->calls, calls + 1);
    }
    for (const u32 access : {sdk::GENERIC_READ, 0u}) {
        const auto handle = open(access); ASSERT_NE(handle, INVALID_HANDLE);
        const auto calls = fs->calls; EXPECT_FALSE(flush(handle));
        EXPECT_EQ(get_last_error(), sdk::ERROR_ACCESS_DENIED); EXPECT_EQ(fs->calls, calls);
    }
}
TEST_P(FlushBuffersContract, VfsFlushFailureIsNeverAcknowledgedAsSuccess) {
    const auto handle = open(sdk::GENERIC_WRITE); ASSERT_NE(handle, INVALID_HANDLE);
    fs->fail_flush = true; EXPECT_FALSE(flush(handle));
    // AccessDenied injected from the backing filesystem, not the handle mask.
    EXPECT_EQ(get_last_error(), sdk::ERROR_ACCESS_DENIED); EXPECT_EQ(fs->calls, 1u);
    fs->fail_flush = false; EXPECT_TRUE(flush(handle)); EXPECT_EQ(fs->calls, 2u);
}
TEST_P(FlushBuffersContract, NonFileObjectsCannotBecomeWritableFiles) {
    for (const Handle handle : {NULL_HANDLE, INVALID_HANDLE}) {
        EXPECT_FALSE(flush(handle)); EXPECT_EQ(get_last_error(), sdk::ERROR_INVALID_HANDLE);
    }
    for (const HandleType type : {HandleType::Event, HandleType::Process, HandleType::Thread,
        HandleType::Mutex, HandleType::Semaphore, HandleType::FileMapping, HandleType::ConsoleInput,
        HandleType::ConsoleOutput, HandleType::Registry, HandleType::Heap, HandleType::Timer,
        HandleType::Module, HandleType::Token, HandleType::Job, HandleType::IoCompletionPort,
        HandleType::NetworkNotification, HandleType::SharedResource}) {
        const auto handle = handles.allocate(type, nullptr, 0, sdk::GENERIC_WRITE);
        EXPECT_FALSE(flush(handle)) << handle_type_to_string(type);
        EXPECT_EQ(get_last_error(), sdk::ERROR_INVALID_HANDLE);
    }
    EXPECT_EQ(fs->calls, 0u);
}
TEST_P(FlushBuffersContract, RealAnonymousPipeReadDeniesAndWriteFailsExplicitlyUntilDrainExists) {
    Handle reader{}, writer{}; ASSERT_TRUE(files->create_anonymous_pipe(reader, writer, false));
    EXPECT_FALSE(flush(reader)); EXPECT_EQ(get_last_error(), sdk::ERROR_ACCESS_DENIED);
    EXPECT_FALSE(flush(writer)); EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    EXPECT_EQ(fs->calls, 0u);
}
TEST_P(FlushBuffersContract, UnbackedWritableFilesAndDevicesFailExplicitly) {
    for (const HandleType type : {HandleType::File, HandleType::Device, HandleType::HidDevice, HandleType::Mailslot}) {
        const auto handle = handles.allocate(type, nullptr, 0, sdk::GENERIC_WRITE);
        EXPECT_FALSE(flush(handle)); EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    }
    EXPECT_EQ(fs->calls, 0u);
}
TEST_P(FlushBuffersContract, HostBackendActuallyInvokesSyncWithoutEnvironmentOptIn) {
    // Apple fsync(2): a pipe is not a flushable filesystem descriptor (EINVAL).
    // Adopt one at the backend boundary to distinguish a real host fsync call
    // from the former default-success VITREO_FSYNC opt-out. This does not
    // substitute POSIX fsync for the Windows peer-consumption pipe contract.
    int fds[2]{}; ASSERT_EQ(::pipe(fds), 0);
    const auto adopted = fs->adopt_native_fd(fds[1], AccessMode::Write, ShareMode::All, "flush-test-pipe");
    ::close(fds[0]);
    if (adopted.is_err()) ::close(fds[1]);
    ASSERT_TRUE(adopted.is_ok());
    EXPECT_TRUE(fs->flush(adopted.value()).is_err());
    EXPECT_TRUE(fs->close(adopted.value()).is_ok());
}
INSTANTIATE_TEST_SUITE_P(BothGuestAbis, FlushBuffersContract, ::testing::Bool());
} // namespace

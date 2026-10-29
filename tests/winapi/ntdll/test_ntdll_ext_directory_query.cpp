// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_ntdll_ext_directory_query.cpp
/// @brief Unit tests for ntdll extended functions (WIN32-086): NtQueryDirectoryFile, NtReadFile and NtWriteFile

#include "ntdll_ext_test_fixture.hpp"

namespace {

u32 read_le_u32(const std::vector<u8>& buffer, usize offset) {
    return static_cast<u32>(buffer[offset]) |
           (static_cast<u32>(buffer[offset + 1]) << 8) |
           (static_cast<u32>(buffer[offset + 2]) << 16) |
           (static_cast<u32>(buffer[offset + 3]) << 24);
}

std::vector<u16> read_wide_name(const std::vector<u8>& buffer,
                                usize offset, usize byte_length) {
    std::vector<u16> result;
    for (usize index = 0; index + 1 < byte_length; index += 2) {
        result.push_back(static_cast<u16>(buffer[offset + index]) |
                         static_cast<u16>(buffer[offset + index + 1] << 8));
    }
    return result;
}

class NtdllDirectoryQueryTest : public ::testing::Test {
protected:
    void SetUp() override {
        char path_template[] = "/tmp/vitreo_ntquerydir_XXXXXX";
        char* root = ::mkdtemp(path_template);
        ASSERT_NE(root, nullptr);
        root_ = root;
        std::filesystem::create_directories(root_ + "/folder/subdir");
        std::ofstream(root_ + "/folder/alpha.txt") << "alpha";
        std::ofstream(root_ + "/folder/beta.bin") << "beta";
        std::ofstream(root_ + "/folder/very-long-filename.data") << "long";
        ASSERT_TRUE(vfs_.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root_)).is_ok());
        manager_.set_vfs(&vfs_);
        manager_.set_handle_table(&handles_);
        handles_.set_close_callback(
            [this](Handle handle, HandleType type, void*) {
                return manager_.on_handle_close(handle, type);
            });
    }

    void TearDown() override {
        for (const Handle handle : handles_.snapshot_handles()) {
            (void)handles_.close(handle);
        }
        std::filesystem::remove_all(root_);
    }

    Handle open_directory(u32 create_options = nt_file::FILE_DIRECTORY_FILE) {
        const auto opened = manager_.nt_create_file(
            "C:\\folder", 0x0001, 0, 0x7, nt_file::FILE_OPEN,
            create_options);
        EXPECT_EQ(opened.status, nt_status::STATUS_SUCCESS);
        return opened.handle;
    }

    std::string root_;
    capsule::Vfs vfs_;
    NtdllExtManager manager_;
    HandleTable handles_;
};

} // namespace

// =============================================================================
// NtQueryDirectoryFile Tests
// =============================================================================

// Learn (ZwQueryDirectoryFile): FileName is honoured on EVERY call, not only
// the first — a new expression restarts the enumeration under that mask. This
// test used to assert the opposite (later expressions ignored). Caching the
// first call's mask made a handle answer every later lookup with the first
// match it had already found, and Electron's module resolver — which probes a
// directory one name at a time with ReturnSingleEntry plus a fresh mask —
// span 3.9 million identical calls on one handle because of it.
TEST_F(NtdllDirectoryQueryTest, HonoursANewExpressionOnEveryCall) {
    const Handle directory = open_directory();
    std::vector<u8> buffer(256, 0);
    const std::optional<std::vector<u16>> txt_expression{
        std::vector<u16>{u'*', u'.', u'T', u'X', u'T'}};

    // Matching is case-insensitive: "*.TXT" finds alpha.txt.
    auto first = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileBothDirectoryInformation,
        false, txt_expression, false);
    ASSERT_EQ(first.status, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(first.information, 112u); // 94-byte header + 9 UTF-16 units.
    EXPECT_EQ(read_le_u32(buffer, 60), 18u);
    EXPECT_EQ(read_wide_name(buffer, 94, 18),
              (std::vector<u16>{u'a', u'l', u'p', u'h', u'a', u'.',
                                u't', u'x', u't'}));

    // A different expression re-filters from the top of the directory.
    const std::optional<std::vector<u16>> bin_expression{
        std::vector<u16>{u'b', u'e', u't', u'a', u'.', u'b', u'i', u'n'}};
    std::fill(buffer.begin(), buffer.end(), 0);
    auto refiltered = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileBothDirectoryInformation,
        false, bin_expression, false);
    ASSERT_EQ(refiltered.status, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(read_wide_name(buffer, 94, read_le_u32(buffer, 60)),
              (std::vector<u16>{u'b', u'e', u't', u'a', u'.',
                                u'b', u'i', u'n'}));

    // Repeating that expression continues the scan rather than restarting it,
    // so the single match is now exhausted.
    std::fill(buffer.begin(), buffer.end(), 0);
    auto exhausted = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileBothDirectoryInformation,
        false, bin_expression, false);
    EXPECT_EQ(exhausted.status, nt_status::STATUS_NO_MORE_FILES);
    EXPECT_EQ(exhausted.information, 0u);

    // RestartScan rewinds without changing the mask.
    std::fill(buffer.begin(), buffer.end(), 0);
    auto restarted = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileBothDirectoryInformation,
        false, std::nullopt, true);
    ASSERT_EQ(restarted.status, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(read_wide_name(buffer, 94, read_le_u32(buffer, 60)),
              (std::vector<u16>{u'b', u'e', u't', u'a', u'.',
                                u'b', u'i', u'n'}));
}

TEST_F(NtdllDirectoryQueryTest, PacksNamesEntriesAtFourByteBoundaries) {
    const Handle directory = open_directory();
    std::vector<u8> buffer(512, 0);
    const auto result = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileNamesInformation,
        false, std::nullopt, false);
    ASSERT_EQ(result.status, nt_status::STATUS_SUCCESS);
    ASSERT_GT(result.information, 0u);

    const u32 first_delta = read_le_u32(buffer, 0);
    ASSERT_NE(first_delta, 0u);
    EXPECT_EQ(first_delta % 4, 0u);
    EXPECT_EQ(read_wide_name(buffer, 12, read_le_u32(buffer, 8)),
              (std::vector<u16>{u'.'}));
    EXPECT_EQ(read_wide_name(buffer, first_delta + 12,
                             read_le_u32(buffer, first_delta + 8)),
              (std::vector<u16>{u'.', u'.'}));
}

TEST_F(NtdllDirectoryQueryTest,
       FirstPartialNameReturnsOverflowAndAdvancesCursor) {
    const Handle directory = open_directory();
    const std::optional<std::vector<u16>> expression{
        std::vector<u16>{u'v', u'e', u'r', u'y', u'-', u'l', u'o', u'n',
                         u'g', u'-', u'f', u'i', u'l', u'e', u'n', u'a',
                         u'm', u'e', u'.', u'd', u'a', u't', u'a'}};
    std::vector<u8> buffer(68, 0);
    const auto partial = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileDirectoryInformation,
        false, expression, false);
    ASSERT_EQ(partial.status, nt_status::STATUS_BUFFER_OVERFLOW);
    EXPECT_EQ(partial.information, 68u);
    EXPECT_EQ(read_le_u32(buffer, 60), 46u); // Full name byte length.
    EXPECT_EQ(read_wide_name(buffer, 64, 4),
              (std::vector<u16>{u'v', u'e'}));

    std::vector<u8> large_buffer(256, 0);
    const auto exhausted = manager_.nt_query_directory_file(
        directory, large_buffer, nt_file::FileDirectoryInformation,
        false, std::nullopt, false);
    EXPECT_EQ(exhausted.status, nt_status::STATUS_NO_MORE_FILES);
}

TEST_F(NtdllDirectoryQueryTest, TooSmallBaseCanBeRetriedWithoutLosingEntry) {
    const Handle directory = open_directory();
    const std::optional<std::vector<u16>> expression{
        std::vector<u16>{u'b', u'e', u't', u'a', u'.', u'b', u'i', u'n'}};
    std::vector<u8> too_small(11, 0);
    const auto failed = manager_.nt_query_directory_file(
        directory, too_small, nt_file::FileNamesInformation,
        true, expression, false);
    EXPECT_EQ(failed.status, nt_status::STATUS_BUFFER_TOO_SMALL);
    EXPECT_EQ(failed.information, 0u);

    std::vector<u8> enough(64, 0);
    const auto retried = manager_.nt_query_directory_file(
        directory, enough, nt_file::FileNamesInformation,
        true, std::nullopt, false);
    ASSERT_EQ(retried.status, nt_status::STATUS_SUCCESS);
    EXPECT_EQ(read_wide_name(enough, 12, read_le_u32(enough, 8)),
              (std::vector<u16>{u'b', u'e', u't', u'a', u'.', u'b',
                                u'i', u'n'}));
}

TEST_F(NtdllDirectoryQueryTest, EmptyFirstQueryAndCloseUseNtStatusesAndVfsLifetime) {
    const Handle directory = open_directory();
    EXPECT_TRUE(manager_.owns_file_handle(directory));
    const std::optional<std::vector<u16>> expression{
        std::vector<u16>{u'n', u'o', u't', u'-', u'p', u'r', u'e', u's',
                         u'e', u'n', u't'}};
    std::vector<u8> buffer(128, 0);
    const auto missing = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileIdFullDirectoryInformation,
        false, expression, false);
    EXPECT_EQ(missing.status, nt_status::STATUS_NO_SUCH_FILE);
    EXPECT_EQ(missing.information, 0u);

    EXPECT_TRUE(handles_.close(directory));
    EXPECT_FALSE(manager_.owns_file_handle(directory));
    const auto after_close = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileIdFullDirectoryInformation,
        false, std::nullopt, false);
    EXPECT_EQ(after_close.status, nt_status::STATUS_INVALID_HANDLE);
}

TEST_F(NtdllDirectoryQueryTest, TracksSynchronousVersusAsynchronousOpenMode) {
    const Handle asynchronous = open_directory();
    EXPECT_TRUE(manager_.is_asynchronous_file_handle(asynchronous));

    const Handle synchronous = open_directory(
        nt_file::FILE_DIRECTORY_FILE |
        nt_file::FILE_SYNCHRONOUS_IO_NONALERT);
    EXPECT_FALSE(manager_.is_asynchronous_file_handle(synchronous));
}

TEST_F(NtdllDirectoryQueryTest, AcceptsDirectoryHandleOpenedByCreateFile) {
    FileManager file_manager(vfs_, handles_);
    manager_.set_file_manager(&file_manager);
    handles_.set_close_callback(
        [this, &file_manager](Handle handle, HandleType type, void* object) {
            manager_.forget_directory_query(handle);
            if (manager_.owns_file_handle(handle)) {
                return manager_.on_handle_close(handle, type);
            }
            return file_manager.on_handle_close(handle, type, object);
        });
    const Handle directory = file_manager.create_file(
        "C:\\folder", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    if (directory == INVALID_HANDLE) {
        manager_.set_file_manager(nullptr);
        handles_.set_close_callback(
            [this](Handle handle, HandleType type, void*) {
                return manager_.on_handle_close(handle, type);
            });
        FAIL() << "CreateFile-style directory open failed";
        return;
    }

    const std::optional<std::vector<u16>> expression{
        std::vector<u16>{u'a', u'l', u'p', u'h', u'a', u'.', u't', u'x', u't'}};
    std::vector<u8> buffer(128, 0);
    const auto result = manager_.nt_query_directory_file(
        directory, buffer, nt_file::FileNamesInformation,
        true, expression, false);
    EXPECT_EQ(result.status, nt_status::STATUS_SUCCESS);
    if (result.status == nt_status::STATUS_SUCCESS) {
        EXPECT_EQ(read_wide_name(buffer, 12, read_le_u32(buffer, 8)),
                  *expression);
    }

    EXPECT_TRUE(handles_.close(directory));
    manager_.set_file_manager(nullptr);
    handles_.set_close_callback(
        [this](Handle handle, HandleType type, void*) {
            return manager_.on_handle_close(handle, type);
        });
}

TEST_F(NtdllDirectoryQueryTest, ExportWritesNativeWidthIoStatusBlockOnX64) {
    const Handle directory = open_directory();
    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kIoStatus = kData + 0x100;
    constexpr GuestAddr kInformation = kData + 0x200;
    constexpr GuestAddr kStack = 0x80000;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = kStack + 0x400;
    cpu.gpr.rcx = directory;
    cpu.gpr.rdx = 0; // Event
    cpu.gpr.r8 = 0;  // ApcRoutine
    cpu.gpr.r9 = 0;  // ApcContext
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kIoStatus).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kInformation).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 128).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x40, nt_file::FileNamesInformation).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x50, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x58, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = make_ntdll_ext_exports(manager_);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtQueryDirectoryFile";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 44u);

    entry->handler(marshaler);

    ASSERT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(kIoStatus).value(), nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u64(kIoStatus + 8).value(), 14u);
    EXPECT_EQ(memory.read_u32(kInformation + 8).value(), 2u);
    EXPECT_EQ(memory.read_u16(kInformation + 12).value(), u'.');
}

TEST_F(NtdllDirectoryQueryTest, NtReadFileSignalsEventAndQueuesApc) {
    // Learn NtReadFile: Event is signaled on completion; ApcRoutine is queued
    // for the issuing thread's next alertable wait.
    const auto opened = manager_.nt_create_file(
        "C:\\folder\\alpha.txt", 0x0001, 0, 0x7, nt_file::FILE_OPEN, 0);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);
    const Handle file = opened.handle;

    SyncManager sync{handles_};
    ThreadManager threads{handles_};
    manager_.set_sync_manager(&sync);
    manager_.set_thread_manager(&threads);
    ThreadManager::set_current_thread_id(1);

    const Handle event = sync.create_event(false, false, "");
    ASSERT_NE(event, 0u);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_TIMEOUT);

    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kIoStatus = kData + 0x100;
    constexpr GuestAddr kBuffer = kData + 0x200;
    constexpr GuestAddr kStack = 0x80000;
    constexpr GuestAddr kApc = 0x5000;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = kStack + 0x400;
    cpu.gpr.rcx = file;
    cpu.gpr.rdx = event;
    cpu.gpr.r8 = kApc;
    cpu.gpr.r9 = 0xABCDu;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kIoStatus).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kBuffer).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 5).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(kIoStatus + 4, 0xA5A5A5A5u).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = make_ntdll_ext_exports(manager_);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtReadFile";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(kIoStatus).value(), nt_status::STATUS_SUCCESS);
    EXPECT_EQ(memory.read_u32(kIoStatus + 4).value(), 0xA5A5A5A5u);
    EXPECT_EQ(memory.read_u64(kIoStatus + 8).value(), 5u);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_OBJECT_0);
    EXPECT_TRUE(threads.has_pending_nt_io_apcs(1));

    GuestAddr seen_pfn = 0;
    GuestAddr seen_ctx = 0;
    GuestAddr seen_iosb = 0;
    EXPECT_EQ(threads.drain_nt_io_apcs(
                  1,
                  [&](GuestAddr pfn, GuestAddr context, GuestAddr iosb) {
                      seen_pfn = pfn;
                      seen_ctx = context;
                      seen_iosb = iosb;
                  }),
              1u);
    EXPECT_EQ(seen_pfn, kApc);
    EXPECT_EQ(seen_ctx, 0xABCDu);
    EXPECT_EQ(seen_iosb, kIoStatus);

    manager_.set_sync_manager(nullptr);
    manager_.set_thread_manager(nullptr);
}

TEST_F(NtdllDirectoryQueryTest, NtWriteFileSignalsEventAndQueuesApc) {
    // Learn NtWriteFile: Event is signaled on completion; ApcRoutine is queued
    // for the issuing thread's next alertable wait.
    const auto opened = manager_.nt_create_file(
        "C:\\folder\\alpha.txt", 0x0002, 0, 0x7, nt_file::FILE_OPEN, 0);
    ASSERT_EQ(opened.status, nt_status::STATUS_SUCCESS);
    const Handle file = opened.handle;

    SyncManager sync{handles_};
    ThreadManager threads{handles_};
    manager_.set_sync_manager(&sync);
    manager_.set_thread_manager(&threads);
    ThreadManager::set_current_thread_id(1);

    const Handle event = sync.create_event(false, false, "");
    ASSERT_NE(event, 0u);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_TIMEOUT);

    GuestMemory memory;
    constexpr GuestAddr kData = 0x10000;
    constexpr GuestAddr kIoStatus = kData + 0x100;
    constexpr GuestAddr kBuffer = kData + 0x200;
    constexpr GuestAddr kStack = 0x80000;
    constexpr GuestAddr kApc = 0x5000;
    ASSERT_TRUE(memory.map_region(
        kData, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u8(kBuffer, 'Z').is_ok());
    ASSERT_TRUE(memory.write_u8(kBuffer + 1, 'Z').is_ok());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = kStack + 0x400;
    cpu.gpr.rcx = file;
    cpu.gpr.rdx = event;
    cpu.gpr.r8 = kApc;
    cpu.gpr.r9 = 0xABCDu;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, kIoStatus).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, kBuffer).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 2).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, 0).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    const auto exports = make_ntdll_ext_exports(manager_);
    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "NtWriteFile";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), nt_status::STATUS_SUCCESS);
    EXPECT_EQ(sync.wait_for_single_object(event, 0), WAIT_OBJECT_0);
    EXPECT_TRUE(threads.has_pending_nt_io_apcs(1));

    GuestAddr seen_pfn = 0;
    GuestAddr seen_ctx = 0;
    GuestAddr seen_iosb = 0;
    EXPECT_EQ(threads.drain_nt_io_apcs(
                  1,
                  [&](GuestAddr pfn, GuestAddr context, GuestAddr iosb) {
                      seen_pfn = pfn;
                      seen_ctx = context;
                      seen_iosb = iosb;
                  }),
              1u);
    EXPECT_EQ(seen_pfn, kApc);
    EXPECT_EQ(seen_ctx, 0xABCDu);
    EXPECT_EQ(seen_iosb, kIoStatus);

    manager_.set_sync_manager(nullptr);
    manager_.set_thread_manager(nullptr);
}

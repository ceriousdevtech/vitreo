/// @file test_file.cpp
/// @brief Unit tests for CreateFileW / CreateFileA (WIN32-010); the console bridge tests (WIN32-089) live in test_file_console_bridge.cpp and the other test_file_*.cpp parts

#include "kernel32_file_fixture.hpp"

// =============================================================================
// to_access_mode Tests
// =============================================================================

TEST_F(FileManagerTest, AccessModeRead) {
    auto mode = FileManager::to_access_mode(GENERIC_READ);
    EXPECT_EQ(mode, AccessMode::Read);
}

TEST_F(FileManagerTest, AccessModeWrite) {
    auto mode = FileManager::to_access_mode(GENERIC_WRITE);
    EXPECT_EQ(mode, AccessMode::Write);
}

TEST_F(FileManagerTest, AccessModeReadWrite) {
    auto mode = FileManager::to_access_mode(GENERIC_READ | GENERIC_WRITE);
    EXPECT_EQ(mode, AccessMode::ReadWrite);
}

TEST_F(FileManagerTest, AccessModeExecute) {
    auto mode = FileManager::to_access_mode(GENERIC_EXECUTE);
    EXPECT_EQ(mode, AccessMode::Execute);
}

TEST_F(FileManagerTest, AccessModeAll) {
    auto mode = FileManager::to_access_mode(GENERIC_ALL);
    EXPECT_EQ(mode, AccessMode::All);
}

TEST_F(FileManagerTest, AccessModeNone) {
    auto mode = FileManager::to_access_mode(0);
    EXPECT_EQ(mode, AccessMode::None);
}

// =============================================================================
// to_share_mode Tests
// =============================================================================

TEST_F(FileManagerTest, ShareModeNone) {
    auto mode = FileManager::to_share_mode(0);
    EXPECT_EQ(mode, ShareMode::None);
}

TEST_F(FileManagerTest, ShareModeRead) {
    auto mode = FileManager::to_share_mode(FILE_SHARE_READ);
    EXPECT_EQ(mode, ShareMode::Read);
}

TEST_F(FileManagerTest, ShareModeWrite) {
    auto mode = FileManager::to_share_mode(FILE_SHARE_WRITE);
    EXPECT_EQ(mode, ShareMode::Write);
}

TEST_F(FileManagerTest, ShareModeDelete) {
    auto mode = FileManager::to_share_mode(FILE_SHARE_DELETE);
    EXPECT_EQ(mode, ShareMode::Delete);
}

TEST_F(FileManagerTest, ShareModeAll) {
    auto mode = FileManager::to_share_mode(FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    EXPECT_EQ(mode, ShareMode::All);
}

// =============================================================================
// to_disposition Tests
// =============================================================================

TEST_F(FileManagerTest, DispositionCreateNew) {
    EXPECT_EQ(FileManager::to_disposition(CREATE_NEW), CreationDisposition::CreateNew);
}

TEST_F(FileManagerTest, DispositionCreateAlways) {
    EXPECT_EQ(FileManager::to_disposition(CREATE_ALWAYS), CreationDisposition::CreateAlways);
}

TEST_F(FileManagerTest, DispositionOpenExisting) {
    EXPECT_EQ(FileManager::to_disposition(OPEN_EXISTING), CreationDisposition::OpenExisting);
}

TEST_F(FileManagerTest, DispositionOpenAlways) {
    EXPECT_EQ(FileManager::to_disposition(OPEN_ALWAYS), CreationDisposition::OpenAlways);
}

TEST_F(FileManagerTest, DispositionTruncateExisting) {
    EXPECT_EQ(FileManager::to_disposition(TRUNCATE_EXISTING), CreationDisposition::TruncateExisting);
}

TEST_F(FileManagerTest, DispositionUnknownDefaultsToOpenExisting) {
    EXPECT_EQ(FileManager::to_disposition(99), CreationDisposition::OpenExisting);
}

// =============================================================================
// create_file — Success Cases
// =============================================================================

TEST_F(FileManagerTest, CreateFileReturnsValidHandle) {
    FileManager fm = make_fm();
    Handle h = fm.create_file("C:\\test.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);

    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_NE(h, NULL_HANDLE);
    EXPECT_TRUE(handles.is_valid(h));
}

TEST_F(FileManagerTest, ReOpenFileReturnsDistinctHandleToSamePath) {
    FileManager fm = make_fm();
    Handle original = fm.create_file(
        "C:\\test.txt", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);
    ASSERT_NE(original, INVALID_HANDLE);
    const u32 opens_after_create = mock_fs->open_count.load();

    Handle reopened = fm.reopen_file(
        original, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, 0);
    EXPECT_NE(reopened, INVALID_HANDLE);
    EXPECT_NE(reopened, original);
    EXPECT_TRUE(handles.is_valid(reopened));
    EXPECT_EQ(fm.path_for_handle(reopened), "C:\\test.txt");
    EXPECT_EQ(mock_fs->last_access, AccessMode::ReadWrite);
    EXPECT_EQ(mock_fs->last_disposition, CreationDisposition::OpenExisting);
    EXPECT_GT(mock_fs->open_count.load(), opens_after_create);
}

TEST_F(FileManagerTest, ReOpenFileInvalidHandleFails) {
    FileManager fm = make_fm();
    Handle h = fm.reopen_file(0xBEEF, GENERIC_READ, 0, 0);
    EXPECT_EQ(h, INVALID_HANDLE);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

TEST_F(FileManagerTest, CreateFilePassesAccessModeToVfs) {
    FileManager fm = make_fm();
    (void)fm.create_file("C:\\test.txt", GENERIC_READ | GENERIC_WRITE, 0, OPEN_EXISTING, 0);

    EXPECT_EQ(mock_fs->last_access, AccessMode::ReadWrite);
}

TEST_F(FileManagerTest, CreateFilePassesShareModeToVfs) {
    FileManager fm = make_fm();
    (void)fm.create_file("C:\\test.txt", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, 0);

    EXPECT_EQ(mock_fs->last_share, ShareMode::ReadWrite);
}

TEST_F(FileManagerTest, CreateFilePassesDispositionToVfs) {
    FileManager fm = make_fm();
    (void)fm.create_file("C:\\test.txt", GENERIC_WRITE, 0, CREATE_ALWAYS, 0);

    EXPECT_EQ(mock_fs->last_disposition, CreationDisposition::CreateAlways);
}

TEST_F(FileManagerTest, CreateFileHandlesAreUnique) {
    FileManager fm = make_fm();
    Handle h1 = fm.create_file("C:\\a.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);
    Handle h2 = fm.create_file("C:\\b.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);
    Handle h3 = fm.create_file("C:\\c.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);

    EXPECT_NE(h1, INVALID_HANDLE);
    EXPECT_NE(h2, INVALID_HANDLE);
    EXPECT_NE(h3, INVALID_HANDLE);

    EXPECT_NE(h1, h2);
    EXPECT_NE(h2, h3);
    EXPECT_NE(h1, h3);
}

// Host SIGSEGV under Battle.net CEF: concurrent CreateFile raced
// FileManager::m_path_open_counts (std::map emplace) with null deref.
TEST_F(FileManagerTest, ConcurrentCreateAndCloseDoesNotCrash) {
    FileManager fm = make_fm();
    handles.set_close_callback(
        [&fm](Handle h, HandleType type, void* object) -> bool {
            return fm.on_handle_close(h, type, object);
        });

    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 250;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&fm]() {
            for (int i = 0; i < kOpsPerThread; ++i) {
                Handle h = fm.create_file(
                    "C:\\shared.txt", GENERIC_READ, FILE_SHARE_READ,
                    OPEN_EXISTING, 0);
                if (h != INVALID_HANDLE && h != NULL_HANDLE) {
                    (void)fm.close_file(h);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    EXPECT_EQ(mock_fs->open_count.load(),
              static_cast<u32>(kThreads * kOpsPerThread));
}

TEST_F(FileManagerTest, CreateFileAllocatesFileHandleType) {
    FileManager fm = make_fm();
    Handle h = fm.create_file("C:\\test.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);

    EXPECT_EQ(handles.get_type(h), HandleType::File);
}

TEST_F(FileManagerTest, CreateFileRejectsDirectoryWithoutBackupSemantics) {
    mock_fs->opened_handle_is_directory = true;
    FileManager fm = make_fm();

    const Handle h = fm.create_file(
        "C:\\directory", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, 0);

    EXPECT_EQ(h, INVALID_HANDLE);
    EXPECT_TRUE(handles.empty());
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_EQ(mock_fs->closed_handles.count(1), 1u);
}

TEST_F(FileManagerTest, CreateFileOpensDirectoryWithBackupSemantics) {
    mock_fs->opened_handle_is_directory = true;
    FileManager fm = make_fm();

    const Handle h = fm.create_file(
        "C:\\directory", GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS);

    EXPECT_NE(h, INVALID_HANDLE);
    EXPECT_EQ(handles.get_type(h), HandleType::File);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
}

// =============================================================================
// create_file — Failure Cases
// =============================================================================

TEST_F(FileManagerTest, CreateFileReturnsInvalidOnVfsFailure) {
    mock_fs->fail_open = true;

    FileManager fm = make_fm();
    Handle h = fm.create_file("C:\\nonexistent.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);

    EXPECT_EQ(h, INVALID_HANDLE);
}

TEST_F(FileManagerTest, FailedOpenDoesNotAllocateHandle) {
    mock_fs->fail_open = true;

    FileManager fm = make_fm();
    (void)fm.create_file("C:\\nonexistent.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);

    EXPECT_TRUE(handles.empty());
}

// =============================================================================
// close_file Tests
// =============================================================================

TEST_F(FileManagerTest, CloseFileReleasesHandle) {
    FileManager fm = make_fm();
    Handle h = fm.create_file("C:\\test.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    bool ok = fm.close_file(h);
    EXPECT_TRUE(ok);
    EXPECT_FALSE(handles.is_valid(h));
}

TEST_F(FileManagerTest, CloseFileClosesVfsHandle) {
    FileManager fm = make_fm();
    handles.set_close_callback(
        [&fm](Handle h, HandleType type, void* object) -> bool {
            return fm.on_handle_close(h, type, object);
        });
    Handle h = fm.create_file("C:\\test.txt", GENERIC_READ, 0, OPEN_EXISTING, 0);
    ASSERT_NE(h, INVALID_HANDLE);

    fm.close_file(h);

    // The mock tracks VFS handle 1 (first open)
    EXPECT_TRUE(mock_fs->closed_handles.count(1) > 0);
}

TEST_F(FileManagerTest, CloseInvalidHandleReturnsFalse) {
    FileManager fm = make_fm();
    EXPECT_FALSE(fm.close_file(INVALID_HANDLE));
    EXPECT_FALSE(fm.close_file(0x9999));
}

// =============================================================================
// Win32 Constant Values Sanity
// =============================================================================

TEST(FileConstantsTest, Win32ConstantValues) {
    EXPECT_EQ(GENERIC_READ,    0x80000000u);
    EXPECT_EQ(GENERIC_WRITE,   0x40000000u);
    EXPECT_EQ(GENERIC_EXECUTE, 0x20000000u);
    EXPECT_EQ(GENERIC_ALL,     0x10000000u);

    EXPECT_EQ(FILE_SHARE_READ,   0x00000001u);
    EXPECT_EQ(FILE_SHARE_WRITE,  0x00000002u);
    EXPECT_EQ(FILE_SHARE_DELETE, 0x00000004u);

    EXPECT_EQ(CREATE_NEW,        1u);
    EXPECT_EQ(CREATE_ALWAYS,     2u);
    EXPECT_EQ(OPEN_EXISTING,     3u);
    EXPECT_EQ(OPEN_ALWAYS,       4u);
    EXPECT_EQ(TRUNCATE_EXISTING, 5u);
}

/// @file test_file_search.cpp
/// @brief Tests for FileSearchManager (WIN32-081)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/filesystem/file_search.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::capsule;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture — creates a temp directory with known files
// =============================================================================

class FileSearchTest : public ::testing::Test {
protected:
    FileSearchManager mgr;
    std::string tmp_dir;

    void SetUp() override {
        // Create a unique temporary directory
        char tmpl[] = "/tmp/vitreo_fs_test_XXXXXX";
        char* dir = ::mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        tmp_dir = dir;

        // Create test files
        create_test_file("alpha.txt", 100);
        create_test_file("beta.txt", 200);
        create_test_file("gamma.log", 300);
        create_test_file(".hidden_file", 50);

        // Create a subdirectory
        std::string subdir = tmp_dir + "/subdir";
        ::mkdir(subdir.c_str(), 0755);

        // Create a read-only file
        create_test_file("readonly.txt", 10);
        std::string ro_path = tmp_dir + "/readonly.txt";
        ::chmod(ro_path.c_str(), 0444);
    }

    void TearDown() override {
        // Restore write permissions for cleanup
        std::string ro_path = tmp_dir + "/readonly.txt";
        ::chmod(ro_path.c_str(), 0644);

        // Cleanup temp directory
        std::string cmd = "rm -rf " + tmp_dir;
        (void)::system(cmd.c_str());
    }

    void create_test_file(const std::string& name, usize size) {
        std::string path = tmp_dir + "/" + name;
        std::ofstream f(path, std::ios::binary);
        std::string content(size, 'A');
        f.write(content.data(), static_cast<std::streamsize>(size));
    }
};

// =============================================================================
// 0. Concurrent searches — FindFirstFile/FindNextFile/FindClose are ordinary
//    thread-safe Win32 APIs. Several threads driving their own search handles
//    at once must each see a complete, correct enumeration; an unguarded
//    handle table corrupts on concurrent insert/erase instead.
// =============================================================================

TEST_F(FileSearchTest, ConcurrentSearchesEachSeeCompleteEnumeration) {
    constexpr int kThreads = 8;
    constexpr int kIterations = 40;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                FindData data{};
                u64 handle = mgr.find_first_file(tmp_dir + "/*.txt", data);
                if (handle == 0) { ++failures; continue; }
                std::vector<std::string> names{data.file_name};
                while (mgr.find_next_file(handle, data)) {
                    names.push_back(data.file_name);
                }
                if (!mgr.find_close(handle)) ++failures;
                if (names.size() != 3 || names[0] != "alpha.txt" ||
                    names[1] != "beta.txt" || names[2] != "readonly.txt") {
                    ++failures;
                }
            }
        });
    }
    for (auto& th : threads) th.join();
    EXPECT_EQ(failures.load(), 0);
}

// =============================================================================
// 1. FindFirstFile with wildcard *
// =============================================================================

TEST_F(FileSearchTest, FindFirstFile_WildcardStar_FindsEntries) {
    FindData data{};
    u64 handle = mgr.find_first_file(tmp_dir + "/*", data);
    ASSERT_NE(handle, 0u);
    EXPECT_FALSE(data.file_name.empty());
    mgr.find_close(handle);
}

// =============================================================================
// 2. FindFirstFile with specific extension pattern
// =============================================================================

TEST_F(FileSearchTest, FindFirstFile_TxtPattern_FindsOnlyTxt) {
    FindData data{};
    u64 handle = mgr.find_first_file(tmp_dir + "/*.txt", data);
    ASSERT_NE(handle, 0u);

    // Collect all matches
    std::vector<std::string> names;
    names.push_back(data.file_name);
    while (mgr.find_next_file(handle, data)) {
        names.push_back(data.file_name);
    }
    mgr.find_close(handle);

    // Should find alpha.txt, beta.txt, readonly.txt (sorted)
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "alpha.txt");
    EXPECT_EQ(names[1], "beta.txt");
    EXPECT_EQ(names[2], "readonly.txt");
}

// =============================================================================
// 3. FindFirstFile — no match returns 0 and sets FILE_NOT_FOUND
// =============================================================================

TEST_F(FileSearchTest, FindFirstFile_NoMatch_ReturnsZero) {
    FindData data{};
    u64 handle = mgr.find_first_file(tmp_dir + "/*.xyz", data);
    EXPECT_EQ(handle, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

// =============================================================================
// 4. FindNextFile iteration exhausts entries
// =============================================================================

TEST_F(FileSearchTest, FindNextFile_ExhaustsEntries_SetsNoMoreFiles) {
    FindData data{};
    u64 handle = mgr.find_first_file(tmp_dir + "/*.log", data);
    ASSERT_NE(handle, 0u);
    EXPECT_EQ(data.file_name, "gamma.log");

    // Only one .log file, so next should fail
    bool got_next = mgr.find_next_file(handle, data);
    EXPECT_FALSE(got_next);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NO_MORE_FILES);

    mgr.find_close(handle);
}

// =============================================================================
// 5. FindNextFile with invalid handle
// =============================================================================

TEST_F(FileSearchTest, FindNextFile_InvalidHandle_Fails) {
    FindData data{};
    bool ok = mgr.find_next_file(99999, data);
    EXPECT_FALSE(ok);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// 6. FindClose succeeds and invalidates handle
// =============================================================================

TEST_F(FileSearchTest, FindClose_SucceedsAndInvalidates) {
    FindData data{};
    u64 handle = mgr.find_first_file(tmp_dir + "/*", data);
    ASSERT_NE(handle, 0u);

    EXPECT_TRUE(mgr.find_close(handle));

    // Second close should fail
    EXPECT_FALSE(mgr.find_close(handle));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// 7. FindClose with invalid handle
// =============================================================================

TEST_F(FileSearchTest, FindClose_InvalidHandle_Fails) {
    EXPECT_FALSE(mgr.find_close(12345));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
}

// =============================================================================
// 8. GetFileAttributes on a regular file
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributes_RegularFile_HasArchive) {
    u32 attrs = mgr.get_file_attributes(tmp_dir + "/alpha.txt");
    EXPECT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(attrs & FILE_ATTRIBUTE_ARCHIVE);
    EXPECT_FALSE(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// =============================================================================
// 9. GetFileAttributes on a directory
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributes_Directory_HasDirFlag) {
    u32 attrs = mgr.get_file_attributes(tmp_dir + "/subdir");
    EXPECT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// =============================================================================
// 10. GetFileAttributes on nonexistent path
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributes_Nonexistent_ReturnsInvalid) {
    u32 attrs = mgr.get_file_attributes(tmp_dir + "/no_such_file.txt");
    EXPECT_EQ(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

// =============================================================================
// 10b. GetFileAttributes rejects wildcards with ERROR_INVALID_NAME
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributes_WildcardPath_ReturnsInvalidName) {
    // Blizzard MSLU probes native Unicode via GetFileAttributesW(L"???.???")
    // and requires LastError == ERROR_INVALID_NAME (123), not FILE_NOT_FOUND.
    u32 attrs = mgr.get_file_attributes("???.???");
    EXPECT_EQ(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_NAME);

    attrs = mgr.get_file_attributes(tmp_dir + "/*.txt");
    EXPECT_EQ(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_NAME);

    // Extended-path prefix uses a literal '?' that is not a wildcard; wildcards
    // after the prefix still return ERROR_INVALID_NAME.
    attrs = mgr.get_file_attributes("\\\\?\\C:\\does_not_exist_???.txt");
    EXPECT_EQ(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_NAME);
}

TEST_F(FileSearchTest, GetFileAttributesEx_WildcardPath_ReturnsInvalidName) {
    FindData data{};
    EXPECT_FALSE(mgr.get_file_attributes_ex("???.???", data));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_NAME);
}

// =============================================================================
// 11. GetFileAttributesEx fills size and times
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributesEx_FillsSizeAndTimes) {
    FindData data{};
    bool ok = mgr.get_file_attributes_ex(tmp_dir + "/beta.txt", data);
    ASSERT_TRUE(ok);
    EXPECT_EQ(data.file_size, 200u);
    EXPECT_GT(data.last_write_time, 0u);
    EXPECT_GT(data.creation_time, 0u);
    EXPECT_EQ(data.file_name, "beta.txt");
}

// =============================================================================
// 12. GetFileAttributesEx on nonexistent returns false
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributesEx_Nonexistent_ReturnsFalse) {
    FindData data{};
    bool ok = mgr.get_file_attributes_ex(tmp_dir + "/nope.txt", data);
    EXPECT_FALSE(ok);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

// =============================================================================
// 13. Pattern matching with ? wildcard
// =============================================================================

TEST_F(FileSearchTest, FindFirstFile_QuestionMark_MatchesSingleChar) {
    FindData data{};
    // "????.txt" should match "beta.txt" (4 chars before .txt) but not "alpha.txt" (5)
    // Actually "????.txt" won't match "alpha.txt" since alpha is 5 chars
    u64 handle = mgr.find_first_file(tmp_dir + "/????.txt", data);
    ASSERT_NE(handle, 0u);

    std::vector<std::string> names;
    names.push_back(data.file_name);
    while (mgr.find_next_file(handle, data)) {
        names.push_back(data.file_name);
    }
    mgr.find_close(handle);

    // beta.txt (4 chars stem) matches ????.txt
    EXPECT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "beta.txt");
}

// =============================================================================
// 14. Case-insensitive pattern matching
// =============================================================================

TEST_F(FileSearchTest, FindFirstFile_CaseInsensitive) {
    FindData data{};
    u64 handle = mgr.find_first_file(tmp_dir + "/ALPHA.TXT", data);
    ASSERT_NE(handle, 0u);
    EXPECT_EQ(data.file_name, "alpha.txt");
    mgr.find_close(handle);
}

// =============================================================================
// 15. Hidden file has HIDDEN attribute
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributes_HiddenFile_HasHiddenFlag) {
    u32 attrs = mgr.get_file_attributes(tmp_dir + "/.hidden_file");
    EXPECT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(attrs & FILE_ATTRIBUTE_HIDDEN);
}

// =============================================================================
// 16. Read-only file has READONLY attribute
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributes_ReadOnly_HasReadOnlyFlag) {
    u32 attrs = mgr.get_file_attributes(tmp_dir + "/readonly.txt");
    EXPECT_NE(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE(attrs & FILE_ATTRIBUTE_READONLY);
}

// =============================================================================
// 17. Empty pattern returns error
// =============================================================================

TEST_F(FileSearchTest, FindFirstFile_EmptyPattern_ReturnsZero) {
    FindData data{};
    u64 handle = mgr.find_first_file("", data);
    EXPECT_EQ(handle, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// =============================================================================
// 18. GetFileAttributes with empty path
// =============================================================================

TEST_F(FileSearchTest, GetFileAttributes_EmptyPath_ReturnsInvalid) {
    u32 attrs = mgr.get_file_attributes("");
    EXPECT_EQ(attrs, INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(FileSearchTest, GetCompressedFileSize_ReturnsFileSize) {
    u64 size = 0;
    ASSERT_TRUE(mgr.get_compressed_file_size(tmp_dir + "/beta.txt", size));
    EXPECT_EQ(size, 200u);
}

TEST_F(FileSearchTest, GetCompressedFileSize_MissingFileFails) {
    u64 size = 123;
    EXPECT_FALSE(mgr.get_compressed_file_size(tmp_dir + "/missing.dat", size));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

// A built-in DLL's file is a generated VFS file (the module manager is its
// source in a process); the file APIs see it as they see any file, with its
// real size and times (builtin_dll_file_probe: ARCHIVE, size matches between
// GetFileAttributesEx and FindFirstFile). Nothing else is invented: a DLL
// the source does not serve, or the same name outside System32, is absent.
TEST(FileSearchVfsTest, FileApisSeeGeneratedSystemFiles) {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() /
        ("vitreo_sysdll_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "Windows" / "System32");
    fs::create_directories(root / "Windows" / "System");

    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
                    'C', std::make_unique<HostFileSystem>(root.string()))
                    .is_ok());
    const auto image = std::make_shared<const std::vector<u8>>(
        std::vector<u8>(0x10000, 0));
    vfs.set_generated_file_source(
        [&](std::string_view path) -> std::shared_ptr<const std::vector<u8>> {
            std::string lower(path);
            for (char& c : lower) c = static_cast<char>(std::tolower(c));
            return lower == "c:\\windows\\system32\\d3dx9_31.dll" ? image
                                                                  : nullptr;
        },
        [](std::string_view directory) {
            std::vector<std::pair<std::string, u64>> files;
            std::string lower(directory);
            for (char& c : lower) c = static_cast<char>(std::tolower(c));
            if (lower == "c:\\windows\\system32") {
                files.emplace_back("d3dx9_31.dll", 0x10000);
            }
            return files;
        });

    FileSearchManager search;
    search.set_vfs(&vfs);

    const std::string path = "C:\\Windows\\System32\\d3dx9_31.dll";
    EXPECT_EQ(search.get_file_attributes(path), FILE_ATTRIBUTE_ARCHIVE);
    FindData attrs{};
    ASSERT_TRUE(search.get_file_attributes_ex(path, attrs));
    EXPECT_EQ(attrs.attributes, FILE_ATTRIBUTE_ARCHIVE);
    EXPECT_EQ(attrs.file_size, 0x10000u);
    EXPECT_NE(attrs.last_write_time, 0u);

    for (const char* pattern : {"C:\\Windows\\System32\\d3dx9_31.dll",
                                "C:\\Windows\\System32\\D3DX9_*.DLL"}) {
        SCOPED_TRACE(pattern);
        FindData data{};
        const u64 handle = search.find_first_file(pattern, data);
        ASSERT_NE(handle, 0u);
        EXPECT_EQ(data.file_name, "d3dx9_31.dll");
        EXPECT_EQ(data.attributes, FILE_ATTRIBUTE_ARCHIVE);
        EXPECT_EQ(data.file_size, attrs.file_size);
        EXPECT_EQ(data.last_write_time, attrs.last_write_time);
        search.find_close(handle);
    }

    EXPECT_EQ(search.get_file_attributes("C:\\Windows\\System\\d3dx9_31.dll"),
              INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(
        search.get_file_attributes("C:\\Windows\\System32\\not_served.dll"),
        INVALID_FILE_ATTRIBUTES);
    FindData none{};
    EXPECT_EQ(search.find_first_file("C:\\Windows\\System\\d3dx9_31.dll", none),
              0u);

    vfs.set_generated_file_source(nullptr, nullptr);
    fs::remove_all(root);
}

TEST_F(FileSearchTest, ExportsCompressedFileSizeVariants) {
    const DllExports exports = create_file_search_thunks(mgr);
    bool has_wide = false;
    bool has_ansi = false;
    for (const auto& entry : exports.entries) {
        has_wide = has_wide || entry.name == "GetCompressedFileSizeW";
        has_ansi = has_ansi || entry.name == "GetCompressedFileSizeA";
    }
    EXPECT_TRUE(has_wide);
    EXPECT_TRUE(has_ansi);
}

// PE32+ INVALID_HANDLE_VALUE is (HANDLE)-1 (all bits set). Zero-extending
// 0xFFFFFFFF makes `h == INVALID_HANDLE_VALUE` false and steers extractors
// into FindNextFile on a bogus handle (ERROR_INVALID_HANDLE).
TEST(FindFirstFileExWThunkTest, NoMatchReturnsFullWidthInvalidHandleOnX64) {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() /
        ("vitreo_findfirstex_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "Spotify");

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    Vfs vfs;
    FileSearchManager search;
    Kernel32ExtManager ext{memory, vfs};

    ASSERT_TRUE(vfs.mount_drive(
                      'C', std::make_unique<HostFileSystem>(root.string()))
                      .is_ok());
    search.set_vfs(&vfs);
    ext.set_file_search_manager(&search);

    constexpr GuestAddr kBase = 0x1000;
    constexpr GuestAddr kStack = 0x80000;
    constexpr GuestAddr kPattern = 0x2000;
    constexpr GuestAddr kFindData = 0x3000;
    ASSERT_TRUE(memory.map_region(kBase, 0x10000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.cpu_is_64bit());

    const char16_t pattern[] = u"C:\\Spotify\\locales\\*";
    for (usize i = 0; i < sizeof(pattern) / sizeof(pattern[0]); ++i) {
        ASSERT_TRUE(memory.write_u16(kPattern + i * 2,
                                     static_cast<u16>(pattern[i])).is_ok());
    }

    cpu.gpr.rcx = kPattern;
    cpu.gpr.rdx = 0;  // FindExInfoStandard
    cpu.gpr.r8 = kFindData;
    cpu.gpr.r9 = 0;   // FindExSearchNameMatch
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());  // filter
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 0).is_ok());  // flags

    ext.impl_FindFirstFileExW(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_NE(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    fs::remove_all(root);
}


// Expectations follow FindFirstStreamW/FindNextStreamW on Microsoft Learn:
// unnamed file stream, no directory stream, EOF=38, reserved flags=0.
TEST_F(FileSearchTest, StreamEnumerationThunkBothBitnesses) {
    for (const bool mode64 : {false, true}) {
        GuestMemory memory;
        cpu::CpuState cpu;
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        ASSERT_TRUE(memory.map_region(0x1000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu.gpr.rsp = 0x9000;
        Marshaler m{memory};
        m.attach_cpu(cpu);
        auto exports = create_file_search_thunks(mgr);
        auto entry = [&](const char* name) -> const ExportEntry& {
            return *std::find_if(exports.entries.begin(), exports.entries.end(),
                                [&](const auto& e) { return e.name == name; });
        };
        auto args = [&](u64 a, u64 b, u64 c, u64 d) {
            cpu.gpr.rcx = a; cpu.gpr.rdx = b; cpu.gpr.r8 = c; cpu.gpr.r9 = d;
            const u64 values[] = {a,b,c,d};
            for (usize i = 0; i < 4; ++i)
                ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + i * 4,
                                             static_cast<u32>(values[i])).is_ok());
        };
        auto path = [&](const std::string& value) {
            for (usize i = 0; i <= value.size(); ++i)
                ASSERT_TRUE(memory.write_u16(0x2000 + i * 2,
                    i == value.size() ? 0 : static_cast<u16>(value[i])).is_ok());
        };
        const auto& first = entry("FindFirstStreamW");
        const auto& next = entry("FindNextStreamW");
        EXPECT_EQ(first.stdcall_bytes, 16u);
        EXPECT_EQ(next.stdcall_bytes, 8u);
        path(tmp_dir + "/alpha.txt");
        args(0x2000, 0, 0x3000, 0);
        first.handler(m);
        const u64 handle = cpu.gpr.rax;
        ASSERT_NE(handle, mode64 ? ~u64{0} : u64{0xffffffff});
        EXPECT_EQ(memory.read_u64(0x3000).value(), 100u);
        EXPECT_EQ(m.read_wide_string(0x3008).value(), "::$DATA");
        args(handle, 0x3000, 0, 0);
        next.handler(m);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_HANDLE_EOF);
        EXPECT_EQ(memory.read_u64(0x3000).value(), 100u);
        args(handle, 0, 0, 0);
        entry("FindClose").handler(m);
        EXPECT_EQ(cpu.gpr.rax, 1u);
        args(handle, 0x3000, 0, 0);
        next.handler(m);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
        const u64 invalid = mode64 ? ~u64{0} : u64{0xffffffff};
        for (const auto& p : {tmp_dir + "/subdir", tmp_dir + "/missing.txt"}) {
            path(p); args(0x2000, 0, 0x3000, 0); first.handler(m);
            EXPECT_EQ(cpu.gpr.rax, invalid);
            EXPECT_EQ(get_last_error(), p.ends_with("subdir") ?
                      error_codes::ERROR_HANDLE_EOF : error_codes::ERROR_FILE_NOT_FOUND);
        }
        path(tmp_dir + "/alpha.txt");
        args(0x2000, 1, 0x3000, 0); first.handler(m);
        EXPECT_EQ(cpu.gpr.rax, invalid);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
        args(0x2000, 0, 0x3000, 1); first.handler(m);
        EXPECT_EQ(cpu.gpr.rax, invalid);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
        args(0x2000, 0, 0, 0); first.handler(m);
        EXPECT_EQ(cpu.gpr.rax, invalid);
    }
}

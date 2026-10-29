/// @file test_vfs.cpp
/// @brief Unit tests for VFS (Virtual Filesystem) subsystem

#include <gtest/gtest.h>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <system_error>
#include <unistd.h>
#include <sys/wait.h>

#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/directory.hpp"
#include "capsule/vfs/win32_full_path.hpp"

using namespace vitreo;
using namespace vitreo::capsule;

// =============================================================================
// Test Fixture
// =============================================================================

class VfsTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create a temporary directory for testing
        char temp_template[] = "/tmp/vitreo_vfs_test_XXXXXX";
        char* temp_dir = mkdtemp(temp_template);
        ASSERT_NE(temp_dir, nullptr);
        test_root_ = temp_dir;
        
        // Create some test structure
        std::filesystem::create_directories(test_root_ + "/subdir");
        
        // Create a test file
        std::ofstream(test_root_ + "/test.txt") << "Hello, VFS!";
        std::ofstream(test_root_ + "/subdir/nested.txt") << "Nested content";
    }
    
    void TearDown() override {
        // Clean up test directory
        std::filesystem::remove_all(test_root_);
    }
    
    std::string test_root_;
};

TEST_F(VfsTest, DuplicatedFilesShareReadWriteSeekAndInheritedPositions) {
    // DuplicateHandle's documented shared file pointer; independently opened
    // files have independent pointers. The CRT native probe confirms abc reads
    // through alternating aliases, including after the first alias closes.
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    const auto source = vfs.open("C:\\test.txt", AccessMode::ReadWrite, ShareMode::All,
                                 CreationDisposition::OpenExisting);
    const auto independent = vfs.open("C:\\test.txt", AccessMode::Read, ShareMode::All,
                                      CreationDisposition::OpenExisting);
    ASSERT_TRUE(source.is_ok());
    ASSERT_TRUE(independent.is_ok());
    ASSERT_TRUE(vfs.seek(source.value(), 1, SeekOrigin::Begin).is_ok());
    const auto fd = vfs.duplicate_native_fd(source.value());
    ASSERT_TRUE(fd.is_ok());
    const auto alias = vfs.adopt_native_fd(fd.value(), AccessMode::ReadWrite, ShareMode::All, "dup");
    ASSERT_TRUE(alias.is_ok());
    u8 byte = 0;
    const auto read_one = [&](FileHandle handle, char expected) {
        const auto read = vfs.read(handle, std::span<u8>(&byte, 1));
        ASSERT_TRUE(read.is_ok());
        EXPECT_EQ(read.value(), 1u);
        EXPECT_EQ(byte, static_cast<u8>(expected));
    };
    read_one(source.value(), 'e');
    read_one(alias.value(), 'l');
    EXPECT_EQ(vfs.seek(source.value(), 0, SeekOrigin::Current).value(), 3u);
    ASSERT_TRUE(vfs.seek(alias.value(), 0, SeekOrigin::Begin).is_ok());
    EXPECT_EQ(vfs.seek(source.value(), 0, SeekOrigin::Current).value(), 0u);
    byte = 'E';
    ASSERT_TRUE(vfs.write(alias.value(), std::span<const u8>(&byte, 1)).is_ok());
    read_one(source.value(), 'e');
    read_one(independent.value(), 'E');
    EXPECT_EQ(vfs.seek(independent.value(), 0, SeekOrigin::Current).value(), 1u);
    // Duplicating an alias with a stale cached position must not rewind it.
    const auto second_fd = vfs.duplicate_native_fd(alias.value());
    ASSERT_TRUE(second_fd.is_ok());
    const auto second = vfs.adopt_native_fd(second_fd.value(), AccessMode::Read,
                                           ShareMode::All, "dup2");
    ASSERT_TRUE(second.is_ok());
    read_one(second.value(), 'l');
    EXPECT_EQ(vfs.seek(alias.value(), 0, SeekOrigin::Current).value(), 3u);
    ASSERT_TRUE(vfs.close(source.value()).is_ok());
    read_one(alias.value(), 'l');

    const auto inherited_fd = vfs.duplicate_native_fd(second.value());
    ASSERT_TRUE(inherited_fd.is_ok());
    int report[2]; ASSERT_EQ(::pipe(report), 0);
    const auto pid = ::fork(); ASSERT_GE(pid, 0);
    if (pid == 0) {
        ::close(report[0]);
        Vfs child;
        if (child.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_err()) ::_exit(1);
        const auto h = child.adopt_native_fd(inherited_fd.value(), AccessMode::Read, ShareMode::All, "inherited");
        if (h.is_err()) ::_exit(2);
        u8 value = 0;
        const auto got = child.read(h.value(), std::span<u8>(&value, 1));
        if (got.is_err() || got.value() != 1) ::_exit(3);
        (void)!::write(report[1], &value, 1);
        (void)child.close(h.value());
        ::_exit(0);
    }
    ::close(report[1]); ::close(inherited_fd.value());
    ASSERT_EQ(::read(report[0], &byte, 1), 1);
    EXPECT_EQ(byte, static_cast<u8>('o'));
    int status = 0; ASSERT_EQ(::waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(vfs.seek(alias.value(), 0, SeekOrigin::Current).value(), 5u);
    read_one(second.value(), ',');
    ::close(report[0]);
    EXPECT_TRUE(vfs.close(alias.value()).is_ok());
    EXPECT_TRUE(vfs.close(second.value()).is_ok());
    EXPECT_TRUE(vfs.close(independent.value()).is_ok());
}

namespace {
struct EnumerationGate {
    std::promise<void> entered;
    std::promise<void> release;
    std::atomic<bool> destroyed{false};
};

class PausedEnumerationFileSystem : public HostFileSystem {
public:
    PausedEnumerationFileSystem(std::string root,
                               std::shared_ptr<EnumerationGate> gate)
        : HostFileSystem(std::move(root)), gate_(std::move(gate)) {}
    ~PausedEnumerationFileSystem() override { gate_->destroyed = true; }

    Result<std::vector<DirectoryEntry>, VfsError>
    list_dir(std::string_view path) override {
        gate_->entered.set_value();
        gate_->release.get_future().wait();
        return HostFileSystem::list_dir(path);
    }
private:
    std::shared_ptr<EnumerationGate> gate_;
};
} // namespace

TEST_F(VfsTest, EnumerationAllowsFileIoAndRetainsUnmountedFilesystem) {
    using namespace std::chrono_literals;
    Vfs vfs;
    auto gate = std::make_shared<EnumerationGate>();
    auto entered = gate->entered.get_future();
    ASSERT_TRUE(vfs.mount_drive('C',
        std::make_unique<PausedEnumerationFileSystem>(test_root_, gate)).is_ok());
    auto file = vfs.open("C:\\test.txt", AccessMode::ReadWrite, ShareMode::All,
                         CreationDisposition::OpenExisting);
    ASSERT_TRUE(file.is_ok());
    auto listing = std::async(std::launch::async, [&] {
        return vfs.list_dir("C:\\");
    });
    const auto started = entered.wait_for(2s);
    if (started != std::future_status::ready) {
        gate->release.set_value();
        listing.wait();
        FAIL() << "Enumeration did not start";
    }
    auto io = std::async(std::launch::async, [&] {
        std::array<u8, 11> bytes{};
        auto read = vfs.read(file.value(), bytes);
        EXPECT_TRUE(read.is_ok());
        EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "Hello, VFS!");
        const std::array<u8, 1> suffix{'!'};
        auto written = vfs.write(file.value(), suffix);
        EXPECT_TRUE(written.is_ok());
        EXPECT_TRUE(vfs.close(file.value()).is_ok());
        EXPECT_TRUE(vfs.unmount_drive('C').is_ok());
        EXPECT_FALSE(gate->destroyed.load());
        EXPECT_EQ(vfs.list_dir("C:\\").error(), VfsError::DriveNotMounted);
        EXPECT_TRUE(vfs.mount_drive('C',
            std::make_unique<HostFileSystem>(test_root_ + "/subdir")).is_ok());
        auto replacement = vfs.list_dir("C:\\");
        EXPECT_TRUE(replacement.is_ok());
        if (replacement.is_ok()) {
            EXPECT_EQ(replacement.value().size(), 1u);
            EXPECT_EQ(replacement.value().front().name, "nested.txt");
        }
    });
    // Always release the gate, including on the old globally locked code,
    // so a regression fails instead of leaving the test process deadlocked.
    const auto progressed = io.wait_for(2s);
    gate->release.set_value();
    io.get();
    auto original = listing.get();
    EXPECT_EQ(progressed, std::future_status::ready);
    ASSERT_TRUE(original.is_ok());
    EXPECT_EQ(original.value().size(), 2u);
    EXPECT_TRUE(gate->destroyed.load());
    std::ifstream stream(test_root_ + "/test.txt");
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(stream), {}),
              "Hello, VFS!!");
}

// =============================================================================
// Path Parsing Tests
// =============================================================================

TEST(PathParsingTest, ParseDriveLetter) {
    auto parsed = parse_windows_path("C:\\Users\\file.txt");
    EXPECT_EQ(parsed.drive_letter, 'C');
    EXPECT_EQ(parsed.relative_path, "Users/file.txt");
    EXPECT_TRUE(parsed.is_absolute);
}

TEST(PathParsingTest, ParseLowerCaseDrive) {
    auto parsed = parse_windows_path("d:\\data\\file.txt");
    EXPECT_EQ(parsed.drive_letter, 'D');
    EXPECT_EQ(parsed.relative_path, "data/file.txt");
}

TEST(PathParsingTest, ParseForwardSlashes) {
    auto parsed = parse_windows_path("C:/Users/file.txt");
    EXPECT_EQ(parsed.drive_letter, 'C');
    EXPECT_EQ(parsed.relative_path, "Users/file.txt");
}

// Measured (tools/oracle_probe/builtin_dll_file_probe{64,32}.expected): a
// system DLL's file is FILE_ATTRIBUTE_ARCHIVE alone with nonzero times, and
// every open that could change it fails with ERROR_ACCESS_DENIED.
TEST_F(VfsTest, GeneratedFilesAreProtectedFallbacks) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    auto bytes = std::make_shared<const std::vector<u8>>(
        std::vector<u8>{'M', 'Z', 0x90, 0x00});
    ASSERT_TRUE(vfs.publish_generated_file(
        "C:\\Windows\\System32\\ntdll.dll", bytes).is_ok());

    auto opened = vfs.open("c:\\windows\\SYSTEM32\\NTDLL.DLL",
                           AccessMode::Read, ShareMode::Read,
                           CreationDisposition::OpenExisting);
    ASSERT_TRUE(opened.is_ok());
    std::array<u8, 4> read{};
    ASSERT_TRUE(vfs.read(opened.value(), read).is_ok());
    EXPECT_EQ(read, (std::array<u8, 4>{'M', 'Z', 0x90, 0x00}));
    auto info = vfs.stat_handle(opened.value());
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().size, 4u);
    EXPECT_EQ(info.value().attributes, FileAttributes::Archive);
    EXPECT_NE(info.value().last_write_time, 0u);
    EXPECT_EQ(vfs.write(opened.value(), std::span<const u8>(read)).error(),
              VfsError::AccessDenied);
    EXPECT_TRUE(vfs.close(opened.value()).is_ok());

    auto writable = vfs.open("C:\\Windows\\System32\\ntdll.dll",
                             AccessMode::Write, ShareMode::Read,
                             CreationDisposition::OpenExisting);
    ASSERT_TRUE(writable.is_err());
    EXPECT_EQ(writable.error(), VfsError::AccessDenied);
}

TEST_F(VfsTest, GeneratedFilesRefuseEveryModification) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    ASSERT_TRUE(vfs.publish_generated_file(
        "C:\\Windows\\System32\\dsound.dll",
        std::make_shared<const std::vector<u8>>(
            std::vector<u8>{'M', 'Z'})).is_ok());
    const char* const path = "C:\\Windows\\System32\\dsound.dll";

    // GENERIC_WRITE, read+write, DELETE, WRITE_ATTRIBUTES, WRITE_DAC.
    for (const AccessMode access :
         {AccessMode::Write, AccessMode::ReadWrite, AccessMode::Delete,
          AccessMode::WriteMetadata,
          AccessMode::Read | AccessMode::WriteMetadata}) {
        auto opened = vfs.open(path, access, ShareMode::All,
                               CreationDisposition::OpenExisting);
        ASSERT_TRUE(opened.is_err());
        EXPECT_EQ(opened.error(), VfsError::AccessDenied);
    }
    // Replacing or truncating it is a write; creating it finds it exists.
    for (const auto disposition : {CreationDisposition::CreateAlways,
                                   CreationDisposition::TruncateExisting}) {
        auto opened = vfs.open(path, AccessMode::Read, ShareMode::All,
                               disposition);
        ASSERT_TRUE(opened.is_err());
        EXPECT_EQ(opened.error(), VfsError::AccessDenied);
    }
    auto created = vfs.open(path, AccessMode::Read, ShareMode::All,
                            CreationDisposition::CreateNew);
    ASSERT_TRUE(created.is_err());
    EXPECT_EQ(created.error(), VfsError::PathAlreadyExists);
    // Opening it for read with OPEN_ALWAYS opens the generated file and
    // creates nothing in the mount.
    auto always = vfs.open(path, AccessMode::Read, ShareMode::All,
                           CreationDisposition::OpenAlways);
    ASSERT_TRUE(always.is_ok());
    EXPECT_TRUE(vfs.close(always.value()).is_ok());
    EXPECT_FALSE(std::filesystem::exists(
        test_root_ + "/Windows/System32/dsound.dll"));

    EXPECT_EQ(vfs.remove(path).error(), VfsError::AccessDenied);
    EXPECT_EQ(vfs.set_attributes(path, 0x80).error(), VfsError::AccessDenied);
    EXPECT_EQ(vfs.rename(path, "C:\\moved.dll").error(),
              VfsError::AccessDenied);
    EXPECT_EQ(vfs.rename("C:\\test.txt", path).error(),
              VfsError::AccessDenied);
    EXPECT_TRUE(std::filesystem::exists(test_root_ + "/test.txt"));
    EXPECT_TRUE(vfs.stat(path).is_ok());
}

// Learn "File System Redirector": in a 32-bit process a System32 name reaches
// SysWOW64, for a generated file as for a host one (a 32-bit CreateProcess of
// System32\msiexec.exe runs SysWOW64's image); Sysnative and a thread with
// redirection off reach System32.
TEST_F(VfsTest, GeneratedFileLookupFollowsTheWow64View) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    auto native = std::make_shared<const std::vector<u8>>(std::vector<u8>(16, 0x64));
    auto wow = std::make_shared<const std::vector<u8>>(std::vector<u8>(16, 0x32));
    vfs.set_generated_file_source(
        [&](std::string_view path) -> std::shared_ptr<const std::vector<u8>> {
            std::string lower(path);
            for (char& c : lower) c = static_cast<char>(std::tolower(c));
            if (lower == "c:\\windows\\system32\\prog.exe") return native;
            if (lower == "c:\\windows\\syswow64\\prog.exe") return wow;
            return nullptr;
        },
        [](std::string_view) { return std::vector<std::pair<std::string, u64>>{}; });
    const auto first_byte = [&](const char* path) -> int {
        auto h = vfs.open(path, AccessMode::Read, ShareMode::Read, CreationDisposition::OpenExisting);
        if (h.is_err()) return -1;
        u8 b = 0;
        auto got = vfs.read(h.value(), std::span<u8>(&b, 1));
        (void)vfs.close(h.value());
        return got.is_ok() && got.value() == 1 ? b : -1;
    };
    vfs.set_wow64_view(true);
    EXPECT_EQ(first_byte("C:\\Windows\\Sysnative\\prog.exe"), 0x64);
    EXPECT_EQ(first_byte("C:\\Windows\\System32\\prog.exe"), 0x32);
    EXPECT_EQ(first_byte("C:\\Windows\\SysWOW64\\prog.exe"), 0x32);
    Vfs::set_thread_redirection_disabled(true);
    EXPECT_EQ(first_byte("C:\\Windows\\System32\\prog.exe"), 0x64);
    EXPECT_EQ(first_byte("C:\\Windows\\SysWOW64\\prog.exe"), 0x32);
    Vfs::set_thread_redirection_disabled(false);
    EXPECT_TRUE(vfs.is_generated_file("C:\\Windows\\System32\\prog.exe"));
}

TEST_F(VfsTest, GeneratedFileSourceServesFilesTheMountLacksOnDemand) {
    std::filesystem::create_directories(test_root_ + "/Windows/System32");
    std::ofstream(test_root_ + "/Windows/System32/real.dll") << "real";
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    auto image = std::make_shared<const std::vector<u8>>(
        std::vector<u8>(0x10000, 0x5A));
    int built = 0;
    vfs.set_generated_file_source(
        [&](std::string_view path) -> std::shared_ptr<const std::vector<u8>> {
            std::string lower(path);
            for (char& c : lower) c = static_cast<char>(std::tolower(c));
            if (lower != "c:\\windows\\system32\\gen.dll" &&
                lower != "c:\\windows\\system32\\real.dll") {
                return nullptr;
            }
            ++built;
            return image;
        },
        [](std::string_view directory) {
            std::vector<std::pair<std::string, u64>> files;
            std::string lower(directory);
            for (char& c : lower) c = static_cast<char>(std::tolower(c));
            if (lower == "c:\\windows\\system32") {
                files.emplace_back("gen.dll", 0x10000);
                files.emplace_back("REAL.DLL", 0x10000);
            }
            return files;
        });

    // Enumeration lists it without building it; a real file of the same
    // name is listed once, as itself.
    auto listed = vfs.list_dir("C:\\Windows\\System32");
    ASSERT_TRUE(listed.is_ok());
    EXPECT_EQ(built, 0);
    int gen = 0, real = 0;
    for (const auto& entry : listed.value()) {
        std::string lower = entry.name;
        for (char& c : lower) c = static_cast<char>(std::tolower(c));
        if (lower == "gen.dll") {
            ++gen;
            EXPECT_EQ(entry.info.size, 0x10000u);
            EXPECT_EQ(entry.info.attributes, FileAttributes::Archive);
            EXPECT_NE(entry.info.last_write_time, 0u);
        } else if (lower == "real.dll") {
            ++real;
            EXPECT_EQ(entry.info.size, 4u);
        }
    }
    EXPECT_EQ(gen, 1);
    EXPECT_EQ(real, 1);

    // The mount's own file wins over the source.
    auto real_info = vfs.stat("C:\\Windows\\System32\\real.dll");
    ASSERT_TRUE(real_info.is_ok());
    EXPECT_EQ(real_info.value().size, 4u);

    auto info = vfs.stat("c:/windows/system32/GEN.DLL");
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().size, 0x10000u);
    EXPECT_EQ(info.value().attributes, FileAttributes::Archive);
    EXPECT_NE(info.value().creation_time, 0u);
    EXPECT_NE(info.value().last_access_time, 0u);
    EXPECT_NE(info.value().last_write_time, 0u);
    auto opened = vfs.open("C:\\Windows\\System32\\gen.dll", AccessMode::Read,
                           ShareMode::Read, CreationDisposition::OpenExisting);
    ASSERT_TRUE(opened.is_ok());
    std::array<u8, 2> head{};
    ASSERT_TRUE(vfs.read(opened.value(), head).is_ok());
    EXPECT_EQ(head, (std::array<u8, 2>{0x5A, 0x5A}));
    // Named as its directory lists it, not as the first caller spelled it.
    auto final_path = vfs.final_path_for_handle(opened.value());
    ASSERT_TRUE(final_path.is_ok());
    EXPECT_EQ(final_path.value(), "C:\\windows\\system32\\gen.dll");
    EXPECT_TRUE(vfs.close(opened.value()).is_ok());
    // Built once, then served from what it published.
    EXPECT_EQ(built, 1);

    auto missing = vfs.stat("C:\\Windows\\System32\\absent.dll");
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.error(), VfsError::FileNotFound);
}

// Learn (DeleteFile): the call fails while other handles have the file open
// for normal I/O or as a memory-mapped file without FILE_SHARE_DELETE. The
// enforcement has to hold across guest processes, which is why it lives in
// the host filesystem rather than in one process's handle table.
TEST_F(VfsTest, RemoveIsRefusedWhileAnotherHandleDeniesDeleteSharing) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());

    auto held = vfs.open("C:\\test.txt", AccessMode::Read, ShareMode::Read,
                         CreationDisposition::OpenExisting);
    ASSERT_TRUE(held.is_ok());

    auto refused = vfs.remove("C:\\test.txt");
    ASSERT_TRUE(refused.is_err());
    EXPECT_EQ(refused.error(), VfsError::SharingViolation);
    EXPECT_TRUE(std::filesystem::exists(test_root_ + "/test.txt"));

    // A second handle that itself shares delete does not lift the first
    // handle's denial.
    auto sharing = vfs.open("C:\\test.txt", AccessMode::Read,
                            ShareMode::Read | ShareMode::Delete,
                            CreationDisposition::OpenExisting);
    ASSERT_TRUE(sharing.is_ok());
    EXPECT_EQ(vfs.remove("C:\\test.txt").error(), VfsError::SharingViolation);
    EXPECT_TRUE(vfs.close(sharing.value()).is_ok());

    EXPECT_TRUE(vfs.close(held.value()).is_ok());
    EXPECT_TRUE(vfs.remove("C:\\test.txt").is_ok());
    EXPECT_FALSE(std::filesystem::exists(test_root_ + "/test.txt"));
}

TEST_F(VfsTest, RemoveSucceedsWhenEveryOpenHandleSharesDelete) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());

    auto held = vfs.open("C:\\test.txt", AccessMode::Read,
                         ShareMode::Read | ShareMode::Delete,
                         CreationDisposition::OpenExisting);
    ASSERT_TRUE(held.is_ok());
    EXPECT_TRUE(vfs.remove("C:\\test.txt").is_ok());
    EXPECT_FALSE(std::filesystem::exists(test_root_ + "/test.txt"));
    EXPECT_TRUE(vfs.close(held.value()).is_ok());
}

// Audit 04_files X2 / CreateDirectoryW #1: a missing parent directory fails
// PathNotFound and nothing is created (files_core_probe
// CreateFileW_missing_parent, files_ns_probe CreateDirectoryW_missing_parent).
TEST_F(VfsTest, CreateAndMkdirNeverCreateMissingParents) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    for (auto disp : {CreationDisposition::CreateNew,
                      CreationDisposition::CreateAlways,
                      CreationDisposition::OpenAlways}) {
        auto r = vfs.open("C:\\nodir\\x.txt", AccessMode::ReadWrite,
                          ShareMode::All, disp);
        ASSERT_TRUE(r.is_err());
        EXPECT_EQ(r.error(), VfsError::PathNotFound);
    }
    auto d = vfs.create_dir("C:\\p1\\p2");
    ASSERT_TRUE(d.is_err());
    EXPECT_EQ(d.error(), VfsError::PathNotFound);
    EXPECT_FALSE(std::filesystem::exists(test_root_ + "/nodir"));
    EXPECT_FALSE(std::filesystem::exists(test_root_ + "/p1"));
}

// Audit 04_files X4: CreateFile share modes (files_core_probe "# sharing").
TEST_F(VfsTest, ShareModesArbitrateOpensDeletesAndRenames) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    const char* p = "C:\\test.txt";
    const auto rd = AccessMode::Read;
    const auto oe = CreationDisposition::OpenExisting;

    auto excl = vfs.open(p, rd, ShareMode::None, oe);
    ASSERT_TRUE(excl.is_ok());
    auto second = vfs.open(p, rd, ShareMode::All, oe);          // share0_then_read
    ASSERT_TRUE(second.is_err());
    EXPECT_EQ(second.error(), VfsError::SharingViolation);
    auto attrs = vfs.open(p, AccessMode::None, ShareMode::All, oe);  // attr_only
    ASSERT_TRUE(attrs.is_ok());
    EXPECT_TRUE(vfs.close(attrs.value()).is_ok());
    EXPECT_TRUE(vfs.close(excl.value()).is_ok());

    auto reader = vfs.open(p, rd, ShareMode::Read, oe);
    ASSERT_TRUE(reader.is_ok());
    auto r2 = vfs.open(p, rd, ShareMode::Read, oe);              // shareR_then_read
    ASSERT_TRUE(r2.is_ok());
    EXPECT_EQ(vfs.open(p, AccessMode::Write, ShareMode::All, oe).error(),
              VfsError::SharingViolation);                       // shareR_then_write
    EXPECT_EQ(vfs.open(p, AccessMode::Delete, ShareMode::All, oe).error(),
              VfsError::SharingViolation);                       // shareR_then_delete
    // CREATE_ALWAYS that loses the check must not truncate.
    EXPECT_EQ(vfs.open(p, AccessMode::ReadWrite, ShareMode::All,
                       CreationDisposition::CreateAlways).error(),
              VfsError::SharingViolation);
    EXPECT_EQ(std::filesystem::file_size(test_root_ + "/test.txt"), 11u);
    EXPECT_TRUE(vfs.close(reader.value()).is_ok());
    EXPECT_TRUE(vfs.close(r2.value()).is_ok());

    auto writer = vfs.open(p, AccessMode::Write, ShareMode::ReadWrite, oe);
    ASSERT_TRUE(writer.is_ok());
    EXPECT_EQ(vfs.open(p, rd, ShareMode::Read, oe).error(),
              VfsError::SharingViolation);                       // denying write
    auto sharing_reader = vfs.open(p, rd, ShareMode::ReadWrite, oe);
    ASSERT_TRUE(sharing_reader.is_ok());
    EXPECT_EQ(vfs.remove(p).error(), VfsError::SharingViolation);
    EXPECT_EQ(vfs.rename(p, "C:\\moved.txt").error(), VfsError::SharingViolation);
    EXPECT_TRUE(vfs.close(sharing_reader.value()).is_ok());
    EXPECT_TRUE(vfs.close(writer.value()).is_ok());
    EXPECT_TRUE(vfs.rename(p, "C:\\moved.txt").is_ok());
    EXPECT_TRUE(vfs.rename("C:\\moved.txt", p).is_ok());

    // Another process's exclusive open is visible here (guest processes are
    // separate host processes).
    int to_parent[2], to_child[2];
    ASSERT_EQ(::pipe(to_parent), 0);
    ASSERT_EQ(::pipe(to_child), 0);
    const pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        Vfs child_vfs;
        (void)child_vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_));
        auto h = child_vfs.open(p, rd, ShareMode::None, oe);
        char c = h.is_ok() ? 'y' : 'n';
        (void)!::write(to_parent[1], &c, 1);
        (void)!::read(to_child[0], &c, 1);
        ::_exit(0);
    }
    char c = 0;
    ASSERT_EQ(::read(to_parent[0], &c, 1), 1);
    ASSERT_EQ(c, 'y');
    auto cross = vfs.open(p, rd, ShareMode::All, oe);
    EXPECT_TRUE(cross.is_err());
    if (cross.is_err()) EXPECT_EQ(cross.error(), VfsError::SharingViolation);
    EXPECT_EQ(vfs.remove(p).error(), VfsError::SharingViolation);
    (void)!::write(to_child[1], &c, 1);
    int status = 0;
    ::waitpid(pid, &status, 0);
    auto after = vfs.open(p, rd, ShareMode::All, oe);   // child's death released it
    ASSERT_TRUE(after.is_ok());
    EXPECT_TRUE(vfs.close(after.value()).is_ok());
    for (int fd : {to_parent[0], to_parent[1], to_child[0], to_child[1]}) ::close(fd);
}

// GetFullPathNameW shapes measured on Windows 11 (files_ns_probe.expected,
// gfp_* lines, cwd C:\\oracle\\fil). file_part is the UTF-16 offset the probe
// printed (-1 = lpFilePart NULL); these cases are ASCII so bytes == units.
TEST(Win32FullPathTest, MatchesWindowsOracle) {
    struct Case { const char* in; const char* out; int part; };
    const Case cases[] = {
        {"C:\\a\\b.txt", "C:\\a\\b.txt", 5},
        {"C:\\a\\dir\\", "C:\\a\\dir\\", -1},
        {"C:\\a\\missing\\..\\b.txt", "C:\\a\\b.txt", 5},
        {"C:\\..\\x", "C:\\x", 3},
        {"C:\\a\\.\\b", "C:\\a\\b", 5},
        {"x.txt", "C:\\oracle\\fil\\x.txt", 14},
        {"..\\x.txt", "C:\\oracle\\x.txt", 10},
        {"\\foo", "C:\\foo", 3},
        {"D:foo", "D:\\foo", 3},
        {"C:foo", "C:\\oracle\\fil\\foo", 14},
        {"U:", "U:\\", -1},
        {"C:", "C:\\oracle\\fil", 10},
        {"\\\\server\\share\\x", "\\\\server\\share\\x", 15},
        {"\\\\server\\share\\..\\x", "\\\\server\\share\\x", 15},
        {"\\\\?\\C:\\x\\..\\y", "\\\\?\\C:\\y", 7},
        {"\\\\?\\UNC\\test-2\\q$\\lh", "\\\\?\\UNC\\test-2\\q$\\lh", 18},
        {"\\\\.\\pipe\\x", "\\\\.\\pipe\\x", 9},
        {"\\\\.\\C:\\a\\..\\b", "\\\\.\\C:\\b", 7},
        {"C:\\a\\foo. .", "C:\\a\\foo", 5},
        {"C:\\a..\\b", "C:\\a..\\b", 7},
        {"C:/a//b/c", "C:\\a\\b\\c", 7},
        {"CON", "\\\\.\\CON", -1},
        {"C:\\a\\nul.txt", "C:\\a\\nul.txt", 5},
        {"C:\\", "C:\\", -1},
        {".", "C:\\oracle\\fil", 10},
    };
    for (const Case& c : cases) {
        auto r = win32_full_path(c.in, "C:\\oracle\\fil");
        ASSERT_TRUE(r.has_value()) << c.in;
        EXPECT_EQ(r->path, c.out) << c.in;
        EXPECT_EQ(r->file_part == std::string::npos ? -1 : static_cast<int>(r->file_part),
                  c.part) << c.in;
    }
    EXPECT_FALSE(win32_full_path("", "C:\\oracle\\fil").has_value());  // gfp_empty
}

TEST(PathParsingTest, ParseNoDrive) {
    auto parsed = parse_windows_path("\\Users\\file.txt");
    EXPECT_EQ(parsed.drive_letter, 0);
    EXPECT_EQ(parsed.relative_path, "Users/file.txt");
    EXPECT_TRUE(parsed.is_absolute);
}

TEST(PathParsingTest, ParseRelative) {
    auto parsed = parse_windows_path("subdir\\file.txt");
    EXPECT_EQ(parsed.drive_letter, 0);
    EXPECT_EQ(parsed.relative_path, "subdir/file.txt");
    EXPECT_FALSE(parsed.is_absolute);
}

TEST(PathParsingTest, ParseEmpty) {
    auto parsed = parse_windows_path("");
    EXPECT_EQ(parsed.drive_letter, 0);
    EXPECT_TRUE(parsed.relative_path.empty());
}

TEST(PathParsingTest, ParseDriveOnly) {
    auto parsed = parse_windows_path("C:");
    EXPECT_EQ(parsed.drive_letter, 'C');
    EXPECT_TRUE(parsed.relative_path.empty());
}

TEST(PathParsingTest, ParseDriveWithSlash) {
    auto parsed = parse_windows_path("C:\\");
    EXPECT_EQ(parsed.drive_letter, 'C');
    EXPECT_TRUE(parsed.relative_path.empty());
}

TEST(PathParsingTest, NormalizeDuplicateSlashes) {
    auto normalized = normalize_path_separators("dir\\\\subdir//file.txt");
    EXPECT_EQ(normalized, "dir/subdir/file.txt");
}

// =============================================================================
// Drive Letter Utilities Tests
// =============================================================================

TEST(DriveUtilsTest, IsValidDriveLetter) {
    EXPECT_TRUE(is_valid_drive_letter('A'));
    EXPECT_TRUE(is_valid_drive_letter('Z'));
    EXPECT_TRUE(is_valid_drive_letter('a'));
    EXPECT_TRUE(is_valid_drive_letter('z'));
    EXPECT_FALSE(is_valid_drive_letter('1'));
    EXPECT_FALSE(is_valid_drive_letter('@'));
    EXPECT_FALSE(is_valid_drive_letter('['));
}

TEST(DriveUtilsTest, NormalizeDriveLetter) {
    EXPECT_EQ(normalize_drive_letter('c'), 'C');
    EXPECT_EQ(normalize_drive_letter('z'), 'Z');
    EXPECT_EQ(normalize_drive_letter('A'), 'A');
}

// =============================================================================
// VFS Drive Mounting Tests
// =============================================================================

TEST_F(VfsTest, MountDrive) {
    Vfs vfs;
    
    auto result = vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_));
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(vfs.is_drive_mounted('C'));
    EXPECT_TRUE(vfs.is_drive_mounted('c'));  // Case insensitive
}

TEST_F(VfsTest, MountDriveInvalidLetter) {
    Vfs vfs;
    
    auto result = vfs.mount_drive('1', std::make_unique<HostFileSystem>(test_root_));
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::InvalidDriveLetter);
}

TEST_F(VfsTest, MountDriveAlreadyMounted) {
    Vfs vfs;
    
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_));
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::DriveAlreadyMounted);
}

TEST_F(VfsTest, MountDriveNull) {
    Vfs vfs;
    
    auto result = vfs.mount_drive('C', nullptr);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::InvalidArgument);
}

TEST_F(VfsTest, UnmountDrive) {
    Vfs vfs;
    
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.unmount_drive('C');
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(vfs.is_drive_mounted('C'));
}

TEST_F(VfsTest, UnmountNotMounted) {
    Vfs vfs;
    
    auto result = vfs.unmount_drive('D');
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::DriveNotMounted);
}

TEST_F(VfsTest, MountedDrivesList) {
    Vfs vfs;
    
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    ASSERT_TRUE(vfs.mount_drive('D', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto drives = vfs.mounted_drives();
    ASSERT_EQ(drives.size(), 2u);
    EXPECT_EQ(drives[0], 'C');
    EXPECT_EQ(drives[1], 'D');
}

// =============================================================================
// VFS File Operations Tests
// =============================================================================

TEST_F(VfsTest, OpenExistingFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.open("C:\\test.txt", AccessMode::Read, ShareMode::Read, 
                           CreationDisposition::OpenExisting);
    EXPECT_TRUE(result.is_ok());
    
    FileHandle handle = result.value();
    EXPECT_NE(handle, INVALID_FILE_HANDLE);
    
    // Clean up
    (void)vfs.close(handle);
}

TEST_F(VfsTest, OpenNonExistentFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.open("C:\\nonexistent.txt", AccessMode::Read, ShareMode::Read,
                           CreationDisposition::OpenExisting);
    EXPECT_TRUE(result.is_err());
    // The drive root exists and only the leaf is missing, so Windows reports
    // ERROR_FILE_NOT_FOUND rather than ERROR_PATH_NOT_FOUND.
    EXPECT_EQ(result.error(), VfsError::FileNotFound);
}

TEST_F(VfsTest, OpenUnmountedDrive) {
    Vfs vfs;
    
    auto result = vfs.open("D:\\file.txt", AccessMode::Read, ShareMode::Read,
                           CreationDisposition::OpenExisting);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::DriveNotMounted);
}

TEST_F(VfsTest, CreateNewFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.open("C:\\newfile.txt", AccessMode::ReadWrite, ShareMode::None,
                           CreationDisposition::CreateNew);
    EXPECT_TRUE(result.is_ok());
    
    FileHandle handle = result.value();
    (void)vfs.close(handle);
    
    // Verify file exists
    EXPECT_TRUE(std::filesystem::exists(test_root_ + "/newfile.txt"));
}

TEST_F(VfsTest, CreateNewFileThatExists) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.open("C:\\test.txt", AccessMode::ReadWrite, ShareMode::None,
                           CreationDisposition::CreateNew);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::PathAlreadyExists);
}

TEST_F(VfsTest, ReadFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto open_result = vfs.open("C:\\test.txt", AccessMode::Read, ShareMode::Read,
                                 CreationDisposition::OpenExisting);
    ASSERT_TRUE(open_result.is_ok());
    
    FileHandle handle = open_result.value();
    
    u8 buffer[32] = {};
    auto read_result = vfs.read(handle, std::span<u8>(buffer, sizeof(buffer)));
    EXPECT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), 11u);  // "Hello, VFS!" = 11 chars
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buffer), 11), "Hello, VFS!");
    
    (void)vfs.close(handle);
}

TEST_F(VfsTest, WriteFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    // Create and write
    auto create_result = vfs.open("C:\\write_test.txt", AccessMode::Write, ShareMode::None,
                                   CreationDisposition::CreateNew);
    ASSERT_TRUE(create_result.is_ok());
    
    FileHandle handle = create_result.value();
    
    const char* data = "Test write data";
    auto write_result = vfs.write(handle, std::span<const u8>(
        reinterpret_cast<const u8*>(data), std::strlen(data)));
    EXPECT_TRUE(write_result.is_ok());
    EXPECT_EQ(write_result.value(), std::strlen(data));
    
    (void)vfs.close(handle);
    
    // Verify content
    std::ifstream ifs(test_root_ + "/write_test.txt");
    std::string content;
    std::getline(ifs, content);
    EXPECT_EQ(content, "Test write data");
}

TEST_F(VfsTest, SeekFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto open_result = vfs.open("C:\\test.txt", AccessMode::Read, ShareMode::Read,
                                 CreationDisposition::OpenExisting);
    ASSERT_TRUE(open_result.is_ok());
    
    FileHandle handle = open_result.value();
    
    // Seek to position 7 ("VFS!")
    auto seek_result = vfs.seek(handle, 7, SeekOrigin::Begin);
    EXPECT_TRUE(seek_result.is_ok());
    EXPECT_EQ(seek_result.value(), 7u);
    
    // Read from that position
    u8 buffer[4] = {};
    auto read_result = vfs.read(handle, std::span<u8>(buffer, 4));
    EXPECT_TRUE(read_result.is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(buffer), 4), "VFS!");
    
    (void)vfs.close(handle);
}

// The file pointer is file-object state that SetFilePointer updates without
// doing any I/O, so seek() is an in-memory update and read()/write() apply the
// position themselves. These pin the parts of that contract a pread()/pwrite()
// implementation can silently get wrong.
TEST_F(VfsTest, SeekPastEndOfFileSucceedsWithoutExtending) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());

    auto open_result = vfs.open("C:\\test.txt", AccessMode::Read, ShareMode::Read,
                                CreationDisposition::OpenExisting);
    ASSERT_TRUE(open_result.is_ok());
    FileHandle handle = open_result.value();

    // Windows allows the pointer to move well past EOF; the file is unchanged
    // and a read there simply returns zero bytes.
    auto seek_result = vfs.seek(handle, 1'000'000, SeekOrigin::Begin);
    ASSERT_TRUE(seek_result.is_ok());
    EXPECT_EQ(seek_result.value(), 1'000'000u);

    u8 buffer[4] = {};
    auto read_result = vfs.read(handle, std::span<u8>(buffer, 4));
    ASSERT_TRUE(read_result.is_ok());
    EXPECT_EQ(read_result.value(), 0u);

    // The pointer stays where it was put; a failed/short read does not rewind.
    auto tell = vfs.seek(handle, 0, SeekOrigin::Current);
    ASSERT_TRUE(tell.is_ok());
    EXPECT_EQ(tell.value(), 1'000'000u);

    (void)vfs.close(handle);
}

TEST_F(VfsTest, ReadAdvancesPositionAndInterleavesWithSeek) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());

    auto open_result = vfs.open("C:\\test.txt", AccessMode::Read, ShareMode::Read,
                                CreationDisposition::OpenExisting);
    ASSERT_TRUE(open_result.is_ok());
    FileHandle handle = open_result.value();

    // A read must advance the pointer by exactly the byte count returned.
    u8 first[7] = {};
    ASSERT_TRUE(vfs.read(handle, std::span<u8>(first, 7)).is_ok());
    auto after_read = vfs.seek(handle, 0, SeekOrigin::Current);
    ASSERT_TRUE(after_read.is_ok());
    EXPECT_EQ(after_read.value(), 7u);

    // Reading on from there must continue where the pointer sits.
    u8 rest[4] = {};
    ASSERT_TRUE(vfs.read(handle, std::span<u8>(rest, 4)).is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(rest), 4), "VFS!");

    // A relative seek backwards re-reads the same bytes.
    ASSERT_TRUE(vfs.seek(handle, -4, SeekOrigin::Current).is_ok());
    u8 again[4] = {};
    ASSERT_TRUE(vfs.read(handle, std::span<u8>(again, 4)).is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(again), 4), "VFS!");

    // Seeking relative to the end resolves against the real file size.
    auto from_end = vfs.seek(handle, -4, SeekOrigin::End);
    ASSERT_TRUE(from_end.is_ok());
    u8 tail[4] = {};
    ASSERT_TRUE(vfs.read(handle, std::span<u8>(tail, 4)).is_ok());
    EXPECT_EQ(std::string(reinterpret_cast<char*>(tail), 4), "VFS!");

    (void)vfs.close(handle);
}

TEST_F(VfsTest, CloseInvalidHandle) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.close(999);
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::InvalidHandle);
}

TEST_F(VfsTest, ExistingDirectoryCanBackAWriteCapableMetadataHandle) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());

    auto opened = vfs.open("C:\\subdir", AccessMode::Write,
                           ShareMode::All,
                           CreationDisposition::OpenExisting);
    ASSERT_TRUE(opened.is_ok());

    auto info = vfs.stat_handle(opened.value());
    ASSERT_TRUE(info.is_ok());
    EXPECT_TRUE(has_attribute(info.value().attributes,
                              FileAttributes::Directory));
    EXPECT_TRUE(vfs.close(opened.value()).is_ok());
}

// =============================================================================
// VFS Directory Operations Tests
// =============================================================================

TEST_F(VfsTest, StatFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.stat("C:\\test.txt");
    EXPECT_TRUE(result.is_ok());
    
    const auto& info = result.value();
    EXPECT_EQ(info.size, 11u);  // "Hello, VFS!"
    EXPECT_FALSE(has_attribute(info.attributes, FileAttributes::Directory));
}

TEST_F(VfsTest, StatDirectory) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.stat("C:\\subdir");
    EXPECT_TRUE(result.is_ok());
    
    const auto& info = result.value();
    EXPECT_TRUE(has_attribute(info.attributes, FileAttributes::Directory));
}

TEST_F(VfsTest, StatNonExistent) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.stat("C:\\nonexistent");
    EXPECT_TRUE(result.is_err());
    // Parent (the drive root) exists — missing leaf, not a missing path.
    EXPECT_EQ(result.error(), VfsError::FileNotFound);
}

TEST_F(VfsTest, ListDirectory) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.list_dir("C:\\");
    EXPECT_TRUE(result.is_ok());
    
    const auto& entries = result.value();
    EXPECT_GE(entries.size(), 2u);  // At least test.txt and subdir
    
    // Find test.txt
    bool found_test = false;
    bool found_subdir = false;
    for (const auto& entry : entries) {
        if (entry.name == "test.txt") {
            found_test = true;
            EXPECT_FALSE(has_attribute(entry.info.attributes, FileAttributes::Directory));
        }
        if (entry.name == "subdir") {
            found_subdir = true;
            EXPECT_TRUE(has_attribute(entry.info.attributes, FileAttributes::Directory));
        }
    }
    EXPECT_TRUE(found_test);
    EXPECT_TRUE(found_subdir);
}

TEST_F(VfsTest, CreateDirectory) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.create_dir("C:\\newdir");
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(std::filesystem::is_directory(test_root_ + "/newdir"));
}

TEST_F(VfsTest, CreateDirectoryExists) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.create_dir("C:\\subdir");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::PathAlreadyExists);
}

TEST_F(VfsTest, RemoveFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    // Create a file to remove
    std::ofstream(test_root_ + "/to_remove.txt") << "delete me";
    
    auto result = vfs.remove("C:\\to_remove.txt");
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(std::filesystem::exists(test_root_ + "/to_remove.txt"));
}

TEST_F(VfsTest, RemoveDirectory) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    // Create an empty directory to remove
    std::filesystem::create_directory(test_root_ + "/empty_dir");
    
    auto result = vfs.remove("C:\\empty_dir");
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(std::filesystem::exists(test_root_ + "/empty_dir"));
}

TEST_F(VfsTest, RemoveNonEmptyDirectory) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.remove("C:\\subdir");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::DirectoryNotEmpty);
}

TEST_F(VfsTest, RenameFile) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.rename("C:\\test.txt", "C:\\renamed.txt");
    EXPECT_TRUE(result.is_ok());
    EXPECT_FALSE(std::filesystem::exists(test_root_ + "/test.txt"));
    EXPECT_TRUE(std::filesystem::exists(test_root_ + "/renamed.txt"));
}

TEST_F(VfsTest, CreateHardLinkSharesFileIdentityAndContents) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());

    ASSERT_TRUE(vfs.create_hard_link("C:\\linked.txt", "C:\\test.txt").is_ok());
    const auto original = std::filesystem::status(
        std::filesystem::path(test_root_) / "test.txt");
    const auto linked = std::filesystem::status(
        std::filesystem::path(test_root_) / "linked.txt");
    EXPECT_EQ(original.type(), linked.type());

    struct stat original_stat {};
    struct stat linked_stat {};
    ASSERT_EQ(::stat((std::filesystem::path(test_root_) / "test.txt").c_str(),
                     &original_stat), 0);
    ASSERT_EQ(::stat((std::filesystem::path(test_root_) / "linked.txt").c_str(),
                     &linked_stat), 0);
    EXPECT_EQ(original_stat.st_ino, linked_stat.st_ino);
    EXPECT_GE(original_stat.st_nlink, 2);
}

TEST_F(VfsTest, CreateHardLinkRejectsCrossVolumeNames) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    ASSERT_TRUE(vfs.mount_drive(
        'D', std::make_unique<HostFileSystem>(test_root_)).is_ok());

    auto result = vfs.create_hard_link("D:\\linked.txt", "C:\\test.txt");
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::NotSameDevice);
}

TEST_F(VfsTest, RenameCrossDriveFails) {
    Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    ASSERT_TRUE(vfs.mount_drive('D', std::make_unique<HostFileSystem>(test_root_)).is_ok());
    
    auto result = vfs.rename("C:\\test.txt", "D:\\test.txt");
    EXPECT_TRUE(result.is_err());
    EXPECT_EQ(result.error(), VfsError::NotSameDevice);
}

// =============================================================================
// HostFileSystem Direct Tests
// =============================================================================

TEST_F(VfsTest, HostFsRootPath) {
    HostFileSystem fs(test_root_);
    EXPECT_EQ(fs.root_path(), test_root_);
}

TEST_F(VfsTest, HostFsOpenAtRoot) {
    HostFileSystem fs(test_root_);
    
    auto result = fs.open("test.txt", AccessMode::Read, ShareMode::Read,
                          CreationDisposition::OpenExisting);
    EXPECT_TRUE(result.is_ok());
    (void)fs.close(result.value());
}

TEST_F(VfsTest, HostFsOpenNestedPath) {
    HostFileSystem fs(test_root_);
    
    auto result = fs.open("subdir/nested.txt", AccessMode::Read, ShareMode::Read,
                          CreationDisposition::OpenExisting);
    EXPECT_TRUE(result.is_ok());
    (void)fs.close(result.value());
}

TEST_F(VfsTest, HostFsStatRoot) {
    HostFileSystem fs(test_root_);
    
    auto result = fs.stat("");
    EXPECT_TRUE(result.is_ok());
    EXPECT_TRUE(has_attribute(result.value().attributes, FileAttributes::Directory));
}

TEST_F(VfsTest, HostFsListRoot) {
    HostFileSystem fs(test_root_);
    
    auto result = fs.list_dir("");
    EXPECT_TRUE(result.is_ok());
    EXPECT_GE(result.value().size(), 2u);
}

// =============================================================================
// FileTime Tests
// =============================================================================

TEST(FileTimeTest, CurrentFileTime) {
    auto time1 = current_file_time();
    auto time2 = current_file_time();
    
    // Should be non-zero and monotonic
    EXPECT_GT(time1, 0u);
    EXPECT_GE(time2, time1);
}

// =============================================================================
// VfsError Tests
// =============================================================================

TEST(VfsErrorTest, ErrorToString) {
    EXPECT_STREQ(vfs_error_to_string(VfsError::None), "None");
    EXPECT_STREQ(vfs_error_to_string(VfsError::PathNotFound), "PathNotFound");
    EXPECT_STREQ(vfs_error_to_string(VfsError::AccessDenied), "AccessDenied");
    EXPECT_STREQ(vfs_error_to_string(VfsError::DriveNotMounted), "DriveNotMounted");
}

// =============================================================================
// Access and Share Mode Tests
// =============================================================================

TEST(AccessModeTest, BitwiseOr) {
    auto mode = AccessMode::Read | AccessMode::Write;
    EXPECT_TRUE(has_access(mode, AccessMode::Read));
    EXPECT_TRUE(has_access(mode, AccessMode::Write));
    EXPECT_FALSE(has_access(mode, AccessMode::Execute));
}

TEST(AccessModeTest, ReadWritePreset) {
    auto mode = AccessMode::ReadWrite;
    EXPECT_TRUE(has_access(mode, AccessMode::Read));
    EXPECT_TRUE(has_access(mode, AccessMode::Write));
}

TEST(FileAttributesTest, BitwiseOr) {
    auto attrs = FileAttributes::ReadOnly | FileAttributes::Hidden;
    EXPECT_TRUE(has_attribute(attrs, FileAttributes::ReadOnly));
    EXPECT_TRUE(has_attribute(attrs, FileAttributes::Hidden));
    EXPECT_FALSE(has_attribute(attrs, FileAttributes::Directory));
}

// =============================================================================
// ENOENT disambiguation: missing leaf vs missing directory
// =============================================================================
//
// POSIX collapses both cases into ENOENT, but Windows reports
// ERROR_FILE_NOT_FOUND (2) when only the final component is absent and
// ERROR_PATH_NOT_FOUND (3) when a directory along the way is absent.
// Applications branch on the difference — the StarCraft: Brood War installer
// treats a missing file as "nothing to back up" but a missing path as fatal,
// and aborted with "Error 3: The system cannot find the path specified" when
// Vitreo reported PathNotFound for an absent map file in an existing folder.

TEST_F(VfsTest, OpenMissingFileInExistingDirectoryReportsFileNotFound) {
    HostFileSystem fs(test_root_);
    auto r = fs.open("subdir/does_not_exist.txt", AccessMode::Read,
                     ShareMode::Read, CreationDisposition::OpenExisting);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::FileNotFound);
}

TEST_F(VfsTest, OpenFileUnderMissingDirectoryReportsPathNotFound) {
    HostFileSystem fs(test_root_);
    auto r = fs.open("no_such_dir/file.txt", AccessMode::Read,
                     ShareMode::Read, CreationDisposition::OpenExisting);
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::PathNotFound);
}

TEST_F(VfsTest, StatMissingFileInExistingDirectoryReportsFileNotFound) {
    HostFileSystem fs(test_root_);
    auto r = fs.stat("subdir/does_not_exist.txt");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::FileNotFound);
}

TEST_F(VfsTest, StatUnderMissingDirectoryReportsPathNotFound) {
    HostFileSystem fs(test_root_);
    auto r = fs.stat("no_such_dir/file.txt");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::PathNotFound);
}

TEST_F(VfsTest, IsDirectoryStillFalseForBothMissingKinds) {
    // is_directory() treats "absent" as false; the new error must not turn
    // a missing leaf into a propagated failure.
    HostFileSystem fs(test_root_);
    DirectoryOps dir(fs);
    auto a = dir.is_directory("subdir/does_not_exist.txt");
    ASSERT_TRUE(a.is_ok());
    EXPECT_FALSE(a.value());
    auto b = dir.is_directory("no_such_dir/file.txt");
    ASSERT_TRUE(b.is_ok());
    EXPECT_FALSE(b.value());
}

TEST_F(VfsTest, RemoveMissingFileInExistingDirectoryReportsFileNotFound) {
    // The Brood War installer aborts on ERROR_PATH_NOT_FOUND while deleting a
    // file that simply is not there ("Unable to delete the file ...\\maps\\
    // (8)Green Valleys.scm"). Windows reports ERROR_FILE_NOT_FOUND for that.
    HostFileSystem fs(test_root_);
    auto r = fs.remove("subdir/does_not_exist.txt");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::FileNotFound);
}

TEST_F(VfsTest, RemoveUnderMissingDirectoryReportsPathNotFound) {
    HostFileSystem fs(test_root_);
    auto r = fs.remove("no_such_dir/file.txt");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::PathNotFound);
}

TEST_F(VfsTest, RenameMissingSourceInExistingDirectoryReportsFileNotFound) {
    HostFileSystem fs(test_root_);
    auto r = fs.rename("subdir/does_not_exist.txt", "subdir/other.txt");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::FileNotFound);
}

TEST_F(VfsTest, ListDirOfMissingDirectoryReportsPathNotFound) {
    HostFileSystem fs(test_root_);
    auto r = fs.list_dir("no_such_dir");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::PathNotFound);
}

TEST_F(VfsTest, RemoveOnDirectoryStillWorksAtVfsLevel) {
    // The VFS layer intentionally removes both files and directories; it is
    // DeleteFile (Win32) that must refuse directories, not the VFS.
    HostFileSystem fs(test_root_);
    std::filesystem::create_directory(test_root_ + "/emptydir");
    auto r = fs.remove("emptydir");
    EXPECT_TRUE(r.is_ok());
}

// =============================================================================
// Case-Insensitive Path Resolution
// =============================================================================

/// Windows resolves paths case-insensitively regardless of the medium, so a
/// guest may ask for "DSETUP.DLL" and reach a file stored as "DSETUP.dll".
/// macOS boot volumes hide this because they are already case-insensitive;
/// mounted install media (ISO9660/UDF) is not, so these tests need a volume
/// that really distinguishes case.
class CaseSensitiveVolumeTest : public ::testing::Test {
protected:
    void SetUp() override {
        image_ = std::filesystem::temp_directory_path() /
                 ("vitreo_case_vol_" + std::to_string(::getpid()));
        mount_ = std::filesystem::temp_directory_path() /
                 ("vitreo_case_mnt_" + std::to_string(::getpid()));

        const std::string create =
            "hdiutil create -size 16m -fs 'Case-sensitive APFS' -volname "
            "VitreoCase -type SPARSE '" + image_.string() + "' >/dev/null 2>&1";
        if (std::system(create.c_str()) != 0) {
            GTEST_SKIP() << "cannot create a case-sensitive volume here";
        }

        std::filesystem::create_directories(mount_);
        const std::string attach =
            "hdiutil attach '" + image_.string() + ".sparseimage' -nobrowse "
            "-mountpoint '" + mount_.string() + "' >/dev/null 2>&1";
        if (std::system(attach.c_str()) != 0) {
            GTEST_SKIP() << "cannot attach a case-sensitive volume here";
        }
        attached_ = true;
        ASSERT_EQ(::pathconf(mount_.c_str(), _PC_CASE_SENSITIVE), 1);
    }

    void TearDown() override {
        if (attached_) {
            const std::string detach =
                "hdiutil detach '" + mount_.string() + "' -force >/dev/null 2>&1";
            (void)std::system(detach.c_str());
        }
        std::error_code ec;
        std::filesystem::remove_all(mount_, ec);
        std::filesystem::remove(image_.string() + ".sparseimage", ec);
    }

    std::filesystem::path image_;
    std::filesystem::path mount_;
    bool attached_ = false;
};

TEST_F(CaseSensitiveVolumeTest, OpenFindsFileStoredWithDifferentCase) {
    std::filesystem::create_directories(mount_ / "Support" / "DirectX");
    std::ofstream(mount_ / "Support" / "DirectX" / "DSETUP.dll") << "MZ";

    HostFileSystem fs(mount_.string());
    auto handle = fs.open("SUPPORT/directx/DSETUP.DLL", AccessMode::Read,
                          ShareMode::Read, CreationDisposition::OpenExisting);
    ASSERT_TRUE(handle.is_ok()) << "open must match the on-disc casing";
    EXPECT_TRUE(fs.close(handle.value()).is_ok());
}

TEST_F(CaseSensitiveVolumeTest, StatFindsFileStoredWithDifferentCase) {
    std::ofstream(mount_ / "DSETUP.dll") << "MZ";

    HostFileSystem fs(mount_.string());
    auto info = fs.stat("dsetup.DLL");
    ASSERT_TRUE(info.is_ok());
    EXPECT_EQ(info.value().size, 2u);
}

TEST_F(CaseSensitiveVolumeTest, MissingFileStillReportsNotFound) {
    HostFileSystem fs(mount_.string());
    auto handle = fs.open("absent.dll", AccessMode::Read, ShareMode::Read,
                          CreationDisposition::OpenExisting);
    ASSERT_TRUE(handle.is_err());
    EXPECT_EQ(handle.error(), VfsError::FileNotFound);
}

TEST_F(CaseSensitiveVolumeTest, CreateKeepsTheRequestedSpelling) {
    HostFileSystem fs(mount_.string());
    auto handle = fs.open("NewFile.TXT", AccessMode::Write, ShareMode::None,
                          CreationDisposition::CreateAlways);
    ASSERT_TRUE(handle.is_ok());
    EXPECT_TRUE(fs.close(handle.value()).is_ok());
    // A name that matched nothing must be created exactly as asked, not folded.
    EXPECT_TRUE(std::filesystem::exists(mount_ / "NewFile.TXT"));
}

TEST_F(CaseSensitiveVolumeTest, CreateAlwaysReusesAnExistingDifferentlyCasedFile) {
    std::ofstream(mount_ / "Existing.txt") << "old contents";

    HostFileSystem fs(mount_.string());
    auto handle = fs.open("EXISTING.TXT", AccessMode::Write, ShareMode::None,
                          CreationDisposition::CreateAlways);
    ASSERT_TRUE(handle.is_ok());
    EXPECT_TRUE(fs.close(handle.value()).is_ok());
    // Windows would truncate the one file that exists rather than add a second
    // name differing only by case.
    EXPECT_FALSE(std::filesystem::exists(mount_ / "EXISTING.TXT"));
    EXPECT_EQ(std::filesystem::file_size(mount_ / "Existing.txt"), 0u);
}

// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_delete_file.cpp
/// @brief DeleteFileA/DeleteFileW Windows contract tests
///
/// Windows distinguishes a missing file from a missing directory, refuses to
/// delete directories, and must behave identically through the ANSI and
/// Unicode entry points. The two variants previously carried separate copies
/// of the VfsError -> Win32 mapping and had drifted: the wide form reported
/// ERROR_FILE_NOT_FOUND for a missing *path*, so an installer probing a path
/// under a deleted directory got the wrong branch.

#include <gtest/gtest.h>

#include <vitreo/common.hpp>
#include <capsule/vfs/vfs.hpp>
#include "capsule/vfs/host_filesystem.hpp"
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/filesystem/file_ops.hpp>
#include <winapi/core/marshaler.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr DATA_BASE  = 0x2000;
constexpr GuestAddr STACK_BASE = 0x80000;

/// Which entry point to exercise. Every case runs through both.
enum class Variant { Ansi, Wide };

class DeleteFileTest : public ::testing::TestWithParam<Variant> {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/vitreo_delfile_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        root_ = dir;

        std::filesystem::create_directories(root_ + "/sub");
        std::ofstream(root_ + "/sub/present.txt") << "data";
        std::filesystem::create_directories(root_ + "/adir");

        ASSERT_TRUE(memory_.map_region(0x1000, 0x100000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(vfs_.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(root_)).is_ok());

        cpu_.init_protected_mode();
        cpu_.set_sp(STACK_BASE + 0x800);
        marshaler_.attach_cpu(cpu_);
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    /// Call DeleteFileA or DeleteFileW with `guest_path`; returns the BOOL.
    u32 call_delete(const std::string& guest_path) {
        if (GetParam() == Variant::Ansi) {
            for (usize i = 0; i < guest_path.size(); ++i) {
                (void)memory_.write_u8(DATA_BASE + static_cast<GuestAddr>(i),
                                       static_cast<u8>(guest_path[i]));
            }
            (void)memory_.write_u8(
                DATA_BASE + static_cast<GuestAddr>(guest_path.size()), 0);
        } else {
            for (usize i = 0; i < guest_path.size(); ++i) {
                (void)memory_.write_u16(DATA_BASE + static_cast<GuestAddr>(i) * 2,
                                        static_cast<u16>(guest_path[i]));
            }
            (void)memory_.write_u16(
                DATA_BASE + static_cast<GuestAddr>(guest_path.size()) * 2, 0);
        }
        (void)memory_.write_u32(static_cast<u32>(cpu_.sp() + 4), DATA_BASE);
        set_last_error(error_codes::ERROR_SUCCESS);

        if (GetParam() == Variant::Ansi) {
            file_ops_.impl_DeleteFileA(marshaler_);
        } else {
            file_ops_.impl_DeleteFileW(marshaler_);
        }
        return static_cast<u32>(cpu_.gpr.rax);
    }

    std::string   root_;
    GuestMemory   memory_;
    capsule::Vfs  vfs_;
    cpu::CpuState cpu_;
    Marshaler     marshaler_{memory_};
    FileOpsManager file_ops_{memory_, vfs_};
};

// 1. Existing parent, missing file -> FALSE + ERROR_FILE_NOT_FOUND
TEST_P(DeleteFileTest, MissingFileInExistingDirectory) {
    EXPECT_EQ(call_delete("C:\\sub\\nope.txt"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND);
}

// 2. Missing parent directory -> FALSE + ERROR_PATH_NOT_FOUND
TEST_P(DeleteFileTest, MissingParentDirectory) {
    EXPECT_EQ(call_delete("C:\\nosuchdir\\nope.txt"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND);
}

// 3. Target is a directory -> FALSE + ERROR_ACCESS_DENIED, directory survives
TEST_P(DeleteFileTest, RefusesDirectoryAndLeavesItIntact) {
    EXPECT_EQ(call_delete("C:\\adir"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_ACCESS_DENIED);
    EXPECT_TRUE(std::filesystem::is_directory(root_ + "/adir"))
        << "DeleteFile must not remove directories";
}

// 4. Existing normal file -> TRUE + ERROR_SUCCESS, file is gone
TEST_P(DeleteFileTest, DeletesExistingFile) {
    ASSERT_TRUE(std::filesystem::exists(root_ + "/sub/present.txt"));
    EXPECT_EQ(call_delete("C:\\sub\\present.txt"), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_FALSE(std::filesystem::exists(root_ + "/sub/present.txt"))
        << "delete must actually remove the file, not just report success";
}

// 6. Path containing spaces and parentheses — the real capsule shape. Exercises
// host-path translation and parent detection against a directory name that a
// naive splitter or an unquoted shell-style path would mangle.
TEST_P(DeleteFileTest, PathWithSpacesAndParenthesesMissingLeaf) {
    std::filesystem::create_directories(root_ + "/Program Files (x86)/Starcraft/Temp");
    EXPECT_EQ(call_delete("C:\\Program Files (x86)\\Starcraft\\Temp\\missing.loc"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_FILE_NOT_FOUND)
        << "parent exists, so the leaf is what is missing";
}

TEST_P(DeleteFileTest, PathWithSpacesAndParenthesesMissingParent) {
    EXPECT_EQ(call_delete("C:\\Program Files (x86)\\Starcraft\\NoTemp\\missing.loc"), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_PATH_NOT_FOUND)
        << "intermediate directory absent";
}

TEST_P(DeleteFileTest, PathWithSpacesAndParenthesesDeletesRealFile) {
    std::filesystem::create_directories(root_ + "/Program Files (x86)/Starcraft/Temp");
    const std::string f = root_ + "/Program Files (x86)/Starcraft/Temp/SEditPTG.loc";
    std::ofstream(f) << "x";
    ASSERT_TRUE(std::filesystem::exists(f));
    EXPECT_EQ(call_delete("C:\\Program Files (x86)\\Starcraft\\Temp\\SEditPTG.loc"), 1u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_FALSE(std::filesystem::exists(f));
}

INSTANTIATE_TEST_SUITE_P(
    AnsiAndWide, DeleteFileTest,
    ::testing::Values(Variant::Ansi, Variant::Wide),
    [](const ::testing::TestParamInfo<Variant>& i) {
        return i.param == Variant::Ansi ? "DeleteFileA" : "DeleteFileW";
    });

// 5. The two variants must agree on every case above. Parameterising the suite
// covers that by construction; this pins the equivalence explicitly so a future
// divergence fails with an obvious message.
TEST(DeleteFileParityTest, AnsiAndWideReportIdenticalErrors) {
    struct Case { const char* path; u32 expected_ret; u32 expected_err; };
    const Case cases[] = {
        {"C:\\sub\\nope.txt",       0u, error_codes::ERROR_FILE_NOT_FOUND},
        {"C:\\nosuchdir\\nope.txt", 0u, error_codes::ERROR_PATH_NOT_FOUND},
        {"C:\\adir",                0u, error_codes::ERROR_ACCESS_DENIED},
    };

    for (const auto& c : cases) {
        u32 rets[2]; u32 errs[2];
        for (int variant = 0; variant < 2; ++variant) {
            char tmpl[] = "/tmp/vitreo_delparity_XXXXXX";
            const char* dir = ::mkdtemp(tmpl);
            ASSERT_NE(dir, nullptr);
            const std::string root = dir;
            std::filesystem::create_directories(root + "/sub");
            std::filesystem::create_directories(root + "/adir");

            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x1000, 0x100000,
                                          MemoryProtection::ReadWrite).is_ok());
            capsule::Vfs vfs;
            ASSERT_TRUE(vfs.mount_drive(
                'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
            cpu::CpuState cpu;
            cpu.init_protected_mode();
            cpu.set_sp(STACK_BASE + 0x800);
            Marshaler m(memory);
            m.attach_cpu(cpu);
            FileOpsManager ops(memory, vfs);

            const std::string path = c.path;
            if (variant == 0) {
                for (usize i = 0; i < path.size(); ++i)
                    (void)memory.write_u8(DATA_BASE + static_cast<GuestAddr>(i),
                                          static_cast<u8>(path[i]));
                (void)memory.write_u8(
                    DATA_BASE + static_cast<GuestAddr>(path.size()), 0);
            } else {
                for (usize i = 0; i < path.size(); ++i)
                    (void)memory.write_u16(DATA_BASE + static_cast<GuestAddr>(i) * 2,
                                           static_cast<u16>(path[i]));
                (void)memory.write_u16(
                    DATA_BASE + static_cast<GuestAddr>(path.size()) * 2, 0);
            }
            (void)memory.write_u32(static_cast<u32>(cpu.sp() + 4), DATA_BASE);
            set_last_error(error_codes::ERROR_SUCCESS);

            if (variant == 0) ops.impl_DeleteFileA(m);
            else              ops.impl_DeleteFileW(m);

            rets[variant] = static_cast<u32>(cpu.gpr.rax);
            errs[variant] = get_last_error();
            std::filesystem::remove_all(root);
        }
        EXPECT_EQ(rets[0], rets[1]) << "return differs for " << c.path;
        EXPECT_EQ(errs[0], errs[1]) << "last error differs for " << c.path;
        EXPECT_EQ(rets[0], c.expected_ret) << c.path;
        EXPECT_EQ(errs[0], c.expected_err) << c.path;
    }
}

} // namespace

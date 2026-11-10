// SPDX-License-Identifier: LGPL-2.1-only
//
// known_windows_exe_path: an executable inside the capsule's drive tree runs
// under its own Windows path, however the host names that file. A host path
// reached through a symlink (a linked capsule root, /tmp vs /private/tmp)
// used to look external, so the executable was republished under
// C:\Vitreo\Apps and ran from the wrong Windows directory.

#include "app/cli/commands/run_internal.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
using vitreo::cli::known_windows_exe_path;
using vitreo::cli::RunConfig;

class KnownWindowsExePathTest : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/vitreo-exepath-XXXXXX";
        ASSERT_NE(mkdtemp(tmpl), nullptr);
        base_ = tmpl;
        real_ = base_ / "real";
        capsule_ = real_ / "capsules" / "Steam";
        fs::create_directories(capsule_ / "drives" / "c" / "Program Files" / "App");
        std::ofstream(capsule_ / "drives" / "c" / "Program Files" / "App" / "app.exe")
            << "MZ";
        // ~/.vitreo -> ~/.vitrum, as an install that kept an older folder.
        fs::create_directory_symlink(real_, base_ / "link");
        linked_capsule_ = base_ / "link" / "capsules" / "Steam";
    }
    void TearDown() override {
        std::error_code ec;
        fs::remove_all(base_, ec);
    }

    std::optional<std::string> known(const fs::path& capsule_root,
                                     const fs::path& exe) const {
        return known_windows_exe_path(config_, capsule_root.string(),
                                      exe.string(), false);
    }

    RunConfig config_{};
    fs::path base_, real_, capsule_, linked_capsule_;
};

TEST_F(KnownWindowsExePathTest, SamePathsMapToTheDrive) {
    EXPECT_EQ(known(capsule_,
                    capsule_ / "drives/c/Program Files/App/app.exe"),
              std::optional<std::string>("C:\\Program Files\\App\\app.exe"));
}

// The capsule root is named through a link, the executable by its real path
// (what the manager app passed for Steam on the Intel iMac).
TEST_F(KnownWindowsExePathTest, LinkedCapsuleRootRealExecutablePath) {
    EXPECT_EQ(known(linked_capsule_,
                    capsule_ / "drives/c/Program Files/App/app.exe"),
              std::optional<std::string>("C:\\Program Files\\App\\app.exe"));
}

TEST_F(KnownWindowsExePathTest, RealCapsuleRootLinkedExecutablePath) {
    EXPECT_EQ(known(capsule_,
                    linked_capsule_ / "drives/c/Program Files/App/app.exe"),
              std::optional<std::string>("C:\\Program Files\\App\\app.exe"));
}

// Something genuinely outside the capsule stays external (it is published
// under C:\Vitreo\Apps by resolve_windows_exe_path).
TEST_F(KnownWindowsExePathTest, ExecutableOutsideTheCapsuleIsExternal) {
    const fs::path outside = base_ / "elsewhere" / "tool.exe";
    fs::create_directories(outside.parent_path());
    std::ofstream(outside) << "MZ";
    EXPECT_EQ(known(linked_capsule_, outside), std::nullopt);
}

// A drive letter that is itself a link onto a host folder must not claim
// every path under that folder once paths are resolved.
TEST_F(KnownWindowsExePathTest, LinkedDriveDoesNotClaimItsTargetTree) {
    const fs::path host_tree = base_ / "hostroot";
    fs::create_directories(host_tree / "bin");
    std::ofstream(host_tree / "bin" / "tool.exe") << "MZ";
    fs::create_directory_symlink(host_tree, capsule_ / "drives" / "z");
    EXPECT_EQ(known(linked_capsule_, host_tree / "bin" / "tool.exe"),
              std::nullopt);
    // Named through the drive itself it is still that drive, as before.
    EXPECT_EQ(known(capsule_, capsule_ / "drives/z/bin/tool.exe"),
              std::optional<std::string>("Z:\\bin\\tool.exe"));
}

// A relaunch's --relaunched-as path wins, unchanged.
TEST_F(KnownWindowsExePathTest, RelaunchedAsWins) {
    config_.relaunched_as = "C:\\Program Files\\App\\app.exe";
    EXPECT_EQ(known(capsule_, "/nowhere/app.exe"),
              std::optional<std::string>("C:\\Program Files\\App\\app.exe"));
}

}  // namespace

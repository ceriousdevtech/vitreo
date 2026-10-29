// SPDX-License-Identifier: LGPL-2.1-only
// Regression coverage for the capsule-side mapping of external executables.

#include "app/cli/commands/external_app_mapping.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <unistd.h>

namespace vitreo::cli {
namespace {

namespace fs = std::filesystem;

class ExternalAppMappingTest : public ::testing::Test {
protected:
    void SetUp() override {
        root = fs::temp_directory_path() /
            ("vitreo-app-mapping-test-" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(root, ec);
        ASSERT_TRUE(fs::create_directories(root, ec));
        capsule = root / "capsule";
        ASSERT_TRUE(fs::create_directories(capsule / "drives" / "c", ec));
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    [[nodiscard]] fs::path apps_dir() const {
        return capsule / "drives" / "c" / "Vitreo" / "Apps";
    }

    /// @return the canonical host directory, matching what a mapping records
    ///         as its target after symlinked path components are resolved.
    [[nodiscard]] fs::path make_host_dir(const std::string& relative) {
        const fs::path dir = root / "host" / relative;
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream(dir / "app.exe") << "MZ";
        return fs::weakly_canonical(dir, ec);
    }

    fs::path root;
    fs::path capsule;
};

TEST_F(ExternalAppMappingTest, ClaimsBasenameAndPublishesHostDirectory) {
    const fs::path host = make_host_dir("Solitaire");
    ExternalAppMappingError error = ExternalAppMappingError::NoAvailableName;

    ExternalAppMapping mapping =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);

    ASSERT_TRUE(mapping.is_owned())
        << external_app_mapping_error_message(error);
    EXPECT_EQ(error, ExternalAppMappingError::None);
    EXPECT_EQ(mapping.guest_directory_name(), "Solitaire");
    EXPECT_TRUE(fs::is_symlink(mapping.link_path()));
    EXPECT_TRUE(fs::exists(fs::path(mapping.link_path()) / "app.exe"));
}

TEST_F(ExternalAppMappingTest, SameBasenameDirectoriesDoNotCollide) {
    const fs::path first = make_host_dir("a/bin");
    const fs::path second = make_host_dir("b/bin");
    ExternalAppMappingError error = ExternalAppMappingError::None;

    ExternalAppMapping first_mapping =
        ExternalAppMapping::claim(capsule.string(), first.string(), error);
    ASSERT_TRUE(first_mapping.is_owned());
    ExternalAppMapping second_mapping =
        ExternalAppMapping::claim(capsule.string(), second.string(), error);
    ASSERT_TRUE(second_mapping.is_owned())
        << external_app_mapping_error_message(error);

    EXPECT_NE(first_mapping.guest_directory_name(),
              second_mapping.guest_directory_name());
    EXPECT_EQ(fs::read_symlink(first_mapping.link_path()), first);
    EXPECT_EQ(fs::read_symlink(second_mapping.link_path()), second);
}

TEST_F(ExternalAppMappingTest, ConcurrentClaimsOfSameDirectoryStayValid) {
    const fs::path host = make_host_dir("Shared");
    ExternalAppMappingError error = ExternalAppMappingError::None;

    ExternalAppMapping first =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);
    ExternalAppMapping second =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);

    ASSERT_TRUE(first.is_owned());
    ASSERT_TRUE(second.is_owned());
    EXPECT_NE(first.link_path(), second.link_path());

    // The first launch exiting must not invalidate the second launch's view.
    first.release();
    EXPECT_FALSE(fs::exists(fs::symlink_status(first.link_path())));
    EXPECT_TRUE(fs::exists(fs::path(second.link_path()) / "app.exe"));
}

TEST_F(ExternalAppMappingTest, NeverRemovesRealDirectoryAtCandidateName) {
    const fs::path host = make_host_dir("Solitaire");
    const fs::path installed = apps_dir() / "Solitaire";
    std::error_code ec;
    ASSERT_TRUE(fs::create_directories(installed, ec));
    std::ofstream(installed / "installed.dat") << "portable app payload";

    ExternalAppMappingError error = ExternalAppMappingError::None;
    ExternalAppMapping mapping =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);

    ASSERT_TRUE(mapping.is_owned())
        << external_app_mapping_error_message(error);
    EXPECT_NE(mapping.guest_directory_name(), "Solitaire");
    EXPECT_TRUE(fs::is_directory(installed));
    EXPECT_TRUE(fs::exists(installed / "installed.dat"));
}

TEST_F(ExternalAppMappingTest, ReleaseLeavesRetargetedLinkUntouched) {
    const fs::path host = make_host_dir("Game");
    const fs::path other = make_host_dir("Other");
    ExternalAppMappingError error = ExternalAppMappingError::None;

    ExternalAppMapping mapping =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);
    ASSERT_TRUE(mapping.is_owned());

    const std::string link = mapping.link_path();
    fs::remove(link);
    fs::create_directory_symlink(other, link);

    mapping.release();
    ASSERT_TRUE(fs::is_symlink(link));
    EXPECT_EQ(fs::read_symlink(link), other);
}

TEST_F(ExternalAppMappingTest, DestructorRemovesOwnedMapping) {
    const fs::path host = make_host_dir("Scoped");
    std::string link;
    {
        ExternalAppMappingError error = ExternalAppMappingError::None;
        ExternalAppMapping mapping =
            ExternalAppMapping::claim(capsule.string(), host.string(), error);
        ASSERT_TRUE(mapping.is_owned());
        link = mapping.link_path();
        EXPECT_TRUE(fs::is_symlink(link));
    }
    EXPECT_FALSE(fs::exists(fs::symlink_status(link)));
}

TEST_F(ExternalAppMappingTest, DetachTransfersOwnershipWithoutRemoving) {
    const fs::path host = make_host_dir("Deferred");
    std::string link;
    std::string target;
    {
        ExternalAppMappingError error = ExternalAppMappingError::None;
        ExternalAppMapping mapping =
            ExternalAppMapping::claim(capsule.string(), host.string(), error);
        ASSERT_TRUE(mapping.is_owned());
        auto detached = mapping.detach();
        link = std::move(detached.link_path);
        target = std::move(detached.target_path);
        if (detached.lease_fd >= 0) (void)::close(detached.lease_fd);
        EXPECT_FALSE(mapping.is_owned());
    }
    EXPECT_TRUE(fs::is_symlink(link));
    EXPECT_EQ(target, host.string());
}

TEST_F(ExternalAppMappingTest, ReclaimsDanglingLinkFromCrashedRun) {
    const fs::path host = make_host_dir("Recovered");
    std::error_code ec;
    fs::create_directories(apps_dir(), ec);
    fs::create_directory_symlink(root / "gone", apps_dir() / "Recovered");

    ExternalAppMappingError error = ExternalAppMappingError::None;
    ExternalAppMapping mapping =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);

    ASSERT_TRUE(mapping.is_owned());
    EXPECT_EQ(mapping.guest_directory_name(), "Recovered");
    EXPECT_EQ(fs::read_symlink(mapping.link_path()), host);
}

TEST_F(ExternalAppMappingTest, ReclaimsSameTargetAfterLeaseOwnerDies) {
    const fs::path host = make_host_dir("RecoveredSameTarget");
    ExternalAppMappingError error = ExternalAppMappingError::None;

    ExternalAppMapping first =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);
    ASSERT_TRUE(first.is_owned());
    auto abandoned = first.detach();
    ASSERT_GE(abandoned.lease_fd, 0);
    (void)::close(abandoned.lease_fd);  // Model process death: locks close.

    ExternalAppMapping recovered =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);
    ASSERT_TRUE(recovered.is_owned())
        << external_app_mapping_error_message(error);
    EXPECT_EQ(recovered.link_path(), abandoned.link_path);
    EXPECT_EQ(fs::read_symlink(recovered.link_path()), host);
}

TEST_F(ExternalAppMappingTest, MissingHostDirectoryFailsHonestly) {
    ExternalAppMappingError error = ExternalAppMappingError::None;
    ExternalAppMapping mapping = ExternalAppMapping::claim(
        capsule.string(), (root / "does-not-exist").string(), error);

    EXPECT_FALSE(mapping.is_owned());
    EXPECT_EQ(error, ExternalAppMappingError::InvalidHostDirectory);
    EXPECT_TRUE(mapping.guest_directory_name().empty());
}

TEST_F(ExternalAppMappingTest, ExhaustedCandidatesFailInsteadOfOverwriting) {
    const fs::path host = make_host_dir("Crowded");
    ExternalAppMappingError error = ExternalAppMappingError::None;

    // Occupy every candidate name with real directories.
    std::vector<ExternalAppMapping> probes;
    for (int i = 0; i < 64; ++i) {
        ExternalAppMapping probe =
            ExternalAppMapping::claim(capsule.string(), host.string(), error);
        if (!probe.is_owned()) break;
        const std::string link = probe.link_path();
        (void)probe.detach();
        std::error_code ec;
        fs::remove(link, ec);
        fs::create_directories(link, ec);
    }

    ExternalAppMapping mapping =
        ExternalAppMapping::claim(capsule.string(), host.string(), error);
    EXPECT_FALSE(mapping.is_owned());
    EXPECT_EQ(error, ExternalAppMappingError::NoAvailableName);
}

} // namespace
} // namespace vitreo::cli

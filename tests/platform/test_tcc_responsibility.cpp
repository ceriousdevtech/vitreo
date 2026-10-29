// SPDX-License-Identifier: LGPL-2.1-only
//
// Which process macOS privacy consent judges. TCC itself is not exercised
// (that needs a person to answer a prompt); these tests cover what decides
// whom it asks: the responsibility attribute on spawn, and the Info.plist
// lookup TCC applies to the responsible code.

#include "platform/macos/tcc_responsibility.hpp"

#include <gtest/gtest.h>

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace vitreo::platform {
namespace {

/// A child that lives until killed, and its responsible process.
pid_t responsible_of_sleeper(bool disclaim) {
    char path[] = "/bin/sleep";
    char seconds[] = "30";
    char* argv[] = {path, seconds, nullptr};
    pid_t child = 0;
    int rc = 0;
    if (disclaim) {
        rc = spawn_disclaiming_responsibility(&child, path, argv, *_NSGetEnviron(), false);
    } else {
        rc = posix_spawn(&child, path, nullptr, nullptr, argv, *_NSGetEnviron());
    }
    EXPECT_EQ(rc, 0);
    if (rc != 0) return -1;
    pid_t responsible = -1;
    // The responsibility is fixed at spawn; read it while the child lives.
    for (int i = 0; i < 100 && responsible <= 0; ++i) {
        responsible = responsible_process(child);
        if (responsible <= 0) ::usleep(1000);
    }
    ::kill(child, SIGKILL);
    int status = 0;
    (void)::waitpid(child, &status, 0);
    return responsible == child ? 0 : responsible;  // 0: its own
}

TEST(TccResponsibilityTest, DisclaimedChildIsResponsibleForItself) {
    if (!can_disclaim_responsibility()) GTEST_SKIP() << "no responsibility SPI on this host";
    const pid_t mine = responsible_process(::getpid());
    ASSERT_GT(mine, 0);
    // An ordinary child is judged as whoever this test process is judged as;
    // a disclaimed one as itself.
    EXPECT_EQ(responsible_of_sleeper(false), mine);
    EXPECT_EQ(responsible_of_sleeper(true), 0);
}

TEST(TccResponsibilityTest, ExecutablePathOfARunningProcess) {
    char own[4096];
    uint32_t size = sizeof(own);
    ASSERT_EQ(_NSGetExecutablePath(own, &size), 0);
    EXPECT_EQ(std::filesystem::canonical(process_executable_path(::getpid())),
              std::filesystem::canonical(own));
    EXPECT_EQ(process_executable_path(-5), "");
}

TEST(TccResponsibilityTest, UsageStringsComeFromTheCodesInfoPlist) {
    // This test binary embeds src/app/macos/vitreo-runtime-Info.plist the way
    // vitreo does (__TEXT,__info_plist): the plist TCC reads when a guest is
    // responsible for itself declares the microphone and Bluetooth purposes.
    const std::string self = process_executable_path(::getpid());
    EXPECT_TRUE(code_declares_usage(self, "NSMicrophoneUsageDescription"));
    EXPECT_TRUE(code_declares_usage(self, "NSBluetoothAlwaysUsageDescription"));
    EXPECT_FALSE(code_declares_usage(self, "NSCameraUsageDescription"));
    // A bare executable without an embedded plist declares nothing.
    EXPECT_FALSE(code_declares_usage("/bin/sleep", "NSMicrophoneUsageDescription"));
    EXPECT_FALSE(code_declares_usage("", "NSMicrophoneUsageDescription"));
    // An executable inside an application bundle: the bundle's Info.plist.
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("vitreo-tcc-test-" + std::to_string(::getpid()));
    const fs::path macos = root / "Probe.app" / "Contents" / "MacOS";
    fs::create_directories(macos);
    std::ofstream(root / "Probe.app" / "Contents" / "Info.plist")
        << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
           "<plist version=\"1.0\"><dict>"
           "<key>CFBundleIdentifier</key><string>dev.vitreo.test.probe</string>"
           "<key>CFBundleExecutable</key><string>Probe</string>"
           "<key>NSMicrophoneUsageDescription</key><string>test</string>"
           "<key>NSCameraUsageDescription</key><string></string>"
           "</dict></plist>\n";
    std::ofstream(macos / "Probe") << "#!/bin/sh\n";
    EXPECT_TRUE(code_declares_usage((macos / "Probe").string(), "NSMicrophoneUsageDescription"));
    EXPECT_FALSE(code_declares_usage((macos / "Probe").string(), "NSCameraUsageDescription"));
    EXPECT_FALSE(code_declares_usage((macos / "Probe").string(),
                                     "NSBluetoothAlwaysUsageDescription"));
    fs::remove_all(root);
}

}  // namespace
}  // namespace vitreo::platform

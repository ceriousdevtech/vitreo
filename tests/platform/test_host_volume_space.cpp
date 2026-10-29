// SPDX-License-Identifier: LGPL-2.1-only
#include "platform/host_volume_space.hpp"

#include <gtest/gtest.h>

#include <CoreFoundation/CoreFoundation.h>
#include <sys/statvfs.h>

#include <cstring>
#include <filesystem>

namespace vitreo::platform {
namespace {

/// Apple's figure for space an explicit user request may claim, read
/// straight from CFURL (0 when the volume does not answer).
u64 important_usage(const char* path) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(path),
        static_cast<CFIndex>(std::strlen(path)), true);
    if (url == nullptr) return 0;
    CFTypeRef value = nullptr;
    SInt64 n = 0;
    if (CFURLCopyResourcePropertyForKey(
            url, kCFURLVolumeAvailableCapacityForImportantUsageKey, &value, nullptr) &&
        value != nullptr) {
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &n);
        CFRelease(value);
    }
    CFRelease(url);
    return n > 0 ? static_cast<u64>(n) : 0;
}

TEST(HostVolumeSpaceTest, CountsPurgeableSpaceAsAvailable) {
    const std::string dir = std::filesystem::temp_directory_path().string();
    HostVolumeSpace space;
    ASSERT_TRUE(host_volume_space(dir.c_str(), space));
    struct statvfs st {};
    ASSERT_EQ(::statvfs(dir.c_str(), &st), 0);
    const u64 unit = static_cast<u64>(st.f_frsize);
    EXPECT_EQ(space.allocation_unit, unit);
    EXPECT_EQ(space.total_bytes, static_cast<u64>(st.f_blocks) * unit);
    // Never less than the unallocated blocks, nor than what macOS would
    // free for an explicit request. Both move while the test runs, so allow
    // 1% of the volume between the two reads.
    const u64 slack = space.total_bytes / 100;
    EXPECT_GE(space.available_bytes + slack, static_cast<u64>(st.f_bavail) * unit);
    EXPECT_GE(space.available_bytes + slack, important_usage(dir.c_str()));
    EXPECT_LE(space.available_bytes, space.free_bytes);
    EXPECT_LE(space.free_bytes, space.total_bytes);
}

TEST(HostVolumeSpaceTest, MissingPathFailsAndZeroesTheOutput) {
    HostVolumeSpace space{1, 2, 3, 4};
    EXPECT_FALSE(host_volume_space("/nonexistent/vitreo/volume/space", space));
    EXPECT_EQ(space.total_bytes, 0u);
    EXPECT_EQ(space.free_bytes, 0u);
    EXPECT_EQ(space.available_bytes, 0u);
    EXPECT_EQ(space.allocation_unit, 0u);
    EXPECT_FALSE(host_volume_space(nullptr, space));
}

}  // namespace
}  // namespace vitreo::platform

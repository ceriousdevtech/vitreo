// SPDX-License-Identifier: LGPL-2.1-only
#include "platform/host_volume_space.hpp"

#include <CoreFoundation/CoreFoundation.h>

#include <sys/statvfs.h>

#include <algorithm>
#include <cstring>

namespace vitreo::platform {
namespace {

/// kCFURLVolumeAvailableCapacityForImportantUsageKey, or 0 when the volume
/// does not answer it (non-APFS, network and image volumes).
u64 important_usage_capacity(const char* path) noexcept {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path),
        static_cast<CFIndex>(std::strlen(path)), true);
    if (url == nullptr) return 0;
    u64 bytes = 0;
    CFTypeRef value = nullptr;
    if (CFURLCopyResourcePropertyForKey(
            url, kCFURLVolumeAvailableCapacityForImportantUsageKey, &value,
            nullptr) &&
        value != nullptr) {
        SInt64 n = 0;
        if (CFGetTypeID(value) == CFNumberGetTypeID() &&
            CFNumberGetValue(static_cast<CFNumberRef>(value),
                             kCFNumberSInt64Type, &n) &&
            n > 0) {
            bytes = static_cast<u64>(n);
        }
        CFRelease(value);
    }
    CFRelease(url);
    return bytes;
}

}  // namespace

bool host_volume_space(const char* path, HostVolumeSpace& out) noexcept {
    out = HostVolumeSpace{};
    struct statvfs st {};
    if (path == nullptr || ::statvfs(path, &st) != 0 || st.f_frsize == 0) {
        return false;
    }
    const u64 unit = static_cast<u64>(st.f_frsize);
    out.allocation_unit = unit;
    out.total_bytes = static_cast<u64>(st.f_blocks) * unit;
    out.available_bytes = static_cast<u64>(st.f_bavail) * unit;
    out.free_bytes = static_cast<u64>(st.f_bfree) * unit;

    // Purgeable space is free to whoever asks for it, so it raises both
    // counts. It can never exceed the volume.
    const u64 important =
        std::min(important_usage_capacity(path), out.total_bytes);
    out.available_bytes = std::max(out.available_bytes, important);
    out.free_bytes = std::max(out.free_bytes, out.available_bytes);
    return true;
}

}  // namespace vitreo::platform

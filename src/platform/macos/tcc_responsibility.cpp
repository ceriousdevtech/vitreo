// SPDX-License-Identifier: LGPL-2.1-only
#include "platform/macos/tcc_responsibility.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <libproc.h>
#include <spawn.h>

#include <cerrno>
#include <string_view>

// Darwin responsibility SPI (libSystem), the attribute macOS terminals and
// IDEs set on the programs they launch. Weakly imported: absent symbols read
// as null and the caller falls back to an ordinary exec.
extern "C" {
int responsibility_spawnattrs_setdisclaim(posix_spawnattr_t* attrs, int disclaim)
    __attribute__((weak_import));
pid_t responsibility_get_pid_responsible_for_pid(pid_t pid) __attribute__((weak_import));
}

namespace vitreo::platform {

pid_t responsible_process(pid_t pid) {
    if (responsibility_get_pid_responsible_for_pid == nullptr) return -1;
    return responsibility_get_pid_responsible_for_pid(pid);
}

bool can_disclaim_responsibility() {
    return responsibility_spawnattrs_setdisclaim != nullptr;
}

int spawn_disclaiming_responsibility(pid_t* pid, const char* path, char* const argv[],
                                     char* const envp[], bool replace_current) {
    if (!can_disclaim_responsibility()) return ENOTSUP;
    posix_spawnattr_t attributes;
    int result = posix_spawnattr_init(&attributes);
    if (result != 0) return result;
    if (replace_current) result = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETEXEC);
    if (result == 0) result = responsibility_spawnattrs_setdisclaim(&attributes, 1);
    if (result == 0) result = posix_spawn(pid, path, nullptr, &attributes, argv, envp);
    posix_spawnattr_destroy(&attributes);
    return result;
}

std::string process_executable_path(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    return proc_pidpath(pid, path, sizeof(path)) > 0 ? std::string(path) : std::string();
}

bool code_declares_usage(const std::string& executable, const char* key) {
    if (executable.empty() || key == nullptr) return false;
    // The code's own Info.plist: its application bundle's, or the plist
    // embedded in a bare executable (CFBundleCopyInfoDictionaryForURL reads
    // both forms).
    std::string target = executable;
    constexpr std::string_view marker = ".app/Contents/MacOS/";
    if (const auto at = executable.rfind(marker); at != std::string::npos &&
        executable.find('/', at + marker.size()) == std::string::npos) {
        target = executable.substr(0, at + 4);  // ".../<App>.app"
    }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(target.data()),
        static_cast<CFIndex>(target.size()), target.size() >= 4 &&
                                                 target.compare(target.size() - 4, 4, ".app") == 0);
    if (url == nullptr) return false;
    CFDictionaryRef info = CFBundleCopyInfoDictionaryForURL(url);
    CFRelease(url);
    if (info == nullptr) return false;
    CFStringRef name = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    bool declared = false;
    if (name != nullptr) {
        const void* value = CFDictionaryGetValue(info, name);
        declared = value != nullptr && CFGetTypeID(value) == CFStringGetTypeID() &&
                   CFStringGetLength(static_cast<CFStringRef>(value)) > 0;
        CFRelease(name);
    }
    CFRelease(info);
    return declared;
}

}  // namespace vitreo::platform

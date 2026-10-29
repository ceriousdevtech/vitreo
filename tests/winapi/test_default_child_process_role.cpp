// Keeps vitreo_winapi_tests out of the Dock.
//
// The user32 tests create windows through CocoaBridge. A session-root process
// takes NSApplicationActivationPolicyRegular, which turns every ctest-launched
// copy of this gtest binary into a Dock app, and macOS keeps reopening it
// after a kill. Run as a session child (accessory) unless the environment
// already names a role, as vitreo_windowing_tests does.

#include <cstdlib>

namespace {
struct DefaultChildProcessRole {
    DefaultChildProcessRole() {
        if (std::getenv("VITREO_MACOS_PROCESS_ROLE") == nullptr) {
            ::setenv("VITREO_MACOS_PROCESS_ROLE", "child", 0);
        }
    }
} g_default_child_process_role;
}  // namespace

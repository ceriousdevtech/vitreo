// SPDX-License-Identifier: LGPL-2.1-only
//
// A stand-in for a Vitreo host process, for test_scoped_environment.cpp.
//
// It has what matters about one: a knob read ONCE into a namespace-scope
// constant while the image's static initialisers run (the pattern of
// VITREO_JIT_MAP, jit_finalize.cpp), and a main() that gives
// VITREO_SCOPED_ENV its two chances at the process, exactly as vitreo's main()
// and execute_pe do.
//
//   scoped_environment_probe <guest command line> [--no-exe-path] [--clobber]
//
//   --no-exe-path  the image's own path is "unknown", so no re-exec can happen
//   --bad-exe-path the image's own path is wrong, so the exec itself fails
//   --clobber      remove the knob after the start-up re-exec, then ask for
//                  delivery again: the loop guard must stop a second exec
//
// Everything is written to stdout, one `key=value` or log line per line, so a
// test reads one ordered stream.

#include "platform/scoped_environment.hpp"

#include <mach-o/dyld.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr const char* kKnob = "VITREO_SCOPED_ENV_PROBE_KNOB";

// Static initialisation, as every latched knob does it.
const std::string g_latched = [] {
    const char* value = std::getenv(kKnob);
    return value != nullptr ? std::string(value) : std::string("<unset>");
}();

std::string own_path() {
    uint32_t size = 0;
    (void)_NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return buffer.data();
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) return 2;
    const std::string_view command_line = argv[1];
    bool no_exe_path = false;
    bool bad_exe_path = false;
    bool clobber = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-exe-path") == 0) no_exe_path = true;
        if (std::strcmp(argv[i], "--bad-exe-path") == 0) bad_exe_path = true;
        if (std::strcmp(argv[i], "--clobber") == 0) clobber = true;
    }

    if (clobber && vitreo::platform::scoped_environment_startup().reexeced) {
        ::unsetenv(kKnob);
    }
    const std::string path =
        no_exe_path ? std::string()
                    : bad_exe_path ? std::string("/nonexistent/probe") : own_path();
    vitreo::platform::reexec_for_scoped_environment(
        command_line, path.empty() ? nullptr : path.c_str(), argv, stdout);
    vitreo::platform::deliver_scoped_environment_for_guest(command_line, stdout);

    const auto& startup = vitreo::platform::scoped_environment_startup();
    std::printf("latched=%s\n", g_latched.c_str());
    std::printf("lazy=%s\n",
                std::getenv(kKnob) != nullptr ? std::getenv(kKnob) : "<unset>");
    std::printf("pid=%d\n", static_cast<int>(::getpid()));
    std::printf("reexeced=%d\n", startup.reexeced ? 1 : 0);
    std::fflush(stdout);
    return 0;
}

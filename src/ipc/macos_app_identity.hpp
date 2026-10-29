// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <uuid/uuid.h>

namespace vitreo::ipc {

/// One unguessable presentation identity per launch family. Unlike a PID or
/// capsule name it survives a launcher exiting and separates concurrent runs.
/// Workers receive it explicitly in their spawn environment.
inline const std::string& macos_app_identity() {
    static const std::string identity = [] {
        const char* role = std::getenv("VITREO_MACOS_PROCESS_ROLE");
        const char* inherited = std::getenv("VITREO_MACOS_APP_SESSION");
        uuid_t value{};
        if (role && std::strcmp(role, "child") == 0 && inherited &&
            uuid_parse(inherited, value) == 0) {
            return std::string(inherited);
        }
        uuid_generate_random(value);
        char text[37];
        uuid_unparse_lower(value, text);
        return std::string(text);
    }();
    return identity;
}

} // namespace vitreo::ipc

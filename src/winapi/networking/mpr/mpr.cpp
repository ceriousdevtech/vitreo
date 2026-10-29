/// @file mpr.cpp
/// @brief WNetGetUniversalNameA/W — drive-based path to UNC.

#include "mpr.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <cctype>

namespace vitreo::winapi {
namespace {

constexpr u32 ERROR_BAD_DEVICE = 1200;
constexpr u32 ERROR_NOT_CONNECTED = 2250;
constexpr u32 UNIVERSAL_NAME_INFO_LEVEL = 1;
constexpr u32 REMOTE_NAME_INFO_LEVEL = 2;

bool is_drive_path(const std::string& path) {
    if (path.size() < 2) return false;
    const unsigned char drive = static_cast<unsigned char>(path[0]);
    return std::isalpha(drive) != 0 && path[1] == ':';
}

void wnet_get_universal_name(Marshaler& m, bool wide) {
    const GuestAddr path_address = m.arg_ptr(0);
    const u32 info_level = static_cast<u32>(m.arg(1));
    const GuestAddr size_address = m.arg_ptr(3);
    if (path_address == 0 || size_address == 0) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (info_level != UNIVERSAL_NAME_INFO_LEVEL &&
        info_level != REMOTE_NAME_INFO_LEVEL) {
        m.set_return(error_codes::ERROR_INVALID_PARAMETER);
        return;
    }

    const auto path = wide ? m.read_wide_string(path_address)
                           : m.read_ansi_string(path_address);
    if (path.is_err() || !is_drive_path(path.value())) {
        m.set_return(ERROR_BAD_DEVICE);
        return;
    }

    // Learn: ERROR_NOT_CONNECTED if the device is not redirected. Vitreo's
    // capsule volumes are local, so a drive-based path has no UNC form.
    m.set_return(ERROR_NOT_CONNECTED);
}

}  // namespace

DllExports create_mpr_thunks() {
    DllExports exports;
    exports.entries.push_back(
        {"WNetGetUniversalNameW", 0,
         [](Marshaler& m) { wnet_get_universal_name(m, true); }, 16});
    exports.entries.push_back(
        {"WNetGetUniversalNameA", 0,
         [](Marshaler& m) { wnet_get_universal_name(m, false); }, 16});
    return exports;
}

}  // namespace vitreo::winapi

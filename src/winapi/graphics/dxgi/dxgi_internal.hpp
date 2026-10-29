// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file dxgi_internal.hpp
/// @brief File-local DXGI helpers shared by the dxgi*.cpp files: the
///        adapter LUID, the GuidBytes alias, guid_equal and dxgi_diag.
///        guid_equal sits in an anonymous namespace (d3d11_internal.hpp
///        has an external vitreo::winapi::guid_equal with the same
///        signature); dxgi_diag is an external inline function so the
///        process keeps one cached VITREO_DXGI_DIAG check. Include from
///        those files only. Split out of dxgi.cpp as verbatim code motion.

#include "winapi/graphics/dxgi/dxgi.hpp"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

namespace vitreo::winapi {
namespace {

using dxgi::ADAPTER_LUID_HIGH;
using dxgi::ADAPTER_LUID_LOW;

using GuidBytes = std::array<u8, 16>;

[[nodiscard]] inline bool guid_equal(const GuidBytes& left,
                                     const GuidBytes& right) noexcept {
    return left == right;
}

} // namespace

inline void dxgi_diag(const char* format, ...) {
    static const bool enabled = [] {
        const char* value = std::getenv("VITREO_DXGI_DIAG");
        if (value != nullptr && value[0] != '\0' &&
            std::strcmp(value, "0") != 0) {
            return true;
        }
        return ::access("/tmp/vitreo-dxgi-diag", F_OK) == 0;
    }();
    if (!enabled) return;

    std::fprintf(stderr, "[dxgi-diag] ");
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

} // namespace vitreo::winapi

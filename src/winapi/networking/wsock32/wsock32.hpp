// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file wsock32.hpp
/// @brief WSOCK32.dll API stubs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// Wsock32Manager
// =============================================================================

class Wsock32Manager {
public:
    Wsock32Manager() = default;
    ~Wsock32Manager() = default;

    Wsock32Manager(const Wsock32Manager&) = delete;
    Wsock32Manager& operator=(const Wsock32Manager&) = delete;

    /// WSAStartup — zero 400 bytes at data_ptr, fill version fields, return 0
    [[nodiscard]] u32 wsa_startup(Marshaler& m, u16 version_requested,
                                   GuestAddr data_ptr);
};

/// Create wsock32.dll export thunks
DllExports create_wsock32_thunks(Wsock32Manager& mgr);

} // namespace vitreo::winapi

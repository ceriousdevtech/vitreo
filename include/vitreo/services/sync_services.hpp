// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service group: synchronization primitives

#pragma once

#include <vitreo/types.hpp>

namespace vitreo::winapi {
class SyncManager;
class CriticalSectionManager;
class TlsManager;
class FlsManager;
} // namespace vitreo::winapi

namespace vitreo {

struct SyncServices {
    static constexpr u32 kServiceId = 4;

    winapi::SyncManager&            sync_mgr;
    winapi::CriticalSectionManager& cs_mgr;
    winapi::TlsManager&             tls_mgr;
    winapi::FlsManager&             fls_mgr;
};

} // namespace vitreo

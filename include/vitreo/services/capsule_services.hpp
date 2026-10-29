// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service group: capsule (VFS, registry, environment)

#pragma once

#include <vitreo/types.hpp>

namespace vitreo::capsule {
class Vfs;
class Registry;
} // namespace vitreo::capsule

namespace vitreo::winapi {
class EnvironmentManager;
} // namespace vitreo::winapi

namespace vitreo {

struct CapsuleServices {
    static constexpr u32 kServiceId = 6;

    capsule::Vfs&               vfs;
    capsule::Registry&          registry;
    winapi::EnvironmentManager& env_mgr;
};

} // namespace vitreo

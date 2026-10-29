// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service group: module / DLL management

#pragma once

#include <vitreo/types.hpp>

namespace vitreo::winapi {
class ModuleManager;
class Kernel32ExtManager;
class CrtDataBlock;
} // namespace vitreo::winapi

namespace vitreo {

struct ModuleServices {
    static constexpr u32 kServiceId = 3;

    winapi::ModuleManager&      module_mgr;
    winapi::Kernel32ExtManager& k32_ext_mgr;
    winapi::CrtDataBlock&       crt_data;
};

} // namespace vitreo

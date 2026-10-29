// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service group: process & thread management

#pragma once

#include <vitreo/types.hpp>

namespace vitreo::winapi {
class ProcessManager;
class ThreadManager;
} // namespace vitreo::winapi

namespace vitreo {

struct ProcessServices {
    static constexpr u32 kServiceId = 0;

    winapi::ProcessManager& process_mgr;
    winapi::ThreadManager&  thread_mgr;
};

} // namespace vitreo

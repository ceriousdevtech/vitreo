// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service group: file I/O

#pragma once

#include <vitreo/types.hpp>

namespace vitreo::winapi {
class FileManager;
class FileMappingManager;
class FileSearchManager;
class ConsoleManager;
class FileOpsManager;
} // namespace vitreo::winapi

namespace vitreo {

struct FileServices {
    static constexpr u32 kServiceId = 2;

    winapi::FileManager&        file_mgr;
    winapi::FileMappingManager& fmm_mgr;
    winapi::FileSearchManager&  fsearch_mgr;
    winapi::ConsoleManager&     console_mgr;
    winapi::FileOpsManager&     file_ops_mgr;
};

} // namespace vitreo

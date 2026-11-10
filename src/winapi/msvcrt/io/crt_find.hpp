// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_find.hpp
/// @brief The CRT's _findfirst / _findnext / _findclose family.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class CrtMemoryManager;
class FileSearchManager;

/// Register msvcrt's _find* / _wfind* exports (plain, i64 and 64 forms) and
/// the UCRT's 32 / 32i64 / 64i32 / 64 forms over kernel32's file search, so a
/// CRT search handle is the FindFirstFile handle, as on Windows.
DllExports create_crt_find_thunks(FileSearchManager& search,
                                  CrtMemoryManager& memory_manager);

} // namespace vitreo::winapi

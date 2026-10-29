// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_filesystem.hpp
/// @brief UCRT filesystem API set (api-ms-win-crt-filesystem-l1-1-0)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

namespace vitreo {

namespace capsule { class Vfs; struct FileInfo; }

namespace winapi {

class CrtMemoryManager;
class Marshaler;

/// Write a `struct _stat64` (or `_stat64i32`) into guest memory at @p out.
/// Shared with the low-level `_fstat64` in the stdio module.
bool write_crt_stat(Marshaler& m, GuestAddr out, const capsule::FileInfo& info,
                    bool size_is_32bit, bool time_is_32bit, u32 drive_number,
                    bool executable, bool& time_overflow);

bool write_stat64(Marshaler& m, GuestAddr out, const capsule::FileInfo& info,
                  bool size_is_32bit);

/// Build the DllExports for the path-based CRT filesystem functions. Everything
/// here goes through the capsule VFS, so guest paths stay guest paths.
DllExports create_crt_filesystem_thunks(capsule::Vfs& vfs,
                                        CrtMemoryManager& memory_manager);

} // namespace winapi
} // namespace vitreo

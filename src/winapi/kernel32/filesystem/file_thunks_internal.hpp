// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file file_thunks_internal.hpp
/// @brief Shared thunk entry declarations for FileManager helper TUs.

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/core/marshaler.hpp"

namespace vitreo::winapi {

void impl_CreateFileW(Marshaler& m, FileManager& fm);
void impl_CreateFileA(Marshaler& m, FileManager& fm);
void impl_ReOpenFile(Marshaler& m, FileManager& fm);
void impl_WriteFile(Marshaler& m, FileManager& fm);
void impl_ReadFile(Marshaler& m, FileManager& fm);
void impl_ReadFileEx(Marshaler& m, FileManager& fm);
void impl_WriteFileEx(Marshaler& m, FileManager& fm);

} // namespace vitreo::winapi

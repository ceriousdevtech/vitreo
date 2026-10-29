#pragma once

/// @file filepos.hpp
/// @brief GetFileSize/GetFileSizeEx/SetFilePointer/SetFilePointerEx thunks (WIN32-037)
///
/// Provides thunk wrappers that bridge Win32 file-positioning APIs to the
/// existing FileManager::get_file_size() and FileManager::set_file_pointer()
/// methods, plus a registration function for the export table.

#include <vitreo/common.hpp>
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

class Marshaler;
class NtdllExtManager;

// =============================================================================
// Win32 Constants
// =============================================================================

/// INVALID_SET_FILE_POINTER — returned by SetFilePointer on failure
constexpr u32 INVALID_SET_FILE_POINTER = 0xFFFFFFFF;

/// INVALID_FILE_SIZE — returned by GetFileSize on failure
constexpr u32 INVALID_FILE_SIZE = 0xFFFFFFFF;

// =============================================================================
// Thunk Implementations
// =============================================================================

/// GetFileSize thunk
///
/// DWORD WINAPI GetFileSize(
///     HANDLE  hFile,           // arg0
///     LPDWORD lpFileSizeHigh   // arg1  (guest pointer, may be NULL)
/// );
///
/// Returns low 32 bits of file size; writes high 32 bits through
/// lpFileSizeHigh if non-null.  Returns INVALID_FILE_SIZE on failure.
void impl_GetFileSize(Marshaler& m, FileManager& fm,
                      NtdllExtManager* nt_files = nullptr);

/// GetFileSizeEx thunk
///
/// BOOL WINAPI GetFileSizeEx(
///     HANDLE         hFile,       // arg0
///     PLARGE_INTEGER lpFileSize   // arg1  (guest pointer to i64)
/// );
///
/// Writes full 64-bit size to *lpFileSize.  Returns TRUE (1) / FALSE (0).
void impl_GetFileSizeEx(Marshaler& m, FileManager& fm,
                        NtdllExtManager* nt_files = nullptr);

/// SetFilePointer thunk
///
/// DWORD WINAPI SetFilePointer(
///     HANDLE hFile,                // arg0
///     LONG   lDistanceToMove,      // arg1  (i32)
///     PLONG  lpDistanceToMoveHigh, // arg2  (guest pointer, may be NULL)
///     DWORD  dwMoveMethod          // arg3
/// );
///
/// Returns low 32 bits of new position or INVALID_SET_FILE_POINTER on error.
void impl_SetFilePointer(Marshaler& m, FileManager& fm);

/// SetFilePointerEx thunk
///
/// BOOL WINAPI SetFilePointerEx(
///     HANDLE         hFile,             // arg0
///     LARGE_INTEGER  liDistanceToMove,  // arg1+arg2 (i64 split across two u32 slots)
///     PLARGE_INTEGER lpNewFilePointer,  // arg3  (guest pointer, may be NULL)
///     DWORD          dwMoveMethod       // arg4
/// );
///
/// Returns TRUE (1) / FALSE (0).
void impl_SetFilePointerEx(Marshaler& m, FileManager& fm);

// =============================================================================
// Export Registration
// =============================================================================

/// Build the DllExports table for the four file-positioning APIs.
[[nodiscard]] DllExports make_filepos_exports(
    FileManager& fm, NtdllExtManager* nt_files = nullptr);

} // namespace vitreo::winapi

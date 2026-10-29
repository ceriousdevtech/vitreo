#pragma once

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

class ComCoreManager;
class RegistryThunks;
class LocaleStringManager;
class GlobalLocalManager;

/// The process's CompareString implementation, which the StrCmpN / StrCmpI
/// family compares with (set once at process start; without it those
/// exports fail as CompareString does without a locale: 0 - CSTR_EQUAL).
void set_shlwapi_locale_strings(LocaleStringManager* strings);

/// Minimal SHLWAPI exports for shell and installer callers.
///
/// This is intentionally small and will grow as we observe real-world
/// GetProcAddress usage.
/// @param vfs VFS for PathFileExistsW; may be nullptr for tests.
/// @param com Process-owned COM manager used for SHCreateMemStream.
/// @param registry Registry thunks backing SHDeleteKeyA/W. When null those
///        exports report ERROR_INVALID_HANDLE rather than pretending to
///        delete anything.
/// @param local_heap LocalAlloc for PathCreateFromUrlAlloc (E_OUTOFMEMORY
///        without one).
DllExports create_shlwapi_thunks(capsule::Vfs* vfs = nullptr,
                                 ComCoreManager* com = nullptr,
                                 RegistryThunks* registry = nullptr,
                                 GlobalLocalManager* local_heap = nullptr);

} // namespace vitreo::winapi

#pragma once

/// @file pathcch.hpp
/// @brief `PathCch*` exports of api-ms-win-core-path-l1-1-0.dll.
///
/// Real Windows implements this contract in KernelBase.dll and forwards the
/// API-set DLL to it, so Vitreo publishes the same surface from kernel32 (the
/// API-set resolver's target for `api-ms-win-core-*`) and from kernelbase.

#include "winapi/core/thunk_registry.hpp"

namespace vitreo::winapi {

/// Build the PathCch export set. Stateless: these APIs are pure string
/// algebra over caller-owned guest buffers.
[[nodiscard]] DllExports make_pathcch_exports();

} // namespace vitreo::winapi

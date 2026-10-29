#pragma once

/// @file d3dkmt_dxcore.hpp
/// @brief Ordinal exports for ext-ms-win-dx-d3dkmt-dxcore-l1-1-{0,1,3,4,5}.
///
/// Ordinal → D3DKMT* name identity is taken from dumpbin /exports of
/// gdi32.dll on Windows (forwards to ext-ms-win-dx-d3dkmt-dxcore-l1-1-5.#N).
/// Vitreo has no WDDM kernel path, so each body fails loudly with
/// STATUS_NOT_IMPLEMENTED after GetProcAddress has resolved the ordinal.

#include "winapi/core/thunk_registry.hpp"

#include <string_view>

namespace vitreo::winapi {

/// Register the D3DKMT ordinals imported from this contract DLL.
[[nodiscard]] DllExports create_d3dkmt_dxcore_thunks(std::string_view contract_dll);

} // namespace vitreo::winapi

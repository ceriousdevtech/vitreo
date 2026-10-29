// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d9_device_internal.hpp
/// @brief Helpers shared between the IDirect3DDevice9 dispatch translation
///        units.
///
/// Private to the D3D9 frontend and not part of the d3d9 interface. These
/// were file-local helpers in an anonymous namespace at the top of
/// d3d9_device.cpp before that file was split by responsibility; only the
/// d3d9_device*.cpp parts that use them include this. Other d3d9 .cpp files
/// keep their own anonymous-namespace copies and must not include it.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include <cstdlib>

namespace vitreo::winapi {

inline constexpr u32 D3DUSAGE_RENDERTARGET = 0x00000001;
inline constexpr u32 D3DUSAGE_DEPTHSTENCIL = 0x00000002;

[[nodiscard]] inline bool d3d9_sequence_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_SEQUENCE_DIAG") != nullptr;
    return enabled;
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_caps_log.hpp
/// @brief /tmp/vitreo-d3d11-caps: log what a guest asks of D3D11CreateDevice
///        and the device's capability queries, and what it was told.
///
/// DIAGNOSTIC, log only: it changes no behaviour (it reads the guest's
/// arguments and the answer the method already produced, after the method
/// returned). A guest launched by a store client does not reliably inherit
/// environment variables, so the switch is a marker file, re-probed at most
/// once a second (live_diag.hpp); with it absent each guarded call costs the
/// marker check.
///
/// Lines, tag `d3d11-caps` (live_diag::caps formats):
///   D3D11CreateDevice[AndSwapChain] driver_type flags requested_fl chosen_fl hr
///   CheckFeatureSupport feature name size hr data=<returned bytes as hex>
///   CheckFormatSupport format hr support
///   CheckMultisampleQualityLevels[1] format samples [flags] hr levels
///   Adapter IDXGIAdapter::GetDesc[1|2|3] ... (dxgi.cpp)
/// A FORMAT_SUPPORT2 query is the CheckFeatureSupport line with
/// name=FORMAT_SUPPORT2 and its format / support fields.

#include <vitreo/types.hpp>

#include "winapi/graphics/common/live_diag.hpp"

namespace vitreo::winapi {

class Marshaler;

/// Logs one device capability query when its method returns.
class CapsQueryLog {
public:
    enum class Kind : u8 {
        CheckFormatSupport,
        CheckMultisampleQualityLevels,
        CheckMultisampleQualityLevels1,
        CheckFeatureSupport,
    };

    CapsQueryLog(Marshaler& m, Kind kind) noexcept;
    ~CapsQueryLog();
    CapsQueryLog(const CapsQueryLog&) = delete;
    CapsQueryLog& operator=(const CapsQueryLog&) = delete;

private:
    Marshaler& m_marshaler;
    Kind m_kind;
    bool m_enabled;
    /// Arguments after `this`, captured at entry.
    u64 m_arg1{0};
    u64 m_arg2{0};
    u64 m_arg3{0};
    u64 m_arg4{0};
};

/// Logs a D3D11CreateDevice / D3D11CreateDeviceAndSwapChain call when it
/// returns: the levels it asked for, the one it got, its flags and its
/// HRESULT. @p chosen_level points at the variable the call selects the level
/// into (declare the log after it, so it outlives the log).
class CreateDeviceLog {
public:
    CreateDeviceLog(Marshaler& m, const char* api, u32 driver_type, u32 flags,
                    GuestAddr feature_levels, u32 feature_level_count,
                    const u32* chosen_level) noexcept;
    ~CreateDeviceLog();
    CreateDeviceLog(const CreateDeviceLog&) = delete;
    CreateDeviceLog& operator=(const CreateDeviceLog&) = delete;

private:
    Marshaler& m_marshaler;
    const char* m_api;
    bool m_enabled;
    u32 m_driver_type;
    u32 m_flags;
    GuestAddr m_levels;
    u32 m_level_count;
    const u32* m_chosen;
};

}  // namespace vitreo::winapi

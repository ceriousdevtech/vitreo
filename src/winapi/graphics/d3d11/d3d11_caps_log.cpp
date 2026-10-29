// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_caps_log.cpp
/// @brief /tmp/vitreo-d3d11-caps (see d3d11_caps_log.hpp). Log only.

#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"

#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <span>

namespace vitreo::winapi {

namespace {

/// CheckFeatureSupport's largest data structure is 56 bytes
/// (D3D11_FEATURE_DATA_D3D11_OPTIONS); a caller's size past that is logged as
/// given but its buffer is read only this far.
constexpr u32 kMaxLoggedBytes = 256;

}  // namespace

CapsQueryLog::CapsQueryLog(Marshaler& m, Kind kind) noexcept
    : m_marshaler(m), m_kind(kind), m_enabled(live_diag::caps_log_enabled()) {
    if (!m_enabled) return;
    m_arg1 = m.arg(1);
    m_arg2 = m.arg(2);
    m_arg3 = m.arg(3);
    m_arg4 = m.arg(4);
}

CapsQueryLog::~CapsQueryLog() {
    if (!m_enabled) return;
    using live_diag::caps::kTag;
    const u32 hr = static_cast<u32>(m_marshaler.return_value());
    GuestMemory& memory = m_marshaler.memory();
    // The output the guest sees, read after the method wrote it (or, on a
    // refusal, whatever it left there).
    const auto read_u32_at = [&](GuestAddr address, u32& out) {
        if (address == 0) return false;
        const auto value = memory.read_u32(address);
        if (value.is_err()) return false;
        out = value.value();
        return true;
    };
    switch (m_kind) {
    case Kind::CheckFeatureSupport: {
        const u32 feature = static_cast<u32>(m_arg1);
        const GuestAddr data = static_cast<GuestAddr>(m_arg2);
        const u32 size = static_cast<u32>(m_arg3);
        std::array<u8, kMaxLoggedBytes> bytes{};
        const u32 count = std::min(size, kMaxLoggedBytes);
        const bool readable =
            data != 0 && memory.read_bytes(data, bytes.data(), count).is_ok();
        live_diag::emit(kTag, live_diag::caps::format_check_feature_support(
                                  feature, size, hr, readable,
                                  std::span<const u8>(bytes.data(),
                                                      readable ? count : 0)));
        break;
    }
    case Kind::CheckFormatSupport: {
        u32 mask = 0;
        (void)read_u32_at(static_cast<GuestAddr>(m_arg2), mask);
        live_diag::emit(kTag, live_diag::caps::format_check_format_support(
                                  static_cast<u32>(m_arg1), hr, mask));
        break;
    }
    case Kind::CheckMultisampleQualityLevels: {
        u32 levels = 0;
        (void)read_u32_at(static_cast<GuestAddr>(m_arg3), levels);
        live_diag::emit(kTag, live_diag::caps::format_check_multisample(
                                  "CheckMultisampleQualityLevels",
                                  static_cast<u32>(m_arg1),
                                  static_cast<u32>(m_arg2), false, 0, hr,
                                  levels));
        break;
    }
    case Kind::CheckMultisampleQualityLevels1: {
        u32 levels = 0;
        (void)read_u32_at(static_cast<GuestAddr>(m_arg4), levels);
        live_diag::emit(kTag, live_diag::caps::format_check_multisample(
                                  "CheckMultisampleQualityLevels1",
                                  static_cast<u32>(m_arg1),
                                  static_cast<u32>(m_arg2), true,
                                  static_cast<u32>(m_arg3), hr, levels));
        break;
    }
    }
}

CreateDeviceLog::CreateDeviceLog(Marshaler& m, const char* api,
                                 u32 driver_type, u32 flags,
                                 GuestAddr feature_levels,
                                 u32 feature_level_count,
                                 const u32* chosen_level) noexcept
    : m_marshaler(m),
      m_api(api),
      m_enabled(live_diag::caps_log_enabled()),
      m_driver_type(driver_type),
      m_flags(flags),
      m_levels(feature_levels),
      m_level_count(feature_level_count),
      m_chosen(chosen_level) {}

CreateDeviceLog::~CreateDeviceLog() {
    if (!m_enabled) return;
    // The guest's array of D3D_FEATURE_LEVELs. D3D11 takes at most 9 (the
    // runtime refuses more); read only what is there.
    std::array<u32, 16> levels{};
    const u32 count = std::min<u32>(m_level_count, 16);
    usize read = 0;
    if (m_levels != 0) {
        for (; read < count; ++read) {
            const auto level = m_marshaler.memory().read_u32(m_levels + read * 4);
            if (level.is_err()) break;
            levels[read] = level.value();
        }
    }
    live_diag::emit(
        live_diag::caps::kTag,
        live_diag::caps::format_create_device(
            m_api, m_driver_type, m_flags,
            std::span<const u32>(levels.data(), read), m_levels != 0,
            m_chosen != nullptr ? *m_chosen : 0,
            static_cast<u32>(m_marshaler.return_value())));
}

}  // namespace vitreo::winapi

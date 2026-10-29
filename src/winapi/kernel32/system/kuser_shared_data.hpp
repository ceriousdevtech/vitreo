#pragma once

/// @file kuser_shared_data.hpp
/// @brief User-mapped KUSER_SHARED_DATA at 0x7FFE0000.

#include <vitreo/common.hpp>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

/// Map the 4 KiB SharedUserData page and write the static fields plus the
/// first clock snapshot. Safe to call once per process GuestMemory.
/// @p long_mode selects the virtual CPUID ProcessorFeatures[] is derived from.
[[nodiscard]] bool install_kuser_shared_data(GuestMemory& memory,
                                             bool long_mode = true);

/// Refresh TickCount / InterruptTime / SystemTime. Cheap; call from the
/// guest run-loop pump so lock-free ntdll readers see wall time advance.
void refresh_kuser_shared_data();

}  // namespace vitreo::winapi

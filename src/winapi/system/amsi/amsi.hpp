#pragma once

/// @file amsi.hpp
/// @brief Antimalware Scan Interface (amsi.dll).
///
/// AMSI is the documented interface a script host uses to hand content to
/// whatever antimalware provider the machine has registered. PowerShell,
/// the Windows Script Host, Office VBA and the .NET runtime all call it
/// before evaluating script content, so a capsule without amsi.dll cannot
/// run any of them.
///
/// A capsule has no antimalware provider, which is a state real Windows
/// also supports: with no provider registered, AmsiInitialize still
/// succeeds and every scan reports AMSI_RESULT_NOT_DETECTED. That is the
/// behaviour modelled here -- not a stub that fails, which would push
/// every caller down its degraded path.
///
/// https://learn.microsoft.com/en-us/windows/win32/api/amsi/

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <mutex>
#include <unordered_set>

namespace vitreo::winapi {

/// AMSI_RESULT (amsi.h). AmsiResultIsMalware() is a client-side macro over
/// these values, not an export, so only the enumeration matters here.
namespace amsi_result {
constexpr u32 kClean = 0;
constexpr u32 kNotDetected = 1;
constexpr u32 kBlockedByAdminStart = 0x4000;
constexpr u32 kBlockedByAdminEnd = 0x4FFF;
constexpr u32 kDetected = 32768;
}  // namespace amsi_result

/// Tracks the opaque HAMSICONTEXT / HAMSISESSION handles handed to the
/// guest so a stale or forged handle is rejected rather than trusted.
class AmsiManager {
public:
    AmsiManager() = default;

    AmsiManager(const AmsiManager&) = delete;
    AmsiManager& operator=(const AmsiManager&) = delete;

    [[nodiscard]] u64 create_context();
    [[nodiscard]] bool context_valid(u64 context) const;
    void destroy_context(u64 context);

    [[nodiscard]] u64 create_session();
    [[nodiscard]] bool session_valid(u64 session) const;
    void destroy_session(u64 session);

private:
    mutable std::mutex mutex_;
    // Handles are deliberately not pointers into host memory: the guest
    // only ever passes them back, and a value it can dereference is a
    // needless way to let a bad guest corrupt the host.
    std::unordered_set<u64> contexts_;
    std::unordered_set<u64> sessions_;
    u64 next_context_{0x414D5300ull};  // 'AMS\0'
    u64 next_session_{0x414D5301ull};
};

DllExports create_amsi_thunks(AmsiManager& manager);

}  // namespace vitreo::winapi

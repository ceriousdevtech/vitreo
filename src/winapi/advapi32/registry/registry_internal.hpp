#pragma once
/// @file registry_internal.hpp
/// @brief Shared surface between registry.cpp and registry_registration.cpp.
///
/// Private to advapi32's registry implementation. These were file-local before
/// the export table was split out; this header exists so the two halves can
/// still see each other, not to publish them.
///
/// The registry API is doubled across A and W forms and the table's entries do
/// the widening at the registration site, which is why the string helpers are
/// reachable from there rather than only from the implementations.

#include "winapi/core/marshaler.hpp"

#include <vitreo/common.hpp>
#include <vitreo/diagnostics.hpp>

#include <chrono>
#include <string>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

/// VITREO_REGISTRY_LOG tracing, split by what the call shape is worth showing:
/// an open reports the subkey it was given, a query also reports the value's
/// type and size, which is what a failing caller usually needs.
void log_registry_open(const char* api, u64 hkey, const std::wstring& subkey,
                       u32 err);
void log_registry_query(const char* api, u64 hkey,
                        const std::wstring& value_name, u32 err, u32 type,
                        u32 size);

/// Widen an ANSI string for the W-form implementation behind an A export.
[[nodiscard]] std::wstring ansi_to_wide(const std::string& s);

/// Read a null-terminated string out of guest memory, capped.
[[nodiscard]] std::wstring read_guest_wstring(GuestMemory& mem, GuestAddr ptr,
                                              usize max_chars = 4096);
[[nodiscard]] std::string read_guest_astring(GuestMemory& mem, GuestAddr ptr,
                                             usize max_chars = 4096);

/// Write a null-terminated UTF-16 string, truncating to `max_chars`.
void write_guest_wstring(GuestMemory& mem, GuestAddr ptr, const std::wstring& s,
                         u32 max_chars);

/// Write a PHKEY out at the guest's pointer width. HKEY is pointer-sized on
/// Windows even though Vitreo's handle values currently fit in 32 bits, so a
/// 64-bit guest must get eight bytes written or it reads garbage in the top half.
void write_guest_hkey(Marshaler& m, GuestAddr ptr, u64 hkey);

/// Times one registry thunk for the performance window. No-op when the window
/// is inactive.
class RegistryWindowTimer {
public:
    RegistryWindowTimer() noexcept
        : on_(diagnostics::perf_window_active())
        , t0_(on_ ? std::chrono::steady_clock::now()
                  : std::chrono::steady_clock::time_point{}) {}
    ~RegistryWindowTimer() {
        if (!on_) return;
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0_)
                .count());
        diagnostics::perf_add_registry(ns);
    }
    RegistryWindowTimer(const RegistryWindowTimer&) = delete;
    RegistryWindowTimer& operator=(const RegistryWindowTimer&) = delete;

private:
    bool on_;
    std::chrono::steady_clock::time_point t0_;
};

} // namespace vitreo::winapi

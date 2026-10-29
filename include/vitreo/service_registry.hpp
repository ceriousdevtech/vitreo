// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Service registry: compile-time typed service lookup (no RTTI)

#pragma once

#include <vitreo/common.hpp>
#include <cstddef>

namespace vitreo {

// =============================================================================
// ServiceTag — specialise for each service type to provide a unique ID
// =============================================================================

/// Primary template — deliberately left undefined.
/// Each service struct must provide:
///   static constexpr u32 kServiceId = <unique>;
/// The registry reads T::kServiceId directly, so no separate
/// ServiceTag specialization boilerplate is needed.

// =============================================================================
// ServiceRegistry
// =============================================================================

/// Maximum number of distinct service groups.  Increase if you add more than
/// this many service types.  The cost is one pointer per slot.
inline constexpr std::size_t kMaxServices = 32;

/// Lightweight, RTTI-free service locator.
///
/// Services are registered at startup and read (lock-free) during execution.
/// Thread-safety model: register all services on a single thread before any
/// concurrent calls to get<T>().
class ServiceRegistry {
public:
    ServiceRegistry() noexcept {
        for (std::size_t i = 0; i < kMaxServices; ++i)
            slots_[i] = nullptr;
    }

    /// Register a service pointer under its compile-time ID.
    /// T must expose  static constexpr u32 kServiceId.
    template<typename T>
    void register_service(T* ptr) noexcept {
        static_assert(T::kServiceId < kMaxServices,
                      "kServiceId exceeds kMaxServices — bump the limit");
        slots_[T::kServiceId] = static_cast<void*>(ptr);
    }

    /// Retrieve a previously registered service, or nullptr.
    template<typename T>
    [[nodiscard]] T* get() const noexcept {
        static_assert(T::kServiceId < kMaxServices,
                      "kServiceId exceeds kMaxServices — bump the limit");
        return static_cast<T*>(slots_[T::kServiceId]);
    }

private:
    void* slots_[kMaxServices];
};

} // namespace vitreo

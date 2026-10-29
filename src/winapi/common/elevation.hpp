#pragma once

/// @file elevation.hpp
/// @brief Immutable Windows-visible elevation state for one guest process.

#include <string>
#include <string_view>
#include <utility>

namespace vitreo::winapi {

enum class GuestTokenOrigin {
    User,
    AuthorizedAdministrator,
    LocalSystemService,
    RestrictedService,
};

/// All elevation-related Win32 APIs share this process-scoped value. It is
/// established only by Vitreo's trusted launch path after native macOS
/// authorization, then remains immutable for the lifetime of the guest.
class GuestElevationState {
public:
    explicit GuestElevationState(
        bool elevated = false,
        std::string credential = {},
        GuestTokenOrigin origin = GuestTokenOrigin::User)
        : m_elevated(elevated)
        , m_credential(std::move(credential))
        , m_origin(origin) {}

    [[nodiscard]] constexpr bool is_elevated() const noexcept {
        return m_elevated;
    }
    [[nodiscard]] std::string_view credential() const noexcept {
        return m_credential;
    }
    [[nodiscard]] constexpr GuestTokenOrigin origin() const noexcept {
        return m_origin;
    }
    [[nodiscard]] constexpr bool is_service_managed() const noexcept {
        return m_origin == GuestTokenOrigin::LocalSystemService ||
               m_origin == GuestTokenOrigin::RestrictedService;
    }

private:
    bool m_elevated;
    std::string m_credential;
    GuestTokenOrigin m_origin;
};

} // namespace vitreo::winapi

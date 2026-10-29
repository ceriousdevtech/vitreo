// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace vitreo::platform {

struct ElevationRequest {
    std::string application_name;
    std::string executable_path;
};

enum class ElevationAuthorizationResult {
    Authorized,
    Cancelled,
    Denied,
    Failed,
};

/// Host-owned consent boundary for creating an elevated guest process.
/// Guest elevation does not require the macOS administrator password.
class ElevationAuthorizer {
public:
    virtual ~ElevationAuthorizer() = default;

    [[nodiscard]] virtual ElevationAuthorizationResult authorize(
        const ElevationRequest& request,
        std::string& credential) = 0;

    /// Verify a credential issued by a previous successful authorization.
    /// No user interaction is allowed during validation.
    [[nodiscard]] virtual bool validate_credential(
        std::string_view credential) = 0;
};

/// Create the native macOS authorization implementation.
[[nodiscard]] std::unique_ptr<ElevationAuthorizer>
create_elevation_authorizer();

} // namespace vitreo::platform

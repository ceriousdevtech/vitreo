// SPDX-License-Identifier: LGPL-2.1-only

#include "platform/macos/elevation_authorization.hpp"

#include <vitreo/common.hpp>

#include <CommonCrypto/CommonHMAC.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <string_view>

namespace vitreo::platform {

namespace {

/// Guest elevation only flips Windows-visible admin/token state inside the
/// capsule. It does not grant host root or unrestricted macOS filesystem
/// access, so it is granted without a host prompt or password.
/// Credentials are HMAC-verified capability cookies that child workers can
/// validate without shared process state.
constexpr std::string_view kCredentialMagic = "VTEL1";
constexpr usize kNonceSize = 16;
constexpr usize kMacSize = CC_SHA256_DIGEST_LENGTH;
constexpr usize kCredentialSize =
    kCredentialMagic.size() + kNonceSize + kMacSize;

// Not a secret against a local attacker who reads the binary; it only stops
// trivial forged argv values from elevating a worker.
constexpr std::string_view kCredentialKey =
    "vitreo-guest-elevation-v1";

void fill_mac(std::span<const u8, kNonceSize> nonce,
              std::span<u8, kMacSize> mac_out) {
    CCHmac(
        kCCHmacAlgSHA256,
        kCredentialKey.data(), kCredentialKey.size(),
        nonce.data(), nonce.size(),
        mac_out.data());
}

[[nodiscard]] std::string mint_credential() {
    std::array<u8, kNonceSize> nonce{};
    std::random_device device;
    for (u8& byte : nonce) {
        byte = static_cast<u8>(device());
    }
    std::array<u8, kMacSize> mac{};
    fill_mac(nonce, mac);

    std::string credential;
    credential.reserve(kCredentialSize);
    credential.append(kCredentialMagic);
    credential.append(
        reinterpret_cast<const char*>(nonce.data()), nonce.size());
    credential.append(
        reinterpret_cast<const char*>(mac.data()), mac.size());
    return credential;
}

[[nodiscard]] bool credential_mac_valid(std::string_view credential) {
    if (credential.size() != kCredentialSize ||
        !credential.starts_with(kCredentialMagic)) {
        return false;
    }
    std::array<u8, kNonceSize> nonce{};
    std::memcpy(
        nonce.data(),
        credential.data() + kCredentialMagic.size(),
        kNonceSize);
    std::array<u8, kMacSize> expected{};
    fill_mac(nonce, expected);
    const auto* actual = reinterpret_cast<const u8*>(
        credential.data() + kCredentialMagic.size() + kNonceSize);
    return std::memcmp(expected.data(), actual, kMacSize) == 0;
}

class MacOSElevationAuthorizer final : public ElevationAuthorizer {
public:
    ElevationAuthorizationResult authorize(
        const ElevationRequest& request,
        std::string& credential) override {
        // Capsule elevation is granted without a prompt (product decision,
        // 2026-09-26). It only flips Windows-visible token state inside the
        // capsule and grants nothing on the Mac, and a modal consent alert
        // parked the requesting guest thread until someone answered it:
        // A launcher runs each title's install script through an elevated
        // service on every play, so each launch waited on the alert for
        // anywhere from 44 seconds to 8 minutes.
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            std::fprintf(stderr,
                         "[elevation] capsule elevation auto-approved "
                         "(first request: '%s')\n",
                         request.application_name.c_str());
        }
        credential = mint_credential();
        return ElevationAuthorizationResult::Authorized;
    }

    bool validate_credential(
        std::string_view credential) override {
        const bool ok = credential_mac_valid(credential);
        VITREO_TRACE(
            "[elevation] credential validation ok=%d\n", ok ? 1 : 0);
        return ok;
    }
};

} // namespace

std::unique_ptr<ElevationAuthorizer> create_elevation_authorizer() {
    return std::make_unique<MacOSElevationAuthorizer>();
}

} // namespace vitreo::platform

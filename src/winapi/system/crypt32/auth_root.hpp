// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file auth_root.hpp
/// @brief Windows' automatic root certificate update: Microsoft's trusted
///        root list (authroot.stl) and the per-root properties it carries.
///
/// authroot.stl is a PKCS#7 SignedData whose content (szOID_CTL,
/// 1.3.6.1.4.1.311.10.1) is a Certificate Trust List (Learn, CTL_INFO /
/// CTL_ENTRY): subjectUsage szOID_ROOT_LIST_SIGNER, then one TrustedSubject
/// per root -- the root's SHA-1 thumbprint and attributes named
/// szOID_CERT_PROP_ID_PREFIX + property id, each value an OCTET STRING
/// holding that property (wincrypt.h): 9 CERT_ENHKEY_USAGE (allowed EKUs),
/// 20 CERT_KEY_IDENTIFIER, 29 CERT_SUBJECT_NAME_MD5_HASH, 84 root-program
/// name constraints, 104 CERT_DISALLOWED_FILETIME, 122
/// CERT_DISALLOWED_ENHKEY_USAGE.
///
/// This header is the format; Crypt32Manager (auth_root.cpp) fetches and
/// verifies the list and serves roots from it.

#include <vitreo/common.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vitreo::winapi::crypt32 {

struct AuthRootEntry {
    std::array<u8, 20> sha1{};
    std::vector<u8> key_id;                       ///< property 20
    std::vector<u8> subject_name_md5;             ///< property 29
    std::optional<std::vector<u8>> eku;           ///< property 9, encoded EKU
    std::optional<u64> disallowed_filetime;       ///< property 104
    std::optional<std::vector<u8>> disallowed_eku;  ///< property 122, encoded EKU
    bool name_constraints{false};                 ///< property 84 present
    std::vector<u8> der;                          ///< the TrustedSubject TLV
};

/// The TrustedSubjects of a CTL (the SignedData content octets). nullopt
/// when it is not a CTL for szOID_ROOT_LIST_SIGNER.
[[nodiscard]] std::optional<std::vector<AuthRootEntry>> parse_auth_root_ctl(std::span<const u8> ctl);

/// One TrustedSubject TLV (as kept beside a root the list installed).
[[nodiscard]] std::optional<AuthRootEntry> parse_auth_root_entry(std::span<const u8> der);

}  // namespace vitreo::winapi::crypt32

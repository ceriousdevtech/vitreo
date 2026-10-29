// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file x509_parse.hpp
/// @brief Minimal DER helpers for CERT_INFO extension materialization.
///
/// Used by Crypt32 to populate CERT_INFO.rgExtension so curl's Schannel
/// hostname check (CertFindExtension + CryptDecodeObjectEx) can read the
/// subjectAltName extension from a real peer certificate.

#include <vitreo/common.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::winapi::x509 {

struct Extension {
    std::string oid;          // dotted decimal, e.g. "2.5.29.17"
    bool critical{false};
    std::vector<u8> value;    // raw OCTET STRING contents
};

struct NameAttribute {
    std::string oid;
    std::string value;
};

struct AlgorithmIdentifier {
    std::string oid;              // dotted decimal, e.g. "1.2.840.113549.1.1.1"
    std::vector<u8> parameters;   // remaining DER after the OID, often NULL (05 00)
};

struct PublicKeyInfo {
    AlgorithmIdentifier algorithm;
    std::vector<u8> key;          // BIT STRING payload (no unused-bits prefix)
    u32 unused_bits{0};
};

struct BitString {
    bool present{false};
    std::vector<u8> bytes;        // contents after the unused-bits octet
    u32 unused_bits{0};
};

struct ParsedCertificate {
    u32 version{0};               // CERT_INFO.dwVersion: 0 = v1 ... 2 = v3
    std::vector<u8> serial;       // INTEGER contents, big-endian as encoded
    AlgorithmIdentifier signature_algorithm;
    std::vector<u8> issuer_der;
    std::vector<u8> subject_der;
    PublicKeyInfo spki;
    std::vector<u8> spki_der;     // whole SubjectPublicKeyInfo element
    BitString issuer_unique_id;
    BitString subject_unique_id;
    std::string issuer_simple_name;
    std::string subject_simple_name;
    std::vector<NameAttribute> issuer_attributes;
    std::vector<NameAttribute> subject_attributes;
    std::vector<Extension> extensions;
    // Windows FILETIME (100ns since 1601-01-01 UTC). Zero if Validity was
    // absent or unparsable.
    u64 not_before_filetime{0};
    u64 not_after_filetime{0};
    // The exact bytes a signature is computed over (the tbsCertificate
    // element *including* its SEQUENCE header) and the outer signatureValue
    // BIT STRING payload, so a caller can verify the signature without
    // re-walking the DER. Empty when the certificate could not supply them.
    std::vector<u8> tbs_der;
    std::vector<u8> signature_value;
};

/// CRYPT_E_* codes the Windows ASN.1 decoder reports (winerror.h).
inline constexpr u32 kCryptEAsn1Eod = 0x80093102u;
inline constexpr u32 kCryptEAsn1Corrupt = 0x80093103u;
inline constexpr u32 kCryptEAsn1BadTag = 0x8009310Bu;
inline constexpr u32 kCryptEBadEncode = 0x80092002u;

/// The error CertCreateCertificateContext reports for this encoding, or 0 when
/// it decodes. Modelled on tools/oracle_probe/crypt_prop_probe.expected
/// ("create" rows): BER lengths (long form, indefinite) are accepted, extra
/// elements after the signature and bytes after the certificate are ignored;
/// a missing wrapper element or a wrong tag is CRYPT_E_ASN1_BADTAG, running
/// out of data inside a field is CRYPT_E_ASN1_EOD, a malformed Validity or an
/// unexpected trailing TBS element is CRYPT_E_ASN1_CORRUPT, and an impossible
/// time value is CRYPT_E_BAD_ENCODE.
[[nodiscard]] u32 certificate_decode_error(std::span<const u8> der);

/// Dotted-decimal text of an OBJECT IDENTIFIER's contents octets.
[[nodiscard]] std::string oid_to_string(std::span<const u8> oid);

/// Contents octets for a dotted-decimal OID; empty when the text is not a
/// valid OID (fewer than two arcs, first arc > 2, non-digits).
[[nodiscard]] std::vector<u8> encode_oid(std::string_view dotted);

/// X509_ENHANCED_KEY_USAGE (SEQUENCE OF OBJECT IDENTIFIER). Returns 0 and the
/// OIDs in order, or the CRYPT_E_ASN1_* error for malformed input.
[[nodiscard]] u32 decode_enhanced_key_usage(std::span<const u8> encoded,
                                            std::vector<std::string>& oids);
[[nodiscard]] std::vector<u8> encode_enhanced_key_usage(
    const std::vector<std::string>& oids);

/// Bit length of a public key: the RSA modulus, or the named EC curve's
/// size. 0 when the algorithm or curve is not one of those.
[[nodiscard]] u32 public_key_bit_length(const PublicKeyInfo& key);

/// One GeneralName: its context tag number (0 otherName, 1 rfc822Name,
/// 2 dNSName, 4 directoryName, 6 uniformResourceIdentifier, 7 iPAddress, ...)
/// and contents. otherName carries its type-id and the [0] value TLV.
struct GeneralName {
    u8 choice{0};
    std::vector<u8> value;
    std::string other_oid;
};
/// GeneralNames (subjectAltName / issuerAltName extension value).
[[nodiscard]] std::vector<GeneralName> decode_general_names(std::span<const u8> ext_value);

/// certificatePolicies (2.5.29.32) policy identifiers, in order.
[[nodiscard]] std::vector<std::string> decode_certificate_policies(
    std::span<const u8> ext_value);

/// authorityKeyIdentifier (2.5.29.35): which of keyIdentifier [0] and
/// authorityCertIssuer [1] + authorityCertSerialNumber [2] are present.
struct AuthorityKeyId {
    bool present{false};
    std::vector<u8> key_id;
    bool has_issuer_serial{false};
};
[[nodiscard]] AuthorityKeyId decode_authority_key_id(std::span<const u8> ext_value);

/// True when the certificate names a place to fetch revocation status: a
/// cRLDistributionPoints extension or an authorityInfoAccess OCSP method.
[[nodiscard]] bool has_revocation_urls(const ParsedCertificate& cert);

/// basicConstraints: cA flag and optional pathLenConstraint.
struct BasicConstraints {
    bool present{false};
    bool ca{false};
    std::optional<u32> path_length;
};
[[nodiscard]] BasicConstraints basic_constraints(const ParsedCertificate& cert);

/// Parse a DER-encoded X.509 certificate enough for Serial/Issuer/Subject and
/// the Extensions SEQUENCE. Returns nullopt exactly when
/// certificate_decode_error() reports an error.
[[nodiscard]] std::optional<ParsedCertificate> parse_certificate(
    std::span<const u8> der);

/// Decode a subjectAltName extension value into DNS names (IA5String only).
[[nodiscard]] std::vector<std::string> decode_subject_alt_dns_names(
    std::span<const u8> ext_value);

/// Format a CERT_NAME_BLOB (DER Name / RDNSequence) for CertNameToStr.
/// `str_type` uses the Microsoft Learn CERT_*_NAME_STR values (low bits);
/// unsupported option flags are ignored.
[[nodiscard]] std::string format_distinguished_name(
    std::span<const u8> name_der, u32 str_type);

/// Subject Key Identifier extension (2.5.29.14) octets when present.
/// Empty when the extension is absent (callers may fall back to SHA-1 of
/// `spki.key` for CERT_KEY_IDENTIFIER_PROP_ID).
[[nodiscard]] std::vector<u8> certificate_key_identifier(
    const ParsedCertificate& cert);

} // namespace vitreo::winapi::x509

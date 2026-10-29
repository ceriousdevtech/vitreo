#pragma once
/// @file crypt32_internal.hpp
/// @brief Shared surface between crypt32.cpp and its export table.
///
/// Private to crypt32. The handlers sit in a nested namespace rather than
/// directly in vitreo::winapi because certificate-store and encoding names
/// recur across the crypto DLLs — bcrypt and ncrypt keep their own statics with
/// overlapping spellings, and one shared namespace for all of them is a trap.

#include "winapi/core/marshaler.hpp"
#include "winapi/system/crypt32/authenticode.hpp"
#include "winapi/system/crypt32/crypt32.hpp"
#include "winapi/system/crypt32/x509_parse.hpp"

namespace vitreo::winapi::crypt32 {

/// HRESULT-style last-error values the crypt32 contracts use (winerror.h).
namespace err {
inline constexpr u32 kEInvalidArg = 0x80070057u;
inline constexpr u32 kENotImpl = 0x80004001u;
inline constexpr u32 kCryptENotFound = 0x80092004u;
inline constexpr u32 kCryptEExists = 0x80092005u;
inline constexpr u32 kCryptEPendingClose = 0x8009200Fu;
inline constexpr u32 kCryptENoKeyProperty = 0x8009200Bu;
inline constexpr u32 kNteBadAlgid = 0x80090008u;
inline constexpr u32 kNteBadSignature = 0x80090006u;
} // namespace err

/// One `[vitreo] UNIMPLEMENTED <dll>!<api>: <case>` line per distinct case.
void report_unimplemented(const char* api, const char* detail);

void impl_CertCompareCertificateName(Marshaler& m);

/// The implicit value of a computed certificate property (SHA-1/MD5/SHA-256
/// hashes, key identifier, signature hash, ...), or nullopt when the id is
/// not computed.
[[nodiscard]] std::optional<std::vector<u8>> compute_certificate_property(
    u32 id, const std::vector<u8>& der, const x509::ParsedCertificate& parsed);

class GuestBlock;
/// Append a CMSG_SIGNER_INFO (and everything it points at) for `signer` to
/// `block`; returns its offset. Shared by CryptMsgGetParam and WinVerifyTrust.
u32 put_cmsg_signer_info(GuestBlock& block, const authenticode::SignerInfo& signer);

/// Raise STATUS_ACCESS_VIOLATION in the guest at `address` (read or write),
/// where Windows faults on a bad pointer argument.
void raise_access_violation(Marshaler& m, u64 address, bool write);

void impl_CryptProtectData(Marshaler& m, Crypt32Manager& mgr);
void impl_CryptUnprotectData(Marshaler& m, Crypt32Manager& mgr);
void impl_CertOpenStore(Marshaler& m, Crypt32Manager& mgr);
void impl_CertOpenSystemStoreW(Marshaler& m, Crypt32Manager& mgr);
void impl_CertEnumCertificatesInStore(Marshaler& m, Crypt32Manager& mgr);
void impl_CertCloseStore(Marshaler& m, Crypt32Manager& mgr);
void impl_CertFindCertificateInStore(Marshaler& m, Crypt32Manager& mgr);
void impl_CertFreeCertificateContext(Marshaler& m, Crypt32Manager& mgr);
void impl_CertDuplicateCertificateContext(Marshaler& m, Crypt32Manager& mgr);
void impl_CertGetCertificateChain(Marshaler& m, Crypt32Manager& mgr);
void impl_CertFreeCertificateChain(Marshaler& m, Crypt32Manager& mgr);
void impl_CertVerifyCertificateChainPolicy(Marshaler& m, Crypt32Manager& mgr);
void impl_CertCreateCertificateChainEngine(Marshaler& m, Crypt32Manager& mgr);
void impl_CertFreeCertificateChainEngine(Marshaler& m, Crypt32Manager& mgr);
void impl_CryptInstallOIDFunctionAddress(Marshaler& m, Crypt32Manager& mgr);
void impl_CryptStringToBinaryA(Marshaler& m, Crypt32Manager& mgr);
void impl_CryptStringToBinaryW(Marshaler& m, Crypt32Manager& mgr);
void impl_CryptBinaryToStringA(Marshaler& m, Crypt32Manager& mgr);
void impl_CryptDecodeObjectEx(Marshaler& m, Crypt32Manager& mgr);
void impl_CertFindExtension(Marshaler& m, Crypt32Manager& mgr);
void impl_CryptEncodeObjectEx(Marshaler& m, Crypt32Manager& mgr);
void impl_PFXImportCertStore(Marshaler& m, Crypt32Manager& mgr);
void impl_CertAddCertificateContextToStore(Marshaler& m, Crypt32Manager& mgr);
void impl_CertAddStoreToCollection(Marshaler& m, Crypt32Manager& mgr);
void impl_CertAddEncodedCertificateToStore(
    Marshaler& m, Crypt32Manager& mgr);
void impl_CertDeleteCertificateFromStore(
    Marshaler& m, Crypt32Manager& mgr);
void impl_CertCreateCertificateContext(Marshaler& m, Crypt32Manager& mgr);
void impl_CertComparePublicKeyInfo(Marshaler& m);
void impl_CertGetIntendedKeyUsage(Marshaler& m);

/// A SignerInfo's signed attributes against @p cert and the content they
/// digest: 0, or STATUS_INVALID_SIGNATURE / CRYPT_E_HASH_VALUE
/// (wintrust_dll_authenticode_module_verification.cpp).
[[nodiscard]] u32 verify_signer_attributes(const authenticode::SignerInfo& signer,
                                           const x509::ParsedCertificate& cert,
                                           std::span<const u8> signed_content);
[[nodiscard]] const authenticode::Certificate* find_signer_certificate(
    const std::vector<authenticode::Certificate>& certificates, const authenticode::SignerInfo& signer);

} // namespace vitreo::winapi::crypt32

namespace vitreo::winapi {

/// True when `issuer`'s public key verifies `subject`'s signature. Shared by
/// CryptVerifyCertificateSignatureEx and by issuer lookup, which must report
/// CERT_STORE_SIGNATURE_FLAG honestly rather than assume the match is good.
[[nodiscard]] bool certificate_signed_by(
    const x509::ParsedCertificate& subject,
    const x509::ParsedCertificate& issuer);

/// PKCS#1 v1.5 (RSA) or X9.62 (ECDSA) signature over `message` hashed with
/// `hash` ("SHA1", "SHA256", "SHA384", "SHA512") by the host Security
/// framework.
[[nodiscard]] bool verify_message_signature(
    const x509::PublicKeyInfo& key, std::string_view hash,
    std::span<const u8> message, std::span<const u8> signature);

} // namespace vitreo::winapi

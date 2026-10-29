// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <vitreo/common.hpp>
#include "winapi/com/ole32/storage.hpp"

#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vitreo::winapi::authenticode {

enum class ParseError {
    InvalidPe,
    NoSignature,
    InvalidWinCertificate,
    InvalidPkcs7,
    MissingSigner,
};

struct Certificate {
    std::vector<u8> der;
    std::vector<u8> issuer;
    std::vector<u8> serial;
    std::string common_name;
};

struct Signature {
    std::vector<Certificate> certificates;
    usize signer_index{0};
};

[[nodiscard]] Result<Signature, ParseError> parse_pe(std::span<const u8> image);

/// One CMS/PKCS#7 SignerInfo, with the raw pieces a verifier needs.
struct SignerInfo {
    u32 version{0};
    std::vector<u8> issuer;              // issuerAndSerialNumber.issuer (Name TLV)
    std::vector<u8> serial;              // INTEGER contents, big-endian
    std::string digest_oid;
    std::vector<u8> digest_parameters;   // encoded parameters (after the OID)
    bool has_signed_attributes{false};
    std::vector<u8> signed_attributes;   // [0] contents; hashed re-tagged as SET
    std::string signature_oid;
    std::vector<u8> signature_parameters;
    std::vector<u8> signature;           // encryptedDigest
    struct Attribute {
        std::string oid;
        std::vector<std::vector<u8>> values;  // each value's full TLV
    };
    std::vector<Attribute> signed_attribute_list;
    std::vector<Attribute> unsigned_attribute_list;
};

/// A parsed ContentInfo(SignedData).
struct SignedData {
    std::string content_type;
    std::vector<u8> content;             // eContent element TLV (inside [0])
    std::vector<u8> content_value;       // its contents octets (what is digested)
    std::vector<Certificate> certificates;
    std::vector<SignerInfo> signers;
};

[[nodiscard]] Result<SignedData, ParseError> parse_signed_data(std::span<const u8> content_info);

/// A bare SignerInfo TLV (the PKCS#9 countersignature attribute value).
[[nodiscard]] Result<SignerInfo, ParseError> parse_signer_info(std::span<const u8> der);

/// The PKCS#7 blob of a PE's first WIN_CERT_TYPE_PKCS_SIGNED_DATA
/// certificate, and the certificate table's file offset and size.
struct PeSignatureLocation {
    std::vector<u8> pkcs7;
    u32 table_offset{0};
    u32 table_size{0};
};
[[nodiscard]] Result<PeSignatureLocation, ParseError> locate_pe_signature(std::span<const u8> image);

/// True when the image is a PE (MZ + PE header) at all.
[[nodiscard]] bool is_pe_image(std::span<const u8> image);

/// Authenticode image digest input: the byte ranges of the file that are
/// hashed, in order (headers without CheckSum and the security directory
/// entry, sections by PointerToRawData, trailing data before the
/// certificate table).
[[nodiscard]] std::vector<std::pair<usize, usize>> pe_digest_ranges(std::span<const u8> image);
/// True when the file is a cabinet (CFHEADER signature "MSCF").
[[nodiscard]] bool is_cab_image(std::span<const u8> image);

/// A signed cabinet's PKCS#7 blob and where it sits. The CFHEADER carries
/// cfhdrRESERVE_PRESENT with an abReserve area of at least 20 bytes laid out
/// as { u16 0, u16 0x10, u32 offset, u32 size, ... }; the blob starts at
/// cbCabinet. Measured on winoracle (WinVerifyTrust over single-byte edits
/// of a signed DirectX redistributable cabinet): a different leading word,
/// an offset other than cbCabinet, or a non-zero cbCFData is
/// TRUST_E_NOSIGNATURE, so all three map to ParseError::NoSignature.
struct CabSignatureLocation {
    std::vector<u8> pkcs7;
    u32 offset{0};
    u32 size{0};
};
[[nodiscard]] Result<CabSignatureLocation, ParseError> locate_cab_signature(std::span<const u8> image);

/// Cabinet Authenticode digest input, measured the same way: the CFHEADER
/// without reserved1 (bytes 4-7), cbCFHeader (36-37) and the abReserve area,
/// then everything to the signature, then anything after it. reserved2,
/// reserved3, iCabinet, cbCFFolder and cbCFData are all covered. Empty when
/// the file carries no locatable signature.
[[nodiscard]] std::vector<std::pair<usize, usize>> cab_digest_ranges(std::span<const u8> image);

/// True when the file starts with the OLE compound file header signature
/// ([MS-CFB] 2.2: D0 CF 11 E0 A1 B1 1A E1). Windows Installer packages,
/// patches and transforms are compound files; Authenticode handles them
/// through the structured-storage SIP ({000C10F1-0000-0000-C000-000000000046}).
[[nodiscard]] bool is_compound_file_image(std::span<const u8> image);

/// A structured-storage subject's Authenticode signature. The PKCS#7
/// ContentInfo is the whole root-level stream "\x05DigitalSignature";
/// "\x05MsiDigitalSignatureEx", when present, holds the digest of the
/// metadata pre-hash (see storage_digest_input). `root` owns the parsed tree
/// the digest input spans point into.
struct StorageSignature {
    std::shared_ptr<const StorageNode> root;
    std::vector<u8> pkcs7;
    bool has_extended{false};
    std::vector<u8> extended;
};
/// ParseError::InvalidPe when the image is not a readable compound file,
/// ParseError::NoSignature when it has no "\x05DigitalSignature" stream.
[[nodiscard]] Result<StorageSignature, ParseError> locate_storage_signature(std::span<const u8> image);

/// Structured-storage Authenticode digest input. Established against the
/// messageDigest Windows wrote into real signed packages (17 files: SHA-1
/// vc_red.msi / msp_kb2565063.msp without the extended stream, SHA-256 VC++
/// 14.x runtime MSIs and LibreOffice 26.8 with it, including nested
/// storages), each reproduced byte for byte:
///  - content: below every storage, its children in ascending order of their
///    on-disk UTF-16LE name bytes (memcmp, a proper prefix first), each
///    stream's bytes and each storage's content recursively, then the
///    storage's own CLSID; the root-level signature streams are skipped;
///  - metadata (only when "\x05MsiDigitalSignatureEx" is present): the
///    root's CLSID and state bits, then in the same order and recursion,
///    per stream: name bytes, u32 size, state bits, creation and modified
///    FILETIMEs; per storage: name bytes, CLSID, state bits, both FILETIMEs,
///    then its children;
///  - the signed digest is H(content) without the extended stream, and
///    H(H(metadata) || content) with it.
struct StorageDigestInput {
    bool extended{false};
    std::vector<u8> metadata;
    std::vector<std::span<const u8>> content;
};
[[nodiscard]] StorageDigestInput storage_digest_input(const StorageSignature& subject);

[[nodiscard]] const char* parse_error_string(ParseError error) noexcept;

} // namespace vitreo::winapi::authenticode

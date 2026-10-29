#pragma once
/// @file msi_product.hpp
/// @brief Windows Installer product operations area: the encodings and file
/// facts other areas share (compressed GUIDs, Darwin descriptors, MSI file
/// versions and hashes, Darwin shortcuts) and the multi-package transaction
/// state the engine consults.
///
/// Contract sources: Msi.h (SDK 10.0.26100), the Microsoft Learn pages of
/// each function named below, [MS-SHLLINK] (the .lnk layout), the OpenType
/// `name` table specification, and tools/oracle_probe/msi_productops_probe.c
/// (msi_productops32/64.expected) for what Learn leaves open. Contract notes
/// and gaps: .docs/contracts/msi/productops.md.

#include <vitreo/common.hpp>

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo::capsule { class Vfs; }
namespace vitreo::winapi { class MsiManager; class Marshaler; }

namespace vitreo::winapi::msi {

// ---- GUIDs --------------------------------------------------------------------------

/// True for a registry-format GUID "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}",
/// hex digits in either case (probe: a lowercase product code is accepted
/// and reaches ERROR_UNKNOWN_PRODUCT; a malformed one is ERROR_INVALID_PARAMETER).
[[nodiscard]] bool is_guid(std::string_view text);

/// The 20-character compressed form of a GUID used in Darwin descriptors:
/// the GUID's 16 bytes as four little-endian DWORDs, each written as five
/// base-85 digits, least significant first (round-tripped through the real
/// MsiDecomposeDescriptor for every digit in every position). Empty when
/// @p guid is not a GUID.
[[nodiscard]] std::string compress_guid(std::string_view guid);
/// Inverse of compress_guid over exactly 20 characters. Five digits wrap
/// modulo 2^32 (probe: "~~~~~" decodes to 0x08780EC4). nullopt when a
/// character is outside the alphabet.
[[nodiscard]] std::optional<std::string> decompress_guid(std::u16string_view compressed);

// ---- Darwin descriptors -------------------------------------------------------------

/// MAX_FEATURE_CHARS (Msi.h).
inline constexpr usize kMaxFeatureChars = 38;

/// A decoded descriptor: compressed product (20) + feature name +
/// ('>' + compressed component (20) | '<'). The text after it is the
/// command-line remainder; `consumed` counts the descriptor's characters.
struct Descriptor {
    std::string product;    ///< "{...}"
    std::string feature;    ///< UTF-8
    std::string component;  ///< "{...}", or "" for the '<' form
    usize consumed{0};
};

/// What MsiDecomposeDescriptor wrote before it stopped (probe: the product
/// is written once decoded; the feature's characters are written as they
/// are scanned, terminated only when its delimiter was found; the component
/// only when it decoded; the offset only on success).
struct DescriptorProgress {
    bool product{false};
    usize feature_chars{0};     ///< characters of `feature` written
    bool feature_terminated{false};
    bool component{false};
};

/// Decode @p text. Returns ERROR_SUCCESS or ERROR_INVALID_PARAMETER (every
/// malformed form, probe decompose lines). @p out holds what was decoded so
/// far either way; @p progress what the caller should have written.
u32 decompose_descriptor(std::u16string_view text, Descriptor& out, DescriptorProgress* progress = nullptr);

/// Encode a descriptor (the inverse of decompose_descriptor); @p component
/// empty gives the '<' form. Empty when a GUID is malformed or the feature
/// is empty or longer than MAX_FEATURE_CHARS.
[[nodiscard]] std::string compose_descriptor(std::string_view product, std::string_view feature,
                                             std::string_view component);

// ---- File facts ---------------------------------------------------------------------

/// MsiGetFileVersion's two strings (Learn "MsiGetFileVersion"; probe version
/// lines): a PE or NE image's VS_FIXEDFILEINFO file version "a.b.c.d" and
/// every VarFileInfo\Translation language in decimal, ascending, comma
/// separated; a TrueType/OpenType font's version from its `name` table
/// version string as "major.minor.0.0" with an empty language.
struct FileVersion {
    std::string version;
    std::string language;
};
/// ERROR_SUCCESS, ERROR_FILE_INVALID (no version resource / not a font),
/// or ERROR_INVALID_DATA (a version resource without a valid fixed block).
u32 file_version_of_image(std::span<const u8> image, FileVersion& out);

/// MsiGetFileHash (MsiFileHash table): the MD5 of the whole file as four
/// little-endian DWORDs; an empty file hashes to four zeros (probe hash lines,
/// checked against MD5 of the same files).
[[nodiscard]] std::array<u32, 4> msi_file_hash(std::span<const u8> bytes);

/// The Darwin descriptor stored in a .lnk's EXP_DARWIN_LINK extra-data block
/// (signature 0xA0000006, [MS-SHLLINK] 2.5.3): the Unicode field, else the
/// ANSI one. nullopt when the file is not a shell link or has no such block.
[[nodiscard]] std::optional<std::u16string> darwin_descriptor_of_shortcut(std::span<const u8> lnk);

// ---- Transactions (MsiBeginTransaction) -------------------------------------------------

/// The machine-wide multi-package transaction, when one is open: installs
/// started inside it keep their rollback until MsiEndTransaction.
struct TransactionInfo {
    bool active{false};
    std::string name;
    u32 attributes{0};
    u32 handle{0};
};
[[nodiscard]] TransactionInfo current_transaction(MsiManager& mgr);

// ---- Platform services product operations need from other DLLs ------------------------

/// Kernel event objects (MsiBeginTransaction's change-of-owner event) and
/// crypt32 certificate contexts (MsiGetFileSignatureInformation), wired by
/// whoever registers msi.dll. Unset members fail at the call loudly.
struct ProductOpsPlatform {
    /// A Win32 event HANDLE (manual reset, initial state), or 0.
    std::function<u64(bool manual_reset, bool initial_state)> create_event;
    std::function<void(u64 handle)> set_event;
    /// Verify @p image's Authenticode signature as WinVerifyTrust does and
    /// return the HRESULT; on S_OK fill @p signer_der and @p signature.
    std::function<u32(std::span<const u8> image, bool invalid_hash_is_fatal,
                      std::vector<u8>& signer_der, std::vector<u8>& signature)> verify_signature;
    /// A guest CERT_CONTEXT for @p der the caller frees with
    /// CertFreeCertificateContext, or 0.
    std::function<u64(Marshaler& m, std::span<const u8> der)> create_cert_context;
};
void set_product_ops_platform(MsiManager& mgr, ProductOpsPlatform platform);

}  // namespace vitreo::winapi::msi

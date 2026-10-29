// SPDX-License-Identifier: LGPL-2.1-only

/// @file auth_root.cpp
/// @brief Windows' automatic root certificate update.
///
/// When a chain ends at a root no store trusts (or at a certificate whose
/// issuer no store holds), Windows looks the root up in Microsoft's trusted
/// root list, downloads it and adds it to the machine ROOT store. The list,
/// authroot.stl, comes from Windows Update in authrootstl.cab; each listed
/// root from the same place by its SHA-1 thumbprint.
///
/// Trust: Windows verifies the list against a Microsoft root it already
/// holds. A capsule's ROOT store is seeded from the host's anchors, which
/// lack Microsoft's 2010 root, so the list's signing root is fetched over
/// HTTPS from the "CA Issuers" address its own chain names -- accepted only
/// on www.microsoft.com, with macOS validating that TLS connection -- and
/// must be self-signed and sign the chain (user decision, 2026-10-27). The
/// list must be a CTL for szOID_ROOT_LIST_SIGNER signed by a certificate
/// whose chain carries that EKU, and a root is installed only if its
/// thumbprint matches a list entry. No certificate or list ships with
/// Vitreo.

#include "winapi/system/crypt32/auth_root.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/compression/cabinet.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/networking/winhttp/http_bridge.hpp"
#include "winapi/system/crypt32/authenticode.hpp"
#include "winapi/system/crypt32/crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "winapi/system/crypt32/x509_parse.hpp"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

namespace vitreo::winapi {

namespace crypt32 {

namespace {

constexpr const char* kAuthRootCab =
    "http://ctldl.windowsupdate.com/msdownload/update/v3/static/trustedr/en/authrootstl.cab";
constexpr const char* kAuthRootCertBase =
    "http://ctldl.windowsupdate.com/msdownload/update/v3/static/trustedr/en/";
constexpr const char* kCtlOid = "1.3.6.1.4.1.311.10.1";            // szOID_CTL
constexpr const char* kRootListSigner = "1.3.6.1.4.1.311.10.3.9";  // szOID_ROOT_LIST_SIGNER
constexpr const char* kPropPrefix = "1.3.6.1.4.1.311.10.11.";      // szOID_CERT_PROP_ID_PREFIX
constexpr const char* kAuthRootDirectory = "C:\\ProgramData\\Vitreo\\CertificateStores\\AuthRoot";

/// Minimal DER walker over one constructed value's contents.
struct Der {
    std::span<const u8> data;
    usize pos = 0;
    bool next(u8& tag, std::span<const u8>& value, std::span<const u8>* whole = nullptr) {
        const usize start = pos;
        if (pos + 2 > data.size()) return false;
        tag = data[pos++];
        usize len = data[pos++];
        if ((len & 0x80) != 0) {
            const usize n = len & 0x7F;
            if (n == 0 || n > 4 || pos + n > data.size()) return false;
            len = 0;
            for (usize i = 0; i < n; ++i) len = (len << 8) | data[pos++];
        }
        if (len > data.size() - pos) return false;
        value = data.subspan(pos, len);
        pos += len;
        if (whole != nullptr) *whole = data.subspan(start, pos - start);
        return true;
    }
    [[nodiscard]] bool more() const { return pos < data.size(); }
};

[[nodiscard]] std::string hex_upper(std::span<const u8> bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string out;
    for (u8 b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xF]);
    }
    return out;
}

[[nodiscard]] std::array<u8, 20> sha1_of(std::span<const u8> data) {
    std::array<u8, 20> out{};
    CC_SHA1(data.data(), static_cast<CC_LONG>(data.size()), out.data());
    return out;
}

[[nodiscard]] std::vector<u8> md5_of(std::span<const u8> data) {
    std::vector<u8> out(CC_MD5_DIGEST_LENGTH);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5(data.data(), static_cast<CC_LONG>(data.size()), out.data());
#pragma clang diagnostic pop
    return out;
}

[[nodiscard]] u64 now_filetime() {
    const auto unix = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
    return (static_cast<u64>(unix) + 11644473600ull) * 10000000ull;
}

[[nodiscard]] std::vector<std::string> eku_of(const x509::ParsedCertificate& cert, bool& present) {
    present = false;
    std::vector<std::string> oids;
    for (const auto& extension : cert.extensions) {
        if (extension.oid != "2.5.29.37") continue;
        present = true;
        (void)x509::decode_enhanced_key_usage(extension.value, oids);
    }
    return oids;
}

/// The caIssuers URI of a certificate's authorityInfoAccess (RFC 5280
/// 4.2.2.1), if any.
[[nodiscard]] std::string ca_issuers_uri(const x509::ParsedCertificate& cert) {
    for (const auto& extension : cert.extensions) {
        if (extension.oid != "1.3.6.1.5.5.7.1.1") continue;
        Der top{extension.value};
        u8 tag = 0;
        std::span<const u8> seq;
        if (!top.next(tag, seq) || tag != 0x30) return {};
        Der descriptions{seq};
        std::span<const u8> description;
        while (descriptions.next(tag, description)) {
            Der d{description};
            std::span<const u8> oid, name;
            if (!d.next(tag, oid) || tag != 0x06 || !d.next(tag, name)) continue;
            if (x509::oid_to_string(oid) == "1.3.6.1.5.5.7.48.2" && tag == 0x86) {
                return std::string(name.begin(), name.end());
            }
        }
    }
    return {};
}

void log_once(const char* what) {
    static bool logged = false;
    if (logged) return;
    logged = true;
    std::fprintf(stderr, "[vitreo] crypt32 root auto-update unavailable: %s\n", what);
}

/// The CTL in authroot.stl (its full DER element), verified, or nullopt.
[[nodiscard]] std::optional<std::vector<u8>> verified_auth_root_ctl() {
    const HttpResponse cab = http_request("GET", kAuthRootCab);
    if (!cab.success || cab.status_code != 200) {
        log_once("authrootstl.cab could not be downloaded");
        return std::nullopt;
    }
    auto parsed_cab = compression::cabinet::Cabinet::parse(cab.body);
    if (parsed_cab.is_err()) {
        log_once("authrootstl.cab is not a cabinet");
        return std::nullopt;
    }
    std::vector<u8> stl;
    for (const auto& file : parsed_cab.value().files()) {
        std::string name = file.name;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name != "authroot.stl") continue;
        auto bytes = parsed_cab.value().extract_file(file);
        if (bytes.is_ok()) stl = std::move(bytes).value();
    }
    auto sd = authenticode::parse_signed_data(stl);
    if (stl.empty() || sd.is_err() || sd.value().content_type != kCtlOid || sd.value().signers.empty()) {
        log_once("authroot.stl is not a signed certificate trust list");
        return std::nullopt;
    }
    const auto& data = sd.value();
    const auto* signer_cert = find_signer_certificate(data.certificates, data.signers[0]);
    auto signer = signer_cert ? x509::parse_certificate(signer_cert->der) : std::nullopt;
    if (!signer || verify_signer_attributes(data.signers[0], *signer, data.content_value) != 0) {
        log_once("authroot.stl signature does not verify");
        return std::nullopt;
    }
    // The signer and every CA above it are time-valid and allowed to sign
    // root lists; the chain ends at a root fetched from Microsoft over HTTPS.
    const u64 now = now_filetime();
    std::vector<x509::ParsedCertificate> chain{*signer};
    for (int depth = 0; depth < 8; ++depth) {
        const auto& current = chain.back();
        bool found = false;
        for (const auto& c : data.certificates) {
            auto candidate = x509::parse_certificate(c.der);
            if (!candidate || candidate->subject_der != current.issuer_der ||
                candidate->subject_der == candidate->issuer_der || !certificate_signed_by(current, *candidate)) {
                continue;
            }
            chain.push_back(*candidate);
            found = true;
            break;
        }
        if (!found) break;
    }
    for (const auto& cert : chain) {
        bool has_eku = false;
        const auto eku = eku_of(cert, has_eku);
        if (now < cert.not_before_filetime || now > cert.not_after_filetime ||
            (has_eku && std::find(eku.begin(), eku.end(), kRootListSigner) == eku.end()) ||
            (&cert == &chain.front() && !has_eku)) {
            log_once("authroot.stl signer is not a valid root list signer");
            return std::nullopt;
        }
    }
    std::string uri = ca_issuers_uri(chain.back());
    constexpr std::string_view kHttp = "http://www.microsoft.com/";
    constexpr std::string_view kHttps = "https://www.microsoft.com/";
    if (uri.rfind(kHttp, 0) == 0) uri = std::string(kHttps) + uri.substr(kHttp.size());
    if (uri.rfind(kHttps, 0) != 0) {
        log_once("authroot.stl signing root is not published on www.microsoft.com");
        return std::nullopt;
    }
    const HttpResponse root_response = http_request("GET", uri);
    auto root = root_response.success && root_response.status_code == 200
                    ? x509::parse_certificate(root_response.body) : std::nullopt;
    if (!root || root->subject_der != root->issuer_der || !certificate_signed_by(*root, *root) ||
        !certificate_signed_by(chain.back(), *root) || now < root->not_before_filetime ||
        now > root->not_after_filetime) {
        log_once("authroot.stl signing root could not be fetched and verified over HTTPS");
        return std::nullopt;
    }
    ipc::ipc_log("crypt32 auto-update: authroot.stl verified (%zu bytes, root from %s)",
                 data.content_value.size(), uri.c_str());
    // The CTL element itself (the digest covered its contents octets).
    return data.content;
}

}  // namespace

std::optional<AuthRootEntry> parse_auth_root_entry(std::span<const u8> der) {
    Der outer{der};
    u8 tag = 0;
    std::span<const u8> body;
    if (!outer.next(tag, body) || tag != 0x30) return std::nullopt;
    Der fields{body};
    std::span<const u8> id;
    if (!fields.next(tag, id) || tag != 0x04 || id.size() != 20) return std::nullopt;
    AuthRootEntry entry;
    std::copy(id.begin(), id.end(), entry.sha1.begin());
    entry.der.assign(der.begin(), der.end());
    std::span<const u8> attributes;
    if (!fields.more()) return entry;
    if (!fields.next(tag, attributes) || tag != 0x31) return std::nullopt;
    Der list{attributes};
    std::span<const u8> attribute;
    while (list.next(tag, attribute)) {
        Der a{attribute};
        std::span<const u8> oid, values, value;
        if (!a.next(tag, oid) || tag != 0x06 || !a.next(tag, values) || tag != 0x31) return std::nullopt;
        Der v{values};
        if (!v.next(tag, value) || tag != 0x04) continue;
        const std::string name = x509::oid_to_string(oid);
        if (name.rfind(kPropPrefix, 0) != 0) continue;
        const std::string prop = name.substr(std::string_view(kPropPrefix).size());
        const std::vector<u8> bytes(value.begin(), value.end());
        if (prop == "9") entry.eku = bytes;
        else if (prop == "20") entry.key_id = bytes;
        else if (prop == "29") entry.subject_name_md5 = bytes;
        else if (prop == "84") entry.name_constraints = true;
        else if (prop == "122") entry.disallowed_eku = bytes;
        else if (prop == "104" && bytes.size() == 8) {
            u64 ft = 0;
            for (int i = 7; i >= 0; --i) ft = (ft << 8) | bytes[static_cast<usize>(i)];
            entry.disallowed_filetime = ft;
        }
    }
    return entry;
}

std::optional<std::vector<AuthRootEntry>> parse_auth_root_ctl(std::span<const u8> ctl) {
    Der outer{ctl};
    u8 tag = 0;
    std::span<const u8> body;
    if (!outer.next(tag, body) || tag != 0x30) return std::nullopt;
    Der fields{body};
    std::span<const u8> value;
    if (!fields.next(tag, value)) return std::nullopt;
    if (tag == 0x02 && !fields.next(tag, value)) return std::nullopt;  // version
    if (tag != 0x30) return std::nullopt;
    // subjectUsage must name the root list.
    bool root_list = false;
    Der usage{value};
    std::span<const u8> oid;
    while (usage.next(tag, oid)) {
        if (tag == 0x06 && x509::oid_to_string(oid) == kRootListSigner) root_list = true;
    }
    if (!root_list) return std::nullopt;
    // listIdentifier, sequenceNumber, thisUpdate, nextUpdate,
    // subjectAlgorithm, then the subjects.
    std::span<const u8> whole;
    int sequences = 0;
    while (fields.next(tag, value, &whole)) {
        if (tag != 0x30) continue;
        if (++sequences == 1) continue;  // subjectAlgorithm
        std::vector<AuthRootEntry> entries;
        Der subjects{value};
        std::span<const u8> subject, subject_whole;
        while (subjects.next(tag, subject, &subject_whole)) {
            if (auto entry = parse_auth_root_entry(subject_whole)) entries.push_back(std::move(*entry));
        }
        return entries;
    }
    return std::nullopt;
}

}  // namespace crypt32

const std::vector<crypt32::AuthRootEntry>* Crypt32Manager::auth_root_list_locked() {
    if (m_auth_root_state == 1) return &m_auth_roots;
    if (m_auth_root_state == 2) return nullptr;
    m_auth_root_state = 2;
    const auto ctl = crypt32::verified_auth_root_ctl();
    if (!ctl) return nullptr;
    auto entries = crypt32::parse_auth_root_ctl(*ctl);
    if (!entries) {
        std::fprintf(stderr, "[vitreo] crypt32 root auto-update unavailable: authroot.stl is not a root list\n");
        return nullptr;
    }
    m_auth_roots = std::move(*entries);
    m_auth_root_state = 1;
    return &m_auth_roots;
}

bool Crypt32Manager::auto_update_root_locked(const std::vector<u8>& top_der, bool self_signed) {
    if (!m_auth_root_enabled) return false;
    auto top = x509::parse_certificate(top_der);
    if (!top) return false;
    const auto* list = auth_root_list_locked();
    if (list == nullptr) return false;
    const crypt32::AuthRootEntry* entry = nullptr;
    if (self_signed) {
        const auto sha1 = crypt32::sha1_of(top_der);
        for (const auto& e : *list) if (e.sha1 == sha1) entry = &e;
    } else {
        x509::AuthorityKeyId aki;
        for (const auto& extension : top->extensions) {
            if (extension.oid == "2.5.29.35") aki = x509::decode_authority_key_id(extension.value);
        }
        const auto issuer_md5 = crypt32::md5_of(top->issuer_der);
        for (const auto& e : *list) {
            if ((!aki.key_id.empty() && e.key_id == aki.key_id) || e.subject_name_md5 == issuer_md5) {
                entry = &e;
                break;
            }
        }
    }
    if (entry == nullptr) return false;
    if (entry->name_constraints) {
        crypt32::report_unimplemented("crypt32!CertGetCertificateChain",
                                      "auto-update root with root-program name constraints");
        return false;
    }
    const std::string hex = crypt32::hex_upper(entry->sha1);
    const HttpResponse response = http_request("GET", std::string(crypt32::kAuthRootCertBase) + hex + ".crt");
    if (!response.success || response.status_code != 200 || crypt32::sha1_of(response.body) != entry->sha1) {
        return false;
    }
    auto root = x509::parse_certificate(response.body);
    if (!root || (!self_signed && !certificate_signed_by(*top, *root))) return false;

    const std::string store = "LocalMachine\\ROOT";
    if (!load_system_store_locked(store)) return false;
    std::string record;
    if (!append_system_record_locked(store, response.body, record)) return false;
    m_system_store_certs[store].push_back(StoredCertificate{response.body, record});
    // The entry carries the root's usage restrictions; keep it beside the
    // store so they apply in later processes too.
    if (m_vfs != nullptr) {
        (void)m_vfs->create_dir("C:\\ProgramData\\Vitreo\\CertificateStores");
        (void)m_vfs->create_dir(crypt32::kAuthRootDirectory);
        const std::string path = std::string(crypt32::kAuthRootDirectory) + "\\" + hex;
        auto opened = m_vfs->open(path, capsule::AccessMode::Write, capsule::ShareMode::None,
                                  capsule::CreationDisposition::CreateAlways);
        if (opened.is_ok()) {
            (void)m_vfs->write(opened.value(), entry->der);
            (void)m_vfs->close(opened.value());
        }
    }
    m_root_restrictions[hex] = *entry;
    ipc::ipc_log("crypt32 auto-update: installed root %s", hex.c_str());
    std::fprintf(stderr, "[vitreo] crypt32: root auto-update installed %s\n", hex.c_str());
    return true;
}

std::optional<crypt32::AuthRootEntry> Crypt32Manager::root_restriction_locked(const std::vector<u8>& root_der) {
    const std::string hex = crypt32::hex_upper(crypt32::sha1_of(root_der));
    if (const auto it = m_root_restrictions.find(hex); it != m_root_restrictions.end()) return it->second;
    std::optional<crypt32::AuthRootEntry> entry;
    if (m_vfs != nullptr) {
        const std::string path = std::string(crypt32::kAuthRootDirectory) + "\\" + hex;
        auto opened = m_vfs->open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                                  capsule::CreationDisposition::OpenExisting);
        if (opened.is_ok()) {
            std::vector<u8> bytes(4096);
            auto n = m_vfs->read(opened.value(), bytes);
            (void)m_vfs->close(opened.value());
            if (n.is_ok()) {
                bytes.resize(n.value());
                entry = crypt32::parse_auth_root_entry(bytes);
            }
        }
    }
    m_root_restrictions[hex] = entry;
    return entry;
}

}  // namespace vitreo::winapi

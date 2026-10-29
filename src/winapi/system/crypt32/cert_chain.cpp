// SPDX-License-Identifier: LGPL-2.1-only
/// @file cert_chain.cpp
/// @brief Certificate chain engine: CertGetCertificateChain,
///        CertFree/DuplicateCertificateChain, CertVerifyCertificateChainPolicy,
///        CertCreate/FreeCertificateChainEngine.
///
/// Contracts:
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetcertificatechain
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/ns-wincrypt-cert_trust_status
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certverifycertificatechainpolicy
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/ns-wincrypt-cert_chain_engine_config
/// Measured Windows behaviour (x86 and x64) is tools/oracle_probe/
/// crypt_chain_probe.c (+ .expected / .x86.expected). The model below is
/// derived from it:
///  - issuers come from hAdditionalStore, the engine's stores and the system
///    CA/ROOT stores, never from the subject context's own hCertStore;
///  - a missing issuer is CERT_TRUST_IS_PARTIAL_CHAIN on the chain only; a
///    self-signed top that no trusted root store holds is
///    CERT_TRUST_IS_UNTRUSTED_ROOT on that element; a signature that does not
///    verify is CERT_TRUST_IS_NOT_SIGNATURE_VALID on the subject; time and
///    requested-usage failures land on the element concerned;
///  - element info: HAS_KEY/EXACT/NAME_MATCH_ISSUER by the subject's
///    authorityKeyIdentifier form, IS_SELF_SIGNED, HAS_PREFERRED_ISSUER once
///    the issuer verified, IS_FROM_EXCLUSIVE_TRUST_STORE on an exclusive
///    root; the chain carries HAS_PREFERRED_ISSUER only if every element does;
///  - revocation is reported the way Windows reports it when no status can be
///    fetched (Vitreo has no CRL/OCSP retrieval): UNKNOWN (+ IS_OFFLINE when
///    the chain is not trusted or the certificate names a URL).

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "winapi/system/crypt32/guest_block.hpp"
#include "x509_parse.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <functional>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <optional>
#include <string>

namespace vitreo::winapi {

namespace {

// CERT_TRUST_STATUS.dwErrorStatus (wincrypt.h)
constexpr u32 kNotTimeValid = 0x00000001u;
constexpr u32 kNotSignatureValid = 0x00000008u;
constexpr u32 kNotValidForUsage = 0x00000010u;
constexpr u32 kUntrustedRoot = 0x00000020u;
constexpr u32 kRevocationUnknown = 0x00000040u;
constexpr u32 kIsRevoked = 0x00000004u;
constexpr u32 kPartialChain = 0x00010000u;
constexpr u32 kOfflineRevocation = 0x01000000u;
// dwInfoStatus
constexpr u32 kExactMatchIssuer = 0x00000001u;
constexpr u32 kKeyMatchIssuer = 0x00000002u;
constexpr u32 kNameMatchIssuer = 0x00000004u;
constexpr u32 kSelfSigned = 0x00000008u;
constexpr u32 kPreferredIssuer = 0x00000100u;
constexpr u32 kFromExclusiveTrustStore = 0x00002000u;
// dwFlags
constexpr u32 kRevocationEndCert = 0x10000000u;
constexpr u32 kRevocationChain = 0x20000000u;
constexpr u32 kRevocationChainExcludeRoot = 0x40000000u;
// Revocation results
constexpr u32 kCryptENoRevocationCheck = 0x80092012u;
constexpr u32 kCryptERevocationOffline = 0x80092013u;
constexpr u32 kCryptERevoked = 0x80092010u;
// Policy errors (winerror.h)
constexpr u32 kTrustECertSignature = 0x80096004u;
constexpr u32 kCertEChaining = 0x800B010Au;
constexpr u32 kCertEUntrustedRoot = 0x800B0109u;
constexpr u32 kCertEExpired = 0x800B0101u;
constexpr u32 kCertEWrongUsage = 0x800B0110u;
constexpr u32 kCertERevocationFailure = 0x800B010Eu;
constexpr u32 kCertECnNoMatch = 0x800B010Fu;

constexpr u32 kUsageMatchAnd = 0;
constexpr u32 kUsageMatchOr = 1;
// HCCE_* predefined engines.
constexpr u32 kHcceCurrentUser = 0;
constexpr u32 kHcceLocalMachine = 1;
constexpr u32 kHcceSerialLocalMachine = 2;

u64 current_filetime() {
    return (static_cast<u64>(std::time(nullptr)) + 11644473600ULL) * 10000000ULL;
}

struct Node {
    std::vector<u8> der;
    x509::ParsedCertificate parsed;
    std::vector<u8> ski;
};

/// EKU for one certificate: nullopt = valid for all uses.
std::optional<std::vector<std::string>> certificate_usage(
    const x509::ParsedCertificate& cert, const std::vector<u8>* property) {
    std::optional<std::vector<std::string>> ext;
    for (const auto& extension : cert.extensions) {
        if (extension.oid != "2.5.29.37") continue;
        std::vector<std::string> oids;
        if (x509::decode_enhanced_key_usage(extension.value, oids) == 0) ext = oids;
        break;
    }
    std::optional<std::vector<std::string>> prop;
    if (property != nullptr) {
        std::vector<std::string> oids;
        if (x509::decode_enhanced_key_usage(*property, oids) == 0) prop = oids;
    }
    if (ext && prop) {
        std::vector<std::string> both;
        for (const auto& oid : *ext) {
            if (std::find(prop->begin(), prop->end(), oid) != prop->end()) both.push_back(oid);
        }
        return both;
    }
    return ext ? ext : prop;
}

/// Restrict `upper` (the issuer's effective set) by the subject's own set,
/// keeping the issuer's order (crypt_chain_probe "bnet" app: rows).
std::optional<std::vector<std::string>> intersect(
    const std::optional<std::vector<std::string>>& upper,
    const std::optional<std::vector<std::string>>& own) {
    if (!upper) return own;
    if (!own) return upper;
    std::vector<std::string> out;
    for (const auto& oid : *upper) {
        if (std::find(own->begin(), own->end(), oid) != own->end()) out.push_back(oid);
    }
    return out;
}

bool usage_satisfies(const std::optional<std::vector<std::string>>& usage,
                     const std::vector<std::string>& requested, u32 match_type) {
    if (!usage || requested.empty()) return true;
    auto has = [&](const std::string& oid) {
        return std::find(usage->begin(), usage->end(), oid) != usage->end();
    };
    if (match_type == kUsageMatchOr) return std::any_of(requested.begin(), requested.end(), has);
    return std::all_of(requested.begin(), requested.end(), has);
}

std::vector<std::string> read_oid_array(Marshaler& m, GuestAddr array, u32 count) {
    std::vector<std::string> out;
    const u32 ps = m.cpu_is_64bit() ? 8u : 4u;
    for (u32 i = 0; i < count && array != 0 && i < 256; ++i) {
        const auto text = m.read_ansi_string(
            static_cast<GuestAddr>(m.read_ptr(array + static_cast<GuestAddr>(i) * ps).value_or(0)),
            256);
        if (text.is_ok()) out.push_back(text.value());
    }
    return out;
}

} // namespace

// =============================================================================
// Engines
// =============================================================================

// BOOL CertCreateCertificateChainEngine(PCERT_CHAIN_ENGINE_CONFIG pConfig,
//     HCERTCHAINENGINE* phChainEngine)
// cbSize must be one of the structure's published sizes (without, with the
// Windows 7 exclusive stores, with dwExclusiveFlags); anything else is
// E_INVALIDARG (crypt_chain_probe "engine_cb" rows).
u32 Crypt32Manager::cert_create_certificate_chain_engine(Marshaler& m) {
    const GuestAddr config = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = is64 ? 8u : 4u;
    const u32 size = m.read_u32(config).value_or(0);
    const u32 base_size = is64 ? 0x40u : 0x28u;
    const u32 exclusive_size = is64 ? 0x50u : 0x30u;
    const u32 full_size = is64 ? 0x58u : 0x34u;
    if (config == 0 || out == 0 ||
        (size != base_size && size != exclusive_size && size != full_size)) {
        set_last_error(crypt32::err::kEInvalidArg);
        return 0;
    }
    auto handle_at = [&](u32 offset) {
        return static_cast<u32>(m.read_ptr(config + offset).value_or(0));
    };
    ChainEngine engine;
    engine.restricted_root = handle_at(ps);
    const u32 restricted_trust = handle_at(2 * ps);
    const u32 restricted_other = handle_at(3 * ps);
    const u32 additional_count = m.read_u32(config + 4 * ps).value_or(0);
    const GuestAddr additional =
        static_cast<GuestAddr>(m.read_ptr(config + 5 * ps).value_or(0));
    engine.flags = m.read_u32(config + 6 * ps).value_or(0);
    if (size >= exclusive_size) engine.exclusive_root = handle_at(base_size);
    const u32 exclusive_trusted_people = size >= exclusive_size ? handle_at(base_size + ps) : 0;
    if (restricted_trust != 0 || restricted_other != 0 || exclusive_trusted_people != 0) {
        crypt32::report_unimplemented(
            "crypt32!CertCreateCertificateChainEngine",
            "hRestrictedTrust / hRestrictedOther / hExclusiveTrustedPeople (CTL and peer trust)");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    for (u32 i = 0; i < additional_count && additional != 0 && i < 64; ++i) {
        engine.additional_stores.push_back(static_cast<u32>(
            m.read_ptr(additional + static_cast<GuestAddr>(i) * ps).value_or(0)));
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    hold_store_locked(engine.restricted_root);
    hold_store_locked(engine.exclusive_root);
    for (const u32 store : engine.additional_stores) hold_store_locked(store);
    const u32 handle = m_next_chain_engine++;
    m_chain_engines[handle] = std::move(engine);
    (void)m.write_ptr(out, handle);
    return 1;
}

void Crypt32Manager::cert_free_certificate_chain_engine(Marshaler& m) {
    const u32 handle = static_cast<u32>(m.arg(0));
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_chain_engines.find(handle);
    if (found == m_chain_engines.end()) return;
    release_store_hold_locked(found->second.restricted_root);
    release_store_hold_locked(found->second.exclusive_root);
    for (const u32 store : found->second.additional_stores) release_store_hold_locked(store);
    m_chain_engines.erase(found);
}

// =============================================================================
// CertGetCertificateChain
// =============================================================================

u32 Crypt32Manager::cert_get_certificate_chain(Marshaler& m) {
    const u32 engine_handle = static_cast<u32>(m.arg(0));
    const GuestAddr leaf_context = m.arg_ptr(1);
    const GuestAddr time_ptr = m.arg_ptr(2);
    const u32 additional_store = static_cast<u32>(m.arg(3));
    const GuestAddr para = m.arg_ptr(4);
    const u32 flags = m.arg32(5);
    const GuestAddr out = m.arg_ptr(7);
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = is64 ? 8u : 4u;
    constexpr const char* kApi = "crypt32!CertGetCertificateChain";

    if (out != 0) (void)m.write_ptr(out, 0);
    if (para == 0 || out == 0) {
        set_last_error(crypt32::err::kEInvalidArg);
        return 0;
    }
    if (leaf_context == 0) {
        // Windows dereferences pCertContext (crypt_chain_probe "nullcert"
        // row: STATUS_ACCESS_VIOLATION).
        crypt32::raise_access_violation(m, ps, false);
        return 0;
    }

    // CERT_CHAIN_PARA { cbSize; CERT_USAGE_MATCH RequestedUsage; ...extra }.
    const u32 para_size = m.read_u32(para).value_or(0);
    u32 usage_type = kUsageMatchAnd;
    std::vector<std::string> requested;
    const u32 usage_offset = ps;                   // after cbSize (+pad)
    const u32 usage_size = is64 ? 24u : 12u;       // CERT_USAGE_MATCH
    if (para_size >= usage_offset + usage_size) {
        usage_type = m.read_u32(para + usage_offset).value_or(0);
        const u32 count = m.read_u32(para + usage_offset + ps).value_or(0);
        const GuestAddr array =
            static_cast<GuestAddr>(m.read_ptr(para + usage_offset + 2 * ps).value_or(0));
        requested = read_oid_array(m, array, count);
    }
    // The optional tail (RequestedIssuancePolicy, freshness, strong-sign
    // parameters) is only honoured when unused.
    const u32 issuance_offset = usage_offset + usage_size;
    if (para_size >= issuance_offset + usage_size &&
        m.read_u32(para + issuance_offset + ps).value_or(0) != 0) {
        crypt32::report_unimplemented(kApi, "CERT_CHAIN_PARA.RequestedIssuancePolicy");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    const u32 strong_offset = issuance_offset + usage_size + 4 * 4 + ps;
    if (para_size >= strong_offset + ps &&
        m.read_ptr(para + strong_offset).value_or(0) != 0) {
        crypt32::report_unimplemented(kApi, "CERT_CHAIN_PARA.pStrongSignPara");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }

    ChainRequest request;
    request.engine = engine_handle;
    request.additional_store = additional_store;
    request.when = current_filetime();
    if (time_ptr != 0) {
        const u32 low = m.read_u32(time_ptr).value_or(0);
        const u32 high = m.read_u32(time_ptr + 4).value_or(0);
        request.when = (static_cast<u64>(high) << 32) | low;
    }
    request.usage_type = usage_type;
    request.requested_usage = requested;
    request.flags = flags;

    ChainResult result;
    bool tracked = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<u8> leaf_der;
        std::optional<std::vector<u8>> leaf_eku;
        auto leaf_entry = m_cert_contexts.find(leaf_context);
        if (leaf_entry != m_cert_contexts.end()) {
            tracked = true;
            leaf_der = leaf_entry->second.der_data;
            auto prop = leaf_entry->second.props->blobs.find(9);
            if (prop != leaf_entry->second.props->blobs.end()) leaf_eku = prop->second;
        } else {
            const GuestAddr bytes =
                static_cast<GuestAddr>(m.read_ptr(leaf_context + ps).value_or(0));
            const u32 size = m.read_u32(leaf_context + 2 * ps).value_or(0);
            leaf_der.resize(size);
            if (size == 0 || size > 0x100000 ||
                m.memory().read_bytes(bytes, reinterpret_cast<Byte*>(leaf_der.data()),
                                      size).is_err()) {
                set_last_error(crypt32::err::kEInvalidArg);
                return 0;
            }
        }
        if (const u32 error = build_chain_locked(request, leaf_der, leaf_eku, result)) {
            set_last_error(error);
            return 0;
        }
    }
    const MaterializedChain chain =
        materialize_chain(m, result, tracked ? leaf_context : 0);
    if (chain.chain == 0) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    (void)m.write_ptr(out, chain.chain);
    // Measured: the success path leaves the error from Windows' internal
    // store lookups (CRYPT_E_NOT_FOUND), except after a revocation pass
    // over a trusted chain.
    set_last_error(result.last_error_after);
    return 1;
}

u32 Crypt32Manager::build_chain_locked(const ChainRequest& request,
                                       const std::vector<u8>& leaf_der,
                                       const std::optional<std::vector<u8>>& leaf_eku,
                                       ChainResult& out) {
    const u32 result = build_chain_once_locked(request, leaf_der, leaf_eku, out);
    if (result != 0 || out.trusted || out.elements.empty()) return result;
    // Automatic root update (auth_root.cpp), unless the engine has an
    // exclusive root store or opted out with
    // CERT_CHAIN_DISABLE_AUTH_ROOT_AUTO_UPDATE / CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL
    // (CERT_CHAIN_ENGINE_CONFIG.dwFlags, wincrypt.h).
    constexpr u32 kDisableAuthRootAutoUpdate = 0x00000100u;
    constexpr u32 kCacheOnlyUrlRetrieval = 0x00000004u;
    if (request.engine != kHcceCurrentUser && request.engine != kHcceLocalMachine &&
        request.engine != kHcceSerialLocalMachine) {
        const auto engine = m_chain_engines.find(request.engine);
        if (engine == m_chain_engines.end() || engine->second.exclusive_root != 0 ||
            (engine->second.flags & (kDisableAuthRootAutoUpdate | kCacheOnlyUrlRetrieval)) != 0) {
            return result;
        }
    }
    // A complete chain that is not trusted ends at a self-signed root no
    // store trusts; an incomplete one lacks the top certificate's issuer.
    if (!auto_update_root_locked(out.elements.back().der, out.complete)) return result;
    out = ChainResult{};
    return build_chain_once_locked(request, leaf_der, leaf_eku, out);
}

u32 Crypt32Manager::build_chain_once_locked(const ChainRequest& request,
                                            const std::vector<u8>& leaf_der,
                                            const std::optional<std::vector<u8>>& leaf_eku,
                                            ChainResult& out) {
    constexpr const char* kApi = "crypt32!CertGetCertificateChain";
    auto leaf_parsed = x509::parse_certificate(leaf_der);
    if (!leaf_parsed) return x509::kCryptEAsn1Corrupt;
    const u32 engine_handle = request.engine;
    const u32 flags = request.flags;
    const u64 when = request.when;
    const auto& requested = request.requested_usage;
    const u32 usage_type = request.usage_type;
    const std::vector<u8>* leaf_eku_property = leaf_eku ? &*leaf_eku : nullptr;
    {
        {
        const ChainEngine* engine = nullptr;
        if (engine_handle != kHcceCurrentUser && engine_handle != kHcceLocalMachine &&
            engine_handle != kHcceSerialLocalMachine) {
            auto found = m_chain_engines.find(engine_handle);
            if (found == m_chain_engines.end()) return crypt32::err::kEInvalidArg;
            engine = &found->second;
        }
        const bool machine = engine_handle == kHcceLocalMachine ||
                             engine_handle == kHcceSerialLocalMachine ||
                             (engine != nullptr && (engine->flags & 0x1) != 0);

        std::vector<std::vector<u8>> candidates;  // intermediates and roots
        std::vector<std::vector<u8>> trusted;     // trust anchors
        auto append_store = [&](u32 store, std::vector<std::vector<u8>>& into) {
            for (auto& der : store_certificates_locked(store)) into.push_back(std::move(der));
        };
        auto append_system = [&](const std::string& key, std::vector<std::vector<u8>>& into) {
            if (!load_system_store_locked(key)) return;
            for (const auto& record : m_system_store_certs[key]) into.push_back(record.der_data);
        };
        if (request.additional_store != 0) append_store(request.additional_store, candidates);
        for (const auto& der : request.extra_certificates) candidates.push_back(der);
        if (engine != nullptr) {
            for (const u32 store : engine->additional_stores) append_store(store, candidates);
        }
        const std::string location = machine ? "LocalMachine" : "CurrentUser";
        append_system(location + "\\CA", candidates);
        if (!machine) append_system("LocalMachine\\CA", candidates);
        const bool exclusive = engine != nullptr && engine->exclusive_root != 0;
        if (exclusive) {
            append_store(engine->exclusive_root, trusted);
        } else {
            append_system(location + "\\ROOT", trusted);
            if (!machine) append_system("LocalMachine\\ROOT", trusted);
            if (engine != nullptr && engine->restricted_root != 0) {
                std::vector<std::vector<u8>> restricted;
                append_store(engine->restricted_root, restricted);
                std::erase_if(trusted, [&](const std::vector<u8>& der) {
                    return std::find(restricted.begin(), restricted.end(), der) ==
                           restricted.end();
                });
            }
        }
        for (const auto& der : trusted) candidates.push_back(der);

        // --- build ---------------------------------------------------------
        std::vector<Node> pool;
        pool.reserve(candidates.size());
        for (auto& der : candidates) {
            if (std::any_of(pool.begin(), pool.end(),
                            [&](const Node& n) { return n.der == der; })) {
                continue;
            }
            auto parsed = x509::parse_certificate(der);
            if (!parsed) continue;
            Node node{std::move(der), std::move(*parsed), {}};
            node.ski = x509::certificate_key_identifier(node.parsed);
            pool.push_back(std::move(node));
        }
        auto is_trusted = [&](const std::vector<u8>& der) {
            return std::find(trusted.begin(), trusted.end(), der) != trusted.end();
        };

        struct Element {
            Node node;
            u32 error{0};
            u32 info{0};
            bool issuer_verified{false};
        };
        // Every candidate path from the leaf, then the one CAPI2 selects:
        // highest quality (complete and trusted, then complete; fewer
        // signature failures; more key-identifier matches), then the later
        // NotBefore of the issuers starting at the end entity's issuer, then
        // the later NotAfter, then the shorter chain (Microsoft, "Certificate
        // Path Validation in Bridge CA and Cross-Certification Environments";
        // tools/oracle_probe/wintrust_signer_name_probe.expected: socialclub's
        // signer chain takes the newer G4 cross-certificate to the Assured ID
        // root, not the 2013 self-signed G4 root).
        Node leaf_node{leaf_der, std::move(*leaf_parsed), {}};
        leaf_node.ski = x509::certificate_key_identifier(leaf_node.parsed);
        struct Path {
            std::vector<const Node*> nodes;
            std::vector<bool> verified;  // issuer signature of nodes[k] checked
            bool complete{false};
            bool top_trusted{false};
            u32 signature_failures{0};
            u32 key_matches{0};
        };
        auto authority_key = [](const x509::ParsedCertificate& cert) {
            x509::AuthorityKeyId aki;
            for (const auto& extension : cert.extensions) {
                if (extension.oid == "2.5.29.35") aki = x509::decode_authority_key_id(extension.value);
            }
            return aki;
        };
        std::vector<Path> paths;
        constexpr usize kMaxPaths = 64;
        std::function<void(Path&)> extend = [&](Path& path) {
            if (paths.size() >= kMaxPaths) return;
            const Node& current = *path.nodes.back();
            const auto& cert = current.parsed;
            if (is_trusted(current.der) && cert.issuer_der != cert.subject_der) {
                // A trust anchor that is not self-issued still ends the chain.
                path.complete = path.top_trusted = true;
                paths.push_back(path);
                return;
            }
            if (cert.issuer_der == cert.subject_der && certificate_signed_by(cert, cert)) {
                path.complete = true;
                path.top_trusted = is_trusted(current.der);
                path.verified.back() = true;
                paths.push_back(path);
                return;
            }
            const auto aki = authority_key(cert);
            bool extended = false;
            if (path.nodes.size() < 20) {
                for (const Node& node : pool) {
                    if (node.parsed.subject_der != cert.issuer_der) continue;
                    if (std::any_of(path.nodes.begin(), path.nodes.end(),
                                    [&](const Node* n) { return n->der == node.der; })) {
                        continue;
                    }
                    const bool verifies = certificate_signed_by(cert, node.parsed);
                    const bool key_match = !aki.key_id.empty() && node.ski == aki.key_id;
                    Path next = path;
                    next.verified.back() = verifies;
                    next.signature_failures += verifies ? 0u : 1u;
                    next.key_matches += key_match ? 1u : 0u;
                    next.nodes.push_back(&node);
                    next.verified.push_back(false);
                    extended = true;
                    extend(next);
                }
            }
            if (!extended) paths.push_back(path);  // partial chain
        };
        {
            Path first;
            first.nodes.push_back(&leaf_node);
            first.verified.push_back(false);
            extend(first);
        }
        const auto better = [](const Path& a, const Path& b) {
            const int qa = a.complete ? (a.top_trusted ? 2 : 1) : 0;
            const int qb = b.complete ? (b.top_trusted ? 2 : 1) : 0;
            if (qa != qb) return qa > qb;
            if (a.signature_failures != b.signature_failures)
                return a.signature_failures < b.signature_failures;
            if (a.key_matches != b.key_matches) return a.key_matches > b.key_matches;
            for (usize k = 1; k < std::min(a.nodes.size(), b.nodes.size()); ++k) {
                const auto& x = a.nodes[k]->parsed;
                const auto& y = b.nodes[k]->parsed;
                if (x.not_before_filetime != y.not_before_filetime)
                    return x.not_before_filetime > y.not_before_filetime;
                if (x.not_after_filetime != y.not_after_filetime)
                    return x.not_after_filetime > y.not_after_filetime;
            }
            return a.nodes.size() < b.nodes.size();
        };
        const Path* best = &paths.front();
        for (const Path& path : paths)
            if (better(path, *best)) best = &path;

        std::vector<Element> chain;
        for (usize k = 0; k < best->nodes.size(); ++k) {
            Element element{*best->nodes[k]};
            const auto aki = authority_key(element.node.parsed);
            element.info |= !aki.key_id.empty() ? kKeyMatchIssuer
                          : aki.has_issuer_serial ? kExactMatchIssuer
                          : kNameMatchIssuer;
            const bool top = k + 1 == best->nodes.size();
            const auto& cert = element.node.parsed;
            if (top && best->complete && cert.issuer_der == cert.subject_der) {
                element.info |= kSelfSigned | kPreferredIssuer;
                element.issuer_verified = true;
            } else if (!top) {
                if (best->verified[k]) {
                    element.info |= kPreferredIssuer;
                    element.issuer_verified = true;
                } else {
                    element.error |= kNotSignatureValid;
                }
            }
            chain.push_back(std::move(element));
        }
        const bool complete = best->complete;
        bool top_trusted = best->top_trusted;
        // A root the automatic update installed carries the restrictions of
        // its list entry: allowed EKUs (property 9), and no trust for a leaf
        // issued after CERT_DISALLOWED_FILETIME (104) -- for the
        // CERT_DISALLOWED_ENHKEY_USAGE (122) usages only, when given.
        std::optional<crypt32::AuthRootEntry> root_entry;
        if (complete && top_trusted) root_entry = root_restriction_locked(chain.back().node.der);
        if (root_entry && root_entry->disallowed_filetime &&
            chain.front().node.parsed.not_before_filetime > *root_entry->disallowed_filetime) {
            bool applies = true;
            if (root_entry->disallowed_eku) {
                std::vector<std::string> disallowed;
                (void)x509::decode_enhanced_key_usage(*root_entry->disallowed_eku, disallowed);
                const auto leaf_usage = certificate_usage(chain.front().node.parsed, leaf_eku_property);
                applies = !leaf_usage || std::any_of(leaf_usage->begin(), leaf_usage->end(), [&](const std::string& o) {
                    return std::find(disallowed.begin(), disallowed.end(), o) != disallowed.end();
                });
            }
            if (applies) top_trusted = false;
        }
        if (complete && !top_trusted) chain.back().error |= kUntrustedRoot;
        if (complete && top_trusted && exclusive) chain.back().info |= kFromExclusiveTrustStore;
        const bool chain_trusted = complete && top_trusted;

        // Time validity, application / issuance usage (top down).
        std::vector<std::optional<std::vector<std::string>>> app(chain.size());
        std::vector<std::optional<std::vector<std::string>>> issuance(chain.size());
        std::optional<std::vector<std::string>> app_upper;
        std::optional<std::vector<std::string>> iss_upper;
        for (usize k = chain.size(); k-- > 0;) {
            Element& element = chain[k];
            const auto& cert = element.node.parsed;
            if (when < cert.not_before_filetime || when > cert.not_after_filetime) {
                element.error |= kNotTimeValid;
            }
            const std::vector<u8>* property = k == 0 ? leaf_eku_property : nullptr;
            if (k + 1 == chain.size() && root_entry && root_entry->eku) property = &*root_entry->eku;
            app[k] = intersect(app_upper, certificate_usage(cert, property));
            app_upper = app[k];
            if (!usage_satisfies(app[k], requested, usage_type)) {
                element.error |= kNotValidForUsage;
            }
            const bool self_signed_top = k + 1 == chain.size() && (element.info & kSelfSigned);
            if (!self_signed_top) {
                std::optional<std::vector<std::string>> own;
                for (const auto& extension : cert.extensions) {
                    if (extension.oid == "2.5.29.32") {
                        own = x509::decode_certificate_policies(extension.value);
                    }
                }
                std::vector<std::string> policies =
                    iss_upper && !iss_upper->empty() &&
                            std::find(iss_upper->begin(), iss_upper->end(), "2.5.29.32.0") ==
                                iss_upper->end()
                        ? *intersect(iss_upper, own ? own : std::optional<std::vector<std::string>>(
                                                              std::vector<std::string>{}))
                        : own.value_or(std::vector<std::string>{});
                issuance[k] = policies;
                iss_upper = issuance[k];
            }
        }

        // Revocation: reported as Windows reports it when no status can be
        // retrieved, since Vitreo fetches neither CRLs nor OCSP responses.
        struct Revocation { bool present{false}; u32 result{0}; };
        std::vector<Revocation> revocation(chain.size());
        const bool check_end = (flags & (kRevocationEndCert | kRevocationChain |
                                         kRevocationChainExcludeRoot)) != 0;
        if (check_end) {
            for (usize k = 0; k < chain.size(); ++k) {
                const bool self_signed_top = k + 1 == chain.size() &&
                                             (chain[k].info & kSelfSigned) != 0;
                const bool wanted = k == 0 || (flags & (kRevocationChain |
                                                        kRevocationChainExcludeRoot)) != 0;
                if (!wanted) continue;
                if (self_signed_top) {
                    if ((flags & kRevocationChain) != 0 &&
                        (flags & kRevocationChainExcludeRoot) == 0) {
                        revocation[k] = {true, 0};
                    }
                    continue;
                }
                const bool urls = x509::has_revocation_urls(chain[k].node.parsed);
                if (chain_trusted && !urls) {
                    chain[k].error |= kRevocationUnknown;
                    revocation[k] = {true, kCryptENoRevocationCheck};
                } else {
                    if (chain_trusted) {
                        crypt32::report_unimplemented(
                            kApi, "CRL/OCSP retrieval (revocation reported offline)");
                    }
                    chain[k].error |= kRevocationUnknown | kOfflineRevocation;
                    revocation[k] = {true, kCryptERevocationOffline};
                }
            }
            if (chain_trusted) out.last_error_after = 0;
        }

        u32 chain_error = complete ? 0u : kPartialChain;
        u32 chain_info = kPreferredIssuer;
        for (const auto& element : chain) {
            chain_error |= element.error;
            if ((element.info & kPreferredIssuer) == 0) chain_info = 0;
        }
        if (!complete) chain_info = 0;
        chain_info |= chain.back().info & kFromExclusiveTrustStore;


        out.elements.clear();
        for (usize k = 0; k < chain.size(); ++k) {
            ChainResult::Element element;
            element.der = std::move(chain[k].node.der);
            element.error = chain[k].error;
            element.info = chain[k].info;
            element.app = std::move(app[k]);
            element.issuance = std::move(issuance[k]);
            element.revocation_present = revocation[k].present;
            element.revocation_result = revocation[k].result;
            out.elements.push_back(std::move(element));
        }
        out.error = chain_error;
        out.info = chain_info;
        out.complete = complete;
        out.trusted = chain_trusted;
        out.flags = flags;
    }
    }
    return 0;
}

Crypt32Manager::MaterializedChain Crypt32Manager::materialize_chain(
    Marshaler& m, const ChainResult& result, GuestAddr leaf_context) {
    MaterializedChain made;
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = is64 ? 8u : 4u;
    std::vector<GuestAddr> element_contexts(result.elements.size(), 0);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto leaf_entry = m_cert_contexts.find(leaf_context);
        if (leaf_context != 0 && leaf_entry != m_cert_contexts.end()) {
            // The leaf element is the caller's context with another reference.
            ++leaf_entry->second.reference_count;
            element_contexts[0] = leaf_context;
        }
    }
    bool failed = false;
    for (usize k = 0; k < result.elements.size(); ++k) {
        if (element_contexts[k] != 0) continue;
        element_contexts[k] = create_cert_context_from_der(m, result.elements[k].der, 0, 1u);
        if (element_contexts[k] == 0) failed = true;
    }

    crypt32::GuestBlock block(is64);
    const u32 chain_size = is64 ? 0x48u : 0x38u;
    const u32 simple_size = is64 ? 0x28u : 0x20u;
    const u32 element_size = is64 ? 0x38u : 0x20u;
    const u32 revocation_size = is64 ? 0x28u : 0x1cu;
    const u32 c = block.reserve(chain_size);
    block.put_u32(c + 0, chain_size);
    block.put_u32(c + 4, result.error);
    block.put_u32(c + 8, result.info);
    block.put_u32(c + 12, 1);
    const u32 chain_array = block.reserve(ps);
    block.put_ptr(c + 16, chain_array);
    block.put_u32(c + (is64 ? 48u : 36u), result.flags);
    {
        const u32 id = c + (is64 ? 52u : 40u);
        for (u32 i = 0; i < 4; ++i) block.put_u32(id + 4 * i, arc4random());
    }
    const u32 simple = block.reserve(simple_size);
    block.put_ptr(chain_array, simple);
    block.put_u32(simple + 0, simple_size);
    block.put_u32(simple + 4, result.error);
    block.put_u32(simple + 8, result.info);
    block.put_u32(simple + 12, static_cast<u32>(result.elements.size()));
    const u32 element_array = block.reserve(static_cast<u32>(result.elements.size()) * ps);
    block.put_ptr(simple + 16, element_array);
    auto put_usage = [&](u32 at, const std::optional<std::vector<std::string>>& usage) {
        if (!usage) return;
        const u32 header = block.reserve(2 * ps);
        block.put_ptr(at, header);
        block.put_u32(header, static_cast<u32>(usage->size()));
        if (usage->empty()) return;
        const u32 array = block.reserve(static_cast<u32>(usage->size()) * ps);
        block.put_ptr(header + ps, array);
        for (usize i = 0; i < usage->size(); ++i) {
            block.put_ptr(array + static_cast<u32>(i) * ps, block.add_cstring((*usage)[i]));
        }
    };
    std::vector<u32> element_offsets;
    for (usize k = 0; k < result.elements.size(); ++k) {
        const auto& element = result.elements[k];
        const u32 e = block.reserve(element_size);
        element_offsets.push_back(e);
        block.put_ptr(element_array + static_cast<u32>(k) * ps, e);
        block.put_u32(e + 0, element_size);
        block.put_value(e + ps, element_contexts[k]);
        block.put_u32(e + 2 * ps, element.error);
        block.put_u32(e + 2 * ps + 4, element.info);
        // CERT_CHAIN_ELEMENT { cbSize; pCertContext; TrustStatus (2 DWORDs);
        // pRevocationInfo; pIssuanceUsage; pApplicationUsage;
        // pwszExtendedErrorInfo }.
        const u32 revocation_field = 2 * ps + 8;
        if (element.revocation_present) {
            const u32 r = block.reserve(revocation_size);
            block.put_u32(r + 0, revocation_size);
            block.put_u32(r + 4, element.revocation_result);
            block.put_ptr(e + revocation_field, r);
        }
        put_usage(e + revocation_field + ps, element.issuance);
        put_usage(e + revocation_field + 2 * ps, element.app);
    }

    GuestAddr base = 0;
    if (!failed) base = alloc_guest(block.size());
    if (base == 0) {
        std::vector<GuestAddr> blocks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const GuestAddr context : element_contexts) {
                if (context == 0) continue;
                auto released = release_cert_context_locked(context);
                blocks.insert(blocks.end(), released.begin(), released.end());
            }
        }
        for (const GuestAddr b : blocks) free_guest(b);
        return made;
    }
    const auto image = block.resolve(base);
    (void)m.memory().write_bytes(base, reinterpret_cast<const Byte*>(image.data()), image.size());
    made.chain = base + c;
    for (const u32 offset : element_offsets) made.elements.push_back(base + offset);
    made.contexts = element_contexts;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_chains[made.chain] = ChainRecord{base, 1, element_contexts};
    return made;
}

void Crypt32Manager::cert_free_certificate_chain(Marshaler& m) {
    const GuestAddr chain = m.arg_ptr(0);
    if (chain == 0) return;
    std::vector<GuestAddr> blocks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_chains.find(chain);
        if (found == m_chains.end()) return;
        if (--found->second.references != 0) return;
        for (const GuestAddr context : found->second.contexts) {
            auto released = release_cert_context_locked(context);
            blocks.insert(blocks.end(), released.begin(), released.end());
        }
        blocks.push_back(found->second.block);
        m_chains.erase(found);
    }
    for (const GuestAddr block : blocks) free_guest(block);
}

GuestAddr Crypt32Manager::cert_duplicate_certificate_chain(Marshaler& m) {
    const GuestAddr chain = m.arg_ptr(0);
    std::lock_guard<std::mutex> lock(m_mutex);
    auto found = m_chains.find(chain);
    if (found != m_chains.end()) ++found->second.references;
    return chain;
}

// =============================================================================
// CertVerifyCertificateChainPolicy
// =============================================================================

u32 Crypt32Manager::cert_verify_certificate_chain_policy(Marshaler& m) {
    const GuestAddr policy_arg = m.arg_ptr(0);
    const GuestAddr chain = m.arg_ptr(1);
    const GuestAddr policy_para = m.arg_ptr(2);
    const GuestAddr status = m.arg_ptr(3);
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = is64 ? 8u : 4u;
    constexpr u32 kPolicyBase = 1;
    constexpr u32 kPolicyAuthenticode = 2;
    constexpr u32 kPolicySsl = 4;

    // pszPolicyOID is a small integer (CERT_CHAIN_POLICY_*) or an OID string.
    u32 policy = 0;
    if ((policy_arg >> 16) == 0) {
        policy = static_cast<u32>(policy_arg);
    }
    if (policy != kPolicyBase && policy != kPolicyAuthenticode && policy != kPolicySsl) {
        crypt32::report_unimplemented("crypt32!CertVerifyCertificateChainPolicy",
                                      "policy other than BASE, AUTHENTICODE, SSL");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    }
    if (chain == 0 || status == 0) {
        set_last_error(crypt32::err::kEInvalidArg);
        return 0;
    }

    // Walk the guest chain: first simple chain's elements.
    const GuestAddr chains = static_cast<GuestAddr>(m.read_ptr(chain + 16).value_or(0));
    const u32 chain_error = m.read_u32(chain + 4).value_or(0);
    const GuestAddr simple = static_cast<GuestAddr>(m.read_ptr(chains).value_or(0));
    const u32 count = m.read_u32(simple + 12).value_or(0);
    const GuestAddr elements = static_cast<GuestAddr>(m.read_ptr(simple + 16).value_or(0));
    std::vector<u32> errors;
    std::vector<GuestAddr> contexts;
    for (u32 i = 0; i < count && i < 64; ++i) {
        const GuestAddr element =
            static_cast<GuestAddr>(m.read_ptr(elements + static_cast<GuestAddr>(i) * ps).value_or(0));
        errors.push_back(m.read_u32(element + 2 * ps).value_or(0));
        contexts.push_back(static_cast<GuestAddr>(m.read_ptr(element + ps).value_or(0)));
    }

    u32 error = 0;
    i32 chain_index = -1;
    i32 element_index = -1;
    auto first = [&](u32 bit, u32 code) {
        if (error != 0) return;
        for (usize i = 0; i < errors.size(); ++i) {
            if ((errors[i] & bit) != 0) {
                error = code;
                chain_index = 0;
                element_index = static_cast<i32>(i);
                return;
            }
        }
    };
    first(kNotSignatureValid, kTrustECertSignature);
    if (error == 0 && (chain_error & kPartialChain) != 0) {
        error = policy == kPolicySsl ? kCertEUntrustedRoot : kCertEChaining;
        chain_index = 0;
        element_index = -1;
    }
    first(kUntrustedRoot, kCertEUntrustedRoot);
    first(kNotTimeValid, kCertEExpired);
    first(kNotValidForUsage, kCertEWrongUsage);
    first(kIsRevoked, kCryptERevoked);
    if (policy == kPolicyAuthenticode) {
        first(kRevocationUnknown, kCertERevocationFailure);
    } else {
        first(kOfflineRevocation, kCryptERevocationOffline);
        first(kRevocationUnknown, kCryptENoRevocationCheck);
    }

    // SSL: SSL_EXTRA_CERT_CHAIN_POLICY_PARA.pwszServerName against the end
    // certificate's DNS names (subjectAltName, else the subject CN).
    if (error == 0 && policy == kPolicySsl && policy_para != 0) {
        const GuestAddr extra =
            static_cast<GuestAddr>(m.read_ptr(policy_para + 2 * 4).value_or(0));
        if (extra != 0) {
            // { cbSize (union); dwAuthType; fdwChecks; pwszServerName }
            const GuestAddr name_ptr =
                static_cast<GuestAddr>(m.read_ptr(extra + 12 + (is64 ? 4u : 0u)).value_or(0));
            const u32 checks = m.read_u32(extra + 8).value_or(0);
            constexpr u32 kIgnoreCnInvalid = 0x00001000u;  // SECURITY_FLAG_IGNORE_CERT_CN_INVALID
            if (name_ptr != 0 && (checks & kIgnoreCnInvalid) == 0) {
                const auto wanted = m.read_wide_string(name_ptr, 1024);
                std::vector<std::string> names;
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    auto leaf = m_cert_contexts.find(contexts.empty() ? 0 : contexts[0]);
                    if (leaf != m_cert_contexts.end()) {
                        auto parsed = x509::parse_certificate(leaf->second.der_data);
                        if (parsed) {
                            for (const auto& extension : parsed->extensions) {
                                if (extension.oid == "2.5.29.17") {
                                    names = x509::decode_subject_alt_dns_names(extension.value);
                                }
                            }
                            if (names.empty()) {
                                for (const auto& attribute : parsed->subject_attributes) {
                                    if (attribute.oid == "2.5.4.3") names.push_back(attribute.value);
                                }
                            }
                        }
                    }
                }
                auto lower = [](std::string text) {
                    for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                    return text;
                };
                const std::string host = wanted.is_ok() ? lower(wanted.value()) : std::string{};
                bool matched = false;
                for (const auto& raw : names) {
                    const std::string pattern = lower(raw);
                    if (!host.empty() && pattern == host) matched = true;
                    if (!host.empty() && pattern.size() > 2 && pattern[0] == '*' &&
                        pattern[1] == '.') {
                        const usize dot = host.find('.');
                        if (dot != std::string::npos && dot > 0 &&
                            host.substr(dot) == pattern.substr(1)) {
                            matched = true;
                        }
                    }
                }
                if (!matched) {
                    error = kCertECnNoMatch;
                    chain_index = 0;
                    element_index = 0;
                }
            }
        }
    }

    // CERT_CHAIN_POLICY_STATUS { cbSize; dwError; lChainIndex; lElementIndex;
    // pvExtraPolicyStatus } — pvExtraPolicyStatus is not written.
    (void)m.write_u32(status + 4, error);
    (void)m.write_u32(status + 8, static_cast<u32>(chain_index));
    (void)m.write_u32(status + 12, static_cast<u32>(element_index));
    return 1;
}

} // namespace vitreo::winapi

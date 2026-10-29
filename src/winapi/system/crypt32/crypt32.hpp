// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file crypt32.hpp
/// @brief CRYPT32.dll API and capsule-backed certificate stores
///
/// Windows certificate contexts and named current-user stores are owned by the
/// guest runtime. Persistent store records live in the capsule VFS; host
/// Security.framework APIs are used only as a verification delegate.

#include "winapi/system/crypt32/auth_root.hpp"
#include <vitreo/common.hpp>
#include "authenticode.hpp"
#include "x509_parse.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>

namespace vitreo::winapi {

class Marshaler;
class GlobalLocalManager;

} // namespace vitreo::winapi

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

/// Guest address range reserved for Crypt32 scratch allocations.
///
/// These fixtures must not overlap each other. ws2_32 reserves a whole
/// megabyte at 0x5E000000, and this base used to be 0x5E010000 — *inside* it —
/// so the reservation simply failed and crypt32 then used memory ws2_32 owned.
/// opengl32 (0x5E060000) and glu32 (0x5E061000) had the same problem. All
/// three now live in the free gap between the gdi32 DIB pool (which ends at
/// 0x5ED00000) and ole32 (0x5EF00000). See the overlap test in
/// tests/winapi/test_scratch_fixtures.cpp, which fails if any two collide.
static constexpr GuestAddr CRYPT32_SCRATCH_BASE = 0x5ED00000;
static constexpr u32       CRYPT32_SCRATCH_SIZE = 0x10000; // 64 KB

// =============================================================================
// Crypt32Manager
// =============================================================================

class Crypt32Manager {
public:
    Crypt32Manager() = default;
    ~Crypt32Manager() = default;

    Crypt32Manager(const Crypt32Manager&) = delete;
    Crypt32Manager& operator=(const Crypt32Manager&) = delete;

    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }
    /// Windows' automatic root certificate update (auth_root.cpp). On in a
    /// running process (the runtime enables it); off by default so a host
    /// that builds chains without a capsule -- unit tests -- never reaches
    /// the network.
    void set_root_auto_update(bool enabled) noexcept { m_auth_root_enabled = enabled; }

    /// LocalAlloc/LocalFree backend for CryptDecodeObjectEx(CRYPT_DECODE_ALLOC_FLAG)
    /// and Schannel REMOTE_CERT_CONTEXT certificate contexts.
    void set_local_memory(GlobalLocalManager* local) noexcept {
        m_local_memory = local;
    }

    /// Prefer this owner-only key file for DPAPI before consulting Keychain.
    /// Capsule runs set this to `<capsule>/dpapi-master.key` so rebuilt
    /// binaries do not re-prompt Keychain for `vitreo.dpapi.master`.
    /// `VITREO_DPAPI_KEY_FILE` still overrides when set.
    void set_dpapi_key_file(std::string path) { m_dpapi_key_file = std::move(path); }

    // ---- Scratch allocator (guest memory bump pointer) ----

    /// Allocate `size` bytes from the scratch region (4-byte aligned).
    [[nodiscard]] GuestAddr scratch_alloc(u32 size) noexcept;

    /// Reset scratch allocator (e.g. between SSL attempts).
    void scratch_reset() noexcept { m_scratch_offset = 0; }

    // ---- DPAPI via CommonCrypto and Keychain/dev key-file storage ----
    [[nodiscard]] u32 crypt_protect_data(Marshaler& m);
    [[nodiscard]] u32 crypt_unprotect_data(Marshaler& m);
    /// CryptProtectMemory (protect) / CryptUnprotectMemory (!protect).
    [[nodiscard]] u32 crypt_protect_memory(Marshaler& m, bool protect);
    [[nodiscard]] u32 crypt_string_to_binary_a(Marshaler& m);
    [[nodiscard]] u32 crypt_string_to_binary_w(Marshaler& m);
    [[nodiscard]] u32 crypt_binary_to_string_a(Marshaler& m);
    [[nodiscard]] u32 crypt_decode_object_ex(Marshaler& m);
    /// CryptDecodeObject: CryptDecodeObjectEx with no CRYPT_DECODE_PARA.
    [[nodiscard]] u32 crypt_decode_object(Marshaler& m);
    /// The decoder behind both, over explicit arguments.
    [[nodiscard]] u32 crypt_decode_object_core(Marshaler& m, u32 encoding, GuestAddr struct_type,
                                               GuestAddr encoded, u32 encoded_len, u32 flags,
                                               GuestAddr decode_para, GuestAddr struct_info,
                                               GuestAddr size_ptr);
    [[nodiscard]] u32 crypt_decode_object_body(Marshaler& m, u32 encoding, GuestAddr struct_type,
                                               GuestAddr encoded, u32 encoded_len, u32 flags,
                                               GuestAddr decode_para, GuestAddr struct_info,
                                               GuestAddr size_ptr);
    /// CryptHashPublicKeyInfo (crypt_msg_signer.cpp).
    [[nodiscard]] u32 crypt_hash_public_key_info(Marshaler& m);
    /// CryptMsgGetAndVerifySigner (crypt_msg_signer.cpp).
    [[nodiscard]] u32 crypt_msg_get_and_verify_signer(Marshaler& m);
    [[nodiscard]] u32 crypt_encode_object_ex();
    [[nodiscard]] u32 pfx_import_cert_store();
    [[nodiscard]] u32 crypt_hash_certificate2(Marshaler& m);
    [[nodiscard]] u32 cert_control_store(Marshaler& m);
    [[nodiscard]] u32 crypt_acquire_certificate_private_key(Marshaler& m);
    /// CertFindExtension — returns guest address of matching CERT_EXTENSION.
    [[nodiscard]] GuestAddr cert_find_extension(Marshaler& m);

    /// Materialize a CERT_CONTEXT from host DER into guest memory (LocalAlloc
    /// when available). Used by Schannel SECPKG_ATTR_REMOTE_CERT_CONTEXT.
    [[nodiscard]] GuestAddr create_cert_context_from_der(
        Marshaler& m, std::span<const u8> der, u32 store_handle = 0,
        u32 encoding_type = 0x00010001u,
        std::string persistent_record = {},
        usize store_index = std::numeric_limits<usize>::max());

    /// A CERT_CONTEXT in a store of its own holding just this certificate,
    /// as CertCreateCertificateContext returns (last error 0 on success).
    [[nodiscard]] GuestAddr create_owned_cert_context(Marshaler& m, std::span<const u8> der,
                                                      const x509::ParsedCertificate& parsed,
                                                      u32 cert_encoding);

    /// Materialize Schannel's remote end-certificate context and its owned,
    /// transient intermediate-certificate store.
    [[nodiscard]] GuestAddr create_remote_cert_context(
        Marshaler& m,
        std::span<const std::vector<u8>> peer_chain_der);

    // ---- Certificate stores ----
    [[nodiscard]] u32 cert_open_store();
    [[nodiscard]] u32 cert_open_store(Marshaler& m);
    [[nodiscard]] u32 cert_open_system_store_w(Marshaler& m);
    [[nodiscard]] u32 cert_open_system_store_a(Marshaler& m);
    /// Shared by both CertOpenSystemStore forms: the store name is the only
    /// thing that differs between them.
    [[nodiscard]] u32 open_system_store_by_name(const std::string& raw_name);
    [[nodiscard]] u32 cert_close_store(u32 store_handle, u32 flags);
    [[nodiscard]] u32 cert_add_store_to_collection(Marshaler& m);

    // ---- Certificate contexts ----
    [[nodiscard]] GuestAddr cert_create_certificate_context(Marshaler& m);
    [[nodiscard]] u32 cert_free_certificate_context(Marshaler& m);
    [[nodiscard]] GuestAddr cert_duplicate_certificate_context(Marshaler& m);
    [[nodiscard]] GuestAddr cert_enum_certificates_in_store(Marshaler& m);
    // Extended properties (cert_properties.cpp).
    [[nodiscard]] u32 cert_get_certificate_context_property(Marshaler& m);
    [[nodiscard]] u32 cert_set_certificate_context_property(Marshaler& m);
    [[nodiscard]] u32 cert_get_enhanced_key_usage(Marshaler& m);
    [[nodiscard]] u32 cert_set_enhanced_key_usage(Marshaler& m);
    /// CertGetIssuerCertificateFromStore — the next certificate in the store
    /// whose Subject matches the subject certificate's Issuer. A self-signed
    /// subject yields NULL with CRYPT_E_SELF_SIGNED.
    [[nodiscard]] GuestAddr cert_get_issuer_certificate_from_store(Marshaler& m);
    [[nodiscard]] u32 cert_delete_certificate_from_store(Marshaler& m);
    [[nodiscard]] u32 cert_find_certificate_in_store();
    [[nodiscard]] GuestAddr cert_find_certificate_in_query_store(Marshaler& m);
    [[nodiscard]] u32 cert_add_encoded_certificate_to_store(Marshaler& m);
    [[nodiscard]] u32 cert_add_certificate_context_to_store(Marshaler& m);
    /// CertGetNameStringA/W (cert_find_names.cpp).
    [[nodiscard]] u32 cert_get_name_string(Marshaler& m, bool wide);
    [[nodiscard]] u32 cert_name_to_str_a(Marshaler& m);
    [[nodiscard]] u32 cert_name_to_str_w(Marshaler& m);
    /// Free a CryptMemAlloc / CRYPT_DECODE_ALLOC_FLAG buffer (LocalAlloc family).
    void free_guest(GuestAddr addr) noexcept;

    // ---- Authenticode module verification (wintrust.dll) ----
    // The recent x64 bootstrap self-verifies its own module's Authenticode
    // signature via WinVerifyTrust then walks the provider chain to read the
    // signer certificate's SHA1 thumbprint. These implement that contract over
    // the PE's embedded PKCS#7; the WTHelper* accessors return the built
    // provider chain.
    [[nodiscard]] u32       win_verify_trust(Marshaler& m);
    [[nodiscard]] GuestAddr wt_helper_prov_data_from_state_data(Marshaler& m);
    [[nodiscard]] GuestAddr wt_helper_get_prov_signer_from_chain(Marshaler& m);
    [[nodiscard]] GuestAddr wt_helper_get_prov_cert_from_chain(Marshaler& m);

    // ---- Signed-object queries ----
    [[nodiscard]] u32 crypt_query_object(Marshaler& m);

    /// CryptVerifyCertificateSignatureEx — checks the subject's signature
    /// against the issuer's public key via the host Security framework.
    [[nodiscard]] u32 crypt_verify_certificate_signature_ex(Marshaler& m);

    // ---- OID function sets ----
    /// CryptInitOIDFunctionSet — interned handle for one function name.
    [[nodiscard]] u32 crypt_init_oid_function_set(Marshaler& m);
    /// CryptGetOIDFunctionAddress — resolve an OID against the entries the
    /// guest installed with CryptInstallOIDFunctionAddress.
    [[nodiscard]] u32 crypt_get_oid_function_address(Marshaler& m);
    [[nodiscard]] u32 crypt_msg_get_param(Marshaler& m);
    [[nodiscard]] u32 crypt_msg_close(Marshaler& m);

    // ---- Certificate chain verification (cert_chain.cpp) ----
    [[nodiscard]] u32 cert_get_certificate_chain(Marshaler& m);
    /// CertFreeCertificateChain(PCCERT_CHAIN_CONTEXT).
    void cert_free_certificate_chain(Marshaler& m);
    /// Host-side callers that hold no chain reference (not a guest export).
    void cert_free_certificate_chain() {}
    [[nodiscard]] GuestAddr cert_duplicate_certificate_chain(Marshaler& m);
    [[nodiscard]] u32 cert_verify_certificate_chain_policy(Marshaler& m);

    /// HCERTCHAINENGINE create/free. Callers create a custom engine before
    /// CertGetCertificateChain. A missing-export stub returned 0xFFFFFFFF with
    /// zero stdcall cleanup and corrupted the x86 stack (AV near 0x2a).
    [[nodiscard]] u32 cert_create_certificate_chain_engine(Marshaler& m);
    void cert_free_certificate_chain_engine(Marshaler& m);

    /// Register OID function entries. Vitreo does not dispatch installed OID
    /// callbacks yet; returning TRUE with correct stdcall arity unblocks
    /// crypt32 init that previously hit the missing-export stub.
    [[nodiscard]] u32 crypt_install_oid_function_address(Marshaler& m);

    /// Host-side certificate verification via macOS Security.framework (SecTrust).
    /// Returns 0 (CERT_TRUST_NO_ERROR) if trusted, or a CERT_TRUST error flag.
    /// @param leaf_der   DER-encoded leaf certificate
    /// @param chain_ders DER-encoded intermediate certificates
    [[nodiscard]] u32 host_verify_cert_chain(
        const std::vector<u8>& leaf_der,
        const std::vector<std::vector<u8>>& chain_ders,
        const std::vector<std::vector<u8>>& trust_anchors);

private:
    std::mutex m_mutex;

    // DPAPI master key, lazy-loaded from macOS Keychain or the explicitly
    // configured owner-only development key file.
    std::vector<u8> m_dpapi_key;
    std::vector<u8> m_process_memory_key;  // CRYPTPROTECTMEMORY_SAME_PROCESS
    std::once_flag m_hash_certificate2_once;  // first CryptHashCertificate2 clears LE
    std::string m_dpapi_key_file;
    bool ensure_dpapi_key();

    // Scratch bump allocator for guest-side structures.
    u32 m_scratch_offset = 0;

    // Handle counters.
    u32 m_next_store_handle = 0xCE580100;
    u32 m_next_chain_engine = 0xCE5A0100;
    /// HCERTCHAINENGINE state (CERT_CHAIN_ENGINE_CONFIG). Store handles the
    /// engine names stay usable after the caller closes them (Windows
    /// duplicates them), via m_store_holds.
    struct ChainEngine {
        u32 restricted_root{0};
        std::vector<u32> additional_stores;
        u32 exclusive_root{0};
        u32 flags{0};
    };
    std::unordered_map<u32, ChainEngine> m_chain_engines;
    std::unordered_map<u32, u32> m_store_holds;
    std::unordered_set<u32> m_stores_closed_while_held;
    void hold_store_locked(u32 store);
    void release_store_hold_locked(u32 store);
    /// A certificate context taken from @p store is still referenced.
    [[nodiscard]] bool store_has_live_contexts_locked(u32 store) const;
    /// Erase a store whose handle was closed once nothing holds it any more
    /// (no chain engine, no live certificate context).
    void finish_closed_store_locked(u32 store);
    /// Forget every map entry for a store handle (the real close).
    void erase_store_locked(u32 store);
    /// A CERT_CHAIN_CONTEXT handed to the guest: its one allocation, its
    /// reference count, and the element certificate contexts it holds.
    struct ChainRecord {
        GuestAddr block{0};
        u32 references{1};
        std::vector<GuestAddr> contexts;
    };
    std::unordered_map<GuestAddr, ChainRecord> m_chains;

    /// Host-side chain building shared by CertGetCertificateChain and
    /// WinVerifyTrust (cert_chain.cpp).
    struct ChainRequest {
        u32 engine{0};                       // HCERTCHAINENGINE / HCCE_*
        u32 additional_store{0};
        std::vector<std::vector<u8>> extra_certificates;
        u64 when{0};                         // FILETIME
        u32 usage_type{0};                   // USAGE_MATCH_TYPE_AND / _OR
        std::vector<std::string> requested_usage;
        u32 flags{0};                        // CertGetCertificateChain dwFlags
    };
    struct ChainResult {
        struct Element {
            std::vector<u8> der;
            u32 error{0};
            u32 info{0};
            std::optional<std::vector<std::string>> app;
            std::optional<std::vector<std::string>> issuance;
            bool revocation_present{false};
            u32 revocation_result{0};
        };
        std::vector<Element> elements;
        u32 error{0};
        u32 info{0};
        bool complete{false};
        bool trusted{false};
        u32 flags{0};
        u32 last_error_after{0x80092004u};
    };
    /// 0 on success, else the error to report. Caller holds m_mutex.
    [[nodiscard]] u32 build_chain_once_locked(const ChainRequest& request,
                                              const std::vector<u8>& leaf_der,
                                              const std::optional<std::vector<u8>>& leaf_eku,
                                              ChainResult& out);
    [[nodiscard]] u32 build_chain_locked(const ChainRequest& request,
                                         const std::vector<u8>& leaf_der,
                                         const std::optional<std::vector<u8>>& leaf_eku,
                                         ChainResult& out);
    struct MaterializedChain {
        GuestAddr chain{0};
        std::vector<GuestAddr> elements;   // CERT_CHAIN_ELEMENT addresses
        std::vector<GuestAddr> contexts;   // their certificate contexts
    };
    /// Lay the chain out in guest memory and register it (one reference).
    /// `leaf_context` (a tracked context, or 0) becomes element 0.
    [[nodiscard]] MaterializedChain materialize_chain(
        Marshaler& m, const ChainResult& result, GuestAddr leaf_context);

public:
    /// Extended properties of one certificate element (Windows keeps them
    /// on the store element, so every context of it sees the same set).
    struct KeyProvInfo {
        bool has_container{false};
        std::u16string container;
        bool has_provider{false};
        std::u16string provider;
        u32 prov_type{0};
        u32 flags{0};
        u32 key_spec{0};
        struct Param {
            u32 param{0};
            u32 flags{0};
            std::vector<u8> data;
        };
        std::vector<Param> params;
    };
    struct CertProperties {
        std::map<u32, std::vector<u8>> blobs;  // CRYPT_DATA_BLOB-valued ids
        std::optional<KeyProvInfo> key_prov_info;
        std::optional<u32> key_context_spec;   // CERT_KEY_CONTEXT, hCryptProv 0
    };

private:
    /// Store handle -> element DER -> its properties.
    std::unordered_map<u32, std::map<std::vector<u8>, std::shared_ptr<CertProperties>>>
        m_element_properties;
    /// Shared CertAdd*ToStore implementation (all dispositions).
    [[nodiscard]] u32 store_add_certificate(Marshaler& m, u32 store_handle,
                                            const std::vector<u8>& der, u32 disposition,
                                            std::shared_ptr<CertProperties> source_props,
                                            GuestAddr out_ptr);
    /// The property set of (store, der), created on first use. Caller holds m_mutex.
    [[nodiscard]] std::shared_ptr<CertProperties> element_properties_locked(
        u32 store, const std::vector<u8>& der);

    // Host-side tracking of cert data keyed by guest CERT_CONTEXT pointer.
    struct CertEntry {
        std::vector<u8> der_data;
        GuestAddr guest_cert_context; // guest-side CERT_CONTEXT address
        std::string common_name;
        std::string issuer_name;
        std::vector<x509::NameAttribute> subject_attributes;
        std::vector<x509::NameAttribute> issuer_attributes;
        // A context returned from a memory store keeps that store's
        // certificate material alive after CertCloseStore. The handle is
        // closed, but Windows chain building can still use the context's
        // originating store to find issuers.
        std::vector<std::vector<u8>> originating_store_certificates;
        u32 store_handle{0};
        usize store_index{std::numeric_limits<usize>::max()};
        std::string persistent_record;
        // CertCreateCertificateContext owns every guest allocation comprising
        // the context until the final CertFreeCertificateContext. Other
        // producers may leave this empty when their storage has a wider owner.
        std::vector<GuestAddr> owned_allocations;
        bool owns_store_handle{false};
        u32 reference_count{1};
        // Extended properties (CertSet/GetCertificateContextProperty),
        // shared by every context of the same store element.
        std::shared_ptr<CertProperties> props{std::make_shared<CertProperties>()};
    };
    /// Lay out CERT_CONTEXT + CERT_INFO and every blob it points at in one
    /// guest allocation (Windows' own layout rules: little-endian serial,
    /// decoded algorithm identifiers, extensions). Returns the context
    /// address, or 0 when the allocation fails. Does not register the entry.
    [[nodiscard]] GuestAddr build_cert_context_block(
        Marshaler& m, std::span<const u8> der,
        const x509::ParsedCertificate& parsed, u32 encoding_type,
        u32 store_handle);
    [[nodiscard]] static CertEntry make_cert_entry(
        std::span<const u8> der, const x509::ParsedCertificate& parsed,
        GuestAddr context);
    std::unordered_map<GuestAddr, CertEntry> m_cert_contexts;
    /// Consume one Windows certificate-context reference. Caller holds
    /// m_mutex. Context-owned guest blocks are released at reference zero.
    [[nodiscard]] std::vector<GuestAddr> release_cert_context_locked(
        GuestAddr context);

    // Per-store tracking of DER certs added via CertAddCertificateContextToStore.
    // Maps store handle → list of DER cert blobs.
    std::unordered_map<u32, std::vector<std::vector<u8>>> m_store_certs;
    std::unordered_map<u32, std::string> m_system_store_names;
    std::unordered_map<u32, bool> m_system_store_read_only;
    // CERT_STORE_PROV_COLLECTION: sibling stores linked by CertAddStoreToCollection.
    // Enumeration spans siblings in descending priority; adds go to the first
    // sibling opened with CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG.
    struct CollectionSibling {
        u32 store_handle{0};
        // A collection retains the opened system-store provider after the
        // caller closes its original handle. Keep its stable logical identity
        // rather than relying on that handle remaining in the open-handle map.
        std::string system_store_name;
        u32 update_flags{0};
        u32 priority{0};
    };
    std::unordered_map<u32, std::vector<CollectionSibling>> m_collection_stores;
    struct StoredCertificate {
        std::vector<u8> der_data;
        std::string record_name;
    };
    std::unordered_map<std::string, std::vector<StoredCertificate>>
        m_system_store_certs;

    /// Every certificate in one store, in enumeration order (memory store,
    /// system store, or collection). Caller holds m_mutex.
    [[nodiscard]] std::vector<std::vector<u8>> store_certificates_locked(
        u32 store_handle);
    [[nodiscard]] bool load_system_store_locked(const std::string& name);
    /// Merge host trust anchors into LocalMachine\ROOT without replacing or
    /// duplicating roots installed by the guest.
    [[nodiscard]] bool seed_local_machine_root_from_host_locked(
        const std::string& name);
    [[nodiscard]] bool append_system_record_locked(
        const std::string& name, std::span<const u8> der,
        std::string& record_name);
    [[nodiscard]] bool overwrite_system_record_locked(
        const std::string& name, const std::string& record_name,
        std::span<const u8> der);
    [[nodiscard]] bool remove_system_record_locked(
        const std::string& name, const std::string& record_name);

    // OID function registry. CryptInstallOIDFunctionAddress registers guest
    // callbacks per (function name, encoding type, OID); CryptGetOIDFunctionAddress
    // hands the matching one back. Key: "<name>\0<encoding>\0<oid>".
    [[nodiscard]] static std::string oid_function_key(
        const std::string& func_name, u32 encoding, const std::string& oid);
    std::unordered_map<std::string, GuestAddr> m_oid_functions;
    std::unordered_map<std::string, u32> m_oid_func_set_handles;
    std::unordered_map<u32, std::string> m_oid_func_set_names;
    u32 m_next_oid_func_set{0x0F5E0100};

    /// A decoded signed message (the HCRYPTMSG of CryptQueryObject and of
    /// WinVerifyTrust's provider data). Independent of the store opened
    /// beside it: closing either leaves the other usable (authenticode_chain_probe).
    struct QueryMessage {
        std::vector<authenticode::Certificate> certificates;
        std::vector<authenticode::SignerInfo> signers;  // for CryptMsgGetParam
        std::string content_type;
        std::vector<u8> content;
        std::vector<u8> content_value;  ///< eContent's contents octets (what signers digest)
    };

    /// Make store_handle a memory store holding a signed message's
    /// certificates. Windows fills it as a memory store, one add per
    /// certificate in message order, so it enumerates them newest-first: in
    /// reverse (authenticode_chain_probe). Caller holds m_mutex.
    void fill_message_store_locked(
        u32 store_handle, const std::vector<authenticode::Certificate>& certificates);

    // WinVerifyTrust per-verification state. WTD_STATEACTION_VERIFY builds a
    // provider chain in guest memory and keys it by the handle written to
    // WINTRUST_DATA.hWVTStateData; the WTHelper* accessors map the pointers we
    // hand back; WTD_STATEACTION_CLOSE frees it.
    struct WvtState {
        GuestAddr block{0};                // provider data allocation
        GuestAddr prov_data{0};
        GuestAddr chain{0};                // CERT_CHAIN_CONTEXT reference
        GuestAddr timestamp_chain{0};      // the counter-signer's chain reference
        std::vector<GuestAddr> contexts;   // certificate context references
        u32 message{0};                    // HCRYPTMSG for hMsg
        u32 store{0};
    };
    u32 m_next_wvt_state{0xC7570100};
    std::unordered_map<u32, WvtState> m_wvt_states;

    capsule::Vfs* m_vfs{nullptr};
    GlobalLocalManager* m_local_memory{nullptr};

    // ---- automatic root update (auth_root.cpp) --------------------------------
    /// Microsoft's verified root list, fetched once per process; null when it
    /// could not be fetched or verified. Caller holds m_mutex.
    [[nodiscard]] const std::vector<crypt32::AuthRootEntry>* auth_root_list_locked();
    /// Install the listed root a chain ending at @p top_der lacks: the root
    /// itself when @p self_signed, else the issuer of @p top_der. True when a
    /// root was added to LocalMachine\ROOT. Caller holds m_mutex.
    [[nodiscard]] bool auto_update_root_locked(const std::vector<u8>& top_der, bool self_signed);
    /// The list entry an auto-updated root was installed with (its usage
    /// restrictions), kept beside the store; nullopt for any other root.
    [[nodiscard]] std::optional<crypt32::AuthRootEntry> root_restriction_locked(const std::vector<u8>& root_der);
    bool m_auth_root_enabled{false};
    int m_auth_root_state{0};  ///< 0 not fetched, 1 loaded, 2 unavailable
    std::vector<crypt32::AuthRootEntry> m_auth_roots;
    std::unordered_map<std::string, std::optional<crypt32::AuthRootEntry>> m_root_restrictions;
    u32 m_next_message_handle{0xCE590100};
    std::unordered_map<u32, QueryMessage> m_query_messages;

    [[nodiscard]] GuestAddr alloc_guest(u32 size) noexcept;
};

/// Create CRYPT32.dll export thunks
DllExports create_crypt32_thunks(Crypt32Manager& mgr);

} // namespace vitreo::winapi

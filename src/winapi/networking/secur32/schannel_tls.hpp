// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file schannel_tls.hpp
/// @brief Host-side TLS client engine backing Vitreo's Schannel SSPI.
///
/// Callers that use libcurl's schannel backend drive TLS through SSPI: they
/// own the TCP socket via ws2_32 and
/// exchange raw handshake/record bytes with the security package via SecBuffers.
/// `InitializeSecurityContext` produces the ClientHello and consumes server
/// flights; `EncryptMessage`/`DecryptMessage` are the record layer.
///
/// This engine implements exactly that byte-in / byte-out contract on top of
/// macOS Secure Transport (`SSLContextRef`), whose I/O-callback model is a
/// natural fit: the guest's socket bytes are fed through a memory buffer instead
/// of a real fd. No guest types appear here — the SSPI marshaling lives in
/// secur32.cpp; this layer is pure host C++.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace vitreo::winapi::schannel {

/// One TLS client session. Not thread-safe; callers serialize per session
/// (SSPI contexts are used from a single guest thread at a time).
class TlsSession {
public:
    ~TlsSession();
    TlsSession(const TlsSession&) = delete;
    TlsSession& operator=(const TlsSession&) = delete;

    /// Create a client session for `host` (used for SNI and, when verifying,
    /// certificate hostname validation). `verify_peer` false disables trust
    /// evaluation (SCH_CRED_MANUAL_CRED_VALIDATION / ISC_REQ_MANUAL_CRED_
    /// VALIDATION). `check_server_name` false keeps chain validation but skips
    /// the host-name match (SCH_CRED_NO_SERVERNAME_CHECK). Returns null on
    /// allocation failure.
    static TlsSession* create(std::string host, bool verify_peer,
                              bool check_server_name = true);

    enum class Step {
        Continue,        ///< Produced a token to send and/or consumed input; call again.
        Complete,        ///< Handshake finished successfully.
        NeedMoreInput,   ///< Not even one whole record available; nothing consumed.
        Fatal,           ///< Handshake failed.
    };

    /// Handshake finished (SEC_E_OK was returned). Schannel rejects the record
    /// layer and SECPKG_ATTR_STREAM_SIZES on a context before this point.
    bool complete() const noexcept { return m_complete; }

    /// Why the last handshake step returned Step::Fatal.
    enum class Failure {
        None,
        PeerAlert,        ///< The server sent a fatal alert.
        Protocol,         ///< Malformed or unexpected handshake message.
        Negotiation,      ///< No common protocol version / cipher suite.
        UntrustedRoot,    ///< Chain does not end in a trusted root.
        CertExpired,      ///< A chain certificate is expired / not yet valid.
        WrongPrincipal,   ///< Leaf does not match the target host name.
        Other,
    };
    Failure last_failure() const noexcept { return m_last_failure; }

    /// Server leaf certificate NotAfter as a UTC FILETIME (100 ns since 1601),
    /// or 0 while no certificate has been received. Schannel reports it as the
    /// context expiry from the leg that processed the server Certificate.
    uint64_t peer_not_after_filetime() const noexcept { return m_peer_not_after; }

    /// Advance the handshake.
    /// @param in         Server bytes available (may be null/0 on the first call).
    /// @param in_len     Length of `in`.
    /// @param consumed   Out: how many leading bytes of `in` were consumed. The
    ///                   caller must re-present `in[consumed..]` next time
    ///                   (SSPI SECBUFFER_EXTRA). Always 0 when NeedMoreInput.
    /// @param out        Out: bytes to transmit to the server (the handshake
    ///                   token), empty if none.
    Step handshake(const uint8_t* in, size_t in_len, size_t& consumed,
                   std::vector<uint8_t>& out);

    /// Encrypt one application-data message into a single TLS record.
    bool encrypt(const uint8_t* plaintext, size_t len,
                 std::vector<uint8_t>& record);

    enum class DecStep {
        Ok,              ///< Produced plaintext (possibly empty) and consumed input.
        NeedMoreInput,   ///< Incomplete record; nothing consumed.
        Closed,          ///< Peer sent close_notify.
        Fatal,           ///< Decrypt failed.
    };

    /// Decrypt the first TLS record of `in` (Schannel processes exactly one
    /// record per DecryptMessage call and reports the rest as SECBUFFER_EXTRA).
    /// @param consumed  Out: leading bytes of `in` consumed; re-present the rest.
    DecStep decrypt(const uint8_t* in, size_t in_len, size_t& consumed,
                    std::vector<uint8_t>& plaintext);

    /// Mark the context for Schannel-style outbound shutdown. The next
    /// InitializeSecurityContext call emits the close_notify token.
    void request_shutdown() noexcept { m_shutdown_pending = true; }

    /// Emit a queued TLS close_notify alert, if one was requested.
    bool emit_shutdown(std::vector<uint8_t>& out);

    bool shutdown_pending() const noexcept { return m_shutdown_pending; }

    /// Record-layer sizes for SECPKG_ATTR_STREAM_SIZES, valid after the
    /// handshake completes (conservative upper bounds before then).
    size_t header_size() const noexcept { return m_header_size; }
    size_t trailer_size() const noexcept { return m_trailer_size; }
    size_t max_message() const noexcept { return m_max_message; }

    /// DER-encoded leaf certificate from the peer, captured when the handshake
    /// completes. Empty if unavailable. Used for SECPKG_ATTR_REMOTE_CERT_CONTEXT.
    const std::vector<uint8_t>& peer_certificate_der() const noexcept {
        return m_peer_cert_der;
    }

    /// DER-encoded peer chain in leaf-first order. Schannel exposes the
    /// intermediates through CERT_CONTEXT::hCertStore when callers query
    /// SECPKG_ATTR_REMOTE_CERT_CONTEXT.
    const std::vector<std::vector<uint8_t>>&
    peer_certificate_chain_der() const noexcept {
        return m_peer_cert_chain_der;
    }

private:
    void capture_peer_certificate();
    TlsSession() = default;

    // Secure Transport I/O callbacks pull from / push to these members.
    friend int32_t schannel_st_read(void* conn, void* data, size_t* len);
    friend int32_t schannel_st_write(void* conn, const void* data, size_t* len);

    void* m_ctx = nullptr;      ///< SSLContextRef (opaque here to keep header pure C++).
    std::string m_host;
    bool m_verify_peer = true;
    bool m_check_server_name = true;
    bool m_complete = false;
    Failure m_last_failure = Failure::None;
    uint64_t m_peer_not_after = 0;
    bool m_shutdown_pending = false;
    bool m_shutdown_sent = false;

    // Per-call handshake input window (complete records only) and consume cursor.
    const uint8_t* m_in_ptr = nullptr;
    size_t m_in_len = 0;
    size_t m_in_pos = 0;
    // Bytes Secure Transport wants transmitted, accumulated by the write cb.
    std::vector<uint8_t> m_out;

    // The suite is pinned to TLS 1.2 AES-GCM (see schannel_tls.mm), whose record
    // is a 5-byte header + 8-byte explicit nonce, ciphertext == plaintext, and a
    // 16-byte tag. Windows reports exactly these for TLS 1.2 AES-256-GCM
    // (sspi_schannel_stream_probe.expected: header=13 trailer=16 max=16384).
    size_t m_header_size = 13;
    size_t m_trailer_size = 16;
    size_t m_max_message = 16384;
    std::vector<uint8_t> m_peer_cert_der;
    std::vector<std::vector<uint8_t>> m_peer_cert_chain_der;
};

// ---------------------------------------------------------------------------
// Handle tables. Credentials and contexts are addressed by opaque u32 ids so
// they fit a 32-bit guest CredHandle/CtxtHandle (dwLower); dwUpper is unused.
// ---------------------------------------------------------------------------

/// What AcquireCredentialsHandle recorded for a Schannel credential.
struct CredentialConfig {
    bool outbound = true;           ///< SECPKG_CRED_OUTBOUND was requested.
    bool inbound = false;           ///< SECPKG_CRED_INBOUND was requested.
    bool verify_peer = true;        ///< !SCH_CRED_MANUAL_CRED_VALIDATION
    bool check_server_name = true;  ///< !SCH_CRED_NO_SERVERNAME_CHECK
};

/// Register a credential. Returns a nonzero id.
uint32_t create_credential(const CredentialConfig& config);
bool get_credential(uint32_t cred, CredentialConfig& out);
bool credential_verify_peer(uint32_t cred);
/// False when `cred` is not a live credential (SEC_E_INVALID_HANDLE).
bool free_credential(uint32_t cred);

/// Create a context/session bound to `host`. Returns a nonzero id, or 0 on
/// failure.
uint32_t create_context(const CredentialConfig& config, std::string host);
TlsSession* get_context(uint32_t ctx);
/// False when `ctx` is not a live context (SEC_E_INVALID_HANDLE).
bool free_context(uint32_t ctx);

} // namespace vitreo::winapi::schannel

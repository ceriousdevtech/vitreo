// SPDX-License-Identifier: LGPL-2.1-only
/// @file schannel_tls.mm
/// @brief Schannel SSPI backed by macOS Secure Transport.
///
/// See schannel_tls.hpp for the contract. The guest owns the TCP socket; this
/// engine only transforms bytes: handshake tokens and TLS records in and out of
/// an SSLContextRef whose I/O callbacks read/write in-memory buffers instead of
/// a file descriptor.
///
/// Deliberate scope limit: the negotiated suite is pinned to TLS 1.2 AEAD-GCM.
/// That makes record framing deterministic (5-byte header + 8-byte explicit
/// nonce, ciphertext length == plaintext length, 16-byte tag), which is what
/// lets EncryptMessage lay ciphertext back into the caller's fixed
/// header/data/trailer SecBuffers without overflow. A server that offers
/// TLS 1.2 GCM succeeds. A server that does not fails the handshake honestly
/// rather than silently mis-framing records.

#include "schannel_tls.hpp"

#ifdef __APPLE__

// Secure Transport is deprecated in favour of Network.framework but remains the
// only macOS TLS API with a synchronous memory-BIO shape, which is exactly what
// the SSPI buffer-in/buffer-out contract needs. Silence the deprecation noise.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#include <Security/Security.h>
#include <Security/SecureTransport.h>

#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace vitreo::winapi::schannel {

// ---------------------------------------------------------------------------
// TLS record framing helper
// ---------------------------------------------------------------------------

/// Length of the leading run of *complete* TLS records in `buf`. Feeding Secure
/// Transport only whole records keeps a trailing partial record in the caller's
/// buffer so it can be re-presented (SSPI SECBUFFER_EXTRA / SEC_E_INCOMPLETE_
/// MESSAGE) rather than consumed and lost.
static size_t complete_records_len(const uint8_t* buf, size_t len) {
    size_t pos = 0;
    while (pos + 5 <= len) {
        // buf[pos] = content type, buf[pos+1..2] = version, buf[pos+3..4] = length.
        const size_t rec = 5 + (static_cast<size_t>(buf[pos + 3]) << 8 |
                                static_cast<size_t>(buf[pos + 4]));
        if (pos + rec > len) break;   // record body not fully arrived yet
        pos += rec;
    }
    return pos;
}

/// Length of the first TLS record in `buf` if it has fully arrived, else 0.
static size_t first_record_len(const uint8_t* buf, size_t len) {
    if (len < 5) return 0;
    const size_t rec = 5 + (static_cast<size_t>(buf[3]) << 8 | static_cast<size_t>(buf[4]));
    return rec <= len ? rec : 0;
}

/// Map a trust-evaluation failure onto the Secure Transport status the
/// automatic path would have produced, so secur32 reports one error family.
static OSStatus trust_failure_status(CFErrorRef err) {
    const CFIndex code = err ? CFErrorGetCode(err) : 0;
    switch (code) {
    case errSecCertificateExpired:
    case errSecCertificateNotValidYet: return errSSLCertExpired;
    case errSecHostNameMismatch:       return errSSLHostNameMismatch;
    case errSecNotTrusted:             return errSSLUnknownRootCert;
    default:                           return errSSLXCertChainInvalid;
    }
}

// ---------------------------------------------------------------------------
// Secure Transport I/O callbacks — friends of TlsSession so they can reach the
// per-call input window and output accumulator directly.
// ---------------------------------------------------------------------------

int32_t schannel_st_read(void* conn, void* data, size_t* len) {
    auto* s = static_cast<TlsSession*>(conn);
    const size_t want = *len;
    const size_t avail = s->m_in_len - s->m_in_pos;
    const size_t n = want < avail ? want : avail;
    if (n) std::memcpy(data, s->m_in_ptr + s->m_in_pos, n);
    s->m_in_pos += n;
    *len = n;
    return n < want ? errSSLWouldBlock : noErr;
}

int32_t schannel_st_write(void* conn, const void* data, size_t* len) {
    auto* s = static_cast<TlsSession*>(conn);
    const auto* p = static_cast<const uint8_t*>(data);
    s->m_out.insert(s->m_out.end(), p, p + *len);
    return noErr;   // *len unchanged: everything "sent" into the buffer
}

// ---------------------------------------------------------------------------
// TlsSession
// ---------------------------------------------------------------------------

TlsSession::~TlsSession() {
    if (m_ctx) CFRelease(static_cast<SSLContextRef>(m_ctx));
}

TlsSession* TlsSession::create(std::string host, bool verify_peer,
                               bool check_server_name) {
    SSLContextRef ctx = SSLCreateContext(nullptr, kSSLClientSide, kSSLStreamType);
    if (!ctx) return nullptr;

    auto* s = new TlsSession();
    s->m_ctx = ctx;
    s->m_host = std::move(host);
    s->m_verify_peer = verify_peer;
    s->m_check_server_name = check_server_name;

    SSLSetIOFuncs(ctx,
        reinterpret_cast<SSLReadFunc>(&schannel_st_read),
        reinterpret_cast<SSLWriteFunc>(&schannel_st_write));
    SSLSetConnection(ctx, s);

    // SNI + hostname for trust evaluation.
    if (!s->m_host.empty())
        SSLSetPeerDomainName(ctx, s->m_host.c_str(), s->m_host.size());

    // Pin TLS 1.2 (see file header) and offer only AEAD-GCM suites so record
    // framing is the fixed shape EncryptMessage relies on.
    SSLSetProtocolVersionMin(ctx, kTLSProtocol12);
    SSLSetProtocolVersionMax(ctx, kTLSProtocol12);
    static const SSLCipherSuite kGcm[] = {
        (SSLCipherSuite)0xC02C, // ECDHE_ECDSA_AES_256_GCM_SHA384
        (SSLCipherSuite)0xC02B, // ECDHE_ECDSA_AES_128_GCM_SHA256
        (SSLCipherSuite)0xC030, // ECDHE_RSA_AES_256_GCM_SHA384
        (SSLCipherSuite)0xC02F, // ECDHE_RSA_AES_128_GCM_SHA256
        (SSLCipherSuite)0x009D, // RSA_AES_256_GCM_SHA384
        (SSLCipherSuite)0x009C, // RSA_AES_128_GCM_SHA256
    };
    SSLSetEnabledCiphers(ctx, kGcm, sizeof(kGcm) / sizeof(kGcm[0]));

    // When not verifying, break out of the handshake at server-auth time and
    // accept whatever certificate arrives. SCH_CRED_NO_SERVERNAME_CHECK also
    // breaks out, and evaluates the chain with a host-name-free SSL policy.
    if (!verify_peer || !check_server_name)
        SSLSetSessionOption(ctx, kSSLSessionOptionBreakOnServerAuth, true);

    return s;
}

TlsSession::Step TlsSession::handshake(const uint8_t* in, size_t in_len,
                                       size_t& consumed,
                                       std::vector<uint8_t>& out) {
    auto ctx = static_cast<SSLContextRef>(m_ctx);
    m_in_ptr = in;
    m_in_len = complete_records_len(in, in_len);   // whole records only
    m_in_pos = 0;
    m_out.clear();

    OSStatus s;
    for (;;) {
        s = SSLHandshake(ctx);
        if (s != errSSLPeerAuthCompleted) break;
        // Break-on-auth: ST stopped so we can judge the server certificate.
        if (!m_verify_peer) continue;          // manual validation: accept
        // SCH_CRED_NO_SERVERNAME_CHECK: validate the chain, not the name.
        SecTrustRef trust = nullptr;
        if (SSLCopyPeerTrust(ctx, &trust) != errSecSuccess || trust == nullptr) {
            s = errSSLXCertChainInvalid;
            break;
        }
        SecPolicyRef policy = SecPolicyCreateSSL(true, nullptr);
        SecTrustSetPolicies(trust, policy);
        CFErrorRef err = nullptr;
        const bool trusted = SecTrustEvaluateWithError(trust, &err);
        if (!trusted) s = trust_failure_status(err);
        if (err) CFRelease(err);
        if (policy) CFRelease(policy);
        CFRelease(trust);
        if (!trusted) break;
    }

    consumed = m_in_pos;
    out.swap(m_out);

    // The server Certificate may arrive on any leg; Schannel reports its
    // NotAfter as the context expiry from then on.
    if (m_peer_cert_chain_der.empty()) capture_peer_certificate();

    if (s == noErr) {
        capture_peer_certificate();
        m_complete = true;
        return Step::Complete;
    }
    if (s == errSSLWouldBlock) {
        // A token to send, or records consumed, means "call me again". Only when
        // nothing at all could be done (not one whole record) do we report
        // NeedMoreInput, which must consume nothing so the caller re-presents.
        if (!out.empty() || m_in_pos > 0) return Step::Continue;
        consumed = 0;
        return Step::NeedMoreInput;
    }
    if (s <= errSSLPeerUnexpectedMsg && s >= errSSLPeerNoRenegotiation) {
        m_last_failure = Failure::PeerAlert;
    } else {
        switch (s) {
        case errSSLProtocol:          m_last_failure = Failure::Protocol; break;
        case errSSLNegotiation:       m_last_failure = Failure::Negotiation; break;
        case errSSLXCertChainInvalid:
        case errSSLUnknownRootCert:
        case errSSLNoRootCert:        m_last_failure = Failure::UntrustedRoot; break;
        case errSSLCertExpired:
        case errSSLCertNotYetValid:   m_last_failure = Failure::CertExpired; break;
        case errSSLHostNameMismatch:  m_last_failure = Failure::WrongPrincipal; break;
        default:                      m_last_failure = Failure::Other; break;
        }
    }
    return Step::Fatal;
}

bool TlsSession::encrypt(const uint8_t* plaintext, size_t len,
                         std::vector<uint8_t>& record) {
    auto ctx = static_cast<SSLContextRef>(m_ctx);
    m_out.clear();
    size_t processed = 0;
    OSStatus s = SSLWrite(ctx, plaintext, len, &processed);
    if (s != noErr || processed != len) return false;
    record.swap(m_out);
    return true;
}

bool TlsSession::emit_shutdown(std::vector<uint8_t>& out) {
    out.clear();
    if (!m_shutdown_pending) return true;
    if (m_shutdown_sent) {
        m_shutdown_pending = false;
        return true;
    }

    auto ctx = static_cast<SSLContextRef>(m_ctx);
    m_in_ptr = nullptr;
    m_in_len = 0;
    m_in_pos = 0;
    m_out.clear();

    OSStatus s = SSLClose(ctx);
    out.swap(m_out);
    if (s == noErr || s == errSSLWouldBlock || s == errSSLClosedGraceful) {
        m_shutdown_pending = false;
        m_shutdown_sent = true;
        return true;
    }
    return false;
}

TlsSession::DecStep TlsSession::decrypt(const uint8_t* in, size_t in_len,
                                        size_t& consumed,
                                        std::vector<uint8_t>& plaintext) {
    auto ctx = static_cast<SSLContextRef>(m_ctx);
    m_in_ptr = in;
    // Exactly one record per call: Schannel hands back the rest as
    // SECBUFFER_EXTRA (sspi_schannel_stream_probe.expected "two_records").
    m_in_len = first_record_len(in, in_len);
    m_in_pos = 0;
    plaintext.clear();

    if (m_in_len == 0) { consumed = 0; return DecStep::NeedMoreInput; }

    // Drain all records ST can decrypt from the fed window.
    uint8_t chunk[16384];
    for (;;) {
        size_t got = 0;
        OSStatus s = SSLRead(ctx, chunk, sizeof(chunk), &got);
        if (got) plaintext.insert(plaintext.end(), chunk, chunk + got);
        if (s == noErr) {
            if (got == 0) break;      // nothing left buffered
            continue;                 // more may remain
        }
        if (s == errSSLWouldBlock) break;               // fed window exhausted
        if (s == errSSLClosedGraceful) { consumed = m_in_pos; return DecStep::Closed; }
        consumed = m_in_pos;
        return DecStep::Fatal;
    }
    consumed = m_in_pos;
    return DecStep::Ok;
}

void TlsSession::capture_peer_certificate() {
    m_peer_cert_der.clear();
    m_peer_cert_chain_der.clear();
    m_peer_not_after = 0;
#ifdef __APPLE__
    auto ctx = static_cast<SSLContextRef>(m_ctx);
    SecTrustRef trust = nullptr;
    if (SSLCopyPeerTrust(ctx, &trust) != errSecSuccess || trust == nullptr) {
        return;
    }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    const CFIndex certificate_count = SecTrustGetCertificateCount(trust);
#pragma clang diagnostic pop
    for (CFIndex index = 0; index < certificate_count; ++index) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        SecCertificateRef certificate =
            SecTrustGetCertificateAtIndex(trust, index);
#pragma clang diagnostic pop
        if (certificate == nullptr) continue;
        if (index == 0) {
            // FILETIME = 100 ns ticks since 1601-01-01 UTC; CFAbsoluteTime is
            // seconds since 2001-01-01 UTC (12,622,780,800 s apart).
            // SecCertificateCopyValues (macOS 10.7+) reports NotAfter as a
            // CFNumber of CFAbsoluteTime seconds.
            const void* oid = kSecOIDX509V1ValidityNotAfter;
            CFArrayRef keys = CFArrayCreate(nullptr, &oid, 1, &kCFTypeArrayCallBacks);
            CFDictionaryRef values = keys ? SecCertificateCopyValues(certificate, keys, nullptr) : nullptr;
            if (values) {
                const void* entry = CFDictionaryGetValue(values, kSecOIDX509V1ValidityNotAfter);
                if (entry && CFGetTypeID(entry) == CFDictionaryGetTypeID()) {
                    const void* num = CFDictionaryGetValue(static_cast<CFDictionaryRef>(entry),
                                                           kSecPropertyKeyValue);
                    double abs_time = 0;
                    bool have = false;
                    if (num && CFGetTypeID(num) == CFNumberGetTypeID()) {
                        have = CFNumberGetValue(static_cast<CFNumberRef>(num),
                                                kCFNumberDoubleType, &abs_time);
                    } else if (num && CFGetTypeID(num) == CFDateGetTypeID()) {
                        abs_time = CFDateGetAbsoluteTime(static_cast<CFDateRef>(num));
                        have = true;
                    }
                    if (have) {
                        const double secs = abs_time + 12622780800.0;
                        if (secs > 0) {
                            m_peer_not_after =
                                static_cast<uint64_t>(std::llround(secs)) * 10000000ULL;
                        }
                    }
                }
                CFRelease(values);
            }
            if (keys) CFRelease(keys);
        }
        CFDataRef der = SecCertificateCopyData(certificate);
        if (der == nullptr) continue;
        const UInt8* bytes = CFDataGetBytePtr(der);
        const CFIndex len = CFDataGetLength(der);
        if (bytes != nullptr && len > 0) {
            m_peer_cert_chain_der.emplace_back(bytes, bytes + len);
        }
        CFRelease(der);
    }
    CFRelease(trust);
    if (!m_peer_cert_chain_der.empty()) {
        m_peer_cert_der = m_peer_cert_chain_der.front();
    }
#else
    (void)m_ctx;
#endif
}

// ---------------------------------------------------------------------------
// Handle tables
// ---------------------------------------------------------------------------

namespace {
std::mutex g_mtx;
std::unordered_map<uint32_t, CredentialConfig> g_creds;
std::unordered_map<uint32_t, TlsSession*> g_ctxs;           // id -> session
uint32_t g_next_cred = 0xC0DE0001u;
uint32_t g_next_ctx = 0x5E550001u;
}

uint32_t create_credential(const CredentialConfig& config) {
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t id = g_next_cred++;
    g_creds[id] = config;
    return id;
}

bool get_credential(uint32_t cred, CredentialConfig& out) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_creds.find(cred);
    if (it == g_creds.end()) return false;
    out = it->second;
    return true;
}

bool credential_verify_peer(uint32_t cred) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_creds.find(cred);
    return it == g_creds.end() ? true : it->second.verify_peer;
}

bool free_credential(uint32_t cred) {
    std::lock_guard<std::mutex> lk(g_mtx);
    return g_creds.erase(cred) != 0;
}

uint32_t create_context(const CredentialConfig& config, std::string host) {
    TlsSession* s = TlsSession::create(std::move(host), config.verify_peer,
                                       config.check_server_name);
    if (!s) return 0;
    std::lock_guard<std::mutex> lk(g_mtx);
    uint32_t id = g_next_ctx++;
    g_ctxs[id] = s;
    return id;
}

TlsSession* get_context(uint32_t ctx) {
    std::lock_guard<std::mutex> lk(g_mtx);
    auto it = g_ctxs.find(ctx);
    return it == g_ctxs.end() ? nullptr : it->second;
}

bool free_context(uint32_t ctx) {
    TlsSession* victim = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_ctxs.find(ctx);
        if (it != g_ctxs.end()) { victim = it->second; g_ctxs.erase(it); }
    }
    delete victim;
    return victim != nullptr;
}

} // namespace vitreo::winapi::schannel

#pragma clang diagnostic pop

#endif // __APPLE__

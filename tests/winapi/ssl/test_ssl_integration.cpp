/// @file test_ssl_integration.cpp
/// @brief SSL/TLS integration tests for Vitreo Win32 API layer
///
/// Exercises the exact Win32 crypto API sequence that Steam's embedded OpenSSL
/// calls during SSL initialisation and certificate verification:
///
///   1. BCryptGenRandom(NULL, buf, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
///      → entropy for ClientHello nonce / PRNG seeding
///   2. CertOpenStore   → opaque store handle
///   3. CertCreateCertificateContext(X509_ASN_ENCODING, der, len)
///   4. CertAddCertificateContextToStore
///   5. CertGetCertificateChain  → chain context for the leaf
///   6. CertFreeCertificateChain / CertFreeCertificateContext / CertCloseStore
///
/// Also includes DISABLED_ live-TLS tests that open a real socket to an HTTPS
/// host and complete a TLS handshake via macOS SecureTransport to verify the
/// ws2_32 socket infrastructure is suitable for TLS.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/bcrypt/bcrypt.hpp>
#include <winapi/system/crypt32/crypt32.hpp>
#include "../../oracle_data/crypt_chain_certs.h"
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <array>
#include <algorithm>

// macOS SecureTransport (deprecated API, still present & functional)
#ifdef __APPLE__
#include <Security/SecureTransport.h>
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Windows constants used by Steam / OpenSSL
// =============================================================================

static constexpr u32 STATUS_SUCCESS              = 0x00000000;
static constexpr u32 X509_ASN_ENCODING           = 0x00000001;
static constexpr u32 BCRYPT_USE_SYSTEM_PREFERRED_RNG = 0x00000002;

// CERT_CHAIN_CONTEXT field offsets (32-bit guest layout)
static constexpr u32 CHAIN_CTX_OFF_ERROR_STATUS  = 4;
// wincrypt.h. The probe leaf's issuer is in no store, so Windows reports a
// partial chain (crypt_chain_probe.x86.expected "leaf_alone": 0x10000).
static constexpr u32 CERT_TRUST_IS_PARTIAL_CHAIN = 0x00010000;
static constexpr u32 CHAIN_CTX_OFF_CCHAIN        = 12;

// =============================================================================
// Shared test fixture
// =============================================================================

class SslIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // General-purpose region (cert data, output pointers, etc.)
        ASSERT_TRUE(m_memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        // crypt32 scratch region
        ASSERT_TRUE(m_memory.map_region(CRYPT32_SCRATCH_BASE, CRYPT32_SCRATCH_SIZE,
                                        MemoryProtection::ReadWrite).is_ok());
        // Stack
        ASSERT_TRUE(m_memory.map_region(STACK_BASE, 0x2000, MemoryProtection::ReadWrite).is_ok());

        m_cpu.init_protected_mode();
        m_cpu.set_sp(STACK_BASE + 0x1000);
    }

    // Write a well-formed DER certificate to guest memory. Windows (and now
    // Vitreo) decode the certificate and reject malformed encodings with
    // CRYPT_E_ASN1_* (tools/oracle_probe/crypt_prop_probe.expected).
    GuestAddr write_minimal_cert(GuestAddr addr, u32* out_len = nullptr) {
        EXPECT_TRUE(m_memory.write_bytes(addr, kProbeLeafDer, sizeof(kProbeLeafDer)).is_ok());
        if (out_len) *out_len = static_cast<u32>(sizeof(kProbeLeafDer));
        return addr;
    }

    /// CERT_CHAIN_PARA (32-bit) with an empty RequestedUsage. Windows fails
    /// CertGetCertificateChain with E_INVALIDARG when pChainPara is NULL
    /// (crypt_chain_probe.x86.expected "nullpara").
    GuestAddr write_chain_para() {
        EXPECT_TRUE(m_memory.write_u32(PARA_ADDR, 16).is_ok());      // cbSize
        EXPECT_TRUE(m_memory.write_u32(PARA_ADDR + 4, 0).is_ok());   // dwType = USAGE_MATCH_TYPE_AND
        EXPECT_TRUE(m_memory.write_u32(PARA_ADDR + 8, 0).is_ok());   // cUsageIdentifier
        EXPECT_TRUE(m_memory.write_u32(PARA_ADDR + 12, 0).is_ok());  // rgpszUsageIdentifier
        return PARA_ADDR;
    }

    /// Push stdcall argument n at the standard stack position (ESP+4*(n+1))
    void push_arg(u32 slot, GuestAddr value) {
        u32 sp = static_cast<u32>(m_cpu.sp());
        ASSERT_TRUE(m_memory.write_u32(sp + 4 * (slot + 1), static_cast<u32>(value)).is_ok());
    }

    static constexpr GuestAddr STACK_BASE       = 0x80000;
    static constexpr GuestAddr CERT_ADDR        = 0x10000;  // cert DER bytes
    static constexpr GuestAddr OUTPUT_ADDR      = 0x11000;  // pointer-output slots
    static constexpr GuestAddr PARA_ADDR        = 0x11800;  // CERT_CHAIN_PARA

    GuestMemory        m_memory;
    cpu::CpuState      m_cpu;
    BcryptManager      m_bcrypt;
    Crypt32Manager     m_crypt32;
};

// =============================================================================
// 1. BCryptGenRandom — entropy smoke tests
// =============================================================================

// Steam calls: BCryptGenRandom(NULL, buf, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
// The NULL hAlgorithm + BCRYPT_USE_SYSTEM_PREFERRED_RNG flag is the modern path
// used by OpenSSL 1.1+ on Windows to seed its RAND subsystem.

TEST_F(SslIntegrationTest, BCryptGenRandom_SystemPreferredRng_ReturnsSuccess) {
    Marshaler m(m_memory);
    m.attach_cpu(m_cpu);

    u32 rc = m_bcrypt.bcrypt_gen_random(m,
        /*hAlgorithm=*/0,
        /*pbBuffer=*/CERT_ADDR,
        /*cbBuffer=*/32,
        /*dwFlags=*/BCRYPT_USE_SYSTEM_PREFERRED_RNG);

    EXPECT_EQ(rc, STATUS_SUCCESS)
        << "BCryptGenRandom with BCRYPT_USE_SYSTEM_PREFERRED_RNG must return STATUS_SUCCESS";
}

TEST_F(SslIntegrationTest, BCryptGenRandom_SystemPreferredRng_FillsBuffer) {
    Marshaler m(m_memory);
    m.attach_cpu(m_cpu);

    // Zero the target region first so we can detect writes
    for (u32 i = 0; i < 32; ++i)
        ASSERT_TRUE(m_memory.write_u8(CERT_ADDR + i, 0).is_ok());

    u32 rc = m_bcrypt.bcrypt_gen_random(m, 0, CERT_ADDR, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    ASSERT_EQ(rc, STATUS_SUCCESS);

    // Verify that at least one byte is non-zero  
    // (all-zero would indicate the buffer was never written)
    bool any_nonzero = false;
    for (u32 i = 0; i < 32; ++i) {
        auto byte = m_memory.read_u8(CERT_ADDR + i);
        ASSERT_TRUE(byte.is_ok());
        if (byte.value() != 0) { any_nonzero = true; break; }
    }
    EXPECT_TRUE(any_nonzero) << "BCryptGenRandom must produce non-zero entropy";
}

TEST_F(SslIntegrationTest, BCryptGenRandom_SystemPreferredRng_ProducesEntropy) {
    Marshaler m(m_memory);
    m.attach_cpu(m_cpu);

    // Two consecutive calls should produce different 32-byte outputs
    // (probability of collision is 1/2^256 ≈ negligible)
    static constexpr GuestAddr BUF1 = CERT_ADDR;
    static constexpr GuestAddr BUF2 = CERT_ADDR + 0x40;

    ASSERT_EQ(m_bcrypt.bcrypt_gen_random(m, 0, BUF1, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG),
              STATUS_SUCCESS);
    ASSERT_EQ(m_bcrypt.bcrypt_gen_random(m, 0, BUF2, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG),
              STATUS_SUCCESS);

    bool same = true;
    for (u32 i = 0; i < 32; ++i) {
        auto b1 = m_memory.read_u8(BUF1 + i);
        auto b2 = m_memory.read_u8(BUF2 + i);
        ASSERT_TRUE(b1.is_ok() && b2.is_ok());
        if (b1.value() != b2.value()) { same = false; break; }
    }
    EXPECT_FALSE(same) << "Two BCryptGenRandom calls must return different values";
}

TEST_F(SslIntegrationTest, BCryptGenRandom_SmallBuffer_Works) {
    Marshaler m(m_memory);
    m.attach_cpu(m_cpu);

    // OpenSSL sometimes seeds with small incremental reads
    for (u32 len : {1u, 4u, 8u, 16u}) {
        u32 rc = m_bcrypt.bcrypt_gen_random(m, 0, CERT_ADDR, len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        EXPECT_EQ(rc, STATUS_SUCCESS) << "BCryptGenRandom must succeed for len=" << len;
    }
}

// =============================================================================
// 2. Steam cert-chain sequence — verifies what happens during SSL_CTX_new
// =============================================================================
//
// When Steam's embedded OpenSSL builds its certificate store it calls:
//   CertOpenStore → CertCreateCertificateContext → CertAddCertificateContextToStore
//   → CertGetCertificateChain → CertFreeCertificateChain → CertFree... → CertCloseStore
//
// The critical assertion is that CertGetCertificateChain returns TRUE and the
// resulting CERT_CHAIN_CONTEXT has dwErrorStatus == 0 (meaning "trusted").
// Any non-zero error status causes OpenSSL's verify callback to fail with a
// fatal internal_error alert before the ClientHello is even sent.

TEST_F(SslIntegrationTest, SteamCertChain_OpenStoreReturnsNonZeroHandle) {
    u32 handle = m_crypt32.cert_open_store();
    EXPECT_NE(handle, 0u) << "CertOpenStore must return a non-zero handle";
}

TEST_F(SslIntegrationTest, SteamCertChain_CreateContextSucceeds) {
    u32 cert_len = 0;
    write_minimal_cert(CERT_ADDR, &cert_len);

    push_arg(0, X509_ASN_ENCODING);
    push_arg(1, CERT_ADDR);
    push_arg(2, cert_len);

    Marshaler m(m_memory);
    m.attach_cpu(m_cpu);

    u32 ctx = m_crypt32.cert_create_certificate_context(m);
    EXPECT_NE(ctx, 0u) << "CertCreateCertificateContext must return non-NULL context";
}

TEST_F(SslIntegrationTest, SteamCertChain_FullSequence_AllStepsSucceed) {
    // ---- Step 1: CertOpenStore ----
    u32 store = m_crypt32.cert_open_store();
    ASSERT_NE(store, 0u);

    // ---- Step 2: CertCreateCertificateContext ----
    u32 cert_len = 0;
    write_minimal_cert(CERT_ADDR, &cert_len);

    push_arg(0, X509_ASN_ENCODING);
    push_arg(1, CERT_ADDR);
    push_arg(2, cert_len);
    {
        Marshaler m(m_memory); m.attach_cpu(m_cpu);
        u32 ctx = m_crypt32.cert_create_certificate_context(m);
        ASSERT_NE(ctx, 0u);

        // ---- Step 3: CertAddCertificateContextToStore ----
        push_arg(0, store);
        push_arg(1, ctx);
        push_arg(2, 4 /*CERT_STORE_ADD_ALWAYS*/);
        push_arg(3, 0);
        {
            Marshaler m2(m_memory); m2.attach_cpu(m_cpu);
            EXPECT_EQ(m_crypt32.cert_add_certificate_context_to_store(m2), 1u)
                << "CertAddCertificateContextToStore must return TRUE";
        }

        // ---- Step 4: CertGetCertificateChain ----
        //
        // Output pointer written at OUTPUT_ADDR; CertGetCertificateChain writes
        // a guest pointer to the newly-allocated CERT_CHAIN_CONTEXT there.
        push_arg(0, 0);             // hChainEngine = NULL (default)
        push_arg(1, ctx);           // pCertContext
        push_arg(2, 0);             // pTime = NULL
        push_arg(3, 0);             // hAdditionalStore = NULL
        push_arg(4, write_chain_para());  // pChainPara
        push_arg(5, 0);             // dwFlags = 0
        push_arg(6, 0);             // pvReserved = NULL
        push_arg(7, OUTPUT_ADDR);   // ppChainContext (output)
        {
            Marshaler m3(m_memory); m3.attach_cpu(m_cpu);
            u32 ok = m_crypt32.cert_get_certificate_chain(m3);
            ASSERT_EQ(ok, 1u) << "CertGetCertificateChain must return TRUE";

            // Read the output CERT_CHAIN_CONTEXT pointer
            auto chain_ptr_r = m_memory.read_u32(OUTPUT_ADDR);
            ASSERT_TRUE(chain_ptr_r.is_ok());
            u32 chain_addr = chain_ptr_r.value();
            ASSERT_NE(chain_addr, 0u) << "ppChainContext output must be non-NULL";

            // The leaf was added to a store that is not passed as
            // hAdditionalStore, so its issuer is unknown: partial chain.
            auto error_status_r = m_memory.read_u32(chain_addr + CHAIN_CTX_OFF_ERROR_STATUS);
            ASSERT_TRUE(error_status_r.is_ok());
            EXPECT_EQ(error_status_r.value(), CERT_TRUST_IS_PARTIAL_CHAIN);

            // Verify cChain >= 1
            auto cchain_r = m_memory.read_u32(chain_addr + CHAIN_CTX_OFF_CCHAIN);
            ASSERT_TRUE(cchain_r.is_ok());
            EXPECT_GE(cchain_r.value(), 1u) << "Chain must contain at least one simple chain";
        }

        // ---- Step 5: CertFreeCertificateChain (no-op) ----
        m_crypt32.cert_free_certificate_chain();  // must not crash

        // ---- Step 6: CertFreeCertificateContext ----
        push_arg(0, ctx);
        Marshaler free_context(m_memory);
        free_context.attach_cpu(m_cpu);
        EXPECT_EQ(m_crypt32.cert_free_certificate_context(free_context), 1u);
    }

    // ---- Step 7: CertCloseStore ----
    EXPECT_EQ(m_crypt32.cert_close_store(store, 0), 1u);
}

TEST_F(SslIntegrationTest, SteamCertChain_MultipleContexts_IndependentChains) {
    // OpenSSL may call CertGetCertificateChain multiple times with different
    // cert contexts (e.g. leaf + intermediates). Verify scratch region handles
    // multiple allocations without overlap or OOM.
    u32 cert_len = 0;
    write_minimal_cert(CERT_ADDR, &cert_len);

    for (int i = 0; i < 4; ++i) {
        push_arg(0, X509_ASN_ENCODING);
        push_arg(1, CERT_ADDR);
        push_arg(2, cert_len);
        Marshaler mc(m_memory); mc.attach_cpu(m_cpu);
        u32 ctx = m_crypt32.cert_create_certificate_context(mc);
        ASSERT_NE(ctx, 0u) << "Context " << i << " must be non-NULL";

        GuestAddr out_addr = OUTPUT_ADDR + static_cast<u32>(i) * 4;
        push_arg(0, 0); push_arg(1, ctx); push_arg(2, 0); push_arg(3, 0);
        push_arg(4, write_chain_para()); push_arg(5, 0);  push_arg(6, 0); push_arg(7, out_addr);

        Marshaler mg(m_memory); mg.attach_cpu(m_cpu);
        ASSERT_EQ(m_crypt32.cert_get_certificate_chain(mg), 1u);

        auto ptr_r = m_memory.read_u32(out_addr);
        ASSERT_TRUE(ptr_r.is_ok());
        ASSERT_NE(ptr_r.value(), 0u);

        auto err_r = m_memory.read_u32(ptr_r.value() + CHAIN_CTX_OFF_ERROR_STATUS);
        ASSERT_TRUE(err_r.is_ok());
        EXPECT_EQ(err_r.value(), CERT_TRUST_IS_PARTIAL_CHAIN) << "Chain " << i;
    }
}

// =============================================================================
// 3. Combined: BCryptGenRandom + CertGetCertificateChain round-trip
//    Simulates the full Steam SSL_CTX_new → SSL_connect pre-ClientHello path
// =============================================================================

TEST_F(SslIntegrationTest, FullSteamSslPreClientHelloSequence) {
    // --- Entropy seeding (Steam calls this once at startup) ---
    {
        Marshaler m(m_memory); m.attach_cpu(m_cpu);
        u32 rc = m_bcrypt.bcrypt_gen_random(m, 0, CERT_ADDR + 0x200, 32,
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        ASSERT_EQ(rc, STATUS_SUCCESS) << "BCryptGenRandom must succeed before SSL init";
    }

    // --- Certificate context + chain (Steam does this per TLS connection) ---
    u32 cert_len = 0;
    write_minimal_cert(CERT_ADDR, &cert_len);

    push_arg(0, X509_ASN_ENCODING);
    push_arg(1, CERT_ADDR);
    push_arg(2, cert_len);
    Marshaler mc(m_memory); mc.attach_cpu(m_cpu);
    u32 ctx = m_crypt32.cert_create_certificate_context(mc);
    ASSERT_NE(ctx, 0u);

    push_arg(0, 0); push_arg(1, ctx); push_arg(2, 0); push_arg(3, 0);
    push_arg(4, write_chain_para()); push_arg(5, 0);  push_arg(6, 0); push_arg(7, OUTPUT_ADDR);
    Marshaler mg(m_memory); mg.attach_cpu(m_cpu);
    ASSERT_EQ(m_crypt32.cert_get_certificate_chain(mg), 1u);

    auto chain_ptr = m_memory.read_u32(OUTPUT_ADDR);
    ASSERT_TRUE(chain_ptr.is_ok());
    ASSERT_NE(chain_ptr.value(), 0u);

    auto err_status = m_memory.read_u32(chain_ptr.value() + CHAIN_CTX_OFF_ERROR_STATUS);
    ASSERT_TRUE(err_status.is_ok());
    EXPECT_EQ(err_status.value(), CERT_TRUST_IS_PARTIAL_CHAIN);
}

// =============================================================================
// 4. Live TLS handshake tests (DISABLED_ — require network access)
//
// These tests make a real connection to an HTTPS server and complete a TLS
// handshake using macOS SecureTransport, proving the underlying socket
// infrastructure (ws2_32.alloc_socket) can support TLS.
//
// Run manually with: --gtest_filter='*DISABLED_*' --gtest_also_run_disabled_tests
// =============================================================================

#ifdef __APPLE__

/// Minimal SecureTransport read/write callbacks using a raw socket fd.
static OSStatus ssl_read_cb(SSLConnectionRef conn, void* data, size_t* len) {
    int fd = *static_cast<const int*>(conn);
    ssize_t n = ::recv(fd, data, *len, 0);
    if (n > 0)  { *len = static_cast<size_t>(n); return noErr; }
    if (n == 0) { *len = 0; return errSSLClosedGraceful; }
    *len = 0;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? errSSLWouldBlock
                                                     : errSSLClosedAbort;
}

static OSStatus ssl_write_cb(SSLConnectionRef conn, const void* data, size_t* len) {
    int fd = *static_cast<const int*>(conn);
    ssize_t n = ::send(fd, data, *len, 0);
    if (n >= 0) { *len = static_cast<size_t>(n); return noErr; }
    *len = 0;
    return (errno == EAGAIN || errno == EWOULDBLOCK) ? errSSLWouldBlock
                                                     : errSSLClosedAbort;
}

/// Connect a plain TCP socket to host:port. Returns native fd or -1 on failure.
static int connect_tcp(const char* host, int port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    std::snprintf(port_str, sizeof(port_str), "%d", port);

    if (::getaddrinfo(host, port_str, &hints, &res) != 0 || !res)
        return -1;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { ::freeaddrinfo(res); return -1; }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        ::close(fd);
        ::freeaddrinfo(res);
        return -1;
    }
    ::freeaddrinfo(res);
    return fd;
}

/// Complete a TLS handshake with SecureTransport over the given socket fd.
/// @param fd       Connected TCP socket
/// @param hostname SNI hostname for the TLS handshake
/// Returns noErr on success.
static OSStatus do_tls_handshake(int fd, const char* hostname) {
    SSLContextRef ctx = SSLCreateContext(nullptr, kSSLClientSide, kSSLStreamType);
    if (!ctx) return errSecAllocate;

    SSLSetIOFuncs(ctx, ssl_read_cb, ssl_write_cb);
    SSLSetConnection(ctx, &fd);
    SSLSetPeerDomainName(ctx, hostname, std::strlen(hostname));

    OSStatus status;
    do {
        status = SSLHandshake(ctx);
    } while (status == errSSLWouldBlock);

    SSLClose(ctx);
    CFRelease(ctx);
    return status;
}

TEST(SslLiveTest, DISABLED_TlsHandshakeToExampleComSucceeds) {
    // Connects to example.com:443 and completes a TLS handshake.
    // Validates that:
    //   - DNS resolution works
    //   - TCP connect works on port 443
    //   - TLS 1.2/1.3 handshake succeeds (macOS SecureTransport)
    //
    // This test checks that the raw networking layer works for TLS; it does NOT
    // go through Vitreo's Win32 thunks because Steam's TLS runs inside the guest.

    int fd = connect_tcp("example.com", 443);
    if (fd < 0) GTEST_SKIP() << "Cannot connect to example.com:443 — network unavailable";

    OSStatus st = do_tls_handshake(fd, "example.com");
    ::close(fd);

    EXPECT_EQ(st, noErr) << "TLS handshake failed: SecureTransport OSStatus=" << st;
}

TEST(SslLiveTest, DISABLED_TlsHandshakeToCdnSteamstaticSucceeds) {
    // Same test targeting the actual Steam CDN host.
    // This is the domain Steam's updater connects to when downloading manifests.

    int fd = connect_tcp("cdn.steamstatic.com", 443);
    if (fd < 0) GTEST_SKIP() << "Cannot connect to cdn.steamstatic.com:443 — network unavailable";

    OSStatus st = do_tls_handshake(fd, "cdn.steamstatic.com");
    ::close(fd);

    EXPECT_EQ(st, noErr) << "TLS handshake to cdn.steamstatic.com failed: OSStatus=" << st;
}

TEST(SslLiveTest, DISABLED_TlsHandshakeReceivesHttpResponse) {
    // Full HTTPS round-trip: connect → TLS → send HTTP request → read response.
    // A successful HTTP 200/301/302 response proves TLS is working end-to-end.

    int fd = connect_tcp("example.com", 443);
    if (fd < 0) GTEST_SKIP() << "Cannot connect to example.com:443 — network unavailable";

    SSLContextRef ctx = SSLCreateContext(nullptr, kSSLClientSide, kSSLStreamType);
    OSStatus st = errSecAllocate;
    if (ctx) {
        SSLSetIOFuncs(ctx, ssl_read_cb, ssl_write_cb);
        SSLSetConnection(ctx, &fd);
        SSLSetPeerDomainName(ctx, "example.com", 11);

        do { st = SSLHandshake(ctx); } while (st == errSSLWouldBlock);
        if (st == noErr) {
            // Send HTTP/1.1 GET
            const char* req =
                "GET / HTTP/1.1\r\n"
                "Host: example.com\r\n"
                "Connection: close\r\n"
                "\r\n";
            size_t processed = 0;
            SSLWrite(ctx, req, std::strlen(req), &processed);

            // Read response
            char buf[2048] = {};
            size_t read_sz = 0;
            do {
                size_t chunk = 0;
                st = SSLRead(ctx, buf + read_sz, sizeof(buf) - read_sz - 1, &chunk);
                read_sz += chunk;
            } while (st == noErr && read_sz < sizeof(buf) - 1);

            buf[read_sz] = '\0';
            bool is_http = (std::strstr(buf, "HTTP/1.") != nullptr);
            EXPECT_TRUE(is_http)
                << "Expected HTTP response, got: " << std::string(buf, std::min(read_sz, size_t(200)));
        }
        SSLClose(ctx);
        CFRelease(ctx);
    }
    ::close(fd);
    // errSSLClosedGraceful (-9805): server sent TLS close_notify — normal for Connection: close
    EXPECT_TRUE(st == noErr || st == errSSLClosedGraceful)
        << "TLS handshake or HTTP read failed: OSStatus=" << st;
}

#pragma clang diagnostic pop

#endif  // __APPLE__

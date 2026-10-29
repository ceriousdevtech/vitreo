// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_context_info.cpp
/// @brief Certificate verification flow, context isolation and CERT_INFO read-back

#include "crypt32_test_fixture.hpp"

// =============================================================================
// Integration-style Tests
// =============================================================================

TEST_F(Crypt32FullTest, FullCertificateVerificationFlow) {
    // Simulate the full flow Steam's OpenSSL callback uses:
    // 1. CertOpenSystemStoreW("ROOT")
    // 2. CertCreateCertificateContext
    // 3. CertGetCertificateChain
    // 4. CertVerifyCertificateChainPolicy
    // 5. CertFreeCertificateChain
    // 6. CertFreeCertificateContext
    // 7. CertCloseStore

    // Step 1: Open store
    GuestAddr stack = m_cpu.sp();
    Marshaler open_store(m_memory);
    open_store.attach_cpu(m_cpu);
    ASSERT_TRUE(open_store.write_wide_string(CERT_DATA_ADDR + 0x800, "ROOT").is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, CERT_DATA_ADDR + 0x800).is_ok());
    u32 store_handle = m_mgr->cert_open_system_store_w(open_store);
    EXPECT_NE(store_handle, 0u);

    // Step 2: Create certificate context
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh_create(m_memory);
    marsh_create.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh_create);
    ASSERT_NE(ctx_addr, 0u);

    // Step 3: Get certificate chain
    GuestAddr chain_out_addr = OUTPUT_PTR_ADDR;
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());             // hChainEngine = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 8, ctx_addr).is_ok());      // pCertContext
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());            // pTime = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());            // hAdditionalStore = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());            // pChainPara
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());            // dwFlags
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());            // pvReserved
    ASSERT_TRUE(m_memory.write_u32(stack + 32, chain_out_addr).is_ok()); // ppChainContext

    Marshaler marsh_chain(m_memory);
    marsh_chain.attach_cpu(m_cpu);

    u32 chain_result = m_mgr->cert_get_certificate_chain(marsh_chain);
    ASSERT_EQ(chain_result, 1u);

    auto chain_ptr = m_memory.read_u32(chain_out_addr);
    ASSERT_TRUE(chain_ptr.is_ok());
    ASSERT_NE(chain_ptr.value(), 0u);

    // Step 4: Verify chain policy
    GuestAddr status_addr = OUTPUT_PTR_ADDR + 0x100;
    ASSERT_TRUE(m_memory.write_u32(status_addr, POLICY_STATUS_SIZE).is_ok());

    ASSERT_TRUE(m_memory.write_u32(stack + 4, CERT_CHAIN_POLICY_SSL).is_ok());  // pszPolicyOID
    ASSERT_TRUE(m_memory.write_u32(stack + 8, chain_ptr.value()).is_ok());      // pChainContext
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());                     // pPolicyPara = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 16, status_addr).is_ok());           // pPolicyStatus

    Marshaler marsh_verify(m_memory);
    marsh_verify.attach_cpu(m_cpu);

    u32 verify_result = m_mgr->cert_verify_certificate_chain_policy(marsh_verify);
    EXPECT_EQ(verify_result, 1u);

    auto error_code = m_memory.read_u32(status_addr + 4);
    ASSERT_TRUE(error_code.is_ok());
    EXPECT_NE(error_code.value(), 0u) << "Unsigned synthetic certificate must fail policy verification";

    // Step 5: Free chain (no-op)
    m_mgr->cert_free_certificate_chain();

    // Step 6: Free cert context
    EXPECT_EQ(free_cert_context(ctx_addr), 1u);

    // Step 7: Close store
    EXPECT_EQ(m_mgr->cert_close_store(store_handle, 0), 1u);
}

// =============================================================================
// Memory Isolation Tests
// =============================================================================

TEST_F(Crypt32FullTest, MultipleCertContextsIndependent) {
    // Create two certificates and verify they get independent contexts
    auto [cert1_addr, cert1_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    auto [cert2_addr, cert2_len] = write_minimal_der_cert(CERT_DATA_ADDR + 0x2000);

    GuestAddr stack = m_cpu.sp();

    // Create first context
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert1_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert1_len).is_ok());

    Marshaler marsh1(m_memory);
    marsh1.attach_cpu(m_cpu);

    u32 ctx1 = m_mgr->cert_create_certificate_context(marsh1);
    ASSERT_NE(ctx1, 0u);

    // Create second context
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert2_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert2_len).is_ok());

    Marshaler marsh2(m_memory);
    marsh2.attach_cpu(m_cpu);

    u32 ctx2 = m_mgr->cert_create_certificate_context(marsh2);
    ASSERT_NE(ctx2, 0u);

    // Contexts should be at different addresses
    EXPECT_NE(ctx1, ctx2);

    // Each should have correct encoding type
    auto enc1 = m_memory.read_u32(ctx1 + CERT_CONTEXT_OFFSET_ENCODING_TYPE);
    auto enc2 = m_memory.read_u32(ctx2 + CERT_CONTEXT_OFFSET_ENCODING_TYPE);

    ASSERT_TRUE(enc1.is_ok());
    ASSERT_TRUE(enc2.is_ok());

    // Only the certificate half of the encoding type is kept (crypt_prop_probe
    // "ctx" row).
    EXPECT_EQ(enc1.value(), X509_ASN_ENCODING);
    EXPECT_EQ(enc2.value(), X509_ASN_ENCODING);
}

// =============================================================================
// Phase 3 Tests: pCertInfo stub and scratch OOM
// =============================================================================

TEST_F(Crypt32FullTest, CertCreateContextHasNonNullCertInfo) {
    // After creating a cert context, pCertInfo (offset 12) should be non-NULL
    // and point to a valid CERT_INFO stub with dwVersion=2 (CERT_V3)
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);

    // Read pCertInfo pointer at offset 12
    auto cert_info_ptr = m_memory.read_u32(ctx + CERT_CONTEXT_OFFSET_P_CERT_INFO);
    ASSERT_TRUE(cert_info_ptr.is_ok());
    EXPECT_NE(cert_info_ptr.value(), 0u) << "pCertInfo must be non-NULL";

    // Read dwVersion from the CERT_INFO stub
    auto version = m_memory.read_u32(cert_info_ptr.value() + 0);
    ASSERT_TRUE(version.is_ok());
    EXPECT_EQ(version.value(), 2u) << "dwVersion should be CERT_V3 (2)";
}

TEST_F(Crypt32FullTest, CertCreateContextDecodesSerialAndValidity) {
    auto [cert_addr, cert_len] = write_der_cert_with_validity(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);

    auto cert_info_ptr = m_memory.read_u32(ctx + CERT_CONTEXT_OFFSET_P_CERT_INFO);
    ASSERT_TRUE(cert_info_ptr.is_ok());
    GuestAddr info = cert_info_ptr.value();

    auto serial_len = m_memory.read_u32(info + 4);
    auto serial_ptr = m_memory.read_u32(info + 8);
    ASSERT_TRUE(serial_len.is_ok());
    ASSERT_TRUE(serial_ptr.is_ok());
    // kProbeLeafDer serial 0x3003, little-endian in CERT_INFO (crypt_prop_probe
    // "info" row: 2 bytes, 03 30).
    EXPECT_EQ(serial_len.value(), 2u);
    ASSERT_NE(serial_ptr.value(), 0u);
    EXPECT_EQ(m_memory.read_u8(serial_ptr.value()).value_or(0), 0x03u);
    EXPECT_EQ(m_memory.read_u8(serial_ptr.value() + 1).value_or(0), 0x30u);

    const u32 not_after_lo = m_memory.read_u32(info + 40).value_or(0);
    const u32 not_after_hi = m_memory.read_u32(info + 44).value_or(0);
    EXPECT_NE(not_after_lo | not_after_hi, 0u);

    auto exports = create_crypt32_thunks(*m_mgr);
    auto verify = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "CertVerifyTimeValidity"; });
    ASSERT_NE(verify, exports.entries.end());
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, static_cast<u32>(info)).is_ok());
    verify->handler(marsh);
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rax), 0u);
}

TEST_F(Crypt32FullTest, CertCreateContextMaterializesSanExtension) {
    // kProbeLeafDer carries six extensions; subjectAltName is the fourth
    // (crypt_prop_probe "info" row).
    const u8* cert = kProbeLeafDer;
    ASSERT_TRUE(m_memory.write_bytes(CERT_DATA_ADDR, cert, sizeof(kProbeLeafDer)).is_ok());

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, CERT_DATA_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(sizeof(kProbeLeafDer))).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);

    const GuestAddr info =
        m_memory.read_u32(ctx + CERT_CONTEXT_OFFSET_P_CERT_INFO).value_or(0);
    ASSERT_NE(info, 0u);
    EXPECT_EQ(m_memory.read_u32(info + 104).value_or(0), 6u);
    const GuestAddr rg_ext = m_memory.read_u32(info + 108).value_or(0);
    ASSERT_NE(rg_ext, 0u);
    const GuestAddr san = rg_ext + 3 * 16;
    const GuestAddr oid = m_memory.read_u32(san).value_or(0);
    ASSERT_NE(oid, 0u);
    std::string oid_text;
    for (u32 i = 0; i < 16; ++i) {
        const u8 ch = m_memory.read_u8(oid + i).value_or(0);
        if (ch == 0) break;
        oid_text.push_back(static_cast<char>(ch));
    }
    EXPECT_EQ(oid_text, "2.5.29.17");
    EXPECT_EQ(m_memory.read_u32(san + 8).value_or(0), 0x15u);
}

TEST_F(Crypt32FullTest, CertCreateContextMaterializesSubjectPublicKeyInfo) {
    // sha256WithRSAEncryption + rsaEncryption (crypt_prop_probe "info" row:
    // Parameters 2 bytes, PublicKey 0x10e bytes, 0 unused bits).
    ASSERT_TRUE(m_memory.write_bytes(CERT_DATA_ADDR, kProbeLeafDer, sizeof(kProbeLeafDer)).is_ok());

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, CERT_DATA_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(sizeof(kProbeLeafDer))).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);

    const GuestAddr info =
        m_memory.read_u32(ctx + CERT_CONTEXT_OFFSET_P_CERT_INFO).value_or(0);
    ASSERT_NE(info, 0u);
    EXPECT_EQ(m_memory.read_u32(info + 16).value_or(0), 2u);  // Parameters (05 00)

    const GuestAddr sig_oid = m_memory.read_u32(info + 12).value_or(0);
    ASSERT_NE(sig_oid, 0u);
    EXPECT_EQ(m_memory.read_u8(sig_oid).value_or(0), static_cast<u8>('1'));

    const GuestAddr spki_oid = m_memory.read_u32(info + 56).value_or(0);
    ASSERT_NE(spki_oid, 0u);
    std::string spki_oid_str;
    for (u32 i = 0; i < 32; ++i) {
        const u8 ch = m_memory.read_u8(spki_oid + i).value_or(0);
        if (ch == 0) break;
        spki_oid_str.push_back(static_cast<char>(ch));
    }
    EXPECT_EQ(spki_oid_str, "1.2.840.113549.1.1.1");

    EXPECT_EQ(m_memory.read_u32(info + 68).value_or(0), 0x10eu);
    const GuestAddr key = m_memory.read_u32(info + 72).value_or(0);
    ASSERT_NE(key, 0u);
    EXPECT_EQ(m_memory.read_u8(key).value_or(0), 0x30u);  // RSAPublicKey SEQUENCE
    EXPECT_EQ(m_memory.read_u32(info + 76).value_or(0xff), 0u);
}

TEST_F(Crypt32FullTest, CertGetNameStringAReturnsSimpleDisplayName) {
    // Subject CN=vitrum-probe.test, O=Vitrum Oracle: SIMPLE_DISPLAY (4) is the CN.
    ASSERT_TRUE(m_memory.write_bytes(CERT_DATA_ADDR, kProbeLeafDer, sizeof(kProbeLeafDer)).is_ok());
    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, CERT_DATA_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(sizeof(kProbeLeafDer))).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);

    constexpr GuestAddr name_addr = 0x13000;
    ASSERT_TRUE(m_memory.write_u32(stack + 4, ctx).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 4).is_ok()); // CERT_NAME_SIMPLE_DISPLAY_TYPE
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, name_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 32).is_ok());
    EXPECT_EQ(m_mgr->cert_get_name_string(marsh, false), 18u);
    const std::string expected = "vitrum-probe.test";
    for (usize i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(m_memory.read_u8(name_addr + i).value_or(0), static_cast<u8>(expected[i]));
    }
    EXPECT_EQ(m_memory.read_u8(name_addr + expected.size()).value_or(1), 0u);
}

TEST_F(Crypt32FullTest, CertGetNameStringRdnWithNullTypeParaIsSimpleNameStr) {
    // wintrust_signer_name_probe.expected: CERT_NAME_RDN_TYPE with a NULL
    // pvTypePara is CERT_SIMPLE_NAME_STR (values only, no "CN=" keys); an
    // explicit CERT_X500_NAME_STR keeps the keys.
    ASSERT_TRUE(m_memory.write_bytes(CERT_DATA_ADDR, kProbeLeafDer, sizeof(kProbeLeafDer)).is_ok());
    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, CERT_DATA_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(sizeof(kProbeLeafDer))).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    const u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);
    constexpr GuestAddr name_addr = 0x13000, para_addr = 0x13200;
    const auto name = [&](GuestAddr para) {
        EXPECT_TRUE(m_memory.write_u32(stack + 4, ctx).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, 2).is_ok());  // CERT_NAME_RDN_TYPE
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 16, para).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 20, name_addr).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 24, 128).is_ok());
        const u32 n = m_mgr->cert_get_name_string(marsh, false);
        std::string text;
        for (u32 i = 0; i + 1 < n; ++i) text.push_back(static_cast<char>(m_memory.read_u8(name_addr + i).value_or(0)));
        return text;
    };
    const std::string simple = name(0);
    EXPECT_EQ(simple.find('='), std::string::npos) << simple;
    EXPECT_NE(simple.find("vitrum-probe.test"), std::string::npos) << simple;
    ASSERT_TRUE(m_memory.write_u32(para_addr, 3).is_ok());  // CERT_X500_NAME_STR
    EXPECT_NE(name(para_addr).find("CN=vitrum-probe.test"), std::string::npos);
}

TEST_F(Crypt32FullTest, CertNameToStrAFormatsX500CommonName) {
    // Subject Name DER: SEQUENCE { SET { SEQUENCE { OID CN, Printable "Test" } } }
    static const u8 name_der[] = {
        0x30, 0x0f,
          0x31, 0x0d,
            0x30, 0x0b,
              0x06, 0x03, 0x55, 0x04, 0x03,
              0x13, 0x04, 'T', 'e', 's', 't',
    };
    constexpr GuestAddr blob_addr = 0x12800;
    constexpr GuestAddr der_addr = 0x12900;
    constexpr GuestAddr out_addr = 0x12a00;
    for (u32 i = 0; i < sizeof(name_der); ++i) {
        ASSERT_TRUE(m_memory.write_u8(der_addr + i, name_der[i]).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(blob_addr, static_cast<u32>(sizeof(name_der))).is_ok());
    ASSERT_TRUE(m_memory.write_u32(blob_addr + 4, static_cast<u32>(der_addr)).is_ok());

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, blob_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 3).is_ok()); // CERT_X500_NAME_STR
    ASSERT_TRUE(m_memory.write_u32(stack + 16, out_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, 32).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_name_to_str_a(marsh), 8u); // "CN=Test\0"
    EXPECT_EQ(m_memory.read_u8(out_addr).value_or(0), static_cast<u8>('C'));
    EXPECT_EQ(m_memory.read_u8(out_addr + 1).value_or(0), static_cast<u8>('N'));
    EXPECT_EQ(m_memory.read_u8(out_addr + 2).value_or(0), static_cast<u8>('='));
    EXPECT_EQ(m_memory.read_u8(out_addr + 3).value_or(0), static_cast<u8>('T'));
    EXPECT_EQ(m_memory.read_u8(out_addr + 7).value_or(0), 0u);
}

TEST_F(Crypt32FullTest, CertVerifyTimeValidityRejectsExpiredCertInfo) {
    // kProbeExpiredLeafDer: valid 2020-01-01..2021-01-01.
    ASSERT_TRUE(m_memory.write_bytes(CERT_DATA_ADDR, kProbeExpiredLeafDer,
                                     sizeof(kProbeExpiredLeafDer)).is_ok());
    const GuestAddr cert_addr = CERT_DATA_ADDR;
    const u32 cert_len = sizeof(kProbeExpiredLeafDer);
    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);
    const u32 info =
        m_memory.read_u32(ctx + CERT_CONTEXT_OFFSET_P_CERT_INFO).value_or(0);
    ASSERT_NE(info, 0u);

    auto exports = create_crypt32_thunks(*m_mgr);
    auto verify = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) { return e.name == "CertVerifyTimeValidity"; });
    ASSERT_NE(verify, exports.entries.end());
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, info).is_ok());
    verify->handler(marsh);
    // "Now" is after NotAfter.
    EXPECT_EQ(static_cast<u32>(m_cpu.gpr.rax), 1u);
}

TEST_F(Crypt32BasicTest, ScratchOomReturnsZero) {
    // Fill scratch almost to capacity, verify next alloc fails
    GuestAddr addr = m_mgr->scratch_alloc(CRYPT32_SCRATCH_SIZE - 4);
    EXPECT_NE(addr, 0u);

    // One more allocation of 8 bytes should fail (only 4 left)
    GuestAddr fail = m_mgr->scratch_alloc(8);
    EXPECT_EQ(fail, 0u);
}

TEST_F(Crypt32FullTest, CertCloseStoreDoesNotInvalidateOutstandingContext) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    GuestAddr stack = m_cpu.sp();
    const u32 store = m_mgr->cert_open_store();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    const u32 context = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(context, 0u);

    ASSERT_EQ(m_mgr->cert_close_store(store, 0), 1u);
    EXPECT_EQ(m_memory.read_u32(
                  context + CERT_CONTEXT_OFFSET_CB_CERT_ENCODED).value_or(0),
              cert_len);
}

TEST_F(Crypt32FullTest, CertContextReadBackValidation) {
    // Verify that all CERT_CONTEXT fields can be read back correctly
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    u32 ctx = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx, 0u);

    // Read back all fields
    auto enc = m_memory.read_u32(ctx + 0);
    auto pb = m_memory.read_u32(ctx + 4);
    auto cb = m_memory.read_u32(ctx + 8);
    auto info = m_memory.read_u32(ctx + 12);
    auto store = m_memory.read_u32(ctx + 16);

    ASSERT_TRUE(enc.is_ok());
    ASSERT_TRUE(pb.is_ok());
    ASSERT_TRUE(cb.is_ok());
    ASSERT_TRUE(info.is_ok());
    ASSERT_TRUE(store.is_ok());

    EXPECT_EQ(enc.value(), X509_ASN_ENCODING);
    EXPECT_NE(pb.value(), 0u) << "pbCertEncoded should point to cert data copy";
    EXPECT_EQ(cb.value(), cert_len);
    EXPECT_NE(info.value(), 0u) << "pCertInfo should point to CERT_INFO";
    EXPECT_NE(store.value(), 0u) << "the context has a store of its own";
}

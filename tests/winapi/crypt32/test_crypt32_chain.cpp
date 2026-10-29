// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_chain.cpp
/// @brief CertGetCertificateChain, CertFreeCertificateChain and CertVerifyCertificateChainPolicy

#include "crypt32_test_fixture.hpp"

// =============================================================================
// CertGetCertificateChain Tests
// =============================================================================

TEST_F(Crypt32FullTest, CertGetChainReturnsSuccessStatus) {
    // Create a certificate context first
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh1(m_memory);
    marsh1.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh1);
    ASSERT_NE(ctx_addr, 0u);

    // Now call CertGetCertificateChain
    // Args: hChainEngine(0), pCertContext(1), pTime(2), hAdditionalStore(3),
    //       pChainPara(4), dwFlags(5), pvReserved(6), ppChainContext(7)
    // In 32-bit stdcall: all args on stack at ESP+4, +8, +12, etc.
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());             // hChainEngine = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 8, ctx_addr).is_ok());      // pCertContext
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());            // pTime = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());            // hAdditionalStore = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());            // pChainPara
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());            // dwFlags
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());            // pvReserved
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok()); // ppChainContext

    Marshaler marsh2(m_memory);
    marsh2.attach_cpu(m_cpu);

    u32 result = m_mgr->cert_get_certificate_chain(marsh2);
    EXPECT_EQ(result, 1u) << "CertGetCertificateChain should return TRUE";

    // Verify the chain context pointer was written
    auto chain_ptr_res = m_memory.read_u32(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(chain_ptr_res.is_ok());
    u32 chain_addr = chain_ptr_res.value();
    EXPECT_NE(chain_addr, 0u);
}

TEST_F(Crypt32FullTest, CertGetChainBuildsValidChainContext) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh1(m_memory);
    marsh1.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh1);
    ASSERT_NE(ctx_addr, 0u);

    // Call CertGetCertificateChain - all args on stack for 32-bit stdcall
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());             // hChainEngine = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 8, ctx_addr).is_ok());      // pCertContext
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());            // pTime = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());            // hAdditionalStore = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());            // pChainPara
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());            // dwFlags
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());            // pvReserved
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok()); // ppChainContext

    Marshaler marsh2(m_memory);
    marsh2.attach_cpu(m_cpu);

    u32 result = m_mgr->cert_get_certificate_chain(marsh2);
    ASSERT_EQ(result, 1u);

    auto chain_ptr_res = m_memory.read_u32(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(chain_ptr_res.is_ok());
    u32 chain_addr = chain_ptr_res.value();
    ASSERT_NE(chain_addr, 0u);

    // Verify CERT_CHAIN_CONTEXT structure - critical for "always-trust"
    auto cb_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_CBSIZE);
    auto err_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS);
    auto info_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_INFO_STATUS);
    auto count_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_CCHAIN);
    auto chain_arr_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_RGPCHAIN);

    ASSERT_TRUE(cb_res.is_ok());
    ASSERT_TRUE(err_res.is_ok());
    ASSERT_TRUE(info_res.is_ok());
    ASSERT_TRUE(count_res.is_ok());
    ASSERT_TRUE(chain_arr_res.is_ok());

    EXPECT_EQ(cb_res.value(), 56u) << "cbSize should be 56 bytes";
    // A leaf whose issuer is in no store: CERT_TRUST_IS_PARTIAL_CHAIN and no
    // info bits (crypt_chain_probe.x86.expected "leaf_alone").
    EXPECT_EQ(err_res.value(), 0x00010000u);
    EXPECT_EQ(info_res.value(), 0u);
    EXPECT_EQ(count_res.value(), 1u) << "Should have exactly 1 chain";
    EXPECT_NE(chain_arr_res.value(), 0u) << "rgpChain should not be NULL";
}

TEST_F(Crypt32FullTest, CertGetChainBuildsValid64BitPointerLayout) {
    m_cpu.init_long_mode();
    m_cpu.set_sp(STACK_BASE + 0x800);

    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    m_cpu.gpr.rcx = X509_ASN_ENCODING;
    m_cpu.gpr.rdx = cert_addr;
    m_cpu.gpr.r8 = cert_len;

    Marshaler marsh1(m_memory);
    marsh1.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh1);
    ASSERT_NE(ctx_addr, 0u);

    auto ctx_cb_res = m_memory.read_u32(ctx_addr + CERT_CONTEXT64_OFFSET_CB_CERT_ENCODED);
    auto ctx_data_res = m_memory.read_u64(ctx_addr + CERT_CONTEXT64_OFFSET_PB_CERT_ENCODED);
    auto ctx_info_res = m_memory.read_u64(ctx_addr + CERT_CONTEXT64_OFFSET_P_CERT_INFO);
    auto ctx_store_res = m_memory.read_u64(ctx_addr + CERT_CONTEXT64_OFFSET_H_CERT_STORE);
    ASSERT_TRUE(ctx_cb_res.is_ok());
    ASSERT_TRUE(ctx_data_res.is_ok());
    ASSERT_TRUE(ctx_info_res.is_ok());
    ASSERT_TRUE(ctx_store_res.is_ok());
    EXPECT_EQ(ctx_cb_res.value(), cert_len);
    EXPECT_NE(ctx_data_res.value(), 0u);
    EXPECT_NE(ctx_info_res.value(), 0u);
    EXPECT_NE(ctx_store_res.value(), 0u);  // the context's own store

    m_cpu.gpr.rcx = 0;
    m_cpu.gpr.rdx = ctx_addr;
    m_cpu.gpr.r8 = 0;
    m_cpu.gpr.r9 = 0;

    GuestAddr stack = static_cast<GuestAddr>(m_cpu.sp());
    ASSERT_TRUE(m_memory.write_u64(stack + 0x28, chain_para(true)).is_ok());
    ASSERT_TRUE(m_memory.write_u64(stack + 0x30, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u64(stack + 0x38, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u64(stack + 0x40, OUTPUT_PTR_ADDR).is_ok());

    Marshaler marsh2(m_memory);
    marsh2.attach_cpu(m_cpu);

    ASSERT_EQ(m_mgr->cert_get_certificate_chain(marsh2), 1u);

    auto chain_ptr_res = m_memory.read_u64(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(chain_ptr_res.is_ok());
    GuestAddr chain_addr = static_cast<GuestAddr>(chain_ptr_res.value());
    ASSERT_NE(chain_addr, 0u);

    auto chain_cb_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_CBSIZE);
    auto chain_err_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS);
    auto chain_count_res = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_CCHAIN);
    auto chain_array_res = m_memory.read_u64(chain_addr + CERT_CHAIN_CONTEXT64_OFFSET_RGPCHAIN);
    ASSERT_TRUE(chain_cb_res.is_ok());
    ASSERT_TRUE(chain_err_res.is_ok());
    ASSERT_TRUE(chain_count_res.is_ok());
    ASSERT_TRUE(chain_array_res.is_ok());
    EXPECT_EQ(chain_cb_res.value(), CERT_CHAIN_CONTEXT64_SIZE);
    EXPECT_NE(chain_err_res.value(), CERT_TRUST_NO_ERROR);
    EXPECT_EQ(chain_count_res.value(), 1u);

    auto simple_ptr_res = m_memory.read_u64(static_cast<GuestAddr>(chain_array_res.value()));
    ASSERT_TRUE(simple_ptr_res.is_ok());
    GuestAddr simple_addr = static_cast<GuestAddr>(simple_ptr_res.value());

    auto simple_cb_res = m_memory.read_u32(simple_addr + CERT_CHAIN_CONTEXT_OFFSET_CBSIZE);
    auto elem_array_res = m_memory.read_u64(simple_addr + CERT_SIMPLE_CHAIN64_OFFSET_RGP_ELEMENT);
    ASSERT_TRUE(simple_cb_res.is_ok());
    ASSERT_TRUE(elem_array_res.is_ok());
    EXPECT_EQ(simple_cb_res.value(), CERT_SIMPLE_CHAIN64_SIZE);

    auto elem_ptr_res = m_memory.read_u64(static_cast<GuestAddr>(elem_array_res.value()));
    ASSERT_TRUE(elem_ptr_res.is_ok());
    GuestAddr elem_addr = static_cast<GuestAddr>(elem_ptr_res.value());

    auto elem_cb_res = m_memory.read_u32(elem_addr + CERT_CHAIN_CONTEXT_OFFSET_CBSIZE);
    auto elem_cert_res = m_memory.read_u64(elem_addr + CERT_CHAIN_ELEMENT64_OFFSET_CERT_CONTEXT);
    ASSERT_TRUE(elem_cb_res.is_ok());
    ASSERT_TRUE(elem_cert_res.is_ok());
    EXPECT_EQ(elem_cb_res.value(), CERT_CHAIN_ELEMENT64_SIZE);
    EXPECT_EQ(elem_cert_res.value(), static_cast<u64>(ctx_addr));
}

TEST_F(Crypt32FullTest, CertGetChainReturnsFalseWithNullOutput) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh1(m_memory);
    marsh1.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh1);
    ASSERT_NE(ctx_addr, 0u);

    // Call CertGetCertificateChain with NULL output pointer
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());             // hChainEngine = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 8, ctx_addr).is_ok());      // pCertContext
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());            // pTime = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());            // hAdditionalStore = NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());            // pChainPara
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());            // dwFlags
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());            // pvReserved
    ASSERT_TRUE(m_memory.write_u32(stack + 32, 0).is_ok());            // ppChainContext = NULL

    Marshaler marsh2(m_memory);
    marsh2.attach_cpu(m_cpu);

    u32 result = m_mgr->cert_get_certificate_chain(marsh2);
    EXPECT_EQ(result, 0u) << "Should return FALSE with NULL output pointer";
}

// =============================================================================
// CertFreeCertificateChain Tests
// =============================================================================

TEST_F(Crypt32BasicTest, CertFreeCertificateChainNoOp) {
    // Just ensure it doesn't crash
    m_mgr->cert_free_certificate_chain();
    SUCCEED();
}

TEST_F(Crypt32BasicTest, MissingOrMalformedCertificateMaterialCannotEstablishTrust) {
    EXPECT_NE(m_mgr->host_verify_cert_chain({}, {}, {}), 0u);
    EXPECT_NE(m_mgr->host_verify_cert_chain({0x30, 0x00}, {}, {}), 0u);
}

// =============================================================================
// CertVerifyCertificateChainPolicy Tests
// =============================================================================

// Expected values: tools/oracle_probe/crypt_chain_probe.x86.expected.

TEST_F(Crypt32FullTest, CertVerifyChainPolicyReturnsSuccess) {
    // leaf <- intermediate <- root, with the probe root trusted through an
    // engine's exclusive root store ("excl_leaf_inter": every policy passes
    // with lChainIndex = lElementIndex = -1).
    const u32 chain = build_trusted_leaf_chain();
    ASSERT_NE(chain, 0u);
    EXPECT_EQ(m_memory.read_u32(chain + CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS).value_or(1), 0u);
    EXPECT_EQ(m_memory.read_u32(chain + CERT_CHAIN_CONTEXT_OFFSET_INFO_STATUS).value_or(0), 0x2100u);
    for (u32 policy : {CERT_CHAIN_POLICY_BASE, CERT_CHAIN_POLICY_SSL, 2u}) {
        const auto [ok, status] = verify_policy(policy, chain);
        EXPECT_EQ(ok, 1u);
        EXPECT_EQ(status[1], 0u) << policy;
        EXPECT_EQ(status[2], 0xffffffffu);
        EXPECT_EQ(status[3], 0xffffffffu);
    }
}

TEST_F(Crypt32FullTest, CertVerifyChainPolicyWritesFullStatus) {
    // A partial chain under the BASE policy: CERT_E_CHAINING, chain 0,
    // element -1; cbSize and pvExtraPolicyStatus are not written.
    const u32 chain = build_lone_leaf_chain();
    ASSERT_NE(chain, 0u);
    const auto [ok, status] = verify_policy(CERT_CHAIN_POLICY_BASE, chain);
    EXPECT_EQ(ok, 1u);
    EXPECT_EQ(status[0], POLICY_STATUS_SIZE);
    EXPECT_EQ(status[1], 0x800B010Au);
    EXPECT_EQ(status[2], 0u);
    EXPECT_EQ(status[3], 0xffffffffu);
    EXPECT_EQ(status[4], 0xDDDDDDDDu);
}

TEST_F(Crypt32FullTest, CertVerifyChainPolicySslReportsUntrustedRootForPartialChain) {
    const u32 chain = build_lone_leaf_chain();
    ASSERT_NE(chain, 0u);
    const auto [ok, status] = verify_policy(CERT_CHAIN_POLICY_SSL, chain);
    EXPECT_EQ(ok, 1u);
    EXPECT_EQ(status[1], 0x800B0109u);
    EXPECT_EQ(status[2], 0u);
    EXPECT_EQ(status[3], 0xffffffffu);
}

TEST_F(Crypt32FullTest, CertVerifyChainPolicyAuthenticodeReportsChainingForPartialChain) {
    const u32 chain = build_lone_leaf_chain();
    ASSERT_NE(chain, 0u);
    const auto [ok, status] = verify_policy(2u, chain);  // CERT_CHAIN_POLICY_AUTHENTICODE
    EXPECT_EQ(ok, 1u);
    EXPECT_EQ(status[1], 0x800B010Au);
    EXPECT_EQ(status[2], 0u);
    EXPECT_EQ(status[3], 0xffffffffu);
}

// Learn CertCloseStore: without CERT_CLOSE_STORE_FORCE_FLAG the store stays
// open until the contexts taken from it are freed, and a context's hCertStore
// stays usable. Chromium's Windows verifier closes its memory store right after
// adding the server's certificates and passes the leaf's hCertStore as the
// additional store (crypt32_ssl_chain_probe CSC_CLOSE_STORE: Windows builds the
// full chain). Erasing the store at the close left a partial chain, which
// Battle.net's login page reported as ERR_CERT_AUTHORITY_INVALID.
TEST_F(Crypt32FullTest, ClosedStoreStaysReachableThroughItsCertificateContexts) {
    const GuestAddr stack = m_cpu.sp();
    const u32 roots = m_mgr->cert_open_store();
    const u32 server = m_mgr->cert_open_store();
    auto add = [&](u32 store, const u8* der, u32 size, GuestAddr at, GuestAddr out) {
        ASSERT_TRUE(m_memory.write_bytes(at, der, size).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(at)).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 16, size).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 20, CERT_STORE_ADD_ALWAYS).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 24, static_cast<u32>(out)).is_ok());
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        ASSERT_EQ(m_mgr->cert_add_encoded_certificate_to_store(m), 1u);
    };
    constexpr GuestAddr kLeafOut = OUTPUT_PTR_ADDR + 0x400;
    add(roots, kProbeRootDer, sizeof(kProbeRootDer), CERT_DATA_ADDR + 0x4000, 0);
    add(server, kProbeLeafDer, sizeof(kProbeLeafDer), CERT_DATA_ADDR, kLeafOut);
    add(server, kProbeIntermediateDer, sizeof(kProbeIntermediateDer),
        CERT_DATA_ADDR + 0x5000, 0);
    const u32 leaf = m_memory.read_u32(kLeafOut).value_or(0);
    ASSERT_NE(leaf, 0u);

    EXPECT_EQ(m_mgr->cert_close_store(server, 0), 1u);
    // x86 CERT_CONTEXT: dwCertEncodingType, pbCertEncoded, cbCertEncoded,
    // pCertInfo, hCertStore.
    const u32 leaf_store = m_memory.read_u32(leaf + 16).value_or(0);
    EXPECT_EQ(leaf_store, server);

    // x86 CERT_CHAIN_ENGINE_CONFIG with hExclusiveRoot (cbSize 0x30), as in
    // build_trusted_leaf_chain.
    constexpr GuestAddr kConfig = 0x13800;
    for (u32 off = 0; off < 0x34; off += 4) {
        ASSERT_TRUE(m_memory.write_u32(kConfig + off, 0).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(kConfig, 0x30).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kConfig + 0x28, roots).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 4, static_cast<u32>(kConfig)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, static_cast<u32>(OUTPUT_PTR_ADDR + 0x300)).is_ok());
    {
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        ASSERT_EQ(m_mgr->cert_create_certificate_chain_engine(m), 1u);
    }
    const u32 engine = m_memory.read_u32(OUTPUT_PTR_ADDR + 0x300).value_or(0);

    const u32 chain = get_chain(engine, leaf, leaf_store);
    ASSERT_NE(chain, 0u);
    EXPECT_EQ(m_memory.read_u32(chain + CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS).value_or(1), 0u)
        << "the intermediate in the closed store must still be found";
    const auto [ok, status] = verify_policy(CERT_CHAIN_POLICY_SSL, chain);
    EXPECT_EQ(ok, 1u);
    EXPECT_EQ(status[1], 0u);

    // Once the last context from it is freed the store is really closed.
    EXPECT_EQ(free_cert_context(leaf), 1u);
    set_last_error(0);
    EXPECT_EQ(m_mgr->cert_close_store(server, 0), 0u);
    EXPECT_EQ(get_last_error(), 6u);  // ERROR_INVALID_HANDLE
}

// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_chain_layout.cpp
/// @brief CERT_CHAIN_CONTEXT layout conformance and certificate handle lifecycles

#include "crypt32_test_fixture.hpp"

// =============================================================================
// Phase 7: Contract Conformance Tests
// =============================================================================

// --- CERT_CHAIN_CONTEXT Layout Conformance ---
// Verifies that the guest-side CERT_CHAIN_CONTEXT matches the Windows SDK
// 32-bit layout exactly, including traversal down to CERT_CHAIN_ELEMENT.

TEST_F(Crypt32FullTest, ChainContextCbSizeIs56) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    // Create context
    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    GuestAddr created_ctx = 0;
    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      created_ctx = m_mgr->cert_create_certificate_context(m);
      ASSERT_NE(created_ctx, 0u); }

    // Build chain
    auto ctx_r = m_memory.read_u32(stack + 8); // reuse cert_addr for pCertContext
    ASSERT_TRUE(m_memory.write_u32(stack + 4,  0).is_ok());           // hChainEngine
    ASSERT_TRUE(m_memory.write_u32(stack + 8,  static_cast<u32>(created_ctx)).is_ok()); // pCertContext
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());           // pTime
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());           // hAdditionalStore
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());           // pChainPara
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());           // dwFlags
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());           // pvReserved
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok()); // ppChainContext

    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      ASSERT_EQ(m_mgr->cert_get_certificate_chain(m), 1u); }

    auto chain_ptr = m_memory.read_u32(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(chain_ptr.is_ok());
    u32 chain_addr = chain_ptr.value();
    ASSERT_NE(chain_addr, 0u);

    // cbSize at offset 0 must be 56 (Windows SDK: sizeof(CERT_CHAIN_CONTEXT) == 56)
    auto cb = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_CBSIZE);
    ASSERT_TRUE(cb.is_ok());
    EXPECT_EQ(cb.value(), 56u) << "CERT_CHAIN_CONTEXT.cbSize must be 56";
}

TEST_F(Crypt32FullTest, ChainContextTrustStatusLayout) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    u32 context = 0;
    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      context = m_mgr->cert_create_certificate_context(m);
      ASSERT_NE(context, 0u); }

    ASSERT_TRUE(m_memory.write_u32(stack + 4,  0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8,  context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok());

    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      ASSERT_EQ(m_mgr->cert_get_certificate_chain(m), 1u); }

    u32 chain_addr = m_memory.read_u32(OUTPUT_PTR_ADDR).value();

    // TrustStatus is a CERT_TRUST_STATUS struct at offsets 4 and 8:
    //   +4 dwErrorStatus (DWORD)
    //   +8 dwInfoStatus  (DWORD)
    auto err_status = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS);
    auto info_status = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_INFO_STATUS);
    ASSERT_TRUE(err_status.is_ok());
    ASSERT_TRUE(info_status.is_ok());

    // An environment variable cannot confer trust; a lone leaf is a partial
    // chain with no info bits (crypt_chain_probe.x86.expected "leaf_alone").
    EXPECT_EQ(err_status.value(), 0x00010000u);
    EXPECT_EQ(info_status.value(), 0u);
}

TEST_F(Crypt32FullTest, ChainContextTraversalToElement) {
    // Full traversal: CERT_CHAIN_CONTEXT → rgpChain[0] → CERT_SIMPLE_CHAIN
    //   → rgpElement[0] → CERT_CHAIN_ELEMENT → pCertContext

    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    u32 cert_ctx;
    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      cert_ctx = m_mgr->cert_create_certificate_context(m);
      ASSERT_NE(cert_ctx, 0u); }

    ASSERT_TRUE(m_memory.write_u32(stack + 4,  0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8,  cert_ctx).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok());

    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      ASSERT_EQ(m_mgr->cert_get_certificate_chain(m), 1u); }

    u32 chain_addr = m_memory.read_u32(OUTPUT_PTR_ADDR).value();

    // cChain >= 1
    auto cChain = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_CCHAIN);
    ASSERT_TRUE(cChain.is_ok());
    ASSERT_GE(cChain.value(), 1u);

    // rgpChain pointer at offset 16
    auto rgpChain = m_memory.read_u32(chain_addr + CERT_CHAIN_CONTEXT_OFFSET_RGPCHAIN);
    ASSERT_TRUE(rgpChain.is_ok());
    ASSERT_NE(rgpChain.value(), 0u);

    // Dereference rgpChain[0] → CERT_SIMPLE_CHAIN address
    auto simple_addr_r = m_memory.read_u32(rgpChain.value());
    ASSERT_TRUE(simple_addr_r.is_ok());
    u32 simple_addr = simple_addr_r.value();
    ASSERT_NE(simple_addr, 0u);

    // CERT_SIMPLE_CHAIN.cbSize at offset 0 must be 32
    auto simple_cb = m_memory.read_u32(simple_addr + 0);
    ASSERT_TRUE(simple_cb.is_ok());
    EXPECT_EQ(simple_cb.value(), 32u) << "CERT_SIMPLE_CHAIN.cbSize must be 32";

    // CERT_SIMPLE_CHAIN.cElement at offset 12 must be >= 1
    auto cElement = m_memory.read_u32(simple_addr + 12);
    ASSERT_TRUE(cElement.is_ok());
    ASSERT_GE(cElement.value(), 1u);

    // CERT_SIMPLE_CHAIN.rgpElement at offset 16
    auto rgpElement = m_memory.read_u32(simple_addr + 16);
    ASSERT_TRUE(rgpElement.is_ok());
    ASSERT_NE(rgpElement.value(), 0u);

    // Dereference rgpElement[0] → CERT_CHAIN_ELEMENT address
    auto elem_addr_r = m_memory.read_u32(rgpElement.value());
    ASSERT_TRUE(elem_addr_r.is_ok());
    u32 elem_addr = elem_addr_r.value();
    ASSERT_NE(elem_addr, 0u);

    // CERT_CHAIN_ELEMENT.cbSize at offset 0 must be 32
    auto elem_cb = m_memory.read_u32(elem_addr + 0);
    ASSERT_TRUE(elem_cb.is_ok());
    EXPECT_EQ(elem_cb.value(), 32u) << "CERT_CHAIN_ELEMENT.cbSize must be 32";

    // CERT_CHAIN_ELEMENT.pCertContext at offset 4 must point to our cert context
    auto elem_ctx = m_memory.read_u32(elem_addr + 4);
    ASSERT_TRUE(elem_ctx.is_ok());
    EXPECT_EQ(elem_ctx.value(), cert_ctx)
        << "CERT_CHAIN_ELEMENT.pCertContext must point to the leaf cert context";

    // A partial chain's element carries no error of its own; its info names
    // the authority-key-identifier match (0x2) (crypt_chain_probe
    // "leaf_alone").
    EXPECT_EQ(m_memory.read_u32(elem_addr + 8).value_or(0xff), 0u);
    EXPECT_EQ(m_memory.read_u32(elem_addr + 12).value_or(0xff), 0x2u);
}

TEST_F(Crypt32FullTest, ChainPolicyOverwritesPriorValues) {
    // dwError, lChainIndex and lElementIndex are all written.
    const u32 chain = build_lone_leaf_chain();
    ASSERT_NE(chain, 0u);
    const auto [ok, status] = verify_policy(CERT_CHAIN_POLICY_SSL, chain);
    EXPECT_EQ(ok, 1u) << "CertVerifyCertificateChainPolicy must return TRUE";
    EXPECT_EQ(status[1], 0x800B0109u);
    EXPECT_EQ(status[2], 0u);
    EXPECT_EQ(status[3], 0xffffffffu);
}

// --- Handle Lifecycle Conformance ---

TEST_F(Crypt32FullTest, HandleLifecycleOpenCloseCycle) {
    // CertOpenStore → CertCloseStore must not crash or leak
    for (int i = 0; i < 5; ++i) {
        u32 store = m_mgr->cert_open_store();
        EXPECT_NE(store, 0u) << "Iteration " << i;
        EXPECT_EQ(m_mgr->cert_close_store(store, 0), 1u) << "Iteration " << i;
    }
}

TEST_F(Crypt32FullTest, CreateFreeCyclesDoNotExhaustCertificateStorage) {
    // This exceeds the former fixed 64 KiB Crypt32 scratch arena. Each final
    // free must release its context-owned guest allocations for the next TLS
    // certificate chain.
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    for (int i = 0; i < 512; ++i) {
        GuestAddr stack = m_cpu.sp();
        ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        u32 ctx = m_mgr->cert_create_certificate_context(m);
        EXPECT_NE(ctx, 0u) << "Iteration " << i;

        EXPECT_EQ(free_cert_context(ctx), 1u) << "Iteration " << i;
    }
}

TEST_F(Crypt32FullTest, CertFreeCertificateChainIdempotent) {
    // CertFreeCertificateChain is a no-op — must not crash on repeated calls
    m_mgr->cert_free_certificate_chain();
    m_mgr->cert_free_certificate_chain();
    m_mgr->cert_free_certificate_chain();
}

TEST_F(Crypt32FullTest, FullLifecycleWithChain) {
    // Complete lifecycle: OpenStore → Create → Add → GetChain → FreeChain → Free → Close
    u32 store = m_mgr->cert_open_store();
    ASSERT_NE(store, 0u);

    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    u32 ctx;
    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      ctx = m_mgr->cert_create_certificate_context(m);
      ASSERT_NE(ctx, 0u); }

    // Add to store
    ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, ctx).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      EXPECT_EQ(m_mgr->cert_add_certificate_context_to_store(m), 1u); }

    // Get chain
    ASSERT_TRUE(m_memory.write_u32(stack + 4,  0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8,  ctx).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok());
    { Marshaler m(m_memory); m.attach_cpu(m_cpu);
      ASSERT_EQ(m_mgr->cert_get_certificate_chain(m), 1u); }

    auto chain_ptr = m_memory.read_u32(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(chain_ptr.is_ok());
    auto err = m_memory.read_u32(chain_ptr.value() + CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS);
    ASSERT_TRUE(err.is_ok());
    EXPECT_NE(err.value(), 0u) << "Unsigned certificate must not be trusted";

    // Teardown sequence (must not crash)
    m_mgr->cert_free_certificate_chain();
    EXPECT_EQ(free_cert_context(ctx), 1u);
    EXPECT_EQ(m_mgr->cert_close_store(store, 0), 1u);
}

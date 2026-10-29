// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_trust_anchors.cpp
/// @brief Encoded-certificate adds, self-signed root anchoring and issuer lookup

#include "crypt32_test_fixture.hpp"

TEST_F(Crypt32FullTest, CertAddEncodedContextSurvivesDeferredStoreClose) {
    const auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    const u32 store = m_mgr->cert_open_store();
    const GuestAddr stack = m_cpu.sp();

    // Chromium's CreateCertContextWithChain call pattern.
    ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, cert_len).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, OUTPUT_PTR_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(OUTPUT_PTR_ADDR, 0).is_ok());

    Marshaler add(m_memory);
    add.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_add_encoded_certificate_to_store(add), 1u);

    const auto context_result = m_memory.read_u32(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(context_result.is_ok());
    const GuestAddr context = context_result.value();
    ASSERT_NE(context, 0u);
    EXPECT_EQ(m_memory.read_u32(
                  context + CERT_CONTEXT_OFFSET_ENCODING_TYPE).value_or(0),
              X509_ASN_ENCODING);
    EXPECT_EQ(m_memory.read_u32(
                  context + CERT_CONTEXT_OFFSET_CB_CERT_ENCODED).value_or(0),
              cert_len);
    EXPECT_EQ(m_memory.read_u32(
                  context + CERT_CONTEXT_OFFSET_H_CERT_STORE).value_or(0),
              store);

    const GuestAddr copied = m_memory.read_u32(
        context + CERT_CONTEXT_OFFSET_PB_CERT_ENCODED).value_or(0);
    ASSERT_NE(copied, 0u);
    for (u32 index = 0; index < cert_len; ++index) {
        EXPECT_EQ(m_memory.read_u8(copied + index).value_or(0),
                  m_memory.read_u8(cert_addr + index).value_or(0));
    }

    // CERT_STORE_DEFER_CLOSE_UNTIL_LAST_FREE_FLAG lets Chromium close the
    // temporary store while retaining the returned context.
    ASSERT_EQ(m_mgr->cert_close_store(store, 0), 1u);
    EXPECT_EQ(m_memory.read_u32(
                  context + CERT_CONTEXT_OFFSET_CB_CERT_ENCODED).value_or(0),
              cert_len);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok());
    Marshaler chain(m_memory);
    chain.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_get_certificate_chain(chain), 1u);
}

TEST_F(Crypt32FullTest, CertAddEncodedRejectsInvalidStore) {
    const auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    const GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0xDEAD1234u).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, cert_len).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, OUTPUT_PTR_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(OUTPUT_PTR_ADDR, 0xAAAAAAAAu).is_ok());
    Marshaler add(m_memory);
    add.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_add_encoded_certificate_to_store(add), 0u);
    EXPECT_EQ(m_memory.read_u32(OUTPUT_PTR_ADDR).value_or(1), 0u);
}

TEST_F(Crypt32FullTest, RootSystemStorePersistsAndAnchorsSelfSignedCertificate) {
    struct RestoreTrustOverride {
        ~RestoreTrustOverride() { (void)::setenv("VITREO_CERT_ALWAYS_TRUST", "1", 1); }
    } restore;
    (void)::unsetenv("VITREO_CERT_ALWAYS_TRUST");

    std::vector<u8> cert = decode_hex(kLocalBattleNetSelfSignedHex);
    ASSERT_EQ(cert.size(), 477u);
    ASSERT_TRUE(m_memory.write_bytes(
        CERT_DATA_ADDR, cert.data(), cert.size()).is_ok());

    const GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, CERT_DATA_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(
        stack + 12, static_cast<u32>(cert.size())).is_ok());
    Marshaler create(m_memory);
    create.attach_cpu(m_cpu);
    const u32 context = m_mgr->cert_create_certificate_context(create);
    ASSERT_NE(context, 0u);

    constexpr GuestAddr store_name = CERT_DATA_ADDR + 0x800;
    ASSERT_TRUE(create.write_wide_string(store_name, "ROOT").is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, store_name).is_ok());
    const u32 root = m_mgr->cert_open_system_store_w(create);
    ASSERT_NE(root, 0u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, root).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    Marshaler add(m_memory);
    add.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_add_certificate_context_to_store(add), 1u);
    ASSERT_EQ(m_mgr->cert_close_store(root, 0), 1u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 32, OUTPUT_PTR_ADDR).is_ok());
    Marshaler chain(m_memory);
    chain.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_get_certificate_chain(chain), 1u);
    const u32 chain_context =
        m_memory.read_u32(OUTPUT_PTR_ADDR).value_or(0);
    ASSERT_NE(chain_context, 0u);
    EXPECT_EQ(m_memory.read_u32(chain_context + 4).value_or(0xffffffffu), 0u);
}


// Learn (CertGetIssuerCertificateFromStore): a self-signed root reports
// NULL + CRYPT_E_SELF_SIGNED. It must not hand back another certificate that
// merely shares the name, or a caller walking the chain until NULL never
// stops (StarCraft II's main thread spun here for 23 minutes).
TEST_F(Crypt32FullTest, GetIssuerOfSelfSignedRootIsSelfSignedNotNameSibling) {
    struct RestoreTrustOverride {
        ~RestoreTrustOverride() { (void)::setenv("VITREO_CERT_ALWAYS_TRUST", "1", 1); }
    } restore;
    (void)::unsetenv("VITREO_CERT_ALWAYS_TRUST");
    constexpr u32 CRYPT_E_SELF_SIGNED = 0x80092007u;
    constexpr u32 CERT_STORE_SIGNATURE_FLAG = 0x00000001u;

    const std::vector<u8> root = decode_hex(kLocalBattleNetSelfSignedHex);
    // Same Subject and Issuer names, different serial: a re-issued root
    // whose bytes differ, exactly the sibling the old search returned.
    std::vector<u8> sibling = root;
    ASSERT_EQ(sibling[15], 0x01u);  // serialNumber INTEGER value
    sibling[15] = 0x02;

    constexpr GuestAddr root_addr = CERT_DATA_ADDR;
    constexpr GuestAddr sibling_addr = CERT_DATA_ADDR + 0x400;
    constexpr GuestAddr flags_addr = CERT_DATA_ADDR + 0xc00;
    ASSERT_TRUE(m_memory.write_bytes(root_addr, root.data(), root.size()).is_ok());
    ASSERT_TRUE(m_memory.write_bytes(
        sibling_addr, sibling.data(), sibling.size()).is_ok());

    const GuestAddr stack = m_cpu.sp();
    auto create = [&](GuestAddr addr, usize size) -> u32 {
        EXPECT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, static_cast<u32>(addr)).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(size)).is_ok());
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        return m_mgr->cert_create_certificate_context(m);
    };
    const u32 root_ctx = create(root_addr, root.size());
    ASSERT_NE(root_ctx, 0u);
    const u32 sibling_ctx = create(sibling_addr, sibling.size());
    ASSERT_NE(sibling_ctx, 0u);

    const u32 store = m_mgr->cert_open_store();
    ASSERT_NE(store, 0u);
    for (u32 ctx : {root_ctx, sibling_ctx}) {
        ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 8, ctx).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
        Marshaler add(m_memory);
        add.attach_cpu(m_cpu);
        ASSERT_EQ(m_mgr->cert_add_certificate_context_to_store(add), 1u);
    }

    ASSERT_TRUE(m_memory.write_u32(flags_addr, CERT_STORE_SIGNATURE_FLAG).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, root_ctx).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, flags_addr).is_ok());
    Marshaler issuer(m_memory);
    issuer.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_get_issuer_certificate_from_store(issuer), 0u);
    EXPECT_EQ(get_last_error(), CRYPT_E_SELF_SIGNED);

    EXPECT_EQ(m_mgr->cert_close_store(store, 0), 1u);
}

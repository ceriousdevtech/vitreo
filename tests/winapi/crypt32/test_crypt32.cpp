/// @file test_crypt32.cpp
/// @brief CRYPT32 store handles, scratch allocator and certificate-context creation/lifetime

#include "crypt32_test_fixture.hpp"

// =============================================================================
// Certificate Store Handle Tests
// =============================================================================

TEST_F(Crypt32BasicTest, CertOpenStoreReturnsValidHandle) {
    u32 handle = m_mgr->cert_open_store();
    EXPECT_NE(handle, 0u);
    EXPECT_GE(handle, 0xCE580100u) << "Handle should start at expected base";
}

TEST_F(Crypt32BasicTest, CertOpenStoreHandlesAreSequential) {
    u32 h1 = m_mgr->cert_open_store();
    u32 h2 = m_mgr->cert_open_store();
    u32 h3 = m_mgr->cert_open_store();

    EXPECT_EQ(h2, h1 + 1);
    EXPECT_EQ(h3, h2 + 1);
}

TEST_F(Crypt32FullTest, CertOpenSystemStoreWReturnsValidHandle) {
    Marshaler marshaler(m_memory);
    marshaler.attach_cpu(m_cpu);
    ASSERT_TRUE(marshaler.write_wide_string(CERT_DATA_ADDR, "ROOT").is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.sp() + 8, CERT_DATA_ADDR).is_ok());
    u32 handle = m_mgr->cert_open_system_store_w(marshaler);
    EXPECT_NE(handle, 0u);
    EXPECT_GE(handle, 0xCE580100u);
}

TEST_F(Crypt32FullTest, CertOpenStoreAndSystemStoreShareHandleCounter) {
    Marshaler marshaler(m_memory);
    marshaler.attach_cpu(m_cpu);
    ASSERT_TRUE(marshaler.write_wide_string(CERT_DATA_ADDR, "ROOT").is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.sp() + 8, CERT_DATA_ADDR).is_ok());
    u32 h1 = m_mgr->cert_open_store();
    u32 h2 = m_mgr->cert_open_system_store_w(marshaler);
    u32 h3 = m_mgr->cert_open_store();

    EXPECT_EQ(h2, h1 + 1);
    EXPECT_EQ(h3, h2 + 1);
}

TEST_F(Crypt32BasicTest, CertCloseStoreReturnsTrue) {
    const u32 store = m_mgr->cert_open_store();
    EXPECT_EQ(m_mgr->cert_close_store(store, 0), 1u);
    EXPECT_EQ(m_mgr->cert_close_store(store, 0), 0u);
}

// =============================================================================
// Scratch Allocator Tests
// =============================================================================

TEST_F(Crypt32BasicTest, ScratchAllocReturnsBaseOnFirstCall) {
    GuestAddr addr = m_mgr->scratch_alloc(16);
    EXPECT_EQ(addr, CRYPT32_SCRATCH_BASE);
}

TEST_F(Crypt32BasicTest, ScratchAllocAdvancesPointer) {
    GuestAddr addr1 = m_mgr->scratch_alloc(16);
    GuestAddr addr2 = m_mgr->scratch_alloc(32);
    GuestAddr addr3 = m_mgr->scratch_alloc(8);

    EXPECT_EQ(addr1, CRYPT32_SCRATCH_BASE);
    EXPECT_EQ(addr2, CRYPT32_SCRATCH_BASE + 16);
    EXPECT_EQ(addr3, CRYPT32_SCRATCH_BASE + 48);
}

TEST_F(Crypt32BasicTest, ScratchAllocAlignsTo4Bytes) {
    GuestAddr addr1 = m_mgr->scratch_alloc(5);   // Rounds to 8
    GuestAddr addr2 = m_mgr->scratch_alloc(1);   // Rounds to 4
    GuestAddr addr3 = m_mgr->scratch_alloc(7);   // Rounds to 8

    EXPECT_EQ(addr1, CRYPT32_SCRATCH_BASE);
    EXPECT_EQ(addr2, CRYPT32_SCRATCH_BASE + 8);
    EXPECT_EQ(addr3, CRYPT32_SCRATCH_BASE + 12);
}

TEST_F(Crypt32BasicTest, ScratchResetClearsOffset) {
    m_mgr->scratch_alloc(100);
    m_mgr->scratch_alloc(200);
    m_mgr->scratch_reset();

    GuestAddr addr = m_mgr->scratch_alloc(16);
    EXPECT_EQ(addr, CRYPT32_SCRATCH_BASE);
}

TEST_F(Crypt32BasicTest, ScratchAllocReturnsZeroOnOverflow) {
    // Allocate most of the scratch space
    GuestAddr addr1 = m_mgr->scratch_alloc(CRYPT32_SCRATCH_SIZE - 100);
    EXPECT_NE(addr1, 0u);

    // Try to allocate more than remaining
    GuestAddr addr2 = m_mgr->scratch_alloc(200);
    EXPECT_EQ(addr2, 0u);
}

// =============================================================================
// CertCreateCertificateContext Tests
// =============================================================================

TEST_F(Crypt32FullTest, CertCreateContextReturnsValidPointer) {
    // Write cert data to guest memory
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    // Set up stdcall arguments on stack
    // For stdcall: args are on stack at [ESP+4], [ESP+8], [ESP+12], ...
    // arg0: dwCertEncodingType
    // arg1: pbCertEncoded (guest pointer)
    // arg2: cbCertEncoded (length)
    GuestAddr stack = m_cpu.sp();

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh);
    EXPECT_NE(ctx_addr, 0u);
    EXPECT_NE(m_local->global_size(ctx_addr), 0u);
}

TEST_F(Crypt32FullTest, CertCreateContextWritesCorrectStructure) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(ctx_addr, 0u);

    // Verify CERT_CONTEXT structure
    auto enc_res = m_memory.read_u32(ctx_addr + CERT_CONTEXT_OFFSET_ENCODING_TYPE);
    auto pb_res = m_memory.read_u32(ctx_addr + CERT_CONTEXT_OFFSET_PB_CERT_ENCODED);
    auto cb_res = m_memory.read_u32(ctx_addr + CERT_CONTEXT_OFFSET_CB_CERT_ENCODED);
    auto info_res = m_memory.read_u32(ctx_addr + CERT_CONTEXT_OFFSET_P_CERT_INFO);
    auto store_res = m_memory.read_u32(ctx_addr + CERT_CONTEXT_OFFSET_H_CERT_STORE);

    ASSERT_TRUE(enc_res.is_ok());
    ASSERT_TRUE(pb_res.is_ok());
    ASSERT_TRUE(cb_res.is_ok());
    ASSERT_TRUE(info_res.is_ok());
    ASSERT_TRUE(store_res.is_ok());

    // Windows keeps only the certificate half of the encoding type and gives
    // the new context a store of its own (crypt_prop_probe "ctx" row).
    EXPECT_EQ(enc_res.value(), X509_ASN_ENCODING);
    EXPECT_NE(pb_res.value(), 0u);  // Should point to cert data copy
    EXPECT_NE(pb_res.value(), cert_addr);
    EXPECT_EQ(cb_res.value(), cert_len);
    EXPECT_NE(info_res.value(), 0u);
    EXPECT_NE(store_res.value(), 0u);
}

TEST_F(Crypt32FullTest, CertCreateContextReturnsNullForInvalidArgs) {
    GuestAddr stack = m_cpu.sp();

    // Test with NULL cert pointer
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());  // NULL
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 100).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    EXPECT_EQ(m_mgr->cert_create_certificate_context(marsh), 0u);
}

TEST_F(Crypt32FullTest, CertCreateContextReturnsNullForZeroLength) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());  // Zero length

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    EXPECT_EQ(m_mgr->cert_create_certificate_context(marsh), 0u);
}

TEST_F(Crypt32FullTest, CertCreateContextReturnsNullForOversizedCert) {
    GuestAddr stack = m_cpu.sp();

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, CERT_DATA_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0x200000).is_ok());  // > 1MB limit

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);

    EXPECT_EQ(m_mgr->cert_create_certificate_context(marsh), 0u);
}

// =============================================================================
// CertFreeCertificateContext Tests
// =============================================================================

TEST_F(Crypt32BasicTest, CertDuplicateCertificateContextExportCleanup) {
    auto exports = create_crypt32_thunks(*m_mgr);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) {
            return e.name == "CertDuplicateCertificateContext";
        });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 4u);
}

TEST_F(Crypt32FullTest, CertDuplicateCertificateContextReturnsSamePointer) {
    GuestAddr stack = m_cpu.sp();
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    const GuestAddr context = m_mgr->cert_create_certificate_context(marsh);
    ASSERT_NE(context, 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, context).is_ok());
    EXPECT_EQ(m_mgr->cert_duplicate_certificate_context(marsh), context);
    EXPECT_EQ(free_cert_context(context), 1u);
    EXPECT_NE(m_local->global_size(context), 0u)
        << "the duplicate reference must keep the context alive";
    EXPECT_EQ(free_cert_context(context), 1u);
    EXPECT_EQ(m_local->global_size(context), 0u)
        << "the final free must release the context";

    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0u).is_ok());
    EXPECT_EQ(m_mgr->cert_duplicate_certificate_context(marsh), 0u);
}

// =============================================================================
// CertAddCertificateContextToStore Tests
// =============================================================================

TEST_F(Crypt32FullTest, CertAddContextToStoreReturnsTrue) {
    // First create a certificate context
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);

    GuestAddr stack = m_cpu.sp();

    // Create context
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());

    Marshaler marsh1(m_memory);
    marsh1.attach_cpu(m_cpu);

    u32 ctx_addr = m_mgr->cert_create_certificate_context(marsh1);
    ASSERT_NE(ctx_addr, 0u);

    // Open a store
    u32 store_handle = m_mgr->cert_open_store();

    // Add context to store
    // CertAddCertificateContextToStore(hCertStore, pCertContext, dwAddDisposition, ppStoreContext)
    ASSERT_TRUE(m_memory.write_u32(stack + 4, store_handle).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, ctx_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, OUTPUT_PTR_ADDR).is_ok());

    Marshaler marsh2(m_memory);
    marsh2.attach_cpu(m_cpu);

    u32 result = m_mgr->cert_add_certificate_context_to_store(marsh2);
    EXPECT_EQ(result, 1u);

    // Verify output pointer was written
    auto out_res = m_memory.read_u32(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(out_res.is_ok());
    ASSERT_NE(out_res.value(), 0u);
    EXPECT_NE(out_res.value(), ctx_addr);
    EXPECT_EQ(m_memory.read_u32(
                  out_res.value() + CERT_CONTEXT_OFFSET_H_CERT_STORE).value_or(0),
              store_handle);
}

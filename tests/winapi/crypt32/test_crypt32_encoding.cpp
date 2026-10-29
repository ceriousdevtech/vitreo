// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_encoding.cpp
/// @brief CRYPT32 string/ASN.1 encoding entry points and empty-store enumeration

#include "crypt32_test_fixture.hpp"

// =============================================================================
// Stub Function Tests
// =============================================================================

TEST_F(Crypt32FullTest, CryptStringToBinaryADecodesBase64) {
    // "AQID" base64 -> 0x01 0x02 0x03
    constexpr GuestAddr str_addr = 0x12000;
    constexpr GuestAddr out_addr = 0x12100;
    constexpr GuestAddr cb_addr = 0x12200;
    const char* b64 = "AQID";
    for (u32 i = 0; b64[i]; ++i) {
        ASSERT_TRUE(m_memory.write_u8(str_addr + i, static_cast<u8>(b64[i])).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u8(str_addr + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(cb_addr, 16).is_ok());

    m_cpu.gpr.rsp = STACK_BASE + 0x800;
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 4, static_cast<u32>(str_addr)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 8, 0).is_ok()); // cch=0 NUL-term
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 12, 1).is_ok()); // CRYPT_STRING_BASE64
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 16, static_cast<u32>(out_addr)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 20, static_cast<u32>(cb_addr)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 28, 0).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->crypt_string_to_binary_a(marsh), 1u);
    EXPECT_EQ(m_memory.read_u32(cb_addr).value(), 3u);
    EXPECT_EQ(m_memory.read_u8(out_addr).value(), 0x01u);
    EXPECT_EQ(m_memory.read_u8(out_addr + 1).value(), 0x02u);
    EXPECT_EQ(m_memory.read_u8(out_addr + 2).value(), 0x03u);
}

TEST_F(Crypt32FullTest, CryptStringToBinaryWDecodesBase64) {
    constexpr GuestAddr str_addr = 0x12000;
    constexpr GuestAddr out_addr = 0x12100;
    constexpr GuestAddr cb_addr = 0x12200;
    const char16_t* b64 = u"AQID";
    for (u32 i = 0; b64[i]; ++i) {
        ASSERT_TRUE(m_memory.write_u16(str_addr + i * 2, b64[i]).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u16(str_addr + 8, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(cb_addr, 16).is_ok());

    m_cpu.gpr.rsp = STACK_BASE + 0x800;
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 4, static_cast<u32>(str_addr)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 12, 1).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 16, static_cast<u32>(out_addr)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 20, static_cast<u32>(cb_addr)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.gpr.rsp + 28, 0).is_ok());

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->crypt_string_to_binary_w(marsh), 1u);
    EXPECT_EQ(m_memory.read_u32(cb_addr).value(), 3u);
    EXPECT_EQ(m_memory.read_u8(out_addr).value(), 0x01u);
    EXPECT_EQ(m_memory.read_u8(out_addr + 1).value(), 0x02u);
    EXPECT_EQ(m_memory.read_u8(out_addr + 2).value(), 0x03u);
}

TEST_F(Crypt32FullTest, CryptDecodeObjectExDecodesSubjectAltDnsNames) {
    // GeneralNames: SEQUENCE { [2] IA5String "a.example" }
    const u8 san[] = {
        0x30, 0x0b, 0x82, 0x09, 'a', '.', 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    };
    Marshaler marsh{m_memory};
    marsh.attach_cpu(m_cpu);

    const GuestAddr encoded = CERT_DATA_ADDR + 0x200;
    const GuestAddr out_ptr = OUTPUT_PTR_ADDR;
    const GuestAddr size_ptr = OUTPUT_PTR_ADDR + 8;
    ASSERT_TRUE(m_memory.write_bytes(encoded, san, sizeof(san)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(size_ptr, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(out_ptr, 0).is_ok());

    auto push = [&](u32 index, u32 value) {
        (void)m_memory.write_u32(
            static_cast<u32>(m_cpu.gpr.rsp + 4 + index * 4), value);
    };
    push(0, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING);
    push(1, 12); // X509_ALTERNATE_NAME
    push(2, static_cast<u32>(encoded));
    push(3, static_cast<u32>(sizeof(san)));
    push(4, 0x8000); // CRYPT_DECODE_ALLOC_FLAG
    push(5, 0);
    push(6, static_cast<u32>(out_ptr));
    push(7, static_cast<u32>(size_ptr));
    EXPECT_EQ(m_mgr->crypt_decode_object_ex(marsh), 1u);
    const GuestAddr info = m_memory.read_u32(out_ptr).value();
    ASSERT_NE(info, 0u);
    EXPECT_EQ(m_memory.read_u32(info).value(), 1u);
    const GuestAddr entry = m_memory.read_u32(info + 4).value();
    EXPECT_EQ(m_memory.read_u32(entry).value(), 3u); // CERT_ALT_NAME_DNS_NAME
    const GuestAddr name = m_memory.read_u32(entry + 4).value();
    EXPECT_EQ(m_memory.read_u16(name).value(), static_cast<u16>('a'));
}

TEST_F(Crypt32FullTest, CertFindExtensionMatchesOid) {
    Marshaler marsh{m_memory};
    marsh.attach_cpu(m_cpu);

    const char* oid = "2.5.29.17";
    const GuestAddr oid_g = CERT_DATA_ADDR + 0x400;
    for (usize i = 0; i <= std::strlen(oid); ++i) {
        ASSERT_TRUE(m_memory.write_u8(oid_g + i, static_cast<u8>(oid[i])).is_ok());
    }
    const GuestAddr other_oid = CERT_DATA_ADDR + 0x440;
    const char* other = "2.5.29.19";
    for (usize i = 0; i <= std::strlen(other); ++i) {
        ASSERT_TRUE(m_memory.write_u8(other_oid + i, static_cast<u8>(other[i])).is_ok());
    }
    const GuestAddr exts = CERT_DATA_ADDR + 0x480;
    ASSERT_TRUE(m_memory.write_u32(exts + 0, static_cast<u32>(other_oid)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(exts + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(exts + 8, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(exts + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(exts + 16, static_cast<u32>(oid_g)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(exts + 20, 1).is_ok());
    ASSERT_TRUE(m_memory.write_u32(exts + 24, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(exts + 28, 0).is_ok());

    auto push = [&](u32 index, u32 value) {
        (void)m_memory.write_u32(
            static_cast<u32>(m_cpu.gpr.rsp + 4 + index * 4), value);
    };
    push(0, static_cast<u32>(oid_g));
    push(1, 2);
    push(2, static_cast<u32>(exts));
    EXPECT_EQ(m_mgr->cert_find_extension(marsh), exts + 16);
}

TEST_F(Crypt32BasicTest, CryptEncodeObjectExReturnsZero) {
    EXPECT_EQ(m_mgr->crypt_encode_object_ex(), 0u);
}

TEST_F(Crypt32BasicTest, PfxImportCertStoreReturnsZero) {
    EXPECT_EQ(m_mgr->pfx_import_cert_store(), 0u);
}

TEST_F(Crypt32FullTest, CertEnumCertificatesInEmptyStoreReturnsZero) {
    const u32 store = m_mgr->cert_open_store();
    ASSERT_TRUE(m_memory.write_u32(m_cpu.sp() + 4, store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(m_cpu.sp() + 8, 0).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_enum_certificates_in_store(marsh), 0u);
}

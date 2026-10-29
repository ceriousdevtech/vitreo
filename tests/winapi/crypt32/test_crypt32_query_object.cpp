// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_query_object.cpp
/// @brief CryptQueryObject signer subject and key identifier exposure; the
/// message store as a store; the add-disposition last-error contract

#include "crypt32_test_fixture.hpp"

#include <winapi/system/crypt32/x509_parse.hpp>

TEST_F(Crypt32FullTest, CryptQueryObjectSignerExposesSubjectAndKeyIdentifier) {
    // Notepad++ SciLexer verification: CryptQueryObject → CERT_FIND_SUBJECT_CERT
    // materializes CERT_INFO.Subject for CertNameToStr(X500) size > 1, and
    // CertGetCertificateContextProperty(CERT_KEY_IDENTIFIER_PROP_ID).
    // The SignerInfo carries the digestEncryptionAlgorithm RFC 2315 §9.2
    // requires; without it CryptQueryObject rejects the message as Windows
    // does (it is not a decodable PKCS #7 SignedData).
    static const u8 kPkcs7[] = {
        0x30,0x82,0x01,0x1c,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x07,
        0x02,0xa0,0x82,0x01,0x0d,0x30,0x82,0x01,0x09,0x02,0x01,0x01,0x31,0x09,
        0x30,0x07,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x30,0x0b,0x06,0x09,0x2a,
        0x86,0x48,0x86,0xf7,0x0d,0x01,0x07,0x01,0xa0,0x81,0xa3,0x30,0x81,0xa0,
        0x30,0x81,0x8b,0x02,0x01,0x01,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,
        0xf7,0x0d,0x01,0x01,0x01,0x05,0x00,0x30,0x0c,0x31,0x0a,0x30,0x08,0x06,
        0x03,0x55,0x04,0x03,0x0c,0x01,0x49,0x30,0x1e,0x17,0x0d,0x32,0x35,0x30,
        0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x17,0x0d,0x33,0x35,
        0x30,0x31,0x30,0x31,0x30,0x30,0x30,0x30,0x30,0x30,0x5a,0x30,0x0f,0x31,
        0x0d,0x30,0x0b,0x06,0x03,0x55,0x04,0x03,0x0c,0x04,0x54,0x65,0x73,0x74,
        0x30,0x15,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,
        0x01,0x05,0x00,0x03,0x04,0x00,0x01,0x02,0x03,0xa3,0x21,0x30,0x1f,0x30,
        0x1d,0x06,0x03,0x55,0x1d,0x0e,0x04,0x16,0x04,0x14,0x01,0x02,0x03,0x04,
        0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,0x11,0x12,
        0x13,0x14,0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,
        0x01,0x05,0x00,0x03,0x01,0x00,0x31,0x46,0x30,0x44,0x02,0x01,0x01,0x30,
        0x11,0x30,0x0c,0x31,0x0a,0x30,0x08,0x06,0x03,0x55,0x04,0x03,0x0c,0x01,
        0x49,0x02,0x01,0x01,0x30,0x07,0x06,0x05,0x2b,0x0e,0x03,0x02,0x1a,0x30,
        0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00,
        0x04,0x14,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
        0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    };
    static const u8 kIssuerName[] = {
        0x30,0x0c,0x31,0x0a,0x30,0x08,0x06,0x03,0x55,0x04,0x03,0x0c,0x01,0x49,
    };
    static const u8 kSerial[] = {0x01};
    static const u8 kSki[] = {
        0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,
        0x0f,0x10,0x11,0x12,0x13,0x14,
    };

    char directory_template[] = "/tmp/vitreo-query-cert-XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    ASSERT_NE(directory, nullptr);
    struct ScopedDirectory {
        std::string path;
        ~ScopedDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};

    constexpr u32 kCertOff = 0x200;
    constexpr u32 kPeOff = 0x80;
    constexpr u32 kOptOff = kPeOff + 24;
    const u32 win_len = 8u + static_cast<u32>(sizeof(kPkcs7));
    const u32 win_padded = (win_len + 7u) & ~7u;
    std::vector<u8> pe(kCertOff + win_padded, 0);
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[kPeOff] = 'P';
    pe[kPeOff + 1] = 'E';
    auto write32 = [&](u32 off, u32 value) {
        pe[off] = static_cast<u8>(value);
        pe[off + 1] = static_cast<u8>(value >> 8);
        pe[off + 2] = static_cast<u8>(value >> 16);
        pe[off + 3] = static_cast<u8>(value >> 24);
    };
    auto write16 = [&](u32 off, u16 value) {
        pe[off] = static_cast<u8>(value);
        pe[off + 1] = static_cast<u8>(value >> 8);
    };
    write32(0x3c, kPeOff);
    write16(kPeOff + 20, 0xE0);
    write16(kOptOff, 0x10b);
    write32(kOptOff + 92, 16);
    write32(kOptOff + 96 + 32, kCertOff);
    write32(kOptOff + 96 + 36, win_padded);
    write32(kCertOff, win_len);
    write16(kCertOff + 4, 0x200);
    write16(kCertOff + 6, 2);
    std::copy(std::begin(kPkcs7), std::end(kPkcs7), pe.begin() + kCertOff + 8);

    {
        std::ofstream out(std::string(directory) + "/signed.dll",
                          std::ios::binary);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(pe.data()),
                              static_cast<std::streamsize>(pe.size())));
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    m_mgr->set_vfs(&vfs);

    constexpr GuestAddr path_addr = CERT_DATA_ADDR + 0x400;
    constexpr GuestAddr store_out = CERT_DATA_ADDR + 0x500;
    constexpr GuestAddr msg_out = CERT_DATA_ADDR + 0x510;
    constexpr GuestAddr find_info = CERT_DATA_ADDR + 0x600;
    constexpr GuestAddr serial_buf = CERT_DATA_ADDR + 0x700;
    constexpr GuestAddr issuer_buf = CERT_DATA_ADDR + 0x720;
    constexpr GuestAddr name_out = CERT_DATA_ADDR + 0x800;
    constexpr GuestAddr prop_out = CERT_DATA_ADDR + 0x900;
    constexpr GuestAddr prop_size = CERT_DATA_ADDR + 0x940;

    Marshaler path_writer(m_memory);
    path_writer.attach_cpu(m_cpu);
    ASSERT_TRUE(path_writer.write_wide_string(
                    path_addr, "C:\\signed.dll").is_ok());
    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 1).is_ok()); // CERT_QUERY_OBJECT_FILE
    ASSERT_TRUE(m_memory.write_u32(stack + 8, path_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0x400).is_ok()); // PKCS7 embedded
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0x2).is_ok()); // binary
    ASSERT_TRUE(m_memory.write_u32(stack + 20, 0).is_ok());
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(m_memory.write_u32(stack + 24 + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(stack + 36, store_out).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 40, msg_out).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 44, 0).is_ok());
    Marshaler query(m_memory);
    query.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->crypt_query_object(query), 1u);
    const u32 store = m_memory.read_u32(store_out).value_or(0);
    ASSERT_NE(store, 0u);

    for (u32 i = 0; i < sizeof(kSerial); ++i) {
        ASSERT_TRUE(m_memory.write_u8(serial_buf + i, kSerial[i]).is_ok());
    }
    for (u32 i = 0; i < sizeof(kIssuerName); ++i) {
        ASSERT_TRUE(m_memory.write_u8(issuer_buf + i, kIssuerName[i]).is_ok());
    }
    for (u32 i = 0; i < 64; ++i) {
        ASSERT_TRUE(m_memory.write_u32(find_info + i * 4, 0).is_ok());
    }
    // CERT_INFO (x86): SerialNumber @4, Issuer @24
    ASSERT_TRUE(m_memory.write_u32(find_info + 4,
                                  static_cast<u32>(sizeof(kSerial))).is_ok());
    ASSERT_TRUE(m_memory.write_u32(find_info + 8, serial_buf).is_ok());
    ASSERT_TRUE(m_memory.write_u32(
                    find_info + 24,
                    static_cast<u32>(sizeof(kIssuerName))).is_ok());
    ASSERT_TRUE(m_memory.write_u32(find_info + 28, issuer_buf).is_ok());

    ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, CERT_FIND_SUBJECT_CERT).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, find_info).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    Marshaler find(m_memory);
    find.attach_cpu(m_cpu);
    const u32 ctx = m_mgr->cert_find_certificate_in_query_store(find);
    ASSERT_NE(ctx, 0u);

    const u32 info =
        m_memory.read_u32(ctx + CERT_CONTEXT_OFFSET_P_CERT_INFO).value_or(0);
    ASSERT_NE(info, 0u);
    const u32 subject_cb = m_memory.read_u32(info + 48).value_or(0);
    const u32 subject_pb = m_memory.read_u32(info + 52).value_or(0);
    ASSERT_GT(subject_cb, 0u);
    ASSERT_NE(subject_pb, 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, info + 48).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 3).is_ok()); // CERT_X500_NAME_STR
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, 0).is_ok());
    Marshaler name_size(m_memory);
    name_size.attach_cpu(m_cpu);
    const u32 required = m_mgr->cert_name_to_str_w(name_size);
    ASSERT_GT(required, 1u) << "empty Subject yields size 1 (\"x509 field size\")";

    ASSERT_TRUE(m_memory.write_u32(stack + 16, name_out).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, required).is_ok());
    Marshaler name_write(m_memory);
    name_write.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_name_to_str_w(name_write), required);

    auto exports = create_crypt32_thunks(*m_mgr);
    auto prop = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& e) {
            return e.name == "CertGetCertificateContextProperty";
        });
    ASSERT_NE(prop, exports.entries.end());
    ASSERT_TRUE(m_memory.write_u32(prop_size, 32).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 4, ctx).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 20).is_ok()); // KEY_IDENTIFIER
    ASSERT_TRUE(m_memory.write_u32(stack + 12, prop_out).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, prop_size).is_ok());
    Marshaler get_prop(m_memory);
    get_prop.attach_cpu(m_cpu);
    prop->handler(get_prop);
    EXPECT_EQ(m_cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::EAX)), 1u);
    EXPECT_EQ(m_memory.read_u32(prop_size).value_or(0), sizeof(kSki));
    for (u32 i = 0; i < sizeof(kSki); ++i) {
        EXPECT_EQ(m_memory.read_u8(prop_out + i).value_or(0), kSki[i]);
    }
}

namespace {

std::vector<u8> der_tlv(u8 tag, const std::vector<u8>& body) {
    std::vector<u8> out{tag};
    const usize n = body.size();
    if (n < 0x80) {
        out.push_back(static_cast<u8>(n));
    } else if (n < 0x100) {
        out.insert(out.end(), {0x81, static_cast<u8>(n)});
    } else {
        out.insert(out.end(), {0x82, static_cast<u8>(n >> 8), static_cast<u8>(n)});
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::vector<u8> concat(std::initializer_list<std::vector<u8>> parts) {
    std::vector<u8> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

/// A PKCS #7 SignedData carrying `certificates` in that order, signed (in
/// form only) by `signer`.
std::vector<u8> signed_data(const std::vector<std::vector<u8>>& certificates,
                            const std::vector<u8>& signer) {
    const auto parsed = x509::parse_certificate(signer);
    EXPECT_TRUE(parsed.has_value());
    const std::vector<u8> sha1 = {0x30, 0x07, 0x06, 0x05, 0x2b, 0x0e, 0x03, 0x02, 0x1a};
    const std::vector<u8> rsa = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
                                 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
    const std::vector<u8> data_content = {0x30, 0x0b, 0x06, 0x09, 0x2a, 0x86, 0x48,
                                          0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01};
    const std::vector<u8> signed_data_oid = {0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
                                             0xf7, 0x0d, 0x01, 0x07, 0x02};
    std::vector<u8> certs;
    for (const auto& c : certificates) certs.insert(certs.end(), c.begin(), c.end());
    const auto signer_info = der_tlv(0x30, concat({
        {0x02, 0x01, 0x01},
        der_tlv(0x30, concat({parsed->issuer_der, der_tlv(0x02, parsed->serial)})),
        sha1, rsa, der_tlv(0x04, std::vector<u8>(20, 0)),
    }));
    const auto body = der_tlv(0x30, concat({
        {0x02, 0x01, 0x01}, der_tlv(0x31, sha1), data_content,
        der_tlv(0xa0, certs), der_tlv(0x31, signer_info),
    }));
    return der_tlv(0x30, concat({signed_data_oid, der_tlv(0xa0, body)}));
}

/// A PE32 whose security directory holds `pkcs7` as a WIN_CERTIFICATE.
std::vector<u8> signed_pe(const std::vector<u8>& pkcs7) {
    constexpr u32 kCertOff = 0x200;
    constexpr u32 kPeOff = 0x80;
    constexpr u32 kOptOff = kPeOff + 24;
    const u32 win_len = 8u + static_cast<u32>(pkcs7.size());
    const u32 win_padded = (win_len + 7u) & ~7u;
    std::vector<u8> pe(kCertOff + win_padded, 0);
    auto write32 = [&](u32 off, u32 value) {
        for (u32 i = 0; i < 4; ++i) pe[off + i] = static_cast<u8>(value >> (8 * i));
    };
    auto write16 = [&](u32 off, u16 value) {
        pe[off] = static_cast<u8>(value);
        pe[off + 1] = static_cast<u8>(value >> 8);
    };
    pe[0] = 'M';
    pe[1] = 'Z';
    pe[kPeOff] = 'P';
    pe[kPeOff + 1] = 'E';
    write32(0x3c, kPeOff);
    write16(kPeOff + 20, 0xE0);
    write16(kOptOff, 0x10b);
    write32(kOptOff + 92, 16);
    write32(kOptOff + 96 + 32, kCertOff);
    write32(kOptOff + 96 + 36, win_padded);
    write32(kCertOff, win_len);
    write16(kCertOff + 4, 0x200);
    write16(kCertOff + 6, 2);
    std::copy(pkcs7.begin(), pkcs7.end(), pe.begin() + kCertOff + 8);
    return pe;
}

std::vector<u8> bytes_of(const unsigned char* der, usize size) {
    return {der, der + size};
}

} // namespace

// authenticode_chain_probe on Spotify's signature (message order: CA, leaf):
// the CryptQueryObject store enumerates the leaf then the CA, completes the
// chain as an additional store, takes an add at its head (success leaving
// CRYPT_E_NOT_FOUND), and closing it leaves the message usable.
TEST_F(Crypt32FullTest, CryptQueryObjectStoreIsAMemoryStoreOfTheMessageCertificates) {
    const auto inter = bytes_of(kProbeIntermediateDer, sizeof(kProbeIntermediateDer));
    const auto leaf = bytes_of(kProbeLeafDer, sizeof(kProbeLeafDer));
    const auto root = bytes_of(kProbeRootDer, sizeof(kProbeRootDer));
    const auto pe = signed_pe(signed_data({inter, leaf}, leaf));

    char directory_template[] = "/tmp/vitreo-query-store-XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    ASSERT_NE(directory, nullptr);
    struct ScopedDirectory {
        std::string path;
        ~ScopedDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};
    {
        std::ofstream out(std::string(directory) + "/signed.dll", std::ios::binary);
        ASSERT_TRUE(out.write(reinterpret_cast<const char*>(pe.data()),
                              static_cast<std::streamsize>(pe.size())));
    }
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    m_mgr->set_vfs(&vfs);

    constexpr GuestAddr path_addr = CERT_DATA_ADDR + 0x400;
    constexpr GuestAddr store_out = CERT_DATA_ADDR + 0x500;
    constexpr GuestAddr msg_out = CERT_DATA_ADDR + 0x510;
    constexpr GuestAddr param_out = CERT_DATA_ADDR + 0x520;
    constexpr GuestAddr param_size = CERT_DATA_ADDR + 0x530;
    const GuestAddr stack = m_cpu.sp();
    auto args = [&](std::initializer_list<u32> values) {
        u32 at = 4;
        for (u32 v : values) {
            EXPECT_TRUE(m_memory.write_u32(stack + at, v).is_ok());
            at += 4;
        }
    };
    auto marshaler = [&] {
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        return m;
    };

    Marshaler path_writer = marshaler();
    ASSERT_TRUE(path_writer.write_wide_string(path_addr, "C:\\signed.dll").is_ok());
    args({1, static_cast<u32>(path_addr), 0x400, 0x2, 0, 0, 0, 0,
          static_cast<u32>(store_out), static_cast<u32>(msg_out), 0});
    Marshaler query = marshaler();
    ASSERT_EQ(m_mgr->crypt_query_object(query), 1u);
    const u32 store = m_memory.read_u32(store_out).value_or(0);
    const u32 msg = m_memory.read_u32(msg_out).value_or(0);
    ASSERT_NE(store, 0u);
    ASSERT_NE(msg, 0u);

    auto encoded = [&](GuestAddr context) {
        const u32 pb = m_memory.read_u32(context + CERT_CONTEXT_OFFSET_PB_CERT_ENCODED).value_or(0);
        const u32 cb = m_memory.read_u32(context + CERT_CONTEXT_OFFSET_CB_CERT_ENCODED).value_or(0);
        std::vector<u8> out(cb);
        for (u32 i = 0; i < cb; ++i) out[i] = m_memory.read_u8(pb + i).value_or(0);
        return out;
    };
    auto enumerate = [&] {
        std::vector<std::vector<u8>> out;
        GuestAddr context = 0;
        for (;;) {
            args({store, static_cast<u32>(context)});
            Marshaler m = marshaler();
            context = m_mgr->cert_enum_certificates_in_store(m);
            if (context == 0) break;
            out.push_back(encoded(context));
        }
        EXPECT_EQ(get_last_error(), CRYPT_E_NOT_FOUND);
        return out;
    };

    // "store cert" rows: newest-first, the reverse of the message order.
    EXPECT_EQ(enumerate(), (std::vector<std::vector<u8>>{leaf, inter}));

    // The store supplies the intermediate to the chain ("chain error 0",
    // three elements), with the root trusted through hExclusiveRoot.
    const u32 roots = m_mgr->cert_open_store();
    ASSERT_TRUE(m_memory.write_bytes(CERT_DATA_ADDR + 0x4000, root.data(), root.size()).is_ok());
    args({roots, X509_ASN_ENCODING, static_cast<u32>(CERT_DATA_ADDR + 0x4000),
          static_cast<u32>(root.size()), CERT_STORE_ADD_ALWAYS, 0});
    Marshaler add_root = marshaler();
    ASSERT_EQ(m_mgr->cert_add_encoded_certificate_to_store(add_root), 1u);
    constexpr GuestAddr kConfig = 0x13800;
    for (u32 off = 0; off < 0x34; off += 4) ASSERT_TRUE(m_memory.write_u32(kConfig + off, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kConfig, 0x30).is_ok());
    ASSERT_TRUE(m_memory.write_u32(kConfig + 0x28, roots).is_ok());
    args({static_cast<u32>(kConfig), static_cast<u32>(OUTPUT_PTR_ADDR + 0x300)});
    Marshaler engine_create = marshaler();
    ASSERT_EQ(m_mgr->cert_create_certificate_chain_engine(engine_create), 1u);
    const u32 engine = m_memory.read_u32(OUTPUT_PTR_ADDR + 0x300).value_or(0);
    const u32 leaf_context = create_context(leaf.data(), static_cast<u32>(leaf.size()), CERT_DATA_ADDR);
    const u32 chain = get_chain(engine, leaf_context, store);
    ASSERT_NE(chain, 0u);
    EXPECT_EQ(m_memory.read_u32(chain + CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS).value_or(1), 0u);
    const u32 simple = m_memory.read_u32(
        m_memory.read_u32(chain + CERT_CHAIN_CONTEXT_OFFSET_RGPCHAIN).value_or(0)).value_or(0);
    EXPECT_EQ(m_memory.read_u32(simple + 12).value_or(0), 3u);  // cElement

    // "add root 1 error 0x80092004", then "store after add": root first.
    const u32 root_context = create_context(root.data(), static_cast<u32>(root.size()), CERT_DATA_ADDR + 0x4000);
    args({store, root_context, CERT_STORE_ADD_NEW, 0});
    Marshaler add = marshaler();
    set_last_error(0);
    EXPECT_EQ(m_mgr->cert_add_certificate_context_to_store(add), 1u);
    EXPECT_EQ(get_last_error(), CRYPT_E_NOT_FOUND);
    EXPECT_EQ(enumerate(), (std::vector<std::vector<u8>>{root, leaf, inter}));

    // "store closed 1, message signer count 1 error 0x00000000 count 1".
    EXPECT_EQ(m_mgr->cert_close_store(store, 0), 1u);
    ASSERT_TRUE(m_memory.write_u32(param_size, 4).is_ok());
    args({msg, 5 /* CMSG_SIGNER_COUNT_PARAM */, 0, static_cast<u32>(param_out),
          static_cast<u32>(param_size)});
    Marshaler count = marshaler();
    set_last_error(0xdead);
    EXPECT_EQ(m_mgr->crypt_msg_get_param(count), 1u);
    EXPECT_EQ(m_memory.read_u32(param_out).value_or(0), 1u);
    args({msg});
    Marshaler close = marshaler();
    EXPECT_EQ(m_mgr->crypt_msg_close(close), 1u);
}

// authenticode_chain_probe "disposition" rows: a memory store holding only
// the intermediate, then CertAddCertificateContextToStore of the
// intermediate (present) or the root (absent) with each disposition.
TEST_F(Crypt32FullTest, CertAddCertificateContextToStoreDispositionLastError) {
    constexpr u32 kExists = 0x80092005u;
    struct Row {
        u32 disposition;
        bool present;
        u32 ok;
        u32 error;
        u32 count;
    };
    static constexpr Row kRows[] = {
        {1, true, 0, kExists, 1},           {1, false, 1, CRYPT_E_NOT_FOUND, 2},
        {2, true, 1, 0, 1},                 {2, false, 1, CRYPT_E_NOT_FOUND, 2},
        {3, true, 1, CRYPT_E_NOT_FOUND, 1}, {3, false, 1, CRYPT_E_NOT_FOUND, 2},
        {4, true, 1, 0, 2},                 {4, false, 1, 0, 2},
        {5, true, 1, 0, 1},                 {5, false, 1, CRYPT_E_NOT_FOUND, 2},
        {6, true, 0, kExists, 1},           {6, false, 1, CRYPT_E_NOT_FOUND, 2},
        {7, true, 0, kExists, 1},           {7, false, 1, CRYPT_E_NOT_FOUND, 2},
    };
    const u32 inter = create_context(kProbeIntermediateDer, sizeof(kProbeIntermediateDer), CERT_DATA_ADDR);
    const u32 root = create_context(kProbeRootDer, sizeof(kProbeRootDer), CERT_DATA_ADDR + 0x1000);
    const GuestAddr stack = m_cpu.sp();
    auto add = [&](u32 store, u32 context, u32 disposition) {
        EXPECT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, context).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, disposition).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        return m_mgr->cert_add_certificate_context_to_store(m);
    };
    for (const Row& row : kRows) {
        const u32 store = m_mgr->cert_open_store();
        ASSERT_EQ(add(store, inter, CERT_STORE_ADD_ALWAYS), 1u);
        set_last_error(0xdead);
        EXPECT_EQ(add(store, row.present ? inter : root, row.disposition), row.ok)
            << row.disposition << (row.present ? " present" : " absent");
        EXPECT_EQ(get_last_error(), row.error)
            << row.disposition << (row.present ? " present" : " absent");
        u32 count = 0;
        GuestAddr context = 0;
        for (;;) {
            EXPECT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
            EXPECT_TRUE(m_memory.write_u32(stack + 8, static_cast<u32>(context)).is_ok());
            Marshaler m(m_memory);
            m.attach_cpu(m_cpu);
            context = m_mgr->cert_enum_certificates_in_store(m);
            if (context == 0) break;
            ++count;
        }
        EXPECT_EQ(count, row.count) << row.disposition << (row.present ? " present" : " absent");
        EXPECT_EQ(m_mgr->cert_close_store(store, 0), 1u);
    }
}

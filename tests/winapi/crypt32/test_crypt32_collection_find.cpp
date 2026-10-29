// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_collection_find.cpp
/// @brief Registry system stores, collection stores and CertFindCertificateInStore

#include "crypt32_test_fixture.hpp"

TEST_F(Crypt32FullTest, SystemRegistryStoreOpensKnownLocations) {
    static constexpr u32 CERT_STORE_PROV_SYSTEM_REGISTRY = 13;
    static constexpr u32 CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY =
        0x00070000u;
    static constexpr u32 CERT_STORE_READONLY_FLAG = 0x00008000u;
    static constexpr u32 CERT_STORE_OPEN_EXISTING_FLAG = 0x00004000u;

    GuestAddr stack = m_cpu.sp();
    constexpr GuestAddr store_name = CERT_DATA_ADDR + 0x800;
    Marshaler open(m_memory);
    open.attach_cpu(m_cpu);
    ASSERT_TRUE(open.write_wide_string(store_name, "ROOT").is_ok());

    auto open_registry = [&](u32 location_flags) -> u32 {
        EXPECT_TRUE(m_memory.write_u32(
            stack + 4, CERT_STORE_PROV_SYSTEM_REGISTRY).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(
            stack + 16,
            location_flags | CERT_STORE_READONLY_FLAG |
                CERT_STORE_OPEN_EXISTING_FLAG)
                        .is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 20, store_name).is_ok());
        return m_mgr->cert_open_store(open);
    };

    const u32 cu = open_registry(CERT_SYSTEM_STORE_CURRENT_USER);
    ASSERT_NE(cu, 0u);
    const u32 gp = open_registry(CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY);
    ASSERT_NE(gp, 0u);
    EXPECT_EQ(m_mgr->cert_close_store(cu, 0), 1u);
    EXPECT_EQ(m_mgr->cert_close_store(gp, 0), 1u);
}

TEST_F(Crypt32FullTest, CollectionStoreLinksSiblingAndEnumerates) {
    static constexpr u32 CERT_STORE_PROV_COLLECTION = 11;
    static constexpr u32 CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG = 0x1;

    GuestAddr stack = m_cpu.sp();
    Marshaler open(m_memory);
    open.attach_cpu(m_cpu);

    // CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, NULL)
    ASSERT_TRUE(m_memory.write_u32(stack + 4, CERT_STORE_PROV_COLLECTION).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, 0).is_ok());
    const u32 collection = m_mgr->cert_open_store(open);
    ASSERT_NE(collection, 0u);

    const u32 memory = m_mgr->cert_open_store();
    ASSERT_NE(memory, 0u);

    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    Marshaler create(m_memory);
    create.attach_cpu(m_cpu);
    const u32 source_context = m_mgr->cert_create_certificate_context(create);
    ASSERT_NE(source_context, 0u);

    // Add to collection before the sibling is linked -> ACCESS_DENIED.
    ASSERT_TRUE(m_memory.write_u32(stack + 4, collection).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, source_context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    Marshaler add_blocked(m_memory);
    add_blocked.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_add_certificate_context_to_store(add_blocked), 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, collection).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, memory).is_ok());
    ASSERT_TRUE(
        m_memory.write_u32(stack + 12, CERT_PHYSICAL_STORE_ADD_ENABLE_FLAG)
            .is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 1).is_ok());
    Marshaler link(m_memory);
    link.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_add_store_to_collection(link), 1u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, collection).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, source_context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, OUTPUT_PTR_ADDR).is_ok());
    Marshaler add(m_memory);
    add.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_add_certificate_context_to_store(add), 1u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, collection).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler enumerate(m_memory);
    enumerate.attach_cpu(m_cpu);
    const u32 first = m_mgr->cert_enum_certificates_in_store(enumerate);
    ASSERT_NE(first, 0u);

    // Enumeration consumes exactly one reference to pPrevCertContext. A
    // duplicate must remain valid until its own final free.
    ASSERT_TRUE(m_memory.write_u32(stack + 4, first).is_ok());
    Marshaler duplicate(m_memory);
    duplicate.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_duplicate_certificate_context(duplicate), first);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, collection).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, first).is_ok());
    EXPECT_EQ(m_mgr->cert_enum_certificates_in_store(enumerate), 0u);
    EXPECT_NE(m_local->global_size(first), 0u);
    EXPECT_EQ(free_cert_context(first), 1u);
    EXPECT_EQ(m_local->global_size(first), 0u);

    EXPECT_EQ(m_mgr->cert_close_store(collection, 0), 1u);
    EXPECT_EQ(m_mgr->cert_close_store(memory, 0), 1u);
}

TEST_F(Crypt32FullTest, CollectionRetainsClosedSystemStoreSibling) {
    static constexpr u32 CERT_STORE_PROV_COLLECTION = 11;
    static constexpr u32 CERT_STORE_PROV_SYSTEM_REGISTRY_W = 13;

    char directory_template[] = "/tmp/vitreo-collection-system-XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    ASSERT_NE(directory, nullptr);
    struct ScopedDirectory {
        std::string path;
        ~ScopedDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};

    const auto [cert_addr, cert_len] =
        write_der_cert_with_common_name(CERT_DATA_ADDR);
    std::vector<u8> certificate(cert_len);
    ASSERT_TRUE(m_memory.read_bytes(
                    cert_addr, certificate.data(), certificate.size())
                    .is_ok());
    const auto store_dir =
        std::filesystem::path(directory) / "ProgramData" / "Vitreo" /
        "CertificateStores" / "CurrentUser" / "4D59";
    ASSERT_TRUE(std::filesystem::create_directories(store_dir));
    {
        std::ofstream out(store_dir / "0000000000000001.cer",
                          std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(certificate.data()),
                  static_cast<std::streamsize>(certificate.size()));
        ASSERT_TRUE(out.good());
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    m_mgr->set_vfs(&vfs);

    const GuestAddr stack = m_cpu.sp();
    constexpr GuestAddr store_name = CERT_DATA_ADDR + 0x800;
    Marshaler open(m_memory);
    open.attach_cpu(m_cpu);
    ASSERT_TRUE(open.write_wide_string(store_name, "MY").is_ok());
    ASSERT_TRUE(m_memory.write_u32(
        stack + 4, CERT_STORE_PROV_SYSTEM_REGISTRY_W).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(
        stack + 16, CERT_SYSTEM_STORE_CURRENT_USER).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, store_name).is_ok());
    const u32 sibling = m_mgr->cert_open_store(open);
    ASSERT_NE(sibling, 0u);

    ASSERT_TRUE(m_memory.write_u32(
        stack + 4, CERT_STORE_PROV_COLLECTION).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, 0).is_ok());
    const u32 collection = m_mgr->cert_open_store(open);
    ASSERT_NE(collection, 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, collection).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, sibling).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 1).is_ok());
    Marshaler link(m_memory);
    link.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_add_store_to_collection(link), 1u);

    // Microsoft documents that without FORCE the collection and sibling may
    // be closed in either order. The collection must retain the provider.
    ASSERT_EQ(m_mgr->cert_close_store(sibling, 0), 1u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, collection).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler enumerate(m_memory);
    enumerate.attach_cpu(m_cpu);
    const u32 first = m_mgr->cert_enum_certificates_in_store(enumerate);
    EXPECT_NE(first, 0u);
    EXPECT_EQ(m_mgr->cert_close_store(collection, 0), 1u);
}

TEST_F(Crypt32FullTest, CertFindCertificateInStoreFindsAddedContext) {
    auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    Marshaler create(m_memory);
    create.attach_cpu(m_cpu);
    const u32 source = m_mgr->cert_create_certificate_context(create);
    ASSERT_NE(source, 0u);

    const u32 store = m_mgr->cert_open_store();
    ASSERT_NE(store, 0u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, source).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    Marshaler add(m_memory);
    add.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_add_certificate_context_to_store(add), 1u);

    auto find = [&](u32 type, u32 para, u32 prev) -> u32 {
        EXPECT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 16, type).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 20, para).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 24, prev).is_ok());
        Marshaler marsh(m_memory);
        marsh.attach_cpu(m_cpu);
        return m_mgr->cert_find_certificate_in_query_store(marsh);
    };

    const u32 existing = find(CERT_FIND_EXISTING, source, 0);
    ASSERT_NE(existing, 0u);
    EXPECT_EQ(m_memory.read_u32(
                  existing + CERT_CONTEXT_OFFSET_H_CERT_STORE).value_or(0),
              store);

    const u32 info = m_memory.read_u32(
        existing + CERT_CONTEXT_OFFSET_P_CERT_INFO).value_or(0);
    ASSERT_NE(info, 0u);
    const u32 by_subject = find(CERT_FIND_SUBJECT_CERT, info, 0);
    ASSERT_NE(by_subject, 0u);

    const u32 first = find(CERT_FIND_ANY, 0, 0);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(find(CERT_FIND_ANY, 0, first), 0u);
    EXPECT_EQ(get_last_error(), CRYPT_E_NOT_FOUND);

    EXPECT_EQ(m_mgr->cert_close_store(store, 0), 1u);
}

TEST_F(Crypt32FullTest, SystemStoreFindExistingSurvivesReopen) {
    char directory_template[] = "/tmp/vitreo-cert-find-test-XXXXXX";
    char* directory = ::mkdtemp(directory_template);
    ASSERT_NE(directory, nullptr);
    struct ScopedDirectory {
        std::string path;
        ~ScopedDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{directory};

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    m_mgr->set_vfs(&vfs);

    const auto [cert_addr, cert_len] = write_minimal_der_cert(CERT_DATA_ADDR);
    const GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    Marshaler create(m_memory);
    create.attach_cpu(m_cpu);
    const u32 source_context = m_mgr->cert_create_certificate_context(create);
    ASSERT_NE(source_context, 0u);

    constexpr GuestAddr store_name = CERT_DATA_ADDR + 0x800;
    ASSERT_TRUE(create.write_wide_string(store_name, "ROOT").is_ok());
    auto open_root = [&](Crypt32Manager& manager) {
        EXPECT_TRUE(m_memory.write_u32(
            stack + 4, CERT_STORE_PROV_SYSTEM).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(
            stack + 16, CERT_SYSTEM_STORE_CURRENT_USER).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 20, store_name).is_ok());
        Marshaler open(m_memory);
        open.attach_cpu(m_cpu);
        return manager.cert_open_store(open);
    };
    const u32 first_store = open_root(*m_mgr);
    ASSERT_NE(first_store, 0u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, first_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, source_context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    Marshaler add(m_memory);
    add.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_add_certificate_context_to_store(add), 1u);
    ASSERT_EQ(m_mgr->cert_close_store(first_store, 0), 1u);

    Crypt32Manager second_manager;
    second_manager.set_vfs(&vfs);
    const u32 second_store = open_root(second_manager);
    ASSERT_NE(second_store, 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_len).is_ok());
    Marshaler recreate(m_memory);
    recreate.attach_cpu(m_cpu);
    const u32 lookup = second_manager.cert_create_certificate_context(recreate);
    ASSERT_NE(lookup, 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, second_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, CERT_FIND_EXISTING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, lookup).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    Marshaler find(m_memory);
    find.attach_cpu(m_cpu);
    const u32 found = second_manager.cert_find_certificate_in_query_store(find);
    ASSERT_NE(found, 0u) << "system store Find must see a certificate added "
                            "before CertCloseStore / CertOpenStore";
    const u32 found_len = m_memory.read_u32(
        found + CERT_CONTEXT_OFFSET_CB_CERT_ENCODED).value_or(0);
    EXPECT_EQ(found_len, cert_len);
    ASSERT_EQ(second_manager.cert_close_store(second_store, 0), 1u);
}

TEST_F(Crypt32BasicTest, CertFindCertificateInStoreReturnsZeroWithoutArgs) {
    EXPECT_EQ(m_mgr->cert_find_certificate_in_store(), 0u);
}

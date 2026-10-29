// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_system_store.cpp
/// @brief Persisted CRYPT32 system stores and host trust-anchor seeding

#include "crypt32_test_fixture.hpp"

TEST_F(Crypt32FullTest, SystemStorePersistsAcrossManagersAndDeletePersists) {
    char directory_template[] = "/tmp/vitreo-cert-store-test-XXXXXX";
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

    ASSERT_TRUE(m_memory.write_u32(stack + 4, second_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler enumerate(m_memory);
    enumerate.attach_cpu(m_cpu);
    const u32 persisted_context =
        second_manager.cert_enum_certificates_in_store(enumerate);
    ASSERT_NE(persisted_context, 0u);
    const u32 persisted_data = m_memory.read_u32(
        persisted_context + CERT_CONTEXT_OFFSET_PB_CERT_ENCODED).value_or(0);
    ASSERT_NE(persisted_data, 0u);
    ASSERT_EQ(m_memory.read_u32(
                  persisted_context + CERT_CONTEXT_OFFSET_CB_CERT_ENCODED)
                  .value_or(0),
              cert_len);
    for (u32 index = 0; index < cert_len; ++index) {
        EXPECT_EQ(m_memory.read_u8(persisted_data + index).value_or(0),
                  m_memory.read_u8(cert_addr + index).value_or(0));
    }

    ASSERT_TRUE(m_memory.write_u32(stack + 4, persisted_context).is_ok());
    Marshaler remove(m_memory);
    remove.attach_cpu(m_cpu);
    ASSERT_EQ(second_manager.cert_delete_certificate_from_store(remove), 1u);
    ASSERT_EQ(second_manager.cert_close_store(second_store, 0), 1u);

    Crypt32Manager third_manager;
    third_manager.set_vfs(&vfs);
    const u32 third_store = open_root(third_manager);
    ASSERT_NE(third_store, 0u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, third_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler final_enumerate(m_memory);
    final_enumerate.attach_cpu(m_cpu);
    EXPECT_EQ(third_manager.cert_enum_certificates_in_store(final_enumerate), 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, third_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, cert_addr).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, cert_len).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    Marshaler add_encoded(m_memory);
    add_encoded.attach_cpu(m_cpu);
    ASSERT_EQ(third_manager.cert_add_encoded_certificate_to_store(add_encoded), 1u);
    ASSERT_EQ(third_manager.cert_close_store(third_store, 0), 1u);

    Crypt32Manager fourth_manager;
    fourth_manager.set_vfs(&vfs);
    const u32 fourth_store = open_root(fourth_manager);
    ASSERT_NE(fourth_store, 0u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, fourth_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler encoded_enumerate(m_memory);
    encoded_enumerate.attach_cpu(m_cpu);
    EXPECT_NE(fourth_manager.cert_enum_certificates_in_store(encoded_enumerate),
              0u);
}

TEST_F(Crypt32FullTest, LocalMachineSystemStorePersistsSeparatelyFromCurrentUser) {
    char directory_template[] = "/tmp/vitreo-machine-cert-store-test-XXXXXX";
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

    const auto [cert_addr, cert_len] =
        write_der_cert_with_common_name(CERT_DATA_ADDR);
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
    auto open_root = [&](Crypt32Manager& manager, u32 location) {
        EXPECT_TRUE(m_memory.write_u32(stack + 4, CERT_STORE_PROV_SYSTEM).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 16, location).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 20, store_name).is_ok());
        Marshaler open(m_memory);
        open.attach_cpu(m_cpu);
        return manager.cert_open_store(open);
    };

    const u32 machine_store =
        open_root(*m_mgr, CERT_SYSTEM_STORE_LOCAL_MACHINE);
    ASSERT_NE(machine_store, 0u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, machine_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, source_context).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, CERT_STORE_ADD_ALWAYS).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    Marshaler add(m_memory);
    add.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->cert_add_certificate_context_to_store(add), 1u);
    ASSERT_EQ(m_mgr->cert_close_store(machine_store, 0), 1u);

    // LocalMachine\ROOT is seeded from host trust anchors when empty, so the
    // store may contain many certificates. Persistence of the cert we added is
    // verified on disk (full CertEnum of 100+ contexts exhausts test scratch).
    std::vector<u8> expected_der(cert_len);
    ASSERT_TRUE(m_memory.read_bytes(cert_addr, expected_der.data(), cert_len)
                    .is_ok());
    const auto root_dir =
        std::filesystem::path(directory) / "ProgramData" / "Vitreo" /
        "CertificateStores" / "LocalMachine" / "524F4F54";
    bool found_added = false;
    usize cer_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root_dir)) {
        if (entry.path().extension() != ".cer") continue;
        ++cer_count;
        std::ifstream in(entry.path(), std::ios::binary);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
        if (bytes == expected_der) found_added = true;
    }
    EXPECT_GE(cer_count, 2u); // host seed(s) + the cert added above
    EXPECT_TRUE(found_added);

    Crypt32Manager second_manager;
    second_manager.set_vfs(&vfs);
    const u32 current_user =
        open_root(second_manager, CERT_SYSTEM_STORE_CURRENT_USER);
    ASSERT_NE(current_user, 0u);
    ASSERT_TRUE(m_memory.write_u32(stack + 4, current_user).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler enumerate_user(m_memory);
    enumerate_user.attach_cpu(m_cpu);
    EXPECT_EQ(second_manager.cert_enum_certificates_in_store(enumerate_user),
              0u);
}

TEST_F(Crypt32FullTest, EmptyLocalMachineRootStoreSeedsHostTrustAnchors) {
    char directory_template[] = "/tmp/vitreo-machine-root-seed-XXXXXX";
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

    constexpr GuestAddr store_name = CERT_DATA_ADDR + 0x800;
    const GuestAddr stack = m_cpu.sp();
    Marshaler open(m_memory);
    open.attach_cpu(m_cpu);
    ASSERT_TRUE(open.write_wide_string(store_name, "ROOT").is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 4, CERT_STORE_PROV_SYSTEM).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(
        stack + 16, CERT_SYSTEM_STORE_LOCAL_MACHINE).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, store_name).is_ok());
    const u32 store = m_mgr->cert_open_store(open);
    ASSERT_NE(store, 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler enumerate(m_memory);
    enumerate.attach_cpu(m_cpu);
    const u32 first =
        m_mgr->cert_enum_certificates_in_store(enumerate);
#if defined(__APPLE__)
    EXPECT_NE(first, 0u);
#else
    (void)first;
#endif
    ASSERT_EQ(m_mgr->cert_close_store(store, 0), 1u);
}

TEST_F(Crypt32FullTest,
       RemoteCertificateContextOwnsIntermediateCertificateStore) {
    const auto [cert_addr, cert_len] =
        write_der_cert_with_common_name(CERT_DATA_ADDR);
    std::vector<u8> certificate(cert_len);
    ASSERT_TRUE(m_memory.read_bytes(
                    cert_addr, certificate.data(), certificate.size())
                    .is_ok());
    const std::array<std::vector<u8>, 2> peer_chain = {
        certificate, certificate};

    Marshaler create(m_memory);
    create.attach_cpu(m_cpu);
    const GuestAddr context =
        m_mgr->create_remote_cert_context(create, peer_chain);
    ASSERT_NE(context, 0u);

    // x86 CERT_CONTEXT::hCertStore is the final pointer-sized field at +16.
    const u32 intermediate_store =
        m_memory.read_u32(context + 16).value_or(0);
    ASSERT_NE(intermediate_store, 0u);

    const GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, intermediate_store).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
    Marshaler enumerate(m_memory);
    enumerate.attach_cpu(m_cpu);
    EXPECT_NE(m_mgr->cert_enum_certificates_in_store(enumerate), 0u);

    ASSERT_TRUE(m_memory.write_u32(stack + 4, context).is_ok());
    Marshaler release(m_memory);
    release.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->cert_free_certificate_context(release), 1u);
    EXPECT_EQ(m_mgr->cert_close_store(intermediate_store, 0), 0u)
        << "the transient intermediate store follows context lifetime";
}

TEST_F(Crypt32FullTest, NonEmptyLocalMachineRootStillSeedsHostTrustAnchors) {
    char directory_template[] = "/tmp/vitreo-machine-root-merge-XXXXXX";
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
    std::vector<u8> private_root(cert_len);
    ASSERT_TRUE(m_memory.read_bytes(
                    cert_addr, private_root.data(), private_root.size())
                    .is_ok());
    const auto root_dir =
        std::filesystem::path(directory) / "ProgramData" / "Vitreo" /
        "CertificateStores" / "LocalMachine" / "524F4F54";
    ASSERT_TRUE(std::filesystem::create_directories(root_dir));
    {
        std::ofstream out(root_dir / "0000000000000001.cer",
                          std::ios::binary);
        ASSERT_TRUE(out.good());
        out.write(reinterpret_cast<const char*>(private_root.data()),
                  static_cast<std::streamsize>(private_root.size()));
        ASSERT_TRUE(out.good());
    }

    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    m_mgr->set_vfs(&vfs);

    constexpr GuestAddr store_name = CERT_DATA_ADDR + 0x800;
    const GuestAddr stack = m_cpu.sp();
    Marshaler open(m_memory);
    open.attach_cpu(m_cpu);
    ASSERT_TRUE(open.write_wide_string(store_name, "ROOT").is_ok());
    auto open_root = [&]() {
        EXPECT_TRUE(m_memory.write_u32(
            stack + 4, CERT_STORE_PROV_SYSTEM).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(
            stack + 16, CERT_SYSTEM_STORE_LOCAL_MACHINE).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 20, store_name).is_ok());
        return m_mgr->cert_open_store(open);
    };

    const u32 first_store = open_root();
    ASSERT_NE(first_store, 0u);
    ASSERT_EQ(m_mgr->cert_close_store(first_store, 0), 1u);

    const auto certificate_count = [&]() {
        return static_cast<usize>(std::count_if(
            std::filesystem::directory_iterator(root_dir),
            std::filesystem::directory_iterator{},
            [](const auto& entry) {
                return entry.path().extension() == ".cer";
            }));
    };
    const usize merged_count = certificate_count();
#if defined(__APPLE__)
    EXPECT_GT(merged_count, 1u);
#endif

    const u32 second_store = open_root();
    ASSERT_NE(second_store, 0u);
    ASSERT_EQ(m_mgr->cert_close_store(second_store, 0), 1u);
    EXPECT_EQ(certificate_count(), merged_count)
        << "reopening ROOT must not duplicate imported anchors";
}

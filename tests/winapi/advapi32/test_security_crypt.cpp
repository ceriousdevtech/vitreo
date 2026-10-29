// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_security_crypt.cpp
/// @brief Unit tests for the advapi32 CryptoAPI provider and key stubs
///        (WIN32-062).

#include "security_stubs_fixture.hpp"

TEST_F(SecurityStubsTest, CryptBaseProviderEnumeratesAndOwnsContextLifetime) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr type_ptr = data;
    constexpr GuestAddr name_ptr = data + 0x20;
    constexpr GuestAddr size_ptr = data + 0x100;
    constexpr GuestAddr provider_ptr = data + 0x104;
    constexpr GuestAddr random_ptr = data + 0x200;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(size_ptr, 64).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, type_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, name_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(
        cpu.gpr.rsp + 24, static_cast<u32>(size_ptr)).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* enumerate = find_export(exports, "CryptEnumProvidersA");
    auto* acquire = find_export(exports, "CryptAcquireContextA");
    auto* random = find_export(exports, "CryptGenRandom");
    auto* release = find_export(exports, "CryptReleaseContext");
    ASSERT_NE(enumerate, nullptr);
    ASSERT_NE(acquire, nullptr);
    ASSERT_NE(random, nullptr);
    ASSERT_NE(release, nullptr);
    set_last_error(0xDEADu);
    enumerate->handler(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 1u);
    // CryptEnumProviders success leaves the last error untouched (measured on
    // Windows 11: last_error_on_success_advapi32_probe.expected).
    EXPECT_EQ(get_last_error(), 0xDEADu);
    EXPECT_EQ(memory.read_u32(type_ptr).value(), 1u);
    EXPECT_EQ(memory.read_u32(size_ptr).value(), 43u);
    EXPECT_EQ(
        marshaler.read_ansi_string(name_ptr).value_or(""),
        "Microsoft Base Cryptographic Provider v1.0");

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 1).is_ok());
    enumerate->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NO_MORE_ITEMS);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, name_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0xF0000040u).is_ok());
    acquire->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 provider = memory.read_u32(provider_ptr).value_or(0);
    ASSERT_NE(provider, 0u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, random_ptr).is_ok());
    random->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    bool any_nonzero = false;
    for (u32 i = 0; i < 16; ++i) {
        any_nonzero |= memory.read_u8(random_ptr + i).value_or(0) != 0;
    }
    EXPECT_TRUE(any_nonzero);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0x0BAD0000u).is_ok());
    random->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_NOACCESS);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    release->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, random_ptr).is_ok());
    random->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 0x80090001u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 1).is_ok());
    enumerate->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), 0x80090009u);
}

TEST_F(SecurityStubsTest, CryptImportKeyPublicBlobAndDestroyKey) {
    // stdcall=24 must clean six args; a missing-export stub leaves them on the
    // stack and corrupts CryptoAPI cleanup on x86 guests (TS3 Mono scripting).
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr provider_ptr = data;
    constexpr GuestAddr key_ptr = data + 0x10;
    constexpr GuestAddr blob_ptr = data + 0x20;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;

    // PUBLICKEYSTRUC + RSAPUBKEY + 8-byte modulus (toy key for handle lifecycle)
    ASSERT_TRUE(memory.write_u8(blob_ptr + 0, 0x06).is_ok());   // PUBLICKEYBLOB
    ASSERT_TRUE(memory.write_u8(blob_ptr + 1, 0x02).is_ok());   // CUR_BLOB_VERSION
    ASSERT_TRUE(memory.write_u16(blob_ptr + 2, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(blob_ptr + 4, 0x0000A400u).is_ok());  // CALG_RSA_KEYX
    // RSAPUBKEY: magic 'RSA1', bitlen=64, pubexp=65537
    ASSERT_TRUE(memory.write_u32(blob_ptr + 8, 0x31415352u).is_ok());
    ASSERT_TRUE(memory.write_u32(blob_ptr + 12, 64).is_ok());
    ASSERT_TRUE(memory.write_u32(blob_ptr + 16, 65537).is_ok());
    for (u32 i = 0; i < 8; ++i) {
        ASSERT_TRUE(memory.write_u8(blob_ptr + 20 + i, static_cast<u8>(0x11 + i)).is_ok());
    }
    constexpr u32 blob_len = 28;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* acquire = find_export(exports, "CryptAcquireContextA");
    auto* import_key = find_export(exports, "CryptImportKey");
    auto* destroy_key = find_export(exports, "CryptDestroyKey");
    auto* release = find_export(exports, "CryptReleaseContext");
    ASSERT_NE(acquire, nullptr);
    ASSERT_NE(import_key, nullptr);
    ASSERT_NE(destroy_key, nullptr);
    ASSERT_NE(release, nullptr);
    EXPECT_EQ(import_key->stdcall_bytes, 24u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0xF0000040u).is_ok());
    acquire->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 provider = memory.read_u32(provider_ptr).value_or(0);
    ASSERT_NE(provider, 0u);

    const u32 rsp_before = static_cast<u32>(cpu.gpr.rsp);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, blob_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, blob_len).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 24, key_ptr).is_ok());
    import_key->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 key = memory.read_u32(key_ptr).value_or(0);
    ASSERT_NE(key, 0u);
    // Handler itself does not adjust ESP; the thunk retimm of 24 is what
    // matters at runtime. Spot-check the export metadata instead.
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rsp), rsp_before);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, key).is_ok());
    destroy_key->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, key).is_ok());
    destroy_key->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    // Measured on real Windows: destroying an already-destroyed key reports
    // ERROR_INVALID_PARAMETER, not NTE_BAD_KEY.
    EXPECT_EQ(get_last_error(), 0x57u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    release->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(SecurityStubsTest, CryptPrivateKeyContainerExportsAndSignsHash) {
    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr provider_ptr = data;
    constexpr GuestAddr imported_key_ptr = data + 0x04;
    constexpr GuestAddr user_key_ptr = data + 0x08;
    constexpr GuestAddr public_key_ptr = data + 0x0c;
    constexpr GuestAddr hash_ptr = data + 0x10;
    constexpr GuestAddr size_ptr = data + 0x14;
    constexpr GuestAddr private_blob_ptr = data + 0x100;
    constexpr GuestAddr public_blob_ptr = data + 0x1000;
    constexpr GuestAddr digest_ptr = data + 0x2000;
    constexpr GuestAddr signature_ptr = data + 0x2100;
    constexpr GuestAddr container_ptr = data + 0x2300;
    constexpr GuestAddr container_out_ptr = data + 0x2400;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // A fixed 1024-bit PKCS#1 RSA key encoded as a CryptoAPI PRIVATEKEYBLOB.
    // Keeping the fixture in Windows' wire format exercises the endian and
    // component-layout contract rather than a host-only shortcut.
    constexpr std::string_view blob_hex =
        "0702000000240000525341320004000001000100bb704b6a4dce347cd57a04fe"
        "d97b9742e820ed698f8dcdb0573a9d0d9f659ca1c19979af5f233d769bf3e44a"
        "bbad9713efe72fe58381ff67f08cebfef6327027da9079daf06df9262389571ef"
        "355e44443e7c2fc27c751b3481454561483f66815392e745e5c69383f40313b5"
        "fb485078bb9e67a4ad8d5ba2c466b9b1c2c55bc451f2266a934f32438edcd736"
        "bd4d281ed7c89849619122117b4138817bdc28be9dd5c0dbf61d3fca8f56d8c"
        "c1812718b883471646e420f50e48896ea0985af2ff4f8802493a383aaeec04ff7"
        "89f1b15576461cd1f53dd0b6550371160f65e2eece0e4da7be0f80080402904b"
        "bfdbedb819a922847cf85f1b1cba2209ee3efc695233c7f80a9fe1596261415c"
        "c9a921f775f87504091cc1648d2ceca14ecac80d497ce139882a5fd796def51d0"
        "d96d1c9d4ecb19ed95c7102c9358a57eae1fbea3d8f90342d9ae90dc29c5af0"
        "a663b63ac2403a8425c57eb9fcbfcbf7cdb97036d8c813019f6192a4b6e1d488"
        "cd7edb28f207d2f66d4581b017c4444c97c25b70d47511bb269edd4aee72d8f5"
        "fc93f110faf23840139f90d31d05abc75157d457f42f84ac82307c523c57575d"
        "5ee3494df5c2aade8945062c246968083a5955cb12c2e359ad4850ee928480dd7"
        "c53b08a5b0ad49d5f22479d007c2181028dbf21c1f6e15be53ecc850b7d79620"
        "a0ff1a1458847b8594cd4926cb050890eeed1574ecf365a9a712a868d72c4299e"
        "94bee94e156d048fed6f3aecb005b1cf7efc65f20b858dff34ad61a075bb7d0b"
        "94b9eb502229cc6680d65326d68b7b5f5e2aa";
    auto nibble = [](char c) -> u8 {
        return static_cast<u8>(c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10);
    };
    std::vector<u8> private_blob(blob_hex.size() / 2);
    for (usize i = 0; i < private_blob.size(); ++i) {
        private_blob[i] = static_cast<u8>(
            (nibble(blob_hex[i * 2]) << 4) | nibble(blob_hex[i * 2 + 1]));
    }
    ASSERT_EQ(private_blob.size(), 596u);
    ASSERT_TRUE(memory.write_bytes(
        private_blob_ptr, private_blob.data(), private_blob.size()).is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(
        container_ptr, "Vitreo CryptoAPI Test").is_ok());

    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* acquire = find_export(exports, "CryptAcquireContextA");
    auto* import_key = find_export(exports, "CryptImportKey");
    auto* get_user_key = find_export(exports, "CryptGetUserKey");
    auto* get_prov_param = find_export(exports, "CryptGetProvParam");
    auto* export_key = find_export(exports, "CryptExportKey");
    auto* create_hash = find_export(exports, "CryptCreateHash");
    auto* set_hash = find_export(exports, "CryptSetHashParam");
    auto* sign_hash = find_export(exports, "CryptSignHashW");
    auto* verify = find_export(exports, "CryptVerifySignatureW");
    ASSERT_NE(acquire, nullptr);
    ASSERT_NE(import_key, nullptr);
    ASSERT_NE(get_user_key, nullptr);
    ASSERT_NE(get_prov_param, nullptr);
    ASSERT_NE(export_key, nullptr);
    ASSERT_NE(create_hash, nullptr);
    ASSERT_NE(set_hash, nullptr);
    ASSERT_NE(sign_hash, nullptr);
    ASSERT_NE(verify, nullptr);
    EXPECT_EQ(get_user_key->stdcall_bytes, 12u);
    EXPECT_EQ(get_prov_param->stdcall_bytes, 20u);
    EXPECT_EQ(set_hash->stdcall_bytes, 16u);
    EXPECT_EQ(sign_hash->stdcall_bytes, 24u);
    EXPECT_EQ(export_key->stdcall_bytes, 24u);

    // A named container must exist before it can be opened (flags 0 on a
    // missing one is NTE_BAD_KEYSET, advapi_capi_probe acq_open_missing):
    // delete any leftover, then create it with CRYPT_NEWKEYSET. SHA-256
    // signing needs PROV_RSA_AES (24): the PROV_RSA_FULL default provider
    // has no CALG_SHA_256 (probe hash_sha256 on type 1: NTE_BAD_ALGID).
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, container_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 24).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0x10).is_ok());   // CRYPT_DELETEKEYSET
    acquire->handler(marshaler);
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, container_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 24).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0x8).is_ok());    // CRYPT_NEWKEYSET
    acquire->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 provider = memory.read_u32(provider_ptr).value_or(0);
    ASSERT_NE(provider, 0u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, private_blob_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(
        cpu.gpr.rsp + 12, static_cast<u32>(private_blob.size())).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 24, imported_key_ptr).is_ok());
    import_key->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    ASSERT_NE(memory.read_u32(imported_key_ptr).value_or(0), 0u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, user_key_ptr).is_ok());
    get_user_key->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 user_key = memory.read_u32(user_key_ptr).value_or(0);
    ASSERT_NE(user_key, 0u);

    ASSERT_TRUE(memory.write_u32(size_ptr, 64).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 6).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, container_out_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, size_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0).is_ok());
    get_prov_param->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    EXPECT_EQ(marshaler.read_ansi_string(container_out_ptr).value_or(""),
              "Vitreo CryptoAPI Test");

    ASSERT_TRUE(memory.write_u32(size_ptr, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, user_key).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 6).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 24, size_ptr).is_ok());
    export_key->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 public_blob_size = memory.read_u32(size_ptr).value_or(0);
    ASSERT_EQ(public_blob_size, 148u);
    ASSERT_TRUE(memory.write_u32(size_ptr, public_blob_size).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, public_blob_ptr).is_ok());
    export_key->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, public_blob_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, public_blob_size).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 24, public_key_ptr).is_ok());
    import_key->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 public_key = memory.read_u32(public_key_ptr).value_or(0);
    ASSERT_NE(public_key, 0u);

    std::array<u8, 32> digest{};
    for (usize i = 0; i < digest.size(); ++i)
        digest[i] = static_cast<u8>(i * 7 + 3);
    ASSERT_TRUE(memory.write_bytes(
        digest_ptr, digest.data(), digest.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x800cu).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, hash_ptr).is_ok());
    create_hash->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 hash = memory.read_u32(hash_ptr).value_or(0);
    ASSERT_NE(hash, 0u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, hash).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, digest_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    set_hash->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(size_ptr, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, hash).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 2).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 24, size_ptr).is_ok());
    sign_hash->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    ASSERT_EQ(memory.read_u32(size_ptr).value_or(0), 128u);
    ASSERT_TRUE(memory.write_u32(size_ptr, 128).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, signature_ptr).is_ok());
    sign_hash->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, hash).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, signature_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 128).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, public_key).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 24, 0).is_ok());
    verify->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(SecurityStubsTest, CryptDeriveKeyFailsWithCorrectStdcall) {
    // stdcall=20 must clean five args; a missing-export stub leaves them on
    // the stack (same smash class as CryptImportKey before it was registered).
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* derive = find_export(exports, "CryptDeriveKey");
    ASSERT_NE(derive, nullptr);
    EXPECT_EQ(derive->stdcall_bytes, 20u);

    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x6801u).is_ok()); // CALG_RC4
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, data).is_ok());
    derive->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    // hash handle 0 -- Windows reports ERROR_INVALID_PARAMETER here, for a
    // good algid and a bad one alike. (A *valid* hash with an unsupported
    // algorithm, e.g. AES on the Base provider, is the NTE_BAD_ALGID case.)
    EXPECT_EQ(get_last_error(), 0x57u);
}

TEST_F(SecurityStubsTest, CryptGenKeySessionKeyAndDestroy) {
    auto exports = create_advapi32_security_thunks(*m_stubs);
    auto* gen_key = find_export(exports, "CryptGenKey");
    auto* acquire = find_export(exports, "CryptAcquireContextA");
    auto* destroy_key = find_export(exports, "CryptDestroyKey");
    auto* release = find_export(exports, "CryptReleaseContext");
    ASSERT_NE(gen_key, nullptr);
    ASSERT_NE(acquire, nullptr);
    ASSERT_NE(destroy_key, nullptr);
    ASSERT_NE(release, nullptr);
    EXPECT_EQ(gen_key->stdcall_bytes, 16u);

    constexpr GuestAddr data = 0x10000;
    constexpr GuestAddr stack = 0x20000;
    constexpr GuestAddr provider_ptr = data;
    constexpr GuestAddr key_ptr = data + 0x10;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        data, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        stack, 0x1000, MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack + 0x800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider_ptr).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, 1).is_ok()); // PROV_RSA_FULL
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 20, 0xF0000000u).is_ok());
    acquire->handler(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 1u);
    const u32 provider = memory.read_u32(provider_ptr).value();

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x00006801u).is_ok()); // CALG_RC4
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, 0x00000001u).is_ok()); // CRYPT_EXPORTABLE
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 16, key_ptr).is_ok());
    gen_key->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    const u32 key = memory.read_u32(key_ptr).value();
    EXPECT_NE(key, 0u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, key).is_ok());
    destroy_key->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0).is_ok());
    release->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

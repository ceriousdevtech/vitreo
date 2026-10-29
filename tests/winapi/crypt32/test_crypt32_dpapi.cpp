// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_dpapi.cpp
/// @brief CryptProtectData/CryptUnprotectData data-blob layouts and key-file permission checks

#include "crypt32_test_fixture.hpp"

TEST_F(Crypt32FullTest, DpapiRoundTripUsesX86DataBlobLayout) {
#ifdef __APPLE__
    constexpr GuestAddr input_blob = 0x12000;
    constexpr GuestAddr input_data = 0x12100;
    constexpr GuestAddr encrypted_blob = 0x12200;
    constexpr GuestAddr decrypted_blob = 0x12300;
    constexpr u8 plaintext[] = {'v', 'i', 't', 'r', 'u', 'm'};

    ASSERT_TRUE(m_memory.write_bytes(
        input_data, reinterpret_cast<const Byte*>(plaintext),
        sizeof(plaintext)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(input_blob, sizeof(plaintext)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(input_blob + 4, input_data).is_ok());

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, input_blob).is_ok());
    for (u32 index = 1; index < 6; ++index) {
        ASSERT_TRUE(m_memory.write_u32(stack + 4 + index * 4, 0).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(stack + 28, encrypted_blob).is_ok());

    Marshaler protect(m_memory);
    protect.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->crypt_protect_data(protect), 1u);

    const u32 encrypted_size = m_memory.read_u32(encrypted_blob).value();
    const u32 encrypted_data = m_memory.read_u32(encrypted_blob + 4).value();
    ASSERT_GT(encrypted_size, sizeof(plaintext));
    ASSERT_NE(encrypted_data, 0u);

    // A fresh manager must load the same file-backed key, which also proves
    // this test never falls through to an interactive Keychain lookup.
    m_mgr = std::make_unique<Crypt32Manager>();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, encrypted_blob).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 28, decrypted_blob).is_ok());
    Marshaler unprotect(m_memory);
    unprotect.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->crypt_unprotect_data(unprotect), 1u);

    EXPECT_EQ(m_memory.read_u32(decrypted_blob).value(), sizeof(plaintext));
    const GuestAddr decrypted_data = m_memory.read_u32(decrypted_blob + 4).value();
    std::array<u8, sizeof(plaintext)> round_trip{};
    ASSERT_TRUE(m_memory.read_bytes(
        decrypted_data, reinterpret_cast<Byte*>(round_trip.data()),
        round_trip.size()).is_ok());
    EXPECT_TRUE(std::equal(round_trip.begin(), round_trip.end(), plaintext));
#endif
}

TEST_F(Crypt32FullTest, DpapiRoundTripUsesX64DataBlobLayout) {
#ifdef __APPLE__
    m_cpu.init_long_mode();
    m_cpu.set_sp(STACK_BASE + 0x800);

    constexpr GuestAddr input_blob = 0x12000;
    constexpr GuestAddr input_data = 0x12100;
    constexpr GuestAddr encrypted_blob = 0x12200;
    constexpr GuestAddr decrypted_blob = 0x12300;
    constexpr u8 plaintext[] = {'c', 'e', 'f'};

    ASSERT_TRUE(m_memory.write_bytes(
        input_data, reinterpret_cast<const Byte*>(plaintext),
        sizeof(plaintext)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(input_blob, sizeof(plaintext)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(input_blob + 4, 0xDEADBEEFu).is_ok());
    ASSERT_TRUE(m_memory.write_u64(input_blob + 8, input_data).is_ok());

    m_cpu.gpr.rcx = input_blob;
    m_cpu.gpr.rdx = 0;
    m_cpu.gpr.r8 = 0;
    m_cpu.gpr.r9 = 0;
    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u64(stack + 0x28, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u64(stack + 0x30, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u64(stack + 0x38, encrypted_blob).is_ok());

    Marshaler protect(m_memory);
    protect.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->crypt_protect_data(protect), 1u);

    const u32 encrypted_size = m_memory.read_u32(encrypted_blob).value();
    const u64 encrypted_data = m_memory.read_u64(encrypted_blob + 8).value();
    ASSERT_GT(encrypted_size, sizeof(plaintext));
    ASSERT_NE(encrypted_data, 0u);

    m_cpu.gpr.rcx = encrypted_blob;
    ASSERT_TRUE(m_memory.write_u64(stack + 0x38, decrypted_blob).is_ok());
    Marshaler unprotect(m_memory);
    unprotect.attach_cpu(m_cpu);
    ASSERT_EQ(m_mgr->crypt_unprotect_data(unprotect), 1u);

    EXPECT_EQ(m_memory.read_u32(decrypted_blob).value(), sizeof(plaintext));
    const GuestAddr decrypted_data = static_cast<GuestAddr>(
        m_memory.read_u64(decrypted_blob + 8).value());
    std::array<u8, sizeof(plaintext)> round_trip{};
    ASSERT_TRUE(m_memory.read_bytes(
        decrypted_data, reinterpret_cast<Byte*>(round_trip.data()),
        round_trip.size()).is_ok());
    EXPECT_TRUE(std::equal(round_trip.begin(), round_trip.end(), plaintext));
#endif
}

TEST_F(Crypt32FullTest, DpapiRejectsGroupReadableDevelopmentKeyFile) {
#ifdef __APPLE__
    ASSERT_EQ(
        ::chmod(m_dpapi_key_file->path().c_str(),
                S_IRUSR | S_IWUSR | S_IRGRP),
        0);

    constexpr GuestAddr input_blob = 0x12000;
    constexpr GuestAddr input_data = 0x12100;
    constexpr GuestAddr output_blob = 0x12200;
    constexpr u8 plaintext[] = {'k', 'e', 'y'};
    ASSERT_TRUE(m_memory.write_bytes(
        input_data, reinterpret_cast<const Byte*>(plaintext),
        sizeof(plaintext)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(input_blob, sizeof(plaintext)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(input_blob + 4, input_data).is_ok());

    const GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, input_blob).is_ok());
    for (u32 index = 1; index < 6; ++index) {
        ASSERT_TRUE(
            m_memory.write_u32(stack + 4 + index * 4, 0).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(stack + 28, output_blob).is_ok());

    Marshaler protect(m_memory);
    protect.attach_cpu(m_cpu);
    EXPECT_EQ(m_mgr->crypt_protect_data(protect), 0u);
#endif
}

// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_thunk_abi.cpp
/// @brief CRYPT32 export-thunk argument layout and stdcall arity checks

#include "crypt32_test_fixture.hpp"

TEST_F(Crypt32FullTest, CryptQueryObjectUsesWindowsArgumentLayout) {
    auto exports = create_crypt32_thunks(*m_mgr);
    auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) { return candidate.name == "CryptQueryObject"; });
    ASSERT_NE(entry, exports.entries.end());
    EXPECT_EQ(entry->stdcall_bytes, 44u);

    constexpr GuestAddr output_base = 0x12000;
    GuestAddr stack = m_cpu.sp();
    for (u32 i = 0; i < 5; ++i) {
        ASSERT_TRUE(m_memory.write_u32(stack + 4 + i * 4, 0).is_ok());
    }
    for (u32 i = 0; i < 6; ++i) {
        GuestAddr output = output_base + i * 4;
        ASSERT_TRUE(m_memory.write_u32(output, 0xA5A50000u + i).is_ok());
        ASSERT_TRUE(m_memory.write_u32(stack + 24 + i * 4, static_cast<u32>(output)).is_ok());
    }

    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    entry->handler(marsh);

    for (u32 i = 0; i < 6; ++i) {
        auto output = m_memory.read_u32(output_base + i * 4);
        ASSERT_TRUE(output.is_ok());
        EXPECT_EQ(output.value(), 0u);
    }
    EXPECT_EQ(m_cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::EAX)), 0u);
}

TEST_F(Crypt32FullTest, WinTrustExportsUseProviderStateImplementation) {
    auto exports = create_crypt32_thunks(*m_mgr);
    auto find = [&](const char* name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
                            [&](const ExportEntry& entry) {
                                return entry.name == name;
                            });
    };
    auto verify = find("WinVerifyTrust");
    auto provider = find("WTHelperProvDataFromStateData");
    auto signer = find("WTHelperGetProvSignerFromChain");
    auto cert = find("WTHelperGetProvCertFromChain");
    ASSERT_NE(verify, exports.entries.end());
    ASSERT_NE(provider, exports.entries.end());
    ASSERT_NE(signer, exports.entries.end());
    ASSERT_NE(cert, exports.entries.end());
    EXPECT_EQ(verify->stdcall_bytes, 12u);
    EXPECT_EQ(provider->stdcall_bytes, 4u);
    EXPECT_EQ(signer->stdcall_bytes, 16u);
    EXPECT_EQ(cert->stdcall_bytes, 8u);

    // A VERIFY request with valid x86 WINTRUST_DATA/WINTRUST_FILE_INFO
    // layouts but no VFS-backed file must fail honestly. The obsolete export
    // lambda returned success for every request and could never create the
    // provider state consumed by the WTHelper functions.
    constexpr GuestAddr trust_data = 0x12000;
    constexpr GuestAddr file_info = 0x12100;
    constexpr GuestAddr path = 0x12200;
    const char16_t missing[] = u"C:\\missing.exe";
    for (u32 i = 0; i < std::size(missing); ++i) {
        ASSERT_TRUE(m_memory.write_u16(path + i * 2, missing[i]).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(trust_data + 0, 0x34).is_ok()); // cbStruct (x86)
    ASSERT_TRUE(m_memory.write_u32(trust_data + 20, 1).is_ok()); // WTD_CHOICE_FILE
    ASSERT_TRUE(m_memory.write_u32(trust_data + 24, file_info).is_ok());
    ASSERT_TRUE(m_memory.write_u32(trust_data + 28, 1).is_ok()); // WTD_STATEACTION_VERIFY
    ASSERT_TRUE(m_memory.write_u32(file_info + 4, path).is_ok());
    // WINTRUST_ACTION_GENERIC_VERIFY_V2 (softpub.h).
    constexpr GuestAddr action = 0x12300;
    const u8 v2[16] = {0x6b, 0xc5, 0xaa, 0x00, 0x44, 0xcd, 0xd0, 0x11,
                       0x8c, 0xc2, 0x00, 0xc0, 0x4f, 0xc2, 0x95, 0xee};
    ASSERT_TRUE(m_memory.write_bytes(action, v2, sizeof(v2)).is_ok());

    GuestAddr stack = m_cpu.sp();
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, action).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 12, trust_data).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    verify->handler(marsh);
    // A file that cannot be opened is CRYPT_E_FILE_ERROR, and VERIFY still
    // creates state (tools/oracle_probe/crypt_wvt_probe.x86.expected "file
    // missing").
    EXPECT_EQ(m_cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::EAX)), 0x80092003u);
    EXPECT_NE(m_memory.read_u32(trust_data + 32).value_or(0), 0u);
}

TEST_F(Crypt32FullTest, ChainEngineAndOidInstallHaveCorrectStdcallArity) {
    // Battle.net NetworkService: missing-export stubs for these APIs used
    // stdcall_bytes=0, which corrupted the x86 stack after ServerHello
    // (STATUS_ACCESS_VIOLATION near address 0x2a).
    auto exports = create_crypt32_thunks(*m_mgr);
    auto find = [&](const char* name) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
                            [&](const ExportEntry& e) { return e.name == name; });
    };
    auto install = find("CryptInstallOIDFunctionAddress");
    auto create_eng = find("CertCreateCertificateChainEngine");
    auto free_eng = find("CertFreeCertificateChainEngine");
    ASSERT_NE(install, exports.entries.end());
    ASSERT_NE(create_eng, exports.entries.end());
    ASSERT_NE(free_eng, exports.entries.end());
    EXPECT_EQ(install->stdcall_bytes, 24u);
    EXPECT_EQ(create_eng->stdcall_bytes, 8u);
    EXPECT_EQ(free_eng->stdcall_bytes, 4u);

    GuestAddr stack = m_cpu.sp();
    // An x86 CERT_CHAIN_ENGINE_CONFIG (cbSize 0x28, the base layout); a NULL
    // config faults on Windows.
    constexpr GuestAddr kConfig = 0x13800;
    for (u32 off = 0; off < 0x28; off += 4) {
        ASSERT_TRUE(m_memory.write_u32(kConfig + off, 0).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(kConfig, 0x28).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 4, kConfig).is_ok());   // pConfig
    ASSERT_TRUE(m_memory.write_u32(stack + 8, OUTPUT_PTR_ADDR).is_ok());
    ASSERT_TRUE(m_memory.write_u32(OUTPUT_PTR_ADDR, 0).is_ok());
    Marshaler marsh(m_memory);
    marsh.attach_cpu(m_cpu);
    create_eng->handler(marsh);
    EXPECT_EQ(m_cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::EAX)), 1u);
    auto eng = m_memory.read_u32(OUTPUT_PTR_ADDR);
    ASSERT_TRUE(eng.is_ok());
    EXPECT_NE(eng.value(), 0u);

    // CryptInstallOIDFunctionAddress(hModule, enc, name, count, entries, flags)
    constexpr GuestAddr kName = 0x13000;
    const char fname[] = "CryptDllEncodeObject";
    for (size_t i = 0; i <= sizeof(fname); ++i) {
        ASSERT_TRUE(
            m_memory.write_u8(kName + i, static_cast<u8>(fname[i])).is_ok());
    }
    ASSERT_TRUE(m_memory.write_u32(stack + 4, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 8, 1).is_ok()); // X509_ASN_ENCODING
    ASSERT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(kName)).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 16, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 20, 0).is_ok());
    ASSERT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
    Marshaler marsh2(m_memory);
    marsh2.attach_cpu(m_cpu);
    install->handler(marsh2);
    EXPECT_EQ(m_cpu.gpr.get32(static_cast<u8>(cpu::RegIndex::EAX)), 1u);
}

// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crypt32_test_fixture.hpp
/// @brief Shared fixtures and Windows CRYPT32 constants for the crypt32 tests.
///
/// Included by every test_crypt32*.cpp that drives Crypt32Manager. Crypt32BasicTest
/// and Crypt32FullTest are global-namespace classes defined identically in each
/// including translation unit, because every TEST_F of a suite must name the same
/// fixture type; all their members are defined in-class.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/crypt32/crypt32.hpp>
#include <winapi/system/crypt32/crypt32_internal.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/memory/global_local.hpp>
#include <winapi/kernel32/memory/memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include "../../oracle_data/crypt_chain_certs.h"
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>


using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Windows CRYPT32 Constants
// =============================================================================

// Certificate encoding types
static constexpr u32 X509_ASN_ENCODING        = 0x00000001;
static constexpr u32 PKCS_7_ASN_ENCODING      = 0x00010000;

// Certificate store provider types
static constexpr u32 CERT_STORE_PROV_MEMORY   = 2;
static constexpr u32 CERT_STORE_PROV_SYSTEM   = 10;
static constexpr u32 CERT_SYSTEM_STORE_CURRENT_USER = 0x00010000;
static constexpr u32 CERT_SYSTEM_STORE_LOCAL_MACHINE = 0x00020000;
static constexpr u32 TRUST_E_NOSIGNATURE              = 0x800B0100;

// Certificate find types (dwFindType)
static constexpr u32 CERT_FIND_ANY            = 0;
static constexpr u32 CERT_FIND_SUBJECT_CERT   = 0x000B0000;
static constexpr u32 CERT_FIND_EXISTING       = 0x000D0000;
static constexpr u32 CRYPT_E_NOT_FOUND        = 0x80092004;

// Certificate add disposition
static constexpr u32 CERT_STORE_ADD_ALWAYS    = 4;
static constexpr u32 CERT_STORE_ADD_NEW       = 1;

// Certificate chain trust status error flags
static constexpr u32 CERT_TRUST_NO_ERROR                      = 0x00000000;
static constexpr u32 CERT_TRUST_IS_NOT_TIME_VALID             = 0x00000001;
static constexpr u32 CERT_TRUST_IS_REVOKED                    = 0x00000004;
static constexpr u32 CERT_TRUST_IS_NOT_SIGNATURE_VALID        = 0x00000008;
static constexpr u32 CERT_TRUST_IS_UNTRUSTED_ROOT             = 0x00000020;
static constexpr u32 CERT_TRUST_REVOCATION_STATUS_UNKNOWN     = 0x00000040;

// Certificate chain trust status info flags
static constexpr u32 CERT_TRUST_HAS_PREFERRED_ISSUER          = 0x00000100;

// Certificate chain policy types (as numeric values)
static constexpr u32 CERT_CHAIN_POLICY_BASE                   = 1;
static constexpr u32 CERT_CHAIN_POLICY_SSL                    = 4;

// CERT_CONTEXT structure offsets (32-bit)
static constexpr u32 CERT_CONTEXT_OFFSET_ENCODING_TYPE        = 0;
static constexpr u32 CERT_CONTEXT_OFFSET_PB_CERT_ENCODED      = 4;
static constexpr u32 CERT_CONTEXT_OFFSET_CB_CERT_ENCODED      = 8;
static constexpr u32 CERT_CONTEXT_OFFSET_P_CERT_INFO          = 12;
static constexpr u32 CERT_CONTEXT_OFFSET_H_CERT_STORE         = 16;
static constexpr u32 CERT_CONTEXT_SIZE                        = 20;

// CERT_CONTEXT structure offsets (64-bit)
static constexpr u32 CERT_CONTEXT64_OFFSET_PB_CERT_ENCODED    = 8;
static constexpr u32 CERT_CONTEXT64_OFFSET_CB_CERT_ENCODED    = 16;
static constexpr u32 CERT_CONTEXT64_OFFSET_P_CERT_INFO        = 24;
static constexpr u32 CERT_CONTEXT64_OFFSET_H_CERT_STORE       = 32;

// CERT_CHAIN_CONTEXT structure offsets (32-bit)
static constexpr u32 CERT_CHAIN_CONTEXT_OFFSET_CBSIZE         = 0;
static constexpr u32 CERT_CHAIN_CONTEXT_OFFSET_ERROR_STATUS   = 4;
static constexpr u32 CERT_CHAIN_CONTEXT_OFFSET_INFO_STATUS    = 8;
static constexpr u32 CERT_CHAIN_CONTEXT_OFFSET_CCHAIN         = 12;
static constexpr u32 CERT_CHAIN_CONTEXT_OFFSET_RGPCHAIN       = 16;

// 64-bit chain layout offsets used by Steam's win64 verifier path
static constexpr u32 CERT_CHAIN_ELEMENT64_SIZE                = 56;
static constexpr u32 CERT_SIMPLE_CHAIN64_SIZE                 = 40;
static constexpr u32 CERT_CHAIN_CONTEXT64_SIZE                = 72;
static constexpr u32 CERT_CHAIN_ELEMENT64_OFFSET_CERT_CONTEXT = 8;
static constexpr u32 CERT_SIMPLE_CHAIN64_OFFSET_RGP_ELEMENT   = 16;
static constexpr u32 CERT_CHAIN_CONTEXT64_OFFSET_RGPCHAIN     = 16;

// CERT_CHAIN_POLICY_STATUS structure offsets
static constexpr u32 POLICY_STATUS_OFFSET_CBSIZE              = 0;
static constexpr u32 POLICY_STATUS_OFFSET_DWERROR             = 4;
static constexpr u32 POLICY_STATUS_OFFSET_LCHAININDEX         = 8;
static constexpr u32 POLICY_STATUS_OFFSET_LELEMENTINDEX       = 12;
static constexpr u32 POLICY_STATUS_SIZE                       = 20;

// =============================================================================
// Test Fixtures
// =============================================================================

#ifdef __APPLE__
class ScopedDpapiKeyFile {
public:
    ScopedDpapiKeyFile() {
        char path[] = "/tmp/vitreo-dpapi-test-XXXXXX";
        const int fd = ::mkstemp(path);
        if (fd < 0) return;
        m_path = path;
        std::array<u8, 32> key{};
        for (usize index = 0; index < key.size(); ++index) {
            key[index] = static_cast<u8>(index + 1);
        }
        usize offset = 0;
        while (offset < key.size()) {
            const ssize_t count =
                ::write(fd, key.data() + offset, key.size() - offset);
            if (count <= 0) break;
            offset += static_cast<usize>(count);
        }
        (void)::fchmod(fd, S_IRUSR | S_IWUSR);
        (void)::close(fd);
        if (offset != key.size()) {
            (void)::unlink(m_path.c_str());
            m_path.clear();
            return;
        }
        const char* previous = std::getenv("VITREO_DPAPI_KEY_FILE");
        if (previous != nullptr) m_previous = previous;
        m_had_previous = previous != nullptr;
        (void)::setenv("VITREO_DPAPI_KEY_FILE", m_path.c_str(), 1);
    }

    ~ScopedDpapiKeyFile() {
        if (m_had_previous) {
            (void)::setenv(
                "VITREO_DPAPI_KEY_FILE", m_previous.c_str(), 1);
        } else {
            (void)::unsetenv("VITREO_DPAPI_KEY_FILE");
        }
        if (!m_path.empty()) {
            (void)::unlink(m_path.c_str());
            const std::string lock = m_path + ".lock";
            (void)::unlink(lock.c_str());
        }
    }

    ScopedDpapiKeyFile(const ScopedDpapiKeyFile&) = delete;
    ScopedDpapiKeyFile& operator=(const ScopedDpapiKeyFile&) = delete;

    [[nodiscard]] bool valid() const noexcept { return !m_path.empty(); }
    [[nodiscard]] const std::string& path() const noexcept { return m_path; }

private:
    std::string m_path;
    std::string m_previous;
    bool m_had_previous{false};
};
#endif

/// Basic Crypt32Manager test fixture (no Marshaler)
class Crypt32BasicTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_mgr = std::make_unique<Crypt32Manager>();
    }

    std::unique_ptr<Crypt32Manager> m_mgr;
};

/// Full Crypt32Manager test fixture with Marshaler and guest memory
/// A real self-signed RSA certificate (CN=localbattle.net, CA:TRUE) whose
/// signature verifies with its own key.
static constexpr std::string_view kLocalBattleNetSelfSignedHex =
    "308201d930820142a003020102020101300d06092a864886f70d01010b0500301a"
    "3118301606035504030c0f6c6f63616c626174746c652e6e6574301e170d3235"
    "303130313030303030305a170d3335303130313030303030305a301a3118301606"
    "035504030c0f6c6f63616c626174746c652e6e657430819f300d06092a864886"
    "f70d010101050003818d0030818902818100ab027d915b8a1c89435e4fba9fb6"
    "9ab9c817c6f98c78f6c73044cefc5a408cd425bc077be6a50939ac906bca741b"
    "7cc63a623046661c6c2e02826965a348283ab50be67612e315c26493c71a15c76"
    "cb829a98768ef501ab820cfd374a1843c3d2afd86049735da3e86611196272def"
    "935ffc92bfbdc0f0eba890e5409695aa270203010001a32f302d300f0603551d13"
    "0101ff040530030101ff301a0603551d1104133011820f6c6f63616c626174746c"
    "652e6e6574300d06092a864886f70d01010b0500038181003bb22adf770a0a4a"
    "f57b4b94bdff14ce34334edc3eb635a6602b1ace350280262e7732ed893f1ac68"
    "095fb8eca304ee26dd31419b936c88d7d8595943454fa896b2329c2b438697826"
    "d5b29b79467601cd6ba94b435e263d2da0d00b83ce3007493b83fb861dff82bb"
    "3445a48e3d473d57de164b99328b69fec3d2011e67ad44";

inline std::vector<u8> decode_hex(std::string_view hex) {
    auto nibble = [](char c) -> u8 {
        return static_cast<u8>(c >= '0' && c <= '9' ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c - 'A' + 10);
    };
    std::vector<u8> bytes(hex.size() / 2);
    for (usize i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(
            nibble(hex[i * 2]) << 4 | nibble(hex[i * 2 + 1]));
    }
    return bytes;
}

class Crypt32FullTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Regression: the former environment override must not bypass trust.
        setenv("VITREO_CERT_ALWAYS_TRUST", "1", 1);

#ifdef __APPLE__
        m_dpapi_key_file = std::make_unique<ScopedDpapiKeyFile>();
        ASSERT_TRUE(m_dpapi_key_file->valid());
#endif
        m_mgr = std::make_unique<Crypt32Manager>();

        // Map scratch region for crypt32 allocations
        auto scratch_res = m_memory.map_region(
            CRYPT32_SCRATCH_BASE,
            CRYPT32_SCRATCH_SIZE,
            MemoryProtection::ReadWrite
        );
        ASSERT_TRUE(scratch_res.is_ok()) << "Failed to map scratch region";

        // Map general-purpose region for cert data
        auto gen_res = m_memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(gen_res.is_ok()) << "Failed to map general region";

        // Map stack region
        auto stack_res = m_memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok()) << "Failed to map stack region";

        // Initialize CPU in protected mode (32-bit) for stdcall convention
        m_cpu.init_protected_mode();
        m_cpu.set_sp(STACK_BASE + 0x800);

        m_vmm = std::make_unique<VirtualMemoryManager>(m_memory);
        m_handles = std::make_unique<HandleTable>();
        m_heap = std::make_unique<HeapManager>(*m_vmm, m_memory, *m_handles);
        m_local = std::make_unique<GlobalLocalManager>(*m_heap);
        m_mgr->set_local_memory(m_local.get());
    }

    /// Write a well-formed certificate (kProbeLeafDer, CN=vitrum-probe.test,
    /// valid 2021..2039) to guest memory. The former hand-made "minimal"
    /// DER lacked the signature algorithm and value, which Windows rejects
    /// with CRYPT_E_ASN1_BADTAG (crypt_prop_probe "create minimal" row).
    std::pair<GuestAddr, u32> write_minimal_der_cert(GuestAddr addr) {
        EXPECT_TRUE(m_memory.write_bytes(addr, kProbeLeafDer, sizeof(kProbeLeafDer)).is_ok());
        return {addr, static_cast<u32>(sizeof(kProbeLeafDer))};
    }

    /// A certificate valid now, so CertVerifyTimeValidity(NULL) is 0.
    std::pair<GuestAddr, u32> write_der_cert_with_validity(GuestAddr addr) {
        return write_minimal_der_cert(addr);
    }

    u32 free_cert_context(GuestAddr context) {
        EXPECT_TRUE(m_memory.write_u32(m_cpu.sp() + 4, context).is_ok());
        Marshaler marsh(m_memory);
        marsh.attach_cpu(m_cpu);
        return m_mgr->cert_free_certificate_context(marsh);
    }

    /// A self-signed certificate (kProbeRootDer, CN=Vitrum Probe Root).
    std::pair<GuestAddr, u32> write_der_cert_with_common_name(
        GuestAddr addr) {
        EXPECT_TRUE(m_memory.write_bytes(addr, kProbeRootDer, sizeof(kProbeRootDer)).is_ok());
        return {addr, static_cast<u32>(sizeof(kProbeRootDer))};
    }

    /// A CERT_CHAIN_PARA with no requested usage (cbSize for the guest's
    /// pointer width). Windows rejects a NULL pChainPara with E_INVALIDARG
    /// (crypt_chain_probe "nullpara" row).
    GuestAddr chain_para(bool is64 = false) {
        constexpr GuestAddr kChainPara = 0x13c00;
        for (u32 off = 0; off < 0x20; off += 4) {
            EXPECT_TRUE(m_memory.write_u32(kChainPara + off, 0).is_ok());
        }
        EXPECT_TRUE(m_memory.write_u32(kChainPara, is64 ? 0x20u : 0x10u).is_ok());
        return kChainPara;
    }

    u32 create_context(const u8* der, u32 size, GuestAddr at) {
        EXPECT_TRUE(m_memory.write_bytes(at, der, size).is_ok());
        const GuestAddr stack = m_cpu.sp();
        EXPECT_TRUE(m_memory.write_u32(stack + 4, X509_ASN_ENCODING).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, static_cast<u32>(at)).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, size).is_ok());
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        return static_cast<u32>(m_mgr->cert_create_certificate_context(m));
    }

    u32 get_chain(u32 engine, u32 context, u32 additional_store) {
        const GuestAddr stack = m_cpu.sp();
        const GuestAddr out = OUTPUT_PTR_ADDR + 0x200;
        EXPECT_TRUE(m_memory.write_u32(stack + 4, engine).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, context).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 16, additional_store).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 20, static_cast<u32>(chain_para())).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 28, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 32, static_cast<u32>(out)).is_ok());
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        EXPECT_EQ(m_mgr->cert_get_certificate_chain(m), 1u);
        return m_memory.read_u32(out).value_or(0);
    }

    /// kProbeLeafDer alone: its issuer is in no store (a partial chain).
    u32 build_lone_leaf_chain() {
        return get_chain(0, create_context(kProbeLeafDer, sizeof(kProbeLeafDer), CERT_DATA_ADDR), 0);
    }

    /// leaf + intermediate, root trusted by an engine's hExclusiveRoot.
    u32 build_trusted_leaf_chain() {
        const u32 roots = m_mgr->cert_open_store();
        const u32 inter = m_mgr->cert_open_store();
        const GuestAddr stack = m_cpu.sp();
        auto add = [&](u32 store, const u8* der, u32 size, GuestAddr at) {
            EXPECT_TRUE(m_memory.write_bytes(at, der, size).is_ok());
            EXPECT_TRUE(m_memory.write_u32(stack + 4, store).is_ok());
            EXPECT_TRUE(m_memory.write_u32(stack + 8, X509_ASN_ENCODING).is_ok());
            EXPECT_TRUE(m_memory.write_u32(stack + 12, static_cast<u32>(at)).is_ok());
            EXPECT_TRUE(m_memory.write_u32(stack + 16, size).is_ok());
            EXPECT_TRUE(m_memory.write_u32(stack + 20, CERT_STORE_ADD_ALWAYS).is_ok());
            EXPECT_TRUE(m_memory.write_u32(stack + 24, 0).is_ok());
            Marshaler m(m_memory);
            m.attach_cpu(m_cpu);
            EXPECT_EQ(m_mgr->cert_add_encoded_certificate_to_store(m), 1u);
        };
        add(roots, kProbeRootDer, sizeof(kProbeRootDer), CERT_DATA_ADDR + 0x4000);
        add(inter, kProbeIntermediateDer, sizeof(kProbeIntermediateDer), CERT_DATA_ADDR + 0x5000);
        // x86 CERT_CHAIN_ENGINE_CONFIG with hExclusiveRoot (cbSize 0x30).
        constexpr GuestAddr kConfig = 0x13800;
        for (u32 off = 0; off < 0x34; off += 4) {
            EXPECT_TRUE(m_memory.write_u32(kConfig + off, 0).is_ok());
        }
        EXPECT_TRUE(m_memory.write_u32(kConfig, 0x30).is_ok());
        EXPECT_TRUE(m_memory.write_u32(kConfig + 0x28, roots).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 4, static_cast<u32>(kConfig)).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, static_cast<u32>(OUTPUT_PTR_ADDR + 0x300)).is_ok());
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        EXPECT_EQ(m_mgr->cert_create_certificate_chain_engine(m), 1u);
        const u32 engine = m_memory.read_u32(OUTPUT_PTR_ADDR + 0x300).value_or(0);
        return get_chain(engine,
                         create_context(kProbeLeafDer, sizeof(kProbeLeafDer), CERT_DATA_ADDR),
                         inter);
    }

    /// CertVerifyCertificateChainPolicy with no policy para; returns the
    /// result and the five status DWORDs (pre-filled with markers).
    std::pair<u32, std::array<u32, 5>> verify_policy(u32 policy, u32 chain) {
        const GuestAddr status = OUTPUT_PTR_ADDR;
        const u32 markers[5] = {POLICY_STATUS_SIZE, 0xAAAAAAAAu, 0xBBBBBBBBu,
                                0xCCCCCCCCu, 0xDDDDDDDDu};
        for (u32 i = 0; i < 5; ++i) {
            EXPECT_TRUE(m_memory.write_u32(status + 4 * i, markers[i]).is_ok());
        }
        const GuestAddr stack = m_cpu.sp();
        EXPECT_TRUE(m_memory.write_u32(stack + 4, policy).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 8, chain).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 12, 0).is_ok());
        EXPECT_TRUE(m_memory.write_u32(stack + 16, static_cast<u32>(status)).is_ok());
        Marshaler m(m_memory);
        m.attach_cpu(m_cpu);
        const u32 ok = m_mgr->cert_verify_certificate_chain_policy(m);
        std::array<u32, 5> out{};
        for (u32 i = 0; i < 5; ++i) out[i] = m_memory.read_u32(status + 4 * i).value_or(0);
        return {ok, out};
    }

    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr CERT_DATA_ADDR = 0x10000;
    static constexpr GuestAddr OUTPUT_PTR_ADDR = 0x11000;

    std::unique_ptr<Crypt32Manager> m_mgr;
#ifdef __APPLE__
    std::unique_ptr<ScopedDpapiKeyFile> m_dpapi_key_file;
#endif
    GuestMemory m_memory;
    std::unique_ptr<VirtualMemoryManager> m_vmm;
    std::unique_ptr<HandleTable> m_handles;
    std::unique_ptr<HeapManager> m_heap;
    std::unique_ptr<GlobalLocalManager> m_local;
    cpu::CpuState m_cpu;
};

/// @file test_bcrypt.cpp
/// @brief Unit tests for bcrypt.dll cryptographic functions

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/bcrypt/bcrypt.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <array>
#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Windows-compatible constants
// =============================================================================

// Algorithm names as stored in Windows (wide strings)
static constexpr const char* BCRYPT_SHA256_ALGORITHM = "SHA256";
static constexpr const char* BCRYPT_SHA1_ALGORITHM   = "SHA1";
static constexpr const char* BCRYPT_MD5_ALGORITHM    = "MD5";
static constexpr const char* BCRYPT_RNG_ALGORITHM    = "RNG";

// NTSTATUS values
static constexpr u32 STATUS_SUCCESS           = 0x00000000;
static constexpr u32 STATUS_NOT_FOUND         = 0xC0000225;
static constexpr u32 STATUS_INVALID_HANDLE    = 0xC0000008;
static constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000D;

// Digest lengths
static constexpr u32 SHA256_DIGEST_LENGTH = 32;
static constexpr u32 SHA1_DIGEST_LENGTH   = 20;
static constexpr u32 MD5_DIGEST_LENGTH    = 16;

// =============================================================================
// Test Fixture
// =============================================================================

class BcryptManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Map memory regions for testing
        auto res = memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(res.is_ok());

        auto stack_res = memory_.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(stack_res.is_ok());

        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    // Helper to write a wide string to guest memory
    void write_wide_string(GuestAddr addr, const char* str) {
        Marshaler m(memory_);
        (void)m.write_wide_string(addr, str);
    }

    // Helper to read bytes from guest memory
    std::vector<u8> read_bytes(GuestAddr addr, size_t len) {
        std::vector<u8> buf(len);
        (void)memory_.read_bytes(addr, reinterpret_cast<Byte*>(buf.data()), len);
        return buf;
    }

    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    // Guest addresses for test data
    static constexpr GuestAddr ALG_NAME_ADDR   = MEM_BASE;
    static constexpr GuestAddr HANDLE_ADDR     = MEM_BASE + 0x100;
    static constexpr GuestAddr PROP_NAME_ADDR  = MEM_BASE + 0x200;
    static constexpr GuestAddr OUTPUT_ADDR     = MEM_BASE + 0x300;
    static constexpr GuestAddr RESULT_ADDR     = MEM_BASE + 0x400;
    static constexpr GuestAddr INPUT_ADDR      = MEM_BASE + 0x500;
    static constexpr GuestAddr BUFFER_ADDR     = MEM_BASE + 0x600;
    static constexpr GuestAddr BUFFER2_ADDR    = MEM_BASE + 0x700;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    BcryptManager mgr_;
};

// =============================================================================
// BCryptGenRandom Tests
// =============================================================================

TEST_F(BcryptManagerTest, GenRandomReturnsDifferentValues) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Generate first set of random bytes
    u32 status1 = mgr_.bcrypt_gen_random(m, 0, BUFFER_ADDR, 32, 0x2);
    EXPECT_EQ(status1, STATUS_SUCCESS);
    auto bytes1 = read_bytes(BUFFER_ADDR, 32);

    // Generate second set of random bytes
    u32 status2 = mgr_.bcrypt_gen_random(m, 0, BUFFER2_ADDR, 32, 0x2);
    EXPECT_EQ(status2, STATUS_SUCCESS);
    auto bytes2 = read_bytes(BUFFER2_ADDR, 32);

    // They should be different (statistically ~impossible to be equal)
    EXPECT_NE(bytes1, bytes2);
}

TEST_F(BcryptManagerTest, GenRandomFillsBuffer) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Zero the buffer first
    std::vector<u8> zeros(16, 0);
    (void)memory_.write_bytes(BUFFER_ADDR, reinterpret_cast<const Byte*>(zeros.data()), 16);

    u32 status = mgr_.bcrypt_gen_random(m, 0, BUFFER_ADDR, 16, 0x2);
    EXPECT_EQ(status, STATUS_SUCCESS);

    // Buffer should not be all zeros (statistically ~impossible)
    auto bytes = read_bytes(BUFFER_ADDR, 16);
    bool all_zero = std::all_of(bytes.begin(), bytes.end(), [](u8 b) { return b == 0; });
    EXPECT_FALSE(all_zero);
}

TEST_F(BcryptManagerTest, GenRandomZeroCountSucceeds) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    u32 status = mgr_.bcrypt_gen_random(m, 0, BUFFER_ADDR, 0, 0x2);
    EXPECT_EQ(status, STATUS_SUCCESS);
}

TEST_F(BcryptManagerTest, GenRandomRequiresSystemFlagOrLiveRngHandle) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    EXPECT_EQ(
        mgr_.bcrypt_gen_random(m, 0, BUFFER_ADDR, 16, 0),
        STATUS_INVALID_HANDLE);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_RNG_ALGORITHM);
    ASSERT_EQ(
        mgr_.bcrypt_open_algorithm_provider(
            m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0),
        STATUS_SUCCESS);
    const u32 handle = memory_.read_u32(HANDLE_ADDR).value();

    EXPECT_EQ(
        mgr_.bcrypt_gen_random(m, handle, BUFFER_ADDR, 16, 0),
        STATUS_SUCCESS);
    EXPECT_EQ(
        mgr_.bcrypt_close_algorithm_provider(handle, 0),
        STATUS_SUCCESS);
    EXPECT_EQ(
        mgr_.bcrypt_gen_random(m, handle, BUFFER_ADDR, 16, 0),
        STATUS_INVALID_HANDLE);
}

// Learn "CNG Algorithm Pseudo-handles": BCRYPT_RNG_ALG_HANDLE (0x81, bcrypt.h)
// is usable wherever an algorithm handle is. A pointer-sized handle with
// upper bits set is not one this provider issued.
TEST_F(BcryptManagerTest, GenRandomAcceptsRngPseudoHandleAndFullWidthHandles) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    std::vector<u8> zeros(32, 0);
    (void)memory_.write_bytes(BUFFER_ADDR, zeros.data(), zeros.size());
    EXPECT_EQ(mgr_.bcrypt_gen_random(m, 0x81, BUFFER_ADDR, 32, 0), STATUS_SUCCESS);
    EXPECT_NE(read_bytes(BUFFER_ADDR, 32), zeros);
    // SHA256 pseudo-handle (0x41) is a handle, just not an RNG one.
    EXPECT_EQ(mgr_.bcrypt_gen_random(m, 0x41, BUFFER_ADDR, 32, 0), STATUS_INVALID_HANDLE);

    auto exports = create_bcrypt_thunks(mgr_);
    const ExportEntry* gen = nullptr;
    for (const auto& e : exports.entries) {
        if (e.name == "BCryptGenRandom") gen = &e;
    }
    ASSERT_NE(gen, nullptr);
    cpu_.init_long_mode();
    cpu_.gpr.rsp = STACK_BASE + 0x800;
    cpu_.gpr.rcx = 0x0000000100000081ull; // upper bits set: not 0x81
    cpu_.gpr.rdx = BUFFER_ADDR;
    cpu_.gpr.r8 = 16;
    cpu_.gpr.r9 = 0;
    Marshaler call(memory_);
    call.attach_cpu(cpu_);
    gen->handler(call);
    EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax), STATUS_INVALID_HANDLE);
}

// =============================================================================
// BCryptOpenAlgorithmProvider Tests
// =============================================================================

TEST_F(BcryptManagerTest, OpenAlgorithmProviderSHA256Succeeds) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);

    u32 status = mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);

    u32 handle = memory_.read_u32(HANDLE_ADDR).value();
    EXPECT_NE(handle, 0u);
}

// pszImplementation: NULL, "" and the primitive provider's name (any case)
// select it; any other name is STATUS_NOT_FOUND with *phAlgorithm untouched
// (tools/oracle_probe/sspi_bcrypt_probe.expected, Windows 11 x86/x64).
TEST_F(BcryptManagerTest, OpenAlgorithmProviderHonoursImplementationName) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    const struct { const char* impl; u32 status; } cases[] = {
        {"Microsoft Primitive Provider", STATUS_SUCCESS},
        {"microsoft primitive provider", STATUS_SUCCESS},
        {"", STATUS_SUCCESS},
        {"Bogus Provider", STATUS_NOT_FOUND},
        {"Microsoft Smart Card Key Storage Provider", STATUS_NOT_FOUND},
    };
    for (const auto& c : cases) {
        write_wide_string(PROP_NAME_ADDR, c.impl);
        ASSERT_TRUE(memory_.write_u32(HANDLE_ADDR, 0x5A5A5A5A).is_ok());
        EXPECT_EQ(mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR,
                                                      PROP_NAME_ADDR, 0),
                  c.status) << c.impl;
        const u32 handle = memory_.read_u32(HANDLE_ADDR).value();
        if (c.status == STATUS_SUCCESS) {
            EXPECT_NE(handle, 0x5A5A5A5Au);
            EXPECT_EQ(mgr_.bcrypt_close_algorithm_provider(handle, 0), STATUS_SUCCESS);
        } else {
            EXPECT_EQ(handle, 0x5A5A5A5Au);
        }
    }
}

TEST_F(BcryptManagerTest, OpenAlgorithmProviderSHA1Succeeds) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA1_ALGORITHM);

    u32 status = mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);

    u32 handle = memory_.read_u32(HANDLE_ADDR).value();
    EXPECT_NE(handle, 0u);
}

TEST_F(BcryptManagerTest, OpenAlgorithmProviderMD5Succeeds) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_MD5_ALGORITHM);

    u32 status = mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);

    u32 handle = memory_.read_u32(HANDLE_ADDR).value();
    EXPECT_NE(handle, 0u);
}

TEST_F(BcryptManagerTest, OpenAlgorithmProviderRngSucceeds) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_RNG_ALGORITHM);

    const u32 status = mgr_.bcrypt_open_algorithm_provider(
        m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);
    EXPECT_NE(memory_.read_u32(HANDLE_ADDR).value(), 0u);
}

TEST_F(BcryptManagerTest, OpenAlgorithmProviderInvalidAlgorithmFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, "INVALID_ALG");

    u32 status = mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    EXPECT_EQ(status, STATUS_NOT_FOUND);
}

TEST_F(BcryptManagerTest, OpenAlgorithmProviderNullHandleFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);

    u32 status = mgr_.bcrypt_open_algorithm_provider(m, 0, ALG_NAME_ADDR, 0, 0);
    EXPECT_EQ(status, STATUS_INVALID_PARAMETER);
}

TEST_F(BcryptManagerTest, OpenAlgorithmProviderNullAlgIdFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    u32 status = mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, 0, 0, 0);
    EXPECT_EQ(status, STATUS_INVALID_PARAMETER);
}

// =============================================================================
// BCryptCloseAlgorithmProvider Tests
// =============================================================================

TEST_F(BcryptManagerTest, CloseAlgorithmProviderValidHandleSucceeds) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Open first to get a valid handle
    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 handle = memory_.read_u32(HANDLE_ADDR).value();

    u32 status = mgr_.bcrypt_close_algorithm_provider(handle, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);
    EXPECT_EQ(
        mgr_.bcrypt_close_algorithm_provider(handle, 0),
        STATUS_INVALID_HANDLE);
}

TEST_F(BcryptManagerTest, CloseAlgorithmProviderInvalidHandleFails) {
    u32 status = mgr_.bcrypt_close_algorithm_provider(0xBADBAD, 0);
    EXPECT_EQ(status, STATUS_INVALID_HANDLE);
}

// =============================================================================
// BCryptGetProperty Tests
// =============================================================================

TEST_F(BcryptManagerTest, GetPropertySHA256DigestLength) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Open SHA256 algorithm
    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    // Query HashDigestLength property
    write_wide_string(PROP_NAME_ADDR, "HashDigestLength");

    u32 status = mgr_.bcrypt_get_property(m, alg_handle, PROP_NAME_ADDR,
                                           OUTPUT_ADDR, 4, RESULT_ADDR, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);

    u32 digest_len = memory_.read_u32(OUTPUT_ADDR).value();
    EXPECT_EQ(digest_len, SHA256_DIGEST_LENGTH);

    u32 result_size = memory_.read_u32(RESULT_ADDR).value();
    EXPECT_EQ(result_size, 4u);
}

TEST_F(BcryptManagerTest, GetPropertySHA1DigestLength) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA1_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    write_wide_string(PROP_NAME_ADDR, "HashDigestLength");

    u32 status = mgr_.bcrypt_get_property(m, alg_handle, PROP_NAME_ADDR,
                                           OUTPUT_ADDR, 4, RESULT_ADDR, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);

    u32 digest_len = memory_.read_u32(OUTPUT_ADDR).value();
    EXPECT_EQ(digest_len, SHA1_DIGEST_LENGTH);
}

TEST_F(BcryptManagerTest, GetPropertyMD5DigestLength) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_MD5_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    write_wide_string(PROP_NAME_ADDR, "HashDigestLength");

    u32 status = mgr_.bcrypt_get_property(m, alg_handle, PROP_NAME_ADDR,
                                           OUTPUT_ADDR, 4, RESULT_ADDR, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);

    u32 digest_len = memory_.read_u32(OUTPUT_ADDR).value();
    EXPECT_EQ(digest_len, MD5_DIGEST_LENGTH);
}

TEST_F(BcryptManagerTest, GetPropertyInvalidPropertyFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    write_wide_string(PROP_NAME_ADDR, "InvalidProperty");

    u32 status = mgr_.bcrypt_get_property(m, alg_handle, PROP_NAME_ADDR,
                                           OUTPUT_ADDR, 4, RESULT_ADDR, 0);
    EXPECT_EQ(status, STATUS_NOT_FOUND);
}

TEST_F(BcryptManagerTest, GetPropertyInvalidHandleFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(PROP_NAME_ADDR, "HashDigestLength");

    u32 status = mgr_.bcrypt_get_property(m, 0xBADBAD, PROP_NAME_ADDR,
                                           OUTPUT_ADDR, 4, RESULT_ADDR, 0);
    EXPECT_EQ(status, STATUS_INVALID_HANDLE);
}

// =============================================================================
// BCryptCreateHash Tests
// =============================================================================

TEST_F(BcryptManagerTest, CreateHashSHA256Succeeds) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    u32 status = mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    EXPECT_EQ(status, STATUS_SUCCESS);

    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();
    EXPECT_NE(hash_handle, 0u);
}

TEST_F(BcryptManagerTest, CreateHashInvalidAlgHandleFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    u32 status = mgr_.bcrypt_create_hash(m, 0xBADBAD, hash_handle_addr, 0, 0, 0, 0, 0);
    EXPECT_EQ(status, STATUS_INVALID_HANDLE);
}

TEST_F(BcryptManagerTest, CreateHashNullOutputHandleFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    u32 status = mgr_.bcrypt_create_hash(m, alg_handle, 0, 0, 0, 0, 0, 0);
    EXPECT_EQ(status, STATUS_INVALID_PARAMETER);
}

// =============================================================================
// Full Hash Workflow Tests
// =============================================================================

TEST_F(BcryptManagerTest, FullHashWorkflowSHA256) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Open algorithm
    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    u32 status = mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    ASSERT_EQ(status, STATUS_SUCCESS);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    // Create hash
    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    status = mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    ASSERT_EQ(status, STATUS_SUCCESS);
    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();

    // Hash data: "test"
    const char* data = "test";
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>(data), 4);
    status = mgr_.bcrypt_hash_data(m, hash_handle, INPUT_ADDR, 4, 0);
    ASSERT_EQ(status, STATUS_SUCCESS);

    // Finish hash
    status = mgr_.bcrypt_finish_hash(m, hash_handle, OUTPUT_ADDR, SHA256_DIGEST_LENGTH, 0);
    ASSERT_EQ(status, STATUS_SUCCESS);

    // Destroy hash
    status = mgr_.bcrypt_destroy_hash(hash_handle);
    ASSERT_EQ(status, STATUS_SUCCESS);

    // Close algorithm
    status = mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
    ASSERT_EQ(status, STATUS_SUCCESS);
}

// =============================================================================
// Known Hash Value Tests
// =============================================================================

TEST_F(BcryptManagerTest, SHA256KnownValue) {
    // SHA256("test") = 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
    static const u8 expected_sha256[] = {
        0x9f, 0x86, 0xd0, 0x81, 0x88, 0x4c, 0x7d, 0x65,
        0x9a, 0x2f, 0xea, 0xa0, 0xc5, 0x5a, 0xd0, 0x15,
        0xa3, 0xbf, 0x4f, 0x1b, 0x2b, 0x0b, 0x82, 0x2c,
        0xd1, 0x5d, 0x6c, 0x15, 0xb0, 0xf0, 0x0a, 0x08
    };

    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Open algorithm
    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    // Create hash
    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();

    // Hash "test"
    const char* data = "test";
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>(data), 4);
    mgr_.bcrypt_hash_data(m, hash_handle, INPUT_ADDR, 4, 0);

    // Finish
    mgr_.bcrypt_finish_hash(m, hash_handle, OUTPUT_ADDR, SHA256_DIGEST_LENGTH, 0);

    // Verify
    auto digest = read_bytes(OUTPUT_ADDR, SHA256_DIGEST_LENGTH);
    EXPECT_EQ(digest.size(), SHA256_DIGEST_LENGTH);
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        EXPECT_EQ(digest[i], expected_sha256[i]) << "Mismatch at byte " << i;
    }

    mgr_.bcrypt_destroy_hash(hash_handle);
    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

TEST_F(BcryptManagerTest, SHA1KnownValue) {
    // SHA1("test") = a94a8fe5ccb19ba61c4c0873d391e987982fbbd3
    static const u8 expected_sha1[] = {
        0xa9, 0x4a, 0x8f, 0xe5, 0xcc, 0xb1, 0x9b, 0xa6,
        0x1c, 0x4c, 0x08, 0x73, 0xd3, 0x91, 0xe9, 0x87,
        0x98, 0x2f, 0xbb, 0xd3
    };

    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Open algorithm
    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA1_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    // Create hash
    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();

    // Hash "test"
    const char* data = "test";
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>(data), 4);
    mgr_.bcrypt_hash_data(m, hash_handle, INPUT_ADDR, 4, 0);

    // Finish
    mgr_.bcrypt_finish_hash(m, hash_handle, OUTPUT_ADDR, SHA1_DIGEST_LENGTH, 0);

    // Verify
    auto digest = read_bytes(OUTPUT_ADDR, SHA1_DIGEST_LENGTH);
    EXPECT_EQ(digest.size(), SHA1_DIGEST_LENGTH);
    for (size_t i = 0; i < SHA1_DIGEST_LENGTH; ++i) {
        EXPECT_EQ(digest[i], expected_sha1[i]) << "Mismatch at byte " << i;
    }

    mgr_.bcrypt_destroy_hash(hash_handle);
    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

TEST_F(BcryptManagerTest, MD5KnownValue) {
    // MD5("test") = 098f6bcd4621d373cade4e832627b4f6
    static const u8 expected_md5[] = {
        0x09, 0x8f, 0x6b, 0xcd, 0x46, 0x21, 0xd3, 0x73,
        0xca, 0xde, 0x4e, 0x83, 0x26, 0x27, 0xb4, 0xf6
    };

    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // Open algorithm
    write_wide_string(ALG_NAME_ADDR, BCRYPT_MD5_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    // Create hash
    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();

    // Hash "test"
    const char* data = "test";
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>(data), 4);
    mgr_.bcrypt_hash_data(m, hash_handle, INPUT_ADDR, 4, 0);

    // Finish
    mgr_.bcrypt_finish_hash(m, hash_handle, OUTPUT_ADDR, MD5_DIGEST_LENGTH, 0);

    // Verify
    auto digest = read_bytes(OUTPUT_ADDR, MD5_DIGEST_LENGTH);
    EXPECT_EQ(digest.size(), MD5_DIGEST_LENGTH);
    for (size_t i = 0; i < MD5_DIGEST_LENGTH; ++i) {
        EXPECT_EQ(digest[i], expected_md5[i]) << "Mismatch at byte " << i;
    }

    mgr_.bcrypt_destroy_hash(hash_handle);
    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

TEST_F(BcryptManagerTest, SHA256EmptyString) {
    // SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    static const u8 expected[] = {
        0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
        0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
        0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
        0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55
    };

    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();

    // Hash empty string (don't call hash_data at all, or call with 0 length)
    mgr_.bcrypt_hash_data(m, hash_handle, INPUT_ADDR, 0, 0);
    mgr_.bcrypt_finish_hash(m, hash_handle, OUTPUT_ADDR, SHA256_DIGEST_LENGTH, 0);

    auto digest = read_bytes(OUTPUT_ADDR, SHA256_DIGEST_LENGTH);
    for (size_t i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        EXPECT_EQ(digest[i], expected[i]) << "Mismatch at byte " << i;
    }

    mgr_.bcrypt_destroy_hash(hash_handle);
    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

// =============================================================================
// Incremental Hashing Tests
// =============================================================================

TEST_F(BcryptManagerTest, IncrementalHashProducesSameResult) {
    // Hash "test" all at once vs "te" then "st" should produce same result
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    // First: hash "test" all at once
    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    u32 hash_handle1 = memory_.read_u32(hash_handle_addr).value();

    const char* data = "test";
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>(data), 4);
    mgr_.bcrypt_hash_data(m, hash_handle1, INPUT_ADDR, 4, 0);
    mgr_.bcrypt_finish_hash(m, hash_handle1, OUTPUT_ADDR, SHA256_DIGEST_LENGTH, 0);
    auto digest1 = read_bytes(OUTPUT_ADDR, SHA256_DIGEST_LENGTH);
    mgr_.bcrypt_destroy_hash(hash_handle1);

    // Second: hash "te" then "st" incrementally
    GuestAddr hash_handle_addr2 = MEM_BASE + 0x900;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr2, 0, 0, 0, 0, 0);
    u32 hash_handle2 = memory_.read_u32(hash_handle_addr2).value();

    // Hash "te"
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>("te"), 2);
    mgr_.bcrypt_hash_data(m, hash_handle2, INPUT_ADDR, 2, 0);

    // Hash "st"
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>("st"), 2);
    mgr_.bcrypt_hash_data(m, hash_handle2, INPUT_ADDR, 2, 0);

    GuestAddr output2_addr = MEM_BASE + 0xA00;
    mgr_.bcrypt_finish_hash(m, hash_handle2, output2_addr, SHA256_DIGEST_LENGTH, 0);
    auto digest2 = read_bytes(output2_addr, SHA256_DIGEST_LENGTH);
    mgr_.bcrypt_destroy_hash(hash_handle2);

    // Both should be equal
    EXPECT_EQ(digest1, digest2);

    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

// =============================================================================
// Error Handling Tests
// =============================================================================

TEST_F(BcryptManagerTest, HashDataInvalidHandleFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>("test"), 4);
    u32 status = mgr_.bcrypt_hash_data(m, 0xBADBAD, INPUT_ADDR, 4, 0);
    EXPECT_EQ(status, STATUS_INVALID_HANDLE);
}

TEST_F(BcryptManagerTest, FinishHashInvalidHandleFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    u32 status = mgr_.bcrypt_finish_hash(m, 0xBADBAD, OUTPUT_ADDR, 32, 0);
    EXPECT_EQ(status, STATUS_INVALID_HANDLE);
}

TEST_F(BcryptManagerTest, FinishHashBufferTooSmallFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();

    // Try to finish with buffer too small (SHA256 needs 32 bytes)
    u32 status = mgr_.bcrypt_finish_hash(m, hash_handle, OUTPUT_ADDR, 16, 0);
    EXPECT_EQ(status, STATUS_INVALID_PARAMETER);

    mgr_.bcrypt_destroy_hash(hash_handle);
    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

TEST_F(BcryptManagerTest, DestroyHashInvalidHandleFails) {
    u32 status = mgr_.bcrypt_destroy_hash(0xBADBAD);
    EXPECT_EQ(status, STATUS_INVALID_HANDLE);
}

TEST_F(BcryptManagerTest, DestroyHashTwiceFails) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    GuestAddr hash_handle_addr = MEM_BASE + 0x800;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr, 0, 0, 0, 0, 0);
    u32 hash_handle = memory_.read_u32(hash_handle_addr).value();

    // First destroy should succeed
    u32 status = mgr_.bcrypt_destroy_hash(hash_handle);
    EXPECT_EQ(status, STATUS_SUCCESS);

    // Second destroy should fail
    status = mgr_.bcrypt_destroy_hash(hash_handle);
    EXPECT_EQ(status, STATUS_INVALID_HANDLE);

    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

// =============================================================================
// Multiple Hash Contexts Test
// =============================================================================

TEST_F(BcryptManagerTest, MultipleHashContextsAreIndependent) {
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_wide_string(ALG_NAME_ADDR, BCRYPT_SHA256_ALGORITHM);
    mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0);
    u32 alg_handle = memory_.read_u32(HANDLE_ADDR).value();

    // Create two hash contexts
    GuestAddr hash_handle_addr1 = MEM_BASE + 0x800;
    GuestAddr hash_handle_addr2 = MEM_BASE + 0x810;
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr1, 0, 0, 0, 0, 0);
    mgr_.bcrypt_create_hash(m, alg_handle, hash_handle_addr2, 0, 0, 0, 0, 0);
    u32 hash1 = memory_.read_u32(hash_handle_addr1).value();
    u32 hash2 = memory_.read_u32(hash_handle_addr2).value();

    // Hash different data into each
    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>("hello"), 5);
    mgr_.bcrypt_hash_data(m, hash1, INPUT_ADDR, 5, 0);

    (void)memory_.write_bytes(INPUT_ADDR, reinterpret_cast<const Byte*>("world"), 5);
    mgr_.bcrypt_hash_data(m, hash2, INPUT_ADDR, 5, 0);

    // Finish both
    GuestAddr output1_addr = MEM_BASE + 0x900;
    GuestAddr output2_addr = MEM_BASE + 0x940;
    mgr_.bcrypt_finish_hash(m, hash1, output1_addr, SHA256_DIGEST_LENGTH, 0);
    mgr_.bcrypt_finish_hash(m, hash2, output2_addr, SHA256_DIGEST_LENGTH, 0);

    auto digest1 = read_bytes(output1_addr, SHA256_DIGEST_LENGTH);
    auto digest2 = read_bytes(output2_addr, SHA256_DIGEST_LENGTH);

    // They should be different (different inputs)
    EXPECT_NE(digest1, digest2);

    mgr_.bcrypt_destroy_hash(hash1);
    mgr_.bcrypt_destroy_hash(hash2);
    mgr_.bcrypt_close_algorithm_provider(alg_handle, 0);
}

// Source: Windows x86/x64 bcrypt_set_property_probe{32,64}.expected.
TEST_F(BcryptManagerTest, SetPropertyValidatesInputsAndPreservesState) {
    Marshaler m(memory_); m.attach_cpu(cpu_);
    write_wide_string(ALG_NAME_ADDR, "AES");
    ASSERT_EQ(mgr_.bcrypt_open_algorithm_provider(m, HANDLE_ADDR, ALG_NAME_ADDR, 0, 0), STATUS_SUCCESS);
    const u32 h = memory_.read_u32(HANDLE_ADDR).value();
    write_wide_string(PROP_NAME_ADDR, "ChainingMode");
    write_wide_string(INPUT_ADDR, "ChainingModeCBC");
    // Learn explicitly says cbInput does not bound this string.
    EXPECT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_SUCCESS);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,1),STATUS_INVALID_PARAMETER);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,h,0,INPUT_ADDR,0,0),STATUS_INVALID_PARAMETER);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,0,0,0),STATUS_INVALID_PARAMETER);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,0,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_INVALID_HANDLE);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,0,0,0,0,0),STATUS_INVALID_PARAMETER);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,0x41,PROP_NAME_ADDR,INPUT_ADDR,0,0),0xc0000022u);
    write_wide_string(INPUT_ADDR,"bad");
    EXPECT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_INVALID_PARAMETER);
    write_wide_string(PROP_NAME_ADDR,"BlockLength");
    EXPECT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,0),0xc00000bbu);
    write_wide_string(PROP_NAME_ADDR,"ChainingMode");
    for(const char* mode : {"ChainingModeCFB","ChainingModeCCM","ChainingModeGCM"}) {
        write_wide_string(INPUT_ADDR,mode);
        EXPECT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,0),0xc00000bbu);
    }
    write_wide_string(ALG_NAME_ADDR,"SHA256");
    ASSERT_EQ(mgr_.bcrypt_open_algorithm_provider(m,HANDLE_ADDR,ALG_NAME_ADDR,0,0),STATUS_SUCCESS);
    const u32 sha=memory_.read_u32(HANDLE_ADDR).value();
    EXPECT_EQ(mgr_.bcrypt_set_property(m,sha,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_INVALID_HANDLE);
    ASSERT_EQ(mgr_.bcrypt_create_hash(m,sha,HANDLE_ADDR,0,0,0,0,0),STATUS_SUCCESS);
    const u32 hash=memory_.read_u32(HANDLE_ADDR).value();
    EXPECT_EQ(mgr_.bcrypt_set_property(m,hash,PROP_NAME_ADDR,INPUT_ADDR,0,0),0xc00000bbu);
    ASSERT_EQ(mgr_.bcrypt_destroy_hash(hash),STATUS_SUCCESS);
    ASSERT_EQ(mgr_.bcrypt_close_algorithm_provider(sha,0),STATUS_SUCCESS);
    ASSERT_EQ(mgr_.bcrypt_close_algorithm_provider(h,0),STATUS_SUCCESS);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_INVALID_HANDLE);
}

TEST_F(BcryptManagerTest, SetPropertySnapshotsKeysAndControlsRealAesModes) {
    Marshaler m(memory_); m.attach_cpu(cpu_);
    write_wide_string(ALG_NAME_ADDR,"AES");
    ASSERT_EQ(mgr_.bcrypt_open_algorithm_provider(m,HANDLE_ADDR,ALG_NAME_ADDR,0,0),STATUS_SUCCESS);
    const u32 h=memory_.read_u32(HANDLE_ADDR).value();
    write_wide_string(PROP_NAME_ADDR,"ChainingMode");
    write_wide_string(INPUT_ADDR,"ChainingModeECB");
    ASSERT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_SUCCESS);
    std::array<u8,32> zero{};
    ASSERT_TRUE(memory_.write_bytes(BUFFER_ADDR,zero.data(),zero.size()).is_ok());
    ASSERT_EQ(mgr_.bcrypt_generate_symmetric_key(m,h,HANDLE_ADDR,0,0,BUFFER_ADDR,16,0),STATUS_SUCCESS);
    const u32 key=memory_.read_u32(HANDLE_ADDR).value();
    write_wide_string(INPUT_ADDR,"ChainingModeCBC");
    ASSERT_EQ(mgr_.bcrypt_set_property(m,h,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_SUCCESS);
    ASSERT_EQ(mgr_.bcrypt_get_property(m,key,PROP_NAME_ADDR,OUTPUT_ADDR,64,RESULT_ADDR,0),STATUS_SUCCESS);
    EXPECT_EQ(m.read_wide_string(OUTPUT_ADDR).value(),"ChainingModeECB");
    ASSERT_EQ(mgr_.bcrypt_close_algorithm_provider(h,0),STATUS_SUCCESS);
    // A key owns its snapshotted state and survives closing the provider.
    ASSERT_EQ(mgr_.bcrypt_encrypt(m,key,BUFFER_ADDR,32,0,0,0,OUTPUT_ADDR,32,RESULT_ADDR,0),STATUS_SUCCESS);
    const std::vector<u8> ecb={0x66,0xe9,0x4b,0xd4,0xef,0x8a,0x2c,0x3b,0x88,0x4c,0xfa,0x59,0xca,0x34,0x2b,0x2e,
                             0x66,0xe9,0x4b,0xd4,0xef,0x8a,0x2c,0x3b,0x88,0x4c,0xfa,0x59,0xca,0x34,0x2b,0x2e};
    EXPECT_EQ(read_bytes(OUTPUT_ADDR,32),ecb);
    EXPECT_EQ(mgr_.bcrypt_encrypt(m,key,BUFFER_ADDR,32,0,BUFFER2_ADDR,16,OUTPUT_ADDR,32,RESULT_ADDR,0),STATUS_INVALID_PARAMETER);
    EXPECT_EQ(memory_.read_u32(RESULT_ADDR).value(),32u);
    EXPECT_EQ(mgr_.bcrypt_encrypt(m,key,BUFFER_ADDR,32,0,BUFFER2_ADDR,0,OUTPUT_ADDR,32,RESULT_ADDR,0),0xc0000206u);
    EXPECT_EQ(mgr_.bcrypt_decrypt(m,key,OUTPUT_ADDR,32,0,BUFFER2_ADDR,16,BUFFER_ADDR,32,RESULT_ADDR,0),STATUS_INVALID_PARAMETER);
    EXPECT_EQ(mgr_.bcrypt_decrypt(m,key,OUTPUT_ADDR,32,0,BUFFER2_ADDR,0,BUFFER_ADDR,32,RESULT_ADDR,0),0xc0000206u);
    EXPECT_EQ(mgr_.bcrypt_encrypt(m,key,BUFFER_ADDR,32,0,0,16,OUTPUT_ADDR,32,RESULT_ADDR,0),STATUS_SUCCESS);
    ASSERT_EQ(mgr_.bcrypt_decrypt(m,key,OUTPUT_ADDR,32,0,0,0,BUFFER2_ADDR,32,RESULT_ADDR,0),STATUS_SUCCESS);
    EXPECT_EQ(read_bytes(BUFFER2_ADDR,32),std::vector<u8>(32,0));
    ASSERT_EQ(mgr_.bcrypt_set_property(m,key,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_SUCCESS);
    ASSERT_EQ(mgr_.bcrypt_encrypt(m,key,BUFFER_ADDR,32,0,0,0,OUTPUT_ADDR,32,RESULT_ADDR,0),STATUS_SUCCESS);
    const std::vector<u8> cbc={0x66,0xe9,0x4b,0xd4,0xef,0x8a,0x2c,0x3b,0x88,0x4c,0xfa,0x59,0xca,0x34,0x2b,0x2e,
                             0xf7,0x95,0xbd,0x4a,0x52,0xe2,0x9e,0xd7,0x13,0xd3,0x13,0xfa,0x20,0xe9,0x8d,0xbc};
    EXPECT_EQ(read_bytes(OUTPUT_ADDR,32),cbc);
    EXPECT_EQ(mgr_.bcrypt_get_property(m,key,PROP_NAME_ADDR,0,0,RESULT_ADDR,0),STATUS_SUCCESS);
    EXPECT_EQ(memory_.read_u32(RESULT_ADDR).value(),32u);
    ASSERT_TRUE(memory_.write_u32(OUTPUT_ADDR,0x5a5a5a5a).is_ok());
    EXPECT_EQ(mgr_.bcrypt_get_property(m,key,PROP_NAME_ADDR,OUTPUT_ADDR,31,RESULT_ADDR,0),0xc0000023u);
    EXPECT_EQ(memory_.read_u32(OUTPUT_ADDR).value(),0x5a5a5a5au);
    EXPECT_EQ(memory_.read_u32(RESULT_ADDR).value(),32u);
    ASSERT_TRUE(memory_.write_u32(RESULT_ADDR,0x55555555).is_ok());
    EXPECT_EQ(mgr_.bcrypt_get_property(m,key,PROP_NAME_ADDR,OUTPUT_ADDR,64,RESULT_ADDR,1),STATUS_INVALID_PARAMETER);
    EXPECT_EQ(memory_.read_u32(RESULT_ADDR).value(),0x55555555u);
    EXPECT_EQ(mgr_.bcrypt_get_property(m,key,PROP_NAME_ADDR,OUTPUT_ADDR,64,0,0),STATUS_INVALID_PARAMETER);
    ASSERT_EQ(mgr_.bcrypt_destroy_key(key),STATUS_SUCCESS);
    EXPECT_EQ(mgr_.bcrypt_set_property(m,key,PROP_NAME_ADDR,INPUT_ADDR,0,0),STATUS_INVALID_HANDLE);
}

TEST_F(BcryptManagerTest, SetPropertyThunkUsesBothGuestCallingConventions) {
    auto exports=create_bcrypt_thunks(mgr_);
    const ExportEntry* setter=nullptr;
    for(const auto& e:exports.entries) if(e.name=="BCryptSetProperty") setter=&e;
    ASSERT_NE(setter,nullptr); EXPECT_EQ(setter->stdcall_bytes,20u);
    for(bool x64:{false,true}) {
        if(x64)cpu_.init_long_mode(); else cpu_.init_protected_mode();
        cpu_.gpr.rsp=STACK_BASE+0x800;
        Marshaler m(memory_);m.attach_cpu(cpu_);
        write_wide_string(ALG_NAME_ADDR,"AES");
        ASSERT_EQ(mgr_.bcrypt_open_algorithm_provider(m,HANDLE_ADDR,ALG_NAME_ADDR,0,0),STATUS_SUCCESS);
        const u32 h=memory_.read_u32(HANDLE_ADDR).value();
        write_wide_string(PROP_NAME_ADDR,"ChainingMode"); write_wide_string(INPUT_ADDR,"ChainingModeECB");
        if(x64) {
            cpu_.gpr.rcx=h;cpu_.gpr.rdx=PROP_NAME_ADDR;cpu_.gpr.r8=INPUT_ADDR;cpu_.gpr.r9=0;
            ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp+40,0).is_ok());
        } else {
            const u32 args[]={h,static_cast<u32>(PROP_NAME_ADDR),static_cast<u32>(INPUT_ADDR),0,0};
            for(unsigned i=0;i<5;i++)ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp+4+i*4,args[i]).is_ok());
        }
        setter->handler(m); EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax),STATUS_SUCCESS);
        if(x64) {
            cpu_.gpr.rcx=0x100000000ull|h;setter->handler(m);
            EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax),STATUS_INVALID_HANDLE);
        }
        ASSERT_EQ(mgr_.bcrypt_close_algorithm_provider(h,0),STATUS_SUCCESS);
    }
}

TEST_F(BcryptManagerTest, OneShotHashExportIsRegistered) {
    const auto exports=create_bcrypt_thunks(mgr_);
    bool found=false;
    for(const auto& entry:exports.entries) if(entry.name=="BCryptHash") { found=true; EXPECT_EQ(entry.stdcall_bytes,28u); }
    EXPECT_TRUE(found);
}

#include <winapi/kernel32/error.hpp>
#include <winapi/system/bcrypt/hash_contract.inc>
#include "hash_oracle.inc"
TEST_F(BcryptManagerTest, OneShotHashMatchesWindowsOracleBothBitnesses) {
    for(bool wide : {false,true}) {
        if(wide) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        Marshaler m(memory_); m.attach_cpu(cpu_);
        auto open=[&](const char* name,u32 flags) {
            write_wide_string(ALG_NAME_ADDR,name);
            EXPECT_EQ(mgr_.bcrypt_open_algorithm_provider(m,HANDLE_ADDR,ALG_NAME_ADDR,0,flags),STATUS_SUCCESS);
            return static_cast<u32>(m.read_ptr(HANDLE_ADDR).value());
        };
        const u32 sha=open("SHA256",0), hmac=open("SHA256",8), aes=open("AES",0), rng=open("RNG",0);
        ASSERT_EQ(mgr_.bcrypt_create_hash(m,sha,HANDLE_ADDR,0,0,0,0,0),STATUS_SUCCESS);
        const u32 hash_handle=static_cast<u32>(m.read_ptr(HANDLE_ADDR).value());
        const u8 input[]={'a','b','c'}, key[]={'k','e','y'};
        ASSERT_TRUE(memory_.write_bytes(INPUT_ADDR,input,sizeof(input)).is_ok());
        ASSERT_TRUE(memory_.write_bytes(BUFFER_ADDR,key,sizeof(key)).is_ok());
        for(const auto& expected:hash_oracle_cases) {
            if(expected.wide!=wide) continue;
            const std::string_view name=expected.name;
            // A closed provider is outside the API's valid-handle contract;
            // Windows may retain a cached provider after CloseAlgorithmProvider.
            if(name=="closed") continue;
            SCOPED_TRACE(::testing::Message()<<"wide="<<wide<<" case="<<name);
            u64 alg=sha; GuestAddr secret=0, in=INPUT_ADDR, out=OUTPUT_ADDR;
            u32 secret_size=0, input_size=3, output_size=32;
            if(name=="short") output_size=31;
            if(name=="long") output_size=33;
            if(name=="zero-output") output_size=0;
            if(name=="null-output") out=0;
            if(name=="null-input-zero") { in=0; input_size=0; }
            if(name=="null-input-three") in=0;
            if(name=="input-zero") input_size=0;
            if(name=="plain-key"||name=="plain-key-zero") {secret=BUFFER_ADDR;secret_size=name=="plain-key"?3:0;}
            if(name=="plain-nullkey-three") secret_size=3;
            if(name=="hmac"||name=="hmac-nullkey-three"||name=="hmac-empty"||name=="hmac-key-zero") {
                alg=hmac;
                if(name=="hmac"||name=="hmac-key-zero") secret=BUFFER_ADDR;
                if(name=="hmac"||name=="hmac-nullkey-three") secret_size=3;
            }
            if(name=="invalid") alg=0x123456;
            if(name=="null-handle") alg=0;
            if(name=="aes") alg=aes;
            if(name=="rng") alg=rng;
            if(name=="hash-handle") alg=hash_handle;
            if(name=="pseudo-sha") alg=BCRYPT_SHA256_ALG_HANDLE;
            if(name=="pseudo-hmac") {alg=BCRYPT_HMAC_SHA256_ALG_HANDLE;secret=BUFFER_ADDR;secret_size=3;}
            if(name=="pseudo-hmac-md5") {alg=BCRYPT_HMAC_MD5_ALG_HANDLE;secret=BUFFER_ADDR;secret_size=3;output_size=16;}
            if(name=="pseudo-hmac-sha1") {alg=BCRYPT_HMAC_SHA1_ALG_HANDLE;secret=BUFFER_ADDR;secret_size=3;output_size=20;}
            if(name=="pseudo-md5") {alg=BCRYPT_MD5_ALG_HANDLE;output_size=16;}
            if(name=="pseudo-sha1") {alg=BCRYPT_SHA1_ALG_HANDLE;output_size=20;}
            if(name=="input-error-before-short") {in=0;output_size=31;}
            if(name=="secret-error-before-short") {alg=hmac;secret_size=3;output_size=31;}
            if(name=="unmapped-output") out=1;
            if(name=="unmapped-input") in=1;
            if(name=="unmapped-key") {alg=hmac;secret=1;secret_size=3;}
            if(name=="alias") out=INPUT_ADDR;
            ASSERT_TRUE(memory_.write_bytes(INPUT_ADDR,input,sizeof(input)).is_ok());
            std::array<u8,64> sentinel; sentinel.fill(0xa5);
            ASSERT_TRUE(memory_.write_bytes(OUTPUT_ADDR,sentinel.data(),sentinel.size()).is_ok());
            set_last_error(0x12345678); cpu_.pending_raise.active=false;
            EXPECT_EQ(mgr_.bcrypt_hash(m,alg,secret,secret_size,in,input_size,out,output_size),expected.status);
            EXPECT_EQ(cpu_.pending_raise.active,expected.status==HASH_STATUS_ACCESS_VIOLATION);
            if(cpu_.pending_raise.active) EXPECT_EQ(cpu_.pending_raise.code,HASH_STATUS_ACCESS_VIOLATION);
            EXPECT_EQ(get_last_error(),expected.last);
            EXPECT_EQ(read_bytes(name=="alias" ? INPUT_ADDR : OUTPUT_ADDR,32),std::vector<u8>(expected.bytes.begin(),expected.bytes.end()));
        }
        EXPECT_EQ(mgr_.bcrypt_destroy_hash(hash_handle),STATUS_SUCCESS);
        for(u32 alg:{sha,hmac,aes,rng}) EXPECT_EQ(mgr_.bcrypt_close_algorithm_provider(alg,0),STATUS_SUCCESS);
    }
}

TEST_F(BcryptManagerTest, OneShotHashThunkUsesGuestCallingConventionAndIndependentCalls) {
    auto exports=create_bcrypt_thunks(mgr_);
    const ExportEntry* entry=nullptr;
    for(const auto& candidate:exports.entries) if(candidate.name==BCRYPT_HASH_EXPORT_NAME) entry=&candidate;
    ASSERT_NE(entry,nullptr);
    const auto& expected=hash_oracle_cases[4]; // Windows SHA256("abc"), following pointer/alias cases.
    ASSERT_EQ(std::string_view(expected.name),"sha");
    for(bool wide:{false,true}) {
        if(wide) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        Marshaler m(memory_);m.attach_cpu(cpu_);cpu_.gpr.rsp=STACK_BASE+0x800;
        const u8 text[]={'a','b','c'};
        ASSERT_TRUE(memory_.write_bytes(INPUT_ADDR,text,sizeof(text)).is_ok());
        const u64 args[]={BCRYPT_SHA256_ALG_HANDLE,0,0,INPUT_ADDR,3,OUTPUT_ADDR,32};
        if(wide) {
            cpu_.gpr.rcx=args[0];cpu_.gpr.rdx=args[1];cpu_.gpr.r8=args[2];cpu_.gpr.r9=args[3];
            for(unsigned i=4;i<7;++i) ASSERT_TRUE(memory_.write_u64(cpu_.gpr.rsp+40+(i-4)*8,args[i]).is_ok());
        } else {
            for(unsigned i=0;i<7;++i) ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp+4+i*4,static_cast<u32>(args[i])).is_ok());
        }
        for(unsigned call=0;call<3;++call) {
            entry->handler(m);
            EXPECT_EQ(static_cast<u32>(cpu_.gpr.rax),STATUS_SUCCESS);
            EXPECT_EQ(read_bytes(OUTPUT_ADDR,32),std::vector<u8>(expected.bytes.begin(),expected.bytes.end()));
        }
        EXPECT_EQ(read_bytes(INPUT_ADDR,3),std::vector<u8>(text,text+3));
        if(wide) {
            cpu_.gpr.rcx=0x100000000ull|BCRYPT_SHA256_ALG_HANDLE;
            cpu_.pending_raise.active=false;
            entry->handler(m);
            EXPECT_TRUE(cpu_.pending_raise.active);
            EXPECT_EQ(cpu_.pending_raise.code,HASH_STATUS_ACCESS_VIOLATION);
        }
    }
}

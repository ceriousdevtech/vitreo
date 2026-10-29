/// @file test_cpuid_extended.cpp
/// @brief Tests for CPUID extended leaves (CPU-034)

#include <gtest/gtest.h>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include "core/cpu/interpreter/interpreter.hpp"
#include "platform/host_hardware.hpp"

using namespace vitreo;
using namespace vitreo::cpu;

// Register indices (x86 encoding order)
constexpr u8 kRAX = static_cast<u8>(RegIndex::RAX);  // 0
constexpr u8 kRCX = static_cast<u8>(RegIndex::RCX);  // 1
constexpr u8 kRDX = static_cast<u8>(RegIndex::RDX);  // 2
constexpr u8 kRBX = static_cast<u8>(RegIndex::RBX);  // 3
constexpr u8 kRSP = static_cast<u8>(RegIndex::RSP);  // 4

class CpuidExtendedTest : public ::testing::Test {
protected:
    void SetUp() override {
        m_memory = std::make_unique<GuestMemory>();
        auto map_result = m_memory->map_region(0x1000, 0x10000, MemoryProtection::ReadWrite);
        ASSERT_TRUE(map_result);

        m_cpu = std::make_unique<CpuState>();
        m_cpu->init_long_mode();
        m_cpu->gpr.set64(kRSP, 0x10000);
        m_cpu->rip = 0x1000;
    }

    void run_cpuid(u32 leaf, u32 subleaf = 0) {
        // Set leaf / subleaf
        m_cpu->gpr.set64(kRAX, leaf);
        m_cpu->gpr.set64(kRCX, subleaf);

        // Write CPUID opcode (0F A2) at code area
        u8 code[] = { 0x0F, 0xA2 };
        auto wr = m_memory->write_bytes(0x1000, code, sizeof(code));
        ASSERT_TRUE(wr);

        // Reset RIP and execute
        m_cpu->rip = 0x1000;
        Interpreter interp(*m_cpu, *m_memory);
        ASSERT_EQ(interp.step(), ExecResult::Continue);
    }

    u64 run_rdtsc() {
        const u8 code[] = {0x0F, 0x31};
        auto wr = m_memory->write_bytes(0x1000, code, sizeof(code));
        EXPECT_TRUE(wr);
        m_cpu->rip = 0x1000;
        Interpreter interp(*m_cpu, *m_memory);
        EXPECT_EQ(interp.step(), ExecResult::Continue);
        return (static_cast<u64>(edx()) << 32) | eax();
    }

    u32 eax() const { return m_cpu->gpr.get32(kRAX); }
    u32 ebx() const { return m_cpu->gpr.get32(kRBX); }
    u32 ecx() const { return m_cpu->gpr.get32(kRCX); }
    u32 edx() const { return m_cpu->gpr.get32(kRDX); }

    std::unique_ptr<GuestMemory> m_memory;
    std::unique_ptr<CpuState> m_cpu;
};

// ---------------------------------------------------------------------------
// Leaf 0 – Vendor string
// ---------------------------------------------------------------------------

TEST_F(CpuidExtendedTest, Leaf0_VendorStringIsGenuineIntel) {
    run_cpuid(0);

    // Build the 12-byte vendor string from EBX:EDX:ECX
    char vendor[13] = {};
    std::memcpy(vendor + 0, reinterpret_cast<const char*>(&m_cpu->gpr.r[kRBX]), 4);
    std::memcpy(vendor + 4, reinterpret_cast<const char*>(&m_cpu->gpr.r[kRDX]), 4);
    std::memcpy(vendor + 8, reinterpret_cast<const char*>(&m_cpu->gpr.r[kRCX]), 4);
    EXPECT_STREQ(vendor, "GenuineIntel");
}

TEST_F(CpuidExtendedTest, Leaf0_MaxLeafAtLeast7) {
    run_cpuid(0);
    EXPECT_GE(eax(), 7u);
}

// ---------------------------------------------------------------------------
// Leaf 1 – Feature flags
// ---------------------------------------------------------------------------

TEST_F(CpuidExtendedTest, Leaf1_SSEBitSet) {
    run_cpuid(1);
    EXPECT_NE(edx() & (1u << 25), 0u) << "SSE bit (EDX.25) must be set";
}

TEST_F(CpuidExtendedTest, Leaf1_SSE2BitSet) {
    run_cpuid(1);
    EXPECT_NE(edx() & (1u << 26), 0u) << "SSE2 bit (EDX.26) must be set";
}

TEST_F(CpuidExtendedTest, Leaf1_SSE3BitSet) {
    run_cpuid(1);
    EXPECT_NE(ecx() & (1u << 0), 0u) << "SSE3 bit (ECX.0) must be set";
}

TEST_F(CpuidExtendedTest, Leaf1_SSE42BitSet_Legacy) {
    run_cpuid(1);
    EXPECT_NE(ecx() & (1u << 20), 0u) << "SSE4.2 bit (ECX.20) must be set";
}

TEST_F(CpuidExtendedTest, Leaf1_POPCNTBitSet) {
    run_cpuid(1);
    EXPECT_NE(ecx() & (1u << 23), 0u) << "POPCNT bit (ECX.23) must be set";
}

TEST_F(CpuidExtendedTest, Leaf1_SSE41BitSet) {
    run_cpuid(1);
    EXPECT_NE(ecx() & (1u << 19), 0u) << "SSE4.1 bit (ECX.19) must be set";
}

TEST_F(CpuidExtendedTest, Leaf1_SSE42BitSet) {
    run_cpuid(1);
    EXPECT_NE(ecx() & (1u << 20), 0u) << "SSE4.2 bit (ECX.20) must be set";
}

TEST_F(CpuidExtendedTest, Leaf1_ReportsTranslatorPresent) {
    run_cpuid(1);
    EXPECT_NE(ecx() & (1u << 31), 0u);
}

TEST_F(CpuidExtendedTest, Leaf1_ReportsHostExecutionCapacity) {
    // EBX[23:16] is the number of addressable logical-processor IDs (valid
    // with HTT): a power of two covering every host logical processor.
    run_cpuid(1);
    const u32 addressable = (ebx() >> 16) & 0xFFu;
    EXPECT_NE(edx() & (1u << 28), 0u);
    EXPECT_GE(addressable, std::thread::hardware_concurrency());
    EXPECT_EQ(addressable & (addressable - 1), 0u);
}

TEST_F(CpuidExtendedTest, Leaf1_FamilyModel) {
    run_cpuid(1);
    EXPECT_EQ(eax(), 0x000306C3u) << "EAX should report Haswell-like family/model/stepping";
}

// ---------------------------------------------------------------------------
// Leaf 7 – Structured extended features
// ---------------------------------------------------------------------------

TEST_F(CpuidExtendedTest, Leaf7_DoesNotCrash) {
    run_cpuid(7, 0);
    EXPECT_EQ(eax(), 0u);
    EXPECT_NE(ebx() & (1u << 3), 0u);   // BMI1
    EXPECT_NE(ebx() & (1u << 5), 0u);   // AVX2
    EXPECT_NE(ebx() & (1u << 8), 0u);   // BMI2
    EXPECT_NE(ebx() & (1u << 19), 0u);  // ADX
    EXPECT_NE(ebx() & (1u << 29), 0u);  // SHA-NI
    EXPECT_EQ(ecx(), 0u);
    EXPECT_EQ(edx(), 0u);
}

TEST_F(CpuidExtendedTest, Leaf7_ProtectedModeDoesNotAdvertiseAvx2) {
    m_cpu->init_protected_mode();
    run_cpuid(7, 0);

    EXPECT_EQ(ebx() & (1u << 5), 0u);

    // AVX itself remains available; only the incomplete 32-bit AVX2
    // execution contract is withheld.
    run_cpuid(1);
    EXPECT_NE(ecx() & (1u << 28), 0u);
}

// ---------------------------------------------------------------------------
// Extended leaves
// ---------------------------------------------------------------------------

TEST_F(CpuidExtendedTest, Ext_MaxLeafAtLeast80000004) {
    run_cpuid(0x80000000);
    EXPECT_GE(eax(), 0x80000004u);
}

TEST_F(CpuidExtendedTest, Ext_BrandString) {
    char brand[49] = {};

    // Leaf 0x80000002 → bytes [0..15]
    run_cpuid(0x80000002);
    u32 regs2[4] = { eax(), ebx(), ecx(), edx() };
    std::memcpy(brand + 0, regs2, 16);

    // Leaf 0x80000003 → bytes [16..31]
    run_cpuid(0x80000003);
    u32 regs3[4] = { eax(), ebx(), ecx(), edx() };
    std::memcpy(brand + 16, regs3, 16);

    // Leaf 0x80000004 → bytes [32..47]
    run_cpuid(0x80000004);
    u32 regs4[4] = { eax(), ebx(), ecx(), edx() };
    std::memcpy(brand + 32, regs4, 16);

    brand[48] = '\0';
    EXPECT_EQ(std::string_view(brand), platform::host_hardware().cpu.brand);
}

TEST_F(CpuidExtendedTest, HypervisorLeafIdentifiesVitreo) {
    run_cpuid(0x40000000);
    EXPECT_EQ(eax(), 0x40000000u);
    char vendor[13] = {};
    const u32 regs[3] = {ebx(), ecx(), edx()};
    std::memcpy(vendor, regs, sizeof(regs));
    EXPECT_STREQ(vendor, "VitreoVirt  ");
}

TEST_F(CpuidExtendedTest, RdtscTracksElapsedTimeAtOneGigahertz) {
    const u64 first = run_rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const u64 second = run_rdtsc();
    EXPECT_GE(second - first, 1'000'000u);
}

// ---------------------------------------------------------------------------
// Extended feature bits
// ---------------------------------------------------------------------------

TEST_F(CpuidExtendedTest, Ext_LAHFBitSet) {
    run_cpuid(0x80000001);
    EXPECT_NE(ecx() & (1u << 0), 0u) << "LAHF-in-long-mode bit (ECX.0) must be set";
}

TEST_F(CpuidExtendedTest, Ext_SyscallBitSet) {
    run_cpuid(0x80000001);
    EXPECT_NE(edx() & (1u << 11), 0u) << "SYSCALL bit (EDX.11) must be set";
}

TEST_F(CpuidExtendedTest, Ext_LongModeBitSet) {
    run_cpuid(0x80000001);
    EXPECT_NE(edx() & (1u << 29), 0u) << "Long-Mode bit (EDX.29) must be set";
}

// ---------------------------------------------------------------------------
// Unknown leaf
// ---------------------------------------------------------------------------

TEST_F(CpuidExtendedTest, UnknownLeaf_ReturnsZeros) {
    run_cpuid(0x12345678);
    EXPECT_EQ(eax(), 0u);
    EXPECT_EQ(ebx(), 0u);
    EXPECT_EQ(ecx(), 0u);
    EXPECT_EQ(edx(), 0u);
}

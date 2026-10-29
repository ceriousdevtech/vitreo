/// @file test_ncrypt.cpp
/// @brief Unit tests for ncrypt.dll CNG key storage provider contracts.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/ncrypt/ncrypt.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <cstring>
#include <string>
#include <utility>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

ExportEntry* find_export(DllExports& exports, const char* name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

} // namespace

class NcryptThunkTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(
            MEM_BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu_.init_protected_mode();
        cpu_.gpr.rsp = STACK_BASE + 0x800;
        marshaler_.attach_cpu(cpu_);
        exports_ = create_ncrypt_thunks();
    }

    void write_wide(GuestAddr addr, const char* ascii) {
        (void)marshaler_.write_wide_string(addr, ascii);
    }

    static constexpr GuestAddr MEM_BASE = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr HANDLE_ADDR = MEM_BASE;
    static constexpr GuestAddr NAME_ADDR = MEM_BASE + 0x100;
    static constexpr GuestAddr OUT_ADDR = MEM_BASE + 0x200;
    static constexpr GuestAddr SIZE_ADDR = MEM_BASE + 0x300;

    GuestMemory memory_;
    cpu::CpuState cpu_;
    Marshaler marshaler_{memory_};
    DllExports exports_;
};

TEST_F(NcryptThunkTest, OpenStorageProviderAndIsAlgSupported) {
    auto* open = find_export(exports_, "NCryptOpenStorageProvider");
    auto* is_alg = find_export(exports_, "NCryptIsAlgSupported");
    auto* get_prop = find_export(exports_, "NCryptGetProperty");
    auto* free_obj = find_export(exports_, "NCryptFreeObject");
    auto* sign = find_export(exports_, "NCryptSignHash");
    ASSERT_NE(open, nullptr);
    ASSERT_NE(is_alg, nullptr);
    ASSERT_NE(get_prop, nullptr);
    ASSERT_NE(free_obj, nullptr);
    ASSERT_NE(sign, nullptr);
    EXPECT_EQ(open->stdcall_bytes, 12u);
    EXPECT_EQ(is_alg->stdcall_bytes, 12u);
    EXPECT_EQ(get_prop->stdcall_bytes, 24u);
    EXPECT_EQ(sign->stdcall_bytes, 32u);

    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, HANDLE_ADDR).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 12, 0).is_ok());
    open->handler(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    const u32 provider = memory_.read_u32(HANDLE_ADDR).value_or(0);
    EXPECT_NE(provider, 0u);

    // Windows' KSP supports RSA (sspi_ncrypt_probe.expected); Vitreo's
    // provider can create no key, so it honestly reports NTE_NOT_SUPPORTED.
    write_wide(NAME_ADDR, "RSA");
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, NAME_ADDR).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 12, 0).is_ok());
    is_alg->handler(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 0x80090029u);

    write_wide(NAME_ADDR, "NOTANALG");
    is_alg->handler(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 0x80090029u); // NTE_NOT_SUPPORTED

    write_wide(NAME_ADDR, "Name");
    ASSERT_TRUE(memory_.write_u32(SIZE_ADDR, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, provider).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 8, NAME_ADDR).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 12, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 16, 0).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 20, SIZE_ADDR).is_ok());
    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 24, 0).is_ok());
    get_prop->handler(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_GT(memory_.read_u32(SIZE_ADDR).value_or(0), 0u);

    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, 0).is_ok());
    sign->handler(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 0x80090026u); // NTE_INVALID_HANDLE

    ASSERT_TRUE(memory_.write_u32(cpu_.gpr.rsp + 4, provider).is_ok());
    free_obj->handler(marshaler_);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

// =============================================================================
// Contract (x86 and x64). Expected values: Windows 11,
// tools/oracle_probe/sspi_ncrypt_probe.expected / .x86.expected.
// =============================================================================

namespace {

constexpr u64 NTE_BAD_FLAGS_ = 0x80090009;
constexpr u64 NTE_BAD_KEYSET_ = 0x80090016;
constexpr u64 NTE_INVALID_HANDLE_ = 0x80090026;
constexpr u64 NTE_INVALID_PARAMETER_ = 0x80090027;
constexpr u64 NTE_BUFFER_TOO_SMALL_ = 0x80090028;
constexpr u64 NTE_NOT_SUPPORTED_ = 0x80090029;

class NcryptContractTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr kStack = 0x80000;
    static constexpr GuestAddr kData = 0x00400000;
    static constexpr GuestAddr kH = kData;           // handle out-param
    static constexpr GuestAddr kStr = kData + 0x100;
    static constexpr GuestAddr kBuf = kData + 0x400;
    static constexpr GuestAddr kCb = kData + 0x800;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite).is_ok());
        is64 = GetParam();
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        exports = create_ncrypt_thunks();
    }
    u64 call(const char* name, std::initializer_list<u64> args) {
        cpu.set_sp(kStack + 0xE000);
        const GuestAddr sp = static_cast<GuestAddr>(cpu.sp());
        u32 i = 0;
        for (u64 a : args) {
            if (is64) {
                switch (i) {
                case 0: cpu.gpr.rcx = a; break;
                case 1: cpu.gpr.rdx = a; break;
                case 2: cpu.gpr.r8 = a; break;
                case 3: cpu.gpr.r9 = a; break;
                default: (void)memory.write_u64(sp + 40 + (i - 4) * 8, a); break;
                }
            } else {
                (void)memory.write_u32(sp + 4 + i * 4, static_cast<u32>(a));
            }
            ++i;
        }
        ExportEntry* e = find_export(exports, name);
        EXPECT_NE(e, nullptr) << name;
        if (!e) return ~0ULL;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        e->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u64 rptr(GuestAddr a) { return is64 ? memory.read_u64(a).value_or(0) : memory.read_u32(a).value_or(0); }
    void wptr(GuestAddr a, u64 v) {
        if (is64) (void)memory.write_u64(a, v); else (void)memory.write_u32(a, static_cast<u32>(v));
    }
    GuestAddr wstr(GuestAddr at, const char* s) {
        (void)Marshaler(memory).write_wide_string(at, s);
        return at;
    }
    u64 open_ksp() {
        EXPECT_EQ(call("NCryptOpenStorageProvider",
                       {kH, wstr(kStr, "Microsoft Software Key Storage Provider"), 0}), 0u);
        return rptr(kH);
    }

    GuestMemory memory;
    cpu::CpuState cpu;
    DllExports exports;
    bool is64 = false;
};

INSTANTIATE_TEST_SUITE_P(Bitness, NcryptContractTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) {
                             return info.param ? "x64" : "x86";
                         });

// open_ksp / open_null_name / open_bogus_name / open_silent_flag / open_null_out.
TEST_P(NcryptContractTest, OpenStorageProvider) {
    const u64 prov = open_ksp();
    EXPECT_NE(prov, 0u);
    EXPECT_EQ(call("NCryptOpenStorageProvider", {kH, 0, 0}), 0u);
    EXPECT_EQ(call("NCryptFreeObject", {rptr(kH)}), 0u);
    EXPECT_EQ(call("NCryptOpenStorageProvider", {kH, wstr(kStr, "Bogus Provider"), 0}), 0xC0000225u);
    EXPECT_EQ(call("NCryptOpenStorageProvider", {kH, wstr(kStr, "Microsoft Platform Crypto Provider"), 0}),
              0xC0000225u);
    EXPECT_EQ(call("NCryptOpenStorageProvider",
                   {kH, wstr(kStr, "Microsoft Software Key Storage Provider"), 0x40}), 0u);
    EXPECT_EQ(call("NCryptFreeObject", {rptr(kH)}), 0u);
    EXPECT_EQ(call("NCryptOpenStorageProvider", {0, 0, 0}), NTE_INVALID_PARAMETER_);
    EXPECT_EQ(call("NCryptFreeObject", {prov}), 0u);
    EXPECT_EQ(call("NCryptFreeObject", {0}), NTE_INVALID_HANDLE_);
}

// is_alg_*: handle, flags and NULL-algorithm errors; no key algorithm is
// supported by a provider that can create no key.
TEST_P(NcryptContractTest, IsAlgSupported) {
    const u64 prov = open_ksp();
    EXPECT_EQ(call("NCryptIsAlgSupported", {0, wstr(kStr, "RSA"), 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(call("NCryptIsAlgSupported", {prov, wstr(kStr, "RSA"), 0x1}), NTE_BAD_FLAGS_);
    EXPECT_EQ(call("NCryptIsAlgSupported", {prov, 0, 0}), 0x800706F4u);
    for (const char* alg : {"SHA256", "MD5", "Bogus", "RSA", "ECDSA_P256"})
        EXPECT_EQ(call("NCryptIsAlgSupported", {prov, wstr(kStr, alg), 0}), NTE_NOT_SUPPORTED_) << alg;
    EXPECT_EQ(call("NCryptFreeObject", {prov}), 0u);
}

// open_key_* / create_key_* / import / export / finalize / delete / sign.
TEST_P(NcryptContractTest, KeyOperations) {
    const u64 prov = open_ksp();
    wptr(kH + 0x40, 0x5A5A5A5A);
    EXPECT_EQ(call("NCryptOpenKey", {prov, kH + 0x40, wstr(kStr, "vitreo-probe-nonexistent-key-7f3a"), 0, 0}),
              NTE_BAD_KEYSET_);
    EXPECT_EQ(rptr(kH + 0x40), 0x5A5A5A5Au);
    EXPECT_EQ(call("NCryptOpenKey", {0, kH + 0x40, wstr(kStr, "x"), 0, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(call("NCryptOpenKey", {prov, kH + 0x40, wstr(kStr, "x"), 0, 0x12345678}), NTE_BAD_FLAGS_);
    EXPECT_EQ(call("NCryptOpenKey", {prov, kH + 0x40, wstr(kStr, "x"), 0, 0x40 | 0x20}), NTE_BAD_KEYSET_);
    EXPECT_EQ(rptr(kH + 0x40), 0x5A5A5A5Au);

    EXPECT_EQ(call("NCryptCreatePersistedKey", {0, kH + 0x40, wstr(kStr, "RSA"), 0, 0, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(call("NCryptCreatePersistedKey", {prov, kH + 0x40, wstr(kStr, "Bogus"), 0, 0, 0}), NTE_NOT_SUPPORTED_);
    EXPECT_EQ(rptr(kH + 0x40), 0x5A5A5A5Au);
    EXPECT_EQ(call("NCryptImportKey", {0, 0, wstr(kStr, "RSAPUBLICBLOB"), 0, kH + 0x40, kBuf, 16, 0}),
              NTE_INVALID_HANDLE_);
    (void)memory.write_u32(kCb, 0x5A5A);
    EXPECT_EQ(call("NCryptExportKey", {0, 0, wstr(kStr, "RSAPUBLICBLOB"), 0, 0, 0, kCb, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(memory.read_u32(kCb).value_or(0), 0x5A5Au);
    EXPECT_EQ(call("NCryptFinalizeKey", {0, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(call("NCryptDeleteKey", {0, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(call("NCryptSignHash", {0, 0, kBuf, 32, 0, 0, kCb, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(memory.read_u32(kCb).value_or(0), 0x5A5Au);
    EXPECT_EQ(call("NCryptFreeObject", {prov}), 0u);
}

// getprop_* / setprop_*.
TEST_P(NcryptContractTest, Properties) {
    const u64 prov = open_ksp();
    (void)memory.write_u32(kCb, 0x5A5A);
    EXPECT_EQ(call("NCryptGetProperty", {prov, wstr(kStr, "Name"), kBuf, 256, kCb, 0}), 0u);
    EXPECT_EQ(memory.read_u32(kCb).value_or(0), 80u);
    EXPECT_EQ(call("NCryptGetProperty", {prov, wstr(kStr, "Name"), 0, 0, kCb, 0}), 0u);
    EXPECT_EQ(memory.read_u32(kCb).value_or(0), 80u);
    EXPECT_EQ(call("NCryptGetProperty", {prov, wstr(kStr, "Name"), kBuf, 4, kCb, 0}), NTE_BUFFER_TOO_SMALL_);
    EXPECT_EQ(memory.read_u32(kCb).value_or(0), 80u);
    (void)memory.write_u32(kCb, 0x5A5A);
    EXPECT_EQ(call("NCryptGetProperty", {prov, wstr(kStr, "Bogus Property"), kBuf, 256, kCb, 0}), NTE_NOT_SUPPORTED_);
    EXPECT_EQ(memory.read_u32(kCb).value_or(1), 0u);
    EXPECT_EQ(call("NCryptGetProperty", {0, wstr(kStr, "Name"), kBuf, 256, kCb, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(call("NCryptSetProperty", {prov, wstr(kStr, "Bogus Property"), kBuf, 4, 0}), NTE_NOT_SUPPORTED_);
    EXPECT_EQ(call("NCryptSetProperty", {0, wstr(kStr, "Bogus Property"), kBuf, 4, 0}), NTE_INVALID_HANDLE_);
    EXPECT_EQ(call("NCryptFreeObject", {prov}), 0u);
}

// x86 stdcall byte counts from ncrypt.h prototypes.
TEST(NcryptExportTable, StdcallByteCounts) {
    DllExports exports = create_ncrypt_thunks();
    const std::pair<const char*, u32> expected[] = {
        {"NCryptOpenStorageProvider", 12}, {"NCryptOpenKey", 20}, {"NCryptCreatePersistedKey", 24},
        {"NCryptImportKey", 32}, {"NCryptExportKey", 32}, {"NCryptFinalizeKey", 8},
        {"NCryptDeleteKey", 8}, {"NCryptFreeObject", 4}, {"NCryptSetProperty", 20},
        {"NCryptIsAlgSupported", 12}, {"NCryptGetProperty", 24}, {"NCryptSignHash", 32},
    };
    for (const auto& [name, bytes] : expected) {
        ExportEntry* e = find_export(exports, name);
        ASSERT_NE(e, nullptr) << name;
        EXPECT_EQ(e->stdcall_bytes, bytes) << name;
    }
}

} // namespace

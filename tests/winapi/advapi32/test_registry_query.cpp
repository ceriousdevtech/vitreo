#include <gtest/gtest.h>
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/kernel32/error.hpp"
#include <algorithm>
#include <array>
#include <cstring>
using namespace vitreo;
using namespace vitreo::winapi;
namespace sdk {
#include "winapi/advapi32/registry/reg_query_contract.inc"
}
namespace facts = sdk::reg_query_contract;
namespace oracle {
#include "../../oracle_data/reg_query_value.generated.inc"
}

class RegistryQueryContract : public ::testing::Test {
protected:
    capsule::Registry registry;
    HandleTable handles;
    RegistryThunks thunks{registry, handles};
    u64 key = 0;
    void SetUp() override {
        ASSERT_EQ(thunks.reg_create_key_ex_w(capsule::HKEY_CURRENT_USER,
            L"Software\\RegistryQueryContract", &key, nullptr, facts::KEY_ALL_ACCESS), ERROR_SUCCESS);
        const char16_t multi[] = {u'A', u'\u20ac', 0, u'B', 0, 0};
        ASSERT_EQ(thunks.reg_set_value_ex_w(key, L"Multi", REG_MULTI_SZ,
            reinterpret_cast<const u8*>(multi), sizeof(multi)), ERROR_SUCCESS);
        const char16_t unterminated[] = {u'A', u'B', u'X', u'X'};
        ASSERT_EQ(thunks.reg_set_value_ex_w(key, L"Unterminated", REG_SZ,
            reinterpret_cast<const u8*>(unterminated), sizeof(char16_t) * 2), ERROR_SUCCESS);
    }
};

TEST_F(RegistryQueryContract, CountedMultiStringsUseTheProcessAcp) {
    u32 kind = REG_NONE, count = 0;
    ASSERT_EQ(thunks.reg_query_value_ex_a(key, "Multi", &kind, nullptr, &count), oracle::a_multi_size_status);
    EXPECT_EQ(kind, oracle::a_multi_size_type); EXPECT_EQ(count, oracle::a_multi_size_size);
    std::array<u8, 32> bytes{};
    count = bytes.size();
    ASSERT_EQ(thunks.reg_query_value_ex_a(key, "Multi", &kind, bytes.data(), &count), oracle::a_multi_full_status);
    EXPECT_TRUE(std::equal(oracle::a_multi_full_bytes.begin(), oracle::a_multi_full_bytes.end(), bytes.begin()));
    EXPECT_EQ(count, oracle::a_multi_full_size);
    count = bytes.size(); bytes.fill(0);
    ASSERT_EQ(thunks.reg_query_value_ex_w(key, L"Multi", &kind, bytes.data(), &count), oracle::w_multi_full_status);
    EXPECT_TRUE(std::equal(oracle::w_multi_full_bytes.begin(), oracle::w_multi_full_bytes.end(), bytes.begin()));
    EXPECT_EQ(count, oracle::w_multi_full_size);
    count = 0;
    EXPECT_EQ(thunks.reg_query_value_ex_a(key, "Multi", &kind, bytes.data(), &count), oracle::a_zero_buffer_status);
    EXPECT_EQ(count, oracle::a_zero_buffer_size);
}

TEST_F(RegistryQueryContract, MissingTerminatorsRemainOutsideTheReportedCount) {
    std::array<u8, 32> bytes{};
    u32 kind = REG_NONE, count = bytes.size();
    ASSERT_EQ(thunks.reg_query_value_ex_a(key, "Unterminated", &kind, bytes.data(), &count), oracle::a_unterminated_status);
    EXPECT_EQ(count, oracle::a_unterminated_size);
    EXPECT_TRUE(std::equal(oracle::a_unterminated_bytes.begin(), oracle::a_unterminated_bytes.end(), bytes.begin()));
    count = bytes.size(); bytes.fill(0);
    ASSERT_EQ(thunks.reg_query_value_ex_w(key, L"Unterminated", &kind, bytes.data(), &count), oracle::w_unterminated_status);
    EXPECT_EQ(count, oracle::w_unterminated_size);
    EXPECT_TRUE(std::equal(oracle::w_unterminated_bytes.begin(), oracle::w_unterminated_bytes.end(), bytes.begin()));
}

TEST_F(RegistryQueryContract, HandleRightsAndFailureOutputsMatchWindows) {
    u64 write_only = 0;
    ASSERT_EQ(thunks.reg_open_key_ex_w(capsule::HKEY_CURRENT_USER,
        L"Software\\RegistryQueryContract", 0, facts::KEY_SET_VALUE, &write_only), ERROR_SUCCESS);
    std::array<u8, 32> bytes; bytes.fill(0xa5);
    u32 kind = 0xfeedbeef, count = bytes.size();
    EXPECT_EQ(thunks.reg_query_value_ex_a(write_only, "Multi", &kind, bytes.data(), &count), oracle::query_mask_2_status);
    EXPECT_EQ(kind, oracle::query_mask_2_type); EXPECT_EQ(count, bytes.size());
    kind = 0xfeedbeef;
    EXPECT_EQ(thunks.reg_query_value_ex_w(0, L"Multi", &kind, bytes.data(), &count), oracle::w_invalid_key_status);
    EXPECT_EQ(kind, oracle::w_invalid_key_type);
    EXPECT_EQ(thunks.reg_query_value_ex_a(key, "Missing", &kind, bytes.data(), &count), oracle::a_missing_value_status);
    EXPECT_EQ(kind, oracle::a_missing_value_type);
    EXPECT_EQ(thunks.reg_query_value_ex_a(key, "Multi", &kind, bytes.data(), nullptr), oracle::a_no_size_with_data_status);
    EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [](u8 byte) { return byte == 0xa5; }));
}

TEST_F(RegistryQueryContract, ExportParameterRelationshipsAndOutputsInBothAbis) {
    for (const bool is64 : {false, true}) for (const bool wide : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x6000, MemoryProtection::ReadWrite).is_ok());
        const char16_t wname[] = u"Multi";
        const char aname[] = "Multi";
        ASSERT_TRUE(memory.write_bytes(0x11000,
            reinterpret_cast<const Byte*>(wide ? static_cast<const void*>(wname) : static_cast<const void*>(aname)),
            wide ? sizeof(wname) : sizeof(aname)).is_ok());
        auto exports = create_advapi32_registry_thunks(thunks);
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [wide](const ExportEntry& value) { return value.name == (wide ? "RegQueryValueExW" : "RegQueryValueExA"); });
        ASSERT_NE(entry, exports.entries.end());
        const auto call = [&](GuestAddr reserved, GuestAddr data, GuestAddr size) {
            cpu::CpuState state;
            std::array<u64, 6> args{key, 0x11000, reserved, 0x12000, data, size};
            if (is64) {
                state.init_long_mode(); state.gpr.rsp = 0x14000;
                std::array<u64*, 4> regs{&state.gpr.rcx, &state.gpr.rdx, &state.gpr.r8, &state.gpr.r9};
                for (usize i = 0; i < regs.size(); ++i) *regs[i] = args[i];
                for (usize i = regs.size(); i < args.size(); ++i)
                    (void)memory.write_u64(0x14000 + (i + 1) * sizeof(u64), args[i]);
            } else {
                state.init_protected_mode(); state.set_sp(0x14000);
                for (usize i = 0; i < args.size(); ++i)
                    (void)memory.write_u32(0x14000 + (i + 1) * sizeof(u32), static_cast<u32>(args[i]));
            }
            Marshaler m(memory); m.attach_cpu(state);
            set_last_error(0x12345678); entry->handler(m);
            EXPECT_EQ(get_last_error(), 0x12345678u);
            return static_cast<u32>(state.gpr.rax);
        };
        ASSERT_TRUE(memory.write_u32(0x12000, 0xfeedbeef).is_ok());
        ASSERT_TRUE(memory.write_u32(0x12004, 32).is_ok());
        EXPECT_EQ(call(0x12500, 0x13000, 0x12004), oracle::a_reserved_zero_status);
        EXPECT_EQ(memory.read_u32(0x12000).value(), oracle::a_reserved_zero_type);
        EXPECT_EQ(memory.read_u32(0x12004).value(), oracle::a_reserved_zero_size);
        EXPECT_EQ(call(0, 0x13000, 0), oracle::a_no_size_with_data_status);
        ASSERT_TRUE(memory.write_u32(0x12004, 0).is_ok());
        EXPECT_EQ(call(0, 0x13000, 0x12004), oracle::a_zero_buffer_status);
        EXPECT_EQ(memory.read_u32(0x12004).value(), wide ? oracle::w_zero_buffer_size : oracle::a_zero_buffer_size);
        EXPECT_EQ(call(0, 0, 0), oracle::a_no_data_no_size_status);
    }
}

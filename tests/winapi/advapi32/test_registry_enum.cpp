/// @file test_registry_enum.cpp
/// @brief RegEnumValueW/A, RegQueryInfoKeyW/A and byte-exact value storage,
///        through the exported thunks.
///
/// Expected values: tools/oracle_probe/s4_registry_probe.expected (Windows 11,
/// identical on x86 and x64); each check names its probe line.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/advapi32/registry/registry.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::capsule;

namespace {

constexpr GuestAddr kStack = 0x80000;
constexpr GuestAddr kName = 0x10000;
constexpr GuestAddr kNameLen = 0x10200;
constexpr GuestAddr kType = 0x10210;
constexpr GuestAddr kData = 0x10300;
constexpr GuestAddr kDataLen = 0x10400;
constexpr GuestAddr kOuts = 0x10500;   // RegQueryInfoKey DWORD outputs
constexpr GuestAddr kFt = 0x10600;

class RegistryEnumTest : public ::testing::Test {
protected:
    void SetUp() override {
        registry = std::make_unique<Registry>();
        handles = std::make_unique<HandleTable>();
        thunks = std::make_unique<RegistryThunks>(*registry, *handles);
        exports = create_advapi32_registry_thunks(*thunks);
        ASSERT_TRUE(memory.map_region(0x10000, 0x80000, MemoryProtection::ReadWrite).is_ok());
        u32 disp = 0;
        ASSERT_EQ(thunks->reg_create_key_ex_w(HKEY_CURRENT_USER, L"Software\\VitreoS4Probe",
                                              &key, &disp), ERROR_SUCCESS);
        // The probe's values, in the probe's creation order.
        const u32 dw = 0x11223344;
        set(L"alpha", REG_DWORD, &dw, 4);
        set(L"bravo_long_name", REG_SZ, u"hello", 12);
        set(L"c", REG_BINARY, "\x01\x02\x03\x04\x05\x06\x07", 7);
        set(L"multi", REG_MULTI_SZ, u"ab\0cd\0", 14);
        set(L"", REG_SZ, u"def", 8);
        u64 sub = 0;
        ASSERT_EQ(thunks->reg_create_key_ex_w(key, L"sub_one", &sub, &disp), ERROR_SUCCESS);
        ASSERT_EQ(thunks->reg_create_key_ex_w(key, L"sub_twotwo", &sub, &disp), ERROR_SUCCESS);
    }

    void set(const std::wstring& name, u32 type, const void* data, u32 size) {
        ASSERT_EQ(thunks->reg_set_value_ex_w(key, name, type,
                                             static_cast<const u8*>(data), size),
                  ERROR_SUCCESS);
    }

    const ExportEntry& find(std::string_view name) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        return *it;
    }

    u32 call(std::string_view name, const std::vector<u64>& args) {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.set_sp(kStack);
        (void)memory.write_u32(kStack, 0xDEADBEEF);
        for (usize i = 0; i < args.size(); ++i) {
            (void)memory.write_u32(kStack + 4 + static_cast<GuestAddr>(i) * 4,
                                   static_cast<u32>(args[i]));
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        find(name).handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    struct Enum {
        u32 r, cch, type, cb, d0, n0;
        std::string name;
    };
    /// One probe row: 64-unit '#' name buffer, 0xCC data, type 0xAAAA.
    Enum enum_value(bool wide, u64 hkey, u32 index, u32 cch, bool name_null,
                    int data_mode, u32 cb) {
        for (u32 i = 0; i < 128; ++i) (void)memory.write_u8(kName + i, wide ? (i % 2 ? 0 : '#') : '#');
        for (u32 i = 0; i < 64; ++i) (void)memory.write_u8(kData + i, 0xCC);
        (void)memory.write_u32(kNameLen, cch);
        (void)memory.write_u32(kType, 0xAAAA);
        (void)memory.write_u32(kDataLen, cb);
        const bool with_data = data_mode == 1 || data_mode == 3;
        const bool with_size = data_mode == 1 || data_mode == 2;
        Enum e{};
        e.r = call(wide ? "RegEnumValueW" : "RegEnumValueA",
                   {hkey, index, name_null ? 0 : kName, kNameLen, 0, kType,
                    with_data ? kData : 0, with_size ? kDataLen : 0});
        e.cch = memory.read_u32(kNameLen).value_or(0);
        e.type = memory.read_u32(kType).value_or(0);
        e.cb = memory.read_u32(kDataLen).value_or(0);
        e.d0 = memory.read_u8(kData).value_or(0);
        e.n0 = wide ? memory.read_u16(kName).value_or(0) : memory.read_u8(kName).value_or(0);
        for (u32 i = 0; i < 20; ++i) {
            const u32 ch = wide ? memory.read_u16(kName + i * 2).value_or(0)
                                : memory.read_u8(kName + i).value_or(0);
            if (ch == 0) break;
            e.name.push_back(static_cast<char>(ch));
        }
        return e;
    }

    GuestMemory memory;
    std::unique_ptr<Registry> registry;
    std::unique_ptr<HandleTable> handles;
    std::unique_ptr<RegistryThunks> thunks;
    DllExports exports;
    u64 key = 0;
};

TEST_F(RegistryEnumTest, ValuesEnumerateInCreationOrder) {
    // enumw_all_0..4
    const char* names[] = {"alpha", "bravo_long_name", "c", "multi", ""};
    const u32 types[] = {4, 1, 3, 7, 1};
    const u32 cbs[] = {4, 0xc, 7, 0xe, 8};
    for (u32 i = 0; i < 5; ++i) {
        const Enum e = enum_value(true, key, i, 64, false, 1, 64);
        EXPECT_EQ(e.r, 0u) << i;
        EXPECT_EQ(e.name, names[i]) << i;
        EXPECT_EQ(e.type, types[i]) << i;
        EXPECT_EQ(e.cb, cbs[i]) << i;
    }
    // Overwriting keeps the slot.
    const u32 dw = 7;
    set(L"alpha", REG_DWORD, &dw, 4);
    EXPECT_EQ(enum_value(true, key, 0, 64, false, 1, 64).name, "alpha");
    // enumw_all_5 / enumw_past_end: NO_MORE_ITEMS, W stores REG_NONE.
    const Enum end = enum_value(true, key, 5, 64, false, 1, 64);
    EXPECT_EQ(end.r, 0x103u);
    EXPECT_EQ(end.type, 0u);
    EXPECT_EQ(end.cch, 0x40u);
    EXPECT_EQ(end.cb, 0x40u);
    // enumw_bad_key
    const Enum bad = enum_value(true, 0x1234, 0, 64, false, 1, 64);
    EXPECT_EQ(bad.r, 6u);
    EXPECT_EQ(bad.type, 0u);
}

TEST_F(RegistryEnumTest, EnumValueWBufferRulesMatchWindows) {
    Enum e = enum_value(true, key, 1, 15, false, 0, 0);          // enumw_name_exact
    EXPECT_EQ(e.r, 0xeau); EXPECT_EQ(e.cch, 0xfu); EXPECT_EQ(e.type, 1u);
    EXPECT_EQ(e.n0, static_cast<u32>('#'));
    e = enum_value(true, key, 1, 16, false, 0, 0);               // enumw_name_plus1
    EXPECT_EQ(e.r, 0u); EXPECT_EQ(e.cch, 0xfu); EXPECT_EQ(e.name, "bravo_long_name");
    e = enum_value(true, key, 1, 0, false, 0, 0);                // enumw_name_zero
    EXPECT_EQ(e.r, 0xeau); EXPECT_EQ(e.cch, 0u);
    e = enum_value(true, key, 1, 16, true, 0, 0);                // enumw_name_null
    EXPECT_EQ(e.r, 0x57u); EXPECT_EQ(e.cch, 0x10u); EXPECT_EQ(e.type, 0xaaaau);
    e = enum_value(true, key, 1, 64, false, 1, 4);               // enumw_data_small
    EXPECT_EQ(e.r, 0xeau); EXPECT_EQ(e.cch, 0xfu); EXPECT_EQ(e.cb, 0xcu);
    EXPECT_EQ(e.d0, 0xccu); EXPECT_EQ(e.name, "bravo_long_name");
    e = enum_value(true, key, 1, 64, false, 1, 10);              // enumw_data_nonul
    EXPECT_EQ(e.r, 0xeau); EXPECT_EQ(e.cb, 0xcu);
    e = enum_value(true, key, 1, 64, false, 2, 0);               // enumw_data_sizeonly
    EXPECT_EQ(e.r, 0u); EXPECT_EQ(e.cb, 0xcu); EXPECT_EQ(e.d0, 0xccu);
    e = enum_value(true, key, 1, 64, false, 3, 0);               // enumw_data_noptr_size
    EXPECT_EQ(e.r, 0x57u); EXPECT_EQ(e.cch, 0x40u); EXPECT_EQ(e.type, 0xaaaau);
    e = enum_value(true, key, 1, 4, false, 1, 4);                // enumw_both_small
    EXPECT_EQ(e.r, 0xeau); EXPECT_EQ(e.cch, 4u); EXPECT_EQ(e.type, 1u);
    EXPECT_EQ(e.cb, 0xcu); EXPECT_EQ(e.n0, static_cast<u32>('#'));
}

TEST_F(RegistryEnumTest, EnumValueAIsImplementedWithItsOwnBufferRules) {
    Enum e = enum_value(false, key, 1, 64, false, 1, 64);        // enuma_all_1
    EXPECT_EQ(e.r, 0u); EXPECT_EQ(e.cch, 0xfu); EXPECT_EQ(e.type, 1u);
    EXPECT_EQ(e.cb, 6u); EXPECT_EQ(e.d0, static_cast<u32>('h'));
    e = enum_value(false, key, 1, 15, false, 0, 0);              // enuma_name_exact
    EXPECT_EQ(e.r, 0xeau); EXPECT_EQ(e.cch, 0xfu); EXPECT_EQ(e.name, "bravo_long_nam");
    e = enum_value(false, key, 1, 64, false, 1, 4);              // enuma_data_small
    EXPECT_EQ(e.r, 0xeau); EXPECT_EQ(e.cch, 0x40u); EXPECT_EQ(e.type, 1u);
    EXPECT_EQ(e.cb, 6u); EXPECT_EQ(e.n0, static_cast<u32>('#'));
    e = enum_value(false, key, 3, 64, false, 1, 64);             // enuma_multi
    EXPECT_EQ(e.r, 0u); EXPECT_EQ(e.type, 7u); EXPECT_EQ(e.cb, 7u);
    const Enum end = enum_value(false, key, 5, 64, false, 1, 64);// enuma_all_5
    EXPECT_EQ(end.r, 0x103u); EXPECT_EQ(end.type, 0xaaaau);
}

TEST_F(RegistryEnumTest, QueryInfoKeyReportsClassAndMaximumSizes) {
    for (bool wide : {true, false}) {
        for (u32 i = 0; i < 8; ++i) (void)memory.write_u32(kOuts + i * 4, 0xAAAA);
        (void)memory.write_u32(kName, 0x23232323);
        (void)memory.write_u32(kNameLen, 16);
        const u32 r = call(wide ? "RegQueryInfoKeyW" : "RegQueryInfoKeyA",
                           {key, kName, kNameLen, 0, kOuts, kOuts + 4, kOuts + 8,
                            kOuts + 12, kOuts + 16, kOuts + 20, kOuts + 24, kFt});
        // qikw / qika: ccls=0 cls0=0 nsub=2 msub=a mcls=0 nval=5 mvn=f mvd=e
        EXPECT_EQ(r, 0u);
        EXPECT_EQ(memory.read_u32(kNameLen).value_or(1), 0u);
        EXPECT_EQ(wide ? memory.read_u16(kName).value_or(1) : memory.read_u8(kName).value_or(1), 0u);
        const u32 expected[6] = {2, 0xa, 0, 5, 0xf, 0xe};
        for (u32 i = 0; i < 6; ++i) {
            EXPECT_EQ(memory.read_u32(kOuts + i * 4).value_or(0), expected[i]) << i;
        }
    }
    // qikw_cls_no_len
    EXPECT_EQ(call("RegQueryInfoKeyW", {key, kName, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}), 0x57u);
    // qikw_bad_key: ERROR_INVALID_HANDLE, *lpcSubKeys zeroed.
    (void)memory.write_u32(kOuts, 0xAAAA);
    EXPECT_EQ(call("RegQueryInfoKeyW", {0x1234, 0, 0, 0, kOuts, 0, 0, 0, 0, 0, 0, 0}), 6u);
    EXPECT_EQ(memory.read_u32(kOuts).value_or(1), 0u);
}

TEST_F(RegistryEnumTest, ValuesTheTypedStoreCannotHoldStayByteExact) {
    // Learn (RegSetValueEx / Registry Value Types): the stored bytes come
    // back unchanged. These used to be stored as an empty REG_NONE.
    struct Row { const wchar_t* name; u32 type; std::vector<u8> bytes; };
    const Row rows[] = {
        {L"short_dword", REG_DWORD, {0x01, 0x02}},
        {L"none_data", REG_NONE, {0xAA, 0xBB, 0xCC}},
        {L"big_endian", 5 /*REG_DWORD_BIG_ENDIAN*/, {0, 0, 0, 1}},
        {L"odd_type", 0x1234, {9, 8, 7, 6, 5}},
        {L"bad_multi", REG_MULTI_SZ, {'a', 0, 'b', 0}},
    };
    for (const auto& row : rows) {
        set(row.name, row.type, row.bytes.data(), static_cast<u32>(row.bytes.size()));
    }
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("vitreo_regraw_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    registry->save(dir);
    Registry reloaded;
    reloaded.load(dir);
    for (Registry* reg : {registry.get(), &reloaded}) {
        auto h = reg->open_key(HKEY_CURRENT_USER, L"Software\\VitreoS4Probe", false);
        ASSERT_TRUE(h.is_ok());
        for (const auto& row : rows) {
            auto v = reg->query_value(h.value(), row.name);
            ASSERT_TRUE(v.is_ok());
            EXPECT_EQ(RegistryThunks::reg_type_to_win32(v.value()), row.type);
            const u32 n = RegistryThunks::serialise_value(v.value(), nullptr, 0);
            std::vector<u8> out(n);
            (void)RegistryThunks::serialise_value(v.value(), out.data(), n);
            EXPECT_EQ(out, row.bytes);
        }
        // A well-formed REG_MULTI_SZ stays typed and returns its 14 bytes.
        auto multi = reg->query_value(h.value(), L"multi");
        ASSERT_TRUE(multi.is_ok());
        EXPECT_EQ(multi.value().type, RegType::MultiSz);
        EXPECT_EQ(RegistryThunks::serialise_value(multi.value(), nullptr, 0), 14u);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

}  // namespace

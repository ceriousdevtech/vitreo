// CompareString A/W contract subset: Windows compare_string_probe.expected,
// Microsoft Learn LCID/codepage/count rules; unsupported cases fail explicitly.
#include <gtest/gtest.h>
#include "winapi/kernel32/locale/locale_string.hpp"
#include "winapi/kernel32/locale/locale_string_internal.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "../../../tools/oracle_probe/compare_locale.generated.inc"
#include <algorithm>
#include <array>
#include <tuple>
using namespace vitreo;
using namespace vitreo::winapi;
namespace sdk = compare_locale_sdk;
namespace {
class CompareStringContract : public ::testing::TestWithParam<std::tuple<bool, bool>> {
protected:
    GuestMemory memory;
    LocaleStringManager manager;
    DllExports exports;
    bool is64() const { return std::get<0>(GetParam()); }
    bool wide() const { return std::get<1>(GetParam()); }
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000, 0x6000, MemoryProtection::ReadWrite).is_ok());
        exports = create_locale_string_thunks(manager);
    }
    void text(GuestAddr address, std::u16string_view value) {
        for (usize i = 0; i <= value.size(); ++i) {
            const auto ch = i == value.size() ? 0 : value[i];
            if (wide()) (void)memory.write_u16(address + i * 2, static_cast<u16>(ch));
            else (void)memory.write_u8(address + i, static_cast<u8>(ch));
        }
    }
    u32 call(u32 locale, u32 flags, GuestAddr first = 0x11000, i32 first_count = -1,
        GuestAddr second = 0x12000, i32 second_count = -1) {
        const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(), [&](const auto& e) {
            return e.name == (wide() ? "CompareStringW" : "CompareStringA"); });
        EXPECT_NE(entry, exports.entries.end()); if (entry == exports.entries.end()) return ~0u;
        const std::array<u64, 6> args{locale, flags, first, static_cast<u64>(first_count), second, static_cast<u64>(second_count)};
        cpu::CpuState state;
        if (is64()) {
            state.init_long_mode(); state.gpr.rsp = 0x14000;
            const std::array<u64*, 4> regs{&state.gpr.rcx, &state.gpr.rdx, &state.gpr.r8, &state.gpr.r9};
            for (usize i = 0; i < args.size(); ++i) {
                if (i < 4) *regs[i] = args[i];
                else (void)memory.write_u64(0x14000 + (i + 1) * 8, args[i]);
            }
        } else {
            state.init_protected_mode(); state.set_sp(0x14000);
            for (usize i = 0; i < args.size(); ++i) (void)memory.write_u32(0x14000 + (i + 1) * 4, static_cast<u32>(args[i]));
        }
        Marshaler m(memory); m.attach_cpu(state); set_last_error(0xdead); entry->handler(m);
        return static_cast<u32>(state.gpr.rax);
    }
};
TEST_P(CompareStringContract, PreservesWindowsPositiveLinguisticCorpus) {
    // Exact expected results are from compare_string_probe.expected.
    const struct Case { const char16_t* a; const char16_t* b; u32 flags; u32 result; } cases[] = {
        {u"apple", u"Banana", 0, CSTR_LESS_THAN}, {u"Banana", u"apple", 0, CSTR_GREATER_THAN},
        {u"\u00c9", u"\u00e9", 0, CSTR_GREATER_THAN}, {u"\u00c9", u"\u00e9", NORM_IGNORECASE, CSTR_EQUAL},
        {u"coop", u"co-op", 0, CSTR_LESS_THAN}, {u"coop", u"co-op", SORT_STRINGSORT, CSTR_GREATER_THAN},
        {u"bills", u"bill's", 0, CSTR_LESS_THAN}, {u"bills", u"bill's", SORT_STRINGSORT, CSTR_GREATER_THAN},
        {u"2", u"10", 0, CSTR_GREATER_THAN}, {u"2", u"10", SORT_DIGITSASNUMBERS, CSTR_LESS_THAN},
        {u"a", u"\u00e1", 0, CSTR_LESS_THAN}, {u"a", u"\u00e1", NORM_IGNORENONSPACE, CSTR_EQUAL}};
    for (const auto& item : cases) {
        text(0x11000, item.a); text(0x12000, item.b);
        EXPECT_EQ(call(sdk::en_us, item.flags), item.result);
    }
}
TEST_P(CompareStringContract, LocaleAndFlagsAreNeverSilentlyIgnored) {
    text(0x11000, u"a"); text(0x12000, u"a");
    for (const u32 locale : {sdk::neutral, sdk::en_us, sdk::user_default, sdk::system_default})
        EXPECT_EQ(call(locale, 0), static_cast<u32>(CSTR_EQUAL));
    EXPECT_EQ(call(~sdk::encoded_lcid_mask, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(sdk::invariant, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    for (const u32 flag : {NORM_IGNOREKANATYPE, NORM_IGNOREWIDTH, NORM_IGNORESYMBOLS,
        LINGUISTIC_IGNORECASE, LINGUISTIC_IGNOREDIACRITIC, NORM_LINGUISTIC_CASING}) {
        EXPECT_EQ(call(sdk::en_us, flag), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED);
    }
    EXPECT_EQ(call(sdk::en_us, ~kCompareStringValidFlags), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
}
TEST_P(CompareStringContract, CountedAndNegativeLengthsRespectReadableGuestMemory) {
    text(0x11000, u"abc"); text(0x12000, u"ab");
    EXPECT_EQ(call(sdk::en_us, 0, 0x11000, 2, 0x12000, 2), static_cast<u32>(CSTR_EQUAL));
    EXPECT_EQ(call(sdk::en_us, 0, 0x11000, 3, 0x12000, 2), static_cast<u32>(CSTR_GREATER_THAN));
    EXPECT_EQ(call(sdk::en_us, 0, 0x11000, -2, 0x12000, -3), static_cast<u32>(CSTR_GREATER_THAN));
    EXPECT_EQ(call(sdk::en_us, 0, 0), 0u); EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    const GuestAddr edge = wide() ? 0x15ffe : 0x15fff;
    if (wide()) (void)memory.write_u16(edge, 'a'); else (void)memory.write_u8(edge, 'a');
    for (const i32 count : {2, -1}) {
        EXPECT_EQ(call(sdk::en_us, 0, edge, count), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    }
}
TEST_P(CompareStringContract, NegativeLengthHasNoGuessed65536CharacterTruncation) {
    ASSERT_TRUE(memory.map_region(0x20000, 0x30000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(0x60000, 0x30000, MemoryProtection::ReadWrite).is_ok());
    const usize unit = wide() ? 2 : 1;
    std::vector<u8> bytes(65538 * unit, 0);
    for (usize i = 0; i < 65537; ++i) bytes[i * unit] = 'a';
    ASSERT_TRUE(memory.write_bytes(0x20000, reinterpret_cast<const Byte*>(bytes.data()), bytes.size()).is_ok());
    bytes[65536 * unit] = 'b';
    ASSERT_TRUE(memory.write_bytes(0x60000, reinterpret_cast<const Byte*>(bytes.data()), bytes.size()).is_ok());
    EXPECT_EQ(call(sdk::en_us, 0, 0x20000, -1, 0x60000, -1), static_cast<u32>(CSTR_LESS_THAN));
}
TEST_P(CompareStringContract, AnsiUsesSuppliedLocaleCodepageWhenProcessAcpIsUtf8) {
    if (wide()) return;
    const u32 saved = codepage::ansi_code_page(); codepage::set_ansi_code_page(codepage::kCpUtf8);
    text(0x11000, u"\u00c9"); text(0x12000, u"\u00e9");
    const auto result = call(sdk::en_us, 0);
    codepage::set_ansi_code_page(saved);
    EXPECT_EQ(result, static_cast<u32>(CSTR_GREATER_THAN));
}
INSTANTIATE_TEST_SUITE_P(BothAbisAndEncodings, CompareStringContract,
    ::testing::Combine(::testing::Bool(), ::testing::Bool()));
} // namespace

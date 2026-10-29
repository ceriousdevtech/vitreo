/// @file test_crt_locale.cpp
/// @brief Tests for guest-visible UCRT C-locale state

#include <gtest/gtest.h>
#include <winapi/msvcrt/text/crt_locale.hpp>
#include <winapi/msvcrt/memory/crt_memory.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

std::string read_ascii(GuestMemory& memory, GuestAddr address) {
    std::string result;
    for (;;) {
        const u8 ch = memory.read_u8(address + result.size()).value_or(0);
        if (ch == 0) return result;
        result.push_back(static_cast<char>(ch));
    }
}

std::u16string read_utf16(GuestMemory& memory, GuestAddr address) {
    std::u16string result;
    for (;;) {
        const u16 ch =
            memory.read_u16(address + result.size() * sizeof(u16)).value_or(0);
        if (ch == 0) return result;
        result.push_back(static_cast<char16_t>(ch));
    }
}

} // namespace

class CrtLocaleTest : public ::testing::Test {
protected:
    GuestMemory memory;
    CrtMemoryManager memory_manager{memory, 0x20000000, 0x10000};
    CrtLocaleManager manager{memory, memory_manager};
};

TEST_F(CrtLocaleTest, CLocaleStateIsGuestReadable) {
    ASSERT_TRUE(manager.valid());
    ASSERT_NE(manager.pctype_addr(), 0u);

    EXPECT_EQ(memory.read_u16(manager.pctype_addr() + 'A' * 2).value(),
              0x0181u);
    EXPECT_EQ(memory.read_u16(manager.pctype_addr() + 'a' * 2).value(),
              0x0182u);
    EXPECT_EQ(memory.read_u16(manager.pctype_addr() + '9' * 2).value(),
              0x0084u);
    EXPECT_EQ(memory.read_u16(manager.pctype_addr() + ' ' * 2).value(),
              0x0048u);
    EXPECT_EQ(memory.read_u16(manager.pctype_addr() + '!' * 2).value(),
              0x0010u);
    EXPECT_EQ(memory.read_u16(manager.pctype_addr() + 0x80 * 2).value(), 0u);
}

TEST_F(CrtLocaleTest, LocaleNameArraysUseGuestPointerWidth) {
    ASSERT_TRUE(manager.valid());
    ASSERT_NE(manager.locale_names_addr(false), 0u);
    ASSERT_NE(manager.locale_names_addr(true), 0u);

    for (usize index = 0; index < 6; ++index) {
        EXPECT_EQ(memory.read_u32(
            manager.locale_names_addr(false) + index * 4).value(), 0u);
        EXPECT_EQ(memory.read_u64(
            manager.locale_names_addr(true) + index * 8).value(), 0u);
    }
}

TEST_F(CrtLocaleTest, LocaleConventionsMatchCLocaleForBothGuestModes) {
    ASSERT_TRUE(manager.valid());

    const GuestAddr lconv32 = manager.locale_conventions_addr(false);
    const GuestAddr decimal32 = memory.read_u32(lconv32).value();
    ASSERT_NE(decimal32, 0u);
    EXPECT_EQ(memory.read_u8(decimal32).value(), static_cast<u8>('.'));
    EXPECT_EQ(memory.read_u8(lconv32 + 40).value(),
              static_cast<u8>(CHAR_MAX));

    const GuestAddr lconv64 = manager.locale_conventions_addr(true);
    const GuestAddr decimal64 = memory.read_u64(lconv64).value();
    ASSERT_NE(decimal64, 0u);
    EXPECT_EQ(memory.read_u8(decimal64).value(), static_cast<u8>('.'));
    EXPECT_EQ(memory.read_u8(lconv64 + 80).value(),
              static_cast<u8>(CHAR_MAX));
}

TEST_F(CrtLocaleTest, UnsignedCharInitSetsEveryLconvCharMemberToUcharMax) {
    ASSERT_TRUE(manager.valid());
    ASSERT_TRUE(manager.initialize_lconv_for_unsigned_char());

    const GuestAddr lconv32 = manager.locale_conventions_addr(false);
    const GuestAddr lconv64 = manager.locale_conventions_addr(true);
    for (usize index = 0; index < 8; ++index) {
        EXPECT_EQ(memory.read_u8(lconv32 + 40 + index).value(), UCHAR_MAX);
        EXPECT_EQ(memory.read_u8(lconv64 + 80 + index).value(), UCHAR_MAX);
    }
    // String members are untouched: decimal_point is still ".".
    EXPECT_EQ(memory.read_u8(memory.read_u32(lconv32).value()).value(),
              static_cast<u8>('.'));
    EXPECT_EQ(memory.read_u8(memory.read_u64(lconv64).value()).value(),
              static_cast<u8>('.'));
}

TEST_F(CrtLocaleTest, ExportsCoverUcrtLocaleApiSetImports) {
    const DllExports exports = create_crt_locale_thunks(manager);
    constexpr std::array<const char*, 13> required = {
        "__initialize_lconv_for_unsigned_char",
        "_lock_locales",
        "_unlock_locales",
        "__pctype_func",
        "___lc_codepage_func",
        "___lc_collate_cp_func",
        "___mb_cur_max_func",
        "___lc_locale_name_func",
        "localeconv",
        "_Getdays",
        "_Getmonths",
        "_W_Getdays",
        "_W_Getmonths",
    };

    for (const char* name : required) {
        EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(),
                               [name](const ExportEntry& entry) {
                                   return entry.name == name;
                               }),
                  exports.entries.end())
            << name;
    }
}

TEST_F(CrtLocaleTest, CLocaleScalarContractsAreStable) {
    EXPECT_EQ(CrtLocaleManager::code_page(), 0u);
    EXPECT_EQ(CrtLocaleManager::collate_code_page(), 0u);
    EXPECT_EQ(CrtLocaleManager::multibyte_max(), 1);
}

TEST_F(CrtLocaleTest, TimeNameStringsAreAllocatedInCrtHeap) {
    const GuestAddr days = manager.allocate_days(false);
    const GuestAddr months = manager.allocate_months(false);
    const GuestAddr wide_days = manager.allocate_days(true);
    const GuestAddr wide_months = manager.allocate_months(true);

    ASSERT_NE(days, 0u);
    ASSERT_NE(months, 0u);
    ASSERT_NE(wide_days, 0u);
    ASSERT_NE(wide_months, 0u);
    EXPECT_NE(days, manager.allocate_days(false));

    EXPECT_EQ(read_ascii(memory, days),
              ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:"
              "Thu:Thursday:Fri:Friday:Sat:Saturday");
    EXPECT_EQ(read_ascii(memory, months),
              ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:"
              "Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:"
              "Nov:November:Dec:December");
    EXPECT_EQ(read_utf16(memory, wide_days),
              u":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:"
              u"Thu:Thursday:Fri:Friday:Sat:Saturday");
    EXPECT_EQ(read_utf16(memory, wide_months),
              u":Jan:January:Feb:February:Mar:March:Apr:April:May:May:"
              u"Jun:June:Jul:July:Aug:August:Sep:September:Oct:October:"
              u"Nov:November:Dec:December");
}

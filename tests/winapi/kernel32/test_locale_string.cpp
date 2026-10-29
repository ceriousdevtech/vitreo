/// @file test_locale_string.cpp
/// @brief Unit tests for locale string/mapping implementations (LCMapStringA, GetStringTypeA, GetLocaleInfoA)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/locale/locale_string.hpp>
#include <winapi/kernel32/locale/codepage_convert.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

// =============================================================================
// Test Fixture
// =============================================================================

class LocaleStringTest : public ::testing::Test {
protected:
    static constexpr GuestAddr MEM_BASE   = 0x1000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr SRC_BUF    = 0x2000;
    static constexpr GuestAddr DST_BUF    = 0x3000;

    void SetUp() override {
        m_mgr = std::make_unique<LocaleStringManager>();
        (void)memory_.map_region(MEM_BASE, 0x10000, MemoryProtection::ReadWrite);
        (void)memory_.map_region(STACK_BASE, 0x10000, MemoryProtection::ReadWrite);
        cpu_.gpr.rsp = STACK_BASE + 0x800;
    }

    ExportEntry find_export(const std::string& name) {
        DllExports exports = create_locale_string_thunks(*m_mgr);
        for (const auto& e : exports.entries) {
            if (e.name == name) return e;
        }
        ADD_FAILURE() << name << " not found in locale_string thunks";
        return {};
    }

    void write_ascii_string(GuestAddr addr, const char* str) {
        for (size_t i = 0; str[i] != '\0'; ++i) {
            (void)memory_.write_u8(addr + static_cast<u32>(i), static_cast<u8>(str[i]));
        }
        (void)memory_.write_u8(addr + static_cast<u32>(strlen(str)), 0);
    }

    /// Write a u64 to the stack for args >=4 (fastcall: RSP + 0x28 + (idx-4)*8)
    void set_stack_arg(usize index, u64 value) {
        GuestAddr addr = static_cast<GuestAddr>(cpu_.gpr.rsp) + 0x28 + static_cast<u32>((index - 4) * 8);
        (void)memory_.write_u64(addr, value);
    }

    std::unique_ptr<LocaleStringManager> m_mgr;
    GuestMemory memory_;
    cpu::CpuState cpu_;
};

TEST_F(LocaleStringTest, WakeAllConditionVariableDoesNotLeakThunkAddressInRax) {
    auto entry = find_export("WakeAllConditionVariable");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = SRC_BUF;
    cpu_.gpr.rax = 0x00007ffb00000ff0ULL;
    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
}

// =============================================================================
// LCMapStringA Tests
// =============================================================================

TEST_F(LocaleStringTest, LCMapStringAUppercase) {
    auto entry = find_export("LCMapStringA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_ascii_string(SRC_BUF, "hello");

    constexpr u32 LCMAP_UPPERCASE = 0x00000200;
    // x64 fastcall: rcx=locale, rdx=flags, r8=src, r9=srcLen
    cpu_.gpr.rcx = 0x0409;          // LCID = en-US
    cpu_.gpr.rdx = LCMAP_UPPERCASE;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(-1); // null-terminated
    // Stack args: arg(4)=dst, arg(5)=dstLen
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, 256);

    entry.handler(m);

    EXPECT_GT(cpu_.gpr.rax, 0u); // success

    // Read result
    auto h = memory_.read_u8(DST_BUF);
    ASSERT_TRUE(h.is_ok());
    EXPECT_EQ(h.value(), 'H');

    auto e = memory_.read_u8(DST_BUF + 1);
    ASSERT_TRUE(e.is_ok());
    EXPECT_EQ(e.value(), 'E');
}

TEST_F(LocaleStringTest, LCMapStringALowercase) {
    auto entry = find_export("LCMapStringA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_ascii_string(SRC_BUF, "WORLD");

    constexpr u32 LCMAP_LOWERCASE = 0x00000100;
    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = LCMAP_LOWERCASE;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(-1);
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, 256);

    entry.handler(m);

    EXPECT_GT(cpu_.gpr.rax, 0u);

    auto w = memory_.read_u8(DST_BUF);
    ASSERT_TRUE(w.is_ok());
    EXPECT_EQ(w.value(), 'w');
}

TEST_F(LocaleStringTest, LCMapStringAQueryLength) {
    auto entry = find_export("LCMapStringA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_ascii_string(SRC_BUF, "test");

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = 0x00000200; // LCMAP_UPPERCASE
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(-1);
    set_stack_arg(4, 0); // dst=NULL
    set_stack_arg(5, 0); // dstLen=0

    entry.handler(m);

    // Should return required length (5 = "test" + NUL)
    EXPECT_EQ(cpu_.gpr.rax, 5u);
}

// =============================================================================
// GetStringTypeA Tests
// =============================================================================

TEST_F(LocaleStringTest, GetStringTypeADigit) {
    auto entry = find_export("GetStringTypeA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_ascii_string(SRC_BUF, "5");

    // x64 fastcall: rcx=locale, rdx=infoType, r8=src, r9=srcLen
    cpu_.gpr.rcx = 0x0409;   // LCID
    cpu_.gpr.rdx = 1;        // CT_CTYPE1
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = 1;        // cchSrc = 1
    set_stack_arg(4, DST_BUF); // lpCharType

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u); // TRUE

    auto ct = memory_.read_u16(DST_BUF);
    ASSERT_TRUE(ct.is_ok());
    EXPECT_EQ(ct.value() & 0x0004, 0x0004u); // C1_DIGIT
}

TEST_F(LocaleStringTest, GetStringTypeAUpperAlpha) {
    auto entry = find_export("GetStringTypeA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_ascii_string(SRC_BUF, "A");

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = 1;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = 1;
    set_stack_arg(4, DST_BUF);

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u);

    auto ct = memory_.read_u16(DST_BUF);
    ASSERT_TRUE(ct.is_ok());
    EXPECT_NE(ct.value() & 0x0001, 0u); // C1_UPPER
    EXPECT_NE(ct.value() & 0x0100, 0u); // C1_ALPHA
}

// =============================================================================
// GetLocaleInfoA Tests
// =============================================================================

TEST_F(LocaleStringTest, GetLocaleInfoWNoUserOverrideKeepsDecimal) {
    auto entry = find_export("GetLocaleInfoW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    constexpr u32 LOCALE_SDECIMAL = 0x000E;
    constexpr u32 LOCALE_NOUSEROVERRIDE = 0x80000000u;
    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = LOCALE_SDECIMAL | LOCALE_NOUSEROVERRIDE;
    cpu_.gpr.r8  = DST_BUF;
    cpu_.gpr.r9  = 16;
    entry.handler(m);
    EXPECT_GT(cpu_.gpr.rax, 0u);
    EXPECT_EQ(memory_.read_u16(DST_BUF).value(), u'.');
}

TEST_F(LocaleStringTest, GetLocaleInfoWUnknownTypeFails) {
    auto entry = find_export("GetLocaleInfoW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = 0x00FE;
    cpu_.gpr.r8  = DST_BUF;
    cpu_.gpr.r9  = 16;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
}

TEST_F(LocaleStringTest, GetLocaleInfoWReturnNumber) {
    auto entry = find_export("GetLocaleInfoW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    constexpr u32 LOCALE_ILANGUAGE = 0x0001;
    constexpr u32 LOCALE_SDECIMAL = 0x000E;
    constexpr u32 LOCALE_RETURN_NUMBER = 0x20000000u;

    // ILANGUAGE | RETURN_NUMBER → hex parse "0409" → 0x409
    set_last_error(0);
    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = LOCALE_ILANGUAGE | LOCALE_RETURN_NUMBER;
    cpu_.gpr.r8  = DST_BUF;
    cpu_.gpr.r9  = 2;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 2u);
    EXPECT_EQ(memory_.read_u32(DST_BUF).value(), 0x409u);

    // String type + RETURN_NUMBER → ERROR_INVALID_FLAGS
    set_last_error(0);
    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = LOCALE_SDECIMAL | LOCALE_RETURN_NUMBER;
    cpu_.gpr.r8  = DST_BUF;
    cpu_.gpr.r9  = 2;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);

    // Too-small buffer for number → ERROR_INSUFFICIENT_BUFFER
    set_last_error(0);
    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = LOCALE_ILANGUAGE | LOCALE_RETURN_NUMBER;
    cpu_.gpr.r8  = DST_BUF;
    cpu_.gpr.r9  = 1;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

TEST_F(LocaleStringTest, IsValidLocaleExactLcidAndFlags) {
    auto entry = find_export("IsValidLocale");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    constexpr u32 LCID_INSTALLED = 1;
    constexpr u32 LCID_SUPPORTED = 2;

    auto call = [&](u32 locale, u32 flags) -> u64 {
        cpu_.gpr.rcx = locale;
        cpu_.gpr.rdx = flags;
        entry.handler(m);
        return cpu_.gpr.rax;
    };

    EXPECT_EQ(call(0x0409, LCID_INSTALLED), 1u);
    EXPECT_EQ(call(0x0415, LCID_INSTALLED), 1u); // pl-PL
    EXPECT_EQ(call(0x041D, LCID_SUPPORTED), 1u); // sv-SE
    EXPECT_EQ(call(0x10409, LCID_INSTALLED), 0u); // not primary-lang-only
    EXPECT_EQ(call(0x0409, LCID_INSTALLED | LCID_SUPPORTED), 0u);
    EXPECT_EQ(call(0, LCID_INSTALLED), 0u);
    EXPECT_EQ(call(0x0400, LCID_INSTALLED), 0u);
}

TEST_F(LocaleStringTest, GetLocaleInfoADecimalSep) {
    auto entry = find_export("GetLocaleInfoA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 LOCALE_SDECIMAL = 0x000E;

    // x64 fastcall: rcx=locale, rdx=lcType, r8=buf, r9=cchData
    cpu_.gpr.rcx = 0x0409;       // LCID
    cpu_.gpr.rdx = LOCALE_SDECIMAL;
    cpu_.gpr.r8  = DST_BUF;
    cpu_.gpr.r9  = 256;

    entry.handler(m);

    EXPECT_GT(cpu_.gpr.rax, 0u); // success

    auto ch = memory_.read_u8(DST_BUF);
    ASSERT_TRUE(ch.is_ok());
    EXPECT_EQ(ch.value(), '.'); // en-US decimal separator
}

TEST_F(LocaleStringTest, GetLocaleInfoAQueryRequiredSize) {
    auto entry = find_export("GetLocaleInfoA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 LOCALE_SCURRENCY = 0x001F;

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = LOCALE_SCURRENCY;
    cpu_.gpr.r8  = 0;   // NULL buffer
    cpu_.gpr.r9  = 0;   // cchData = 0

    entry.handler(m);

    // Should return required size including NUL for "$"
    EXPECT_EQ(cpu_.gpr.rax, 2u); // "$" + NUL = 2
}

TEST_F(LocaleStringTest, GetLocaleInfoAThousandSep) {
    auto entry = find_export("GetLocaleInfoA");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 LOCALE_STHOUSAND = 0x000F;

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = LOCALE_STHOUSAND;
    cpu_.gpr.r8  = DST_BUF;
    cpu_.gpr.r9  = 256;

    entry.handler(m);

    EXPECT_GT(cpu_.gpr.rax, 0u);

    auto ch = memory_.read_u8(DST_BUF);
    ASSERT_TRUE(ch.is_ok());
    EXPECT_EQ(ch.value(), ','); // en-US thousand separator
}

// =============================================================================
// CompareStringEx Tests
// =============================================================================

void write_utf16(GuestMemory& memory, GuestAddr addr, const char16_t* s, i32 explicit_len = -1) {
    i32 n = 0;
    if (explicit_len >= 0) {
        n = explicit_len;
        for (i32 i = 0; i < n; ++i) {
            (void)memory.write_u16(addr + static_cast<u64>(i) * 2,
                                   static_cast<u16>(s[i]));
        }
        return;
    }
    while (s[n] != 0) {
        (void)memory.write_u16(addr + static_cast<u64>(n) * 2,
                               static_cast<u16>(s[n]));
        ++n;
    }
    (void)memory.write_u16(addr + static_cast<u64>(n) * 2, 0);
}

TEST_F(LocaleStringTest, CompareStringWLinguisticOrderMatchesWindows) {
    // Windows compare_string_probe.expected: apple < Banana (not code-point).
    auto entry = find_export("CompareStringW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_utf16(memory_, SRC_BUF, u"apple");
    write_utf16(memory_, DST_BUF, u"Banana");

    constexpr u32 LOCALE_USER_DEFAULT = 0x0400;
    cpu_.gpr.rcx = LOCALE_USER_DEFAULT;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, static_cast<u64>(static_cast<i32>(-1)));

    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u); // CSTR_LESS_THAN
}

TEST_F(LocaleStringTest, CompareStringWIgnoreCaseFoldsLatin1) {
    auto entry = find_export("CompareStringW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_utf16(memory_, SRC_BUF, u"\u00C9"); // É
    write_utf16(memory_, DST_BUF, u"\u00E9"); // é

    constexpr u32 NORM_IGNORECASE = 0x1;
    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = NORM_IGNORECASE;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, static_cast<u64>(static_cast<i32>(-1)));

    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 2u); // CSTR_EQUAL
}

TEST_F(LocaleStringTest, CompareStringWWordSortSoftHyphen) {
    // Word sort: coop < co-op; SORT_STRINGSORT: coop > co-op.
    auto entry = find_export("CompareStringW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_utf16(memory_, SRC_BUF, u"coop");
    write_utf16(memory_, DST_BUF, u"co-op");

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, static_cast<u64>(static_cast<i32>(-1)));
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);

    constexpr u32 SORT_STRINGSORT = 0x00001000;
    cpu_.gpr.rdx = SORT_STRINGSORT;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 3u);
}

TEST_F(LocaleStringTest, CompareStringWDigitsAsNumbers) {
    auto entry = find_export("CompareStringW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_utf16(memory_, SRC_BUF, u"2");
    write_utf16(memory_, DST_BUF, u"10");

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, static_cast<u64>(static_cast<i32>(-1)));
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 3u);

    constexpr u32 SORT_DIGITSASNUMBERS = 0x00000008;
    cpu_.gpr.rdx = SORT_DIGITSASNUMBERS;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
}

TEST_F(LocaleStringTest, CompareStringWRejectsInvalidFlags) {
    auto entry = find_export("CompareStringW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_utf16(memory_, SRC_BUF, u"a");
    write_utf16(memory_, DST_BUF, u"a");

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = 0x40; // not a documented CompareString flag
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, static_cast<u64>(static_cast<i32>(-1)));
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
}

TEST_F(LocaleStringTest, LCMapStringExUppercasesCyrillicAndYDiaeresis) {
    // Windows lcmap_string_probe: а→А, ÿ→Ÿ
    auto entry = find_export("LCMapStringEx");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_utf16(memory_, SRC_BUF, u"\u0430\u00FF");
    constexpr u32 LCMAP_UPPERCASE = 0x00000200;
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = LCMAP_UPPERCASE;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, 64);
    set_stack_arg(6, 0);
    set_stack_arg(7, 0);
    set_stack_arg(8, 0);
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 3u); // А Ÿ NUL
    EXPECT_EQ(memory_.read_u16(DST_BUF).value(), 0x0410u);
    EXPECT_EQ(memory_.read_u16(DST_BUF + 2).value(), 0x0178u);
    EXPECT_EQ(memory_.read_u16(DST_BUF + 4).value(), 0u);
}

TEST_F(LocaleStringTest, LCMapStringExTitleCaseAndByteRev) {
    auto entry = find_export("LCMapStringEx");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    write_utf16(memory_, SRC_BUF, u"hello world");
    constexpr u32 LCMAP_TITLECASE = 0x00000300;
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = LCMAP_TITLECASE;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, 64);
    set_stack_arg(6, 0);
    set_stack_arg(7, 0);
    set_stack_arg(8, 0);
    entry.handler(m);
    EXPECT_EQ(memory_.read_u16(DST_BUF).value(), u'H');
    EXPECT_EQ(memory_.read_u16(DST_BUF + 12).value(), u'W');

    write_utf16(memory_, SRC_BUF, u"\u3450\u4822");
    constexpr u32 LCMAP_BYTEREV = 0x00000800;
    cpu_.gpr.rdx = LCMAP_BYTEREV;
    entry.handler(m);
    EXPECT_EQ(memory_.read_u16(DST_BUF).value(), 0x5034u);
    EXPECT_EQ(memory_.read_u16(DST_BUF + 2).value(), 0x2248u);
}

TEST_F(LocaleStringTest, LCMapStringExSortKeyOrdersLikeCompareString) {
    auto entry = find_export("LCMapStringEx");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr u32 LCMAP_SORTKEY = 0x00000400;
    constexpr GuestAddr KEY_A = 0x4000;
    constexpr GuestAddr KEY_B = 0x4100;
    (void)memory_.map_region(0x4000, 0x1000, MemoryProtection::ReadWrite);

    write_utf16(memory_, SRC_BUF, u"apple");
    write_utf16(memory_, DST_BUF, u"Banana");

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = LCMAP_SORTKEY;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = static_cast<u64>(static_cast<i32>(-1));
    set_stack_arg(4, KEY_A);
    set_stack_arg(5, 256);
    set_stack_arg(6, 0);
    set_stack_arg(7, 0);
    set_stack_arg(8, 0);
    entry.handler(m);
    const auto la = static_cast<i32>(cpu_.gpr.rax);
    ASSERT_GT(la, 0);

    cpu_.gpr.r8 = DST_BUF;
    set_stack_arg(4, KEY_B);
    entry.handler(m);
    const auto lb = static_cast<i32>(cpu_.gpr.rax);
    ASSERT_GT(lb, 0);

    std::vector<u8> ka(static_cast<usize>(la)), kb(static_cast<usize>(lb));
    for (i32 i = 0; i < la; ++i) {
        ka[static_cast<usize>(i)] =
            static_cast<u8>(memory_.read_u8(KEY_A + static_cast<u64>(i)).value());
    }
    for (i32 i = 0; i < lb; ++i) {
        kb[static_cast<usize>(i)] =
            static_cast<u8>(memory_.read_u8(KEY_B + static_cast<u64>(i)).value());
    }
    const int n = std::min(la, lb);
    EXPECT_LT(std::memcmp(ka.data(), kb.data(), static_cast<usize>(n)), 0);

    // IGNORECASE: É and é keys equal.
    write_utf16(memory_, SRC_BUF, u"\u00C9");
    write_utf16(memory_, DST_BUF, u"\u00E9");
    constexpr u32 NORM_IGNORECASE = 0x1;
    cpu_.gpr.rdx = LCMAP_SORTKEY | NORM_IGNORECASE;
    cpu_.gpr.r8  = SRC_BUF;
    set_stack_arg(4, KEY_A);
    entry.handler(m);
    const auto le = static_cast<i32>(cpu_.gpr.rax);
    cpu_.gpr.r8 = DST_BUF;
    set_stack_arg(4, KEY_B);
    entry.handler(m);
    const auto lee = static_cast<i32>(cpu_.gpr.rax);
    ASSERT_EQ(le, lee);
    for (i32 i = 0; i < le; ++i) {
        EXPECT_EQ(memory_.read_u8(KEY_A + static_cast<u64>(i)).value(),
                  memory_.read_u8(KEY_B + static_cast<u64>(i)).value());
    }
}

TEST_F(LocaleStringTest, CompareStringExRejectsNullStringPointers) {
    auto entry = find_export("CompareStringEx");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8  = 0;
    cpu_.gpr.r9  = 0;
    set_stack_arg(4, 0);
    set_stack_arg(5, 0);
    set_stack_arg(6, 0);
    set_stack_arg(7, 0);
    set_stack_arg(8, 0);

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_F(LocaleStringTest, CompareStringExAllowsNullLocaleAndEmptyString) {
    auto entry = find_export("CompareStringEx");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const char16_t value[] = u"steam";
    for (size_t i = 0; i < 5; ++i) {
        (void)memory_.write_u16(SRC_BUF + static_cast<u64>(i) * 2,
                                static_cast<u16>(value[i]));
    }
    (void)memory_.write_u16(SRC_BUF + 10, 0);
    (void)memory_.write_u16(DST_BUF, 0);

    constexpr u32 NORM_IGNORENONSPACE = 0x00000002;
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = NORM_IGNORENONSPACE;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = 5;
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, 0);
    set_stack_arg(6, 0);
    set_stack_arg(7, 0);
    set_stack_arg(8, 0);

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 3u);
}

TEST_F(LocaleStringTest, CompareStringExExplicitLengthCountsEmbeddedNulls) {
    auto entry = find_export("CompareStringEx");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    (void)memory_.write_u16(SRC_BUF, 0);
    (void)memory_.write_u16(SRC_BUF + 2, u'A');

    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8  = SRC_BUF;
    cpu_.gpr.r9  = 2;
    (void)memory_.write_u16(DST_BUF, 0);
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, 0);
    set_stack_arg(6, 0);
    set_stack_arg(7, 0);
    set_stack_arg(8, 0);

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 3u);
}

TEST_F(LocaleStringTest, GetThreadPreferredUILanguagesNullBufferQueriesSize) {
    auto entry = find_export("GetThreadPreferredUILanguages");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr GuestAddr language_count = 0x4000;
    constexpr GuestAddr buffer_size = 0x4008;
    (void)memory_.write_u32(language_count, 0);
    (void)memory_.write_u32(buffer_size, 0);
    set_last_error(0x1234);
    cpu_.gpr.rcx = 0x38;
    cpu_.gpr.rdx = language_count;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = buffer_size;

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(language_count).value(), 1u);
    EXPECT_EQ(memory_.read_u32(buffer_size).value(), 7u);
    EXPECT_EQ(get_last_error(), 0x1234u);
}

TEST_F(LocaleStringTest, GetThreadPreferredUILanguagesSmallBufferFails) {
    auto entry = find_export("GetThreadPreferredUILanguages");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr GuestAddr language_count = 0x4000;
    constexpr GuestAddr buffer_size = 0x4008;
    constexpr GuestAddr buffer = 0x5000;
    (void)memory_.write_u32(buffer_size, 6);
    cpu_.gpr.rcx = 0x38;
    cpu_.gpr.rdx = language_count;
    cpu_.gpr.r8 = buffer;
    cpu_.gpr.r9 = buffer_size;

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(memory_.read_u32(buffer_size).value(), 7u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
}

TEST_F(LocaleStringTest, GetUserPreferredUILanguagesQueriesNameSize) {
    auto entry = find_export("GetUserPreferredUILanguages");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr GuestAddr language_count = 0x4000;
    constexpr GuestAddr buffer_size = 0x4008;
    (void)memory_.write_u32(language_count, 0);
    (void)memory_.write_u32(buffer_size, 0);
    set_last_error(0x1234);
    cpu_.gpr.rcx = 0x8; // MUI_LANGUAGE_NAME
    cpu_.gpr.rdx = language_count;
    cpu_.gpr.r8 = 0;
    cpu_.gpr.r9 = buffer_size;

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(language_count).value(), 1u);
    EXPECT_EQ(memory_.read_u32(buffer_size).value(), 7u);
    EXPECT_EQ(get_last_error(), 0x1234u);
}

TEST_F(LocaleStringTest, GetUserPreferredUILanguagesWritesLanguageIdList) {
    auto entry = find_export("GetUserPreferredUILanguages");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    constexpr GuestAddr language_count = 0x4000;
    constexpr GuestAddr buffer_size = 0x4008;
    constexpr GuestAddr buffer = 0x5000;
    (void)memory_.write_u32(buffer_size, 6);
    cpu_.gpr.rcx = 0x4; // MUI_LANGUAGE_ID
    cpu_.gpr.rdx = language_count;
    cpu_.gpr.r8 = buffer;
    cpu_.gpr.r9 = buffer_size;

    entry.handler(m);

    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(language_count).value(), 1u);
    EXPECT_EQ(memory_.read_u32(buffer_size).value(), 6u);
    EXPECT_EQ(memory_.read_u16(buffer + 0).value(), u'0');
    EXPECT_EQ(memory_.read_u16(buffer + 2).value(), u'4');
    EXPECT_EQ(memory_.read_u16(buffer + 4).value(), u'0');
    EXPECT_EQ(memory_.read_u16(buffer + 6).value(), u'9');
    EXPECT_EQ(memory_.read_u16(buffer + 8).value(), 0u);
    EXPECT_EQ(memory_.read_u16(buffer + 10).value(), 0u);
}

TEST_F(LocaleStringTest, GetCurrencyFormatWWritesDollarPrefix) {
    auto entry = find_export("GetCurrencyFormatW");
    EXPECT_EQ(entry.stdcall_bytes, 24u);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const char16_t value[] = u"12";
    for (usize i = 0; i < 3; ++i) {
        (void)memory_.write_u16(SRC_BUF + static_cast<u32>(i) * 2,
                                static_cast<u16>(value[i]));
    }

    cpu_.gpr.rcx = 0x0409; // LCID en-US
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = SRC_BUF;
    cpu_.gpr.r9 = 0;
    set_stack_arg(4, DST_BUF);
    set_stack_arg(5, 16);

    entry.handler(m);

    EXPECT_GT(cpu_.gpr.rax, 0u);
    EXPECT_EQ(memory_.read_u16(DST_BUF).value_or(0), u'$');
    EXPECT_EQ(memory_.read_u16(DST_BUF + 2).value_or(0), u'1');
    EXPECT_EQ(memory_.read_u16(DST_BUF + 4).value_or(0), u'2');
}

TEST_F(LocaleStringTest, GetCurrencyFormatWSizeQueryIncludesNul) {
    auto entry = find_export("GetCurrencyFormatW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    const char16_t value[] = u"1";
    for (usize i = 0; i < 2; ++i) {
        (void)memory_.write_u16(SRC_BUF + static_cast<u32>(i) * 2,
                                static_cast<u16>(value[i]));
    }

    cpu_.gpr.rcx = 0x0409;
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = SRC_BUF;
    cpu_.gpr.r9 = 0;
    set_stack_arg(4, 0);
    set_stack_arg(5, 0);

    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 3u); // "$1" + NUL
}

TEST_F(LocaleStringTest, EnumSystemLocalesWAlternateSortsMatchesOracle) {
    // Windows enum_locales_alt_probe: LCID_ALTERNATE_SORTS → 14 LCIDs.
    static constexpr u32 kAlt[] = {
        0x00010407, 0x0001040e, 0x00040411, 0x00010437, 0x0001007f,
        0x00050804, 0x00020804, 0x00040c04, 0x00041404, 0x00021404,
        0x00051004, 0x00021004, 0x00030404, 0x00040404,
    };
    std::vector<u32> seen;
    m_mgr->set_guest_call([&](GuestAddr /*cb*/, const u64* args, usize,
                              bool) -> u64 {
        GuestAddr str = static_cast<GuestAddr>(args[0]);
        char hex[9] = {};
        for (int i = 0; i < 8; ++i) {
            hex[i] = static_cast<char>(memory_.read_u16(str + static_cast<u64>(i) * 2)
                                           .value_or(0));
        }
        seen.push_back(static_cast<u32>(std::strtoul(hex, nullptr, 16)));
        return 1;
    });

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    constexpr u32 LCID_ALTERNATE_SORTS = 0x00000004;
    EXPECT_EQ(m_mgr->enum_system_locales(m, /*wide=*/true, /*callback=*/1,
                                         LCID_ALTERNATE_SORTS),
              1u);
    ASSERT_EQ(seen.size(), sizeof(kAlt) / sizeof(kAlt[0]));
    for (usize i = 0; i < seen.size(); ++i) {
        EXPECT_EQ(seen[i], kAlt[i]) << "index " << i;
    }
}

TEST_F(LocaleStringTest, EnumSystemLocalesScratchIsThreadLocal) {
    m_mgr->set_guest_call([](GuestAddr, const u64*, usize, bool) -> u64 {
        return 1;
    });
    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    GuestAddr a = 0;
    GuestAddr b = 0;
    m_mgr->set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
        a = static_cast<GuestAddr>(args[0]);
        return 0; // stop after first
    });
    EXPECT_EQ(m_mgr->enum_system_locales(m, true, 1, 0), 1u);
    ASSERT_NE(a, 0u);

    std::thread t([&] {
        Marshaler m2(memory_);
        m_mgr->set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
            b = static_cast<GuestAddr>(args[0]);
            return 0;
        });
        EXPECT_EQ(m_mgr->enum_system_locales(m2, true, 1, 0), 1u);
    });
    t.join();
    ASSERT_NE(b, 0u);
    EXPECT_NE(a, b);
}

TEST_F(LocaleStringTest, GetAcpDefaultsTo1252AndHonorsUtf8ManifestSetting) {
    // Default process ACP is Windows-1252.
    EXPECT_EQ(m_mgr->get_acp(), 1252u);
    EXPECT_EQ(codepage::ansi_code_page(), 1252u);
    EXPECT_EQ(codepage::resolve_pseudo(codepage::kCpAcp), 1252u);

    // Manifest activeCodePage=UTF-8 → GetACP / CP_ACP = 65001.
    codepage::set_ansi_code_page(codepage::kCpUtf8);
    EXPECT_EQ(m_mgr->get_acp(), 65001u);
    EXPECT_EQ(codepage::resolve_pseudo(codepage::kCpAcp), 65001u);

    codepage::set_ansi_code_page(codepage::kAnsiCodePage);
    EXPECT_EQ(m_mgr->get_acp(), 1252u);
}

TEST_F(LocaleStringTest, GetCPInfoExWAcpCodePageAndNameMatchOracle) {
    // Windows cpinfoex_name_probe: CP_ACP → CodePage=1252,
    // Name="1252  (ANSI - Latin I)".
    auto entry = find_export("GetCPInfoExW");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    codepage::set_ansi_code_page(codepage::kAnsiCodePage);
    constexpr GuestAddr kInfo = DST_BUF;
    cpu_.gpr.rcx = 0; // CP_ACP
    cpu_.gpr.rdx = 0;
    cpu_.gpr.r8 = kInfo;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(kInfo + 20).value_or(0), 1252u);

    std::string name;
    for (usize i = 0; i < 64; ++i) {
        u16 c = memory_.read_u16(kInfo + 24 + i * 2).value_or(0);
        if (c == 0) break;
        name.push_back(static_cast<char>(c));
    }
    EXPECT_EQ(name, "1252  (ANSI - Latin I)");

    // UTF-8 process ACP: CP_ACP reports 65001 and the UTF-8 name.
    codepage::set_ansi_code_page(codepage::kCpUtf8);
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(kInfo + 20).value_or(0), 65001u);
    name.clear();
    for (usize i = 0; i < 64; ++i) {
        u16 c = memory_.read_u16(kInfo + 24 + i * 2).value_or(0);
        if (c == 0) break;
        name.push_back(static_cast<char>(c));
    }
    EXPECT_EQ(name, "65001 (UTF-8)");
    codepage::set_ansi_code_page(codepage::kAnsiCodePage);
}

TEST_F(LocaleStringTest, SetThreadLocaleResolvesDefaultsAndRejectsUnknown) {
    auto get_entry = find_export("GetThreadLocale");
    auto set_entry = find_export("SetThreadLocale");
    Marshaler m(memory_);
    m.attach_cpu(cpu_);

    auto current = [&]() -> u64 {
        get_entry.handler(m);
        return cpu_.gpr.rax;
    };
    auto set_locale = [&](u32 locale) -> u64 {
        cpu_.gpr.rcx = locale;
        set_entry.handler(m);
        return cpu_.gpr.rax;
    };

    EXPECT_EQ(current(), 0x0409u);
    EXPECT_EQ(set_entry.stdcall_bytes, 4);

    set_last_error(0);
    EXPECT_EQ(set_locale(0x0400u), 1u); // LOCALE_USER_DEFAULT -> en-US
    EXPECT_EQ(current(), 0x0409u);

    EXPECT_EQ(set_locale(0x0800u), 1u); // LOCALE_SYSTEM_DEFAULT -> en-US
    EXPECT_EQ(current(), 0x0409u);

    EXPECT_EQ(set_locale(0x0415u), 1u); // pl-PL
    EXPECT_EQ(current(), 0x0415u);

    EXPECT_EQ(set_locale(0x007Fu), 1u); // LOCALE_INVARIANT
    EXPECT_EQ(current(), 0x007Fu);

    set_last_error(0);
    EXPECT_EQ(set_locale(0x0C00u), 0u); // LOCALE_CUSTOM_DEFAULT, none installed
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(current(), 0x007Fu);

    set_last_error(0);
    EXPECT_EQ(set_locale(0x1234u), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(current(), 0x007Fu);
}

TEST_F(LocaleStringTest, ThreadUiLanguageTracksPreferredList) {
    auto get_ui = find_export("GetThreadUILanguage");
    auto set_pref = find_export("SetThreadPreferredUILanguages");
    auto get_pref = find_export("GetThreadPreferredUILanguages");
    EXPECT_EQ(get_ui.stdcall_bytes, 0);
    EXPECT_EQ(set_pref.stdcall_bytes, 12);

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    get_ui.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x0409u);

    constexpr GuestAddr kLang = 0x6000;
    constexpr GuestAddr kCount = 0x6100;
    auto write_list = [&](std::initializer_list<const char*> tags) {
        GuestAddr at = kLang;
        for (const char* tag : tags) {
            for (u32 i = 0; tag[i] != '\0'; ++i) {
                (void)memory_.write_u16(at, static_cast<u16>(tag[i]));
                at += 2;
            }
            (void)memory_.write_u16(at, 0);
            at += 2;
        }
        (void)memory_.write_u16(at, 0);
    };
    write_list({"de-DE"});
    (void)memory_.write_u32(kCount, 0xFFFFFFFFu);
    cpu_.gpr.rcx = 0x8; // MUI_LANGUAGE_NAME
    cpu_.gpr.rdx = kLang;
    cpu_.gpr.r8 = kCount;
    set_pref.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(kCount).value(), 1u);

    get_ui.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x0407u);

    constexpr GuestAddr kNum = 0x6200;
    constexpr GuestAddr kCch = 0x6204;
    constexpr GuestAddr kBuf = 0x6300;
    (void)memory_.write_u32(kCch, 8);
    cpu_.gpr.rcx = 0x4 | 0x40; // MUI_LANGUAGE_ID | MUI_THREAD_LANGUAGES
    cpu_.gpr.rdx = kNum;
    cpu_.gpr.r8 = kBuf;
    cpu_.gpr.r9 = kCch;
    get_pref.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(kNum).value(), 1u);
    EXPECT_EQ(memory_.read_u32(kCch).value(), 6u);
    std::string id;
    for (u32 i = 0; i < 4; ++i) {
        id.push_back(static_cast<char>(memory_.read_u16(kBuf + i * 2).value()));
    }
    EXPECT_EQ(id, "0407");

    set_last_error(0);
    cpu_.gpr.rcx = 0x4 | 0x8;
    cpu_.gpr.rdx = kLang;
    cpu_.gpr.r8 = kCount;
    set_pref.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    get_ui.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x0407u);

    write_list({"0400"});
    set_last_error(0);
    cpu_.gpr.rcx = 0x4; // MUI_LANGUAGE_ID, LOCALE_USER_DEFAULT is not a language
    cpu_.gpr.rdx = kLang;
    cpu_.gpr.r8 = kCount;
    set_pref.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    write_list({});
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = kLang;
    cpu_.gpr.r8 = kCount;
    set_pref.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(kCount).value(), 0u);
    get_ui.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0x0409u);

    write_list({"en-US", "en-GB", "de-DE", "fr-FR", "es-ES", "it-IT"});
    cpu_.gpr.rcx = 0x8;
    cpu_.gpr.rdx = kLang;
    cpu_.gpr.r8 = kCount;
    set_pref.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(kCount).value(), 5u);

    (void)memory_.write_u32(kCch, 64);
    cpu_.gpr.rcx = 0x8 | 0x40;
    cpu_.gpr.rdx = kNum;
    cpu_.gpr.r8 = kBuf;
    cpu_.gpr.r9 = kCch;
    get_pref.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 1u);
    EXPECT_EQ(memory_.read_u32(kNum).value(), 5u);
    // Five 5-character names, a NUL after each, and the list's extra NUL.
    EXPECT_EQ(memory_.read_u32(kCch).value(), 31u);
    std::string packed;
    for (u32 i = 0; i < 31; ++i) {
        const u16 unit = memory_.read_u16(kBuf + i * 2).value_or(0xFFFF);
        packed.push_back(unit == 0 ? '|' : static_cast<char>(unit));
    }
    EXPECT_EQ(packed, "en-US|en-GB|de-DE|fr-FR|es-ES||");
}

TEST_F(LocaleStringTest, EnumCalendarInfoWEnumeratesGregorianEnUs) {
    auto entry = find_export("EnumCalendarInfoW");
    EXPECT_EQ(entry.stdcall_bytes, 16);

    std::vector<std::string> seen;
    m_mgr->set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
        std::string text;
        const GuestAddr str = static_cast<GuestAddr>(args[0]);
        for (u32 i = 0; i < 64; ++i) {
            const u16 unit = memory_.read_u16(str + static_cast<u64>(i) * 2).value_or(0);
            if (unit == 0) break;
            text.push_back(static_cast<char>(unit));
        }
        seen.push_back(text);
        return 1;
    });

    Marshaler m(memory_);
    m.attach_cpu(cpu_);
    set_last_error(0);
    cpu_.gpr.rcx = 0;
    cpu_.gpr.rdx = 0x0409;
    cpu_.gpr.r8 = 1;
    cpu_.gpr.r9 = 0x15;
    entry.handler(m);
    EXPECT_EQ(cpu_.gpr.rax, 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);

    auto call = [&](u32 locale, u32 calendar, u32 cal_type) {
        seen.clear();
        cpu_.gpr.rcx = 1;
        cpu_.gpr.rdx = locale;
        cpu_.gpr.r8 = calendar;
        cpu_.gpr.r9 = cal_type;
        entry.handler(m);
        return cpu_.gpr.rax;
    };

    EXPECT_EQ(call(0x0409, 1, 0x15), 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "January");

    EXPECT_EQ(call(0x0400, 0xFFFFFFFFu, 0x01), 1u);
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0], "1");

    EXPECT_EQ(call(0x0409, 1, 0x05), 1u);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], "M/d/yyyy");
    EXPECT_EQ(seen[1], "M/d/yy");

    m_mgr->set_guest_call([&](GuestAddr, const u64* args, usize, bool) -> u64 {
        seen.push_back("stop");
        (void)args;
        return 0;
    });
    seen.clear();
    EXPECT_EQ(call(0x0409, 1, 0x05), 1u);
    EXPECT_EQ(seen.size(), 1u);

    set_last_error(0);
    EXPECT_EQ(call(0x0409, 1, 0x21), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_TRUE(seen.empty());

    set_last_error(0);
    EXPECT_EQ(call(0x0409, 2, 0x02), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// Expected values: tools/oracle_probe/enum_ui_languages_probe{,.x86}.expected
// (winoracle, en-US system; x86 and x64 identical apart from lParam width).
TEST_F(LocaleStringTest, EnumUILanguagesMatchesOracleBothBitnesses) {
    constexpr u32 kSentinel = 0xDEADBEEFu;
    for (const bool mode64 : {false, true}) {
        SCOPED_TRACE(mode64 ? "x64" : "x86");
        cpu::CpuState cpu;
        if (mode64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        Marshaler m(memory_);
        m.attach_cpu(cpu);
        const u64 lparam = mode64 ? 0x1122334455667788ull : 0x55667788ull;

        bool wide = true;
        int stop_after = 0;
        std::vector<std::string> seen;
        std::vector<u64> lparams;
        m_mgr->set_guest_call([&](GuestAddr cb, const u64* args, usize count,
                                  bool is64) -> u64 {
            EXPECT_EQ(cb, 0x4000u);
            EXPECT_EQ(count, 2u);
            EXPECT_EQ(is64, mode64);
            std::string text;
            const GuestAddr str = static_cast<GuestAddr>(args[0]);
            for (u32 i = 0; i < 64; ++i) {
                const u32 unit = wide
                    ? memory_.read_u16(str + static_cast<u64>(i) * 2).value_or(0)
                    : memory_.read_u8(str + i).value_or(0);
                if (unit == 0) break;
                text.push_back(static_cast<char>(unit));
            }
            seen.push_back(text);
            lparams.push_back(args[1]);
            // Upper RAX bits are not part of a BOOL return.
            if (stop_after != 0 && static_cast<int>(seen.size()) >= stop_after) {
                return 0xFFFFFFFF00000000ull;
            }
            return 1;
        });

        auto call = [&](const char* name, u64 proc, u32 flags) {
            const ExportEntry entry = find_export(name);
            EXPECT_EQ(entry.stdcall_bytes, 12);
            wide = name[std::strlen(name) - 1] == 'W';
            seen.clear();
            lparams.clear();
            cpu.gpr.rcx = proc;
            cpu.gpr.rdx = flags;
            cpu.gpr.r8 = lparam;
            const u64 stack[] = {proc, flags, lparam};
            for (usize i = 0; i < 3; ++i) {
                EXPECT_TRUE(memory_.write_u32(cpu.gpr.rsp + 4 + i * 4,
                                              static_cast<u32>(stack[i])).is_ok());
            }
            set_last_error(kSentinel);
            entry.handler(m);
            return static_cast<u32>(cpu.gpr.rax);
        };

        for (const char* name : {"EnumUILanguagesA", "EnumUILanguagesW"}) {
            SCOPED_TRACE(name);
            // Default and MUI_LANGUAGE_ID: "0409"; MUI_LANGUAGE_NAME: "en-US".
            for (const u32 flags : {0x0u, 0x4u, 0x14u, 0x84u, 0x90u}) {
                EXPECT_EQ(call(name, 0x4000, flags), 1u) << flags;
                EXPECT_EQ(get_last_error(), kSentinel);
                ASSERT_EQ(seen.size(), 1u);
                EXPECT_EQ(seen[0], "0409");
                EXPECT_EQ(lparams[0], lparam);
            }
            for (const u32 flags : {0x8u, 0x28u, 0x48u, 0x88u}) {
                EXPECT_EQ(call(name, 0x4000, flags), 1u) << flags;
                EXPECT_EQ(get_last_error(), kSentinel);
                ASSERT_EQ(seen.size(), 1u);
                EXPECT_EQ(seen[0], "en-US");
            }
            // A FALSE from the callback stops the walk; the call is TRUE.
            stop_after = 1;
            EXPECT_EQ(call(name, 0x4000, 0x8), 1u);
            EXPECT_EQ(seen.size(), 1u);
            EXPECT_EQ(get_last_error(), kSentinel);
            stop_after = 0;
            // NULL proc is checked before the flags.
            for (const u32 flags : {0x0u, 0x8u, 0xCu, 0x80000000u}) {
                EXPECT_EQ(call(name, 0, flags), 0u);
                EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
            }
            for (const u32 flags : {0x1u, 0x2u, 0xCu, 0x100u, 0x80000000u,
                                    0x30u, 0x50u, 0x60u, 0x70u, 0xF0u, 0x9u}) {
                EXPECT_EQ(call(name, 0x4000, flags), 0u) << flags;
                EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
                EXPECT_TRUE(seen.empty());
            }
        }
    }
}

// =============================================================================
// GetNumberFormatW / GetNumberFormatA
// =============================================================================
//
// Learn GetNumberFormatA / NUMBERFMTA, and where Learn leaves a value open the
// winoracle measurement (tools/oracle_probe/getnumberformat_probe64.expected,
// gnfw-* / gnfa-* tags; the i686 build is identical). Both bitnesses: the
// arguments move between the stack and registers, and NUMBERFMT's pointers
// sit at +12/+16 (NegativeOrder +20) on x86 and +16/+24 (+32) on x64.

namespace {

class GetNumberFormatTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr MEM = 0x10000;
    static constexpr GuestAddr STACK = 0x80000;
    static constexpr GuestAddr VALUE = MEM + 0x100;
    static constexpr GuestAddr FMT = MEM + 0x400;
    static constexpr GuestAddr DEC = MEM + 0x500;
    static constexpr GuestAddr THOU = MEM + 0x540;
    static constexpr GuestAddr NAME = MEM + 0x600;
    static constexpr GuestAddr OUT = MEM + 0x800;
    static constexpr u32 kSeed = 0xDEAD;

    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MEM, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK, 0x2000, MemoryProtection::ReadWrite).is_ok());
        if (GetParam()) cpu_.init_long_mode(); else cpu_.init_protected_mode();
        DllExports exports = create_locale_string_thunks(mgr_);
        for (auto& e : exports.entries) {
            if (e.name == "GetNumberFormatW") w_ = e;
            if (e.name == "GetNumberFormatA") a_ = e;
            if (e.name == "GetNumberFormatEx") ex_ = e;
        }
        ASSERT_TRUE(static_cast<bool>(w_.handler));
        ASSERT_TRUE(static_cast<bool>(a_.handler));
        EXPECT_EQ(w_.stdcall_bytes, 24u);
        EXPECT_EQ(a_.stdcall_bytes, 24u);
        ASSERT_TRUE(static_cast<bool>(ex_.handler));
        EXPECT_EQ(ex_.stdcall_bytes, 24u);   // winnls.h: six parameters, 4 bytes each on x86
    }

    void put_w(GuestAddr at, std::u16string_view s) {
        for (usize i = 0; i < s.size(); ++i) (void)memory_.write_u16(at + i * 2, s[i]);
        (void)memory_.write_u16(at + s.size() * 2, 0);
    }
    void put_a(GuestAddr at, std::string_view s) {
        (void)memory_.write_bytes(at, reinterpret_cast<const Byte*>(s.data()), s.size());
        (void)memory_.write_u8(at + s.size(), 0);
    }
    /// NUMBERFMT{W,A} at FMT with the separators at DEC / THOU (false: NULL).
    void put_format(u32 digits, u32 lzero, u32 grouping, bool dec, bool thou, u32 neg) {
        (void)memory_.write_u32(FMT, digits);
        (void)memory_.write_u32(FMT + 4, lzero);
        (void)memory_.write_u32(FMT + 8, grouping);
        if (GetParam()) {
            (void)memory_.write_u64(FMT + 16, dec ? DEC : 0);
            (void)memory_.write_u64(FMT + 24, thou ? THOU : 0);
            (void)memory_.write_u32(FMT + 32, neg);
        } else {
            (void)memory_.write_u32(FMT + 12, dec ? DEC : 0);
            (void)memory_.write_u32(FMT + 16, thou ? THOU : 0);
            (void)memory_.write_u32(FMT + 20, neg);
        }
    }

    /// Calls @p e with a stdcall frame or x64 registers (garbage upper halves).
    u32 call(ExportEntry& e, u32 lcid, u32 flags, GuestAddr value, GuestAddr fmt, GuestAddr out, i32 cch) {
        return call_first(e, lcid, true, flags, value, fmt, out, cch);
    }
    /// GetNumberFormatEx: the first argument is lpLocaleName, a full pointer.
    u32 call_ex(GuestAddr name, u32 flags, GuestAddr value, GuestAddr fmt, GuestAddr out, i32 cch) {
        return call_first(ex_, name, false, flags, value, fmt, out, cch);
    }
    u32 call_first(ExportEntry& e, u64 first, bool first_is_u32, u32 flags, GuestAddr value, GuestAddr fmt,
                   GuestAddr out, i32 cch) {
        const u64 args[6] = {first, flags, value, fmt, out, static_cast<u32>(cch)};
        const GuestAddr frame = STACK + 0x1000;
        cpu_.set_sp(frame);
        if (GetParam()) {
            cpu_.gpr.rcx = (first_is_u32 ? 0xABCD000000000000ull : 0) | args[0];
            cpu_.gpr.rdx = 0xABCD000000000000ull | args[1];
            cpu_.gpr.r8 = args[2];
            cpu_.gpr.r9 = args[3];
            (void)memory_.write_u64(frame + 0x28, args[4]);
            (void)memory_.write_u64(frame + 0x30, 0xABCD000000000000ull | args[5]);
        } else {
            for (usize i = 0; i < 6; ++i) (void)memory_.write_u32(frame + 4 + i * 4, static_cast<u32>(args[i]));
        }
        set_last_error(kSeed);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        e.handler(m);
        return static_cast<u32>(cpu_.gpr.rax);
    }
    u32 w(u32 lcid, u32 flags, std::u16string_view value, bool fmt, i32 cch, bool null_out = false) {
        put_w(VALUE, value);
        return call(w_, lcid, flags, VALUE, fmt ? FMT : 0, null_out ? 0 : OUT, cch);
    }
    u32 a(u32 lcid, u32 flags, std::string_view value, bool fmt, i32 cch, bool null_out = false) {
        put_a(VALUE, value);
        return call(a_, lcid, flags, VALUE, fmt ? FMT : 0, null_out ? 0 : OUT, cch);
    }
    /// GetNumberFormatEx with @p name at NAME (nullptr: LOCALE_NAME_USER_DEFAULT, NULL).
    u32 ex(const char16_t* name, u32 flags, std::u16string_view value, bool fmt, i32 cch, bool null_out = false) {
        if (name != nullptr) put_w(NAME, name);
        put_w(VALUE, value);
        return call_ex(name != nullptr ? NAME : 0, flags, VALUE, fmt ? FMT : 0, null_out ? 0 : OUT, cch);
    }
    std::u16string out_w(usize n) {
        std::u16string s;
        for (usize i = 0; i < n; ++i) s.push_back(static_cast<char16_t>(memory_.read_u16(OUT + i * 2).value_or(0)));
        return s;
    }
    std::string out_a(usize n) {
        std::string s;
        for (usize i = 0; i < n; ++i) s.push_back(static_cast<char>(memory_.read_u8(OUT + i).value_or(0)));
        return s;
    }
    void fill_out() {
        for (u32 i = 0; i < 256; ++i) (void)memory_.write_u8(OUT + i, '#');
    }
    static std::u16string z(const char16_t* s) { return std::u16string(s) + u'\0'; }

    LocaleStringManager mgr_;
    GuestMemory memory_;
    cpu::CpuState cpu_;
    ExportEntry w_, a_, ex_;
};

TEST_P(GetNumberFormatTest, LocaleDefaultsAndValueGrammar) {
    struct Case {
        const char16_t* value;
        const char16_t* out;   // nullptr: ERROR_INVALID_PARAMETER
    };
    const Case cases[] = {   // gnfw-us-null-fmt value=...
        {u"1234567.891", u"1,234,567.89"}, {u"-1234.5", u"-1,234.50"}, {u"0.5", u"0.50"}, {u".5", u"0.50"},
        {u"-.5", u"-0.50"}, {u"1234", u"1,234.00"}, {u"-0", u"0.00"}, {u"00012.30", u"12.30"},
        {u"999.995", u"1,000.00"}, {u"999.994", u"999.99"}, {u"0.005", u"0.01"}, {u"-0.004", u"0.00"},
        {u"-0.005", u"-0.01"}, {u"1.", u"1.00"}, {u"-", u"0.00"}, {u"", u"0.00"}, {u".", u"0.00"}, {u"-.", u"0.00"},
        {u"-9.995", u"-10.00"}, {u"0000", u"0.00"}, {u"000.000", u"0.00"},
        {u"12345678901234567890123456789012345678901234567890.25",
         u"12,345,678,901,234,567,890,123,456,789,012,345,678,901,234,567,890.25"},
        {u"abc", nullptr}, {u"1.2.3", nullptr}, {u"+5", nullptr}, {u" 5", nullptr}, {u"5 ", nullptr},
        {u"5-", nullptr}, {u"1e5", nullptr}, {u"--5", nullptr}, {u"0x10", nullptr}, {u"1,234", nullptr},
    };
    for (const Case& c : cases) {
        fill_out();
        const u32 r = w(0x0409, 0, c.value, false, 128);
        if (c.out == nullptr) {
            EXPECT_EQ(r, 0u);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
            EXPECT_EQ(out_a(2), "##");   // nothing written
            continue;
        }
        const std::u16string want = z(c.out);
        EXPECT_EQ(r, want.size());
        EXPECT_EQ(out_w(want.size()), want);
        EXPECT_EQ(get_last_error(), kSeed);   // success leaves the last error alone
    }
    // gnfw-locale-*: each locale's defaults.
    EXPECT_EQ(w(0x0400, 0, u"1234.5", false, 128), 9u);    // LOCALE_USER_DEFAULT
    EXPECT_EQ(w(0x0800, 0, u"1234.5", false, 128), 9u);    // LOCALE_SYSTEM_DEFAULT
    EXPECT_EQ(w(0, 0, u"1234.5", false, 128), 9u);         // LOCALE_NEUTRAL
    EXPECT_EQ(w(0x007F, 0, u"-1234.5", false, 128), 10u);  // LOCALE_INVARIANT
    EXPECT_EQ(out_w(10), z(u"-1,234.50"));
    EXPECT_EQ(w(0x0407, 0, u"-1234567.891", false, 128), 14u);   // de-DE
    EXPECT_EQ(out_w(14), z(u"-1.234.567,89"));
    EXPECT_EQ(w(0x040C, 0, u"-1234567.891", false, 128), 14u);   // gnfw-locale-fr-FR
    EXPECT_EQ(out_w(14), z(u"-1\u202F234\u202F567,89"));
    EXPECT_EQ(w(0x1234, 0, u"1234.5", false, 128), 0u);          // gnfw-locale-bad-0x1234
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_P(GetNumberFormatTest, NumberFmtMembers) {
    put_w(DEC, u".");
    put_w(THOU, u",");
    struct Case {
        u32 nd, lz, gr, no;
        const char16_t* value;
        const char16_t* out;   // nullptr: ERROR_INVALID_PARAMETER
    };
    const Case cases[] = {   // gnfw-fmt-*
        {0, 1, 3, 1, u"-1234567.891", u"-1,234,568"}, {0, 1, 3, 1, u"0.5", u"1"}, {0, 1, 3, 1, u"-.004", u"0"},
        {3, 1, 3, 1, u"-.004", u"-0.004"}, {9, 1, 3, 1, u"0.5", u"0.500000000"}, {10, 1, 3, 1, u"0.5", nullptr},
        {2, 0, 3, 1, u"-0.5", u"-.50"}, {2, 0, 3, 1, u"-.004", u".00"}, {2, 2, 3, 1, u"0.5", nullptr},
        {2, 1, 0, 1, u"-1234567.891", u"-1234567.89"}, {2, 1, 32, 1, u"123456789012", u"1,23,45,67,89,012.00"},
        {2, 1, 32, 1, u"-1234567.891", u"-12,34,567.89"},
        {2, 1, 320, 1, u"123456789012", u"1234567,89,012.00"},
        {2, 1, 321, 1, u"123456789012", u"1,2,3,4,5,6,7,89,012.00"},
        {2, 1, 30, 1, u"123456789012", u"123456789,012.00"}, {2, 1, 2, 1, u"-1234567.891", u"-1,23,45,67.89"},
        {2, 1, 9, 1, u"123456789012", u"123,456789012.00"}, {2, 1, 10, 1, u"123456789012", u"12345678901,2.00"},
        {2, 1, 33, 1, u"123456789012", u"123,456,789,012.00"},
        {2, 1, 123, 1, u"123456789012", u"123,456,789,01,2.00"},
        {2, 1, 1230, 1, u"123456789012", u"123456,789,01,2.00"},
        {2, 1, 31, 1, u"-1234567.891", u"-1,2,3,4,567.89"}, {2, 1, 99, 1, u"123456789012", u"123,456789012.00"},
        {2, 1, 3, 0, u"-1234567.891", u"(1,234,567.89)"}, {2, 1, 3, 2, u"-0.5", u"- 0.50"},
        {2, 1, 3, 3, u"-0.5", u"0.50-"}, {2, 1, 3, 4, u"-1234567.891", u"1,234,567.89 -"},
        {2, 1, 3, 0, u"0.5", u"0.50"}, {2, 1, 3, 5, u"-0.5", nullptr},
    };
    for (const Case& c : cases) {
        put_format(c.nd, c.lz, c.gr, true, true, c.no);
        const u32 r = w(0x0409, 0, c.value, true, 128);
        if (c.out == nullptr) {
            EXPECT_EQ(r, 0u) << c.nd << " " << c.lz << " " << c.no;
            EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
            continue;
        }
        const std::u16string want = z(c.out);
        EXPECT_EQ(r, want.size()) << c.gr;
        EXPECT_EQ(out_w(want.size()), want) << c.gr;
    }
    // Separators: decimal 1-3 characters, thousand 0-3, whatever NumDigits and
    // Grouping are (gnfw-fmt-decimal-len*, -thousand-len*, -nd0-decimal-len4,
    // -gr0-thousand-len4, -nd0-decimal-unused-len4).
    struct Sep {
        const char16_t* dec;
        const char16_t* thou;
        u32 nd, gr;
        const char16_t* value;
        const char16_t* out;
    };
    const Sep seps[] = {
        {u"", u",", 2, 3, u"-1234.5", nullptr}, {u".", u"", 2, 3, u"-1234.5", u"-1234.50"},
        {u"a", u",", 2, 3, u"-1234.5", u"-1,234a50"}, {u".", u"a", 2, 3, u"-1234.5", u"-1a234.50"},
        {u"abc", u",", 2, 3, u"-1234.5", u"-1,234abc50"}, {u".", u"abc", 2, 3, u"-1234.5", u"-1abc234.50"},
        {u"abcd", u",", 2, 3, u"-1234.5", nullptr}, {u".", u"abcd", 2, 3, u"-1234.5", nullptr},
        {u"abcd", u",", 0, 3, u"1234.5", nullptr}, {u".", u"abcd", 2, 0, u"1234.5", nullptr},
        {u".", u",", 0, 3, u"1234.5", u"1,235"},
    };
    for (const Sep& s : seps) {
        put_w(DEC, s.dec);
        put_w(THOU, s.thou);
        put_format(s.nd, 1, s.gr, true, true, 1);
        const u32 r = w(0x0409, 0, s.value, true, 128);
        if (s.out == nullptr) {
            EXPECT_EQ(r, 0u);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
            continue;
        }
        const std::u16string want = z(s.out);
        EXPECT_EQ(r, want.size());
        EXPECT_EQ(out_w(want.size()), want);
    }
    // NULL separators (gnfw-fmt-null-decimal, -null-thousand).
    put_w(DEC, u".");
    put_w(THOU, u",");
    put_format(2, 1, 3, false, true, 1);
    EXPECT_EQ(w(0x0409, 0, u"1234.5", true, 128), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    put_format(2, 1, 3, true, false, 1);
    EXPECT_EQ(w(0x0409, 0, u"1234.5", true, 128), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // gnfw-fmt-bad-value, -bad-locale.
    put_format(2, 1, 3, true, true, 1);
    EXPECT_EQ(w(0x0409, 0, u"12a", true, 128), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(w(0x1234, 0, u"1234.5", true, 128), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_P(GetNumberFormatTest, FlagsSizeQueryAndBuffers) {
    put_w(DEC, u".");
    put_w(THOU, u",");
    put_format(2, 1, 3, true, true, 1);
    // Flags (Learn dwFlags): none with lpFormat; without it only
    // LOCALE_NOUSEROVERRIDE (and LOCALE_USE_CP_ACP); checked first.
    EXPECT_EQ(w(0x0409, 0x80000000u, u"1234.5", true, 128), 0u);   // gnfw-fmt-flags-nouseroverride
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(w(0x0409, 1, u"1234.5", true, 128), 0u);             // gnfw-fmt-flags-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(w(0x0409, 0x40000000u, u"1234.5", true, 128), 9u);   // gnfex-crosscheck-w-fmt-flags-use-cp-acp
    EXPECT_EQ(get_last_error(), kSeed);
    EXPECT_EQ(w(0x0409, 1, u"1234.5", false, 128), 0u);            // gnfw-flags-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(w(0x0409, 1, u"x", false, 128), 0u);                 // gnfw-flags-1-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(w(0x0409, 0x80000000u, u"1234.5", false, 128), 9u);  // gnfw-flags-nouseroverride
    EXPECT_EQ(w(0x0409, 0x40000000u, u"1234.5", false, 128), 9u);  // gnfw-flags-use-cp-acp
    EXPECT_EQ(w(0x0409, 0x80000000u, u"x", false, 128), 0u);       // -flags-0x80000000-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // The size query (cchNumber 0) counts the NUL and does not use lpNumberStr.
    fill_out();
    EXPECT_EQ(w(0x0409, 0, u"-1234567.891", false, 0), 14u);       // gnfw-size-query
    EXPECT_EQ(out_a(2), "##");
    EXPECT_EQ(w(0x0409, 0, u"-1234567.891", false, 0, true), 14u); // -size-query-null-buffer
    EXPECT_EQ(w(0x0409, 0, u"x", false, 0, true), 0u);             // -size-query-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // Buffers.
    EXPECT_EQ(w(0x0409, 0, u"-1234567.891", false, 14), 14u);      // -exact-buffer-14
    EXPECT_EQ(out_w(14), z(u"-1,234,567.89"));
    fill_out();
    EXPECT_EQ(w(0x0409, 0, u"-1234567.891", false, 13), 0u);       // -short-buffer-13: nothing written
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(out_a(2), "##");
    EXPECT_EQ(w(0x0409, 0, u"-1234567.891", false, 1), 0u);        // -short-buffer-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(w(0x0409, 0, u"1", false, 5, true), 0u);             // -null-buffer-cch5
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(w(0x0409, 0, u"1", false, -1), 0u);                  // -negative-cch
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(w_, 0x0409, 0, 0, 0, OUT, 128), 0u);            // -null-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(w_, 0x0409, 0, 0, 0, 0, 0), 0u);                // -null-value-size-query
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(w(0x0409, 0, u"x", false, 1), 0u);                   // -bad-value-short-buffer
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(w(0x0409, 1, u"1", false, 1), 0u);                   // -bad-flags-short-buffer
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
}

TEST_P(GetNumberFormatTest, GetNumberFormatAMatchesWideFormThroughAcp) {
    // Locale defaults (gnfa-us-null-fmt; gnfa-a-locale-fr-FR: the space -> 0xA0).
    EXPECT_EQ(a(0x0409, 0, "1234567.891", false, 128), 13u);
    EXPECT_EQ(out_a(13), std::string("1,234,567.89") + '\0');
    EXPECT_EQ(a(0x0409, 0, "-1234.5", false, 128), 10u);
    EXPECT_EQ(out_a(10), std::string("-1,234.50") + '\0');
    EXPECT_EQ(a(0x0409, 0, "", false, 128), 5u);
    EXPECT_EQ(out_a(5), std::string("0.00") + '\0');
    EXPECT_EQ(a(0x040C, 0, "-1234567.891", false, 128), 14u);
    EXPECT_EQ(out_a(14), std::string("-1\xA0" "234\xA0" "567,89") + '\0');
    EXPECT_EQ(a(0x040C, 0, "-1234567.891", false, 0, true), 14u);   // the size counts bytes
    EXPECT_EQ(a(0x0409, 0, "1x", false, 128), 0u);                   // gnfa-a-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(a(0x0409, 0, "1\xB2", false, 128), 0u);                // -a-value-high-byte (U+00B2)
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // NUMBERFMTA with CP_ACP separators (gnfa-a-fmt, -a-fmt-cp1252-separators).
    put_a(DEC, ".");
    put_a(THOU, ",");
    put_format(2, 1, 3, true, true, 1);
    EXPECT_EQ(a(0x0409, 0, "-1234567.891", true, 128), 14u);
    EXPECT_EQ(out_a(14), std::string("-1,234,567.89") + '\0');
    put_a(DEC, "\x80");
    put_a(THOU, "\xA0");
    EXPECT_EQ(a(0x0409, 0, "-1234567.891", true, 128), 14u);
    EXPECT_EQ(out_a(14), std::string("-1\xA0" "234\xA0" "567\x80" "89") + '\0');
    put_a(DEC, "<dec>");
    EXPECT_EQ(a(0x0409, 0, "1.5", true, 128), 0u);                   // -a-fmt-long-decimal
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(a(0x0409, 0, "-1234567.891", true, 0, true), 0u);      // -a-fmt-size-query (bad format)
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    put_a(DEC, ".");
    put_a(THOU, ",");
    EXPECT_EQ(a(0x0409, 0x80000000u, "1234.5", true, 128), 0u);      // -a-fmt-flags-nouseroverride
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(a(0x0409, 0x40000000u, "1234.5", true, 128), 9u);      // -a-fmt-flags-use-cp-acp
    EXPECT_EQ(out_a(9), std::string("1,234.50") + '\0');
    EXPECT_EQ(get_last_error(), kSeed);
    EXPECT_EQ(a(0x0409, 0xC0000000u, "1234.5", true, 128), 0u);      // -a-fmt-flags-use-cp-acp-nouseroverride
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    put_format(2, 1, 3, false, true, 1);
    EXPECT_EQ(a(0x0409, 0, "1234.5", true, 128), 0u);                // -a-fmt-null-decimal
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // Sizes and buffers: a short buffer gets the first cchNumber bytes and no
    // NUL (gnfa-a-short-buffer-13).
    EXPECT_EQ(a(0x0409, 0, "-1234567.891", false, 0, true), 14u);    // -a-size-query
    EXPECT_EQ(a(0x0409, 0, "-1234567.891", false, 14), 14u);         // -a-exact-buffer-14
    fill_out();
    EXPECT_EQ(a(0x0409, 0, "-1234567.891", false, 13), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(out_a(14), std::string("-1,234,567.89") + '#');
    EXPECT_EQ(a(0x0409, 0, "1", false, 5, true), 0u);                // -a-null-buffer-cch5
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(a(0x0409, 0, "1", false, -1), 0u);                     // -a-negative-cch
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(a_, 0x0409, 0, 0, 0, OUT, 128), 0u);              // -a-null-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(a(0x0409, 1, "1", false, 128), 0u);                    // -a-flags-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(a(0x0409, 0x40000000u, "1234.5", false, 128), 9u);     // -a-flags-use-cp-acp
    EXPECT_EQ(a(0x0409, 0x80000000u, "1234.5", false, 128), 9u);     // -a-flags-nouseroverride
    EXPECT_EQ(a(0x1234, 0, "1", false, 128), 0u);                    // -a-locale-bad
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// A value is read to its NUL whatever its length (Learn: lpValue is a
// null-terminated string): 70,000 digits format into 70,000 digits with a
// thousand separator every three (en-US) and two decimals.
TEST_P(GetNumberFormatTest, LongValuesAreFormattedWhole) {
    constexpr usize kDigits = 70000;
    constexpr GuestAddr kBig = 0x100000;
    ASSERT_TRUE(memory_.map_region(kBig, 0x80000, MemoryProtection::ReadWrite).is_ok());
    const std::vector<u8> ones(kDigits, '1');
    (void)memory_.write_bytes(kBig, ones.data(), ones.size());
    (void)memory_.write_u8(kBig + kDigits, 0);
    const usize want = kDigits + (kDigits - 1) / 3 + 3 + 1;   // digits, commas, ".00", NUL
    EXPECT_EQ(call(a_, 0x0409, 0, kBig, 0, 0, 0), want);
    // Unreadable before a NUL: ERROR_INVALID_PARAMETER, never a silent prefix.
    const std::vector<u8> tail(0x1000, '1');
    (void)memory_.write_bytes(kBig + 0x80000 - 0x1000, tail.data(), tail.size());
    EXPECT_EQ(call(a_, 0x0409, 0, kBig + 0x80000 - 0x1000, 0, 0, 0), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// -----------------------------------------------------------------------------
// GetNumberFormatEx (Learn GetNumberFormatEx / NUMBERFMTW / LOCALE_NAME_*;
// gnfex-* lines of getnumberformat_probe{32,64}.expected, identical for both
// bitnesses). lpLocaleName is a full pointer in RCX on x64.
// -----------------------------------------------------------------------------

TEST_P(GetNumberFormatTest, ExLocaleNames) {
    struct Case {
        const char16_t* name;   // nullptr: LOCALE_NAME_USER_DEFAULT (NULL)
        const char16_t* out;    // nullptr: ERROR_INVALID_PARAMETER
    };
    const Case cases[] = {   // gnfex-name-*, gnfex-grammar-* with value -1234567.891
        {nullptr, u"-1,234,567.89"}, {u"", u"-1,234,567.89"}, {u"!x-sys-default-locale", u"-1,234,567.89"},
        {u"!X-SYS-DEFAULT-LOCALE", u"-1,234,567.89"}, {u"en-US", u"-1,234,567.89"}, {u"en-GB", u"-1,234,567.89"},
        {u"de-DE", u"-1.234.567,89"}, {u"fr-FR", u"-1 234 567,89"}, {u"es-ES", u"-1.234.567,89"},
        {u"it-IT", u"-1.234.567,89"}, {u"pt-BR", u"-1.234.567,89"}, {u"nl-NL", u"-1.234.567,89"},
        {u"ja-JP", u"-1,234,567.89"}, {u"zh-CN", u"-1,234,567.89"}, {u"ko-KR", u"-1,234,567.89"},
        {u"ru-RU", u"-1 234 567,89"}, {u"zh-TW", u"-1,234,567.89"}, {u"pl-PL", u"-1 234 567,89"},
        {u"sv-SE", u"-1 234 567,89"}, {u"tr-TR", u"-1.234.567,89"}, {u"uk-UA", u"-1 234 567,89"},
        {u"cs-CZ", u"-1 234 567,89"}, {u"EN-us", u"-1,234,567.89"}, {u"en-us", u"-1,234,567.89"},
        {u"en_US", u"-1,234,567.89"}, {u"de_DE", u"-1.234.567,89"}, {u"DE-de", u"-1.234.567,89"},
        {u"de-DE_phoneb", u"-1.234.567,89"}, {u"de-DE_PHONEB", u"-1.234.567,89"},
        {u"es-ES_tradnl", u"-1.234.567,89"}, {u"ja-JP_radstr", u"-1,234,567.89"},
        // Measured aliases of a profile (gnfex-name-en, -de, gnfex-grammar-zh-Hans-CN, gnfex-fix1-name-EN, -De,
        // -zh-hans-cn, -zh_Hans_CN).
        {u"en", u"-1,234,567.89"}, {u"EN", u"-1,234,567.89"}, {u"de", u"-1.234.567,89"}, {u"De", u"-1.234.567,89"},
        {u"zh-Hans-CN", u"-1,234,567.89"}, {u"zh-hans-cn", u"-1,234,567.89"}, {u"zh_Hans_CN", u"-1,234,567.89"},
        // '_' and '-' are one separator everywhere (gnfex-fix1-name-sys-underscores, -sys-one-underscore,
        // -de-DE-phoneb, -es_ES_tradnl, -de_DE_phoneb, -es-ES-tradnl, -de_DE-phoneb).
        {u"!x_sys_default_locale", u"-1,234,567.89"}, {u"!x-sys_default-locale", u"-1,234,567.89"},
        {u"de-DE-phoneb", u"-1.234.567,89"}, {u"es_ES_tradnl", u"-1.234.567,89"}, {u"de_DE_phoneb", u"-1.234.567,89"},
        {u"es-ES-tradnl", u"-1.234.567,89"}, {u"de_DE-phoneb", u"-1.234.567,89"},
        // Malformed names.
        {u"not a locale", nullptr}, {u"en-US ", nullptr}, {u"e", nullptr}, {u"en-", nullptr}, {u"-en", nullptr},
        {u"en--US", nullptr}, {u"abcdefgh", nullptr}, {u"abcdefghi", nullptr}, {u"1234", nullptr},
        {u"en.US", nullptr}, {u"xx-YYY", nullptr}, {u"en-US-", nullptr}, {u"én-US", nullptr},
        {u"de-DE_phonebook", nullptr}, {u"!x-sys-default-locale-x", nullptr},
        {u"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdef", nullptr},
        {u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee-ffffffff-gggggggg-hhhhhhhh-iiiiiiii-j", nullptr},
        {u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee-ffffffff-gggggggg-hhhhhhhh-iiiiiiii-jj", nullptr},
        {u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee-ffffffff-gggggggg-hhhhhhhh-iiiiiiii-jjj", nullptr},
    };
    for (const Case& c : cases) {
        fill_out();
        const std::u16string label = c.name ? std::u16string(c.name) : u"<null>";
        const std::string tag(label.begin(), label.end());
        const u32 r = ex(c.name, 0, u"-1234567.891", false, 128);
        if (c.out == nullptr) {
            EXPECT_EQ(r, 0u) << tag;
            EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER) << tag;
            EXPECT_EQ(out_a(2), "##") << tag;
            continue;
        }
        const std::u16string want = z(c.out);
        EXPECT_EQ(r, want.size()) << tag;
        EXPECT_EQ(out_w(want.size()), want) << tag;
        EXPECT_EQ(get_last_error(), kSeed) << tag;
    }
}

// Known divergence, pinned so that a fix or a regression shows up. Windows
// answers a well-formed name it has no data for (gnfex-name-xx-YY: ret=14
// "-1,234,567.89") and real locales outside Vitreo's profile table
// (gnfex-name-en-IN: ret=14 "-12,34,567.89"). Vitreo has no data for either and
// fails them with ERROR_INVALID_PARAMETER, printing a one-time [vitreo] line
// for each name.
TEST_P(GetNumberFormatTest, ExWellFormedNamesOutsideTheProfileTableFail) {
    for (const char16_t* name : {u"xx-YY", u"en-IN"}) {
        fill_out();
        EXPECT_EQ(ex(name, 0, u"-1234567.891", false, 128), 0u);
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
        EXPECT_EQ(out_a(2), "##");
    }
}

TEST_P(GetNumberFormatTest, ExFlags) {
    put_w(DEC, u".");
    put_w(THOU, u",");
    put_format(2, 1, 3, true, true, 1);
    // lpFormat NULL: LOCALE_NOUSEROVERRIDE and LOCALE_USE_CP_ACP.
    EXPECT_EQ(ex(u"en-US", 0x80000000u, u"1234.5", false, 128), 9u);   // gnfex-flags-nouseroverride
    EXPECT_EQ(out_w(9), z(u"1,234.50"));
    EXPECT_EQ(ex(u"en-US", 0x40000000u, u"1234.5", false, 128), 9u);   // -flags-use-cp-acp
    EXPECT_EQ(ex(u"en-US", 0xC0000000u, u"1", false, 128), 5u);        // -flags-use-cp-acp-nouseroverride
    EXPECT_EQ(out_w(5), z(u"1.00"));
    EXPECT_EQ(ex(nullptr, 0x80000000u, u"1234.5", false, 128), 9u);    // -flags-nouseroverride-null-name
    EXPECT_EQ(ex(u"de-DE", 0x80000000u, u"1234.5", false, 128), 9u);   // -flags-nouseroverride-de-DE
    EXPECT_EQ(out_w(9), z(u"1.234,50"));
    EXPECT_EQ(get_last_error(), kSeed);
    EXPECT_EQ(ex(u"en-US", 1, u"1234.5", false, 128), 0u);             // -flags-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(ex(u"en-US", 0x20000000u, u"1234.5", false, 128), 0u);   // -flags-0x20000000
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(ex(u"en-US", 1, u"x", false, 128), 0u);                  // -flags-1-bad-value: flags before value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(ex(u"en-US", 0x80000000u, u"x", false, 128), 0u);        // -flags-0x80000000-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // lpFormat set: only LOCALE_USE_CP_ACP (Learn: dwFlags 0 with lpFormat).
    EXPECT_EQ(ex(u"en-US", 0x40000000u, u"1234.5", true, 128), 9u);    // -fmt-flags-use-cp-acp
    EXPECT_EQ(out_w(9), z(u"1,234.50"));
    EXPECT_EQ(ex(u"en-US", 0x80000000u, u"1234.5", true, 128), 0u);    // -fmt-flags-nouseroverride
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(ex(u"en-US", 1, u"1234.5", true, 128), 0u);              // -fmt-flags-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(ex(u"en-US", 0xC0000000u, u"1", true, 128), 0u);         // -fmt-flags-use-cp-acp-nouseroverride
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    // The name is checked before the flags.
    EXPECT_EQ(ex(u"not a locale", 1, u"1234.5", false, 128), 0u);      // -invalid-name-flags-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(ex(u"not a locale", 0x80000000u, u"1", true, 128), 0u);  // -invalid-name-fmt-flags-nouseroverride
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_P(GetNumberFormatTest, ExNumberFmt) {
    put_w(DEC, u".");
    put_w(THOU, u",");
    put_format(2, 1, 3, true, true, 1);
    // lpFormat replaces the locale's whole format (gnfex-fmt, -fmt-de-DE,
    // -fmt-null-name, -fmt-invariant).
    for (const char16_t* name : {u"en-US", u"de-DE", static_cast<const char16_t*>(nullptr), u""}) {
        EXPECT_EQ(ex(name, 0, u"-1234567.891", true, 128), 14u);
        EXPECT_EQ(out_w(14), z(u"-1,234,567.89"));
    }
    EXPECT_EQ(ex(u"en-US", 0, u"-1234567.891", true, 0, true), 14u);   // -fmt-size-query
    put_w(THOU, u" ");
    put_format(0, 1, 32, true, true, 0);
    EXPECT_EQ(ex(u"en-US", 0, u"-1234567.891", true, 128), 12u);       // -fmt-nd0-gr32-no0
    EXPECT_EQ(out_w(12), z(u"(12 34 568)"));
    put_format(3, 0, 0, true, true, 4);
    EXPECT_EQ(ex(u"fr-FR", 0, u"-0.5", true, 128), 7u);                // -fmt-nd3-lz0-gr0-no4
    EXPECT_EQ(out_w(7), z(u".500 -"));
    put_w(THOU, u",");
    // NUMBERFMTW checks: ERROR_INVALID_PARAMETER.
    struct Bad { u32 nd, lz, no; bool dec, thou; const char16_t* decimal; };
    const Bad bad[] = {
        {10, 1, 1, true, true, u"."},      // -fmt-nd10
        {2, 2, 1, true, true, u"."},       // -fmt-lz2
        {2, 1, 5, true, true, u"."},       // -fmt-no5
        {2, 1, 1, true, true, u"<dec>"},   // -fmt-long-decimal
        {2, 1, 1, false, true, u"."},      // -fmt-null-decimal
        {2, 1, 1, true, false, u"."},      // -fmt-null-thousand
    };
    for (const Bad& b : bad) {
        put_w(DEC, b.decimal);
        put_format(b.nd, b.lz, 3, b.dec, b.thou, b.no);
        fill_out();
        EXPECT_EQ(ex(u"en-US", 0, u"1", true, 128), 0u) << b.nd << " " << b.lz << " " << b.no;
        EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
        EXPECT_EQ(out_a(2), "##");
    }
    put_w(DEC, u".");
    put_format(2, 1, 3, true, true, 1);
    EXPECT_EQ(ex(u"en-US", 0, u"12a", true, 128), 0u);                 // -fmt-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(ex(u"not a locale", 0, u"-1234567.891", true, 128), 0u); // -invalid-name-fmt
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    put_format(10, 1, 3, true, true, 1);
    EXPECT_EQ(ex(u"not a locale", 0, u"1", true, 128), 0u);            // -invalid-name-fmt-nd10
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

TEST_P(GetNumberFormatTest, ExValueSizeQueryAndBuffers) {
    struct Case {
        const char16_t* value;
        const char16_t* out;   // nullptr: ERROR_INVALID_PARAMETER
    };
    const Case cases[] = {   // gnfex-value-de-DE value=...
        {u"1234567.891", u"1.234.567,89"}, {u"-1234.5", u"-1.234,50"}, {u".5", u"0,50"}, {u"-0.004", u"0,00"},
        {u"999.995", u"1.000,00"}, {u"", u"0,00"}, {u"-", u"0,00"}, {u"abc", nullptr}, {u"1.2.3", nullptr},
        {u"+5", nullptr}, {u" 5", nullptr}, {u"1e5", nullptr}, {u"1,234", nullptr}, {u"0x10", nullptr},
    };
    for (const Case& c : cases) {
        fill_out();
        const u32 r = ex(u"de-DE", 0, c.value, false, 128);
        if (c.out == nullptr) {
            EXPECT_EQ(r, 0u);
            EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
            EXPECT_EQ(out_a(2), "##");
            continue;
        }
        const std::u16string want = z(c.out);
        EXPECT_EQ(r, want.size());
        EXPECT_EQ(out_w(want.size()), want);
        EXPECT_EQ(get_last_error(), kSeed);
    }
    // cchNumber 0 returns the size, NUL included, and writes nothing.
    fill_out();
    EXPECT_EQ(ex(u"en-US", 0, u"-1234567.891", false, 0), 14u);        // gnfex-size-query
    EXPECT_EQ(out_a(2), "##");
    EXPECT_EQ(get_last_error(), kSeed);
    EXPECT_EQ(ex(u"en-US", 0, u"-1234567.891", false, 0, true), 14u);  // -size-query-null-buffer
    EXPECT_EQ(get_last_error(), kSeed);
    EXPECT_EQ(ex(u"fr-FR", 0, u"-1234567.891", false, 0, true), 14u);  // -size-query-fr-FR
    EXPECT_EQ(get_last_error(), kSeed);
    EXPECT_EQ(ex(u"en-US", 0, u"x", false, 0, true), 0u);              // -size-query-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    // Buffers: a short one gets nothing (ERROR_INSUFFICIENT_BUFFER).
    EXPECT_EQ(ex(u"en-US", 0, u"-1234567.891", false, 14), 14u);       // -exact-buffer-14
    EXPECT_EQ(out_w(14), z(u"-1,234,567.89"));
    EXPECT_EQ(get_last_error(), kSeed);
    fill_out();
    EXPECT_EQ(ex(u"en-US", 0, u"-1234567.891", false, 13), 0u);        // -short-buffer-13
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(out_a(2), "##");
    EXPECT_EQ(ex(u"en-US", 0, u"-1234567.891", false, 1), 0u);         // -short-buffer-1
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INSUFFICIENT_BUFFER);
    EXPECT_EQ(out_a(2), "##");
    EXPECT_EQ(ex(u"en-US", 0, u"1", false, 5, true), 0u);              // -null-buffer-cch5
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(ex(u"en-US", 0, u"1", false, -1), 0u);                   // -negative-cch
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    put_w(NAME, u"en-US");
    EXPECT_EQ(call_ex(NAME, 0, 0, 0, OUT, 128), 0u);                   // -null-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call_ex(NAME, 0, 0, 0, 0, 0), 0u);                       // -null-value-size-query
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    fill_out();
    EXPECT_EQ(ex(u"en-US", 0, u"x", false, 1), 0u);                    // -bad-value-short-buffer
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(ex(u"en-US", 1, u"1", false, 1), 0u);                    // -bad-flags-short-buffer
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_FLAGS);
    EXPECT_EQ(out_a(2), "##");
    // A malformed name fails first, whatever else is wrong.
    EXPECT_EQ(ex(u"not a locale", 0, u"x", false, 128), 0u);           // gnfex-invalid-name-bad-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    put_w(NAME, u"not a locale");
    EXPECT_EQ(call_ex(NAME, 0, 0, 0, OUT, 128), 0u);                   // -invalid-name-null-value
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(ex(u"not a locale", 0, u"1", false, 0, true), 0u);       // -invalid-name-size-query
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(ex(u"not a locale", 0, u"1", false, -1), 0u);            // -invalid-name-negative-cch
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(ex(u"not a locale", 0, u"1", false, 5, true), 0u);       // -invalid-name-null-buffer-cch5
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    fill_out();
    EXPECT_EQ(ex(u"not a locale", 0, u"1", false, 1), 0u);             // -invalid-name-short-buffer
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(out_a(2), "##");
}

// locale_name_well_formed against every name the oracle measured (Windows
// ret != 0: well formed; ret=0 err=87: malformed). gnfex-name-*,
// gnfex-grammar-*, gnfex-fix1-name-*.
TEST(LocaleNameWellFormed, MatchesOracle) {
    const char16_t* well_formed[] = {
        u"en-US", u"en-GB", u"de-DE", u"fr-FR", u"zh-TW", u"cs-CZ", u"en-IN", u"EN-us", u"en_US", u"en", u"de",
        u"de-DE_phoneb", u"xx-YY", u"zz", u"x-sys-default-locale", u"de_DE", u"DE-de", u"de-DE_PHONEB",
        u"en-US_phoneb", u"es-ES_tradnl", u"ja-JP_radstr", u"en-US-x-foo", u"es-419", u"en-u-nu-thai",
        u"zh-Hans-CN", u"sr-Latn-RS", u"qps-ploc", u"x-foo", u"xx", u"xx-Yyyy-ZZ", u"en-US-POSIX",
        u"de-DE-phoneb", u"es_ES_tradnl", u"de_DE_phoneb", u"es-ES-tradnl", u"EN", u"De", u"zh-hans-cn",
        u"zh_Hans_CN", u"zh-Hans", u"en_US_x_foo", u"en-abcd", u"en-US-aaaaa-bbbbb", u"en-x-aaaaaaaa", u"en-a-bb",
        u"en-a-b", u"x-aaaaaaaa", u"en-Latn", u"en-Latn-US", u"en-123", u"abc", u"i-klingon", u"en-GB-oed",
        u"de_DE-phoneb",
    };
    const char16_t* malformed[] = {
        u"not a locale", u"en-US ", u"e", u"en-", u"-en", u"en--US", u"abcdefgh", u"abcdefghi", u"1234", u"en.US",
        u"xx-YYY", u"en-US-", u"\u00e9n-US", u"de-DE_phonebook", u"!x-sys-default-locale-x",
        u"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdef",
        u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee-ffffffff-gggggggg-hhhhhhhh-iiiiiiii-j",
        u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee-ffffffff-gggggggg-hhhhhhhh-iiiiiiii-jj",
        u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee-ffffffff-gggggggg-hhhhhhhh-iiiiiiii-jjj",
        u"en-aaaaa", u"en-aaaaaaaa", u"en-aaaaaaaaa", u"en-1abc", u"en-abc", u"en-aaaaaaaa-bbbbbbbb",
        u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd", u"en-aaaaaaaa-bbbbbbbb-cccccccc-dddddddd-eeeeeeee-ffffffff",
        u"en-x-aaaaaaaaa", u"x", u"en-12", u"abcd",
    };
    for (const char16_t* n : well_formed) {
        const std::u16string s(n);
        EXPECT_TRUE(locale_name_well_formed(s)) << std::string(s.begin(), s.end());
    }
    for (const char16_t* n : malformed) {
        const std::u16string s(n);
        EXPECT_FALSE(locale_name_well_formed(s)) << std::string(s.begin(), s.end());
    }
}

INSTANTIATE_TEST_SUITE_P(Bitness, GetNumberFormatTest, ::testing::Values(false, true),
                         [](const ::testing::TestParamInfo<bool>& info) { return info.param ? "x64" : "x86"; });

}  // namespace

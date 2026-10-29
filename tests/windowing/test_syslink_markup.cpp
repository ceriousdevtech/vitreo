// SysLink markup parsing. Expected link counts, IDs and URLs are the Windows
// results recorded in tools/oracle_probe/syslink_probe{32,64}.expected
// ("caseN link..." / "caseN count=" lines); display text follows from them.

#include <gtest/gtest.h>
#include <windowing/contracts/syslink_markup.hpp>

using namespace vitreo::windowing::syslink;

namespace {

Markup parse(const wchar_t* s) { return parse_markup(s, /*process_prefix=*/true); }

TEST(SysLinkMarkupTest, ProbedCasesMatchWindows) {
    {   // case0
        const auto m = parse(L"Visit <a href=\"https://x.example/\">site</a> now");
        ASSERT_EQ(m.items.size(), 1u);
        EXPECT_EQ(m.items[0].id, L"");
        EXPECT_EQ(m.items[0].url, L"https://x.example/");
        EXPECT_EQ(m.display, L"Visit site now");
        EXPECT_EQ(m.items[0].start, 6u);
        EXPECT_EQ(m.items[0].length, 4u);
    }
    {   // case1: attribute names and the tag are case-insensitive
        const auto m = parse(L"<a id=\"one\" href=\"u1\">A</a> and <A HREF=\"u2\" ID=\"two\">B</A>");
        ASSERT_EQ(m.items.size(), 2u);
        EXPECT_EQ(m.items[0].id, L"one");
        EXPECT_EQ(m.items[0].url, L"u1");
        EXPECT_EQ(m.items[1].id, L"two");
        EXPECT_EQ(m.items[1].url, L"u2");
        EXPECT_EQ(m.display, L"A and B");
    }
    // case2 single quotes, case3 unquoted, case5 unterminated, case6 other
    // tags, case12 spaces around '=': no links.
    EXPECT_TRUE(parse(L"<a href='u3'>C</a>").items.empty());
    EXPECT_TRUE(parse(L"<a href=u4>D</a>").items.empty());
    EXPECT_TRUE(parse(L"<a href=\"u5\">F unterminated").items.empty());
    EXPECT_TRUE(parse(L"x < y <b>bold</b> &amp; z").items.empty());
    EXPECT_TRUE(parse(L"<a  href = \"u9\" >J</a>").items.empty());
    {   // case4: <a> without href is a link with empty id and url
        const auto m = parse(L"<a>E</a> tail");
        ASSERT_EQ(m.items.size(), 1u);
        EXPECT_EQ(m.items[0].url, L"");
        EXPECT_EQ(m.display, L"E tail");
    }
    {   // case7: unknown attributes are ignored
        const auto m = parse(L"<a href=\"u6\" target=\"t\">G</a>");
        ASSERT_EQ(m.items.size(), 1u);
        EXPECT_EQ(m.items[0].url, L"u6");
    }
    {   // case8: id truncated to MAX_LINKID_TEXT - 1 = 47
        const auto m = parse(
            L"<a id=\"0123456789012345678901234567890123456789012345678901234567890\">H</a>");
        ASSERT_EQ(m.items.size(), 1u);
        EXPECT_EQ(m.items[0].id, L"01234567890123456789012345678901234567890123456");
    }
    EXPECT_TRUE(parse(L"").items.empty());  // case9
    {   // case11: empty link text is still a link
        const auto m = parse(L"<a href=\"u8\"></a>empty");
        ASSERT_EQ(m.items.size(), 1u);
        EXPECT_EQ(m.items[0].url, L"u8");
        EXPECT_EQ(m.items[0].length, 0u);
    }
    {   // case13: entities are not decoded
        const auto m = parse(L"<a href=\"a&amp;b\">K</a>");
        ASSERT_EQ(m.items.size(), 1u);
        EXPECT_EQ(m.items[0].url, L"a&amp;b");
    }
    {   // case14: adjacent links
        const auto m = parse(L"<a href=\"u10\">L</a><a href=\"u11\">M</a>");
        ASSERT_EQ(m.items.size(), 2u);
        EXPECT_EQ(m.items[0].url, L"u10");
        EXPECT_EQ(m.items[1].url, L"u11");
        EXPECT_EQ(m.items[1].start, 1u);
    }
}

TEST(SysLinkMarkupTest, UrlTruncatesToMaxUrlLengthMinusOne) {
    std::wstring s = L"<a href=\"" + std::wstring(3000, L'u') + L"\">x</a>";
    const auto m = parse_markup(s, true);
    ASSERT_EQ(m.items.size(), 1u);
    EXPECT_EQ(m.items[0].url.size(), kMaxUrlLength - 1);
    EXPECT_EQ(kMaxUrlLength, 2084u);
}

// Learn LWS_NOPREFIX: without it '&' marks the mnemonic and "&&" is '&'.
TEST(SysLinkMarkupTest, PrefixProcessingShiftsLinkOffsets) {
    const auto with = parse_markup(L"&Prefix && <a href=\"u7\">I</a>", true);
    EXPECT_EQ(with.display, L"Prefix & I");
    EXPECT_EQ(with.mnemonic, 0);
    ASSERT_EQ(with.items.size(), 1u);
    EXPECT_EQ(with.items[0].start, 9u);
    EXPECT_EQ(with.items[0].length, 1u);

    const auto without = parse_markup(L"&Prefix <a href=\"u7\">I</a>", false);
    EXPECT_EQ(without.display, L"&Prefix I");
    EXPECT_EQ(without.mnemonic, -1);
    EXPECT_EQ(without.items[0].start, 8u);
}

TEST(SysLinkMarkupTest, LayoutsMatchSdk) {
    // syslink_probe line 1: sizeof(LITEM) = 4280, NMLINK 4292 / 4304.
    EXPECT_EQ(kLitemSize, 4280u);
    EXPECT_EQ(nmlink_item_offset(false) + kLitemSize, 4292u);
    EXPECT_EQ(nmlink_item_offset(true) + kLitemSize, 4304u);
    EXPECT_EQ(LM_HITTEST, 0x700u);
    EXPECT_EQ(LM_GETITEM, 0x703u);
}

}  // namespace

// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_manager_harness.hpp"

#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST_F(DWriteManagerTest, CreateTypographyStoresFontFeatures) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const GuestAddr factory_vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(factory_vtable + 16 * 8).value(),
              CREATE_TYPOGRAPHY_THUNK);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_CreateTypography(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr typography = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(typography, 0u);
    const GuestAddr vtable = memory.read_u64(typography).value();
    EXPECT_EQ(memory.read_u64(vtable + 3 * 8).value(),
              TYPOGRAPHY_ADD_FONT_FEATURE_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 4 * 8).value(),
              TYPOGRAPHY_GET_FONT_FEATURE_COUNT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 5 * 8).value(),
              TYPOGRAPHY_GET_FONT_FEATURE_THUNK);

    cpu.gpr.rcx = typography;
    // DWRITE_FONT_FEATURE { nameTag=0x6C696761 ('liga'), parameter=1 }
    cpu.gpr.rdx = (u64{1} << 32) | 0x6C696761ull;
    manager.impl_TypographyAddFontFeature(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = typography;
    manager.impl_TypographyGetFontFeatureCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = typography;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    manager.impl_TypographyGetFontFeature(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(QUERY_OUTPUT_ADDRESS).value(), 0x6C696761u);
    EXPECT_EQ(memory.read_u32(QUERY_OUTPUT_ADDRESS + 4).value(), 1u);
}

TEST_F(DWriteManagerTest, CreateTextAnalyzerReportsScriptAndBidiViaSink) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const GuestAddr factory_vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(factory_vtable + 21 * 8).value(),
              CREATE_TEXT_ANALYZER_THUNK);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_CreateTextAnalyzer(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr analyzer = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(analyzer, 0u);
    const GuestAddr analyzer_vtable = memory.read_u64(analyzer).value();
    EXPECT_EQ(memory.read_u64(analyzer_vtable + 3 * 8).value(),
              ANALYZER_ANALYZE_SCRIPT_THUNK);
    EXPECT_EQ(memory.read_u64(analyzer_vtable + 7 * 8).value(),
              ANALYZER_GET_GLYPHS_THUNK);

    constexpr GuestAddr SOURCE = DATA_BASE + 0x300;
    constexpr GuestAddr SOURCE_VTABLE = DATA_BASE + 0x380;
    constexpr GuestAddr SINK = DATA_BASE + 0x400;
    constexpr GuestAddr SINK_VTABLE = DATA_BASE + 0x480;
    constexpr GuestAddr SET_SCRIPT = 0x71000100;
    constexpr GuestAddr SET_BIDI = 0x71000110;
    ASSERT_TRUE(memory.write_u64(SOURCE, SOURCE_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(SINK, SINK_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(SINK_VTABLE + 3 * 8, SET_SCRIPT).is_ok());
    ASSERT_TRUE(memory.write_u64(SINK_VTABLE + 5 * 8, SET_BIDI).is_ok());

    u32 script_calls = 0;
    u32 bidi_calls = 0;
    manager.set_guest_call(
        [&](GuestAddr function, const u64* args, usize count, bool is_64bit) {
            EXPECT_TRUE(is_64bit);
            if (function == SET_SCRIPT) {
                ++script_calls;
                EXPECT_EQ(count, 4u);
                EXPECT_EQ(args[0], SINK);
                EXPECT_EQ(args[1], 0u);
                EXPECT_EQ(args[2], 5u);
                EXPECT_NE(args[3], 0u);
                const u16 script =
                    memory.read_u16(static_cast<GuestAddr>(args[3])).value();
                EXPECT_EQ(script, 1u);
                return u64{0};
            }
            EXPECT_EQ(function, SET_BIDI);
            ++bidi_calls;
            EXPECT_EQ(count, 5u);
            EXPECT_EQ(args[0], SINK);
            EXPECT_EQ(args[1], 0u);
            EXPECT_EQ(args[2], 5u);
            EXPECT_EQ(args[3], 0u);
            EXPECT_EQ(args[4], 0u);
            return u64{0};
        });

    cpu.gpr.rcx = analyzer;
    cpu.gpr.rdx = SOURCE;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 5;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, SINK).is_ok());
    manager.impl_AnalyzerAnalyzeScript(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(script_calls, 1u);

    cpu.gpr.rcx = analyzer;
    cpu.gpr.rdx = SOURCE;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 5;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, SINK).is_ok());
    manager.impl_AnalyzerAnalyzeBidi(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(bidi_calls, 1u);
}

TEST_F(DWriteManagerTest, TextAnalyzerGetGlyphsMapsLatinRunAndPlacesAdvances) {
    const GuestAddr analyzer_factory = create_factory();
    ASSERT_NE(analyzer_factory, 0u);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = analyzer_factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_CreateTextAnalyzer(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr analyzer = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(analyzer, 0u);

    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);

    constexpr GuestAddr TEXT = DATA_BASE + 0x500;
    constexpr GuestAddr CLUSTER = DATA_BASE + 0x510;
    constexpr GuestAddr TEXT_PROPS = DATA_BASE + 0x520;
    constexpr GuestAddr GLYPHS = DATA_BASE + 0x530;
    constexpr GuestAddr GLYPH_PROPS = DATA_BASE + 0x540;
    constexpr GuestAddr COUNT_OUT = DATA_BASE + 0x550;
    constexpr GuestAddr ADVANCES = DATA_BASE + 0x560;
    constexpr GuestAddr OFFSETS = DATA_BASE + 0x580;
    const char16_t text[] = u"Hi";
    ASSERT_TRUE(memory.write_bytes(
        TEXT, reinterpret_cast<const Byte*>(text), 4).is_ok());

    auto write_stack = [&](usize index, u64 value) {
        ASSERT_TRUE(memory.write_u64(
            cpu.gpr.rsp + 0x28 + (index - 4) * 8, value).is_ok());
    };

    cpu.gpr.rcx = analyzer;
    cpu.gpr.rdx = TEXT;
    cpu.gpr.r8 = 2;
    cpu.gpr.r9 = face;
    write_stack(4, 0);   // isSideways
    write_stack(5, 0);   // isRightToLeft
    write_stack(6, 0);   // scriptAnalysis
    write_stack(7, 0);   // localeName
    write_stack(8, 0);   // numberSubstitution
    write_stack(9, 0);   // features
    write_stack(10, 0);  // featureRangeLengths
    write_stack(11, 0);  // featureRanges
    write_stack(12, 1);  // maxGlyphCount too small
    write_stack(13, CLUSTER);
    write_stack(14, TEXT_PROPS);
    write_stack(15, GLYPHS);
    write_stack(16, GLYPH_PROPS);
    write_stack(17, COUNT_OUT);
    manager.impl_AnalyzerGetGlyphs(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x8007007Au);

    write_stack(12, 8);
    manager.impl_AnalyzerGetGlyphs(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(COUNT_OUT).value(), 2u);
    EXPECT_EQ(memory.read_u16(CLUSTER).value(), 0u);
    EXPECT_EQ(memory.read_u16(CLUSTER + 2).value(), 1u);
    EXPECT_NE(memory.read_u16(GLYPHS).value(), 0u);
    EXPECT_NE(memory.read_u16(GLYPHS + 2).value(), 0u);
    EXPECT_EQ(memory.read_u16(GLYPH_PROPS).value(), 0x10u);

    const auto float_bits = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    cpu.gpr.rcx = analyzer;
    cpu.gpr.rdx = TEXT;
    cpu.gpr.r8 = CLUSTER;
    cpu.gpr.r9 = TEXT_PROPS;
    write_stack(4, 2);          // textLength
    write_stack(5, GLYPHS);
    write_stack(6, GLYPH_PROPS);
    write_stack(7, 2);          // glyphCount
    write_stack(8, face);
    write_stack(9, float_bits(16.0f));
    write_stack(10, 0);         // isSideways
    write_stack(11, 0);         // isRightToLeft
    write_stack(12, 0);         // scriptAnalysis
    write_stack(13, 0);         // localeName
    write_stack(14, 0);         // features
    write_stack(15, 0);         // featureRangeLengths
    write_stack(16, 0);         // featureRanges
    write_stack(17, ADVANCES);
    write_stack(18, OFFSETS);
    manager.impl_AnalyzerGetGlyphPlacements(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const u32 first_advance = memory.read_u32(ADVANCES).value();
    float advance = 0;
    std::memcpy(&advance, &first_advance, sizeof(advance));
    EXPECT_GT(advance, 0.0f);
    EXPECT_EQ(memory.read_u32(OFFSETS).value(), 0u);
}

TEST_F(DWriteManagerTest, CreatesIUnknownNumberSubstitutionObject) {
    constexpr GuestAddr LOCALE = DATA_BASE + 0x400;
    ASSERT_TRUE(memory.write_u16(LOCALE + 0, u'e').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 2, u'n').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 4, u'-').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 6, u'U').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 8, u'S').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 10, 0).is_ok());

    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 2; // DWRITE_NUMBER_SUBSTITUTION_METHOD_NONE
    cpu.gpr.r8 = LOCALE;
    cpu.gpr.r9 = 1;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, QUERY_OUTPUT_ADDRESS).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_CreateNumberSubstitution(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr substitution =
        memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(substitution, 0u);
    const GuestAddr vtable = memory.read_u64(substitution).value();
    EXPECT_EQ(memory.read_u64(vtable + 0 * 8).value(), QUERY_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 1 * 8).value(), ADDREF_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 2 * 8).value(), RELEASE_THUNK);
}

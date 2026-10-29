// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_manager_harness.hpp"

#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST_F(DWriteManagerTest, FontFaceReturnsIdealDesignGlyphMetrics) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);

    constexpr GuestAddr CODEPOINT = DATA_BASE + 0x500;
    constexpr GuestAddr GLYPH = DATA_BASE + 0x510;
    constexpr GuestAddr METRICS = DATA_BASE + 0x520;
    ASSERT_TRUE(memory.write_u32(CODEPOINT, 'A').is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = CODEPOINT;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = GLYPH;
    manager.impl_FaceGetGlyphIndices(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_NE(memory.read_u16(GLYPH).value_or(0), 0u);

    cpu.gpr.rcx = face;
    cpu.gpr.rdx = GLYPH;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = METRICS;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());
    manager.impl_FaceGetDesignGlyphMetrics(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_GT(memory.read_u32(METRICS + 4).value_or(0), 0u);
    EXPECT_GT(memory.read_u32(METRICS + 16).value_or(0), 0u);
    EXPECT_GT(memory.read_u32(METRICS + 24).value_or(0), 0u);

    cpu.gpr.rcx = face;
    manager.impl_FaceGetGlyphCount(marshaler);
    ASSERT_GT(cpu.gpr.rax, 0u);
    ASSERT_TRUE(memory.write_u16(
        GLYPH, static_cast<u16>(cpu.gpr.rax)).is_ok());
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = GLYPH;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = METRICS;
    manager.impl_FaceGetDesignGlyphMetrics(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u);
}

TEST_F(DWriteManagerTest, FontFaceReturnsGdiCompatibleGlyphMetricsForLatinH) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);

    constexpr GuestAddr CODEPOINT = DATA_BASE + 0x500;
    constexpr GuestAddr GLYPH = DATA_BASE + 0x510;
    constexpr GuestAddr METRICS = DATA_BASE + 0x520;
    ASSERT_TRUE(memory.write_u32(CODEPOINT, 'h').is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = CODEPOINT;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = GLYPH;
    manager.impl_FaceGetGlyphIndices(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_NE(memory.read_u16(GLYPH).value_or(0), 0u);

    const auto float_bits = [](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return bits;
    };
    cpu.gpr.rcx = face;
    cpu.sse.xmm[1].u32_[0] = float_bits(16.0f);
    cpu.sse.xmm[2].u32_[0] = float_bits(1.0f);
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, GLYPH).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, METRICS).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, 0).is_ok());
    manager.impl_FaceGetGdiCompatibleGlyphMetrics(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);

    const i32 lsb = static_cast<i32>(memory.read_u32(METRICS).value_or(0));
    const u32 advance = memory.read_u32(METRICS + 4).value_or(0);
    const i32 rsb = static_cast<i32>(memory.read_u32(METRICS + 8).value_or(0));
    ASSERT_GT(advance, 0u);
    const i32 black_box = static_cast<i32>(advance) - lsb - rsb;
    EXPECT_GT(black_box, static_cast<i32>(advance) / 4)
        << "lsb=" << lsb << " advance=" << advance << " rsb=" << rsb;
}

TEST_F(DWriteManagerTest, FontFaceQueryInterfaceAcceptsFontFace3AndHasCharacter) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);

    constexpr Guid IID_IDWRITE_FONT_FACE3{
        0xD37D7598u, 0x09BEu, 0x4222u,
        {0xA2, 0x36, 0x20, 0x81, 0x34, 0x1C, 0xC1, 0xF2}};
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_IDWRITE_FONT_FACE3),
        sizeof(IID_IDWRITE_FONT_FACE3)).is_ok());
    ASSERT_TRUE(memory.write_u64(QUERY_OUTPUT_ADDRESS, 0).is_ok());

    Marshaler query(memory);
    query.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    manager.impl_QueryInterface(query);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(QUERY_OUTPUT_ADDRESS).value(), face);

    const GuestAddr vtable = memory.read_u64(face).value();
    // IDWriteFontFace3::HasCharacter is slot 43 (0-based).
    EXPECT_EQ(memory.read_u64(vtable + 43 * 8).value(), FACE_HAS_CHARACTER_THUNK);
    // IDWriteFontFace7 final slot must be non-null (CreatePaintReader).
    EXPECT_NE(memory.read_u64(vtable + 61 * 8).value(), 0u);

    cpu.gpr.rcx = face;
    cpu.gpr.rdx = static_cast<u64>(u'A');
    manager.impl_FaceHasCharacter(query);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(DWriteManagerTest, FontTablesHaveIndependentReleasableGuestMappings) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);
    const GuestAddr vtable = memory.read_u64(face).value();
    EXPECT_EQ(memory.read_u64(vtable + 12 * 8).value(),
              FACE_TRY_GET_FONT_TABLE_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 13 * 8).value(),
              FACE_RELEASE_FONT_TABLE_THUNK);

    constexpr GuestAddr TABLE_DATA_OUT = DATA_BASE + 0x300;
    constexpr GuestAddr TABLE_SIZE_OUT = DATA_BASE + 0x308;
    constexpr GuestAddr TABLE_CONTEXT_OUT = DATA_BASE + 0x310;
    constexpr GuestAddr TABLE_EXISTS_OUT = DATA_BASE + 0x318;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = 0x70616d63u; // cmap in DWRITE_MAKE_OPENTYPE_TAG order.
    cpu.gpr.r8 = TABLE_DATA_OUT;
    cpu.gpr.r9 = TABLE_SIZE_OUT;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, TABLE_CONTEXT_OUT).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x30, TABLE_EXISTS_OUT).is_ok());
    manager.impl_FaceTryGetFontTable(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_EQ(memory.read_u32(TABLE_EXISTS_OUT).value(), 1u);
    ASSERT_GT(memory.read_u32(TABLE_SIZE_OUT).value(), 0u);
    const GuestAddr table_data = memory.read_u64(TABLE_DATA_OUT).value();
    const GuestAddr table_context =
        memory.read_u64(TABLE_CONTEXT_OUT).value();
    ASSERT_NE(table_data, 0u);
    EXPECT_EQ(table_context, table_data);
    EXPECT_TRUE(memory.is_mapped(table_data));
    EXPECT_TRUE(memory.read_u32(table_data).is_ok());
    EXPECT_TRUE(memory.write_u8(table_data, 0).is_err())
        << "font table data must be guest-read-only";

    cpu.gpr.rcx = face;
    cpu.gpr.rdx = table_context;
    manager.impl_FaceReleaseFontTable(marshaler);
    EXPECT_FALSE(memory.is_mapped(table_data));

    cpu.gpr.rcx = face;
    cpu.gpr.rdx = 0x7a7a7a7au;
    cpu.gpr.r8 = TABLE_DATA_OUT;
    cpu.gpr.r9 = TABLE_SIZE_OUT;
    manager.impl_FaceTryGetFontTable(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(TABLE_EXISTS_OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(TABLE_SIZE_OUT).value(), 0u);
    EXPECT_EQ(memory.read_u64(TABLE_DATA_OUT).value(), 0u);
    EXPECT_EQ(memory.read_u64(TABLE_CONTEXT_OUT).value(), 0u);
}

TEST_F(DWriteManagerTest, FaceGetGlyphRunOutlineDrivesGeometrySink) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);
    const GuestAddr face_vtable = memory.read_u64(face).value();
    EXPECT_EQ(memory.read_u64(face_vtable + 14 * 8).value(),
              FACE_GET_GLYPH_RUN_OUTLINE_THUNK);

    constexpr GuestAddr CODEPOINTS = DATA_BASE + 0x300;
    constexpr GuestAddr GLYPHS_OUT = DATA_BASE + 0x310;
    constexpr GuestAddr SINK = DATA_BASE + 0x400;
    constexpr GuestAddr SINK_VTABLE = DATA_BASE + 0x480;
    ASSERT_TRUE(memory.write_u32(CODEPOINTS, static_cast<u32>(u'B')).is_ok());
    for (u32 slot = 0; slot < 10; ++slot) {
        ASSERT_TRUE(memory.write_u64(
                        SINK_VTABLE + slot * 8,
                        0x70000600ull + slot * 0x10)
                        .is_ok());
    }
    ASSERT_TRUE(memory.write_u64(SINK, SINK_VTABLE).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = CODEPOINTS;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = GLYPHS_OUT;
    manager.impl_FaceGetGlyphIndices(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const u16 glyph = memory.read_u16(GLYPHS_OUT).value();
    ASSERT_NE(glyph, 0u);

    std::vector<u32> sink_arg_counts;
    manager.set_guest_call(
        [&](GuestAddr, const u64*, usize count, bool) -> u64 {
            sink_arg_counts.push_back(static_cast<u32>(count));
            return 0;
        });

    const float em_size = 18.0f;
    u32 em_bits = 0;
    std::memcpy(&em_bits, &em_size, sizeof(em_bits));
    cpu.sse.xmm[1].u32_[0] = em_bits;
    cpu.gpr.rcx = face;
    cpu.gpr.r8 = GLYPHS_OUT;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x30, 1).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x38, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x40, 0).is_ok());
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x48, SINK).is_ok());
    manager.impl_FaceGetGlyphRunOutline(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    ASSERT_FALSE(sink_arg_counts.empty());
    EXPECT_EQ(sink_arg_counts.front(), 2u);
    EXPECT_GE(sink_arg_counts.size(), 3u);
}

TEST_F(DWriteManagerTest, ReleasingFontFaceInvalidatesUnreleasedTables) {
    const GuestAddr face = create_font_face();
    ASSERT_NE(face, 0u);

    constexpr GuestAddr TABLE_DATA_OUT = DATA_BASE + 0x300;
    constexpr GuestAddr TABLE_SIZE_OUT = DATA_BASE + 0x308;
    constexpr GuestAddr TABLE_CONTEXT_OUT = DATA_BASE + 0x310;
    constexpr GuestAddr TABLE_EXISTS_OUT = DATA_BASE + 0x318;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = face;
    cpu.gpr.rdx = 0x70616d63u;
    cpu.gpr.r8 = TABLE_DATA_OUT;
    cpu.gpr.r9 = TABLE_SIZE_OUT;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, TABLE_CONTEXT_OUT).is_ok());
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x30, TABLE_EXISTS_OUT).is_ok());
    manager.impl_FaceTryGetFontTable(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr table_data = memory.read_u64(TABLE_DATA_OUT).value();
    ASSERT_TRUE(memory.is_mapped(table_data));

    cpu.gpr.rcx = face;
    manager.impl_Release(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_FALSE(memory.is_mapped(table_data));
}

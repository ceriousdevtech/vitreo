// SPDX-License-Identifier: LGPL-2.1-only

#include "dwrite_manager_harness.hpp"

#include <array>

using namespace vitreo;
using namespace vitreo::winapi;
using namespace vitreo::winapi::dwrite_test;

TEST_F(DWriteManagerTest, GetSystemFontSetExposesNonZeroFontCount) {
    const GuestAddr factory = create_factory();
    ASSERT_NE(factory, 0u);
    const GuestAddr factory_vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(factory_vtable + 35 * 8).value(),
              GET_SYSTEM_FONT_SET_THUNK);
    EXPECT_EQ(memory.read_u64(factory_vtable + 27 * 8).value(),
              CREATE_FALLBACK_BUILDER_THUNK);
    EXPECT_EQ(memory.read_u64(factory_vtable + 39 * 8).value(),
              GET_FONT_DOWNLOAD_QUEUE_THUNK);

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_GetSystemFontSet(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr font_set = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(font_set, 0u);
    const GuestAddr vtable = memory.read_u64(font_set).value();
    EXPECT_EQ(memory.read_u64(vtable + 3 * 8).value(),
              FONT_SET_GET_FONT_COUNT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 11 * 8).value(),
              FONT_SET_GET_MATCHING_FONTS_FAMILY_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 12 * 8).value(),
              FONT_SET_GET_MATCHING_FONTS_PROPERTIES_THUNK);

    cpu.gpr.rcx = font_set;
    manager.impl_FontSetGetFontCount(marshaler);
    EXPECT_GT(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = font_set;
    cpu.gpr.rdx = DATA_BASE + 0x300; // DWRITE_FONT_PROPERTY array
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = QUERY_OUTPUT_ADDRESS;
    manager.impl_FontSetGetMatchingFontsProperties(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr filtered_set =
        memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(filtered_set, 0u);
    cpu.gpr.rcx = filtered_set;
    manager.impl_FontSetGetFontCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_CreateFontFallbackBuilder(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr builder = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(builder, 0u);
    cpu.gpr.rcx = builder;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_FallbackBuilderCreateFontFallback(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_NE(memory.read_u64(QUERY_OUTPUT_ADDRESS).value(), 0u);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_GetFontDownloadQueue(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr queue = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(queue, 0u);
    cpu.gpr.rcx = queue;
    manager.impl_DownloadQueueIsEmpty(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
}

TEST_F(DWriteManagerTest, SystemFontCollectionContainsCoreTextFamilies) {
    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    cpu.gpr.r8 = 0;
    Marshaler get_collection(memory);
    get_collection.attach_cpu(cpu);
    manager.impl_GetSystemFontCollection(get_collection);
    ASSERT_EQ(cpu.gpr.rax, 0u);

    const GuestAddr collection = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(collection, 0u);
    const GuestAddr vtable = memory.read_u64(collection).value();
    EXPECT_EQ(memory.read_u64(vtable + 24).value(), GET_COUNT_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 32).value(), GET_FAMILY_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 40).value(), FIND_FAMILY_THUNK);

    cpu.gpr.rcx = collection;
    Marshaler get_count(memory);
    get_count.attach_cpu(cpu);
    manager.impl_GetFontFamilyCount(get_count);
    EXPECT_GT(cpu.gpr.rax, 0u);
}

TEST_F(DWriteManagerTest, SystemFontFallbackIsACompleteComObject) {
    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_GetSystemFontFallback(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr fallback = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(fallback, 0u);
    const GuestAddr vtable = memory.read_u64(fallback).value();
    EXPECT_EQ(memory.read_u64(vtable + 0 * 8).value(), QUERY_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 1 * 8).value(), ADDREF_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 2 * 8).value(), RELEASE_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 3 * 8).value(), MAP_CHARACTERS_THUNK);
}

TEST_F(DWriteManagerTest, FontFallbackMapsGuestTextToRealFont) {
    constexpr GuestAddr SOURCE = DATA_BASE + 0x400;
    constexpr GuestAddr SOURCE_VTABLE = DATA_BASE + 0x480;
    constexpr GuestAddr TEXT = DATA_BASE + 0x600;
    constexpr GuestAddr LOCALE = DATA_BASE + 0x680;
    constexpr GuestAddr GET_TEXT_CALLBACK = 0x71000010;
    constexpr GuestAddr GET_LOCALE_CALLBACK = 0x71000020;
    constexpr GuestAddr MAPPED_LENGTH = DATA_BASE + 0x700;
    constexpr GuestAddr MAPPED_FONT = DATA_BASE + 0x708;
    constexpr GuestAddr SCALE = DATA_BASE + 0x710;

    ASSERT_TRUE(memory.write_u64(SOURCE, SOURCE_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u64(
        SOURCE_VTABLE + 3 * 8, GET_TEXT_CALLBACK).is_ok());
    ASSERT_TRUE(memory.write_u64(
        SOURCE_VTABLE + 6 * 8, GET_LOCALE_CALLBACK).is_ok());
    ASSERT_TRUE(memory.write_u16(TEXT + 0, u'a').is_ok());
    ASSERT_TRUE(memory.write_u16(TEXT + 2, u'b').is_ok());
    ASSERT_TRUE(memory.write_u16(TEXT + 4, u'c').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 0, u'e').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 2, u'n').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 4, u'-').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 6, u'U').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 8, u'S').is_ok());
    ASSERT_TRUE(memory.write_u16(LOCALE + 10, 0).is_ok());

    manager.set_guest_call(
        [this, get_text_callback = GET_TEXT_CALLBACK,
         get_locale_callback = GET_LOCALE_CALLBACK, text = TEXT,
         locale = LOCALE](GuestAddr function, const u64* args, usize count,
                          bool is_64bit) {
            EXPECT_TRUE(is_64bit);
            EXPECT_EQ(count, 4u);
            if (function == get_text_callback) {
                EXPECT_TRUE(memory.write_u64(args[2], text).is_ok());
                EXPECT_TRUE(memory.write_u32(args[3], 3).is_ok());
                return u64{0};
            }
            EXPECT_EQ(function, get_locale_callback);
            EXPECT_TRUE(memory.write_u32(args[2], 3).is_ok());
            EXPECT_TRUE(memory.write_u64(args[3], locale).is_ok());
            return u64{0};
        });

    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_GetSystemFontFallback(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr fallback = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();

    const std::array<u64, 8> stack_args{
        0, 0, 400, 0, 5, MAPPED_LENGTH, MAPPED_FONT, SCALE};
    for (usize index = 0; index < stack_args.size(); ++index) {
        ASSERT_TRUE(memory.write_u64(
            cpu.gpr.rsp + 0x28 + index * sizeof(u64),
            stack_args[index]).is_ok());
    }
    cpu.gpr.rcx = fallback;
    cpu.gpr.rdx = SOURCE;
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = 3;
    manager.impl_FallbackMapCharacters(marshaler);

    ASSERT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(MAPPED_LENGTH).value(), 3u);
    const GuestAddr mapped_font = memory.read_u64(MAPPED_FONT).value();
    ASSERT_NE(mapped_font, 0u);
    EXPECT_EQ(memory.read_u32(SCALE).value(), 0x3f800000u);
    const GuestAddr font_vtable = memory.read_u64(mapped_font).value();
    EXPECT_EQ(memory.read_u64(font_vtable + 4 * 8).value(),
              FONT_GET_WEIGHT_THUNK);
}

TEST_F(DWriteManagerTest, FirstMatchingFontExposesResolvedPropertiesAndMetrics) {
    const GuestAddr factory = create_factory();
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    cpu.gpr.r8 = 0;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_GetSystemFontCollection(marshaler);
    const GuestAddr collection = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();

    cpu.gpr.rcx = collection;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    manager.impl_GetFontFamily(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr family = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();

    cpu.gpr.rcx = family;
    cpu.gpr.rdx = 400;
    cpu.gpr.r8 = 5;
    cpu.gpr.r9 = 0;
    ASSERT_TRUE(memory.write_u64(
        cpu.gpr.rsp + 0x28, QUERY_OUTPUT_ADDRESS).is_ok());
    manager.impl_GetFirstMatchingFont(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr font = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(font, 0u);
    const GuestAddr vtable = memory.read_u64(font).value();
    EXPECT_EQ(memory.read_u64(vtable + 10 * 8).value(),
              FONT_GET_SIMULATIONS_THUNK);
    EXPECT_EQ(memory.read_u64(vtable + 8 * 8).value(),
              FONT_GET_FACE_NAMES_THUNK);

    cpu.gpr.rcx = font;
    manager.impl_FontGetWeight(marshaler);
    EXPECT_GE(cpu.gpr.rax, 1u);
    EXPECT_LE(cpu.gpr.rax, 999u);
    manager.impl_FontGetSimulations(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    cpu.gpr.rcx = font;
    cpu.gpr.rdx = DATA_BASE + 0x300;
    manager.impl_FontGetMetrics(marshaler);
    EXPECT_GT(memory.read_u16(DATA_BASE + 0x300).value_or(0), 0u);
    EXPECT_GT(memory.read_u16(DATA_BASE + 0x302).value_or(0), 0u);

    cpu.gpr.rcx = font;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_FontGetFaceNames(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr face_names =
        memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);
    ASSERT_NE(face_names, 0u);
    cpu.gpr.rcx = face_names;
    manager.impl_StringsGetCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);
    constexpr GuestAddr FACE_NAME_LENGTH = DATA_BASE + 0x340;
    constexpr GuestAddr FACE_NAME_BUFFER = DATA_BASE + 0x360;
    cpu.gpr.rcx = face_names;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = FACE_NAME_LENGTH;
    manager.impl_StringsGetStringLength(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const u32 face_name_length =
        memory.read_u32(FACE_NAME_LENGTH).value_or(0);
    ASSERT_GT(face_name_length, 0u);
    cpu.gpr.rcx = face_names;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = FACE_NAME_BUFFER;
    cpu.gpr.r9 = face_name_length + 1;
    manager.impl_StringsGetString(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const auto face_name = marshaler.read_wide_string(FACE_NAME_BUFFER);
    ASSERT_TRUE(face_name.is_ok());
    EXPECT_FALSE(face_name.value().empty());
}

TEST_F(DWriteManagerTest, FontFamilyExposesLocalizedFamilyName) {
    const GuestAddr factory = create_factory();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    cpu.gpr.r8 = 0;
    manager.impl_GetSystemFontCollection(marshaler);
    const GuestAddr collection = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();

    cpu.gpr.rcx = collection;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
    manager.impl_GetFontFamily(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr family = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    const GuestAddr family_vtable = memory.read_u64(family).value();
    EXPECT_EQ(memory.read_u64(family_vtable + 6 * 8).value(),
              FAMILY_GET_NAMES_THUNK);

    cpu.gpr.rcx = family;
    cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
    manager.impl_FamilyGetNames(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr names = memory.read_u64(QUERY_OUTPUT_ADDRESS).value();
    ASSERT_NE(names, 0u);
    const GuestAddr names_vtable = memory.read_u64(names).value();
    EXPECT_EQ(memory.read_u64(names_vtable + 3 * 8).value(),
              STRINGS_GET_COUNT_THUNK);
    EXPECT_EQ(memory.read_u64(names_vtable + 7 * 8).value(),
              STRINGS_GET_STRING_LENGTH_THUNK);
    EXPECT_EQ(memory.read_u64(names_vtable + 8 * 8).value(),
              STRINGS_GET_STRING_THUNK);

    cpu.gpr.rcx = names;
    manager.impl_StringsGetCount(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    constexpr GuestAddr LENGTH_OUT = DATA_BASE + 0x300;
    constexpr GuestAddr STRING_OUT = DATA_BASE + 0x320;
    cpu.gpr.rcx = names;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = LENGTH_OUT;
    manager.impl_StringsGetStringLength(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const u32 length = memory.read_u32(LENGTH_OUT).value();
    ASSERT_GT(length, 0u);

    cpu.gpr.rcx = names;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = STRING_OUT;
    cpu.gpr.r9 = length + 1;
    manager.impl_StringsGetString(marshaler);
    ASSERT_EQ(cpu.gpr.rax, 0u);
    const auto family_name = marshaler.read_wide_string(STRING_OUT);
    ASSERT_TRUE(family_name.is_ok());
    EXPECT_FALSE(family_name.value().empty());

    constexpr GuestAddr LOCALE = DATA_BASE + 0x380;
    constexpr GuestAddr INDEX_OUT = DATA_BASE + 0x3A0;
    constexpr GuestAddr EXISTS_OUT = DATA_BASE + 0x3A8;
    const std::u16string locale = u"en-US";
    ASSERT_TRUE(memory.write_bytes(
        LOCALE, reinterpret_cast<const Byte*>(locale.c_str()),
        (locale.size() + 1) * sizeof(char16_t)).is_ok());
    cpu.gpr.rcx = names;
    cpu.gpr.rdx = LOCALE;
    cpu.gpr.r8 = INDEX_OUT;
    cpu.gpr.r9 = EXISTS_OUT;
    manager.impl_StringsFindLocaleName(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(INDEX_OUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(EXISTS_OUT).value(), 1u);

    cpu.gpr.rcx = names;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = STRING_OUT;
    cpu.gpr.r9 = length;
    manager.impl_StringsGetString(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x8007007au);
}

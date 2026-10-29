// SPDX-License-Identifier: LGPL-2.1-only
// VITREO_LARGE_FILE_OK: a single fixture class; its members are one contract and cannot be split without changing code
#pragma once
/// @file dwrite_manager_harness.hpp
/// @brief Shared DWriteManagerTest fixture for the DirectWrite manager tests.
///
/// Builds a DWriteManager over a fresh GuestMemory/CpuState with a complete
/// method-thunk table, and drives COM methods through their vtable slots the
/// way a guest does. The namespace is named (not anonymous) because every
/// TEST_F of the DWriteManagerTest suite, across all test_dwrite_*.cpp files,
/// must name the same fixture type.

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/dwrite.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <tuple>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace vitreo::winapi::dwrite_test {

struct Guid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};

inline constexpr Guid IID_IDWRITE_FACTORY{
    0xB859EE5Au, 0xD838u, 0x4B5Bu, {0xA2, 0xE8, 0x1A, 0xDC, 0x7D, 0x93, 0xDB, 0x48}};
inline constexpr Guid IID_IDWRITE_FACTORY3{
    0x9A1B41C3u, 0xD3BBu, 0x466Au, {0x87, 0xFC, 0xFE, 0x67, 0x55, 0x6A, 0x3B, 0x65}};

class DWriteManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.mode = cpu::CpuMode::Long;
        cpu.gpr.rsp = STACK_BASE + 0x800;
        manager.set_method_thunks(method_thunks());
        ASSERT_TRUE(memory.write_bytes(IID_ADDRESS,
            reinterpret_cast<const Byte*>(&IID_IDWRITE_FACTORY), sizeof(IID_IDWRITE_FACTORY)).is_ok());
    }

    GuestAddr create_factory() {
        cpu.gpr.rcx = 0;
        cpu.gpr.rdx = IID_ADDRESS;
        cpu.gpr.r8 = OUTPUT_ADDRESS;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_DWriteCreateFactory(marshaler);
        EXPECT_EQ(cpu.gpr.rax, 0u);
        return memory.read_u64(OUTPUT_ADDRESS).value();
    }

    GuestAddr create_font_face() {
        const GuestAddr factory = create_factory();
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);

        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
        cpu.gpr.r8 = 0;
        manager.impl_GetSystemFontCollection(marshaler);
        const GuestAddr collection =
            memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);

        cpu.gpr.rcx = collection;
        cpu.gpr.rdx = 0;
        cpu.gpr.r8 = QUERY_OUTPUT_ADDRESS;
        manager.impl_GetFontFamily(marshaler);
        const GuestAddr family =
            memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);

        cpu.gpr.rcx = family;
        cpu.gpr.rdx = 400;
        cpu.gpr.r8 = 5;
        cpu.gpr.r9 = 0;
        EXPECT_TRUE(memory.write_u64(
            cpu.gpr.rsp + 0x28, QUERY_OUTPUT_ADDRESS).is_ok());
        manager.impl_GetFirstMatchingFont(marshaler);
        const GuestAddr font =
            memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);

        cpu.gpr.rcx = font;
        cpu.gpr.rdx = QUERY_OUTPUT_ADDRESS;
        manager.impl_FontCreateFontFace(marshaler);
        return memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);
    }

    // Calls a COM method the way a guest does: through the slot of the
    // object's vtable, dispatched to the impl its installed thunk stands for.
    u32 call_vtable(GuestAddr object, u32 slot, u64 a1 = 0, u64 a2 = 0,
                    u64 a3 = 0) {
        using Impl = void (DWriteManager::*)(Marshaler&);
        static const std::pair<GuestAddr, Impl> impls[]{
            {RELEASE_THUNK, &DWriteManager::impl_Release},
            {GET_GDI_INTEROP_THUNK, &DWriteManager::impl_GetGdiInterop},
            {GDI_CREATE_FONT_FROM_LOGFONT_THUNK,
             &DWriteManager::impl_GdiCreateFontFromLogfont},
            {GDI_CONVERT_FONT_TO_LOGFONT_THUNK,
             &DWriteManager::impl_GdiConvertFontToLogfont},
            {GDI_CONVERT_FONT_FACE_TO_LOGFONT_THUNK,
             &DWriteManager::impl_GdiConvertFontFaceToLogfont},
            {GET_FAMILY_THUNK, &DWriteManager::impl_GetFontFamily},
            {FAMILY_GET_FONT_THUNK, &DWriteManager::impl_FamilyGetFont},
            {FAMILY_GET_NAMES_THUNK, &DWriteManager::impl_FamilyGetNames},
            {FONT_GET_FAMILY_THUNK, &DWriteManager::impl_FontGetFamily},
            {FONT_GET_WEIGHT_THUNK, &DWriteManager::impl_FontGetWeight},
            {FONT_GET_STYLE_THUNK, &DWriteManager::impl_FontGetStyle},
            {FONT_GET_FACE_NAMES_THUNK, &DWriteManager::impl_FontGetFaceNames},
            {FONT_GET_SIMULATIONS_THUNK, &DWriteManager::impl_FontGetSimulations},
            {FONT_CREATE_FONT_FACE_THUNK, &DWriteManager::impl_FontCreateFontFace},
            {FACE_GET_FILES_THUNK, &DWriteManager::impl_FaceGetFiles},
            {FACE_GET_SIMULATIONS_THUNK, &DWriteManager::impl_FaceGetSimulations},
            {FACE_GET_WEIGHT_THUNK, &DWriteManager::impl_FaceGetWeight},
            {FACE_GET_STYLE_THUNK, &DWriteManager::impl_FaceGetStyle},
            {FACE_GET_FACE_NAMES_THUNK, &DWriteManager::impl_FaceGetFaceNames},
            {STRINGS_GET_STRING_LENGTH_THUNK,
             &DWriteManager::impl_StringsGetStringLength},
            {STRINGS_GET_STRING_THUNK, &DWriteManager::impl_StringsGetString},
        };
        const GuestAddr vtable = memory.read_u64(object).value_or(0);
        const GuestAddr thunk = memory.read_u64(vtable + slot * 8).value_or(0);
        const auto found = std::find_if(
            std::begin(impls), std::end(impls),
            [&](const auto& entry) { return entry.first == thunk; });
        EXPECT_NE(found, std::end(impls))
            << "slot " << slot << " holds thunk 0x" << std::hex << thunk;
        if (found == std::end(impls)) return 0xFFFFFFFFu;
        cpu.gpr.rcx = object;
        cpu.gpr.rdx = a1;
        cpu.gpr.r8 = a2;
        cpu.gpr.r9 = a3;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        (manager.*found->second)(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }

    GuestAddr create_gdi_interop() {
        const GuestAddr factory = create_factory();
        EXPECT_EQ(call_vtable(factory, 17, QUERY_OUTPUT_ADDRESS), 0u);
        return memory.read_u64(QUERY_OUTPUT_ADDRESS).value_or(0);
    }

public:
    static DWriteManager::MethodThunks method_thunks() {
        DWriteManager::MethodThunks thunks{};
        thunks.query_interface = QUERY_THUNK;
        thunks.add_ref = ADDREF_THUNK;
        thunks.release = RELEASE_THUNK;
        thunks.get_system_font_collection = GET_COLLECTION_THUNK;
        thunks.create_custom_font_collection = CREATE_CUSTOM_COLLECTION_THUNK;
        thunks.register_font_collection_loader = REGISTER_COLLECTION_LOADER_THUNK;
        thunks.unregister_font_collection_loader = UNREGISTER_COLLECTION_LOADER_THUNK;
        thunks.create_font_file_reference = CREATE_FONT_FILE_REFERENCE_THUNK;
        thunks.create_custom_font_file_reference =
            CREATE_CUSTOM_FONT_FILE_REFERENCE_THUNK;
        thunks.create_rendering_params = CREATE_RENDERING_PARAMS_THUNK;
        thunks.create_monitor_rendering_params =
            CREATE_MONITOR_RENDERING_PARAMS_THUNK;
        thunks.create_typography = CREATE_TYPOGRAPHY_THUNK;
        thunks.create_text_analyzer = CREATE_TEXT_ANALYZER_THUNK;
        thunks.typography_add_font_feature = TYPOGRAPHY_ADD_FONT_FEATURE_THUNK;
        thunks.typography_get_font_feature_count =
            TYPOGRAPHY_GET_FONT_FEATURE_COUNT_THUNK;
        thunks.typography_get_font_feature =
            TYPOGRAPHY_GET_FONT_FEATURE_THUNK;
        thunks.analyzer_analyze_script = ANALYZER_ANALYZE_SCRIPT_THUNK;
        thunks.analyzer_analyze_bidi = ANALYZER_ANALYZE_BIDI_THUNK;
        thunks.analyzer_analyze_number_substitution =
            ANALYZER_ANALYZE_NUMBER_SUBSTITUTION_THUNK;
        thunks.analyzer_analyze_line_breakpoints =
            ANALYZER_ANALYZE_LINE_BREAKPOINTS_THUNK;
        thunks.analyzer_get_glyphs = ANALYZER_GET_GLYPHS_THUNK;
        thunks.analyzer_get_glyph_placements =
            ANALYZER_GET_GLYPH_PLACEMENTS_THUNK;
        thunks.analyzer_get_gdi_compatible_glyph_placements =
            ANALYZER_GET_GDI_COMPATIBLE_GLYPH_PLACEMENTS_THUNK;
        thunks.register_font_file_loader = REGISTER_FILE_LOADER_THUNK;
        thunks.unregister_font_file_loader = UNREGISTER_FILE_LOADER_THUNK;
        thunks.get_gdi_interop = GET_GDI_INTEROP_THUNK;
        thunks.gdi_create_font_from_logfont = GDI_CREATE_FONT_FROM_LOGFONT_THUNK;
        thunks.gdi_convert_font_to_logfont = GDI_CONVERT_FONT_TO_LOGFONT_THUNK;
        thunks.gdi_convert_font_face_to_logfont =
            GDI_CONVERT_FONT_FACE_TO_LOGFONT_THUNK;
        thunks.gdi_create_font_face_from_hdc =
            GDI_CREATE_FONT_FACE_FROM_HDC_THUNK;
        thunks.create_glyph_run_analysis = CREATE_GLYPH_RUN_ANALYSIS_THUNK;
        thunks.create_glyph_run_analysis2 = CREATE_GLYPH_RUN_ANALYSIS2_THUNK;
        thunks.analysis_get_alpha_texture_bounds =
            ANALYSIS_GET_ALPHA_TEXTURE_BOUNDS_THUNK;
        thunks.analysis_create_alpha_texture =
            ANALYSIS_CREATE_ALPHA_TEXTURE_THUNK;
        thunks.analysis_get_alpha_blend_params =
            ANALYSIS_GET_ALPHA_BLEND_PARAMS_THUNK;
        thunks.get_system_font_fallback = GET_FALLBACK_THUNK;
        thunks.create_font_fallback_builder = CREATE_FALLBACK_BUILDER_THUNK;
        thunks.get_system_font_set = GET_SYSTEM_FONT_SET_THUNK;
        thunks.get_font_download_queue = GET_FONT_DOWNLOAD_QUEUE_THUNK;
        thunks.font_set_get_font_count = FONT_SET_GET_FONT_COUNT_THUNK;
        thunks.font_set_get_matching_fonts_family =
            FONT_SET_GET_MATCHING_FONTS_FAMILY_THUNK;
        thunks.font_set_get_matching_fonts_properties =
            FONT_SET_GET_MATCHING_FONTS_PROPERTIES_THUNK;
        thunks.fallback_builder_add_mapping =
            FALLBACK_BUILDER_ADD_MAPPING_THUNK;
        thunks.fallback_builder_add_mappings =
            FALLBACK_BUILDER_ADD_MAPPINGS_THUNK;
        thunks.fallback_builder_create_font_fallback =
            FALLBACK_BUILDER_CREATE_FONT_FALLBACK_THUNK;
        thunks.download_queue_is_empty = DOWNLOAD_QUEUE_IS_EMPTY_THUNK;
        thunks.download_queue_begin_download =
            DOWNLOAD_QUEUE_BEGIN_DOWNLOAD_THUNK;
        thunks.download_queue_get_generation_count =
            DOWNLOAD_QUEUE_GET_GENERATION_COUNT_THUNK;
        thunks.create_number_substitution = CREATE_NUMBER_SUBSTITUTION_THUNK;
        thunks.fallback_map_characters = MAP_CHARACTERS_THUNK;
        thunks.get_font_family_count = GET_COUNT_THUNK;
        thunks.get_font_family = GET_FAMILY_THUNK;
        thunks.find_family_name = FIND_FAMILY_THUNK;
        thunks.family_get_font_count = FAMILY_GET_FONT_COUNT_THUNK;
        thunks.family_get_font = FAMILY_GET_FONT_THUNK;
        thunks.family_get_names = FAMILY_GET_NAMES_THUNK;
        thunks.get_first_matching_font = GET_MATCHING_FONT_THUNK;
        thunks.font_get_family = FONT_GET_FAMILY_THUNK;
        thunks.font_get_weight = FONT_GET_WEIGHT_THUNK;
        thunks.font_get_stretch = FONT_GET_STRETCH_THUNK;
        thunks.font_get_style = FONT_GET_STYLE_THUNK;
        thunks.font_is_symbol_font = FONT_IS_SYMBOL_THUNK;
        thunks.font_get_face_names = FONT_GET_FACE_NAMES_THUNK;
        thunks.font_get_simulations = FONT_GET_SIMULATIONS_THUNK;
        thunks.font_get_metrics = FONT_GET_METRICS_THUNK;
        thunks.font_create_font_face = FONT_CREATE_FONT_FACE_THUNK;
        thunks.face_get_type = FACE_GET_TYPE_THUNK;
        thunks.face_get_files = FACE_GET_FILES_THUNK;
        thunks.face_get_index = FACE_GET_INDEX_THUNK;
        thunks.face_get_simulations = FACE_GET_SIMULATIONS_THUNK;
        thunks.face_is_symbol_font = FACE_IS_SYMBOL_THUNK;
        thunks.face_get_metrics = FACE_GET_METRICS_THUNK;
        thunks.face_get_glyph_count = FACE_GET_GLYPH_COUNT_THUNK;
        thunks.face_get_design_glyph_metrics =
            FACE_GET_DESIGN_GLYPH_METRICS_THUNK;
        thunks.face_get_gdi_compatible_glyph_metrics =
            FACE_GET_GDI_COMPATIBLE_GLYPH_METRICS_THUNK;
        thunks.face_get_glyph_indices = FACE_GET_GLYPH_INDICES_THUNK;
        thunks.face_try_get_font_table = FACE_TRY_GET_FONT_TABLE_THUNK;
        thunks.face_release_font_table = FACE_RELEASE_FONT_TABLE_THUNK;
        thunks.face_get_glyph_run_outline = FACE_GET_GLYPH_RUN_OUTLINE_THUNK;
        thunks.face_get_metrics1 = FACE_GET_METRICS1_THUNK;
        thunks.face_get_caret_metrics = FACE_GET_CARET_METRICS_THUNK;
        thunks.face_get_unicode_ranges = FACE_GET_UNICODE_RANGES_THUNK;
        thunks.face_get_design_glyph_advances =
            FACE_GET_DESIGN_GLYPH_ADVANCES_THUNK;
        thunks.face_get_kerning_pair_adjustments =
            FACE_GET_KERNING_PAIR_ADJUSTMENTS_THUNK;
        thunks.face_get_recommended_rendering_mode =
            FACE_GET_RECOMMENDED_RENDERING_MODE_THUNK;
        thunks.face_get_recommended_rendering_mode1 =
            FACE_GET_RECOMMENDED_RENDERING_MODE1_THUNK;
        thunks.face_get_recommended_rendering_mode2 =
            FACE_GET_RECOMMENDED_RENDERING_MODE2_THUNK;
        thunks.face_get_weight = FACE_GET_WEIGHT_THUNK;
        thunks.face_get_stretch = FACE_GET_STRETCH_THUNK;
        thunks.face_get_style = FACE_GET_STYLE_THUNK;
        thunks.face_get_panose = FACE_GET_PANOSE_THUNK;
        thunks.face_get_family_names = FACE_GET_FAMILY_NAMES_THUNK;
        thunks.face_get_face_names = FACE_GET_FACE_NAMES_THUNK;
        thunks.face_get_family_names_locale =
            FACE_GET_FAMILY_NAMES_LOCALE_THUNK;
        thunks.face_get_face_names_locale = FACE_GET_FACE_NAMES_LOCALE_THUNK;
        thunks.face_get_informational_strings =
            FACE_GET_INFORMATIONAL_STRINGS_THUNK;
        thunks.face_has_character = FACE_HAS_CHARACTER_THUNK;
        thunks.face_is_local = FACE_IS_LOCAL_THUNK;
        thunks.face_are_local = FACE_ARE_LOCAL_THUNK;
        thunks.face_equals = FACE_EQUALS_THUNK;
        thunks.strings_get_count = STRINGS_GET_COUNT_THUNK;
        thunks.strings_find_locale_name = STRINGS_FIND_LOCALE_NAME_THUNK;
        thunks.strings_get_locale_name_length =
            STRINGS_GET_LOCALE_NAME_LENGTH_THUNK;
        thunks.strings_get_locale_name = STRINGS_GET_LOCALE_NAME_THUNK;
        thunks.strings_get_string_length = STRINGS_GET_STRING_LENGTH_THUNK;
        thunks.strings_get_string = STRINGS_GET_STRING_THUNK;
        thunks.file_get_reference_key = FILE_GET_REFERENCE_KEY_THUNK;
        thunks.file_get_loader = FILE_GET_LOADER_THUNK;
        thunks.file_analyze = FILE_ANALYZE_THUNK;
        thunks.local_loader_get_path_length = LOCAL_GET_PATH_LENGTH_THUNK;
        thunks.local_loader_get_path = LOCAL_GET_PATH_THUNK;
        thunks.local_loader_get_last_write_time = LOCAL_GET_TIME_THUNK;
        thunks.local_loader_create_stream_from_key =
            LOCAL_CREATE_STREAM_THUNK;
        thunks.stream_read_file_fragment = STREAM_READ_FRAGMENT_THUNK;
        thunks.stream_release_file_fragment = STREAM_RELEASE_FRAGMENT_THUNK;
        thunks.stream_get_file_size = STREAM_GET_FILE_SIZE_THUNK;
        thunks.stream_get_last_write_time = STREAM_GET_LAST_WRITE_TIME_THUNK;
        thunks.rendering_params_get_gamma = RENDERING_PARAMS_GET_GAMMA_THUNK;
        thunks.rendering_params_get_enhanced_contrast =
            RENDERING_PARAMS_GET_ENHANCED_CONTRAST_THUNK;
        thunks.rendering_params_get_clear_type_level =
            RENDERING_PARAMS_GET_CLEAR_TYPE_LEVEL_THUNK;
        thunks.rendering_params_get_pixel_geometry =
            RENDERING_PARAMS_GET_PIXEL_GEOMETRY_THUNK;
        thunks.rendering_params_get_rendering_mode =
            RENDERING_PARAMS_GET_RENDERING_MODE_THUNK;
        for (u32 count = 0; count < thunks.not_impl_by_arg_count.size(); ++count) {
            thunks.not_impl_by_arg_count[count] =
                NOTIMPL_THUNK_BASE + count * 0x10;
        }
        for (u32 count = 0; count < thunks.return_zero_by_arg_count.size();
             ++count) {
            thunks.return_zero_by_arg_count[count] =
                RETURN_ZERO_THUNK_BASE + count * 0x10;
        }
        return thunks;
    }

    static constexpr GuestAddr DATA_BASE = 0x1000;
    static constexpr GuestAddr IID_ADDRESS = DATA_BASE + 0x100;
    static constexpr GuestAddr OUTPUT_ADDRESS = DATA_BASE + 0x200;
    static constexpr GuestAddr QUERY_OUTPUT_ADDRESS = DATA_BASE + 0x208;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr QUERY_THUNK = 0x70000010;
    static constexpr GuestAddr ADDREF_THUNK = 0x70000020;
    static constexpr GuestAddr RELEASE_THUNK = 0x70000030;
    static constexpr GuestAddr NOTIMPL_THUNK_BASE = 0x70000100;
    static constexpr GuestAddr GET_COLLECTION_THUNK = 0x70000050;
    static constexpr GuestAddr CREATE_CUSTOM_COLLECTION_THUNK = 0x70000350;
    static constexpr GuestAddr REGISTER_COLLECTION_LOADER_THUNK = 0x70000360;
    static constexpr GuestAddr UNREGISTER_COLLECTION_LOADER_THUNK = 0x70000370;
    static constexpr GuestAddr CREATE_FONT_FILE_REFERENCE_THUNK = 0x70000380;
    static constexpr GuestAddr CREATE_CUSTOM_FONT_FILE_REFERENCE_THUNK =
        0x70000440;
    static constexpr GuestAddr CREATE_RENDERING_PARAMS_THUNK = 0x70000540;
    static constexpr GuestAddr CREATE_MONITOR_RENDERING_PARAMS_THUNK =
        0x70000550;
    static constexpr GuestAddr CREATE_TYPOGRAPHY_THUNK = 0x700005B0;
    static constexpr GuestAddr CREATE_TEXT_ANALYZER_THUNK = 0x700005C0;
    static constexpr GuestAddr TYPOGRAPHY_ADD_FONT_FEATURE_THUNK = 0x700005D0;
    static constexpr GuestAddr TYPOGRAPHY_GET_FONT_FEATURE_COUNT_THUNK =
        0x700005E0;
    static constexpr GuestAddr TYPOGRAPHY_GET_FONT_FEATURE_THUNK = 0x700005F0;
    static constexpr GuestAddr ANALYZER_ANALYZE_SCRIPT_THUNK = 0x70000600;
    static constexpr GuestAddr ANALYZER_ANALYZE_BIDI_THUNK = 0x70000610;
    static constexpr GuestAddr ANALYZER_ANALYZE_NUMBER_SUBSTITUTION_THUNK =
        0x70000620;
    static constexpr GuestAddr ANALYZER_ANALYZE_LINE_BREAKPOINTS_THUNK =
        0x70000630;
    static constexpr GuestAddr ANALYZER_GET_GLYPHS_THUNK = 0x70000640;
    static constexpr GuestAddr ANALYZER_GET_GLYPH_PLACEMENTS_THUNK =
        0x70000650;
    static constexpr GuestAddr ANALYZER_GET_GDI_COMPATIBLE_GLYPH_PLACEMENTS_THUNK =
        0x70000660;
    static constexpr GuestAddr REGISTER_FILE_LOADER_THUNK = 0x70000390;
    static constexpr GuestAddr UNREGISTER_FILE_LOADER_THUNK = 0x700003A0;
    static constexpr GuestAddr GET_GDI_INTEROP_THUNK = 0x70000460;
    static constexpr GuestAddr GDI_CREATE_FONT_FACE_FROM_HDC_THUNK =
        0x70000470;
    static constexpr GuestAddr GDI_CREATE_FONT_FROM_LOGFONT_THUNK = 0x700008B0;
    static constexpr GuestAddr GDI_CONVERT_FONT_TO_LOGFONT_THUNK = 0x700008C0;
    static constexpr GuestAddr GDI_CONVERT_FONT_FACE_TO_LOGFONT_THUNK =
        0x700008D0;
    static constexpr GuestAddr CREATE_GLYPH_RUN_ANALYSIS_THUNK = 0x70000490;
    static constexpr GuestAddr CREATE_GLYPH_RUN_ANALYSIS2_THUNK = 0x70000494;
    static constexpr GuestAddr ANALYSIS_GET_ALPHA_TEXTURE_BOUNDS_THUNK =
        0x700004A0;
    static constexpr GuestAddr ANALYSIS_CREATE_ALPHA_TEXTURE_THUNK =
        0x700004B0;
    static constexpr GuestAddr ANALYSIS_GET_ALPHA_BLEND_PARAMS_THUNK =
        0x700008A0;
    static constexpr GuestAddr GET_FALLBACK_THUNK = 0x70000290;
    static constexpr GuestAddr CREATE_FALLBACK_BUILDER_THUNK = 0x70000670;
    static constexpr GuestAddr GET_SYSTEM_FONT_SET_THUNK = 0x70000680;
    static constexpr GuestAddr GET_FONT_DOWNLOAD_QUEUE_THUNK = 0x70000690;
    static constexpr GuestAddr FONT_SET_GET_FONT_COUNT_THUNK = 0x700006A0;
    static constexpr GuestAddr FONT_SET_GET_MATCHING_FONTS_FAMILY_THUNK =
        0x700006B0;
    static constexpr GuestAddr FONT_SET_GET_MATCHING_FONTS_PROPERTIES_THUNK =
        0x700006C0;
    static constexpr GuestAddr FALLBACK_BUILDER_ADD_MAPPING_THUNK = 0x700006D0;
    static constexpr GuestAddr FALLBACK_BUILDER_ADD_MAPPINGS_THUNK =
        0x700006E0;
    static constexpr GuestAddr FALLBACK_BUILDER_CREATE_FONT_FALLBACK_THUNK =
        0x700006F0;
    static constexpr GuestAddr DOWNLOAD_QUEUE_IS_EMPTY_THUNK = 0x70000700;
    static constexpr GuestAddr DOWNLOAD_QUEUE_BEGIN_DOWNLOAD_THUNK =
        0x70000710;
    static constexpr GuestAddr DOWNLOAD_QUEUE_GET_GENERATION_COUNT_THUNK =
        0x70000720;
    static constexpr GuestAddr MAP_CHARACTERS_THUNK = 0x700002A0;
    static constexpr GuestAddr CREATE_NUMBER_SUBSTITUTION_THUNK = 0x700002B0;
    static constexpr GuestAddr GET_COUNT_THUNK = 0x70000060;
    static constexpr GuestAddr GET_FAMILY_THUNK = 0x70000070;
    static constexpr GuestAddr FIND_FAMILY_THUNK = 0x70000080;
    static constexpr GuestAddr FAMILY_GET_FONT_COUNT_THUNK = 0x700003B0;
    static constexpr GuestAddr FAMILY_GET_FONT_THUNK = 0x700003C0;
    static constexpr GuestAddr FAMILY_GET_NAMES_THUNK = 0x700002E0;
    static constexpr GuestAddr GET_MATCHING_FONT_THUNK = 0x70000090;
    static constexpr GuestAddr FONT_GET_FAMILY_THUNK = 0x700000A0;
    static constexpr GuestAddr FONT_GET_WEIGHT_THUNK = 0x700000B0;
    static constexpr GuestAddr FONT_GET_STRETCH_THUNK = 0x700000C0;
    static constexpr GuestAddr FONT_GET_STYLE_THUNK = 0x700000D0;
    static constexpr GuestAddr FONT_IS_SYMBOL_THUNK = 0x700000E0;
    static constexpr GuestAddr FONT_GET_FACE_NAMES_THUNK = 0x70000450;
    static constexpr GuestAddr FONT_GET_SIMULATIONS_THUNK = 0x700000F0;
    static constexpr GuestAddr FONT_GET_METRICS_THUNK = 0x70000200;
    static constexpr GuestAddr FONT_CREATE_FONT_FACE_THUNK = 0x70000210;
    static constexpr GuestAddr FACE_GET_TYPE_THUNK = 0x70000220;
    static constexpr GuestAddr FACE_GET_FILES_THUNK = 0x700003D0;
    static constexpr GuestAddr FACE_GET_INDEX_THUNK = 0x70000230;
    static constexpr GuestAddr FACE_GET_SIMULATIONS_THUNK = 0x70000240;
    static constexpr GuestAddr FACE_IS_SYMBOL_THUNK = 0x70000250;
    static constexpr GuestAddr FACE_GET_METRICS_THUNK = 0x70000260;
    static constexpr GuestAddr FACE_GET_GLYPH_COUNT_THUNK = 0x70000270;
    static constexpr GuestAddr FACE_GET_DESIGN_GLYPH_METRICS_THUNK =
        0x70000480;
    static constexpr GuestAddr FACE_GET_GDI_COMPATIBLE_GLYPH_METRICS_THUNK =
        0x70000890;
    static constexpr GuestAddr FACE_GET_GLYPH_INDICES_THUNK = 0x70000280;
    static constexpr GuestAddr FACE_TRY_GET_FONT_TABLE_THUNK = 0x700002C0;
    static constexpr GuestAddr FACE_RELEASE_FONT_TABLE_THUNK = 0x700002D0;
    static constexpr GuestAddr FACE_GET_GLYPH_RUN_OUTLINE_THUNK = 0x70000530;
    static constexpr GuestAddr FACE_GET_METRICS1_THUNK = 0x70000700;
    static constexpr GuestAddr FACE_GET_CARET_METRICS_THUNK = 0x70000710;
    static constexpr GuestAddr FACE_GET_UNICODE_RANGES_THUNK = 0x70000720;
    static constexpr GuestAddr FACE_GET_DESIGN_GLYPH_ADVANCES_THUNK =
        0x70000730;
    static constexpr GuestAddr FACE_GET_KERNING_PAIR_ADJUSTMENTS_THUNK =
        0x70000740;
    static constexpr GuestAddr FACE_GET_RECOMMENDED_RENDERING_MODE_THUNK =
        0x70000750;
    static constexpr GuestAddr FACE_GET_RECOMMENDED_RENDERING_MODE1_THUNK =
        0x70000760;
    static constexpr GuestAddr FACE_GET_RECOMMENDED_RENDERING_MODE2_THUNK =
        0x70000770;
    static constexpr GuestAddr FACE_GET_WEIGHT_THUNK = 0x70000780;
    static constexpr GuestAddr FACE_GET_STRETCH_THUNK = 0x70000790;
    static constexpr GuestAddr FACE_GET_STYLE_THUNK = 0x700007A0;
    static constexpr GuestAddr FACE_GET_PANOSE_THUNK = 0x700007B0;
    static constexpr GuestAddr FACE_GET_FAMILY_NAMES_THUNK = 0x700007C0;
    static constexpr GuestAddr FACE_GET_FACE_NAMES_THUNK = 0x700007D0;
    static constexpr GuestAddr FACE_GET_FAMILY_NAMES_LOCALE_THUNK = 0x700007E0;
    static constexpr GuestAddr FACE_GET_FACE_NAMES_LOCALE_THUNK = 0x700007F0;
    static constexpr GuestAddr FACE_GET_INFORMATIONAL_STRINGS_THUNK =
        0x70000800;
    static constexpr GuestAddr FACE_HAS_CHARACTER_THUNK = 0x70000810;
    static constexpr GuestAddr FACE_IS_LOCAL_THUNK = 0x70000820;
    static constexpr GuestAddr FACE_ARE_LOCAL_THUNK = 0x70000830;
    static constexpr GuestAddr FACE_EQUALS_THUNK = 0x70000840;
    static constexpr GuestAddr RETURN_ZERO_THUNK_BASE = 0x70000850;
    static constexpr GuestAddr STRINGS_GET_COUNT_THUNK = 0x700002F0;
    static constexpr GuestAddr STRINGS_FIND_LOCALE_NAME_THUNK = 0x70000300;
    static constexpr GuestAddr STRINGS_GET_LOCALE_NAME_LENGTH_THUNK = 0x70000310;
    static constexpr GuestAddr STRINGS_GET_LOCALE_NAME_THUNK = 0x70000320;
    static constexpr GuestAddr STRINGS_GET_STRING_LENGTH_THUNK = 0x70000330;
    static constexpr GuestAddr STRINGS_GET_STRING_THUNK = 0x70000340;
    static constexpr GuestAddr FILE_GET_REFERENCE_KEY_THUNK = 0x700003E0;
    static constexpr GuestAddr FILE_GET_LOADER_THUNK = 0x700003F0;
    static constexpr GuestAddr FILE_ANALYZE_THUNK = 0x70000400;
    static constexpr GuestAddr LOCAL_GET_PATH_LENGTH_THUNK = 0x70000410;
    static constexpr GuestAddr LOCAL_GET_PATH_THUNK = 0x70000420;
    static constexpr GuestAddr LOCAL_GET_TIME_THUNK = 0x70000430;
    static constexpr GuestAddr LOCAL_CREATE_STREAM_THUNK = 0x700004E0;
    static constexpr GuestAddr STREAM_READ_FRAGMENT_THUNK = 0x700004F0;
    static constexpr GuestAddr STREAM_RELEASE_FRAGMENT_THUNK = 0x70000500;
    static constexpr GuestAddr STREAM_GET_FILE_SIZE_THUNK = 0x70000510;
    static constexpr GuestAddr STREAM_GET_LAST_WRITE_TIME_THUNK = 0x70000520;
    static constexpr GuestAddr RENDERING_PARAMS_GET_GAMMA_THUNK = 0x70000560;
    static constexpr GuestAddr RENDERING_PARAMS_GET_ENHANCED_CONTRAST_THUNK =
        0x70000570;
    static constexpr GuestAddr RENDERING_PARAMS_GET_CLEAR_TYPE_LEVEL_THUNK =
        0x70000580;
    static constexpr GuestAddr RENDERING_PARAMS_GET_PIXEL_GEOMETRY_THUNK =
        0x70000590;
    static constexpr GuestAddr RENDERING_PARAMS_GET_RENDERING_MODE_THUNK =
        0x700005A0;

    GuestMemory memory;
    cpu::CpuState cpu;
    HandleTable handles;
    GdiManager gdi{handles};
    capsule::Vfs vfs;
    DWriteManager manager{memory, true, &vfs, &gdi};
};

}  // namespace vitreo::winapi::dwrite_test

// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: Direct2D, dwrite.dll and DWriteCore.dll

#include "dll_registry_internal.hpp"

#include <cstdio>
#include <string>
#include <utility>

namespace vitreo::cli {

void register_dwrite_dlls(RuntimeContext& ctx) {
    auto& reg = ctx.thunk_registry;

    ctx.d2d1_mgr.set_dpi_provider([&ctx] {
        const float dpi = static_cast<float>(ctx.u32_ext_mgr.system_dpi());
        return std::pair<float, float>{dpi, dpi};
    });
    ctx.d2d1_mgr.set_dwrite(&ctx.dwrite_mgr);
    const auto d2d_registered = reg.register_dll(
        "d2d1.dll", winapi::create_d2d1_thunks(ctx.d2d1_mgr));
    if (d2d_registered.is_err() || !ctx.d2d1_mgr.bind_thunks(reg)) {
        std::fprintf(stderr, "[vitreo] Direct2D thunk binding failed\n");
    }

    // -- DWrite.dll --
    {
        (void)reg.register_dll("dwrite.dll", winapi::create_dwrite_thunks(ctx.dwrite_mgr));
        auto query_interface = reg.get_thunk_address("dwrite.dll", "_DWrite_QueryInterface");
        auto add_ref = reg.get_thunk_address("dwrite.dll", "_DWrite_AddRef");
        auto release = reg.get_thunk_address("dwrite.dll", "_DWrite_Release");
        auto get_system_font_collection = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GetSystemFontCollection");
        auto create_custom_font_collection = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateCustomFontCollection");
        auto register_font_collection_loader = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_RegisterFontCollectionLoader");
        auto unregister_font_collection_loader = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_UnregisterFontCollectionLoader");
        auto create_font_file_reference = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateFontFileReference");
        auto create_custom_font_file_reference = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateCustomFontFileReference");
        auto register_font_file_loader = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_RegisterFontFileLoader");
        auto unregister_font_file_loader = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_UnregisterFontFileLoader");
        auto get_gdi_interop = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GetGdiInterop");
        auto create_rendering_params = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateRenderingParams");
        auto create_monitor_rendering_params = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateMonitorRenderingParams");
        auto create_typography = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateTypography");
        auto create_text_analyzer = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateTextAnalyzer");
        auto typography_add_font_feature = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_TypographyAddFontFeature");
        auto typography_get_font_feature_count = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_TypographyGetFontFeatureCount");
        auto typography_get_font_feature = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_TypographyGetFontFeature");
        auto analyzer_analyze_script = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalyzerAnalyzeScript");
        auto analyzer_analyze_bidi = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalyzerAnalyzeBidi");
        auto analyzer_analyze_number_substitution = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalyzerAnalyzeNumberSubstitution");
        auto analyzer_analyze_line_breakpoints = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalyzerAnalyzeLineBreakpoints");
        auto analyzer_get_glyphs = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalyzerGetGlyphs");
        auto analyzer_get_glyph_placements = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalyzerGetGlyphPlacements");
        auto analyzer_get_gdi_compatible_glyph_placements =
            reg.get_thunk_address(
                "dwrite.dll",
                "_DWrite_AnalyzerGetGdiCompatibleGlyphPlacements");
        auto rendering_params_get_gamma = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_RenderingParamsGetGamma");
        auto rendering_params_get_enhanced_contrast = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_RenderingParamsGetEnhancedContrast");
        auto rendering_params_get_clear_type_level = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_RenderingParamsGetClearTypeLevel");
        auto rendering_params_get_pixel_geometry = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_RenderingParamsGetPixelGeometry");
        auto rendering_params_get_rendering_mode = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_RenderingParamsGetRenderingMode");
        auto gdi_create_font_from_logfont = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GdiCreateFontFromLOGFONT");
        auto gdi_convert_font_to_logfont = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GdiConvertFontToLOGFONT");
        auto gdi_convert_font_face_to_logfont = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GdiConvertFontFaceToLOGFONT");
        auto gdi_create_font_face_from_hdc = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GdiCreateFontFaceFromHdc");
        auto create_glyph_run_analysis = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateGlyphRunAnalysis");
        auto create_glyph_run_analysis2 = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateGlyphRunAnalysis2");
        auto analysis_get_alpha_texture_bounds = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalysisGetAlphaTextureBounds");
        auto analysis_create_alpha_texture = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalysisCreateAlphaTexture");
        auto analysis_get_alpha_blend_params = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_AnalysisGetAlphaBlendParams");
        auto get_system_font_fallback = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GetSystemFontFallback");
        auto create_font_fallback_builder = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateFontFallbackBuilder");
        auto get_system_font_set = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GetSystemFontSet");
        auto get_font_download_queue = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GetFontDownloadQueue");
        auto font_set_get_font_count = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontSetGetFontCount");
        auto font_set_get_matching_fonts_family = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontSetGetMatchingFontsFamily");
        auto font_set_get_matching_fonts_properties = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontSetGetMatchingFontsProperties");
        auto fallback_builder_add_mapping = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FallbackBuilderAddMapping");
        auto fallback_builder_add_mappings = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FallbackBuilderAddMappings");
        auto fallback_builder_create_font_fallback = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FallbackBuilderCreateFontFallback");
        auto download_queue_is_empty = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_DownloadQueueIsEmpty");
        auto download_queue_begin_download = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_DownloadQueueBeginDownload");
        auto download_queue_get_generation_count = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_DownloadQueueGetGenerationCount");
        auto fallback_map_characters = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FallbackMapCharacters");
        auto create_number_substitution = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_CreateNumberSubstitution");
        auto get_font_family_count = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GetFontFamilyCount");
        auto get_font_family = reg.get_thunk_address("dwrite.dll", "_DWrite_GetFontFamily");
        auto find_family_name = reg.get_thunk_address("dwrite.dll", "_DWrite_FindFamilyName");
        auto family_get_names = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FamilyGetNames");
        auto family_get_font_count = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FamilyGetFontCount");
        auto family_get_font = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FamilyGetFont");
        const auto address_or_zero = [](const auto& result) {
            return result.is_ok() ? result.value() : GuestAddr{0};
        };
        winapi::DWriteManager::MethodThunks method_thunks{};
        method_thunks.query_interface = address_or_zero(query_interface);
        method_thunks.add_ref = address_or_zero(add_ref);
        method_thunks.release = address_or_zero(release);
        method_thunks.get_system_font_collection =
            address_or_zero(get_system_font_collection);
        method_thunks.create_custom_font_collection =
            address_or_zero(create_custom_font_collection);
        method_thunks.register_font_collection_loader =
            address_or_zero(register_font_collection_loader);
        method_thunks.unregister_font_collection_loader =
            address_or_zero(unregister_font_collection_loader);
        method_thunks.create_font_file_reference =
            address_or_zero(create_font_file_reference);
        method_thunks.create_custom_font_file_reference =
            address_or_zero(create_custom_font_file_reference);
        method_thunks.register_font_file_loader =
            address_or_zero(register_font_file_loader);
        method_thunks.unregister_font_file_loader =
            address_or_zero(unregister_font_file_loader);
        method_thunks.get_gdi_interop = address_or_zero(get_gdi_interop);
        method_thunks.create_rendering_params =
            address_or_zero(create_rendering_params);
        method_thunks.create_monitor_rendering_params =
            address_or_zero(create_monitor_rendering_params);
        method_thunks.create_typography = address_or_zero(create_typography);
        method_thunks.create_text_analyzer =
            address_or_zero(create_text_analyzer);
        method_thunks.typography_add_font_feature =
            address_or_zero(typography_add_font_feature);
        method_thunks.typography_get_font_feature_count =
            address_or_zero(typography_get_font_feature_count);
        method_thunks.typography_get_font_feature =
            address_or_zero(typography_get_font_feature);
        method_thunks.analyzer_analyze_script =
            address_or_zero(analyzer_analyze_script);
        method_thunks.analyzer_analyze_bidi =
            address_or_zero(analyzer_analyze_bidi);
        method_thunks.analyzer_analyze_number_substitution =
            address_or_zero(analyzer_analyze_number_substitution);
        method_thunks.analyzer_analyze_line_breakpoints =
            address_or_zero(analyzer_analyze_line_breakpoints);
        method_thunks.analyzer_get_glyphs =
            address_or_zero(analyzer_get_glyphs);
        method_thunks.analyzer_get_glyph_placements =
            address_or_zero(analyzer_get_glyph_placements);
        method_thunks.analyzer_get_gdi_compatible_glyph_placements =
            address_or_zero(analyzer_get_gdi_compatible_glyph_placements);
        method_thunks.rendering_params_get_gamma =
            address_or_zero(rendering_params_get_gamma);
        method_thunks.rendering_params_get_enhanced_contrast =
            address_or_zero(rendering_params_get_enhanced_contrast);
        method_thunks.rendering_params_get_clear_type_level =
            address_or_zero(rendering_params_get_clear_type_level);
        method_thunks.rendering_params_get_pixel_geometry =
            address_or_zero(rendering_params_get_pixel_geometry);
        method_thunks.rendering_params_get_rendering_mode =
            address_or_zero(rendering_params_get_rendering_mode);
        method_thunks.gdi_create_font_from_logfont =
            address_or_zero(gdi_create_font_from_logfont);
        method_thunks.gdi_convert_font_to_logfont =
            address_or_zero(gdi_convert_font_to_logfont);
        method_thunks.gdi_convert_font_face_to_logfont =
            address_or_zero(gdi_convert_font_face_to_logfont);
        method_thunks.gdi_create_font_face_from_hdc =
            address_or_zero(gdi_create_font_face_from_hdc);
        method_thunks.create_glyph_run_analysis =
            address_or_zero(create_glyph_run_analysis);
        method_thunks.create_glyph_run_analysis2 =
            address_or_zero(create_glyph_run_analysis2);
        method_thunks.analysis_get_alpha_texture_bounds =
            address_or_zero(analysis_get_alpha_texture_bounds);
        method_thunks.analysis_create_alpha_texture =
            address_or_zero(analysis_create_alpha_texture);
        method_thunks.analysis_get_alpha_blend_params =
            address_or_zero(analysis_get_alpha_blend_params);
        method_thunks.get_system_font_fallback =
            address_or_zero(get_system_font_fallback);
        method_thunks.create_font_fallback_builder =
            address_or_zero(create_font_fallback_builder);
        method_thunks.get_system_font_set =
            address_or_zero(get_system_font_set);
        method_thunks.get_font_download_queue =
            address_or_zero(get_font_download_queue);
        method_thunks.font_set_get_font_count =
            address_or_zero(font_set_get_font_count);
        method_thunks.font_set_get_matching_fonts_family =
            address_or_zero(font_set_get_matching_fonts_family);
        method_thunks.font_set_get_matching_fonts_properties =
            address_or_zero(font_set_get_matching_fonts_properties);
        method_thunks.fallback_builder_add_mapping =
            address_or_zero(fallback_builder_add_mapping);
        method_thunks.fallback_builder_add_mappings =
            address_or_zero(fallback_builder_add_mappings);
        method_thunks.fallback_builder_create_font_fallback =
            address_or_zero(fallback_builder_create_font_fallback);
        method_thunks.download_queue_is_empty =
            address_or_zero(download_queue_is_empty);
        method_thunks.download_queue_begin_download =
            address_or_zero(download_queue_begin_download);
        method_thunks.download_queue_get_generation_count =
            address_or_zero(download_queue_get_generation_count);
        method_thunks.fallback_map_characters =
            address_or_zero(fallback_map_characters);
        method_thunks.create_number_substitution =
            address_or_zero(create_number_substitution);
        method_thunks.get_font_family_count =
            address_or_zero(get_font_family_count);
        method_thunks.get_font_family = address_or_zero(get_font_family);
        method_thunks.find_family_name = address_or_zero(find_family_name);
        method_thunks.family_get_names = address_or_zero(family_get_names);
        method_thunks.family_get_font_count =
            address_or_zero(family_get_font_count);
        method_thunks.family_get_font = address_or_zero(family_get_font);
        const auto get_first_matching_font = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_GetFirstMatchingFont");
        const auto font_get_family = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontGetFamily");
        const auto font_get_weight = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontGetWeight");
        const auto font_get_stretch = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontGetStretch");
        const auto font_get_style = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontGetStyle");
        const auto font_is_symbol = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontIsSymbolFont");
        const auto font_get_face_names = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontGetFaceNames");
        const auto font_get_simulations = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontGetSimulations");
        const auto font_get_metrics = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontGetMetrics");
        const auto font_create_font_face = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FontCreateFontFace");
        const auto face_get_type = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetType");
        const auto face_get_files = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetFiles");
        const auto face_get_index = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetIndex");
        const auto face_get_simulations = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetSimulations");
        const auto face_is_symbol = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceIsSymbolFont");
        const auto face_get_metrics = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetMetrics");
        const auto face_get_glyph_count = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetGlyphCount");
        const auto face_get_design_glyph_metrics = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetDesignGlyphMetrics");
        const auto face_get_gdi_compatible_glyph_metrics =
            reg.get_thunk_address(
                "dwrite.dll", "_DWrite_FaceGetGdiCompatibleGlyphMetrics");
        const auto face_get_glyph_indices = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetGlyphIndices");
        const auto face_try_get_font_table = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceTryGetFontTable");
        const auto face_release_font_table = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceReleaseFontTable");
        const auto face_get_glyph_run_outline = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetGlyphRunOutline");
        const auto face_get_metrics1 = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetMetrics1");
        const auto face_get_caret_metrics = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetCaretMetrics");
        const auto face_get_unicode_ranges = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetUnicodeRanges");
        const auto face_get_design_glyph_advances = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetDesignGlyphAdvances");
        const auto face_get_kerning_pair_adjustments = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetKerningPairAdjustments");
        const auto face_get_recommended_rendering_mode = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetRecommendedRenderingMode");
        const auto face_get_recommended_rendering_mode1 =
            reg.get_thunk_address(
                "dwrite.dll", "_DWrite_FaceGetRecommendedRenderingMode1");
        const auto face_get_recommended_rendering_mode2 =
            reg.get_thunk_address(
                "dwrite.dll", "_DWrite_FaceGetRecommendedRenderingMode2");
        const auto face_get_weight = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetWeight");
        const auto face_get_stretch = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetStretch");
        const auto face_get_style = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetStyle");
        const auto face_get_panose = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetPanose");
        const auto face_get_family_names = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetFamilyNames");
        const auto face_get_face_names = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetFaceNames");
        const auto face_get_family_names_locale = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetFamilyNamesLocale");
        const auto face_get_face_names_locale = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetFaceNamesLocale");
        const auto face_get_informational_strings = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceGetInformationalStrings");
        const auto face_has_character = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceHasCharacter");
        const auto face_is_local = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceIsLocal");
        const auto face_are_local = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceAreLocal");
        const auto face_equals = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FaceEquals");
        const auto strings_get_count = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StringsGetCount");
        const auto strings_find_locale_name = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StringsFindLocaleName");
        const auto strings_get_locale_name_length = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StringsGetLocaleNameLength");
        const auto strings_get_locale_name = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StringsGetLocaleName");
        const auto strings_get_string_length = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StringsGetStringLength");
        const auto strings_get_string = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StringsGetString");
        const auto file_get_reference_key = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FileGetReferenceKey");
        const auto file_get_loader = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FileGetLoader");
        const auto file_analyze = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_FileAnalyze");
        const auto local_loader_get_path_length = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_LocalLoaderGetPathLength");
        const auto local_loader_get_path = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_LocalLoaderGetPath");
        const auto local_loader_get_last_write_time = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_LocalLoaderGetLastWriteTime");
        const auto local_loader_create_stream_from_key = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_LocalLoaderCreateStreamFromKey");
        const auto stream_read_file_fragment = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StreamReadFileFragment");
        const auto stream_release_file_fragment = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StreamReleaseFileFragment");
        const auto stream_get_file_size = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StreamGetFileSize");
        const auto stream_get_last_write_time = reg.get_thunk_address(
            "dwrite.dll", "_DWrite_StreamGetLastWriteTime");
        method_thunks.get_first_matching_font =
            address_or_zero(get_first_matching_font);
        method_thunks.font_get_family = address_or_zero(font_get_family);
        method_thunks.font_get_weight = address_or_zero(font_get_weight);
        method_thunks.font_get_stretch = address_or_zero(font_get_stretch);
        method_thunks.font_get_style = address_or_zero(font_get_style);
        method_thunks.font_is_symbol_font = address_or_zero(font_is_symbol);
        method_thunks.font_get_face_names =
            address_or_zero(font_get_face_names);
        method_thunks.font_get_simulations =
            address_or_zero(font_get_simulations);
        method_thunks.font_get_metrics = address_or_zero(font_get_metrics);
        method_thunks.font_create_font_face =
            address_or_zero(font_create_font_face);
        method_thunks.face_get_type = address_or_zero(face_get_type);
        method_thunks.face_get_files = address_or_zero(face_get_files);
        method_thunks.face_get_index = address_or_zero(face_get_index);
        method_thunks.face_get_simulations =
            address_or_zero(face_get_simulations);
        method_thunks.face_is_symbol_font = address_or_zero(face_is_symbol);
        method_thunks.face_get_metrics = address_or_zero(face_get_metrics);
        method_thunks.face_get_glyph_count =
            address_or_zero(face_get_glyph_count);
        method_thunks.face_get_design_glyph_metrics =
            address_or_zero(face_get_design_glyph_metrics);
        method_thunks.face_get_gdi_compatible_glyph_metrics =
            address_or_zero(face_get_gdi_compatible_glyph_metrics);
        method_thunks.face_get_glyph_indices =
            address_or_zero(face_get_glyph_indices);
        method_thunks.face_try_get_font_table =
            address_or_zero(face_try_get_font_table);
        method_thunks.face_release_font_table =
            address_or_zero(face_release_font_table);
        method_thunks.face_get_glyph_run_outline =
            address_or_zero(face_get_glyph_run_outline);
        method_thunks.face_get_metrics1 = address_or_zero(face_get_metrics1);
        method_thunks.face_get_caret_metrics =
            address_or_zero(face_get_caret_metrics);
        method_thunks.face_get_unicode_ranges =
            address_or_zero(face_get_unicode_ranges);
        method_thunks.face_get_design_glyph_advances =
            address_or_zero(face_get_design_glyph_advances);
        method_thunks.face_get_kerning_pair_adjustments =
            address_or_zero(face_get_kerning_pair_adjustments);
        method_thunks.face_get_recommended_rendering_mode =
            address_or_zero(face_get_recommended_rendering_mode);
        method_thunks.face_get_recommended_rendering_mode1 =
            address_or_zero(face_get_recommended_rendering_mode1);
        method_thunks.face_get_recommended_rendering_mode2 =
            address_or_zero(face_get_recommended_rendering_mode2);
        method_thunks.face_get_weight = address_or_zero(face_get_weight);
        method_thunks.face_get_stretch = address_or_zero(face_get_stretch);
        method_thunks.face_get_style = address_or_zero(face_get_style);
        method_thunks.face_get_panose = address_or_zero(face_get_panose);
        method_thunks.face_get_family_names =
            address_or_zero(face_get_family_names);
        method_thunks.face_get_face_names =
            address_or_zero(face_get_face_names);
        method_thunks.face_get_family_names_locale =
            address_or_zero(face_get_family_names_locale);
        method_thunks.face_get_face_names_locale =
            address_or_zero(face_get_face_names_locale);
        method_thunks.face_get_informational_strings =
            address_or_zero(face_get_informational_strings);
        method_thunks.face_has_character =
            address_or_zero(face_has_character);
        method_thunks.face_is_local = address_or_zero(face_is_local);
        method_thunks.face_are_local = address_or_zero(face_are_local);
        method_thunks.face_equals = address_or_zero(face_equals);
        method_thunks.strings_get_count = address_or_zero(strings_get_count);
        method_thunks.strings_find_locale_name =
            address_or_zero(strings_find_locale_name);
        method_thunks.strings_get_locale_name_length =
            address_or_zero(strings_get_locale_name_length);
        method_thunks.strings_get_locale_name =
            address_or_zero(strings_get_locale_name);
        method_thunks.strings_get_string_length =
            address_or_zero(strings_get_string_length);
        method_thunks.strings_get_string = address_or_zero(strings_get_string);
        method_thunks.file_get_reference_key =
            address_or_zero(file_get_reference_key);
        method_thunks.file_get_loader = address_or_zero(file_get_loader);
        method_thunks.file_analyze = address_or_zero(file_analyze);
        method_thunks.local_loader_get_path_length =
            address_or_zero(local_loader_get_path_length);
        method_thunks.local_loader_get_path =
            address_or_zero(local_loader_get_path);
        method_thunks.local_loader_get_last_write_time =
            address_or_zero(local_loader_get_last_write_time);
        method_thunks.local_loader_create_stream_from_key =
            address_or_zero(local_loader_create_stream_from_key);
        method_thunks.stream_read_file_fragment =
            address_or_zero(stream_read_file_fragment);
        method_thunks.stream_release_file_fragment =
            address_or_zero(stream_release_file_fragment);
        method_thunks.stream_get_file_size =
            address_or_zero(stream_get_file_size);
        method_thunks.stream_get_last_write_time =
            address_or_zero(stream_get_last_write_time);
        bool complete = query_interface.is_ok() && add_ref.is_ok() &&
                        release.is_ok() && get_system_font_collection.is_ok() &&
                        create_custom_font_collection.is_ok() &&
                        register_font_collection_loader.is_ok() &&
                        unregister_font_collection_loader.is_ok() &&
                        create_font_file_reference.is_ok() &&
                        create_custom_font_file_reference.is_ok() &&
                        register_font_file_loader.is_ok() &&
                        unregister_font_file_loader.is_ok() &&
                        get_gdi_interop.is_ok() &&
                        create_rendering_params.is_ok() &&
                        create_monitor_rendering_params.is_ok() &&
                        create_typography.is_ok() &&
                        create_text_analyzer.is_ok() &&
                        typography_add_font_feature.is_ok() &&
                        typography_get_font_feature_count.is_ok() &&
                        typography_get_font_feature.is_ok() &&
                        analyzer_analyze_script.is_ok() &&
                        analyzer_analyze_bidi.is_ok() &&
                        analyzer_analyze_number_substitution.is_ok() &&
                        analyzer_analyze_line_breakpoints.is_ok() &&
                        analyzer_get_glyphs.is_ok() &&
                        analyzer_get_glyph_placements.is_ok() &&
                        analyzer_get_gdi_compatible_glyph_placements.is_ok() &&
                        rendering_params_get_gamma.is_ok() &&
                        rendering_params_get_enhanced_contrast.is_ok() &&
                        rendering_params_get_clear_type_level.is_ok() &&
                        rendering_params_get_pixel_geometry.is_ok() &&
                        rendering_params_get_rendering_mode.is_ok() &&
                        create_glyph_run_analysis.is_ok() &&
                        analysis_get_alpha_texture_bounds.is_ok() &&
                        analysis_create_alpha_texture.is_ok() &&
                        analysis_get_alpha_blend_params.is_ok() &&
                        get_system_font_fallback.is_ok() &&
                        create_font_fallback_builder.is_ok() &&
                        get_system_font_set.is_ok() &&
                        get_font_download_queue.is_ok() &&
                        font_set_get_font_count.is_ok() &&
                        font_set_get_matching_fonts_family.is_ok() &&
                        font_set_get_matching_fonts_properties.is_ok() &&
                        fallback_builder_add_mapping.is_ok() &&
                        fallback_builder_add_mappings.is_ok() &&
                        fallback_builder_create_font_fallback.is_ok() &&
                        download_queue_is_empty.is_ok() &&
                        download_queue_begin_download.is_ok() &&
                        download_queue_get_generation_count.is_ok() &&
                        fallback_map_characters.is_ok() &&
                        create_number_substitution.is_ok() &&
                        get_font_family_count.is_ok() &&
                        get_font_family.is_ok() && find_family_name.is_ok() &&
                        family_get_names.is_ok() &&
                        family_get_font_count.is_ok() &&
                        family_get_font.is_ok() &&
                        get_first_matching_font.is_ok() &&
                        font_get_family.is_ok() && font_get_weight.is_ok() &&
                        font_get_stretch.is_ok() && font_get_style.is_ok() &&
                        font_is_symbol.is_ok() &&
                        font_get_simulations.is_ok() &&
                        font_get_metrics.is_ok() &&
                        font_create_font_face.is_ok() &&
                        face_get_type.is_ok() && face_get_files.is_ok() &&
                        face_get_index.is_ok() &&
                        face_get_simulations.is_ok() &&
                        face_is_symbol.is_ok() &&
                        face_get_metrics.is_ok() &&
                        face_get_glyph_count.is_ok() &&
                        face_get_design_glyph_metrics.is_ok() &&
                        face_get_gdi_compatible_glyph_metrics.is_ok() &&
                        face_get_glyph_indices.is_ok() &&
                        face_try_get_font_table.is_ok() &&
                        face_release_font_table.is_ok() &&
                        face_get_glyph_run_outline.is_ok() &&
                        face_get_metrics1.is_ok() &&
                        face_get_caret_metrics.is_ok() &&
                        face_get_unicode_ranges.is_ok() &&
                        face_get_design_glyph_advances.is_ok() &&
                        face_get_kerning_pair_adjustments.is_ok() &&
                        face_get_recommended_rendering_mode.is_ok() &&
                        face_get_recommended_rendering_mode1.is_ok() &&
                        face_get_recommended_rendering_mode2.is_ok() &&
                        face_get_weight.is_ok() &&
                        face_get_stretch.is_ok() && face_get_style.is_ok() &&
                        face_get_panose.is_ok() &&
                        face_get_family_names.is_ok() &&
                        face_get_face_names.is_ok() &&
                        face_get_family_names_locale.is_ok() &&
                        face_get_face_names_locale.is_ok() &&
                        face_get_informational_strings.is_ok() &&
                        face_has_character.is_ok() &&
                        face_is_local.is_ok() && face_are_local.is_ok() &&
                        face_equals.is_ok();
        complete = complete && strings_get_count.is_ok() &&
                   strings_find_locale_name.is_ok() &&
                   strings_get_locale_name_length.is_ok() &&
                   strings_get_locale_name.is_ok() &&
                   strings_get_string_length.is_ok() &&
                   strings_get_string.is_ok() &&
                   file_get_reference_key.is_ok() &&
                   file_get_loader.is_ok() && file_analyze.is_ok() &&
                   local_loader_get_path_length.is_ok() &&
                   local_loader_get_path.is_ok() &&
                   local_loader_get_last_write_time.is_ok() &&
                   local_loader_create_stream_from_key.is_ok() &&
                   stream_read_file_fragment.is_ok() &&
                   stream_release_file_fragment.is_ok() &&
                   stream_get_file_size.is_ok() &&
                   stream_get_last_write_time.is_ok();
        {
            const auto thunk = [&](const char* name) {
                const auto address = reg.get_thunk_address("dwrite.dll", name);
                if (address.is_err()) complete = false;
                return address.is_ok() ? address.value() : GuestAddr{0};
            };
            method_thunks.create_text_format = thunk("_DWrite_CreateTextFormat");
            method_thunks.create_text_layout = thunk("_DWrite_CreateTextLayout");
            method_thunks.create_gdi_compatible_text_layout =
                thunk("_DWrite_CreateGdiCompatibleTextLayout");
            method_thunks.create_ellipsis_trimming_sign = thunk("_DWrite_CreateEllipsisTrimmingSign");
            const auto slots = [&](const char* prefix, auto& table) {
                for (u32 slot = 3; slot < table.size(); ++slot) {
                    table[slot] = thunk((std::string(prefix) + std::to_string(slot)).c_str());
                }
            };
            slots("_DWrite_TextFormat_", method_thunks.text_format);
            slots("_DWrite_TextLayout_", method_thunks.text_layout);
            slots("_DWrite_InlineObject_", method_thunks.inline_object);
        }
        for (const u32 arg_count : winapi::DWRITE_NOT_IMPL_ARG_COUNTS) {
            const std::string name =
                "_DWrite_NotImpl" + std::to_string(arg_count);
            const auto thunk = reg.get_thunk_address("dwrite.dll", name);
            if (thunk.is_err()) {
                complete = false;
                break;
            }
            method_thunks.not_impl_by_arg_count[arg_count] = thunk.value();
        }
        for (const u32 arg_count : winapi::DWRITE_RETURN_ZERO_ARG_COUNTS) {
            const std::string name =
                "_DWrite_ReturnZero" + std::to_string(arg_count);
            const auto thunk = reg.get_thunk_address("dwrite.dll", name);
            if (thunk.is_err()) {
                complete = false;
                break;
            }
            method_thunks.return_zero_by_arg_count[arg_count] = thunk.value();
        }
        if (complete) ctx.dwrite_mgr.set_method_thunks(std::move(method_thunks));

        // DWriteCore.dll. Guests import this instead of dwrite.dll.
        // Same factory surface (DWriteCoreCreateFactory and shared method
        // thunks).
        (void)reg.register_dll(
            "DWriteCore.dll", winapi::create_dwrite_thunks(ctx.dwrite_mgr));
    }
}

} // namespace vitreo::cli

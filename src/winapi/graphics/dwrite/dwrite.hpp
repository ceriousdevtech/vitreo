// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include <vitreo/common.hpp>
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/dwrite/text_layout_engine.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <functional>
#include <initializer_list>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vitreo {
class GuestMemory;
}
namespace vitreo::capsule {
class Vfs;
}

#include "winapi/graphics/dwrite/dwrite_text_contract.inc"

namespace vitreo::winapi {

class Marshaler;
class GdiManager;

// Unsupported HRESULT-returning COM methods still require their exact x86
// stdcall footprint. Keep export creation and DLL-registry wiring driven by
// one list so every published thunk is also installed into DWriteManager.
inline constexpr std::array<u32, 11> DWRITE_NOT_IMPL_ARG_COUNTS{
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
};

// BOOL/UINT32 getters that must return 0/FALSE (not E_NOTIMPL).
inline constexpr std::array<u32, 2> DWRITE_RETURN_ZERO_ARG_COUNTS{1, 2};

class DWriteManager {
public:
    struct MethodThunks {
        GuestAddr query_interface{0};
        GuestAddr add_ref{0};
        GuestAddr release{0};
        GuestAddr get_system_font_collection{0};
        GuestAddr create_custom_font_collection{0};
        GuestAddr register_font_collection_loader{0};
        GuestAddr unregister_font_collection_loader{0};
        GuestAddr create_font_file_reference{0};
        GuestAddr create_custom_font_file_reference{0};
        GuestAddr create_rendering_params{0};
        GuestAddr create_monitor_rendering_params{0};
        GuestAddr create_typography{0};
        GuestAddr create_text_analyzer{0};
        GuestAddr typography_add_font_feature{0};
        GuestAddr typography_get_font_feature_count{0};
        GuestAddr typography_get_font_feature{0};
        GuestAddr analyzer_analyze_script{0};
        GuestAddr analyzer_analyze_bidi{0};
        GuestAddr analyzer_analyze_number_substitution{0};
        GuestAddr analyzer_analyze_line_breakpoints{0};
        GuestAddr analyzer_get_glyphs{0};
        GuestAddr analyzer_get_glyph_placements{0};
        GuestAddr analyzer_get_gdi_compatible_glyph_placements{0};
        GuestAddr register_font_file_loader{0};
        GuestAddr unregister_font_file_loader{0};
        GuestAddr get_gdi_interop{0};
        GuestAddr gdi_create_font_from_logfont{0};
        GuestAddr gdi_convert_font_to_logfont{0};
        GuestAddr gdi_convert_font_face_to_logfont{0};
        GuestAddr gdi_create_font_face_from_hdc{0};
        GuestAddr create_glyph_run_analysis{0};
        GuestAddr create_glyph_run_analysis2{0};
        GuestAddr analysis_get_alpha_texture_bounds{0};
        GuestAddr analysis_create_alpha_texture{0};
        GuestAddr analysis_get_alpha_blend_params{0};
        GuestAddr get_system_font_fallback{0};
        GuestAddr create_font_fallback_builder{0};
        GuestAddr get_system_font_set{0};
        GuestAddr get_font_download_queue{0};
        GuestAddr font_set_get_font_count{0};
        GuestAddr font_set_get_matching_fonts_family{0};
        GuestAddr font_set_get_matching_fonts_properties{0};
        GuestAddr fallback_builder_add_mapping{0};
        GuestAddr fallback_builder_add_mappings{0};
        GuestAddr fallback_builder_create_font_fallback{0};
        GuestAddr download_queue_is_empty{0};
        GuestAddr download_queue_begin_download{0};
        GuestAddr download_queue_get_generation_count{0};
        GuestAddr create_number_substitution{0};
        GuestAddr fallback_map_characters{0};
        GuestAddr get_font_family_count{0};
        GuestAddr get_font_family{0};
        GuestAddr find_family_name{0};
        GuestAddr family_get_font_count{0};
        GuestAddr family_get_font{0};
        GuestAddr family_get_names{0};
        GuestAddr get_first_matching_font{0};
        GuestAddr font_get_family{0};
        GuestAddr font_get_weight{0};
        GuestAddr font_get_stretch{0};
        GuestAddr font_get_style{0};
        GuestAddr font_is_symbol_font{0};
        GuestAddr font_get_face_names{0};
        GuestAddr font_get_simulations{0};
        GuestAddr font_get_metrics{0};
        GuestAddr font_create_font_face{0};
        GuestAddr face_get_type{0};
        GuestAddr face_get_files{0};
        GuestAddr face_get_index{0};
        GuestAddr face_get_simulations{0};
        GuestAddr face_is_symbol_font{0};
        GuestAddr face_get_metrics{0};
        GuestAddr face_get_glyph_count{0};
        GuestAddr face_get_design_glyph_metrics{0};
        GuestAddr face_get_gdi_compatible_glyph_metrics{0};
        GuestAddr face_get_glyph_indices{0};
        GuestAddr face_try_get_font_table{0};
        GuestAddr face_release_font_table{0};
        GuestAddr face_get_glyph_run_outline{0};
        GuestAddr face_get_metrics1{0};
        GuestAddr face_get_caret_metrics{0};
        GuestAddr face_get_design_glyph_advances{0};
        GuestAddr face_get_kerning_pair_adjustments{0};
        GuestAddr face_get_recommended_rendering_mode{0};
        GuestAddr face_get_recommended_rendering_mode1{0};
        GuestAddr face_get_recommended_rendering_mode2{0};
        GuestAddr face_get_weight{0};
        GuestAddr face_get_stretch{0};
        GuestAddr face_get_style{0};
        GuestAddr face_get_family_names{0};
        GuestAddr face_get_face_names{0};
        GuestAddr face_get_family_names_locale{0};
        GuestAddr face_get_face_names_locale{0};
        GuestAddr face_get_panose{0};
        GuestAddr face_has_character{0};
        GuestAddr face_is_local{0};
        GuestAddr face_are_local{0};
        GuestAddr face_equals{0};
        GuestAddr face_get_informational_strings{0};
        GuestAddr face_get_unicode_ranges{0};
        GuestAddr strings_get_count{0};
        GuestAddr strings_find_locale_name{0};
        GuestAddr strings_get_locale_name_length{0};
        GuestAddr strings_get_locale_name{0};
        GuestAddr strings_get_string_length{0};
        GuestAddr strings_get_string{0};
        GuestAddr file_get_reference_key{0};
        GuestAddr file_get_loader{0};
        GuestAddr file_analyze{0};
        GuestAddr local_loader_get_path_length{0};
        GuestAddr local_loader_get_path{0};
        GuestAddr local_loader_get_last_write_time{0};
        GuestAddr local_loader_create_stream_from_key{0};
        GuestAddr stream_read_file_fragment{0};
        GuestAddr stream_release_file_fragment{0};
        GuestAddr stream_get_file_size{0};
        GuestAddr stream_get_last_write_time{0};
        GuestAddr rendering_params_get_gamma{0};
        GuestAddr rendering_params_get_enhanced_contrast{0};
        GuestAddr rendering_params_get_clear_type_level{0};
        GuestAddr rendering_params_get_pixel_geometry{0};
        GuestAddr rendering_params_get_rendering_mode{0};
        // Indexed by the total number of x86 stack arguments, including
        // `this`. COM methods are stdcall on x86, so unsupported methods still
        // need a signature-compatible thunk even when they return E_NOTIMPL.
        std::array<GuestAddr, 12> not_impl_by_arg_count{};
        GuestAddr create_text_format{0};
        GuestAddr create_text_layout{0};
        GuestAddr create_gdi_compatible_text_layout{0};
        GuestAddr create_ellipsis_trimming_sign{0};
        // Indexed by vtable slot (dwrite_text_contract.inc); IUnknown slots
        // use the shared QueryInterface/AddRef/Release thunks.
        std::array<GuestAddr, std::size(dwrite_text_contract::text_format_methods)> text_format{};
        std::array<GuestAddr, std::size(dwrite_text_contract::text_layout_methods)> text_layout{};
        std::array<GuestAddr, std::size(dwrite_text_contract::inline_object_methods)> inline_object{};
        std::array<GuestAddr, 3> return_zero_by_arg_count{};
    };

    DWriteManager(GuestMemory& memory, bool is_64bit,
                  capsule::Vfs* vfs = nullptr,
                  GdiManager* gdi = nullptr);

    void set_method_thunks(MethodThunks thunks);
    using GuestCall = std::function<u64(
        GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }
    /// Reuse of already-parsed font files and shared local-loader stream
    /// mappings. Defaults to on; VITREO_DWRITE_FONT_REUSE=0 restores the
    /// copy-and-reparse-every-call path.
    void set_font_reuse_enabled(bool enabled) { m_font_reuse = enabled; }

    // DirectWrite shared-factory objects are callable from multiple guest
    // threads. Keep the manager's shared COM object/cache state coherent while
    // still permitting same-thread re-entry through guest callbacks.
    void invoke_serialized(const ApiHandler& handler, Marshaler& m);

    void impl_DWriteCreateFactory(Marshaler& m);
    void impl_QueryInterface(Marshaler& m);
    /// VITREO_DWRITE_DIAG=1: dump the first 20 vtable slots of @p object.
    void dump_vtable(const char* label, GuestAddr object,
                     GuestAddr vtable) const;
    void impl_AddRef(Marshaler& m);
    void impl_Release(Marshaler& m);
    void impl_NotImpl(Marshaler& m, u32 argument_count = 0);
    void impl_GetSystemFontCollection(Marshaler& m);
    void impl_CreateCustomFontCollection(Marshaler& m);
    void impl_RegisterFontCollectionLoader(Marshaler& m);
    void impl_UnregisterFontCollectionLoader(Marshaler& m);
    void impl_CreateFontFileReference(Marshaler& m);
    void impl_CreateCustomFontFileReference(Marshaler& m);
    void impl_RegisterFontFileLoader(Marshaler& m);
    void impl_UnregisterFontFileLoader(Marshaler& m);
    void impl_GetGdiInterop(Marshaler& m);
    /// IDWriteGdiInterop::CreateFontFromLOGFONT / ConvertFontToLOGFONT /
    /// ConvertFontFaceToLOGFONT (dwrite.h), measured by
    /// tools/oracle_probe/dwrite_gdi_interop_probe.cpp.
    void impl_GdiCreateFontFromLogfont(Marshaler& m);
    void impl_GdiConvertFontToLogfont(Marshaler& m);
    void impl_GdiConvertFontFaceToLogfont(Marshaler& m);
    void impl_GdiCreateFontFaceFromHdc(Marshaler& m);
    void impl_CreateGlyphRunAnalysis(Marshaler& m);
    /// IDWriteFactory2::CreateGlyphRunAnalysis (dwrite_2.h) — same object,
    /// no pixelsPerDip, plus gridFitMode / antialiasMode.
    void impl_CreateGlyphRunAnalysis2(Marshaler& m);
    void impl_AnalysisGetAlphaTextureBounds(Marshaler& m);
    void impl_AnalysisCreateAlphaTexture(Marshaler& m);
    void impl_AnalysisGetAlphaBlendParams(Marshaler& m);
    void impl_GetSystemFontFallback(Marshaler& m);
    void impl_CreateFontFallbackBuilder(Marshaler& m);
    void impl_GetSystemFontSet(Marshaler& m);
    void impl_GetFontDownloadQueue(Marshaler& m);
    void impl_FontSetGetFontCount(Marshaler& m);
    void impl_FontSetGetMatchingFontsFamily(Marshaler& m);
    void impl_FontSetGetMatchingFontsProperties(Marshaler& m);
    void impl_FallbackBuilderAddMapping(Marshaler& m);
    void impl_FallbackBuilderAddMappings(Marshaler& m);
    void impl_FallbackBuilderCreateFontFallback(Marshaler& m);
    void impl_DownloadQueueIsEmpty(Marshaler& m);
    void impl_DownloadQueueBeginDownload(Marshaler& m);
    void impl_DownloadQueueGetGenerationCount(Marshaler& m);
    void impl_CreateRenderingParams(Marshaler& m);
    void impl_CreateMonitorRenderingParams(Marshaler& m);
    void impl_CreateTypography(Marshaler& m);
    void impl_CreateTextAnalyzer(Marshaler& m);
    void impl_TypographyAddFontFeature(Marshaler& m);
    void impl_TypographyGetFontFeatureCount(Marshaler& m);
    void impl_TypographyGetFontFeature(Marshaler& m);
    void impl_AnalyzerAnalyzeScript(Marshaler& m);
    void impl_AnalyzerAnalyzeBidi(Marshaler& m);
    void impl_AnalyzerAnalyzeNumberSubstitution(Marshaler& m);
    void impl_AnalyzerAnalyzeLineBreakpoints(Marshaler& m);
    void impl_AnalyzerGetGlyphs(Marshaler& m);
    void impl_AnalyzerGetGlyphPlacements(Marshaler& m);
    void impl_AnalyzerGetGdiCompatibleGlyphPlacements(Marshaler& m);
    void impl_RenderingParamsGetGamma(Marshaler& m);
    void impl_RenderingParamsGetEnhancedContrast(Marshaler& m);
    void impl_RenderingParamsGetClearTypeLevel(Marshaler& m);
    void impl_RenderingParamsGetPixelGeometry(Marshaler& m);
    void impl_RenderingParamsGetRenderingMode(Marshaler& m);
    void impl_CreateNumberSubstitution(Marshaler& m);
    void impl_FallbackMapCharacters(Marshaler& m);
    void impl_GetFontFamilyCount(Marshaler& m);
    void impl_GetFontFamily(Marshaler& m);
    void impl_FindFamilyName(Marshaler& m);
    void impl_FamilyGetFontCount(Marshaler& m);
    void impl_FamilyGetFont(Marshaler& m);
    void impl_FamilyGetNames(Marshaler& m);
    void impl_GetFirstMatchingFont(Marshaler& m);
    void impl_FontGetFamily(Marshaler& m);
    void impl_FontGetWeight(Marshaler& m);
    void impl_FontGetStretch(Marshaler& m);
    void impl_FontGetStyle(Marshaler& m);
    void impl_FontIsSymbolFont(Marshaler& m);
    void impl_FontGetFaceNames(Marshaler& m);
    void impl_FontGetSimulations(Marshaler& m);
    void impl_FontGetMetrics(Marshaler& m);
    void impl_FontCreateFontFace(Marshaler& m);
    void impl_FaceGetType(Marshaler& m);
    void impl_FaceGetFiles(Marshaler& m);
    void impl_FaceGetIndex(Marshaler& m);
    void impl_FaceGetSimulations(Marshaler& m);
    void impl_FaceIsSymbolFont(Marshaler& m);
    void impl_FaceGetMetrics(Marshaler& m);
    void impl_FaceGetGlyphCount(Marshaler& m);
    void impl_FaceGetDesignGlyphMetrics(Marshaler& m);
    void impl_FaceGetGdiCompatibleGlyphMetrics(Marshaler& m);
    void impl_FaceGetGlyphIndices(Marshaler& m);
    void impl_FaceTryGetFontTable(Marshaler& m);
    void impl_FaceReleaseFontTable(Marshaler& m);
    void impl_FaceGetGlyphRunOutline(Marshaler& m);
    void impl_FaceGetMetrics1(Marshaler& m);
    void impl_FaceGetCaretMetrics(Marshaler& m);
    void impl_FaceGetDesignGlyphAdvances(Marshaler& m);
    void impl_FaceGetKerningPairAdjustments(Marshaler& m);
    void impl_FaceGetRecommendedRenderingMode(Marshaler& m);
    void impl_FaceGetRecommendedRenderingMode1(Marshaler& m);
    void impl_FaceGetRecommendedRenderingMode2(Marshaler& m);
    void impl_FaceGetWeight(Marshaler& m);
    void impl_FaceGetStretch(Marshaler& m);
    void impl_FaceGetStyle(Marshaler& m);
    void impl_FaceGetFamilyNames(Marshaler& m);
    void impl_FaceGetFaceNames(Marshaler& m);
    void impl_FaceGetFamilyNamesLocale(Marshaler& m);
    void impl_FaceGetFaceNamesLocale(Marshaler& m);
    void impl_FaceGetPanose(Marshaler& m);
    void impl_FaceHasCharacter(Marshaler& m);
    void impl_FaceIsLocal(Marshaler& m);
    void impl_FaceAreLocal(Marshaler& m);
    void impl_FaceEquals(Marshaler& m);
    void impl_FaceGetInformationalStrings(Marshaler& m);
    void impl_FaceGetUnicodeRanges(Marshaler& m);
    void impl_ReturnZero(Marshaler& m);
    struct TextStyleSpec;  // dwrite_text_internal.hpp
    struct TextState;
    void impl_CreateTextFormat(Marshaler& m);
    void impl_CreateTextLayout(Marshaler& m);
    void impl_CreateGdiCompatibleTextLayout(Marshaler& m);
    void impl_CreateEllipsisTrimmingSign(Marshaler& m);
    /// IDWriteTextFormat / IDWriteTextLayout / IDWriteInlineObject (the
    /// ellipsis trimming sign) method @p slot of dwrite_text_contract.
    enum class TextInterface : u8 { Format, Layout, InlineObject };
    void text_method(Marshaler& m, TextInterface kind, u32 slot);

    /// What Direct2D needs to draw a text layout: the formatted runs plus
    /// each run's IUnknown drawing effect, and the underline/strikethrough
    /// rectangles DirectWrite would report through IDWriteTextRenderer.
    struct TextDecoration {
        float x{0};
        float baseline_y{0};
        float width{0};
        float thickness{0};
        float offset{0};  ///< from the baseline, positive below
        GuestAddr effect{0};
        bool strikethrough{false};
    };
    struct TextDrawing {
        dwrite::TextLayoutResult layout;
        float max_width{0};   ///< the layout rectangle (D2D1_DRAW_TEXT_OPTIONS_CLIP)
        float max_height{0};
        std::vector<GuestAddr> run_effects;
        std::vector<TextDecoration> decorations;
        /// Inline objects (other than the trimming sign) placed in the layout.
        struct InlineObject { GuestAddr object{0}; float x{0}; float y{0}; GuestAddr effect{0}; bool rtl{false}; };
        std::vector<InlineObject> inline_objects;
    };
    /// S_OK and @p out for an IDWriteTextLayout this manager created;
    /// E_INVALIDARG for anything else; the layout's failure HRESULT when it
    /// cannot be formatted.
    [[nodiscard]] u32 text_drawing(GuestAddr layout, TextDrawing& out);
    /// ID2D1RenderTarget::DrawText: format @p text with an IDWriteTextFormat
    /// into a @p width x @p height box, as CreateTextLayout would.
    /// The face and DWRITE_FONT_SIMULATIONS of an IDWriteFontFace this
    /// manager created (DWRITE_GLYPH_RUN::fontFace); false otherwise.
    [[nodiscard]] bool font_face_description(GuestAddr face, dwrite::MatchedFont& font,
                                             u32& simulations);
    /// DWRITE_RENDERING_MODE of IDWriteRenderingParams this manager created.
    [[nodiscard]] std::optional<u32> rendering_params_mode(GuestAddr params);
    [[nodiscard]] u32 text_drawing_for_format(GuestAddr format, std::u16string text,
                                              float width, float height,
                                              TextDrawing& out);
    void impl_StringsGetCount(Marshaler& m);
    void impl_StringsFindLocaleName(Marshaler& m);
    void impl_StringsGetLocaleNameLength(Marshaler& m);
    void impl_StringsGetLocaleName(Marshaler& m);
    void impl_StringsGetStringLength(Marshaler& m);
    void impl_StringsGetString(Marshaler& m);
    void impl_FileGetReferenceKey(Marshaler& m);
    void impl_FileGetLoader(Marshaler& m);
    void impl_FileAnalyze(Marshaler& m);
    void impl_LocalLoaderGetPathLength(Marshaler& m);
    void impl_LocalLoaderGetPath(Marshaler& m);
    void impl_LocalLoaderGetLastWriteTime(Marshaler& m);
    void impl_LocalLoaderCreateStreamFromKey(Marshaler& m);
    void impl_StreamReadFileFragment(Marshaler& m);
    void impl_StreamReleaseFileFragment(Marshaler& m);
    void impl_StreamGetFileSize(Marshaler& m);
    void impl_StreamGetLastWriteTime(Marshaler& m);

private:
    /// Shared body of the IDWriteFactory and IDWriteFactory2 overloads of
    /// CreateGlyphRunAnalysis; @p factory2_form selects the argument layout.
    void create_glyph_run_analysis(Marshaler& m, bool factory2_form);


    enum class ObjectType {
        Factory,
        FontCollection,
        FontFamily,
        Font,
        FontFace,
        FontFallback,
        FontFallbackBuilder,
        FontSet,
        FontDownloadQueue,
        NumberSubstitution,
        LocalizedStrings,
        FontFile,
        LocalFontFileLoader,
        FontFileStream,
        RenderingParams,
        Typography,
        TextAnalyzer,
        GdiInterop,
        GlyphRunAnalysis,
        TextFormat,
        TextLayout,
        TrimmingSign,
    };

    using ParsedFonts = std::shared_ptr<const std::vector<dwrite::MatchedFont>>;
    using CollectionFamilies =
        std::vector<std::shared_ptr<std::vector<dwrite::MatchedFont>>>;

    struct Object {
        ObjectType type{ObjectType::Factory};
        u32 ref_count{1};
        usize family_index{0};
        dwrite::MatchedFont font{};
        // DWRITE_FONT_SIMULATIONS of a Font/FontFace. `font` stays the
        // physical face, so reopening it never picks up a different one.
        u32 simulations{0};
        u32 number_method{0};
        bool ignore_user_override{false};
        std::u16string locale;
        std::u16string localized_value;
        std::u16string local_font_path;
        GuestAddr key_address{0};
        u32 key_size{0};
        usize key_mapping_size{0};
        GuestAddr font_file_loader{0};
        GuestAddr stream_data_address{0};
        u64 stream_file_size{0};
        usize stream_mapping_size{0};
        // The data mapping is an entry of m_stream_mappings, not owned.
        bool stream_shared_mapping{false};
        u64 stream_last_write_time{0};
        float gamma{1.8f};
        float enhanced_contrast{0.5f};
        float clear_type_level{1.0f};
        u32 pixel_geometry{1}; // DWRITE_PIXEL_GEOMETRY_RGB
        u32 rendering_mode{0}; // DWRITE_RENDERING_MODE_DEFAULT
        std::vector<std::pair<u32, u32>> typography_features;
        bool font_set_empty{false};
        bool font_set_single_family{false};
        bool custom_collection{false};
        std::vector<std::shared_ptr<std::vector<dwrite::MatchedFont>>>
            collection_families;
        std::shared_ptr<std::vector<dwrite::MatchedFont>> family_fonts;
        std::unordered_set<GuestAddr> collection_loaders;
        std::unordered_set<GuestAddr> file_loaders;
        // A factory's custom collections by (collection loader, collection
        // key) and custom-loader font files by (file loader, reference
        // key), each dropped when its loader is unregistered. Measured by
        // tools/oracle_probe/dwrite_custom_loader_probe.cpp.
        std::unordered_map<std::string, CollectionFamilies> custom_collections;
        std::unordered_map<std::string, ParsedFonts> custom_font_files;
        // Named for the DWRITE_TEXTURE_* type each one answers, because that
        // is what selects them: ClearType is the only three-bytes-per-pixel
        // case, while an aliased rendering mode and grayscale antialiasing
        // both produce one byte per pixel.
        dwrite::RasterizedGlyphRun cleartype_3x1_texture{};
        dwrite::RasterizedGlyphRun aliased_1x1_texture{};
        /// TextFormat / TextLayout / TrimmingSign state.
        std::shared_ptr<TextState> text;
    };

    struct FontTableMapping {
        GuestAddr face{0};
        usize mapped_size{0};
    };

    // Text formatting (dwrite_text_format.cpp / dwrite_text_layout.cpp).
    void text_format_method(Marshaler& m, GuestAddr self, TextState& state, u32 slot);
    void text_layout_method(Marshaler& m, GuestAddr self, TextState& state, u32 slot);
    void trimming_sign_method(Marshaler& m, GuestAddr self, TextState& state, u32 slot);
    void retain_com(GuestAddr object);
    void release_com(GuestAddr object);
    void release_text_state(TextState& state);
    [[nodiscard]] u32 format_text(TextState& state);
    [[nodiscard]] std::optional<dwrite::MatchedFont> resolve_text_font(
        GuestAddr collection, const std::u16string& family, u32 weight,
        u32 style, u32 stretch, u32& simulations);
    [[nodiscard]] u32 text_layout_query(Marshaler& m, GuestAddr self, TextState& state, u32 slot);
    [[nodiscard]] u32 hit_test(Marshaler& m, TextState& state, u32 slot);
    /// Runs IDWriteTextRenderer callbacks for a formatted layout at (x, y).
    [[nodiscard]] u32 render_to(TextState& state, GuestAddr context, GuestAddr renderer, float x, float y);
    [[nodiscard]] u32 collect_drawing(TextState& state, TextDrawing& out);
    bool write_floats_at(GuestAddr at, std::initializer_list<u32> words);
    [[nodiscard]] GuestAddr create_text_object(ObjectType type, std::shared_ptr<TextState> state);

    [[nodiscard]] bool ensure_storage();
    [[nodiscard]] GuestAddr create_factory();
    [[nodiscard]] GuestAddr create_object(
        ObjectType type, usize family_index = 0,
        dwrite::MatchedFont font = {});
    [[nodiscard]] bool iid_is_supported(GuestAddr iid_address, ObjectType type) const;
    [[nodiscard]] bool write_pointer(GuestAddr address, GuestAddr value);
    [[nodiscard]] VResult<u64> read_pointer(GuestAddr address) const;
    [[nodiscard]] VResult<std::u16string> read_utf16(
        GuestAddr address, usize length) const;
    [[nodiscard]] VResult<std::u16string> read_utf16_z(
        GuestAddr address, usize max_length) const;
    [[nodiscard]] usize ensure_family_index(const std::u16string& family);
    [[nodiscard]] GuestAddr not_impl_thunk(u32 arg_count) const;
    [[nodiscard]] GuestAddr return_zero_thunk(u32 arg_count) const;
    [[nodiscard]] std::optional<u64> call_guest_method(
        GuestAddr object, u32 slot, const u64* trailing_args,
        usize trailing_count) const;
    void release_guest_object(GuestAddr object) const;
    [[nodiscard]] std::shared_ptr<std::vector<dwrite::MatchedFont>>
        ensure_family_fonts(Object& family);
    [[nodiscard]] std::u16string object_family_name(const Object& object) const;
    [[nodiscard]] std::optional<std::vector<u8>> read_vfs_file(
        const std::u16string& path) const;
    /// Runs the IDWriteFontFileLoader/IDWriteFontFileStream sequence for a
    /// custom-loader file and hands the whole-file fragment to @p consume
    /// while it is held (between ReadFileFragment and ReleaseFileFragment).
    /// @p fragment_limit caps the fragment at the start of the file;
    /// @p query_last_write_time adds GetLastWriteTime after GetFileSize.
    using FragmentConsumer = std::function<u32(GuestAddr fragment, usize size)>;
    [[nodiscard]] u32 read_guest_font_stream(
        const Object& file, GuestAddr scratch_address,
        const FragmentConsumer& consume,
        u64 fragment_limit = ~u64{0}, bool query_last_write_time = false);
    [[nodiscard]] std::pair<u32, std::vector<u8>> read_font_file_bytes(
        const Object& file, GuestAddr scratch_address);
    /// Guest loader address followed by the key bytes; nullopt if the key
    /// cannot be read.
    [[nodiscard]] std::optional<std::string> loader_key(
        GuestAddr loader, GuestAddr key, u32 key_size) const;
    /// Reads and parses @p file, reusing an earlier parse only when the
    /// bytes are proven identical (see m_font_files).
    [[nodiscard]] std::pair<u32, ParsedFonts> load_font_file(
        const Object& file, GuestAddr scratch_address);
    void remember_font_file(std::string key, std::shared_ptr<const std::vector<u8>> bytes,
                            ParsedFonts fonts, u64 file_size, u64 last_write_time);
    /// Shared read-only guest mapping of a local font file's bytes for
    /// IDWriteLocalFontFileLoader streams; 0 on failure.
    [[nodiscard]] GuestAddr acquire_stream_mapping(
        const std::u16string& path, const std::string& utf8_path,
        u64 file_size, u64 last_write_time, usize& mapped_size);
    void release_stream_mapping(GuestAddr address);
    void unmap_stream_mapping(GuestAddr address);
    [[nodiscard]] std::optional<std::u16string> publish_font_file(
        const dwrite::MatchedFont& font);
    [[nodiscard]] GuestAddr create_local_font_file(
        const std::u16string& path);

    /// A face of a system family as GDI names and weighs it: its win32
    /// family (name ID 1), full name (name ID 4) and OS/2 usWeightClass.
    struct GdiFace {
        dwrite::MatchedFont font;
        std::u16string win32_family;
        std::u16string full_name;
        u32 weight{400};
    };
    struct GdiFamily {
        std::vector<GdiFace> faces;
        std::shared_ptr<std::vector<dwrite::MatchedFont>> fonts;
    };
    [[nodiscard]] std::shared_ptr<const GdiFamily> gdi_family(
        const std::u16string& family);
    /// Writes the LOGFONTW ConvertFontToLOGFONT / ConvertFontFaceToLOGFONT
    /// report for @p object (a Font or FontFace).
    [[nodiscard]] bool write_gdi_logfont(GuestAddr address,
                                         const Object& object);
    std::unordered_map<std::u16string, std::shared_ptr<const GdiFamily>>
        m_gdi_families;

    struct PublishedFontFile {
        std::string source_version;
        std::weak_ptr<const std::vector<u8>> source_data;
        std::u16string guest_path;
        usize size{0};
    };
    std::unordered_map<std::string, PublishedFontFile> m_published_fonts;

    // Parsed font files from earlier CreateCustomFontCollection /
    // IDWriteFontFile::Analyze calls. The key (loader + reference key, or
    // local path) only selects a candidate: a custom-loader entry is reused
    // only when the stream's current bytes compare equal to `bytes`, and a
    // local-file entry only when the file's size and last write time match.
    struct FontFileEntry {
        std::shared_ptr<const std::vector<u8>> bytes;
        ParsedFonts fonts;
        u64 file_size{0};
        u64 last_write_time{0};
        u64 last_use{0};
    };
    std::unordered_map<std::string, FontFileEntry> m_font_files;
    usize m_font_file_bytes{0};
    u64 m_font_file_clock{0};

    // Read-only guest mappings behind local-loader streams, by address. An
    // entry is current for its path while its size and last write time still
    // describe the file; idle current entries are retained up to a budget.
    struct StreamMapping {
        std::u16string path;
        usize mapped_size{0};
        u64 file_size{0};
        u64 last_write_time{0};
        u32 streams{0};
        u64 last_use{0};
        bool current{true};
    };
    std::unordered_map<GuestAddr, StreamMapping> m_stream_mappings;
    std::unordered_map<std::u16string, GuestAddr> m_current_stream_mapping;
    usize m_idle_stream_bytes{0};
    bool m_font_reuse{true};

    GuestMemory& m_memory;
    bool m_is_64bit;
    capsule::Vfs* m_vfs;
    GdiManager* m_gdi;
    GuestAddr m_region_base{0};
    // DWriteCreateFactory may be called concurrently. Serializing the lazy
    // vtable mapping prevents two guest threads from racing the same region.
    std::mutex m_storage_mutex;
    std::recursive_mutex m_api_mutex;
    GuestAddr m_factory_vtable{0};
    GuestAddr m_collection_vtable{0};
    GuestAddr m_family_vtable{0};
    GuestAddr m_font_vtable{0};
    GuestAddr m_face_vtable{0};
    GuestAddr m_fallback_vtable{0};
    GuestAddr m_fallback_builder_vtable{0};
    GuestAddr m_font_set_vtable{0};
    GuestAddr m_font_download_queue_vtable{0};
    GuestAddr m_number_substitution_vtable{0};
    GuestAddr m_localized_strings_vtable{0};
    GuestAddr m_font_file_vtable{0};
    GuestAddr m_local_loader_vtable{0};
    GuestAddr m_font_file_stream_vtable{0};
    GuestAddr m_rendering_params_vtable{0};
    GuestAddr m_typography_vtable{0};
    GuestAddr m_text_analyzer_vtable{0};
    GuestAddr m_gdi_interop_vtable{0};
    GuestAddr m_glyph_run_analysis_vtable{0};
    GuestAddr m_text_format_vtable{0};
    GuestAddr m_text_layout_vtable{0};
    GuestAddr m_trimming_sign_vtable{0};
    GuestAddr m_callback_scratch{0};
    GuestAddr m_next_object{0};
    std::vector<GuestAddr> m_free_objects;
    GuestAddr m_system_collection{0};
    GuestAddr m_local_loader{0};
    bool m_storage_ready{false};
    MethodThunks m_method_thunks{};
    GuestCall m_guest_call;
    std::vector<std::u16string> m_font_families;
    std::unordered_map<GuestAddr, Object> m_objects;
    // Keyed by the opaque tableContext returned to the guest. The context is
    // also the base of the dedicated read-only guest mapping.
    std::unordered_map<GuestAddr, FontTableMapping> m_font_tables;
};

[[nodiscard]] DllExports create_dwrite_thunks(DWriteManager& manager);

} // namespace vitreo::winapi

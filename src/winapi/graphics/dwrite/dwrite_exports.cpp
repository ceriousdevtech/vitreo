// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/graphics/dwrite/dwrite_internal.hpp"

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dwrite/font_backend.hpp"
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unistd.h>
#include <utility>

namespace vitreo::winapi {
namespace {

// Every interface method, not just the ones with hand-written diagnostics, so
// a caller that stops mid-sequence shows its last successful call.
bool dwrite_call_trace_enabled() {
    static const bool on = std::getenv("VITREO_DWRITE_CALLS") != nullptr;
    return on;
}

} // namespace

DllExports create_dwrite_thunks(DWriteManager& manager) {
    DllExports exports;
    exports.entries.push_back({"DWriteCreateFactory", 0,
        [&manager](Marshaler& m) { manager.impl_DWriteCreateFactory(m); }, 12});
    // DWriteCore.dll exports DWriteCoreCreateFactory with the same
    // (factoryType, iid, factory**) contract as DWriteCreateFactory.
    exports.entries.push_back({"DWriteCoreCreateFactory", 0,
        [&manager](Marshaler& m) { manager.impl_DWriteCreateFactory(m); }, 12});
    exports.entries.push_back({"_DWrite_QueryInterface", 0,
        [&manager](Marshaler& m) { manager.impl_QueryInterface(m); }, 12});
    exports.entries.push_back({"_DWrite_AddRef", 0,
        [&manager](Marshaler& m) { manager.impl_AddRef(m); }, 4});
    exports.entries.push_back({"_DWrite_Release", 0,
        [&manager](Marshaler& m) { manager.impl_Release(m); }, 4});
    const auto add_not_impl = [&exports, &manager](
                                  std::string name, u16 stdcall_bytes) {
        exports.entries.push_back({std::move(name), 0,
            [&manager, argument_count = static_cast<u32>(
                           stdcall_bytes / sizeof(u32))](
                Marshaler& m) {
                manager.impl_NotImpl(m, argument_count);
            },
            stdcall_bytes});
    };
    for (const u32 arg_count : DWRITE_NOT_IMPL_ARG_COUNTS) {
        add_not_impl(
            "_DWrite_NotImpl" + std::to_string(arg_count),
            static_cast<u16>(arg_count * sizeof(u32)));
    }
    for (const u32 arg_count : DWRITE_RETURN_ZERO_ARG_COUNTS) {
        exports.entries.push_back(
            {"_DWrite_ReturnZero" + std::to_string(arg_count), 0,
             [&manager](Marshaler& m) { manager.impl_ReturnZero(m); },
             static_cast<u16>(arg_count * sizeof(u32))});
    }
    exports.entries.push_back({"_DWrite_GetSystemFontCollection", 0,
        [&manager](Marshaler& m) { manager.impl_GetSystemFontCollection(m); }, 12});
    exports.entries.push_back({"_DWrite_CreateCustomFontCollection", 0,
        [&manager](Marshaler& m) { manager.impl_CreateCustomFontCollection(m); }, 20});
    exports.entries.push_back({"_DWrite_RegisterFontCollectionLoader", 0,
        [&manager](Marshaler& m) { manager.impl_RegisterFontCollectionLoader(m); }, 8});
    exports.entries.push_back({"_DWrite_UnregisterFontCollectionLoader", 0,
        [&manager](Marshaler& m) { manager.impl_UnregisterFontCollectionLoader(m); }, 8});
    exports.entries.push_back({"_DWrite_CreateFontFileReference", 0,
        [&manager](Marshaler& m) { manager.impl_CreateFontFileReference(m); }, 16});
    exports.entries.push_back({"_DWrite_CreateCustomFontFileReference", 0,
        [&manager](Marshaler& m) {
            manager.impl_CreateCustomFontFileReference(m);
        }, 20});
    exports.entries.push_back({"_DWrite_RegisterFontFileLoader", 0,
        [&manager](Marshaler& m) { manager.impl_RegisterFontFileLoader(m); }, 8});
    exports.entries.push_back({"_DWrite_UnregisterFontFileLoader", 0,
        [&manager](Marshaler& m) { manager.impl_UnregisterFontFileLoader(m); }, 8});
    exports.entries.push_back({"_DWrite_GetGdiInterop", 0,
        [&manager](Marshaler& m) { manager.impl_GetGdiInterop(m); }, 8});
    exports.entries.push_back({"_DWrite_CreateRenderingParams", 0,
        [&manager](Marshaler& m) { manager.impl_CreateRenderingParams(m); }, 8});
    exports.entries.push_back({"_DWrite_CreateMonitorRenderingParams", 0,
        [&manager](Marshaler& m) {
            manager.impl_CreateMonitorRenderingParams(m);
        }, 12});
    exports.entries.push_back({"_DWrite_CreateTypography", 0,
        [&manager](Marshaler& m) { manager.impl_CreateTypography(m); }, 8});
    exports.entries.push_back({"_DWrite_CreateTextAnalyzer", 0,
        [&manager](Marshaler& m) { manager.impl_CreateTextAnalyzer(m); }, 8});
    exports.entries.push_back({"_DWrite_TypographyAddFontFeature", 0,
        [&manager](Marshaler& m) {
            manager.impl_TypographyAddFontFeature(m);
        }, 12});
    exports.entries.push_back({"_DWrite_TypographyGetFontFeatureCount", 0,
        [&manager](Marshaler& m) {
            manager.impl_TypographyGetFontFeatureCount(m);
        }, 4});
    exports.entries.push_back({"_DWrite_TypographyGetFontFeature", 0,
        [&manager](Marshaler& m) {
            manager.impl_TypographyGetFontFeature(m);
        }, 12});
    exports.entries.push_back({"_DWrite_AnalyzerAnalyzeScript", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalyzerAnalyzeScript(m);
        }, 20});
    exports.entries.push_back({"_DWrite_AnalyzerAnalyzeBidi", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalyzerAnalyzeBidi(m);
        }, 20});
    exports.entries.push_back({"_DWrite_AnalyzerAnalyzeNumberSubstitution", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalyzerAnalyzeNumberSubstitution(m);
        }, 20});
    exports.entries.push_back({"_DWrite_AnalyzerAnalyzeLineBreakpoints", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalyzerAnalyzeLineBreakpoints(m);
        }, 20});
    exports.entries.push_back({"_DWrite_AnalyzerGetGlyphs", 0,
        [&manager](Marshaler& m) { manager.impl_AnalyzerGetGlyphs(m); }, 72});
    exports.entries.push_back({"_DWrite_AnalyzerGetGlyphPlacements", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalyzerGetGlyphPlacements(m);
        }, 76});
    exports.entries.push_back(
        {"_DWrite_AnalyzerGetGdiCompatibleGlyphPlacements", 0,
         [&manager](Marshaler& m) {
             manager.impl_AnalyzerGetGdiCompatibleGlyphPlacements(m);
         },
         88});
    exports.entries.push_back({"_DWrite_RenderingParamsGetGamma", 0,
        [&manager](Marshaler& m) {
            manager.impl_RenderingParamsGetGamma(m);
        }, 4});
    exports.entries.push_back({"_DWrite_RenderingParamsGetEnhancedContrast", 0,
        [&manager](Marshaler& m) {
            manager.impl_RenderingParamsGetEnhancedContrast(m);
        }, 4});
    exports.entries.push_back({"_DWrite_RenderingParamsGetClearTypeLevel", 0,
        [&manager](Marshaler& m) {
            manager.impl_RenderingParamsGetClearTypeLevel(m);
        }, 4});
    exports.entries.push_back({"_DWrite_RenderingParamsGetPixelGeometry", 0,
        [&manager](Marshaler& m) {
            manager.impl_RenderingParamsGetPixelGeometry(m);
        }, 4});
    exports.entries.push_back({"_DWrite_RenderingParamsGetRenderingMode", 0,
        [&manager](Marshaler& m) {
            manager.impl_RenderingParamsGetRenderingMode(m);
        }, 4});
    // IDWriteGdiInterop slots 3-5: x86 stdcall bytes include `this`.
    exports.entries.push_back({"_DWrite_GdiCreateFontFromLOGFONT", 0,
        [&manager](Marshaler& m) {
            manager.impl_GdiCreateFontFromLogfont(m);
        }, 12});
    exports.entries.push_back({"_DWrite_GdiConvertFontToLOGFONT", 0,
        [&manager](Marshaler& m) {
            manager.impl_GdiConvertFontToLogfont(m);
        }, 16});
    exports.entries.push_back({"_DWrite_GdiConvertFontFaceToLOGFONT", 0,
        [&manager](Marshaler& m) {
            manager.impl_GdiConvertFontFaceToLogfont(m);
        }, 12});
    exports.entries.push_back({"_DWrite_GdiCreateFontFaceFromHdc", 0,
        [&manager](Marshaler& m) {
            manager.impl_GdiCreateFontFaceFromHdc(m);
        }, 12});
    exports.entries.push_back({"_DWrite_CreateGlyphRunAnalysis", 0,
        [&manager](Marshaler& m) {
            manager.impl_CreateGlyphRunAnalysis(m);
        }, 36});
    exports.entries.push_back({"_DWrite_CreateGlyphRunAnalysis2", 0,
        [&manager](Marshaler& m) {
            manager.impl_CreateGlyphRunAnalysis2(m);
        }, 40});
    exports.entries.push_back({"_DWrite_AnalysisGetAlphaTextureBounds", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalysisGetAlphaTextureBounds(m);
        }, 12});
    exports.entries.push_back({"_DWrite_AnalysisCreateAlphaTexture", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalysisCreateAlphaTexture(m);
        }, 20});
    exports.entries.push_back({"_DWrite_AnalysisGetAlphaBlendParams", 0,
        [&manager](Marshaler& m) {
            manager.impl_AnalysisGetAlphaBlendParams(m);
        }, 20});
    exports.entries.push_back({"_DWrite_GetSystemFontFallback", 0,
        [&manager](Marshaler& m) { manager.impl_GetSystemFontFallback(m); }, 8});
    exports.entries.push_back({"_DWrite_CreateFontFallbackBuilder", 0,
        [&manager](Marshaler& m) {
            manager.impl_CreateFontFallbackBuilder(m);
        }, 8});
    exports.entries.push_back({"_DWrite_GetSystemFontSet", 0,
        [&manager](Marshaler& m) { manager.impl_GetSystemFontSet(m); }, 8});
    exports.entries.push_back({"_DWrite_GetFontDownloadQueue", 0,
        [&manager](Marshaler& m) { manager.impl_GetFontDownloadQueue(m); }, 8});
    exports.entries.push_back({"_DWrite_FontSetGetFontCount", 0,
        [&manager](Marshaler& m) { manager.impl_FontSetGetFontCount(m); }, 4});
    exports.entries.push_back({"_DWrite_FontSetGetMatchingFontsFamily", 0,
        [&manager](Marshaler& m) {
            manager.impl_FontSetGetMatchingFontsFamily(m);
        }, 24});
    exports.entries.push_back({"_DWrite_FontSetGetMatchingFontsProperties", 0,
        [&manager](Marshaler& m) {
            manager.impl_FontSetGetMatchingFontsProperties(m);
        }, 16});
    exports.entries.push_back({"_DWrite_FallbackBuilderAddMapping", 0,
        [&manager](Marshaler& m) {
            manager.impl_FallbackBuilderAddMapping(m);
        }, 36});
    exports.entries.push_back({"_DWrite_FallbackBuilderAddMappings", 0,
        [&manager](Marshaler& m) {
            manager.impl_FallbackBuilderAddMappings(m);
        }, 8});
    exports.entries.push_back({"_DWrite_FallbackBuilderCreateFontFallback", 0,
        [&manager](Marshaler& m) {
            manager.impl_FallbackBuilderCreateFontFallback(m);
        }, 8});
    exports.entries.push_back({"_DWrite_DownloadQueueIsEmpty", 0,
        [&manager](Marshaler& m) { manager.impl_DownloadQueueIsEmpty(m); }, 4});
    exports.entries.push_back({"_DWrite_DownloadQueueBeginDownload", 0,
        [&manager](Marshaler& m) {
            manager.impl_DownloadQueueBeginDownload(m);
        }, 8});
    exports.entries.push_back({"_DWrite_DownloadQueueGetGenerationCount", 0,
        [&manager](Marshaler& m) {
            manager.impl_DownloadQueueGetGenerationCount(m);
        }, 4});
    exports.entries.push_back({"_DWrite_CreateNumberSubstitution", 0,
        [&manager](Marshaler& m) { manager.impl_CreateNumberSubstitution(m); }, 20});
    exports.entries.push_back({"_DWrite_FallbackMapCharacters", 0,
        [&manager](Marshaler& m) { manager.impl_FallbackMapCharacters(m); }, 48});
    exports.entries.push_back({"_DWrite_GetFontFamilyCount", 0,
        [&manager](Marshaler& m) { manager.impl_GetFontFamilyCount(m); }, 4});
    exports.entries.push_back({"_DWrite_GetFontFamily", 0,
        [&manager](Marshaler& m) { manager.impl_GetFontFamily(m); }, 12});
    exports.entries.push_back({"_DWrite_FindFamilyName", 0,
        [&manager](Marshaler& m) { manager.impl_FindFamilyName(m); }, 16});
    exports.entries.push_back({"_DWrite_FamilyGetFontCount", 0,
        [&manager](Marshaler& m) { manager.impl_FamilyGetFontCount(m); }, 4});
    exports.entries.push_back({"_DWrite_FamilyGetFont", 0,
        [&manager](Marshaler& m) { manager.impl_FamilyGetFont(m); }, 12});
    exports.entries.push_back({"_DWrite_FamilyGetNames", 0,
        [&manager](Marshaler& m) { manager.impl_FamilyGetNames(m); }, 8});
    exports.entries.push_back({"_DWrite_GetFirstMatchingFont", 0,
        [&manager](Marshaler& m) { manager.impl_GetFirstMatchingFont(m); }, 20});
    exports.entries.push_back({"_DWrite_FontGetFamily", 0,
        [&manager](Marshaler& m) { manager.impl_FontGetFamily(m); }, 8});
    exports.entries.push_back({"_DWrite_FontGetWeight", 0,
        [&manager](Marshaler& m) { manager.impl_FontGetWeight(m); }, 4});
    exports.entries.push_back({"_DWrite_FontGetStretch", 0,
        [&manager](Marshaler& m) { manager.impl_FontGetStretch(m); }, 4});
    exports.entries.push_back({"_DWrite_FontGetStyle", 0,
        [&manager](Marshaler& m) { manager.impl_FontGetStyle(m); }, 4});
    exports.entries.push_back({"_DWrite_FontIsSymbolFont", 0,
        [&manager](Marshaler& m) { manager.impl_FontIsSymbolFont(m); }, 4});
    exports.entries.push_back({"_DWrite_FontGetFaceNames", 0,
        [&manager](Marshaler& m) { manager.impl_FontGetFaceNames(m); }, 8});
    exports.entries.push_back({"_DWrite_FontGetSimulations", 0,
        [&manager](Marshaler& m) { manager.impl_FontGetSimulations(m); }, 4});
    exports.entries.push_back({"_DWrite_FontGetMetrics", 0,
        [&manager](Marshaler& m) { manager.impl_FontGetMetrics(m); }, 8});
    exports.entries.push_back({"_DWrite_FontCreateFontFace", 0,
        [&manager](Marshaler& m) { manager.impl_FontCreateFontFace(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetType", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetType(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceGetFiles", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetFiles(m); }, 12});
    exports.entries.push_back({"_DWrite_FaceGetIndex", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetIndex(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceGetSimulations", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetSimulations(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceIsSymbolFont", 0,
        [&manager](Marshaler& m) { manager.impl_FaceIsSymbolFont(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceGetMetrics", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetMetrics(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetGlyphCount", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetGlyphCount(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceGetDesignGlyphMetrics", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetDesignGlyphMetrics(m);
        }, 20});
    exports.entries.push_back({"_DWrite_FaceGetGdiCompatibleGlyphMetrics", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetGdiCompatibleGlyphMetrics(m);
        }, 36});
    exports.entries.push_back({"_DWrite_FaceGetGlyphIndices", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetGlyphIndices(m); }, 16});
    exports.entries.push_back({"_DWrite_FaceTryGetFontTable", 0,
        [&manager](Marshaler& m) { manager.impl_FaceTryGetFontTable(m); }, 24});
    exports.entries.push_back({"_DWrite_FaceReleaseFontTable", 0,
        [&manager](Marshaler& m) { manager.impl_FaceReleaseFontTable(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetGlyphRunOutline", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetGlyphRunOutline(m);
        }, 36});
    exports.entries.push_back({"_DWrite_FaceGetMetrics1", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetMetrics1(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetCaretMetrics", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetCaretMetrics(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetUnicodeRanges", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetUnicodeRanges(m); }, 16});
    exports.entries.push_back({"_DWrite_FaceGetDesignGlyphAdvances", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetDesignGlyphAdvances(m);
        }, 20});
    exports.entries.push_back({"_DWrite_FaceGetKerningPairAdjustments", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetKerningPairAdjustments(m);
        }, 16});
    exports.entries.push_back({"_DWrite_FaceGetRecommendedRenderingMode", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetRecommendedRenderingMode(m);
        }, 24});
    exports.entries.push_back({"_DWrite_FaceGetRecommendedRenderingMode1", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetRecommendedRenderingMode1(m);
        }, 36});
    exports.entries.push_back({"_DWrite_FaceGetRecommendedRenderingMode2", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetRecommendedRenderingMode2(m);
        }, 44});
    exports.entries.push_back({"_DWrite_FaceGetWeight", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetWeight(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceGetStretch", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetStretch(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceGetStyle", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetStyle(m); }, 4});
    exports.entries.push_back({"_DWrite_FaceGetPanose", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetPanose(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetFamilyNames", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetFamilyNames(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetFaceNames", 0,
        [&manager](Marshaler& m) { manager.impl_FaceGetFaceNames(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceGetFamilyNamesLocale", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetFamilyNamesLocale(m);
        }, 12});
    exports.entries.push_back({"_DWrite_FaceGetFaceNamesLocale", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetFaceNamesLocale(m);
        }, 12});
    exports.entries.push_back({"_DWrite_FaceGetInformationalStrings", 0,
        [&manager](Marshaler& m) {
            manager.impl_FaceGetInformationalStrings(m);
        }, 16});
    exports.entries.push_back({"_DWrite_FaceHasCharacter", 0,
        [&manager](Marshaler& m) { manager.impl_FaceHasCharacter(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceIsLocal", 0,
        [&manager](Marshaler& m) { manager.impl_FaceIsLocal(m); }, 8});
    exports.entries.push_back({"_DWrite_FaceAreLocal", 0,
        [&manager](Marshaler& m) { manager.impl_FaceAreLocal(m); }, 20});
    exports.entries.push_back({"_DWrite_FaceEquals", 0,
        [&manager](Marshaler& m) { manager.impl_FaceEquals(m); }, 8});
    exports.entries.push_back({"_DWrite_StringsGetCount", 0,
        [&manager](Marshaler& m) { manager.impl_StringsGetCount(m); }, 4});
    exports.entries.push_back({"_DWrite_StringsFindLocaleName", 0,
        [&manager](Marshaler& m) { manager.impl_StringsFindLocaleName(m); }, 16});
    exports.entries.push_back({"_DWrite_StringsGetLocaleNameLength", 0,
        [&manager](Marshaler& m) { manager.impl_StringsGetLocaleNameLength(m); }, 12});
    exports.entries.push_back({"_DWrite_StringsGetLocaleName", 0,
        [&manager](Marshaler& m) { manager.impl_StringsGetLocaleName(m); }, 16});
    exports.entries.push_back({"_DWrite_StringsGetStringLength", 0,
        [&manager](Marshaler& m) { manager.impl_StringsGetStringLength(m); }, 12});
    exports.entries.push_back({"_DWrite_StringsGetString", 0,
        [&manager](Marshaler& m) { manager.impl_StringsGetString(m); }, 16});
    exports.entries.push_back({"_DWrite_FileGetReferenceKey", 0,
        [&manager](Marshaler& m) { manager.impl_FileGetReferenceKey(m); }, 12});
    exports.entries.push_back({"_DWrite_FileGetLoader", 0,
        [&manager](Marshaler& m) { manager.impl_FileGetLoader(m); }, 8});
    exports.entries.push_back({"_DWrite_FileAnalyze", 0,
        [&manager](Marshaler& m) { manager.impl_FileAnalyze(m); }, 20});
    exports.entries.push_back({"_DWrite_LocalLoaderGetPathLength", 0,
        [&manager](Marshaler& m) { manager.impl_LocalLoaderGetPathLength(m); }, 16});
    exports.entries.push_back({"_DWrite_LocalLoaderGetPath", 0,
        [&manager](Marshaler& m) { manager.impl_LocalLoaderGetPath(m); }, 20});
    exports.entries.push_back({"_DWrite_LocalLoaderGetLastWriteTime", 0,
        [&manager](Marshaler& m) { manager.impl_LocalLoaderGetLastWriteTime(m); }, 16});
    exports.entries.push_back({"_DWrite_LocalLoaderCreateStreamFromKey", 0,
        [&manager](Marshaler& m) {
            manager.impl_LocalLoaderCreateStreamFromKey(m);
        }, 16});
    exports.entries.push_back({"_DWrite_StreamReadFileFragment", 0,
        [&manager](Marshaler& m) { manager.impl_StreamReadFileFragment(m); }, 28});
    exports.entries.push_back({"_DWrite_StreamReleaseFileFragment", 0,
        [&manager](Marshaler& m) {
            manager.impl_StreamReleaseFileFragment(m);
        }, 8});
    exports.entries.push_back({"_DWrite_StreamGetFileSize", 0,
        [&manager](Marshaler& m) { manager.impl_StreamGetFileSize(m); }, 8});
    exports.entries.push_back({"_DWrite_StreamGetLastWriteTime", 0,
        [&manager](Marshaler& m) { manager.impl_StreamGetLastWriteTime(m); }, 8});
    exports.entries.push_back({"_DWrite_CreateTextFormat", 0,
        [&manager](Marshaler& m) { manager.impl_CreateTextFormat(m); }, 36});
    exports.entries.push_back({"_DWrite_CreateTextLayout", 0,
        [&manager](Marshaler& m) { manager.impl_CreateTextLayout(m); }, 28});
    exports.entries.push_back({"_DWrite_CreateGdiCompatibleTextLayout", 0,
        [&manager](Marshaler& m) { manager.impl_CreateGdiCompatibleTextLayout(m); }, 40});
    exports.entries.push_back({"_DWrite_CreateEllipsisTrimmingSign", 0,
        [&manager](Marshaler& m) { manager.impl_CreateEllipsisTrimmingSign(m); }, 12});
    // Text objects: one thunk per slot (IUnknown slots use the shared ones),
    // stdcall byte counts from dwrite_text_contract.inc.
    const auto add_text = [&](const char* prefix, const auto& methods,
                              DWriteManager::TextInterface kind) {
        for (u32 slot = 3; slot < std::size(methods); ++slot) {
            exports.entries.push_back({std::string(prefix) + std::to_string(slot), 0,
                [&manager, kind, slot](Marshaler& m) { manager.text_method(m, kind, slot); },
                methods[slot].bytes});
        }
    };
    add_text("_DWrite_TextFormat_", dwrite_text_contract::text_format_methods,
             DWriteManager::TextInterface::Format);
    add_text("_DWrite_TextLayout_", dwrite_text_contract::text_layout_methods,
             DWriteManager::TextInterface::Layout);
    add_text("_DWrite_InlineObject_", dwrite_text_contract::inline_object_methods,
             DWriteManager::TextInterface::InlineObject);
    for (auto& entry : exports.entries) {
        ApiHandler handler = std::move(entry.handler);
        entry.handler = [&manager, handler = std::move(handler)](
                            Marshaler& m) {
            manager.invoke_serialized(handler, m);
        };
    }
    if (dwrite_call_trace_enabled()) {
        for (auto& entry : exports.entries) {
            entry.handler = [name = entry.name, handler = entry.handler](
                                Marshaler& m) {
                dwrite_diag_printf("[dwrite-call] %s self=0x%llx\n",
                                   name.c_str(),
                                   static_cast<unsigned long long>(
                                       m.arg_ptr(0)));
                handler(m);
            };
        }
    }
    return exports;
}

} // namespace vitreo::winapi

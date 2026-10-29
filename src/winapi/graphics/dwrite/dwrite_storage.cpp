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

// IDWriteFactory3 is the DirectWrite factory revision shipped with the
// Windows 10 surface Vitreo reports to guests. Its vtable contains the 24
// IDWriteFactory slots, two IDWriteFactory1 slots, five IDWriteFactory2 slots,
// and nine IDWriteFactory3 slots.
constexpr u32 FACTORY_VTABLE_SLOTS = 40;
constexpr u32 COLLECTION_VTABLE_SLOTS = 7;
constexpr u32 FAMILY_VTABLE_SLOTS = 9;
constexpr u32 FONT_VTABLE_SLOTS = 14;
// IDWriteFontFace through IDWriteFontFace7 (dwrite.h / dwrite_1.h /
// dwrite_2.h / dwrite_3.h). Base face is 18 slots; Face1+12, Face2+5,
// Face3+14, Face4+4, Face5+5, Face6+2, Face7+2.
constexpr u32 FONT_FACE_VTABLE_SLOTS = 62;
constexpr u32 FALLBACK_VTABLE_SLOTS = 4;
constexpr u32 FALLBACK_BUILDER_VTABLE_SLOTS = 6;
constexpr u32 FONT_SET_VTABLE_SLOTS = 13;
constexpr u32 FONT_DOWNLOAD_QUEUE_VTABLE_SLOTS = 9;
constexpr u32 NUMBER_SUBSTITUTION_VTABLE_SLOTS = 3;
constexpr u32 LOCALIZED_STRINGS_VTABLE_SLOTS = 9;
constexpr u32 FONT_FILE_VTABLE_SLOTS = 6;
constexpr u32 LOCAL_LOADER_VTABLE_SLOTS = 7;
constexpr u32 FONT_FILE_STREAM_VTABLE_SLOTS = 7;
constexpr u32 RENDERING_PARAMS_VTABLE_SLOTS = 8;
constexpr u32 TYPOGRAPHY_VTABLE_SLOTS = 6;
constexpr u32 TEXT_ANALYZER_VTABLE_SLOTS = 10;
constexpr u32 GDI_INTEROP_VTABLE_SLOTS = 8;
constexpr u32 GLYPH_RUN_ANALYSIS_VTABLE_SLOTS = 6;
constexpr usize STORAGE_SIZE = 0x10000;

// Total x86 stack arguments for each IDWriteFactory method, including `this`.
// Slots 0-3 are implemented separately. The remaining values come directly
// from the public IDWriteFactory, IDWriteFactory1, IDWriteFactory2, and
// IDWriteFactory3 signatures in dwrite.h, dwrite_1.h, dwrite_2.h, and
// dwrite_3.h. The counts include `this` because x86 COM methods are stdcall.
constexpr std::array<u8, FACTORY_VTABLE_SLOTS> FACTORY_METHOD_ARG_COUNTS{
    3, 1, 1, 3, 5, 2, 2, 4, 5, 7, 2, 3,
    7, 2, 2, 9, 2, 2, 7, 10, 3, 2, 5, 9,
    // IDWriteFactory1
    3, 8,
    // IDWriteFactory2
    2, 2, 9, 9, 10,
    // IDWriteFactory3
    10, 9, 6, 5, 2, 2, 3, 4, 2,
};

constexpr std::array<u8, FAMILY_VTABLE_SLOTS> FAMILY_METHOD_ARG_COUNTS{
    3, 1, 1, 2, 1, 3, 2, 5, 5,
};

} // namespace

bool DWriteManager::ensure_storage() {
    std::lock_guard lock(m_storage_mutex);
    if (m_storage_ready) return true;
    if (m_region_base == 0) {
        for (GuestAddr minimum = 0x60000000ull; minimum != 0;) {
            const GuestAddr base = m_memory.find_free_gap(minimum, STORAGE_SIZE);
            if (base == 0) {
                if (dwrite_diag_enabled()) {
                    dwrite_diag_printf(
                                 "[dwrite-diag] storage: no free guest region\n");
                }
                return false;
            }
            if (m_memory.map_region(
                    base, STORAGE_SIZE, MemoryProtection::ReadWrite).is_ok()) {
                m_region_base = base;
                m_factory_vtable = base;
                m_collection_vtable = base + 0x200;
                m_family_vtable = base + 0x400;
                m_font_vtable = base + 0x600;
                m_face_vtable = base + 0x800;
                m_fallback_vtable = base + 0xA00;
                m_number_substitution_vtable = base + 0xB00;
                m_callback_scratch = base + 0xC00;
                m_localized_strings_vtable = base + 0xD00;
                m_font_file_vtable = base + 0xE00;
                m_local_loader_vtable = base + 0xF00;
                m_gdi_interop_vtable = base + 0x1000;
                m_glyph_run_analysis_vtable = base + 0x1100;
                m_font_file_stream_vtable = base + 0x1200;
                m_rendering_params_vtable = base + 0x1280;
                m_typography_vtable = base + 0x1300;
                m_text_analyzer_vtable = base + 0x1380;
                m_font_set_vtable = base + 0x1400;
                m_fallback_builder_vtable = base + 0x1500;
                m_font_download_queue_vtable = base + 0x1580;
                // IDWriteTextFormat 28, IDWriteTextLayout 67 and
                // IDWriteInlineObject 7 slots (dwrite_text_contract.inc).
                m_text_format_vtable = base + 0x1600;
                m_text_layout_vtable = base + 0x1700;
                m_trimming_sign_vtable = base + 0x1A00;
                m_next_object = base + 0x1C00;
                break;
            }
            minimum = base + STORAGE_SIZE;
        }
    }
    if (m_region_base == 0) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] storage: guest mapping failed\n");
        }
        return false;
    }

    const usize pointer_size = m_is_64bit ? 8 : 4;
    const auto install_method =
        [&](const char* interface_name, GuestAddr vtable, u32 slot,
            GuestAddr thunk) {
            const bool ok = thunk != 0 &&
                write_pointer(vtable + slot * pointer_size, thunk);
            if (!ok && dwrite_diag_enabled()) {
                std::fprintf(
                    stderr,
                    "[dwrite-diag] storage: %s slot=%u thunk=0x%llx failed\n",
                    interface_name, slot,
                    static_cast<unsigned long long>(thunk));
            }
            return ok;
        };
    for (u32 slot = 0; slot < FACTORY_VTABLE_SLOTS; ++slot) {
        GuestAddr thunk = not_impl_thunk(FACTORY_METHOD_ARG_COUNTS[slot]);
        if (slot == 0) thunk = m_method_thunks.query_interface;
        if (slot == 1) thunk = m_method_thunks.add_ref;
        if (slot == 2) thunk = m_method_thunks.release;
        if (slot == 3) thunk = m_method_thunks.get_system_font_collection;
        if (slot == 4) thunk = m_method_thunks.create_custom_font_collection;
        if (slot == 5) thunk = m_method_thunks.register_font_collection_loader;
        if (slot == 6) thunk = m_method_thunks.unregister_font_collection_loader;
        if (slot == 7) thunk = m_method_thunks.create_font_file_reference;
        if (slot == 8) {
            thunk = m_method_thunks.create_custom_font_file_reference;
        }
        if (slot == 10) thunk = m_method_thunks.create_rendering_params;
        if (slot == 11) thunk = m_method_thunks.create_monitor_rendering_params;
        // Text factory methods keep the unbound default when not bound.
        const auto bound = [&](GuestAddr text_thunk) { if (text_thunk != 0) thunk = text_thunk; };
        if (slot == 15) bound(m_method_thunks.create_text_format);
        if (slot == 18) bound(m_method_thunks.create_text_layout);
        if (slot == 19) bound(m_method_thunks.create_gdi_compatible_text_layout);
        if (slot == 20) bound(m_method_thunks.create_ellipsis_trimming_sign);
        if (slot == 13) thunk = m_method_thunks.register_font_file_loader;
        if (slot == 14) thunk = m_method_thunks.unregister_font_file_loader;
        if (slot == 16) thunk = m_method_thunks.create_typography;
        if (slot == 17) thunk = m_method_thunks.get_gdi_interop;
        if (slot == 21) thunk = m_method_thunks.create_text_analyzer;
        if (slot == 22) thunk = m_method_thunks.create_number_substitution;
        if (slot == 23) thunk = m_method_thunks.create_glyph_run_analysis;
        if (slot == 26) thunk = m_method_thunks.get_system_font_fallback;
        if (slot == 27) thunk = m_method_thunks.create_font_fallback_builder;
        // IDWriteFactory2::CreateGlyphRunAnalysis
        if (slot == 30) thunk = m_method_thunks.create_glyph_run_analysis2;
        if (slot == 35) thunk = m_method_thunks.get_system_font_set;
        if (slot == 39) thunk = m_method_thunks.get_font_download_queue;
        if (!install_method("factory", m_factory_vtable, slot, thunk))
            return false;
    }
    const std::array<GuestAddr, GDI_INTEROP_VTABLE_SLOTS> gdi_interop_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.gdi_create_font_from_logfont,
        m_method_thunks.gdi_convert_font_to_logfont,
        m_method_thunks.gdi_convert_font_face_to_logfont,
        m_method_thunks.gdi_create_font_face_from_hdc,
        not_impl_thunk(5), // CreateBitmapRenderTarget
    };
    for (u32 slot = 0; slot < GDI_INTEROP_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "gdi-interop", m_gdi_interop_vtable, slot,
                gdi_interop_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, GLYPH_RUN_ANALYSIS_VTABLE_SLOTS>
        glyph_run_analysis_methods{
            m_method_thunks.query_interface,
            m_method_thunks.add_ref,
            m_method_thunks.release,
            m_method_thunks.analysis_get_alpha_texture_bounds,
            m_method_thunks.analysis_create_alpha_texture,
            m_method_thunks.analysis_get_alpha_blend_params,
        };
    for (u32 slot = 0; slot < GLYPH_RUN_ANALYSIS_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "glyph-run-analysis", m_glyph_run_analysis_vtable, slot,
                glyph_run_analysis_methods[slot])) {
            return false;
        }
    }
    for (u32 slot = 0; slot < COLLECTION_VTABLE_SLOTS; ++slot) {
        // GetFontFromFontFace (slot 6) has three x86 stack arguments.
        GuestAddr thunk = not_impl_thunk(3);
        if (slot == 0) thunk = m_method_thunks.query_interface;
        if (slot == 1) thunk = m_method_thunks.add_ref;
        if (slot == 2) thunk = m_method_thunks.release;
        if (slot == 3) thunk = m_method_thunks.get_font_family_count;
        if (slot == 4) thunk = m_method_thunks.get_font_family;
        if (slot == 5) thunk = m_method_thunks.find_family_name;
        if (!install_method("collection", m_collection_vtable, slot, thunk))
            return false;
    }
    for (u32 slot = 0; slot < FAMILY_VTABLE_SLOTS; ++slot) {
        GuestAddr thunk = not_impl_thunk(FAMILY_METHOD_ARG_COUNTS[slot]);
        if (slot == 0) thunk = m_method_thunks.query_interface;
        if (slot == 1) thunk = m_method_thunks.add_ref;
        if (slot == 2) thunk = m_method_thunks.release;
        if (slot == 4) thunk = m_method_thunks.family_get_font_count;
        if (slot == 5) thunk = m_method_thunks.family_get_font;
        if (slot == 6) thunk = m_method_thunks.family_get_names;
        if (slot == 7) thunk = m_method_thunks.get_first_matching_font;
        if (!install_method("family", m_family_vtable, slot, thunk))
            return false;
    }
    const std::array<GuestAddr, FONT_VTABLE_SLOTS> font_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.font_get_family,
        m_method_thunks.font_get_weight,
        m_method_thunks.font_get_stretch,
        m_method_thunks.font_get_style,
        m_method_thunks.font_is_symbol_font,
        m_method_thunks.font_get_face_names,
        not_impl_thunk(4),
        m_method_thunks.font_get_simulations,
        m_method_thunks.font_get_metrics,
        not_impl_thunk(3),
        m_method_thunks.font_create_font_face,
    };
    for (u32 slot = 0; slot < FONT_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font", m_font_vtable, slot, font_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, FONT_FACE_VTABLE_SLOTS> face_methods{
        // IDWriteFontFace
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.face_get_type,
        m_method_thunks.face_get_files,
        m_method_thunks.face_get_index,
        m_method_thunks.face_get_simulations,
        m_method_thunks.face_is_symbol_font,
        m_method_thunks.face_get_metrics,
        m_method_thunks.face_get_glyph_count,
        m_method_thunks.face_get_design_glyph_metrics,
        m_method_thunks.face_get_glyph_indices,
        m_method_thunks.face_try_get_font_table,
        m_method_thunks.face_release_font_table,
        m_method_thunks.face_get_glyph_run_outline,
        m_method_thunks.face_get_recommended_rendering_mode, // argc 6
        not_impl_thunk(5), // GetGdiCompatibleMetrics
        m_method_thunks.face_get_gdi_compatible_glyph_metrics,
        // IDWriteFontFace1
        m_method_thunks.face_get_metrics1,
        not_impl_thunk(5), // GetGdiCompatibleMetrics (metrics1)
        m_method_thunks.face_get_caret_metrics,
        m_method_thunks.face_get_unicode_ranges,
        return_zero_thunk(1), // IsMonospacedFont
        m_method_thunks.face_get_design_glyph_advances,
        not_impl_thunk(9), // GetGdiCompatibleGlyphAdvances
        m_method_thunks.face_get_kerning_pair_adjustments,
        return_zero_thunk(1), // HasKerningPairs
        m_method_thunks.face_get_recommended_rendering_mode1,
        not_impl_thunk(4), // GetVerticalGlyphVariants
        return_zero_thunk(1), // HasVerticalGlyphVariants
        // IDWriteFontFace2
        return_zero_thunk(1), // IsColorFont
        return_zero_thunk(1), // GetColorPaletteCount
        return_zero_thunk(1), // GetPaletteEntryCount
        not_impl_thunk(5), // GetPaletteEntries
        m_method_thunks.face_get_recommended_rendering_mode2,
        // IDWriteFontFace3
        not_impl_thunk(2), // GetFontFaceReference
        m_method_thunks.face_get_panose,
        m_method_thunks.face_get_weight,
        m_method_thunks.face_get_stretch,
        m_method_thunks.face_get_style,
        m_method_thunks.face_get_family_names,
        m_method_thunks.face_get_face_names,
        m_method_thunks.face_get_informational_strings,
        m_method_thunks.face_has_character,
        m_method_thunks.face_get_recommended_rendering_mode2,
        m_method_thunks.face_is_local, // IsCharacterLocal
        m_method_thunks.face_is_local, // IsGlyphLocal
        m_method_thunks.face_are_local, // AreCharactersLocal
        m_method_thunks.face_are_local, // AreGlyphsLocal
        // IDWriteFontFace4
        return_zero_thunk(1), // GetGlyphImageFormats()
        not_impl_thunk(5), // GetGlyphImageFormats(glyph,...)
        not_impl_thunk(6), // GetGlyphImageData
        not_impl_thunk(2), // ReleaseGlyphImageData
        // IDWriteFontFace5
        return_zero_thunk(1), // GetFontAxisValueCount
        not_impl_thunk(3), // GetFontAxisValues
        return_zero_thunk(1), // HasVariations
        not_impl_thunk(2), // GetFontResource
        m_method_thunks.face_equals,
        // IDWriteFontFace6
        m_method_thunks.face_get_family_names_locale,
        m_method_thunks.face_get_face_names_locale,
        // IDWriteFontFace7
        return_zero_thunk(2), // GetPaintFeatureLevel
        not_impl_thunk(4), // CreatePaintReader
    };
    for (u32 slot = 0; slot < FONT_FACE_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font-face", m_face_vtable, slot, face_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, FALLBACK_VTABLE_SLOTS> fallback_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.fallback_map_characters,
    };
    for (u32 slot = 0; slot < FALLBACK_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font-fallback", m_fallback_vtable, slot,
                fallback_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, FALLBACK_BUILDER_VTABLE_SLOTS>
        fallback_builder_methods{
            m_method_thunks.query_interface,
            m_method_thunks.add_ref,
            m_method_thunks.release,
            m_method_thunks.fallback_builder_add_mapping,
            m_method_thunks.fallback_builder_add_mappings,
            m_method_thunks.fallback_builder_create_font_fallback,
        };
    for (u32 slot = 0; slot < FALLBACK_BUILDER_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font-fallback-builder", m_fallback_builder_vtable, slot,
                fallback_builder_methods[slot])) {
            return false;
        }
    }
    // IDWriteFontSet vtable order from dwrite_3.h (not alphabetical Learn docs).
    const std::array<GuestAddr, FONT_SET_VTABLE_SLOTS> font_set_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.font_set_get_font_count,
        not_impl_thunk(3), // GetFontFaceReference
        not_impl_thunk(4), // FindFontFaceReference
        not_impl_thunk(4), // FindFontFace
        not_impl_thunk(5), // GetPropertyValues(listIndex,...)
        not_impl_thunk(4), // GetPropertyValues(propertyID, locale,...)
        not_impl_thunk(3), // GetPropertyValues(propertyID,...)
        not_impl_thunk(3), // GetPropertyOccurrenceCount
        m_method_thunks.font_set_get_matching_fonts_family,
        m_method_thunks.font_set_get_matching_fonts_properties,
    };
    for (u32 slot = 0; slot < FONT_SET_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font-set", m_font_set_vtable, slot, font_set_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, FONT_DOWNLOAD_QUEUE_VTABLE_SLOTS>
        download_queue_methods{
            m_method_thunks.query_interface,
            m_method_thunks.add_ref,
            m_method_thunks.release,
            not_impl_thunk(3), // AddListener
            not_impl_thunk(2), // RemoveListener
            m_method_thunks.download_queue_is_empty,
            m_method_thunks.download_queue_begin_download,
            not_impl_thunk(1), // CancelDownload
            m_method_thunks.download_queue_get_generation_count,
        };
    for (u32 slot = 0; slot < FONT_DOWNLOAD_QUEUE_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font-download-queue", m_font_download_queue_vtable, slot,
                download_queue_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, NUMBER_SUBSTITUTION_VTABLE_SLOTS>
        number_substitution_methods{
            m_method_thunks.query_interface,
            m_method_thunks.add_ref,
            m_method_thunks.release,
        };
    for (u32 slot = 0; slot < NUMBER_SUBSTITUTION_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "number-substitution", m_number_substitution_vtable, slot,
                number_substitution_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, LOCALIZED_STRINGS_VTABLE_SLOTS>
        localized_strings_methods{
            m_method_thunks.query_interface,
            m_method_thunks.add_ref,
            m_method_thunks.release,
            m_method_thunks.strings_get_count,
            m_method_thunks.strings_find_locale_name,
            m_method_thunks.strings_get_locale_name_length,
            m_method_thunks.strings_get_locale_name,
            m_method_thunks.strings_get_string_length,
            m_method_thunks.strings_get_string,
        };
    for (u32 slot = 0; slot < LOCALIZED_STRINGS_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "localized-strings", m_localized_strings_vtable, slot,
                localized_strings_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, FONT_FILE_VTABLE_SLOTS> file_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.file_get_reference_key,
        m_method_thunks.file_get_loader,
        m_method_thunks.file_analyze,
    };
    for (u32 slot = 0; slot < FONT_FILE_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font-file", m_font_file_vtable, slot,
                file_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, LOCAL_LOADER_VTABLE_SLOTS> loader_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.local_loader_create_stream_from_key,
        m_method_thunks.local_loader_get_path_length,
        m_method_thunks.local_loader_get_path,
        m_method_thunks.local_loader_get_last_write_time,
    };
    for (u32 slot = 0; slot < LOCAL_LOADER_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "local-loader", m_local_loader_vtable, slot,
                loader_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, FONT_FILE_STREAM_VTABLE_SLOTS> stream_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.stream_read_file_fragment,
        m_method_thunks.stream_release_file_fragment,
        m_method_thunks.stream_get_file_size,
        m_method_thunks.stream_get_last_write_time,
    };
    for (u32 slot = 0; slot < FONT_FILE_STREAM_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "font-file-stream", m_font_file_stream_vtable, slot,
                stream_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, RENDERING_PARAMS_VTABLE_SLOTS>
        rendering_params_methods{
            m_method_thunks.query_interface,
            m_method_thunks.add_ref,
            m_method_thunks.release,
            m_method_thunks.rendering_params_get_gamma,
            m_method_thunks.rendering_params_get_enhanced_contrast,
            m_method_thunks.rendering_params_get_clear_type_level,
            m_method_thunks.rendering_params_get_pixel_geometry,
            m_method_thunks.rendering_params_get_rendering_mode,
        };
    for (u32 slot = 0; slot < RENDERING_PARAMS_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "rendering-params", m_rendering_params_vtable, slot,
                rendering_params_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, TYPOGRAPHY_VTABLE_SLOTS> typography_methods{
        m_method_thunks.query_interface,
        m_method_thunks.add_ref,
        m_method_thunks.release,
        m_method_thunks.typography_add_font_feature,
        m_method_thunks.typography_get_font_feature_count,
        m_method_thunks.typography_get_font_feature,
    };
    for (u32 slot = 0; slot < TYPOGRAPHY_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "typography", m_typography_vtable, slot,
                typography_methods[slot])) {
            return false;
        }
    }
    const std::array<GuestAddr, TEXT_ANALYZER_VTABLE_SLOTS>
        text_analyzer_methods{
            m_method_thunks.query_interface,
            m_method_thunks.add_ref,
            m_method_thunks.release,
            m_method_thunks.analyzer_analyze_script,
            m_method_thunks.analyzer_analyze_bidi,
            m_method_thunks.analyzer_analyze_number_substitution,
            m_method_thunks.analyzer_analyze_line_breakpoints,
            m_method_thunks.analyzer_get_glyphs,
            m_method_thunks.analyzer_get_glyph_placements,
            m_method_thunks.analyzer_get_gdi_compatible_glyph_placements,
        };
    for (u32 slot = 0; slot < TEXT_ANALYZER_VTABLE_SLOTS; ++slot) {
        if (!install_method(
                "text-analyzer", m_text_analyzer_vtable, slot,
                text_analyzer_methods[slot])) {
            return false;
        }
    }
    // Text objects: IUnknown is the shared object protocol; every other slot
    // dispatches by slot number through DWriteManager::text_method.
    const auto install_text = [&](const char* name, GuestAddr vtable, const auto& thunks,
                                  const auto& methods) {
        for (u32 slot = 0; slot < thunks.size(); ++slot) {
            // An unbound slot answers like every other unbound DWrite slot,
            // with the method's exact x86 stack footprint.
            GuestAddr thunk = thunks[slot] != 0 ? thunks[slot] : not_impl_thunk(methods[slot].bytes / 4);
            if (slot == 0) thunk = m_method_thunks.query_interface;
            if (slot == 1) thunk = m_method_thunks.add_ref;
            if (slot == 2) thunk = m_method_thunks.release;
            if (!install_method(name, vtable, slot, thunk)) return false;
        }
        return true;
    };
    if (!install_text("text-format", m_text_format_vtable, m_method_thunks.text_format,
                      dwrite_text_contract::text_format_methods) ||
        !install_text("text-layout", m_text_layout_vtable, m_method_thunks.text_layout,
                      dwrite_text_contract::text_layout_methods) ||
        !install_text("trimming-sign", m_trimming_sign_vtable, m_method_thunks.inline_object,
                      dwrite_text_contract::inline_object_methods)) {
        return false;
    }
    m_font_families = dwrite::enumerate_system_font_families();
    m_storage_ready = true;
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] storage ready base=0x%llx families=%zu\n",
                     static_cast<unsigned long long>(m_region_base),
                     m_font_families.size());
    }
    return true;
}

GuestAddr DWriteManager::create_factory() {
    return create_object(ObjectType::Factory);
}

GuestAddr DWriteManager::create_object(
    ObjectType type, usize family_index, dwrite::MatchedFont font) {
    if (!ensure_storage()) return 0;
    GuestAddr object_address = 0;
    if (!m_free_objects.empty()) {
        object_address = m_free_objects.back();
        m_free_objects.pop_back();
    } else {
        if (m_next_object + 0x20 > m_region_base + STORAGE_SIZE) return 0;
        object_address = m_next_object;
        m_next_object += 0x20;
    }
    GuestAddr vtable = m_factory_vtable;
    if (type == ObjectType::FontCollection) vtable = m_collection_vtable;
    if (type == ObjectType::FontFamily) vtable = m_family_vtable;
    if (type == ObjectType::Font) vtable = m_font_vtable;
    if (type == ObjectType::FontFace) vtable = m_face_vtable;
    if (type == ObjectType::FontFallback) vtable = m_fallback_vtable;
    if (type == ObjectType::FontFallbackBuilder) {
        vtable = m_fallback_builder_vtable;
    }
    if (type == ObjectType::FontSet) vtable = m_font_set_vtable;
    if (type == ObjectType::FontDownloadQueue) {
        vtable = m_font_download_queue_vtable;
    }
    if (type == ObjectType::NumberSubstitution) {
        vtable = m_number_substitution_vtable;
    }
    if (type == ObjectType::LocalizedStrings) {
        vtable = m_localized_strings_vtable;
    }
    if (type == ObjectType::FontFile) vtable = m_font_file_vtable;
    if (type == ObjectType::LocalFontFileLoader) {
        vtable = m_local_loader_vtable;
    }
    if (type == ObjectType::FontFileStream) {
        vtable = m_font_file_stream_vtable;
    }
    if (type == ObjectType::RenderingParams) {
        vtable = m_rendering_params_vtable;
    }
    if (type == ObjectType::Typography) vtable = m_typography_vtable;
    if (type == ObjectType::TextAnalyzer) vtable = m_text_analyzer_vtable;
    if (type == ObjectType::GdiInterop) vtable = m_gdi_interop_vtable;
    if (type == ObjectType::GlyphRunAnalysis) {
        vtable = m_glyph_run_analysis_vtable;
    }
    if (type == ObjectType::TextFormat) vtable = m_text_format_vtable;
    if (type == ObjectType::TextLayout) vtable = m_text_layout_vtable;
    if (type == ObjectType::TrimmingSign) vtable = m_trimming_sign_vtable;
    if (!write_pointer(object_address, vtable)) return 0;
    Object object{};
    object.type = type;
    object.ref_count = 1;
    object.family_index = family_index;
    object.font = std::move(font);
    m_objects.emplace(object_address, std::move(object));
    return object_address;
}

} // namespace vitreo::winapi

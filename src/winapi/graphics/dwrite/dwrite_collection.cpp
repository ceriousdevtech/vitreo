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

// The sfnt container signatures (OpenType spec, "Organization of an OpenType
// Font" / "Font Collections"): TrueType 0x00010000 or 'true', CFF 'OTTO',
// collection 'ttcf'.
bool has_sfnt_signature(const u8* data, usize size) {
    if (size < 4) return false;
    const u32 tag = (u32{data[0]} << 24) | (u32{data[1]} << 16) |
                    (u32{data[2]} << 8) | u32{data[3]};
    return tag == 0x00010000u || tag == 0x74727565u || tag == 0x4F54544Fu ||
           tag == 0x74746366u;
}

} // namespace

usize DWriteManager::ensure_family_index(const std::u16string& family) {
    const auto found = std::find(
        m_font_families.begin(), m_font_families.end(), family);
    if (found != m_font_families.end()) {
        return static_cast<usize>(
            std::distance(m_font_families.begin(), found));
    }
    m_font_families.push_back(family);
    return m_font_families.size() - 1;
}

std::shared_ptr<std::vector<dwrite::MatchedFont>>
DWriteManager::ensure_family_fonts(Object& family) {
    if (family.family_fonts) return family.family_fonts;
    if (family.family_index >= m_font_families.size()) return {};
    auto fonts = dwrite::enumerate_system_family_fonts(
        m_font_families[family.family_index]);
    if (fonts.empty()) {
        auto regular = dwrite::match_system_font(
            m_font_families[family.family_index], 400, 5, 0);
        if (regular) fonts.push_back(std::move(*regular));
    }
    family.family_fonts =
        std::make_shared<std::vector<dwrite::MatchedFont>>(
            std::move(fonts));
    return family.family_fonts;
}

std::u16string DWriteManager::object_family_name(const Object& object) const {
    if (object.family_fonts && !object.family_fonts->empty()) {
        return object.family_fonts->front().family_name;
    }
    if (!object.font.family_name.empty()) return object.font.family_name;
    if (object.family_index < m_font_families.size()) {
        return m_font_families[object.family_index];
    }
    return {};
}

void DWriteManager::impl_GetSystemFontCollection(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr collection_out = m.arg_ptr(1);
    (void)m.arg(2);
    if (collection_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    auto factory = m_objects.find(self);
    if (factory == m_objects.end() || factory->second.type != ObjectType::Factory) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (m_system_collection == 0) {
        m_system_collection = create_object(ObjectType::FontCollection);
    } else {
        ++m_objects.at(m_system_collection).ref_count;
    }
    if (m_system_collection == 0 || m.write_ptr(collection_out, m_system_collection).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_CreateCustomFontCollection(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr loader = m.arg_ptr(1);
    const GuestAddr key = m.arg_ptr(2);
    const u32 key_size = static_cast<u32>(m.arg(3));
    const GuestAddr collection_out = m.arg_ptr(4);
    if (collection_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(collection_out, 0);
    const auto factory = m_objects.find(self);
    if (factory == m_objects.end() ||
        factory->second.type != ObjectType::Factory || loader == 0 ||
        (key_size != 0 && key == 0) ||
        !factory->second.collection_loaders.contains(loader) ||
        !m_guest_call) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateCustomFontCollection begin "
                     "factory=0x%llx loader=0x%llx key=0x%llx size=%u\n",
                     static_cast<unsigned long long>(self),
                     static_cast<unsigned long long>(loader),
                     static_cast<unsigned long long>(key), key_size);
    }

    // A factory loads a (loader, collection key) once: repeating it, even
    // after every earlier collection is released, makes no loader calls and
    // returns a new collection object until the loader is unregistered
    // (tools/oracle_probe/dwrite_custom_loader_probe.expected). Learn
    // ("Custom Font Collections"): "custom font collections are cached, so
    // the fonts are only loaded one time"; the key must be "valid until the
    // loader is unregistered".
    std::optional<std::string> collection_key;
    if (m_font_reuse) {
        collection_key = loader_key(loader, key, key_size);
        if (!collection_key) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto& cached = factory->second.custom_collections;
        if (auto hit = cached.find(*collection_key); hit != cached.end()) {
            auto families = hit->second;
            const GuestAddr collection = create_object(ObjectType::FontCollection);
            if (collection == 0 || m.write_ptr(collection_out, collection).is_err()) {
                if (collection != 0) m_objects.erase(collection);
                m.set_return(E_OUTOFMEMORY);
                return;
            }
            auto& object = m_objects.at(collection);
            object.custom_collection = true;
            object.collection_families = std::move(families);
            m.set_return(S_OK);
            return;
        }
    }

    constexpr usize SCRATCH_SIZE = GUEST_PAGE_SIZE;
    const GuestAddr scratch_address =
        m_memory.find_free_gap(0x6A000000ull, SCRATCH_SIZE);
    if (scratch_address == 0 || m_memory.map_region(
            scratch_address, SCRATCH_SIZE,
            MemoryProtection::ReadWrite).is_err()) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    struct ScratchMapping {
        GuestMemory& memory;
        GuestAddr address;
        ~ScratchMapping() {
            (void)memory.unmap_region(address, GUEST_PAGE_SIZE);
        }
    } scratch{m_memory, scratch_address};
    const GuestAddr enumerator_out = scratch_address;
    const GuestAddr has_current_out = scratch_address + 0x10;
    const GuestAddr file_out = scratch_address + 0x20;
    if (!write_pointer(enumerator_out, 0)) {
        m.set_return(E_POINTER);
        return;
    }
    const u64 create_args[]{self, key, key_size, enumerator_out};
    const auto create_result = call_guest_method(
        loader, 3, create_args, std::size(create_args));
    if (!create_result) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] CreateCustomFontCollection "
                         "CreateEnumeratorFromKey callback unavailable\n");
        }
        m.set_return(E_INVALIDARG);
        return;
    }
    const u32 create_hr = static_cast<u32>(*create_result);
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateCustomFontCollection "
                     "CreateEnumeratorFromKey -> hr=0x%08x\n",
                     create_hr);
    }
    if ((create_hr & 0x80000000u) != 0) {
        m.set_return(create_hr);
        return;
    }
    const auto enumerator_value = read_pointer(enumerator_out);
    if (enumerator_value.is_err() || enumerator_value.value() == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr enumerator = enumerator_value.value();
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateCustomFontCollection "
                     "enumerator=0x%llx\n",
                     static_cast<unsigned long long>(enumerator));
    }
    std::vector<dwrite::MatchedFont> fonts;
    u32 file_count = 0;
    u32 result_hr = S_OK;
    for (; file_count < 4096; ++file_count) {
        if (m_memory.write_u32(has_current_out, 0).is_err()) {
            result_hr = E_POINTER;
            break;
        }
        const u64 move_args[]{has_current_out};
        const auto move_result = call_guest_method(
            enumerator, 3, move_args, std::size(move_args));
        if (!move_result) {
            result_hr = E_INVALIDARG;
            break;
        }
        const u32 move_hr = static_cast<u32>(*move_result);
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] CreateCustomFontCollection "
                         "MoveNext[%u] -> hr=0x%08x\n",
                         file_count, move_hr);
        }
        if ((move_hr & 0x80000000u) != 0) {
            result_hr = move_hr;
            break;
        }
        const auto has_current = m_memory.read_u32(has_current_out);
        if (has_current.is_err()) {
            result_hr = E_POINTER;
            break;
        }
        if (has_current.value() == 0) break;

        if (!write_pointer(file_out, 0)) {
            result_hr = E_POINTER;
            break;
        }
        const u64 get_args[]{file_out};
        const auto get_result = call_guest_method(
            enumerator, 4, get_args, std::size(get_args));
        if (!get_result) {
            result_hr = E_INVALIDARG;
            break;
        }
        const u32 get_hr = static_cast<u32>(*get_result);
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                         "[dwrite-diag] CreateCustomFontCollection "
                         "GetCurrentFontFile[%u] -> hr=0x%08x\n",
                         file_count, get_hr);
        }
        if ((get_hr & 0x80000000u) != 0) {
            result_hr = get_hr;
            break;
        }
        const auto file_value = read_pointer(file_out);
        if (file_value.is_err() || file_value.value() == 0) {
            result_hr = E_INVALIDARG;
            break;
        }
        const GuestAddr file = file_value.value();
        const auto file_object = m_objects.find(file);
        if (file_object == m_objects.end() ||
            file_object->second.type != ObjectType::FontFile) {
            release_guest_object(file);
            result_hr = E_INVALIDARG;
            break;
        }
        std::optional<std::string> file_key;
        if (m_font_reuse && file_object->second.font_file_loader != 0) {
            file_key = loader_key(file_object->second.font_file_loader,
                                  file_object->second.key_address,
                                  file_object->second.key_size);
        }
        const auto* known = [&]() -> const ParsedFonts* {
            const auto owner = m_objects.find(self);
            if (!file_key || owner == m_objects.end()) return nullptr;
            const auto& files = owner->second.custom_font_files;
            const auto hit = files.find(*file_key);
            return hit != files.end() ? &hit->second : nullptr;
        }();
        u32 load_hr = S_OK;
        ParsedFonts parsed_fonts;
        if (known != nullptr) {
            // A factory loads a (file loader, reference key) once; later
            // collections still open the stream and read the header to
            // identify the file type, and an unrecognized header is skipped.
            parsed_fonts = *known;
            bool recognized = false;
            load_hr = read_guest_font_stream(
                file_object->second, scratch_address,
                [&](GuestAddr fragment, usize size) -> u32 {
                    std::array<u8, 12> header{};
                    if (size != 0 && m_memory.read_bytes(
                            fragment, reinterpret_cast<Byte*>(header.data()),
                            size).is_err()) {
                        return E_POINTER;
                    }
                    recognized = has_sfnt_signature(header.data(), size);
                    return S_OK;
                }, 12);
            if (!recognized) parsed_fonts.reset();
        } else {
            std::tie(load_hr, parsed_fonts) =
                load_font_file(file_object->second, scratch_address);
            const auto owner = m_objects.find(self);
            if (file_key && (load_hr & 0x80000000u) == 0 && owner != m_objects.end()) {
                owner->second.custom_font_files[*file_key] = parsed_fonts;
            }
        }
        release_guest_object(file);
        if ((load_hr & 0x80000000u) != 0) {
            result_hr = load_hr;
            break;
        }
        // A file DirectWrite does not recognize as a font is left out of the
        // collection, which still succeeds (S_OK, 0 families).
        if (!parsed_fonts || parsed_fonts->empty()) {
            if (dwrite_diag_enabled()) {
                dwrite_diag_printf(
                    "[dwrite-diag] CreateCustomFontCollection file[%u] "
                    "not a supported font file; skipped\n", file_count);
            }
            continue;
        }
        const auto& parsed = *parsed_fonts;
        if (dwrite_diag_enabled()) {
            for (const auto& parsed_font : parsed) {
                const std::string family = utf16_to_utf8(
                    reinterpret_cast<const u16*>(
                        parsed_font.family_name.data()),
                    static_cast<int>(parsed_font.family_name.size()));
                const std::string postscript = utf16_to_utf8(
                    reinterpret_cast<const u16*>(
                        parsed_font.postscript_name.data()),
                    static_cast<int>(parsed_font.postscript_name.size()));
                std::fprintf(
                    stderr,
                    "[dwrite-diag] custom font file[%u] family='%s' "
                    "postscript='%s' face=%u glyphs=%u units=%u bytes=%zu\n",
                    file_count, family.c_str(), postscript.c_str(),
                    parsed_font.face_index, parsed_font.glyph_count,
                    parsed_font.metrics.design_units_per_em,
                    parsed_font.source_data ? parsed_font.source_data->size() : 0);
            }
        }
        fonts.insert(fonts.end(), parsed.begin(), parsed.end());
    }
    if (file_count == 4096) result_hr = E_OUTOFMEMORY;
    release_guest_object(enumerator);
    if ((result_hr & 0x80000000u) != 0) {
        m.set_return(result_hr);
        return;
    }

    std::vector<std::shared_ptr<std::vector<dwrite::MatchedFont>>> families;
    for (auto& font : fonts) {
        auto found = std::find_if(
            families.begin(), families.end(), [&](const auto& family) {
                return !family->empty() &&
                       family->front().family_name == font.family_name;
            });
        if (found == families.end()) {
            auto family =
                std::make_shared<std::vector<dwrite::MatchedFont>>();
            family->push_back(std::move(font));
            families.push_back(std::move(family));
        } else {
            (*found)->push_back(std::move(font));
        }
    }
    if (const auto owner = m_objects.find(self);
        collection_key && owner != m_objects.end()) {
        owner->second.custom_collections[*collection_key] = families;
    }
    const GuestAddr collection = create_object(ObjectType::FontCollection);
    if (collection == 0 ||
        m.write_ptr(collection_out, collection).is_err()) {
        if (collection != 0) m_objects.erase(collection);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    auto& object = m_objects.at(collection);
    object.custom_collection = true;
    object.collection_families = std::move(families);
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] CreateCustomFontCollection "
                     "loader=0x%llx files=%u families=%zu -> S_OK "
                     "collection=0x%llx\n",
                     static_cast<unsigned long long>(loader), file_count,
                     object.collection_families.size(),
                     static_cast<unsigned long long>(collection));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_GetFontFamilyCount(Marshaler& m) {
    auto collection = m_objects.find(m.arg_ptr(0));
    if (collection == m_objects.end() || collection->second.type != ObjectType::FontCollection) {
        m.set_return(0);
        return;
    }
    const usize count = collection->second.custom_collection
                            ? collection->second.collection_families.size()
                            : m_font_families.size();
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
            "[dwrite-diag] GetFontFamilyCount collection=0x%llx custom=%u "
            "-> %zu\n",
            static_cast<unsigned long long>(m.arg_ptr(0)),
            collection->second.custom_collection ? 1u : 0u, count);
    }
    m.set_return(static_cast<u32>(count));
}

void DWriteManager::impl_GetFontFamily(Marshaler& m) {
    auto collection = m_objects.find(m.arg_ptr(0));
    const usize index = static_cast<usize>(m.arg(1));
    const GuestAddr family_out = m.arg_ptr(2);
    if (family_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(family_out, 0);
    const usize family_count =
        collection != m_objects.end() && collection->second.custom_collection
            ? collection->second.collection_families.size()
            : m_font_families.size();
    if (collection == m_objects.end() || collection->second.type != ObjectType::FontCollection ||
        index >= family_count) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr family = create_object(ObjectType::FontFamily, index);
    if (family == 0 || m.write_ptr(family_out, family).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (collection->second.custom_collection) {
        m_objects.at(family).family_fonts =
            collection->second.collection_families[index];
    }
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
            "[dwrite-diag] GetFontFamily collection=0x%llx index=%zu "
            "-> S_OK family=0x%llx\n",
            static_cast<unsigned long long>(m.arg_ptr(0)), index,
            static_cast<unsigned long long>(family));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FindFamilyName(Marshaler& m) {
    auto collection = m_objects.find(m.arg_ptr(0));
    const GuestAddr name_address = m.arg_ptr(1);
    const GuestAddr index_out = m.arg_ptr(2);
    const GuestAddr exists_out = m.arg_ptr(3);
    if (collection == m_objects.end() || collection->second.type != ObjectType::FontCollection ||
        name_address == 0 || index_out == 0 || exists_out == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }
    auto name = read_utf16_z(name_address, 32768);
    if (name.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto fold = [](char16_t ch) {
        return ch >= u'A' && ch <= u'Z' ? static_cast<char16_t>(ch + (u'a' - u'A')) : ch;
    };
    const auto matches = [&](const std::u16string& family) {
        if (family.size() != name.value().size()) return false;
        return std::equal(family.begin(), family.end(), name.value().begin(),
                          [&](char16_t left, char16_t right) { return fold(left) == fold(right); });
    };
    u32 index = 0;
    bool exists = false;
    if (collection->second.custom_collection) {
        for (usize candidate = 0;
             candidate < collection->second.collection_families.size();
             ++candidate) {
            const auto& fonts =
                collection->second.collection_families[candidate];
            if (fonts && !fonts->empty() &&
                matches(fonts->front().family_name)) {
                exists = true;
                index = static_cast<u32>(candidate);
                break;
            }
        }
    } else {
        const auto found = std::find_if(
            m_font_families.begin(), m_font_families.end(), matches);
        exists = found != m_font_families.end();
        if (exists) {
            index = static_cast<u32>(
                std::distance(m_font_families.begin(), found));
        }
    }
    if (m.write_u32(index_out, index).is_err() || m.write_u32(exists_out, exists ? 1u : 0u).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    if (dwrite_diag_enabled()) {
        std::string ascii_name;
        ascii_name.reserve(name.value().size());
        for (const char16_t ch : name.value()) {
            ascii_name.push_back(
                ch <= 0x7fu ? static_cast<char>(ch) : '?');
        }
        dwrite_diag_printf(
                     "[dwrite-diag] FindFamilyName collection=0x%llx "
                     "name='%s' -> S_OK exists=%u index=%u\n",
                     static_cast<unsigned long long>(m.arg_ptr(0)),
                     ascii_name.c_str(), exists ? 1u : 0u, index);
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_FamilyGetFontCount(Marshaler& m) {
    auto family = m_objects.find(m.arg_ptr(0));
    if (family == m_objects.end() ||
        family->second.type != ObjectType::FontFamily) {
        m.set_return(0);
        return;
    }
    const auto fonts = ensure_family_fonts(family->second);
    m.set_return(fonts ? static_cast<u32>(fonts->size()) : 0u);
}

void DWriteManager::impl_FamilyGetFont(Marshaler& m) {
    auto family = m_objects.find(m.arg_ptr(0));
    const usize index = static_cast<usize>(m.arg(1));
    const GuestAddr font_out = m.arg_ptr(2);
    if (font_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(font_out, 0);
    if (family == m_objects.end() ||
        family->second.type != ObjectType::FontFamily) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto fonts = ensure_family_fonts(family->second);
    if (!fonts || index >= fonts->size()) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr font = create_object(
        ObjectType::Font, family->second.family_index, (*fonts)[index]);
    if (font == 0 || m.write_ptr(font_out, font).is_err()) {
        if (font != 0) m_objects.erase(font);
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    m_objects.at(font).family_fonts = fonts;
    m.set_return(S_OK);
}

void DWriteManager::impl_FamilyGetNames(Marshaler& m) {
    const GuestAddr self = m.arg_ptr(0);
    const GuestAddr names_out = m.arg_ptr(1);
    if (names_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(names_out, 0);
    const auto family = m_objects.find(self);
    if (family == m_objects.end() ||
        family->second.type != ObjectType::FontFamily) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const GuestAddr names = create_object(
        ObjectType::LocalizedStrings, family->second.family_index);
    if (names == 0 || m.write_ptr(names_out, names).is_err()) {
        if (names != 0) m_objects.erase(names);
        m.set_return(E_POINTER);
        return;
    }
    m_objects.at(names).localized_value = object_family_name(family->second);
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
                     "[dwrite-diag] GetFamilyNames family=0x%llx index=%zu "
                     "-> S_OK names=0x%llx\n",
                     static_cast<unsigned long long>(self),
                     family->second.family_index,
                     static_cast<unsigned long long>(names));
    }
    m.set_return(S_OK);
}

void DWriteManager::impl_GetFirstMatchingFont(Marshaler& m) {
    const auto family = m_objects.find(m.arg_ptr(0));
    const u32 weight = static_cast<u32>(m.arg(1));
    const u32 stretch = static_cast<u32>(m.arg(2));
    const u32 style = static_cast<u32>(m.arg(3));
    const GuestAddr font_out = m.arg_ptr(4);
    if (font_out == 0) {
        m.set_return(E_POINTER);
        return;
    }
    (void)m.write_ptr(font_out, 0);
    if (family == m_objects.end() ||
        family->second.type != ObjectType::FontFamily ||
        weight < 1 || weight > 999 || stretch < 1 || stretch > 9 ||
        style > 2) {
        m.set_return(E_INVALIDARG);
        return;
    }
    const auto fonts = ensure_family_fonts(family->second);
    if (!fonts || fonts->empty()) {
        if (dwrite_diag_enabled()) {
            dwrite_diag_printf(
                "[dwrite-diag] GetFirstMatchingFont family=0x%llx "
                "-> DWRITE_E_FILEFORMAT (no fonts)\n",
                static_cast<unsigned long long>(m.arg_ptr(0)));
        }
        m.set_return(DWRITE_E_FILEFORMAT);
        return;
    }
    const auto score = [&](const dwrite::MatchedFont& font) {
        const u32 weight_distance = font.weight > weight
            ? font.weight - weight : weight - font.weight;
        const u32 stretch_distance = font.stretch > stretch
            ? font.stretch - stretch : stretch - font.stretch;
        const u32 style_distance = font.style == style ? 0u : 1u;
        return weight_distance + stretch_distance * 1000u +
               style_distance * 10000u;
    };
    const auto matched = std::min_element(
        fonts->begin(), fonts->end(), [&](const auto& left, const auto& right) {
            return score(left) < score(right);
        });
    const GuestAddr font = create_object(
        ObjectType::Font, family->second.family_index, *matched);
    if (font == 0 || m.write_ptr(font_out, font).is_err()) {
        m.set_return(E_POINTER);
        return;
    }
    m_objects.at(font).family_fonts = fonts;
    if (dwrite_diag_enabled()) {
        dwrite_diag_printf(
            "[dwrite-diag] GetFirstMatchingFont family=0x%llx weight=%u "
            "stretch=%u style=%u -> S_OK font=0x%llx\n",
            static_cast<unsigned long long>(m.arg_ptr(0)), weight, stretch,
            style, static_cast<unsigned long long>(font));
    }
    m.set_return(S_OK);
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
/// @file typelib.cpp
/// @brief MSFT type-library container reader.
///
/// Layout of the part Vitreo reads:
///
///   0x00  header          21 little-endian INTs (0x54 bytes)
///   0x54  typeinfo offsets  nrtypeinfos INTs
///   ...   segment directory 15 entries of { offset, length, res08, res0c }
///
/// Segments used:
///   0  TypeInfoBase (0x64 bytes each) — per-type kind/flags/name/guid link
///   5  GUID table (24-byte entries) — GUID + hreftype + next_hash
///   7  Name table
///   8  String table
///
/// Local type references are resolved through the header offset table.
/// Evidence and supported scope: tests/fixtures/typelib-native/README.md.

#include "typelib.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vitreo::winapi {

namespace {

/// 'MSFT' as it appears in the first four bytes of a little-endian image.
constexpr u32 MSFT_SIGNATURE = 0x5446534Du;
/// 'SLTG' — the pre-MIDL container, recognised only so it can be rejected.
constexpr u32 SLTG_SIGNATURE = 0x47544C53u;

constexpr usize HEADER_SIZE     = 0x54;
constexpr usize SEGMENT_COUNT   = 15;
constexpr usize SEGMENT_SIZE    = 16;
constexpr usize SEGMENT_TYPEINFO = 0;
constexpr usize SEGMENT_GUID     = 5;
constexpr usize SEGMENT_NAME     = 7;
constexpr usize SEGMENT_STRING   = 8;
constexpr usize SEGMENT_TYPEDESC = 9;

constexpr usize GUID_ENTRY_SIZE     = 24;
constexpr usize NAME_ENTRY_HEADER   = 12;
constexpr usize TYPEINFO_BASE_SIZE  = 0x64;
constexpr usize PARAM_INFO_SIZE     = 12; ///< MSFT_ParameterInfo (3×INT).

constexpr u16 VT_I4            = 3;
constexpr u16 VT_UI4           = 19;
constexpr u16 VT_INT           = 22;
constexpr u16 VT_UINT          = 23;
constexpr u16 VT_HRESULT       = 25;
constexpr u16 VT_PTR           = 26;
constexpr u16 VT_SAFEARRAY     = 27;
constexpr u16 VT_USERDEFINED   = 29;
constexpr u16 VT_RECORD        = 36;
constexpr u16 VT_ERROR         = 10;
constexpr u16 VT_DISPATCH      = 9;
constexpr u16 VT_UNKNOWN       = 13;
constexpr u16 PARAMFLAG_FRETVAL = 0x0008;

struct Segment {
    u32 offset{0};
    u32 length{0};
};

[[nodiscard]] bool read_u32_at(std::span<const u8> image, usize offset, u32& out) {
    if (offset + sizeof(u32) > image.size()) return false;
    u32 value = 0;
    std::memcpy(&value, image.data() + offset, sizeof(value));
    out = value;
    return true;
}

[[nodiscard]] bool read_u16_at(std::span<const u8> image, usize offset, u16& out) {
    if (offset + sizeof(u16) > image.size()) return false;
    u16 value = 0;
    std::memcpy(&value, image.data() + offset, sizeof(value));
    out = value;
    return true;
}

[[nodiscard]] bool segment_contains(std::span<const u8> image,
                                    const Segment& segment,
                                    u32 offset,
                                    usize size) {
    const usize segment_end =
        static_cast<usize>(segment.offset) + static_cast<usize>(segment.length);
    if (segment_end > image.size()) return false;
    if (offset > segment.length) return false;
    return static_cast<usize>(offset) + size <= static_cast<usize>(segment.length);
}

[[nodiscard]] std::string read_name(std::span<const u8> image,
                                    const Segment& names,
                                    u32 offset) {
    if (!segment_contains(image, names, offset, NAME_ENTRY_HEADER)) return {};
    const usize entry = static_cast<usize>(names.offset) + offset;
    u32 packed_length = 0;
    if (!read_u32_at(image, entry + 8, packed_length)) return {};
    const usize length = packed_length & 0xFFu;
    if (!segment_contains(image, names, offset,
                          NAME_ENTRY_HEADER + length)) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(image.data() + entry + NAME_ENTRY_HEADER),
        length);
}

[[nodiscard]] std::string read_string(std::span<const u8> image,
                                      const Segment& strings,
                                      u32 offset) {
    if (!segment_contains(image, strings, offset, sizeof(u16))) return {};
    const usize entry = static_cast<usize>(strings.offset) + offset;
    u16 length = 0;
    std::memcpy(&length, image.data() + entry, sizeof(length));
    if (!segment_contains(image, strings, offset, sizeof(u16) + length)) {
        return {};
    }
    return std::string(
        reinterpret_cast<const char*>(image.data() + entry + sizeof(u16)),
        length);
}

[[nodiscard]] TypeLibParseResult fail(TypeLibParseError error) {
    TypeLibParseResult result;
    result.error = error;
    return result;
}

// Record schema measured from native Windows fixture variants 0..9.
// See tools/oracle_probe/typelib_fixture_probe.cpp and the native fixture
// regression in tests/winapi/oleaut32/test_typelib.cpp (also run by CTest).
// All reads are relative to bounded sections; no host structure is overlaid
// on guest bytes. Public Automation enums retain their SDK values.
class TypeRecords {
    std::span<const u8> file;
    const std::array<Segment, SEGMENT_COUNT>& sections;
    std::vector<TypeInfoSummary>& types;
    std::vector<u32> locations;
    bool valid{true};

    u32 word(usize offset) {
        u32 value{};
        if (!read_u32_at(file, offset, value)) valid = false;
        return value;
    }
    u16 half(usize offset) {
        u16 value{};
        if (!read_u16_at(file, offset, value)) valid = false;
        return value;
    }
    std::optional<usize> address(usize section, u32 relative, usize size) {
        if (!segment_contains(file, sections[section], relative, size)) {
            valid = false;
            return std::nullopt;
        }
        return static_cast<usize>(sections[section].offset) + relative;
    }
    std::optional<usize> local_type(u32 reference) const {
        for (usize index = 0; index != locations.size(); ++index)
            if (locations[index] == reference) return index;
        return std::nullopt;
    }
    struct Parameter { u16 value_type{}; i32 reference{-1}; TypeDescription description; };

    Parameter parameter(u32 encoded) {
        // Native fixtures contain both immediate VARTYPEs and section links.
        // Follow links iteratively with explicit cycle detection.
        std::array<u32, 32> visited{};
        usize count = 0;
        bool array = false;
        Parameter result;
        while (valid) {
            u16 tag;
            u32 payload = 0;
            if (static_cast<i32>(encoded) < 0) {
                tag = static_cast<u16>(encoded & 0x0fff);
            } else {
                if (count == visited.size() ||
                    std::find(visited.begin(), visited.begin() + count, encoded)
                        != visited.begin() + count) {
                    valid = false;
                    return {};
                }
                visited[count++] = encoded;
                auto pos = address(SEGMENT_TYPEDESC, encoded, 8);
                if (!pos) return {};
                tag = half(*pos) & 0x0fff;
                payload = word(*pos + 4);
            }
            result.description.tags.push_back(tag);
            if (tag == VT_PTR || tag == VT_SAFEARRAY) {
                array |= tag == VT_SAFEARRAY;
                encoded = payload;
                continue;
            }
            if (tag == VT_USERDEFINED) {
                result.reference = static_cast<i32>(payload);
                result.description.reference = payload;
                auto index = local_type(payload);
                if (index) {
                    const auto kind = types[*index].typekind & 0x0f;
                    switch (kind) {
                    case 0: tag = VT_I4; break;
                    case 1: case 7: tag = VT_RECORD; break;
                    case 3: case 5: tag = VT_UNKNOWN; break;
                    case 4: tag = VT_DISPATCH; break;
                    default: valid = false; return {};
                    }
                }
                // Preserve imported references as VT_USERDEFINED. An external
                // resolver is required before they can be used for coercion.
            }
            if (tag == VT_INT) tag = VT_I4;
            if (tag == VT_UINT) tag = VT_UI4;
            if (tag == VT_HRESULT) tag = VT_ERROR;
            result.value_type = static_cast<u16>(tag | (array ? 0x2000 : 0));
            return result;
        }
        return {};
    }

    void describe(usize index) {
        auto pos = address(0, locations[index], TYPEINFO_BASE_SIZE);
        if (!pos) return;
        const usize start = *pos;
        auto& type = types[index];
        type.reference = locations[index];
        type.typekind = word(start);
        type.alignment = static_cast<u16>((type.typekind >> 6) & 31u);
        const u32 members = word(start + 24);
        type.c_funcs = static_cast<u16>(members);
        type.c_vars = static_cast<u16>(members >> 16);
        type.type_flags = half(start + 48);
        type.major = half(start + 56);
        type.minor = half(start + 58);
        type.c_impl_types = half(start + 76);
        type.cb_size_vft = half(start + 78);
        type.cb_size_instance = word(start + 80);
        const u32 guid_offset = word(start + 44);
        if (static_cast<i32>(guid_offset) >= 0) {
            auto guid = address(SEGMENT_GUID, guid_offset, GUID_ENTRY_SIZE);
            if (!guid) return;
            std::copy_n(file.begin() + *guid, type.guid.size(), type.guid.begin());
            type.has_guid = true;
        }
        const u32 name = word(start + 52), doc = word(start + 60);
        if (static_cast<i32>(name) >= 0)
            type.name = read_name(file, sections[SEGMENT_NAME], name);
        if (static_cast<i32>(doc) >= 0)
            type.doc = read_string(file, sections[SEGMENT_STRING], doc);
        u32 reference = word(start + 84);
        if ((type.typekind & 0x0f) == 5) {
            // Native coclass records link entries in section 3.
            std::vector<u32> visited;
            for (u16 n = 0; n < type.c_impl_types && valid; ++n) {
                if (std::find(visited.begin(), visited.end(), reference) != visited.end()) {
                    valid = false;
                    return;
                }
                visited.push_back(reference);
                auto entry = address(3, reference, 16);
                if (!entry) return;
                type.impl_hrefs.push_back(word(*entry));
                reference = word(*entry + 12);
            }
        } else if (type.c_impl_types) {
            if (type.c_impl_types != 1 || static_cast<i32>(reference) < 0) {
                valid = false;
                return;
            }
            type.impl_hrefs.push_back(reference);
        }
    }

    void functions(usize index) {
        auto& type = types[index];
        if (!type.c_funcs) return;
        auto base = address(0, locations[index], TYPEINFO_BASE_SIZE);
        if (!base) return;
        const usize table = word(*base + 4);
        const usize bytes = word(table);
        if (!valid || table > file.size() || file.size() - table < 4 ||
            bytes > file.size() - table - 4) { valid = false; return; }
        const usize descriptors = table + 4;
        const usize ids = descriptors + bytes;
        const usize member_count = static_cast<usize>(type.c_funcs) + type.c_vars;
        if (member_count > (file.size() - ids) / 12) { valid = false; return; }
        const usize names = ids + member_count * 4;
        const usize offsets = names + member_count * 4;
        type.funcs.reserve(type.c_funcs);
        for (u16 index = 0; index < type.c_funcs && valid; ++index) {
            const usize relative = word(offsets + 4 * index);
            if (relative > bytes || bytes - relative < 24) { valid = false; break; }
            const usize record = descriptors + relative;
            const usize length = half(record);
            if (length < 24 || length > bytes - relative) { valid = false; break; }
            FuncSummary function;
            function.memid = static_cast<i32>(word(ids + 4 * index));
            function.name = read_name(file, sections[SEGMENT_NAME], word(names + 4 * index));
            function.o_vft = static_cast<u16>(half(record + 12) & ~1u);
            const u32 flags = word(record + 16);
            function.funckind = flags & 7;
            function.invkind = (flags >> 3) & 15;
            function.c_params = half(record + 20);
            function.callconv = static_cast<u16>((flags >> 8) & 15);
            function.optional_params = static_cast<i16>(half(record + 22));
            function.function_flags = half(record + 8);
            function.result_description = parameter(word(record + 4)).description;
            if (function.c_params > (length - 24) / PARAM_INFO_SIZE) {
                valid = false;
                break;
            }
            const usize args = record + length - function.c_params * PARAM_INFO_SIZE;
            for (u16 arg = 0; arg < function.c_params && valid; ++arg) {
                const usize entry = args + arg * PARAM_INFO_SIZE;
                const auto resolved = parameter(word(entry));
                const u16 direction = half(entry + 8);
                function.param_vts.push_back(resolved.value_type);
                function.param_descriptions.push_back(resolved.description);
                function.param_flags.push_back(direction);
                function.param_hrefs.push_back(resolved.reference);
                TypeLibGuid guid{};
                if (auto local = local_type(static_cast<u32>(resolved.reference)); local)
                    guid = types[*local].guid;
                function.param_iids.push_back(guid);
                if (direction & PARAMFLAG_FRETVAL) function.retval_vt = resolved.value_type;
            }
            type.funcs.push_back(std::move(function));
        }
    }
public:
    TypeRecords(std::span<const u8> bytes,
                const std::array<Segment, SEGMENT_COUNT>& directory,
                std::vector<TypeInfoSummary>& output)
        : file(bytes), sections(directory), types(output) {}
    bool decode(u32 count) {
        if (count > (file.size() > HEADER_SIZE ? (file.size() - HEADER_SIZE) / 4 : 0))
            return false;
        types.resize(count);
        for (u32 index = 0; index < count; ++index)
            locations.push_back(word(HEADER_SIZE + ((word(20) & 0x100) ? 4 : 0) + index * 4));
        for (usize index = 0; index < count && valid; ++index) describe(index);
        for (usize index = 0; index < count && valid; ++index) functions(index);
        return valid;
    }
};

[[nodiscard]] bool parse_typeinfos(std::span<const u8> image,
    const std::array<Segment, SEGMENT_COUNT>& sections, u32 count,
    std::vector<TypeInfoSummary>& output) {
    return TypeRecords(image, sections, output).decode(count);
}

} // namespace

const char* typelib_parse_error_name(TypeLibParseError error) noexcept {
    switch (error) {
        case TypeLibParseError::NotTypeLib:        return "not-a-typelib";
        case TypeLibParseError::UnsupportedFormat: return "unsupported-container";
        case TypeLibParseError::Truncated:         return "truncated";
    }
    return "unknown";
}

std::optional<u32> TypeLibParseResult::find_typeinfo_by_guid(
    const TypeLibGuid& guid) const {
    for (u32 i = 0; i < typeinfos.size(); ++i) {
        if (typeinfos[i].has_guid && typeinfos[i].guid == guid) {
            return i;
        }
    }
    return std::nullopt;
}

TypeLibParseResult parse_typelib(std::span<const u8> image) {
    u32 signature = 0;
    if (!read_u32_at(image, 0, signature)) return fail(TypeLibParseError::NotTypeLib);
    if (signature == SLTG_SIGNATURE) {
        return fail(TypeLibParseError::UnsupportedFormat);
    }
    if (signature != MSFT_SIGNATURE) {
        return fail(TypeLibParseError::NotTypeLib);
    }
    if (image.size() < HEADER_SIZE) return fail(TypeLibParseError::Truncated);

    u32 raw_posguid = 0, lcid = 0, varflags = 0, version = 0, flags = 0;
    u32 raw_type_info_count = 0, raw_helpstring = 0, raw_name_offset = 0;
    if (!read_u32_at(image, 0x08, raw_posguid) ||
        !read_u32_at(image, 0x0C, lcid) ||
        !read_u32_at(image, 0x14, varflags) ||
        !read_u32_at(image, 0x18, version) ||
        !read_u32_at(image, 0x1C, flags) ||
        !read_u32_at(image, 0x20, raw_type_info_count) ||
        !read_u32_at(image, 0x24, raw_helpstring) ||
        !read_u32_at(image, 0x38, raw_name_offset)) {
        return fail(TypeLibParseError::Truncated);
    }

    const i32 type_info_count = static_cast<i32>(raw_type_info_count);
    if (type_info_count < 0) return fail(TypeLibParseError::Truncated);

    // HELPDLLFLAG (0x100) inserts an extra INT before the segment directory.
    usize segment_directory =
        HEADER_SIZE + static_cast<usize>(type_info_count) * sizeof(u32);
    if ((varflags & 0x100u) != 0) {
        segment_directory += sizeof(u32);
    }
    if (segment_directory + SEGMENT_COUNT * SEGMENT_SIZE > image.size()) {
        return fail(TypeLibParseError::Truncated);
    }

    std::array<Segment, SEGMENT_COUNT> segments{};
    for (usize i = 0; i < SEGMENT_COUNT; ++i) {
        const usize entry = segment_directory + i * SEGMENT_SIZE;
        if (!read_u32_at(image, entry, segments[i].offset) ||
            !read_u32_at(image, entry + 4, segments[i].length)) {
            return fail(TypeLibParseError::Truncated);
        }
    }

    TypeLibAttributes attributes;
    attributes.lcid            = lcid;
    attributes.syskind         = static_cast<SysKind>(varflags & 0x0Fu);
    attributes.major           = static_cast<u16>(version & 0xFFFFu);
    attributes.minor           = static_cast<u16>((version >> 16) & 0xFFFFu);
    attributes.lib_flags       = static_cast<u16>(flags & 0xFFFFu);
    attributes.type_info_count = static_cast<u32>(type_info_count);

    const i32 posguid = static_cast<i32>(raw_posguid);
    if (posguid < 0 ||
        !segment_contains(image, segments[SEGMENT_GUID],
                          static_cast<u32>(posguid), GUID_ENTRY_SIZE)) {
        return fail(TypeLibParseError::Truncated);
    }
    std::memcpy(attributes.libid.data(),
                image.data() + segments[SEGMENT_GUID].offset +
                    static_cast<usize>(posguid),
                attributes.libid.size());

    if (static_cast<i32>(raw_name_offset) >= 0) {
        attributes.name =
            read_name(image, segments[SEGMENT_NAME], raw_name_offset);
    }
    if (static_cast<i32>(raw_helpstring) >= 0) {
        attributes.doc =
            read_string(image, segments[SEGMENT_STRING], raw_helpstring);
    }

    TypeLibParseResult result;
    result.attributes = std::move(attributes);
    if (type_info_count > 0) {
        if (!parse_typeinfos(image, segments,
                             static_cast<u32>(type_info_count),
                             result.typeinfos)) {
            return fail(TypeLibParseError::Truncated);
        }
    }
    return result;
}

namespace {

[[nodiscard]] bool read_u16_file(std::span<const u8> img, usize off, u16& out) {
    if (off + 2 > img.size()) return false;
    out = static_cast<u16>(img[off] | (static_cast<u16>(img[off + 1]) << 8));
    return true;
}

[[nodiscard]] bool read_u32_file(std::span<const u8> img, usize off, u32& out) {
    if (off + 4 > img.size()) return false;
    out = static_cast<u32>(img[off]) |
          (static_cast<u32>(img[off + 1]) << 8) |
          (static_cast<u32>(img[off + 2]) << 16) |
          (static_cast<u32>(img[off + 3]) << 24);
    return true;
}

[[nodiscard]] std::optional<usize> pe_rva_to_file_offset(
    std::span<const u8> pe, u32 rva) {
    u32 e_lfanew = 0;
    if (!read_u32_file(pe, 0x3C, e_lfanew)) return std::nullopt;
    if (e_lfanew + 24 > pe.size()) return std::nullopt;
    u32 sig = 0;
    if (!read_u32_file(pe, e_lfanew, sig) || sig != 0x00004550u) {
        return std::nullopt;
    }
    u16 num_sections = 0;
    u16 size_opt = 0;
    if (!read_u16_file(pe, e_lfanew + 6, num_sections) ||
        !read_u16_file(pe, e_lfanew + 20, size_opt)) {
        return std::nullopt;
    }
    const usize sec_off = static_cast<usize>(e_lfanew) + 24 + size_opt;
    for (u16 i = 0; i < num_sections; ++i) {
        const usize o = sec_off + static_cast<usize>(i) * 40;
        u32 vsize = 0, va = 0, rawsize = 0, rawptr = 0;
        if (!read_u32_file(pe, o + 8, vsize) ||
            !read_u32_file(pe, o + 12, va) ||
            !read_u32_file(pe, o + 16, rawsize) ||
            !read_u32_file(pe, o + 20, rawptr)) {
            return std::nullopt;
        }
        const u32 span = rawsize > vsize ? rawsize : vsize;
        if (rva >= va && rva < va + span) {
            return static_cast<usize>(rawptr) + (rva - va);
        }
    }
    return std::nullopt;
}

[[nodiscard]] bool pe_resource_dir_rva(std::span<const u8> pe, u32& rva_out) {
    u32 e_lfanew = 0;
    if (!read_u32_file(pe, 0x3C, e_lfanew)) return false;
    u16 magic = 0;
    if (!read_u16_file(pe, e_lfanew + 24, magic)) return false;
    const usize dd = static_cast<usize>(e_lfanew) + 24 +
                     (magic == 0x20Bu ? 112u : 96u);
    // DataDirectory[2] = resources
    return read_u32_file(pe, dd + 2 * 8, rva_out) && rva_out != 0;
}

/// IMAGE_RESOURCE_DIRECTORY entry walk: named "TYPELIB", first name, first lang.
[[nodiscard]] std::vector<u8> extract_typelib_from_rsrc(
    std::span<const u8> pe, usize rsrc_file_off,
    std::optional<u32> resource_id) {
    auto dir_counts = [&](usize dir_off, u16& named, u16& id) -> bool {
        return read_u16_file(pe, dir_off + 12, named) &&
               read_u16_file(pe, dir_off + 14, id);
    };
    auto entry_at = [&](usize dir_off, u32 index, u32& name_or_id,
                        u32& offset) -> bool {
        const usize eo = dir_off + 16 + static_cast<usize>(index) * 8;
        return read_u32_file(pe, eo, name_or_id) &&
               read_u32_file(pe, eo + 4, offset);
    };
    auto entry_name = [&](u32 name_or_id) -> std::u16string {
        if ((name_or_id & 0x80000000u) == 0) return {};
        const usize no = rsrc_file_off + (name_or_id & 0x7FFFFFFFu);
        u16 len = 0;
        if (!read_u16_file(pe, no, len)) return {};
        std::u16string s;
        s.resize(len);
        for (u16 i = 0; i < len; ++i) {
            u16 ch = 0;
            if (!read_u16_file(pe, no + 2 + static_cast<usize>(i) * 2, ch)) {
                return {};
            }
            s[i] = static_cast<char16_t>(ch);
        }
        return s;
    };

    u16 named = 0, id = 0;
    if (!dir_counts(rsrc_file_off, named, id)) return {};
    usize typelib_dir = 0;
    for (u32 i = 0; i < static_cast<u32>(named) + id; ++i) {
        u32 name_or_id = 0, offset = 0;
        if (!entry_at(rsrc_file_off, i, name_or_id, offset)) return {};
        if ((offset & 0x80000000u) == 0) continue;
        const auto name = entry_name(name_or_id);
        static constexpr char16_t kTypeLib[] = {
            u'T', u'Y', u'P', u'E', u'L', u'I', u'B'};
        if (name.size() == 7 &&
            std::equal(name.begin(), name.end(), kTypeLib)) {
            typelib_dir = rsrc_file_off + (offset & 0x7FFFFFFFu);
            break;
        }
    }
    if (typelib_dir == 0) return {};

    // Requested name/id under TYPELIB, or the first entry when LoadTypeLib's
    // path did not append a resource selector.
    if (!dir_counts(typelib_dir, named, id)) return {};
    if (named + id == 0) return {};
    u32 name_or_id = 0, offset = 0;
    bool resource_found = false;
    for (u32 i = 0; i < static_cast<u32>(named) + id; ++i) {
        if (!entry_at(typelib_dir, i, name_or_id, offset)) return {};
        if (!resource_id ||
            ((name_or_id & 0x80000000u) == 0 &&
             name_or_id == *resource_id)) {
            resource_found = true;
            break;
        }
    }
    if (!resource_found) return {};
    if ((offset & 0x80000000u) == 0) return {};
    const usize lang_dir = rsrc_file_off + (offset & 0x7FFFFFFFu);

    if (!dir_counts(lang_dir, named, id)) return {};
    if (named + id == 0) return {};
    if (!entry_at(lang_dir, 0, name_or_id, offset)) return {};
    if ((offset & 0x80000000u) != 0) return {};
    const usize data_entry = rsrc_file_off + offset;

    u32 data_rva = 0, data_size = 0;
    if (!read_u32_file(pe, data_entry, data_rva) ||
        !read_u32_file(pe, data_entry + 4, data_size) ||
        data_size == 0) {
        return {};
    }
    auto file_off = pe_rva_to_file_offset(pe, data_rva);
    if (!file_off || *file_off + data_size > pe.size()) return {};
    return std::vector<u8>(pe.begin() + static_cast<std::ptrdiff_t>(*file_off),
                           pe.begin() + static_cast<std::ptrdiff_t>(*file_off) +
                               static_cast<std::ptrdiff_t>(data_size));
}

} // namespace

std::vector<u8> extract_typelib_resource_from_pe(
    std::span<const u8> pe_image, std::optional<u32> resource_id) {
    if (pe_image.size() < 0x40 || pe_image[0] != 'M' || pe_image[1] != 'Z') {
        return {};
    }
    u32 rsrc_rva = 0;
    if (!pe_resource_dir_rva(pe_image, rsrc_rva)) return {};
    auto rsrc_off = pe_rva_to_file_offset(pe_image, rsrc_rva);
    if (!rsrc_off) return {};
    return extract_typelib_from_rsrc(pe_image, *rsrc_off, resource_id);
}

} // namespace vitreo::winapi

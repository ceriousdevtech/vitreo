/// @file resource_walker.cpp
/// @brief PE resource directory (.rsrc) walker implementation

#include "windowing/resources/resource_walker.hpp"
#include "core/memory/guest_memory.hpp"
#include <cstring>
#include <algorithm>

namespace vitreo::windowing {

namespace {

/// IMAGE_RESOURCE_DIRECTORY (16 bytes)
struct ResDir {
    u32 characteristics;
    u32 time_date_stamp;
    u16 major_version;
    u16 minor_version;
    u16 named_entries;
    u16 id_entries;
    // followed by (named_entries + id_entries) ResEntry structs
};

/// IMAGE_RESOURCE_DIRECTORY_ENTRY (8 bytes)
struct ResEntry {
    u32 name_or_id;      ///< High bit set → offset to name string; else integer ID
    u32 offset_to_data;  ///< High bit set → offset to subdirectory; else offset to data entry
};

/// IMAGE_RESOURCE_DATA_ENTRY (16 bytes)
struct ResDataEntry {
    u32 offset_to_data;  ///< RVA of the resource data
    u32 size;
    u32 code_page;
    u32 reserved;
};

static_assert(sizeof(ResDir)  == 16, "ResDir must be 16 bytes");
static_assert(sizeof(ResEntry) == 8, "ResEntry must be 8 bytes");
static_assert(sizeof(ResDataEntry) == 16, "ResDataEntry must be 16 bytes");

/// Read a struct from guest memory at (base + offset)
template<typename T>
bool read_at(const GuestMemory& mem, GuestAddr base, u32 offset, T& out) {
    GuestAddr addr = base + offset;
    auto result = mem.read_bytes(addr, reinterpret_cast<Byte*>(&out), sizeof(T));
    return result.is_ok();
}

/// Search a resource directory level for an entry matching 'target_id'
/// Returns the offset_to_data field of the matching entry, or 0 on failure.
u32 find_entry_by_id(const GuestMemory& mem, GuestAddr rsrc_base,
                     u32 dir_offset, u16 target_id) {
    ResDir dir;
    if (!read_at(mem, rsrc_base, dir_offset, dir)) return 0;

    u32 total = dir.named_entries + dir.id_entries;
    u32 entries_offset = dir_offset + sizeof(ResDir);

    for (u32 i = 0; i < total; ++i) {
        ResEntry entry;
        if (!read_at(mem, rsrc_base, entries_offset + i * sizeof(ResEntry), entry))
            return 0;

        // Skip named entries (high bit set in name_or_id)
        if (entry.name_or_id & 0x80000000) continue;

        if (static_cast<u16>(entry.name_or_id) == target_id) {
            return entry.offset_to_data;
        }
    }

    return 0;
}

/// Search a resource directory level for a named entry matching a UTF-16LE string.
/// Resource name strings are stored as: u16 length, then length u16 chars (no NUL).
u32 find_entry_by_name(const GuestMemory& mem, GuestAddr rsrc_base,
                       u32 dir_offset, const char16_t* target, u16 target_len) {
    ResDir dir;
    if (!read_at(mem, rsrc_base, dir_offset, dir)) return 0;

    u32 entries_offset = dir_offset + sizeof(ResDir);

    // Named entries come first (before id entries)
    for (u32 i = 0; i < dir.named_entries; ++i) {
        ResEntry entry;
        if (!read_at(mem, rsrc_base, entries_offset + i * sizeof(ResEntry), entry))
            return 0;

        if (!(entry.name_or_id & 0x80000000)) continue;  // not a named entry
        u32 name_offset = entry.name_or_id & 0x7FFFFFFF;

        // Read the name string length (u16)
        u16 str_len = 0;
        auto r = mem.read_bytes(rsrc_base + name_offset,
                                reinterpret_cast<Byte*>(&str_len), 2);
        if (r.is_err() || str_len != target_len) continue;

        // Read the name characters (case-insensitive compare)
        bool match = true;
        for (u16 c = 0; c < str_len; ++c) {
            u16 ch = 0;
            auto r2 = mem.read_bytes(rsrc_base + name_offset + 2 + c * 2,
                                     reinterpret_cast<Byte*>(&ch), 2);
            u16 tgt = static_cast<u16>(target[c]);
            // Uppercase both for comparison
            if (ch >= u'a' && ch <= u'z') ch -= 32;
            if (tgt >= u'a' && tgt <= u'z') tgt -= 32;
            if (r2.is_err() || ch != tgt) {
                match = false;
                break;
            }
        }
        if (match) return entry.offset_to_data;
    }

    return 0;
}

/// Search a language directory for the first entry sharing @p primary as its
/// primary language, ignoring the sublanguage. Windows resolves e.g. en-GB
/// (0x0809) against an en-US (0x0409) resource this way; without it a module
/// that ships many locales falls through to an unrelated one.
u32 find_entry_by_primary_language(const GuestMemory& mem, GuestAddr rsrc_base,
                                   u32 dir_offset, u16 primary) {
    ResDir dir;
    if (!read_at(mem, rsrc_base, dir_offset, dir)) return 0;

    u32 total = dir.named_entries + dir.id_entries;
    u32 entries_offset = dir_offset + sizeof(ResDir);
    for (u32 i = 0; i < total; ++i) {
        ResEntry entry;
        if (!read_at(mem, rsrc_base, entries_offset + i * sizeof(ResEntry), entry))
            return 0;
        if (entry.name_or_id & 0x80000000) continue;
        constexpr u16 primary_language_mask = 0x03FF;
        if ((static_cast<u16>(entry.name_or_id) & primary_language_mask) == primary) {
            return entry.offset_to_data;
        }
    }
    return 0;
}

/// Get the first entry in a directory (for language-neutral lookup)
u32 first_entry(const GuestMemory& mem, GuestAddr rsrc_base, u32 dir_offset) {
    ResDir dir;
    if (!read_at(mem, rsrc_base, dir_offset, dir)) return 0;

    u32 total = dir.named_entries + dir.id_entries;
    if (total == 0) return 0;

    u32 entries_offset = dir_offset + sizeof(ResDir);
    ResEntry entry;
    if (!read_at(mem, rsrc_base, entries_offset, entry)) return 0;

    return entry.offset_to_data;
}

u32 select_language_entry(const GuestMemory& mem, GuestAddr rsrc_base,
                          u32 dir_offset, u16 preferred_language = 0x0409) {
    if (u32 exact = find_entry_by_id(mem, rsrc_base, dir_offset, preferred_language)) {
        return exact;
    }

    // SUBLANG_NEUTRAL for this primary language (e.g. 0x0009 for English).
    constexpr u16 primary_language_mask = 0x03FF;
    const u16 primary_neutral = preferred_language & primary_language_mask;
    if (u32 primary = find_entry_by_id(mem, rsrc_base, dir_offset, primary_neutral)) {
        return primary;
    }
    // Any sublanguage of the same primary language: en-GB resolves to en-US
    // (or en-CA) rather than dropping to an unrelated locale below.
    if (u32 sibling = find_entry_by_primary_language(mem, rsrc_base, dir_offset,
                                                     primary_neutral)) {
        return sibling;
    }
    if (u32 neutral = find_entry_by_id(mem, rsrc_base, dir_offset, 0)) {
        return neutral;
    }
    // en-US before an arbitrary first entry: a multi-locale module lists
    // languages in numeric order, so first_entry() is whatever sorts lowest
    // (0x0404, Chinese Traditional, in a module that lists 23 languages)
    // rather than anything the caller would want.
    if (u32 en_us = find_entry_by_id(mem, rsrc_base, dir_offset, 0x0409)) {
        return en_us;
    }
    return first_entry(mem, rsrc_base, dir_offset);
}

} // anonymous namespace

ResourceEntry find_resource(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id,
    u16 name_id,
    u16 language
) {
    (void)rsrc_size;  // Used only for bounds checking if needed

    GuestAddr rsrc_base = image_base + rsrc_rva;

    // Level 1: Find type directory (e.g., RT_DIALOG = 5)
    u32 type_offset = find_entry_by_id(memory, rsrc_base, 0, type_id);
    if (type_offset == 0) return {};

    // Check if it's a subdirectory (high bit set)
    if (!(type_offset & 0x80000000)) return {};
    u32 name_dir_offset = type_offset & 0x7FFFFFFF;

    // Level 2: Find name/ID directory (the dialog template ID)
    u32 name_offset = find_entry_by_id(memory, rsrc_base, name_dir_offset, name_id);
    if (name_offset == 0) return {};

    // Check if it's a subdirectory
    if (!(name_offset & 0x80000000)) return {};
    u32 lang_dir_offset = name_offset & 0x7FFFFFFF;

    // Level 3: Select the requested locale before neutral/first-entry fallback.
    u32 lang_offset =
        select_language_entry(memory, rsrc_base, lang_dir_offset, language);
    if (lang_offset == 0) return {};

    // This should be a data entry (high bit NOT set)
    if (lang_offset & 0x80000000) return {};

    // Read the data entry
    ResDataEntry data_entry;
    if (!read_at(memory, rsrc_base, lang_offset, data_entry)) return {};

    // data_entry.offset_to_data is an RVA (from image base, not from rsrc section)
    GuestAddr data_addr = image_base + data_entry.offset_to_data;

    // Translate to host pointer
    auto host_ptr = memory.translate(data_addr);
    if (host_ptr.is_err()) return {};

    return ResourceEntry{
        reinterpret_cast<const u8*>(host_ptr.value()),
        data_entry.size,
        data_addr
    };
}

ResourceEntry find_resource_by_named_type(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    const char16_t* type_name,
    u16 type_name_len,
    u16 name_id
) {
    (void)rsrc_size;
    GuestAddr rsrc_base = image_base + rsrc_rva;

    // Level 1: Find type directory by name string (e.g., u"WAVE")
    u32 type_offset = find_entry_by_name(memory, rsrc_base, 0, type_name, type_name_len);
    if (type_offset == 0) return {};
    if (!(type_offset & 0x80000000)) return {};
    u32 name_dir_offset = type_offset & 0x7FFFFFFF;

    // Level 2: Find resource by integer ID
    u32 name_offset = find_entry_by_id(memory, rsrc_base, name_dir_offset, name_id);
    if (name_offset == 0) return {};
    if (!(name_offset & 0x80000000)) return {};
    u32 lang_dir_offset = name_offset & 0x7FFFFFFF;

    // Level 3: Select the guest locale before neutral/first-entry fallback.
    u32 lang_offset = select_language_entry(memory, rsrc_base, lang_dir_offset);
    if (lang_offset == 0) return {};
    if (lang_offset & 0x80000000) return {};

    ResDataEntry data_entry;
    if (!read_at(memory, rsrc_base, lang_offset, data_entry)) return {};

    GuestAddr data_addr = image_base + data_entry.offset_to_data;
    auto host_ptr = memory.translate(data_addr);
    if (host_ptr.is_err()) return {};

    return ResourceEntry{
        reinterpret_cast<const u8*>(host_ptr.value()),
        data_entry.size,
        data_addr
    };
}

ResourceEntry find_resource_by_named_type_and_name(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    const char16_t* type_name,
    u16 type_name_len,
    const char16_t* res_name,
    u16 res_name_len
) {
    (void)rsrc_size;
    GuestAddr rsrc_base = image_base + rsrc_rva;

    // Level 1: Find type directory by name string
    u32 type_offset = find_entry_by_name(memory, rsrc_base, 0, type_name, type_name_len);
    if (type_offset == 0) return {};
    if (!(type_offset & 0x80000000)) return {};
    u32 name_dir_offset = type_offset & 0x7FFFFFFF;

    // Level 2: Find resource by name string
    u32 name_offset = find_entry_by_name(memory, rsrc_base, name_dir_offset,
                                         res_name, res_name_len);
    if (name_offset == 0) return {};
    if (!(name_offset & 0x80000000)) return {};
    u32 lang_dir_offset = name_offset & 0x7FFFFFFF;

    // Level 3: Select the guest locale before neutral/first-entry fallback.
    u32 lang_offset = select_language_entry(memory, rsrc_base, lang_dir_offset);
    if (lang_offset == 0) return {};
    if (lang_offset & 0x80000000) return {};

    ResDataEntry data_entry;
    if (!read_at(memory, rsrc_base, lang_offset, data_entry)) return {};

    GuestAddr data_addr = image_base + data_entry.offset_to_data;
    auto host_ptr = memory.translate(data_addr);
    if (host_ptr.is_err()) return {};

    return ResourceEntry{
        reinterpret_cast<const u8*>(host_ptr.value()),
        data_entry.size,
        data_addr
    };
}

ResourceEntry find_first_resource(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id
) {
    (void)rsrc_size;

    GuestAddr rsrc_base = image_base + rsrc_rva;

    // Level 1: Find type directory
    u32 type_offset = find_entry_by_id(memory, rsrc_base, 0, type_id);
    if (type_offset == 0) return {};
    if (!(type_offset & 0x80000000)) return {};
    u32 name_dir_offset = type_offset & 0x7FFFFFFF;

    // Level 2: Take the FIRST name entry (instead of searching by ID)
    u32 name_offset = first_entry(memory, rsrc_base, name_dir_offset);
    if (name_offset == 0) return {};
    if (!(name_offset & 0x80000000)) return {};
    u32 lang_dir_offset = name_offset & 0x7FFFFFFF;

    // Level 3: Select the guest locale before neutral/first-entry fallback.
    u32 lang_offset = select_language_entry(memory, rsrc_base, lang_dir_offset);
    if (lang_offset == 0) return {};
    if (lang_offset & 0x80000000) return {};

    ResDataEntry data_entry;
    if (!read_at(memory, rsrc_base, lang_offset, data_entry)) return {};

    GuestAddr data_addr = image_base + data_entry.offset_to_data;
    auto host_ptr = memory.translate(data_addr);
    if (host_ptr.is_err()) return {};

    return ResourceEntry{
        reinterpret_cast<const u8*>(host_ptr.value()),
        data_entry.size,
        data_addr
    };
}

std::vector<ResourceNameId> enumerate_resource_names(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id,
    bool type_is_string,
    const char16_t* type_name,
    u16 type_name_len
) {
    (void)rsrc_size;
    std::vector<ResourceNameId> names;
    if (rsrc_rva == 0) {
        return names;
    }

    GuestAddr rsrc_base = image_base + rsrc_rva;
    u32 type_offset = 0;
    if (type_is_string) {
        if (type_name == nullptr || type_name_len == 0) {
            return names;
        }
        type_offset = find_entry_by_name(memory, rsrc_base, 0, type_name, type_name_len);
    } else {
        type_offset = find_entry_by_id(memory, rsrc_base, 0, type_id);
    }
    if (type_offset == 0 || !(type_offset & 0x80000000)) {
        return names;
    }

    const u32 name_dir_offset = type_offset & 0x7FFFFFFF;
    ResDir dir;
    if (!read_at(memory, rsrc_base, name_dir_offset, dir)) {
        return names;
    }

    const u32 total = static_cast<u32>(dir.named_entries) +
                      static_cast<u32>(dir.id_entries);
    const u32 entries_offset = name_dir_offset + sizeof(ResDir);
    names.reserve(total);

    for (u32 i = 0; i < total; ++i) {
        ResEntry entry;
        if (!read_at(memory, rsrc_base, entries_offset + i * sizeof(ResEntry),
                     entry)) {
            break;
        }

        ResourceNameId item;
        if (entry.name_or_id & 0x80000000) {
            const u32 name_offset = entry.name_or_id & 0x7FFFFFFF;
            u16 str_len = 0;
            auto len_r = memory.read_bytes(
                rsrc_base + name_offset,
                reinterpret_cast<Byte*>(&str_len), 2);
            if (len_r.is_err()) {
                continue;
            }
            item.is_id = false;
            item.name.resize(str_len);
            for (u16 c = 0; c < str_len; ++c) {
                u16 ch = 0;
                auto ch_r = memory.read_bytes(
                    rsrc_base + name_offset + 2 + c * 2,
                    reinterpret_cast<Byte*>(&ch), 2);
                if (ch_r.is_err()) {
                    item.name.clear();
                    break;
                }
                item.name[c] = static_cast<char16_t>(ch);
            }
            if (item.name.empty() && str_len != 0) {
                continue;
            }
        } else {
            item.is_id = true;
            item.id = static_cast<u16>(entry.name_or_id);
        }
        names.push_back(std::move(item));
    }
    return names;
}

ResourceLanguageEnum enumerate_resource_languages(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id,
    bool type_is_string,
    const char16_t* type_name,
    u16 type_name_len,
    u16 name_id,
    bool name_is_string,
    const char16_t* res_name,
    u16 res_name_len
) {
    (void)rsrc_size;
    ResourceLanguageEnum result;
    if (rsrc_rva == 0) {
        return result;
    }

    GuestAddr rsrc_base = image_base + rsrc_rva;
    u32 type_offset = 0;
    if (type_is_string) {
        if (type_name == nullptr || type_name_len == 0) {
            return result;
        }
        type_offset = find_entry_by_name(memory, rsrc_base, 0, type_name, type_name_len);
    } else {
        type_offset = find_entry_by_id(memory, rsrc_base, 0, type_id);
    }
    if (type_offset == 0 || !(type_offset & 0x80000000)) {
        return result;
    }

    const u32 name_dir_offset = type_offset & 0x7FFFFFFF;
    u32 name_offset = 0;
    if (name_is_string) {
        if (res_name == nullptr || res_name_len == 0) {
            result.status = ResourceLanguageEnum::Status::NameNotFound;
            return result;
        }
        name_offset = find_entry_by_name(memory, rsrc_base, name_dir_offset,
                                         res_name, res_name_len);
    } else {
        name_offset = find_entry_by_id(memory, rsrc_base, name_dir_offset, name_id);
    }
    if (name_offset == 0 || !(name_offset & 0x80000000)) {
        result.status = ResourceLanguageEnum::Status::NameNotFound;
        return result;
    }

    const u32 lang_dir_offset = name_offset & 0x7FFFFFFF;
    ResDir dir;
    if (!read_at(memory, rsrc_base, lang_dir_offset, dir)) {
        result.status = ResourceLanguageEnum::Status::NameNotFound;
        return result;
    }

    const u32 total = static_cast<u32>(dir.named_entries) +
                      static_cast<u32>(dir.id_entries);
    const u32 entries_offset = lang_dir_offset + sizeof(ResDir);
    result.languages.reserve(total);
    for (u32 i = 0; i < total; ++i) {
        ResEntry entry;
        if (!read_at(memory, rsrc_base, entries_offset + i * sizeof(ResEntry),
                     entry)) {
            break;
        }
        if (entry.name_or_id & 0x80000000) {
            continue;
        }
        result.languages.push_back(static_cast<u16>(entry.name_or_id));
    }
    result.status = ResourceLanguageEnum::Status::Ok;
    return result;
}

ResourceEntry find_resource_by_string_name(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id,
    const char16_t* res_name,
    u16 res_name_len
) {
    (void)rsrc_size;
    GuestAddr rsrc_base = image_base + rsrc_rva;

    // Level 1: Find type directory by integer ID (e.g., RT_BITMAP = 2)
    u32 type_offset = find_entry_by_id(memory, rsrc_base, 0, type_id);
    if (type_offset == 0) return {};
    if (!(type_offset & 0x80000000)) return {};
    u32 name_dir_offset = type_offset & 0x7FFFFFFF;

    // Level 2: Find resource by string name
    u32 name_offset = find_entry_by_name(memory, rsrc_base, name_dir_offset, res_name, res_name_len);
    if (name_offset == 0) return {};
    if (!(name_offset & 0x80000000)) return {};
    u32 lang_dir_offset = name_offset & 0x7FFFFFFF;

    // Level 3: Select the guest locale before neutral/first-entry fallback.
    u32 lang_offset = select_language_entry(memory, rsrc_base, lang_dir_offset);
    if (lang_offset == 0) return {};
    if (lang_offset & 0x80000000) return {};

    ResDataEntry data_entry;
    if (!read_at(memory, rsrc_base, lang_offset, data_entry)) return {};

    GuestAddr data_addr = image_base + data_entry.offset_to_data;
    auto host_ptr = memory.translate(data_addr);
    if (host_ptr.is_err()) return {};

    return ResourceEntry{
        reinterpret_cast<const u8*>(host_ptr.value()),
        data_entry.size,
        data_addr
    };
}

// =============================================================================
// Icon extraction — assemble a .ICO file from RT_GROUP_ICON + RT_ICON
// =============================================================================

#pragma pack(push, 1)

/// GRPICONDIR header in a RT_GROUP_ICON resource
struct GrpIconDir {
    u16 reserved;   // 0
    u16 type;       // 1 = icon
    u16 count;      // number of images
};

/// Per-image entry in the GRPICONDIR (14 bytes)
struct GrpIconDirEntry {
    u8  width;
    u8  height;
    u8  color_count;
    u8  reserved;
    u16 planes;
    u16 bit_count;
    u32 bytes_in_res;
    u16 id;           // ordinal of the RT_ICON resource
};

/// ICO file directory entry (16 bytes — same as GrpIconDirEntry but with
/// a 4-byte file offset instead of a 2-byte resource ID)
struct IcoDirEntry {
    u8  width;
    u8  height;
    u8  color_count;
    u8  reserved;
    u16 planes;
    u16 bit_count;
    u32 bytes_in_res;
    u32 image_offset;   // offset from start of file
};

#pragma pack(pop)

static_assert(sizeof(GrpIconDirEntry) == 14, "GrpIconDirEntry must be 14 bytes");
static_assert(sizeof(IcoDirEntry) == 16, "IcoDirEntry must be 16 bytes");

/// Common helper: given a GROUP_ICON resource entry, assemble an ICO file.
static std::vector<u8> assemble_ico_from_group(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    const ResourceEntry& group
) {
    if (!group.data || group.size < sizeof(GrpIconDir)) {
        return {};
    }

    GrpIconDir hdr;
    std::memcpy(&hdr, group.data, sizeof(hdr));
    if (hdr.type != 1 || hdr.count == 0) {
        VITREO_TRACE( "[icon] Invalid GROUP_ICON header (type=%u count=%u)\n",
                     hdr.type, hdr.count);
        return {};
    }

    const u32 dir_bytes = static_cast<u32>(hdr.count) * sizeof(GrpIconDirEntry);
    if (sizeof(GrpIconDir) + dir_bytes > group.size) {
        VITREO_TRACE( "[icon] GROUP_ICON truncated\n");
        return {};
    }

    std::vector<GrpIconDirEntry> entries(hdr.count);
    std::memcpy(entries.data(),
                group.data + sizeof(GrpIconDir),
                dir_bytes);

    struct IconBlob {
        GrpIconDirEntry dir;
        std::vector<u8> data;
    };
    std::vector<IconBlob> blobs;
    blobs.reserve(hdr.count);

    for (auto& e : entries) {
        auto icon = find_resource(memory, image_base, rsrc_rva, rsrc_size,
                                  RT::ICON, e.id);
        if (!icon.data || icon.size == 0) {
            VITREO_TRACE( "[icon] RT_ICON id=%u not found, skipping\n", e.id);
            continue;
        }
        IconBlob blob;
        blob.dir = e;
        blob.data.assign(icon.data, icon.data + icon.size);

        // Older PE resources may store the raw DIB height (XOR bitmap plus
        // AND mask) in GRPICONDIR instead of the logical icon height. An ICO
        // directory entry must use the logical dimensions. Derive them from
        // BITMAPINFOHEADER when present so consumers such as NSImage do not
        // interpret a 32x32 icon as 32x64 and render it blank.
        if (blob.data.size() >= 12) {
            u32 dib_header_size = 0;
            i32 dib_width = 0;
            i32 dib_height = 0;
            std::memcpy(&dib_header_size, blob.data.data(), sizeof(dib_header_size));
            std::memcpy(&dib_width, blob.data.data() + 4, sizeof(dib_width));
            std::memcpy(&dib_height, blob.data.data() + 8, sizeof(dib_height));
            if (dib_header_size >= 40 && dib_header_size <= 124 &&
                dib_width > 0 && dib_height != 0) {
                const u32 logical_width = static_cast<u32>(dib_width);
                const i64 absolute_height =
                    dib_height < 0 ? -static_cast<i64>(dib_height)
                                   : static_cast<i64>(dib_height);
                const u32 logical_height =
                    static_cast<u32>(absolute_height / 2);
                blob.dir.width = logical_width >= 256
                    ? 0
                    : static_cast<u8>(logical_width);
                blob.dir.height = logical_height >= 256
                    ? 0
                    : static_cast<u8>(logical_height);
            }
        }
        blobs.push_back(std::move(blob));
    }

    if (blobs.empty()) {
        VITREO_TRACE( "[icon] No usable RT_ICON entries\n");
        return {};
    }

    const u16 count = static_cast<u16>(blobs.size());
    const u32 header_size = 6;
    const u32 dir_size    = count * sizeof(IcoDirEntry);
    u32 data_offset       = header_size + dir_size;

    u32 total = data_offset;
    for (auto& b : blobs) total += static_cast<u32>(b.data.size());

    std::vector<u8> ico(total);
    ico[0] = 0; ico[1] = 0;
    ico[2] = 1; ico[3] = 0;
    ico[4] = static_cast<u8>(count);
    ico[5] = static_cast<u8>(count >> 8);

    u32 img_offset = data_offset;
    for (u16 i = 0; i < count; ++i) {
        IcoDirEntry ide{};
        ide.width        = blobs[i].dir.width;
        ide.height       = blobs[i].dir.height;
        ide.color_count  = blobs[i].dir.color_count;
        ide.reserved     = 0;
        ide.planes       = blobs[i].dir.planes;
        ide.bit_count    = blobs[i].dir.bit_count;
        ide.bytes_in_res = static_cast<u32>(blobs[i].data.size());
        ide.image_offset = img_offset;

        std::memcpy(&ico[header_size + i * sizeof(IcoDirEntry)],
                    &ide, sizeof(IcoDirEntry));

        std::memcpy(&ico[img_offset], blobs[i].data.data(), blobs[i].data.size());
        img_offset += static_cast<u32>(blobs[i].data.size());
    }

    VITREO_TRACE( "[icon] Assembled ICO: %u images, %u bytes\n", count, total);
    return ico;
}

std::vector<u8> extract_app_icon(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size
) {
    auto group = find_first_resource(memory, image_base, rsrc_rva, rsrc_size,
                                     RT::GROUP_ICON);
    if (!group.data || group.size < sizeof(GrpIconDir)) {
        VITREO_TRACE( "[icon] No RT_GROUP_ICON found\n");
        return {};
    }
    return assemble_ico_from_group(memory, image_base, rsrc_rva, rsrc_size, group);
}

std::vector<u8> extract_icon_by_group_id(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 group_id
) {
    auto group = find_resource(memory, image_base, rsrc_rva, rsrc_size,
                               RT::GROUP_ICON, group_id);
    if (!group.data || group.size < sizeof(GrpIconDir)) {
        VITREO_TRACE( "[icon] RT_GROUP_ICON id=%u not found\n", group_id);
        return {};
    }
    return assemble_ico_from_group(memory, image_base, rsrc_rva, rsrc_size, group);
}

// =============================================================================
// Cursor decode — RT_CURSOR DIB and CreateCursor AND/XOR masks
// =============================================================================

namespace {

constexpr u32 kMaxCursorDim = 256;

u32 cursor_dib_stride(u32 width, u32 bpp) {
    return ((width * bpp + 31u) / 32u) * 4u;
}

u32 cursor_word_stride(u32 width) {
    return ((width + 15u) / 16u) * 2u;
}

bool cursor_bit(const u8* row, u32 x) {
    return (row[x / 8u] & static_cast<u8>(0x80u >> (x % 8u))) != 0;
}

void write_rgba(std::vector<u8>& rgba, u32 x, u32 y, u32 width,
                u8 r, u8 g, u8 b, u8 a) {
    const usize i = (static_cast<usize>(y) * width + x) * 4u;
    rgba[i + 0] = r;
    rgba[i + 1] = g;
    rgba[i + 2] = b;
    rgba[i + 3] = a;
}

void apply_and_xor_pixel(std::vector<u8>& rgba, u32 x, u32 y, u32 width,
                         bool and_bit, bool xor_bit) {
    // Learn CreateCursor truth table:
    // AND 0 XOR 0 = black, AND 0 XOR 1 = white,
    // AND 1 XOR 0 = screen (transparent), AND 1 XOR 1 = reverse screen.
    // A host OS cursor cannot invert the pixels under the pointer, so both
    // screen cases are transparent. Painting invert as opaque white produced
    // a solid box around color cursors whose XOR leftover is non-black.
    if (and_bit) {
        write_rgba(rgba, x, y, width, 0, 0, 0, 0);
    } else if (xor_bit) {
        write_rgba(rgba, x, y, width, 255, 255, 255, 255);
    } else {
        write_rgba(rgba, x, y, width, 0, 0, 0, 255);
    }
}

} // namespace

DecodedCursor decode_and_xor_cursor(
    u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
    const u8* and_plane, const u8* xor_plane
) {
    if (and_plane == nullptr || xor_plane == nullptr ||
        width == 0 || height == 0 ||
        width > kMaxCursorDim || height > kMaxCursorDim) {
        return {};
    }
    const u32 stride = cursor_word_stride(width);
    DecodedCursor out;
    out.width = width;
    out.height = height;
    out.hotspot_x = hotspot_x;
    out.hotspot_y = hotspot_y;
    out.rgba.assign(static_cast<usize>(width) * height * 4u, 0);
    for (u32 y = 0; y < height; ++y) {
        const u8* and_row = and_plane + static_cast<usize>(y) * stride;
        const u8* xor_row = xor_plane + static_cast<usize>(y) * stride;
        for (u32 x = 0; x < width; ++x) {
            apply_and_xor_pixel(out.rgba, x, y, width,
                                cursor_bit(and_row, x), cursor_bit(xor_row, x));
        }
    }
    return out;
}

namespace {

/// Read a little-endian value at @p off. Callers bounds-check first.
template <typename T>
[[nodiscard]] T read_le(const u8* p, usize off) {
    T v{};
    std::memcpy(&v, p + off, sizeof(T));
    return v;
}

constexpr u32 kAniAttrIcon = 0x1u;      // frames are .cur/.ico files
constexpr u32 kAniAttrSequence = 0x2u;  // a `seq ` chunk is present

/// One jiffy is 1/60 s. Windows clamps a zero rate to the header default and
/// a still-zero rate to one jiffy; a zero-delay frame would otherwise spin.
[[nodiscard]] u32 jiffies_to_ms(u32 jiffies) {
    if (jiffies == 0) jiffies = 1;
    return (jiffies * 1000u + 30u) / 60u;
}

} // namespace

DecodedCursor decode_cur_file(const u8* data, u32 size, u32 want_dim) {
    // ICONDIR: idReserved(0) idType(1=icon, 2=cursor) idCount
    if (data == nullptr || size < 6) return {};
    const u16 type = read_le<u16>(data, 2);
    const u16 count = read_le<u16>(data, 4);
    if (read_le<u16>(data, 0) != 0 || count == 0 ||
        (type != 1 && type != 2)) {
        return {};
    }
    constexpr u32 kEntry = 16;
    if (static_cast<u64>(6) + static_cast<u64>(count) * kEntry > size) return {};

    // Pick the entry closest to want_dim. A 0 dimension byte means 256.
    u32 best = 0;
    u32 best_score = ~0u;
    for (u32 i = 0; i < count; ++i) {
        const u32 off = 6 + i * kEntry;
        const u32 w = data[off] == 0 ? 256u : data[off];
        const u32 score = w > want_dim ? (w - want_dim) : (want_dim - w);
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    const u32 off = 6 + best * kEntry;
    // For a CUR the two words after the reserved byte are the hotspot; for an
    // ICO they are planes/bitcount, which is not a hotspot and must not be
    // used as one.
    const u32 hot_x = (type == 2) ? read_le<u16>(data, off + 4) : 0u;
    const u32 hot_y = (type == 2) ? read_le<u16>(data, off + 6) : 0u;
    const u32 bytes = read_le<u32>(data, off + 8);
    const u32 image_off = read_le<u32>(data, off + 12);
    if (bytes == 0 || static_cast<u64>(image_off) + bytes > size) return {};

    // A Vista-era entry may hold a PNG rather than a DIB. Report failure
    // rather than decoding a PNG header as a BITMAPINFOHEADER.
    static constexpr u8 kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes >= sizeof(kPng) &&
        std::memcmp(data + image_off, kPng, sizeof(kPng)) == 0) {
        return {};
    }

    // decode_rt_cursor expects the RT_CURSOR layout: hotspot then DIB. Give it
    // one rather than duplicating the DIB/palette/mask handling here.
    std::vector<u8> rt(4 + static_cast<usize>(bytes));
    rt[0] = static_cast<u8>(hot_x & 0xFFu);
    rt[1] = static_cast<u8>((hot_x >> 8) & 0xFFu);
    rt[2] = static_cast<u8>(hot_y & 0xFFu);
    rt[3] = static_cast<u8>((hot_y >> 8) & 0xFFu);
    std::memcpy(rt.data() + 4, data + image_off, bytes);
    return decode_rt_cursor(rt.data(), static_cast<u32>(rt.size()));
}

DecodedAnimatedCursor decode_ani_cursor(const u8* data, u32 size) {
    if (data == nullptr || size < 12 || std::memcmp(data, "RIFF", 4) != 0 ||
        std::memcmp(data + 8, "ACON", 4) != 0) {
        return {};
    }
    const u32 riff_size = read_le<u32>(data, 4);
    // The RIFF size counts everything after the size field itself.
    const u32 end = (riff_size <= size - 8) ? (8 + riff_size) : size;

    u32 n_steps = 0;
    u32 default_rate = 0;
    u32 attributes = 0;
    std::vector<u32> rates;
    std::vector<u32> sequence;
    std::vector<std::vector<u8>> stored;  // storage order
    DecodedAnimatedCursor out;

    // Walk the top-level chunks. Chunk payloads are WORD aligned.
    u32 off = 12;
    while (off + 8 <= end) {
        const u8* id = data + off;
        const u32 chunk = read_le<u32>(data, off + 4);
        const u32 body = off + 8;
        if (chunk > end - body) break;

        if (std::memcmp(id, "anih", 4) == 0 && chunk >= 36) {
            n_steps = read_le<u32>(data, body + 8);
            default_rate = read_le<u32>(data, body + 28);
            attributes = read_le<u32>(data, body + 32);
        } else if (std::memcmp(id, "rate", 4) == 0) {
            for (u32 i = 0; i + 4 <= chunk; i += 4) {
                rates.push_back(read_le<u32>(data, body + i));
            }
        } else if (std::memcmp(id, "seq ", 4) == 0) {
            for (u32 i = 0; i + 4 <= chunk; i += 4) {
                sequence.push_back(read_le<u32>(data, body + i));
            }
        } else if (std::memcmp(id, "LIST", 4) == 0 && chunk >= 4 &&
                   std::memcmp(data + body, "fram", 4) == 0) {
            u32 sub = body + 4;
            const u32 sub_end = body + chunk;
            while (sub + 8 <= sub_end) {
                const u32 sub_size = read_le<u32>(data, sub + 4);
                const u32 sub_body = sub + 8;
                if (sub_size > sub_end - sub_body) break;
                if (std::memcmp(data + sub, "icon", 4) == 0) {
                    // AF_ICON clear means the frame is raw RT_CURSOR data.
                    DecodedCursor frame =
                        (attributes & kAniAttrIcon) != 0
                            ? decode_cur_file(data + sub_body, sub_size)
                            : decode_rt_cursor(data + sub_body, sub_size);
                    if (!frame.rgba.empty()) {
                        if (out.frames.empty() && stored.empty()) {
                            out.width = frame.width;
                            out.height = frame.height;
                            out.hotspot_x = frame.hotspot_x;
                            out.hotspot_y = frame.hotspot_y;
                        }
                        // Every frame must share the first frame's geometry:
                        // the host installs one cursor size for the sequence.
                        if (frame.width == out.width &&
                            frame.height == out.height) {
                            stored.push_back(std::move(frame.rgba));
                        } else {
                            stored.emplace_back();
                        }
                    } else {
                        stored.emplace_back();
                    }
                }
                sub = sub_body + sub_size + (sub_size & 1u);
            }
        }
        off = body + chunk + (chunk & 1u);
    }

    if (stored.empty() || out.width == 0 || out.height == 0) return {};

    // Expand the display sequence. Without a `seq ` chunk each stored frame is
    // shown once in order, which is also the fallback when the chunk is
    // malformed.
    const bool have_seq =
        (attributes & kAniAttrSequence) != 0 && !sequence.empty();
    const u32 steps = have_seq ? static_cast<u32>(sequence.size())
                               : static_cast<u32>(stored.size());
    (void)n_steps;
    for (u32 i = 0; i < steps; ++i) {
        const u32 index = have_seq ? sequence[i] : i;
        if (index >= stored.size() || stored[index].empty()) continue;
        out.frames.push_back(stored[index]);
        const u32 jiffies = (i < rates.size()) ? rates[i] : default_rate;
        out.delays_ms.push_back(jiffies_to_ms(jiffies));
    }
    if (out.frames.empty()) return {};
    return out;
}

DecodedCursor decode_rt_cursor(const u8* data, u32 size) {
    if (data == nullptr || size < 4 + 40) {
        return {};
    }
    const u32 hotspot_x = static_cast<u32>(data[0] | (data[1] << 8));
    const u32 hotspot_y = static_cast<u32>(data[2] | (data[3] << 8));
    const u8* dib = data + 4;
    const u32 dib_size = size - 4;
    u32 header_size = 0;
    i32 dib_width = 0;
    i32 dib_height_raw = 0;
    u16 bit_count = 0;
    std::memcpy(&header_size, dib, 4);
    std::memcpy(&dib_width, dib + 4, 4);
    std::memcpy(&dib_height_raw, dib + 8, 4);
    std::memcpy(&bit_count, dib + 14, 2);
    if (header_size < 40 || header_size > 124 || header_size > dib_size ||
        dib_width <= 0 || dib_height_raw == 0 ||
        static_cast<u32>(dib_width) > kMaxCursorDim) {
        return {};
    }
    const u32 width = static_cast<u32>(dib_width);
    const u32 abs_height = dib_height_raw < 0
        ? static_cast<u32>(-dib_height_raw)
        : static_cast<u32>(dib_height_raw);
    if (abs_height == 0 || abs_height > kMaxCursorDim * 2u) {
        return {};
    }

    u32 palette_entries = 0;
    if (bit_count <= 8) {
        u32 clr_used = 0;
        if (header_size >= 36) {
            std::memcpy(&clr_used, dib + 32, 4);
        }
        palette_entries = clr_used != 0 ? clr_used : (1u << bit_count);
    }
    const u32 palette_bytes = palette_entries * 4u;
    const u32 xor_stride = cursor_dib_stride(width, bit_count);
    const u32 and_stride = cursor_dib_stride(width, 1);
    const u32 pixels_off = header_size + palette_bytes;
    if (pixels_off > dib_size) {
        return {};
    }
    const u32 pixel_bytes = dib_size - pixels_off;

    // Cursor DIBs store XOR then AND, so biHeight is usually 2× the visual
    // height. Fall back to XOR-only when the AND mask is absent.
    u32 height = abs_height;
    bool use_and = false;
    if (abs_height % 2u == 0) {
        const u32 half = abs_height / 2u;
        const u32 xor_half = xor_stride * half;
        const u32 and_half = and_stride * half;
        if (half > 0 && half <= kMaxCursorDim &&
            xor_half + and_half <= pixel_bytes) {
            height = half;
            use_and = true;
        } else if (half > 0 && half <= kMaxCursorDim &&
                   xor_half <= pixel_bytes &&
                   xor_stride * abs_height > pixel_bytes) {
            height = half;
        }
    }
    if (height == 0 || height > kMaxCursorDim ||
        xor_stride * height > pixel_bytes) {
        return {};
    }
    const u8* palette = dib + header_size;
    const u8* xor_bits = dib + pixels_off;
    const u8* and_bits = use_and ? xor_bits + xor_stride * height : nullptr;

    DecodedCursor out;
    out.width = width;
    out.height = height;
    out.hotspot_x = hotspot_x;
    out.hotspot_y = hotspot_y;
    out.rgba.assign(static_cast<usize>(width) * height * 4u, 0);

    auto xor_color = [&](u32 x, u32 src_row, u8& r, u8& g, u8& b, u8& a) {
        a = 255;
        const u8* row = xor_bits + static_cast<usize>(src_row) * xor_stride;
        if (bit_count == 1) {
            const u32 idx = cursor_bit(row, x) ? 1u : 0u;
            if (idx < palette_entries) {
                b = palette[idx * 4u + 0];
                g = palette[idx * 4u + 1];
                r = palette[idx * 4u + 2];
            } else {
                r = g = b = idx ? 255 : 0;
            }
            return;
        }
        if (bit_count == 4) {
            const u8 packed = row[x / 2u];
            const u32 idx = (x & 1u) ? (packed & 0x0Fu) : (packed >> 4);
            if (idx < palette_entries) {
                b = palette[idx * 4u + 0];
                g = palette[idx * 4u + 1];
                r = palette[idx * 4u + 2];
            }
            return;
        }
        if (bit_count == 8) {
            const u32 idx = row[x];
            if (idx < palette_entries) {
                b = palette[idx * 4u + 0];
                g = palette[idx * 4u + 1];
                r = palette[idx * 4u + 2];
            }
            return;
        }
        if (bit_count == 24) {
            const u8* px = row + static_cast<usize>(x) * 3u;
            b = px[0];
            g = px[1];
            r = px[2];
            return;
        }
        if (bit_count == 32) {
            const u8* px = row + static_cast<usize>(x) * 4u;
            b = px[0];
            g = px[1];
            r = px[2];
            a = px[3];
            return;
        }
        r = g = b = 0;
    };

    // A 32bpp image with any non-zero alpha is drawn by its alpha channel and
    // the AND mask is ignored; one whose alpha bytes are all zero is opaque,
    // with the AND mask deciding transparency (s3gdi_icon_probe: "alpha" and
    // "alpha+halfmask" DrawIconEx NORMAL are identical, "opaque" takes the
    // mask). Same rule as icon_display_rgba for CreateIconIndirect.
    bool has_alpha = false;
    if (bit_count == 32) {
        for (u32 row = 0; row < height && !has_alpha; ++row) {
            const u8* px = xor_bits + static_cast<usize>(row) * xor_stride;
            for (u32 x = 0; x < width; ++x) {
                if (px[static_cast<usize>(x) * 4u + 3u] != 0) {
                    has_alpha = true;
                    break;
                }
            }
        }
    }

    for (u32 y = 0; y < height; ++y) {
        const u32 src_row = height - 1u - y;
        const u8* and_row = and_bits
            ? and_bits + static_cast<usize>(src_row) * and_stride
            : nullptr;
        for (u32 x = 0; x < width; ++x) {
            u8 r = 0, g = 0, b = 0, a = 255;
            xor_color(x, src_row, r, g, b, a);
            if (has_alpha) {
                write_rgba(out.rgba, x, y, width, r, g, b, a);
                continue;
            }
            a = 255;
            const bool and_bit = and_row ? cursor_bit(and_row, x) : false;
            if (and_bit) {
                r = 0;
                g = 0;
                b = 0;
                a = 0;
            }
            write_rgba(out.rgba, x, y, width, r, g, b, a);
        }
    }
    return out;
}

namespace {
DecodedCursor cursor_from_group(const GuestMemory& memory, GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size,
                                const ResourceEntry& group);
}  // namespace

std::vector<u8> extract_icon_by_group_name(const GuestMemory& memory, GuestAddr image_base, u32 rsrc_rva,
                                           u32 rsrc_size, const char16_t* name, u16 name_len) {
    auto group = find_resource_by_string_name(memory, image_base, rsrc_rva, rsrc_size, RT::GROUP_ICON, name,
                                              name_len);
    if (!group.data || group.size < sizeof(GrpIconDir)) return {};
    return assemble_ico_from_group(memory, image_base, rsrc_rva, rsrc_size, group);
}

DecodedCursor extract_cursor_by_group_name(const GuestMemory& memory, GuestAddr image_base, u32 rsrc_rva,
                                           u32 rsrc_size, const char16_t* name, u16 name_len) {
    auto group = find_resource_by_string_name(memory, image_base, rsrc_rva, rsrc_size, RT::GROUP_CURSOR, name,
                                              name_len);
    if (!group.data || group.size < sizeof(GrpIconDir)) return {};
    return cursor_from_group(memory, image_base, rsrc_rva, rsrc_size, group);
}

DecodedCursor extract_cursor_by_group_id(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 group_id
) {
    auto group = find_resource(memory, image_base, rsrc_rva, rsrc_size,
                               RT::GROUP_CURSOR, group_id);
    if (!group.data || group.size < sizeof(GrpIconDir)) {
        VITREO_TRACE("[cursor] RT_GROUP_CURSOR id=%u not found\n", group_id);
        return {};
    }
    return cursor_from_group(memory, image_base, rsrc_rva, rsrc_size, group);
}

namespace {
DecodedCursor cursor_from_group(const GuestMemory& memory, GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size,
                                const ResourceEntry& group) {
    GrpIconDir hdr;
    std::memcpy(&hdr, group.data, sizeof(hdr));
    if ((hdr.type != 2 && hdr.type != 1) || hdr.count == 0) {
        return {};
    }
    const u32 dir_bytes = static_cast<u32>(hdr.count) * sizeof(GrpIconDirEntry);
    if (sizeof(GrpIconDir) + dir_bytes > group.size) {
        return {};
    }
    for (u16 i = 0; i < hdr.count; ++i) {
        GrpIconDirEntry entry{};
        std::memcpy(&entry,
                    group.data + sizeof(GrpIconDir) + i * sizeof(GrpIconDirEntry),
                    sizeof(entry));
        auto cursor = find_resource(memory, image_base, rsrc_rva, rsrc_size,
                                    RT::CURSOR, entry.id);
        if (!cursor.data || cursor.size == 0) {
            continue;
        }
        DecodedCursor decoded = decode_rt_cursor(cursor.data, cursor.size);
        if (!decoded.rgba.empty()) {
            return decoded;
        }
    }
    return {};
}
}  // namespace

} // namespace vitreo::windowing

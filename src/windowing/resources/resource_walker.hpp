#pragma once
/// @file resource_walker.hpp
/// @brief PE resource directory (.rsrc) walker

#include <vitreo/common.hpp>
#include <string>
#include <vector>

namespace vitreo { class GuestMemory; }

namespace vitreo::windowing {

/// Standard resource type IDs (RT_*)
namespace RT {
    constexpr u16 CURSOR       = 1;
    constexpr u16 BITMAP       = 2;
    constexpr u16 ICON         = 3;
    constexpr u16 MENU         = 4;
    constexpr u16 DIALOG       = 5;
    constexpr u16 STRING       = 6;
    constexpr u16 FONTDIR      = 7;
    constexpr u16 FONT         = 8;
    constexpr u16 ACCELERATOR  = 9;
    constexpr u16 RCDATA       = 10;
    constexpr u16 MESSAGETABLE = 11;
    constexpr u16 GROUP_CURSOR = 12;
    constexpr u16 GROUP_ICON   = 14;
    constexpr u16 VERSION      = 16;
    constexpr u16 MANIFEST     = 24;
} // namespace RT

/// Result of a resource lookup: pointer to raw data + size
struct ResourceEntry {
    const u8* data = nullptr;
    u32 size       = 0;
    GuestAddr guest_addr = 0;  ///< Guest virtual address of the data
};

/// One name/ID under a resource type (for EnumResourceNames).
struct ResourceNameId {
    bool is_id = false;
    u16 id = 0;
    std::u16string name;
};

/// Walk the PE resource directory tree in guest memory.
///
/// The resource directory is a 3-level tree: type → name/ID → language.
/// Given the resource directory RVA and the image base, this walks the
/// directory to find a resource by type and ID.
///
/// @param memory      Guest memory (PE sections already mapped)
/// @param image_base  Base address where PE was loaded
/// @param rsrc_rva    RVA of the resource directory (data_directories[DIR_RESOURCE].VirtualAddress)
/// @param rsrc_size   Size of the resource directory (data_directories[DIR_RESOURCE].Size)
/// @param type_id     Resource type (e.g., RT::DIALOG = 5)
/// @param name_id     Resource name/ID (e.g., the dialog template ID)
/// @param language    Preferred LANGID (from FindResourceEx); falls back
///                    through primary-language, neutral, then first entry.
///                    Modules that ship many languages (one ships 23) resolve
///                    to an unrelated locale without this.
/// @return ResourceEntry with data pointer and size, or empty on failure
[[nodiscard]] ResourceEntry find_resource(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id,
    u16 name_id,
    u16 language = 0x0409);

/// Find the first resource of a given type (any name/ID).
/// Useful for extracting the main app icon (first RT_GROUP_ICON).
[[nodiscard]] ResourceEntry find_first_resource(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id);

/// Find a resource where the type is a named string (e.g., u"WAVE")
/// rather than a standard RT_* integer ID.
[[nodiscard]] ResourceEntry find_resource_by_named_type(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    const char16_t* type_name,
    u16 type_name_len,
    u16 name_id);

/// Find a resource where both the type and the name are strings.
/// Windows allows either level of the resource tree to be named, so this
/// completes the four type/name combinations FindResource accepts.
[[nodiscard]] ResourceEntry find_resource_by_named_type_and_name(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    const char16_t* type_name,
    u16 type_name_len,
    const char16_t* res_name,
    u16 res_name_len);

/// Find a resource where the name is a string (e.g., "CARDS")
/// rather than an integer ID. Type is still a standard RT_* integer.
[[nodiscard]] ResourceEntry find_resource_by_string_name(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id,
    const char16_t* res_name,
    u16 res_name_len);

/// RT_GROUP_ICON / RT_GROUP_CURSOR looked up by string name (LoadIcon /
/// LoadCursor / LoadImage with a name): the assembled .ICO image, or the
/// first decodable cursor; empty when the group is missing.
[[nodiscard]] std::vector<u8> extract_icon_by_group_name(
    const GuestMemory& memory, GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size,
    const char16_t* name, u16 name_len);
struct DecodedCursor;
[[nodiscard]] DecodedCursor extract_cursor_by_group_name(
    const GuestMemory& memory, GuestAddr image_base, u32 rsrc_rva, u32 rsrc_size,
    const char16_t* name, u16 name_len);

/// Enumerate name/ID entries under a resource type for EnumResourceNames.
/// Returns an empty vector when the type directory is missing.
[[nodiscard]] std::vector<ResourceNameId> enumerate_resource_names(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 type_id,
    bool type_is_string,
    const char16_t* type_name,
    u16 type_name_len);

/// Result of walking type → name → language for EnumResourceLanguages.
struct ResourceLanguageEnum {
    enum class Status { Ok, TypeNotFound, NameNotFound };
    Status status = Status::TypeNotFound;
    std::vector<u16> languages;
};

/// Enumerate LANGIDs under a type/name pair for EnumResourceLanguages.
[[nodiscard]] ResourceLanguageEnum enumerate_resource_languages(
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
    u16 res_name_len);

/// Extract the application icon from PE resources as a .ICO file blob.
/// Looks up the first RT_GROUP_ICON, resolves each RT_ICON entry,
/// and assembles a standard ICO file in memory.
/// Returns an empty vector on failure.
[[nodiscard]] std::vector<u8> extract_app_icon(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size);

/// Extract a specific icon group resource by its ID and assemble a .ICO file.
/// Similar to extract_app_icon but targets a specific RT_GROUP_ICON by ID.
/// Returns an empty vector on failure.
[[nodiscard]] std::vector<u8> extract_icon_by_group_id(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 group_id);

/// Top-down RGBA8 cursor decoded from a PE RT_CURSOR or CreateCursor masks.
struct DecodedCursor {
    u32 width = 0;
    u32 height = 0;
    u32 hotspot_x = 0;
    u32 hotspot_y = 0;
    std::vector<u8> rgba;
};

/// Decode an RT_CURSOR payload: LOCALHEADER hotspot plus a DIB (XOR + AND).
[[nodiscard]] DecodedCursor decode_rt_cursor(const u8* data, u32 size);

/// Decode a .CUR / .ICO *file* image (ICONDIR + ICONDIRENTRY + DIB).
///
/// This is not the RT_CURSOR layout: the hotspot lives in the directory entry
/// rather than in a 4-byte prefix, and there may be several images. The best
/// match for @p want_dim is chosen, or the first entry when nothing matches.
[[nodiscard]] DecodedCursor decode_cur_file(const u8* data, u32 size,
                                            u32 want_dim = 32);

/// One animated cursor: the displayed sequence, already expanded.
///
/// `frames` is in STEP order, not storage order — an ANI may show a stored
/// frame more than once via its `seq ` chunk, and callers should not have to
/// know that. `delays_ms` is parallel to it.
struct DecodedAnimatedCursor {
    u32 width = 0;
    u32 height = 0;
    u32 hotspot_x = 0;
    u32 hotspot_y = 0;
    std::vector<std::vector<u8>> frames;
    std::vector<u32> delays_ms;
};

/// Decode a RIFF/ACON animated cursor (the payload of an .ani file, an
/// RT_ANICURSOR resource, or a CreateIconFromResource blob that starts with
/// "RIFF"). Returns an empty `frames` when the data is not an ACON RIFF or no
/// frame decodes.
[[nodiscard]] DecodedAnimatedCursor decode_ani_cursor(const u8* data, u32 size);

/// Decode CreateCursor AND/XOR planes (1bpp, WORD-aligned scanlines, top-down).
/// AND/XOR use the documented CreateCursor truth table; screen and
/// reverse-screen pixels are transparent because the host cannot invert
/// under the pointer.
[[nodiscard]] DecodedCursor decode_and_xor_cursor(
    u32 width, u32 height, u32 hotspot_x, u32 hotspot_y,
    const u8* and_plane, const u8* xor_plane);

/// Load RT_GROUP_CURSOR @p group_id and decode its first usable RT_CURSOR.
[[nodiscard]] DecodedCursor extract_cursor_by_group_id(
    const GuestMemory& memory,
    GuestAddr image_base,
    u32 rsrc_rva,
    u32 rsrc_size,
    u16 group_id);

} // namespace vitreo::windowing

// SPDX-License-Identifier: LGPL-2.1-only
/// @file sxs_section.cpp
/// @brief Activation-context DLL redirection section (see header).

#include "winapi/common/sxs/sxs_section.hpp"

#include <algorithm>
#include <cwctype>

namespace vitreo::winapi::sxs {

namespace {

std::u16string utf16(std::string_view utf8) {
    std::u16string out;
    for (usize i = 0; i < utf8.size();) {
        const u8 b = static_cast<u8>(utf8[i]);
        u32 cp = b;
        usize n = 1;
        if (b >= 0xF0) { cp = b & 0x07u; n = 4; }
        else if (b >= 0xE0) { cp = b & 0x0Fu; n = 3; }
        else if (b >= 0xC0) { cp = b & 0x1Fu; n = 2; }
        for (usize k = 1; k < n && i + k < utf8.size(); ++k)
            cp = (cp << 6) | (static_cast<u8>(utf8[i + k]) & 0x3Fu);
        i += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
    return out;
}

void put32(std::vector<u8>& b, usize at, u32 v) {
    b[at] = static_cast<u8>(v);
    b[at + 1] = static_cast<u8>(v >> 8);
    b[at + 2] = static_cast<u8>(v >> 16);
    b[at + 3] = static_cast<u8>(v >> 24);
}

constexpr u32 kHeaderSize = 0x2c;
constexpr u32 kElementSize = 24;
constexpr u32 kDataSize = 0x14;
constexpr u32 kPathOmitsAssemblyRoot = 2;

}  // namespace

u32 pseudo_key(std::string_view name) {
    u32 h = 0;
    for (const char16_t unit : utf16(name)) {
        const char16_t upper = unit < 0x80
                                   ? static_cast<char16_t>((unit >= 'a' && unit <= 'z') ? unit - 32 : unit)
                                   : static_cast<char16_t>(std::towupper(static_cast<wint_t>(unit)));
        h = h * 65599u + upper;
    }
    return h;
}

DllRedirectionSection build_dll_redirection_section(const ContextData& context) {
    struct Item {
        u32 key;
        std::string name;   // as the manifest spells it
        std::u16string wide;
        u32 roster;
    };
    std::vector<Item> items;
    for (usize index = 0; index < context.assemblies.size(); ++index) {
        for (const std::string& file : context.assemblies[index].files) {
            items.push_back({pseudo_key(file), file, utf16(file), static_cast<u32>(index + 1)});
        }
    }
    // Keys and data follow the roster (assembly, then file) order; only the
    // element list is sorted by pseudo-key (measured with keys from two
    // assemblies: actctx_os_assemblies.*.expected).
    DllRedirectionSection section;
    const u32 count = static_cast<u32>(items.size());
    usize size = kHeaderSize + count * kElementSize;
    std::vector<std::pair<u32, u32>> placed;  // key offset, data offset (roster order)
    for (const Item& item : items) {
        const u32 key_offset = static_cast<u32>(size);
        size += (item.wide.size() + 1) * 2;
        size = (size + 3) & ~usize{3};
        placed.emplace_back(key_offset, static_cast<u32>(size));
        size += kDataSize;
    }
    std::vector<u8>& b = section.bytes;
    b.assign(size, 0);
    put32(b, 0x00, 0x64487353u);  // 'SsHd'
    put32(b, 0x04, kHeaderSize);
    put32(b, 0x08, 1);
    put32(b, 0x0c, 1);
    put32(b, 0x10, 3);
    put32(b, 0x14, count);
    put32(b, 0x18, kHeaderSize);
    put32(b, 0x1c, 1);
    std::vector<u32> order(count);
    for (u32 i = 0; i < count; ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return items[a].key < items[b].key; });
    for (u32 slot = 0; slot < count; ++slot) {
        const u32 i = order[slot];
        const Item& item = items[i];
        const auto [key_offset, data_offset] = placed[i];
        const usize element = kHeaderSize + slot * kElementSize;
        put32(b, element, item.key);
        put32(b, element + 4, key_offset);
        put32(b, element + 8, static_cast<u32>(item.wide.size() * 2));
        put32(b, element + 12, data_offset);
        put32(b, element + 16, kDataSize);
        put32(b, element + 20, item.roster);
        for (usize k = 0; k < item.wide.size(); ++k) {
            b[key_offset + k * 2] = static_cast<u8>(item.wide[k]);
            b[key_offset + k * 2 + 1] = static_cast<u8>(item.wide[k] >> 8);
        }
        put32(b, data_offset, kDataSize);
        put32(b, data_offset + 4, kPathOmitsAssemblyRoot);
        section.entries[to_lower(item.name)] = {data_offset, kDataSize, item.roster};
    }
    return section;
}

}  // namespace vitreo::winapi::sxs

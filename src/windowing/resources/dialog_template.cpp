/// @file dialog_template.cpp
/// @brief DLGTEMPLATE / DLGTEMPLATEEX parser implementation

#include "windowing/resources/dialog_template.hpp"
#include <cstring>

namespace vitreo::windowing {

namespace {

/// Safe reader over a byte buffer with bounds checking
class TemplateReader {
public:
    TemplateReader(const u8* data, usize size)
        : m_data{data}, m_size{size}, m_pos{0} {}

    [[nodiscard]] bool has(usize bytes) const { return m_pos + bytes <= m_size; }
    [[nodiscard]] usize pos() const { return m_pos; }

    void align_to(usize boundary) {
        usize rem = m_pos % boundary;
        if (rem != 0) m_pos += (boundary - rem);
    }

    [[nodiscard]] bool read_u8(u8& out) {
        if (!has(1)) return false;
        out = m_data[m_pos++];
        return true;
    }

    [[nodiscard]] bool read_u16(u16& out) {
        if (!has(2)) return false;
        std::memcpy(&out, m_data + m_pos, 2);
        m_pos += 2;
        return true;
    }

    [[nodiscard]] bool read_i16(i16& out) {
        if (!has(2)) return false;
        std::memcpy(&out, m_data + m_pos, 2);
        m_pos += 2;
        return true;
    }

    [[nodiscard]] bool read_u32(u32& out) {
        if (!has(4)) return false;
        std::memcpy(&out, m_data + m_pos, 4);
        m_pos += 4;
        return true;
    }

    /// Read a sz_Or_Ord value:
    /// - If first WORD is 0x0000 → empty (no value)
    /// - If first WORD is 0xFFFF → ordinal follows as next WORD
    /// - Otherwise → null-terminated UTF-16LE string
    /// Returns true if successfully read, ordinal is set for ordinal case
    [[nodiscard]] bool read_sz_or_ord(std::wstring& str, u16& ordinal) {
        str.clear();
        ordinal = 0;

        u16 first;
        if (!read_u16(first)) return false;

        if (first == 0x0000) {
            return true;  // empty
        }
        if (first == 0xFFFF) {
            if (!read_u16(ordinal)) return false;
            return true;
        }

        // It's a string starting with 'first'
        str += static_cast<wchar_t>(first);
        for (;;) {
            u16 ch;
            if (!read_u16(ch)) return false;
            if (ch == 0) break;
            str += static_cast<wchar_t>(ch);
        }
        return true;
    }

    /// Read a null-terminated UTF-16LE string
    [[nodiscard]] bool read_wstring(std::wstring& str) {
        str.clear();
        for (;;) {
            u16 ch;
            if (!read_u16(ch)) return false;
            if (ch == 0) break;
            str += static_cast<wchar_t>(ch);
        }
        return true;
    }

    void skip(usize bytes) { m_pos += bytes; }

private:
    const u8* m_data;
    usize m_size;
    usize m_pos;
};

/// Parse a DLGTEMPLATEEX (version 1, signature 0xFFFF)
bool parse_extended(TemplateReader& r, DialogTemplate& tmpl) {
    tmpl.is_extended = true;

    // Already consumed dlgVer(2) + signature(2)
    if (!r.read_u32(tmpl.help_id)) return false;
    if (!r.read_u32(tmpl.ex_style)) return false;
    if (!r.read_u32(tmpl.style)) return false;

    if (!r.read_u16(tmpl.item_count)) return false;
    if (!r.read_i16(tmpl.x))  return false;
    if (!r.read_i16(tmpl.y))  return false;
    if (!r.read_i16(tmpl.cx)) return false;
    if (!r.read_i16(tmpl.cy)) return false;

    // menu sz_Or_Ord
    u16 menu_ord = 0;
    if (!r.read_sz_or_ord(tmpl.menu, menu_ord)) return false;

    // windowClass sz_Or_Ord
    u16 class_ord = 0;
    if (!r.read_sz_or_ord(tmpl.window_class, class_ord)) return false;

    // title (null-terminated wstring)
    if (!r.read_wstring(tmpl.title)) return false;

    // Font info (if DS_SETFONT or DS_SHELLFONT)
    if (tmpl.style & DS::SETFONT) {
        if (!r.read_u16(tmpl.font_size)) return false;
        if (!r.read_u16(tmpl.font_weight)) return false;
        if (!r.read_u8(tmpl.font_italic)) return false;
        if (!r.read_u8(tmpl.font_charset)) return false;
        if (!r.read_wstring(tmpl.font_face)) return false;
    }

    // Parse each DLGITEMTEMPLATEEX
    tmpl.items.reserve(tmpl.item_count);
    for (u16 i = 0; i < tmpl.item_count; ++i) {
        r.align_to(4);  // Each item must start on DWORD boundary

        DialogItem item;
        u32 item_help;
        if (!r.read_u32(item_help)) return false;
        if (!r.read_u32(item.ex_style)) return false;
        if (!r.read_u32(item.style)) return false;
        if (!r.read_i16(item.x))  return false;
        if (!r.read_i16(item.y))  return false;
        if (!r.read_i16(item.cx)) return false;
        if (!r.read_i16(item.cy)) return false;

        // DLGITEMTEMPLATEEX has DWORD id (not WORD)
        if (!r.read_u32(item.id)) return false;

        // windowClass sz_Or_Ord
        u16 cls_ord = 0;
        std::wstring cls_name;
        if (!r.read_sz_or_ord(cls_name, cls_ord)) return false;
        if (cls_ord != 0) {
            item.class_ordinal = cls_ord;
        } else {
            item.class_name = std::move(cls_name);
        }

        // title sz_Or_Ord (text for control, or resource ID for icons etc.)
        u16 title_ord = 0;
        if (!r.read_sz_or_ord(item.title, title_ord)) return false;
        item.title_ordinal = title_ord;

        // extraCount
        if (!r.read_u16(item.extra_count)) return false;
        if (item.extra_count > 0) {
            if (!r.has(item.extra_count)) return false;
            r.skip(item.extra_count);
        }

        tmpl.items.push_back(std::move(item));
    }

    return true;
}

/// Parse a classic DLGTEMPLATE
bool parse_classic(TemplateReader& r, DialogTemplate& tmpl) {
    tmpl.is_extended = false;

    // First 4 bytes already consumed as the "dlgVer+signature" check,
    // but for classic templates those are style(u32). We need to re-read.
    // Actually, the caller already peeked at bytes 0-3. Let's handle this
    // properly — the caller will reset position for classic.

    if (!r.read_u32(tmpl.style)) return false;
    if (!r.read_u32(tmpl.ex_style)) return false;
    if (!r.read_u16(tmpl.item_count)) return false;
    if (!r.read_i16(tmpl.x))  return false;
    if (!r.read_i16(tmpl.y))  return false;
    if (!r.read_i16(tmpl.cx)) return false;
    if (!r.read_i16(tmpl.cy)) return false;

    // menu sz_Or_Ord
    u16 menu_ord = 0;
    if (!r.read_sz_or_ord(tmpl.menu, menu_ord)) return false;

    // windowClass sz_Or_Ord
    u16 class_ord = 0;
    if (!r.read_sz_or_ord(tmpl.window_class, class_ord)) return false;

    // title
    if (!r.read_wstring(tmpl.title)) return false;

    // Font info (if DS_SETFONT)
    if (tmpl.style & DS::SETFONT) {
        if (!r.read_u16(tmpl.font_size)) return false;
        if (!r.read_wstring(tmpl.font_face)) return false;
    }

    // Parse each DLGITEMTEMPLATE
    tmpl.items.reserve(tmpl.item_count);
    for (u16 i = 0; i < tmpl.item_count; ++i) {
        r.align_to(4);  // DWORD-aligned

        DialogItem item;
        if (!r.read_u32(item.style)) return false;
        if (!r.read_u32(item.ex_style)) return false;
        if (!r.read_i16(item.x))  return false;
        if (!r.read_i16(item.y))  return false;
        if (!r.read_i16(item.cx)) return false;
        if (!r.read_i16(item.cy)) return false;

        // Classic DLGITEMTEMPLATE has WORD id
        u16 wid;
        if (!r.read_u16(wid)) return false;
        item.id = static_cast<u32>(wid);

        // windowClass sz_Or_Ord
        u16 cls_ord = 0;
        std::wstring cls_name;
        if (!r.read_sz_or_ord(cls_name, cls_ord)) return false;
        if (cls_ord != 0) {
            item.class_ordinal = cls_ord;
        } else {
            item.class_name = std::move(cls_name);
        }

        // title sz_Or_Ord
        u16 title_ord = 0;
        if (!r.read_sz_or_ord(item.title, title_ord)) return false;
        item.title_ordinal = title_ord;

        // extraCount
        if (!r.read_u16(item.extra_count)) return false;
        if (item.extra_count > 0) {
            if (!r.has(item.extra_count)) return false;
            r.skip(item.extra_count);
        }

        tmpl.items.push_back(std::move(item));
    }

    return true;
}

} // anonymous namespace

VResult<DialogTemplate> parse_dialog_template(const u8* data, usize size) {
    if (!data || size < 18) {
        return VResult<DialogTemplate>::err(Error::InvalidArgument);
    }

    // Check if this is DLGTEMPLATEEX: first WORD=1, second WORD=0xFFFF
    u16 w0, w1;
    std::memcpy(&w0, data, 2);
    std::memcpy(&w1, data + 2, 2);

    DialogTemplate tmpl;

    if (w0 == 1 && w1 == 0xFFFF) {
        // Extended dialog template
        TemplateReader r(data + 4, size - 4);  // skip version + signature
        if (!parse_extended(r, tmpl)) {
            return VResult<DialogTemplate>::err(Error::InvalidArgument);
        }
    } else {
        // Classic dialog template — starts with style(u32), exStyle(u32) ...
        TemplateReader r(data, size);
        if (!parse_classic(r, tmpl)) {
            return VResult<DialogTemplate>::err(Error::InvalidArgument);
        }
    }

    return VResult<DialogTemplate>::ok(std::move(tmpl));
}

} // namespace vitreo::windowing

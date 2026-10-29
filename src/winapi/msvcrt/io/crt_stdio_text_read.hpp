#pragma once
/// @file crt_stdio_text_read.hpp
/// @brief Translated text reads shared by the std-descriptor and _read paths.
///
/// A template over the byte source, so it must be visible to both
/// crt_stdio_std_descriptors.cpp (std_read) and
/// crt_lowio_transfer_registration.cpp (the _read export).

#include <vitreo/types.hpp>

#include <cerrno>
#include <optional>
#include <vector>

namespace vitreo::winapi::crt_stdio_internal {

// Learn translation-mode constants and crt_unicode_read_utf16.expected.
// Translate complete UTF-16LE units, never individual bytes. Windows peeks
// past a trailing CR: a pipe retains the peek locally, whereas a disk file
// rewinds it. When a disk read already delivered text, a peeked LF is left
// for the next read. An incomplete final unit is consumed but not delivered.
template<class Read, class Rewind>
i64 read_translated_text(u8* dst, u32 count, bool& eof,
                    std::optional<u16>& lookahead, bool seekable, bool wide,
                    Read&& read, Rewind&& rewind) {
    if (wide && (count & 1u) != 0) { errno = EINVAL; return -1; }
    const u32 width = wide ? 2u : 1u;
    if (eof) return 0;
    std::vector<u8> raw(count);
    u32 prefix = 0;
    if (lookahead) {
        raw[0] = static_cast<u8>(*lookahead);
        if (wide) raw[1] = static_cast<u8>(*lookahead >> 8);
        lookahead.reset();
        prefix = width;
    }
    const i64 n = prefix == count ? 0 : read(raw.data() + prefix, count - prefix);
    if (n <= 0) return n;
    const usize got = (static_cast<usize>(n) + prefix) / width * width;
    u32 out = 0;
    const auto unit_at = [wide](const u8* p) {
        return static_cast<u16>(p[0] | (wide ? (static_cast<u16>(p[1]) << 8) : 0));
    };
    const auto emit = [&](u16 unit) {
        dst[out++] = static_cast<u8>(unit);
        if (wide) dst[out++] = static_cast<u8>(unit >> 8);
    };
    for (usize i = 0; i < got; i += width) {
        const u16 c = unit_at(raw.data() + i);
        if (c == 0x1a) { eof = true; break; }
        if (c != '\r') { emit(c); continue; }
        if (i + width < got) {
            if (unit_at(raw.data() + i + width) == '\n') {
                emit('\n');
                i += width;
            } else emit('\r');
            continue;
        }
        u8 next_bytes[2];
        const i64 next_got = read(next_bytes, width);
        if (next_got != width) { emit('\r'); continue; }
        const u16 next = unit_at(next_bytes);
        if (seekable) {
            if (next == '\n' && out == 0) emit('\n');
            else {
                if (!rewind(width)) return -1;
                if (next != '\n') emit('\r');
            }
        } else if (next == '\n') emit('\n');
        else { lookahead = next; emit('\r'); }
    }
    return out;
}

} // namespace vitreo::winapi::crt_stdio_internal

// SPDX-License-Identifier: LGPL-2.1-only

/// @file format_message.cpp
/// @brief FormatMessageW / FormatMessageA engine (see format_message.hpp for
///        the contract and where each rule was measured).

#include "winapi/kernel32/text/format_message.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/locale_string.hpp"
#include "winapi/kernel32/locale/locale_string_internal.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include <vitreo/diagnostics.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace vitreo::winapi::format_message {
namespace {

// winerror.h (Windows SDK 10.0.26100.0) codes error.hpp does not carry.
constexpr u32 kErrorNoWorkDone = 235;          // ERROR_NO_WORK_DONE
constexpr u32 kErrorMuiFileNotFound = 15100;   // ERROR_MUI_FILE_NOT_FOUND

// Measured (format_message_probe n200/n201, n1000, n200_ign/n201_ign).
constexpr u32 kMaxInsert = 200;
constexpr u32 kMaxInsertNumber = 999;
// Measured: FormatMessageA converts at most this many UTF-16 units each way
// (big_literal_32766/32767, insert32766_then_x).
constexpr usize kAnsiConversionLimit = 32766;
// Measured: ALLOCATE_BUFFER works in units of 64K WCHARs (cap_* cases).
constexpr u64 kAllocUnit = 65536;

constexpr u16 kCr = u'\r';
constexpr u16 kLf = u'\n';

enum class Status { Ok, InvalidParameter, Fault };

// -----------------------------------------------------------------------------
// Output with the line-width rules.
// -----------------------------------------------------------------------------

enum class Kind : u8 { Regular, Breakable, Insert };

class Output {
public:
    explicit Output(u32 width) : width_(width) {}

    [[nodiscard]] bool wrapping() const { return width_ != 0 && width_ != kMaxWidthMask; }

    void put(u16 c, Kind kind) {
        chars_.push_back(c);
        kinds_.push_back(kind);
        ++col_;
        check();
    }
    void put_insert(const std::vector<u16>& units) {
        for (u16 c : units) {
            chars_.push_back(c);
            kinds_.push_back(Kind::Insert);
        }
        col_ += static_cast<u32>(units.size());
        check();
    }
    /// "%n", and a source line break when the width byte is 0.
    void hard_break() {
        push_crlf();
        line_start_ = chars_.size();
        col_ = 0;
    }
    /// "%r": a bare CR that starts the next line.
    void carriage_return() {
        chars_.push_back(kCr);
        kinds_.push_back(Kind::Regular);
        line_start_ = chars_.size();
        col_ = 0;
    }
    /// "%t": with a line width the tab is a break opportunity that moves the
    /// column to the next multiple of 8, then counts itself (tab_* cases).
    void tab() {
        if (!wrapping()) {
            put(u'\t', Kind::Regular);
            return;
        }
        chars_.push_back(u'\t');
        kinds_.push_back(Kind::Breakable);
        col_ = (col_ / 8 + 1) * 8 + 1;
        check();
    }
    /// A CR, LF, CRLF or LFCR in the source text.
    void source_break() {
        if (width_ == 0) {
            hard_break();
        } else {
            put(u' ', Kind::Breakable);
        }
    }

    [[nodiscard]] const std::vector<u16>& chars() const { return chars_; }

private:
    void push_crlf() {
        chars_.push_back(kCr);
        chars_.push_back(kLf);
        kinds_.push_back(Kind::Regular);
        kinds_.push_back(Kind::Regular);
    }
    [[nodiscard]] bool trimmable(usize i) const {
        if (kinds_[i] == Kind::Insert) return false;
        return chars_[i] == u' ' ||
               (chars_[i] == u'\t' && kinds_[i] == Kind::Breakable);
    }
    /// Once the column reaches the width: break at the last breakable space
    /// of the line, dropping the run of spaces around it; with none, end the
    /// line here. The check runs once per emitted unit, never again for the
    /// remainder it leaves (wrap_insert_w4: an over-long insert stays whole).
    void check() {
        if (!wrapping() || col_ < width_) return;
        const usize end = chars_.size();
        usize idx = end;
        for (usize i = end; i > line_start_; --i) {
            if (kinds_[i - 1] == Kind::Breakable) {
                idx = i - 1;
                break;
            }
        }
        if (idx == end) {
            push_crlf();
            line_start_ = chars_.size();
            col_ = 0;
            return;
        }
        usize left = idx;
        while (left > line_start_ && trimmable(left - 1)) --left;
        usize right = idx + 1;
        while (right < end && trimmable(right)) ++right;
        std::vector<u16> rest_chars(chars_.begin() + static_cast<std::ptrdiff_t>(right),
                                    chars_.end());
        std::vector<Kind> rest_kinds(kinds_.begin() + static_cast<std::ptrdiff_t>(right),
                                     kinds_.end());
        chars_.resize(left);
        kinds_.resize(left);
        push_crlf();
        line_start_ = chars_.size();
        chars_.insert(chars_.end(), rest_chars.begin(), rest_chars.end());
        kinds_.insert(kinds_.end(), rest_kinds.begin(), rest_kinds.end());
        col_ = static_cast<u32>(rest_chars.size());
    }

    u32 width_;
    std::vector<u16> chars_;
    std::vector<Kind> kinds_;
    usize line_start_ = 0;
    u32 col_ = 0;
};

// -----------------------------------------------------------------------------
// Guest reads
// -----------------------------------------------------------------------------

struct Reader {
    Marshaler& m;
    bool is64;
    bool faulted = false;
    GuestAddr fault_addr = 0;

    bool fail(GuestAddr a) {
        if (!faulted) {
            faulted = true;
            fault_addr = a;
        }
        return false;
    }
    bool u8_at(GuestAddr a, u8& v) {
        auto r = m.memory().read_u8(a);
        if (!r.is_ok()) return fail(a);
        v = r.value();
        return true;
    }
    bool u16_at(GuestAddr a, u16& v) {
        auto r = m.memory().read_u16(a);
        if (!r.is_ok()) return fail(a);
        v = r.value();
        return true;
    }
    bool u32_at(GuestAddr a, u32& v) {
        auto r = m.memory().read_u32(a);
        if (!r.is_ok()) return fail(a);
        v = r.value();
        return true;
    }
    bool u64_at(GuestAddr a, u64& v) {
        auto r = m.memory().read_u64(a);
        if (!r.is_ok()) return fail(a);
        v = r.value();
        return true;
    }
    /// One pointer-sized (4 or 8 byte) value, zero-extended.
    bool slot_at(GuestAddr a, u64& v) {
        if (is64) return u64_at(a, v);
        u32 w = 0;
        if (!u32_at(a, w)) return false;
        v = w;
        return true;
    }
    [[nodiscard]] u32 slot() const { return is64 ? 8u : 4u; }

    /// NUL-terminated UTF-16 string, at most @p max units.
    bool wide_string(GuestAddr a, usize max, std::vector<u16>& out) {
        for (usize i = 0; i < max; ++i) {
            u16 c = 0;
            if (!u16_at(a + i * 2, c)) return false;
            if (c == 0) break;
            out.push_back(c);
        }
        return true;
    }
    /// NUL-terminated byte string, at most @p max bytes.
    bool narrow_string(GuestAddr a, usize max, std::string& out) {
        for (usize i = 0; i < max; ++i) {
            u8 c = 0;
            if (!u8_at(a + i, c)) return false;
            if (c == 0) break;
            out.push_back(static_cast<char>(c));
        }
        return true;
    }
};

std::vector<u16> widen(u32 code_page, const std::string& bytes) {
    std::vector<u16> out;
    if (bytes.empty()) return out;
    out.resize(bytes.size() * 2 + 2);
    const auto r = codepage::multi_byte_to_wide(
        code_page, 0, reinterpret_cast<const u8*>(bytes.data()), bytes.size(),
        out.data(), out.size());
    out.resize(r.count);
    return out;
}

std::string narrow(const std::vector<u16>& units) {
    if (units.empty()) return {};
    std::vector<u8> buf(units.size() * 4 + 4);
    const auto r = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, units.data(),
                                                units.size(), buf.data(), buf.size(),
                                                nullptr, false);
    return std::string(reinterpret_cast<const char*>(buf.data()), r.count);
}

void append_ascii(std::vector<u16>& out, std::string_view s) {
    for (char c : s) out.push_back(static_cast<u16>(static_cast<unsigned char>(c)));
}

// -----------------------------------------------------------------------------
// The printf that formats one insert (ntdll's: format_message_probe pf_*).
// -----------------------------------------------------------------------------

enum class Size { Default, Short, Long, LongLong, I64, I32, Ptr };

struct Conversion {
    bool left = false;
    bool plus = false;
    bool space = false;
    bool alt = false;
    bool zero = false;
    bool width_star = false;
    i64 width = 0;
    bool precision_star = false;
    i64 precision = -1;
    Size size = Size::Default;
    bool narrow_prefix = false;  // h
    bool wide_prefix = false;    // l, w
    u16 type = 0;                // 0: the format ended inside the spec
    usize next = 0;              // index after the conversion
};

bool is_conversion_char(u16 c) {
    switch (c) {
        case u'd': case u'i': case u'o': case u'u': case u'x': case u'X':
        case u'c': case u'C': case u's': case u'S': case u'Z': case u'p':
        case u'n': case u'e': case u'E': case u'f': case u'g': case u'G':
        case u'a': case u'A':
            return true;
        default:
            return false;
    }
}

/// Parses the conversion that starts after the '%' at @p i.
Conversion parse_conversion(const std::vector<u16>& f, usize i) {
    Conversion c;
    const usize n = f.size();
    for (; i < n; ++i) {
        const u16 ch = f[i];
        if (ch == u'-') c.left = true;
        else if (ch == u'+') c.plus = true;
        else if (ch == u' ') c.space = true;
        else if (ch == u'#') c.alt = true;
        else if (ch == u'0') c.zero = true;
        else break;
    }
    if (i < n && f[i] == u'*') {
        c.width_star = true;
        ++i;
    } else {
        for (; i < n && f[i] >= u'0' && f[i] <= u'9'; ++i) {
            c.width = std::min<i64>(c.width * 10 + (f[i] - u'0'), 1 << 24);
        }
    }
    if (i < n && f[i] == u'.') {
        ++i;
        c.precision = 0;
        if (i < n && f[i] == u'*') {
            c.precision_star = true;
            ++i;
        } else {
            for (; i < n && f[i] >= u'0' && f[i] <= u'9'; ++i) {
                c.precision = std::min<i64>(c.precision * 10 + (f[i] - u'0'), 1 << 24);
            }
        }
    }
    for (bool more = true; more && i < n;) {
        switch (f[i]) {
            case u'h':
                c.size = Size::Short;
                c.narrow_prefix = true;
                ++i;
                if (i < n && f[i] == u'h') ++i;  // hh is h (pf_hhd)
                break;
            case u'l':
                ++i;
                if (i < n && f[i] == u'l') {
                    c.size = Size::LongLong;
                    ++i;
                } else {
                    c.size = Size::Long;
                    c.wide_prefix = true;
                }
                break;
            case u'w':
                c.wide_prefix = true;
                ++i;
                break;
            case u'j':
                c.size = Size::LongLong;
                ++i;
                break;
            case u'z': case u't':
                c.size = Size::Ptr;
                ++i;
                break;
            case u'L': case u'F': case u'N':
                ++i;  // no effect on the conversions this printf has (pf_Ld, pf_F)
                break;
            case u'I':
                if (i + 2 < n && f[i + 1] == u'6' && f[i + 2] == u'4') {
                    c.size = Size::I64;
                    i += 3;
                } else if (i + 2 < n && f[i + 1] == u'3' && f[i + 2] == u'2') {
                    c.size = Size::I32;
                    i += 3;
                } else if (i + 1 < n && is_conversion_char(f[i + 1])) {
                    c.size = Size::Ptr;
                    ++i;
                } else {
                    more = false;  // a lone 'I' is the conversion (pf_I16d, pf_Ilu)
                }
                break;
            default:
                more = false;
                break;
        }
    }
    if (i < n) {
        c.type = f[i];
        c.next = i + 1;
    } else {
        c.next = n;
    }
    return c;
}

/// The values one insert's printf consumes: the cached first value, then
/// the argument memory at the cursor.
class ArgStream {
public:
    ArgStream(Reader& rd, std::vector<u64> first, GuestAddr raw)
        : rd_(rd), first_(std::move(first)), raw_(raw) {}

    bool next_slot(u64& v) {
        if (pos_ < first_.size()) {
            v = first_[pos_++];
            return true;
        }
        if (!rd_.slot_at(raw_, v)) return false;
        raw_ += rd_.slot();
        return true;
    }
    bool next_int32(u32& v) {
        u64 s = 0;
        if (!next_slot(s)) return false;
        v = static_cast<u32>(s);
        return true;
    }
    bool next_int64(u64& v) {
        if (rd_.is64) return next_slot(v);
        u64 lo = 0, hi = 0;
        if (!next_slot(lo) || !next_slot(hi)) return false;
        v = (lo & 0xFFFFFFFFull) | ((hi & 0xFFFFFFFFull) << 32);
        return true;
    }
    bool next_ptr(u64& v) { return next_slot(v); }

private:
    Reader& rd_;
    std::vector<u64> first_;
    usize pos_ = 0;
    GuestAddr raw_;
};

void pad_and_append(std::vector<u16>& out, const std::vector<u16>& body,
                    const Conversion& c, i64 width, u16 pad_char) {
    const usize w = width > 0 ? static_cast<usize>(width) : 0;
    const usize pad = w > body.size() ? w - body.size() : 0;
    if (!c.left) out.insert(out.end(), pad, pad_char);
    out.insert(out.end(), body.begin(), body.end());
    if (c.left) out.insert(out.end(), pad, u' ');
}

std::vector<u16> format_integer(u64 magnitude, bool negative, bool is_signed,
                                u16 type, const Conversion& c, i64 width,
                                i64 precision) {
    std::string digits;
    const unsigned base = type == u'o' ? 8 : (type == u'x' || type == u'X') ? 16 : 10;
    const char* table = type == u'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    for (u64 v = magnitude; v != 0; v /= base) digits.insert(digits.begin(), table[v % base]);
    if (precision < 0 && digits.empty()) digits = "0";
    if (precision >= 0 && static_cast<i64>(digits.size()) < precision) {
        digits.insert(digits.begin(), static_cast<usize>(precision) - digits.size(), '0');
    }
    std::string prefix;
    if (is_signed) {
        if (negative) prefix = "-";
        else if (c.plus) prefix = "+";
        else if (c.space) prefix = " ";
    }
    if (c.alt) {
        if (type == u'o') {
            if (digits.empty() || digits.front() != '0') digits.insert(digits.begin(), '0');
        } else if ((type == u'x' || type == u'X') && magnitude != 0) {
            prefix += type == u'X' ? "0X" : "0x";
        }
    }
    std::vector<u16> out;
    const usize len = prefix.size() + digits.size();
    const usize w = width > 0 ? static_cast<usize>(width) : 0;
    if (c.zero && !c.left && precision < 0 && w > len) {
        append_ascii(out, prefix);
        out.insert(out.end(), w - len, u'0');
        append_ascii(out, digits);
        return out;
    }
    std::vector<u16> body;
    append_ascii(body, prefix);
    append_ascii(body, digits);
    pad_and_append(out, body, c, width, u' ');
    return out;
}

struct PrintfContext {
    Reader& rd;
    bool wide_entry;  // FormatMessageW: %s and %c are wide by default
};

/// Formats @p fmt (the insert's "%" + spec) into @p out. False on a fault.
bool run_printf(PrintfContext& ctx, const std::vector<u16>& fmt, ArgStream& args,
                std::vector<u16>& out) {
    Reader& rd = ctx.rd;
    for (usize i = 0; i < fmt.size();) {
        if (fmt[i] != u'%') {
            out.push_back(fmt[i++]);
            continue;
        }
        Conversion c = parse_conversion(fmt, i + 1);
        i = c.next;
        i64 width = c.width;
        i64 precision = c.precision;
        if (c.width_star) {
            u32 v = 0;
            if (!args.next_int32(v)) return false;
            width = static_cast<i32>(v);
            if (width < 0) {
                c.left = true;
                width = -width;
            }
        }
        if (c.precision_star) {
            u32 v = 0;
            if (!args.next_int32(v)) return false;
            precision = static_cast<i32>(v);
            if (precision < 0) precision = -1;
        }
        if (c.type == 0) break;  // the spec ended before a conversion: nothing
        const bool lower = c.type >= u'a' && c.type <= u'z';
        const bool wide_text = c.narrow_prefix ? false
                             : c.wide_prefix   ? true
                             : (lower == ctx.wide_entry);
        switch (c.type) {
            case u'%':
                out.push_back(u'%');
                break;
            case u'd': case u'i': case u'u': case u'o': case u'x': case u'X': {
                const bool is_signed = c.type == u'd' || c.type == u'i';
                u64 raw = 0;
                bool ok = true;
                u32 bits = 32;
                switch (c.size) {
                    case Size::Short: {
                        u32 v = 0;
                        ok = args.next_int32(v);
                        raw = v;
                        bits = 16;
                        break;
                    }
                    case Size::LongLong: case Size::I64:
                        ok = args.next_int64(raw);
                        bits = 64;
                        break;
                    case Size::Ptr:
                        ok = args.next_ptr(raw);
                        bits = rd.is64 ? 64 : 32;
                        break;
                    default: {
                        u32 v = 0;
                        ok = args.next_int32(v);
                        raw = v;
                        break;
                    }
                }
                if (!ok) return false;
                if (bits < 64) raw &= (1ull << bits) - 1;
                bool negative = false;
                u64 magnitude = raw;
                if (is_signed && bits < 64 && (raw >> (bits - 1)) & 1) {
                    negative = true;
                    magnitude = (1ull << bits) - raw;
                } else if (is_signed && bits == 64 && (raw >> 63)) {
                    negative = true;
                    magnitude = ~raw + 1;
                }
                const auto text = format_integer(magnitude, negative, is_signed, c.type, c,
                                                 width, precision);
                out.insert(out.end(), text.begin(), text.end());
                break;
            }
            case u'p': {
                u64 v = 0;
                if (!args.next_ptr(v)) return false;
                char buf[24];
                if (rd.is64) {
                    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(v));
                } else {
                    std::snprintf(buf, sizeof buf, "%08X", static_cast<unsigned>(v));
                }
                std::vector<u16> body;
                if (c.alt) append_ascii(body, "0X");
                append_ascii(body, buf);
                pad_and_append(out, body, c, width, u' ');
                break;
            }
            case u'c': case u'C': {
                u32 v = 0;
                if (!args.next_int32(v)) return false;
                std::vector<u16> body;
                if (wide_text) {
                    body.push_back(static_cast<u16>(v));
                } else {
                    const u8 byte = static_cast<u8>(v);
                    if (byte == 0) {
                        body.push_back(0);
                    } else {
                        body = widen(codepage::kCpAcp, std::string(1, static_cast<char>(byte)));
                    }
                }
                pad_and_append(out, body, c, width, c.zero ? u'0' : u' ');
                break;
            }
            case u's': case u'S': case u'Z': {
                u64 p = 0;
                if (!args.next_ptr(p)) return false;
                std::vector<u16> body;
                const usize limit = precision >= 0 ? static_cast<usize>(precision)
                                                   : static_cast<usize>(-1);
                GuestAddr text_addr = static_cast<GuestAddr>(p);
                usize text_len = limit;
                bool is_null = p == 0;
                if (c.type == u'Z' && !is_null) {
                    // ANSI_STRING / UNICODE_STRING: Length (bytes), MaximumLength,
                    // Buffer at the pointer-aligned offset.
                    u16 length = 0;
                    u64 buffer = 0;
                    if (!rd.u16_at(text_addr, length)) return false;
                    if (!rd.slot_at(text_addr + (rd.is64 ? 8 : 4), buffer)) return false;
                    is_null = buffer == 0;
                    text_addr = static_cast<GuestAddr>(buffer);
                    const bool z_wide = c.wide_prefix;
                    text_len = std::min<usize>(limit, z_wide ? length / 2u : length);
                    if (!is_null) {
                        if (z_wide) {
                            for (usize k = 0; k < text_len; ++k) {
                                u16 ch = 0;
                                if (!rd.u16_at(text_addr + k * 2, ch)) return false;
                                body.push_back(ch);
                            }
                        } else {
                            std::string bytes;
                            for (usize k = 0; k < text_len; ++k) {
                                u8 ch = 0;
                                if (!rd.u8_at(text_addr + k, ch)) return false;
                                bytes.push_back(static_cast<char>(ch));
                            }
                            body = widen(codepage::kCpAcp, bytes);
                        }
                    }
                } else if (!is_null) {
                    if (wide_text) {
                        if (!rd.wide_string(text_addr, limit, body)) return false;
                    } else {
                        std::string bytes;
                        if (!rd.narrow_string(text_addr, limit, bytes)) return false;
                        body = widen(codepage::kCpAcp, bytes);
                    }
                }
                if (is_null) {
                    append_ascii(body, "(null)");
                    if (body.size() > limit) body.resize(limit);
                }
                pad_and_append(out, body, c, width, c.zero ? u'0' : u' ');
                break;
            }
            default:
                // n, the floating-point conversions and anything unknown print
                // their own character and consume nothing (pf_e, pf_n, pf_q).
                out.push_back(c.type);
                break;
        }
    }
    return true;
}

// -----------------------------------------------------------------------------
// Insert arguments: va_list walking and ARGUMENT_ARRAY.
// -----------------------------------------------------------------------------

class Arguments {
public:
    Arguments(Reader& rd, GuestAddr arguments, bool array)
        : rd_(rd), arguments_(arguments), array_(array) {}

    [[nodiscard]] bool present() const { return arguments_ != 0; }

    /// Formats insert @p n with printf @p fmt ("%" + spec).
    bool format_insert(PrintfContext& ctx, u32 n, const std::vector<u16>& fmt,
                       std::vector<u16>& out) {
        if (!started_) {
            started_ = true;
            if (array_) {
                cursor_ = arguments_;
            } else {
                u64 va = 0;
                if (!rd_.slot_at(arguments_, va)) return false;
                cursor_ = static_cast<GuestAddr>(va);
            }
        }
        const Conversion conv = parse_conversion(fmt, 1);
        const u32 stars = (conv.width_star ? 1u : 0u) + (conv.precision_star ? 1u : 0u);
        // A literal I64 makes an x86 va_list hold 8-byte values; I64 and ll
        // make printf take an 8-byte first value (va_* and as_* cases).
        const bool fetch64 = !rd_.is64 && conv.size == Size::I64;
        const bool push64 = conv.size == Size::I64 || conv.size == Size::LongLong;
        const u32 slot = rd_.slot();

        u64 first = 0;
        bool frontier = false;
        if (array_) {
            if (!rd_.slot_at(arguments_ + static_cast<GuestAddr>(n - 1) * slot, first)) {
                return false;
            }
            if (n > max_) {
                cursor_ = arguments_ + static_cast<GuestAddr>(n) * slot;
                max_ = n;
                frontier = true;
            }
        } else if (n > max_) {
            const u32 size = fetch64 ? 8u : slot;
            for (u32 k = max_ + 1; k <= n; ++k) {
                u64 v = 0;
                if (!(size == 8 ? rd_.u64_at(cursor_, v) : rd_.slot_at(cursor_, v))) {
                    return false;
                }
                cache_[k] = v;
                cursor_ += size;
            }
            first = cache_[n];
            max_ = n;
            frontier = true;
        } else {
            first = cache_[n];
        }

        std::vector<u64> head;
        if (rd_.is64) {
            head.push_back(first);
        } else if (push64) {
            head.push_back(first & 0xFFFFFFFFull);
            head.push_back(first >> 32);
        } else {
            head.push_back(first & 0xFFFFFFFFull);
        }
        ArgStream stream(rd_, std::move(head), cursor_);
        if (!run_printf(ctx, fmt, stream, out)) return false;

        if (stars >= 1) cursor_ += slot;
        if (stars >= 2 && frontier && n < kMaxInsert) {
            if (!array_) {
                u64 v = 0;
                if (!rd_.slot_at(cursor_, v)) return false;
                cache_[n + 1] = v;
            }
            cursor_ += slot;
            max_ = n + 1;
        }
        return true;
    }

private:
    Reader& rd_;
    GuestAddr arguments_;
    bool array_;
    bool started_ = false;
    GuestAddr cursor_ = 0;
    u32 max_ = 0;
    u64 cache_[kMaxInsert + 2] = {};
};

// -----------------------------------------------------------------------------
// The message scan
// -----------------------------------------------------------------------------

struct ScanResult {
    Status status = Status::Ok;
    std::vector<u16> text;
};

ScanResult scan(Reader& rd, const std::vector<u16>& src, u32 flags, GuestAddr arguments,
                bool wide_entry) {
    ScanResult result;
    Output out(flags & kMaxWidthMask);
    const bool ignore = (flags & kIgnoreInserts) != 0;
    Arguments args(rd, arguments, (flags & kArgumentArray) != 0);
    PrintfContext ctx{rd, wide_entry};
    const usize n = src.size();

    auto finish = [&](Status s) {
        result.status = s;
        result.text = out.chars();
        return result;
    };

    for (usize i = 0; i < n;) {
        const u16 c = src[i];
        if (c == u'%') {
            if (i + 1 >= n) return finish(Status::InvalidParameter);
            const u16 d = src[i + 1];
            if (d >= u'1' && d <= u'9') {
                usize j = i + 1;
                u32 number = 0;
                for (; j < n && src[j] >= u'0' && src[j] <= u'9'; ++j) {
                    number = std::min<u32>(number * 10 + (src[j] - u'0'), 100000);
                }
                if (number > kMaxInsertNumber) return finish(Status::InvalidParameter);
                std::vector<u16> fmt{u'%'};
                usize next = j;
                if (j < n && src[j] == u'!') {
                    usize k = j + 1;
                    while (k < n && src[k] != u'!') ++k;
                    if (k >= n) return finish(Status::InvalidParameter);
                    fmt.insert(fmt.end(), src.begin() + static_cast<std::ptrdiff_t>(j + 1),
                               src.begin() + static_cast<std::ptrdiff_t>(k));
                    next = k + 1;
                } else {
                    fmt.push_back(u's');
                }
                if (ignore) {
                    // The insert is copied, minus a "!s!" that only restates
                    // the default (ign_spec_s; ign_spec_S keeps its spec).
                    const bool default_spec = fmt.size() == 2 && fmt[1] == u's' && next != j;
                    out.put_insert(std::vector<u16>(
                        src.begin() + static_cast<std::ptrdiff_t>(i),
                        src.begin() + static_cast<std::ptrdiff_t>(default_spec ? j : next)));
                } else {
                    if (number > kMaxInsert || !args.present()) {
                        return finish(Status::InvalidParameter);
                    }
                    std::vector<u16> units;
                    if (!args.format_insert(ctx, number, fmt, units)) {
                        return finish(Status::Fault);
                    }
                    out.put_insert(units);
                }
                i = next;
                continue;
            }
            switch (d) {
                case u'0':
                    return finish(Status::Ok);
                case u'n':
                    out.hard_break();
                    break;
                case u'r':
                    out.carriage_return();
                    break;
                case u't':
                    out.tab();
                    break;
                case u'b':
                    out.put(u' ', Kind::Breakable);
                    break;
                default:
                    if (ignore) out.put(u'%', Kind::Regular);
                    out.put(d, Kind::Regular);
                    break;
            }
            i += 2;
            continue;
        }
        if (c == kCr || c == kLf) {
            const u16 pair = c == kCr ? kLf : kCr;
            i += (i + 1 < n && src[i + 1] == pair) ? 2 : 1;
            out.source_break();
            continue;
        }
        out.put(c, c == u' ' ? Kind::Breakable : Kind::Regular);
        ++i;
    }
    return finish(Status::Ok);
}

// -----------------------------------------------------------------------------
// Message sources
// -----------------------------------------------------------------------------

u16 resolve_default_language(const Environment& env, u32 lang) {
    // MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT / SUBLANG_SYS_DEFAULT).
    if (lang == 0x0400) return env.user_default_language;
    if (lang == 0x0800) return env.system_default_language;
    return static_cast<u16>(lang);
}

/// Finds @p id in a MESSAGE_RESOURCE_DATA blob (winnt.h).
u32 message_from_table(const std::vector<u8>& data, u32 id, std::vector<u16>& text) {
    auto rd32 = [&data](usize at, u32& v) {
        if (at + 4 > data.size()) return false;
        v = static_cast<u32>(data[at]) | (static_cast<u32>(data[at + 1]) << 8) |
            (static_cast<u32>(data[at + 2]) << 16) | (static_cast<u32>(data[at + 3]) << 24);
        return true;
    };
    auto rd16 = [&data](usize at, u16& v) {
        if (at + 2 > data.size()) return false;
        v = static_cast<u16>(data[at] | (data[at + 1] << 8));
        return true;
    };
    u32 blocks = 0;
    if (!rd32(0, blocks)) return error_codes::ERROR_MR_MID_NOT_FOUND;
    for (u32 b = 0; b < blocks; ++b) {
        u32 low = 0, high = 0, offset = 0;
        if (!rd32(4 + b * 12, low) || !rd32(8 + b * 12, high) || !rd32(12 + b * 12, offset)) {
            break;
        }
        if (id < low || id > high) continue;
        usize at = offset;
        for (u32 k = low; k < id; ++k) {
            u16 length = 0;
            if (!rd16(at, length) || length == 0) return error_codes::ERROR_MR_MID_NOT_FOUND;
            at += length;
        }
        u16 length = 0, entry_flags = 0;
        if (!rd16(at, length) || !rd16(at + 2, entry_flags) || length < 4 ||
            at + length > data.size()) {
            return error_codes::ERROR_MR_MID_NOT_FOUND;
        }
        const usize begin = at + 4;
        const usize end = at + length;
        if (entry_flags & kMessageResourceUnicode) {
            for (usize p = begin; p + 1 < end; p += 2) {
                const u16 ch = static_cast<u16>(data[p] | (data[p + 1] << 8));
                if (ch == 0) break;
                text.push_back(ch);
            }
        } else {
            std::string bytes;
            for (usize p = begin; p < end && data[p] != 0; ++p) {
                bytes.push_back(static_cast<char>(data[p]));
            }
            text = widen(entry_flags & kMessageResourceUtf8 ? codepage::kCpUtf8
                                                            : codepage::kCpAcp,
                         bytes);
        }
        return 0;
    }
    return error_codes::ERROR_MR_MID_NOT_FOUND;
}

u32 message_from_module(const Environment& env, GuestAddr hmodule, u32 id, u32 lang,
                        std::vector<u16>& text) {
    std::vector<u16> languages;
    if (!env.message_table_languages || !env.message_table_languages(hmodule, languages)) {
        // No RT_MESSAGETABLE: measured ERROR_RESOURCE_DATA_NOT_FOUND (nomsg_*).
        return error_codes::ERROR_RESOURCE_DATA_NOT_FOUND;
    }
    auto has = [&languages](u16 l) {
        return std::find(languages.begin(), languages.end(), l) != languages.end();
    };
    u16 chosen = 0;
    if (lang > 0xFFFF) return error_codes::ERROR_RESOURCE_LANG_NOT_FOUND;  // mod_badlang
    if (lang != 0) {
        chosen = resolve_default_language(env, lang);
        if (!has(chosen)) return error_codes::ERROR_RESOURCE_LANG_NOT_FOUND;
    } else {
        // Learn: neutral, thread, user default, system default, US English,
        // then any language present.
        const u16 order[] = {0x0000, env.thread_ui_language, env.user_default_language,
                             env.system_default_language, 0x0409};
        bool found = false;
        for (u16 l : order) {
            if (has(l)) {
                chosen = l;
                found = true;
                break;
            }
        }
        if (!found) chosen = languages.front();
    }
    std::vector<u8> data;
    if (!env.message_table_data(hmodule, chosen, data)) {
        return error_codes::ERROR_RESOURCE_DATA_NOT_FOUND;
    }
    return message_from_table(data, id, text);
}

u32 message_from_system(const Environment& env, u32 id, u32 lang, std::vector<u16>& text) {
    if (lang > 0xFFFF) return error_codes::ERROR_INVALID_PARAMETER;  // sys_2_badlang
    if (lang != 0 && resolve_default_language(env, lang) != kSystemTableLanguage) {
        return kErrorMuiFileNotFound;  // sys_2_407 / sys_2_411 / sys_2_009
    }
    const char* msg = env.system_message ? env.system_message(id) : nullptr;
    if (msg == nullptr && (id & 0xFFFF0000u) == 0x80070000u) {
        // HRESULT_FROM_WIN32 resolves to the Win32 code's text (sys_hresult_2).
        msg = env.system_message(id & 0xFFFFu);
    }
    if (msg == nullptr) return error_codes::ERROR_MR_MID_NOT_FOUND;
    text = widen(codepage::kCpUtf8, msg);
    // Message-table text ends with the CRLF its .mc line break produced.
    if (text.size() < 2 || text[text.size() - 2] != kCr || text.back() != kLf) {
        text.push_back(kCr);
        text.push_back(kLf);
    }
    return 0;
}

// -----------------------------------------------------------------------------
// Entry
// -----------------------------------------------------------------------------

struct Call {
    u32 flags;
    GuestAddr source;
    u32 id;
    u32 lang;
    GuestAddr buffer;
    u32 size;
    GuestAddr arguments;
};

void finish(Marshaler& m, u32 ret, u32 error) {
    if (error != 0) set_last_error(error);
    m.set_return(ret);
}

bool write_pointer(Marshaler& m, GuestAddr at, u64 value) {
    if (m.cpu_is_64bit()) return m.memory().write_u64(at, value).is_ok();
    return m.memory().write_u32(at, static_cast<u32>(value)).is_ok();
}

bool write_units(Marshaler& m, GuestAddr at, const u16* units, usize count, bool nul) {
    std::vector<Byte> bytes((count + (nul ? 1 : 0)) * 2, 0);
    for (usize i = 0; i < count; ++i) {
        bytes[i * 2] = static_cast<Byte>(units[i] & 0xFF);
        bytes[i * 2 + 1] = static_cast<Byte>(units[i] >> 8);
    }
    if (bytes.empty()) return true;
    return m.memory().write_bytes(at, bytes.data(), bytes.size()).is_ok();
}

bool write_bytes(Marshaler& m, GuestAddr at, const char* data, usize count, bool nul) {
    std::vector<Byte> bytes(data, data + count);
    if (nul) bytes.push_back(0);
    if (bytes.empty()) return true;
    return m.memory().write_bytes(at, bytes.data(), bytes.size()).is_ok();
}

u64 alloc_capacity(u32 size, usize source_len) {
    const u64 need = std::max<u64>(static_cast<u64>(size) + 1, 4 * (static_cast<u64>(source_len) + 1));
    return (need + kAllocUnit - 1) / kAllocUnit * kAllocUnit;
}

} // namespace

void format_message(Marshaler& m, const Environment& env, bool wide) {
    const Call call{static_cast<u32>(m.arg(0)), m.arg_ptr(1), static_cast<u32>(m.arg(2)),
                    static_cast<u32>(m.arg(3)), m.arg_ptr(4), static_cast<u32>(m.arg(5)),
                    m.arg_ptr(6)};
    const bool alloc = (call.flags & kAllocateBuffer) != 0;
    VITREO_TRACE("[k32] FormatMessage%c(flags=0x%x, src=0x%llx, id=0x%x, lang=0x%x, "
                 "buf=0x%llx, n=%u, args=0x%llx)\n",
                 wide ? 'W' : 'A', call.flags, static_cast<unsigned long long>(call.source),
                 call.id, call.lang, static_cast<unsigned long long>(call.buffer), call.size,
                 static_cast<unsigned long long>(call.arguments));

    if (call.buffer == 0) {
        finish(m, 0, error_codes::ERROR_INVALID_PARAMETER);  // null_buf_*
        return;
    }

    Reader rd{m, m.cpu_is_64bit()};
    auto fault = [&]() {
        // Windows touches the bad address itself (null_src_*, bad_str_arg_w).
        if (!m.raise_access_violation(false, rd.fault_addr)) {
            set_last_error(error_codes::ERROR_NOACCESS);
        }
        m.set_return(0);
    };

    // FormatMessageA converts a FROM_STRING source before anything else; a
    // source past the conversion limit leaves *lpBuffer untouched.
    std::vector<u16> source;
    u32 source_error = 0;
    const bool from_string = (call.flags & kFromString) != 0;
    if (from_string) {
        if (wide) {
            if (!rd.wide_string(call.source, static_cast<usize>(-1), source)) return fault();
        } else {
            std::string bytes;
            if (!rd.narrow_string(call.source, static_cast<usize>(-1), bytes)) return fault();
            source = widen(codepage::kCpAcp, bytes);
            if (source.size() > kAnsiConversionLimit) {
                finish(m, 0, error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
        }
    }

    if (alloc && !write_pointer(m, call.buffer, 0)) {
        rd.fail(call.buffer);
        return fault();
    }

    if (!from_string) {
        const bool from_module = (call.flags & kFromHmodule) != 0;
        const bool from_system = (call.flags & kFromSystem) != 0;
        if (!from_module && !from_system) {
            source_error = error_codes::ERROR_INVALID_PARAMETER;  // flags_none
        } else if (from_module) {
            source_error = message_from_module(env, call.source, call.id, call.lang, source);
            // Only a message missing from the chosen table falls back to the
            // system (modsys_2_407); a missing table or language does not
            // (nomsg_sys_2, modsys_2_411).
            if (from_system && source_error == error_codes::ERROR_MR_MID_NOT_FOUND) {
                source.clear();
                source_error = message_from_system(env, call.id, call.lang, source);
            }
        } else {
            source_error = message_from_system(env, call.id, call.lang, source);
        }
    }
    if (source_error != 0) {
        finish(m, 0, source_error);
        return;
    }

    ScanResult r = scan(rd, source, call.flags, call.arguments, wide);
    if (r.status == Status::Fault) return fault();
    const std::vector<u16>& text = r.text;
    const usize len = text.size();

    if (wide) {
        if (alloc) {
            if (r.status != Status::Ok) {
                finish(m, 0, error_codes::ERROR_INVALID_PARAMETER);
                return;
            }
            if (len + 1 > alloc_capacity(call.size, source.size())) {
                finish(m, 0, error_codes::ERROR_MORE_DATA);
                return;
            }
            if (len == 0) {
                finish(m, 0, kErrorNoWorkDone);
                return;
            }
            const u64 chars = std::max<u64>(len, call.size) + 1;
            const GuestAddr block = env.local_alloc ? env.local_alloc(chars * 2) : 0;
            if (block == 0) {
                finish(m, 0, error_codes::ERROR_NOT_ENOUGH_MEMORY);
                return;
            }
            if (!write_units(m, block, text.data(), len, true) ||
                !write_pointer(m, call.buffer, block)) {
                rd.fail(block);
                return fault();
            }
            finish(m, static_cast<u32>(len), 0);
            return;
        }
        // Fixed buffer: a too-small one receives what fits and a NUL; a
        // malformed escape leaves the text before it, unterminated.
        if (len + 1 > call.size) {
            if (call.size > 0 &&
                !write_units(m, call.buffer, text.data(), call.size - 1, true)) {
                rd.fail(call.buffer);
                return fault();
            }
            finish(m, 0, error_codes::ERROR_INSUFFICIENT_BUFFER);
            return;
        }
        if (r.status != Status::Ok) {
            if (!write_units(m, call.buffer, text.data(), len, false)) {
                rd.fail(call.buffer);
                return fault();
            }
            finish(m, 0, error_codes::ERROR_INVALID_PARAMETER);
            return;
        }
        if (!write_units(m, call.buffer, text.data(), len, true)) {
            rd.fail(call.buffer);
            return fault();
        }
        if (len == 0) {
            finish(m, 0, kErrorNoWorkDone);
            return;
        }
        finish(m, static_cast<u32>(len), 0);
        return;
    }

    // FormatMessageA: the UTF-16 result is converted to CP_ACP; on failure
    // nothing is stored.
    if (r.status != Status::Ok) {
        finish(m, 0, error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    if (alloc) {
        if (len + 1 > alloc_capacity(call.size, source.size())) {
            finish(m, 0, error_codes::ERROR_MORE_DATA);
            return;
        }
        if (len == 0) {
            finish(m, 0, kErrorNoWorkDone);
            return;
        }
        if (len > kAnsiConversionLimit) {
            finish(m, 0, error_codes::ERROR_INVALID_PARAMETER);
            return;
        }
        const std::string ansi = narrow(text);
        // nSize is not a minimum for the ANSI entry (alloc hello_n100).
        const GuestAddr block = env.local_alloc ? env.local_alloc(ansi.size() + 1) : 0;
        if (block == 0) {
            finish(m, 0, error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return;
        }
        if (!write_bytes(m, block, ansi.data(), ansi.size(), true) ||
            !write_pointer(m, call.buffer, block)) {
            rd.fail(block);
            return fault();
        }
        finish(m, static_cast<u32>(ansi.size()), 0);
        return;
    }
    if (len + 1 > call.size) {
        finish(m, 0, error_codes::ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    if (len == 0) {
        finish(m, 0, kErrorNoWorkDone);
        return;
    }
    if (len > kAnsiConversionLimit) {
        finish(m, 0, error_codes::ERROR_INVALID_PARAMETER);
        return;
    }
    const std::string ansi = narrow(text);
    // The conversion's capacity is nSize as a 16-bit count (nsize_10003:
    // "he" and a NUL, ERROR_INSUFFICIENT_BUFFER).
    const usize capacity = call.size & 0xFFFFu;
    if (ansi.size() + 1 > capacity) {
        if (capacity > 0 && !write_bytes(m, call.buffer, ansi.data(), capacity - 1, true)) {
            rd.fail(call.buffer);
            return fault();
        }
        finish(m, 0, error_codes::ERROR_INSUFFICIENT_BUFFER);
        return;
    }
    if (!write_bytes(m, call.buffer, ansi.data(), ansi.size(), true)) {
        rd.fail(call.buffer);
        return fault();
    }
    finish(m, static_cast<u32>(ansi.size()), 0);
}

DllExports make_format_message_exports(Kernel32ExtManager& resources,
                                       GlobalLocalManager& local_heap,
                                       LocaleStringManager& locale) {
    auto run = [&resources, &local_heap, &locale](Marshaler& m, bool wide) {
        Environment env;
        env.message_table_languages = [&resources](GuestAddr h, std::vector<u16>& out) {
            return resources.message_table_languages(h, out);
        };
        env.message_table_data = [&resources](GuestAddr h, u16 lang, std::vector<u8>& out) {
            return resources.message_table_data(h, lang, out);
        };
        env.system_message = [](u32 id) { return lookup_error_message(id); };
        const bool is64 = m.cpu_is_64bit();
        env.local_alloc = [&local_heap, is64](u64 bytes) -> GuestAddr {
            local_heap.set_guest_64bit(is64);
            return static_cast<GuestAddr>(local_heap.global_alloc(0 /*LMEM_FIXED*/, bytes, true));
        };
        env.thread_ui_language = static_cast<u16>(thread_ui_language(locale));
        env.user_default_language = static_cast<u16>(locale.get_user_default_lang_id());
        env.system_default_language = static_cast<u16>(locale.get_system_default_lang_id());
        format_message(m, env, wide);
    };
    DllExports exports;
    // DWORD FormatMessageW/A(DWORD, LPCVOID, DWORD, DWORD, LPTSTR, DWORD,
    //                        va_list*): 7 arguments, stdcall_bytes = 28.
    exports.entries.push_back(ExportEntry{
        "FormatMessageW", 0, [run](Marshaler& m) { run(m, true); }, 28});
    exports.entries.push_back(ExportEntry{
        "FormatMessageA", 0, [run](Marshaler& m) { run(m, false); }, 28});
    return exports;
}

} // namespace vitreo::winapi::format_message

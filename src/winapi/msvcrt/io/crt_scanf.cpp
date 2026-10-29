/// @file crt_scanf.cpp
/// @brief The scanf input processor behind every CRT formatted-input export:
///        the UCRT's __stdio_common_vsscanf / vswscanf / vfscanf and the
///        msvcrt.dll sscanf, swscanf, _snscanf, _snwscanf, fscanf and scanf
///        families with their _s and _l forms.
///
/// Contract, UCRT: the Windows SDK's UCRT source
/// (ucrt/inc/corecrt_internal_stdio_input.h -- format_string_parser,
/// input_processor, the string and stream input adapters -- ucrt/stdio/input.cpp
/// and the parse_integer / parse_floating_point / input_adapter_character_source
/// parts of corecrt_internal_strtox.h), ported rule for rule and verified by
/// tools/oracle_probe/crt_scanf_probe.c (x86 and x64 .expected).
///
/// Contract, msvcrt.dll: measured by the same probe. The system msvcrt.dll runs
/// the pre-UCRT engine; on everything the probe exercises it agrees with the
/// UCRT processor under _CRT_INTERNAL_SCANF_LEGACY_MSVCRT_COMPATIBILITY except:
///  - size modifiers follow the old counters: `h` narrows (short, and a double
///    for floats), `hh`/`hhh` land back on int, `L` is `l`, `ll` only widens
///    integers (a %llf is a float, a %ll[ is narrow), `F` and `N` are ignored,
///    `T` is not a modifier, and `*`, width digits and modifiers may come in
///    any order; `z`/`t` are pointer-sized and `j` is 64-bit;
///  - `%a`, `%A` and `%F` are not conversions;
///  - floats are collected as [sign]digits[.digits][e[sign]digits] with no
///    back-up (so "1e" is 1.0 and "0x" is 0.0 followed by "x"), decimal only;
///  - integer overflow wraps instead of clamping and never sets ERANGE, and
///    only ASCII digits count;
///  - a format error in a secure (_s) call does not reach the handler;
///  - swscanf/_snwscanf never return EOF: input that runs out before the first
///    conversion yields 0;
///  - "ran out of input" means the last character read was the end, so a
///    secure buffer overflowing on the final character returns 0, not EOF.
/// An unterminated scanset is a format error here; the legacy engine reads
/// past the format's terminator looking for ']' (undefined behaviour). A NULL
/// input or format dereferences on Windows; here it fails with EINVAL and EOF.
///
/// Wide-character streams: __stdio_common_vfwscanf reads what fgetwc reads.
/// On a text stream in the locale's multibyte code page that is one
/// multibyte character per wchar_t; Vitreo's CRT locale is "C", where each
/// byte is the unit of the same value (crt_ucrt_s6_probe "w" rows: bytes
/// 80 e9 read as U+0080 U+00E9), with the stream's own text translation
/// (CR-LF, Ctrl+Z) and one-byte pushback. Binary streams (UTF-16 units read
/// as byte pairs, pushed back as two bytes) and ccs=UTF-8 / UTF-16LE /
/// _O_U8TEXT / _O_U16TEXT streams are not implemented: the call logs
/// UNIMPLEMENTED once and returns EOF. msvcrt.dll's fwscanf family and the
/// console forms (_cscanf) are not registered.

#include "winapi/msvcrt/io/crt_stdio.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/io/crt_printf.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/msvcrt/text/crt_float_parse.hpp"
#include "winapi/msvcrt/text/crt_string_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <limits>
#include <vector>

namespace vitreo::winapi {

namespace {

using crt_printf::ArgCursor;

// corecrt_stdio_config.h (_CRT_INTERNAL_SCANF_*).
constexpr u64 kSecureCrt = 1ULL << 0;
constexpr u64 kLegacyWideSpecifiers = 1ULL << 1;
constexpr u64 kLegacyMsvcrtCompatibility = 1ULL << 2;

constexpr i32 kEof = -1;
constexpr u32 kENOMEM = 12;
constexpr u32 kEINVAL = 22;
constexpr u32 kERANGE = 34;
constexpr u32 kEILSEQ = 42;
constexpr u64 kUnbounded = std::numeric_limits<u64>::max();  // _CRT_UNBOUNDED_BUFFER_SIZE
constexpr usize kMbLenMax = 5;                                // MB_LEN_MAX

enum class Flavor : u8 { Ucrt, Msvcrt };

// ---- Input adapters -------------------------------------------------------------

/// get() returns a character (bytes 0..255, UTF-16 units 0..65535) or kEof.
/// The last value get() returned is kept for the legacy engine's end-of-input
/// rule.
class Input {
public:
    virtual ~Input() = default;
    i32 get() {
        m_last = do_get();
        m_any = true;
        return m_last;
    }
    void unget(i32 c) { do_unget(c); }
    [[nodiscard]] virtual u64 characters_read() const = 0;
    [[nodiscard]] bool any_read() const { return m_any; }
    [[nodiscard]] i32 last_read() const { return m_last; }

protected:
    virtual i32 do_get() = 0;
    virtual void do_unget(i32 c) = 0;

private:
    i32 m_last{kEof};
    bool m_any{false};
};

/// string_input_adapter over [first, first + length) in guest memory, read in
/// windows so a long buffer is not copied up front.
class StringInput final : public Input {
public:
    StringInput(GuestMemory& memory, GuestAddr first, usize length, bool wide)
        : m_memory(memory), m_first(first), m_length(length), m_wide(wide) {}

    [[nodiscard]] u64 characters_read() const override { return m_it; }

protected:
    i32 do_get() override {
        if (m_it == m_length) return kEof;
        return static_cast<i32>(unit(m_it++));
    }
    void do_unget(i32 c) override {
        if (m_it == 0) return;
        if (m_it == m_length && c == kEof) return;
        --m_it;
    }

private:
    u32 unit(usize index) {
        if (index < m_window_first || index >= m_window_first + m_window.size()) {
            constexpr usize kWindow = 512;
            m_window_first = index;
            m_window.assign(std::min(kWindow, m_length - index), 0);
            for (usize i = 0; i < m_window.size(); ++i) {
                const GuestAddr at = m_first + (index + i) * (m_wide ? 2 : 1);
                m_window[i] = m_wide ? m_memory.read_u16(at).value_or(0)
                                     : m_memory.read_u8(at).value_or(0);
            }
        }
        return m_window[index - m_window_first];
    }

    GuestMemory& m_memory;
    GuestAddr m_first;
    usize m_length;
    bool m_wide;
    usize m_it{0};
    usize m_window_first{0};
    std::vector<u32> m_window;
};

/// stream_input_adapter (narrow): the stream's own getc / ungetc.
class StreamInput final : public Input {
public:
    StreamInput(CrtStdioManager& io, u32 stream) : m_io(io), m_stream(stream) {}
    [[nodiscard]] u64 characters_read() const override { return m_read; }

protected:
    i32 do_get() override {
        const i32 c = m_io.guest_fgetc(m_stream);
        if (c == EOF) return kEof;
        ++m_read;
        return c & 0xFF;
    }
    void do_unget(i32 c) override {
        if (c == kEof) return;
        --m_read;
        (void)m_io.guest_ungetc(c, m_stream);
    }

private:
    CrtStdioManager& m_io;
    u32 m_stream;
    u64 m_read{0};
};

/// input_adapter_character_source: a field-width-limited view of the input
/// for the number parsers. It reads 0 past the width or at end of input, and
/// can restore a saved state only when nothing was read since.
class FieldSource {
public:
    FieldSource(Input& input, u64 width) : m_input(input), m_max(width) {}

    u32 get() {
        ++m_count;
        if (m_max != 0 && m_count > m_max) return 0;
        const i32 c = m_input.get();
        return c == kEof ? 0 : static_cast<u32>(c);
    }
    void unget(u32 c) {
        --m_count;
        if (m_max != 0 && m_count > m_max) return;
        if (c == 0) return;
        m_input.unget(static_cast<i32>(c));
    }
    [[nodiscard]] u64 save_state() const { return m_count; }
    bool restore_state(u64 state) {
        if (state != m_count) {
            m_ok = false;
            return false;
        }
        return true;
    }
    /// The destructor's rule: consuming nothing is a failure too.
    [[nodiscard]] bool succeeded() const { return m_ok && m_count != 0; }

private:
    Input& m_input;
    u64 m_max;
    u64 m_count{0};
    bool m_ok{true};
};

// ---- Format string parser -------------------------------------------------------

enum class DirectiveKind : u8 { Error, End, Whitespace, Literal, Conversion };

enum class Mode : u8 {
    Character,
    String,
    SignedUnknown,
    SignedDecimal,
    UnsignedOctal,
    UnsignedDecimal,
    UnsignedHexadecimal,
    FloatingPoint,
    Scanset,
    ReportCharacterCount,
};

enum class Length : u8 { None, hh, h, l, ll, j, z, t, L, I32, I64, T };

/// is_length_valid (UCRT).
bool length_valid(Mode mode, Length length) {
    static constexpr u8 kTable[10][12] = {
        //            none hh h  l  ll j  z  t  L  I32 I64 T
        /* char  */ {1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1},
        /* str   */ {1, 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1},
        /* i     */ {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0},
        /* d     */ {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0},
        /* o     */ {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0},
        /* u     */ {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0},
        /* x     */ {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0},
        /* float */ {1, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0},
        /* [     */ {1, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1},
        /* n     */ {1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0},
    };
    return kTable[static_cast<usize>(mode)][static_cast<usize>(length)] != 0;
}

class FormatParser {
public:
    FormatParser(std::vector<u32> format, u64 options, Flavor flavor, bool wide,
                 bool guest64)
        : m_format(std::move(format)),
          m_options(options),
          m_flavor(flavor),
          m_wide(wide),
          m_ptr_size(guest64 ? 8 : 4),
          m_scanset((wide ? 65536 : 256) / 8, 0) {}

    bool advance() {
        if (m_error != 0) return false;
        reset();
        const u32 c = at(m_pos);
        if (c == 0) {
            m_kind = DirectiveKind::End;
            return false;
        }
        if (format_space(c)) {
            m_kind = DirectiveKind::Whitespace;
            while (format_space(at(m_pos))) ++m_pos;
            return true;
        }
        if (c != '%' || at(m_pos + 1) == '%') {
            // No lead bytes in the "C" locale.
            m_kind = DirectiveKind::Literal;
            m_literal = c;
            m_pos += c != '%' ? 1 : 2;
            return true;
        }
        m_kind = DirectiveKind::Conversion;
        ++m_pos;
        return m_flavor == Flavor::Ucrt ? conversion_ucrt() : conversion_legacy();
    }

    [[nodiscard]] u32 error() const { return m_error; }
    [[nodiscard]] DirectiveKind kind() const { return m_kind; }
    [[nodiscard]] u32 literal() const { return m_literal; }
    [[nodiscard]] bool suppress() const { return m_suppress; }
    [[nodiscard]] u64 width() const { return m_width; }
    [[nodiscard]] Mode mode() const { return m_mode; }
    [[nodiscard]] bool wide_buffer() const { return m_is_wide; }
    /// Destination size in bytes of an integer, %n or floating conversion.
    [[nodiscard]] usize size() const { return m_size; }
    [[nodiscard]] bool scanset_test(u32 c) const {
        if (c / 8 >= m_scanset.size()) return false;
        return (m_scanset[c / 8] & (1u << (c % 8))) != 0;
    }

private:
    [[nodiscard]] u32 at(usize i) const { return i < m_format.size() ? m_format[i] : 0; }

    /// traits::istspace: isspace ("C" locale) or iswspace.
    [[nodiscard]] bool format_space(u32 c) const { return crt_float::is_space(c, m_wide); }

    void reset() {
        m_kind = DirectiveKind::Error;
        m_literal = 0;
        m_suppress = false;
        m_width = 0;
        m_length = Length::None;
        m_is_wide = false;
        m_mode = Mode::Character;
        m_size = 0;
    }
    bool fail(u32 error) {
        reset();
        m_error = error;
        return false;
    }

    // -- UCRT format_string_parser --

    bool conversion_ucrt() {
        if (at(m_pos) == '*') {
            m_suppress = true;
            ++m_pos;
        }
        if (crt_float::parse_digit(at(m_pos), m_wide) <= 9) {
            // tcstoull(format, &end, 10): saturates on overflow.
            u64 width = 0;
            bool overflow = false;
            usize end = m_pos;
            for (;; ++end) {
                const unsigned d = crt_float::parse_digit(at(end), m_wide);
                if (d > 9) break;
                if (width > (kUnbounded - d) / 10) overflow = true;
                width = width * 10 + d;
            }
            if (overflow) width = kUnbounded;
            if (width == 0 || end == m_pos) return fail(kEINVAL);
            m_width = width;
            m_pos = end;
        }
        scan_length_ucrt();
        if (at(m_pos) == 'w') {
            ++m_pos;
            m_is_wide = true;
        } else if (should_default_to_wide(at(m_pos))) {
            m_is_wide = true;
        }
        if (!conversion_specifier_ucrt()) return false;
        if (!length_valid(m_mode, m_length)) return fail(kEINVAL);
        m_size = ucrt_size();
        return true;
    }

    void scan_length_ucrt() {
        const u32 c = at(m_pos);
        switch (c) {
        case 'h':
            if (at(m_pos + 1) == 'h') {
                m_pos += 2;
                m_length = Length::hh;
            } else {
                m_pos += 1;
                m_length = Length::h;
            }
            return;
        case 'I': {
            if (at(m_pos + 1) == '3' && at(m_pos + 2) == '2') {
                m_pos += 3;
                m_length = Length::I32;
                return;
            }
            if (at(m_pos + 1) == '6' && at(m_pos + 2) == '4') {
                m_pos += 3;
                m_length = Length::I64;
                return;
            }
            const u32 n = at(m_pos + 1);
            if (n == 'd' || n == 'i' || n == 'o' || n == 'u' || n == 'x' || n == 'X') {
                ++m_pos;
                m_length = m_ptr_size == 4 ? Length::I32 : Length::I64;
            }
            return;
        }
        case 'l':
            if (at(m_pos + 1) == 'l') {
                m_pos += 2;
                m_length = Length::ll;
            } else {
                m_pos += 1;
                m_length = Length::l;
            }
            return;
        case 'L': ++m_pos; m_length = Length::L; return;
        case 'j': ++m_pos; m_length = Length::j; return;
        case 't': ++m_pos; m_length = Length::t; return;
        case 'z': ++m_pos; m_length = Length::z; return;
        case 'T': ++m_pos; m_length = Length::T; return;
        default: return;
        }
    }

    [[nodiscard]] bool should_default_to_wide(u32 c) const {
        if (!m_wide) return c == 'C' || c == 'S';
        if (c == 'C' || c == 'S') return false;
        if (m_length == Length::T) return true;
        return (m_options & kLegacyWideSpecifiers) != 0;
    }

    void set_wide_for_c_s_or_scanset() {
        if (m_length == Length::h) m_is_wide = false;
        if (m_length == Length::l || m_length == Length::ll || m_length == Length::L) {
            m_is_wide = true;
        }
    }

    bool conversion_specifier_ucrt() {
        const u32 c = at(m_pos);
        switch (c) {
        case 'C':
        case 'c':
            if (m_width == 0) m_width = 1;
            set_wide_for_c_s_or_scanset();
            m_mode = Mode::Character;
            ++m_pos;
            return true;
        case 'S':
        case 's':
            set_wide_for_c_s_or_scanset();
            m_mode = Mode::String;
            ++m_pos;
            return true;
        case 'i':
        case 'I': m_mode = Mode::SignedUnknown; ++m_pos; return true;
        case 'd': m_mode = Mode::SignedDecimal; ++m_pos; return true;
        case 'u': m_mode = Mode::UnsignedDecimal; ++m_pos; return true;
        case 'X':
        case 'x': m_mode = Mode::UnsignedHexadecimal; ++m_pos; return true;
        case 'p':
            m_length = m_ptr_size == 4 ? Length::I32 : Length::I64;
            m_mode = Mode::UnsignedHexadecimal;
            ++m_pos;
            return true;
        case 'o': m_mode = Mode::UnsignedOctal; ++m_pos; return true;
        case 'A':
        case 'a':
        case 'E':
        case 'e':
        case 'F':
        case 'f':
        case 'G':
        case 'g': m_mode = Mode::FloatingPoint; ++m_pos; return true;
        case '[':
            set_wide_for_c_s_or_scanset();
            m_mode = Mode::Scanset;
            ++m_pos;
            return scan_scanset();
        case 'n': m_mode = Mode::ReportCharacterCount; ++m_pos; return true;
        default: return fail(kEINVAL);
        }
    }

    [[nodiscard]] usize ucrt_size() const {
        switch (m_mode) {
        case Mode::Character:
        case Mode::String:
        case Mode::Scanset: return m_is_wide ? 2 : 1;
        case Mode::FloatingPoint:
            // long double is double.
            return m_length == Length::None ? 4 : 8;
        default: break;
        }
        switch (m_length) {
        case Length::hh: return 1;
        case Length::h: return 2;
        case Length::ll:
        case Length::j:
        case Length::I64: return 8;
        case Length::z:
        case Length::t: return m_ptr_size;
        default: return 4;  // none, l (long is 32-bit), I32
        }
    }

    /// scan_scanset_range (shared by both engines; see the file comment).
    bool scan_scanset() {
        std::fill(m_scanset.begin(), m_scanset.end(), 0);
        const bool reject = at(m_pos) == '^';
        if (reject) ++m_pos;
        if (at(m_pos) == ']') {
            ++m_pos;
            set(']');
        }
        const usize first = m_pos;
        usize last_range_end = std::numeric_limits<usize>::max();
        for (; at(m_pos) != ']' && at(m_pos) != 0; ++m_pos) {
            const u32 c = at(m_pos);
            if (c != '-' || m_pos - 1 == last_range_end || m_pos == first ||
                at(m_pos + 1) == ']') {
                set(c);
            } else {
                u32 lower = at(m_pos - 1);
                u32 upper = at(m_pos + 1);
                last_range_end = m_pos + 1;
                if (lower > upper) std::swap(lower, upper);
                for (u32 r = lower; r <= upper; ++r) set(r);
            }
        }
        if (at(m_pos) == 0) return fail(kEINVAL);
        if (reject) {
            for (u8& b : m_scanset) b = static_cast<u8>(~b);
        }
        ++m_pos;
        return true;
    }

    void set(u32 c) {
        if (c / 8 < m_scanset.size()) m_scanset[c / 8] |= static_cast<u8>(1u << (c % 8));
    }

    // -- msvcrt.dll (measured; see the file comment) --

    bool conversion_legacy() {
        int longone = 1;
        int wideflag = 0;
        bool integer64 = false;
        bool widthset = false;
        u64 width = 0;
        for (bool done = false; !done;) {
            const u32 c = at(m_pos);
            if (c >= '0' && c <= '9') {
                widthset = true;
                width = width * 10 + (c - '0');
                ++m_pos;
                continue;
            }
            switch (c) {
            case 'F':
            case 'N': ++m_pos; break;
            case 'h':
                --longone;
                --wideflag;
                ++m_pos;
                break;
            case 'I': {
                const u32 n = at(m_pos + 1);
                if (n == '6' && at(m_pos + 2) == '4') {
                    integer64 = true;
                    m_pos += 3;
                } else if (n == '3' && at(m_pos + 2) == '2') {
                    m_pos += 3;
                } else if (n == 'd' || n == 'i' || n == 'o' || n == 'u' || n == 'x' ||
                           n == 'X') {
                    if (m_ptr_size == 8) integer64 = true;
                    ++m_pos;
                } else {
                    done = true;  // 'I' is then the (invalid) conversion
                }
                break;
            }
            case 'L':
                ++longone;
                ++m_pos;
                break;
            case 'l':
                if (at(m_pos + 1) == 'l') {
                    integer64 = true;
                    m_pos += 2;
                } else {
                    ++longone;
                    ++wideflag;
                    ++m_pos;
                }
                break;
            case 'w':
                ++wideflag;
                ++m_pos;
                break;
            case 'z':
            case 't':
                if (m_ptr_size == 8) integer64 = true;
                ++m_pos;
                break;
            case 'j':
                integer64 = true;
                ++m_pos;
                break;
            case '*':
                m_suppress = true;
                ++m_pos;
                break;
            default: done = true; break;
            }
        }
        if (widthset && width == 0) return fail(kEINVAL);
        m_width = width;
        const u32 conversion = at(m_pos);
        ++m_pos;
        const auto pick_wide = [&](bool flips) {
            if (wideflag > 0) return true;
            if (wideflag < 0) return false;
            return flips ? !m_wide : m_wide;
        };
        switch (conversion) {
        case 'c':
        case 'C':
            if (!widthset) m_width = 1;
            m_mode = Mode::Character;
            m_is_wide = pick_wide(conversion == 'C');
            m_size = m_is_wide ? 2 : 1;
            return true;
        case 's':
        case 'S':
            m_mode = Mode::String;
            m_is_wide = pick_wide(conversion == 'S');
            m_size = m_is_wide ? 2 : 1;
            return true;
        case '[':
            m_mode = Mode::Scanset;
            m_is_wide = pick_wide(false);
            m_size = m_is_wide ? 2 : 1;
            return scan_scanset();
        case 'd': m_mode = Mode::SignedDecimal; break;
        case 'i': m_mode = Mode::SignedUnknown; break;
        case 'u': m_mode = Mode::UnsignedDecimal; break;
        case 'o': m_mode = Mode::UnsignedOctal; break;
        case 'x':
        case 'X': m_mode = Mode::UnsignedHexadecimal; break;
        case 'p':
            m_mode = Mode::UnsignedHexadecimal;
            m_size = m_ptr_size;
            return true;
        case 'n': m_mode = Mode::ReportCharacterCount; break;
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G':
            m_mode = Mode::FloatingPoint;
            m_size = (longone - 1) != 0 ? 8 : 4;
            return true;
        default: return fail(kEINVAL);
        }
        m_size = integer64 ? 8 : (longone != 0 ? 4 : 2);
        return true;
    }

    std::vector<u32> m_format;
    u64 m_options;
    Flavor m_flavor;
    bool m_wide;
    usize m_ptr_size;
    usize m_pos{0};
    u32 m_error{0};

    DirectiveKind m_kind{DirectiveKind::Error};
    u32 m_literal{0};
    bool m_suppress{false};
    u64 m_width{0};
    Length m_length{Length::None};
    bool m_is_wide{false};
    Mode m_mode{Mode::Character};
    usize m_size{0};
    std::vector<u8> m_scanset;
};

// ---- Input processor -------------------------------------------------------------

struct Request {
    Flavor flavor{Flavor::Ucrt};
    u64 options{0};
    bool wide{false};  // wchar_t format and input
    /// msvcrt.dll's swscanf / _snwscanf: running out of input before the first
    /// conversion returns 0, never EOF (measured).
    bool never_eof{false};
};

class InputProcessor {
public:
    InputProcessor(Marshaler& m, CrtMemoryManager& mm, const Request& request,
                   std::vector<u32> format, Input& input, ArgCursor args)
        : m_m(m),
          m_mm(mm),
          m_request(request),
          m_parser(std::move(format), request.options, request.flavor, request.wide,
                   m.cpu_is_64bit()),
          m_input(input),
          m_args(args) {}

    int process() {
        while (m_parser.advance()) {
            if (!process_state()) break;
        }
        int result = static_cast<int>(m_assigned);
        if (m_assigned == 0 && m_parser.kind() != DirectiveKind::End) {
            bool at_end = false;
            if (!ucrt() && m_input.any_read()) {
                // The legacy engine asks whether the last character it read
                // was the end of input, not whether one more read would be:
                // a secure buffer that overflows on the final character
                // returns 0 (measured), where the UCRT returns EOF.
                at_end = m_input.last_read() == kEof;
            } else {
                const i32 c = m_input.get();
                at_end = c == kEof;
                m_input.unget(c);
            }
            if (at_end && !m_request.never_eof) result = kEof;
        }
        if (secure() && m_request.flavor == Flavor::Ucrt && m_parser.error() != 0) {
            invalid(m_parser.error());
        }
        return result;
    }

private:
    [[nodiscard]] bool secure() const { return (m_request.options & kSecureCrt) != 0; }
    [[nodiscard]] bool legacy_compat() const {
        return (m_request.options & kLegacyMsvcrtCompatibility) != 0;
    }
    [[nodiscard]] bool ucrt() const { return m_request.flavor == Flavor::Ucrt; }

    /// _VALIDATE_RETURN: errno, then the invalid-parameter handler (whose
    /// no-handler policy follows the calling DLL).
    void invalid(u32 error) {
        m_mm.set_errno(error);
        (void)m_mm.invalid_parameter();
    }

    bool process_state() {
        switch (m_parser.kind()) {
        case DirectiveKind::Whitespace: process_whitespace(); return true;
        case DirectiveKind::Literal: return process_literal();
        case DirectiveKind::Conversion: {
            const bool ok = process_conversion();
            if (ok && m_parser.mode() != Mode::ReportCharacterCount &&
                !m_parser.suppress()) {
                ++m_assigned;
            }
            return ok;
        }
        default: return false;
        }
    }

    void process_whitespace() {
        i32 c;
        do {
            c = m_input.get();
            if (c == kEof) break;
        } while (crt_float::is_space(static_cast<u32>(c), m_request.wide));
        m_input.unget(c);
    }

    bool process_literal() {
        const i32 c = m_input.get();
        if (c == kEof) return false;
        if (static_cast<u32>(c) != m_parser.literal()) {
            m_input.unget(c);
            return false;
        }
        return true;
    }

    bool process_conversion() {
        switch (m_parser.mode()) {
        case Mode::Character:
        case Mode::String:
        case Mode::Scanset: return process_string(m_parser.mode());
        case Mode::SignedUnknown: return process_integer(0, true);
        case Mode::SignedDecimal: return process_integer(10, true);
        case Mode::UnsignedOctal: return process_integer(8, false);
        case Mode::UnsignedDecimal: return process_integer(10, false);
        case Mode::UnsignedHexadecimal: return process_integer(16, false);
        case Mode::FloatingPoint: return process_float();
        case Mode::ReportCharacterCount:
            if (m_parser.suppress()) return true;
            return write_integer(m_input.characters_read());
        }
        return false;
    }

    // -- %c %s %[ --

    [[nodiscard]] bool allowed_in_string(Mode mode, i32 c) const {
        if (c == kEof) return false;
        switch (mode) {
        case Mode::Character: return true;
        case Mode::String: return !((c >= '\t' && c <= '\r') || c == ' ');
        case Mode::Scanset: return m_parser.scanset_test(static_cast<u32>(c));
        default: return false;
        }
    }

    void store_char(GuestAddr buffer, u64 index, u32 value, bool wide_buffer) {
        if (wide_buffer) {
            (void)m_m.memory().write_u16(buffer + index * 2, static_cast<u16>(value));
        } else {
            (void)m_m.memory().write_u8(buffer + index, static_cast<u8>(value));
        }
    }

    /// _RESET_STRING (release UCRT): only the first element.
    void reset_buffer(GuestAddr buffer, u64 count, bool wide_buffer) {
        if (count == kUnbounded) return;
        store_char(buffer, 0, 0, wide_buffer);
    }

    bool process_string(Mode mode) {
        if (mode == Mode::String) process_whitespace();
        const bool wide_buffer = m_parser.wide_buffer();

        GuestAddr buffer = 0;
        if (!m_parser.suppress()) {
            buffer = static_cast<GuestAddr>(m_args.next_pointer(m_m));
            if (buffer == 0) {
                invalid(kEINVAL);
                return false;
            }
        }
        const u64 count =
            (buffer != 0 && secure()) ? u64{m_args.next_int32(m_m)} : kUnbounded;
        if (count == 0) {
            if (legacy_compat()) {
                // The old implementation consumed a character and wrote a
                // terminator into the zero-sized buffer.
                (void)m_input.get();
                store_char(buffer, 0, 0, wide_buffer);
            }
            m_mm.set_errno(kENOMEM);
            return false;
        }

        const u64 width = m_parser.width();
        u64 at = 0;
        u64 remaining = count;
        if (mode != Mode::Character && remaining != kUnbounded) --remaining;

        u64 consumed = 0;
        for (; width == 0 || consumed != width; ++consumed) {
            const i32 c = m_input.get();
            if (!allowed_in_string(mode, c)) {
                m_input.unget(c);
                break;
            }
            if (m_parser.suppress()) continue;
            if (remaining == 0) {
                reset_buffer(buffer, count, wide_buffer);
                m_mm.set_errno(kENOMEM);
                return false;
            }
            if (!write_character(buffer, count, at, remaining, static_cast<u32>(c),
                                 wide_buffer)) {
                break;
            }
        }
        if (consumed == 0) return false;
        if (mode == Mode::Character && consumed != width && !legacy_compat()) return false;
        if (m_parser.suppress()) return true;
        if (mode != Mode::Character) store_char(buffer, at, 0, wide_buffer);
        return true;
    }

    /// The four write_character overloads. Narrow input into a wide buffer is
    /// widened by value (the UCRT converts it and then stores the byte anyway;
    /// in the "C" locale that is the same thing). Wide input into a narrow
    /// buffer goes through the "C" locale's wctomb_s: a character above 0xFF
    /// is EILSEQ, clears the space wctomb_s was given, and stores nothing.
    bool write_character(GuestAddr buffer, u64 count, u64& at, u64& remaining, u32 c,
                         bool wide_buffer) {
        if (wide_buffer || !m_request.wide || c <= 0xFF) {
            store_char(buffer, at, c, wide_buffer);
            ++at;
            --remaining;
            return true;
        }
        const u64 space = count == kUnbounded ? kMbLenMax : remaining;
        for (u64 i = 0; i < space; ++i) store_char(buffer, at + i, 0, false);
        m_mm.set_errno(kEILSEQ);
        return true;
    }

    // -- integers --

    /// parse_integer<uint64_t> (UCRT); the legacy engine wraps instead of
    /// clamping and only knows ASCII digits.
    u64 parse_integer(FieldSource& source, unsigned base, bool is_signed) {
        const bool unicode_digits = m_request.wide && ucrt();
        const auto digit = [unicode_digits](u32 c) {
            return crt_float::parse_digit(c, unicode_digits);
        };
        const auto initial = source.save_state();
        u32 c = source.get();
        while (crt_float::is_space(c, m_request.wide)) c = source.get();
        const bool negative = c == '-';
        if (c == '-' || c == '+') c = source.get();
        if (base == 0 || base == 16) {
            if (digit(c) != 0) {
                if (base == 0) base = 10;
            } else {
                const u32 next = source.get();
                if (next == 'x' || next == 'X') {
                    if (base == 0) base = 16;
                    c = source.get();
                } else {
                    if (base == 0) base = 8;
                    source.unget(next);
                }
            }
        }
        const u64 max_pre_multiply = std::numeric_limits<u64>::max() / base;
        u64 number = 0;
        bool read_digit = false;
        bool overflow = false;
        for (;;) {
            const unsigned d = digit(c);
            if (d >= base) break;
            read_digit = true;
            const u64 after_multiply = number * base;
            const u64 after_add = after_multiply + d;
            if (number > max_pre_multiply || after_add < after_multiply) overflow = true;
            number = after_add;
            c = source.get();
        }
        source.unget(c);
        if (!read_digit) {
            (void)source.restore_state(initial);
            return 0;
        }
        constexpr u64 kInt64Max = static_cast<u64>(std::numeric_limits<i64>::max());
        if (ucrt()) {
            const bool range = overflow ||
                               (is_signed && negative && number > kInt64Max + 1) ||
                               (is_signed && !negative && number > kInt64Max);
            if (range) {
                m_mm.set_errno(kERANGE);
                if (!is_signed) return std::numeric_limits<u64>::max();
                return negative ? kInt64Max + 1 : kInt64Max;
            }
        }
        return negative ? (0 - number) : number;
    }

    bool process_integer(unsigned base, bool is_signed) {
        process_whitespace();
        FieldSource source(m_input, m_parser.width());
        const u64 number = parse_integer(source, base, is_signed);
        if (!source.succeeded()) return false;
        if (m_parser.suppress()) return true;
        return write_integer(number);
    }

    bool write_integer(u64 value) {
        const GuestAddr p = static_cast<GuestAddr>(m_args.next_pointer(m_m));
        if (p == 0) {
            invalid(kEINVAL);
            return false;
        }
        auto& mem = m_m.memory();
        switch (m_parser.size()) {
        case 1: (void)mem.write_u8(p, static_cast<u8>(value)); return true;
        case 2: (void)mem.write_u16(p, static_cast<u16>(value)); return true;
        case 4: (void)mem.write_u32(p, static_cast<u32>(value)); return true;
        case 8: (void)mem.write_u64(p, value); return true;
        default: return false;
        }
    }

    // -- floating point --

    [[nodiscard]] unsigned rounding() const {
        return m_m.has_cpu() ? ((m_m.cpu()->sse.mxcsr >> 13) & 3) : 0;
    }

    /// The legacy engine: [sign]digits[.digits][e[sign]digits], each character
    /// taken only while the width lasts, no back-up; the collected text is
    /// then converted as strtod would.
    bool legacy_float(u64 width, crt_float::Result& result, crt_float::FpString& fp) {
        u64 left = width == 0 ? kUnbounded : width;
        const auto take = [&left]() {
            if (left == 0) return false;
            --left;
            return true;
        };
        const auto is_digit = [](i32 c) { return c >= '0' && c <= '9'; };
        std::vector<u32> text;
        bool started = false;
        i32 c = m_input.get();
        if (c == '-' || c == '+') {
            if (c == '-') text.push_back('-');
            if (left != 0) --left;
            c = m_input.get();
        }
        while (is_digit(c) && take()) {
            started = true;
            text.push_back(static_cast<u32>(c));
            c = m_input.get();
        }
        if (c == '.' && take()) {
            text.push_back('.');
            c = m_input.get();
            while (is_digit(c) && take()) {
                started = true;
                text.push_back(static_cast<u32>(c));
                c = m_input.get();
            }
        }
        if (started && (c == 'e' || c == 'E') && take()) {
            text.push_back('e');
            c = m_input.get();
            if (c == '-' || c == '+') {
                if (c == '-') text.push_back('-');
                if (take()) c = m_input.get();
            }
            while (is_digit(c) && take()) {
                text.push_back(static_cast<u32>(c));
                c = m_input.get();
            }
        }
        m_input.unget(c);
        if (!started) return false;
        struct TextSource {
            const std::vector<u32>& s;
            usize p{0};
            u32 get() { return p < s.size() ? s[p++] : (++p, 0u); }
            void unget(u32) { --p; }
            usize save_state() const { return p; }
            bool restore_state(usize state) {
                p = state;
                return true;
            }
        } source{text};
        result = crt_float::parse_from_source(source, crt_float::Syntax{false, false}, fp);
        return true;
    }

    bool process_float() {
        process_whitespace();
        const bool is_double = m_parser.size() == 8;
        crt_float::Result result = crt_float::Result::NoDigits;
        crt_float::FpString fp;
        if (ucrt()) {
            FieldSource source(m_input, m_parser.width());
            result = crt_float::parse_from_source(
                source, crt_float::Syntax{m_request.wide, true}, fp);
            if (!source.succeeded()) return false;
        } else if (!legacy_float(m_parser.width(), result, fp)) {
            return false;
        }
        const crt_float::Bits bits = crt_float::to_bits(result, fp, is_double, rounding());
        if (bits.no_digits) return false;
        if (m_parser.suppress()) return true;
        const GuestAddr p = static_cast<GuestAddr>(m_args.next_pointer(m_m));
        if (p == 0) {
            invalid(kEINVAL);
            return false;
        }
        // Stored as an integer so signalling and indeterminate NaNs keep
        // their bits.
        if (is_double) {
            (void)m_m.memory().write_u64(p, bits.bits);
        } else {
            (void)m_m.memory().write_u32(p, static_cast<u32>(bits.bits));
        }
        return true;
    }

    Marshaler& m_m;
    CrtMemoryManager& m_mm;
    Request m_request;
    FormatParser m_parser;
    Input& m_input;
    ArgCursor m_args;
    u64 m_assigned{0};
};

// ---- Entry points ----------------------------------------------------------------

std::vector<u32> read_format(Marshaler& m, GuestAddr format, bool wide) {
    auto& mem = m.memory();
    const usize n = wide ? guest_wcslen(mem, format) : guest_strlen(mem, format);
    std::vector<u32> units;
    units.reserve(n);
    for (usize i = 0; i < n; ++i) {
        units.push_back(wide ? mem.read_u16(format + i * 2).value_or(0)
                             : mem.read_u8(format + i).value_or(0));
    }
    return units;
}

/// The size_t "no limit" of the guest (SIZE_MAX at its width).
u64 guest_size_max(const Marshaler& m) {
    return m.cpu_is_64bit() ? std::numeric_limits<u64>::max()
                            : std::numeric_limits<u32>::max();
}

/// common_vsscanf (ucrt/stdio/input.cpp): validate, bound the input by
/// tcsnlen(buffer, count), run the processor.
int scan_string(Marshaler& m, CrtMemoryManager& mm, const Request& request,
                GuestAddr buffer, u64 count, GuestAddr format, ArgCursor args) {
    if (buffer == 0 || format == 0) {
        mm.set_errno(kEINVAL);
        (void)mm.invalid_parameter();
        return kEof;
    }
    const usize cap = static_cast<usize>(std::min<u64>(count, UNBOUNDED_STRING));
    const usize length = request.wide ? guest_wcslen(m.memory(), buffer, cap)
                                      : guest_strlen(m.memory(), buffer, cap);
    StringInput input(m.memory(), buffer, length, request.wide);
    InputProcessor processor(m, mm, request, read_format(m, format, request.wide), input,
                             args);
    return processor.process();
}

/// common_vfscanf / common_vfwscanf: validate (crt_ucrt_s6_probe: a NULL
/// stream or format sets EINVAL, then calls the handler, and returns EOF
/// without reading), then run the processor over the stream.
int scan_stream(Marshaler& m, CrtMemoryManager& mm, CrtStdioManager& io,
                const Request& request, GuestAddr stream, GuestAddr format,
                ArgCursor args) {
    if (stream == 0 || format == 0) {
        mm.set_errno(kEINVAL);
        (void)mm.invalid_parameter();
        return kEof;
    }
    if (request.wide &&
        io.guest_stream_encoding(static_cast<u32>(stream)) !=
            CrtStdioManager::WideEncoding::Ansi) {
        static std::atomic<bool> reported{false};
        if (!reported.exchange(true)) {
            std::fprintf(stderr,
                         "[vitreo] UNIMPLEMENTED ucrtbase!__stdio_common_vfwscanf: "
                         "binary, UTF-8 or UTF-16 stream\n");
        }
        return kEof;
    }
    // A "C"-locale text stream: StreamInput's bytes are the wide units.
    StreamInput input(io, static_cast<u32>(stream));
    InputProcessor processor(m, mm, request, read_format(m, format, request.wide),
                             input, args);
    return processor.process();
}

void set_int_return(Marshaler& m, int value) {
    m.set_return(static_cast<u64>(static_cast<u32>(value)));
}

} // namespace

void add_crt_scanf_exports(DllExports& exports, CrtStdioManager& io,
                           CrtMemoryManager& mm, CrtDataBlock& crt_data) {
    // ---- UCRT: int __stdio_common_vs[w]scanf(unsigned __int64 options,
    //      const T* buffer, size_t buffer_count, const T* format,
    //      _locale_t locale, va_list arglist) ----
    for (const bool wide : {false, true}) {
        exports.entries.push_back(
            {wide ? "__stdio_common_vswscanf" : "__stdio_common_vsscanf", 0,
             [wide, &mm](Marshaler& m) {
                 const bool is64 = m.cpu_is_64bit();
                 const usize base = is64 ? 1 : 2;
                 const u64 options =
                     is64 ? m.arg(0) : (m.arg(0) | (static_cast<u64>(m.arg(1)) << 32));
                 const Request request{Flavor::Ucrt, options, wide, false};
                 const u64 count = is64 ? m.arg(base + 1) : (m.arg(base + 1) & 0xFFFFFFFFULL);
                 set_int_return(m, scan_string(m, mm, request, m.arg_ptr(base), count,
                                               m.arg_ptr(base + 2),
                                               ArgCursor::va_list_at(m.arg_ptr(base + 4))));
             },
             0});
    }
    // int __stdio_common_vf[w]scanf(unsigned __int64 options, FILE* stream,
    //                               const T* format, _locale_t, va_list)
    for (const bool wide : {false, true}) {
        exports.entries.push_back(
            {wide ? "__stdio_common_vfwscanf" : "__stdio_common_vfscanf", 0,
             [wide, &mm, &io](Marshaler& m) {
                 const bool is64 = m.cpu_is_64bit();
                 const usize base = is64 ? 1 : 2;
                 const u64 options =
                     is64 ? m.arg(0) : (m.arg(0) | (static_cast<u64>(m.arg(1)) << 32));
                 const Request request{Flavor::Ucrt, options, wide, false};
                 set_int_return(m, scan_stream(m, mm, io, request, m.arg_ptr(base),
                                               m.arg_ptr(base + 1),
                                               ArgCursor::va_list_at(m.arg_ptr(base + 3))));
             },
             0});
    }

    // ---- msvcrt.dll string forms ----
    struct StringRow {
        const char* name;
        bool wide;
        bool secure;
        bool counted;  // _snscanf: (buffer, count, format, ...)
        bool locale;   // _l: a _locale_t before the variadic arguments
    };
    static constexpr StringRow kStringRows[] = {
        {"sscanf", false, false, false, false},
        {"_sscanf_l", false, false, false, true},
        {"sscanf_s", false, true, false, false},
        {"_sscanf_s_l", false, true, false, true},
        {"_snscanf", false, false, true, false},
        {"_snscanf_l", false, false, true, true},
        {"_snscanf_s", false, true, true, false},
        {"_snscanf_s_l", false, true, true, true},
        {"swscanf", true, false, false, false},
        {"_swscanf_l", true, false, false, true},
        {"swscanf_s", true, true, false, false},
        {"_swscanf_s_l", true, true, false, true},
        {"_snwscanf", true, false, true, false},
        {"_snwscanf_l", true, false, true, true},
        {"_snwscanf_s", true, true, true, false},
        {"_snwscanf_s_l", true, true, true, true},
    };
    for (const StringRow& row : kStringRows) {
        const StringRow r = row;
        exports.entries.push_back(
            {r.name, 0,
             [r, &mm](Marshaler& m) {
                 const Request request{
                     Flavor::Msvcrt,
                     kLegacyMsvcrtCompatibility | (r.secure ? kSecureCrt : 0), r.wide,
                     r.wide};
                 usize i = 0;
                 const GuestAddr buffer = m.arg_ptr(i++);
                 const u64 count = r.counted ? (m.arg(i++) & guest_size_max(m))
                                             : guest_size_max(m);
                 const GuestAddr format = m.arg_ptr(i++);
                 if (r.locale) ++i;
                 set_int_return(m, scan_string(m, mm, request, buffer, count, format,
                                               ArgCursor::call_args(i)));
             },
             0});
    }

    // ---- msvcrt.dll narrow stream forms ----
    struct StreamRow {
        const char* name;
        bool stdin_stream;  // scanf: no FILE* argument
        bool secure;
        bool locale;
    };
    static constexpr StreamRow kStreamRows[] = {
        {"fscanf", false, false, false},  {"_fscanf_l", false, false, true},
        {"fscanf_s", false, true, false}, {"_fscanf_s_l", false, true, true},
        {"scanf", true, false, false},    {"_scanf_l", true, false, true},
        {"scanf_s", true, true, false},   {"_scanf_s_l", true, true, true},
    };
    for (const StreamRow& row : kStreamRows) {
        const StreamRow r = row;
        exports.entries.push_back(
            {r.name, 0,
             [r, &mm, &io, &crt_data](Marshaler& m) {
                 const Request request{
                     Flavor::Msvcrt,
                     kLegacyMsvcrtCompatibility | (r.secure ? kSecureCrt : 0), false,
                     false};
                 usize i = 0;
                 const GuestAddr stream =
                     r.stdin_stream
                         ? static_cast<GuestAddr>(
                               crt_data.legacy_stream_addr(0, m.cpu_is_64bit()))
                         : m.arg_ptr(i++);
                 const GuestAddr format = m.arg_ptr(i++);
                 if (r.locale) ++i;
                 set_int_return(m, scan_stream(m, mm, io, request, stream, format,
                                               ArgCursor::call_args(i)));
             },
             0});
    }
}

} // namespace vitreo::winapi

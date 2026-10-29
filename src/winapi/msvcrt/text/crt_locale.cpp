// SPDX-License-Identifier: LGPL-2.1-only
/// @file crt_locale.cpp
/// @brief UCRT-compatible state for the supported CRT "C" locale

#include "winapi/msvcrt/text/crt_locale.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u16 CTYPE_UPPER = 0x0001;
constexpr u16 CTYPE_LOWER = 0x0002;
constexpr u16 CTYPE_DIGIT = 0x0004;
constexpr u16 CTYPE_SPACE = 0x0008;
constexpr u16 CTYPE_PUNCT = 0x0010;
constexpr u16 CTYPE_CONTROL = 0x0020;
constexpr u16 CTYPE_BLANK = 0x0040;
constexpr u16 CTYPE_HEX = 0x0080;
constexpr u16 CTYPE_ALPHA = 0x0100;
constexpr usize LC_CATEGORY_COUNT = 6;
constexpr usize LCONV_STRING_COUNT = 10;
constexpr usize LCONV_CHAR_COUNT = 8;
constexpr usize LCONV_WIDE_STRING_COUNT = 8;
constexpr const char* C_LOCALE_DAYS =
    ":Sun:Sunday:Mon:Monday:Tue:Tuesday:Wed:Wednesday:"
    "Thu:Thursday:Fri:Friday:Sat:Saturday";
constexpr const char* C_LOCALE_MONTHS =
    ":Jan:January:Feb:February:Mar:March:Apr:April:May:May:Jun:June:"
    "Jul:July:Aug:August:Sep:September:Oct:October:Nov:November:Dec:December";

std::array<u16, 256> make_c_locale_ctype() {
    std::array<u16, 256> table{};

    for (u16 value = 0; value < 0x20; ++value) {
        table[value] = CTYPE_CONTROL;
    }
    table[0x7F] = CTYPE_CONTROL;

    for (u16 value : {u16{'\t'}, u16{'\n'}, u16{'\v'}, u16{'\f'}, u16{'\r'}}) {
        table[value] |= CTYPE_SPACE;
    }
    table['\t'] |= CTYPE_BLANK;
    table[' '] = CTYPE_SPACE | CTYPE_BLANK;

    for (u16 value = '0'; value <= '9'; ++value) {
        table[value] = CTYPE_DIGIT | CTYPE_HEX;
    }
    for (u16 value = 'A'; value <= 'Z'; ++value) {
        table[value] = CTYPE_UPPER | CTYPE_ALPHA;
        if (value <= 'F') table[value] |= CTYPE_HEX;
    }
    for (u16 value = 'a'; value <= 'z'; ++value) {
        table[value] = CTYPE_LOWER | CTYPE_ALPHA;
        if (value <= 'f') table[value] |= CTYPE_HEX;
    }
    for (u16 value = 0x21; value <= 0x7E; ++value) {
        if (table[value] == 0) table[value] = CTYPE_PUNCT;
    }

    return table;
}

void write_pointer(std::vector<Byte>& buffer, usize offset, GuestAddr value,
                   bool is_64bit) {
    if (is_64bit) {
        const u64 pointer = value;
        std::memcpy(buffer.data() + offset, &pointer, sizeof(pointer));
    } else {
        const u32 pointer = static_cast<u32>(value);
        std::memcpy(buffer.data() + offset, &pointer, sizeof(pointer));
    }
}

std::vector<Byte> make_c_locale_lconv(bool is_64bit,
                                      GuestAddr decimal_point,
                                      GuestAddr empty_string,
                                      GuestAddr wide_decimal_point,
                                      GuestAddr wide_empty_string) {
    const usize pointer_size = is_64bit ? 8 : 4;
    const usize chars_offset = LCONV_STRING_COUNT * pointer_size;
    const usize wide_offset =
        align_up(chars_offset + LCONV_CHAR_COUNT, pointer_size);
    std::vector<Byte> result(
        wide_offset + LCONV_WIDE_STRING_COUNT * pointer_size, Byte{0});

    write_pointer(result, 0, decimal_point, is_64bit);
    for (usize index = 1; index < LCONV_STRING_COUNT; ++index) {
        write_pointer(result, index * pointer_size, empty_string, is_64bit);
    }

    std::fill_n(result.begin() + static_cast<std::ptrdiff_t>(chars_offset),
                LCONV_CHAR_COUNT, static_cast<Byte>(CHAR_MAX));

    write_pointer(result, wide_offset, wide_decimal_point, is_64bit);
    for (usize index = 1; index < LCONV_WIDE_STRING_COUNT; ++index) {
        write_pointer(result, wide_offset + index * pointer_size,
                      wide_empty_string, is_64bit);
    }
    return result;
}

} // namespace

CrtLocaleManager::CrtLocaleManager(GuestMemory& memory,
                                   CrtMemoryManager& memory_manager)
    : m_memory(memory),
      m_memory_manager(memory_manager) {
    const GuestAddr decimal_point = memory_manager.crt_malloc(2);
    const GuestAddr empty_string = memory_manager.crt_malloc(1);
    const GuestAddr wide_decimal_point = memory_manager.crt_malloc(4);
    const GuestAddr wide_empty_string = memory_manager.crt_malloc(2);
    m_pctype_addr = memory_manager.crt_malloc(256 * sizeof(u16));
    m_locale_names32_addr =
        memory_manager.crt_malloc(LC_CATEGORY_COUNT * sizeof(u32));
    m_locale_names64_addr =
        memory_manager.crt_malloc(LC_CATEGORY_COUNT * sizeof(u64));

    const auto lconv32 = make_c_locale_lconv(
        false, decimal_point, empty_string, wide_decimal_point, wide_empty_string);
    const auto lconv64 = make_c_locale_lconv(
        true, decimal_point, empty_string, wide_decimal_point, wide_empty_string);
    m_lconv32_addr = memory_manager.crt_malloc(lconv32.size());
    m_lconv64_addr = memory_manager.crt_malloc(lconv64.size());

    if (decimal_point == 0 || empty_string == 0 || wide_decimal_point == 0 ||
        wide_empty_string == 0 || m_pctype_addr == 0 ||
        m_locale_names32_addr == 0 || m_locale_names64_addr == 0 ||
        m_lconv32_addr == 0 || m_lconv64_addr == 0) {
        return;
    }

    const std::array<Byte, 2> decimal_bytes{Byte{'.'}, Byte{0}};
    const std::array<Byte, 1> empty_bytes{Byte{0}};
    const std::array<u16, 2> wide_decimal{u16{'.'}, u16{0}};
    const std::array<u16, 1> wide_empty{u16{0}};
    const auto ctype = make_c_locale_ctype();
    const std::array<Byte, LC_CATEGORY_COUNT * sizeof(u32)> names32{};
    const std::array<Byte, LC_CATEGORY_COUNT * sizeof(u64)> names64{};

    m_valid =
        memory.write_bytes(decimal_point, decimal_bytes.data(),
                           decimal_bytes.size()).is_ok() &&
        memory.write_bytes(empty_string, empty_bytes.data(),
                           empty_bytes.size()).is_ok() &&
        memory.write_bytes(wide_decimal_point,
                           reinterpret_cast<const Byte*>(wide_decimal.data()),
                           sizeof(wide_decimal)).is_ok() &&
        memory.write_bytes(wide_empty_string,
                           reinterpret_cast<const Byte*>(wide_empty.data()),
                           sizeof(wide_empty)).is_ok() &&
        memory.write_bytes(m_pctype_addr,
                           reinterpret_cast<const Byte*>(ctype.data()),
                           sizeof(ctype)).is_ok() &&
        memory.write_bytes(m_locale_names32_addr, names32.data(),
                           names32.size()).is_ok() &&
        memory.write_bytes(m_locale_names64_addr, names64.data(),
                           names64.size()).is_ok() &&
        memory.write_bytes(m_lconv32_addr, lconv32.data(),
                           lconv32.size()).is_ok() &&
        memory.write_bytes(m_lconv64_addr, lconv64.data(),
                           lconv64.size()).is_ok();
}

bool CrtLocaleManager::initialize_lconv_for_unsigned_char() {
    std::lock_guard lock(m_locale_lock);
    if (!m_valid) return false;
    // int_frac_digits .. n_sign_posn follow the ten string pointers in both
    // layouts; UCRT stores (char)UCHAR_MAX into each of them.
    const std::array<Byte, LCONV_CHAR_COUNT> unsigned_max{
        Byte{0xFF}, Byte{0xFF}, Byte{0xFF}, Byte{0xFF},
        Byte{0xFF}, Byte{0xFF}, Byte{0xFF}, Byte{0xFF}};
    return m_memory.write_bytes(m_lconv32_addr + LCONV_STRING_COUNT * 4,
                                unsigned_max.data(),
                                unsigned_max.size()).is_ok() &&
           m_memory.write_bytes(m_lconv64_addr + LCONV_STRING_COUNT * 8,
                                unsigned_max.data(),
                                unsigned_max.size()).is_ok();
}

GuestAddr CrtLocaleManager::allocate_ascii_time_string(const char* value,
                                                       bool wide) {
    std::lock_guard lock(m_locale_lock);
    const usize length = std::strlen(value);

    if (!wide) {
        const GuestAddr result = m_memory_manager.crt_malloc(length + 1);
        if (result == 0 ||
            m_memory.write_bytes(result,
                                 reinterpret_cast<const Byte*>(value),
                                 length + 1).is_err()) {
            if (result != 0) m_memory_manager.crt_free(result);
            return 0;
        }
        return result;
    }

    std::vector<u16> utf16(length + 1, 0);
    std::transform(value, value + length, utf16.begin(),
                   [](char ch) { return static_cast<u16>(
                       static_cast<unsigned char>(ch)); });
    const GuestAddr result =
        m_memory_manager.crt_malloc(utf16.size() * sizeof(u16));
    if (result == 0 ||
        m_memory.write_bytes(
            result, reinterpret_cast<const Byte*>(utf16.data()),
            utf16.size() * sizeof(u16)).is_err()) {
        if (result != 0) m_memory_manager.crt_free(result);
        return 0;
    }
    return result;
}

GuestAddr CrtLocaleManager::allocate_days(bool wide) {
    return allocate_ascii_time_string(C_LOCALE_DAYS, wide);
}

GuestAddr CrtLocaleManager::allocate_months(bool wide) {
    return allocate_ascii_time_string(C_LOCALE_MONTHS, wide);
}

DllExports create_crt_locale_thunks(CrtLocaleManager& mgr) {
    DllExports exports;
    exports.entries.reserve(14);

    exports.entries.push_back({"_lock_locales", 0, [&mgr](Marshaler&) {
        mgr.lock();
    }});
    exports.entries.push_back({"_unlock_locales", 0, [&mgr](Marshaler&) {
        mgr.unlock();
    }});
    exports.entries.push_back({"__pctype_func", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.valid() ? mgr.pctype_addr() : 0);
    }});
    exports.entries.push_back({"___lc_codepage_func", 0, [](Marshaler& m) {
        m.set_return(CrtLocaleManager::code_page());
    }});
    exports.entries.push_back({"___lc_collate_cp_func", 0, [](Marshaler& m) {
        m.set_return(CrtLocaleManager::collate_code_page());
    }});
    exports.entries.push_back({"___mb_cur_max_func", 0, [](Marshaler& m) {
        m.set_return(CrtLocaleManager::multibyte_max());
    }});
    exports.entries.push_back({"___lc_locale_name_func", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.valid()
                ? mgr.locale_names_addr(m.cpu_is_64bit())
                : 0);
        }});
    exports.entries.push_back({"localeconv", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.valid()
            ? mgr.locale_conventions_addr(m.cpu_is_64bit())
            : 0);
    }});
    // int __initialize_lconv_for_unsigned_char(void)
    // Called from _initterm_e by images built with /J; 0 means success.
    exports.entries.push_back({"__initialize_lconv_for_unsigned_char", 0,
        [&mgr](Marshaler& m) {
            m.set_return(mgr.initialize_lconv_for_unsigned_char() ? 0u : 1u);
        }});
    exports.entries.push_back({"_Getdays", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.allocate_days(false));
    }});
    exports.entries.push_back({"_Getmonths", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.allocate_months(false));
    }});
    exports.entries.push_back({"_W_Getdays", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.allocate_days(true));
    }});
    exports.entries.push_back({"_W_Getmonths", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.allocate_months(true));
    }});

    // int _setmbcp(int codepage)
    // Stores the multibyte code page; common values 0, -2, 65001, 1252.
    // Returns 0 on success, -1 on an unrecognized code page.
    exports.entries.push_back({"_setmbcp", 0, [](Marshaler& m) {
        static int s_mbcp = 0;
        const auto codepage = static_cast<i32>(m.arg(0));
        switch (codepage) {
        case 0:      // system ANSI default
        case -2:     // _MB_CP_SBCS
        case -3:     // _MB_CP_OEM
        case -4:     // _MB_CP_THREAD
        case 65001:  // UTF-8
        case 1252:   // Windows-1252
        case 437:
        case 932:
        case 936:
        case 949:
        case 950:
            s_mbcp = codepage;
            m.set_return(0);
            return;
        default:
            (void)s_mbcp;
            m.set_return(static_cast<u32>(-1));
            return;
        }
    }});

    return exports;
}

} // namespace vitreo::winapi

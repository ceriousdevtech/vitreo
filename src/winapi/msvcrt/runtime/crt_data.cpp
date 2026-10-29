// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64

#include "crt_data.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <string>
#include <vector>

namespace vitreo::winapi {

u16 CrtDataBlock::ctype_bits(u8 ch) noexcept {
    // The "C" locale table. Bytes >= 0x80 classify as nothing, matching the
    // CRT's single-byte default locale.
    if (ch == '\t') return CT_SPACE | CT_CONTROL;
    if (ch >= '\n' && ch <= '\r') return CT_SPACE | CT_CONTROL;
    if (ch == ' ') return CT_SPACE | CT_BLANK;
    if (ch < 0x20 || ch == 0x7F) return CT_CONTROL;
    if (ch >= '0' && ch <= '9') return CT_DIGIT | CT_HEX;
    if (ch >= 'A' && ch <= 'F') return CT_UPPER | CT_HEX;
    if (ch >= 'G' && ch <= 'Z') return CT_UPPER;
    if (ch >= 'a' && ch <= 'f') return CT_LOWER | CT_HEX;
    if (ch >= 'g' && ch <= 'z') return CT_LOWER;
    if (ch < 0x7F) return CT_PUNCT;
    return 0;
}

bool CrtDataBlock::initialize(GuestMemory& memory, u64 base, std::string_view cmdline) {
    auto result = memory.map_region(base, REGION_SIZE,
                                     MemoryProtection::ReadWrite);
    if (result.is_err()) {
        return false;
    }

    // Initialize _commode = 0 (commit mode)
    (void)memory.write_u32(base + COMMODE_OFFSET, 0);
    // Initialize _fmode = 0 (text mode)
    (void)memory.write_u32(base + FMODE_OFFSET, 0);
    // Initialize _adjust_fdiv = 0 (no FDIV bug workaround)
    (void)memory.write_u32(base + ADJUST_FDIV_OFFSET, 0);

    // Initialize _acmdln — write command line string at CMDLINE_STR_OFFSET,
    // then store its guest address at ACMDLN_OFFSET.
    const char default_cmdline[] = "program.exe";
    const char* cmd_src = cmdline.empty() ? default_cmdline : cmdline.data();
    m_command_line.assign(cmdline.empty() ? std::string_view(default_cmdline)
                                          : cmdline);
    usize cmd_len = cmdline.empty() ? sizeof(default_cmdline) : cmdline.size() + 1;
    // Strings share the region below the argv pointer table.
    constexpr usize max_string_bytes =
        static_cast<usize>(ARGV_TABLE_OFFSET - CMDLINE_STR_OFFSET);
    if (cmd_len > max_string_bytes) cmd_len = max_string_bytes;
    for (usize i = 0; i < cmd_len; ++i) {
        (void)memory.write_u8(base + CMDLINE_STR_OFFSET + i,
                              static_cast<u8>(i < cmd_len - 1 ? cmd_src[i] : '\0'));
    }
    (void)memory.write_u32(base + ACMDLN_OFFSET,
                           static_cast<u32>(base + CMDLINE_STR_OFFSET));

    // Build __argv / __argc from the same command line. Tokenize a working
    // copy so _acmdln keeps the original unbroken string. Supports basic
    // double-quoted arguments (Windows CommandLineToArgvW subset).
    std::vector<std::string> args;
    {
        std::string working(cmd_src, cmd_len > 0 ? cmd_len - 1 : 0);
        std::string current;
        bool in_quote = false;
        auto flush = [&]() {
            if (!current.empty()) {
                args.push_back(current);
                current.clear();
            }
        };
        for (char ch : working) {
            if (ch == '"') {
                in_quote = !in_quote;
                continue;
            }
            if (!in_quote && (ch == ' ' || ch == '\t')) {
                flush();
                continue;
            }
            current.push_back(ch);
        }
        flush();
        if (args.empty()) args.emplace_back("program.exe");
    }

    // Leave room for the null terminator slot in the argv table.
    constexpr usize max_argv =
        static_cast<usize>((ACRT_IOB_OFFSET - ARGV_TABLE_OFFSET) / 4) - 1;
    if (args.size() > max_argv) args.resize(max_argv);

    usize string_cursor = CMDLINE_STR_OFFSET + cmd_len;
    // Align string storage for the per-arg copies.
    string_cursor = (string_cursor + 3u) & ~usize{3};

    std::vector<u32> arg_addrs;
    arg_addrs.reserve(args.size());
    for (const auto& arg : args) {
        const usize need = arg.size() + 1;
        if (string_cursor + need > ARGV_TABLE_OFFSET) break;
        const u32 addr = static_cast<u32>(base + string_cursor);
        for (usize i = 0; i < arg.size(); ++i) {
            (void)memory.write_u8(base + string_cursor + i,
                                  static_cast<u8>(arg[i]));
        }
        (void)memory.write_u8(base + string_cursor + arg.size(), 0);
        arg_addrs.push_back(addr);
        string_cursor += need;
    }
    if (arg_addrs.empty()) {
        // Fall back to pointing argv[0] at the _acmdln string.
        arg_addrs.push_back(static_cast<u32>(base + CMDLINE_STR_OFFSET));
    }

    for (usize i = 0; i < arg_addrs.size(); ++i) {
        (void)memory.write_u32(base + ARGV_TABLE_OFFSET + i * 4, arg_addrs[i]);
    }
    (void)memory.write_u32(
        base + ARGV_TABLE_OFFSET + arg_addrs.size() * 4, 0);
    (void)memory.write_u32(base + ARGC_OFFSET,
                           static_cast<u32>(arg_addrs.size()));
    (void)memory.write_u32(base + ARGV_VAR_OFFSET,
                           static_cast<u32>(base + ARGV_TABLE_OFFSET));

    // Modern UCRT FILE is opaque to callers. The backing cells exposed by
    // _get_stream_buffer_pointers start empty, which models unbuffered streams.
    for (u32 index = 0; index < ACRT_STREAM_COUNT; ++index) {
        const u64 stream = base + ACRT_IOB_OFFSET + index * ACRT_FILE_SIZE;
        (void)memory.write_u64(stream + ACRT_PTR_OFFSET, 0);
        (void)memory.write_u32(stream + ACRT_COUNT_OFFSET, 0);
        (void)memory.write_u64(stream + ACRT_BASE_OFFSET, 0);
    }

    // Legacy msvcrt `FILE _iob[3]`. The struct is public in older CRT headers:
    //   char* _ptr; int _cnt; char* _base; int _flag;
    //   int _file; int _charbuf; int _bufsiz; char* _tmpfname;
    // Only _file and _flag are meaningful to a guest that hands the FILE*
    // straight back to the CRT; the buffer cells stay null (unbuffered).
    constexpr u32 IOREAD  = 0x0001;
    constexpr u32 IOWRT   = 0x0002;
    for (u32 index = 0; index < ACRT_STREAM_COUNT; ++index) {
        const u64 file = base + IOB_OFFSET + index * LEGACY_FILE_SIZE;
        for (u64 off = 0; off < LEGACY_FILE_SIZE; off += 4) {
            (void)memory.write_u32(file + off, 0);
        }
        (void)memory.write_u32(file + 12, index == 0 ? IOREAD : IOWRT);
        (void)memory.write_u32(file + 16, index);
    }
    // The x64 `_iob`: char* _ptr; int _cnt; char* _base; int _flag;
    // int _file; int _charbuf; int _bufsiz; char* _tmpfname.
    for (u32 index = 0; index < ACRT_STREAM_COUNT; ++index) {
        const u64 file = base + IOB64_OFFSET + index * LEGACY_FILE_SIZE64;
        for (u64 off = 0; off < LEGACY_FILE_SIZE64; off += 8) {
            (void)memory.write_u64(file + off, 0);
        }
        (void)memory.write_u32(file + LEGACY64_FLAG_OFFSET,
                               index == 0 ? IOREAD : IOWRT);
        (void)memory.write_u32(file + LEGACY64_FILE_OFFSET, index);
    }

    // ctype table + the `_pctype` pointer variable that indexes into it.
    for (u64 slot = 0; slot < CTYPE_TOTAL_SLOTS; ++slot) {
        const u16 bits = slot < CTYPE_NEGATIVE_SLOTS
                             ? 0
                             : ctype_bits(static_cast<u8>(
                                   slot - CTYPE_NEGATIVE_SLOTS));
        (void)memory.write_u16(base + CTYPE_TABLE_OFFSET + slot * 2, bits);
    }
    (void)memory.write_u32(
        base + PCTYPE_PTR_OFFSET,
        static_cast<u32>(base + CTYPE_TABLE_OFFSET + CTYPE_NEGATIVE_SLOTS * 2));
    // Single-byte "C" locale.
    (void)memory.write_u32(base + MB_CUR_MAX_OFFSET, 1);

    // _HUGE / HUGE_VAL — IEEE-754 positive infinity (MSVC exports the double).
    constexpr u64 huge_bits = 0x7FF0000000000000ull;
    (void)memory.write_u64(base + HUGE_OFFSET, huge_bits);

    // _wcmdln — UTF-16LE copy of the same command line as _acmdln.
    // ASCII code points are stored as zero-extended wchar_t (sufficient for the
    // capsule paths Vitreo passes through at process start).
    constexpr usize max_wchars =
        static_cast<usize>((REGION_SIZE - WCMDLINE_STR_OFFSET) / 2) - 1;
    const usize wlen = std::min(cmd_len > 0 ? cmd_len - 1 : 0, max_wchars);
    for (usize i = 0; i < wlen; ++i) {
        const u16 ch = static_cast<u8>(cmd_src[i]);
        (void)memory.write_u16(base + WCMDLINE_STR_OFFSET + i * 2, ch);
    }
    (void)memory.write_u16(base + WCMDLINE_STR_OFFSET + wlen * 2, 0);
    (void)memory.write_u32(base + WCMDLN_OFFSET,
                           static_cast<u32>(base + WCMDLINE_STR_OFFSET));
    (void)memory.write_u16(base + EMPTY_WSTRING_OFFSET, 0);

    m_base = base;
    m_initialized = true;
    return true;
}

u64 CrtDataBlock::resolve_data_import(std::string_view name) const {
    if (!m_initialized) {
        return 0;
    }
    if (name == "_commode") {
        return commode_addr();
    }
    if (name == "_fmode") {
        return fmode_addr();
    }
    if (name == "_adjust_fdiv") {
        return adjust_fdiv_addr();
    }
    if (name == "_acmdln") {
        return acmdln_addr();
    }
    if (name == "_wcmdln") {
        return wcmdln_addr();
    }
    if (name == "_HUGE") {
        return huge_addr();
    }
    if (name == "__argc") {
        return argc_addr();
    }
    if (name == "__argv") {
        return argv_var_addr();
    }
    if (name == "_iob") {
        return iob_addr();
    }
    if (name == "__initenv") {
        return initenv_addr();
    }
    if (name == "__winitenv") {
        return winitenv_addr();
    }
    if (name == "_pctype") {
        return pctype_addr();
    }
    if (name == "__mb_cur_max") {
        return mb_cur_max_addr();
    }
    return 0;
}

} // namespace vitreo::winapi

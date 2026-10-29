#pragma once
/// @file crt_data.hpp
/// @brief CRT global data block for msvcrt data imports (_commode, _fmode, _iob)

#include <vitreo/common.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

/// Manages the CRT global data block in guest memory.
///
/// Some CRT "imports" like _commode and _fmode are actually extern data
/// variables, not functions. The IAT entry must point to the actual data
/// in guest memory, not to a thunk. This class allocates and manages
/// that data region.
class CrtDataBlock {
public:
    /// Default base address for the CRT data region
    static constexpr u64 DEFAULT_BASE = 0x6FF00000;
    static constexpr u64 REGION_SIZE  = 0x2000; // 8 KiB

    // Offsets within the data block
    static constexpr u64 COMMODE_OFFSET    = 0x00; ///< int _commode
    static constexpr u64 FMODE_OFFSET      = 0x04; ///< int _fmode
    static constexpr u64 ADJUST_FDIV_OFFSET = 0x08; ///< int _adjust_fdiv
    static constexpr u64 ACMDLN_OFFSET     = 0x0C; ///< char* _acmdln
    static constexpr u64 IOB_OFFSET        = 0x10; ///< FILE _iob[3] (x86)
    static constexpr u64 LEGACY_FILE_SIZE  = 32;
    /// The x64 `FILE _iob[3]`. msvcrt's struct _iobuf has three pointer
    /// members, so an x64 FILE is 48 bytes with _flag at +24 and _file at +28
    /// (measured, crt_msvcrt_stdio_probe: stdout is __iob_func()+48). It
    /// cannot share the x86 array's slot, whose 96 bytes end at __argc.
    static constexpr u64 IOB64_OFFSET        = 0xE00;
    static constexpr u64 LEGACY_FILE_SIZE64  = 48;
    static constexpr u64 LEGACY64_FLAG_OFFSET = 24;
    static constexpr u64 LEGACY64_FILE_OFFSET = 28;
    static constexpr u64 ARGC_OFFSET       = 0x70; ///< int __argc
    static constexpr u64 ARGV_VAR_OFFSET   = 0x74; ///< char** __argv
    static constexpr u64 CMDLINE_STR_OFFSET = 0x100; ///< actual command line string bytes
    /// Null-terminated `char*` table for `__argv`. Placed just below the UCRT
    /// FILE objects so the cmdline/string area can grow up to it.
    static constexpr u64 ARGV_TABLE_OFFSET = 0xB00;
    static constexpr u64 ACRT_IOB_OFFSET   = 0xC00; ///< opaque UCRT FILE objects
    static constexpr u64 ACRT_FILE_SIZE    = 0x40;
    static constexpr u64 ACRT_STREAM_COUNT = 3;
    static constexpr u64 ACRT_PTR_OFFSET   = 0x00;
    static constexpr u64 ACRT_COUNT_OFFSET = 0x08;
    static constexpr u64 ACRT_BASE_OFFSET  = 0x10;

    /// int __mb_cur_max — bytes per multibyte character in the current locale.
    static constexpr u64 MB_CUR_MAX_OFFSET = 0xD00;
    /// unsigned short* _pctype — pointer variable, not the table itself.
    static constexpr u64 PCTYPE_PTR_OFFSET = 0xD08;
    /// double _HUGE — same bit pattern as HUGE_VAL (+infinity).
    static constexpr u64 HUGE_OFFSET = 0xD10;
    /// wchar_t* _wcmdln — pointer cell (wide command line lives at WCMDLINE_STR).
    static constexpr u64 WCMDLN_OFFSET = 0xD18;
    /// char** __initenv / wchar_t** __winitenv — the environment block handed
    /// to main/wmain. CRT startup stores into these through __p___initenv /
    /// __p___winitenv, so the accessors must hand back writable cells.
    static constexpr u64 INITENV_OFFSET = 0xD20;
    static constexpr u64 WINITENV_OFFSET = 0xD28;
    /// wchar_t** __wargv — the wide argv variable. UCRT's __p___wargv hands
    /// back the address of this cell, and the caller dereferences whatever it
    /// finds there, so it must be a real writable cell pointing at a real
    /// table. The missing-export stub returned 0xFFFFFFFF, which pwsh.exe
    /// dereferenced 601 instructions into startup.
    static constexpr u64 WARGV_VAR_OFFSET = 0xD30;
    /// Stable empty UTF-16 string for preinitialization WinMain queries.
    static constexpr u64 EMPTY_WSTRING_OFFSET = 0xD38;
    /// std::exception's vftable, RTTI locator and "Unknown exception" text
    /// (crt_exception_class.cpp), after the x64 _iob array.
    static constexpr u64 EXCEPTION_RTTI_OFFSET = 0xEA0;
    static constexpr u64 EXCEPTION_RTTI_SIZE = 0x160;
    /// Wide command-line string storage (UTF-16LE).
    static constexpr u64 WCMDLINE_STR_OFFSET = 0x1800;
    /// unsigned short _ctype_table[] — the classification table _pctype points
    /// into. Indices run from -128 through 255 because the CRT's ctype macros
    /// pass a plain (possibly negative) int straight through as an index.
    static constexpr u64 CTYPE_TABLE_OFFSET = 0x1000;
    static constexpr u64 CTYPE_NEGATIVE_SLOTS = 128;
    static constexpr u64 CTYPE_TOTAL_SLOTS = CTYPE_NEGATIVE_SLOTS + 256;

    // MSVC ctype classification bits (public, stable across CRT versions).
    static constexpr u16 CT_UPPER   = 0x0001;
    static constexpr u16 CT_LOWER   = 0x0002;
    static constexpr u16 CT_DIGIT   = 0x0004;
    static constexpr u16 CT_SPACE   = 0x0008;
    static constexpr u16 CT_PUNCT   = 0x0010;
    static constexpr u16 CT_CONTROL = 0x0020;
    static constexpr u16 CT_BLANK   = 0x0040;
    static constexpr u16 CT_HEX     = 0x0080;

    /// Classification bits for one byte value in the "C" locale.
    [[nodiscard]] static u16 ctype_bits(u8 ch) noexcept;

    CrtDataBlock() = default;

    /// Allocate and initialize the CRT data block in guest memory.
    /// @param memory Guest memory to map the region in
    /// @param base Base address (default: DEFAULT_BASE)
    /// @param cmdline Command line string for _acmdln (default: empty → "program.exe")
    /// @return true on success, false on failure
    [[nodiscard]] bool initialize(GuestMemory& memory, u64 base = DEFAULT_BASE,
                                  std::string_view cmdline = "");

    /// Get the base address of the CRT data block
    [[nodiscard]] u64 base() const noexcept { return m_base; }

    /// Get address of _commode
    [[nodiscard]] u64 commode_addr() const noexcept { return m_base + COMMODE_OFFSET; }

    /// Get address of _fmode
    [[nodiscard]] u64 fmode_addr() const noexcept { return m_base + FMODE_OFFSET; }

    /// Get address of _adjust_fdiv
    [[nodiscard]] u64 adjust_fdiv_addr() const noexcept { return m_base + ADJUST_FDIV_OFFSET; }

    /// Get address of _acmdln
    [[nodiscard]] u64 acmdln_addr() const noexcept { return m_base + ACMDLN_OFFSET; }

    /// Get address of _wcmdln (wchar_t* cell)
    [[nodiscard]] u64 wcmdln_addr() const noexcept { return m_base + WCMDLN_OFFSET; }

    /// Get address of _HUGE (double)
    [[nodiscard]] u64 huge_addr() const noexcept { return m_base + HUGE_OFFSET; }

    /// Address of the `_iob` array for the guest's bitness (set_guest_64bit).
    [[nodiscard]] u64 iob_addr() const noexcept {
        return legacy_iob_addr(m_guest_64bit);
    }

    /// Address of the x86 (32-byte FILE) or x64 (48-byte FILE) `_iob` array.
    [[nodiscard]] u64 legacy_iob_addr(bool is_64bit) const noexcept {
        return m_base + (is_64bit ? IOB64_OFFSET : IOB_OFFSET);
    }

    /// `&_iob[index]` for the given bitness.
    [[nodiscard]] u64 legacy_stream_addr(u32 index, bool is_64bit) const noexcept {
        return legacy_iob_addr(is_64bit) +
               index * (is_64bit ? LEGACY_FILE_SIZE64 : LEGACY_FILE_SIZE);
    }

    /// Standard stream index (0..2) of a legacy msvcrt FILE* in either
    /// array, or -1.
    [[nodiscard]] i32 legacy_stream_index(u64 stream) const noexcept {
        for (const bool is64 : {false, true}) {
            const u64 base = legacy_iob_addr(is64);
            const u64 size = is64 ? LEGACY_FILE_SIZE64 : LEGACY_FILE_SIZE;
            if (stream < base) continue;
            const u64 offset = stream - base;
            if (offset % size != 0) continue;
            const u64 index = offset / size;
            if (index < ACRT_STREAM_COUNT) return static_cast<i32>(index);
        }
        return -1;
    }

    /// The guest image's bitness; picks the `_iob` layout the `_iob` data
    /// export and iob_addr() publish. Set before imports are resolved.
    void set_guest_64bit(bool is_64bit) noexcept { m_guest_64bit = is_64bit; }
    [[nodiscard]] bool guest_64bit() const noexcept { return m_guest_64bit; }

    /// Address of the `char** __initenv` cell.
    [[nodiscard]] u64 initenv_addr() const noexcept {
        return m_base + INITENV_OFFSET;
    }

    /// Address of the `wchar_t** __winitenv` cell.
    [[nodiscard]] u64 winitenv_addr() const noexcept {
        return m_base + WINITENV_OFFSET;
    }

    /// Get address of the `unsigned short* _pctype` pointer variable.
    [[nodiscard]] u64 pctype_addr() const noexcept {
        return m_base + PCTYPE_PTR_OFFSET;
    }

    /// Get address of `int __mb_cur_max`.
    [[nodiscard]] u64 mb_cur_max_addr() const noexcept {
        return m_base + MB_CUR_MAX_OFFSET;
    }

    /// Get address of ctype table slot 0 (i.e. the value _pctype holds).
    [[nodiscard]] u64 ctype_zero_addr() const noexcept {
        return m_base + CTYPE_TABLE_OFFSET + CTYPE_NEGATIVE_SLOTS * 2;
    }

    /// Get address of the writable __argc global.
    [[nodiscard]] u64 argc_addr() const noexcept {
        return m_base + ARGC_OFFSET;
    }

    /// Get address of the writable `char** __argv` global (not the table).
    [[nodiscard]] u64 argv_var_addr() const noexcept {
        return m_base + ARGV_VAR_OFFSET;
    }

    /// Get address of the `char* argv[]` pointer table.
    [[nodiscard]] u64 argv_table_addr() const noexcept {
        return m_base + ARGV_TABLE_OFFSET;
    }

    /// Get address of the writable `wchar_t** __wargv` global.
    [[nodiscard]] u64 wargv_var_addr() const noexcept {
        return m_base + WARGV_VAR_OFFSET;
    }

    /// Get the opaque UCRT FILE object for stdin/stdout/stderr.
    [[nodiscard]] u64 acrt_stream_addr(u32 index) const noexcept {
        return index < ACRT_STREAM_COUNT
                   ? m_base + ACRT_IOB_OFFSET + index * ACRT_FILE_SIZE
                   : 0;
    }

    /// Return a standard stream index for a UCRT FILE pointer, or -1.
    [[nodiscard]] i32 acrt_stream_index(u64 stream) const noexcept {
        if (stream < m_base + ACRT_IOB_OFFSET) return -1;
        const u64 offset = stream - (m_base + ACRT_IOB_OFFSET);
        if (offset % ACRT_FILE_SIZE != 0) return -1;
        const u64 index = offset / ACRT_FILE_SIZE;
        return index < ACRT_STREAM_COUNT ? static_cast<i32>(index) : -1;
    }

    /// Check if a function name is a CRT data import (not a thunk)
    /// @return Guest data address if it's a data import, 0 otherwise
    [[nodiscard]] u64 resolve_data_import(std::string_view name) const;

    /// Whether the block has been initialized
    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }

    /// The full, untruncated process command line (the text _acmdln holds).
    [[nodiscard]] const std::string& command_line() const noexcept {
        return m_command_line;
    }

    /// Lists the names in a directory (Windows path, "" = current directory)
    /// for the UCRT's _crt_argv_expanded_arguments wildcard expansion.
    /// Injected by the DLL registry, which owns the VFS.
    using DirectoryLister = std::function<std::optional<std::vector<std::string>>(
        const std::string& directory)>;
    void set_directory_lister(DirectoryLister lister) {
        m_directory_lister = std::move(lister);
    }
    [[nodiscard]] const DirectoryLister& directory_lister() const noexcept {
        return m_directory_lister;
    }

private:
    u64 m_base{0};
    bool m_guest_64bit{false};
    bool m_initialized{false};
    std::string m_command_line;
    DirectoryLister m_directory_lister;
};

} // namespace vitreo::winapi

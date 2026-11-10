// SPDX-License-Identifier: LGPL-2.1-only

/// @file profile.cpp
/// @brief kernel32 INI profile exports, A and W, over the one INI model
///        (ini_profile.hpp).
///
/// GetPrivateProfileString/Int/Section/SectionNames/Struct,
/// WritePrivateProfileString/Section/Struct and the win.ini forms
/// GetProfileString/Int/Section and WriteProfileString/Section.
/// Microsoft Learn for each function, plus the Windows 11 measurements in
/// tests/oracle_data/profile_api_probe.x86/x64.expected
/// (tools/oracle_probe/profile_api_probe.cpp). The contract and its documented
/// gaps are in .docs/contracts/kernel32.md § Profile (INI) files.
///
/// Every A export works in UTF-16 internally: its strings are decoded from
/// the process ANSI code page and its results encoded back (unmappable
/// characters become the code page's default character), so A and W share
/// every rule and differ only in their output units.

#include "winapi/kernel32/kernel32_ext.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/environment/ini_profile.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace vitreo::winapi {

namespace {

using ini::Document;

/// Windows serializes profile access; so do the read-modify-write cycles here.
std::mutex& profile_mutex() {
    static std::mutex mutex;
    return mutex;
}

constexpr usize kMaxGuestString = 65536;

[[nodiscard]] std::u16string from_ansi(const std::string& bytes) {
    if (bytes.empty()) return {};
    std::u16string out(bytes.size(), u'\0');
    const auto r = codepage::multi_byte_to_wide(
        codepage::ansi_code_page(), 0, reinterpret_cast<const u8*>(bytes.data()),
        bytes.size(), reinterpret_cast<u16*>(out.data()), out.size());
    out.resize(r.ok() ? r.count : 0);
    return out;
}

[[nodiscard]] std::string to_ansi(std::u16string_view text) {
    if (text.empty()) return {};
    const auto* src = reinterpret_cast<const u16*>(text.data());
    const u32 cp = codepage::ansi_code_page();
    const auto need =
        codepage::wide_to_multi_byte(cp, 0, src, text.size(), nullptr, 0, nullptr, false);
    if (!need.ok() || need.count == 0) return {};
    std::string out(need.count, '\0');
    const auto r = codepage::wide_to_multi_byte(cp, 0, src, text.size(),
                                                reinterpret_cast<u8*>(out.data()),
                                                out.size(), nullptr, false);
    out.resize(r.ok() ? r.count : 0);
    return out;
}

/// Learn (GetPrivateProfileString, lpFileName): a name without a path is
/// looked up in the Windows directory.
[[nodiscard]] std::string profile_path(const std::u16string& name) {
    const bool has_path = name.find_first_of(u"\\/:") != std::u16string::npos;
    const std::u16string full = has_path ? name : u"C:\\Windows\\" + name;
    return utf16_to_utf8(reinterpret_cast<const u16*>(full.data()),
                         static_cast<int>(full.size()));
}

struct Loaded {
    bool exists = false;
    Document doc;
};

[[nodiscard]] Loaded load(capsule::Vfs& vfs, const std::optional<std::u16string>& file) {
    Loaded loaded;
    if (!file || file->empty()) return loaded;
    auto fh = vfs.open(profile_path(*file), capsule::AccessMode::Read,
                       capsule::ShareMode::All, capsule::CreationDisposition::OpenExisting);
    if (fh.is_err()) return loaded;
    std::vector<u8> content;
    std::vector<u8> buf(65536);
    bool ok = true;
    for (;;) {
        auto nr = vfs.read(fh.value(), std::span<u8>(buf));
        if (nr.is_err()) { ok = false; break; }
        if (nr.value() == 0) break;
        content.insert(content.end(), buf.begin(),
                       buf.begin() + static_cast<std::ptrdiff_t>(nr.value()));
    }
    (void)vfs.close(fh.value());
    if (!ok) return loaded;
    loaded.exists = true;
    loaded.doc = Document::parse(content, codepage::ansi_code_page());
    return loaded;
}

/// Write the document back; returns 0 or the Win32 error.
[[nodiscard]] u32 store(capsule::Vfs& vfs, const std::u16string& file, const Document& doc) {
    const std::vector<u8> bytes = doc.serialize(codepage::ansi_code_page());
    auto fh = vfs.open(profile_path(file), capsule::AccessMode::Write,
                       capsule::ShareMode::None, capsule::CreationDisposition::CreateAlways);
    if (fh.is_err()) return vfs_error_to_win32(fh.error());
    u32 error = 0;
    if (!bytes.empty()) {
        auto nw = vfs.write(fh.value(), std::span<const u8>(bytes));
        if (nw.is_err()) error = vfs_error_to_win32(nw.error());
        else if (nw.value() != bytes.size()) error = error_codes::ERROR_WRITE_FAULT;
    }
    (void)vfs.close(fh.value());
    return error;
}

/// The answer GetPrivateProfileString computes before it touches the buffer.
struct Answer {
    bool found = false;
    bool is_list = false;
    std::u16string value;
    std::vector<std::u16string> list;
};

[[nodiscard]] Answer query(const Loaded& f, const std::optional<std::u16string>& section,
                           const std::optional<std::u16string>& key) {
    Answer a;
    if (!f.exists) return a;
    if (!section || !key) {
        a.is_list = true;
        if (!section) {
            a.list = f.doc.section_names();
        } else if (auto keys = f.doc.key_names(*section)) {
            a.list = std::move(*keys);
        }
        // Nothing to list answers the default (enumA.*.absent_*).
        a.found = !a.list.empty();
        return a;
    }
    if (auto v = f.doc.value(*section, *key)) {
        a.found = true;
        a.value = std::move(*v);
    }
    return a;
}

/// GetPrivateProfileInt's conversion, consistent with every measured row
/// (intA.*): leading blanks, one sign, a 0x (and, per Learn
/// RtlCharToInteger base 0, 0o / 0b) prefix, digits up to the first
/// non-digit, 32-bit wrap-around.
[[nodiscard]] u32 parse_profile_int(std::u16string_view s) {
    usize i = 0;
    while (i < s.size() && (s[i] == u' ' || s[i] == u'\t')) ++i;
    bool negative = false;
    if (i < s.size() && (s[i] == u'+' || s[i] == u'-')) {
        negative = s[i] == u'-';
        ++i;
    }
    u32 base = 10;
    if (i + 1 < s.size() && s[i] == u'0') {
        const char16_t p = s[i + 1];
        if (p == u'x' || p == u'X') base = 16;
        else if (p == u'o' || p == u'O') base = 8;
        else if (p == u'b' || p == u'B') base = 2;
        if (base != 10) i += 2;
    }
    u32 value = 0;
    for (; i < s.size(); ++i) {
        const char16_t c = s[i];
        u32 digit = 0;
        if (c >= u'0' && c <= u'9') digit = c - u'0';
        else if (c >= u'a' && c <= u'f') digit = c - u'a' + 10;
        else if (c >= u'A' && c <= u'F') digit = c - u'A' + 10;
        else break;
        if (digit >= base) break;
        value = value * base + digit;
    }
    return negative ? 0u - value : value;
}

/// One export call: guest memory, the file system and the character set.
class ProfileCall {
public:
    ProfileCall(GuestMemory& memory, capsule::Vfs& vfs, bool wide)
        : m_memory(memory), m_vfs(vfs), m_wide(wide) {}

    [[nodiscard]] std::optional<std::u16string> text(GuestAddr addr) const {
        if (addr == 0) return std::nullopt;
        if (m_wide) {
            std::u16string s;
            for (usize i = 0; i < kMaxGuestString; ++i) {
                auto r = m_memory.read_u16(addr + i * 2);
                if (r.is_err() || r.value() == 0) break;
                s.push_back(static_cast<char16_t>(r.value()));
            }
            return s;
        }
        return from_ansi(ansi_bytes(addr));
    }

    /// A double-NUL-terminated list (WritePrivateProfileSection lpString).
    [[nodiscard]] std::vector<std::u16string> multi_text(GuestAddr addr) const {
        std::vector<std::u16string> entries;
        const usize unit = m_wide ? 2 : 1;
        usize offset = 0;
        while (offset < kMaxGuestString * 16) {
            const GuestAddr at = addr + offset * unit;
            std::u16string entry;
            usize length = 0;
            if (m_wide) {
                entry = *text(at);
                length = entry.size();
            } else {
                const std::string bytes = ansi_bytes(at);
                length = bytes.size();
                entry = from_ansi(bytes);
            }
            if (length == 0) break;
            entries.push_back(std::move(entry));
            offset += length + 1;
        }
        return entries;
    }

    /// Output units of the export's character set (ANSI bytes for A).
    [[nodiscard]] std::vector<u16> units(std::u16string_view s) const {
        if (m_wide) return std::vector<u16>(s.begin(), s.end());
        const std::string bytes = to_ansi(s);
        std::vector<u16> out;
        out.reserve(bytes.size());
        for (char c : bytes) out.push_back(static_cast<u8>(c));
        return out;
    }

    [[nodiscard]] std::vector<u16> list_units(const std::vector<std::u16string>& list) const {
        std::vector<u16> out;
        for (const auto& entry : list) {
            const auto u = units(entry);
            out.insert(out.end(), u.begin(), u.end());
            out.push_back(0);
        }
        return out;
    }

    void put(GuestAddr out, usize index, u16 unit) const {
        if (m_wide) (void)m_memory.write_u16(out + index * 2, unit);
        else (void)m_memory.write_u8(out + index, static_cast<u8>(unit));
    }

    /// Store a string truncated to @p size - 1 units plus its NUL.
    /// Returns the units stored before the NUL.
    u32 put_string(GuestAddr out, u32 size, const std::vector<u16>& s) const {
        const usize n = std::min<usize>(s.size(), size - 1);
        for (usize i = 0; i < n; ++i) put(out, i, s[i]);
        put(out, n, 0);
        return static_cast<u32>(n);
    }

    /// Store a list (entries each with their NUL) plus the final NUL. A list
    /// that does not fit is cut at @p size - 2 units and ends in two NULs.
    /// Returns {units stored before the final NUL, truncated}.
    std::pair<u32, bool> put_list(GuestAddr out, u32 size, const std::vector<u16>& list) const {
        if (list.size() + 1 <= size) {
            for (usize i = 0; i < list.size(); ++i) put(out, i, list[i]);
            put(out, list.size(), 0);
            return {static_cast<u32>(list.size()), false};
        }
        if (size == 0) return {0, true};
        if (size == 1) {
            put(out, 0, 0);
            return {0, true};
        }
        for (usize i = 0; i < size - 2; ++i) put(out, i, list[i]);
        put(out, size - 2, 0);
        put(out, size - 1, 0);
        return {size - 2, true};
    }

    // ---- GetPrivateProfileString ----
    u32 get_string(GuestAddr sec_addr, GuestAddr key_addr, GuestAddr def_addr,
                   GuestAddr out, u32 size, const std::optional<std::u16string>& file) const {
        if (out == 0) {
            // Measured nSize 0: 0, ERROR_INVALID_PARAMETER. nSize > 0 faults
            // on Windows (child.nullbuf*10); Vitreo fails the same way
            // instead of crashing the guest.
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        const auto section = text(sec_addr);
        const auto key = text(key_addr);
        if (!m_wide && section && section->empty()) {
            // GetPrivateProfileStringA("") faults on Windows
            // (child.emptysecA); the W form finds "[]". Fail instead.
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (size == 0) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            return 0;
        }
        Answer a;
        {
            std::lock_guard lock(profile_mutex());
            a = query(load(m_vfs, file), section, key);
        }
        if (a.found && a.is_list) {
            const auto [n, truncated] = put_list(out, size, list_units(a.list));
            set_last_error(truncated ? error_codes::ERROR_MORE_DATA : error_codes::ERROR_SUCCESS);
            return n;
        }
        if (a.found) {
            const auto v = units(a.value);
            const u32 n = put_string(out, size, v);
            // Even an exact fit (len == nSize - 1) reports ERROR_MORE_DATA
            // (truncA.value.size_21).
            set_last_error(v.size() >= size - 1 ? error_codes::ERROR_MORE_DATA
                                                : error_codes::ERROR_SUCCESS);
            return n;
        }
        std::u16string def = text(def_addr).value_or(u"");
        while (!def.empty() && def.back() == u' ') def.pop_back();
        const u32 n = put_string(out, size, units(def));
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return n;
    }

    // ---- GetPrivateProfileInt ----
    u32 get_int(GuestAddr sec_addr, GuestAddr key_addr, u32 n_default,
                const std::optional<std::u16string>& file) const {
        Answer a;
        {
            std::lock_guard lock(profile_mutex());
            a = query(load(m_vfs, file), text(sec_addr), text(key_addr));
        }
        if (!a.found) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return n_default;
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        const std::u16string& s = a.is_list ? a.list.front() : a.value;
        if (s.empty()) return n_default;  // intA.emptyint: default, error 0
        return parse_profile_int(s);
    }

    // ---- GetPrivateProfileSection ----
    u32 get_section(GuestAddr sec_addr, GuestAddr out, u32 size,
                    const std::optional<std::u16string>& file) const {
        if (out == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (size == 0) return 0;
        const auto section = text(sec_addr);
        std::optional<std::vector<std::u16string>> entries;
        {
            std::lock_guard lock(profile_mutex());
            const Loaded f = load(m_vfs, file);
            if (f.exists && section) entries = f.doc.section_entries(*section);
        }
        if (!entries) {
            put(out, 0, 0);
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return 0;
        }
        const auto [n, truncated] = put_list(out, size, list_units(*entries));
        // Measured: a truncated section leaves the last error unchanged.
        if (!truncated) set_last_error(error_codes::ERROR_SUCCESS);
        return n;
    }

    // ---- GetPrivateProfileSectionNames ----
    u32 get_section_names(GuestAddr out, u32 size,
                          const std::optional<std::u16string>& file) const {
        if (out == 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (size == 0) {
            set_last_error(error_codes::ERROR_MORE_DATA);
            return 0;
        }
        Loaded f;
        {
            std::lock_guard lock(profile_mutex());
            f = load(m_vfs, file);
        }
        if (!f.exists) {
            put(out, 0, 0);
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return 0;
        }
        const auto [n, truncated] = put_list(out, size, list_units(f.doc.section_names()));
        set_last_error(truncated ? error_codes::ERROR_MORE_DATA : error_codes::ERROR_SUCCESS);
        return n;
    }

    /// Load, edit and store @p file. Returns BOOL; success keeps the last
    /// error unchanged (writeA.*.err=0xdeadbeef).
    template <typename Edit>
    u32 edit(const std::u16string& file, Edit&& apply) const {
        std::lock_guard lock(profile_mutex());
        Loaded f = load(m_vfs, file);
        apply(f.doc);
        if (const u32 error = store(m_vfs, file, f.doc); error != 0) {
            set_last_error(error);
            return 0;
        }
        return 1;
    }

    // ---- WritePrivateProfileString ----
    u32 write_string(GuestAddr sec_addr, GuestAddr key_addr, GuestAddr str_addr,
                     const std::optional<std::u16string>& file) const {
        // All NULL flushes the profile cache; Vitreo writes through, so there
        // is nothing left to flush (writeA.flush_all_null).
        if (sec_addr == 0 && key_addr == 0 && str_addr == 0 && !file) return 1;
        const auto section = text(sec_addr);
        if (!section) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return 0;
        }
        if (!file || file->empty()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        return write_entry(*section, text(key_addr), text(str_addr), *file);
    }

    u32 write_entry(const std::u16string& section, const std::optional<std::u16string>& key,
                    const std::optional<std::u16string>& value,
                    const std::u16string& file) const {
        return edit(file, [&](Document& doc) {
            if (!key) doc.delete_section(section);
            else if (!value) doc.delete_key(section, *key);
            else doc.set_value(section, *key, *value);
        });
    }

    // ---- WritePrivateProfileSection ----
    u32 write_section(GuestAddr sec_addr, GuestAddr data_addr,
                      const std::optional<std::u16string>& file) const {
        const auto section = text(sec_addr);
        if (!section) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return 0;
        }
        if (!file || file->empty()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        if (data_addr == 0) {
            return edit(*file, [&](Document& doc) { doc.delete_section(*section); });
        }
        const auto lines = multi_text(data_addr);
        return edit(*file, [&](Document& doc) { doc.replace_section(*section, lines); });
    }

    // ---- GetPrivateProfileStruct ----
    u32 get_struct(GuestAddr sec_addr, GuestAddr key_addr, GuestAddr out, u32 size,
                   const std::optional<std::u16string>& file) const {
        const auto section = text(sec_addr);
        const auto key = text(key_addr);
        if (!section || !key) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        std::optional<std::u16string> hex;
        {
            std::lock_guard lock(profile_mutex());
            const Loaded f = load(m_vfs, file);
            if (f.exists) hex = f.doc.value(*section, *key);
        }
        // Absent, odd or wrong length: ERROR_BAD_LENGTH, buffer untouched.
        if (!hex || hex->size() != 2 * (static_cast<u64>(size) + 1)) {
            set_last_error(error_codes::ERROR_BAD_LENGTH);
            return 0;
        }
        if (out == 0 && size != 0) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        // Each digit is c - '0', or toupper(c) - 'A' + 10 above '9', and the
        // pair is (hi << 4) | lo in a byte: measured "ZZ" -> 0x33
        // (structA.read.non_hex). Non-hex text is caught only by the
        // checksum, after the buffer is filled.
        auto nibble = [](char16_t c) -> u32 {
            if (c <= u'9') return static_cast<u32>(c) - u'0';
            const char16_t upper = (c >= u'a' && c <= u'z') ? c - (u'a' - u'A') : c;
            return static_cast<u32>(upper) - u'A' + 10;
        };
        auto byte_at = [&](usize i) -> u8 {
            return static_cast<u8>((nibble((*hex)[2 * i]) << 4) | nibble((*hex)[2 * i + 1]));
        };
        u32 sum = 0;
        for (u32 i = 0; i < size; ++i) {
            const u8 b = byte_at(i);
            sum += b;
            (void)m_memory.write_u8(out + i, b);
        }
        if (static_cast<u8>(sum) != byte_at(size)) {
            set_last_error(error_codes::ERROR_INVALID_DATA);
            return 0;
        }
        set_last_error(error_codes::ERROR_SUCCESS);
        return 1;
    }

    // ---- WritePrivateProfileStruct ----
    u32 write_struct(GuestAddr sec_addr, GuestAddr key_addr, GuestAddr data, u32 size,
                     const std::optional<std::u16string>& file) const {
        const auto section = text(sec_addr);
        if (!section) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return 0;
        }
        if (!file || file->empty()) {
            set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            return 0;
        }
        const auto key = text(key_addr);
        if (data == 0) return write_entry(*section, key, std::nullopt, *file);
        // Uppercase hex of every byte, then the low byte of their sum
        // (structA.write: 01 02 03 AB FF -> "010203ABFFB0").
        static constexpr char16_t kHex[] = u"0123456789ABCDEF";
        std::u16string hex;
        hex.reserve(2 * (static_cast<usize>(size) + 1));
        u32 sum = 0;
        for (u32 i = 0; i < size; ++i) {
            auto r = m_memory.read_u8(data + i);
            if (r.is_err()) {
                set_last_error(error_codes::ERROR_NOACCESS);
                return 0;
            }
            sum += r.value();
            hex.push_back(kHex[r.value() >> 4]);
            hex.push_back(kHex[r.value() & 0xF]);
        }
        hex.push_back(kHex[(sum >> 4) & 0xF]);
        hex.push_back(kHex[sum & 0xF]);
        return write_entry(*section, key, hex, *file);
    }

private:
    [[nodiscard]] std::string ansi_bytes(GuestAddr addr) const {
        std::string s;
        for (usize i = 0; i < kMaxGuestString; ++i) {
            auto r = m_memory.read_u8(addr + i);
            if (r.is_err() || r.value() == 0) break;
            s.push_back(static_cast<char>(r.value()));
        }
        return s;
    }

    GuestMemory& m_memory;
    capsule::Vfs& m_vfs;
    bool m_wide;
};

/// The win.ini forms: Learn names win.ini in the Windows directory (a bare
/// name, resolved there by profile_path).
const std::optional<std::u16string> kWinIni{u"win.ini"};

} // namespace

// =============================================================================
// GetPrivateProfileString / GetProfileString
// =============================================================================

// DWORD GetPrivateProfileString(LPCTSTR lpAppName, LPCTSTR lpKeyName,
//   LPCTSTR lpDefault, LPTSTR lpReturnedString, DWORD nSize, LPCTSTR lpFileName)
void Kernel32ExtManager::impl_GetPrivateProfileStringA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3),
                              m.arg32(4), c.text(m.arg_ptr(5))));
}
void Kernel32ExtManager::impl_GetPrivateProfileStringW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3),
                              m.arg32(4), c.text(m.arg_ptr(5))));
}
// DWORD GetProfileString(LPCTSTR, LPCTSTR, LPCTSTR, LPTSTR, DWORD)
void Kernel32ExtManager::impl_GetProfileStringA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3),
                              m.arg32(4), kWinIni));
}
void Kernel32ExtManager::impl_GetProfileStringW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg_ptr(3),
                              m.arg32(4), kWinIni));
}

// =============================================================================
// GetPrivateProfileInt / GetProfileInt
// =============================================================================

// UINT GetPrivateProfileInt(LPCTSTR lpAppName, LPCTSTR lpKeyName, INT nDefault,
//   LPCTSTR lpFileName)
void Kernel32ExtManager::impl_GetPrivateProfileIntA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_int(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), c.text(m.arg_ptr(3))));
}
void Kernel32ExtManager::impl_GetPrivateProfileIntW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_int(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), c.text(m.arg_ptr(3))));
}
// UINT GetProfileInt(LPCTSTR lpAppName, LPCTSTR lpKeyName, INT nDefault)
void Kernel32ExtManager::impl_GetProfileIntA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_int(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), kWinIni));
}
void Kernel32ExtManager::impl_GetProfileIntW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_int(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), kWinIni));
}

// =============================================================================
// GetPrivateProfileSection / GetProfileSection / GetPrivateProfileSectionNames
// =============================================================================

// DWORD GetPrivateProfileSection(LPCTSTR lpAppName, LPTSTR lpReturnedString,
//   DWORD nSize, LPCTSTR lpFileName)
void Kernel32ExtManager::impl_GetPrivateProfileSectionA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_section(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), c.text(m.arg_ptr(3))));
}
void Kernel32ExtManager::impl_GetPrivateProfileSectionW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_section(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), c.text(m.arg_ptr(3))));
}
// DWORD GetProfileSection(LPCTSTR lpAppName, LPTSTR lpReturnedString, DWORD nSize)
void Kernel32ExtManager::impl_GetProfileSectionA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_section(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), kWinIni));
}
void Kernel32ExtManager::impl_GetProfileSectionW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_section(m.arg_ptr(0), m.arg_ptr(1), m.arg32(2), kWinIni));
}
// DWORD GetPrivateProfileSectionNames(LPTSTR lpszReturnBuffer, DWORD nSize,
//   LPCTSTR lpFileName)
void Kernel32ExtManager::impl_GetPrivateProfileSectionNamesA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_section_names(m.arg_ptr(0), m.arg32(1), c.text(m.arg_ptr(2))));
}
void Kernel32ExtManager::impl_GetPrivateProfileSectionNamesW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_section_names(m.arg_ptr(0), m.arg32(1), c.text(m.arg_ptr(2))));
}

// =============================================================================
// WritePrivateProfileString / WriteProfileString
// =============================================================================

// BOOL WritePrivateProfileString(LPCTSTR lpAppName, LPCTSTR lpKeyName,
//   LPCTSTR lpString, LPCTSTR lpFileName)
void Kernel32ExtManager::impl_WritePrivateProfileStringA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.write_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), c.text(m.arg_ptr(3))));
}
void Kernel32ExtManager::impl_WritePrivateProfileStringW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.write_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), c.text(m.arg_ptr(3))));
}
// BOOL WriteProfileString(LPCTSTR lpAppName, LPCTSTR lpKeyName, LPCTSTR lpString)
// Learn: all three NULL flushes the win.ini cache.
void Kernel32ExtManager::impl_WriteProfileStringA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    const bool flush = m.arg_ptr(0) == 0 && m.arg_ptr(1) == 0 && m.arg_ptr(2) == 0;
    m.set_return(flush ? 1u : c.write_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), kWinIni));
}
void Kernel32ExtManager::impl_WriteProfileStringW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    const bool flush = m.arg_ptr(0) == 0 && m.arg_ptr(1) == 0 && m.arg_ptr(2) == 0;
    m.set_return(flush ? 1u : c.write_string(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), kWinIni));
}

// =============================================================================
// WritePrivateProfileSection / WriteProfileSection
// =============================================================================

// BOOL WritePrivateProfileSection(LPCTSTR lpAppName, LPCTSTR lpString,
//   LPCTSTR lpFileName)
void Kernel32ExtManager::impl_WritePrivateProfileSectionA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.write_section(m.arg_ptr(0), m.arg_ptr(1), c.text(m.arg_ptr(2))));
}
void Kernel32ExtManager::impl_WritePrivateProfileSectionW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.write_section(m.arg_ptr(0), m.arg_ptr(1), c.text(m.arg_ptr(2))));
}
// BOOL WriteProfileSection(LPCTSTR lpAppName, LPCTSTR lpString)
void Kernel32ExtManager::impl_WriteProfileSectionA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.write_section(m.arg_ptr(0), m.arg_ptr(1), kWinIni));
}
void Kernel32ExtManager::impl_WriteProfileSectionW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.write_section(m.arg_ptr(0), m.arg_ptr(1), kWinIni));
}

// =============================================================================
// GetPrivateProfileStruct / WritePrivateProfileStruct
// =============================================================================

// BOOL GetPrivateProfileStruct(LPCTSTR lpszSection, LPCTSTR lpszKey,
//   LPVOID lpStruct, UINT uSizeStruct, LPCTSTR szFile)
void Kernel32ExtManager::impl_GetPrivateProfileStructA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.get_struct(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg32(3),
                              c.text(m.arg_ptr(4))));
}
void Kernel32ExtManager::impl_GetPrivateProfileStructW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.get_struct(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg32(3),
                              c.text(m.arg_ptr(4))));
}
// BOOL WritePrivateProfileStruct(LPCTSTR lpszSection, LPCTSTR lpszKey,
//   LPVOID lpStruct, UINT uSizeStruct, LPCTSTR szFile)
void Kernel32ExtManager::impl_WritePrivateProfileStructA(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, false);
    m.set_return(c.write_struct(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg32(3),
                                c.text(m.arg_ptr(4))));
}
void Kernel32ExtManager::impl_WritePrivateProfileStructW(Marshaler& m) {
    ProfileCall c(m_memory, m_vfs, true);
    m.set_return(c.write_struct(m.arg_ptr(0), m.arg_ptr(1), m.arg_ptr(2), m.arg32(3),
                                c.text(m.arg_ptr(4))));
}

// =============================================================================
// Math
// =============================================================================

void Kernel32ExtManager::impl_MulDiv(Marshaler& m) {
    // int MulDiv(int nNumber, int nNumerator, int nDenominator)
    i32 number      = static_cast<i32>(m.arg(0));
    i32 numerator   = static_cast<i32>(m.arg(1));
    i32 denominator = static_cast<i32>(m.arg(2));

    if (denominator == 0) {
        m.set_return(static_cast<u64>(static_cast<u32>(static_cast<i32>(-1))));
        return;
    }

    // CV-11: Windows MulDiv rounds to nearest: (a * b + denom/2) / denom
    i64 product = static_cast<i64>(number) * static_cast<i64>(numerator);
    // Add half the denominator for rounding, matching the sign of the product.
    if (product >= 0) {
        product += static_cast<i64>(denominator) / 2;
    } else {
        product -= static_cast<i64>(denominator) / 2;
    }
    i64 result = product / static_cast<i64>(denominator);

    m.set_return(static_cast<u64>(static_cast<u32>(static_cast<i32>(result))));
}

} // namespace vitreo::winapi

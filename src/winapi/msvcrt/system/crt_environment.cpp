// SPDX-License-Identifier: LGPL-2.1-only
/// @file crt_environment.cpp
/// @brief Guest-visible CRT environment access

#include "winapi/msvcrt/system/crt_environment.hpp"
#include "capsule/vfs/vfs.hpp"

#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace vitreo::winapi {

namespace {

constexpr u32 EINVAL_VALUE = 22;
constexpr u32 ERANGE_VALUE = 34;

std::string cache_key(std::string_view name) {
    return uppercase_key(name);
}

void return_invalid_parameter(Marshaler& m, CrtEnvironmentManager& mgr,
                              GuestAddr required_size) {
    if (required_size != 0) (void)m.write_ptr(required_size, 0);
    mgr.set_errno(EINVAL_VALUE);
    m.set_return(EINVAL_VALUE);
}

} // namespace

CrtEnvironmentManager::CrtEnvironmentManager(
    EnvironmentManager& environment,
    GuestMemory& memory,
    CrtMemoryManager& memory_manager)
    : m_environment(environment),
      m_memory(memory),
      m_memory_manager(memory_manager) {}

std::optional<std::string> CrtEnvironmentManager::value(
    std::string_view name) const {
    auto result = m_environment.get_variable(name);
    if (result.is_err()) return std::nullopt;
    return result.value();
}

std::vector<u8> CrtEnvironmentManager::ansi_value_bytes(
    std::string_view value_string) {
    // getenv returns ACP bytes (Learn); values are stored UTF-8 internally.
    const std::vector<u16> wide = utf8_to_utf16(value_string);
    std::vector<u8> acp;
    if (!wide.empty()) {
        acp.resize(wide.size() * 4 + 4);
        auto nr = codepage::wide_to_multi_byte(
            codepage::kCpAcp, 0, wide.data(), wide.size(), acp.data(),
            acp.size(), nullptr, false);
        if (nr.ok() && nr.count > 0) {
            acp.resize(static_cast<usize>(nr.count));
        } else {
            // Unmappable / convert failure: fall back to raw UTF-8 bytes so
            // the value is not lost (ASCII paths stay identical).
            acp.assign(value_string.begin(), value_string.end());
        }
    }
    return acp;
}

GuestAddr CrtEnvironmentManager::cache_ansi(std::string_view name,
                                            std::string_view value_string) {
    const std::vector<u8> acp = ansi_value_bytes(value_string);

    std::lock_guard lock(m_lock);
    auto& entry = m_ansi_cache[cache_key(name)];
    if (entry.address != 0 && entry.value == value_string) {
        return entry.address;
    }

    const GuestAddr address =
        m_memory_manager.crt_malloc(acp.size() + 1);
    if (address == 0 ||
        (!acp.empty() &&
         m_memory.write_bytes(address, acp.data(), acp.size()).is_err()) ||
        m_memory.write_u8(address + acp.size(), 0).is_err()) {
        return 0;
    }

    entry.value = value_string;
    entry.address = address;
    return address;
}

GuestAddr CrtEnvironmentManager::cache_wide(std::string_view name,
                                            std::string_view value_string) {
    const std::vector<u16> wide = utf8_to_utf16(value_string);
    std::lock_guard lock(m_lock);
    auto& entry = m_wide_cache[cache_key(name)];
    if (entry.address != 0 && entry.value == value_string) {
        return entry.address;
    }

    const usize byte_count = (wide.size() + 1) * sizeof(u16);
    const GuestAddr address = m_memory_manager.crt_malloc(byte_count);
    if (address == 0 ||
        (!wide.empty() &&
         m_memory.write_bytes(
             address, reinterpret_cast<const Byte*>(wide.data()),
             wide.size() * sizeof(u16)).is_err()) ||
        m_memory.write_u16(address + wide.size() * sizeof(u16), 0).is_err()) {
        return 0;
    }

    entry.value = value_string;
    entry.address = address;
    return address;
}

GuestAddr CrtEnvironmentManager::getenv_ansi(std::string_view name) {
    auto result = value(name);
    return result ? cache_ansi(name, *result) : 0;
}

GuestAddr CrtEnvironmentManager::getenv_wide(std::string_view name) {
    auto result = value(name);
    return result ? cache_wide(name, *result) : 0;
}

void CrtEnvironmentManager::set_errno(u32 value) {
    (void)m_memory.write_u32(m_memory_manager.errno_addr(), value);
}

bool CrtEnvironmentManager::put_variable(std::string_view name,
                                         std::string_view value_string,
                                         bool remove) {
    if (name.empty()) return false;
    if (remove) {
        m_environment.delete_variable(name);
    } else {
        m_environment.set_variable(name, value_string);
    }
    bool refresh_wide = false;
    bool wide_is_64bit = false;
    {
        std::lock_guard lock(m_lock);
        refresh_wide = m_wenviron_built;
        wide_is_64bit = m_wenviron_is_64bit;
    }
    if (refresh_wide && wenviron_variable_addr(wide_is_64bit, true) == 0)
        return false;
    return true;
}

namespace {
/// Write a NULL-terminated pointer table at `array`, pointer-width per slot.
void write_pointer_table(GuestMemory& memory, GuestAddr array,
                         const std::vector<GuestAddr>& entries,
                         usize ptr_size) {
    for (usize i = 0; i < entries.size(); ++i) {
        const GuestAddr slot = array + i * ptr_size;
        if (ptr_size == 8) {
            (void)memory.write_u64(slot, entries[i]);
        } else {
            (void)memory.write_u32(slot, static_cast<u32>(entries[i]));
        }
    }
    const GuestAddr terminator = array + entries.size() * ptr_size;
    if (ptr_size == 8) {
        (void)memory.write_u64(terminator, 0);
    } else {
        (void)memory.write_u32(terminator, 0);
    }
}
} // namespace

GuestAddr CrtEnvironmentManager::build_wide_string_table(
    const std::vector<std::wstring>& args, bool is_64bit) {
    const usize ptr_size = is_64bit ? 8u : 4u;
    std::lock_guard lock(m_lock);

    std::vector<GuestAddr> entry_addrs;
    entry_addrs.reserve(args.size());
    for (const auto& arg : args) {
        // UTF-16LE, null terminated. std::wstring is UTF-32 on this host, so
        // the code units have to be narrowed rather than copied wholesale;
        // memcpy'ing wchar_t would write 4 bytes per character and every
        // guest-side read would see the string terminate after one char.
        const usize bytes = (arg.size() + 1) * 2;
        const GuestAddr addr = m_memory_manager.crt_malloc(bytes);
        if (addr == 0) return 0;
        for (usize i = 0; i < arg.size(); ++i) {
            (void)m_memory.write_u16(addr + i * 2,
                                     static_cast<u16>(arg[i] & 0xFFFF));
        }
        (void)m_memory.write_u16(addr + arg.size() * 2, 0);
        entry_addrs.push_back(addr);
    }

    const GuestAddr array =
        m_memory_manager.crt_malloc((entry_addrs.size() + 1) * ptr_size);
    if (array == 0) return 0;
    write_pointer_table(m_memory, array, entry_addrs, ptr_size);
    return array;
}

GuestAddr CrtEnvironmentManager::build_narrow_string_table(
    const std::vector<std::string>& args, bool is_64bit) {
    const usize ptr_size = is_64bit ? 8u : 4u;
    std::lock_guard lock(m_lock);
    std::vector<GuestAddr> entry_addrs;
    entry_addrs.reserve(args.size());
    for (const auto& arg : args) {
        const GuestAddr addr = m_memory_manager.crt_malloc(arg.size() + 1);
        if (addr == 0) return 0;
        if (!arg.empty()) {
            (void)m_memory.write_bytes(
                addr, reinterpret_cast<const Byte*>(arg.data()), arg.size());
        }
        (void)m_memory.write_u8(addr + arg.size(), 0);
        entry_addrs.push_back(addr);
    }
    const GuestAddr array =
        m_memory_manager.crt_malloc((entry_addrs.size() + 1) * ptr_size);
    if (array == 0) return 0;
    write_pointer_table(m_memory, array, entry_addrs, ptr_size);
    return array;
}

GuestAddr CrtEnvironmentManager::wenviron_variable_addr(bool is_64bit,
                                                       bool initialize) {
    std::lock_guard lock(m_lock);
    if (m_wenviron_variable == 0) {
        m_wenviron_variable = m_memory_manager.crt_malloc(sizeof(u64));
        if (m_wenviron_variable == 0) return 0;
        (void)m_memory.write_u64(m_wenviron_variable, 0);
    }
    // The CRT DLL starts with a null wide environment. _wgetenv or explicit
    // wide startup initializes it; the accessor itself only exposes the cell.
    if (!initialize && !m_wenviron_built) return m_wenviron_variable;
    auto variables = m_environment.get_all_variables_as_named();
    if (m_wenviron_built && m_wenviron_is_64bit == is_64bit &&
        variables == m_wenviron_snapshot) return m_wenviron_variable;
    std::vector<GuestAddr> entries;
    for (const auto& [name, value] : variables) {
        const auto wide = utf8_to_utf16(name + "=" + value);
        const GuestAddr entry = m_memory_manager.crt_malloc((wide.size() + 1) * 2);
        if (entry == 0) return 0;
        if (m_memory.write_bytes(entry, reinterpret_cast<const Byte*>(wide.data()),
                                 wide.size() * 2).is_err() ||
            m_memory.write_u16(entry + wide.size() * 2, 0).is_err()) return 0;
        entries.push_back(entry);
    }
    const usize width = is_64bit ? 8 : 4;
    const GuestAddr array = m_memory_manager.crt_malloc((entries.size() + 1) * width);
    if (array == 0) return 0;
    write_pointer_table(m_memory, array, entries, width);
    if (is_64bit) (void)m_memory.write_u64(m_wenviron_variable, array);
    else (void)m_memory.write_u32(m_wenviron_variable, static_cast<u32>(array));
    m_wenviron_snapshot = std::move(variables);
    m_wenviron_is_64bit = is_64bit;
    m_wenviron_built = true;
    return m_wenviron_variable;
}

GuestAddr CrtEnvironmentManager::wenviron_array_addr(bool is_64bit) {
    const GuestAddr cell = wenviron_variable_addr(is_64bit, true);
    if (cell == 0) return 0;
    return is_64bit ? m_memory.read_u64(cell).value_or(0)
                    : m_memory.read_u32(cell).value_or(0);
}

GuestAddr CrtEnvironmentManager::environ_array_addr(bool is_64bit) {
    const GuestAddr var = environ_variable_addr(is_64bit);
    if (var == 0) return 0;
    if (is_64bit) {
        const auto v = m_memory.read_u64(var);
        return v.is_ok() ? static_cast<GuestAddr>(v.value()) : 0;
    }
    const auto v = m_memory.read_u32(var);
    return v.is_ok() ? static_cast<GuestAddr>(v.value()) : 0;
}

GuestAddr CrtEnvironmentManager::environ_variable_addr(bool is_64bit) {
    std::lock_guard lock(m_lock);

    if (m_environ_variable == 0) {
        // One pointer-sized cell holding the array base. It must outlive every
        // rebuild because the guest keeps the address of the variable itself.
        m_environ_variable = m_memory_manager.crt_malloc(sizeof(u64));
        if (m_environ_variable == 0) return 0;
        (void)m_memory.write_u64(m_environ_variable, 0);
    }

    auto variables = m_environment.get_all_variables_as_named();
    if (m_environ_built && variables == m_environ_snapshot) {
        return m_environ_variable;
    }

    // Build "NAME=VALUE" strings, then a NULL-terminated array of pointers to
    // them. Windows leaks the previous block on rebuild as well; the CRT arena
    // never reuses allocations, so guests holding an old array still read
    // valid strings.
    const usize ptr_size = is_64bit ? 8u : 4u;

    std::vector<GuestAddr> entry_addrs;
    entry_addrs.reserve(variables.size());
    for (const auto& [key, value_string] : variables) {
        std::string entry = key;
        entry.push_back('=');
        entry.append(value_string);
        const GuestAddr addr = m_memory_manager.crt_malloc(entry.size() + 1);
        if (addr == 0) return m_environ_variable;
        if (m_memory.write_bytes(addr,
                                 reinterpret_cast<const Byte*>(entry.data()),
                                 entry.size()).is_err() ||
            m_memory.write_u8(addr + entry.size(), 0).is_err()) {
            return m_environ_variable;
        }
        entry_addrs.push_back(addr);
    }

    const GuestAddr array =
        m_memory_manager.crt_malloc((entry_addrs.size() + 1) * ptr_size);
    if (array == 0) return m_environ_variable;
    for (usize i = 0; i < entry_addrs.size(); ++i) {
        const GuestAddr slot = array + i * ptr_size;
        const bool ok = is_64bit
            ? m_memory.write_u64(slot, entry_addrs[i]).is_ok()
            : m_memory.write_u32(slot,
                                 static_cast<u32>(entry_addrs[i])).is_ok();
        if (!ok) return m_environ_variable;
    }
    const GuestAddr terminator = array + entry_addrs.size() * ptr_size;
    if (is_64bit) {
        (void)m_memory.write_u64(terminator, 0);
        (void)m_memory.write_u64(m_environ_variable, array);
    } else {
        (void)m_memory.write_u32(terminator, 0);
        (void)m_memory.write_u32(m_environ_variable, static_cast<u32>(array));
    }

    m_environ_snapshot = std::move(variables);
    m_environ_built = true;
    return m_environ_variable;
}

DllExports create_crt_environment_thunks(CrtEnvironmentManager& mgr,
                                         capsule::Vfs& vfs) {
    DllExports exports;
    exports.entries.reserve(4);

    exports.entries.push_back({"getenv", 0, [&mgr](Marshaler& m) {
        const GuestAddr name_ptr = m.arg_ptr(0);
        if (name_ptr == 0) {
            mgr.set_errno(22); // EINVAL
            m.set_return(0);
            return;
        }
        auto name = m.read_ansi_string(name_ptr);
        m.set_return(name.is_ok() ? mgr.getenv_ansi(name.value()) : 0);
    }});

    exports.entries.push_back({"_wgetenv", 0, [&mgr](Marshaler& m) {
        const GuestAddr name_ptr = m.arg_ptr(0);
        if (name_ptr == 0) {
            mgr.set_errno(22); // EINVAL
            m.set_return(0);
            return;
        }
        auto name = m.read_wide_string(name_ptr);
        if (name.is_ok()) (void)mgr.wenviron_array_addr(m.cpu_is_64bit());
        m.set_return(name.is_ok() ? mgr.getenv_wide(name.value()) : 0);
    }});

    exports.entries.push_back({"getenv_s", 0, [&mgr](Marshaler& m) {
        const GuestAddr required_size = m.arg_ptr(0);
        const GuestAddr buffer = m.arg_ptr(1);
        const usize buffer_count = static_cast<usize>(m.arg(2));
        const GuestAddr name_ptr = m.arg_ptr(3);
        if (required_size == 0 || (buffer == 0 && buffer_count != 0) ||
            name_ptr == 0) {
            return_invalid_parameter(m, mgr, required_size);
            return;
        }

        auto name = m.read_ansi_string(name_ptr);
        if (name.is_err()) {
            return_invalid_parameter(m, mgr, required_size);
            return;
        }
        auto result = mgr.value(name.value());
        if (!result) {
            (void)m.write_ptr(required_size, 0);
            if (buffer != 0 && buffer_count != 0) (void)m.write_u8(buffer, 0);
            m.set_return(0);
            return;
        }

        const usize needed = result->size() + 1;
        (void)m.write_ptr(required_size, needed);
        if (buffer == 0) {
            m.set_return(0);
            return;
        }
        if (buffer_count < needed) {
            (void)m.write_u8(buffer, 0);
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(ERANGE_VALUE);
            return;
        }
        if (m.write_ansi_string(buffer, *result).is_err()) {
            (void)m.write_u8(buffer, 0);
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        m.set_return(0);
    }});

    exports.entries.push_back({"_wgetenv_s", 0, [&mgr](Marshaler& m) {
        const GuestAddr required_size = m.arg_ptr(0);
        const GuestAddr buffer = m.arg_ptr(1);
        const usize buffer_count = static_cast<usize>(m.arg(2));
        const GuestAddr name_ptr = m.arg_ptr(3);
        if (required_size == 0 || (buffer == 0 && buffer_count != 0) ||
            name_ptr == 0) {
            return_invalid_parameter(m, mgr, required_size);
            return;
        }

        auto name = m.read_wide_string(name_ptr);
        if (name.is_err()) {
            return_invalid_parameter(m, mgr, required_size);
            return;
        }
        auto result = mgr.value(name.value());
        if (!result) {
            (void)m.write_ptr(required_size, 0);
            if (buffer != 0 && buffer_count != 0) (void)m.write_u16(buffer, 0);
            m.set_return(0);
            return;
        }

        const std::vector<u16> wide = utf8_to_utf16(*result);
        const usize needed = wide.size() + 1;
        (void)m.write_ptr(required_size, needed);
        if (buffer == 0) {
            m.set_return(0);
            return;
        }
        if (buffer_count < needed) {
            (void)m.write_u16(buffer, 0);
            mgr.set_errno(ERANGE_VALUE);
            m.set_return(ERANGE_VALUE);
            return;
        }
        if ((!wide.empty() &&
             m.memory().write_bytes(
                 buffer, reinterpret_cast<const Byte*>(wide.data()),
                 wide.size() * sizeof(u16)).is_err()) ||
            m.write_u16(buffer + wide.size() * sizeof(u16), 0).is_err()) {
            (void)m.write_u16(buffer, 0);
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        m.set_return(0);
    }});

    // errno_t _dupenv_s(char** buffer, size_t* numberOfElements,
    //                   const char* varname)
    // errno_t _wdupenv_s(wchar_t** buffer, size_t* numberOfElements,
    //                    const wchar_t* varname)
    //
    // Learn (_dupenv_s, _wdupenv_s):
    //   - buffer == NULL or varname == NULL: invalid parameter handler; if
    //     execution continues, errno = EINVAL and the return is EINVAL.
    //   - found: *buffer = a malloc'd copy of the value the caller releases
    //     with free, *numberOfElements = its size in elements (the value plus
    //     its terminator); numberOfElements may be NULL.
    //   - not found: *buffer = NULL, *numberOfElements = 0, return 0.
    //   - allocation failure: *buffer = NULL, *numberOfElements = 0, ENOMEM.
    //   - case-insensitive lookup in the same environment _environ /
    //     _wenviron expose (the store getenv/_wgetenv read).
    // The outputs are reset as soon as `buffer` has been validated, so every
    // later failure (varname NULL included) leaves NULL / 0 behind -- the
    // documented not-found and ENOMEM states.
    // The copy comes from crt_malloc: the heap the guest's free() releases.
    auto dupenv_s = [&mgr](Marshaler& m, bool wide) {
        constexpr u32 ENOMEM_VALUE = 12;
        CrtMemoryManager& crt = mgr.memory_manager();
        const GuestAddr buffer_out = m.arg_ptr(0);
        const GuestAddr count_out = m.arg_ptr(1);
        const GuestAddr name_ptr = m.arg_ptr(2);
        auto invalid = [&] {
            crt.set_errno(EINVAL_VALUE);
            if (!crt.invalid_parameter()) return;  // fast-failed: no handler
            m.set_return(EINVAL_VALUE);
        };
        if (buffer_out == 0) {
            invalid();
            return;
        }
        (void)m.write_ptr(buffer_out, 0);
        if (count_out != 0) (void)m.write_ptr(count_out, 0);
        if (name_ptr == 0) {
            invalid();
            return;
        }

        auto name = wide ? m.read_wide_string(name_ptr)
                         : m.read_ansi_string(name_ptr);
        if (name.is_err()) {
            invalid();
            return;
        }
        // _wdupenv_s reads _wenviron, which the CRT creates on first wide
        // access -- the same side effect _wgetenv has.
        if (wide) (void)mgr.wenviron_array_addr(m.cpu_is_64bit());
        const auto result = mgr.value(name.value());
        if (!result) {
            m.set_return(0);
            return;
        }

        std::vector<u8> bytes;
        usize elements = 0;
        if (wide) {
            const std::vector<u16> units = utf8_to_utf16(*result);
            elements = units.size() + 1;
            bytes.resize(elements * sizeof(u16), 0);
            if (!units.empty()) {
                std::memcpy(bytes.data(), units.data(),
                            units.size() * sizeof(u16));
            }
        } else {
            bytes = CrtEnvironmentManager::ansi_value_bytes(*result);
            elements = bytes.size() + 1;
            bytes.push_back(0);
        }

        const GuestAddr copy = crt.crt_malloc(bytes.size());
        if (copy == 0) {
            crt.set_errno(ENOMEM_VALUE);
            m.set_return(ENOMEM_VALUE);
            return;
        }
        if (m.memory().write_bytes(copy, bytes.data(), bytes.size()).is_err()) {
            crt.crt_free(copy);
            crt.set_errno(ENOMEM_VALUE);
            m.set_return(ENOMEM_VALUE);
            return;
        }
        (void)m.write_ptr(buffer_out, copy);
        if (count_out != 0) (void)m.write_ptr(count_out, elements);
        m.set_return(0);
    };
    exports.entries.push_back({"_dupenv_s", 0,
        [dupenv_s](Marshaler& m) { dupenv_s(m, false); }});
    exports.entries.push_back({"_wdupenv_s", 0,
        [dupenv_s](Marshaler& m) { dupenv_s(m, true); }});

    // char*** __p__environ(void)
    //
    // Returns the address of the `char** _environ` variable, not the array.
    // CRT startup code dereferences it twice, so handing back the array here
    // would make the guest read a "NAME=VALUE" string as a pointer.
    exports.entries.push_back({"__p__environ", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.environ_variable_addr(m.cpu_is_64bit()));
    }});

    exports.entries.push_back({"__p__wenviron", 0, [&mgr](Marshaler& m) {
        m.set_return(mgr.wenviron_variable_addr(m.cpu_is_64bit()));
    }});

    // int _putenv(const char* "NAME=VALUE")
    //
    // "NAME=" (empty value) removes the variable. A string with no '=' is
    // invalid and Windows reports failure rather than silently ignoring it.
    exports.entries.push_back({"_putenv", 0, [&mgr](Marshaler& m) {
        const GuestAddr entry_ptr = m.arg_ptr(0);
        if (entry_ptr == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        auto entry = m.read_ansi_string(entry_ptr);
        if (entry.is_err()) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const std::string& text = entry.value();
        const usize eq = text.find('=');
        if (eq == std::string::npos || eq == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        const std::string name = text.substr(0, eq);
        const std::string value_string = text.substr(eq + 1);
        const bool ok = mgr.put_variable(name, value_string,
                                         value_string.empty());
        VITREO_TRACE("[crt] _putenv(\"%s=%s\") -> %d\n", name.c_str(),
                     value_string.c_str(), ok ? 0 : -1);
        if (!ok) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(0xFFFFFFFFu);
            return;
        }
        m.set_return(0);
    }});

    // int _putenv_s(const char* name, const char* value)
    exports.entries.push_back({"_putenv_s", 0, [&mgr](Marshaler& m) {
        auto name = m.read_ansi_string(m.arg_ptr(0));
        auto value_string = m.read_ansi_string(m.arg_ptr(1));
        if (name.is_err() || value_string.is_err() || name.value().empty()) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        const bool ok = mgr.put_variable(name.value(), value_string.value(),
                                         value_string.value().empty());
        m.set_return(ok ? 0u : EINVAL_VALUE);
    }});

    // errno_t _wputenv_s(const wchar_t* varname, const wchar_t* value_string)
    //
    // Learn (_putenv_s, _wputenv_s): returns 0 on success, an error code
    // otherwise. The documented error table is varname == NULL -> EINVAL and
    // value_string == NULL -> EINVAL; an empty value_string REMOVES the
    // variable. The environment store is shared with the narrow _putenv_s —
    // the UCRT keeps one table behind _environ/_wenviron, so a variable set
    // through the wide entry must be visible to getenv and vice versa.
    exports.entries.push_back({"_wputenv_s", 0, [&mgr](Marshaler& m) {
        const GuestAddr name_ptr = m.arg_ptr(0);
        const GuestAddr value_ptr = m.arg_ptr(1);
        if (name_ptr == 0 || value_ptr == 0) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        auto name = m.read_wide_string(name_ptr);
        auto value_string = m.read_wide_string(value_ptr);
        if (name.is_err() || value_string.is_err() || name.value().empty()) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        const bool ok = mgr.put_variable(name.value(), value_string.value(),
                                         value_string.value().empty());
        VITREO_TRACE("[crt] _wputenv_s(\"%s\", \"%s\") -> %d\n",
                     name.value().c_str(), value_string.value().c_str(),
                     ok ? 0 : EINVAL_VALUE);
        if (!ok) {
            mgr.set_errno(EINVAL_VALUE);
            m.set_return(EINVAL_VALUE);
            return;
        }
        m.set_return(0);
    }});

    // void _wsearchenv(const wchar_t* filename, const wchar_t* varname,
    //                  wchar_t* pathname)
    //
    // Search the directories listed in the named environment variable (PATH-
    // style, semicolon-separated) for filename. On success write the full
    // path into pathname; on failure write an empty string. Without a VFS
    // in this factory, existence cannot be verified — leave empty unless
    // filename is already an absolute/relative path with a separator.
    exports.entries.push_back({"_wsearchenv", 0, [&mgr, &vfs](Marshaler& m) {
        const GuestAddr file_ptr = m.arg_ptr(0);
        const GuestAddr var_ptr = m.arg_ptr(1);
        const GuestAddr buf = m.arg_ptr(2);
        if (buf == 0) return;
        (void)m.write_u16(buf, 0);
        if (file_ptr == 0 || var_ptr == 0) return;

        auto filename = m.read_wide_string(file_ptr);
        auto varname = m.read_wide_string(var_ptr);
        if (filename.is_err() || varname.is_err() ||
            filename.value().empty() || varname.value().empty()) {
            return;
        }

        // Confirm the env var exists so callers that pass a bogus name still
        // see the empty-buffer failure mode.
        if (!mgr.value(varname.value())) return;

        const std::string& file = filename.value();
        if (file.find('\\') != std::string::npos ||
            file.find('/') != std::string::npos ||
            (file.size() >= 2 && file[1] == ':')) {
            // Already a path: reported as-is, no search.
            (void)m.write_wide_string(buf, file);
            return;
        }

        // A bare name is searched along the variable's semicolon-separated
        // directory list, first hit wins -- previously skipped entirely, so
        // _wsearchenv("cmd.exe", "PATH", ...) came back empty where Windows
        // finds it.
        const auto list = mgr.value(varname.value());
        if (!list.has_value()) return;
        const std::string dirs = *list;
        usize begin = 0;
        while (begin <= dirs.size()) {
            usize sep = dirs.find(';', begin);
            if (sep == std::string::npos) sep = dirs.size();
            std::string dir = dirs.substr(begin, sep - begin);
            begin = sep + 1;
            if (dir.empty()) continue;
            if (dir.back() != '\\' && dir.back() != '/') dir += '\\';
            const std::string candidate = dir + file;
            if (vfs.stat(candidate).is_ok()) {
                (void)m.write_wide_string(buf, candidate);
                return;
            }
        }
        // Not found anywhere: the buffer stays empty, as Windows leaves it.
    }});

    // intptr_t _wspawnv(int mode, const wchar_t* path, const wchar_t* const* argv)
    // Child process creation is not available from the CRT thunk path.
    exports.entries.push_back({"_wspawnv", 0, [&mgr](Marshaler& m) {
        (void)m.arg(0);
        (void)m.arg_ptr(1);
        (void)m.arg_ptr(2);
        mgr.set_errno(EINVAL_VALUE);
        m.set_return(static_cast<u64>(static_cast<i64>(-1)));
    }});

    return exports;
}

} // namespace vitreo::winapi

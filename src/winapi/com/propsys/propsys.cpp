// SPDX-License-Identifier: LGPL-2.1-only

#include "propsys.hpp"

#include "winapi/com/ole32/com_core.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u32 S_OK = 0x00000000u;
constexpr u32 E_INVALIDARG = 0x80070057u;
constexpr u32 E_OUTOFMEMORY = 0x8007000Eu;
constexpr u32 DISP_E_TYPEMISMATCH = 0x80020005u;
constexpr u32 STRSAFE_E_INSUFFICIENT_BUFFER = 0x8007007Au;
constexpr u32 TYPE_E_ELEMENTNOTFOUND = 0x8002802Bu;
constexpr u32 CO_E_NOTINITIALIZED = 0x800401F0u;

struct PropkeyName {
    const char* name;
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
    u32 pid;
    u16 vt;
    bool registered;
    bool tree;
    bool enumerated;
};

#include "propkey_names.inc"

PropertyKey key_of(const PropkeyName& e) {
    PropertyKey k;
    k.data1 = e.data1;
    k.data2 = e.data2;
    k.data3 = e.data3;
    std::copy(std::begin(e.data4), std::end(e.data4), k.data4.begin());
    k.pid = e.pid;
    return k;
}

/// The name as ASCII, or nullopt when it holds anything else (no canonical
/// or legacy name does).
std::optional<std::string> ascii_name(std::u16string_view name) {
    std::string out;
    out.reserve(name.size());
    for (char16_t c : name) {
        if (c == 0 || c > 0x7F) return std::nullopt;
        out.push_back(static_cast<char>(c));
    }
    return out;
}

char ascii_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

int hex_digit(char16_t c) {
    if (c >= u'0' && c <= u'9') return c - u'0';
    if (c >= u'a' && c <= u'f') return c - u'a' + 10;
    if (c >= u'A' && c <= u'F') return c - u'A' + 10;
    return -1;
}

/// PSPropertyKeyFromString's grammar as measured on winoracle
/// (propsys_name_lines{64,32}.expected, the "P" and "{"-prefixed "N" rows):
/// "{" 8-4-4-4-12 hex digits (either case) "}", then spaces, an end of
/// string here is E_INVALIDARG; then at most one ',' or '-' separator and
/// more spaces, an end of string here is E_INVALIDARG too; then the pid: an
/// optional '-' and decimal digits up to the first non-digit, accumulated
/// modulo 2^32 and negated modulo 2^32 after a '-' (no digits is pid 0;
/// '+', tabs and other characters are not skipped).
std::optional<PropertyKey> parse_key_string(std::u16string_view s) {
    usize i = 0;
    auto at = [&](usize k) -> char16_t { return k < s.size() ? s[k] : u'\0'; };
    if (at(i++) != u'{') return std::nullopt;
    static constexpr usize kGroups[5] = {8, 4, 4, 4, 12};
    u64 values[5] = {};
    std::array<u8, 6> node{};
    for (usize g = 0; g < 5; ++g) {
        if (g > 0 && at(i++) != u'-') return std::nullopt;
        for (usize d = 0; d < kGroups[g]; ++d) {
            const int h = hex_digit(at(i++));
            if (h < 0) return std::nullopt;
            values[g] = (values[g] << 4) | static_cast<u64>(h);
        }
    }
    if (at(i++) != u'}') return std::nullopt;
    for (usize b = 0; b < node.size(); ++b) node[b] = static_cast<u8>(values[4] >> (40 - 8 * b));
    while (at(i) == u' ') ++i;
    if (i >= s.size()) return std::nullopt;
    if (at(i) == u',' || at(i) == u'-') {
        ++i;
        while (at(i) == u' ') ++i;
        if (i >= s.size()) return std::nullopt;
    }
    bool negative = false;
    if (at(i) == u'-') {
        negative = true;
        ++i;
    }
    u32 pid = 0;
    while (at(i) >= u'0' && at(i) <= u'9') {
        pid = pid * 10u + static_cast<u32>(at(i) - u'0');
        ++i;
    }
    if (negative) pid = 0u - pid;
    PropertyKey k;
    k.data1 = static_cast<u32>(values[0]);
    k.data2 = static_cast<u16>(values[1]);
    k.data3 = static_cast<u16>(values[2]);
    k.data4[0] = static_cast<u8>(values[3] >> 8);
    k.data4[1] = static_cast<u8>(values[3]);
    for (usize b = 0; b < node.size(); ++b) k.data4[2 + b] = node[b];
    k.pid = pid;
    return k;
}

/// Longest "{fmtid} pid" name PSGetPropertyKeyFromName accepts: 50
/// characters pass, 51 return STRSAFE_E_INSUFFICIENT_BUFFER (measured).
constexpr usize kMaxKeyStringName = 50;

const PropkeyName* find_by_key(const PropertyKey& key) {
    for (const auto& e : kPropkeyCanonicalNames) {
        if (key_of(e) == key) return &e;
    }
    return nullptr;
}

/// Reads a NUL-terminated UTF-16 string; on an unreadable unit, the address
/// that faulted.
struct GuestString {
    std::u16string text;
    std::optional<GuestAddr> fault;
};
GuestString read_guest_utf16(const Marshaler& m, GuestAddr source) {
    GuestString out;
    for (GuestAddr a = source;; a += sizeof(u16)) {
        auto unit = m.read_u16(a);
        if (unit.is_err()) {
            out.fault = a;
            return out;
        }
        if (unit.value() == 0) return out;
        out.text.push_back(static_cast<char16_t>(unit.value()));
    }
}

/// The access violation Windows takes; when no CPU can carry it, the
/// documented E_INVALIDARG.
void fault_or_invalid(Marshaler& m, bool write, GuestAddr address) {
    if (!m.raise_access_violation(write, address)) m.set_return(E_INVALIDARG);
}

bool write_key(Marshaler& m, GuestAddr out, const PropertyKey& k) {
    bool ok = m.write_u32(out, k.data1).is_ok() && m.write_u16(out + 4, k.data2).is_ok() &&
              m.write_u16(out + 6, k.data3).is_ok() && m.write_u32(out + 16, k.pid).is_ok();
    for (usize b = 0; b < k.data4.size() && ok; ++b) ok = m.write_u8(out + 8 + b, k.data4[b]).is_ok();
    return ok;
}

std::optional<PropertyKey> read_key(const Marshaler& m, GuestAddr in) {
    const auto d1 = m.read_u32(in);
    const auto d2 = m.read_u16(in + 4);
    const auto d3 = m.read_u16(in + 6);
    const auto pid = m.read_u32(in + 16);
    if (d1.is_err() || d2.is_err() || d3.is_err() || pid.is_err()) return std::nullopt;
    PropertyKey k;
    k.data1 = d1.value();
    k.data2 = d2.value();
    k.data3 = d3.value();
    k.pid = pid.value();
    for (usize b = 0; b < k.data4.size(); ++b) {
        const auto v = m.read_u8(in + 8 + b);
        if (v.is_err()) return std::nullopt;
        k.data4[b] = v.value();
    }
    return k;
}

// HRESULT PSGetPropertyKeyFromName(PCWSTR pszName, PROPERTYKEY* ppropkey)
// Learn: S_OK, E_INVALIDARG (pszName NULL), TYPE_E_ELEMENTNOTFOUND.
// Measured on winoracle, x86 and x64 alike (propsys_name_probe{32,64}):
// *ppropkey is zeroed before anything else (a NULL ppropkey faults on that
// write even for an unknown name); the thread must be in a COM apartment,
// its own or the process MTA (else CO_E_NOTINITIALIZED); a NULL pszName
// faults reading address 0 rather than returning E_INVALIDARG.
void impl_PSGetPropertyKeyFromName(Marshaler& m, ComCoreManager& com) {
    const GuestAddr name = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0 || !write_key(m, out, PropertyKey{})) {
        fault_or_invalid(m, true, out);
        return;
    }
    if (!com.thread_in_apartment(m)) {
        m.set_return(CO_E_NOTINITIALIZED);
        return;
    }
    if (name == 0) {
        fault_or_invalid(m, false, 0);
        return;
    }
    const GuestString text = read_guest_utf16(m, name);
    if (text.fault) {
        fault_or_invalid(m, false, *text.fault);
        return;
    }
    const PropertyKeyLookup r = property_key_from_name(text.text);
    if (r.hr == S_OK) (void)write_key(m, out, r.key);
    m.set_return(r.hr);
}

// HRESULT PSGetNameFromPropertyKey(REFPROPERTYKEY propkey, PWSTR* ppszCanonicalName)
// Learn: S_OK, TYPE_E_ELEMENTNOTFOUND; the string is the caller's to
// CoTaskMemFree. Measured: *ppszCanonicalName is set to NULL first (a NULL
// pointer faults on that write even for an unknown key); COM apartment as
// above; a NULL propkey faults reading address 0.
void impl_PSGetNameFromPropertyKey(Marshaler& m, ComCoreManager& com, ComTaskMemoryManager& task_memory) {
    const GuestAddr key_ptr = m.arg_ptr(0);
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0 || m.write_ptr(out, 0).is_err()) {
        fault_or_invalid(m, true, out);
        return;
    }
    if (!com.thread_in_apartment(m)) {
        m.set_return(CO_E_NOTINITIALIZED);
        return;
    }
    if (key_ptr == 0) {
        fault_or_invalid(m, false, 0);
        return;
    }
    const auto key = read_key(m, key_ptr);
    if (!key) {
        fault_or_invalid(m, false, key_ptr);
        return;
    }
    const PropkeyName* e = find_by_key(*key);
    if (e == nullptr) {
        m.set_return(TYPE_E_ELEMENTNOTFOUND);
        return;
    }
    const std::string_view name(e->name);
    const GuestAddr buffer = task_memory.allocate((name.size() + 1) * sizeof(u16));
    if (buffer == 0) {
        m.set_return(E_OUTOFMEMORY);
        return;
    }
    for (usize i = 0; i <= name.size(); ++i) {
        const u16 unit = i < name.size() ? static_cast<u16>(static_cast<unsigned char>(name[i])) : 0;
        (void)m.write_u16(buffer + i * sizeof(u16), unit);
    }
    (void)m.write_ptr(out, buffer);
    m.set_return(S_OK);
}

constexpr u16 VT_EMPTY = 0;
constexpr u16 VT_BSTR = 8;
constexpr u16 VT_LPWSTR = 31;

VResult<std::vector<u16>> read_utf16_z(const Marshaler& m,
                                       GuestAddr source) {
    if (source == 0) {
        return VResult<std::vector<u16>>::err(Error::InvalidArgument);
    }

    std::vector<u16> result;
    result.reserve(64);
    for (usize i = 0; i < 65536; ++i) {
        auto value = m.read_u16(source + i * sizeof(u16));
        if (value.is_err()) {
            return VResult<std::vector<u16>>::err(value.error());
        }
        if (value.value() == 0) {
            return VResult<std::vector<u16>>::ok(std::move(result));
        }
        result.push_back(value.value());
    }
    return VResult<std::vector<u16>>::err(Error::InvalidArgument);
}

u32 write_bounded_utf16(Marshaler& m, GuestAddr destination, u32 cch,
                        const std::vector<u16>& value) {
    if (destination == 0 || cch == 0) {
        return E_INVALIDARG;
    }

    const usize capacity = static_cast<usize>(cch - 1);
    const usize copied = std::min(capacity, value.size());
    for (usize i = 0; i < copied; ++i) {
        if (m.write_u16(destination + i * sizeof(u16), value[i]).is_err()) {
            return E_INVALIDARG;
        }
    }
    if (m.write_u16(destination + copied * sizeof(u16), 0).is_err()) {
        return E_INVALIDARG;
    }

    return copied == value.size() ? S_OK : STRSAFE_E_INSUFFICIENT_BUFFER;
}

u32 write_bounded_ascii_as_utf16(Marshaler& m, GuestAddr destination, u32 cch,
                                 std::string_view value) {
    std::vector<u16> wide;
    wide.reserve(value.size());
    for (char ch : value) {
        wide.push_back(static_cast<u16>(static_cast<unsigned char>(ch)));
    }
    return write_bounded_utf16(m, destination, cch, wide);
}

void impl_PSStringFromPropertyKey(Marshaler& m) {
    const GuestAddr key = m.arg_ptr(0);
    const GuestAddr output = m.arg_ptr(1);
    const u32 cch = static_cast<u32>(m.arg(2));
    if (key == 0 || output == 0 || cch == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const auto data1 = m.read_u32(key);
    const auto data2 = m.read_u16(key + 4);
    const auto data3 = m.read_u16(key + 6);
    const auto pid = m.read_u32(key + 16);
    std::array<u8, 8> data4{};
    bool readable = data1.is_ok() && data2.is_ok() &&
                    data3.is_ok() && pid.is_ok();
    for (usize i = 0; i < data4.size() && readable; ++i) {
        const auto byte = m.read_u8(key + 8 + i);
        if (byte.is_err()) {
            readable = false;
        } else {
            data4[i] = byte.value();
        }
    }
    if (!readable) {
        m.set_return(E_INVALIDARG);
        return;
    }

    std::array<char, 64> formatted{};
    const int length = std::snprintf(
        formatted.data(), formatted.size(),
        "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} %u",
        data1.value(), data2.value(), data3.value(),
        data4[0], data4[1], data4[2], data4[3],
        data4[4], data4[5], data4[6], data4[7], pid.value());
    if (length < 0 || static_cast<usize>(length) >= formatted.size()) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const std::string_view result(formatted.data(),
                                  static_cast<usize>(length));
    const u32 hr = write_bounded_ascii_as_utf16(m, output, cch, result);
    VITREO_TRACE(
        "[propsys] PSStringFromPropertyKey(key=0x%llx, cch=%u) "
        "-> \"%.*s\" hr=0x%08x\n",
        static_cast<unsigned long long>(key), cch,
        static_cast<int>(result.size()), result.data(), hr);
    m.set_return(hr);
}

void impl_PropVariantToString(Marshaler& m) {
    const GuestAddr propvariant = m.arg_ptr(0);
    const GuestAddr output = m.arg_ptr(1);
    const u32 cch = static_cast<u32>(m.arg(2));
    if (propvariant == 0 || output == 0 || cch == 0) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const auto vt_result = m.read_u16(propvariant);
    if (vt_result.is_err()) {
        m.set_return(E_INVALIDARG);
        return;
    }

    const u16 vt = vt_result.value();
    if (vt == VT_EMPTY) {
        const std::vector<u16> empty;
        m.set_return(write_bounded_utf16(m, output, cch, empty));
        return;
    }

    if (vt == VT_LPWSTR || vt == VT_BSTR) {
        const auto string_pointer = m.read_ptr(propvariant + 8);
        if (string_pointer.is_err() || string_pointer.value() == 0) {
            (void)m.write_u16(output, 0);
            m.set_return(E_INVALIDARG);
            return;
        }
        const auto value = read_utf16_z(m, string_pointer.value());
        if (value.is_err()) {
            (void)m.write_u16(output, 0);
            m.set_return(E_INVALIDARG);
            return;
        }
        m.set_return(write_bounded_utf16(m, output, cch, value.value()));
        return;
    }

    // Windows converts many additional scalar and vector VARTYPEs. Vitreo
    // does not claim those conversions until their exact formatting is
    // implemented; leave a valid empty output and report the type mismatch.
    (void)m.write_u16(output, 0);
    VITREO_TRACE(
        "[propsys] PropVariantToString unsupported VARTYPE=%u "
        "-> DISP_E_TYPEMISMATCH\n",
        static_cast<unsigned>(vt));
    m.set_return(DISP_E_TYPEMISMATCH);
}

} // namespace

PropertyKeyLookup property_key_from_name(std::u16string_view name) {
    PropertyKeyLookup r;
    if (!name.empty() && name.front() == u'{') {
        if (name.size() > kMaxKeyStringName) {
            r.hr = STRSAFE_E_INSUFFICIENT_BUFFER;
            return r;
        }
        const auto key = parse_key_string(name);
        r.hr = key ? S_OK : E_INVALIDARG;
        if (key) r.key = *key;
        return r;
    }
    r.hr = TYPE_E_ELEMENTNOTFOUND;
    const auto ascii = ascii_name(name);
    if (!ascii) return r;
    const std::string_view wanted(*ascii);
    const auto* begin = std::begin(kPropkeyCanonicalNames);
    const auto* end = std::end(kPropkeyCanonicalNames);
    const auto* hit = std::lower_bound(begin, end, wanted, [](const PropkeyName& e, std::string_view n) {
        return std::string_view(e.name) < n;
    });
    if (hit != end && std::string_view(hit->name) == wanted) {
        r.hr = S_OK;
        r.key = key_of(*hit);
        return r;
    }
    std::string folded(wanted);
    for (char& c : folded) c = ascii_lower(c);
    for (const auto& e : kPropkeyLegacyNames) {
        std::string_view legacy(e.name);
        if (legacy.size() != folded.size()) continue;
        bool same = true;
        for (usize i = 0; i < legacy.size() && same; ++i) same = ascii_lower(legacy[i]) == folded[i];
        if (same) {
            r.hr = S_OK;
            r.key = key_of(e);
            return r;
        }
    }
    return r;
}

std::optional<PropertySchema> property_schema(const PropertyKey& key) {
    const PropkeyName* e = find_by_key(key);
    if (e == nullptr) return std::nullopt;
    PropertySchema s;
    s.canonical_name = e->name;
    s.vt = e->vt;
    s.registered = e->registered;
    s.tree = e->tree;
    s.enumerated = e->enumerated;
    return s;
}

DllExports make_propsys_exports(ComCoreManager& com, ComTaskMemoryManager& task_memory) {
    DllExports exports;
    exports.entries.reserve(4);
    exports.entries.push_back(
        {"PSStringFromPropertyKey", 0, impl_PSStringFromPropertyKey, 12});
    exports.entries.push_back(
        {"PropVariantToString", 0, impl_PropVariantToString, 12});
    exports.entries.push_back({"PSGetPropertyKeyFromName", 0,
                               [&com](Marshaler& m) { impl_PSGetPropertyKeyFromName(m, com); }, 8});
    exports.entries.push_back({"PSGetNameFromPropertyKey", 0,
                               [&com, &task_memory](Marshaler& m) {
                                   impl_PSGetNameFromPropertyKey(m, com, task_memory);
                               },
                               8});
    return exports;
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
/// @file cert_find_names.cpp
/// @brief CertFindCertificateInStore and CertGetNameStringA/W.
///
/// Contracts:
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certfindcertificateinstore
///  https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetnamestringw
///  wincrypt.h CERT_COMPARE_* / CERT_FIND_* values.
/// Measured Windows behaviour, x86 and x64: tools/oracle_probe/crypt_find_probe.c
/// (+ .expected / .x86.expected) — every find type over a memory store
/// (newest certificate first), and every name type for subject and issuer
/// including truncation, "not found" (empty string, count 1,
/// CRYPT_E_NOT_FOUND) and the last error on success (0; untouched when the
/// friendly name comes from the property).

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "winapi/system/crypt32/guest_block.hpp"
#include "x509_parse.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "ipc/ipc_gate.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>

namespace vitreo::winapi {

namespace {

constexpr u32 kCryptENotFound = 0x80092004u;
constexpr u32 kCertInfoIssuerFlag = 4;

std::u16string utf8_to_utf16(std::string_view text) {
    std::u16string out;
    for (usize i = 0; i < text.size();) {
        const u8 c = static_cast<u8>(text[i]);
        u32 cp = c;
        usize n = 1;
        if (c >= 0xf0) { cp = c & 0x07; n = 4; }
        else if (c >= 0xe0) { cp = c & 0x0f; n = 3; }
        else if (c >= 0xc0) { cp = c & 0x1f; n = 2; }
        if (i + n > text.size()) { cp = 0xfffd; n = 1; }
        else for (usize k = 1; k < n; ++k) cp = (cp << 6) | (static_cast<u8>(text[i + k]) & 0x3f);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xd800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xdc00 + (cp & 0x3ff)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
        i += n;
    }
    return out;
}

std::u16string ascii_to_utf16(std::span<const u8> text) {
    return std::u16string(text.begin(), text.end());
}

/// A certificate's serial (INTEGER contents, big-endian) against a
/// CRYPT_INTEGER_BLOB from CERT_INFO / CMSG_SIGNER_INFO / CERT_ID, which
/// Windows keeps little-endian. Leading zero octets are not significant.
bool same_serial(std::span<const u8> big_endian, std::span<const u8> little_endian) {
    usize a = 0;
    while (a < big_endian.size() && big_endian[a] == 0) ++a;
    usize b = little_endian.size();
    while (b > 0 && little_endian[b - 1] == 0) --b;
    if (big_endian.size() - a != b) return false;
    for (usize i = 0; i < b; ++i) {
        if (big_endian[a + i] != little_endian[b - 1 - i]) return false;
    }
    return true;
}

const x509::Extension* find_extension(const x509::ParsedCertificate& cert, std::string_view oid) {
    for (const auto& e : cert.extensions) if (e.oid == oid) return &e;
    return nullptr;
}

std::vector<x509::GeneralName> alt_names(const x509::ParsedCertificate& cert, bool issuer) {
    const auto* e = find_extension(cert, issuer ? "2.5.29.18" : "2.5.29.17");
    return e ? x509::decode_general_names(e->value) : std::vector<x509::GeneralName>{};
}

const std::string* attribute(const std::vector<x509::NameAttribute>& attributes, std::string_view oid) {
    for (const auto& a : attributes) if (a.oid == oid) return &a.value;
    return nullptr;
}

struct NameResult {
    bool found{false};
    std::u16string text;        // without terminator (multi-string keeps its inner NULs)
    bool from_property{false};
    bool multi{false};
};

NameResult resolve_name(const x509::ParsedCertificate& cert, const Crypt32Manager::CertProperties* props,
                        u32 type, u32 flags, Marshaler& m, GuestAddr type_para) {
    constexpr u32 kEmail = 1, kRdn = 2, kAttr = 3, kSimple = 4, kFriendly = 5, kDns = 6, kUrl = 7,
                  kUpn = 8;
    constexpr u32 kIssuerFlag = 0x1;
    constexpr u32 kSearchAllNames = 0x2;
    const bool issuer = (flags & kIssuerFlag) != 0;
    const auto& attributes = issuer ? cert.issuer_attributes : cert.subject_attributes;
    const auto& name_der = issuer ? cert.issuer_der : cert.subject_der;
    NameResult r;
    auto found = [&](std::u16string text) {
        r.found = true;
        r.text = std::move(text);
        return r;
    };
    if ((flags & kSearchAllNames) != 0 && type != kDns) return r;  // empty string
    const auto names = alt_names(cert, issuer);
    auto first_alt = [&](u8 choice) -> const x509::GeneralName* {
        for (const auto& n : names) if (n.choice == choice) return &n;
        return nullptr;
    };
    switch (type) {
    case kEmail: {
        if (const auto* n = first_alt(1)) return found(ascii_to_utf16(n->value));
        if (const auto* v = attribute(attributes, "1.2.840.113549.1.9.1")) return found(utf8_to_utf16(*v));
        return r;
    }
    case kRdn: {
        // A NULL pvTypePara formats as CERT_SIMPLE_NAME_STR: values only,
        // no "C=" keys (tools/oracle_probe/wintrust_signer_name_probe.expected,
        // Cert[0] RDN 'US, New York, New York, "Rockstar Games, Inc.", ...').
        constexpr u32 kCertSimpleNameStr = 1;
        const u32 str_type = type_para ? m.read_u32(type_para).value_or(0) : kCertSimpleNameStr;
        std::string text = x509::format_distinguished_name(name_der, str_type);
        if (text.empty()) {
            if (const auto* n = first_alt(4)) text = x509::format_distinguished_name(n->value, str_type);
        }
        if (text.empty()) return r;
        return found(utf8_to_utf16(text));
    }
    case kAttr: {
        const auto oid = m.read_ansi_string(type_para, 256);
        if (oid.is_err()) return r;
        if (const auto* v = attribute(attributes, oid.value())) return found(utf8_to_utf16(*v));
        return r;
    }
    case kFriendly:
        if (props != nullptr) {
            auto friendly = props->blobs.find(11);  // CERT_FRIENDLY_NAME_PROP_ID
            if (friendly != props->blobs.end()) {
                std::u16string text;
                for (usize i = 0; i + 1 < friendly->second.size(); i += 2) {
                    const char16_t c = static_cast<char16_t>(friendly->second[i] |
                                                             friendly->second[i + 1] << 8);
                    if (c == 0) break;
                    text.push_back(c);
                }
                r.from_property = true;
                return found(std::move(text));
            }
        }
        [[fallthrough]];
    case kSimple: {
        for (const char* oid : {"2.5.4.3", "2.5.4.11", "2.5.4.10", "1.2.840.113549.1.9.1"}) {
            if (const auto* v = attribute(attributes, oid)) return found(utf8_to_utf16(*v));
        }
        if (const auto* n = first_alt(1)) return found(ascii_to_utf16(n->value));
        if (!attributes.empty()) return found(utf8_to_utf16(attributes.front().value));
        return r;
    }
    case kDns: {
        if ((flags & kSearchAllNames) != 0) {
            std::u16string all;
            for (const auto& n : names) {
                if (n.choice != 2) continue;
                all += ascii_to_utf16(n.value);
                all.push_back(0);
            }
            if (all.empty()) {
                if (const auto* v = attribute(attributes, "2.5.4.3")) {
                    all = utf8_to_utf16(*v);
                    all.push_back(0);
                }
            }
            r.multi = true;
            r.found = !all.empty();
            r.text = std::move(all);
            return r;
        }
        if (const auto* n = first_alt(2)) return found(ascii_to_utf16(n->value));
        if (const auto* v = attribute(attributes, "2.5.4.3")) return found(utf8_to_utf16(*v));
        return r;
    }
    case kUrl:
        if (const auto* n = first_alt(6)) return found(ascii_to_utf16(n->value));
        return r;
    case kUpn:
        for (const auto& n : names) {
            if (n.choice != 0 || n.other_oid != "1.3.6.1.4.1.311.20.2.3") continue;
            // [0] EXPLICIT { UTF8String / other string }
            std::span<const u8> v(n.value);
            if (v.size() < 2 || v[0] != 0xa0) continue;
            usize at = 2;
            if (v[1] & 0x80) at += v[1] & 0x7f;
            if (at + 2 > v.size()) continue;
            usize len = v[at + 1];
            usize data = at + 2;
            if (len & 0x80) {
                const usize k = len & 0x7f;
                len = 0;
                for (usize i = 0; i < k && data < v.size(); ++i) len = (len << 8) | v[data++];
            }
            if (data + len > v.size()) continue;
            return found(utf8_to_utf16(std::string_view(reinterpret_cast<const char*>(v.data() + data), len)));
        }
        return r;
    default:
        return r;
    }
}

} // namespace

// DWORD CertGetNameStringW/A(PCCERT_CONTEXT pCertContext, DWORD dwType,
//     DWORD dwFlags, void* pvTypePara, LPWSTR/LPSTR pszNameString, DWORD cchNameString)
u32 Crypt32Manager::cert_get_name_string(Marshaler& m, bool wide) {
    const GuestAddr context = m.arg_ptr(0);
    const u32 type = m.arg32(1);
    const u32 flags = m.arg32(2);
    const GuestAddr type_para = m.arg_ptr(3);
    const GuestAddr output = m.arg_ptr(4);
    const u32 capacity = m.arg32(5);
    const u32 ps = m.cpu_is_64bit() ? 8u : 4u;

    std::vector<u8> der;
    std::shared_ptr<CertProperties> props;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto entry = m_cert_contexts.find(context);
        if (entry != m_cert_contexts.end()) {
            der = entry->second.der_data;
            props = entry->second.props;
        }
    }
    if (der.empty()) {
        const GuestAddr bytes = static_cast<GuestAddr>(m.read_ptr(context + ps).value_or(0));
        const u32 size = m.read_u32(context + 2 * ps).value_or(0);
        der.resize(size);
        if (size == 0 || size > 0x100000 ||
            m.memory().read_bytes(bytes, reinterpret_cast<Byte*>(der.data()), size).is_err()) {
            der.clear();
        }
    }
    NameResult name;
    if (auto parsed = x509::parse_certificate(der)) {
        name = resolve_name(*parsed, props.get(), type, flags, m, type_para);
    }

    // The characters to hand back, including the terminator(s).
    std::vector<u16> units;
    std::vector<u8> bytes;
    if (wide) {
        units.assign(name.text.begin(), name.text.end());
        units.push_back(0);
    } else {
        const std::vector<u16> source(name.text.begin(), name.text.end());
        if (!source.empty()) {
            auto size = codepage::wide_to_multi_byte(codepage::kCpAcp, 0, source.data(), source.size(),
                                                     nullptr, 0, nullptr, false);
            bytes.resize(size.count);
            (void)codepage::wide_to_multi_byte(codepage::kCpAcp, 0, source.data(), source.size(),
                                               bytes.data(), bytes.size(), nullptr, false);
        }
        bytes.push_back(0);
    }
    const u32 needed = static_cast<u32>(wide ? units.size() : bytes.size());
    {
        std::string text_utf8;
        for (const char16_t c : name.text) text_utf8.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        ipc::ipc_log("CertGetNameString%c(ctx=0x%llx, type=%u, flags=0x%x, para=0x%llx, cch=%u) "
                     "found=%d -> '%s'",
                     wide ? 'W' : 'A', static_cast<unsigned long long>(context), type, flags,
                     static_cast<unsigned long long>(type_para), capacity, name.found ? 1 : 0,
                     text_utf8.c_str());
    }
    if (!name.found) {
        set_last_error(kCryptENotFound);
    } else if (!name.from_property) {
        set_last_error(0);
    }
    if (output == 0 || capacity == 0) return needed;
    const u32 count = std::min(needed, capacity);
    if (wide) {
        for (u32 i = 0; i + 1 < count; ++i) (void)m.write_u16(output + i * 2u, units[i]);
        (void)m.write_u16(output + (count - 1) * 2u, 0);
    } else {
        for (u32 i = 0; i + 1 < count; ++i) (void)m.write_u8(output + i, bytes[i]);
        (void)m.write_u8(output + count - 1, 0);
    }
    return count;
}

// PCCERT_CONTEXT CertFindCertificateInStore(HCERTSTORE hCertStore,
//     DWORD dwCertEncodingType, DWORD dwFindFlags, DWORD dwFindType,
//     const void* pvFindPara, PCCERT_CONTEXT pPrevCertContext)
GuestAddr Crypt32Manager::cert_find_certificate_in_query_store(Marshaler& m) {
    std::vector<GuestAddr> released_allocations;
    auto free_released = [this, &released_allocations](void*) {
        for (GuestAddr allocation : released_allocations) free_guest(allocation);
    };
    std::unique_ptr<void, decltype(free_released)> release_guard(
        reinterpret_cast<void*>(1), free_released);
    constexpr const char* kApi = "crypt32!CertFindCertificateInStore";
    const u32 store_handle = static_cast<u32>(m.arg(0));
    const u32 find_flags = m.arg32(2);
    const u32 find_type = m.arg32(3);
    const GuestAddr find = m.arg_ptr(4);
    const GuestAddr previous = m.arg_ptr(5);
    const bool is64 = m.cpu_is_64bit();
    const u32 ps = is64 ? 8u : 4u;
    const u32 compare = find_type >> 16;
    const bool issuer_field = (find_type & 0xffff) == kCertInfoIssuerFlag;

    auto read_bytes = [&](GuestAddr ptr, u32 size) -> std::vector<u8> {
        std::vector<u8> bytes(size);
        if (size != 0 && (ptr == 0 || size > 0x100000u ||
                          m.memory().read_bytes(ptr, reinterpret_cast<Byte*>(bytes.data()), size).is_err())) {
            return {};
        }
        return bytes;
    };
    auto read_blob = [&](GuestAddr blob) {
        return read_bytes(static_cast<GuestAddr>(m.read_ptr(blob + ps).value_or(0)),
                          m.read_u32(blob).value_or(0));
    };
    auto context_der = [&](GuestAddr context) {
        return read_bytes(static_cast<GuestAddr>(m.read_ptr(context + ps).value_or(0)),
                          m.read_u32(context + 2 * ps).value_or(0));
    };

    // --- decode pvFindPara ---------------------------------------------------
    std::vector<u8> want;                  // hash / name / key / der
    std::vector<u8> want_serial;           // little-endian
    std::string want_oid;
    std::u16string want_text;
    std::vector<std::string> want_usage;
    u32 want_dword = 0;
    u32 prop_for_hash = 0;
    enum class Mode : u8 { Any, Hash, Name, Property, PublicKey, NameString, KeySpec, Usage,
                           IssuerSerial, IssuerOf, Existing, CertIdKey, PrivateKey, None };
    Mode mode = Mode::None;
    const crypt32::CertLayout l(is64);
    switch (compare) {
    case 0: mode = Mode::Any; break;
    case 1: case 4: case 14: case 15: case 18: case 22: case 23:
        mode = Mode::Hash;
        want = read_blob(find);
        prop_for_hash = compare == 1 ? 3 : compare == 4 ? 4 : compare == 14 ? 15 : compare == 15 ? 20
                      : compare == 18 ? 25 : compare == 22 ? 107 : 129;
        break;
    case 2: mode = Mode::Name; want = read_blob(find); break;
    case 5: mode = Mode::Property; want_dword = m.read_u32(find).value_or(0); break;
    case 6: {
        mode = Mode::PublicKey;
        auto oid = m.read_ansi_string(static_cast<GuestAddr>(m.read_ptr(find).value_or(0)), 256);
        want_oid = oid.is_ok() ? oid.value() : std::string{};
        want = read_blob(find + 3 * ps);
        break;
    }
    case 7: case 8: {
        mode = Mode::NameString;
        if (compare == 8) {
            for (GuestAddr p = find; p != 0; p += 2) {
                const u16 c = m.read_u16(p).value_or(0);
                if (c == 0 || want_text.size() > 4096) break;
                want_text.push_back(static_cast<char16_t>(c));
            }
        } else {
            auto text = m.read_ansi_string(find, 4096);
            if (text.is_ok()) want_text = utf8_to_utf16(text.value());
        }
        break;
    }
    case 9: mode = Mode::KeySpec; want_dword = m.read_u32(find).value_or(0); break;
    case 10: {
        mode = Mode::Usage;
        constexpr u32 kValidUsage = 0x20;
        if ((find_flags & kValidUsage) != 0) {
            crypt32::report_unimplemented(kApi, "CERT_FIND_VALID_ENHKEY_USAGE_FLAG");
            set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
            return 0;
        }
        const u32 count = m.read_u32(find).value_or(0);
        const GuestAddr array = static_cast<GuestAddr>(m.read_ptr(find + ps).value_or(0));
        for (u32 i = 0; i < count && i < 64; ++i) {
            auto oid = m.read_ansi_string(static_cast<GuestAddr>(m.read_ptr(array + i * ps).value_or(0)), 256);
            if (oid.is_ok()) want_usage.push_back(oid.value());
        }
        break;
    }
    case 11:
        mode = Mode::IssuerSerial;
        want = read_blob(find + l.issuer);
        want_serial = read_blob(find + l.serial);
        break;
    case 12: {
        mode = Mode::IssuerOf;
        auto subject = x509::parse_certificate(context_der(find));
        if (subject) want = subject->issuer_der;
        break;
    }
    case 13: mode = Mode::Existing; want = context_der(find); break;
    case 16: {                                   // CERT_ID
        const u32 choice = m.read_u32(find).value_or(0);
        if (choice == 1) {                       // CERT_ID_ISSUER_SERIAL_NUMBER
            mode = Mode::IssuerSerial;
            want = read_blob(find + ps);
            want_serial = read_blob(find + 3 * ps);
        } else if (choice == 2) {                // CERT_ID_KEY_IDENTIFIER
            mode = Mode::Hash;
            prop_for_hash = 20;
            want = read_blob(find + ps);
        } else if (choice == 3) {                // CERT_ID_SHA1_HASH
            mode = Mode::Hash;
            prop_for_hash = 3;
            want = read_blob(find + ps);
        }
        break;
    }
    case 20: {                                   // CERT_FIND_HASH_STR: hex, spaces allowed
        mode = Mode::Hash;
        prop_for_hash = 3;
        int high = -1;
        for (GuestAddr p = find; p != 0; p += 2) {
            const u16 c = m.read_u16(p).value_or(0);
            if (c == 0 || want.size() > 64) break;
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (v < 0) continue;
            if (high < 0) { high = v; } else { want.push_back(static_cast<u8>(high << 4 | v)); high = -1; }
        }
        break;
    }
    case 21: mode = Mode::PrivateKey; break;
    case 3: case 17: case 19:
        crypt32::report_unimplemented(kApi, "CERT_FIND_*_ATTR / CROSS_CERT_DIST_POINTS / SUBJECT_INFO_ACCESS");
        set_last_error(error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        return 0;
    default:
        mode = Mode::None;                       // no certificate matches an unknown type
        break;
    }

    // --- the store's elements, in enumeration order ---------------------------
    struct FlatEntry {
        std::vector<u8> der;
        std::string record;
        u32 source_store{0};
        std::shared_ptr<CertProperties> props;
    };
    std::vector<FlatEntry> flat;
    usize start = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto system_name = m_system_store_names.find(store_handle);
        auto memory_store = m_store_certs.find(store_handle);
        auto collection = m_collection_stores.find(store_handle);
        if (system_name == m_system_store_names.end() &&
            memory_store == m_store_certs.end() && collection == m_collection_stores.end()) {
            if (previous != 0) released_allocations = release_cert_context_locked(previous);
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            return 0;
        }
        if (previous != 0) {
            auto prior = m_cert_contexts.find(previous);
            if (prior == m_cert_contexts.end() || prior->second.store_handle != store_handle ||
                prior->second.store_index == std::numeric_limits<usize>::max()) {
                released_allocations = release_cert_context_locked(previous);
                set_last_error(crypt32::err::kEInvalidArg);
                return 0;
            }
            start = prior->second.store_index + 1;
            released_allocations = release_cert_context_locked(previous);
        }
        auto props_of = [&](u32 store, const std::vector<u8>& der) -> std::shared_ptr<CertProperties> {
            auto s = m_element_properties.find(store);
            if (s == m_element_properties.end()) return nullptr;
            auto e = s->second.find(der);
            return e == s->second.end() ? nullptr : e->second;
        };
        auto append_system = [&](const std::string& name, u32 handle) {
            if (!load_system_store_locked(name)) return;
            for (const auto& c : m_system_store_certs[name]) {
                flat.push_back(FlatEntry{c.der_data, c.record_name, handle, props_of(handle, c.der_data)});
            }
        };
        if (collection != m_collection_stores.end()) {
            for (const auto& sibling : collection->second) {
                auto sibling_system = m_system_store_names.find(sibling.store_handle);
                if (sibling_system != m_system_store_names.end()) {
                    append_system(sibling_system->second, sibling.store_handle);
                } else if (!sibling.system_store_name.empty()) {
                    append_system(sibling.system_store_name, sibling.store_handle);
                } else if (auto sm = m_store_certs.find(sibling.store_handle); sm != m_store_certs.end()) {
                    for (const auto& c : sm->second) {
                        flat.push_back(FlatEntry{c, {}, sibling.store_handle, props_of(sibling.store_handle, c)});
                    }
                }
            }
        } else if (system_name != m_system_store_names.end()) {
            append_system(system_name->second, store_handle);
        } else {
            for (const auto& c : memory_store->second) {
                flat.push_back(FlatEntry{c, {}, store_handle, props_of(store_handle, c)});
            }
        }
    }

    auto matches = [&](const FlatEntry& entry) -> bool {
        if (mode == Mode::Any) return true;
        if (mode == Mode::None) return false;
        auto parsed = x509::parse_certificate(entry.der);
        if (!parsed) return false;
        switch (mode) {
        case Mode::Hash: {
            if (entry.props) {
                auto stored = entry.props->blobs.find(prop_for_hash);
                if (stored != entry.props->blobs.end()) return stored->second == want;
            }
            auto value = crypt32::compute_certificate_property(prop_for_hash, entry.der, *parsed);
            return value && *value == want;
        }
        case Mode::Name:
            return (issuer_field ? parsed->issuer_der : parsed->subject_der) == want;
        case Mode::Property:
            if (entry.props) {
                if (entry.props->blobs.contains(want_dword)) return true;
                if (want_dword == 2 && entry.props->key_prov_info) return true;
                if ((want_dword == 5 || want_dword == 6) && entry.props->key_context_spec) return true;
            }
            return crypt32::compute_certificate_property(want_dword, entry.der, *parsed).has_value();
        case Mode::PublicKey:
            return parsed->spki.algorithm.oid == want_oid && parsed->spki.key == want;
        case Mode::NameString: {
            const std::string text = x509::format_distinguished_name(
                issuer_field ? parsed->issuer_der : parsed->subject_der,
                1u /* CERT_SIMPLE_NAME_STR */ | 0x10000000u /* CERT_NAME_STR_NO_QUOTING_FLAG */);
            std::u16string hay = utf8_to_utf16(text);
            std::u16string needle = want_text;
            auto fold = [](std::u16string s) {
                for (char16_t& c : s) if (c < 0x80) c = static_cast<char16_t>(std::tolower(c));
                return s;
            };
            return fold(hay).find(fold(needle)) != std::u16string::npos;
        }
        case Mode::KeySpec: {
            if (!entry.props) return false;
            if (entry.props->key_context_spec) return *entry.props->key_context_spec == want_dword;
            return entry.props->key_prov_info && entry.props->key_prov_info->key_spec == want_dword;
        }
        case Mode::PrivateKey:
            return entry.props && (entry.props->key_prov_info || entry.props->key_context_spec);
        case Mode::Usage: {
            constexpr u32 kOptional = 0x1, kExtOnly = 0x2, kPropOnly = 0x4, kNone = 0x8, kOr = 0x10;
            std::optional<std::vector<std::string>> ext;
            if ((find_flags & kPropOnly) == 0) {
                if (const auto* e = find_extension(*parsed, "2.5.29.37")) {
                    std::vector<std::string> oids;
                    if (x509::decode_enhanced_key_usage(e->value, oids) == 0) ext = oids;
                }
            }
            std::optional<std::vector<std::string>> prop;
            if ((find_flags & kExtOnly) == 0 && entry.props) {
                auto p = entry.props->blobs.find(9);
                std::vector<std::string> oids;
                if (p != entry.props->blobs.end() && x509::decode_enhanced_key_usage(p->second, oids) == 0) {
                    prop = oids;
                }
            }
            std::optional<std::vector<std::string>> usage = ext ? ext : prop;
            if (ext && prop) {
                std::vector<std::string> both;
                for (const auto& o : *ext) if (std::find(prop->begin(), prop->end(), o) != prop->end()) both.push_back(o);
                usage = both;
            }
            if ((find_flags & kNone) != 0) return !usage.has_value();
            if (!usage) return (find_flags & kOptional) != 0;
            if (want_usage.empty()) return true;
            auto has = [&](const std::string& o) { return std::find(usage->begin(), usage->end(), o) != usage->end(); };
            return (find_flags & kOr) != 0 ? std::any_of(want_usage.begin(), want_usage.end(), has)
                                           : std::all_of(want_usage.begin(), want_usage.end(), has);
        }
        case Mode::IssuerSerial:
            return parsed->issuer_der == want && same_serial(parsed->serial, want_serial);
        case Mode::IssuerOf:
            return !want.empty() && parsed->subject_der == want;
        case Mode::Existing:
            return entry.der == want;
        default:
            return false;
        }
    };

    for (usize index = start; index < flat.size(); ++index) {
        if (!matches(flat[index])) continue;
        const GuestAddr context = create_cert_context_from_der(
            m, flat[index].der, store_handle, 0x00000001u, flat[index].record, index);
        if (context == 0) {
            set_last_error(error_codes::ERROR_INVALID_DATA);
            return 0;
        }
        if (flat[index].props) {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto created = m_cert_contexts.find(context);
            if (created != m_cert_contexts.end()) created->second.props = flat[index].props;
        }
        return context;
    }
    set_last_error(kCryptENotFound);
    return 0;
}

} // namespace vitreo::winapi

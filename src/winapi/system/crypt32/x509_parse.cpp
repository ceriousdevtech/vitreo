// SPDX-License-Identifier: LGPL-2.1-only
/// @file x509_parse.cpp
/// @brief Minimal DER X.509 certificate / SAN parsing (clean-room).

#include "x509_parse.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <string_view>

namespace vitreo::winapi::x509 {
namespace {

struct Cursor {
    const u8* p{nullptr};
    const u8* end{nullptr};

    bool empty() const { return p >= end; }
    usize remaining() const {
        return p < end ? static_cast<usize>(end - p) : 0;
    }
};

// Length of the contents of an indefinite-length constructed element that
// starts at `p` (just after its 0x80 length byte): scan nested elements until
// the matching end-of-contents octets. Returns false when they are missing.
bool indefinite_content_length(const u8* p, const u8* end, usize& length) {
    const u8* cursor = p;
    usize depth = 1;
    while (depth != 0) {
        if (end - cursor < 2) return false;
        const u8 tag = *cursor++;
        const u8 len0 = *cursor++;
        if (tag == 0 && len0 == 0) {
            if (--depth == 0) break;
            continue;
        }
        if (len0 == 0x80) {
            if ((tag & 0x20) == 0) return false;
            ++depth;
            continue;
        }
        usize n = len0;
        if (len0 & 0x80) {
            const u8 bytes = len0 & 0x7f;
            if (bytes == 0 || bytes > 8 || end - cursor < bytes) return false;
            n = 0;
            for (u8 i = 0; i < bytes; ++i) n = (n << 8) | *cursor++;
        }
        if (static_cast<usize>(end - cursor) < n) return false;
        cursor += n;
    }
    length = static_cast<usize>(cursor - p) - 2;  // exclude the EOC octets
    return true;
}

// BER element: definite short/long form or (constructed) indefinite form,
// which Windows accepts in certificates (crypt_prop_probe "indefinite" row).
bool read_tag_len(Cursor& c, u8& tag, std::span<const u8>& content) {
    if (c.remaining() < 2) return false;
    tag = *c.p++;
    u8 len0 = *c.p++;
    usize len = 0;
    if (len0 == 0x80) {
        if ((tag & 0x20) == 0) return false;
        if (!indefinite_content_length(c.p, c.end, len)) return false;
        content = std::span<const u8>(c.p, len);
        c.p += len + 2;
        return true;
    }
    if ((len0 & 0x80) == 0) {
        len = len0;
    } else {
        const u8 nbytes = len0 & 0x7f;
        if (nbytes == 0 || nbytes > 8 || c.remaining() < nbytes) return false;
        for (u8 i = 0; i < nbytes; ++i) {
            len = (len << 8) | *c.p++;
        }
    }
    if (c.remaining() < len) return false;
    content = std::span<const u8>(c.p, len);
    c.p += len;
    return true;
}

bool expect_tag(Cursor& c, u8 want, std::span<const u8>& content) {
    u8 tag = 0;
    if (!read_tag_len(c, tag, content)) return false;
    return tag == want;
}

bool skip_tag(Cursor& c, u8 want) {
    std::span<const u8> content;
    return expect_tag(c, want, content);
}

void append_utf8(std::string& out, u32 codepoint) {
    if (codepoint <= 0x7f) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        out.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string decode_directory_string(u8 tag, std::span<const u8> value) {
    // DirectoryString permits UTF8String, PrintableString, TeletexString,
    // UniversalString, and BMPString. IA5String is also used by emailAddress.
    if (tag == 0x0c || tag == 0x13 || tag == 0x16 || tag == 0x1a) {
        return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
    std::string decoded;
    if (tag == 0x1e && value.size() % 2 == 0) {
        for (usize i = 0; i < value.size(); i += 2) {
            append_utf8(decoded,
                        (static_cast<u32>(value[i]) << 8) | value[i + 1]);
        }
        return decoded;
    }
    if (tag == 0x1c && value.size() % 4 == 0) {
        for (usize i = 0; i < value.size(); i += 4) {
            append_utf8(decoded,
                        (static_cast<u32>(value[i]) << 24) |
                        (static_cast<u32>(value[i + 1]) << 16) |
                        (static_cast<u32>(value[i + 2]) << 8) | value[i + 3]);
        }
        return decoded;
    }
    if (tag == 0x14) {
        // TeletexString has no universal modern mapping. Preserve its common
        // single-byte repertoire as Unicode rather than returning no name.
        for (const u8 byte : value) append_utf8(decoded, byte);
        return decoded;
    }
    return {};
}

std::vector<NameAttribute> parse_name_attributes(std::span<const u8> name) {
    std::vector<NameAttribute> result;
    Cursor rdns{name.data(), name.data() + name.size()};
    while (!rdns.empty()) {
        std::span<const u8> set;
        if (!expect_tag(rdns, 0x31, set)) return {};
        Cursor attributes{set.data(), set.data() + set.size()};
        while (!attributes.empty()) {
            std::span<const u8> attribute;
            if (!expect_tag(attributes, 0x30, attribute)) return {};
            Cursor item{attribute.data(), attribute.data() + attribute.size()};
            std::span<const u8> oid;
            if (!expect_tag(item, 0x06, oid) || item.empty()) return {};
            u8 value_tag = 0;
            std::span<const u8> value;
            if (!read_tag_len(item, value_tag, value)) return {};
            std::string text = decode_directory_string(value_tag, value);
            if (text.empty()) continue;
            result.push_back(NameAttribute{oid_to_string(oid), std::move(text)});
        }
    }
    return result;
}

std::string simple_display_name(const std::vector<NameAttribute>& attributes) {
    static constexpr std::array<const char*, 4> preferred_oids{
        "2.5.4.3",       // commonName
        "2.5.4.11",      // organizationalUnitName
        "2.5.4.10",      // organizationName
        "1.2.840.113549.1.9.1", // emailAddress
    };
    for (const char* oid : preferred_oids) {
        auto match = std::find_if(attributes.begin(), attributes.end(),
                                  [&](const NameAttribute& attribute) {
                                      return attribute.oid == oid;
                                  });
        if (match != attributes.end()) return match->value;
    }
    return attributes.empty() ? std::string{} : attributes.front().value;
}

bool parse_extensions(std::span<const u8> ext_seq,
                      std::vector<Extension>& out) {
    Cursor c{ext_seq.data(), ext_seq.data() + ext_seq.size()};
    while (!c.empty()) {
        std::span<const u8> ext_body;
        if (!expect_tag(c, 0x30, ext_body)) return false;
        Cursor ec{ext_body.data(), ext_body.data() + ext_body.size()};
        std::span<const u8> oid_bytes;
        if (!expect_tag(ec, 0x06, oid_bytes)) return false;
        Extension ext;
        ext.oid = oid_to_string(oid_bytes);
        if (!ec.empty() && *ec.p == 0x01) {
            std::span<const u8> bool_bytes;
            if (!expect_tag(ec, 0x01, bool_bytes)) return false;
            ext.critical = !bool_bytes.empty() && bool_bytes[0] != 0;
        }
        std::span<const u8> octet;
        if (!expect_tag(ec, 0x04, octet)) return false;
        ext.value.assign(octet.begin(), octet.end());
        out.push_back(std::move(ext));
    }
    return true;
}

int two_digits(std::span<const u8> body, usize off) {
    if (off + 1 >= body.size()) return -1;
    const u8 a = body[off];
    const u8 b = body[off + 1];
    if (a < '0' || a > '9' || b < '0' || b > '9') return -1;
    return (a - '0') * 10 + (b - '0');
}

// UTCTime (0x17) / GeneralizedTime (0x18) → Windows FILETIME.
bool asn1_time_to_filetime(u8 tag, std::span<const u8> body, u64& filetime) {
    int year = 0;
    usize off = 0;
    if (tag == 0x17) {
        const int yy = two_digits(body, 0);
        if (yy < 0) return false;
        year = yy < 50 ? 2000 + yy : 1900 + yy;
        off = 2;
    } else if (tag == 0x18) {
        const int yhi = two_digits(body, 0);
        const int ylo = two_digits(body, 2);
        if (yhi < 0 || ylo < 0) return false;
        year = yhi * 100 + ylo;
        off = 4;
    } else {
        return false;
    }
    const int month = two_digits(body, off);
    const int day = two_digits(body, off + 2);
    const int hour = two_digits(body, off + 4);
    const int minute = two_digits(body, off + 6);
    const int second = two_digits(body, off + 8);
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 60) {
        return false;
    }
    if (off + 10 >= body.size() || body[off + 10] != 'Z') return false;

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
    tm.tm_isdst = 0;
    const time_t unix_time = timegm(&tm);
    if (unix_time == static_cast<time_t>(-1)) return false;
    filetime = (static_cast<u64>(unix_time) + 11644473600ULL) * 10000000ULL;
    return true;
}

bool parse_validity(std::span<const u8> validity, u64& not_before,
                    u64& not_after) {
    Cursor c{validity.data(), validity.data() + validity.size()};
    u8 tag = 0;
    std::span<const u8> first;
    std::span<const u8> second;
    if (!read_tag_len(c, tag, first) ||
        !asn1_time_to_filetime(tag, first, not_before)) {
        return false;
    }
    if (!read_tag_len(c, tag, second) ||
        !asn1_time_to_filetime(tag, second, not_after)) {
        return false;
    }
    return true;
}

} // namespace

std::optional<ParsedCertificate> parse_certificate(std::span<const u8> der) {
    // Accept exactly what Windows' certificate decoder accepts, so every
    // consumer (contexts, stores, chains) agrees with CertCreateCertificateContext.
    if (certificate_decode_error(der) != 0) return std::nullopt;
    Cursor top{der.data(), der.data() + der.size()};
    std::span<const u8> cert;
    if (!expect_tag(top, 0x30, cert)) return std::nullopt;

    Cursor c{cert.data(), cert.data() + cert.size()};
    const u8* tbs_tlv = c.p;          // signature covers the header too
    std::span<const u8> tbs;
    if (!expect_tag(c, 0x30, tbs)) return std::nullopt;
    const u8* tbs_end = c.p;

    Cursor t{tbs.data(), tbs.data() + tbs.size()};
    ParsedCertificate parsed;
    // Optional version [0] EXPLICIT INTEGER; absent means v1 (dwVersion 0).
    if (!t.empty() && *t.p == 0xa0) {
        std::span<const u8> version_wrap;
        if (!expect_tag(t, 0xa0, version_wrap)) return std::nullopt;
        Cursor v{version_wrap.data(), version_wrap.data() + version_wrap.size()};
        std::span<const u8> version;
        if (expect_tag(v, 0x02, version)) {
            u32 value = 0;
            for (const u8 byte : version) value = (value << 8) | byte;
            parsed.version = value;
        }
    }
    auto parse_alg_id = [](std::span<const u8> seq,
                           AlgorithmIdentifier& out) -> bool {
        if (seq.empty()) return true;
        Cursor a{seq.data(), seq.data() + seq.size()};
        std::span<const u8> oid;
        if (!expect_tag(a, 0x06, oid)) return false;
        out.oid = oid_to_string(oid);
        if (!a.empty()) {
            out.parameters.assign(a.p, a.end);
        }
        return true;
    };

    std::span<const u8> serial;
    if (!expect_tag(t, 0x02, serial)) return std::nullopt;
    std::span<const u8> sig_alg;
    if (!expect_tag(t, 0x30, sig_alg)) return std::nullopt;
    const u8* issuer_tlv = t.p;
    std::span<const u8> issuer;
    if (!expect_tag(t, 0x30, issuer)) return std::nullopt;
    const u8* issuer_end = t.p;
    std::span<const u8> validity;
    if (!expect_tag(t, 0x30, validity)) return std::nullopt;
    const u8* subject_tlv = t.p;
    std::span<const u8> subject;
    if (!expect_tag(t, 0x30, subject)) return std::nullopt;
    const u8* subject_end = t.p;
    const u8* spki_tlv = t.p;
    std::span<const u8> spki_seq;
    if (!expect_tag(t, 0x30, spki_seq)) return std::nullopt;
    parsed.spki_der.assign(spki_tlv, t.p);
    // Optional issuerUniqueID [1] / subjectUniqueID [2]: IMPLICIT BIT STRING
    // (primitive 0x81/0x82; the constructed forms are tolerated).
    auto unique_id = [&](u8 primitive, BitString& out) -> bool {
        if (t.empty() || (*t.p != primitive && *t.p != (primitive | 0x20))) {
            return true;
        }
        u8 tag = 0;
        std::span<const u8> bits;
        if (!read_tag_len(t, tag, bits)) return false;
        if (!bits.empty()) {
            out.unused_bits = bits[0];
            out.bytes.assign(bits.begin() + 1, bits.end());
        }
        out.present = true;
        return true;
    };
    if (!unique_id(0x81, parsed.issuer_unique_id) ||
        !unique_id(0x82, parsed.subject_unique_id)) {
        return std::nullopt;
    }

    parsed.tbs_der.assign(tbs_tlv, tbs_end);
    {
        // AlgorithmIdentifier then BIT STRING signatureValue follow tbs.
        Cursor sc{c.p, c.end};
        std::span<const u8> outer_alg;
        if (expect_tag(sc, 0x30, outer_alg)) {
            std::span<const u8> sig_bits;
            if (expect_tag(sc, 0x03, sig_bits) && !sig_bits.empty()) {
                parsed.signature_value.assign(sig_bits.begin() + 1,
                                              sig_bits.end());
            }
        }
    }
    parsed.serial.assign(serial.begin(), serial.end());
    if (!parse_alg_id(sig_alg, parsed.signature_algorithm)) return std::nullopt;
    parsed.issuer_der.assign(issuer_tlv, issuer_end);
    parsed.subject_der.assign(subject_tlv, subject_end);
    if (!spki_seq.empty()) {
        Cursor sp{spki_seq.data(), spki_seq.data() + spki_seq.size()};
        std::span<const u8> alg_seq;
        if (!expect_tag(sp, 0x30, alg_seq)) return std::nullopt;
        if (!parse_alg_id(alg_seq, parsed.spki.algorithm)) return std::nullopt;
        std::span<const u8> bit_string;
        if (!expect_tag(sp, 0x03, bit_string)) return std::nullopt;
        if (!bit_string.empty()) {
            parsed.spki.unused_bits = bit_string[0];
            parsed.spki.key.assign(bit_string.begin() + 1, bit_string.end());
        }
    }
    parsed.issuer_attributes = parse_name_attributes(issuer);
    parsed.subject_attributes = parse_name_attributes(subject);
    parsed.issuer_simple_name = simple_display_name(parsed.issuer_attributes);
    parsed.subject_simple_name = simple_display_name(parsed.subject_attributes);
    (void)parse_validity(validity, parsed.not_before_filetime,
                         parsed.not_after_filetime);

    if (!t.empty() && *t.p == 0xa3) {
        std::span<const u8> ext_wrap;
        if (!expect_tag(t, 0xa3, ext_wrap)) return std::nullopt;
        Cursor ew{ext_wrap.data(), ext_wrap.data() + ext_wrap.size()};
        std::span<const u8> ext_seq;
        if (!expect_tag(ew, 0x30, ext_seq)) return std::nullopt;
        if (!parse_extensions(ext_seq, parsed.extensions)) return std::nullopt;
    }
    return parsed;
}

std::vector<std::string> decode_subject_alt_dns_names(
    std::span<const u8> ext_value) {
    // GeneralNames ::= SEQUENCE OF GeneralName
    // dNSName [2] IA5String
    std::vector<std::string> names;
    Cursor c{ext_value.data(), ext_value.data() + ext_value.size()};
    std::span<const u8> seq;
    if (!expect_tag(c, 0x30, seq)) return names;
    Cursor s{seq.data(), seq.data() + seq.size()};
    while (!s.empty()) {
        u8 tag = 0;
        std::span<const u8> body;
        if (!read_tag_len(s, tag, body)) break;
        if (tag == 0x82) { // [2] dNSName
            names.emplace_back(reinterpret_cast<const char*>(body.data()),
                               body.size());
        }
    }
    return names;
}

namespace {

const char* x500_key_for_oid(std::string_view oid) {
    // Microsoft Learn CertNameToStr X.500 key names (subset).
    if (oid == "2.5.4.3") return "CN";
    if (oid == "2.5.4.4") return "SN";
    if (oid == "2.5.4.5") return "SERIALNUMBER";
    if (oid == "2.5.4.6") return "C";
    if (oid == "2.5.4.7") return "L";
    if (oid == "2.5.4.8") return "S"; // Windows uses S, not ST
    if (oid == "2.5.4.10") return "O";
    if (oid == "2.5.4.11") return "OU";
    if (oid == "2.5.4.12") return "T";
    if (oid == "2.5.4.42") return "G";
    if (oid == "2.5.4.43") return "I";
    if (oid == "1.2.840.113549.1.9.1") return "E";
    return nullptr;
}

bool needs_x500_quotes(std::string_view value) {
    if (value.empty()) return true;
    if (value.front() == ' ' || value.back() == ' ') return true;
    for (char c : value) {
        if (c == ',' || c == '+' || c == '=' || c == '"' || c == '<' ||
            c == '>' || c == '#' || c == ';' || c == '\n') {
            return true;
        }
    }
    return false;
}

std::string quote_x500_value(std::string_view value) {
    std::string out;
    out.push_back('"');
    for (char c : value) {
        if (c == '"') out.push_back('"');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

std::string format_distinguished_name(std::span<const u8> name_der,
                                      u32 str_type) {
    // Microsoft Learn (wincrypt!CertNameToStr): CERT_NAME_BLOB is a DER Name
    // (SEQUENCE OF RelativeDistinguishedName). Strip the outer SEQUENCE when
    // present so the RDN SET walker can run.
    std::span<const u8> rdn_seq = name_der;
    if (!name_der.empty() && name_der[0] == 0x30) {
        Cursor c{name_der.data(), name_der.data() + name_der.size()};
        std::span<const u8> body;
        if (expect_tag(c, 0x30, body)) rdn_seq = body;
    }
    auto attributes = parse_name_attributes(rdn_seq);
    if (attributes.empty()) return {};
    // CERT_NAME_STR_REVERSE_FLAG: the RDNs in reverse order (Learn,
    // CertNameToStr; wintrust_signer_name_probe RDN(strType=0x2000003)).
    if ((str_type & 0x02000000u) != 0) std::reverse(attributes.begin(), attributes.end());

    const u32 format = str_type & 0xffffu;
    const bool simple = format == 1u; // CERT_SIMPLE_NAME_STR
    const bool oid_form = format == 2u; // CERT_OID_NAME_STR
    // CERT_X500_NAME_STR (3) and unknown low values use X.500 key names.
    const bool no_quote = (str_type & 0x10000000u) != 0;
    const bool semicolon = (str_type & 0x40000000u) != 0;
    const bool crlf = (str_type & 0x08000000u) != 0;
    const char* sep = crlf ? "\r\n" : (semicolon ? "; " : ", ");

    std::string out;
    for (usize i = 0; i < attributes.size(); ++i) {
        if (i != 0) out += sep;
        const auto& attr = attributes[i];
        if (simple) {
            // Values keep CertNameToStr quoting in the simple form too
            // (wintrust_signer_name_probe.expected: '"Rockstar Games, Inc."').
            out += !no_quote && needs_x500_quotes(attr.value) ? quote_x500_value(attr.value)
                                                              : attr.value;
            continue;
        }
        if (oid_form) {
            out += attr.oid;
        } else if (const char* key = x500_key_for_oid(attr.oid)) {
            out += key;
        } else {
            out += "OID.";
            out += attr.oid;
        }
        out.push_back('=');
        if (!no_quote && needs_x500_quotes(attr.value)) {
            out += quote_x500_value(attr.value);
        } else {
            out += attr.value;
        }
    }
    return out;
}

std::vector<u8> certificate_key_identifier(const ParsedCertificate& cert) {
    for (const auto& ext : cert.extensions) {
        if (ext.oid != "2.5.29.14") continue;
        // Extension OCTET STRING holds KeyIdentifier ::= OCTET STRING.
        std::span<const u8> value{ext.value};
        if (value.size() >= 2 && value[0] == 0x04) {
            Cursor c{value.data(), value.data() + value.size()};
            std::span<const u8> inner;
            if (expect_tag(c, 0x04, inner)) {
                return {inner.begin(), inner.end()};
            }
        }
        return ext.value;
    }
    // No SKI extension: callers may SHA-1 hash spki.key (RFC 5280 method 1 /
    // Microsoft Learn CERT_KEY_IDENTIFIER_PROP_ID fallback).
    return {};
}

std::string oid_to_string(std::span<const u8> oid) {
    if (oid.empty()) return {};
    std::string out;
    // The first subidentifier encodes two arcs (X.690 8.19.4).
    u64 value = 0;
    usize i = 0;
    for (; i < oid.size(); ++i) {
        value = (value << 7) | (oid[i] & 0x7f);
        if ((oid[i] & 0x80) == 0) break;
    }
    const u64 first = value < 40 ? 0 : value < 80 ? 1 : 2;
    out += std::to_string(first);
    out.push_back('.');
    out += std::to_string(value - first * 40);
    value = 0;
    for (++i; i < oid.size(); ++i) {
        value = (value << 7) | (oid[i] & 0x7f);
        if ((oid[i] & 0x80) == 0) {
            out.push_back('.');
            out += std::to_string(value);
            value = 0;
        }
    }
    return out;
}

std::vector<u8> encode_oid(std::string_view dotted) {
    std::vector<u64> arcs;
    u64 value = 0;
    bool digit = false;
    for (const char c : dotted) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<u64>(c - '0');
            digit = true;
        } else if (c == '.' && digit) {
            arcs.push_back(value);
            value = 0;
            digit = false;
        } else {
            return {};
        }
    }
    if (!digit) return {};
    arcs.push_back(value);
    if (arcs.size() < 2 || arcs[0] > 2 || (arcs[0] < 2 && arcs[1] >= 40)) {
        return {};
    }
    std::vector<u8> out;
    auto append = [&](u64 arc) {
        u8 bytes[10];
        int n = 0;
        do { bytes[n++] = static_cast<u8>(arc & 0x7f); arc >>= 7; } while (arc);
        while (n > 0) {
            --n;
            out.push_back(static_cast<u8>(bytes[n] | (n ? 0x80 : 0)));
        }
    };
    append(arcs[0] * 40 + arcs[1]);
    for (usize i = 2; i < arcs.size(); ++i) append(arcs[i]);
    return out;
}

namespace {

void append_der_length(std::vector<u8>& out, usize length) {
    if (length < 0x80) {
        out.push_back(static_cast<u8>(length));
        return;
    }
    u8 bytes[8];
    int n = 0;
    while (length) { bytes[n++] = static_cast<u8>(length & 0xff); length >>= 8; }
    out.push_back(static_cast<u8>(0x80 | n));
    while (n > 0) out.push_back(bytes[--n]);
}

/// Result of reading one BER element for the decode-status model.
enum class Read : u8 { Ok, Missing, Eod };

Read read_element(Cursor& c, u8& tag, std::span<const u8>& content) {
    if (c.empty()) return Read::Missing;
    return read_tag_len(c, tag, content) ? Read::Ok : Read::Eod;
}

/// A required field inside a TBS structure: running out is ASN1_EOD, a
/// different tag ASN1_BADTAG.
u32 required(Cursor& c, u8 want, std::span<const u8>& content) {
    u8 tag = 0;
    switch (read_element(c, tag, content)) {
        case Read::Missing:
        case Read::Eod: return kCryptEAsn1Eod;
        case Read::Ok: break;
    }
    return tag == want ? 0u : kCryptEAsn1BadTag;
}

u32 algorithm_identifier_error(std::span<const u8> content) {
    Cursor a{content.data(), content.data() + content.size()};
    std::span<const u8> oid;
    return required(a, 0x06, oid);
}

} // namespace

u32 certificate_decode_error(std::span<const u8> der) {
    if (der.empty()) return kCryptEAsn1Eod;
    Cursor top{der.data(), der.data() + der.size()};
    u8 tag = 0;
    std::span<const u8> cert;
    if (read_element(top, tag, cert) != Read::Ok) return kCryptEAsn1Eod;
    if (tag != 0x30) return kCryptEAsn1BadTag;

    // Signed-content wrapper: toBeSigned, signatureAlgorithm, signature.
    // Every wrapper element that is missing or mistagged is ASN1_BADTAG;
    // elements after the signature are ignored.
    Cursor wrapper{cert.data(), cert.data() + cert.size()};
    std::span<const u8> tbs;
    std::span<const u8> outer_alg;
    std::span<const u8> signature;
    const std::array<std::pair<u8, std::span<const u8>*>, 3> parts{{
        {0x30, &tbs}, {0x30, &outer_alg}, {0x03, &signature}}};
    for (const auto& [want, out] : parts) {
        const Read r = read_element(wrapper, tag, *out);
        if (r == Read::Eod) return kCryptEAsn1Eod;
        if (r == Read::Missing || tag != want) return kCryptEAsn1BadTag;
    }
    if (const u32 error = algorithm_identifier_error(outer_alg)) return error;

    Cursor t{tbs.data(), tbs.data() + tbs.size()};
    std::span<const u8> field;
    if (!t.empty() && *t.p == 0xa0) {
        if (read_element(t, tag, field) != Read::Ok) return kCryptEAsn1Eod;
    }
    if (const u32 error = required(t, 0x02, field)) return error;       // serial
    if (const u32 error = required(t, 0x30, field)) return error;       // signature
    if (const u32 error = algorithm_identifier_error(field)) return error;
    if (const u32 error = required(t, 0x30, field)) return error;       // issuer
    if (const u32 error = required(t, 0x30, field)) return error;       // validity
    {
        Cursor v{field.data(), field.data() + field.size()};
        for (int i = 0; i < 2; ++i) {
            std::span<const u8> time;
            const Read r = read_element(v, tag, time);
            if (r == Read::Eod) return kCryptEAsn1Eod;
            if (r == Read::Missing) return kCryptEAsn1Corrupt;
            if (tag != 0x17 && tag != 0x18) return kCryptEAsn1BadTag;
            u64 filetime = 0;
            if (!asn1_time_to_filetime(tag, time, filetime)) return kCryptEBadEncode;
        }
    }
    if (const u32 error = required(t, 0x30, field)) return error;       // subject
    if (const u32 error = required(t, 0x30, field)) return error;       // spki
    {
        Cursor k{field.data(), field.data() + field.size()};
        std::span<const u8> algorithm;
        if (const u32 error = required(k, 0x30, algorithm)) return error;
        if (const u32 error = algorithm_identifier_error(algorithm)) return error;
        std::span<const u8> key;
        if (const u32 error = required(k, 0x03, key)) return error;
    }
    // Optional [1], [2] unique IDs then [3] extensions, in that order.
    const u8 optional_tags[3][2] = {{0x81, 0xa1}, {0x82, 0xa2}, {0xa3, 0xa3}};
    for (const auto& allowed : optional_tags) {
        if (t.empty()) return 0;
        if (*t.p != allowed[0] && *t.p != allowed[1]) continue;
        if (read_element(t, tag, field) != Read::Ok) return kCryptEAsn1Eod;
        if (tag != 0xa3) continue;
        Cursor e{field.data(), field.data() + field.size()};
        std::span<const u8> sequence;
        if (const u32 error = required(e, 0x30, sequence)) return error;
        Cursor list{sequence.data(), sequence.data() + sequence.size()};
        while (!list.empty()) {
            std::span<const u8> extension;
            if (const u32 error = required(list, 0x30, extension)) return error;
            Cursor x{extension.data(), extension.data() + extension.size()};
            std::span<const u8> part;
            if (const u32 error = required(x, 0x06, part)) return error;
            if (!x.empty() && *x.p == 0x01) {
                if (read_element(x, tag, part) != Read::Ok) return kCryptEAsn1Eod;
            }
            if (const u32 error = required(x, 0x04, part)) return error;
        }
    }
    // Anything left after the last optional field is not part of TBSCertificate.
    return t.empty() ? 0u : kCryptEAsn1Corrupt;
}

u32 decode_enhanced_key_usage(std::span<const u8> encoded,
                              std::vector<std::string>& oids) {
    oids.clear();
    Cursor c{encoded.data(), encoded.data() + encoded.size()};
    std::span<const u8> sequence;
    if (const u32 error = required(c, 0x30, sequence)) return error;
    Cursor list{sequence.data(), sequence.data() + sequence.size()};
    while (!list.empty()) {
        std::span<const u8> oid;
        if (const u32 error = required(list, 0x06, oid)) return error;
        oids.push_back(oid_to_string(oid));
    }
    return 0;
}

std::vector<u8> encode_enhanced_key_usage(const std::vector<std::string>& oids) {
    std::vector<u8> body;
    for (const auto& text : oids) {
        const std::vector<u8> oid = encode_oid(text);
        body.push_back(0x06);
        append_der_length(body, oid.size());
        body.insert(body.end(), oid.begin(), oid.end());
    }
    std::vector<u8> out{0x30};
    append_der_length(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

u32 public_key_bit_length(const PublicKeyInfo& key) {
    if (key.algorithm.oid == "1.2.840.113549.1.1.1") {
        // RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
        Cursor c{key.key.data(), key.key.data() + key.key.size()};
        std::span<const u8> sequence;
        if (!expect_tag(c, 0x30, sequence)) return 0;
        Cursor s{sequence.data(), sequence.data() + sequence.size()};
        std::span<const u8> modulus;
        if (!expect_tag(s, 0x02, modulus)) return 0;
        usize first = 0;
        while (first < modulus.size() && modulus[first] == 0) ++first;
        if (first == modulus.size()) return 0;
        u32 bits = static_cast<u32>(modulus.size() - first - 1) * 8;
        for (u8 top = modulus[first]; top; top >>= 1) ++bits;
        return bits;
    }
    if (key.algorithm.oid == "1.2.840.10045.2.1") {
        Cursor c{key.algorithm.parameters.data(),
                 key.algorithm.parameters.data() + key.algorithm.parameters.size()};
        std::span<const u8> curve;
        if (!expect_tag(c, 0x06, curve)) return 0;
        const std::string name = oid_to_string(curve);
        if (name == "1.2.840.10045.3.1.7") return 256;  // secp256r1
        if (name == "1.3.132.0.34") return 384;         // secp384r1
        if (name == "1.3.132.0.35") return 521;         // secp521r1
    }
    return 0;
}

std::vector<std::string> decode_certificate_policies(std::span<const u8> ext_value) {
    std::vector<std::string> out;
    Cursor c{ext_value.data(), ext_value.data() + ext_value.size()};
    std::span<const u8> sequence;
    if (!expect_tag(c, 0x30, sequence)) return out;
    Cursor list{sequence.data(), sequence.data() + sequence.size()};
    while (!list.empty()) {
        std::span<const u8> info;
        if (!expect_tag(list, 0x30, info)) break;
        Cursor i{info.data(), info.data() + info.size()};
        std::span<const u8> oid;
        if (!expect_tag(i, 0x06, oid)) break;
        out.push_back(oid_to_string(oid));
    }
    return out;
}

AuthorityKeyId decode_authority_key_id(std::span<const u8> ext_value) {
    AuthorityKeyId out;
    Cursor c{ext_value.data(), ext_value.data() + ext_value.size()};
    std::span<const u8> sequence;
    if (!expect_tag(c, 0x30, sequence)) return out;
    out.present = true;
    Cursor fields{sequence.data(), sequence.data() + sequence.size()};
    bool issuer = false;
    bool serial = false;
    while (!fields.empty()) {
        u8 tag = 0;
        std::span<const u8> value;
        if (!read_tag_len(fields, tag, value)) break;
        if (tag == 0x80) out.key_id.assign(value.begin(), value.end());
        if (tag == 0xa1) issuer = true;
        if (tag == 0x82) serial = true;
    }
    out.has_issuer_serial = issuer && serial;
    return out;
}

bool has_revocation_urls(const ParsedCertificate& cert) {
    for (const auto& extension : cert.extensions) {
        if (extension.oid == "2.5.29.31") return true;
        if (extension.oid != "1.3.6.1.5.5.7.1.1") continue;
        Cursor c{extension.value.data(), extension.value.data() + extension.value.size()};
        std::span<const u8> sequence;
        if (!expect_tag(c, 0x30, sequence)) continue;
        Cursor list{sequence.data(), sequence.data() + sequence.size()};
        while (!list.empty()) {
            std::span<const u8> access;
            if (!expect_tag(list, 0x30, access)) break;
            Cursor a{access.data(), access.data() + access.size()};
            std::span<const u8> method;
            if (expect_tag(a, 0x06, method) &&
                oid_to_string(method) == "1.3.6.1.5.5.7.48.1") {
                return true;
            }
        }
    }
    return false;
}

BasicConstraints basic_constraints(const ParsedCertificate& cert) {
    BasicConstraints out;
    for (const auto& extension : cert.extensions) {
        if (extension.oid != "2.5.29.19") continue;
        out.present = true;
        Cursor c{extension.value.data(), extension.value.data() + extension.value.size()};
        std::span<const u8> sequence;
        if (!expect_tag(c, 0x30, sequence)) return out;
        Cursor fields{sequence.data(), sequence.data() + sequence.size()};
        if (!fields.empty() && *fields.p == 0x01) {
            std::span<const u8> flag;
            if (expect_tag(fields, 0x01, flag)) out.ca = !flag.empty() && flag[0] != 0;
        }
        if (!fields.empty() && *fields.p == 0x02) {
            std::span<const u8> length;
            if (expect_tag(fields, 0x02, length)) {
                u32 value = 0;
                for (const u8 byte : length) value = (value << 8) | byte;
                out.path_length = value;
            }
        }
        return out;
    }
    return out;
}

std::vector<GeneralName> decode_general_names(std::span<const u8> ext_value) {
    std::vector<GeneralName> out;
    Cursor c{ext_value.data(), ext_value.data() + ext_value.size()};
    std::span<const u8> sequence;
    if (!expect_tag(c, 0x30, sequence)) return out;
    Cursor list{sequence.data(), sequence.data() + sequence.size()};
    while (!list.empty()) {
        u8 tag = 0;
        std::span<const u8> value;
        if (!read_tag_len(list, tag, value)) break;
        GeneralName name;
        name.choice = static_cast<u8>(tag & 0x1f);
        if (name.choice == 0) {
            Cursor o{value.data(), value.data() + value.size()};
            std::span<const u8> type;
            if (expect_tag(o, 0x06, type)) name.other_oid = oid_to_string(type);
            name.value.assign(o.p, o.end);
        } else {
            name.value.assign(value.begin(), value.end());
        }
        out.push_back(std::move(name));
    }
    return out;
}

} // namespace vitreo::winapi::x509

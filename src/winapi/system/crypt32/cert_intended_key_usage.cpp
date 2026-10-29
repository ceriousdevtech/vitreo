// SPDX-License-Identifier: LGPL-2.1-only
// CertGetIntendedKeyUsage: Microsoft Learn wincrypt contract, with Windows
// decoding/precedence observations in tools/oracle_probe/cert_usage_probe.c.
#include "crypt32_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <algorithm>
#include <span>
#include <vector>

namespace vitreo::winapi::crypt32 {
namespace {
#include "cert_usage_abi.inc"
using Bytes = std::span<const u8>;
struct Element { u8 tag; Bytes value; };

// BER definite and indefinite lengths. Iterative EOC scanning avoids native
// recursion for attacker-controlled certificate nesting.
u32 element(Bytes& input, Element& out) {
    if (input.size() < 2) return asn_eod;
    out.tag = input[0];
    usize pos = 2, size = input[1];
    if (size == 0x80) {
        if (!(out.tag & 0x20)) return asn_corrupt;
        usize cursor = pos, depth = 1;
        while (depth) {
            if (input.size() - cursor < 2) return asn_eod;
            const u8 tag = input[cursor++], len = input[cursor++];
            if (tag == 0 && len == 0) { --depth; continue; }
            if (len == 0x80) {
                if (!(tag & 0x20)) return asn_corrupt;
                ++depth; continue;
            }
            usize n = len;
            if (len & 0x80) {
                const unsigned bytes = len & 0x7f;
                if (bytes > sizeof(usize) || bytes > input.size() - cursor) return asn_eod;
                n = 0;
                for (unsigned i = 0; i < bytes; ++i) n = (n << 8) | input[cursor++];
            }
            if (n > input.size() - cursor) return asn_eod;
            cursor += n;
        }
        out.value = input.subspan(pos, cursor - pos - 2);
        input = input.subspan(cursor);
        return 0;
    }
    if (size & 0x80) {
        const unsigned bytes = size & 0x7f;
        if (bytes > sizeof(usize) || bytes > input.size() - pos) return asn_eod;
        size = 0;
        for (unsigned i = 0; i < bytes; ++i) size = (size << 8) | input[pos++];
    }
    if (size > input.size() - pos) return asn_eod;
    out.value = input.subspan(pos, size);
    input = input.subspan(pos + size);
    return 0;
}

u32 bit_string(Element item, std::vector<u8>& bytes) {
    while (item.tag == 0x23) {
        auto contents = item.value;
        if (auto error = element(contents, item)) return error;
        // The Windows decoder accepts a nested single BIT STRING, but rejects
        // the multiple-fragment encoding (measured even with zero unused bits).
        if (!contents.empty()) return asn_corrupt;
    }
    if (item.tag != 3) return asn_badtag;
    if (item.value.empty()) return asn_eod;
    const unsigned unused = item.value.front();
    if (unused > 7 || (item.value.size() == 1 && unused)) return asn_corrupt;
    bytes.assign(item.value.begin() + 1, item.value.end());
    if (!bytes.empty()) bytes.back() &= static_cast<u8>(0xffu << unused);
    return 0;
}

u32 generalized_time(Bytes text) {
    // Windows accepts hour/minute precision, fractions and timezone offsets;
    // malformed syntax differs from an impossible calendar value.
    usize digits = 0;
    while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') ++digits;
    if (digits != 10 && digits != 12 && digits != 14) return asn_corrupt;
    const auto number = [&](usize at, usize n) {
        unsigned value = 0;
        for (usize i = 0; i < n; ++i) value = value * 10 + text[at+i] - '0';
        return value;
    };
    const auto year = number(0,4), month = number(4,2), day = number(6,2);
    if (year < 1601 || month < 1 || month > 12 || day < 1 || number(8,2) > 23 ||
        (digits >= 12 && number(10,2) > 59) || (digits == 14 && number(12,2) > 59)) return bad_encode;
    constexpr unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month-1] + (month == 2 && leap)) return bad_encode;
    usize tail = digits;
    if (tail < text.size() && (text[tail] == '.' || text[tail] == ',')) {
        ++tail;
        while (tail < text.size() && text[tail] >= '0' && text[tail] <= '9') ++tail;
    }
    if (tail == text.size()) return 0;
    if (text[tail] == 'Z' && tail + 1 == text.size()) return 0;
    if ((text[tail] == '+' || text[tail] == '-') &&
        (tail + 3 == text.size() || tail + 5 == text.size())) {
        for (usize i = tail+1; i < text.size(); ++i) if (text[i] < '0' || text[i] > '9') return asn_corrupt;
        // Windows normalizes numeric offsets, including +2400 and -9999;
        // offset digits do not have the calendar hour/minute range checks.
        return 0;
    }
    return asn_corrupt;
}

u32 decode(Bytes encoded, bool legacy, std::vector<u8>& bytes) {
    Element item{};
    if (auto error = element(encoded, item)) return error;
    if (!legacy) return bit_string(item, bytes);
    if (item.tag != 0x30) return asn_badtag;
    auto fields = item.value;
    unsigned order = 0;
    while (!fields.empty()) {
        if (auto error = element(fields, item)) return error;
        if ((item.tag == 4 || item.tag == 0x24) && order == 0) {
            while (item.tag == 0x24) {
                auto contents = item.value;
                if (auto error = element(contents, item)) return error;
                if (!contents.empty()) return asn_corrupt;
            }
            if (item.tag != 4) return asn_badtag;
            order = 1; // optional key identifier, not a key-usage byte
        } else if ((item.tag == 3 || item.tag == 0x23) && order <= 1) {
            if (auto error = bit_string(item, bytes)) return error;
            order = 2;
        } else if (item.tag == 0x30 && order <= 2) {
            auto times = item.value;
            unsigned previous = 0;
            while (!times.empty()) {
                Element time{};
                if (auto error = element(times, time)) return error;
                if ((time.tag != 0x80 && time.tag != 0x81) || time.tag < previous) return asn_corrupt;
                previous = time.tag + 1;
                if (auto error = generalized_time(time.value)) return error;
            }
            order = 3;
        } else return asn_corrupt;
    }
    return 0;
}

void access_violation(Marshaler& m, GuestAddr address, bool write) {
    if (auto* cpu = m.cpu()) {
        auto& fault = cpu->pending_raise;
        fault.active = true; fault.from_helper = false;
        fault.code = access_violation_status; fault.flags = 0; fault.num_args = 2;
        fault.args[0] = write ? 1 : 0; fault.args[1] = address;
    }
}
} // namespace

void impl_CertGetIntendedKeyUsage(Marshaler& m) {
    const u32 encoding = m.arg32(0), capacity = m.arg32(3);
    const GuestAddr info = m.arg_ptr(1), output = m.arg_ptr(2);
    const auto& layout = m.cpu_is_64bit() ? usage_layout_64 : usage_layout_32;
    const auto count = m.read_u32(info + layout.count);
    if (!count.is_ok()) { access_violation(m, info + layout.count, false); return; }
    const auto extensions = m.read_ptr(info + layout.extensions);
    if (!extensions.is_ok()) { access_violation(m, info + layout.extensions, false); return; }
    for (u32 i = 0; i < capacity; ++i) if (!m.write_u8(output + i, 0).is_ok()) {
        access_violation(m, output + i, true); return;
    }
    bool fault = false;
    auto find = [&](const char* oid) -> GuestAddr {
        for (u32 i = 0; i < count.value(); ++i) {
            const GuestAddr entry = extensions.value() + static_cast<u64>(i) * layout.stride;
            const auto pointer = m.read_ptr(entry);
            if (!pointer.is_ok()) { access_violation(m, entry, false); fault = true; return 0; }
            const auto name = m.read_ansi_string(pointer.value());
            if (!name.is_ok()) { access_violation(m, pointer.value(), false); fault = true; return 0; }
            if (name.value() == oid) return entry;
        }
        return 0;
    };
    std::vector<u8> usage;
    u32 error = 0;
    bool decoded = false;
    for (bool legacy : {false, true}) {
        const GuestAddr entry = find(legacy ? key_attributes_oid : key_usage_oid);
        if (fault) return;
        error = 0;
        if (!entry) continue;
        const u32 cert_encoding = encoding & cert_encoding_mask;
        if (cert_encoding != x509_encoding &&
            (cert_encoding != 0 || (encoding & msg_encoding_mask) != pkcs7_encoding)) {
            error = encoding_not_found; continue;
        }
        const auto size = m.read_u32(entry + layout.blob);
        const auto pointer = m.read_ptr(entry + layout.blob + layout.data);
        if (!size.is_ok() || !pointer.is_ok()) { access_violation(m, entry + layout.blob, false); return; }
        if (size.value() && !m.memory().is_mapped(pointer.value(), size.value())) {
            access_violation(m, pointer.value(), false); return;
        }
        std::vector<u8> encoded(size.value());
        if (!m.memory().read_bytes(pointer.value(), encoded.data(), encoded.size()).is_ok()) {
            access_violation(m, pointer.value(), false); return;
        }
        usage.clear();
        error = decode(encoded, legacy, usage);
        if (!error) { decoded = true; break; }
    }
    const u32 copied = decoded ? static_cast<u32>(std::min<usize>(capacity, usage.size())) : 0;
    for (u32 i = 0; i < copied; ++i) if (!m.write_u8(output + i, usage[i]).is_ok()) {
        access_violation(m, output + i, true); return;
    }
    set_last_error(error);
    m.set_return(copied != 0);
}
} // namespace vitreo::winapi::crypt32

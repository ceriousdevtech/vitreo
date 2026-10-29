// SPDX-License-Identifier: LGPL-2.1-only
/// @file pfx_is_pfx_blob.cpp
/// @brief crypt32!PFXIsPFXBlob: does the outer layer of a blob decode as a
///        PKCS#12 PFX PDU?
///
/// Learn (nf-wincrypt-pfxispfxblob): BOOL PFXIsPFXBlob(CRYPT_DATA_BLOB* pPFX)
/// "returns TRUE if the BLOB can be decoded as a PFX packet"; FALSE when "the
/// outer layer of the BLOB cannot be decoded as a PFX packet".
///
/// The PFX outer layer is (RFC 7292 section 4):
///   PFX ::= SEQUENCE { version INTEGER, authSafe ContentInfo,
///                      macData MacData OPTIONAL }
///   ContentInfo ::= SEQUENCE { contentType OBJECT IDENTIFIER,
///                              content [0] EXPLICIT ANY OPTIONAL }
///   MacData ::= SEQUENCE { mac DigestInfo, macSalt OCTET STRING,
///                          iterations INTEGER DEFAULT 1 }
///   DigestInfo ::= SEQUENCE { digestAlgorithm AlgorithmIdentifier,
///                             digest OCTET STRING }
///   AlgorithmIdentifier ::= SEQUENCE { algorithm OBJECT IDENTIFIER,
///                                      parameters ANY OPTIONAL }
///
/// The decoding rules below are measured, not documented:
/// tests/winapi/crypt32/oracle/pfx_is_pfx_blob_probe.c (x86 and x64 agree).
///  - BER, not DER: indefinite lengths on constructed elements, non-minimal
///    long-form lengths (at most 4 length octets), high-tag-number form,
///    constructed OCTET STRINGs. 0xFF and 5+ length octets fail.
///  - The version value is not checked (0, 2, 4, negative all decode); an
///    INTEGER of 1..4 content octets decodes, 0 or 5+ octets fails. The same
///    holds for iterations.
///  - contentType and the content are not interpreted (any OID, any single
///    element inside [0]); an OID whose last octet has bit 7 set fails.
///  - Bytes after the outer PFX element are ignored; elements after macData
///    inside it fail.
///  - pbData NULL (any cbData) or cbData 0 is FALSE. Otherwise the decoder
///    reads only the bytes it needs: an unreadable needed byte faults
///    (STATUS_ACCESS_VIOLATION), bytes past the PFX element are never read.
///  - The last error is 0 on every return.

#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "winapi/system/crypt32/pfx_is_pfx_blob.hpp"

#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <optional>
#include <vector>

namespace vitreo::winapi {
namespace {

constexpr u32 kClassUniversal = 0;
constexpr u32 kClassContext = 2;
constexpr u32 kTagInteger = 2;
constexpr u32 kTagOctetString = 4;
constexpr u32 kTagOid = 6;
constexpr u32 kTagSequence = 16;

class BerReader {
public:
    BerReader(std::span<const u8> readable, u64 declared)
        : m_data(readable), m_declared(declared) {}

    struct Header {
        u32 cls{0};
        bool constructed{false};
        u32 number{0};
        bool indefinite{false};
        u64 content{0};  ///< offset of the first content octet
        u64 length{0};   ///< content length when definite
    };

    // A region of content octets: definite [pos, end) or indefinite (ends
    // at an end-of-contents 00 00 before `end`).
    struct Region {
        u64 pos;
        u64 end;
        bool indefinite;
    };

    [[nodiscard]] std::optional<u8> byte(u64 at, u64 end) {
        if (at >= end || at >= m_declared) return std::nullopt;
        if (at >= m_data.size()) {
            // A needed byte the guest cannot read: Windows faults here. Every
            // caller turns nullopt into failure, so the fault propagates.
            if (!m_fault) m_fault = at;
            return std::nullopt;
        }
        return m_data[at];
    }

    [[nodiscard]] std::optional<u64> fault() const { return m_fault; }

    // Read an identifier and length at `pos` within `end`.
    [[nodiscard]] std::optional<Header> header(u64 pos, u64 end) {
        Header h;
        auto b = byte(pos++, end);
        if (!b) return std::nullopt;
        h.cls = *b >> 6;
        h.constructed = (*b & 0x20) != 0;
        h.number = *b & 0x1F;
        if (h.number == 0x1F) {
            u64 number = 0;
            for (;;) {
                auto t = byte(pos++, end);
                if (!t) return std::nullopt;
                number = (number << 7) | (*t & 0x7F);
                if (number > 0xFFFFFFFFull) return std::nullopt;
                if ((*t & 0x80) == 0) break;
            }
            h.number = static_cast<u32>(number);
        }
        auto l = byte(pos++, end);
        if (!l) return std::nullopt;
        if (*l == 0x80) {
            if (!h.constructed) return std::nullopt;
            h.indefinite = true;
        } else if (*l & 0x80) {
            const u32 count = *l & 0x7F;
            if (count > 4) return std::nullopt;
            u64 length = 0;
            for (u32 i = 0; i < count; ++i) {
                auto o = byte(pos++, end);
                if (!o) return std::nullopt;
                length = (length << 8) | *o;
            }
            h.length = length;
        } else {
            h.length = *l;
        }
        h.content = pos;
        if (!h.indefinite && (h.length > end - pos)) return std::nullopt;
        return h;
    }

    [[nodiscard]] Region contents(const Header& h, u64 end) const {
        return h.indefinite ? Region{h.content, end, true}
                            : Region{h.content, h.content + h.length, false};
    }

    // True when no element remains in `r` (consumes the EOC of an
    // indefinite region, which is then required).
    [[nodiscard]] std::optional<bool> at_end(Region& r) {
        if (!r.indefinite) return r.pos == r.end;
        auto a = byte(r.pos, r.end);
        if (!a) return std::nullopt;  // indefinite with no end-of-contents
        if (*a != 0) return false;
        auto b = byte(r.pos + 1, r.end);
        if (!b) return std::nullopt;
        if (*b != 0) return false;
        r.pos += 2;
        return true;
    }

    // Skip one complete element (ANY) at r.pos.
    [[nodiscard]] std::optional<Header> skip(Region& r) {
        auto h = header(r.pos, r.end);
        if (!h) return std::nullopt;
        if (h->indefinite) {
            Region inner = contents(*h, r.end);
            for (;;) {
                auto done = at_end(inner);
                if (!done) return std::nullopt;
                if (*done) break;
                if (!skip(inner)) return std::nullopt;
            }
            r.pos = inner.pos;
        } else {
            r.pos = h->content + h->length;
        }
        return h;
    }

    // Enter the constructed element at r.pos with the given tag; advances r
    // past it and returns its contents.
    [[nodiscard]] std::optional<Region> enter(Region& r, u32 cls, u32 number) {
        const u64 start = r.pos;
        auto h = header(r.pos, r.end);
        if (!h || !h->constructed || h->cls != cls || h->number != number)
            return std::nullopt;
        Region probe{start, r.end, r.indefinite};
        if (!skip(probe)) return std::nullopt;
        r.pos = probe.pos;
        return contents(*h, probe.pos);
    }

    // INTEGER that fits in 32 bits (1..4 content octets).
    [[nodiscard]] bool integer(Region& r) {
        auto h = header(r.pos, r.end);
        if (!h || h->cls != kClassUniversal || h->constructed ||
            h->number != kTagInteger || h->length < 1 || h->length > 4)
            return false;
        r.pos = h->content + h->length;
        return true;
    }

    [[nodiscard]] bool oid(Region& r) {
        auto h = header(r.pos, r.end);
        if (!h || h->cls != kClassUniversal || h->constructed ||
            h->number != kTagOid)
            return false;
        if (h->length != 0) {
            auto last = byte(h->content + h->length - 1, r.end);
            if (!last || (*last & 0x80)) return false;
        }
        r.pos = h->content + h->length;
        return true;
    }

    // OCTET STRING, primitive or constructed from OCTET STRING segments.
    [[nodiscard]] bool octet_string(Region& r) {
        auto h = header(r.pos, r.end);
        if (!h || h->cls != kClassUniversal || h->number != kTagOctetString)
            return false;
        if (!h->constructed) {
            r.pos = h->content + h->length;
            return true;
        }
        auto inner = enter(r, kClassUniversal, kTagOctetString);
        if (!inner) return false;
        for (;;) {
            auto done = at_end(*inner);
            if (!done) return false;
            if (*done) return true;
            if (!octet_string(*inner)) return false;
        }
    }

private:
    std::span<const u8> m_data;
    u64 m_declared;
    std::optional<u64> m_fault;
};

bool content_info(BerReader& rd, BerReader::Region& r) {
    auto ci = rd.enter(r, kClassUniversal, kTagSequence);
    if (!ci || !rd.oid(*ci)) return false;
    auto done = rd.at_end(*ci);
    if (!done) return false;
    if (*done) return true;
    auto explicit0 = rd.enter(*ci, kClassContext, 0);
    if (!explicit0) return false;
    // [0] EXPLICIT: exactly one element.
    if (!rd.skip(*explicit0)) return false;
    auto inner_done = rd.at_end(*explicit0);
    if (!inner_done || !*inner_done) return false;
    auto ci_done = rd.at_end(*ci);
    return ci_done && *ci_done;
}

bool algorithm_identifier(BerReader& rd, BerReader::Region& r) {
    auto alg = rd.enter(r, kClassUniversal, kTagSequence);
    if (!alg || !rd.oid(*alg)) return false;
    auto done = rd.at_end(*alg);
    if (!done) return false;
    if (*done) return true;
    if (!rd.skip(*alg)) return false;
    auto end = rd.at_end(*alg);
    return end && *end;
}

bool mac_data(BerReader& rd, BerReader::Region& r) {
    auto mac = rd.enter(r, kClassUniversal, kTagSequence);
    if (!mac) return false;
    auto digest_info = rd.enter(*mac, kClassUniversal, kTagSequence);
    if (!digest_info || !algorithm_identifier(rd, *digest_info) ||
        !rd.octet_string(*digest_info))
        return false;
    auto di_done = rd.at_end(*digest_info);
    if (!di_done || !*di_done) return false;
    if (!rd.octet_string(*mac)) return false;
    auto done = rd.at_end(*mac);
    if (!done) return false;
    if (*done) return true;
    if (!rd.integer(*mac)) return false;
    auto end = rd.at_end(*mac);
    return end && *end;
}

bool pfx(BerReader& rd, u64 size) {
    BerReader::Region all{0, size, false};
    auto body = rd.enter(all, kClassUniversal, kTagSequence);
    if (!body || !rd.integer(*body) || !content_info(rd, *body)) return false;
    auto done = rd.at_end(*body);
    if (!done) return false;
    if (*done) return true;
    if (!mac_data(rd, *body)) return false;
    auto end = rd.at_end(*body);
    return end && *end;
}

} // namespace

namespace crypt32 {

PfxBlobResult classify_pfx_blob(std::span<const u8> readable, u64 declared_size) {
    if (declared_size == 0) return {PfxBlobResult::Kind::NotPfx, 0};
    BerReader rd(readable, declared_size);
    const bool ok = pfx(rd, declared_size);
    if (rd.fault()) return {PfxBlobResult::Kind::Fault, *rd.fault()};
    return {ok ? PfxBlobResult::Kind::Pfx : PfxBlobResult::Kind::NotPfx, 0};
}

void impl_PFXIsPFXBlob(Marshaler& m) {
    const GuestAddr blob = m.arg_ptr(0);
    const auto size = m.read_u32(blob);
    const auto data = m.read_ptr(blob + (m.cpu_is_64bit() ? 8 : 4));
    if (size.is_err() || data.is_err()) {
        raise_access_violation(m, size.is_err() ? blob : blob + 4, false);
        m.set_return(0);
        return;
    }
    const u32 cb = size.value();
    const GuestAddr pb = data.value();
    set_last_error(error_codes::ERROR_SUCCESS);
    if (pb == 0 || cb == 0) {
        m.set_return(0);
        return;
    }
    // Copy the readable prefix of [pb, pb + cb), a page at a time; the
    // decoder faults only if it needs a byte beyond it.
    constexpr u64 kPage = 4096;
    std::vector<u8> bytes;
    bytes.reserve(cb);
    u64 at = 0;
    while (at < cb) {
        const u64 chunk = std::min<u64>(cb - at, kPage - ((pb + at) % kPage));
        const usize old = bytes.size();
        bytes.resize(old + chunk);
        if (m.memory().read_bytes(pb + at, reinterpret_cast<Byte*>(bytes.data() + old),
                                  chunk).is_err()) {
            bytes.resize(old);
            break;
        }
        at += chunk;
    }
    const PfxBlobResult result = classify_pfx_blob(bytes, cb);
    if (result.kind == PfxBlobResult::Kind::Fault) {
        raise_access_violation(m, pb + result.fault_offset, false);
        m.set_return(0);
        return;
    }
    m.set_return(result.kind == PfxBlobResult::Kind::Pfx ? 1 : 0);
}

} // namespace crypt32
} // namespace vitreo::winapi

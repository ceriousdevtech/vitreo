// SPDX-License-Identifier: LGPL-2.1-only

#include "authenticode.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace vitreo::winapi::authenticode {
namespace {

struct DerNode {
    u8 tag{0};
    usize begin{0};
    usize content{0};
    usize size{0};

    [[nodiscard]] usize end() const noexcept { return content + size; }
};

[[nodiscard]] bool read_u16(std::span<const u8> data, usize offset, u16& value) {
    if (offset > data.size() || data.size() - offset < 2) return false;
    value = static_cast<u16>(data[offset]) |
            static_cast<u16>(static_cast<u16>(data[offset + 1]) << 8);
    return true;
}

[[nodiscard]] bool read_u32(std::span<const u8> data, usize offset, u32& value) {
    if (offset > data.size() || data.size() - offset < 4) return false;
    value = static_cast<u32>(data[offset]) |
            (static_cast<u32>(data[offset + 1]) << 8) |
            (static_cast<u32>(data[offset + 2]) << 16) |
            (static_cast<u32>(data[offset + 3]) << 24);
    return true;
}

[[nodiscard]] bool read_der_node(
    std::span<const u8> data, usize& cursor, usize limit, DerNode& node) {
    if (limit > data.size() || cursor >= limit) return false;

    node.begin = cursor;
    node.tag = data[cursor++];
    if (cursor >= limit) return false;

    u8 length_byte = data[cursor++];
    usize length = 0;
    if ((length_byte & 0x80u) == 0) {
        length = length_byte;
    } else {
        const usize count = length_byte & 0x7Fu;
        if (count == 0 || count > 4 || count > limit - cursor) return false;
        for (usize i = 0; i < count; ++i) {
            if (length > (std::numeric_limits<usize>::max() >> 8)) return false;
            length = (length << 8) | data[cursor++];
        }
    }

    if (length > limit - cursor) return false;
    node.content = cursor;
    node.size = length;
    cursor += length;
    return true;
}

[[nodiscard]] std::vector<u8> copy_full(
    std::span<const u8> data, const DerNode& node) {
    return std::vector<u8>(data.begin() + static_cast<isize>(node.begin),
                           data.begin() + static_cast<isize>(node.end()));
}

[[nodiscard]] std::vector<u8> copy_integer(
    std::span<const u8> data, const DerNode& node) {
    usize first = node.content;
    while (first + 1 < node.end() && data[first] == 0) ++first;
    return std::vector<u8>(data.begin() + static_cast<isize>(first),
                           data.begin() + static_cast<isize>(node.end()));
}

[[nodiscard]] std::string decode_der_string(
    std::span<const u8> data, const DerNode& node) {
    if (node.tag == 0x1E) {
        if ((node.size & 1u) != 0) return {};
        std::string result;
        result.reserve(node.size / 2);
        for (usize i = node.content; i < node.end(); i += 2) {
            const u16 ch = static_cast<u16>(static_cast<u16>(data[i]) << 8) |
                           static_cast<u16>(data[i + 1]);
            if (ch < 0x80) {
                result.push_back(static_cast<char>(ch));
            } else if (ch < 0x800) {
                result.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            } else {
                result.push_back(static_cast<char>(0xE0 | (ch >> 12)));
                result.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
                result.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            }
        }
        return result;
    }

    if (node.tag != 0x0C && node.tag != 0x13 && node.tag != 0x14 &&
        node.tag != 0x16) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data.data() + node.content),
                       node.size);
}

[[nodiscard]] std::string parse_common_name(
    std::span<const u8> data, const DerNode& name) {
    static constexpr u8 kCommonNameOid[] = {0x55, 0x04, 0x03};
    usize rdn_cursor = name.content;
    while (rdn_cursor < name.end()) {
        DerNode rdn;
        if (!read_der_node(data, rdn_cursor, name.end(), rdn) || rdn.tag != 0x31) {
            return {};
        }
        usize attr_cursor = rdn.content;
        while (attr_cursor < rdn.end()) {
            DerNode attribute;
            if (!read_der_node(data, attr_cursor, rdn.end(), attribute) ||
                attribute.tag != 0x30) {
                return {};
            }
            usize value_cursor = attribute.content;
            DerNode oid;
            DerNode value;
            if (!read_der_node(data, value_cursor, attribute.end(), oid) || oid.tag != 0x06 ||
                !read_der_node(data, value_cursor, attribute.end(), value)) {
                return {};
            }
            if (oid.size == std::size(kCommonNameOid) &&
                std::equal(std::begin(kCommonNameOid), std::end(kCommonNameOid),
                           data.begin() + static_cast<isize>(oid.content))) {
                return decode_der_string(data, value);
            }
        }
    }
    return {};
}

[[nodiscard]] bool parse_certificate(
    std::span<const u8> data, const DerNode& certificate_node, Certificate& result) {
    if (certificate_node.tag != 0x30) return false;
    usize cert_cursor = certificate_node.content;
    DerNode tbs;
    if (!read_der_node(data, cert_cursor, certificate_node.end(), tbs) || tbs.tag != 0x30) {
        return false;
    }

    usize tbs_cursor = tbs.content;
    DerNode node;
    if (!read_der_node(data, tbs_cursor, tbs.end(), node)) return false;
    if (node.tag == 0xA0) {
        if (!read_der_node(data, tbs_cursor, tbs.end(), node)) return false;
    }
    if (node.tag != 0x02) return false;
    result.serial = copy_integer(data, node);

    if (!read_der_node(data, tbs_cursor, tbs.end(), node)) return false;
    DerNode issuer;
    if (!read_der_node(data, tbs_cursor, tbs.end(), issuer) || issuer.tag != 0x30) {
        return false;
    }
    result.issuer = copy_full(data, issuer);

    if (!read_der_node(data, tbs_cursor, tbs.end(), node)) return false;
    DerNode subject;
    if (!read_der_node(data, tbs_cursor, tbs.end(), subject) || subject.tag != 0x30) {
        return false;
    }
    result.common_name = parse_common_name(data, subject);
    result.der = copy_full(data, certificate_node);
    return true;
}

[[nodiscard]] Result<Signature, ParseError> parse_pkcs7(std::span<const u8> data) {
    static constexpr u8 kSignedDataOid[] = {
        0x2A, 0x86, 0x48, 0x86, 0xF7, 0x0D, 0x01, 0x07, 0x02
    };
    usize cursor = 0;
    DerNode content_info;
    if (!read_der_node(data, cursor, data.size(), content_info) || content_info.tag != 0x30) {
        return Result<Signature, ParseError>::err(ParseError::InvalidPkcs7);
    }

    usize info_cursor = content_info.content;
    DerNode oid;
    DerNode wrapper;
    if (!read_der_node(data, info_cursor, content_info.end(), oid) || oid.tag != 0x06 ||
        oid.size != std::size(kSignedDataOid) ||
        !std::equal(std::begin(kSignedDataOid), std::end(kSignedDataOid),
                    data.begin() + static_cast<isize>(oid.content)) ||
        !read_der_node(data, info_cursor, content_info.end(), wrapper) || wrapper.tag != 0xA0) {
        return Result<Signature, ParseError>::err(ParseError::InvalidPkcs7);
    }

    usize wrapper_cursor = wrapper.content;
    DerNode signed_data;
    if (!read_der_node(data, wrapper_cursor, wrapper.end(), signed_data) ||
        signed_data.tag != 0x30) {
        return Result<Signature, ParseError>::err(ParseError::InvalidPkcs7);
    }

    usize signed_cursor = signed_data.content;
    DerNode node;
    for (int i = 0; i < 3; ++i) {
        if (!read_der_node(data, signed_cursor, signed_data.end(), node)) {
            return Result<Signature, ParseError>::err(ParseError::InvalidPkcs7);
        }
    }

    DerNode certificates;
    DerNode signer_infos;
    bool have_certificates = false;
    bool have_signers = false;
    while (signed_cursor < signed_data.end()) {
        if (!read_der_node(data, signed_cursor, signed_data.end(), node)) {
            return Result<Signature, ParseError>::err(ParseError::InvalidPkcs7);
        }
        if (node.tag == 0xA0 && !have_certificates) {
            certificates = node;
            have_certificates = true;
        } else if (node.tag == 0x31) {
            signer_infos = node;
            have_signers = true;
        }
    }
    if (!have_certificates || !have_signers) {
        return Result<Signature, ParseError>::err(ParseError::MissingSigner);
    }

    Signature result;
    usize certificate_cursor = certificates.content;
    while (certificate_cursor < certificates.end()) {
        DerNode certificate_node;
        if (!read_der_node(data, certificate_cursor, certificates.end(), certificate_node)) {
            return Result<Signature, ParseError>::err(ParseError::InvalidPkcs7);
        }
        Certificate certificate;
        if (certificate_node.tag == 0x30 && parse_certificate(data, certificate_node, certificate)) {
            result.certificates.push_back(std::move(certificate));
        }
    }
    if (result.certificates.empty()) {
        return Result<Signature, ParseError>::err(ParseError::MissingSigner);
    }

    usize signer_set_cursor = signer_infos.content;
    DerNode signer;
    if (!read_der_node(data, signer_set_cursor, signer_infos.end(), signer) || signer.tag != 0x30) {
        return Result<Signature, ParseError>::err(ParseError::MissingSigner);
    }
    usize signer_cursor = signer.content;
    if (!read_der_node(data, signer_cursor, signer.end(), node)) {
        return Result<Signature, ParseError>::err(ParseError::MissingSigner);
    }
    DerNode signer_id;
    if (!read_der_node(data, signer_cursor, signer.end(), signer_id) || signer_id.tag != 0x30) {
        return Result<Signature, ParseError>::err(ParseError::MissingSigner);
    }
    usize id_cursor = signer_id.content;
    DerNode signer_issuer;
    DerNode signer_serial;
    if (!read_der_node(data, id_cursor, signer_id.end(), signer_issuer) ||
        signer_issuer.tag != 0x30 ||
        !read_der_node(data, id_cursor, signer_id.end(), signer_serial) ||
        signer_serial.tag != 0x02) {
        return Result<Signature, ParseError>::err(ParseError::MissingSigner);
    }

    const auto issuer = copy_full(data, signer_issuer);
    const auto serial = copy_integer(data, signer_serial);
    auto match = std::find_if(result.certificates.begin(), result.certificates.end(),
        [&](const Certificate& certificate) {
            return certificate.issuer == issuer && certificate.serial == serial;
        });
    if (match == result.certificates.end()) {
        return Result<Signature, ParseError>::err(ParseError::MissingSigner);
    }
    result.signer_index = static_cast<usize>(match - result.certificates.begin());
    return Result<Signature, ParseError>::ok(std::move(result));
}

} // namespace

Result<Signature, ParseError> parse_pe(std::span<const u8> image) {
    u16 mz = 0;
    u32 pe_offset = 0;
    if (!read_u16(image, 0, mz) || mz != 0x5A4D ||
        !read_u32(image, 0x3C, pe_offset) ||
        pe_offset > image.size() || image.size() - pe_offset < 24 ||
        image[pe_offset] != 'P' || image[pe_offset + 1] != 'E' ||
        image[pe_offset + 2] != 0 || image[pe_offset + 3] != 0) {
        return Result<Signature, ParseError>::err(ParseError::InvalidPe);
    }

    u16 optional_size = 0;
    if (!read_u16(image, pe_offset + 20, optional_size)) {
        return Result<Signature, ParseError>::err(ParseError::InvalidPe);
    }
    const usize optional = static_cast<usize>(pe_offset) + 24;
    if (optional > image.size() || optional_size > image.size() - optional) {
        return Result<Signature, ParseError>::err(ParseError::InvalidPe);
    }

    u16 magic = 0;
    if (!read_u16(image, optional, magic)) {
        return Result<Signature, ParseError>::err(ParseError::InvalidPe);
    }
    const usize directory_base = optional + (magic == 0x10B ? 96u : magic == 0x20B ? 112u : 0u);
    const usize count_offset = optional + (magic == 0x10B ? 92u : magic == 0x20B ? 108u : 0u);
    u32 directory_count = 0;
    if (directory_base == optional || !read_u32(image, count_offset, directory_count) ||
        directory_count <= 4 || directory_base + 40 > optional + optional_size) {
        return Result<Signature, ParseError>::err(ParseError::NoSignature);
    }

    u32 certificate_offset = 0;
    u32 certificate_size = 0;
    if (!read_u32(image, directory_base + 32, certificate_offset) ||
        !read_u32(image, directory_base + 36, certificate_size) ||
        certificate_offset == 0 || certificate_size < 8) {
        return Result<Signature, ParseError>::err(ParseError::NoSignature);
    }
    if (certificate_offset > image.size() || certificate_size > image.size() - certificate_offset) {
        return Result<Signature, ParseError>::err(ParseError::InvalidWinCertificate);
    }

    u32 win_certificate_length = 0;
    u16 revision = 0;
    u16 certificate_type = 0;
    if (!read_u32(image, certificate_offset, win_certificate_length) ||
        !read_u16(image, certificate_offset + 4, revision) ||
        !read_u16(image, certificate_offset + 6, certificate_type) ||
        win_certificate_length < 8 || win_certificate_length > certificate_size ||
        win_certificate_length > image.size() - certificate_offset ||
        revision != 0x0200 || certificate_type != 0x0002) {
        return Result<Signature, ParseError>::err(ParseError::InvalidWinCertificate);
    }

    return parse_pkcs7(image.subspan(certificate_offset + 8, win_certificate_length - 8));
}

const char* parse_error_string(ParseError error) noexcept {
    switch (error) {
        case ParseError::InvalidPe: return "invalid PE image";
        case ParseError::NoSignature: return "no embedded signature";
        case ParseError::InvalidWinCertificate: return "invalid WIN_CERTIFICATE";
        case ParseError::InvalidPkcs7: return "invalid PKCS#7 signed data";
        case ParseError::MissingSigner: return "PKCS#7 signer not found";
    }
    return "unknown Authenticode parse error";
}

namespace {

[[nodiscard]] std::string oid_text(std::span<const u8> data, const DerNode& node) {
    if (node.tag != 0x06 || node.size == 0) return {};
    std::string out;
    u64 value = 0;
    usize i = node.content;
    for (; i < node.end(); ++i) {
        value = (value << 7) | (data[i] & 0x7F);
        if ((data[i] & 0x80) == 0) break;
    }
    const u64 first = value < 40 ? 0 : value < 80 ? 1 : 2;
    out = std::to_string(first) + "." + std::to_string(value - first * 40);
    value = 0;
    for (++i; i < node.end(); ++i) {
        value = (value << 7) | (data[i] & 0x7F);
        if ((data[i] & 0x80) == 0) {
            out += "." + std::to_string(value);
            value = 0;
        }
    }
    return out;
}

[[nodiscard]] std::vector<u8> contents(std::span<const u8> data, const DerNode& node) {
    return std::vector<u8>(data.begin() + static_cast<isize>(node.content),
                           data.begin() + static_cast<isize>(node.end()));
}

/// AlgorithmIdentifier: OID text and the encoded parameters after it.
bool algorithm(std::span<const u8> data, const DerNode& node, std::string& oid,
               std::vector<u8>& parameters) {
    if (node.tag != 0x30) return false;
    usize cursor = node.content;
    DerNode id;
    if (!read_der_node(data, cursor, node.end(), id) || id.tag != 0x06) return false;
    oid = oid_text(data, id);
    parameters.assign(data.begin() + static_cast<isize>(cursor),
                      data.begin() + static_cast<isize>(node.end()));
    return true;
}

bool attributes(std::span<const u8> data, const DerNode& set,
                std::vector<SignerInfo::Attribute>& out) {
    usize cursor = set.content;
    while (cursor < set.end()) {
        DerNode attribute;
        if (!read_der_node(data, cursor, set.end(), attribute) || attribute.tag != 0x30) {
            return false;
        }
        usize inner = attribute.content;
        DerNode type;
        DerNode values;
        if (!read_der_node(data, inner, attribute.end(), type) || type.tag != 0x06 ||
            !read_der_node(data, inner, attribute.end(), values) || values.tag != 0x31) {
            return false;
        }
        SignerInfo::Attribute item;
        item.oid = oid_text(data, type);
        usize value_cursor = values.content;
        while (value_cursor < values.end()) {
            DerNode value;
            if (!read_der_node(data, value_cursor, values.end(), value)) return false;
            item.values.push_back(copy_full(data, value));
        }
        out.push_back(std::move(item));
    }
    return true;
}

bool signer_info(std::span<const u8> data, const DerNode& node, SignerInfo& out) {
    if (node.tag != 0x30) return false;
    usize cursor = node.content;
    DerNode field;
    if (!read_der_node(data, cursor, node.end(), field) || field.tag != 0x02) return false;
    for (usize i = field.content; i < field.end(); ++i) out.version = (out.version << 8) | data[i];
    DerNode sid;
    if (!read_der_node(data, cursor, node.end(), sid) || sid.tag != 0x30) return false;
    {
        usize id_cursor = sid.content;
        DerNode issuer;
        DerNode serial;
        if (!read_der_node(data, id_cursor, sid.end(), issuer) || issuer.tag != 0x30 ||
            !read_der_node(data, id_cursor, sid.end(), serial) || serial.tag != 0x02) {
            return false;
        }
        out.issuer = copy_full(data, issuer);
        out.serial = contents(data, serial);
    }
    if (!read_der_node(data, cursor, node.end(), field) ||
        !algorithm(data, field, out.digest_oid, out.digest_parameters)) {
        return false;
    }
    if (!read_der_node(data, cursor, node.end(), field)) return false;
    if (field.tag == 0xA0) {
        out.has_signed_attributes = true;
        out.signed_attributes = contents(data, field);
        if (!attributes(data, field, out.signed_attribute_list)) return false;
        if (!read_der_node(data, cursor, node.end(), field)) return false;
    }
    if (!algorithm(data, field, out.signature_oid, out.signature_parameters)) return false;
    if (!read_der_node(data, cursor, node.end(), field) || field.tag != 0x04) return false;
    out.signature = contents(data, field);
    if (cursor < node.end()) {
        if (!read_der_node(data, cursor, node.end(), field)) return false;
        if (field.tag == 0xA1 && !attributes(data, field, out.unsigned_attribute_list)) {
            return false;
        }
    }
    return true;
}

} // namespace

Result<SignedData, ParseError> parse_signed_data(std::span<const u8> data) {
    using R = Result<SignedData, ParseError>;
    usize cursor = 0;
    DerNode content_info;
    if (!read_der_node(data, cursor, data.size(), content_info) || content_info.tag != 0x30) {
        return R::err(ParseError::InvalidPkcs7);
    }
    usize info_cursor = content_info.content;
    DerNode type;
    DerNode wrapper;
    if (!read_der_node(data, info_cursor, content_info.end(), type) ||
        oid_text(data, type) != "1.2.840.113549.1.7.2" ||
        !read_der_node(data, info_cursor, content_info.end(), wrapper) || wrapper.tag != 0xA0) {
        return R::err(ParseError::InvalidPkcs7);
    }
    usize wrapper_cursor = wrapper.content;
    DerNode signed_data;
    if (!read_der_node(data, wrapper_cursor, wrapper.end(), signed_data) ||
        signed_data.tag != 0x30) {
        return R::err(ParseError::InvalidPkcs7);
    }
    SignedData out;
    usize sd = signed_data.content;
    DerNode node;
    // version, digestAlgorithms
    for (int i = 0; i < 2; ++i) {
        if (!read_der_node(data, sd, signed_data.end(), node)) return R::err(ParseError::InvalidPkcs7);
    }
    DerNode encap;
    if (!read_der_node(data, sd, signed_data.end(), encap) || encap.tag != 0x30) {
        return R::err(ParseError::InvalidPkcs7);
    }
    {
        usize e = encap.content;
        DerNode content_type;
        if (!read_der_node(data, e, encap.end(), content_type) || content_type.tag != 0x06) {
            return R::err(ParseError::InvalidPkcs7);
        }
        out.content_type = oid_text(data, content_type);
        if (e < encap.end()) {
            DerNode explicit_content;
            DerNode inner;
            if (!read_der_node(data, e, encap.end(), explicit_content) ||
                explicit_content.tag != 0xA0) {
                return R::err(ParseError::InvalidPkcs7);
            }
            usize c = explicit_content.content;
            if (!read_der_node(data, c, explicit_content.end(), inner)) {
                return R::err(ParseError::InvalidPkcs7);
            }
            out.content = copy_full(data, inner);
            out.content_value = contents(data, inner);
        }
    }
    while (sd < signed_data.end()) {
        if (!read_der_node(data, sd, signed_data.end(), node)) return R::err(ParseError::InvalidPkcs7);
        if (node.tag == 0xA0) {
            usize c = node.content;
            while (c < node.end()) {
                DerNode certificate_node;
                if (!read_der_node(data, c, node.end(), certificate_node)) {
                    return R::err(ParseError::InvalidPkcs7);
                }
                Certificate certificate;
                if (certificate_node.tag == 0x30 &&
                    parse_certificate(data, certificate_node, certificate)) {
                    out.certificates.push_back(std::move(certificate));
                }
            }
        } else if (node.tag == 0x31) {
            usize c = node.content;
            while (c < node.end()) {
                DerNode signer;
                if (!read_der_node(data, c, node.end(), signer)) return R::err(ParseError::InvalidPkcs7);
                SignerInfo info;
                if (!signer_info(data, signer, info)) return R::err(ParseError::MissingSigner);
                out.signers.push_back(std::move(info));
            }
        }
    }
    if (out.signers.empty()) return R::err(ParseError::MissingSigner);
    return R::ok(std::move(out));
}

Result<SignerInfo, ParseError> parse_signer_info(std::span<const u8> data) {
    usize cursor = 0;
    DerNode node;
    SignerInfo out;
    if (!read_der_node(data, cursor, data.size(), node) || !signer_info(data, node, out)) {
        return Result<SignerInfo, ParseError>::err(ParseError::MissingSigner);
    }
    return Result<SignerInfo, ParseError>::ok(std::move(out));
}

bool is_pe_image(std::span<const u8> image) {
    u16 mz = 0;
    u32 pe_offset = 0;
    return read_u16(image, 0, mz) && mz == 0x5A4D && read_u32(image, 0x3C, pe_offset) &&
           pe_offset <= image.size() && image.size() - pe_offset >= 24 &&
           image[pe_offset] == 'P' && image[pe_offset + 1] == 'E' &&
           image[pe_offset + 2] == 0 && image[pe_offset + 3] == 0;
}

Result<PeSignatureLocation, ParseError> locate_pe_signature(std::span<const u8> image) {
    using R = Result<PeSignatureLocation, ParseError>;
    if (!is_pe_image(image)) return R::err(ParseError::InvalidPe);
    u32 pe_offset = 0;
    (void)read_u32(image, 0x3C, pe_offset);
    u16 optional_size = 0;
    if (!read_u16(image, pe_offset + 20, optional_size)) return R::err(ParseError::InvalidPe);
    const usize optional = static_cast<usize>(pe_offset) + 24;
    u16 magic = 0;
    if (!read_u16(image, optional, magic)) return R::err(ParseError::InvalidPe);
    const usize directory_base = optional + (magic == 0x10B ? 96u : magic == 0x20B ? 112u : 0u);
    const usize count_offset = optional + (magic == 0x10B ? 92u : magic == 0x20B ? 108u : 0u);
    u32 directory_count = 0;
    if (directory_base == optional || !read_u32(image, count_offset, directory_count) ||
        directory_count <= 4 || directory_base + 40 > optional + optional_size) {
        return R::err(ParseError::NoSignature);
    }
    PeSignatureLocation out;
    if (!read_u32(image, directory_base + 32, out.table_offset) ||
        !read_u32(image, directory_base + 36, out.table_size) ||
        out.table_offset == 0 || out.table_size < 8) {
        return R::err(ParseError::NoSignature);
    }
    if (out.table_offset > image.size() || out.table_size > image.size() - out.table_offset) {
        return R::err(ParseError::InvalidWinCertificate);
    }
    u32 length = 0;
    u16 revision = 0;
    u16 type = 0;
    if (!read_u32(image, out.table_offset, length) ||
        !read_u16(image, out.table_offset + 4, revision) ||
        !read_u16(image, out.table_offset + 6, type) || length < 8 ||
        length > out.table_size || revision != 0x0200 || type != 0x0002) {
        return R::err(ParseError::InvalidWinCertificate);
    }
    out.pkcs7.assign(image.begin() + out.table_offset + 8,
                     image.begin() + out.table_offset + length);
    return R::ok(std::move(out));
}

std::vector<std::pair<usize, usize>> pe_digest_ranges(std::span<const u8> image) {
    std::vector<std::pair<usize, usize>> ranges;
    u32 pe_offset = 0;
    u16 sections = 0;
    u16 optional_size = 0;
    u16 magic = 0;
    if (!is_pe_image(image) || !read_u32(image, 0x3C, pe_offset) ||
        !read_u16(image, pe_offset + 6, sections) ||
        !read_u16(image, pe_offset + 20, optional_size)) {
        return ranges;
    }
    const usize optional = static_cast<usize>(pe_offset) + 24;
    if (!read_u16(image, optional, magic) || (magic != 0x10B && magic != 0x20B)) return ranges;
    const usize checksum = optional + 64;
    const usize security = optional + (magic == 0x20B ? 144u : 128u);
    u32 size_of_headers = 0;
    u32 table_offset = 0;
    u32 table_size = 0;
    if (!read_u32(image, optional + 60, size_of_headers) ||
        !read_u32(image, security, table_offset) || !read_u32(image, security + 4, table_size) ||
        size_of_headers > image.size() || security + 8 > size_of_headers) {
        return ranges;
    }
    ranges.emplace_back(0, checksum);
    ranges.emplace_back(checksum + 4, security);
    ranges.emplace_back(security + 8, size_of_headers);
    struct Section { u32 pointer, size; };
    std::vector<Section> list;
    const usize table = optional + optional_size;
    for (u16 i = 0; i < sections; ++i) {
        Section section{};
        if (!read_u32(image, table + i * 40u + 20, section.pointer) ||
            !read_u32(image, table + i * 40u + 16, section.size)) {
            return {};
        }
        if (section.size != 0) list.push_back(section);
    }
    std::sort(list.begin(), list.end(),
              [](const Section& a, const Section& b) { return a.pointer < b.pointer; });
    usize hashed = size_of_headers;
    for (const auto& section : list) {
        if (section.pointer > image.size() || section.size > image.size() - section.pointer) {
            return {};
        }
        ranges.emplace_back(section.pointer, section.pointer + section.size);
        hashed += section.size;
    }
    // Data past the sections, excluding the certificate table.
    const usize end = image.size() - (table_offset != 0 ? std::min<usize>(table_size, image.size()) : 0);
    if (hashed < end) ranges.emplace_back(hashed, end);
    return ranges;
}

bool is_cab_image(std::span<const u8> image) {
    return image.size() >= 36 && image[0] == 'M' && image[1] == 'S' && image[2] == 'C' &&
           image[3] == 'F';
}

Result<CabSignatureLocation, ParseError> locate_cab_signature(std::span<const u8> image) {
    using R = Result<CabSignatureLocation, ParseError>;
    constexpr u16 kReservePresent = 0x0004;  // cfhdrRESERVE_PRESENT
    u32 cabinet_size = 0;
    u16 flags = 0;
    u16 header_reserve = 0;
    if (!is_cab_image(image) || !read_u32(image, 8, cabinet_size) || !read_u16(image, 30, flags)) {
        return R::err(ParseError::NoSignature);
    }
    if ((flags & kReservePresent) == 0 || !read_u16(image, 36, header_reserve) ||
        header_reserve < 20 || image.size() < 40u + header_reserve) {
        return R::err(ParseError::NoSignature);
    }
    u32 lead = 0;
    CabSignatureLocation out;
    if (!read_u32(image, 40, lead) || lead != 0x00100000u || image[39] != 0 ||
        !read_u32(image, 44, out.offset) || !read_u32(image, 48, out.size) ||
        out.offset != cabinet_size || out.size == 0) {
        return R::err(ParseError::NoSignature);
    }
    if (out.offset > image.size() || out.size > image.size() - out.offset) {
        return R::err(ParseError::NoSignature);
    }
    out.pkcs7.assign(image.begin() + out.offset, image.begin() + out.offset + out.size);
    return R::ok(std::move(out));
}

std::vector<std::pair<usize, usize>> cab_digest_ranges(std::span<const u8> image) {
    auto located = locate_cab_signature(image);
    if (located.is_err()) return {};
    const auto& where = located.value();
    u16 header_reserve = 0;
    (void)read_u16(image, 36, header_reserve);
    std::vector<std::pair<usize, usize>> ranges;
    ranges.emplace_back(0, 4);
    ranges.emplace_back(8, 36);
    ranges.emplace_back(38, 40);
    const usize body = 40u + header_reserve;
    if (body < where.offset) ranges.emplace_back(body, where.offset);
    const usize tail = static_cast<usize>(where.offset) + where.size;
    if (tail < image.size()) ranges.emplace_back(tail, image.size());
    return ranges;
}

namespace {

constexpr std::u16string_view kDigitalSignatureStream = u"\x05" u"DigitalSignature";
constexpr std::u16string_view kMsiDigitalSignatureExStream = u"\x05" u"MsiDigitalSignatureEx";

/// A root-level signature stream, found the way IStorage::OpenStream finds
/// it (case-insensitively, through the children map); the digest walk then
/// skips that node by identity.
[[nodiscard]] const StorageNode* find_child(const StorageNode& storage, std::u16string_view name) {
    std::string utf8;
    for (const char16_t unit : name) utf8.push_back(static_cast<char>(unit));  // ASCII names only
    auto it = storage.children.find(utf8);
    return it == storage.children.end() ? nullptr : it->second.get();
}

/// Ascending order of the UTF-16LE name bytes, memcmp style: unit by unit,
/// low byte before high byte, a proper prefix first.
[[nodiscard]] bool storage_name_less(const std::u16string& a, const std::u16string& b) {
    const usize n = std::min(a.size(), b.size());
    for (usize i = 0; i < n; ++i) {
        const u8 alo = static_cast<u8>(a[i] & 0xFF), blo = static_cast<u8>(b[i] & 0xFF);
        if (alo != blo) return alo < blo;
        const u8 ahi = static_cast<u8>(a[i] >> 8), bhi = static_cast<u8>(b[i] >> 8);
        if (ahi != bhi) return ahi < bhi;
    }
    return a.size() < b.size();
}

[[nodiscard]] std::vector<const StorageNode*> digest_order(const StorageNode& storage,
                                                           const StorageNode* skip_a,
                                                           const StorageNode* skip_b) {
    std::vector<const StorageNode*> out;
    out.reserve(storage.children.size());
    for (const auto& [name, child] : storage.children) {
        if (child.get() != skip_a && child.get() != skip_b) out.push_back(child.get());
    }
    std::sort(out.begin(), out.end(), [](const StorageNode* x, const StorageNode* y) {
        return storage_name_less(x->name_utf16, y->name_utf16);
    });
    return out;
}

void append_le(std::vector<u8>& out, u64 value, usize bytes) {
    for (usize i = 0; i < bytes; ++i) out.push_back(static_cast<u8>(value >> (8 * i)));
}

void append_name(std::vector<u8>& out, const std::u16string& name) {
    for (const char16_t unit : name) append_le(out, unit, 2);
}

void storage_content(const StorageNode& storage, const StorageNode* skip_a, const StorageNode* skip_b,
                     std::vector<std::span<const u8>>& out) {
    for (const StorageNode* child : digest_order(storage, skip_a, skip_b)) {
        if (child->is_storage) {
            storage_content(*child, nullptr, nullptr, out);
        } else if (!child->data.empty()) {
            out.emplace_back(child->data);
        }
    }
    out.emplace_back(storage.clsid);
}

void storage_metadata(const StorageNode& storage, const StorageNode* skip_a, const StorageNode* skip_b,
                      std::vector<u8>& out) {
    for (const StorageNode* child : digest_order(storage, skip_a, skip_b)) {
        append_name(out, child->name_utf16);
        if (child->is_storage) {
            out.insert(out.end(), child->clsid.begin(), child->clsid.end());
        } else {
            append_le(out, child->data.size(), 4);
        }
        append_le(out, child->state_bits, 4);
        append_le(out, child->creation_time, 8);
        append_le(out, child->modified_time, 8);
        if (child->is_storage) storage_metadata(*child, nullptr, nullptr, out);
    }
}

} // namespace

bool is_compound_file_image(std::span<const u8> image) {
    static constexpr u8 kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    return image.size() >= sizeof(kSignature) &&
           std::equal(std::begin(kSignature), std::end(kSignature), image.begin());
}

Result<StorageSignature, ParseError> locate_storage_signature(std::span<const u8> image) {
    using R = Result<StorageSignature, ParseError>;
    std::shared_ptr<const StorageNode> root =
        is_compound_file_image(image) ? parse_compound_file(image) : nullptr;
    if (root == nullptr) return R::err(ParseError::InvalidPe);
    const StorageNode* signature = find_child(*root, kDigitalSignatureStream);
    if (signature == nullptr || signature->is_storage || signature->data.empty()) {
        return R::err(ParseError::NoSignature);
    }
    StorageSignature out;
    out.pkcs7 = signature->data;
    if (const StorageNode* ex = find_child(*root, kMsiDigitalSignatureExStream); ex != nullptr && !ex->is_storage) {
        out.has_extended = true;
        out.extended = ex->data;
    }
    out.root = std::move(root);
    return R::ok(std::move(out));
}

StorageDigestInput storage_digest_input(const StorageSignature& subject) {
    StorageDigestInput input;
    if (subject.root == nullptr) return input;
    const StorageNode& root = *subject.root;
    const StorageNode* signature = find_child(root, kDigitalSignatureStream);
    const StorageNode* ex = find_child(root, kMsiDigitalSignatureExStream);
    input.extended = subject.has_extended;
    if (input.extended) {
        input.metadata.insert(input.metadata.end(), root.clsid.begin(), root.clsid.end());
        append_le(input.metadata, root.state_bits, 4);
        storage_metadata(root, signature, ex, input.metadata);
    }
    storage_content(root, signature, ex, input.content);
    return input;
}

} // namespace vitreo::winapi::authenticode

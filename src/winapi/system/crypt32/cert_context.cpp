// SPDX-License-Identifier: LGPL-2.1-only
/// @file cert_context.cpp
/// @brief CERT_CONTEXT materialization and CertCreateCertificateContext.
///
/// Contract: https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certcreatecertificatecontext
/// and CERT_INFO (wincrypt.h). Measured Windows behaviour is in
/// tools/oracle_probe/crypt_prop_probe.c (+ .expected / .x86.expected):
///  - only the certificate half of the encoding type is used: a zero
///    certificate encoding is E_INVALIDARG, one without a decoder
///    ERROR_FILE_NOT_FOUND, and dwCertEncodingType keeps only that half;
///  - malformed encodings fail with the CRYPT_E_ASN1_* the decoder reports
///    (x509::certificate_decode_error);
///  - CERT_INFO.SerialNumber is little-endian (every INTEGER content octet
///    reversed, leading zero kept), SignatureAlgorithm / SubjectPublicKeyInfo
///    are decoded, and the whole context lives in one allocation;
///  - the new context's hCertStore is a store of its own that holds it.

#include "crypt32.hpp"
#include "winapi/system/crypt32/crypt32_internal.hpp"
#include "winapi/system/crypt32/guest_block.hpp"
#include "x509_parse.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/memory/global_local.hpp"

#include <algorithm>

namespace vitreo::winapi {

namespace {

constexpr u32 kX509AsnEncoding = 0x00000001u;
constexpr u32 kCertEncodingMask = 0x0000FFFFu;

} // namespace

Crypt32Manager::CertEntry Crypt32Manager::make_cert_entry(
    std::span<const u8> der, const x509::ParsedCertificate& parsed,
    GuestAddr context) {
    CertEntry entry;
    entry.der_data.assign(der.begin(), der.end());
    entry.guest_cert_context = context;
    entry.common_name = parsed.subject_simple_name;
    entry.issuer_name = parsed.issuer_simple_name;
    entry.subject_attributes = parsed.subject_attributes;
    entry.issuer_attributes = parsed.issuer_attributes;
    return entry;
}

GuestAddr Crypt32Manager::build_cert_context_block(
    Marshaler& m, std::span<const u8> der,
    const x509::ParsedCertificate& parsed, u32 encoding_type,
    u32 store_handle) {
    const bool is64 = m.cpu_is_64bit();
    const crypt32::CertLayout l(is64);
    crypt32::GuestBlock block(is64);

    const u32 ctx = block.reserve(l.ctx_size);
    const u32 info = block.reserve(l.info_size);
    const u32 encoded = block.add_bytes(der, 8);

    block.put_u32(ctx + 0, encoding_type);
    block.put_ptr(ctx + l.ptr, encoded);
    block.put_u32(ctx + 2 * l.ptr, static_cast<u32>(der.size()));
    block.put_ptr(ctx + 3 * l.ptr, info);
    block.put_value(ctx + 4 * l.ptr, store_handle);

    block.put_u32(info + 0, parsed.version);
    std::vector<u8> serial_le(parsed.serial.rbegin(), parsed.serial.rend());
    block.put_blob(info + l.serial, serial_le);
    auto put_algorithm = [&](u32 at, const x509::AlgorithmIdentifier& alg) {
        if (!alg.oid.empty()) block.put_ptr(at, block.add_cstring(alg.oid));
        block.put_blob(at + l.ptr, alg.parameters);
    };
    put_algorithm(info + l.sig_alg, parsed.signature_algorithm);
    block.put_blob(info + l.issuer, parsed.issuer_der);
    block.put_u32(info + l.not_before, static_cast<u32>(parsed.not_before_filetime));
    block.put_u32(info + l.not_before + 4,
                  static_cast<u32>(parsed.not_before_filetime >> 32));
    block.put_u32(info + l.not_after, static_cast<u32>(parsed.not_after_filetime));
    block.put_u32(info + l.not_after + 4,
                  static_cast<u32>(parsed.not_after_filetime >> 32));
    block.put_blob(info + l.subject, parsed.subject_der);
    put_algorithm(info + l.spki, parsed.spki.algorithm);
    block.put_blob(info + l.pubkey, parsed.spki.key);
    block.put_u32(info + l.pubkey + 2 * l.ptr, parsed.spki.unused_bits);
    auto put_bits = [&](u32 at, const x509::BitString& bits) {
        block.put_blob(at, bits.bytes);
        block.put_u32(at + 2 * l.ptr, bits.unused_bits);
    };
    put_bits(info + l.issuer_uid, parsed.issuer_unique_id);
    put_bits(info + l.subject_uid, parsed.subject_unique_id);

    const u32 count = static_cast<u32>(parsed.extensions.size());
    block.put_u32(info + l.c_ext, count);
    if (count != 0) {
        const u32 array = block.reserve(count * l.ext_size);
        block.put_ptr(info + l.rg_ext, array);
        for (u32 i = 0; i < count; ++i) {
            const auto& extension = parsed.extensions[i];
            const u32 entry = array + i * l.ext_size;
            block.put_ptr(entry, block.add_cstring(extension.oid));
            block.put_u32(entry + l.ptr, extension.critical ? 1u : 0u);
            block.put_blob(entry + 2 * l.ptr, extension.value);
        }
    }

    const GuestAddr base = alloc_guest(block.size());
    if (base == 0) return 0;
    const std::vector<u8> image = block.resolve(base);
    if (m.memory().write_bytes(base, reinterpret_cast<const Byte*>(image.data()),
                               image.size()).is_err()) {
        free_guest(base);
        return 0;
    }
    return base + ctx;
}

// PCCERT_CONTEXT CertCreateCertificateContext(DWORD dwCertEncodingType,
//     const BYTE* pbCertEncoded, DWORD cbCertEncoded)
GuestAddr Crypt32Manager::cert_create_certificate_context(Marshaler& m) {
    const u32 encoding = m.arg32(0);
    const GuestAddr source = m.arg_ptr(1);
    const u32 size = m.arg32(2);

    const u32 cert_encoding = encoding & kCertEncodingMask;
    if (cert_encoding == 0) {
        set_last_error(crypt32::err::kEInvalidArg);
        return 0;
    }
    if (cert_encoding != kX509AsnEncoding) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }
    std::vector<u8> der(size);
    if (size != 0 &&
        m.memory().read_bytes(source, reinterpret_cast<Byte*>(der.data()), size).is_err()) {
        set_last_error(error_codes::ERROR_NOACCESS);
        return 0;
    }
    if (const u32 error = x509::certificate_decode_error(der)) {
        set_last_error(error);
        return 0;
    }
    auto parsed = x509::parse_certificate(der);
    if (!parsed) {
        set_last_error(x509::kCryptEAsn1Corrupt);
        return 0;
    }

    return create_owned_cert_context(m, der, *parsed, cert_encoding);
}

GuestAddr Crypt32Manager::create_owned_cert_context(Marshaler& m, std::span<const u8> der_span,
                                                    const x509::ParsedCertificate& parsed_cert,
                                                    u32 cert_encoding) {
    const std::vector<u8> der(der_span.begin(), der_span.end());
    const auto* parsed = &parsed_cert;
    // The context gets a store of its own (Windows reports a non-NULL
    // hCertStore; crypt_prop_probe "ctx" row) that holds just this cert.
    const u32 store_handle = cert_open_store();
    const GuestAddr context =
        build_cert_context_block(m, der, *parsed, cert_encoding, store_handle);
    std::lock_guard<std::mutex> lock(m_mutex);
    if (context == 0) {
        m_store_certs.erase(store_handle);
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }
    m_store_certs[store_handle].push_back(der);
    CertEntry entry = make_cert_entry(der, *parsed, context);
    entry.props = element_properties_locked(store_handle, der);
    entry.store_handle = store_handle;
    entry.store_index = 0;
    entry.owns_store_handle = true;
    // The context is the first structure in its block.
    if (m_local_memory != nullptr) entry.owned_allocations.push_back(context);
    m_cert_contexts[context] = std::move(entry);
    // Measured: a successful create leaves the last error at 0.
    set_last_error(error_codes::ERROR_SUCCESS);
    return context;
}

GuestAddr Crypt32Manager::create_cert_context_from_der(
    Marshaler& m, std::span<const u8> der, u32 store_handle,
    u32 encoding_type, std::string persistent_record, usize store_index) {
    if (der.empty() || der.size() > 0x100000) return 0;
    auto parsed = x509::parse_certificate(der);
    if (!parsed) return 0;
    const GuestAddr context = build_cert_context_block(
        m, der, *parsed, encoding_type & kCertEncodingMask, store_handle);
    if (context == 0) return 0;

    std::lock_guard<std::mutex> lock(m_mutex);
    CertEntry entry = make_cert_entry(der, *parsed, context);
    if (store_handle != 0) {
        entry.props = element_properties_locked(
            store_handle, std::vector<u8>(der.begin(), der.end()));
    }
    entry.store_handle = store_handle;
    entry.store_index = store_index;
    entry.persistent_record = std::move(persistent_record);
    if (m_local_memory != nullptr) entry.owned_allocations.push_back(context);
    m_cert_contexts[context] = std::move(entry);
    return context;
}

std::shared_ptr<Crypt32Manager::CertProperties> Crypt32Manager::element_properties_locked(
    u32 store, const std::vector<u8>& der) {
    auto& slot = m_element_properties[store][der];
    if (!slot) slot = std::make_shared<CertProperties>();
    return slot;
}

} // namespace vitreo::winapi

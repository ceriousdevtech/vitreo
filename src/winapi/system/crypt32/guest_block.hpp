// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file guest_block.hpp
/// @brief Host-side image of one guest allocation holding a structure graph.
///
/// Crypt32 returns structures whose pointers lead to further data in the same
/// allocation (CERT_CONTEXT + CERT_INFO + blobs, CERT_CHAIN_CONTEXT + simple
/// chains + elements, CRYPT_KEY_PROV_INFO + strings). The layout is built on
/// the host with offsets, and pointer fields are recorded as fixups resolved
/// once the guest base address is known, at the guest's pointer width.

#include <vitreo/common.hpp>

#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

namespace vitreo::winapi::crypt32 {

class GuestBlock {
public:
    static constexpr u32 kNull = std::numeric_limits<u32>::max();

    explicit GuestBlock(bool is64) : m_is64(is64) {}

    [[nodiscard]] bool is64() const noexcept { return m_is64; }
    [[nodiscard]] u32 pointer_size() const noexcept { return m_is64 ? 8u : 4u; }
    [[nodiscard]] u32 size() const noexcept {
        return static_cast<u32>(m_bytes.size());
    }

    /// Zero-filled space, aligned relative to the block start.
    u32 reserve(u32 size, u32 align = 8) {
        usize offset = m_bytes.size();
        if (align > 1) offset = (offset + align - 1) / align * align;
        m_bytes.resize(offset + size, 0);
        return static_cast<u32>(offset);
    }

    u32 add_bytes(std::span<const u8> bytes, u32 align = 1) {
        const u32 offset = reserve(static_cast<u32>(bytes.size()), align);
        if (!bytes.empty()) std::memcpy(m_bytes.data() + offset, bytes.data(), bytes.size());
        return offset;
    }

    /// NUL-terminated narrow string.
    u32 add_cstring(std::string_view text, u32 align = 1) {
        const u32 offset = reserve(static_cast<u32>(text.size() + 1), align);
        if (!text.empty()) std::memcpy(m_bytes.data() + offset, text.data(), text.size());
        return offset;
    }

    /// NUL-terminated UTF-16LE string.
    u32 add_wstring(std::span<const u16> text, u32 align = 2) {
        const u32 offset = reserve(static_cast<u32>((text.size() + 1) * 2), align);
        for (usize i = 0; i < text.size(); ++i) {
            m_bytes[offset + i * 2] = static_cast<u8>(text[i]);
            m_bytes[offset + i * 2 + 1] = static_cast<u8>(text[i] >> 8);
        }
        return offset;
    }

    void put_u32(u32 offset, u32 value) {
        for (u32 i = 0; i < 4; ++i) {
            m_bytes[offset + i] = static_cast<u8>(value >> (8 * i));
        }
    }

    /// Pointer field at `offset` that will hold base + `target` (or NULL).
    void put_ptr(u32 offset, u32 target) {
        if (target != kNull) m_fixups.push_back({offset, target});
    }

    /// A raw pointer-width value (a handle or an external guest address).
    void put_value(u32 offset, u64 value) {
        for (u32 i = 0; i < pointer_size(); ++i) {
            m_bytes[offset + i] = static_cast<u8>(value >> (8 * i));
        }
    }

    /// CRYPT_*_BLOB {DWORD cbData; BYTE* pbData} at `offset`; the data is
    /// appended to the block. Empty data leaves pbData NULL.
    void put_blob(u32 offset, std::span<const u8> data, u32 align = 1) {
        put_u32(offset, static_cast<u32>(data.size()));
        if (!data.empty()) put_ptr(offset + pointer_size(), add_bytes(data, align));
    }

    [[nodiscard]] std::vector<u8> resolve(u64 base) const {
        std::vector<u8> out = m_bytes;
        for (const auto& fixup : m_fixups) {
            const u64 value = base + fixup.target;
            for (u32 i = 0; i < pointer_size(); ++i) {
                out[fixup.offset + i] = static_cast<u8>(value >> (8 * i));
            }
        }
        return out;
    }

private:
    struct Fixup {
        u32 offset;
        u32 target;
    };
    bool m_is64;
    std::vector<u8> m_bytes;
    std::vector<Fixup> m_fixups;
};

/// Offsets of CERT_CONTEXT / CERT_INFO / CERT_EXTENSION members for one
/// pointer width (wincrypt.h; sizes 0x14/0x70 x86, 0x28/0xd0 x64 as the
/// probe's "layout" row reports).
struct CertLayout {
    u32 ptr, blob, alg, bit_blob;
    u32 ctx_size, info_size, ext_size;
    u32 serial, sig_alg, issuer, not_before, not_after, subject, spki, pubkey,
        issuer_uid, subject_uid, c_ext, rg_ext;

    explicit CertLayout(bool is64)
        : ptr(is64 ? 8u : 4u), blob(is64 ? 16u : 8u), alg(is64 ? 24u : 12u),
          bit_blob(is64 ? 24u : 12u), ctx_size(is64 ? 40u : 20u),
          ext_size(is64 ? 32u : 16u) {
        serial = ptr;
        sig_alg = serial + blob;
        issuer = sig_alg + alg;
        not_before = issuer + blob;
        not_after = not_before + 8;
        subject = not_after + 8;
        spki = subject + blob;
        pubkey = spki + alg;
        issuer_uid = pubkey + bit_blob;
        subject_uid = issuer_uid + bit_blob;
        c_ext = subject_uid + bit_blob;
        rg_ext = c_ext + ptr;
        info_size = rg_ext + ptr;
    }
};


} // namespace vitreo::winapi::crypt32

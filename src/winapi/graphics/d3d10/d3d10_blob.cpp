// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d10_blob.cpp
/// @brief ID3D10Blob, D3D10CreateBlob, D3D10Get*SignatureBlob and
///        D3D10Get*ShaderProfile. Split out of d3d10_device.cpp as
///        verbatim code motion.

#include "winapi/graphics/d3d10/d3d10.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d10/d3d10_device_internal.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace vitreo::winapi {
namespace {

/// D3DERR_INVALIDCALL: D3D10CreateBlob's answer to a NULL ppBuffer
/// (measured).
constexpr u32 kD3DErrInvalidCall = 0x8876086Cu;

} // namespace

// -----------------------------------------------------------------------------
// ID3D10Blob
// -----------------------------------------------------------------------------

GuestAddr D3D11Manager::d3d10_create_blob(const u8* bytes, usize size) {
    if (!ensure_storage() || !m_d3d10_ready) return 0;
    // The bytes live in guest memory of their own (GetBufferPointer hands
    // them to the guest); an empty blob still has a buffer (measured:
    // D3D10CreateBlob(0) answers a non-NULL pointer).
    const usize mapped = std::max<usize>((size + 0xFFFu) & ~usize{0xFFF},
                                         0x1000u);
    const GuestAddr data = m_memory.map_first_fit(
        0x10000000ull, mapped, MemoryProtection::ReadWrite);
    if (data == 0) return 0;
    if (bytes != nullptr && size != 0 &&
        m_memory.write_bytes(data, bytes, size).is_err()) {
        (void)m_memory.unmap_region(data, mapped);
        return 0;
    }
    const GuestAddr address = allocate_object(m_d3d10_blob_vtable);
    if (address == 0) {
        (void)m_memory.unmap_region(data, mapped);
        return 0;
    }
    m_d3d10_blobs.emplace(address, D3D10Blob{data, size, mapped, 1});
    return address;
}

void D3D11Manager::impl_D3D10CreateBlob(Marshaler& m) {
    // D3D10CreateBlob(NumBytes, ppBuffer).
    const usize size = static_cast<usize>(m.arg(0));
    const GuestAddr out = m.arg_ptr(1);
    if (out == 0) {
        m.set_return(kD3DErrInvalidCall);  // measured
        return;
    }
    (void)m.write_ptr(out, 0);
    const GuestAddr blob = d3d10_create_blob(nullptr, size);
    if (blob == 0) {
        m.set_return(kEOutOfMemory);
        return;
    }
    (void)m.write_ptr(out, blob);
    m.set_return(kSOk);
}

void D3D11Manager::impl_D3D10BlobMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    auto blob = m_d3d10_blobs.find(self);
    if (blob == m_d3d10_blobs.end()) {
        m.set_return(slot == 1 || slot == 2 ? 0 : kEInvalidArg);
        return;
    }
    switch (slot) {
    case 0: {
        // IUnknown and ID3D10Blob (= ID3DBlob) only (measured).
        const GuestAddr out = m.arg_ptr(2);
        if (out == 0) {
            m.set_return(kEPointer);
            return;
        }
        GuidBytes iid{};
        if (!read_guid(m.arg_ptr(1), iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D10BLOB))) {
            (void)m.write_ptr(out, 0);
            m.set_return(kENoInterface);
            return;
        }
        ++blob->second.ref_count;
        (void)m.write_ptr(out, self);
        m.set_return(kSOk);
        return;
    }
    case 1:
        m.set_return(++blob->second.ref_count);
        return;
    case 2: {
        if (blob->second.ref_count == 0) {
            m.set_return(0);
            return;
        }
        const u32 count = --blob->second.ref_count;
        if (count == 0) {
            const D3D10Blob dead = blob->second;
            m_d3d10_blobs.erase(blob);
            (void)m_memory.unmap_region(dead.data, dead.mapped);
            free_object(self);
        }
        m.set_return(count);
        return;
    }
    case 3:  // GetBufferPointer
        m.set_return(blob->second.data);
        return;
    case 4:  // GetBufferSize (SIZE_T)
        m.set_return(blob->second.size);
        return;
    default:
        m.set_return(0);
        return;
    }
}

// -----------------------------------------------------------------------------
// D3D10Get*SignatureBlob and D3D10Get*ShaderProfile
// -----------------------------------------------------------------------------

void D3D11Manager::impl_D3D10GetSignatureBlob(Marshaler& m, bool input,
                                              bool output) {
    // (pShaderBytecode, BytecodeLength, ppSignatureBlob). A DXBC container
    // holding the requested ISGN / OSGN chunks verbatim, input first, with a
    // freshly computed container hash (byte-exact against Windows,
    // d3d10_contract_probe [blob]). Only a Shader Model 4 program (an SHDR
    // chunk) has them to give: a vs_5_0 blob, junk, a truncated or a NULL
    // one is E_FAIL with the out-parameter untouched (measured); a NULL
    // out-parameter with good bytecode succeeds without a blob.
    const GuestAddr code = m.arg_ptr(0);
    const usize length = static_cast<usize>(m.arg(1));
    const GuestAddr out = m.arg_ptr(2);
    constexpr u32 kDxbc = 0x43425844u;  // "DXBC"
    constexpr usize kHeader = 32;
    if (code == 0 || length < kHeader || length > (64u << 20)) {
        m.set_return(kEFail);
        return;
    }
    std::vector<u8> bytes(length);
    if (m_memory.read_bytes(code, bytes.data(), length).is_err()) {
        m.set_return(kEFail);
        return;
    }
    const auto word = [&](usize at) -> u32 {
        if (at + 4 > bytes.size()) return 0;
        u32 value = 0;
        std::memcpy(&value, bytes.data() + at, 4);
        return value;
    };
    const u32 total = word(24);
    const u32 chunks = word(28);
    if (word(0) != kDxbc || total < kHeader || total > length ||
        chunks > (total - kHeader) / 4u) {
        m.set_return(kEFail);
        return;
    }
    const auto fourcc = [](const char* text) {
        u32 value = 0;
        std::memcpy(&value, text, 4);
        return value;
    };
    struct Chunk {
        u32 offset{0};
        u32 size{0};
    };
    Chunk isgn{}, osgn{};
    bool shdr = false;
    for (u32 i = 0; i < chunks; ++i) {
        const u32 offset = word(kHeader + i * 4u);
        if (static_cast<u64>(offset) + 8 > total) {
            m.set_return(kEFail);
            return;
        }
        const u32 size = word(offset + 4);
        if (static_cast<u64>(offset) + 8 + size > total) {
            m.set_return(kEFail);
            return;
        }
        const u32 name = word(offset);
        if (name == fourcc("SHDR")) shdr = true;
        if (name == fourcc("ISGN")) isgn = {offset, size};
        if (name == fourcc("OSGN")) osgn = {offset, size};
    }
    if (!shdr || (input && isgn.offset == 0) ||
        (output && osgn.offset == 0)) {
        m.set_return(kEFail);
        return;
    }
    if (out == 0) {
        m.set_return(kSOk);
        return;
    }
    std::vector<Chunk> parts;
    if (input) parts.push_back(isgn);
    if (output) parts.push_back(osgn);
    usize size = kHeader + parts.size() * 4u;
    for (const Chunk& part : parts) size += 8u + part.size;
    std::vector<u8> blob(size);
    const auto put = [&](usize at, u32 value) {
        std::memcpy(blob.data() + at, &value, 4);
    };
    put(0, kDxbc);
    put(20, 1);
    put(24, static_cast<u32>(size));
    put(28, static_cast<u32>(parts.size()));
    usize at = kHeader + parts.size() * 4u;
    for (usize i = 0; i < parts.size(); ++i) {
        put(kHeader + i * 4u, static_cast<u32>(at));
        std::memcpy(blob.data() + at, bytes.data() + parts[i].offset,
                    8u + parts[i].size);
        at += 8u + parts[i].size;
    }
    graphics::dxbc_container_checksum(blob.data(), static_cast<u32>(size),
                                      blob.data() + 4);
    const GuestAddr created = d3d10_create_blob(blob.data(), blob.size());
    if (created == 0) {
        m.set_return(kEOutOfMemory);
        return;
    }
    (void)m.write_ptr(out, created);
    m.set_return(kSOk);
}

void D3D11Manager::impl_D3D10GetShaderProfile(Marshaler& m, char stage) {
    // D3D10Get{Vertex,Geometry,Pixel}ShaderProfile(pDevice): static strings,
    // "vs_4_0" / "gs_4_0" / "ps_4_0" whatever the device -- 10.1 and 9_x
    // included (measured; the x86 export is "mov eax,<string>; ret 4").
    if (m_d3d10_profiles == 0) {
        static constexpr char kProfiles[] = "vs_4_0\0gs_4_0\0ps_4_0";
        m_d3d10_profiles = m_memory.map_first_fit(
            0x10000000ull, 0x1000, MemoryProtection::ReadWrite);
        if (m_d3d10_profiles == 0 ||
            m_memory.write_bytes(m_d3d10_profiles,
                                 reinterpret_cast<const u8*>(kProfiles),
                                 sizeof(kProfiles)).is_err()) {
            m_d3d10_profiles = 0;
            m.set_return(0);
            return;
        }
    }
    const GuestAddr offset = stage == 'v' ? 0u : stage == 'g' ? 7u : 14u;
    m.set_return(m_d3d10_profiles + offset);
}

} // namespace vitreo::winapi

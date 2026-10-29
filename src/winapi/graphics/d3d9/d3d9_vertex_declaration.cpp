// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_vertex_declaration.cpp
/// @brief IDirect3DVertexDeclaration9 parsing and interface dispatch.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <cstdio>

namespace vitreo::winapi {

bool D3D9Manager::parse_vertex_declaration(GuestAddr elements_addr,
                                           VertexDeclarationState& out,
                                           const char*& rejection) const {
    out.raw.clear();
    out.elements.clear();
    out.stream_strides.fill(0);
    if (m_memory == nullptr || elements_addr == 0) {
        rejection = "a null D3DVERTEXELEMENT9 array";
        return false;
    }

    // D3DDECLTYPE values — Microsoft Learn / D3D9Types.h.
    constexpr u8 D3DDECLTYPE_FLOAT1 = 0;
    constexpr u8 D3DDECLTYPE_FLOAT2 = 1;
    constexpr u8 D3DDECLTYPE_FLOAT3 = 2;
    constexpr u8 D3DDECLTYPE_FLOAT4 = 3;
    constexpr u8 D3DDECLTYPE_D3DCOLOR = 4;
    constexpr u8 D3DDECLTYPE_UBYTE4 = 5;
    constexpr u8 D3DDECLTYPE_SHORT2 = 6;
    constexpr u8 D3DDECLTYPE_SHORT4 = 7;
    constexpr u8 D3DDECLTYPE_UBYTE4N = 8;
    constexpr u8 D3DDECLTYPE_SHORT2N = 9;
    constexpr u8 D3DDECLTYPE_SHORT4N = 10;
    constexpr u8 D3DDECLTYPE_USHORT2N = 11;
    constexpr u8 D3DDECLTYPE_USHORT4N = 12;
    // D3DDECLTYPE_UDEC3 (13) has no Metal float-compatible unnormalized
    // 10:10:10 vertex format — refuse honestly in the default case.
    constexpr u8 D3DDECLTYPE_DEC3N = 14;
    constexpr u8 D3DDECLTYPE_FLOAT16_2 = 15;
    constexpr u8 D3DDECLTYPE_FLOAT16_4 = 16;
    constexpr u8 D3DDECLTYPE_UNUSED = 17;
    constexpr u16 D3DDECL_END_STREAM = 0xFF;
    constexpr usize kMaxElements = 64;

    for (usize i = 0; i <= kMaxElements; ++i) {
        const GuestAddr addr =
            elements_addr + static_cast<GuestAddr>(i * 8);
        auto stream_r = m_memory->read_u16(addr);
        auto offset_r = m_memory->read_u16(addr + 2);
        auto type_r = m_memory->read_u8(addr + 4);
        auto method_r = m_memory->read_u8(addr + 5);
        auto usage_r = m_memory->read_u8(addr + 6);
        auto usage_index_r = m_memory->read_u8(addr + 7);
        if (stream_r.is_err() || offset_r.is_err() || type_r.is_err() ||
            method_r.is_err() || usage_r.is_err() || usage_index_r.is_err()) {
            rejection = "an unreadable D3DVERTEXELEMENT9 array";
            return false;
        }

        VertexDeclarationState::RawElement raw{};
        raw.stream = stream_r.value();
        raw.offset = offset_r.value();
        raw.type = type_r.value();
        raw.method = method_r.value();
        raw.usage = usage_r.value();
        raw.usage_index = usage_index_r.value();
        out.raw.push_back(raw);

        if (raw.stream == D3DDECL_END_STREAM) {
            break;
        }
        if (i == kMaxElements) {
            rejection = "a D3DVERTEXELEMENT9 array without D3DDECL_END";
            return false;
        }
        if (raw.method != 0) {
            rejection = "a non-default D3DDECLMETHOD";
            return false;
        }
        if (raw.stream >= D3D9_MAX_STREAMS) {
            rejection = "a D3DVERTEXELEMENT9 stream index out of range";
            return false;
        }

        graphics::BackendVertexFormat format =
            graphics::BackendVertexFormat::Float4;
        u32 type_bytes = 0;
        bool emit_attribute = true;
        switch (raw.type) {
        case D3DDECLTYPE_FLOAT1:
            format = graphics::BackendVertexFormat::Float1;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_FLOAT2:
            format = graphics::BackendVertexFormat::Float2;
            type_bytes = 8;
            break;
        case D3DDECLTYPE_FLOAT3:
            format = graphics::BackendVertexFormat::Float3;
            type_bytes = 12;
            break;
        case D3DDECLTYPE_FLOAT4:
            format = graphics::BackendVertexFormat::Float4;
            type_bytes = 16;
            break;
        case D3DDECLTYPE_D3DCOLOR:
            format = graphics::BackendVertexFormat::UByte4NormalizedBGRA;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_UBYTE4N:
            // D3DCOLOR is the exceptional packed BGRA input. UBYTE4N keeps
            // the byte order supplied by the stream and merely normalizes
            // each component by 255.
            format = graphics::BackendVertexFormat::UByte4Normalized;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_UBYTE4:
            format = graphics::BackendVertexFormat::UByte4;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_SHORT2:
            format = graphics::BackendVertexFormat::Short2;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_SHORT4:
            format = graphics::BackendVertexFormat::Short4;
            type_bytes = 8;
            break;
        case D3DDECLTYPE_SHORT2N:
            format = graphics::BackendVertexFormat::Short2Normalized;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_SHORT4N:
            format = graphics::BackendVertexFormat::Short4Normalized;
            type_bytes = 8;
            break;
        case D3DDECLTYPE_USHORT2N:
            format = graphics::BackendVertexFormat::UShort2Normalized;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_USHORT4N:
            format = graphics::BackendVertexFormat::UShort4Normalized;
            type_bytes = 8;
            break;
        case D3DDECLTYPE_DEC3N:
            // Learn: signed 10:10:10 normalized, expanded to
            // (v/511, v/511, v/511, 1). Metal Int1010102Normalized.
            format = graphics::BackendVertexFormat::Dec3N;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_FLOAT16_2:
            format = graphics::BackendVertexFormat::Half2;
            type_bytes = 4;
            break;
        case D3DDECLTYPE_FLOAT16_4:
            format = graphics::BackendVertexFormat::Half4;
            type_bytes = 8;
            break;
        case D3DDECLTYPE_UNUSED:
            // Learn: type unused (also the D3DDECL_END type). With DEFAULT
            // method, treat as stream padding — size 0, no Metal attribute;
            // offset can still extend the stream stride.
            emit_attribute = false;
            type_bytes = 0;
            break;
        default: {
            // Keep the numeric type in the rejection so logs name the
            // missing D3DDECLTYPE without a second decode pass.
            static thread_local char unsupported_type[64];
            std::snprintf(unsupported_type, sizeof(unsupported_type),
                          "an unsupported D3DDECLTYPE (%u)",
                          static_cast<unsigned>(raw.type));
            VITREO_TRACE("[d3d9] parse_vertex_declaration: %s\n",
                         unsupported_type);
            rejection = unsupported_type;
            return false;
        }
        }

        if (emit_attribute) {
            out.elements.push_back({raw.usage, raw.usage_index, raw.offset,
                                    format, raw.stream});
        }
        const u32 end = static_cast<u32>(raw.offset) + type_bytes;
        if (end > out.stream_strides[raw.stream]) {
            out.stream_strides[raw.stream] = end;
        }
    }

    if (out.raw.empty() ||
        out.raw.back().stream != D3DDECL_END_STREAM) {
        rejection = "a D3DVERTEXELEMENT9 array without D3DDECL_END";
        return false;
    }
    bool has_stream0_attribute = false;
    for (const auto& element : out.elements) {
        if (element.stream == 0) {
            has_stream0_attribute = true;
            break;
        }
    }
    if (!has_stream0_attribute || out.stream_strides[0] == 0) {
        rejection = "a vertex declaration with no stream-0 elements";
        return false;
    }
    return true;
}

void D3D9Manager::dispatch_vertex_declaration(Marshaler& m, usize slot,
                                              u32 self) {
    if (dispatch_unknown(m, slot, self, D3D9InterfaceId::VertexDeclaration)) {
        return;
    }

    ComObject* entry = find(self, D3D9InterfaceId::VertexDeclaration);
    if (entry == nullptr || !entry->vertex_declaration) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    VertexDeclarationState& decl = *entry->vertex_declaration;

    switch (slot) {
    case 3: { // GetDevice
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || entry->device == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)add_ref(entry->device);
        if (m.write_u32(out, entry->device).is_err()) {
            (void)release(entry->device);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 4: { // GetDeclaration
        const GuestAddr elements_out = m.arg_ptr(1);
        const GuestAddr count_out = m.arg_ptr(2);
        if (count_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const u32 count = static_cast<u32>(decl.raw.size());
        if (m.write_u32(count_out, count).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (elements_out != 0) {
            for (u32 i = 0; i < count; ++i) {
                const auto& raw = decl.raw[i];
                const GuestAddr addr =
                    elements_out + static_cast<GuestAddr>(i * 8);
                if (m.write_u16(addr, raw.stream).is_err() ||
                    m.write_u16(addr + 2, raw.offset).is_err() ||
                    m.write_u8(addr + 4, raw.type).is_err() ||
                    m.write_u8(addr + 5, raw.method).is_err() ||
                    m.write_u8(addr + 6, raw.usage).is_err() ||
                    m.write_u8(addr + 7, raw.usage_index).is_err()) {
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

} // namespace vitreo::winapi

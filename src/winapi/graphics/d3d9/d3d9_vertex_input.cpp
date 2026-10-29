// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_vertex_input.cpp
/// @brief IDirect3DDevice9 vertex-processing mode, draw entry points, vertex
///        declarations and FVF, stream sources and indices.
///
/// Vtable slots 77-90 and 100-105 of IDirect3DDevice9, routed here from
/// `D3D9Manager::dispatch_device` in d3d9_device_dispatch.cpp.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace vitreo::winapi {

/// IDirect3DDevice9 vtable slots: vertex-processing mode, the draw entry
/// points, vertex declarations and FVF, stream sources and indices.
void D3D9Manager::dispatch_device_vertex_input(Marshaler& m, usize slot,
                                               u32 self,
                                               DeviceState& state) {
    switch (slot) {
    case 77: // SetSoftwareVertexProcessing
        state.software_vertex_processing =
            static_cast<u32>(m.arg(1)) != 0 ? 1 : 0;
        m.set_return(D3D_OK);
        return;
    case 78: // GetSoftwareVertexProcessing (BOOL return)
        m.set_return(state.software_vertex_processing);
        return;
    case 79: // SetNPatchMode
        state.npatch_mode = static_cast<u32>(m.arg(1));
        m.set_return(D3D_OK);
        return;
    case 80: // GetNPatchMode (float return)
        m.set_return(state.npatch_mode);
        return;

    case 81: // DrawPrimitive
        draw_primitive(m, self, false);
        return;
    case 82: // DrawIndexedPrimitive
        draw_primitive(m, self, true);
        return;
    case 83: // DrawPrimitiveUP
        draw_primitive_up(m, self, false);
        return;
    case 84: // DrawIndexedPrimitiveUP
        draw_primitive_up(m, self, true);
        return;
    case 85: // ProcessVertices
        process_vertices(m, self);
        return;

    case 86: { // CreateVertexDeclaration
        const GuestAddr elements_addr = m.arg_ptr(1);
        const GuestAddr out = m.arg_ptr(2);
        if (out != 0) (void)m.write_u32(out, 0);
        if (elements_addr == 0 || out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        auto decl = std::make_unique<VertexDeclarationState>();
        const char* rejection = "an unsupported vertex declaration";
        if (!parse_vertex_declaration(elements_addr, *decl, rejection)) {
            // Always-on: a failed declaration leaves CachedVertexFormat
            // +0x14 NULL; the guest then Unlock-crashes. Name the type.
            std::fprintf(stderr,
                         "[d3d9] CreateVertexDeclaration FAILED %s "
                         "elements=",
                         rejection);
            if (m_memory != nullptr) {
                for (usize i = 0; i < 16; ++i) {
                    const GuestAddr addr =
                        elements_addr + static_cast<GuestAddr>(i * 8);
                    const auto stream = m_memory->read_u16(addr);
                    const auto type = m_memory->read_u8(addr + 4);
                    const auto usage = m_memory->read_u8(addr + 6);
                    if (stream.is_err() || type.is_err() || usage.is_err()) {
                        std::fprintf(stderr, " <unreadable>");
                        break;
                    }
                    if (stream.value() == 0xFF) {
                        break;
                    }
                    std::fprintf(stderr, " t%u/u%u", type.value(),
                                 usage.value());
                }
            }
            std::fprintf(stderr, "\n");
            m.set_return(E_NOTIMPL_D3D);
            return;
        }
        const u32 stream0_stride = decl->stream_strides[0];
        const usize element_count = decl->raw.size();
        if (d3d9_state_coverage_enabled_for_declarations()) {
            std::string signature =
                "stride0=" + std::to_string(stream0_stride) + " |";
            for (const auto& element : decl->raw) {
                if (element.stream == 0xFF) continue;  // END marker
                signature += " s" + std::to_string(element.stream) +
                             "+" + std::to_string(element.offset) +
                             " t" + std::to_string(element.type) +
                             " m" + std::to_string(element.method) +
                             " u" + std::to_string(element.usage) +
                             ":" + std::to_string(element.usage_index);
            }
            record_d3d9_vertex_declaration(signature);
        }
        const u32 object =
            alloc_com_object(D3D9InterfaceId::VertexDeclaration, self);
        if (object == 0) {
            m.set_return(E_OUTOFMEMORY_D3D);
            return;
        }
        ComObject* decl_entry =
            find(object, D3D9InterfaceId::VertexDeclaration);
        decl_entry->vertex_declaration = std::move(decl);
        if (m.write_u32(out, object).is_err()) {
            destroy_object(object);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        VITREO_TRACE("[d3d9] CreateVertexDeclaration -> 0x%08x "
                     "stream0_stride=%u elements=%zu\n",
                     object, stream0_stride, element_count);
        m.set_return(D3D_OK);
        return;
    }
    case 87: { // SetVertexDeclaration
        const u32 decl = static_cast<u32>(m.arg(1));
        if (decl != 0 &&
            find(decl, D3D9InterfaceId::VertexDeclaration) == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.vertex_declaration != 0) {
            (void)release(state.vertex_declaration);
        }
        state.vertex_declaration = decl;
        if (decl != 0) {
            (void)add_ref(decl);
            // Windows clears FVF when a declaration is bound.
            state.fvf = 0;
        }
        if (state.recording_state_block) {
            state.recording_block.has_vertex_declaration = true;
            state.recording_block.vertex_declaration = decl;
            state.recording_block.has_fvf = true;
            state.recording_block.fvf = state.fvf;
        }
        m.set_return(D3D_OK);
        return;
    }
    case 88: { // GetVertexDeclaration
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.vertex_declaration != 0) {
            (void)add_ref(state.vertex_declaration);
        }
        if (m.write_u32(out, state.vertex_declaration).is_err()) {
            if (state.vertex_declaration != 0) {
                (void)release(state.vertex_declaration);
            }
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 89: // SetFVF
        state.fvf = static_cast<u32>(m.arg(1));
        if (state.fvf != 0 && state.vertex_declaration != 0) {
            (void)release(state.vertex_declaration);
            state.vertex_declaration = 0;
        }
        if (state.recording_state_block) {
            state.recording_block.has_fvf = true;
            state.recording_block.fvf = state.fvf;
            state.recording_block.has_vertex_declaration = true;
            state.recording_block.vertex_declaration =
                state.vertex_declaration;
        }
        m.set_return(D3D_OK);
        return;
    case 90: { // GetFVF
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0 || m.write_u32(out, state.fvf).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 100: { // SetStreamSource
        const u32 stream = static_cast<u32>(m.arg(1));
        const u32 buffer = static_cast<u32>(m.arg(2));
        const u32 offset = static_cast<u32>(m.arg(3));
        const u32 stride = static_cast<u32>(m.arg(4));
        if (stream >= D3D9_MAX_STREAMS) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // Windows requires the stride to be zero exactly when the stream is
        // being detached.
        if ((buffer == 0) != (stride == 0)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (buffer != 0 &&
            find(buffer, D3D9InterfaceId::VertexBuffer) == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        DeviceState::StreamSource& source = state.streams[stream];
        if (source.buffer != buffer) {
            if (buffer != 0) (void)add_ref(buffer);
            if (source.buffer != 0) (void)release(source.buffer);
            source.buffer = buffer;
        }
        if (source.stride != stride) {
            state.invalidate_draw_pipeline_reuse();
        }
        source.offset = offset;
        source.stride = stride;
        m.set_return(D3D_OK);
        return;
    }

    case 101: { // GetStreamSource
        const u32 stream = static_cast<u32>(m.arg(1));
        const GuestAddr buffer_out = m.arg_ptr(2);
        const GuestAddr offset_out = m.arg_ptr(3);
        const GuestAddr stride_out = m.arg_ptr(4);
        if (stream >= D3D9_MAX_STREAMS || buffer_out == 0 ||
            offset_out == 0 || stride_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        const DeviceState::StreamSource& source = state.streams[stream];
        if (source.buffer != 0) (void)add_ref(source.buffer);
        if (m.write_u32(buffer_out, source.buffer).is_err() ||
            m.write_u32(offset_out, source.offset).is_err() ||
            m.write_u32(stride_out, source.stride).is_err()) {
            if (source.buffer != 0) (void)release(source.buffer);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 102: { // SetStreamSourceFreq
        const u32 stream = static_cast<u32>(m.arg(1));
        const u32 setting = static_cast<u32>(m.arg(2));
        if (stream >= D3D9_MAX_STREAMS) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.streams[stream].frequency != setting) {
            state.streams[stream].frequency = setting;
            state.invalidate_draw_pipeline_reuse();
        }
        m.set_return(D3D_OK);
        return;
    }

    case 103: { // GetStreamSourceFreq
        const u32 stream = static_cast<u32>(m.arg(1));
        const GuestAddr out = m.arg_ptr(2);
        if (stream >= D3D9_MAX_STREAMS || out == 0 ||
            m.write_u32(out, state.streams[stream].frequency).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 104: { // SetIndices
        const u32 buffer = static_cast<u32>(m.arg(1));
        if (buffer != 0 &&
            find(buffer, D3D9InterfaceId::IndexBuffer) == nullptr) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.indices != buffer) {
            if (buffer != 0) (void)add_ref(buffer);
            if (state.indices != 0) (void)release(state.indices);
            state.indices = buffer;
        }
        m.set_return(D3D_OK);
        return;
    }

    case 105: { // GetIndices
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (state.indices != 0) (void)add_ref(state.indices);
        if (m.write_u32(out, state.indices).is_err()) {
            if (state.indices != 0) (void)release(state.indices);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi

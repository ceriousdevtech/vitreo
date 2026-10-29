// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_draw_up.cpp
/// @brief D3D9 user-pointer draws (DrawPrimitiveUP / DrawIndexedPrimitiveUP)
///        and the transient host buffers they feed.

#include "winapi/graphics/d3d9/d3d9_draw_internal.hpp"
#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <vector>

namespace vitreo::winapi {

using namespace d3d9_draw_detail;

// =============================================================================
// Draw
// =============================================================================

u32 D3D9Manager::create_host_buffer(u32 device, bool index_buffer, u32 length,
                                    u32 fvf_or_format, const void* data) {
    if (length == 0 || data == nullptr || m_backend == nullptr ||
        !m_backend->available()) {
        return 0;
    }
    graphics::BackendBufferDesc desc{};
    desc.byte_width = length;
    desc.vertex_buffer = !index_buffer;
    desc.index_buffer = index_buffer;
    desc.cpu_writable = true;
    auto backend_buffer = m_backend->create_buffer(desc, nullptr);
    if (!backend_buffer || !backend_buffer->valid()) return 0;

    const u32 object = alloc_com_object(
        index_buffer ? D3D9InterfaceId::IndexBuffer
                     : D3D9InterfaceId::VertexBuffer,
        device);
    if (object == 0) return 0;
    ComObject* entry = find_any(object);
    entry->buffer = std::make_unique<BufferState>();
    BufferState& buffer = *entry->buffer;
    buffer.index_buffer = index_buffer;
    buffer.length = length;
    buffer.fvf_or_format = fvf_or_format;
    buffer.pool = D3DPOOL_DEFAULT;
    buffer.backend = std::move(backend_buffer);
    buffer.storage = map_lock_region(length);
    if (buffer.storage == 0) {
        destroy_object(object);
        return 0;
    }
    if (m_memory == nullptr ||
        m_memory
            ->write_bytes(buffer.storage, static_cast<const Byte*>(data),
                          length)
            .is_err() ||
        !buffer.backend->update(0, data, length)) {
        destroy_object(object);
        return 0;
    }
    return object;
}

u32 D3D9Manager::create_up_buffer(u32 device, bool index_buffer, u32 length,
                                  u32 fvf_or_format, GuestAddr data,
                                  Marshaler& m) {
    if (length == 0 || data == 0) {
        return 0;
    }
    std::vector<u8> bytes(length);
    if (m.memory().read_bytes(data, bytes.data(), length).is_err()) {
        return 0;
    }
    return create_host_buffer(device, index_buffer, length, fvf_or_format,
                              bytes.data());
}

void D3D9Manager::draw_primitive_up(Marshaler& m, u32 self, bool indexed) {
    // Learn: DrawPrimitiveUP / DrawIndexedPrimitiveUP — copy user memory into
    // temp VB/IB, enqueue like Draw*, then reset stream 0 (and indices).
    // Snapshot `retained` keeps the temps alive until the worker finishes.
    ComObject* entry = find(self, D3D9InterfaceId::Device);
    if (entry == nullptr || !entry->device_state) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    DeviceState& state = *entry->device_state;
    if (state.lost) {
        m.set_return(D3DERR_DEVICELOST);
        return;
    }
    if (!state.in_scene) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    const u32 primitive_type = static_cast<u32>(m.arg(1));
    u32 primitive_count = 0;
    u32 num_vertices = 0;
    u32 min_vertex_index = 0;
    GuestAddr vertex_data = 0;
    u32 stride = 0;
    GuestAddr index_data = 0;
    u32 index_format = 0;
    if (indexed) {
        min_vertex_index = static_cast<u32>(m.arg(2));
        num_vertices = static_cast<u32>(m.arg(3));
        primitive_count = static_cast<u32>(m.arg(4));
        index_data = m.arg_ptr(5);
        index_format = static_cast<u32>(m.arg(6));
        vertex_data = m.arg_ptr(7);
        stride = static_cast<u32>(m.arg(8));
        if (num_vertices == 0 || index_data == 0 ||
            (index_format != D3DFMT_INDEX16 && index_format != D3DFMT_INDEX32)) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
    } else {
        primitive_count = static_cast<u32>(m.arg(2));
        vertex_data = m.arg_ptr(3);
        stride = static_cast<u32>(m.arg(4));
        num_vertices = vertices_for_primitives(primitive_type, primitive_count);
    }

    if (primitive_count == 0 || vertex_data == 0 || stride == 0 ||
        num_vertices == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    const u32 vertex_bytes = num_vertices * stride;
    const u32 temp_vb =
        create_up_buffer(self, false, vertex_bytes, state.fvf, vertex_data, m);
    if (temp_vb == 0) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }

    u32 temp_ib = 0;
    if (indexed) {
        const u32 index_count =
            vertices_for_primitives(primitive_type, primitive_count);
        const u32 index_stride = index_format == D3DFMT_INDEX32 ? 4u : 2u;
        const u32 index_bytes = index_count * index_stride;
        temp_ib = create_up_buffer(self, true, index_bytes, index_format,
                                   index_data, m);
        if (temp_ib == 0) {
            (void)release(temp_vb);
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
    }

    // Replace stream 0 (and indices for indexed UP) for the draw.
    if (state.streams[0].buffer != 0) {
        (void)release(state.streams[0].buffer);
    }
    state.streams[0] = DeviceState::StreamSource{temp_vb, 0, stride, 1};
    (void)add_ref(temp_vb);
    if (indexed) {
        if (state.indices != 0) (void)release(state.indices);
        state.indices = temp_ib;
        (void)add_ref(temp_ib);
    }

    DeviceState::UpDrawOverride up{};
    up.primitive_type = primitive_type;
    up.primitive_count = primitive_count;
    up.first_vertex = 0;
    up.index_start = 0;
    // pVertexData[0] is vertex MinVertexIndex, so indices need that bias.
    up.base_vertex = indexed ? -static_cast<i32>(min_vertex_index) : 0;
    state.up_override = up;

    draw_primitive(m, self, indexed);
    state.up_override.reset();

    // Learn: after Draw*UP, stream 0 is NULL; indexed UP also clears indices.
    if (state.streams[0].buffer != 0) {
        (void)release(state.streams[0].buffer);
    }
    state.streams[0] = {};
    (void)release(temp_vb);
    if (indexed) {
        if (state.indices != 0) (void)release(state.indices);
        state.indices = 0;
        (void)release(temp_ib);
    }
}

} // namespace vitreo::winapi

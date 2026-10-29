// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d9_buffer.cpp
/// @brief IDirect3DVertexBuffer9 and IDirect3DIndexBuffer9: creation and
///        dispatch.

#include "winapi/graphics/d3d9/d3d9_state.hpp"

#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace vitreo::winapi {
namespace {

[[nodiscard]] bool d3d9_buffer_diag_enabled() noexcept {
    static const bool enabled =
        std::getenv("VITREO_D3D9_BUFFER_DIAG") != nullptr;
    return enabled;
}

} // namespace

// =============================================================================
// IDirect3DVertexBuffer9 / IDirect3DIndexBuffer9
// =============================================================================

void D3D9Manager::create_buffer(Marshaler& m, u32 device, bool index_buffer) {
    const u32 length = static_cast<u32>(m.arg(1));
    const u32 usage = static_cast<u32>(m.arg(2));
    const u32 fvf_or_format = static_cast<u32>(m.arg(3));
    const u32 pool = static_cast<u32>(m.arg(4));
    const GuestAddr out = m.arg_ptr(5);
    const GuestAddr shared_handle = m.arg_ptr(6);

    if (out != 0) (void)m.write_u32(out, 0);
    if (length == 0 || out == 0) {
        std::fprintf(stderr,
                     "[d3d9] Create%sBuffer FAILED INVALIDCALL "
                     "length=%u out=0x%llx\n",
                     index_buffer ? "Index" : "Vertex", length,
                     static_cast<unsigned long long>(out));
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (shared_handle != 0) {
        std::fprintf(stderr,
                     "[d3d9] Create%sBuffer FAILED E_NOTIMPL "
                     "(shared handle) length=%u\n",
                     index_buffer ? "Index" : "Vertex", length);
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
    if (index_buffer && fvf_or_format != D3DFMT_INDEX16 &&
        fvf_or_format != D3DFMT_INDEX32) {
        std::fprintf(stderr,
                     "[d3d9] CreateIndexBuffer FAILED INVALIDCALL "
                     "format=0x%x length=%u\n",
                     fvf_or_format, length);
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    if (m_backend == nullptr || !m_backend->available()) {
        std::fprintf(stderr,
                     "[d3d9] Create%sBuffer FAILED NOTAVAILABLE length=%u\n",
                     index_buffer ? "Index" : "Vertex", length);
        m.set_return(D3DERR_NOTAVAILABLE);
        return;
    }

    graphics::BackendBufferDesc desc{};
    desc.byte_width = length;
    desc.vertex_buffer = !index_buffer;
    desc.index_buffer = index_buffer;
    desc.cpu_writable = true;

    auto backend_buffer = m_backend->create_buffer(desc, nullptr);
    if (!backend_buffer || !backend_buffer->valid()) {
        std::fprintf(stderr,
                     "[d3d9] Create%sBuffer FAILED OUTOFVIDEOMEMORY "
                     "length=%u\n",
                     index_buffer ? "Index" : "Vertex", length);
        m.set_return(D3DERR_OUTOFVIDEOMEMORY);
        return;
    }

    const u32 object = alloc_com_object(
        index_buffer ? D3D9InterfaceId::IndexBuffer
                     : D3D9InterfaceId::VertexBuffer,
        device);
    if (object == 0) {
        std::fprintf(stderr,
                     "[d3d9] Create%sBuffer FAILED E_OUTOFMEMORY "
                     "(COM object) length=%u\n",
                     index_buffer ? "Index" : "Vertex", length);
        m.set_return(E_OUTOFMEMORY_D3D);
        return;
    }
    ComObject* entry = find_any(object);
    entry->buffer = std::make_unique<BufferState>();
    BufferState& buffer = *entry->buffer;
    buffer.index_buffer = index_buffer;
    buffer.length = length;
    buffer.usage = usage;
    buffer.fvf_or_format = fvf_or_format;
    buffer.pool = pool;
    buffer.backend = std::move(backend_buffer);

    // D3D9 Lock returns a pointer into buffer memory that stays valid for the
    // application's use, so the mapping is owned by the buffer, not the lock.
    buffer.storage = map_lock_region(length);
    if (buffer.storage == 0) {
        destroy_object(object);
        std::fprintf(stderr,
                     "[d3d9] Create%sBuffer FAILED E_OUTOFMEMORY "
                     "(map %u bytes)\n",
                     index_buffer ? "Index" : "Vertex", length);
        m.set_return(E_OUTOFMEMORY_D3D);
        return;
    }

    if (m.write_u32(out, object).is_err()) {
        destroy_object(object);
        m.set_return(E_POINTER_D3D);
        return;
    }
    VITREO_TRACE("[d3d9] Create%sBuffer -> buffer=0x%08x length=%u "
                 "usage=0x%x pool=%u storage=0x%08llx\n",
                 index_buffer ? "Index" : "Vertex", object, length, usage,
                 pool, static_cast<unsigned long long>(buffer.storage));
    m.set_return(D3D_OK);
}

/// Lock/Unlock/GetDesc are identical for the two buffer interfaces apart from
/// the descriptor they report.
void D3D9Manager::dispatch_buffer(Marshaler& m, usize slot, u32 self,
                                  bool index_buffer) {
    const D3D9InterfaceId iface = index_buffer
                                      ? D3D9InterfaceId::IndexBuffer
                                      : D3D9InterfaceId::VertexBuffer;
    if (dispatch_unknown(m, slot, self, iface)) return;
    if (dispatch_resource_base(
            m, slot, self,
            index_buffer ? D3DRTYPE_INDEXBUFFER : D3DRTYPE_VERTEXBUFFER)) {
        return;
    }

    ComObject* entry = find(self, iface);
    if (entry == nullptr || !entry->buffer) {
        m.set_return(D3DERR_INVALIDCALL);
        return;
    }
    BufferState& buffer = *entry->buffer;

    switch (slot) {
    case 11: { // Lock
        const u32 offset = static_cast<u32>(m.arg(1));
        u32 size = static_cast<u32>(m.arg(2));
        const GuestAddr data_out = m.arg_ptr(3);
        const u32 flags = static_cast<u32>(m.arg(4));
        if (data_out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        (void)m.write_u32(data_out, 0);
        if (buffer.locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        // D3DLOCK (learn.microsoft.com/windows/win32/direct3d9/d3dlock),
        // D3DLOCK_NOOVERWRITE: "Indicates that memory that was referred to in
        // a drawing call since the last lock without this flag will not be
        // modified during the lock. ... Specifying this flag enables the
        // driver to return immediately if the resource is in use, otherwise,
        // the driver must finish using the resource before returning from
        // locking."
        //
        // The wait is therefore required for a plain lock and is not owed for
        // NOOVERWRITE. The app's guarantee is what makes skipping it safe:
        // Unlock writes the locked sub-range in place and never rotates for
        // NOOVERWRITE, so a draw that is still queued keeps reading the same
        // MTLBuffer, in a region the app has promised not to touch.
        //
        // DISCARD is deliberately NOT exempted, though the same doc lets it
        // return a fresh region without waiting -- not even with NOOVERWRITE
        // beside it, since Unlock then renames (DISCARD wins).
        // d3d9_cs_drain() here is an *encode* barrier, and draws resolve
        // their MTLBuffer/offset when the submit worker encodes them, not
        // when they are enqueued. Rotating on DISCARD while draws are still
        // queued would move those draws onto the new slice. That is the
        // geometry-at-infinity failure documented in metal_buffer.mm (the
        // DISCARD pool). Honouring DISCARD's no-wait contract requires
        // binding buffer+offset at enqueue time.
        if ((flags & D3DLOCK_NOOVERWRITE) == 0 ||
            (flags & D3DLOCK_DISCARD) != 0) {
            d3d9_cs_drain();
        }
        // Offset and size both zero means the whole buffer.
        if (offset == 0 && size == 0) size = buffer.length;
        if (static_cast<u64>(offset) + size > buffer.length) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        if (m.write_u32(data_out,
                        static_cast<u32>(buffer.storage) + offset).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        buffer.locked = true;
        buffer.lock_offset = offset;
        buffer.lock_size = size;
        buffer.lock_flags = flags;
        if (d3d9_buffer_diag_enabled()) {
            std::fprintf(stderr,
                         "[d3d9-buffer-diag] lock buffer=0x%08x index=%u "
                         "offset=%u size=%u flags=0x%x length=%u\n",
                         self, index_buffer ? 1u : 0u, offset, size, flags,
                         buffer.length);
        }
        m.set_return(D3D_OK);
        return;
    }
    case 12: { // Unlock
        if (!buffer.locked) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        buffer.locked = false;
        if (d3d9_buffer_diag_enabled()) {
            std::fprintf(stderr,
                         "[d3d9-buffer-diag] unlock buffer=0x%08x index=%u "
                         "offset=%u size=%u flags=0x%x\n",
                         self, index_buffer ? 1u : 0u, buffer.lock_offset,
                         buffer.lock_size, buffer.lock_flags);
        }
        if ((buffer.lock_flags & D3DLOCK_READONLY) == 0 && buffer.backend &&
            buffer.lock_size != 0) {
            const GuestAddr src_addr = buffer.storage + buffer.lock_offset;
            const Byte* src =
                m.memory().host_ptr(src_addr, buffer.lock_size);
            std::vector<u8> fallback;
            if (src == nullptr) {
                fallback.resize(buffer.lock_size);
                if (m.memory()
                        .read_bytes(src_addr, fallback.data(), fallback.size())
                        .is_ok()) {
                    src = fallback.data();
                }
            }
            if (src != nullptr) {
                // D3DLOCK: DISCARD hands the GPU's old storage back only once
                // it is done with it; NOOVERWRITE writes in place by the
                // application's promise; a plain lock "must finish using the
                // resource" first, so work already recorded keeps the old
                // bytes. DISCARD wins when both are given: renaming honours
                // either promise.
                const graphics::BufferWrite mode =
                    (buffer.lock_flags & D3DLOCK_DISCARD) != 0
                        ? graphics::BufferWrite::Discard
                    : (buffer.lock_flags & D3DLOCK_NOOVERWRITE) != 0
                        ? graphics::BufferWrite::NoOverwrite
                        : graphics::BufferWrite::Ordered;
                // Only NOOVERWRITE leaves the storage where it is. Any other
                // write may rename, and the worker resolves storage when it
                // records a draw: draws queued since Lock (on any buffer, by
                // any thread) must be recorded first, and none may record
                // while the rename reclaims pool slabs.
                if (mode != graphics::BufferWrite::NoOverwrite) {
                    d3d9_cs_drain();
                }
                if (!buffer.backend->update(buffer.lock_offset, src,
                                            buffer.lock_size, mode)) {
                    VITREO_TRACE("[d3d9] Unlock: backend rejected %u bytes "
                                 "at offset %u\n",
                                 buffer.lock_size, buffer.lock_offset);
                    m.set_return(D3DERR_INVALIDCALL);
                    return;
                }
            }
        }
        m.set_return(D3D_OK);
        return;
    }
    case 13: { // GetDesc
        const GuestAddr out = m.arg_ptr(1);
        if (out == 0) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        std::array<u32, 6> desc{};
        if (index_buffer) {
            desc[0] = buffer.fvf_or_format;
            desc[1] = D3DRTYPE_INDEXBUFFER;
            desc[2] = buffer.usage;
            desc[3] = buffer.pool;
            desc[4] = buffer.length;
        } else {
            // A vertex buffer always reports D3DFMT_VERTEXDATA.
            desc[0] = 100;
            desc[1] = D3DRTYPE_VERTEXBUFFER;
            desc[2] = buffer.usage;
            desc[3] = buffer.pool;
            desc[4] = buffer.length;
            desc[5] = buffer.fvf_or_format;
        }
        const usize size = index_buffer ? D3DINDEXBUFFER_DESC_SIZE
                                        : D3DVERTEXBUFFER_DESC_SIZE;
        if (m.memory().write_bytes(
                out, reinterpret_cast<const Byte*>(desc.data()),
                size).is_err()) {
            m.set_return(D3DERR_INVALIDCALL);
            return;
        }
        m.set_return(D3D_OK);
        return;
    }
    default:
        m.set_return(E_NOTIMPL_D3D);
        return;
    }
}

void D3D9Manager::dispatch_vertex_buffer(Marshaler& m, usize slot, u32 self) {
    dispatch_buffer(m, slot, self, false);
}

void D3D9Manager::dispatch_index_buffer(Marshaler& m, usize slot, u32 self) {
    dispatch_buffer(m, slot, self, true);
}

} // namespace vitreo::winapi

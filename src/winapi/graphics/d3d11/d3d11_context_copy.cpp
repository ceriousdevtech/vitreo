// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context_copy.cpp
/// @brief ID3D11DeviceContext resource copies and resolves.
///
/// Slots 46 and 115 (CopySubresourceRegion[1]), 47 (CopyResource), 49
/// (CopyStructureCount) and 57 (ResolveSubresource).
/// impl_ContextMethod (d3d11_context.cpp) routes exactly these slots here.
///
/// Split out of d3d11_context.cpp.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_texel_encode.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <array>
#include <span>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <string>
#include <mutex>
#include <set>
#include <vector>

namespace vitreo::winapi {

void D3D11Manager::context_copy_method(Marshaler& m, usize slot) {
    switch (slot) {
    // Slot 115 is ID3D11DeviceContext1::CopySubresourceRegion1, whose first eight
    // arguments are this method's and whose ninth is CopyFlags. Learn
    // (CopySubresourceRegion1): zero "behaves like
    // ID3D11DeviceContext::CopySubresourceRegion". D3D11_COPY_NO_OVERWRITE is
    // the application's promise that no queued work reads the destination,
    // D3D11_COPY_DISCARD that its old contents are not needed: an ordered copy
    // keeps both promises (FlagsForUpdateAndCopySeenByDriver). Measured
    // (d3d11_fl111_probe "copyflags"): 0, 1, 2, 4, 5 and 6 copy; both
    // NO_OVERWRITE and DISCARD together, and any other bit, drop the call.
    case 115:
    case 46: {
        if (slot == 115) {
            const u32 copy_flags = static_cast<u32>(m.arg(9));
            if ((copy_flags & ~0x7u) != 0 || (copy_flags & 0x3u) == 0x3u) {
                d3d11_diag("CopySubresourceRegion1 CopyFlags 0x%x -> dropped",
                           copy_flags);
                m.set_return(0);
                return;
            }
        }
        // CopySubresourceRegion — D3D11_BOX coordinates are bytes for buffers;
        // left/top/front are inclusive and right/bottom/back are exclusive.
        const GuestAddr dst = m.arg_ptr(1);
        const u32 dst_sub = static_cast<u32>(m.arg(2));
        const u32 dst_x = static_cast<u32>(m.arg(3));
        const u32 dst_y = static_cast<u32>(m.arg(4));
        const u32 dst_z = static_cast<u32>(m.arg(5));
        const GuestAddr src = m.arg_ptr(6);
        const u32 src_sub = static_cast<u32>(m.arg(7));
        const GuestAddr src_box = m.arg_ptr(8);
        if (m_diag_transfer_log != nullptr) [[unlikely]] {
            diag_log_staging_copy(slot == 115 ? "CopySubresourceRegion1"
                                              : "CopySubresourceRegion",
                                  dst, dst_sub, dst_x, dst_y, dst_z, src,
                                  src_sub, src_box);
        }
        auto dst_buf = m_buffers.find(dst);
        auto src_buf = m_buffers.find(src);
        if (dst_buf != m_buffers.end() && src_buf != m_buffers.end()) {
            u32 left = 0;
            u32 top = 0;
            u32 front = 0;
            u32 right = src_buf->second.desc.byte_width;
            u32 bottom = 1;
            u32 back = 1;
            const auto read_box_field = [&](usize offset, u32& value) {
                const auto field = m_memory.read_u32(src_box + offset);
                if (field.is_err()) return false;
                value = field.value();
                return true;
            };
            const bool box_ok =
                src_box == 0 ||
                (read_box_field(0, left) && read_box_field(4, top) &&
                 read_box_field(8, front) && read_box_field(12, right) &&
                 read_box_field(16, bottom) && read_box_field(20, back));
            if (!box_ok) {
                d3d11_diag(
                    "CopySubresourceRegion buffer source box unreadable "
                    "box=0x%llx",
                    static_cast<unsigned long long>(src_box));
                m.set_return(0);
                return;
            }
            if (left >= right || top >= bottom || front >= back) {
                d3d11_diag("CopySubresourceRegion buffer empty box -> no-op");
                m.set_return(0);
                return;
            }
            const usize copy_size = static_cast<usize>(right - left);
            // D3D11.3 5.6.5: between two structured buffers the strides must
            // match, and a structured side is copied in whole structures;
            // "the runtime will fail the copy" otherwise (oracle: a
            // partial-structure region is dropped).
            const auto stride_of = [](const Buffer& b) {
                return (b.desc.misc_flags &
                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0
                    ? b.desc.structure_byte_stride : 0u;
            };
            const u32 src_stride = stride_of(src_buf->second);
            const u32 dst_stride = stride_of(dst_buf->second);
            const bool whole_structures =
                (src_stride == 0 || dst_stride == 0 ||
                 src_stride == dst_stride) &&
                (src_stride == 0 ||
                 (left % src_stride == 0 && copy_size % src_stride == 0)) &&
                (dst_stride == 0 ||
                 (dst_x % dst_stride == 0 && copy_size % dst_stride == 0));
            // The same buffer is a legal pair (Learn: "different subresources,
            // although they can be from the same resource"; for a buffer the
            // byte ranges are the subresources), and D3D11.3 5.6.2.1 requires
            // an overlapping copy to behave as a memmove.
            const bool valid =
                dst_sub == 0 && src_sub == 0 &&
                dst_buf->second.desc.usage != D3D11_USAGE_IMMUTABLE &&
                dst_y == 0 && dst_z == 0 && top == 0 && bottom == 1 &&
                front == 0 && back == 1 && whole_structures &&
                right <= src_buf->second.desc.byte_width &&
                dst_x <= dst_buf->second.desc.byte_width &&
                copy_size <= dst_buf->second.desc.byte_width - dst_x &&
                !dst_buf->second.mapped && !src_buf->second.mapped &&
                // A tile pool is not a copy source or destination; a tiled
                // buffer copies through its backend only (no shadow).
                !dst_buf->second.tile_pool && !src_buf->second.tile_pool;
            if (!valid) {
                // Learn specifies undefined behavior for out-of-bounds or
                // incompatible copies. Keep the device alive, do no copy, and
                // make the rejected contract visible in diagnostics.
                d3d11_diag(
                    "CopySubresourceRegion invalid buffer region "
                    "dst=0x%llx+%u src=0x%llx[%u,%u) "
                    "sub=%u/%u yz=%u/%u box_yz=%u..%u/%u..%u",
                    static_cast<unsigned long long>(dst), dst_x,
                    static_cast<unsigned long long>(src), left, right,
                    dst_sub, src_sub, dst_y, dst_z, top, bottom, front, back);
                m.set_return(0);
                return;
            }

            // On the GPU timeline, like CopyResource: the source may hold
            // GPU-written data its shadow never saw, and the copy must be
            // ordered after the work that wrote it. The shadow mirror is kept
            // for CPU paths; a Map(READ) of the destination reads back.
            const bool shadows = !src_buf->second.shadow.empty() &&
                                 !dst_buf->second.shadow.empty();
            const u8* source =
                shadows ? src_buf->second.shadow.data() + left : nullptr;
            bool ok = true;
            const bool overlap = dst == src && dst_x < right &&
                                 left < dst_x + copy_size;
            if (dst_buf->second.backend && src_buf->second.backend &&
                overlap) {
                // A blit may not overlap itself: read the whole range into a
                // transient buffer first, then write it back -- memmove.
                graphics::BackendBufferDesc temp_desc{};
                temp_desc.byte_width = copy_size;
                auto temp = m_backend.create_buffer(temp_desc, nullptr);
                ok = temp != nullptr && temp->valid() &&
                     m_backend.copy_buffer_region(
                         temp.get(), 0, src_buf->second.backend.get(), left,
                         copy_size) &&
                     m_backend.copy_buffer_region(
                         dst_buf->second.backend.get(), dst_x, temp.get(), 0,
                         copy_size);
                if (ok) {
                    dst_buf->second.gpu_newer = true;
                    note_gpu_use(dst_buf->second);
                }
            } else if (dst_buf->second.backend && src_buf->second.backend) {
                ok = m_backend.copy_buffer_region(
                    dst_buf->second.backend.get(), dst_x,
                    src_buf->second.backend.get(), left, copy_size);
                if (ok) {
                    dst_buf->second.gpu_newer = true;
                    note_gpu_use(dst_buf->second);
                    note_gpu_use(src_buf->second);
                }
            } else if (dst_buf->second.backend && source != nullptr) {
                // A copy is a command in the stream, like UpdateSubresource.
                ok = dst_buf->second.backend->update(
                    dst_x, source, copy_size, graphics::BufferWrite::Ordered);
            } else {
                ok = false;
            }
            if (ok && shadows) {
                std::memmove(dst_buf->second.shadow.data() + dst_x,
                             source, copy_size);
                ++dst_buf->second.shadow_version;
            }
            d3d11_diag(
                "CopySubresourceRegion buffers dst=0x%llx+%u "
                "src=0x%llx+%u bytes=%zu -> %s",
                static_cast<unsigned long long>(dst), dst_x,
                static_cast<unsigned long long>(src), left, copy_size,
                ok ? "ok" : "failed");
            m.set_return(0);
            return;
        }
        if (dst_buf != m_buffers.end() || src_buf != m_buffers.end()) {
            d3d11_diag("CopySubresourceRegion incompatible resource types");
            m.set_return(0);
            return;
        }
        auto dst_tex = m_textures.find(dst);
        if (dst_tex != m_textures.end()) ++dst_tex->second.diag_copy_dst;
        auto src_tex = m_textures.find(src);
        if (dst_tex != m_textures.end() && src_tex != m_textures.end()) {
            std::array<u32, 6> box{};
            if (src_box != 0) {
                for (usize index = 0; index < box.size(); ++index) {
                    const auto field =
                        m_memory.read_u32(src_box + index * sizeof(u32));
                    if (field.is_err()) {
                        d3d11_diag("CopySubresourceRegion texture source box "
                                   "unreadable box=0x%llx",
                                   static_cast<unsigned long long>(src_box));
                        m.set_return(0);
                        return;
                    }
                    box[index] = field.value();
                }
            }
            const char* refused = copy_texture_region(
                dst_tex->second, dst_sub, dst_x, dst_y, dst_z,
                src_tex->second, src_sub, src_box != 0 ? &box : nullptr);
            if (m_diag_transfer_log != nullptr) [[unlikely]] {
                diag_log_large_texture_write(
                    "CopySubresourceRegion", dst, dst_sub, dst_x, dst_y,
                    dst_z, 0, src, src_sub, src_box, 0, 0,
                    refused != nullptr ? refused : "ok");
            }
            d3d11_diag(
                "CopySubresourceRegion textures dst=0x%llx[%u]+(%u,%u,%u) "
                "src=0x%llx[%u] -> %s",
                static_cast<unsigned long long>(dst), dst_sub, dst_x, dst_y,
                dst_z, static_cast<unsigned long long>(src), src_sub,
                refused != nullptr ? refused : "ok");
            m.set_return(0);
            return;
        }
        if (dst_tex != m_textures.end() || src_tex != m_textures.end()) {
            d3d11_diag("CopySubresourceRegion incompatible resource types");
            m.set_return(0);
            return;
        }
        d3d11_diag("CopySubresourceRegion unsupported resource pair");
        m.set_return(0);
        return;
    }
    case 47: {
        // CopyResource
        const GuestAddr dst = m.arg_ptr(1);
        const GuestAddr src = m.arg_ptr(2);
        if (m_diag_transfer_log != nullptr) [[unlikely]] {
            diag_log_staging_copy("CopyResource", dst, 0, 0, 0, 0, src, 0, 0);
        }
        auto dst_buf = m_buffers.find(dst);
        auto src_buf = m_buffers.find(src);
        if (dst_buf != m_buffers.end() && src_buf != m_buffers.end() &&
            dst_buf->second.backend && src_buf->second.backend) {
            // Learn (CopyResource): not an IMMUTABLE destination, not the
            // same resource, identical dimensions -- which between two
            // structured buffers includes the stride (oracle: 16 -> 8 is
            // dropped; structured -> plain STAGING is the usual readback).
            const auto stride_of = [](const Buffer& b) {
                return (b.desc.misc_flags &
                        D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0
                    ? b.desc.structure_byte_stride : 0u;
            };
            const u32 dst_stride = stride_of(dst_buf->second);
            const u32 src_stride = stride_of(src_buf->second);
            const bool legal =
                dst != src &&
                dst_buf->second.desc.usage != D3D11_USAGE_IMMUTABLE &&
                (dst_stride == 0 || src_stride == 0 ||
                 dst_stride == src_stride) &&
                !dst_buf->second.mapped;
            const bool ok = legal &&
                m_backend.copy_buffer(dst_buf->second.backend.get(),
                                      src_buf->second.backend.get());
            if (ok) {
                // The source's shadow is only what the CPU last gave it; a
                // compute or stream-out write to it lives on the GPU. The copy
                // happens there too, so a later Map(READ) of the destination
                // reads it back from the GPU. A tiled buffer keeps no shadow
                // (d3d11_tiled.cpp), so none moves to or from it.
                if (!dst_buf->second.shadow.empty() &&
                    src_buf->second.shadow.size() ==
                        dst_buf->second.shadow.size()) {
                    dst_buf->second.shadow = src_buf->second.shadow;
                }
                ++dst_buf->second.shadow_version;
                dst_buf->second.gpu_newer = true;
                note_gpu_use(dst_buf->second);
                note_gpu_use(src_buf->second);
            }
            d3d11_diag("CopyResource buffers -> %s", ok ? "ok" : "failed");
            m.set_return(0);
            return;
        }
        auto dst_tex = m_textures.find(dst);
        if (dst_tex != m_textures.end()) ++dst_tex->second.diag_copy_dst;
        auto src_tex = m_textures.find(src);
        if (dst_tex != m_textures.end() && src_tex != m_textures.end()) {
            const bool ok = copy_texture_resource(
                dst_tex->second, src_tex->second);
            d3d11_diag("CopyResource textures -> %s", ok ? "ok" : "failed");
            m.set_return(0);
            return;
        }
        d3d11_diag("CopyResource unsupported resource pair");
        m.set_return(0);
        return;
    }
    case 49: {
        // CopyStructureCount(ID3D11Buffer *pDstBuffer,
        //   UINT DstAlignedByteOffset, ID3D11UnorderedAccessView *pSrcView).
        //
        // Returns void, so an argument it cannot serve is dropped with a
        // diagnostic rather than half-applied. Learn: pSrcView must be a view
        // "of a Structured Buffer resource created with either
        // D3D11_BUFFER_UAV_FLAG_APPEND or D3D11_BUFFER_UAV_FLAG_COUNTER", and
        // the destination is "any buffer resource that other copy commands ...
        // are able to write to".
        const GuestAddr dst_address = m.arg_ptr(1);
        const u32 dst_offset = static_cast<u32>(m.arg(2));
        const GuestAddr src_view_address = m.arg_ptr(3);
        auto dst = m_buffers.find(dst_address);
        auto src_view = m_unordered_access_views.find(src_view_address);
        if (dst == m_buffers.end() || !dst->second.backend ||
            src_view == m_unordered_access_views.end()) {
            d3d11_diag("ID3D11DeviceContext::CopyStructureCount dst=0x%llx "
                       "view=0x%llx -> dropped (unknown object)",
                       static_cast<unsigned long long>(dst_address),
                       static_cast<unsigned long long>(src_view_address));
            m.set_return(0);
            return;
        }
        if (!src_view->second.counter) {
            d3d11_diag("ID3D11DeviceContext::CopyStructureCount view=0x%llx -> "
                       "dropped (no hidden counter: the view was not created "
                       "with APPEND or COUNTER)",
                       static_cast<unsigned long long>(src_view_address));
            m.set_return(0);
            return;
        }
        // The count is a 32-bit UINT, so the destination offset is a dword
        // offset. An unaligned one would split the value across two dwords,
        // which is not a count a guest can read back.
        if ((dst_offset % sizeof(u32)) != 0) {
            d3d11_diag("ID3D11DeviceContext::CopyStructureCount offset=%u -> "
                       "dropped (not 4-byte aligned)", dst_offset);
            m.set_return(0);
            return;
        }
        // On the GPU timeline, not the CPU's: the shader writes that moved the
        // counter are ordered ahead of this copy only if the copy is recorded
        // behind them.
        if (!m_backend.copy_buffer_region(dst->second.backend.get(), dst_offset,
                                          src_view->second.counter.get(), 0,
                                          sizeof(u32))) {
            d3d11_diag("ID3D11DeviceContext::CopyStructureCount dst=0x%llx "
                       "offset=%u -> dropped (backend refused; dst is %zu "
                       "bytes)",
                       static_cast<unsigned long long>(dst_address), dst_offset,
                       static_cast<usize>(dst->second.desc.byte_width));
            m.set_return(0);
            return;
        }
        dst->second.gpu_newer = true;
        note_gpu_use(dst->second);
        d3d11_diag("ID3D11DeviceContext::CopyStructureCount dst=0x%llx "
                   "offset=%u view=0x%llx",
                   static_cast<unsigned long long>(dst_address), dst_offset,
                   static_cast<unsigned long long>(src_view_address));
        m.set_return(0);
        return;
    }
    case 57: {
        // ResolveSubresource
        const GuestAddr dst_address = m.arg_ptr(1);
        const u32 dst_subresource = static_cast<u32>(m.arg(2));
        const GuestAddr src_address = m.arg_ptr(3);
        const u32 src_subresource = static_cast<u32>(m.arg(4));
        const u32 format = static_cast<u32>(m.arg(5));
        auto destination = m_textures.find(dst_address);
        auto source = m_textures.find(src_address);
        bool resolved = false;
        if (destination != m_textures.end() && source != m_textures.end()) {
            const auto& dst = destination->second;
            const auto& src = source->second;
            const u32 dst_subresource_count =
                dst.desc.mip_levels * dst.desc.array_size;
            const u32 src_subresource_count =
                src.desc.mip_levels * src.desc.array_size;
            const u32 dst_mip = dst.desc.mip_levels != 0
                                    ? dst_subresource % dst.desc.mip_levels
                                    : 0;
            const u32 dst_slice = dst.desc.mip_levels != 0
                                      ? dst_subresource / dst.desc.mip_levels
                                      : 0;
            const u32 src_mip = src.desc.mip_levels != 0
                                    ? src_subresource % src.desc.mip_levels
                                    : 0;
            const u32 src_slice = src.desc.mip_levels != 0
                                      ? src_subresource / src.desc.mip_levels
                                      : 0;
            const u32 dst_width = std::max(1u, dst.desc.width >> dst_mip);
            const u32 dst_height = std::max(1u, dst.desc.height >> dst_mip);
            const u32 src_width = std::max(1u, src.desc.width >> src_mip);
            const u32 src_height = std::max(1u, src.desc.height >> src_mip);
            const bool valid =
                dst_subresource < dst_subresource_count &&
                src_subresource < src_subresource_count &&
                dst.device_owner == src.device_owner &&
                dst.desc.usage == D3D11_USAGE_DEFAULT &&
                dst.desc.sample_count == 1 && src.desc.sample_count > 1 &&
                dst.desc.width == src.desc.width &&
                dst.desc.height == src.desc.height &&
                dst_width == src_width && dst_height == src_height &&
                dxgi_resolve_formats_compatible(
                    src.desc.format, dst.desc.format, format) &&
                // D3D11.3 5.6.6: the resolve format must support
                // MULTISAMPLE_RESOLVE; an integer format does not, and the
                // runtime drops the call (a06 probe: RES ... fmt UINT).
                d3d11_texel::is_filterable_color(format) &&
                dst.backend && src.backend;
            if (valid) {
                resolved = m_backend.resolve_texture_2d(
                    dst.backend.get(), dst_mip, dst_slice,
                    src.backend.get(), src_mip, src_slice,
                    backend_format(format));
            }
        }
        d3d11_diag(
            "ResolveSubresource dst=0x%llx[%u] src=0x%llx[%u] fmt=%u -> %s",
            static_cast<unsigned long long>(dst_address), dst_subresource,
            static_cast<unsigned long long>(src_address), src_subresource,
            format, resolved ? "ok" : "ignored");
        m.set_return(0);
        return;
    }
    }
}

} // namespace vitreo::winapi

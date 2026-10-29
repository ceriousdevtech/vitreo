// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_resources.cpp
/// @brief ID3D11Device resource creators: CreateBuffer and
///        CreateTexture1D/2D/3D (vtable slots 3-6).
///
/// CreateTexture2D and CreateTexture3D are also the bodies of
/// ID3D11Device3::CreateTexture2D1 / CreateTexture3D1. Split out of
/// d3d11_device_methods.cpp; impl_DeviceMethod forwards these slots here
/// once it has found the device.

#include "core/memory/host_memory_floor.hpp"
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_vertex_formats.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/gpu_resource.hpp"
#include "graphics/shaders/dxbc_parser.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstring>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace vitreo::winapi {

namespace {
/// Learn (CreateTexture1D/2D/3D, CreateBuffer): E_OUTOFMEMORY when there is
/// not enough memory for the resource. A backend refusal at the host memory
/// floor is exactly that; anything else stays a generic E_FAIL.
[[nodiscard]] u32 resource_creation_failure() noexcept {
    return vitreo::memory::take_host_memory_refusal() ? 0x8007000Eu
                                                      : d3d11::E_FAIL;
}
}  // namespace

void D3D11Manager::device_resource_method(Marshaler& m, usize slot,
                                          GuestAddr self, Device* device) {
    switch (slot) {
    case 3: {
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr initial_data_address = m.arg_ptr(2);
        const GuestAddr buffer_out = m.arg_ptr(3);
        if (buffer_out != 0) (void)m.write_ptr(buffer_out, 0);

        BufferDesc desc{};
        if (!read_buffer_desc(desc_address, desc)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // One rule table for every buffer descriptor
        // (d3d11_format_caps.cpp, buffer_desc_check), measured on Windows
        // (d3d11_a02_create_matrix_probe, d3d11_a02_create_contracts_probe,
        // d3d11_buffer_misc_probe).
        //
        // D3D11_RESOURCE_MISC_FLAG's Remarks claim the misc flags "cannot be
        // used when creating resources with D3D11_CPU_ACCESS flags", and
        // reading that literally refused UE5's dynamic byte-address buffer
        // (DYNAMIC, SRV|VERTEX_BUFFER, CPU_ACCESS_WRITE, ALLOW_RAW_VIEWS) as
        // an engine fatal. Measured, the buffer-SHAPE flags all succeed
        // alongside CPU_ACCESS_WRITE; only sharing conflicts.
        const char* violation = nullptr;
        const u32 check = d3d11::buffer_desc_check(
            d3d11::BufferRuleDesc{
                .byte_width = desc.byte_width,
                .usage = desc.usage,
                .bind_flags = desc.bind_flags,
                .cpu_access_flags = desc.cpu_access_flags,
                .misc_flags = desc.misc_flags,
                .structure_byte_stride = desc.structure_byte_stride,
                .initial_data = initial_data_address != 0,
                .map_on_default_buffers =
                    device->feature_level >= d3d11::FEATURE_LEVEL_11_0,
                .tiled_resources =
                    tiled_resources_tier(device->feature_level) != 0,
            },
            &violation);
        if (check != d3d11::S_OK) {
            d3d11_diag(
                "ID3D11Device::CreateBuffer invalid (%s) "
                "bytes=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x stride=%u "
                "initial=%s",
                violation != nullptr ? violation : "?",
                desc.byte_width, desc.usage, desc.bind_flags,
                desc.cpu_access_flags, desc.misc_flags,
                desc.structure_byte_stride,
                initial_data_address != 0 ? "yes" : "no");
            m.set_return(check);
            return;
        }
        // D3D11_BUFFER_DESC.StructureByteStride is "the size of each element
        // in the buffer structure (in bytes) when the buffer represents a
        // structured buffer"; GetDesc reports 0 otherwise (measured "F BUF VB
        // stride16 non-structured" S=0), and a typed view of such a buffer
        // takes its element size from the view format, not this field.
        if ((desc.misc_flags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) == 0) {
            desc.structure_byte_stride = 0;
        }

        std::vector<u8> initial_bytes;
        if (initial_data_address != 0) {
            GuestAddr data_pointer = 0;
            if (!read_pointer(initial_data_address, data_pointer) ||
                data_pointer == 0) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
            initial_bytes.resize(desc.byte_width);
            if (m_memory.read_bytes(
                    data_pointer, initial_bytes.data(),
                    initial_bytes.size()).is_err()) {
                m.set_return(d3d11::E_INVALIDARG);
                return;
            }
        }
        if (buffer_out == 0) {
            m.set_return(1u);
            return;
        }
        const GuestAddr owner = m_interfaces.find(self)->second.owner;
        const void* const initial =
            initial_bytes.empty() ? nullptr : initial_bytes.data();
        // An ordinary buffer's storage is a pure backend allocation (an
        // IOKit call on Metal) that needs no device state. UE creates
        // thousands of transient buffers a second on worker threads; made
        // under the device lock, each one queued the RHI thread's draws
        // behind it (~80 lock waits a frame at the menu). So
        // the storage is allocated with the lock released and recorded
        // under it; nothing here holds an iterator across the window.
        // Shared, keyed-mutex, tiled and tile-pool buffers keep the locked
        // path in create_buffer.
        constexpr u32 kLockedStorage =
            D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX |
            D3D11_RESOURCE_MISC_TILE_POOL | D3D11_RESOURCE_MISC_TILED;
        std::unique_ptr<graphics::BackendBuffer> storage;
        if ((desc.misc_flags & kLockedStorage) == 0) {
            const graphics::BackendBufferDesc backend_desc =
                backend_buffer_desc(desc);
            DxgiManager::ApiUnlock unlocked(m_dxgi);
            storage = m_backend.create_buffer(backend_desc, initial);
            if (!storage || !storage->valid()) {
                std::fprintf(stderr,
                    "[d3d11] CreateBuffer backend allocation failed: "
                    "bytes=%u\n",
                    desc.byte_width);
                storage.reset();
            }
        }
        if ((desc.misc_flags & kLockedStorage) == 0 && !storage) {
            m.set_return(0x8007000Eu);
            return;
        }
        const GuestAddr buffer =
            create_buffer(owner, desc, initial, std::move(storage), 0);
        if (buffer == 0) {
            m.set_return(0x8007000Eu);
            return;
        }
        if (m.write_ptr(buffer_out, buffer).is_err()) {
            (void)release_object(buffer);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m_buffer_create_callers[buffer] = m.guest_return_address();
        d3d11_diag(
            "ID3D11Device::CreateBuffer -> 0x%llx "
            "bytes=%u usage=%u bind=0x%x misc=0x%x stride=%u initial=%s",
            static_cast<unsigned long long>(buffer),
            desc.byte_width, desc.usage, desc.bind_flags, desc.misc_flags,
            desc.structure_byte_stride,
            initial_bytes.empty() ? "no" : "yes");
        m.set_return(d3d11::S_OK);
        return;
    }
    case 4: {
        // CreateTexture1D(const D3D11_TEXTURE1D_DESC *pDesc,
        //                 const D3D11_SUBRESOURCE_DATA *pInitialData,
        //                 ID3D11Texture1D **ppTexture1D).
        //
        // D3D11_TEXTURE1D_DESC is the 2D one without Height and SampleDesc:
        // Width, MipLevels, ArraySize, Format, Usage, BindFlags,
        // CPUAccessFlags, MiscFlags. The resource is backed by a height-1 2D
        // texture -- see Texture2D::linear for why that beats a Metal 1D
        // texture -- so everything after creation is the 2D path unchanged.
        // The descriptor rules are the 2D ones with Height 1 and one sample
        // (measured, d3d11_a02_create_contracts_probe [B]); a descriptor that
        // skipped them reached Metal's own validation and aborted the host.
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr initial_data_address = m.arg_ptr(2);
        const GuestAddr texture_out = m.arg_ptr(3);
        if (texture_out != 0) (void)m.write_ptr(texture_out, 0);
        std::array<u32, 8> fields{};
        bool readable = desc_address != 0;
        for (usize index = 0; readable && index < fields.size(); ++index) {
            const auto value =
                m_memory.read_u32(desc_address + index * sizeof(u32));
            readable = value.is_ok();
            if (readable) fields[index] = value.value();
        }
        if (!readable) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        Texture2DDesc desc{};
        desc.width = fields[0];
        desc.height = 1;
        desc.mip_levels = fields[1];
        desc.array_size = fields[2];
        desc.format = fields[3];
        desc.usage = fields[4];
        desc.bind_flags = fields[5];
        desc.cpu_access_flags = fields[6];
        desc.misc_flags = fields[7];
        desc.sample_count = 1;
        desc.sample_quality = 0;
        if (const char* violation = d3d11::texture_desc_violation(
                d3d11::TextureRuleDesc{
                    .dimension = 1,
                    .width = desc.width,
                    .height = 1,
                    .depth = 1,
                    .mip_levels = desc.mip_levels,
                    .array_size = desc.array_size,
                    .format = desc.format,
                    .usage = desc.usage,
                    .bind_flags = desc.bind_flags,
                    .cpu_access_flags = desc.cpu_access_flags,
                    .misc_flags = desc.misc_flags,
                    .initial_data = initial_data_address != 0,
                },
                format_caps_context(*device))) {
            d3d11_diag("ID3D11Device::CreateTexture1D REJECT (%s) w=%u "
                       "mips=%u arr=%u fmt=%u usage=%u bind=0x%x cpu=0x%x "
                       "misc=0x%x", violation, desc.width, desc.mip_levels,
                       desc.array_size, desc.format, desc.usage,
                       desc.bind_flags, desc.cpu_access_flags,
                       desc.misc_flags);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // Learn: MipLevels 0 generates the full chain, driven by width alone.
        if (desc.mip_levels == 0) {
            desc.mip_levels = full_mip_levels(desc.width, 1);
        }
        const graphics::PixelFormat pixel_format =
            backend_texture_format(desc.format, desc.bind_flags);
        // Initial data: one D3D11_SUBRESOURCE_DATA per subresource, mip-major
        // within each array slice. SysMemPitch and SysMemSlicePitch have no
        // meaning for a 1D texture (Learn), so a subresource is one row of
        // guest texels (4 or 8 bytes for packed depth+stencil).
        const u32 texel_bytes = staging_bytes_per_pixel(
            desc.format, graphics::format_table(pixel_format).bytes_per_pixel);
        std::vector<std::vector<u8>> initial_rows;
        if (initial_data_address != 0) {
            const u32 subresources = desc.mip_levels * desc.array_size;
            initial_rows.resize(subresources);
            for (u32 index = 0; index < subresources; ++index) {
                const u32 mip = index % desc.mip_levels;
                const usize row_bytes =
                    static_cast<usize>(std::max(1u, desc.width >> mip)) *
                    texel_bytes;
                if (!read_subresource_data(initial_data_address, index,
                                           row_bytes, 1, 1, false, false,
                                           initial_rows[index])) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
            }
        }
        // Learn marks ppTexture1D optional: a null one validates the
        // description (above) without creating anything.
        if (texture_out == 0) {
            m.set_return(1u);  // S_FALSE
            return;
        }
        const GuestAddr texture = create_texture_2d(
            m_interfaces.find(self)->second.owner, desc, nullptr, 0, 0,
            nullptr, 2 /* D3D11_RESOURCE_DIMENSION_TEXTURE1D */);
        if (texture == 0) {
            m.set_return(resource_creation_failure());
            return;
        }
        auto created = m_textures.find(texture);
        if (created == m_textures.end()) {
            m.set_return(d3d11::E_FAIL);
            return;
        }
        created->second.linear = true;
        for (u32 index = 0; index < initial_rows.size(); ++index) {
            const u32 mip = index % desc.mip_levels;
            const u32 slice = index / desc.mip_levels;
            if (!upload_initial_subresource(
                    created->second, mip, slice, initial_rows[index].data(),
                    initial_rows[index].size(), 1)) {
                // Handing back a texture whose contents were never written
                // would have the guest sample whatever the allocation held.
                (void)release_object(texture);
                m.set_return(d3d11::E_FAIL);
                return;
            }
        }
        if (m.write_ptr(texture_out, texture).is_err()) {
            (void)release_object(texture);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag("ID3D11Device::CreateTexture1D -> 0x%llx w=%u mips=%u "
                   "array=%u fmt=%u",
                   static_cast<unsigned long long>(texture), desc.width,
                   desc.mip_levels, desc.array_size, desc.format);
        m.set_return(d3d11::S_OK);
        return;
    }
    case 5: {
        // CreateTexture2D, and the body of ID3D11Device3::CreateTexture2D1,
        // whose D3D11_TEXTURE_LAYOUT arrives in m_pending_texture_layout.
        const u32 layout = std::exchange(m_pending_texture_layout, 0u);
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr initial_data_address = m.arg_ptr(2);
        const GuestAddr texture_out = m.arg_ptr(3);
        if (texture_out != 0) (void)m.write_ptr(texture_out, 0);
        Texture2DDesc desc{};
        if (!read_texture_desc(desc_address, desc)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // One rule table for every texture descriptor
        // (d3d11_format_caps.cpp, texture_desc_violation), measured on
        // Windows (d3d11_a02_create_matrix_probe,
        // d3d11_a02_create_contracts_probe, d3d11_msaa_depth_probe,
        // d3d11_depth_cube_probe). Everything Metal would refuse -- MipLevels
        // beyond the chain, a multisampled block-compressed texture -- is
        // E_INVALIDARG here, before any backend call: Metal's descriptor
        // validation aborts the process instead of failing.
        //
        // Nothing in the contract limits a TEXTURECUBE to sampling: UE5's
        // SkyLight.ConvolvedSkyRenderTarget0 is a RENDER_TARGET |
        // SHADER_RESOURCE | UNORDERED_ACCESS R11G11B10 cube, and a depth cube
        // is the point-light shadow map.
        if (const char* violation = d3d11::texture_desc_violation(
                d3d11::TextureRuleDesc{
                    .dimension = 2,
                    .width = desc.width,
                    .height = desc.height,
                    .depth = 1,
                    .mip_levels = desc.mip_levels,
                    .array_size = desc.array_size,
                    .format = desc.format,
                    .sample_count = desc.sample_count,
                    .sample_quality = desc.sample_quality,
                    .usage = desc.usage,
                    .bind_flags = desc.bind_flags,
                    .cpu_access_flags = desc.cpu_access_flags,
                    .misc_flags = desc.misc_flags,
                    .initial_data = initial_data_address != 0,
                    .layout = layout,
                },
                format_caps_context(*device))) {
            static thread_local u32 rejected_texture_diagnostics = 0;
            if (rejected_texture_diagnostics++ < 32) {
                std::fprintf(stderr,
                    "[d3d11] rejected texture (%s) %ux%u mips=%u array=%u "
                    "format=%u samples=%u quality=%u usage=%u bind=%x cpu=%x "
                    "misc=%x\n",
                    violation, desc.width, desc.height, desc.mip_levels,
                    desc.array_size, desc.format, desc.sample_count,
                    desc.sample_quality, desc.usage, desc.bind_flags,
                    desc.cpu_access_flags, desc.misc_flags);
            }
            d3d11_diag(
                "ID3D11Device::CreateTexture2D REJECT (%s) "
                "%ux%u mips=%u arr=%u fmt=%u samples=%u usage=%u "
                "bind=0x%x cpu=0x%x misc=0x%x", violation,
                desc.width, desc.height, desc.mip_levels, desc.array_size,
                desc.format, desc.sample_count, desc.usage, desc.bind_flags,
                desc.cpu_access_flags, desc.misc_flags);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // Microsoft Learn: MipLevels 0 generates a full set of subtextures.
        if (desc.mip_levels == 0) {
            desc.mip_levels = full_mip_levels(desc.width, desc.height);
        }
        const graphics::PixelFormat pixel_format =
            backend_texture_format(desc.format, desc.bind_flags);
        const auto format = graphics::format_table(pixel_format);

        // Initial data, one D3D11_SUBRESOURCE_DATA per subresource, read in
        // the guest-visible layout: rows of 4x4 blocks for a compressed
        // format, 4 / 8 bytes a texel for packed depth+stencil (whose host
        // footprint differs, staging_bytes_per_pixel).
        struct InitialSubresource {
            std::vector<u8> bytes;
            usize row_bytes{0};
            u32 row_count{0};
        };
        std::vector<InitialSubresource> initial_subresources;
        if (initial_data_address != 0) {
            const u32 block_bytes =
                graphics::compressed_block_bytes(pixel_format);
            const u32 texel_bytes =
                staging_bytes_per_pixel(desc.format, format.bytes_per_pixel);
            const u32 count = desc.mip_levels * desc.array_size;
            initial_subresources.resize(count);
            for (u32 index = 0; index < count; ++index) {
                const u32 mip = index % desc.mip_levels;
                const u32 width = std::max(1u, desc.width >> mip);
                const u32 height = std::max(1u, desc.height >> mip);
                auto& initial = initial_subresources[index];
                if (format.is_compressed) {
                    initial.row_bytes =
                        static_cast<usize>((width + 3u) / 4u) * block_bytes;
                    initial.row_count = (height + 3u) / 4u;
                } else {
                    initial.row_bytes = static_cast<usize>(width) * texel_bytes;
                    // Planar video: the luma rows then the chroma rows (Learn
                    // DXGI_FORMAT NV12).
                    initial.row_count =
                        dxgi_subresource_rows(desc.format, height);
                }
                if (!read_subresource_data(initial_data_address, index,
                                           initial.row_bytes,
                                           initial.row_count, 1, true, false,
                                           initial.bytes)) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
            }
        }
        // Learn CreateTexture2D: a NULL ppTexture2D validates only.
        if (texture_out == 0) {
            m.set_return(1u);  // S_FALSE
            return;
        }
        const GuestAddr texture = create_texture_2d(
            m_interfaces.find(self)->second.owner, desc, nullptr, 0);
        if (texture == 0) {
            m.set_return(resource_creation_failure());
            return;
        }
        if (auto created = m_textures.find(texture);
            created != m_textures.end()) {
            created->second.layout = layout;
        }
        if (!initial_subresources.empty()) {
            auto created = m_textures.find(texture);
            bool initialized = created != m_textures.end();
            for (u32 index = 0;
                 initialized && index < initial_subresources.size(); ++index) {
                const auto& initial = initial_subresources[index];
                initialized = upload_initial_subresource(
                    created->second, index % desc.mip_levels,
                    index / desc.mip_levels, initial.bytes.data(),
                    initial.row_bytes, initial.row_count);
            }
            if (!initialized) {
                (void)release_object(texture);
                m.set_return(d3d11::E_FAIL);
                return;
            }
        }
        if (m.write_ptr(texture_out, texture).is_err()) {
            (void)release_object(texture);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag(
            "ID3D11Device::CreateTexture2D -> 0x%llx %ux%u fmt=%u "
            "bind=0x%x misc=0x%x mips=%u usage=%u initial=%zu",
            static_cast<unsigned long long>(texture),
            desc.width, desc.height, desc.format, desc.bind_flags,
            desc.misc_flags, desc.mip_levels, desc.usage,
            initial_subresources.size());
        m.set_return(d3d11::S_OK);
        return;
    }
    case 6: {
        // CreateTexture3D, and the body of ID3D11Device3::CreateTexture3D1
        // (layout in m_pending_texture_layout, as for case 5).
        const u32 layout = std::exchange(m_pending_texture_layout, 0u);
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr initial_data = m.arg_ptr(2);
        const GuestAddr texture_out = m.arg_ptr(3);
        if (texture_out != 0) (void)m.write_ptr(texture_out, 0);
        Texture3DDesc desc{};
        u32* fields[] = {&desc.width, &desc.height, &desc.depth,
                         &desc.mip_levels, &desc.format, &desc.usage,
                         &desc.bind_flags, &desc.cpu_access_flags,
                         &desc.misc_flags};
        bool readable = desc_address != 0;
        for (u32 i = 0; i < 9 && readable; ++i) {
            const auto value = m_memory.read_u32(desc_address + i * 4u);
            readable = value.is_ok();
            if (readable) *fields[i] = value.value();
        }
        if (!readable) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        // The shared descriptor rules (d3d11_format_caps.cpp), measured on
        // Windows (d3d11_a02_create_contracts_probe [D]): a volume may be
        // DEFAULT with no bind (a copy target), GENERATE_MIPS with
        // RT|SRV|UAV, RESOURCE_CLAMP on DEFAULT / IMMUTABLE, and a
        // block-compressed one needs a multiple-of-4 top level.
        if (const char* violation = d3d11::texture_desc_violation(
                d3d11::TextureRuleDesc{
                    .dimension = 3,
                    .width = desc.width,
                    .height = desc.height,
                    .depth = desc.depth,
                    .mip_levels = desc.mip_levels,
                    .array_size = 1,
                    .format = desc.format,
                    .usage = desc.usage,
                    .bind_flags = desc.bind_flags,
                    .cpu_access_flags = desc.cpu_access_flags,
                    .misc_flags = desc.misc_flags,
                    .initial_data = initial_data != 0,
                    .layout = layout,
                },
                format_caps_context(*device))) {
            d3d11_diag("ID3D11Device::CreateTexture3D REJECT (%s) %ux%ux%u "
                       "mips=%u fmt=%u usage=%u bind=0x%x cpu=0x%x misc=0x%x",
                       violation, desc.width, desc.height, desc.depth,
                       desc.mip_levels, desc.format, desc.usage,
                       desc.bind_flags, desc.cpu_access_flags,
                       desc.misc_flags);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (desc.mip_levels == 0) {
            desc.mip_levels = full_mip_levels(
                desc.width, desc.height, desc.depth);
        }
        const auto pixel_format = d3d11::texture_storage_format(desc.format);
        const auto format = graphics::format_table(pixel_format);
        // Learn D3D11_USAGE: STAGING cannot be bound to any stage and exists
        // for CPU access plus copies; DYNAMIC is CPU write-only.
        const bool staging_usage = desc.usage == D3D11_USAGE_STAGING;
        const bool dynamic_usage = desc.usage == D3D11_USAGE_DYNAMIC;
        const bool cpu_mappable = staging_usage || dynamic_usage;
        // A SHARED / KEYEDMUTEX volume is opened by other devices and
        // processes (measured, d3d11_share_ers_probe "3D ..."): its storage
        // records the creator's description for every opener.
        const bool shared =
            (desc.misc_flags & (D3D11_RESOURCE_MISC_SHARED |
                                D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0;
        const u64 nonce = shared ? new_shared_nonce() : 0u;
        graphics::BackendTexture3DDesc backend_desc{};
        backend_desc.width = desc.width;
        backend_desc.height = desc.height;
        backend_desc.depth = desc.depth;
        backend_desc.mip_levels = desc.mip_levels;
        backend_desc.pixel_format = pixel_format;
        backend_desc.shader_resource =
            (desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0;
        backend_desc.render_target =
            (desc.bind_flags & D3D11_BIND_RENDER_TARGET) != 0;
        backend_desc.unordered_access =
            (desc.bind_flags & D3D11_BIND_UNORDERED_ACCESS) != 0;
        // A TYPELESS volume's views name its type (Learn, "Format
        // Conversion using Direct3D 10.1"): they bind as pixel-format views.
        backend_desc.pixel_format_view = dxgi_format_is_typeless(desc.format);
        backend_desc.cpu_writable = dynamic_usage;
        if (shared && !staging_usage) {
            SharedResourceDesc metadata{};
            metadata.dimension = 4;  // D3D11_RESOURCE_DIMENSION_TEXTURE3D
            metadata.texture = Texture2DDesc{
                desc.width,  desc.height,     desc.mip_levels,
                1,           desc.format,     1,
                0,           desc.usage,      desc.bind_flags,
                desc.cpu_access_flags, desc.misc_flags};
            metadata.depth = desc.depth;
            metadata.nonce = nonce;
            backend_desc.shareable = true;
            backend_desc.shared_metadata = shared_resource_metadata(metadata);
        }
        // A staging volume is CPU memory the GPU only copies to and from
        // (Learn D3D11_USAGE_STAGING); its bytes live in the record's shadow
        // and copies move them through the other resource's backend.
        std::unique_ptr<graphics::BackendTexture3D> backend;
        if (!staging_usage) {
            backend = m_backend.create_texture_3d(backend_desc, nullptr, 0, 0);
            if (!backend || !backend->valid()) {
                m.set_return(resource_creation_failure());
                return;
            }
        }
        std::vector<std::vector<u8>> initial_levels;
        if (initial_data != 0) {
            const u32 block_bytes =
                graphics::compressed_block_bytes(pixel_format);
            initial_levels.resize(desc.mip_levels);
            for (u32 mip = 0; mip < desc.mip_levels; ++mip) {
                const u32 level_width = std::max(1u, desc.width >> mip);
                const u32 level_height = std::max(1u, desc.height >> mip);
                const u32 level_depth = std::max(1u, desc.depth >> mip);
                // A compressed level is measured in 4x4 blocks: RowPitch spans
                // one row of blocks and DepthPitch one W slice of them.
                const usize row_bytes = format.is_compressed
                    ? static_cast<usize>((level_width + 3u) / 4u) * block_bytes
                    : static_cast<usize>(level_width) *
                          format.bytes_per_pixel;
                const u32 row_count = format.is_compressed
                    ? (level_height + 3u) / 4u
                    : level_height;
                auto& bytes = initial_levels[mip];
                if (!read_subresource_data(initial_data, mip, row_bytes,
                                           row_count, level_depth, true, true,
                                           bytes) ||
                    (backend && !backend->update(
                                    mip, bytes.data(),
                                    static_cast<u32>(row_bytes),
                                    static_cast<u32>(row_bytes * row_count)))) {
                    m.set_return(d3d11::E_INVALIDARG);
                    return;
                }
            }
        }
        const GuestAddr owner = m_interfaces.find(self)->second.owner;
        const GuestAddr address = create_texture_3d_object(
            owner, desc, std::move(backend), nullptr, nonce, false);
        if (address == 0) {
            m.set_return(d3d11::E_FAIL);
            return;
        }
        auto& texture = m_textures.find(address)->second;
        texture.layout = layout;
        if (cpu_mappable && !initial_levels.empty()) {
            // Repack the tight rows into the shadow's layout.
            for (u32 mip = 0; mip < desc.mip_levels; ++mip) {
                const auto& storage = texture.subresources[mip];
                const u32 row_count = format.is_compressed
                    ? (storage.height + 3u) / 4u
                    : storage.height;
                const usize row_bytes =
                    initial_levels[mip].size() /
                    (static_cast<usize>(row_count) * storage.depth);
                for (u32 z = 0; z < storage.depth; ++z) {
                    for (u32 row = 0; row < row_count; ++row) {
                        std::memcpy(
                            texture.shadow.data() + storage.offset +
                                z * storage.slice_pitch +
                                static_cast<usize>(row) * storage.row_pitch,
                            initial_levels[mip].data() +
                                (static_cast<usize>(z) * row_count + row) *
                                    row_bytes,
                            row_bytes);
                    }
                }
            }
            if (texture.shared_memory) texture.shared_memory->note_written();
        }
        if (texture_out == 0) {
            (void)release_object(address);
            m.set_return(1u);
            return;
        }
        if (m.write_ptr(texture_out, address).is_err()) {
            (void)release_object(address);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        m.set_return(d3d11::S_OK);
        return;
    }
    }
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_sharing.cpp
/// @brief D3D11 / DXGI shared resources: the handles a shared resource
///        hands out, the openings made from them on any device of any
///        process, and IDXGIKeyedMutex.
///
/// Every SHARED / KEYEDMUTEX resource -- Texture1D/2D/3D of any shape,
/// buffer, STAGING resource -- has storage the backend opens by a global id
/// (GraphicsBackendDevice::open_shared_resource): a single-image colour
/// texture's IOSurface, or a canonical store every process mirrors
/// (graphics/metal/metal_shared_resource.hpp), exported on first use.
/// Stored with it is the creator's description (shared_resource_metadata),
/// so an opening anywhere is the same resource: Windows hands back the
/// creator's description, misc flags included (measured,
/// tools/oracle_probe/d3d11_share_probe and d3d11_share_ers_probe "od=").
///
/// Handles (measured, same probe):
///  * GetSharedHandle ("KMT") values are global -- they open in another
///    process as they are -- and are not kernel handles (GetHandleInformation
///    fails). Here: kKmtTag | the storage id, outside every kernel-handle
///    range.
///  * CreateSharedHandle values are NT handles in the process handle table
///    (winapi/kernel32/shared_resource_handle.hpp), openable only by
///    OpenSharedResource1 and only in the process that holds them.
///  * OpenSharedResource refuses an NT handle and OpenSharedResource1 a KMT
///    value (E_INVALIDARG both ways).

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/kernel32/shared_resource_handle.hpp"
#include "winapi/core/marshaler.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "core/memory/guest_memory.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <unistd.h>

namespace vitreo::winapi {
namespace {

// winerror.h / winnt.h values (measured on Windows where listed).
constexpr u32 S_FALSE = 1u;
constexpr u32 E_ACCESSDENIED = 0x80070005u;
constexpr u32 DXGI_ERROR_INVALID_CALL_HR = 0x887A0001u;
constexpr u32 DXGI_ERROR_NAME_ALREADY_EXISTS = 0x887A002Cu;
/// MAKE_D3DHRESULT(2154): OpenSharedResourceByName without read access.
constexpr u32 D3DERR_NOTAVAILABLE = 0x8876086Au;
constexpr u32 GENERIC_ALL = 0x10000000u;
constexpr u32 WAIT_ABANDONED = 0x00000080u;

/// KMT handles: bit 30 set, bit 31 clear -- above every HandleTable value
/// (small multiples of 4), below the pseudo handles and the synthetic
/// 0xCC / 0xCE / 0xFACE ranges.
constexpr u32 kKmtTag = 0x40000000u;
constexpr u32 kKmtMask = 0xC0000000u;

[[nodiscard]] u64 kmt_handle(u32 surface) {
    return surface != 0 && (surface & kKmtMask) == 0 ? (kKmtTag | surface)
                                                      : 0u;
}

[[nodiscard]] u32 kmt_surface(u64 handle) {
    if (handle > 0xFFFFFFFFull) return 0;
    const u32 value = static_cast<u32>(handle);
    return (value & kKmtMask) == kKmtTag ? (value & ~kKmtMask) : 0u;
}

/// Learn DXGI_SHARED_RESOURCE_READ is GENERIC_READ; GENERIC_ALL includes
/// it. Measured: an NT handle created with 0, WRITE or 0x2 does not open
/// (E_ACCESSDENIED); READ, GENERIC_ALL and GENERIC_READ|GENERIC_WRITE do.
[[nodiscard]] bool grants_read(u32 access) {
    return (access & (DXGI_SHARED_RESOURCE_READ | GENERIC_ALL)) != 0;
}

void unimplemented_once(const std::string& what) {
    static std::mutex mutex;
    static std::unordered_set<std::string> logged;
    std::lock_guard lock(mutex);
    if (logged.insert(what).second) {
        std::fprintf(stderr, "[vitreo] UNIMPLEMENTED %s\n", what.c_str());
    }
}

/// shared_resource_metadata layout.
struct MetadataRecord {
    static constexpr u32 kMagic = 0x52533344u;  // 'D3SR'
    static constexpr u32 kVersion = 2u;
    u32 magic{0};
    u32 version{0};
    u64 nonce{0};
    u32 dimension{0};
    u32 width{0};
    u32 height{0};
    u32 depth{0};
    u32 mip_levels{0};
    u32 array_size{0};
    u32 format{0};
    u32 sample_count{0};
    u32 sample_quality{0};
    u32 usage{0};
    u32 bind_flags{0};
    u32 cpu_access_flags{0};
    u32 misc_flags{0};
    u32 structure_byte_stride{0};
    u32 creator_pid{0};
    u32 reserved{0};
};

/// D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE.
constexpr u32 kMiscRestrictSharedResource = 0x2000u;

/// D3D11_RESOURCE_DIMENSION.
constexpr u32 kDimensionBuffer = 1u;
constexpr u32 kDimensionTexture1D = 2u;
constexpr u32 kDimensionTexture3D = 4u;

}  // namespace

std::vector<u8> D3D11Manager::shared_resource_metadata(
    const SharedResourceDesc& desc) {
    MetadataRecord record{};
    record.magic = MetadataRecord::kMagic;
    record.version = MetadataRecord::kVersion;
    record.nonce = desc.nonce;
    record.dimension = desc.dimension;
    record.creator_pid = desc.creator_pid != 0
                             ? desc.creator_pid
                             : static_cast<u32>(::getpid());
    if (desc.dimension == kDimensionBuffer) {
        record.width = desc.buffer.byte_width;
        record.usage = desc.buffer.usage;
        record.bind_flags = desc.buffer.bind_flags;
        record.cpu_access_flags = desc.buffer.cpu_access_flags;
        record.misc_flags = desc.buffer.misc_flags;
        record.structure_byte_stride = desc.buffer.structure_byte_stride;
    } else {
        const auto& t = desc.texture;
        record.width = t.width;
        record.height = t.height;
        record.depth = desc.depth;
        record.mip_levels = t.mip_levels;
        record.array_size = t.array_size;
        record.format = t.format;
        record.sample_count = t.sample_count;
        record.sample_quality = t.sample_quality;
        record.usage = t.usage;
        record.bind_flags = t.bind_flags;
        record.cpu_access_flags = t.cpu_access_flags;
        record.misc_flags = t.misc_flags;
    }
    std::vector<u8> bytes(sizeof(record));
    std::memcpy(bytes.data(), &record, sizeof(record));
    return bytes;
}

bool D3D11Manager::parse_shared_resource_metadata(
    const std::vector<u8>& bytes, SharedResourceDesc& desc) {
    MetadataRecord record{};
    if (bytes.size() != sizeof(record)) return false;
    std::memcpy(&record, bytes.data(), sizeof(record));
    if (record.magic != MetadataRecord::kMagic ||
        record.version != MetadataRecord::kVersion ||
        record.dimension < kDimensionBuffer ||
        record.dimension > kDimensionTexture3D) {
        return false;
    }
    desc = {};
    desc.nonce = record.nonce;
    desc.dimension = record.dimension;
    desc.creator_pid = record.creator_pid;
    if (record.dimension == kDimensionBuffer) {
        desc.buffer.byte_width = record.width;
        desc.buffer.usage = record.usage;
        desc.buffer.bind_flags = record.bind_flags;
        desc.buffer.cpu_access_flags = record.cpu_access_flags;
        desc.buffer.misc_flags = record.misc_flags;
        desc.buffer.structure_byte_stride = record.structure_byte_stride;
        return true;
    }
    auto& t = desc.texture;
    t.width = record.width;
    t.height = record.height;
    desc.depth = record.depth;
    t.mip_levels = record.mip_levels;
    t.array_size = record.array_size;
    t.format = record.format;
    t.sample_count = record.sample_count;
    t.sample_quality = record.sample_quality;
    t.usage = record.usage;
    t.bind_flags = record.bind_flags;
    t.cpu_access_flags = record.cpu_access_flags;
    t.misc_flags = record.misc_flags;
    return true;
}

std::vector<u8> D3D11Manager::shared_texture_metadata(
    const Texture2DDesc& desc, u64 nonce, u32 dimension) {
    SharedResourceDesc shared{};
    shared.dimension = dimension;
    shared.texture = desc;
    shared.nonce = nonce;
    return shared_resource_metadata(shared);
}

bool D3D11Manager::parse_shared_texture_metadata(const std::vector<u8>& bytes,
                                                 Texture2DDesc& desc,
                                                 u64& nonce) {
    SharedResourceDesc shared{};
    if (!parse_shared_resource_metadata(bytes, shared) ||
        shared.dimension == kDimensionBuffer) {
        return false;
    }
    desc = shared.texture;
    nonce = shared.nonce;
    return true;
}

u64 D3D11Manager::new_shared_nonce() {
    u64 nonce = 0;
    while (nonce == 0) arc4random_buf(&nonce, sizeof(nonce));
    return nonce;
}

u32 D3D11Manager::shared_storage_of_handle(u64 handle) const {
    if (const u32 surface = kmt_surface(handle); surface != 0) return surface;
    if (m_handle_table == nullptr || handle > 0xFFFFFFFFull) return 0;
    const auto object =
        shared_resource_object(*m_handle_table, static_cast<Handle>(handle));
    return object != nullptr ? object->surface_id : 0u;
}

u64 D3D11Manager::export_shared_texture(Texture2D& texture) {
    // The storage another device or process opens: a shared STAGING
    // resource's memory, or the backend texture's shared storage (a mirrored
    // texture allocates its canonical store and publishes its contents here,
    // on first use -- a resource nobody takes a handle to never pays for it).
    if (texture.shared_memory) return texture.shared_memory->shared_handle();
    if (texture.volume) {
        return texture.backend_3d ? texture.backend_3d->export_shared() : 0u;
    }
    return texture.backend ? texture.backend->export_shared() : 0u;
}

namespace {

/// IDXGIResource::GetSharedHandle for a resource with @p misc_flags whose
/// storage @p export_storage names (0: none). Learn GetSharedHandle: the
/// handle of a resource created SHARED or SHARED_KEYEDMUTEX (or opened from
/// one). Measured: an NT-handle resource is E_INVALIDARG with the
/// out-parameter left alone, an unshared one S_OK with a NULL handle, and
/// asking twice gives the same value.
template <typename Export>
u32 shared_handle_result(Marshaler& m, u32 misc, Export export_storage,
                         const char* what) {
    const GuestAddr handle_out = m.arg_ptr(1);
    if (handle_out == 0) return DXGI_ERROR_INVALID_CALL_HR;
    if ((misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0) {
        return d3d11::E_INVALIDARG;
    }
    const bool shared = (misc & (D3D11_RESOURCE_MISC_SHARED |
                                 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0;
    if (!shared) {
        return m.write_ptr(handle_out, 0).is_err() ? DXGI_ERROR_INVALID_CALL_HR
                                                   : d3d11::S_OK;
    }
    const u64 storage = export_storage();
    const u64 handle =
        storage <= 0xFFFFFFFFull ? kmt_handle(static_cast<u32>(storage)) : 0u;
    (void)m.write_ptr(handle_out, handle);
    if (handle == 0) {
        // Created (the runtime does not validate a non-NT shared resource),
        // but its storage could not be exported (no memory for the store, or
        // a format the backend cannot copy).
        unimplemented_once(std::string("dxgi!IDXGIResource::GetSharedHandle: ") +
                           what + " could not export shared storage");
        return DXGI_ERROR_INVALID_CALL_HR;
    }
    return d3d11::S_OK;
}

}  // namespace

u32 D3D11Manager::get_shared_handle(Marshaler& m, Texture2D& texture) {
    const u32 misc = texture.volume ? texture.volume_desc.misc_flags
                                    : texture.desc.misc_flags;
    const u32 hr = shared_handle_result(
        m, misc, [&] { return export_shared_texture(texture); },
        texture.volume ? "Texture3D" : texture.linear ? "Texture1D"
                                                      : "Texture2D");
    d3d11_diag("IDXGIResource::GetSharedHandle texture=0x%llx -> 0x%x",
               static_cast<unsigned long long>(texture.interface_address), hr);
    return hr;
}

u32 D3D11Manager::get_shared_handle(Marshaler& m, Buffer& buffer) {
    // A shared buffer's storage IS its shared memory, allocated at creation.
    return shared_handle_result(
        m, buffer.desc.misc_flags,
        [&] { return buffer.backend ? buffer.backend->shared_handle() : 0u; },
        "buffer");
}

std::shared_ptr<graphics::GpuCompletion>
D3D11Manager::publish_shared_writes() {
    return m_backend.publish_shared_writes();
}

bool D3D11Manager::copy_shared_staging(
    Texture2D& staging, u32 subresource, bool to_staging,
    const graphics::BackendTextureLocation& gpu, u32 x, u32 y, u32 z,
    u32 width, u32 height, u32 depth) {
    // A shared STAGING resource is memory every opening maps: a GPU copy
    // into or out of it is a GPU blit against that memory, on this device's
    // timeline, published with the device's next synchronization point.
    // (x, y, z) is the staging box origin in texels; the packed depth
    // layouts (a D24S8 staging texel is one word, the host keeps two planes)
    // would need a CPU repack and are refused.
    if (!staging.shared_memory || subresource >= staging.subresources.size()) {
        return false;
    }
    const u32 format = staging.desc.format;
    if (is_packed_depth_stencil(format)) {
        unimplemented_once(
            "d3d11!CopyResource: GPU copy into or out of a shared STAGING "
            "resource of a packed depth-stencil format");
        return false;
    }
    const auto storage_format = d3d11::texture_storage_format(format);
    const auto info = graphics::format_table(storage_format);
    const u32 block = graphics::compressed_block_bytes(storage_format);
    const u32 elem = info.is_compressed ? block : info.bytes_per_pixel;
    if (elem == 0) return false;
    const auto& storage = staging.subresources[subresource];
    // x is in the staging resource's pixels: a block of a compressed format,
    // a host texel per pixel pair of a packed 4:2:2 one (width and the GPU
    // location are in host texels already).
    const u32 ex = info.is_compressed ? x / 4u
                                      : x / dxgi_pixels_per_texel(format);
    const u32 ey = info.is_compressed ? y / 4u : y;
    const usize image = storage.slice_pitch != 0 ? storage.slice_pitch
                                                 : storage.size;
    const usize offset = storage.offset + static_cast<usize>(z) * image +
                         static_cast<usize>(ey) * storage.row_pitch +
                         static_cast<usize>(ex) * elem;
    const bool ok =
        to_staging
            ? m_backend.copy_texture_to_shared_memory(
                  gpu, width, height, depth, *staging.shared_memory, offset,
                  storage.row_pitch, image)
            : m_backend.copy_shared_memory_to_texture(
                  *staging.shared_memory, offset, storage.row_pitch, image,
                  gpu, width, height, depth);
    if (ok) {
        // The generation the copy joined (note_gpu_use): a poll with
        // DO_NOT_WAIT completes once exactly that work has run.
        const u64 generation = recorded_work_generation();
        staging.shared_memory_gen = generation != 0 ? generation : ~0ull;
    }
    return ok;
}

void D3D11Manager::acquire_shared_staging(Texture2D& texture) {
    if (!texture.shared_memory) return;
    // This device's own GPU copies into or out of it, then what other
    // devices and processes published.
    if (texture.shared_memory_gen != 0) {
        const u64 generation = texture.shared_memory_gen;
        texture.shared_memory_gen = 0;
        if (generation == ~0ull) {
            (void)m_backend.wait_idle();
        } else {
            (void)m_backend.wait_generation(generation);
        }
    }
    (void)texture.shared_memory->acquire();
}

u32 D3D11Manager::create_shared_handle(Marshaler& m, Texture2D& texture) {
    // IDXGIResource1::CreateSharedHandle(pAttributes, dwAccess, lpName,
    // pHandle). Learn: only for a resource created SHARED_NTHANDLE (with
    // SHARED or SHARED_KEYEDMUTEX); the handle is an NT handle. Measured:
    // pHandle NULL is DXGI_ERROR_INVALID_CALL; a resource without NTHANDLE
    // is E_INVALIDARG with the handle NULLed; every dwAccess value creates
    // (access is checked when the handle is opened); a second call makes
    // another handle; bInheritHandle sets HANDLE_FLAG_INHERIT; a live name
    // -- even this resource's own, or an event's -- is
    // DXGI_ERROR_NAME_ALREADY_EXISTS.
    const GuestAddr attributes = m.arg_ptr(1);
    const u32 access = static_cast<u32>(m.arg(2));
    const GuestAddr name = m.arg_ptr(3);
    const GuestAddr handle_out = m.arg_ptr(4);
    if (handle_out == 0) return DXGI_ERROR_INVALID_CALL_HR;
    (void)m.write_ptr(handle_out, 0);
    const u32 misc = texture.volume ? texture.volume_desc.misc_flags
                                    : texture.desc.misc_flags;
    const bool nt_shareable =
        (misc & D3D11_RESOURCE_MISC_SHARED_NTHANDLE) != 0 &&
        (misc & (D3D11_RESOURCE_MISC_SHARED |
                 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0;
    if (!nt_shareable) return d3d11::E_INVALIDARG;
    const u64 exported = export_shared_texture(texture);
    const u32 surface =
        exported <= 0xFFFFFFFFull ? static_cast<u32>(exported) : 0u;
    if (surface == 0 || m_handle_table == nullptr) {
        // Creation validated the shape and format (d3d11_format_caps.cpp),
        // so only a missing handle table reaches this.
        unimplemented_once(
            "dxgi!IDXGIResource1::CreateSharedHandle: no shared storage or "
            "no process handle table");
        return d3d11::E_INVALIDARG;
    }
    bool inherit = false;
    if (attributes != 0) {
        // SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor;
        // BOOL bInheritHandle; } -- bInheritHandle at 8 (x86) / 16 (x64).
        // The descriptor governs who may open the handle; every opener here
        // runs as the same user.
        const auto value = m_memory.read_u32(attributes + (m_is_64bit ? 16 : 8));
        if (value.is_err()) return d3d11::E_INVALIDARG;
        inherit = value.value() != 0;
    }
    auto object = std::make_shared<SharedResourceObject>();
    object->surface_id = surface;
    object->nonce = texture.shared_nonce;
    object->keepalive = m_backend.retain_shared_texture(surface);
    if (name != 0) {
        const auto text = m.read_wide_string(name, 260);
        if (text.is_err() || text.value().empty()) return d3d11::E_INVALIDARG;
        std::string key;
        switch (register_shared_resource_name(text.value(), surface,
                                              texture.shared_nonce,
                                              object->name, key)) {
        case SharedResourceNameResult::Registered:
            object->name_key = std::move(key);
            break;
        case SharedResourceNameResult::AlreadyExists:
            return DXGI_ERROR_NAME_ALREADY_EXISTS;
        case SharedResourceNameResult::Failed:
            return d3d11::E_OUTOFMEMORY;
        }
    }
    const Handle handle = allocate_shared_resource_handle(
        *m_handle_table, std::move(object), access, inherit);
    if (handle == NULL_HANDLE) return d3d11::E_OUTOFMEMORY;
    if (m.write_ptr(handle_out, handle).is_err()) {
        (void)m_handle_table->close(handle);
        return DXGI_ERROR_INVALID_CALL_HR;
    }
    d3d11_diag(
        "IDXGIResource1::CreateSharedHandle texture=0x%llx access=0x%x "
        "-> 0x%x (storage %u)",
        static_cast<unsigned long long>(texture.interface_address), access,
        handle, surface);
    return d3d11::S_OK;
}

u32 D3D11Manager::open_shared_resource(Marshaler& m, GuestAddr self,
                                       SharedHandleKind kind) {
    // OpenSharedResource(hResource, ReturnedInterface, ppResource),
    // OpenSharedResource1(hResource, returnedInterface, ppResource),
    // OpenSharedResourceByName(lpName, dwDesiredAccess, returnedInterface,
    // ppResource). Learn: each call creates a new resource object (measured
    // also on the creating device). Measured: an unknown handle or name,
    // NULL, and the other kind of handle are E_INVALIDARG; ppResource NULL
    // validates and answers S_FALSE; ByName without read access is
    // D3DERR_NOTAVAILABLE and an NT handle without it E_ACCESSDENIED.
    const bool by_name = kind == SharedHandleKind::Name;
    const GuestAddr iid = m.arg_ptr(by_name ? 3 : 2);
    const GuestAddr out = m.arg_ptr(by_name ? 4 : 3);
    if (out != 0) (void)m.write_ptr(out, 0);
    u32 surface = 0;
    u64 nonce = 0;
    switch (kind) {
    case SharedHandleKind::Kmt:
        surface = kmt_surface(m.arg(1));
        break;
    case SharedHandleKind::Nt: {
        const u64 value = m.arg(1);
        u32 access = 0;
        const auto object =
            m_handle_table != nullptr && value != 0 && value <= 0xFFFFFFFFull
                ? shared_resource_object(*m_handle_table,
                                         static_cast<Handle>(value), &access)
                : nullptr;
        if (object == nullptr) return d3d11::E_INVALIDARG;
        if (!grants_read(access)) return E_ACCESSDENIED;
        surface = object->surface_id;
        nonce = object->nonce;
        break;
    }
    case SharedHandleKind::Name: {
        const GuestAddr name = m.arg_ptr(1);
        if (name == 0) return d3d11::E_INVALIDARG;
        const auto text = m.read_wide_string(name, 260);
        if (text.is_err()) return d3d11::E_INVALIDARG;
        const auto target = lookup_shared_resource_name(text.value());
        if (!target) return d3d11::E_INVALIDARG;
        if (!grants_read(static_cast<u32>(m.arg(2)))) {
            return D3DERR_NOTAVAILABLE;
        }
        surface = target->surface_id;
        nonce = target->nonce;
        break;
    }
    }
    if (surface == 0) return d3d11::E_INVALIDARG;
    return open_shared_storage(self, surface, nonce, iid, out);
}

u32 D3D11Manager::open_shared_storage(GuestAddr self, u32 surface, u64 nonce,
                                      GuestAddr iid_address,
                                      GuestAddr resource_out) {
    GuidBytes iid{};
    if (!read_guid(iid_address, iid)) return d3d11::E_INVALIDARG;
    auto opening = m_backend.open_shared_resource(surface);
    using Kind = graphics::GraphicsBackendDevice::SharedOpening::Kind;
    if (opening.kind == Kind::None) return d3d11::E_INVALIDARG;
    SharedResourceDesc desc{};
    if (!parse_shared_resource_metadata(opening.metadata, desc)) {
        // Storage another API shared: the image as an SRV | RTV texture.
        if (opening.kind != Kind::Texture2D || !opening.texture_2d) {
            return d3d11::E_INVALIDARG;
        }
        const auto& opened = opening.texture_2d->desc();
        desc = {};
        desc.dimension = 3;
        desc.texture.width = opened.width;
        desc.texture.height = opened.height;
        desc.texture.mip_levels = 1;
        desc.texture.array_size = 1;
        desc.texture.format =
            opened.pixel_format == graphics::PixelFormat::BGRA8Unorm ? 87u
                                                                     : 28u;
        desc.texture.sample_count = 1;
        desc.texture.usage = D3D11_USAGE_DEFAULT;
        desc.texture.bind_flags = D3D11_BIND_SHADER_RESOURCE |
                                  D3D11_BIND_RENDER_TARGET;
        desc.texture.misc_flags = D3D11_RESOURCE_MISC_SHARED;
    } else if (nonce != 0 && desc.nonce != nonce) {
        // The id was recycled: the resource the handle named is gone.
        return d3d11::E_INVALIDARG;
    }
    const bool buffer = desc.dimension == kDimensionBuffer;
    const bool volume = desc.dimension == kDimensionTexture3D;
    const u32 usage = buffer ? desc.buffer.usage : desc.texture.usage;
    // Measured: an IMMUTABLE shared resource hands out a handle, but the
    // handle does not open (E_INVALIDARG).
    if (usage == D3D11_USAGE_IMMUTABLE) return d3d11::E_INVALIDARG;
    // Learn D3D11_RESOURCE_MISC_RESTRICT_SHARED_RESOURCE: "the operating
    // system restricts access to the shared surface ... The process that
    // creates the shared resource can always open the shared resource."
    // Measured: another process's open is E_INVALIDARG; the _DRIVER form is
    // the driver's policy, and WARP restricts nothing (d3d11_share_ers_probe
    // "RESTRICT_*").
    const u32 misc = buffer ? desc.buffer.misc_flags : desc.texture.misc_flags;
    if ((misc & kMiscRestrictSharedResource) != 0 && desc.creator_pid != 0 &&
        desc.creator_pid != static_cast<u32>(::getpid())) {
        return d3d11::E_INVALIDARG;
    }
    // The resource kind must be what the storage holds.
    const bool staging = usage == D3D11_USAGE_STAGING;
    const bool kind_ok =
        buffer ? opening.kind == Kind::Buffer
        : staging ? opening.kind == Kind::Memory
        : volume ? opening.kind == Kind::Texture3D
                 : opening.kind == Kind::Texture2D;
    if (!kind_ok) return d3d11::E_INVALIDARG;
    // The answer to ReturnedInterface is known from the description; with
    // ppResource NULL the call only validates (S_FALSE).
    if (resource_out == 0) return S_FALSE;
    const GuestAddr owner = m_interfaces.find(self)->second.owner;
    GuestAddr object = 0;
    GuestAddr result = 0;
    if (buffer) {
        object = create_buffer(owner, desc.buffer, nullptr,
                               std::move(opening.buffer), desc.nonce);
        if (object == 0) return d3d11::E_OUTOFMEMORY;
        auto created = m_buffers.find(object);
        created->second.shared_opening = true;
        result = buffer_interface_for(object, iid);
        if (result == 0) {
            (void)release_object(object);
            return d3d11::E_NOINTERFACE;
        }
    } else {
        object = volume
            ? create_texture_3d_object(
                  owner,
                  Texture3DDesc{desc.texture.width, desc.texture.height,
                                desc.depth, desc.texture.mip_levels,
                                desc.texture.format, desc.texture.usage,
                                desc.texture.bind_flags,
                                desc.texture.cpu_access_flags,
                                desc.texture.misc_flags},
                  std::move(opening.texture_3d), opening.memory, desc.nonce,
                  true)
            : create_texture_2d(owner, desc.texture, nullptr, 0, 0,
                                std::move(opening.texture_2d), desc.dimension,
                                opening.memory, desc.nonce);
        if (object == 0) return d3d11::E_OUTOFMEMORY;
        auto created = m_textures.find(object);
        created->second.shared_opening = true;
        result = texture_interface_for(object, iid);
        if (result == 0) {
            (void)release_object(object);
            return d3d11::E_NOINTERFACE;
        }
    }
    if (!write_pointer(resource_out, result)) {
        if (buffer) {
            (void)release_object(object);
        } else {
            (void)release_object(object);
        }
        return d3d11::E_INVALIDARG;
    }
    d3d11_diag("OpenSharedResource storage=%u -> 0x%llx dimension=%u "
               "misc=0x%x",
               surface, static_cast<unsigned long long>(object),
               desc.dimension,
               buffer ? desc.buffer.misc_flags : desc.texture.misc_flags);
    return d3d11::S_OK;
}

void D3D11Manager::attach_keyed_mutex(
    std::shared_ptr<SharedKeyedMutex>& state, u32& opening, u64 storage_id,
    u64 nonce) {
    opening = ++m_next_keyed_mutex_opening;
    if (storage_id != 0 && storage_id <= 0xFFFFFFFFull) {
        const u64 key = (storage_id << 32) ^ nonce;
        auto found = m_keyed_mutex_states.find(key);
        if (found != m_keyed_mutex_states.end()) state = found->second.lock();
        if (!state) {
            // Openings in other processes attach the same (id, nonce) state.
            state = SharedKeyedMutex::attach(static_cast<u32>(storage_id),
                                             nonce);
            if (state) {
                std::erase_if(m_keyed_mutex_states, [](const auto& entry) {
                    return entry.second.expired();
                });
                m_keyed_mutex_states[key] = state;
            }
        }
    }
    if (!state) {
        // Nothing can open this resource elsewhere (no shared storage).
        state = SharedKeyedMutex::create_local();
    }
}

void D3D11Manager::attach_keyed_mutex(Texture2D& texture) {
    // A KEYEDMUTEX resource exists to be shared: its storage is exported at
    // creation, so the mutex state is the one every opening attaches.
    attach_keyed_mutex(texture.keyed_mutex, texture.keyed_mutex_opening,
                       export_shared_texture(texture), texture.shared_nonce);
}

u32 D3D11Manager::keyed_mutex_acquire(
    const std::shared_ptr<SharedKeyedMutex>& state, u32 opening, u64 key,
    u32 timeout_ms) {
    if (!state) return d3d11::E_FAIL;
    auto result = state->acquire(opening, key, 0);
    if (result == SharedKeyedMutex::Result::Timeout && timeout_ms != 0) {
        // The ReleaseSync that ends this wait may come from another guest
        // thread through the same device lock: give it up while waiting.
        DxgiManager::ApiUnlock unlock(m_dxgi);
        result = state->acquire(opening, key, timeout_ms);
    }
    switch (result) {
    case SharedKeyedMutex::Result::Acquired:
        // Learn D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX: a device "must call
        // IDXGIKeyedMutex::AcquireSync before they issue any rendering
        // commands to the surface" -- what the previous owner released is
        // what this device's next commands see.
        m_backend.acquire_shared_writes();
        return d3d11::S_OK;
    case SharedKeyedMutex::Result::AlreadyOwner:
        return DXGI_ERROR_INVALID_CALL_HR;
    case SharedKeyedMutex::Result::Timeout: return WAIT_TIMEOUT;
    case SharedKeyedMutex::Result::Abandoned: return WAIT_ABANDONED;
    }
    return d3d11::E_FAIL;
}

u32 D3D11Manager::keyed_mutex_release(
    const std::shared_ptr<SharedKeyedMutex>& state, u32 opening, u64 key) {
    if (!state) return d3d11::E_FAIL;
    // What the owner rendered must be visible to the next acquirer. Every
    // device of this process records into the one backend queue, so an
    // opening here is ordered after this work already. An opening in another
    // process is not (command-queue order does not cross processes): push
    // this process's writes into their shared storage, submit, and let other
    // processes acquire only once that work has completed
    // (SharedKeyedMutex::release waits for it off this thread).
    std::shared_ptr<graphics::GpuCompletion> pending;
    if (state->cross_process()) {
        pending = m_backend.publish_shared_writes();
        if (!pending) pending = m_backend.flush_completion();
        // A backend without completion tracking: complete it here.
        if (!pending) (void)m_backend.wait_idle();
    }
    return state->release(opening, key, std::move(pending))
        ? d3d11::S_OK
        : DXGI_ERROR_INVALID_CALL_HR;
}

}  // namespace vitreo::winapi

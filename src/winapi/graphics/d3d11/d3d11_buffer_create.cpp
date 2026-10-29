// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_buffer_create.cpp
/// @brief Buffer creation (ID3D11Device::CreateBuffer) and its backend
///        description. Split out of d3d11.cpp as verbatim code motion.
#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/core/guest_com.hpp"
#include "winapi/graphics/d3d11/d3d11_caps_log.hpp"
#include "winapi/graphics/d3d11/d3d11_in_chain.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/graphics/d3d11/d3d11_state_census.hpp"
#include "winapi/graphics/d3d11/d3d11_stdcall.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"
#include "winapi/graphics/common/shared_keyed_mutex.hpp"
#include "winapi/common/virtual_display_modes.hpp"

#include "graphics/contracts/backend_device.hpp"
#include "graphics/common/diagnostic_path.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/graphics/dxgi/dxgi.hpp"
#include "winapi/core/marshaler.hpp"
#include <vitreo/diagnostics.hpp>
#include <vitreo/virtual_tsc.hpp>

#include <algorithm>
#include <cmath>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <optional>
#include <unordered_map>
#include <thread>
#include <utility>
#include <vector>

namespace vitreo::winapi {

graphics::BackendBufferDesc D3D11Manager::backend_buffer_desc(
    const BufferDesc& desc) {
    graphics::BackendBufferDesc backend_desc{};
    backend_desc.byte_width = desc.byte_width;
    backend_desc.vertex_buffer =
        (desc.bind_flags & D3D11_BIND_VERTEX_BUFFER) != 0;
    backend_desc.index_buffer =
        (desc.bind_flags & D3D11_BIND_INDEX_BUFFER) != 0;
    backend_desc.constant_buffer =
        (desc.bind_flags & D3D11_BIND_CONSTANT_BUFFER) != 0;
    backend_desc.shader_resource =
        (desc.bind_flags & D3D11_BIND_SHADER_RESOURCE) != 0;
    backend_desc.unordered_access =
        (desc.bind_flags & D3D11_BIND_UNORDERED_ACCESS) != 0;
    backend_desc.stream_output =
        (desc.bind_flags & D3D11_BIND_STREAM_OUTPUT) != 0;
    backend_desc.cpu_writable = desc.usage == D3D11_USAGE_DYNAMIC ||
                                desc.usage == D3D11_USAGE_STAGING;
    return backend_desc;
}

namespace {

// VITREO_D3D11_CREATE_BUFFER_STATS=1: DIAGNOSTIC. CreateBuffer counts and
// host time by bind class, usage and size, with the backend allocation's
// share, every 5 s on stderr. Prices which buffers a pool would have to cover.
struct CreateBufferStatScope {
    static bool enabled() {
        static const bool on =
            std::getenv("VITREO_D3D11_CREATE_BUFFER_STATS") != nullptr;
        return on;
    }
    struct Row { u64 count{0}, ns{0}, backend_ns{0}, bytes{0}; };
    static std::mutex& mutex() { static auto* m = new std::mutex(); return *m; }
    static std::map<std::string, Row>& rows() {
        static auto* r = new std::map<std::string, Row>(); return *r;
    }
    CreateBufferStatScope(u32 bind, u32 use, u32 width)
        : bind_flags(bind), usage(use), byte_width(width), on(enabled()) {
        if (on) start = std::chrono::steady_clock::now();
    }
    ~CreateBufferStatScope() {
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - start)
                .count());
        const char* bind =
            (bind_flags & D3D11_BIND_CONSTANT_BUFFER) ? "cb"
            : (bind_flags & 0x1u) ? "vb" : (bind_flags & 0x2u) ? "ib"
            : (bind_flags & 0x80u) ? "uav" : (bind_flags & 0x8u) ? "srv"
            : "other";
        static const char* const usages[] = {"default", "immutable",
                                             "dynamic", "staging"};
        const char* use = usage < 4 ? usages[usage] : "?";
        const char* size = byte_width <= 4096 ? "<=4K"
                         : byte_width <= 65536 ? "<=64K"
                         : byte_width <= (1u << 20) ? "<=1M" : ">1M";
        std::string key = std::string(bind) + "/" + use + "/" + size;
        std::lock_guard<std::mutex> lock(mutex());
        Row& r = rows()[key];
        ++r.count; r.ns += ns; r.backend_ns += backend_ns; r.bytes += byte_width;
        static auto last = now;
        if (now - last >= std::chrono::seconds(5)) {
            last = now;
            std::fprintf(stderr, "[d3d11-createbuffer] cumulative:\n");
            for (const auto& [k, v] : rows()) {
                std::fprintf(stderr,
                    "[d3d11-createbuffer]   %-24s n=%-8llu avg=%7.2fus "
                    "backend=%7.2fus avg_bytes=%llu\n", k.c_str(),
                    static_cast<unsigned long long>(v.count),
                    v.ns / 1000.0 / static_cast<double>(v.count),
                    v.backend_ns / 1000.0 / static_cast<double>(v.count),
                    static_cast<unsigned long long>(v.bytes / v.count));
            }
        }
    }
    u32 bind_flags, usage, byte_width;
    bool on;
    std::chrono::steady_clock::time_point start{};
    u64 backend_ns{0};
};

}  // namespace

GuestAddr D3D11Manager::create_buffer(
    GuestAddr device_owner, const BufferDesc& desc,
    const void* initial_data,
    std::unique_ptr<graphics::BackendBuffer> opened_backend,
    u64 opened_nonce) {
    CreateBufferStatScope stat_scope(static_cast<u32>(desc.bind_flags),
                                     static_cast<u32>(desc.usage),
                                     static_cast<u32>(desc.byte_width));
    auto device = m_devices.find(device_owner);
    if (device == m_devices.end() || !ensure_storage()) {
        std::fprintf(stderr,
            "[d3d11] CreateBuffer failed: device=0x%llx bytes=%u "
            "next_object=0x%llx chunk_end=0x%llx live_buffers=%zu\n",
            static_cast<unsigned long long>(device_owner), desc.byte_width,
            static_cast<unsigned long long>(m_next_object),
            static_cast<unsigned long long>(m_region_end), m_buffers.size());
        return 0;
    }

    // Every usage gets GPU storage, STAGING included: a staging buffer is
    // the CPU end of a GPU copy (CopyResource / CopySubresourceRegion in
    // either direction), and without storage of its own that copy has
    // nowhere to land -- both readback and upload were dropped. Its shadow
    // stays the CPU view; Map(READ) pulls GPU-written contents back
    // (backend_buffer_desc makes it CPU-writable like a DYNAMIC buffer).
    // A SHARED / KEYEDMUTEX buffer lives in memory every opening -- on any
    // device of any process -- maps (measured: a shared buffer opens as an
    // ID3D11Buffer elsewhere and round-trips both ways,
    // d3d11_share_ers_probe).
    const bool shared =
        (desc.misc_flags & (D3D11_RESOURCE_MISC_SHARED |
                            D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0;
    const u64 shared_nonce =
        opened_backend ? opened_nonce : shared ? new_shared_nonce() : 0u;
    std::unique_ptr<graphics::BackendBuffer> backend_buffer =
        std::move(opened_backend);
    // Tiled resources (d3d11_tiled.cpp): a tile pool is memory the backend
    // allocates as its tiles are mapped, with no buffer of its own; a tiled
    // buffer is address space whose tiles map into a pool. Neither keeps a
    // CPU shadow (either may be gigabytes; neither is ever CPU-mapped).
    const bool tile_pool =
        (desc.misc_flags & D3D11_RESOURCE_MISC_TILE_POOL) != 0;
    const bool tiled = (desc.misc_flags & D3D11_RESOURCE_MISC_TILED) != 0;
    std::shared_ptr<TilePool> pool_state;
    std::unique_ptr<TiledState> tiled_state;
    if (tile_pool) {
        pool_state = std::make_shared<TilePool>();
        pool_state->backend = m_backend.create_tile_pool();
        if (!pool_state->backend) return 0;
        pool_state->tiles = desc.byte_width / graphics::BACKEND_TILE_BYTES;
        pool_state->backend->set_size(pool_state->tiles);
    } else if (tiled) {
        tiled_state = tiled_buffer_state(desc.byte_width);
        if (!tiled_state) return 0;
    }
    if (!backend_buffer && !tile_pool) {
        graphics::BackendBufferDesc backend_desc = backend_buffer_desc(desc);
        backend_desc.tiled = tiled;
        if (shared) {
            SharedResourceDesc metadata{};
            metadata.dimension = 1;  // D3D11_RESOURCE_DIMENSION_BUFFER
            metadata.buffer = desc;
            metadata.nonce = shared_nonce;
            backend_desc.shareable = true;
            backend_desc.shared_metadata = shared_resource_metadata(metadata);
        }
        const auto backend_start = stat_scope.on
            ? std::chrono::steady_clock::now()
            : std::chrono::steady_clock::time_point{};
        backend_buffer =
            m_backend.create_buffer(backend_desc, initial_data);
        if (stat_scope.on) {
            stat_scope.backend_ns = static_cast<u64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - backend_start).count());
        }
        if (shared && backend_buffer && backend_buffer->valid() &&
            backend_buffer->shared_handle() == 0) {
            d3d11_unimplemented_once(
                "CreateBuffer", "shared buffer without shared storage");
        }
        if (!backend_buffer || !backend_buffer->valid()) {
            std::fprintf(stderr,
                "[d3d11] CreateBuffer backend allocation failed: bytes=%u\n",
                desc.byte_width);
            return 0;
        }
    }

    const GuestAddr address = allocate_object(m_buffer_vtable);
    if (address == 0) return 0;
    // The DXGI facets sit where a texture's do: IDXGIResource at object +
    // 0x20, IDXGISurface at + 0x30.
    const GuestAddr dxgi_resource_interface = address + 0x20;
    const GuestAddr dxgi_surface_interface = address + 0x30;
    if (!write_pointer(dxgi_resource_interface, m_dxgi_resource_vtable) ||
        !write_pointer(dxgi_surface_interface, m_dxgi_surface_vtable)) {
        free_object(address);
        return 0;
    }
    const bool keyed =
        (desc.misc_flags & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0;
    // IDXGIKeyedMutex facet at object + 0x28, as a texture's.
    const GuestAddr keyed_mutex_interface = address + 0x28;
    if (keyed && !write_pointer(keyed_mutex_interface, m_keyed_mutex_vtable)) {
        free_object(address);
        return 0;
    }
    Buffer buffer{};
    buffer.interface_address = address;
    buffer.dxgi_resource_interface = dxgi_resource_interface;
    buffer.dxgi_surface_interface = dxgi_surface_interface;
    buffer.keyed_mutex_interface = keyed ? keyed_mutex_interface : 0;
    buffer.device_owner = device_owner;
    buffer.desc = desc;
    buffer.backend = std::move(backend_buffer);
    buffer.shared_nonce = shared_nonce;
    buffer.tile_pool = std::move(pool_state);
    buffer.tiled = std::move(tiled_state);
    if (!tile_pool && !tiled) buffer.shadow.resize(desc.byte_width);
    if (initial_data != nullptr && !buffer.shadow.empty()) {
        std::memcpy(buffer.shadow.data(), initial_data, desc.byte_width);
    }
    if (tiled) buffer.gpu_newer = true;  // no shadow to trust
    if (buffer.backend && buffer.backend->shared_handle() != 0) {
        // Other openings write the shared memory without touching this
        // opening's shadow: the backend's contents are the current ones.
        buffer.gpu_newer = true;
    }
    if (keyed) {
        attach_keyed_mutex(buffer.keyed_mutex, buffer.keyed_mutex_opening,
                           buffer.backend ? buffer.backend->shared_handle()
                                          : 0u,
                           buffer.shared_nonce);
    }
    m_buffers.emplace(address, std::move(buffer));
    (void)add_ref(device->second.device_interface);
    return address;
}

} // namespace vitreo::winapi

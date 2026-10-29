// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11.cpp
/// @brief The D3D11Manager core: construction and destruction, service wiring,
///        guest pointer and GUID marshalling, the buffer and texture
///        description readers and writers, and the unsupported-method fallback.
///        The rest of the manager lives in the d3d11_*.cpp files beside it.
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
#include <atomic>
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

usize D3D11Manager::GuidHash::operator()(const GuidBytes& guid) const noexcept {
    usize hash = 1469598103934665603ull;
    for (const u8 byte : guid) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

D3D11Manager::D3D11Manager(GuestMemory& memory, bool is_64bit,
                           graphics::GraphicsBackendDevice& backend,
                           DxgiManager& dxgi_manager)
    : m_memory(memory)
    , m_is_64bit(is_64bit)
    , m_backend(backend)
    , m_dxgi(dxgi_manager)
    , m_api_mutex(dxgi_manager.api_mutex()) {
    // IDXGIFactory2::GetSharedResourceAdapterLuid asks whether a handle names
    // a shared resource: a texture of this process published it, or the
    // backend can open it (another process's IOSurface).
    m_dxgi.set_shared_resource_query([this](u64 handle) {
        std::scoped_lock lock(m_api_mutex);
        // A KMT value or an NT handle of this process (measured: both
        // answer the adapter's LUID).
        const u32 surface = shared_storage_of_handle(handle);
        if (surface == 0) return false;
        for (const auto& [address, texture] : m_textures) {
            (void)address;
            if (texture.backend && texture.backend->shared_handle() == surface) {
                return true;
            }
        }
        return m_backend.retain_shared_texture(surface) != nullptr;
    });
}

D3D11Manager::~D3D11Manager() {
    m_shutting_down = true;
    if (m_diag_transfer_log != nullptr && !m_diag_transfer_to_stderr) {
        std::fclose(m_diag_transfer_log);
    }
    m_diag_transfer_log = nullptr;
    m_dxgi.set_shared_resource_query({});
    // The process is going away with its devices: a keyed mutex one of its
    // openings owns is abandoned for every other process (measured: a child
    // that exits owning it leaves WAIT_ABANDONED behind). Waiting for the
    // host process to be reaped would leave the other side timing out.
    for (auto& [address, texture] : m_textures) {
        (void)address;
        if (texture.keyed_mutex) {
            texture.keyed_mutex->opening_destroyed(texture.keyed_mutex_opening);
        }
    }
    for (auto& [address, buffer] : m_buffers) {
        (void)address;
        if (buffer.keyed_mutex) {
            buffer.keyed_mutex->opening_destroyed(buffer.keyed_mutex_opening);
        }
    }
    if (m_foreign_present_worker.joinable()) {
        m_foreign_present_worker.request_stop();
        m_foreign_present_cv.notify_all();
        m_foreign_present_worker = {};
    }
    for (const auto& [address, device] : m_devices) {
        (void)address;
        if (device.adapter != 0) m_dxgi.release_adapter(device.adapter);
    }
}

void D3D11Manager::set_method_thunks(MethodThunks thunks) {
    m_method_thunks = thunks;
}

void D3D11Manager::set_presentation_view_resolver(
    PresentationViewResolver resolver) {
    m_presentation_view_resolver = std::move(resolver);
}

int& D3D11Manager::api_lock_depth() noexcept {
    return m_dxgi.api_lock_depth();
}

namespace {
std::atomic<void (*)(void*)> g_pure_work_unlock_hook{nullptr};
std::atomic<void*> g_pure_work_unlock_hook_context{nullptr};
}  // namespace

void D3D11Manager::set_pure_work_unlock_hook_for_test(
    void (*hook)(void*), void* context) noexcept {
    g_pure_work_unlock_hook_context.store(context);
    g_pure_work_unlock_hook.store(hook);
}

D3D11Manager::PureWorkUnlock::PureWorkUnlock(D3D11Manager& manager,
                                             bool allowed)
    : m_manager(manager) {
    int& depth = manager.api_lock_depth();
    if (!allowed || depth != 1) return;
    device_lock_profile::suspend();
    depth = 0;
    manager.m_api_mutex.unlock();
    m_released = true;
    if (auto* hook = g_pure_work_unlock_hook.load()) {
        hook(g_pure_work_unlock_hook_context.load());
    }
}

D3D11Manager::PureWorkUnlock::~PureWorkUnlock() {
    if (!m_released) return;
    device_lock_profile::lock(m_manager.m_api_mutex);
    m_manager.api_lock_depth() = 1;
    device_lock_profile::resume();
}

void D3D11Manager::set_window_services(WindowServices services) {
    m_window_services = std::move(services);
}

void D3D11Manager::set_sync_services(SyncServices services) {
    m_sync_services = std::move(services);
}

void D3D11Manager::set_foreign_window_presenter(
    ForeignWindowResolver resolver, WindowPixelsPresenter pixel_presenter,
    WindowSurfacePresenter surface_presenter) {
    m_foreign_window_resolver = std::move(resolver);
    m_window_pixels_presenter = std::move(pixel_presenter);
    m_window_surface_presenter = std::move(surface_presenter);
}

void D3D11Manager::set_foreign_surface_view_resolver(
    PresentationViewResolver resolver) {
    m_foreign_surface_view_resolver = std::move(resolver);
}

bool D3D11Manager::write_pointer(GuestAddr address, GuestAddr value) {
    if (m_is_64bit) return m_memory.write_u64(address, value).is_ok();
    return m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
}

bool D3D11Manager::read_pointer(GuestAddr address, GuestAddr& value) const {
    if (m_is_64bit) {
        const auto result = m_memory.read_u64(address);
        if (result.is_err()) return false;
        value = result.value();
        return true;
    }
    const auto result = m_memory.read_u32(address);
    if (result.is_err()) return false;
    value = result.value();
    return true;
}

bool D3D11Manager::read_guid(GuestAddr address, GuidBytes& guid) const {
    return address != 0 &&
        m_memory.read_bytes(address, guid.data(), guid.size()).is_ok();
}

graphics::PixelFormat D3D11Manager::backend_format(
    u32 dxgi_format) const {
    return graphics::pixel_format_from_dxgi(dxgi_format);
}

graphics::PixelFormat D3D11Manager::backend_texture_format(
    u32 dxgi_format, u32 bind_flags) const {
    if ((bind_flags & D3D11_BIND_DEPTH_STENCIL) != 0) {
        switch (dxgi_format) {
        case 19: return graphics::PixelFormat::D32Float_S8Uint;
        case 39: return graphics::PixelFormat::D32Float;
        case 44: return graphics::PixelFormat::D24Unorm_S8Uint;
        case 53: return graphics::PixelFormat::D16Unorm;
        default: break;
        }
    }
    // Storage-only formats (R10G10B10_XR_BIAS_A2) and the refusal of the
    // ones with no texture form (R32G32B32) come from the one format table.
    return d3d11::texture_storage_format(dxgi_format);
}

bool D3D11Manager::read_buffer_desc(
    GuestAddr address, BufferDesc& desc) const {
    if (address == 0) return false;
    u32* fields[] = {
        &desc.byte_width, &desc.usage, &desc.bind_flags,
        &desc.cpu_access_flags, &desc.misc_flags,
        &desc.structure_byte_stride,
    };
    for (usize index = 0; index < std::size(fields); ++index) {
        const auto value = m_memory.read_u32(address + index * sizeof(u32));
        if (value.is_err()) return false;
        *fields[index] = value.value();
    }
    return true;
}

bool D3D11Manager::write_buffer_desc(
    GuestAddr address, const BufferDesc& desc) {
    if (address == 0) return false;
    const u32 fields[] = {
        desc.byte_width, desc.usage, desc.bind_flags,
        desc.cpu_access_flags, desc.misc_flags,
        desc.structure_byte_stride,
    };
    for (usize index = 0; index < std::size(fields); ++index) {
        if (m_memory.write_u32(address + index * sizeof(u32),
                               fields[index]).is_err()) {
            return false;
        }
    }
    return true;
}

bool D3D11Manager::read_texture_desc(
    GuestAddr address, Texture2DDesc& desc) const {
    if (address == 0) return false;
    u32* fields[] = {
        &desc.width, &desc.height, &desc.mip_levels, &desc.array_size,
        &desc.format, &desc.sample_count, &desc.sample_quality, &desc.usage,
        &desc.bind_flags, &desc.cpu_access_flags, &desc.misc_flags,
    };
    for (usize index = 0; index < std::size(fields); ++index) {
        const auto value = m_memory.read_u32(address + index * sizeof(u32));
        if (value.is_err()) return false;
        *fields[index] = value.value();
    }
    return true;
}

bool D3D11Manager::write_texture_desc(
    GuestAddr address, const Texture2DDesc& desc) {
    if (address == 0) return false;
    const u32 fields[] = {
        desc.width, desc.height, desc.mip_levels, desc.array_size,
        desc.format, desc.sample_count, desc.sample_quality, desc.usage,
        desc.bind_flags, desc.cpu_access_flags, desc.misc_flags,
    };
    for (usize index = 0; index < std::size(fields); ++index) {
        if (m_memory.write_u32(address + index * sizeof(u32),
                               fields[index]).is_err()) {
            return false;
        }
    }
    return true;
}

void D3D11Manager::unsupported(Marshaler& m, const char* interface_name,
                               usize slot) {
    const char* method = nullptr;
    if (std::strcmp(interface_name, "ID3D11Device") == 0 &&
        slot < DEVICE_METHOD_NAMES.size()) {
        method = DEVICE_METHOD_NAMES[slot];
    } else if (std::strcmp(interface_name, "IDXGIDevice") == 0 &&
               slot < DXGI_DEVICE_METHOD_NAMES.size()) {
        method = DXGI_DEVICE_METHOD_NAMES[slot];
    } else if (std::strcmp(interface_name, "ID3D11Buffer") == 0 &&
               slot < BUFFER_METHOD_NAMES.size()) {
        method = BUFFER_METHOD_NAMES[slot];
    } else if (std::strcmp(interface_name, "ID3D11Texture2D") == 0 &&
               slot < TEXTURE_2D_METHOD_NAMES.size()) {
        method = TEXTURE_2D_METHOD_NAMES[slot];
    } else if (std::strcmp(
                   interface_name, "ID3D11ShaderResourceView") == 0 &&
               slot < SHADER_RESOURCE_VIEW_METHOD_NAMES.size()) {
        method = SHADER_RESOURCE_VIEW_METHOD_NAMES[slot];
    } else if (std::strcmp(
                   interface_name, "ID3D11RenderTargetView") == 0 &&
               slot < RENDER_TARGET_VIEW_METHOD_NAMES.size()) {
        method = RENDER_TARGET_VIEW_METHOD_NAMES[slot];
    } else if (std::strcmp(interface_name, "IDXGISwapChain") == 0 &&
               slot < SWAP_CHAIN_METHOD_NAMES.size()) {
        method = SWAP_CHAIN_METHOD_NAMES[slot];
    }
    if (method != nullptr) {
        d3d11_diag("%s::%s -> E_NOTIMPL",
                   interface_name, method);
        VITREO_TRACE("[d3d11] %s::%s -> E_NOTIMPL\n",
                     interface_name, method);
    } else {
        d3d11_diag("%s vtable slot %zu -> unsupported",
                   interface_name, slot);
        VITREO_TRACE("[d3d11] %s vtable slot %zu -> unsupported\n",
                     interface_name, slot);
    }
    m.set_return(d3d11::E_NOTIMPL);
}

} // namespace vitreo::winapi

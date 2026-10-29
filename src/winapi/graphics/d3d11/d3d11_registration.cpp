// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_registration.cpp
/// @brief The d3d11.dll export table: the per-interface method export names,
///        the COM method entry (device lock, lock depth, state census) and
///        create_d3d11_thunks. Split out of d3d11.cpp as verbatim code motion.
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

std::string d3d11_device_method_export_name(usize slot) {
    return method_export_name("Device", slot);
}

std::string d3d11_context_method_export_name(usize slot) {
    return method_export_name("Context", slot);
}

std::string d3d11_dxgi_device_method_export_name(usize slot) {
    return method_export_name("DxgiDevice", slot);
}

std::string d3d11_buffer_method_export_name(usize slot) {
    return method_export_name("Buffer", slot);
}

std::string d3d11_texture_2d_method_export_name(usize slot) {
    return method_export_name("Texture2D", slot);
}

std::string d3d11_dxgi_resource_method_export_name(usize slot) {
    return method_export_name("DxgiResource", slot);
}

std::string d3d11_keyed_mutex_method_export_name(usize slot) {
    return method_export_name("KeyedMutex", slot);
}

std::string d3d11_shader_resource_view_method_export_name(usize slot) {
    return method_export_name("ShaderResourceView", slot);
}

std::string d3d11_unordered_access_view_method_export_name(usize slot) {
    return method_export_name("UnorderedAccessView", slot);
}

std::string d3d11_render_target_view_method_export_name(usize slot) {
    return method_export_name("RenderTargetView", slot);
}

std::string d3d11_depth_stencil_view_method_export_name(usize slot) {
    return method_export_name("DepthStencilView", slot);
}

std::string d3d11_query_method_export_name(usize slot) {
    return method_export_name("Query", slot);
}

std::string d3d11_fence_method_export_name(usize slot) {
    return method_export_name("Fence", slot);
}

std::string d3d11_command_list_method_export_name(usize slot) {
    return method_export_name("CommandList", slot);
}

std::string d3d11_swap_chain_method_export_name(usize slot) {
    return method_export_name("SwapChain", slot);
}

std::string d3d11_swap_chain_media_method_export_name(usize slot) {
    return method_export_name("SwapChainMedia", slot);
}

std::string d3d11_annotation_method_export_name(usize slot) {
    return method_export_name("Annotation", slot);
}

std::string d3d11_video_context_method_export_name(usize slot) {
    return method_export_name("VideoContext", slot);
}

std::string d3d11_multithread_method_export_name(usize slot) {
    return method_export_name("Multithread", slot);
}

std::string d3d11_class_linkage_method_export_name(usize slot) {
    return method_export_name("ClassLinkage", slot);
}

std::string d3d11_class_instance_method_export_name(usize slot) {
    return method_export_name("ClassInstance", slot);
}

namespace {

/// One D3D11 COM method as its export's direct entry (DirectHandler): the
/// device lock every guest entry takes (D3D11Manager::m_api_mutex; see
/// EveryGuestEntryHoldsTheDeviceLock), the tracked lock depth that a
/// blocking call or a guest COM callback releases the lock by, then the
/// method. It is the export's whole body: make_direct_export derives the
/// export's `handler` from it, so the full dispatch path and the direct
/// crossing (cli::run_direct_thunk) run this same function.
// ---- P0 state census (VITREO_D3D11_STATE_CENSUS=1, DIAGNOSTIC;
// d3d11_state_census.hpp). The interface each entry belongs to, and the
// census's copy of d3d11_com_method: the same lock, depth and method, timed
// and counted around them.
template <void (D3D11Manager::*Method)(Marshaler&, usize)>
constexpr d3d11_census::Iface census_iface() {
    using I = d3d11_census::Iface;
    using M = D3D11Manager;
    if constexpr (Method == &M::impl_ContextMethod) return I::Context;
    else if constexpr (Method == &M::impl_AnnotationMethod) return I::Annotation;
    else if constexpr (Method == &M::impl_VideoContextMethod) return I::VideoContext;
    else if constexpr (Method == &M::impl_DeviceMethod) return I::Device;
    else if constexpr (Method == &M::impl_MultithreadMethod) return I::Multithread;
    else if constexpr (Method == &M::impl_DxgiDeviceMethod) return I::DxgiDevice;
    else if constexpr (Method == &M::impl_DxgiResourceMethod) return I::DxgiResource;
    else if constexpr (Method == &M::impl_DxgiSurfaceMethod) return I::DxgiSurface;
    else if constexpr (Method == &M::impl_SwapChainMethod) return I::SwapChain;
    else if constexpr (Method == &M::impl_SwapChainMediaMethod) return I::SwapChainMedia;
    else if constexpr (Method == &M::impl_KeyedMutexMethod) return I::KeyedMutex;
    else if constexpr (Method == &M::impl_BufferMethod) return I::Buffer;
    else if constexpr (Method == &M::impl_Texture2DMethod) return I::Texture;
    else if constexpr (Method == &M::impl_ShaderResourceViewMethod) return I::ShaderResourceView;
    else if constexpr (Method == &M::impl_UnorderedAccessViewMethod) return I::UnorderedAccessView;
    else if constexpr (Method == &M::impl_RenderTargetViewMethod) return I::RenderTargetView;
    else if constexpr (Method == &M::impl_DepthStencilViewMethod) return I::DepthStencilView;
    else if constexpr (Method == &M::impl_QueryMethod) return I::Query;
    else if constexpr (Method == &M::impl_FenceMethod) return I::Fence;
    else if constexpr (Method == &M::impl_CommandListMethod) return I::CommandList;
    else if constexpr (Method == &M::impl_VertexShaderMethod ||
                       Method == &M::impl_PixelShaderMethod ||
                       Method == &M::impl_GeometryShaderMethod ||
                       Method == &M::impl_HullShaderMethod ||
                       Method == &M::impl_DomainShaderMethod ||
                       Method == &M::impl_ComputeShaderMethod) return I::Shader;
    else if constexpr (Method == &M::impl_InputLayoutMethod) return I::InputLayout;
    else if constexpr (Method == &M::impl_BlendStateMethod ||
                       Method == &M::impl_DepthStencilStateMethod ||
                       Method == &M::impl_RasterizerStateMethod ||
                       Method == &M::impl_SamplerStateMethod) return I::StateObject;
    else if constexpr (Method == &M::impl_ClassLinkageMethod ||
                       Method == &M::impl_ClassInstanceMethod) return I::ClassLinkage;
    else if constexpr (Method == &M::impl_DeviceContextStateMethod) return I::DeviceContextState;
    else return I::Other;
}

template <void (D3D11Manager::*Method)(Marshaler&, usize)>
[[gnu::noinline, gnu::cold]] void d3d11_com_method_census(
    D3D11Manager& manager, Marshaler& m, u32 slot) {
    d3d11_census::Crossing crossing{};
    crossing.iface = census_iface<Method>();
    crossing.slot = slot;
    crossing.t_enter = d3d11_census::now_ns();
    std::scoped_lock lock(manager.api_mutex());
    crossing.t_locked = d3d11_census::now_ns();
    int& depth = manager.api_lock_depth();
    crossing.outer = depth == 0;
    ++depth;
    struct DepthGuard {
        int& depth;
        ~DepthGuard() { --depth; }
    } depth_guard{depth};
    d3d11_census::before_method(crossing, m);
    (manager.*Method)(m, slot);
    d3d11_census::after_method(crossing);
}
// ---- end P0 state census

/// The interface an entry belongs to, as `[d3d11-lock]` names it.
[[nodiscard]] constexpr const char* iface_name(d3d11_census::Iface iface) {
    using I = d3d11_census::Iface;
    switch (iface) {
    case I::Context: return "ID3D11DeviceContext";
    case I::Annotation: return "ID3DUserDefinedAnnotation";
    case I::VideoContext: return "ID3D11VideoContext";
    case I::Device: return "ID3D11Device";
    case I::Multithread: return "ID3D11Multithread";
    case I::DxgiDevice: return "IDXGIDevice";
    case I::DxgiResource: return "IDXGIResource";
    case I::DxgiSurface: return "IDXGISurface";
    case I::SwapChain: return "IDXGISwapChain";
    case I::SwapChainMedia: return "IDXGISwapChainMedia";
    case I::KeyedMutex: return "IDXGIKeyedMutex";
    case I::Buffer: return "ID3D11Buffer";
    case I::Texture: return "ID3D11Texture";
    case I::ShaderResourceView: return "ID3D11ShaderResourceView";
    case I::UnorderedAccessView: return "ID3D11UnorderedAccessView";
    case I::RenderTargetView: return "ID3D11RenderTargetView";
    case I::DepthStencilView: return "ID3D11DepthStencilView";
    case I::Query: return "ID3D11Query";
    case I::Fence: return "ID3D11Fence";
    case I::CommandList: return "ID3D11CommandList";
    case I::Shader: return "ID3D11Shader";
    case I::InputLayout: return "ID3D11InputLayout";
    case I::StateObject: return "ID3D11State";
    case I::ClassLinkage: return "ID3D11ClassLinkage";
    case I::DeviceContextState: return "ID3DDeviceContextState";
    case I::Other:
    case I::Count: break;
    }
    return "d3d11";
}

template <void (D3D11Manager::*Method)(Marshaler&, usize)>
void d3d11_com_method(void* context, Marshaler& m, u32 slot) {
    auto& manager = *static_cast<D3D11Manager*>(context);
    // ---- P0 state census: off, this is the one branch it costs an entry.
    if (d3d11_census::enabled) [[unlikely]] {
        d3d11_com_method_census<Method>(manager, m, slot);
        return;
    }
    // ---- end P0 state census
    device_lock_profile::lock(manager.api_mutex());
    std::unique_lock lock(manager.api_mutex(), std::adopt_lock);
    // One lookup of this thread's depth for the whole entry: the node it
    // names never moves (DxgiManager::api_lock_depth).
    int& depth = manager.api_lock_depth();
    const bool outer = depth == 0;
    if (outer) device_lock_profile::begin_hold();
    ++depth;
    struct DepthGuard {
        int& depth;
        bool outer;
        u32 slot;
        ~DepthGuard() {
            --depth;
            if (outer) {
                device_lock_profile::end_hold(
                    iface_name(census_iface<Method>()), slot);
            }
        }
    } depth_guard{depth, outer, slot};
    (manager.*Method)(m, slot);
}

template <void (D3D11Manager::*Method)(Marshaler&, usize)>
[[nodiscard]] DirectHandler d3d11_com_entry(D3D11Manager& manager,
                                            usize slot) {
    return DirectHandler{&d3d11_com_method<Method>, &manager,
                         static_cast<u32>(slot)};
}

}  // namespace

DllExports create_d3d11_thunks(D3D11Manager& manager) {
    DllExports exports;
    exports.entries.push_back({"D3D11CreateDevice", 0,
        [&manager](Marshaler& m) {
            manager.impl_D3D11CreateDevice(m);
        }, 40});
    exports.entries.push_back({"D3D11On12CreateDevice", 24,
        [&manager](Marshaler& m) {
            manager.impl_D3D11On12CreateDevice(m);
        }, 40});
    exports.entries.push_back({"D3D11CreateDeviceAndSwapChain", 0,
        [&manager](Marshaler& m) {
            manager.impl_D3D11CreateDeviceAndSwapChain(m);
        }, 48});

    for (usize slot = 0; slot < d3d11::DEVICE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_device_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DeviceMethod>(manager, slot),
            d3d11::DEVICE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::CONTEXT_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_context_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_ContextMethod>(manager, slot),
            d3d11::CONTEXT_STDCALL_BYTES[slot],
            d3d11::context_method_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::DXGI_DEVICE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_dxgi_device_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DxgiDeviceMethod>(manager, slot),
            d3d11::DXGI_DEVICE_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::BUFFER_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_buffer_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_BufferMethod>(manager, slot),
            d3d11::BUFFER_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::TEXTURE_2D_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_texture_2d_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_Texture2DMethod>(manager, slot),
            d3d11::TEXTURE_2D_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::DXGI_RESOURCE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_dxgi_resource_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DxgiResourceMethod>(manager, slot),
            d3d11::DXGI_RESOURCE_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::KEYED_MUTEX_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_keyed_mutex_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_KeyedMutexMethod>(manager, slot),
            d3d11::KEYED_MUTEX_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0;
         slot < d3d11::SHADER_RESOURCE_VIEW_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_shader_resource_view_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_ShaderResourceViewMethod>(manager, slot),
            d3d11::SHADER_RESOURCE_VIEW_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0;
         slot < d3d11::UNORDERED_ACCESS_VIEW_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_unordered_access_view_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_UnorderedAccessViewMethod>(manager, slot),
            d3d11::UNORDERED_ACCESS_VIEW_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0;
         slot < d3d11::RENDER_TARGET_VIEW_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_render_target_view_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_RenderTargetViewMethod>(manager, slot),
            d3d11::RENDER_TARGET_VIEW_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0;
         slot < d3d11::DEPTH_STENCIL_VIEW_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_depth_stencil_view_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DepthStencilViewMethod>(manager, slot),
            d3d11::DEPTH_STENCIL_VIEW_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::QUERY_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_query_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_QueryMethod>(manager, slot),
            d3d11::QUERY_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::FENCE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_fence_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_FenceMethod>(manager, slot),
            d3d11::FENCE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::COMMAND_LIST_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_command_list_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_CommandListMethod>(manager, slot),
            d3d11::COMMAND_LIST_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::SWAP_CHAIN_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_swap_chain_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_SwapChainMethod>(manager, slot),
            d3d11::SWAP_CHAIN_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::SWAP_CHAIN_MEDIA_METHOD_COUNT;
         ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_swap_chain_media_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_SwapChainMediaMethod>(manager, slot),
            d3d11::SWAP_CHAIN_MEDIA_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::ANNOTATION_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_annotation_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_AnnotationMethod>(manager, slot),
            d3d11::ANNOTATION_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::VERTEX_SHADER_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_vertex_shader_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_VertexShaderMethod>(manager, slot),
            d3d11::VERTEX_SHADER_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::PIXEL_SHADER_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_pixel_shader_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_PixelShaderMethod>(manager, slot),
            d3d11::PIXEL_SHADER_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::COMPUTE_SHADER_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_compute_shader_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_ComputeShaderMethod>(manager, slot),
            d3d11::COMPUTE_SHADER_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::DXGI_SURFACE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_dxgi_surface_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DxgiSurfaceMethod>(manager, slot),
            d3d11::DXGI_SURFACE_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::DEVICE_CONTEXT_STATE_METHOD_COUNT;
         ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_device_context_state_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DeviceContextStateMethod>(manager, slot),
            d3d11::DEVICE_CONTEXT_STATE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::GEOMETRY_SHADER_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_geometry_shader_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_GeometryShaderMethod>(manager, slot),
            d3d11::GEOMETRY_SHADER_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::HULL_SHADER_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_hull_shader_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_HullShaderMethod>(manager, slot),
            d3d11::HULL_SHADER_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::DOMAIN_SHADER_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_domain_shader_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DomainShaderMethod>(manager, slot),
            d3d11::DOMAIN_SHADER_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::INPUT_LAYOUT_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_input_layout_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_InputLayoutMethod>(manager, slot),
            d3d11::INPUT_LAYOUT_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::BLEND_STATE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_blend_state_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_BlendStateMethod>(manager, slot),
            d3d11::BLEND_STATE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::RASTERIZER_STATE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_rasterizer_state_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_RasterizerStateMethod>(manager, slot),
            d3d11::RASTERIZER_STATE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0;
         slot < d3d11::DEPTH_STENCIL_STATE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_depth_stencil_state_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_DepthStencilStateMethod>(manager, slot),
            d3d11::DEPTH_STENCIL_STATE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::SAMPLER_STATE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_sampler_state_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_SamplerStateMethod>(manager, slot),
            d3d11::SAMPLER_STATE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::VIDEO_CONTEXT_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_video_context_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_VideoContextMethod>(manager, slot),
            d3d11::VIDEO_CONTEXT_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::MULTITHREAD_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_multithread_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_MultithreadMethod>(manager, slot),
            d3d11::MULTITHREAD_STDCALL_BYTES[slot]));
    }
    for (usize slot = 0; slot < d3d11::CLASS_LINKAGE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_class_linkage_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_ClassLinkageMethod>(manager, slot),
            d3d11::CLASS_LINKAGE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    for (usize slot = 0; slot < d3d11::CLASS_INSTANCE_METHOD_COUNT; ++slot) {
        exports.entries.push_back(make_direct_export(
            d3d11_class_instance_method_export_name(slot),
            d3d11_com_entry<&D3D11Manager::impl_ClassInstanceMethod>(manager, slot),
            d3d11::CLASS_INSTANCE_STDCALL_BYTES[slot],
            d3d11::device_child_in_chain_class(slot)));
    }
    // The D3D10 facets of the same objects (d3d10_device.cpp).
    for (const D3D10VtableSpec& spec : d3d10_vtable_specs()) {
        for (usize slot = 0; slot < spec.slots; ++slot) {
            exports.entries.push_back({
                d3d10_method_export_name(spec.interface_name, slot), 0,
                [&manager, method = spec.method, slot](Marshaler& m) {
                    (manager.*method)(m, slot);
                }, spec.stdcall_bytes[slot]});
        }
    }
    // One device lock for every guest entry (D3D11Manager::m_api_mutex):
    // device methods are free-threaded by contract, and every object here
    // lives in the manager's shared maps.
    for (ExportEntry& entry : exports.entries) {
        // A COM method's direct entry takes the lock itself.
        if (entry.direct.fn != nullptr) continue;
        entry.handler = [&manager, inner = std::move(entry.handler)](
                            Marshaler& m) {
            device_lock_profile::lock(manager.api_mutex());
            std::unique_lock lock(manager.api_mutex(), std::adopt_lock);
            const bool outer = manager.api_lock_depth() == 0;
            if (outer) device_lock_profile::begin_hold();
            ++manager.api_lock_depth();
            struct DepthGuard {
                D3D11Manager& manager;
                bool outer;
                ~DepthGuard() {
                    --manager.api_lock_depth();
                    if (outer) device_lock_profile::end_hold("d3d11", 0);
                }
            } depth_guard{manager, outer};
            inner(m);
        };
    }
    return exports;
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_device_methods.cpp
/// @brief The ID3D11Device dispatcher and the device methods it answers
///        itself.
///
/// impl_DeviceMethod finds the device, answers IUnknown, OpenSharedResource,
/// the private-data, feature-level, exception-mode and immediate-context
/// methods, CreateQuery/CreatePredicate, CreateDeferredContext and
/// CreateFence, and forwards the rest of the vtable by area:
///   - CreateBuffer, CreateTexture1D/2D/3D: d3d11_device_resources.cpp
///   - CreateShaderResourceView: d3d11_device_srv.cpp
///   - CreateUnorderedAccessView: d3d11_device_uav.cpp
///   - CreateRenderTargetView, CreateDepthStencilView:
///     d3d11_device_rtv_dsv.cpp
///   - CheckFormatSupport, CheckMultisampleQualityLevels,
///     CheckFeatureSupport: d3d11_device_caps.cpp
///   - CreateInputLayout, Create*Shader, CreateClassLinkage:
///     d3d11_device_shaders.cpp
///   - the state-object creators: d3d11_device_states.cpp (validation,
///     canonical descriptions and deduplication in
///     d3d11_device_state_pool.cpp)
///   - the rest of ID3D11Device1-5: device_ext_method (d3d11_device_ext.cpp)
/// IDXGIDevice, the same object's DXGI facet, is in
/// d3d11_dxgi_device_methods.cpp, and the immediate context's
/// ID3D11VideoContext facet in d3d11_video_context_methods.cpp.
///
/// Device *creation* stays in d3d11.cpp: D3D11CreateDevice is an export, not a
/// vtable slot, and it runs before any of this exists.
///
/// Split out of d3d11.cpp once the constants moved to d3d11_internal.hpp.

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

void D3D11Manager::impl_DeviceMethod(Marshaler& m, usize slot) {
    const GuestAddr self = m.arg_ptr(0);
    Device* device = find_device(self);
    if (device == nullptr) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    switch (slot) {
    case 0:
        m.set_return(query_interface(m, self));
        return;
    case 1:
        m.set_return(add_ref(self));
        return;
    case 2:
        m.set_return(release(self));
        return;
    case 3:
    case 4:
    case 5:
    case 6:
        // CreateBuffer, CreateTexture1D/2D/3D: d3d11_device_resources.cpp.
        device_resource_method(m, slot, self, device);
        return;
    case 8:
        // CreateUnorderedAccessView: d3d11_device_uav.cpp.
        device_uav_method(m, slot, self, device);
        return;
    case 7:
        // CreateShaderResourceView: d3d11_device_srv.cpp.
        device_srv_method(m, slot, self, device);
        return;
    case 9:
    case 10:
        // CreateRenderTargetView / CreateDepthStencilView:
        // d3d11_device_rtv_dsv.cpp.
        device_rtv_dsv_method(m, slot, self, device);
        return;
    case 28:
    case 48: {
        // Learn OpenSharedResource / Device1::OpenSharedResource1: same
        // HANDLE + IID shape. Slot 28 opens GetSharedHandle values, slot 48
        // NT handles from CreateSharedHandle (d3d11_sharing.cpp).
        const u32 hr = open_shared_resource(
            m, self, slot == 48 ? SharedHandleKind::Nt
                                : SharedHandleKind::Kmt);
        d3d11_diag("ID3D11Device::%s handle=0x%llx -> 0x%08x",
                   slot == 48 ? "OpenSharedResource1" : "OpenSharedResource",
                   static_cast<unsigned long long>(m.arg(1)), hr);
        m.set_return(hr);
        return;
    }
    case 29:
    case 30:
    case 33:
        // CheckFormatSupport / CheckMultisampleQualityLevels /
        // CheckFeatureSupport: d3d11_device_caps.cpp.
        device_caps_method(m, slot, device);
        return;
    case 34:
        m.set_return(get_private_data(m, device->private_data,
                                      device->device_interface));
        return;
    case 35:
        m.set_return(set_private_data(m, device->private_data,
                                      device->device_interface));
        return;
    case 36:
        // ID3D11Device::SetPrivateDataInterface. The AddRef/Release land on an
        // interface the guest implements, through its own vtable -- see
        // winapi/core/guest_com.hpp.
        m.set_return(set_private_data_interface(m, device->private_data,
                                                device->device_interface));
        return;
    case 37:
        m.set_return(device->feature_level);
        return;
    case 38:
        m.set_return(device->creation_flags);
        return;
    case 39:
        m.set_return(d3d11::S_OK);
        return;
    case 40: {
        const GuestAddr context_out = m.arg_ptr(1);
        if (context_out == 0) {
            m.set_return(0);
            return;
        }
        (void)add_ref(device->context_interface);
        if (m.write_ptr(context_out, device->context_interface).is_err()) {
            (void)release(device->context_interface);
        }
        m.set_return(0);
        return;
    }
    case 41: {
        const u32 mode = static_cast<u32>(m.arg(1));
        if ((mode & ~1u) != 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        device->exception_mode = mode;
        m.set_return(d3d11::S_OK);
        return;
    }
    case 42:
        m.set_return(device->exception_mode);
        return;
    case 43: {
        const GuestAddr context_out = m.arg_ptr(1);
        if (context_out != 0) {
            (void)add_ref(device->context_interface);
            if (m.write_ptr(context_out,
                            device->context_interface).is_err()) {
                (void)release(device->context_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 24:
    case 25: {
        // CreateQuery(const D3D11_QUERY_DESC*, ID3D11Query**) and
        // CreatePredicate(const D3D11_QUERY_DESC*, ID3D11Predicate**).
        //
        // D3D11.3 20.4.1: every D3D11_QUERY value "must be supported", and a
        // predicate type made through CreateQuery is a predicate (it answers
        // QI(ID3D11Predicate) and drives SetPredication). CreatePredicate
        // takes only OCCLUSION_PREDICATE and SO_OVERFLOW_PREDICATE (Learn
        // CreatePredicate). D3D11_QUERY_MISC_PREDICATEHINT is legal only on
        // OCCLUSION_PREDICATE (Learn D3D11_QUERY_MISC_FLAG, D3D11.3 20.4.10).
        // Every refusal is E_INVALIDARG. Out parameters, measured
        // (d3d11_a10_queries_probe [create], [null]): CreateQuery writes NULL
        // on every failure; CreatePredicate leaves *ppPredicate untouched for
        // a NULL desc or a type it does not take, and writes NULL only once
        // the type is a predicate. A NULL out parameter is the S_FALSE "could
        // this be created" probe after full validation.
        const bool via_create_predicate = slot == 25;
        const char* method =
            via_create_predicate ? "CreatePredicate" : "CreateQuery";
        const GuestAddr desc_address = m.arg_ptr(1);
        const GuestAddr query_out = m.arg_ptr(2);
        if (query_out != 0 && !via_create_predicate) {
            (void)m.write_ptr(query_out, 0);
        }
        if (desc_address == 0) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const auto query_type = m_memory.read_u32(desc_address);
        const auto misc_flags = m_memory.read_u32(desc_address + 4);
        if (query_type.is_err() || misc_flags.is_err()) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const u32 type = query_type.value();
        const u32 misc = misc_flags.value();
        const bool type_ok =
            via_create_predicate
                ? type == D3D11_QUERY_OCCLUSION_PREDICATE ||
                      type == D3D11_QUERY_SO_OVERFLOW_PREDICATE
                : query_type_valid(type);
        if (!type_ok) {
            d3d11_diag("ID3D11Device::%s type=%u -> E_INVALIDARG", method,
                       type);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (query_out != 0 && via_create_predicate) {
            (void)m.write_ptr(query_out, 0);
        }
        const bool misc_ok =
            misc == 0 || (misc == D3D11_QUERY_MISC_PREDICATEHINT &&
                          type == D3D11_QUERY_OCCLUSION_PREDICATE);
        if (!misc_ok) {
            d3d11_diag("ID3D11Device::%s type=%u MiscFlags=0x%x -> "
                       "E_INVALIDARG",
                       method, type, misc);
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        if (query_out == 0) {
            m.set_return(1u);
            return;
        }
        const GuestAddr owner = m_interfaces.find(self)->second.owner;
        const GuestAddr query = create_query(owner, type, misc);
        if (query == 0) {
            // Learn (CreateQuery): E_OUTOFMEMORY when the object cannot be
            // allocated.
            m.set_return(d3d11::E_OUTOFMEMORY);
            return;
        }
        if (m.write_ptr(query_out, query).is_err()) {
            (void)release_object(query);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag("ID3D11Device::%s type=%u MiscFlags=0x%x -> 0x%llx", method,
                   type, misc, static_cast<unsigned long long>(query));
        m.set_return(d3d11::S_OK);
        return;
    }
    case 50:
    case 61: {
        const GuestAddr context_out = m.arg_ptr(1);
        if (context_out != 0) {
            (void)add_ref(device->context_interface);
            if (m.write_ptr(context_out,
                            device->context_interface).is_err()) {
                (void)release(device->context_interface);
            }
        }
        m.set_return(0);
        return;
    }
    case 11:
    case 12:
    case 13:
    case 14:
    case 15:
    case 16:
    case 17:
    case 18:
    case 19:
        // CreateInputLayout, Create*Shader, CreateClassLinkage:
        // d3d11_device_shaders.cpp.
        device_shader_method(m, slot, self);
        return;
    case 20:
    case 21:
    case 22:
    case 23:
    case 56:
        // CreateBlendState / DepthStencilState / RasterizerState(2) /
        // SamplerState: d3d11_device_states.cpp.
        device_state_method(m, slot, self, device);
        return;
    case 27:
        // CreateDeferredContext(ContextFlags, ppDeferredContext):
        // d3d11_deferred.cpp.
        create_deferred_context(m, self);
        return;
    case 68: {
        // CreateFence(UINT64 InitialValue, D3D11_FENCE_FLAG Flags,
        //             REFIID ReturnedInterface, void** ppFence).
        //
        // InitialValue is 64-bit, so every parameter after it sits one slot
        // later on x86 than on x64; read_arg_u64 reports where.
        usize next = 0;
        const u64 initial_value = read_arg_u64(m, m_is_64bit, 1, next);
        const u32 flags = m.arg32(next);
        const GuestAddr iid_address = m.arg_ptr(next + 1);
        const GuestAddr fence_out = m.arg_ptr(next + 2);
        if (fence_out != 0) (void)m.write_ptr(fence_out, 0);
        // D3D11_FENCE_FLAG (Learn): NONE 0, SHARED 0x2, SHARED_CROSS_ADAPTER
        // 0x4, NON_MONITORED 0x8. Measured (a12 [fence]): 0 and 0x1 make a
        // plain fence; CROSS_ADAPTER without SHARED, NON_MONITORED (this
        // adapter has no non-monitored fences either) and any undefined bit
        // are E_INVALIDARG.
        constexpr u32 kFenceShared = 0x2u;
        constexpr u32 kFenceSharedCrossAdapter = 0x4u;
        constexpr u32 kFenceNonMonitored = 0x8u;
        if ((flags & ~0xFu) != 0 || (flags & kFenceNonMonitored) != 0 ||
            ((flags & kFenceSharedCrossAdapter) != 0 &&
             (flags & kFenceShared) == 0)) {
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        GuidBytes iid{};
        if (!read_guid(iid_address, iid) ||
            (!guid_equal(iid, IID_IUNKNOWN) &&
             !guid_equal(iid, IID_ID3D11DEVICECHILD) &&
             !guid_equal(iid, IID_ID3D11FENCE))) {
            m.set_return(d3d11::E_NOINTERFACE);
            return;
        }
        if ((flags & kFenceShared) != 0) {
            // A shared fence is an NT handle onto the backend timeline that
            // another device (or process) opens with OpenSharedFence; that
            // handle transport does not exist yet.
            static std::once_flag reported;
            std::call_once(reported, [] {
                std::fprintf(stderr,
                             "[vitreo] UNIMPLEMENTED d3d11!CreateFence: "
                             "D3D11_FENCE_FLAG_SHARED -> E_NOTIMPL\n");
            });
            m.set_return(d3d11::E_NOTIMPL);
            return;
        }
        if (fence_out == 0) {
            // Learn (CreateFence): a NULL ppFence validates only (measured
            // S_FALSE).
            m.set_return(1u);
            return;
        }
        if (!m_backend.supports_fences()) {
            d3d11_diag("ID3D11Device5::CreateFence -> E_NOTIMPL (backend has no "
                       "timeline fences)");
            m.set_return(d3d11::E_NOTIMPL);
            return;
        }
        auto backend_fence = m_backend.create_fence(initial_value);
        if (!backend_fence) {
            m.set_return(d3d11::E_FAIL);
            return;
        }
        const GuestAddr address = allocate_object(m_fence_vtable);
        if (address == 0) {
            m.set_return(0x8007000Eu);  // E_OUTOFMEMORY
            return;
        }
        Fence fence{};
        fence.interface_address = address;
        fence.device_owner = self;
        fence.flags = flags;
        fence.backend = std::move(backend_fence);
        m_fences.emplace(address, std::move(fence));
        acquire_parent(self);
        if (m.write_ptr(fence_out, address).is_err()) {
            (void)release_object(address);
            m.set_return(d3d11::E_POINTER);
            return;
        }
        d3d11_diag("ID3D11Device5::CreateFence initial=%llu -> 0x%llx",
                   static_cast<unsigned long long>(initial_value),
                   static_cast<unsigned long long>(address));
        m.set_return(d3d11::S_OK);
        return;
    }
    default:
        if (device_ext_method(m, slot, self)) return;
        unsupported(m, "ID3D11Device", slot);
        return;
    }
}

} // namespace vitreo::winapi

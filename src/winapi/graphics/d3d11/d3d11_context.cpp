// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d11_context.cpp
/// @brief ID3D11DeviceContext — the vtable a guest issues draw work through.
///
/// impl_ContextMethod is the one entry the whole immediate-context interface
/// arrives at: every vtable entry is a slot index, and the switch on that
/// index *is* the interface. It does the per-call work every slot shares (the
/// census, the context-record memo, the device lookup, deferred recording,
/// the D3D10 gate, predication and the UAV write note) and owns the IUnknown,
/// private-data, query, fence, ClearState/Flush/command-list, predication and
/// min-LOD arms.
///
/// The remaining arms live by family in d3d11_context_<family>.cpp
/// (constant_buffers, shader_stages, fixed_stages, draw, copy, cpu_access,
/// clear, getters), and the switch here routes each slot to exactly one of
/// them. Map/Unmap, the binding hazards and ClearState's reset are in
/// d3d11_map_texture.cpp, d3d11_map_buffer.cpp, d3d11_mapped_image.cpp,
/// d3d11_bind_hazards.cpp and d3d11_context_state.cpp.
///
/// Split out of d3d11.cpp once the constants it reads moved to
/// d3d11_internal.hpp; closed in both directions after that.

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

void D3D11Manager::impl_ContextMethod(Marshaler& m, usize slot) {
    // Diagnostic census. A survey built from failure messages alone only sees
    // methods that log; one that is implemented-but-wrong, or that silently
    // does nothing, never appears. Counting every call gives the actual API
    // surface a title depends on, which is the only complete input to "what
    // does this game need". Gated, and dumped once at exit.
    if (d3d11_census_enabled()) {
        static std::array<std::atomic<u64>, d3d11::CONTEXT_METHOD_COUNT>
            context_calls{};
        // Dump periodically rather than at exit: Vitreo's teardown calls
        // _exit, which does not run atexit handlers, and a killed guest never
        // reaches one either.
        static std::atomic<u64> total{0};
        const u64 seen = total.fetch_add(1, std::memory_order_relaxed) + 1;
        const auto dump = [] {
                std::fprintf(stderr, "[d3d11-census] ID3D11DeviceContext\n");
                for (usize i = 0; i < context_calls.size(); ++i) {
                    const u64 count =
                        context_calls[i].load(std::memory_order_relaxed);
                    if (count != 0) {
                        std::fprintf(stderr,
                                     "[d3d11-census]   slot %3zu  %llu\n", i,
                                     static_cast<unsigned long long>(count));
                    }
                }
        };
        if (slot < context_calls.size()) {
            context_calls[slot].fetch_add(1, std::memory_order_relaxed);
        }
        if ((seen % 5000u) == 0u) dump();
    }
    const GuestAddr self = m.arg_ptr(0);
    // WP-R1 (com_direct): ONE context-record lookup per call. The interface
    // -> device, context state and deferred? answers are memoized across
    // calls (m_context_memo) and reused while m_context_lookup_generation is
    // unchanged; otherwise they are looked up here once, and the predication
    // test and the UAV note below reuse the record instead of finding it
    // again. Off, every lookup is made where it always was.
    const bool lean = com_direct::active();
    Device* device = nullptr;
    ContextState* context_state = nullptr;
    bool deferred = false;
    if (lean && m_context_memo.self == self &&
        m_context_memo.generation == m_context_lookup_generation) {
        device = m_context_memo.device;
        context_state = m_context_memo.state;
        deferred = m_context_memo.deferred;
    } else {
        device = find_device(self);
        if (device != nullptr) {
            deferred = is_deferred_context(self);
            if (lean) {
                const auto state = m_context_states.find(self);
                if (state != m_context_states.end()) {
                    context_state = &state->second;
                    m_context_memo = ContextMemo{self,
                                                 m_context_lookup_generation,
                                                 device, context_state,
                                                 deferred};
                }
            }
        }
    }
    // The record in hand is only trusted while no lookup it came from could
    // have changed -- a call below can run guest code that calls back in.
    const u64 lookup_generation = m_context_lookup_generation;
    if (device == nullptr) {
        m.set_return(slot == 1 || slot == 2 ? 0 : d3d11::E_INVALIDARG);
        return;
    }
    // A deferred context records instead of executing (d3d11_deferred.cpp).
    // What it does not fully handle -- the state setters, which also update
    // its own bindings, and the getters -- runs below against its own state.
    if (deferred && deferred_context_method(m, slot, self)) {
        return;
    }
    // A D3D10 context state turns most of the immediate context off (Learn,
    // ID3D11Device1::CreateDeviceContextState remarks): measured, the
    // setters, draws and copies do nothing and the getters report an empty
    // state. The D3D10 device's own forwarded calls pass (d3d10_device.cpp).
    if (device->d3d10_active && m_d3d10_forwarding == 0 &&
        self == device->context_interface &&
        d3d11_context_slot_disabled_by_d3d10(slot)) {
        const bool getter = (slot >= 72 && slot <= 109) || slot == 56 ||
                            slot == 112;
        if (getter) {
            d3d10_run_on_empty_state(self,
                                     [&] { impl_ContextMethod(m, slot); });
        } else {
            m.set_return(0);
        }
        return;
    }
    // Learn (SetPredication): the operations on its list "are not actually
    // performed if the resulting predicate data of the predicate is equal to
    // the PredicateValue". All of them return void.
    // (lean: predicated_off's first test -- no predicate set -- answered
    // from the record in hand.)
    if (predicated_context_slot(slot) &&
        !(context_state != nullptr &&
          lookup_generation == m_context_lookup_generation &&
          context_state->predicate == 0) &&
        predicated_off(self)) {
        d3d11_diag("ID3D11DeviceContext slot %zu -> skipped by predication",
                   slot);
        m.set_return(0);
        return;
    }
    // Draw, DrawIndexed, DrawIndexedInstanced, DrawInstanced, DrawAuto, the
    // two indirect draws, Dispatch and DispatchIndirect: once recorded, note
    // the buffers their UAVs wrote.
    struct UavWriteNote {
        D3D11Manager& manager;
        GuestAddr context;
        int kind;  // -1 none, 0 draw, 1 dispatch
        // lean: the context's record, while `generation` is current.
        const ContextState* state;
        u64 generation;
        ~UavWriteNote() {
            if (kind < 0) return;
            // With no UAV bound on the stage there is nothing to note
            // (note_uav_buffer_writes skips every unbound slot), so the
            // record in hand answers without the lookup.
            if (state != nullptr &&
                generation == manager.m_context_lookup_generation) {
                const auto& views =
                    kind == 1 ? state->compute_unordered_access_views
                              : state->output_merger_unordered_access_views;
                if (std::all_of(views.begin(), views.end(),
                                [](GuestAddr view) { return view == 0; })) {
                    return;
                }
            }
            manager.note_uav_buffer_writes(context, kind == 1);
        }
    } const uav_write_note{
        *this, self,
        slot == 41 || slot == 42 ? 1
        : (slot == 12 || slot == 13 || slot == 20 || slot == 21 ||
           (slot >= 38 && slot <= 40))
            ? 0
            : -1,
        context_state, lookup_generation};
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
    case 3: {
        const GuestAddr device_out = m.arg_ptr(1);
        if (device_out != 0) {
            (void)add_ref(device->device_interface);
            if (m.write_ptr(device_out, device->device_interface).is_err()) {
                (void)release(device->device_interface);
            }
        }
        m.set_return(0);
        return;
    }
    // The immediate context's own store: it shares nothing with the device
    // (measured, a12 [pd] share).
    case 4:
        m.set_return(get_private_data(m, device->context_private_data,
                                      device->context_interface));
        return;
    case 5:
        m.set_return(set_private_data(m, device->context_private_data,
                                      device->context_interface));
        return;
    case 6:
        m.set_return(set_private_data_interface(
            m, device->context_private_data, device->context_interface));
        return;
    case 27:
        // Begin(ID3D11Asynchronous*): d3d11_query.cpp.
        query_begin(self, m.arg_ptr(1));
        m.set_return(0);
        return;
    case 28:
        // End(ID3D11Asynchronous*).
        query_end(self, m.arg_ptr(1));
        if (m_diag_transfer_log != nullptr) [[unlikely]] {
            diag_log_event_query(m.arg_ptr(1), true, 0, 0, 0);
        }
        m.set_return(0);
        return;
    case 29: {
        // GetData(pAsync, pData, DataSize, GetDataFlags).
        const u32 hr = query_get_data(self, m.arg_ptr(1), m.arg_ptr(2),
                                      static_cast<u32>(m.arg(3)),
                                      static_cast<u32>(m.arg(4)));
        if (m_diag_transfer_log != nullptr) [[unlikely]] {
            diag_log_event_query(m.arg_ptr(1), false,
                                 static_cast<u32>(m.arg(4)),
                                 static_cast<u32>(m.arg(3)), hr);
        }
        m.set_return(hr);
        return;
    }
    // ID3D11DeviceContext4, slots 147 and 148. Learn: Signal makes the fence
    // reach Value once the GPU has finished the work submitted before the call,
    // and Wait makes later work not begin until the fence reaches Value. Both
    // are ordering operations, so the backend records them into the command
    // stream rather than applying them at once.
    case 147:   // Signal
    case 148: { // Wait
        const GuestAddr fence_address = m.arg_ptr(1);
        usize next = 0;
        const u64 value = read_arg_u64(m, m_is_64bit, 2, next);
        (void)next;
        auto fence = m_fences.find(fence_address);
        if (fence == m_fences.end() || !fence->second.backend) {
            d3d11_diag("ID3D11DeviceContext4::%s fence=0x%llx is not a live "
                       "fence", slot == 147 ? "Signal" : "Wait",
                       static_cast<unsigned long long>(fence_address));
            m.set_return(d3d11::E_INVALIDARG);
            return;
        }
        const bool ok = slot == 147 ? fence->second.backend->signal(value)
                                    : fence->second.backend->wait_gpu(value);
        d3d11_diag("ID3D11DeviceContext4::%s fence=0x%llx value=%llu -> %s",
                   slot == 147 ? "Signal" : "Wait",
                   static_cast<unsigned long long>(fence_address),
                   static_cast<unsigned long long>(value),
                   ok ? "recorded" : "failed");
        m.set_return(ok ? d3d11::S_OK : d3d11::E_FAIL);
        return;
    }

    case 110: {
        // ClearState — Learn IDL slot 110 (was miswired at 109).
        clear_context_state(self);
        m.set_return(0);
        return;
    }
    case 111:
        // Flush — Learn IDL slot 111. Also the producer's synchronization
        // point for shared resources (Learn OpenSharedResource: "If a shared
        // texture is updated on one device ID3D11DeviceContext::Flush must be
        // called on that device"): what this device wrote to them is pushed
        // to their shared storage and published once it completes.
        (void)publish_shared_writes();
        m_backend.flush();
        m.set_return(0);
        return;
    case 112:
        // GetType — Learn returns D3D11_DEVICE_CONTEXT_TYPE in EAX.
        m.set_return(0); // D3D11_DEVICE_CONTEXT_IMMEDIATE
        return;
    case 113:
        // GetContextFlags — Learn returns UINT in EAX.
        m.set_return(0);
        return;
    case 114: {
        // FinishCommandList(RestoreDeferredContextState, ppCommandList).
        // Learn: valid only on a DEFERRED context (d3d11_deferred.cpp); an
        // immediate one returns DXGI_ERROR_INVALID_CALL and NULLs the out.
        const GuestAddr list_out = m.arg_ptr(2);
        if (list_out != 0) (void)m.write_ptr(list_out, 0);
        d3d11_diag("ID3D11DeviceContext::FinishCommandList on an immediate "
                   "context -> DXGI_ERROR_INVALID_CALL");
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    case 58:
        // ExecuteCommandList(pCommandList, RestoreContextState): replays the
        // list's recorded calls here (d3d11_deferred.cpp). A deferred context
        // records this call instead. The list stays valid, so it may be
        // executed again.
        execute_command_list(self, m.arg_ptr(1), m.arg32(2) != 0);
        m.set_return(0);
        return;

    // Predication. Learn (SetPredication / GetPredication): a predicate makes
    // the operations on the SetPredication list conditional. The predicate
    // is evaluated before each of them (predicated_off, d3d11_query.cpp);
    // here it is only bound.
    case 30: {
        // SetPredication(ID3D11Predicate *pPredicate, BOOL PredicateValue).
        //
        // Learn: "Use this method to denote that subsequent rendering and
        // resource manipulation commands are not actually performed if the
        // resulting predicate data of the predicate is equal to the
        // PredicateValue." Returns void, so an object that is not a predicate
        // is dropped with a diagnostic.
        auto& state = m_context_states[self];
        const GuestAddr next_predicate = m.arg_ptr(1);
        const u32 value = static_cast<u32>(m.arg(2));
        m.set_return(0);
        if (next_predicate != 0) {
            auto found = m_queries.find(next_predicate);
            if (found == m_queries.end() || !found->second.predicate) {
                d3d11_diag("ID3D11DeviceContext::SetPredication 0x%llx -> "
                           "dropped (not a predicate)",
                           static_cast<unsigned long long>(next_predicate));
                return;
            }
            ++found->second.ref_count;
        }
        const GuestAddr previous = state.predicate;
        state.predicate = next_predicate;
        // Learn: with a NULL predicate "the value of PredicateValue is
        // irrelevant but will be preserved for GetPredication", so it is stored
        // either way.
        state.predicate_value = value;
        if (previous != 0) (void)release_query(previous);
        return;
    }
    case 86: {
        // GetPredication(ID3D11Predicate **ppPredicate,
        //                BOOL *pPredicateValue).
        auto& state = m_context_states[self];
        const GuestAddr predicate_out = m.arg_ptr(1);
        const GuestAddr value_out = m.arg_ptr(2);
        if (predicate_out != 0) {
            (void)m.write_ptr(predicate_out, 0);
            if (state.predicate != 0 && m_queries.contains(state.predicate)) {
                (void)retain_object(state.predicate);
                if (m.write_ptr(predicate_out, state.predicate).is_err()) {
                    (void)release_object(state.predicate);
                }
            }
        }
        if (value_out != 0) {
            (void)m.write_u32(value_out, state.predicate_value);
        }
        m.set_return(0);
        return;
    }

    // Learn (SetResourceMinLOD / GetResourceMinLOD): the clamp belongs to a
    // resource created with D3D11_RESOURCE_MISC_RESOURCE_CLAMP ("you must set
    // the D3D11_RESOURCE_MISC_RESOURCE_CLAMP flag"). Measured on Windows
    // (a06 probe LOD lines): Get returns what Set stored, verbatim (-1 and 10
    // included), and 0 for any other resource, which Set leaves alone.
    case 55: {
        const GuestAddr resource = m.arg_ptr(1);
        // FLOAT MinLOD: XMM1 on x64, the stack on x86.
        const float min_lod = m.arg_f32(2);
        auto texture = m_textures.find(resource);
        if (texture != m_textures.end() &&
            ((texture->second.volume ? texture->second.volume_desc.misc_flags
                                     : texture->second.desc.misc_flags) &
             D3D11_RESOURCE_MISC_RESOURCE_CLAMP) != 0) {
            texture->second.min_lod = min_lod;
            // The value round-trips; clamping sampling to it is not built.
            d3d11_unimplemented_once(
                "SetResourceMinLOD",
                "the clamp is stored for GetResourceMinLOD but not applied "
                "to sampling");
        }
        m.set_return(0);
        return;
    }
    case 56: {
        const GuestAddr resource = m.arg_ptr(1);
        auto texture = m_textures.find(resource);
        // Only a RESOURCE_CLAMP resource ever stores a value; any other
        // keeps its initial 0.
        const float min_lod =
            texture != m_textures.end() ? texture->second.min_lod : 0.0f;
        // FLOAT return: XMM0 on x64, ST(0) on x86 -- never RAX/EAX.
        m.set_return_f32(min_lod);
        return;
    }

    case 7:    // VSSetConstantBuffers
    case 16:   // PSSetConstantBuffers
    case 22:   // GSSetConstantBuffers
    case 62:   // HSSetConstantBuffers
    case 66:   // DSSetConstantBuffers
    case 71:   // CSSetConstantBuffers
    case 119:  // VSSetConstantBuffers1
    case 120:  // HSSetConstantBuffers1
    case 121:  // DSSetConstantBuffers1
    case 122:  // GSSetConstantBuffers1
    case 123:  // PSSetConstantBuffers1
    case 124:  // CSSetConstantBuffers1
    case 125:  // VSGetConstantBuffers1
    case 126:  // HSGetConstantBuffers1
    case 127:  // DSGetConstantBuffers1
    case 128:  // GSGetConstantBuffers1
    case 129:  // PSGetConstantBuffers1
    case 130:  // CSGetConstantBuffers1
        context_constant_buffer_method(m, slot, self);
        return;
    case 8:    // PSSetShaderResources
    case 9:    // PSSetShader
    case 10:   // PSSetSamplers
    case 11:   // VSSetShader
    case 23:   // GSSetShader
    case 25:   // VSSetShaderResources
    case 26:   // VSSetSamplers
    case 31:   // GSSetShaderResources
    case 32:   // GSSetSamplers
    case 59:   // HSSetShaderResources
    case 60:   // HSSetShader
    case 61:   // HSSetSamplers
    case 63:   // DSSetShaderResources
    case 64:   // DSSetShader
    case 65:   // DSSetSamplers
    case 67:   // CSSetShaderResources
    case 68:   // CSSetUnorderedAccessViews
    case 69:   // CSSetShader
    case 70:   // CSSetSamplers
        context_shader_stage_method(m, slot, self);
        return;
    case 17:   // IASetInputLayout
    case 18:   // IASetVertexBuffers
    case 19:   // IASetIndexBuffer
    case 24:   // IASetPrimitiveTopology
    case 33:   // OMSetRenderTargets
    case 34:   // OMSetRenderTargetsAndUnorderedAccessViews
    case 35:   // OMSetBlendState
    case 36:   // OMSetDepthStencilState
    case 37:   // SOSetTargets
    case 43:   // RSSetState
    case 44:   // RSSetViewports
    case 45:   // RSSetScissorRects
        context_fixed_stage_method(m, slot, self);
        return;
    case 12:   // DrawIndexed
    case 13:   // Draw
    case 20:   // DrawIndexedInstanced
    case 21:   // DrawInstanced
    case 38:   // DrawAuto
    case 39:   // DrawIndexedInstancedIndirect
    case 40:   // DrawInstancedIndirect
    case 41:   // Dispatch
    case 42:   // DispatchIndirect
        context_draw_method(m, slot, self);
        return;
    case 46:   // CopySubresourceRegion
    case 47:   // CopyResource
    case 49:   // CopyStructureCount
    case 57:   // ResolveSubresource
    case 115:  // CopySubresourceRegion1
        context_copy_method(m, slot);
        return;
    case 14:   // Map
    case 15:   // Unmap
    case 48:   // UpdateSubresource
    case 116:  // UpdateSubresource1
        context_cpu_access_method(m, slot, self);
        return;
    case 50:   // ClearRenderTargetView
    case 51:   // ClearUnorderedAccessViewUint
    case 52:   // ClearUnorderedAccessViewFloat
    case 53:   // ClearDepthStencilView
    case 54:   // GenerateMips
        context_clear_method(m, slot);
        return;
    case 72:   // VSGetConstantBuffers
    case 73:   // PSGetShaderResources
    case 74:   // PSGetShader
    case 75:   // PSGetSamplers
    case 76:   // VSGetShader
    case 77:   // PSGetConstantBuffers
    case 78:   // IAGetInputLayout
    case 79:   // IAGetVertexBuffers
    case 80:   // IAGetIndexBuffer
    case 81:   // GSGetConstantBuffers
    case 82:   // GSGetShader
    case 83:   // IAGetPrimitiveTopology
    case 84:   // VSGetShaderResources
    case 85:   // VSGetSamplers
    case 87:   // GSGetShaderResources
    case 88:   // GSGetSamplers
    case 89:   // OMGetRenderTargets
    case 90:   // OMGetRenderTargetsAndUnorderedAccessViews
    case 91:   // OMGetBlendState
    case 92:   // OMGetDepthStencilState
    case 93:   // SOGetTargets
    case 94:   // RSGetState
    case 95:   // RSGetViewports
    case 96:   // RSGetScissorRects
    case 97:   // HSGetShaderResources
    case 98:   // HSGetShader
    case 99:   // HSGetSamplers
    case 100:  // HSGetConstantBuffers
    case 101:  // DSGetShaderResources
    case 102:  // DSGetShader
    case 103:  // DSGetSamplers
    case 104:  // DSGetConstantBuffers
    case 105:  // CSGetShaderResources
    case 106:  // CSGetUnorderedAccessViews
    case 107:  // CSGetShader
    case 108:  // CSGetSamplers
    case 109:  // CSGetConstantBuffers
        context_getter_method(m, slot, self);
        return;

    default:
        if (context_ext_method(m, slot, self)) return;
        unsupported(m, "ID3D11DeviceContext", slot);
        return;
    }
}

} // namespace vitreo::winapi

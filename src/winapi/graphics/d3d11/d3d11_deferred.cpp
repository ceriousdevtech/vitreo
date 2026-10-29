// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d11_deferred.cpp
/// @brief Deferred contexts and command lists: the ID3D11DeviceContext that
///        records instead of executing, FinishCommandList and
///        ExecuteCommandList.
///
/// Sources: Learn (ID3D11Device::CreateDeferredContext,
/// ID3D11DeviceContext::FinishCommandList / ExecuteCommandList / Map /
/// UpdateSubresource / GetData / GetType, ID3D11DeviceContext4::Signal) and the
/// Windows measurements in tools/oracle_probe/d3d11_a12_object_model_probe
/// (section [def]) and d3d11_deferred_rules_probe (.expected beside each).
///
/// Design. Vitreo reports DriverCommandLists = FALSE (D3D11_FEATURE_THREADING),
/// the same answer WARP gives, and implements command lists the way the
/// Windows runtime does for such a driver: a deferred context RECORDS each
/// call -- slot number, argument values, a copy of everything its pointer
/// arguments point at, and a reference on every object it names -- and
/// ExecuteCommandList replays those calls on the executing context through
/// the ordinary method dispatch (impl_ContextMethod). Nothing recorded runs
/// before the list executes: not a draw, not a Clear, UpdateSubresource,
/// Copy*, query Begin/End or a Map upload. Replaying at execute time also
/// resolves every resource then, so a list sees the storage its resources
/// have when it runs, and predication, occlusion counting and stream output
/// behave exactly as on the immediate context.
///
/// State (Learn, ExecuteCommandList / FinishCommandList): a list never
/// inherits the executing context's state. It starts from the state its
/// deferred context had when recording began -- the default, or what a
/// FinishCommandList(TRUE) preserved -- and afterwards the executing context
/// gets its own state back (RestoreContextState TRUE) or the default (FALSE).
/// The deferred context's own state is updated at record time too, so its
/// Get* methods answer what was set.

#include "winapi/graphics/d3d11/d3d11.hpp"
#include "winapi/graphics/d3d11/d3d11_internal.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/format_converter.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace vitreo::winapi {

namespace {

/// winerror.h D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD: the
/// first Map of a resource after CreateDeferredContext or FinishCommandList
/// was not WRITE_DISCARD (measured on buffers, and for every NO_OVERWRITE
/// of a texture).
constexpr u32 kMapWithoutInitialDiscard = 0x887C0004u;
/// OMSetRenderTargetsAndUnorderedAccessViews: NumRTVs /
/// NumUAVs = D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL /
/// D3D11_KEEP_UNORDERED_ACCESS_VIEWS leave those bindings alone.
constexpr u32 kKeepBindings = 0xFFFFFFFFu;
/// No D3D11 register space is wider (128 t# slots); a larger count is refused
/// by the method before it reads the array, so there is nothing to copy.
constexpr u32 kMaxRecordedArray = 128;
constexpr usize kArenaChunkBytes = 64 * 1024;
constexpr usize kPooledArenaChunks = 64;
/// Synthetic call frame: the arguments (x64 home area + stack slots, x86
/// stack slots) below kReplayMappedOffset, the D3D11_MAPPED_SUBRESOURCE a
/// replayed Map writes above it.
constexpr usize kReplayFrameBytes = 256;
constexpr usize kReplayMappedOffset = 0x80;

}  // namespace

// -----------------------------------------------------------------------------
// Object model
// -----------------------------------------------------------------------------

GuestAddr D3D11Manager::deferred_context_of(GuestAddr interface_address) const {
    if (m_deferred_contexts.find(interface_address) !=
        m_deferred_contexts.end()) {
        return interface_address;
    }
    const auto facet = m_deferred_annotations.find(interface_address);
    return facet != m_deferred_annotations.end() ? facet->second : 0;
}

void D3D11Manager::create_deferred_context(Marshaler& m, GuestAddr self) {
    // CreateDeferredContext(ContextFlags, ppDeferredContext); the 1/2/3
    // versions take the same arguments and return the same object.
    const u32 context_flags = m.arg32(1);
    const GuestAddr context_out = m.arg_ptr(2);
    Device* device = find_device(self);
    if (device == nullptr) {
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
    // Learn: ContextFlags is reserved and must be 0. Measured: E_INVALIDARG
    // with *ppDeferredContext untouched.
    if (context_flags != 0) {
        d3d11_diag("ID3D11Device::CreateDeferredContext flags=0x%x -> "
                   "E_INVALIDARG (reserved, must be 0)", context_flags);
        m.set_return(d3d11::E_INVALIDARG);
        return;
    }
    // Learn (CreateDeferredContext): "returns DXGI_ERROR_INVALID_CALL if the
    // device was created with the D3D11_CREATE_DEVICE_SINGLETHREADED value".
    // Measured: the out-parameter is left untouched.
    if ((device->creation_flags & d3d11::CREATE_DEVICE_SINGLETHREADED) != 0) {
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return;
    }
    if (context_out == 0) {
        // Learn: a NULL ppDeferredContext validates the arguments only.
        m.set_return(0x00000001u);  // S_FALSE
        return;
    }
    const GuestAddr owner = m_interfaces.find(self)->second.owner;
    const GuestAddr address = allocate_object(m_context_vtable);
    const GuestAddr annotation =
        address != 0 ? allocate_object(m_annotation_vtable) : 0;
    if (address == 0 || annotation == 0) {
        if (address != 0) free_object(address);
        m.set_return(d3d11::E_OUTOFMEMORY);
        return;
    }
    m_interfaces.emplace(address, Interface{owner, InterfaceType::Context});
    m_interfaces.emplace(annotation,
                         Interface{owner, InterfaceType::Annotation});
    m_context_states.emplace(address, ContextState{});
    DeferredContext context{};
    context.device_owner = owner;
    context.annotation_interface = annotation;
    context.context_flags = context_flags;
    context.recording = new_recording();
    // `address` becomes a deferred context: no memoized lookup may still
    // call it immediate (m_context_lookup_generation).
    invalidate_context_lookups();
    m_deferred_contexts.emplace(address, std::move(context));
    m_deferred_annotations.emplace(annotation, address);
    // A device child holds its device (measured: device-refs+1).
    (void)add_ref(device->device_interface);
    if (m.write_ptr(context_out, address).is_err()) {
        (void)deferred_release(address);
        m.set_return(d3d11::E_POINTER);
        return;
    }
    d3d11_diag("ID3D11Device::CreateDeferredContext -> 0x%llx",
               static_cast<unsigned long long>(address));
    m.set_return(d3d11::S_OK);
}

u32 D3D11Manager::deferred_add_ref(GuestAddr context) {
    auto found = m_deferred_contexts.find(context);
    if (found == m_deferred_contexts.end()) return 0;
    return ++found->second.ref_count;
}

u32 D3D11Manager::deferred_release(GuestAddr context) {
    auto found = m_deferred_contexts.find(context);
    if (found == m_deferred_contexts.end() || found->second.ref_count == 0) {
        return 0;
    }
    if (--found->second.ref_count > 0) return found->second.ref_count;
    // The last reference: the recording, the bindings and both facets go,
    // then the device reference -- last, because it can destroy the device.
    DeferredContext dead = std::move(found->second);
    invalidate_context_lookups();  // the erases below (context memo)
    m_deferred_contexts.erase(found);
    m_deferred_annotations.erase(dead.annotation_interface);
    if (dead.recording) release_recording(*dead.recording);
    clear_context_state(context);
    // Again after clear_context_state, whose releases can run guest code.
    invalidate_context_lookups();
    m_context_states.erase(context);
    m_interfaces.erase(context);
    m_interfaces.erase(dead.annotation_interface);
    free_object(context);
    free_object(dead.annotation_interface);
    const auto device = m_devices.find(dead.device_owner);
    if (device != m_devices.end()) {
        (void)release(device->second.device_interface);
    }
    return 0;
}

u32 D3D11Manager::deferred_query_interface(Marshaler& m, GuestAddr context) {
    // Measured (a12 [qi] deferred_context): IUnknown, ID3D11DeviceChild,
    // ID3D11DeviceContext..4 are the deferred context itself -- never the
    // immediate one -- and ID3DUserDefinedAnnotation is its own facet. It
    // answers nothing else: not ID3D11Multithread, not ID3D11VideoContext.
    const GuestAddr object_out = m.arg_ptr(2);
    if (object_out == 0) return d3d11::E_POINTER;
    (void)m.write_ptr(object_out, 0);
    auto found = m_deferred_contexts.find(context);
    GuidBytes iid{};
    if (found == m_deferred_contexts.end() || !read_guid(m.arg_ptr(1), iid)) {
        return d3d11::E_NOINTERFACE;
    }
    GuestAddr result = 0;
    if (guid_equal(iid, IID_IUNKNOWN) ||
        guid_equal(iid, IID_ID3D11DEVICECHILD) ||
        guid_equal(iid, IID_ID3D11DEVICECONTEXT) ||
        guid_equal(iid, IID_ID3D11DEVICECONTEXT1) ||
        guid_equal(iid, IID_ID3D11DEVICECONTEXT2) ||
        guid_equal(iid, IID_ID3D11DEVICECONTEXT3) ||
        guid_equal(iid, IID_ID3D11DEVICECONTEXT4)) {
        result = context;
    } else if (guid_equal(iid, IID_ID3DUSERDEFINEDANNOTATION)) {
        result = found->second.annotation_interface;
    }
    if (result == 0) return d3d11::E_NOINTERFACE;
    ++found->second.ref_count;
    if (m.write_ptr(object_out, result).is_err()) {
        --found->second.ref_count;
        return d3d11::E_POINTER;
    }
    return d3d11::S_OK;
}

// -----------------------------------------------------------------------------
// References a recording holds
// -----------------------------------------------------------------------------

// hold_object / unhold_object: d3d11_child_refcount.cpp (reference counts).

template <typename F>
void D3D11Manager::visit_bound_objects(const ContextBindings& b, F&& f) {
    // Exactly the objects clear_context_state releases: a snapshot applied
    // to a live context is released by it.
    const auto each = [&](const auto& slots) {
        for (const GuestAddr address : slots) {
            if (address != 0) f(address);
        }
    };
    const auto one = [&](GuestAddr address) {
        if (address != 0) f(address);
    };
    // A stage bound with class instances holds the application's shader and
    // the instances (ContextBindings::linked), not its specialisation.
    const auto shader = [&](GuestAddr bound, LinkedStage stage) {
        const auto& linked = b.linked[static_cast<usize>(stage)];
        if (linked.shader == 0) {
            one(bound);
            return;
        }
        one(linked.shader);
        each(linked.instances);
    };
    shader(b.vertex_shader, LinkedStage::Vertex);
    shader(b.pixel_shader, LinkedStage::Pixel);
    shader(b.compute_shader, LinkedStage::Compute);
    shader(b.geometry_shader, LinkedStage::Geometry);
    shader(b.hull_shader, LinkedStage::Hull);
    shader(b.domain_shader, LinkedStage::Domain);
    each(b.vertex_constant_buffers);
    each(b.pixel_constant_buffers);
    each(b.compute_constant_buffers);
    each(b.geometry_constant_buffers);
    each(b.hull_constant_buffers);
    each(b.domain_constant_buffers);
    each(b.vertex_shader_resources);
    each(b.pixel_shader_resources);
    each(b.compute_shader_resources);
    each(b.geometry_shader_resources);
    each(b.hull_shader_resources);
    each(b.domain_shader_resources);
    each(b.vertex_samplers);
    each(b.pixel_samplers);
    each(b.compute_samplers);
    each(b.geometry_samplers);
    each(b.hull_samplers);
    each(b.domain_samplers);
    each(b.compute_unordered_access_views);
    each(b.output_merger_unordered_access_views);
    each(b.stream_out_targets);
    one(b.input_layout);
    one(b.blend_state);
    one(b.rasterizer_state);
    one(b.depth_stencil_state);
    each(b.vertex_buffers);
    one(b.index_buffer);
    each(b.render_targets);
    one(b.depth_stencil_view);
    one(b.predicate);
}

void D3D11Manager::hold_bindings(const ContextBindings& b) {
    visit_bound_objects(b, [this](GuestAddr address) {
        (void)hold_object(address);
    });
}

void D3D11Manager::unhold_bindings(const ContextBindings& b) {
    visit_bound_objects(b, [this](GuestAddr address) {
        unhold_object(address);
    });
}

void D3D11Manager::retain_bound_objects(const ContextBindings& b) {
    visit_bound_objects(b, [this](GuestAddr address) {
        (void)retain_object(address);
    });
}

void D3D11Manager::release_bound_objects(const ContextBindings& b) {
    visit_bound_objects(b, [this](GuestAddr address) {
        (void)release_object(address);
    });
}

// -----------------------------------------------------------------------------
// Recordings
// -----------------------------------------------------------------------------

std::unique_ptr<D3D11Manager::DeferredRecording> D3D11Manager::new_recording() {
    return std::make_unique<DeferredRecording>();
}

GuestAddr D3D11Manager::arena_allocate(GuestArena& arena, usize bytes) {
    bytes = std::max<usize>((bytes + 15u) & ~usize{15}, 16u);
    if (!arena.chunks.empty()) {
        auto& chunk = arena.chunks.back();
        if (chunk.size - chunk.used >= bytes) {
            const GuestAddr address = chunk.base + chunk.used;
            chunk.used += bytes;
            return address;
        }
    }
    GuestArena::Chunk chunk{};
    if (bytes <= kArenaChunkBytes && !m_arena_pool.empty()) {
        chunk.base = m_arena_pool.back();
        m_arena_pool.pop_back();
        chunk.size = kArenaChunkBytes;
    } else {
        chunk.size = std::max(kArenaChunkBytes,
                              (bytes + 0xFFFu) & ~usize{0xFFF});
        chunk.base = m_memory.map_first_fit(0x10000000ull, chunk.size,
                                            MemoryProtection::ReadWrite);
        if (chunk.base == 0) {
            d3d11_diag("deferred context: no guest memory for a %zu-byte "
                       "recording chunk", chunk.size);
            return 0;
        }
    }
    chunk.used = bytes;
    arena.chunks.push_back(chunk);
    return chunk.base;
}

void D3D11Manager::release_recording(DeferredRecording& recording) {
    // Move everything out first: dropping a reference can release a nested
    // command list, which releases its own recording.
    std::vector<RecordedCall> calls = std::move(recording.calls);
    recording.calls.clear();
    std::optional<ContextBindings> initial = std::move(recording.initial);
    recording.initial.reset();
    std::vector<GuestArena::Chunk> chunks = std::move(recording.arena.chunks);
    recording.arena.chunks.clear();
    recording.replay_frame = 0;
    for (const RecordedCall& call : calls) {
        for (const GuestAddr held : call.held) unhold_object(held);
    }
    if (initial) unhold_bindings(*initial);
    for (const auto& chunk : chunks) {
        if (chunk.size == kArenaChunkBytes &&
            m_arena_pool.size() < kPooledArenaChunks) {
            m_arena_pool.push_back(chunk.base);
        } else {
            (void)m_memory.unmap_region(chunk.base, chunk.size);
        }
    }
}

// -----------------------------------------------------------------------------
// Recording a call
// -----------------------------------------------------------------------------

bool D3D11Manager::deferred_context_method(Marshaler& m, usize slot,
                                           GuestAddr self) {
    auto found = m_deferred_contexts.find(self);
    if (found == m_deferred_contexts.end()) return false;
    DeferredContext& context = found->second;
    switch (slot) {
    case 0:
        m.set_return(deferred_query_interface(m, self));
        return true;
    case 1:
        m.set_return(deferred_add_ref(self));
        return true;
    case 2:
        m.set_return(deferred_release(self));
        return true;
    // Measured (a12 [pd] share): a deferred context's private data is its
    // own, not the device's or the immediate context's.
    case 4:
        m.set_return(get_private_data(m, context.private_data));
        return true;
    case 5:
        m.set_return(set_private_data(m, context.private_data));
        return true;
    case 6:
        m.set_return(set_private_data_interface(m, context.private_data));
        return true;
    case 29:
        // GetData: DXGI_ERROR_INVALID_CALL on a deferred context, whatever
        // the arguments, and pData is not written (measured).
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return true;
    case 111:  // Flush
    case 144:  // Flush1
        // Nothing of a deferred context's reaches the GPU before it executes.
        m.set_return(0);
        return true;
    case 112:
        m.set_return(1);  // D3D11_DEVICE_CONTEXT_DEFERRED
        return true;
    case 113:
        m.set_return(context.context_flags);
        return true;
    case 114:
        m.set_return(finish_command_list(m, self));
        return true;
    case 131: {
        // SwapDeviceContextState: a no-op on a deferred context that hands
        // back no previous state (measured: *ppPreviousState = NULL).
        const GuestAddr previous_out = m.arg_ptr(2);
        if (previous_out != 0) (void)m.write_ptr(previous_out, 0);
        m.set_return(0);
        return true;
    }
    case 147:   // Signal
    case 148:   // Wait
        // Learn: "This method only applies to immediate-mode contexts."
        // Measured: DXGI_ERROR_INVALID_CALL, and nothing is recorded.
        m.set_return(DXGI_ERROR_INVALID_CALL);
        return true;
    default:
        break;
    }

    DeferredRecording& recording = *context.recording;
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    RecordedCall call{};
    call.slot = static_cast<u16>(slot);
    const auto take = [&](u8 count) {
        call.arg_count = count;
        for (u8 i = 0; i < count; ++i) call.args[i] = m.arg(i + 1u);
        if (m_is_64bit && m.cpu() != nullptr) {
            for (usize i = 0; i < call.xmm.size(); ++i) {
                call.xmm[i] = m.cpu()->sse.xmm[i].u32_[0];
            }
        }
    };
    // Copy what argument @p arg (1-based) points at into the arena, and
    // point the recorded argument at the copy. An unreadable source keeps
    // the guest pointer, so the replay fails the way the call would have.
    const auto copy = [&](u8 arg, usize bytes) {
        const GuestAddr source = static_cast<GuestAddr>(call.args[arg - 1u]);
        if (source == 0 || bytes == 0) return;
        std::vector<u8> data(bytes);
        if (m_memory.read_bytes(source, data.data(), bytes).is_err()) return;
        const GuestAddr copy_address = arena_allocate(recording.arena, bytes);
        if (copy_address == 0 ||
            m_memory.write_bytes(copy_address, data.data(), bytes).is_err()) {
            return;
        }
        call.args[arg - 1u] = copy_address;
    };
    const auto hold = [&](GuestAddr address) {
        if (hold_object(address)) call.held.push_back(address);
    };
    const auto hold_arg = [&](u8 arg) {
        hold(static_cast<GuestAddr>(call.args[arg - 1u]));
    };
    const auto hold_array = [&](u8 arg, u32 count) {
        const GuestAddr base = static_cast<GuestAddr>(call.args[arg - 1u]);
        if (base == 0) return;
        for (u32 i = 0; i < count; ++i) {
            const auto value = m.read_ptr(base + i * pointer_size);
            if (value.is_ok()) hold(static_cast<GuestAddr>(value.value()));
        }
    };
    const auto count_of = [&](u8 arg) -> u32 {
        const u32 count = static_cast<u32>(call.args[arg - 1u]);
        return count <= kMaxRecordedArray ? count : 0u;
    };
    // true: the call only records. false: it also runs now against the
    // deferred context's own bindings (the state setters).
    bool record_only = true;
    switch (slot) {
    // --- State setters --------------------------------------------------
    case 7: case 16: case 22: case 62: case 66: case 71:   // *SetConstantBuffers
    case 8: case 25: case 31: case 59: case 63: case 67:   // *SetShaderResources
    case 10: case 26: case 32: case 61: case 65: case 70: {  // *SetSamplers
        take(3);
        const u32 count = count_of(2);
        copy(3, count * pointer_size);
        hold_array(3, count);
        record_only = false;
        break;
    }
    case 119: case 120: case 121: case 122: case 123: case 124: {
        // *SetConstantBuffers1(Start, Num, pp, pFirstConstant, pNumConstants)
        take(5);
        const u32 count = count_of(2);
        copy(3, count * pointer_size);
        copy(4, count * sizeof(u32));
        copy(5, count * sizeof(u32));
        hold_array(3, count);
        record_only = false;
        break;
    }
    case 9: case 11: case 23: case 60: case 64: case 69: {
        // *SetShader(pShader, ppClassInstances, NumClassInstances)
        take(3);
        hold_arg(1);
        const u32 count = count_of(3);
        copy(2, count * pointer_size);
        hold_array(2, count);
        record_only = false;
        break;
    }
    case 17:   // IASetInputLayout
    case 43:   // RSSetState
        take(1);
        hold_arg(1);
        record_only = false;
        break;
    case 18: {  // IASetVertexBuffers(Start, Num, pp, pStrides, pOffsets)
        take(5);
        const u32 count = count_of(2);
        copy(3, count * pointer_size);
        copy(4, count * sizeof(u32));
        copy(5, count * sizeof(u32));
        hold_array(3, count);
        record_only = false;
        break;
    }
    case 19:   // IASetIndexBuffer(p, Format, Offset)
        take(3);
        hold_arg(1);
        record_only = false;
        break;
    case 24:   // IASetPrimitiveTopology
        take(1);
        record_only = false;
        break;
    case 30:   // SetPredication(pPredicate, PredicateValue)
    case 36:   // OMSetDepthStencilState(p, StencilRef)
        take(2);
        hold_arg(1);
        record_only = false;
        break;
    case 33: {  // OMSetRenderTargets(Num, pp, pDSV)
        take(3);
        const u32 count = count_of(1);
        copy(2, count * pointer_size);
        hold_array(2, count);
        hold_arg(3);
        record_only = false;
        break;
    }
    case 34: {
        // OMSetRenderTargetsAndUnorderedAccessViews(NumRTVs, ppRTV, pDSV,
        // UAVStartSlot, NumUAVs, ppUAV, pUAVInitialCounts)
        take(7);
        const bool keep_rtvs =
            static_cast<u32>(call.args[0]) == kKeepBindings;
        const bool keep_uavs =
            static_cast<u32>(call.args[4]) == kKeepBindings;
        const u32 rtvs = keep_rtvs ? 0u : count_of(1);
        const u32 uavs = keep_uavs ? 0u : count_of(5);
        copy(2, rtvs * pointer_size);
        hold_array(2, rtvs);
        if (!keep_rtvs) hold_arg(3);
        copy(6, uavs * pointer_size);
        hold_array(6, uavs);
        copy(7, uavs * sizeof(u32));
        record_only = false;
        break;
    }
    case 35:   // OMSetBlendState(p, BlendFactor[4], SampleMask)
        take(3);
        hold_arg(1);
        copy(2, 4 * sizeof(float));
        record_only = false;
        break;
    case 37: {  // SOSetTargets(Num, pp, pOffsets)
        take(3);
        const u32 count = count_of(1);
        copy(2, count * pointer_size);
        hold_array(2, count);
        copy(3, count * sizeof(u32));
        record_only = false;
        break;
    }
    case 44:   // RSSetViewports(Num, D3D11_VIEWPORT*): 6 floats each
        take(2);
        copy(2, count_of(1) * 24u);
        record_only = false;
        break;
    case 45:   // RSSetScissorRects(Num, D3D11_RECT*)
        take(2);
        copy(2, count_of(1) * 16u);
        record_only = false;
        break;
    case 68: {  // CSSetUnorderedAccessViews(Start, Num, pp, pInitialCounts)
        take(4);
        const u32 count = count_of(2);
        copy(3, count * pointer_size);
        hold_array(3, count);
        copy(4, count * sizeof(u32));
        record_only = false;
        break;
    }
    case 110:  // ClearState
        take(0);
        record_only = false;
        break;
    case 58: {
        // ExecuteCommandList on a deferred context records the list; the
        // context's own state then goes where the executing context's would
        // (Learn: RestoreContextState FALSE leaves the default state).
        take(2);
        if (!m_command_lists.contains(static_cast<GuestAddr>(call.args[0]))) {
            m.set_return(0);
            return true;
        }
        hold_arg(1);
        recording.calls.push_back(std::move(call));
        if (static_cast<u32>(recording.calls.back().args[1]) == 0) {
            clear_context_state(self);
        }
        m.set_return(0);
        return true;
    }

    // --- Commands: recorded, nothing happens now ---------------------------
    case 12: take(3); break;   // DrawIndexed
    case 13: take(2); break;   // Draw
    case 20: take(5); break;   // DrawIndexedInstanced
    case 21: take(4); break;   // DrawInstanced
    case 38: take(0); break;   // DrawAuto
    case 41: take(3); break;   // Dispatch
    case 39:   // DrawIndexedInstancedIndirect(pArgs, Offset)
    case 40:   // DrawInstancedIndirect
    case 42:   // DispatchIndirect
        take(2);
        hold_arg(1);
        break;
    case 27:   // Begin
    case 28:   // End
    case 54:   // GenerateMips(pSRV)
    case 117:  // DiscardResource
    case 118:  // DiscardView
        take(1);
        hold_arg(1);
        break;
    case 46:    // CopySubresourceRegion(pDst, Sub, X, Y, Z, pSrc, Sub, pBox)
    case 115:   // CopySubresourceRegion1(..., CopyFlags)
        take(slot == 46 ? 8 : 9);
        hold_arg(1);
        hold_arg(6);
        copy(8, 6 * sizeof(u32));
        break;
    case 47:   // CopyResource(pDst, pSrc)
        take(2);
        hold_arg(1);
        hold_arg(2);
        break;
    case 48:    // UpdateSubresource
    case 116:   // UpdateSubresource1
        record_update_subresource(m, slot, recording, call);
        break;
    case 49:   // CopyStructureCount(pDstBuffer, Offset, pSrcView)
        take(3);
        hold_arg(1);
        hold_arg(3);
        break;
    case 50:   // ClearRenderTargetView(pRTV, ColorRGBA[4])
    case 51:   // ClearUnorderedAccessViewUint(pUAV, Values[4])
    case 52:   // ClearUnorderedAccessViewFloat
        take(2);
        hold_arg(1);
        copy(2, 16);
        break;
    case 53:   // ClearDepthStencilView(pDSV, Flags, FLOAT Depth, UINT8 Stencil)
        take(4);
        hold_arg(1);
        break;
    case 55:   // SetResourceMinLOD(pResource, FLOAT MinLOD)
        take(2);
        hold_arg(1);
        break;
    case 57:   // ResolveSubresource(pDst, Sub, pSrc, Sub, Format)
        take(5);
        hold_arg(1);
        hold_arg(3);
        break;
    case 132: {  // ClearView(pView, Color[4], pRects, NumRects)
        take(4);
        hold_arg(1);
        copy(2, 16);
        copy(3, count_of(4) * 16u);
        break;
    }
    case 133:  // DiscardView1(pView, pRects, NumRects)
        take(3);
        hold_arg(1);
        copy(2, count_of(3) * 16u);
        break;

    case 14:
        m.set_return(deferred_map(m, self, context));
        return true;
    case 15: {
        // Unmap. The upload was recorded by the Map and reads the block
        // when the list executes, so this only ends the mapping.
        const auto key = std::make_pair(m.arg_ptr(1),
                                        static_cast<u32>(m.arg(2)));
        auto mapping = context.mappings.find(key);
        if (mapping != context.mappings.end()) mapping->second.mapped = false;
        m.set_return(0);
        return true;
    }
    // --- Tiled resources (d3d11_tiled.cpp) ----------------------------------
    // Measured (d3d11_tiled_probe [data] D): on a deferred context the
    // runtime's command lists (DriverCommandLists FALSE) refuse
    // UpdateTileMappings and CopyTileMappings with E_INVALIDARG whatever the
    // arguments, record nothing; ResizeTilePool answers S_OK and resizes when
    // the list executes; UpdateTiles, CopyTiles and TiledResourceBarrier
    // take effect when it executes.
    case 134:  // UpdateTileMappings
    case 135:  // CopyTileMappings
        m.set_return(d3d11::E_INVALIDARG);
        return true;
    case 136: {  // CopyTiles(pRes, pCoord, pSize, pBuffer, UINT64, Flags)
        take(m_is_64bit ? 6 : 7);
        hold_arg(1);
        hold_arg(4);
        copy(2, 16);
        copy(3, 16);
        break;
    }
    case 137: {  // UpdateTiles(pRes, pCoord, pSize, pSourceTileData, Flags)
        take(5);
        hold_arg(1);
        // Every tile of the region is read from pSourceTileData: copied now,
        // like UpdateSubresource's source.
        u64 tiles = 0;
        const GuestAddr size_address = static_cast<GuestAddr>(call.args[2]);
        if (size_address != 0) {
            const auto count = m_memory.read_u32(size_address);
            const auto box = m_memory.read_u32(size_address + 4);
            const auto width = m_memory.read_u32(size_address + 8);
            const auto extents = m_memory.read_u32(size_address + 12);
            if (count.is_ok() && box.is_ok() && width.is_ok() &&
                extents.is_ok()) {
                tiles = box.value() != 0
                            ? static_cast<u64>(width.value()) *
                                  (extents.value() & 0xFFFFu) *
                                  (extents.value() >> 16)
                            : count.value();
            }
        }
        copy(2, 16);
        copy(3, 16);
        if (tiles != 0 && tiles <= (u64{1} << 12)) {
            copy(4, static_cast<usize>(tiles * graphics::BACKEND_TILE_BYTES));
        }
        break;
    }
    case 138: {  // ResizeTilePool(pTilePool, UINT64 NewSizeInBytes)
        take(m_is_64bit ? 2 : 3);
        const u64 bytes = arg_u64(m, 2);
        auto pool = m_buffers.find(static_cast<GuestAddr>(call.args[0]));
        if (pool == m_buffers.end() || !pool->second.tile_pool ||
            bytes % graphics::BACKEND_TILE_BYTES != 0 ||
            bytes > 0xFFFFFFFFull) {
            m.set_return(d3d11::E_INVALIDARG);
            return true;
        }
        hold_arg(1);
        break;
    }
    case 139:  // TiledResourceBarrier(pBefore, pAfter)
        take(2);
        hold_arg(1);
        hold_arg(2);
        break;

    default:
        // Getters and annotation markers answer from this context.
        return false;
    }
    recording.calls.push_back(std::move(call));
    if (record_only) {
        m.set_return(0);
        return true;
    }
    return false;
}

void D3D11Manager::record_update_subresource(Marshaler& m, usize slot,
                                             DeferredRecording& recording,
                                             RecordedCall& call) {
    // UpdateSubresource(pDst, Sub, pDstBox, pSrcData, SrcRowPitch,
    // SrcDepthPitch[, CopyFlags]). Learn: "When UpdateSubresource returns,
    // the application is free to change or even free the data pointed to by
    // pSrcData", so the source is copied now.
    call.arg_count = slot == 48 ? 6 : 7;
    for (u8 i = 0; i < call.arg_count; ++i) call.args[i] = m.arg(i + 1u);
    const GuestAddr resource = static_cast<GuestAddr>(call.args[0]);
    const u32 subresource = static_cast<u32>(call.args[1]);
    const GuestAddr box_address = static_cast<GuestAddr>(call.args[2]);
    GuestAddr source = static_cast<GuestAddr>(call.args[3]);
    const u32 row_pitch = static_cast<u32>(call.args[4]);
    const u32 depth_pitch = static_cast<u32>(call.args[5]);
    if (hold_object(resource)) call.held.push_back(resource);

    std::array<u32, 6> box{};
    bool have_box = box_address != 0;
    for (usize i = 0; have_box && i < box.size(); ++i) {
        const auto field = m_memory.read_u32(box_address + i * sizeof(u32));
        if (field.is_err()) have_box = false;
        else box[i] = field.value();
    }
    if (have_box) {
        const GuestAddr copy_address =
            arena_allocate(recording.arena, sizeof(box));
        if (copy_address != 0 &&
            m_memory.write_bytes(copy_address,
                                 reinterpret_cast<const u8*>(box.data()),
                                 sizeof(box)).is_ok()) {
            call.args[2] = copy_address;
        }
    }
    if (source == 0) return;

    // The bytes the method reads from pSrcData, and -- Learn (UpdateSubresource,
    // "Calling UpdateSubresource on a Deferred Context") -- where they start.
    // Without driver command lists (Vitreo reports DriverCommandLists FALSE,
    // d3d11::kDriverCommandLists) the runtime "inappropriately applies that
    // destination-box offset to the pSrcData parameter": front * SrcDepthPitch
    // + top * SrcRowPitch + left * element size, in blocks for a
    // block-compressed format. Apps detect this through FEATURE_THREADING and
    // compensate, so it must happen. Measured: UpdateSubresource does it (a
    // buffer box at byte 8 reads pSrcData + 8; a texture box at (1,1) with a
    // 64-byte pitch reads pSrcData + 68), UpdateSubresource1 does not.
    const bool offset_quirk = slot == 48 && have_box && !d3d11::kDriverCommandLists;
    usize extent = 0;
    usize quirk_offset = 0;
    if (auto buffer = m_buffers.find(resource); buffer != m_buffers.end()) {
        const u32 width = buffer->second.desc.byte_width;
        if (have_box) {
            extent = box[3] > box[0] ? std::min(box[3], width) - std::min(box[0], box[3]) : 0u;
            quirk_offset = static_cast<usize>(box[0]) +
                           static_cast<usize>(box[1]) * row_pitch +
                           static_cast<usize>(box[2]) * depth_pitch;
        } else {
            extent = width;
        }
    } else if (auto texture = m_textures.find(resource);
               texture != m_textures.end()) {
        // A DEFAULT texture keeps no CPU layout, so the subresource's extent
        // comes from the description, as update_texture_box computes it.
        const Texture2D& t = texture->second;
        const bool volume = t.volume;
        const u32 dxgi_format = volume ? t.volume_desc.format : t.desc.format;
        const u32 mips = volume ? t.volume_desc.mip_levels : t.desc.mip_levels;
        const u32 count = volume ? mips : mips * t.desc.array_size;
        const u32 mip = mips != 0 ? subresource % mips : 0u;
        const auto format = graphics::format_table(backend_format(dxgi_format));
        const u32 left = have_box ? box[0] : 0u;
        const u32 top = have_box ? box[1] : 0u;
        const u32 front = have_box ? box[2] : 0u;
        const u32 right = have_box ? box[3]
            : std::max(1u, (volume ? t.volume_desc.width : t.desc.width) >> mip);
        const u32 bottom = have_box ? box[4]
            : std::max(1u, (volume ? t.volume_desc.height : t.desc.height) >> mip);
        const u32 back = have_box ? box[5]
            : volume ? std::max(1u, t.volume_desc.depth >> mip) : 1u;
        if (subresource < count && right > left && bottom > top &&
            back > front) {
            const u32 block = format.is_compressed
                ? graphics::compressed_block_bytes(backend_format(dxgi_format))
                : 0u;
            // The guest layout's element (a packed 4:2:2 pixel is half a
            // host texel, staging_bytes_per_pixel).
            const usize element =
                format.is_compressed
                    ? block
                    : staging_bytes_per_pixel(dxgi_format,
                                              format.bytes_per_pixel);
            const usize rows = format.is_compressed ? (bottom - top + 3u) / 4u
                                                    : dxgi_subresource_rows(
                                                          dxgi_format,
                                                          bottom - top);
            const usize row_bytes =
                (format.is_compressed ? (right - left + 3u) / 4u
                                      : right - left) * element;
            const usize pitch = row_pitch != 0 ? row_pitch : row_bytes;
            const usize plane = depth_pitch != 0 ? depth_pitch : pitch * rows;
            extent = static_cast<usize>(back - front - 1u) * plane +
                     (rows - 1u) * pitch + row_bytes;
            quirk_offset =
                static_cast<usize>(front) * depth_pitch +
                static_cast<usize>(format.is_compressed ? top / 4u : top) *
                    row_pitch +
                static_cast<usize>(format.is_compressed ? left / 4u : left) *
                    element;
        }
    }
    if (offset_quirk) {
        source += quirk_offset;
        call.args[3] = source;
    }
    if (extent == 0) return;
    std::vector<u8> data(extent);
    if (m_memory.read_bytes(source, data.data(), extent).is_err()) return;
    const GuestAddr copy_address = arena_allocate(recording.arena, extent);
    if (copy_address == 0 ||
        m_memory.write_bytes(copy_address, data.data(), extent).is_err()) {
        return;
    }
    call.args[3] = copy_address;
}

u32 D3D11Manager::deferred_map(Marshaler& m, GuestAddr self,
                               DeferredContext& context) {
    // Map(pResource, Subresource, MapType, MapFlags, pMappedResource).
    // Learn (Map): on a deferred context "you can only pass
    // D3D11_MAP_WRITE_DISCARD, D3D11_MAP_WRITE_NO_OVERWRITE, or both".
    // Measured (d3d11_deferred_rules_probe [map], a12 [def]):
    //   * any other type, any MapFlags (DO_NOT_WAIT too), a NULL
    //     pMappedResource, a subresource out of range, and every non-DYNAMIC
    //     resource: E_INVALIDARG;
    //   * WRITE_NO_OVERWRITE before a WRITE_DISCARD of the resource in this
    //     recording, and any WRITE_NO_OVERWRITE of a texture:
    //     D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD; the
    //     requirement starts over at FinishCommandList;
    //   * a failure writes pData = NULL.
    // A WRITE_DISCARD hands out fresh memory owned by the recording and
    // records the upload; NO_OVERWRITE hands out the same memory again. The
    // upload copies the block when the list EXECUTES, so every recorded use
    // sees what the application wrote into it (measured: a copy recorded
    // between the DISCARD and a NO_OVERWRITE write sees that write), and the
    // immediate context sees nothing until then.
    const GuestAddr resource = m.arg_ptr(1);
    const u32 subresource = static_cast<u32>(m.arg(2));
    const u32 map_type = static_cast<u32>(m.arg(3));
    const u32 map_flags = static_cast<u32>(m.arg(4));
    const GuestAddr mapped_out = m.arg_ptr(5);
    const auto fail = [&](u32 hr) {
        if (mapped_out != 0) (void)m.write_ptr(mapped_out, 0);
        return hr;
    };
    const GuestAddr owner = m_interfaces.find(self)->second.owner;
    auto buffer = m_buffers.find(resource);
    auto texture = m_textures.find(resource);
    const bool is_buffer = buffer != m_buffers.end();
    const bool is_texture = !is_buffer && texture != m_textures.end();
    if (mapped_out == 0 || (!is_buffer && !is_texture) || map_flags != 0 ||
        (map_type != D3D11_MAP_WRITE_DISCARD &&
         map_type != D3D11_MAP_WRITE_NO_OVERWRITE)) {
        return fail(d3d11::E_INVALIDARG);
    }
    usize bytes = 0;
    u32 row_pitch = 0;
    u32 depth_pitch = 0;
    if (is_buffer) {
        const auto& desc = buffer->second.desc;
        if (buffer->second.device_owner != owner ||
            desc.usage != D3D11_USAGE_DYNAMIC ||
            (desc.cpu_access_flags & D3D11_CPU_ACCESS_WRITE) == 0 ||
            subresource != 0) {
            return fail(d3d11::E_INVALIDARG);
        }
        // Measured: a deferred buffer Map reports both pitches as 0.
        bytes = desc.byte_width;
    } else {
        const auto& desc = texture->second.desc;
        if (texture->second.device_owner != owner ||
            desc.usage != D3D11_USAGE_DYNAMIC ||
            (desc.cpu_access_flags & D3D11_CPU_ACCESS_WRITE) == 0 ||
            subresource >= texture->second.subresources.size()) {
            return fail(d3d11::E_INVALIDARG);
        }
        if (map_type == D3D11_MAP_WRITE_NO_OVERWRITE) {
            return fail(kMapWithoutInitialDiscard);
        }
        // The pitches of the subresource's layout, which the replayed Map
        // hands back too. (WARP measured 0 for both here, as for a buffer;
        // a texture written with a 0 row pitch would lose every row but the
        // first, so the real layout is reported.) A texture created with
        // 64K_STANDARD_SWIZZLE maps swizzled on the immediate context, so
        // the recorded block is that image and the replay copies it as is.
        const auto layout = mapped_image_layout(texture->second, subresource);
        if (layout.kind == MappedImageLayout::Kind::None) {
            return fail(d3d11::E_INVALIDARG);
        }
        bytes = layout.size;
        row_pitch = layout.row_pitch;
        depth_pitch = layout.depth_pitch;
    }
    const auto key = std::make_pair(resource, subresource);
    auto mapping = context.mappings.find(key);
    GuestAddr block = 0;
    if (map_type == D3D11_MAP_WRITE_NO_OVERWRITE) {
        if (mapping == context.mappings.end()) {
            return fail(kMapWithoutInitialDiscard);
        }
        block = mapping->second.block;
    } else {
        DeferredRecording& recording = *context.recording;
        block = arena_allocate(recording.arena, bytes);
        if (block == 0) return fail(d3d11::E_OUTOFMEMORY);
        RecordedCall upload{};
        upload.slot = kUploadMappedBlock;
        upload.arg_count = 4;
        upload.args = {resource, subresource, block, bytes};
        if (hold_object(resource)) upload.held.push_back(resource);
        recording.calls.push_back(std::move(upload));
    }
    context.mappings[key] = DeferredContext::Mapping{block, true};
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    if (m.write_ptr(mapped_out, block).is_err() ||
        m_memory.write_u32(mapped_out + pointer_size, row_pitch).is_err() ||
        m_memory.write_u32(mapped_out + pointer_size + 4u, depth_pitch)
            .is_err()) {
        return d3d11::E_INVALIDARG;
    }
    return d3d11::S_OK;
}

// -----------------------------------------------------------------------------
// FinishCommandList / ExecuteCommandList
// -----------------------------------------------------------------------------

u32 D3D11Manager::finish_command_list(Marshaler& m, GuestAddr self) {
    // FinishCommandList(RestoreDeferredContextState, ppCommandList).
    const u32 restore_state = m.arg32(1);
    const GuestAddr list_out = m.arg_ptr(2);
    // Measured: a NULL ppCommandList is E_POINTER.
    if (list_out == 0) return d3d11::E_POINTER;
    (void)m.write_ptr(list_out, 0);
    auto found = m_deferred_contexts.find(self);
    if (found == m_deferred_contexts.end()) return DXGI_ERROR_INVALID_CALL;
    DeferredContext& context = found->second;
    const GuestAddr frame =
        arena_allocate(context.recording->arena, kReplayFrameBytes);
    const GuestAddr address =
        frame != 0 ? allocate_object(m_command_list_vtable) : 0;
    if (address == 0) return d3d11::E_OUTOFMEMORY;
    context.recording->replay_frame = frame;
    CommandList list{};
    list.interface_address = address;
    list.device_owner = context.device_owner;
    list.context_flags = context.context_flags;
    list.recording = std::move(context.recording);
    const usize calls = list.recording->calls.size();
    m_command_lists.emplace(address, std::move(list));
    const auto device = m_devices.find(context.device_owner);
    if (device != m_devices.end()) {
        (void)add_ref(device->second.device_interface);
    }
    // The next list starts over: no WRITE_DISCARD has happened in it yet.
    context.mappings.clear();
    context.recording = new_recording();
    if (restore_state == 0) {
        // Learn: FALSE returns the deferred context to its default state.
        clear_context_state(self);
    } else {
        // TRUE keeps it -- and the next list starts from it.
        const ContextBindings& now = m_context_states[self];
        context.recording->initial = now;
        hold_bindings(now);
    }
    if (m.write_ptr(list_out, address).is_err()) {
        (void)release_object(address);
        return d3d11::E_POINTER;
    }
    d3d11_diag("ID3D11DeviceContext::FinishCommandList -> 0x%llx (%zu calls, "
               "restore=%u)",
               static_cast<unsigned long long>(address), calls, restore_state);
    return d3d11::S_OK;
}

u64 D3D11Manager::replay_call(GuestAddr context, const RecordedCall& call,
                              GuestAddr frame) {
    // Rebuild the guest call frame the thunk would have seen and dispatch it.
    cpu::CpuState cpu{};
    if (m_is_64bit) {
        cpu.init_long_mode();
        cpu.gpr.rsp = frame;
        cpu.gpr.rcx = context;
        const std::array<u64*, 3> registers{&cpu.gpr.rdx, &cpu.gpr.r8,
                                            &cpu.gpr.r9};
        for (u8 i = 0; i < call.arg_count; ++i) {
            if (i < registers.size()) {
                *registers[i] = call.args[i];
            } else {
                // Marshaler::arg: argument n >= 4 lives at RSP + 0x28 + 8(n-4).
                (void)m_memory.write_u64(frame + 0x28 + (i - 3u) * 8u,
                                         call.args[i]);
            }
        }
        for (usize i = 0; i < call.xmm.size(); ++i) {
            cpu.sse.xmm[i].u32_[0] = call.xmm[i];
        }
    } else {
        cpu.init_protected_mode();
        cpu.gpr.rsp = frame;
        // stdcall: argument n at ESP + 4 + 4n.
        (void)m_memory.write_u32(frame + 4, static_cast<u32>(context));
        for (u8 i = 0; i < call.arg_count; ++i) {
            (void)m_memory.write_u32(frame + 8 + i * 4u,
                                     static_cast<u32>(call.args[i]));
        }
    }
    Marshaler m(m_memory);
    m.attach_cpu(cpu);
    impl_ContextMethod(m, call.slot);
    return cpu.gpr.rax;
}

void D3D11Manager::execute_command_list(GuestAddr context,
                                        GuestAddr list_address,
                                        bool restore_state) {
    auto list = m_command_lists.find(list_address);
    const auto interface = m_interfaces.find(context);
    if (list == m_command_lists.end() || !list->second.recording ||
        interface == m_interfaces.end() ||
        list->second.device_owner != interface->second.owner) {
        d3d11_diag("ID3D11DeviceContext::ExecuteCommandList 0x%llx is not a "
                   "live command list of this device",
                   static_cast<unsigned long long>(list_address));
        return;
    }
    // The list must outlive its own replay (a replayed call may release the
    // last outside reference to it).
    (void)hold_object(list_address);
    const DeferredRecording& recording = *list->second.recording;

    // Learn: "the immediate context state is cleared before and after a
    // command list is executed" unless RestoreContextState keeps it. The
    // context's caches and its open queries are not state: they go along.
    ContextState outer = std::move(m_context_states[context]);
    ContextState run{};
    run.pipelines = std::move(outer.pipelines);
    run.descriptor_tables = std::move(outer.descriptor_tables);
    run.counting_queries = std::move(outer.counting_queries);
    run.occlusion_interval = std::move(outer.occlusion_interval);
    if (recording.initial) {
        static_cast<ContextBindings&>(run) = *recording.initial;
        hold_bindings(*recording.initial);
    }
    m_context_states[context] = std::move(run);
    if (!restore_state) release_bindings(std::move(outer));

    for (const RecordedCall& call : recording.calls) {
        if (call.slot == kUploadMappedBlock) {
            replay_mapped_upload(context, call, recording.replay_frame);
        } else if (call.slot == kDebugMarker) {
            std::string label(static_cast<usize>(call.args[2]), '\0');
            if (!label.empty() &&
                m_memory.read_bytes(static_cast<GuestAddr>(call.args[1]),
                                    reinterpret_cast<Byte*>(label.data()),
                                    label.size()).is_err()) {
                label.clear();
            }
            emit_debug_marker(static_cast<DebugMarkerKind>(call.args[0]),
                              label);
        } else {
            (void)replay_call(context, call, recording.replay_frame);
        }
    }

    ContextState done = std::move(m_context_states[context]);
    ContextState after = restore_state ? std::move(outer) : ContextState{};
    after.pipelines = std::move(done.pipelines);
    after.descriptor_tables = std::move(done.descriptor_tables);
    after.counting_queries = std::move(done.counting_queries);
    after.occlusion_interval = std::move(done.occlusion_interval);
    m_context_states[context] = std::move(after);
    release_bindings(std::move(done));
    d3d11_diag("ID3D11DeviceContext::ExecuteCommandList 0x%llx: %zu calls "
               "replayed (restore=%d)",
               static_cast<unsigned long long>(list_address),
               recording.calls.size(), restore_state ? 1 : 0);
    unhold_object(list_address);
}

void D3D11Manager::replay_mapped_upload(GuestAddr context,
                                        const RecordedCall& upload,
                                        GuestAddr frame) {
    // Map(WRITE_DISCARD) on the executing context, the block's bytes into
    // what it hands out, Unmap: the immediate context's own upload path,
    // renaming and all.
    const GuestAddr resource = static_cast<GuestAddr>(upload.args[0]);
    const u32 subresource = static_cast<u32>(upload.args[1]);
    const GuestAddr block = static_cast<GuestAddr>(upload.args[2]);
    const usize bytes = static_cast<usize>(upload.args[3]);
    const GuestAddr mapped = frame + kReplayMappedOffset;
    RecordedCall map{};
    map.slot = 14;
    map.arg_count = 5;
    map.args = {resource, subresource, D3D11_MAP_WRITE_DISCARD, 0, mapped};
    if (static_cast<u32>(replay_call(context, map, frame)) != d3d11::S_OK) {
        d3d11_diag("ExecuteCommandList: the recorded Map of 0x%llx could not "
                   "be replayed",
                   static_cast<unsigned long long>(resource));
        return;
    }
    GuestAddr destination = 0;
    if (m_is_64bit) {
        const auto value = m_memory.read_u64(mapped);
        if (value.is_ok()) destination = static_cast<GuestAddr>(value.value());
    } else {
        const auto value = m_memory.read_u32(mapped);
        if (value.is_ok()) destination = value.value();
    }
    if (destination != 0) {
        std::vector<u8> data(bytes);
        if (m_memory.read_bytes(block, data.data(), bytes).is_ok()) {
            (void)m_memory.write_bytes(destination, data.data(), bytes);
        }
    }
    RecordedCall unmap{};
    unmap.slot = 15;
    unmap.arg_count = 2;
    unmap.args = {resource, subresource};
    (void)replay_call(context, unmap, frame);
}

}  // namespace vitreo::winapi

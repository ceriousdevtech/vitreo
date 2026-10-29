// SPDX-License-Identifier: LGPL-2.1-only

/// @file d3d12_command_list.cpp
/// @brief ID3D12CommandAllocator and ID3D12GraphicsCommandList recording.
///
/// A command list records a frontend command stream (Recording); nothing
/// reaches the backend until ExecuteCommandLists replays it
/// (d3d12_replay.cpp). That is what lets descriptor tables be resolved at
/// execution, as D3D12 requires: an application may rewrite a descriptor after
/// recording the draw that reads it and before executing the list (PLAN
/// decision 6). The CPU descriptors a method takes BY HANDLE -- render
/// targets, depth-stencil, clears -- are read when the method is called, as
/// Learn says they are ("the descriptor ... can be reused immediately").
///
/// Recording methods return void. One that cannot be honoured records the
/// error Close then returns (Learn, Close: E_INVALIDARG for an invalid
/// argument, E_FAIL for an invalid call), and such a list can never be reset
/// or executed.

#include "winapi/graphics/d3d12/d3d12_internal.hpp"
#include "core/memory/guest_memory.hpp"

#include <algorithm>
#include <cstring>

namespace vitreo::winapi::d3d12 {
namespace {

[[nodiscard]] bool list_type_supported(u32 type) {
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT ||
           type == D3D12_COMMAND_LIST_TYPE_BUNDLE ||
           type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
           type == D3D12_COMMAND_LIST_TYPE_COPY;
}

/// Every D3D12_RESOURCE_STATES bit d3d12.h defines.
constexpr u32 kDefinedStates =
    0x3FFFu | D3D12_RESOURCE_STATE_VIDEO_DECODE_READ |
    D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE |
    D3D12_RESOURCE_STATE_VIDEO_PROCESS_READ |
    D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE |
    D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ |
    D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE |
    D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE |
    D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
constexpr u32 kWriteStateBits =
    D3D12_RESOURCE_STATE_RENDER_TARGET | D3D12_RESOURCE_STATE_UNORDERED_ACCESS |
    D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_STREAM_OUT |
    D3D12_RESOURCE_STATE_COPY_DEST | D3D12_RESOURCE_STATE_RESOLVE_DEST |
    D3D12_RESOURCE_STATE_VIDEO_DECODE_WRITE |
    D3D12_RESOURCE_STATE_VIDEO_PROCESS_WRITE |
    D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;

/// The runtime's checks of a transition barrier (Learn, "Using resource
/// barriers"; measured, a04 close_rules): nullptr when valid.
const char* transition_violation(const Resource& r, u32 subresource, u32 before,
                                 u32 after, u32 list_type) {
    if (subresource != D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
        subresource >= r.subresource_count()) {
        return "subresource out of range";
    }
    if (before == after) return "a transition to the same state";
    for (const u32 state : {before, after}) {
        if ((state & ~kDefinedStates) != 0) return "undefined state bits";
        const u32 writes = state & kWriteStateBits;
        // A write state stands alone: with no read state and no other write
        // state (read states combine; measured, a04 "COPY_DEST|
        // UNORDERED_ACCESS (write|write)": E_INVALIDARG at Close).
        if (writes != 0 && (writes != state || (writes & (writes - 1)) != 0)) {
            return "a write state combined";
        }
    }
    // Upload and readback resources keep their one state.
    if (r.heap_properties.type == D3D12_HEAP_TYPE_UPLOAD &&
        (after & ~D3D12_RESOURCE_STATE_GENERIC_READ) != 0) {
        return "an UPLOAD resource leaving GENERIC_READ";
    }
    if (r.heap_properties.type == D3D12_HEAP_TYPE_READBACK &&
        (after & ~(D3D12_RESOURCE_STATE_COPY_DEST |
                   D3D12_RESOURCE_STATE_RESOLVE_DEST)) != 0) {
        return "a READBACK resource leaving COPY_DEST";
    }
    // A state needs its usage flag.
    const u32 flags = r.desc.flags;
    if ((after & D3D12_RESOURCE_STATE_RENDER_TARGET) != 0 &&
        (r.is_buffer() || (flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)) {
        return "RENDER_TARGET without ALLOW_RENDER_TARGET";
    }
    if ((after & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) != 0 &&
        (flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
        return "UNORDERED_ACCESS without ALLOW_UNORDERED_ACCESS";
    }
    if ((after & (D3D12_RESOURCE_STATE_DEPTH_WRITE |
                  D3D12_RESOURCE_STATE_DEPTH_READ)) != 0 &&
        (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) == 0) {
        return "a depth state without ALLOW_DEPTH_STENCIL";
    }
    // Learn (command list types): a copy list knows copy states only, a
    // compute list no graphics-only state.
    const u32 both = before | after;
    if (list_type == D3D12_COMMAND_LIST_TYPE_COPY &&
        (both & ~(D3D12_RESOURCE_STATE_COPY_DEST |
                  D3D12_RESOURCE_STATE_COPY_SOURCE)) != 0) {
        return "a non-copy state on a copy list";
    }
    if (list_type == D3D12_COMMAND_LIST_TYPE_COMPUTE &&
        // (VERTEX_AND_CONSTANT_BUFFER is a compute state: constant buffers.)
        (both & (D3D12_RESOURCE_STATE_INDEX_BUFFER |
                 D3D12_RESOURCE_STATE_RENDER_TARGET |
                 D3D12_RESOURCE_STATE_DEPTH_WRITE |
                 D3D12_RESOURCE_STATE_DEPTH_READ |
                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                 D3D12_RESOURCE_STATE_STREAM_OUT |
                 D3D12_RESOURCE_STATE_RESOLVE_DEST |
                 D3D12_RESOURCE_STATE_RESOLVE_SOURCE)) != 0) {
        return "a graphics state on a compute list";
    }
    return nullptr;
}

/// Learn (D3D12_QUERY_HEAP_TYPE / D3D12_QUERY_TYPE): the query types each
/// heap type holds.
[[nodiscard]] bool query_type_fits(u32 heap_type, u32 type) {
    switch (heap_type) {
    case D3D12_QUERY_HEAP_TYPE_OCCLUSION:
        return type == D3D12_QUERY_TYPE_OCCLUSION ||
               type == D3D12_QUERY_TYPE_BINARY_OCCLUSION;
    case D3D12_QUERY_HEAP_TYPE_TIMESTAMP:
    case D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP:
        return type == D3D12_QUERY_TYPE_TIMESTAMP;
    case D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS:
        return type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS;
    case D3D12_QUERY_HEAP_TYPE_SO_STATISTICS:
        return type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 &&
               type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3;
    default:
        return false;
    }
}

/// Bytes ResolveQueryData writes per query (d3d12.h: UINT64 for
/// occlusion and timestamps, D3D12_QUERY_DATA_PIPELINE_STATISTICS 11 x
/// UINT64, D3D12_QUERY_DATA_SO_STATISTICS 2 x UINT64).
[[nodiscard]] u64 query_result_bytes(u32 type) {
    switch (type) {
    case D3D12_QUERY_TYPE_PIPELINE_STATISTICS: return 88;
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM1:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM2:
    case D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3: return 16;
    default: return 8;
    }
}

}  // namespace

// =============================================================================
// ID3D12CommandAllocator
// =============================================================================

void Frontend::create_command_allocator(Marshaler& m, Device& device) {
    // CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, REFIID, void **).
    const u32 type = m.arg32(1);
    const GuestAddr out = m.arg_ptr(3);
    if (out != 0) (void)write_pointer(out, 0);
    if (out == 0 && list_type_supported(type)) {
        // Measured (a04 allocator "ppv=NULL").
        m.set_return(E_UNEXPECTED);
        return;
    }
    if (out == 0 || !list_type_supported(type)) {
        // The VIDEO_* types name engines this backend does not expose.
        if (type >= D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE &&
            type <= D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE) {
            unimplemented("ID3D12Device::CreateCommandAllocator",
                          "video command list types");
        }
        m.set_return(E_INVALIDARG);
        return;
    }
    auto allocator = std::make_unique<CommandAllocator>();
    allocator->device = device.address;
    allocator->type = type;
    m.set_return(return_new_object(std::move(allocator), m.arg_ptr(2), out));
}

void Frontend::command_allocator_method(Marshaler& m,
                                        CommandAllocator& allocator,
                                        usize slot) {
    switch (slot) {
    case CommandAllocatorSlot::Reset:
        // Learn: "This method returns E_FAIL if there is an actively
        // recording command list referencing the command allocator."
        if (allocator.recording_lists != 0) {
            m.set_return(E_FAIL);
            return;
        }
        // Recorded commands belong to each list's Recording, which an
        // execution in flight holds itself; the allocator has nothing more
        // to release.
        m.set_return(S_OK);
        return;
    default:
        unimplemented("ID3D12CommandAllocator", "slot");
        m.set_return(E_NOTIMPL);
        return;
    }
}

// =============================================================================
// Creation
// =============================================================================

void Frontend::create_command_list(Marshaler& m, Device& device, bool list1) {
    // CreateCommandList(UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
    //     ID3D12CommandAllocator *pCommandAllocator,
    //     ID3D12PipelineState *pInitialState, REFIID riid,
    //     void **ppCommandList).
    // ID3D12Device4::CreateCommandList1(UINT nodeMask, type,
    //     D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **): created
    //     CLOSED, with no allocator.
    const u32 node_mask = m.arg32(1);
    const u32 type = m.arg32(2);
    const usize riid_index = list1 ? 4 : 5;
    const GuestAddr out = m.arg_ptr(riid_index + 1);
    if (out != 0) (void)write_pointer(out, 0);
    if (!list_type_supported(type) || node_mask > 1) {
        m.set_return(E_INVALIDARG);
        return;
    }
    if (out == 0) {
        // Measured (a04 list_create "ppv=NULL").
        m.set_return(E_UNEXPECTED);
        return;
    }
    auto list = std::make_unique<CommandList>();
    list->device = device.address;
    list->type = type;
    list->node_mask = node_mask;
    list->recording = std::make_shared<Recording>();
    list->recording->type = type;
    CommandAllocator* allocator = nullptr;
    if (list1) {
        // D3D12_COMMAND_LIST_FLAGS has no flag defined; measured (a04
        // list_create "CreateCommandList1(flags=1)"): S_OK all the same.
        list->closed = true;
    } else {
        allocator = find<CommandAllocator>(m.arg_ptr(3), Kind::CommandAllocator);
        // Learn: the allocator's type must match the list's, and an allocator
        // backs one recording list at a time.
        if (allocator == nullptr || allocator->type != type ||
            allocator->recording_lists != 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        const GuestAddr initial = m.arg_ptr(4);
        if (initial != 0) {
            PipelineState* pso = find<PipelineState>(initial,
                                                     Kind::PipelineState);
            if (pso == nullptr) {
                m.set_return(E_INVALIDARG);
                return;
            }
            list->recording->initial_pso = pso->address;
            list->recording->initial_pso_serial = pso->serial;
        }
        list->allocator = allocator->address;
    }
    const u32 result = return_new_object(std::move(list),
                                         m.arg_ptr(riid_index), out);
    // Learn: a list is created recording (CreateCommandList1: closed).
    if (result == S_OK && allocator != nullptr) ++allocator->recording_lists;
    m.set_return(result);
}

void Frontend::record_error(CommandList& list, u32 error, const char* method,
                            const char* why) {
    if (list.record_error == 0) {
        list.record_error = error;
        d3d12_diag("ID3D12GraphicsCommandList::%s: %s; list 0x%llx will fail "
                   "Close with 0x%08x",
                   method, why, static_cast<unsigned long long>(list.address),
                   error);
    }
}

bool Frontend::snapshot_cpu_descriptor(GuestAddr handle, DescriptorType type,
                                       Descriptor& out) {
    DescriptorHeap* heap = nullptr;
    const Descriptor* record = cpu_descriptor(handle, &heap);
    if (record == nullptr) return false;
    if ((type == DescriptorType::Rtv &&
         heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV) ||
        (type == DescriptorType::Dsv &&
         heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_DSV) ||
        (type == DescriptorType::Uav &&
         heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)) {
        return false;
    }
    out = *record;
    return true;
}

// =============================================================================
// ID3D12GraphicsCommandList
// =============================================================================

void Frontend::command_list_method(Marshaler& m, CommandList& list,
                                   usize slot) {
    using S = GraphicsCommandListSlot;
    const char* name = slot < GRAPHICS_COMMAND_LIST_METHOD_COUNT
                           ? GRAPHICS_COMMAND_LIST_METHOD_NAMES[slot]
                           : "<slot>";
    switch (slot) {
    case S::GetType:
        m.set_return(list.type);
        return;
    case S::Close: {
        // Learn: "E_FAIL if the command list has already been closed, or an
        // invalid API was called during command list recording"; an error
        // recorded earlier is what Close returns.
        if (list.closed) {
            m.set_return(E_FAIL);
            return;
        }
        if (list.in_render_pass) {
            record_error(list, E_FAIL, name, "closed inside a render pass");
        }
        // Measured (a04 close_rules "BeginQuery without EndQuery").
        if (!list.open_queries.empty()) {
            record_error(list, E_FAIL, name, "a query begun and never ended");
        }
        list.closed = true;
        if (auto* allocator =
                find<CommandAllocator>(list.allocator, Kind::CommandAllocator);
            allocator != nullptr && allocator->recording_lists > 0) {
            --allocator->recording_lists;
        }
        m.set_return(list.record_error != 0 ? list.record_error : S_OK);
        return;
    }
    case S::Reset: {
        // Reset(ID3D12CommandAllocator *, ID3D12PipelineState *).
        // Learn: "If a call to Close fails, the command list can never be
        // reset. Calling Reset will result in the same error being returned
        // that Close returned." / the list must be closed.
        if (list.record_error != 0) {
            m.set_return(list.record_error);
            return;
        }
        if (!list.closed) {
            m.set_return(E_FAIL);
            return;
        }
        auto* allocator =
            find<CommandAllocator>(m.arg_ptr(1), Kind::CommandAllocator);
        // Learn: "E_INVALIDARG if the allocator is currently being used with
        // another command list in the 'recording' state."
        if (allocator == nullptr || allocator->type != list.type ||
            allocator->recording_lists != 0) {
            m.set_return(E_INVALIDARG);
            return;
        }
        GuestAddr initial = m.arg_ptr(2);
        u64 initial_serial = 0;
        if (initial != 0) {
            PipelineState* pso = find<PipelineState>(initial,
                                                     Kind::PipelineState);
            if (pso == nullptr) {
                m.set_return(E_INVALIDARG);
                return;
            }
            initial_serial = pso->serial;
        }
        // A fresh recording: an execution still waiting holds the old one.
        list.recording = std::make_shared<Recording>();
        list.recording->type = list.type;
        list.recording->initial_pso = initial;
        list.recording->initial_pso_serial = initial_serial;
        list.allocator = allocator->address;
        list.closed = false;
        list.in_render_pass = false;
        list.render_pass_resolves.clear();
        list.open_queries.clear();
        list.heaps = {};
        list.heaps_set = false;
        ++allocator->recording_lists;
        m.set_return(S_OK);
        return;
    }
    default:
        break;
    }

    m.set_return(0);
    if (list.closed) {
        // A recording method on a closed list records nothing; the runtime
        // drops it (the debug layer is what reports it).
        d3d12_diag("ID3D12GraphicsCommandList::%s on a closed list", name);
        return;
    }
    Recording& rec = *list.recording;
    const bool bundle = list.type == D3D12_COMMAND_LIST_TYPE_BUNDLE;
    const bool compute = list.type == D3D12_COMMAND_LIST_TYPE_COMPUTE;
    const bool copy = list.type == D3D12_COMMAND_LIST_TYPE_COPY;
    const usize pointer_size = m_is_64bit ? 8u : 4u;
    // Learn (Command list types): copy lists take copies, barriers and
    // timestamps; compute lists no rasterizer or output-merger work.
    const auto require = [&](bool allowed) {
        if (!allowed) record_error(list, E_FAIL, name,
                                   "not callable on this command list type");
        return allowed;
    };
    const auto resource_ref = [&](GuestAddr address, u64& serial) {
        Resource* resource = find<Resource>(address, Kind::Resource);
        serial = resource != nullptr ? resource->serial : 0;
        return resource;
    };

    switch (slot) {
    case S::ClearState: {
        // ClearState(ID3D12PipelineState *pPipelineState). Direct lists only.
        if (!require(!bundle && !copy && !compute)) return;
        const GuestAddr pso_address = m.arg_ptr(1);
        u64 serial = 0;
        if (pso_address != 0) {
            PipelineState* pso =
                find<PipelineState>(pso_address, Kind::PipelineState);
            if (pso == nullptr) {
                record_error(list, E_INVALIDARG, name, "not a pipeline state");
                return;
            }
            serial = pso->serial;
        }
        rec.commands.emplace_back(cmd::ClearState{pso_address, serial});
        return;
    }
    case S::DrawInstanced:
        if (!require(!compute && !copy)) return;
        rec.commands.emplace_back(
            cmd::Draw{m.arg32(1), m.arg32(2), m.arg32(3), m.arg32(4)});
        return;
    case S::DrawIndexedInstanced:
        if (!require(!compute && !copy)) return;
        rec.commands.emplace_back(cmd::DrawIndexed{
            m.arg32(1), m.arg32(2), m.arg32(3),
            static_cast<i32>(m.arg32(4)), m.arg32(5)});
        return;
    case S::Dispatch:
        if (!require(!copy)) return;
        rec.commands.emplace_back(
            cmd::Dispatch{m.arg32(1), m.arg32(2), m.arg32(3)});
        return;
    case S::CopyBufferRegion: {
        // CopyBufferRegion(pDstBuffer, UINT64 DstOffset, pSrcBuffer,
        //     UINT64 SrcOffset, UINT64 NumBytes).
        if (!require(!bundle)) return;
        usize next = 0;
        const GuestAddr dst = m.arg_ptr(1);
        const u64 dst_offset = read_arg_u64(m, m_is_64bit, 2, next);
        const GuestAddr src = m.arg_ptr(next);
        const u64 src_offset = read_arg_u64(m, m_is_64bit, next + 1, next);
        const u64 bytes = read_arg_u64(m, m_is_64bit, next, next);
        u64 dst_serial = 0;
        u64 src_serial = 0;
        Resource* d = resource_ref(dst, dst_serial);
        Resource* s = resource_ref(src, src_serial);
        if (d == nullptr || s == nullptr || !d->is_buffer() ||
            !s->is_buffer() || dst_offset > d->desc.width ||
            bytes > d->desc.width - dst_offset ||
            src_offset > s->desc.width || bytes > s->desc.width - src_offset) {
            record_error(list, E_INVALIDARG, name, "invalid buffer region");
            return;
        }
        rec.commands.emplace_back(cmd::CopyBufferRegion{
            dst, dst_serial, dst_offset, src, src_serial, src_offset, bytes});
        return;
    }
    case S::CopyTextureRegion: {
        // CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION *pDst, DstX,
        //     DstY, DstZ, const D3D12_TEXTURE_COPY_LOCATION *pSrc,
        //     const D3D12_BOX *pSrcBox).
        if (!require(!bundle)) return;
        const auto read_location = [&](GuestAddr at, CopyLocation& out) {
            const auto& l = layout::D3D12_TEXTURE_COPY_LOCATION[m_is_64bit];
            GuestAddr resource = 0;
            if (at == 0 || !read_pointer(at + l.pResource, resource)) {
                return false;
            }
            u64 serial = 0;
            if (resource_ref(resource, serial) == nullptr) return false;
            out.resource = resource;
            out.serial = serial;
            out.type = read_u32(at + l.Type);
            if (out.type == D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) {
                out.subresource = read_u32(at + l.SubresourceIndex);
            } else if (out.type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
                out.offset = read_u64(at + l.PlacedFootprint_Offset);
                out.format = read_u32(at + l.PlacedFootprint_Footprint_Format);
                out.width = read_u32(at + l.PlacedFootprint_Footprint_Width);
                out.height = read_u32(at + l.PlacedFootprint_Footprint_Height);
                out.depth = read_u32(at + l.PlacedFootprint_Footprint_Depth);
                out.row_pitch =
                    read_u32(at + l.PlacedFootprint_Footprint_RowPitch);
            } else {
                return false;
            }
            return true;
        };
        cmd::CopyTextureRegion copy_cmd{};
        if (!read_location(m.arg_ptr(1), copy_cmd.dst) ||
            !read_location(m.arg_ptr(5), copy_cmd.src)) {
            record_error(list, E_INVALIDARG, name, "invalid copy location");
            return;
        }
        copy_cmd.x = m.arg32(2);
        copy_cmd.y = m.arg32(3);
        copy_cmd.z = m.arg32(4);
        const GuestAddr box = m.arg_ptr(6);
        copy_cmd.has_box = box != 0;
        if (box != 0) {
            for (u32 index = 0; index < 6; ++index) {
                copy_cmd.box[index] = read_u32(box + index * 4);
            }
        }
        rec.commands.emplace_back(std::move(copy_cmd));
        return;
    }
    case S::CopyResource: {
        if (!require(!bundle)) return;
        u64 dst_serial = 0;
        u64 src_serial = 0;
        Resource* d = resource_ref(m.arg_ptr(1), dst_serial);
        Resource* s = resource_ref(m.arg_ptr(2), src_serial);
        // Learn (CopyResource): same dimension, size and sample count.
        if (d == nullptr || s == nullptr || d == s ||
            d->desc.dimension != s->desc.dimension ||
            d->desc.width != s->desc.width ||
            d->desc.height != s->desc.height ||
            d->desc.depth_or_array_size != s->desc.depth_or_array_size ||
            d->desc.mip_levels != s->desc.mip_levels ||
            d->desc.sample_count != s->desc.sample_count) {
            record_error(list, E_INVALIDARG, name, "incompatible resources");
            return;
        }
        rec.commands.emplace_back(
            cmd::CopyResource{d->address, dst_serial, s->address, src_serial});
        return;
    }
    case S::ResolveSubresource: {
        // ResolveSubresource(pDst, DstSubresource, pSrc, SrcSubresource,
        //     DXGI_FORMAT Format).
        if (!require(!bundle && !compute && !copy)) return;
        u64 dst_serial = 0;
        u64 src_serial = 0;
        Resource* d = resource_ref(m.arg_ptr(1), dst_serial);
        Resource* s = resource_ref(m.arg_ptr(3), src_serial);
        if (d == nullptr || s == nullptr || s->desc.sample_count <= 1 ||
            d->desc.sample_count != 1) {
            record_error(list, E_INVALIDARG, name, "invalid resolve");
            return;
        }
        rec.commands.emplace_back(cmd::ResolveSubresource{
            d->address, dst_serial, m.arg32(2), s->address, src_serial,
            m.arg32(4), m.arg32(5)});
        return;
    }
    case S::IASetPrimitiveTopology:
        if (!require(!compute && !copy)) return;
        rec.commands.emplace_back(cmd::SetTopology{m.arg32(1)});
        return;
    case S::RSSetViewports: {
        if (!require(!bundle && !compute && !copy)) return;
        const u32 count = m.arg32(1);
        const GuestAddr at = m.arg_ptr(2);
        if (count != 0 && at == 0) {
            record_error(list, E_INVALIDARG, name, "invalid viewports");
            return;
        }
        // Measured (a04 close_limits "RSSetViewports(17)"): no error; the
        // viewports past the sixteen a pipeline has are not state.
        cmd::SetViewports viewports{};
        const auto& l = layout::D3D12_VIEWPORT[m_is_64bit];
        for (u32 index = 0;
             index < std::min<u32>(count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
             ++index) {
            const GuestAddr v = at + index * l.size;
            viewports.viewports.push_back(
                {read_f32(v + l.TopLeftX), read_f32(v + l.TopLeftY),
                 read_f32(v + l.Width), read_f32(v + l.Height),
                 read_f32(v + l.MinDepth), read_f32(v + l.MaxDepth)});
        }
        rec.commands.emplace_back(std::move(viewports));
        return;
    }
    case S::RSSetScissorRects: {
        if (!require(!bundle && !compute && !copy)) return;
        const u32 count = m.arg32(1);
        const GuestAddr at = m.arg_ptr(2);
        if (count != 0 && at == 0) {
            record_error(list, E_INVALIDARG, name, "invalid scissor rects");
            return;
        }
        // Measured (a04 close_limits "RSSetScissorRects(17)"): as viewports.
        cmd::SetScissors scissors{};
        const auto& l = layout::D3D12_RECT[m_is_64bit];
        for (u32 index = 0;
             index < std::min<u32>(count, D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);
             ++index) {
            const GuestAddr r = at + index * l.size;
            scissors.rects.push_back(
                {static_cast<i32>(read_u32(r + l.left)),
                 static_cast<i32>(read_u32(r + l.top)),
                 static_cast<i32>(read_u32(r + l.right)),
                 static_cast<i32>(read_u32(r + l.bottom))});
        }
        rec.commands.emplace_back(std::move(scissors));
        return;
    }
    case S::OMSetBlendFactor: {
        if (!require(!compute && !copy)) return;
        // Learn: a null pointer means {1, 1, 1, 1}.
        const GuestAddr at = m.arg_ptr(1);
        cmd::SetBlendFactor factor{{1.0f, 1.0f, 1.0f, 1.0f}};
        if (at != 0) {
            for (u32 c = 0; c < 4; ++c) factor.factor[c] = read_f32(at + c * 4);
        }
        rec.commands.emplace_back(factor);
        return;
    }
    case S::OMSetStencilRef:
        if (!require(!compute && !copy)) return;
        rec.commands.emplace_back(cmd::SetStencilRef{m.arg32(1), m.arg32(1)});
        return;
    case S::SetPipelineState: {
        if (!require(!copy)) return;
        PipelineState* pso = find<PipelineState>(m.arg_ptr(1),
                                                 Kind::PipelineState);
        if (pso == nullptr) {
            record_error(list, E_INVALIDARG, name, "not a pipeline state");
            return;
        }
        rec.commands.emplace_back(
            cmd::SetPipelineState{pso->address, pso->serial});
        return;
    }
    case S::ResourceBarrier: {
        // ResourceBarrier(UINT NumBarriers, const D3D12_RESOURCE_BARRIER *).
        // The backend orders every read after every write it tracks (Metal's
        // hazard tracking, tracked heaps included), which covers what a
        // transition, aliasing and UAV barrier each promise. What remains is
        // the runtime's validation of the description.
        if (!require(!bundle)) return;
        const u32 count = m.arg32(1);
        const GuestAddr at = m.arg_ptr(2);
        if (count != 0 && at == 0) {
            record_error(list, E_INVALIDARG, name, "null barrier array");
            return;
        }
        const auto& l = layout::D3D12_RESOURCE_BARRIER[m_is_64bit];
        bool uav = false;
        for (u32 index = 0; index < count; ++index) {
            const GuestAddr b = at + index * l.size;
            const u32 type = read_u32(b + l.Type);
            const u32 flags = read_u32(b + l.Flags);
            if (flags > (D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY |
                         D3D12_RESOURCE_BARRIER_FLAG_END_ONLY) ||
                flags == (D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY |
                          D3D12_RESOURCE_BARRIER_FLAG_END_ONLY)) {
                record_error(list, E_INVALIDARG, name, "invalid flags");
                return;
            }
            if (type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                GuestAddr address = 0;
                (void)read_pointer(b + l.Transition_pResource, address);
                const Resource* r = find<Resource>(address, Kind::Resource);
                if (r == nullptr) {
                    record_error(list, E_INVALIDARG, name,
                                 "transition of a non-resource");
                    return;
                }
                const u32 subresource = read_u32(b + l.Transition_Subresource);
                const u32 before = read_u32(b + l.Transition_StateBefore);
                const u32 after = read_u32(b + l.Transition_StateAfter);
                if (const char* why = transition_violation(*r, subresource,
                                                           before, after,
                                                           list.type)) {
                    record_error(list, E_INVALIDARG, name, why);
                    return;
                }
            } else if (type == D3D12_RESOURCE_BARRIER_TYPE_UAV) {
                // Learn (D3D12_RESOURCE_UAV_BARRIER): NULL for every UAV
                // access, else a resource that can be one (measured, a04
                // close_rules "UAV barrier on UPLOAD buffer").
                GuestAddr address = 0;
                (void)read_pointer(b + l.UAV_pResource, address);
                if (address != 0) {
                    const Resource* r = find<Resource>(address, Kind::Resource);
                    if (r == nullptr ||
                        (r->desc.flags &
                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0) {
                        record_error(list, E_INVALIDARG, name,
                                     "UAV barrier on a resource without UAVs");
                        return;
                    }
                }
                uav = true;
            } else if (type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING) {
                // Learn (D3D12_RESOURCE_ALIASING_BARRIER): placed or reserved
                // resources, or NULL (measured, a04 close_rules "aliasing
                // barrier committed/NULL").
                for (const u32 offset : {l.Aliasing_pResourceBefore,
                                         l.Aliasing_pResourceAfter}) {
                    GuestAddr address = 0;
                    (void)read_pointer(b + offset, address);
                    if (address == 0) continue;
                    const Resource* r = find<Resource>(address, Kind::Resource);
                    if (r == nullptr || r->committed) {
                        record_error(list, E_INVALIDARG, name,
                                     "aliasing barrier on a committed resource");
                        return;
                    }
                }
            } else {
                record_error(list, E_INVALIDARG, name, "invalid barrier type");
                return;
            }
        }
        if (count != 0) rec.commands.emplace_back(cmd::Barrier{uav});
        return;
    }
    case S::ExecuteBundle: {
        if (!require(!bundle && !compute && !copy)) return;
        CommandList* child = find<CommandList>(m.arg_ptr(1), Kind::CommandList);
        // Learn: the bundle must be closed, and a bundle, and error-free.
        if (child == nullptr ||
            child->type != D3D12_COMMAND_LIST_TYPE_BUNDLE || !child->closed ||
            child->record_error != 0) {
            record_error(list, E_INVALIDARG, name, "not a closed bundle");
            return;
        }
        if (child->heaps_set && child->heaps != list.heaps) {
            record_error(list, E_INVALIDARG, name,
                         "the bundle's descriptor heaps differ from the caller's");
            return;
        }
        rec.commands.emplace_back(cmd::ExecuteBundle{child->recording});
        return;
    }
    case S::SetDescriptorHeaps: {
        // SetDescriptorHeaps(UINT NumDescriptorHeaps,
        //     ID3D12DescriptorHeap *const *ppDescriptorHeaps): at most one
        // CBV/SRV/UAV and one sampler heap, both shader visible (Learn).
        if (!require(!copy)) return;
        const u32 count = m.arg32(1);
        const GuestAddr array = m.arg_ptr(2);
        cmd::SetDescriptorHeaps heaps{};
        for (u32 index = 0; index < count; ++index) {
            GuestAddr address = 0;
            (void)read_pointer(array + index * pointer_size, address);
            DescriptorHeap* heap =
                find<DescriptorHeap>(address, Kind::DescriptorHeap);
            if (heap == nullptr || heap->type > D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER ||
                (heap->flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) == 0 ||
                heaps.heaps[heap->type] != 0) {
                record_error(list, E_INVALIDARG, name, "invalid heap set");
                return;
            }
            heaps.heaps[heap->type] = heap->address;
            heaps.serials[heap->type] = heap->serial;
        }
        list.heaps = heaps.heaps;
        list.heaps_set = true;
        rec.commands.emplace_back(heaps);
        return;
    }
    case S::SetComputeRootSignature:
    case S::SetGraphicsRootSignature: {
        const bool is_compute = slot == S::SetComputeRootSignature;
        if (!require(!copy && (is_compute || !compute))) return;
        const GuestAddr address = m.arg_ptr(1);
        u64 serial = 0;
        if (address != 0) {
            RootSignature* root =
                find<RootSignature>(address, Kind::RootSignature);
            if (root == nullptr) {
                record_error(list, E_INVALIDARG, name, "not a root signature");
                return;
            }
            serial = root->serial;
        }
        rec.commands.emplace_back(
            cmd::SetRootSignature{is_compute, address, serial});
        return;
    }
    case S::SetComputeRootDescriptorTable:
    case S::SetGraphicsRootDescriptorTable: {
        // (UINT RootParameterIndex, D3D12_GPU_DESCRIPTOR_HANDLE by value --
        // two stack slots on x86).
        const bool is_compute = slot == S::SetComputeRootDescriptorTable;
        if (!require(!copy && (is_compute || !compute))) return;
        usize next = 0;
        const u64 handle = read_arg_u64(m, m_is_64bit, 2, next);
        rec.commands.emplace_back(
            cmd::SetRootTable{is_compute, m.arg32(1), handle});
        return;
    }
    case S::SetComputeRoot32BitConstant:
    case S::SetGraphicsRoot32BitConstant: {
        // (UINT RootParameterIndex, UINT SrcData, UINT DestOffsetIn32BitValues)
        const bool is_compute = slot == S::SetComputeRoot32BitConstant;
        if (!require(!copy && (is_compute || !compute))) return;
        rec.commands.emplace_back(cmd::SetRootConstants{
            is_compute, m.arg32(1), m.arg32(3), {m.arg32(2)}});
        return;
    }
    case S::SetComputeRoot32BitConstants:
    case S::SetGraphicsRoot32BitConstants: {
        // (UINT RootParameterIndex, UINT Num32BitValuesToSet,
        //  const void *pSrcData, UINT DestOffsetIn32BitValues)
        const bool is_compute = slot == S::SetComputeRoot32BitConstants;
        if (!require(!copy && (is_compute || !compute))) return;
        const u32 count = m.arg32(2);
        const GuestAddr data = m.arg_ptr(3);
        if (count > 64 || (count != 0 && data == 0)) {
            record_error(list, E_INVALIDARG, name, "invalid constants");
            return;
        }
        cmd::SetRootConstants constants{is_compute, m.arg32(1), m.arg32(4), {}};
        constants.values.resize(count);
        if (count != 0 &&
            m_memory.read_bytes(data,
                                reinterpret_cast<Byte*>(constants.values.data()),
                                count * 4u)
                .is_err()) {
            record_error(list, E_INVALIDARG, name, "unreadable constants");
            return;
        }
        rec.commands.emplace_back(std::move(constants));
        return;
    }
    case S::SetComputeRootConstantBufferView:
    case S::SetGraphicsRootConstantBufferView:
    case S::SetComputeRootShaderResourceView:
    case S::SetGraphicsRootShaderResourceView:
    case S::SetComputeRootUnorderedAccessView:
    case S::SetGraphicsRootUnorderedAccessView: {
        // (UINT RootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS by value)
        const bool is_compute = slot == S::SetComputeRootConstantBufferView ||
                                slot == S::SetComputeRootShaderResourceView ||
                                slot == S::SetComputeRootUnorderedAccessView;
        if (!require(!copy && (is_compute || !compute))) return;
        const u32 type =
            slot <= S::SetGraphicsRootConstantBufferView
                ? D3D12_ROOT_PARAMETER_TYPE_CBV
                : slot <= S::SetGraphicsRootShaderResourceView
                      ? D3D12_ROOT_PARAMETER_TYPE_SRV
                      : D3D12_ROOT_PARAMETER_TYPE_UAV;
        usize next = 0;
        const u64 va = read_arg_u64(m, m_is_64bit, 2, next);
        rec.commands.emplace_back(
            cmd::SetRootView{is_compute, m.arg32(1), type, va});
        return;
    }
    case S::IASetIndexBuffer: {
        if (!require(!compute && !copy)) return;
        const GuestAddr view = m.arg_ptr(1);
        cmd::SetIndexBuffer ib{0, 0, 0};
        if (view != 0) {
            const auto& l = layout::D3D12_INDEX_BUFFER_VIEW[m_is_64bit];
            ib.va = read_u64(view + l.BufferLocation);
            ib.size = read_u32(view + l.SizeInBytes);
            ib.format = read_u32(view + l.Format);
            // Learn: R16_UINT or R32_UINT.
            if (ib.format != DXGI_FORMAT_R16_UINT &&
                ib.format != DXGI_FORMAT_R32_UINT) {
                record_error(list, E_INVALIDARG, name, "index format");
                return;
            }
        }
        rec.commands.emplace_back(ib);
        return;
    }
    case S::IASetVertexBuffers: {
        // IASetVertexBuffers(UINT StartSlot, UINT NumViews,
        //     const D3D12_VERTEX_BUFFER_VIEW *pViews); null views unbind.
        if (!require(!compute && !copy)) return;
        const u32 start = m.arg32(1);
        const u32 count = m.arg32(2);
        const GuestAddr views = m.arg_ptr(3);
        // Measured (a04 close_limits "IASetVertexBuffers(0, 33)",
        // "(31, 2)"): no error; slots past the 32 the input assembler has
        // are not state.
        const u32 slots = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT;
        const u32 kept = start >= slots ? 0u : std::min(count, slots - start);
        cmd::SetVertexBuffers set{start, {}};
        const auto& l = layout::D3D12_VERTEX_BUFFER_VIEW[m_is_64bit];
        for (u32 index = 0; index < kept; ++index) {
            VertexBufferView v{};
            if (views != 0) {
                const GuestAddr at = views + index * l.size;
                v.va = read_u64(at + l.BufferLocation);
                v.size = read_u32(at + l.SizeInBytes);
                v.stride = read_u32(at + l.StrideInBytes);
            }
            set.views.push_back(v);
        }
        rec.commands.emplace_back(std::move(set));
        return;
    }
    case S::SOSetTargets: {
        // No pipeline state with stream output can be created
        // (CreateGraphicsPipelineState refuses one), so targets set here are
        // never written; binding them has no other effect.
        if (!require(!bundle && !compute && !copy)) return;
        return;
    }
    case S::OMSetRenderTargets: {
        // OMSetRenderTargets(UINT NumRenderTargetDescriptors,
        //     const D3D12_CPU_DESCRIPTOR_HANDLE *pRenderTargetDescriptors,
        //     BOOL RTsSingleHandleToDescriptorRange,
        //     const D3D12_CPU_DESCRIPTOR_HANDLE *pDepthStencilDescriptor).
        if (!require(!bundle && !compute && !copy)) return;
        const u32 count = m.arg32(1);
        const GuestAddr handles = m.arg_ptr(2);
        const bool single_range = m.arg32(3) != 0;
        const GuestAddr dsv_handle = m.arg_ptr(4);
        // Measured (a04 close_limits "OMSetRenderTargets(9 RTVs)"): an
        // invalid call.
        if (count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT) {
            record_error(list, E_FAIL, name, "more than 8 render targets");
            return;
        }
        if (count != 0 && handles == 0) {
            record_error(list, E_INVALIDARG, name, "invalid render targets");
            return;
        }
        cmd::SetRenderTargets targets{};
        targets.count = count;
        GuestAddr first = 0;
        if (count != 0 && !read_pointer(handles, first)) return;
        for (u32 index = 0; index < count; ++index) {
            GuestAddr handle = 0;
            if (single_range) {
                handle = first + static_cast<GuestAddr>(index) *
                                     descriptor_increment();
            } else if (!read_pointer(handles + index * pointer_size, handle)) {
                return;
            }
            if (!snapshot_cpu_descriptor(handle, DescriptorType::Rtv,
                                         targets.rtvs[index])) {
                record_error(list, E_INVALIDARG, name, "not an RTV descriptor");
                return;
            }
        }
        if (dsv_handle != 0) {
            GuestAddr handle = 0;
            if (!read_pointer(dsv_handle, handle) ||
                !snapshot_cpu_descriptor(handle, DescriptorType::Dsv,
                                         targets.dsv)) {
                record_error(list, E_INVALIDARG, name, "not a DSV descriptor");
                return;
            }
            targets.has_dsv = true;
        }
        rec.commands.emplace_back(std::move(targets));
        return;
    }
    case S::ClearDepthStencilView: {
        // ClearDepthStencilView(D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS,
        //     FLOAT Depth, UINT8 Stencil, UINT NumRects, const D3D12_RECT *).
        if (!require(!bundle && !compute && !copy)) return;
        cmd::ClearDepthStencil clear{};
        if (!snapshot_cpu_descriptor(m.arg_ptr(1), DescriptorType::Dsv,
                                     clear.dsv)) {
            record_error(list, E_INVALIDARG, name, "not a DSV descriptor");
            return;
        }
        clear.flags = m.arg32(2);
        clear.depth = m.arg_f32(3);
        clear.stencil = static_cast<u8>(m.arg32(4));
        const u32 count = m.arg32(5);
        const GuestAddr rects = m.arg_ptr(6);
        const auto& l = layout::D3D12_RECT[m_is_64bit];
        for (u32 index = 0; index < count && rects != 0; ++index) {
            const GuestAddr r = rects + index * l.size;
            clear.rects.push_back({static_cast<i32>(read_u32(r + l.left)),
                                   static_cast<i32>(read_u32(r + l.top)),
                                   static_cast<i32>(read_u32(r + l.right)),
                                   static_cast<i32>(read_u32(r + l.bottom))});
        }
        rec.commands.emplace_back(std::move(clear));
        return;
    }
    case S::ClearRenderTargetView: {
        // ClearRenderTargetView(D3D12_CPU_DESCRIPTOR_HANDLE,
        //     const FLOAT ColorRGBA[4], UINT NumRects, const D3D12_RECT *).
        if (!require(!bundle && !compute && !copy)) return;
        cmd::ClearRenderTarget clear{};
        if (!snapshot_cpu_descriptor(m.arg_ptr(1), DescriptorType::Rtv,
                                     clear.rtv)) {
            record_error(list, E_INVALIDARG, name, "not an RTV descriptor");
            return;
        }
        const GuestAddr color = m.arg_ptr(2);
        for (u32 c = 0; c < 4; ++c) clear.color[c] = read_f32(color + c * 4);
        const u32 count = m.arg32(3);
        const GuestAddr rects = m.arg_ptr(4);
        const auto& l = layout::D3D12_RECT[m_is_64bit];
        for (u32 index = 0; index < count && rects != 0; ++index) {
            const GuestAddr r = rects + index * l.size;
            clear.rects.push_back({static_cast<i32>(read_u32(r + l.left)),
                                   static_cast<i32>(read_u32(r + l.top)),
                                   static_cast<i32>(read_u32(r + l.right)),
                                   static_cast<i32>(read_u32(r + l.bottom))});
        }
        rec.commands.emplace_back(std::move(clear));
        return;
    }
    case S::ClearUnorderedAccessViewUint:
    case S::ClearUnorderedAccessViewFloat: {
        // (D3D12_GPU_DESCRIPTOR_HANDLE ViewGPUHandleInCurrentHeap -- two
        //  slots on x86, D3D12_CPU_DESCRIPTOR_HANDLE ViewCPUHandle,
        //  ID3D12Resource *pResource, const UINT/FLOAT Values[4],
        //  UINT NumRects, const D3D12_RECT *pRects).
        if (!require(!bundle && !copy)) return;
        usize next = 0;
        (void)read_arg_u64(m, m_is_64bit, 1, next);
        cmd::ClearUnorderedAccess clear{};
        clear.is_float = slot == S::ClearUnorderedAccessViewFloat;
        if (!snapshot_cpu_descriptor(m.arg_ptr(next), DescriptorType::Uav,
                                     clear.uav) ||
            clear.uav.type != DescriptorType::Uav ||
            clear.uav.resource != m.arg_ptr(next + 1)) {
            record_error(list, E_INVALIDARG, name,
                         "not a UAV descriptor of pResource");
            return;
        }
        const GuestAddr values = m.arg_ptr(next + 2);
        for (u32 c = 0; c < 4; ++c) clear.values[c] = read_u32(values + c * 4);
        const u32 count = m.arg32(next + 3);
        const GuestAddr rects = m.arg_ptr(next + 4);
        const auto& l = layout::D3D12_RECT[m_is_64bit];
        for (u32 index = 0; index < count && rects != 0; ++index) {
            const GuestAddr r = rects + index * l.size;
            clear.rects.push_back({static_cast<i32>(read_u32(r + l.left)),
                                   static_cast<i32>(read_u32(r + l.top)),
                                   static_cast<i32>(read_u32(r + l.right)),
                                   static_cast<i32>(read_u32(r + l.bottom))});
        }
        rec.commands.emplace_back(std::move(clear));
        return;
    }
    case S::DiscardResource:
        // DiscardResource(pResource, const D3D12_DISCARD_REGION *): the
        // contents become undefined. Keeping them is one of the defined
        // outcomes of "undefined", so there is nothing to do but validate.
        if (!require(!bundle)) return;
        {
            Resource* r = find<Resource>(m.arg_ptr(1), Kind::Resource);
            if (r == nullptr) {
                record_error(list, E_INVALIDARG, name, "not a resource");
                return;
            }
            // D3D12_DISCARD_REGION {UINT NumRects; const D3D12_RECT *pRects;
            // UINT FirstSubresource; UINT NumSubresources}: the subresources
            // exist, and rectangles name one subresource (measured, a07
            // discard: 0..3 with a rect and index 4 of 4 are E_INVALIDARG).
            if (const GuestAddr region = m.arg_ptr(2); region != 0) {
                const auto& l = layout::D3D12_DISCARD_REGION[m_is_64bit];
                const u32 rects = read_u32(region + l.NumRects);
                const u32 first = read_u32(region + l.FirstSubresource);
                const u32 count = read_u32(region + l.NumSubresources);
                const u64 total = r->subresource_count();
                if (static_cast<u64>(first) + count > total ||
                    (rects != 0 && count > 1)) {
                    record_error(list, E_INVALIDARG, name,
                                 "subresources out of range / rects on several");
                }
            }
        }
        return;
    case S::SetMarker:
    case S::BeginEvent: {
        // (UINT Metadata, const void *pData, UINT Size). Metadata 1
        // (PIX_EVENT_UNICODE_VERSION) is a wide string, 0 ANSI; anything else
        // is an opaque PIX blob.
        const u32 metadata = m.arg32(1);
        const GuestAddr data = m.arg_ptr(2);
        const u32 size = m.arg32(3);
        std::string label;
        if (data != 0 && metadata == 0) {
            const auto text = m.read_ansi_string(data, std::min(size, 256u));
            if (text.is_ok()) label = text.value();
        } else if (data != 0 && metadata == 1) {
            for (u32 index = 0; index + 1 < size && index < 512; index += 2) {
                const auto unit = m_memory.read_u16(data + index);
                if (!unit.is_ok() || unit.value() == 0) break;
                label.push_back(unit.value() < 0x80
                                    ? static_cast<char>(unit.value())
                                    : '?');
            }
        }
        rec.commands.emplace_back(
            cmd::Event{slot == S::SetMarker ? 0u : 1u, std::move(label)});
        return;
    }
    case S::EndEvent:
        rec.commands.emplace_back(cmd::Event{2u, std::string()});
        return;
    case S::SetPredication: {
        // SetPredication(ID3D12Resource *pBuffer, UINT64 AlignedBufferOffset,
        //     D3D12_PREDICATION_OP Operation). A null buffer turns predication
        //     off, the state every list starts in. Measured (a06
        //     predication): the offset is 8-byte aligned, the op one of the
        //     two, any heap type will do.
        if (!require(!bundle && !copy)) return;
        usize next = 0;
        const GuestAddr buffer = m.arg_ptr(1);
        const u64 offset = read_arg_u64(m, m_is_64bit, 2, next);
        const u32 op = m.arg32(next);
        if (buffer == 0) {
            rec.commands.emplace_back(cmd::SetPredication{0, 0, 0, 0});
            return;
        }
        u64 serial = 0;
        Resource* r = resource_ref(buffer, serial);
        if (r == nullptr || !r->is_buffer() || offset % 8 != 0 ||
            offset > r->desc.width || r->desc.width - offset < 8 ||
            op > D3D12_PREDICATION_OP_NOT_EQUAL_ZERO) {
            record_error(list, E_INVALIDARG, name, "invalid predicate");
            return;
        }
        rec.commands.emplace_back(cmd::SetPredication{buffer, serial, offset, op});
        return;
    }
    case S::BeginQuery:
    case S::EndQuery: {
        // (ID3D12QueryHeap *pQueryHeap, D3D12_QUERY_TYPE Type, UINT Index).
        const bool begin = slot == S::BeginQuery;
        QueryHeap* heap = find<QueryHeap>(m.arg_ptr(1), Kind::QueryHeap);
        const u32 type = m.arg32(2);
        const u32 index = m.arg32(3);
        if (heap == nullptr || index >= heap->count ||
            !query_type_fits(heap->type, type)) {
            record_error(list, E_INVALIDARG, name, "query type / heap / index");
            return;
        }
        // Learn (Queries): a timestamp is only ended; copy lists take only
        // timestamps (COPY_QUEUE_TIMESTAMP heaps), compute lists no
        // occlusion; bundles no queries at all.
        const bool timestamp = type == D3D12_QUERY_TYPE_TIMESTAMP;
        // Measured (a04 close_rules): a query on the wrong list type is an
        // invalid call (E_FAIL), a bad heap / index an invalid argument.
        if (bundle || (copy && !timestamp) ||
            (compute && (type == D3D12_QUERY_TYPE_OCCLUSION ||
                         type == D3D12_QUERY_TYPE_BINARY_OCCLUSION))) {
            record_error(list, E_FAIL, name,
                         "query not valid on this list type");
            return;
        }
        if ((timestamp && begin) ||
            (copy != (heap->type == D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP) &&
             timestamp)) {
            record_error(list, E_INVALIDARG, name, "query heap / type");
            return;
        }
        if (type == D3D12_QUERY_TYPE_PIPELINE_STATISTICS ||
            (type >= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 &&
             type <= D3D12_QUERY_TYPE_SO_STATISTICS_STREAM3)) {
            unimplemented(begin ? "ID3D12GraphicsCommandList::BeginQuery"
                                : "ID3D12GraphicsCommandList::EndQuery",
                          "pipeline / stream-output statistics queries");
            record_error(list, E_INVALIDARG, name, "statistics queries");
            return;
        }
        if (!timestamp) {
            const std::pair<GuestAddr, u32> key{heap->address, index};
            auto& open = list.open_queries;
            const auto it = std::find(open.begin(), open.end(), key);
            if (begin == (it != open.end())) {
                record_error(list, E_FAIL, name,
                             begin ? "query already begun" : "query not begun");
                return;
            }
            if (begin) {
                open.push_back(key);
            } else {
                open.erase(it);
            }
        }
        rec.commands.emplace_back(
            cmd::Query{heap->address, heap->serial, type, index, begin});
        return;
    }
    case S::ResolveQueryData: {
        // ResolveQueryData(pQueryHeap, Type, UINT StartIndex,
        //     UINT NumQueries, ID3D12Resource *pDestinationBuffer,
        //     UINT64 AlignedDestinationBufferOffset).
        if (!require(!bundle)) return;
        QueryHeap* heap = find<QueryHeap>(m.arg_ptr(1), Kind::QueryHeap);
        const u32 type = m.arg32(2);
        const u32 start = m.arg32(3);
        const u32 count = m.arg32(4);
        usize next = 0;
        const GuestAddr dst = m.arg_ptr(5);
        const u64 offset = read_arg_u64(m, m_is_64bit, 6, next);
        u64 dst_serial = 0;
        Resource* d = resource_ref(dst, dst_serial);
        const u64 element = query_result_bytes(type);
        if (heap == nullptr || d == nullptr || !d->is_buffer() ||
            !query_type_fits(heap->type, type) || start > heap->count ||
            count > heap->count - start || offset % 8 != 0 ||
            offset > d->desc.width ||
            static_cast<u64>(count) * element > d->desc.width - offset) {
            record_error(list, E_INVALIDARG, name, "invalid resolve");
            return;
        }
        if (element != 8) {
            unimplemented("ID3D12GraphicsCommandList::ResolveQueryData",
                          "pipeline / stream-output statistics queries");
            record_error(list, E_INVALIDARG, name, "statistics queries");
            return;
        }
        if (count == 0) return;
        rec.commands.emplace_back(cmd::ResolveQuery{
            heap->address, heap->serial, type, start, count, dst, dst_serial,
            offset});
        return;
    }
    case S::ExecuteIndirect: {
        // ExecuteIndirect(ID3D12CommandSignature *, UINT MaxCommandCount,
        //     ID3D12Resource *pArgumentBuffer, UINT64 ArgumentBufferOffset,
        //     ID3D12Resource *pCountBuffer, UINT64 CountBufferOffset).
        if (!require(!copy)) return;
        CommandSignature* signature =
            find<CommandSignature>(m.arg_ptr(1), Kind::CommandSignature);
        const u32 max_count = m.arg32(2);
        const GuestAddr args = m.arg_ptr(3);
        usize next = 0;
        const u64 args_offset = read_arg_u64(m, m_is_64bit, 4, next);
        const GuestAddr count_buffer = m.arg_ptr(next);
        const u64 count_offset = read_arg_u64(m, m_is_64bit, next + 1, next);
        u64 args_serial = 0;
        u64 count_serial = 0;
        Resource* a = resource_ref(args, args_serial);
        Resource* c = count_buffer != 0 ? resource_ref(count_buffer, count_serial)
                                        : nullptr;
        const bool draws =
            signature != nullptr &&
            signature->arguments.back().type != D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        if (signature == nullptr || a == nullptr || !a->is_buffer() ||
            args_offset % 4 != 0 ||
            (count_buffer != 0 &&
             (c == nullptr || !c->is_buffer() || count_offset % 4 != 0 ||
              count_offset > c->desc.width || c->desc.width - count_offset < 4)) ||
            (draws && compute)) {
            record_error(list, E_INVALIDARG, name, "invalid indirect execution");
            return;
        }
        rec.commands.emplace_back(cmd::ExecuteIndirect{
            signature->address, signature->serial, max_count, args,
            args_serial, args_offset, count_buffer, count_serial,
            count_offset});
        return;
    }
    case S::CopyTiles:
        // Only a reserved resource has tiles, and none can be created
        // (TiledResourcesTier NOT_SUPPORTED).
        if (!require(!bundle)) return;
        record_error(list, E_INVALIDARG, name, "not a reserved resource");
        return;
    case S::AtomicCopyBufferUINT:
    case S::AtomicCopyBufferUINT64: {
        // (pDstBuffer, UINT64 DstOffset, pSrcBuffer, UINT64 SrcOffset,
        //  UINT Dependencies, ID3D12Resource *const *ppDependentResources,
        //  const D3D12_SUBRESOURCE_RANGE_UINT64 *). One 4 / 8-byte value,
        //  copied in order with every earlier write of the source and the
        //  dependent resources -- which the one backend stream guarantees --
        //  and as one unit (a single aligned blit element).
        if (!require(!bundle)) return;
        const u64 bytes = slot == S::AtomicCopyBufferUINT ? 4u : 8u;
        usize next = 0;
        const GuestAddr dst = m.arg_ptr(1);
        const u64 dst_offset = read_arg_u64(m, m_is_64bit, 2, next);
        const GuestAddr src = m.arg_ptr(next);
        const u64 src_offset = read_arg_u64(m, m_is_64bit, next + 1, next);
        const u32 dependencies = m.arg32(next);
        const GuestAddr dependents = m.arg_ptr(next + 1);
        u64 dst_serial = 0;
        u64 src_serial = 0;
        Resource* d = resource_ref(dst, dst_serial);
        Resource* s = resource_ref(src, src_serial);
        bool valid = d != nullptr && s != nullptr && d->is_buffer() &&
                     s->is_buffer() && dst_offset % bytes == 0 &&
                     src_offset % bytes == 0 && dst_offset <= d->desc.width &&
                     d->desc.width - dst_offset >= bytes &&
                     src_offset <= s->desc.width &&
                     s->desc.width - src_offset >= bytes &&
                     dependencies <= 31 &&
                     (dependencies == 0 || dependents != 0);
        for (u32 index = 0; valid && index < dependencies; ++index) {
            GuestAddr dependent = 0;
            valid = read_pointer(dependents + index * pointer_size, dependent) &&
                    find<Resource>(dependent, Kind::Resource) != nullptr;
        }
        if (!valid) {
            record_error(list, E_INVALIDARG, name, "invalid atomic copy");
            return;
        }
        rec.commands.emplace_back(cmd::CopyBufferRegion{
            dst, dst_serial, dst_offset, src, src_serial, src_offset, bytes});
        return;
    }
    case S::OMSetDepthBounds:
        // (FLOAT Min, FLOAT Max). DepthBoundsTestSupported is FALSE, so no
        // pipeline tests against them; they are state all the same.
        if (!require(!compute && !copy)) return;
        rec.commands.emplace_back(cmd::SetDepthBounds{m.arg_f32(1), m.arg_f32(2)});
        return;
    case S::SetSamplePositions:
        // (UINT NumSamplesPerPixel, UINT NumPixels, D3D12_SAMPLE_POSITION *).
        // ProgrammableSamplePositionsTier NOT_SUPPORTED: only the reset to
        // the standard pattern (0 samples or no positions) is valid.
        if (!require(!compute && !copy)) return;
        if (m.arg32(1) == 0 || m.arg_ptr(3) == 0) return;
        unimplemented("ID3D12GraphicsCommandList1::SetSamplePositions",
                      "programmable sample positions (tier NOT_SUPPORTED)");
        record_error(list, E_INVALIDARG, name, "programmable sample positions");
        return;
    case S::ResolveSubresourceRegion: {
        // ResolveSubresourceRegion(pDst, DstSubresource, DstX, DstY, pSrc,
        //     SrcSubresource, D3D12_RECT *pSrcRect, DXGI_FORMAT Format,
        //     D3D12_RESOLVE_MODE ResolveMode).
        if (!require(!bundle && !compute && !copy)) return;
        u64 dst_serial = 0;
        u64 src_serial = 0;
        Resource* d = resource_ref(m.arg_ptr(1), dst_serial);
        Resource* s = resource_ref(m.arg_ptr(5), src_serial);
        const u32 mode = m.arg32(9);
        if (d == nullptr || s == nullptr || mode > D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK) {
            record_error(list, E_INVALIDARG, name, "invalid resolve");
            return;
        }
        if (mode != D3D12_RESOLVE_MODE_AVERAGE) {
            // The backend resolves by averaging (MIN / MAX need
            // ProgrammableSamplePositions-era hardware support the caps pass
            // does not report; DECOMPRESS and sampler feedback likewise).
            unimplemented("ID3D12GraphicsCommandList1::ResolveSubresourceRegion",
                          "a MIN / MAX / DECOMPRESS / sampler feedback resolve");
            record_error(list, E_INVALIDARG, name, "resolve mode");
            return;
        }
        if (s->desc.sample_count <= 1 || d->desc.sample_count != 1) {
            record_error(list, E_INVALIDARG, name, "invalid resolve");
            return;
        }
        cmd::ResolveSubresource resolve{d->address, dst_serial, m.arg32(2),
                                        s->address, src_serial, m.arg32(6),
                                        m.arg32(8)};
        resolve.dst_x = m.arg32(3);
        resolve.dst_y = m.arg32(4);
        if (const GuestAddr rect = m.arg_ptr(7); rect != 0) {
            resolve.has_rect = true;
            for (u32 i = 0; i < 4; ++i) {
                resolve.rect[i] = static_cast<i32>(read_u32(rect + i * 4u));
            }
        }
        rec.commands.emplace_back(resolve);
        return;
    }
    case S::WriteBufferImmediate: {
        // WriteBufferImmediate(UINT Count,
        //     const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER *pParams,
        //     const D3D12_WRITEBUFFERIMMEDIATE_MODE *pModes). Every mode is
        //     honoured by the one in-order backend stream: MARKER_IN /
        //     MARKER_OUT only relax when the write may land.
        const u32 count = m.arg32(1);
        const GuestAddr params = m.arg_ptr(2);
        const GuestAddr modes = m.arg_ptr(3);
        if (count != 0 && params == 0) {
            record_error(list, E_INVALIDARG, name, "null parameters");
            return;
        }
        const auto& l = layout::D3D12_WRITEBUFFERIMMEDIATE_PARAMETER[m_is_64bit];
        cmd::WriteImmediate write{};
        for (u32 index = 0; index < count; ++index) {
            const GuestAddr at = params + index * l.size;
            const u64 va = read_u64(at + l.Dest);
            const u32 mode = modes != 0 ? read_u32(modes + index * 4u) : 0u;
            graphics::BackendBuffer* target = nullptr;
            u64 offset = 0;
            // Learn asks for a 4-byte aligned Dest, but the runtime does not
            // check it (measured, a07 write_immediate "VA+2": S_OK); the
            // value is written at the address given.
            if (mode > D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT ||
                !resolve_va(va, 4, target, offset)) {
                record_error(list, E_INVALIDARG, name, "invalid destination");
                return;
            }
            write.writes.emplace_back(va, read_u32(at + l.Value));
        }
        if (!write.writes.empty()) rec.commands.emplace_back(std::move(write));
        return;
    }
    case S::BeginRenderPass:
        begin_render_pass(m, list, name);
        return;
    case S::EndRenderPass: {
        if (!require(!bundle && !compute && !copy)) return;
        if (!list.in_render_pass) {
            record_error(list, E_FAIL, name, "no render pass begun");
            return;
        }
        list.in_render_pass = false;
        for (auto& resolve : list.render_pass_resolves) {
            rec.commands.emplace_back(resolve);
        }
        list.render_pass_resolves.clear();
        return;
    }
    case S::InitializeMetaCommand:
    case S::ExecuteMetaCommand:
        // No meta command exists (EnumerateMetaCommands reports none).
        record_error(list, E_INVALIDARG, name, "not a meta command");
        return;
    case S::BuildRaytracingAccelerationStructure:
    case S::EmitRaytracingAccelerationStructurePostbuildInfo:
    case S::CopyRaytracingAccelerationStructure:
    case S::SetPipelineState1:
    case S::DispatchRays:
        unimplemented((std::string("ID3D12GraphicsCommandList4::") + name).c_str(),
                      "raytracing (tier NOT_SUPPORTED)");
        record_error(list, E_INVALIDARG, name, "raytracing");
        return;
    case S::RSSetShadingRate:
        // (D3D12_SHADING_RATE, const D3D12_SHADING_RATE_COMBINER *):
        // VariableShadingRateTier NOT_SUPPORTED; 1x1 with no combiners is
        // the default and changes nothing.
        if (!require(!compute && !copy)) return;
        if (m.arg32(1) == 0 && m.arg_ptr(2) == 0) return;
        unimplemented("ID3D12GraphicsCommandList5::RSSetShadingRate",
                      "variable-rate shading (tier NOT_SUPPORTED)");
        record_error(list, E_INVALIDARG, name, "variable-rate shading");
        return;
    case S::RSSetShadingRateImage:
        if (!require(!compute && !copy)) return;
        if (m.arg_ptr(1) == 0) return;
        unimplemented("ID3D12GraphicsCommandList5::RSSetShadingRateImage",
                      "variable-rate shading (tier NOT_SUPPORTED)");
        record_error(list, E_INVALIDARG, name, "variable-rate shading");
        return;
    case S::DispatchMesh:
        unimplemented("ID3D12GraphicsCommandList6::DispatchMesh",
                      "mesh shaders (tier NOT_SUPPORTED)");
        record_error(list, E_INVALIDARG, name, "mesh shaders");
        return;
    case S::Barrier:
        // ID3D12GraphicsCommandList7::Barrier: D3D12_OPTIONS12
        // EnhancedBarriersSupported is FALSE on this device.
        unimplemented("ID3D12GraphicsCommandList7::Barrier",
                      "enhanced barriers (EnhancedBarriersSupported FALSE)");
        record_error(list, E_INVALIDARG, name, "enhanced barriers");
        return;
    case S::OMSetFrontAndBackStencilRef:
        // D3D12_OPTIONS14 IndependentFrontAndBackStencilRefMaskSupported is
        // FALSE; one reference for both faces is what OMSetStencilRef sets.
        if (!require(!compute && !copy)) return;
        if (m.arg32(1) == m.arg32(2)) {
            rec.commands.emplace_back(cmd::SetStencilRef{m.arg32(1), m.arg32(1)});
            return;
        }
        unimplemented("ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef",
                      "independent front / back stencil references");
        record_error(list, E_INVALIDARG, name, "independent stencil references");
        return;
    case S::RSSetDepthBias:
        // D3D12_OPTIONS16 DynamicDepthBiasSupported is FALSE.
        unimplemented("ID3D12GraphicsCommandList9::RSSetDepthBias",
                      "dynamic depth bias (not supported)");
        record_error(list, E_INVALIDARG, name, "dynamic depth bias");
        return;
    case S::IASetIndexBufferStripCutValue:
        // D3D12_OPTIONS15 DynamicIndexBufferStripCutSupported is FALSE.
        unimplemented("ID3D12GraphicsCommandList9::IASetIndexBufferStripCutValue",
                      "dynamic strip-cut values (not supported)");
        record_error(list, E_INVALIDARG, name, "dynamic strip cut");
        return;
    case S::SetProgram:
    case S::DispatchGraph:
        unimplemented((std::string("ID3D12GraphicsCommandList10::") + name).c_str(),
                      "work graphs / generic programs (not supported)");
        record_error(list, E_INVALIDARG, name, "work graphs");
        return;
    case S::SetProtectedResourceSession:
        // A null session is the default.
        if (m.arg_ptr(1) == 0) return;
        break;
    case S::SetViewInstanceMask:
        // View instancing is reported unsupported (caps pass); mask 0 is the
        // default and changes nothing.
        if (m.arg32(1) == 0) return;
        break;
    default:
        break;
    }
    // Every slot has a case above; the IUnknown / ID3D12Object ones are
    // d3d12_objects.cpp's. A non-null protected session or a non-zero view
    // instance mask (features the caps pass reports unsupported) end here.
    std::string api = std::string("ID3D12GraphicsCommandList::") + name;
    unimplemented(api.c_str(), "not implemented");
    record_error(list, E_INVALIDARG, name, "not implemented");
}

void Frontend::begin_render_pass(Marshaler& m, CommandList& list,
                                 const char* name) {
    // BeginRenderPass(UINT NumRenderTargets,
    //     const D3D12_RENDER_PASS_RENDER_TARGET_DESC *pRenderTargets,
    //     const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *pDepthStencil,
    //     D3D12_RENDER_PASS_FLAGS Flags).
    // D3D12_RENDER_PASS_TIER_0 (what the caps pass reports): the pass is
    // what the runtime turns it into on such a device -- the CLEAR
    // beginnings as clears, the targets bound, and the RESOLVE endings run
    // at EndRenderPass. PRESERVE, DISCARD and NO_ACCESS keep the contents
    // (a defined outcome of DISCARD's "undefined").
    if (list.type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        record_error(list, E_FAIL, name, "not callable on this list type");
        return;
    }
    if (list.in_render_pass) {
        record_error(list, E_FAIL, name, "a render pass is already begun");
        return;
    }
    const u32 count = m.arg32(1);
    const GuestAddr targets = m.arg_ptr(2);
    const GuestAddr depth = m.arg_ptr(3);
    const u32 flags = m.arg32(4);
    if (count > D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT ||
        (count != 0 && targets == 0) ||
        (flags & ~(D3D12_RENDER_PASS_FLAG_ALLOW_UAV_WRITES |
                   D3D12_RENDER_PASS_FLAG_SUSPENDING_PASS |
                   D3D12_RENDER_PASS_FLAG_RESUMING_PASS)) != 0) {
        record_error(list, E_INVALIDARG, name, "invalid render pass");
        return;
    }
    Recording& rec = *list.recording;
    std::vector<cmd::ResolveSubresource> resolves;
    const auto& sp = layout::D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS[m_is_64bit];
    // An ending RESOLVE: one whole-subresource average per parameter.
    const auto add_resolves = [&](GuestAddr src_ptr, GuestAddr dst_ptr,
                                  u32 subresources, GuestAddr params,
                                  u32 format, u32 mode) {
        GuestAddr src = 0;
        GuestAddr dst = 0;
        (void)read_pointer(src_ptr, src);
        (void)read_pointer(dst_ptr, dst);
        Resource* s = find<Resource>(src, Kind::Resource);
        Resource* d = find<Resource>(dst, Kind::Resource);
        if (s == nullptr || d == nullptr || (subresources != 0 && params == 0)) {
            return false;
        }
        for (u32 index = 0; index < subresources; ++index) {
            const GuestAddr at = params + index * sp.size;
            const bool whole =
                read_u32(at + sp.DstX) == 0 && read_u32(at + sp.DstY) == 0 &&
                read_u32(at + sp.SrcRect_left) == 0 &&
                read_u32(at + sp.SrcRect_top) == 0;
            if (mode != D3D12_RESOLVE_MODE_AVERAGE || !whole ||
                s->desc.sample_count <= 1) {
                unimplemented("ID3D12GraphicsCommandList4::BeginRenderPass",
                              "a resolve ending other than a whole-"
                              "subresource average");
                return false;
            }
            resolves.push_back(cmd::ResolveSubresource{
                d->address, d->serial, read_u32(at + sp.DstSubresource),
                s->address, s->serial, read_u32(at + sp.SrcSubresource),
                format});
        }
        return true;
    };
    cmd::SetRenderTargets bind{};
    bind.count = count;
    const auto& rt = layout::D3D12_RENDER_PASS_RENDER_TARGET_DESC[m_is_64bit];
    for (u32 index = 0; index < count; ++index) {
        const GuestAddr at = targets + index * rt.size;
        GuestAddr handle = 0;
        (void)read_pointer(at + rt.cpuDescriptor_ptr, handle);
        if (!snapshot_cpu_descriptor(handle, DescriptorType::Rtv,
                                     bind.rtvs[index])) {
            record_error(list, E_INVALIDARG, name, "not an RTV descriptor");
            return;
        }
        const u32 begin_type = read_u32(at + rt.BeginningAccess_Type);
        const u32 end_type = read_u32(at + rt.EndingAccess_Type);
        if (begin_type > D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS ||
            end_type > D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS) {
            record_error(list, E_INVALIDARG, name, "invalid access type");
            return;
        }
        if (begin_type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
            cmd::ClearRenderTarget clear{};
            clear.rtv = bind.rtvs[index];
            for (u32 c = 0; c < 4; ++c) {
                clear.color[c] = read_f32(
                    at + rt.BeginningAccess_Clear_ClearValue_Color + c * 4);
            }
            rec.commands.emplace_back(std::move(clear));
        }
        if (end_type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE &&
            !add_resolves(at + rt.EndingAccess_Resolve_pSrcResource,
                          at + rt.EndingAccess_Resolve_pDstResource,
                          read_u32(at + rt.EndingAccess_Resolve_SubresourceCount),
                          [&] {
                              GuestAddr p = 0;
                              (void)read_pointer(
                                  at + rt.EndingAccess_Resolve_pSubresourceParameters,
                                  p);
                              return p;
                          }(),
                          read_u32(at + rt.EndingAccess_Resolve_Format),
                          read_u32(at + rt.EndingAccess_Resolve_ResolveMode))) {
            record_error(list, E_INVALIDARG, name, "invalid resolve ending");
            return;
        }
    }
    if (depth != 0) {
        const auto& ds = layout::D3D12_RENDER_PASS_DEPTH_STENCIL_DESC[m_is_64bit];
        GuestAddr handle = 0;
        (void)read_pointer(depth + ds.cpuDescriptor_ptr, handle);
        const u32 depth_begin = read_u32(depth + ds.DepthBeginningAccess_Type);
        const u32 stencil_begin =
            read_u32(depth + ds.StencilBeginningAccess_Type);
        const u32 depth_end = read_u32(depth + ds.DepthEndingAccess_Type);
        const u32 stencil_end = read_u32(depth + ds.StencilEndingAccess_Type);
        const bool no_access =
            depth_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS &&
            stencil_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
        if (!no_access) {
            if (!snapshot_cpu_descriptor(handle, DescriptorType::Dsv, bind.dsv)) {
                record_error(list, E_INVALIDARG, name, "not a DSV descriptor");
                return;
            }
            bind.has_dsv = true;
            u32 clear_flags = 0;
            if (depth_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
                clear_flags |= D3D12_CLEAR_FLAG_DEPTH;
            }
            if (stencil_begin == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR) {
                clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
            }
            if (clear_flags != 0) {
                cmd::ClearDepthStencil clear{};
                clear.dsv = bind.dsv;
                clear.flags = clear_flags;
                clear.depth = read_f32(
                    depth +
                    ds.DepthBeginningAccess_Clear_ClearValue_DepthStencil_Depth);
                const auto stencil = m_memory.read_u8(
                    depth +
                    ds.StencilBeginningAccess_Clear_ClearValue_DepthStencil_Stencil);
                clear.stencil = stencil.is_ok() ? stencil.value() : 0;
                rec.commands.emplace_back(std::move(clear));
            }
        }
        if (depth_end == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE ||
            stencil_end == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE) {
            unimplemented("ID3D12GraphicsCommandList4::BeginRenderPass",
                          "depth / stencil resolve endings");
            record_error(list, E_INVALIDARG, name, "depth-stencil resolve");
            return;
        }
    }
    rec.commands.emplace_back(std::move(bind));
    list.in_render_pass = true;
    list.render_pass_resolves = std::move(resolves);
}

}  // namespace vitreo::winapi::d3d12

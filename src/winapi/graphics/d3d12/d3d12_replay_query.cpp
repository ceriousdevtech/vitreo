// SPDX-License-Identifier: LGPL-2.1-only
/// @file d3d12_replay_query.cpp
/// @brief GPU-ordered readback, queries and predication.
///        Split out of d3d12_replay.cpp as verbatim code motion.

#include "winapi/graphics/d3d12/d3d12_replay_internal.hpp"
#include "winapi/graphics/common/gpu_event_signaler.hpp"

#include <cstring>
#include <thread>

namespace vitreo::winapi::d3d12 {

bool Replayer::gpu_read(Resource& r, u64 offset, void* out, usize bytes) {
    graphics::BackendBuffer* source = r.buffer();
    if (source == nullptr || bytes == 0) return false;
    // A GPU copy into CPU-visible memory, in order after everything
    // recorded so far (a resolve's GPU wait and copy included), then the
    // wait for it: the bytes as the GPU leaves them, from any heap type.
    graphics::BackendBuffer* staging = nullptr;
    u64 staging_offset = 0;
    u8* host = nullptr;
    std::unique_ptr<graphics::BackendHeap> large;
    if (bytes <= CONSTANT_CHUNK_BYTES / 4) {
        if (!reserve(bytes, staging, staging_offset, host)) return false;
    } else {
        graphics::BackendHeapDesc desc{};
        desc.size = (bytes + 0xFFFFu) & ~u64{0xFFFFu};
        desc.cpu_visible = true;
        desc.buffers_only = true;
        large = f.m_backend.create_heap(desc);
        if (!large || large->buffer() == nullptr ||
            large->host_pointer() == nullptr) {
            return false;
        }
        staging = large->buffer();
        host = static_cast<u8*>(large->host_pointer());
    }
    if (!f.m_backend.copy_buffer_region(staging, staging_offset, source,
                                        r.offset + offset, bytes)) {
        return false;
    }
    if (auto completion = f.m_backend.flush_completion()) {
        if (!completion->wait()) return false;
    } else if (!f.m_backend.wait_idle()) {
        return false;
    }
    std::memcpy(out, host, bytes);
    return true;
}

void Replayer::query(ReplayState& st, const cmd::Query& c) {
    QueryHeap* heap = f.find_serial<QueryHeap>(c.heap, c.serial, Kind::QueryHeap);
    if (heap == nullptr || c.index >= heap->slots.size()) return;
    auto& slot = heap->slots[c.index];
    if (c.type == D3D12_QUERY_TYPE_TIMESTAMP) {
        // The time the GPU finished everything recorded before it (D3D12
        // "Timing": the End of a TIMESTAMP query), in GetTimestampFrequency
        // ticks -- the backend's host-clock nanoseconds.
        slot.timestamp = f.m_backend.record_timestamp();
        if (!slot.timestamp) {
            unimplemented("ExecuteCommandLists", "timestamps on this backend");
        }
        return;
    }
    // OCCLUSION / BINARY_OCCLUSION: the samples that pass the depth and
    // stencil tests in the draws between Begin and End.
    if (c.begin) {
        st.occlusion = f.m_backend.create_occlusion_counter();
        if (!st.occlusion) {
            unimplemented("ExecuteCommandLists",
                          "occlusion queries on this backend");
        }
        return;
    }
    slot.occlusion = std::move(st.occlusion);
    slot.timestamp.reset();
    st.occlusion.reset();
}

void Replayer::resolve_query(const cmd::ResolveQuery& c) {
    QueryHeap* heap =
        f.find_serial<QueryHeap>(c.heap, c.heap_serial, Kind::QueryHeap);
    Resource* dst = resource(c.dst, c.dst_serial);
    if (heap == nullptr || dst == nullptr || dst->buffer() == nullptr) return;
    // The results exist only once the GPU has run the queries, so the
    // values are written by the CPU after the work recorded so far
    // completes, into upload space a GPU copy then moves into the
    // destination -- a copy that waits for that write on a private fence,
    // so every later command (a copy to a readback buffer, a predicate)
    // reads the resolved data in order, without the CPU waiting here.
    struct Source {
        std::shared_ptr<graphics::GpuOcclusionCounter> occlusion;
        std::shared_ptr<graphics::GpuTimestamp> timestamp;
    };
    std::vector<Source> sources;
    for (u32 index = 0; index < c.count; ++index) {
        const auto& slot = heap->slots[c.start + index];
        sources.push_back({slot.occlusion, slot.timestamp});
    }
    const usize bytes = static_cast<usize>(c.count) * 8u;
    graphics::BackendBuffer* staging = nullptr;
    u64 staging_offset = 0;
    u8* host = nullptr;
    graphics::BackendFence* fence = f.resolve_fence();
    if (fence == nullptr || !reserve(bytes, staging, staging_offset, host)) {
        d3d12_diag("ResolveQueryData: no upload space or fence");
        return;
    }
    const u64 value = ++f.m_resolve_value;
    const bool binary = c.type == D3D12_QUERY_TYPE_BINARY_OCCLUSION;
    auto completion = f.m_backend.flush_completion();
    std::shared_ptr<graphics::BackendFence> keep = f.m_resolve_fence;
    GpuEventSignaler::instance().run_after(
        std::move(completion),
        [sources = std::move(sources), host, binary, keep, value]() {
            for (usize index = 0; index < sources.size(); ++index) {
                u64 result = 0;
                if (sources[index].occlusion) {
                    result = sources[index].occlusion->samples();
                    if (binary) result = result != 0 ? 1u : 0u;
                } else if (const auto& t = sources[index].timestamp) {
                    // Ready once the submission holding it has completed;
                    // its completion handler may trail the generation's.
                    for (int spin = 0; spin < 200000 && !t->ready(); ++spin) {
                        std::this_thread::yield();
                    }
                    result = t->ready() ? t->gpu_time_ns() : 0u;
                }
                std::memcpy(host + index * 8u, &result, 8u);
            }
            (void)keep->signal_cpu(value);
        });
    if (!fence->wait_gpu(value) ||
        !f.m_backend.copy_buffer_region(dst->buffer(), dst->offset + c.offset,
                                        staging, staging_offset, bytes)) {
        d3d12_diag("ResolveQueryData: the backend refused the copy");
    }
}

void Replayer::set_predication(ReplayState& st, const cmd::SetPredication& c) {
    st.predicated_off = false;
    if (c.buffer == 0) return;
    Resource* r = resource(c.buffer, c.serial);
    if (r == nullptr) return;
    // Metal has no predicated rendering: the predicate is read once the GPU
    // has written it (everything recorded so far), and decides for every
    // predicated operation until the next SetPredication (Learn:
    // EQUAL_ZERO skips when the 64-bit value is 0, NOT_EQUAL_ZERO when it
    // is not).
    u64 predicate = 0;
    if (!gpu_read(*r, c.offset, &predicate, sizeof(predicate))) {
        d3d12_diag("SetPredication: the predicate could not be read");
        return;
    }
    st.predicated_off = c.op == D3D12_PREDICATION_OP_EQUAL_ZERO
                            ? predicate == 0
                            : predicate != 0;
}

}  // namespace vitreo::winapi::d3d12

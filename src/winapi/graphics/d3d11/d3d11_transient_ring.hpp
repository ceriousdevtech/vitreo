// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file d3d11_transient_ring.hpp
/// @brief gs-ring (graphics/contracts/slim_record.hpp): the geometry
///        emulation's per-draw buffers, reused once the GPU has retired them.
///
/// A draw through a geometry program records a capture (pass A), an
/// expansion kernel (B) and a draw of what B wrote (C), and each used to
/// create the buffers between them -- the vertex records and primitive ids,
/// the expansion and its counter block -- as new MTLBuffers, 2-4 a draw, each
/// an IOGPUResourceCreate. The buffers are transient: nothing outlives the
/// draw that made them except the GPU work recorded with them.
///
/// The contract a reused buffer keeps:
///   - It is reused only once the submission generation it was returned in
///     has completed (GraphicsBackendDevice::generation_complete). Every use
///     was recorded before it was returned, so no GPU work still reads or
///     writes it: in-flight expansions never alias one another.
///   - Each buffer is its own backing resource, so Metal's hazard tracking
///     (per MTLResource) orders passes A, B and C of the draw that holds it
///     exactly as it did for a fresh buffer.
///   - What the caller asks to be in it (the counter's control block, the
///     primitive ids) is written whole. Bytes the caller does not initialise
///     keep what the previous draw left; every consumer reads only what the
///     current draw's GPU work writes first (pass C culls the vertices past an
///     invocation's written count without reading them; stream output and
///     the statistics read only counted vertices), as it did with a fresh
///     buffer, whose contents D3D11 never defined either.
///   - A buffer is rounded up to a size class (at most 25% larger) so a
///     steady scene stops creating buffers after warm-up. No consumer reads
///     a transient's length: its kernels and draws are bounded by the counts
///     the draw computed, and buffer copies are bounded by the region.
///   - Untracked work (a backend without generations, or a thread capturing
///     into a command list, recording_generation() == 0) cannot be waited
///     on: such a buffer is created exactly as before and freed when the
///     draw's locals die.
/// Single-threaded: the D3D11 device lock covers every draw.
///
/// Scope: the buffers above (geometry_expand's expansion and counter, the
/// direct capture's records, ids and capture indices, and line_quad_params).
/// A plain direct GS draw therefore creates no buffer once warm. Still made
/// per draw, outside the ring: a stream-output GS draw's buffers
/// (d3d11_stream_out.cpp: stream_out_pass's op and control blocks,
/// stream_out_geometry_expansion's order and total words; and a non-GS
/// stream-out draw's own capture), the indirect draw's IA-resolve buffers
/// (capture_indirect, which outlive the draw for overflow feedback), and the
/// tessellation chain's own buffers.

#include <vitreo/common.hpp>
#include "graphics/contracts/backend_device.hpp"
#include "graphics/contracts/slim_record.hpp"

#include <cstdio>
#include <deque>
#include <memory>
#include <vector>

namespace vitreo::winapi {

class TransientBufferRing {
public:
    struct Return {
        TransientBufferRing* ring{nullptr};
        void operator()(graphics::BackendBuffer* buffer) const noexcept {
            if (buffer == nullptr) return;
            if (ring == nullptr) {
                delete buffer;
                return;
            }
            ring->give(buffer);
        }
    };
    /// A transient buffer: back to its ring (or freed) when it goes out of
    /// scope. Every GPU use must be recorded before then.
    using Buffer = std::unique_ptr<graphics::BackendBuffer, Return>;

    struct Stats {
        u64 created{0};    ///< buffers the ring created (to be returned to it)
        u64 reused{0};     ///< acquisitions served by a retired buffer
        u64 bypassed{0};   ///< acquisitions created outside the ring
        usize idle{0};     ///< buffers waiting for reuse
        usize idle_bytes{0};
    };

    /// Buffers larger than this are not kept: created and freed per draw.
    static constexpr usize kMaxBufferBytes = usize{32} << 20;
    /// What the idle buffers may hold at most; the oldest go first.
    static constexpr usize kMaxIdleBytes = usize{128} << 20;
    static constexpr usize kMaxIdle = 512;
    /// An idle buffer not reused within this many submissions is freed.
    static constexpr u64 kStaleGenerations = 4096;

    explicit TransientBufferRing(graphics::GraphicsBackendDevice& backend)
        : m_backend(backend) {}
    TransientBufferRing(const TransientBufferRing&) = delete;
    TransientBufferRing& operator=(const TransientBufferRing&) = delete;

    /// The smallest size class holding @p bytes: 4 KiB, then quarter-octave
    /// steps (5, 6, 7, 8 x 2^k / 4 for 2^k < bytes <= 2^(k+1)), so a class
    /// is less than 25% above the request.
    [[nodiscard]] static constexpr usize size_class(usize bytes) noexcept {
        constexpr usize kMin = 4096;
        if (bytes <= kMin) return kMin;
        unsigned k = 0;  // 2^k < bytes <= 2^(k + 1), k >= 12
        for (usize v = bytes - 1; v > 1; v >>= 1) ++k;
        const usize step = usize{1} << (k - 2);  // a quarter of 2^k
        return (bytes + step - 1) & ~(step - 1);
    }

    /// A buffer shaped as @p desc, at least desc.byte_width long, whose
    /// first @p initial_bytes are @p initial (null: no contents promised; see
    /// above). Null when the backend cannot make one.
    [[nodiscard]] Buffer acquire(const graphics::BackendBufferDesc& desc,
                                 const void* initial, usize initial_bytes) {
        const bool ringable =
            !desc.shareable && !desc.tiled && !desc.constant_buffer &&
            desc.byte_width != 0 && desc.byte_width <= kMaxBufferBytes &&
            initial_bytes <= desc.byte_width &&
            graphics::slim_record::active(graphics::slim_record::Part::GsRing);
        const u64 now = ringable ? m_backend.recording_generation() : 0;
        if (now == 0) {
            ++m_stats.bypassed;
            if (initial == nullptr || initial_bytes >= desc.byte_width) {
                return Buffer(m_backend.create_buffer(desc, initial).release(),
                              Return{nullptr});
            }
            // create_buffer copies desc.byte_width bytes from its initial
            // data; only the first initial_bytes are the caller's.
            auto fresh = m_backend.create_buffer(desc, nullptr);
            if (!fresh || !fresh->update(0, initial, initial_bytes)) return {};
            return Buffer(fresh.release(), Return{nullptr});
        }
        trim_stale(now);
        const usize bytes = size_class(desc.byte_width);
        Bucket& bucket = bucket_for(flags_of(desc), bytes);
        if (!bucket.idle.empty() &&
            m_backend.generation_complete(bucket.idle.front().generation)) {
            graphics::BackendBuffer* buffer =
                bucket.idle.front().buffer.release();
            bucket.idle.pop_front();
            --m_stats.idle;
            m_stats.idle_bytes -= bytes;
            if (initial != nullptr && initial_bytes != 0 &&
                !buffer->update(0, initial, initial_bytes)) {
                delete buffer;
                return {};
            }
            ++m_stats.reused;
            if (graphics::slim_record::count_tick(m_stats.reused)) {
                std::fprintf(stderr,
                             "[wpr2] gs-ring reused=%llu created=%llu "
                             "idle=%zu\n",
                             static_cast<unsigned long long>(m_stats.reused),
                             static_cast<unsigned long long>(m_stats.created),
                             m_stats.idle);
            }
            return Buffer(buffer, Return{this});
        }
        graphics::BackendBufferDesc sized = desc;
        sized.byte_width = bytes;
        auto fresh = m_backend.create_buffer(sized, nullptr);
        if (!fresh) return {};
        if (initial != nullptr && initial_bytes != 0 &&
            !fresh->update(0, initial, initial_bytes)) {
            return {};
        }
        ++m_stats.created;
        return Buffer(fresh.release(), Return{this});
    }

    [[nodiscard]] const Stats& stats() const noexcept { return m_stats; }

private:
    struct Idle {
        std::unique_ptr<graphics::BackendBuffer> buffer;
        u64 generation{0};  ///< recording generation when it was returned
    };
    struct Bucket {
        u32 flags{0};
        usize bytes{0};
        std::deque<Idle> idle;  ///< in return order: generations ascend
    };

    [[nodiscard]] static u32 flags_of(
        const graphics::BackendBufferDesc& desc) noexcept {
        return (desc.vertex_buffer ? 1u : 0u) | (desc.index_buffer ? 2u : 0u) |
               (desc.shader_resource ? 4u : 0u) |
               (desc.cpu_writable ? 8u : 0u) |
               (desc.unordered_access ? 16u : 0u) |
               (desc.stream_output ? 32u : 0u);
    }

    Bucket& bucket_for(u32 flags, usize bytes) {
        for (Bucket& bucket : m_buckets) {
            if (bucket.flags == flags && bucket.bytes == bytes) return bucket;
        }
        m_buckets.push_back(Bucket{flags, bytes, {}});
        return m_buckets.back();
    }

    /// Not noexcept: keeping the buffer may allocate (a bucket, a deque
    /// block). Called from the deleter, so an allocation failure terminates,
    /// as a failed operator new does everywhere else on this path.
    void give(graphics::BackendBuffer* buffer) {
        // Work recorded from now on is a later generation than everything
        // this draw recorded with the buffer.
        const u64 generation = m_backend.recording_generation();
        const auto& desc = buffer->desc();
        if (generation == 0) {
            delete buffer;
            return;
        }
        Bucket& bucket = bucket_for(flags_of(desc), desc.byte_width);
        bucket.idle.push_back(Idle{
            std::unique_ptr<graphics::BackendBuffer>(buffer), generation});
        ++m_stats.idle;
        m_stats.idle_bytes += desc.byte_width;
        while (m_stats.idle > kMaxIdle || m_stats.idle_bytes > kMaxIdleBytes) {
            evict_oldest();
        }
    }

    void evict_oldest() noexcept {
        Bucket* oldest = nullptr;
        for (Bucket& bucket : m_buckets) {
            if (bucket.idle.empty()) continue;
            if (oldest == nullptr || bucket.idle.front().generation <
                                         oldest->idle.front().generation) {
                oldest = &bucket;
            }
        }
        if (oldest == nullptr) return;
        // Freeing an idle buffer is safe in flight too: the GPU work that
        // still uses it holds its own reference to the backing resource.
        oldest->idle.pop_front();
        --m_stats.idle;
        m_stats.idle_bytes -= oldest->bytes;
    }

    /// Frees only (pop_front), never allocates. Walks the buckets at most
    /// once per kStaleGenerations / 8 submissions, so a buffer is freed
    /// between kStaleGenerations and 1.125x that after its return.
    void trim_stale(u64 now) noexcept {
        if (now <= kStaleGenerations || now < m_next_trim) return;
        m_next_trim = now + kStaleGenerations / 8;
        for (Bucket& bucket : m_buckets) {
            while (!bucket.idle.empty() &&
                   bucket.idle.front().generation + kStaleGenerations < now) {
                bucket.idle.pop_front();
                --m_stats.idle;
                m_stats.idle_bytes -= bucket.bytes;
            }
        }
    }

    graphics::GraphicsBackendDevice& m_backend;
    std::vector<Bucket> m_buckets;
    u64 m_next_trim{0};  ///< trim_stale's next walk, in generations
    Stats m_stats{};
};

}  // namespace vitreo::winapi

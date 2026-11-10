// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file compile_profile.hpp
/// @brief Process-wide totals of where shader and pipeline creation spends
///        its time, by phase: DXIL lowering, DXBC -> SPIR-V, SPIR-V -> MSL,
///        Metal library compiles, pipeline-state creation, and the persistent
///        cache's hits and misses.
///
/// Always counted (a relaxed atomic add per phase, nothing per instruction),
/// read by the pipeline replay benchmark (tests/tools/d3d12_pso_replay) and
/// by tests. Each phase also keeps how many of it ran at once at most, and
/// the integral of that count over time, so a run's achieved concurrency is
/// busy_ns / wall_ns.

#include <vitreo/common.hpp>

#include <atomic>
#include <chrono>

namespace vitreo::graphics::compile_profile {

enum Phase : u8 {
    DxilLower,
    DxbcToSpirv,
    SpirvToMsl,
    MetalLibrary,
    MetalRenderState,
    MetalComputeState,
    kPhaseCount
};

/// The cache lookups (graphics/shaders/translation_cache.hpp).
enum CacheEvent : u8 {
    MslHit,
    MslMiss,
    DxilHit,
    DxilMiss,
    MetalRenderHit,
    MetalRenderMiss,
    SamplerBufferHit,
    SamplerBufferMiss,
    kCacheEventCount
};

/// D3D12 samplers by kind: what a descriptor-table shader's sampler-state
/// records will ask of its sample instructions (a MINIMUM_ / MAXIMUM_
/// reduction runs the out-of-line helper of dxbc_to_spirv_minmax_sample.cpp).
enum SamplerEvent : u8 {
    SamplerCreated,        ///< CreateSampler / CreateSampler2
    SamplerMinMax,         ///< ... with a MINIMUM_ / MAXIMUM_ filter
    StaticSamplerCreated,  ///< root-signature static samplers
    StaticSamplerMinMax,   ///< ... with a MINIMUM_ / MAXIMUM_ filter
    SamplerRecordBorderTwin, ///< packed records that need custom-border sampling
    SamplerRecordNonNormalized, ///< packed records using texel coordinates
    kSamplerEventCount
};

struct Totals {
    std::atomic<u64> count[kPhaseCount]{};
    std::atomic<u64> ns[kPhaseCount]{};
    std::atomic<u64> in_flight[kPhaseCount]{};
    std::atomic<u64> max_in_flight[kPhaseCount]{};
    std::atomic<u64> cache[kCacheEventCount]{};
    std::atomic<u64> samplers[kSamplerEventCount]{};
};

inline Totals& totals() {
    static Totals* t = new Totals();
    return *t;
}

[[nodiscard]] inline u64 now_ns() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

inline void begin(Phase phase) {
    Totals& t = totals();
    const u64 now = t.in_flight[phase].fetch_add(1, std::memory_order_relaxed) + 1;
    u64 seen = t.max_in_flight[phase].load(std::memory_order_relaxed);
    while (now > seen &&
           !t.max_in_flight[phase].compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
    }
}

inline void end(Phase phase, u64 ns) {
    Totals& t = totals();
    t.in_flight[phase].fetch_sub(1, std::memory_order_relaxed);
    t.count[phase].fetch_add(1, std::memory_order_relaxed);
    t.ns[phase].fetch_add(ns, std::memory_order_relaxed);
}

inline void note(CacheEvent event) {
    totals().cache[event].fetch_add(1, std::memory_order_relaxed);
}

inline void note(SamplerEvent event) {
    totals().samplers[event].fetch_add(1, std::memory_order_relaxed);
}

/// Times one phase from construction to destruction.
class Scope {
public:
    explicit Scope(Phase phase) : m_phase(phase), m_t0(now_ns()) { begin(phase); }
    ~Scope() { end(m_phase, now_ns() - m_t0); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Phase m_phase;
    u64 m_t0;
};

/// A copy of the totals (for differences between two points).
struct Snapshot {
    u64 count[kPhaseCount]{};
    u64 ns[kPhaseCount]{};
    u64 max_in_flight[kPhaseCount]{};
    u64 cache[kCacheEventCount]{};
    u64 samplers[kSamplerEventCount]{};
};

[[nodiscard]] inline Snapshot snapshot() {
    Snapshot s;
    Totals& t = totals();
    for (int p = 0; p < kPhaseCount; ++p) {
        s.count[p] = t.count[p].load(std::memory_order_relaxed);
        s.ns[p] = t.ns[p].load(std::memory_order_relaxed);
        s.max_in_flight[p] = t.max_in_flight[p].load(std::memory_order_relaxed);
    }
    for (int e = 0; e < kCacheEventCount; ++e) {
        s.cache[e] = t.cache[e].load(std::memory_order_relaxed);
    }
    for (int e = 0; e < kSamplerEventCount; ++e) {
        s.samplers[e] = t.samplers[e].load(std::memory_order_relaxed);
    }
    return s;
}

/// Restart the max-in-flight high-water marks.
inline void reset_max_in_flight() {
    for (auto& m : totals().max_in_flight) m.store(0, std::memory_order_relaxed);
}

}  // namespace vitreo::graphics::compile_profile

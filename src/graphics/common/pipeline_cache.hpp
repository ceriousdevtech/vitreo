// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file pipeline_cache.hpp
/// @brief Fast hash-based pipeline state cache

#include <vitreo/common.hpp>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace vitreo::graphics {

/// Fast hash-based pipeline state cache.
/// Frontends hash their current render state -> u64 key.
/// The cache maps key -> compiled pipeline state (void* = id<MTLRenderPipelineState>).
class PipelineCache {
public:
    PipelineCache() = default;
    ~PipelineCache() = default;

    /// Look up a cached pipeline. Returns nullptr on miss.
    void* find(u64 key) const;

    /// Insert a compiled pipeline state.
    void insert(u64 key, void* pso);

    /// Remove a specific entry.
    void erase(u64 key);

    /// Clear all entries.
    void clear();

    usize size() const;

    /// Serialization (for disk cache warm-start)
    struct CacheEntry {
        u64 key;
        std::vector<u8> msl_source;  // The MSL that produced this PSO
    };
    std::vector<CacheEntry> serialize() const;

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<u64, void*> m_cache;
    std::unordered_map<u64, std::vector<u8>> m_msl_sources;
};

} // namespace vitreo::graphics

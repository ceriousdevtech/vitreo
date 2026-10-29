// SPDX-License-Identifier: LGPL-2.1-only

/// @file pipeline_cache.cpp
/// @brief Pipeline state cache implementation

#include "pipeline_cache.hpp"

namespace vitreo::graphics {

void* PipelineCache::find(u64 key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_cache.find(key);
    if (it != m_cache.end()) {
        return it->second;
    }
    return nullptr;
}

void PipelineCache::insert(u64 key, void* pso) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.emplace(key, pso);
}

void PipelineCache::erase(u64 key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.erase(key);
    m_msl_sources.erase(key);
}

void PipelineCache::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
    m_msl_sources.clear();
}

usize PipelineCache::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cache.size();
}

std::vector<PipelineCache::CacheEntry> PipelineCache::serialize() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<CacheEntry> entries;
    entries.reserve(m_cache.size());
    for (const auto& [key, _] : m_cache) {
        CacheEntry entry;
        entry.key = key;
        auto src_it = m_msl_sources.find(key);
        if (src_it != m_msl_sources.end()) {
            entry.msl_source = src_it->second;
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}

} // namespace vitreo::graphics

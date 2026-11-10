// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

// Pipelines rebuild when a previously absent sampler feature becomes needed.
// A state already recorded by another command remains retained. A failed
// required rebuild disables new uses; using the old state would render the
// wrong sampler behavior.
#include <vitreo/common.hpp>
#include "graphics/shaders/sampler_features.hpp"
#include <atomic>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vitreo::graphics {
std::shared_ptr<const std::string> intern_msl_source(const std::string& text);
void release_retained_metal_object(void* object);

class SamplerFeatureRebuild {
public:
    // The factory returns one retained state. Feature bits never turn off;
    // the initial snapshot is a lower bound on what that state supports.
    SamplerFeatureRebuild(unsigned interests, unsigned compiled,
                          std::function<void*()> rebuild)
        : m_interests(interests), m_compiled(compiled & interests),
          m_rebuild(std::move(rebuild)) {}
    ~SamplerFeatureRebuild() {
        for (void* state : m_retired) release_retained_metal_object(state);
    }
    SamplerFeatureRebuild(const SamplerFeatureRebuild&) = delete;
    SamplerFeatureRebuild& operator=(const SamplerFeatureRebuild&) = delete;

    void* current(std::atomic<void*>& state,
                  unsigned enabled = enabled_sampler_features()) {
        unsigned compiled = m_compiled.load(std::memory_order_acquire);
        if (compiled == kFailed) return nullptr;
        const unsigned needed = enabled & m_interests;
        if ((needed & ~compiled) != 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            compiled = m_compiled.load(std::memory_order_relaxed);
            if (compiled == kFailed) return nullptr;
            if ((needed & ~compiled) != 0) {
                void* fresh = m_rebuild ? m_rebuild() : nullptr;
                if (void* old = state.exchange(fresh, std::memory_order_acq_rel))
                    m_retired.push_back(old);
                if (fresh == nullptr) {
                    m_rebuild = nullptr;
                    m_compiled.store(kFailed, std::memory_order_release);
                    std::fprintf(stderr,
                        "[vitreo] UNIMPLEMENTED metal!pipeline: rebuild for sampler "
                        "features 0x%x failed; pipeline disabled\n", needed);
                    return nullptr;
                }
                compiled |= needed;
                if ((compiled & m_interests) == m_interests) m_rebuild = nullptr;
                m_compiled.store(compiled, std::memory_order_release);
            }
        }
        return state.load(std::memory_order_acquire);
    }

private:
    static constexpr unsigned kFailed = ~0u;
    const unsigned m_interests;
    std::atomic<unsigned> m_compiled;
    std::mutex m_mutex;
    std::function<void*()> m_rebuild;
    std::vector<void*> m_retired;
};
} // namespace vitreo::graphics

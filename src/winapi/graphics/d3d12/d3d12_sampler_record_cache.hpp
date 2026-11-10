// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

#include "graphics/contracts/backend_device.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi::d3d12 {

/// Replay-owned immutable sampler-record storage, for one backend/device.
/// Lookup happens only AFTER the current descriptors have been resolved and
/// packed. Exact word equality decides reuse; descriptor identities do not.
/// The replay lock serializes access. Eviction drops only the cache reference;
/// callers and generation-retirement holders own their references separately.
class SamplerRecordBufferCache {
public:
    using Buffer = std::shared_ptr<const graphics::BackendBuffer>;

    explicit SamplerRecordBufferCache(usize max_entries = 128,
                                      usize max_bytes = 4u * 1024u * 1024u)
        : m_max_entries(max_entries), m_max_bytes(max_bytes) {}

    [[nodiscard]] Buffer find(const std::vector<u32>& words) {
        if (!fits(words)) return {};
        const auto found = m_entries.find(words);
        if (found == m_entries.end()) return {};
        found->second.used = ++m_clock;
        return found->second.buffer;
    }

    /// The caller guarantees these immutable bytes initialized @p buffer.
    /// False means not retained, not that a valid caller buffer is unusable.
    [[nodiscard]] bool remember(const std::vector<u32>& words, Buffer buffer) {
        if (!fits(words) || !buffer || !buffer->valid() ||
            buffer->desc().byte_width != words.size() * sizeof(u32)) return false;
        if (const auto found = m_entries.find(words); found != m_entries.end()) {
            found->second.used = ++m_clock;
            return true;
        }
        // Count both the owned key and the logical GPU payload. Entry count
        // also bounds small-buffer alignment and wrapper overhead.
        const usize cost = words.size() * sizeof(u32) * 2u;
        while (!m_entries.empty() &&
               (m_entries.size() >= m_max_entries || m_bytes > m_max_bytes - cost)) {
            auto oldest = m_entries.begin();
            for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
                if (it->second.used < oldest->second.used) oldest = it;
            }
            m_bytes -= oldest->second.bytes;
            m_entries.erase(oldest);
        }
        m_entries.emplace(words, Entry{std::move(buffer), ++m_clock, cost});
        m_bytes += cost;
        return true;
    }

    [[nodiscard]] usize size() const noexcept { return m_entries.size(); }
    [[nodiscard]] usize retained_bytes() const noexcept { return m_bytes; }

private:
    struct WordHash {
        usize operator()(const std::vector<u32>& words) const noexcept {
            u64 hash = 1469598103934665603ull;
            for (u32 word : words) hash = (hash ^ word) * 1099511628211ull;
            return static_cast<usize>(hash);
        }
    };
    struct Entry {
        Buffer buffer;
        u64 used{0};
        usize bytes{0};
    };
    [[nodiscard]] bool fits(const std::vector<u32>& words) const noexcept {
        return m_max_entries != 0 && !words.empty() &&
               words.size() <= m_max_bytes / (2u * sizeof(u32));
    }
    usize m_max_entries;
    usize m_max_bytes;
    usize m_bytes{0};
    u64 m_clock{0};
    std::unordered_map<std::vector<u32>, Entry, WordHash> m_entries;
};

} // namespace vitreo::winapi::d3d12

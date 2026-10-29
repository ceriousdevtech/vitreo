#pragma once

/// @file jit_region_registry.hpp
/// @brief Global registry of JIT code slab address ranges.
///
/// macOS ARM64 can hand MAP_JIT and MAP_ANONYMOUS mappings at
/// overlapping virtual addresses.  Both the JIT allocator and the guest
/// memory allocator consult this registry so that neither stomps on
/// the other's pages.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace vitreo {

class JitRegionRegistry {
public:
    static JitRegionRegistry& instance() {
        static JitRegionRegistry s;
        return s;
    }

    /// Register a newly-allocated JIT slab.
    void add(uintptr_t start, uintptr_t end) {
        std::lock_guard lk(mu_);
        ranges_.push_back({start, end});
    }

    /// Remove a previously-registered JIT slab by its start address.
    void remove(uintptr_t start) {
        std::lock_guard lk(mu_);
        for (auto it = ranges_.begin(); it != ranges_.end(); ++it) {
            if (it->start == start) {
                ranges_.erase(it);
                return;
            }
        }
    }

    /// Check whether [start, end) overlaps any registered JIT slab.
    bool overlaps(uintptr_t start, uintptr_t end) const {
        std::lock_guard lk(mu_);
        for (auto& r : ranges_) {
            if (start < r.end && end > r.start)
                return true;
        }
        return false;
    }

    /// Return the highest end address of any registered slab (useful as
    /// a hint to mmap guest memory past the JIT region).
    uintptr_t max_end() const {
        std::lock_guard lk(mu_);
        uintptr_t m = 0;
        for (auto& r : ranges_)
            if (r.end > m) m = r.end;
        return m;
    }

private:
    JitRegionRegistry() = default;
    struct Range { uintptr_t start; uintptr_t end; };
    mutable std::mutex mu_;
    std::vector<Range> ranges_;
};

} // namespace vitreo

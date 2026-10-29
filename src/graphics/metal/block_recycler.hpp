// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file block_recycler.hpp
/// @brief A process-wide cache of large allocations, and the allocator that
///        routes a container's large allocations through it.
///
/// The Metal submit queue records its op lists into a std::deque<Op>. An Op
/// is ~3.4 KB, so libc++ gives the deque 16-op blocks of ~54 KB: each one a
/// large-size-class malloc that maps and accounts its pages
/// (mach_vm_reclaim), made on the recording thread and freed on the encode
/// thread, several hundred times a frame -- ~4% of the render thread at a
/// the menu. The blocks all have one size, so the thread that frees
/// one gives it back here and the next recording takes it.
///
/// By default only allocations of at least kMinBytes are cached; malloc's own
/// caches serve smaller ones well, except when one thread allocates and
/// another frees, a fixed size, thousands of times a frame (the per-draw
/// push constants), which opt in with a lower threshold. A cached block
/// satisfies only a request of exactly its own size, and at most
/// kMaxCachedBytes are held.

#include <vitreo/ab_cycle.hpp>
#include <vitreo/common.hpp>

#include <array>
#include <cstddef>
#include <mutex>
#include <new>
#include <vector>

namespace vitreo::graphics {

class BlockRecycler {
public:
    static constexpr usize kMinBytes = 16 * 1024;
    static constexpr usize kMaxCachedBytes = usize{64} << 20;

    struct Stats {
        u64 fresh{0};     ///< cacheable requests no cached block satisfied
        u64 reused{0};    ///< requests served from the cache
        u64 released{0};  ///< blocks freed because the cache was full
        usize cached_bytes{0};
    };

    /// A cached block of exactly @p bytes, or nullptr.
    [[nodiscard]] static void* take(usize bytes) noexcept {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mu);
        for (Bin& bin : s.bins) {
            if (bin.bytes != bytes || bin.blocks.empty()) continue;
            void* const block = bin.blocks.back();
            bin.blocks.pop_back();
            s.cached -= bytes;
            ++s.reused;
            return block;
        }
        ++s.fresh;
        return nullptr;
    }

    /// Keep @p block for reuse. False: the cache is full, or has no bin for
    /// this size, and the caller frees the block itself.
    [[nodiscard]] static bool give(void* block, usize bytes) noexcept {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mu);
        if (s.cached + bytes > kMaxCachedBytes) {
            ++s.released;
            return false;
        }
        // Its own bin, else one never used, else any empty one: a bin that
        // is empty only because all its blocks are out in a recording keeps
        // its size while a never-used bin is free.
        Bin* home = nullptr;
        Bin* unused = nullptr;
        Bin* empty = nullptr;
        for (Bin& bin : s.bins) {
            if (bin.bytes == bytes) { home = &bin; break; }
            if (bin.bytes == 0 && unused == nullptr) unused = &bin;
            if (bin.blocks.empty() && empty == nullptr) empty = &bin;
        }
        if (home == nullptr) home = unused != nullptr ? unused : empty;
        if (home == nullptr) {
            ++s.released;
            return false;
        }
        // Built without exceptions: an allocation failure here aborts, as
        // any other operator new failure does.
        home->blocks.push_back(block);
        home->bytes = bytes;
        s.cached += bytes;
        return true;
    }

    [[nodiscard]] static Stats stats() noexcept {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mu);
        return Stats{s.fresh, s.reused, s.released, s.cached};
    }

    /// Tests only: free every cached block.
    static void drain_for_test() noexcept {
        State& s = state();
        std::lock_guard<std::mutex> lock(s.mu);
        for (Bin& bin : s.bins) {
            for (void* block : bin.blocks) ::operator delete(block);
            bin.blocks.clear();
            bin.bytes = 0;
        }
        s.cached = 0;
    }

private:
    struct Bin {
        usize bytes{0};
        std::vector<void*> blocks;
    };
    struct State {
        std::mutex mu;
        std::array<Bin, 8> bins;
        usize cached{0};
        u64 fresh{0};
        u64 reused{0};
        u64 released{0};
    };
    /// Never destroyed: a list may still be freed during static destruction.
    static State& state() noexcept {
        static State* const s = new State();
        return *s;
    }
};

/// std::allocator-shaped adapter: allocations of at least @p MinBytes go
/// through the recycler, the rest to operator new. Stateless, so every
/// instance compares equal and a container moves between threads in O(1).
/// Under VITREO_AB_CYCLE_S the "off" arm is plain operator new and delete.
template <class T, usize MinBytes = BlockRecycler::kMinBytes>
struct RecyclingAllocator {
    using value_type = T;
    template <class U>
    struct rebind {
        using other = RecyclingAllocator<U, MinBytes>;
    };
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__,
                  "blocks come from the default-aligned operator new");

    RecyclingAllocator() noexcept = default;
    template <class U>
    RecyclingAllocator(const RecyclingAllocator<U, MinBytes>&) noexcept {}

    [[nodiscard]] T* allocate(usize n) {
        const usize bytes = n * sizeof(T);
        if (bytes >= MinBytes &&
            !diagnostics::ab_cycle_off("recycler")) {
            if (void* block = BlockRecycler::take(bytes)) {
                return static_cast<T*>(block);
            }
        }
        return static_cast<T*>(::operator new(bytes));
    }

    void deallocate(T* p, usize n) noexcept {
        const usize bytes = n * sizeof(T);
        if (bytes >= MinBytes &&
            !diagnostics::ab_cycle_off("recycler") &&
            BlockRecycler::give(p, bytes)) {
            return;
        }
        ::operator delete(p);
    }

    template <class U>
    bool operator==(const RecyclingAllocator<U, MinBytes>&) const noexcept {
        return true;
    }
};

}  // namespace vitreo::graphics

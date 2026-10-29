// SPDX-License-Identifier: LGPL-2.1-only
/// @file metal_buffer.mm
/// @brief Metal GPU buffer implementation

#import <Metal/Metal.h>
#include "graphics/metal/metal_buffer.hpp"
#include "graphics/metal/metal_submit_queue.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace vitreo::graphics {

namespace {

// -- The DISCARD pool -------------------------------------------------------
//
// Learn ("How to: Use dynamic resources", "Using Dynamic Vertex and Index
// Buffers"): a DISCARD of a buffer the GPU is still using returns "a new
// region of memory ... This allows the GPU to continue using the old data
// ... the old buffer is reused or destroyed automatically when the GPU is
// finished with it." A plain write (and UpdateSubresource) must look the same
// to work already recorded. This pool is that renaming memory: renames and
// ordered-upload staging bump-allocate slices from shared slabs.
//
// A slab is reused when the GPU has retired every generation that may read
// it -- checked when an allocation needs space, not at Present. Each slab is
// tagged with the highest such generation: one past the recording generation
// when a slice is handed out, pinned or bound (see kTagMargin). When no slab
// is free and the pool is at its bound, the allocation waits for the oldest
// generation still reading a slab (MetalSubmitQueue::wait_for_generation) and
// retries; it never hands back storage the GPU may read, and the caller never
// falls back to writing in place.
//
// History. This used to be a ring of three per-Present slots of up to 32
// 16 MiB slabs each, recycled only at a Present and only if the slot's
// generation had retired by then. A buffer renamed once and then bound in
// every list re-tagged its slot each time, so with the GPU a list or two
// behind the slot was never recycled, every third frame's renames piled
// into it, and at 32 slabs DISCARD wrote in place under unexecuted draws
// (3 of 67 runs, with no command buffer slower than 31 ms).
// Before that the ring recycled on the Present count alone and a 32-bit
// title drew geometry "at infinity" when it wrapped onto in-flight vertices.

constexpr usize kMiB = 1024u * 1024u;
/// The smallest slab. A slab is max(16 MiB, 4 x the rename that opened it),
/// so a rename that does not fit the current slab's tail strands at most a
/// quarter of a slab, whatever its size.
constexpr usize kDiscardSlabBytes = 16u * kMiB;
/// The pool's bound. Live renaming memory is the DISCARD traffic of the
/// work the GPU has not retired. A Present waits until the Present two
/// frames back has retired (MetalSubmitQueue::kPresentLatency), so about
/// three frames are in flight; 256 MiB holds ~60-80 MiB of renames per frame
/// across them (measured: the menu peaks at 32 MiB with no
/// waits). The old ring could reach 3 x 32 x 16 MiB = 1.5 GiB of shared
/// memory, which also pushes the system into swap. A frame that needs more
/// waits for the oldest in-flight generation instead of growing.
constexpr usize kDiscardPoolMaxBytes = 256u * kMiB;
/// A rename above this gets an oversize slab of its own size instead of a 4x
/// slab. At 1/12 of the bound (~21 MiB) two 4x slabs of the largest such
/// rename take 2/3 of it, leaving room for the 16 MiB slabs steady small
/// traffic keeps live, so renames up to it can alternate between slabs while
/// the GPU lags. (At 1/8, 32 MiB renames' two 128 MiB slabs filled the bound
/// alone and any live small slab made every rename wait.)
constexpr usize kOversizeRename = kDiscardPoolMaxBytes / 12u;
/// Oversize slabs have their own bound. Past it an oversize rename waits for
/// the oldest oversize slab to retire rather than allocating another.
constexpr usize kOversizeMaxBytes = 512u * kMiB;
/// Slab records: the 16 smallest slabs the bound allows, and in-flight
/// oversize slabs.
constexpr u32 kDiscardSlabEntries = 48;
constexpr usize kDiscardAlign = 256;
/// Presents a reclaimable slab may stay untouched before it is released.
constexpr u32 kDiscardIdlePresents = 120;
/// Presents a retired oversize slab may stay untouched before it is released.
/// Not one: an oversize buffer (over ~21 MiB) DISCARDed every other frame
/// would free and reallocate its slab each time (estimated 1-3 ms of
/// render-thread time per 25 MiB). Eight Presents (~130 ms at 60 fps) keep a slab for buffers
/// renamed every few frames; kOversizeMaxBytes still caps the memory.
constexpr u32 kOversizeIdlePresents = 8;
constexpr u32 kNoSlab = std::numeric_limits<u32>::max();
/// Slab tags are one generation past the recording generation of the take,
/// pin or bind. A tag records the list the work is being recorded into, but
/// the work lands in that list only if nothing submits it between the bind
/// and the append -- and D3D9's Present releases its lock across the submit,
/// so another guest thread's draw can be split across it.
///  - Enough for D3D9 alone: one Present's submit falls in that window.
///  - Not enough for a process recording D3D9 and D3D11 at once: the other
///    API's thread can submit any number of lists in the window. Two or more
///    submits there are not covered (nor are buffer-level tags such as
///    m_owned_gen, or the backend's other generation tags).
///  - Cost: every slab stays live one list longer, so about one more list of
///    renaming memory is held; and an exhaustion wait may now submit the list
///    being recorded mid-recording, and give it (and the list after it) a
///    small fill of their own, to retire tags that run past it (advance_to).
constexpr u64 kTagMargin = 1;

/// Test seam (metal_discard_pool_fail_allocations_for_test).
std::atomic<bool> g_fail_allocations{false};

[[nodiscard]] usize align_up(usize value, usize align) {
    return (value + align - 1u) & ~(align - 1u);
}

/// Raise @p gen to at least @p value. seq_cst: see DiscardPool's reclaim
/// handshake.
void raise_to(std::atomic<u64>& gen, u64 value) {
    u64 seen = gen.load(std::memory_order_seq_cst);
    while (seen < value &&
           !gen.compare_exchange_weak(seen, value, std::memory_order_seq_cst,
                                      std::memory_order_seq_cst)) {
    }
}

struct UnfairLockGuard {
    explicit UnfairLockGuard(os_unfair_lock& lock) : m_lock(lock) {
        os_unfair_lock_lock(&m_lock);
    }
    ~UnfairLockGuard() { os_unfair_lock_unlock(&m_lock); }
    UnfairLockGuard(const UnfairLockGuard&) = delete;
    UnfairLockGuard& operator=(const UnfairLockGuard&) = delete;
    os_unfair_lock& m_lock;
};

struct DiscardSlab {
    void* buffer{nullptr};  // retained id<MTLBuffer>; null: free record
    u8* contents{nullptr};
    usize capacity{0};
    usize used{0};
    /// Highest generation that may read a slice handed out since the slab
    /// was last reset. Raised without the pool lock by pins and binds.
    std::atomic<u64> gen{0};
    /// Bumped by every reclaim attempt (reset or release), before it
    /// re-checks gen. A holder that sees it move checks resets.
    std::atomic<u64> epoch{0};
    /// Reclaims that went through (reset or release), under the pool lock.
    std::atomic<u64> resets{0};
    /// Outside the bound: one rename of more than kOversizeRename.
    bool oversize{false};
    /// Allocated from since the last Present.
    bool touched{false};
    /// Consecutive Presents the slab was reclaimable and untouched.
    u32 idle{0};
};

struct DiscardSlice {
    void* buffer{nullptr};
    usize offset{0};
    u8* cpu{nullptr};
    u32 slab{kNoSlab};
    u64 epoch{0};
    u64 resets{0};
    /// The recording generation the slab was tagged for (tag - margin).
    u64 recording{0};
};

enum class PinResult : u8 {
    /// The slice is still the holder's and is pinned.
    Pinned,
    /// A reclaim was attempted and abandoned since the slice was handed out
    /// (the new epoch is returned); the slice is still the holder's, pinned.
    Revalidated,
    /// The slab was reclaimed: the holder must not touch the slice again.
    Reclaimed,
};

/// One pool for the process, its allocation and reclaim under one lock.
///
/// Reclaim safety does not rest on a caller lock: binds run on threads that
/// hold no lock the reclaimer holds (the D3D9 command-stream worker binds
/// while D3D9's API thread presents with its device lock released, and that
/// Present trims the pool; D3D9 and D3D11 can share a process). Instead:
///  - Every reset or release goes through claim(): read gen <= done, bump the
///    slab's epoch, re-read gen, and back off if it rose (all seq_cst); a
///    claim that goes through then bumps resets.
///  - A buffer keeping a slice pins it first (MetalBuffer::handle, and before
///    a NO_OVERWRITE write into it): raise gen past the recording generation,
///    then read the epoch. Unchanged: pinned. Changed: under the pool lock
///    (no claim in progress then) the reset count says whether the slice is
///    still the holder's (every claim since was abandoned) or not.
///  - In the single total order of those seq_cst operations either the
///    claim's re-read sees the pin (the claim backs off, the slice stays
///    valid) or the pin's epoch read sees the bump.
///  - Leaving a reclaimed slice is always possible. A claim proceeds only
///    past gen <= done, and gen is never below the m_owned_gen of any buffer
///    naming a slice in the slab: take() tags the slab past the recording
///    generation, which is at least the renamed buffer's m_owned_gen, frozen
///    while the slice is published. So the owned storage has retired.
///  - A successful pin holds until the list it was taken for has retired --
///    and that list is still being recorded, so the bind it serves (appended
///    before the list is submitted) is covered.
class DiscardPool {
public:
    /// Leaked, like MetalSubmitQueue: slabs are referenced by work that may
    /// still be in flight at static destruction.
    static DiscardPool& instance() {
        static DiscardPool* pool = new DiscardPool();
        return *pool;
    }

    /// A slice of @p bytes no work recorded so far reads. Waits for the GPU
    /// when the pool (or the oversize bound) is full. False only when Metal
    /// refuses the slab, every slab record is in use, this thread is
    /// capturing a command list, or the queue has shut down; the caller then
    /// waits for its own storage.
    [[nodiscard]] bool allocate(void* device, usize bytes, DiscardSlice& out) {
        if (device == nullptr || bytes == 0) return false;
        if (g_fail_allocations.load(std::memory_order_relaxed)) return false;
        const usize need = align_up(bytes, kDiscardAlign);
        const bool oversize = need > kOversizeRename;
        auto& queue = MetalSubmitQueue::instance();
        std::unique_lock<std::mutex> lock(m_mu);
        for (;;) {
            const u64 done = queue.completed_generation();
            const u64 tag = queue.recording_generation() + kTagMargin;
            // 1. The slab being filled.
            if (!oversize && m_current != kNoSlab &&
                take(m_current, need, tag, out)) {
                return true;
            }
            // 2. A slab every reader of which has retired, that fits this
            // rename four times over (or the whole bound) -- so reuse does not
            // strand tails either. The lowest such: traffic settles into few
            // slabs and the rest go idle (trim).
            const usize good_fit =
                oversize ? need : std::min(4u * need, kDiscardPoolMaxBytes);
            if (reuse(good_fit, oversize, done, need, tag, out)) return true;
            u64 oldest = std::numeric_limits<u64>::max();
            if (oversize) {
                // 3. Grow an oversize slab, first releasing retired ones too
                // small to serve, unless the oversize bound is reached.
                for (u32 i = 0; i < kDiscardSlabEntries; ++i) {
                    if (m_slabs[i].buffer != nullptr && m_slabs[i].oversize &&
                        claim(i, done)) {
                        release(i);
                    }
                }
                const usize capacity = align_up(need, kMiB);
                if (m_oversize_bytes == 0 ||
                    m_oversize_bytes + capacity <= kOversizeMaxBytes) {
                    const u32 entry = free_entry();
                    if (entry == kNoSlab ||
                        !create(device, entry, capacity, true)) {
                        return false;
                    }
                    return take(entry, need, tag, out);
                }
                oldest = oldest_tag(true, done);
            } else {
                const usize capacity = std::max(kDiscardSlabBytes, 4u * need);
                // 3. Grow. Release retired slabs for room only if that makes
                // enough of it; otherwise keep them for 3b.
                usize retired = 0;
                for (const DiscardSlab& slab : m_slabs) {
                    if (slab.buffer != nullptr && !slab.oversize &&
                        slab.gen.load(std::memory_order_seq_cst) <= done) {
                        retired += slab.capacity;
                    }
                }
                if (m_bytes - retired + capacity <= kDiscardPoolMaxBytes) {
                    for (u32 i = 0; i < kDiscardSlabEntries &&
                                    m_bytes + capacity > kDiscardPoolMaxBytes;
                         ++i) {
                        if (m_slabs[i].buffer != nullptr &&
                            !m_slabs[i].oversize && claim(i, done)) {
                            release(i);
                        }
                    }
                }
                if (m_bytes + capacity <= kDiscardPoolMaxBytes) {
                    const u32 entry = free_entry();
                    if (entry != kNoSlab) {
                        if (!create(device, entry, capacity, false)) {
                            return false;
                        }
                        m_current = entry;
                        return take(entry, need, tag, out);
                    }
                }
                // 3b. No room to grow: any retired slab the rename fits.
                if (reuse(need, false, done, need, tag, out)) return true;
                oldest = oldest_tag(false, done);
            }
            // 4. Out of renaming memory: wait for the oldest generation still
            // reading a slab of this bound, then reclaim it.
            if (oldest == std::numeric_limits<u64>::max()) return false;
            ++m_waits;
            if ((m_waits & (m_waits - 1)) == 0) {  // 1, 2, 4, 8, ...
                std::fprintf(stderr,
                             "[metal] DISCARD pool full at %zu MiB (%u slabs, "
                             "%zu MiB oversize): waiting for gen %llu "
                             "(completed %llu); %llu waits so far\n",
                             m_bytes >> 20, m_count, m_oversize_bytes >> 20,
                             static_cast<unsigned long long>(oldest),
                             static_cast<unsigned long long>(done),
                             static_cast<unsigned long long>(m_waits));
            }
            // Not under the pool lock: submit() can run the main thread's run
            // loop, and a Present from there trims the pool.
            //
            // Known theoretical hang: the generation waited for may hold a
            // GPU wait on a fence (ID3D11DeviceContext4::Wait, D3D12) whose
            // value only this thread would signal later from the CPU. It
            // needs the pool exhausted by renames recorded behind that wait,
            // with the signal still to come; Windows' Map(WRITE_DISCARD) would
            // not block there. No such pattern has been seen.
            void* const scratch = scratch_buffer(device);
            lock.unlock();
            const bool progressed = advance_to(oldest, scratch);
            lock.lock();
            if (!progressed) return false;
        }
    }

    /// Pin a published slice in @p slab for work recorded in @p recording.
    /// @p epoch / @p resets: what the holder saw when it took or last pinned
    /// the slice; on Revalidated @p epoch is updated.
    [[nodiscard]] PinResult pin(u32 slab, u64 recording, u64& epoch,
                                u64 resets) {
        if (slab >= kDiscardSlabEntries) return PinResult::Reclaimed;
        DiscardSlab& entry = m_slabs[slab];
        raise_to(entry.gen, recording + kTagMargin);
        if (entry.epoch.load(std::memory_order_seq_cst) == epoch) {
            return PinResult::Pinned;
        }
        // A reclaim began since. Under the lock none is in progress, and the
        // pin above stops any later one, so the reset count is final.
        std::lock_guard<std::mutex> lock(m_mu);
        if (entry.resets.load(std::memory_order_seq_cst) != resets) {
            return PinResult::Reclaimed;
        }
        epoch = entry.epoch.load(std::memory_order_seq_cst);
        return PinResult::Revalidated;
    }

    /// A bind of a slice in @p slab by work recorded in @p recording.
    void note_read(u32 slab, u64 recording) {
        if (slab < kDiscardSlabEntries) {
            raise_to(m_slabs[slab].gen, recording + kTagMargin);
        }
    }

    /// Release slabs that have been reclaimable and unused for
    /// kDiscardIdlePresents Presents in a row (oversize slabs:
    /// kOversizeIdlePresents).
    void trim(u64 done) {
        std::lock_guard<std::mutex> lock(m_mu);
        for (u32 i = 0; i < kDiscardSlabEntries; ++i) {
            DiscardSlab& slab = m_slabs[i];
            if (slab.buffer == nullptr) continue;
            if (slab.touched ||
                slab.gen.load(std::memory_order_seq_cst) > done) {
                slab.touched = false;
                slab.idle = 0;
                continue;
            }
            const u32 limit =
                slab.oversize ? kOversizeIdlePresents : kDiscardIdlePresents;
            if (++slab.idle >= limit && claim(i, done)) release(i);
        }
    }

    [[nodiscard]] MetalDiscardPoolStats stats() {
        std::lock_guard<std::mutex> lock(m_mu);
        MetalDiscardPoolStats out;
        out.slabs = m_count;
        out.bytes = m_bytes;
        out.waits = m_waits;
        out.oversize_bytes = m_oversize_bytes;
        out.oversize_created = m_oversize_created;
        for (const DiscardSlab& slab : m_slabs) {
            if (slab.buffer != nullptr && slab.oversize) ++out.oversize_slabs;
        }
        return out;
    }

    /// Test seam (metal_discard_pool_abandon_claim_for_test): what a claim
    /// that saw a pin and backed off leaves behind.
    [[nodiscard]] bool abandon_claim_for_test(void* storage) {
        std::lock_guard<std::mutex> lock(m_mu);
        for (DiscardSlab& slab : m_slabs) {
            if (slab.buffer != storage || storage == nullptr) continue;
            slab.epoch.fetch_add(1, std::memory_order_seq_cst);
            return true;
        }
        return false;
    }

    /// Test seam (metal_discard_pool_reclaim_for_test).
    [[nodiscard]] bool reclaim_for_test(void* storage) {
        std::lock_guard<std::mutex> lock(m_mu);
        const u64 done = MetalSubmitQueue::instance().completed_generation();
        for (u32 i = 0; i < kDiscardSlabEntries; ++i) {
            DiscardSlab& slab = m_slabs[i];
            if (slab.buffer != storage || storage == nullptr) continue;
            if (!claim(i, done)) return false;
            reset(i);
            std::memset(slab.contents, 0xDD, slab.capacity);
            return true;
        }
        return false;
    }

private:
    /// Reset and take from the lowest retired slab of @p oversize kind with
    /// at least @p min_capacity.
    [[nodiscard]] bool reuse(usize min_capacity, bool oversize, u64 done,
                             usize need, u64 tag, DiscardSlice& out) {
        for (u32 i = 0; i < kDiscardSlabEntries; ++i) {
            DiscardSlab& slab = m_slabs[i];
            if (slab.buffer == nullptr || slab.oversize != oversize ||
                slab.capacity < min_capacity || !claim(i, done)) {
                continue;
            }
            reset(i);
            if (!oversize) m_current = i;
            if (take(i, need, tag, out)) return true;
        }
        return false;
    }

    /// Begin reclaiming slab @p index: true when no work up to @p done is
    /// still reading it and no holder pinned it meanwhile.
    [[nodiscard]] bool claim(u32 index, u64 done) {
        DiscardSlab& slab = m_slabs[index];
        if (slab.gen.load(std::memory_order_seq_cst) > done) return false;
        slab.epoch.fetch_add(1, std::memory_order_seq_cst);
        return slab.gen.load(std::memory_order_seq_cst) <= done;
    }

    /// Only after claim().
    void reset(u32 index) {
        DiscardSlab& slab = m_slabs[index];
        slab.used = 0;
        slab.resets.fetch_add(1, std::memory_order_seq_cst);
    }

    /// The oldest generation still reading a slab of @p oversize kind --
    /// live slabs only. A retired slab that is too small to serve and too
    /// small to make room by its release would answer at once, and the
    /// allocation would spin on it while the work it needs never retires.
    [[nodiscard]] u64 oldest_tag(bool oversize, u64 done) const {
        u64 oldest = std::numeric_limits<u64>::max();
        for (const DiscardSlab& slab : m_slabs) {
            if (slab.buffer == nullptr || slab.oversize != oversize) continue;
            const u64 gen = slab.gen.load(std::memory_order_seq_cst);
            if (gen > done) oldest = std::min(oldest, gen);
        }
        return oldest;
    }

    /// A small buffer to give the timeline a list when it needs one.
    [[nodiscard]] void* scratch_buffer(void* device) {
        if (m_scratch == nullptr) {
            id<MTLDevice> mtl = (__bridge id<MTLDevice>)device;
            id<MTLBuffer> buffer =
                [mtl newBufferWithLength:kDiscardAlign
                                 options:MTLResourceStorageModeShared];
            if (buffer != nil) m_scratch = (__bridge_retained void*)buffer;
        }
        return m_scratch;
    }

    /// Retire generation @p target. It may lie past the newest submission --
    /// the list being recorded, or (kTagMargin) the one after it -- so hand
    /// over the recording, giving it work of its own if it has none, until a
    /// list at or past @p target exists; then wait for it.
    [[nodiscard]] static bool advance_to(u64 target, void* scratch) {
        auto& queue = MetalSubmitQueue::instance();
        u64 submitted = queue.recording_generation() - 1;
        for (u32 round = 0; round < 3 && submitted < target; ++round) {
            if (round > 0) {
                // The recording was empty (or the target is the list after
                // it): give it a list of its own. Captured operations would
                // not reach the stream.
                if (MetalSubmitQueue::thread_capturing() || scratch == nullptr) {
                    return false;
                }
                queue.record_fill_buffer(scratch, 0, 4, 0);
            }
            submitted = queue.submit();
        }
        if (submitted < target) return false;
        return queue.wait_for_generation(target);
    }

    [[nodiscard]] bool take(u32 index, usize need, u64 tag,
                            DiscardSlice& out) {
        DiscardSlab& slab = m_slabs[index];
        const usize start = align_up(slab.used, kDiscardAlign);
        if (start > slab.capacity || need > slab.capacity - start) {
            return false;
        }
        slab.used = start + need;
        // Read by the list being recorded at the earliest.
        raise_to(slab.gen, tag);
        slab.touched = true;
        out.buffer = slab.buffer;
        out.offset = start;
        out.cpu = slab.contents + start;
        out.slab = index;
        out.epoch = slab.epoch.load(std::memory_order_seq_cst);
        out.resets = slab.resets.load(std::memory_order_seq_cst);
        out.recording = tag - kTagMargin;
        return true;
    }

    [[nodiscard]] u32 free_entry() const {
        for (u32 i = 0; i < kDiscardSlabEntries; ++i) {
            if (m_slabs[i].buffer == nullptr) return i;
        }
        return kNoSlab;
    }

    [[nodiscard]] bool create(void* device, u32 index, usize capacity,
                              bool oversize) {
        id<MTLDevice> mtl = (__bridge id<MTLDevice>)device;
        id<MTLBuffer> buffer =
            [mtl newBufferWithLength:capacity
                             options:MTLResourceStorageModeShared];
        if (buffer == nil || [buffer contents] == nullptr) return false;
        DiscardSlab& slab = m_slabs[index];
        slab.buffer = (__bridge_retained void*)buffer;
        slab.contents = static_cast<u8*>([buffer contents]);
        slab.capacity = capacity;
        slab.used = 0;
        slab.gen.store(0, std::memory_order_seq_cst);
        slab.oversize = oversize;
        slab.touched = true;
        slab.idle = 0;
        if (oversize) {
            m_oversize_bytes += capacity;
            ++m_oversize_created;
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                std::fprintf(stderr,
                             "[metal] DISCARD rename of %zu MiB takes an "
                             "oversize slab outside the %zu MiB bound\n",
                             capacity >> 20, kDiscardPoolMaxBytes >> 20);
            }
            return true;
        }
        m_bytes += capacity;
        ++m_count;
        if (m_bytes > m_high_water) {
            m_high_water = m_bytes;
            std::fprintf(stderr,
                         "[metal] DISCARD pool grew to %u slabs, %zu MiB "
                         "(bound %zu MiB)\n",
                         m_count, m_bytes >> 20, kDiscardPoolMaxBytes >> 20);
        }
        return true;
    }

    /// Only after claim(). Work that bound a slice retains the MTLBuffer
    /// until it has run, and a holder that still names a slice here leaves it
    /// at its next pin (the epoch moved and so did the reset count), so
    /// dropping the pool's reference is safe.
    void release(u32 index) {
        DiscardSlab& slab = m_slabs[index];
        if (slab.buffer == nullptr) return;
        slab.resets.fetch_add(1, std::memory_order_seq_cst);
        CFRelease(slab.buffer);
        if (slab.oversize) {
            m_oversize_bytes -= slab.capacity;
        } else {
            m_bytes -= slab.capacity;
            --m_count;
        }
        slab.buffer = nullptr;
        slab.contents = nullptr;
        slab.capacity = 0;
        slab.used = 0;
        slab.gen.store(0, std::memory_order_seq_cst);
        slab.oversize = false;
        slab.touched = false;
        slab.idle = 0;
        if (m_current == index) m_current = kNoSlab;
    }

    std::mutex m_mu;
    std::array<DiscardSlab, kDiscardSlabEntries> m_slabs{};
    u32 m_current{kNoSlab};
    usize m_bytes{0};
    u32 m_count{0};
    usize m_oversize_bytes{0};
    u64 m_oversize_created{0};
    u64 m_waits{0};
    usize m_high_water{0};
    void* m_scratch{nullptr};
};

std::atomic<u64> g_hazard_writes{0};

[[nodiscard]] const char* write_name(BufferWrite mode) {
    switch (mode) {
    case BufferWrite::Ordered: return "ordered";
    case BufferWrite::Discard: return "discard";
    case BufferWrite::NoOverwrite: return "no-overwrite";
    }
    return "?";
}

[[nodiscard]] MTLResourceOptions storage_options(BufferUsage usage) {
    if (usage == BufferUsage::Uniform || usage == BufferUsage::Staging) {
        return MTLResourceStorageModeShared;
    }
    return MTLResourceStorageModeShared;
}

void note_hazard(BufferWrite mode, usize length, usize offset, usize size,
                 u64 gen, u64 done, bool owned) {
    g_hazard_writes.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<unsigned> reported{0};
    if (reported.fetch_add(1) < 8) {
        std::fprintf(stderr,
                     "[metal] HAZARD: %s %zu-byte write at +%zu into %zu-byte "
                     "buffer storage read by gen %llu (completed %llu) "
                     "owned=%d\n",
                     write_name(mode), length, offset, size,
                     static_cast<unsigned long long>(gen),
                     static_cast<unsigned long long>(done), owned ? 1 : 0);
    }
}

} // namespace

MetalDiscardPoolStats metal_discard_pool_stats() {
    return DiscardPool::instance().stats();
}

u64 metal_buffer_hazard_writes() {
    return g_hazard_writes.load(std::memory_order_relaxed);
}

bool metal_discard_pool_reclaim_for_test(void* storage) {
    return DiscardPool::instance().reclaim_for_test(storage);
}

bool metal_discard_pool_abandon_claim_for_test(void* storage) {
    return DiscardPool::instance().abandon_claim_for_test(storage);
}

void metal_discard_pool_fail_allocations_for_test(bool fail) {
    g_fail_allocations.store(fail, std::memory_order_relaxed);
}

void metal_note_presented_buffers() {
    DiscardPool::instance().trim(
        MetalSubmitQueue::instance().completed_generation());
}

void MetalBuffer::note_gpu_bind() const {
    const u64 gen = MetalSubmitQueue::instance().recording_generation();
    if (published() == m_owned) {
        if (gen > m_owned_gen.load(std::memory_order_relaxed)) {
            m_owned_gen.store(gen, std::memory_order_relaxed);
        }
    } else if (gen > m_slice_gen.load(std::memory_order_relaxed)) {
        m_slice_gen.store(gen, std::memory_order_relaxed);
        DiscardPool::instance().note_read(m_slice_slab, gen);
    }
}

void MetalBuffer::note_gpu_bind_once(u64 gen) const {
    void* const storage = published();
    const usize offset = gpu_offset();
    if (m_bind_mark_gen.load(std::memory_order_acquire) == gen &&
        m_bind_mark_storage.load(std::memory_order_relaxed) == storage &&
        m_bind_mark_offset.load(std::memory_order_relaxed) == offset) {
        return;
    }
    note_gpu_bind();
    m_bind_mark_storage.store(storage, std::memory_order_relaxed);
    m_bind_mark_offset.store(offset, std::memory_order_relaxed);
    m_bind_mark_gen.store(gen, std::memory_order_release);
}

void MetalBuffer::move_to_owned(bool rewrite_all, bool carry_bind) {
    // A bind that tagged the slice before asking for the handle (the
    // argument-table and buffer<->texture blit paths do) is recording work
    // that will use the storage returned now: the owned storage.
    const u64 slice_gen = m_slice_gen.load(std::memory_order_relaxed);
    if (carry_bind &&
        slice_gen >= MetalSubmitQueue::instance().recording_generation() &&
        slice_gen > m_owned_gen.load(std::memory_order_relaxed)) {
        m_owned_gen.store(slice_gen, std::memory_order_relaxed);
    }
    if (!rewrite_all) {
        // Pool storage may already belong to another buffer: the CPU mirror
        // (always present while a slice is published) is the contents.
        id<MTLBuffer> owned = (__bridge id<MTLBuffer>)m_owned;
        if (owned != nil && [owned contents] != nullptr && !m_shadow.empty()) {
            std::memcpy(static_cast<u8*>([owned contents]) + m_owned_offset,
                        m_shadow.data(), m_size);
        }
    }
    m_pin_recording.store(0, std::memory_order_relaxed);
    m_slice_gen.store(0, std::memory_order_relaxed);
    publish(m_owned, m_owned_offset);
}

void MetalBuffer::settle_slice(u64 done, bool may_move, bool rewrite_all,
                               bool carry_bind) {
    if (published() == m_owned) return;
    // The slice was taken because work recorded earlier still read the owned
    // storage. Once that work has retired the owned storage is free: move
    // back, so the slice stops being bound and its slab can be reclaimed.
    // Waiting for the slice's own last read instead never ends for a buffer
    // bound in every list while the GPU lags -- each bind moves it on.
    auto& queue = MetalSubmitQueue::instance();
    if (may_move && m_owned_gen.load(std::memory_order_relaxed) <= done) {
        move_to_owned(rewrite_all, carry_bind);
        return;
    }
    // Keep it, pinned for the list being recorded -- unless its slab was
    // reclaimed, which happens only once the owned storage has retired.
    const u64 recording = queue.recording_generation();
    switch (DiscardPool::instance().pin(m_slice_slab, recording, m_slice_epoch,
                                        m_slice_resets)) {
    case PinResult::Pinned:
    case PinResult::Revalidated:
        m_pin_recording.store(recording, std::memory_order_relaxed);
        return;
    case PinResult::Reclaimed:
        break;
    }
    const u64 now = queue.completed_generation();
    const u64 owned_gen = m_owned_gen.load(std::memory_order_relaxed);
    if (owned_gen > now) {
        // Unreachable by the reclaim handshake; say so if it ever is.
        note_hazard(BufferWrite::Ordered, m_size, 0, m_size, owned_gen, now,
                    true);
    }
    move_to_owned(rewrite_all, carry_bind);
}

void* MetalBuffer::handle_locked() {
    settle_slice(MetalSubmitQueue::instance().completed_generation(),
                 /*may_move=*/true, /*rewrite_all=*/false, /*carry_bind=*/true);
    return published();
}

void* MetalBuffer::handle() {
    // Only the thread that renames this buffer changes which storage is
    // published, and a frontend renames only while nothing else binds it, so
    // the owned case needs no lock.
    void* const current = published();
    if (current == m_owned) return current;
    // Lock-free while the slice is pinned for the list still being recorded
    // and the owned storage is still read. Then the slice stays valid: after
    // that pin every claim on its slab backs off until the pinned list
    // retires, and it is still being recorded. And nothing moves it: this
    // thread would only move back to the owned storage, which is still read;
    // a rename runs only while nothing else binds the buffer; a NO_OVERWRITE
    // write on another thread moves storage only off a slab that was really
    // reclaimed, which the pin rules out. The pin was taken under the lock,
    // by this thread or the one renaming before it binds.
    auto& queue = MetalSubmitQueue::instance();
    if (m_pin_recording.load(std::memory_order_relaxed) ==
            queue.recording_generation() &&
        m_owned_gen.load(std::memory_order_relaxed) >
            queue.completed_generation()) {
        return current;
    }
    UnfairLockGuard lock(m_mu);
    return handle_locked();
}

MetalBuffer::MetalBuffer(void* device, usize size, BufferUsage usage,
                         const void* initial_data)
    : m_device(device), m_size(size), m_usage(usage)
{
    // A small constant buffer takes a slot of a shared slab rather than an
    // MTLBuffer of its own (MetalConstantBufferPool). A slot may have held
    // another buffer, so it is filled exactly as a new MTLBuffer would be:
    // with the initial data, or with zeros.
    if (usage == BufferUsage::Uniform && size != 0 &&
        size <= MetalConstantBufferPool::kMaxBytes &&
        MetalConstantBufferPool::instance().acquire(device, size, m_slot)) {
        if (initial_data != nullptr) {
            std::memcpy(m_slot.contents, initial_data, size);
        } else {
            std::memset(m_slot.contents, 0, size);
        }
        m_owned = m_slot.buffer;
        m_owned_offset = m_slot.offset;
        m_pooled = true;
    } else {
        m_owned = alloc_mtl_buffer(initial_data);
    }
    publish(m_owned, m_owned_offset);
    VITREO_TRACE("[buffer] created %zu bytes, usage=%u\n",
                 m_size, static_cast<unsigned>(m_usage));
}

MetalBuffer::MetalBuffer(void* device, void* external, usize size,
                         BufferUsage usage, const void* initial_data)
    : m_device(device), m_size(size), m_usage(usage) {
    id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)external;
    if (buffer == nil || [buffer length] < size) return;
    m_owned = (__bridge_retained void*)buffer;
    publish(m_owned, 0);
    m_pinned = true;
    m_gpu_written = true;
    if (initial_data != nullptr && [buffer contents] != nullptr) {
        // Nothing has been recorded against a new buffer yet.
        std::memcpy([buffer contents], initial_data, size);
    }
}

MetalBuffer::~MetalBuffer() {
    // A published pool slice needs nothing: its slab stays tagged with every
    // generation that bound it.
    if (m_pooled) {
        // Work recorded so far may still read the slot: it is reused once the
        // list being recorded now has completed.
        MetalConstantBufferPool::instance().release(
            m_slot, MetalSubmitQueue::instance().recording_generation());
        m_owned = nullptr;
    } else if (m_owned) {
        CFRelease(m_owned);
        m_owned = nullptr;
    }
    m_buffer.store(nullptr, std::memory_order_relaxed);
}

void* MetalBuffer::alloc_mtl_buffer(const void* initial_data) const {
    id<MTLDevice> mtl_device = (__bridge id<MTLDevice>)m_device;
    if (mtl_device == nil || m_size == 0) return nullptr;
    const MTLResourceOptions opts = storage_options(m_usage);
    id<MTLBuffer> buf = nil;
    if (initial_data) {
        buf = [mtl_device newBufferWithBytes:initial_data
                                      length:m_size
                                     options:opts];
    } else {
        buf = [mtl_device newBufferWithLength:m_size options:opts];
    }
    return (__bridge_retained void*)buf;
}

void* MetalBuffer::map() {
    UnfairLockGuard lock(m_mu);
    // Pinned (or back on the owned storage) for the list being recorded, so
    // the bytes stay this buffer's while the caller reads them now.
    id<MTLBuffer> buf = (__bridge id<MTLBuffer>)handle_locked();
    if (buf == nil || [buf contents] == nullptr) return nullptr;
    return static_cast<u8*>([buf contents]) +
           m_offset.load(std::memory_order_relaxed);
}

bool MetalBuffer::rename_storage(bool whole) {
    // Every byte outside the write keeps its value, whatever the lock flags:
    // D3D9 backs a buffer with one persistent guest allocation and Unlock
    // uploads only the locked range, so the GPU copy is an incrementally
    // maintained mirror of it (d3d9_resources.cpp, `buffer.storage`). The
    // old contents come from the CPU mirror, never from a published slice:
    // the allocation below may wait, and a slab that retires meanwhile can be
    // handed straight back, overlapping the slice this buffer still names.
    if (m_shadow.empty()) {
        // No mirror yet, so the owned storage is published (a slice is only
        // taken with one) and no GPU write into it is pending (update()
        // orders those): its bytes are the contents.
        id<MTLBuffer> source = (__bridge id<MTLBuffer>)published();
        if (source == nil || [source contents] == nullptr) return false;
        m_shadow.resize(m_size);
        if (!whole) {
            std::memcpy(m_shadow.data(),
                        static_cast<u8*>([source contents]) +
                            m_offset.load(std::memory_order_relaxed),
                        m_size);
        }
    }
    DiscardSlice slice{};
    if (!DiscardPool::instance().allocate(m_device, m_size, slice)) {
        return false;
    }
    if (!whole) std::memcpy(slice.cpu, m_shadow.data(), m_size);
    m_slice_slab = slice.slab;
    m_slice_epoch = slice.epoch;
    m_slice_resets = slice.resets;
    m_slice_gen.store(0, std::memory_order_relaxed);
    // take() tagged the slab past the recording it ran in, as a pin would.
    m_pin_recording.store(slice.recording, std::memory_order_relaxed);
    publish(slice.buffer, slice.offset);
    return true;
}

void MetalBuffer::wait_for_storage() {
    auto& queue = MetalSubmitQueue::instance();
    u64 gen = storage_gen();
    if (gen >= queue.recording_generation()) {
        // Hand over the list that may read it. A generation above the newest
        // submission was never given work: nothing reads the storage there.
        gen = std::min(gen, queue.submit());
    }
    if (!queue.wait_for_generation(gen)) return;  // shut down
    if (published() == m_owned) {
        m_owned_gen.store(gen, std::memory_order_relaxed);
    } else {
        m_slice_gen.store(gen, std::memory_order_relaxed);
    }
}

void MetalBuffer::cpu_update(const void* data, usize offset, usize length,
                             BufferWrite mode, u64 done) {
    const bool whole = offset == 0 && length == m_size;
    const bool no_overwrite = mode == BufferWrite::NoOverwrite;
    // A NO_OVERWRITE write keeps the published storage -- the D3D9 worker
    // may be binding it right now -- and pins a slice it writes into. It
    // leaves the slice only if the slab was really reclaimed, which no bind
    // in progress allows (it pinned first), so no bind sees the move half-way.
    settle_slice(done, /*may_move=*/!no_overwrite, whole,
                 /*carry_bind=*/false);
    if (!no_overwrite && storage_gen() > done && !rename_storage(whole)) {
        // No renaming memory can be had (Metal refused the slab, or every
        // slab record is in use). Learn D3DLOCK: without NOOVERWRITE "the
        // driver must finish using the resource before returning from
        // locking" -- so wait for it.
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr,
                         "[metal] DISCARD pool cannot rename %zu bytes; "
                         "waiting for the GPU to finish with the buffer\n",
                         m_size);
        }
        wait_for_storage();
        // The wait may have let a published slice's slab retire: pin it (or
        // leave it) again before writing.
        settle_slice(MetalSubmitQueue::instance().completed_generation(),
                     /*may_move=*/true, whole, /*carry_bind=*/false);
    }
    if (!m_shadow.empty()) {
        std::memcpy(m_shadow.data() + offset, data, length);
    }
    // NO_OVERWRITE writes beside in-flight reads by the application's
    // promise. Any other write here lands where nothing recorded reads --
    // unless the GPU could not be waited for (queue shut down).
    if (!no_overwrite) {
        const u64 now = MetalSubmitQueue::instance().completed_generation();
        if (storage_gen() > now) {
            note_hazard(mode, length, offset, m_size, storage_gen(), now,
                        published() == m_owned);
        }
    }
    id<MTLBuffer> buf = (__bridge id<MTLBuffer>)published();
    if (buf == nil || [buf contents] == nullptr) return;
    std::memcpy(static_cast<u8*>([buf contents]) +
                    m_offset.load(std::memory_order_relaxed) + offset,
                data, length);
}

void MetalBuffer::note_gpu_write_locked() {
    void* const current = handle_locked();
    if (current != m_owned) {
        // Pin the slice until the copy below has read it, then move back to
        // the owned buffer. The copy is ordered after every GPU write already
        // recorded into the slice and before the write the caller records.
        note_gpu_bind();
        MetalSubmitQueue::instance().record_copy_buffer(
            current, m_owned, m_size, m_offset.load(std::memory_order_relaxed),
            m_owned_offset);
        m_pin_recording.store(0, std::memory_order_relaxed);
        m_slice_gen.store(0, std::memory_order_relaxed);
        publish(m_owned, m_owned_offset);
    }
    // From here the GPU's writes are the contents and the CPU mirror no
    // longer describes them. A later rename rebuilds it from the storage once
    // they have retired (update() orders every write until then); restoring a
    // stale mirror would undo them.
    m_shadow.clear();
    note_gpu_bind();
    m_gpu_write_gen = MetalSubmitQueue::instance().recording_generation();
}

void MetalBuffer::note_gpu_write() {
    UnfairLockGuard lock(m_mu);
    note_gpu_write_locked();
}

void MetalBuffer::ordered_update(const void* data, usize offset,
                                 usize length) {
    // D3D11 UpdateSubresource is a command in the stream: work recorded
    // before it keeps the old bytes, work after it sees the new ones. With
    // GPU writes still pending, the CPU cannot build that image itself, so
    // the bytes travel through a staging slice and a queued blit.
    note_gpu_write_locked();
    DiscardSlice staging{};
    id<MTLBuffer> fallback = nil;
    void* source = nullptr;
    usize source_offset = 0;
    if (DiscardPool::instance().allocate(m_device, length, staging)) {
        std::memcpy(staging.cpu, data, length);
        source = staging.buffer;
        source_offset = staging.offset;
    } else {
        // No pool slice could be had: a buffer of its own, which the copy
        // retains until it has run.
        id<MTLDevice> device = (__bridge id<MTLDevice>)m_device;
        fallback = [device newBufferWithBytes:data
                                       length:length
                                      options:MTLResourceStorageModeShared];
        if (fallback == nil) return;
        source = (__bridge void*)fallback;
    }
    MetalSubmitQueue::instance().record_copy_buffer(
        source, published(), length, source_offset,
        m_offset.load(std::memory_order_relaxed) + offset);
}

void MetalBuffer::update(const void* data, usize offset, usize length,
                         BufferWrite mode) {
    if (data == nullptr || length == 0 || offset > m_size ||
        length > m_size - offset) {
        return;
    }
    UnfairLockGuard lock(m_mu);
    // Pending GPU writes (or a GPU-written buffer still in use) make every
    // CPU-side path wrong: a rename copies stale "old contents", an in-place
    // write lands under queued work. Order the update on the queue instead
    // (d3d11_a10_dispatch_readback_probe: UpdateSubresource between
    // dispatches lost whole dispatches; Windows 20/20). An ordered write
    // satisfies every mode, NO_OVERWRITE included.
    const u64 done = MetalSubmitQueue::instance().completed_generation();
    const bool in_use =
        std::max(m_owned_gen.load(std::memory_order_relaxed),
                 m_slice_gen.load(std::memory_order_relaxed)) > done;
    // Shared storage is read by other processes' GPUs as well: a CPU write
    // in place, or a rename, would bypass the order they synchronize on.
    if (m_pinned || m_gpu_write_gen > done || (m_gpu_written && in_use)) {
        ordered_update(data, offset, length);
        return;
    }
    // Learn (D3D11_MAP_WRITE_DISCARD): the previous contents are no longer
    // needed and the GPU keeps reading them for work already queued, whatever
    // the buffer is bound as. Renaming was limited to vertex, index and
    // constant buffers, so a DYNAMIC structured/raw/typed SRV buffer refilled
    // before queued dispatches ran was overwritten under them (measured:
    // tools/oracle_probe/d3d11_uav_readback_probe, "discard-upload"). A
    // D3D11 renderer's scene-data scatter uploads are exactly that pattern.
    cpu_update(data, offset, length, mode, done);
}

} // namespace vitreo::graphics

/// @file heap.cpp
/// @brief HeapCreate / HeapAlloc / HeapFree implementation (WIN32-021)

#include "winapi/kernel32/memory/heap.hpp"
#include "core/memory/guest_memory.hpp"
#include "ipc/ipc_gate.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <cstdio>
#include "heap_projection.hpp"
#include <cstring>
#include <limits>

namespace vitreo::winapi {

// =============================================================================
// Heap
// =============================================================================

Heap::Heap(VirtualMemoryManager& vmm, GuestMemory& guest_memory,
           u64 /*initial_size*/, u32 options, u64 max_size)
    : m_vmm(vmm)
    , m_guest_memory(guest_memory)
    , m_options(options)
    , m_max_size(max_size) {
}

bool Heap::initialize_identity(u32 flags, bool is_64bit) {
    m_identity = m_vmm.virtual_alloc_internal(GUEST_PAGE_SIZE, PAGE_READWRITE);
    if (m_identity == 0) return false;
    m_identity_flags = flags;
    m_identity_is_64bit = is_64bit;
    return refresh_identity(is_64bit);
}

bool Heap::refresh_identity(bool is_64bit) {
    if (m_identity == 0) return false;
    const auto previous = m_identity_is_64bit ? heap_projection::layout64 : heap_projection::layout32;
    const auto layout = is_64bit ? heap_projection::layout64 : heap_projection::layout32;
    if (is_64bit != m_identity_is_64bit) {
        if (m_guest_memory.write_u32(m_identity + previous.flags, 0).is_err() ||
            m_guest_memory.write_u32(m_identity + previous.force_flags, 0).is_err()) return false;
    }
    m_identity_is_64bit = is_64bit;
    return m_guest_memory.write_u32(m_identity + layout.flags, m_identity_flags).is_ok() &&
        m_guest_memory.write_u32(m_identity + layout.force_flags, m_options & layout.force_mask).is_ok();
}

Heap::~Heap() {
    // Release direct-VirtualAlloc allocations (not slab sub-allocations).
    for (auto& [addr, info] : m_allocations) {
        if (!info.from_slab) {
            m_vmm.virtual_free_internal(
                info.region_base != 0 ? info.region_base : addr);
        }
    }
    // Release all slabs as whole regions.
    for (auto& slab : m_slabs) {
        m_vmm.virtual_free_internal(slab.base);
    }
    if (m_identity != 0) m_vmm.virtual_free_internal(m_identity);
}

namespace {
// Slab sub-allocations are rounded to a size CLASS, not just to 16 bytes:
// the free lists are keyed by the committed size, so a block freed at one
// exact size could only ever serve a request of that same exact size. On a
// workload that allocates varying sizes (every real program) reuse almost
// never happened and the heap grew a new slab for memory it was holding on
// its free lists -- tools/stress sc_alloc reached 8.3 GB resident for a
// bounded live set. 16-byte classes up to 1 KB, then four classes per
// doubling (at most 12.5% over-allocation), which is the NT LFH shape.
[[nodiscard]] u64 slab_size_class(u64 n) noexcept {
    if (n <= 1024) return n;
    const int top = 63 - __builtin_clzll(n);
    const u64 step = u64{1} << (top - 2);
    return (n + step - 1) & ~(step - 1);
}
} // namespace

u64 Heap::alloc(u64 size, u32 flags) {
    // Win32 HeapAlloc(hHeap, 0, 0) is valid and returns a non-null pointer
    // whose HeapSize is 0 (heap_contract_probe: zero_block size=0x0), so the
    // block gets 1 byte of capacity but records the requested size.
    const u64 requested = size;
    if (size == 0) {
        size = 1;
    }

    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    // Learn HeapCreate: non-growable heaps refuse a single block larger than
    // slightly less than 1 MiB (x64).
    if (m_max_size != 0 && size > HEAP_FIXED_MAX_BLOCK_X64) {
        return 0;
    }

    // ---- Small allocations: slab sub-allocator ----
    if (size < SLAB_THRESHOLD) {
        const u64 aligned_size = slab_size_class(align_up(size, SLAB_ALIGN));

        // Try to reuse a previously freed slab block of the same size class.
        auto fl_it = m_slab_free_lists.find(aligned_size);
        if (fl_it != m_slab_free_lists.end() && !fl_it->second.empty()) {
            u64 addr = fl_it->second.back();
            fl_it->second.pop_back();
            auto& info = m_allocations[addr];
            info.in_use = true;
            info.requested_size = requested;
            if (flags & HEAP_ZERO_MEMORY) {
                auto* p = m_guest_memory.host_ptr_mut(
                    static_cast<GuestAddr>(addr), static_cast<usize>(size));
                if (p) std::memset(p, 0, static_cast<usize>(size));
            }
            return addr;
        }

        u64 addr = slab_bump_alloc(aligned_size);
        if (addr == 0) {
        ipc::ipc_log("HeapAlloc FAIL: slab_bump_alloc returned 0 (req=%llu aligned=%llu)",
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned long long>(aligned_size));
            std::fprintf(stderr,
                "[heap] HeapAlloc FAIL slab req=%llu aligned=%llu\n",
                static_cast<unsigned long long>(size),
                static_cast<unsigned long long>(aligned_size));
            return 0;
        }
        m_allocations[addr] = AllocationInfo{requested, aligned_size, true, true};
        // Fresh slab memory is already zeroed by VirtualAlloc.
        return addr;
    }

    // ---- Large allocations: direct VirtualAlloc ----
    // NT places a HEAP_VIRTUAL_ALLOC_ENTRY before the user bytes, so the
    // returned pointer is not the reservation base.
    // Non-growable heaps still serve large requests up to
    // HEAP_FIXED_MAX_BLOCK_X64 (oracle fixed_largest_block=0xfeff8), counted
    // against dwMaximumSize.
    if (size > std::numeric_limits<u64>::max() - SLAB_SEGMENT_PREFIX -
                   static_cast<u64>(GUEST_PAGE_SIZE)) {
        return 0;
    }
    const u64 alloc_size =
        align_up(size + SLAB_SEGMENT_PREFIX,
                 static_cast<u64>(GUEST_PAGE_SIZE));
    if (m_max_size != 0) {
        if (m_committed_bytes > m_max_size ||
            alloc_size > m_max_size - m_committed_bytes) {
            return 0;
        }
    }

    // Try to reuse a previously freed block of the same committed size.
    auto pool_it = m_free_pool.find(alloc_size);
    if (pool_it != m_free_pool.end() && !pool_it->second.empty()) {
        u64 addr = pool_it->second.back();
        pool_it->second.pop_back();
        auto& info = m_allocations[addr];
        info.in_use = true;
        info.requested_size = requested;
        if (flags & HEAP_ZERO_MEMORY) {
            auto* p = m_guest_memory.host_ptr_mut(
                static_cast<GuestAddr>(addr), static_cast<usize>(size));
            if (p) std::memset(p, 0, static_cast<usize>(size));
        }
        VITREO_TRACE("[heap] HeapAlloc(size=%llu, flags=0x%x) -> 0x%llx (reused, alloc_size=%llu)\n",
                     static_cast<unsigned long long>(size),
                     static_cast<unsigned>(flags),
                     static_cast<unsigned long long>(addr),
                     static_cast<unsigned long long>(alloc_size));
        return addr;
    }

    // No free block available — allocate a fresh one via VirtualAlloc.
    u64 base = m_vmm.virtual_alloc_internal(alloc_size, backing_protect());
    if (base == 0) {
        ipc::ipc_log("HeapAlloc FAIL: virtual_alloc(0,%llu) returned 0 (req=%llu)",
                     static_cast<unsigned long long>(alloc_size),
                     static_cast<unsigned long long>(size));
        std::fprintf(stderr,
            "[heap] HeapAlloc FAIL valloc req=%llu aligned=%llu\n",
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(alloc_size));
        return 0;
    }
    m_committed_bytes += alloc_size;

    const u64 addr = base + SLAB_SEGMENT_PREFIX;
    m_allocations[addr] = AllocationInfo{requested, alloc_size, true, false, base};

    VITREO_TRACE("[heap] HeapAlloc(size=%llu, flags=0x%x) -> 0x%llx (alloc_size=%llu)\n",
                 static_cast<unsigned long long>(size),
                 static_cast<unsigned>(flags),
                 static_cast<unsigned long long>(addr),
                 static_cast<unsigned long long>(alloc_size));
    if (addr >= 0x10000000ull) {
        std::fprintf(stderr,
            "[heap] HeapAlloc HIGH size=%llu -> 0x%llx\n",
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(addr));
    }
    return addr;
}

// Caller holds m_mutex (heap_alloc).
u64 Heap::slab_bump_alloc(u64 aligned_size) {
    // Try the current slab first.
    if (!m_slabs.empty()) {
        auto& slab = m_slabs.back();
        if (slab.offset + aligned_size <= slab.size) {
            u64 addr = slab.base + slab.offset;
            slab.offset += aligned_size;
            return addr;
        }
        // Non-growable: cannot add another slab once the maximum is reserved.
        if (m_max_size != 0) {
            return 0;
        }
    }
    // Need a new slab. Leave a committed prefix so the first user pointer is
    // not the region base — NT heap segments store HEAP/HEAP_SEGMENT there.
    u64 slab_size = std::max(SLAB_SIZE, SLAB_SEGMENT_PREFIX + aligned_size);
    if (m_max_size != 0) {
        // One reservation covering the whole fixed heap.
        if (m_committed_bytes >= m_max_size) {
            return 0;
        }
        slab_size = m_max_size - m_committed_bytes;
        if (slab_size < SLAB_SEGMENT_PREFIX + aligned_size) {
            return 0;
        }
    }
    u64 base = m_vmm.virtual_alloc_internal(slab_size, backing_protect());
    if (base == 0) {
        return 0;
    }
    m_committed_bytes += slab_size;
    m_slabs.push_back(Slab{base, slab_size, SLAB_SEGMENT_PREFIX + aligned_size});
    return base + SLAB_SEGMENT_PREFIX;
}

bool Heap::free(u64 address) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto it = m_allocations.find(address);
    if (it == m_allocations.end() || !it->second.in_use) {
        return false;
    }

    const u64 committed = it->second.committed_size;
    it->second.in_use = false;

    if (it->second.from_slab) {
        // Return to the slab free list for reuse (no size limit — the memory
        // lives inside an arena that is freed only when the heap is destroyed).
        m_slab_free_lists[committed].push_back(address);
    } else if (committed >= LARGE_FREE_THRESHOLD) {
        // Large allocations: free immediately to avoid holding address space.
        // Growing-buffer patterns (realloc N → N+1 → N+2) create unique sizes
        // so pooling would never reuse them — just accumulate dead weight.
        if (address >= 0x10000000ull) {
            std::fprintf(stderr,
                "[heap] HeapFree HIGH addr=0x%llx size=%llu\n",
                static_cast<unsigned long long>(address),
                static_cast<unsigned long long>(committed));
        }
        m_vmm.virtual_free_internal(
            it->second.region_base != 0 ? it->second.region_base : address);
        if (m_committed_bytes >= committed) {
            m_committed_bytes -= committed;
        }
        m_allocations.erase(it);
    } else {
        // Direct-VirtualAlloc block: keep a small pool per size bucket.
        static constexpr usize kMaxPoolPerBucket = 4;
        auto& bucket = m_free_pool[committed];
        if (bucket.size() < kMaxPoolPerBucket) {
            bucket.push_back(address);
        } else {
            m_vmm.virtual_free_internal(
                it->second.region_base != 0 ? it->second.region_base : address);
            if (m_committed_bytes >= committed) {
                m_committed_bytes -= committed;
            }
            m_allocations.erase(it);
        }
    }

    VITREO_TRACE("[heap] HeapFree(0x%llx)\n",
                 static_cast<unsigned long long>(address));
    return true;
}

u64 Heap::realloc(u64 address, u64 new_size, u32 flags) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto it = m_allocations.find(address);
    if (it == m_allocations.end() || !it->second.in_use) {
        return 0;
    }

    const u64 old_size = it->second.requested_size;

    const u64 usable = it->second.from_slab
        ? it->second.committed_size
        : (it->second.committed_size > SLAB_SEGMENT_PREFIX
               ? it->second.committed_size - SLAB_SEGMENT_PREFIX
               : 0);
    // Learn HeapReAlloc: HEAP_REALLOC_IN_PLACE_ONLY "... If this value is
    // specified and the block cannot be resized without moving, the function
    // fails". A block whose existing capacity holds the new size is resized
    // in place with or without the flag (heap_contract_probe: inplace_shrink
    // and inplace_grow_small return the same pointer, last error untouched).
    if (usable >= new_size) {
        it->second.requested_size = new_size;
        if (flags & HEAP_ZERO_MEMORY && new_size > old_size) {
            const u64 extra = new_size - old_size;
            auto* p = m_guest_memory.host_ptr_mut(
                static_cast<GuestAddr>(address + old_size),
                static_cast<usize>(extra));
            if (p) std::memset(p, 0, static_cast<usize>(extra));
        }
        return address;
    }

    // HEAP_REALLOC_IN_PLACE_ONLY and the block must move: fail, leaving the
    // block and its contents untouched. Windows reports ERROR_NOT_ENOUGH_MEMORY
    // (heap_contract_probe: inplace_grow_big ret=0 err=0x8).
    if (flags & HEAP_REALLOC_IN_PLACE_ONLY) {
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
        return 0;
    }

    // Allocate a new block.
    u64 new_addr = alloc(new_size, flags);
    if (new_addr == 0) {
        return 0;
    }

    // Copy the smaller of old / new sizes via direct host pointers.
    const u64 copy_len = std::min(old_size, new_size);
    if (copy_len > 0) {
        auto* src = m_guest_memory.host_ptr(static_cast<GuestAddr>(address),
                                            static_cast<usize>(copy_len));
        auto* dst = m_guest_memory.host_ptr_mut(static_cast<GuestAddr>(new_addr),
                                                static_cast<usize>(copy_len));
        if (src && dst) {
            std::memcpy(dst, src, static_cast<usize>(copy_len));
        }
    }

    // Free the old block.
    free(address);

    return new_addr;
}

u64 Heap::size(u64 address) const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    auto it = m_allocations.find(address);
    if (it == m_allocations.end() || !it->second.in_use) {
        return ~0ULL;
    }
    return it->second.requested_size;
}

// =============================================================================
// HeapManager
// =============================================================================

HeapManager::HeapManager(VirtualMemoryManager& vmm, GuestMemory& guest_memory,
                         HandleTable& handles)
    : m_vmm(vmm)
    , m_guest_memory(guest_memory)
    , m_handles(handles) {}

GuestAddr HeapManager::heap_create(u32 options, u64 initial_size, u64 max_size, bool rtl) {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);

    // Learn: "dwInitialSize must be smaller than dwMaximumSize" when the heap
    // is non-growable (max_size != 0).
    if (max_size != 0 && initial_size > max_size) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return NULL_HANDLE;
    }

    auto heap = std::make_unique<Heap>(m_vmm, m_guest_memory,
                                       initial_size, options, max_size);
    const auto layout = m_peb_is_64bit ? heap_projection::layout64 : heap_projection::layout32;
    const u32 projection_flags = (options & ~HEAP_GROWABLE) |
        (max_size == 0 ? layout.process_flags : 0u) | (rtl ? 0u : layout.private_class);
    if (!heap->initialize_identity(projection_flags, m_peb_is_64bit)) return NULL_HANDLE;
    const GuestAddr identity = heap->m_identity;
    if (!m_peb_is_64bit && identity > UINT32_MAX) return NULL_HANDLE;
    m_heaps.emplace(identity, std::move(heap));
    return identity;
}

bool HeapManager::heap_destroy(GuestAddr heap) {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);

    auto it = m_heaps.find(heap);
    if (it == m_heaps.end() || heap == m_process_heap) {
        return false;
    }

    // Heap destructor releases all VirtualAlloc'd regions.
    m_heaps.erase(it);

    return true;
}

u64 HeapManager::heap_alloc(GuestAddr heap, u32 flags, u64 size) {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    const auto it = m_heaps.find(heap);
    Heap* h = it == m_heaps.end() ? nullptr : it->second.get();
    if (!h) {
        return 0;
    }
    return h->alloc(size, flags);
}

usize HeapManager::heap_slab_count(GuestAddr heap) const {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    const auto it = m_heaps.find(heap);
    const Heap* h = it == m_heaps.end() ? nullptr : it->second.get();
    return h ? h->slab_count() : 0;
}

u32 HeapManager::heap_options(GuestAddr heap) const {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    const auto it = m_heaps.find(heap);
    const Heap* h = it == m_heaps.end() ? nullptr : it->second.get();
    return h ? h->options() : 0;
}

bool HeapManager::heap_valid(GuestAddr heap) const {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    return m_heaps.contains(heap);
}

bool HeapManager::heap_free(GuestAddr heap, u32 flags, u64 address) {
    (void)flags;
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    const auto it = m_heaps.find(heap);
    Heap* h = it == m_heaps.end() ? nullptr : it->second.get();
    if (!h) {
        return false;
    }
    return h->free(address);
}

u64 HeapManager::heap_realloc(GuestAddr heap, u32 flags,
                              u64 address, u64 new_size) {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    const auto it = m_heaps.find(heap);
    Heap* h = it == m_heaps.end() ? nullptr : it->second.get();
    if (!h) {
        return 0;
    }
    return h->realloc(address, new_size, flags);
}

u64 HeapManager::heap_size(GuestAddr heap, u32 flags, u64 address) {
    (void)flags;
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    const auto it = m_heaps.find(heap);
    Heap* h = it == m_heaps.end() ? nullptr : it->second.get();
    if (!h) {
        return ~0ULL;
    }
    return h->size(address);
}

std::vector<GuestAddr> HeapManager::heap_handles() {
    const GuestAddr process_heap = get_process_heap();
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    std::vector<GuestAddr> handles;
    handles.reserve(m_heaps.size());
    if (process_heap != NULL_HANDLE) handles.push_back(process_heap);
    std::vector<GuestAddr> others;
    for (const auto& [handle, heap] : m_heaps) {
        if (handle != process_heap) others.push_back(handle);
    }
    // Creation order: handle values are handed out increasingly.
    std::sort(others.begin(), others.end());
    handles.insert(handles.end(), others.begin(), others.end());
    return handles;
}

GuestAddr HeapManager::get_process_heap() {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);

    if (m_process_heap == INVALID_HANDLE || m_process_heap == NULL_HANDLE) {
        m_process_heap = heap_create(HEAP_GROWABLE, 0x10000, 0, true);
        if (m_process_heap == INVALID_HANDLE) {
            m_process_heap = NULL_HANDLE;
        }
        sync_peb_process_heap_locked();
    }
    return m_process_heap;
}

void HeapManager::bind_peb(u64 peb_address, bool is_64bit) noexcept {
    std::lock_guard<std::recursive_mutex> lock(m_mgr_mutex);
    m_peb_address = peb_address;
    m_peb_is_64bit = is_64bit;
    for (auto& [identity, heap] : m_heaps) (void)heap->refresh_identity(is_64bit);
    if (m_process_heap != INVALID_HANDLE && m_process_heap != NULL_HANDLE) {
        sync_peb_process_heap_locked();
    }
}

void HeapManager::sync_peb_process_heap_locked() {
    if (m_peb_address == 0 ||
        m_process_heap == INVALID_HANDLE ||
        m_process_heap == NULL_HANDLE) {
        return;
    }
    // Windows heap_identity_probe measures this pointer-width projection.
    const auto layout = m_peb_is_64bit ? heap_projection::layout64 : heap_projection::layout32;
    if (m_peb_is_64bit) {
        (void)m_guest_memory.write_u64(
            static_cast<GuestAddr>(m_peb_address + layout.peb_process_heap),
            static_cast<u64>(m_process_heap));
    } else {
        (void)m_guest_memory.write_u32(
            static_cast<GuestAddr>(m_peb_address + layout.peb_process_heap),
            static_cast<u32>(m_process_heap));
    }
}

} // namespace vitreo::winapi

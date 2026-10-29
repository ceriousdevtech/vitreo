// SPDX-License-Identifier: LGPL-2.1-only
/// @file crt_memory.cpp
/// @brief msvcrt heap, exception stubs, and misc runtime thunks

#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/runtime/cxx_eh.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/cpu/seh/seh32.hpp"
#include "winapi/kernel32/thread/thread.hpp"

#include <cstdio>

#include <algorithm>
#include <array>
#include <cstdlib>   // std::abort
#include <cstring>   // std::memset
#include <limits>
#include <optional>

namespace vitreo::winapi {

namespace {

/// Bytes reserved immediately before the user pointer for the requested size.
/// Always 8 so an x64 request ≥ 4 GiB can be recorded (Learn malloc /
/// _HEAP_MAXREQ ≈ SIZE_MAX). User pointers stay 16-byte aligned.
constexpr usize kSizeHeader = 8;

/// Header value of a block sitting on a free list. malloc rewrites it with
/// the new request; a second free of the same block sees it and is ignored
/// instead of listing the block twice.
constexpr u64 kFreedHeader = 0xFFFFFFFFFFFFFFFFull;

/// Bytes a block laid out by allocate_from_arena_locked() physically owns for
/// a request of `size`.
///
/// The allocator places the next block's size header at
/// `align_up(bump + kSizeHeader, 16) - kSizeHeader`, so the span a block owns
/// runs from its user pointer to that next header. Keying the free lists on
/// THIS rather than on the requested size is what makes reuse exactly safe:
/// capacity is always >= the request, so a recycled block can never be too
/// small.
[[nodiscard]] usize crt_block_capacity(usize size) noexcept {
    const u64 span =
        align_up(static_cast<u64>(size) + kSizeHeader, u64{16}) - kSizeHeader;
    return static_cast<usize>(span);
}

/// The reuse CLASS a block of `size` is laid out and recycled in. Exact
/// 16-byte capacities up to 1 KB, then four classes per doubling (at most
/// 12.5% over-allocation): a freed block serves every later request in its
/// class instead of only requests of its exact size, so free lists actually
/// drain. Both the arena layout and the free lists use this, so a block is
/// never handed to a request larger than it. Requests that need more than
/// 4 GiB-1 of capacity are not recycled (u32 free-list key).
[[nodiscard]] u32 crt_reuse_class(usize size) noexcept {
    const usize cap = crt_block_capacity(size);
    if (cap > std::numeric_limits<u32>::max()) {
        return 0;  // sentinel: caller must not reuse
    }
    if (cap <= 1024) return static_cast<u32>(cap);
    const int top = 63 - __builtin_clzll(cap);
    const u64 step = u64{1} << (top - 2);
    return static_cast<u32>((cap + step - 1) & ~(step - 1));
}

/// Free-list reuse is ON; VITREO_CRT_HEAP_REUSE=0 restores the monotonic
/// arenas for A/B.
///
/// Reuse used to be off: recycling addresses had crashed guests (MSVCP
/// locale facets, and EIP=0 shortly after the first present), read as
/// guest use-after-free that a never-reusing heap tolerated. Those guests run
/// on Windows' reusing heap, so the fault was in this allocator: crt_free
/// accepted ANY pointer whose preceding word was readable (a HeapAlloc block
/// or static data went on a free list with a garbage capacity and was later
/// handed out), a double free listed a block twice, and _aligned_malloc
/// stored its base pointer as 32 bits, so on x64 _aligned_free recycled a
/// truncated address. All three are closed below and in the registrations.
/// Read per manager, NOT into a function-local static: a static caches the
/// first answer for the process and makes the knob untestable.
[[nodiscard]] bool crt_heap_reuse_from_env() noexcept {
    const char* v = std::getenv("VITREO_CRT_HEAP_REUSE");
    return !(v != nullptr && v[0] == '0' && v[1] == '\0');
}

} // namespace


// =============================================================================
// CrtMemoryManager
// =============================================================================

CrtMemoryManager::CrtMemoryManager(GuestMemory& memory,
                                   GuestAddr arena_base,
                                   usize arena_size)
    : m_memory(memory)
    , m_initial_arena_base(arena_base)
    , m_growth_arena_size(
          align_up(std::max(arena_size, GUEST_PAGE_SIZE), GUEST_PAGE_SIZE))
    , m_next_arena_search(arena_base)
    , m_total_arena_size(0)
    , m_errno_addr(0)
{
    auto mapped = m_memory.map_region(
        m_initial_arena_base, m_growth_arena_size,
        MemoryProtection::ReadWrite);
    if (mapped.is_err()) {
        VITREO_TRACE(
            "[crt] preferred heap arena unavailable base=0x%llx size=0x%zx "
            "error=%u; searching for a free range\n",
            static_cast<unsigned long long>(m_initial_arena_base),
            m_growth_arena_size,
            static_cast<unsigned>(mapped.error()));
        if (!add_growth_arena_locked(1)) {
            VITREO_TRACE(
                "[crt] initial heap arena allocation failed -> ENOMEM\n");
            return;
        }
    } else {
        m_arenas.push_back(Arena{
            m_initial_arena_base, m_growth_arena_size,
            m_initial_arena_base});
        m_next_arena_search =
            m_initial_arena_base +
            static_cast<GuestAddr>(m_growth_arena_size);
        m_total_arena_size = m_growth_arena_size;
    }

    // Reserve the first 16 bytes for the 4-byte errno and _doserrno slots.
    Arena& first_arena = m_arenas.front();
    m_heap_reuse = crt_heap_reuse_from_env();
    m_errno_addr = first_arena.base;
    const GuestAddr first_bump =
        align_up(first_arena.base + 8, GuestAddr{16});
    first_arena.bump = first_bump;

    // Zero-initialise errno and _doserrno.
    (void)m_memory.write_u32(m_errno_addr, 0);
    (void)m_memory.write_u32(m_errno_addr + 4, 0);
}

// ---- per-thread errno / invalid parameter ------------------------------------

GuestAddr CrtMemoryManager::errno_addr() {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (tid == 0 || m_errno_addr == 0) return m_errno_addr;
    {
        std::lock_guard lock(m_thread_slot_lock);
        auto it = m_thread_errno_blocks.find(tid);
        if (it != m_thread_errno_blocks.end()) return it->second;
    }
    // A thread starts with errno == 0 and _doserrno == 0.
    const GuestAddr block = crt_malloc(8);
    if (block == 0) return m_errno_addr;
    (void)m_memory.write_u32(block, 0);
    (void)m_memory.write_u32(block + 4, 0);
    std::lock_guard lock(m_thread_slot_lock);
    auto [it, inserted] = m_thread_errno_blocks.emplace(tid, block);
    return it->second;
}

GuestAddr CrtMemoryManager::exchange_thread_invalid_parameter_handler(
    GuestAddr handler) {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_thread_slot_lock);
    GuestAddr& slot = m_thread_invalid_parameter_handlers[tid * 2 + static_cast<u32>(flavor_index())];
    const GuestAddr previous = slot;
    slot = handler;
    return previous;
}

GuestAddr CrtMemoryManager::thread_invalid_parameter_handler() {
    const u32 tid = ThreadManager::get_current_thread_id_static();
    std::lock_guard lock(m_thread_slot_lock);
    auto it = m_thread_invalid_parameter_handlers.find(tid * 2 + static_cast<u32>(flavor_index()));
    return it != m_thread_invalid_parameter_handlers.end() ? it->second : 0;
}

bool CrtMemoryManager::invalid_parameter() {
    GuestAddr handler = thread_invalid_parameter_handler();
    if (handler == 0) handler = m_invalid_parameter_handler[flavor_index()].load();
    if (handler != 0 && m_guest_caller) {
        // void handler(const wchar_t* expression, const wchar_t* function,
        //              const wchar_t* file, unsigned line, uintptr_t reserved)
        // Release UCRT passes empty (NULL/0) parameters.
        const u64 args[5] = {0, 0, 0, 0, 0};
        (void)m_guest_caller(handler, args, 5);
        return true;
    }
    if (CrtDllFlavorScope::current() == CrtDllFlavor::Msvcrt) return true;
    invoke_watson();
    return false;
}

void CrtMemoryManager::invoke_watson() {
    // UCRT _invoke_watson: __fastfail(FAST_FAIL_INVALID_ARG). A fast-fail
    // terminates with STATUS_STACK_BUFFER_OVERRUN (0xC0000409) as the exit
    // status (measured: crt_ucrt_probe).
    std::fprintf(stderr,
                 "[crt] invalid parameter with no handler -> fast fail "
                 "(0xC0000409)\n");
    terminate_process(0xC0000409u);
}

// ---- heap ------------------------------------------------------------------


namespace {
/// Set by set_out_of_memory_locked(); crt_malloc() stores ENOMEM in the
/// calling thread's errno after dropping the heap lock (errno_addr() may
/// itself allocate the thread's errno block).
thread_local bool t_crt_out_of_memory = false;
} // namespace

GuestAddr CrtMemoryManager::crt_malloc(usize size) {
    t_crt_out_of_memory = false;
    if (size == 0) size = 1;  // MSVC malloc(0) returns a unique pointer

    // Prefer the Win32 process heap when wired: `_get_heap_handle` returns that
    // heap, and HeapSize/HeapWalk must see CRT blocks (09_crt.md MEDIUM).
    if (m_win32_heap.alloc) {
        constexpr usize kMaxReq =
            std::numeric_limits<usize>::max() - 32;
        if (size > kMaxReq) {
            set_errno(12);
            return 0;
        }
        const u64 addr = m_win32_heap.alloc(size, 0);
        if (addr == 0) set_errno(12);
        return static_cast<GuestAddr>(addr);
    }

    const GuestAddr result = crt_malloc_heap_locked(size);
    if (result == 0 && t_crt_out_of_memory) {
        t_crt_out_of_memory = false;
        constexpr u32 ENOMEM_VALUE = 12;
        set_errno(ENOMEM_VALUE);
    }
    return result;
}

usize CrtMemoryManager::crt_block_size(GuestAddr ptr) const {
    if (ptr == 0) return 0;
    if (m_win32_heap.size) {
        const u64 sz = m_win32_heap.size(ptr);
        return sz == ~0ULL ? 0 : static_cast<usize>(sz);
    }
    if (ptr < 8) return 0;
    const auto stored = m_memory.read_u64(ptr - 8);
    if (!stored.is_ok()) return 0;
    const u64 size = stored.value();
    if (size == kFreedHeader) return 0;
    return static_cast<usize>(size);
}

GuestAddr CrtMemoryManager::crt_malloc_heap_locked(usize size) {
    if (size == 0) size = 1;  // MSVC malloc(0) returns a unique pointer

    std::lock_guard lock(m_heap_lock);
    // Learn malloc: fail when size exceeds _HEAP_MAXREQ (≈ SIZE_MAX on x64),
    // not at an artificial 4 GiB ceiling. Leave room for the size header and
    // 16-byte alignment so growth arithmetic cannot wrap.
    constexpr usize kMaxReq =
        std::numeric_limits<usize>::max() - (kSizeHeader + 16);
    if (size > kMaxReq) {
        set_out_of_memory_locked();
        return 0;
    }

    // Reuse a freed block of this size class before growing the arenas.
    if (m_heap_reuse) {
        const u32 capacity = crt_reuse_class(size);
        if (capacity != 0) {
            auto it = m_heap_free_lists.find(capacity);
            if (it != m_heap_free_lists.end() && !it->second.empty()) {
                const GuestAddr user_ptr = it->second.back();
                it->second.pop_back();
                m_heap_free_bytes -=
                    std::min<usize>(m_heap_free_bytes, capacity);
                // Rewrite the header: _msize and realloc read the REQUESTED
                // size from it, and this block is being handed out for a new
                // request.
                if (m_memory.write_u64(user_ptr - kSizeHeader,
                                       static_cast<u64>(size))
                        .is_ok()) {
                    return user_ptr;
                }
                // The header write failed, so the block is unusable; drop it
                // rather than hand back something whose size cannot be read.
            }
        }
    }

    if (!m_arenas.empty()) {
        const GuestAddr allocation =
            allocate_from_arena_locked(m_arenas.back(), size);
        if (allocation != 0) {
            return allocation;
        }
    }

    if (!add_growth_arena_locked(size)) {
        set_out_of_memory_locked();
        VITREO_TRACE(
            "[crt] heap growth failed request=0x%zx total_capacity=0x%zx "
            "next_search=0x%llx -> ENOMEM\n",
            size, m_total_arena_size,
            static_cast<unsigned long long>(m_next_arena_search));
        return 0;
    }

    const GuestAddr allocation =
        allocate_from_arena_locked(m_arenas.back(), size);
    if (allocation == 0) {
        set_out_of_memory_locked();
    }
    return allocation;
}

GuestAddr CrtMemoryManager::allocate_from_arena_locked(Arena& arena,
                                                        usize size) {
    // Store the requested size in an 8-byte header immediately before the
    // returned pointer, keeping the user pointer 16-byte aligned.
    const GuestAddr arena_end =
        arena.base + static_cast<GuestAddr>(arena.size);
    if (arena.bump > arena_end ||
        arena.bump > std::numeric_limits<GuestAddr>::max() -
                         static_cast<GuestAddr>(kSizeHeader + 15)) {
        return 0;
    }
    const GuestAddr header =
        align_up(arena.bump + static_cast<GuestAddr>(kSizeHeader),
                 GuestAddr{16}) -
        static_cast<GuestAddr>(kSizeHeader);
    const GuestAddr user_ptr =
        header + static_cast<GuestAddr>(kSizeHeader);
    // Lay the block out at its reuse class so a recycled block always has
    // the capacity its free list promises. Huge blocks skip reuse classes.
    const u32 reuse = crt_reuse_class(size);
    const usize span =
        reuse != 0 ? static_cast<usize>(reuse) : crt_block_capacity(size);
    if (user_ptr > arena_end ||
        span > static_cast<usize>(arena_end - user_ptr)) {
        return 0;
    }
    const GuestAddr end = user_ptr + static_cast<GuestAddr>(span);

    if (m_memory.write_u64(header, static_cast<u64>(size)).is_err()) {
        return 0;
    }

    arena.bump = align_up(end, GuestAddr{16});
    return user_ptr;
}

bool CrtMemoryManager::add_growth_arena_locked(usize allocation_size) {
    constexpr usize MAX_ATTEMPTS = 8;
    constexpr usize ALLOCATION_OVERHEAD = 16;

    if (allocation_size >
        std::numeric_limits<usize>::max() - ALLOCATION_OVERHEAD) {
        return false;
    }
    const usize required = allocation_size + ALLOCATION_OVERHEAD;
    if (required >
        std::numeric_limits<usize>::max() - (GUEST_PAGE_SIZE - 1)) {
        return false;
    }
    const usize minimum_size = align_up(required, GUEST_PAGE_SIZE);
    const usize arena_size = std::max(m_growth_arena_size, minimum_size);

    // find_free_gap only searches upward, so a bump-only search strands every
    // gap below the newest arena: once it reaches the top of the user address
    // range, malloc reports ENOMEM while most of the 2 GiB is still free.
    // Windows' CRT reserves segments with VirtualAlloc, which considers the
    // whole range, so sweep from the newest arena first (keeps arenas dense),
    // then from the heap base, and finally accept a request-sized arena.
    // Lowest address any user allocation may occupy (Windows keeps the first
    // 64 KiB permanently free).
    constexpr GuestAddr HEAP_SEARCH_FLOOR = 0x10000;
    const struct { GuestAddr search; usize size; } passes[] = {
        {m_next_arena_search, arena_size},
        {HEAP_SEARCH_FLOOR, arena_size},
        {HEAP_SEARCH_FLOOR, minimum_size},
    };

    for (const auto& pass : passes) {
        GuestAddr search = pass.search;
        for (usize attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            const GuestAddr base = m_memory.find_free_gap(search, pass.size);
            if (base == 0) {
                break;
            }

            auto mapped = m_memory.map_region(
                base, pass.size, MemoryProtection::ReadWrite);
            if (mapped.is_ok()) {
                m_arenas.push_back(Arena{base, pass.size, base});
                m_next_arena_search =
                    base + static_cast<GuestAddr>(pass.size);
                m_total_arena_size += pass.size;
                VITREO_TRACE(
                    "[crt] heap grew arena=%zu base=0x%llx size=0x%zx "
                    "total_capacity=0x%zx request=0x%zx\n",
                    m_arenas.size(),
                    static_cast<unsigned long long>(base), pass.size,
                    m_total_arena_size, allocation_size);
                return true;
            }

            if (mapped.error() != Error::AlreadyExists ||
                base > std::numeric_limits<GuestAddr>::max() - GUEST_PAGE_SIZE) {
                break;
            }
            search = base + GUEST_PAGE_SIZE;
        }
    }
    return false;
}

void CrtMemoryManager::set_out_of_memory_locked() {
    t_crt_out_of_memory = true;
}

void CrtMemoryManager::crt_free(GuestAddr ptr) {
    if (ptr == 0) return;

    // Win32 process-heap backend: always free through HeapFree.
    if (m_win32_heap.free) {
        (void)m_win32_heap.free(ptr);
        return;
    }

    // Arena backend: blocks go back on a free list keyed by capacity when
    // reuse is enabled (VITREO_CRT_HEAP_REUSE). Default OFF.
    if (!m_heap_reuse) return;

    std::lock_guard lock(m_heap_lock);
    // Only a block this allocator laid out may be recycled: a foreign
    // pointer's "header" is whatever happens to precede it.
    if ((ptr & 15u) != 0 || !owns_locked(ptr)) return;
    const auto stored = m_memory.read_u64(ptr - kSizeHeader);
    if (!stored.is_ok()) return;
    const u64 size = stored.value();
    if (size == kFreedHeader) return;     // double free: already listed
    if (size == 0 || size > std::numeric_limits<u32>::max()) return;
    if (m_memory.write_u64(ptr - kSizeHeader, kFreedHeader).is_err()) return;

    const u32 capacity = crt_reuse_class(static_cast<usize>(size));
    if (capacity == 0) return;
    m_heap_free_lists[capacity].push_back(ptr);
    m_heap_free_bytes += capacity;
}

bool CrtMemoryManager::owns_locked(GuestAddr user_ptr) const noexcept {
    for (const Arena& arena : m_arenas) {
        const GuestAddr lo =
            arena.base + static_cast<GuestAddr>(kSizeHeader);
        const GuestAddr hi = arena.base + static_cast<GuestAddr>(arena.size);
        if (user_ptr >= lo && user_ptr <= hi && user_ptr <= arena.bump) {
            return true;
        }
    }
    return false;
}

// ---- _onexit / __dllonexit -------------------------------------------------

GuestAddr CrtMemoryManager::register_onexit(GuestAddr func) {
    if (func == 0) return 0;
    std::lock_guard lock(m_onexit_lock);
    m_onexit_callbacks.push_back(func);
    return func;
}

GuestAddr CrtMemoryManager::register_dllonexit(GuestAddr func,
                                               GuestAddr /*begin*/,
                                               GuestAddr /*end*/) {
    return register_onexit(func);
}

i32 CrtMemoryManager::initialize_onexit_table(GuestAddr table, bool is_64bit) {
    if (table == 0) return -1;

    const usize pointer_size = is_64bit ? 8u : 4u;
    // UCRT: a table whose first != end is already initialised and is left
    // alone (crt_ucrt_rt_probe: "reinit=0 reinit_kept=1").
    {
        const auto first = is_64bit ? m_memory.read_u64(table)
                                    : m_memory.read_u32(table).map([](u32 v) { return u64{v}; });
        const auto end = is_64bit ? m_memory.read_u64(table + 16)
                                  : m_memory.read_u32(table + 8).map([](u32 v) { return u64{v}; });
        if (first.is_ok() && end.is_ok() && first.value() != end.value()) return 0;
    }
    std::array<Byte, 24> zeros{};
    return m_memory.write_bytes(table, zeros.data(), pointer_size * 3).is_ok()
        ? 0
        : -1;
}

i32 CrtMemoryManager::register_onexit_function(GuestAddr table, GuestAddr func,
                                               bool is_64bit) {
    if (table == 0 || func == 0) return -1;

    const GuestAddr pointer_size = is_64bit ? 8u : 4u;
    const auto read_pointer = [&](GuestAddr address) -> std::optional<GuestAddr> {
        if (is_64bit) {
            auto value = m_memory.read_u64(address);
            if (value.is_err()) return std::nullopt;
            return static_cast<GuestAddr>(value.value());
        }
        auto value = m_memory.read_u32(address);
        if (value.is_err()) return std::nullopt;
        return static_cast<GuestAddr>(value.value());
    };
    const auto write_pointer = [&](GuestAddr address, GuestAddr value) {
        return is_64bit
            ? m_memory.write_u64(address, value).is_ok()
            : m_memory.write_u32(address, static_cast<u32>(value)).is_ok();
    };

    std::lock_guard lock(m_onexit_lock);
    auto first = read_pointer(table);
    auto next = read_pointer(table + pointer_size);
    auto end = read_pointer(table + pointer_size * 2);
    if (!first || !next || !end) return -1;

    if (*first == 0 && *next == 0 && *end == 0) {
        constexpr usize INITIAL_CAPACITY = 32;
        const GuestAddr allocation =
            crt_malloc(INITIAL_CAPACITY * static_cast<usize>(pointer_size));
        if (allocation == 0) return -1;
        *first = allocation;
        *next = allocation;
        *end = allocation + INITIAL_CAPACITY * pointer_size;
    }

    if (*first == 0 || *next < *first || *end < *next ||
        ((*next - *first) % pointer_size) != 0 ||
        ((*end - *first) % pointer_size) != 0) {
        return -1;
    }

    if (*next == *end) {
        const usize old_bytes = static_cast<usize>(*end - *first);
        if (old_bytes > m_growth_arena_size) return -1;
        const usize new_bytes = old_bytes == 0
            ? 32 * static_cast<usize>(pointer_size)
            : old_bytes * 2;
        const GuestAddr replacement = crt_malloc(new_bytes);
        std::vector<Byte> existing(old_bytes);
        if (replacement == 0 ||
            m_memory.read_bytes(*first, existing.data(), existing.size()).is_err() ||
            m_memory.write_bytes(replacement, existing.data(), existing.size()).is_err()) {
            return -1;
        }
        *next = replacement + (*next - *first);
        *first = replacement;
        *end = replacement + new_bytes;
    }

    if (!write_pointer(*next, func)) return -1;
    *next += pointer_size;
    if (!write_pointer(table, *first) ||
        !write_pointer(table + pointer_size, *next) ||
        !write_pointer(table + pointer_size * 2, *end)) {
        return -1;
    }
    return 0;
}

i32 CrtMemoryManager::execute_onexit_table(GuestAddr table, bool is_64bit) {
    if (table == 0) return -1;

    const GuestAddr pointer_size = is_64bit ? 8u : 4u;
    const auto read_pointer = [&](GuestAddr address) -> std::optional<GuestAddr> {
        if (is_64bit) {
            auto value = m_memory.read_u64(address);
            if (value.is_err()) return std::nullopt;
            return static_cast<GuestAddr>(value.value());
        }
        auto value = m_memory.read_u32(address);
        if (value.is_err()) return std::nullopt;
        return static_cast<GuestAddr>(value.value());
    };
    const auto write_pointer = [&](GuestAddr address, GuestAddr value) {
        (void)(is_64bit ? m_memory.write_u64(address, value)
                        : m_memory.write_u32(address, static_cast<u32>(value)));
    };

    // UCRT runs the table last-to-first and re-reads it after every call, so
    // a function registered while the table runs is run too, next
    // (crt_ucrt_rt_probe: "ox_2 ox_late ox_1").
    auto first = read_pointer(table);
    auto last = read_pointer(table + pointer_size);
    if (!first || !last || *last < *first ||
        ((*last - *first) % pointer_size) != 0) {
        return -1;
    }
    GuestAddr lo = *first;
    GuestAddr pos = *last;
    while (pos > lo) {
        pos -= pointer_size;
        auto callback = read_pointer(pos);
        if (!callback || *callback == 0) continue;
        write_pointer(pos, 0);  // never run an entry twice
        if (m_void_callback) {
            m_void_callback(*callback);
        } else if (m_guest_caller) {
            (void)m_guest_caller(*callback, nullptr, 0);
        }
        auto new_first = read_pointer(table);
        auto new_last = read_pointer(table + pointer_size);
        if (!new_first || !new_last) return -1;
        if (*new_first != lo || *new_last != *last) {
            lo = *new_first;
            last = new_last;
            pos = *new_last;
        }
    }
    std::lock_guard lock(m_onexit_lock);
    std::array<Byte, 24> zeros{};
    return m_memory.write_bytes(table, zeros.data(), pointer_size * 3).is_ok()
        ? 0 : -1;
}

i32 CrtMemoryManager::crt_atexit(GuestAddr func) {
    return register_onexit(func) != 0 ? 0 : -1;
}

void CrtMemoryManager::run_atexit_handlers() {
    for (;;) {
        GuestAddr func = 0;
        {
            std::lock_guard lock(m_onexit_lock);
            if (m_onexit_callbacks.empty()) return;
            func = m_onexit_callbacks.back();
            m_onexit_callbacks.pop_back();
        }
        if (m_void_callback) {
            m_void_callback(func);
        } else if (m_guest_caller) {
            (void)m_guest_caller(func, nullptr, 0);
        }
    }
}

i32 CrtMemoryManager::crt_at_quick_exit(GuestAddr func) {
    if (func == 0) return 0;
    std::lock_guard lock(m_onexit_lock);
    m_quick_exit_callbacks.push_back(func);
    return 0;
}

void CrtMemoryManager::run_quick_exit_handlers() {
    for (;;) {
        GuestAddr func = 0;
        {
            std::lock_guard lock(m_onexit_lock);
            if (m_quick_exit_callbacks.empty()) return;
            func = m_quick_exit_callbacks.back();
            m_quick_exit_callbacks.pop_back();
        }
        if (m_void_callback) {
            m_void_callback(func);
        } else if (m_guest_caller) {
            (void)m_guest_caller(func, nullptr, 0);
        }
    }
}

// ---- qsort -----------------------------------------------------------------

void CrtMemoryManager::crt_qsort(GuestAddr base, usize count, usize width,
                                 GuestAddr compare) {
    if (count < 2) return;
    if (base == 0 || width == 0 || compare == 0 || !m_compare_callback) {
        VITREO_TRACE("[crt] qsort: invalid arguments or missing guest callback\n");
        return;
    }
    heap_sort(base, count, width, [&](GuestAddr lhs, GuestAddr rhs) {
        return m_compare_callback(compare, lhs, rhs);
    });
}

void CrtMemoryManager::crt_qsort_s(GuestAddr base, usize count, usize width,
                                   GuestAddr compare, GuestAddr context) {
    if (count < 2 || !m_guest_caller) return;
    heap_sort(base, count, width, [&](GuestAddr lhs, GuestAddr rhs) {
        const u64 args[3] = {context, lhs, rhs};
        return static_cast<i32>(static_cast<u32>(m_guest_caller(compare, args, 3)));
    });
}

void CrtMemoryManager::heap_sort(
    GuestAddr base, usize count, usize width,
    const std::function<i32(GuestAddr, GuestAddr)>& compare_addresses) {
    if (count > SIZE_MAX / width) {
        VITREO_TRACE("[crt] qsort: element range overflow\n");
        return;
    }
    const usize byte_count = count * width;
    if (byte_count > static_cast<usize>(~GuestAddr{0} - base)) {
        VITREO_TRACE("[crt] qsort: guest address overflow\n");
        return;
    }

    std::vector<Byte> left(width);
    std::vector<Byte> right(width);
    auto element = [base, width](usize index) {
        return base + static_cast<GuestAddr>(index * width);
    };
    auto compare_elements = [&](usize lhs, usize rhs) {
        return compare_addresses(element(lhs), element(rhs));
    };
    auto swap_elements = [&](usize lhs, usize rhs) {
        if (lhs == rhs) return true;
        const GuestAddr lhs_addr = element(lhs);
        const GuestAddr rhs_addr = element(rhs);
        if (m_memory.read_bytes(lhs_addr, left.data(), width).is_err() ||
            m_memory.read_bytes(rhs_addr, right.data(), width).is_err()) {
            return false;
        }
        return m_memory.write_bytes(lhs_addr, right.data(), width).is_ok() &&
               m_memory.write_bytes(rhs_addr, left.data(), width).is_ok();
    };

    auto sift_down = [&](usize start, usize end) {
        if (start >= end) return true;
        usize root = start;
        while (root <= (end - 1) / 2) {
            usize child = root * 2 + 1;
            if (child < end && compare_elements(child, child + 1) < 0) {
                ++child;
            }
            if (compare_elements(root, child) >= 0) return true;
            if (!swap_elements(root, child)) return false;
            root = child;
        }
        return true;
    };

    for (usize start = count / 2; start > 0; --start) {
        if (!sift_down(start - 1, count - 1)) return;
    }
    for (usize end = count - 1; end > 0; --end) {
        if (!swap_elements(0, end) || !sift_down(0, end - 1)) return;
    }
}

GuestAddr CrtMemoryManager::crt_bsearch(GuestAddr key, GuestAddr base, usize count,
                                        usize width, GuestAddr compare) {
    if (key == 0 || base == 0 || width == 0 || compare == 0 ||
        !m_compare_callback || count == 0) {
        return 0;
    }
    if (count > SIZE_MAX / width) return 0;
    const usize byte_count = count * width;
    if (byte_count > static_cast<usize>(~GuestAddr{0} - base)) return 0;

    usize lo = 0;
    usize hi = count;
    while (lo < hi) {
        const usize mid = lo + (hi - lo) / 2;
        const GuestAddr elem =
            base + static_cast<GuestAddr>(mid * width);
        const i32 cmp = m_compare_callback(compare, key, elem);
        if (cmp == 0) return elem;
        if (cmp < 0) hi = mid;
        else lo = mid + 1;
    }
    return 0;
}


} // namespace vitreo::winapi

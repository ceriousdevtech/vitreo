// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_memory.hpp
/// @brief msvcrt memory, exception, and misc runtime stubs

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "core/memory/guest_memory.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

/// Which CRT DLL's export is running on this thread. The export tables are
/// built from one implementation, but msvcrt.dll and the UCRT differ where a
/// parameter check fails with no handler installed: the UCRT fast-fails
/// (_invoke_watson), while the system msvcrt.dll -- which exports no handler
/// setter -- returns the documented error to its caller (measured,
/// tools/oracle_probe/crt_msvcrt_stdio_probe: strcpy_s/_itoa_s/fclose failures
/// and msvcrt!_invalid_parameter all return). dll_registry tags every msvcrt
/// and ucrtbase export with its DLL.
enum class CrtDllFlavor : u8 { Ucrt, Msvcrt };

class CrtDllFlavorScope {
public:
    explicit CrtDllFlavorScope(CrtDllFlavor flavor) : m_previous(slot()) {
        slot() = flavor;
    }
    ~CrtDllFlavorScope() { slot() = m_previous; }
    CrtDllFlavorScope(const CrtDllFlavorScope&) = delete;
    CrtDllFlavorScope& operator=(const CrtDllFlavorScope&) = delete;
    [[nodiscard]] static CrtDllFlavor current() noexcept { return slot(); }

private:
    static CrtDllFlavor& slot() noexcept {
        thread_local CrtDllFlavor flavor = CrtDllFlavor::Ucrt;
        return flavor;
    }
    CrtDllFlavor m_previous;
};

class Marshaler;

/// Manages CRT heap allocation (segmented monotonic arenas in guest memory),
/// errno storage, and miscellaneous runtime stubs (exception handling,
/// locking, _onexit, etc.).
class CrtMemoryManager {
public:
    /// Supplies the Win32 heap handle `_get_heap_handle` reports. UCRT uses
    /// the process heap; set by the DLL registry.
    void set_process_heap_provider(std::function<u64()> provider) {
        m_process_heap_provider = std::move(provider);
    }
    /// Returns 0 when unset; callers treat that as "no CRT heap handle".
    [[nodiscard]] u64 process_heap_handle() const {
        return m_process_heap_provider ? m_process_heap_provider() : 0u;
    }

    /// Optional Win32 HeapAlloc/HeapFree/HeapSize/HeapReAlloc backend. When
    /// wired (dll_registry), CRT malloc/free go through the process heap so
    /// HeapSize/HeapValidate/HeapWalk on `_get_heap_handle()` see CRT blocks.
    /// Unit tests leave this unset and keep the private arena allocator.
    struct Win32HeapOps {
        std::function<u64(u64 size, u32 flags)> alloc;
        std::function<bool(u64 address)> free;
        std::function<u64(u64 address)> size;
        std::function<u64(u64 address, u64 new_size, u32 flags)> realloc;
    };
    void set_win32_heap_ops(Win32HeapOps ops) {
        m_win32_heap = std::move(ops);
    }
    [[nodiscard]] bool uses_win32_heap() const noexcept {
        return static_cast<bool>(m_win32_heap.alloc);
    }
    /// Requested size of a CRT block (HeapSize or arena header). 0 if unknown.
    [[nodiscard]] usize crt_block_size(GuestAddr ptr) const;

    /// @param arena_base  Guest address for the heap arena start. Default is
    ///        high in 32-bit user space so we do not steal common PE
    ///        ImageBases (0x10000000 / 0x20000000 / …) that packers and
    ///        DRM DLLs require; Windows CRT heaps are not fixed at those
    ///        addresses either.
    ///
    ///        Packed against the other top-of-window fixtures (SEH protector
    ///        0x7BFE0000, module arena 0x7C000000, thunks 0x7D000000) rather
    ///        than the old 0x78000000: a fixture in the middle of the 2 GB
    ///        window splits the largest free run a non-LAA guest can get.
    ///        A guest that sizes its loading pools from ullAvailPhys on a
    ///        quiet 24 GB host demands ~1.87 GB across large blocks; with the
    ///        arena mid-window a 135 MB block had no gap (largest run
    ///        129 MB) and loading hung on an endless retry.
    ///        Windows clusters its own fixtures at the top of the window, so
    ///        this is also the contract-faithful layout.
    CrtMemoryManager(GuestMemory& memory,
                     GuestAddr arena_base = 0x7A000000,
                     usize arena_size = 16 * 1024 * 1024);

    CrtMemoryManager(const CrtMemoryManager&) = delete;
    CrtMemoryManager& operator=(const CrtMemoryManager&) = delete;
    ~CrtMemoryManager() = default;

    // ---- heap ----

    /// Monotonically allocate `size` bytes (16-byte aligned), adding another
    /// guest-memory arena when the current arena is full. Returns 0 when no
    /// suitable guest range can be mapped.
    [[nodiscard]] GuestAddr crt_malloc(usize size);

    /// Free a CRT heap block. By default arenas grow monotonically (addresses
    /// are not reused) because recycling exposed live-object UAFs in guests;
    /// VITREO_CRT_HEAP_REUSE=1 enables the free-list path for measurement.
    void crt_free(GuestAddr ptr);

    // ---- errno ----

    /// Guest address of the CALLING guest thread's errno (u32).
    ///
    /// UCRT errno.h: `#define errno (*_errno())`, and C11 7.5p2 gives errno
    /// thread storage duration; the UCRT keeps it in its per-thread data
    /// block. One process-wide slot let two threads clobber each other's
    /// errno (09_crt.md cross-cutting finding). Each guest thread gets its own
    /// 8-byte {errno, _doserrno} block on first use, keyed by guest tid; code
    /// with no guest thread (tid 0, unit tests) uses the initial block.
    [[nodiscard]] GuestAddr errno_addr();

    /// Guest address of the calling thread's _doserrno (u32).
    /// Learn `__doserrno` returns `int*`, and the UCRT dereferences it
    /// immediately (`*__doserrno() = 0`). Returning the missing-export
    /// sentinel instead made that a write to 0xFFFFFFFF.
    [[nodiscard]] GuestAddr doserrno_addr() { return errno_addr() + 4; }

    /// `errno = value` for the calling guest thread.
    void set_errno(u32 value) { (void)m_memory.write_u32(errno_addr(), value); }
    /// `_doserrno = value` for the calling guest thread.
    void set_doserrno(u32 value) {
        (void)m_memory.write_u32(doserrno_addr(), value);
    }
    [[nodiscard]] u32 get_errno() {
        return m_memory.read_u32(errno_addr()).value_or(0);
    }

    // ---- guest calls / invalid parameter / termination ----

    /// Calls a guest __cdecl function with integer/pointer arguments. Injected
    /// by the run loop (same mechanism as combase's GuestFunctionCaller).
    using GuestCaller =
        std::function<u64(GuestAddr function, const u64* args, usize count)>;
    void set_guest_caller(GuestCaller caller) { m_guest_caller = std::move(caller); }
    [[nodiscard]] bool has_guest_caller() const noexcept {
        return static_cast<bool>(m_guest_caller);
    }
    /// Returns 0 when no caller is installed (callers must check
    /// has_guest_caller() when the call is required for the contract).
    u64 call_guest(GuestAddr function, const u64* args, usize count) {
        return m_guest_caller ? m_guest_caller(function, args, count) : 0;
    }

    /// Terminates the process with the given exit status (ProcessManager
    /// exit_process). Injected by the DLL registry.
    void set_process_terminator(std::function<void(u32)> fn) {
        m_terminator = std::move(fn);
    }
    void terminate_process(u32 status) {
        if (m_terminator) m_terminator(status);
    }

    /// _set_invalid_parameter_handler / _get_invalid_parameter_handler. Each
    /// CRT DLL keeps its own (CrtDllFlavorScope): a handler installed through
    /// ucrtbase is never run by msvcrt.dll (measured, crt_strtod_probe), while
    /// one installed through an msvcr* alias is run by that family.
    GuestAddr exchange_invalid_parameter_handler(GuestAddr handler) {
        return handler_slot().exchange(handler);
    }
    [[nodiscard]] GuestAddr invalid_parameter_handler() const noexcept {
        return m_invalid_parameter_handler[flavor_index()].load();
    }
    /// _set/_get_thread_local_invalid_parameter_handler (per guest thread).
    GuestAddr exchange_thread_invalid_parameter_handler(GuestAddr handler);
    [[nodiscard]] GuestAddr thread_invalid_parameter_handler();

    /// UCRT `_invalid_parameter(NULL, NULL, NULL, 0, 0)` — what every CRT
    /// function does on a parameter-validation failure (Learn "Parameter
    /// validation", invalid-parameter-functions): calls the calling thread's
    /// handler if one is set, else the process-wide handler, and RETURNS true
    /// so the caller sets errno and returns its documented error value. With
    /// no handler it calls _invoke_watson: the process fast-fails
    /// (status 0xC0000409, measured by crt_ucrt_probe) and this returns false;
    /// the caller must then return immediately without further guest effects.
    /// Inside an msvcrt.dll export (CrtDllFlavorScope) a missing handler is
    /// not fatal: this returns true and the caller reports its error.
    bool invalid_parameter();

    /// `_invoke_watson`: __fastfail(FAST_FAIL_INVALID_ARG) — terminates.
    void invoke_watson();

    /// _set_new_handler / _query_new_handler (process-wide _PNH).
    GuestAddr exchange_new_handler(GuestAddr handler) {
        return m_new_handler.exchange(handler);
    }
    [[nodiscard]] GuestAddr new_handler() const noexcept {
        return m_new_handler.load();
    }

    // ---- _onexit / __dllonexit ----

    /// Store a callback address; return 0 on failure.
    [[nodiscard]] GuestAddr register_onexit(GuestAddr func);

    /// DLL variant — same semantics for our stub.
    [[nodiscard]] GuestAddr register_dllonexit(GuestAddr func,
                                               GuestAddr begin,
                                               GuestAddr end);

    /// UCRT on-exit table operations. The guest structure contains three
    /// pointer-sized fields: first callback, next slot, and allocation end.
    [[nodiscard]] i32 initialize_onexit_table(GuestAddr table, bool is_64bit);
    [[nodiscard]] i32 register_onexit_function(GuestAddr table, GuestAddr func,
                                               bool is_64bit);
    [[nodiscard]] i32 execute_onexit_table(GuestAddr table, bool is_64bit);
    [[nodiscard]] i32 crt_atexit(GuestAddr func);

    /// exit()/_cexit(): call the atexit/_onexit/_crt_atexit registrations in
    /// LIFO order (Learn exit/_cexit). A function registered while the list
    /// is running is run too, as the UCRT does. The list is empty afterwards.
    void run_atexit_handlers();

    /// int _crt_at_quick_exit(_PVFV) (corecrt_startup.h): the EXE's
    /// at_quick_exit registration, the list quick_exit runs. Measured
    /// (crt_ucrt_s6_probe "q" rows): returns 0, NULL included, and a NULL
    /// registration is never called.
    [[nodiscard]] i32 crt_at_quick_exit(GuestAddr func);

    /// quick_exit(): call the _crt_at_quick_exit registrations in LIFO order
    /// (Learn quick_exit); one registered while the list runs is run next
    /// (measured "nested": B R E A). The atexit list is not touched.
    void run_quick_exit_handlers();

    using GuestVoidCallback = std::function<void(GuestAddr)>;
    void set_void_callback(GuestVoidCallback cb) {
        m_void_callback = std::move(cb);
    }

    // ---- qsort ----

    using GuestCompareCallback = std::function<i32(GuestAddr, GuestAddr, GuestAddr)>;
    void set_compare_callback(GuestCompareCallback cb) { m_compare_callback = std::move(cb); }

    /// Invoke an MSVC EH3 `__except` filter (or `__finally`) with parent EBP.
    using GuestSehFilterCallback = std::function<i32(GuestAddr, u32)>;
    void set_seh_filter_callback(GuestSehFilterCallback cb) {
        m_seh_filter_callback = std::move(cb);
    }
    [[nodiscard]] i32 call_seh_filter(GuestAddr filter, u32 parent_ebp) const {
        if (!m_seh_filter_callback || filter == 0) return 0;
        return m_seh_filter_callback(filter, parent_ebp);
    }
    void crt_qsort(GuestAddr base, usize count, usize width, GuestAddr compare);
    /// qsort_s's sort: compare(context, a, b) through the guest caller.
    /// Arguments are already validated by the export.
    void crt_qsort_s(GuestAddr base, usize count, usize width, GuestAddr compare,
                     GuestAddr context);
    [[nodiscard]] GuestAddr crt_bsearch(GuestAddr key, GuestAddr base, usize count,
                                        usize width, GuestAddr compare);

private:
    std::function<u64()> m_process_heap_provider;
    Win32HeapOps m_win32_heap;
    struct Arena {
        GuestAddr base;
        usize size;
        GuestAddr bump;
    };

    [[nodiscard]] GuestAddr crt_malloc_heap_locked(usize size);
    [[nodiscard]] bool add_growth_arena_locked(usize allocation_size);
    [[nodiscard]] GuestAddr allocate_from_arena_locked(Arena& arena,
                                                        usize size);
    void set_out_of_memory_locked();

    GuestMemory& m_memory;
    GuestAddr m_initial_arena_base;
    usize m_growth_arena_size;
    GuestAddr m_next_arena_search;
    usize m_total_arena_size;
    /// True when @p user_ptr lies inside an arena this manager laid out.
    [[nodiscard]] bool owns_locked(GuestAddr user_ptr) const noexcept;
    std::vector<Arena> m_arenas;
    std::mutex m_heap_lock;

    /// Freed blocks available for reuse, keyed by EXACT block capacity.
    ///
    /// crt_free() used to do nothing at all: the arenas were monotonic, so a
    /// guest that allocated and freed in a loop grew the heap forever. That is
    /// worth gigabytes — tools/stress `sc_alloc` holds a live set under 2 MB by
    /// construction and reached 6.2 GB resident, the worst row in the corpus by
    /// raw ratio.
    ///
    /// The key is the capacity a block physically has, not the size that was
    /// requested, so a recycled block is never smaller than the new request.
    /// See crt_block_capacity().
    std::unordered_map<u32, std::vector<GuestAddr>> m_heap_free_lists;
    /// Bytes currently parked in m_heap_free_lists (statistics only).
    usize m_heap_free_bytes{0};
    /// VITREO_CRT_HEAP_REUSE=1, read once per manager. Per-instance rather than
    /// a function-local static so a test can set the variable and construct a
    /// fresh manager to exercise the other policy. Default false (fail closed).
    bool m_heap_reuse{false};
    GuestAddr m_errno_addr; ///< tid-0 {errno,_doserrno} block (first arena)
    std::mutex m_thread_slot_lock;
    std::unordered_map<u32, GuestAddr> m_thread_errno_blocks;
    std::unordered_map<u32, GuestAddr> m_thread_invalid_parameter_handlers;
    std::atomic<GuestAddr> m_invalid_parameter_handler[2]{};
    static usize flavor_index() noexcept {
        return CrtDllFlavorScope::current() == CrtDllFlavor::Msvcrt ? 1 : 0;
    }
    std::atomic<GuestAddr>& handler_slot() noexcept {
        return m_invalid_parameter_handler[flavor_index()];
    }
    std::atomic<GuestAddr> m_new_handler{0};
    GuestCaller m_guest_caller;
    std::function<void(u32)> m_terminator;

    /// In-place heap sort of `count` elements of `width` bytes at `base`.
    void heap_sort(GuestAddr base, usize count, usize width,
                   const std::function<i32(GuestAddr, GuestAddr)>& compare_addresses);

    std::vector<GuestAddr> m_onexit_callbacks;
    std::vector<GuestAddr> m_quick_exit_callbacks;  ///< guarded by m_onexit_lock
    std::mutex m_onexit_lock;
    GuestVoidCallback m_void_callback;
    GuestCompareCallback m_compare_callback;
    GuestSehFilterCallback m_seh_filter_callback;
};

/// Register all memory / exception / misc thunks for msvcrt.dll
DllExports create_crt_memory_thunks(CrtMemoryManager& mgr);

/// msvcp80.dll exports that TSL-class guests import. iostream methods whose
/// VC8 object layout is not implemented fail at the call.
DllExports create_msvcp80_thunks(CrtMemoryManager& mgr);

/// The _aligned_* allocation family (crt_aligned.cpp, UCRT align.cpp
/// layout); appended by create_crt_memory_thunks.
void add_crt_aligned_exports(DllExports& exports, CrtMemoryManager& mm);

} // namespace vitreo::winapi

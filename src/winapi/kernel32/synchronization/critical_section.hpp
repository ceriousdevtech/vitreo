#pragma once

/// @file critical_section.hpp
/// @brief CRITICAL_SECTION with guest memory layout (WIN32-065)

#include <cstdio>
#include <vitreo/common.hpp>
#include "core/memory/guest_memory.hpp"
#include "winapi/core/thunk_registry.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>

namespace vitreo::winapi {

class Marshaler;

// =============================================================================
// Guest CRITICAL_SECTION Layout
// =============================================================================
//
// Real Win32 RTL_CRITICAL_SECTION layout differs by bitness:
//
//   32-bit (PE32):                       offset  size
//     PRTL_CRITICAL_SECTION_DEBUG DebugInfo;   0    4
//     LONG  LockCount;                          4    4
//     LONG  RecursionCount;                     8    4
//     HANDLE OwningThread;                     12    4
//     HANDLE LockSemaphore;                    16    4
//     ULONG_PTR SpinCount;                     20    4
//   total size = 24 bytes
//
//   64-bit (PE32+):                      offset  size
//     PRTL_CRITICAL_SECTION_DEBUG DebugInfo;   0    8
//     LONG  LockCount;                          8    4
//     LONG  RecursionCount;                    12    4
//     HANDLE OwningThread;                     16    8 (32-bit field but padded)
//     HANDLE LockSemaphore;                    24    8
//     ULONG_PTR SpinCount;                     32    8
//   total size = 40 bytes
//
// Writing 64-bit offsets into a 32-bit guest CS smashes adjacent fields
// of containing structures with stale spin/owner values, manifesting as
// wild pointer values (e.g. spin=0xFA0 read as a pointer → AV).

struct CsLayout {
    u32 size;
    u32 off_debug_info;
    u32 off_lock_count;
    u32 off_recursion_count;
    u32 off_owning_thread;
    u32 off_lock_semaphore;
    u32 off_spin_count;
    u32 ptr_size; // size of a HANDLE / pointer field
};

constexpr CsLayout cs_layout_32{
    /*size=*/24,
    /*debug=*/0,
    /*lock=*/4,
    /*rec=*/8,
    /*owner=*/12,
    /*sem=*/16,
    /*spin=*/20,
    /*ptr=*/4,
};

constexpr CsLayout cs_layout_64{
    /*size=*/40,
    /*debug=*/0,
    /*lock=*/8,
    /*rec=*/12,
    /*owner=*/16,
    /*sem=*/24,
    /*spin=*/32,
    /*ptr=*/8,
};

namespace cs_layout {
    // Server 2003 SP1+ LockCount encoding (Learn CRITICAL_SECTION debugger notes):
    //   free              = -1
    //   held, no waiters  = -2   (bit0 clear = locked)
    //   held, n waiters   = -2 - 4*n   (CS_LOCK_WAITER_INC per waiter)
    constexpr u32 UNLOCKED           = 0xFFFFFFFFu; // -1
    constexpr u32 LOCKED_NO_WAITERS  = 0xFFFFFFFEu; // -2
    constexpr u32 WAITER_INC         = 4u;
    // Convenience offset constants for tests that target the 32-bit layout.
    constexpr u32 OFF_DEBUG_INFO      = 0;
    constexpr u32 OFF_LOCK_COUNT      = 4;
    constexpr u32 OFF_RECURSION_COUNT = 8;
    constexpr u32 OFF_OWNING_THREAD   = 12;
    constexpr u32 OFF_LOCK_SEMAPHORE  = 16;
    constexpr u32 OFF_SPIN_COUNT      = 20;
} // namespace cs_layout

// =============================================================================
// SpinCount flag byte (winnt.h; tools/audit_critical_section_constants.py)
// =============================================================================
//
// "These flags define the upper byte of the critical section SpinCount field"
// (winnt.h). InitializeCriticalSectionEx accepts them in Flags;
// CRITICAL_SECTION_NO_DEBUG_INFO (winbase.h) is the first.

namespace cs_flags {
    constexpr u32 RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO    = 0x01000000u;
    constexpr u32 RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN     = 0x02000000u;
    constexpr u32 RTL_CRITICAL_SECTION_FLAG_STATIC_INIT      = 0x04000000u;
    constexpr u32 RTL_CRITICAL_SECTION_FLAG_RESOURCE_TYPE    = 0x08000000u;
    constexpr u32 RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO = 0x10000000u;
    constexpr u32 RTL_CRITICAL_SECTION_ALL_FLAG_BITS         = 0xFF000000u;
    constexpr u32 RTL_CRITICAL_SECTION_FLAG_RESERVED =
        RTL_CRITICAL_SECTION_ALL_FLAG_BITS &
        ~(RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO |
          RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN |
          RTL_CRITICAL_SECTION_FLAG_STATIC_INIT |
          RTL_CRITICAL_SECTION_FLAG_RESOURCE_TYPE |
          RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO);
    constexpr u32 CRITICAL_SECTION_NO_DEBUG_INFO =
        RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO;
    /// The iteration count: SpinCount without its flag byte.
    constexpr u32 SPIN_COUNT_MASK = ~RTL_CRITICAL_SECTION_ALL_FLAG_BITS;
    /// Measured, not documented: the count Windows stores beside
    /// RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN when the caller asks for the
    /// dynamic flag or for a zero count (handles_waits_probe.expected:151,
    /// cs_init_flags_probe.expected ex_spin*_dynamic).
    constexpr u32 DEFAULT_DYNAMIC_SPIN_COUNT = 2000;
} // namespace cs_flags

// =============================================================================
// RTL_CRITICAL_SECTION_DEBUG layout (winnt.h)
// =============================================================================
//
//   WORD Type; WORD CreatorBackTraceIndex; PRTL_CRITICAL_SECTION
//   CriticalSection; LIST_ENTRY ProcessLocksList; DWORD EntryCount; DWORD
//   ContentionCount; DWORD Flags; WORD CreatorBackTraceIndexHigh; WORD
//   Identifier.

struct CsDebugLayout {
    u32 size;
    u32 off_type;
    u32 off_critical_section;
    u32 off_process_locks_list;
    u32 off_entry_count;
    u32 off_contention_count;
    u32 off_flags;
    u32 off_creator_back_trace_index_high;
    u32 off_identifier;
};

constexpr CsDebugLayout cs_debug_layout_32{
    /*size=*/32, /*type=*/0, /*cs=*/4, /*list=*/8, /*entry=*/16,
    /*contention=*/20, /*flags=*/24, /*cbti_high=*/28, /*identifier=*/30,
};

constexpr CsDebugLayout cs_debug_layout_64{
    /*size=*/48, /*type=*/0, /*cs=*/8, /*list=*/16, /*entry=*/32,
    /*contention=*/36, /*flags=*/40, /*cbti_high=*/44, /*identifier=*/46,
};

/// A guest access the API would have faulted on: Windows raises
/// STATUS_ACCESS_VIOLATION with this access (write or read) and address.
struct CsGuestFault {
    GuestAddr address = 0;
    bool write = false;
};

/// InitializeCriticalSectionEx's outcome: TRUE, FALSE with a last error, or
/// a fault (`fault` set; nothing is returned).
struct CsInitExResult {
    bool ok = false;
    u32 error = 0;
    std::optional<CsGuestFault> fault;
};

/// SetCriticalSectionSpinCount's outcome.
struct CsSetSpinResult {
    u32 previous = 0;
    std::optional<CsGuestFault> fault;
};

// =============================================================================
// CriticalSectionManager
// =============================================================================

/// Manages CRITICAL_SECTION operations with guest memory layout.
///
/// Unlike the THREAD-002 SyncManager which keeps critical sections purely
/// host-side, this manager writes the actual CRITICAL_SECTION structure
/// to guest memory so that the CRT can read its fields directly.
class CriticalSectionManager {
public:
    explicit CriticalSectionManager(GuestMemory& memory);

    CriticalSectionManager(const CriticalSectionManager&) = delete;
    CriticalSectionManager& operator=(const CriticalSectionManager&) = delete;
    ~CriticalSectionManager() = default;

    /// Inform the manager whether the guest process is 32- or 64-bit.
    /// Process bitness is fixed for a single run, so this is a one-shot
    /// latch: subsequent calls are ignored. Called from the impl thunks
    /// (the first one to fire wins, since CS is process-scoped state).
    void note_bitness(bool is_64bit) noexcept;

    /// InitializeCriticalSection: InitializeCriticalSectionAndSpinCount(cs, 0).
    /// Returns the fault when the section is not writable.
    std::optional<CsGuestFault> initialize(GuestAddr cs_addr);

    /// InitializeCriticalSectionAndSpinCount. Always succeeds (Learn, Vista+)
    /// unless the section is not writable. The high byte of dwSpinCount is
    /// discarded; a zero count is stored as the dynamic default.
    std::optional<CsGuestFault> initialize_and_spin_count(GuestAddr cs_addr,
                                                          u32 spin_count);

    /// InitializeCriticalSectionEx: validates Flags and dwSpinCount
    /// (ERROR_INVALID_PARAMETER) before touching the section.
    CsInitExResult initialize_ex(GuestAddr cs_addr, u32 spin_count, u32 flags);

    /// SetCriticalSectionSpinCount: install a new count, keep the flag byte,
    /// return the previous count without its flag byte. Reads and writes the
    /// guest SpinCount field only; it never creates host state.
    CsSetSpinResult set_spin_count(GuestAddr cs_addr, u32 spin_count);

    /// The SpinCount word Windows stores for a count and flag byte (a zero
    /// count or RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN become the dynamic
    /// default; on a uniprocessor the count is 0).
    [[nodiscard]] static u32 stored_spin_word(u32 count, u32 flags) noexcept;

    /// EnterCriticalSection
    void enter(GuestAddr cs_addr);

    /// TryEnterCriticalSection
    [[nodiscard]] bool try_enter(GuestAddr cs_addr);

    /// LeaveCriticalSection
    void leave(GuestAddr cs_addr);

    /// DeleteCriticalSection
    void delete_cs(GuestAddr cs_addr);

    /// Learn EnterCriticalSection: while the process is exiting, a call that
    /// would block terminates immediately instead of parking. Wake every
    /// enter_slow waiter and refuse new contended parks.
    void begin_shutdown();

    /// Contended entries that had to wait (park) for the section, process
    /// total. An entry that took the section while spinning is not one.
    [[nodiscard]] u64 contended_waits() const noexcept {
        return m_contended_waits.load(std::memory_order_relaxed);
    }

    /// TEST-BUILD-ONLY seam: LeaveCriticalSection with `hook` run right after
    /// the releasing store, the window in which a peer may acquire, leave and
    /// delete the section. Declared here, DEFINED only in the test sources
    /// (tests/winapi/critical_section_test_seam.cpp): the shipping Leave is
    /// the same template instantiated without the hook, so it carries no seam
    /// code at all, and a production caller of this function fails to link.
    using ReleasedHook = void (*)(GuestAddr cs_addr);
    void leave_with_released_hook_for_test(GuestAddr cs_addr, ReleasedHook hook);

    /// CsState objects allocated so far (live, recycled and retired), for
    /// tests. See m_free_states for the bound.
    [[nodiscard]] u32 state_objects_for_test() const noexcept {
        return m_state_objects.load(std::memory_order_relaxed);
    }

    /// Guest TID currently owning this CS (0 if unlocked / unknown).
    [[nodiscard]] u32 owning_thread(GuestAddr cs_addr);
    /// Active recursion depth for this CS (0 if unlocked / unknown).
    [[nodiscard]] u32 recursion_count(GuestAddr cs_addr);

private:
    GuestMemory& m_memory;

    /// Latched on first impl-thunk call; defaults to 32-bit until
    /// note_bitness() records the CPU bitness, before the first init/enter
    /// call.
    std::atomic<int> m_bitness_latched{0}; // 0=unset, 1=32, 2=64

    [[nodiscard]] const CsLayout& layout() const noexcept {
        return m_bitness_latched.load(std::memory_order_acquire) == 2
                   ? cs_layout_64
                   : cs_layout_32;
    }

    /// Host-side backing state per CS address.
    ///
    /// `mtx` is retained for the legacy path (VITREO_NO_CS_ACCEL). The
    /// accelerated protocol does NOT use it: there the authority is the guest's
    /// own LockCount word, so that emitted guest code and host callers contend
    /// for the same thing, and these two only provide parking.
    struct CsState {
        std::recursive_timed_mutex mtx;
        std::mutex park_mtx;
        std::condition_variable park_cv;
        /// Legacy path only: how many times the owner holds `mtx` (written by
        /// the holder, under it). The accelerated path keeps recursion in the
        /// guest's RecursionCount and never touches this, so it is exactly
        /// the host mutex's hold depth (release_for_reuse_locked relies on
        /// that).
        u32 recursion_count{0};
        /// Guest TID of the host-side owner: the legacy path's owner, and on
        /// the accelerated path the owner enter_slow installed (the hang
        /// dump's host_owner=). Relaxed atomic: the hang dump reads it
        /// unlocked and leave_wake clears it after the section is released.
        std::atomic<u32> owning_thread{0};
        /// The section's RTL_CRITICAL_SECTION_DEBUG, 0 while DebugInfo is
        /// (PVOID)-1. Guarded by park_mtx after initialisation.
        GuestAddr debug_info{0};
        /// Contended waiters registered in enter_slow. Host-only so Leave can
        /// decide whether to wake without touching the guest CRITICAL_SECTION
        /// after ownership is released (Learn LeaveCriticalSection).
        std::atomic<u32> registered_waiters{0};
        /// True once LockSemaphore has been published (keyed-event sentinel).
        bool lock_semaphore_published{false};

        /// Back to a fresh section's bookkeeping when a recycled state is
        /// handed out. Only states release_for_reuse_locked accepted are
        /// recycled, so `mtx` is unowned and no waiter is registered;
        /// park_mtx and park_cv need nothing.
        void reset_for_reuse() noexcept {
            recursion_count = 0;
            owning_thread.store(0, std::memory_order_relaxed);
            debug_info = 0;
            lock_semaphore_published = false;
        }
    };

    /// Section states are TYPE-STABLE: once allocated, a CsState is never
    /// freed while the manager lives. DeleteCriticalSection (and
    /// re-initialising a live section) moves it to m_free_states, and new
    /// sections take states from there before allocating. A
    /// LeaveCriticalSection still past its releasing store when a peer
    /// acquires, leaves and deletes the section therefore touches valid
    /// memory -- at worst a recycled state's waiter count and park
    /// condition, i.e. one spurious notify that enter_slow's predicate
    /// absorbs. (The same holds for a legacy-path unlock and a parked
    /// enter_slow that outlive the delete.)
    ///
    /// MEMORY BOUND: every state is live (m_states) or free, and a new one is
    /// allocated only when the free list is empty, so the total is the PEAK
    /// number of simultaneously live sections -- plus m_retired_states:
    /// sections deleted while another thread still held them or waited on
    /// them (undefined per Learn), whose state cannot be handed to an
    /// unrelated section and is kept, unused, instead. All three containers
    /// are guarded by m_map_lock.
    std::unordered_map<GuestAddr, std::unique_ptr<CsState>> m_states;
    std::vector<std::unique_ptr<CsState>> m_free_states;
    std::vector<std::unique_ptr<CsState>> m_retired_states;
    /// CsState objects ever allocated (live + free + retired), for tests.
    std::atomic<u32> m_state_objects{0};
    std::mutex m_map_lock;
    std::atomic<bool> m_shutdown{false};
    std::atomic<u64> m_contended_waits{0};

public:
    /// Print every tracked section's live words into a hang dump.
    void dump_locked_sections(std::FILE* out);

private:
    /// RTL_CRITICAL_SECTION_DEBUG records live in guest regions Vitreo maps;
    /// the first holds the process lock list head every record is linked
    /// into. m_debug_mutex is a leaf lock: nothing is acquired under it.
    std::mutex m_debug_mutex;
    /// Latched when no guest region for records could be mapped.
    std::atomic<bool> m_debug_exhausted{false};
    GuestAddr m_debug_region_base{0};
    u32 m_debug_region_offset{0};
    GuestAddr m_debug_list_head{0};
    std::vector<GuestAddr> m_debug_free;

    CsState& get_or_create(GuestAddr cs_addr);
    /// A recycled state, or a new one. Caller holds m_map_lock.
    std::unique_ptr<CsState> take_state_locked();
    /// A section's state leaves the map (delete, re-initialisation): onto the
    /// free list, or retired when it cannot be reused. Caller holds
    /// m_map_lock.
    void retire_state_locked(std::unique_ptr<CsState> state);
    /// Whether a departing state may serve another section: no registered
    /// waiter, and a host mutex no other thread holds. Holds the calling
    /// thread still has (DeleteCriticalSection on a section it owns) are
    /// released here. Caller holds m_map_lock.
    static bool release_for_reuse_locked(CsState& state);
    /// Wake one thread parked in enter_slow on `state`.
    static void wake_one_parked(CsState& state);
    /// The accelerated LeaveCriticalSection on the section's lock word.
    /// Defined in critical_section_leave.hpp. kReleasedHook is false in the
    /// shipping instantiation (leave()); only the test build's
    /// leave_with_released_hook_for_test instantiates it with true.
    template <bool kReleasedHook>
    void leave_accel(GuestAddr cs_addr, std::atomic<u32>& word,
                     ReleasedHook hook);



public:
    /// Host half of the accelerated protocol, reached from emitted guest code
    /// via the __vitreo_cs_* exports when its fast path cannot complete.
    /// `enter_slow` blocks until the caller owns the section; `leave_wake`
    /// finishes a release the fast path could not, because waiters were
    /// registered on the lock word.
    void enter_slow(GuestAddr cs_addr);
    void leave_wake(GuestAddr cs_addr);

private:
    /// The guest's LockCount as an atomic in host address space, or nullptr if
    /// the section is not backed by linear memory.
    [[nodiscard]] std::atomic<u32>* lock_word(GuestAddr cs_addr) const noexcept;
    /// Publish LockSemaphore as the keyed-event sentinel ((HANDLE)-1) on first
    /// contention. LockSemaphore is never a waiter counter.
    void publish_lock_semaphore(GuestAddr cs_addr, CsState& state);
    /// Allocate and link an RTL_CRITICAL_SECTION_DEBUG for cs_addr; 0 when
    /// no guest memory can be had (DebugInfo then stays (PVOID)-1).
    GuestAddr allocate_debug_info(GuestAddr cs_addr);
    /// Unlink a record from the process lock list and recycle it.
    void free_debug_info(GuestAddr record);
    /// A thread is about to wait for cs_addr: give the section its debug
    /// record if Windows would (first wait, no NO_DEBUG_INFO flag) and count
    /// the wait in ContentionCount. Caller holds state.park_mtx.
    void note_contended_wait(GuestAddr cs_addr, CsState& state);
    /// Write a freshly initialised section. `first_off` is the field Windows
    /// stores first (where an unwritable section faults).
    std::optional<CsGuestFault> init_section(GuestAddr cs_addr, u32 spin_word,
                                             bool force_debug_info,
                                             u32 first_off);
    /// Enter/Leave only change LockCount, RecursionCount, and OwningThread.
    void write_lock_state(GuestAddr cs_addr, const CsState& state);
};

/// Create thunks for InitializeCriticalSection, Enter/Leave/TryEnter/Delete
DllExports create_critical_section_thunks(CriticalSectionManager& mgr);

} // namespace vitreo::winapi

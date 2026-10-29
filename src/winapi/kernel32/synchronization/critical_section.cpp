/// @file critical_section.cpp
/// @brief CRITICAL_SECTION with guest memory layout (WIN32-065)

#include "critical_section.hpp"
#include "critical_section_leave.hpp"
#include <vitreo/ab_cycle.hpp>
#include <runtime/diagnostics/hang_dump.hpp>

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/system/sysinfo.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace vitreo::winapi {

// cs_accel_enabled() is declared in critical_section_leave.hpp and defined
// below, next to the accelerated protocol it gates.

namespace {

constexpr GuestAddr kCriticalSectionDebugMin = 0x5E080000;
constexpr usize kCriticalSectionDebugRegionSize = 0x10000;
const bool kCritsecDiag = std::getenv("VITREO_CRITSEC_DIAG") != nullptr;

void trace_cs_write_failure(const char* op, GuestAddr addr, const VoidResult& result) {
    if (!result.is_err()) {
        return;
    }
    VITREO_TRACE("[critsec] %s write failed at 0x%llx: %s\n",
                 op,
                 static_cast<unsigned long long>(addr),
                 error_to_string(result.error()));
}

/// VITREO_CS_CONTENTION: every contended accelerated enter that waited at
/// least 1 ms is charged to its section, together with the owning guest
/// thread and where that owner was executing when the wait began. The ranked
/// summary prints every 5 s. Owner positions are torn samples: hints only.
struct CsContentionProfile {
    struct Entry {
        u64 waits = 0;
        u64 wait_ns = 0;
        std::map<u32, u64> owners;
        std::map<u64, u64> owner_rips;
    };
    std::mutex mu;
    std::map<GuestAddr, Entry> by_cs;
    std::chrono::steady_clock::time_point last_dump =
        std::chrono::steady_clock::now();

    static bool enabled() {
        static const bool on = std::getenv("VITREO_CS_CONTENTION") != nullptr;
        return on;
    }
    static CsContentionProfile& get() {
        static auto* p = new CsContentionProfile();
        return *p;
    }
    void add(GuestAddr cs, u32 owner, u64 owner_rip, u64 ns) {
        std::lock_guard<std::mutex> lk(mu);
        Entry& e = by_cs[cs];
        ++e.waits;
        e.wait_ns += ns;
        ++e.owners[owner];
        ++e.owner_rips[owner_rip];
        const auto now = std::chrono::steady_clock::now();
        if (now - last_dump < std::chrono::seconds(5)) return;
        last_dump = now;
        std::vector<std::pair<GuestAddr, const Entry*>> rows;
        for (const auto& [addr, entry] : by_cs) rows.emplace_back(addr, &entry);
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
            return a.second->wait_ns > b.second->wait_ns;
        });
        std::fprintf(stderr, "[cs-contention] ---- top sections by wait time\n");
        for (usize i = 0; i < rows.size() && i < 6; ++i) {
            const Entry& en = *rows[i].second;
            std::fprintf(stderr,
                         "[cs-contention] cs=0x%llx waits=%llu wait_ms=%llu\n",
                         static_cast<unsigned long long>(rows[i].first),
                         static_cast<unsigned long long>(en.waits),
                         static_cast<unsigned long long>(en.wait_ns / 1000000));
            for (const auto& [tid, n] : en.owners) {
                std::fprintf(stderr, "[cs-contention]    owner=%u n=%llu\n",
                             tid, static_cast<unsigned long long>(n));
            }
            std::vector<std::pair<u64, u64>> rips(en.owner_rips.begin(),
                                                  en.owner_rips.end());
            std::sort(rips.begin(), rips.end(), [](const auto& a, const auto& b) {
                return a.second > b.second;
            });
            for (usize j = 0; j < rips.size() && j < 8; ++j) {
                std::fprintf(stderr,
                             "[cs-contention]    owner_rip=0x%llx n=%llu\n",
                             static_cast<unsigned long long>(rips[j].first),
                             static_cast<unsigned long long>(rips[j].second));
            }
        }
        by_cs.clear();
    }
};

} // namespace

// =============================================================================
// Construction
// =============================================================================

namespace {
/// Set while a manager exists so the hang dumper can reach it. A hang dump is
/// a debugging read of a process that is already stuck; it deliberately does
/// not take m_map_lock, because the thread holding that lock may be exactly
/// the one wedged.
std::atomic<CriticalSectionManager*> g_dump_target{nullptr};
}  // namespace

CriticalSectionManager::CriticalSectionManager(GuestMemory& memory)
    : m_memory(memory) {
    g_dump_target.store(this, std::memory_order_release);
    diagnostics::register_hang_dump_section([](std::FILE* out) {
        auto* self = g_dump_target.load(std::memory_order_acquire);
        if (self == nullptr || out == nullptr) return;
        self->dump_locked_sections(out);
    });
}

/// Print every tracked section's live words.
///
/// This is what separates the two ways threads pile up in enter_slow: if the
/// lock word reads FREE while threads are parked, a wakeup was lost; if it is
/// held by a thread that is itself parked, it is a genuine deadlock cycle.
/// The thread list alone cannot tell those apart.
void CriticalSectionManager::dump_locked_sections(std::FILE* out) {
    const CsLayout& L = layout();
    std::fprintf(out, "[hang-dump] === critical sections ===\n");
    for (const auto& [addr, state] : m_states) {
        if (state == nullptr) continue;
        const auto lc = m_memory.read_u32(addr + L.off_lock_count);
        const auto rc = m_memory.read_u32(addr + L.off_recursion_count);
        const auto ow = m_memory.read_u32(addr + L.off_owning_thread);
        const auto wt = m_memory.read_u32(addr + L.off_lock_semaphore);
        const u32 lock = lc.value_or(0xDEADBEEFu);
        std::fprintf(out,
            "[hang-dump]   cs=0x%08x lock=0x%08x (%s) recursion=%u "
            "owner=%u waiters=%u host_owner=%u\n",
            static_cast<unsigned>(addr), lock,
            lock == cs_layout::UNLOCKED ? "FREE" : "HELD",
            rc.value_or(0), ow.value_or(0), wt.value_or(0),
            state->owning_thread.load(std::memory_order_relaxed));
    }
}

void CriticalSectionManager::note_bitness(bool is_64bit) noexcept {
    // Every Enter/Leave/TryEnter reaches here. A compare-exchange takes the
    // cache line exclusive even when it fails, so an unconditional one
    // bounced this line between every core entering a critical section
    // (1.6% of UE's render thread). Once latched it never changes: read
    // first.
    if (m_bitness_latched.load(std::memory_order_acquire) != 0) return;
    int desired = is_64bit ? 2 : 1;
    int expected = 0;
    // One-shot latch: only the first call sets the bitness. Concurrent calls
    // with the same value are harmless; a mismatch is impossible since
    // process bitness is fixed.
    m_bitness_latched.compare_exchange_strong(expected, desired,
                                              std::memory_order_acq_rel);
}

// =============================================================================
// Private Helpers
// =============================================================================

CriticalSectionManager::CsState&
CriticalSectionManager::get_or_create(GuestAddr cs_addr) {
    std::lock_guard<std::mutex> lock(m_map_lock);
    auto it = m_states.find(cs_addr);
    if (it == m_states.end()) {
        // No debug record here: Windows gives a section one at its first
        // contended wait (note_contended_wait), not when it is first seen.
        auto state = take_state_locked();
        // Publishing an unlocked lock word here is safe ONLY while the guest
        // fast path is off.
        //
        // With it on, an uncontended acquire never enters the host, so the
        // manager's first sight of a section can be inside enter_slow -- while
        // that section is legitimately HELD by a fast-path owner. Writing
        // UNLOCKED there resets a live lock, two threads believe they own it,
        // and the guest deadlocks. There is no way to tell "uninitialized
        // garbage" from "held" by inspecting the word, so the accelerated
        // protocol simply requires InitializeCriticalSection -- which Windows
        // requires too. The legacy path keeps the old nicety.
        if (!cs_accel_enabled()) {
            const CsLayout& L = layout();
            (void)m_memory.write_u32(cs_addr + L.off_lock_count,
                                     cs_layout::UNLOCKED);
            (void)m_memory.write_u32(cs_addr + L.off_recursion_count, 0u);
            if (L.ptr_size == 8) {
                (void)m_memory.write_u64(cs_addr + L.off_owning_thread, 0u);
            } else {
                (void)m_memory.write_u32(cs_addr + L.off_owning_thread, 0u);
            }
        }
        //
        // This creates host bookkeeping, and it is reached from the CONTENDED
        // path: with the guest fast path enabled (guest_accel.cpp) an
        // uncontended acquire never enters the host at all, so the very first
        // time the manager sees a section can be inside enter_slow -- while
        // that section is legitimately HELD by a fast-path owner. Writing
        // UNLOCKED there resets a live lock, two threads then believe they own
        // it, and the guest deadlocks. Publishing belongs to initialize(),
        // which by definition runs when the section is not yet held.
        auto [inserted, _] = m_states.emplace(cs_addr, std::move(state));
        return *inserted->second;
    }
    return *it->second;
}

std::unique_ptr<CriticalSectionManager::CsState>
CriticalSectionManager::take_state_locked() {
    if (!m_free_states.empty()) {
        std::unique_ptr<CsState> state = std::move(m_free_states.back());
        m_free_states.pop_back();
        state->reset_for_reuse();
        return state;
    }
    m_state_objects.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<CsState>();
}

bool CriticalSectionManager::release_for_reuse_locked(CsState& state) {
    // A thread still parked on the deleted section (undefined per Learn)
    // would share a recycled state's waiter count and park condition with an
    // unrelated section: phantom waiter bits in its LockCount, notifies
    // absorbed by the stranger.
    if (state.registered_waiters.load(std::memory_order_seq_cst) != 0u) {
        return false;
    }
    // Legacy path: the host mutex must be unowned before another section can
    // use it. try_lock never blocks; it fails while another thread holds the
    // mutex (that state is retired). It succeeds when nobody holds it, or
    // when this thread does: DeleteCriticalSection on a section the caller
    // still owns, which is undefined but common. Then recursion_count is this
    // thread's hold depth (the accelerated path never writes it), so this
    // thread releases its own holds plus the one just taken.
    if (!state.mtx.try_lock()) return false;
    const u32 held = state.recursion_count;
    if (held != 0 && state.owning_thread.load(std::memory_order_relaxed) !=
                         ThreadManager::get_current_thread_id_static()) {
        // Bookkeeping that does not describe this thread's holds (an
        // unbalanced Leave): never guess how many unlocks would be right.
        state.mtx.unlock();
        return false;
    }
    for (u32 i = 0; i < held; ++i) state.mtx.unlock();
    state.mtx.unlock();
    state.recursion_count = 0;
    return true;
}

void CriticalSectionManager::retire_state_locked(std::unique_ptr<CsState> state) {
    if (state == nullptr) return;
    if (release_for_reuse_locked(*state)) {
        m_free_states.push_back(std::move(state));
    } else {
        // Never freed (type-stable) and never reused: the thread that still
        // holds or waits on it may touch it again.
        m_retired_states.push_back(std::move(state));
    }
}

void CriticalSectionManager::wake_one_parked(CsState& state) {
    // Taking park_mtx before notifying closes the lost-wakeup window: a waiter
    // holds it from before it registers until it is inside wait().
    {
        std::lock_guard<std::mutex> lk(state.park_mtx);
    }
    state.park_cv.notify_one();
}

namespace {

/// Learn, InitializeCriticalSectionAndSpinCount: "On single-processor systems,
/// the spin count is ignored and the critical section spin count is set to 0".
/// The processor count is the one the guest sees (GetSystemInfo's).
[[nodiscard]] bool cs_multiprocessor() noexcept {
    static const bool mp = host_active_processor_count() > 1;
    return mp;
}

} // namespace

u32 CriticalSectionManager::stored_spin_word(u32 count, u32 flags) noexcept {
    using namespace cs_flags;
    count &= SPIN_COUNT_MASK;
    flags &= RTL_CRITICAL_SECTION_ALL_FLAG_BITS;
    // Uniprocessor: Learn's zero count. The flag byte Windows keeps there is
    // unmeasured (Windows is a multiprocessor); dynamic spinning cannot
    // apply, so that flag is not set.
    if (!cs_multiprocessor()) {
        return flags & ~RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN;
    }
    // Measured (handles_waits_probe.expected:151,179,191; cs_init_flags_probe
    // ex_spin*_dynamic): a zero count, or the dynamic flag with any count, is
    // stored as RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN | 2000.
    if (count == 0 || (flags & RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN) != 0) {
        return flags | RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN |
               DEFAULT_DYNAMIC_SPIN_COUNT;
    }
    return flags | count;
}

GuestAddr CriticalSectionManager::allocate_debug_info(GuestAddr cs_addr) {
    const bool is_64bit = layout().ptr_size == 8;
    const CsDebugLayout& D = is_64bit ? cs_debug_layout_64 : cs_debug_layout_32;
    const u32 ptr = is_64bit ? 8u : 4u;
    auto read_ptr = [&](GuestAddr a) -> GuestAddr {
        if (is_64bit) return m_memory.read_u64(a).value_or(0);
        return m_memory.read_u32(a).value_or(0);
    };
    auto write_ptr = [&](GuestAddr a, GuestAddr v) {
        if (is_64bit) (void)m_memory.write_u64(a, v);
        else (void)m_memory.write_u32(a, static_cast<u32>(v));
    };

    std::lock_guard<std::mutex> lock(m_debug_mutex);
    const u32 aligned_size = (D.size + 15u) & ~15u;
    GuestAddr debug = 0;
    if (!m_debug_free.empty()) {
        debug = m_debug_free.back();
        m_debug_free.pop_back();
    } else {
        // Once guest memory for records could not be had, stop asking: later
        // contended waits (under park_mtx) must not retry the gap search.
        // Recycled records above are still handed out.
        if (m_debug_exhausted.load(std::memory_order_acquire)) return 0;
        if (m_debug_region_base == 0 ||
            m_debug_region_offset + aligned_size > kCriticalSectionDebugRegionSize) {
            // A fresh region. Records never move: a full region stays in use
            // and the next one continues the bump allocation.
            const GuestAddr hint = m_debug_region_base != 0
                ? m_debug_region_base + kCriticalSectionDebugRegionSize
                : kCriticalSectionDebugMin;
            const GuestAddr base =
                m_memory.find_free_gap(hint, kCriticalSectionDebugRegionSize);
            if (base == 0 ||
                m_memory.map_region(base, kCriticalSectionDebugRegionSize,
                                    MemoryProtection::ReadWrite).is_err()) {
                // Sections without a record keep DebugInfo (PVOID)-1, which
                // is what Windows leaves when its allocation fails too.
                if (!m_debug_exhausted.exchange(true, std::memory_order_acq_rel)) {
                    std::fprintf(stderr,
                                 "[critsec] unable to allocate guest debug-info "
                                 "region; further sections keep DebugInfo "
                                 "(PVOID)-1\n");
                }
                return 0;
            }
            m_debug_region_base = base;
            m_debug_region_offset = 0;
            if (m_debug_list_head == 0) {
                // The process lock list head (a LIST_ENTRY), empty.
                m_debug_list_head = base;
                write_ptr(base, base);
                write_ptr(base + ptr, base);
                m_debug_region_offset = 16;
            }
        }
        debug = m_debug_region_base + m_debug_region_offset;
        m_debug_region_offset += aligned_size;
    }
    for (u32 i = 0; i < D.size; ++i) {
        (void)m_memory.write_u8(debug + i, 0);
    }
    // Type 0, no entries, no contention, no flags (cs_init_flags_probe
    // forcedebug_init_record); CriticalSection points back at the section.
    write_ptr(debug + D.off_critical_section, cs_addr);
    // InsertTailList(head, &debug->ProcessLocksList): Windows links every
    // record into the process's lock list (list_self_linked=0).
    const GuestAddr entry = debug + D.off_process_locks_list;
    const GuestAddr head = m_debug_list_head;
    const GuestAddr tail = read_ptr(head + ptr);
    write_ptr(entry, head);
    write_ptr(entry + ptr, tail);
    write_ptr(tail, entry);
    write_ptr(head + ptr, entry);
    return debug;
}

void CriticalSectionManager::free_debug_info(GuestAddr record) {
    if (record == 0) return;
    const bool is_64bit = layout().ptr_size == 8;
    const CsDebugLayout& D = is_64bit ? cs_debug_layout_64 : cs_debug_layout_32;
    const u32 ptr = is_64bit ? 8u : 4u;
    auto read_ptr = [&](GuestAddr a) -> GuestAddr {
        if (is_64bit) return m_memory.read_u64(a).value_or(0);
        return m_memory.read_u32(a).value_or(0);
    };
    auto write_ptr = [&](GuestAddr a, GuestAddr v) {
        if (is_64bit) (void)m_memory.write_u64(a, v);
        else (void)m_memory.write_u32(a, static_cast<u32>(v));
    };
    std::lock_guard<std::mutex> lock(m_debug_mutex);
    // RemoveEntryList(&record->ProcessLocksList).
    const GuestAddr entry = record + D.off_process_locks_list;
    const GuestAddr flink = read_ptr(entry);
    const GuestAddr blink = read_ptr(entry + ptr);
    if (flink != 0 && blink != 0) {
        write_ptr(blink, flink);
        write_ptr(flink + ptr, blink);
    }
    m_debug_free.push_back(record);
}

void CriticalSectionManager::note_contended_wait(GuestAddr cs_addr,
                                                 CsState& state) {
    const CsLayout& L = layout();
    const bool is_64bit = L.ptr_size == 8;
    const CsDebugLayout& D = is_64bit ? cs_debug_layout_64 : cs_debug_layout_32;
    if (state.debug_info == 0) {
        // Measured (handles_waits_probe "waiter dbg=00000001",
        // cs_init_flags_probe "-- debug_info"): DebugInfo stays (PVOID)-1
        // until the first thread waits for the section, which then gets a
        // record -- unless it was initialised with NO_DEBUG_INFO. A failed
        // TryEnter waits for nothing and allocates nothing.
        const u64 debug_field = is_64bit
            ? m_memory.read_u64(cs_addr + L.off_debug_info).value_or(0)
            : m_memory.read_u32(cs_addr + L.off_debug_info).value_or(0);
        const u64 sentinel = is_64bit ? ~u64{0} : u64{0xFFFFFFFFu};
        const u32 flags =
            m_memory.read_u32(cs_addr + L.off_spin_count).value_or(0) &
            cs_flags::RTL_CRITICAL_SECTION_ALL_FLAG_BITS;
        if (debug_field == sentinel &&
            (flags & cs_flags::RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO) == 0) {
            const GuestAddr record = allocate_debug_info(cs_addr);
            if (record != 0) {
                state.debug_info = record;
                if (is_64bit) {
                    (void)m_memory.write_u64(cs_addr + L.off_debug_info, record);
                } else {
                    (void)m_memory.write_u32(cs_addr + L.off_debug_info,
                                             static_cast<u32>(record));
                }
            }
        }
    }
    if (state.debug_info != 0) {
        // ContentionCount counts the waits (1 after the first, 2 after the
        // second: cs_init_flags_probe record / record_after_second).
        const GuestAddr at = state.debug_info + D.off_contention_count;
        (void)m_memory.write_u32(at, m_memory.read_u32(at).value_or(0) + 1u);
    }
}

std::optional<CsGuestFault> CriticalSectionManager::init_section(
    GuestAddr cs_addr, u32 spin_word, bool force_debug_info, u32 first_off) {
    const CsLayout& L = layout();
    const bool ptr_is_8 = (L.ptr_size == 8);
    // Every byte of RTL_CRITICAL_SECTION is a field (24 bytes x86, 40 x64,
    // no padding), so writing each field initialises the whole structure.
    struct Field { u32 off; u32 size; u64 value; };
    const u64 no_debug = ptr_is_8 ? ~u64{0} : u64{0xFFFFFFFFu};
    Field fields[] = {
        {L.off_debug_info, L.ptr_size, no_debug},
        {L.off_lock_count, 4, cs_layout::UNLOCKED},
        {L.off_recursion_count, 4, 0},
        {L.off_owning_thread, L.ptr_size, 0},
        {L.off_lock_semaphore, L.ptr_size, 0},
        {L.off_spin_count, L.ptr_size, spin_word},
    };
    // Windows' first store decides where an unwritable section faults
    // (cs_init_flags_probe "-- unwritable_section").
    std::stable_partition(std::begin(fields), std::end(fields),
                          [&](const Field& f) { return f.off == first_off; });
    for (const Field& f : fields) {
        const GuestAddr at = cs_addr + f.off;
        const bool ok = f.size == 8
            ? m_memory.write_u64(at, f.value).is_ok()
            : m_memory.write_u32(at, static_cast<u32>(f.value)).is_ok();
        if (!ok) return CsGuestFault{at, /*write=*/true};
    }

    GuestAddr record = 0;
    if (force_debug_info) {
        record = allocate_debug_info(cs_addr);
        if (record != 0) {
            if (ptr_is_8) {
                (void)m_memory.write_u64(cs_addr + L.off_debug_info, record);
            } else {
                (void)m_memory.write_u32(cs_addr + L.off_debug_info,
                                         static_cast<u32>(record));
            }
        }
    }
    GuestAddr stale_record = 0;
    {
        std::lock_guard<std::mutex> lock(m_map_lock);
        auto& slot = m_states[cs_addr];
        if (slot) {
            // Re-initialising a live section is undefined (Learn). Its state
            // leaves exactly as at DeleteCriticalSection (recycled, or
            // retired while another thread holds or waits on it), and the
            // previous record is recycled rather than leaked.
            stale_record = slot->debug_info;
            slot->debug_info = 0;
            retire_state_locked(std::move(slot));
        }
        slot = take_state_locked();
        slot->debug_info = record;
    }
    free_debug_info(stale_record);
    return std::nullopt;
}

void CriticalSectionManager::write_lock_state(GuestAddr cs_addr,
                                              const CsState& state) {
    const CsLayout& L = layout();
    const u32 lock_count = (state.recursion_count == 0)
                               ? cs_layout::UNLOCKED
                               : cs_layout::LOCKED_NO_WAITERS;
    trace_cs_write_failure(
        "write_lock_state.LockCount", cs_addr + L.off_lock_count,
        m_memory.write_u32(cs_addr + L.off_lock_count, lock_count));
    trace_cs_write_failure(
        "write_lock_state.RecursionCount", cs_addr + L.off_recursion_count,
        m_memory.write_u32(cs_addr + L.off_recursion_count,
                           state.recursion_count));
    if (L.ptr_size == 8) {
        trace_cs_write_failure(
            "write_lock_state.OwningThread", cs_addr + L.off_owning_thread,
            m_memory.write_u64(cs_addr + L.off_owning_thread,
                               static_cast<u64>(state.owning_thread.load(
                                   std::memory_order_relaxed))));
    } else {
        trace_cs_write_failure(
            "write_lock_state.OwningThread", cs_addr + L.off_owning_thread,
            m_memory.write_u32(cs_addr + L.off_owning_thread,
                               state.owning_thread.load(std::memory_order_relaxed)));
    }
}

// =============================================================================
// Public API
// =============================================================================

std::optional<CsGuestFault> CriticalSectionManager::initialize(GuestAddr cs_addr) {
    VITREO_TRACE("[critsec] initialize cs=0x%llx\n",
                 static_cast<unsigned long long>(cs_addr));
    // InitializeCriticalSection stores what AndSpinCount(cs, 0) stores
    // (handles_waits_probe.expected:151 and :191 agree) and faults at the
    // same first store (cs_init_flags_probe init_bad / asc_bad).
    return initialize_and_spin_count(cs_addr, 0);
}

std::optional<CsGuestFault> CriticalSectionManager::initialize_and_spin_count(
    GuestAddr cs_addr, u32 spin_count) {
    VITREO_TRACE("[critsec] initialize_and_spin_count cs=0x%llx spin=0x%x\n",
                 static_cast<unsigned long long>(cs_addr),
                 static_cast<unsigned>(spin_count));
    // The high byte of dwSpinCount carries no flags here: 0x80000FA0,
    // 0x01000FA0 and 0x02000FA0 all store 0xFA0, and 0xFF000000 stores the
    // dynamic default (handles_waits_probe.expected:196-199,
    // cs_init_flags_probe "-- and_spin_count").
    const u32 word = stored_spin_word(spin_count & cs_flags::SPIN_COUNT_MASK, 0);
    return init_section(cs_addr, word, /*force_debug_info=*/false,
                        layout().off_lock_count);
}

CsSetSpinResult CriticalSectionManager::set_spin_count(GuestAddr cs_addr,
                                                       u32 spin_count) {
    const CsLayout& L = layout();
    const GuestAddr at = cs_addr + L.off_spin_count;
    CsSetSpinResult result;
    // Windows reads SpinCount first (cs_init_flags_probe set_bad: a READ
    // fault at SpinCount).
    u32 old_word = 0;
    bool readable = false;
    if (L.ptr_size == 8) {
        const auto r = m_memory.read_u64(at);
        readable = r.is_ok();
        if (readable) old_word = static_cast<u32>(r.value());
    } else {
        const auto r = m_memory.read_u32(at);
        readable = r.is_ok();
        if (readable) old_word = r.value();
    }
    if (!readable) {
        result.fault = CsGuestFault{at, /*write=*/false};
        return result;
    }
    // The flag byte is kept and the new count replaces only the low 24
    // bits: a default section set to 100 reads 0x02000064, and 0x81000010
    // stores 0x02000010 (handles_waits_probe.expected:192-195). A zero count
    // stays zero (cs_init_flags_probe default_set_0: 0x02000000).
    const u32 count = cs_multiprocessor() ? (spin_count & cs_flags::SPIN_COUNT_MASK) : 0u;
    const u32 new_word = (old_word & cs_flags::RTL_CRITICAL_SECTION_ALL_FLAG_BITS) | count;
    const bool wrote = L.ptr_size == 8
        ? m_memory.write_u64(at, new_word).is_ok()
        : m_memory.write_u32(at, new_word).is_ok();
    if (!wrote) {
        result.fault = CsGuestFault{at, /*write=*/true};
        return result;
    }
    // "returns the previous spin count": without its flag byte
    // (set_spin_on_default ret=000007d0).
    result.previous = old_word & cs_flags::SPIN_COUNT_MASK;
    return result;
}

CsInitExResult CriticalSectionManager::initialize_ex(GuestAddr cs_addr,
                                                     u32 spin_count,
                                                     u32 flags) {
    VITREO_TRACE("[critsec] initialize_ex cs=0x%llx spin=0x%x flags=0x%x\n",
                 static_cast<unsigned long long>(cs_addr),
                 static_cast<unsigned>(spin_count),
                 static_cast<unsigned>(flags));
    using namespace cs_flags;
    CsInitExResult result;
    // Validated before the section is touched (cs_init_flags_probe
    // ex_bad_reserved returns FALSE without faulting): reserved flag bits,
    // flag bits inside dwSpinCount, and NO_DEBUG_INFO with FORCE_DEBUG_INFO
    // are ERROR_INVALID_PARAMETER (handles_waits_probe.expected:163-165,
    // cs_init_flags_probe ex_spin4000_flag_{40,80}000000,
    // ex_spin4000_nodebug_forcedebug, ex_spin_ffffffff). Bits below the
    // flag byte are ignored (ex_spin4000_flag_00ffffff, init_ex_low_bit_flag).
    const bool both_debug_flags =
        (flags & RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO) != 0 &&
        (flags & RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO) != 0;
    if ((flags & RTL_CRITICAL_SECTION_FLAG_RESERVED) != 0 ||
        (spin_count & RTL_CRITICAL_SECTION_ALL_FLAG_BITS) != 0 ||
        both_debug_flags) {
        result.error = error_codes::ERROR_INVALID_PARAMETER;
        return result;
    }
    result.ok = true;
    // RTL_CRITICAL_SECTION_FLAG_STATIC_INIT: TRUE, the structure untouched
    // (handles_waits_probe.expected:184-185, cs_init_flags_probe
    // ex_bad_static: no fault on an unwritable section).
    if ((flags & RTL_CRITICAL_SECTION_FLAG_STATIC_INIT) != 0) return result;
    // NO_DEBUG_INFO, DYNAMIC_SPIN and RESOURCE_TYPE are kept in the flag
    // byte; FORCE_DEBUG_INFO is not (it gives the section a record now).
    const u32 kept = flags & (RTL_CRITICAL_SECTION_FLAG_NO_DEBUG_INFO |
                              RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN |
                              RTL_CRITICAL_SECTION_FLAG_RESOURCE_TYPE);
    const u32 word = stored_spin_word(spin_count, kept);
    // x64 Ex stores RecursionCount first, x86 LockCount (ex_bad faults at
    // 0x10C on x64, 0x104 on x86).
    const CsLayout& L = layout();
    const u32 first = L.ptr_size == 8 ? L.off_recursion_count : L.off_lock_count;
    result.fault = init_section(
        cs_addr, word, (flags & RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO) != 0,
        first);
    if (result.fault) result.ok = false;
    return result;
}

// The accelerated critical-section protocol.
//
// LockCount uses the Server 2003 SP1+ encoding so guest-visible values match
// Windows: -1 free, -2 held with no waiters, -2-4*n with n waiters (bit0 clear
// means locked). The guest fast path in guest_accel.cpp performs the
// uncontended -1↔-2 transitions with `lock cmpxchg`, and these host halves
// contend for the *same* word -- a host-side mutex used as a second authority
// would not exclude the guest.
//
// Waiter registration lives in CsState::registered_waiters (host-only).
// LockSemaphore is NULL until first contention, then the keyed-event sentinel
// ((HANDLE)-1); it is never a waiter counter.
//
// Every LockCount mutation except the guest's own uncontended cmpxchg happens
// under park_mtx, so waiter registration and wakeup cannot race. Barging is
// permitted: a guest fast path may take a section a woken waiter was about to
// claim, and the waiter simply waits again. Windows critical sections are not
// FIFO either.
// On by default. VITREO_NO_CS_ACCEL=1 restores the host-mutex path.
bool cs_accel_enabled() noexcept {
    static const bool on = std::getenv("VITREO_NO_CS_ACCEL") == nullptr;
    return on;
}

std::atomic<u32>* CriticalSectionManager::lock_word(
    GuestAddr cs_addr) const noexcept {
    // linear_host() is only meaningful when a linear model is actually active:
    // otherwise it hands back `nullptr + addr`, a wild pointer rather than the
    // null this used to test for. Ask whether the model exists first.
    if (!m_memory.is_linear()) return nullptr;
    const GuestAddr word_addr = cs_addr + layout().off_lock_count;
    // A CRITICAL_SECTION inside a module loaded above the reservation is a
    // legitimate guest address that linear_host() cannot back; the managed
    // CsState path is the correct (and pre-existing) fallback.
    if (!m_memory.linear_backs(word_addr, sizeof(u32))) return nullptr;
    // linear_host() is pure arithmetic: it never rejects an address, so the
    // guest pointer must be validated here or the host dereferences whatever
    // the guest handed us. Two independent requirements:
    //
    //  * Natural alignment. A lock-free u32 CAS is only defined on a 4-byte
    //    aligned word, and on ARM64 an unaligned exclusive/atomic access
    //    faults (SIGBUS) rather than splitting. A CRITICAL_SECTION at an
    //    unaligned address is a guest bug, not a reason to kill the runtime.
    //  * Actually accessible guest memory. Inside a linear reservation an
    //    unmapped page is present-but-inaccessible, so a bad pointer is a
    //    host SIGSEGV instead of a translation failure. read_u32 is the
    //    TLB-backed accessibility question and answers it without taking the
    //    region lock on the hot path.
    //
    // Either failure falls back to the managed CsState path, which is the
    // general implementation this accel path is an optimization of.
    if ((word_addr & 3u) != 0) return nullptr;
    if (!m_memory.read_u32(word_addr).is_ok()) return nullptr;
    u8* host = m_memory.linear_host(word_addr);
    if (host == nullptr) return nullptr;
    return reinterpret_cast<std::atomic<u32>*>(host);
}

void CriticalSectionManager::publish_lock_semaphore(GuestAddr cs_addr,
                                                    CsState& state) {
    if (state.lock_semaphore_published) return;
    state.lock_semaphore_published = true;
    // Vista+ keyed-event sentinel: LockSemaphore reads as (HANDLE)-1 once the
    // section has been contended. Matches Windows cs_lockcount_probe.
    const CsLayout& L = layout();
    if (L.ptr_size == 8) {
        (void)m_memory.write_u64(cs_addr + L.off_lock_semaphore, ~u64{0});
    } else {
        (void)m_memory.write_u32(cs_addr + L.off_lock_semaphore, ~u32{0});
    }
}

void CriticalSectionManager::begin_shutdown() {
    m_shutdown.store(true, std::memory_order_release);
    // Wake every enter_slow waiter. Learn: ExitProcess must not leave threads
    // parked forever inside EnterCriticalSection; a contended enter during
    // process exit terminates instead of blocking.
    std::lock_guard<std::mutex> lock(m_map_lock);
    for (auto& [addr, state] : m_states) {
        (void)addr;
        {
            std::lock_guard<std::mutex> lk(state->park_mtx);
        }
        state->park_cv.notify_all();
        // Legacy path: unlock is not interruptible; notify alone is enough for
        // the accel path. Contended legacy enterers poll m_shutdown below.
    }
}

void CriticalSectionManager::enter_slow(GuestAddr cs_addr) {
    CsState& state = get_or_create(cs_addr);
    std::atomic<u32>* word = lock_word(cs_addr);
    if (word == nullptr) {   // no linear backing: legacy
        if (!state.mtx.try_lock()) {
            {
                std::lock_guard<std::mutex> lk(state.park_mtx);
                note_contended_wait(cs_addr, state);
            }
            // Contended legacy enter during process exit must not park forever.
            while (!state.mtx.try_lock_for(std::chrono::milliseconds(10))) {
                if (m_shutdown.load(std::memory_order_acquire)) {
                    return;
                }
            }
        }
        state.recursion_count++;
        state.owning_thread.store(ThreadManager::get_current_thread_id_static(),
                                  std::memory_order_relaxed);
        write_lock_state(cs_addr, state);
        return;
    }
    const u32 tid = ThreadManager::get_current_thread_id_static();
    const CsLayout& layout_now = layout();

    // Learn (InitializeCriticalSectionAndSpinCount): on a multiprocessor,
    // "the calling thread spins dwSpinCount times before performing a wait
    // operation on a semaphore that is associated with the critical section.
    // If the critical section becomes free during the spin operation, the
    // calling thread avoids the wait operation." The count is the section's
    // own SpinCount field, which InitializeCriticalSectionAndSpinCount,
    // InitializeCriticalSectionEx and SetCriticalSectionSpinCount write; its
    // high byte holds RTL_CRITICAL_SECTION_FLAG_* bits, not iterations. On a
    // single processor the count is ignored.
    //
    // RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN: Windows adapts the count of a
    // dynamic-spin section to how often its spins succeed. That adaptation is
    // internal, undocumented and timing-dependent (the probes print no
    // SpinCount after contention for that reason), so it is NOT modelled:
    // the stored count (2000 for a default section) is spun as given.
    // (VITREO_AB_CYCLE_S: the off arm parks at once, for an in-process A/B.)
    const u32 spins =
        cs_multiprocessor() && !diagnostics::ab_cycle_off("cs-spin")
            ? static_cast<u32>(
                  m_memory.read_u32(cs_addr + layout_now.off_spin_count)
                      .value_or(0)) &
                  cs_flags::SPIN_COUNT_MASK
            : 0u;
    for (u32 i = 0; i < spins; ++i) {
        if (word->load(std::memory_order_relaxed) == cs_layout::UNLOCKED) {
            u32 expected = cs_layout::UNLOCKED;
            if (word->compare_exchange_weak(expected,
                                            cs_layout::LOCKED_NO_WAITERS,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed)) {
                // As below: peers already parked keep their waiter encoding.
                const u32 others =
                    state.registered_waiters.load(std::memory_order_seq_cst);
                if (others != 0u) {
                    std::lock_guard<std::mutex> lk(state.park_mtx);
                    word->store(cs_layout::LOCKED_NO_WAITERS -
                                    cs_layout::WAITER_INC *
                                        state.registered_waiters.load(
                                            std::memory_order_seq_cst),
                                std::memory_order_release);
                }
                // Host owner for the hang dump only; recursion lives in the
                // guest's RecursionCount on this path.
                state.owning_thread.store(tid, std::memory_order_relaxed);
                (void)m_memory.write_u32(cs_addr + layout_now.off_recursion_count,
                                         1u);
                if (layout_now.ptr_size == 8) {
                    (void)m_memory.write_u64(
                        cs_addr + layout_now.off_owning_thread, tid);
                } else {
                    (void)m_memory.write_u32(
                        cs_addr + layout_now.off_owning_thread, tid);
                }
                return;
            }
        }
#if defined(__aarch64__)
        __builtin_arm_yield();
#endif
    }

    // Register as a waiter BEFORE the final acquire attempt, and hold park_mtx
    // across check-and-wait. A releaser that observes registered_waiters==0
    // therefore knows this thread has not yet committed to sleeping.
    std::unique_lock<std::mutex> lk(state.park_mtx);
    state.registered_waiters.fetch_add(1u, std::memory_order_seq_cst);
    publish_lock_semaphore(cs_addr, state);

    // Publish SP1 waiter encoding into LockCount only while the section is
    // held (bit0 clear). Never adjust an unlocked (-1) word: that would turn
    // free into a false "locked with waiters" and hang the section.
    auto publish_waiter_lockcount = [&]() {
        const u32 n =
            state.registered_waiters.load(std::memory_order_seq_cst);
        for (;;) {
            u32 cur = word->load(std::memory_order_relaxed);
            if (cur == cs_layout::UNLOCKED || (cur & 1u) != 0u) {
                return;
            }
            const u32 want = cs_layout::LOCKED_NO_WAITERS -
                             cs_layout::WAITER_INC * n;
            if (cur == want) {
                return;
            }
            if (word->compare_exchange_weak(cur, want,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                return;
            }
        }
    };

    m_contended_waits.fetch_add(1, std::memory_order_relaxed);
    bool noted_wait = false;
    const bool profile = CsContentionProfile::enabled();
    const auto wait_start = profile ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
    u32 profile_owner = 0;
    u64 profile_owner_rip = 0;
    if (profile) {
        profile_owner = m_memory.read_u32(cs_addr + layout().off_owning_thread)
                            .value_or(0);
        for (const auto& sample :
             ThreadSuspendRegistry::instance().sample_threads()) {
            if (sample.tid == profile_owner) {
                profile_owner_rip = sample.rip;
                break;
            }
        }
    }
    for (;;) {
        if (m_shutdown.load(std::memory_order_acquire)) {
            // Process exiting: do not park. Unregister and return without
            // ownership — the guest run loop observes has_exited and stops.
            state.registered_waiters.fetch_sub(1u, std::memory_order_seq_cst);
            lk.unlock();
            return;
        }
        u32 expected = cs_layout::UNLOCKED;
        if (word->compare_exchange_strong(expected, cs_layout::LOCKED_NO_WAITERS,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed)) {
            break;
        }
        publish_waiter_lockcount();
        // Windows creates the debug record and counts ContentionCount only
        // when a thread actually waits: once, just before the first park
        // (a section freed before then is taken without waiting).
        if (!noted_wait) {
            noted_wait = true;
            note_contended_wait(cs_addr, state);
        }
        // The predicate must include "section released": wait_for re-parks
        // after leave_wake's notify whenever the predicate is still false, so
        // a shutdown-only predicate turned every contended enter into a full
        // timeout.
        state.park_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
            return m_shutdown.load(std::memory_order_acquire) ||
                   word->load(std::memory_order_acquire) ==
                       cs_layout::UNLOCKED;
        });
    }
    state.registered_waiters.fetch_sub(1u, std::memory_order_seq_cst);
    if (profile) {
        const u64 ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - wait_start).count());
        if (ns >= 1000000) {
            CsContentionProfile::get().add(cs_addr, profile_owner,
                                           profile_owner_rip, ns);
        }
    }
    // CAS left LockCount at -2; re-apply waiter encoding for peers still parked.
    const u32 others = state.registered_waiters.load(std::memory_order_seq_cst);
    if (others != 0u) {
        word->store(cs_layout::LOCKED_NO_WAITERS -
                        cs_layout::WAITER_INC * others,
                    std::memory_order_release);
    }
    lk.unlock();

    // Host owner for the hang dump only (see the spin acquire above).
    state.owning_thread.store(tid, std::memory_order_relaxed);
    const CsLayout& L = layout();
    (void)m_memory.write_u32(cs_addr + L.off_recursion_count, 1u);
    if (L.ptr_size == 8) {
        (void)m_memory.write_u64(cs_addr + L.off_owning_thread, tid);
    } else {
        (void)m_memory.write_u32(cs_addr + L.off_owning_thread, tid);
    }
}

void CriticalSectionManager::leave_wake(GuestAddr cs_addr) {
    // Do not get_or_create and do not touch guest memory: the releaser already
    // unlocked, and a peer may have DeleteCriticalSection'd / freed the CS.
    CsState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_map_lock);
        auto it = m_states.find(cs_addr);
        if (it == m_states.end()) return;
        state = it->second.get();
    }
    // Hang-dump bookkeeping only (host_owner=): the guest fast path released
    // without telling the host, so this thread -- the releaser -- retires the
    // host owner enter_slow installed for it. Only its own id is cleared, and
    // atomically: once released, a new owner may already have written its
    // id, which must stay. States are type-stable, so even a section a peer
    // has deleted meanwhile is valid memory here. (Host recursion_count is
    // the legacy path's and is not touched.)
    if (u32 owner = state->owning_thread.load(std::memory_order_relaxed);
        owner != 0 && owner == ThreadManager::get_current_thread_id_static()) {
        (void)state->owning_thread.compare_exchange_strong(
            owner, 0u, std::memory_order_relaxed, std::memory_order_relaxed);
    }
    if (state->registered_waiters.load(std::memory_order_seq_cst) == 0u) {
        return;
    }
    wake_one_parked(*state);
}

void CriticalSectionManager::enter(GuestAddr cs_addr) {
    if (cs_accel_enabled()) {
        if (std::atomic<u32>* word = lock_word(cs_addr)) {
            const CsLayout& L = layout();
            const u32 self = ThreadManager::get_current_thread_id_static();
            // Recursion is decided by OwningThread, which a releaser clears
            // *before* unlocking; a stale read can therefore never match a
            // thread that does not currently hold the section.
            const u32 owner =
                m_memory.read_u32(cs_addr + L.off_owning_thread).value_or(0);
            if (owner != 0 && owner == self) {
                const u32 rc =
                    m_memory.read_u32(cs_addr + L.off_recursion_count)
                        .value_or(0);
                (void)m_memory.write_u32(cs_addr + L.off_recursion_count, rc + 1);
                return;
            }
            u32 expected = cs_layout::UNLOCKED;
            if (word->compare_exchange_strong(expected, cs_layout::LOCKED_NO_WAITERS,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
                // The guest CRITICAL_SECTION is the authority on the
                // accelerated path (owning_thread()/recursion_count() read
                // it), so no CsState lookup under m_map_lock is needed here.
                (void)m_memory.write_u32(cs_addr + L.off_recursion_count, 1u);
                if (L.ptr_size == 8) {
                    (void)m_memory.write_u64(cs_addr + L.off_owning_thread, self);
                } else {
                    (void)m_memory.write_u32(cs_addr + L.off_owning_thread, self);
                }
                return;
            }
            enter_slow(cs_addr);
            return;
        }
    }
    CsState& state = get_or_create(cs_addr);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    // A guest thread parked in EnterCriticalSection is otherwise invisible:
    // it makes no further calls, so a lock cycle looks like an idle process.
    // VITREO_CRITSEC_DIAG names the section, the waiter and the current owner
    // once a wait passes the threshold, then blocks normally.
    // Process-exit escape: never park forever once begin_shutdown latched.
    for (bool first = true;; first = false) {
        if (first ? state.mtx.try_lock()
                  : state.mtx.try_lock_for(std::chrono::milliseconds(
                        kCritsecDiag ? 2000 : 10))) {
            break;
        }
        if (first) {
            std::lock_guard<std::mutex> lk(state.park_mtx);
            note_contended_wait(cs_addr, state);
            continue;
        }
        if (m_shutdown.load(std::memory_order_acquire)) {
            return;
        }
        if (kCritsecDiag) {
            std::fprintf(stderr,
                         "[critsec] enter blocked >2s cs=0x%llx self=%u "
                         "owner=%u recursion=%u\n",
                         static_cast<unsigned long long>(cs_addr), tid,
                         state.owning_thread.load(std::memory_order_relaxed),
                         state.recursion_count);
            std::fflush(stderr);
        }
    }
    state.recursion_count++;
    state.owning_thread.store(tid, std::memory_order_relaxed);
    write_lock_state(cs_addr, state);
}

bool CriticalSectionManager::try_enter(GuestAddr cs_addr) {
    if (cs_accel_enabled()) {
        if (std::atomic<u32>* word = lock_word(cs_addr)) {
            const CsLayout& L = layout();
            const u32 self = ThreadManager::get_current_thread_id_static();
            const u32 owner =
                m_memory.read_u32(cs_addr + L.off_owning_thread).value_or(0);
            if (owner != 0 && owner == self) {
                const u32 rc =
                    m_memory.read_u32(cs_addr + L.off_recursion_count).value_or(0);
                (void)m_memory.write_u32(cs_addr + L.off_recursion_count, rc + 1);
                return true;
            }
            u32 expected = cs_layout::UNLOCKED;
            if (!word->compare_exchange_strong(expected, cs_layout::LOCKED_NO_WAITERS,
                                               std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
                return false;
            }
            (void)m_memory.write_u32(cs_addr + L.off_recursion_count, 1u);
            if (L.ptr_size == 8) {
                (void)m_memory.write_u64(cs_addr + L.off_owning_thread, self);
            } else {
                (void)m_memory.write_u32(cs_addr + L.off_owning_thread, self);
            }
            return true;
        }
    }
    CsState& state = get_or_create(cs_addr);
    const u32 tid = ThreadManager::get_current_thread_id_static();
    if (!state.mtx.try_lock()) {
        return false;
    }
    state.recursion_count++;
    state.owning_thread.store(tid, std::memory_order_relaxed);
    write_lock_state(cs_addr, state);
    return true;
}

void CriticalSectionManager::leave(GuestAddr cs_addr) {
    if (cs_accel_enabled()) {
        if (std::atomic<u32>* word = lock_word(cs_addr)) {
            // critical_section_leave.hpp; the shipping instantiation has no
            // test hook.
            leave_accel</*kReleasedHook=*/false>(cs_addr, *word, nullptr);
            return;
        }
    }
    CsState& state = get_or_create(cs_addr);
    state.recursion_count--;
    if (state.recursion_count == 0) {
        state.owning_thread.store(0, std::memory_order_relaxed);
    }
    write_lock_state(cs_addr, state);

    // std::recursive_mutex tracks a host recursion count of its own. Every
    // successful lock()/try_lock() must therefore have a matching unlock(),
    // including intermediate recursive leaves. Deferring unlock until the
    // guest recursion count reached zero left one host lock held forever after
    // a recursive enter/leave pair, while the guest layout misleadingly
    // reported the critical section as unowned.
    state.mtx.unlock();
}

void CriticalSectionManager::delete_cs(GuestAddr cs_addr) {
    // Zero-fill the CS struct in guest memory (size depends on bitness)
    const CsLayout& L = layout();
    for (u32 i = 0; i < L.size; ++i) {
        (void)m_memory.write_u8(cs_addr + i, 0);
    }

    // Remove host-side state; the section's debug record leaves the process
    // lock list and is recycled. The state object is recycled or retired,
    // never freed (type-stable, see m_free_states): a Leave still past its
    // releasing store may touch it.
    GuestAddr record = 0;
    {
        std::lock_guard<std::mutex> lock(m_map_lock);
        auto it = m_states.find(cs_addr);
        if (it != m_states.end()) {
            record = it->second->debug_info;
            it->second->debug_info = 0;
            retire_state_locked(std::move(it->second));
            m_states.erase(it);
        }
    }
    free_debug_info(record);
}

u32 CriticalSectionManager::owning_thread(GuestAddr cs_addr) {
    // The accelerated protocol's authority is the guest CRITICAL_SECTION
    // layout.  Uncontended guest and host fast paths deliberately do not take
    // m_map_lock or update CsState, so consulting that bookkeeping here can
    // report an unlocked section while TryEnterCriticalSection owns it.
    if (cs_accel_enabled() && m_memory.is_linear()) {
        return m_memory.read_u32(cs_addr + layout().off_owning_thread)
            .value_or(0u);
    }
    std::lock_guard<std::mutex> lock(m_map_lock);
    auto it = m_states.find(cs_addr);
    return it == m_states.end()
               ? 0u
               : it->second->owning_thread.load(std::memory_order_relaxed);
}

u32 CriticalSectionManager::recursion_count(GuestAddr cs_addr) {
    if (cs_accel_enabled() && m_memory.is_linear()) {
        return m_memory.read_u32(cs_addr + layout().off_recursion_count)
            .value_or(0u);
    }
    std::lock_guard<std::mutex> lock(m_map_lock);
    auto it = m_states.find(cs_addr);
    return it == m_states.end() ? 0u : it->second->recursion_count;
}

// =============================================================================
// Thunk Registration
// =============================================================================

// Helper to read a guest pointer (u32 on x86 guest stack) as GuestAddr
static GuestAddr read_guest_ptr(Marshaler& m, usize arg_index) {
    return m.arg_ptr(arg_index);
}

// An unwritable (or, for Set, unreadable) section: Windows' own store or
// load faults, so the guest sees STATUS_ACCESS_VIOLATION with the access and
// the address (cs_init_flags_probe "-- unwritable_section"). With no CPU to
// raise on, the call fails as a BOOL/DWORD API would: 0, ERROR_NOACCESS.
static void raise_cs_fault(Marshaler& m, const CsGuestFault& fault) {
    if (!m.raise_access_violation(fault.write, fault.address)) {
        set_last_error(error_codes::ERROR_NOACCESS);
        m.set_return(0);
    }
}

DllExports create_critical_section_thunks(CriticalSectionManager& mgr) {
    DllExports exports;

    // VOID InitializeCriticalSection(LPCRITICAL_SECTION)
    exports.entries.push_back({"InitializeCriticalSection", 0, [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        if (auto fault = mgr.initialize(read_guest_ptr(m, 0))) {
            raise_cs_fault(m, *fault);
        }
    }, 4});

    // BOOL InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION, DWORD):
    // "always succeeds and returns a nonzero value" (Vista+); the last error
    // is left alone.
    exports.entries.push_back({"InitializeCriticalSectionAndSpinCount", 0,
                               [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        const GuestAddr cs_addr = read_guest_ptr(m, 0);
        const u32 spin_count = static_cast<u32>(m.arg(1));
        if (auto fault = mgr.initialize_and_spin_count(cs_addr, spin_count)) {
            raise_cs_fault(m, *fault);
            return;
        }
        m.set_return(1);
    }, 8});

    // DWORD SetCriticalSectionSpinCount(LPCRITICAL_SECTION, DWORD)
    // Returns the PREVIOUS spin count, not a BOOL.
    exports.entries.push_back({"SetCriticalSectionSpinCount", 0,
                               [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        const GuestAddr cs_addr = read_guest_ptr(m, 0);
        const u32 spin_count = static_cast<u32>(m.arg(1));
        const CsSetSpinResult r = mgr.set_spin_count(cs_addr, spin_count);
        if (r.fault) {
            raise_cs_fault(m, *r.fault);
            return;
        }
        m.set_return(r.previous);
    }, 8});

    // BOOL InitializeCriticalSectionEx(LPCRITICAL_SECTION, DWORD, DWORD):
    // FALSE + GetLastError on failure; success leaves the last error alone.
    exports.entries.push_back({"InitializeCriticalSectionEx", 0,
                               [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        const GuestAddr cs_addr = read_guest_ptr(m, 0);
        const u32 spin_count = static_cast<u32>(m.arg(1));
        const u32 flags      = static_cast<u32>(m.arg(2));
        const CsInitExResult r = mgr.initialize_ex(cs_addr, spin_count, flags);
        if (r.fault) {
            raise_cs_fault(m, *r.fault);
            return;
        }
        if (!r.ok) {
            set_last_error(r.error);
            m.set_return(0);
            return;
        }
        m.set_return(1);
    }, 12});

    exports.entries.push_back({"EnterCriticalSection", 0, [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        GuestAddr cs_addr = read_guest_ptr(m, 0);
        if (cs_addr) mgr.enter(cs_addr);
    }, 4});

    exports.entries.push_back({"TryEnterCriticalSection", 0, [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        GuestAddr cs_addr = read_guest_ptr(m, 0);
        bool ok = cs_addr ? mgr.try_enter(cs_addr) : false;
        m.set_return(ok ? 1u : 0u);
    }, 4});

    exports.entries.push_back({"LeaveCriticalSection", 0, [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        GuestAddr cs_addr = read_guest_ptr(m, 0);
        if (cs_addr) mgr.leave(cs_addr);
    }, 4});

    // Slow-path entry points for the emitted guest fast path (guest_accel.cpp).
    // Not Windows exports: they exist so guest-side code can tail-jump into the
    // blocking half without a name collision with anything the guest imports.
    exports.entries.push_back({"__vitreo_cs_enter_slow", 0, [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        GuestAddr cs_addr = read_guest_ptr(m, 0);
        if (cs_addr) mgr.enter_slow(cs_addr);
    }, 4});

    exports.entries.push_back({"__vitreo_cs_leave_wake", 0, [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        GuestAddr cs_addr = read_guest_ptr(m, 0);
        if (cs_addr) mgr.leave_wake(cs_addr);
    }, 4});

    exports.entries.push_back({"DeleteCriticalSection", 0, [&mgr](Marshaler& m) {
        mgr.note_bitness(m.cpu_is_64bit());
        GuestAddr cs_addr = read_guest_ptr(m, 0);
        if (cs_addr) mgr.delete_cs(cs_addr);
    }, 4});

    return exports;
}

} // namespace vitreo::winapi

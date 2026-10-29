#pragma once

/// @file critical_section_leave.hpp
/// @brief The accelerated LeaveCriticalSection, as one template for the
///        shipping Leave and the test build's hooked Leave.
///
/// Include ONLY from critical_section.cpp, which instantiates
/// leave_accel<false> for CriticalSectionManager::leave(), and from the
/// test-only seam tests/winapi/critical_section_test_seam.cpp, which
/// instantiates leave_accel<true>. The release binary therefore contains no
/// hook code: not a load, not a branch.

#include "critical_section.hpp"

namespace vitreo::winapi {

// Defined in critical_section.cpp, next to the accelerated protocol it gates.
[[nodiscard]] bool cs_accel_enabled() noexcept;

template <bool kReleasedHook>
inline __attribute__((always_inline)) void CriticalSectionManager::leave_accel(
    GuestAddr cs_addr, std::atomic<u32>& word, ReleasedHook hook) {
    const CsLayout& L = layout();
    const u32 rc = m_memory.read_u32(cs_addr + L.off_recursion_count).value_or(0);
    if (rc > 1) {
        (void)m_memory.write_u32(cs_addr + L.off_recursion_count, rc - 1);
        return;
    }
    (void)m_memory.write_u32(cs_addr + L.off_recursion_count, 0u);
    if (L.ptr_size == 8) {
        (void)m_memory.write_u64(cs_addr + L.off_owning_thread, 0u);
    } else {
        (void)m_memory.write_u32(cs_addr + L.off_owning_thread, 0u);
    }
    // Learn LeaveCriticalSection: do not touch the CRITICAL_SECTION after
    // ownership is released. The moment the releasing store lands, a peer may
    // acquire the section, leave it and DeleteCriticalSection it. So the host
    // owner field is cleared while this thread still owns the section, and
    // after the store only `st`'s waiter count and park condition are used --
    // never the map, never guest memory. `st` cannot dangle: states are
    // type-stable (m_free_states); if it was deleted and recycled meanwhile,
    // the worst case is one spurious notify that enter_slow's predicate
    // absorbs.
    //
    // seq_cst store, then seq_cst load of registered_waiters: the Dekker pair
    // with enter_slow's registration (fetch_add, then CAS) is formally
    // ordered. On AArch64 this is the same STLR the release store was.
    CsState& st = get_or_create(cs_addr);
    st.owning_thread.store(0, std::memory_order_relaxed);
    word.store(cs_layout::UNLOCKED, std::memory_order_seq_cst);
    if constexpr (kReleasedHook) {
        if (hook != nullptr) hook(cs_addr);
    } else {
        (void)hook;
    }
    if (st.registered_waiters.load(std::memory_order_seq_cst) != 0u) {
        wake_one_parked(st);
    }
}

} // namespace vitreo::winapi

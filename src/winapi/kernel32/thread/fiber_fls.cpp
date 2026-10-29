/// @file fiber_fls.cpp
/// @brief Fiber Local Storage and fiber conversion (WIN32-072)

#include "fiber_fls.hpp"
#include "winapi/kernel32/error.hpp"
#include "thread.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "core/memory/guest_memory.hpp"
#include <algorithm>
#include <vector>

namespace vitreo::winapi {

// =============================================================================
// Construction
// =============================================================================

FlsManager::FlsManager() = default;

// =============================================================================
// FLS Operations
// =============================================================================

u64 next_fls_manager_generation() noexcept {
    static std::atomic<u64> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

namespace {

/// Per-thread memo of this thread's block in one manager. FLS get/set are leaf
/// accessors called once per scheduled task by the CRT and by thread-local
/// storage, so a process-global mutex here serialises every guest thread.
struct FlsBlockCache {
    u64 generation{0};
    u32 thread_id{0};
    FlsThreadSlots* slots{nullptr};
};
thread_local FlsBlockCache t_fls_block{};

} // namespace

FlsThreadSlots& FlsManager::thread_slots() {
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    if (t_fls_block.slots != nullptr && t_fls_block.generation == m_generation &&
        t_fls_block.thread_id == thread_id) {
        return *t_fls_block.slots;
    }
    FlsThreadSlots* slots = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& entry = m_thread_values[thread_id];
        if (!entry) entry = std::make_unique<FlsThreadSlots>();
        slots = entry.get();
    }
    // Entries are never erased while the manager lives, so the block address
    // stays valid for as long as this cache claims it.
    t_fls_block = FlsBlockCache{m_generation, thread_id, slots};
    return *slots;
}

u32 FlsManager::fls_alloc(GuestAddr callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (u32 i = 0; i < MAX_FLS_SLOTS; ++i) {
        if (!m_slots[i].allocated.load(std::memory_order_relaxed)) {
            m_slots[i].callback = callback;
            for (auto& [address, fiber] : m_fibers) fiber->values[i] = 0;
            for (auto& [thread_id, slots] : m_thread_values) {
                (void)thread_id;
                slots->values[i].store(0, std::memory_order_relaxed);
            }
            // Publish last: a reader that observes the index as allocated must
            // also observe the zeroed slot, not the previous owner's value.
            m_slots[i].allocated.store(true, std::memory_order_release);
            return i;
        }
    }
    set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
    return FLS_OUT_OF_INDEXES;
}

bool FlsManager::fls_free(u32 index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (index >= MAX_FLS_SLOTS ||
        !m_slots[index].allocated.load(std::memory_order_relaxed)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    m_slots[index].allocated.store(false, std::memory_order_release);
    m_slots[index].callback = 0;
    for (auto& [address, fiber] : m_fibers) fiber->values[index] = 0;
    for (auto& [thread_id, slots] : m_thread_values) {
        (void)thread_id;
        slots->values[index].store(0, std::memory_order_relaxed);
    }
    return true;
}

u64 FlsManager::fls_get_value(u32 index) {
    if (index >= MAX_FLS_SLOTS ||
        !m_slots[index].allocated.load(std::memory_order_acquire)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    const u64 value =
        thread_slots().values[index].load(std::memory_order_relaxed);
    VITREO_TRACE("[fls] FlsGetValue(%u, tid=%u) -> 0x%llx\n", index,
                 thread_id, static_cast<unsigned long long>(value));
    return value;
}

bool FlsManager::fls_set_value(u32 index, u64 value) {
    if (index >= MAX_FLS_SLOTS ||
        !m_slots[index].allocated.load(std::memory_order_acquire)) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    const u32 thread_id = ThreadManager::get_current_thread_id_static();
    VITREO_TRACE("[fls] FlsSetValue(%u, 0x%llx, tid=%u)\n", index,
                 static_cast<unsigned long long>(value), thread_id);
    thread_slots().values[index].store(value, std::memory_order_relaxed);
    return true;
}

// =============================================================================
// Fiber Operations
// =============================================================================

// =============================================================================
// Thunk Helpers
// =============================================================================

static void impl_FlsAlloc(Marshaler& m, FlsManager& mgr) {
    // DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback)
    GuestAddr cb = m.arg_ptr(0);
    u32 idx = mgr.fls_alloc(cb);
    m.set_return(static_cast<u64>(idx));
}

static void impl_FlsFree(Marshaler& m, FlsManager& mgr) {
    // BOOL FlsFree(DWORD dwFlsIndex)
    u32 index = static_cast<u32>(m.arg(0));
    bool ok = mgr.fls_free(index);
    m.set_return(ok ? 1u : 0u);
}

static void impl_FlsGetValue(Marshaler& m, FlsManager& mgr) {
    // PVOID FlsGetValue(DWORD dwFlsIndex)
    u32 index = static_cast<u32>(m.arg(0));
    u64 val = mgr.fls_get_value(index);
    m.set_return(val);
}

static void impl_FlsGetValue2(Marshaler& m, FlsManager& mgr) {
    // PVOID FlsGetValue2(DWORD dwFlsIndex). Learn: the fast variant performs
    // no error checking and does not touch the last error; measured on
    // Windows 11 it keeps a sentinel even for a NULL value, where FlsGetValue
    // sets ERROR_SUCCESS (last_error_on_success_probe.expected).
    const u32 entry_error = get_last_error();
    u32 index = static_cast<u32>(m.arg(0));
    u64 val = mgr.fls_get_value(index);
    set_last_error(entry_error);
    m.set_return(val);
}

static void impl_FlsSetValue(Marshaler& m, FlsManager& mgr) {
    // BOOL FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData)
    u32 index = static_cast<u32>(m.arg(0));
    u64 data = m.arg(1);
    bool ok = mgr.fls_set_value(index, data);
    m.set_return(ok ? 1u : 0u);
}

static void impl_ConvertThreadToFiber(Marshaler& m, FlsManager& mgr) {
    // LPVOID ConvertThreadToFiber(LPVOID lpParameter)
    u64 param = m.arg(0);
    u64 addr = mgr.convert_thread_to_fiber(param, m.cpu());
    m.set_return(addr);
}

static void impl_ConvertFiberToThread(Marshaler& m, FlsManager& mgr) {
    // BOOL ConvertFiberToThread(void)
    bool ok = mgr.convert_fiber_to_thread(m.cpu());
    m.set_return(ok ? 1u : 0u);
}

static void impl_IsThreadAFiber(Marshaler& m, FlsManager& mgr) {
    // BOOL IsThreadAFiber(void)
    bool is_fiber = mgr.is_thread_a_fiber();
    m.set_return(is_fiber ? 1u : 0u);
}

static void impl_GetFiberData(Marshaler& m, FlsManager& mgr) {
    // PVOID GetFiberData(void)
    u64 data = mgr.get_fiber_data();
    m.set_return(data);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports create_fiber_fls_thunks(FlsManager& mgr) {
    DllExports exports;
    exports.entries.reserve(8);

    exports.entries.push_back(ExportEntry{
        "FlsAlloc", 1,
        [&mgr](Marshaler& m) { impl_FlsAlloc(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FlsFree", 2,
        [&mgr](Marshaler& m) { impl_FlsFree(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FlsGetValue", 3,
        [&mgr](Marshaler& m) { impl_FlsGetValue(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FlsGetValue2", 9,
        [&mgr](Marshaler& m) { impl_FlsGetValue2(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "FlsSetValue", 4,
        [&mgr](Marshaler& m) { impl_FlsSetValue(m, mgr); },
        8
    });
    exports.entries.push_back(ExportEntry{
        "ConvertThreadToFiber", 5,
        [&mgr](Marshaler& m) { impl_ConvertThreadToFiber(m, mgr); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "ConvertFiberToThread", 6,
        [&mgr](Marshaler& m) { impl_ConvertFiberToThread(m, mgr); }
    });
    exports.entries.push_back(ExportEntry{
        "IsThreadAFiber", 7,
        [&mgr](Marshaler& m) { impl_IsThreadAFiber(m, mgr); },
        0});
    exports.entries.push_back(ExportEntry{
        "GetFiberData", 8,
        [&mgr](Marshaler& m) { impl_GetFiberData(m, mgr); }
    });

    exports.entries.push_back(ExportEntry{"CreateFiber", 0,
        [&mgr](Marshaler& m) { mgr.create_fiber(m, false); }, 12});
    exports.entries.push_back(ExportEntry{"CreateFiberEx", 0,
        [&mgr](Marshaler& m) { mgr.create_fiber(m, true); }, 20});
    exports.entries.push_back(ExportEntry{"SwitchToFiber", 0,
        [&mgr](Marshaler& m) { mgr.switch_to_fiber(m); }, 4});
    exports.entries.push_back(ExportEntry{"DeleteFiber", 0,
        [&mgr](Marshaler& m) { mgr.delete_fiber(m); }, 4});
    exports.entries.push_back(ExportEntry{"ConvertThreadToFiberEx", 0,
        [&mgr](Marshaler& m) { m.set_return(mgr.convert_thread_to_fiber(
            m.arg(0), m.cpu(), static_cast<u32>(m.arg(1)))); }, 8});

    return exports;
}

} // namespace vitreo::winapi

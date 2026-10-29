#pragma once
/// @file fiber_fls.hpp
/// @brief Fiber Local Storage and fiber conversion (WIN32-072)

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <functional>
#include <memory>

#include <array>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace vitreo { class GuestMemory; }

namespace vitreo::winapi {

class Marshaler;
class VirtualMemoryManager;

constexpr u32 MAX_FLS_SLOTS = 128;
constexpr u32 FLS_OUT_OF_INDEXES = 0xFFFFFFFF;

/// One guest thread's FLS slots. Held by stable address so a thread can cache
/// a pointer to its own block and read it without touching FlsManager's lock;
/// the values are atomic because FlsAlloc/FlsFree zero one index across every
/// registered thread while those threads may be reading it.
struct FlsThreadSlots {
    std::array<std::atomic<u64>, MAX_FLS_SLOTS> values{};
};

/// Distinguishes FlsManager instances for the per-thread block cache, so a new
/// manager constructed at a recycled address cannot inherit a stale pointer.
[[nodiscard]] u64 next_fls_manager_generation() noexcept;

/// Manages FLS slots and fiber state
class FlsManager {
public:
    FlsManager();
    void bind_runtime(GuestMemory& memory, VirtualMemoryManager& vmm,
                      GuestAddr exit_thread);
    void set_cleanup_callback(std::function<void(GuestAddr, u64)> callback) {
        m_cleanup = std::move(callback);
    }
    void create_fiber(Marshaler& m, bool extended);
    void switch_to_fiber(Marshaler& m);
    void delete_fiber(Marshaler& m);

    FlsManager(const FlsManager&) = delete;
    FlsManager& operator=(const FlsManager&) = delete;
    ~FlsManager() = default;

    // ---- FLS APIs ----

    /// Allocate an FLS index. callback is a guest address for cleanup (0 = none)
    [[nodiscard]] u32 fls_alloc(GuestAddr callback = 0);

    /// Free an FLS index
    bool fls_free(u32 index);

    /// Get FLS value for current thread
    [[nodiscard]] u64 fls_get_value(u32 index);

    /// Set FLS value for current thread
    bool fls_set_value(u32 index, u64 value);

    // ---- Fiber APIs ----

    /// ConvertThreadToFiber
    [[nodiscard]] u64 convert_thread_to_fiber(u64 parameter,
        cpu::CpuState* cpu = nullptr, u32 flags = 0);

    /// ConvertFiberToThread
    bool convert_fiber_to_thread(cpu::CpuState* cpu = nullptr);

    /// IsThreadAFiber
    [[nodiscard]] bool is_thread_a_fiber() const noexcept;

    /// GetFiberData
    [[nodiscard]] u64 get_fiber_data() const noexcept;

private:
    struct SlotInfo {
        std::atomic<bool> allocated{false};
        GuestAddr callback{0};
    };

    /// This thread's slot block, registering it on first use. Only the
    /// registration takes m_mutex; later calls on the same thread hit a
    /// thread-local cache.
    [[nodiscard]] FlsThreadSlots& thread_slots();

    mutable std::mutex m_mutex;
    std::array<SlotInfo, MAX_FLS_SLOTS> m_slots{};
    std::unordered_map<u32, std::unique_ptr<FlsThreadSlots>> m_thread_values;
    const u64 m_generation{next_fls_manager_generation()};
    struct Fiber {
        cpu::CpuState state;
        u64 parameter = 0;
        u64 stack_allocation = 0;
        u64 stack_base = 0;
        u64 stack_limit = 0;
        u64 exception_list = 0;
        u32 flags = 0;
        bool converted = false;
        u32 running_thread = 0;
        std::array<u64, MAX_FLS_SLOTS> values{};
    };
    std::unordered_map<u64, std::unique_ptr<Fiber>> m_fibers;
    std::unordered_map<u32, u64> m_current_fibers;
    GuestMemory* m_memory = nullptr;
    VirtualMemoryManager* m_vmm = nullptr;
    GuestAddr m_exit_thread = 0;
    std::function<void(GuestAddr, u64)> m_cleanup;
    void publish_fiber(cpu::CpuState& cpu, u64 address, const Fiber& fiber);
};

DllExports create_fiber_fls_thunks(FlsManager& mgr);

} // namespace vitreo::winapi

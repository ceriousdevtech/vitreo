#pragma once

/// @file tls.hpp
/// @brief Thread Local Storage and Fiber Local Storage (WIN32-039)
///
/// Implements TlsAlloc, TlsFree, TlsGetValue, TlsSetValue and
/// FLS equivalents for the emulated Win32 environment.
/// Slot allocation is process-global; values are per-thread (TEB TlsSlots /
/// TlsExpansionSlots when a CPU/TEB is attached, else host thread_local).

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <array>
#include <mutex>
#include <vector>

namespace vitreo {
class GuestMemory;
}

namespace vitreo::winapi {

// Forward declaration
class Marshaler;

// =============================================================================
// TLS Constants
// =============================================================================

constexpr u32 TLS_MINIMUM_AVAILABLE = 64;
constexpr u32 TLS_EXPANSION_SLOTS   = 1024;
constexpr u32 TLS_TOTAL_SLOTS       = TLS_MINIMUM_AVAILABLE + TLS_EXPANSION_SLOTS;
constexpr u32 TLS_OUT_OF_INDEXES    = 0xFFFFFFFF;

// =============================================================================
// TLS Manager
// =============================================================================

/// Manages Thread Local Storage and Fiber Local Storage slots
///
/// Slot allocation is global (same slot IDs shared across threads),
/// but values are per-thread using thread_local storage.
/// FLS functions alias directly to TLS (single-fiber assumption).
class TlsManager {
public:
    TlsManager();

    /// No copy
    TlsManager(const TlsManager&) = delete;
    TlsManager& operator=(const TlsManager&) = delete;

    /// Default move / dtor
    TlsManager(TlsManager&&) noexcept = default;
    TlsManager& operator=(TlsManager&&) = delete;
    ~TlsManager() = default;

    // -------------------------------------------------------------------------
    // TLS Operations
    // -------------------------------------------------------------------------

    /// Allocate a TLS slot (global — same slot index across all threads)
    /// @return Slot index, or TLS_OUT_OF_INDEXES if none available
    [[nodiscard]] u32 tls_alloc();

    /// Free a TLS slot
    /// @param index Slot index to free
    /// @return true on success, false if index is invalid
    bool tls_free(u32 index);

    /// Get the value stored in a TLS slot (per-thread)
    /// @param index Slot index
    /// @return Stored value (0 if slot is unset or index is invalid)
    [[nodiscard]] u64 tls_get_value(u32 index);

    /// Set the value in a TLS slot (per-thread)
    /// @param index Slot index
    /// @param value Value to store
    /// @return true on success, false if index is invalid
    bool tls_set_value(u32 index, u64 value);

    // -------------------------------------------------------------------------
    // FLS Operations (alias to TLS for single-fiber)
    // -------------------------------------------------------------------------

    /// Allocate an FLS slot (delegates to tls_alloc)
    [[nodiscard]] u32 fls_alloc();

    /// Free an FLS slot (delegates to tls_free)
    bool fls_free(u32 index);

    /// Get an FLS value (delegates to tls_get_value)
    [[nodiscard]] u64 fls_get_value(u32 index);

    /// Set an FLS value (delegates to tls_set_value)
    bool fls_set_value(u32 index, u64 value);

    /// Get the per-thread values array (creates if needed)
    static std::vector<u64>& thread_values();

    /// A guest thread's TEB exists: its TLS slots now belong to the set that
    /// TlsAlloc/TlsFree reset. Called by the thread lifecycle when the TEB is
    /// created (main thread and every CreateThread thread) -- never inferred
    /// from which threads happen to call a Tls* API, because the JIT fast path
    /// reads and writes slots without calling into this manager.
    void register_teb(GuestMemory& memory, GuestAddr teb, bool is_64bit);

    /// The TEB is about to be freed. Must precede the release, so a later
    /// TlsAlloc/TlsFree never writes into memory the TEB no longer owns.
    void unregister_teb(GuestAddr teb);

    /// Reset an allocated/reused index in every live guest TEB.
    ///
    /// Microsoft documents that TlsAlloc initializes the new index to NULL
    /// for every thread, not only the allocating thread.
    void clear_teb_index(u32 index);

private:
    struct TebRecord {
        GuestAddr base{0};
        bool is_64bit{false};
    };

    std::mutex        m_alloc_mutex;   // protects m_allocated
    std::vector<bool> m_allocated;
    GuestMemory* m_guest_memory{nullptr};
    std::vector<TebRecord> m_tebs;
};

/// TEB.TlsExpansionSlots of @p teb: the per-thread array behind TLS indices
/// 64..1087, allocated zeroed and published on first use, as Windows does on
/// the first TlsSetValue of an expansion index. 0 only when the guest is out
/// of memory -- the one way TlsSetValue fails (ERROR_NOT_ENOUGH_MEMORY).
///
/// Every reader and writer of expansion slots (the thunks, the JIT fast path)
/// goes through this one function. Its placement is first-fit and therefore
/// contended: every thread's first expansion write searches up from the same
/// address, so a search that loses the gap to another thread retries
/// (GuestMemory::map_first_fit) instead of failing.
[[nodiscard]] GuestAddr ensure_tls_expansion_slots(GuestMemory& memory,
                                                   GuestAddr teb, bool is_64bit);
/// TEB.TlsExpansionSlots of @p teb, or 0 when none was allocated.
[[nodiscard]] GuestAddr tls_expansion_slots(GuestMemory& memory, GuestAddr teb,
                                            bool is_64bit);
/// Free a thread's expansion array and clear the TEB field (thread exit).
void release_tls_expansion_slots(GuestMemory& memory, GuestAddr teb,
                                 bool is_64bit);

// =============================================================================
// Win32 Thunk Declarations
// =============================================================================

void impl_TlsAlloc(Marshaler& m, TlsManager& tm);
void impl_TlsFree(Marshaler& m, TlsManager& tm);
void impl_TlsGetValue(Marshaler& m, TlsManager& tm);
void impl_TlsSetValue(Marshaler& m, TlsManager& tm);
void impl_FlsAlloc(Marshaler& m, TlsManager& tm);
void impl_FlsFree(Marshaler& m, TlsManager& tm);
void impl_FlsGetValue(Marshaler& m, TlsManager& tm);
void impl_FlsSetValue(Marshaler& m, TlsManager& tm);

/// Build the export table for TLS/FLS functions
[[nodiscard]] DllExports make_tls_exports(TlsManager& tm);

} // namespace vitreo::winapi

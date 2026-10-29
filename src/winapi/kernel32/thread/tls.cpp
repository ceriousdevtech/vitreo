/// @file tls.cpp
/// @brief Implementation of Thread Local Storage and Fiber Local Storage (WIN32-039)

#include "tls.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/marshaler.hpp"
#include "core/memory/memory_region.hpp"

#include <algorithm>

namespace vitreo::winapi {

// =============================================================================
// Per-thread value storage (thread_local)
// =============================================================================

std::vector<u64>& TlsManager::thread_values() {
    thread_local std::vector<u64> values(TLS_TOTAL_SLOTS, 0);
    return values;
}

// =============================================================================
// Construction
// =============================================================================

TlsManager::TlsManager()
    : m_allocated(TLS_TOTAL_SLOTS, false) {}

// =============================================================================
// TLS Operations
// =============================================================================

u32 TlsManager::tls_alloc() {
    std::lock_guard<std::mutex> lock(m_alloc_mutex);
    for (u32 i = 0; i < TLS_TOTAL_SLOTS; ++i) {
        if (!m_allocated[i]) {
            m_allocated[i] = true;
            // Clear slot in current thread's storage
            thread_values()[i] = 0;
            return i;
        }
    }
    return TLS_OUT_OF_INDEXES;
}

bool TlsManager::tls_free(u32 index) {
    std::lock_guard<std::mutex> lock(m_alloc_mutex);
    if (index >= TLS_TOTAL_SLOTS || !m_allocated[index]) {
        return false;
    }
    m_allocated[index] = false;
    thread_values()[index] = 0;
    return true;
}

u64 TlsManager::tls_get_value(u32 index) {
    set_last_error(error_codes::ERROR_SUCCESS);
    if (index >= TLS_TOTAL_SLOTS) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    return thread_values()[index];
}

bool TlsManager::tls_set_value(u32 index, u64 value) {
    if (index >= TLS_TOTAL_SLOTS) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    thread_values()[index] = value;
    return true;
}

// =============================================================================
// FLS Operations (single-fiber aliases)
// =============================================================================

u32 TlsManager::fls_alloc() {
    return tls_alloc();
}

bool TlsManager::fls_free(u32 index) {
    return tls_free(index);
}

u64 TlsManager::fls_get_value(u32 index) {
    auto v = tls_get_value(index);
    VITREO_TRACE("[tls-fls] FlsGetValue(%u) -> 0x%llx\n", index,
                 static_cast<unsigned long long>(v));
    return v;
}

bool TlsManager::fls_set_value(u32 index, u64 value) {
    VITREO_TRACE("[tls-fls] FlsSetValue(%u, 0x%llx)\n", index,
                 static_cast<unsigned long long>(value));
    return tls_set_value(index, value);
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

// TLS values live in the guest TEB, not a host-only store. Optimized guest
// code (the CRT included) inlines TlsGetValue as TEB reads
// and never calls the export — so thunk set/get must match TEB layout:
//
//   TlsSlots[64]:        x86 TEB+0xE10 / x64 TEB+0x1480
//   TlsExpansionSlots*:  x86 TEB+0xF94 / x64 TEB+0x1780
//                        → pointer to 1024 slots, demand-allocated on Set
//
// Windows TlsGetValue returns 0 when the expansion array is not yet allocated
// for the calling thread; TlsSetValue allocates it on first use (nynaeve /
// public TEB layout). Host thread_values() remains the no-CPU fallback.
namespace {
constexpr u32 TLS_TEB_SLOTS = 64;  // TLS_MINIMUM_AVAILABLE
constexpr u32 TEB_TLS_SLOTS_32 = 0x0E10;
constexpr u32 TEB_TLS_SLOTS_64 = 0x1480;
constexpr u32 TEB_TLS_EXPANSION_32 = 0x0F94;
constexpr u32 TEB_TLS_EXPANSION_64 = 0x1780;
constexpr GuestAddr kTlsExpansionAllocMin = 0x5E090000;

[[nodiscard]] GuestAddr current_teb(Marshaler& m) {
    if (!m.has_cpu()) return 0;
    auto* c = m.cpu();
    if (m.cpu_is_64bit()) return static_cast<GuestAddr>(c->seg.gs_desc.base);
    return static_cast<GuestAddr>(c->seg.fs_desc.base);
}

[[nodiscard]] u32 ptr_size(Marshaler& m) {
    return m.cpu_is_64bit() ? 8u : 4u;
}

void tls_teb_write(Marshaler& m, GuestAddr slot, u64 value) {
    if (m.cpu_is_64bit()) (void)m.write_u64(slot, value);
    else (void)m.write_u32(slot, static_cast<u32>(value));
}

[[nodiscard]] u64 tls_teb_read(Marshaler& m, GuestAddr slot) {
    if (m.cpu_is_64bit()) return m.read_u64(slot).value_or(0);
    return static_cast<u64>(m.read_u32(slot).value_or(0));
}

// Read TEB.TlsExpansionSlots pointer (0 if unset / no TEB).
[[nodiscard]] GuestAddr read_expansion_ptr(Marshaler& m, GuestAddr teb) {
    return tls_expansion_slots(m.memory(), teb, m.cpu_is_64bit());
}

// Demand-allocate the 1024-slot expansion array and publish TEB.TlsExpansionSlots.
[[nodiscard]] GuestAddr ensure_expansion_array(Marshaler& m, GuestAddr teb) {
    return ensure_tls_expansion_slots(m.memory(), teb, m.cpu_is_64bit());
}

// Guest address of the slot for @p index, or 0 if unavailable.
// For expansion indices, @p allocate_expansion controls demand-alloc (Set only).
[[nodiscard]] GuestAddr tls_teb_slot_addr(Marshaler& m, u32 index,
                                          bool allocate_expansion) {
    const GuestAddr teb = current_teb(m);
    if (teb == 0) return 0;

    if (index < TLS_TEB_SLOTS) {
        if (m.cpu_is_64bit()) {
            return teb + TEB_TLS_SLOTS_64 +
                   static_cast<GuestAddr>(index) * 8;
        }
        return teb + TEB_TLS_SLOTS_32 + static_cast<GuestAddr>(index) * 4;
    }

    GuestAddr expansion = read_expansion_ptr(m, teb);
    if (expansion == 0) {
        if (!allocate_expansion) return 0;
        expansion = ensure_expansion_array(m, teb);
        if (expansion == 0) return 0;
    }
    const u32 off = index - TLS_TEB_SLOTS;
    return expansion + static_cast<GuestAddr>(off) * ptr_size(m);
}
}  // namespace

GuestAddr tls_expansion_slots(GuestMemory& memory, GuestAddr teb,
                              bool is_64bit) {
    if (teb == 0) return 0;
    const GuestAddr field =
        teb + (is_64bit ? TEB_TLS_EXPANSION_64 : TEB_TLS_EXPANSION_32);
    return is_64bit
        ? static_cast<GuestAddr>(memory.read_u64(field).value_or(0))
        : static_cast<GuestAddr>(memory.read_u32(field).value_or(0));
}

GuestAddr ensure_tls_expansion_slots(GuestMemory& memory, GuestAddr teb,
                                     bool is_64bit) {
    if (teb == 0) return 0;
    if (GuestAddr existing = tls_expansion_slots(memory, teb, is_64bit)) {
        return existing;
    }
    const usize bytes =
        static_cast<usize>(TLS_EXPANSION_SLOTS) * (is_64bit ? 8u : 4u);
    // Learn, TlsAlloc: every index starts NULL in every thread; a fresh array
    // holds no values yet. The bytes are written explicitly rather than
    // trusted to the backing.
    const GuestAddr base = memory.map_first_fit(kTlsExpansionAllocMin, bytes,
                                                MemoryProtection::ReadWrite);
    if (base == 0) {
        VITREO_TRACE("[tls] failed to allocate TlsExpansionSlots (%zu bytes)\n",
                     bytes);
        return 0;
    }
    const std::vector<u8> zeros(bytes, 0);
    (void)memory.write_bytes(base, zeros.data(), zeros.size());
    const GuestAddr field =
        teb + (is_64bit ? TEB_TLS_EXPANSION_64 : TEB_TLS_EXPANSION_32);
    if (is_64bit) (void)memory.write_u64(field, base);
    else (void)memory.write_u32(field, static_cast<u32>(base));
    return base;
}

void release_tls_expansion_slots(GuestMemory& memory, GuestAddr teb,
                                 bool is_64bit) {
    const GuestAddr base = tls_expansion_slots(memory, teb, is_64bit);
    if (base == 0) return;
    const GuestAddr field =
        teb + (is_64bit ? TEB_TLS_EXPANSION_64 : TEB_TLS_EXPANSION_32);
    if (is_64bit) (void)memory.write_u64(field, 0);
    else (void)memory.write_u32(field, 0);
    const usize bytes =
        static_cast<usize>(TLS_EXPANSION_SLOTS) * (is_64bit ? 8u : 4u);
    (void)memory.unmap_region(base, bytes);
}

void TlsManager::register_teb(GuestMemory& memory, GuestAddr teb, bool is_64bit) {
    if (teb == 0) return;
    std::lock_guard<std::mutex> lock(m_alloc_mutex);
    m_guest_memory = &memory;
    for (const TebRecord& record : m_tebs) {
        if (record.base == teb) return;
    }
    m_tebs.push_back(TebRecord{teb, is_64bit});
}

void TlsManager::unregister_teb(GuestAddr teb) {
    std::lock_guard<std::mutex> lock(m_alloc_mutex);
    m_tebs.erase(std::remove_if(m_tebs.begin(), m_tebs.end(),
                                [teb](const TebRecord& r) { return r.base == teb; }),
                 m_tebs.end());
}

void TlsManager::clear_teb_index(u32 index) {
    if (index >= TLS_TOTAL_SLOTS) return;

    std::lock_guard<std::mutex> lock(m_alloc_mutex);
    if (m_guest_memory == nullptr) return;

    for (const TebRecord& record : m_tebs) {
        const u32 pointer_size = record.is_64bit ? 8u : 4u;
        GuestAddr slot = 0;
        if (index < TLS_TEB_SLOTS) {
            slot = record.base +
                   (record.is_64bit ? TEB_TLS_SLOTS_64 : TEB_TLS_SLOTS_32) +
                   static_cast<GuestAddr>(index) * pointer_size;
        } else {
            const GuestAddr expansion_field =
                record.base + (record.is_64bit
                    ? TEB_TLS_EXPANSION_64 : TEB_TLS_EXPANSION_32);
            GuestAddr expansion = 0;
            if (record.is_64bit) {
                expansion = static_cast<GuestAddr>(
                    m_guest_memory->read_u64(expansion_field).value_or(0));
            } else {
                expansion = static_cast<GuestAddr>(
                    m_guest_memory->read_u32(expansion_field).value_or(0));
            }
            if (expansion == 0) continue;
            slot = expansion +
                   static_cast<GuestAddr>(index - TLS_TEB_SLOTS) * pointer_size;
        }

        if (record.is_64bit) {
            (void)m_guest_memory->write_u64(slot, 0);
        } else {
            (void)m_guest_memory->write_u32(slot, 0);
        }
    }
}

void impl_TlsAlloc(Marshaler& m, TlsManager& tm) {
    // DWORD TlsAlloc(void)
    // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsalloc
    u32 index = tm.tls_alloc();
    if (index != TLS_OUT_OF_INDEXES) {
        // A newly allocated (including reused) index is NULL in every thread.
        tm.clear_teb_index(index);
    } else {
        // Learn: failure -> TLS_OUT_OF_INDEXES + GetLastError; measured
        // ERROR_NOT_ENOUGH_MEMORY (thread_lifecycle_probe TLSALLOC_EXHAUST).
        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
    }
    m.set_return(static_cast<u64>(index));
}

void impl_TlsFree(Marshaler& m, TlsManager& tm) {
    // BOOL TlsFree(DWORD dwTlsIndex)
    u32 index = static_cast<u32>(m.arg(0));
    bool ok = tm.tls_free(index);
    if (ok) {
        tm.clear_teb_index(index);
        (void)tm.tls_set_value(index, 0);  // clear host fallback too
    } else {
        // Out of range or not allocated: ERROR_INVALID_PARAMETER (measured,
        // thread_lifecycle_probe TLSFREE_OUT_OF_RANGE / TLSFREE_TWICE).
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    }
    m.set_return(ok ? 1u : 0u);
}

namespace {

/// VITREO_TLS_DIAG=1 — report TLS slots whose reads keep answering 0.
bool tls_diag_enabled() {
    static const bool on = (std::getenv("VITREO_TLS_DIAG") != nullptr);
    return on;
}

void note_tls_get(u32 index, u64 value) {
    static std::atomic<u64> total{0};
    static std::atomic<u64> hits[64]{};
    static std::atomic<u64> zeros[64]{};
    if (index < 64) {
        hits[index].fetch_add(1, std::memory_order_relaxed);
        if (value == 0) zeros[index].fetch_add(1, std::memory_order_relaxed);
    }
    const u64 n = total.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n % 200000ull) != 0) return;
    std::fprintf(stderr, "[tls-diag] after %llu TlsGetValue calls:\n",
                 static_cast<unsigned long long>(n));
    for (u32 i = 0; i < 64; ++i) {
        const u64 h = hits[i].load(std::memory_order_relaxed);
        if (h == 0) continue;
        const u64 z = zeros[i].load(std::memory_order_relaxed);
        std::fprintf(stderr,
                     "[tls-diag]   slot %2u: %llu reads, %llu zero (%.1f%%)\n",
                     i, static_cast<unsigned long long>(h),
                     static_cast<unsigned long long>(z),
                     h ? 100.0 * static_cast<double>(z) / static_cast<double>(h) : 0.0);
    }
    std::fflush(stderr);
}

} // namespace

void impl_TlsGetValue(Marshaler& m, TlsManager& tm) {
    // LPVOID TlsGetValue(DWORD dwTlsIndex)
    // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsgetvalue
    u32 index = static_cast<u32>(m.arg(0));
    if (index >= TLS_TOTAL_SLOTS) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    set_last_error(error_codes::ERROR_SUCCESS);
    if (m.has_cpu() && current_teb(m) != 0) {
        // Windows: missing expansion array ⇒ 0 (still success).
        u64 value = 0;
        if (GuestAddr slot = tls_teb_slot_addr(m, index, /*allocate=*/false)) {
            value = tls_teb_read(m, slot);
        }
        // TlsGetValue is far too hot for a line per call, but a slot that
        // always answers 0 is worth seeing: UE's binned allocator keeps its
        // per-thread free lists in a TLS slot and falls back to the OS for
        // EVERY allocation when the lookup returns null. Count and report
        // periodically instead of tracing each call.
        if (tls_diag_enabled()) {
            note_tls_get(index, value);
        }
        m.set_return(value);
        return;
    }
    m.set_return(tm.tls_get_value(index));  // no-CPU unit-test fallback
}

void impl_TlsSetValue(Marshaler& m, TlsManager& tm) {
    // BOOL TlsSetValue(DWORD dwTlsIndex, LPVOID lpTlsValue)
    // https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlssetvalue
    u32 index = static_cast<u32>(m.arg(0));
    u64 value = m.arg(1);
    if (index >= TLS_TOTAL_SLOTS) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        m.set_return(0);
        return;
    }
    if (m.has_cpu() && current_teb(m) != 0) {
        GuestAddr slot = tls_teb_slot_addr(m, index, /*allocate=*/true);
        if (slot == 0) {
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            m.set_return(0);
            return;
        }
        tls_teb_write(m, slot, value);
        // Keep host fallback in sync for paths that lack a CPU/TEB later.
        (void)tm.tls_set_value(index, value);
        m.set_return(1u);
        return;
    }
    bool ok = tm.tls_set_value(index, value);
    m.set_return(ok ? 1u : 0u);
}

void impl_FlsAlloc(Marshaler& m, TlsManager& tm) {
    // DWORD FlsAlloc(PFLS_CALLBACK_FUNCTION lpCallback)
    // Callback parameter ignored for single-fiber assumption
    u32 index = tm.fls_alloc();
    m.set_return(static_cast<u64>(index));
}

void impl_FlsFree(Marshaler& m, TlsManager& tm) {
    // BOOL FlsFree(DWORD dwFlsIndex)
    u32 index = static_cast<u32>(m.arg(0));
    bool ok = tm.fls_free(index);
    m.set_return(ok ? 1u : 0u);
}

void impl_FlsGetValue(Marshaler& m, TlsManager& tm) {
    // PVOID FlsGetValue(DWORD dwFlsIndex)
    u32 index = static_cast<u32>(m.arg(0));
    u64 val = tm.fls_get_value(index);
    m.set_return(val);
}

void impl_FlsSetValue(Marshaler& m, TlsManager& tm) {
    // BOOL FlsSetValue(DWORD dwFlsIndex, PVOID lpFlsData)
    u32 index = static_cast<u32>(m.arg(0));
    u64 data = m.arg(1);
    bool ok = tm.fls_set_value(index, data);
    m.set_return(ok ? 1u : 0u);
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports make_tls_exports(TlsManager& tm) {
    DllExports exports;
    exports.entries.reserve(8);

    exports.entries.push_back(ExportEntry{
        "TlsAlloc", 1,
        [&tm](Marshaler& m) { impl_TlsAlloc(m, tm); }
    });
    exports.entries.push_back(ExportEntry{
        "TlsFree", 2,
        [&tm](Marshaler& m) { impl_TlsFree(m, tm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "TlsGetValue", 3,
        [&tm](Marshaler& m) { impl_TlsGetValue(m, tm); },
        4
    });
    exports.entries.push_back(ExportEntry{
        "TlsSetValue", 4,
        [&tm](Marshaler& m) { impl_TlsSetValue(m, tm); },
        8
    });
    // FLS APIs are owned by fiber_fls.cpp (create_fiber_fls_thunks).
    // Do not re-register them here — the second registration would override
    // the contract-correct guards in fiber_fls.cpp.

    return exports;
}

} // namespace vitreo::winapi

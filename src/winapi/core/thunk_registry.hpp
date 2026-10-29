#pragma once

/// @file thunk_registry.hpp
/// @brief ThunkRegistry for registering Win32 API thunks
///
/// Maps DLL function imports to thunk addresses for IAT patching.
/// Each registered function gets a unique thunk address that can be
/// resolved back to the API handler.

#include <vitreo/common.hpp>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vitreo::winapi {

// Forward declaration
class Marshaler;

// =============================================================================
// API Identifier
// =============================================================================

/// Unique identifier for each API function
struct ApiId {
    u16 dll_id;       ///< DLL index
    u16 function_id;  ///< Function index within DLL
    
    [[nodiscard]] constexpr bool operator==(const ApiId& other) const noexcept {
        return dll_id == other.dll_id && function_id == other.function_id;
    }
    
    [[nodiscard]] constexpr bool operator!=(const ApiId& other) const noexcept {
        return !(*this == other);
    }
    
    /// Pack ApiId into a 32-bit value
    [[nodiscard]] constexpr u32 pack() const noexcept {
        return (static_cast<u32>(dll_id) << 16) | static_cast<u32>(function_id);
    }
    
    /// Unpack a 32-bit value into an ApiId
    [[nodiscard]] static constexpr ApiId unpack(u32 packed) noexcept {
        return ApiId{
            static_cast<u16>(packed >> 16),
            static_cast<u16>(packed & 0xFFFF)
        };
    }
};

// =============================================================================
// API Handler Type
// =============================================================================

/// Function signature for all API implementations
/// The handler receives a Marshaler for argument/return value handling
using ApiHandler = std::function<void(Marshaler&)>;

// =============================================================================
// Export Entry
// =============================================================================

/// An export's host body as one plain function call: `fn(context, m, slot)`.
///
/// The direct entry is what a crossing calls when it can skip the general
/// dispatch (resolve_handler, the std::function layers, the per-crossing
/// diagnostics): cli::run_direct_thunk today, and the target an in-chain call
/// from translated code (L3) can branch to later. It IS the export's
/// implementation -- register_dll derives `handler` from it -- so it does
/// everything the export does, its own locks included.
struct DirectHandler {
    void (*fn)(void* context, Marshaler& m, u32 slot){nullptr};
    void* context{nullptr};
    u32 slot{0};
};

/// Whether translated code may serve an export's direct entry in-chain
/// (L3: cli::serve_in_chain_thunk, reached from JIT code without returning
/// to the dispatcher). A property of the export's DOCUMENTED contract, set
/// at registration from the export's identity (for COM methods, the SDK
/// header's method name in that vtable slot) -- never from what a run
/// happened to observe. Anything not proven eligible is None.
///
/// Every class keeps the whole direct crossing (cli::run_direct_thunk): the
/// export's own locks, the arena safepoint (the chain's lease is parked for
/// the call), SuspendThread's host-call rendezvous, the nonvolatile restore
/// and the stdcall pop. The class only says whether the crossing may RESUME
/// in translated code afterwards, so what it must rule out is anything
/// after which the guest's next instruction is not the one after the call.
/// How long the call takes is not part of it: a class-A export may block
/// (the device lock, a GPU readback or flush, a synchronous shader compile)
/// exactly as on the dispatcher path, with its host call published and its
/// lease parked, and the resume decision is made after it returns.
enum class InChainClass : u8 {
    /// Not classified (anything not proven eligible), or by contract the
    /// export may transfer control -- raise, unwind, longjmp, end the thread
    /// or process (never eligible: ends_thread_or_process) -- or run guest
    /// code other than through run_guest_callback; or kept out by decision
    /// (D3D11 Map: d3d11_in_chain.cpp). Every call takes the dispatcher
    /// crossing.
    None = 0,
    /// Class A: by contract it returns to its caller (no raise, unwind or
    /// control transfer) and runs no guest code. It may block.
    A = 1,
    /// Class A, except that by contract it may run guest code -- only through
    /// run_guest_callback, a nested and balanced call -- namely IUnknown::
    /// AddRef or Release on an interface the GUEST implements: the D3D11/DXGI
    /// private-data interfaces (Learn, ID3D11DeviceChild::
    /// SetPrivateDataInterface: "Release will be called on the interface"
    /// when the private data is destroyed), which an object's destruction
    /// releases -- and a context drops the last reference to an object
    /// whenever it replaces or clears a binding it holds a reference to
    /// (Learn: the binding methods "hold a reference to the interfaces
    /// passed in"). That guest call is a nested, balanced callback
    /// (run_guest_callback); a crossing that made one resumes through the
    /// dispatcher instead of in-chain.
    AGuestComRef = 2,
    /// Class A, except that by contract it may call a guest function the
    /// program registered for the purpose -- only through
    /// run_guest_callback: the CRT math functions, which call the user
    /// `_matherr` installed with `__setusermatherr` on a math error (Learn,
    /// _matherr). Served exactly as AGuestComRef.
    AGuestCallback = 3,
};

/// True when @p cls lets translated code serve the export in-chain.
[[nodiscard]] constexpr bool in_chain_eligible(InChainClass cls) noexcept {
    return cls == InChainClass::A || cls == InChainClass::AGuestComRef ||
           cls == InChainClass::AGuestCallback;
}

/// Exports that end the calling thread or process by contract (Learn:
/// ExitProcess, ExitThread, FreeLibraryAndExitThread, TerminateProcess and
/// TerminateThread, which may name the caller; ntdll's RtlExitUserProcess,
/// RtlExitUserThread and the Nt/Zw TerminateProcess and TerminateThread
/// system services; the CRT's exit, _exit, _Exit, quick_exit and abort).
/// None of these is ever in-chain, whatever class it is registered with
/// (register_dll registers it None): it has no caller to return to, so its
/// exit stays entirely the dispatcher's. (The in-chain serve does test
/// has_exited() after every call, as the dispatcher does after every thunk,
/// but only so that ANOTHER thread's ExitProcess stops this one.)
[[nodiscard]] bool ends_thread_or_process(std::string_view name) noexcept;

/// Represents a single DLL export
struct ExportEntry {
    std::string name;      ///< Function name (may be empty for ordinal-only exports)
    u16 ordinal;           ///< Export ordinal
    ApiHandler handler;    ///< Host-side implementation
    u16 stdcall_bytes{0};  ///< Bytes of args for callee to pop (stdcall, 32-bit only)
    bool internal{false}; ///< Runtime tail target; absent from guest export lookup
    /// Optional direct entry: then the export's whole body. Such an entry
    /// leaves `handler` EMPTY (make_direct_export) and register_dll derives
    /// `handler` from `direct`, so the two cannot disagree. A non-empty
    /// `handler` beside `direct` can only be a wrapper added later (for
    /// example a per-DLL `entry.handler = [inner = ...]` loop); register_dll
    /// then refuses to publish the direct entry, loudly, rather than let the
    /// direct crossing bypass the wrapper. To wrap such an export, wrap its
    /// DirectHandler instead.
    DirectHandler direct{};
    /// The export's in-chain class (InChainClass), from its documented
    /// contract. With a direct entry it is that entry's (L3); without one,
    /// an eligible class makes register_dll publish an in-chain-only entry
    /// for the registered handler (L3-K: see DirectThunk::in_chain_only).
    InChainClass in_chain{InChainClass::None};
};

/// An export whose implementation is @p direct (see ExportEntry::direct).
/// @p in_chain is the export's contract class (InChainClass); None unless
/// the caller derived it from the export's documented contract.
[[nodiscard]] ExportEntry make_direct_export(
    std::string name, DirectHandler direct, u16 stdcall_bytes,
    InChainClass in_chain = InChainClass::None);

/// Call an export's body as a crossing would: its direct entry when it has
/// one, else its handler. For code (tests, mostly) that holds a DllExports
/// before it is registered.
inline void call_export(const ExportEntry& entry, Marshaler& m) {
    if (entry.direct.fn != nullptr) {
        entry.direct.fn(entry.direct.context, m, entry.direct.slot);
    } else {
        entry.handler(m);
    }
}

/// One dispatch gateway's direct entry (ThunkRegistry::direct_thunk). An
/// entry, once published, never changes and lives as long as its registry,
/// so a caller may keep the pointer (an emitted call site, for one).
struct DirectThunk {
    DirectHandler handler;
    /// The export's x86 stdcall argument bytes (what its RET pops past the
    /// return address). Recorded for both bitnesses: x64 pops none, but the
    /// count still bounds where the stack arguments of either ABI end.
    u16 stdcall_bytes{0};
    /// Whether translated code may serve it in-chain (InChainClass).
    InChainClass in_chain{InChainClass::None};
    /// L3-K: an entry register_dll built around an export's REGISTERED
    /// handler (no direct entry of its own) so that the in-chain crossing
    /// can serve it. Only the in-chain crossing ever uses it: direct_thunk
    /// -- the dispatcher's direct crossing -- skips it, so without the
    /// in-chain call every crossing of the export takes the path it took
    /// before.
    bool in_chain_only{false};
    /// Sync TEB.LastErrorValue around the call exactly as the full path
    /// does (into the host copy before, back out after): set for every
    /// in_chain_only entry, whose handlers report errors through it. The
    /// D3D11/DXGI direct entries leave it alone (thunk_direct.hpp).
    bool sync_last_error{false};
};

/// The in-chain slot map's byte for a slot (ThunkRegistry::in_chain_slot_map;
/// translated code tests only for nonzero).
inline constexpr u8 kInChainSlotDirect = 1;  ///< a direct entry (L3, WP-R1)
inline constexpr u8 kInChainSlotHost = 2;    ///< an in_chain_only entry (L3-K)

/// Collection of exports for a DLL
struct DllExports {
    std::vector<ExportEntry> entries;
};

// =============================================================================
// Thunk Registry
// =============================================================================

/// Manages thunk address allocation and API lookup
///
/// The ThunkRegistry:
/// - Allocates unique guest addresses for each API function
/// - Maps DLL name + function name/ordinal to thunk addresses
/// - Resolves thunk addresses back to API handlers
class ThunkRegistry {
public:
    static constexpr usize STUB_SIZE = 8;

    /// Create a ThunkRegistry with the given thunk region base address
    /// @param thunk_region_base Base address for thunk allocation
    /// @param thunk_spacing Bytes between each thunk (default 16)
    explicit ThunkRegistry(GuestAddr thunk_region_base, usize thunk_spacing = 16);
    
    /// No copy
    ThunkRegistry(const ThunkRegistry&) = delete;
    ThunkRegistry& operator=(const ThunkRegistry&) = delete;
    
    /// Move operations
    ThunkRegistry(ThunkRegistry&&) noexcept = default;
    ThunkRegistry& operator=(ThunkRegistry&&) noexcept = default;
    
    /// Destructor
    ~ThunkRegistry() = default;
    
    // -------------------------------------------------------------------------
    // DLL Registration
    // -------------------------------------------------------------------------
    
    /// Register a DLL's exports
    /// @param dll_name DLL name (case-insensitive, e.g., "kernel32.dll")
    /// @param exports Collection of export entries
    /// @return Success or error if DLL already registered
    [[nodiscard]] VoidResult register_dll(std::string_view dll_name, DllExports exports);

    /// Allocate a synthetic "missing export" thunk for an import that could not
    /// be resolved by any registered DLL or extra resolver.
    ///
    /// The returned thunk address is patched into the IAT so that calls go
    /// through a logging handler instead of dereferencing a NULL slot. The
    /// handler returns `missing_export_return_value(dll, func)` — an explicit
    /// failure under the calling convention we can infer, and 0xFFFFFFFF
    /// otherwise. Subsequent calls to the same dll!func reuse the same thunk
    /// address.
    ///
    /// The first CALL through a stub prints an unmissable banner (an
    /// unimplemented API that is actually invoked is always a Vitreo defect)
    /// and later calls are re-reported at powers of two, so a hot missing
    /// export can never scroll silently out of a log. Setting
    /// `VITREO_STRICT_MISSING_EXPORTS` aborts the process on that first call.
    ///
    /// @param dll_name DLL name (case-insensitive)
    /// @param func_name Function name (empty for ordinal-only imports)
    /// @param ordinal Export ordinal (0 if imported by name)
    /// @return Guest address of the synthetic stub thunk
    [[nodiscard]] GuestAddr register_missing_export(
        std::string_view dll_name,
        std::string_view func_name,
        u16 ordinal);

    /// Return value a synthetic missing-export stub for `dll!func` will hand
    /// back to the guest.
    ///
    /// 0xFFFFFFFF is the default: it is an explicit failure for HRESULT,
    /// NTSTATUS, LSTATUS and errno_t, and it is the value every app that
    /// currently survives a missing import already sees. It is *not* a failure
    /// for BOOL (it reads as TRUE) or for a NULL-on-failure pointer/handle, so
    /// the value is overridden wherever the DLL and name identify the return
    /// convention from a documented contract:
    ///   - `PathCch*` / `PathAlloc*` (pathcch.h)  -> E_NOTIMPL
    ///   - `DllGetClassObject` and friends        -> E_NOTIMPL
    ///   - `ntdll!Nt*` / `ntdll!Zw*`              -> STATUS_NOT_IMPLEMENTED
    ///
    /// @param dll_name  Normalized (lowercase) DLL name
    /// @param func_name Function name (empty for ordinal-only imports)
    [[nodiscard]] static u64 missing_export_return_value(
        std::string_view dll_name,
        std::string_view func_name);

    /// Number of times the synthetic stub for `dll!func` has been called.
    /// Zero if the symbol was never stubbed, or was imported but never called
    /// (the common, harmless case). Diagnostics and tests only.
    [[nodiscard]] u64 missing_export_call_count(
        std::string_view dll_name,
        std::string_view func_name) const;

    /// Install the thread-local last-error setter used by missing-export
    /// stubs.
    ///
    /// Every documented Win32 failure path leaves a retrievable error code
    /// behind; a stub that returns a failure marker and leaves the slot alone
    /// makes the guest report "failed, GetLastError() == 0", a state no real
    /// Windows API produces. The error slot lives in the kernel32 module,
    /// which sits above this one, so the owner of both injects the setter
    /// here instead of this module reaching upward for it.
    static void set_missing_export_error_hook(void (*setter)(u32)) noexcept;

    // -------------------------------------------------------------------------
    // Address Lookup
    // -------------------------------------------------------------------------
    
    /// Get thunk address for a function by name
    /// @param dll_name DLL name (case-insensitive)
    /// @param func_name Function name (case-sensitive)
    /// @return Thunk address or NotFound error
    [[nodiscard]] VResult<GuestAddr> get_thunk_address(
        std::string_view dll_name,
        std::string_view func_name,
        bool include_internal = false
    ) const;
    
    /// Get thunk address for a function by ordinal
    /// @param dll_name DLL name (case-insensitive)
    /// @param ordinal Export ordinal
    /// @return Thunk address or NotFound error
    [[nodiscard]] VResult<GuestAddr> get_thunk_address(
        std::string_view dll_name,
        u16 ordinal
    ) const;

    /// Publish a synthesized export ordinal without changing enumeration order.
    /// Existing ordinals may only be registered again for the same handler.
    [[nodiscard]] bool register_ordinal_alias(
        std::string_view dll_name, u16 ordinal, GuestAddr thunk_address);

    /// Register a DLL *data* export (e.g. msvcrt's `_iob`, `_pctype`).
    ///
    /// Data imports must patch the IAT slot with the address of the variable
    /// itself, never with a callable thunk: the guest dereferences the slot as
    /// data. Registration is idempotent for an identical address.
    ///
    /// @param dll_name DLL name (case-insensitive)
    /// @param name Exported variable name
    /// @param address Guest address of the variable
    void register_data_export(std::string_view dll_name,
                              std::string_view name,
                              GuestAddr address);

    /// Look up a DLL data export registered via register_data_export().
    /// @return Guest address of the variable, or 0 when not a data export.
    [[nodiscard]] GuestAddr lookup_data_export(std::string_view dll_name,
                                               std::string_view name) const;

    /// Route an exact guest address outside the thunk arena to an existing
    /// thunk handler. Synthetic PE entry veneers use this for instruction
    /// addresses which must retain Windows-compatible bytes while still
    /// entering Vitreo's host implementation when executed.
    ///
    /// Registration is idempotent when @p alias already names the same thunk.
    /// Returns false if the target is unknown, the alias is inside the thunk
    /// arena, or an existing alias names a different thunk.
    [[nodiscard]] bool register_dispatch_alias(
        GuestAddr alias,
        GuestAddr thunk_address
    );

    /// Publish ntdll's system-service table: entry N is the thunk that native
    /// service number N enters (its stub's `mov eax, N`, then SYSCALL,
    /// SYSENTER or INT 2Eh). Replaces the table published before, because
    /// the synthetic ntdll image numbers its services afresh each time it is
    /// rebuilt.
    void publish_native_services(std::vector<GuestAddr> thunks_by_number);

    /// The thunk native service @p number enters, or 0 when @p number is past
    /// the table.
    [[nodiscard]] GuestAddr native_service_thunk(u32 number) const noexcept;

    // -------------------------------------------------------------------------
    // Resolution
    // -------------------------------------------------------------------------
    
    /// Resolve a thunk address to its API identifier
    /// @param thunk_address Address to resolve
    /// @return ApiId or NotFound error
    [[nodiscard]] VResult<ApiId> resolve_address(GuestAddr thunk_address) const;
    
    /// Resolve a thunk address to its handler
    /// @param thunk_address Address to resolve
    /// @return Pointer to handler or NotFound error
    [[nodiscard]] VResult<const ApiHandler*> resolve_handler(GuestAddr thunk_address) const;

    /// Resolve a thunk address to its stdcall cleanup bytes
    /// @param thunk_address Address to resolve
    /// @return Number of bytes the callee should pop (0 for cdecl/fastcall)
    [[nodiscard]] u16 resolve_stdcall_bytes(GuestAddr thunk_address) const;

    /// The direct entry behind the slab dispatch gateway @p dispatch, or
    /// null when that export has none (or @p dispatch is not a slab gateway).
    ///
    /// Lock-free: one acquire load of an immutable table that register_dll
    /// republishes whole; every published table lives as long as the
    /// registry. Never built under VITREO_API_PROFILE, whose timing wraps
    /// `handler`: profiling keeps every call on the profiled path.
    [[nodiscard]] const DirectThunk* direct_thunk(
        GuestAddr dispatch) const noexcept {
        const DirectTable* table =
            direct_table_.load(std::memory_order_acquire);
        if (table == nullptr || dispatch < thunk_base_) return nullptr;
        // A table exists only for a power-of-two spacing (register_dll), so
        // the gateway test is a mask and the slab index a shift.
        const GuestAddr offset = dispatch - thunk_base_;
        if ((offset & direct_spacing_mask_) != STUB_SIZE) return nullptr;
        const usize index = static_cast<usize>(offset >> direct_spacing_shift_);
        if (index >= table->count) return nullptr;
        const DirectThunk& entry = table->entries[index];
        // In-chain-only entries are not direct entries (DirectThunk).
        return entry.handler.fn != nullptr && !entry.in_chain_only ? &entry
                                                                   : nullptr;
    }

    /// The in-chain slot map translated code tests before it offers a thunk
    /// to the in-chain crossing (L3): a u64 slot count, then one byte per
    /// slab slot -- kInChainSlotDirect for a direct entry whose InChainClass
    /// is in_chain_eligible while the direct crossing is statically on
    /// (com_direct::statically_on), kInChainSlotHost for an in_chain_only
    /// entry (built only while L3-K is on: host_in_chain::enabled), else 0
    /// -- indexed by
    /// (address - thunk_base()) >> kInChainSlotShift for a stub or its
    /// gateway alike. Never freed (it lives in the direct table it was
    /// built with), so a reader may keep the pointer; a reader holding an
    /// older map just sees fewer slots. Its bytes are fixed once published
    /// except through withdraw_in_chain, which only ever clears a byte to 0
    /// (one relaxed atomic byte store, under the registry lock): a reader
    /// racing it sees the old byte, and the serve then declines the call
    /// (the sled is no longer intact), or the new one. Never null: with no
    /// eligible export or a spacing other than 16 it is a zero-slot map.
    [[nodiscard]] const u8* in_chain_slot_map() const noexcept {
        const DirectTable* table =
            direct_table_.load(std::memory_order_acquire);
        return table != nullptr && table->in_chain_map
                   ? table->in_chain_map.get()
                   : empty_in_chain_slot_map();
    }
    /// The zero-slot map (also the JIT's default before any publication).
    [[nodiscard]] static const u8* empty_in_chain_slot_map() noexcept;
    /// Guest code now occupies @p stub's entry sled (x86 guest_accel.cpp
    /// patches it with a jmp to a guest-side implementation): the in-chain
    /// crossing would decline every call there (the sled is not intact), so
    /// translated code must not call out for it. Clears the slot in every
    /// map published so far (a reader may hold an older one) and in every
    /// map published later.
    void withdraw_in_chain(GuestAddr stub) noexcept;
    static constexpr u32 kInChainSlotShift = 4;  ///< log2(thunk spacing)

    /// The entry (direct or in_chain_only) for an in-chain candidate
    /// @p addr: its gateway, or its entry stub (whose sled the caller must
    /// still check). Null for anything else. @p is_stub says which.
    /// Shift-and-mask only: the slot map exists only for a 16-byte spacing.
    [[nodiscard]] const DirectThunk* in_chain_candidate(
        GuestAddr addr, bool& is_stub) const noexcept {
        const DirectTable* table =
            direct_table_.load(std::memory_order_acquire);
        if (table == nullptr || !table->in_chain_map || addr < thunk_base_) {
            return nullptr;
        }
        const GuestAddr offset = addr - thunk_base_;
        const GuestAddr within = offset & ((GuestAddr{1} << kInChainSlotShift) - 1);
        if (within != 0 && within != STUB_SIZE) return nullptr;
        const usize index = static_cast<usize>(offset >> kInChainSlotShift);
        if (index >= table->count) return nullptr;
        const DirectThunk& entry = table->entries[index];
        if (entry.handler.fn == nullptr) return nullptr;
        is_stub = within == 0;
        return &entry;
    }

    /// resolve_handler calls made by the calling thread (all registries)
    /// while counting is on. Tests use it to prove a crossing never
    /// consulted the general path; off (the default) it costs resolve_handler
    /// one load of a constant and a predicted branch, no thread-local access.
    [[nodiscard]] static u64 resolve_handler_calls_this_thread() noexcept;
    static void count_resolve_handler_calls(bool on) noexcept;

    /// Resolve an ApiId to its handler
    /// @param api_id API identifier
    /// @return Pointer to handler or NotFound error
    [[nodiscard]] VResult<const ApiHandler*> get_handler(ApiId api_id) const;
    
    // -------------------------------------------------------------------------
    // Inspection
    // -------------------------------------------------------------------------
    
    [[nodiscard]] u64 instance_id() const noexcept { return instance_id_; }

    /// Get the number of registered DLLs
    [[nodiscard]] usize dll_count() const {
        std::shared_lock lock(*dispatch_alias_mutex_);
        return dll_entries_.size();
    }
    
    /// Get the total number of registered functions
    [[nodiscard]] usize function_count() const noexcept { return total_functions_; }
    
    /// Get the base address of the thunk region
    [[nodiscard]] GuestAddr thunk_base() const noexcept { return thunk_base_; }
    
    /// Cached "dll!function" for a stub or dispatch-gateway address, or an
    /// empty string when the address names no registered thunk. Diagnostics
    /// only: the API profiler needs to turn a dispatch address back into a
    /// name, and nothing else in the registry inverts that mapping.
    [[nodiscard]] std::string debug_name_for(GuestAddr addr) const;

    /// Get the end of the allocated thunk region (exclusive)
    [[nodiscard]] GuestAddr thunk_end() const noexcept { return next_thunk_; }

    /// A guest-code implementation of the export whose thunk is @p thunk.
    ///
    /// For a few tiny, hot exports Vitreo supplies real guest machine code
    /// (winapi/msvcrt/guest_accel), the way Windows itself implements them in
    /// a handful of user-mode instructions. Guest-facing resolution -- import
    /// binding, delay-load binding, GetProcAddress -- hands out that code's
    /// address via guest_entry(), so a call links like any guest function
    /// instead of leaving translated code for a host thunk. Internal Vitreo
    /// users (the fast-thunk table, tail-jump fixups) keep the thunk address,
    /// and the thunk itself still works for a guest that finds it through the
    /// export table. Written once at startup, before any guest thread runs.
    void set_guest_implementation(GuestAddr thunk, GuestAddr code) {
        guest_impl_[thunk] = code;
    }
    /// @p thunk's guest implementation, or @p thunk itself when it has none.
    [[nodiscard]] GuestAddr guest_entry(GuestAddr thunk) const {
        if (guest_impl_.empty()) return thunk;
        const auto it = guest_impl_.find(thunk);
        return it != guest_impl_.end() ? it->second : thunk;
    }
    
    /// Get the next available thunk address
    [[nodiscard]] GuestAddr next_thunk_address() const noexcept { return next_thunk_; }
    
    /// Check if an address falls within the allocated thunk region
    /// @param addr Guest address to check
    /// @return true if addr is within [thunk_base_, next_thunk_)
    [[nodiscard]] bool is_in_thunk_region(GuestAddr addr) const noexcept {
        return addr >= thunk_base_ && addr < next_thunk_;
    }

    /// Return true for bytes in the guest-visible, hookable entry stub.
    [[nodiscard]] bool is_stub_address(GuestAddr addr) const noexcept {
        if (!is_in_thunk_region(addr)) return false;
        return ((addr - thunk_base_) % thunk_spacing_) < STUB_SIZE;
    }

    /// Return true for an exact internal host-dispatch gateway.
    ///
    /// Hot path: every guest run-loop iteration probes the current RIP. The
    /// contiguous thunk slab is decided by arithmetic alone (no mutex). Out-of-
    /// region dispatch aliases (ntdll syscall stubs, COM remoting) use a
    /// generation-gated thread-local snapshot — a bare `atomic_load` of
    /// `shared_ptr` still takes libc++ `__sp_mut` and showed up next to
    /// `pthread_mutex_lock` after the first cut of this optimization.
    [[nodiscard]] bool is_dispatch_address(GuestAddr addr) const noexcept {
        if (is_in_thunk_region(addr)) {
            return thunk_spacing_ != 0 &&
                   ((addr - thunk_base_) % thunk_spacing_) == STUB_SIZE;
        }
        const u64 gen =
            dispatch_alias_gen_.load(std::memory_order_acquire);
        if (gen == 0) return false;  // no out-of-region aliases published

        struct AliasCache {
            u64 owner_id{0};
            u64 gen{0};
            std::shared_ptr<const AliasMap> map;
        };
        static thread_local AliasCache cache;
        if (cache.owner_id != instance_id_ || cache.gen != gen) {
            cache.map = std::atomic_load_explicit(&dispatch_aliases_,
                                                  std::memory_order_acquire);
            cache.gen = gen;
            cache.owner_id = instance_id_;
        }
        return cache.map && cache.map->find(addr) != cache.map->end();
    }

    /// For an address inside a thunk entry stub (the guest-visible NOP sled),
    /// return the slot's internal dispatch gateway (slot_base + STUB_SIZE) when
    /// that gateway lies in the published thunk range; otherwise 0. The
    /// execution loop uses this to jump past an unmodified NOP sled in one
    /// step instead of single-stepping every NOP on each import call.
    [[nodiscard]] GuestAddr stub_dispatch_target(GuestAddr addr) const noexcept {
        if (!is_stub_address(addr)) return 0;
        const GuestAddr slot_base =
            thunk_base_ +
            ((addr - thunk_base_) / thunk_spacing_) * thunk_spacing_;
        const GuestAddr dispatch = slot_base + STUB_SIZE;
        // Sequential allocate_thunk() publishes [thunk_base_, next_thunk_), so
        // a gateway-shaped address in that range is a live dispatch entry.
        return is_in_thunk_region(dispatch) ? dispatch : 0;
    }

    /// Normalize a get_thunk_address result (entry stub) or an already-dispatch
    /// address to the gateway compared against cpu.rip after NOP-sled skip.
    [[nodiscard]] GuestAddr dispatch_address_for_export(
        GuestAddr stub_or_dispatch) const noexcept {
        if (GuestAddr d = stub_dispatch_target(stub_or_dispatch)) {
            return d;
        }
        if (is_dispatch_address(stub_or_dispatch)) {
            return stub_or_dispatch;
        }
        const GuestAddr candidate = stub_or_dispatch + STUB_SIZE;
        if (is_in_thunk_region(candidate) && is_dispatch_address(candidate)) {
            return candidate;
        }
        return stub_or_dispatch;
    }

    /// For a guest instruction address registered with register_dispatch_alias,
    /// the in-region dispatch gateway that identifies its thunk; 0 otherwise.
    ///
    /// native64 executes guest code on the real CPU, so an aliased instruction
    /// cannot be recognised before it runs -- it has to trap. The fault path
    /// then resumes the thread through the shared collector with this gateway
    /// pushed as the return address, which is byte-for-byte what a thunk
    /// stub's `call fs:[collect]` does.
    [[nodiscard]] GuestAddr alias_dispatch_gateway(
        GuestAddr alias) const noexcept;

    /// Check if a DLL is registered
    [[nodiscard]] bool has_dll(std::string_view dll_name) const;

    /// Every registered DLL's normalized (lowercase) name, sorted.
    [[nodiscard]] std::vector<std::string> dll_names() const;

    /// Resolve a thunk address to "dll!function" name (for debug logging)
    [[nodiscard]] std::string resolve_name(GuestAddr thunk_address) const;

    /// One row of enumerate_exports().
    struct ExportInfo {
        std::string dll_name;        ///< Normalized (lowercase) DLL name
        std::string function_name;   ///< Export name (empty if ordinal-only)
        u16         ordinal{0};
        u16         stdcall_bytes{0};
        GuestAddr   thunk_address{0};
    };

    /// Enumerate every (dll, function) registered. Useful for tooling
    /// (e.g. auto-generating oracle contract tests). Names are
    /// lowercased for the DLL and original-case for the function.
    [[nodiscard]] std::vector<ExportInfo> enumerate_exports() const;

    /// Gateway address of `vitreo-cfg.dll!__guard_dispatch_icall`, or 0 when
    /// it is not registered. Cached: a CFG-compiled guest reaches this export
    /// on every indirect call, so identifying it must not cost a lookup (and
    /// certainly not a resolve_name() string). Thunk registration completes
    /// before the guest runs, so the address is stable once resolved.
    [[nodiscard]] GuestAddr guard_dispatch_icall_address() const noexcept {
        GuestAddr cached =
            guard_dispatch_addr_.load(std::memory_order_relaxed);
        if (cached != kGuardDispatchUnresolved) return cached;
        GuestAddr resolved = 0;
        if (auto stub = get_thunk_address("vitreo-cfg.dll",
                                          "__guard_dispatch_icall");
            stub.is_ok()) {
            resolved = dispatch_address_for_export(stub.value());
        }
        guard_dispatch_addr_.store(resolved, std::memory_order_relaxed);
        return resolved;
    }

private:
    u64 instance_id_;
    static constexpr GuestAddr kGuardDispatchUnresolved = ~GuestAddr{0};
    mutable std::atomic<GuestAddr> guard_dispatch_addr_{
        kGuardDispatchUnresolved};

    /// Internal thunk entry
    struct ThunkEntry {
        ApiId api_id;
        GuestAddr thunk_address;
        ApiHandler handler;
        u16 stdcall_bytes{0};  ///< Bytes to pop for stdcall cleanup
        std::string debug_name; ///< Cached "dll!function" for diagnostics
        bool internal{false};
    };
    
    /// Per-DLL data
    struct DllEntry {
        u16 dll_id;
        std::unordered_map<std::string, usize> by_name;    ///< name -> thunk index
        std::unordered_map<u16, usize> by_ordinal;          ///< ordinal -> thunk index
        std::unordered_map<u16, usize> ordinal_aliases;
    };
    
    /// Normalize DLL name to lowercase
    [[nodiscard]] static std::string normalize_dll_name(std::string_view name);
    
    /// Allocate a new thunk address
    [[nodiscard]] GuestAddr allocate_thunk();

    /// O(1) index for an in-slab stub or dispatch-gateway address.
    /// Returns false for out-of-region, inter-stub padding, or an index past
    /// the published thunk table (fall back to addr_to_thunk_).
    /// Caller holds dispatch_alias_mutex_ (shared): it reads thunks_.size().
    [[nodiscard]] bool slab_thunk_index(GuestAddr addr, usize& out_idx) const noexcept;
    
    /// set_guest_implementation(): thunk address -> guest code address.
    std::unordered_map<GuestAddr, GuestAddr> guest_impl_;
    GuestAddr thunk_base_;                                    ///< Base of thunk region
    GuestAddr next_thunk_;                                    ///< Next available thunk address
    usize thunk_spacing_;                                     ///< Bytes between thunks
    usize total_functions_{0};                                ///< Total registered functions
    
    /// Deque so `register_missing_export` can append while another thread
    /// holds an `ApiHandler*` into an existing entry. `vector` reallocation
    /// ran neighboring `std::function` destructors during `_wfopen`.
    ///
    /// Every table below (thunks_, dll_entries_, addr_to_thunk_,
    /// missing_export_*, data_exports_) is read and written only under
    /// dispatch_alias_mutex_: shared for lookups, unique for registration.
    /// A missing-export registration (any guest thread's LoadLibrary) appends
    /// to the deque while every other thread's dispatcher indexes it; a deque
    /// append can reallocate its block map, so even `thunks_[idx]` needs the
    /// shared hold. Cached `ApiHandler*` / `ThunkEntry&` stay valid (deque
    /// references survive push_back; nothing is ever erased). None of the
    /// locked functions calls another locked one (std::shared_mutex is not
    /// recursive).
    std::deque<ThunkEntry> thunks_;
    /// Slab slots withdraw_in_chain cleared (under dispatch_alias_mutex_).
    std::vector<usize> in_chain_withdrawn_;
    std::unordered_map<std::string, DllEntry> dll_entries_;   ///< Per-DLL lookups
    std::unordered_map<GuestAddr, usize> addr_to_thunk_;      ///< Address -> thunk index
    std::shared_ptr<std::shared_mutex> dispatch_alias_mutex_{
        std::make_shared<std::shared_mutex>()};

    /// Lock-free snapshot of out-of-region dispatch aliases only. Readers of
    /// `is_dispatch_address` load this with acquire (via a thread-local cache
    /// keyed by `dispatch_alias_gen_`); writers clone-and-publish under
    /// `dispatch_alias_mutex_`. In-region gateways never appear here — they
    /// are recognized by thunk-slab arithmetic.
    using AliasMap = std::unordered_map<GuestAddr, usize>;
    std::shared_ptr<const AliasMap> dispatch_aliases_{
        std::make_shared<const AliasMap>()};
    /// Bumped on every successful out-of-region alias publish. Zero means no
    /// aliases have ever been registered, so `is_dispatch_address` can reject
    /// non-slab RIPs with a single atomic load and no `shared_ptr` traffic.
    mutable std::atomic<u64> dispatch_alias_gen_{0};

    /// publish_native_services' table, replaced whole: guest threads read it
    /// on every native service while the loader may republish it.
    std::shared_ptr<const std::vector<GuestAddr>> native_services_{
        std::make_shared<const std::vector<GuestAddr>>()};

    /// Cache of synthetic "missing export" stub thunks keyed by
    /// "<lower-dll>!<func>" or "<lower-dll>!#<ordinal>" so duplicate imports
    /// of the same missing symbol share one thunk.
    std::unordered_map<std::string, GuestAddr> missing_export_thunks_;

    /// Per-stub call counters, same key space as `missing_export_thunks_`.
    /// Shared with the stub handler so the count survives registry moves.
    std::unordered_map<std::string, std::shared_ptr<std::atomic<u64>>>
        missing_export_calls_;

    /// Pretty name ("dll!func" or "dll!#ord") for synthetic missing-export
    /// thunks, indexed by thunk address. Used by resolve_name() for tracing.
    std::unordered_map<GuestAddr, std::string> missing_export_names_;

    /// DLL data exports keyed by "<lower-dll>!<name>". These resolve to the
    /// address of a guest variable rather than to a callable thunk.
    std::unordered_map<std::string, GuestAddr> data_exports_;

    /// direct_thunk(): entries indexed like thunks_ (by slab index), with a
    /// null `fn` for exports that have no direct entry. register_dll
    /// publishes a new table whenever a DLL brings direct entries; older
    /// tables stay in direct_tables_ so a reader holding one never sees it
    /// freed. Only built when thunk_spacing_ is nonzero (direct_thunk
    /// divides by it).
    struct DirectTable {
        usize count{0};
        std::unique_ptr<DirectThunk[]> entries;
        /// in_chain_slot_map(): {u64 count, u8 eligible[count]}, built with
        /// the table (null when the spacing is not 1 << kInChainSlotShift).
        std::unique_ptr<u8[]> in_chain_map;
    };
    std::atomic<const DirectTable*> direct_table_{nullptr};
    std::vector<std::unique_ptr<DirectTable>> direct_tables_;
    /// thunk_spacing_ as a mask and a shift (valid whenever a table exists).
    GuestAddr direct_spacing_mask_{0};
    u32 direct_spacing_shift_{0};
};

} // namespace vitreo::winapi

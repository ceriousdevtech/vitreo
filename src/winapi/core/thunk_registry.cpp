/// @file thunk_registry.cpp
/// @brief ThunkRegistry implementation

#include "winapi/core/thunk_registry.hpp"
#include <winapi/core/api_profile.hpp>
#include <chrono>
#include "winapi/core/marshaler.hpp"
#include "winapi/core/com_direct.hpp"
#include "winapi/core/host_in_chain.hpp"

#include <algorithm>
#include <cstring>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace vitreo::winapi {

namespace {
struct ThunkResolveCache {
    u64 owner_id{0};  ///< ThunkRegistry::instance_id_; 0 is never assigned
    GuestAddr addr{0};
    const ApiHandler* handler{nullptr};
    u16 stdcall_bytes{0};
    u64 gen{0};
};
/// Per-thread resolutions, direct-mapped by thunk address. One entry missed on
/// almost every call of a thread that alternates APIs (UE's RHI thread runs
/// Draw / Set* / Map back to back), and every miss took the registry's
/// std::shared_mutex, whose libc++ lock_shared serialises readers on an
/// internal mutex: 2.4% of that thread, a third of it parked in the kernel.
/// An entry is valid for one registry instance (instance_id_, never its
/// address), one thunk address and an unchanged dispatch_alias_gen_ (cached
/// handlers point into the never-erased thunks_ deque).
constexpr usize kThunkResolveEntries = 64;
thread_local std::array<ThunkResolveCache, kThunkResolveEntries> t_resolve;

[[nodiscard]] ThunkResolveCache& resolve_entry(GuestAddr addr) noexcept {
    // Thunks sit at a fixed spacing; a multiplicative hash spreads any
    // power-of-two spacing over every entry.
    return t_resolve[static_cast<usize>(
        (addr * 0x9E3779B97F4A7C15ull) >> 58)];
}

// -----------------------------------------------------------------------------
// Missing-export stub return-value policy
// -----------------------------------------------------------------------------
//
// A synthetic stub has to pick one integer for a call whose real return
// convention it does not know. No single value is a failure under every Win32
// convention:
//
//   0xFFFFFFFF  BOOL     -> TRUE            (looks like SUCCESS)
//               HRESULT  -> S=1, FAILED()   (honest failure)
//               NTSTATUS -> high bit set    (honest failure)
//               LSTATUS  -> != ERROR_SUCCESS(honest failure)
//               errno_t  -> != 0            (honest failure)
//               HANDLE   -> INVALID_HANDLE_VALUE for the Create* family
//                           (honest), but non-NULL for the Open*/NULL-on-
//                           failure family (looks like SUCCESS)
//               pointer  -> non-NULL garbage (looks like SUCCESS until deref)
//               count    -> 4294967295      (catastrophic if used as a length)
//   0x00000000  is the exact mirror: honest for BOOL/pointer/handle/count and
//               a silent SUCCESS for HRESULT/NTSTATUS/LSTATUS/errno_t.
//
// So the default stays 0xFFFFFFFF (failure under the four status conventions,
// and the value every currently-working app already survives), and we override
// it only where the DLL+name identifies the convention with certainty from
// documented contracts. Narrow on purpose: a wrong guess here is a new silent
// corruption, which is the very bug class this exists to remove.
constexpr u64 kMissingExportLegacyFailure = 0xFFFFFFFFull;
constexpr u64 kMissingExportENotImpl = 0x80004001ull;       // E_NOTIMPL
constexpr u64 kMissingExportStatusNotImpl = 0xC0000002ull;  // STATUS_NOT_IMPLEMENTED

bool name_starts_with(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() &&
           name.compare(0, prefix.size(), prefix) == 0;
}

bool strict_missing_exports() {
    static const bool strict = [] {
        const char* v = std::getenv("VITREO_STRICT_MISSING_EXPORTS");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return strict;
}
}  // namespace

u64 ThunkRegistry::missing_export_return_value(std::string_view dll_name,
                                               std::string_view func_name) {
    if (func_name.empty()) {
        // Ordinal-only import: nothing to infer from.
        return kMissingExportLegacyFailure;
    }

    // --- HRESULT surfaces -----------------------------------------------------
    // pathcch.dll / api-ms-win-core-path-l1-1-0.dll: every documented
    // PathCch*/PathAlloc* export returns HRESULT (Microsoft Learn, pathcch.h).
    // PathIsUNCEx is the one BOOL export in that header and is excluded by the
    // prefix test.
    if (name_starts_with(func_name, "PathCch") ||
        name_starts_with(func_name, "PathAlloc")) {
        return kMissingExportENotImpl;
    }
    // Documented in-proc server entry points; all return HRESULT.
    if (func_name == "DllGetClassObject" || func_name == "DllCanUnloadNow" ||
        func_name == "DllRegisterServer" || func_name == "DllUnregisterServer" ||
        func_name == "DllInstall") {
        return kMissingExportENotImpl;
    }

    // --- NTSTATUS surfaces ----------------------------------------------------
    // ntdll's Nt*/Zw* system-service surface returns NTSTATUS by contract.
    // Rtl* is deliberately excluded: it returns BOOLEAN, pointers, void and
    // NTSTATUS depending on the function.
    if (dll_name == "ntdll.dll" && func_name.size() > 2 &&
        (name_starts_with(func_name, "Nt") || name_starts_with(func_name, "Zw")) &&
        func_name[2] >= 'A' && func_name[2] <= 'Z') {
        return kMissingExportStatusNotImpl;
    }

    return kMissingExportLegacyFailure;
}

// =============================================================================
// Constructor
// =============================================================================

ThunkRegistry::ThunkRegistry(GuestAddr thunk_region_base, usize thunk_spacing)
    : instance_id_([] {
          static std::atomic<u64> next{1};
          return next.fetch_add(1, std::memory_order_relaxed);
      }())
    , thunk_base_(thunk_region_base)
    , next_thunk_(thunk_region_base)
    , thunk_spacing_(thunk_spacing)
{
    // direct_thunk masks and shifts; a spacing that is not a power of two
    // (none is in use: the default is 16) simply gets no direct table.
    if (thunk_spacing_ != 0 && (thunk_spacing_ & (thunk_spacing_ - 1)) == 0) {
        direct_spacing_mask_ = static_cast<GuestAddr>(thunk_spacing_ - 1);
        direct_spacing_shift_ =
            static_cast<u32>(__builtin_ctzll(static_cast<u64>(thunk_spacing_)));
    }
}

// =============================================================================
// DLL Registration
// =============================================================================

namespace {

/// The DirectHandler of an in_chain_only entry (L3-K): the export's
/// registered handler, the same object resolve_handler returns (it lives in
/// thunks_, a deque, so it never moves).
void call_registered_handler(void* context, Marshaler& m, u32 /*slot*/) {
    (*static_cast<const ApiHandler*>(context))(m);
}

/// Structural: an export that ends the thread or process is never in-chain,
/// whatever class it was registered with (ends_thread_or_process: it has no
/// caller to return to, and its exit stays the dispatcher's).
[[nodiscard]] InChainClass in_chain_class_for(std::string_view dll,
                                              const ExportEntry& entry) {
    if (!in_chain_eligible(entry.in_chain) ||
        !ends_thread_or_process(entry.name)) {
        return entry.in_chain;
    }
    std::fprintf(stderr,
                 "[vitreo] BUG %.*s!%s: ends the thread or process, never "
                 "in-chain; registered as InChainClass::None\n",
                 static_cast<int>(dll.size()), dll.data(), entry.name.c_str());
    return InChainClass::None;
}

}  // namespace

VoidResult ThunkRegistry::register_dll(std::string_view dll_name, DllExports exports) {
    auto normalized = normalize_dll_name(dll_name);
    std::unique_lock lock(*dispatch_alias_mutex_);
    
    // Check if already registered
    if (dll_entries_.find(normalized) != dll_entries_.end()) {
        return VoidResult::err(Error::AlreadyExists);
    }
    
    // Create DLL entry
    auto dll_id = static_cast<u16>(dll_entries_.size());
    DllEntry dll_entry{};
    dll_entry.dll_id = dll_id;
    
    // Direct entries this DLL brings (direct_thunk), by slab index. Never under
    // VITREO_API_PROFILE: the profile times `handler`, so profiling keeps every
    // call on the path that runs it.
    const bool build_direct = thunk_spacing_ != 0 &&
                              (thunk_spacing_ & (thunk_spacing_ - 1)) == 0 &&
                              !api_profile::enabled();
    std::vector<std::pair<usize, DirectThunk>> direct_entries;

    // Register each export
    u16 function_id = 0;
    for (auto& entry : exports.entries) {
        // Allocate thunk address
        auto thunk_addr = allocate_thunk();
        auto thunk_index = thunks_.size();
        
        // Create API ID
        ApiId api_id{dll_id, function_id};
        
        // Store thunk entry
        std::string debug_name;
        if (!entry.name.empty()) {
            debug_name.reserve(normalized.size() + 1 + entry.name.size());
            debug_name.append(normalized);
            debug_name.push_back('!');
            debug_name.append(entry.name);
        }
        // Profiling wraps the handler rather than a call site: the JIT, the
        // interpreter and native32 each reach handlers by a different route,
        // and only the handler itself is common to all three.
        // An export defined by its direct entry runs that entry on the full
        // path too: one body, whichever way the crossing reaches it.
        if (entry.direct.fn != nullptr) {
            if (!entry.handler) {
                entry.handler = [direct = entry.direct](Marshaler& m) {
                    direct.fn(direct.context, m, direct.slot);
                };
                if (build_direct) {
                    direct_entries.emplace_back(
                        thunk_index,
                        DirectThunk{entry.direct, entry.stdcall_bytes,
                                    in_chain_class_for(normalized, entry)});
                }
            } else {
                // A handler beside a direct entry is a wrapper added after
                // make_direct_export (see ExportEntry::direct): publishing the
                // entry would let the direct crossing skip it.
                std::fprintf(stderr,
                             "[vitreo] BUG %s!%s: handler wrapped around a "
                             "direct export; its direct entry is not used\n",
                             normalized.c_str(), entry.name.c_str());
            }
        }
        ApiHandler handler = std::move(entry.handler);
        if (api_profile::enabled()) {
            api_profile::set_name_source(this);
            const GuestAddr key = thunk_addr + STUB_SIZE;   // dispatch gateway
            handler = [inner = std::move(handler), key](Marshaler& m) {
                const auto t0 = std::chrono::steady_clock::now();
                inner(m);
                api_profile::record(key, static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count()));
            };
        }
        thunks_.push_back(ThunkEntry{
            api_id,
            thunk_addr,
            std::move(handler),
            entry.stdcall_bytes,
            std::move(debug_name),
            entry.internal
        });
        // L3-K: an export without a direct entry whose contract class lets
        // it resume in-chain gets an in-chain-only entry around the handler
        // just registered (DirectThunk::in_chain_only). The dispatcher never
        // uses it; with L3-K off (VITREO_L3K=0, or a full-path diagnostic:
        // host_in_chain::full_path_diagnostic) none is built, so the slot
        // map never marks these exports and their calls never call out.
        if (build_direct && entry.direct.fn == nullptr &&
            thunks_.back().handler && host_in_chain::enabled()) {
            if (const InChainClass in_chain =
                    in_chain_class_for(normalized, entry);
                in_chain_eligible(in_chain)) {
                DirectThunk host{
                    DirectHandler{&call_registered_handler,
                                  &thunks_.back().handler, 0},
                    entry.stdcall_bytes, in_chain};
                host.in_chain_only = true;
                host.sync_last_error = true;
                direct_entries.emplace_back(thunk_index, host);
            }
        }
        
        // Index by name if available
        if (!entry.name.empty()) {
            dll_entry.by_name[entry.name] = thunk_index;
        }
        
        // Index by ordinal. Ordinal 0 means name-only (no PE export slot);
        // indexing it would make every name-only export collide on key 0.
        if (entry.ordinal != 0) {
            dll_entry.by_ordinal[entry.ordinal] = thunk_index;
        }
        
        // Index by address
        addr_to_thunk_[thunk_addr] = thunk_index;
        addr_to_thunk_[thunk_addr + STUB_SIZE] = thunk_index;
        dispatch_alias_gen_.fetch_add(1, std::memory_order_release);

        // Optional: correlate a raw thunk address back to its dll!name.
        static const bool log_thunkmap =
            std::getenv("VITREO_LOG_THUNKMAP") != nullptr;
        if (log_thunkmap && !entry.name.empty()) {
            std::fprintf(stderr, "[thunkmap] %s!%s -> 0x%llx\n",
                         normalized.c_str(), entry.name.c_str(),
                         (unsigned long long)thunk_addr);
        }

        ++function_id;
        ++total_functions_;
    }
    
    // Store DLL entry
    dll_entries_[normalized] = std::move(dll_entry);

    if (!direct_entries.empty()) {
        // Publish a whole new table: every slab index so far, the previous
        // table's entries carried over, this DLL's added. Readers never lock.
        auto table = std::make_unique<DirectTable>();
        table->count = thunks_.size();
        table->entries = std::make_unique<DirectThunk[]>(table->count);
        if (const DirectTable* old =
                direct_table_.load(std::memory_order_relaxed)) {
            std::copy_n(old->entries.get(), std::min(old->count, table->count),
                        table->entries.get());
        }
        for (const auto& [index, direct] : direct_entries) {
            table->entries[index] = direct;
        }
        // The in-chain slot map over the whole table (in_chain_slot_map):
        // one byte per slab slot, so translated code can tell an in-chain
        // export from any other thunk with one load, before calling out. A
        // direct entry is marked only while the direct crossing is
        // statically on: with VITREO_D3D11_COM_DIRECT=0 no crossing could be
        // served, so its calls must not pay the call-out to be declined.
        if (thunk_spacing_ == (usize{1} << kInChainSlotShift)) {
            table->in_chain_map =
                std::make_unique<u8[]>(sizeof(u64) + table->count);
            const u64 slots = table->count;
            std::memcpy(table->in_chain_map.get(), &slots, sizeof(slots));
            const bool direct_on = com_direct::statically_on();
            for (usize i = 0; i < table->count; ++i) {
                const DirectThunk& slot = table->entries[i];
                u8 byte = 0;
                if (slot.handler.fn != nullptr &&
                    in_chain_eligible(slot.in_chain)) {
                    if (slot.in_chain_only) {
                        byte = kInChainSlotHost;
                    } else if (direct_on) {
                        byte = kInChainSlotDirect;
                    }
                }
                table->in_chain_map[sizeof(u64) + i] = byte;
            }
            for (const usize withdrawn : in_chain_withdrawn_) {
                if (withdrawn < table->count) {
                    table->in_chain_map[sizeof(u64) + withdrawn] = 0;
                }
            }
        }
        // Kept before it is published: a reader may hold it from then on.
        direct_tables_.push_back(std::move(table));
        direct_table_.store(direct_tables_.back().get(),
                            std::memory_order_release);
    }

    return VoidResult::ok();
}

bool ends_thread_or_process(std::string_view name) noexcept {
    static constexpr std::string_view kExits[] = {
        // kernel32 / kernelbase
        "ExitProcess", "ExitThread", "FreeLibraryAndExitThread",
        "TerminateProcess", "TerminateThread",
        // ntdll
        "RtlExitUserProcess", "RtlExitUserThread",
        "NtTerminateProcess", "ZwTerminateProcess",
        "NtTerminateThread", "ZwTerminateThread",
        // the C runtime
        "exit", "_exit", "_Exit", "quick_exit", "abort",
    };
    for (const std::string_view exit_name : kExits) {
        if (name == exit_name) return true;
    }
    return false;
}

void ThunkRegistry::withdraw_in_chain(GuestAddr stub) noexcept {
    if (stub < thunk_base_) return;
    const usize index =
        static_cast<usize>((stub - thunk_base_) >> kInChainSlotShift);
    std::unique_lock lock(*dispatch_alias_mutex_);
    in_chain_withdrawn_.push_back(index);
    // Every map ever published, not only the current one: a JIT context may
    // still hold an older map (in_chain_slot_map lets a reader keep the
    // pointer). The maps are owned here; this is the only store to a
    // published byte, and only ever to 0, so a reader racing it sees the
    // old byte (and the serve declines: the sled is not intact) or the new
    // one. Maps published later apply in_chain_withdrawn_ themselves.
    for (const auto& table : direct_tables_) {
        if (table->in_chain_map && index < table->count) {
            std::atomic_ref<u8>(table->in_chain_map[sizeof(u64) + index])
                .store(0, std::memory_order_relaxed);
        }
    }
}

const u8* ThunkRegistry::empty_in_chain_slot_map() noexcept {
    // {u64 count = 0}: no slot is in-chain.
    alignas(8) static constexpr u8 kEmpty[sizeof(u64)] = {};
    return kEmpty;
}

ExportEntry make_direct_export(std::string name, DirectHandler direct,
                               u16 stdcall_bytes, InChainClass in_chain) {
    // `handler` stays empty: register_dll derives it (ExportEntry::direct).
    ExportEntry entry{std::move(name), 0, ApiHandler{}, stdcall_bytes};
    entry.direct = direct;
    entry.in_chain = in_chain;
    return entry;
}

namespace {
std::atomic<bool> g_count_resolve_handler_calls{false};
thread_local u64 t_resolve_handler_calls = 0;
}  // namespace

u64 ThunkRegistry::resolve_handler_calls_this_thread() noexcept {
    return t_resolve_handler_calls;
}

void ThunkRegistry::count_resolve_handler_calls(bool on) noexcept {
    g_count_resolve_handler_calls.store(on, std::memory_order_relaxed);
}

// =============================================================================
// Synthetic "missing export" stubs
// =============================================================================

namespace {
/// Injected by the layer that owns both the registry and the kernel32 error
/// slot; null in unit tests that construct a registry on its own.
std::atomic<void (*)(u32)> g_missing_export_error_setter{nullptr};
}  // namespace

void ThunkRegistry::set_missing_export_error_hook(
    void (*setter)(u32)) noexcept {
    g_missing_export_error_setter.store(setter, std::memory_order_relaxed);
}

GuestAddr ThunkRegistry::register_missing_export(
    std::string_view dll_name,
    std::string_view func_name,
    u16 ordinal)
{
    auto normalized_dll = normalize_dll_name(dll_name);

    // Build cache key + pretty name. Prefer name; fall back to ordinal.
    std::string key = normalized_dll + "!";
    std::string pretty = normalized_dll + "!";
    if (!func_name.empty()) {
        key.append(func_name);
        pretty.append(func_name);
    } else {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "#%u", static_cast<unsigned>(ordinal));
        key.append(buf);
        pretty.append(buf);
    }

    std::unique_lock lock(*dispatch_alias_mutex_);
    // Dedup: same dll!func across multiple importers shares one thunk.
    auto cached = missing_export_thunks_.find(key);
    if (cached != missing_export_thunks_.end()) {
        return cached->second;
    }

    auto thunk_addr = allocate_thunk();
    auto thunk_index = thunks_.size();

    // Synthetic API id outside the real DLL id-space (high u16) so nothing
    // confuses these with registered DLLs.
    ApiId api_id{static_cast<u16>(0xFFFFu),
                 static_cast<u16>(missing_export_thunks_.size() & 0xFFFFu)};

    // Per-stub call counter, shared with the handler. An unimplemented API that
    // is actually CALLED is always a Vitreo defect, so it must stay visible for
    // the whole run rather than being logged once and suppressed forever.
    auto call_count = std::make_shared<std::atomic<u64>>(0);
    missing_export_calls_[key] = call_count;

    const u64 return_value = missing_export_return_value(normalized_dll, func_name);
    std::string captured = pretty; // owned copy for the lambda

    ApiHandler handler = [call_count, captured, return_value](Marshaler& m) {
        const u64 n = call_count->fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1) {
            std::fprintf(stderr,
                "\n[winapi] *** UNIMPLEMENTED API CALLED: %s ***\n"
                "[winapi] *** returning 0x%08llX; the guest asked for behavior "
                "Vitreo does not provide. This is a Vitreo defect, not a guest "
                "bug. ***\n\n",
                captured.c_str(),
                static_cast<unsigned long long>(return_value & 0xFFFFFFFFull));
            if (strict_missing_exports()) {
                std::fprintf(stderr,
                    "[winapi] VITREO_STRICT_MISSING_EXPORTS: terminating on the "
                    "first call to an unimplemented export (%s)\n",
                    captured.c_str());
                std::fflush(stderr);
                std::abort();
            }
        } else if ((n & (n - 1)) == 0 || (n % (1ull << 20)) == 0) {
            // Powers of two, then every ~1M: bounded output, but a hot
            // unimplemented API can never scroll silently out of the log.
            std::fprintf(stderr,
                "[winapi] unimplemented API %s still being called (%llu times)\n",
                captured.c_str(), static_cast<unsigned long long>(n));
        }
        // A failure return must carry a retrievable error code:
        // ERROR_CALL_NOT_IMPLEMENTED (120) is what Windows reports for an
        // export that exists but is not implemented on this platform. Without
        // it the guest sees "the call failed and GetLastError() == 0", a state
        // no documented Win32 failure path produces, and its own logs then
        // name the wrong culprit.
        if (auto* setter =
                g_missing_export_error_setter.load(std::memory_order_relaxed)) {
            setter(120u);  // ERROR_CALL_NOT_IMPLEMENTED
        }
        // Return-convention-aware explicit failure; see
        // missing_export_return_value() for why the default stays 0xFFFFFFFF
        // and where it is overridden. Note 0xFFFFFFFF is NOT null-ish for a
        // pointer return -- that is exactly one of the conventions it fails to
        // serve, which is why the override table exists.
        m.set_return(return_value);
    };

    thunks_.push_back(ThunkEntry{
        api_id,
        thunk_addr,
        std::move(handler),
        0, // Caller cleans up. Safe for cdecl; stdcall argument size is unknown.
        pretty
    });
    addr_to_thunk_[thunk_addr] = thunk_index;
    addr_to_thunk_[thunk_addr + STUB_SIZE] = thunk_index;
    dispatch_alias_gen_.fetch_add(1, std::memory_order_release);
    missing_export_thunks_[key] = thunk_addr;
    // Log the mapping at registration time so guest crashes that report a
    // raw thunk address (e.g. EIP=0x700008e0) can be correlated to a name.
    std::fprintf(stderr,
        "[winapi] missing-export stub %s -> 0x%llx (returns 0x%08llX if called)\n",
        pretty.c_str(),
        static_cast<unsigned long long>(thunk_addr),
        static_cast<unsigned long long>(return_value & 0xFFFFFFFFull));
    missing_export_names_[thunk_addr] = pretty;
    missing_export_names_[thunk_addr + STUB_SIZE] = std::move(pretty);

    return thunk_addr;
}

u64 ThunkRegistry::missing_export_call_count(std::string_view dll_name,
                                             std::string_view func_name) const {
    std::string key = normalize_dll_name(dll_name) + "!" + std::string(func_name);
    std::shared_lock lock(*dispatch_alias_mutex_);
    auto it = missing_export_calls_.find(key);
    if (it == missing_export_calls_.end() || !it->second) return 0;
    return it->second->load(std::memory_order_relaxed);
}

// =============================================================================
// Data Exports
// =============================================================================

void ThunkRegistry::register_data_export(std::string_view dll_name,
                                         std::string_view name,
                                         GuestAddr address) {
    if (name.empty() || address == 0) return;
    std::unique_lock lock(*dispatch_alias_mutex_);
    data_exports_[normalize_dll_name(dll_name) + "!" + std::string(name)] =
        address;
}

GuestAddr ThunkRegistry::lookup_data_export(std::string_view dll_name,
                                            std::string_view name) const {
    if (name.empty()) return 0;
    std::shared_lock lock(*dispatch_alias_mutex_);
    auto it = data_exports_.find(normalize_dll_name(dll_name) + "!" +
                                 std::string(name));
    return it == data_exports_.end() ? 0 : it->second;
}

// =============================================================================
// Address Lookup - By Name
// =============================================================================

VResult<GuestAddr> ThunkRegistry::get_thunk_address(
    std::string_view dll_name,
    std::string_view func_name,
    bool include_internal
) const {
    auto normalized = normalize_dll_name(dll_name);
    std::shared_lock lock(*dispatch_alias_mutex_);

    // Find DLL
    auto dll_it = dll_entries_.find(normalized);
    if (dll_it == dll_entries_.end()) {
        return VResult<GuestAddr>::err(Error::NotFound);
    }

    // Find function by name
    auto func_it = dll_it->second.by_name.find(std::string(func_name));
    if (func_it == dll_it->second.by_name.end()) {
        return VResult<GuestAddr>::err(Error::NotFound);
    }
    
    const auto& thunk = thunks_[func_it->second];
    if (thunk.internal && !include_internal)
        return VResult<GuestAddr>::err(Error::NotFound);
    return VResult<GuestAddr>::ok(thunk.thunk_address);
}

// =============================================================================
// Address Lookup - By Ordinal
// =============================================================================

VResult<GuestAddr> ThunkRegistry::get_thunk_address(
    std::string_view dll_name,
    u16 ordinal
) const {
    auto normalized = normalize_dll_name(dll_name);
    std::shared_lock lock(*dispatch_alias_mutex_);
    
    // Find DLL
    auto dll_it = dll_entries_.find(normalized);
    if (dll_it == dll_entries_.end()) {
        return VResult<GuestAddr>::err(Error::NotFound);
    }
    
    // Find function by ordinal
    auto ord_it = dll_it->second.by_ordinal.find(ordinal);
    if (ord_it == dll_it->second.by_ordinal.end()) {
        const auto alias = dll_it->second.ordinal_aliases.find(ordinal);
        if (alias != dll_it->second.ordinal_aliases.end()) {
            return VResult<GuestAddr>::ok(thunks_[alias->second].thunk_address);
        }
        return VResult<GuestAddr>::err(Error::NotFound);
    }
    
    return VResult<GuestAddr>::ok(thunks_[ord_it->second].thunk_address);
}

bool ThunkRegistry::register_ordinal_alias(
    std::string_view dll_name, u16 ordinal, GuestAddr thunk_address) {
    if (ordinal == 0) return false;
    const auto normalized = normalize_dll_name(dll_name);
    std::unique_lock lock(*dispatch_alias_mutex_);
    const auto dll = dll_entries_.find(normalized);
    const auto target = addr_to_thunk_.find(thunk_address);
    if (dll == dll_entries_.end() || target == addr_to_thunk_.end() ||
        thunks_[target->second].api_id.dll_id != dll->second.dll_id) {
        return false;
    }
    const auto registered = dll->second.by_ordinal.find(ordinal);
    if (registered != dll->second.by_ordinal.end()) {
        return registered->second == target->second;
    }
    const auto [alias, inserted] =
        dll->second.ordinal_aliases.emplace(ordinal, target->second);
    return inserted || alias->second == target->second;
}

bool ThunkRegistry::register_dispatch_alias(
    GuestAddr alias,
    GuestAddr thunk_address
) {
    if (is_in_thunk_region(alias)) return false;

    std::unique_lock lock(*dispatch_alias_mutex_);
    const auto target = addr_to_thunk_.find(thunk_address);
    if (target == addr_to_thunk_.end()) return false;

    const auto existing = addr_to_thunk_.find(alias);
    if (existing != addr_to_thunk_.end()) {
        return existing->second == target->second;
    }
    addr_to_thunk_[alias] = target->second;

    // Publish a new immutable alias snapshot for lock-free is_dispatch_address.
    auto prev = std::atomic_load_explicit(&dispatch_aliases_,
                                          std::memory_order_relaxed);
    auto next = std::make_shared<AliasMap>(prev ? *prev : AliasMap{});
    (*next)[alias] = target->second;
    std::atomic_store_explicit(
        &dispatch_aliases_,
        std::shared_ptr<const AliasMap>(std::move(next)),
        std::memory_order_release);
    dispatch_alias_gen_.fetch_add(1, std::memory_order_release);
    return true;
}

void ThunkRegistry::publish_native_services(
    std::vector<GuestAddr> thunks_by_number) {
    std::atomic_store_explicit(
        &native_services_,
        std::shared_ptr<const std::vector<GuestAddr>>(
            std::make_shared<const std::vector<GuestAddr>>(
                std::move(thunks_by_number))),
        std::memory_order_release);
}

GuestAddr ThunkRegistry::native_service_thunk(u32 number) const noexcept {
    const auto table =
        std::atomic_load_explicit(&native_services_, std::memory_order_acquire);
    return number < table->size() ? (*table)[number] : 0;
}

// =============================================================================
// Resolution
// =============================================================================

GuestAddr ThunkRegistry::alias_dispatch_gateway(
    GuestAddr alias) const noexcept {
    if (thunk_spacing_ == 0 || is_in_thunk_region(alias)) return 0;
    if (dispatch_alias_gen_.load(std::memory_order_acquire) == 0) return 0;
    std::shared_lock lock(*dispatch_alias_mutex_);
    const auto it = addr_to_thunk_.find(alias);
    if (it == addr_to_thunk_.end()) return 0;
    const GuestAddr gateway =
        thunk_base_ + it->second * thunk_spacing_ + STUB_SIZE;
    return is_in_thunk_region(gateway) ? gateway : 0;
}

VResult<ApiId> ThunkRegistry::resolve_address(GuestAddr thunk_address) const {
    std::shared_lock lock(*dispatch_alias_mutex_);
    usize idx = 0;
    if (slab_thunk_index(thunk_address, idx)) {
        return VResult<ApiId>::ok(thunks_[idx].api_id);
    }
    auto it = addr_to_thunk_.find(thunk_address);
    if (it == addr_to_thunk_.end()) {
        return VResult<ApiId>::err(Error::NotFound);
    }
    
    return VResult<ApiId>::ok(thunks_[it->second].api_id);
}

VResult<const ApiHandler*> ThunkRegistry::resolve_handler(GuestAddr thunk_address) const {
    if (g_count_resolve_handler_calls.load(std::memory_order_relaxed)) {
        ++t_resolve_handler_calls;
    }
    const u64 gen = dispatch_alias_gen_.load(std::memory_order_acquire);
    ThunkResolveCache& cached = resolve_entry(thunk_address);
    if (cached.owner_id == instance_id_ && cached.addr == thunk_address &&
        cached.gen == gen && cached.handler != nullptr) {
        return VResult<const ApiHandler*>::ok(cached.handler);
    }
    // Cache miss only (the per-thread cache above answers the hot path):
    // one shared hold covers the slab index and the deque read.
    std::shared_lock lock(*dispatch_alias_mutex_);
    usize idx = 0;
    if (!slab_thunk_index(thunk_address, idx)) {
        auto it = addr_to_thunk_.find(thunk_address);
        if (it == addr_to_thunk_.end()) {
            return VResult<const ApiHandler*>::err(Error::NotFound);
        }
        idx = it->second;
    }
    const ApiHandler* handler = &thunks_[idx].handler;
    cached.owner_id = instance_id_;
    cached.addr = thunk_address;
    cached.handler = handler;
    cached.stdcall_bytes = thunks_[idx].stdcall_bytes;
    cached.gen = gen;
    return VResult<const ApiHandler*>::ok(handler);
}

u16 ThunkRegistry::resolve_stdcall_bytes(GuestAddr thunk_address) const {
    const u64 gen = dispatch_alias_gen_.load(std::memory_order_acquire);
    ThunkResolveCache& cached = resolve_entry(thunk_address);
    if (cached.owner_id == instance_id_ && cached.addr == thunk_address &&
        cached.gen == gen) {
        return cached.stdcall_bytes;
    }
    std::shared_lock lock(*dispatch_alias_mutex_);
    usize idx = 0;
    if (!slab_thunk_index(thunk_address, idx)) {
        auto it = addr_to_thunk_.find(thunk_address);
        if (it == addr_to_thunk_.end()) return 0;
        idx = it->second;
    }
    cached.owner_id = instance_id_;
    cached.addr = thunk_address;
    cached.handler = &thunks_[idx].handler;
    cached.stdcall_bytes = thunks_[idx].stdcall_bytes;
    cached.gen = gen;
    return cached.stdcall_bytes;
}

VResult<const ApiHandler*> ThunkRegistry::get_handler(ApiId api_id) const {
    std::shared_lock lock(*dispatch_alias_mutex_);
    // Search through DLLs to find matching entry
    for (const auto& [dll_name, dll_entry] : dll_entries_) {
        if (dll_entry.dll_id == api_id.dll_id) {
            // Search ordinal map for matching function
            for (const auto& [ordinal, thunk_idx] : dll_entry.by_ordinal) {
                if (thunks_[thunk_idx].api_id == api_id) {
                    return VResult<const ApiHandler*>::ok(&thunks_[thunk_idx].handler);
                }
            }
        }
    }
    
    return VResult<const ApiHandler*>::err(Error::NotFound);
}

// =============================================================================
// Inspection
// =============================================================================

bool ThunkRegistry::has_dll(std::string_view dll_name) const {
    const std::string normalized = normalize_dll_name(dll_name);
    std::shared_lock lock(*dispatch_alias_mutex_);
    return dll_entries_.find(normalized) != dll_entries_.end();
}

std::vector<std::string> ThunkRegistry::dll_names() const {
    std::vector<std::string> names;
    {
        std::shared_lock lock(*dispatch_alias_mutex_);
        names.reserve(dll_entries_.size());
        for (const auto& [name, entry] : dll_entries_) {
            (void)entry;
            names.push_back(name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string ThunkRegistry::resolve_name(GuestAddr thunk_address) const {
    std::shared_lock lock(*dispatch_alias_mutex_);
    usize idx = 0;
    if (slab_thunk_index(thunk_address, idx)) {
        return thunks_[idx].debug_name;
    }
    auto it = addr_to_thunk_.find(thunk_address);
    if (it == addr_to_thunk_.end()) return {};
    return thunks_[it->second].debug_name;
}

std::vector<ThunkRegistry::ExportInfo>
ThunkRegistry::enumerate_exports() const {
    std::vector<ExportInfo> out;
    std::shared_lock lock(*dispatch_alias_mutex_);
    out.reserve(thunks_.size());
    for (const auto& [dll_name, dll_entry] : dll_entries_) {
        // Build thunk_idx -> ordinal lookup so we can attach an ordinal
        // when one was registered for a given name.
        std::unordered_map<usize, u16> idx_to_ord;
        idx_to_ord.reserve(dll_entry.by_ordinal.size());
        for (const auto& [ordinal, thunk_idx] : dll_entry.by_ordinal) {
            idx_to_ord[thunk_idx] = ordinal;
        }
        // Iterate every named export. Many of Vitreo's thunks are
        // registered by name only (ordinal=0) — those are still real
        // exports we want to enumerate.
        for (const auto& [func_name, thunk_idx] : dll_entry.by_name) {
            const auto& t = thunks_[thunk_idx];
            if (t.internal) continue;
            ExportInfo info{};
            info.dll_name      = dll_name;
            info.function_name = func_name;
            auto oit           = idx_to_ord.find(thunk_idx);
            info.ordinal       = (oit != idx_to_ord.end()) ? oit->second : 0;
            info.stdcall_bytes = t.stdcall_bytes;
            info.thunk_address = t.thunk_address;
            out.push_back(std::move(info));
        }
    }
    return out;
}

// =============================================================================
// Private Helpers
// =============================================================================

std::string ThunkRegistry::normalize_dll_name(std::string_view name) {
    std::string result(name);
    std::transform(result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

GuestAddr ThunkRegistry::allocate_thunk() {
    auto addr = next_thunk_;
    next_thunk_ += thunk_spacing_;
    return addr;
}

std::string ThunkRegistry::debug_name_for(GuestAddr addr) const {
    std::shared_lock lock(*dispatch_alias_mutex_);
    usize idx = 0;
    if (slab_thunk_index(addr, idx)) {
        return thunks_[idx].debug_name;
    }
    const auto it = addr_to_thunk_.find(addr);
    if (it != addr_to_thunk_.end() && it->second < thunks_.size()) {
        return thunks_[it->second].debug_name;
    }
    return {};
}

bool ThunkRegistry::slab_thunk_index(GuestAddr addr, usize& out_idx) const noexcept {
    if (thunk_spacing_ == 0 || !is_in_thunk_region(addr)) {
        return false;
    }
    const GuestAddr offset = addr - thunk_base_;
    const GuestAddr rem = offset % thunk_spacing_;
    // Stub bytes: rem < STUB_SIZE; exact dispatch gateway: rem == STUB_SIZE.
    if (rem > STUB_SIZE) {
        return false;
    }
    const usize idx = static_cast<usize>(offset / thunk_spacing_);
    if (idx >= thunks_.size()) {
        return false;
    }
    out_idx = idx;
    return true;
}

} // namespace vitreo::winapi

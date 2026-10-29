#pragma once

/// @file module.hpp
/// @brief Module management for Windows API emulation (WIN32-033)
///
/// Implements GetModuleHandle, GetProcAddress, LoadLibrary, FreeLibrary,
/// and GetModuleFileName for the emulated Win32 environment.

#include <vitreo/common.hpp>
#include "winapi/core/thunk_registry.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "capsule/vfs/vfs.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/loader/pe_loader.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "core/sync/host_rwlock.hpp"

namespace vitreo::winapi {

// Forward declaration
class Marshaler;

// =============================================================================
// Synthetic Base Addresses
// =============================================================================

/// Well-known base addresses for thunk-backed system DLL images.
///
/// HMODULE is the mapped image base on Windows, so these addresses are backed
/// by small, valid PE images even though their exports execute through Vitreo
/// thunks. Keep the arenas away from the guest stack/TEB and from the x86/x64
/// thunk instruction regions.
// 0x7C000000, not 0x6F000000. Two reasons, and they agree:
//
// Fidelity: Windows loads system DLLs near the top of the 2GB user window --
// kernel32 sat at ~0x7C800000 -- not in the middle of the heap. A guest that
// reasons about where system modules live sees a plausible layout.
//
// Fragmentation: at 0x6F000000 these arenas plus the thunk region split the
// guest's address space in half. A 32-bit guest that is not
// large-address-aware has a 2 GiB ceiling. A block in the middle splits the
// contiguous run it needs; a retry of that allocation never succeeds.
// Moving the block to the top merges those runs.
constexpr GuestAddr SYNTHETIC_MODULE_BASE_X86 = 0x7C000000;
// The x64 arena is NOT a constant: it is probed against the host address
// space at process construction, because the preferred window is occupied
// on x86_64 macOS. Read it from ModuleManager::synthetic_module_arena(),
// or vitreo::memory::x64_synthetic_arena_base() before one exists.
// See core/memory/x64_high_arena.hpp.
constexpr GuestAddr SYNTHETIC_MODULE_SLOT_SIZE = 0x10000;

constexpr GuestAddr KERNEL32_BASE = SYNTHETIC_MODULE_BASE_X86 + 0x00000;
constexpr GuestAddr NTDLL_BASE    = SYNTHETIC_MODULE_BASE_X86 + 0x10000;
constexpr GuestAddr USER32_BASE   = SYNTHETIC_MODULE_BASE_X86 + 0x20000;
constexpr GuestAddr WS2_32_BASE   = SYNTHETIC_MODULE_BASE_X86 + 0x30000;
constexpr GuestAddr SHELL32_BASE  = SYNTHETIC_MODULE_BASE_X86 + 0x40000;
constexpr GuestAddr GDI32_BASE    = SYNTHETIC_MODULE_BASE_X86 + 0x50000;
constexpr GuestAddr ADVAPI32_BASE = SYNTHETIC_MODULE_BASE_X86 + 0x60000;
constexpr GuestAddr OLE32_BASE    = SYNTHETIC_MODULE_BASE_X86 + 0x70000;
constexpr GuestAddr PSAPI_BASE    = SYNTHETIC_MODULE_BASE_X86 + 0x80000;
constexpr GuestAddr KERNELBASE_BASE = SYNTHETIC_MODULE_BASE_X86 + 0x90000;


// =============================================================================
// Module Info
// =============================================================================

/// Snapshot of a mapped PE image, used to restore a detached module in place.
///
/// The bytes are taken with a copy-on-write virtual-memory remap of the guest
/// image where the host allows it, so a 200 MB module costs no resident memory
/// until something writes to the snapshotted pages. `owns_vm` records which
/// release path the bytes need. A plain heap copy is the fallback whenever the
/// remap cannot be made (unaligned or non-contiguous host backing).
struct MappedImageSnapshot {
    const u8* data{nullptr};
    usize size{0};
    usize vm_size{0};   ///< page-rounded length of the remapped region
    bool owns_vm{false};

    MappedImageSnapshot() = default;
    MappedImageSnapshot(const MappedImageSnapshot&) = delete;
    MappedImageSnapshot& operator=(const MappedImageSnapshot&) = delete;
    ~MappedImageSnapshot();
};

/// Describes a loaded module (DLL or main executable)
struct ModuleInfo {
    /// Loader state machine. Modules transition:
    ///   NotLoaded → Mapping → Initializing → Loaded   (success)
    ///                       → Failed                  (DllMain returned FALSE
    ///                                                  or unrecoverable error)
    /// A reentrant load_library() while a module is in Mapping or
    /// Initializing must return the existing handle (no recursion into
    /// DllMain). This mirrors the Windows loader lock + InProgress flag.
    enum class State : u8 {
        NotLoaded   = 0,
        Mapping     = 1,
        Initializing = 2,
        Loaded      = 3,
        Failed      = 4,
    };

    GuestAddr   base_address;   ///< Base address in guest memory
    GuestAddr   image_end{0};    ///< First address past the loaded image (0 for synthetic modules)
    GuestAddr   entry_point{0};  ///< Entry point (DllMain) for PE-backed modules
    u32         bitness{0};      ///< 32 or 64 (0 for synthetic modules)
    u32         exception_rva{0}; ///< RVA of exception directory (.pdata)
    u32         exception_size{0}; ///< Size in bytes of exception directory
    u32         tls_dir_rva{0};   ///< RVA of TLS directory (0 if module has no TLS)
    u32         resource_rva{0};  ///< RVA of resource directory (0 if none)
    u32         resource_size{0}; ///< Size in bytes of resource directory
    std::string name;           ///< Short name (e.g. "kernel32.dll")
    std::string path;           ///< Full path (e.g. "C:\\Windows\\System32\\kernel32.dll")
    u32         ref_count;      ///< Reference count for LoadLibrary / FreeLibrary
    bool        pinned{false};  ///< GetModuleHandleEx(PIN): loaded until process exit
    State       state{State::Loaded};

    // For PE-backed modules, cache parsed exports so GetProcAddress works.
    std::unordered_map<std::string, GuestAddr> exports_by_name;
    std::unordered_map<u16, GuestAddr> exports_by_ordinal;

    /// Forwarded exports of this module: name/ordinal to the raw
    /// "TargetDll.ExportName" string. Resolved on lookup, because resolving
    /// one means loading the target module.
    std::unordered_map<std::string, std::string> forwarders_by_name;
    std::unordered_map<u16, std::string> forwarders_by_ordinal;

    /// Whether DllMain(DLL_PROCESS_ATTACH) has been called.
    /// False for modules loaded before m_guest_call is wired.
    bool dllmain_called{false};

    /// Suppress DLL_THREAD_ATTACH and DLL_THREAD_DETACH notifications.
    bool thread_notifications_disabled{false};

    /// True for a Vitreo thunk-backed Windows DLL image rather than a real
    /// guest PE loaded from disk.
    bool synthetic_image{false};

    /// Region descriptors for the mapped PE image, retained so the image can
    /// be unmapped and re-mapped when a LoadLibrary takes the reference count
    /// 0→1 (see reattach_detached_module). Empty for synthetic modules and for
    /// modules registered without going through the PE loader.
    std::vector<pe::MappedSection> mapped_sections;
    usize mapped_header_size{0};

    /// Win32 PAGE_* runs keyed by offset from base_address. Images report
    /// WRITECOPY forms until a page is privately written or VirtualProtect
    /// stores a new value (virtual_memory_contract_probe img_*). Empty when
    /// the module has no mapped_sections (synthetic / bare registration).
    std::map<u64, u32> protection_runs;

    /// AllocationProtect for MEM_IMAGE pages (PAGE_EXECUTE_WRITECOPY).
    static constexpr u32 kImageAllocationProtect = 0x80;

    /// Post-link, pre-DllMain snapshot of `[base_address, image_end)`.
    /// FreeLibrary unloads the module when the last reference drops (Microsoft
    /// Learn libloaderapi/FreeLibrary); the next LoadLibrary must therefore
    /// present on-disk section contents again. Restoring this snapshot in
    /// place keeps that contract without unmapping `.text` (which would
    /// discard JIT). Shared so ModuleInfo stays copyable.
    std::shared_ptr<const MappedImageSnapshot> mapped_image_template;

    /// Lowercased basenames (without .dll) of DLLs in this module's PE
    /// import table.  Populated lazily by call_deferred_dll_mains() so it
    /// can call dependency DllMains first (Windows loader order).
    std::vector<std::string> imports;
    bool imports_parsed{false};

    [[nodiscard]] bool is_pe_backed() const noexcept {
        return image_end != 0;
    }
};

// =============================================================================
// Module Manager
// =============================================================================

/// Manages loaded modules and provides Win32 module-management APIs
///
/// The ModuleManager keeps a registry of loaded modules (keyed by lowercase
/// name) and resolves GetProcAddress calls by delegating to ThunkRegistry.
class ModuleManager {
public:
    /// Synthetic base addresses exposed for tests / registration helpers
    static constexpr GuestAddr KERNEL32_BASE = vitreo::winapi::KERNEL32_BASE;
    static constexpr GuestAddr NTDLL_BASE    = vitreo::winapi::NTDLL_BASE;
    static constexpr GuestAddr USER32_BASE   = vitreo::winapi::USER32_BASE;

    /// Create a ModuleManager backed by the given ThunkRegistry
    /// @param registry ThunkRegistry used for GetProcAddress resolution
    explicit ModuleManager(ThunkRegistry& registry);

    /// Attach optional dynamic-loading support for real PE-backed DLLs.
    ///
    /// If attached, LoadLibraryExW/LoadLibraryW can map arbitrary DLL files
    /// from the capsule VFS into guest memory via the PE loader.
    void attach_dynamic_loader(GuestMemory& memory, capsule::Vfs& vfs,
                               bool is_64bit_process = false);

    /// Bind guest memory without a VFS (tests / image-protect wiring).
    void bind_guest_memory(GuestMemory& memory) noexcept { m_memory = &memory; }

    /// Synthetic system-module arena selected for this process. Capped x64
    /// linear address spaces relocate it below their user-mode ceiling.
    [[nodiscard]] GuestAddr synthetic_module_arena() const noexcept {
        return m_synthetic_module_arena;
    }

    /// Update process DLL-search state maintained by kernel32.
    void set_current_directory(std::string_view directory);

    /// Resolve a CRT *data* export (\_HUGE, \_environ, \_wcmdln, ...) that
    /// GetProcAddress asks for.
    ///
    /// These have no thunk, because they are variables rather than functions:
    /// they are bound directly into the import table at load time. A guest
    /// that reaches for one through GetProcAddress instead therefore got NULL,
    /// where Windows hands back the address of the variable.
    void set_crt_data_resolver(std::function<u64(std::string_view)> resolver);

    /// Supplies the process's current PATH environment variable (UTF-8), read
    /// at each implicit-path LoadLibrary: the last step of the standard DLL
    /// search order searches its directories.
    void set_path_variable_provider(std::function<std::string()> provider);

    /// Look up a CRT data export by name, or 0 when there is no resolver or
    /// no such variable.
    [[nodiscard]] u64 resolve_crt_data(std::string_view name) const;

    /// The registry backing GetProcAddress, for callers that need to consult
    /// its data-export table directly.
    [[nodiscard]] ThunkRegistry& registry() const noexcept { return registry_; }
    void set_dll_directory(std::optional<std::string> directory);

    /// AddDllDirectory / RemoveDllDirectory — opaque cookie table of absolute
    /// user DLL search directories (Microsoft Learn libloaderapi).
    [[nodiscard]] u64 add_dll_directory(std::string directory);
    [[nodiscard]] bool remove_dll_directory(u64 cookie);
    void set_default_dll_directories(u32 flags);
    [[nodiscard]] u32 default_dll_directories() const noexcept {
        return m_default_dll_directory_flags;
    }

    /// Configure a guest-call callback used to invoke PE entry points (DllMain)
    /// for dynamically loaded modules.
    ///
    /// Signature mirrors run_guest_callback: (func, args, arg_count, is_64bit) -> RAX/EAX.
    using GuestCall = std::function<u64(GuestAddr, const u64*, usize, bool)>;
    void set_guest_call(GuestCall call) { m_guest_call = std::move(call); }

    [[nodiscard]] u32 register_dll_notification(
        u32 flags, GuestAddr callback, GuestAddr context,
        GuestAddr* cookie);
    [[nodiscard]] u32 unregister_dll_notification(GuestAddr cookie);

    /// No copy
    ModuleManager(const ModuleManager&) = delete;
    ModuleManager& operator=(const ModuleManager&) = delete;

    /// Default move / dtor
    ModuleManager(ModuleManager&&) noexcept = delete;
    ModuleManager& operator=(ModuleManager&&) = delete;
    ~ModuleManager() = default;

    /// Call DllMain(DLL_PROCESS_ATTACH) for any PE-backed modules that
    /// were loaded before m_guest_call was wired up (e.g. companion DLLs
    /// loaded during static import resolution).
    void call_deferred_dll_mains();

    /// Deliver the Windows loader's per-thread initialization notifications
    /// on the newly-created guest thread, after its static TLS blocks exist
    /// and before its entry point runs. TLS callbacks precede DllMain for each
    /// module; DLL_THREAD_ATTACH return values are ignored by Windows.
    void notify_thread_attach();

    /// Deliver DLL_THREAD_DETACH on a cleanly exiting guest thread while its
    /// TEB, static TLS, and stack are still valid.
    void notify_thread_detach();

    /// Deliver DLL_PROCESS_DETACH (and TLS process-detach callbacks) for every
    /// loaded PE-backed DLL in reverse load order. Learn ExitProcess step 3;
    /// lpvReserved is non-NULL because the process is terminating
    /// (loader_contract_probe.expected "== exit").
    void notify_process_detach();

    /// Record a real PE DLL's loader insertion order without relying on the
    /// unordered module table's iteration order.
    void record_module_load_order(std::string_view key);

    // -------------------------------------------------------------------------
    // Module Registration
    // -------------------------------------------------------------------------
    // Module Registration
    // -------------------------------------------------------------------------

    /// Register a loaded module
    /// @param name  Short DLL name (e.g. "kernel32.dll")
    /// @param base  Guest base address
    /// @param path  Full file-system path
    void register_module(std::string_view name, GuestAddr base, std::string_view path);

    /// Make a host-implemented DLL that an image imports statically a loaded
    /// module (its thunk-backed synthetic image), so GetModuleHandle by base
    /// name finds it. No-op when already loaded; 0 when no host DLL exists.
    GuestAddr ensure_static_import_module(std::string_view dll_name);

    /// Rebuild every thunk-backed synthetic PE image from the current export
    /// registry. RuntimeContext establishes well-known HMODULE identities
    /// before registering their exports, so this runs once registration is
    /// complete and before guest execution starts.
    void materialize_synthetic_modules();

    /// The file a built-in system DLL has at @p path (a Windows path), for
    /// the VFS to serve where the capsule holds none: its synthetic PE image,
    /// the bytes a loaded module of that name has in memory. GetModuleFileNameW
    /// names C:\Windows\System32 for every built-in, and on Windows that
    /// path is a real file a process may open, map and parse. System32 holds this process's bitness, SysWOW64 the x86
    /// image -- a 32-bit process opening either sees the x86 file, as WOW64
    /// redirection gives it on Windows (builtin_dll_file_probe). Null for any
    /// other path or a DLL the thunk registry does not provide.
    [[nodiscard]] std::shared_ptr<const std::vector<u8>> builtin_dll_file(
        std::string_view path);
    /// The built-in DLL files of @p directory (System32 or SysWOW64) with
    /// their sizes, for directory enumeration; empty for any other directory.
    [[nodiscard]] std::vector<std::pair<std::string, u64>> builtin_dll_files_in(
        std::string_view directory) const;

    /// Set the main executable module
    /// @param base Guest base address of the main EXE
    /// @param path Full path to the executable
    void set_main_module(GuestAddr base, std::string_view path,
                         u32 exception_rva = 0, u32 exception_size = 0);

    /// The EXE's TLS directory (RVA, 0 = none) and bitness. Its TLS callbacks
    /// receive DLL_THREAD_ATTACH / DLL_THREAD_DETACH for every thread but the
    /// first (PE format, "TLS Callback Functions"), after the DLLs' own
    /// notifications in both directions (thread_lifecycle_probe "NOTIFY").
    void set_main_module_tls(u32 tls_dir_rva, u32 bitness) noexcept {
        main_module_tls_rva_ = tls_dir_rva;
        main_module_bitness_ = bitness;
    }

    /// Full Windows path of the process executable. The loader uses its
    /// directory as the first application-local DLL search location.
    [[nodiscard]] std::string_view main_module_path() const noexcept {
        return main_module_path_;
    }

    /// Register the main executable in the module table with parsed exports
    /// so GetModuleHandle / GetProcAddress can resolve symbols against it.
    /// Must be called after set_main_module(). `pe_bytes` is the raw file
    /// data used to parse the export directory.
    void register_main_module_exports(GuestAddr base,
                                       std::string_view path,
                                       GuestAddr image_end,
                                       u32 bitness,
                                       std::span<const u8> pe_bytes);

    /// Record the main thread's TEB so LoadLibrary'd DLLs can install their
    /// per-thread TLS slot (via TEB.ThreadLocalStoragePointer / fs:[0x2c]).
    /// Must be called after the main EXE's TLS array is set up (see run.cpp).
    void set_main_teb(GuestAddr teb_addr) { m_main_teb = teb_addr; }

    /// Record the process PEB used for the guest-visible loader module list.
    /// Must be called after setup_teb() has initialized PEB_LDR_DATA.
    void set_main_peb(GuestAddr peb_addr) { m_main_peb = peb_addr; }

    /// Register an existing guest thread's TEB for dynamic PE TLS updates.
    ///
    /// Windows makes a newly loaded DLL's static TLS available to threads
    /// that already exist. Re-registering the same TEB is harmless.
    ///
    /// @p tls_capacity is the number of entries in the vector the caller
    /// allocated at TEB.ThreadLocalStoragePointer; a vector smaller than the
    /// process's current implicit-TLS index range is grown here.
    void register_thread_teb(GuestAddr teb_addr, u32 bitness,
                             u32 tls_capacity = kMinTlsArrayCapacity);

    /// Smallest ThreadLocalStoragePointer vector any thread is created with.
    static constexpr u32 kMinTlsArrayCapacity = 64;

    /// Entries a new thread's ThreadLocalStoragePointer vector must hold to
    /// cover every implicit-TLS index handed out so far.
    [[nodiscard]] u32 tls_array_capacity() const {
        std::lock_guard<std::mutex> lock(m_tls_mutex);
        return m_tls_array_capacity;
    }

    /// Forget a TEB whose thread has exited and whose guest pages have been
    /// released. A stale entry would have a later static-TLS-bearing DLL write
    /// its slot into freed (possibly reused) guest memory.
    void unregister_thread_teb(GuestAddr teb_addr);

    // -------------------------------------------------------------------------
    // Win32-style Queries
    // -------------------------------------------------------------------------

    /// GetModuleHandle — return the base address for a loaded module.
    /// A null / empty name returns the main executable base.
    /// Lookup is case-insensitive; a trailing ".dll" is optional.
    /// @return Base address, or 0 if the module is not found
    [[nodiscard]] GuestAddr get_module_handle(std::string_view name) const;

    /// GetProcAddress by name
    /// @param module_base  Base address returned by GetModuleHandle / LoadLibrary
    /// @param func_name    Exported function name
    /// @return Thunk address, or 0 if not found
    [[nodiscard]] GuestAddr get_proc_address(GuestAddr module_base,
                                             std::string_view func_name) const;

    /// GetProcAddress by ordinal
    /// @param module_base  Base address of the module
    /// @param ordinal      Export ordinal
    /// @return Thunk address, or 0 if not found
    [[nodiscard]] GuestAddr get_proc_address_by_ordinal(GuestAddr module_base,
                                                        u16 ordinal) const;

    /// Resolve one forwarded export string ("TargetDll.ExportName" or
    /// "TargetDll.#123") to the target's address, loading the target module
    /// if it is not loaded yet, exactly as the Windows loader does when it
    /// finds an EAT entry inside the export directory.
    /// @param forwarder  Raw forwarder string from the export directory
    /// @param depth      Recursion guard for forwarder chains
    /// @return Address of the ultimate export, or 0 if it cannot be resolved
    [[nodiscard]] GuestAddr resolve_forwarded_export(std::string_view forwarder,
                                                     unsigned depth) const;

    /// Resolve a function via GetProcAddress, falling back to a synthetic
    /// "missing export" logging stub when the export is unknown. This keeps
    /// runtime function-pointer lookups symmetric with PE-loader IAT
    /// patching: the guest never sees a NULL FARPROC for a real DLL, so a
    /// later `call dword ptr [iat]` cannot crash with EIP=0; instead the
    /// stub logs "called missing export" and returns 0xFFFFFFFF.
    [[nodiscard]] GuestAddr get_or_stub_proc_address(GuestAddr module_base,
                                                     std::string_view func_name);
    [[nodiscard]] GuestAddr get_or_stub_proc_address_by_ordinal(GuestAddr module_base,
                                                                u16 ordinal);

    /// LoadLibrary / LoadLibraryEx — for known system DLLs return their existing
    /// handle; for unknown DLLs set ERROR_MOD_NOT_FOUND and return 0.
    /// @param flags LoadLibraryEx dwFlags. DONT_RESOLVE_DLL_REFERENCES (0x1)
    ///        skips import binding and DllMain/TLS attach. A non-DLL PE
    ///        (executable without IMAGE_FILE_DLL) is always loaded that way
    ///        (Learn LoadLibraryW).
    /// @return Module base address, or 0 on failure
    [[nodiscard]] GuestAddr load_library(std::string_view name, u32 flags = 0);

    /// Acquire the reference represented by a successful GetModuleHandleEx.
    ///
    /// A normal acquisition increments the module reference count. A pinned
    /// acquisition additionally prevents FreeLibrary from unloading the
    /// module for the remainder of the process.
    [[nodiscard]] bool acquire_module_reference(GuestAddr module_base, bool pin);

    /// FreeLibrary — decrement the reference count.
    /// System DLLs are never truly unloaded.
    /// @return true on success
    [[nodiscard]] bool free_library(GuestAddr module_base);

    /// Disable future thread attach/detach notifications for a loaded DLL.
    /// Fails for invalid handles and PE modules with static TLS.
    [[nodiscard]] bool disable_thread_library_calls(GuestAddr module_base);

    /// Re-run DLL_PROCESS_ATTACH for a PE module whose reference count had
    /// dropped to zero.
    ///
    /// free_library() sends DLL_PROCESS_DETACH but deliberately keeps the image
    /// mapped (see PE-008). A later LoadLibrary of that same module therefore
    /// hands back a base whose DllMain has already torn itself down — for an
    /// MSVC-CRT DLL that means a destroyed CRT heap and freed globals. Windows
    /// guarantees a load that takes the count 0→1 runs DLL_PROCESS_ATTACH, so
    /// re-attach here to keep that observable sequence intact.
    /// @return false if DllMain reported initialization failure.
    [[nodiscard]] bool reattach_detached_module(ModuleInfo& info);

    /// Restore a detached module to the section contents a fresh Windows load
    /// would provide at the same base, so the following DLL_PROCESS_ATTACH
    /// does not observe CRT teardown left by DLL_PROCESS_DETACH.
    ///
    /// Prefers `mapped_image_template` (in-place, JIT-preserving). Falls back
    /// to unmap + map from disk when no snapshot exists.
    /// @return false if the image could not be refreshed; a failed disk remap
    ///         leaves the module in State::Failed because the old image is gone.
    [[nodiscard]] bool remap_detached_image(ModuleInfo& info);

    /// Release every DLL TLS slot whose template lives in [base, end).
    ///
    /// A TLS template is part of the image, so unmapping an image invalidates
    /// its slots. Returning the indices to the pool is what keeps a repeated
    /// LoadLibrary/FreeLibrary cycle from exhausting the 64-slot budget.
    void release_dll_tls_slots_in_range(GuestAddr base, GuestAddr end);

    /// GetModuleFileName — copy the full path into @p buffer.
    /// @param module_base Base address (0 = main exe)
    /// @param buffer      Destination buffer
    /// @param size        Buffer size in chars
    /// @return Number of chars written (excluding NUL), or 0 on error
    [[nodiscard]] u32 get_module_filename(GuestAddr module_base,
                                          char* buffer, u32 size) const;

    /// The full path (UTF-8) of the module at @p module_base (0 = main exe),
    /// or nullptr when no module is loaded there.
    [[nodiscard]] const std::string* module_path(GuestAddr module_base) const;

    // -------------------------------------------------------------------------
    // Inspection
    // -------------------------------------------------------------------------

    /// Number of registered modules (including main exe if set)
    [[nodiscard]] usize module_count() const noexcept {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        return modules_by_name_.size();
    }

    /// Get the main executable base address
    [[nodiscard]] GuestAddr main_module_base() const noexcept { return main_module_base_; }
    [[nodiscard]] bool is_64bit_process() const noexcept { return m_is_64bit_process; }

    /// Get all module handles (base addresses), including main exe
    /// @return Vector of GuestAddr module handles
    [[nodiscard]] std::vector<GuestAddr> get_all_module_handles() const;

    /// Get module info by handle (base address)
    /// @param module_base Base address of the module (0 = main exe)
    /// @return Pointer to ModuleInfo, or nullptr if not found
    [[nodiscard]] const ModuleInfo* get_module_info_by_handle(GuestAddr module_base) const;

    /// Look up an x64 RUNTIME_FUNCTION entry in the exception directory.
    /// Returns the guest address of the matching RUNTIME_FUNCTION record, or 0.
    // ---------------------------------------------------------------
    // Dynamic function tables (RtlAddFunctionTable /
    // RtlInstallFunctionTableCallback)
    //
    // A JIT has no .pdata: it generates code at runtime and registers unwind
    // information for it. Without these, SEH cannot unwind out of a
    // JIT-compiled frame -- which is why coreclr cannot run.
    //
    // These live HERE rather than in SehManager because this is the store the
    // unwinder actually consults. RtlAddFunctionTable was already implemented
    // against SehManager's own table, which nothing on the unwind path ever
    // reads, so its entries were invisible.
    // ---------------------------------------------------------------

    /// Invoked to run a guest GET_RUNTIME_FUNCTION_CALLBACK. Supplied by the
    /// runtime, which is the only layer that can execute guest code.
    /// Returns the guest RUNTIME_FUNCTION* the callback produced, or 0.
    using RuntimeFunctionCallbackRunner =
        std::function<GuestAddr(GuestAddr callback, GuestAddr control_pc,
                                GuestAddr context)>;
    /// Process default, used for RtlLookupFunctionEntry called by the guest.
    void set_runtime_function_callback_runner(
        RuntimeFunctionCallbackRunner runner) {
        runtime_function_callback_runner_ = std::move(runner);
    }

    /// Overrides the runner on the calling thread while alive. Exception
    /// dispatch runs the callbacks on the dispatching thread's own CpuState;
    /// other threads keep using theirs.
    class ScopedThreadCallbackRunner {
    public:
        explicit ScopedThreadCallbackRunner(
            const RuntimeFunctionCallbackRunner* runner) noexcept;
        ~ScopedThreadCallbackRunner();
        ScopedThreadCallbackRunner(const ScopedThreadCallbackRunner&) = delete;
        ScopedThreadCallbackRunner& operator=(const ScopedThreadCallbackRunner&) = delete;
    private:
        const RuntimeFunctionCallbackRunner* previous_;
    };

    /// RtlAddFunctionTable: `count` RUNTIME_FUNCTIONs at `table`, relative to
    /// `base`. @return false when the range is degenerate.
    bool add_dynamic_function_table(GuestAddr table, u32 count, GuestAddr base);

    /// RtlInstallFunctionTableCallback: the guest resolves entries on demand
    /// for [base, base+length). `identifier` is the caller's handle: Windows
    /// requires its low two bits set to distinguish it from a table pointer.
    bool add_dynamic_function_table_callback(GuestAddr identifier,
                                             GuestAddr base, u32 length,
                                             GuestAddr callback,
                                             GuestAddr context);

    /// RtlAddGrowableFunctionTable: like add_dynamic_function_table, but the
    /// covered range is stated explicitly rather than inferred from the
    /// entries, and the entry count changes over the table's life. The
    /// returned handle is the identifier for grow/delete.
    bool add_growable_function_table(GuestAddr table, u32 count,
                                     GuestAddr range_base, GuestAddr range_end);

    /// RtlGrowFunctionTable: publish a larger entry count for a table already
    /// registered by add_growable_function_table. @return false if unknown.
    bool grow_dynamic_function_table(GuestAddr identifier, u32 new_count);

    /// RtlDeleteFunctionTable, by table pointer or by callback identifier.
    bool remove_dynamic_function_table(GuestAddr table_or_identifier);

    [[nodiscard]] GuestAddr lookup_runtime_function_entry(
        GuestAddr control_pc,
        GuestAddr* image_base_out = nullptr) const;

    /// Result of a module-by-address lookup.
    struct ModuleAtAddr {
        const ModuleInfo* info{nullptr};
        u32 rva{0};
    };

    /// Find the loaded module containing @p addr (half-open
    /// [base_address, image_end)). Synthetic modules use the mapped
    /// SYNTHETIC_MODULE_SLOT_SIZE range when image_end is 0.
    /// Returns {nullptr, 0} if no module covers @p addr.
    [[nodiscard]] ModuleAtAddr module_for_address(GuestAddr addr) const;

    /// Rebuild protection_runs from mapped_sections (headers PAGE_READONLY,
    /// writable sections PAGE_*WRITECOPY). Call after sections are assigned.
    void rebuild_image_protection_runs(ModuleInfo& info) const;

    /// Test/loader helper: attach section layout and rebuild protection runs.
    void set_mapped_image_layout(GuestAddr base,
                                 std::vector<pe::MappedSection> sections,
                                 usize header_size);

    /// VirtualQuery path for MEM_IMAGE Win32 protections (WRITECOPY forms).
    /// @return false when @p address is not in a module with protection_runs.
    [[nodiscard]] bool query_image_protection(
        GuestAddr address, GuestAddr& image_base, u64& image_size,
        u32* protection = nullptr, u64* protection_run_end = nullptr,
        u32* allocation_protection = nullptr) const;

    /// VirtualProtect on a loaded PE image: old protect is the Win32 image
    /// form (WRITECOPY until privately written); PAGE_READWRITE /
    /// PAGE_EXECUTE_READWRITE are stored as WRITECOPY forms.
    /// @return 0 = not an image with runs, 1 = success,
    ///         -1 = ERROR_INVALID_ADDRESS, -2 = ERROR_INVALID_PARAMETER.
    [[nodiscard]] int virtual_protect_image(GuestAddr address, usize size,
                                            u32 new_protect,
                                            u32* old_protect);

    /// Look up a module name from a base address (nullptr if none registered).
    /// Caller holds m_tables_mutex (shared is enough), or the loader lock on
    /// the load path where every structural writer is already serialized.
    [[nodiscard]] const std::string* name_for_base(GuestAddr base) const;

    /// Resource directory of the module loaded at `base`.
    /// Lets FindResource/LoadAccelerators/LoadString serve requests naming a
    /// DLL's HINSTANCE — localized resource DLLs are the common case.
    /// Accepts LoadLibraryEx AS_DATAFILE / AS_IMAGE_RESOURCE tagged handles
    /// (low 2 bits set); @p untagged_base_out receives the mapping base to use
    /// for RVA arithmetic when non-null.
    /// @return false when the base is unknown or the module has no resources
    [[nodiscard]] bool resource_directory_for_base(
        GuestAddr base, u32& rva_out, u32& size_out,
        GuestAddr* untagged_base_out = nullptr) const;

    /// True when @p handle is a LoadLibraryEx AS_DATAFILE / AS_IMAGE_RESOURCE
    /// tagged mapping (not an executable HMODULE).
    [[nodiscard]] bool is_data_file_handle(GuestAddr handle) const {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        return data_file_mappings_.find(handle) != data_file_mappings_.end();
    }

    /// LoadLibraryEx AS_DATAFILE / AS_DATAFILE_EXCLUSIVE / AS_IMAGE_RESOURCE.
    /// Maps the PE for resource access without DllMain or loader visibility
    /// (Learn LoadLibraryExW). Returns a tagged handle (base|1 or base|2), or
    /// the real HMODULE (refcount bumped) when the module is already loaded
    /// as an executable image.
    [[nodiscard]] GuestAddr load_as_data_file(std::string_view name, u32 flags);

    /// normalize_name(@p name) == @p key, without building the normalized
    /// string. The loaded-module scans run this once per module per lookup,
    /// and an import binding looks a module up once per imported FUNCTION:
    /// normalizing every loaded name on each of those was most of a large
    /// title's LoadLibrary time (~400 modules, thousands of imports each
    /// from its core module, which is keyed by path and so always missed
    /// the direct map lookup).
    [[nodiscard]] static bool normalized_name_equals(std::string_view name,
                                                     std::string_view key) noexcept;

private:
    friend class ModuleManagerTestAccessor;
    GuestAddr synthesize_thunk_module(const std::string& dll_base);
    /// The built-in module whose generated file @p win_path is, for this
    /// process's bitness: its thunk module (synthesize_thunk_module), not a
    /// PE mapped from those bytes. 0 when @p win_path is a real file, not a
    /// built-in's, or the other bitness's (SysWOW64 in a 64-bit process,
    /// which the PE path then refuses as ERROR_BAD_EXE_FORMAT, as Windows).
    GuestAddr load_builtin_dll_file(std::string_view win_path);
    /// Normalize a module name to lowercase, stripping a trailing ".dll" suffix.
    /// Executable suffixes remain part of the key so foo.exe and foo.dll cannot
    /// alias the same loaded-module entry.
    [[nodiscard]] static std::string normalize_name(std::string_view name);

    /// Map ModuleManager's normalized module key to a ThunkRegistry DLL name.
    /// normalize_name strips ".dll" but preserves ".drv"/".ocx"; registry keys
    /// keep the on-disk extension (e.g. "winspool.drv", "gdi32.dll").
    [[nodiscard]] static std::string thunk_registry_dll_key(
        std::string_view module_key) {
        if (module_key.find('.') != std::string_view::npos) {
            return std::string(module_key);
        }
        std::string key(module_key);
        key += ".dll";
        return key;
    }

    /// Build an ImportCallback for PeLoader::load that resolves a dependent
    /// DLL by attempting on-disk load from @p companion_dir (and from the main
    /// module's directory) before falling back to the thunk registry.
    /// Mirrors the behaviour used for the main executable's imports so a
    /// dynamically loaded module (LoadLibraryExW) can resolve bundled siblings
    /// from the same directory.
    ///
    /// @p dep_failed (optional) is set to true if a dependency DLL exists
    /// on disk but fails to initialize (DllMain returned FALSE). The caller
    /// uses this signal to fail the parent module load — mirroring Windows
    /// behavior where a failed dependency causes LoadLibrary to return NULL
    /// with ERROR_DLL_INIT_FAILED instead of silently stubbing imports.
    [[nodiscard]] pe::ImportCallback make_companion_resolver(
        std::string companion_dir,
        bool* dep_failed = nullptr);

    // -------------------------------------------------------------------------
    // Import graph (circular static imports)
    // -------------------------------------------------------------------------
    // PE/COFF "The .idata Section" and Learn "Dynamic-Link Library
    // Initialization": the loader maps every module of a load, snaps all
    // their import tables, and only then runs initializers, dependencies
    // first. A module is therefore visible (by name, with its exports) from
    // the moment it is mapped, so a dependency that imports back from a
    // module whose own imports are still being bound resolves against it
    // instead of loading it again. Cyclic imports among sibling DLLs rely on
    // that: each module is already visible while its own imports are still
    // being bound.

    /// Enter @p cache_key into the module table in State::Mapping, before its
    /// imports are bound, with the exports a cyclic importer needs.
    void register_mapping_module(const std::string& cache_key,
                                 const std::string& name,
                                 const std::string& path,
                                 const pe::ParsedPe& parsed,
                                 std::span<const u8> pe_data,
                                 GuestAddr image_base,
                                 usize image_size);

    /// Remove a module registered by register_mapping_module whose load did
    /// not complete (its mapping is owned by the caller).
    void discard_mapping_module(const std::string& cache_key);

    /// Base of a module that is mapped but not yet Loaded (Mapping or
    /// Initializing) and matches @p dll_name, or 0. A match in State::Mapping
    /// is an import cycle: the initializers of every module bound from here
    /// until that module finishes binding are deferred to it.
    GuestAddr in_progress_module_base(std::string_view dll_name);

    /// Pop @p cache_key off the binding stack after its imports were bound.
    /// Returns true when it sits inside an import cycle whose root is still
    /// binding, so its initializer must wait for that root.
    [[nodiscard]] bool finish_module_binding(const std::string& cache_key);

    /// Remember a module whose DLL_PROCESS_ATTACH waits for its cycle root.
    void defer_cycle_init(const std::string& cache_key, bool call_entry);

    /// Called by a cycle root once its own imports are bound: run the
    /// deferred initializers in binding-completion (dependency-first) order.
    /// On a DllMain FALSE, every module of the cycle — including the root
    /// @p root_key — is detached if attached, unloaded and removed, and false
    /// is returned so the root load fails with ERROR_DLL_INIT_FAILED.
    [[nodiscard]] bool run_deferred_cycle_inits(const std::string& root_key);

    /// Detach (if attached), unpublish, unmap and forget one module.
    void unload_failed_module(const std::string& cache_key, bool call_detach);

    struct DeferredCycleInit {
        std::string key;
        bool call_entry{true};
    };
    /// Keys of PE modules whose import tables are being bound, outermost
    /// first. Only touched under m_loader_mutex.
    std::vector<std::string> m_binding_stack;
    /// Lowest binding-stack index that a cycle reached back to, or npos.
    usize m_cycle_root_depth{static_cast<usize>(-1)};
    std::vector<DeferredCycleInit> m_deferred_cycle_inits;

    /// Snapshot `[base, image_end)` after IAT/CFG patching and before TLS
    /// callbacks / DllMain. Used by remap_detached_image on the next 0→1 load.
    void capture_mapped_image_template(ModuleInfo& info);

    /// Restore writable (and any mutated) pages from mapped_image_template.
    /// Identical pages, including RX `.text`, are left untouched so JIT stays
    /// valid. @return false if there is no usable snapshot.
    [[nodiscard]] bool restore_mapped_image_template(ModuleInfo& info);

    /// Project a host-side loaded module into the guest PEB loader list.
    /// Before the PEB exists, setup_teb() will publish the current module set.
    [[nodiscard]] bool publish_loader_entry(const ModuleInfo& info);
    void unpublish_loader_entry(const ModuleInfo& info);

    /// Fire the PE TLS callbacks for a freshly-loaded module with the given
    /// reason (typically DLL_PROCESS_ATTACH). Statically-linked CRTs register
    /// their C++ static-initializer dispatcher (e.g. __dyn_tls_init) here, so
    /// these MUST run before DllMain to keep static-storage objects valid.
    void fire_tls_callbacks(GuestAddr image_base,
                            u32 bitness,
                            GuestAddr tls_dir_va,
                            u64 reason,
                            const char* dbg_path);

    /// Allocate a unique TLS slot index for a freshly-loaded DLL that has a
    /// PE TLS directory, populate a distinct TLS data block for every
    /// registered guest thread, publish those blocks in each thread's TLS
    /// array, and write the slot index to *AddressOfIndex. This MUST run before
    /// fire_tls_callbacks/DllMain so MSVC's `_Init_thread_*` and other
    /// `__declspec(thread)` accessors see the correct per-DLL TLS layout.
    /// No-op for modules without a TLS directory or before the main TEB is
    /// registered.
    void setup_dll_tls(GuestAddr image_base,
                       u32 bitness,
                       GuestAddr tls_dir_va,
                       const char* dbg_path);

    /// Records a TLS slot allocated by setup_dll_tls so that worker-thread
    /// setup can replay the per-thread template-copy + array-slot-write
    /// dance for each DLL with TLS. Without this, DLLs whose TLS index is
    /// > 0 (everything past the main EXE) see a NULL TLS array slot on
    /// non-main threads and immediately AV inside `__declspec(thread)`
    /// accessors.
    struct DllTlsSlot {
        u32       slot_index;     ///< Index in TEB->TlsArray.
        GuestAddr template_start; ///< Guest VA of the raw template bytes.
        u64       template_size;  ///< Template size in bytes (may be 0).
        u32       zero_fill;      ///< Trailing zero-fill region size.
        u32       bitness;        ///< 32 or 64.
        std::string dbg_path;     ///< For trace output.
        // Blocks mapped directly by setup_dll_tls for threads that already
        // existed when the image was loaded.  They belong to this loaded
        // image instance and must be unmapped when its last reference drops.
        std::vector<GuestAddr> owned_blocks;
        usize allocation_size{};
    };

public:
    /// Snapshot of every DLL TLS slot allocated so far. Worker-thread
    /// startup iterates this list to populate its private TLS array.
    [[nodiscard]] std::vector<DllTlsSlot> dll_tls_slots() const {
        std::lock_guard<std::mutex> lock(m_tls_mutex);
        return m_dll_tls_slots;
    }

private:
    ThunkRegistry& registry_;

    // Optional dynamic loader state
    GuestMemory* m_memory{nullptr};
    capsule::Vfs* m_vfs{nullptr};
    std::unique_ptr<pe::PeLoader> m_pe_loader;
    bool m_is_64bit_process{false};
    GuestCall m_guest_call;

    struct DllNotificationRegistration {
        GuestAddr cookie;
        GuestAddr callback;
        GuestAddr context;
    };
    std::vector<DllNotificationRegistration> m_dll_notifications;
    GuestAddr m_next_dll_notification_cookie{0x10000};
    mutable std::mutex m_dll_notification_mutex;

    void notify_dll(u32 reason, std::string_view full_name,
                    std::string_view base_name, GuestAddr image_base,
                    usize image_size);
    [[nodiscard]] pe::PeLoader::PreImportCallback make_pre_import_callback(
        std::string full_name, std::string base_name);
    GuestAddr m_dynamic_base_start{0x18000000};
    GuestAddr m_next_dynamic_base{0x18000000};
    GuestAddr m_synthetic_module_arena{SYNTHETIC_MODULE_BASE_X86};
    std::string m_current_directory{"C:\\"};
    std::function<u64(std::string_view)> m_crt_data_resolver;
    std::function<std::string()> m_path_variable_provider;
    // nullopt restores the default search order; an empty string explicitly
    // removes the current directory from that order.
    std::optional<std::string> m_dll_directory;
    // AddDllDirectory cookies → absolute guest paths. Searched when
    // SetDefaultDllDirectories includes LOAD_LIBRARY_SEARCH_USER_DIRS, or when
    // that API has not yet restricted the search (flags == 0).
    std::unordered_map<u64, std::string> m_user_dll_directories;
    u64 m_next_dll_directory_cookie{1};
    u32 m_default_dll_directory_flags{0};

    /// Main thread TEB (recorded by run.cpp after TEB/TLS setup).
    /// Used by setup_dll_tls() to locate the TLS array via TEB+0x2c (32-bit)
    /// or TEB+0x58 (64-bit). Zero means TEB is not yet known and per-DLL
    /// TLS-index installation will be skipped.
    GuestAddr m_main_teb{0};

    /// Process PEB containing the guest-visible PEB_LDR_DATA projection.
    GuestAddr m_main_peb{0};

    struct ThreadTeb {
        GuestAddr teb_addr;
        u32 bitness;
        /// Entries in this thread's ThreadLocalStoragePointer vector.
        u32 tls_capacity{kMinTlsArrayCapacity};
        /// Vectors the loader allocated when growing this thread's array
        /// (base, bytes). The superseded vector is kept mapped: another
        /// thread's accessor may still be reading through it, and the
        /// original one shares an allocation with the EXE's TLS block.
        std::vector<std::pair<GuestAddr, usize>> grown_arrays;
    };
    std::vector<ThreadTeb> m_thread_tebs;
    mutable std::mutex m_tls_mutex;

    /// Replace @p thread's TLS vector with one of @p new_capacity entries,
    /// copying the populated prefix. m_tls_mutex must be held.
    bool grow_thread_tls_array(ThreadTeb& thread, u32 new_capacity);

    /// Next TLS slot index to hand out to a LoadLibrary'd DLL with a TLS
    /// directory. Slot 0 is reserved for the main EXE. Not capped: implicit
    /// TLS indices are unbounded on Windows (the 64/1088 limit is TlsAlloc's);
    /// every thread's vector grows to m_tls_array_capacity instead.
    u32 m_next_tls_index{1};
    /// Entries every registered thread's TLS vector is guaranteed to hold.
    /// Only raised, under m_tls_mutex.
    u32 m_tls_array_capacity{kMinTlsArrayCapacity};
    /// Slot indices freed by release_dll_tls_slots_in_range, reused before
    /// m_next_tls_index advances.
    std::vector<u32> m_free_tls_indices;

    /// Bump-pointer arena for per-thread TLS data blocks belonging to
    /// LoadLibrary'd DLLs. Sits below the dynamic-DLL base region to avoid
    /// collisions.
    GuestAddr m_next_tls_data{0x06000000};

    /// Tracks every DllTlsSlot ever allocated, in registration order. New
    /// worker threads replay this list to set up their own TLS arrays.
    std::vector<DllTlsSlot> m_dll_tls_slots;

    /// Reserve a guest-memory base for a dynamically loaded DLL of the
    /// given image size. Probes for a free gap (rather than blindly
    /// bumping by 16 MB) so over-large images don't collide with the
    /// next slot. Advances `m_next_dynamic_base` past the chosen range.
    [[nodiscard]] GuestAddr reserve_dynamic_base(usize image_size);

    /// name (lowercase, without .dll; .exe retained) → ModuleInfo
    std::unordered_map<std::string, ModuleInfo> modules_by_name_;
    /// Windows serializes module mapping and initialization under a
    /// process-wide recursive loader lock. Recursive acquisition permits
    /// dependency loads performed by the same loader thread.
    std::recursive_mutex m_loader_mutex;
    /// Table lock for modules_by_name_ / base_to_name_ / failed_modules_ /
    /// data_file_mappings_ / m_user_dll_directories (the loader's
    /// LdrpModuleDatatableLock). GetModuleHandle / GetProcAddress /
    /// RtlLookupFunctionEntry / thread attach run on every guest thread
    /// without the loader lock while LoadLibrary / FreeLibrary on another
    /// thread insert and erase; an unordered_map walked during a rehash is a
    /// host SIGSEGV (FileMappingManager, 2026-09-30). Readers take it shared
    /// for the lookup only; the loader-lock holder takes it exclusive for the
    /// structural operation only (insert / erase), never across DllMain, file
    /// I/O or a nested load. pthread rwlock: a thread that holds it shared
    /// must not take it again (a queued writer deadlocks it), so no reader
    /// section calls another ModuleManager entry point; name_for_base is the
    /// lock-free leaf. Lock order: m_loader_mutex -> m_tables_mutex.
    mutable HostRwLock m_tables_mutex;

    /// base address → normalized name (for reverse lookup)
    std::unordered_map<GuestAddr, std::string> base_to_name_;

    /// Short-name set of DLLs that previously failed to initialize
    /// (DllMain returned FALSE). Subsequent load_library() calls for any
    /// of these names fail immediately with ERROR_DLL_INIT_FAILED instead
    /// of remapping and re-running DllMain. This matches Windows behavior:
    /// once a DLL fails DLL_PROCESS_ATTACH the loader caches the failure.
    std::unordered_set<std::string> failed_modules_;

    /// LoadLibraryEx AS_DATAFILE / AS_IMAGE_RESOURCE mappings. Keyed by the
    /// tagged handle returned to the guest (base|1 or base|2). Not visible to
    /// GetModuleHandle / EnumProcessModules.
    struct DataFileMapping {
        GuestAddr base{0};       ///< Untagged mapping base
        GuestAddr image_end{0};
        u32 resource_rva{0};
        u32 resource_size{0};
        u32 header_size{0};
        std::vector<pe::MappedSection> sections;
        std::string path;
    };
    std::unordered_map<GuestAddr, DataFileMapping> data_file_mappings_;

    /// Main executable info
    /// One registered dynamic range. Exactly one of `table` / `callback` is
    /// set; a callback entry resolves per-PC through the guest.
    struct DynamicFunctionTable {
        GuestAddr identifier{0};   ///< table pointer, or the caller's handle
        GuestAddr base{0};
        u64       length{0};       ///< 0 for a table (bounded by its entries)
        GuestAddr table{0};
        u32       count{0};
        GuestAddr callback{0};
        GuestAddr context{0};
    };
    std::vector<DynamicFunctionTable> dynamic_tables_;
    mutable std::mutex dynamic_tables_mutex_;
    RuntimeFunctionCallbackRunner runtime_function_callback_runner_;

    /// Search the dynamic tables for a PC. Returns the guest
    /// RUNTIME_FUNCTION*, or 0.
    [[nodiscard]] GuestAddr lookup_dynamic_function_entry(
        GuestAddr control_pc, GuestAddr* image_base_out) const;

    GuestAddr   main_module_base_{0};
    std::string main_module_path_;
    u32         main_module_exception_rva_{0};
    u32         main_module_exception_size_{0};
    u32         main_module_tls_rva_{0};
    u32         main_module_bitness_{0};
};

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void impl_GetModuleHandleW(Marshaler& m, ModuleManager& mm);
void impl_GetModuleHandleA(Marshaler& m, ModuleManager& mm);
void impl_GetProcAddress(Marshaler& m, ModuleManager& mm);
void impl_LoadLibraryW(Marshaler& m, ModuleManager& mm);
void impl_LoadLibraryA(Marshaler& m, ModuleManager& mm);
void impl_FreeLibrary(Marshaler& m, ModuleManager& mm);
void impl_GetModuleFileNameW(Marshaler& m, ModuleManager& mm);
void impl_GetModuleFileNameA(Marshaler& m, ModuleManager& mm);

// =============================================================================
// Export Table Factory
// =============================================================================

/// Build DllExports containing the 8 module-management functions.
/// The returned ApiHandlers capture @p mm by reference.
[[nodiscard]] DllExports make_module_exports(ModuleManager& mm);

} // namespace vitreo::winapi

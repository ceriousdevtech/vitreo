#pragma once

/// @file handle.hpp
/// @brief Handle management for Windows API emulation
///
/// Provides a HandleTable class for tracking open handles (files, processes,
/// threads, events, etc.) and the CloseHandle implementation.

#include <vitreo/common.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vitreo::winapi {

// Forward declaration
class Marshaler;
struct DllExports;

// =============================================================================
// Handle Types
// =============================================================================

/// Types of handles that can be managed
enum class HandleType : u8 {
    Invalid = 0,
    File,
    Process,
    Thread,
    Event,
    Mutex,
    Semaphore,
    FileMapping,
    Pipe,
    Mailslot,
    ConsoleInput,
    ConsoleOutput,
    Registry,
    Heap,
    Timer,
    Module,
    Token,
    Job,
    Device,
    IoCompletionPort,
    NetworkNotification,
    /// An NT handle to a DXGI shared resource (IDXGIResource1::
    /// CreateSharedHandle); winapi/kernel32/shared_resource_handle.hpp.
    SharedResource,
};

/// Convert HandleType to string for debugging
[[nodiscard]] constexpr const char* handle_type_to_string(HandleType type) noexcept {
    switch (type) {
        case HandleType::Invalid:       return "Invalid";
        case HandleType::File:          return "File";
        case HandleType::Process:       return "Process";
        case HandleType::Thread:        return "Thread";
        case HandleType::Event:         return "Event";
        case HandleType::Mutex:         return "Mutex";
        case HandleType::Semaphore:     return "Semaphore";
        case HandleType::FileMapping:   return "FileMapping";
        case HandleType::Pipe:          return "Pipe";
        case HandleType::Mailslot:      return "Mailslot";
        case HandleType::ConsoleInput:  return "ConsoleInput";
        case HandleType::ConsoleOutput: return "ConsoleOutput";
        case HandleType::Registry:      return "Registry";
        case HandleType::Heap:          return "Heap";
        case HandleType::Timer:         return "Timer";
        case HandleType::Module:        return "Module";
        case HandleType::Token:         return "Token";
        case HandleType::Job:           return "Job";
        case HandleType::Device:        return "Device";
        case HandleType::IoCompletionPort: return "IoCompletionPort";
        case HandleType::NetworkNotification: return "NetworkNotification";
        case HandleType::SharedResource: return "SharedResource";
    }
    return "Unknown";
}

/// Notification device files supply their own completion wait. Ownership stays
/// with the registering provider; the handle table only borrows this object.
class OverlappedNotificationFile {
public:
    virtual ~OverlappedNotificationFile() = default;
    virtual void wait_for_completion(GuestAddr overlapped) = 0;
};

// =============================================================================
// Handle Type Alias
// =============================================================================

/// Windows HANDLE value (opaque 32-bit value for 32-bit guests)
using Handle = u32;

/// Invalid handle constant (matches Windows INVALID_HANDLE_VALUE)
constexpr Handle INVALID_HANDLE = static_cast<Handle>(-1);

/// Null handle constant
constexpr Handle NULL_HANDLE = 0;

// =============================================================================
// Handle Entry
// =============================================================================

/// Internal entry for a handle in the table
struct HandleEntry {
    HandleType type;          ///< Type of object this handle refers to
    void* object;             ///< Pointer to host-side object (type-specific)
    u32 flags;                ///< Handle flags (e.g., HANDLE_FLAG_INHERIT)
    u32 access;               ///< Granted access mask for APIs that enforce it
    
    /// Check if entry is valid
    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return type != HandleType::Invalid;
    }
};

// =============================================================================
// Handle Flags (matches Windows)
// =============================================================================

/// Handle flags for SetHandleInformation/GetHandleInformation
namespace HandleFlags {
    constexpr u32 Inherit = 0x00000001;        ///< HANDLE_FLAG_INHERIT
    constexpr u32 ProtectFromClose = 0x00000002; ///< HANDLE_FLAG_PROTECT_FROM_CLOSE
}

namespace HandleAccess {
    constexpr u32 All = 0xFFFFFFFFu;
}

// =============================================================================
// Close Callback Type
// =============================================================================

/// Callback function type for handle cleanup
/// @param handle The HANDLE value being closed
/// @param type The type of handle being closed
/// @param object The object pointer associated with the handle
/// @return true if cleanup succeeded, false otherwise
using HandleCloseCallback = std::function<bool(Handle handle, HandleType type, void* object)>;

// =============================================================================
// Handle Table
// =============================================================================

/// Manages Win32 handles for a guest process
///
/// The HandleTable provides:
/// - Allocation of unique handle values
/// - Tracking of handle type and associated object
/// - Validation and lookup of handles
/// - Thread-safe operations
///
/// Handle values start at 4 and increment by 4 (Windows convention).
class HandleTable {
public:
    /// Create an empty handle table
    HandleTable();

    /// Reserve inherited values before child managers allocate local handles.
    /// Reservations are not valid handles; allocate_at publishes the object.
    explicit HandleTable(std::span<const Handle> inherited_values);
    
    /// Create with a custom close callback
    /// @param close_callback Function to call when handles are closed
    explicit HandleTable(HandleCloseCallback close_callback);
    
    /// No copy (contains mutex)
    HandleTable(const HandleTable&) = delete;
    HandleTable& operator=(const HandleTable&) = delete;
    
    /// No move (contains mutex)
    HandleTable(HandleTable&&) = delete;
    HandleTable& operator=(HandleTable&&) = delete;
    
    /// Destructor - closes all remaining handles
    ~HandleTable();
    
    // -------------------------------------------------------------------------
    // Handle Allocation
    // -------------------------------------------------------------------------
    
    /// Allocate a new handle
    /// @param type Type of object
    /// @param object Pointer to the object (may be nullptr)
    /// @param flags Initial handle flags (default 0)
    /// @return Newly allocated handle
    [[nodiscard]] Handle allocate(HandleType type, void* object, u32 flags = 0,
                                  u32 access = HandleAccess::All);

    /// Allocate a new handle whose value is at least @p floor (values below
    /// it that come up in sequence are skipped, not handed out).
    [[nodiscard]] Handle allocate_at_least(Handle floor, HandleType type,
                                           void* object, u32 flags = 0,
                                           u32 access = HandleAccess::All);

    /// Import a handle with a prescribed value during process inheritance.
    [[nodiscard]] bool allocate_at(Handle handle, HandleType type, void* object,
                                   u32 flags = 0,
                                   u32 access = HandleAccess::All);
    
    // -------------------------------------------------------------------------
    // Handle Operations
    // -------------------------------------------------------------------------
    
    /// Close a handle and release resources
    /// @param h Handle to close
    /// @return true if handle was valid and closed, false otherwise
    [[nodiscard]] bool close(Handle h);
    
    /// Get the object pointer for a handle
    /// @param h Handle to look up
    /// @param expected_type Expected type (HandleType::Invalid = any type)
    /// @return Pointer to object or nullptr if invalid/wrong type
    [[nodiscard]] void* get(Handle h, HandleType expected_type = HandleType::Invalid) const;
    
    /// Get typed object pointer for a handle
    /// @tparam T Expected object type
    /// @param h Handle to look up
    /// @param expected_type Expected handle type
    /// @return Pointer to object or nullptr if invalid/wrong type
    template<typename T>
    [[nodiscard]] T* get_as(Handle h, HandleType expected_type) const {
        return static_cast<T*>(get(h, expected_type));
    }

    /// How many open handles still refer to @p object with @p type.
    /// Used to destroy terminated thread objects once their last handle closes
    /// (OpenThread then fails with ERROR_INVALID_PARAMETER, as on Windows).
    [[nodiscard]] usize count_object(void* object, HandleType type) const;

    /// Copy a shared object reference while the handle-table lock is held.
    ///
    /// The handle entry must contain a pointer to std::shared_ptr<T>. This is
    /// useful for operations which continue on a worker thread after the
    /// caller returns: closing the handle can delete the stored wrapper, but
    /// the copied object remains alive until the operation finishes.
    template<typename T>
    [[nodiscard]] std::shared_ptr<T> get_shared(
        Handle h, HandleType expected_type) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = handles_.find(h);
        if (it == handles_.end() ||
            (expected_type != HandleType::Invalid &&
             it->second.type != expected_type)) {
            return {};
        }
        auto* holder =
            static_cast<std::shared_ptr<T>*>(it->second.object);
        return holder ? *holder : std::shared_ptr<T>{};
    }
    
    /// Check if a handle is valid
    /// @param h Handle to check
    /// @return true if handle exists in table
    [[nodiscard]] bool is_valid(Handle h) const;
    
    /// Get the type of a handle
    /// @param h Handle to check
    /// @return Handle type or HandleType::Invalid if not found
    [[nodiscard]] HandleType get_type(Handle h) const;
    
    /// Get handle entry (for advanced use)
    /// @param h Handle to look up
    /// @return Pointer to entry or nullptr if invalid
    [[nodiscard]] const HandleEntry* get_entry(Handle h) const;
    
    // -------------------------------------------------------------------------
    // Handle Flags
    // -------------------------------------------------------------------------
    
    /// Get handle flags
    /// @param h Handle to query
    /// @return Flags or 0 if handle invalid
    [[nodiscard]] u32 get_flags(Handle h) const;

    /// Get the access mask granted to a handle.
    /// @return Access mask, or 0 if the handle is invalid.
    [[nodiscard]] u32 get_access(Handle h) const;
    
    /// Set handle flags
    /// @param h Handle to modify
    /// @param mask Flags to modify
    /// @param flags New flag values
    /// @return true if handle was valid and flags were set
    [[nodiscard]] bool set_flags(Handle h, u32 mask, u32 flags);
    
    // -------------------------------------------------------------------------
    // Inspection
    // -------------------------------------------------------------------------
    
    /// Get number of open handles
    [[nodiscard]] usize count() const;

    /// Snapshot every open handle value (unordered).
    /// Used by NtQueryInformationProcess(ProcessHandleTable).
    [[nodiscard]] std::vector<Handle> snapshot_handles() const;

    /// Snapshot handles containing all requested flags.
    [[nodiscard]] std::vector<Handle> handles_with_flags(u32 flags) const;
    
    /// Check if table is empty
    [[nodiscard]] bool empty() const;
    
    /// Set close callback
    void set_close_callback(HandleCloseCallback callback);
    
private:
    /// Allocate next handle value, at least @p floor
    [[nodiscard]] Handle allocate_handle_value(Handle floor = 0);
    
    mutable std::mutex mutex_;                        ///< Protects all members
    std::unordered_map<Handle, HandleEntry> handles_; ///< Handle -> entry map
    std::unordered_set<Handle> inherited_values_;     ///< Not yet imported
    Handle next_handle_;                              ///< Next handle value
    HandleCloseCallback close_callback_;              ///< Cleanup callback
    
    /// Starting handle value (Windows uses small multiples of 4)
    static constexpr Handle FIRST_HANDLE = 4;
    
    /// Handle increment (Windows allocates handles as multiples of 4)
    static constexpr Handle HANDLE_INCREMENT = 4;
};

// =============================================================================
// CloseHandle Implementation
// =============================================================================

/// Implementation of CloseHandle Win32 API
///
/// BOOL WINAPI CloseHandle(HANDLE hObject);
///
/// Closes an open object handle. The object handle can identify:
/// - File
/// - File mapping
/// - Process
/// - Thread
/// - Event
/// - Mutex
/// - Semaphore
/// - Named pipe
/// - Timer
/// - etc.
///
/// @param m Marshaler for argument/return value handling
/// @param table HandleTable to use for handle management
///
/// Return value (via marshaler):
/// - TRUE (1) if the function succeeds
/// - FALSE (0) if the function fails (GetLastError will have error code)
void impl_CloseHandle(Marshaler& m, HandleTable& table);

// =============================================================================
// Export Registration
// =============================================================================

/// Build the DllExports table for handle management APIs
/// @param ht HandleTable instance to bind into thunk closures
/// @return DllExports with CloseHandle entry
DllExports make_handle_exports(HandleTable& ht);

} // namespace vitreo::winapi

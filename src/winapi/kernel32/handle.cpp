/// @file handle.cpp
/// @brief Handle management implementation

#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/synthetic_handle_registry.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "ipc/process_handle.hpp"

#include <algorithm>
#include <cstdlib>
#include <unistd.h>

namespace vitreo::winapi {

namespace {

bool handle_trace_enabled() {
    static const bool enabled = std::getenv("VITREO_HANDLE_TRACE") != nullptr;
    return enabled;
}

void trace_handle(const char* operation, Handle handle, HandleType type, u32 flags) {
    if (!handle_trace_enabled()) return;
    std::fprintf(stderr, "[handle-trace pid=%d] %s h=0x%x type=%u flags=0x%x\n",
                 static_cast<int>(::getpid()), operation,
                 static_cast<unsigned>(handle), static_cast<unsigned>(type), flags);
}

} // namespace

// =============================================================================
// HandleTable Implementation
// =============================================================================

HandleTable::HandleTable()
    : next_handle_(FIRST_HANDLE)
    , close_callback_(nullptr)
{
}

HandleTable::HandleTable(HandleCloseCallback close_callback)
    : next_handle_(FIRST_HANDLE)
    , close_callback_(close_callback)
{
}

HandleTable::HandleTable(std::span<const Handle> inherited_values)
    : inherited_values_(inherited_values.begin(), inherited_values.end())
    , next_handle_(FIRST_HANDLE)
    , close_callback_(nullptr)
{
}

HandleTable::~HandleTable() {
    // Close all remaining handles
    // Lock not needed in destructor (single-threaded access assumed)
    for (auto& [handle, entry] : handles_) {
        if (entry.is_valid() && close_callback_) {
            close_callback_(handle, entry.type, entry.object);
        }
    }
    handles_.clear();
}

Handle HandleTable::allocate(HandleType type, void* object, u32 flags,
                             u32 access) {
    return allocate_at_least(0, type, object, flags, access);
}

Handle HandleTable::allocate_at_least(Handle floor, HandleType type,
                                      void* object, u32 flags, u32 access) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Handle h = allocate_handle_value(floor);
    if (h == NULL_HANDLE) return NULL_HANDLE;
    
    HandleEntry entry{};
    entry.type = type;
    entry.object = object;
    entry.flags = flags;
    entry.access = access;
    
    handles_.emplace(h, entry);

    trace_handle("allocate", h, type, flags);
    
    return h;
}

bool HandleTable::allocate_at(Handle handle, HandleType type, void* object,
                              u32 flags, u32 access) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle == NULL_HANDLE || handle == INVALID_HANDLE ||
        handles_.contains(handle)) {
        return false;
    }
    handles_.emplace(handle, HandleEntry{type, object, flags, access});
    inherited_values_.erase(handle);
    trace_handle("import", handle, type, flags);
    if (handle >= next_handle_) {
        next_handle_ = handle + HANDLE_INCREMENT;
        if (next_handle_ <= handle) next_handle_ = FIRST_HANDLE;
    }
    return true;
}

bool HandleTable::close(Handle h) {
    HandleEntry entry{};
    HandleCloseCallback close_callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = handles_.find(h);
        if (it == handles_.end()) {
            trace_handle("close-missing", h, HandleType::Invalid, 0);
            return false;
        }

        entry = it->second;

        // Check if protected from close
        if (entry.flags & HandleFlags::ProtectFromClose) {
            trace_handle("close-protected", h, entry.type, entry.flags);
            return false;
        }

        trace_handle("close", h, entry.type, entry.flags);
        handles_.erase(it);
        close_callback = close_callback_;
    }

    // Cleanup may acquire a subsystem lock. Invoke it after releasing the
    // table mutex so allocation paths that take the subsystem lock first
    // cannot deadlock with CloseHandle.
    return !close_callback || !entry.is_valid()
        ? true
        : close_callback(h, entry.type, entry.object);
}

void* HandleTable::get(Handle h, HandleType expected_type) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = handles_.find(h);
    if (it == handles_.end()) {
        return nullptr;
    }
    
    const HandleEntry& entry = it->second;
    
    // Check type if specified
    if (expected_type != HandleType::Invalid && entry.type != expected_type) {
        return nullptr;
    }
    
    return entry.object;
}

usize HandleTable::count_object(void* object, HandleType type) const {
    if (object == nullptr) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    usize count = 0;
    for (const auto& [h, entry] : handles_) {
        (void)h;
        if (entry.type == type && entry.object == object) {
            ++count;
        }
    }
    return count;
}

bool HandleTable::is_valid(Handle h) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handles_.find(h) != handles_.end();
}

HandleType HandleTable::get_type(Handle h) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = handles_.find(h);
    if (it == handles_.end()) {
        return HandleType::Invalid;
    }
    
    return it->second.type;
}

const HandleEntry* HandleTable::get_entry(Handle h) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = handles_.find(h);
    if (it == handles_.end()) {
        return nullptr;
    }
    
    return &it->second;
}

u32 HandleTable::get_flags(Handle h) const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = handles_.find(h);
    if (it == handles_.end()) {
        return 0;
    }
    
    return it->second.flags;
}

u32 HandleTable::get_access(Handle h) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = handles_.find(h);
    if (it == handles_.end()) {
        return 0;
    }

    return it->second.access;
}

bool HandleTable::set_flags(Handle h, u32 mask, u32 flags) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto it = handles_.find(h);
    if (it == handles_.end()) {
        return false;
    }
    
    HandleEntry& entry = it->second;
    entry.flags = (entry.flags & ~mask) | (flags & mask);
    
    return true;
}

usize HandleTable::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handles_.size();
}

std::vector<Handle> HandleTable::snapshot_handles() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Handle> result;
    result.reserve(handles_.size());
    for (const auto& [handle, entry] : handles_) {
        (void)entry;
        result.push_back(handle);
    }
    return result;
}

std::vector<Handle> HandleTable::handles_with_flags(u32 flags) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Handle> result;
    result.reserve(handles_.size());
    for (const auto& [handle, entry] : handles_) {
        if ((entry.flags & flags) == flags) result.push_back(handle);
    }
    return result;
}

bool HandleTable::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handles_.empty();
}

void HandleTable::set_close_callback(HandleCloseCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    close_callback_ = callback;
}

Handle HandleTable::allocate_handle_value(Handle floor) {
    // Note: mutex_ must be held by caller
    const Handle first = next_handle_;
    do {
        const Handle h = next_handle_;
        next_handle_ += HANDLE_INCREMENT;
        if (next_handle_ <= h) next_handle_ = FIRST_HANDLE;
        if (h >= floor && h != NULL_HANDLE && h != INVALID_HANDLE &&
            !handles_.contains(h) && !inherited_values_.contains(h)) return h;
    } while (next_handle_ != first);
    return NULL_HANDLE;
}

// =============================================================================
// CloseHandle Implementation
// =============================================================================

void impl_CloseHandle(Marshaler& m, HandleTable& table) {
    // BOOL WINAPI CloseHandle(HANDLE hObject);
    auto handle = static_cast<Handle>(m.arg(0));
    VITREO_TRACE("[handle] CloseHandle(0x%x)\n", static_cast<unsigned>(handle));

    if (handle == NULL_HANDLE) {
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    // NT pseudo handles: -1 GetCurrentProcess, -2 GetCurrentThread,
    // -3 GetCurrentSession, -4/-5/-6 token pseudo handles. On Windows,
    // CloseHandle on a pseudo handle is a successful no-op (returns TRUE).
    // Note INVALID_HANDLE_VALUE == -1 == the process pseudo handle.
    if (handle >= 0xFFFFFFFAu) {
        m.set_return(1);
        return;
    }

    // Synthetic handle namespaces issued outside the HandleTable
    // (CreateToolhelp32Snapshot 0xFACE*, legacy semaphore 0xCE*, legacy IOCP
    // 0xCC*). Only a handle that was actually issued may close successfully;
    // a never-issued value fails ERROR_INVALID_HANDLE (Windows
    // synthetic_handle_probe).
    if (synthetic_handles::is_synthetic_range(handle)) {
        u32 flags = 0;
        if (synthetic_handles::get_flags(handle, flags) &&
            (flags & HandleFlags::ProtectFromClose)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        if (!synthetic_handles::close(handle)) {
            set_last_error(error_codes::ERROR_INVALID_HANDLE);
            m.set_return(0);
            return;
        }
        m.set_return(1);
        return;
    }

    // A process-handle close is observable to peers that still hold a
    // duplicate. Log it under pipe diag so bring-up captures stay one env var.
    if (table.get_type(handle) == HandleType::Process &&
        std::getenv("VITREO_PIPE_DIAG") != nullptr) {
        auto* process = static_cast<ipc::ProcessObject*>(
            table.get(handle, HandleType::Process));
        std::fprintf(
            stderr,
            "[handle pid=%d] CloseHandle Process h=0x%x pid=%d access=0x%x\n",
            static_cast<int>(::getpid()), static_cast<unsigned>(handle),
            process ? static_cast<int>(process->pid) : -1,
            static_cast<unsigned>(table.get_access(handle)));
        std::fflush(stderr);
    }

    // Pipe CloseHandle sites matter when one end closes while the peer still
    // holds the other. Log the guest return address and EBP walk under
    // VITREO_PIPE_DIAG (Microsoft Learn: CloseHandle destroys the object only
    // after the last handle closes).
    if (table.get_type(handle) == HandleType::Pipe &&
        std::getenv("VITREO_PIPE_DIAG") != nullptr) {
        u64 caller_ret = 0;
        u64 ebp_rets[5]{};
        if (auto* cpu = m.cpu()) {
            const bool is64 = cpu->is_64bit();
            const u64 sp = cpu->sp();
            if (is64) {
                caller_ret = m.memory().read_u64(sp).value_or(0);
                u64 frame = cpu->gpr.rbp;
                for (usize i = 0; i < 5 && frame >= 0x10000; ++i) {
                    ebp_rets[i] =
                        m.memory().read_u64(frame + 8).value_or(0);
                    const u64 next = m.memory().read_u64(frame).value_or(0);
                    if (next <= frame || next - frame > 16 * 1024 * 1024) {
                        break;
                    }
                    frame = next;
                }
            } else {
                caller_ret = m.memory().read_u32(static_cast<u32>(sp)).value_or(0);
                u32 frame = static_cast<u32>(cpu->gpr.rbp);
                for (usize i = 0; i < 5 && frame >= 0x10000u; ++i) {
                    ebp_rets[i] =
                        m.memory().read_u32(frame + 4).value_or(0);
                    const u32 next = m.memory().read_u32(frame).value_or(0);
                    if (next <= frame || next - frame > 16 * 1024 * 1024) {
                        break;
                    }
                    frame = next;
                }
            }
        }
        std::fprintf(
            stderr,
            "[handle pid=%d] CloseHandle pipe h=0x%x "
            "caller_ret=0x%llx ebp_ret=[0x%llx,0x%llx,0x%llx,0x%llx,0x%llx]\n",
            static_cast<int>(::getpid()), static_cast<unsigned>(handle),
            static_cast<unsigned long long>(caller_ret),
            static_cast<unsigned long long>(ebp_rets[0]),
            static_cast<unsigned long long>(ebp_rets[1]),
            static_cast<unsigned long long>(ebp_rets[2]),
            static_cast<unsigned long long>(ebp_rets[3]),
            static_cast<unsigned long long>(ebp_rets[4]));
        std::fflush(stderr);
    }

    // Attempt to close the handle
    bool success = table.close(handle);
    if (!success) {
        // Failed closes are rare and usually a handle-lifetime bug. Callers
        // treat CloseHandle FALSE as fatal. Always log the value so the
        // owning subsystem is identifiable.
        const cpu::CpuState* cpu = m.cpu();
        const u64 caller_ret = cpu == nullptr
            ? 0
            : (cpu->is_64bit()
                ? m.memory().read_u64(cpu->sp()).value_or(0)
                : m.memory().read_u32(cpu->sp()).value_or(0));
        std::fprintf(stderr,
                     "[handle] CloseHandle(0x%x) -> FALSE (not in table) "
                     "rip=0x%llx caller_ret=0x%llx\n",
                     handle,
                     static_cast<unsigned long long>(cpu != nullptr
                         ? cpu->rip : 0),
                     static_cast<unsigned long long>(caller_ret));
        // Diagnostic: the immediate caller is often a CRT wrapper, so scan
        // the guest stack for plausible return addresses to name the
        // subsystem that owns the handle value.
        if (cpu != nullptr && cpu->is_64bit()) {
            auto& memory = m.memory();
            const u64 rsp = cpu->sp();
            int found = 0;
            for (u64 off = 0; off < 0x400 && found < 12; off += 8) {
                auto v = memory.read_u64(rsp + off);
                if (!v.is_ok()) break;
                const u64 a = v.value();
                if (a < 0x10000 || a >= 0x0000800000000000ull) continue;
                auto b5 = memory.read_u8(a - 5);
                auto b6 = memory.read_u8(a - 6);
                auto b2 = memory.read_u8(a - 2);
                const bool is_ret = (b5.is_ok() && b5.value() == 0xE8) ||
                                    (b6.is_ok() && b6.value() == 0xFF) ||
                                    (b2.is_ok() && b2.value() == 0xFF);
                if (is_ret) {
                    std::fprintf(stderr,
                                 "[handle-bt] rsp+0x%03llx ret=0x%llx\n",
                                 static_cast<unsigned long long>(off),
                                 static_cast<unsigned long long>(a));
                    ++found;
                }
            }
        }
        set_last_error(error_codes::ERROR_INVALID_HANDLE);
        m.set_return(0);
        return;
    }

    // Success leaves the last error untouched (measured on Windows 11, for
    // file and pseudo handles alike: last_error_on_success_probe.expected).
    m.set_return(1);
}

// =============================================================================
// Export Registration
// =============================================================================

DllExports make_handle_exports(HandleTable& ht) {
    DllExports exports;
    // CloseHandle (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "CloseHandle", 0,
        [&ht](Marshaler& m) { impl_CloseHandle(m, ht); },
        4
    });
    // SetHandleCount (1 arg, stdcall_bytes=4) - legacy function, just returns the input
    exports.entries.push_back(ExportEntry{
        "SetHandleCount", 0,
        [](Marshaler& m) {
            // UINT SetHandleCount(UINT uNumber)
            // This is a legacy function from Win16. On Win32 it just returns uNumber.
            u32 count = static_cast<u32>(m.arg(0));
            m.set_return(count);
        },
        4
    });
    return exports;
}

} // namespace vitreo::winapi

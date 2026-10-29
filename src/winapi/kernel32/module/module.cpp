/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "core/pe/imports/api_set_availability.hpp"
#include "core/pe/imports/api_set_query_export.inc"
#include "capsule/vfs/filesystem.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdlib>
#include <unistd.h>
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>
#include <functional>
#include <unordered_set>

#include <vitreo/diagnostics.hpp>

namespace vitreo::winapi {

namespace {

// Kept out-of-line so adding thread-notification bookkeeping does not change
// ModuleManager's ABI. This matters for narrow incremental builds and also
// keeps loader serialization process-wide as Windows specifies.
std::mutex g_module_load_order_mutex;
std::recursive_mutex g_thread_notification_mutex;
std::unordered_map<const ModuleManager*, std::vector<std::string>>
    g_module_load_order;

} // namespace

namespace {

/// A failed dynamic export lookup is a loader event, not an IPC one.
///
/// These misses used to be logged through `ipc::ipc_log`, i.e. gated on
/// VITREO_IPC_TRACE. Nobody enabling an *IPC* trace is looking for a missing
/// export, so a real defect stayed invisible: Mono's P/Invoke fallback calls
/// GetProcAddress on every loaded module, and the miss that mattered was never
/// printed. Give it its own gate so it can be asked for by name.
///
/// Not unconditional on purpose: probing for an optional export and handling
/// NULL is normal, documented Windows practice, so a miss is suspicious rather
/// than always wrong.
/// VITREO_EXPORT_TRACE=1 logs misses; =2 also logs every successful
/// GetProcAddress (module, name, result), to see what a guest resolves.
int export_trace_level() {
    static const int level = [] {
        const char* v = std::getenv("VITREO_EXPORT_TRACE");
        if (v == nullptr || (v[0] == '0' && v[1] == '\0')) return 0;
        return v[0] == '2' ? 2 : 1;
    }();
    return level;
}

__attribute__((format(printf, 1, 2)))
void export_miss_log(const char* fmt, ...) {
    if (export_trace_level() == 0) return;
    char line[1024];
    const int prefix = std::snprintf(line, sizeof(line), "[export pid=%d] ",
                                     static_cast<int>(::getpid()));
    if (prefix < 0) return;
    std::va_list ap;
    va_start(ap, fmt);
    const int body = std::vsnprintf(
        line + prefix, sizeof(line) - static_cast<std::size_t>(prefix) - 1,
        fmt, ap);
    va_end(ap);
    if (body < 0) return;
    std::size_t len = static_cast<std::size_t>(prefix) +
                      static_cast<std::size_t>(body);
    if (len >= sizeof(line) - 1) len = sizeof(line) - 2;
    line[len] = '\n';
    (void)::write(2, line, len + 1);
}

}  // namespace







// =============================================================================
// Win32-style Queries
// =============================================================================



GuestAddr ModuleManager::get_proc_address_by_ordinal(GuestAddr module_base,
                                                      u16 ordinal) const {
    // Same shape as get_proc_address: table lookups under the shared table
    // lock, forwarder resolution (which may load) after it is released.
    std::string dll_key;
    bool image_backed = false;
    std::string forwarder;
    {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        const std::string* dll_name = name_for_base(module_base);
        if (!dll_name) {
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            return 0;
        }
        dll_key = *dll_name;

        auto it = modules_by_name_.find(dll_key);
        if (it != modules_by_name_.end() &&
            (it->second.is_pe_backed() || it->second.synthetic_image)) {
            image_backed = true;
            auto ex = it->second.exports_by_ordinal.find(ordinal);
            if (ex != it->second.exports_by_ordinal.end()) {
                return ex->second;
            }

            auto fwd = it->second.forwarders_by_ordinal.find(ordinal);
            if (fwd != it->second.forwarders_by_ordinal.end()) {
                forwarder = fwd->second;
            }
        }
    }

    std::string dll_with_ext = ModuleManager::thunk_registry_dll_key(dll_key);

    if (image_backed) {
        if (!forwarder.empty()) {
            const GuestAddr target = resolve_forwarded_export(forwarder, 0);
            if (target != 0) {
                return target;
            }
        }

        auto thunk = registry_.get_thunk_address(dll_with_ext, ordinal);
        if (thunk.is_ok()) {
            return registry_.guest_entry(thunk.value());
        }

        set_last_error(error_codes::ERROR_PROC_NOT_FOUND);
        return 0;
    }

    auto result = registry_.get_thunk_address(dll_with_ext, ordinal);
    if (result.is_ok()) {
        return registry_.guest_entry(result.value());
    }

    set_last_error(error_codes::ERROR_PROC_NOT_FOUND);
    return 0;
}

GuestAddr ModuleManager::get_or_stub_proc_address(GuestAddr module_base,
                                                  std::string_view func_name) {
    GuestAddr addr = get_proc_address(module_base, func_name);
    if (addr != 0) return addr;

    // Resolution failed. If we know the DLL name, install a logging
    // missing-export stub so the guest never receives a NULL FARPROC for
    // a real DLL handle. This mirrors PE-loader IAT patching for static
    // imports (see iat-missing-export-stub note) so dynamic resolution
    // does not silently leave NULLs in guest function pointer tables.
    std::string dll_key;
    {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        const std::string* dll_name = name_for_base(module_base == 0
                                                      ? main_module_base_
                                                      : module_base);
        if (!dll_name) {
            // Unknown handle — preserve the existing failure semantics.
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            return 0;
        }
        dll_key = *dll_name;
    }
    std::string dll_with_ext = ModuleManager::thunk_registry_dll_key(dll_key);
    GuestAddr stub = registry_.register_missing_export(dll_with_ext, func_name, 0);
    return stub;
}

GuestAddr ModuleManager::get_or_stub_proc_address_by_ordinal(GuestAddr module_base,
                                                             u16 ordinal) {
    GuestAddr addr = get_proc_address_by_ordinal(module_base, ordinal);
    if (addr != 0) return addr;

    std::string dll_key;
    {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        const std::string* dll_name = name_for_base(module_base == 0
                                                      ? main_module_base_
                                                      : module_base);
        if (!dll_name) {
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            return 0;
        }
        dll_key = *dll_name;
    }
    std::string dll_with_ext = ModuleManager::thunk_registry_dll_key(dll_key);
    GuestAddr stub = registry_.register_missing_export(dll_with_ext, std::string_view{}, ordinal);
    return stub;
}


ModuleManager::ModuleAtAddr ModuleManager::module_for_address(GuestAddr addr) const {
    if (addr == 0) return {};
    // The returned pointer is a map node: stable across a rehash, gone on
    // FreeLibrary (the guest's own race on Windows as well).
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    for (const auto& [name, info] : modules_by_name_) {
        (void)name;
        // RtlPcToFileHeader searches mapped images, including a DLL while
        // its imports bind or its TLS/DllMain initialization is executing.
        // https://learn.microsoft.com/en-us/windows/win32/api/winnt/nf-winnt-rtlpctofileheader
        if ((info.state != ModuleInfo::State::Mapping &&
             info.state != ModuleInfo::State::Initializing &&
             info.state != ModuleInfo::State::Loaded) || info.ref_count == 0) continue;
        GuestAddr end = info.image_end;
        if (end == 0) {
            // Synthetic/thunk-backed system DLLs leave image_end=0 but map a
            // SYNTHETIC_MODULE_SLOT_SIZE PE veneer; GetModuleHandleEx(FROM_ADDRESS)
            // on GetProcAddress(kernel32,...) must resolve (loader_contract_probe).
            if (!info.synthetic_image) continue;
            end = info.base_address + SYNTHETIC_MODULE_SLOT_SIZE;
        }
        if (addr >= info.base_address && addr < end) {
            return { &info, static_cast<u32>(addr - info.base_address) };
        }
    }
    return {};
}

GuestAddr ModuleManager::reserve_dynamic_base(usize image_size) {
    // Pad the requested image size to the 16 MB stride we historically
    // used so adjacent dynamic loads still get well-separated bases.
    constexpr usize STRIDE = 0x01000000;
    const usize padded =
        (image_size + STRIDE - 1) & ~(STRIDE - 1);

    if (m_memory == nullptr) {
        GuestAddr addr = m_next_dynamic_base;
        m_next_dynamic_base += padded;
        return addr;
    }

    GuestAddr gap = m_memory->find_free_gap(m_next_dynamic_base, padded);
    if (gap == 0) {
        // No room past the bump pointer — search from the very bottom of
        // the dynamic-load arena. As a last resort, fall back to the old
        // bump behaviour so the caller can fail with a meaningful error.
        gap = m_memory->find_free_gap(m_dynamic_base_start, padded);
    }
    if (gap == 0) {
        GuestAddr addr = m_next_dynamic_base;
        m_next_dynamic_base += padded;
        return addr;
    }

    m_next_dynamic_base = gap + padded;
    return gap;
}

bool ModuleManager::grow_thread_tls_array(ThreadTeb& thread,
                                          u32 new_capacity) {
    // Windows sizes TEB.ThreadLocalStoragePointer to the process's implicit
    // TLS index count and, when a later load needs a higher index, gives
    // every existing thread a larger vector with the old entries copied
    // (LoadLibrary of a DLL with IMAGE_TLS_DIRECTORY, Vista and later).
    if (m_memory == nullptr) return false;
    const usize ptr_bytes = thread.bitness == 64 ? 8u : 4u;
    const GuestAddr field = thread.teb_addr + (thread.bitness == 64 ? 0x58u : 0x2Cu);
    const GuestAddr old_array = thread.bitness == 64
        ? static_cast<GuestAddr>(m_memory->read_u64(field).value_or(0))
        : static_cast<GuestAddr>(m_memory->read_u32(field).value_or(0));

    // Whole host pages: see round_up_host_page.
    const usize bytes = static_cast<usize>(
        round_up_host_page(static_cast<u64>(new_capacity) * ptr_bytes));
    const GuestAddr search_start = round_up_host_page(m_next_tls_data);
    const GuestAddr array =
        m_memory->find_free_gap(search_start, bytes, kLinearHostPageSize);
    if (array == 0 ||
        (thread.bitness == 32 && array + bytes > 0x100000000ULL) ||
        m_memory->map_region(array, bytes, MemoryProtection::ReadWrite).is_err()) {
        return false;
    }
    m_next_tls_data = array + bytes;

    if (old_array != 0) {
        std::vector<u8> entries(static_cast<usize>(thread.tls_capacity) * ptr_bytes);
        if (m_memory->read_bytes(old_array, entries.data(), entries.size()).is_err() ||
            m_memory->write_bytes(array, entries.data(), entries.size()).is_err()) {
            (void)m_memory->unmap_region(array, bytes);
            return false;
        }
    }
    const bool published = thread.bitness == 64
        ? m_memory->write_u64(field, array).is_ok()
        : m_memory->write_u32(field, static_cast<u32>(array)).is_ok();
    if (!published) {
        (void)m_memory->unmap_region(array, bytes);
        return false;
    }
    thread.grown_arrays.emplace_back(array, bytes);
    thread.tls_capacity = static_cast<u32>(bytes / ptr_bytes);
    return true;
}

void ModuleManager::register_thread_teb(GuestAddr teb_addr, u32 bitness,
                                        u32 tls_capacity) {
    if (teb_addr == 0 || (bitness != 32 && bitness != 64)) return;
    std::lock_guard<std::mutex> lock(m_tls_mutex);
    auto entry = std::find_if(
        m_thread_tebs.begin(), m_thread_tebs.end(),
        [teb_addr](const ThreadTeb& entry) {
            return entry.teb_addr == teb_addr;
        });
    if (entry == m_thread_tebs.end()) {
        ThreadTeb added{teb_addr, bitness};
        added.tls_capacity = std::max(tls_capacity, kMinTlsArrayCapacity);
        m_thread_tebs.push_back(std::move(added));
        entry = std::prev(m_thread_tebs.end());
    }
    // A re-registration keeps the recorded capacity: it already reflects any
    // growth. A thread whose vector was allocated before a DLL raised the
    // index range gets a larger vector before its slots are repaired below.
    if (entry->tls_capacity < m_tls_array_capacity &&
        !grow_thread_tls_array(*entry, m_tls_array_capacity)) {
        fprintf(stderr,
                "[module] register_thread_teb: could not grow TLS vector of "
                "teb=0x%llx to %u entries\n",
                static_cast<unsigned long long>(teb_addr), m_tls_array_capacity);
    }
    const u32 thread_capacity = entry->tls_capacity;
    // Thread creation snapshots the currently loaded static-TLS modules before
    // registering its TEB.  A DLL can be loaded between those two operations.
    // Loader callbacks also synchronize their active CPU's TEB through this
    // path, so an already-registered TEB must still have missing slots repaired.
    // Close both gaps while holding the same lock as setup_dll_tls().
    GuestAddr tls_array = 0;
    if (m_memory != nullptr) {
        if (bitness == 64) {
            const auto array = m_memory->read_u64(teb_addr + 0x58);
            if (array.is_ok()) {
                tls_array = static_cast<GuestAddr>(array.value());
            }
        } else {
            const auto array = m_memory->read_u32(teb_addr + 0x2C);
            if (array.is_ok()) {
                tls_array = static_cast<GuestAddr>(array.value());
            }
        }
    }

    if (tls_array != 0) {
        for (DllTlsSlot& slot : m_dll_tls_slots) {
            if (slot.bitness != bitness) continue;
            if (slot.slot_index >= thread_capacity) continue;

            const GuestAddr slot_addr = tls_array +
                static_cast<GuestAddr>(slot.slot_index) *
                    (bitness == 64 ? 8u : 4u);
            const u64 existing = bitness == 64
                ? m_memory->read_u64(slot_addr).value_or(0)
                : static_cast<u64>(m_memory->read_u32(slot_addr).value_or(0));
            if (existing != 0) continue;

            u64 block_size = slot.template_size + slot.zero_fill;
            if (block_size == 0) block_size = 256;
            // Whole host pages: see round_up_host_page.
            const usize allocation_size =
                static_cast<usize>(round_up_host_page(block_size));
            const GuestAddr search_start = round_up_host_page(m_next_tls_data);
            const GuestAddr block = m_memory->find_free_gap(
                search_start, allocation_size, kLinearHostPageSize);
            if (block == 0 ||
                m_memory->map_region(block, allocation_size,
                                     MemoryProtection::ReadWrite).is_err()) {
                continue;
            }
            m_next_tls_data = block + allocation_size;

            bool initialized = true;
            if (slot.template_size > 0) {
                std::vector<u8> bytes(static_cast<usize>(slot.template_size));
                initialized = m_memory->read_bytes(
                    slot.template_start, bytes.data(), bytes.size()).is_ok() &&
                    m_memory->write_bytes(
                        block, bytes.data(), bytes.size()).is_ok();
            }
            const bool published = initialized &&
                (bitness == 64
                    ? m_memory->write_u64(slot_addr, block).is_ok()
                    : m_memory->write_u32(
                          slot_addr, static_cast<u32>(block)).is_ok());
            if (!published) {
                (void)m_memory->unmap_region(block, allocation_size);
                continue;
            }
            slot.owned_blocks.push_back(block);
        }
    }
}

void ModuleManager::unregister_thread_teb(GuestAddr teb_addr) {
    if (teb_addr == 0) return;
    std::lock_guard<std::mutex> lock(m_tls_mutex);
    if (m_memory != nullptr) {
        for (const ThreadTeb& entry : m_thread_tebs) {
            if (entry.teb_addr != teb_addr) continue;
            for (const auto& [array, bytes] : entry.grown_arrays) {
                (void)m_memory->unmap_region(array, bytes);
            }
        }
    }
    m_thread_tebs.erase(
        std::remove_if(m_thread_tebs.begin(), m_thread_tebs.end(),
                       [teb_addr](const ThreadTeb& entry) {
                           return entry.teb_addr == teb_addr;
                       }),
        m_thread_tebs.end());
}







u32 ModuleManager::register_dll_notification(
    u32 flags, GuestAddr callback, GuestAddr context, GuestAddr* cookie) {
    constexpr u32 STATUS_SUCCESS = 0x00000000u;
    constexpr u32 STATUS_INVALID_PARAMETER = 0xC000000Du;
    if (flags != 0 || callback == 0 || cookie == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    std::lock_guard<std::mutex> lock(m_dll_notification_mutex);
    const GuestAddr value = m_next_dll_notification_cookie++;
    m_dll_notifications.push_back({value, callback, context});
    *cookie = value;
    VITREO_TRACE(
        "[loader] LdrRegisterDllNotification callback=0x%llx "
        "context=0x%llx cookie=0x%llx\n",
        static_cast<unsigned long long>(callback),
        static_cast<unsigned long long>(context),
        static_cast<unsigned long long>(value));
    return STATUS_SUCCESS;
}

u32 ModuleManager::unregister_dll_notification(GuestAddr cookie) {
    constexpr u32 STATUS_SUCCESS = 0x00000000u;
    constexpr u32 STATUS_DLL_NOT_FOUND = 0xC0000135u;
    std::lock_guard<std::mutex> lock(m_dll_notification_mutex);
    const auto it = std::find_if(
        m_dll_notifications.begin(), m_dll_notifications.end(),
        [cookie](const DllNotificationRegistration& registration) {
            return registration.cookie == cookie;
        });
    if (it == m_dll_notifications.end()) return STATUS_DLL_NOT_FOUND;
    m_dll_notifications.erase(it);
    VITREO_TRACE(
        "[loader] LdrUnregisterDllNotification cookie=0x%llx -> success\n",
        static_cast<unsigned long long>(cookie));
    return STATUS_SUCCESS;
}

pe::PeLoader::PreImportCallback ModuleManager::make_pre_import_callback(
    std::string full_name, std::string base_name) {
    return [this, full_name = std::move(full_name),
            base_name = std::move(base_name)](
               GuestAddr image_base, usize image_size, bool) {
        notify_dll(1, full_name, base_name, image_base, image_size);
    };
}


bool ModuleManager::acquire_module_reference(GuestAddr module_base, bool pin) {
    // Exclusive: two GetModuleHandleEx callers must not both ++ref_count on a
    // stale read.
    std::unique_lock<HostRwLock> tables(m_tables_mutex);
    const std::string* dll_name = name_for_base(module_base);
    if (!dll_name) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        return false;
    }

    auto it = modules_by_name_.find(*dll_name);
    if (it == modules_by_name_.end() ||
        it->second.state != ModuleInfo::State::Loaded ||
        it->second.ref_count == 0) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        return false;
    }

    ++it->second.ref_count;
    if (pin) {
        it->second.pinned = true;
    }
    return true;
}

void ModuleManager::release_dll_tls_slots_in_range(GuestAddr base,
                                                   GuestAddr end) {
    std::lock_guard<std::mutex> lock(m_tls_mutex);
    for (auto it = m_dll_tls_slots.begin(); it != m_dll_tls_slots.end();) {
        if (it->template_start >= base && it->template_start < end) {
            // Static PE TLS belongs to the loaded image instance.  Leaving
            // these mappings alive across FreeLibrary/LoadLibrary cycles both
            // exposes stale storage through each TEB and leaks one block per
            // extant thread on every reload.
            for (const ThreadTeb& thread : m_thread_tebs) {
                if (thread.bitness != it->bitness) continue;
                GuestAddr tls_array = 0;
                if (it->bitness == 64) {
                    auto array = m_memory->read_u64(thread.teb_addr + 0x58);
                    if (array.is_ok()) {
                        tls_array = static_cast<GuestAddr>(array.value());
                    }
                    if (tls_array != 0) {
                        (void)m_memory->write_u64(
                            tls_array + it->slot_index * 8u, 0);
                    }
                } else {
                    auto array = m_memory->read_u32(thread.teb_addr + 0x2C);
                    if (array.is_ok()) {
                        tls_array = static_cast<GuestAddr>(array.value());
                    }
                    if (tls_array != 0) {
                        (void)m_memory->write_u32(
                            tls_array + it->slot_index * 4u, 0);
                    }
                }
            }
            for (GuestAddr block : it->owned_blocks) {
                if (block == 0 || it->allocation_size == 0) continue;
                (void)m_memory->unmap_region(block, it->allocation_size);
                m_next_tls_data = std::min(m_next_tls_data, block);
            }
            m_free_tls_indices.push_back(it->slot_index);
            it = m_dll_tls_slots.erase(it);
        } else {
            ++it;
        }
    }
}



bool ModuleManager::disable_thread_library_calls(GuestAddr module_base) {
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    auto name_it = base_to_name_.find(module_base);
    if (name_it == base_to_name_.end()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    auto module_it = modules_by_name_.find(name_it->second);
    if (module_it == modules_by_name_.end() || module_it->second.tls_dir_rva != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return false;
    }
    module_it->second.thread_notifications_disabled = true;
    return true;
}

void ModuleManager::notify_thread_attach() {
    if (!m_guest_call) return;

    // Microsoft documents DLL entry-point calls as process-wide serialized
    // and in the context of the newly-created thread. Keep the lock recursive
    // because a legal Kernel32 call made by DllMain can re-enter loader-facing
    // code on this same thread.
    std::lock_guard<std::recursive_mutex> notification_lock(
        g_thread_notification_mutex);

    // Snapshot keys: guest callbacks may mutate the module table, and holding
    // unordered-map iterators across guest execution would be invalid.
    std::vector<std::string> load_order;
    {
        std::lock_guard<std::mutex> load_order_lock(
            g_module_load_order_mutex);
        load_order = g_module_load_order[this];
    }
    constexpr u64 DLL_THREAD_ATTACH_REASON = 2;
    for (const std::string& key : load_order) {
        // Copy everything used after the callback boundary, under the table
        // lock: a callback can LoadLibrary and rehash modules_by_name_, and so
        // can another thread's LoadLibrary right now.
        GuestAddr base = 0;
        GuestAddr entry = 0;
        u32 bitness = 0;
        u32 tls_rva = 0;
        bool notifications_disabled = false;
        std::string path;
        {
            std::shared_lock<HostRwLock> tables(m_tables_mutex);
            auto it = modules_by_name_.find(key);
            if (it == modules_by_name_.end()) continue;

            const ModuleInfo& current = it->second;
            if (current.state != ModuleInfo::State::Loaded ||
                current.ref_count == 0 ||
                !current.dllmain_called || !current.is_pe_backed() ||
                current.base_address == main_module_base_) {
                continue;
            }
            base = current.base_address;
            entry = current.entry_point;
            bitness = current.bitness;
            tls_rva = current.tls_dir_rva;
            notifications_disabled = current.thread_notifications_disabled;
            path = current.path;
        }

        // DisableThreadLibraryCalls fails for modules with static TLS, so TLS
        // callbacks are never suppressed by that optimization.
        if (tls_rva != 0) {
            fire_tls_callbacks(base, bitness, base + tls_rva,
                               DLL_THREAD_ATTACH_REASON, path.c_str());
        }
        if (entry != 0 && !notifications_disabled) {
            const u64 args[3] = {base, DLL_THREAD_ATTACH_REASON, 0};
            (void)m_guest_call(entry, args, 3, bitness == 64);
        }
    }
    // The EXE is never called at its entry point for thread notifications,
    // but its TLS callbacks are -- after every DLL's (measured order "aA").
    if (main_module_tls_rva_ != 0 && main_module_base_ != 0) {
        fire_tls_callbacks(main_module_base_, main_module_bitness_,
                           main_module_base_ + main_module_tls_rva_,
                           DLL_THREAD_ATTACH_REASON, main_module_path_.c_str());
    }
}

void ModuleManager::notify_thread_detach() {
    if (!m_guest_call) return;

    // A cleanly exiting Windows thread receives loader notifications while
    // its TEB and TLS storage still exist. Loader notifications are serialized
    // process-wide, just like DLL_THREAD_ATTACH.
    std::lock_guard<std::recursive_mutex> notification_lock(
        g_thread_notification_mutex);

    std::vector<std::string> load_order;
    {
        std::lock_guard<std::mutex> load_order_lock(
            g_module_load_order_mutex);
        load_order = g_module_load_order[this];
    }

    constexpr u64 DLL_THREAD_DETACH_REASON = 3;
    for (auto order_it = load_order.rbegin();
         order_it != load_order.rend(); ++order_it) {
        GuestAddr base = 0;
        GuestAddr entry = 0;
        u32 bitness = 0;
        u32 tls_rva = 0;
        bool notifications_disabled = false;
        std::string path;
        {
            std::shared_lock<HostRwLock> tables(m_tables_mutex);
            auto it = modules_by_name_.find(*order_it);
            if (it == modules_by_name_.end()) continue;

            const ModuleInfo& current = it->second;
            if (current.state != ModuleInfo::State::Loaded ||
                current.ref_count == 0 ||
                !current.dllmain_called || !current.is_pe_backed() ||
                current.base_address == main_module_base_) {
                continue;
            }
            base = current.base_address;
            entry = current.entry_point;
            bitness = current.bitness;
            tls_rva = current.tls_dir_rva;
            notifications_disabled = current.thread_notifications_disabled;
            path = current.path;
        }

        // Windows runs TLS deinitializers before the DLL entry point while the
        // exiting thread's static TLS vector is still valid.
        if (tls_rva != 0) {
            fire_tls_callbacks(base, bitness, base + tls_rva,
                               DLL_THREAD_DETACH_REASON, path.c_str());
        }
        if (entry != 0 && !notifications_disabled) {
            const u64 args[3] = {base, DLL_THREAD_DETACH_REASON, 0};
            (void)m_guest_call(entry, args, 3, bitness == 64);
        }
    }
    // EXE TLS callbacks run after the DLLs' detach notifications too
    // (measured order "dD"): this is where MSVC's __dyn_tls_dtor runs the
    // EXE's thread_local destructors.
    if (main_module_tls_rva_ != 0 && main_module_base_ != 0) {
        fire_tls_callbacks(main_module_base_, main_module_bitness_,
                           main_module_base_ + main_module_tls_rva_,
                           DLL_THREAD_DETACH_REASON, main_module_path_.c_str());
    }
}

void ModuleManager::notify_process_detach() {
    // Learn ExitProcess: "The entry-point functions of all loaded dynamic-link
    // libraries (DLLs) are called with DLL_PROCESS_DETACH." Measured on
    // Windows 11 (loader_contract_probe.expected "== exit"): reverse load
    // order, TLS then DllMain, and lpvReserved is non-NULL.
    if (!m_guest_call) return;
    std::lock_guard<std::recursive_mutex> loader_guard(m_loader_mutex);

    std::vector<std::string> load_order;
    {
        std::lock_guard<std::mutex> load_order_lock(
            g_module_load_order_mutex);
        load_order = g_module_load_order[this];
    }

    constexpr u64 DLL_PROCESS_DETACH_REASON = 0;
    // Non-NULL reserved marks process termination (vs FreeLibrary unload).
    constexpr u64 kProcessTerminating = 1;
    for (auto order_it = load_order.rbegin();
         order_it != load_order.rend(); ++order_it) {
        auto it = modules_by_name_.find(*order_it);
        if (it == modules_by_name_.end()) continue;

        ModuleInfo& current = it->second;
        if (current.state != ModuleInfo::State::Loaded ||
            !current.dllmain_called || !current.is_pe_backed() ||
            current.base_address == main_module_base_) {
            continue;
        }

        const GuestAddr base = current.base_address;
        const GuestAddr entry = current.entry_point;
        const u32 bitness = current.bitness;
        const u32 tls_rva = current.tls_dir_rva;
        const std::string path = current.path;

        if (tls_rva != 0) {
            fire_tls_callbacks(base, bitness, base + tls_rva,
                               DLL_PROCESS_DETACH_REASON, path.c_str());
        }
        if (entry != 0) {
            const u64 args[3] = {base, DLL_PROCESS_DETACH_REASON,
                                 kProcessTerminating};
            (void)m_guest_call(entry, args, 3, bitness == 64);
            current.dllmain_called = false;
        }
    }
}

void ModuleManager::record_module_load_order(std::string_view key) {
    std::lock_guard<std::mutex> lock(g_module_load_order_mutex);
    auto& order = g_module_load_order[this];
    if (std::find(order.begin(), order.end(), key) == order.end()) {
        order.emplace_back(key);
    }
}

const std::string* ModuleManager::module_path(GuestAddr module_base) const {
    if (module_base == 0 || module_base == main_module_base_) {
        return &main_module_path_;
    }
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    const std::string* dll_name = name_for_base(module_base);
    if (!dll_name) return nullptr;
    auto it = modules_by_name_.find(*dll_name);
    return it == modules_by_name_.end() ? nullptr : &it->second.path;
}

u32 ModuleManager::get_module_filename(GuestAddr module_base,
                                        char* buffer, u32 size) const {
    if (!buffer || size == 0) {
        return 0;
    }

    // 0 / main module base → main exe path
    const std::string* path = nullptr;
    // Held through the copy below: `path` points into the table.
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    if (module_base == 0 || module_base == main_module_base_) {
        path = &main_module_path_;
    } else {
        const std::string* dll_name = name_for_base(module_base);
        if (!dll_name) {
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            buffer[0] = '\0';
            return 0;
        }
        auto it = modules_by_name_.find(*dll_name);
        if (it == modules_by_name_.end()) {
            set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
            buffer[0] = '\0';
            return 0;
        }
        path = &it->second.path;
    }

    u32 copy_len = static_cast<u32>(path->size());
    if (copy_len >= size) {
        // Truncate — Windows behaviour: fills buffer, no NUL, returns size
        std::memcpy(buffer, path->data(), size);
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return size;
    }

    std::memcpy(buffer, path->data(), copy_len);
    buffer[copy_len] = '\0';
    // Success leaves the last error to the caller: GetModuleFileNameA/W set
    // ERROR_SUCCESS when the name fits, but the other exports built on this
    // (K32GetModuleFileNameEx, QueryFullProcessImageName, SearchPath, ...)
    // leave it untouched (measured on Windows 11:
    // last_error_on_success_probe.expected).
    return copy_len;
}

std::vector<GuestAddr> ModuleManager::get_all_module_handles() const {
    std::vector<GuestAddr> handles;

    // Main executable first (if set) — matches PEB InLoadOrderModuleList.
    if (main_module_base_ != 0) {
        handles.push_back(main_module_base_);
    }

    // Remaining modules in recorded load order (not unordered_map iteration).
    std::vector<std::string> load_order;
    {
        std::lock_guard<std::mutex> lock(g_module_load_order_mutex);
        auto it = g_module_load_order.find(this);
        if (it != g_module_load_order.end()) {
            load_order = it->second;
        }
    }
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    handles.reserve(modules_by_name_.size() + 1);
    for (const std::string& key : load_order) {
        auto it = modules_by_name_.find(key);
        if (it == modules_by_name_.end()) {
            continue;
        }
        const ModuleInfo& info = it->second;
        if (info.state != ModuleInfo::State::Loaded || info.ref_count == 0) {
            continue;
        }
        if (info.base_address == main_module_base_) {
            continue;
        }
        handles.push_back(info.base_address);
    }

    // Modules never recorded in load order (e.g. synthetic builtins) still
    // appear after the ordered list so the snapshot stays complete.
    for (const auto& [name, info] : modules_by_name_) {
        (void)name;
        if (info.state != ModuleInfo::State::Loaded || info.ref_count == 0) {
            continue;
        }
        if (info.base_address == main_module_base_) {
            continue;
        }
        if (std::find(handles.begin(), handles.end(), info.base_address) ==
            handles.end()) {
            handles.push_back(info.base_address);
        }
    }

    return handles;
}

const ModuleInfo* ModuleManager::get_module_info_by_handle(GuestAddr module_base) const {
    // Special case: 0 or main module base
    if (module_base == 0) {
        module_base = main_module_base_;
    }

    // Look up by base address
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    const std::string* dll_name = name_for_base(module_base);
    if (!dll_name) {
        return nullptr;
    }

    auto it = modules_by_name_.find(*dll_name);
    if (it == modules_by_name_.end()) {
        return nullptr;
    }

    return &it->second;
}

// =============================================================================
// Private Helpers
// =============================================================================

bool ModuleManager::resource_directory_for_base(GuestAddr base, u32& rva_out,
                                                u32& size_out,
                                                GuestAddr* untagged_base_out) const {
    // AS_DATAFILE / AS_IMAGE_RESOURCE handles carry LDR_IS_DATAFILE (bit 0)
    // or LDR_IS_IMAGEMAPPING (bit 1). Strip them for lookup and for RVA math.
    const GuestAddr tagged = base;
    const GuestAddr untagged = base & ~static_cast<GuestAddr>(3u);
    if (untagged_base_out) *untagged_base_out = untagged;

    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    auto df = data_file_mappings_.find(tagged);
    if (df == data_file_mappings_.end() && tagged != untagged) {
        // Callers may pass the untagged base after stripping themselves.
        for (const auto& [h, mapping] : data_file_mappings_) {
            if (mapping.base == untagged) {
                if (mapping.resource_rva == 0) return false;
                rva_out = mapping.resource_rva;
                size_out = mapping.resource_size;
                return true;
            }
        }
    } else if (df != data_file_mappings_.end()) {
        if (df->second.resource_rva == 0) return false;
        rva_out = df->second.resource_rva;
        size_out = df->second.resource_size;
        return true;
    }

    const std::string* dll_name = name_for_base(untagged);
    if (!dll_name) return false;
    auto it = modules_by_name_.find(*dll_name);
    if (it == modules_by_name_.end() || it->second.resource_rva == 0) return false;
    rva_out  = it->second.resource_rva;
    size_out = it->second.resource_size;
    return true;
}

// Caller holds m_tables_mutex (shared) or the loader lock on the load path.
const std::string* ModuleManager::name_for_base(GuestAddr base) const {
    auto it = base_to_name_.find(base);
    if (it == base_to_name_.end()) {
        return nullptr;
    }
    return &it->second;
}

// =============================================================================
// Win32 Thunk Implementations
// =============================================================================

void impl_GetModuleHandleA(Marshaler& m, ModuleManager& mm) {
    // HMODULE GetModuleHandleA(LPCSTR lpModuleName)
    GuestAddr name_ptr = m.arg_ptr(0);

    GuestAddr result = 0;
    std::string name = "(null)";
    if (name_ptr == 0) {
        result = mm.get_module_handle("");
    } else {
        auto str_result = m.read_ansi_string(name_ptr);
        if (str_result.is_ok()) {
            name = str_result.value();
            result = mm.get_module_handle(name);
        }
    }
    VITREO_TRACE( "[module] GetModuleHandleA(\"%s\") → 0x%llx\n",
                 name.c_str(), static_cast<unsigned long long>(result));
    // A module that is not loaded reports ERROR_MOD_NOT_FOUND. Guests probe
    // for optional DLLs with GetModuleHandle and read the error to tell
    // "not loaded" apart from a call that simply returned a null handle.
    if (result == 0) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
    }
    m.set_return(result);
}

void impl_GetModuleHandleW(Marshaler& m, ModuleManager& mm) {
    // HMODULE GetModuleHandleW(LPCWSTR lpModuleName)
    GuestAddr name_ptr = m.arg_ptr(0);

    GuestAddr result = 0;
    std::string name = "(null)";
    if (name_ptr == 0) {
        result = mm.get_module_handle("");
    } else {
        auto str_result = m.read_wide_string(name_ptr);
        if (str_result.is_ok()) {
            name = str_result.value();
            result = mm.get_module_handle(name);
        }
    }
    VITREO_TRACE("[module] GetModuleHandleW(\"%s\") -> 0x%llx\n",
                 name.c_str(), static_cast<unsigned long long>(result));
    // A module that is not loaded reports ERROR_MOD_NOT_FOUND. Guests probe
    // for optional DLLs with GetModuleHandle and read the error to tell
    // "not loaded" apart from a call that simply returned a null handle.
    if (result == 0) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
    }
    m.set_return(result);
}

void impl_GetProcAddress(Marshaler& m, ModuleManager& mm) {
    // FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
    GuestAddr hmod = m.arg_ptr(0);
    GuestAddr name_ptr = m.arg_ptr(1);

    // Learn LoadLibraryEx AS_DATAFILE: GetProcAddress is not valid on a
    // data-file handle (loader_contract_probe.expected → ERROR_MOD_NOT_FOUND).
    if (mm.is_data_file_handle(hmod) || (hmod & 3u) != 0) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        m.set_return(0u);
        return;
    }

    GuestAddr result = 0;
    // Real Windows behaviour: GetProcAddress returns NULL when the export
    // is not present and sets ERROR_PROC_NOT_FOUND.  Auto-stubbing missing
    // exports here would break the very common feature-detection pattern
    //
    //     pfn = GetProcAddress(h, "MaybeNewerApi");
    //     if (pfn) pfn(...);
    //
    // because `pfn` would be a non-NULL stub that returns 0xFFFFFFFF, and
    // the caller would then dereference that bogus value.  Static-import
    // resolution (PE loader IAT patching) keeps its own auto-stub path so
    // images with unsatisfiable static imports still load.
    // Learn: an ordinal is in the low word "and the high-order word must be
    // zero" -- for a 64-bit LPCSTR that is every bit above 15 (IS_INTRESOURCE).
    // Testing only bits 16-31 misread a name above 4 GiB as an ordinal.
    if (name_ptr != 0 && (static_cast<u64>(name_ptr) >> 16) == 0) {
        result = mm.get_proc_address_by_ordinal(hmod, static_cast<u16>(name_ptr));
        const std::string* mod_nm = mm.name_for_base(hmod);
        VITREO_TRACE( "[module] GetProcAddress(0x%llx=%s, ord=%u) → 0x%llx\n",
                     static_cast<unsigned long long>(hmod),
                     mod_nm ? mod_nm->c_str() : "?",
                     static_cast<unsigned>(name_ptr),
                     static_cast<unsigned long long>(result));
    } else if (name_ptr != 0) {
        auto str_result = m.read_ansi_string(name_ptr);
        if (str_result.is_ok()) {
            result = mm.get_proc_address(hmod, str_result.value());
            const std::string* traced_module = mm.name_for_base(hmod);
            VITREO_TRACE( "[module] GetProcAddress(0x%llx=%s, \"%s\") → 0x%llx\n",
                         static_cast<unsigned long long>(hmod),
                         traced_module ? traced_module->c_str() : "?",
                         str_result.value().c_str(),
                         static_cast<unsigned long long>(result));
            if (result == 0) {
                // No thunk by that name -- it may be a DATA export. Those are
                // variables (_HUGE, _environ, _wcmdln, _adjust_fdiv ...), so
                // they are bound straight into the import table at load time
                // and have no thunk to find; a guest resolving one through
                // GetProcAddress still expects its address, which is what
                // Windows returns.
                if (const std::string* mod_nm = mm.name_for_base(hmod)) {
                    result = mm.registry().lookup_data_export(
                        *mod_nm, str_result.value());
                }
                if (result == 0) {
                    result = static_cast<GuestAddr>(
                        mm.resolve_crt_data(str_result.value()));
                }
            }
            if (result == 0) {
                // A null return here is the seed of a later null indirect call
                // if the guest skips its own null-check. Log misses so missing
                // Windows exports the guest dynamically resolves are visible.
                // Rate-limit: RCT3 hammered DebugSetMute misses thousands of
                // times per menu frame and the IPC log I/O starved the UI.
                static thread_local std::string s_last_miss;
                static thread_local u32 s_miss_repeat = 0;
                const std::string& miss_name = str_result.value();
                if (miss_name == s_last_miss) {
                    ++s_miss_repeat;
                    if (s_miss_repeat > 4u && (s_miss_repeat & 0xFFu) != 0u) {
                        // skip
                    } else {
                        u64 caller = 0;
                        if (m.has_cpu()) {
                            auto* cpu = m.cpu();
                            if (cpu->is_64bit()) {
                                auto read = m.memory().read_u64(
                                    static_cast<GuestAddr>(cpu->gpr.rsp));
                                if (read.is_ok()) caller = read.value();
                            } else {
                                auto read = m.memory().read_u32(
                                    static_cast<GuestAddr>(cpu->sp()));
                                if (read.is_ok()) caller = read.value();
                            }
                        }
                        export_miss_log(
                            "GetProcAddress MISS hmod=0x%llx \"%s\" "
                            "caller=0x%llx (repeat=%u)",
                            static_cast<unsigned long long>(hmod),
                            miss_name.c_str(),
                            static_cast<unsigned long long>(caller),
                            s_miss_repeat);
                    }
                } else {
                    s_last_miss = miss_name;
                    s_miss_repeat = 1;
                    u64 caller = 0;
                    if (m.has_cpu()) {
                        auto* cpu = m.cpu();
                        if (cpu->is_64bit()) {
                            auto read = m.memory().read_u64(
                                static_cast<GuestAddr>(cpu->gpr.rsp));
                            if (read.is_ok()) caller = read.value();
                        } else {
                            auto read = m.memory().read_u32(
                                static_cast<GuestAddr>(cpu->sp()));
                            if (read.is_ok()) caller = read.value();
                        }
                    }
                    export_miss_log(
                        "GetProcAddress MISS hmod=0x%llx \"%s\" caller=0x%llx",
                        static_cast<unsigned long long>(hmod),
                        miss_name.c_str(),
                        static_cast<unsigned long long>(caller));
                }
            }
        }
    }
    if (result == 0) {
        set_last_error(error_codes::ERROR_PROC_NOT_FOUND);
    } else if (export_trace_level() >= 2) {
        const std::string* mod_nm = mm.name_for_base(hmod);
        u64 caller = 0;
        if (m.has_cpu()) {
            auto* cpu = m.cpu();
            caller = cpu->is_64bit()
                ? m.memory().read_u64(static_cast<GuestAddr>(cpu->gpr.rsp)).value_or(0)
                : m.memory().read_u32(static_cast<GuestAddr>(cpu->sp())).value_or(0);
        }
        if (name_ptr != 0 && (name_ptr >> 16) != 0) {
            auto name = m.read_ansi_string(name_ptr);
            export_miss_log("GetProcAddress HIT %s!\"%s\" -> 0x%llx caller=0x%llx",
                            mod_nm ? mod_nm->c_str() : "?",
                            name.is_ok() ? name.value().c_str() : "?",
                            static_cast<unsigned long long>(result),
                            static_cast<unsigned long long>(caller));
        } else {
            export_miss_log("GetProcAddress HIT %s!#%llu -> 0x%llx caller=0x%llx",
                            mod_nm ? mod_nm->c_str() : "?",
                            static_cast<unsigned long long>(name_ptr),
                            static_cast<unsigned long long>(result),
                            static_cast<unsigned long long>(caller));
        }
    }
    m.set_return(result);
}

// VITREO_LOG_LOADLIB=1: report the guest return address of a dynamic load.
// The module-relative caller identifies the routine that decides *which*
// module to load, which is what a "why was this never loaded" investigation
// needs — the request name alone cannot point at the deciding code.
static void log_loadlib_caller(Marshaler& m, const std::string& what) {
    static const bool on = std::getenv("VITREO_LOG_LOADLIB") != nullptr;
    if (!on || !m.has_cpu()) return;
    const GuestAddr sp = static_cast<GuestAddr>(m.cpu()->sp());
    u64 caller = 0;
    if (m.cpu_is_64bit()) {
        auto r = m.read_u64(sp);
        if (r.is_ok()) caller = r.value();
    } else {
        auto r = m.read_u32(sp);
        if (r.is_ok()) caller = r.value();
    }
    fprintf(stderr, "[module] LoadLibrary caller ret=0x%llx for \"%s\"\n",
            static_cast<unsigned long long>(caller), what.c_str());
}

void impl_LoadLibraryA(Marshaler& m, ModuleManager& mm) {
    // HMODULE LoadLibraryA(LPCSTR lpLibFileName)
    GuestAddr name_ptr = m.arg_ptr(0);

    GuestAddr result = 0;
    if (name_ptr != 0) {
        auto str_result = m.read_ansi_string(name_ptr);
        if (str_result.is_ok()) {
            log_loadlib_caller(m, str_result.value());
            result = mm.load_library(str_result.value());
        }
    } else {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    }
    m.set_return(result);
}

void impl_LoadLibraryW(Marshaler& m, ModuleManager& mm) {
    // HMODULE LoadLibraryW(LPCWSTR lpLibFileName)
    GuestAddr name_ptr = m.arg_ptr(0);

    GuestAddr result = 0;
    if (name_ptr != 0) {
        auto str_result = m.read_wide_string(name_ptr);
        if (str_result.is_ok()) {
            if (str_result.value().empty()) {
                // loader_contract_probe.expected LoadLibraryW("") → null err=57
                set_last_error(error_codes::ERROR_INVALID_PARAMETER);
            } else {
                log_loadlib_caller(m, str_result.value());
                result = mm.load_library(str_result.value());
            }
        }
    } else {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
    }
    m.set_return(result);
}

void impl_FreeLibrary(Marshaler& m, ModuleManager& mm) {
    // BOOL FreeLibrary(HMODULE hLibModule)
    GuestAddr hmod = m.arg_ptr(0);

    bool ok = mm.free_library(hmod);
    m.set_return(ok ? 1u : 0u);
}

void impl_DisableThreadLibraryCalls(Marshaler& m, ModuleManager& mm) {
    const GuestAddr module = m.arg_ptr(0);
    const bool ok = mm.disable_thread_library_calls(module);
    VITREO_TRACE("[module] DisableThreadLibraryCalls(0x%llx) -> %s\n",
                 static_cast<unsigned long long>(module), ok ? "TRUE" : "FALSE");
    m.set_return(ok ? 1u : 0u);
}

namespace {

/// GetModuleFileNameA/W buffer contract (Learn, Vista+ behaviour; measured by
/// tools/oracle_probe/module_filename_probe.c / .expected), in TCHARs:
///  - the name fits (length < nSize): name + NUL, returns the length, last
///    error ERROR_SUCCESS;
///  - otherwise: the first nSize-1 TCHARs + NUL, returns nSize,
///    ERROR_INSUFFICIENT_BUFFER; nSize == 0 writes nothing and returns 0.
template <typename Unit, typename WriteUnit>
u32 store_module_file_name(const std::vector<Unit>& name, u32 capacity,
                           GuestAddr buffer, WriteUnit write_unit) {
    const u32 length = static_cast<u32>(name.size());
    if (capacity == 0) {
        set_last_error(error_codes::ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    const bool fits = length < capacity;
    const u32 copied = fits ? length : capacity - 1;
    if (buffer != 0) {
        for (u32 i = 0; i < copied; ++i) write_unit(buffer, i, name[i]);
        write_unit(buffer, copied, Unit{0});
    }
    set_last_error(fits ? error_codes::ERROR_SUCCESS
                        : error_codes::ERROR_INSUFFICIENT_BUFFER);
    return fits ? length : capacity;
}

} // namespace

void impl_GetModuleFileNameA(Marshaler& m, ModuleManager& mm) {
    // DWORD GetModuleFileNameA(HMODULE hModule, LPSTR lpFilename, DWORD nSize)
    // Learn: *A writes ANSI (CP_ACP) TCHARs, not host UTF-8 bytes.
    const std::string* path = mm.module_path(m.arg_ptr(0));
    if (path == nullptr) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        m.set_return(0);
        return;
    }
    const std::string acp = utf8_to_acp(*path);
    const std::vector<u8> name(acp.begin(), acp.end());
    m.set_return(store_module_file_name(
        name, static_cast<u32>(m.arg(2)), m.arg_ptr(1),
        [&m](GuestAddr base, u32 i, u8 unit) { (void)m.write_u8(base + i, unit); }));
}

void impl_GetModuleFileNameW(Marshaler& m, ModuleManager& mm) {
    // DWORD GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize)
    const std::string* path = mm.module_path(m.arg_ptr(0));
    if (path == nullptr) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        m.set_return(0);
        return;
    }
    // Length and truncation are in UTF-16 units, not UTF-8 bytes.
    constexpr u32 kCpUtf8 = 65001;
    const auto* bytes = reinterpret_cast<const u8*>(path->data());
    std::vector<u16> name(codepage::multi_byte_to_wide(
                              kCpUtf8, 0, bytes, path->size(), nullptr, 0).count);
    if (!name.empty()) {
        (void)codepage::multi_byte_to_wide(kCpUtf8, 0, bytes, path->size(),
                                           name.data(), name.size());
    }
    m.set_return(store_module_file_name(
        name, static_cast<u32>(m.arg(2)), m.arg_ptr(1),
        [&m](GuestAddr base, u32 i, u16 unit) { (void)m.write_u16(base + i * 2u, unit); }));
}

// =============================================================================
// Export Table Factory
// =============================================================================

DllExports make_module_exports(ModuleManager& mm) {
    DllExports exports;
    exports.entries.push_back({"IsApiSetImplemented", 0, [](Marshaler& m) {
        const auto contract = m.read_ansi_string(m.arg_ptr(0));
        if (contract.is_err()) { m.set_return(0); return; }
        if constexpr (pe::certified_api_sets.empty()) {
            static std::once_flag unavailable;
            std::call_once(unavailable, [] {
                std::fprintf(stderr, "[vitreo] UNIMPLEMENTED kernel32!IsApiSetImplemented: complete API-set availability certification is pending\n");
            });
        }
        // Neither success nor absence changes the caller's LastError.
        m.set_return(pe::is_api_set_implemented(contract.value()) ? 1u : 0u);
    }, 4});

    exports.entries.push_back({VITREO_API_SET_PRESENCE_EXPORT, 0, [&mm](Marshaler& m) {
        // Learn declares BOOL, but both real Windows bitnesses return raw
        // STATUS_SUCCESS (zero) and write a single BOOLEAN. Undocumented
        // validation/fault rules: apiset_presence_probe{32,64}.expected.
        const GuestAddr descriptor = m.arg_ptr(0);
        const GuestAddr present = m.arg_ptr(1);
        const auto length = m.read_u16(descriptor);
        if (length.is_err()) { m.raise_access_violation(false, descriptor); return; }
        const usize units = length.value() / sizeof(u16);
        std::string contract;
        bool ascii = true;
        // Windows short names (<=4 WCHARs) do not dereference Buffer.
        if (units > 4) {
            // SDK Winternl.h UNICODE_STRING: USHORT Length, MaximumLength,
            // pointer-aligned PWSTR Buffer. MaximumLength is not consulted.
            struct Descriptor32 { u16 length, maximum; u32 buffer; };
            struct Descriptor64 { u16 length, maximum; alignas(u64) u64 buffer; };
            const usize offset = m.cpu_is_64bit() ? offsetof(Descriptor64, buffer)
                                                 : offsetof(Descriptor32, buffer);
            const auto buffer = m.read_ptr(descriptor + offset);
            if (buffer.is_err()) { m.raise_access_violation(false, descriptor + offset); return; }
            contract.reserve(units);
            for (usize i = 0; i < units; ++i) {
                const GuestAddr address = buffer.value() + i * sizeof(u16);
                const auto ch = m.read_u16(address);
                if (ch.is_err()) { m.raise_access_violation(false, address); return; }
                if (ch.value() > 0x7f) ascii = false;
                contract.push_back(static_cast<char>(ch.value() & 0x7f));
            }
        }
        const auto host = ascii ? pe::api_set_namespace_host(contract) : std::string_view{};
        const u8 value = !host.empty() && mm.registry().has_dll(host) ? 1u : 0u;
        if (m.write_u8(present, value).is_err()) {
            m.raise_access_violation(true, present); return;
        }
        // No success, absence, or fault path changes LastError.
        m.set_return(0);
    }, 8});

    exports.entries.reserve(11);

    // GetModuleHandleA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetModuleHandleA", 1,
        [&mm](Marshaler& m) { impl_GetModuleHandleA(m, mm); },
        4
    });
    // GetModuleHandleW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "GetModuleHandleW", 2,
        [&mm](Marshaler& m) { impl_GetModuleHandleW(m, mm); },
        4
    });
    // GetProcAddress (2 args, stdcall_bytes=8)
    exports.entries.push_back(ExportEntry{
        "GetProcAddress", 3,
        [&mm](Marshaler& m) { impl_GetProcAddress(m, mm); },
        8
    });
    // LoadLibraryA (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LoadLibraryA", 4,
        [&mm](Marshaler& m) { impl_LoadLibraryA(m, mm); },
        4
    });
    // LoadLibraryW (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "LoadLibraryW", 5,
        [&mm](Marshaler& m) { impl_LoadLibraryW(m, mm); },
        4
    });
    // FreeLibrary (1 arg, stdcall_bytes=4)
    exports.entries.push_back(ExportEntry{
        "FreeLibrary", 6,
        [&mm](Marshaler& m) { impl_FreeLibrary(m, mm); },
        4
    });
    // GetModuleFileNameA (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetModuleFileNameA", 7,
        [&mm](Marshaler& m) { impl_GetModuleFileNameA(m, mm); },
        12
    });
    // GetModuleFileNameW (3 args, stdcall_bytes=12)
    exports.entries.push_back(ExportEntry{
        "GetModuleFileNameW", 8,
        [&mm](Marshaler& m) { impl_GetModuleFileNameW(m, mm); },
        12
    });
    exports.entries.push_back(ExportEntry{
        "DisableThreadLibraryCalls", 9,
        [&mm](Marshaler& m) { impl_DisableThreadLibraryCalls(m, mm); },
        4
    });

    return exports;
}

} // namespace vitreo::winapi

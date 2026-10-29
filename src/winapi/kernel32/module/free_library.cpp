/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/kernel32/thread/thread_suspend.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
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

/// @file free_library.cpp
/// @brief ModuleManager::free_library
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

namespace {

/// True when a guest thread other than the caller is still inside [base, end).
/// RIP is the strong signal. A thread blocked in a host call has its return
/// address on the stack, so a short stack walk covers that too. The image base
/// itself is ignored: it is the HMODULE and is often spilled by the caller.
bool thread_still_inside_image(GuestAddr base, GuestAddr end) {
    const u32 self = ThreadManager::get_current_thread_id_static();
    const auto samples = ThreadSuspendRegistry::instance().sample_threads();
    GuestMemory* memory = ThreadSuspendRegistry::instance().guest_memory();
    for (const auto& sample : samples) {
        if (sample.tid == self) continue;
        const auto inside = [&](u64 addr) {
            return addr > base && addr < end;
        };
        if (inside(sample.rip)) return true;
        if (memory == nullptr || sample.sp == 0) continue;
        const u64 sp = sample.is_64bit ? sample.sp : (sample.sp & 0xffffffffu);
        const int slots = sample.is_64bit ? 48 : 24;
        const u64 step = sample.is_64bit ? 8u : 4u;
        for (int i = 0; i < slots; ++i) {
            u64 value = 0;
            if (sample.is_64bit) {
                const auto word = memory->read_u64(
                    static_cast<GuestAddr>(sp + static_cast<u64>(i) * step));
                if (!word.is_ok()) break;
                value = word.value();
            } else {
                const auto word = memory->read_u32(
                    static_cast<GuestAddr>(sp + static_cast<u64>(i) * step));
                if (!word.is_ok()) break;
                value = word.value();
            }
            if (inside(value)) return true;
        }
    }
    return false;
}

}  // namespace

bool ModuleManager::free_library(GuestAddr module_base) {
    // Learn (FreeLibrary / DllMain): DLL_PROCESS_DETACH runs under the loader
    // lock, and an unload is a loader transaction like a load. Without it two
    // FreeLibrary calls, or FreeLibrary against a concurrent LoadLibrary, were
    // unserialized writers of the module tables.
    std::lock_guard<std::recursive_mutex> loader_guard(m_loader_mutex);
    // LoadLibraryEx AS_DATAFILE / AS_IMAGE_RESOURCE: FreeLibrary destroys the
    // mapping without touching any executable-module reference count (Learn).
    {
        auto df = data_file_mappings_.find(module_base);
        if (df != data_file_mappings_.end()) {
            if (m_pe_loader) {
                pe::LoadedImage img{};
                img.image_base = df->second.base;
                img.image_end = df->second.image_end;
                img.sections = df->second.sections;
                img.header_size = df->second.header_size;
                m_pe_loader->unload(img);
            }
            std::unique_lock<HostRwLock> tables(m_tables_mutex);
            data_file_mappings_.erase(df);
            return true;
        }
    }

    const std::string* dll_name = name_for_base(module_base);
    if (!dll_name) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        return false;
    }

    auto it = modules_by_name_.find(*dll_name);
    if (it == modules_by_name_.end()) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        return false;
    }

    // GetModuleHandleEx(PIN) makes the module immune to FreeLibrary until
    // process termination.
    if (it->second.pinned) {
        return true;
    }

    if (module_diag_enabled()) {
        fprintf(stderr,
                "[module] FreeLibrary \"%s\" @ 0x%llx ref_count=%u pe_backed=%d\n",
                it->second.name.c_str(),
                static_cast<unsigned long long>(module_base),
                it->second.ref_count,
                it->second.is_pe_backed() ? 1 : 0);
    }

    // Already unloaded — Learn FreeLibrary fails on an invalid handle
    // (loader_contract_probe.expected FreeLibrary#2 stale → err=7e).
    if (it->second.ref_count == 0) {
        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        return false;
    }
    if (it->second.ref_count > 1) {
        --it->second.ref_count;
        return true;
    }

    // Thunk-backed system DLLs stay loaded for the process lifetime. FreeLibrary
    // succeeds but does not drop them below one reference (GetModuleHandle still
    // resolves kernel32 after FreeLibrary — FreeLibrarySystemDllStaysAlive).
    if (!it->second.is_pe_backed()) {
        return true;
    }

    // Refcount is 1 on a PE-backed module — notify detach then unload.
    if (m_guest_call && it->second.entry_point != 0 && it->second.dllmain_called) {
        constexpr u64 DLL_PROCESS_DETACH = 0;
        u64 args[3] = {
            static_cast<u64>(it->second.base_address),
            DLL_PROCESS_DETACH,
            0,
        };
        // Logged at the same volume as the DLL_PROCESS_ATTACH sites. A detach
        // runs the guest CRT's uninitializers, which tears down process-wide
        // CRT state (lowio's __pioinfo, locale, cached module handles); a
        // later attach that reuses that state faults. Without this line the
        // teardown is invisible in a loader trace and looks like memory being
        // clobbered by an unrelated path.
        if (module_diag_enabled()) {
            fprintf(stderr,
                    "[loader] DllMain BEGIN \"%s\" entry=0x%llx reason=DETACH\n",
                    it->second.name.c_str(),
                    static_cast<unsigned long long>(it->second.entry_point));
        }
        (void)m_guest_call(it->second.entry_point, args, 3, it->second.bitness == 64);
        if (module_diag_enabled()) {
            fprintf(stderr,
                    "[loader] DllMain END   \"%s\" reason=DETACH\n",
                    it->second.name.c_str());
        }
        it->second.dllmain_called = false;
    }

    // Another thread is still executing this image. Unmapping now is a
    // use-after-free: the thread's next IAT load faults and the process
    // dies.
    // Windows joins that thread inside DLL_PROCESS_DETACH before LdrUnloadDll.
    // When the thread is still inside after detach returns, keep the mapping
    // and pin the module so a later FreeLibrary cannot tear it out either.
    const GuestAddr image_base = it->second.base_address;
    const GuestAddr image_end = it->second.image_end;
    if (image_end > image_base && thread_still_inside_image(image_base, image_end)) {
        fprintf(stderr,
                "[module] FreeLibrary \"%s\" @ 0x%llx kept mapped: "
                "another thread is still executing it\n",
                it->second.name.c_str(),
                static_cast<unsigned long long>(image_base));
        it->second.pinned = true;
        return true;
    }

    notify_dll(2, it->second.path, it->second.name,
               it->second.base_address,
               static_cast<usize>(it->second.image_end -
                                  it->second.base_address));
    unpublish_loader_entry(it->second);
    // The loader owns static PE TLS.  Its index and each thread's data
    // block cease to exist with the loaded image instance, not on a later
    // LoadLibrary that happens to reuse the same base.
    release_dll_tls_slots_in_range(it->second.base_address,
                                   it->second.image_end);
    if (m_pe_loader) {
        pe::LoadedImage unload_img{};
        unload_img.image_base = it->second.base_address;
        unload_img.image_end = it->second.image_end;
        unload_img.sections = it->second.mapped_sections;
        unload_img.header_size = it->second.mapped_header_size;
        m_pe_loader->unload(unload_img);
    }
    const GuestAddr base = it->second.base_address;
    {
        std::unique_lock<HostRwLock> tables(m_tables_mutex);
        base_to_name_.erase(base);
        modules_by_name_.erase(it);
    }
    return true;
}

} // namespace vitreo::winapi

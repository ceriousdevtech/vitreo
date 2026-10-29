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

/// @file lookup_runtime_function_entry.cpp
/// @brief ModuleManager::lookup_runtime_function_entry
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

namespace {
thread_local const ModuleManager::RuntimeFunctionCallbackRunner*
    t_callback_runner = nullptr;
}  // namespace

ModuleManager::ScopedThreadCallbackRunner::ScopedThreadCallbackRunner(
    const RuntimeFunctionCallbackRunner* runner) noexcept
    : previous_{t_callback_runner} {
    t_callback_runner = runner;
}

ModuleManager::ScopedThreadCallbackRunner::~ScopedThreadCallbackRunner() {
    t_callback_runner = previous_;
}


bool ModuleManager::add_dynamic_function_table(GuestAddr table, u32 count,
                                              GuestAddr base) {
    if (table == 0 || count == 0) return false;
    std::lock_guard<std::mutex> lock(dynamic_tables_mutex_);
    DynamicFunctionTable entry{};
    entry.identifier = table;
    entry.base = base;
    entry.table = table;
    entry.count = count;
    dynamic_tables_.push_back(entry);
    return true;
}

bool ModuleManager::add_dynamic_function_table_callback(
    GuestAddr identifier, GuestAddr base, u32 length, GuestAddr callback,
    GuestAddr context) {
    // Microsoft Learn, RtlInstallFunctionTableCallback: the low two bits of
    // TableIdentifier must be set. That is how the unwinder tells a callback
    // registration from a plain table pointer, which is always aligned.
    if (callback == 0 || length == 0) return false;
    if ((identifier & 0x3u) != 0x3u) return false;
    std::lock_guard<std::mutex> lock(dynamic_tables_mutex_);
    DynamicFunctionTable entry{};
    entry.identifier = identifier;
    entry.base = base;
    entry.length = length;
    entry.callback = callback;
    entry.context = context;
    dynamic_tables_.push_back(entry);
    return true;
}

bool ModuleManager::add_growable_function_table(GuestAddr table, u32 count,
                                               GuestAddr range_base,
                                               GuestAddr range_end) {
    // A growable table starts empty legitimately: coreclr registers the range
    // for a code heap before it has JIT-compiled anything into it, then calls
    // RtlGrowFunctionTable as methods appear. Refusing count == 0 here (as
    // add_dynamic_function_table does, where it means "nothing to find") would
    // reject exactly the registration that matters.
    if (table == 0 || range_end <= range_base) return false;
    std::lock_guard<std::mutex> lock(dynamic_tables_mutex_);
    DynamicFunctionTable entry{};
    entry.identifier = table;
    entry.base = range_base;
    entry.length = range_end - range_base;
    entry.table = table;
    entry.count = count;
    dynamic_tables_.push_back(entry);
    return true;
}

bool ModuleManager::grow_dynamic_function_table(GuestAddr identifier,
                                                u32 new_count) {
    std::lock_guard<std::mutex> lock(dynamic_tables_mutex_);
    for (auto& entry : dynamic_tables_) {
        if (entry.identifier != identifier || entry.table == 0) continue;
        // Windows only ever grows a table; a smaller count would hide entries
        // the unwinder may already have handed out.
        if (new_count > entry.count) entry.count = new_count;
        return true;
    }
    return false;
}

bool ModuleManager::remove_dynamic_function_table(GuestAddr id) {
    std::lock_guard<std::mutex> lock(dynamic_tables_mutex_);
    const auto before = dynamic_tables_.size();
    dynamic_tables_.erase(
        std::remove_if(dynamic_tables_.begin(), dynamic_tables_.end(),
                       [id](const DynamicFunctionTable& e) {
                           return e.identifier == id;
                       }),
        dynamic_tables_.end());
    return dynamic_tables_.size() != before;
}

GuestAddr ModuleManager::lookup_dynamic_function_entry(
    GuestAddr control_pc, GuestAddr* image_base_out) const {
    if (m_memory == nullptr) return 0;

    // Copy the matching registration before releasing the lock: a callback
    // runs GUEST code, which may itself register or delete a table, and
    // holding the lock across that would deadlock or invalidate the iterator.
    DynamicFunctionTable match{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(dynamic_tables_mutex_);
        for (const auto& e : dynamic_tables_) {
            if (e.callback != 0) {
                if (control_pc >= e.base && control_pc < e.base + e.length) {
                    match = e;
                    found = true;
                    break;
                }
            } else if (e.table != 0) {
                // A growable table (RtlAddGrowableFunctionTable) states its
                // range explicitly; honour it, so a PC outside the registered
                // code heap cannot match a stale entry that happens to cover
                // the same RVA under a different base. A plain
                // RtlAddFunctionTable registration leaves length 0 and is
                // bounded only by its entries.
                if (e.length != 0 &&
                    (control_pc < e.base || control_pc >= e.base + e.length)) {
                    continue;
                }
                // Check the entries directly while holding the lock -- no
                // guest code runs for this case.
                for (u32 i = 0; i < e.count; ++i) {
                    const GuestAddr rf = e.table + static_cast<GuestAddr>(i) * 12u;
                    const auto begin_r = m_memory->read_u32(rf + 0);
                    const auto end_r = m_memory->read_u32(rf + 4);
                    if (begin_r.is_err() || end_r.is_err()) break;
                    const GuestAddr fn_begin = e.base + begin_r.value();
                    const GuestAddr fn_end = e.base + end_r.value();
                    if (control_pc >= fn_begin && control_pc < fn_end) {
                        if (image_base_out != nullptr) *image_base_out = e.base;
                        return rf;
                    }
                }
            }
        }
    }
    if (!found) return 0;

    const RuntimeFunctionCallbackRunner& runner =
        t_callback_runner != nullptr ? *t_callback_runner
                                     : runtime_function_callback_runner_;
    if (!runner) return 0;
    const GuestAddr rf = runner(match.callback, control_pc, match.context);    if (rf != 0 && image_base_out != nullptr) *image_base_out = match.base;
    return rf;
}

GuestAddr ModuleManager::lookup_runtime_function_entry(GuestAddr control_pc,
                                                       GuestAddr* image_base_out) const {
    if (m_memory == nullptr) {
        return 0;
    }

    // A ControlPc inside a loaded image is answered by that image's table
    // alone. Measured on Windows (tools/oracle_probe/
    // function_table_precedence_probe.c): a dynamic table or callback whose
    // range also covers the image is never consulted for such a PC, even when
    // the image has no entry for it. Mono registers one callback over a range
    // spanning all of its code chunks, which can enclose plugin DLLs; asking
    // it first turned their SEH frames into leaves.
    //
    // Resolve that image by address -- scanning every loaded module's .pdata
    // until one happens to match is O(modules x entries) per call, and a
    // ControlPc inside module A can never be described by module B's table.
    if (const ModuleAtAddr at = module_for_address(control_pc);
        at.info != nullptr) {
        // The main image is tracked both by name and in the main_module_*
        // fields; prefer whichever of the two actually carries a parsed
        // exception directory.
        u32 exception_rva = at.info->exception_rva;
        u32 exception_size = at.info->exception_size;
        if (exception_rva == 0 && at.info->base_address == main_module_base_) {
            exception_rva = main_module_exception_rva_;
            exception_size = main_module_exception_size_;
        }
        const GuestAddr entry = lookup_runtime_function_in_table(
            *m_memory,
            at.info->base_address,
            exception_rva,
            exception_size,
            control_pc);
        if (entry != 0 && image_base_out != nullptr) {
            *image_base_out = at.info->base_address;
        }
        // A ControlPc that the containing image's table does not describe is a
        // leaf function: Windows returns NULL rather than consulting another
        // image.
        return entry;
    }

    // Outside every image: JIT-generated code, described only by the dynamic
    // tables its generator registered.
    if (const GuestAddr dynamic_entry =
            lookup_dynamic_function_entry(control_pc, image_base_out);
        dynamic_entry != 0) {
        return dynamic_entry;
    }

    // No PE-backed module covers the PC (the main image is not always
    // registered by name, and synthetic modules are skipped above).
    if (main_module_base_ != 0) {
        if (GuestAddr entry = lookup_runtime_function_in_table(
                *m_memory,
                main_module_base_,
                main_module_exception_rva_,
                main_module_exception_size_,
                control_pc);
            entry != 0) {
            if (image_base_out != nullptr) {
                *image_base_out = main_module_base_;
            }
            return entry;
        }
    }

    // RtlLookupFunctionEntry runs on every x64 exception dispatch and stack
    // walk, on any thread, while LoadLibrary rehashes the table.
    std::shared_lock<HostRwLock> tables(m_tables_mutex);
    for (const auto& [name, info] : modules_by_name_) {
        (void)name;
        if (!info.is_pe_backed()) {
            continue;
        }
        if (GuestAddr entry = lookup_runtime_function_in_table(
                *m_memory,
                info.base_address,
                info.exception_rva,
                info.exception_size,
                control_pc);
            entry != 0) {
            if (image_base_out != nullptr) {
                *image_base_out = info.base_address;
            }
            return entry;
        }
    }

    return 0;
}

} // namespace vitreo::winapi

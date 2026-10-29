/// @file import_graph.cpp
/// @brief Circular static imports: register-before-bind and cycle-deferred
///        DLL_PROCESS_ATTACH.
///
/// Contract (PE/COFF specification, "The .idata Section"; Microsoft Learn,
/// "Dynamic-Link Library Initialization" and "DllMain entry point"):
///   * A module's import table names other modules; nothing forbids those
///     modules from importing back. The loader maps a module and inserts it
///     into the process module list before it snaps that module's imports,
///     so a back-reference binds to the already-mapped image's exports.
///   * Import binding for the whole set of modules brought in by one load
///     completes before any of their initializers run; initializers then run
///     dependencies first. Within a cycle there is no "dependency first", so
///     the member that completes binding first initializes first.
///   * If an initializer fails, the load fails with ERROR_DLL_INIT_FAILED and
///     the modules it brought in are unloaded.
///
/// Acyclic loads keep their existing order unchanged: each module finishes
/// binding only after its whole dependency subtree has bound and attached.

#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"

#include <cstdio>

namespace vitreo::winapi {

namespace {
constexpr usize kNoCycle = static_cast<usize>(-1);
}  // namespace

void ModuleManager::register_mapping_module(const std::string& cache_key,
                                            const std::string& name,
                                            const std::string& path,
                                            const pe::ParsedPe& parsed,
                                            std::span<const u8> pe_data,
                                            GuestAddr image_base,
                                            usize image_size) {
    ModuleInfo info;
    info.base_address = image_base;
    info.image_end = image_base + image_size;
    info.entry_point = parsed.entry_point_rva != 0
                           ? image_base + parsed.entry_point_rva
                           : 0;
    info.bitness = parsed.is_64bit ? 64u : 32u;
    info.name = name;
    info.path = path;
    info.ref_count = 1;
    info.state = ModuleInfo::State::Mapping;
    if (parsed.num_data_directories > pe::DIR_EXCEPTION) {
        info.exception_rva =
            parsed.data_directories[pe::DIR_EXCEPTION].VirtualAddress;
        info.exception_size = parsed.data_directories[pe::DIR_EXCEPTION].Size;
    }
    if (parsed.num_data_directories > pe::DIR_TLS) {
        const auto& tls_dd = parsed.data_directories[pe::DIR_TLS];
        if (tls_dd.Size != 0) info.tls_dir_rva = tls_dd.VirtualAddress;
    }
    if (parsed.num_data_directories > pe::DIR_RESOURCE) {
        const auto& rsrc_dd = parsed.data_directories[pe::DIR_RESOURCE];
        info.resource_rva = rsrc_dd.VirtualAddress;
        info.resource_size = rsrc_dd.Size;
    }
    auto ex = parse_exports_from_file(pe_data, parsed, image_base);
    info.exports_by_name = std::move(ex.by_name);
    info.exports_by_ordinal = std::move(ex.by_ordinal);
    info.forwarders_by_name = std::move(ex.forwarders_by_name);
    info.forwarders_by_ordinal = std::move(ex.forwarders_by_ordinal);

    {
        std::unique_lock<HostRwLock> tables(m_tables_mutex);
        base_to_name_[image_base] = cache_key;
        modules_by_name_[cache_key] = std::move(info);
    }
    record_module_load_order(cache_key);
    m_binding_stack.push_back(cache_key);
}

void ModuleManager::discard_mapping_module(const std::string& cache_key) {
    for (usize i = m_binding_stack.size(); i-- > 0;) {
        if (m_binding_stack[i] == cache_key) {
            m_binding_stack.erase(m_binding_stack.begin() +
                                  static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (m_cycle_root_depth != kNoCycle &&
        m_cycle_root_depth >= m_binding_stack.size()) {
        // The cycle root itself failed to bind. The members it deferred were
        // bound against an image that is going away and never attached, so
        // they are unloaded with it, as Windows unloads a failed load's graph.
        m_cycle_root_depth = kNoCycle;
        std::vector<DeferredCycleInit> orphaned;
        orphaned.swap(m_deferred_cycle_inits);
        for (const DeferredCycleInit& item : orphaned) {
            unload_failed_module(item.key, /*call_detach=*/false);
        }
    }
    std::unique_lock<HostRwLock> tables(m_tables_mutex);
    auto it = modules_by_name_.find(cache_key);
    if (it == modules_by_name_.end()) return;
    auto bit = base_to_name_.find(it->second.base_address);
    if (bit != base_to_name_.end() && bit->second == cache_key) {
        base_to_name_.erase(bit);
    }
    modules_by_name_.erase(it);
}

GuestAddr ModuleManager::in_progress_module_base(std::string_view dll_name) {
    const std::string key = normalize_name(dll_name);
    const ModuleInfo* match = nullptr;
    std::string match_key;
    auto in_progress = [](const ModuleInfo& info) {
        return info.state == ModuleInfo::State::Mapping ||
               info.state == ModuleInfo::State::Initializing;
    };
    auto it = modules_by_name_.find(key);
    if (it != modules_by_name_.end() && in_progress(it->second)) {
        match = &it->second;
        match_key = it->first;
    } else {
        for (const auto& [loaded_key, info] : modules_by_name_) {
            if (in_progress(info) && normalized_name_equals(info.name, key)) {
                match = &info;
                match_key = loaded_key;
                break;
            }
        }
    }
    if (match == nullptr) return 0;

    if (match->state == ModuleInfo::State::Mapping) {
        for (usize i = 0; i < m_binding_stack.size(); ++i) {
            if (m_binding_stack[i] == match_key) {
                if (m_cycle_root_depth == kNoCycle || i < m_cycle_root_depth) {
                    m_cycle_root_depth = i;
                }
                break;
            }
        }
    }
    return match->base_address;
}

bool ModuleManager::finish_module_binding(const std::string& cache_key) {
    usize depth = kNoCycle;
    for (usize i = m_binding_stack.size(); i-- > 0;) {
        if (m_binding_stack[i] == cache_key) {
            depth = i;
            m_binding_stack.erase(m_binding_stack.begin() +
                                  static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    auto it = modules_by_name_.find(cache_key);
    if (it != modules_by_name_.end() &&
        it->second.state == ModuleInfo::State::Mapping) {
        it->second.state = ModuleInfo::State::Initializing;
    }
    if (depth == kNoCycle || m_cycle_root_depth == kNoCycle) return false;
    if (depth > m_cycle_root_depth) return true;
    // This module is the cycle root: its imports are the last of the cycle to
    // be bound. It initializes after the members it deferred.
    m_cycle_root_depth = kNoCycle;
    return false;
}

void ModuleManager::defer_cycle_init(const std::string& cache_key,
                                     bool call_entry) {
    m_deferred_cycle_inits.push_back(DeferredCycleInit{cache_key, call_entry});
}

void ModuleManager::unload_failed_module(const std::string& cache_key,
                                         bool call_detach) {
    auto it = modules_by_name_.find(cache_key);
    if (it == modules_by_name_.end()) return;
    ModuleInfo& failed = it->second;
    if (call_detach && m_guest_call && failed.entry_point != 0) {
        constexpr u64 DLL_PROCESS_DETACH = 0;
        u64 dargs[3] = {static_cast<u64>(failed.base_address),
                        DLL_PROCESS_DETACH, 0};
        (void)m_guest_call(failed.entry_point, dargs, 3, failed.bitness == 64);
    }
    unpublish_loader_entry(failed);
    if (m_pe_loader) {
        pe::LoadedImage unload_img{};
        unload_img.image_base = failed.base_address;
        unload_img.image_end = failed.image_end;
        unload_img.sections = failed.mapped_sections;
        unload_img.header_size = failed.mapped_header_size;
        m_pe_loader->unload(unload_img);
    }
    {
        std::unique_lock<HostRwLock> tables(m_tables_mutex);
        base_to_name_.erase(failed.base_address);
        modules_by_name_.erase(it);
    }
}

bool ModuleManager::run_deferred_cycle_inits(const std::string& root_key) {
    // Only the members deferred while this root was binding belong to it;
    // an outer cycle (lower root) cannot be pending, because a deferred
    // module only exists while its root is still on the binding stack and
    // this root is the lowest.
    std::vector<DeferredCycleInit> pending;
    pending.swap(m_deferred_cycle_inits);

    std::vector<std::string> attached;
    bool ok = true;
    usize next = 0;
    for (; next < pending.size(); ++next) {
        const DeferredCycleInit& item = pending[next];
        auto it = modules_by_name_.find(item.key);
        if (it == modules_by_name_.end()) continue;
        ModuleInfo& info = it->second;
        if (!m_guest_call || !item.call_entry || info.entry_point == 0) {
            info.state = ModuleInfo::State::Loaded;
            continue;
        }
        constexpr u64 DLL_PROCESS_ATTACH = 1;
        const GuestAddr base = info.base_address;
        const u32 bitness = info.bitness;
        const GuestAddr entry = info.entry_point;
        const std::string path = info.path;
        if (info.tls_dir_rva != 0) {
            const GuestAddr tls_va =
                static_cast<GuestAddr>(base + info.tls_dir_rva);
            setup_dll_tls(base, bitness, tls_va, path.c_str());
            fire_tls_callbacks(base, bitness, tls_va, DLL_PROCESS_ATTACH,
                               path.c_str());
        }
        u64 args[3] = {static_cast<u64>(base), DLL_PROCESS_ATTACH, 0};
        if (module_diag_enabled()) {
            std::fprintf(stderr,
                         "[loader] DllMain BEGIN \"%s\" entry=0x%llx "
                         "(import cycle)\n",
                         path.c_str(), static_cast<unsigned long long>(entry));
        }
        const u64 ret = m_guest_call(entry, args, 3, bitness == 64);
        if (module_diag_enabled()) {
            std::fprintf(stderr,
                         "[loader] DllMain END   \"%s\" returned 0x%llx\n",
                         path.c_str(), static_cast<unsigned long long>(ret));
        }
        auto again = modules_by_name_.find(item.key);
        if (again == modules_by_name_.end()) continue;
        again->second.dllmain_called = true;
        if (ret == 0) {
            std::fprintf(stderr,
                         "[loader] DllMain returned FALSE for \"%s\" inside an "
                         "import cycle - failing load of \"%s\"\n",
                         path.c_str(), root_key.c_str());
            unload_failed_module(item.key, /*call_detach=*/true);
            ok = false;
            ++next;
            break;
        }
        again->second.state = ModuleInfo::State::Loaded;
        attached.push_back(item.key);
    }
    if (ok) return true;

    // Learn "Dynamic-Link Library Initialization": a failed initializer fails
    // the load and the modules it brought in are unloaded — attached ones
    // receive DLL_PROCESS_DETACH, never-attached ones do not. Like a plain
    // DllMain FALSE, the failure is not cached: a later load retries.
    for (usize i = attached.size(); i-- > 0;) {
        unload_failed_module(attached[i], /*call_detach=*/true);
    }
    for (; next < pending.size(); ++next) {
        unload_failed_module(pending[next].key, /*call_detach=*/false);
    }
    unload_failed_module(root_key, /*call_detach=*/false);
    set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
    return false;
}

}  // namespace vitreo::winapi

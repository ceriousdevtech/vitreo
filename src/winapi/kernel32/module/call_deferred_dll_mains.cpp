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

/// @file call_deferred_dll_mains.cpp
/// @brief ModuleManager::call_deferred_dll_mains
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

void ModuleManager::call_deferred_dll_mains() {
    if (!m_guest_call) return;

    // ------------------------------------------------------------------
    // Parse PE imports from the in-memory image so we can call dependency
    // DllMains *before* dependents. Otherwise a dependent's DllMain can
    // run before the module it imports has initialized, and a null vtable
    // call faults.
    // ------------------------------------------------------------------
    auto parse_imports = [&](ModuleInfo& info) {
        if (info.imports_parsed) return;
        info.imports_parsed = true;
        if (!m_memory) return;
        if (!info.is_pe_backed()) return;

        const GuestAddr base = info.base_address;
        // IMAGE_DOS_HEADER.e_lfanew at base+0x3c
        auto e_lfanew = m_memory->read_u32(base + 0x3c);
        if (e_lfanew.is_err()) return;
        const u64 nt = base + e_lfanew.value();
        // Signature "PE\0\0" at nt + 0
        auto sig = m_memory->read_u32(nt);
        if (sig.is_err() || sig.value() != 0x00004550u) return;
        // Magic at nt + 24 (start of optional header)
        auto magic = m_memory->read_u16(nt + 24);
        if (magic.is_err()) return;
        const bool pe32plus = (magic.value() == 0x20b);
        // DataDirectory base: optional_header + (0x60 for PE32, 0x70 for PE32+)
        const u64 dd_base = nt + 24 + (pe32plus ? 0x70 : 0x60);
        // Entry 1 = Import Table; 8 bytes (RVA + size)
        auto imp_rva = m_memory->read_u32(dd_base + 1 * 8 + 0);
        auto imp_sz  = m_memory->read_u32(dd_base + 1 * 8 + 4);
        if (imp_rva.is_err() || imp_sz.is_err()) return;
        if (imp_rva.value() == 0 || imp_sz.value() == 0) return;

        // Walk IMAGE_IMPORT_DESCRIPTORs (20 bytes each, terminated by all-zero).
        GuestAddr desc = base + imp_rva.value();
        for (int i = 0; i < 256; ++i) {  // safety cap
            auto orig_thunk = m_memory->read_u32(desc + 0);
            auto name_rva   = m_memory->read_u32(desc + 12);
            auto first_thk  = m_memory->read_u32(desc + 16);
            if (orig_thunk.is_err() || name_rva.is_err() || first_thk.is_err()) break;
            if (orig_thunk.value() == 0 && name_rva.value() == 0 && first_thk.value() == 0) break;
            if (name_rva.value() == 0) { desc += 20; continue; }
            // Read null-terminated DLL name at base + name_rva
            std::string n;
            GuestAddr p = base + name_rva.value();
            for (int j = 0; j < 256; ++j) {
                auto c = m_memory->read_u8(p + j);
                if (c.is_err() || c.value() == 0) break;
                n.push_back(static_cast<char>(c.value()));
            }
            if (!n.empty()) {
                // Lowercase + strip .dll suffix to match modules_by_name_ keys.
                for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (n.size() > 4 && n.compare(n.size() - 4, 4, ".dll") == 0) {
                    n.resize(n.size() - 4);
                }
                info.imports.push_back(std::move(n));
            }
            desc += 20;
        }
    };

    // Recursive worker: invoke DllMains in topological order.
    std::function<void(const std::string&, std::unordered_set<std::string>&)> visit;
    visit = [&](const std::string& key, std::unordered_set<std::string>& on_stack) {
        auto it = modules_by_name_.find(key);
        if (it == modules_by_name_.end()) return;
        ModuleInfo& info = it->second;
        if (info.dllmain_called) return;
        if (!info.is_pe_backed() || info.entry_point == 0) return;
        if (!on_stack.insert(key).second) return;  // cycle: break

        parse_imports(info);
        // Snapshot deps before recursing (recursion may rehash modules_by_name_
        // if a load is triggered, invalidating `info`).
        std::vector<std::string> deps = info.imports;
        // Build a basename->key index so an explicit-path key matches a
        // dependency basename.
        auto resolve_dep_key = [&](const std::string& dep) -> std::string {
            // Direct hit (key already a basename)
            if (modules_by_name_.find(dep) != modules_by_name_.end()) return dep;
            // Fallback: scan and compare normalized info.name.
            for (const auto& [k, mi] : modules_by_name_) {
                if (normalized_name_equals(mi.name, dep)) return k;
            }
            return {};
        };
        for (const auto& dep : deps) {
            std::string dep_key = resolve_dep_key(dep);
            if (!dep_key.empty()) {
                visit(dep_key, on_stack);
            }
        }

        // Re-fetch in case the map rehashed during recursion.
        auto it2 = modules_by_name_.find(key);
        if (it2 == modules_by_name_.end()) { on_stack.erase(key); return; }
        ModuleInfo& info2 = it2->second;
        if (info2.dllmain_called) { on_stack.erase(key); return; }

        constexpr u64 DLL_PROCESS_ATTACH = 1;
        if (info2.tls_dir_rva != 0) {
            setup_dll_tls(
                info2.base_address,
                info2.bitness,
                static_cast<GuestAddr>(info2.base_address + info2.tls_dir_rva),
                info2.path.c_str());
            fire_tls_callbacks(
                info2.base_address,
                info2.bitness,
                static_cast<GuestAddr>(info2.base_address + info2.tls_dir_rva),
                DLL_PROCESS_ATTACH,
                info2.path.c_str());
        }
        u64 args[3] = {
            static_cast<u64>(info2.base_address),
            DLL_PROCESS_ATTACH,
            1,  // Non-null for DLLs loaded as part of process initialization.
        };
        const bool dllmain_diag = std::getenv("VITREO_DLLMAIN_DIAG") != nullptr;
        if (dllmain_diag) {
            std::fprintf(
                stderr,
                "[loader] Deferred DllMain BEGIN \"%s\" entry=0x%llx\n",
                info2.path.c_str(),
                static_cast<unsigned long long>(info2.entry_point));
        }
        const u64 dllmain_result =
            m_guest_call(info2.entry_point, args, 3, info2.bitness == 64);
        if (dllmain_diag) {
            std::fprintf(
                stderr,
                "[loader] Deferred DllMain END   \"%s\" returned 0x%llx\n",
                info2.path.c_str(),
                static_cast<unsigned long long>(dllmain_result));
        }
        // Re-fetch one more time before storing the flag.
        auto it3 = modules_by_name_.find(key);
        if (it3 != modules_by_name_.end()) {
            it3->second.dllmain_called = true;
        }
        on_stack.erase(key);
    };

    // Snapshot keys first so we can iterate without map-rehash hazards.
    std::vector<std::string> keys;
    keys.reserve(modules_by_name_.size());
    for (const auto& [k, _] : modules_by_name_) keys.push_back(k);
    std::unordered_set<std::string> on_stack;
    for (const auto& k : keys) {
        visit(k, on_stack);
    }
}

} // namespace vitreo::winapi

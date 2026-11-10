// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Import setup: CRT data allocation and IAT patching

#include "winapi/msvcrt/runtime/crt_exception_class.hpp"
#include "import_setup.hpp"
#include "runtime/process/runtime_context.hpp"
#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/parser/manifest_info.hpp"
#include "core/pe/imports/import_resolver.hpp"
#include "core/pe/imports/iat_patcher.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/module/module.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/common/sxs/sxs_context.hpp"
#include "capsule/vfs/vfs.hpp"
#include <array>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace vitreo::cli {

std::vector<std::string> main_import_search_directories(
    std::string_view main_module_path,
    std::string_view current_directory,
    std::span<const std::string> private_assemblies) {
    std::vector<std::string> directories;

    const auto separator = main_module_path.find_last_of("\\/");
    if (separator != std::string_view::npos) {
        const std::string application_directory(
            main_module_path.substr(0, separator));
        directories.emplace_back(application_directory);

        // Microsoft Learn, "Assembly Searching Sequence": after the
        // application directory, private side-by-side assemblies are probed
        // in a subdirectory named for the dependent assembly.
        for (const std::string& assembly : private_assemblies) {
            directories.emplace_back(
                application_directory + "\\" + assembly);
        }
    }

    // Microsoft Learn, "Dynamic-Link Library Search Order" (safe search mode,
    // the default since XP SP2): the system directory, the 16-bit system
    // directory and then the Windows directory are searched after the
    // application directory and *before* the current directory. Omitting them
    // means a DLL that lives where an installer put it — a redistributable in
    // System32 is the ordinary case — is never found, and the process dies
    // with STATUS_DLL_NOT_FOUND while the file is sitting right there.
    directories.emplace_back("C:\\Windows\\System32");
    directories.emplace_back("C:\\Windows\\System");
    directories.emplace_back("C:\\Windows");

    auto canonicalize = [](std::string_view path) {
        std::string result(path);
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char value) {
                           if (value == '/') return '\\';
                           return static_cast<char>(std::tolower(value));
                       });
        while (result.size() > 3 && result.back() == '\\') {
            result.pop_back();
        }
        return result;
    };

    if (!current_directory.empty()) {
        const std::string current_key = canonicalize(current_directory);
        const bool duplicate = std::any_of(
            directories.begin(), directories.end(),
            [&](const std::string& directory) {
                return canonicalize(directory) == current_key;
            });
        if (!duplicate) directories.emplace_back(current_directory);
    }
    return directories;
}

RunResult setup_imports(RuntimeContext& ctx,
                        GuestMemory& memory,
                        const pe::ParsedPe& pe,
                        std::span<const u8> pe_data,
                        u64 base_address,
                        bool verbose) {
    // Allocate CRT global data block for data imports (_commode, _fmode, etc.)
    // Pass the actual command line so _acmdln reflects the guest PE name.
    const std::string cmdline_utf8 = ctx.env_mgr.get_command_line_w_utf8();
    // The `_iob` data export's FILE layout follows the image's bitness.
    ctx.crt_data.set_guest_64bit(pe.is_64bit);
    if (!ctx.crt_data.initialize(memory, winapi::CrtDataBlock::DEFAULT_BASE, cmdline_utf8)) {
        return RunResult{1, 0, "Failed to allocate CRT data region", false};
    }

    // Publish the CRT data variables to the thunk registry so that companion
    // DLLs loaded through PeLoader get
    // the same variable addresses the main executable does, instead of a
    // callable missing-export stub they would dereference as data.
    //
    // _HUGE / _wcmdln / _environ are data exports: an IAT slot must hold the
    // address of the variable. Pointing it at a code thunk makes
    // `mov eax,[iat]; mov eax,[eax]` load the thunk's leading NOP dword
    // (0x90909090) and then AV on that address — observed on VC2005 /MD apps.
    for (const char* data_name : {"_commode", "_fmode", "_adjust_fdiv",
                                  "_acmdln", "_wcmdln", "_HUGE",
                                  "__argc", "__argv", "_iob", "_pctype",
                                  "__mb_cur_max", "__initenv", "__winitenv"}) {
        const u64 addr = ctx.crt_data.resolve_data_import(data_name);
        if (addr == 0) continue;
        // Versioned redistributables own their data in their real PE images.
        for (const char* crt_dll : {"msvcrt.dll", "crtdll.dll"}) {
            ctx.thunk_registry.register_data_export(crt_dll, data_name, addr);
        }
    }
    // msvcrt.dll's multibyte table and the VC6 locale globals MSVCP60 reads
    // directly (crt_data.hpp); crtdll.dll exports none of them.
    for (const char* data_name : {"_mbctype", "__lc_codepage", "__lc_collate_cp",
                                  "__lc_handle", "__setlc_active",
                                  "__unguarded_readlc_active"}) {
        if (const u64 addr = ctx.crt_data.resolve_data_import(data_name); addr != 0) {
            ctx.thunk_registry.register_data_export("msvcrt.dll", data_name, addr);
        }
    }

    // std::exception's vftable (??_7exception@@6B@) and its RTTI locator,
    // built over the msvcrt thunks the vftable slots name.
    {
        const GuestAddr vftable = winapi::publish_crt_exception_vftable(
            memory, ctx.thunk_registry, pe.is_64bit,
            ctx.crt_data.base() + winapi::CrtDataBlock::EXCEPTION_RTTI_OFFSET,
            static_cast<u32>(winapi::CrtDataBlock::EXCEPTION_RTTI_SIZE));
        if (vftable != 0) {
            ctx.thunk_registry.register_data_export("msvcrt.dll", "??_7exception@@6B@", vftable);
        }
    }

    // char** _environ lives in the environment manager (rebuilt on demand).
    {
        const u64 environ_addr =
            ctx.crt_environment_mgr.environ_variable_addr(pe.is_64bit);
        if (environ_addr != 0) {
            for (const char* crt_dll : {"msvcrt.dll", "crtdll.dll"}) {
                ctx.thunk_registry.register_data_export(
                    crt_dll, "_environ", environ_addr);
            }
        }
    }

    if (!pe.has_imports()) {
        return RunResult{0, 0, {}, true};
    }

    auto& thunk_registry = ctx.thunk_registry;
    auto& module_mgr     = ctx.module_mgr;
    auto& kernel32       = ctx.k32_ext_mgr;

    // The process default activation context (Learn "Activation Contexts",
    // "Application Manifests"): the executable's
    // CREATEPROCESS_MANIFEST_RESOURCE_ID (1) manifest, or <exe>.manifest
    // beside it when it has none (an embedded manifest takes precedence),
    // with <exe>.config's assembly bindings. Windows refuses to start a
    // process whose context cannot be generated.
    {
        const std::string exe(module_mgr.main_module_path());
        const auto slash = exe.find_last_of("\\/");
        const std::string exe_dir = slash == std::string::npos ? std::string{} : exe.substr(0, slash);
        std::vector<u8> external;
        std::span<const u8> manifest = pe::pe_manifest_resource(pe_data, pe, 1);
        const auto read_all = [&](const std::string& path) -> std::optional<std::vector<u8>> {
            auto h = ctx.vfs.open(path, capsule::AccessMode::Read, capsule::ShareMode::Read,
                                  capsule::CreationDisposition::OpenExisting);
            if (h.is_err()) return std::nullopt;
            std::vector<u8> bytes;
            std::array<u8, 64 * 1024> chunk{};
            for (;;) {
                auto n = ctx.vfs.read(h.value(), chunk);
                if (n.is_err() || n.value() == 0) break;
                bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(n.value()));
            }
            (void)ctx.vfs.close(h.value());
            return bytes;
        };
        if (manifest.empty() && !exe.empty()) {
            if (auto bytes = read_all(exe + ".manifest")) {
                external = std::move(*bytes);
                manifest = external;
            }
        }
        if (!manifest.empty()) {
            winapi::sxs::ContextRequest request;
            request.manifest = manifest;
            request.manifest_path = external.empty() ? exe : exe + ".manifest";
            request.root_directory = exe_dir;
            request.application_dir = exe_dir;
            request.architecture = pe.is_64bit ? "amd64" : "x86";
            bool config_ok = true;
            if (auto config = read_all(exe + ".config")) {
                request.config = winapi::sxs::parse_app_config(*config);
                config_ok = request.config.has_value();
            }
            winapi::sxs::ContextResult generated;
            if (config_ok) generated = winapi::sxs::build_context(ctx.vfs, request);
            if (!config_ok || !generated.data) {
                constexpr u32 kStatusSxsCantGenActCtx = 0xC0150002u;  // ntstatus.h
                const std::string detail = config_ok ? generated.detail : exe + ".config is not well-formed";
                std::fprintf(stderr, "[vitreo] side-by-side configuration is incorrect: %s "
                             "(STATUS_SXS_CANT_GEN_ACTCTX)\n", detail.c_str());
                return RunResult{static_cast<int>(kStatusSxsCantGenActCtx), 0,
                                 "Side-by-side configuration is incorrect: " + detail, false};
            }
            kernel32.set_process_default_activation_context(
                std::make_shared<const winapi::sxs::ContextData>(std::move(*generated.data)));
        }
    }
    const auto search_directories = main_import_search_directories(
        module_mgr.main_module_path(), ctx.vfs.cwd(),
        pe::pe_private_assemblies(pe_data, pe));

    pe::ImportCallback resolver = [
        &thunk_registry, &module_mgr, &kernel32, search_directories
    ](
        std::string_view dll_name,
        std::string_view func_name,
        u16 ordinal
    ) -> u64 {
        // DATA IMPORTS: return pointer to actual guest data, not a thunk
        if (!func_name.empty()) {
            // Data binding is DLL-scoped just like function binding. A real
            // guest CRT or another DLL may export the same spelling while
            // owning a completely different variable.
            u64 data_addr = thunk_registry.lookup_data_export(dll_name, func_name);
            if (data_addr != 0) return data_addr;
            auto mapped = pe::resolve_api_set(dll_name);
            if (mapped != dll_name) {
                data_addr = thunk_registry.lookup_data_export(mapped, func_name);
                if (data_addr != 0) return data_addr;
            }
        }

        // Load-time dynamic linking loads every imported DLL: a host DLL
        // bound here must be a module GetModuleHandle finds by base name.
        // Side-by-side redirection is the first step of the DLL search
        // order for a module name; a redirected DLL is the one bound.
        if (const auto redirected = kernel32.sxs_redirect_dll(dll_name)) {
            const GuestAddr base = module_mgr.load_library(*redirected);
            if (base == 0) return 0;
            if (!func_name.empty()) return module_mgr.get_proc_address(base, func_name);
            if (ordinal != 0) return module_mgr.get_proc_address_by_ordinal(base, ordinal);
            return 0;
        }

        if (thunk_registry.has_dll(dll_name) ||
            thunk_registry.has_dll(pe::resolve_api_set(dll_name))) {
            (void)module_mgr.ensure_static_import_module(dll_name);
        }

        // A Windows import and GetProcAddress identify the same exported
        // address. Synthetic modules publish callable in-image veneers in
        // their EAT, so resolve through ModuleManager before falling back to
        // the host-dispatch registry.
        GuestAddr module_base = module_mgr.get_module_handle(dll_name);
        if (module_base != 0) {
            if (!func_name.empty()) {
                const auto addr = module_mgr.get_proc_address(
                    module_base, func_name);
                if (addr != 0) return addr;
            }
            if (ordinal != 0) {
                const auto addr = module_mgr.get_proc_address_by_ordinal(
                    module_base, ordinal);
                if (addr != 0) return addr;
            }
        }

        // Try direct DLL name first
        if (!func_name.empty()) {
            auto addr = thunk_registry.get_thunk_address(dll_name, func_name);
            if (addr.is_ok()) return thunk_registry.guest_entry(addr.value());
        }
        if (ordinal != 0) {
            auto addr = thunk_registry.get_thunk_address(dll_name, ordinal);
            if (addr.is_ok()) return thunk_registry.guest_entry(addr.value());
        }

        // Try API set mapping
        auto mapped = pe::resolve_api_set(dll_name);
        if (mapped != dll_name) {
            if (!func_name.empty()) {
                auto addr = thunk_registry.get_thunk_address(mapped, func_name);
                if (addr.is_ok()) return thunk_registry.guest_entry(addr.value());
            }
            if (ordinal != 0) {
                auto addr = thunk_registry.get_thunk_address(mapped, ordinal);
                if (addr.is_ok()) return thunk_registry.guest_entry(addr.value());
            }
        }

        // ==================================================================
        // Companion DLL loading: if not in the thunk registry, try loading
        // the real PE DLL from the application's directory via VFS.
        // This handles cases like sol.exe importing from cards.dll that
        // sits alongside the executable.
        // ==================================================================
        GuestAddr mod_base = module_mgr.get_module_handle(dll_name);
        if (mod_base == 0) {
            for (const auto& directory : search_directories) {
                std::string full_path =
                    directory + "\\" + std::string(dll_name);
                mod_base = module_mgr.load_library(full_path);
                if (mod_base != 0) {
                    VITREO_TRACE(
                        "[vitreo] Loaded companion DLL: %.*s from \"%s\" "
                        "@ 0x%llx\n",
                        static_cast<int>(dll_name.size()), dll_name.data(),
                        full_path.c_str(),
                        static_cast<unsigned long long>(mod_base));
                    break;
                }
            }
        }
        if (mod_base != 0) {
            if (!func_name.empty()) {
                GuestAddr addr = module_mgr.get_proc_address(mod_base, func_name);
                if (addr != 0) return addr;
            }
            if (ordinal != 0) {
                GuestAddr addr = module_mgr.get_proc_address_by_ordinal(mod_base, ordinal);
                if (addr != 0) return addr;
            }
        }

        // Missing-export stubs are only for unimplemented functions of a
        // host WinAPI module already in the thunk registry. Microsoft Learn
        // ("Load-Time Dynamic Linking"): if the system cannot locate a
        // required DLL, it terminates the process. Vendor/app DLLs that
        // are neither registered nor found on disk must fail that way —
        // never with a synthetic IAT stub.
        if (!thunk_registry.has_dll(dll_name) &&
            !thunk_registry.has_dll(mapped)) {
            return 0;
        }

        // Last resort: install a synthetic "missing export" stub so the
        // IAT slot points at a logging handler instead of NULL. Without
        // this, calls through unresolved imports crash via NULL-deref
        // (EIP=0 InvalidOpcode) which is undebuggable.  Mirrors the
        // behaviour pe_loader.cpp installs for non-main PE images.
        std::fprintf(stderr,
            "[vitreo] missing-export stub (main PE): %.*s!%.*s (ord %u)\n",
            static_cast<int>(dll_name.size()), dll_name.data(),
            static_cast<int>(func_name.size()), func_name.data(),
            static_cast<unsigned>(ordinal));
        return thunk_registry.register_missing_export(dll_name, func_name, ordinal);
    };

    auto import_result = pe::resolve_and_patch_imports(
        memory, pe, pe_data, base_address, resolver);

    if (import_result.is_err()) {
        // NoImports is not fatal
        if (import_result.error() != pe::ImportError::NoImports) {
            return RunResult{1, 0,
                std::string("Import resolution error: ") +
                    pe::import_error_message(import_result.error()),
                false};
        }
    } else {
        const auto& patch = import_result.value();
        std::fprintf(stderr, "[vitreo] Imports: %u resolved, "
                     "%u unresolved\n",
                     patch.stats.imports_resolved,
                     patch.stats.imports_unresolved);
        for (const auto& u : patch.unresolved) {
            std::fprintf(stderr, "[vitreo]   unresolved: %s!%s (ord %u)\n",
                         u.dll_name.c_str(),
                         u.function_name.c_str(),
                         u.ordinal);
        }
        if (patch.stats.imports_unresolved != 0) {
            // ntstatus.h: STATUS_DLL_NOT_FOUND
            // ("The program can't start because %hs is missing from your
            // computer.") GetExitCodeProcess reports this NTSTATUS.
            constexpr u32 kStatusDllNotFound = 0xC0000135u;
            const std::string& missing = patch.unresolved.front().dll_name;
            std::fprintf(stderr,
                "[vitreo] required DLL not found: %s (STATUS_DLL_NOT_FOUND)\n",
                missing.c_str());
            return RunResult{
                static_cast<int>(kStatusDllNotFound),
                0,
                "Required DLL not found: " + missing,
                false};
        }
    }

    return RunResult{0, 0, {}, true};
}

} // namespace vitreo::cli

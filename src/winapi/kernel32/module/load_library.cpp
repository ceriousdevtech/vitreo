/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "module.hpp"
#include "module_internal.hpp"
#include "core/pe/loader/cor_image.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/win32_full_path.hpp"
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

/// @file load_library.cpp
/// @brief ModuleManager::load_library
///
/// Closed method cut from module.cpp.

namespace vitreo::winapi {

GuestAddr ModuleManager::load_library(std::string_view name, u32 flags) {
    // Microsoft Learn: DllMain runs while the Windows loader lock is held.
    // Serialize the complete lookup/map/TLS/DllMain transaction so concurrent
    // callers cannot publish two instances of one normalized module. The lock
    // is recursive because dependency resolution re-enters load_library().
    std::lock_guard<std::recursive_mutex> loader_guard(m_loader_mutex);

    // A successful load leaves the caller's last error as it found it. Measured
    // on Windows 11 (tools/oracle_probe/last_error_on_success_probe.expected):
    // the real loader does not clear it — it can even leak an internal SxS
    // probe code — so no caller may depend on a success-path value. Restore
    // the entry value on every success path; intermediate lookups below may
    // set errors that must not escape a load that ultimately succeeds.
    const u32 entry_last_error = get_last_error();
    // Learn LoadLibraryExW DONT_RESOLVE_DLL_REFERENCES (0x1): do not bind
    // static imports and do not call DllMain / TLS PROCESS_ATTACH.
    const bool dont_resolve_requested = (flags & 0x00000001u) != 0;

    // LoadLibraryEx documented dwFlags (winbase.h). Unknown bits →
    // ERROR_INVALID_PARAMETER (loader_contract_probe.expected unknown flag).
    constexpr u32 kLoadWithAlteredSearchPath = 0x00000008u;
    constexpr u32 kLoadLibrarySearchDllLoadDir = 0x00000100u;
    constexpr u32 kLoadLibrarySearchApplicationDir = 0x00000200u;
    constexpr u32 kLoadLibrarySearchUserDirs = 0x00000400u;
    constexpr u32 kLoadLibrarySearchSystem32 = 0x00000800u;
    constexpr u32 kLoadLibrarySearchDefaultDirs = 0x00001000u;
    constexpr u32 kLoadLibrarySearchMask =
        kLoadLibrarySearchDllLoadDir | kLoadLibrarySearchApplicationDir |
        kLoadLibrarySearchUserDirs | kLoadLibrarySearchSystem32 |
        kLoadLibrarySearchDefaultDirs;
    constexpr u32 kKnownLoadLibraryExFlags =
        0x00000001u |  // DONT_RESOLVE_DLL_REFERENCES
        0x00000002u |  // AS_DATAFILE
        kLoadWithAlteredSearchPath |
        0x00000010u |  // LOAD_IGNORE_CODE_AUTHZ_LEVEL
        0x00000020u |  // AS_IMAGE_RESOURCE
        0x00000040u |  // AS_DATAFILE_EXCLUSIVE
        0x00000080u |  // REQUIRE_SIGNED_TARGET
        kLoadLibrarySearchMask |
        0x00002000u |  // SAFE_CURRENT_DIRS
        0x00008000u;   // OS_INTEGRITY_CONTINUITY (SDK)

    if (name.empty()) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & ~kKnownLoadLibraryExFlags) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    if ((flags & kLoadWithAlteredSearchPath) != 0 &&
        (flags & kLoadLibrarySearchMask) != 0) {
        // Learn: ALTERED_SEARCH_PATH cannot be combined with any SEARCH_* flag.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    // VITREO_LOG_LOADLIB=1 records every dynamic load request. Only successful
    // PE maps produce a "[loader] mapped" line, so without this a load that was
    // never attempted is indistinguishable from one that failed early — which
    // is exactly the question when an application declines to load a module.
    static const bool s_log_loadlib = std::getenv("VITREO_LOG_LOADLIB") != nullptr;
    if (s_log_loadlib) {
        fprintf(stderr, "[module] LoadLibrary request \"%.*s\"\n",
                static_cast<int>(name.size()), name.data());
    }

    // Resolve API set schema names (e.g. "api-ms-win-core-synch-l1-2-0") to
    // real DLL names (e.g. "kernel32.dll") so runtime LoadLibrary calls work
    // the same way static PE import resolution does.
    auto resolved = pe::resolve_api_set(name);
    if (resolved != name) {
        name = resolved;
    }

    // A module is named by its full path (GetFullPathNameW normalization), so
    // "dir\..\dir\x.dll" is the module already loaded as "dir\x.dll" and
    // GetModuleFileName reports the canonical spelling
    // (loader_path_canon_probe{32,64}.expected).
    std::string canonical_path;
    if (is_absolute_windows_path(name)) {
        if (auto full = capsule::win32_full_path(name, m_current_directory)) {
            canonical_path = std::move(full->path);
            name = canonical_path;
        }
    }

    // Absolute paths load that file only. Relative paths with separators
    // (sub\x.dll, .\x.dll) are appended to every DLL-search token (Learn
    // LoadLibrary remarks) — they are not a single VFS lookup against CWD.
    const bool is_absolute_path = is_absolute_windows_path(name);
    const bool is_relative_path =
        looks_like_path(name) && !is_absolute_path;
    const bool is_explicit_path = is_absolute_path;
    if ((flags & kLoadLibrarySearchDllLoadDir) != 0 && !is_absolute_path) {
        // Learn: SEARCH_DLL_LOAD_DIR requires an absolute lpFileName.
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }

    std::string base_key = normalize_name(name);
    std::string cache_key = is_explicit_path ? normalize_path_key(name) : base_key;
    std::string requested_filename(name);
    if (auto separator = requested_filename.find_last_of("\\/");
        separator != std::string::npos) {
        requested_filename.erase(0, separator + 1);
    }
    std::transform(requested_filename.begin(), requested_filename.end(),
                   requested_filename.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (requested_filename.find('.') == std::string::npos) {
        requested_filename += ".dll";
    }


    // Already loaded?
    auto it = modules_by_name_.find(cache_key);
    if (it != modules_by_name_.end()) {
        if (it->second.state == ModuleInfo::State::Failed) {
            set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
            return 0;
        }
        if (!reattach_detached_module(it->second)) {
            set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
            return 0;
        }
        ++it->second.ref_count;
        set_last_error(entry_last_error);
        return it->second.base_address;
    }

    // An earlier base-name load is indexed by its short name even though its
    // resolved ModuleInfo::path is absolute. A later absolute-path load of the
    // same file must return that existing module, not map a second image.
    if (is_explicit_path) {
        for (auto& [loaded_key, info] : modules_by_name_) {
            (void)loaded_key;
            if (!info.path.empty() &&
                normalize_path_key(info.path) == cache_key) {
                if (info.state == ModuleInfo::State::Failed ||
                    !reattach_detached_module(info)) {
                    set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
                    return 0;
                }
                ++info.ref_count;
                set_last_error(entry_last_error);
                return info.base_address;
            }
        }
    }

    // Learn LoadLibrary / DllMain: a DllMain FALSE failure unloads the DLL and
    // a later LoadLibrary retries (loader_contract_probe.expected "== DllMain
    // FALSE"). Do not permanently cache the failure here.

    // If the caller is loading by base name (not an explicit path), also
    // consider modules that were previously loaded via explicit path.
    if (!is_explicit_path) {
        for (auto& [loaded_key, info] : modules_by_name_) {
            (void)loaded_key;
            if (normalized_name_equals(info.name, base_key)) {
                if (!reattach_detached_module(info)) {
                    set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
                    return 0;
                }
                ++info.ref_count;
                set_last_error(entry_last_error);
                return info.base_address;
            }
        }
    }

    // If the caller provided an explicit path, prefer loading that PE from VFS.
    // This avoids incorrectly returning a thunk-backed module for plugin DLLs.
    if (is_explicit_path && m_memory && m_vfs && m_pe_loader) {
        std::string win_path = std::string(name);
        // A built-in DLL's file is its image, served for reading; loading
        // it is loading the built-in module.
        if (const GuestAddr base = load_builtin_dll_file(win_path);
            base != 0) {
            set_last_error(entry_last_error);
            return base;
        }
        auto data_res = module_read_all_bytes(*m_vfs, win_path);
        if (data_res.is_err()) {
            VITREO_TRACE(
                         "[module] LoadLibraryExW explicit-path read failed: path=\"%s\" err=%s\n",
                         win_path.c_str(),
                         capsule::vfs_error_to_string(data_res.error()));
        } else if (data_res.value().empty()) {
            VITREO_TRACE(
                         "[module] LoadLibraryExW explicit-path read returned 0 bytes: path=\"%s\"\n",
                         win_path.c_str());
        } else {
            // A 64-bit process maps an AnyCPU IL-only PE32 image as PE32+
            // (core/pe/loader/cor_image.hpp).
            if (m_is_64bit_process) (void)pe::rewrite_il_only_pe32_as_pe32plus(data_res.value());
            const auto& data = data_res.value();
            if (data.size() >= 2) {
                VITREO_TRACE(
                             "[module] LoadLibraryExW explicit-path read ok: path=\"%s\" bytes=%zu magic=%02X%02X\n",
                             win_path.c_str(), data.size(), data[0], data[1]);
            } else {
                VITREO_TRACE(
                             "[module] LoadLibraryExW explicit-path read ok: path=\"%s\" bytes=%zu\n",
                             win_path.c_str(), data.size());
            }

            auto parse_res = pe::parse_pe(std::span<const u8>(data.data(), data.size()));
            if (parse_res.is_err()) {
                VITREO_TRACE(
                             "[module] PE parse failed: path=\"%s\" err=%s\n",
                             win_path.c_str(), pe::parse_error_message(parse_res.error()));
            } else {
                const auto& parsed = parse_res.value();
                // Learn LoadLibrary / process interoperability: a 64-bit
                // process cannot map a 32-bit DLL (and the reverse). The
                // file exists; the machine type is wrong, so this is
                // ERROR_BAD_EXE_FORMAT rather than ERROR_MOD_NOT_FOUND or
                // a DllMain failure after mapping the image.
                // An IL-ONLY managed assembly is exempt. A pure-IL (AnyCPU)
                // assembly is emitted as a PE32 image with machine i386 even
                // when it will run in a 64-bit process -- there is no native
                // code in it, so the bitness of its headers describes nothing
                // that has to match. Windows loads such an image into either
                // process width; the CLR, not the OS loader, decides how to
                // run it.
                //
                // Rejecting it as ERROR_BAD_EXE_FORMAT is what stopped
                // coreclr: it LoadLibrary'd System.Runtime.dll (PE32, machine
                // 0x014c, COR20 flags ILONLY) inside 64-bit pwsh.exe, got
                // NULL, and threw -- surfacing as a C++ exception, a clean
                // seven-frame unwind, and Fatal error 0x80131506 with nothing
                // naming the loader.
                //
                // COMIMAGE_FLAGS_32BITREQUIRED still means what it says, so an
                // assembly that demands 32-bit is NOT exempt.
                bool il_only_any_cpu = false;
                if (parsed.is_managed()) {
                    constexpr u32 kComimageIlOnly = 0x00000001;
                    constexpr u32 kComimage32BitRequired = 0x00000002;
                    const u32 cor_rva =
                        parsed.data_directories[pe::DIR_CLR_RUNTIME].VirtualAddress;
                    // COR20 header: cb, MajorRuntimeVersion, MinorRuntimeVersion,
                    // MetaData(rva,size), Flags at +16.
                    const auto cor_off = parsed.rva_to_file_offset(cor_rva);
                    if (cor_off.is_ok() &&
                        static_cast<usize>(cor_off.value()) + 20 <= data.size()) {
                        u32 cor_flags = 0;
                        std::memcpy(&cor_flags, data.data() + cor_off.value() + 16,
                                    sizeof(cor_flags));
                        il_only_any_cpu =
                            (cor_flags & kComimageIlOnly) != 0 &&
                            (cor_flags & kComimage32BitRequired) == 0;
                    }
                }
                if (parsed.is_64bit != m_is_64bit_process && !il_only_any_cpu) {
                    set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
                    return 0;
                }

                // Learn LoadLibraryW: an executable module (no IMAGE_FILE_DLL)
                // is loaded as if DONT_RESOLVE_DLL_REFERENCES — no static
                // imports, no entry-point call (loader_contract_probe.expected
                // "== exe as library").
                const bool resolve_refs =
                    !dont_resolve_requested && parsed.is_dll();

                // Prefer the PE's ImageBase (Windows loader contract). Only
                // fall back to the dynamic arena when that range is taken
                // (or when VITREO_FORCE_DLL_RELOC requests relocated loads).
                GuestAddr preferred = 0;
                if (!force_dll_reloc() && parsed.image_base != 0) {
                    const u64 end = parsed.image_base + parsed.size_of_image;
                    if (end >= parsed.image_base &&
                        (parsed.is_64bit || end <= 0xFFFF0000ULL)) {
                        preferred = static_cast<GuestAddr>(parsed.image_base);
                    }
                }
                if (preferred == 0) {
                    preferred = reserve_dynamic_base(parsed.size_of_image);
                }

                // Retry once with a fresh base if the chosen range turns out
                // to overlap an already-mapped region. find_free_gap should
                // make this unnecessary, but defend against races/stale state.
                Result<pe::LoadedImage, Error> load_res =
                    Result<pe::LoadedImage, Error>::err(Error::OutOfMemory);
                bool dep_failed = false;
                const std::string module_leaf =
                    requested_filename.find('.') != std::string::npos
                        ? requested_filename
                        : (base_key + ".dll");
                // Register the image as soon as it is mapped, before its
                // imports are bound, so an import cycle back into it binds
                // to this image instead of loading it again (import_graph.cpp).
                bool registered = false;
                auto notify_loaded = make_pre_import_callback(win_path, module_leaf);
                auto pre_import = [&](GuestAddr image_base, usize image_size,
                                      bool is_64bit) {
                    notify_loaded(image_base, image_size, is_64bit);
                    register_mapping_module(
                        cache_key, module_leaf, win_path, parsed,
                        std::span<const u8>(data.data(), data.size()),
                        image_base, image_size);
                    registered = true;
                };
                for (int attempt = 0; attempt < 3; ++attempt) {
                    load_res = m_pe_loader->load(
                        std::span<const u8>(data.data(), data.size()),
                        preferred,
                        resolve_refs
                            ? make_companion_resolver(parent_windows_dir(win_path),
                                                      &dep_failed)
                            : pe::ImportCallback{},
                        pre_import,
                        resolve_refs);
                    if (load_res.is_ok()) {
                        break;
                    }
                    if (load_res.error() != Error::AlreadyExists) {
                        break;
                    }
                    VITREO_TRACE(
                                 "[module] PE load base 0x%llx already mapped, retrying (attempt %d) path=\"%s\"\n",
                                 static_cast<unsigned long long>(preferred),
                                 attempt + 1, win_path.c_str());
                    preferred = reserve_dynamic_base(parsed.size_of_image);
                }
                const bool defer_init = load_res.is_ok() && registered &&
                                        finish_module_binding(cache_key);
                if (load_res.is_err()) {
                    if (registered) discard_mapping_module(cache_key);
                    VITREO_TRACE(
                                 "[module] PE load failed: path=\"%s\" err=%s\n",
                                 win_path.c_str(),
                                 vitreo::error_to_string(load_res.error()));
                } else if (resolve_refs && dep_failed) {
                    if (registered) discard_mapping_module(cache_key);
                    // A dependency DLL existed on disk but failed to
                    // initialize. Real Windows fails LoadLibrary with
                    // ERROR_DLL_INIT_FAILED rather than returning a handle
                    // with stubbed imports. Do not run TLS/DllMain for a
                    // module whose imports are known-bad.
                    fprintf(stderr,
                            "[loader] failing load of \"%s\" \u2014 a dependency DLL "
                            "failed DllMain (cascading ERROR_DLL_INIT_FAILED)\n",
                            win_path.c_str());
                    {
                        std::unique_lock<HostRwLock> tables(m_tables_mutex);
                        failed_modules_.insert(base_key);
                        failed_modules_.insert(cache_key);
                    }
                    set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
                    return 0;
                } else {
                    const auto& img = load_res.value();

                    ModuleInfo info;
                    info.base_address = img.image_base;
                    info.image_end    = img.image_end;
                    info.entry_point  = img.entry_point;
                    info.bitness      = img.bitness;
                    info.exception_rva = parsed.data_directories[pe::DIR_EXCEPTION].VirtualAddress;
                    info.exception_size = parsed.data_directories[pe::DIR_EXCEPTION].Size;
                    if (parsed.num_data_directories > pe::DIR_TLS) {
                        const auto& tls_dd = parsed.data_directories[pe::DIR_TLS];
                        if (tls_dd.Size != 0) info.tls_dir_rva = tls_dd.VirtualAddress;
                    }
                    // Resource directory, so FindResource/LoadAccelerators/
                    // LoadString can serve requests naming this module's
                    // HINSTANCE rather than only the main image.
                    if (parsed.num_data_directories > pe::DIR_RESOURCE) {
                        const auto& rsrc_dd = parsed.data_directories[pe::DIR_RESOURCE];
                        info.resource_rva  = rsrc_dd.VirtualAddress;
                        info.resource_size = rsrc_dd.Size;
                    }
                    info.mapped_sections = img.sections;
                    info.mapped_header_size = img.header_size;
                    rebuild_image_protection_runs(info);
                    info.name         = module_leaf;
                    info.path         = win_path;
                    info.ref_count    = 1;
                    // Insert with Initializing state BEFORE running TLS/DllMain
                    // so reentrant load_library() during init returns this handle
                    // instead of recursing into mapping again.
                    info.state        = ModuleInfo::State::Initializing;

                    auto ex = parse_exports_from_file(std::span<const u8>(data.data(), data.size()), parsed, img.image_base);
                    info.exports_by_name = std::move(ex.by_name);
                    info.exports_by_ordinal = std::move(ex.by_ordinal);
                    info.forwarders_by_name = std::move(ex.forwarders_by_name);
                    info.forwarders_by_ordinal = std::move(ex.forwarders_by_ordinal);

                    {
                        std::unique_lock<HostRwLock> tables(m_tables_mutex);
                        base_to_name_[img.image_base] = cache_key;
                        modules_by_name_[cache_key] = std::move(info);
                    }
                    record_module_load_order(cache_key);

                    // A mapped DLL is loader-visible while its TLS callbacks
                    // and DllMain execute. Publish it before either initializer
                    // can inspect PEB_LDR_DATA.
                    if (!publish_loader_entry(modules_by_name_[cache_key])) {
                        modules_by_name_[cache_key].state =
                            ModuleInfo::State::Failed;
                        set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
                        return 0;
                    }

                    if (module_diag_enabled()) {
                        fprintf(stderr,
                                "[loader pid=%d] mapped \"%s\" base=0x%llx end=0x%llx entry=0x%llx bitness=%u "
                                "tls_rva=0x%x guest_call=%s\n",
                                static_cast<int>(getpid()),
                                win_path.c_str(),
                                static_cast<unsigned long long>(img.image_base),
                                static_cast<unsigned long long>(img.image_end),
                                static_cast<unsigned long long>(img.entry_point),
                                static_cast<unsigned>(img.bitness),
                                static_cast<unsigned>(modules_by_name_[cache_key].tls_dir_rva),
                                m_guest_call ? "yes" : "NULL");
                    }

                    patch_guard_cf_load_config(*m_memory, registry_, parsed,
                                               img.image_base, win_path.c_str());
                    capture_mapped_image_template(modules_by_name_[cache_key]);

                    // Inside an import cycle whose root is still binding: the
                    // root runs this initializer once every import is bound.
                    if (defer_init) {
                        defer_cycle_init(cache_key, resolve_refs && !il_only_any_cpu);
                        set_last_error(entry_last_error);
                        return img.image_base;
                    }
                    // Cycle root: the members it deferred initialize first.
                    if (!m_deferred_cycle_inits.empty() &&
                        !run_deferred_cycle_inits(cache_key)) {
                        return 0;
                    }

                    // An IL-ONLY managed assembly has no DllMain to call.
                    // Its AddressOfEntryPoint is the vestigial
                    // mscoree!_CorDllMain thunk, kept for .NET Framework 1.x
                    // compatibility; on modern Windows the loader ignores the
                    // entry point of an ILONLY image exactly as it ignores its
                    // import table. Here the import is deliberately unbound
                    // (see the ILONLY exemption in pe_loader), so the thunk
                    // jumps into nothing -- coreclr's assemblies died with
                    // STATUS_ILLEGAL_INSTRUCTION (0xc000001d) inside "DllMain",
                    // and the loader dutifully reported DllMain returned FALSE.
                    // DONT_RESOLVE / non-DLL EXE: never call the entry point.
                    if (resolve_refs && m_guest_call && img.entry_point != 0 &&
                        !il_only_any_cpu) {
                        constexpr u64 DLL_PROCESS_ATTACH = 1;

                        // If VITREO_TRACE_DURING_DLLMAIN matches this module
                        // (case-insensitive substring on the short name),
                        // arm the hostcall ring's "in DllMain" flag so the
                        // ring only records calls made while this DLL is
                        // initializing.
                        bool dllmain_trace_armed = false;
                        if (const char* mt = std::getenv("VITREO_TRACE_DURING_DLLMAIN")) {
                            auto contains_ci = [](const std::string& hay, const char* needle) {
                                std::string h = hay, n = needle;
                                std::transform(h.begin(), h.end(), h.begin(),
                                               [](unsigned char c){ return std::tolower(c); });
                                std::transform(n.begin(), n.end(), n.begin(),
                                               [](unsigned char c){ return std::tolower(c); });
                                return h.find(n) != std::string::npos;
                            };
                            if (contains_ci(cache_key, mt) || contains_ci(base_key, mt)) {
                                vitreo::diagnostics::enter_module_init();
                                dllmain_trace_armed = true;
                            }
                        }

                        if (parsed.num_data_directories > pe::DIR_TLS) {
                            const auto& tls_dd = parsed.data_directories[pe::DIR_TLS];
                            if (tls_dd.VirtualAddress != 0 && tls_dd.Size != 0) {
                                setup_dll_tls(
                                    img.image_base,
                                    img.bitness,
                                    static_cast<GuestAddr>(img.image_base + tls_dd.VirtualAddress),
                                    win_path.c_str());
                                if (module_diag_enabled()) {
                                    fprintf(stderr,
                                            "[loader] TLS callbacks BEGIN \"%s\" tls_va=0x%llx\n",
                                            win_path.c_str(),
                                            static_cast<unsigned long long>(img.image_base + tls_dd.VirtualAddress));
                                }
                                fire_tls_callbacks(
                                    img.image_base,
                                    img.bitness,
                                    static_cast<GuestAddr>(img.image_base + tls_dd.VirtualAddress),
                                    DLL_PROCESS_ATTACH,
                                    win_path.c_str());
                                if (module_diag_enabled()) {
                                    fprintf(stderr,
                                            "[loader] TLS callbacks END   \"%s\"\n",
                                            win_path.c_str());
                                }
                            }
                        }

                        u64 args[3] = {
                            static_cast<u64>(img.image_base),
                            DLL_PROCESS_ATTACH,
                            0,
                        };
                        if (module_diag_enabled()) {
                            fprintf(stderr,
                                    "[loader] DllMain BEGIN \"%s\" entry=0x%llx\n",
                                    win_path.c_str(),
                                    static_cast<unsigned long long>(img.entry_point));
                        }
                        u64 dllmain_ret = m_guest_call(img.entry_point, args, 3, img.bitness == 64);
                        if (module_diag_enabled()) {
                            fprintf(stderr,
                                    "[loader] DllMain END   \"%s\" returned 0x%llx\n",
                                    win_path.c_str(),
                                    static_cast<unsigned long long>(dllmain_ret));
                        }
                        if (dllmain_trace_armed) {
                            vitreo::diagnostics::leave_module_init();
                        }
                        modules_by_name_[cache_key].dllmain_called = true;

                        // Per MSDN: if the DllMain entry-point returns FALSE
                        // for DLL_PROCESS_ATTACH, LoadLibraryEx must fail and
                        // unload the partially-loaded module.
                        if (dllmain_ret == 0) {
                            // Learn: unload the DLL (DLL_PROCESS_DETACH with
                            // reserved=NULL) and fail this LoadLibrary; a later
                            // call retries (loader_contract_probe).
                            fprintf(stderr,
                                    "[loader] DllMain returned FALSE — failing load of \"%s\"\n",
                                    win_path.c_str());
                            ModuleInfo& failed = modules_by_name_[cache_key];
                            if (m_guest_call && failed.entry_point != 0) {
                                constexpr u64 DLL_PROCESS_DETACH = 0;
                                u64 dargs[3] = {
                                    static_cast<u64>(failed.base_address),
                                    DLL_PROCESS_DETACH, 0};
                                (void)m_guest_call(failed.entry_point, dargs, 3,
                                                   failed.bitness == 64);
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
                                modules_by_name_.erase(cache_key);
                            }
                            set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
                            return 0;
                        }
                    }

                    modules_by_name_[cache_key].state = ModuleInfo::State::Loaded;
                    set_last_error(entry_last_error);
                    return img.image_base;
                }
            }
        }

        // If this looks like a system DLL path (e.g. C:\Windows\System32\SHLWAPI.dll)
        // and we have thunk-backed exports, fall back to a synthetic module.
        // This is important for fresh capsules that don't include real System32 DLL files.
        if (is_system_dll_path(win_path)) {
            if (auto base = synthesize_thunk_module(base_key); base != 0) {
                set_last_error(entry_last_error);
                return base;
            }
        }

        // Apps frequently call LoadLibraryExW with a full path inside their own
        // install directory (a full path whose file name is a system DLL) as the
        // first attempt of the standard search order. When that file isn't
        // present but vitreo has a thunk-backed implementation of the
        // requested DLL, fall back to the synthetic thunk module so the
        // import succeeds instead of returning ERROR_MOD_NOT_FOUND. This
        // matches what Windows does once the loader's search-order falls
        // through to System32.
        const std::string& fallback_with_ext = requested_filename;
        if (registry_.has_dll(fallback_with_ext)) {
            VITREO_TRACE(
                "[module] LoadLibraryExW thunk fallback: path=\"%s\" -> %s\n",
                win_path.c_str(), fallback_with_ext.c_str());
            if (auto base = synthesize_thunk_module(base_key); base != 0) {
                set_last_error(entry_last_error);
                return base;
            }
        }

        set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
        return 0;
    }

    // For implicit (non-explicit-path) loads, search the application directory,
    // the SetDllDirectory directory, system directories, and the process CWD.
    // Real PE files on disk win over thunk-backed implementations so that apps
    // which ship their own DLL get the file they expect.
    const std::string& dll_with_ext = requested_filename;

    bool machine_mismatch = false;
    bool dll_init_failed = false;
    auto try_load_pe_from_vfs = [&](const std::string& win_path) -> GuestAddr {
        if (!m_memory || !m_vfs || !m_pe_loader) return 0;
        if (const GuestAddr base = load_builtin_dll_file(win_path);
            base != 0) {
            set_last_error(entry_last_error);
            return base;
        }
        auto data_res = module_read_all_bytes(*m_vfs, win_path);
        if (data_res.is_err() || data_res.value().empty()) return 0;
        // A 64-bit process maps an AnyCPU IL-only PE32 image as PE32+
        // (core/pe/loader/cor_image.hpp).
        if (m_is_64bit_process) (void)pe::rewrite_il_only_pe32_as_pe32plus(data_res.value());
        const auto& data = data_res.value();

        auto parse_res = pe::parse_pe(std::span<const u8>(data.data(), data.size()));
        if (parse_res.is_err()) return 0;
        const auto& parsed = parse_res.value();
        if (parsed.is_64bit != m_is_64bit_process) {
            set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
            machine_mismatch = true;
            return 0;
        }

        GuestAddr preferred = 0;
        if (!force_dll_reloc() && parsed.image_base != 0) {
            const u64 end = parsed.image_base + parsed.size_of_image;
            if (end >= parsed.image_base &&
                (parsed.is_64bit || end <= 0xFFFF0000ULL)) {
                preferred = static_cast<GuestAddr>(parsed.image_base);
            }
        }
        if (preferred == 0) {
            preferred = reserve_dynamic_base(parsed.size_of_image);
        }
        // Learn: non-DLL PE or DONT_RESOLVE_DLL_REFERENCES → no imports/DllMain.
        const bool resolve_refs =
            !dont_resolve_requested && parsed.is_dll();
        Result<pe::LoadedImage, Error> load_res =
            Result<pe::LoadedImage, Error>::err(Error::OutOfMemory);
        bool dep_failed = false;
        // A search-order load is keyed by its short name (cache_key); an
        // import cycle back into it must bind to this image (import_graph.cpp).
        bool registered = false;
        auto notify_loaded = make_pre_import_callback(win_path, dll_with_ext);
        auto pre_import = [&](GuestAddr image_base, usize image_size,
                              bool is_64bit) {
            notify_loaded(image_base, image_size, is_64bit);
            register_mapping_module(
                cache_key, dll_with_ext, win_path, parsed,
                std::span<const u8>(data.data(), data.size()),
                image_base, image_size);
            registered = true;
        };
        for (int attempt = 0; attempt < 3; ++attempt) {
            load_res = m_pe_loader->load(
                std::span<const u8>(data.data(), data.size()),
                preferred,
                resolve_refs
                    ? make_companion_resolver(parent_windows_dir(win_path),
                                              &dep_failed)
                    : pe::ImportCallback{},
                pre_import,
                resolve_refs);
            if (load_res.is_ok()) break;
            if (load_res.error() != Error::AlreadyExists) break;
            VITREO_TRACE(
                         "[module] PE load base 0x%llx already mapped, retrying (attempt %d) path=\"%s\"\n",
                         static_cast<unsigned long long>(preferred),
                         attempt + 1, win_path.c_str());
            preferred = reserve_dynamic_base(parsed.size_of_image);
        }
        const bool defer_init = load_res.is_ok() && registered &&
                                finish_module_binding(cache_key);
        if (load_res.is_err()) {
            if (registered) discard_mapping_module(cache_key);
            return 0;
        }
        const auto& img = load_res.value();

        if (resolve_refs && dep_failed) {
            if (registered) discard_mapping_module(cache_key);
            // Dependency DLL failed DllMain — fail this parent load too,
            // mirroring Windows ERROR_DLL_INIT_FAILED cascade.
            fprintf(stderr,
                    "[loader] failing load of \"%s\" — a dependency DLL "
                    "failed DllMain (cascading ERROR_DLL_INIT_FAILED)\n",
                    win_path.c_str());
            {
                std::unique_lock<HostRwLock> tables(m_tables_mutex);
                failed_modules_.insert(base_key);
                failed_modules_.insert(cache_key);
            }
            set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
            return 0;
        }

        ModuleInfo info;
        info.base_address = img.image_base;
        info.image_end    = img.image_end;
        info.entry_point  = img.entry_point;
        info.bitness      = img.bitness;
        info.mapped_sections = img.sections;
        info.mapped_header_size = img.header_size;
        rebuild_image_protection_runs(info);
        info.name         = dll_with_ext;
        info.path         = win_path;
        info.ref_count    = 1;
        // Insert with Initializing state BEFORE running TLS/DllMain so
        // reentrant load_library() during init returns this handle.
        info.state        = ModuleInfo::State::Initializing;
        // The .pdata table RtlLookupFunctionEntry and SEH dispatch search.
        // Without it every function in this module unwinds as a leaf, and its
        // own __try/__except handlers are never found.
        if (parsed.num_data_directories > pe::DIR_EXCEPTION) {
            info.exception_rva = parsed.data_directories[pe::DIR_EXCEPTION].VirtualAddress;
            info.exception_size = parsed.data_directories[pe::DIR_EXCEPTION].Size;
        }
        if (parsed.num_data_directories > pe::DIR_TLS) {
            const auto& tls_dd = parsed.data_directories[pe::DIR_TLS];
            if (tls_dd.Size != 0) info.tls_dir_rva = tls_dd.VirtualAddress;
        }
        // Resource directory, so FindResource/LoadAccelerators/LoadString can
        // serve requests naming this module's HINSTANCE rather than only the
        // main image — same as the explicit-path branch above. Omitting it
        // makes resource_directory_for_base() report the module as
        // "resourceless", and a caller that retries the failed lookup spins.
        if (parsed.num_data_directories > pe::DIR_RESOURCE) {
            const auto& rsrc_dd = parsed.data_directories[pe::DIR_RESOURCE];
            info.resource_rva  = rsrc_dd.VirtualAddress;
            info.resource_size = rsrc_dd.Size;
        }

        auto ex = parse_exports_from_file(std::span<const u8>(data.data(), data.size()), parsed, img.image_base);
        info.exports_by_name = std::move(ex.by_name);
        info.exports_by_ordinal = std::move(ex.by_ordinal);
        info.forwarders_by_name = std::move(ex.forwarders_by_name);
        info.forwarders_by_ordinal = std::move(ex.forwarders_by_ordinal);

        {
            std::unique_lock<HostRwLock> tables(m_tables_mutex);
            base_to_name_[img.image_base] = cache_key;
            modules_by_name_[cache_key] = std::move(info);
        }
        record_module_load_order(cache_key);

        if (!publish_loader_entry(modules_by_name_[cache_key])) {
            modules_by_name_[cache_key].state = ModuleInfo::State::Failed;
            set_last_error(error_codes::ERROR_NOT_ENOUGH_MEMORY);
            return 0;
        }

        if (module_diag_enabled()) {
            fprintf(stderr,
                    "[loader pid=%d] mapped \"%s\" base=0x%llx end=0x%llx entry=0x%llx bitness=%u "
                    "tls_rva=0x%x guest_call=%s (vfs)\n",
                    static_cast<int>(getpid()),
                    win_path.c_str(),
                    static_cast<unsigned long long>(img.image_base),
                    static_cast<unsigned long long>(img.image_end),
                    static_cast<unsigned long long>(img.entry_point),
                    static_cast<unsigned>(img.bitness),
                    static_cast<unsigned>(modules_by_name_[cache_key].tls_dir_rva),
                    m_guest_call ? "yes" : "NULL");
        }

        patch_guard_cf_load_config(*m_memory, registry_, parsed,
                                   img.image_base, win_path.c_str());
        capture_mapped_image_template(modules_by_name_[cache_key]);

        // Inside an import cycle whose root is still binding: the root runs
        // this initializer once every import is bound.
        if (defer_init) {
            defer_cycle_init(cache_key, resolve_refs);
            set_last_error(entry_last_error);
            return img.image_base;
        }
        // Cycle root: the members it deferred initialize first.
        if (!m_deferred_cycle_inits.empty() &&
            !run_deferred_cycle_inits(cache_key)) {
            dll_init_failed = true;
            return 0;
        }

        if (resolve_refs && m_guest_call && img.entry_point != 0) {
            constexpr u64 DLL_PROCESS_ATTACH = 1;

            if (parsed.num_data_directories > pe::DIR_TLS) {
                const auto& tls_dd = parsed.data_directories[pe::DIR_TLS];
                if (tls_dd.VirtualAddress != 0 && tls_dd.Size != 0) {
                    setup_dll_tls(
                        img.image_base,
                        img.bitness,
                        static_cast<GuestAddr>(img.image_base + tls_dd.VirtualAddress),
                        win_path.c_str());
                    if (module_diag_enabled()) {
                        fprintf(stderr,
                                "[loader] TLS callbacks BEGIN \"%s\" tls_va=0x%llx\n",
                                win_path.c_str(),
                                static_cast<unsigned long long>(img.image_base + tls_dd.VirtualAddress));
                    }
                    fire_tls_callbacks(
                        img.image_base,
                        img.bitness,
                        static_cast<GuestAddr>(img.image_base + tls_dd.VirtualAddress),
                        DLL_PROCESS_ATTACH,
                        win_path.c_str());
                    if (module_diag_enabled()) {
                        fprintf(stderr,
                                "[loader] TLS callbacks END   \"%s\"\n",
                                win_path.c_str());
                    }
                }
            }

            u64 args[3] = {
                static_cast<u64>(img.image_base),
                DLL_PROCESS_ATTACH,
                0,
            };
            if (module_diag_enabled()) {
                fprintf(stderr,
                        "[loader] DllMain BEGIN \"%s\" entry=0x%llx\n",
                        win_path.c_str(),
                        static_cast<unsigned long long>(img.entry_point));
            }
            u64 dllmain_ret = m_guest_call(img.entry_point, args, 3, img.bitness == 64);
            if (module_diag_enabled()) {
                fprintf(stderr,
                        "[loader] DllMain END   \"%s\" returned 0x%llx\n",
                        win_path.c_str(),
                        static_cast<unsigned long long>(dllmain_ret));
            }
            modules_by_name_[cache_key].dllmain_called = true;

            if (dllmain_ret == 0) {
                fprintf(stderr,
                        "[loader] DllMain returned FALSE — failing load of \"%s\"\n",
                        win_path.c_str());
                ModuleInfo& failed = modules_by_name_[cache_key];
                if (m_guest_call && failed.entry_point != 0) {
                    constexpr u64 DLL_PROCESS_DETACH = 0;
                    u64 dargs[3] = {
                        static_cast<u64>(failed.base_address),
                        DLL_PROCESS_DETACH, 0};
                    (void)m_guest_call(failed.entry_point, dargs, 3,
                                       failed.bitness == 64);
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
                    modules_by_name_.erase(cache_key);
                }
                dll_init_failed = true;
                set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
                return 0;
            }
        }

        modules_by_name_[cache_key].state = ModuleInfo::State::Loaded;
        VITREO_TRACE("[module] LoadLibrary resolved \"%s\" via VFS (%s) base=0x%llx\n",
                     dll_with_ext.c_str(), win_path.c_str(),
                     static_cast<unsigned long long>(img.image_base));
        set_last_error(entry_last_error);
        return img.image_base;
    };

    if (!is_explicit_path) {
        auto try_dir = [&](std::string_view directory) -> GuestAddr {
            if (directory.empty()) return 0;
            std::string candidate(directory);
            if (candidate.back() != '\\' && candidate.back() != '/') {
                candidate.push_back('\\');
            }
            // Relative paths: append the entire relative path to each search
            // token (Learn LoadLibrary). Bare names: append the basename.
            if (is_relative_path) {
                candidate.append(name);
            } else {
                candidate += dll_with_ext;
            }
            return try_load_pe_from_vfs(candidate);
        };

        u32 search_flags = flags & kLoadLibrarySearchMask;
        if ((search_flags & kLoadLibrarySearchDefaultDirs) != 0) {
            search_flags |= kLoadLibrarySearchApplicationDir |
                            kLoadLibrarySearchUserDirs |
                            kLoadLibrarySearchSystem32;
        }
        // SetDefaultDllDirectories replaces the standard order for LoadLibrary
        // / LoadLibraryEx calls that omit SEARCH_* flags.
        if (search_flags == 0 && m_default_dll_directory_flags != 0) {
            search_flags = m_default_dll_directory_flags & kLoadLibrarySearchMask;
            if ((search_flags & kLoadLibrarySearchDefaultDirs) != 0) {
                search_flags |= kLoadLibrarySearchApplicationDir |
                                kLoadLibrarySearchUserDirs |
                                kLoadLibrarySearchSystem32;
            }
        }

        const bool restricted = search_flags != 0;

        auto try_user_dirs = [&]() -> GuestAddr {
            // AddDllDirectory / RemoveDllDirectory mutate this map from any
            // thread without the loader lock; snapshot it, since try_dir
            // loads (and takes the table lock exclusively) per directory.
            std::vector<std::string> user_dirs;
            {
                std::shared_lock<HostRwLock> tables(m_tables_mutex);
                user_dirs.reserve(m_user_dll_directories.size());
                for (const auto& [cookie, dir] : m_user_dll_directories) {
                    (void)cookie;
                    user_dirs.push_back(dir);
                }
            }
            for (const std::string& dir : user_dirs) {
                if (auto base = try_dir(dir);
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
            return 0;
        };

        if (restricted) {
            // LOAD_LIBRARY_SEARCH_* / SetDefaultDllDirectories: only the
            // selected directories (loader_contract_probe.expected search flags).
            if ((search_flags & kLoadLibrarySearchDllLoadDir) != 0 &&
                is_absolute_path) {
                if (auto base = try_dir(parent_windows_dir(name));
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
            if ((search_flags & kLoadLibrarySearchApplicationDir) != 0 &&
                !main_module_path_.empty()) {
                if (auto base = try_dir(parent_windows_dir(main_module_path_));
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
            if ((search_flags & kLoadLibrarySearchUserDirs) != 0) {
                if (auto base = try_user_dirs();
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
            if ((search_flags & kLoadLibrarySearchSystem32) != 0) {
                if (auto base = try_load_pe_from_vfs(
                        std::string("C:\\Windows\\System32\\") + dll_with_ext);
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
        } else {
            // Standard SafeDllSearchMode order (Learn). AddDllDirectory dirs
            // are NOT part of this order — only LOAD_LIBRARY_SEARCH_USER_DIRS
            // / SetDefaultDllDirectories(USER_DIRS) include them
            // (loader_contract_probe plain LoadLibraryW(lc_sub) → null).
            if (!main_module_path_.empty()) {
                std::string app_dir = parent_windows_dir(main_module_path_);
                if (auto base = try_dir(app_dir);
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
            if (m_dll_directory.has_value() && !m_dll_directory->empty()) {
                if (auto base = try_dir(*m_dll_directory);
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
            if (auto base = try_load_pe_from_vfs(
                    std::string("C:\\Windows\\System32\\") +
                    (is_relative_path ? std::string(name) : dll_with_ext));
                base != 0 || machine_mismatch || dll_init_failed) {
                return base;
            }
            if (auto base = try_load_pe_from_vfs(
                    std::string("C:\\Windows\\") +
                    (is_relative_path ? std::string(name) : dll_with_ext));
                base != 0 || machine_mismatch || dll_init_failed) {
                return base;
            }
            if (!m_dll_directory.has_value() || !m_dll_directory->empty()) {
                if (auto base = try_dir(m_current_directory);
                    base != 0 || machine_mismatch || dll_init_failed) {
                    return base;
                }
            }
            // PATH directories (dll_search_path_probe): not in any SEARCH_* set.
            if (m_path_variable_provider) {
                const std::string path_variable = m_path_variable_provider();
                std::string_view rest = path_variable;
                while (!rest.empty()) {
                    const auto semicolon = rest.find(';');
                    std::string_view entry = rest.substr(0, semicolon);
                    rest = semicolon == std::string_view::npos
                        ? std::string_view{} : rest.substr(semicolon + 1);
                    if (entry.size() >= 2 && entry.front() == '"' &&
                        entry.back() == '"') {
                        entry = entry.substr(1, entry.size() - 2);
                    }
                    if (auto base = try_dir(entry);
                        base != 0 || machine_mismatch || dll_init_failed) {
                        return base;
                    }
                }
            }
        }
    }

    // 4) Thunk-backed fallback for system DLLs we provide synthetically.
    if (registry_.has_dll(dll_with_ext)) {
        if (GuestAddr base = synthesize_thunk_module(base_key); base != 0) {
            set_last_error(entry_last_error);
            return base;
        }
    }

    set_last_error(error_codes::ERROR_MOD_NOT_FOUND);
    fprintf(stderr, "[module] LoadLibrary FAILED: \"%.*s\" (not found in thunks or VFS)\n",
            static_cast<int>(name.size()), name.data());
    return 0;
}

GuestAddr ModuleManager::load_as_data_file(std::string_view name, u32 flags) {
    // Learn LoadLibraryExW AS_DATAFILE / AS_IMAGE_RESOURCE /
    // AS_DATAFILE_EXCLUSIVE (loader_contract_probe.expected).
    constexpr u32 kAsDatafile = 0x00000002;
    constexpr u32 kAsImageResource = 0x00000020;
    constexpr u32 kAsDatafileExclusive = 0x00000040;
    std::lock_guard<std::recursive_mutex> loader_guard(m_loader_mutex);
    const u32 entry_last_error = get_last_error();

    if ((flags & kAsDatafile) != 0 && (flags & kAsDatafileExclusive) != 0) {
        set_last_error(error_codes::ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!m_memory || !m_vfs || !m_pe_loader) {
        set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        return 0;
    }

    auto resolved = pe::resolve_api_set(name);
    if (resolved != name) name = resolved;

    const bool is_explicit_path = looks_like_path(name);
    std::string base_key = normalize_name(name);
    std::string requested_filename(name);
    if (auto separator = requested_filename.find_last_of("\\/");
        separator != std::string::npos) {
        requested_filename.erase(0, separator + 1);
    }
    std::transform(requested_filename.begin(), requested_filename.end(),
                   requested_filename.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (requested_filename.find('.') == std::string::npos) {
        requested_filename += ".dll";
    }
    const std::string requested_key = normalize_name(requested_filename);

    // Already loaded as an executable image → return that HMODULE and bump
    // the reference count (Learn). No tag bits.
    for (auto& [key, info] : modules_by_name_) {
        (void)key;
        if (info.state != ModuleInfo::State::Loaded &&
            info.state != ModuleInfo::State::Initializing) {
            continue;
        }
        if (!normalized_name_equals(info.name, base_key) &&
            !normalized_name_equals(info.name, requested_key)) {
            continue;
        }
        if (!reattach_detached_module(info)) {
            set_last_error(error_codes::ERROR_DLL_INIT_FAILED);
            return 0;
        }
        ++info.ref_count;
        set_last_error(entry_last_error);
        return info.base_address;
    }

    auto try_map = [&](const std::string& win_path) -> GuestAddr {
        auto data_res = module_read_all_bytes(*m_vfs, win_path);
        if (data_res.is_err() || data_res.value().empty()) return 0;
        const auto& data = data_res.value();
        auto parse_res = pe::parse_pe(std::span<const u8>(data.data(), data.size()));
        if (parse_res.is_err()) {
            set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
            return 0;
        }
        const auto& parsed = parse_res.value();
        if (parsed.is_64bit != m_is_64bit_process) {
            set_last_error(error_codes::ERROR_BAD_EXE_FORMAT);
            return 0;
        }

        GuestAddr preferred = reserve_dynamic_base(parsed.size_of_image);
        auto load_res = m_pe_loader->load(
            std::span<const u8>(data.data(), data.size()), preferred,
            pe::ImportCallback{}, {}, /*resolve_imports=*/false);
        if (load_res.is_err()) {
            preferred = reserve_dynamic_base(parsed.size_of_image);
            load_res = m_pe_loader->load(
                std::span<const u8>(data.data(), data.size()), preferred,
                pe::ImportCallback{}, {}, false);
        }
        if (load_res.is_err()) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
            return 0;
        }
        const auto& img = load_res.value();

        DataFileMapping mapping;
        mapping.base = img.image_base;
        mapping.image_end = img.image_end;
        mapping.header_size = static_cast<u32>(img.header_size);
        mapping.sections = img.sections;
        mapping.path = win_path;
        if (parsed.num_data_directories > pe::DIR_RESOURCE) {
            mapping.resource_rva =
                parsed.data_directories[pe::DIR_RESOURCE].VirtualAddress;
            mapping.resource_size =
                parsed.data_directories[pe::DIR_RESOURCE].Size;
        }
        // LDR_IS_IMAGEMAPPING (bit 1) when AS_IMAGE_RESOURCE is set;
        // otherwise LDR_IS_DATAFILE (bit 0).
        const GuestAddr tag =
            (flags & kAsImageResource) != 0 ? GuestAddr{2} : GuestAddr{1};
        const GuestAddr handle = img.image_base | tag;
        {
            std::unique_lock<HostRwLock> tables(m_tables_mutex);
            data_file_mappings_[handle] = std::move(mapping);
        }
        set_last_error(entry_last_error);
        return handle;
    };

    if (is_explicit_path) {
        if (GuestAddr h = try_map(std::string(name)); h != 0) return h;
        // try_map may have set BAD_EXE_FORMAT; otherwise FILE_NOT_FOUND
        // (Learn/oracle: AS_DATAFILE missing file → ERROR_FILE_NOT_FOUND).
        if (get_last_error() != error_codes::ERROR_BAD_EXE_FORMAT) {
            set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
        }
        return 0;
    }

    auto try_dir = [&](std::string_view directory) -> GuestAddr {
        if (directory.empty()) return 0;
        std::string candidate(directory);
        if (candidate.back() != '\\' && candidate.back() != '/') {
            candidate.push_back('\\');
        }
        candidate += requested_filename;
        return try_map(candidate);
    };

    if (!main_module_path_.empty()) {
        if (GuestAddr h = try_dir(parent_windows_dir(main_module_path_)); h != 0) {
            return h;
        }
    }
    if (m_dll_directory.has_value() && !m_dll_directory->empty()) {
        if (GuestAddr h = try_dir(*m_dll_directory); h != 0) return h;
    }
    if (GuestAddr h = try_dir("C:\\Windows\\System32"); h != 0) return h;
    if (GuestAddr h = try_dir("C:\\Windows\\System"); h != 0) return h;
    if (GuestAddr h = try_dir("C:\\Windows"); h != 0) return h;
    if (!m_dll_directory.has_value() || !m_dll_directory->empty()) {
        if (GuestAddr h = try_dir(m_current_directory); h != 0) return h;
    }

    set_last_error(error_codes::ERROR_FILE_NOT_FOUND);
    return 0;
}


/// Register the thunk-backed synthetic module for @p dll_base (normalized
/// name without extension) so GetModuleHandle / GetProcAddress see it.
/// Returns its base, or 0 when no host implementation exists.
GuestAddr ModuleManager::synthesize_thunk_module(const std::string& dll_base) {
    std::string dll_with_ext = dll_base + ".dll";
    if (!registry_.has_dll(dll_with_ext)) {
        // Try the name as-is for non-.dll extensions (.ocx, .drv, etc.)
        if (dll_base.find('.') != std::string::npos && registry_.has_dll(dll_base)) {
            dll_with_ext = dll_base;
        } else {
            return 0;
        }
    }

    GuestAddr base =
        well_known_synthetic_base(dll_base, m_is_64bit_process,
                                  m_synthetic_module_arena);
    const GuestAddr synthetic_arena = m_is_64bit_process
        ? m_synthetic_module_arena
        : SYNTHETIC_MODULE_BASE_X86;
    if (base == 0) {
        base = synthetic_arena + 0x100000 +
            static_cast<GuestAddr>(modules_by_name_.size()) *
                SYNTHETIC_MODULE_SLOT_SIZE;
    }

    // Ensure the synthesized base does not collide with an already loaded
    // module. Collisions break GetProcAddress because module handle lookup
    // becomes ambiguous.
    auto desired_name = normalize_name(dll_with_ext);
    auto same_or_free = [&](GuestAddr candidate) {
        auto itb = base_to_name_.find(candidate);
        if (itb == base_to_name_.end()) {
            return true;
        }
        return normalized_name_equals(itb->second, desired_name);
    };

    if (!same_or_free(base)) {
        GuestAddr probe = synthetic_arena + 0x100000 +
            static_cast<GuestAddr>(modules_by_name_.size()) *
                SYNTHETIC_MODULE_SLOT_SIZE;
        while (!same_or_free(probe)) {
            probe += SYNTHETIC_MODULE_SLOT_SIZE;
        }
        base = probe;
    }

    // Register under the normalized base name key (no path) so GetModuleHandle works.
    register_module(dll_with_ext, base, std::string("C:\\Windows\\System32\\") + dll_with_ext);
    return base;
}

GuestAddr ModuleManager::ensure_static_import_module(std::string_view dll_name) {
    // Learn "Load-Time Dynamic Linking": the loader loads every DLL an image
    // imports, so a host-implemented DLL bound by static import is a loaded
    // module -- GetModuleHandle finds it by base name in any case, and
    // LoadLibrary returns the same handle (s4_static_import_module_probe).
    // Registering the synthetic module directly (not through the DLL search
    // path) keeps binding on the host implementation the import already
    // resolved to.
    std::lock_guard<std::recursive_mutex> loader_guard(m_loader_mutex);
    const u32 entry_last_error = get_last_error();
    GuestAddr base = get_module_handle(dll_name);
    if (base == 0) {
        const std::string_view host = pe::resolve_api_set(dll_name);
        base = get_module_handle(host);
        if (base == 0) base = synthesize_thunk_module(normalize_name(host));
    }
    set_last_error(entry_last_error);
    return base;
}

} // namespace vitreo::winapi

// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// DLL registry part: msvcrt.dll / ucrtbase.dll / crtdll.dll and the CRT export tables

#include "dll_registry_internal.hpp"

#include "winapi/msvcrt/crt_ordinals.hpp"
#include "winapi/msvcrt/io/crt_printf.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace vitreo::cli {

/// Stamp ExportEntry::ordinal from a dumpbin /exports table. Names absent
/// from the Windows DLL stay ordinal 0 (name-only). Invented sequential
/// ordinals from the CRT registration tables must not survive into
/// ucrtbase/msvcrt — by_ordinal last-wins would map every colliding slot to
/// the wrong export.
// (implementation: winapi/msvcrt/crt_ordinals.cpp)

/// Run every export of one CRT DLL inside its CrtDllFlavorScope (see
/// crt_memory.hpp): the tables are shared, the failure policy is per DLL.
static void tag_crt_flavor(winapi::DllExports& exports,
                           winapi::CrtDllFlavor flavor) {
    for (auto& entry : exports.entries) {
        entry.handler = [flavor, inner = std::move(entry.handler)](
                            winapi::Marshaler& m) {
            winapi::CrtDllFlavorScope scope(flavor);
            inner(m);
        };
    }
}

static winapi::DllExports make_crt_exports(
    RuntimeContext& ctx,
    winapi::crt_stdio_buffer::Layout stdio_layout =
        winapi::crt_stdio_buffer::Layout::LegacyIobuf32) {
    winapi::DllExports exports;
    merge_exports(exports, winapi::create_crt_init_thunks(
        ctx.crt_init_mgr, ctx.crt_data, ctx.crt_environment_mgr));
    merge_exports(exports, winapi::create_crt_string_thunks(
        ctx.crt_str_mgr, ctx.crt_mem_mgr, ctx.vfs));
    // _get_heap_handle has to name a heap the Heap* APIs accept; only the
    // registry sees both managers. Wire CRT malloc/free through that heap so
    // HeapSize/HeapWalk on a malloc'd block succeed (09_crt.md).
    ctx.crt_mem_mgr.set_process_heap_provider(
        [&ctx]() { return ctx.heap_mgr.get_process_heap(); });
    ctx.crt_mem_mgr.set_win32_heap_ops({
        [&ctx](u64 size, u32 flags) {
            return ctx.heap_mgr.heap_alloc(
                ctx.heap_mgr.get_process_heap(), flags, size);
        },
        [&ctx](u64 address) {
            return ctx.heap_mgr.heap_free(
                ctx.heap_mgr.get_process_heap(), 0, address);
        },
        [&ctx](u64 address) {
            return ctx.heap_mgr.heap_size(
                ctx.heap_mgr.get_process_heap(), 0, address);
        },
        [&ctx](u64 address, u64 new_size, u32 flags) {
            return ctx.heap_mgr.heap_realloc(
                ctx.heap_mgr.get_process_heap(), flags, address, new_size);
        },
    });
    ctx.crt_mem_mgr.set_process_terminator(
        [&ctx](vitreo::u32 status) { ctx.process_mgr.exit_process(status); });
    // _configure_narrow_argv(_crt_argv_expanded_arguments) lists directories.
    ctx.crt_data.set_directory_lister(
        [&ctx](const std::string& dir) -> std::optional<std::vector<std::string>> {
            auto listed = ctx.vfs.list_dir(dir.empty() ? ctx.vfs.cwd() : dir);
            if (listed.is_err()) return std::nullopt;
            std::vector<std::string> names;
            for (const auto& e : listed.value()) names.push_back(e.name);
            return names;
        });
    merge_exports(exports, winapi::create_crt_math_thunks(ctx.crt_math_mgr, ctx.crt_mem_mgr));
    ctx.crt_stdio_mgr.set_file_manager(&ctx.file_mgr);
    ctx.crt_stdio_mgr.set_thread_manager(&ctx.thread_mgr);
    ctx.crt_stdio_mgr.set_console_manager(&ctx.console_mgr);
    merge_exports(exports, winapi::create_crt_stdio_thunks(
        ctx.crt_stdio_mgr, ctx.vfs, ctx.crt_mem_mgr, ctx.crt_data,
        stdio_layout));
    merge_exports(exports, winapi::create_crt_process_thunks(ctx.process_mgr, ctx.crt_data, ctx.thread_mgr, ctx.crt_mem_mgr));
    merge_exports(exports, winapi::create_crt_memory_thunks(ctx.crt_mem_mgr));
    merge_exports(exports, winapi::create_crt_environment_thunks(
        ctx.crt_environment_mgr, ctx.vfs));
    merge_exports(exports, winapi::create_crt_locale_thunks(ctx.crt_locale_mgr));
    ctx.crt_time_mgr.set_tz_reader([&env = ctx.crt_environment_mgr]() {
        return env.value("TZ");
    });
    merge_exports(exports, winapi::create_crt_time_thunks(ctx.crt_time_mgr));
    merge_exports(exports, winapi::create_crt_filesystem_thunks(ctx.vfs, ctx.crt_mem_mgr));
    // printf family on the shared output processor (crt_printf.hpp).
    merge_exports(exports, winapi::create_crt_printf_thunks(ctx.crt_mem_mgr));

    // HANDLE _get_heap_handle(void)
    //
    // The Win32 heap the CRT allocates from. Since the UCRT that is the
    // process heap, and callers pass the result straight to HeapAlloc /
    // HeapSize / HeapValidate -- so it has to be a handle those accept, not a
    // token. Registered here rather than in the CRT tables because it is the
    // one CRT export whose answer belongs to the kernel32 heap manager.
    exports.entries.push_back({"_get_heap_handle", 0,
        [&mgr = ctx.heap_mgr](winapi::Marshaler& m) {
            m.set_return(static_cast<u64>(mgr.get_process_heap()));
        }, 0});

    return exports;
}

/// The UCRT publishes an `_o_<name>` alias for most of its exports through
/// api-ms-win-crt-private-l1-1-0.dll. They are not separate functions: MSVC's
/// import library redirects to them so a statically linked CRT and the DLL CRT
/// can coexist in one process, and each behaves exactly like the export it is
/// named after.
///
/// Generating them from the table means the aliases can never drift from the
/// functions they mirror, and a guest that imports `_o_malloc` gets malloc
/// rather than the missing-export stub -- which on x64 answers a
/// 32-bit-shaped 0xFFFFFFFF that no caller recognises as a failure.
static void add_ucrt_o_aliases(winapi::DllExports& exports) {
    const std::size_t original = exports.entries.size();
    std::unordered_set<std::string> existing;
    existing.reserve(original * 2);
    for (const auto& entry : exports.entries) existing.insert(entry.name);

    for (std::size_t i = 0; i < original; ++i) {
        // Aliasing an alias would produce `_o__o_malloc`, which nothing imports.
        if (exports.entries[i].name.rfind("_o_", 0) == 0) continue;
        std::string alias = "_o_" + exports.entries[i].name;
        if (!existing.insert(alias).second) continue;
        winapi::ExportEntry copy = exports.entries[i];
        copy.name = std::move(alias);
        // The alias is not published under the original's ordinal, so it is
        // exported by name only.
        copy.ordinal = 0;
        exports.entries.push_back(std::move(copy));
    }
}

static winapi::DllExports make_crtdll_exports(RuntimeContext& ctx) {
    static constexpr std::array<std::string_view, 20> supported_names{
        "_CIpow", "_ftol", "_fullpath", "_purecall", "_stricmp",
        "_strnicmp", "_strupr", "_vsnprintf", "memmove", "qsort",
        "setlocale", "strncmp", "strpbrk", "strstr", "strtol", "strtoul",
        "toupper", "vsprintf", "wcslen", "wcstombs",
    };

    auto available = make_crt_exports(
        ctx, winapi::crt_stdio_buffer::Layout::LegacyIobuf32);
    winapi::DllExports exports;
    for (auto& entry : available.entries) {
        if (std::find(supported_names.begin(), supported_names.end(), entry.name) ==
            supported_names.end()) {
            continue;
        }
        entry.ordinal = 0;
        exports.entries.push_back(std::move(entry));
    }
    return exports;
}

void register_crt_dlls(RuntimeContext& ctx, bool guest_is_64bit) {
    auto& reg = ctx.thunk_registry;

    // -- msvcrt.dll --
    {
        {
            winapi::DllExports msv = make_crt_exports(
                ctx, winapi::crt_stdio_buffer::Layout::LegacyIobuf32);
            winapi::apply_msvcrt_windows_ordinals(msv, guest_is_64bit);
            tag_crt_flavor(msv, winapi::CrtDllFlavor::Msvcrt);
            (void)reg.register_dll("msvcrt.dll", std::move(msv));
        }
        {
            winapi::DllExports msvcp =
                winapi::create_msvcp80_thunks(ctx.crt_mem_mgr);
            (void)reg.register_dll("msvcp80.dll", std::move(msvcp));
        }
        // mscoree.dll: the OS .NET runtime shim (dll_registry_mscoree.cpp).
        register_mscoree_dll(ctx, guest_is_64bit);
        {
            // The `_o_` aliases are a UCRT contract; legacy msvcrt has none.
            winapi::DllExports ucrt = make_crt_exports(
                ctx, winapi::crt_stdio_buffer::Layout::UcrtOpaque32);
            add_ucrt_o_aliases(ucrt);
            winapi::apply_ucrtbase_windows_ordinals(ucrt, guest_is_64bit);
            tag_crt_flavor(ucrt, winapi::CrtDllFlavor::Ucrt);
            (void)reg.register_dll("ucrtbase.dll", std::move(ucrt));
        }
        {
            // crtdll.dll is a separate PE; leave name-only (ordinal 0) rather
            // than inventing slots or borrowing msvcrt's table wholesale.
            winapi::DllExports crtdll = make_crtdll_exports(ctx);
            for (auto& e : crtdll.entries) e.ordinal = 0;
            tag_crt_flavor(crtdll, winapi::CrtDllFlavor::Msvcrt);
            (void)reg.register_dll("crtdll.dll", std::move(crtdll));
        }
    }

    // -- vitreo-cfg.dll --
    // Internal thunks used to patch MSVC Control Flow Guard load-config
    // pointers in dynamically loaded PE images. The dispatch thunk has custom
    // control-flow handling in execution_loop.cpp; the handler itself is a
    // no-op so it preserves RAX, which carries the guarded indirect-call target.
    {
        winapi::DllExports cfg;
        cfg.entries.push_back({"__guard_check_icall", 0,
            [](winapi::Marshaler& m) {
                (void)m;
            }, 0});
        cfg.entries.push_back({"__guard_dispatch_icall", 0,
            [](winapi::Marshaler& m) {
                (void)m;
            }, 0});
        (void)reg.register_dll("vitreo-cfg.dll", std::move(cfg));
    }
}

} // namespace vitreo::cli

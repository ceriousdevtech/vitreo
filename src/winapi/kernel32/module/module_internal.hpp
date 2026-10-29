// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file module_internal.hpp
/// @brief Shared helpers for ModuleManager translation units.

#include "module.hpp"

#include "capsule/vfs/filesystem.hpp"
#include "core/pe/parser/pe_parser.hpp"

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <vitreo/result.hpp>

namespace vitreo::winapi {

[[nodiscard]] bool force_dll_reloc() noexcept;
[[nodiscard]] bool module_diag_enabled();
[[nodiscard]] bool looks_like_path(std::string_view s);
[[nodiscard]] bool is_absolute_windows_path(std::string_view s);
[[nodiscard]] std::string normalize_path_key(std::string_view path);
[[nodiscard]] bool is_system_dll_path(std::string_view path);
[[nodiscard]] std::string parent_windows_dir(std::string_view win_path);

struct ParsedExports {
    std::unordered_map<std::string, GuestAddr> by_name;
    std::unordered_map<u16, GuestAddr> by_ordinal;

    /// Forwarded exports, keyed the same way, holding the raw
    /// "TargetDll.ExportName" (or "TargetDll.#123") string from the export
    /// directory rather than an address. An export whose EAT entry points
    /// back inside the export directory is a forwarder, not code, so it can
    /// only be resolved by loading the named module — which is a loader
    /// action, not a parse action. See Microsoft Learn, "Export Address
    /// Table" in the PE format specification.
    std::unordered_map<std::string, std::string> forwarders_by_name;
    std::unordered_map<u16, std::string> forwarders_by_ordinal;
};

struct SyntheticPeImage {
    std::vector<Byte> bytes;
    ParsedExports exports;
    GuestAddr executable_rva{0};
    usize executable_size{0};
    u32 exception_rva{0};
    u32 exception_size{0};
};

/// @p register_aliases records the ntdll service veneers' ordinal and
/// dispatch aliases at @p image_base in @p registry, which an image mapped
/// there needs; an image built only as a file's bytes must not (its base is
/// not the module's, or nothing is mapped there). The bytes are the same.
[[nodiscard]] SyntheticPeImage build_synthetic_pe_image(
    GuestAddr image_base,
    bool is_64bit,
    std::string_view dll_name,
    ThunkRegistry& registry,
    bool register_aliases = true);

[[nodiscard]] GuestAddr well_known_synthetic_base(
    std::string_view dll_base,
    bool is_64bit,
    GuestAddr x64_synthetic_arena);

/// Capsule VFS whole-file read (module-local name; avoids colliding with
/// user32_ext's identically-named helper in the same linkage unit).
[[nodiscard]] Result<std::vector<u8>, capsule::VfsError> module_read_all_bytes(
    capsule::Vfs& vfs,
    std::string_view win_path);

[[nodiscard]] ParsedExports parse_exports_from_file(
    std::span<const u8> pe_data,
    const pe::ParsedPe& pe,
    GuestAddr image_base);

void patch_guard_cf_load_config(GuestMemory& memory,
                                ThunkRegistry& registry,
                                const pe::ParsedPe& pe,
                                GuestAddr image_base,
                                const char* dbg_path);

[[nodiscard]] GuestAddr lookup_runtime_function_in_table(GuestMemory& memory,
                                                         GuestAddr base,
                                                         u32 exception_rva,
                                                         u32 exception_size,
                                                         GuestAddr control_pc);

} // namespace vitreo::winapi

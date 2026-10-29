// SPDX-License-Identifier: LGPL-2.1-only

/// @file module.cpp
/// @brief Implementation of module management APIs (WIN32-033)

#include "winapi/kernel32/module/system_programs.hpp"
#include "module.hpp"
#include "module_internal.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/string_conv.hpp"
#include "winapi/core/marshaler.hpp"
#include "ipc/ipc_gate.hpp"

#include "core/pe/parser/pe_parser.hpp"
#include "core/pe/imports/api_set_resolver.hpp"
#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include <vitreo/result.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <optional>
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

/// @file registration.cpp
/// @brief Registration
///
/// Closed cut from module.cpp — verified with plan_extraction.py.

namespace vitreo::winapi {

// =============================================================================
// Registration
// =============================================================================

void ModuleManager::register_module(std::string_view name,
                                     GuestAddr base,
                                     std::string_view path) {
    std::optional<SyntheticPeImage> image;
    if (m_memory != nullptr &&
        !m_memory->is_mapped(base, SYNTHETIC_MODULE_SLOT_SIZE)) {
        auto mapped = m_memory->map_region(
            base, SYNTHETIC_MODULE_SLOT_SIZE, MemoryProtection::ReadWrite);
        if (mapped.is_ok()) {
            image = build_synthetic_pe_image(
                base, m_is_64bit_process, name, registry_);
            (void)m_memory->write_bytes(
                base, image->bytes.data(), image->bytes.size());
            (void)m_memory->protect_region(
                base, SYNTHETIC_MODULE_SLOT_SIZE,
                MemoryProtection::ReadExecute);
        } else {
            std::fprintf(
                stderr,
                "[module] failed to map synthetic image name=\"%.*s\" "
                "base=0x%llx\n",
                static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long long>(base));
        }
    }

    // The module's file holds the image it has in memory at this base, so a
    // guest that opens GetModuleFileNameW's path sees the module it loaded
    // (a read before this load may have published one at the default base).
    if (m_vfs != nullptr && !path.empty() && registry_.has_dll(name)) {
        if (!image) {
            image = build_synthetic_pe_image(base, m_is_64bit_process, name,
                                             registry_,
                                             /*register_aliases=*/false);
        }
        const auto published = m_vfs->publish_generated_file(
            path, std::make_shared<const std::vector<u8>>(image->bytes));
        if (published.is_err()) {
            std::fprintf(
                stderr,
                "[module] failed to publish synthetic file path=\"%.*s\" "
                "error=%s\n",
                static_cast<int>(path.size()), path.data(),
                capsule::vfs_error_to_string(published.error()));
        }
    }

    std::string key = normalize_name(name);

    ModuleInfo info;
    info.base_address = base;
    info.name         = std::string(name);
    info.path         = std::string(path);
    info.ref_count    = 1;
    info.synthetic_image = true;

    {
        std::unique_lock<HostRwLock> tables(m_tables_mutex);
        base_to_name_[base] = key;
        modules_by_name_[key] = std::move(info);
    }
}

void ModuleManager::materialize_synthetic_modules() {
    if (m_memory == nullptr) return;

    for (auto& [key, info] : modules_by_name_) {
        (void)key;
        if (!info.synthetic_image || !registry_.has_dll(info.name)) continue;

        if (!m_memory->is_mapped(
                info.base_address, SYNTHETIC_MODULE_SLOT_SIZE)) {
            auto mapped = m_memory->map_region(
                info.base_address, SYNTHETIC_MODULE_SLOT_SIZE,
                MemoryProtection::ReadWrite);
            if (mapped.is_err()) {
                std::fprintf(
                    stderr,
                    "[module] failed to materialize synthetic image "
                    "name=\"%s\" base=0x%llx\n",
                    info.name.c_str(),
                    static_cast<unsigned long long>(info.base_address));
                continue;
            }
        } else {
            auto writable = m_memory->protect_region(
                info.base_address, SYNTHETIC_MODULE_SLOT_SIZE,
                MemoryProtection::ReadWrite);
            if (writable.is_err()) {
                std::fprintf(
                    stderr,
                    "[module] failed to make synthetic image writable "
                    "name=\"%s\" base=0x%llx\n",
                    info.name.c_str(),
                    static_cast<unsigned long long>(info.base_address));
                continue;
            }
        }

        auto image = build_synthetic_pe_image(
            info.base_address, m_is_64bit_process, info.name, registry_);
        auto file_bytes =
            std::make_shared<const std::vector<u8>>(image.bytes);
        const auto written = m_memory->write_bytes(
            info.base_address, image.bytes.data(), image.bytes.size());
        const auto protected_image = m_memory->protect_region(
            info.base_address, SYNTHETIC_MODULE_SLOT_SIZE,
            MemoryProtection::Read);
        const auto protected_code = m_memory->protect_region(
            info.base_address + image.executable_rva,
            image.executable_size, MemoryProtection::ReadExecute);
        if (written.is_err() || protected_image.is_err() ||
            protected_code.is_err()) {
            std::fprintf(
                stderr,
                "[module] failed to finalize synthetic image name=\"%s\" "
                "base=0x%llx\n",
                info.name.c_str(),
                static_cast<unsigned long long>(info.base_address));
        } else {
            info.exports_by_name = std::move(image.exports.by_name);
            info.exports_by_ordinal = std::move(image.exports.by_ordinal);
            info.forwarders_by_name = std::move(image.exports.forwarders_by_name);
            info.forwarders_by_ordinal = std::move(image.exports.forwarders_by_ordinal);
            info.exception_rva = image.exception_rva;
            info.exception_size = image.exception_size;
            if (m_vfs != nullptr && !info.path.empty()) {
                const auto published =
                    m_vfs->publish_generated_file(info.path, file_bytes);
                if (published.is_err()) {
                    std::fprintf(
                        stderr,
                        "[module] failed to publish synthetic file "
                        "path=\"%s\" error=%s\n",
                        info.path.c_str(),
                        capsule::vfs_error_to_string(published.error()));
                }
            }
        }
    }
}

namespace {

/// The built-in DLL directory @p path names, or none: "system32" or
/// "syswow64" under C:\Windows, with @p leaf the rest when it is one name.
std::optional<std::string> builtin_dll_directory(std::string_view path,
                                                 std::string* leaf) {
    std::string lower;
    lower.reserve(path.size());
    for (char c : path) {
        lower.push_back(c == '/' ? '\\'
                                 : static_cast<char>(std::tolower(
                                       static_cast<unsigned char>(c))));
    }
    std::string_view rest = lower;
    if (rest.starts_with("\\\\?\\")) rest.remove_prefix(4);
    constexpr std::string_view kWindows = "c:\\windows\\";
    if (!rest.starts_with(kWindows)) return std::nullopt;
    rest.remove_prefix(kWindows.size());
    for (std::string_view dir : {std::string_view("system32"),
                                 std::string_view("syswow64")}) {
        if (!rest.starts_with(dir)) continue;
        std::string_view tail = rest.substr(dir.size());
        if (leaf == nullptr) {
            while (!tail.empty() && tail.front() == '\\') tail.remove_prefix(1);
            if (!tail.empty()) return std::nullopt;
            return std::string(dir);
        }
        if (tail.size() < 2 || tail.front() != '\\' ||
            tail.find('\\', 1) != std::string_view::npos) {
            return std::nullopt;
        }
        *leaf = std::string(tail.substr(1));
        return std::string(dir);
    }
    return std::nullopt;
}

}  // namespace

std::shared_ptr<const std::vector<u8>> ModuleManager::builtin_dll_file(
    std::string_view path) {
    std::string leaf;
    const auto dir = builtin_dll_directory(path, &leaf);
    if (!dir) return nullptr;
    if (is_system_program(leaf)) {
        return std::make_shared<const std::vector<u8>>(
            build_system_program_image(*dir == "system32"));
    }
    if (!registry_.has_dll(leaf) || normalize_name(leaf) == normalize_name(kSystemProgramDll)) {
        return nullptr;
    }
    // The VFS has already applied WOW64 file-system redirection, so a path
    // that still names System32 is the native directory (a 64-bit process,
    // or a 32-bit one with redirection off or through Sysnative), and
    // SysWOW64 is the x86 one in either process.
    const bool image_64bit = *dir == "system32";
    const std::string key = normalize_name(leaf);
    GuestAddr base = 0;
    if (image_64bit == m_is_64bit_process) {
        std::shared_lock<HostRwLock> tables(m_tables_mutex);
        if (const auto it = modules_by_name_.find(key);
            it != modules_by_name_.end() && it->second.synthetic_image) {
            base = it->second.base_address;
        }
    }
    if (base == 0) {
        // Not loaded: the base this module takes when it is, or the arena's.
        const GuestAddr arena = image_64bit ? m_synthetic_module_arena
                                            : SYNTHETIC_MODULE_BASE_X86;
        base = well_known_synthetic_base(key, image_64bit, arena);
        if (base == 0) base = arena;
    }
    return std::make_shared<const std::vector<u8>>(
        build_synthetic_pe_image(base, image_64bit, leaf, registry_,
                                 /*register_aliases=*/false)
            .bytes);
}

GuestAddr ModuleManager::load_builtin_dll_file(std::string_view win_path) {
    std::string leaf;
    const auto dir = builtin_dll_directory(win_path, &leaf);
    if (!dir || (*dir == "syswow64" && m_is_64bit_process) ||
        !registry_.has_dll(leaf) || m_vfs == nullptr ||
        !m_vfs->is_generated_file(win_path)) {
        return 0;
    }
    return synthesize_thunk_module(normalize_name(leaf));
}

std::vector<std::pair<std::string, u64>> ModuleManager::builtin_dll_files_in(
    std::string_view directory) const {
    std::vector<std::pair<std::string, u64>> files;
    if (!builtin_dll_directory(directory, nullptr)) return files;
    for (std::string& name : registry_.dll_names()) {
        if (normalize_name(name) == normalize_name(kSystemProgramDll)) continue;
        files.emplace_back(std::move(name), SYNTHETIC_MODULE_SLOT_SIZE);
    }
    for (std::string& name : system_program_names()) {
        const usize size = build_system_program_image(false).size();
        files.emplace_back(std::move(name), size);
    }
    return files;
}

void ModuleManager::set_main_module(GuestAddr base, std::string_view path,
                                    u32 exception_rva, u32 exception_size) {
    main_module_base_ = base;
    main_module_path_ = std::string(path);
    main_module_exception_rva_ = exception_rva;
    main_module_exception_size_ = exception_size;
}

void ModuleManager::register_main_module_exports(GuestAddr base,
                                                  std::string_view path,
                                                  GuestAddr image_end,
                                                  u32 bitness,
                                                  std::span<const u8> pe_bytes) {
    // Derive a key the same way LoadLibrary does: short basename, lowercased,
    // with a ".dll" suffix stripped while preserving ".exe".
    std::string short_name(path);
    auto sep = short_name.find_last_of("\\/");
    if (sep != std::string::npos) short_name = short_name.substr(sep + 1);
    std::string key = normalize_name(short_name);

    ModuleInfo info;
    info.base_address    = base;
    info.image_end       = image_end;
    info.bitness         = bitness;
    info.exception_rva   = main_module_exception_rva_;
    info.exception_size  = main_module_exception_size_;
    info.name            = short_name;
    info.path            = std::string(path);
    info.ref_count       = 1;
    info.state           = ModuleInfo::State::Loaded;
    info.dllmain_called  = true;  // CRT init for the main EXE is the loop driver

    if (!pe_bytes.empty()) {
        auto parsed_pe = pe::parse_pe(pe_bytes);
        if (parsed_pe.is_ok()) {
            auto ex = parse_exports_from_file(pe_bytes, parsed_pe.value(), base);
            info.exports_by_name = std::move(ex.by_name);
            info.exports_by_ordinal = std::move(ex.by_ordinal);
            info.forwarders_by_name = std::move(ex.forwarders_by_name);
            info.forwarders_by_ordinal = std::move(ex.forwarders_by_ordinal);
        }
    }

    {
        std::unique_lock<HostRwLock> tables(m_tables_mutex);
        base_to_name_[base] = key;
        modules_by_name_[key] = std::move(info);
    }
}

} // namespace vitreo::winapi

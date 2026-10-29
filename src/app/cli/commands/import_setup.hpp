// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Import setup: CRT data allocation and IAT patching

#pragma once

#include "run.hpp"
#include <span>
#include <string>
#include <string_view>
#include <vector>

class GuestMemory;

namespace vitreo::pe { struct ParsedPe; }

namespace vitreo::cli {

struct RuntimeContext;

/// Return the application-local DLL search directories for a main executable.
/// Windows searches the executable's directory before the process current
/// directory; they are often different for CreateProcess children.
[[nodiscard]] std::vector<std::string> main_import_search_directories(
    std::string_view main_module_path,
    std::string_view current_directory,
    std::span<const std::string> private_assemblies = {});

/// Initialise CRT data block and resolve/patch all PE imports.
///
/// Steps:
///   1. Allocate CrtDataBlock in guest memory
///   2. Build ImportCallback that checks CRT data, direct thunks, API sets
///   3. Call pe::resolve_and_patch_imports()
///
/// @param ctx      Runtime context (thunk_registry + crt_data used)
/// @param memory   Guest memory
/// @param pe       Parsed PE headers
/// @param pe_data  Raw PE file bytes
/// @param base_address  Guest base address of the mapped image
/// @param verbose  Enable verbose import logging
/// @return RunResult (success=true if imports resolved or no imports present;
///         success=false with STATUS_DLL_NOT_FOUND if a required non-host
///         DLL could not be located)
RunResult setup_imports(RuntimeContext& ctx,
                        GuestMemory& memory,
                        const pe::ParsedPe& pe,
                        std::span<const u8> pe_data,
                        u64 base_address,
                        bool verbose);

} // namespace vitreo::cli

#pragma once

/// @file load_config.hpp
/// @brief Windows loader processing for PE load-configuration data.

#include "core/pe/parser/pe_parser.hpp"
#include "core/memory/guest_memory.hpp"

namespace vitreo::pe {

/// Initialize the image security cookie described by
/// IMAGE_LOAD_CONFIG_DIRECTORY{32,64}.
///
/// Windows initializes this cookie while loading the image, before any image
/// code executes. Returns false when a present load-config directory or cookie
/// pointer cannot be read or written.
[[nodiscard]] bool initialize_load_config_security_cookie(
    GuestMemory& memory,
    const ParsedPe& pe,
    GuestAddr image_base);

/// Guest-memory addresses of the Control Flow Guard fallback stubs.
struct GuardCfStubs {
    GuestAddr check = 0;     ///< `ret`, the MSVC `_guard_check_icall_nop` shape
    GuestAddr dispatch = 0;  ///< `jmp eax`/`jmp rax`, `_guard_dispatch_icall_nop`
};

/// Map (once per process) a page of real guest code holding the two CFG
/// fallback stubs, and return their addresses.
///
/// Windows points GuardCFCheckFunctionPointer at a bare `ret` and
/// GuardCFDispatchFunctionPointer at a bare `jmp rax` whenever CFG is not
/// being enforced for the process. Vitreo maintains no CFG bitmap, so that is
/// always the case here. Publishing the stubs as guest code rather than host
/// thunks keeps every guarded indirect call inside generated code.
///
/// Returns zeroed addresses when the page cannot be mapped; callers then fall
/// back to their host thunk addresses.
[[nodiscard]] GuardCfStubs install_guard_cf_stubs(GuestMemory& memory);

/// Initialize the image's Control Flow Guard function-pointer variables.
///
/// GuardCFCheckFunctionPointer and GuardCFDispatchFunctionPointer are VAs of
/// writable pointer variables in the mapped image, not direct call targets.
/// Windows initializes those variables while loading every image. Returns
/// false when a present load-config field or target variable cannot be read or
/// written.
[[nodiscard]] bool initialize_load_config_guard_cf(
    GuestMemory& memory,
    const ParsedPe& pe,
    GuestAddr image_base,
    GuestAddr check_target,
    GuestAddr dispatch_target);

} // namespace vitreo::pe

// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// Private surface shared by the run*.cpp parts

#pragma once

/// @file run_internal.hpp
/// @brief Phases of execute_pe, split out of run.cpp into their own files.
///
/// Private to the `run` command. execute_pe used to be one function; its
/// phases are free functions in vitreo::cli so each can live in its own
/// translation unit. They take the locals they used as parameters, spelled
/// as before, so the lambdas they install capture the same names they always
/// did. They must be called in the order execute_pe calls them. The public
/// entry points stay in run.hpp.

#include "run.hpp"
#include "external_app_mapping.hpp"
#include "guest_thread_context.hpp"

#include <vitreo/common.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vitreo {
class GuestMemory;
}
namespace vitreo::cpu {
struct CpuState;
}
namespace vitreo::cpu::jit {
class LinearReservationRegistration;
}
namespace vitreo::ipc {
class ProcessRegistration;
class RemoteMemoryServer;
}
namespace vitreo::pe {
struct ParsedPe;
struct MappedImage;
struct ImagePlacement;
struct TebInfo;
}

namespace vitreo::cli {

struct RuntimeContext;

// -----------------------------------------------------------------------------
// run_image_load.cpp
// -----------------------------------------------------------------------------

/// Read an entire host file into a byte vector.
RunResult read_file_bytes(const std::string& path, std::vector<u8>& out);

/// Read an entire file from a mounted optical image into a byte vector.
RunResult read_image_file_bytes(const std::string& capsule_root,
                                const std::string& windows_path,
                                std::vector<u8>& out);

/// Step 4: apply base relocations, the /GS cookie and the ImageBase fix-up
/// to the freshly mapped main image.
[[nodiscard]] std::optional<RunResult> apply_main_image_relocations(
    const RunConfig& config, GuestMemory& memory, const pe::ParsedPe& pe,
    const std::vector<u8>& pe_data, const pe::MappedImage& image,
    const pe::ImagePlacement& placement);

// -----------------------------------------------------------------------------
// run_memory_model.cpp
// -----------------------------------------------------------------------------

/// Step 3: choose and enable the guest memory model (identity, 32-bit linear
/// or x64 linear window) before any region is mapped.
void configure_guest_memory_model(
    GuestMemory& memory,
    cpu::jit::LinearReservationRegistration& linear_fault_registration,
    const pe::ParsedPe& pe);

// -----------------------------------------------------------------------------
// run_main_tls.cpp
// -----------------------------------------------------------------------------

/// Step 8b2: publish the main image TLS directory (or an empty TLS array) in
/// the primary TEB and report the template location and callback array.
[[nodiscard]] std::optional<RunResult> setup_main_image_tls(
    const RunConfig& config, GuestMemory& memory, const pe::ParsedPe& pe,
    const pe::MappedImage& image, const pe::TebInfo& teb_info,
    u64& pe_tls_data_start, u32& pe_tls_data_size,
    u64& pe_tls_callbacks_addr);

// -----------------------------------------------------------------------------
// run_thunk_region.cpp
// -----------------------------------------------------------------------------

/// Step 5b: back the thunk region with executable guest memory and install
/// the API-return stubs.
void map_thunk_region(GuestMemory& memory, RuntimeContext& ctx,
                      const pe::ParsedPe& pe);

// -----------------------------------------------------------------------------
// run_launch_setup.cpp
// -----------------------------------------------------------------------------

/// Step 2: decide whether the process is elevated, prompting for authorization
/// when the manifest (or installer detection) asks for it.
[[nodiscard]] std::optional<RunResult> resolve_launch_elevation(
    const RunConfig& config, const pe::ParsedPe& pe,
    const std::vector<u8>& pe_data, const std::string& resolved_path,
    std::string& elevation_credential, bool& process_elevated);

/// Run a Windows installer package (`msiexec /i` or `/p`) instead of loading
/// it as a PE. nullopt when @p config's program is not an installer package.
[[nodiscard]] std::optional<RunResult> run_installer_package(
    const RunConfig& config, const std::string& capsule_root,
    const std::string& capsule_name, const std::string& resolved_path,
    bool pe_from_image);

/// Host path of the capsule named `capsule_name` ("default" when empty):
/// `$HOME/.vitreo/capsules/<name>`. Creates nothing.
[[nodiscard]] std::string capsule_root_for(std::string_view capsule_name);

/// Step 0.5: the host path execute_pe reads for `pe_path`. A Windows path
/// (`C:\...`) maps into the capsule's drive tree, except on a mounted optical
/// image, where it is read through the image (`pe_from_image`). Reads the
/// capsule's mount records; creates nothing.
[[nodiscard]] std::string resolve_pe_host_path(const std::string& pe_path,
                                               const std::string& capsule_root,
                                               bool& pe_from_image);

/// The Windows-side exe path, when it is decided by the arguments and the
/// capsule's layout alone: a relaunch's `--relaunched-as` path, a mounted
/// image, or an executable inside the capsule's drive tree. nullopt for an
/// executable outside the capsule, whose guest name is only chosen when its
/// directory is published (resolve_windows_exe_path). Touches nothing.
[[nodiscard]] std::optional<std::string> known_windows_exe_path(
    const RunConfig& config, const std::string& capsule_root,
    const std::string& resolved_path, bool pe_from_image);

/// The command line GetCommandLineW returns: `config.guest_cmdline` verbatim
/// when the launcher supplied one, else `"<win_exe_path>" <args>`.
[[nodiscard]] std::string guest_command_line(const RunConfig& config,
                                             const std::string& win_exe_path);

/// Step 5: compute the Windows-side exe path, publishing the exe directory
/// under C:\Vitreo\Apps when it is not already inside the capsule.
[[nodiscard]] std::optional<RunResult> resolve_windows_exe_path(
    const RunConfig& config, const std::string& capsule_root,
    const std::string& capsule_name, const std::string& resolved_path,
    bool pe_from_image, std::string& win_exe_path,
    ExternalAppMapping& app_mapping);

// -----------------------------------------------------------------------------
// run_process_setup.cpp
// -----------------------------------------------------------------------------

/// Step 5: configure the freshly constructed RuntimeContext from the manifest,
/// the launch config and inherited handles.
[[nodiscard]] std::optional<RunResult> configure_process_context(
    RuntimeContext& ctx, const RunConfig& config, const pe::ParsedPe& pe,
    const std::vector<u8>& pe_data, const std::string& win_exe_path);

/// Step 5: make sure the standard Windows directory tree exists in the capsule.
void ensure_capsule_directory_skeleton(RuntimeContext& ctx);

/// Step 5: register the main executable with the module manager and wire its
/// resources, process registration and application icon.
void register_main_module(
    RuntimeContext& ctx, GuestMemory& memory, const RunConfig& config,
    const pe::ParsedPe& pe, const std::vector<u8>& pe_data,
    const pe::MappedImage& image, const std::string& win_exe_path,
    std::optional<ipc::ProcessRegistration>& process_registration);

// -----------------------------------------------------------------------------
// run_remote_memory.cpp
// -----------------------------------------------------------------------------

/// Install the cross-process window, message and guest-call handlers on the
/// process memory endpoint (before it starts serving).
void install_remote_memory_handlers(
    ipc::RemoteMemoryServer& remote_memory_server, RuntimeContext& ctx,
    GuestMemory& memory, bool is_64bit, u64 peb_address,
    u64 pe_tls_data_start, u32 pe_tls_data_size,
    std::optional<GuestThreadContext>& com_rpc_thread_context,
    bool& com_rpc_jit_initialized);

// -----------------------------------------------------------------------------
// run_callbacks_*.cpp - Step 8c: guest-call boundaries for the managers
// -----------------------------------------------------------------------------

/// run_callbacks_window.cpp: WinHTTP, window-procedure, COM-caller and dialog
/// callbacks.
void install_window_message_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx,
    bool is_64bit, u64 peb_address, u64 pe_tls_data_start,
    u32 pe_tls_data_size);

/// run_callbacks_thread.cpp: guest thread creation and execution.
void install_thread_callbacks(
    GuestMemory& memory, RuntimeContext& ctx, bool is_64bit,
    u64 peb_address, u64 pe_tls_data_start, u32 pe_tls_data_size);

/// run_callbacks_media.cpp: multimedia timers, waveOut and FLS cleanup.
void install_media_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx,
    bool is_64bit, u64 peb_address, u64 pe_tls_data_start,
    u32 pe_tls_data_size);

/// run_callbacks_runtime.cpp: CRT initializers and exception filters.
void install_crt_and_exception_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx,
    bool is_64bit);

/// run_callbacks_runtime.cpp: COM, module, service and common-control guest
/// calls.
void install_module_and_com_callbacks(
    cpu::CpuState& cpu, GuestMemory& memory, RuntimeContext& ctx);

// -----------------------------------------------------------------------------
// run_lifecycle.cpp
// -----------------------------------------------------------------------------

/// Step 9: run the primary guest thread to completion (checkpoint restore,
/// DllMain, TLS callbacks, execution loop, checkpoint save).
[[nodiscard]] RunResult run_guest_primary_thread(
    const RunConfig& config, RuntimeContext& ctx, GuestMemory& memory,
    cpu::CpuState& cpu, const pe::MappedImage& image, bool is_64bit,
    u64 pe_tls_callbacks_addr, const std::string& win_exe_path);

/// Tear the guest process down: shutdown cascade, worker wait, registry save
/// and external app mapping hand-off.
void shut_down_guest_process(RuntimeContext& ctx, const RunResult& result,
                             const RunConfig& config,
                             ExternalAppMapping& app_mapping);

} // namespace vitreo::cli

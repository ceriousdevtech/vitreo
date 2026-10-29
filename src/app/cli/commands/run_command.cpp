// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
// CLI-002: vitreoctl run command - argument parsing and chain-load loop

#include "run.hpp"
#include "core/cpu/jit/jit_compiler.hpp"
#include "ipc/process_handle.hpp"
#include "ipc/process_session.hpp"
#include "platform/macos/elevation_authorization.hpp"
#include "winapi/kernel32/error.hpp"
#include <vitreo/diagnostics.hpp>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>

namespace vitreo::cli {

// =============================================================================
// Helpers
// =============================================================================

[[nodiscard]] static std::optional<std::string> hex_decode(
    std::string_view encoded) {
    if ((encoded.size() & 1u) != 0) return std::nullopt;
    auto digit = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
        if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
        return -1;
    };
    std::string decoded;
    decoded.reserve(encoded.size() / 2);
    for (usize offset = 0; offset < encoded.size(); offset += 2) {
        const int high = digit(encoded[offset]);
        const int low = digit(encoded[offset + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        decoded.push_back(static_cast<char>((high << 4) | low));
    }
    return decoded;
}

void assign_launch_identity(RunConfig& config, const ParsedArgs& args) {
    config.pe_path = args.positional_args[0];
    if (args.relaunched_as.has_value()) {
        config.relaunched_as = args.relaunched_as.value();
    }
    if (args.guest_cmdline.has_value()) {
        config.guest_cmdline = args.guest_cmdline.value();
    }
    // Any additional positional args are treated as guest command line tokens.
    // Use `--` to pass args that begin with '-' or '--'.
    if (args.positional_args.size() > 1) {
        config.guest_args.assign(args.positional_args.begin() + 1,
                                 args.positional_args.end());
    }
    if (args.capsule_name.has_value()) {
        config.capsule_name = args.capsule_name.value();
    }
}

// =============================================================================
// RunCommand::execute
// =============================================================================

CommandResult RunCommand::execute(const ParsedArgs& args) {
    // Require at least one positional argument (the PE path)
    if (args.positional_args.empty()) {
        return CommandResult::failure(
            "Usage: vitreo run <pe-path> [--capsule <name>] "
            "[--app-name <name>] [--verbose] [-- <args...>]");
    }

    ipc::ProcessSession process_session;
    auto elevation_authorizer =
        platform::create_elevation_authorizer();
    RunConfig config;
    config.process_session = &process_session;
    config.elevation_authorizer = elevation_authorizer.get();
    if (args.guest_elevation_credential.has_value()) {
        auto credential = hex_decode(
            *args.guest_elevation_credential);
        if (!credential.has_value() ||
            !elevation_authorizer->validate_credential(*credential)) {
            return CommandResult::failure(
                "Invalid guest elevation authorization credential",
                static_cast<int>(
                    winapi::error_codes::ERROR_ACCESS_DENIED));
        }
        config.elevation_credential = std::move(*credential);
    }
    if (args.guest_token_context_fd.has_value()) {
        int capability_fd = -1;
        const std::string_view text = *args.guest_token_context_fd;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), capability_fd);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size()) {
            return CommandResult::failure(
                "Invalid guest token-context capability descriptor",
                static_cast<int>(winapi::error_codes::ERROR_ACCESS_DENIED));
        }
        auto token_context =
            ipc::consume_worker_token_context_capability(capability_fd);
        if (!token_context.has_value()) {
            return CommandResult::failure(
                "Invalid guest token-context capability",
                static_cast<int>(winapi::error_codes::ERROR_ACCESS_DENIED));
        }
        config.token_context = *token_context;
    }
    // A supervised worker inherits the read end of its owner's parent-death
    // pipe. Start watching it before running guest code so an abnormal death of
    // the owning process tears this worker (and its descendants) down instead of
    // orphaning them. The top-level `vitreo run` has no such descriptor.
    if (args.parent_death_fd.has_value()) {
        int death_fd = -1;
        const std::string_view text = *args.parent_death_fd;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), death_fd);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() || death_fd < 0) {
            return CommandResult::failure(
                "Invalid parent-death descriptor",
                static_cast<int>(winapi::error_codes::ERROR_INVALID_PARAMETER));
        }
        ipc::install_parent_death_watch(death_fd);
    }
    if (args.guest_parent_pid.has_value()) {
        const std::string_view text = *args.guest_parent_pid;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(),
            config.guest_parent_process_id);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() ||
            config.guest_parent_process_id == 0) {
            return CommandResult::failure(
                "Invalid guest parent process id",
                static_cast<int>(winapi::error_codes::ERROR_INVALID_PARAMETER));
        }
    }
    int guest_exit_code_fd = -1;
    if (args.guest_exit_code_fd.has_value()) {
        const std::string_view text = *args.guest_exit_code_fd;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), guest_exit_code_fd);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size() || guest_exit_code_fd < 0) {
            return CommandResult::failure(
                "Invalid guest-exit-code descriptor",
                static_cast<int>(winapi::error_codes::ERROR_INVALID_PARAMETER));
        }
    }
    assign_launch_identity(config, args);
    config.verbose = args.verbose || args.trace;
    auto parse_u64_option = [](const std::optional<std::string>& value,
                               const char* option,
                               u64& output) -> std::optional<std::string> {
        if (!value.has_value()) return std::nullopt;
        const std::string_view text = *value;
        const auto parsed = std::from_chars(
            text.data(), text.data() + text.size(), output);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != text.data() + text.size()) {
            return std::string("Invalid value for ") + option;
        }
        return std::nullopt;
    };
    if (args.checkpoint_save.has_value()) {
        config.checkpoint_save_path = *args.checkpoint_save;
    }
    if (args.checkpoint_restore.has_value()) {
        config.checkpoint_restore_path = *args.checkpoint_restore;
    }
    if (auto error = parse_u64_option(
            args.checkpoint_at, "--checkpoint-at", config.checkpoint_at)) {
        return CommandResult::failure(*error);
    }
    if (auto error = parse_u64_option(
            args.replay_instructions, "--replay-instructions",
            config.replay_instructions)) {
        return CommandResult::failure(*error);
    }
    if (auto error = parse_u64_option(
            args.replay_seed, "--replay-seed",
            config.deterministic_rng_seed)) {
        return CommandResult::failure(*error);
    }
    if (args.checkpoint_at.has_value() && config.checkpoint_at == 0) {
        return CommandResult::failure(
            "--checkpoint-at requires a positive instruction count");
    }
    if (args.replay_instructions.has_value() &&
        config.replay_instructions == 0) {
        return CommandResult::failure(
            "--replay-instructions requires a positive instruction count");
    }
    if (config.checkpoint_at != 0 && config.checkpoint_save_path.empty()) {
        return CommandResult::failure(
            "--checkpoint-at requires --checkpoint-save");
    }
    if (config.replay_instructions != 0 &&
        config.checkpoint_restore_path.empty()) {
        return CommandResult::failure(
            "--replay-instructions requires --checkpoint-restore");
    }
    if (config.checkpoint_at != 0 && config.replay_instructions != 0) {
        return CommandResult::failure(
            "--checkpoint-at and --replay-instructions cannot be combined");
    }
    config.max_instructions = config.replay_instructions != 0
        ? config.replay_instructions
        : config.checkpoint_at;
    if (args.app_name.has_value()) {
        config.app_name = args.app_name.value();
    }

    // --interpreter / --no-jit: force pure-interpreter execution. The JIT is
    // gated on the VITREO_NO_JIT env var (read in the execution loop), so set
    // it here for the in-process guest run.
    if (args.interpreter || config.checkpoint_at != 0 ||
        config.replay_instructions != 0) {
        ::setenv("VITREO_NO_JIT", "1", 1);
    }
    if (args.guest_cwd.has_value()) {
        config.guest_cwd = args.guest_cwd.value();
    }
    if (args.conpty_handles.has_value()) {
        config.conpty_handles = args.conpty_handles.value();
    }
    if (args.inherited_pipes.has_value()) {
        config.inherited_pipes = args.inherited_pipes.value();
    }
    if (args.inherited_sync.has_value()) {
        config.inherited_sync = args.inherited_sync.value();
    }
    if (args.inherited_mappings.has_value()) {
        config.inherited_mappings = args.inherited_mappings.value();
    }
    config.guest_service_arguments_utf16 =
        args.guest_service_arguments_utf16;

    for (const auto& entry : args.guest_environment) {
        const auto separator = guest_environment_separator(entry);
        config.guest_environment.emplace_back(
            entry.substr(0, separator), entry.substr(separator + 1));
    }

    auto result = execute_pe(config);

    // Chain-load loop: if the guest requested launching another .exe
    // (via ShellExecuteW), run it in the same Vitreo process so we
    // reuse the Cocoa app context and windows appear seamlessly.
    while (result.success && !result.pending_launch_path.empty()) {
        VITREO_TRACE("[vitreo] Chain-loading: '%s'\n",
                     result.pending_launch_path.c_str());

        // Drop the process-wide JIT compile memo before re-entering
        // execute_pe.  The memo maps guest VAs to host code addresses in
        // the shared JIT arena; that host code embeds pointers to the
        // *previous* run's BlockCache L1 array (stack-allocated in
        // run_execution_loop) and into the previous run's GuestMemory.
        // Both are destroyed when execute_pe returns, so any reuse on
        // the next chain-load iteration would dereference freed memory.
        // Forcing recompilation against the new run's BlockCache /
        // GuestMemory keeps the JIT-link tails and dispatcher pointers
        // valid.
        cpu::jit::JitCompileMemo::clear_compile_memo();

        RunConfig next_config;
        next_config.pe_path = std::move(result.pending_launch_path);
        next_config.guest_args = std::move(result.pending_launch_args);
        next_config.verbose = config.verbose;
        next_config.capsule_name = config.capsule_name;
        next_config.app_name = config.app_name;
        next_config.process_session = &process_session;
        next_config.elevation_authorizer = elevation_authorizer.get();
        next_config.elevation_credential =
            std::move(result.elevation_credential);
        next_config.token_context = result.token_context;
        result = execute_pe(next_config);
    }

    // Publish the full Windows DWORD to the parent before this host process
    // exits. Host wait status alone truncates to 8 bits.
    const bool has_guest_parent = guest_exit_code_fd >= 0;
    if (guest_exit_code_fd >= 0) {
        ipc::publish_windows_exit_code(
            guest_exit_code_fd, static_cast<u32>(result.exit_code));
        guest_exit_code_fd = -1;
    }

    // A Windows child may legitimately outlive the guest process that created
    // it. The user-facing run session stays the host owner and gives its
    // directly launched workers a grace period; a worker with a guest parent
    // exits at once, as a Windows process does whatever its children are
    // doing, so its parent sees the exit without waiting on grandchildren.
    process_session.wait_for_workers(/*grace=*/!has_guest_parent);

    if (result.success) {
        std::string msg = "Execution completed exit_code=" +
                          std::to_string(result.exit_code);
        if (result.instructions_executed > 0) {
            msg += " (" + std::to_string(result.instructions_executed) +
                   " instructions)";
        }
        // Preserve the guest ExitProcess code as the host status (low 8 bits
        // visible to wait(2); full DWORD goes through the exit-code pipe).
        return CommandResult{result.exit_code, std::move(msg)};
    }

    return CommandResult::failure(
        result.error_message.empty() ? "Execution failed" : result.error_message,
        result.exit_code != 0 ? result.exit_code : 1);
}

} // namespace vitreo::cli

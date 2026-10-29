#include "arg_parser.hpp"
#include <cstring>

namespace vitreo::cli {

namespace {

bool is_flag(const char* arg) {
    return arg[0] == '-';
}

/// Commands that take a subcommand
bool expects_subcommand(std::string_view cmd) {
    return cmd == "capsule" || cmd == "config" || cmd == "process";
}

} // anonymous namespace

ParseResult parse_args(int argc, const char* const* argv) {
    ParsedArgs args{};

    // Skip argv[0] (program name)
    int i = 1;

    // First pass: extract flags and named options, collect non-flag tokens
    std::vector<std::string> tokens;

    while (i < argc) {
        std::string_view arg{argv[i]};

        if (arg == "--verbose" || arg == "-v") {
            args.verbose = true;
            ++i;
        } else if (arg == "--help" || arg == "-h") {
            args.help = true;
            ++i;
        } else if (arg == "--all") {
            args.all = true;
            ++i;
        } else if (arg == "--json") {
            args.json = true;
            ++i;
        } else if (arg == "--interpreter" || arg == "--no-jit") {
            // Force pure-interpreter execution (disable the JIT).
            args.interpreter = true;
            ++i;
        } else if (arg == "--trace") {
            // Enable verbose/trace logging (alias of --verbose for callers,
            // e.g. the manager, that pass --trace).
            args.trace = true;
            ++i;
        } else if (arg == "--checkpoint-save" ||
                   arg == "--checkpoint-restore" ||
                   arg == "--checkpoint-at" ||
                   arg == "--replay-instructions" ||
                   arg == "--replay-seed") {
            const std::string option(arg);
            ++i;
            if (i >= argc) {
                return ParseResult::err(option + " requires a value");
            }
            if (option == "--checkpoint-save") {
                args.checkpoint_save = argv[i];
            } else if (option == "--checkpoint-restore") {
                args.checkpoint_restore = argv[i];
            } else if (option == "--checkpoint-at") {
                args.checkpoint_at = argv[i];
            } else if (option == "--replay-instructions") {
                args.replay_instructions = argv[i];
            } else {
                args.replay_seed = argv[i];
            }
            ++i;
        } else if (arg == "--capsule") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--capsule requires a value");
            }
            args.capsule_name = argv[i];
            ++i;
        } else if (arg == "--app-name") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--app-name requires a value");
            }
            args.app_name = argv[i];
            ++i;
        } else if (arg == "--drive") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--drive requires a value");
            }
            args.drive_letter = argv[i];
            ++i;
        } else if (arg == "--env") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--env requires NAME=VALUE");
            }
            std::string value = argv[i];
            const auto separator = guest_environment_separator(value);
            if (separator == std::string::npos || separator == 0 ||
                (value.starts_with('=') && separator == 1)) {
                return ParseResult::err("--env requires NAME=VALUE");
            }
            args.guest_environment.push_back(std::move(value));
            ++i;
        } else if (arg == "--relaunched-as") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--relaunched-as requires a value");
            }
            args.relaunched_as = argv[i];
            ++i;
        } else if (arg == "--guest-cwd") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--guest-cwd requires a value");
            }
            args.guest_cwd = argv[i];
            ++i;
        } else if (arg == "--guest-cmdline") {
            // Full Windows command line for a CreateProcess-spawned child,
            // passed as ONE argv element so GetCommandLineW returns it verbatim
            // (browser subprocesses are entirely command-line driven).
            ++i;
            if (i >= argc) {
                return ParseResult::err("--guest-cmdline requires a value");
            }
            args.guest_cmdline = argv[i];
            ++i;
        } else if (arg == "--inherited-pipes") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--inherited-pipes requires a value");
            }
            args.inherited_pipes = argv[i];
            ++i;
        } else if (arg == "--conpty-handles") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--conpty-handles requires a value");
            }
            args.conpty_handles = argv[i];
            ++i;
        } else if (arg == "--inherited-sync") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--inherited-sync requires a value");
            }
            args.inherited_sync = argv[i];
            ++i;
        } else if (arg == "--inherited-mappings") {
            ++i;
            if (i >= argc) {
                return ParseResult::err(
                    "--inherited-mappings requires a value");
            }
            args.inherited_mappings = argv[i];
            ++i;
        } else if (arg == "--guest-service-arg-utf16") {
            ++i;
            if (i >= argc) {
                return ParseResult::err(
                    "--guest-service-arg-utf16 requires a value");
            }
            args.guest_service_arguments_utf16.emplace_back(argv[i]);
            ++i;
        } else if (arg == "--guest-elevation-credential") {
            ++i;
            if (i >= argc) {
                return ParseResult::err(
                    "--guest-elevation-credential requires a value");
            }
            args.guest_elevation_credential = argv[i];
            ++i;
        } else if (arg == "--guest-token-context-fd") {
            ++i;
            if (i >= argc) {
                return ParseResult::err(
                    "--guest-token-context-fd requires a value");
            }
            args.guest_token_context_fd = argv[i];
            ++i;
        } else if (arg == "--guest-parent-pid") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--guest-parent-pid requires a value");
            }
            args.guest_parent_pid = argv[i];
            ++i;
        } else if (arg == "--parent-death-fd") {
            ++i;
            if (i >= argc) {
                return ParseResult::err("--parent-death-fd requires a value");
            }
            args.parent_death_fd = argv[i];
            ++i;
        } else if (arg == "--guest-exit-code-fd") {
            ++i;
            if (i >= argc) {
                return ParseResult::err(
                    "--guest-exit-code-fd requires a value");
            }
            args.guest_exit_code_fd = argv[i];
            ++i;
        } else if (arg == "--") {
            // Everything after -- is positional
            ++i;
            while (i < argc) {
                tokens.emplace_back(argv[i]);
                ++i;
            }
        } else if (is_flag(argv[i])) {
            return ParseResult::err(std::string("Unknown option: ") + argv[i]);
        } else {
            tokens.emplace_back(argv[i]);
            ++i;
        }
    }

    // If no tokens and --help was set, treat as help command
    if (tokens.empty()) {
        if (args.help) {
            args.command = "help";
            return ParseResult::ok(std::move(args));
        }
        return ParseResult::err("No command specified. Use --help for usage information.");
    }

    // First token is the command
    args.command = std::move(tokens[0]);
    size_t pos = 1;

    // If command expects a subcommand, consume next token
    if (expects_subcommand(args.command) && pos < tokens.size()) {
        args.subcommand = std::move(tokens[pos]);
        ++pos;
    }

    // Remaining tokens are positional arguments
    while (pos < tokens.size()) {
        args.positional_args.emplace_back(std::move(tokens[pos]));
        ++pos;
    }

    return ParseResult::ok(std::move(args));
}

} // namespace vitreo::cli

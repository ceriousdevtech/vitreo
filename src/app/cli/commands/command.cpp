#include "command.hpp"

#include <vitreo/common.hpp>

#include <cstdio>
#include <string>

namespace vitreo::cli {

void CommandDispatcher::register_command(std::unique_ptr<Command> cmd) {
    commands_.push_back(std::move(cmd));
}

CommandResult CommandDispatcher::dispatch(const ParsedArgs& args) {
    if (args.command.empty()) {
        return CommandResult::failure("No command specified. Use 'vitreoctl help' for usage.");
    }

    for (const auto& cmd : commands_) {
        if (cmd->name() == args.command) {
            return cmd->execute(args);
        }
    }

    return CommandResult::failure(
        std::string("Unknown command: '") + args.command +
        "'. Use 'vitreoctl help' for a list of commands.");
}

void CommandDispatcher::print_help() const {
    std::printf("vitreo - Vitreo Windows Runtime Engine\n\n");
    std::printf("Usage: vitreo <command> [options] [args...]\n\n");
    std::printf("Commands:\n");
    for (const auto& cmd : commands_) {
        std::printf("  %-16s %s\n",
                    std::string(cmd->name()).c_str(),
                    std::string(cmd->help()).c_str());
    }
    std::printf("\nGlobal options:\n");
    std::printf("  --verbose, -v    Enable verbose output\n");
    std::printf("  --help, -h       Show help information\n");
}

bool CommandDispatcher::has_command(std::string_view name) const {
    for (const auto& cmd : commands_) {
        if (cmd->name() == name) {
            return true;
        }
    }
    return false;
}

// --- VersionCommand ---

CommandResult VersionCommand::execute(const ParsedArgs& /*args*/) {
    return CommandResult::success(std::string("Vitreo ") + vitreo::VERSION_STRING);
}

// --- HelpCommand ---

CommandResult HelpCommand::execute(const ParsedArgs& /*args*/) {
    dispatcher_.print_help();
    return CommandResult::success();
}

} // namespace vitreo::cli

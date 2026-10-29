#pragma once
#include "../parser/arg_parser.hpp"
#include <memory>
#include <string_view>
#include <vector>

namespace vitreo::cli {

/// Command execution result
struct CommandResult {
    int exit_code;
    std::string message;
    
    static CommandResult success(std::string msg = "") { return {0, std::move(msg)}; }
    static CommandResult failure(std::string msg, int code = 1) { return {code, std::move(msg)}; }
};

/// Base interface for commands
class Command {
public:
    virtual ~Command() = default;
    
    /// Execute the command with parsed arguments
    [[nodiscard]] virtual CommandResult execute(const ParsedArgs& args) = 0;
    
    /// Get the command name
    [[nodiscard]] virtual std::string_view name() const = 0;
    
    /// Get usage/help text
    [[nodiscard]] virtual std::string_view help() const = 0;
};

/// Command registry and dispatcher
class CommandDispatcher {
public:
    /// Register a command
    void register_command(std::unique_ptr<Command> cmd);
    
    /// Dispatch based on parsed args
    [[nodiscard]] CommandResult dispatch(const ParsedArgs& args);
    
    /// Print help for all commands
    void print_help() const;
    
    /// Check if a command exists
    [[nodiscard]] bool has_command(std::string_view name) const;
    
private:
    std::vector<std::unique_ptr<Command>> commands_;
};

/// Built-in version command
class VersionCommand : public Command {
public:
    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "version"; }
    std::string_view help() const override { return "Show Vitreo version information"; }
};

/// Built-in help command
class HelpCommand : public Command {
public:
    explicit HelpCommand(const CommandDispatcher& dispatcher) : dispatcher_(dispatcher) {}
    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "help"; }
    std::string_view help() const override { return "Show help information"; }
private:
    const CommandDispatcher& dispatcher_;
};

} // namespace vitreo::cli

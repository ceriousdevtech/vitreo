#pragma once
/// @file config.hpp
/// @brief Configuration management (CLI-004)

#include <vitreo/common.hpp>
#include "command.hpp"
#include <map>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace vitreo::cli {

/// In-memory configuration store with typed validation
class Config {
public:
    Config();

    /// Get the value for a key, or nullopt if unknown
    [[nodiscard]] std::optional<std::string> get(std::string_view key) const;

    /// Set a key to a value. Returns false if key is unknown or value is invalid.
    bool set(std::string_view key, std::string_view value);

    /// Reset a single key to its default. Returns false if key is unknown.
    bool reset(std::string_view key);

    /// Reset all keys to defaults
    void reset_all();

    /// Check whether a key exists in the schema
    [[nodiscard]] bool is_valid_key(std::string_view key) const;

    /// Return all key/value pairs sorted by key
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> get_all() const;

private:
    struct KeyInfo {
        std::string default_value;
    };

    std::map<std::string, KeyInfo, std::less<>> m_schema;
    std::map<std::string, std::string, std::less<>> m_values;

    void init_schema();
    [[nodiscard]] bool validate(std::string_view key, std::string_view value) const;
};

/// CLI command: vitreoctl config get|set|reset
class ConfigCommand : public Command {
public:
    explicit ConfigCommand(Config& config);

    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "config"; }
    std::string_view help() const override { return "Get, set, or reset configuration values"; }

private:
    Config& m_config;

    CommandResult do_get(const ParsedArgs& args);
    CommandResult do_set(const ParsedArgs& args);
    CommandResult do_reset(const ParsedArgs& args);
};

} // namespace vitreo::cli

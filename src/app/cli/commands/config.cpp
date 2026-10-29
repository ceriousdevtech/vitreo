#include "config.hpp"
#include <charconv>

namespace vitreo::cli {

// =============================================================================
// Config
// =============================================================================

Config::Config() {
    init_schema();
}

void Config::init_schema() {
    m_schema["cpu.mode"]              = {"interpreter"};
    m_schema["cpu.max_instructions"]  = {"0"};
    m_schema["memory.guest_size_mb"]  = {"4096"};
    m_schema["memory.stack_size_mb"]  = {"8"};
    m_schema["log.level"]             = {"warn"};
    m_schema["log.file"]              = {""};
    m_schema["capsule.default_root"]  = {"~/.vitreo/capsules"};
    m_schema["capsule.auto_create"]   = {"true"};

    // Populate m_values with defaults
    for (const auto& [key, info] : m_schema) {
        m_values[key] = info.default_value;
    }
}

bool Config::validate(std::string_view key, std::string_view value) const {
    if (key == "cpu.mode") {
        return value == "interpreter" || value == "jit";
    }
    if (key == "log.level") {
        return value == "error" || value == "warn" || value == "info" ||
               value == "debug" || value == "trace";
    }
    if (key == "capsule.auto_create") {
        return value == "true" || value == "false";
    }
    // Numeric keys: must parse as valid unsigned integer
    if (key == "cpu.max_instructions" || key == "memory.guest_size_mb" ||
        key == "memory.stack_size_mb") {
        u64 result = 0;
        auto [ptr, ec] = std::from_chars(value.data(),
                                         value.data() + value.size(),
                                         result);
        return ec == std::errc{} && ptr == value.data() + value.size();
    }
    // log.file and capsule.default_root accept any string
    return true;
}

std::optional<std::string> Config::get(std::string_view key) const {
    auto it = m_values.find(key);
    if (it == m_values.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool Config::set(std::string_view key, std::string_view value) {
    if (!is_valid_key(key)) {
        return false;
    }
    if (!validate(key, value)) {
        return false;
    }
    m_values[std::string(key)] = std::string(value);
    return true;
}

bool Config::reset(std::string_view key) {
    auto it = m_schema.find(key);
    if (it == m_schema.end()) {
        return false;
    }
    m_values[std::string(key)] = it->second.default_value;
    return true;
}

void Config::reset_all() {
    for (const auto& [key, info] : m_schema) {
        m_values[key] = info.default_value;
    }
}

bool Config::is_valid_key(std::string_view key) const {
    return m_schema.find(key) != m_schema.end();
}

std::vector<std::pair<std::string, std::string>> Config::get_all() const {
    std::vector<std::pair<std::string, std::string>> result;
    result.reserve(m_values.size());
    for (const auto& [key, val] : m_values) {
        result.emplace_back(key, val);
    }
    return result;
}

// =============================================================================
// ConfigCommand
// =============================================================================

ConfigCommand::ConfigCommand(Config& config)
    : m_config(config) {}

CommandResult ConfigCommand::execute(const ParsedArgs& args) {
    const auto& sub = args.subcommand;

    if (sub == "get")   return do_get(args);
    if (sub == "set")   return do_set(args);
    if (sub == "reset") return do_reset(args);

    return CommandResult::failure(
        "Unknown config subcommand: '" + sub +
        "'. Use 'get', 'set', or 'reset'.");
}

CommandResult ConfigCommand::do_get(const ParsedArgs& args) {
    // --all flag or no key specified → dump everything
    if (args.all || args.positional_args.empty()) {
        auto entries = m_config.get_all();
        std::string msg;
        for (const auto& [k, v] : entries) {
            msg += k + " = " + v + '\n';
        }
        return CommandResult::success(std::move(msg));
    }

    const auto& key = args.positional_args[0];
    auto val = m_config.get(key);
    if (!val) {
        return CommandResult::failure("Unknown config key: '" + key + "'");
    }
    return CommandResult::success(key + " = " + *val);
}

CommandResult ConfigCommand::do_set(const ParsedArgs& args) {
    if (args.positional_args.size() < 2) {
        return CommandResult::failure(
            "Usage: vitreo config set <key> <value>");
    }
    const auto& key   = args.positional_args[0];
    const auto& value = args.positional_args[1];

    if (!m_config.is_valid_key(key)) {
        return CommandResult::failure("Unknown config key: '" + key + "'");
    }
    if (!m_config.set(key, value)) {
        return CommandResult::failure(
            "Invalid value '" + value + "' for key '" + key + "'");
    }
    return CommandResult::success("Set " + key + " = " + value);
}

CommandResult ConfigCommand::do_reset(const ParsedArgs& args) {
    if (args.positional_args.empty()) {
        m_config.reset_all();
        return CommandResult::success("All configuration reset to defaults");
    }
    const auto& key = args.positional_args[0];
    if (!m_config.reset(key)) {
        return CommandResult::failure("Unknown config key: '" + key + "'");
    }
    auto val = m_config.get(key);
    return CommandResult::success("Reset " + key + " = " + (val ? *val : ""));
}

} // namespace vitreo::cli

#pragma once
#include "../parser/arg_parser.hpp"
#include "command.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace vitreo::cli {

/// Info about a capsule
struct CapsuleInfo {
    std::string name;
    std::filesystem::path root_path;
    bool has_c_drive = false;
    bool has_registry = false;
};

/// Manages capsule directories
class CapsuleManager {
public:
    explicit CapsuleManager(const std::filesystem::path& base_dir);

    /// Create a new capsule with a C: drive directory
    CommandResult create(const std::string& name);

    /// List all capsules
    std::vector<CapsuleInfo> list() const;

    /// Destroy a capsule (remove directory)
    CommandResult destroy(const std::string& name);

    /// Rename a capsule directory.
    CommandResult rename(const std::string& source, const std::string& destination);

    /// Clone a capsule directory recursively.
    CommandResult clone(const std::string& source, const std::string& destination);

    /// Move a capsule into the current user's Trash.
    CommandResult trash(const std::string& name);

    /// Export a capsule as a portable directory package.
    CommandResult export_to(
        const std::string& name, const std::filesystem::path& destination);

    /// Import a portable capsule directory package.
    CommandResult import_from(
        const std::filesystem::path& source, const std::string& name);

    /// Validate the minimum capsule structure.
    CommandResult validate(const std::string& name, bool json = false) const;

    /// Describe one capsule.
    CommandResult info(const std::string& name, bool json = false) const;

    /// Check if capsule exists
    bool exists(const std::string& name) const;

    /// Get capsule root path
    std::filesystem::path capsule_path(const std::string& name) const;

private:
    std::filesystem::path m_base_dir;
};

/// CLI command for capsule management
class CapsuleCommand : public Command {
public:
    explicit CapsuleCommand(CapsuleManager& manager);

    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "capsule"; }
    std::string_view help() const override {
        return "Manage capsules (create/list/info/validate/rename/clone/trash/export/import/destroy)";
    }

private:
    CapsuleManager& m_manager;
};

} // namespace vitreo::cli

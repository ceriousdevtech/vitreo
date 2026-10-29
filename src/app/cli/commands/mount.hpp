#pragma once

#include "command.hpp"

#include <filesystem>
#include <optional>

namespace vitreo::cli {

class MountManager {
public:
    explicit MountManager(std::filesystem::path capsule_base);

    [[nodiscard]] CommandResult mount_image(
        const std::string& image, const std::string& capsule,
        std::optional<char> requested_drive) const;
    [[nodiscard]] CommandResult unmount(char drive, const std::string& capsule) const;
    [[nodiscard]] CommandResult list(const std::string& capsule) const;

private:
    std::filesystem::path capsule_base_;
};

class MountCommand final : public Command {
public:
    explicit MountCommand(const MountManager& manager) : manager_(manager) {}
    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "mount"; }
    std::string_view help() const override { return "Mount or list capsule optical images"; }
private:
    const MountManager& manager_;
};

class UnmountCommand final : public Command {
public:
    explicit UnmountCommand(const MountManager& manager) : manager_(manager) {}
    CommandResult execute(const ParsedArgs& args) override;
    std::string_view name() const override { return "unmount"; }
    std::string_view help() const override { return "Unmount a capsule optical image"; }
private:
    const MountManager& manager_;
};

} // namespace vitreo::cli

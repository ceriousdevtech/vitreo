#include "mount.hpp"

#include "capsule/vfs/image_mount_store.hpp"
#include "capsule/vfs/iso_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"

#include <cctype>
#include <sstream>

namespace vitreo::cli {
namespace {

std::optional<char> parse_drive(const std::string& value) {
    if (value.empty() || value.size() > 2 ||
        (value.size() == 2 && value[1] != ':') ||
        !capsule::is_valid_drive_letter(value[0])) {
        return std::nullopt;
    }
    return capsule::normalize_drive_letter(value[0]);
}

} // namespace

MountManager::MountManager(std::filesystem::path capsule_base)
    : capsule_base_(std::move(capsule_base)) {}

CommandResult MountManager::mount_image(
    const std::string& image, const std::string& capsule_name,
    std::optional<char> requested_drive) const {
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(image, ec);
    if (ec || !std::filesystem::is_regular_file(canonical, ec)) {
        return CommandResult::failure("Image file not found: " + image);
    }

    capsule::IsoFileSystem filesystem(canonical.string());
    if (!filesystem.valid()) {
        return CommandResult::failure("Unsupported optical image: " + filesystem.error_message());
    }

    const auto capsule_root = capsule_base_ / capsule_name;
    capsule::ImageMountStore store(capsule_root);
    char drive = requested_drive.value_or(store.next_available_drive());
    if (drive == 0) {
        return CommandResult::failure("No available drive letters in capsule '" + capsule_name + "'");
    }
    drive = capsule::normalize_drive_letter(drive);
    if (drive == 'C') {
        return CommandResult::failure("C: is reserved for the capsule system drive");
    }
    for (const auto& record : store.list()) {
        if (record.drive == drive) {
            return CommandResult::failure(std::string(1, drive) + ": is already mounted");
        }
    }
    if (std::filesystem::exists(capsule_root / "drives" /
                                std::string(1, static_cast<char>(std::tolower(drive))), ec)) {
        return CommandResult::failure(std::string(1, drive) + ": is already backed by a capsule directory");
    }

    auto saved = store.save(drive, canonical.string());
    if (saved.is_err()) {
        return CommandResult::failure("Failed to persist optical mount");
    }
    return CommandResult::success(
        "Mounted " + canonical.string() + " as " + std::string(1, drive) +
        ": in capsule '" + capsule_name + "'");
}

CommandResult MountManager::unmount(char drive, const std::string& capsule_name) const {
    capsule::ImageMountStore store(capsule_base_ / capsule_name);
    auto removed = store.remove(drive);
    if (removed.is_err()) {
        return CommandResult::failure(std::string(1, capsule::normalize_drive_letter(drive)) +
                                      ": is not an optical-image mount in capsule '" +
                                      capsule_name + "'");
    }
    return CommandResult::success(
        "Unmounted " + std::string(1, capsule::normalize_drive_letter(drive)) +
        ": from capsule '" + capsule_name + "'");
}

CommandResult MountManager::list(const std::string& capsule_name) const {
    capsule::ImageMountStore store(capsule_base_ / capsule_name);
    const auto records = store.list();
    if (records.empty()) {
        return CommandResult::success("No optical images mounted in capsule '" + capsule_name + "'.");
    }
    std::ostringstream output;
    for (const auto& record : records) {
        output << record.drive << ":  " << record.image_path << '\n';
    }
    return CommandResult::success(output.str());
}

CommandResult MountCommand::execute(const ParsedArgs& args) {
    const std::string capsule_name = args.capsule_name.value_or("default");
    if (args.positional_args.empty()) {
        return manager_.list(capsule_name);
    }
    std::optional<char> drive;
    if (args.drive_letter) {
        drive = parse_drive(*args.drive_letter);
        if (!drive) return CommandResult::failure("--drive requires a letter from D through Z");
    }
    return manager_.mount_image(args.positional_args.front(), capsule_name, drive);
}

CommandResult UnmountCommand::execute(const ParsedArgs& args) {
    if (args.positional_args.size() != 1) {
        return CommandResult::failure("Usage: vitreo unmount <drive> [--capsule <name>]");
    }
    auto drive = parse_drive(args.positional_args.front());
    if (!drive || *drive == 'C') {
        return CommandResult::failure("Drive must be a letter from D through Z");
    }
    return manager_.unmount(*drive, args.capsule_name.value_or("default"));
}

} // namespace vitreo::cli

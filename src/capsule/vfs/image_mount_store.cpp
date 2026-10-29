#include "image_mount_store.hpp"

#include "vfs.hpp"

#include <algorithm>
#include <fstream>
#include <set>

namespace vitreo::capsule {

ImageMountStore::ImageMountStore(std::filesystem::path capsule_root)
    : capsule_root_(std::move(capsule_root)) {}

std::filesystem::path ImageMountStore::record_path(char drive) const {
    const char normalized = normalize_drive_letter(drive);
    return capsule_root_ / "mounts" / (std::string(1, normalized) + ".mount");
}

std::vector<ImageMountRecord> ImageMountStore::list() const {
    std::vector<ImageMountRecord> records;
    const auto directory = capsule_root_ / "mounts";
    std::error_code ec;
    if (!std::filesystem::is_directory(directory, ec)) {
        return records;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (ec || !entry.is_regular_file(ec) || entry.path().extension() != ".mount") {
            continue;
        }
        const std::string stem = entry.path().stem().string();
        if (stem.size() != 1 || !is_valid_drive_letter(stem[0])) {
            continue;
        }
        std::ifstream stream(entry.path());
        std::string image_path;
        std::getline(stream, image_path);
        if (!stream && image_path.empty()) {
            continue;
        }
        records.push_back(ImageMountRecord{normalize_drive_letter(stem[0]), image_path});
    }
    std::sort(records.begin(), records.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.drive < rhs.drive; });
    return records;
}

Result<void, VfsError> ImageMountStore::save(char drive, const std::string& image_path) {
    if (!is_valid_drive_letter(drive) || normalize_drive_letter(drive) == 'C' || image_path.empty()) {
        return Result<void, VfsError>::err(VfsError::InvalidArgument);
    }
    std::error_code ec;
    std::filesystem::create_directories(capsule_root_ / "mounts", ec);
    if (ec) return Result<void, VfsError>::err(VfsError::IoError);

    const auto destination = record_path(drive);
    const auto temporary = destination.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::trunc);
        if (!stream) return Result<void, VfsError>::err(VfsError::IoError);
        stream << image_path << '\n';
        if (!stream) return Result<void, VfsError>::err(VfsError::IoError);
    }
    std::filesystem::rename(temporary, destination, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        return Result<void, VfsError>::err(VfsError::IoError);
    }
    return Result<void, VfsError>::ok();
}

Result<void, VfsError> ImageMountStore::remove(char drive) {
    if (!is_valid_drive_letter(drive) || normalize_drive_letter(drive) == 'C') {
        return Result<void, VfsError>::err(VfsError::InvalidArgument);
    }
    std::error_code ec;
    if (!std::filesystem::remove(record_path(drive), ec)) {
        return Result<void, VfsError>::err(ec ? VfsError::IoError : VfsError::DriveNotMounted);
    }
    return Result<void, VfsError>::ok();
}

char ImageMountStore::next_available_drive() const {
    std::set<char> used{'C'};
    for (const auto& record : list()) used.insert(record.drive);

    std::error_code ec;
    const auto drives = capsule_root_ / "drives";
    if (std::filesystem::is_directory(drives, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(drives, ec)) {
            const std::string name = entry.path().filename().string();
            if (name.size() == 1 && is_valid_drive_letter(name[0])) {
                used.insert(normalize_drive_letter(name[0]));
            }
        }
    }
    for (char drive = 'D'; drive <= 'Z'; ++drive) {
        if (!used.contains(drive)) return drive;
    }
    return 0;
}

} // namespace vitreo::capsule

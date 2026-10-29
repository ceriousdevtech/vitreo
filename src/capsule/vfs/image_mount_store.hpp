#pragma once

#include "filesystem.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace vitreo::capsule {

struct ImageMountRecord {
    char drive = 0;
    std::string image_path;
};

/// Persistent optical-image mount records for one capsule.
class ImageMountStore {
public:
    explicit ImageMountStore(std::filesystem::path capsule_root);

    [[nodiscard]] std::vector<ImageMountRecord> list() const;
    [[nodiscard]] Result<void, VfsError> save(char drive, const std::string& image_path);
    [[nodiscard]] Result<void, VfsError> remove(char drive);
    [[nodiscard]] char next_available_drive() const;

private:
    [[nodiscard]] std::filesystem::path record_path(char drive) const;

    std::filesystem::path capsule_root_;
};

} // namespace vitreo::capsule

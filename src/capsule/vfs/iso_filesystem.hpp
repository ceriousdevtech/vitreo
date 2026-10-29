#pragma once

#include "filesystem.hpp"

#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace vitreo::capsule {

/// Read-only ISO9660 filesystem backed directly by an ISO or MODE1/2352 image.
class IsoFileSystem final : public IFileSystem {
public:
    explicit IsoFileSystem(std::string image_path);
    ~IsoFileSystem() override = default;

    IsoFileSystem(const IsoFileSystem&) = delete;
    IsoFileSystem& operator=(const IsoFileSystem&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] bool is_optical() const noexcept override { return true; }
    [[nodiscard]] bool volume_info(VolumeInfo& info) const noexcept override;
    [[nodiscard]] Result<usize, VfsError> read_volume(
        u64 offset, std::span<u8> buffer) override;
    [[nodiscard]] const std::string& error_message() const noexcept { return error_message_; }
    [[nodiscard]] const std::string& image_path() const noexcept { return image_path_; }

    [[nodiscard]] Result<FileHandle, VfsError> open(
        std::string_view path, AccessMode access, ShareMode share,
        CreationDisposition disposition) override;
    [[nodiscard]] Result<void, VfsError> close(FileHandle handle) override;
    [[nodiscard]] Result<usize, VfsError> read(FileHandle handle, std::span<u8> buffer) override;
    [[nodiscard]] Result<usize, VfsError> write(FileHandle, std::span<const u8>) override;
    [[nodiscard]] Result<u64, VfsError> seek(FileHandle handle, i64 offset, SeekOrigin origin) override;
    [[nodiscard]] Result<FileInfo, VfsError> stat(std::string_view path) override;
    [[nodiscard]] Result<FileInfo, VfsError> stat_handle(FileHandle handle) override;
    /// Each component spelled as its directory record names it.
    [[nodiscard]] Result<std::string, VfsError> final_path(FileHandle handle) override;
    [[nodiscard]] Result<std::vector<DirectoryEntry>, VfsError> list_dir(std::string_view path) override;
    [[nodiscard]] Result<void, VfsError> create_dir(std::string_view) override;
    [[nodiscard]] Result<void, VfsError> remove(std::string_view) override;
    [[nodiscard]] Result<void, VfsError> rename(std::string_view, std::string_view) override;

private:
    struct Entry {
        std::string name;
        std::string path;
        u32 extent = 0;
        u32 size = 0;
        bool directory = false;
        std::vector<std::string> children;
    };

    struct OpenState {
        std::string path;
        u64 position = 0;
    };

    [[nodiscard]] bool initialize();
    [[nodiscard]] bool configure_image_layout();
    [[nodiscard]] bool parse_directory(const std::string& path, u32 extent, u32 size, u32 depth);
    [[nodiscard]] bool read_logical(u64 offset, std::span<u8> buffer);
    [[nodiscard]] std::string normalize_path(std::string_view path) const;
    [[nodiscard]] const Entry* find_entry(std::string_view path) const;
    [[nodiscard]] static FileInfo entry_info(const Entry& entry);

    std::string image_path_;
    std::ifstream image_;
    u32 physical_sector_size_ = 2048;
    u32 payload_offset_ = 0;
    /// True when the disc carries a Joliet supplementary descriptor, whose
    /// tree is used instead of the 8.3 primary one.
    bool joliet_ = false;
    bool valid_ = false;
    std::string error_message_;
    VolumeInfo volume_info_;
    std::unordered_map<std::string, Entry> entries_;
    std::unordered_map<FileHandle, OpenState> open_files_;
    mutable std::mutex mutex_;
};

} // namespace vitreo::capsule

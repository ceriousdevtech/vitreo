/// @file test_directory.cpp
/// @brief Unit tests for DirHandle and DirectoryOps (VFS-004)

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "vitreo/common.hpp"
#include "capsule/vfs/directory.hpp"
#include "capsule/vfs/filesystem.hpp"

using namespace vitreo;
using namespace vitreo::capsule;

// =============================================================================
// In-memory IFileSystem for testing
// =============================================================================

class InMemoryFS : public IFileSystem {
public:
    struct Entry {
        bool is_dir = false;
        std::vector<u8> data;
    };

    std::map<std::string, Entry> entries;

    InMemoryFS() {
        entries[""] = Entry{true, {}};  // root
    }

    void add_file(const std::string& path, const std::vector<u8>& data = {}) {
        entries[path] = Entry{false, data};
    }

    void add_dir(const std::string& path) {
        entries[path] = Entry{true, {}};
    }

    // -- IFileSystem overrides ------------------------------------------------

    Result<FileHandle, VfsError> open(
        std::string_view, AccessMode, ShareMode, CreationDisposition) override {
        return Result<FileHandle, VfsError>::err(VfsError::NotSupported);
    }

    Result<void, VfsError> close(FileHandle) override {
        return Result<void, VfsError>::ok();
    }

    Result<usize, VfsError> read(FileHandle, std::span<u8>) override {
        return Result<usize, VfsError>::err(VfsError::NotSupported);
    }

    Result<usize, VfsError> write(FileHandle, std::span<const u8>) override {
        return Result<usize, VfsError>::err(VfsError::NotSupported);
    }

    Result<u64, VfsError> seek(FileHandle, i64, SeekOrigin) override {
        return Result<u64, VfsError>::err(VfsError::NotSupported);
    }

    Result<FileInfo, VfsError> stat(std::string_view path) override {
        auto it = entries.find(std::string(path));
        if (it == entries.end()) {
            return Result<FileInfo, VfsError>::err(VfsError::PathNotFound);
        }
        FileInfo info{};
        if (it->second.is_dir) {
            info.attributes = FileAttributes::Directory;
        } else {
            info.attributes = FileAttributes::Normal;
            info.size = it->second.data.size();
        }
        return Result<FileInfo, VfsError>::ok(info);
    }

    Result<std::vector<DirectoryEntry>, VfsError> list_dir(
        std::string_view path) override {

        std::string dir_path(path);
        auto it = entries.find(dir_path);
        if (it == entries.end()) {
            return Result<std::vector<DirectoryEntry>, VfsError>::err(
                VfsError::PathNotFound);
        }
        if (!it->second.is_dir) {
            return Result<std::vector<DirectoryEntry>, VfsError>::err(
                VfsError::NotADirectory);
        }

        std::vector<DirectoryEntry> result;
        std::string prefix = dir_path.empty() ? "" : dir_path + "/";

        for (auto& [p, e] : entries) {
            if (p == dir_path) continue;
            if (p.starts_with(prefix)) {
                auto rest = p.substr(prefix.size());
                if (rest.find('/') == std::string::npos) {
                    DirectoryEntry de;
                    de.name = std::string(rest);
                    if (e.is_dir) {
                        de.info.attributes = FileAttributes::Directory;
                    } else {
                        de.info.attributes = FileAttributes::Normal;
                        de.info.size = e.data.size();
                    }
                    result.push_back(std::move(de));
                }
            }
        }
        return Result<std::vector<DirectoryEntry>, VfsError>::ok(
            std::move(result));
    }

    Result<void, VfsError> create_dir(std::string_view path) override {
        std::string p(path);
        if (entries.count(p)) {
            return Result<void, VfsError>::err(VfsError::PathAlreadyExists);
        }
        entries[p] = Entry{true, {}};
        return Result<void, VfsError>::ok();
    }

    Result<void, VfsError> remove(std::string_view path) override {
        std::string p(path);
        auto it = entries.find(p);
        if (it == entries.end()) {
            return Result<void, VfsError>::err(VfsError::PathNotFound);
        }
        if (it->second.is_dir) {
            std::string prefix = p + "/";
            for (auto& [k, v] : entries) {
                if (k.starts_with(prefix)) {
                    return Result<void, VfsError>::err(
                        VfsError::DirectoryNotEmpty);
                }
            }
        }
        entries.erase(it);
        return Result<void, VfsError>::ok();
    }

    Result<void, VfsError> rename(std::string_view, std::string_view) override {
        return Result<void, VfsError>::err(VfsError::NotSupported);
    }
};

// =============================================================================
// DirHandle Tests
// =============================================================================

class DirHandleTest : public ::testing::Test {
protected:
    static std::vector<DirectoryEntry> make_entries(
        std::initializer_list<std::string> names) {
        std::vector<DirectoryEntry> out;
        for (auto& n : names) {
            DirectoryEntry de;
            de.name = n;
            de.info.attributes = FileAttributes::Normal;
            out.push_back(std::move(de));
        }
        return out;
    }
};

TEST_F(DirHandleTest, IterateAllEntries) {
    DirHandle h(make_entries({"a.txt", "b.txt", "c.txt"}));
    ASSERT_TRUE(h.is_open());

    std::vector<std::string> names;
    while (true) {
        auto r = h.next();
        ASSERT_TRUE(r.is_ok());
        if (!r.value().has_value()) break;
        names.push_back(r.value()->name);
    }
    EXPECT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a.txt");
    EXPECT_EQ(names[1], "b.txt");
    EXPECT_EQ(names[2], "c.txt");
}

TEST_F(DirHandleTest, NextReturnsNulloptWhenExhausted) {
    DirHandle h(make_entries({"only.txt"}));
    auto r1 = h.next();
    ASSERT_TRUE(r1.is_ok());
    EXPECT_TRUE(r1.value().has_value());

    auto r2 = h.next();
    ASSERT_TRUE(r2.is_ok());
    EXPECT_FALSE(r2.value().has_value());

    // Calling again still returns nullopt
    auto r3 = h.next();
    ASSERT_TRUE(r3.is_ok());
    EXPECT_FALSE(r3.value().has_value());
}

TEST_F(DirHandleTest, RewindResetsIteration) {
    DirHandle h(make_entries({"x.txt", "y.txt"}));
    h.next();
    h.next();

    auto rw = h.rewind();
    ASSERT_TRUE(rw.is_ok());
    EXPECT_EQ(h.remaining(), 2u);

    auto r = h.next();
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->name, "x.txt");
}

TEST_F(DirHandleTest, ClosePreventsFurtherNext) {
    DirHandle h(make_entries({"a.txt"}));
    h.close();
    EXPECT_FALSE(h.is_open());

    auto r = h.next();
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::HandleClosed);
}

TEST_F(DirHandleTest, ClosePreventsFurtherRewind) {
    DirHandle h(make_entries({"a.txt"}));
    h.close();

    auto r = h.rewind();
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::HandleClosed);
}

TEST_F(DirHandleTest, CountAndRemaining) {
    DirHandle h(make_entries({"a.txt", "b.txt", "c.txt"}));
    EXPECT_EQ(h.count(), 3u);
    EXPECT_EQ(h.remaining(), 3u);

    h.next();
    EXPECT_EQ(h.count(), 3u);
    EXPECT_EQ(h.remaining(), 2u);

    h.next();
    h.next();
    EXPECT_EQ(h.remaining(), 0u);
}

TEST_F(DirHandleTest, EmptyDirectoryHasZeroEntries) {
    DirHandle h(std::vector<DirectoryEntry>{});
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.remaining(), 0u);

    auto r = h.next();
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value().has_value());
}

TEST_F(DirHandleTest, MoveConstructorTransfersOwnership) {
    DirHandle h1(make_entries({"a.txt", "b.txt"}));
    h1.next();  // advance cursor by one

    DirHandle h2(std::move(h1));
    EXPECT_TRUE(h2.is_open());
    EXPECT_FALSE(h1.is_open());  // NOLINT — intentional use-after-move check
    EXPECT_EQ(h2.count(), 2u);
    EXPECT_EQ(h2.remaining(), 1u);

    auto r = h2.next();
    ASSERT_TRUE(r.is_ok());
    EXPECT_EQ(r.value()->name, "b.txt");
}

// =============================================================================
// DirectoryOps Tests
// =============================================================================

class DirectoryOpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        fs_ = std::make_unique<InMemoryFS>();
        ops_ = std::make_unique<DirectoryOps>(*fs_);
    }

    std::unique_ptr<InMemoryFS> fs_;
    std::unique_ptr<DirectoryOps> ops_;
};

TEST_F(DirectoryOpsTest, CreateDirectoryNonRecursive) {
    auto r = ops_->create_directory("docs");
    ASSERT_TRUE(r.is_ok());

    auto stat = fs_->stat("docs");
    ASSERT_TRUE(stat.is_ok());
    EXPECT_TRUE(has_attribute(stat.value().attributes, FileAttributes::Directory));
}

TEST_F(DirectoryOpsTest, CreateDirectoryRecursiveCreatesParents) {
    auto r = ops_->create_directory("a/b/c", true);
    ASSERT_TRUE(r.is_ok());

    EXPECT_TRUE(fs_->stat("a").is_ok());
    EXPECT_TRUE(fs_->stat("a/b").is_ok());
    EXPECT_TRUE(fs_->stat("a/b/c").is_ok());
}

TEST_F(DirectoryOpsTest, CreateDirectoryRecursiveIdempotent) {
    ops_->create_directory("a/b", true);
    // Creating again with same path should succeed (parents already exist).
    auto r = ops_->create_directory("a/b/c", true);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(fs_->stat("a/b/c").is_ok());
}

TEST_F(DirectoryOpsTest, RemoveDirectoryNonRecursiveSucceedsOnEmpty) {
    fs_->add_dir("empty_dir");
    auto r = ops_->remove_directory("empty_dir");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(fs_->stat("empty_dir").is_err());
}

TEST_F(DirectoryOpsTest, RemoveDirectoryNonRecursiveFailsOnNonEmpty) {
    fs_->add_dir("stuff");
    fs_->add_file("stuff/file.txt");

    auto r = ops_->remove_directory("stuff");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::DirectoryNotEmpty);
}

TEST_F(DirectoryOpsTest, RemoveDirectoryRecursive) {
    fs_->add_dir("top");
    fs_->add_dir("top/sub");
    fs_->add_file("top/sub/data.bin");
    fs_->add_file("top/readme.txt");

    auto r = ops_->remove_directory("top", true);
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(fs_->stat("top").is_err());
    EXPECT_TRUE(fs_->stat("top/sub").is_err());
    EXPECT_TRUE(fs_->stat("top/sub/data.bin").is_err());
    EXPECT_TRUE(fs_->stat("top/readme.txt").is_err());
}

TEST_F(DirectoryOpsTest, IsDirectoryTrueForDirectories) {
    fs_->add_dir("mydir");
    auto r = ops_->is_directory("mydir");
    ASSERT_TRUE(r.is_ok());
    EXPECT_TRUE(r.value());
}

TEST_F(DirectoryOpsTest, IsDirectoryFalseForFiles) {
    fs_->add_file("file.txt");
    auto r = ops_->is_directory("file.txt");
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value());
}

TEST_F(DirectoryOpsTest, IsDirectoryFalseForNonExisting) {
    auto r = ops_->is_directory("nope");
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value());
}

TEST_F(DirectoryOpsTest, ExistsTrueForExistingPaths) {
    fs_->add_dir("d");
    fs_->add_file("f.txt");
    EXPECT_TRUE(ops_->exists("d").value());
    EXPECT_TRUE(ops_->exists("f.txt").value());
}

TEST_F(DirectoryOpsTest, ExistsFalseForMissingPaths) {
    auto r = ops_->exists("ghost");
    ASSERT_TRUE(r.is_ok());
    EXPECT_FALSE(r.value());
}

TEST_F(DirectoryOpsTest, OpenDirectoryReturnsCorrectEntries) {
    fs_->add_dir("dir");
    fs_->add_file("dir/a.txt");
    fs_->add_file("dir/b.txt");
    fs_->add_dir("dir/sub");

    auto r = ops_->open_directory("dir");
    ASSERT_TRUE(r.is_ok());

    DirHandle h = std::move(r.value());
    EXPECT_EQ(h.count(), 3u);

    std::vector<std::string> names;
    while (true) {
        auto e = h.next();
        ASSERT_TRUE(e.is_ok());
        if (!e.value().has_value()) break;
        names.push_back(e.value()->name);
    }

    std::sort(names.begin(), names.end());
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "a.txt");
    EXPECT_EQ(names[1], "b.txt");
    EXPECT_EQ(names[2], "sub");
}

TEST_F(DirectoryOpsTest, OpenDirectoryFailsOnNonExisting) {
    auto r = ops_->open_directory("nope");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::PathNotFound);
}

TEST_F(DirectoryOpsTest, EnumerationIncludesFilesAndSubdirs) {
    fs_->add_dir("root_dir");
    fs_->add_dir("root_dir/child_dir");
    fs_->add_file("root_dir/child_file.dat", {0x01, 0x02});

    auto r = ops_->open_directory("root_dir");
    ASSERT_TRUE(r.is_ok());

    DirHandle h = std::move(r.value());
    bool found_dir = false;
    bool found_file = false;

    while (true) {
        auto e = h.next();
        ASSERT_TRUE(e.is_ok());
        if (!e.value().has_value()) break;

        if (e.value()->name == "child_dir") {
            EXPECT_TRUE(has_attribute(e.value()->info.attributes,
                                      FileAttributes::Directory));
            found_dir = true;
        }
        if (e.value()->name == "child_file.dat") {
            EXPECT_FALSE(has_attribute(e.value()->info.attributes,
                                       FileAttributes::Directory));
            EXPECT_EQ(e.value()->info.size, 2u);
            found_file = true;
        }
    }
    EXPECT_TRUE(found_dir);
    EXPECT_TRUE(found_file);
}

TEST_F(DirectoryOpsTest, RemoveDirectoryPathNotFound) {
    auto r = ops_->remove_directory("nonexistent");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::PathNotFound);
}

TEST_F(DirectoryOpsTest, CreateDirectoryAlreadyExists) {
    fs_->add_dir("existing");
    auto r = ops_->create_directory("existing");
    ASSERT_TRUE(r.is_err());
    EXPECT_EQ(r.error(), VfsError::PathAlreadyExists);
}

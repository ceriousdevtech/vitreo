// SPDX-License-Identifier: LGPL-2.1-only

#include "ipc/process_registry.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <array>
#include <fstream>
#include <cstdlib>
#include <string>

#include <unistd.h>

namespace vitreo::ipc {
namespace {

TEST(ProcessRegistryTest, PublishesProcessForRegistrationLifetime) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("vitreo-process-registry-test-" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(root, ec);

    const char* saved_scope = std::getenv("VITREO_IPC_SCOPE");
    const std::optional<std::string> previous = saved_scope
        ? std::optional<std::string>(saved_scope) : std::nullopt;
    struct RestoreScope {
        std::optional<std::string> value;
        ~RestoreScope() {
            if (value) ::setenv("VITREO_IPC_SCOPE", value->c_str(), 1);
            else ::unsetenv("VITREO_IPC_SCOPE");
        }
    } restore{previous};
    ASSERT_EQ(::setenv("VITREO_IPC_SCOPE", root.c_str(), 1), 0);
    {
        ProcessRegistration registration(
            root.string(), "Agent.exe",
            "C:\\ProgramData\\Battle.net\\Agent\\Agent.exe", 101);
        ASSERT_TRUE(registration.valid());

        const auto processes = list_processes(root.string());
        ASSERT_EQ(processes.size(), 1u);
        EXPECT_EQ(processes[0].pid, static_cast<u32>(::getpid()));
        EXPECT_EQ(processes[0].primary_thread_id, 101u);
        EXPECT_EQ(primary_thread_owner(101), static_cast<u32>(::getpid()));
        EXPECT_FALSE(primary_thread_owner(102).has_value());
        EXPECT_EQ(processes[0].parent_pid, static_cast<u32>(::getppid()));
        EXPECT_EQ(processes[0].image_name, "Agent.exe");
        EXPECT_EQ(processes[0].image_path,
                  "C:\\ProgramData\\Battle.net\\Agent\\Agent.exe");
    }

    EXPECT_TRUE(list_processes(root.string()).empty());
    EXPECT_FALSE(primary_thread_owner(101).has_value());
    fs::remove_all(root, ec);
}

TEST(ProcessRegistryTest, ReadsPreviousRecordFormatWithoutInventingAThreadId) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() /
        ("vitreo-process-registry-v2-test-" + std::to_string(::getpid()));
    fs::create_directories(root / "runtime/processes");
    const std::string name = "Guest.exe", image = "C:\\Guest.exe";
    const std::array<u32, 6> header{
        0x56545052, 2, static_cast<u32>(::getpid()),
        static_cast<u32>(::getppid()), static_cast<u32>(name.size()),
        static_cast<u32>(image.size())};
    {
        std::ofstream out(root / "runtime/processes" / std::to_string(::getpid()),
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(header.data()), sizeof(header));
        out.write(name.data(), name.size());
        out.write(image.data(), image.size());
    }
    const auto entries = list_processes(root.string());
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].image_name, name);
    EXPECT_EQ(entries[0].image_path, image);
    EXPECT_EQ(entries[0].primary_thread_id, 0u);
    std::error_code ec;
    fs::remove_all(root, ec);
}

} // namespace
} // namespace vitreo::ipc

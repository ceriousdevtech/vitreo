// SPDX-License-Identifier: LGPL-2.1-only
#pragma once

/// @file com_test_shell_link_fixture.hpp
/// @brief ShellLinkEncodingTest fixture (test_com_shell_link.cpp,
/// test_com_network_options.cpp, test_com_audio_endpoints.cpp). Test-local.

#include "com_test_common.hpp"

class ShellLinkEncodingTest : public ::testing::Test {
protected:
    static constexpr GuestAddr DATA_BASE = 0x00100000;
    static constexpr GuestAddr STACK_BASE = 0x00200000;
    static constexpr GuestAddr COM_BASE = ComCoreManager::COM_SCRATCH_BASE;
    static constexpr GuestAddr CLSID_ADDR = DATA_BASE;
    static constexpr GuestAddr IID_ADDR = DATA_BASE + 0x20;
    static constexpr GuestAddr QI_IID_ADDR = DATA_BASE + 0x40;
    static constexpr GuestAddr OUT_ADDR = DATA_BASE + 0x80;
    static constexpr GuestAddr QI_OUT_ADDR = DATA_BASE + 0x84;
    static constexpr GuestAddr ANSI_PATH_ADDR = DATA_BASE + 0x100;
    static constexpr GuestAddr WIDE_DIR_ADDR = DATA_BASE + 0x300;
    static constexpr GuestAddr SAVE_PATH_ADDR = DATA_BASE + 0x500;

    static constexpr Guid CLSID_SHELLLINK{
        0x00021401u, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    static constexpr Guid IID_ISHELLLINKA{
        0x000214EEu, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    static constexpr Guid IID_ISHELLLINKW{
        0x000214F9u, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
    static constexpr Guid IID_IPERSISTFILE{
        0x0000010Bu, 0x0000u, 0x0000u,
        {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(DATA_BASE, 0x10000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(STACK_BASE, 0x1000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(COM_BASE, ComCoreManager::COM_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        cpu_.init_protected_mode();

        ASSERT_TRUE(
            registry_.register_dll("ole32.dll", create_com_core_thunks(mgr_)).is_ok());
        mgr_.set_thunk_registry(&registry_);
        // CoCreateInstance needs an apartment on the calling thread
        // (measured: CO_E_NOTINITIALIZED otherwise, s7_ole_probe.expected).
        (void)mgr_.co_initialize();

        char temp_template[] = "/tmp/vitreo_shelllink_test_XXXXXX";
        char* temp_dir = ::mkdtemp(temp_template);
        ASSERT_NE(temp_dir, nullptr);
        test_root_ = temp_dir;
        ASSERT_TRUE(vfs_.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(test_root_)).is_ok());
        mgr_.set_vfs(&vfs_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_root_);
    }

    void set_args(const std::vector<u32>& args) {
        const GuestAddr frame = STACK_BASE + 0x400;
        cpu_.set_sp(frame);
        ASSERT_TRUE(memory_.write_u32(frame, 0xDEADBEEF).is_ok());
        for (usize index = 0; index < args.size(); ++index) {
            ASSERT_TRUE(memory_.write_u32(
                frame + 4 + static_cast<GuestAddr>(index * 4), args[index]).is_ok());
        }
    }

    [[nodiscard]] Marshaler make_marshaler() {
        Marshaler marshaler(memory_);
        marshaler.attach_cpu(cpu_);
        return marshaler;
    }

    void write_guid(GuestAddr address, const Guid& guid) {
        ASSERT_TRUE(memory_.write_bytes(
            address, reinterpret_cast<const Byte*>(&guid), sizeof(guid)).is_ok());
    }

    [[nodiscard]] u32 read_u32(GuestAddr address) {
        auto result = memory_.read_u32(address);
        EXPECT_TRUE(result.is_ok());
        return result.is_ok() ? result.value() : 0;
    }

    GuestMemory memory_;
    HandleTable handles_;
    VirtualMemoryManager vmm_{memory_};
    HeapManager heap_manager_{vmm_, memory_, handles_};
    ComTaskMemoryManager task_memory_{heap_manager_};
    cpu::CpuState cpu_;
    ComCoreManager mgr_{task_memory_};
    ThunkRegistry registry_{0x70000000};
    capsule::Vfs vfs_;
    std::string test_root_;
};

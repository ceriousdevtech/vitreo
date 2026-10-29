/// @file test_module_lifetime.cpp
/// @brief Tests for ModuleManager reference counting, FreeLibrary and re-attach after detach

#include "module_test_fixture.hpp"

TEST_F(ModuleManagerTest, GetModuleHandleExReferenceBalancesFreeLibrary) {
    const ModuleInfo* before =
        mm_->get_module_info_by_handle(ModuleManager::KERNEL32_BASE);
    ASSERT_NE(before, nullptr);
    ASSERT_EQ(before->ref_count, 1u);

    ASSERT_TRUE(mm_->acquire_module_reference(
        ModuleManager::KERNEL32_BASE, false));
    const ModuleInfo* acquired =
        mm_->get_module_info_by_handle(ModuleManager::KERNEL32_BASE);
    ASSERT_NE(acquired, nullptr);
    EXPECT_EQ(acquired->ref_count, 2u);

    ASSERT_TRUE(mm_->free_library(ModuleManager::KERNEL32_BASE));
    const ModuleInfo* released =
        mm_->get_module_info_by_handle(ModuleManager::KERNEL32_BASE);
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->ref_count, 1u);
}

TEST_F(ModuleManagerTest, GetModuleHandleExPinPreventsFreeLibraryUnload) {
    ASSERT_TRUE(mm_->acquire_module_reference(
        ModuleManager::KERNEL32_BASE, true));
    const ModuleInfo* acquired =
        mm_->get_module_info_by_handle(ModuleManager::KERNEL32_BASE);
    ASSERT_NE(acquired, nullptr);
    ASSERT_TRUE(acquired->pinned);
    const u32 pinned_ref_count = acquired->ref_count;

    ASSERT_TRUE(mm_->free_library(ModuleManager::KERNEL32_BASE));
    ASSERT_TRUE(mm_->free_library(ModuleManager::KERNEL32_BASE));
    const ModuleInfo* released =
        mm_->get_module_info_by_handle(ModuleManager::KERNEL32_BASE);
    ASSERT_NE(released, nullptr);
    EXPECT_TRUE(released->pinned);
    EXPECT_EQ(released->ref_count, pinned_ref_count);
}

// =============================================================================
// FreeLibrary Tests
// =============================================================================

TEST_F(ModuleManagerTest, FreeLibraryRegisteredModule) {
    set_last_error(0xDEADu);
    EXPECT_TRUE(mm_->free_library(ModuleManager::KERNEL32_BASE));
    EXPECT_EQ(get_last_error(), 0xDEADu);  // success leaves it untouched (last_error_on_success_probe.expected)
}

TEST_F(ModuleManagerTest, FreeLibraryUnknownHandleReturnsFalse) {
    EXPECT_FALSE(mm_->free_library(0xDEADBEEF));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MOD_NOT_FOUND);
}

TEST_F(ModuleManagerTest, FreeLibrarySystemDllStaysAlive) {
    // Even after FreeLibrary, handle should still resolve
    EXPECT_TRUE(mm_->free_library(ModuleManager::KERNEL32_BASE));
    EXPECT_EQ(mm_->get_module_handle("kernel32.dll"), ModuleManager::KERNEL32_BASE);
}

TEST_F(ModuleManagerTest, DisableThreadLibraryCallsTracksLoadedModule) {
    set_last_error(0x1234);
    ASSERT_TRUE(mm_->disable_thread_library_calls(ModuleManager::KERNEL32_BASE));
    EXPECT_EQ(get_last_error(), 0x1234u);
    const ModuleInfo* info =
        mm_->get_module_info_by_handle(ModuleManager::KERNEL32_BASE);
    ASSERT_NE(info, nullptr);
    EXPECT_TRUE(info->thread_notifications_disabled);
}

TEST_F(ModuleManagerTest, DisableThreadLibraryCallsRejectsInvalidHandle) {
    EXPECT_FALSE(mm_->disable_thread_library_calls(0xDEADBEEF));
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_PARAMETER);
}

// =============================================================================
// Re-attach after detach (StarCraft standard.snp)
// =============================================================================
//
// free_library() sends DLL_PROCESS_DETACH but keeps the image mapped, so a
// later LoadLibrary must re-run DLL_PROCESS_ATTACH. Without it the guest gets
// back a base whose CRT has already torn down its heap and globals. Storm's
// provider enumeration does exactly load → query → free → load → bind.

TEST_F(ModuleManagerTest, ReattachDetachedModuleRunsProcessAttach) {
    std::vector<u64> reasons;
    mm_->set_guest_call([&reasons](GuestAddr, const u64* args, u32 argc, bool) -> u64 {
        if (argc >= 2) reasons.push_back(args[1]);
        return 1;  // DllMain success
    });

    ModuleInfo info;
    info.base_address = 0x1C000000;
    info.image_end    = 0x1C01A000;   // non-zero → PE-backed
    info.entry_point  = 0x1C007E9E;
    info.bitness      = 32;
    info.name         = "standard.snp.dll";
    info.ref_count    = 0;            // detached, still mapped

    EXPECT_TRUE(mm_->reattach_detached_module(info));
    ASSERT_EQ(reasons.size(), 1u);
    EXPECT_EQ(reasons[0], 1u);  // DLL_PROCESS_ATTACH
}

TEST_F(ModuleManagerTest, ReattachIsNoOpWhileStillReferenced) {
    u32 calls = 0;
    mm_->set_guest_call([&calls](GuestAddr, const u64*, u32, bool) -> u64 {
        ++calls;
        return 1;
    });

    ModuleInfo info;
    info.base_address = 0x1C000000;
    info.image_end    = 0x1C01A000;
    info.entry_point  = 0x1C007E9E;
    info.bitness      = 32;
    info.name         = "standard.snp.dll";
    info.ref_count    = 1;  // still live — must not re-run DllMain

    EXPECT_TRUE(mm_->reattach_detached_module(info));
    EXPECT_EQ(calls, 0u);
}

TEST_F(ModuleManagerTest, ReattachPropagatesDllMainFailure) {
    mm_->set_guest_call([](GuestAddr, const u64*, u32, bool) -> u64 {
        return 0;  // DllMain reports init failure
    });

    ModuleInfo info;
    info.base_address = 0x1C000000;
    info.image_end    = 0x1C01A000;
    info.entry_point  = 0x1C007E9E;
    info.bitness      = 32;
    info.name         = "standard.snp.dll";
    info.ref_count    = 0;

    EXPECT_FALSE(mm_->reattach_detached_module(info));
}

TEST_F(ModuleManagerTest, RemapDetachedImageRestoresWritablePagesFromTemplate) {
    GuestMemory memory;
    capsule::Vfs vfs;
    mm_->attach_dynamic_loader(memory, vfs, false);

    const GuestAddr base = 0x1C000000;
    ASSERT_TRUE(memory.map_region(base, 0x3000, MemoryProtection::ReadWrite).is_ok());
    Byte* image = memory.host_ptr_mut(base, 0x3000);
    ASSERT_NE(image, nullptr);
    std::memset(image, 0xAA, 0x3000);
    image[0x2000] = 0x42;

    ModuleInfo info;
    info.base_address = base;
    info.image_end = base + 0x3000;
    info.mapped_header_size = 0x1000;
    pe::MappedSection text{};
    text.guest_address = base + 0x1000;
    text.size = 0x1000;
    text.protection = MemoryProtection::ReadExecute;
    std::memcpy(text.name, ".text", 6);
    pe::MappedSection data{};
    data.guest_address = base + 0x2000;
    data.size = 0x1000;
    data.protection = MemoryProtection::ReadWrite;
    std::memcpy(data.name, ".data", 6);
    info.mapped_sections = {text, data};
    info.path = "C:\\probe.dll";
    info.name = "probe.dll";
    info.ref_count = 0;
    info.entry_point = 0;
    auto snapshot = std::make_shared<MappedImageSnapshot>();
    auto* snapshot_bytes = new u8[0x3000];
    std::memcpy(snapshot_bytes, image, 0x3000);
    snapshot->data = snapshot_bytes;
    snapshot->size = 0x3000;
    info.mapped_image_template = std::move(snapshot);

    image[0x1000] = 0xEE;
    ASSERT_TRUE(memory.protect_region(base, 0x1000, MemoryProtection::Read).is_ok());
    ASSERT_TRUE(memory.protect_region(base + 0x1000, 0x1000,
                                      MemoryProtection::ReadExecute).is_ok());
    ASSERT_TRUE(memory.write_u8(base + 0x2000, 0xFF).is_ok());
    EXPECT_EQ(memory.read_u8(base + 0x2000).value(), 0xFF);

    EXPECT_TRUE(mm_->remap_detached_image(info));
    EXPECT_EQ(memory.read_u8(base + 0x2000).value(), 0x42);
    EXPECT_EQ(memory.read_u8(base + 0x1000).value(), 0xAA);
    EXPECT_FALSE(memory.guest_can_write(base + 0x1000, 1));
    EXPECT_EQ(memory.read_u8(base).value(), 0xAA);
}

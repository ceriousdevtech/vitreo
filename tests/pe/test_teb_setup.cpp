// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "core/pe/loader/teb_setup.hpp"

using namespace vitreo;

namespace {

TEST(TebSetupTest, X86PebPublishesUnlockedLoaderLock) {
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_protected_mode();

    pe::TebConfig config;
    config.stack_base = 0x70000000;
    config.stack_bottom = 0x6FF00000;
    config.image_base = 0x00400000;
    config.process_id = 41;
    config.thread_id = 73;
    config.is_64bit = false;

    const auto result = pe::setup_teb(memory, cpu, config);
    ASSERT_TRUE(result.is_ok());
    const GuestAddr peb = result.value().peb_address;
    const GuestAddr ldr = memory.read_u32(peb + 0x0C).value();
    ASSERT_NE(ldr, 0u);
    const GuestAddr list_head = ldr + 0x14;
    const GuestAddr module_links = memory.read_u32(list_head).value();
    ASSERT_NE(module_links, list_head);
    EXPECT_EQ(memory.read_u32(list_head + 4).value(), module_links);
    EXPECT_EQ(memory.read_u32(module_links).value(), list_head);
    EXPECT_EQ(memory.read_u32(module_links + 4).value(), list_head);
    EXPECT_EQ(memory.read_u32(module_links + 0x10).value(),
              static_cast<u32>(config.image_base));

    const auto fast_peb_lock = memory.read_u32(peb + 0x1C);
    ASSERT_TRUE(fast_peb_lock.is_ok());
    ASSERT_NE(fast_peb_lock.value(), 0u);
    EXPECT_EQ(memory.read_u32(fast_peb_lock.value() + 0x04).value(),
              0xFFFFFFFFu);
    EXPECT_EQ(memory.read_u32(fast_peb_lock.value() + 0x08).value(), 0u);
    EXPECT_EQ(memory.read_u32(fast_peb_lock.value() + 0x0C).value(), 0u);

    const auto loader_lock = memory.read_u32(peb + 0xA0);
    ASSERT_TRUE(loader_lock.is_ok());
    ASSERT_NE(loader_lock.value(), 0u);
    EXPECT_EQ(memory.read_u32(loader_lock.value() + 0x04).value(),
              0xFFFFFFFFu);
    EXPECT_EQ(memory.read_u32(loader_lock.value() + 0x08).value(), 0u);
    EXPECT_EQ(memory.read_u32(loader_lock.value() + 0x0C).value(), 0u);
}

TEST(TebSetupTest, X64PebPublishesUnlockedLoaderLock) {
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_long_mode();

    pe::TebConfig config;
    config.stack_base = 0x0000000200000000;
    config.stack_bottom = 0x00000001FFF00000;
    config.image_base = 0x0000000140000000;
    config.process_id = 41;
    config.thread_id = 73;
    config.is_64bit = true;
    config.loaded_modules = {
        {config.image_base, "C:\\Apps\\Guest.exe"},
        {0x0000000180000000, "C:\\Apps\\ClientSdk.dll"},
    };

    const auto result = pe::setup_teb(memory, cpu, config);
    ASSERT_TRUE(result.is_ok());
    const GuestAddr peb = result.value().peb_address;
    const GuestAddr ldr = memory.read_u64(peb + 0x18).value();
    ASSERT_NE(ldr, 0u);
    const GuestAddr list_head = ldr + 0x20;
    const GuestAddr module_links = memory.read_u64(list_head).value();
    ASSERT_NE(module_links, list_head);
    const GuestAddr second_links = memory.read_u64(module_links).value();
    ASSERT_NE(second_links, list_head);
    EXPECT_EQ(memory.read_u64(list_head + 8).value(), second_links);
    EXPECT_EQ(memory.read_u64(module_links + 8).value(), list_head);
    EXPECT_EQ(memory.read_u64(module_links + 0x20).value(), config.image_base);
    EXPECT_EQ(memory.read_u64(second_links + 0x20).value(),
              config.loaded_modules[1].base_address);
    EXPECT_EQ(memory.read_u64(second_links).value(), list_head);
    EXPECT_EQ(memory.read_u64(second_links + 8).value(), module_links);
    const GuestAddr second_entry = second_links - 0x10;
    EXPECT_EQ(memory.read_u16(second_entry + 0x58).value(), 26u);
    const GuestAddr second_base_name =
        memory.read_u64(second_entry + 0x60).value();
    ASSERT_NE(second_base_name, 0u);
    const char16_t expected_base_name[] = u"ClientSdk.dll";
    for (usize i = 0; i < std::size(expected_base_name); ++i) {
        EXPECT_EQ(memory.read_u16(second_base_name + i * 2).value(),
                  static_cast<u16>(expected_base_name[i]));
    }

    const auto fast_peb_lock = memory.read_u64(peb + 0x38);
    ASSERT_TRUE(fast_peb_lock.is_ok());
    ASSERT_NE(fast_peb_lock.value(), 0u);
    EXPECT_EQ(memory.read_u32(fast_peb_lock.value() + 0x08).value(),
              0xFFFFFFFFu);
    EXPECT_EQ(memory.read_u32(fast_peb_lock.value() + 0x0C).value(), 0u);
    EXPECT_EQ(memory.read_u64(fast_peb_lock.value() + 0x10).value(), 0u);

    const auto loader_lock = memory.read_u64(peb + 0x110);
    ASSERT_TRUE(loader_lock.is_ok());
    ASSERT_NE(loader_lock.value(), 0u);
    EXPECT_EQ(memory.read_u32(loader_lock.value() + 0x08).value(),
              0xFFFFFFFFu);
    EXPECT_EQ(memory.read_u32(loader_lock.value() + 0x0C).value(), 0u);
    EXPECT_EQ(memory.read_u64(loader_lock.value() + 0x10).value(), 0u);
}

TEST(TebSetupTest, X64PebPublishesImagePathAndCommandLine) {
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_long_mode();

    pe::TebConfig config;
    config.stack_base = 0x0000000200000000;
    config.stack_bottom = 0x00000001FFF00000;
    config.image_base = 0x0000000140000000;
    config.is_64bit = true;
    config.image_path = "C:\\Apps\\Guest.exe";
    config.command_line = "\"C:\\Apps\\Guest.exe\" --flag";

    const auto result = pe::setup_teb(memory, cpu, config);
    ASSERT_TRUE(result.is_ok());
    const GuestAddr peb = result.value().peb_address;
    const GuestAddr params = memory.read_u64(peb + 0x20).value();
    ASSERT_NE(params, 0u);

    const u16 img_len = memory.read_u16(params + 0x60).value();
    const u64 img_buf = memory.read_u64(params + 0x68).value();
    EXPECT_EQ(img_len, static_cast<u16>(config.image_path.size() * 2));
    EXPECT_EQ(memory.read_u16(img_buf).value(), static_cast<u16>('C'));

    const u16 cmd_len = memory.read_u16(params + 0x70).value();
    const u64 cmd_buf = memory.read_u64(params + 0x78).value();
    EXPECT_EQ(cmd_len, static_cast<u16>(config.command_line.size() * 2));
    EXPECT_EQ(memory.read_u16(cmd_buf).value(), static_cast<u16>('"'));
}

TEST(TebSetupTest, X64PebLoaderListTracksDynamicModules) {
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_long_mode();

    pe::TebConfig config;
    config.stack_base = 0x0000000200000000;
    config.stack_bottom = 0x00000001FFF00000;
    config.image_base = 0x0000000140000000;
    config.is_64bit = true;
    config.image_path = "C:\\Apps\\Guest.exe";

    const auto result = pe::setup_teb(memory, cpu, config);
    ASSERT_TRUE(result.is_ok());
    const GuestAddr peb = result.value().peb_address;
    const GuestAddr ldr = memory.read_u64(peb + 0x18).value();
    const GuestAddr head = ldr + 0x20;
    const GuestAddr first_links = memory.read_u64(head).value();

    const pe::TebModuleInfo first_dll{
        0x0000000180000000, "C:\\Apps\\First.dll"};
    ASSERT_TRUE(pe::append_peb_loader_module(
        memory, peb, true, first_dll));
    const GuestAddr dll_links = memory.read_u64(first_links).value();
    ASSERT_NE(dll_links, head);
    EXPECT_EQ(memory.read_u64(dll_links + 0x20).value(),
              first_dll.base_address);
    EXPECT_EQ(memory.read_u64(dll_links).value(), head);
    EXPECT_EQ(memory.read_u64(head + 8).value(), dll_links);

    // Publishing the same HMODULE twice is idempotent.
    ASSERT_TRUE(pe::append_peb_loader_module(
        memory, peb, true, first_dll));
    EXPECT_EQ(memory.read_u64(first_links).value(), dll_links);
    EXPECT_EQ(memory.read_u64(dll_links).value(), head);

    ASSERT_TRUE(pe::remove_peb_loader_module(
        memory, peb, true, first_dll.base_address));
    EXPECT_EQ(memory.read_u64(first_links).value(), head);
    EXPECT_EQ(memory.read_u64(head + 8).value(), first_links);

    const pe::TebModuleInfo replacement{
        0x0000000190000000, "C:\\Apps\\Replacement.dll"};
    ASSERT_TRUE(pe::append_peb_loader_module(
        memory, peb, true, replacement));
    const GuestAddr replacement_links = memory.read_u64(first_links).value();
    EXPECT_EQ(replacement_links, dll_links);  // Removed slot was reused.
    EXPECT_EQ(memory.read_u64(replacement_links + 0x20).value(),
              replacement.base_address);
    const GuestAddr replacement_entry = replacement_links - 0x10;
    const GuestAddr base_name =
        memory.read_u64(replacement_entry + 0x60).value();
    const char16_t expected[] = u"Replacement.dll";
    for (usize i = 0; i < std::size(expected); ++i) {
        EXPECT_EQ(memory.read_u16(base_name + i * 2).value(),
                  static_cast<u16>(expected[i]));
    }
}

TEST(TebSetupTest, X64PebLoaderListDoesNotTruncateLargeModuleSets) {
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_long_mode();

    pe::TebConfig config;
    config.stack_base = 0x0000000200000000;
    config.stack_bottom = 0x00000001FFF00000;
    config.image_base = 0x0000000140000000;
    config.is_64bit = true;
    for (u64 i = 0; i < 96; ++i) {
        config.loaded_modules.push_back(pe::TebModuleInfo{
            0x0000000180000000 + i * 0x100000,
            "C:\\Apps\\Module" + std::to_string(i) + ".dll"});
    }

    const auto result = pe::setup_teb(memory, cpu, config);
    ASSERT_TRUE(result.is_ok());
    const GuestAddr peb = result.value().peb_address;
    const GuestAddr ldr = memory.read_u64(peb + 0x18).value();
    const GuestAddr head = ldr + 0x20;

    usize count = 0;
    GuestAddr links = memory.read_u64(head).value();
    while (links != head) {
        ASSERT_LT(count, config.loaded_modules.size());
        const GuestAddr entry = links - 0x10;
        EXPECT_EQ(memory.read_u64(entry + 0x30).value(),
                  config.loaded_modules[count].base_address);
        links = memory.read_u64(links).value();
        ++count;
    }
    EXPECT_EQ(count, config.loaded_modules.size());
    const GuestAddr last_entry = memory.read_u64(head + 8).value() - 0x10;
    EXPECT_EQ(memory.read_u64(last_entry + 0x30).value(),
              config.loaded_modules.back().base_address);
}

TEST(TebSetupTest, X86PebLoaderListTracksDynamicModules) {
    GuestMemory memory;
    cpu::CpuState cpu;
    cpu.init_protected_mode();

    pe::TebConfig config;
    config.stack_base = 0x70000000;
    config.stack_bottom = 0x6FF00000;
    config.image_base = 0x00400000;
    config.is_64bit = false;

    const auto result = pe::setup_teb(memory, cpu, config);
    ASSERT_TRUE(result.is_ok());
    const GuestAddr peb = result.value().peb_address;
    const GuestAddr ldr = memory.read_u32(peb + 0x0C).value();
    const GuestAddr head = ldr + 0x14;
    const GuestAddr first_links = memory.read_u32(head).value();
    const pe::TebModuleInfo dll{0x10000000, "C:\\Apps\\Plugin.dll"};

    ASSERT_TRUE(pe::append_peb_loader_module(memory, peb, false, dll));
    const GuestAddr dll_links = memory.read_u32(first_links).value();
    EXPECT_EQ(memory.read_u32(dll_links + 0x10).value(),
              static_cast<u32>(dll.base_address));
    EXPECT_EQ(memory.read_u32(dll_links).value(), head);
    EXPECT_EQ(memory.read_u32(head + 4).value(), dll_links);

    ASSERT_TRUE(pe::remove_peb_loader_module(
        memory, peb, false, dll.base_address));
    EXPECT_EQ(memory.read_u32(first_links).value(), head);
    EXPECT_EQ(memory.read_u32(head + 4).value(), first_links);
}

} // namespace

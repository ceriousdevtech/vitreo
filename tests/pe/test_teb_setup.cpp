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


// LDR_DATA_TABLE_ENTRY Win10+ contract. Expected values come from
// tools/oracle_probe/ldr_entry_contract_probe{64,32}.expected and the
// RtlHashUnicodeString values winoracle returns for these names.
struct FakeImage {
    u32 time_date_stamp;
    u32 check_sum;
    bool tls;
    u16 dependent_load_flags;
};

void map_fake_image(GuestMemory& memory, GuestAddr base, bool is_64bit,
                    const FakeImage& image) {
    ASSERT_TRUE(memory.map_region(base, 0x2000, MemoryProtection::ReadWrite).is_ok());
    (void)memory.write_u16(base, 0x5A4D);
    (void)memory.write_u32(base + 0x3C, 0x80);
    const GuestAddr nt = base + 0x80;
    (void)memory.write_u32(nt, 0x00004550);
    (void)memory.write_u32(nt + 0x08, image.time_date_stamp);
    const GuestAddr opt = nt + 0x18;
    (void)memory.write_u16(opt, is_64bit ? 0x20B : 0x10B);
    (void)memory.write_u32(opt + 0x40, image.check_sum);
    (void)memory.write_u32(opt + (is_64bit ? 0x6C : 0x5C), 16);
    const GuestAddr dirs = opt + (is_64bit ? 0x70 : 0x60);
    if (image.tls) {
        (void)memory.write_u32(dirs + 9 * 8, 0x1000);
        (void)memory.write_u32(dirs + 9 * 8 + 4, is_64bit ? 0x28 : 0x18);
    }
    constexpr u32 kLoadConfigRva = 0x1100;
    (void)memory.write_u32(dirs + 10 * 8, kLoadConfigRva);
    (void)memory.write_u32(dirs + 10 * 8 + 4, 0x100);
    (void)memory.write_u32(base + kLoadConfigRva, 0x100);  // Size
    (void)memory.write_u16(base + kLoadConfigRva + (is_64bit ? 0x4E : 0x36),
                           image.dependent_load_flags);
}

struct EntryOffsets {
    u64 ptr, load_links, dll_base, flags, obslc, tls, hash_links, time, ddag,
        node_link, original_base, load_time, base_name_hash, load_reason,
        reference_count, dependent_load_flags, check_sum, ddag_load_count,
        ddag_state, hash_table, hash_head_size, load_head;
};
constexpr EntryOffsets kOffsets64{8, 0x00, 0x30, 0x68, 0x6C, 0x6E, 0x70, 0x80, 0x98,
                                  0xA0, 0xF8, 0x100, 0x108, 0x10C, 0x114, 0x118, 0x120,
                                  0x18, 0x38, 0x100, 0x10, 0x10};
constexpr EntryOffsets kOffsets32{4, 0x00, 0x18, 0x34, 0x38, 0x3A, 0x3C, 0x44, 0x50,
                                  0x54, 0x80, 0x88, 0x90, 0x94, 0x9C, 0xA0, 0xA8,
                                  0x0C, 0x20, 0x100, 0x08, 0x0C};

u64 read_ptr(GuestMemory& memory, bool is_64bit, GuestAddr at) {
    return is_64bit ? memory.read_u64(at).value() : memory.read_u32(at).value();
}

GuestAddr find_entry(GuestMemory& memory, bool is_64bit, GuestAddr ldr, u64 base) {
    const EntryOffsets& o = is_64bit ? kOffsets64 : kOffsets32;
    const GuestAddr head = ldr + o.load_head;
    for (GuestAddr link = read_ptr(memory, is_64bit, head); link != head;
         link = read_ptr(memory, is_64bit, link)) {
        if (read_ptr(memory, is_64bit, link + o.dll_base) == base) return link;
    }
    return 0;
}

struct ExpectedEntry {
    u32 flags;
    u16 obslc;
    u16 tls;
    u32 hash;
    u32 load_reason;
    u32 reference_count;
    u32 ddag_load_count;
};

void expect_entry(GuestMemory& memory, bool is_64bit, GuestAddr ldr, u64 base,
                  const FakeImage* image, const ExpectedEntry& want) {
    const EntryOffsets& o = is_64bit ? kOffsets64 : kOffsets32;
    const GuestAddr entry = find_entry(memory, is_64bit, ldr, base);
    ASSERT_NE(entry, 0u);
    EXPECT_EQ(memory.read_u32(entry + o.flags).value(), want.flags);
    EXPECT_EQ(memory.read_u16(entry + o.obslc).value(), want.obslc);
    EXPECT_EQ(memory.read_u16(entry + o.tls).value(), want.tls);
    EXPECT_EQ(memory.read_u32(entry + o.base_name_hash).value(), want.hash);
    EXPECT_EQ(memory.read_u32(entry + o.load_reason).value(), want.load_reason);
    EXPECT_EQ(memory.read_u32(entry + o.reference_count).value(), want.reference_count);
    EXPECT_EQ(read_ptr(memory, is_64bit, entry + o.original_base), base);
    EXPECT_NE(memory.read_u64(entry + o.load_time).value(), 0u);
    EXPECT_EQ(memory.read_u32(entry + o.time).value(), image ? image->time_date_stamp : 0u);
    EXPECT_EQ(memory.read_u32(entry + o.check_sum).value(), image ? image->check_sum : 0u);
    EXPECT_EQ(memory.read_u32(entry + o.dependent_load_flags).value(),
              image ? image->dependent_load_flags : 0u);
    // HashLinks sits in LdrpHashTable bucket hash & 31.
    const GuestAddr bucket = ldr + o.hash_table + (want.hash & 31) * o.hash_head_size;
    bool in_bucket = false;
    for (GuestAddr link = read_ptr(memory, is_64bit, bucket); link != bucket;
         link = read_ptr(memory, is_64bit, link)) {
        in_bucket |= link == entry + o.hash_links;
    }
    EXPECT_TRUE(in_bucket);
    // DdagNode: one-member Modules ring, LoadCount, LdrModulesReadyToRun.
    const GuestAddr node = read_ptr(memory, is_64bit, entry + o.ddag);
    ASSERT_NE(node, 0u);
    EXPECT_EQ(read_ptr(memory, is_64bit, node), entry + o.node_link);
    EXPECT_EQ(read_ptr(memory, is_64bit, entry + o.node_link), node);
    EXPECT_EQ(memory.read_u32(node + o.ddag_load_count).value(), want.ddag_load_count);
    EXPECT_EQ(memory.read_u32(node + o.ddag_state).value(), 9u);
}

void run_ldr_entry_contract(bool is_64bit, u32 x64_bit21) {
    GuestMemory memory;
    cpu::CpuState cpu;
    if (is_64bit) cpu.init_long_mode(); else cpu.init_protected_mode();
    const u64 exe = is_64bit ? 0x140000000ull : 0x00400000ull;
    const u64 ntdll = is_64bit ? 0x180000000ull : 0x10000000ull;
    const u64 kernel32 = is_64bit ? 0x181000000ull : 0x11000000ull;
    const u64 version = is_64bit ? 0x182000000ull : 0x12000000ull;
    const FakeImage exe_image{0x6AE976DA, 0x0002F1E3, true, 0};
    const FakeImage version_image{0x1F2E3D4C, 0x00012345, false, 0x800};
    map_fake_image(memory, exe, is_64bit, exe_image);
    map_fake_image(memory, version, is_64bit, version_image);

    pe::TebConfig config;
    config.stack_base = is_64bit ? 0x0000000200000000ull : 0x70000000ull;
    config.stack_bottom = config.stack_base - 0x100000;
    config.image_base = exe;
    config.is_64bit = is_64bit;
    config.image_path = "C:\\Apps\\Launcher.exe";
    pe::TebModuleInfo exe_module{exe, config.image_path};
    exe_module.is_executable = true;
    pe::TebModuleInfo ntdll_module{ntdll, "C:\\Windows\\System32\\ntdll.dll"};
    ntdll_module.process_attach_called = true;  // ignored: ntdll is never attached
    pe::TebModuleInfo kernel32_module{kernel32, "C:\\Windows\\System32\\KERNEL32.DLL"};
    kernel32_module.process_attach_called = true;
    kernel32_module.thread_calls_disabled = true;
    config.loaded_modules = {exe_module, ntdll_module, kernel32_module};
    const auto result = pe::setup_teb(memory, cpu, config);
    ASSERT_TRUE(result.is_ok());
    const GuestAddr peb = result.value().peb_address;
    const GuestAddr ldr = read_ptr(memory, is_64bit, peb + (is_64bit ? 0x18 : 0x0C));

    expect_entry(memory, is_64bit, ldr, exe, &exe_image,
                 {0x22CC, 0xFFFF, 0xFFFF, 0x488F2544, 4, 2, 0xFFFFFFFF});
    expect_entry(memory, is_64bit, ldr, ntdll, nullptr,
                 {0xA2C4 | x64_bit21, 0xFFFF, 0, 0xF46857D4, 0, 2, 0xFFFFFFFF});
    expect_entry(memory, is_64bit, ldr, kernel32, nullptr,
                 {0xCA2CC | x64_bit21, 0xFFFF, 0, 0x536CD652, 4, 2, 0xFFFFFFFF});

    pe::TebModuleInfo version_module{version, "C:\\Windows\\System32\\version.dll"};
    version_module.dynamic_load = true;
    version_module.process_attach_called = true;
    ASSERT_TRUE(pe::append_peb_loader_module(memory, peb, is_64bit, version_module));
    const u32 dynamic_flags = 0x8A2CC | x64_bit21;
    expect_entry(memory, is_64bit, ldr, version, &version_image,
                 {dynamic_flags, 6, 0, 0xDAA98CAE, 4, 1, 1});

    // A second LoadLibrary raises DdagNode->LoadCount; the rest is unchanged.
    version_module.load_count = 2;
    version_module.dynamic_load = false;  // fixed at publication, not by the caller
    ASSERT_TRUE(pe::update_peb_loader_module(memory, peb, is_64bit, version_module));
    expect_entry(memory, is_64bit, ldr, version, &version_image,
                 {dynamic_flags, 6, 0, 0xDAA98CAE, 4, 1, 2});

    // Unloading leaves the hash bucket without the entry.
    ASSERT_TRUE(pe::remove_peb_loader_module(memory, peb, is_64bit, version));
    const EntryOffsets& o = is_64bit ? kOffsets64 : kOffsets32;
    const GuestAddr bucket = ldr + o.hash_table + (0xDAA98CAEu & 31) * o.hash_head_size;
    EXPECT_EQ(read_ptr(memory, is_64bit, bucket), bucket);
}

TEST(TebSetupTest, X64LdrEntryMatchesWindowsContract) {
    run_ldr_entry_contract(true, 0x200000);
}

TEST(TebSetupTest, X86LdrEntryMatchesWindowsContract) {
    run_ldr_entry_contract(false, 0);
}

} // namespace

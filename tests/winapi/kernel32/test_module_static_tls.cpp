/// @file test_module_static_tls.cpp
/// @brief Tests for ModuleManager implicit (static) TLS slot allocation, backfill and reuse

#include "module_test_fixture.hpp"

TEST_P(DllNotificationDispatchTest,
       StaticTlsBackfillsLateThreadAndStorageIsReusedAcrossFinalUnload) {
    const bool is_64bit = GetParam();
    char root_template[] = "/tmp/vitreo_dll_tls_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    const auto dll_bytes = build_static_tls_test_dll(is_64bit);
    {
        std::ofstream output(root_path / "tlsprobe.dll", std::ios::binary);
        output.write(reinterpret_cast<const char*>(dll_bytes.data()),
                     static_cast<std::streamsize>(dll_bytes.size()));
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(
        is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, is_64bit);
    modules.set_guest_call([](GuestAddr, const u64*, usize, bool) -> u64 {
        return 1;
    });

    constexpr GuestAddr teb = 0x04000000;
    constexpr GuestAddr tls_array = 0x04001000;
    ASSERT_TRUE(memory.map_region(
        teb, 0x2000, MemoryProtection::ReadWrite).is_ok());
    if (is_64bit) {
        ASSERT_TRUE(memory.write_u64(teb + 0x58, tls_array).is_ok());
    } else {
        ASSERT_TRUE(memory.write_u32(teb + 0x2C,
                                     static_cast<u32>(tls_array)).is_ok());
    }
    modules.set_main_teb(teb);
    modules.register_thread_teb(teb, is_64bit ? 64u : 32u);

    const GuestAddr first_module = modules.load_library("C:\\tlsprobe.dll");
    ASSERT_NE(first_module, 0u);
    const u32 first_slot = memory.read_u32(first_module + 0x1190).value();
    const GuestAddr first_block = is_64bit
        ? memory.read_u64(tls_array + first_slot * 8u).value()
        : memory.read_u32(tls_array + first_slot * 4u).value();
    ASSERT_NE(first_block, 0u);
    EXPECT_EQ(memory.read_u64(first_block).value(), 0xFEDCBA9876543210ull);

    // Model the thread-creation race: this TEB was not present when the DLL
    // loaded, and its creation-time TLS snapshot therefore missed the slot.
    constexpr GuestAddr late_teb = 0x04002000;
    constexpr GuestAddr late_tls_array = 0x04003000;
    ASSERT_TRUE(memory.map_region(
        late_teb, 0x2000, MemoryProtection::ReadWrite).is_ok());
    if (is_64bit) {
        ASSERT_TRUE(memory.write_u64(late_teb + 0x58, late_tls_array).is_ok());
    } else {
        ASSERT_TRUE(memory.write_u32(
            late_teb + 0x2C, static_cast<u32>(late_tls_array)).is_ok());
    }
    modules.register_thread_teb(late_teb, is_64bit ? 64u : 32u);
    const GuestAddr late_block = is_64bit
        ? memory.read_u64(late_tls_array + first_slot * 8u).value()
        : memory.read_u32(late_tls_array + first_slot * 4u).value();
    ASSERT_NE(late_block, 0u);
    EXPECT_NE(late_block, first_block);
    EXPECT_EQ(memory.read_u64(late_block).value(),
              0xFEDCBA9876543210ull);

    // Loader-callback synchronization re-registers an existing TEB. It must
    // repair a slot missed by a concurrent DLL load rather than returning just
    // because the TEB is already known.
    if (is_64bit) {
        ASSERT_TRUE(memory.write_u64(
            late_tls_array + first_slot * 8u, 0).is_ok());
    } else {
        ASSERT_TRUE(memory.write_u32(
            late_tls_array + first_slot * 4u, 0).is_ok());
    }
    modules.register_thread_teb(late_teb, is_64bit ? 64u : 32u);
    const GuestAddr repaired_late_block = is_64bit
        ? memory.read_u64(late_tls_array + first_slot * 8u).value()
        : memory.read_u32(late_tls_array + first_slot * 4u).value();
    ASSERT_NE(repaired_late_block, 0u);
    EXPECT_EQ(memory.read_u64(repaired_late_block).value(),
              0xFEDCBA9876543210ull);

    ASSERT_TRUE(modules.free_library(first_module));
    EXPECT_TRUE(modules.dll_tls_slots().empty());
    EXPECT_EQ(is_64bit
              ? memory.read_u64(tls_array + first_slot * 8u).value()
                  : memory.read_u32(tls_array + first_slot * 4u).value(),
              0u);
    EXPECT_EQ(is_64bit
                  ? memory.read_u64(late_tls_array + first_slot * 8u).value()
                  : memory.read_u32(late_tls_array + first_slot * 4u).value(),
              0u);
    EXPECT_TRUE(memory.read_u8(first_block).is_err());
    EXPECT_TRUE(memory.read_u8(late_block).is_err());
    EXPECT_TRUE(memory.read_u8(repaired_late_block).is_err());

    const GuestAddr second_module = modules.load_library("C:\\tlsprobe.dll");
    ASSERT_EQ(second_module, first_module);
    const u32 second_slot = memory.read_u32(second_module + 0x1190).value();
    const GuestAddr second_block = is_64bit
        ? memory.read_u64(tls_array + second_slot * 8u).value()
        : memory.read_u32(tls_array + second_slot * 4u).value();
    EXPECT_EQ(second_slot, first_slot);
    EXPECT_EQ(second_block, first_block);
    EXPECT_EQ(memory.read_u64(second_block).value(), 0xFEDCBA9876543210ull);

    std::filesystem::remove_all(root_path);
}

// Implicit (__declspec(thread)) TLS indices are not capped at 64 -- that limit
// is TlsAlloc's. A process that loads more TLS-bearing DLLs than a thread's
// ThreadLocalStoragePointer vector holds gets a larger vector on every thread,
// with the existing entries carried over. Satisfactory loads several hundred
// such DLLs; Vitreo refused index 64 onward and every one of them faulted in
// its first thread_local access.
TEST_P(DllNotificationDispatchTest, StaticTlsIndicesGrowPastSixtyFourSlots) {
    const bool is_64bit = GetParam();
    char root_template[] = "/tmp/vitreo_tls_growth_XXXXXX";
    const char* root = ::mkdtemp(root_template);
    ASSERT_NE(root, nullptr);
    const std::filesystem::path root_path(root);
    constexpr u32 kDlls = 80;
    const usize optional_offset = 0x80 + sizeof(u32) + sizeof(pe::CoffHeader);
    for (u32 i = 0; i < kDlls; ++i) {
        auto bytes = build_static_tls_test_dll(is_64bit);
        // Distinct preferred bases (the test image has no relocations), with
        // the TLS directory's absolute VAs moved to match.
        const u64 base = (is_64bit ? 0x0000000180000000ull : 0x18000000ull) +
                         static_cast<u64>(i) * 0x10000u;
        const usize tls_file = 0x200 + (0x1100 - 0x1000);
        if (is_64bit) {
            reinterpret_cast<pe::OptionalHeader64*>(bytes.data() +
                                                     optional_offset)
                ->ImageBase = base;
            const u64 start = base + 0x1180, end = base + 0x1188,
                      index = base + 0x1190;
            std::memcpy(bytes.data() + tls_file + 0, &start, 8);
            std::memcpy(bytes.data() + tls_file + 8, &end, 8);
            std::memcpy(bytes.data() + tls_file + 16, &index, 8);
        } else {
            reinterpret_cast<pe::OptionalHeader32*>(bytes.data() +
                                                     optional_offset)
                ->ImageBase = static_cast<u32>(base);
            const u32 start = static_cast<u32>(base + 0x1180),
                      end = static_cast<u32>(base + 0x1188),
                      index = static_cast<u32>(base + 0x1190);
            std::memcpy(bytes.data() + tls_file + 0, &start, 4);
            std::memcpy(bytes.data() + tls_file + 4, &end, 4);
            std::memcpy(bytes.data() + tls_file + 8, &index, 4);
        }
        write_test_file(root_path / ("tls" + std::to_string(i) + ".dll"), bytes);
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
    ThunkRegistry registry(is_64bit ? 0x00007FFB00000000ull : 0x70000000ull);
    ModuleManager modules(registry);
    modules.attach_dynamic_loader(memory, vfs, is_64bit);
    modules.set_guest_call([](GuestAddr, const u64*, usize, bool) -> u64 {
        return 1;
    });

    const GuestAddr field = is_64bit ? 0x58u : 0x2Cu;
    const auto array_of = [&](GuestAddr teb) -> GuestAddr {
        return is_64bit ? memory.read_u64(teb + field).value()
                        : memory.read_u32(teb + field).value();
    };
    const auto slot_block = [&](GuestAddr teb, u32 slot) -> GuestAddr {
        return is_64bit ? memory.read_u64(array_of(teb) + slot * 8u).value()
                        : memory.read_u32(array_of(teb) + slot * 4u).value();
    };
    // Two existing threads, each created with a 64-entry vector.
    constexpr GuestAddr teb = 0x04000000;
    constexpr GuestAddr other_teb = 0x04002000;
    ASSERT_TRUE(memory.map_region(teb, 0x4000,
                                  MemoryProtection::ReadWrite).is_ok());
    for (GuestAddr t : {teb, other_teb}) {
        const GuestAddr array = t + 0x1000;
        if (is_64bit) {
            ASSERT_TRUE(memory.write_u64(t + field, array).is_ok());
        } else {
            ASSERT_TRUE(memory.write_u32(t + field,
                                         static_cast<u32>(array)).is_ok());
        }
        // Slot 0 belongs to the EXE's own TLS; it must survive a regrow.
        ASSERT_TRUE(memory.write_u32(array, 0x5A5A0000u + static_cast<u32>(t >> 12)).is_ok());
    }
    modules.set_main_teb(teb);
    modules.register_thread_teb(teb, is_64bit ? 64u : 32u, 64);
    modules.register_thread_teb(other_teb, is_64bit ? 64u : 32u, 64);

    std::vector<std::pair<GuestAddr, u32>> loaded;
    for (u32 i = 0; i < kDlls; ++i) {
        const GuestAddr module = modules.load_library(
            "C:\\tls" + std::to_string(i) + ".dll");
        ASSERT_NE(module, 0u) << i;
        loaded.emplace_back(module, memory.read_u32(module + 0x1190).value());
    }
    EXPECT_GE(loaded.back().second, 64u);
    EXPECT_GE(modules.tls_array_capacity(), loaded.back().second + 1);

    for (GuestAddr t : {teb, other_teb}) {
        SCOPED_TRACE(t);
        EXPECT_NE(array_of(t), t + 0x1000);  // the vector was replaced
        EXPECT_EQ(memory.read_u32(array_of(t)).value(),
                  0x5A5A0000u + static_cast<u32>(t >> 12));
        std::vector<GuestAddr> seen;
        for (const auto& [module, slot] : loaded) {
            const GuestAddr block = slot_block(t, slot);
            ASSERT_NE(block, 0u) << slot;
            EXPECT_EQ(memory.read_u64(block).value(), 0xFEDCBA9876543210ull);
            seen.push_back(block);
        }
        std::sort(seen.begin(), seen.end());
        EXPECT_EQ(std::adjacent_find(seen.begin(), seen.end()), seen.end());
    }

    // A thread created afterwards with a vector that is too small is grown
    // and filled when it registers.
    constexpr GuestAddr late_teb = 0x05000000;
    ASSERT_TRUE(memory.map_region(late_teb, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    if (is_64bit) {
        ASSERT_TRUE(memory.write_u64(late_teb + field, late_teb + 0x1000).is_ok());
    } else {
        ASSERT_TRUE(memory.write_u32(late_teb + field,
                                     static_cast<u32>(late_teb + 0x1000)).is_ok());
    }
    modules.register_thread_teb(late_teb, is_64bit ? 64u : 32u, 64);
    for (const auto& [module, slot] : loaded) {
        const GuestAddr block = slot_block(late_teb, slot);
        ASSERT_NE(block, 0u) << slot;
        EXPECT_EQ(memory.read_u64(block).value(), 0xFEDCBA9876543210ull);
    }
    std::filesystem::remove_all(root_path);
}

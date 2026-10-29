// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_buffered_read.cpp
/// @brief Buffered fgetc/fread/ungetc position contracts and the concurrent stream table

#include "crt_stdio_test_fixture.hpp"

TEST_F(CrtStdioTest, BlockBufferedFgetcKeepsFtellAndFseekExact) {
    // A UCRT stream is buffered, so fgetc must not cost one VFS read per byte.
    // The read-ahead that makes it cheap must stay invisible: ftell has to
    // report the logical position, not how far ahead the block was filled, and
    // a seek has to land where the guest asked.
    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr path = 0x10200;
    constexpr GuestAddr mode = 0x10300;

    const std::string host_dir =
        ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp";
    const std::string name = "vitreo_stdio_buffered_read";
    // Longer than one buffered block so a refill boundary is crossed.
    std::string payload;
    payload.reserve(200000);
    for (usize i = 0; i < 200000; ++i) {
        payload.push_back(static_cast<char>('a' + (i % 26)));
    }
    {
        std::ofstream out(host_dir + "/" + name, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(host_dir)).is_ok());
    CrtMemoryManager memory_manager(memory);
    DllExports exports =
        create_crt_stdio_thunks(mgr, vfs, memory_manager, crt_data);

    const auto find_export = [&exports](std::string_view name_) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name_](const ExportEntry& candidate) {
                return candidate.name == name_;
            });
    };
    const auto fsopen = find_export("_fsopen");
    const auto fgetc = find_export("fgetc");
    const auto ftell = find_export("ftell");
    const auto fseek = find_export("fseek");
    const auto close = find_export("fclose");
    ASSERT_NE(fsopen, exports.entries.end());
    ASSERT_NE(fgetc, exports.entries.end());
    ASSERT_NE(ftell, exports.entries.end());
    ASSERT_NE(fseek, exports.entries.end());
    ASSERT_NE(close, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(path, "C:\\" + name).is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(mode, "rb").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, mode).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x40).is_ok());
    fsopen->handler(marshaler);
    const u32 stream = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(stream, 0u);

    const auto call1 = [&](const auto& entry) {
        ASSERT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
        entry->handler(marshaler);
    };

    // Byte-at-a-time reads must return the file's bytes in order.
    for (usize i = 0; i < 1000; ++i) {
        call1(fgetc);
        ASSERT_EQ(static_cast<u32>(cpu.gpr.rax),
                  static_cast<u32>(static_cast<u8>(payload[i]))) << "at " << i;
    }
    // ...and the reported position must be the logical one, not the block end.
    call1(ftell);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1000u);

    // A seek past the filled block must land exactly, and reads continue there.
    ASSERT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 150000).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, SEEK_SET).is_ok());
    fseek->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    call1(ftell);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 150000u);
    call1(fgetc);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              static_cast<u32>(static_cast<u8>(payload[150000])));

    // A backward seek inside an already-filled block must also be honoured.
    ASSERT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, 7).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, SEEK_SET).is_ok());
    fseek->handler(marshaler);
    call1(fgetc);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              static_cast<u32>(static_cast<u8>(payload[7])));

    // EOF must land at the real end of file.
    ASSERT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
    ASSERT_TRUE(memory.write_u32(
        stack + 8, static_cast<u32>(payload.size() - 1)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, SEEK_SET).is_ok());
    fseek->handler(marshaler);
    call1(fgetc);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),
              static_cast<u32>(static_cast<u8>(payload.back())));
    call1(fgetc);
    EXPECT_EQ(static_cast<i32>(static_cast<u32>(cpu.gpr.rax)), -1);

    call1(close);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    std::filesystem::remove(host_dir + "/" + name);
}

TEST_F(CrtStdioTest, BulkAndCharacterReadsShareOneBufferedPosition) {
    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr path = 0x10200;
    constexpr GuestAddr mode = 0x10300;
    constexpr GuestAddr output = 0x18000;

    const std::string host_dir =
        ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp";
    const std::string name = "vitreo_stdio_mixed_buffered_read";
    std::string payload;
    payload.reserve(200000);
    for (usize i = 0; i < 200000; ++i) {
        payload.push_back(static_cast<char>('!' + ((i * 37u) % 90u)));
    }
    {
        std::ofstream out(host_dir + "/" + name, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x20000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(host_dir)).is_ok());
    CrtMemoryManager memory_manager(memory);
    DllExports exports =
        create_crt_stdio_thunks(mgr, vfs, memory_manager, crt_data);

    const auto find_export = [&exports](std::string_view export_name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [export_name](const ExportEntry& candidate) {
                return candidate.name == export_name;
            });
    };
    const auto fsopen = find_export("_fsopen");
    const auto fread = find_export("fread");
    const auto fgetc = find_export("fgetc");
    const auto close = find_export("fclose");
    ASSERT_NE(fsopen, exports.entries.end());
    ASSERT_NE(fread, exports.entries.end());
    ASSERT_NE(fgetc, exports.entries.end());
    ASSERT_NE(close, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(path, "C:\\" + name).is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(mode, "rb").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, mode).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x40).is_ok());
    fsopen->handler(marshaler);
    const u32 stream = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(stream, 0u);

    const auto call_fread = [&](usize count) {
        ASSERT_TRUE(memory.write_u32(stack + 4, output).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, 1).is_ok());
        ASSERT_TRUE(memory.write_u32(
            stack + 12, static_cast<u32>(count)).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 16, stream).is_ok());
        fread->handler(marshaler);
        ASSERT_EQ(static_cast<usize>(cpu.gpr.rax), count);
    };
    const auto call_fgetc = [&]() -> u32 {
        EXPECT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
        fgetc->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };

    // Bulk read first: it leaves a host read-ahead tail. fgetc must consume
    // that tail rather than jumping to the next raw 64 KiB block.
    call_fread(1000);
    std::vector<u8> bytes(1000);
    ASSERT_TRUE(memory.read_bytes(output, bytes.data(), bytes.size()).is_ok());
    EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), payload.begin()));
    EXPECT_EQ(call_fgetc(), static_cast<u8>(payload[1000]));

    // fgetc refills the guest-visible block. A following fread must drain the
    // pending guest bytes rather than reading at the raw block end.
    call_fread(1000);
    ASSERT_TRUE(memory.read_bytes(output, bytes.data(), bytes.size()).is_ok());
    EXPECT_TRUE(std::equal(
        bytes.begin(), bytes.end(), payload.begin() + 1001));
    EXPECT_EQ(call_fgetc(), static_cast<u8>(payload[2001]));

    ASSERT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
    close->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    std::filesystem::remove(host_dir + "/" + name);
}

TEST_F(CrtStdioTest, UcrtVfsUngetcPushesBackOneCharacter) {
    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr path = 0x10200;
    constexpr GuestAddr mode = 0x10300;

    const std::string host_dir =
        ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp";
    const std::string name = "vitreo_stdio_ungetc";
    {
        std::ofstream out(host_dir + "/" + name, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "AB";
    }

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(host_dir)).is_ok());
    CrtMemoryManager memory_manager(memory);
    DllExports exports =
        create_crt_stdio_thunks(mgr, vfs, memory_manager, crt_data);

    const auto find_export = [&exports](std::string_view name_) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name_](const ExportEntry& candidate) {
                return candidate.name == name_;
            });
    };
    const auto fsopen = find_export("_fsopen");
    const auto fgetc = find_export("fgetc");
    const auto ungetc = find_export("ungetc");
    const auto ftell = find_export("ftell");
    const auto close = find_export("fclose");
    ASSERT_NE(fsopen, exports.entries.end());
    ASSERT_NE(fgetc, exports.entries.end());
    ASSERT_NE(ungetc, exports.entries.end());
    ASSERT_NE(ftell, exports.entries.end());
    ASSERT_NE(close, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(path, "C:\\" + name).is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(mode, "rb").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, mode).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x40).is_ok());
    fsopen->handler(marshaler);
    const u32 stream = static_cast<u32>(cpu.gpr.rax);
    ASSERT_NE(stream, 0u);

    const auto call_fgetc = [&]() -> u32 {
        EXPECT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
        fgetc->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };
    const auto call_ungetc = [&](i32 ch) -> u32 {
        EXPECT_TRUE(
            memory.write_u32(stack + 4, static_cast<u32>(ch)).is_ok());
        EXPECT_TRUE(memory.write_u32(stack + 8, stream).is_ok());
        ungetc->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    };

    EXPECT_EQ(call_ungetc('X'), 0xFFFFFFFFu);
    EXPECT_EQ(call_fgetc(), static_cast<u32>('A'));
    EXPECT_EQ(call_ungetc('Z'), static_cast<u32>('Z'));
    EXPECT_EQ(call_ungetc('Y'), 0xFFFFFFFFu);

    ASSERT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
    ftell->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(call_fgetc(), static_cast<u32>('Z'));
    EXPECT_EQ(call_fgetc(), static_cast<u32>('B'));
    EXPECT_EQ(call_fgetc(), 0xFFFFFFFFu);
    EXPECT_EQ(call_ungetc('Q'), static_cast<u32>('Q'));
    EXPECT_EQ(call_fgetc(), static_cast<u32>('Q'));
    EXPECT_EQ(call_ungetc(EOF), 0xFFFFFFFFu);
    EXPECT_EQ(call_fgetc(), 0xFFFFFFFFu);

    ASSERT_TRUE(memory.write_u32(stack + 4, stream).is_ok());
    close->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    std::filesystem::remove(host_dir + "/" + name);
}

TEST_F(CrtStdioTest, UcrtSharedStreamTableSupportsConcurrentLookupAndErase) {
    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr path = 0x20000;
    constexpr GuestAddr mode = 0x20100;
    constexpr usize reader_count = 4;
    constexpr usize close_count = 64;

    const std::string host_dir =
        ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp";
    const std::string name = "vitreo_stdio_test_" +
        std::to_string(static_cast<long long>(::getpid())) +
        "_concurrent_streams";
    {
        std::ofstream out(host_dir + "/" + name, std::ios::binary);
        ASSERT_TRUE(out.is_open());
        out << "AB";
    }

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x20000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(host_dir)).is_ok());
    CrtMemoryManager memory_manager(memory);
    DllExports exports =
        create_crt_stdio_thunks(mgr, vfs, memory_manager, crt_data);

    const auto find_export = [&exports](std::string_view export_name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [export_name](const ExportEntry& candidate) {
                return candidate.name == export_name;
            });
    };
    const auto fsopen = find_export("_fsopen");
    const auto fgetc = find_export("fgetc");
    const auto ungetc = find_export("ungetc");
    const auto close = find_export("fclose");
    ASSERT_NE(fsopen, exports.entries.end());
    ASSERT_NE(fgetc, exports.entries.end());
    ASSERT_NE(ungetc, exports.entries.end());
    ASSERT_NE(close, exports.entries.end());

    cpu::CpuState setup_cpu;
    setup_cpu.init_protected_mode();
    setup_cpu.gpr.rsp = stack;
    Marshaler setup_marshaler{memory};
    setup_marshaler.attach_cpu(setup_cpu);
    ASSERT_TRUE(
        setup_marshaler.write_ansi_string(path, "C:\\" + name).is_ok());
    ASSERT_TRUE(setup_marshaler.write_ansi_string(mode, "rb").is_ok());

    std::vector<u32> streams;
    streams.reserve(reader_count + close_count);
    for (usize i = 0; i < reader_count + close_count; ++i) {
        ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 8, mode).is_ok());
        ASSERT_TRUE(memory.write_u32(stack + 12, 0x40).is_ok());
        fsopen->handler(setup_marshaler);
        const u32 stream = static_cast<u32>(setup_cpu.gpr.rax);
        ASSERT_NE(stream, 0u);
        streams.push_back(stream);
    }

    // Advance the reader streams once so ungetc has a valid logical position.
    for (usize i = 0; i < reader_count; ++i) {
        ASSERT_TRUE(memory.write_u32(stack + 4, streams[i]).is_ok());
        fgetc->handler(setup_marshaler);
        ASSERT_EQ(static_cast<u32>(setup_cpu.gpr.rax),
                  static_cast<u32>('A'));
    }

    std::atomic<bool> start{false};
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    workers.reserve(reader_count + 1);
    for (usize i = 0; i < reader_count; ++i) {
        workers.emplace_back([&, i] {
            cpu::CpuState cpu;
            cpu.init_protected_mode();
            cpu.gpr.rsp = stack + 0x100 + i * 0x40;
            Marshaler marshaler{memory};
            marshaler.attach_cpu(cpu);
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (usize iteration = 0; iteration < 2000; ++iteration) {
                (void)memory.write_u32(cpu.gpr.rsp + 4, 'Z');
                (void)memory.write_u32(cpu.gpr.rsp + 8, streams[i]);
                ungetc->handler(marshaler);
                if (static_cast<u32>(cpu.gpr.rax) !=
                    static_cast<u32>('Z')) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                (void)memory.write_u32(cpu.gpr.rsp + 4, streams[i]);
                fgetc->handler(marshaler);
                if (static_cast<u32>(cpu.gpr.rax) !=
                    static_cast<u32>('Z')) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    workers.emplace_back([&] {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        cpu.gpr.rsp = stack + 0x300;
        Marshaler marshaler{memory};
        marshaler.attach_cpu(cpu);
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (usize i = reader_count; i < streams.size(); ++i) {
            (void)memory.write_u32(cpu.gpr.rsp + 4, streams[i]);
            close->handler(marshaler);
            if (cpu.gpr.rax != 0) {
                failed.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    EXPECT_FALSE(failed.load(std::memory_order_relaxed));

    for (usize i = 0; i < reader_count; ++i) {
        ASSERT_TRUE(memory.write_u32(stack + 4, streams[i]).is_ok());
        close->handler(setup_marshaler);
        EXPECT_EQ(setup_cpu.gpr.rax, 0u);
    }
    std::filesystem::remove(host_dir + "/" + name);
}

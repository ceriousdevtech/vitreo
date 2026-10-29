// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crt_stdio_stream_open.cpp
/// @brief FILE-object buffer cells, fopen failures and modes, and _filelength64

#include "crt_stdio_test_fixture.hpp"

TEST_F(CrtStdioTest, UcrtVfsStreamsExposeMappedBufferCells) {
    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr path = 0x10200;
    constexpr GuestAddr mode = 0x10300;
    constexpr GuestAddr base_out = 0x10400;
    constexpr GuestAddr pointer_out = 0x10404;
    constexpr GuestAddr count_out = 0x10408;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(
                 ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp")).is_ok());
    CrtMemoryManager memory_manager(memory);
    DllExports exports =
        create_crt_stdio_thunks(
            mgr, vfs, memory_manager, crt_data,
            crt_stdio_buffer::Layout::UcrtOpaque32);

    const auto find_export = [&exports](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    const auto fsopen = find_export("_fsopen");
    const auto pointers = find_export("_get_stream_buffer_pointers");
    const auto close = find_export("fclose");
    ASSERT_NE(fsopen, exports.entries.end());
    ASSERT_NE(pointers, exports.entries.end());
    ASSERT_NE(close, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(
        path, "C:\\vitreo_stdio_test_stream_cells").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(mode, "wb").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, mode).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x40).is_ok());
    fsopen->handler(marshaler);
    const GuestAddr stream = cpu.gpr.rax;
    ASSERT_NE(stream, 0u);

    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(stream)).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, base_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, pointer_out).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, count_out).is_ok());
    pointers->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(base_out).value(),
              stream + CrtDataBlock::ACRT_BASE_OFFSET);
    EXPECT_EQ(memory.read_u32(pointer_out).value(),
              stream + CrtDataBlock::ACRT_PTR_OFFSET);
    EXPECT_EQ(memory.read_u32(count_out).value(),
              stream + CrtDataBlock::ACRT_COUNT_OFFSET);
    const u32 base = memory.read_u32(
        stream + CrtDataBlock::ACRT_BASE_OFFSET).value_or(0);
    EXPECT_NE(base, 0u);
    EXPECT_EQ(memory.read_u32(
                  stream + CrtDataBlock::ACRT_PTR_OFFSET).value_or(0), base);
    EXPECT_EQ(memory.read_u32(
                  stream + CrtDataBlock::ACRT_COUNT_OFFSET).value_or(1), 0u);
    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(stream)).is_ok());
    close->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// A FILE object comes from crt_malloc, which recycles freed guest blocks. The
// x64 MSVC STL filebuf reads the ptr/base cells from _get_stream_buffer_pointers
// as 8-byte pointers and memcpy's into *ptr whenever the count cell is
// positive, so stale bytes from the block's previous occupant became a wild
// write into Ark's .text. Poison a same-sized block, free it, and open.
TEST_F(CrtStdioTest, UcrtVfsStreamCellsStartZeroOnRecycledBlock) {
    constexpr GuestAddr stack = 0x10000;
    constexpr GuestAddr path = 0x10200;
    constexpr GuestAddr mode = 0x10300;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        stack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
        'C', std::make_unique<capsule::HostFileSystem>(
                 ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp")).is_ok());
    CrtMemoryManager memory_manager(memory);
    DllExports exports =
        create_crt_stdio_thunks(
            mgr, vfs, memory_manager, crt_data,
            crt_stdio_buffer::Layout::UcrtOpaque32);
    const auto find_export = [&exports](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
    };
    const auto fopen_entry = find_export("fopen");
    const auto close = find_export("fclose");
    ASSERT_NE(fopen_entry, exports.entries.end());
    ASSERT_NE(close, exports.entries.end());

    // Poison a block of the FILE object's size class and free it so the next
    // stream allocation recycles it (reuse is the default).
    const GuestAddr poisoned =
        memory_manager.crt_malloc(CrtDataBlock::ACRT_FILE_SIZE);
    ASSERT_NE(poisoned, 0u);
    for (u64 off = 0; off < CrtDataBlock::ACRT_FILE_SIZE; off += 8) {
        ASSERT_TRUE(memory.write_u64(poisoned + off,
                                     0x6d61472f6d61472fULL).is_ok());
    }
    memory_manager.crt_free(poisoned);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(
        path, "C:\\vitreo_stdio_test_recycled_cells").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(mode, "wb").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, mode).is_ok());
    fopen_entry->handler(marshaler);
    const GuestAddr stream = cpu.gpr.rax;
    ASSERT_NE(stream, 0u);
    EXPECT_EQ(stream, poisoned) << "expected the freed block to be recycled";

    const u64 ptr = memory.read_u64(
        stream + CrtDataBlock::ACRT_PTR_OFFSET).value_or(~0ULL);
    const u64 base = memory.read_u64(
        stream + CrtDataBlock::ACRT_BASE_OFFSET).value_or(~0ULL);
    EXPECT_EQ(ptr >> 32, 0u) << "stale high dword in the 8-byte ptr cell";
    EXPECT_EQ(base >> 32, 0u) << "stale high dword in the 8-byte base cell";
    EXPECT_EQ(ptr, base);
    EXPECT_EQ(memory.read_u32(
                  stream + CrtDataBlock::ACRT_COUNT_OFFSET).value_or(1), 0u);
    // Every other cell (lock, magic, pushback, eof) must be clean too, apart
    // from the fields the open itself sets.
    for (u64 off = 0x18; off < 0x20; off += 4) {
        EXPECT_EQ(memory.read_u32(stream + off).value_or(1), 0u) << off;
    }

    ASSERT_TRUE(memory.write_u32(stack + 4, static_cast<u32>(stream)).is_ok());
    close->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

TEST_F(CrtStdioTest, FailedFopenReportsGuestErrnoForMissingFileAndDirectory) {
    constexpr GuestAddr stack = 0x10000, path = 0x10200, mode = 0x10600;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(stack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(
        ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp")).is_ok());
    CrtMemoryManager memory_manager(memory);
    auto exports = create_crt_stdio_thunks(mgr, vfs, memory_manager, crt_data);
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler m{memory};
    m.attach_cpu(cpu);
    for (const char* name : {"fopen", "_wfopen"}) {
        auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& e) { return e.name == name; });
        ASSERT_NE(entry, exports.entries.end());
        for (const char* suffix : {"missing", "missing/child"}) {
            const std::string filename = "C:\\vitreo_absent_" +
                std::to_string(::getpid()) + "_" + suffix;
            if (name[0] == '_') {
                ASSERT_TRUE(m.write_wide_string(path, filename).is_ok());
                ASSERT_TRUE(m.write_wide_string(mode, "rb").is_ok());
            } else {
                ASSERT_TRUE(m.write_ansi_string(path, filename).is_ok());
                ASSERT_TRUE(m.write_ansi_string(mode, "rb").is_ok());
            }
            ASSERT_TRUE(memory.write_u32(stack + 4, path).is_ok());
            ASSERT_TRUE(memory.write_u32(stack + 8, mode).is_ok());
            ASSERT_TRUE(memory.write_u32(memory_manager.errno_addr(), 0x1234).is_ok());
            entry->handler(m);
            EXPECT_EQ(cpu.gpr.rax, 0u);
            EXPECT_EQ(memory.read_u32(memory_manager.errno_addr()).value(), 2u);
        }
    }
}

TEST_F(CrtStdioTest, Filelength64PreservesOffsetAndValidatesDescriptorsBothAbis) {
    // On Windows: length 0x100000003, errno preserved, offset 7;
    // closed fd returns -1, errno EBADF (9), invalid handler called once.
    const auto host_path = tmp_path("length64");
    { std::ofstream f(host_path); }
    std::filesystem::resize_file(host_path, 0x100000003ull);
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        CrtDataBlock data;
        ASSERT_TRUE(data.initialize(memory));
        capsule::Vfs vfs;
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(
            std::filesystem::path(host_path).parent_path())).is_ok());
        CrtMemoryManager mm(memory);
        auto exports = create_crt_stdio_thunks(mgr, vfs, mm, data);
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x18000;
        Marshaler m(memory);m.attach_cpu(cpu);
        const auto call = [&](const char* name, std::initializer_list<u64> args) {
            auto e = std::find_if(exports.entries.begin(), exports.entries.end(),
                [name](const ExportEntry& entry) { return entry.name == name; });
            EXPECT_NE(e, exports.entries.end());
            if (e == exports.entries.end()) return u64{0};
            usize i = 0;
            for (u64 a : args) {
                if (x64) {
                    if (i == 0) cpu.gpr.rcx = a;
                    if (i == 1) cpu.gpr.rdx = a;
                    if (i == 2) cpu.gpr.r8 = a;
                    if (i == 3) cpu.gpr.r9 = a;
                } else (void)memory.write_u32(0x18004 + i * 4, static_cast<u32>(a));
                ++i;
            }
            e->handler(m);
            return cpu.gpr.rax;
        };
        ASSERT_TRUE(m.write_ansi_string(0x11000, "C:\\" + std::filesystem::path(host_path).filename().string()).is_ok());
        const auto fd = call("_open", {0x11000, 0, 0});
        ASSERT_NE(static_cast<u32>(fd), ~u32{0});
        EXPECT_EQ(call("_lseek", {fd, 7, 0}), 7u);
        mm.set_errno(0x1234);
        call("_filelengthi64", {fd});
        const u64 length = x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xffffffffu) | (cpu.gpr.rdx << 32);
        EXPECT_EQ(length, 0x100000003ull);
        EXPECT_EQ(mm.get_errno(), 0x1234u);
        EXPECT_EQ(call("_lseek", {fd, 0, 1}), 7u);
        call("_close", {fd});
        unsigned invalid_calls = 0;
        mm.exchange_invalid_parameter_handler(0x12000);
        mm.set_guest_caller([&](GuestAddr, const u64*, usize) -> u64 { ++invalid_calls; return 0; });
        call("_filelengthi64", {fd});
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), ~u32{0});
        if (!x64) EXPECT_EQ(static_cast<u32>(cpu.gpr.rdx), ~u32{0});
        EXPECT_EQ(mm.get_errno(), 9u);
        EXPECT_EQ(invalid_calls, 1u);
    }
    std::filesystem::remove(host_path);
}

// 09_crt.md HIGH 13 -- crt_ucrt_io_probe.expected fopen_a "abcde":
// every write on an "a"/"a+" stream lands at EOF even after fseek(0,SEEK_SET).
TEST_F(CrtStdioTest, FopenAppendSeeksToEofBeforeEveryWrite) {
    const auto host_dir = ::getenv("TMPDIR") ? ::getenv("TMPDIR") : "/tmp";
    const std::string leaf =
        "vitreo_fopen_a_" + std::to_string(static_cast<long long>(::getpid())) +
        ".txt";
    const std::filesystem::path host_path =
        std::filesystem::path(host_dir) / leaf;
    {
        std::ofstream out(host_path, std::ios::binary);
        out << "abc";
    }

    constexpr GuestAddr kStack = 0x10000;
    constexpr GuestAddr kPath = 0x10200;
    constexpr GuestAddr kMode = 0x10600;
    constexpr GuestAddr kBuf = 0x10800;
    GuestMemory memory;
    ASSERT_TRUE(
        memory.map_region(kStack, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive(
                      'C',
                      std::make_unique<capsule::HostFileSystem>(host_dir))
                    .is_ok());
    CrtMemoryManager memory_manager(memory);
    auto exports = create_crt_stdio_thunks(
        mgr, vfs, memory_manager, crt_data,
        crt_stdio_buffer::Layout::UcrtOpaque32);
    const auto find_export = [&exports](std::string_view name) {
        return std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& e) { return e.name == name; });
    };
    auto* fopen_e = &*find_export("fopen");
    auto* fwrite_e = &*find_export("fwrite");
    auto* fseek_e = &*find_export("fseek");
    auto* fclose_e = &*find_export("fclose");
    ASSERT_NE(find_export("fopen"), exports.entries.end());
    ASSERT_NE(find_export("fwrite"), exports.entries.end());
    ASSERT_NE(find_export("fseek"), exports.entries.end());
    ASSERT_NE(find_export("fclose"), exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    cpu.gpr.rsp = kStack + 0x800;
    Marshaler m{memory};
    m.attach_cpu(cpu);

    const std::string guest_path = "C:\\" + leaf;
    ASSERT_TRUE(m.write_ansi_string(kPath, guest_path).is_ok());
    ASSERT_TRUE(m.write_ansi_string(kMode, "a").is_ok());
    ASSERT_TRUE(m.write_ansi_string(kBuf, "d").is_ok());
    cpu.gpr.rcx = kPath;
    cpu.gpr.rdx = kMode;
    fopen_e->handler(m);
    const GuestAddr stream = cpu.gpr.rax;
    ASSERT_NE(stream, 0u);

    // fwrite("d")
    cpu.gpr.rcx = kBuf;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = stream;
    fwrite_e->handler(m);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    // fseek(f, 0, SEEK_SET) then fwrite("e") must still append.
    cpu.gpr.rcx = stream;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    fseek_e->handler(m);
    ASSERT_TRUE(m.write_ansi_string(kBuf, "e").is_ok());
    cpu.gpr.rcx = kBuf;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = 1;
    cpu.gpr.r9 = stream;
    fwrite_e->handler(m);
    EXPECT_EQ(cpu.gpr.rax, 1u);

    cpu.gpr.rcx = stream;
    fclose_e->handler(m);

    std::ifstream in(host_path, std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "abcde");
    std::filesystem::remove(host_path);
}

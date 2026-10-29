/// @file test_crt_filesystem.cpp
/// @brief CRT _stat64 / _stat64i32 guest layout contracts

#include <gtest/gtest.h>

#include "capsule/vfs/filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/io/crt_filesystem.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <algorithm>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

capsule::FileInfo sample_info() {
    capsule::FileInfo info{};
    info.volume_id = 2;
    info.file_id = 0x1234;
    info.link_count = 1;
    info.size = 0xABCDu;
    info.last_access_time = 0x01d0000000000001ull;
    info.last_write_time = 0x01d0000000000002ull;
    info.creation_time = 0x01d0000000000003ull;
    info.attributes = capsule::FileAttributes::Normal;
    return info;
}

ExportEntry* find_export(DllExports& exports, const char* name) {
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [name](const ExportEntry& candidate) {
            return candidate.name == name;
        });
    return entry == exports.entries.end() ? nullptr : &*entry;
}

} // namespace

TEST(CrtFilesystemStatLayout, Stat64i32Is48BytesWithoutOverflow) {
    // MSVC struct _stat64i32 is 48 bytes. Writing the 56-byte _stat64 layout
    // into a caller stack slot smashes the next frame (TS3 after _wstat64i32).
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(base, 0x1000, MemoryProtection::ReadWrite).is_ok());
    constexpr GuestAddr out = base;
    constexpr GuestAddr poison = base + 48;
    ASSERT_TRUE(memory.write_u32(poison, 0xDEADBEEFu).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);

    ASSERT_TRUE(write_stat64(m, out, sample_info(), /*size_is_32bit=*/true));
    EXPECT_EQ(memory.read_u32(poison).value(), 0xDEADBEEFu);
    EXPECT_EQ(memory.read_u32(out + 0x14).value(), 0xABCDu);       // st_size
    EXPECT_EQ(memory.read_u64(out + 0x18).value() != 0, true);     // st_atime
    EXPECT_EQ(memory.read_u64(out + 0x20).value() != 0, true);     // st_mtime
    EXPECT_EQ(memory.read_u64(out + 0x28).value() != 0, true);     // st_ctime
}

TEST(CrtFilesystemStatLayout, Stat64KeepsSizeAt0x18) {
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(base, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);

    ASSERT_TRUE(write_stat64(m, base, sample_info(), /*size_is_32bit=*/false));
    EXPECT_EQ(memory.read_u32(base + 0x14).value(), 0u);           // pad
    EXPECT_EQ(memory.read_u64(base + 0x18).value(), 0xABCDu);      // st_size
    EXPECT_EQ(memory.read_u64(base + 0x20).value() != 0, true);    // st_atime
}

TEST(CrtFilesystemGetcwd, ShortBufferSetsErange) {
    // Learn: when buffer is non-NULL and maxlen is shorter than the path
    // (including NUL), _getcwd returns NULL and sets errno to ERANGE.
    // Mono's g_get_current_dir retries on ERANGE; EINVAL makes it give up
    // and feed NULL into g_build_path.
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(base, 0x10000, MemoryProtection::ReadWrite).is_ok());

    capsule::Vfs vfs;
    vfs.set_cwd("C:\\Program Files\\Electronic Arts\\The Sims 3\\Game\\Bin");

    CrtMemoryManager memory_manager{memory, 0x20000000, 0x10000};
    auto exports = create_crt_filesystem_thunks(vfs, memory_manager);
    ExportEntry* entry = find_export(exports, "_getcwd");
    ASSERT_NE(entry, nullptr);

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);

    constexpr GuestAddr buffer = base;
    cpu.gpr.rcx = buffer;
    cpu.gpr.rdx = 0x20;  // 32 chars — shorter than the Sims Bin path
    entry->handler(m);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(memory_manager.errno_addr()).value(), 34u);  // ERANGE

    // Adequately sized buffer succeeds.
    cpu.gpr.rdx = 260;
    entry->handler(m);
    EXPECT_EQ(cpu.gpr.rax, buffer);
    std::string got;
    for (usize i = 0;; ++i) {
        const u8 ch = memory.read_u8(buffer + i).value_or(0);
        if (ch == 0) break;
        got.push_back(static_cast<char>(ch));
    }
    EXPECT_EQ(got, "C:\\Program Files\\Electronic Arts\\The Sims 3\\Game\\Bin");
}

#include "crt_stat_oracle.inc"

TEST(CrtFilesystemStatLayout, AllShapesMatchWindowsOracleBothAbis) {
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory);m.attach_cpu(cpu);
        for (const auto& c : kCrtStatOracle) {
            SCOPED_TRACE(c.name);
            SCOPED_TRACE(c.scenario);
            SCOPED_TRACE(c.path);
            const std::string name = c.name;
            const bool size32 = name == "_stat32" || name == "_stat64i32";
            const bool time32 = name == "_stat32" || name == "_stat32i64";
            capsule::FileInfo info{};
            info.volume_id = 0x98765432; info.file_id = 0x12345678; info.link_count = 7;
            info.size = 3;
            info.attributes = c.scenario == 1 ? capsule::FileAttributes::ReadOnly : capsule::FileAttributes::Normal;
            // Probe input SYSTEMTIME: 2020/2040-02-03 04:05:06 UTC.
            const i64 unix_seconds = c.scenario == 5 ? -312753294ll : c.scenario == 6 ? 32538081906ll : c.scenario == 2 ? 2211854706ll : 1580702706ll;
            info.creation_time = info.last_access_time = info.last_write_time =
                static_cast<u64>(unix_seconds + 11644473600ll) * 10000000ull;
            for (usize i=0;i<64;++i) (void)memory.write_u8(0x10000+i,0xcc);
            bool overflow = false;
            ASSERT_TRUE(write_crt_stat(m, 0x10000, info, size32, time32, 2, c.path != 0, overflow));
            EXPECT_EQ(overflow, c.scenario == 5 || ((c.scenario == 2 || c.scenario == 6) && time32));
            for (usize i=0;i<64;++i) {
                const auto hex = [](char ch) { return ch <= '9' ? ch-'0' : ch-'a'+10; };
                const auto expected = static_cast<u8>((hex(c.bytes[i*2]) << 4) | hex(c.bytes[i*2+1]));
                EXPECT_EQ(memory.read_u8(0x10000+i).value(), expected) << "byte " << i;
            }
        }
    }
}

TEST(CrtFilesystemStatLayout, MissingAndNullPathClearExactlyTheOutputShape) {
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        capsule::Vfs vfs;
        CrtMemoryManager mm(memory);
        auto exports=create_crt_filesystem_thunks(vfs,mm);
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp=0x18000;
        Marshaler m(memory);m.attach_cpu(cpu);
        unsigned calls=0;
        mm.exchange_invalid_parameter_handler(0x12000);
        mm.set_guest_caller([&](GuestAddr,const u64*,usize)->u64{++calls;return 0;});
        for (const char* name : {"_stat32","_stat32i64","_stat64","_stat64i32","_wstat32","_wstat32i64","_wstat64","_wstat64i32"}) {
            const std::string n=name;
            const usize bytes=n.ends_with("32i64") || n.ends_with("64i32") ? 48 : n.ends_with("64") ? 56 : 36;
            for (bool null_path : {false,true}) {
                for (usize i=0;i<64;++i)(void)memory.write_u8(0x11000+i,0xcc);
                if(n.starts_with("_w"))(void)m.write_wide_string(0x10000,"C:\\missing");
                else (void)m.write_ansi_string(0x10000,"C:\\missing");
                const u64 path=null_path?0:0x10000;
                if(x64){cpu.gpr.rcx=path;cpu.gpr.rdx=0x11000;}
                else{(void)memory.write_u32(0x18004,static_cast<u32>(path));(void)memory.write_u32(0x18008,0x11000);}
                const unsigned before=calls;
                auto* entry=find_export(exports,name);ASSERT_NE(entry,nullptr);entry->handler(m);
                EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),~u32{0});
                EXPECT_EQ(mm.get_errno(),null_path?22u:2u);
                EXPECT_EQ(calls,before+(null_path?1:0));
                for(usize i=0;i<64;++i)EXPECT_EQ(memory.read_u8(0x11000+i).value(),i<bytes?0:0xcc);
            }
        }
    }
}

#include "capsule/vfs/host_filesystem.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>

TEST(CrtFilesystemStatLayout, PathThunksUseGuestDriveExecuteSuffixAndRejectSizeOverflow) {
    const auto directory=std::filesystem::temp_directory_path()/
        ("vitreo_stat_contract_"+std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const auto path=directory/"sample.EXE";
    {std::ofstream file(path);file<<"abc";}
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    ASSERT_TRUE(vfs.mount_drive('D',std::make_unique<capsule::HostFileSystem>(directory)).is_ok());
    CrtMemoryManager mm(memory);
    auto exports=create_crt_filesystem_thunks(vfs,mm);
    cpu::CpuState cpu;cpu.init_long_mode();
    Marshaler m(memory);m.attach_cpu(cpu);
    ASSERT_TRUE(m.write_ansi_string(0x10000,"D:\\sample.EXE").is_ok());
    cpu.gpr.rcx=0x10000;cpu.gpr.rdx=0x11000;
    find_export(exports,"_stat32i64")->handler(m);
    EXPECT_EQ(cpu.gpr.rax,0u);
    EXPECT_EQ(memory.read_u32(0x11000).value(),3u);
    EXPECT_EQ(memory.read_u16(0x11004).value(),0u);
    EXPECT_EQ(memory.read_u16(0x11006).value(),0x81ffu);
    EXPECT_EQ(memory.read_u64(0x11018).value(),3u);
    std::filesystem::resize_file(path,0x80000000ull);
    for(const char* name:{"_stat32","_stat64i32"}){
        cpu.gpr.rcx=0x10000;cpu.gpr.rdx=0x11000;
        find_export(exports,name)->handler(m);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),~u32{0});
        EXPECT_EQ(mm.get_errno(),132u); // Windows UCRT EOVERFLOW, not ERANGE
        for(usize i=0;i<(std::string(name)=="_stat32"?36u:48u);++i)
            EXPECT_EQ(memory.read_u8(0x11000+i).value(),0u);
    }
    std::filesystem::remove_all(directory);
}

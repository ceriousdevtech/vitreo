/// @file test_copyfile_exports.cpp
/// @brief CopyFileA/W registration, CopyFileExW last-write time, and related
///        kernel32 copy contracts.
///
/// CopyFileA was unregistered for a long time, which silently routed every
/// ANSI caller to the generic missing-export stub: it returns 0xFFFFFFFF (a
/// TRUE-looking BOOL) and pops zero stdcall bytes, leaking the caller's
/// arguments off the stack. Guard the registration and the arity so the
/// regression cannot recur. See .docs/starcraft-bringup.md §38.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/filesystem/file_ops.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>

#include <algorithm>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

[[nodiscard]] const ExportEntry* find_export(const DllExports& e,
                                             std::string_view name) {
    auto it = std::find_if(e.entries.begin(), e.entries.end(),
                           [&](const ExportEntry& x) { return x.name == name; });
    return (it == e.entries.end()) ? nullptr : &*it;
}

} // namespace

TEST(CopyFileExportsTest, BothAnsiAndWideVariantsAreRegistered) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);

    const auto* a = find_export(exports, "CopyFileA");
    const auto* w = find_export(exports, "CopyFileW");

    ASSERT_NE(a, nullptr) << "CopyFileA missing → ANSI callers hit the "
                             "missing-export stub (fake TRUE + stack leak)";
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(static_cast<bool>(a->handler));
    EXPECT_TRUE(static_cast<bool>(w->handler));
}

TEST(CopyFileExportsTest, BothVariantsCleanUpTwelveStdcallBytes) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);

    // BOOL CopyFile*(LPCSTR/LPCWSTR, LPCSTR/LPCWSTR, BOOL) → 3 args → 12 bytes.
    const auto* a = find_export(exports, "CopyFileA");
    const auto* w = find_export(exports, "CopyFileW");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(w, nullptr);
    EXPECT_EQ(a->stdcall_bytes, 12u);
    EXPECT_EQ(w->stdcall_bytes, 12u);
}

TEST(CopyFileExportsTest, CreateHardLinkAnsiAndWideVariantsAreRegistered) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);

    const auto* a = find_export(exports, "CreateHardLinkA");
    const auto* w = find_export(exports, "CreateHardLinkW");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(w, nullptr);
    EXPECT_TRUE(static_cast<bool>(a->handler));
    EXPECT_TRUE(static_cast<bool>(w->handler));
    EXPECT_EQ(a->stdcall_bytes, 12u);
    EXPECT_EQ(w->stdcall_bytes, 12u);
}

// CopyFileExW last-write time: see test_copyfile_engine.cpp.

// MoveFileEx(NULL, NULL, MOVEFILE_DELAY_UNTIL_REBOOT) is not a queue flush:
// Windows 11 fails it with ERROR_PATH_NOT_FOUND through both entry points, and
// with or without the flag (tools/oracle_probe/s4_movefile_probe.expected
// w_null_null_delay / a_null_null_delay / a_null_null_0). The tests below
// used to expect success, which no Windows build produced.
namespace {
u32 call_move_file_ex(bool wide, u32 src, u32 dst, u32 flags, u32& gle) {
    GuestMemory memory;
    EXPECT_TRUE(memory.map_region(0x1000, 0x100000, MemoryProtection::ReadWrite)
                    .is_ok());
    capsule::Vfs vfs;
    FileOpsManager file_ops(memory, vfs);
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    constexpr GuestAddr kStack = 0x80000;
    cpu.set_sp(kStack);
    EXPECT_TRUE(memory.write_u32(kStack, 0xDEADBEEF).is_ok());
    EXPECT_TRUE(memory.write_u32(kStack + 4, src).is_ok());
    EXPECT_TRUE(memory.write_u32(kStack + 8, dst).is_ok());
    EXPECT_TRUE(memory.write_u32(kStack + 12, flags).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    set_last_error(0xDEADu);
    if (wide) {
        file_ops.impl_MoveFileExW(marshaler);
    } else {
        file_ops.impl_MoveFileExA(marshaler);
    }
    gle = get_last_error();
    return static_cast<u32>(cpu.gpr.rax);
}
}  // namespace

TEST(CopyFileExportsTest, MoveFileExANullNullDelayUntilRebootFailsPathNotFound) {
    u32 gle = 0;
    EXPECT_EQ(call_move_file_ex(false, 0, 0, 0x4, gle), 0u);
    EXPECT_EQ(gle, error_codes::ERROR_PATH_NOT_FOUND);
}

TEST(CopyFileExportsTest, MoveFileExANullNullWithoutDelayFlagFailsPathNotFound) {
    u32 gle = 0;
    EXPECT_EQ(call_move_file_ex(false, 0, 0, 0, gle), 0u);
    EXPECT_EQ(gle, error_codes::ERROR_PATH_NOT_FOUND);
}

TEST(CopyFileExportsTest, MoveFileExWNullNullDelayUntilRebootFailsPathNotFound) {
    u32 gle = 0;
    EXPECT_EQ(call_move_file_ex(true, 0, 0, 0x4, gle), 0u);
    EXPECT_EQ(gle, error_codes::ERROR_PATH_NOT_FOUND);
}

// x86 stdcall cleanup must equal 4 x parameter count (winbase.h / fileapi.h
// prototypes): CreateFile2 has five parameters, MoveFileTransactedW six,
// CopyFileExA/W six, CopyFile2 three.
TEST(CopyFileExportsTest, FileExportsCleanUpTheirPrototypesStackBytes) {
    GuestMemory memory;
    capsule::Vfs vfs;
    HandleTable handles;
    FileManager files(vfs, handles, memory);
    FileOpsManager file_ops(memory, vfs);
    Kernel32ExtManager k32(memory, vfs);
    const DllExports file_exports = make_file_exports(files);
    const DllExports ops_exports = create_file_ops_thunks(file_ops);
    const DllExports k32_exports = create_kernel32_ext_thunks(k32);
    const struct { const DllExports* table; const char* name; u16 bytes; } rows[] = {
        {&file_exports, "CreateFile2", 20},
        {&ops_exports, "MoveFileTransactedW", 24},
        {&ops_exports, "MoveFileExA", 12},
        {&ops_exports, "MoveFileExW", 12},
        {&k32_exports, "CopyFileExA", 24},
        {&k32_exports, "CopyFileExW", 24},
        {&k32_exports, "CopyFile2", 12},
        {&k32_exports, "GetProfileStringA", 20},   // winbase.h: five parameters
        {&k32_exports, "GetProfileStringW", 20},
    };
    for (const auto& row : rows) {
        const auto* e = find_export(*row.table, row.name);
        ASSERT_NE(e, nullptr) << row.name;
        EXPECT_EQ(e->stdcall_bytes, row.bytes) << row.name;
    }
}

// MOVEFILE_DELAY_UNTIL_REBOOT records the operation in HKLM\SYSTEM\
// CurrentControlSet\Control\Session Manager\PendingFileRenameOperations and
// moves nothing now (Learn MoveFileEx). The layout is
// tools/oracle_probe/movefile_delay_probe.expected (Windows 11): each
// operation is "*1\??\<full source>" ("*2" for a source that does not
// exist) then "" for a delete or "*1\??\<full target>" ("*1!\??\..." with
// MOVEFILE_REPLACE_EXISTING); relative names are made full.
#include <capsule/registry/registry.hpp>
#include <winapi/advapi32/registry/registry.hpp>

namespace {
struct PendingRenameFixture {
    GuestMemory memory;
    capsule::Vfs vfs;
    capsule::Registry registry;
    HandleTable handles;
    RegistryThunks reg{registry, handles};
    FileOpsManager file_ops{memory, vfs};
    PendingRenameFixture() {
        EXPECT_TRUE(memory.map_region(0x1000, 0x100000, MemoryProtection::ReadWrite).is_ok());
        file_ops.set_registry(&reg);
    }
    u32 put(GuestAddr at, std::u16string_view s) {
        for (usize i = 0; i <= s.size(); ++i)
            EXPECT_TRUE(memory.write_u16(at + i * 2, i < s.size() ? s[i] : 0).is_ok());
        return static_cast<u32>(at);
    }
    u32 move(u32 src, u32 dst, u32 flags, u32& gle) {
        cpu::CpuState cpu;
        cpu.init_protected_mode();
        constexpr GuestAddr kStack = 0x80000;
        cpu.set_sp(kStack);
        EXPECT_TRUE(memory.write_u32(kStack, 0xDEADBEEF).is_ok());
        EXPECT_TRUE(memory.write_u32(kStack + 4, src).is_ok());
        EXPECT_TRUE(memory.write_u32(kStack + 8, dst).is_ok());
        EXPECT_TRUE(memory.write_u32(kStack + 12, flags).is_ok());
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        set_last_error(0xDEADu);
        file_ops.impl_MoveFileExW(marshaler);
        gle = get_last_error();
        return static_cast<u32>(cpu.gpr.rax);
    }
    std::u16string pending() {
        u64 key = 0;
        EXPECT_EQ(reg.reg_open_key_ex_w(0x80000002, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager", 0,
                                        0x20019, &key), 0u);
        u32 type = 0, size = 4096;
        std::vector<u8> data(size);
        EXPECT_EQ(reg.reg_query_value_ex_w(key, L"PendingFileRenameOperations", &type, data.data(), &size), 0u);
        EXPECT_EQ(type, 7u);  // REG_MULTI_SZ
        (void)reg.reg_close_key(key);
        std::u16string out;
        for (u32 i = 0; i + 1 < size; i += 2) out.push_back(static_cast<char16_t>(data[i] | (data[i + 1] << 8)));
        return out;
    }
};
}  // namespace

TEST(CopyFileExportsTest, MoveFileExDelayUntilRebootRecordsPendingOperations) {
    PendingRenameFixture f;
    u32 gle = 0;
    // A delete of a source that does not exist leaves the last error alone.
    EXPECT_EQ(f.move(f.put(0x2000, u"C:\\gone.txt"), 0, 0x4, gle), 1u);
    EXPECT_EQ(gle, 0xDEADu);
    static constexpr char16_t kFirst[] = u"*2\\??\\C:\\gone.txt\0\0";
    EXPECT_EQ(f.pending(), std::u16string(kFirst, sizeof kFirst / 2));
    // A rename appends after the existing list; REPLACE_EXISTING marks '!'.
    EXPECT_EQ(f.move(f.put(0x2000, u"C:\\b.txt"), f.put(0x3000, u"C:\\dir\\..\\c.txt"), 0x5, gle), 1u);
    EXPECT_EQ(gle, 0u);
    static constexpr char16_t kBoth[] = u"*2\\??\\C:\\gone.txt\0\0*2\\??\\C:\\b.txt\0*1!\\??\\C:\\c.txt\0";
    EXPECT_EQ(f.pending(), std::u16string(kBoth, sizeof kBoth / 2));
}

TEST(CopyFileExportsTest, MoveFileExDelayUntilRebootRejectsCopyAllowedAndEmptyTarget) {
    PendingRenameFixture f;
    u32 gle = 0;
    // movefile_delay_probe copy_allowed: ERROR_INVALID_PARAMETER; empty_dest:
    // ERROR_PATH_NOT_FOUND; nothing recorded.
    EXPECT_EQ(f.move(f.put(0x2000, u"C:\\b.txt"), f.put(0x3000, u"C:\\c.txt"), 0x6, gle), 0u);
    EXPECT_EQ(gle, error_codes::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(f.move(f.put(0x2000, u"C:\\b.txt"), f.put(0x3000, u""), 0x4, gle), 0u);
    EXPECT_EQ(gle, error_codes::ERROR_PATH_NOT_FOUND);
}

/// @file test_comm_port.cpp
/// @brief kernel32 Communications family against the Windows capture
///        tools/oracle_probe/comm_probe.expected (x86 == x64).
///
/// SetupComm was missing, so pyserial's import (and with it every PyInstaller
/// tool that bundles it) died on AttributeError; five neighbours returned TRUE
/// for any handle. Vitreo has no serial devices, so each function must fail
/// exactly as Windows does for the non-comm object it is handed.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/kernel32/kernel32_ext.hpp>
#include <winapi/kernel32/filesystem/file.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/core/marshaler.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <capsule/vfs/vfs.hpp>
#include <capsule/vfs/host_filesystem.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 kSentinel = 0xDEADu;
constexpr GuestAddr kStack = 0x80000;
constexpr GuestAddr kBuf = 0x20000;   // 512-byte output area, 0xCC-filled
constexpr GuestAddr kWord = 0x21000;  // DWORD output, 0xCCCCCCCC-filled

using Impl = void (Kernel32ExtManager::*)(Marshaler&);

struct Outcome {
    u64 ret;
    u32 err;
    usize touched;  // highest byte of kBuf written, +1
    u32 word;
};

class CommRig : public ::testing::Test {
protected:
    void SetUp() override {
        char tmpl[] = "/tmp/vitreo_comm_XXXXXX";
        const char* dir = ::mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        root = dir;
        std::ofstream(root + "/f.bin") << "x";
        std::filesystem::create_directory(root + "/d");
        ASSERT_TRUE(memory.map_region(0x1000, 0x100000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root)).is_ok());
        files = std::make_unique<FileManager>(vfs, handles, memory);
        mgr = std::make_unique<Kernel32ExtManager>(memory, vfs);
        mgr->set_file_manager(files.get());
    }
    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Outcome call(Impl impl, std::initializer_list<u64> args, bool x64 = false,
                 u32 word_in = 0xCCCCCCCCu) {
        for (usize i = 0; i < 512; ++i) (void)memory.write_u8(kBuf + i, 0xCC);
        (void)memory.write_u32(kWord, word_in);
        cpu::CpuState cpu;
        const std::vector<u64> a(args);
        if (x64) {
            cpu.init_long_mode();
            cpu.gpr.rsp = kStack;
            u64* regs[4] = {&cpu.gpr.rcx, &cpu.gpr.rdx, &cpu.gpr.r8, &cpu.gpr.r9};
            for (usize i = 0; i < a.size() && i < 4; ++i) *regs[i] = a[i];
        } else {
            cpu.init_protected_mode();
            cpu.set_sp(kStack);
            (void)memory.write_u32(kStack, 0xDEADBEEF);
            for (usize i = 0; i < a.size(); ++i) {
                (void)memory.write_u32(kStack + 4 + static_cast<GuestAddr>(i) * 4,
                                       static_cast<u32>(a[i]));
            }
        }
        Marshaler m(memory);
        m.attach_cpu(cpu);
        set_last_error(kSentinel);
        ((*mgr).*impl)(m);
        usize touched = 0;
        for (usize i = 0; i < 512; ++i) {
            if (memory.read_u8(kBuf + i).value_or(0xCC) != 0xCC) touched = i + 1;
        }
        return {cpu.gpr.rax, get_last_error(), touched, memory.read_u32(kWord).value_or(0)};
    }

    /// Every handle-based comm function with the probe's arguments; each must
    /// fail with `err` and leave exactly the probe's outputs.
    void expect_family(u64 h, u32 err, bool x64 = false) {
        struct Row { const char* name; Impl impl; std::vector<u64> args; usize touched; u32 word; };
        const u32 untouched = 0xCCCCCCCCu;
        const std::vector<Row> rows = {
            {"ClearCommBreak", &Kernel32ExtManager::impl_ClearCommBreak, {h}, 0, untouched},
            {"ClearCommError", &Kernel32ExtManager::impl_ClearCommError, {h, kWord, kBuf}, 0, untouched},
            {"ClearCommError_nulls", &Kernel32ExtManager::impl_ClearCommError, {h, 0, 0}, 0, untouched},
            {"EscapeCommFunction_SETDTR", &Kernel32ExtManager::impl_EscapeCommFunction, {h, 5}, 0, untouched},
            {"GetCommMask", &Kernel32ExtManager::impl_GetCommMask, {h, kWord}, 0, untouched},
            {"GetCommModemStatus", &Kernel32ExtManager::impl_GetCommModemStatus, {h, kWord}, 0, untouched},
            {"GetCommProperties", &Kernel32ExtManager::impl_GetCommProperties, {h, kBuf}, 0x40, untouched},
            {"GetCommState", &Kernel32ExtManager::impl_GetCommState, {h, kBuf}, 0x1c, untouched},
            {"GetCommTimeouts", &Kernel32ExtManager::impl_GetCommTimeouts, {h, kBuf}, 0, untouched},
            {"PurgeComm", &Kernel32ExtManager::impl_PurgeComm, {h, 0xC}, 0, untouched},
            {"PurgeComm_badflags", &Kernel32ExtManager::impl_PurgeComm, {h, 0x100}, 0, untouched},
            {"SetCommBreak", &Kernel32ExtManager::impl_SetCommBreak, {h}, 0, untouched},
            {"SetCommConfig", &Kernel32ExtManager::impl_SetCommConfig, {h, kBuf + 256, 52}, 0, untouched},
            {"SetCommMask", &Kernel32ExtManager::impl_SetCommMask, {h, 1}, 0, untouched},
            {"SetCommState", &Kernel32ExtManager::impl_SetCommState, {h, kBuf + 256}, 0, untouched},
            {"SetCommTimeouts", &Kernel32ExtManager::impl_SetCommTimeouts, {h, kBuf + 256}, 0, untouched},
            {"SetupComm", &Kernel32ExtManager::impl_SetupComm, {h, 4096, 4096}, 0, untouched},
            {"TransmitCommChar", &Kernel32ExtManager::impl_TransmitCommChar, {h, 'x'}, 0, untouched},
            {"WaitCommEvent", &Kernel32ExtManager::impl_WaitCommEvent, {h, kWord, 0}, 0, untouched},
        };
        for (const Row& r : rows) {
            // The Set* inputs sit at kBuf + 256, inside the watched area:
            // those calls must not write at all.
            Outcome got{};
            switch (r.args.size()) {
                case 1: got = call(r.impl, {r.args[0]}, x64); break;
                case 2: got = call(r.impl, {r.args[0], r.args[1]}, x64); break;
                default: got = call(r.impl, {r.args[0], r.args[1], r.args[2]}, x64); break;
            }
            EXPECT_EQ(got.ret, 0u) << r.name;
            EXPECT_EQ(got.err, err) << r.name;
            EXPECT_EQ(got.touched, r.touched) << r.name;
            EXPECT_EQ(got.word, r.word) << r.name;
        }
        // GetCommConfig with a sufficient size: 50 bytes written, size 52.
        const auto cfg = call(&Kernel32ExtManager::impl_GetCommConfig, {h, kBuf, kWord}, x64, 52);
        EXPECT_EQ(cfg.ret, 0u);
        EXPECT_EQ(cfg.err, err);
        EXPECT_EQ(cfg.touched, 0x32u);
        EXPECT_EQ(cfg.word, 0x34u);
        // EscapeCommFunction with an out-of-range code: 87 before the handle.
        EXPECT_EQ(call(&Kernel32ExtManager::impl_EscapeCommFunction, {h, 0x77}, x64).err,
                  error_codes::ERROR_INVALID_PARAMETER);
    }

    std::string bytes(usize n) {
        std::string s;
        for (usize i = 0; i < n; ++i) {
            char b[3];
            std::snprintf(b, sizeof(b), "%02x", memory.read_u8(kBuf + i).value_or(0));
            s += b;
        }
        return s;
    }

    Handle open(std::string_view path, u32 flags = 0) {
        return files->create_file(path, 0xC0000000u, 1, 3, flags);
    }

    std::string root;
    GuestMemory memory;
    capsule::Vfs vfs;
    HandleTable handles;
    std::unique_ptr<FileManager> files;
    std::unique_ptr<Kernel32ExtManager> mgr;
};

}  // namespace

TEST(CommExportsTest, WholeFamilyIsRegisteredWithWinbaseArity) {
    GuestMemory memory;
    capsule::Vfs vfs;
    Kernel32ExtManager mgr(memory, vfs);
    auto exports = create_kernel32_ext_thunks(mgr);
    const std::vector<std::pair<const char*, u32>> expected = {
        {"ClearCommBreak", 4}, {"ClearCommError", 12}, {"EscapeCommFunction", 8},
        {"GetCommConfig", 12}, {"GetCommMask", 8}, {"GetCommModemStatus", 8},
        {"GetCommProperties", 8}, {"GetCommState", 8}, {"GetCommTimeouts", 8},
        {"PurgeComm", 8}, {"SetCommBreak", 4}, {"SetCommConfig", 12},
        {"SetCommMask", 8}, {"SetCommState", 8}, {"SetCommTimeouts", 8},
        {"SetupComm", 12}, {"TransmitCommChar", 8}, {"WaitCommEvent", 12},
        {"GetDefaultCommConfigA", 12}, {"GetDefaultCommConfigW", 12},
        {"SetDefaultCommConfigA", 12}, {"SetDefaultCommConfigW", 12},
        {"GetCommPorts", 12}, {"OpenCommPort", 12},
    };
    for (const auto& [name, bytes] : expected) {
        const auto n = std::count_if(exports.entries.begin(), exports.entries.end(),
                                     [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_EQ(n, 1) << name << " must be registered exactly once";
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const ExportEntry& e) { return e.name == name; });
        if (it != exports.entries.end()) EXPECT_EQ(it->stdcall_bytes, bytes) << name;
    }
}

TEST_F(CommRig, DiskFilesAndDirectoriesFailWithInvalidParameter) {
    const Handle f = open("C:\\f.bin");
    ASSERT_NE(f, INVALID_HANDLE);
    expect_family(f, error_codes::ERROR_INVALID_PARAMETER);             // file rows
    const Handle d = files->create_file("C:\\d", 0x80000000u, 7, 3, 0x02000000u);
    ASSERT_NE(d, INVALID_HANDLE);
    expect_family(d, error_codes::ERROR_INVALID_PARAMETER);             // dir rows
}

TEST_F(CommRig, PipesFailWithInvalidFunction) {
    Handle rp = INVALID_HANDLE, wp = INVALID_HANDLE;
    ASSERT_TRUE(files->create_anonymous_pipe(rp, wp, false));
    expect_family(rp, error_codes::ERROR_INVALID_FUNCTION);             // anon_pipe rows
}

TEST_F(CommRig, NonFileObjectsFailWithInvalidHandle) {
    const Handle ev = handles.allocate(HandleType::Event, nullptr, 0, 0);
    expect_family(ev, error_codes::ERROR_INVALID_HANDLE);               // event rows
    expect_family(0, error_codes::ERROR_INVALID_HANDLE);                // null rows
    expect_family(0x12340, error_codes::ERROR_INVALID_HANDLE);          // bogus rows
    expect_family(0xFFFFFFFFull, error_codes::ERROR_INVALID_HANDLE);    // invalid, x86
    expect_family(~0ull, error_codes::ERROR_INVALID_HANDLE, /*x64=*/true);
}

TEST_F(CommRig, FailingGettersLeaveTheDefaultStructures) {
    const Handle f = open("C:\\f.bin");
    ASSERT_NE(f, INVALID_HANDLE);
    (void)call(&Kernel32ExtManager::impl_GetCommProperties, {f, kBuf});
    EXPECT_EQ(bytes(0x40), std::string(0x80, '0'));  // GetCommProperties_bytes
    (void)call(&Kernel32ExtManager::impl_GetCommConfig, {f, kBuf, kWord}, false, 52);
    EXPECT_EQ(bytes(0x34),                            // GetCommConfig_bytes
              "34000000010000001c0000000000000001000000000000000000000000000000"
              "000000000100000000000000000000000000cccc");
    (void)call(&Kernel32ExtManager::impl_GetCommState, {f, kBuf});
    EXPECT_EQ(bytes(0x1c),                            // GetCommState_bytes
              "1c000000000000000100000000000000000000000000000000000000");
}

TEST_F(CommRig, GetCommConfigChecksTheSizeBeforeTheHandle) {
    const Handle f = open("C:\\f.bin");
    ASSERT_NE(f, INVALID_HANDLE);
    const auto small = call(&Kernel32ExtManager::impl_GetCommConfig, {f, kBuf, kWord}, false, 4);
    EXPECT_EQ(small.err, error_codes::ERROR_INVALID_PARAMETER);  // GetCommConfig_small
    EXPECT_EQ(small.touched, 0u);
    EXPECT_EQ(small.word, 0x34u);
    const Handle ev = handles.allocate(HandleType::Event, nullptr, 0, 0);
    const auto zero = call(&Kernel32ExtManager::impl_GetCommConfig, {ev, kBuf, kWord}, false, 0);
    EXPECT_EQ(zero.err, error_codes::ERROR_INVALID_PARAMETER);   // event GetCommConfig_size0
    EXPECT_EQ(zero.word, 0x34u);
    const auto big = call(&Kernel32ExtManager::impl_GetCommConfig, {f, kBuf, kWord}, false, 0x100);
    EXPECT_EQ(big.err, error_codes::ERROR_INVALID_PARAMETER);    // GetCommConfig_big
    EXPECT_EQ(big.touched, 0x32u);
    EXPECT_EQ(big.word, 0x34u);
}

TEST_F(CommRig, EscapeCommFunctionValidatesTheCodeFirst) {
    const Handle ev = handles.allocate(HandleType::Event, nullptr, 0, 0);
    for (u64 code = 0; code <= 12; ++code) {
        const auto o = call(&Kernel32ExtManager::impl_EscapeCommFunction, {ev, code});
        EXPECT_EQ(o.ret, 0u) << code;
        EXPECT_EQ(o.err, (code >= 1 && code <= 9) ? error_codes::ERROR_INVALID_HANDLE
                                                  : error_codes::ERROR_INVALID_PARAMETER)
            << code;
    }
}

TEST_F(CommRig, NamedPortsDoNotExist) {
    constexpr GuestAddr kName = 0x22000;
    for (u32 i = 0; i < 5; ++i) (void)memory.write_u16(kName + i * 2, u"COM9"[i]);
    for (Impl impl : {&Kernel32ExtManager::impl_GetDefaultCommConfigW,
                      &Kernel32ExtManager::impl_GetDefaultCommConfigA}) {
        for (u32 size : {52u, 4u}) {  // GetDefaultCommConfigW_COM9 / _COM9_small
            const auto o = call(impl, {kName, kBuf, kWord}, false, size);
            EXPECT_EQ(o.ret, 0u);
            EXPECT_EQ(o.err, error_codes::ERROR_INVALID_PARAMETER);
            EXPECT_EQ(o.touched, 0u);
            EXPECT_EQ(o.word, size);
        }
    }
    for (Impl impl : {&Kernel32ExtManager::impl_SetDefaultCommConfigW,
                      &Kernel32ExtManager::impl_SetDefaultCommConfigA}) {
        const auto o = call(impl, {kName, kBuf + 256, 52});
        EXPECT_EQ(o.ret, 0u);
        EXPECT_EQ(o.err, error_codes::ERROR_INVALID_PARAMETER);
    }
    // OpenCommPort_0 / _2: pointer-wide INVALID_HANDLE_VALUE.
    for (u64 port : {0ull, 1ull, 2ull}) {
        const auto x86 = call(&Kernel32ExtManager::impl_OpenCommPort, {port, 0xC0000000u, 0});
        EXPECT_EQ(x86.ret, 0xFFFFFFFFull);
        EXPECT_EQ(x86.err, error_codes::ERROR_FILE_NOT_FOUND);
        const auto x64 = call(&Kernel32ExtManager::impl_OpenCommPort, {port, 0xC0000000u, 0}, true);
        EXPECT_EQ(x64.ret, ~0ull);
        EXPECT_EQ(x64.err, error_codes::ERROR_FILE_NOT_FOUND);
    }
    // Learn: ERROR_FILE_NOT_FOUND with no comm ports; the result is also the
    // last error, and nothing was found.
    const auto ports = call(&Kernel32ExtManager::impl_GetCommPorts, {kBuf, 8, kWord});
    EXPECT_EQ(ports.ret, error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(ports.err, error_codes::ERROR_FILE_NOT_FOUND);
    EXPECT_EQ(ports.word, 0u);
    EXPECT_EQ(ports.touched, 0u);
}

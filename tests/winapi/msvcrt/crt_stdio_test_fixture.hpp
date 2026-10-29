// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
/// @file crt_stdio_test_fixture.hpp
/// @brief Shared CrtStdioTest fixture and OS-handle rigs for the msvcrt stdio tests.
///
/// Included by every test_crt_stdio*.cpp. CrtStdioTest is a global-namespace class
/// defined identically in each including translation unit, because every TEST_F of
/// a suite must name the same fixture type; all its members are defined in-class.
/// OsHandleRig, StdHandleRig and their address/flag constants live in an anonymous
/// namespace so each translation unit gets its own copy with no external linkage.

#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include <future>
#include <chrono>
#include <vitreo/common.hpp>
#include "capsule/vfs/host_filesystem.hpp"
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/kernel32/thread/thread.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/io/crt_printf.hpp"
#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/console/console.hpp"
#include "winapi/kernel32/handle.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

class CrtStdioTest : public ::testing::Test {
protected:
    CrtStdioManager mgr;

    /// Helper: create a temp file path.
    ///
    /// The pid is part of the name because ctest runs each test in its own
    /// process, in parallel, and TearDown below removes every suffix rather
    /// than just the one its test used. With a shared name that teardown
    /// deletes a concurrently running test's file and its fopen("r") fails.
    static std::string tmp_path(const char* suffix) {
        std::string p = std::string(::getenv("TMPDIR") ? ::getenv("TMPDIR")
                                                        : "/tmp/");
        p += "vitreo_stdio_test_";
        p += std::to_string(static_cast<long long>(::getpid()));
        p += "_";
        p += suffix;
        return p;
    }

    void TearDown() override {
        // Best-effort cleanup of temp files used in tests
        ::remove(tmp_path("basic").c_str());
        ::remove(tmp_path("rw").c_str());
        ::remove(tmp_path("seek").c_str());
        ::remove(tmp_path("eof").c_str());
        ::remove(tmp_path("fgets").c_str());
        ::remove(tmp_path("fputs").c_str());
        ::remove(tmp_path("putcgetc").c_str());
        ::remove(tmp_path("ungetc").c_str());
        ::remove(tmp_path("concurrent_streams").c_str());
        ::remove(tmp_path("fprintf").c_str());
        ::remove(tmp_path("binary").c_str());
        ::remove(tmp_path("stream_cells").c_str());
    }
};

// Learn _get_osfhandle / _open_osfhandle: a CRT descriptor is a thin layer over
// a kernel32 HANDLE. _get_osfhandle returns that handle for a valid fd (the
// invalid parameter handler, then INVALID_HANDLE_VALUE / EBADF otherwise);
// _open_osfhandle allocates a descriptor that takes ownership of a handle;
// _close (or fclose of the descriptor's stream) releases it. Python reads its
// whole standard library through _open + _get_osfhandle + GetFileType, so an
// fd with no handle made every PyInstaller program fail at "encodings".
namespace {

struct OsHandleRig {
    explicit OsHandleRig(bool x64, const std::filesystem::path& dir)
        : files(vfs, handles, memory) {
        EXPECT_TRUE(memory.map_region(0x10000, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        EXPECT_TRUE(data.initialize(memory));
        EXPECT_TRUE(vfs.mount_drive(
            'C', std::make_unique<capsule::HostFileSystem>(dir)).is_ok());
        handles.set_close_callback(
            [this](Handle h, HandleType type, void* object) {
                return files.on_handle_close(h, type, object);
            });
        mgr.set_file_manager(&files);
        exports = create_crt_stdio_thunks(mgr, vfs, mm, data);
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x18000;
        m.attach_cpu(cpu);
        this->x64 = x64;
    }
    u64 call(const char* name, std::initializer_list<u64> args) {
        auto e = std::find_if(exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& entry) { return entry.name == name; });
        EXPECT_NE(e, exports.entries.end()) << name;
        if (e == exports.entries.end()) return 0;
        usize i = 0;
        for (u64 a : args) {
            if (x64) {
                if (i == 0) cpu.gpr.rcx = a;
                if (i == 1) cpu.gpr.rdx = a;
                if (i == 2) cpu.gpr.r8 = a;
                if (i == 3) cpu.gpr.r9 = a;
            } else {
                (void)memory.write_u32(0x18004 + i * 4, static_cast<u32>(a));
            }
            ++i;
        }
        e->handler(m);
        return x64 ? cpu.gpr.rax : (cpu.gpr.rax & 0xffffffffu);
    }
    /// intptr_t results: -1 is 0xffffffff on x86 and all-ones on x64.
    [[nodiscard]] bool is_invalid_handle(u64 v) const {
        return x64 ? v == ~u64{0} : v == 0xffffffffu;
    }
    std::string text(GuestAddr at, usize n) {
        std::string out(n, '\0');
        for (usize i = 0; i < n; ++i) {
            out[i] = static_cast<char>(memory.read_u8(at + i).value_or(0));
        }
        return out;
    }

    GuestMemory memory;
    CrtDataBlock data;
    capsule::Vfs vfs;
    HandleTable handles;
    FileManager files;
    CrtStdioManager mgr;
    CrtMemoryManager mm{memory};
    DllExports exports;
    cpu::CpuState cpu;
    Marshaler m{memory};
    bool x64{false};
};

constexpr u64 kO_RDONLY = 0x0000;
constexpr u64 kO_BINARY = 0x8000;
constexpr GuestAddr kPathAddr = 0x11000;
constexpr GuestAddr kModeAddr = 0x11400;
constexpr GuestAddr kBufAddr = 0x12000;

}  // namespace

// Descriptors 0-2 are bound at start-up to the process's standard handles.
// Expected values: tools/oracle_probe/stdout_pipe_chain_probe.expected
// (Windows 11, identical for msvcrt.dll and the UCRT, x86 and x64). A
// launcher reads a child's output this way: Steam's hardwareupdater (a GUI
// PyInstaller relay) printed its JSON to a CRT fd 1 that went to the host,
// so Steam saw empty output and relaunched it every few seconds.
namespace {

struct StdHandleRig : OsHandleRig {
    StdHandleRig(bool x64, const std::filesystem::path& dir, bool attached = false)
        : OsHandleRig(x64, dir), console(handles, attached) {
        files.set_console_manager(console);
        mgr.set_console_manager(&console);
    }
    /// Everything the pipe's read end holds, as text.
    std::string drain(Handle read_end) {
        std::string out;
        for (;;) {
            u32 got = 0;
            if (!files.read_file(read_end, kBufAddr + 0x800, 0x400, 0, &got) ||
                got == 0) {
                break;
            }
            out += text(kBufAddr + 0x800, got);
            if (got < 0x400) break;
        }
        return out;
    }
    ConsoleManager console;
};

constexpr u64 kO_TEXT = 0x4000;

}  // namespace

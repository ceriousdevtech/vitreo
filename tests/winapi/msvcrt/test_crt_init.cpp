/// @file test_crt_init.cpp
/// @brief Tests for CRT initialization (WIN32-067)

#include <gtest/gtest.h>
#include <algorithm>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/msvcrt/memory/crt_memory.hpp>
#include <winapi/msvcrt/runtime/crt_data.hpp>
#include <winapi/msvcrt/runtime/crt_init.hpp>
#include <winapi/msvcrt/system/crt_environment.hpp>
#include <winapi/kernel32/environment/environment.hpp>
#include <winapi/kernel32/handle.hpp>
#include <winapi/kernel32/process/process.hpp>
#include <winapi/kernel32/thread/thread.hpp>
#include <winapi/kernel32/error.hpp>

namespace {
/// create_crt_init_thunks gained a CrtEnvironmentManager parameter. The
/// exports it returns capture these by reference, so they must outlive the
/// exports — hence one bundle held by value in each test rather than
/// temporaries.
struct CrtEnvFixture {
    explicit CrtEnvFixture(vitreo::GuestMemory& memory)
        : env("test.exe", memory),
          crt_memory(memory),
          crt_env(env, memory, crt_memory) {}
    vitreo::winapi::EnvironmentManager env;
    vitreo::winapi::CrtMemoryManager crt_memory;
    vitreo::winapi::CrtEnvironmentManager crt_env;
};
}  // namespace

using namespace vitreo;
using namespace vitreo::winapi;

class CrtInitTest : public ::testing::Test {
protected:
    CrtInitManager mgr;
};

namespace {
#include "crt_winmain_oracle.inc"
}

TEST_F(CrtInitTest, WideWinMainMatchesWindowsBothAbis) {
    for (bool x64 : {false, true}) {
        GuestMemory memory;
        CrtDataBlock data;
        ASSERT_TRUE(data.initialize(memory));
        CrtEnvFixture environment(memory);
        auto exports=create_crt_init_thunks(mgr,data,environment.crt_env);
        auto found=std::find_if(exports.entries.begin(),exports.entries.end(),[](const ExportEntry& entry){return entry.name=="_get_wide_winmain_command_line";});
        ASSERT_NE(found,exports.entries.end());
        EXPECT_EQ(found->stdcall_bytes,0u);
        cpu::CpuState cpu;
        if(x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler m(memory);m.attach_cpu(cpu);
        const GuestAddr input=data.base()+CrtDataBlock::WCMDLINE_STR_OFFSET;
        auto set_pointer=[&](GuestAddr p){
            if(x64) EXPECT_TRUE(memory.write_u64(data.wcmdln_addr(),p).is_ok());
            else EXPECT_TRUE(memory.write_u32(data.wcmdln_addr(),static_cast<u32>(p)).is_ok());
        };
        for(const auto& row:kWinMainOracle){
            SCOPED_TRACE(std::to_string(x64)+":"+std::to_string(row[0]));
            std::u16string text;
            if(row[0]<std::size(kWinMainInputs)){
                for(const unsigned char* p=reinterpret_cast<const unsigned char*>(kWinMainInputs[row[0]]);*p;++p)text.push_back(*p);
            }else{text={u'p',static_cast<char16_t>(row[0]-std::size(kWinMainInputs)),u'x'};}
            for(usize i=0;i<text.size();++i)ASSERT_TRUE(memory.write_u16(input+i*2,text[i]).is_ok());
            ASSERT_TRUE(memory.write_u16(input+text.size()*2,0).is_ok());
            set_pointer(input);set_last_error(0xdead);
            found->handler(m);
            EXPECT_EQ(cpu.gpr.rax,input+row[3]*2);
            EXPECT_EQ(get_last_error(),row[4]);
        }
        set_pointer(0);set_last_error(0xdead);
        found->handler(m);
        ASSERT_NE(cpu.gpr.rax,0u);
        EXPECT_EQ(memory.read_u16(cpu.gpr.rax).value(),0u);
        EXPECT_EQ(get_last_error(),0xdeadu);
        const u64 first=cpu.gpr.rax;
        found->handler(m);
        EXPECT_EQ(cpu.gpr.rax,first);
    }
}

// 1. Initterm_CallsEachFunction
TEST_F(CrtInitTest, Initterm_CallsEachFunction) {
    int counter = 0;
    CrtInitManager::VoidFunc inc = [&]() { ++counter; };
    mgr.initterm({inc, inc, inc});
    EXPECT_EQ(counter, 3);
}

// 2. Initterm_SkipsNullEntries
TEST_F(CrtInitTest, Initterm_SkipsNullEntries) {
    int counter = 0;
    CrtInitManager::VoidFunc inc = [&]() { ++counter; };
    CrtInitManager::VoidFunc null_func = nullptr;
    mgr.initterm({inc, null_func, inc});
    EXPECT_EQ(counter, 2);
}

// 3. InittermE_ReturnsZeroOnSuccess
TEST_F(CrtInitTest, InittermE_ReturnsZeroOnSuccess) {
    CrtInitManager::IntFunc ok = []() { return 0; };
    int result = mgr.initterm_e({ok, ok, ok});
    EXPECT_EQ(result, 0);
}

// 4. InittermE_StopsOnFirstFailure
TEST_F(CrtInitTest, InittermE_StopsOnFirstFailure) {
    int call_count = 0;
    CrtInitManager::IntFunc ok = [&]() { ++call_count; return 0; };
    CrtInitManager::IntFunc fail = [&]() { ++call_count; return 42; };
    CrtInitManager::IntFunc should_not_run = [&]() { ++call_count; return 0; };
    int result = mgr.initterm_e({ok, fail, should_not_run});
    EXPECT_EQ(result, 42);
    EXPECT_EQ(call_count, 2);
}

// 5. GetMainArgs_DefaultHasProgram
TEST_F(CrtInitTest, GetMainArgs_DefaultHasProgram) {
    EXPECT_EQ(mgr.get_argc(), 1);
    EXPECT_EQ(mgr.get_argv()[0], "vitreo_guest.exe");
}

// 6. GetMainArgs_CustomArgs
TEST_F(CrtInitTest, GetMainArgs_CustomArgs) {
    mgr.set_command_line("prog", {"a", "b"});
    EXPECT_EQ(mgr.get_argc(), 3);
    EXPECT_EQ(mgr.get_argv()[0], "prog");
    EXPECT_EQ(mgr.get_argv()[1], "a");
    EXPECT_EQ(mgr.get_argv()[2], "b");
}

// 7. GetWMainArgs_WideStrings
TEST_F(CrtInitTest, GetWMainArgs_WideStrings) {
    mgr.set_command_line("hello", {"world"});
    const auto& wargv = mgr.get_wargv();
    ASSERT_EQ(wargv.size(), 2u);
    EXPECT_EQ(wargv[0], L"hello");
    EXPECT_EQ(wargv[1], L"world");
}

// 8. AmsgExit_SetsExitRequested
TEST_F(CrtInitTest, AmsgExit_SetsExitRequested) {
    EXPECT_FALSE(mgr.exit_requested());
    mgr.amsg_exit(8);
    EXPECT_TRUE(mgr.exit_requested());
    EXPECT_EQ(mgr.last_error_num(), 8);
}

// 9. SetAppType_StoresValue
TEST_F(CrtInitTest, SetAppType_StoresValue) {
    EXPECT_EQ(mgr.get_app_type(), 0);
    mgr.set_app_type(1);
    EXPECT_EQ(mgr.get_app_type(), 1);
}

// 10. Cexit_DoesNotCrash
TEST_F(CrtInitTest, Cexit_DoesNotCrash) {
    mgr.cexit();
    mgr.c_exit();
    // No crash = pass
}

// 11. ConfigThreadLocale_ReturnsPrevious
TEST_F(CrtInitTest, ConfigThreadLocale_ReturnsPrevious) {
    int prev = mgr.config_thread_locale(1);
    EXPECT_EQ(prev, 0);
    prev = mgr.config_thread_locale(2);
    EXPECT_EQ(prev, 1);
}

// 12. SetNewMode_ReturnsPrevious
TEST_F(CrtInitTest, SetNewMode_ReturnsPrevious) {
    int prev = mgr.set_new_mode(1);
    EXPECT_EQ(prev, 0);
    prev = mgr.set_new_mode(0);
    EXPECT_EQ(prev, 1);
}

TEST_F(CrtInitTest, PArgcReturnsMappedWritableProcessCell) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    CrtEnvFixture crt_env_fixture(memory);
    auto exports = create_crt_init_thunks(mgr, crt_data, crt_env_fixture.crt_env);

    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "__p___argc";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, crt_data.argc_addr());
    ASSERT_EQ(memory.read_u32(crt_data.argc_addr()).value(), 1u);
    ASSERT_TRUE(memory.write_u32(crt_data.argc_addr(), 7).is_ok());
    EXPECT_EQ(memory.read_u32(crt_data.argc_addr()).value(), 7u);
}

TEST_F(CrtInitTest, PArgvReturnsMappedArgvTableFromCommandLine) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(
        memory, CrtDataBlock::DEFAULT_BASE, "Battle.net.exe --session=1"));

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    CrtEnvFixture crt_env_fixture(memory);
    auto exports = create_crt_init_thunks(mgr, crt_data, crt_env_fixture.crt_env);

    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "__p___argv";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, crt_data.argv_var_addr());
    EXPECT_EQ(memory.read_u32(crt_data.argc_addr()).value(), 2u);
    const u32 argv = memory.read_u32(crt_data.argv_var_addr()).value();
    EXPECT_EQ(argv, crt_data.argv_table_addr());
    const u32 arg0 = memory.read_u32(argv).value();
    const u32 arg1 = memory.read_u32(argv + 4).value();
    EXPECT_EQ(memory.read_u32(argv + 8).value(), 0u);
    EXPECT_EQ(marshaler.read_ansi_string(arg0).value(), "Battle.net.exe");
    EXPECT_EQ(marshaler.read_ansi_string(arg1).value(), "--session=1");
}

TEST_F(CrtInitTest, DataImportsResolveHugeAndWcmdlnNotThunkNops) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(
        memory, CrtDataBlock::DEFAULT_BASE, "game.exe -fullscreen"));

    const u64 huge = crt_data.resolve_data_import("_HUGE");
    const u64 wcmdln = crt_data.resolve_data_import("_wcmdln");
    ASSERT_NE(huge, 0u);
    ASSERT_NE(wcmdln, 0u);
    EXPECT_EQ(huge, crt_data.huge_addr());
    EXPECT_EQ(wcmdln, crt_data.wcmdln_addr());

    // Dereferencing a code thunk would yield the NOP prefix 0x90909090.
    EXPECT_NE(memory.read_u32(huge).value(), 0x90909090u);
    EXPECT_EQ(memory.read_u64(huge).value(), 0x7FF0000000000000ull);

    const u32 wide_str = memory.read_u32(wcmdln).value();
    ASSERT_NE(wide_str, 0u);
    EXPECT_NE(wide_str, 0x90909090u);
    EXPECT_EQ(memory.read_u16(wide_str).value(), static_cast<u16>('g'));
    EXPECT_EQ(memory.read_u16(wide_str + 2).value(), static_cast<u16>('a'));
    EXPECT_EQ(memory.read_u16(wide_str + 6).value(), static_cast<u16>('e'));
}

TEST_F(CrtInitTest, SetFmodeAcceptsBinaryAndRejectsInvalid) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    ASSERT_TRUE(memory.map_region(
        0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = 0x80800;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    CrtEnvFixture crt_env_fixture(memory);
    auto exports = create_crt_init_thunks(mgr, crt_data, crt_env_fixture.crt_env);

    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "_set_fmode";
        });
    ASSERT_NE(entry, exports.entries.end());

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0x8000).is_ok()); // _O_BINARY
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u32(crt_data.fmode_addr()).value(), 0x8000u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0x10000).is_ok()); // _O_WTEXT
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 1).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 22u); // EINVAL
}

TEST_F(CrtInitTest, GetNarrowWinmainCommandLineReturnsMappedString) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory, CrtDataBlock::DEFAULT_BASE, "game.exe"));

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    CrtEnvFixture crt_env_fixture(memory);
    auto exports = create_crt_init_thunks(mgr, crt_data, crt_env_fixture.crt_env);

    const auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "_get_narrow_winmain_command_line";
        });
    ASSERT_NE(entry, exports.entries.end());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, crt_data.base() + CrtDataBlock::CMDLINE_STR_OFFSET);
}

// <setjmp.h> reaches the same save through several exported names: `setjmp`
// maps to `_setjmp` (or `_setjmpex`), and MSVC's compiler intrinsics call
// vcruntime's `__intrinsic_setjmp` / `__intrinsic_setjmpex`. Every one must be
// implemented, because the failure mode of *not* implementing one is not a
// missing feature — the missing-export stub returns 0xFFFFFFFF, and a non-zero
// setjmp return means "arrived here via longjmp", so the caller takes its
// error path on the first pass. libpng is built exactly that way.
TEST_F(CrtInitTest, EverySetjmpSpellingSavesTheJumpBuffer) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    ProcessManager process_mgr;
    HandleTable handles;
    ThreadManager thread_mgr(handles);
    CrtMemoryManager crt_memory(memory, 0x20000000, 0x10000);
    auto exports = create_crt_process_thunks(process_mgr, crt_data, thread_mgr,
                                             crt_memory);

    // MSVC's x64 _JUMP_BUFFER, in the order the CRT writes it:
    //   +0 Frame, +8 Rbx, +16 Rsp, +24 Rbp, +32 Rsi, +40 Rdi,
    //   +48 R12 .. +72 R15, +80 Rip.
    constexpr u64 kJbRbx = 8, kJbRsp = 16, kJbRip = 80;
    constexpr u64 kReturnAddress = 0x140001234ull;
    const GuestAddr stack = crt_memory.crt_malloc(0x200);
    const GuestAddr jmp_buf = crt_memory.crt_malloc(0x100);
    ASSERT_NE(stack, 0u);
    ASSERT_NE(jmp_buf, 0u);

    for (const char* name : {"setjmp", "_setjmp", "_setjmp3", "_setjmpex",
                             "__intrinsic_setjmp", "__intrinsic_setjmpex"}) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const ExportEntry& c) {
                                   return c.name == name;
                               });
        ASSERT_NE(it, exports.entries.end()) << name << " is not implemented";

        cpu.gpr.rsp = stack;
        ASSERT_TRUE(memory.write_u64(stack, kReturnAddress).is_ok());
        cpu.gpr.rcx = jmp_buf;
        cpu.gpr.rbx = 0x1111111111111111ull;
        cpu.gpr.rbp = 0x2222222222222222ull;
        cpu.gpr.rax = 0xDEADBEEFDEADBEEFull;

        it->handler(marshaler);

        // The direct pass through setjmp returns zero. Anything else tells the
        // caller a longjmp already happened.
        EXPECT_EQ(cpu.gpr.rax, 0u) << name << " must return 0 on the direct path";
        // And it must actually have saved the buffer, not just returned 0.
        EXPECT_EQ(memory.read_u64(jmp_buf + kJbRip).value_or(0),
                  kReturnAddress) << name;
        EXPECT_EQ(memory.read_u64(jmp_buf + kJbRbx).value_or(0),
                  0x1111111111111111ull) << name;
        EXPECT_EQ(memory.read_u64(jmp_buf + kJbRsp).value_or(0),
                  stack + 8) << name;
    }
}

// __fpe_flt_rounds is the UCRT internal behind <float.h>'s FLT_ROUNDS. It must
// report the C encoding (C17 5.2.4.2.2) of whatever rounding mode _controlfp
// reports, so the macro and the query can never disagree.
//   -1 indeterminable, 0 toward zero, 1 to nearest, 2 toward +inf, 3 toward -inf
TEST_F(CrtInitTest, FpeFltRoundsTracksTheControlfpRoundingMode) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));

    cpu::CpuState cpu;
    // Long mode so arg(0)/arg(1) come from RCX/RDX rather than a guest stack.
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    // The FP-control surface lives with the process thunks, next to
    // _controlfp/_statusfp/_fpreset, because they share one control word.
    ProcessManager process_mgr;
    HandleTable handles;
    ThreadManager thread_mgr(handles);
    CrtMemoryManager crt_memory(memory, 0x20000000, 0x10000);
    auto exports = create_crt_process_thunks(process_mgr, crt_data, thread_mgr,
                                             crt_memory);

    auto find = [&](const char* name) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [&](const ExportEntry& c) {
                                   return c.name == name;
                               });
        EXPECT_NE(it, exports.entries.end()) << name;
        return it;
    };
    const auto rounds = find("__fpe_flt_rounds");
    const auto controlfp = find("_controlfp");
    ASSERT_NE(rounds, exports.entries.end());
    ASSERT_NE(controlfp, exports.entries.end());

    constexpr u32 kMcwRc = 0x00000300u;
    // _RC_NEAR 0, _RC_DOWN 0x100, _RC_UP 0x200, _RC_CHOP 0x300
    struct Case { u32 rc; u64 expected; const char* what; };
    const Case cases[] = {
        {0x00000000u, 1u, "_RC_NEAR -> to nearest"},
        {0x00000100u, 3u, "_RC_DOWN -> toward -inf"},
        {0x00000200u, 2u, "_RC_UP   -> toward +inf"},
        {0x00000300u, 0u, "_RC_CHOP -> toward zero"},
    };
    for (const auto& c : cases) {
        cpu.gpr.rcx = c.rc;
        cpu.gpr.rdx = kMcwRc;
        cpu.gpr.rax = 0;
        controlfp->handler(marshaler);
        // The x87 control word underneath must have moved too, or arithmetic
        // rounds one way while FLT_ROUNDS claims another.
        EXPECT_EQ(cpu.gpr.rax & kMcwRc, c.rc) << c.what;

        cpu.gpr.rax = 0xDEADBEEF;
        rounds->handler(marshaler);
        EXPECT_EQ(cpu.gpr.rax, c.expected) << c.what;
    }

    // Leave the process-wide CRT control word as the CRT startup default.
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = kMcwRc;
    controlfp->handler(marshaler);
}

TEST_F(CrtInitTest, VcruntimeHelpersAreExported) {
    GuestMemory memory;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    CrtEnvFixture crt_env_fixture(memory);
    auto exports = create_crt_init_thunks(mgr, crt_data, crt_env_fixture.crt_env);
    for (const char* name : {"__crtCapturePreviousContext",
                             "__crtUnhandledException",
                             "__crt_debugger_hook"}) {
        const auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        EXPECT_NE(entry, exports.entries.end()) << name;
    }
}

// msvcrt_exception_initenv_probe ("after getmainargs initenv=null",
// "after wgetmainargs winitenv=envp"): __wgetmainargs stores the environment
// it hands back in __winitenv; __getmainargs leaves __initenv alone. Both
// cells are writable data exports (an exe's startup stores into them).
TEST_F(CrtInitTest, WgetmainargsStoresWinitenvGetmainargsLeavesInitenv) {
    for (const bool x64 : {false, true}) {
        GuestMemory memory;
        CrtDataBlock crt_data;
        crt_data.set_guest_64bit(x64);
        ASSERT_TRUE(crt_data.initialize(memory));
        ASSERT_EQ(crt_data.resolve_data_import("__initenv"), crt_data.initenv_addr());
        ASSERT_EQ(crt_data.resolve_data_import("__winitenv"), crt_data.winitenv_addr());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        CrtEnvFixture crt_env_fixture(memory);
        auto exports = create_crt_init_thunks(mgr, crt_data, crt_env_fixture.crt_env);
        auto call = [&](const char* name) {
            const auto entry = std::find_if(exports.entries.begin(), exports.entries.end(),
                                            [&](const ExportEntry& e) { return e.name == name; });
            ASSERT_NE(entry, exports.entries.end()) << name;
            // int __(w)getmainargs(int*, T***, T***, int, _startupinfo*) with
            // the out-pointers in the CRT data block's spare words.
            const u64 out = crt_data.base() + 0x40;
            const u64 sp = crt_data.base() + 0x1F00;
            cpu.gpr.rsp = sp;
            if (x64) {
                cpu.gpr.rcx = out; cpu.gpr.rdx = out + 8; cpu.gpr.r8 = out + 16; cpu.gpr.r9 = 0;
            } else {
                ASSERT_TRUE(memory.write_u32(sp + 4, static_cast<u32>(out)).is_ok());
                ASSERT_TRUE(memory.write_u32(sp + 8, static_cast<u32>(out + 8)).is_ok());
                ASSERT_TRUE(memory.write_u32(sp + 12, static_cast<u32>(out + 16)).is_ok());
                ASSERT_TRUE(memory.write_u32(sp + 16, 0).is_ok());
            }
            entry->handler(marshaler);
        };
        auto read_ptr = [&](u64 at) -> u64 {
            return x64 ? memory.read_u64(at).value_or(0) : memory.read_u32(at).value_or(0);
        };
        call("__getmainargs");
        EXPECT_EQ(read_ptr(crt_data.initenv_addr()), 0u) << x64;
        call("__wgetmainargs");
        const u64 wenv = read_ptr(crt_data.base() + 0x40 + 16);
        EXPECT_NE(wenv, 0u) << x64;
        EXPECT_EQ(read_ptr(crt_data.winitenv_addr()), wenv) << x64;
    }
}

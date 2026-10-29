/// @file test_dbghelp.cpp
/// @brief dbghelp.dll failure contracts, and the missing-export stub's error.
///
/// A failing API that leaves GetLastError() at 0 is indistinguishable from a
/// call that never ran. MiniDumpWriteDump is the sharp case: the caller has
/// already created the .dmp file, so the observable outcome of the failure is
/// a zero-byte crash dump — and with no error code the reporter cannot tell
/// that the write never happened.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/kernel32/error.hpp>
#include <winapi/system/dbghelp/dbghelp.hpp>

#include <algorithm>
#include <string_view>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kRegion = 0x50000;
constexpr GuestAddr kStack = kRegion + 0x800;

[[nodiscard]] const ExportEntry* find_export(const DllExports& e,
                                             std::string_view name) {
    auto it = std::find_if(e.entries.begin(), e.entries.end(),
                           [&](const ExportEntry& x) { return x.name == name; });
    return (it == e.entries.end()) ? nullptr : &*it;
}

u32 call32(const ExportEntry& entry, GuestMemory& memory, u32 arg_count) {
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    EXPECT_TRUE(memory.write_u32(kStack, 0xDEADBEEF).is_ok());
    for (u32 i = 0; i < arg_count; ++i) {
        EXPECT_TRUE(memory.write_u32(kStack + 4 + i * 4, 0).is_ok());
    }
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry.handler(marshaler);
    return static_cast<u32>(cpu.gpr.rax);
}

class DbgHelpTest : public ::testing::Test {
protected:
    GuestMemory memory;
    DllExports exports{create_dbghelp_thunks()};

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                      MemoryProtection::ReadWrite).is_ok());
    }
};

TEST_F(DbgHelpTest, MiniDumpWriteDumpFailsWithCallNotImplemented) {
    const auto* entry = find_export(exports, "MiniDumpWriteDump");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 28u);  // 7 args
    set_last_error(error_codes::ERROR_SUCCESS);

    EXPECT_EQ(call32(*entry, memory, 7), 0u)
        << "no minidump is written, so the honest answer is FALSE";
    // Learn MiniDumpWriteDump: on failure "the last error will be an
    // HRESULT value" (measured 0x80070006 for a bad file handle,
    // s7_dbghelp_probe.expected): HRESULT_FROM_WIN32(ERROR_CALL_NOT_IMPLEMENTED).
    EXPECT_EQ(get_last_error(), 0x80070078u)
        << "a crash reporter reads GetLastError to tell a failed dump from a "
           "written one; zero there means the failure is invisible";
}

TEST_F(DbgHelpTest, MiniDumpWriteDumpRejectsABadFileHandleFirst) {
    // Measured (s7_dbghelp_probe.expected, x86 and x64): hFile 0x1234 is
    // FALSE with HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE) as the last error.
    set_dbghelp_module_lookup(DbghelpModuleLookup{
        {}, {}, [](u64 handle) { return handle == 0x44; }});
    const auto* entry = find_export(exports, "MiniDumpWriteDump");
    ASSERT_NE(entry, nullptr);
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    for (u32 i = 0; i < 7; ++i) {
        ASSERT_TRUE(memory.write_u32(kStack + 4 + i * 4, 0).is_ok());
    }
    ASSERT_TRUE(memory.write_u32(kStack + 4 + 2 * 4, 0x1234).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 0x80070006u);
    // An open file handle gets past validation to the unimplemented dump.
    ASSERT_TRUE(memory.write_u32(kStack + 4 + 2 * 4, 0x44).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(get_last_error(), 0x80070078u);
    set_dbghelp_module_lookup({});
}

// dbgcore.dll carries the minidump entry points, and dbghelp.dll forwards to
// them (Windows 8.1 and later). A guest that finds dbghelp goes looking for
// dbgcore too -- UE loads it on its exit/crash-report path -- and a
// LoadLibrary returning NULL there makes the reporter abandon setup:
//
//   [module] LoadLibrary FAILED: "dbgcore.dll" (not found in thunks or VFS)
//
// The surface is measured, not assumed: a real C:\Windows\System32\dbgcore.dll
// has exactly two named exports.
TEST_F(DbgHelpTest, DbgCoreExportsTheMinidumpPairAndNothingElse) {
    const DllExports core = create_dbgcore_thunks();
    ASSERT_EQ(core.entries.size(), 2u)
        << "the real dbgcore.dll exports exactly MiniDumpWriteDump and "
           "MiniDumpReadDumpStream; adding more invents surface";
    EXPECT_NE(find_export(core, "MiniDumpWriteDump"), nullptr);
    EXPECT_NE(find_export(core, "MiniDumpReadDumpStream"), nullptr);
}

// The two DLLs must not drift: they are one implementation on Windows, so a
// guest reaching a name through either must see the same contract.
TEST_F(DbgHelpTest, DbgCoreAndDbgHelpAgreeOnTheMinidumpContract) {
    const DllExports core = create_dbgcore_thunks();
    for (const char* name : {"MiniDumpWriteDump", "MiniDumpReadDumpStream"}) {
        SCOPED_TRACE(name);
        const auto* in_core = find_export(core, name);
        const auto* in_help = find_export(exports, name);
        ASSERT_NE(in_core, nullptr);
        ASSERT_NE(in_help, nullptr) << "dbghelp forwards this on Windows";
        EXPECT_EQ(in_core->stdcall_bytes, in_help->stdcall_bytes);

        for (const ExportEntry* e : {in_core, in_help}) {
            set_last_error(error_codes::ERROR_SUCCESS);
            const u32 args = e->stdcall_bytes / 4u;
            EXPECT_EQ(call32(*e, memory, args), 0u);
            EXPECT_EQ(get_last_error(),
                      name == std::string_view("MiniDumpWriteDump")
                          ? 0x80070078u  // HRESULT last error (Learn)
                          : error_codes::ERROR_CALL_NOT_IMPLEMENTED);
        }
    }
}

// MiniDumpReadDumpStream reads a dump the CALLER mapped, so a caller that
// ignores the return value goes straight on to dereference the out
// parameters. They must be cleared rather than left holding stack garbage.
TEST_F(DbgHelpTest, MiniDumpReadDumpStreamClearsItsOutParameters) {
    const DllExports core = create_dbgcore_thunks();
    const auto* entry = find_export(core, "MiniDumpReadDumpStream");
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->stdcall_bytes, 20u);  // 5 args

    // Poison the three out slots, then confirm the call zeroes them.
    constexpr GuestAddr kOut = kRegion + 0x100;
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(memory.write_u32(kOut + i * 4, 0xDEADBEEFu).is_ok());
    }
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    Marshaler m(memory);
    m.attach_cpu(cpu);
    cpu.gpr.set32(static_cast<u8>(cpu::RegIndex::RSP), kStack);
    ASSERT_TRUE(memory.write_u32(kStack, 0).is_ok());          // return addr
    ASSERT_TRUE(memory.write_u32(kStack + 4, 0x1000).is_ok()); // BaseOfDump
    ASSERT_TRUE(memory.write_u32(kStack + 8, 3).is_ok());      // StreamNumber
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(memory.write_u32(kStack + 12 + i * 4, kOut + i * 4).is_ok());
    }
    entry->handler(m);
    EXPECT_EQ(cpu.gpr.rax & 0xFFFFFFFFu, 0u);
    for (u32 i = 0; i < 3; ++i) {
        EXPECT_EQ(memory.read_u32(kOut + i * 4).value(), 0u)
            << "out slot " << i << " still holds caller garbage";
    }
}

// tools/oracle_probe/s7_dbghelp_probe.expected: without SymInitialize every
// lookup is ERROR_INVALID_HANDLE; with a session, an address in no module the
// session knows is ERROR_MOD_NOT_FOUND for symbols, ERROR_INVALID_ADDRESS for
// lines, and 0 / ERROR_SUCCESS for SymGetModuleBase64.
TEST_F(DbgHelpTest, SymbolLookupFailuresKeepTheirDocumentedErrors) {
    const auto* from_addr = find_export(exports, "SymGetSymFromAddr64");
    const auto* line = find_export(exports, "SymGetLineFromAddr64");
    const auto* base = find_export(exports, "SymGetModuleBase64");
    const auto* init = find_export(exports, "SymInitialize");
    const auto* cleanup = find_export(exports, "SymCleanup");
    ASSERT_NE(from_addr, nullptr);
    ASSERT_NE(line, nullptr);
    ASSERT_NE(base, nullptr);
    EXPECT_EQ(from_addr->stdcall_bytes, 20u);
    EXPECT_EQ(line->stdcall_bytes, 20u);   // _SymGetLineFromAddr64@20
    EXPECT_EQ(find_export(exports, "SymFromAddr")->stdcall_bytes, 20u);
    EXPECT_EQ(find_export(exports, "SymLoadModule")->stdcall_bytes, 24u);
    EXPECT_EQ(find_export(exports, "SymGetSymFromAddr")->stdcall_bytes, 16u);

    set_last_error(0xDEADu);
    EXPECT_EQ(call32(*from_addr, memory, 5), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);
    set_last_error(0xDEADu);
    EXPECT_EQ(call32(*line, memory, 5), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_INVALID_HANDLE);

    // A session for process handle 0 (the call32 helper passes zeros).
    EXPECT_EQ(call32(*init, memory, 3), 1u);
    set_last_error(0xDEADu);
    EXPECT_EQ(call32(*from_addr, memory, 5), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_MOD_NOT_FOUND);
    set_last_error(0xDEADu);
    EXPECT_EQ(call32(*line, memory, 5), 0u);
    EXPECT_EQ(get_last_error(), 487u);  // ERROR_INVALID_ADDRESS
    set_last_error(0xDEADu);
    EXPECT_EQ(call32(*base, memory, 3), 0u);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_SUCCESS);
    EXPECT_EQ(call32(*cleanup, memory, 1), 1u);
}

TEST_F(DbgHelpTest, SymOptionsStartAsUndnameAndKeepSecureSticky) {
    // Measured initial value SYMOPT_UNDNAME; SYMOPT_SECURE cannot be cleared.
    const auto* set = find_export(exports, "SymSetOptions");
    const auto* get = find_export(exports, "SymGetOptions");
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(kStack + 4, 0x12u).is_ok());
    set->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x12u);
    get->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x12u);
    ASSERT_TRUE(memory.write_u32(kStack + 4, 0x40002u).is_ok());
    set->handler(marshaler);
    ASSERT_TRUE(memory.write_u32(kStack + 4, 0u).is_ok());
    set->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x40000u);
    ASSERT_TRUE(memory.write_u32(kStack + 4, 2u).is_ok());
    set->handler(marshaler);  // leave SYMOPT_UNDNAME|SECURE for other tests
}

// -------------------------------------------------------------------------
// The generic missing-export stub: an import Vitreo does not implement returns
// a failure marker, and must leave a retrievable error behind it.
// -------------------------------------------------------------------------
TEST(MissingExportStubTest, StubSetsCallNotImplemented) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(kRegion, 0x2000,
                                  MemoryProtection::ReadWrite).is_ok());
    ThunkRegistry registry(0x70000000);
    ThunkRegistry::set_missing_export_error_hook(
        [](u32 code) { set_last_error(code); });

    const GuestAddr stub =
        registry.register_missing_export("kernel32.dll", "NotAnApi", 0);
    ASSERT_NE(stub, 0u);
    auto handler = registry.resolve_handler(stub);
    ASSERT_TRUE(handler.is_ok());

    set_last_error(error_codes::ERROR_SUCCESS);
    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(kStack);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    (*handler.value())(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0xFFFFFFFFu);
    EXPECT_EQ(get_last_error(), error_codes::ERROR_CALL_NOT_IMPLEMENTED)
        << "an unimplemented export that returns a failure marker with "
           "GetLastError() == 0 makes the guest log 'failed (GLE 0)'";
    ThunkRegistry::set_missing_export_error_hook(nullptr);
}

}  // namespace

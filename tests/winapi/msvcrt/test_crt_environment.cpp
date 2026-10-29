/// @file test_crt_environment.cpp
/// @brief Tests for guest-visible CRT environment access

#include <gtest/gtest.h>
#include "capsule/vfs/vfs.hpp"

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/kernel32/environment/environment.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/system/crt_environment.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"

#include <algorithm>
#include <array>
#include <string>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

ExportEntry* find_export(DllExports& exports, const char* name) {
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [name](const ExportEntry& candidate) {
            return candidate.name == name;
        });
    return entry == exports.entries.end() ? nullptr : &*entry;
}

std::string read_ascii(GuestMemory& memory, GuestAddr address) {
    std::string result;
    for (;;) {
        const u8 ch = memory.read_u8(address + result.size()).value_or(0);
        if (ch == 0) return result;
        result.push_back(static_cast<char>(ch));
    }
}

class CrtEnvironmentTest : public ::testing::Test {
protected:
    static constexpr GuestAddr kScratch = 0x10000;

    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            kScratch, 0x10000, MemoryProtection::ReadWrite).is_ok());
        environment.set_variable("BattleNetTest", "launcher");
        cpu.init_long_mode();
        marshaler.attach_cpu(cpu);
        exports = create_crt_environment_thunks(manager, vfs);
    }

    GuestMemory memory;
    // _searchenv/_wsearchenv walk the variable's directory list through
    // the VFS, so the thunks need one.
    capsule::Vfs vfs;
    EnvironmentManager environment{"test.exe", memory};
    CrtMemoryManager memory_manager{memory, 0x20000000, 0x10000};
    CrtEnvironmentManager manager{environment, memory, memory_manager};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

} // namespace

TEST_F(CrtEnvironmentTest, ExportsCoverObservedUcrtEnvironmentImports) {
    constexpr std::array<const char*, 6> required = {
        "getenv", "_wgetenv", "getenv_s", "_wgetenv_s",
        "_dupenv_s", "_wdupenv_s",
    };
    for (const char* name : required) {
        EXPECT_NE(find_export(exports, name), nullptr) << name;
    }
}

TEST_F(CrtEnvironmentTest, GetenvReturnsStableCaseInsensitiveGuestPointer) {
    const GuestAddr first = manager.getenv_ansi("battlenettest");
    const GuestAddr second = manager.getenv_ansi("BATTLENETTEST");
    ASSERT_NE(first, 0u);
    EXPECT_EQ(second, first);
    EXPECT_EQ(read_ascii(memory, first), "launcher");
    EXPECT_EQ(manager.getenv_ansi("MISSING_VARIABLE"), 0u);
}

TEST_F(CrtEnvironmentTest, GetenvSReportsRequiredSizeAndCopiesValue) {
    ExportEntry* entry = find_export(exports, "getenv_s");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr required = kScratch;
    constexpr GuestAddr buffer = kScratch + 0x100;
    constexpr GuestAddr name = kScratch + 0x200;
    ASSERT_TRUE(marshaler.write_ansi_string(name, "battlenettest").is_ok());

    cpu.gpr.rcx = required;
    cpu.gpr.rdx = buffer;
    cpu.gpr.r8 = 32;
    cpu.gpr.r9 = name;
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(required).value(), 9u);
    EXPECT_EQ(read_ascii(memory, buffer), "launcher");
}

TEST_F(CrtEnvironmentTest, GetenvSClearsTooSmallDestination) {
    ExportEntry* entry = find_export(exports, "getenv_s");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr required = kScratch;
    constexpr GuestAddr buffer = kScratch + 0x100;
    constexpr GuestAddr name = kScratch + 0x200;
    ASSERT_TRUE(marshaler.write_ansi_string(name, "BattleNetTest").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(buffer, "old").is_ok());

    cpu.gpr.rcx = required;
    cpu.gpr.rdx = buffer;
    cpu.gpr.r8 = 3;
    cpu.gpr.r9 = name;
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 34u);
    EXPECT_EQ(memory.read_u64(required).value(), 9u);
    EXPECT_EQ(memory.read_u8(buffer).value(), 0u);
    EXPECT_EQ(memory.read_u32(memory_manager.errno_addr()).value(), 34u);
}

TEST_F(CrtEnvironmentTest, WgetenvSUsesUtf16ElementCount) {
    environment.set_variable("WideValue", "snow");
    ExportEntry* entry = find_export(exports, "_wgetenv_s");
    ASSERT_NE(entry, nullptr);
    constexpr GuestAddr required = kScratch;
    constexpr GuestAddr buffer = kScratch + 0x100;
    constexpr GuestAddr name = kScratch + 0x200;
    ASSERT_TRUE(marshaler.write_wide_string(name, "WideValue").is_ok());

    cpu.gpr.rcx = required;
    cpu.gpr.rdx = buffer;
    cpu.gpr.r8 = 16;
    cpu.gpr.r9 = name;
    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(required).value(), 5u);
    EXPECT_EQ(memory.read_u16(buffer).value(), static_cast<u16>('s'));
    EXPECT_EQ(memory.read_u16(buffer + 8).value(), 0u);
}

// =============================================================================
// _wputenv_s
// =============================================================================
//
// Learn (_putenv_s, _wputenv_s): errno_t, 0 on success. varname == NULL or
// value_string == NULL -> EINVAL (and errno = EINVAL). An empty value_string
// removes the variable. Steam imports this from
// api-ms-win-crt-environment-l1-1-0.dll, where it was reaching the
// missing-export stub: 0xFFFFFFFF happens to be a nonzero errno_t, so the guest
// saw an unexplainable failure instead of a variable that got set.

TEST_F(CrtEnvironmentTest, WPutenvSSetsAVariableVisibleToTheNarrowStore) {
    ExportEntry* entry = find_export(exports, "_wputenv_s");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr name = kScratch;
    constexpr GuestAddr value = kScratch + 0x100;
    ASSERT_TRUE(marshaler.write_wide_string(name, "VitreoWide").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(value, "set-by-wide").is_ok());

    cpu.gpr.rcx = name;
    cpu.gpr.rdx = value;
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    // One environment table behind _environ/_wenviron: getenv must see it.
    ExportEntry* getenv_entry = find_export(exports, "getenv");
    ASSERT_NE(getenv_entry, nullptr);
    constexpr GuestAddr narrow_name = kScratch + 0x200;
    ASSERT_TRUE(marshaler.write_ansi_string(narrow_name, "VitreoWide").is_ok());
    cpu.gpr.rcx = narrow_name;
    getenv_entry->handler(marshaler);
    ASSERT_NE(cpu.gpr.rax, 0u);
    EXPECT_EQ(read_ascii(memory, static_cast<GuestAddr>(cpu.gpr.rax)),
              "set-by-wide");
}

TEST_F(CrtEnvironmentTest, WPutenvSEmptyValueRemovesTheVariable) {
    ExportEntry* entry = find_export(exports, "_wputenv_s");
    ASSERT_NE(entry, nullptr);

    constexpr GuestAddr name = kScratch;
    constexpr GuestAddr value = kScratch + 0x100;
    ASSERT_TRUE(marshaler.write_wide_string(name, "BattleNetTest").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(value, "").is_ok());

    cpu.gpr.rcx = name;
    cpu.gpr.rdx = value;
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);

    ExportEntry* getenv_entry = find_export(exports, "getenv");
    ASSERT_NE(getenv_entry, nullptr);
    constexpr GuestAddr narrow_name = kScratch + 0x200;
    ASSERT_TRUE(
        marshaler.write_ansi_string(narrow_name, "BattleNetTest").is_ok());
    cpu.gpr.rcx = narrow_name;
    getenv_entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u) << "an empty value removes the variable";
}

TEST_F(CrtEnvironmentTest, WPutenvSNullArgumentsReturnEinval) {
    ExportEntry* entry = find_export(exports, "_wputenv_s");
    ASSERT_NE(entry, nullptr);
    constexpr u64 kEinval = 22u;

    constexpr GuestAddr name = kScratch;
    constexpr GuestAddr value = kScratch + 0x100;
    ASSERT_TRUE(marshaler.write_wide_string(name, "VitreoWide").is_ok());
    ASSERT_TRUE(marshaler.write_wide_string(value, "v").is_ok());

    cpu.gpr.rcx = 0;      // varname == NULL
    cpu.gpr.rdx = value;
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, kEinval);
    EXPECT_EQ(memory.read_u32(memory_manager.errno_addr()).value(), 22u);

    cpu.gpr.rcx = name;
    cpu.gpr.rdx = 0;      // value_string == NULL
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, kEinval);

    // Empty name, matching the narrow _putenv_s: "=VALUE" is not a variable.
    constexpr GuestAddr empty = kScratch + 0x300;
    ASSERT_TRUE(marshaler.write_wide_string(empty, "").is_ok());
    cpu.gpr.rcx = empty;
    cpu.gpr.rdx = value;
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, kEinval);
}

TEST_F(CrtEnvironmentTest, EnvironVariableAddrIsStableDataCell) {
    const GuestAddr first = manager.environ_variable_addr(false);
    const GuestAddr second = manager.environ_variable_addr(false);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(second, first);
    // Must not look like a NOP-prefixed code thunk when read as data.
    EXPECT_NE(memory.read_u32(first).value_or(0x90909090u), 0x90909090u);
}

TEST_F(CrtEnvironmentTest, WideEnvironmentAccessorPublishesStableUtf16PointerCell) {
    auto* accessor = find_export(exports, "__p__wenviron");
    auto* wgetenv = find_export(exports, "_wgetenv");
    ASSERT_NE(accessor, nullptr);
    ASSERT_NE(wgetenv, nullptr);
    accessor->handler(marshaler);
    const GuestAddr cell = cpu.gpr.rax;
    ASSERT_NE(cell, 0u);
    EXPECT_EQ(memory.read_u64(cell).value(), 0u);
    environment.set_variable("Unicode", "snow\xE2\x98\x83");
    ASSERT_TRUE(marshaler.write_wide_string(kScratch, "absent").is_ok());
    cpu.gpr.rcx = kScratch;
    wgetenv->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    const GuestAddr array = memory.read_u64(cell).value();
    ASSERT_NE(array, 0u);
    bool found = false;
    for (usize i = 0; i < 100; ++i) {
        const GuestAddr entry = memory.read_u64(array + i * 8).value();
        if (entry == 0) break;
        auto value = marshaler.read_wide_string(entry);
        ASSERT_TRUE(value.is_ok());
        if (value.value() == "Unicode=snow\xE2\x98\x83") found = true;  // block keeps the spelling
    }
    EXPECT_TRUE(found);
    accessor->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, cell);
    EXPECT_EQ(memory.read_u64(cell).value(), array);
    ASSERT_TRUE(manager.put_variable("Unicode", "changed", false));
    EXPECT_NE(memory.read_u64(cell).value(), array);
    accessor->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, cell);
}

TEST_F(CrtEnvironmentTest, WideEnvironmentAccessorUsesX86PointerSlots) {
    cpu.init_protected_mode();
    marshaler.attach_cpu(cpu);
    auto* accessor = find_export(exports, "__p__wenviron");
    ASSERT_NE(accessor, nullptr);
    const GuestAddr array = manager.wenviron_array_addr(false);
    ASSERT_NE(array, 0u);
    accessor->handler(marshaler);
    EXPECT_EQ(memory.read_u32(cpu.gpr.rax).value(), array);
    const GuestAddr first = memory.read_u32(array).value();
    ASSERT_NE(first, 0u);
    EXPECT_TRUE(marshaler.read_wide_string(first).is_ok());
}

// =============================================================================
// _dupenv_s / _wdupenv_s  (Learn: dupenv-s-wdupenv-s)
//
// Expected behaviour from the Learn page: found -> *buffer is a CRT-heap copy
// the caller frees, *numberOfElements its size (value + terminator); not found
// -> *buffer NULL, *numberOfElements 0, return 0; numberOfElements may be
// NULL; buffer or varname NULL -> invalid parameter handler, errno EINVAL,
// return EINVAL; lookup is case-insensitive. Both ABIs: the out-parameters
// are pointer-sized (size_t*, char**).
// =============================================================================

namespace {

class DupenvHarness {
public:
    static constexpr GuestAddr kBase = 0x40000000;
    static constexpr GuestAddr kBufferOut = kBase + 0x100;
    static constexpr GuestAddr kCountOut = kBase + 0x200;
    static constexpr GuestAddr kName = kBase + 0x300;
    static constexpr u32 kEinval = 22;

    explicit DupenvHarness(bool x64) : m_x64(x64) {
        EXPECT_TRUE(memory.map_region(kBase, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        environment.set_variable("VitreoDupenv", "launcher");
        environment.set_variable("VitreoWideDupenv", "snow\xE2\x98\x83");
        memory_manager.exchange_invalid_parameter_handler(0x1234);
        memory_manager.set_guest_caller(
            [this](GuestAddr fn, const u64*, usize count) -> u64 {
                EXPECT_EQ(fn, 0x1234u);
                EXPECT_EQ(count, 5u);
                ++handler_calls;
                errno_in_handler = memory_manager.get_errno();
                return 0;
            });
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kBase + 0x8000);
        marshaler.attach_cpu(cpu);
        exports = create_crt_environment_thunks(manager, vfs);
    }

    /// Store a pointer-sized value the way the guest's own ABI lays it out.
    void put_ptr(GuestAddr at, u64 value) {
        if (m_x64) ASSERT_TRUE(memory.write_u64(at, value).is_ok());
        else ASSERT_TRUE(memory.write_u32(at, static_cast<u32>(value)).is_ok());
    }
    [[nodiscard]] u64 get_ptr(GuestAddr at) {
        return m_x64 ? memory.read_u64(at).value()
                     : memory.read_u32(at).value();
    }

    void call(const char* name, GuestAddr buffer_out, GuestAddr count_out,
              GuestAddr varname) {
        // Poison both out-parameters so a missed store is visible.
        put_ptr(kBufferOut, 0xA5A5A5A5A5A5A5A5ull);
        put_ptr(kCountOut, 0x5A5A5A5A5A5A5A5Aull);
        if (m_x64) {
            cpu.gpr.rcx = buffer_out;
            cpu.gpr.rdx = count_out;
            cpu.gpr.r8 = varname;
        } else {
            const u32 args[] = {static_cast<u32>(buffer_out),
                                static_cast<u32>(count_out),
                                static_cast<u32>(varname)};
            for (u32 i = 0; i < 3; ++i) {
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 4 + i * 4, args[i]).is_ok());
            }
        }
        memory_manager.set_errno(0);
        handler_calls = 0;
        errno_in_handler = 0;
        cpu.gpr.rax = 0xDEADBEEF;
        ExportEntry* entry = find_export(exports, name);
        ASSERT_NE(entry, nullptr) << name;
        entry->handler(marshaler);
    }

    [[nodiscard]] u32 ret() const { return static_cast<u32>(cpu.gpr.rax); }

    GuestMemory memory;
    capsule::Vfs vfs;
    EnvironmentManager environment{"test.exe", memory};
    CrtMemoryManager memory_manager{memory, 0x20000000, 0x10000};
    CrtEnvironmentManager manager{environment, memory, memory_manager};
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
    unsigned handler_calls = 0;
    u32 errno_in_handler = 0;

private:
    bool m_x64;
};

} // namespace

TEST(CrtDupenvTest, DupenvSReturnsCrtHeapCopyCaseInsensitiveBothAbis) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        DupenvHarness h(x64);
        ASSERT_TRUE(h.marshaler.write_ansi_string(h.kName, "VITREODUPENV").is_ok());
        h.call("_dupenv_s", h.kBufferOut, h.kCountOut, h.kName);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.handler_calls, 0u);
        const GuestAddr copy = static_cast<GuestAddr>(h.get_ptr(h.kBufferOut));
        ASSERT_NE(copy, 0u);
        EXPECT_EQ(read_ascii(h.memory, copy), "launcher");
        EXPECT_EQ(h.get_ptr(h.kCountOut), 9u);  // "launcher" + terminator
        // The copy is a block of the CRT heap the guest's free() releases.
        EXPECT_EQ(h.memory_manager.crt_block_size(copy), 9u);
        // Modifying the copy does not affect the environment.
        ASSERT_TRUE(h.memory.write_u8(copy, 'X').is_ok());
        EXPECT_EQ(h.manager.value("VitreoDupenv").value(), "launcher");
        h.memory_manager.crt_free(copy);

        // A second call yields an independent buffer.
        h.call("_dupenv_s", h.kBufferOut, h.kCountOut, h.kName);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(read_ascii(h.memory,
                             static_cast<GuestAddr>(h.get_ptr(h.kBufferOut))),
                  "launcher");
    }
}

TEST(CrtDupenvTest, DupenvSNotFoundIsNullZeroAndSuccess) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        DupenvHarness h(x64);
        ASSERT_TRUE(h.marshaler.write_ansi_string(h.kName, "NoSuchVariableHere").is_ok());
        h.call("_dupenv_s", h.kBufferOut, h.kCountOut, h.kName);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.get_ptr(h.kBufferOut), 0u);
        EXPECT_EQ(h.get_ptr(h.kCountOut), 0u);
        EXPECT_EQ(h.handler_calls, 0u);
        EXPECT_EQ(h.memory_manager.get_errno(), 0u);

        ASSERT_TRUE(h.marshaler.write_wide_string(h.kName, "NoSuchVariableHere").is_ok());
        h.call("_wdupenv_s", h.kBufferOut, h.kCountOut, h.kName);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.get_ptr(h.kBufferOut), 0u);
        EXPECT_EQ(h.get_ptr(h.kCountOut), 0u);
    }
}

TEST(CrtDupenvTest, DupenvSNumberOfElementsMayBeNull) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        DupenvHarness h(x64);
        ASSERT_TRUE(h.marshaler.write_ansi_string(h.kName, "vitreodupenv").is_ok());
        h.call("_dupenv_s", h.kBufferOut, 0, h.kName);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.handler_calls, 0u);
        EXPECT_EQ(read_ascii(h.memory,
                             static_cast<GuestAddr>(h.get_ptr(h.kBufferOut))),
                  "launcher");
        EXPECT_EQ(h.get_ptr(h.kCountOut), x64 ? 0x5A5A5A5A5A5A5A5Aull
                                              : 0x5A5A5A5Aull);
    }
}

TEST(CrtDupenvTest, DupenvSNullBufferOrVarnameInvokesHandlerAndReturnsEinval) {
    for (const char* name : {"_dupenv_s", "_wdupenv_s"}) {
        for (bool x64 : {false, true}) {
            SCOPED_TRACE(std::string(name) + (x64 ? " x64" : " x86"));
            DupenvHarness h(x64);
            ASSERT_TRUE(h.marshaler.write_ansi_string(h.kName, "VitreoDupenv").is_ok());

            h.call(name, 0, h.kCountOut, h.kName);       // buffer NULL
            EXPECT_EQ(h.ret(), DupenvHarness::kEinval);
            EXPECT_EQ(h.memory_manager.get_errno(), DupenvHarness::kEinval);
            EXPECT_EQ(h.handler_calls, 1u);
            EXPECT_EQ(h.errno_in_handler, DupenvHarness::kEinval);

            h.call(name, h.kBufferOut, h.kCountOut, 0);  // varname NULL
            EXPECT_EQ(h.ret(), DupenvHarness::kEinval);
            EXPECT_EQ(h.memory_manager.get_errno(), DupenvHarness::kEinval);
            EXPECT_EQ(h.handler_calls, 1u);
            // No allocation is handed out on failure.
            EXPECT_EQ(h.get_ptr(h.kBufferOut), 0u);
            EXPECT_EQ(h.get_ptr(h.kCountOut), 0u);
        }
    }
}

TEST(CrtDupenvTest, WdupenvSCopiesUtf16AndCountsWideElements) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        DupenvHarness h(x64);
        ASSERT_TRUE(h.marshaler.write_wide_string(h.kName, "vitreowidedupenv").is_ok());
        h.call("_wdupenv_s", h.kBufferOut, h.kCountOut, h.kName);
        EXPECT_EQ(h.ret(), 0u);
        const GuestAddr copy = static_cast<GuestAddr>(h.get_ptr(h.kBufferOut));
        ASSERT_NE(copy, 0u);
        // "snow" + U+2603 = 5 UTF-16 units, + terminator = 6 elements.
        EXPECT_EQ(h.get_ptr(h.kCountOut), 6u);
        EXPECT_EQ(h.memory_manager.crt_block_size(copy), 12u);
        EXPECT_EQ(h.memory.read_u16(copy + 8).value(), 0x2603u);
        EXPECT_EQ(h.memory.read_u16(copy + 10).value(), 0u);
        EXPECT_EQ(h.marshaler.read_wide_string(copy).value(), "snow\xE2\x98\x83");
    }
}

TEST(CrtDupenvTest, DupenvSEmptyValueIsFoundAsEmptyString) {
    // An empty value cannot be stored through _putenv (it removes the
    // variable), but the process environment can hold one
    // (SetEnvironmentVariable with ""); it is FOUND, so the copy is "" with
    // one element, not the not-found NULL / 0.
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        DupenvHarness h(x64);
        h.environment.set_variable("VitreoEmpty", "");
        ASSERT_TRUE(h.manager.value("VitreoEmpty").has_value());
        ASSERT_TRUE(h.marshaler.write_ansi_string(h.kName, "VitreoEmpty").is_ok());
        h.call("_dupenv_s", h.kBufferOut, h.kCountOut, h.kName);
        EXPECT_EQ(h.ret(), 0u);
        const GuestAddr copy = static_cast<GuestAddr>(h.get_ptr(h.kBufferOut));
        ASSERT_NE(copy, 0u);
        EXPECT_EQ(h.memory.read_u8(copy).value(), 0u);
        EXPECT_EQ(h.get_ptr(h.kCountOut), 1u);
    }
}

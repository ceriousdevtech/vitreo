/// @file test_crt_string.cpp
/// @brief Unit tests for msvcrt string and I/O functions (WIN32-068)

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "capsule/vfs/vfs.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/msvcrt/runtime/crt_data.hpp"
#include "winapi/msvcrt/math/crt_math.hpp"
#include "winapi/msvcrt/memory/crt_memory.hpp"
#include "winapi/msvcrt/io/crt_stdio.hpp"
#include "winapi/msvcrt/io/crt_printf.hpp"
#include "winapi/msvcrt/text/crt_string.hpp"
#include "winapi/kernel32/error.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

class CrtStringTest : public ::testing::Test {};

namespace {
#include "../../oracle_data/crt_itoa_inputs.h"
#include "crt_itoa_oracle.inc"
}

TEST_F(CrtStringTest, SecureIntegerConversionMatchesWindowsBothAbis) {
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager allocation(memory,0x20000000,0x10000);
    CrtStringManager strings;
    const auto exports=create_crt_string_thunks(strings,allocation,vfs);
    unsigned calls=0,seen=0;
    allocation.exchange_invalid_parameter_handler(0x1234);
    allocation.set_guest_caller([&](GuestAddr fn,const u64* args,usize count)->u64 {
        EXPECT_EQ(fn,0x1234u);EXPECT_EQ(count,5u);
        for(usize i=0;i<count;++i)EXPECT_EQ(args[i],0u);
        ++calls;seen=allocation.get_errno();return 0;
    });
    for(bool x64:{false,true}) {
        cpu::CpuState cpu;
        if(x64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.set_sp(0x20004000);
        Marshaler m(memory);m.attach_cpu(cpu);
        for(const auto& row:kItoaOracle) {
            const auto& f=row.fields;
            SCOPED_TRACE(std::to_string(x64)+":"+std::to_string(f[0])+":"+std::to_string(f[1])+":"+std::to_string(f[2])+":"+std::to_string(f[3]));
            const bool wide=f[0]!=0;
            const GuestAddr buffer=0x20002000;
            for(unsigned i=0;i<70;++i)ASSERT_TRUE(memory.write_u8(buffer+i,0xcc).is_ok());
            const bool null_buffer=f[3]==std::size(itoa_sizes);
            const u32 capacity=null_buffer?34:itoa_sizes[f[3]];
            const u32 args[]={itoa_values[f[1]],null_buffer?0u:static_cast<u32>(buffer),capacity,f[2]};
            if(x64){cpu.gpr.rcx=args[0];cpu.gpr.rdx=args[1];cpu.gpr.r8=args[2];cpu.gpr.r9=args[3];}
            else for(unsigned i=0;i<4;++i)ASSERT_TRUE(memory.write_u32(cpu.sp()+4+i*4,args[i]).is_ok());
            allocation.set_errno(0x1234);calls=seen=0;set_last_error(0xdead);
            auto found=std::find_if(exports.entries.begin(),exports.entries.end(),[&](const ExportEntry& e){return e.name==(wide?"_itow_s":"_itoa_s");});
            ASSERT_NE(found,exports.entries.end());EXPECT_EQ(found->stdcall_bytes,0u);
            found->handler(m);
            EXPECT_EQ(static_cast<u32>(cpu.gpr.rax),f[4]);EXPECT_EQ(allocation.get_errno(),f[5]);
            EXPECT_EQ(calls,f[6]);EXPECT_EQ(seen,f[7]);EXPECT_EQ(get_last_error(),f[8]);
            for(unsigned i=0;i<35;++i){
                unsigned expected=0;
                for(unsigned digit=0;digit<4;++digit){const char c=row.output[i*4+digit];expected=expected*16+static_cast<unsigned>(c<='9'?c-'0':c-'a'+10);}
                const u32 actual=wide?memory.read_u16(buffer+i*2).value():memory.read_u8(buffer+i).value();
                EXPECT_EQ(actual,expected)<<"buffer unit "<<i;
            }
        }
    }
}

// =============================================================================
// 1. Stricmp_EqualCaseInsensitive
// =============================================================================

TEST_F(CrtStringTest, Stricmp_EqualCaseInsensitive) {
    EXPECT_EQ(CrtStringManager::stricmp("Hello", "hello"), 0);
    EXPECT_EQ(CrtStringManager::stricmp("ABC", "abc"), 0);
}

// =============================================================================
// 2. Stricmp_LessThan
// =============================================================================

TEST_F(CrtStringTest, Stricmp_LessThan) {
    EXPECT_LT(CrtStringManager::stricmp("abc", "abd"), 0);
}

// =============================================================================
// 3. Wcsicmp_EqualCaseInsensitive
// =============================================================================

TEST_F(CrtStringTest, Wcsicmp_EqualCaseInsensitive) {
    EXPECT_EQ(CrtStringManager::wcsicmp(L"Test", L"test"), 0);
    EXPECT_EQ(CrtStringManager::wcsicmp(L"HELLO", L"hello"), 0);
}

// =============================================================================
// 4. Strdup_ReturnsIndependentCopy
// =============================================================================

TEST_F(CrtStringTest, Strdup_ReturnsIndependentCopy) {
    char original[] = "hello";
    char* dup = CrtStringManager::crt_strdup(original);
    ASSERT_NE(dup, nullptr);
    EXPECT_STREQ(dup, "hello");

    // Modify original, dup unchanged
    original[0] = 'X';
    EXPECT_STREQ(dup, "hello");
    EXPECT_EQ(original[0], 'X');

    ::free(dup);
}

// =============================================================================
// 5. Strdup_NullReturnsNull
// =============================================================================

TEST_F(CrtStringTest, Strdup_NullReturnsNull) {
    EXPECT_EQ(CrtStringManager::crt_strdup(nullptr), nullptr);
}

// =============================================================================
// 6. Wcsdup_ReturnsIndependentCopy
// =============================================================================

TEST_F(CrtStringTest, Wcsdup_ReturnsIndependentCopy) {
    wchar_t original[] = L"world";
    wchar_t* dup = CrtStringManager::crt_wcsdup(original);
    ASSERT_NE(dup, nullptr);
    EXPECT_STREQ(dup, L"world");

    // Modify original, dup unchanged
    original[0] = L'Z';
    EXPECT_STREQ(dup, L"world");

    ::free(dup);
}

// =============================================================================
// 7. Wtoi_ParsesInt
// =============================================================================

TEST_F(CrtStringTest, Wtoi_ParsesInt) {
    EXPECT_EQ(CrtStringManager::wtoi(L"42"), 42);
    EXPECT_EQ(CrtStringManager::wtoi(L"-7"), -7);
    EXPECT_EQ(CrtStringManager::wtoi(L"0"), 0);
}

// =============================================================================
// 8. Atoi64_LargeNumber
// =============================================================================

TEST_F(CrtStringTest, Atoi64_LargeNumber) {
    EXPECT_EQ(CrtStringManager::atoi64("9999999999"), 9999999999LL);
    EXPECT_EQ(CrtStringManager::atoi64("-9999999999"), -9999999999LL);
}

// =============================================================================
// 9. Snprintf_Truncates
// =============================================================================

TEST_F(CrtStringTest, Snprintf_Truncates) {
    char buf[4];
    // "hello" needs 6 bytes (including null), buffer is 4
    int result = CrtStringManager::crt_snprintf(buf, sizeof(buf), "%s", "hello");
    // MSVC _snprintf returns -1 on truncation
    EXPECT_EQ(result, -1);
}

// =============================================================================
// 10. Sprintf_WritesFormatted
// =============================================================================

TEST_F(CrtStringTest, Sprintf_WritesFormatted) {
    char buf[64];
    int result = CrtStringManager::crt_sprintf(buf, "num=%d str=%s", 42, "ok");
    EXPECT_GT(result, 0);
    EXPECT_STREQ(buf, "num=42 str=ok");
}

// =============================================================================
// 11. Sscanf_ParsesInput
// =============================================================================

TEST_F(CrtStringTest, Sscanf_ParsesInput) {
    int num = 0;
    char word[32] = {};
    int fields = CrtStringManager::crt_sscanf("42 hello", "%d %31s", &num, word);
    EXPECT_EQ(fields, 2);
    EXPECT_EQ(num, 42);
    EXPECT_STREQ(word, "hello");
}

// =============================================================================
// 12. Strnicmp_WithLimit
// =============================================================================

TEST_F(CrtStringTest, Strnicmp_WithLimit) {
    // First 3 chars match case-insensitively
    EXPECT_EQ(CrtStringManager::strnicmp("ABCDEF", "abcxyz", 3), 0);
    // Full comparison would differ
    EXPECT_NE(CrtStringManager::strnicmp("ABCDEF", "abcxyz", 6), 0);
}

// =============================================================================
// 13. Scprintf_ReturnsLength
// =============================================================================

TEST_F(CrtStringTest, Scprintf_ReturnsLength) {
    int len = CrtStringManager::crt_scprintf("%d", 12345);
    EXPECT_EQ(len, 5); // "12345" is 5 characters
}

// =============================================================================
// Additional: NullPointer safety
// =============================================================================

TEST_F(CrtStringTest, Stricmp_NullHandling) {
    EXPECT_LT(CrtStringManager::stricmp(nullptr, "abc"), 0);
    EXPECT_GT(CrtStringManager::stricmp("abc", nullptr), 0);
    EXPECT_EQ(CrtStringManager::stricmp(nullptr, nullptr), 0);
}

TEST_F(CrtStringTest, MemoryOps_Basic) {
    char buf[16];
    CrtStringManager::crt_memset(buf, 0xAA, sizeof(buf));
    EXPECT_EQ(static_cast<unsigned char>(buf[0]), 0xAA);
    EXPECT_EQ(static_cast<unsigned char>(buf[15]), 0xAA);

    char src[] = "test";
    CrtStringManager::crt_memcpy(buf, src, 5);
    EXPECT_EQ(CrtStringManager::crt_memcmp(buf, "test", 5), 0);

    // memmove with overlapping regions
    char overlap[] = "abcdef";
    CrtStringManager::crt_memmove(overlap + 2, overlap, 4);
    EXPECT_EQ(CrtStringManager::crt_memcmp(overlap + 2, "abcd", 4), 0);
}

TEST_F(CrtStringTest, Strlen_Basic) {
    EXPECT_EQ(CrtStringManager::crt_strlen("hello"), 5u);
    EXPECT_EQ(CrtStringManager::crt_strlen(""), 0u);
    EXPECT_EQ(CrtStringManager::crt_strlen(nullptr), 0u);
}

TEST_F(CrtStringTest, Wcslen_Basic) {
    EXPECT_EQ(CrtStringManager::crt_wcslen(L"hello"), 5u);
    EXPECT_EQ(CrtStringManager::crt_wcslen(L""), 0u);
    EXPECT_EQ(CrtStringManager::crt_wcslen(nullptr), 0u);
}

TEST_F(CrtStringTest, FullpathPreservesAbsoluteOpticalDrive) {
    auto result = CrtStringManager::resolve_fullpath(
        "D:\\INSTALL.EXE", "C:\\Program Files\\Starcraft");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "D:\\INSTALL.EXE");
}

TEST_F(CrtStringTest, FullpathResolvesRelativeComponentsAgainstCurrentDirectory) {
    auto result = CrtStringManager::resolve_fullpath(
        "..\\Data\\.\\file.bin", "C:\\Program Files\\Starcraft");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "C:\\Program Files\\Data\\file.bin");
}

TEST_F(CrtStringTest, FullpathUsesCurrentDriveForRootRelativePath) {
    auto result = CrtStringManager::resolve_fullpath(
        "\\font\\font.gid", "D:\\Game");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, "D:\\font\\font.gid");
}

TEST_F(CrtStringTest, FullpathRejectsUnsupportedUncPath) {
    EXPECT_FALSE(CrtStringManager::resolve_fullpath(
        "\\\\server\\share\\file", "C:\\").has_value());
}

TEST_F(CrtStringTest, SharedExportsContainCrtdllStringContracts) {
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    DllExports exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);
    // vsprintf is registered with the rest of the printf family.
    for (auto& e : create_crt_printf_thunks(memory_manager).entries) {
        exports.entries.push_back(std::move(e));
    }

    constexpr std::array<const char*, 6> required = {
        "setlocale", "strpbrk", "strtol", "strtoul", "vsprintf", "wcstombs",
    };
    for (const char* name : required) {
        EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(),
                               [name](const ExportEntry& entry) {
                                   return entry.name == name;
                               }),
                  exports.entries.end())
            << name;
    }
}

TEST_F(CrtStringTest, ObservedBattleNetStringExportsHaveRealHandlers) {
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    const DllExports exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);

    constexpr std::array<const char*, 3> required = {
        "strcspn", "strcpy_s", "strncpy_s",
    };
    for (const char* name : required) {
        EXPECT_NE(std::find_if(exports.entries.begin(), exports.entries.end(),
                               [name](const ExportEntry& entry) {
                                   return entry.name == name;
                               }),
                  exports.entries.end())
            << name;
    }
}

TEST_F(CrtStringTest, SecureCopiesAndStrcspnMatchCrtContracts) {
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    DllExports exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    const auto find_entry = [&exports](const char* name) -> ExportEntry* {
        auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        return entry == exports.entries.end() ? nullptr : &*entry;
    };

    constexpr GuestAddr destination = base;
    constexpr GuestAddr source = base + 0x100;
    constexpr GuestAddr control = base + 0x200;
    ASSERT_TRUE(marshaler.write_ansi_string(source, "launcher").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(control, "ch").is_ok());

    ExportEntry* strcpy_s = find_entry("strcpy_s");
    ASSERT_NE(strcpy_s, nullptr);
    cpu.gpr.rcx = destination;
    cpu.gpr.rdx = 32;
    cpu.gpr.r8 = source;
    strcpy_s->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(marshaler.read_ansi_string(destination).value(), "launcher");

    ExportEntry* strcspn = find_entry("strcspn");
    ASSERT_NE(strcspn, nullptr);
    cpu.gpr.rcx = source;
    cpu.gpr.rdx = control;
    strcspn->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 4u);

    ExportEntry* strncpy_s = find_entry("strncpy_s");
    ASSERT_NE(strncpy_s, nullptr);
    cpu.gpr.rcx = destination;
    cpu.gpr.rdx = 5;
    cpu.gpr.r8 = source;
    cpu.gpr.r9 = std::numeric_limits<u64>::max();
    strncpy_s->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 80u);
    EXPECT_EQ(marshaler.read_ansi_string(destination).value(), "laun");
}

TEST_F(CrtStringTest, WcslwrLowercasesInPlaceAndReturnsSamePointer) {
    // Learn: _wcslwr converts in place and returns the input pointer. A
    // missing-export stub returning 0xFFFFFFFF is treated as a wchar_t*.
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x1000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    DllExports exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    const auto find_entry = [&exports](const char* name) -> ExportEntry* {
        auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        return entry == exports.entries.end() ? nullptr : &*entry;
    };

    ExportEntry* wcslwr = find_entry("_wcslwr");
    ASSERT_NE(wcslwr, nullptr);
    constexpr GuestAddr str = base;
    ASSERT_TRUE(marshaler.write_wide_string(str, "Game\\Mods").is_ok());
    cpu.gpr.rcx = str;
    wcslwr->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, str);
    EXPECT_NE(cpu.gpr.rax, 0xFFFFFFFFu);
    EXPECT_EQ(marshaler.read_wide_string(str).value(), "game\\mods");
}

TEST_F(CrtStringTest, Msvcr80ExportsHandlersFstatCiSinAndWcscpyS) {
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtDataBlock crt_data;
    ASSERT_TRUE(crt_data.initialize(memory));
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    CrtMathManager math_manager;
    CrtStdioManager stdio_manager;

    DllExports memory_exports = create_crt_memory_thunks(memory_manager);
    DllExports math_exports = create_crt_math_thunks(math_manager, memory_manager);
    DllExports stdio_exports =
        create_crt_stdio_thunks(stdio_manager, vfs, memory_manager, crt_data);
    DllExports string_exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);

    const auto find_entry = [](DllExports& exports, const char* name)
        -> ExportEntry* {
        auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        return entry == exports.entries.end() ? nullptr : &*entry;
    };

    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    // Handlers return previous=0 the first time.
    ExportEntry* set_pure = find_entry(memory_exports, "_set_purecall_handler");
    ExportEntry* set_inv =
        find_entry(memory_exports, "_set_invalid_parameter_handler");
    ExportEntry* set_thread_inv = find_entry(
        memory_exports, "_set_thread_local_invalid_parameter_handler");
    ASSERT_NE(set_pure, nullptr);
    ASSERT_NE(set_inv, nullptr);
    ASSERT_NE(set_thread_inv, nullptr);
    cpu.gpr.rcx = 0x11110000;
    set_pure->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    cpu.gpr.rcx = 0x22220000;
    set_inv->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    cpu.gpr.rcx = 0x33330000;
    set_thread_inv->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    cpu.gpr.rcx = 0x44440000;
    set_thread_inv->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0x33330000u);

    // _fstat64i32 is registered.
    EXPECT_NE(find_entry(stdio_exports, "_fstat64i32"), nullptr);

    // CI_UNARY includes _CIsin.
    EXPECT_NE(find_entry(math_exports, "_CIsin"), nullptr);

    // wcscpy_s copies.
    ExportEntry* wcscpy_s = find_entry(string_exports, "wcscpy_s");
    ASSERT_NE(wcscpy_s, nullptr);
    constexpr GuestAddr dest = base;
    constexpr GuestAddr src = base + 0x200;
    ASSERT_TRUE(marshaler.write_wide_string(src, "hello").is_ok());
    cpu.gpr.rcx = dest;
    cpu.gpr.rdx = 16;
    cpu.gpr.r8 = src;
    wcscpy_s->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(marshaler.read_wide_string(dest).value(), "hello");
}


// =============================================================================
// sprintf must honour the MSVC size prefixes. TS3 formats 64-bit resource
// instance keys with "%I64x"; a parser that drops the prefix emits the literal
// format string, and every lookup keyed on that text then misses.
// =============================================================================

TEST_F(CrtStringTest, Sprintf_FormatsMsvcI64Prefix) {
    constexpr GuestAddr base = 0x10000;
    constexpr GuestAddr stack = base;
    constexpr GuestAddr destination = base + 0x200;
    constexpr GuestAddr format = base + 0x300;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    (void)string_manager;
    (void)vfs;
    // sprintf is msvcrt's, on the shared printf processor.
    DllExports exports = create_crt_printf_thunks(memory_manager);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "sprintf";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(format, "%08x!%I64x").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, destination).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, format).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 0x2f7d0004u).is_ok());
    // The __int64 argument occupies two x86 stack slots, low half first.
    ASSERT_TRUE(memory.write_u32(stack + 16, 0x7422dd4fu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, 0x66926873u).is_ok());

    entry->handler(marshaler);

    auto written = marshaler.read_ansi_string(destination);
    ASSERT_TRUE(written.is_ok());
    EXPECT_EQ(written.value(), "2f7d0004!669268737422dd4f");
    EXPECT_EQ(cpu.gpr.rax, written.value().size());
}

// =============================================================================
// sscanf must honour the same MSVC size prefixes. TS3's layout XML carries
// resource keys as "type:group:instance" with a 64-bit instance, parsed with
// "%I64x"; dropping the prefix left 'I' as the conversion and stored nothing.
// =============================================================================

TEST_F(CrtStringTest, Sscanf_ParsesMsvcI64Prefix) {
    constexpr GuestAddr base = 0x10000;
    constexpr GuestAddr stack = base;
    constexpr GuestAddr input = base + 0x200;
    constexpr GuestAddr format = base + 0x300;
    constexpr GuestAddr out_type = base + 0x400;
    constexpr GuestAddr out_group = base + 0x410;
    constexpr GuestAddr out_instance = base + 0x420;

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    CrtMemoryManager memory_manager(memory);
    CrtStdioManager stdio_manager;
    CrtDataBlock crt_data;
    DllExports exports;
    add_crt_scanf_exports(exports, stdio_manager, memory_manager, crt_data);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "sscanf";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(
        input, "2f7d0004:00000000:669268737422dd4f").is_ok());
    ASSERT_TRUE(marshaler.write_ansi_string(
        format, "%x:%x:%I64x").is_ok());
    ASSERT_TRUE(memory.write_u64(out_instance, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, input).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, format).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, out_type).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 16, out_group).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 20, out_instance).is_ok());

    entry->handler(marshaler);

    EXPECT_EQ(cpu.gpr.rax, 3u);
    EXPECT_EQ(memory.read_u32(out_type).value_or(0), 0x2f7d0004u);
    EXPECT_EQ(memory.read_u32(out_group).value_or(1), 0u);
    EXPECT_EQ(memory.read_u64(out_instance).value_or(0),
              0x669268737422dd4full);
}

TEST_F(CrtStringTest, StrrevReversesInPlace) {
    constexpr GuestAddr base = 0x00400000;
    constexpr GuestAddr stack = base + 0x200;
    constexpr GuestAddr str = base + 0x300;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    DllExports exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "_strrev";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(marshaler.write_ansi_string(str, "abc").is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 4, str).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, str);
    EXPECT_EQ(marshaler.read_ansi_string(str).value_or(""), "cba");
}

TEST_F(CrtStringTest, UltoaWritesUnsignedDecimal) {
    constexpr GuestAddr base = 0x00400000;
    constexpr GuestAddr stack = base + 0x200;
    constexpr GuestAddr str = base + 0x300;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    DllExports exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);
    auto entry = std::find_if(
        exports.entries.begin(), exports.entries.end(),
        [](const ExportEntry& candidate) {
            return candidate.name == "_ultoa";
        });
    ASSERT_NE(entry, exports.entries.end());

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.gpr.rsp = stack;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);
    ASSERT_TRUE(memory.write_u32(stack + 4, 0xFFFFFFFFu).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 8, str).is_ok());
    ASSERT_TRUE(memory.write_u32(stack + 12, 10).is_ok());
    entry->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, str);
    EXPECT_EQ(marshaler.read_ansi_string(str).value_or(""), "4294967295");
}

// wcstombs_s reports the byte count *including* the terminator, and refuses a
// buffer that is too small (ERANGE) instead of truncating — unless the caller
// asked for truncation with _TRUNCATE. btowc and __strncnt round out the
// conversion set battle.net.dll imports.
TEST_F(CrtStringTest, ConversionContractsMatchTheCrt) {
    constexpr GuestAddr base = 0x10000;
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        base, 0x1000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtMemoryManager memory_manager(memory);
    CrtStringManager string_manager;
    DllExports exports =
        create_crt_string_thunks(string_manager, memory_manager, vfs);
    cpu::CpuState cpu;
    cpu.init_long_mode();
    // The fifth argument lives on the stack, so RSP must point at real memory.
    cpu.gpr.rsp = base + 0x400;
    Marshaler marshaler{memory};
    marshaler.attach_cpu(cpu);

    const auto find_entry = [&exports](const char* name) -> ExportEntry* {
        auto entry = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& candidate) {
                return candidate.name == name;
            });
        return entry == exports.entries.end() ? nullptr : &*entry;
    };

    constexpr GuestAddr wide = base;          // L"abc"
    constexpr GuestAddr narrow = base + 0x100;
    constexpr GuestAddr converted = base + 0x200;
    for (u32 i = 0; i < 3; ++i) {
        ASSERT_TRUE(memory.write_u16(wide + i * 2,
                                     static_cast<u16>('a' + i)).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(wide + 6, 0).is_ok());

    ExportEntry* wcstombs_s = find_entry("wcstombs_s");
    ASSERT_NE(wcstombs_s, nullptr);

    // Fits: 3 characters plus the terminator.
    cpu.gpr.rcx = converted;
    cpu.gpr.rdx = narrow;
    cpu.gpr.r8 = 16;
    cpu.gpr.r9 = wide;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28,
                                 static_cast<u64>(-1)).is_ok());  // _TRUNCATE
    wcstombs_s->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(converted).value(), 4u);
    EXPECT_EQ(marshaler.read_ansi_string(narrow).value(), "abc");

    // Too small without _TRUNCATE: ERANGE, and the buffer is emptied.
    cpu.gpr.r8 = 2;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28, 3).is_ok());
    wcstombs_s->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 34u);   // ERANGE
    EXPECT_EQ(memory.read_u8(narrow).value(), 0u);
    EXPECT_EQ(memory.read_u64(converted).value(), 0u);

    // Size query: NULL buffer with zero size counts without writing.
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = 0;
    ASSERT_TRUE(memory.write_u64(cpu.gpr.rsp + 0x28,
                                 static_cast<u64>(-1)).is_ok());
    wcstombs_s->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0u);
    EXPECT_EQ(memory.read_u64(converted).value(), 4u);

    ExportEntry* btowc = find_entry("btowc");
    ASSERT_NE(btowc, nullptr);
    cpu.gpr.rcx = static_cast<u64>('Q');
    btowc->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, static_cast<u64>('Q'));
    cpu.gpr.rcx = static_cast<u64>(static_cast<u32>(-1));   // EOF
    btowc->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 0xFFFFu);                        // WEOF

    ExportEntry* strncnt = find_entry("__strncnt");
    ASSERT_NE(strncnt, nullptr);
    ASSERT_TRUE(marshaler.write_ansi_string(narrow, "battle").is_ok());
    cpu.gpr.rcx = narrow;
    cpu.gpr.rdx = 16;
    strncnt->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 6u);
    cpu.gpr.rdx = 3;          // bounded below the terminator
    strncnt->handler(marshaler);
    EXPECT_EQ(cpu.gpr.rax, 3u);
}

// Learn: mbtowc / wctomb. Single-byte "C" locale. Expected values are the
// documented return table, not a Vitreo run.
TEST_F(CrtStringTest, MbtowcAndWctombFollowCLocaleContract) {
    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager allocation(memory, 0x20000000, 0x10000);
    CrtStringManager strings;
    const auto exports = create_crt_string_thunks(strings, allocation, vfs);
    auto find_entry = [&](const char* name) -> const ExportEntry* {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    const ExportEntry* mbtowc = find_entry("mbtowc");
    const ExportEntry* wctomb = find_entry("wctomb");
    ASSERT_NE(mbtowc, nullptr);
    ASSERT_NE(wctomb, nullptr);
    EXPECT_EQ(mbtowc->stdcall_bytes, 0u);
    EXPECT_EQ(wctomb->stdcall_bytes, 0u);

    for (bool x64 : {false, true}) {
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode();
        else cpu.init_protected_mode();
        cpu.set_sp(0x20004000);
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        const GuestAddr wide = 0x20001000;
        const GuestAddr narrow = 0x20002000;

        auto call3 = [&](const ExportEntry* entry, u64 a, u64 b, u64 c) {
            if (x64) {
                cpu.gpr.rcx = a;
                cpu.gpr.rdx = b;
                cpu.gpr.r8 = c;
            } else {
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(a)).is_ok());
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, static_cast<u32>(b)).is_ok());
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, static_cast<u32>(c)).is_ok());
            }
            entry->handler(marshaler);
        };
        auto call2 = [&](const ExportEntry* entry, u64 a, u64 b) {
            if (x64) {
                cpu.gpr.rcx = a;
                cpu.gpr.rdx = b;
            } else {
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>(a)).is_ok());
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, static_cast<u32>(b)).is_ok());
            }
            entry->handler(marshaler);
        };

        ASSERT_TRUE(memory.write_u8(narrow, static_cast<u8>('A')).is_ok());
        ASSERT_TRUE(memory.write_u16(wide, 0xABCD).is_ok());
        call3(mbtowc, wide, narrow, 1);
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 1);
        EXPECT_EQ(memory.read_u16(wide).value(), static_cast<u16>('A'));

        call3(mbtowc, 0, narrow, 1);
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 1);

        ASSERT_TRUE(memory.write_u8(narrow, 0).is_ok());
        call3(mbtowc, wide, narrow, 1);
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 0);
        EXPECT_EQ(memory.read_u16(wide).value(), 0u);

        call3(mbtowc, wide, 0, 1);
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 0);

        ASSERT_TRUE(memory.write_u8(narrow, static_cast<u8>('Z')).is_ok());
        call3(mbtowc, wide, narrow, 0);
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), -1);

        allocation.set_errno(0);
        call2(wctomb, narrow, static_cast<u64>('Q'));
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 1);
        EXPECT_EQ(memory.read_u8(narrow).value(), static_cast<u8>('Q'));
        EXPECT_EQ(allocation.get_errno(), 0u);

        call2(wctomb, narrow, 0);
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 1);
        EXPECT_EQ(memory.read_u8(narrow).value(), 0u);

        call2(wctomb, 0, static_cast<u64>('Q'));
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 0);

        allocation.set_errno(0);
        call2(wctomb, narrow, 0x20AC);  // euro sign, not a single byte
        EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), -1);
        EXPECT_EQ(allocation.get_errno(), 42u);  // EILSEQ
    }
}

// Microsoft STL streambuf put pointers and ostream::operator<<(int).
TEST_F(CrtStringTest, Msvcp80SputcAndInserterUseBufferPointers) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x20000000, 0x10000, MemoryProtection::ReadWrite).is_ok());
    capsule::Vfs vfs;
    CrtMemoryManager allocation(memory, 0x20000000, 0x10000);
    const auto exports = create_msvcp80_thunks(allocation);
    auto find_entry = [&](const char* name) -> const ExportEntry* {
        for (const auto& entry : exports.entries) {
            if (entry.name == name) return &entry;
        }
        return nullptr;
    };
    const ExportEntry* sputc = find_entry("?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QAEHD@Z");
    const ExportEntry* insert = find_entry("??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QAEAAV01@H@Z");
    ASSERT_NE(sputc, nullptr);
    ASSERT_NE(insert, nullptr);

    cpu::CpuState cpu;
    cpu.init_protected_mode();
    cpu.set_sp(0x20004000);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr sb = 0x20001000;
    constexpr GuestAddr buf = 0x20001200;
    constexpr GuestAddr ios = 0x20001400;
    constexpr GuestAddr stream = 0x20001600;
    constexpr GuestAddr vbtable = 0x20001800;
    ASSERT_TRUE(memory.write_u8(buf, 0).is_ok());
    ASSERT_TRUE(memory.write_u8(buf + 1, 0).is_ok());
    // _Pnext at +24, _Pcount at +40, _IPnext at +32, _IPcount at +48.
    ASSERT_TRUE(memory.write_u32(sb + 24, static_cast<u32>(buf)).is_ok());
    ASSERT_TRUE(memory.write_u32(sb + 40, 8).is_ok());
    ASSERT_TRUE(memory.write_u32(sb + 32, static_cast<u32>(sb + 24)).is_ok());
    ASSERT_TRUE(memory.write_u32(sb + 48, static_cast<u32>(sb + 40)).is_ok());

    cpu.gpr.rcx = sb;
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, static_cast<u32>('Z')).is_ok());
    sputc->handler(marshaler);
    EXPECT_EQ(static_cast<i32>(cpu.gpr.rax), 'Z');
    EXPECT_EQ(memory.read_u8(buf).value(), static_cast<u8>('Z'));

    // ios: state 0, dec flag, width 0, rdbuf, fill ' '.
    ASSERT_TRUE(memory.write_u32(ios + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(ios + 12, 0x200).is_ok());
    ASSERT_TRUE(memory.write_u32(ios + 48, static_cast<u32>(sb)).is_ok());
    ASSERT_TRUE(memory.write_u8(ios + 56, static_cast<u8>(' ')).is_ok());
    // vbtable[1] = ios - (stream + 4)
    const i32 disp = static_cast<i32>(ios - (stream + 4));
    ASSERT_TRUE(memory.write_u32(vbtable + 4, static_cast<u32>(disp)).is_ok());
    ASSERT_TRUE(memory.write_u32(stream + 4, static_cast<u32>(vbtable)).is_ok());

    cpu.gpr.rcx = stream;
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 42).is_ok());
    insert->handler(marshaler);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), static_cast<u32>(stream));
    EXPECT_EQ(memory.read_u8(buf + 1).value(), static_cast<u8>('4'));
    EXPECT_EQ(memory.read_u8(buf + 2).value(), static_cast<u8>('2'));
}

// =============================================================================
// _splitpath_s / _wsplitpath_s  (Learn: splitpath-s-wsplitpath-s)
//
// Expected values come from the Learn page: drive = letter + ':', dir keeps
// its trailing '\' or '/', fname has no extension, ext keeps its leading '.';
// a missing component is ""; EINVAL (handler, errno, return) for path NULL
// and every buffer/size mismatch; ERANGE (errno, return, every buffer
// cleared to "") when a buffer is too short. Run for both ABIs: the sizes are
// size_t, so x86 passes 4-byte stack slots and x64 8-byte ones.
// =============================================================================

namespace {

class SplitpathSHarness {
public:
    static constexpr GuestAddr kBase = 0x40000000;
    static constexpr GuestAddr kPath = kBase + 0x2000;
    static constexpr std::array<GuestAddr, 4> kOut = {
        kBase + 0x1000, kBase + 0x1100, kBase + 0x1300, kBase + 0x1500};
    static constexpr u32 kEinval = 22;
    static constexpr u32 kErange = 34;

    explicit SplitpathSHarness(bool x64) : m_x64(x64) {
        EXPECT_TRUE(memory.map_region(kBase, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        exports = create_crt_string_thunks(strings, allocation, vfs);
        allocation.exchange_invalid_parameter_handler(0x1234);
        allocation.set_guest_caller(
            [this](GuestAddr fn, const u64*, usize count) -> u64 {
                EXPECT_EQ(fn, 0x1234u);
                EXPECT_EQ(count, 5u);
                ++handler_calls;
                errno_in_handler = allocation.get_errno();
                return 0;
            });
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(kBase + 0x8000);
        marshaler.attach_cpu(cpu);
    }

    /// Pre-fill every output buffer with a non-zero pattern.
    void poison() {
        for (GuestAddr out : kOut) {
            for (u32 i = 0; i < 0x100; ++i) {
                ASSERT_TRUE(memory.write_u8(out + i, 0xAB).is_ok());
            }
        }
    }

    void call(const char* name, const std::array<u64, 9>& args) {
        if (m_x64) {
            cpu.gpr.rcx = args[0];
            cpu.gpr.rdx = args[1];
            cpu.gpr.r8 = args[2];
            cpu.gpr.r9 = args[3];
            for (usize i = 4; i < 9; ++i) {
                ASSERT_TRUE(memory.write_u64(cpu.sp() + 0x28 + (i - 4) * 8,
                                             args[i]).is_ok());
            }
        } else {
            for (usize i = 0; i < 9; ++i) {
                ASSERT_TRUE(memory.write_u32(cpu.sp() + 4 + i * 4,
                                             static_cast<u32>(args[i])).is_ok());
            }
        }
        allocation.set_errno(0);
        handler_calls = 0;
        errno_in_handler = 0;
        cpu.gpr.rax = 0xDEADBEEF;
        auto found = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [name](const ExportEntry& e) { return e.name == name; });
        ASSERT_NE(found, exports.entries.end()) << name;
        found->handler(marshaler);
    }

    /// All four buffers, full sizes.
    void call_full(const char* name, GuestAddr path, u64 size = 0x100) {
        call(name, {path, kOut[0], size, kOut[1], size, kOut[2], size,
                    kOut[3], size});
    }

    [[nodiscard]] u32 ret() const { return static_cast<u32>(cpu.gpr.rax); }

    [[nodiscard]] std::string narrow(GuestAddr p) {
        std::string s;
        for (;;) {
            const u8 c = memory.read_u8(p + s.size()).value_or(0);
            if (c == 0) return s;
            s.push_back(static_cast<char>(c));
        }
    }
    [[nodiscard]] std::u16string wide(GuestAddr p) {
        std::u16string s;
        for (;;) {
            const u16 c = memory.read_u16(p + s.size() * 2).value_or(0);
            if (c == 0) return s;
            s.push_back(static_cast<char16_t>(c));
        }
    }
    void put_narrow(GuestAddr p, std::string_view s) {
        for (usize i = 0; i < s.size(); ++i) {
            ASSERT_TRUE(memory.write_u8(p + i, static_cast<u8>(s[i])).is_ok());
        }
        ASSERT_TRUE(memory.write_u8(p + s.size(), 0).is_ok());
    }
    void put_wide(GuestAddr p, std::u16string_view s) {
        for (usize i = 0; i < s.size(); ++i) {
            ASSERT_TRUE(memory.write_u16(p + i * 2, s[i]).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(p + s.size() * 2, 0).is_ok());
    }

    GuestMemory memory;
    capsule::Vfs vfs;
    CrtMemoryManager allocation{memory, 0x20000000, 0x10000};
    CrtStringManager strings;
    DllExports exports;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    unsigned handler_calls = 0;
    u32 errno_in_handler = 0;

private:
    bool m_x64;
};

} // namespace

TEST_F(CrtStringTest, SplitpathSSplitsEveryComponentBothAbis) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        SplitpathSHarness h(x64);
        h.put_narrow(h.kPath, "C:\\Program Files\\Game\\bin\\app.exe");
        h.poison();
        h.call_full("_splitpath_s", h.kPath);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.narrow(h.kOut[0]), "C:");
        EXPECT_EQ(h.narrow(h.kOut[1]), "\\Program Files\\Game\\bin\\");
        EXPECT_EQ(h.narrow(h.kOut[2]), "app");
        EXPECT_EQ(h.narrow(h.kOut[3]), ".exe");
        EXPECT_EQ(h.handler_calls, 0u);

        // Forward slashes, backslashes, or both; the extension is the part
        // from the last period of the file name.
        h.put_narrow(h.kPath, "d:/dir\\sub/x.tar.gz");
        h.call_full("_splitpath_s", h.kPath);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.narrow(h.kOut[0]), "d:");
        EXPECT_EQ(h.narrow(h.kOut[1]), "/dir\\sub/");
        EXPECT_EQ(h.narrow(h.kOut[2]), "x.tar");
        EXPECT_EQ(h.narrow(h.kOut[3]), ".gz");

        // A component the path does not contain is assigned "".
        h.poison();
        h.put_narrow(h.kPath, "noext");
        h.call_full("_splitpath_s", h.kPath);
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.narrow(h.kOut[0]), "");
        EXPECT_EQ(h.narrow(h.kOut[1]), "");
        EXPECT_EQ(h.narrow(h.kOut[2]), "noext");
        EXPECT_EQ(h.narrow(h.kOut[3]), "");

        // An empty path splits into four empty strings.
        h.poison();
        h.put_narrow(h.kPath, "");
        h.call_full("_splitpath_s", h.kPath);
        EXPECT_EQ(h.ret(), 0u);
        for (GuestAddr out : h.kOut) EXPECT_EQ(h.narrow(out), "");
    }
}

TEST_F(CrtStringTest, SplitpathSAcceptsNullComponentsWithZeroSize) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        SplitpathSHarness h(x64);
        h.put_narrow(h.kPath, "C:\\dir\\file.txt");
        h.poison();
        // Only fname requested.
        h.call("_splitpath_s", {h.kPath, 0, 0, 0, 0, h.kOut[2], 0x100, 0, 0});
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.narrow(h.kOut[2]), "file");
        EXPECT_EQ(h.memory.read_u8(h.kOut[0]).value(), 0xABu);
        EXPECT_EQ(h.handler_calls, 0u);
        // Nothing requested at all is valid too.
        h.call("_splitpath_s", {h.kPath, 0, 0, 0, 0, 0, 0, 0, 0});
        EXPECT_EQ(h.ret(), 0u);
    }
}

TEST_F(CrtStringTest, SplitpathSExactSizesFitAndMaxDriveIsThree) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        SplitpathSHarness h(x64);
        // "C:" + terminator = _MAX_DRIVE (3); every other buffer sized to its
        // component plus the terminator.
        h.put_narrow(h.kPath, "C:\\ab\\cde.fg");
        h.call("_splitpath_s", {h.kPath, h.kOut[0], 3, h.kOut[1], 5,
                                h.kOut[2], 4, h.kOut[3], 4});
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.narrow(h.kOut[0]), "C:");
        EXPECT_EQ(h.narrow(h.kOut[1]), "\\ab\\");
        EXPECT_EQ(h.narrow(h.kOut[2]), "cde");
        EXPECT_EQ(h.narrow(h.kOut[3]), ".fg");
    }
}

TEST_F(CrtStringTest, SplitpathSTooSmallBufferClearsAllAndReturnsErange) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        SplitpathSHarness h(x64);
        h.put_narrow(h.kPath, "C:\\ab\\cde.fg");
        // Each component one element short in turn.
        const std::array<std::array<u64, 4>, 4> sizes = {{
            {2, 5, 4, 4}, {3, 4, 4, 4}, {3, 5, 3, 4}, {3, 5, 4, 3}}};
        for (const auto& s : sizes) {
            SCOPED_TRACE(std::to_string(s[0]) + "," + std::to_string(s[1]) +
                         "," + std::to_string(s[2]) + "," +
                         std::to_string(s[3]));
            h.poison();
            h.call("_splitpath_s", {h.kPath, h.kOut[0], s[0], h.kOut[1], s[1],
                                    h.kOut[2], s[2], h.kOut[3], s[3]});
            EXPECT_EQ(h.ret(), SplitpathSHarness::kErange);
            EXPECT_EQ(h.allocation.get_errno(), SplitpathSHarness::kErange);
            EXPECT_EQ(h.handler_calls, 0u);
            for (GuestAddr out : h.kOut) EXPECT_EQ(h.narrow(out), "");
        }
    }
}

TEST_F(CrtStringTest, SplitpathSInvalidParametersInvokeHandlerAndReturnEinval) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        SplitpathSHarness h(x64);
        h.put_narrow(h.kPath, "C:\\dir\\file.txt");
        const std::array<u64, 9> good = {h.kPath, h.kOut[0], 0x100, h.kOut[1],
                                         0x100, h.kOut[2], 0x100, h.kOut[3],
                                         0x100};
        std::vector<std::array<u64, 9>> cases;
        auto path_null = good;
        path_null[0] = 0;
        cases.push_back(path_null);
        for (usize k = 0; k < 4; ++k) {
            auto null_with_size = good;   // buffer NULL, size non-zero
            null_with_size[1 + 2 * k] = 0;
            cases.push_back(null_with_size);
            auto zero_size = good;        // buffer non-NULL, size zero
            zero_size[2 + 2 * k] = 0;
            cases.push_back(zero_size);
        }
        for (usize c = 0; c < cases.size(); ++c) {
            SCOPED_TRACE("case " + std::to_string(c));
            h.poison();
            h.call("_splitpath_s", cases[c]);
            EXPECT_EQ(h.ret(), SplitpathSHarness::kEinval);
            EXPECT_EQ(h.allocation.get_errno(), SplitpathSHarness::kEinval);
            EXPECT_EQ(h.handler_calls, 1u);
            EXPECT_EQ(h.errno_in_handler, SplitpathSHarness::kEinval);
            // No partial result: every usable buffer is "" and an unusable
            // one (size 0) is untouched.
            for (usize k = 0; k < 4; ++k) {
                const bool usable = cases[c][1 + 2 * k] != 0 &&
                                    cases[c][2 + 2 * k] != 0;
                EXPECT_EQ(h.memory.read_u8(h.kOut[k]).value(),
                          usable ? 0u : 0xABu) << "buffer " << k;
            }
        }
    }
}

TEST_F(CrtStringTest, WsplitpathSSplitsUtf16UnitsAndReportsErrorsBothAbis) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        SplitpathSHarness h(x64);
        // Sizes are in wide characters: U+00E9 and U+4E2D are one unit each.
        h.put_wide(h.kPath, u"C:\\d\u00E9j\\f\u4E2D.txt");
        h.call("_wsplitpath_s", {h.kPath, h.kOut[0], 3, h.kOut[1], 6,
                                 h.kOut[2], 3, h.kOut[3], 5});
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.wide(h.kOut[0]), u"C:");
        EXPECT_EQ(h.wide(h.kOut[1]), u"\\d\u00E9j\\");
        EXPECT_EQ(h.wide(h.kOut[2]), u"f\u4E2D");
        EXPECT_EQ(h.wide(h.kOut[3]), u".txt");

        // One unit short on dir -> ERANGE, every buffer cleared.
        h.poison();
        h.call("_wsplitpath_s", {h.kPath, h.kOut[0], 3, h.kOut[1], 5,
                                 h.kOut[2], 3, h.kOut[3], 5});
        EXPECT_EQ(h.ret(), SplitpathSHarness::kErange);
        EXPECT_EQ(h.allocation.get_errno(), SplitpathSHarness::kErange);
        for (GuestAddr out : h.kOut) EXPECT_EQ(h.memory.read_u16(out).value(), 0u);

        // path NULL -> handler, EINVAL.
        h.call("_wsplitpath_s", {0, h.kOut[0], 3, h.kOut[1], 6, h.kOut[2], 3,
                                 h.kOut[3], 5});
        EXPECT_EQ(h.ret(), SplitpathSHarness::kEinval);
        EXPECT_EQ(h.allocation.get_errno(), SplitpathSHarness::kEinval);
        EXPECT_EQ(h.handler_calls, 1u);

        // NULL components with zero size are allowed.
        h.call("_wsplitpath_s", {h.kPath, 0, 0, 0, 0, 0, 0, h.kOut[3], 5});
        EXPECT_EQ(h.ret(), 0u);
        EXPECT_EQ(h.wide(h.kOut[3]), u".txt");
    }
}

TEST_F(CrtStringTest, SplitpathNonSecureFormsShareTheDecomposition) {
    for (bool x64 : {false, true}) {
        SCOPED_TRACE(x64 ? "x64" : "x86");
        SplitpathSHarness h(x64);
        h.put_narrow(h.kPath, "relative\\x.tar.gz");
        h.call("_splitpath", {h.kPath, h.kOut[0], h.kOut[1], h.kOut[2],
                              h.kOut[3], 0, 0, 0, 0});
        EXPECT_EQ(h.narrow(h.kOut[0]), "");
        EXPECT_EQ(h.narrow(h.kOut[1]), "relative\\");
        EXPECT_EQ(h.narrow(h.kOut[2]), "x.tar");
        EXPECT_EQ(h.narrow(h.kOut[3]), ".gz");
        h.put_wide(h.kPath, u"E:/a/b.c");
        h.call("_wsplitpath", {h.kPath, h.kOut[0], h.kOut[1], h.kOut[2],
                               h.kOut[3], 0, 0, 0, 0});
        EXPECT_EQ(h.wide(h.kOut[0]), u"E:");
        EXPECT_EQ(h.wide(h.kOut[1]), u"/a/");
        EXPECT_EQ(h.wide(h.kOut[2]), u"b");
        EXPECT_EQ(h.wide(h.kOut[3]), u".c");
    }
}

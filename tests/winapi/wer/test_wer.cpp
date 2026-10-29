/// @file test_wer.cpp
/// @brief wer.dll: the export table matches the real DLL (147 exports, the
///        same ordinals on x86 and x64, wer.lib stdcall sizes), and the
///        application exclusion list behaves as oracle_probe/wer_exclusion
///        measured on Windows (wer_exclusion32/64.expected).

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include "capsule/registry/registry.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/wer/wer.hpp"

#include <algorithm>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr std::wstring_view kKey =
    L"Software\\Microsoft\\Windows\\Windows Error Reporting\\ExcludedApplications";

std::vector<std::wstring> listed(capsule::Registry& reg, capsule::RegHandle root) {
    auto key = reg.open_key(root, kKey, false);
    if (key.is_err()) return {};
    auto names = reg.enum_values(key.value());
    reg.close_key(key.value());
    return names.is_ok() ? names.value() : std::vector<std::wstring>{};
}

std::optional<u32> dword(capsule::Registry& reg, capsule::RegHandle root, std::wstring_view name) {
    auto key = reg.open_key(root, kKey, false);
    if (key.is_err()) return std::nullopt;
    auto v = reg.query_value(key.value(), name);
    reg.close_key(key.value());
    if (v.is_err() || v.value().type != capsule::RegType::Dword) return std::nullopt;
    return std::get<u32>(v.value().data);
}

}  // namespace

TEST(WerTest, ExportTableIsTheRealDll) {
    capsule::Registry reg;
    const DllExports exports = create_wer_thunks(reg);
    EXPECT_EQ(exports.entries.size(), 147u);
    auto find = [&](const char* n) {
        return std::find_if(exports.entries.begin(), exports.entries.end(),
                            [n](const ExportEntry& e) { return e.name == n; });
    };
    auto add = find("WerAddExcludedApplication");
    ASSERT_NE(add, exports.entries.end());
    EXPECT_EQ(add->ordinal, 14u);
    EXPECT_EQ(add->stdcall_bytes, 8u);
    auto chain = find("GetThreadWaitChain");
    ASSERT_NE(chain, exports.entries.end());
    EXPECT_EQ(chain->ordinal, 11u);
    EXPECT_EQ(chain->stdcall_bytes, 28u);
}

TEST(WerTest, ExclusionListMatchesTheProbe) {
    using namespace wer;
    capsule::Registry reg;
    EXPECT_EQ(add_excluded_application(reg, L"vitreo_wer_probe.exe", false), S_OK_);
    EXPECT_EQ(dword(reg, capsule::HKEY_CURRENT_USER, L"vitreo_wer_probe.exe"), 1u);
    // Again, and in another case: S_OK, one value, the first spelling kept.
    EXPECT_EQ(add_excluded_application(reg, L"vitreo_wer_probe.exe", false), S_OK_);
    EXPECT_EQ(add_excluded_application(reg, L"VITREO_WER_PROBE.EXE", false), S_OK_);
    EXPECT_EQ(listed(reg, capsule::HKEY_CURRENT_USER), (std::vector<std::wstring>{L"vitreo_wer_probe.exe"}));
    // A path is listed by its file name; a name without extension as given.
    EXPECT_EQ(add_excluded_application(reg, L"C:\\Dir\\vitreo_path.exe", false), S_OK_);
    EXPECT_EQ(add_excluded_application(reg, L"vitreo_noext", false), S_OK_);
    EXPECT_EQ(dword(reg, capsule::HKEY_CURRENT_USER, L"vitreo_path.exe"), 1u);
    EXPECT_EQ(dword(reg, capsule::HKEY_CURRENT_USER, L"vitreo_noext"), 1u);
    EXPECT_EQ(add_excluded_application(reg, L"", false), E_INVALIDARG_);

    EXPECT_EQ(remove_excluded_application(reg, L"vitreo_wer_probe.exe", false), S_OK_);
    EXPECT_EQ(remove_excluded_application(reg, L"vitreo_wer_probe.exe", false), E_FAIL_);
    EXPECT_EQ(remove_excluded_application(reg, L"VITREO_WER_PROBE.EXE", false), E_FAIL_);
    EXPECT_EQ(remove_excluded_application(reg, L"C:\\Dir\\vitreo_path.exe", false), S_OK_);
    EXPECT_EQ(remove_excluded_application(reg, L"vitreo_noext", false), S_OK_);
    EXPECT_EQ(remove_excluded_application(reg, L"", false), E_INVALIDARG_);
    EXPECT_TRUE(listed(reg, capsule::HKEY_CURRENT_USER).empty());

    // All users: HKLM (the 64-bit view for either caller).
    EXPECT_EQ(add_excluded_application(reg, L"vitreo_wer_all.exe", true), S_OK_);
    EXPECT_EQ(dword(reg, capsule::HKEY_LOCAL_MACHINE, L"vitreo_wer_all.exe"), 1u);
    EXPECT_FALSE(dword(reg, capsule::HKEY_CURRENT_USER, L"vitreo_wer_all.exe").has_value());
    EXPECT_EQ(remove_excluded_application(reg, L"vitreo_wer_all.exe", true), S_OK_);
    EXPECT_TRUE(listed(reg, capsule::HKEY_LOCAL_MACHINE).empty());
}

TEST(WerTest, ThunksReadTheNameAndNullIsInvalidBothAbis) {
    for (bool x64 : {false, true}) {
        capsule::Registry reg;
        const DllExports exports = create_wer_thunks(reg);
        auto add = std::find_if(exports.entries.begin(), exports.entries.end(),
                                [](const ExportEntry& e) { return e.name == "WerAddExcludedApplication"; });
        ASSERT_NE(add, exports.entries.end());
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x10800);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        constexpr GuestAddr kName = 0x10100;
        const std::u16string name = u"sal.exe";
        for (usize i = 0; i <= name.size(); ++i)
            ASSERT_TRUE(memory.write_u16(kName + i * 2, i < name.size() ? name[i] : 0).is_ok());
        auto call = [&](GuestAddr p, u32 all) {
            if (x64) { cpu.gpr.rcx = p; cpu.gpr.rdx = all; }
            else {
                ASSERT_TRUE(memory.write_u32(0x10804, static_cast<u32>(p)).is_ok());
                ASSERT_TRUE(memory.write_u32(0x10808, all).is_ok());
            }
            add->handler(m);
        };
        call(kName, 0);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), wer::S_OK_);
        EXPECT_EQ(dword(reg, capsule::HKEY_CURRENT_USER, L"sal.exe"), 1u);
        call(0, 0);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), wer::E_INVALIDARG_);
    }
}

TEST(WerTest, UnimplementedExportsFailWithTheDocumentedValue) {
    capsule::Registry reg;
    const DllExports exports = create_wer_thunks(reg);
    auto run = [&](const char* n, bool x64) {
        auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                               [n](const ExportEntry& e) { return e.name == n; });
        EXPECT_NE(it, exports.entries.end());
        GuestMemory memory;
        EXPECT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x10800);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        cpu.gpr.rax = 0x5a5a5a5a;
        set_last_error(0);
        it->handler(m);
        return static_cast<u32>(cpu.gpr.rax);
    };
    for (bool x64 : {false, true}) {
        EXPECT_EQ(run("WerReportCreate", x64), 0x80004001u);  // E_NOTIMPL
        EXPECT_EQ(run("OpenThreadWaitChainSession", x64), 0u);
        EXPECT_EQ(get_last_error(), 120u);  // ERROR_CALL_NOT_IMPLEMENTED
    }
}

// faultrep.dll: the errorrep.h API (libfaultrep.a: _ReportFault@8,
// _AddERExcludedApplicationA@4, _AddERExcludedApplicationW@4). ReportFault
// reports frrvErrNoDW (4) because the capsule has no reporting client;
// AddERExcludedApplication stores the name in the HKLM list (Learn remarks)
// and is TRUE on success, FALSE with ERROR_INVALID_PARAMETER for NULL.
TEST(WerTest, FaultrepExportsAndBehaviourBothAbis) {
    for (bool x64 : {false, true}) {
        capsule::Registry reg;
        const DllExports exports = create_faultrep_thunks(reg);
        ASSERT_EQ(exports.entries.size(), 3u);
        auto find = [&](const char* n) {
            auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                   [n](const ExportEntry& e) { return e.name == n; });
            EXPECT_NE(it, exports.entries.end()) << n;
            return it;
        };
        auto report = find("ReportFault");
        auto add_w = find("AddERExcludedApplicationW");
        auto add_a = find("AddERExcludedApplicationA");
        EXPECT_EQ(report->stdcall_bytes, 8u);
        EXPECT_EQ(add_w->stdcall_bytes, 4u);
        EXPECT_EQ(add_a->stdcall_bytes, 4u);

        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(0x10800);
        Marshaler m(memory);
        m.attach_cpu(cpu);
        auto call = [&](std::vector<ExportEntry>::const_iterator fn, u64 a0, u64 a1) {
            if (x64) { cpu.gpr.rcx = a0; cpu.gpr.rdx = a1; }
            else {
                ASSERT_TRUE(memory.write_u32(0x10804, static_cast<u32>(a0)).is_ok());
                ASSERT_TRUE(memory.write_u32(0x10808, static_cast<u32>(a1)).is_ok());
            }
            fn->handler(m);
        };

        call(report, 0x10400, 0);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 4u);  // frrvErrNoDW

        constexpr GuestAddr kWide = 0x10100;
        const std::u16string wide = u"goat.exe";
        for (usize i = 0; i <= wide.size(); ++i)
            ASSERT_TRUE(memory.write_u16(kWide + i * 2, i < wide.size() ? wide[i] : 0).is_ok());
        call(add_w, kWide, 0);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
        EXPECT_EQ(dword(reg, capsule::HKEY_LOCAL_MACHINE, L"goat.exe"), 1u);
        EXPECT_FALSE(dword(reg, capsule::HKEY_CURRENT_USER, L"goat.exe").has_value());

        constexpr GuestAddr kAnsi = 0x10200;
        const std::string ansi = "lockdown.exe";
        for (usize i = 0; i <= ansi.size(); ++i)
            ASSERT_TRUE(memory.write_u8(kAnsi + i, i < ansi.size() ? static_cast<u8>(ansi[i]) : 0).is_ok());
        call(add_a, kAnsi, 0);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 1u);
        EXPECT_EQ(dword(reg, capsule::HKEY_LOCAL_MACHINE, L"lockdown.exe"), 1u);

        set_last_error(0);
        call(add_w, 0, 0);
        EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
        EXPECT_EQ(get_last_error(), 87u);  // ERROR_INVALID_PARAMETER
    }
}

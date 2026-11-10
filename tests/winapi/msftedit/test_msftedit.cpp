/// @file test_msftedit.cpp
/// @brief Msftedit.dll contract: export names/ordinals per bitness against
/// the genuine export tables (tools/oracle_probe/msftedit_exports{32,64}.inc),
/// interface identifiers and DllGetVersion against
/// tests/oracle_data/msftedit_lifecycle_probe{32,64}.expected, x86 argument
/// bytes against the measured return sizes, and the windowless text services
/// failures Learn documents.

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <winapi/ui/msftedit/msftedit.hpp>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kDataBase = 0x10000;
constexpr GuestAddr kStackBase = 0x20000;
constexpr u32 kSOk = 0x00000000u;
constexpr u32 kEInvalidArg = 0x80070057u;
constexpr u32 kEFail = 0x80004005u;

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// name -> ordinal rows of a generated msftedit_exports*.inc.
std::map<std::string, u16> read_export_table(bool is64) {
    const std::string text = read_file(std::string(VITREO_SOURCE_DIR) +
        (is64 ? "/tools/oracle_probe/msftedit_exports64.inc"
              : "/tools/oracle_probe/msftedit_exports32.inc"));
    std::map<std::string, u16> rows;
    const std::regex row(R"re(\{"([^"]+)",\s*(\d+)\})re");
    for (std::sregex_iterator it(text.begin(), text.end(), row), end; it != end; ++it) {
        rows[(*it)[1]] = static_cast<u16>(std::stoi((*it)[2]));
    }
    return rows;
}

std::string expected_path(bool is64) {
    return std::string(VITREO_SOURCE_DIR) +
           (is64 ? "/tests/oracle_data/msftedit_lifecycle_probe64.expected"
                 : "/tests/oracle_data/msftedit_lifecycle_probe32.expected");
}

MsfteditServices null_services() {
    MsfteditServices s;
    s.control_proc = [](Marshaler&, bool) {};
    s.register_class = [](GuestAddr, GuestAddr) {};
    s.unregister_class = [](GuestAddr) {};
    s.export_address = [](const char*) -> GuestAddr { return 0; };
    return s;
}

const ExportEntry* find(const DllExports& exports, const std::string& name) {
    const auto it = std::find_if(exports.entries.begin(), exports.entries.end(),
                                 [&](const ExportEntry& e) { return e.name == name; });
    return it == exports.entries.end() ? nullptr : &*it;
}

} // namespace

TEST(MsfteditExports, NamesAndOrdinalsMatchTheGenuineTablePerBitness) {
    for (bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        const auto expected = read_export_table(is64);
        ASSERT_EQ(expected.size(), 25u) << "missing tools/oracle_probe export table";
        std::map<std::string, u16> actual;
        const auto exports = create_msftedit_thunks(is64, null_services());
        for (const auto& e : exports.entries) actual[e.name] = e.ordinal;
        for (const auto& id : msftedit_interface_ids()) actual[id.name] = id.ordinal;
        EXPECT_EQ(actual, expected);
    }
}

TEST(MsfteditExports, NoOrdinalOnlyExports) {
    // msftedit_lifecycle_probe: GetProcAddress by ordinal 1..64 finds exactly
    // the named exports' ordinals.
    std::set<u16> probed;
    const std::string text = read_file(expected_path(true));
    const std::regex row(R"(ordinal value=([0-9a-f]{8}))");
    for (std::sregex_iterator it(text.begin(), text.end(), row), end; it != end; ++it) {
        probed.insert(static_cast<u16>(std::stoul((*it)[1], nullptr, 16)));
    }
    std::set<u16> ours;
    for (const auto& [name, ordinal] : read_export_table(true)) ours.insert(ordinal);
    EXPECT_EQ(probed, ours);
}

TEST(MsfteditExports, X86ArgumentBytesMatchMeasuredReturns) {
    // ret N of each x86 export body in SysWOW64\msftedit.dll; WNDPROC is
    // CALLBACK (16); DllGetActivationFactory takes (HSTRING, IActivationFactory**).
    const std::map<std::string, u16> measured{
        {"CreateTextServices", 12}, {"DllGetActivationFactory", 8},
        {"DllGetVersion", 4}, {"GetMathAlphanumeric", 8},
        {"GetMathAlphanumericCode", 8}, {"MathBuildDown", 12},
        {"MathBuildUp", 12}, {"MathTranslate", 8},
        {"RichEdit10ANSIWndProc", 16}, {"RichEditANSIWndProc", 16},
        {"RichEditWndProc", 16}, {"SetCustomTextOutHandlerEx", 8},
        {"SetTextServicesDpiCalculationOverride", 8}, {"ShutdownTextServices", 4},
        {"_DisableOleinitCheck@0", 0},
    };
    const auto exports = create_msftedit_thunks(false, null_services());
    ASSERT_EQ(exports.entries.size(), measured.size());
    for (const auto& e : exports.entries) {
        SCOPED_TRACE(e.name);
        ASSERT_TRUE(measured.count(e.name));
        EXPECT_EQ(e.stdcall_bytes, measured.at(e.name));
    }
}

TEST(MsfteditExports, InterfaceIdsMatchTheOracle) {
    for (bool is64 : {false, true}) {
        SCOPED_TRACE(is64 ? "x64" : "x86");
        const std::string text = read_file(expected_path(is64));
        const std::regex row(
            R"(iid (\S+) d1=([0-9a-f]{8}) d2=([0-9a-f]{8}) d4a=([0-9a-f]{8}) d4b=([0-9a-f]{8}))");
        usize seen = 0;
        for (std::sregex_iterator it(text.begin(), text.end(), row), end; it != end; ++it) {
            const std::string name = (*it)[1];
            const auto ids = msftedit_interface_ids();
            const auto id = std::find_if(ids.begin(), ids.end(),
                                         [&](const auto& x) { return name == x.name; });
            ASSERT_NE(id, ids.end()) << name;
            const u32 d1 = static_cast<u32>(std::stoul((*it)[2], nullptr, 16));
            const u32 d23 = static_cast<u32>(std::stoul((*it)[3], nullptr, 16));
            const u32 d4a = static_cast<u32>(std::stoul((*it)[4], nullptr, 16));
            const u32 d4b = static_cast<u32>(std::stoul((*it)[5], nullptr, 16));
            std::array<u8, 16> g{};
            for (int i = 0; i < 4; ++i) g[static_cast<usize>(i)] = static_cast<u8>(d1 >> (8 * i));
            g[4] = static_cast<u8>(d23 >> 16); g[5] = static_cast<u8>(d23 >> 24);
            g[6] = static_cast<u8>(d23);       g[7] = static_cast<u8>(d23 >> 8);
            for (int i = 0; i < 4; ++i) {
                g[static_cast<usize>(8 + i)] = static_cast<u8>(d4a >> (24 - 8 * i));
                g[static_cast<usize>(12 + i)] = static_cast<u8>(d4b >> (24 - 8 * i));
            }
            EXPECT_EQ(id->guid, g) << name;
            ++seen;
        }
        EXPECT_EQ(seen, msftedit_interface_ids().size());
    }
}

TEST(MsfteditExports, LifecycleUnloadsAndRegistersTheClassWithRichEditWndProc) {
    GuestAddr registered_base = 0, registered_proc = 0, unregistered_base = 0;
    MsfteditServices s = null_services();
    s.register_class = [&](GuestAddr base, GuestAddr proc) {
        registered_base = base;
        registered_proc = proc;
    };
    s.unregister_class = [&](GuestAddr base) { unregistered_base = base; };
    s.export_address = [](const char* name) -> GuestAddr {
        return std::string(name) == "RichEditWndProc" ? 0x7000 : 0;
    };
    const auto exports = create_msftedit_thunks(true, std::move(s));
    EXPECT_TRUE(exports.lifecycle.unloads_on_last_free);
    ASSERT_TRUE(exports.lifecycle.process_attach);
    ASSERT_TRUE(exports.lifecycle.process_detach);
    exports.lifecycle.process_attach(0x180000000ull);
    EXPECT_EQ(registered_base, 0x180000000ull);
    EXPECT_EQ(registered_proc, 0x7000u);
    exports.lifecycle.process_detach(0x180000000ull);
    EXPECT_EQ(unregistered_base, 0x180000000ull);
}

class MsfteditCallTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(kDataBase, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(kStackBase, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = kStackBase + 0x800;
        marshaler.attach_cpu(cpu);
        exports = create_msftedit_thunks(false, null_services());
    }
    void set_arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + index * sizeof(u32), value).is_ok());
    }
    u32 call(const char* name) {
        const ExportEntry* e = find(exports, name);
        EXPECT_NE(e, nullptr) << name;
        if (e == nullptr) return 0xFFFFFFFFu;
        e->handler(marshaler);
        return static_cast<u32>(cpu.gpr.rax);
    }
    u32 u32_at(GuestAddr a) { return memory.read_u32(a).value(); }

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports;
};

TEST_F(MsfteditCallTest, DllGetVersionAcceptsOnlyDllVersionInfoSize) {
    // msftedit_lifecycle_probe "DllGetVersion" lines.
    for (u32 cb : {0u, 19u, 32u}) {
        SCOPED_TRACE(cb);
        ASSERT_TRUE(memory.write_u32(kDataBase, cb).is_ok());
        for (u32 i = 1; i < 5; ++i) ASSERT_TRUE(memory.write_u32(kDataBase + 4 * i, 0xAAAA).is_ok());
        set_arg(0, kDataBase);
        EXPECT_EQ(call("DllGetVersion"), kEInvalidArg);
        EXPECT_EQ(u32_at(kDataBase), cb);
        EXPECT_EQ(u32_at(kDataBase + 4), 0xAAAAu);
    }
    set_arg(0, 0);
    EXPECT_EQ(call("DllGetVersion"), kEInvalidArg);

    ASSERT_TRUE(memory.write_u32(kDataBase, 20).is_ok());
    set_arg(0, kDataBase);
    EXPECT_EQ(call("DllGetVersion"), kSOk);
    EXPECT_EQ(u32_at(kDataBase), 20u);
    EXPECT_EQ(u32_at(kDataBase + 4), 10u);   // major
    EXPECT_EQ(u32_at(kDataBase + 8), 0u);    // minor
    EXPECT_NE(u32_at(kDataBase + 12), 0u);   // the system build
    EXPECT_EQ(u32_at(kDataBase + 16), 1u);   // DLLVER_PLATFORM_WINDOWS
}

TEST_F(MsfteditCallTest, CreateTextServicesFailsAsLearnDocuments) {
    // NULL host or NULL ppUnk: E_INVALIDARG, output untouched.
    ASSERT_TRUE(memory.write_u32(kDataBase, 0x1234).is_ok());
    set_arg(0, 0); set_arg(1, 0); set_arg(2, kDataBase);
    EXPECT_EQ(call("CreateTextServices"), kEInvalidArg);
    EXPECT_EQ(u32_at(kDataBase), 0x1234u);
    set_arg(1, kDataBase + 0x100); set_arg(2, 0);
    EXPECT_EQ(call("CreateTextServices"), kEInvalidArg);
    // A valid host: the object cannot be initialised -> E_FAIL, *ppUnk NULL.
    set_arg(2, kDataBase);
    EXPECT_EQ(call("CreateTextServices"), kEFail);
    EXPECT_EQ(u32_at(kDataBase), 0u);
}

TEST_F(MsfteditCallTest, ShutdownTextServicesRejectsForeignObjects) {
    set_arg(0, kDataBase);
    EXPECT_EQ(call("ShutdownTextServices"), kEInvalidArg);
    set_arg(0, 0);
    EXPECT_EQ(call("ShutdownTextServices"), kEInvalidArg);
}

TEST(MsfteditRegistry, DataExportsResolveByNameAndOrdinal) {
    ThunkRegistry reg(0x70000000);
    reg.register_data_export("Msftedit.dll", "IID_ITextServices", 0x5000, 5);
    EXPECT_EQ(reg.lookup_data_export("msftedit.dll", "IID_ITextServices"), 0x5000u);
    EXPECT_EQ(reg.lookup_data_export("msftedit.dll", static_cast<u16>(5)), 0x5000u);
    EXPECT_EQ(reg.lookup_data_export("msftedit.dll", static_cast<u16>(6)), 0u);
    EXPECT_EQ(reg.lookup_data_export("msftedit.dll", static_cast<u16>(0)), 0u);
}

TEST(MsfteditRegistry, LifecycleIsKeptPerDll) {
    ThunkRegistry reg(0x70000000);
    DllExports exports = create_msftedit_thunks(true, null_services());
    ASSERT_TRUE(reg.register_dll("msftedit.dll", std::move(exports)).is_ok());
    const DllLifecycle* life = reg.dll_lifecycle("MSFTEDIT.DLL");
    ASSERT_NE(life, nullptr);
    EXPECT_TRUE(life->unloads_on_last_free);
    EXPECT_TRUE(static_cast<bool>(life->process_attach));
    EXPECT_EQ(reg.dll_lifecycle("riched20.dll"), nullptr);
}

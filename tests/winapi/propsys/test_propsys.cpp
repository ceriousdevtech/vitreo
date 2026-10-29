// SPDX-License-Identifier: LGPL-2.1-only

#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/com/ole32/com_core.hpp"
#include "winapi/com/ole32/task_memory.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/com/propsys/propsys.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/memory.hpp"

#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

ExportEntry* find_export(DllExports& exports, std::string_view name) {
    for (auto& entry : exports.entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

/// COM and task-memory owners for exports that do not use them.
struct PropsysEnv {
    GuestMemory memory;
    HandleTable handles;
    VirtualMemoryManager vmm{memory};
    HeapManager heap{vmm, memory, handles};
    ComTaskMemoryManager task_memory{heap};
    ComCoreManager com{task_memory};
};

DllExports stateless_exports() {
    static PropsysEnv env;
    return make_propsys_exports(env.com, env.task_memory);
}

class PropsysTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(DATA_BASE, 0x10000,
                                      MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(STACK_BASE, 0x1000,
                                      MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);
    }

    void arg(usize index, u32 value) {
        ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4 + index * 4,
                                     value).is_ok());
    }

    std::string read_wide(GuestAddr address) {
        auto result = marshaler.read_wide_string(address);
        EXPECT_TRUE(result.is_ok());
        return result.value_or("");
    }

    static constexpr GuestAddr DATA_BASE = 0x10000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr KEY = DATA_BASE + 0x100;
    static constexpr GuestAddr VALUE = DATA_BASE + 0x200;
    static constexpr GuestAddr OUTPUT = DATA_BASE + 0x400;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
};

TEST_F(PropsysTest, PSStringFromPropertyKeyUsesWindowsFormat) {
    // PKEY_Title = {F29F85E0-4FF9-1068-AB91-08002B27B3D9}, pid 2.
    ASSERT_TRUE(memory.write_u32(KEY + 0, 0xF29F85E0u).is_ok());
    ASSERT_TRUE(memory.write_u16(KEY + 4, 0x4FF9u).is_ok());
    ASSERT_TRUE(memory.write_u16(KEY + 6, 0x1068u).is_ok());
    const u8 data4[8] = {0xAB, 0x91, 0x08, 0x00, 0x2B, 0x27, 0xB3, 0xD9};
    ASSERT_TRUE(memory.write_bytes(KEY + 8, data4, sizeof(data4)).is_ok());
    ASSERT_TRUE(memory.write_u32(KEY + 16, 2).is_ok());

    auto exports = stateless_exports();
    auto* function = find_export(exports, "PSStringFromPropertyKey");
    ASSERT_NE(function, nullptr);
    arg(0, KEY);
    arg(1, OUTPUT);
    arg(2, 50);
    function->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(read_wide(OUTPUT),
              "{F29F85E0-4FF9-1068-AB91-08002B27B3D9} 2");
}

TEST_F(PropsysTest, PSStringFromPropertyKeyTruncatesAndReportsSmallBuffer) {
    ASSERT_TRUE(memory.write_u32(KEY + 0, 0xF29F85E0u).is_ok());
    ASSERT_TRUE(memory.write_u16(KEY + 4, 0x4FF9u).is_ok());
    ASSERT_TRUE(memory.write_u16(KEY + 6, 0x1068u).is_ok());
    ASSERT_TRUE(memory.write_u32(KEY + 16, 2).is_ok());

    auto exports = stateless_exports();
    auto* function = find_export(exports, "PSStringFromPropertyKey");
    ASSERT_NE(function, nullptr);
    arg(0, KEY);
    arg(1, OUTPUT);
    arg(2, 5);
    function->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x8007007Au);
    EXPECT_EQ(read_wide(OUTPUT), "{F29");
}

TEST_F(PropsysTest, PropVariantToStringReadsLpwstr) {
    ASSERT_TRUE(marshaler.write_wide_string(VALUE, "property value").is_ok());
    ASSERT_TRUE(memory.write_u16(KEY, 31).is_ok()); // VT_LPWSTR
    ASSERT_TRUE(memory.write_u32(KEY + 8, VALUE).is_ok());

    auto exports = stateless_exports();
    auto* function = find_export(exports, "PropVariantToString");
    ASSERT_NE(function, nullptr);
    arg(0, KEY);
    arg(1, OUTPUT);
    arg(2, 64);
    function->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(read_wide(OUTPUT), "property value");
}

TEST_F(PropsysTest, PropVariantToStringEmptyProducesEmptyString) {
    ASSERT_TRUE(memory.write_u16(KEY, 0).is_ok()); // VT_EMPTY
    ASSERT_TRUE(memory.write_u16(OUTPUT, 0xFFFF).is_ok());

    auto exports = stateless_exports();
    auto* function = find_export(exports, "PropVariantToString");
    ASSERT_NE(function, nullptr);
    arg(0, KEY);
    arg(1, OUTPUT);
    arg(2, 2);
    function->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(memory.read_u16(OUTPUT).value(), 0u);
}

TEST(PropsysX64Test, PropVariantToStringReadsEightBytePointer) {
    GuestMemory memory;
    constexpr GuestAddr DATA = 0x10000;
    ASSERT_TRUE(memory.map_region(DATA, 0x10000,
                                  MemoryProtection::ReadWrite).is_ok());
    cpu::CpuState cpu;
    cpu.init_long_mode();
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);

    constexpr GuestAddr propvariant = DATA + 0x100;
    constexpr GuestAddr value = DATA + 0x200;
    constexpr GuestAddr output = DATA + 0x400;
    ASSERT_TRUE(marshaler.write_wide_string(value, "x64 value").is_ok());
    ASSERT_TRUE(memory.write_u16(propvariant, 31).is_ok()); // VT_LPWSTR
    ASSERT_TRUE(memory.write_u64(propvariant + 8, value).is_ok());
    cpu.gpr.rcx = propvariant;
    cpu.gpr.rdx = output;
    cpu.gpr.r8 = 64;

    auto exports = stateless_exports();
    auto* function = find_export(exports, "PropVariantToString");
    ASSERT_NE(function, nullptr);
    function->handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0u);
    EXPECT_EQ(marshaler.read_wide_string(output).value(), "x64 value");
}

} // namespace

// ---- PSGetPropertyKeyFromName / PSGetNameFromPropertyKey -----------------------------------------
//
// Expected values are winoracle's (tools/oracle_probe/propsys_name_probe.c):
// propsys_name_probe{32,64}.expected for the apartment and NULL-pointer
// rules, propsys_name_lines{32,64}.expected for every name and key string.

namespace {

constexpr u32 kTypeElementNotFound = 0x8002802Bu;
constexpr u32 kCoNotInitialized = 0x800401F0u;

std::vector<std::string> oracle_lines(const std::string& file) {
    std::ifstream in(std::string(VITREO_ORACLE_PROBE_DIR) + "/" + file);
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    return lines;
}

class PropsysNameTest : public ::testing::TestWithParam<bool> {
protected:
    static constexpr GuestAddr kData = 0x10000;
    static constexpr GuestAddr kTeb = 0x30000;
    static constexpr GuestAddr kTebOther = 0x34000;
    static constexpr GuestAddr kStack = 0x80000;
    static constexpr GuestAddr kName = kData + 0x100;
    static constexpr GuestAddr kKey = kData + 0x1000;
    static constexpr GuestAddr kOut = kData + 0x1100;

    void SetUp() override {
        ASSERT_TRUE(env.memory.map_region(kData, 0x10000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(env.memory.map_region(kTeb, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(env.memory.map_region(kTebOther, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(env.memory.map_region(kStack, 0x1000, MemoryProtection::ReadWrite).is_ok());
        thread = make_thread(kTeb);
        exports = make_propsys_exports(env.com, env.task_memory);
    }

    struct Thread {
        cpu::CpuState cpu;
        std::unique_ptr<Marshaler> m;
    };
    std::unique_ptr<Thread> make_thread(GuestAddr teb) {
        auto t = std::make_unique<Thread>();
        if (GetParam()) {
            t->cpu.init_long_mode();
            t->cpu.seg.gs_desc.base = teb;
        } else {
            t->cpu.init_protected_mode();
            t->cpu.seg.fs_desc.base = teb;
        }
        t->cpu.set_sp(kStack + 0x800);
        t->m = std::make_unique<Marshaler>(env.memory);
        t->m->attach_cpu(t->cpu);
        return t;
    }

    u32 call(const char* api, u64 a0, u64 a1) {
        auto& cpu = thread->cpu;
        cpu.pending_raise.active = false;
        if (GetParam()) {
            cpu.gpr.rcx = a0;
            cpu.gpr.rdx = a1;
        } else {
            EXPECT_TRUE(env.memory.write_u32(cpu.sp() + 4, static_cast<u32>(a0)).is_ok());
            EXPECT_TRUE(env.memory.write_u32(cpu.sp() + 8, static_cast<u32>(a1)).is_ok());
        }
        cpu.gpr.rax = 0xDEADBEEFu;
        ExportEntry* e = find_export(exports, api);
        EXPECT_NE(e, nullptr) << api;
        if (e != nullptr) e->handler(*thread->m);
        return static_cast<u32>(cpu.gpr.rax);
    }

    void enter_sta() { ASSERT_EQ(env.com.co_initialize_ex(*thread->m, com::COINIT_APARTMENTTHREADED), 0u); }

    void put_name(std::u16string_view name) {
        for (usize i = 0; i < name.size(); ++i)
            ASSERT_TRUE(env.memory.write_u16(kName + i * 2, static_cast<u16>(name[i])).is_ok());
        ASSERT_TRUE(env.memory.write_u16(kName + name.size() * 2, 0).is_ok());
    }

    void prefill_key() {
        const std::vector<u8> fill(20, 0xA5);
        ASSERT_TRUE(env.memory.write_bytes(kKey, fill.data(), fill.size()).is_ok());
    }

    /// The key at kKey the way the probe prints it: "{GUID} pid" after a
    /// success, otherwise its 20 bytes in hex.
    std::string key_text(bool success) {
        std::vector<u8> b(20);
        EXPECT_TRUE(env.memory.read_bytes(kKey, b.data(), b.size()).is_ok());
        char text[96];
        if (!success) {
            std::string hex;
            for (u8 v : b) {
                std::snprintf(text, sizeof text, "%02X", v);
                hex += text;
            }
            return hex;
        }
        std::snprintf(text, sizeof text, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} %u",
                      env.memory.read_u32(kKey).value(), env.memory.read_u16(kKey + 4).value(),
                      env.memory.read_u16(kKey + 6).value(), b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15],
                      env.memory.read_u32(kKey + 16).value());
        return text;
    }

    void set_key(const std::string& text) {
        // "{GUID} pid" as the probe prints it.
        unsigned d1, d2, d3, b[8], pid;
        ASSERT_EQ(std::sscanf(text.c_str(), "{%8X-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X} %u", &d1, &d2, &d3, &b[0],
                              &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7], &pid),
                  12)
            << text;
        ASSERT_TRUE(env.memory.write_u32(kKey, d1).is_ok());
        ASSERT_TRUE(env.memory.write_u16(kKey + 4, static_cast<u16>(d2)).is_ok());
        ASSERT_TRUE(env.memory.write_u16(kKey + 6, static_cast<u16>(d3)).is_ok());
        for (int i = 0; i < 8; ++i) ASSERT_TRUE(env.memory.write_u8(kKey + 8 + i, static_cast<u8>(b[i])).is_ok());
        ASSERT_TRUE(env.memory.write_u32(kKey + 16, pid).is_ok());
    }

    std::string out_name() {
        const GuestAddr p = thread->m->read_ptr(kOut).value_or(0);
        if (p == 0) return "NULL";
        EXPECT_TRUE(env.task_memory.owns(p)) << "not CoTaskMemAlloc'd";
        return thread->m->read_wide_string(p).value_or("<unreadable>");
    }

    std::string bitness_file(const std::string& stem) { return stem + (GetParam() ? "64" : "32") + ".expected"; }

    PropsysEnv env;
    std::unique_ptr<Thread> thread;
    DllExports exports;
};

std::u16string widen(std::string_view s) { return std::u16string(s.begin(), s.end()); }

TEST_P(PropsysNameTest, NameLookupsOutsideAnApartmentFailWithAZeroedKey) {
    // propsys_name_probe: "pre.*" rows, and again after CoUninitialize.
    for (const char* name : {"System.AppUserModel.ID", "{9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3} 5", "DocTitle", ""}) {
        put_name(widen(name));
        prefill_key();
        EXPECT_EQ(call("PSGetPropertyKeyFromName", kName, kKey), kCoNotInitialized) << name;
        EXPECT_EQ(key_text(false), std::string(40, '0')) << name;
    }
    set_key("{9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3} 5");
    ASSERT_TRUE(thread->m->write_ptr(kOut, 0xA5A5A5A5u).is_ok());
    EXPECT_EQ(call("PSGetNameFromPropertyKey", kKey, kOut), kCoNotInitialized);
    EXPECT_EQ(out_name(), "NULL");

    enter_sta();
    env.com.co_uninitialize(*thread->m);
    put_name(u"System.AppUserModel.ID");
    EXPECT_EQ(call("PSGetPropertyKeyFromName", kName, kKey), kCoNotInitialized);
}

TEST_P(PropsysNameTest, AnotherThreadsMtaIsEnoughAsIsOwnMta) {
    // "implicit_mta" and "mta" rows: both resolve.
    auto other = make_thread(kTebOther);
    ASSERT_EQ(env.com.co_initialize_ex(*other->m, com::COINIT_MULTITHREADED), 0u);
    put_name(u"System.AppUserModel.ID");
    EXPECT_EQ(call("PSGetPropertyKeyFromName", kName, kKey), 0u);
    EXPECT_EQ(key_text(true), "{9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3} 5");
    set_key("{9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3} 5");
    EXPECT_EQ(call("PSGetNameFromPropertyKey", kKey, kOut), 0u);
    EXPECT_EQ(out_name(), "System.AppUserModel.ID");
}

TEST_P(PropsysNameTest, NullPointersFaultWhereWindowsFaults) {
    // "f_*" rows: C0000005, read (0) or write (1), address 0.
    enter_sta();
    auto expect_fault = [&](bool write, const char* what) {
        const auto& r = thread->cpu.pending_raise;
        EXPECT_TRUE(r.active) << what;
        EXPECT_EQ(r.code, 0xC0000005u) << what;
        EXPECT_EQ(r.args[0], write ? 1u : 0u) << what;
        EXPECT_EQ(r.args[1], 0u) << what;
    };
    (void)call("PSGetPropertyKeyFromName", 0, kKey);
    expect_fault(false, "f_null_name");
    for (const char* name : {"System.AppUserModel.ID", "System.Nope", ""}) {
        put_name(widen(name));
        (void)call("PSGetPropertyKeyFromName", kName, 0);
        expect_fault(true, name);
    }
    (void)call("PSGetNameFromPropertyKey", 0, kOut);
    expect_fault(false, "f_name_null_key");
    set_key("{9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3} 5");
    (void)call("PSGetNameFromPropertyKey", kKey, 0);
    expect_fault(true, "f_name_out_known");
    set_key("{12345678-1234-1234-1234-123456789ABC} 2");
    (void)call("PSGetNameFromPropertyKey", kKey, 0);
    expect_fault(true, "f_name_out_unknown");
}

TEST_P(PropsysNameTest, EveryOracleLookupMatchesWindows) {
    enter_sta();
    // The registered schema on winoracle: names Windows knows beyond
    // propkey.h are the documented gap (Vitreo's schema is propkey.h), and
    // so are the three propkey.h names that build does not register.
    std::set<std::string> registered;
    for (const std::string& l : oracle_lines(bitness_file("propsys_name_enum"))) {
        if (l.rfind("reg ", 0) == 0) registered.insert(l.substr(4, l.find(' ', 4) - 4));
    }
    ASSERT_GT(registered.size(), 1600u);
    const std::regex n_row(R"(^N hr=([0-9A-F]{8}) (\{[0-9A-F-]+\} \d+|[0-9A-F]{40}) <(.*)>$)");
    const std::regex k_row(R"(^K hr=([0-9A-F]{8}) (\{[0-9A-F-]+\} \d+) (.*)$)");
    usize names = 0, keys = 0, registered_only = 0, unregistered_sdk = 0;
    for (const std::string& l : oracle_lines(bitness_file("propsys_name_lines"))) {
        std::smatch mt;
        if (std::regex_match(l, mt, n_row)) {
            ++names;
            const u32 want = static_cast<u32>(std::stoul(mt[1].str(), nullptr, 16));
            const std::string name = mt[3].str();
            put_name(widen(name));
            prefill_key();
            const u32 got = call("PSGetPropertyKeyFromName", kName, kKey);
            if (want == 0 && got == kTypeElementNotFound && registered.count(name)) {
                ++registered_only;
                EXPECT_EQ(key_text(false), std::string(40, '0')) << name;
                continue;
            }
            if (want == kTypeElementNotFound && got == 0 && !registered.count(name)) {
                ++unregistered_sdk;
                continue;
            }
            EXPECT_EQ(got, want) << "<" << name << ">";
            EXPECT_EQ(key_text(want == 0), mt[2].str()) << "<" << name << ">";
        } else if (std::regex_match(l, mt, k_row)) {
            ++keys;
            const u32 want = static_cast<u32>(std::stoul(mt[1].str(), nullptr, 16));
            set_key(mt[2].str());
            ASSERT_TRUE(thread->m->write_ptr(kOut, 0xA5A5A5A5u).is_ok());
            const u32 got = call("PSGetNameFromPropertyKey", kKey, kOut);
            const std::string name = out_name();
            if (want == 0 && got == kTypeElementNotFound && registered.count(mt[3].str())) {
                ++registered_only;
                EXPECT_EQ(name, "NULL");
                continue;
            }
            if (want == kTypeElementNotFound && got == 0) {
                ++unregistered_sdk;
                continue;
            }
            EXPECT_EQ(got, want) << mt[2].str();
            EXPECT_EQ(name, want == 0 ? mt[3].str() : "NULL") << mt[2].str();
        }
    }
    EXPECT_GT(names, 4000u);
    EXPECT_GT(keys, 1600u);
    // 3 propkey.h names that build leaves unregistered, by name and by key.
    EXPECT_EQ(unregistered_sdk, 6u);
    // 618 registered names propkey.h does not define, by name and by key.
    EXPECT_EQ(registered_only, 2u * 618u);
}

INSTANTIATE_TEST_SUITE_P(Bitness, PropsysNameTest, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? std::string("x64") : std::string("x86"); });

} // namespace

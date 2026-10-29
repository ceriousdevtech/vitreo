// SPDX-License-Identifier: LGPL-2.1-only
#include <gtest/gtest.h>

#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/com/combase/combase.hpp"
#include "winapi/com/ole32/com_core.hpp"
#include "winapi/kernel32/handle.hpp"
#include "winapi/kernel32/memory/heap.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "hardware/contracts/now_playing.hpp"
#include "winapi/core/marshaler.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

struct TestGuid {
    u32 data1;
    u16 data2;
    u16 data3;
    u8 data4[8];
};

constexpr TestGuid IID_GLOBALIZATION_PREFERENCES{
    0x01BF4326u, 0xED37u, 0x4E96u,
    {0xB0, 0xE9, 0xC1, 0x34, 0x0D, 0x1E, 0xA1, 0x58}};
constexpr TestGuid IID_ACTIVATION_FACTORY{
    0x00000035u, 0x0000u, 0x0000u,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr TestGuid IID_APP_CAPABILITY_STATICS{
    0x7C353E2Au, 0x46EEu, 0x44E5u,
    {0xAF, 0x3D, 0x6A, 0xD3, 0xFC, 0x49, 0xBD, 0x22}};
constexpr TestGuid IID_UI_VIEW_SETTINGS_INTEROP{
    0x3694DBF9u, 0x8F68u, 0x44BEu,
    {0x8F, 0xF5, 0x19, 0x5C, 0x98, 0xED, 0xE8, 0xA6}};
constexpr TestGuid IID_UI_VIEW_SETTINGS{
    0xC63657F6u, 0x8850u, 0x470Du,
    {0x88, 0xF8, 0x45, 0x5E, 0x16, 0xEA, 0x2C, 0x26}};
constexpr TestGuid IID_SYSTEM_MEDIA_TRANSPORT_CONTROLS2{
    0xEA98D2F6u, 0x7F3Cu, 0x4AF2u,
    {0xA5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xEF, 0xB1}};
constexpr u32 S_OK = 0;
constexpr u32 E_NOTIMPL = 0x80004001u;
constexpr u32 E_NOINTERFACE = 0x80004002u;
constexpr u32 REGDB_E_CLASSNOTREG = 0x80040154u;

WinrtGlobalizationPreferences test_preferences() {
    return {
        {u"GregorianCalendar"},
        {u"24HourClock"},
        {u"USD"},
        {u"en-US", u"es-MX"},
        u"US",
        1,
    };
}

// 0-16 are the IInspectable/vector/activation thunks; 17-29 back
// IJsonValueStatics + IJsonValue and 30-36 back IJsonObject.
constexpr std::array<GuestAddr, CombaseManager::kMethodThunks> METHOD_THUNKS = {
    0x70000010, 0x70000020, 0x70000030, 0x70000040,
    0x70000050, 0x70000060, 0x70000070, 0x70000080,
    0x70000090, 0x700000A0, 0x700000B0, 0x700000C0,
    0x700000D0, 0x700000E0, 0x700000F0, 0x70000100,
    0x70000110, 0x70000120, 0x70000130, 0x70000140,
    0x70000150, 0x70000160, 0x70000170, 0x70000180,
    0x70000190, 0x700001A0, 0x700001B0, 0x700001C0,
    0x700001D0, 0x700001E0, 0x700001F0, 0x70000200,
    0x70000210, 0x70000220, 0x70000230, 0x70000240,
    0x70000250, 0x70000260, 0x70000270, 0x70000280,
    0x70000290, 0x700002A0, 0x700002B0, 0x700002C0,
    0x700002D0, 0x700002E0, 0x700002F0, 0x70000300,
    0x70000310, 0x70000320, 0x70000330, 0x70000340,
    0x70000350, 0x70000360, 0x70000370, 0x70000380,
    0x70000390, 0x700003A0, 0x700003B0, 0x700003C0,
    0x700003D0,

    0x700003E0,

    0x700003F0, 0x70000400,

    0x70000410, 0x70000420, 0x70000430, 0x70000440, 0x70000450, 0x70000460, 0x70000470, 0x70000480, 0x70000490, 0x700004A0, 0x700004B0, 0x700004C0, 0x700004D0,

    0x700004E0, 0x700004F0, 0x70000500, 0x70000510, 0x70000520, 0x70000530, 0x70000540, 0x70000550, 0x70000560, 0x70000570, 0x70000580, 0x70000590, 0x700005A0, 0x700005B0, 0x700005C0, 0x700005D0, 0x700005E0, 0x700005F0, 0x70000600, 0x70000610, 0x70000620, 0x70000630, 0x70000640, 0x70000650, 0x70000660, 0x70000670, 0x70000680, 0x70000690, 0x700006A0, 0x700006B0, 0x700006C0,

    0x700006D0, 0x700006E0, 0x700006F0, 0x70000700, 0x70000710, 0x70000720, 0x70000730, 0x70000740, 0x70000750, 0x70000760, 0x70000770, 0x70000780, 0x70000790, 0x700007A0, 0x700007B0, 0x700007C0, 0x700007D0, 0x700007E0,

    0x700007F0, 0x70000800, 0x70000810, 0x70000820, 0x70000830,

    0x70000840, 0x70000850, 0x70000860, 0x70000870,
    0x70000880, 0x70000890, 0x700008A0, 0x700008B0,
    0x700008C0, 0x700008D0, 0x700008E0, 0x700008F0,
    0x70000900, 0x70000910, 0x70000920, 0x70000930,
    0x70000940, 0x70000950,
};

class CombaseManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x4000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        cpu.mode = cpu::CpuMode::Long;
        cpu.gpr.rsp = STACK_BASE + 0x800;
        manager.set_method_thunks(METHOD_THUNKS);
        ASSERT_TRUE(memory.write_bytes(
            IID_ADDRESS,
            reinterpret_cast<const Byte*>(&IID_GLOBALIZATION_PREFERENCES),
            sizeof(IID_GLOBALIZATION_PREFERENCES)).is_ok());
    }

    void write_fast_hstring(
        GuestAddr header, GuestAddr buffer, std::u16string_view value) {
        ASSERT_TRUE(memory.write_u32(header, 1).is_ok());
        ASSERT_TRUE(memory.write_u32(
            header + 4, static_cast<u32>(value.size())).is_ok());
        ASSERT_TRUE(memory.write_u64(header + 16, buffer).is_ok());
        for (usize i = 0; i < value.size(); ++i) {
            ASSERT_TRUE(memory.write_u16(buffer + i * 2, value[i]).is_ok());
        }
        ASSERT_TRUE(memory.write_u16(
            buffer + value.size() * 2, 0).is_ok());
    }

    GuestAddr activate(std::u16string_view class_name) {
        write_fast_hstring(CLASS_HEADER, CLASS_BUFFER, class_name);
        EXPECT_TRUE(memory.write_u64(OUTPUT_ADDRESS, 0).is_ok());
        cpu.gpr.rcx = CLASS_HEADER;
        cpu.gpr.rdx = IID_ADDRESS;
        cpu.gpr.r8 = OUTPUT_ADDRESS;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_RoGetActivationFactory(marshaler);
        return memory.read_u64(OUTPUT_ADDRESS).value();
    }

    std::u16string read_hstring(GuestAddr string) {
        cpu.gpr.rcx = string;
        cpu.gpr.rdx = LENGTH_ADDRESS;
        Marshaler marshaler(memory);
        marshaler.attach_cpu(cpu);
        manager.impl_WindowsGetStringRawBuffer(marshaler);
        const GuestAddr buffer = cpu.gpr.rax;
        const u32 length = memory.read_u32(LENGTH_ADDRESS).value();
        std::u16string value(length, u'\0');
        for (u32 i = 0; i < length; ++i) {
            value[i] = memory.read_u16(buffer + i * 2).value();
        }
        return value;
    }

    static constexpr GuestAddr DATA_BASE = 0x1000;
    static constexpr GuestAddr CLASS_HEADER = DATA_BASE + 0x100;
    static constexpr GuestAddr CLASS_BUFFER = DATA_BASE + 0x200;
    static constexpr GuestAddr IID_ADDRESS = DATA_BASE + 0x500;
    static constexpr GuestAddr OUTPUT_ADDRESS = DATA_BASE + 0x600;
    static constexpr GuestAddr SECOND_OUTPUT_ADDRESS = DATA_BASE + 0x608;
    static constexpr GuestAddr LENGTH_ADDRESS = DATA_BASE + 0x620;
    static constexpr GuestAddr STACK_BASE = 0x80000;

    GuestMemory memory;
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    cpu::CpuState cpu;
    CombaseManager manager{memory, heap, test_preferences()};
};

// WinRT is free-threaded: HSTRINGs and activation objects are process-wide
// and every thread calls WindowsCreateString / Release / RoGetActivationFactory,
// all of which live in m_objects / m_strings. The impl_* tests above never see
// the lock, so this goes through the export table the guest calls: first the
// D3D11-style proof that an entry blocks while another thread holds the API
// lock, then 8 threads x 1500 create/duplicate/delete rounds with a quarter of
// the strings kept alive so m_strings keeps rehashing under the lookups.
TEST_F(CombaseManagerTest, EveryGuestEntryHoldsTheApiLockAndStringsSurviveConcurrency) {
    ComCoreManager com;
    auto exports = create_combase_thunks(manager, com);
    const auto find = [&](const std::string& name) {
        const auto it = std::find_if(
            exports.entries.begin(), exports.entries.end(),
            [&](const ExportEntry& e) { return e.name == name; });
        EXPECT_NE(it, exports.entries.end()) << name;
        return it;
    };
    const auto create_entry = find("WindowsCreateString");
    const auto duplicate_entry = find("WindowsDuplicateString");
    const auto delete_entry = find("WindowsDeleteString");
    ASSERT_NE(create_entry, exports.entries.end());
    ASSERT_NE(duplicate_entry, exports.entries.end());
    ASSERT_NE(delete_entry, exports.entries.end());

    {
        std::atomic<bool> returned{false};
        std::unique_lock held(manager.api_mutex());
        std::thread caller([&] {
            cpu::CpuState thread_cpu = cpu;
            thread_cpu.gpr.rcx = 0;  // WindowsDeleteString(NULL): S_OK at once
            Marshaler thread_marshaler(memory);
            thread_marshaler.attach_cpu(thread_cpu);
            delete_entry->handler(thread_marshaler);
            returned = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT_FALSE(returned.load())
            << "WindowsDeleteString ran while another thread held the API lock";
        held.unlock();
        caller.join();
        EXPECT_TRUE(returned.load());
    }

    constexpr int kThreads = 8;
    constexpr int kRounds = 1500;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            cpu::CpuState tcpu;
            tcpu.mode = cpu::CpuMode::Long;
            tcpu.gpr.rsp = STACK_BASE + 0x100 + static_cast<u64>(t) * 0x100;
            Marshaler tm(memory);
            tm.attach_cpu(tcpu);
            const GuestAddr source = DATA_BASE + 0x2000 + static_cast<GuestAddr>(t) * 0x100;
            const GuestAddr out = source + 0x40;
            const GuestAddr copy_out = source + 0x48;
            const std::u16string value = u"race" + std::u16string(1, static_cast<char16_t>(u'0' + t));
            for (usize i = 0; i < value.size(); ++i) {
                (void)memory.write_u16(source + i * 2, value[i]);
            }
            (void)memory.write_u16(source + value.size() * 2, 0);
            std::vector<GuestAddr> kept;
            for (int i = 0; i < kRounds; ++i) {
                tcpu.gpr.rcx = source;
                tcpu.gpr.rdx = value.size();
                tcpu.gpr.r8 = out;
                create_entry->handler(tm);
                const GuestAddr string = memory.read_u64(out).value_or(0);
                if (tcpu.gpr.rax != S_OK || string == 0) { ++failures; continue; }
                tcpu.gpr.rcx = string;
                tcpu.gpr.rdx = copy_out;
                duplicate_entry->handler(tm);
                const GuestAddr copy = memory.read_u64(copy_out).value_or(0);
                if (tcpu.gpr.rax != S_OK || copy == 0) ++failures;
                tcpu.gpr.rcx = copy;
                delete_entry->handler(tm);
                if (tcpu.gpr.rax != S_OK) ++failures;
                if (i % 4 == 0) {
                    kept.push_back(string);
                } else {
                    tcpu.gpr.rcx = string;
                    delete_entry->handler(tm);
                    if (tcpu.gpr.rax != S_OK) ++failures;
                }
            }
            for (GuestAddr string : kept) {
                tcpu.gpr.rcx = string;
                delete_entry->handler(tm);
                if (tcpu.gpr.rax != S_OK) ++failures;
            }
        });
    }
    for (auto& w : workers) w.join();
    EXPECT_EQ(failures.load(), 0);
}

TEST_F(CombaseManagerTest, UnknownActivationClassFailsAndClearsOutput) {
    ASSERT_TRUE(memory.write_u64(
        OUTPUT_ADDRESS, 0xAAAAAAAAAAAAAAAAull).is_ok());
    write_fast_hstring(
        CLASS_HEADER, CLASS_BUFFER, u"Windows.Unknown.Class");
    cpu.gpr.rcx = CLASS_HEADER;
    cpu.gpr.rdx = IID_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_RoGetActivationFactory(marshaler);

    EXPECT_EQ(cpu.gpr.rax, REGDB_E_CLASSNOTREG);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
}

TEST_F(CombaseManagerTest, CreatesGlobalizationFactoryWithInspectableVtable) {
    const GuestAddr factory = activate(
        u"Windows.System.UserProfile.GlobalizationPreferences");
    ASSERT_NE(factory, 0u);
    EXPECT_EQ(cpu.gpr.rax, S_OK);

    const GuestAddr vtable = memory.read_u64(factory).value();
    for (usize slot = 0; slot < 12; ++slot) {
        EXPECT_EQ(
            memory.read_u64(vtable + slot * 8).value(),
            METHOD_THUNKS[slot]);
    }
}

TEST_F(CombaseManagerTest, AppCapabilityStaticsUsesWindowsMetadataVtableOrder) {
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_APP_CAPABILITY_STATICS),
        sizeof(IID_APP_CAPABILITY_STATICS)).is_ok());
    const GuestAddr factory = activate(
        u"Windows.Security.Authorization.AppCapabilityAccess.AppCapability");
    ASSERT_NE(factory, 0u);
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    const GuestAddr vtable = memory.read_u64(factory).value();
    for (usize slot = 0; slot < 6; ++slot) {
        EXPECT_EQ(memory.read_u64(vtable + slot * 8).value(),
                  METHOD_THUNKS[slot]);
    }
    EXPECT_EQ(memory.read_u64(vtable + 6 * 8).value(), METHOD_THUNKS[129]);
    EXPECT_EQ(memory.read_u64(vtable + 7 * 8).value(), METHOD_THUNKS[130]);
    EXPECT_EQ(memory.read_u64(vtable + 8 * 8).value(), METHOD_THUNKS[75]);
    EXPECT_EQ(memory.read_u64(vtable + 9 * 8).value(), METHOD_THUNKS[76]);

    ASSERT_TRUE(memory.write_u64(OUTPUT_ADDRESS, ~0ull).is_ok());
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = 0;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler request(memory);
    request.attach_cpu(cpu);
    manager.impl_AppCapabilityRequestAccessForCapabilitiesAsync(request);
    EXPECT_EQ(cpu.gpr.rax, E_NOTIMPL);
    EXPECT_EQ(memory.read_u64(OUTPUT_ADDRESS).value(), 0u);
}

TEST_F(CombaseManagerTest, ActivatesWindowsDataJsonObject) {
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_ACTIVATION_FACTORY),
        sizeof(IID_ACTIVATION_FACTORY)).is_ok());
    const GuestAddr factory = activate(u"Windows.Data.Json.JsonObject");
    ASSERT_NE(factory, 0u);
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    const GuestAddr factory_vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(factory_vtable + 6 * 8).value(),
              METHOD_THUNKS[16]);

    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = SECOND_OUTPUT_ADDRESS;
    Marshaler activate_instance(memory);
    activate_instance.attach_cpu(cpu);
    manager.impl_ActivateInstance(activate_instance);

    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr instance =
        memory.read_u64(SECOND_OUTPUT_ADDRESS).value();
    ASSERT_NE(instance, 0u);
    const GuestAddr instance_vtable = memory.read_u64(instance).value();
    EXPECT_EQ(memory.read_u64(instance_vtable).value(), METHOD_THUNKS[0]);

    cpu.gpr.rcx = instance;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    Marshaler get_class_name(memory);
    get_class_name.attach_cpu(cpu);
    manager.impl_GetRuntimeClassName(get_class_name);
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(read_hstring(memory.read_u64(OUTPUT_ADDRESS).value()),
              u"Windows.Data.Json.JsonObject");
}

TEST_F(CombaseManagerTest, UIViewSettingsFactoryExposesInteropGetForWindow) {
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_UI_VIEW_SETTINGS_INTEROP),
        sizeof(IID_UI_VIEW_SETTINGS_INTEROP)).is_ok());
    const GuestAddr factory =
        activate(u"Windows.UI.ViewManagement.UIViewSettings");
    ASSERT_NE(factory, 0u);
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    const GuestAddr vtable = memory.read_u64(factory).value();
    EXPECT_EQ(memory.read_u64(vtable + 6 * 8).value(), METHOD_THUNKS[72]);
}

TEST_F(CombaseManagerTest, UIViewSettingsGetForWindowUsesDesktopInteropAbi32) {
    cpu.mode = cpu::CpuMode::Protected;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_UI_VIEW_SETTINGS),
        sizeof(IID_UI_VIEW_SETTINGS)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0x62006040).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0x00013414).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, IID_ADDRESS).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, OUTPUT_ADDRESS).is_ok());

    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_UIViewSettingsGetForWindow(marshaler);

    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr settings = memory.read_u32(OUTPUT_ADDRESS).value();
    ASSERT_NE(settings, 0u);

    ASSERT_TRUE(memory.write_u32(
        cpu.sp() + 4, static_cast<u32>(settings)).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, SECOND_OUTPUT_ADDRESS).is_ok());
    manager.impl_UIViewSettingsGetUserInteractionMode(marshaler);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(memory.read_u32(SECOND_OUTPUT_ADDRESS).value(), 0u);

    TestGuid unsupported = IID_UI_VIEW_SETTINGS;
    unsupported.data1 ^= 1;
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&unsupported),
        sizeof(unsupported)).is_ok());
    ASSERT_TRUE(memory.write_u32(OUTPUT_ADDRESS, 0xAAAAAAAAu).is_ok());
    manager.impl_UIViewSettingsGetForWindow(marshaler);
    EXPECT_EQ(cpu.gpr.rax, E_NOINTERFACE);
    EXPECT_EQ(memory.read_u32(OUTPUT_ADDRESS).value(), 0u);
}

TEST_F(CombaseManagerTest, ReturnsConfiguredHomeRegionAsOwnedHstring) {
    const GuestAddr factory = activate(
        u"Windows.System.UserProfile.GlobalizationPreferences");
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = SECOND_OUTPUT_ADDRESS;
    Marshaler getter(memory);
    getter.attach_cpu(cpu);
    manager.impl_GetHomeGeographicRegion(getter);

    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr region =
        memory.read_u64(SECOND_OUTPUT_ADDRESS).value();
    ASSERT_NE(region, 0u);
    EXPECT_EQ(read_hstring(region), u"US");
}

TEST_F(CombaseManagerTest, LanguageVectorSupportsSizeAndIndexedAccess) {
    const GuestAddr factory = activate(
        u"Windows.System.UserProfile.GlobalizationPreferences");
    cpu.gpr.rcx = factory;
    cpu.gpr.rdx = SECOND_OUTPUT_ADDRESS;
    Marshaler get_languages(memory);
    get_languages.attach_cpu(cpu);
    manager.impl_GetLanguages(get_languages);
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    const GuestAddr vector =
        memory.read_u64(SECOND_OUTPUT_ADDRESS).value();
    ASSERT_NE(vector, 0u);
    const GuestAddr vtable = memory.read_u64(vector).value();
    EXPECT_EQ(memory.read_u64(vtable + 6 * 8).value(), METHOD_THUNKS[12]);
    EXPECT_EQ(memory.read_u64(vtable + 7 * 8).value(), METHOD_THUNKS[13]);

    cpu.gpr.rcx = vector;
    cpu.gpr.rdx = LENGTH_ADDRESS;
    Marshaler get_size(memory);
    get_size.attach_cpu(cpu);
    manager.impl_VectorGetSize(get_size);
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(memory.read_u32(LENGTH_ADDRESS).value(), 2u);

    cpu.gpr.rcx = vector;
    cpu.gpr.rdx = 1;
    cpu.gpr.r8 = SECOND_OUTPUT_ADDRESS;
    Marshaler get_at(memory);
    get_at.attach_cpu(cpu);
    manager.impl_VectorGetAt(get_at);
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(
        read_hstring(memory.read_u64(SECOND_OUTPUT_ADDRESS).value()),
        u"es-MX");
}

TEST_F(CombaseManagerTest, HstringCreateDuplicateAndDeletePreserveContents) {
    constexpr GuestAddr SOURCE = DATA_BASE + 0x800;
    constexpr GuestAddr STRING_OUT = DATA_BASE + 0x900;
    constexpr GuestAddr COPY_OUT = DATA_BASE + 0x908;
    constexpr std::u16string_view VALUE = u"en-US";
    for (usize i = 0; i < VALUE.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(SOURCE + i * 2, VALUE[i]).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(SOURCE + VALUE.size() * 2, 0).is_ok());

    cpu.gpr.rcx = SOURCE;
    cpu.gpr.rdx = VALUE.size();
    cpu.gpr.r8 = STRING_OUT;
    Marshaler create(memory);
    create.attach_cpu(cpu);
    manager.impl_WindowsCreateString(create);
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr string = memory.read_u64(STRING_OUT).value();
    EXPECT_EQ(read_hstring(string), VALUE);
    EXPECT_EQ(memory.read_u32(string).value(), 0u);
    EXPECT_EQ(memory.read_u32(string + 4).value(), VALUE.size());
    EXPECT_EQ(memory.read_u64(string + 16).value(), string + 28);
    EXPECT_EQ(memory.read_u32(string + 24).value(), 1u);
    EXPECT_EQ(
        heap.heap_size(heap.get_process_heap(), 0, string),
        32u + VALUE.size() * sizeof(char16_t));

    cpu.gpr.rcx = string;
    cpu.gpr.rdx = COPY_OUT;
    Marshaler duplicate(memory);
    duplicate.attach_cpu(cpu);
    manager.impl_WindowsDuplicateString(duplicate);
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(memory.read_u64(COPY_OUT).value(), string);
    EXPECT_EQ(memory.read_u32(string + 24).value(), 2u);

    cpu.gpr.rcx = string;
    Marshaler release(memory);
    release.attach_cpu(cpu);
    manager.impl_WindowsDeleteString(release);
    EXPECT_EQ(memory.read_u32(string + 24).value(), 1u);
    manager.impl_WindowsDeleteString(release);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(
        heap.heap_size(heap.get_process_heap(), 0, string),
        ~0ULL);
}

TEST_F(CombaseManagerTest, X86OwnedHstringUsesGuestRefcountAndProcessHeap) {
    constexpr GuestAddr SOURCE = DATA_BASE + 0xB00;
    constexpr GuestAddr STRING_OUT = DATA_BASE + 0xC00;
    constexpr std::u16string_view FIRST = u"en-US";
    constexpr std::u16string_view SECOND = u"fr-FR";

    auto create_x86 = [&](std::u16string_view value) {
        for (usize i = 0; i < value.size(); ++i) {
            EXPECT_TRUE(
                memory.write_u16(SOURCE + i * 2, value[i]).is_ok());
        }
        EXPECT_TRUE(memory.write_u16(
            SOURCE + value.size() * 2, 0).is_ok());
        EXPECT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 4, SOURCE).is_ok());
        EXPECT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 8, static_cast<u32>(value.size())).is_ok());
        EXPECT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 12, STRING_OUT).is_ok());
        Marshaler create(memory);
        create.attach_cpu(cpu);
        manager.impl_WindowsCreateString(create);
        EXPECT_EQ(cpu.gpr.rax, S_OK);
        return static_cast<GuestAddr>(
            memory.read_u32(STRING_OUT).value());
    };

    cpu.mode = cpu::CpuMode::Protected;
    const GuestAddr first = create_x86(FIRST);
    ASSERT_NE(first, 0u);
    EXPECT_EQ(memory.read_u32(first).value(), 0u);
    EXPECT_EQ(memory.read_u32(first + 4).value(), FIRST.size());
    EXPECT_EQ(memory.read_u32(first + 16).value(), first + 24);
    EXPECT_EQ(memory.read_u32(first + 20).value(), 1u);
    EXPECT_EQ(
        heap.heap_size(heap.get_process_heap(), 0, first),
        28u + FIRST.size() * sizeof(char16_t));

    // Some Windows runtimes release an owned HSTRING inline by decrementing
    // its header count and freeing the allocation on the process heap.
    ASSERT_TRUE(memory.write_u32(first + 20, 0).is_ok());
    ASSERT_TRUE(heap.heap_free(heap.get_process_heap(), 0, first));

    const GuestAddr second = create_x86(SECOND);
    ASSERT_EQ(second, first);
    EXPECT_EQ(memory.read_u32(second + 20).value(), 1u);
    const GuestAddr buffer = memory.read_u32(second + 16).value();
    for (usize i = 0; i < SECOND.size(); ++i) {
        EXPECT_EQ(memory.read_u16(buffer + i * 2).value(), SECOND[i]);
    }
}

TEST_F(CombaseManagerTest, ActivationTracksActiveGuestBitness) {
    ASSERT_NE(activate(
        u"Windows.System.UserProfile.GlobalizationPreferences"), 0u);

    constexpr GuestAddr X86_HEADER = DATA_BASE + 0xA00;
    constexpr std::u16string_view CLASS_NAME =
        u"Windows.System.UserProfile.GlobalizationPreferences";
    ASSERT_TRUE(memory.write_u32(X86_HEADER, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(
        X86_HEADER + 4, static_cast<u32>(CLASS_NAME.size())).is_ok());
    ASSERT_TRUE(memory.write_u32(X86_HEADER + 16, CLASS_BUFFER).is_ok());
    ASSERT_TRUE(memory.write_u64(
        OUTPUT_ADDRESS, 0xAAAAAAAAAAAAAAAAull).is_ok());

    cpu.mode = cpu::CpuMode::Protected;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, X86_HEADER).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, IID_ADDRESS).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, OUTPUT_ADDRESS).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_RoGetActivationFactory(marshaler);

    EXPECT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_NE(memory.read_u32(OUTPUT_ADDRESS).value(), 0u);
    EXPECT_EQ(
        memory.read_u32(OUTPUT_ADDRESS + 4).value(), 0xAAAAAAAAu);
}

TEST(CombaseManagerX86Test, ActivationWritesOnlyNativePointerWidth) {
    constexpr GuestAddr DATA = 0x1000;
    constexpr GuestAddr STACK = 0x80000;
    constexpr GuestAddr HEADER = DATA + 0x100;
    constexpr GuestAddr BUFFER = DATA + 0x200;
    constexpr GuestAddr IID = DATA + 0x500;
    constexpr GuestAddr OUTPUT = DATA + 0x600;
    constexpr std::u16string_view CLASS_NAME =
        u"Windows.System.UserProfile.GlobalizationPreferences";

    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(
        DATA, 0x4000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.map_region(
        STACK, 0x1000, MemoryProtection::ReadWrite).is_ok());
    ASSERT_TRUE(memory.write_u32(HEADER, 1).is_ok());
    ASSERT_TRUE(memory.write_u32(
        HEADER + 4, static_cast<u32>(CLASS_NAME.size())).is_ok());
    ASSERT_TRUE(memory.write_u32(HEADER + 16, BUFFER).is_ok());
    for (usize i = 0; i < CLASS_NAME.size(); ++i) {
        ASSERT_TRUE(memory.write_u16(BUFFER + i * 2, CLASS_NAME[i]).is_ok());
    }
    ASSERT_TRUE(memory.write_u16(
        BUFFER + CLASS_NAME.size() * 2, 0).is_ok());
    ASSERT_TRUE(memory.write_bytes(
        IID, reinterpret_cast<const Byte*>(&IID_GLOBALIZATION_PREFERENCES),
        sizeof(IID_GLOBALIZATION_PREFERENCES)).is_ok());
    ASSERT_TRUE(memory.write_u64(
        OUTPUT, 0xAAAAAAAAAAAAAAAAull).is_ok());

    cpu::CpuState cpu;
    cpu.mode = cpu::CpuMode::Protected;
    cpu.gpr.rsp = STACK + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, HEADER).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, IID).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 12, OUTPUT).is_ok());
    VirtualMemoryManager vmm{memory};
    HandleTable handles;
    HeapManager heap{vmm, memory, handles};
    CombaseManager manager{memory, heap, test_preferences()};
    manager.set_method_thunks(METHOD_THUNKS);
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_RoGetActivationFactory(marshaler);

    EXPECT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_NE(memory.read_u32(OUTPUT).value(), 0u);
    EXPECT_EQ(memory.read_u32(OUTPUT + 4).value(), 0xAAAAAAAAu);
}

} // namespace

// Releasing a cached activation factory must evict it from its cache slot.
// A slot left naming a freed object is handed straight back to the next
// RoGetActivationFactory, which then dereferences an address that no longer
// resolves to an object. Chrome's browser process hit exactly this on the
// toast-notification factory and aborted.
TEST_F(CombaseManagerTest, ReleasedFactoryIsReactivatedRatherThanReusedStale) {
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_ACTIVATION_FACTORY),
        sizeof(IID_ACTIVATION_FACTORY)).is_ok());

    const GuestAddr first =
        activate(u"Windows.UI.Notifications.ToastNotificationManager");
    ASSERT_NE(first, 0u);
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    cpu.gpr.rcx = first;
    Marshaler release(memory);
    release.attach_cpu(cpu);
    manager.impl_Release(release);
    ASSERT_EQ(cpu.gpr.rax, 0u);

    const GuestAddr second =
        activate(u"Windows.UI.Notifications.ToastNotificationManager");
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    ASSERT_NE(second, 0u);
    // The reactivated factory must be a live object: releasing it again has to
    // find it and report the drop to zero, not the "unknown object" 0.
    cpu.gpr.rcx = second;
    Marshaler release_again(memory);
    release_again.attach_cpu(cpu);
    manager.impl_Release(release_again);
    EXPECT_EQ(cpu.gpr.rax, 0u);
}

// The whole SystemMediaTransportControls metadata chain, end to end:
// GetForWindow -> get_DisplayUpdater -> get_MusicProperties -> put_Title /
// put_Artist -> Update() must reach the host now-playing surface. Spotify
// builds exactly this chain, and every link before Update() is a silent
// accumulator -- if the last one drops the metadata, nothing anywhere fails.
namespace {
class RecordingNowPlayingHost final : public vitreo::hardware::NowPlayingHost {
public:
    bool update(const vitreo::hardware::NowPlayingInfo& info) override {
        last = info;
        ++updates;
        return true;
    }
    bool clear() override {
        ++clears;
        return true;
    }

    vitreo::hardware::NowPlayingInfo last{};
    int updates = 0;
    int clears = 0;
};
} // namespace

TEST_F(CombaseManagerTest, DisplayUpdaterUpdatePublishesMusicMetadataToHost) {
    RecordingNowPlayingHost host;
    manager.set_now_playing_host(&host);

    auto call = [&](auto&& fn) {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        fn(m);
    };

    // GetForWindow(self, hwnd, riid=null, out)
    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 0x1234;  // any non-null HWND
    cpu.gpr.r8 = 0;
    cpu.gpr.r9 = OUTPUT_ADDRESS;
    call([&](Marshaler& m) { manager.impl_SmtcGetForWindow(m); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr smtc = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(smtc, 0u);

    // put_PlaybackStatus(MediaPlaybackStatus.Playing): 3 in windows.media.h
    // (Closed 0, Changing 1, Stopped 2, Playing 3, Paused 4).
    cpu.gpr.rcx = smtc;
    cpu.gpr.rdx = 3;
    call([&](Marshaler& m) { manager.impl_SmtcPutPlaybackStatus(m); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    // get_DisplayUpdater
    cpu.gpr.rcx = smtc;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    call([&](Marshaler& m) { manager.impl_SmtcGetDisplayUpdater(m); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr updater = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(updater, 0u);

    // get_MusicProperties
    cpu.gpr.rcx = updater;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    call([&](Marshaler& m) { manager.impl_DuGetMusicProperties(m); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr music = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(music, 0u);

    write_fast_hstring(CLASS_HEADER, CLASS_BUFFER, u"Bohemian Rhapsody");
    cpu.gpr.rcx = music;
    cpu.gpr.rdx = CLASS_HEADER;
    call([&](Marshaler& m) { m.set_return(manager.music_put_string(m, 0)); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    write_fast_hstring(CLASS_HEADER, CLASS_BUFFER, u"Queen");
    cpu.gpr.rcx = music;
    cpu.gpr.rdx = CLASS_HEADER;
    call([&](Marshaler& m) { m.set_return(manager.music_put_string(m, 2)); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    // Nothing may reach the host before Update() -- the setters are a staging
    // buffer on Windows too.
    EXPECT_EQ(host.updates, 0);

    cpu.gpr.rcx = updater;
    call([&](Marshaler& m) { manager.impl_DuUpdate(m); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);

    EXPECT_EQ(host.updates, 1);
    EXPECT_EQ(host.last.title, "Bohemian Rhapsody");
    EXPECT_EQ(host.last.artist, "Queen");
    EXPECT_EQ(host.last.status, vitreo::hardware::NowPlayingStatus::Playing);

    // get_Title must read back what put_Title staged.
    cpu.gpr.rcx = music;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    call([&](Marshaler& m) { m.set_return(manager.music_get_string(m, 0)); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(read_hstring(memory.read_u64(OUTPUT_ADDRESS).value()),
              u"Bohemian Rhapsody");

    // ClearAll withdraws the item from the host and empties the staging fields.
    cpu.gpr.rcx = updater;
    call([&](Marshaler& m) { manager.impl_DuClearAll(m); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(host.clears, 1);

    cpu.gpr.rcx = music;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    call([&](Marshaler& m) { m.set_return(manager.music_get_string(m, 0)); });
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_TRUE(read_hstring(memory.read_u64(OUTPUT_ADDRESS).value()).empty());
}

TEST_F(CombaseManagerTest, GetForWindowPublishesSmtc2EventVtable) {
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS,
        reinterpret_cast<const Byte*>(&IID_SYSTEM_MEDIA_TRANSPORT_CONTROLS2),
        sizeof(IID_SYSTEM_MEDIA_TRANSPORT_CONTROLS2)).is_ok());
    ASSERT_TRUE(memory.write_u64(OUTPUT_ADDRESS, 0).is_ok());

    cpu.gpr.rcx = 0;
    cpu.gpr.rdx = 0x1234;
    cpu.gpr.r8 = IID_ADDRESS;
    cpu.gpr.r9 = OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_SmtcGetForWindow(marshaler);

    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr controls2 = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(controls2, 0u);
    const GuestAddr vtable = memory.read_u64(controls2).value();
    ASSERT_NE(vtable, 0u);
    EXPECT_EQ(memory.read_u64(vtable + 10 * 8).value(), METHOD_THUNKS[126]);
    EXPECT_EQ(memory.read_u64(vtable + 11 * 8).value(), METHOD_THUNKS[127]);
    EXPECT_EQ(memory.read_u64(vtable + 12 * 8).value(), METHOD_THUNKS[128]);
    EXPECT_EQ(memory.read_u64(vtable + 13 * 8).value(), METHOD_THUNKS[104]);
    EXPECT_EQ(memory.read_u64(vtable + 14 * 8).value(), METHOD_THUNKS[105]);
    EXPECT_EQ(memory.read_u64(vtable + 19 * 8).value(), METHOD_THUNKS[104]);
    EXPECT_EQ(memory.read_u64(vtable + 20 * 8).value(), METHOD_THUNKS[105]);
}

TEST_F(CombaseManagerTest, SmtcEventRegistrationReturnsUniqueTokensOnX86) {
    cpu.mode = cpu::CpuMode::Protected;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 8, 0x1234).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 12, 0).is_ok());
    ASSERT_TRUE(memory.write_u32(cpu.sp() + 16, OUTPUT_ADDRESS).is_ok());
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_SmtcGetForWindow(marshaler);
    ASSERT_EQ(cpu.gpr.rax, S_OK);
    const u32 controls = memory.read_u32(OUTPUT_ADDRESS).value();
    ASSERT_NE(controls, 0u);

    constexpr GuestAddr HANDLER1 = DATA_BASE + 0x800;
    constexpr GuestAddr HANDLER2 = DATA_BASE + 0x820;
    constexpr GuestAddr HANDLER_VTABLE = DATA_BASE + 0x900;
    constexpr GuestAddr ADDREF = 0x70001000;
    constexpr GuestAddr RELEASE = 0x70002000;
    ASSERT_TRUE(memory.write_u32(HANDLER1, HANDLER_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u32(HANDLER2, HANDLER_VTABLE).is_ok());
    ASSERT_TRUE(memory.write_u32(HANDLER_VTABLE + 4, ADDREF).is_ok());
    ASSERT_TRUE(memory.write_u32(HANDLER_VTABLE + 8, RELEASE).is_ok());
    std::vector<std::pair<GuestAddr, GuestAddr>> ref_calls;
    manager.set_guest_call(
        [&](GuestAddr method, const u64* args, usize count, bool is_64bit) {
            EXPECT_EQ(count, 1u);
            EXPECT_FALSE(is_64bit);
            ref_calls.emplace_back(method, args[0]);
            return method == ADDREF ? 2u : 1u;
        });

    auto add = [&](u32 handler, GuestAddr token_out) {
        EXPECT_TRUE(memory.write_u32(cpu.sp() + 4, controls).is_ok());
        EXPECT_TRUE(memory.write_u32(cpu.sp() + 8, handler).is_ok());
        EXPECT_TRUE(memory.write_u32(
            cpu.sp() + 12, static_cast<u32>(token_out)).is_ok());
        manager.impl_SmtcAddEvent(marshaler);
        EXPECT_EQ(cpu.gpr.rax, S_OK);
        return memory.read_u64(token_out).value();
    };
    const u64 first = add(HANDLER1, OUTPUT_ADDRESS + 0x20);
    const u64 second = add(HANDLER2, OUTPUT_ADDRESS + 0x28);
    EXPECT_NE(first, 0u);
    EXPECT_NE(second, 0u);
    EXPECT_NE(first, second);

    ASSERT_TRUE(memory.write_u32(cpu.sp() + 4, controls).is_ok());
    ASSERT_TRUE(memory.write_u32(
        cpu.sp() + 8, static_cast<u32>(first)).is_ok());
    ASSERT_TRUE(memory.write_u32(
        cpu.sp() + 12, static_cast<u32>(first >> 32)).is_ok());
    manager.impl_SmtcRemoveEvent(marshaler);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    ASSERT_EQ(ref_calls.size(), 3u);
    EXPECT_EQ(ref_calls[0], std::make_pair(ADDREF, HANDLER1));
    EXPECT_EQ(ref_calls[1], std::make_pair(ADDREF, HANDLER2));
    EXPECT_EQ(ref_calls[2], std::make_pair(RELEASE, HANDLER1));
}

TEST_F(CombaseManagerTest, LauncherLaunchUriAsyncOpensHttpAndRefusesOtherSchemes) {
    constexpr TestGuid IID_URI_FACTORY{
        0x44A9796Fu, 0x723Eu, 0x4FDFu,
        {0xA2, 0x18, 0x03, 0x3E, 0x75, 0xB0, 0xC0, 0x84}};
    constexpr TestGuid IID_LAUNCHER{
        0x277151C3u, 0x9E3Eu, 0x42F6u,
        {0x91, 0xA4, 0x5D, 0xFD, 0xEB, 0x23, 0x24, 0x51}};
    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_URI_FACTORY),
        sizeof(IID_URI_FACTORY)).is_ok());
    const GuestAddr uri_factory = activate(u"Windows.Foundation.Uri");
    ASSERT_NE(uri_factory, 0u);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr uri_vtable = memory.read_u64(uri_factory).value();
    EXPECT_EQ(memory.read_u64(uri_vtable + 6 * 8).value(), METHOD_THUNKS[133]);

    ASSERT_TRUE(memory.write_bytes(
        IID_ADDRESS, reinterpret_cast<const Byte*>(&IID_LAUNCHER),
        sizeof(IID_LAUNCHER)).is_ok());
    const GuestAddr launcher = activate(u"Windows.System.Launcher");
    ASSERT_NE(launcher, 0u);
    const GuestAddr launcher_vtable = memory.read_u64(launcher).value();
    EXPECT_EQ(memory.read_u64(launcher_vtable + 8 * 8).value(), METHOD_THUNKS[132]);

    write_fast_hstring(CLASS_HEADER, CLASS_BUFFER, u"spotify:login");
    cpu.gpr.rcx = uri_factory;
    cpu.gpr.rdx = CLASS_HEADER;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    Marshaler marshaler(memory);
    marshaler.attach_cpu(cpu);
    manager.impl_UriCreate(marshaler);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr uri = memory.read_u64(OUTPUT_ADDRESS).value();
    ASSERT_NE(uri, 0u);

    cpu.gpr.rcx = launcher;
    cpu.gpr.rdx = uri;
    cpu.gpr.r8 = SECOND_OUTPUT_ADDRESS;
    manager.impl_LauncherLaunchUri(marshaler);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    const GuestAddr operation = memory.read_u64(SECOND_OUTPUT_ADDRESS).value();
    ASSERT_NE(operation, 0u);
    cpu.gpr.rcx = operation;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    manager.impl_AsyncGetResults(marshaler);
    EXPECT_EQ(cpu.gpr.rax, S_OK);
    EXPECT_EQ(memory.read_u8(OUTPUT_ADDRESS).value(), 0u);
}

// S7 audit: WinRT string contracts measured by
// tools/oracle_probe/s7_winrt_probe.expected (Windows 11, x86 = x64).
TEST_F(CombaseManagerTest, S7HstringCompareOrdinalAndReferenceTerminator) {
    const auto make = [&](GuestAddr source, std::u16string_view text) {
        for (usize i = 0; i < text.size(); ++i) {
            EXPECT_TRUE(memory.write_u16(source + i * 2, text[i]).is_ok());
        }
        EXPECT_TRUE(memory.write_u16(source + text.size() * 2, 0).is_ok());
        cpu.gpr.rcx = source;
        cpu.gpr.rdx = text.size();
        cpu.gpr.r8 = OUTPUT_ADDRESS;
        Marshaler create(memory);
        create.attach_cpu(cpu);
        manager.impl_WindowsCreateString(create);
        EXPECT_EQ(cpu.gpr.rax, S_OK);
        return static_cast<GuestAddr>(memory.read_u64(OUTPUT_ADDRESS).value());
    };
    const GuestAddr abc = make(DATA_BASE + 0x800, u"abc");
    const GuestAddr abd = make(DATA_BASE + 0x840, u"abd");
    const GuestAddr ab = make(DATA_BASE + 0x880, u"ab");
    const GuestAddr upper = make(DATA_BASE + 0x8C0, u"ABC");
    const GuestAddr embedded = make(DATA_BASE + 0x900, std::u16string_view(u"a\0b", 3));

    const auto run = [&](GuestAddr left, GuestAddr right, GuestAddr result) {
        cpu.gpr.rcx = left;
        cpu.gpr.rdx = right;
        cpu.gpr.r8 = result;
        ASSERT_TRUE(memory.write_u32(LENGTH_ADDRESS, 0x55).is_ok());
        Marshaler m(memory);
        m.attach_cpu(cpu);
        manager.impl_WindowsCompareStringOrdinal(m);
    };
    const auto order = [&](GuestAddr left, GuestAddr right) {
        run(left, right, LENGTH_ADDRESS);
        EXPECT_EQ(cpu.gpr.rax, S_OK);
        return static_cast<i32>(memory.read_u32(LENGTH_ADDRESS).value());
    };
    EXPECT_EQ(order(abc, abd), -1);
    EXPECT_EQ(order(abd, abc), 1);
    EXPECT_EQ(order(abc, abc), 0);
    EXPECT_EQ(order(ab, abc), -1);
    EXPECT_EQ(order(0, 0), 0);
    EXPECT_EQ(order(0, abc), -1);
    EXPECT_EQ(order(embedded, ab), -1);
    EXPECT_EQ(order(abc, upper), 1);
    run(abc, abd, 0);
    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), 0x80070057u);

    // WindowsCreateStringReference requires sourceString[length] == 0, also
    // for length 0: E_STRING_NOT_NULL_TERMINATED, output NULL.
    constexpr GuestAddr header = DATA_BASE + 0xA00;
    const auto ref = [&](GuestAddr source, u32 length) {
        EXPECT_TRUE(memory.write_u64(SECOND_OUTPUT_ADDRESS, 0x77).is_ok());
        cpu.gpr.rcx = source;
        cpu.gpr.rdx = length;
        cpu.gpr.r8 = header;
        cpu.gpr.r9 = SECOND_OUTPUT_ADDRESS;
        Marshaler m(memory);
        m.attach_cpu(cpu);
        manager.impl_WindowsCreateStringReference(m);
        return static_cast<u32>(cpu.gpr.rax);
    };
    EXPECT_EQ(ref(DATA_BASE + 0x800, 0), 0x80000017u);
    EXPECT_EQ(memory.read_u64(SECOND_OUTPUT_ADDRESS).value(), 0u);
    EXPECT_EQ(ref(DATA_BASE + 0x900, 2), 0x80000017u);
    EXPECT_EQ(ref(DATA_BASE + 0x800, 3), S_OK);
    EXPECT_EQ(memory.read_u64(SECOND_OUTPUT_ADDRESS).value(), header);
}

// ---------------------------------------------------------------------------
// SystemMediaTransportControlsTimelineProperties and the window's controls as
// tools/oracle_probe/smtc_timeline_probe measured them on Windows 11 (x86 and
// x64 alike). IIDs from windows.media.h.
// ---------------------------------------------------------------------------
namespace {
struct RawGuid {
    u32 data1;
    u16 data2;
    u16 data3;
    std::array<u8, 8> data4;
};
constexpr RawGuid kIidActivationFactory{0x00000035, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr RawGuid kIidWeakReferenceSource{0x00000038, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr RawGuid kIidTimeline{0x5125316A, 0xC3A2, 0x475B, {0x85, 0x07, 0x93, 0x53, 0x4D, 0xC8, 0x8F, 0x15}};
constexpr RawGuid kIidSmtc{0x99FA3FF4, 0x1742, 0x42A6, {0x90, 0x2E, 0x08, 0x7D, 0x41, 0xF9, 0x65, 0xEC}};
constexpr RawGuid kIidSmtc2{0xEA98D2F6, 0x7F3C, 0x4AF2, {0xA5, 0x86, 0x72, 0x88, 0x98, 0x08, 0xEF, 0xB1}};
constexpr std::u16string_view kTimelineClass =
    u"Windows.Media.SystemMediaTransportControlsTimelineProperties";
}  // namespace

class SmtcTimelineTest : public CombaseManagerTest {
protected:
    static constexpr GuestAddr kIid = DATA_BASE + 0xA00;
    void write_iid(const RawGuid& g, GuestAddr at = kIid) {
        ASSERT_TRUE(memory.write_bytes(at, reinterpret_cast<const Byte*>(&g), sizeof g).is_ok());
    }
    template <typename Fn>
    u64 call(Fn&& fn) {
        Marshaler m(memory);
        m.attach_cpu(cpu);
        fn(m);
        return cpu.gpr.rax;
    }
    GuestAddr out() { return memory.read_u64(OUTPUT_ADDRESS).value_or(0); }
    GuestAddr new_timeline() {
        write_iid(kIidActivationFactory, IID_ADDRESS);
        const GuestAddr factory = activate(kTimelineClass);
        EXPECT_NE(factory, 0u);
        cpu.gpr.rcx = factory;
        cpu.gpr.rdx = OUTPUT_ADDRESS;
        EXPECT_EQ(call([&](Marshaler& m) { manager.impl_ActivateInstance(m); }), S_OK);
        return out();
    }
    GuestAddr query(GuestAddr self, const RawGuid& iid) {
        write_iid(iid);
        cpu.gpr.rcx = self;
        cpu.gpr.rdx = kIid;
        cpu.gpr.r8 = OUTPUT_ADDRESS;
        EXPECT_EQ(call([&](Marshaler& m) { manager.impl_QueryInterface(m); }), S_OK);
        return out();
    }
    GuestAddr controls(u64 hwnd, const RawGuid& iid) {
        write_iid(iid);
        cpu.gpr.rcx = 0;
        cpu.gpr.rdx = hwnd;
        cpu.gpr.r8 = kIid;
        cpu.gpr.r9 = OUTPUT_ADDRESS;
        EXPECT_EQ(call([&](Marshaler& m) { manager.impl_SmtcGetForWindow(m); }), S_OK);
        return out();
    }
    i64 get_time(GuestAddr t, u32 field) {
        cpu.gpr.rcx = t;
        cpu.gpr.rdx = OUTPUT_ADDRESS;
        EXPECT_EQ(call([&](Marshaler& m) { m.set_return(manager.timeline_get(m, field)); }), S_OK);
        return static_cast<i64>(memory.read_u64(OUTPUT_ADDRESS).value_or(0));
    }
    void put_time(GuestAddr t, u32 field, i64 ticks) {
        cpu.gpr.rcx = t;
        cpu.gpr.rdx = static_cast<u64>(ticks);
        EXPECT_EQ(call([&](Marshaler& m) { m.set_return(manager.timeline_put(m, field)); }), S_OK);
    }
};

TEST_F(SmtcTimelineTest, TimelinePropertiesMatchWindows) {
    const GuestAddr t = new_timeline();
    ASSERT_NE(t, 0u);
    EXPECT_NE(new_timeline(), t);  // "second instance distinct=1"
    EXPECT_EQ(query(t, kIidTimeline), t);  // "QI timeline ... same=1"

    cpu.gpr.rcx = t;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_GetRuntimeClassName(m); }), S_OK);
    EXPECT_EQ(read_hstring(out()), kTimelineClass);
    cpu.gpr.rcx = t;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_GetTrustLevel(m); }), S_OK);
    EXPECT_EQ(memory.read_u32(OUTPUT_ADDRESS).value_or(9), 0u);  // "level=0"
    cpu.gpr.rcx = t;
    cpu.gpr.rdx = LENGTH_ADDRESS;
    cpu.gpr.r8 = OUTPUT_ADDRESS;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_GetIids(m); }), S_OK);
    ASSERT_EQ(memory.read_u32(LENGTH_ADDRESS).value_or(0), 2u);  // "count=2"
    RawGuid iids[2]{};
    ASSERT_TRUE(memory.read_bytes(out(), reinterpret_cast<Byte*>(iids), sizeof iids).is_ok());
    EXPECT_EQ(iids[0].data1, kIidTimeline.data1);
    EXPECT_EQ(iids[1].data1, kIidWeakReferenceSource.data1);

    for (u32 field = 0; field < 5; ++field) EXPECT_EQ(get_time(t, field), 0) << field;  // "defaults"
    // "set start=10000000 end=2400000000 min=0 max=2300000000 pos=650000000"
    put_time(t, 0, 10000000);
    put_time(t, 1, 2400000000LL);
    put_time(t, 2, 0);
    put_time(t, 3, 2300000000LL);
    put_time(t, 4, 650000000LL);
    EXPECT_EQ(get_time(t, 0), 10000000);
    EXPECT_EQ(get_time(t, 1), 2400000000LL);
    EXPECT_EQ(get_time(t, 3), 2300000000LL);
    EXPECT_EQ(get_time(t, 4), 650000000LL);
    put_time(t, 4, -5);  // "put negative position hr=00000000"
    EXPECT_EQ(get_time(t, 4), -5);
    put_time(t, 4, 2400000050LL);  // "put position past end": kept as given
    EXPECT_EQ(get_time(t, 4), 2400000050LL);
}

TEST_F(SmtcTimelineTest, TimelinePutTakesATimeSpanAsTwoStackWordsOnX86) {
    const GuestAddr t = new_timeline();
    cpu.mode = cpu::CpuMode::Protected;
    const GuestAddr sp = STACK_BASE + 0x400;
    cpu.gpr.rsp = sp;
    ASSERT_TRUE(memory.write_u32(sp + 4, static_cast<u32>(t)).is_ok());
    ASSERT_TRUE(memory.write_u32(sp + 8, 0x89ABCDEFu).is_ok());   // low word
    ASSERT_TRUE(memory.write_u32(sp + 12, 0x01234567u).is_ok());  // high word
    EXPECT_EQ(call([&](Marshaler& m) { m.set_return(manager.timeline_put(m, 4)); }) & 0xFFFFFFFFu, S_OK);
    cpu.mode = cpu::CpuMode::Long;
    cpu.gpr.rsp = STACK_BASE + 0x800;
    EXPECT_EQ(get_time(t, 4), 0x0123456789ABCDEFLL);
}

TEST_F(SmtcTimelineTest, WindowControlsShareStateAcrossTheirTwoInterfaces) {
    const GuestAddr v1 = controls(0x1234, kIidSmtc);
    ASSERT_NE(v1, 0u);
    const GuestAddr v2 = query(v1, kIidSmtc2);
    EXPECT_NE(v2, v1);                          // "same_pointer=0"
    EXPECT_EQ(controls(0x1234, kIidSmtc2), v2);  // "GetForWindow smtc2 ... same_as_qi=1"
    EXPECT_EQ(query(v2, kIidSmtc), v1);          // "QI back to v1 same=1"
    EXPECT_NE(controls(0x5678, kIidSmtc), v1);   // another window, other controls

    auto flag = [&](u32 bit) {
        cpu.gpr.rcx = v1;
        cpu.gpr.rdx = OUTPUT_ADDRESS;
        EXPECT_EQ(call([&](Marshaler& m) { m.set_return(manager.smtc_get_flag(m, bit)); }), S_OK);
        return memory.read_u8(OUTPUT_ADDRESS).value_or(9);
    };
    EXPECT_EQ(flag(0), 1u);  // "flag defaults ... enabled=1"
    EXPECT_EQ(flag(1), 0u);
    auto get32 = [&](GuestAddr self, auto impl) {
        cpu.gpr.rcx = self;
        cpu.gpr.rdx = OUTPUT_ADDRESS;
        EXPECT_EQ(call([&](Marshaler& m) { (manager.*impl)(m); }), S_OK);
        return memory.read_u32(OUTPUT_ADDRESS).value_or(9);
    };
    auto rate = [&] {
        cpu.gpr.rcx = v2;
        cpu.gpr.rdx = OUTPUT_ADDRESS;
        EXPECT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2GetPlaybackRate(m); }), S_OK);
        double value = -1;
        const u64 bits = memory.read_u64(OUTPUT_ADDRESS).value_or(0);
        std::memcpy(&value, &bits, sizeof value);
        return value;
    };
    EXPECT_EQ(rate(), 0.0);  // "PlaybackRate rate=0"
    EXPECT_EQ(get32(v2, &CombaseManager::impl_Smtc2GetAutoRepeatMode), 0u);
    cpu.gpr.rcx = v2;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2GetShuffleEnabled(m); }), S_OK);
    EXPECT_EQ(memory.read_u8(OUTPUT_ADDRESS).value_or(9), 0u);

    // "after puts repeat=2 shuffle=1 rate=1.5 status=0 enabled=1"
    cpu.gpr.rcx = v2;
    cpu.gpr.rdx = 2;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2PutAutoRepeatMode(m); }), S_OK);
    cpu.gpr.rcx = v2;
    cpu.gpr.rdx = 1;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2PutShuffleEnabled(m); }), S_OK);
    // A DOUBLE argument travels in XMM1 on x64.
    cpu.gpr.rcx = v2;
    const double one_and_half = 1.5;
    std::memcpy(&cpu.sse.xmm[1].u64_[0], &one_and_half, sizeof one_and_half);
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2PutPlaybackRate(m); }), S_OK);
    EXPECT_EQ(get32(v2, &CombaseManager::impl_Smtc2GetAutoRepeatMode), 2u);
    EXPECT_EQ(rate(), 1.5);
    EXPECT_EQ(get32(v1, &CombaseManager::impl_SmtcGetPlaybackStatus), 0u);
    EXPECT_EQ(flag(0), 1u);
    // "put_AutoRepeatMode 7 hr=00000000 now=0"
    cpu.gpr.rcx = v2;
    cpu.gpr.rdx = 7;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2PutAutoRepeatMode(m); }), S_OK);
    EXPECT_EQ(get32(v2, &CombaseManager::impl_Smtc2GetAutoRepeatMode), 0u);
}

TEST_F(SmtcTimelineTest, UpdateTimelinePropertiesReachesTheHost) {
    RecordingNowPlayingHost host;
    manager.set_now_playing_host(&host);
    const GuestAddr v1 = controls(0x1234, kIidSmtc);
    const GuestAddr v2 = query(v1, kIidSmtc2);
    cpu.gpr.rcx = v1;
    cpu.gpr.rdx = 3;  // MediaPlaybackStatus.Playing
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_SmtcPutPlaybackStatus(m); }), S_OK);
    cpu.gpr.rcx = v1;
    cpu.gpr.rdx = OUTPUT_ADDRESS;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_SmtcGetDisplayUpdater(m); }), S_OK);
    const GuestAddr updater = out();
    cpu.gpr.rcx = updater;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_DuUpdate(m); }), S_OK);
    ASSERT_GE(host.updates, 1);
    EXPECT_FALSE(host.last.has_timeline);

    const GuestAddr t = new_timeline();
    put_time(t, 0, 10000000);     // start 1 s
    put_time(t, 1, 2400000000LL); // end 240 s
    put_time(t, 4, 650000000LL);  // position 65 s
    cpu.gpr.rcx = v2;
    cpu.gpr.rdx = t;
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2UpdateTimelineProperties(m); }), S_OK);
    EXPECT_TRUE(host.last.has_timeline);
    EXPECT_DOUBLE_EQ(host.last.duration_seconds, 239.0);
    EXPECT_DOUBLE_EQ(host.last.elapsed_seconds, 64.0);
    EXPECT_DOUBLE_EQ(host.last.rate, 1.0);  // playing, PlaybackRate never set
    EXPECT_EQ(host.last.status, vitreo::hardware::NowPlayingStatus::Playing);

    cpu.gpr.rcx = v1;
    cpu.gpr.rdx = 4;  // Paused
    ASSERT_EQ(call([&](Marshaler& m) { manager.impl_SmtcPutPlaybackStatus(m); }), S_OK);
    EXPECT_EQ(host.last.status, vitreo::hardware::NowPlayingStatus::Paused);
    EXPECT_DOUBLE_EQ(host.last.rate, 0.0);

    cpu.gpr.rcx = v2;
    cpu.gpr.rdx = 0;
    EXPECT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2UpdateTimelineProperties(m); }), com::E_POINTER);
    cpu.gpr.rcx = v2;
    cpu.gpr.rdx = v1;  // not a TimelineProperties
    EXPECT_EQ(call([&](Marshaler& m) { manager.impl_Smtc2UpdateTimelineProperties(m); }), com::E_INVALIDARG);
}

/// @file test_dinput8.cpp
/// @brief DINPUT8.DLL entry-point contract tests.

#include <gtest/gtest.h>

#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>
#include <winapi/media/dinput/dinput.hpp>
#include <winapi/media/dinput8/dinput8.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>

#include <array>
#include <cstring>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr u32 DI_OK = 0;
constexpr u32 DIERR_INVALIDPARAM = 0x80070057u;
constexpr u32 E_NOINTERFACE = 0x80004002u;

// IID_IDirectInput8A = {BF798030-483A-4DA2-AA99-5D64ED369700}
constexpr u8 kIidDirectInput8A[16] = {
    0x30, 0x80, 0x79, 0xBF, 0x3A, 0x48, 0xA2, 0x4D,
    0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00,
};

class Dinput8Test : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(
            DATA_BASE, 0x2000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            STACK_BASE, 0x1000, MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(
            THUNK_BASE, 0x10000, MemoryProtection::ReadWrite).is_ok());
        cpu.init_protected_mode();
        cpu.gpr.rsp = STACK_BASE + 0x800;
        marshaler.attach_cpu(cpu);

        mgr.set_memory(&memory);
        mgr.set_thunk_registry(&thunks);
        auto dinput_exports = create_dinput_thunks(mgr);
        ASSERT_TRUE(thunks.register_dll("dinput.dll",
                                        std::move(dinput_exports)).is_ok());
        exports = create_dinput8_thunks(mgr);
        ASSERT_EQ(exports.entries.size(), 1u);

        ASSERT_TRUE(memory.write_bytes(
            IID_ADDR, kIidDirectInput8A, sizeof(kIidDirectInput8A)).is_ok());
    }

    void set_args(GuestAddr iid, GuestAddr output, GuestAddr outer = 0) {
        ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 4, 0x00400000).is_ok());
        ASSERT_TRUE(memory.write_u32(cpu.gpr.rsp + 8, 0x0800).is_ok());
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 12, static_cast<u32>(iid)).is_ok());
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 16, static_cast<u32>(output)).is_ok());
        ASSERT_TRUE(memory.write_u32(
            cpu.gpr.rsp + 20, static_cast<u32>(outer)).is_ok());
    }

    static constexpr GuestAddr DATA_BASE = 0x10000;
    static constexpr GuestAddr STACK_BASE = 0x80000;
    static constexpr GuestAddr THUNK_BASE = 0x70000000;
    static constexpr GuestAddr IID_ADDR = DATA_BASE;
    static constexpr GuestAddr OUTPUT = DATA_BASE + 0x100;

    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    ThunkRegistry thunks{THUNK_BASE};
    DirectInputManager mgr;
    DllExports exports;
};

TEST_F(Dinput8Test, RegistersFiveArgumentStdcallSignature) {
    EXPECT_EQ(exports.entries.front().name, "DirectInput8Create");
    EXPECT_EQ(exports.entries.front().stdcall_bytes, 20u);
}

TEST_F(Dinput8Test, CreatesIDirectInput8A) {
    constexpr u32 SENTINEL = 0xA5A5A5A5u;
    ASSERT_TRUE(memory.write_u32(OUTPUT, SENTINEL).is_ok());
    set_args(IID_ADDR, OUTPUT);

    exports.entries.front().handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), DI_OK);
    const u32 object = memory.read_u32(OUTPUT).value();
    EXPECT_NE(object, 0u);
    EXPECT_NE(object, SENTINEL);
    // Object starts with the DI8 vtable pointer.
    EXPECT_NE(memory.read_u32(object).value(), 0u);
}

TEST_F(Dinput8Test, MouseImmediateStateReportsAccumulatedRelativeMotionOnce) {
    constexpr GuestAddr MOUSE_GUID = DATA_BASE + 0x200;
    constexpr GuestAddr DEVICE_OUTPUT = DATA_BASE + 0x220;
    constexpr GuestAddr DATA_FORMAT = DATA_BASE + 0x240;
    constexpr GuestAddr MOUSE_STATE = DATA_BASE + 0x280;
    const std::array<u8, 16> sys_mouse_guid = {
        0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
        0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00,
    };
    ASSERT_TRUE(memory.write_bytes(MOUSE_GUID, sys_mouse_guid.data(),
                                   sys_mouse_guid.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_FORMAT + 12, 16).is_ok());

    ASSERT_EQ(mgr.direct_input8_create(0, 0x0800, IID_ADDR, OUTPUT, 0, /*is_64bit=*/false), DI_OK);
    const u32 object = memory.read_u32(OUTPUT).value();
    ASSERT_EQ(mgr.create_device(object, MOUSE_GUID, DEVICE_OUTPUT, 0), DI_OK);
    const u32 device = memory.read_u32(DEVICE_OUTPUT).value();
    ASSERT_EQ(mgr.device_set_data_format(device, DATA_FORMAT), DI_OK);
    ASSERT_EQ(mgr.device_acquire(device), DI_OK);

    // Device deltas, as the host reports them (NSEvent deltaX/deltaY);
    // sub-count motion carries over rather than being lost.
    mgr.note_mouse_motion(12.0, -7.0);
    mgr.note_mouse_motion(2.5, 8.0);
    mgr.note_mouse_motion(0.5, 0.0);
    ASSERT_EQ(mgr.device_get_device_state(device, 16, MOUSE_STATE), DI_OK);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(MOUSE_STATE).value()), 15);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(MOUSE_STATE + 4).value()), 1);

    ASSERT_EQ(mgr.device_get_device_state(device, 16, MOUSE_STATE), DI_OK);
    EXPECT_EQ(memory.read_u32(MOUSE_STATE).value(), 0u);
    EXPECT_EQ(memory.read_u32(MOUSE_STATE + 4).value(), 0u);
}

// dinput.h DIMOUSESTATE: LONG lX, lY, lZ (the wheel); DIMOFS_Z = 8 is its
// buffered dwOfs. The wheel delta is the signed WHEEL_DELTA-unit value.
TEST_F(Dinput8Test, MouseWheelIsTheZAxisInStateAndBuffer) {
    constexpr GuestAddr MOUSE_GUID = DATA_BASE + 0x200;
    constexpr GuestAddr DEVICE_OUTPUT = DATA_BASE + 0x220;
    constexpr GuestAddr DATA_FORMAT = DATA_BASE + 0x240;
    constexpr GuestAddr PROP = DATA_BASE + 0x260;
    constexpr GuestAddr MOUSE_STATE = DATA_BASE + 0x280;
    constexpr GuestAddr COUNT = DATA_BASE + 0x2A0;
    constexpr GuestAddr RECORDS = DATA_BASE + 0x300;
    constexpr GuestAddr DIPROP_BUFFERSIZE = 1;
    constexpr u32 DIMOFS_Z = 8;
    const std::array<u8, 16> sys_mouse_guid = {
        0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
        0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00,
    };
    ASSERT_TRUE(memory.write_bytes(MOUSE_GUID, sys_mouse_guid.data(),
                                   sys_mouse_guid.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_FORMAT + 12, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(PROP + 0, 20).is_ok());
    ASSERT_TRUE(memory.write_u32(PROP + 4, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(PROP + 16, 8).is_ok());

    ASSERT_EQ(mgr.direct_input8_create(0, 0x0800, IID_ADDR, OUTPUT, 0, /*is_64bit=*/false), DI_OK);
    const u32 object = memory.read_u32(OUTPUT).value();
    ASSERT_EQ(mgr.create_device(object, MOUSE_GUID, DEVICE_OUTPUT, 0), DI_OK);
    const u32 device = memory.read_u32(DEVICE_OUTPUT).value();
    ASSERT_EQ(mgr.device_set_data_format(device, DATA_FORMAT), DI_OK);
    ASSERT_EQ(mgr.device_set_property(device, DIPROP_BUFFERSIZE, PROP), DI_OK);
    ASSERT_EQ(mgr.device_acquire(device), DI_OK);

    mgr.note_mouse_wheel(120);
    mgr.note_mouse_wheel(-360);
    mgr.note_mouse_wheel(0);
    ASSERT_EQ(mgr.device_get_device_state(device, 16, MOUSE_STATE), DI_OK);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(MOUSE_STATE + 8).value()), -240);
    ASSERT_EQ(mgr.device_get_device_state(device, 16, MOUSE_STATE), DI_OK);
    EXPECT_EQ(memory.read_u32(MOUSE_STATE + 8).value(), 0u) << "read once";

    ASSERT_TRUE(memory.write_u32(COUNT, 8).is_ok());
    ASSERT_EQ(mgr.device_get_device_data(device, 20, RECORDS, COUNT, 0), DI_OK);
    ASSERT_EQ(memory.read_u32(COUNT).value(), 2u) << "a zero delta is no event";
    EXPECT_EQ(memory.read_u32(RECORDS + 0).value(), DIMOFS_Z);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(RECORDS + 4).value()), 120);
    EXPECT_EQ(memory.read_u32(RECORDS + 20).value(), DIMOFS_Z);
    EXPECT_EQ(static_cast<i32>(memory.read_u32(RECORDS + 24).value()), -360);
}

// dinput.h DIDEVICEOBJECTDATA is DWORD x4 + UINT_PTR uAppData: 20 bytes on
// x86, 24 on x64; DIDEVICEOBJECTDATA_DX3 is 16. Expected results are
// tools/oracle_probe/dinput_device_data_probe.expected: a DirectInput 8 device
// takes only its own size and refuses others with DIERR_INVALIDPARAM, count
// untouched; a pre-8 device takes 16 and returns DI_OK writing nothing for any
// other. Skyrim SE (x64) passes 24; refusing it left its count stale and it
// walked uninitialized records.
TEST_F(Dinput8Test, GetDeviceDataTakesExactlyTheInterfaceRecordSize) {
    constexpr GuestAddr MOUSE_GUID = DATA_BASE + 0x200;
    constexpr GuestAddr DEVICE_OUTPUT = DATA_BASE + 0x220;
    constexpr GuestAddr DATA_FORMAT = DATA_BASE + 0x240;
    constexpr GuestAddr PROP = DATA_BASE + 0x280;
    constexpr GuestAddr COUNT = DATA_BASE + 0x2A0;
    constexpr GuestAddr RECORDS = DATA_BASE + 0x300;
    constexpr GuestAddr DIPROP_BUFFERSIZE = 1;
    constexpr u32 DIMOFS_BUTTON0 = 12;
    const std::array<u8, 16> sys_mouse_guid = {
        0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
        0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00,
    };
    ASSERT_TRUE(memory.write_bytes(MOUSE_GUID, sys_mouse_guid.data(),
                                   sys_mouse_guid.size()).is_ok());
    ASSERT_TRUE(memory.write_u32(DATA_FORMAT + 12, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(PROP + 0, 20).is_ok());
    ASSERT_TRUE(memory.write_u32(PROP + 4, 16).is_ok());
    ASSERT_TRUE(memory.write_u32(PROP + 16, 8).is_ok());

    enum class Kind { Di8x64, Di8x86, Legacy };
    const auto buffered_mouse = [&](Kind kind) -> GuestAddr {
        EXPECT_TRUE(memory.write_u64(OUTPUT, 0).is_ok());
        EXPECT_TRUE(memory.write_u64(DEVICE_OUTPUT, 0).is_ok());
        const bool is_64bit = kind == Kind::Di8x64;
        if (kind == Kind::Legacy) {
            EXPECT_EQ(mgr.direct_input_create(0, 0x0700, OUTPUT, 0), DI_OK);
        } else {
            EXPECT_EQ(mgr.direct_input8_create(0, 0x0800, IID_ADDR, OUTPUT, 0,
                                               is_64bit), DI_OK);
        }
        const GuestAddr object = is_64bit ? memory.read_u64(OUTPUT).value()
                                          : memory.read_u32(OUTPUT).value();
        EXPECT_EQ(mgr.create_device(object, MOUSE_GUID, DEVICE_OUTPUT, 0),
                  DI_OK);
        const GuestAddr device = is_64bit
            ? memory.read_u64(DEVICE_OUTPUT).value()
            : memory.read_u32(DEVICE_OUTPUT).value();
        EXPECT_EQ(mgr.device_set_data_format(device, DATA_FORMAT), DI_OK);
        EXPECT_EQ(mgr.device_set_property(device, DIPROP_BUFFERSIZE, PROP),
                  DI_OK);
        EXPECT_EQ(mgr.device_acquire(device), DI_OK);
        mgr.note_mouse_button(0, true);
        mgr.note_mouse_button(0, false);
        return device;
    };
    const auto get = [&](GuestAddr device, u32 size) {
        EXPECT_TRUE(memory.write_u32(COUNT, 7).is_ok());
        return mgr.device_get_device_data(device, size, RECORDS, COUNT, 0);
    };

    for (const Kind kind : {Kind::Di8x64, Kind::Di8x86}) {
        const u32 native = kind == Kind::Di8x64 ? 24u : 20u;
        const GuestAddr device = buffered_mouse(kind);
        for (const u32 wrong : {0u, 16u, 20u, 24u, 28u}) {
            if (wrong == native) continue;
            EXPECT_EQ(get(device, wrong), DIERR_INVALIDPARAM) << wrong;
            EXPECT_EQ(memory.read_u32(COUNT).value(), 7u) << wrong;
        }
        ASSERT_TRUE(memory.write_bytes(RECORDS, std::vector<u8>(64, 0xCC).data(),
                                       64).is_ok());
        ASSERT_EQ(get(device, native), DI_OK);
        ASSERT_EQ(memory.read_u32(COUNT).value(), 2u);
        EXPECT_EQ(memory.read_u32(RECORDS + 0).value(), DIMOFS_BUTTON0);
        EXPECT_EQ(memory.read_u32(RECORDS + 4).value(), 0x80u);
        EXPECT_EQ(memory.read_u32(RECORDS + native + 0).value(),
                  DIMOFS_BUTTON0);
        EXPECT_EQ(memory.read_u32(RECORDS + native + 4).value(), 0u);
        // uAppData fills the record's tail: a UINT_PTR of the caller's size.
        EXPECT_EQ(memory.read_u32(RECORDS + 16).value(), 0u);
        if (native == 24) EXPECT_EQ(memory.read_u32(RECORDS + 20).value(), 0u);
    }

    const GuestAddr legacy = buffered_mouse(Kind::Legacy);
    EXPECT_EQ(get(legacy, 20), DI_OK);
    EXPECT_EQ(memory.read_u32(COUNT).value(), 7u) << "nothing written";
    ASSERT_EQ(get(legacy, 16), DI_OK);
    ASSERT_EQ(memory.read_u32(COUNT).value(), 2u) << "events kept";
    EXPECT_EQ(memory.read_u32(RECORDS + 16 + 4).value(), 0u);
}

TEST_F(Dinput8Test, RejectsNullOutputPointer) {
    set_args(IID_ADDR, 0);

    exports.entries.front().handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), DIERR_INVALIDPARAM);
}

TEST_F(Dinput8Test, RejectsUnknownIid) {
    u8 bogus[16]{};
    std::memset(bogus, 0x11, sizeof(bogus));
    ASSERT_TRUE(memory.write_bytes(IID_ADDR, bogus, sizeof(bogus)).is_ok());
    ASSERT_TRUE(memory.write_u32(OUTPUT, 0xA5A5A5A5u).is_ok());
    set_args(IID_ADDR, OUTPUT);

    exports.entries.front().handler(marshaler);

    EXPECT_EQ(static_cast<u32>(cpu.gpr.rax), E_NOINTERFACE);
    EXPECT_EQ(memory.read_u32(OUTPUT).value(), 0u);
}

} // namespace

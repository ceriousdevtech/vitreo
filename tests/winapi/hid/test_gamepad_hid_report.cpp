#include "background_input_test_host.hpp"
// SPDX-License-Identifier: LGPL-2.1-only
#include "winapi/common/gamepad_hid_report.hpp"
#include <gtest/gtest.h>
using namespace vitreo;
using namespace vitreo::winapi;
namespace {
u16 read16(const gamepad_hid::Bytes& b, usize at) {
    return static_cast<u16>(u16(b.at(at)) | (u16(b.at(at + 1)) << 8));
}
}
TEST(GamepadHidReport, NeutralAndExtremeFramesMatchPublishedDeviceProtocol) {
    hardware::GamepadState state{}; state.connected = true;
    // Normative byte layout in .docs/contracts/virtual-gamepad-hid.md.
    const gamepad_hid::Report neutral{1, 0,128, 255,127, 0,128, 255,127, 0,128, 0,0, 8};
    EXPECT_EQ(gamepad_hid::encode(state), neutral);
    state.left_x = -32768; state.left_y = 32767;
    state.right_x = 32767; state.right_y = -32768;
    state.right_trigger = 255;
    // Microsoft's XUSB mapping: A=button1, LB=5, Back=7, RSB=10.
    state.buttons = hid_sdk::kXINPUT_GAMEPAD_A | hid_sdk::kXINPUT_GAMEPAD_LEFT_SHOULDER |
        hid_sdk::kXINPUT_GAMEPAD_BACK | hid_sdk::kXINPUT_GAMEPAD_RIGHT_THUMB |
        hid_sdk::kXINPUT_GAMEPAD_DPAD_UP | hid_sdk::kXINPUT_GAMEPAD_DPAD_RIGHT;
    const gamepad_hid::Report extreme{1, 0,0, 0,0, 255,255, 255,255, 255,255, 81,2, 1};
    EXPECT_EQ(gamepad_hid::encode(state), extreme);
    state.right_trigger = 0; state.left_trigger = 255;
    auto left = gamepad_hid::encode(state);
    EXPECT_EQ(left[9], 0); EXPECT_EQ(left[10], 0);
    state.right_trigger = 255;
    auto both = gamepad_hid::encode(state);
    EXPECT_EQ(both[9], 0); EXPECT_EQ(both[10], 128);
}
TEST(GamepadHidReport, CapabilitiesUseOracleSdkLayoutsAndAdvertiseOnlyImplementedReports) {
    for (bool is64 : {false, true}) {
        const auto& l = is64 ? hid_sdk::x64 : hid_sdk::x86;
        const auto caps = gamepad_hid::capabilities(is64);
        ASSERT_EQ(caps.size(), l.sizeof_HIDP_CAPS);
        EXPECT_EQ(read16(caps,l.offsetof_HIDP_CAPS_Usage), hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
        EXPECT_EQ(read16(caps,l.offsetof_HIDP_CAPS_InputReportByteLength), 14);
        EXPECT_EQ(read16(caps,l.offsetof_HIDP_CAPS_OutputReportByteLength), 0);
        EXPECT_EQ(read16(caps,l.offsetof_HIDP_CAPS_FeatureReportByteLength), 0);
        EXPECT_EQ(read16(caps,l.offsetof_HIDP_CAPS_NumberInputButtonCaps), 1);
        EXPECT_EQ(read16(caps,l.offsetof_HIDP_CAPS_NumberInputValueCaps), 6);
        EXPECT_EQ(read16(caps,l.offsetof_HIDP_CAPS_NumberInputDataIndices), 16);
        const auto buttons = gamepad_hid::button_capabilities(is64);
        EXPECT_EQ(read16(buttons,l.offsetof_HIDP_BUTTON_CAPS_Range_UsageMin), 1);
        EXPECT_EQ(read16(buttons,l.offsetof_HIDP_BUTTON_CAPS_Range_UsageMax), 10);
        EXPECT_EQ(read16(buttons,l.offsetof_HIDP_BUTTON_CAPS_Range_DataIndexMin), 5);
        EXPECT_EQ(read16(buttons,l.offsetof_HIDP_BUTTON_CAPS_Range_DataIndexMax), 14);
        EXPECT_EQ(read16(buttons,l.offsetof_HIDP_BUTTON_CAPS_ReportCount), 1); // observed variable-button range
        const auto hat = gamepad_hid::value_capabilities(is64,gamepad_hid::kValues[5]);
        EXPECT_EQ(read16(hat,l.offsetof_HIDP_VALUE_CAPS_NotRange_Usage), hid_sdk::kHID_USAGE_GENERIC_HATSWITCH);
        EXPECT_EQ(read16(hat,l.offsetof_HIDP_VALUE_CAPS_NotRange_DataIndex), 15);
        EXPECT_EQ(hat.at(l.offsetof_HIDP_VALUE_CAPS_HasNull), 1);
        EXPECT_EQ(read16(hat,l.offsetof_HIDP_VALUE_CAPS_BitField), 66u); // USB HID Data/Variable/Absolute/Null

    }
}
TEST(GamepadHidReport, OpaqueCollectionDefinitionIsCopyableAndRejectsCorruption) {
    auto copy = gamepad_hid::kPreparsedData;
    EXPECT_TRUE(gamepad_hid::recognizes(copy));
    copy[0] ^= 1;
    EXPECT_FALSE(gamepad_hid::recognizes(copy));
    EXPECT_FALSE(gamepad_hid::recognizes({}));
}

TEST(GamepadHidReport, ParserReadsPublishedFieldsAndDoesNotTreatPaddingAsButtons) {
    // A literal protocol frame, independent of the producer implementation.
    const gamepad_hid::Report report{1, 0x34,0x12, 0x78,0x56, 0xbc,0x9a,
        0xf0,0xde, 0xff,0xff, 0x81,0xfe, 8};
    constexpr u32 expected[]{0x1234,0x5678,0x9abc,0xdef0,0xffff,8};
    for (usize i=0;i<std::size(expected);++i)
        EXPECT_EQ(gamepad_hid::read_value(report,gamepad_hid::kValues[i]),expected[i]);
    for (u16 usage=1;usage<=10;++usage)
        EXPECT_EQ(gamepad_hid::button_pressed(report,usage),
                  usage==1 || usage==8 || usage==10);
    EXPECT_FALSE(gamepad_hid::button_pressed(report,0));
    EXPECT_FALSE(gamepad_hid::button_pressed(report,11));
    EXPECT_EQ(gamepad_hid::find_value(hid_sdk::kHID_USAGE_GENERIC_X),&gamepad_hid::kValues[0]);
    EXPECT_EQ(gamepad_hid::find_value(0),nullptr);
}
TEST(GamepadHidReport, ParserWritesOnlySelectedRawFieldBits) {
    gamepad_hid::Report report{};
    report[0]=1; report[11]=0xa5; report[12]=0xfc;
    gamepad_hid::write_value(report,gamepad_hid::kValues[0],0x12345678);
    gamepad_hid::write_value(report,gamepad_hid::kValues[5],0x123456ff);
    EXPECT_EQ(report[1],0x78); EXPECT_EQ(report[2],0x56);
    EXPECT_EQ(report[13],0xff);
    EXPECT_TRUE(gamepad_hid::write_button(report,10,true));
    EXPECT_TRUE(gamepad_hid::write_button(report,1,false));
    const gamepad_hid::Report expected{1,0x78,0x56,0,0,0,0,0,0,0,0,0xa4,0xfe,0xff};
    EXPECT_EQ(report,expected);
    EXPECT_FALSE(gamepad_hid::write_button(report,0,true));
    EXPECT_FALSE(gamepad_hid::write_button(report,11,false));
    EXPECT_EQ(report,expected);
}

#include "winapi/common/gamepad_hid_catalog.hpp"
namespace {
class SnapshotHost final : public TestHidHost {
public:
    hardware::GamepadState state{};
    bool works{true};
    bool query_state(u32 slot,hardware::GamepadState& out) override {
        if(!works) return false;
        out=slot==0 ? state : hardware::GamepadState{};return true;
    }
};
}
TEST(GamepadHidCatalog, RealSnapshotsIndependentQueuesAndReplacementIdentity) {
    auto host=std::make_shared<SnapshotHost>();
    GamepadHidCatalog catalog(host);
    EXPECT_TRUE(catalog.devices().empty());
    host->state.connected=true;
    auto devices=catalog.devices();ASSERT_EQ(devices.size(),1);
    auto first=catalog.open(devices[0].path(),0x80000000u,false);
    auto second=catalog.open(devices[0].path(),0x80000000u,true);
    ASSERT_TRUE(first);ASSERT_TRUE(second);
    // FIFO and per-file-object ownership are the HIDClass contract. A copied
    // holder is DuplicateHandle; a second open retains its independent FIFO.
    auto duplicate=first;
    first->reports.clear();EXPECT_TRUE(duplicate->reports.empty());EXPECT_EQ(second->reports.size(),1);
    host->state.buttons=hid_sdk::kXINPUT_GAMEPAD_A;
    (void)catalog.devices();
    ASSERT_EQ(duplicate->reports.size(),1);ASSERT_EQ(second->reports.size(),2);
    EXPECT_EQ(duplicate->reports.front()[11],1); // original normative button1
    const auto old_identity=devices[0].raw_handle();const auto old_path=devices[0].path();
    host->state.connected=false;EXPECT_TRUE(catalog.devices().empty());
    EXPECT_TRUE(first->disconnected);EXPECT_TRUE(first->reports.empty());
    host->state.connected=true;devices=catalog.devices();ASSERT_EQ(devices.size(),1);
    EXPECT_NE(devices[0].raw_handle(),old_identity);
    EXPECT_FALSE(catalog.open(old_path,0,false));
    EXPECT_FALSE(catalog.snapshot(first));
}
TEST(GamepadHidCatalog, SharingAndHostFailureDoNotFabricateInput) {
    auto host=std::make_shared<SnapshotHost>();host->state.connected=true;
    GamepadHidCatalog catalog(host);auto devices=catalog.devices();ASSERT_EQ(devices.size(),1);
    auto first=catalog.open(devices[0].path(),0x80000000u,false,0);ASSERT_TRUE(first);
    bool conflict=false;
    EXPECT_FALSE(catalog.open(devices[0].path(),0x80000000u,false,3,&conflict));EXPECT_TRUE(conflict);
    // Access-zero metadata does not conflict with sharing (CreateFile docs).
    EXPECT_TRUE(catalog.open(devices[0].path(),0,false,3));
    first->reports.clear();host->works=false;host->state.buttons=hid_sdk::kXINPUT_GAMEPAD_B;
    EXPECT_EQ(catalog.devices().size(),1);EXPECT_TRUE(first->reports.empty());
    bool failed=false;EXPECT_FALSE(catalog.snapshot(first,&failed));EXPECT_TRUE(failed);
    host->works=true;(void)catalog.devices();ASSERT_EQ(first->reports.size(),1);
    EXPECT_EQ(first->reports.front()[11],2);
}
TEST(GamepadHidCatalog, NativeIdentityChangeRetiresOpenEvenWithIdenticalInput) {
    auto host=std::make_shared<SnapshotHost>();host->state.connected=true;host->state.connection_id=10;
    GamepadHidCatalog catalog(host);auto first=catalog.devices();ASSERT_EQ(first.size(),1);
    auto file=catalog.open(first[0].path(),0x80000000u,false);ASSERT_TRUE(file);
    const auto old=host->state;host->state.connection_id=11;
    // Windows input packet equality deliberately excludes native identity.
    EXPECT_EQ(old,host->state);
    const auto replacement=catalog.devices();ASSERT_EQ(replacement.size(),1);
    EXPECT_NE(first[0].generation,replacement[0].generation);
    EXPECT_TRUE(file->disconnected);EXPECT_TRUE(file->reports.empty());EXPECT_FALSE(catalog.snapshot(file));
}
// HIDClass collection I/O has no User32 activation/acquisition prerequisite.
TEST(GamepadHidCatalog, FileConsumersOwnBackgroundMonitoringAndKeepActualInput) {
    auto host=std::make_shared<SnapshotHost>();host->state.connected=true;
    GamepadHidCatalog catalog(host);
    auto devices=catalog.devices();ASSERT_EQ(devices.size(),1);
    EXPECT_EQ(host->monitors->load(),0u);
    auto metadata=catalog.open(devices[0].path(),0,false);ASSERT_TRUE(metadata);
    EXPECT_EQ(host->monitors->load(),0u);
    auto file=catalog.open(devices[0].path(),0x80000000u,false);ASSERT_TRUE(file);
    EXPECT_EQ(host->monitors->load(),1u);ASSERT_EQ(file->reports.size(),1);
    auto duplicate=file;file.reset();EXPECT_EQ(host->monitors->load(),1u);
    duplicate->reports.clear();host->state.buttons=hid_sdk::kXINPUT_GAMEPAD_B;
    ASSERT_TRUE(catalog.snapshot(metadata));EXPECT_EQ(host->monitors->load(),1u);
    ASSERT_EQ(duplicate->reports.size(),1);EXPECT_EQ(duplicate->reports.front()[11],2);
    duplicate.reset();EXPECT_EQ(host->monitors->load(),0u);
    EXPECT_TRUE(catalog.snapshot(metadata));EXPECT_EQ(host->monitors->load(),0u);
    host->monitoring_supported=false;
    bool unsupported=false;EXPECT_FALSE(catalog.snapshot(metadata,nullptr,&unsupported));EXPECT_TRUE(unsupported);
}

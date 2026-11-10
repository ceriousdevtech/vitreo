// SPDX-License-Identifier: LGPL-2.1-only
#include "../user32/user32_ext_thunk_harness.hpp"
#include "winapi/common/gamepad_hid_catalog.hpp"
namespace {
class RawHidHost final : public hardware::GamepadHost {
public:
    std::atomic<u32> queries{0};
    std::atomic<u16> buttons{hid_sdk::kXINPUT_GAMEPAD_A};
    bool query_state(u32 slot,hardware::GamepadState& out) override {
        ++queries;out={};out.connected=slot==0;out.buttons=buttons.load();return true;
    }
};
}
TEST_F(User32ExtThunkTest, NormalizedGamepadDiscoveryMetadataAndRawReportsBothBitnesses) {
    auto foreground=std::make_shared<std::atomic<bool>>(false);
    auto host=std::make_shared<RawHidHost>();
    auto catalog=std::make_shared<GamepadHidCatalog>(host);
    mgr_->set_gamepad_foreground_state(foreground);
    mgr_->set_gamepad_catalog(catalog);
    WindowData window{};window.parent=HWND_DESKTOP;window.rect={0,0,640,480};window.client_rect=window.rect;
    const auto hwnd=hwnd_mgr_.create(window);ASSERT_NE(hwnd,NULL_HWND);
    ASSERT_TRUE(mgr_->set_application_active(hwnd,true));EXPECT_TRUE(foreground->load());
    for(bool is64:{false,true}) {
        cpu_.mode=is64 ? cpu::CpuMode::Long : cpu::CpuMode::Protected;mgr_->set_is_64bit(is64);
        Marshaler m(memory_);m.attach_cpu(cpu_);
        auto args=[&](std::initializer_list<u64> values) {
            usize i=0;for(u64 value:values) {
                if(is64) {
                    if(i==0) cpu_.gpr.rcx=value;else if(i==1)cpu_.gpr.rdx=value;
                    else if(i==2)cpu_.gpr.r8=value;else if(i==3)cpu_.gpr.r9=value;
                    else ASSERT_TRUE(memory_.write_u64(cpu_.sp()+40+(i-4)*8,value).is_ok());
                } else ASSERT_TRUE(memory_.write_u32(cpu_.sp()+4+i*4,static_cast<u32>(value)).is_ok());
                ++i;
            }
        };
        ASSERT_TRUE(memory_.write_u32(0x4000,8).is_ok());args({0x2000,0x4000,is64 ? 16u:8u});mgr_->impl_GetRawInputDeviceList(m);
        ASSERT_EQ(cpu_.gpr.rax,3u);EXPECT_EQ(memory_.read_u32(0x4000).value(),8u);
        ASSERT_TRUE(memory_.protect_region(0x4000,0x1000,MemoryProtection::Read).is_ok());
        args({0x2000,0x4000,is64 ? 16u:8u});mgr_->impl_GetRawInputDeviceList(m);EXPECT_EQ(cpu_.gpr.rax,3u);
        EXPECT_EQ(memory_.read_u32(0x4000).value(),8u);
        args({0,0x4000,is64 ? 16u:8u});mgr_->impl_GetRawInputDeviceList(m);EXPECT_EQ(cpu_.gpr.rax,0xffffffffu);EXPECT_EQ(get_last_error(),998u);
        ASSERT_TRUE(memory_.protect_region(0x4000,0x1000,MemoryProtection::ReadWrite).is_ok());
        const GuestAddr entry=0x2000+2*(is64 ? 16:8);
        const u64 device=is64 ? memory_.read_u64(entry).value() : memory_.read_u32(entry).value();
        EXPECT_EQ(device,catalog->devices()[0].raw_handle());
        ASSERT_TRUE(memory_.write_u32(0x4000,128).is_ok());args({device,hid_sdk::kRIDI_PREPARSEDDATA,0x3000,0x4000});mgr_->impl_GetRawInputDeviceInfoW(m);
        EXPECT_EQ(cpu_.gpr.rax,32u);std::array<u8,32> pp{};ASSERT_TRUE(memory_.read_bytes(0x3000,pp.data(),pp.size()).is_ok());EXPECT_TRUE(gamepad_hid::recognizes(pp));
        ASSERT_TRUE(memory_.write_u32(0x4000,128).is_ok());args({device,hid_sdk::kRIDI_DEVICENAME,0x7000,0x4000});mgr_->impl_GetRawInputDeviceInfoW(m);
        const auto path=m.read_wide_string(0x7000);ASSERT_TRUE(path.is_ok());EXPECT_EQ(path.value(),catalog->devices()[0].path());
        EXPECT_NE(path.value().find("IG_"),std::string::npos);
        ASSERT_TRUE(memory_.write_u16(0x5000,1).is_ok());ASSERT_TRUE(memory_.write_u16(0x5002,5).is_ok());
        ASSERT_TRUE(memory_.write_u32(0x5004,0).is_ok());
        if(is64) ASSERT_TRUE(memory_.write_u64(0x5008,hwnd).is_ok());else ASSERT_TRUE(memory_.write_u32(0x5008,hwnd).is_ok());
        args({0x5000,1,is64 ? 16u:12u});mgr_->impl_RegisterRawInputDevices(m);ASSERT_EQ(cpu_.gpr.rax,1u);
        mgr_->poll_raw_gamepads();
        MSG message{};ASSERT_TRUE(queue_.peek(message,hwnd,WM::INPUT,WM::INPUT,PM::REMOVE));
        ASSERT_TRUE(memory_.write_u32(0x4000,128).is_ok());args({message.lParam,0x10000003u,0x6000,0x4000,is64 ? 24u:16u});mgr_->impl_GetRawInputData(m);
        ASSERT_EQ(cpu_.gpr.rax,(is64 ? 24u:16u)+22u);EXPECT_EQ(memory_.read_u32(0x6000).value(),2u);
        const GuestAddr body=0x6000+(is64 ? 24u:16u);
        EXPECT_EQ(memory_.read_u32(body).value(),14u);EXPECT_EQ(memory_.read_u32(body+4).value(),1u);
        EXPECT_EQ(memory_.read_u8(body+8).value(),1u);EXPECT_EQ(memory_.read_u8(body+19).value(),1u);
        mgr_->note_removed_raw_input_message(message);
    }
    EXPECT_TRUE(mgr_->set_application_active(hwnd,false));EXPECT_FALSE(foreground->load());
    EXPECT_EQ(catalog->devices().size(),1u); // deactivation does not hide configuration
    mgr_->set_gamepad_catalog({}); // join the producer before deterministic manual polling
    mgr_->set_gamepad_catalog(catalog);
    MSG pending{};while(queue_.peek(pending,hwnd,WM::INPUT,WM::INPUT,PM::REMOVE)) mgr_->note_removed_raw_input_message(pending);
    host->buttons=hid_sdk::kXINPUT_GAMEPAD_B;
    mgr_->poll_raw_gamepads();
    EXPECT_FALSE(queue_.peek(pending,hwnd,WM::INPUT,WM::INPUT,PM::REMOVE));
    EXPECT_TRUE(mgr_->set_application_active(hwnd,true));
    mgr_->poll_raw_gamepads();
    ASSERT_TRUE(queue_.peek(pending,hwnd,WM::INPUT,WM::INPUT,PM::REMOVE));
    mgr_->note_removed_raw_input_message(pending);
    mgr_->set_gamepad_catalog({});
}

TEST_F(User32ExtThunkTest, IdleGamepadCatalogDoesNotPollNativeHostWithoutSubscription) {
    auto host=std::make_shared<RawHidHost>();
    mgr_->set_gamepad_catalog(std::make_shared<GamepadHidCatalog>(host));
    std::this_thread::sleep_for(std::chrono::milliseconds(32));
    EXPECT_EQ(host->queries.load(),0u);
    mgr_->set_gamepad_catalog({});
}

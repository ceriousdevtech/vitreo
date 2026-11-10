#include "background_input_test_host.hpp"
// SPDX-License-Identifier: LGPL-2.1-only
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "winapi/common/gamepad_hid_report.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/system/hid/hid.hpp"
#include <gtest/gtest.h>
using namespace vitreo;
using namespace vitreo::winapi;

class GamepadHidParser : public ::testing::Test {
protected:
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler marshaler{memory};
    DllExports exports = create_hid_thunks();
    void setup(bool is64) {
        ASSERT_TRUE(memory.map_region(0x10000, 0x10000, MemoryProtection::ReadWrite).is_ok());
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x1f000;
        marshaler.attach_cpu(cpu);
        ASSERT_TRUE(memory.write_bytes(0x11000, gamepad_hid::kPreparsedData.data(),
                                       gamepad_hid::kPreparsedData.size()).is_ok());
    }
    u32 call(const char* name, std::initializer_list<u64> args) {
        usize i = 0;
        for (u64 arg : args) {
            if (cpu.is_64bit()) {
                if (i == 0) cpu.gpr.rcx=arg;
                else if (i == 1) cpu.gpr.rdx=arg;
                else if (i == 2) cpu.gpr.r8=arg;
                else if (i == 3) cpu.gpr.r9=arg;
                else EXPECT_TRUE(memory.write_u64(cpu.sp()+40+(i-4)*8,arg).is_ok());
            } else EXPECT_TRUE(memory.write_u32(cpu.sp()+4+i*4,static_cast<u32>(arg)).is_ok());
            ++i;
        }
        for (auto& entry : exports.entries) if (entry.name==name) {
            entry.handler(marshaler); return static_cast<u32>(cpu.gpr.rax);
        }
        ADD_FAILURE() << name; return 0;
    }
    void verify_caps(bool is64) {
        setup(is64);
        set_last_error(0xdead);
        // Audit the exported interface GUID against independently generated
        // SDK words, including the previously literal implementation array.
        (void)call("HidD_GetHidGuid",{0x14000});
        EXPECT_EQ(memory.read_u32(0x14000).value(),hid_sdk::kGUID_DEVINTERFACE_HID_Data1);
        EXPECT_EQ(memory.read_u16(0x14004).value(),hid_sdk::kGUID_DEVINTERFACE_HID_Data2);
        EXPECT_EQ(memory.read_u16(0x14006).value(),hid_sdk::kGUID_DEVINTERFACE_HID_Data3);
        const u32 tail[]={hid_sdk::kGUID_DEVINTERFACE_HID_Data4_0,hid_sdk::kGUID_DEVINTERFACE_HID_Data4_1,
            hid_sdk::kGUID_DEVINTERFACE_HID_Data4_2,hid_sdk::kGUID_DEVINTERFACE_HID_Data4_3,
            hid_sdk::kGUID_DEVINTERFACE_HID_Data4_4,hid_sdk::kGUID_DEVINTERFACE_HID_Data4_5,
            hid_sdk::kGUID_DEVINTERFACE_HID_Data4_6,hid_sdk::kGUID_DEVINTERFACE_HID_Data4_7};
        for(usize i=0;i<8;++i) EXPECT_EQ(memory.read_u8(0x14008+i).value(),tail[i]);
        // Microsoft HidP_GetCaps contract + SDK ABI + published collection.
        EXPECT_EQ(call("HidP_GetCaps", {0x11000,0x12000}),hid_sdk::kHIDP_STATUS_SUCCESS);
        EXPECT_EQ(get_last_error(),0xdeadu);
        const auto& l=gamepad_hid::layout(is64);
        EXPECT_EQ(memory.read_u16(0x12000+l.offsetof_HIDP_CAPS_Usage).value(),
                  hid_sdk::kHID_USAGE_GENERIC_GAMEPAD);
        EXPECT_EQ(memory.read_u16(0x12000+l.offsetof_HIDP_CAPS_InputReportByteLength).value(),14);
        EXPECT_EQ(memory.read_u16(0x12000+l.offsetof_HIDP_CAPS_NumberInputDataIndices).value(),16);
        // Original collection blob remains valid after a guest byte-for-byte copy.
        EXPECT_TRUE(memory.write_bytes(0x13000,gamepad_hid::kPreparsedData.data(),32).is_ok());
        EXPECT_EQ(call("HidP_GetCaps",{0x13000,0x12000}),hid_sdk::kHIDP_STATUS_SUCCESS);
        EXPECT_TRUE(memory.write_u8(0x13000,0).is_ok());
        EXPECT_TRUE(memory.write_u32(0x12000,0xfeedcafe).is_ok());
        EXPECT_EQ(call("HidP_GetCaps",{0x13000,0x12000}),hid_sdk::kHIDP_STATUS_INVALID_PREPARSED_DATA);
        EXPECT_EQ(memory.read_u32(0x12000).value(),0xfeedcafeu);
        EXPECT_EQ(get_last_error(),0xdeadu);
        // hidpi.h Max*ListLength: maxima for an existing report; zero for
        // absent report types, unmatched pages, invalid enums or preparsed data.
        EXPECT_EQ(call("HidP_MaxDataListLength",{hid_sdk::kHidP_Input,0x11000}),16u);
        EXPECT_EQ(call("HidP_MaxUsageListLength",{hid_sdk::kHidP_Input,0,0x11000}),10u);
        EXPECT_EQ(call("HidP_MaxUsageListLength",{hid_sdk::kHidP_Input,
                  hid_sdk::kHID_USAGE_PAGE_BUTTON,0x11000}),10u);
        EXPECT_EQ(call("HidP_MaxUsageListLength",{hid_sdk::kHidP_Input,
                  hid_sdk::kHID_USAGE_PAGE_GENERIC,0x11000}),0u);
        for (u32 type : {1u,2u,3u,0xffffffffu}) {
            EXPECT_EQ(call("HidP_MaxDataListLength",{type,0x11000}),0u);
            EXPECT_EQ(call("HidP_MaxUsageListLength",{type,0,0x11000}),0u);
        }
        EXPECT_EQ(call("HidP_MaxDataListLength",{0,0x13000}),0u);
        EXPECT_EQ(call("HidP_MaxUsageListLength",{0,0,0x13000}),0u);
        EXPECT_EQ(get_last_error(),0xdeadu);
    }
    void verify_array_caps(bool is64) {
        setup(is64);
        constexpr GuestAddr out=0x12000, length=0x14000;
        std::array<u8,72*8> sentinel{}; sentinel.fill(0xa5);
        for (bool value : {false,true}) for (bool specific : {false,true}) {
            const char* name=value ? (specific?"HidP_GetSpecificValueCaps":"HidP_GetValueCaps") :
                                    (specific?"HidP_GetSpecificButtonCaps":"HidP_GetButtonCaps");
            const u16 required=value?6:1;
            auto invoke=[&](u32 type,u16 page,u16 link,u16 usage) {
                return specific ? call(name,{type,page,link,usage,out,length,0x11000}) :
                                  call(name,{type,out,length,0x11000});
            };
            for (u16 capacity : {u16(0),u16(1),u16(8)}) {
                ASSERT_TRUE(memory.write_bytes(out,sentinel.data(),sentinel.size()).is_ok());
                ASSERT_TRUE(memory.write_u16(length,capacity).is_ok());
                set_last_error(0xdead);
                EXPECT_EQ(invoke(0,0,0,0),capacity<required ?
                    hid_sdk::kHIDP_STATUS_BUFFER_TOO_SMALL : hid_sdk::kHIDP_STATUS_SUCCESS);
                EXPECT_EQ(memory.read_u16(length).value(),required);
                EXPECT_EQ(get_last_error(),0xdeadu);
                const u8* mask=value?hid_sdk::kValueCapsWriteMask:hid_sdk::kButtonCapsWriteMask;
                // Write masks come from real Windows calls, including partial
                // success and preserved Reserved[] fields in either bitness.
                for (usize i=0;i<8;++i) for (usize byte=0;byte<72;++byte) {
                    const bool written=i<std::min(capacity,required) && mask[byte];
                    const auto actual=memory.read_u8(out+i*72+byte).value();
                    if (!written) EXPECT_EQ(actual,0xa5) << name << " row " << i << " byte " << byte;
                }
                if (capacity) {
                    const auto& l=gamepad_hid::layout(is64);
                    EXPECT_EQ(memory.read_u16(out+(value?l.offsetof_HIDP_VALUE_CAPS_BitField:
                                                         l.offsetof_HIDP_BUTTON_CAPS_BitField)).value(),2);
                    EXPECT_EQ(memory.read_u16(out+(value?l.offsetof_HIDP_VALUE_CAPS_UsagePage:
                                                         l.offsetof_HIDP_BUTTON_CAPS_UsagePage)).value(),value?1:9);
                }
            }
            for (u32 type : {1u,2u,3u,0xffffffffu}) {
                ASSERT_TRUE(memory.write_bytes(out,sentinel.data(),sentinel.size()).is_ok());
                ASSERT_TRUE(memory.write_u16(length,8).is_ok());
                EXPECT_EQ(invoke(type,0,0,0),type<=2?hid_sdk::kHIDP_STATUS_USAGE_NOT_FOUND:
                                                          hid_sdk::kHIDP_STATUS_INVALID_REPORT_TYPE);
                EXPECT_EQ(memory.read_u16(length).value(),type<=2?0:8);
                EXPECT_EQ(memory.read_u32(out).value(),0xa5a5a5a5u);
            }
            if (specific) {
                for (auto filter : {std::array<u16,3>{0xfffe,0,0},
                                    std::array<u16,3>{0,1234,0},
                                    std::array<u16,3>{0,0,0xfffe}}) {
                    ASSERT_TRUE(memory.write_u16(length,8).is_ok());
                    EXPECT_EQ(invoke(0,filter[0],filter[1],filter[2]),hid_sdk::kHIDP_STATUS_USAGE_NOT_FOUND);
                    EXPECT_EQ(memory.read_u16(length).value(),0);
                }
                ASSERT_TRUE(memory.write_u16(length,8).is_ok());
                EXPECT_EQ(invoke(0,value?1:9,hid_sdk::kHIDP_LINK_COLLECTION_ROOT,
                                 value?hid_sdk::kHID_USAGE_GENERIC_Y:10),hid_sdk::kHIDP_STATUS_SUCCESS);
                EXPECT_EQ(memory.read_u16(length).value(),1);
            }
        }
    }

    void verify_report_data(bool is64) {
        setup(is64);
        const auto& l=gamepad_hid::layout(is64);
        constexpr GuestAddr out=0x12000,length=0x14000,report_address=0x15000;
        const gamepad_hid::Report report{1,0x34,0x12,0x78,0x56,0xbc,0x9a,
            0xf0,0xde,0xff,0xff,0x81,0x02,8};
        ASSERT_TRUE(memory.write_bytes(report_address,report.data(),report.size()).is_ok());
        std::array<u8,128> sentinel{};sentinel.fill(0xa5);
        constexpr u16 indices[]{0,1,2,3,4,5,12,14,15};
        constexpr u32 values[]{0x1234,0x5678,0x9abc,0xdef0,0xffff,1,1,1,8};
        for(u32 capacity : {0u,1u,6u,16u}) {
            ASSERT_TRUE(memory.write_bytes(out,sentinel.data(),sentinel.size()).is_ok());
            ASSERT_TRUE(memory.write_u32(length,capacity).is_ok());
            set_last_error(0xdead);
            EXPECT_EQ(call("HidP_GetData",{0,out,length,0x11000,report_address,14}),
                      capacity<9?hid_sdk::kHIDP_STATUS_BUFFER_TOO_SMALL:hid_sdk::kHIDP_STATUS_SUCCESS);
            EXPECT_EQ(memory.read_u32(length).value(),9u);
            EXPECT_EQ(get_last_error(),0xdeadu);
            const usize filled=std::min<usize>(capacity,9);
            for(usize i=0;i<filled;++i) {
                const GuestAddr entry=out+i*l.sizeof_HIDP_DATA;
                EXPECT_EQ(memory.read_u16(entry+l.offsetof_HIDP_DATA_DataIndex).value(),indices[i]);
                // Windows oracle preserves HIDP_DATA.Reserved, and a BOOLEAN
                // button write preserves the other three bytes of its union.
                EXPECT_EQ(memory.read_u16(entry+l.offsetof_HIDP_DATA_DataIndex+sizeof(u16)).value(),0xa5a5);
                const GuestAddr value=entry+l.offsetof_HIDP_DATA_RawValue;
                if(i>=5 && i<=7) {
                    EXPECT_EQ(memory.read_u8(value).value(),1);
                    for(usize byte=sizeof(u8);byte<sizeof(u32);++byte)
                        EXPECT_EQ(memory.read_u8(value+byte).value(),0xa5);
                } else EXPECT_EQ(memory.read_u32(value).value(),values[i]);
            }
            EXPECT_EQ(memory.read_u32(out+filled*l.sizeof_HIDP_DATA).value(),0xa5a5a5a5u);
        }
        ASSERT_TRUE(memory.write_bytes(out,sentinel.data(),sentinel.size()).is_ok());
        ASSERT_TRUE(memory.write_u32(length,16).is_ok());
        ASSERT_TRUE(memory.write_u8(report_address,0x7f).is_ok());
        // An unknown report ID is a successful empty GetData result on Windows.
        EXPECT_EQ(call("HidP_GetData",{0,out,length,0x11000,report_address,14}),hid_sdk::kHIDP_STATUS_SUCCESS);
        EXPECT_EQ(memory.read_u32(length).value(),0u);
        EXPECT_EQ(memory.read_u32(out).value(),0xa5a5a5a5u);
        for(auto test : {std::array<u32,3>{0,0,hid_sdk::kHIDP_STATUS_INVALID_REPORT_LENGTH},
                        std::array<u32,3>{0,13,hid_sdk::kHIDP_STATUS_INVALID_REPORT_LENGTH},
                        std::array<u32,3>{0,15,hid_sdk::kHIDP_STATUS_INVALID_REPORT_LENGTH},
                        std::array<u32,3>{1,14,hid_sdk::kHIDP_STATUS_INVALID_REPORT_LENGTH},
                        std::array<u32,3>{1,0,hid_sdk::kHIDP_STATUS_REPORT_DOES_NOT_EXIST},
                        std::array<u32,3>{2,0,hid_sdk::kHIDP_STATUS_REPORT_DOES_NOT_EXIST},
                        std::array<u32,3>{3,0,hid_sdk::kHIDP_STATUS_INVALID_REPORT_TYPE}}) {
            ASSERT_TRUE(memory.write_u32(length,16).is_ok());
            EXPECT_EQ(call("HidP_GetData",{test[0],out,length,0x11000,report_address,test[1]}),test[2]);
            EXPECT_EQ(memory.read_u32(length).value(),16u);
            EXPECT_EQ(memory.read_u32(out).value(),0xa5a5a5a5u);
        }
    }

    void verify_values_and_usages(bool is64) {
        setup(is64);
        constexpr GuestAddr out=0x12000,count=0x14000,report_address=0x15000;
        const gamepad_hid::Report report{1,255,255,0,0,0,0,0,0,0,0,0x81,2,8};
        ASSERT_TRUE(memory.write_bytes(report_address,report.data(),report.size()).is_ok());
        for(bool scaled : {false,true}) {
            const char* name=scaled?"HidP_GetScaledUsageValue":"HidP_GetUsageValue";
            EXPECT_EQ(call(name,{0,1,0,0x30,out,0x11000,report_address,14}),hid_sdk::kHIDP_STATUS_SUCCESS);
            EXPECT_EQ(memory.read_u32(out).value(),65535u);
            EXPECT_EQ(call(name,{0,1,0,0x39,out,0x11000,report_address,14}),
                      scaled?hid_sdk::kHIDP_STATUS_NULL:hid_sdk::kHIDP_STATUS_SUCCESS);
            EXPECT_EQ(memory.read_u32(out).value(),scaled?0u:8u);
            for(auto filter : {std::array<u16,3>{0xfffe,0,0x30},
                               std::array<u16,3>{1,1234,0x30},
                               std::array<u16,3>{1,0,0xfffe}}) {
                ASSERT_TRUE(memory.write_u32(out,0xa5a5a5a5).is_ok());
                EXPECT_EQ(call(name,{0,filter[0],filter[1],filter[2],out,0x11000,report_address,14}),
                          hid_sdk::kHIDP_STATUS_USAGE_NOT_FOUND);
                EXPECT_EQ(memory.read_u32(out).value(),0xa5a5a5a5u);
            }
            for(auto test : {std::array<u32,3>{0,13,hid_sdk::kHIDP_STATUS_INVALID_REPORT_LENGTH},
                             std::array<u32,3>{1,0,hid_sdk::kHIDP_STATUS_REPORT_DOES_NOT_EXIST},
                             std::array<u32,3>{3,14,hid_sdk::kHIDP_STATUS_INVALID_REPORT_TYPE}}) {
                ASSERT_TRUE(memory.write_u32(out,0xa5a5a5a5).is_ok());
                EXPECT_EQ(call(name,{test[0],1,0,0x30,out,0x11000,report_address,test[1]}),test[2]);
                EXPECT_EQ(memory.read_u32(out).value(),0xa5a5a5a5u);
            }
            ASSERT_TRUE(memory.write_u8(report_address,0x7f).is_ok());
            ASSERT_TRUE(memory.write_u32(out,0xa5a5a5a5).is_ok());
            EXPECT_EQ(call(name,{0,1,0,0x30,out,0x11000,report_address,14}),hid_sdk::kHIDP_STATUS_INCOMPATIBLE_REPORT_ID);
            EXPECT_EQ(memory.read_u32(out).value(),0xa5a5a5a5u);
            ASSERT_TRUE(memory.write_u8(report_address,1).is_ok());
        }
        for(bool ex : {false,true}) {
            const char* name=ex?"HidP_GetUsagesEx":"HidP_GetUsages";
            auto invoke=[&](u32 type,u32 length,u16 page,u16 link,GuestAddr pp=0x11000) {
                return ex?call(name,{type,link,out,count,pp,report_address,length}):
                          call(name,{type,page,link,out,count,pp,report_address,length});
            };
            std::array<u8,64> sentinel{};sentinel.fill(0xa5);
            auto reset=[&](u32 capacity) {
                EXPECT_TRUE(memory.write_bytes(out,sentinel.data(),sentinel.size()).is_ok());
                EXPECT_TRUE(memory.write_u32(count,capacity).is_ok());
            };
            for(u32 capacity : {0u,1u,16u}) {
                reset(capacity);set_last_error(0xdead);
                EXPECT_EQ(invoke(0,14,9,0),capacity<3?hid_sdk::kHIDP_STATUS_BUFFER_TOO_SMALL:
                                                   hid_sdk::kHIDP_STATUS_SUCCESS);
                EXPECT_EQ(memory.read_u32(count).value(),3u);
                EXPECT_EQ(get_last_error(),0xdeadu);
                auto expected=sentinel;
                // Measured Windows prefix clear is cap*sizeof(USAGE), even
                // for Ex; only actual returned pairs write the additional bytes.
                std::fill_n(expected.begin(),capacity*2,0);
                constexpr u8 pressed[]{1,8,10};
                for(usize i=0;i<std::min<usize>(capacity,3);++i) {
                    const usize at=i*(ex?4:2);expected[at]=pressed[i];expected[at+1]=0;
                    if(ex){expected[at+2]=9;expected[at+3]=0;}
                }
                std::array<u8,64> actual{};
                ASSERT_TRUE(memory.read_bytes(out,actual.data(),actual.size()).is_ok());
                EXPECT_EQ(actual,expected);
            }
            for(auto test : {std::array<u32,3>{0,13,hid_sdk::kHIDP_STATUS_INVALID_REPORT_LENGTH},
                             std::array<u32,3>{1,0,hid_sdk::kHIDP_STATUS_REPORT_DOES_NOT_EXIST},
                             std::array<u32,3>{3,14,hid_sdk::kHIDP_STATUS_INVALID_REPORT_TYPE}}) {
                reset(16);EXPECT_EQ(invoke(test[0],test[1],9,0),test[2]);
                EXPECT_EQ(memory.read_u32(count).value(),16u);
                EXPECT_EQ(memory.read_u32(out).value(),0u);
                EXPECT_EQ(memory.read_u32(out+32).value(),0xa5a5a5a5u);
            }
            reset(16);EXPECT_EQ(invoke(0,14,9,1234),hid_sdk::kHIDP_STATUS_USAGE_NOT_FOUND);
            EXPECT_EQ(memory.read_u32(count).value(),0u);
            reset(16);ASSERT_TRUE(memory.write_u8(report_address,0x7f).is_ok());
            EXPECT_EQ(invoke(0,14,9,0),hid_sdk::kHIDP_STATUS_INCOMPATIBLE_REPORT_ID);
            EXPECT_EQ(memory.read_u32(count).value(),0u);
            ASSERT_TRUE(memory.write_u8(report_address,1).is_ok());
            reset(16);EXPECT_EQ(invoke(0,14,9,0,0x16000),hid_sdk::kHIDP_STATUS_INVALID_PREPARSED_DATA);
            EXPECT_EQ(memory.read_u32(count).value(),16u);
            EXPECT_EQ(memory.read_u32(out).value(),0xa5a5a5a5u);
        }
    }

};
TEST_F(GamepadHidParser,CapsX86) {verify_caps(false);}
TEST_F(GamepadHidParser,CapsX64) {verify_caps(true);}

TEST_F(GamepadHidParser,ArrayCapsX86) {verify_array_caps(false);}
TEST_F(GamepadHidParser,ArrayCapsX64) {verify_array_caps(true);}

TEST_F(GamepadHidParser,ReportDataX86) {verify_report_data(false);}
TEST_F(GamepadHidParser,ReportDataX64) {verify_report_data(true);}

TEST_F(GamepadHidParser,ValuesAndUsagesX86) {verify_values_and_usages(false);}
TEST_F(GamepadHidParser,ValuesAndUsagesX64) {verify_values_and_usages(true);}

#include "winapi/kernel32/filesystem/file.hpp"
#include "winapi/kernel32/memory/global_local.hpp"
#include "winapi/kernel32/memory/memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "winapi/kernel32/kernel32_ext.hpp"
#include "winapi/kernel32/synchronization/sync.hpp"
#include "winapi/system/ntdll/ntdll_internal.hpp"
#include "winapi/system/setupapi/setupapi_internal.hpp"
#include "winapi/media/xinput/xinput.hpp"
#include "winapi/kernel32/filesystem/hid_io_status.generated.hpp"
#include <condition_variable>
#include <chrono>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <cstdio>
#include <unistd.h>
#include "core/sync/ntsync.hpp"
namespace {
class HidFixtureHost final : public TestHidHost {
public:
    std::atomic<u16> buttons{hid_sdk::kXINPUT_GAMEPAD_A};
    std::atomic<u32> queries{0};
    std::function<void()> observer;
    bool query_state(u32 slot,hardware::GamepadState& out) override {
        ++queries;if(observer) observer();out={};out.connected=slot==0;out.buttons=buttons.load();return true;
    }
};
struct IoFixtureCleanup {
    FileManager& files;Kernel32ExtManager& kernel;SyncManager& sync;HandleTable& handles;
    ~IoFixtureCleanup() {
        for(auto h:handles.snapshot_handles()) if(handles.get_type(h)==HandleType::HidDevice) (void)files.close_file(h);
        files.shutdown_hid_io();
        for(auto h:handles.snapshot_handles()) {
            const auto type=handles.get_type(h);
            if(type==HandleType::Event) (void)sync.close_handle(h);
            else if(type==HandleType::IoCompletionPort) (void)handles.close(h);
        }
        kernel.set_file_manager(nullptr);files.set_hid_waitable_callbacks({},{});handles.set_close_callback({});
    }
};
}
class GamepadHidDevice : public GamepadHidParser {
protected:
    void verify_explicit_events(bool is64,bool oracle_cases=false) {
        setup(is64);
        capsule::Vfs vfs;HandleTable handles;FileManager files(vfs,handles,memory);SyncManager sync(handles);
        files.set_hid_waitable_callbacks([&](Handle h,u64 id){sync.register_waitable(h,id);},[&](Handle h){sync.unregister_waitable(h);});
        Kernel32ExtManager kernel(memory,vfs);kernel.set_handle_table(&handles);kernel.set_sync_manager(&sync);kernel.set_file_manager(&files);
        handles.set_close_callback([&](Handle h,HandleType type,void* object){
            if(type==HandleType::HidDevice) kernel.on_iocp_handle_close(h);
            if(type==HandleType::IoCompletionPort) return kernel.on_iocp_port_close(h);
            return files.on_handle_close(h,type,object);
        });
        IoFixtureCleanup cleanup{files,kernel,sync,handles};
        auto host=std::make_shared<HidFixtureHost>();auto catalog=std::make_shared<GamepadHidCatalog>(host);files.set_gamepad_catalog(catalog);
        const auto devices=catalog->devices();ASSERT_EQ(devices.size(),1u);
        exports=create_hid_thunks(&handles,&files);
        exports.entries.push_back({"ReadProbe",0,[&](Marshaler& m){impl_ReadFile(m,files);},20});
        exports.entries.push_back({"ResultProbe",0,[&](Marshaler& m){kernel.impl_GetOverlappedResult(m);},16});
        exports.entries.push_back({"ModesProbe",0,[&](Marshaler& m){kernel.impl_SetFileCompletionNotificationModes(m);},8});
        exports.entries.push_back({"PortProbe",0,[&](Marshaler& m){kernel.impl_CreateIoCompletionPort(m);},16});
        exports.entries.push_back({"DequeueProbe",0,[&](Marshaler& m){kernel.impl_GetQueuedCompletionStatus(m);},20});
        // ReadFile/OVERLAPPED: issue resets hEvent; completion signals it.
        // GOR can return a completed request even after an auto-reset wait
        // consumed the signal. SetFileCompletionNotificationModes mode2 must
        // preserve explicit events; GQCS documents valid-event low-bit tagging.
        const GuestAddr ov=0x16000;const usize pointer=is64 ? sizeof(u64):sizeof(u32);
        const GuestAddr event_field=ov+2*pointer+2*sizeof(u32);
        if(oracle_cases) {
            const auto path=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path()/"oracle_data"/
                (is64 ? "hid_io_event_access_x64.expected":"hid_io_event_access_x86.expected");
            std::ifstream input(path);ASSERT_TRUE(input.is_open())<<path;
            std::map<std::pair<u32,u32>,std::array<u32,3>> file_states;
            std::map<std::pair<u32,u32>,u32> file_done;
            std::string line;unsigned rows=0;
            while(std::getline(input,line)) {
                unsigned mode=0,tag=0,first=0,second=0,after=0;
                if(std::sscanf(line.c_str(),"file.read.mode=%u low=%u before=%u,%u after=%u",&mode,&tag,&first,&second,&after)==5)
                    file_states[{mode,tag}]={first,second,after};
                if(std::sscanf(line.c_str(),"file.read.done.mode=%u low=%u wait=%u",&mode,&tag,&after)==3) file_done[{mode,tag}]=after;
            }
            input.clear();input.seekg(0);
            while(std::getline(input,line)) {
                if(line.rfind("mode=",0)!=0 || line.find(" submit=")==std::string::npos) continue;
                std::map<std::string,u64> expected;std::istringstream words(line);std::string word;
                while(words>>word) {const auto at=word.find('=');if(at==std::string::npos)continue;
                    const auto key=word.substr(0,at);expected[key]=std::stoull(word.substr(at+1),nullptr,(key=="rights"||key=="internal") ? 16:10);}
                const auto mode=expected.at("mode"),tag=expected.at("low");
                SCOPED_TRACE(::testing::Message()<<"oracle mode="<<mode<<" tag="<<tag);++rows;
                host->buttons=0;
                const Handle file=files.create_file(devices[0].path(),GENERIC_READ,3,OPEN_EXISTING,hid_sdk::kFILE_FLAG_OVERLAPPED);ASSERT_NE(file,INVALID_HANDLE);
                const Handle observer=sync.create_event(true,true,"");ASSERT_NE(observer,NULL_HANDLE);
                Handle supplied=NULL_HANDLE;bool alias_live=false;
                if(mode<6 || mode==11) {
                    supplied=sync.duplicate_handle(observer,0,static_cast<u32>(expected.at("rights")));ASSERT_NE(supplied,NULL_HANDLE);alias_live=true;
                    if(mode==5) {EXPECT_TRUE(sync.close_handle(supplied));alias_live=false;}
                } else if(mode==6) supplied=file; // another NT object type
                else if(mode==7) supplied=0x12345678u; // oracle's unallocated negative control
                else if(mode==8) supplied=INVALID_HANDLE;
                else if(mode==10) supplied=1;
                // Prime a NULL-event input read, exactly as run6 primes the
                // file object's signaled state before event validation.
                std::array<u8,32> zero{};ASSERT_TRUE(memory.write_bytes(ov,zero.data(),zero.size()).is_ok());
                EXPECT_EQ(call("ReadProbe",{file,0x15000,gamepad_hid::kReportBytes,0,ov}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_IO_PENDING);
                EXPECT_EQ(sync.wait_for_single_object(file,2000),WAIT_OBJECT_0);
                EXPECT_EQ(call("ResultProbe",{file,ov,0x14000,0}),1u);
                const auto file_expect=file_states.at({static_cast<u32>(mode),static_cast<u32>(tag)});
                EXPECT_EQ(sync.wait_for_single_object(file,0),file_expect[0]);EXPECT_EQ(sync.wait_for_single_object(file,0),file_expect[1]);
                ASSERT_TRUE(memory.write_bytes(ov,zero.data(),zero.size()).is_ok());
                ASSERT_TRUE(marshaler.write_ptr(ov,0xdead).is_ok());ASSERT_TRUE(marshaler.write_ptr(ov+pointer,0xbeef).is_ok());
                ASSERT_TRUE(marshaler.write_ptr(event_field,supplied|tag).is_ok());ASSERT_TRUE(memory.write_u32(0x14000,0xdead).is_ok());
                set_last_error(0xdead);const u32 submitted=call("ReadProbe",{file,0x15000,gamepad_hid::kReportBytes,0x14000,ov});
                const u32 submit_error=get_last_error();
                EXPECT_EQ(submitted,expected.at("submit"));EXPECT_EQ(submit_error,expected.at("error"));
                EXPECT_EQ(memory.read_u32(0x14000).value(),expected.at("count"));
                EXPECT_EQ(marshaler.read_ptr(ov).value(),expected.at("internal"));EXPECT_EQ(marshaler.read_ptr(ov+pointer).value(),expected.at("high"));
                EXPECT_EQ(sync.wait_for_single_object(observer,0),expected.at("initialwait"));
                EXPECT_EQ(sync.wait_for_single_object(file,0),file_expect[2]);
                if(expected.at("error")==error_codes::ERROR_IO_PENDING) {
                    if(mode==11) {EXPECT_TRUE(sync.close_handle(supplied));alias_live=false;}
                    if(mode==2 || mode==11) {
                        // FINAL gorwait oracle: MODIFY-only is a valid I/O
                        // signal target but cannot be waited; a closed alias
                        // likewise fails the wait without changing the count.
                        ASSERT_TRUE(memory.write_u32(0x14000,0xdead).is_ok());
                        EXPECT_EQ(call("ResultProbe",{file,ov,0x14000,1}),0u);
                        EXPECT_EQ(get_last_error(),mode==2 ? error_codes::ERROR_ACCESS_DENIED:error_codes::ERROR_INVALID_HANDLE);
                        EXPECT_EQ(memory.read_u32(0x14000).value(),0xdeadu);EXPECT_EQ(marshaler.read_ptr(ov).value(),hid_io_status::kSTATUS_PENDING);
                    }
                    host->buttons=hid_sdk::kXINPUT_GAMEPAD_B;
                    // Event observer survives closing the exact alias supplied
                    // by the request. Numeric reuse must not retarget its pin.
                    if(mode!=9) EXPECT_EQ(sync.wait_for_single_object(observer,2000),WAIT_OBJECT_0);
                    for(unsigned poll=0;poll<200 && marshaler.read_ptr(ov).value()==hid_io_status::kSTATUS_PENDING;++poll)
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    EXPECT_EQ(call("ResultProbe",{file,ov,0x14000,0}),1u);EXPECT_EQ(get_last_error(),error_codes::ERROR_SUCCESS);
                    EXPECT_EQ(memory.read_u32(0x14000).value(),gamepad_hid::kReportBytes);
                    EXPECT_EQ(memory.read_u8(0x1500b).value(),2u);
                    const auto expected_file=file_done.at({static_cast<u32>(mode),static_cast<u32>(tag)});
                    EXPECT_EQ(sync.wait_for_single_object(file,expected_file==WAIT_OBJECT_0 ? 2000u:0u),expected_file);
                }
                EXPECT_TRUE(files.close_file(file));files.shutdown_hid_io();
                if(alias_live) EXPECT_TRUE(sync.close_handle(supplied));
                EXPECT_TRUE(sync.close_handle(observer));
            }
            EXPECT_GT(rows,0u);
            // Windows run4: last REAL handle unlinks the name even though an
            // IRP pins the old object; a same-name replacement is a new event.
            for(bool promote:{false,true}) {
                SCOPED_TRACE(::testing::Message()<<"named/promotion="<<promote);
                host->buttons=0;
                const Handle file=files.create_file(devices[0].path(),GENERIC_READ,3,OPEN_EXISTING,hid_sdk::kFILE_FLAG_OVERLAPPED);ASSERT_NE(file,INVALID_HANDLE);
                const std::string name="VitreoHidIo-"+std::to_string(::getpid())+"-"+std::to_string(is64)+"-"+std::to_string(promote);
                const Handle event=sync.create_event(true,false,promote ? "":name);ASSERT_NE(event,NULL_HANDLE);
                u32 error=0;auto pin=sync.reference_event_for_io(event,error);ASSERT_TRUE(pin);EXPECT_EQ(error,0u);
                const auto old_native=pin->nt_id;
                EXPECT_EQ(call("HidD_FlushQueue",{file}),1u);
                std::array<u8,32> zero{};ASSERT_TRUE(memory.write_bytes(ov,zero.data(),zero.size()).is_ok());
                ASSERT_TRUE(marshaler.write_ptr(event_field,event).is_ok());
                EXPECT_EQ(call("ReadProbe",{file,0x15000,gamepad_hid::kReportBytes,0,ov}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_IO_PENDING);
                if(promote) {
                    const auto descriptor=sync.prepare_transferred_handle(event,sync_access::EVENT_ALL_ACCESS,false);ASSERT_TRUE(descriptor.has_value())<<"error="<<get_last_error()<<" errno="<<errno;
                    EXPECT_EQ(pin->nt_id,0u);EXPECT_FALSE(ntsync::global().snapshot(old_native));
                }
                EXPECT_TRUE(sync.close_handle(event));
                Handle replacement=NULL_HANDLE;
                if(!promote) {
                    EXPECT_EQ(sync.open_event(sync_access::EVENT_ALL_ACCESS,name),NULL_HANDLE);EXPECT_EQ(get_last_error(),error_codes::ERROR_FILE_NOT_FOUND);
                    replacement=sync.create_event(true,false,name);ASSERT_NE(replacement,NULL_HANDLE);EXPECT_EQ(get_last_error(),error_codes::ERROR_SUCCESS);
                }
                host->buttons=hid_sdk::kXINPUT_GAMEPAD_B;
                for(unsigned poll=0;poll<200 && marshaler.read_ptr(ov).value()==hid_io_status::kSTATUS_PENDING;++poll)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                EXPECT_EQ(call("ResultProbe",{file,ov,0x14000,0}),1u);EXPECT_EQ(memory.read_u32(0x14000).value(),gamepad_hid::kReportBytes);
                bool manual=false,signaled=false;
                for(unsigned poll=0;poll<200;++poll) {ASSERT_TRUE(pin->query_for_io(manual,signaled));if(signaled) break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
                EXPECT_TRUE(signaled);
                if(replacement) {EXPECT_EQ(sync.wait_for_single_object(replacement,0),WAIT_TIMEOUT);EXPECT_TRUE(sync.close_handle(replacement));}
                EXPECT_TRUE(files.close_file(file));files.shutdown_hid_io();
                pin.reset();
                if(old_native) EXPECT_FALSE(ntsync::global().snapshot(old_native));
            }
            kernel.set_file_manager(nullptr);files.set_hid_waitable_callbacks({},{});handles.set_close_callback({});return;
        }
        for(bool manual:{false,true}) for(u32 tag:{0u,1u,2u,3u}) for(bool skip_file:{false,true}) {
            const bool tagged=(tag&1u)!=0;
            SCOPED_TRACE(::testing::Message()<<"manual="<<manual<<" tag="<<tag<<" skip_file="<<skip_file);
            host->buttons=0;
            const Handle file=files.create_file(devices[0].path(),GENERIC_READ,3,OPEN_EXISTING,hid_sdk::kFILE_FLAG_OVERLAPPED);
            ASSERT_NE(file,INVALID_HANDLE);
            const Handle event=sync.create_event(manual,true,"");ASSERT_NE(event,NULL_HANDLE);
            const Handle port=call("PortProbe",{file,0,0x1234,0});ASSERT_NE(port,NULL_HANDLE);
            if(skip_file) EXPECT_EQ(call("ModesProbe",{file,hid_sdk::kFILE_SKIP_SET_EVENT_ON_HANDLE}),1u);
            EXPECT_EQ(call("HidD_FlushQueue",{file}),1u);
            std::array<u8,32> zero{};ASSERT_TRUE(memory.write_bytes(ov,zero.data(),zero.size()).is_ok());
            ASSERT_TRUE(marshaler.write_ptr(event_field,event | tag).is_ok());
            EXPECT_EQ(call("ReadProbe",{file,0x15000,gamepad_hid::kReportBytes,0,ov}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_IO_PENDING);
            bool event_manual=false,event_signaled=true;ASSERT_TRUE(sync.query_event(event,event_manual,event_signaled));
            EXPECT_EQ(event_manual,manual);EXPECT_FALSE(event_signaled);
            ASSERT_TRUE(memory.write_u32(0x14000,0xdead).is_ok());
            EXPECT_EQ(call("ResultProbe",{file,ov,0x14000,0}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_IO_INCOMPLETE);
            EXPECT_EQ(memory.read_u32(0x14000).value(),0xdeadu);
            std::future<std::pair<u32,u32>> waiting;std::atomic<bool> entered{false};
            if(manual) {
                waiting=std::async(std::launch::async,[&]{entered=true;const u32 result=call("ResultProbe",{file,ov,0x14000,1});return std::pair{result,get_last_error()};});
                while(!entered.load()) std::this_thread::yield();
                EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(32)),std::future_status::timeout);
            }
            host->buttons=hid_sdk::kXINPUT_GAMEPAD_B;
            EXPECT_EQ(sync.wait_for_single_object(event,2000),WAIT_OBJECT_0);
            if(manual) {
                const bool ready=waiting.wait_for(std::chrono::seconds(2))==std::future_status::ready;EXPECT_TRUE(ready);
                if(!ready) (void)sync.set_event(event); // release a failed test's waiter
                const auto result=waiting.get();EXPECT_EQ(result.first,1u);EXPECT_EQ(result.second,error_codes::ERROR_SUCCESS);
            } else {
                const auto internal=marshaler.read_ptr(ov);ASSERT_TRUE(internal.is_ok());ASSERT_EQ(internal.value(),0u);
                EXPECT_EQ(call("ResultProbe",{file,ov,0x14000,1}),1u);EXPECT_EQ(get_last_error(),error_codes::ERROR_SUCCESS);
            }
            ASSERT_TRUE(sync.query_event(event,event_manual,event_signaled));EXPECT_EQ(event_signaled,manual);
            EXPECT_EQ(memory.read_u32(0x14000).value(),gamepad_hid::kReportBytes);
            EXPECT_EQ(memory.read_u8(0x1500b).value(),2u);
            EXPECT_EQ(call("DequeueProbe",{port,0x14000,0x14010,0x14020,tagged ? 0u:2000u}),tagged ? 0u:1u);
            if(tagged) EXPECT_EQ(get_last_error(),WAIT_TIMEOUT);
            else {EXPECT_EQ(memory.read_u32(0x14000).value(),gamepad_hid::kReportBytes);EXPECT_EQ(marshaler.read_ptr(0x14020).value(),ov);}
            EXPECT_EQ(sync.wait_for_single_object(file,0),WAIT_TIMEOUT);
            EXPECT_TRUE(files.close_file(file));EXPECT_TRUE(sync.close_handle(event));EXPECT_TRUE(handles.close(port));
        }
        files.shutdown_hid_io();kernel.set_file_manager(nullptr);files.set_hid_waitable_callbacks({},{});handles.set_close_callback({});
    }
    void verify_device(bool is64) {
        setup(is64);
        capsule::Vfs vfs;HandleTable handles;FileManager files(vfs,handles,memory);
        SyncManager sync(handles);
        files.set_hid_waitable_callbacks([&](Handle h,u64 id){sync.register_waitable(h,id);},[&](Handle h){sync.unregister_waitable(h);});
        Kernel32ExtManager kernel(memory,vfs);kernel.set_handle_table(&handles);kernel.set_file_manager(&files);kernel.set_sync_manager(&sync);
        NtdllExtManager nt;nt.set_file_manager(&files);nt.set_handle_table(&handles);nt.set_sync_manager(&sync);
        handles.set_close_callback([&](Handle h,HandleType type,void* object){
            if(type==HandleType::HidDevice) kernel.on_iocp_handle_close(h);
            if(type==HandleType::IoCompletionPort) return kernel.on_iocp_port_close(h);
            return files.on_handle_close(h,type,object);
        });
        IoFixtureCleanup cleanup{files,kernel,sync,handles};
        auto host=std::make_shared<HidFixtureHost>();
        auto catalog=std::make_shared<GamepadHidCatalog>(host);
        files.set_gamepad_catalog(catalog);
        const auto devices=catalog->devices();ASSERT_EQ(devices.size(),1);
        EXPECT_NE(devices[0].instance_id().find("IG_"),std::string::npos);
        const auto nodes=setupapi_internal::device_nodes("",catalog);
        const auto* node=setupapi_internal::find_node(nodes,devices[0].instance_id());ASSERT_NE(node,nullptr);
        ASSERT_EQ(node->interfaces.size(),1);
        EXPECT_EQ(setupapi_internal::interface_path(node->instance_id,node->interfaces[0],false),devices[0].path());
        const auto handle=files.create_file(devices[0].path(),GENERIC_READ,3,OPEN_EXISTING,0);
        ASSERT_NE(handle,INVALID_HANDLE);
        set_last_error(0xdead);EXPECT_EQ(files.file_type(handle),0u);EXPECT_EQ(get_last_error(),0u);
        VirtualMemoryManager vmm(memory,is64 ? 0x7ffffffeffffull : 0x7ffeffffull);
        HeapManager heap(vmm,memory,handles);GlobalLocalManager allocator(heap);
        exports=create_hid_thunks(&handles,&files,&allocator);
        exports.entries.push_back({"DeviceControlProbe",0,[&](Marshaler& m){kernel.impl_DeviceIoControl(m);},32});
        exports.entries.push_back({"OverlappedResultProbe",0,[&](Marshaler& m){kernel.impl_GetOverlappedResult(m);},16});
        exports.entries.push_back({"CreatePortProbe",0,[&](Marshaler& m){kernel.impl_CreateIoCompletionPort(m);},16});
        exports.entries.push_back({"DequeuePortProbe",0,[&](Marshaler& m){kernel.impl_GetQueuedCompletionStatus(m);},20});
        exports.entries.push_back({"CompletionModesProbe",0,[&](Marshaler& m){kernel.impl_SetFileCompletionNotificationModes(m);},8});
        exports.entries.push_back({"CancelExProbe",0,[&](Marshaler& m){kernel.impl_CancelIoEx(m);},8});
        exports.entries.push_back({"CancelProbe",0,[&](Marshaler& m){kernel.impl_CancelIo(m);},4});
        exports.entries.push_back({"NtDeviceControlProbe",0,[&](Marshaler& m){ntdll::impl_NtDeviceIoControlFile(m,nt);},40});
        for(auto& entry:make_file_exports(files).entries) if(entry.name=="GetFileType") exports.entries.push_back(entry);
        for(auto& entry:create_xinput_thunks(XInputDll::V1_4,host).entries) if(entry.name=="XInputGetState") exports.entries.push_back(entry);
        EXPECT_EQ(call("XInputGetState",{0,0x15000}),0u);
        EXPECT_EQ(memory.read_u16(0x15004).value(),hid_sdk::kXINPUT_GAMEPAD_A);
        set_last_error(0xdead);EXPECT_EQ(call("GetFileType",{handle}),0u);EXPECT_EQ(get_last_error(),0u);
        EXPECT_EQ(sync.wait_for_single_object(handle,0),0u); // fresh HID file signaled (Windows oracle)
        EXPECT_EQ(sync.wait_for_single_object(handle,0),0u); // file completion event is manual reset
        for(u32 flags:{1u,2u,3u}) {EXPECT_EQ(call("CompletionModesProbe",{handle,flags}),0u);EXPECT_EQ(get_last_error(),87u);}
        // Error mappings are independently measured with Windows
        // RtlNtStatusToDosError; pending GetOverlappedResult has its own996
        // contract and must preserve the caller's count (alias oracle).
        const std::pair<u32,u32> mappings[]={
            {hid_io_status::kSTATUS_DEVICE_NOT_CONNECTED,hid_io_status::kDosSTATUS_DEVICE_NOT_CONNECTED},
            {hid_io_status::kSTATUS_ACCESS_VIOLATION,hid_io_status::kDosSTATUS_ACCESS_VIOLATION},
            {hid_io_status::kSTATUS_CANCELLED,hid_io_status::kDosSTATUS_CANCELLED},
            {hid_sdk::kSTATUS_INVALID_BUFFER_SIZE,hid_io_status::kDosSTATUS_INVALID_BUFFER_SIZE},
            {hid_io_status::kSTATUS_NOT_SUPPORTED,hid_io_status::kDosSTATUS_NOT_SUPPORTED}};
        for(const auto& [status,error]:mappings) {
            if(is64) {ASSERT_TRUE(memory.write_u64(0x17000,status).is_ok());ASSERT_TRUE(memory.write_u64(0x17008,0).is_ok());}
            else {ASSERT_TRUE(memory.write_u32(0x17000,status).is_ok());ASSERT_TRUE(memory.write_u32(0x17004,0).is_ok());}
            EXPECT_EQ(call("OverlappedResultProbe",{handle,0x17000,0x14000,0}),0u);EXPECT_EQ(get_last_error(),error);
            EXPECT_EQ(memory.read_u32(0x14000).value(),0u);
        }
        // Metadata oracle: each allocation is independent and LastError is
        // preserved across successful acquisition/free, in either bitness.
        set_last_error(0xdead);
        EXPECT_EQ(call("HidD_GetPreparsedData",{handle,0x14000}),1u);
        EXPECT_EQ(get_last_error(),0xdeadu);
        const GuestAddr pp1=is64 ? memory.read_u64(0x14000).value() : memory.read_u32(0x14000).value();
        EXPECT_EQ(call("HidD_GetPreparsedData",{handle,0x14000}),1u);
        const GuestAddr pp2=is64 ? memory.read_u64(0x14000).value() : memory.read_u32(0x14000).value();
        EXPECT_NE(pp1,pp2);EXPECT_EQ(call("HidD_FreePreparsedData",{pp1}),1u);
        EXPECT_EQ(call("HidP_GetCaps",{pp2,0x12000}),hid_sdk::kHIDP_STATUS_SUCCESS);
        EXPECT_EQ(call("HidD_FreePreparsedData",{0x11000}),0u); // RawInput copy is not owned allocation.
        EXPECT_EQ(call("HidD_FreePreparsedData",{pp2}),1u);EXPECT_EQ(get_last_error(),0xdeadu);
        for(const auto* name:{"HidD_GetAttributes","HidD_GetPreparsedData"}) {
            set_last_error(0xdead);(void)call(name,{handle,0});
            EXPECT_TRUE(cpu.pending_raise.active);EXPECT_EQ(cpu.pending_raise.code,nt_status::STATUS_ACCESS_VIOLATION);
            EXPECT_EQ(cpu.pending_raise.args[0],1u);EXPECT_EQ(cpu.pending_raise.args[1],0u);EXPECT_EQ(get_last_error(),0xdeadu);
            cpu.pending_raise.clear();
        }
        EXPECT_EQ(call("HidD_GetNumInputBuffers",{handle,0}),0u);EXPECT_EQ(get_last_error(),87u);
        for(const auto* name:{"HidD_GetManufacturerString","HidD_GetProductString","HidD_GetSerialNumberString"}) {
            EXPECT_EQ(call(name,{handle,0,256}),0u);EXPECT_EQ(get_last_error(),998u);
        }
        const auto& l=gamepad_hid::layout(is64);
        std::array<u8,12> attrs;attrs.fill(0xa5);
        ASSERT_TRUE(memory.write_bytes(0x12000,attrs.data(),attrs.size()).is_ok());
        set_last_error(0xdead);EXPECT_EQ(call("HidD_GetAttributes",{handle,0x12000}),1u);
        EXPECT_EQ(get_last_error(),0xdeadu);EXPECT_EQ(memory.read_u32(0x12000).value(),l.sizeof_HIDD_ATTRIBUTES);
        EXPECT_EQ(memory.read_u16(0x12008).value(),1u);EXPECT_EQ(memory.read_u16(0x1200a).value(),0xa5a5u);
        // Real Windows short strings fail1784 and preserve all caller bytes.
        EXPECT_EQ(call("HidD_GetProductString",{handle,0x12000,1}),0u);
        EXPECT_EQ(get_last_error(),error_codes::ERROR_INVALID_USER_BUFFER);
        EXPECT_EQ(memory.read_u32(0x12000).value(),l.sizeof_HIDD_ATTRIBUTES);
        set_last_error(0xdead);EXPECT_EQ(call("HidD_GetProductString",{handle,0x13000,128}),1u);EXPECT_EQ(get_last_error(),0xdeadu);
        EXPECT_EQ(memory.read_u16(0x13000).value(),'V');
        EXPECT_EQ(call("HidD_GetNumInputBuffers",{handle,0x14000}),1u);EXPECT_EQ(memory.read_u32(0x14000).value(),32u);
        for(u32 size:{0u,1u,513u}) {EXPECT_EQ(call("HidD_SetNumInputBuffers",{handle,size}),0u);EXPECT_EQ(get_last_error(),87u);}
        EXPECT_EQ(call("HidD_SetNumInputBuffers",{handle,2}),1u);
        // DeviceIoControl failure preserves count and copies only available
        // descriptor prefix; NT_ERROR leaves both IOSB members unchanged.
        ASSERT_TRUE(memory.write_u32(0x14000,0xdead).is_ok());
        EXPECT_EQ(call("DeviceControlProbe",{handle,hid_sdk::kIOCTL_HID_GET_COLLECTION_INFORMATION,0,0,0x12000,1,0x14000,0}),0u);
        EXPECT_EQ(get_last_error(),1784u);EXPECT_EQ(memory.read_u32(0x14000).value(),0xdeadu);
        EXPECT_EQ(call("DeviceControlProbe",{handle,hid_sdk::kIOCTL_HID_GET_COLLECTION_INFORMATION,0,0,0x12000,12,0x14000,0}),1u);
        EXPECT_EQ(memory.read_u32(0x14000).value(),12u);EXPECT_EQ(memory.read_u32(0x12000).value(),32u);
        ASSERT_TRUE(memory.write_u32(0x15000,0xa5a5a5a5u).is_ok());
        ASSERT_TRUE(memory.write_u64(0x14000,0x0000dead0000deadull).is_ok());
        ASSERT_TRUE(memory.write_u64(0x14008,0xdead).is_ok());
        EXPECT_EQ(call("NtDeviceControlProbe",{handle,0,0,0,0x14000,hid_sdk::kIOCTL_HID_GET_COLLECTION_DESCRIPTOR,0,0,0x15000,1}),hid_sdk::kSTATUS_INVALID_BUFFER_SIZE);
        EXPECT_EQ(memory.read_u8(0x15000).value(),'V');EXPECT_EQ(memory.read_u8(0x15001).value(),0xa5u);
        EXPECT_EQ(memory.read_u64(0x14000).value(),0x0000dead0000deadull);EXPECT_EQ(memory.read_u64(0x14008).value(),0xdeadu);
        const auto duplicate=files.duplicate_handle(handle,0,GENERIC_READ);ASSERT_NE(duplicate,NULL_HANDLE);
        const auto second=files.create_file(devices[0].path(),GENERIC_READ,3,OPEN_EXISTING,0);ASSERT_NE(second,INVALID_HANDLE);
        u32 actual=0;EXPECT_TRUE(files.read_hid(duplicate,0x15000,14,0x14000,&actual));EXPECT_EQ(actual,14u);
        EXPECT_EQ(memory.read_u8(0x1500b).value(),1u);
        // Closing one duplicate cannot close the shared open file object.
        EXPECT_TRUE(files.close_file(handle));
        EXPECT_TRUE(handles.get_shared<HidOpenFile>(duplicate,HandleType::HidDevice)->reports.empty());
        EXPECT_TRUE(files.read_hid(second,0x15000,14,0x14000,&actual));
        const auto async=files.create_file(devices[0].path(),GENERIC_READ,3,OPEN_EXISTING,hid_sdk::kFILE_FLAG_OVERLAPPED);ASSERT_NE(async,INVALID_HANDLE);
        EXPECT_EQ(sync.wait_for_single_object(async,0),0u);
        EXPECT_TRUE(files.read_hid(async,0x15000,14,0,&actual));
        EXPECT_EQ(files.begin_overlapped_hid_read(async,0x15000,14,0x16000,is64,actual),OverlappedPipeIoResult::Pending);
        EXPECT_EQ(sync.wait_for_single_object(async,0),258u);
        ASSERT_TRUE(memory.write_u32(0x14000,0xdead).is_ok());
        EXPECT_EQ(call("OverlappedResultProbe",{async,0x16000,0x14000,0}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_IO_INCOMPLETE);
        EXPECT_EQ(memory.read_u32(0x14000).value(),0xdeadu);
        EXPECT_EQ(files.cancel_hid_io(async,0x16000,false),1u);
        EXPECT_EQ(sync.wait_for_single_object(async,2000),0u);
        EXPECT_EQ(call("OverlappedResultProbe",{async,0x16000,0x14000,0}),0u);
        EXPECT_EQ(get_last_error(),error_codes::ERROR_OPERATION_ABORTED);
        EXPECT_EQ(memory.read_u32(0x14000).value(),0u);
        EXPECT_EQ(is64 ? memory.read_u64(0x16000).value() : memory.read_u32(0x16000).value(),hid_io_status::kSTATUS_CANCELLED);
        // A completion route belongs to the shared file object. Alias both
        // before and after association, close the original, then complete via
        // the earlier alias without losing key, port, or shared signal state.
        const auto before=files.duplicate_handle(async,0,GENERIC_READ);ASSERT_NE(before,NULL_HANDLE);
        const auto port=call("CreatePortProbe",{async,0,0x1234,0});ASSERT_NE(port,0u);
        const auto after=files.duplicate_handle(async,0,GENERIC_READ);ASSERT_NE(after,NULL_HANDLE);
        EXPECT_TRUE(kernel.has_iocp_association(before));EXPECT_TRUE(kernel.has_iocp_association(after));
        EXPECT_EQ(files.begin_overlapped_hid_read(async,0x15000,14,0x16020,is64,actual),OverlappedPipeIoResult::Pending);
        EXPECT_TRUE(files.close_file(async));
        EXPECT_EQ(sync.wait_for_single_object(before,0),258u); // issuer close is not last-file cleanup
        host->buttons=hid_sdk::kXINPUT_GAMEPAD_B;
        EXPECT_EQ(call("DequeuePortProbe",{port,0x14000,0x14010,0x14020,2000}),1u);
        EXPECT_EQ(memory.read_u32(0x14000).value(),14u);
        EXPECT_EQ(is64 ? memory.read_u64(0x14010).value() : memory.read_u32(0x14010).value(),0x1234u);
        EXPECT_EQ(is64 ? memory.read_u64(0x14020).value() : memory.read_u32(0x14020).value(),0x16020u);
        EXPECT_EQ(memory.read_u8(0x1500b).value(),2u);EXPECT_EQ(sync.wait_for_single_object(after,2000),0u);
        const u32 modes=hid_sdk::kFILE_SKIP_SET_EVENT_ON_HANDLE|hid_sdk::kFILE_SKIP_COMPLETION_PORT_ON_SUCCESS;
        EXPECT_EQ(call("CompletionModesProbe",{after,modes}),1u);
        EXPECT_EQ(files.begin_overlapped_hid_read(before,0x15000,14,0x16040,is64,actual),OverlappedPipeIoResult::Pending);
        EXPECT_EQ(call("CancelExProbe",{second,0x16040}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_NOT_FOUND);
        set_last_error(0xdead);EXPECT_EQ(call("CancelExProbe",{after,0x16040}),1u);EXPECT_EQ(get_last_error(),0xdeadu);
        EXPECT_EQ(call("DequeuePortProbe",{port,0x14000,0x14010,0x14020,2000}),0u);
        EXPECT_EQ(get_last_error(),error_codes::ERROR_OPERATION_ABORTED);
        EXPECT_EQ(memory.read_u32(0x14000).value(),0u);
        EXPECT_EQ(is64 ? memory.read_u64(0x14020).value() : memory.read_u32(0x14020).value(),0x16040u);
        EXPECT_EQ(sync.wait_for_single_object(after,0),258u);
        EXPECT_EQ(files.begin_overlapped_hid_read(before,0x15000,14,0x16060,is64,actual),OverlappedPipeIoResult::Pending);
        set_last_error(0xdead);EXPECT_EQ(call("CancelProbe",{after}),1u);EXPECT_EQ(get_last_error(),0xdeadu);
        EXPECT_EQ(call("DequeuePortProbe",{port,0x14000,0x14010,0x14020,2000}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_OPERATION_ABORTED);
        EXPECT_EQ(call("CancelExProbe",{after,0x16060}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_NOT_FOUND);
        set_last_error(0xdead);EXPECT_EQ(call("CancelProbe",{after}),1u);EXPECT_EQ(get_last_error(),0xdeadu);
        // HID collection I/O is valid before any window and while User32 is
        // inactive. Monitoring is owned by actual file consumers, not focus.
        EXPECT_EQ(catalog->devices().size(),1u);
        ASSERT_TRUE(memory.write_u8(0x15000,1).is_ok());
        EXPECT_EQ(call("HidD_GetInputReport",{second,0x15000,14}),1u);
        EXPECT_EQ(memory.read_u8(0x1500b).value(),2u);
        EXPECT_EQ(call("HidD_FlushQueue",{second}),1u);
        host->buttons=hid_sdk::kXINPUT_GAMEPAD_A;
        EXPECT_TRUE(files.read_hid(second,0x15000,14,0x14000,&actual));
        EXPECT_EQ(actual,14u);EXPECT_EQ(memory.read_u8(0x1500b).value(),1u);
        EXPECT_EQ(call("HidD_FlushQueue",{after}),1u);
        EXPECT_EQ(files.begin_overlapped_hid_read(after,0x15000,14,0x16080,is64,actual),OverlappedPipeIoResult::Pending);
        host->buttons=hid_sdk::kXINPUT_GAMEPAD_B;
        EXPECT_EQ(call("DequeuePortProbe",{port,0x14000,0x14010,0x14020,2000}),1u);
        EXPECT_EQ(memory.read_u8(0x1500b).value(),2u);
        EXPECT_EQ(call("HidD_GetAttributes",{second,0x12000}),1u);
        EXPECT_EQ(call("HidD_FlushQueue",{after}),1u);
        EXPECT_EQ(files.begin_overlapped_hid_read(before,0x15000,14,0x160a0,is64,actual),OverlappedPipeIoResult::Pending);
        EXPECT_TRUE(files.close_file(before)); // another alias still owns the open file
        EXPECT_EQ(sync.wait_for_single_object(after,0),258u);
        EXPECT_TRUE(files.close_file(after)); // last real handle cancels retained IRPs
        EXPECT_EQ(call("DequeuePortProbe",{port,0x14000,0x14010,0x14020,2000}),0u);EXPECT_EQ(get_last_error(),error_codes::ERROR_OPERATION_ABORTED);
        EXPECT_EQ(is64 ? memory.read_u64(0x160a0).value() : memory.read_u32(0x160a0).value(),hid_io_status::kSTATUS_CANCELLED);
        files.shutdown_hid_io();
        EXPECT_TRUE(files.close_file(second));EXPECT_TRUE(files.close_file(duplicate));EXPECT_TRUE(handles.close(port));
        kernel.set_file_manager(nullptr);
        files.set_hid_waitable_callbacks({},{});
        handles.set_close_callback({});
    }
};
TEST_F(GamepadHidDevice, MetadataReportsLifetimeAndCancelX86) {verify_device(false);}
TEST_F(GamepadHidDevice, MetadataReportsLifetimeAndCancelX64) {verify_device(true);}
TEST_F(GamepadHidDevice, ExplicitEventReadCompletionX86) {verify_explicit_events(false);}
TEST_F(GamepadHidDevice, ExplicitEventReadCompletionX64) {verify_explicit_events(true);}
TEST_F(GamepadHidDevice, OracleEventSubmissionX86) {verify_explicit_events(false,true);}
TEST_F(GamepadHidDevice, OracleEventSubmissionX64) {verify_explicit_events(true,true);}

TEST_F(GamepadHidDevice, MetadataOnlyOpenDoesNotStartPeriodicInputProducer) {
    setup(true);capsule::Vfs vfs;HandleTable handles;FileManager files(vfs,handles,memory);
    handles.set_close_callback([&](Handle h,HandleType type,void* object){return files.on_handle_close(h,type,object);});
    auto host=std::make_shared<HidFixtureHost>();auto catalog=std::make_shared<GamepadHidCatalog>(host);files.set_gamepad_catalog(catalog);
    const auto devices=catalog->devices();ASSERT_EQ(devices.size(),1);
    const auto handle=files.create_file(devices[0].path(),0,3,OPEN_EXISTING,0);ASSERT_NE(handle,INVALID_HANDLE);
    const u32 queried=host->queries.load();std::this_thread::sleep_for(std::chrono::milliseconds(32));
    EXPECT_EQ(host->queries.load(),queried);
    EXPECT_TRUE(files.close_file(handle));handles.set_close_callback({});
}

TEST_F(GamepadHidDevice, SynchronousReadSurvivesIssuingHandleCloseWhileAliasRemains) {
    setup(true);capsule::Vfs vfs;HandleTable handles;FileManager files(vfs,handles,memory);
    handles.set_close_callback([&](Handle h,HandleType type,void* object){return files.on_handle_close(h,type,object);});
    struct Watch {std::mutex mutex;std::condition_variable cv;std::thread::id reader;bool seen{false};};
    auto watch=std::make_shared<Watch>();auto host=std::make_shared<HidFixtureHost>();
    host->observer=[watch] {
        std::lock_guard lock(watch->mutex);
        if(std::this_thread::get_id()==watch->reader) {watch->seen=true;watch->cv.notify_one();}
    };
    auto catalog=std::make_shared<GamepadHidCatalog>(host);files.set_gamepad_catalog(catalog);
    const auto devices=catalog->devices();ASSERT_EQ(devices.size(),1);
    const auto original=files.create_file(devices[0].path(),GENERIC_READ,3,OPEN_EXISTING,0);ASSERT_NE(original,INVALID_HANDLE);
    const auto alias=files.duplicate_handle(original,0,GENERIC_READ);ASSERT_NE(alias,NULL_HANDLE);
    u32 actual=0;ASSERT_TRUE(files.read_hid(original,0x15000,14,0,&actual));
    std::promise<std::pair<bool,u32>> completed;auto result=completed.get_future();
    std::thread reader([&] {
        {std::lock_guard lock(watch->mutex);watch->reader=std::this_thread::get_id();}
        u32 received=0;const bool ok=files.read_hid(original,0x15000,14,0,&received);
        completed.set_value({ok,received});
    });
    bool entered=false;
    {std::unique_lock lock(watch->mutex);entered=watch->cv.wait_for(lock,std::chrono::seconds(2),[&]{return watch->seen;});}
    EXPECT_TRUE(entered);EXPECT_TRUE(files.close_file(original));
    if(entered) {
        EXPECT_EQ(result.wait_for(std::chrono::milliseconds(32)),std::future_status::timeout);
        host->buttons=hid_sdk::kXINPUT_GAMEPAD_B;
    }
    const bool ready=result.wait_for(std::chrono::seconds(2))==std::future_status::ready;
    if(!ready) (void)files.close_file(alias); // bounded failure cleanup releases the blocked reader
    reader.join();const auto [ok,received]=result.get();
    EXPECT_TRUE(ready);EXPECT_TRUE(ok);EXPECT_EQ(received,14u);
    if(ok) EXPECT_EQ(memory.read_u8(0x1500b).value(),2u);
    if(handles.is_valid(alias)) EXPECT_TRUE(files.close_file(alias));
    files.shutdown_hid_io();handles.set_close_callback({});
}

TEST(GamepadIoEvent, NativeReferenceOutlivesManagerAndLastHandle) {
    HandleTable handles;std::shared_ptr<SyncEvent> pin;u64 native=0;
    {
        SyncManager sync(handles);const Handle event=sync.create_event(true,false,"");ASSERT_NE(event,NULL_HANDLE);
        u32 error=0;pin=sync.reference_event_for_io(event,error);ASSERT_TRUE(pin);EXPECT_EQ(error,0u);
        native=pin->nt_id;ASSERT_NE(native,0u);EXPECT_TRUE(sync.close_handle(event));
        EXPECT_TRUE(ntsync::global().snapshot(native));
    }
    EXPECT_TRUE(pin->signal_for_io());bool manual=false,signaled=false;EXPECT_TRUE(pin->query_for_io(manual,signaled));EXPECT_TRUE(signaled);
    pin.reset();EXPECT_FALSE(ntsync::global().snapshot(native));
}

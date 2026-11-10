// SPDX-License-Identifier: LGPL-2.1-only
// Expected values: Windows SDK + tools/oracle_probe/mscms_probe{32,64}.expected.
#include <gtest/gtest.h>
#include "winapi/system/mscms/mscms.hpp"
#include "winapi/system/mscms/mscms_contract.inc"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "core/memory/guest_memory.hpp"
#include "capsule/vfs/vfs.hpp"
#include "capsule/vfs/host_filesystem.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
using namespace vitreo;
using namespace vitreo::winapi;
namespace {
constexpr GuestAddr descriptor=0x12000, data=0x13000, output=0x15000;
constexpr GuestAddr handles=0x16000, intents=0x17000, input_colors=0x18000, output_colors=0x19000;
constexpr u32 sentinel=0x12345678;
class Mscms : public ::testing::TestWithParam<bool> {
protected:
    std::string root;
    capsule::Vfs vfs;
    GuestMemory memory;
    cpu::CpuState cpu;
    Marshaler m{memory};
    DllExports exports;
    std::vector<u8> fixture;
    void SetUp() override {
        char temp[]="/tmp/vitreo-mscms-test-XXXXXX";
        root=::mkdtemp(temp);
        std::filesystem::create_directories(root+"/Windows/System32");
        ASSERT_TRUE(vfs.mount_drive('C',std::make_unique<capsule::HostFileSystem>(root)).is_ok());
        ASSERT_TRUE(memory.map_region(0x10000,0x40000,MemoryProtection::ReadWrite).is_ok());
        if(GetParam())cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=0x10800;m.attach_cpu(cpu);exports=create_mscms_thunks(&vfs);
        std::ifstream file(std::string(VITREO_ORACLE_PROBE_DIR)+"/mscms_srgb.icc",std::ios::binary);
        fixture.assign(std::istreambuf_iterator<char>(file),{});ASSERT_FALSE(fixture.empty());
    }
    void TearDown() override { exports.entries.clear();std::filesystem::remove_all(root); }
    u64 call(const icm::ExportContract& contract,std::initializer_list<u64> args) {
        usize i=0;cpu.pending_raise.active=false;
        for(u64 value:args){
            if(GetParam())switch(i){case 0:cpu.gpr.rcx=value;break;case 1:cpu.gpr.rdx=value;break;case 2:cpu.gpr.r8=value;break;case 3:cpu.gpr.r9=value;break;default:EXPECT_TRUE(memory.write_u64(cpu.gpr.rsp+8+i*8,value).is_ok());break;}
            else EXPECT_TRUE(memory.write_u32(cpu.gpr.rsp+4+i*4,static_cast<u32>(value)).is_ok());++i;
        }
        for(auto& entry:exports.entries)if(entry.name==contract.name){EXPECT_EQ(entry.stdcall_bytes,contract.stack_bytes);entry.handler(m);return cpu.gpr.rax;}
        ADD_FAILURE()<<contract.name;return ~u64{0};
    }
    void pointer(GuestAddr at,u64 value){if(GetParam())ASSERT_TRUE(memory.write_u64(at,value).is_ok());else ASSERT_TRUE(memory.write_u32(at,static_cast<u32>(value)).is_ok());}
    void set_descriptor(u32 type,GuestAddr at,u32 size){
        ASSERT_TRUE(memory.write_u32(descriptor,type).is_ok());
        pointer(descriptor+(GetParam()?icm::PROFILE_pProfileData_OFFSET64:icm::PROFILE_pProfileData_OFFSET32),at);
        ASSERT_TRUE(memory.write_u32(descriptor+(GetParam()?icm::PROFILE_cbDataSize_OFFSET64:icm::PROFILE_cbDataSize_OFFSET32),size).is_ok());
    }
    u64 open_memory(u32 access=icm::PROFILE_READ){
        EXPECT_TRUE(memory.write_bytes(data,fixture.data(),fixture.size()).is_ok());
        set_descriptor(icm::PROFILE_MEMBUFFER,data,static_cast<u32>(fixture.size()));
        return call(icm::OpenColorProfileW,{descriptor,access,~u32{0},~u32{0}});
    }
    void file_descriptor(const std::string& path){
        ASSERT_TRUE(m.write_wide_string(data,path).is_ok());set_descriptor(icm::PROFILE_FILENAME,data,static_cast<u32>((path.size()+1)*sizeof(u16)));
    }
    u64 transform(u64 first,u64 last,u32 flags=icm::NORMAL_MODE,u32 count=1,u32 intent=icm::INTENT_PERCEPTUAL){
        pointer(handles,first);pointer(handles+(GetParam()?sizeof(u64):sizeof(u32)),last);
        EXPECT_TRUE(memory.write_u32(intents,intent).is_ok());
        EXPECT_TRUE(memory.write_u32(intents+sizeof(u32),icm::INTENT_RELATIVE_COLORIMETRIC).is_ok());
        return call(icm::CreateMultiProfileTransform,{handles,2,intents,count,flags,icm::INDEX_DONT_CARE});
    }
    void colors(){
        const u32 stride=GetParam()?icm::COLOR_SIZE64:icm::COLOR_SIZE32;
        for(u32 i=0;i<3;++i){
            for(u32 j=0;j<stride;++j){ASSERT_TRUE(memory.write_u8(input_colors+i*stride+j,j>=3*sizeof(u16)?0x22:0).is_ok());ASSERT_TRUE(memory.write_u8(output_colors+i*stride+j,0xab).is_ok());}
        }
        for(u32 c=0;c<3;++c)ASSERT_TRUE(memory.write_u16(input_colors+stride+c*sizeof(u16),65535).is_ok());
        ASSERT_TRUE(memory.write_u16(input_colors+stride*2,65535).is_ok());
    }
    void check_colors(GuestAddr at=output_colors){
        const u32 stride=GetParam()?icm::COLOR_SIZE64:icm::COLOR_SIZE32;
        // Oracle: black=(0,0,0), white=(65535,65535,65535), red=(65535,0,0).
        for(u32 i=0;i<3;++i)for(u32 c=0;c<3;++c){const u16 expected=icm::ORACLE_RGB[i][c];EXPECT_EQ(memory.read_u16(at+i*stride+c*sizeof(u16)).value(),expected);}
        for(u32 i=0;i<3;++i)for(u32 j=3*sizeof(u16);j<stride;++j)EXPECT_EQ(memory.read_u8(at+i*stride+j).value(),0x22);
    }
};
TEST_P(Mscms, MemoryProfileCopiesInputAndReturnsRealHeader){
    for(u32 access:{icm::PROFILE_READ,icm::PROFILE_READWRITE}){
        const u64 profile=open_memory(access);ASSERT_NE(profile,0u);
        ASSERT_TRUE(memory.write_u32(data,0).is_ok());
        ASSERT_EQ(call(icm::IsColorProfileValid,{profile,output}),1u);EXPECT_EQ(memory.read_u32(output).value(),1u);
        set_last_error(sentinel);ASSERT_EQ(call(icm::GetColorProfileHeader,{profile,output}),1u);EXPECT_EQ(get_last_error(),sentinel);
        EXPECT_EQ(memory.read_u32(output+icm::PROFILEHEADER_phSignature_OFFSET32).value(),icm::ORACLE_SIGNATURE);
        EXPECT_EQ(memory.read_u32(output+icm::PROFILEHEADER_phClass_OFFSET32).value(),icm::ORACLE_CLASS);
        EXPECT_EQ(memory.read_u32(output+icm::PROFILEHEADER_phDataColorSpace_OFFSET32).value(),icm::ORACLE_SPACE);
        ASSERT_EQ(call(icm::CloseColorProfile,{profile}),1u);EXPECT_EQ(get_last_error(),sentinel);
        ASSERT_EQ(call(icm::CloseColorProfile,{profile}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
    }
}
TEST_P(Mscms, InvalidArgumentsPreserveOutputsAndFaultOnlyWhereWindowsDoes){
    EXPECT_EQ(call(icm::OpenColorProfileW,{0,icm::PROFILE_READ,0,icm::OPEN_EXISTING}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
    open_memory();
    for(u32 access:{0u,3u}){EXPECT_EQ(call(icm::OpenColorProfileW,{descriptor,access,0,0}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);}
    ASSERT_TRUE(memory.write_u32(output,sentinel).is_ok());
    EXPECT_EQ(call(icm::IsColorProfileValid,{0,output}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);EXPECT_EQ(memory.read_u32(output).value(),sentinel);
    EXPECT_EQ(call(icm::GetColorProfileHeader,{0,output}),0u);EXPECT_EQ(memory.read_u32(output).value(),sentinel);
    EXPECT_EQ(call(icm::DeleteColorTransform,{0}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(icm::CloseColorProfile,{0}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
    const u64 profile=open_memory();ASSERT_NE(profile,0u);
    EXPECT_EQ(call(icm::IsColorProfileValid,{profile,0}),0u);EXPECT_FALSE(cpu.pending_raise.active);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(icm::GetColorProfileHeader,{profile,0}),0u);EXPECT_TRUE(cpu.pending_raise.active);
    EXPECT_EQ(call(icm::IsColorProfileValid,{1,output}),0u);EXPECT_TRUE(cpu.pending_raise.active);EXPECT_EQ(memory.read_u32(output).value(),sentinel);
    std::vector<u8> junk(sizeof(icm::BLANK_PROFILE));ASSERT_TRUE(memory.write_bytes(data,junk.data(),junk.size()).is_ok());set_descriptor(icm::PROFILE_MEMBUFFER,data,static_cast<u32>(junk.size()));
    EXPECT_EQ(call(icm::OpenColorProfileW,{descriptor,icm::PROFILE_READ,0,0}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PROFILE);
}
TEST_P(Mscms, RealTransformsSurviveProfileClosureAndPreserveColorStride){
    const u64 a=open_memory(),b=open_memory();ASSERT_NE(a,0u);ASSERT_NE(b,0u);
    for(u32 intent=icm::INTENT_PERCEPTUAL;intent<=icm::INTENT_ABSOLUTE_COLORIMETRIC;++intent){
        for(u32 mode:{0u,icm::NORMAL_MODE,icm::BEST_MODE}){
            const u64 t=transform(a,b,mode,1,intent);ASSERT_NE(t,0u);colors();set_last_error(sentinel);
            EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,3,icm::COLOR_RGB,output_colors,icm::COLOR_RGB}),1u);EXPECT_EQ(get_last_error(),sentinel);check_colors();
            EXPECT_EQ(call(icm::DeleteColorTransform,{t}),1u);EXPECT_EQ(get_last_error(),sentinel);
        }
    }
    const u64 t=transform(a,b);ASSERT_NE(t,0u);EXPECT_EQ(call(icm::CloseColorProfile,{a}),1u);EXPECT_EQ(call(icm::CloseColorProfile,{b}),1u);
    colors();EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,3,icm::COLOR_RGB,output_colors,icm::COLOR_RGB}),1u);check_colors();
    colors();const GuestAddr overlap=input_colors+(GetParam()?icm::COLOR_SIZE64:icm::COLOR_SIZE32);
    EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,3,icm::COLOR_RGB,overlap,icm::COLOR_RGB}),1u);check_colors(overlap);
    EXPECT_EQ(call(icm::DeleteColorTransform,{t}),1u);EXPECT_EQ(call(icm::DeleteColorTransform,{t}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
}
TEST_P(Mscms, UncalibratedGrayFormatFailsBeforeWritingOutputs){
    const u64 rgb=open_memory();ASSERT_NE(rgb,0u);
    std::ifstream file(std::string(VITREO_ORACLE_PROBE_DIR)+"/mscms_gray.icc",std::ios::binary);
    fixture.assign(std::istreambuf_iterator<char>(file),{});ASSERT_FALSE(fixture.empty());
    const u64 gray=open_memory();ASSERT_NE(gray,0u);const u64 t=transform(rgb,gray);ASSERT_NE(t,0u);colors();
    EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,3,icm::COLOR_RGB,output_colors,icm::COLOR_GRAY}),0u);
    EXPECT_EQ(get_last_error(),icm::ERROR_CALL_NOT_IMPLEMENTED);
    EXPECT_EQ(memory.read_u16(output_colors).value(),0xababu);
}
TEST_P(Mscms, ClosingProfileReleasesFileShareWhileNativeTransformRemainsUsable){
    const std::string host=root+"/Windows/System32/spool/drivers/color/source.icc";
    {std::ofstream file(host,std::ios::binary);file.write(reinterpret_cast<const char*>(fixture.data()),fixture.size());}
    file_descriptor("source.icc");
    const u64 source=call(icm::OpenColorProfileW,{descriptor,icm::PROFILE_READ,0,icm::OPEN_EXISTING});ASSERT_NE(source,0u);
    const u64 destination=open_memory();ASSERT_NE(destination,0u);const u64 t=transform(source,destination);ASSERT_NE(t,0u);
    EXPECT_EQ(call(icm::CloseColorProfile,{source}),1u);
    file_descriptor("source.icc");
    const u64 reopened=call(icm::OpenColorProfileW,{descriptor,icm::PROFILE_READWRITE,icm::FILE_SHARE_READ|icm::FILE_SHARE_WRITE,icm::OPEN_EXISTING});
    ASSERT_NE(reopened,0u);colors();
    EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,3,icm::COLOR_RGB,output_colors,icm::COLOR_RGB}),1u);check_colors();
}
TEST_P(Mscms, TransformValidationAndUnsupportedModesAreExplicit){
    const u64 a=open_memory(),b=open_memory();ASSERT_NE(a,0u);ASSERT_NE(b,0u);
    for(u32 invalid:{0u,3u}){EXPECT_EQ(transform(a,b,icm::NORMAL_MODE,invalid),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);}
    for(u32 flags:{icm::WCS_ALWAYS,icm::SEQUENTIAL_TRANSFORM,icm::FAST_TRANSLATE,icm::PROOF_MODE,icm::PRESERVEBLACK,icm::ENABLE_GAMUT_CHECKING}){EXPECT_EQ(transform(a,b,flags),0u);EXPECT_EQ(get_last_error(),icm::ERROR_CALL_NOT_IMPLEMENTED);}
    const u64 t=transform(a,b);ASSERT_NE(t,0u);colors();
    for(u32 type:{0u,13u}){EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,1,type,output_colors,icm::COLOR_RGB}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);}
    EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,0,icm::COLOR_RGB,output_colors,icm::COLOR_RGB}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(icm::TranslateColors,{t,0,1,icm::COLOR_RGB,output_colors,icm::COLOR_RGB}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
    EXPECT_EQ(call(icm::TranslateColors,{t,input_colors,1,icm::COLOR_NAMED,output_colors,icm::COLOR_RGB}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_CALL_NOT_IMPLEMENTED);
}
TEST_P(Mscms, DirectoryQueryUsesByteCapacityAndCreatesRealCapsuleDirectory){
    ASSERT_TRUE(memory.write_u32(output,0).is_ok());EXPECT_EQ(call(icm::GetColorDirectoryW,{0,0,output}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INSUFFICIENT_BUFFER);EXPECT_EQ(memory.read_u32(output).value(),icm::DIRECTORY_BYTES_W32);
    EXPECT_EQ(call(icm::GetColorDirectoryW,{0,data,output}),1u);EXPECT_EQ(m.read_wide_string(data).value(),"C:\\Windows\\System32\\spool\\drivers\\color");EXPECT_TRUE(vfs.stat(m.read_wide_string(data).value()).is_ok());
    EXPECT_EQ(call(icm::GetColorDirectoryW,{data,data,output}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_NAME);
    EXPECT_EQ(call(icm::GetColorDirectoryW,{0,data,0}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_INVALID_PARAMETER);
}
TEST_P(Mscms, AllFileCreationModesMatchWindowsAndKeepSharingUntilClose){
    const std::string host=root+"/Windows/System32/spool/drivers/color/test.icc";
    for(bool exists:{false,true})for(u32 mode:{icm::CREATE_NEW,icm::CREATE_ALWAYS,icm::OPEN_EXISTING,icm::OPEN_ALWAYS,icm::TRUNCATE_EXISTING}){
        std::filesystem::remove(host);if(exists){std::ofstream f(host,std::ios::binary);f.write(reinterpret_cast<const char*>(fixture.data()),fixture.size());}
        file_descriptor("test.icc");const u64 h=call(icm::OpenColorProfileW,{descriptor,icm::PROFILE_READWRITE,icm::FILE_SHARE_READ,mode});
        if((!exists&&(mode==icm::OPEN_EXISTING||mode==icm::TRUNCATE_EXISTING))||(exists&&mode==icm::CREATE_NEW)){
            EXPECT_EQ(h,0u);EXPECT_EQ(get_last_error(),exists?icm::ERROR_FILE_EXISTS:icm::ERROR_FILE_NOT_FOUND);continue;
        }
        ASSERT_NE(h,0u);const bool blank=!exists||mode==icm::CREATE_ALWAYS||mode==icm::TRUNCATE_EXISTING;
        EXPECT_EQ(call(icm::GetColorProfileHeader,{h,output}),1u);EXPECT_EQ(memory.read_u32(output).value(),blank?sizeof(icm::BLANK_PROFILE):fixture.size());
        EXPECT_EQ(call(icm::IsColorProfileValid,{h,output}),blank?0u:1u);EXPECT_EQ(memory.read_u32(output).value(),blank?0u:1u);
        EXPECT_EQ(call(icm::CloseColorProfile,{h}),1u);EXPECT_EQ(std::filesystem::file_size(host),blank?sizeof(icm::BLANK_PROFILE):fixture.size());
    }
    {std::ofstream f(host,std::ios::binary);f.write(reinterpret_cast<const char*>(fixture.data()),fixture.size());}
    file_descriptor("test.icc");const u64 h=call(icm::OpenColorProfileW,{descriptor,icm::PROFILE_READ,0,icm::OPEN_EXISTING});ASSERT_NE(h,0u);
    EXPECT_EQ(call(icm::OpenColorProfileW,{descriptor,icm::PROFILE_READ,icm::FILE_SHARE_READ,icm::OPEN_EXISTING}),0u);EXPECT_EQ(get_last_error(),icm::ERROR_SHARING_VIOLATION);
    EXPECT_EQ(call(icm::CloseColorProfile,{h}),1u);EXPECT_NE(call(icm::OpenColorProfileW,{descriptor,icm::PROFILE_READ,icm::FILE_SHARE_READ,icm::OPEN_EXISTING}),0u);
}
INSTANTIATE_TEST_SUITE_P(GuestAbis,Mscms,::testing::Values(false,true));
} // namespace

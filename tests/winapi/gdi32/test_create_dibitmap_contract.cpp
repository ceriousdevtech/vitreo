#include <gtest/gtest.h>
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <algorithm>
#include <array>
#include <bit>
using namespace vitreo;
using namespace vitreo::winapi;
namespace sdk {
#include "winapi/graphics/gdi32/create_dib_bitmap_contract.inc"
}
namespace facts=sdk::create_dib_bitmap_contract;
namespace oracle {
#include "../../oracle_data/create_dib_bitmap.generated.inc"
}

class CreateDIBitmapContract : public ::testing::Test {
protected:
    HandleTable handles;
    GuestMemory memory;
    GdiManager manager{handles};
    Handle display=0;
    void header(GuestAddr address,i32 width=2,i32 height=2,u32 depth=32) {
        (void)memory.write_u32(address+facts::OFFSETOF_BITMAPINFOHEADER_BISIZE,facts::SIZEOF_BITMAPINFOHEADER);
        (void)memory.write_u32(address+facts::OFFSETOF_BITMAPINFOHEADER_BIWIDTH,std::bit_cast<u32>(width));
        (void)memory.write_u32(address+facts::OFFSETOF_BITMAPINFOHEADER_BIHEIGHT,std::bit_cast<u32>(height));
        (void)memory.write_u16(address+facts::OFFSETOF_BITMAPINFOHEADER_BIPLANES,static_cast<u16>(facts::compatible_planes));
        (void)memory.write_u16(address+facts::OFFSETOF_BITMAPINFOHEADER_BIBITCOUNT,static_cast<u16>(depth));
        (void)memory.write_u32(address+facts::OFFSETOF_BITMAPINFOHEADER_BICOMPRESSION,facts::BI_RGB);
    }
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory.map_region(GdiManager::GDI_DIB_SCRATCH_BASE,GdiManager::GDI_DIB_SCRATCH_SIZE,
                                      MemoryProtection::ReadWrite).is_ok());
        manager.set_guest_memory(&memory);display=manager.get_dc(0);ASSERT_NE(display,0u);
        header(0x10000);header(0x11000);
        for(usize i=0;i<oracle::probe_input_pixels.size();++i)
            ASSERT_TRUE(memory.write_u32(0x13000+i*sizeof(u32),oracle::probe_input_pixels[i]).is_ok());
    }
    std::array<u64,6> args(u32 flags=facts::CBM_INIT) const {
        return {display,0x10000,flags,0x13000,0x11000,facts::DIB_RGB_COLORS};
    }
    Handle call(bool is64,const std::array<u64,6>& arguments) {
        auto exports=make_gdi32_exports(manager,{},is64);
        const auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),
            [](const ExportEntry& item){return item.name=="CreateDIBitmap";});
        if(entry==exports.entries.end()){ADD_FAILURE()<<"missing CreateDIBitmap";return 0;}
        cpu::CpuState state;
        if(is64) {
            state.init_long_mode();state.gpr.rsp=0x1f000;
            std::array<u64*,4> registers{&state.gpr.rcx,&state.gpr.rdx,&state.gpr.r8,&state.gpr.r9};
            for(usize i=0;i<registers.size();++i)*registers[i]=arguments[i];
            for(usize i=registers.size();i<arguments.size();++i)
                (void)memory.write_u64(0x1f000+(i+1)*sizeof(u64),arguments[i]);
        } else {
            state.init_protected_mode();state.set_sp(0x1f000);
            for(usize i=0;i<arguments.size();++i)
                (void)memory.write_u32(0x1f000+(i+1)*sizeof(u32),static_cast<u32>(arguments[i]));
        }
        Marshaler m(memory);m.attach_cpu(state);set_last_error(0xdeadbeef);
        entry->handler(m);return static_cast<Handle>(state.gpr.rax);
    }
    std::array<u32,4> pixels(Handle bitmap) {
        GdiManager::BitmapRgbaCopy copy;
        if(!manager.copy_bitmap_rgba(bitmap,copy)||copy.rgba.size()<4*sizeof(u32)) {
            ADD_FAILURE()<<"bitmap pixels unavailable";return {};
        }
        std::array<u32,4> values{};
        for(usize i=0;i<values.size();++i)
            values[i]=(static_cast<u32>(copy.rgba[i*4])<<16)|
                      (static_cast<u32>(copy.rgba[i*4+1])<<8)|copy.rgba[i*4+2];
        return values;
    }
};

TEST_F(CreateDIBitmapContract, InitializedPixelsAndTopDownRowsMatchWindowsInBothAbis) {
    for(const bool is64:{false,true})for(const bool top_down:{false,true}) {
        header(0x10000,2,top_down?-2:2);header(0x11000,2,top_down?-2:2);
        const Handle bitmap=call(is64,args());ASSERT_NE(bitmap,0u);
        EXPECT_EQ(get_last_error(),oracle::valid_init_error);
        EXPECT_EQ(pixels(bitmap),top_down?oracle::valid_topdown_pixels:oracle::valid_init_pixels);
        ASSERT_TRUE(manager.delete_object(bitmap));
    }
}

TEST_F(CreateDIBitmapContract, NullParameterRelationshipsMatchWindowsInBothAbis) {
    const std::array<usize,4> positions{0,1,3,4};
    for(const bool is64:{false,true})for(const auto& row:oracle::null_cases) {
        auto arguments=args(row.init?facts::CBM_INIT:0);
        for(usize bit=0;bit<positions.size();++bit)
            if(row.mask&(u32{1}<<bit))arguments[positions[bit]]=0;
        const Handle bitmap=call(is64,arguments);
        EXPECT_EQ(bitmap!=0,row.ok!=0)<<"mask="<<row.mask<<" init="<<row.init;
        EXPECT_EQ(get_last_error(),row.error)<<"mask="<<row.mask<<" init="<<row.init;
        if(bitmap!=0)EXPECT_TRUE(manager.delete_object(bitmap));
    }
}

TEST_F(CreateDIBitmapContract, FailedUploadReleasesItsUnpublishedBitmap) {
    for(const bool is64:{false,true}) {
        const Handle good=call(is64,args());ASSERT_NE(good,0u);ASSERT_TRUE(manager.delete_object(good));
        const usize before=handles.snapshot_handles().size();
        auto arguments=args();arguments[3]=0x30000; // unmapped guest source data
        EXPECT_EQ(call(is64,arguments),0u);
        EXPECT_EQ(handles.snapshot_handles().size(),before);
    }
}

TEST_F(CreateDIBitmapContract, UnreadableHeaderNeverManufacturesACompatibleBitmap) {
    for(const bool is64:{false,true}) {
        const usize before=handles.snapshot_handles().size();
        auto arguments=args();arguments[1]=0x30000;
        EXPECT_EQ(call(is64,arguments),0u);
        EXPECT_EQ(handles.snapshot_handles().size(),before);
    }
}

TEST_F(CreateDIBitmapContract, InvalidDimensionsAndIncompleteSourceNeverBecomeInitializedSuccess) {
    for(const bool is64:{false,true}) {
        header(0x10000,-2,2);
        EXPECT_EQ(call(is64,args()),0u);EXPECT_EQ(get_last_error(),oracle::negative_width_error);
        EXPECT_EQ(call(is64,args(0)),0u);EXPECT_EQ(get_last_error(),oracle::negative_width_noinit_error);
        header(0x10000,2,3);
        EXPECT_EQ(call(is64,args()),0u);EXPECT_EQ(get_last_error(),oracle::header_height_exceeds_info_error);
        header(0x10000,0,2);
        const Handle zero=call(is64,args());ASSERT_NE(zero,0u);
        GdiManager::BitmapRgbaCopy copy;ASSERT_TRUE(manager.copy_bitmap_rgba(zero,copy));
        EXPECT_EQ(copy.width,oracle::zero_width_width);EXPECT_EQ(copy.height,oracle::zero_width_height);
        EXPECT_EQ(copy.bpp,oracle::zero_width_bpp);EXPECT_FALSE(copy.from_dib);
    }
}

TEST_F(CreateDIBitmapContract, NoInitDoesNotConsumeSourceFieldsOrPixelsAndCoreHeadersWork) {
    for(const bool is64:{false,true}) {
        auto arguments=args(0);arguments[3]=0x30000;arguments[4]=0;
        (void)memory.write_u16(0x10000+facts::OFFSETOF_BITMAPINFOHEADER_BIPLANES,0);
        (void)memory.write_u16(0x10000+facts::OFFSETOF_BITMAPINFOHEADER_BIBITCOUNT,0);
        Handle bitmap=call(is64,arguments);ASSERT_NE(bitmap,0u);ASSERT_TRUE(manager.delete_object(bitmap));
        (void)memory.write_u32(0x10000+facts::OFFSETOF_BITMAPCOREHEADER_BCSIZE,facts::SIZEOF_BITMAPCOREHEADER);
        (void)memory.write_u16(0x10000+facts::OFFSETOF_BITMAPCOREHEADER_BCWIDTH,2);
        (void)memory.write_u16(0x10000+facts::OFFSETOF_BITMAPCOREHEADER_BCHEIGHT,2);
        (void)memory.write_u16(0x10000+facts::OFFSETOF_BITMAPCOREHEADER_BCPLANES,static_cast<u16>(facts::compatible_planes));
        (void)memory.write_u16(0x10000+facts::OFFSETOF_BITMAPCOREHEADER_BCBITCOUNT,static_cast<u16>(oracle::probe_core_bit_count));
        bitmap=call(is64,arguments);ASSERT_NE(bitmap,0u);
        GdiManager::BitmapRgbaCopy copy;ASSERT_TRUE(manager.copy_bitmap_rgba(bitmap,copy));
        EXPECT_EQ(copy.width,oracle::valid_core_noinit_width);EXPECT_EQ(copy.height,oracle::valid_core_noinit_height);
        EXPECT_EQ(copy.bpp,oracle::valid_core_noinit_bpp);EXPECT_FALSE(copy.from_dib);
        ASSERT_TRUE(manager.delete_object(bitmap));header(0x10000);
    }
}

TEST_F(CreateDIBitmapContract, DibSectionReferenceStillProducesADdbInBothAbis) {
    for(const bool is64:{false,true})for(const u32 depth:{oracle::reference_dib32_memory_bpp,oracle::reference_dib8_memory_bpp}) {
        header(0x11000,2,2,depth);
        const Handle section=manager.create_dib_section(0,0x11000,facts::DIB_RGB_COLORS,0x17000,is64);
        ASSERT_NE(section,0u);
        const Handle memory_dc=manager.create_compatible_dc(0);ASSERT_NE(memory_dc,0u);
        const Handle previous=manager.select_object(memory_dc,section);
        auto arguments=args(0);arguments[0]=memory_dc;
        const Handle result=call(is64,arguments);ASSERT_NE(result,0u);
        GdiManager::BitmapRgbaCopy copy;ASSERT_TRUE(manager.copy_bitmap_rgba(result,copy));
        EXPECT_EQ(copy.bpp,depth);EXPECT_FALSE(copy.from_dib);
        const u32 capacity=is64?facts::SIZEOF_DIBSECTION64:facts::SIZEOF_DIBSECTION32;
        const i32 returned=manager.get_object(result,static_cast<i32>(capacity),0x15000,is64,false);
        EXPECT_EQ(returned,static_cast<i32>(is64?facts::SIZEOF_BITMAP64:facts::SIZEOF_BITMAP32));
        const u32 offset=is64?facts::OFFSETOF_BITMAP_BMBITS64:facts::OFFSETOF_BITMAP_BMBITS32;
        if(is64)EXPECT_EQ(memory.read_u64(0x15000+offset).value(),0u);
        else EXPECT_EQ(memory.read_u32(0x15000+offset).value(),0u);
        ASSERT_TRUE(manager.delete_object(result));(void)manager.select_object(memory_dc,previous);
        ASSERT_TRUE(manager.delete_dc(memory_dc));ASSERT_TRUE(manager.delete_object(section));
        header(0x11000);
    }
}

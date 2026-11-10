#include <gtest/gtest.h>
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include "winapi/graphics/gdi32/palette_contract.inc"
#include <algorithm>
#include <array>
using namespace vitreo;
using namespace vitreo::winapi;
class PaletteContract : public ::testing::Test {
protected:
    HandleTable handles;
    GuestMemory memory;
    GdiManager manager{handles};
    cpu::CpuState state;
    Handle display{};
    static constexpr u32 sentinel=0xdead;
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
        manager.set_guest_memory(&memory);display=manager.get_dc(0);ASSERT_NE(display,0u);
    }
    u32 call(bool x64,const char* name,std::initializer_list<u64> args) {
        auto exports=make_gdi32_exports(manager,{},x64);
        const auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),
            [name](const auto& item){return item.name==name;});
        if(entry==exports.entries.end()){ADD_FAILURE()<<name;return 0;}
        if(x64) {
            state.init_long_mode();state.gpr.rsp=0x1f000;
            std::array<u64*,4> regs{&state.gpr.rcx,&state.gpr.rdx,&state.gpr.r8,&state.gpr.r9};
            usize i=0;for(auto v:args){if(i<regs.size())*regs[i]=v;else(void)memory.write_u64(0x1f000+(i+1)*8,v);++i;}
        }else{
            state.init_protected_mode();state.set_sp(0x1f000);usize i=0;
            for(auto v:args)(void)memory.write_u32(0x1f000+(++i)*4,static_cast<u32>(v));
        }
        state.pending_raise.active=false;
        Marshaler marshaler(memory);marshaler.attach_cpu(state);set_last_error(sentinel);
        entry->handler(marshaler);return static_cast<u32>(state.gpr.rax);
    }
    Handle palette(bool x64,u16 count=4,u16 version=palette_contract::logical_version,u8 flags=0) {
        (void)memory.write_u16(0x11000,version);(void)memory.write_u16(0x11002,count);
        for(u32 i=0;i<count;++i) {
            (void)memory.write_u32(0x11004+i*4,static_cast<u32>(flags)<<24 | ((i+31)&255)<<16 | ((i+17)&255)<<8 | (i&255));
        }
        return call(x64,"CreatePalette",{0x11000});
    }
};
// All expected behaviors below come from the original Windows palette
// lifecycle, edge and contract probes (both ABIs), not the product under test.
TEST_F(PaletteContract, LogicalVersionCountsAndRawFlagsMatchWindows) {
    for(bool x64:{false,true}) {
        for(u16 version:{u16(0),u16(512),u16(768),u16(769)})for(u16 count:{u16(0),u16(1),u16(256),u16(257)}) {
            const auto h=palette(x64,count,version);
            EXPECT_EQ(h!=0,version==palette_contract::logical_version&&count!=0);
            EXPECT_EQ(get_last_error(),sentinel);if(h)EXPECT_TRUE(manager.delete_object(h));
        }
        for(u8 flags:{u8(1),u8(2),u8(4),u8(7),u8(128),u8(255)}) {
            const auto h=palette(x64,4,palette_contract::logical_version,flags);ASSERT_NE(h,0u);
            ASSERT_EQ(manager.get_palette_entries(h,0,4,0x13000),4u);
            for(u32 i=0;i<4;++i)EXPECT_EQ(memory.read_u8(0x13003+i*4).value(),flags);
            EXPECT_TRUE(manager.delete_object(h));
        }
    }
}
TEST_F(PaletteContract, SelectionRejectsInvalidWithoutMutatingAndRealizationMapsNothing) {
    for(bool x64:{false,true})for(bool background:{false,true}) {
        auto dc=manager.create_compatible_dc(display);ASSERT_NE(dc,0u);
        auto stock=manager.get_stock_object(15);auto a=palette(x64),b=palette(x64);
        ASSERT_NE(a,0u);ASSERT_NE(b,0u);
        EXPECT_EQ(call(x64,"SelectPalette",{dc,a,background}),stock);
        EXPECT_EQ(call(x64,"SelectPalette",{dc,a,background}),a);
        for(auto bad:{0u,0xdeadbeefu}) {
            EXPECT_EQ(call(x64,"SelectPalette",{dc,bad,background}),0u);
            EXPECT_EQ(get_last_error(),sentinel);
            EXPECT_EQ(call(x64,"SelectPalette",{dc,b,background}),a);
            EXPECT_EQ(call(x64,"SelectPalette",{dc,a,background}),b);
        }
        EXPECT_EQ(call(x64,"RealizePalette",{dc}),0u);EXPECT_EQ(get_last_error(),sentinel);
        EXPECT_EQ(call(x64,"SelectPalette",{0,a,background}),0u);EXPECT_EQ(get_last_error(),palette_edges_sdk::ERROR_INVALID_HANDLE);
        EXPECT_EQ(call(x64,"RealizePalette",{0}),palette_edges_sdk::GDI_ERROR);EXPECT_EQ(get_last_error(),palette_edges_sdk::ERROR_INVALID_HANDLE);
        (void)manager.select_palette(dc,stock,false);EXPECT_TRUE(manager.delete_dc(dc));
        EXPECT_TRUE(manager.delete_object(a));EXPECT_TRUE(manager.delete_object(b));
        EXPECT_EQ(call(x64,"SelectPalette",{display,a,background}),0u);EXPECT_EQ(get_last_error(),sentinel);
    }
}
TEST_F(PaletteContract, SystemEntriesAreMeasuredFlagsAndTruecolorReturnsZeroEvenAfterWrites) {
    for(bool x64:{false,true})for(auto dc:{display,0u,0xdeadbeefu})for(auto start:{0u,255u,256u}) {
        std::array<Byte,1024> fill;fill.fill(0xa5);ASSERT_TRUE(memory.write_bytes(0x13000,fill.data(),fill.size()).is_ok());
        EXPECT_EQ(call(x64,"GetSystemPaletteEntries",{dc,start,4,0x13000}),0u);
        EXPECT_EQ(get_last_error(),dc==display?sentinel:palette_edges_sdk::ERROR_INVALID_HANDLE);
        const auto n=start<256?std::min(4u,256-start):0u;
        for(u32 i=0;i<16;++i)EXPECT_EQ(memory.read_u8(0x13000+i).value(),i<n*4?palette_edges_oracle::system_entries[start+i/4][i%4]:0xa5);
        EXPECT_EQ(call(x64,"GetSystemPaletteEntries",{dc,0,0xffffffffu,0x13000}),0u);EXPECT_EQ(get_last_error(),sentinel);
        EXPECT_EQ(call(x64,"GetSystemPaletteEntries",{dc,0,256,0}),0u);
        EXPECT_EQ(get_last_error(),dc==display?sentinel:palette_edges_sdk::ERROR_INVALID_HANDLE);
    }
}
TEST_F(PaletteContract, OutputPageFaultsPreserveErrorAndMatchAbiPartialCopy) {
    for(bool x64:{false,true}) {
        const std::array<Byte,8> fill{0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};
        ASSERT_TRUE(memory.write_bytes(0x1fff8,fill.data(),fill.size()).is_ok());
        EXPECT_EQ(call(x64,"GetSystemPaletteEntries",{display,0,4,0x1fff8}),0u);
        EXPECT_TRUE(state.pending_raise.active);EXPECT_EQ(state.pending_raise.args[0],1u);EXPECT_EQ(state.pending_raise.args[1],0x20000u);
        EXPECT_EQ(get_last_error(),sentinel);
        for(u32 i=0;i<8;++i)EXPECT_EQ(memory.read_u8(0x1fff8+i).value(),x64?0xa5:palette_edges_oracle::system_entries[i/4][i%4]);
    }
}
TEST_F(PaletteContract, UnrealizeValidatesObjectTypeAndAcceptsRetiredBrushIdentity) {
    for(bool x64:{false,true}) {
        const auto p=palette(x64);const auto brush=manager.create_solid_brush(0);
        for(auto h:{p,brush,manager.get_stock_object(0),manager.get_stock_object(15)}) {
            EXPECT_EQ(call(x64,"UnrealizeObject",{h}),1u);EXPECT_EQ(get_last_error(),sentinel);
        }
        EXPECT_TRUE(manager.delete_object(brush));EXPECT_EQ(call(x64,"UnrealizeObject",{brush}),1u);
        const auto pen=manager.create_pen(0,1,0);
        const auto region=manager.create_rect_rgn(0,0,1,1);
        for(auto h:{0u,0xdeadbeefu,pen,region,display,manager.get_stock_object(17)}) {
            EXPECT_EQ(call(x64,"UnrealizeObject",{h}),0u);EXPECT_EQ(get_last_error(),sentinel);
        }
        EXPECT_TRUE(manager.delete_object(p));EXPECT_TRUE(manager.delete_object(pen));EXPECT_TRUE(manager.delete_object(region));
    }
}

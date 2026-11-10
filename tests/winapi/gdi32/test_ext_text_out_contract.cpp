#include <gtest/gtest.h>
#include "winapi/graphics/gdi32/gdi32.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/kernel32/error.hpp"
#include "winapi/kernel32/locale/codepage_convert.hpp"
#include "core/cpu/state/cpu_state.hpp"
#include <algorithm>
#include <array>
using namespace vitreo;
using namespace vitreo::winapi;
namespace sdk {
#include "winapi/graphics/gdi32/ext_text_out_contract.inc"
}
namespace facts = sdk::ext_text_out_contract;
namespace oracle {
#include "../../oracle_data/ext_text_out.generated.inc"
}

class ExtTextOutContract : public ::testing::Test {
protected:
    HandleTable handles;
    GdiManager manager{handles};
    GuestMemory memory;
    Handle dc = 0, bitmap = 0;
    void SetUp() override {
        ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
        dc=manager.create_compatible_dc(0);
        bitmap=manager.create_compatible_bitmap(manager.get_dc(0),32,32);
        ASSERT_NE(dc,0u); ASSERT_NE(bitmap,0u); (void)manager.select_object(dc,bitmap);
        (void)manager.set_bk_color(dc,0x0000ff);
        ASSERT_TRUE(manager.fill_rect_gdi(dc,0,0,32,32,0xffffff));
        const std::array<i32,4> rect{0,0,4,4};
        ASSERT_TRUE(memory.write_bytes(0x12000,reinterpret_cast<const Byte*>(rect.data()),sizeof(rect)).is_ok());
        const char16_t text[]=u"AB";
        ASSERT_TRUE(memory.write_bytes(0x11000,reinterpret_cast<const Byte*>(text),sizeof(text)).is_ok());
    }
    u32 call(bool is64,bool wide,u32 options,u32 count,GuestAddr dx=0,GuestAddr rect=0) {
        auto exports=make_gdi32_exports(manager,{},is64);
        const auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),
            [wide](const ExportEntry& value){return value.name==(wide?"ExtTextOutW":"ExtTextOutA");});
        if (entry==exports.entries.end()) { ADD_FAILURE()<<"missing ExtTextOut";return 0; }
        cpu::CpuState state;
        std::array<u64,8> args{dc,20,20,options,rect,0x11000,count,dx};
        if (is64) {
            state.init_long_mode();state.gpr.rsp=0x1f000;
            std::array<u64*,4> regs{&state.gpr.rcx,&state.gpr.rdx,&state.gpr.r8,&state.gpr.r9};
            for (usize i=0;i<regs.size();++i)*regs[i]=args[i];
            for (usize i=regs.size();i<args.size();++i)
                (void)memory.write_u64(0x1f000+(i+1)*sizeof(u64),args[i]);
        } else {
            state.init_protected_mode();state.set_sp(0x1f000);
            for (usize i=0;i<args.size();++i)
                (void)memory.write_u32(0x1f000+(i+1)*sizeof(u32),static_cast<u32>(args[i]));
        }
        Marshaler m(memory);m.attach_cpu(state);set_last_error(0x12345678);
        entry->handler(m);return static_cast<u32>(state.gpr.rax);
    }
};

TEST_F(ExtTextOutContract, AnsiGlyphIndicesRetainObservedOpaqueDrawingInBothAbis) {
    // Learn's no-op prose is contradicted by both real Windows probes: ANSI
    // glyph-index output draws and updates CP like W, including ETO_OPAQUE.
    const std::array<u16,1> glyph{0};
    ASSERT_TRUE(memory.write_bytes(0x11000,reinterpret_cast<const Byte*>(glyph.data()),sizeof(glyph)).is_ok());
    for(const bool is64:{false,true})for(const bool wide:{false,true}) {
        ASSERT_TRUE(manager.fill_rect_gdi(dc,0,0,32,32,0xffffff));
        EXPECT_EQ(call(is64,wide,facts::ETO_GLYPH_INDEX|facts::ETO_OPAQUE,1,0,0x12000),oracle::a_glyph_opaque_result);
        EXPECT_EQ(manager.get_pixel(dc,1,1),0x0000ffu);
        EXPECT_EQ(get_last_error(),oracle::a_glyph_opaque_error);
    }
}

TEST_F(ExtTextOutContract, PdyRequiresAnArrayAndZeroVerticalAdvancesMatchWindows) {
    const std::array<i32,4> pdy{7,0,9,0};
    ASSERT_TRUE(memory.write_bytes(0x12500,reinterpret_cast<const Byte*>(pdy.data()),sizeof(pdy)).is_ok());
    for(const bool is64:{false,true}) {
        EXPECT_EQ(call(is64,true,facts::ETO_PDY,0),oracle::w_pdy_empty_result);
        EXPECT_EQ(call(is64,true,facts::ETO_PDY,1),oracle::w_pdy_null_dx_result);
        (void)manager.set_text_align_mode(dc,facts::TA_UPDATECP);
        ASSERT_TRUE(manager.move_to_ex(dc,10,10,nullptr,nullptr));
        EXPECT_EQ(call(is64,true,facts::ETO_PDY,2,0x12500),oracle::w_pdy_zero_result);
        EXPECT_EQ(get_last_error(),oracle::w_pdy_zero_error);
        i32 x=0,y=0;ASSERT_TRUE(manager.current_position(dc,x,y));
        EXPECT_EQ(x,oracle::w_pdy_zero_x);EXPECT_EQ(y,oracle::w_pdy_zero_y);
    }
}

TEST_F(ExtTextOutContract, UnsupportedVerticalDisplacementFailsBeforeStateOrPixelsChange) {
    // The renderer has no vertical advance representation yet. The required
    // behavior for this unsupported capability is a visible failure, never
    // a successful flattened draw (Windows' full behavior is in the probe).
    const std::array<i32,4> pdy{7,3,9,4};
    ASSERT_TRUE(memory.write_bytes(0x12500,reinterpret_cast<const Byte*>(pdy.data()),sizeof(pdy)).is_ok());
    (void)manager.set_text_align_mode(dc,facts::TA_UPDATECP);
    for(const bool is64:{false,true})for(const bool wide:{false,true}) {
        ASSERT_TRUE(manager.move_to_ex(dc,10,10,nullptr,nullptr));
        EXPECT_EQ(call(is64,wide,facts::ETO_PDY|facts::ETO_OPAQUE,2,0x12500,0x12000),0u);
        i32 x=0,y=0;ASSERT_TRUE(manager.current_position(dc,x,y));
        EXPECT_EQ(x,10);EXPECT_EQ(y,10);EXPECT_EQ(manager.get_pixel(dc,1,1),0xffffffu);
    }
}

TEST_F(ExtTextOutContract, UnsupportedCountFailsBeforeOpaqueDrawing) {
    // Windows currently accepts 8193, despite Learn's8192 documented maximum.
    // Vitreo explicitly rejects the unimplemented range before doing any work.
    for(const bool is64:{false,true})for(const bool wide:{false,true}) {
        EXPECT_EQ(call(is64,wide,facts::ETO_OPAQUE,facts::documented_max_count+1,0,0x12000),0u);
        EXPECT_EQ(manager.get_pixel(dc,1,1),0xffffffu);
    }
}

TEST_F(ExtTextOutContract, AnsiByteSpacingUsesSystemAcpDespiteUtf8ProcessManifest) {
    // Learn's UTF-8 page explicitly excludes GDI from per-process activeCodePage.
    // The byte-count/current-position result was measured on system1252 Windows.
    struct RestoreAcp { u32 old=codepage::ansi_code_page();~RestoreAcp(){codepage::set_ansi_code_page(old);} } restore;
    codepage::set_ansi_code_page(facts::CP_UTF8);
    const std::array<u8,4> text{0xe2,0x82,0xac,'A'};
    const std::array<i32,4> dx{2,3,4,5};
    ASSERT_TRUE(memory.write_bytes(0x11000,reinterpret_cast<const Byte*>(text.data()),sizeof(text)).is_ok());
    ASSERT_TRUE(memory.write_bytes(0x12500,reinterpret_cast<const Byte*>(dx.data()),sizeof(dx)).is_ok());
    (void)manager.set_text_align_mode(dc,facts::TA_UPDATECP);
    for(const bool is64:{false,true}) {
        ASSERT_TRUE(manager.move_to_ex(dc,10,10,nullptr,nullptr));
        EXPECT_EQ(call(is64,false,0,text.size(),0x12500),oracle::a_byte_dx_result);
        i32 x=0,y=0;ASSERT_TRUE(manager.current_position(dc,x,y));
        EXPECT_EQ(x,oracle::a_byte_dx_x);EXPECT_EQ(y,oracle::a_byte_dx_y);
    }
}

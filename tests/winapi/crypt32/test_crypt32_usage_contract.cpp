// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_crypt32_usage_contract.cpp
/// @brief CertGetIntendedKeyUsage replay of Windows at both pointer widths

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/crypt32/crypt32.hpp>
#include <winapi/system/crypt32/crypt32_internal.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <core/memory/guest_memory.hpp>

#include <cstring>

using namespace vitreo;
using namespace vitreo::winapi;

namespace usage_oracle {
#include "../../oracle_data/cert_usage_inputs.h"
#include "../../../src/winapi/system/crypt32/cert_usage_abi.inc"
struct Case { u32 input, encoding, capacity, result, error; u8 bytes[5]; };
const Case cases[] = {
#include "cert_usage_cases.inc"
};
}

TEST(Crypt32UsageContract, MatchesWindowsBothBitnesses) {
    using namespace usage_oracle;
    for (bool is64 : {false, true}) {
        SCOPED_TRACE(is64);
        GuestMemory memory;
        const GuestAddr base = is64 ? 0x200010000ULL : 0x10000;
        ASSERT_TRUE(memory.map_region(base,0x3000,MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (is64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.set_sp(base+0x2800);
        Marshaler m(memory); m.attach_cpu(cpu);
        const auto& layout = is64 ? usage_layout_64 : usage_layout_32;
        for (const auto& c : cases) {
            SCOPED_TRACE(c.input);
            SCOPED_TRACE(c.encoding);
            SCOPED_TRACE(c.capacity);
            const GuestAddr info=base, extensions=base+0x300, output=base+0x100;
            ASSERT_TRUE(m.write_u32(info+layout.count,usage_case_size(c.input)).is_ok());
            ASSERT_TRUE(m.write_ptr(info+layout.extensions,extensions).is_ok());
            for (unsigned slot=0;slot<usage_case_size(c.input);++slot) {
                const unsigned input=usage_case_input(c.input,slot);
                const GuestAddr ext=extensions+slot*layout.stride, oid=base+0x500+slot*0x100, data=base+0x800+slot*0x100;
                const char* name=usage_legacy[input]?key_attributes_oid:key_usage_oid;
                ASSERT_TRUE(memory.write_bytes(oid,reinterpret_cast<const u8*>(name),std::strlen(name)+1).is_ok());
                ASSERT_TRUE(memory.write_bytes(data,usage_inputs[input],usage_lengths[input]).is_ok());
                ASSERT_TRUE(m.write_ptr(ext,oid).is_ok());
                ASSERT_TRUE(m.write_u32(ext+layout.blob,usage_lengths[input]).is_ok());
                ASSERT_TRUE(m.write_ptr(ext+layout.blob+layout.data,data).is_ok());
            }
            for (unsigned i=0;i<5;++i) ASSERT_TRUE(m.write_u8(output+i,0xcc).is_ok());
            const GuestAddr args[]={c.encoding,info,output,c.capacity};
            if(is64){cpu.gpr.rcx=args[0];cpu.gpr.rdx=args[1];cpu.gpr.r8=args[2];cpu.gpr.r9=args[3];}
            else for(unsigned i=0;i<4;++i)ASSERT_TRUE(m.write_u32(cpu.sp()+4+i*4,static_cast<u32>(args[i])).is_ok());
            set_last_error(0xdead);
            crypt32::impl_CertGetIntendedKeyUsage(m);
            EXPECT_EQ(cpu.gpr.rax,c.result);
            EXPECT_EQ(get_last_error(),c.error);
            for (unsigned i=0;i<5;++i) EXPECT_EQ(memory.read_u8(output+i).value(),c.bytes[i]);
        }
    }
}

TEST(Crypt32UsageContract, NullArgumentsMatchWindowsFaults) {
    for(bool is64 : {false,true}) {
        GuestMemory memory;
        const GuestAddr base=is64?0x200010000ULL:0x10000;
        ASSERT_TRUE(memory.map_region(base,0x2000,MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if(is64)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.set_sp(base+0x1800);
        Marshaler m(memory);m.attach_cpu(cpu);
        for(unsigned mode=0;mode<5;++mode){
            SCOPED_TRACE(mode);
            cpu.pending_raise = {};
            const GuestAddr args[]={1,(mode==0||mode==2||mode==4)?0:base,(mode==1||mode==2||mode==3)?0:base+0x100,mode>=2?0u:1u};
            ASSERT_TRUE(m.write_u8(base+0x100,0xcc).is_ok());
            if(is64){cpu.gpr.rcx=args[0];cpu.gpr.rdx=args[1];cpu.gpr.r8=args[2];cpu.gpr.r9=args[3];}
            else for(unsigned i=0;i<4;++i)ASSERT_TRUE(m.write_u32(cpu.sp()+4+4*i,static_cast<u32>(args[i])).is_ok());
            crypt32::impl_CertGetIntendedKeyUsage(m);
            if(mode==3){EXPECT_FALSE(cpu.pending_raise.active);EXPECT_EQ(cpu.gpr.rax,0u);EXPECT_EQ(get_last_error(),0u);}
            else {EXPECT_TRUE(cpu.pending_raise.active);EXPECT_EQ(cpu.pending_raise.code,usage_oracle::access_violation_status);}
            EXPECT_EQ(memory.read_u8(base+0x100).value(),0xcc);
        }
    }
}

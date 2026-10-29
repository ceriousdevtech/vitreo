/// @file test_sync_access_oracle.cpp
/// @brief Event/mutex access-mask and security-descriptor tests replayed from captured Windows output (THREAD-002)

#include "sync_test_fixture.hpp"

#include "sync_access_oracle.inc"
TEST_F(SyncTest, EventExAccessMatchesWindowsOracleBothAbis) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x80000, 0x1000, MemoryProtection::ReadWrite).is_ok());
    auto exports = create_sync_thunks(*m_mgr);
    auto create = std::find_if(exports.entries.begin(), exports.entries.end(),
        [](const auto& e) { return e.name == "CreateEventExW"; });
    ASSERT_NE(create, exports.entries.end());
    for (bool x64 : {false,true}) {
        cpu::CpuState cpu;
        if (x64) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp = 0x80100;
        Marshaler m(memory); m.attach_cpu(cpu);
        auto call = [&](u32 flags, u32 access) {
            if (x64) {
                cpu.gpr.rcx=0; cpu.gpr.rdx=0; cpu.gpr.r8=flags; cpu.gpr.r9=access;
            } else {
                (void)memory.write_u32(0x80104,0); (void)memory.write_u32(0x80108,0);
                (void)memory.write_u32(0x8010c,flags); (void)memory.write_u32(0x80110,access);
            }
            create->handler(m); return static_cast<Handle>(cpu.gpr.rax);
        };
        for (const auto& row : sync_event_rows) {
            SCOPED_TRACE(row.access);
            const Handle h=call(3,row.access); ASSERT_NE(h,NULL_HANDLE);
            set_last_error(0x1234);
            EXPECT_EQ(m_mgr->set_event(h),row.op != 0); EXPECT_EQ(get_last_error(),row.op_error);
            set_last_error(0x1234);
            EXPECT_EQ(m_mgr->wait_for_single_object(h,0),row.wait); EXPECT_EQ(get_last_error(),row.wait_error);
            set_last_error(0x1234);
            EXPECT_EQ(m_mgr->wait_for_multiple_objects(1,&h,false,0),row.wait); EXPECT_EQ(get_last_error(),row.wait_error);
            set_last_error(0x1234);
            EXPECT_EQ(m_mgr->reset_event(h),row.op != 0); EXPECT_EQ(get_last_error(),row.op_error);
            (void)m_handles->close(h);
        }
        for (u32 flags=4;flags<8;++flags) {
            EXPECT_EQ(call(flags,0x1f0003),NULL_HANDLE); EXPECT_EQ(get_last_error(),87u);
        }
    }
}

#include <winapi/common/object_access.hpp>
TEST(ObjectAccess, OrderedDaclMatchesSynchronizationWindowsOracle) {
    using namespace object_access;
    using namespace sync_access;
    const std::vector<u8> user{1,1,0,0,0,0,0,5,21,0,0,0};
    const std::vector<u8> everyone{1,1,0,0,0,0,0,1,0,0,0,0};
    Token token; token.user=user;token.groups.push_back({everyone,SE_GROUP_ENABLED});
    auto acl=[&](std::initializer_list<std::pair<bool,u32>> entries){
        std::vector<u8> result(8,0);result[0]=2;result[4]=static_cast<u8>(entries.size());
        for(auto [deny,mask]:entries){const usize off=result.size();result.resize(off+20);result[off]=deny?1:0;result[off+2]=20;std::memcpy(result.data()+off+4,&mask,4);std::copy(everyone.begin(),everyone.end(),result.begin()+static_cast<isize>(off+8));}
        result[2]=static_cast<u8>(result.size());return result;
    };
    // Windows sync_security_probe cases: empty, NULL, allow-all, execute-only,
    // deny-before-allow, and allow-before-deny. Owner READ_CONTROL/WRITE_DAC
    // is independent of these DACL entries.
    std::optional<std::vector<u8>> cases[]={acl({}),std::nullopt,acl({{false,GENERIC_ALL}}),acl({{false,SYNCHRONIZE}}),acl({{true,2},{false,GENERIC_ALL}}),acl({{false,GENERIC_ALL},{true,2}})};
    const bool modify[]={false,true,true,false,false,true};
    for(usize i=0;i<6;++i){
        Descriptor sd{user,cases[i],false};u32 granted=0;
        EXPECT_EQ(evaluate(sd,token,2,EVENT_ALL_ACCESS,READ_CONTROL|1,READ_CONTROL|2,READ_CONTROL|SYNCHRONIZE,granted),modify[i]?object_access::Result::Granted:object_access::Result::Denied);
        EXPECT_EQ(evaluate(sd,token,READ_CONTROL|WRITE_DAC,EVENT_ALL_ACCESS,READ_CONTROL|1,READ_CONTROL|2,READ_CONTROL|SYNCHRONIZE,granted),object_access::Result::Granted);
        EXPECT_EQ(granted,READ_CONTROL|WRITE_DAC);
    }
}

#include "sync_security_oracle.inc"
TEST_F(SyncTest, EventAndMutexExSecurityMatchesWindowsOracleBothAbis) {
    using namespace sync_access;
    const std::vector<u8> user{1,1,0,0,0,0,0,5,21,0,0,0};
    const std::vector<u8> everyone{1,1,0,0,0,0,0,1,0,0,0,0};
    m_mgr->set_object_token_provider([=]{object_access::Token t;t.user=user;t.groups.push_back({everyone,SE_GROUP_ENABLED});return t;});
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
    auto exports=create_sync_thunks(*m_mgr);
    for(bool x64:{false,true}) for(bool wide:{false,true}) for(u32 kind=0;kind<2;++kind)for(u32 scenario=0;scenario<6;++scenario){
        SCOPED_TRACE(std::to_string(x64)+"/"+std::to_string(wide)+"/"+std::to_string(kind)+"/"+std::to_string(scenario));
        const std::string api=std::string(kind?"CreateMutexEx":"CreateEventEx")+(wide?"W":"A");
        auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),[&](const auto& e){return e.name==api;});
        ASSERT_NE(entry,exports.entries.end());EXPECT_EQ(entry->stdcall_bytes,16u);
        std::array<Byte,0x500> zero{};ASSERT_TRUE(memory.write_bytes(0x10000,zero.data(),zero.size()).is_ok());
        (void)memory.write_u8(0x10000,1);(void)memory.write_u16(0x10002,SE_DACL_PRESENT);
        auto ptr=[&](GuestAddr at,GuestAddr value){if(x64)(void)memory.write_u64(at,value);else(void)memory.write_u32(at,static_cast<u32>(value));};
        ptr(0x10100+(x64?8:4),0x10000);(void)memory.write_u32(0x10100+(x64?16:8),1);
        std::vector<u8> acl(8,0);acl[0]=2;
        auto ace=[&](bool deny,u32 mask){const usize off=acl.size();acl.resize(off+20);acl[off]=deny?1:0;acl[off+2]=20;std::memcpy(acl.data()+off+4,&mask,4);std::memcpy(acl.data()+off+8,everyone.data(),everyone.size());++acl[4];};
        if(scenario==2)ace(false,GENERIC_ALL);
        if(scenario==3)ace(false,SYNCHRONIZE);
        if(scenario==4){ace(true,2);ace(false,GENERIC_ALL);}
        if(scenario==5){ace(false,GENERIC_ALL);ace(true,2);}
        acl[2]=static_cast<u8>(acl.size());
        if(scenario!=1){ptr(0x10000+(x64?32:16),0x10200);ASSERT_TRUE(memory.write_bytes(0x10200,reinterpret_cast<const Byte*>(acl.data()),acl.size()).is_ok());}
        const std::string name="sync-sec-"+std::to_string(x64)+std::to_string(wide)+std::to_string(kind)+std::to_string(scenario);
        for(usize i=0;i<name.size();++i){if(wide)(void)memory.write_u16(0x10400+2*i,static_cast<u8>(name[i]));else(void)memory.write_u8(0x10400+i,static_cast<u8>(name[i]));}
        cpu::CpuState cpu;if(x64)cpu.init_long_mode();else cpu.init_protected_mode();cpu.gpr.rsp=0x18000;
        Marshaler m(memory);m.attach_cpu(cpu);
        auto call=[&](GuestAddr attributes,u32 access){
            if(x64){cpu.gpr.rcx=attributes;cpu.gpr.rdx=0x10400;cpu.gpr.r8=0;cpu.gpr.r9=access;}
            else{(void)memory.write_u32(0x18004,static_cast<u32>(attributes));(void)memory.write_u32(0x18008,0x10400);(void)memory.write_u32(0x1800c,0);(void)memory.write_u32(0x18010,access);}
            entry->handler(m);return static_cast<Handle>(cpu.gpr.rax);
        };
        const Handle initial=call(0x10100,kind?MUTEX_ALL_ACCESS:EVENT_ALL_ACCESS);ASSERT_NE(initial,NULL_HANDLE);EXPECT_EQ(get_last_error(),0u);
        EXPECT_EQ(m_handles->get_flags(initial)&HandleFlags::Inherit,HandleFlags::Inherit);
        // The descriptor belongs to the object: existing-name constructors ignore a new malformed SD.
        (void)memory.write_u8(0x10000,0);
        for(const auto& row:sync_security_rows)if(row.kind==kind && row.scenario==scenario){
            set_last_error(0x1234);const Handle h=call(0x10100,row.access);
            EXPECT_EQ(h!=NULL_HANDLE,row.success!=0);EXPECT_EQ(get_last_error(),row.error);
            if(h)(void)m_handles->close(h);
        }
        (void)m_handles->close(initial);
    }
}

TEST_F(SyncTest, MutexExAccessMatchesWindowsOracleBothAbis) {
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x80000,0x1000,MemoryProtection::ReadWrite).is_ok());
    auto exports=create_sync_thunks(*m_mgr);
    for(const char* api:{"CreateMutexExA","CreateMutexExW"})for(bool x64:{false,true}){
        auto entry=std::find_if(exports.entries.begin(),exports.entries.end(),[&](const auto& e){return e.name==api;});ASSERT_NE(entry,exports.entries.end());
        cpu::CpuState cpu;if(x64)cpu.init_long_mode();else cpu.init_protected_mode();cpu.gpr.rsp=0x80100;
        Marshaler m(memory);m.attach_cpu(cpu);
        auto call=[&](u32 flags,u32 access){
            if(x64){cpu.gpr.rcx=0;cpu.gpr.rdx=0;cpu.gpr.r8=flags;cpu.gpr.r9=access;}
            else{(void)memory.write_u32(0x80104,0);(void)memory.write_u32(0x80108,0);(void)memory.write_u32(0x8010c,flags);(void)memory.write_u32(0x80110,access);}
            entry->handler(m);return static_cast<Handle>(cpu.gpr.rax);
        };
        for(const auto& row:sync_event_rows){
            const Handle h=call(1,row.access);ASSERT_NE(h,NULL_HANDLE);
            set_last_error(0x1234);EXPECT_TRUE(m_mgr->release_mutex(h));EXPECT_EQ(get_last_error(),0x1234u);
            set_last_error(0x1234);EXPECT_EQ(m_mgr->wait_for_single_object(h,0),row.wait);EXPECT_EQ(get_last_error(),row.wait_error);
            (void)m_handles->close(h);
        }
        for(u32 flags=2;flags<8;++flags){EXPECT_EQ(call(flags,sync_access::MUTEX_ALL_ACCESS),NULL_HANDLE);EXPECT_EQ(get_last_error(),87u);}
    }
}

// CreateMutexA with a hand-built DACL (CPU-Z's _CPUIDSDK_ mutex sizes its
// Everyone ACE as GetLengthSid + 12, four bytes past the SID). Every row is
// tools/oracle_probe/sync_dacl_ace_layout_probe.expected (x86 == x64): an ACE
// may extend past its SID and the extra bytes are ignored; an AceSize that is
// not a multiple of 4 or that cuts the SID off is ERROR_INVALID_ACL; ACL
// revisions 2..4 are accepted, as is AclSize slack after the last ACE.
TEST_F(SyncTest, MutexDaclAceLayoutMatchesWindowsOracleBothAbis) {
    using namespace sync_access;
    const std::vector<u8> user{1,1,0,0,0,0,0,5,21,0,0,0};
    const std::vector<u8> everyone{1,1,0,0,0,0,0,1,0,0,0,0};
    m_mgr->set_object_token_provider([=]{object_access::Token t;t.user=user;t.groups.push_back({everyone,SE_GROUP_ENABLED});return t;});
    struct Ace {u8 type;u16 size;u32 mask;};
    struct Case {const char* name;u8 revision;std::vector<Ace> aces;u8 fill;u16 acl_slack;u32 create_error;std::array<u32,4> open_errors;};
    constexpr u32 ok=0x1234;  // a successful open leaves the last error alone
    const std::array<u32,4> all_open{ok,ok,ok,ok};
    const Case cases[]={
        {"exact",2,{{0,20,MUTEX_ALL_ACCESS}},0,0,0,all_open},
        {"slack4",2,{{0,24,MUTEX_ALL_ACCESS}},0,0,0,all_open},
        {"slack4_fill",2,{{0,24,MUTEX_ALL_ACCESS}},0xA5,0,0,all_open},
        {"slack8",2,{{0,28,MUTEX_ALL_ACCESS}},0,0,0,all_open},
        {"deny_slack4",2,{{1,24,1},{0,20,MUTEX_ALL_ACCESS}},0xA5,0,0,{ok,5,5,5}},
        {"odd22",2,{{0,22,MUTEX_ALL_ACCESS}},0,2,1336,{}},
        {"odd21",2,{{0,21,MUTEX_ALL_ACCESS}},0,3,1336,{}},
        {"cut16",2,{{0,16,MUTEX_ALL_ACCESS}},0,0,1336,{}},
        {"cut18",2,{{0,18,MUTEX_ALL_ACCESS}},0,2,1336,{}},
        {"rev3",3,{{0,20,MUTEX_ALL_ACCESS}},0,0,0,all_open},
        {"rev4",4,{{0,20,MUTEX_ALL_ACCESS}},0,0,0,all_open},
        {"acl_slack8",2,{{0,20,MUTEX_ALL_ACCESS}},0,8,0,all_open},
        {"acl_odd",2,{{0,20,MUTEX_ALL_ACCESS}},0,2,0,all_open},
    };
    const u32 opens[4]={SYNCHRONIZE,1u,MUTEX_ALL_ACCESS,0x120001u};
    GuestMemory memory;
    ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
    auto exports=create_sync_thunks(*m_mgr);
    auto find=[&](const char* api){return std::find_if(exports.entries.begin(),exports.entries.end(),[&](const auto& e){return e.name==api;});};
    const auto create=find("CreateMutexA");const auto open=find("OpenMutexA");
    ASSERT_NE(create,exports.entries.end());ASSERT_NE(open,exports.entries.end());
    for(bool x64:{false,true})for(const Case& c:cases){
        SCOPED_TRACE(std::string(x64?"x64 ":"x86 ")+c.name);
        std::array<Byte,0x500> zero{};ASSERT_TRUE(memory.write_bytes(0x10000,zero.data(),zero.size()).is_ok());
        std::vector<u8> acl(8,0);
        for(const Ace& a:c.aces){
            const usize at=acl.size();acl.resize(at+a.size,c.fill);
            acl[at]=a.type;acl[at+1]=0;acl[at+2]=static_cast<u8>(a.size);acl[at+3]=0;
            std::memcpy(acl.data()+at+4,&a.mask,4);
            std::memcpy(acl.data()+at+8,everyone.data(),std::min<usize>(a.size-8u,everyone.size()));
        }
        acl[0]=c.revision;acl[2]=static_cast<u8>(acl.size()+c.acl_slack);acl[4]=static_cast<u8>(c.aces.size());
        acl.resize(acl.size()+c.acl_slack,0);
        ASSERT_TRUE(memory.write_bytes(0x10200,reinterpret_cast<const Byte*>(acl.data()),acl.size()).is_ok());
        auto ptr=[&](GuestAddr at,GuestAddr value){if(x64)(void)memory.write_u64(at,value);else(void)memory.write_u32(at,static_cast<u32>(value));};
        (void)memory.write_u8(0x10000,1);(void)memory.write_u16(0x10002,SE_DACL_PRESENT);ptr(0x10000+(x64?32:16),0x10200);
        (void)memory.write_u32(0x10100,x64?24:12);ptr(0x10100+(x64?8:4),0x10000);
        const std::string name=std::string("dacl-layout-")+(x64?"64-":"32-")+c.name;
        ASSERT_TRUE(memory.write_bytes(0x10400,reinterpret_cast<const Byte*>(name.c_str()),name.size()+1).is_ok());
        cpu::CpuState cpu;if(x64)cpu.init_long_mode();else cpu.init_protected_mode();cpu.gpr.rsp=0x18000;
        Marshaler m(memory);m.attach_cpu(cpu);
        auto call=[&](const auto& entry,u64 a0,u64 a1){
            if(x64){cpu.gpr.rcx=a0;cpu.gpr.rdx=a1;cpu.gpr.r8=0x10400;}
            else{(void)memory.write_u32(0x18004,static_cast<u32>(a0));(void)memory.write_u32(0x18008,static_cast<u32>(a1));(void)memory.write_u32(0x1800c,0x10400);}
            set_last_error(0x1234);entry->handler(m);return static_cast<Handle>(cpu.gpr.rax);
        };
        const Handle h=call(create,0x10100,0);
        EXPECT_EQ(h!=NULL_HANDLE,c.create_error==0);EXPECT_EQ(get_last_error(),c.create_error);
        if(!h)continue;
        for(usize i=0;i<4;++i){
            SCOPED_TRACE(opens[i]);
            const Handle r=call(open,opens[i],0);
            EXPECT_EQ(r!=NULL_HANDLE,c.open_errors[i]==ok);EXPECT_EQ(get_last_error(),c.open_errors[i]);
            if(r)(void)m_handles->close(r);
        }
        (void)m_handles->close(h);
    }
}

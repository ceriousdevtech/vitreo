// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_engine.cpp
/// @brief Windows Installer condition, formatted-string and directory tests.
///
/// These three pieces decide what an install actually does, and each fails
/// quietly rather than loudly when it is subtly wrong: a mis-parsed condition
/// runs or skips an action with no error, an unexpanded formatted string
/// writes a literal `[ProductCode]` into the registry, and a bad directory
/// join puts 130 MB of payload in the wrong place. So they are pinned here
/// rather than left to be noticed during an install.

#include <winapi/system/msi/msi_register.hpp>
#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/system/msi/msi_engine.hpp>
#include <winapi/system/msi/msi_install.hpp>
#include <winapi/system/msi/msi.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/kernel32/error.hpp>
#include <core/cpu/state/cpu_state.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <capsule/registry/registry.hpp>
#include <sstream>

using namespace vitreo;
using namespace vitreo::winapi::msi;

TEST(MsiRecordRead, MatchesWindowsOracleBothGuestModes) {
    using namespace vitreo::winapi;
    for (bool wide : {false,true}) {
        SCOPED_TRACE(wide);
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;
        if (wide) cpu.init_long_mode(); else cpu.init_protected_mode();
        cpu.gpr.rsp=0x1f000;
        Marshaler m(memory);m.attach_cpu(cpu);
        MsiManager manager;
        auto exports=create_msi_thunks(manager);
        auto call=[&](const char* name,u32 a=0,u32 b=0,u32 c=0) {
            cpu.gpr.rcx=a;cpu.gpr.rdx=b;cpu.gpr.r8=c;
            (void)memory.write_u32(cpu.gpr.rsp+4,a);
            (void)memory.write_u32(cpu.gpr.rsp+8,b);
            (void)memory.write_u32(cpu.gpr.rsp+12,c);
            for (auto& entry:exports.entries) if(entry.name==name) {
                entry.handler(m);return static_cast<u32>(cpu.gpr.rax);
            }
            ADD_FAILURE()<<"missing "<<name;return 0u;
        };
        std::map<std::string,std::pair<u32,u32>> expected;
        const auto source=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
        std::ifstream oracle(source / "tests/oracle_data" /
            (wide ? "msi_record_read64.expected" : "msi_record_read32.expected"));
        ASSERT_TRUE(oracle.is_open());
        std::string label;u32 value=0,error=0;
        while(oracle>>label>>std::hex>>value>>error) expected[label]={value,error};
        ASSERT_EQ(expected.size(),31u);
        auto check=[&](const std::string& name,const char* function,u32 a,u32 b=0) {
            set_last_error(0xdead);
            EXPECT_EQ(call(function,a,b),expected.at(name).first)<<name;
            EXPECT_EQ(get_last_error(),expected.at(name).second)<<name;
        };
        const u32 record=call("MsiCreateRecord",2);
        ASSERT_NE(record,0u);
        check("invalid-count","MsiRecordGetFieldCount",0);
        check("count","MsiRecordGetFieldCount",record);
        check("invalid-integer","MsiRecordGetInteger",0,1);
        check("outside-integer","MsiRecordGetInteger",record,3);
        const char* cases[]={nullptr,"","0","1","-1","+1"," 1","1 ","\t1","01","0x10","1a","-","+","2147483647","2147483648","-2147483648","-2147483649","4294967295","4294967296","9999999999999999999999999999999","1.0","--1","1\n","\xef\xbc\x91"};
        for(unsigned i=0;i<std::size(cases);++i) {
            if(cases[i]) ASSERT_TRUE(m.write_wide_string(0x10200,cases[i]).is_ok());
            ASSERT_EQ(call("MsiRecordSetStringW",record,0,cases[i]?0x10200:0),0u);
            check("case-"+std::to_string(i),"MsiRecordGetInteger",record,0);
        }
        EXPECT_EQ(call("MsiRecordSetInteger",record,1,123),0u);
        check("typed-integer","MsiRecordGetInteger",record,1);
        EXPECT_EQ(call("MsiRecordSetInteger",record,1,0x80000000u),0u);
        check("typed-null","MsiRecordGetInteger",record,1);
        EXPECT_EQ(call("MsiRecordSetInteger",record,3,1),87u);
        EXPECT_EQ(call("MsiRecordSetInteger",0,0,1),6u);
        EXPECT_EQ(call("MsiCloseHandle",record),0u);
        check("invalid-count","MsiRecordGetFieldCount",record);
    }
}

TEST(MsiProducts, MatchesWindowsContextAndAdvertisementMatrixBothGuestModes) {
    using namespace vitreo::winapi;
    constexpr const char* product="{A61B2C3D-4E5F-4678-9ABC-DEF012345678}";
    const std::wstring packed=L"D3C2B16AF5E48764A9CBED0F21436587";
    const std::wstring sid=L"S-1-5-21-17-18-19-1001";
    const std::wstring userdata=L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\";
    const std::wstring paths[]={
        L"SOFTWARE\\Microsoft\\Installer\\Products\\"+packed,
        L"SOFTWARE\\Classes\\Installer\\Products\\"+packed,
        userdata+sid+L"\\Products\\"+packed,
        userdata+L"S-1-5-18\\Products\\"+packed,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\Managed\\"+sid+L"\\Installer\\Products\\"+packed,
        userdata+L"S-1-5-19\\Products\\"+packed};
    const auto source=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
    for(bool wide:{false,true}) {
        std::ifstream oracle(source/"tests/oracle_data"/(wide?"msi_product_state64.expected":"msi_product_state32.expected"));
        ASSERT_TRUE(oracle.is_open());
        std::string line;
        while(std::getline(oracle,line)) {
            std::istringstream row(line);std::string name;i32 state=0;u32 last_error=0,count=0;
            if(!(row>>name>>state>>last_error>>count)) continue;
            SCOPED_TRACE(name);
            SCOPED_TRACE(wide);
            capsule::Registry registry;
            MsiManager manager;manager.set_registry(&registry);manager.set_user_sid(sid);
            auto add=[&](unsigned index) {
                auto key=registry.open_key(index==0?capsule::HKEY_CURRENT_USER:capsule::HKEY_LOCAL_MACHINE,paths[index],true);
                EXPECT_TRUE(key.is_ok());if(key.is_ok())registry.close_key(key.value());
            };
            auto properties=[&](unsigned index,bool marker,u32 marker_value=1) {
                auto key=registry.open_key(capsule::HKEY_LOCAL_MACHINE,paths[index]+L"\\InstallProperties",true);
                ASSERT_TRUE(key.is_ok());
                if(marker) ASSERT_TRUE(registry.set_value(key.value(),L"WindowsInstaller",
                    capsule::RegValue{capsule::RegType::Dword,marker_value}).is_ok());
                registry.close_key(key.value());
            };
            unsigned a=0,b=0;
            if(std::sscanf(name.c_str(),"marker-type-%u",&a)==1) {
                add(0);properties(2,false);
                auto key=registry.open_key(capsule::HKEY_LOCAL_MACHINE,paths[2]+L"\\InstallProperties",false);
                ASSERT_TRUE(key.is_ok());
                capsule::RegValue marker{capsule::RegType::Binary,std::vector<u8>(a==3?1:8,1)};
                if(a==0) marker={capsule::RegType::Dword,u32{1}};
                if(a==1 || a==2) marker={capsule::RegType::Sz,std::wstring(a==1?1:3,L'x')};
                if(a==5) marker={capsule::RegType::Qword,u64{1}};
                ASSERT_TRUE(registry.set_value(key.value(),L"WindowsInstaller",marker).is_ok());
                registry.close_key(key.value());
            }
            else if(std::sscanf(name.c_str(),"key-%u",&a)==1) add(a);
            else if(std::sscanf(name.c_str(),"properties-%u",&a)==1 || std::sscanf(name.c_str(),"localpackage-%u",&a)==1) properties(a,false);
            else if(std::sscanf(name.c_str(),"marker-%u",&a)==1 || std::sscanf(name.c_str(),"installed-%u",&a)==1) properties(a,true);
            else if(std::sscanf(name.c_str(),"registered-installed-%u",&a)==1) {properties(a,true);add(a==2?0:1);}
            else if(name=="all-advertised") {add(0);add(1);add(4);}
            else if(std::sscanf(name.c_str(),"cross-%u-%u",&a,&b)==2) {add(a);properties(b,true);}
            else if(std::sscanf(name.c_str(),"zero-%u-%u",&a,&b)==2) {add(a);properties(b,true,0);}
            else if(std::sscanf(name.c_str(),"two-%u-%u",&a,&b)==2) {add(a);properties(b,true,2);}
            else if(std::sscanf(name.c_str(),"matrix-%u-%u",&a,&b)==2) {
                const unsigned ids[]={0,1,4};
                for(unsigned i=0;i<3;++i)if(a&(1u<<i))add(ids[i]);
                for(unsigned i=0;i<2;++i)if(b&(1u<<i))properties(i+2,true);
            }
            GuestMemory memory;
            ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
            cpu::CpuState cpu;if(wide)cpu.init_long_mode();else cpu.init_protected_mode();cpu.gpr.rsp=0x1f000;
            Marshaler m(memory);m.attach_cpu(cpu);auto exports=create_msi_thunks(manager);
            auto call=[&](const char* fn,u32 x,u32 y=0) {
                cpu.gpr.rcx=x;cpu.gpr.rdx=y;(void)memory.write_u32(cpu.gpr.rsp+4,x);(void)memory.write_u32(cpu.gpr.rsp+8,y);
                for(auto& entry:exports.entries)if(entry.name==fn){entry.handler(m);return static_cast<u32>(cpu.gpr.rax);}
                ADD_FAILURE()<<fn;return 0u;
            };
            ASSERT_TRUE(m.write_wide_string(0x10100,product).is_ok());
            EXPECT_EQ(static_cast<i32>(call("MsiQueryProductStateW",0x10100)),state);
            EXPECT_EQ(call("MsiEnumProductsW",0,0),87u);
            for(const char* fn:{"MsiEnumProductsA","MsiEnumProductsW"}) {
                ASSERT_TRUE(memory.write_u32(0x10500,0xa5a5a5a5).is_ok());
                const u32 result=call(fn,0,0x10500);
                EXPECT_EQ(result,count?0u:259u);
                if(count) {
                    EXPECT_EQ((fn[15]=='A'?m.read_ansi_string(0x10500):m.read_wide_string(0x10500)).value(),product);
                    EXPECT_EQ(call(fn,1,0x10500),259u);
                } else EXPECT_EQ(memory.read_u32(0x10500).value(),0xa5a5a5a5u);
            }
        }
    }
}

TEST(MsiProducts, ExEnumerationDoesNotRelabelUserProductsAsMachine) {
    using namespace vitreo::winapi;
    for(bool wide:{false,true}) {
        capsule::Registry registry;
        auto key=registry.open_key(capsule::HKEY_CURRENT_USER,
            L"SOFTWARE\\Microsoft\\Installer\\Products\\D3C2B16AF5E48764A9CBED0F21436587",true);
        ASSERT_TRUE(key.is_ok());registry.close_key(key.value());
        MsiManager manager;manager.set_registry(&registry);manager.set_user_sid(L"S-1-5-21-17-18-19-1001");
        auto handlers=msi_handlers(manager);
        GuestMemory memory;
        ASSERT_TRUE(memory.map_region(0x10000,0x10000,MemoryProtection::ReadWrite).is_ok());
        cpu::CpuState cpu;if(wide)cpu.init_long_mode();else cpu.init_protected_mode();
        cpu.gpr.rsp=0x1f000;Marshaler m(memory);m.attach_cpu(cpu);
        auto call=[&](u32 context) {
            const u32 args[]={0,0,context,0,0x10200,0x10300,0,0};
            cpu.gpr.rcx=args[0];cpu.gpr.rdx=args[1];cpu.gpr.r8=args[2];cpu.gpr.r9=args[3];
            for(unsigned i=0;i<8;++i) {
                if(wide && i>=4)(void)memory.write_u64(cpu.gpr.rsp+40+(i-4)*8,args[i]);
                if(!wide)(void)memory.write_u32(cpu.gpr.rsp+4+i*4,args[i]);
            }
            handlers.at("MsiEnumProductsExW")(m);return static_cast<u32>(cpu.gpr.rax);
        };
        (void)memory.write_u32(0x10200,0xdeadbeef);
        EXPECT_EQ(call(4),259u);   // a per-user product is not a machine one
        EXPECT_EQ(memory.read_u32(0x10200).value(),0xdeadbeefu);
        EXPECT_EQ(call(2),0u);     // ... it is the caller's unmanaged one
        EXPECT_EQ(memory.read_u32(0x10300).value(),2u);
        EXPECT_EQ(call(0),87u);
        auto bad=registry.open_key(capsule::HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Classes\\Installer\\Products\\VITREO_CONTRACT_BAD_A61B2C3D",true);
        ASSERT_TRUE(bad.is_ok());registry.close_key(bad.value());
        EXPECT_EQ(call(4),1610u); // msi_product_state{32,64}.expected enum-malformed
    }
}

TEST(MsiProducts, RegistrationPersistsCanonicalContextForAnotherManager) {
    using namespace vitreo::winapi;
    // The product database is the registry: an instance registered through
    // one manager is the same product for another (the elevated installer
    // and its parent), in the context it was registered in.
    for(const bool machine:{false,true}) {
        capsule::Registry registry;
        const std::string code="{A61B2C3D-4E5F-4678-9ABC-DEF012345678}";
        msi::ProductDatabase db(registry,"S-1-5-21-17-18-19-1001");
        const msi::ProductInstance inst{code,machine?msi::kContextMachine:msi::kContextUserUnmanaged,
                                        machine?std::string{}:std::string("S-1-5-21-17-18-19-1001")};
        msi::ProductDatabase::Publication pub;pub.product_name="Oracle registration fixture";
        db.publish_product(inst,pub);
        db.register_product(inst,{{"DisplayName","Oracle registration fixture"}},{{"WindowsInstaller",1}},{},false);
        MsiManager observer;observer.set_registry(&registry);
        observer.set_user_sid(L"S-1-5-21-17-18-19-1001");
        EXPECT_TRUE(observer.is_product_registered(code));
        EXPECT_EQ(observer.registered_products(),std::vector<std::string>{code});
        EXPECT_EQ(observer.product_property(code,"DisplayName"),"Oracle registration fixture");
        const std::wstring path=std::wstring(machine?L"SOFTWARE\\Classes\\Installer\\Products\\":L"SOFTWARE\\Microsoft\\Installer\\Products\\")+L"D3C2B16AF5E48764A9CBED0F21436587";
        auto key=registry.open_key(machine?capsule::HKEY_LOCAL_MACHINE:capsule::HKEY_CURRENT_USER,path,false);
        ASSERT_TRUE(key.is_ok());registry.close_key(key.value());
    }
}

namespace {

/// A session with no package behind it — enough to exercise properties,
/// formatting and conditions, which never touch the database.
Session make_session() {
    Session s;
    s.set_property("ProductCode", "{5DB65884-C963-4454-AABA-4CA3089281FA}");
    s.set_property("ProductVersion", "9.09.0720");
    s.set_property("ALLUSERS", "1");
    s.set_property("VersionNT", "603");
    s.set_property("Privileged", "1");
    s.set_property("SKIP_PHYSX_DRIVER_SET", "0");
    return s;
}

bool cond(const Session& s, const char* expression) {
    auto result = s.evaluate_condition(expression);
    EXPECT_TRUE(result.has_value()) << "failed to parse: " << expression;
    return result.value_or(false);
}

} // namespace

// =============================================================================
// Conditions
// =============================================================================

TEST(MsiCondition, EmptyConditionMeansAlwaysRun) {
    const Session s = make_session();
    EXPECT_TRUE(cond(s, ""));
    EXPECT_TRUE(cond(s, "   "));
}

TEST(MsiCondition, BarePropertyIsTrueWhenSet) {
    const Session s = make_session();
    EXPECT_TRUE(cond(s, "ALLUSERS"));
    EXPECT_TRUE(cond(s, "Privileged"));

    // An unset property is false. A property whose value is "0" is still a
    // non-empty string, and Windows evaluates it true (oracle
    // msi_engine*.expected: "cond [B] -> 1" with B="0"); only the integer
    // literal 0 is false.
    EXPECT_FALSE(cond(s, "Installed"));
    EXPECT_TRUE(cond(s, "SKIP_PHYSX_DRIVER_SET"));
    EXPECT_FALSE(cond(s, "0"));
    EXPECT_TRUE(cond(s, "NOT Installed"));
}

TEST(MsiCondition, ComparesNumbersNumericallyAndTextTextually) {
    Session s = make_session();

    // VersionNT gates several sequenced actions in real packages.
    EXPECT_TRUE(cond(s, "VersionNT >= 501"));
    EXPECT_TRUE(cond(s, "VersionNT >= 500"));
    EXPECT_FALSE(cond(s, "VersionNT < 500"));

    // Numeric comparison must not be done textually: "603" < "70" as text.
    s.set_property("Small", "70");
    EXPECT_TRUE(cond(s, "VersionNT > Small"));

    // Non-numeric operands fall back to text comparison.
    s.set_property("Name", "beta");
    EXPECT_TRUE(cond(s, "Name > \"alpha\""));
}

TEST(MsiCondition, SupportsTheOperatorsRealPackagesUse) {
    Session s = make_session();
    s.set_property("REMOVE", "ALL");

    EXPECT_TRUE(cond(s, "REMOVE = \"ALL\""));
    EXPECT_FALSE(cond(s, "REMOVE <> \"ALL\""));
    EXPECT_TRUE(cond(s, "REMOVE ~= \"all\""));   // case-insensitive equality
    EXPECT_FALSE(cond(s, "REMOVE = \"all\""));   // ...but plain = is not

    // Substring / prefix / suffix operators.
    s.set_property("Path", "C:\\Program Files\\App");
    EXPECT_TRUE(cond(s, "Path >< \"Program\""));
    EXPECT_TRUE(cond(s, "Path << \"C:\""));
    EXPECT_TRUE(cond(s, "Path >> \"App\""));
}

TEST(MsiCondition, HonorsPrecedenceAndParentheses) {
    Session s = make_session();
    s.set_property("A", "1");
    s.set_property("B", "1");
    // C deliberately unset -> false

    // AND binds tighter than OR, so this is A OR (B AND C) = true.
    EXPECT_TRUE(cond(s, "A OR B AND C"));
    // Parentheses override that: (A OR B) AND C = false.
    EXPECT_FALSE(cond(s, "(A OR B) AND C"));

    // A real sequence condition from an installer package.
    EXPECT_TRUE(cond(s, "(NOT Installed AND NOT PATCH) AND (A <> 0)"));
}

TEST(MsiCondition, StateOperandsWithoutStateAreNullAndBadSyntaxFails) {
    const Session s = make_session();

    // Before costing a component/feature state is a null operand (oracle:
    // "cond [$C1 = 3] -> 0", "cond [&NoFeat = \"\"] -> 1").
    EXPECT_EQ(s.evaluate_condition("$Component1 = 3"), std::optional<bool>(false));
    EXPECT_EQ(s.evaluate_condition("&Feature1 > 2"), std::optional<bool>(false));
    EXPECT_EQ(s.evaluate_condition("&Feature1 = \"\""), std::optional<bool>(true));
    EXPECT_FALSE(s.evaluate_condition("A AND").has_value());
    EXPECT_FALSE(s.evaluate_condition("(A OR B").has_value());
}

// =============================================================================
// Formatted strings
// =============================================================================

TEST(MsiFormat, ExpandsPropertyReferences) {
    const Session s = make_session();
    EXPECT_EQ(s.format("Software\\Uninstall\\[ProductCode]"),
              "Software\\Uninstall\\{5DB65884-C963-4454-AABA-4CA3089281FA}");
    EXPECT_EQ(s.format("no references here"), "no references here");
}

TEST(MsiFormat, UnresolvedReferenceBecomesEmpty) {
    const Session s = make_session();
    // Windows expands an unset property to nothing rather than leaving the
    // literal text, so a registry value keyed on it comes out blank, not
    // "[NotSet]".
    EXPECT_EQ(s.format("x[NotSetAnywhere]y"), "xy");
}

TEST(MsiFormat, HandlesEscapesAndUnmatchedBrackets) {
    const Session s = make_session();
    EXPECT_EQ(s.format("100[\\%]"), "100%");
    EXPECT_EQ(s.format("a[b"), "a[b");   // unmatched bracket stays literal
}

// =============================================================================
// msiexec command line
// =============================================================================

TEST(MsiexecCommandLine, ParsesTheQuietInstallFormInstallersUse) {
    // This is verbatim what the Need for Speed Shift bootstrapper issues.
    const auto cmd = parse_msiexec_command_line(
        "msiexec.exe /i \"D:\\PhysX\\PhysX_9.07.20_9.09.0720_SystemSoftware.msi\""
        " /quiet");

    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->mode, MsiexecCommand::Mode::Install);
    EXPECT_EQ(cmd->package,
              "D:\\PhysX\\PhysX_9.07.20_9.09.0720_SystemSoftware.msi");
    EXPECT_TRUE(cmd->quiet);
}

TEST(MsiexecCommandLine, AcceptsTheQuietSpellings) {
    for (const char* form : {"/quiet", "/qn", "/q", "/passive", "-qn"}) {
        const auto cmd = parse_msiexec_command_line(
            std::string("msiexec.exe /i pkg.msi ") + form);
        ASSERT_TRUE(cmd.has_value()) << form;
        EXPECT_TRUE(cmd->quiet) << form;
    }

    // Without a quiet switch the install is interactive.
    const auto interactive = parse_msiexec_command_line("msiexec.exe /i pkg.msi");
    ASSERT_TRUE(interactive.has_value());
    EXPECT_FALSE(interactive->quiet);
}

TEST(MsiexecCommandLine, CollectsPropertyOverrides) {
    const auto cmd = parse_msiexec_command_line(
        "msiexec.exe /i pkg.msi SKIP_PHYSX_DRIVER_SET=1 INSTALLDIR=\"C:\\Apps\\X\""
        " /qn");

    ASSERT_TRUE(cmd.has_value());
    EXPECT_TRUE(cmd->quiet);
    ASSERT_EQ(cmd->properties.count("SKIP_PHYSX_DRIVER_SET"), 1u);
    EXPECT_EQ(cmd->properties.at("SKIP_PHYSX_DRIVER_SET"), "1");
    ASSERT_EQ(cmd->properties.count("INSTALLDIR"), 1u);
    EXPECT_EQ(cmd->properties.at("INSTALLDIR"), "C:\\Apps\\X");

    // A property assignment must never be mistaken for the package path.
    EXPECT_EQ(cmd->package, "pkg.msi");
}

TEST(MsiexecCommandLine, DistinguishesModes) {
    const auto uninstall = parse_msiexec_command_line(
        "msiexec.exe /x {5DB65884-C963-4454-AABA-4CA3089281FA}");
    ASSERT_TRUE(uninstall.has_value());
    EXPECT_EQ(uninstall->mode, MsiexecCommand::Mode::Uninstall);
    EXPECT_EQ(uninstall->package, "{5DB65884-C963-4454-AABA-4CA3089281FA}");

    // Switch value may be attached with no space.
    const auto attached = parse_msiexec_command_line("msiexec.exe /ipkg.msi");
    ASSERT_TRUE(attached.has_value());
    EXPECT_EQ(attached->mode, MsiexecCommand::Mode::Install);
    EXPECT_EQ(attached->package, "pkg.msi");
}

TEST(MsiexecCommandLine, LogPathIsNotMistakenForThePackage) {
    // /l takes a filename argument; consuming it wrongly would leave the log
    // path as the package and try to install the log file.
    const auto cmd = parse_msiexec_command_line(
        "msiexec.exe /i real.msi /l*v C:\\install.log /qn");
    ASSERT_TRUE(cmd.has_value());
    EXPECT_EQ(cmd->package, "real.msi");
    EXPECT_TRUE(cmd->quiet);
}

// =============================================================================
// Directory resolution
// =============================================================================

TEST(MsiDirectory, SplitsDefaultDirTargetSourceAndLongName) {
    // `target:source` — only the target half names the installed directory.
    EXPECT_EQ(default_dir_target("AGEIAT~1|AGEIA Technologies"),
              "AGEIA Technologies");
    EXPECT_EQ(default_dir_target(".:ProgramF|Program Files"), "");
    EXPECT_EQ(default_dir_target("bin"), "bin");

    // A short|long pair on the target side keeps the long name.
    EXPECT_EQ(default_dir_target("PROGRA~1|Program Files:SRC"), "Program Files");
}

TEST(MsiDirectory, DotContributesNoPathSegment) {
    // Merge modules emit rows whose DefaultDir is "." or ".:.", meaning the
    // row adds no directory level. Treating "." as a literal name produces
    // paths like "Program Files\.\bin".
    EXPECT_EQ(default_dir_target("."), "");
    EXPECT_EQ(default_dir_target(".:."), "");
}

// =============================================================================
// Source-side name resolution
//
// Every uncompressed file is found through these two splits, and both fail
// silently: a wrong source directory or short-name choice makes the file
// missing, which reads as a media problem rather than a parsing one.
// =============================================================================

TEST(MsiSourceNames, SourceHalfIsTheOneAfterTheColon) {
    // The real INSTALLLOCATION row from a shipping package: a mangled short
    // target name, a long target name, and a source of "." meaning the media
    // root itself.
    EXPECT_EQ(default_dir_source("0finvyjr|Need for Speed SHIFT:."), "");
    EXPECT_EQ(default_dir_target("0finvyjr|Need for Speed SHIFT:."),
              "Need for Speed SHIFT");
}

TEST(MsiSourceNames, WithoutAColonOneNameServesAsBoth) {
    EXPECT_EQ(default_dir_source("Vehicles"), "Vehicles");
    EXPECT_EQ(default_dir_target("Vehicles"), "Vehicles");
    EXPECT_EQ(default_dir_source("4bsb51xs|Organisers"), "Organisers");
}

TEST(MsiSourceNames, SourceAndTargetDifferWhenBothHalvesAreGiven) {
    EXPECT_EQ(default_dir_target("bin:sourcebin"), "bin");
    EXPECT_EQ(default_dir_source("bin:sourcebin"), "sourcebin");
    EXPECT_EQ(default_dir_source("shrt|LongTarget:srcs|LongSource"), "LongSource");
}

TEST(MsiSourceNames, LongHalfOfAFileNameIsTheInstalledName) {
    EXPECT_EQ(long_file_name("grh3ixdo.bff|Aston_Martin_DB9.bff"),
              "Aston_Martin_DB9.bff");
    EXPECT_EQ(long_file_name("shift.exe"), "shift.exe");
}

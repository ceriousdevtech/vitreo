// SPDX-License-Identifier: LGPL-2.1-only
/// @file test_msi_registration.cpp
/// @brief Contract tests for the Windows Installer registration area, both
/// bitnesses through msi_fixture.hpp.
///
/// Expected values come from Windows 11 (tools/oracle_probe/msi_registration_*):
///   * the packed-GUID key names of the installed-products database;
///   * the query exports replayed over the registry state Windows wrote for
///     the test package (msi_registration_{machine,user}_x86_state.expected
///     loaded into a capsule registry), each output line compared with the
///     line msi_registration_api_probe.c printed there (…_api32/_api64);
///   * the registration actions run by the engine over the same package
///     (tests/winapi/msi/fixtures/registration_x86.msi), the registry they
///     write compared with what Windows wrote.

#include "msi_fixture.hpp"

#include <capsule/registry/registry.hpp>
#include <capsule/vfs/host_filesystem.hpp>
#include <capsule/vfs/vfs.hpp>
#include <winapi/kernel32/locale/string_conv.hpp>
#include <winapi/system/msi/msi_engine.hpp>
#include <winapi/system/msi/msi_features.hpp>
#include <winapi/system/msi/msi_register.hpp>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace vitreo::winapi::msi_test {
namespace {

using msi::ProductDatabase;
using msi::ProductInstance;

const std::string kDir = VITREO_ORACLE_PROBE_DIR;
const std::string kProduct = "{EA844198-B6C8-4E38-9ABA-6BA7E226E338}";
const std::string kUserSid = "S-1-5-21-1828410828-4144948699-2202159460-1001";  // the oracle's user

std::vector<std::string> read_lines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        out.push_back(l);
    }
    return out;
}

std::wstring wide(const std::string& s) {
    const std::vector<u16> u = utf8_to_utf16(s);
    return std::wstring(u.begin(), u.end());
}

/// Load a msi_registration_dump.ps1 state into @p reg in the physical 64-bit
/// layout: 64-bit-view keys as printed; 32-bit-view keys only where the view
/// is redirected and nothing else holds them (the ARP Uninstall entry and
/// HKLM\SOFTWARE\<x> of a 32-bit component -> SOFTWARE\WOW6432Node\<x>).
void load_state(capsule::Registry& reg, const std::string& file) {
    u64 hive = 0;
    std::wstring key;
    bool skip = true;
    for (const std::string& l : read_lines(file)) {
        if (l.rfind("[", 0) == 0) {
            skip = true;
            const usize close = l.find("] ");
            if (close == std::string::npos) continue;
            const std::string tag = l.substr(1, close - 1), path = l.substr(close + 2);
            if (tag == "LocalMachine/64") hive = capsule::HKEY_LOCAL_MACHINE;
            else if (tag == "CurrentUser/64") hive = capsule::HKEY_CURRENT_USER;
            else if (tag == "LocalMachine/32" && path.rfind("SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0) == 0) {
                hive = capsule::HKEY_LOCAL_MACHINE;
                key = wide("SOFTWARE\\WOW6432Node\\" + path.substr(9));
                auto k = reg.open_key(hive, key, true);
                if (k.is_ok()) reg.close_key(k.value());
                skip = false;
                continue;
            } else continue;
            key = wide(path);
            auto k = reg.open_key(hive, key, true);
            if (k.is_ok()) reg.close_key(k.value());
            skip = false;
            continue;
        }
        if (skip || l.rfind("  ", 0) != 0) continue;
        const usize eq = l.find(" = ");
        const usize colon = l.find(':', eq + 3);
        if (eq == std::string::npos || colon == std::string::npos) continue;
        std::string name = l.substr(2, eq - 2);
        if (name == "@") name.clear();
        const std::string kind = l.substr(eq + 3, colon - eq - 3), data = l.substr(colon + 1);
        capsule::RegValue v;
        if (kind == "String") v = {capsule::RegType::Sz, wide(data)};
        else if (kind == "ExpandString") v = {capsule::RegType::ExpandSz, wide(data)};
        else if (kind == "DWord") v = {capsule::RegType::Dword, static_cast<u32>(std::stoul(data))};
        else if (kind == "MultiString") {
            std::vector<std::wstring> items;
            for (usize p = 0; p < data.size();) {
                const usize end = data.find('>', p);
                items.push_back(wide(data.substr(p + 1, end - p - 1)));
                p = end + 1;
            }
            v = {capsule::RegType::MultiSz, items};
        } else continue;
        auto k = reg.open_key(hive, key, true);
        ASSERT_TRUE(k.is_ok());
        (void)reg.set_value(k.value(), wide(name), v);
        reg.close_key(k.value());
    }
}

// ---- packed GUIDs -----------------------------------------------------------------------

TEST(MsiRegistrationFormats, PackedGuidsAreTheRegistryKeyNamesWindowsWrote) {
    // Key names from msi_registration_machine_x86_state.expected.
    const std::pair<const char*, const char*> cases[] = {
        {"{EA844198-B6C8-4E38-9ABA-6BA7E226E338}", "891448AE8C6B83E4A9ABB67A2E623E83"},  // product
        {"{665B9F8B-95FD-4EE0-9518-7A88AB659F5F}", "B8F9B566DF590EE45981A788BA56F9F5"},  // upgrade code
        {"{C318B68A-7537-474E-800A-9A7790816101}", "A86B813C7357E47408A0A97709181610"},  // category
        {"{E5371B0F-C703-4264-9D9A-800DEA3AE7AD}", "F0B1735E307C4624D9A908D0AEA37EDA"},  // CompMain
        {"{A2DD1F8C-F078-4E15-AB6A-DF2C1DC67078}", "C8F1DD2A870F51E4BAA6FDC2D16C0787"},  // CompReg
        {"{626981AD-DB11-4521-AE67-15B9B8C2A693}", "DA18962611BD1254EA76519B8B2C6A39"},  // package code
    };
    for (const auto& [guid, packed] : cases) {
        EXPECT_EQ(msi::pack_guid(guid), packed);
        EXPECT_EQ(msi::unpack_guid(packed), guid);
        std::string lower = guid;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        EXPECT_EQ(msi::pack_guid(lower), packed);
    }
    EXPECT_EQ(msi::pack_guid("{bad}"), "");
    EXPECT_EQ(msi::unpack_guid("891448AE8C6B83E4A9ABB67A2E623E8"), "");
}

// ---- query exports over the registry Windows wrote --------------------------------------------

/// Replays msi_registration_api_probe.c through the exports and checks every
/// line it prints against the probe's output on Windows.
class Msi_registration_Test : public MsiTest {
protected:
    void SetUp() override {
        MsiTest::SetUp();
        mgr.set_registry(&reg);
        mgr.set_user_sid(wide(kUserSid));
    }

    /// Bump-allocated guest strings.
    GuestAddr wstr(const std::string& s) {
        const GuestAddr at = next;
        const std::vector<u16> u = utf8_to_utf16(s);
        for (usize i = 0; i < u.size(); ++i) EXPECT_TRUE(memory.write_u16(at + i * 2, u[i]).is_ok());
        EXPECT_TRUE(memory.write_u16(at + u.size() * 2, 0).is_ok());
        next += (u.size() + 1) * 2 + 16;
        return at;
    }
    GuestAddr astr(const std::string& s) {
        const GuestAddr at = next;
        for (usize i = 0; i < s.size(); ++i) EXPECT_TRUE(memory.write_u8(at + i, static_cast<u8>(s[i])).is_ok());
        EXPECT_TRUE(memory.write_u8(at + s.size(), 0).is_ok());
        next += s.size() + 17;
        return at;
    }
    GuestAddr buf(usize bytes) {
        const GuestAddr at = next;
        std::vector<u8> z(bytes, 0);
        EXPECT_TRUE(memory.write_bytes(at, z.data(), z.size()).is_ok());
        next += bytes + 16;
        return at;
    }
    std::string rw(GuestAddr at) {
        std::vector<u16> u;
        for (usize i = 0;; ++i) {
            const u16 c = memory.read_u16(at + i * 2).value();
            if (c == 0) break;
            u.push_back(c);
        }
        return utf16_to_utf8(u.data(), static_cast<int>(u.size()));
    }
    std::string ra(GuestAddr at) {
        std::string s;
        for (usize i = 0;; ++i) {
            const u8 c = memory.read_u8(at + i).value();
            if (c == 0) break;
            s.push_back(static_cast<char>(c));
        }
        return s;
    }
    u32 dword(GuestAddr at) { return memory.read_u32(at).value(); }
    GuestAddr cch(u32 v) {
        const GuestAddr at = buf(4);
        write_int(at, static_cast<i32>(v));
        return at;
    }
    static std::string i(u64 v) { return std::to_string(static_cast<i32>(static_cast<u32>(v))); }
    static std::string u(u64 v) { return std::to_string(static_cast<u32>(v)); }

    /// The probe's "label ret=R cch=C [value]" line.
    std::string out(const std::string& label, u64 r, GuestAddr b, u32 c) {
        const u32 rr = static_cast<u32>(r);
        return label + " ret=" + std::to_string(rr) + " cch=" + std::to_string(c) + " [" +
               ((rr == 0 || rr == 234) && b ? rw(b) : "") + "]";
    }

    void expect_lines(const std::string& expected_file, const std::vector<std::string>& produced) {
        const auto lines = read_lines(expected_file);
        ASSERT_FALSE(lines.empty()) << expected_file;
        const std::set<std::string> expected(lines.begin(), lines.end());
        for (const std::string& p : produced) {
            if (expected.count(p)) continue;
            // Show the Windows line(s) with the same label for the mismatch.
            const std::string label = p.substr(0, p.find(" ret=") == std::string::npos ? p.size() : p.find(" ret="));
            std::string windows;
            for (const auto& l : lines)
                if (l.rfind(label + " ", 0) == 0 || l == label) windows += "\n    windows: " + l;
            ADD_FAILURE() << "vitreo:  " << p << windows;
        }
    }

    std::vector<std::string> replay();

    capsule::Registry reg;
    GuestAddr next{BUF};
};

std::vector<std::string> Msi_registration_Test::replay() {
    std::vector<std::string> o;
    const GuestAddr P = wstr(kProduct);
    const char* props[] = {"PackageName", "Transforms", "Language", "ProductName", "AssignmentType", "InstanceType",
                           "AuthorizedLUAApp", "PackageCode", "Version", "ProductIcon", "InstalledProductName",
                           "VersionString", "HelpLink", "HelpTelephone", "InstallLocation", "InstallSource",
                           "InstallDate", "Publisher", "LocalPackage", "URLInfoAbout", "URLUpdateInfo", "VersionMinor",
                           "VersionMajor", "ProductID", "RegCompany", "RegOwner", "InstalledLanguage", "Uninstallable",
                           "State", "PatchType", "LUAEnabled", "DisplayName", "MoreInfoURL", "LastUsedSource",
                           "LastUsedType", "MediaPackagePath", "DiskPrompt", "Comments", "Contact", "Readme", "Size",
                           "EstimatedSize", "NoRepair", "ModifyPath", "UninstallString", "WindowsInstaller", "Clients",
                           "AdvertiseFlags", "DeploymentFlags", "Assignment", "productname", "BogusProperty"};
    const GuestAddr big = buf(2100);
    for (const char* p : props) {
        // InstallDate / LocalPackage / InstallSource hold values of the oracle's
        // run (date, random cache name); they are compared as loaded.
        const GuestAddr name = wstr(p);
        GuestAddr b = big, c = cch(1024);
        u64 r = call("MsiGetProductInfoW", {I(P), I(name), I(b), I(c)});
        o.push_back(out(std::string("info ") + p, r, b, dword(c)));
        for (u32 ctx : {4u, 2u, 1u}) {
            b = big, c = cch(1024);
            r = call("MsiGetProductInfoExW", {I(P), I(ctx == 4 ? 0 : wstr("")), I(ctx), I(name), I(b), I(c)});
            o.push_back(out("infoex" + std::to_string(ctx) + " " + p, r, b, dword(c)));
        }
    }
    const GuestAddr vs = wstr("VersionString");
    {
        GuestAddr c = cch(0);
        u64 r = call("MsiGetProductInfoW", {I(P), I(vs), I(0), I(c)});
        o.push_back(out("info-size null-buf", r, 0, dword(c)));
        for (u32 n : {3u, 5u, 6u, 0u}) {
            GuestAddr b = wstr(n == 0 ? "zz" : "zzzzzz");
            c = cch(n);
            r = call("MsiGetProductInfoW", {I(P), I(vs), I(b), I(c)});
            o.push_back(out("info-size cch" + std::to_string(n), r, b, dword(c)));
        }
        r = call("MsiGetProductInfoW", {I(P), I(vs), I(0), I(0)});
        o.push_back(out("info null-both", r, 0, 0));
        r = call("MsiGetProductInfoW", {I(P), I(vs), I(buf(64)), I(0)});
        o.push_back(out("info buf-no-cch", r, 0, 0));
        const std::pair<const char*, std::string> bad[] = {{"unknown-product", "{00000000-1111-2222-3333-444444444444}"},
                                                           {"bad-guid", "{bad}"},
                                                           {"lowercase-guid", "{ea844198-b6c8-4e38-9aba-6ba7e226e338}"}};
        for (const auto& [label, code] : bad) {
            GuestAddr b = buf(64);
            c = cch(16);
            r = call("MsiGetProductInfoW", {I(wstr(code)), I(vs), I(b), I(c)});
            o.push_back(out(std::string("info ") + label, r, b, dword(c)));
        }
        c = cch(16);
        r = call("MsiGetProductInfoW", {I(0), I(vs), I(buf(64)), I(c)});
        o.push_back(out("info null-product", r, 0, dword(c)));
        c = cch(16);
        r = call("MsiGetProductInfoW", {I(P), I(0), I(buf(64)), I(c)});
        o.push_back(out("info null-prop", r, 0, dword(c)));
        GuestAddr ab = buf(64);
        c = cch(64);
        r = call("MsiGetProductInfoA", {I(astr(kProduct)), I(astr("Publisher")), I(ab), I(c)});
        o.push_back("infoA Publisher ret=" + u(r) + " cch=" + u(dword(c)) + " [" + (r == 0 ? ra(ab) : "") + "]");
        ab = astr("zzzzzzz");
        c = cch(4);
        r = call("MsiGetProductInfoA", {I(astr(kProduct)), I(astr("Publisher")), I(ab), I(c)});
        o.push_back("infoA Publisher cch4 ret=" + u(r) + " cch=" + u(dword(c)) + " [" + ra(ab) + "]");
        const std::tuple<const char*, std::string, u32> ex[] = {{"machine-with-sid", "S-1-5-18", 4},
                                                                {"user-null-sid", "", 2},
                                                                {"user-everyone", "S-1-1-0", 2},
                                                                {"ctx7", "", 7},
                                                                {"ctx0", "", 0}};
        for (const auto& [label, sid, ctx] : ex) {
            GuestAddr b = buf(64);
            c = cch(16);
            r = call("MsiGetProductInfoExW", {I(P), I(sid.empty() ? 0 : wstr(sid)), I(ctx), I(vs), I(b), I(c)});
            o.push_back(out(std::string("infoex ") + label, r, b, dword(c)));
        }
    }
    // products
    {
        const GuestAddr code = buf(80);
        u32 found = 0;
        u64 r = 0;
        for (u32 n = 0; (r = call("MsiEnumProductsW", {I(n), I(code)})) == 0; ++n)
            if (rw(code) == kProduct) { ++found; o.push_back("enum found-at-index>=0"); }
        o.push_back("enum found=" + std::to_string(found) + " end=" + u(r));
        o.push_back("enum null-buf ret=" + u(call("MsiEnumProductsW", {I(0), I(0)})));
        const std::pair<std::string, u32> q[] = {{"", 4}, {"", 2}, {"", 7}, {"S-1-1-0", 7}, {"S-1-1-0", 2}, {"S-1-5-18", 4}, {"", 0}, {"", 8}};
        for (const auto& [sid, ctx] : q) {
            const GuestAddr inst = buf(80), got = buf(4), sb = buf(260), sc = cch(128);
            write_int(got, 0);
            r = call("MsiEnumProductsExW", {I(P), I(sid.empty() ? 0 : wstr(sid)), I(ctx), I(0), I(inst), I(got), I(sb), I(sc)});
            const std::string sl = sid.empty() ? "(null)" : sid;
            o.push_back("enumex sid=" + sl + " ctx=" + std::to_string(ctx) + " ret=" + u(r) + " inst=[" + (r == 0 ? rw(inst) : "") +
                        "] got=" + i(dword(got)) + " cch=" + u(dword(sc)) + " sid=[" + (r == 0 ? rw(sb) : "") + "]");
            write_int(sc, 128);
            r = call("MsiEnumProductsExW", {I(P), I(sid.empty() ? 0 : wstr(sid)), I(ctx), I(1), I(inst), I(got), I(sb), I(sc)});
            o.push_back("enumex1 sid=" + sl + " ctx=" + std::to_string(ctx) + " ret=" + u(r));
        }
        {
            const GuestAddr inst = buf(80), got = buf(4), sb = buf(8), sc = cch(2);
            write_int(got, 0);
            r = call("MsiEnumProductsExW", {I(P), I(wstr("S-1-1-0")), I(7), I(0), I(inst), I(got), I(sb), I(sc)});
            o.push_back("enumex small-sid ret=" + u(r) + " cch=" + u(dword(sc)) + " got=" + i(dword(got)));
            write_int(sc, 0);
            write_int(got, 0);
            r = call("MsiEnumProductsExW", {I(P), I(wstr("S-1-1-0")), I(7), I(0), I(inst), I(got), I(0), I(sc)});
            o.push_back("enumex null-sid-buf ret=" + u(r) + " cch=" + u(dword(sc)) + " got=" + i(dword(got)));
        }
        o.push_back("state " + i(call("MsiQueryProductStateW", {I(P)})));
        o.push_back("state unknown " + i(call("MsiQueryProductStateW", {I(wstr("{00000000-1111-2222-3333-444444444444}"))})));
        o.push_back("state bad " + i(call("MsiQueryProductStateW", {I(wstr("nope"))})));
        const GuestAddr upg = wstr("{665B9F8B-95FD-4EE0-9518-7A88AB659F5F}");
        u32 n = 0;
        for (;; ++n) {
            r = call("MsiEnumRelatedProductsW", {I(upg), I(0), I(n), I(code)});
            if (r) break;
            o.push_back("related " + std::to_string(n) + " [" + rw(code) + "]");
        }
        o.push_back("related end=" + u(r));
        o.push_back("related reserved=" + u(call("MsiEnumRelatedProductsW", {I(upg), I(1), I(0), I(code)})));
        o.push_back("related bad=" + u(call("MsiEnumRelatedProductsW", {I(wstr("x")), I(0), I(0), I(code)})));
        const GuestAddr el = buf(4);
        write_int(el, 7);
        r = call("MsiIsProductElevatedW", {I(P), I(el)});
        o.push_back("elevated ret=" + u(r) + " val=" + i(dword(el)));
        write_int(el, 7);
        r = call("MsiIsProductElevatedW", {I(wstr("{00000000-1111-2222-3333-444444444444}")), I(el)});
        o.push_back("elevated unknown ret=" + u(r) + " val=" + i(dword(el)));
        r = call("MsiGetProductCodeFromPackageCodeW", {I(wstr("{626981AD-DB11-4521-AE67-15B9B8C2A693}")), I(code)});
        o.push_back("from-package ret=" + u(r) + " [" + (r == 0 ? rw(code) : "") + "]");
        r = call("MsiGetProductCodeFromPackageCodeW", {I(wstr("{00000000-1111-2222-3333-444444444444}")), I(code)});
        o.push_back("from-package unknown ret=" + u(r));
        const GuestAddr ub = buf(160), ob = buf(160), sb = buf(160), uc = cch(64), oc = cch(64), sc = cch(64);
        r = call("MsiGetUserInfoW", {I(P), I(ub), I(uc), I(ob), I(oc), I(sb), I(sc)});
        const bool present = static_cast<i32>(r) == 1;
        o.push_back("userinfo state=" + i(r) + " user=[" + (present ? rw(ub) : "") + "] " + u(dword(uc)) + " org=[" +
                    (present ? rw(ob) : "") + "] " + u(dword(oc)) + " serial=[" + (present ? rw(sb) : "") + "] " + u(dword(sc)));
        write_int(uc, 2);
        r = call("MsiGetUserInfoW", {I(P), I(ub), I(uc), I(0), I(0), I(0), I(0)});
        o.push_back("userinfo small state=" + i(r) + " cu=" + u(dword(uc)));
        r = call("MsiGetUserInfoW", {I(wstr("{00000000-1111-2222-3333-444444444444}")), I(0), I(0), I(0), I(0), I(0), I(0)});
        o.push_back("userinfo unknown state=" + i(r));
    }
    // features (the enumeration order is checked separately: registry value order)
    {
        const char* names[] = {"FeatMain", "FeatSub", "FeatData", "FeatAbsent", "NoSuchFeature", "featmain"};
        for (const char* f : names) {
            const GuestAddr fn = wstr(f), st = buf(4);
            o.push_back(std::string("fstate ") + f + " " + i(call("MsiQueryFeatureStateW", {I(P), I(fn)})));
            for (u32 ctx : {4u, 2u}) {
                write_int(st, 99);
                const u64 r = call("MsiQueryFeatureStateExW", {I(P), I(0), I(ctx), I(fn), I(st)});
                o.push_back("fstateex" + std::to_string(ctx) + " " + f + " ret=" + u(r) + " st=" + i(dword(st)));
            }
        }
        o.push_back("feature unknown ret=" +
                    u(call("MsiEnumFeaturesW", {I(wstr("{00000000-1111-2222-3333-444444444444}")), I(0), I(buf(80)), I(buf(80))})));
        o.push_back("feature null-parent ret=" + u(call("MsiEnumFeaturesW", {I(P), I(0), I(buf(80)), I(0)})));
    }
    // components
    {
        const std::pair<const char*, const char*> comps[] = {{"main", "{E5371B0F-C703-4264-9D9A-800DEA3AE7AD}"},
                                                             {"reg", "{A2DD1F8C-F078-4E15-AB6A-DF2C1DC67078}"},
                                                             {"dir", "{BD02CD4F-7A91-4B7F-A41F-3D8F6DFEF98C}"},
                                                             {"absent", "{D2A4D7FF-8734-4D25-83DD-61AB15B73F07}"},
                                                             {"unknown", "{00000000-1111-2222-3333-444444444444}"}};
        const GuestAddr code = buf(80);
        int found = 0;
        u64 r = 0;
        for (u32 n = 0; (r = call("MsiEnumComponentsW", {I(n), I(code)})) == 0; ++n)
            for (int k = 0; k < 4; ++k)
                if (rw(code) == comps[k].second) { o.push_back(std::string("enumcomp found ") + comps[k].first); ++found; }
        o.push_back("enumcomp end=" + u(r) + " found=" + std::to_string(found));
        for (const auto& [name, id] : comps) {
            const GuestAddr C = wstr(id);
            GuestAddr b = buf(1100), c = cch(512);
            r = call("MsiGetComponentPathW", {I(P), I(C), I(b), I(c)});
            o.push_back(std::string("comppath ") + name + " st=" + i(r) + " cch=" + u(dword(c)) + " [" + (static_cast<i32>(r) >= 0 ? rw(b) : "") + "]");
            b = buf(1100), c = cch(512);
            r = call("MsiLocateComponentW", {I(C), I(b), I(c)});
            o.push_back(std::string("locate ") + name + " st=" + i(r) + " cch=" + u(dword(c)) + " [" + (static_cast<i32>(r) >= 0 ? rw(b) : "") + "]");
            r = call("MsiGetProductCodeW", {I(C), I(code)});
            o.push_back(std::string("prodcode ") + name + " ret=" + u(r) + " [" + (r == 0 ? rw(code) : "") + "]");
            u32 n = 0;
            for (; (r = call("MsiEnumClientsW", {I(C), I(n), I(code)})) == 0; ++n)
                o.push_back(std::string("clients ") + name + " " + std::to_string(n) + " [" + rw(code) + "]");
            o.push_back(std::string("clients ") + name + " end=" + u(r));
            const GuestAddr st = buf(4);
            for (u32 ctx : {4u, 2u}) {
                write_int(st, 99);
                r = call("MsiQueryComponentStateW", {I(P), I(0), I(ctx), I(C), I(st)});
                o.push_back("cstate" + std::to_string(ctx) + " " + name + " ret=" + u(r) + " st=" + i(dword(st)));
            }
            for (u32 ctx : {4u, 2u}) {
                b = buf(1100), c = cch(512);
                r = call("MsiGetComponentPathExW", {I(P), I(C), I(0), I(ctx), I(b), I(c)});
                o.push_back("comppathex" + std::to_string(ctx) + " " + name + " st=" + i(r) + " cch=" + u(dword(c)) + " [" +
                            (static_cast<i32>(r) >= 0 ? rw(b) : "") + "]");
            }
            n = 0;
            for (;; ++n) {
                const GuestAddr got = buf(4), sb = buf(260), sc = cch(128);
                r = call("MsiEnumClientsExW", {I(C), I(wstr("S-1-1-0")), I(7), I(n), I(code), I(got), I(sb), I(sc)});
                if (r) break;
                o.push_back(std::string("clientsex ") + name + " " + std::to_string(n) + " [" + rw(code) + "] ctx=" + i(dword(got)) +
                            " sid=[" + rw(sb) + "]");
            }
            o.push_back(std::string("clientsex ") + name + " end=" + u(r));
        }
        for (u32 n = 0;; ++n) {
            const GuestAddr got = buf(4), sb = buf(260), sc = cch(128);
            r = call("MsiEnumComponentsExW", {I(wstr("S-1-1-0")), I(7), I(n), I(code), I(got), I(sb), I(sc)});
            if (r) break;
            for (int k = 0; k < 4; ++k)
                if (rw(code) == comps[k].second)
                    o.push_back(std::string("enumcompex ") + comps[k].first + " ctx=" + i(dword(got)) + " sid=[" + rw(sb) + "]");
        }
        o.push_back("enumcompex end=" + u(r));
        const GuestAddr main = wstr(comps[0].second);
        GuestAddr c = cch(4);
        r = call("MsiGetComponentPathW", {I(P), I(main), I(buf(64)), I(c)});
        o.push_back("comppath small st=" + i(r) + " cch=" + u(dword(c)));
        c = cch(0);
        r = call("MsiGetComponentPathW", {I(P), I(main), I(0), I(c)});
        o.push_back("comppath null-buf st=" + i(r) + " cch=" + u(dword(c)));
        o.push_back("comppath null-both st=" + i(call("MsiGetComponentPathW", {I(P), I(main), I(0), I(0)})));
        o.push_back("comppath bad st=" + i(call("MsiGetComponentPathW", {I(wstr("x")), I(main), I(0), I(0)})));
        // qualified components
        const GuestAddr cat = wstr("{C318B68A-7537-474E-800A-9A7790816101}");
        for (u32 n = 0;; ++n) {
            const GuestAddr q = buf(260), a = buf(260), qc = cch(128), ac = cch(128);
            r = call("MsiEnumComponentQualifiersW", {I(cat), I(n), I(q), I(qc), I(a), I(ac)});
            if (r) break;
            o.push_back("qualifier " + std::to_string(n) + " [" + rw(q) + "] " + u(dword(qc)) + " [" + rw(a) + "] " + u(dword(ac)));
        }
        o.push_back("qualifier end=" + u(r));
        {
            const GuestAddr qc = cch(3), ac = cch(128);
            r = call("MsiEnumComponentQualifiersW", {I(cat), I(0), I(buf(64)), I(qc), I(buf(260)), I(ac)});
            o.push_back("qualifier small ret=" + u(r) + " cq=" + u(dword(qc)));
        }
        const std::tuple<const char*, std::string, std::string, i32> pq[] = {
            {"existing", "{C318B68A-7537-474E-800A-9A7790816101}", "probe-qualifier", -1},
            {"nodetection", "{C318B68A-7537-474E-800A-9A7790816101}", "probe-qualifier", -2},
            {"missing-qualifier", "{C318B68A-7537-474E-800A-9A7790816101}", "nope", -1},
            {"unknown-category", "{00000000-1111-2222-3333-444444444444}", "nope", -1}};
        for (const auto& [label, category, qual, mode] : pq) {
            const GuestAddr b = buf(1100);
            c = cch(512);
            r = call("MsiProvideQualifiedComponentW", {I(wstr(category)), I(wstr(qual)), I(static_cast<u32>(mode)), I(b), I(c)});
            o.push_back(out(std::string("provideq ") + label, r, b, dword(c)));
        }
        const std::tuple<const char*, const char*, const char*, i32> pc[] = {
            {"existing", "FeatMain", "{E5371B0F-C703-4264-9D9A-800DEA3AE7AD}", -1},
            {"nodetection", "FeatMain", "{E5371B0F-C703-4264-9D9A-800DEA3AE7AD}", -2},
            {"absent", "FeatAbsent", "{D2A4D7FF-8734-4D25-83DD-61AB15B73F07}", -1},
            {"reg", "FeatMain", "{A2DD1F8C-F078-4E15-AB6A-DF2C1DC67078}", -1}};
        for (const auto& [label, feature, comp, mode] : pc) {
            const GuestAddr b = buf(1100);
            c = cch(512);
            r = call("MsiProvideComponentW", {I(P), I(wstr(feature)), I(wstr(comp)), I(static_cast<u32>(mode)), I(b), I(c)});
            o.push_back(out(std::string("provide ") + label, r, b, dword(c)));
        }
    }
    // source lists
    {
        const char* sprops[] = {"MediaPackagePath", "DiskPrompt", "LastUsedSource", "LastUsedType", "PackageName", "BogusProperty"};
        for (u32 ctx : {4u, 2u}) {
            for (int t = 0; t < 2; ++t) {
                const u32 opt = t ? 2u : 1u;
                u64 r = 0;
                for (u32 n = 0;; ++n) {
                    const GuestAddr b = buf(1100), c = cch(512);
                    r = call("MsiSourceListEnumSourcesW", {I(P), I(0), I(ctx), I(opt), I(n), I(b), I(c)});
                    if (r) break;
                    o.push_back("src" + std::to_string(ctx) + " type" + std::to_string(t) + " " + std::to_string(n) + " [" + rw(b) + "] " + u(dword(c)));
                }
                o.push_back("src" + std::to_string(ctx) + " type" + std::to_string(t) + " end=" + u(r));
            }
            for (const char* p : sprops) {
                const GuestAddr b = buf(1100), c = cch(512);
                const u64 r = call("MsiSourceListGetInfoW", {I(P), I(0), I(ctx), I(0), I(wstr(p)), I(b), I(c)});
                o.push_back(out("srcinfo" + std::to_string(ctx) + " " + p, r, b, dword(c)));
            }
            u64 r = 0;
            for (u32 n = 0;; ++n) {
                const GuestAddr id = buf(4), vb = buf(260), vc = cch(128), pb = buf(260), pcc = cch(128);
                r = call("MsiSourceListEnumMediaDisksW", {I(P), I(0), I(ctx), I(0), I(n), I(id), I(vb), I(vc), I(pb), I(pcc)});
                if (r) break;
                o.push_back("disk" + std::to_string(ctx) + " " + std::to_string(n) + " id=" + u(dword(id)) + " [" + rw(vb) + "] " +
                            u(dword(vc)) + " [" + rw(pb) + "] " + u(dword(pcc)));
            }
            o.push_back("disk" + std::to_string(ctx) + " end=" + u(r));
        }
        const GuestAddr pn = wstr("PackageName");
        GuestAddr b = buf(1100), c = cch(512);
        u64 r = call("MsiSourceListGetInfoW", {I(P), I(wstr("S-1-5-18")), I(4), I(0), I(pn), I(b), I(c)});
        o.push_back(out("srcinfo machine-with-sid", r, b, dword(c)));
        b = buf(1100), c = cch(512);
        r = call("MsiSourceListGetInfoW", {I(wstr("{00000000-1111-2222-3333-444444444444}")), I(0), I(4), I(0), I(pn), I(b), I(c)});
        o.push_back(out("srcinfo unknown", r, b, dword(c)));
        b = buf(1100), c = cch(512);
        r = call("MsiSourceListGetInfoW", {I(P), I(0), I(4), I(0x40000000), I(pn), I(b), I(c)});
        o.push_back(out("srcinfo patch-code", r, b, dword(c)));
        r = call("MsiEnumPatchesW", {I(P), I(0), I(buf(80)), I(buf(128)), I(cch(64))});
        o.push_back("patches ret=" + u(r));
        r = call("MsiEnumPatchesExW", {I(P), I(0), I(4), I(15), I(0), I(buf(80)), I(buf(80)), I(buf(4)), I(buf(128)), I(cch(64))});
        o.push_back("patchesex ret=" + u(r));
        r = call("MsiGetPatchInfoW", {I(wstr("{00000000-1111-2222-3333-444444444444}")), I(wstr("LocalPackage")), I(buf(128)), I(cch(64))});
        o.push_back("patchinfo ret=" + u(r));
        r = call("MsiGetPatchInfoExW", {I(wstr("{00000000-1111-2222-3333-444444444444}")), I(P), I(0), I(4), I(wstr("State")),
                                        I(buf(128)), I(cch(64))});
        o.push_back("patchinfoex ret=" + u(r));
    }
    return o;
}

TEST_P(Msi_registration_Test, QueriesOfAPerMachineInstallMatchWindows) {
    load_state(reg, kDir + "/msi_registration_machine_x86_state.expected");
    expect_lines(kDir + (x64() ? "/msi_registration_machine_x86_api64.expected" : "/msi_registration_machine_x86_api32.expected"),
                 replay());
}

TEST_P(Msi_registration_Test, QueriesOfAPerUserInstallMatchWindows) {
    load_state(reg, kDir + "/msi_registration_user_x86_state.expected");
    expect_lines(kDir + (x64() ? "/msi_registration_user_x86_api64.expected" : "/msi_registration_user_x86_api32.expected"),
                 replay());
}

TEST_P(Msi_registration_Test, FeatureEnumerationListsEveryPublishedFeatureWithItsParent) {
    load_state(reg, kDir + "/msi_registration_machine_x86_state.expected");
    // Windows enumerates in publication order (probe "feature N" lines); the
    // capsule registry enumerates values sorted, so compare as a set.
    std::set<std::string> want, got;
    for (const auto& l : read_lines(kDir + "/msi_registration_machine_x86_api32.expected"))
        if (l.rfind("feature ", 0) == 0 && l.find(" [") != std::string::npos) want.insert(l.substr(l.find(" [")));
    const GuestAddr P = wstr(kProduct);
    for (u32 n = 0;; ++n) {
        const GuestAddr f = buf(80), p = buf(80);
        const u64 r = call("MsiEnumFeaturesW", {I(P), I(n), I(f), I(p)});
        if (r) {
            EXPECT_EQ(r, 259u);
            EXPECT_EQ(n, 5u);
            break;
        }
        got.insert(" [" + rw(f) + "] parent [" + rw(p) + "]");
    }
    EXPECT_EQ(got, want);
}

TEST_P(Msi_registration_Test, SourceListEditsFollowLearnIndexRules) {
    load_state(reg, kDir + "/msi_registration_machine_x86_state.expected");
    const GuestAddr P = wstr(kProduct);
    // Learn MsiSourceListAddSourceEx: index 0 appends a new source; an index
    // inside the list inserts there; re-adding an existing one at 0 is a no-op.
    EXPECT_EQ(call("MsiSourceListAddSourceExW", {I(P), I(0), I(4), I(1), I(wstr("\\\\srv\\b\\")), I(0)}), 0u);
    EXPECT_EQ(call("MsiSourceListAddSourceExW", {I(P), I(0), I(4), I(1), I(wstr("\\\\srv\\a\\")), I(1)}), 0u);
    EXPECT_EQ(call("MsiSourceListAddSourceExW", {I(P), I(0), I(4), I(1), I(wstr("\\\\srv\\a\\")), I(0)}), 0u);
    std::vector<std::string> list;
    for (u32 n = 0;; ++n) {
        const GuestAddr b = buf(600), c = cch(256);
        if (call("MsiSourceListEnumSourcesW", {I(P), I(0), I(4), I(1), I(n), I(b), I(c)}) != 0) break;
        list.push_back(rw(b));
    }
    EXPECT_EQ(list, (std::vector<std::string>{"\\\\srv\\a\\", "C:\\Users\\Owner\\msireg\\x86v1\\", "\\\\srv\\b\\"}));
    // S-1-1-0 is rejected by the edit functions (Learn).
    EXPECT_EQ(call("MsiSourceListAddSourceExW", {I(P), I(wstr("S-1-1-0")), I(2), I(1), I(wstr("x")), I(0)}), 87u);
    EXPECT_EQ(call("MsiSourceListClearSourceW", {I(P), I(0), I(4), I(1), I(wstr("\\\\srv\\a\\"))}), 0u);
    EXPECT_EQ(call("MsiSourceListClearSourceW", {I(P), I(0), I(4), I(1), I(wstr("\\\\srv\\a\\"))}), 0u);  // not found: success
    EXPECT_EQ(call("MsiSourceListSetInfoW", {I(P), I(0), I(4), I(0), I(wstr("DiskPrompt")), I(wstr("Insert [1]"))}), 0u);
    const GuestAddr b = buf(200), c = cch(64);
    EXPECT_EQ(call("MsiSourceListGetInfoW", {I(P), I(0), I(4), I(0), I(wstr("DiskPrompt")), I(b), I(c)}), 0u);
    EXPECT_EQ(rw(b), "Insert [1]");
    EXPECT_EQ(call("MsiSourceListSetInfoW", {I(P), I(0), I(4), I(0), I(wstr("LastUsedType")), I(wstr("n"))}), 1608u);
    EXPECT_EQ(call("MsiSourceListForceResolutionExW", {I(P), I(0), I(4), I(0)}), 0u);
    write_int(c, 64);
    EXPECT_EQ(call("MsiSourceListGetInfoW", {I(P), I(0), I(4), I(0), I(wstr("LastUsedType")), I(b), I(c)}), 0u);
    EXPECT_EQ(dword(c), 0u);
}

INSTANTIATE_MSI_BITNESS(Msi_registration_Test);

// ---- the registration actions, run by the engine -------------------------------------------------

/// Installs tests/winapi/msi/fixtures/registration_x86.msi per-machine with
/// the engine (every standard action of its InstallExecuteSequence) into a
/// temporary C: drive and compares the product database it registers with
/// what Windows registered for the same package
/// (msi_registration_machine_x86_state.expected).
class MsiRegistrationInstall : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               ("vitreo_msi_reg_" + std::to_string(::getpid()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "src" / "PFiles" / "VitreoRP");
        std::filesystem::copy_file(std::filesystem::path(__FILE__).parent_path() / "fixtures" / "registration_x86.msi",
                                   root / "src" / "rp.msi");
        std::ofstream(root / "src" / "PFiles" / "VitreoRP" / "main.dll", std::ios::binary)
            << "vitreo registration probe payload\r\n";
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_vfs(&vfs);
        mgr.set_registry(&reg);
        mgr.set_user_sid(wide(kUserSid));
        handlers = msi_handlers(mgr);
        // The provider is process-wide and other suites replace it
        // (test_msi_files resets it): put the registration area's back.
        msi::install_registration_state_provider();
        services.vfs = &vfs;
        services.registry = &reg;
        services.manager = &mgr;
        services.guest_64bit = true;
    }
    void TearDown() override { std::filesystem::remove_all(root); }

    u32 install(std::map<std::string, std::string> props) {
        std::ifstream in(root / "src" / "rp.msi", std::ios::binary);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
        auto pkg = msi::Package::open(bytes);
        if (!pkg) return 1620;
        msi::Engine::Options o;
        o.package_path = "C:\\src\\rp.msi";
        o.ui_level = 2;
        o.server = true;
        o.allow_system_changes = true;
        o.command_line = std::move(props);
        u32 error = 0;
        auto engine = msi::Engine::open(mgr, std::make_shared<msi::Package>(std::move(*pkg)), o, services, error);
        if (!engine) return error;
        return engine->run_top_level("INSTALL");
    }

    /// Value of @p name under @p key as the dump prints it ("Kind:value").
    std::string dump_value(u64 hive, const std::string& key, const std::string& name) {
        auto k = reg.open_key(hive, wide(key), false);
        if (k.is_err()) return "<no key>";
        auto v = reg.query_value(k.value(), wide(name == "@" ? "" : name));
        reg.close_key(k.value());
        if (v.is_err()) return "<no value>";
        auto narrow = [](const std::wstring& w) {
            std::vector<u16> u(w.begin(), w.end());
            return utf16_to_utf8(u.data(), static_cast<int>(u.size()));
        };
        const auto& d = v.value().data;
        switch (v.value().type) {
            case capsule::RegType::Sz: return "String:" + narrow(std::get<std::wstring>(d));
            case capsule::RegType::ExpandSz: return "ExpandString:" + narrow(std::get<std::wstring>(d));
            case capsule::RegType::Dword: return "DWord:" + std::to_string(std::get<u32>(d));
            case capsule::RegType::MultiSz: {
                std::string s = "MultiString:";
                for (const auto& e : std::get<std::vector<std::wstring>>(d)) s += "<" + narrow(e) + ">";
                return s;
            }
            default: return "<other>";
        }
    }

    std::filesystem::path root;
    capsule::Vfs vfs;
    capsule::Registry reg;
    MsiManager mgr;
    msi::HandlerMap handlers;
    msi::Services services;
};

TEST_F(MsiRegistrationInstall, PerMachineInstallRegistersWhatWindowsRegisters) {
    ASSERT_EQ(install({{"ALLUSERS", "1"}, {"SETPID", "1"}}), 0u);
    // Every value Windows wrote under the product-database, class and ARP
    // keys, except the ones that name the oracle's run (date, random cache
    // name, source directory, cost-derived size: compared below).
    const std::set<std::string> run_specific = {"InstallDate", "LocalPackage", "InstallSource", "LastUsedSource", "EstimatedSize"};
    u64 hive = 0;
    std::string key;
    bool on = false;
    int compared = 0;
    for (const std::string& l : read_lines(kDir + "/msi_registration_machine_x86_state.expected")) {
        if (l.rfind("[", 0) == 0) {
            const usize close = l.find("] ");
            const std::string tag = l.substr(1, close - 1);
            key = l.substr(close + 2);
            on = tag == "LocalMachine/64" || (tag == "LocalMachine/32" && key.find("\\Uninstall\\") != std::string::npos);
            if (tag == "LocalMachine/32") key = "SOFTWARE\\WOW6432Node\\" + key.substr(9);
            hive = capsule::HKEY_LOCAL_MACHINE;
            if (on && key.find("SourceList\\Net") != std::string::npos) on = false;  // the source path
            continue;
        }
        if (!on || l.rfind("  ", 0) != 0) continue;
        const usize eq = l.find(" = ");
        const std::string name = l.substr(2, eq - 2), value = l.substr(eq + 3);
        if (run_specific.count(name)) continue;
        ++compared;
        EXPECT_EQ(dump_value(hive, key, name), value) << key << " : " << name;
    }
    EXPECT_GT(compared, 80);
    // The run-specific values, in the forms Windows uses.
    const std::string props = "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Products\\"
                              "891448AE8C6B83E4A9ABB67A2E623E83\\InstallProperties";
    EXPECT_EQ(dump_value(hive, props, "InstallSource"), "String:C:\\src\\");
    const std::string local = dump_value(hive, props, "LocalPackage");
    EXPECT_EQ(local.rfind("String:C:\\Windows\\Installer\\", 0), 0u) << local;
    EXPECT_EQ(local.size(), std::string("String:C:\\Windows\\Installer\\12345678.msi").size()) << local;
    EXPECT_EQ(dump_value(hive, "SOFTWARE\\Classes\\Installer\\Products\\891448AE8C6B83E4A9ABB67A2E623E83\\SourceList",
                         "LastUsedSource"),
              "ExpandString:n;1;C:\\src\\");
    // The cached package is a copy of the package.
    EXPECT_EQ(std::filesystem::file_size(root / "Windows" / "Installer" / local.substr(local.rfind('\\') + 1)),
              std::filesystem::file_size(root / "src" / "rp.msi"));
}

TEST_F(MsiRegistrationInstall, UninstallRemovesEverythingTheInstallRegistered) {
    ASSERT_EQ(install({{"ALLUSERS", "1"}}), 0u);
    ASSERT_EQ(install({{"ALLUSERS", "1"}, {"REMOVE", "ALL"}}), 0u);
    // Windows leaves none of the probe's keys (msi_registration_machine_x86_removed_state.expected
    // is empty); check the roots the install wrote.
    for (const char* k : {"SOFTWARE\\Classes\\Installer\\Products\\891448AE8C6B83E4A9ABB67A2E623E83",
                          "SOFTWARE\\Classes\\Installer\\Features\\891448AE8C6B83E4A9ABB67A2E623E83",
                          "SOFTWARE\\Classes\\Installer\\UpgradeCodes\\B8F9B566DF590EE45981A788BA56F9F5",
                          "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Products\\891448AE8C6B83E4A9ABB67A2E623E83",
                          "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Installer\\UserData\\S-1-5-18\\Components\\F0B1735E307C4624D9A908D0AEA37EDA",
                          "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{EA844198-B6C8-4E38-9ABA-6BA7E226E338}",
                          "SOFTWARE\\Classes\\WOW6432Node\\CLSID\\{499C6982-7157-444B-B54D-23419DCF7EAB}",
                          "SOFTWARE\\Classes\\Vitreo.Probe.1", "SOFTWARE\\Classes\\.vrp"}) {
        auto h = reg.open_key(capsule::HKEY_LOCAL_MACHINE, wide(k), false);
        EXPECT_TRUE(h.is_err()) << k;
        if (h.is_ok()) reg.close_key(h.value());
    }
}


// ---- MsiShortcutProperty, run by the engine ------------------------------------------------------

/// Installs tests/winapi/msi/fixtures/shortcut_property_strings.msi
/// (tools/oracle_probe/msi_shortcut_property.ps1 -Strings) per-user and
/// compares what CreateShortcuts did with what Windows did for the same
/// package (msi_shortcut_property_strings.expected): the 1946 warnings, in
/// order and with the same arguments, and every property each .lnk's
/// property store carries.
class MsiShortcutPropertyInstall : public ::testing::Test {
protected:
    void SetUp() override {
        root = std::filesystem::temp_directory_path() / ("vitreo_msi_sp_" + std::to_string(::getpid()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / "src" / "LAData" / "VitreoSP");
        std::filesystem::copy_file(std::filesystem::path(__FILE__).parent_path() / "fixtures" /
                                       "shortcut_property_strings.msi",
                                   root / "src" / "sp.msi");
        std::ofstream(root / "src" / "LAData" / "VitreoSP" / "main.exe", std::ios::binary)
            << "vitreo shortcut property probe payload\r\n";
        ASSERT_TRUE(vfs.mount_drive('C', std::make_unique<capsule::HostFileSystem>(root.string())).is_ok());
        mgr.set_vfs(&vfs);
        mgr.set_registry(&reg);
        mgr.set_user_sid(wide(kUserSid));
        handlers = msi_handlers(mgr);
        msi::install_registration_state_provider();
        services.vfs = &vfs;
        services.registry = &reg;
        services.manager = &mgr;
        services.guest_64bit = true;
    }
    void TearDown() override { std::filesystem::remove_all(root); }

    u32 install() {
        std::ifstream in(root / "src" / "sp.msi", std::ios::binary);
        std::vector<u8> bytes((std::istreambuf_iterator<char>(in)), {});
        auto pkg = msi::Package::open(bytes);
        if (!pkg) return 1620;
        msi::Engine::Options o;
        o.package_path = "C:\\src\\sp.msi";
        o.ui_level = 2;
        o.server = true;
        o.allow_system_changes = true;
        o.log_file = "C:\\install.log";
        o.log_mode = 0xFFFFFFFFu;
        u32 error = 0;
        auto engine = msi::Engine::open(mgr, std::make_shared<msi::Package>(std::move(*pkg)), o, services, error);
        if (!engine) return error;
        return engine->run_top_level("INSTALL");
    }

    /// The property-store values of a .lnk ("fmtid-bytes-hex/pid value-hex",
    /// sorted), skipping the two storages the Windows shell link object adds
    /// on its own when it saves ({46588AE2-...} the creator SID,
    /// {446D16B1-...} 104), which are not MsiShortcutProperty rows.
    static std::vector<std::string> store_values(const std::vector<u8>& b) {
        auto u32at = [&](usize i) {
            return static_cast<u32>(b[i]) | (static_cast<u32>(b[i + 1]) << 8) | (static_cast<u32>(b[i + 2]) << 16) |
                   (static_cast<u32>(b[i + 3]) << 24);
        };
        auto hex = [&](usize from, usize to) {
            std::string h;
            char t[3];
            for (usize i = from; i < to; ++i) {
                std::snprintf(t, sizeof t, "%02x", b[i]);
                h += t;
            }
            return h;
        };
        std::vector<std::string> out;
        usize sig = std::string::npos;
        for (usize i = 4; i + 4 <= b.size(); ++i)
            if (u32at(i) == 0xA0000009u) { sig = i; break; }
        if (sig == std::string::npos) return out;
        const usize end = sig - 4 + u32at(sig - 4);
        for (usize st = sig + 4; st + 4 <= end && u32at(st) != 0; st += u32at(st)) {
            const std::string fmtid = hex(st + 8, st + 24);
            if (fmtid == "e28a5846bc4c3843bbfc139326986dce" || fmtid == "b1166d44ad8d7048a748402ea43d788c") continue;
            for (usize v = st + 24; v + 4 <= st + u32at(st) && u32at(v) != 0; v += u32at(v))
                out.push_back(fmtid + "/" + std::to_string(u32at(v + 4)) + " " + hex(v + 9, v + u32at(v)));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::filesystem::path root;
    capsule::Vfs vfs;
    capsule::Registry reg;
    MsiManager mgr;
    msi::HandlerMap handlers;
    msi::Services services;
};

TEST_F(MsiShortcutPropertyInstall, CanonicalNamesKeyStringsAndFailuresMatchWindows) {
    ASSERT_EQ(install(), 0u);  // Windows: exit 0, the 1946 warnings do not stop it
    std::map<std::string, std::vector<u8>> windows_links;
    std::vector<std::string> windows_warnings;
    for (const std::string& l : read_lines(kDir + "/msi_shortcut_property_strings.expected")) {
        if (l.rfind("lnk ", 0) == 0) {
            const usize sp = l.find(' ', 4);
            const std::string name = l.substr(4, sp - 4), hex = l.substr(sp + 1);
            ASSERT_NE(hex, "<absent>") << name;
            std::vector<u8> bytes;
            for (usize i = 0; i + 1 < hex.size(); i += 2) bytes.push_back(static_cast<u8>(std::stoul(hex.substr(i, 2), nullptr, 16)));
            windows_links[name] = bytes;
        } else if (l.rfind("Note: 1: 1946 2: ", 0) == 0) {
            // "Note: 1: 1946 2: <key> 3: <file> 4: <code> "
            const usize f3 = l.find(" 3: "), f4 = l.find(" 4: ");
            windows_warnings.push_back(l.substr(17, f3 - 17) + ", " + l.substr(f3 + 4, f4 - f3 - 4) + ", " +
                                       l.substr(f4 + 4, l.find_last_not_of(' ') - f4 - 3));
        }
    }
    ASSERT_EQ(windows_links.size(), 19u);
    ASSERT_EQ(windows_warnings.size(), 4u);

    std::vector<std::string> vitreo_warnings;
    for (const std::string& l : read_lines((root / "install.log").string())) {
        const usize at = l.find("Warning: ");
        if (at == std::string::npos) continue;
        const usize args = l.find("Arguments: ", at);
        ASSERT_NE(args, std::string::npos) << l;
        ASSERT_NE(l.find("1946", at), std::string::npos) << l;
        vitreo_warnings.push_back(l.substr(args + 11));
    }
    EXPECT_EQ(vitreo_warnings, windows_warnings);

    std::filesystem::path menu;
    for (const auto& e : std::filesystem::recursive_directory_iterator(root))
        if (e.is_directory() && e.path().filename() == "VitreoSP" && std::filesystem::exists(e.path() / "Canon.lnk"))
            menu = e.path();
    ASSERT_FALSE(menu.empty()) << "no shortcuts written";
    for (const auto& [name, windows] : windows_links) {
        std::ifstream in(menu / (name + ".lnk"), std::ios::binary);
        ASSERT_TRUE(in.good()) << name;
        const std::vector<u8> vitreo((std::istreambuf_iterator<char>(in)), {});
        EXPECT_EQ(store_values(vitreo), store_values(windows)) << name;
    }
}

}  // namespace
}  // namespace vitreo::winapi::msi_test

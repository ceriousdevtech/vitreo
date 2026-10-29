/// @file test_mscoree_shim.cpp
/// @brief mscoree.dll, the OS .NET runtime shim: export table, locating
///        mscoreei.dll, and the bind/forward state machine.
///
/// Expected values come from the in-box DLLs' export tables (generated into
/// mscoree_exports.inc), the SDK prototypes (mscoree.h, metahost.h,
/// strongname.h) for argument sizes, and the oracle
/// (tests/oracle_data/cor_shim_locate_probe{32,64}.expected).

#include "winapi/system/mscoree/mscoree_shim.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi::mscoree;

namespace {

const ShimExport* find(bool is64, const char* name) {
    for (const auto& e : shim_exports(is64))
        if (std::strcmp(e.name, name) == 0) return &e;
    return nullptr;
}
const ShimExport* find_ordinal(bool is64, u16 ordinal) {
    for (const auto& e : shim_exports(is64))
        if (e.ordinal == ordinal) return &e;
    return nullptr;
}

TEST(MscoreeExports, BothBitnessesHave125ExportsFrom17To142) {
    for (bool is64 : {false, true}) {
        const auto t = shim_exports(is64);
        EXPECT_EQ(t.size(), 125u);
        EXPECT_EQ(t.front().ordinal, 17);
        EXPECT_EQ(t.back().ordinal, 142);
        EXPECT_EQ(find_ordinal(is64, 141), nullptr) << "ordinal 141 is not exported";
    }
}

TEST(MscoreeExports, CorExeMainOrdinalsDifferByBitness) {
    // SysWOW64: 137 _CorExeMain2, 138 _CorExeMain; System32: the reverse.
    EXPECT_EQ(find(false, "_CorExeMain")->ordinal, 138);
    EXPECT_EQ(find(false, "_CorExeMain2")->ordinal, 137);
    EXPECT_EQ(find(true, "_CorExeMain")->ordinal, 137);
    EXPECT_EQ(find(true, "_CorExeMain2")->ordinal, 138);
    EXPECT_EQ(find(true, "CLRCreateInstance")->ordinal, 38);
    EXPECT_EQ(find(true, "_CorDllMain")->ordinal, 136);
}

TEST(MscoreeExports, X86ArgumentBytesMatchSdkPrototypes) {
    // Bytes = 4 per DWORD/pointer argument, 8 per 64-bit argument.
    const std::map<std::string, u16> sdk = {
        {"CLRCreateInstance", 12},          // (REFCLSID, REFIID, LPVOID*)          metahost.h
        {"CorBindToRuntimeEx", 24},         // (LPCWSTR, LPCWSTR, DWORD, REFCLSID, REFIID, LPVOID*)
        {"CorBindToRuntime", 20},           // (LPCWSTR, LPCWSTR, REFCLSID, REFIID, LPVOID*)
        {"CorBindToCurrentRuntime", 16},    // (LPCWSTR, REFCLSID, REFIID, LPVOID*)
        {"CorBindToRuntimeHost", 32},       // 8 arguments
        {"GetCORVersion", 12},              // (LPWSTR, DWORD, DWORD*)
        {"GetCORSystemDirectory", 12},      // (LPWSTR, DWORD, DWORD*)
        {"GetRequestedRuntimeInfo", 44},    // 11 arguments
        {"GetFileVersion", 16},             // (LPCWSTR, LPWSTR, DWORD, DWORD*)
        {"LoadLibraryShim", 16},            // (LPCWSTR, LPCWSTR, LPVOID, HMODULE*)
        {"GetRealProcAddress", 8},          // (LPCSTR, VOID**)
        {"ClrCreateManagedInstance", 12},   // (LPCWSTR, REFIID, void**)
        {"LockClrVersion", 12},             // (FLockClrVersionCallback, x2 pointers)
        {"CorExitProcess", 4},              // (int)
        {"_CorDllMain", 12},                // (HINSTANCE, DWORD, LPVOID)
        {"_CorExeMain", 0},                 // (void)
        {"_CorExeMain2", 20},               // (PBYTE, DWORD, LPWSTR, LPWSTR, LPWSTR)
        {"_CorValidateImage", 8},           // (PVOID*, LPCWSTR)
        {"_CorImageUnloading", 4},          // (PVOID)
        {"StrongNameErrorInfo", 0},         // (void)                               strongname.h
        {"StrongNameFreeBuffer", 4},        // (BYTE*)
        {"StrongNameSignatureVerificationEx", 12},  // (LPCWSTR, BOOLEAN, BOOLEAN*)
        {"StrongNameTokenFromAssembly", 12},        // (LPCWSTR, BYTE**, ULONG*)
        {"DllGetClassObject", 12},          // (REFCLSID, REFIID, LPVOID*)
        {"DllCanUnloadNow", 0},
    };
    for (const auto& [name, bytes] : sdk) {
        const ShimExport* e = find(false, name.c_str());
        ASSERT_NE(e, nullptr) << name;
        EXPECT_EQ(e->x86_stdcall_bytes, bytes) << name;
    }
}

TEST(MscoreeExports, RolesFollowTheInBoxShim) {
    EXPECT_EQ(export_role(*find(true, "CLRCreateInstance")), ExportRole::ForwardByName);
    EXPECT_EQ(export_role(*find(true, "GetCLRMetaHost")), ExportRole::ForwardByName);
    EXPECT_EQ(export_role(*find(true, "_CorImageUnloading")), ExportRole::ImageUnloading);
    EXPECT_EQ(export_role(*find(true, "_CorValidateImage")), ExportRole::ValidateImage);
    for (bool is64 : {false, true}) {
        EXPECT_EQ(export_role(*find_ordinal(is64, 24)), ExportRole::ForwardByOrdinal);
        EXPECT_EQ(export_role(*find_ordinal(is64, 142)), ExportRole::GetVersionExW);
    }
}

class FakeHost : public ShimHost {
public:
    bool is64 = true;
    std::map<std::u16string, std::u16string> env;
    std::vector<std::u16string> builds{u"30319"};
    std::map<std::u16string, u64> files;   // path -> module
    std::map<std::string, u64> procs;      // name -> address
    std::map<u16, u64> ordinals;
    std::vector<std::pair<u64, std::vector<u64>>> calls;
    std::vector<std::u16string> loads;
    std::vector<u64> freed;

    bool is_64bit() const override { return is64; }
    std::optional<std::u16string> environment_variable(std::u16string_view name) override {
        auto it = env.find(std::u16string(name));
        if (it == env.end()) return std::nullopt;
        return it->second;
    }
    std::u16string windows_directory() override { return u"C:\\Windows"; }
    std::vector<std::u16string> policy_v4_builds() override { return builds; }
    u64 load_library(const std::u16string& path) override {
        loads.push_back(path);
        auto it = files.find(path);
        return it == files.end() ? 0 : it->second;
    }
    void free_library(u64 module) override { freed.push_back(module); }
    u64 get_proc_address(u64, const char* name) override {
        auto it = procs.find(name);
        return it == procs.end() ? 0 : it->second;
    }
    u64 get_proc_address_ordinal(u64, u16 ordinal) override {
        auto it = ordinals.find(ordinal);
        return it == ordinals.end() ? 0 : it->second;
    }
    u64 call(u64 fn, const std::vector<u64>& args) override {
        calls.emplace_back(fn, args);
        return 0;
    }
    u64 shim_instance() override { return 0x7000; }
};

TEST(MscoreeLocate, DefaultIsTheFrameworkDirOfTheProcessBitness) {
    // cor_shim_locate_probe "default": %windir%\Microsoft.NET\Framework64\
    // v4.0.30319\mscoreei.dll (64-bit), ...\Framework\... (32-bit).
    FakeHost h;
    EXPECT_EQ(implementation_path(h), u"C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\mscoreei.dll");
    h.is64 = false;
    EXPECT_EQ(implementation_path(h), u"C:\\Windows\\Microsoft.NET\\Framework\\v4.0.30319\\mscoreei.dll");
}

TEST(MscoreeLocate, InstallRootOverrideAndPolicyBuild) {
    // "root-30319": COMPlus_InstallRoot=<dir>\rootA\ loads rootA\v4.0.30319.
    FakeHost h;
    h.env[u"COMPlus_InstallRoot"] = u"D:\\rootA\\";
    EXPECT_EQ(implementation_path(h), u"D:\\rootA\\v4.0.30319\\mscoreei.dll");
    // "root-99999": the directory is not enumerated; the build comes from
    // Policy\v4.0, whose value names are build numbers (highest wins).
    h.builds = {u"30319", u"not-a-build", u"31000"};
    EXPECT_EQ(implementation_path(h), u"D:\\rootA\\v4.0.31000\\mscoreei.dll");
    h.builds.clear();
    EXPECT_EQ(implementation_path(h), std::nullopt);
}

const ShimExport& exp(bool is64, const char* name) { return *find(is64, name); }

TEST(MscoreeShim, BindsHandshakesAndForwardsByName) {
    FakeHost h;
    const std::u16string path = u"C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\mscoreei.dll";
    h.files[path] = 0x10000;
    h.procs = {{"RegisterShimImplCallback", 0x1100}, {"CLRCreateInstance", 0x2200}};
    h.ordinals[24] = 0x2400;
    MscoreeShim shim(0xC1, 0xC2);
    EXPECT_EQ(shim.forward_target(h, exp(true, "CLRCreateInstance")), 0x2200u);
    EXPECT_EQ(shim.state(), MscoreeShim::State::Bound);
    ASSERT_EQ(h.calls.size(), 1u);
    EXPECT_EQ(h.calls[0].first, 0x1100u);
    EXPECT_EQ(h.calls[0].second, (std::vector<u64>{0xC1, 0xC2}));
    EXPECT_EQ(shim.forward_target(h, *find_ordinal(true, 24)), 0x2400u);
    // An export mscoreei lacks (GetCLRMetaHost in the 4.0 mscoreei) fails.
    EXPECT_EQ(shim.forward_target(h, exp(true, "GetCLRMetaHost")), 0u);
    EXPECT_EQ(h.loads.size(), 1u) << "binds once";
    // After mscoreei's cleanup callback every forward fails.
    shim.on_cleanup();
    EXPECT_EQ(shim.forward_target(h, exp(true, "CLRCreateInstance")), 0u);
    EXPECT_EQ(h.loads.size(), 1u);
}

TEST(MscoreeShim, CleanupOnlyRegistrationAndShellInstance) {
    FakeHost h;
    h.files[u"C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\mscoreei.dll"] = 0x10000;
    h.procs = {{"RegisterShimImplCleanupCallback", 0x1200}, {"SetShellShimInstance", 0x1300},
               {"_CorExeMain", 0x3300}};
    MscoreeShim shim(0xC1, 0xC2);
    EXPECT_EQ(shim.forward_target(h, exp(true, "_CorExeMain")), 0x3300u);
    ASSERT_EQ(h.calls.size(), 2u);
    EXPECT_EQ(h.calls[0].first, 0x1300u);
    EXPECT_EQ(h.calls[0].second, (std::vector<u64>{0x7000}));
    EXPECT_EQ(h.calls[1].first, 0x1200u);
    EXPECT_EQ(h.calls[1].second, (std::vector<u64>{0xC1}));
}

TEST(MscoreeShim, WithoutARegistrationExportTheImplementationIsFreed) {
    FakeHost h;
    h.files[u"C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\mscoreei.dll"] = 0x10000;
    h.procs = {{"_CorExeMain", 0x3300}};
    MscoreeShim shim(0xC1, 0xC2);
    EXPECT_EQ(shim.forward_target(h, exp(true, "_CorExeMain")), 0u);
    EXPECT_EQ(shim.state(), MscoreeShim::State::Failed);
    EXPECT_EQ(h.freed, (std::vector<u64>{0x10000}));
    EXPECT_EQ(shim.forward_target(h, exp(true, "_CorExeMain")), 0u);
    EXPECT_EQ(h.loads.size(), 1u) << "a failed bind is not retried";
}

TEST(MscoreeShim, NoFrameworkInstalledFails) {
    FakeHost h;  // no files: LoadLibrary fails
    MscoreeShim shim(0xC1, 0xC2);
    EXPECT_EQ(shim.forward_target(h, exp(false, "_CorExeMain")), 0u);
    EXPECT_EQ(shim.state(), MscoreeShim::State::Failed);
    FakeHost none;
    none.builds.clear();  // Policy\v4.0 absent: nothing is loaded
    MscoreeShim shim2(0xC1, 0xC2);
    EXPECT_EQ(shim2.forward_target(none, exp(false, "_CorExeMain")), 0u);
    EXPECT_TRUE(none.loads.empty());
    EXPECT_EQ(CLR_E_SHIM_RUNTIMEEXPORT, 0x80131701u);
}

}  // namespace

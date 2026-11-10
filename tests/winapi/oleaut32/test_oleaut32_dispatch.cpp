// SPDX-License-Identifier: LGPL-2.1-only
//
// Automation call engine (oleaut32_dispatch.cpp) facts that do not need a
// guest CPU:
//   * its constants equal the Windows SDK definitions
//     (tests/oracle_data/oleaut32_dispatch_constants.expected), and
//   * the [defaultvalue] decoder returns what native ITypeInfo::GetFuncDesc
//     reports for the midl-built fixtures (`def` rows of
//     tests/oracle_data/typeinfo_invoke_probe.{x86,x64}.expected).
// The calls themselves are covered end to end by the CTest
// vitreo_typeinfo_invoke_probe, which runs the same probe under Vitreo.

#include <gtest/gtest.h>
#include <vitreo/common.hpp>
#include <winapi/com/oleaut32/oleaut32_dispatch_constants.hpp>
#include <winapi/com/oleaut32/typelib.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

std::filesystem::path repo_root() {
    return std::filesystem::absolute(__FILE__).parent_path().parent_path()
        .parent_path().parent_path();
}

std::vector<std::string> lines_of(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) lines.push_back(line);
    return lines;
}

std::string format_default(const ParamDefault& value) {
    char buf[96];
    switch (value.vt) {
    case 2: std::snprintf(buf, sizeof buf, "2:%d", static_cast<i16>(value.bits)); break;
    case 3: std::snprintf(buf, sizeof buf, "3:%d", static_cast<i32>(value.bits)); break;
    case 4: {
        float f; const u32 bits = static_cast<u32>(value.bits);
        std::memcpy(&f, &bits, 4);
        std::snprintf(buf, sizeof buf, "4:%.9g/%08x", f, bits);
        break;
    }
    case 5: {
        double d; std::memcpy(&d, &value.bits, 8);
        std::snprintf(buf, sizeof buf, "5:%.17g/%016llx", d,
                      static_cast<unsigned long long>(value.bits));
        break;
    }
    case 8: std::snprintf(buf, sizeof buf, "8:\"%s\"", value.text.c_str()); break;
    case 11: std::snprintf(buf, sizeof buf, "b:%d", static_cast<i16>(value.bits)); break;
    case 17: std::snprintf(buf, sizeof buf, "11:%u", static_cast<unsigned>(value.bits & 0xFF)); break;
    case 19: std::snprintf(buf, sizeof buf, "13:%u", static_cast<u32>(value.bits)); break;
    default: std::snprintf(buf, sizeof buf, "%x:?", value.vt); break;
    }
    return buf;
}

} // namespace

TEST(OleAut32Dispatch, ConstantsMatchWindowsSdk) {
    using namespace oleaut32_dispatch;
    const std::map<std::string, long long> ours = {
        {"VT_CY", VT_CY}, {"VT_DATE", VT_DATE}, {"VT_ERROR", VT_ERROR},
        {"VT_DECIMAL", VT_DECIMAL}, {"VT_I8", VT_I8}, {"VT_UI8", VT_UI8},
        {"VT_INT", VT_INT}, {"VT_UINT", VT_UINT}, {"VT_VOID", VT_VOID},
        {"VT_HRESULT", VT_HRESULT}, {"VT_PTR", VT_PTR},
        {"VT_USERDEFINED", VT_USERDEFINED}, {"VT_RECORD", VT_RECORD},
        {"VT_INT_PTR", VT_INT_PTR}, {"VT_VECTOR", VT_VECTOR}, {"VT_ARRAY", VT_ARRAY},
        {"VT_BYREF", VT_BYREF}, {"VT_TYPEMASK", VT_TYPEMASK},
        {"DISP_E_MEMBERNOTFOUND", DISP_E_MEMBERNOTFOUND},
        {"DISP_E_PARAMNOTFOUND", DISP_E_PARAMNOTFOUND},
        {"DISP_E_BADVARTYPE", DISP_E_BADVARTYPE},
        {"DISP_E_EXCEPTION", DISP_E_EXCEPTION}, {"DISP_E_OVERFLOW", DISP_E_OVERFLOW},
        {"DISP_E_BADPARAMCOUNT", DISP_E_BADPARAMCOUNT},
        {"DISP_E_PARAMNOTOPTIONAL", DISP_E_PARAMNOTOPTIONAL},
        {"DISP_E_BADCALLEE", DISP_E_BADCALLEE},
        {"DISP_E_NOTACOLLECTION", DISP_E_NOTACOLLECTION},
        {"DISPATCH_METHOD", DISPATCH_METHOD}, {"DISPATCH_PROPERTYGET", DISPATCH_PROPERTYGET},
        {"DISPATCH_PROPERTYPUT", DISPATCH_PROPERTYPUT},
        {"DISPATCH_PROPERTYPUTREF", DISPATCH_PROPERTYPUTREF},
        {"DISPID_VALUE", DISPID_VALUE}, {"DISPID_PROPERTYPUT", DISPID_PROPERTYPUT},
        {"CC_FASTCALL", CC_FASTCALL}, {"CC_CDECL", CC_CDECL}, {"CC_STDCALL", CC_STDCALL},
        {"FUNC_VIRTUAL", FUNC_VIRTUAL}, {"FUNC_PUREVIRTUAL", FUNC_PUREVIRTUAL},
        {"FUNC_NONVIRTUAL", FUNC_NONVIRTUAL}, {"FUNC_STATIC", FUNC_STATIC},
        {"FUNC_DISPATCH", FUNC_DISPATCH}, {"INVOKE_FUNC", INVOKE_FUNC},
        {"INVOKE_PROPERTYGET", INVOKE_PROPERTYGET}, {"INVOKE_PROPERTYPUT", INVOKE_PROPERTYPUT},
        {"INVOKE_PROPERTYPUTREF", INVOKE_PROPERTYPUTREF},
        {"PARAMFLAG_FIN", PARAMFLAG_FIN}, {"PARAMFLAG_FOUT", PARAMFLAG_FOUT},
        {"PARAMFLAG_FLCID", PARAMFLAG_FLCID}, {"PARAMFLAG_FRETVAL", PARAMFLAG_FRETVAL},
        {"PARAMFLAG_FOPT", PARAMFLAG_FOPT}, {"PARAMFLAG_FHASDEFAULT", PARAMFLAG_FHASDEFAULT},
    };
    std::map<std::string, long long> sdk;
    for (const std::string& line :
         lines_of(repo_root() / "tests/oracle_data/oleaut32_dispatch_constants.expected")) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string name;
        long long value = 0;
        row >> name >> value;
        sdk[name] = value;
    }
    ASSERT_EQ(sdk.size(), ours.size());
    for (const auto& [name, value] : ours) {
        ASSERT_TRUE(sdk.count(name)) << name;
        EXPECT_EQ(sdk[name], value) << name;
    }
    EXPECT_EQ(kExcepInfoX86.size, 32u);   // probe: abi excepinfo=32
    EXPECT_EQ(kExcepInfoX64.size, 64u);   // probe: abi excepinfo=64
}

TEST(OleAut32Dispatch, DefaultValuesMatchNativeFuncDesc) {
    for (const char* arch : {"x86", "x64"}) {
        const auto tlb = repo_root() / "tests/fixtures/typelib-invoke" /
            (std::string(arch) == "x86" ? "invoke32.tlb" : "invoke64.tlb");
        std::ifstream input(tlb, std::ios::binary);
        const std::vector<u8> image{std::istreambuf_iterator<char>(input), {}};
        ASSERT_FALSE(image.empty()) << tlb;
        const TypeLibParseResult parsed = parse_typelib(image);
        ASSERT_TRUE(parsed.ok()) << arch;

        const FuncSummary* defs = nullptr;
        for (const TypeInfoSummary& type : parsed.typeinfos)
            for (const FuncSummary& fn : type.funcs)
                if (type.name == "IInvokeDefaults" && fn.name == "Defs") defs = &fn;
        ASSERT_NE(defs, nullptr) << arch;

        std::vector<std::string> native;
        for (const std::string& line : lines_of(
                 repo_root() / "tests/oracle_data" /
                 (std::string("typeinfo_invoke_probe.") + arch + ".expected"))) {
            if (line.rfind("def param=", 0) != 0) continue;
            native.push_back(line.substr(line.find("value=") + 6));
        }
        ASSERT_EQ(native.size(), defs->c_params) << arch;
        for (usize i = 0; i < native.size(); ++i) {
            ASSERT_TRUE(defs->param_defaults[i].has_value()) << arch << " param " << i;
            EXPECT_EQ(format_default(*defs->param_defaults[i]), native[i])
                << arch << " param " << i;
        }
    }
}

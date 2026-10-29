// SPDX-License-Identifier: LGPL-2.1-only
// Vitreo - Windows Runtime Engine for macOS ARM64
//
// IXMLDOMDocument2 / IXMLDOMDocument3 on every DOMDocument coclass, x86 and
// x64. Every expected value is copied from the Windows 11 oracle run
// tests/oracle_data/msxml_document2_probe{,.x86}.expected
// (tools/oracle_probe/msxml_document2_probe.c); x86 and x64 produced identical
// output, so one table serves both bitnesses.

#include <gtest/gtest.h>

#include <winapi/com/msxml/msxml_com.hpp>
#include <winapi/core/marshaler.hpp>
#include <winapi/core/thunk_registry.hpp>
#include <core/memory/guest_memory.hpp>
#include <core/cpu/state/cpu_state.hpp>

#include <cstring>
#include <string>
#include <variant>
#include <vector>

using namespace vitreo;
using namespace vitreo::winapi;

namespace {

constexpr GuestAddr kStack    = 0x30000000;
constexpr GuestAddr kVarArgs  = 0x30002000;  // by-value VARIANTs on x64
constexpr GuestAddr kOut      = 0x30003000;
constexpr GuestAddr kVarOut   = 0x30003100;
constexpr GuestAddr kArena    = 0x31000000;
constexpr u32       kArenaSize = 0x00100000;

constexpr u32 S_OK_ = 0, S_FALSE_ = 1;
constexpr u32 E_NOTIMPL_ = 0x80004001u, E_NOINTERFACE_ = 0x80004002u;
constexpr u32 E_POINTER_ = 0x80004003u, E_FAIL_ = 0x80004005u;
constexpr u32 E_INVALIDARG_ = 0x80070057u, CLASSNOTREG_ = 0x80040154u;
constexpr u32 TYPEMISMATCH_ = 0x80020005u;

constexpr u16 VT_EMPTY = 0, VT_NULL = 1, VT_I4 = 3, VT_BSTR = 8, VT_BOOL = 11;
constexpr u16 kPoison = 0x7777;

#define MSXML_GUID(d1) Guid{d1, 0x7B36u, 0x11d2u, {0xB2,0x0E,0x00,0xC0,0x4F,0x98,0x3E,0x60}}
const Guid kIidDoc  = MSXML_GUID(0x2933BF81u);
const Guid kIidDoc2 = MSXML_GUID(0x2933BF95u);
const Guid kIidDoc3 = MSXML_GUID(0x2933BF96u);
const Guid kIidSchemaCollection{0x373984c8u, 0xb845u, 0x449bu,
                                {0x91,0xe7,0x45,0xac,0x83,0x03,0x6a,0xde}};

const Guid kClsidDomDocument   = MSXML_GUID(0x2933BF90u);
const Guid kClsidFtDomDocument = MSXML_GUID(0x2933BF91u);
const Guid kClsidDomDocument2{0xf6d90f11u, 0x9c73u, 0x11d3u, {0xb3,0x2e,0x00,0xc0,0x4f,0x99,0x0b,0xb4}};
const Guid kClsidFtDomDocument2{0xf6d90f12u, 0x9c73u, 0x11d3u, {0xb3,0x2e,0x00,0xc0,0x4f,0x99,0x0b,0xb4}};
const Guid kClsid26{0xf5078f1bu, 0xc551u, 0x11d3u, {0x89,0xb9,0x00,0x00,0xf8,0x1f,0xe2,0x21}};
const Guid kClsidFt26{0xf5078f1cu, 0xc551u, 0x11d3u, {0x89,0xb9,0x00,0x00,0xf8,0x1f,0xe2,0x21}};
const Guid kClsid30{0xf5078f32u, 0xc551u, 0x11d3u, {0x89,0xb9,0x00,0x00,0xf8,0x1f,0xe2,0x21}};
const Guid kClsidFt30{0xf5078f33u, 0xc551u, 0x11d3u, {0x89,0xb9,0x00,0x00,0xf8,0x1f,0xe2,0x21}};
const Guid kClsid40{0x88d969c0u, 0xf192u, 0x11d4u, {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};
const Guid kClsidFt40{0x88d969c1u, 0xf192u, 0x11d4u, {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};
const Guid kClsid60{0x88d96a05u, 0xf192u, 0x11d4u, {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};
const Guid kClsidFt60{0x88d96a06u, 0xf192u, 0x11d4u, {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};

// Vtable slots (msxml6.h).
enum Slot : u32 {
    QI = 0, Release = 2,
    NodeName = 7, ParentNode = 11, FirstChild = 13, Attributes = 17,
    AppendChild = 21, OwnerDocument = 23, Xml = 34, SelectNodes = 36,
    SelectSingleNode = 37, NamespaceUri = 39,
    DocumentElement = 45, ParseErrorSlot = 60, GetAsync = 62, LoadXml = 65,
    GetValidateOnParse = 67, PutValidateOnParse = 68,
    GetResolveExternals = 69, PutResolveExternals = 70,
    GetNamespaces = 76, GetSchemas = 77, PutrefSchemas = 78, Validate = 79,
    SetProperty = 80, GetProperty = 81, ValidateNode = 82, ImportNode = 83,
    ListLength = 8, MapItem = 10, AttrValue = 44,
    ErrCode = 7, ErrReason = 9, ErrSrcText = 10, ErrLine = 11, ErrLinePos = 12,
    ErrFilePos = 13,
    ScAdd = 7, ScGet = 8, ScRemove = 9, ScLength = 10, ScNamespaceUri = 11,
    ScAddCollection = 12, ScNewEnum = 13,
};

struct VarArg { u16 vt; u64 bits; };
using Arg = std::variant<u64, VarArg>;

class MsxmlDocument2Test : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        ASSERT_TRUE(memory_.map_region(MsxmlManager::MSXML_SCRATCH_BASE,
                                       MsxmlManager::MSXML_SCRATCH_SIZE,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(kStack - 0x1000, 0x5000,
                                       MemoryProtection::ReadWrite).is_ok());
        ASSERT_TRUE(memory_.map_region(kArena, kArenaSize,
                                       MemoryProtection::ReadWrite).is_ok());
        DllExports exports;
        register_msxml_thunks(exports, mgr_);
        ASSERT_TRUE(thunks_.register_dll("msxml6.dll", std::move(exports)).is_ok());
        mgr_.set_memory(&memory_);
        mgr_.set_thunk_registry(&thunks_);
        mgr_.set_scratch_base(MsxmlManager::MSXML_SCRATCH_BASE);
        mgr_.set_bstr_allocator([this](Marshaler& m, const std::vector<u16>& units) {
            return alloc_bstr(m, units);
        });
        cpu_.mode = x64() ? cpu::CpuMode::Long : cpu::CpuMode::Protected;
        cpu_.gpr.rsp = kStack;
    }

    [[nodiscard]] bool x64() const { return GetParam(); }

    GuestAddr alloc_bstr(Marshaler& m, const std::vector<u16>& units) {
        const GuestAddr data = arena_next_ + 4;
        (void)m.write_u32(arena_next_, static_cast<u32>(units.size() * 2));
        for (usize i = 0; i < units.size(); ++i) {
            (void)m.write_u16(data + i * 2, units[i]);
        }
        (void)m.write_u16(data + units.size() * 2, 0);
        arena_next_ = (data + units.size() * 2 + 2 + 7) & ~GuestAddr{7};
        return data;
    }

    GuestAddr bstr(const std::string& ascii) {
        std::vector<u16> units(ascii.begin(), ascii.end());
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        return alloc_bstr(m, units);
    }

    std::string read_bstr(GuestAddr p) {
        if (p == 0) return "(null)";
        const u32 bytes = memory_.read_u32(p - 4).value();
        std::string out;
        for (u32 i = 0; i < bytes / 2; ++i) {
            out.push_back(static_cast<char>(memory_.read_u16(p + i * 2).value()));
        }
        return out;
    }

    GuestAddr read_ptr(GuestAddr at) {
        return x64() ? memory_.read_u64(at).value() : memory_.read_u32(at).value();
    }
    void write_ptr(GuestAddr at, GuestAddr v) {
        if (x64()) (void)memory_.write_u64(at, v);
        else (void)memory_.write_u32(at, static_cast<u32>(v));
    }

    /// Call @p slot of @p iface on @p self with the platform ABI: x64 puts
    /// the first four arguments in registers and passes a VARIANT by hidden
    /// pointer; x86 stdcall puts everything on the stack, VARIANTs inline.
    u32 call(MsxmlIface iface, u32 slot, GuestAddr self, std::vector<Arg> args = {}) {
        std::vector<u64> words{self};
        GuestAddr var_at = kVarArgs;
        std::vector<u32> dwords{static_cast<u32>(self)};
        for (const auto& a : args) {
            if (const auto* scalar = std::get_if<u64>(&a)) {
                words.push_back(*scalar);
                dwords.push_back(static_cast<u32>(*scalar));
            } else {
                const auto& v = std::get<VarArg>(a);
                (void)memory_.write_u64(var_at, v.vt);
                (void)memory_.write_u64(var_at + 8, v.bits);
                words.push_back(var_at);
                var_at += 32;
                dwords.push_back(v.vt);
                dwords.push_back(0);
                dwords.push_back(static_cast<u32>(v.bits));
                dwords.push_back(static_cast<u32>(v.bits >> 32));
            }
        }
        if (x64()) {
            EXPECT_LE(words.size(), 4u);
            u64* regs[] = {&cpu_.gpr.rcx, &cpu_.gpr.rdx, &cpu_.gpr.r8, &cpu_.gpr.r9};
            for (usize i = 0; i < words.size() && i < 4; ++i) *regs[i] = words[i];
        } else {
            for (usize i = 0; i < dwords.size(); ++i) {
                (void)memory_.write_u32(kStack + 4 + 4 * i, dwords[i]);
            }
        }
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        return mgr_.dispatch(iface, slot, m);
    }

    u32 create(const Guid& clsid, const Guid& iid, GuestAddr& out) {
        write_ptr(kOut, 0xBAD);
        Marshaler m(memory_);
        m.attach_cpu(cpu_);
        const u32 hr = mgr_.create_document(m, clsid, iid, kOut);
        out = read_ptr(kOut);
        return hr;
    }

    GuestAddr new_doc(const Guid& clsid) {
        GuestAddr doc = 0;
        EXPECT_EQ(create(clsid, kIidDoc2, doc), S_OK_);
        return doc;
    }

    // -- property helpers ---------------------------------------------------
    struct Prop { u32 hr; u16 vt; std::string value; };

    Prop get(GuestAddr doc, const char* name) {
        (void)memory_.write_u16(kVarOut, kPoison);
        (void)memory_.write_u64(kVarOut + 8, 0);
        const u64 name_arg = name ? bstr(name) : 0;
        Prop p;
        p.hr = call(MsxmlIface::Document, GetProperty, doc, {name_arg, u64{kVarOut}});
        p.vt = memory_.read_u16(kVarOut).value();
        if (p.vt == VT_BSTR) p.value = read_bstr(read_ptr(kVarOut + 8));
        else if (p.vt == VT_BOOL) p.value = std::to_string(static_cast<i16>(memory_.read_u16(kVarOut + 8).value()));
        else if (p.vt == VT_I4) p.value = std::to_string(static_cast<i32>(memory_.read_u32(kVarOut + 8).value()));
        return p;
    }
    u32 set_str(GuestAddr doc, const char* name, const char* value) {
        return call(MsxmlIface::Document, SetProperty, doc,
                    {u64{bstr(name)}, VarArg{VT_BSTR, value ? bstr(value) : 0}});
    }
    u32 set_typed(GuestAddr doc, const char* name, u16 vt, i64 value) {
        u64 bits = static_cast<u64>(value);
        if (vt == VT_BOOL) bits = value ? 0xFFFFu : 0u;
        if (vt == VT_I4) bits = static_cast<u32>(value);
        return call(MsxmlIface::Document, SetProperty, doc, {u64{bstr(name)}, VarArg{vt, bits}});
    }

    // -- DOM helpers ----------------------------------------------------------
    struct Load { u32 hr; i16 ok; };
    Load load(GuestAddr doc, const std::string& xml) {
        Load l{};
        (void)memory_.write_u16(kOut + 0x40, 7);
        l.hr = call(MsxmlIface::Document, LoadXml, doc, {u64{bstr(xml)}, u64{kOut + 0x40}});
        l.ok = static_cast<i16>(memory_.read_u16(kOut + 0x40).value());
        return l;
    }
    struct Err { i32 code, line, pos, fpos; std::string reason, src; };
    Err error_of(GuestAddr err_obj) {
        Err e{};
        const auto u32_of = [&](u32 slot) {
            EXPECT_EQ(call(MsxmlIface::ParseError, slot, err_obj, {u64{kOut + 0x60}}), S_OK_);
            return static_cast<i32>(memory_.read_u32(kOut + 0x60).value());
        };
        e.code = u32_of(ErrCode); e.line = u32_of(ErrLine);
        e.pos = u32_of(ErrLinePos); e.fpos = u32_of(ErrFilePos);
        EXPECT_EQ(call(MsxmlIface::ParseError, ErrReason, err_obj, {u64{kOut + 0x60}}), S_OK_);
        e.reason = read_bstr(read_ptr(kOut + 0x60));
        EXPECT_EQ(call(MsxmlIface::ParseError, ErrSrcText, err_obj, {u64{kOut + 0x60}}), S_OK_);
        e.src = read_bstr(read_ptr(kOut + 0x60));
        return e;
    }
    Err doc_error(GuestAddr doc) {
        EXPECT_EQ(call(MsxmlIface::Document, ParseErrorSlot, doc, {u64{kOut + 0x80}}), S_OK_);
        return error_of(read_ptr(kOut + 0x80));
    }
    struct Sel { u32 hr; i32 len; u32 single_hr; std::string single; };
    Sel select(GuestAddr doc, const std::string& expr) {
        Sel s{};
        const GuestAddr q = bstr(expr);
        write_ptr(kOut + 0x20, 0);
        s.hr = call(MsxmlIface::Document, SelectNodes, doc, {u64{q}, u64{kOut + 0x20}});
        s.len = -1;
        if (const GuestAddr list = read_ptr(kOut + 0x20)) {
            EXPECT_EQ(call(MsxmlIface::NodeList, ListLength, list, {u64{kOut + 0x28}}), S_OK_);
            s.len = static_cast<i32>(memory_.read_u32(kOut + 0x28).value());
        }
        write_ptr(kOut + 0x30, 0);
        s.single_hr = call(MsxmlIface::Document, SelectSingleNode, doc, {u64{q}, u64{kOut + 0x30}});
        s.single = "-";
        if (const GuestAddr n = read_ptr(kOut + 0x30)) {
            EXPECT_EQ(call(MsxmlIface::Node, NodeName, n, {u64{kOut + 0x38}}), S_OK_);
            s.single = read_bstr(read_ptr(kOut + 0x38));
        }
        return s;
    }
    std::string node_string(MsxmlIface iface, u32 slot, GuestAddr node) {
        EXPECT_EQ(call(iface, slot, node, {u64{kOut + 0x50}}), S_OK_);
        return read_bstr(read_ptr(kOut + 0x50));
    }
    GuestAddr out_object(MsxmlIface iface, u32 slot, GuestAddr self) {
        write_ptr(kOut + 0x58, 0);
        (void)call(iface, slot, self, {u64{kOut + 0x58}});
        return read_ptr(kOut + 0x58);
    }

    GuestMemory memory_;
    ThunkRegistry thunks_{0x70000000};
    MsxmlManager mgr_;
    cpu::CpuState cpu_{};
    GuestAddr arena_next_ = kArena;
};

const char* kSelectionDoc =
    "<r xmlns='urn:d' xmlns:a='urn:a' xmlns:q='urn:a'><a:x id='1'/><y/>"
    "<q:x id='2'/><z xmlns=''><w/></z></r>";

}  // namespace

INSTANTIATE_TEST_SUITE_P(Bitness, MsxmlDocument2Test, ::testing::Values(false, true),
                         [](const auto& info) { return info.param ? "x64" : "x86"; });

TEST_P(MsxmlDocument2Test, CoCreateServesDocument2OnEveryRegisteredClsid) {
    struct Case { const Guid* clsid; u32 doc2; u32 doc3; };
    const Case cases[] = {
        {&kClsidDomDocument, S_OK_, E_NOINTERFACE_},
        {&kClsidFtDomDocument, S_OK_, E_NOINTERFACE_},
        {&kClsidDomDocument2, S_OK_, E_NOINTERFACE_},
        {&kClsidFtDomDocument2, S_OK_, E_NOINTERFACE_},
        {&kClsid26, CLASSNOTREG_, CLASSNOTREG_},
        {&kClsidFt26, CLASSNOTREG_, CLASSNOTREG_},
        {&kClsid30, S_OK_, E_NOINTERFACE_},
        {&kClsidFt30, S_OK_, E_NOINTERFACE_},
        {&kClsid40, CLASSNOTREG_, CLASSNOTREG_},
        {&kClsidFt40, CLASSNOTREG_, CLASSNOTREG_},
        {&kClsid60, S_OK_, S_OK_},
        {&kClsidFt60, S_OK_, S_OK_},
    };
    for (const auto& c : cases) {
        GuestAddr p = 0;
        EXPECT_EQ(create(*c.clsid, kIidDoc2, p), c.doc2);
        EXPECT_EQ(p != 0, c.doc2 == S_OK_);
        EXPECT_EQ(create(*c.clsid, kIidDoc3, p), c.doc3);
        EXPECT_EQ(p != 0, c.doc3 == S_OK_);
    }
    // MsxmlManager::is_dom_document_clsid routes only registered coclasses.
    EXPECT_FALSE(MsxmlManager::is_dom_document_clsid(kClsid26));
    EXPECT_FALSE(MsxmlManager::is_dom_document_clsid(kClsid40));
    EXPECT_TRUE(MsxmlManager::is_dom_document_clsid(kClsidDomDocument2));
}

TEST_P(MsxmlDocument2Test, QueryInterfaceRoundTrips) {
    const auto qi = [&](GuestAddr self, const Guid& iid) {
        (void)memory_.write_bytes(kOut + 0x100, reinterpret_cast<const u8*>(&iid), sizeof iid);
        write_ptr(kOut + 0x120, 0xBAD);
        const u32 hr = call(MsxmlIface::Document, QI, self, {u64{kOut + 0x100}, u64{kOut + 0x120}});
        return std::pair{hr, read_ptr(kOut + 0x120)};
    };
    GuestAddr v6 = 0;
    ASSERT_EQ(create(kClsid60, kIidDoc, v6), S_OK_);
    EXPECT_EQ(qi(v6, kIidDoc2), std::pair(S_OK_, v6));
    EXPECT_EQ(qi(v6, kIidDoc3), std::pair(S_OK_, v6));
    EXPECT_EQ(qi(v6, kIidDoc), std::pair(S_OK_, v6));

    GuestAddr v3 = 0;
    ASSERT_EQ(create(kClsidDomDocument, kIidDoc, v3), S_OK_);
    EXPECT_EQ(qi(v3, kIidDoc2), std::pair(S_OK_, v3));
    EXPECT_EQ(qi(v3, kIidDoc3), std::pair(E_NOINTERFACE_, GuestAddr{0}));
    EXPECT_EQ(qi(v3, kIidDoc), std::pair(S_OK_, v3));
}

TEST_P(MsxmlDocument2Test, NodeQueryInterfaceAnswersExactlyItsTypesInterfaces) {
    // msxml2.h inheritance: IXMLDOMComment : IXMLDOMCharacterData : IXMLDOMNode;
    // IXMLDOMCDATASection : IXMLDOMText : IXMLDOMCharacterData;
    // IXMLDOMDocumentFragment : IXMLDOMNode. Any other DOM IID is E_NOINTERFACE
    // with *ppv = NULL (IUnknown::QueryInterface).
    const Guid node = MSXML_GUID(0x2933BF80u), chardata = MSXML_GUID(0x2933BF84u);
    const Guid element = MSXML_GUID(0x2933BF86u), text = MSXML_GUID(0x2933BF87u);
    const Guid comment = MSXML_GUID(0x2933BF88u), cdata = MSXML_GUID(0x2933BF8Au);
    const Guid fragment{0x3efaa413u, 0x272fu, 0x11d2u, {0x83,0x6f,0x00,0x00,0xf8,0x7a,0x77,0x82}};
    const Guid all[] = {node, chardata, element, text, comment, cdata, fragment, kIidDoc};
    const auto qi = [&](MsxmlIface iface, GuestAddr self, const Guid& iid) {
        (void)memory_.write_bytes(kOut + 0x100, reinterpret_cast<const u8*>(&iid), sizeof iid);
        write_ptr(kOut + 0x120, 0xBAD);
        const u32 hr = call(iface, QI, self, {u64{kOut + 0x100}, u64{kOut + 0x120}});
        const GuestAddr out = read_ptr(kOut + 0x120);
        if (hr == S_OK_) EXPECT_EQ(out, self);
        else EXPECT_EQ(out, 0u);
        return hr;
    };
    const GuestAddr d = new_doc(kClsid60);
    const auto make = [&](u32 slot, bool with_text) {
        write_ptr(kOut + 0x60, 0);
        const u32 hr = with_text
            ? call(MsxmlIface::Document, slot, d, {u64{bstr("x")}, u64{kOut + 0x60}})
            : call(MsxmlIface::Document, slot, d, {u64{kOut + 0x60}});
        EXPECT_EQ(hr, S_OK_);
        return read_ptr(kOut + 0x60);
    };
    struct Row { MsxmlIface iface; GuestAddr self; std::vector<Guid> served; };
    const Row rows[] = {
        {MsxmlIface::Element, make(47, true), {node, element}},
        {MsxmlIface::Node, make(48, false), {node, fragment}},
        {MsxmlIface::Text, make(49, true), {node, chardata, text}},
        {MsxmlIface::CharacterData, make(50, true), {node, chardata, comment}},
        {MsxmlIface::Text, make(51, true), {node, chardata, text, cdata}},
    };
    for (const auto& row : rows) {
        ASSERT_NE(row.self, 0u);
        for (const Guid& iid : all) {
            bool served = false;
            for (const Guid& g : row.served) served |= std::memcmp(&g, &iid, sizeof iid) == 0;
            EXPECT_EQ(qi(row.iface, row.self, iid), served ? S_OK_ : E_NOINTERFACE_)
                << "iface " << static_cast<u32>(row.iface) << " iid " << std::hex << iid.Data1;
        }
    }
}

TEST_P(MsxmlDocument2Test, PropertyDefaultsPerVersion) {
    struct Row { const char* name; u32 hr3; u16 vt3; const char* v3; u32 hr6; u16 vt6; const char* v6; };
    // probe lines "get(<name>)" for DOMDocument (msxml3) and DOMDocument60.
    const Row rows[] = {
        {"SelectionLanguage", S_OK_, VT_BSTR, "XSLPattern", S_OK_, VT_BSTR, "XPath"},
        {"SelectionNamespaces", S_OK_, VT_BSTR, "", S_OK_, VT_BSTR, ""},
        {"ServerHTTPRequest", S_OK_, VT_BOOL, "0", S_OK_, VT_BOOL, "0"},
        {"ProhibitDTD", S_OK_, VT_BOOL, "0", S_OK_, VT_BOOL, "-1"},
        {"NewParser", E_FAIL_, kPoison, "", S_OK_, VT_BOOL, "0"},
        {"ResolveExternals", E_FAIL_, kPoison, "", S_OK_, VT_BOOL, "0"},
        {"AllowDocumentFunction", S_OK_, VT_BOOL, "-1", S_OK_, VT_BOOL, "0"},
        {"AllowXsltScript", S_OK_, VT_BOOL, "-1", S_OK_, VT_BOOL, "0"},
        {"ValidateOnParse", E_FAIL_, kPoison, "", S_OK_, VT_BOOL, "-1"},
        {"MaxElementDepth", S_OK_, VT_I4, "5000", S_OK_, VT_I4, "256"},
        {"MaxXMLSize", S_OK_, VT_I4, "0", S_OK_, VT_I4, "0"},
        {"UseInlineSchema", E_FAIL_, kPoison, "", S_OK_, VT_BOOL, "0"},
        {"ForcedResync", S_OK_, VT_BOOL, "-1", S_OK_, VT_BOOL, "-1"},
        {"MultipleErrorMessages", E_FAIL_, kPoison, "", S_OK_, VT_BOOL, "0"},
        {"NormalizeAttributeValues", E_FAIL_, kPoison, "", S_OK_, VT_BOOL, "0"},
        {"PopulateElementDefaultValues", E_FAIL_, kPoison, "", S_OK_, VT_BOOL, "0"},
        {"SchemaLocations", E_FAIL_, kPoison, "", E_FAIL_, VT_EMPTY, ""},
        {"Bogus", E_FAIL_, kPoison, "", E_FAIL_, VT_EMPTY, ""},
        {"selectionlanguage", E_FAIL_, kPoison, "", E_FAIL_, VT_EMPTY, ""},
        {"SELECTIONLANGUAGE", E_FAIL_, kPoison, "", E_FAIL_, VT_EMPTY, ""},
        {"", E_FAIL_, kPoison, "", E_FAIL_, VT_EMPTY, ""},
        {nullptr, E_FAIL_, kPoison, "", E_FAIL_, VT_EMPTY, ""},
    };
    for (const Guid* clsid : {&kClsidDomDocument, &kClsidDomDocument2, &kClsid30, &kClsidFt30,
                              &kClsid60, &kClsidFt60}) {
        const bool v6 = clsid == &kClsid60 || clsid == &kClsidFt60;
        const GuestAddr doc = new_doc(*clsid);
        for (const auto& r : rows) {
            SCOPED_TRACE(r.name ? r.name : "<NULL>");
            const Prop p = get(doc, r.name);
            EXPECT_EQ(p.hr, v6 ? r.hr6 : r.hr3);
            EXPECT_EQ(p.vt, v6 ? r.vt6 : r.vt3);
            EXPECT_EQ(p.value, v6 ? r.v6 : r.v3);
        }
        // getProperty(name, NULL) -> E_INVALIDARG.
        EXPECT_EQ(call(MsxmlIface::Document, GetProperty, doc,
                       {u64{bstr("SelectionLanguage")}, u64{0}}), E_INVALIDARG_);
        // First-level mirrors: validateOnParse -1, resolveExternals -1 (3.0) /
        // 0 (6.0), preserveWhiteSpace 0, async -1.
        const auto first = [&](u32 slot) {
            (void)memory_.write_u16(kOut + 0x70, 7);
            EXPECT_EQ(call(MsxmlIface::Document, slot, doc, {u64{kOut + 0x70}}), S_OK_);
            return static_cast<i16>(memory_.read_u16(kOut + 0x70).value());
        };
        EXPECT_EQ(first(GetValidateOnParse), -1);
        EXPECT_EQ(first(GetResolveExternals), v6 ? 0 : -1);
        EXPECT_EQ(first(GetAsync), -1);
    }
}

TEST_P(MsxmlDocument2Test, SetPropertyContract) {
    for (const bool v6 : {false, true}) {
        SCOPED_TRACE(v6 ? "DOMDocument60" : "DOMDocument");
        const GuestAddr d = new_doc(v6 ? kClsid60 : kClsidDomDocument);
        const char* lang = v6 ? "XPath" : "XSLPattern";
        EXPECT_EQ(set_str(d, "Bogus", "x"), E_FAIL_);
        EXPECT_EQ(set_str(d, "selectionlanguage", "XPath"), E_FAIL_);
        EXPECT_EQ(set_str(d, "SelectionLanguage", "xpath"), E_FAIL_);
        EXPECT_EQ(get(d, "SelectionLanguage").value, lang);
        EXPECT_EQ(set_str(d, "SelectionLanguage", "Bogus"), E_FAIL_);
        EXPECT_EQ(set_typed(d, "SelectionLanguage", VT_I4, 1), E_FAIL_);
        EXPECT_EQ(get(d, "SelectionLanguage").value, lang);
        // SelectionNamespaces keeps even a rejected string.
        EXPECT_EQ(set_str(d, "SelectionNamespaces", "garbage"), E_FAIL_);
        EXPECT_EQ(get(d, "SelectionNamespaces").value, "garbage");
        EXPECT_EQ(set_str(d, "SelectionNamespaces", "xmlns:a=\"urn:a\""), S_OK_);
        EXPECT_EQ(get(d, "SelectionNamespaces").value, "xmlns:a=\"urn:a\"");
        EXPECT_EQ(set_str(d, "SelectionNamespaces", "  xmlns:a='urn:a'   xmlns:b='urn:b' "), S_OK_);
        EXPECT_EQ(get(d, "SelectionNamespaces").value, "  xmlns:a='urn:a'   xmlns:b='urn:b' ");
        EXPECT_EQ(set_str(d, "SelectionNamespaces", "xmlns='urn:d'"), S_OK_);
        EXPECT_EQ(set_str(d, "SelectionNamespaces", "xmlns:a='urn:a' foo='x'"), E_FAIL_);
        EXPECT_EQ(set_str(d, "SelectionNamespaces", "xmlns:a=urn:a"), E_FAIL_);
        EXPECT_EQ(get(d, "SelectionNamespaces").value, "xmlns:a=urn:a");
        EXPECT_EQ(set_str(d, "SelectionNamespaces", ""), S_OK_);
        EXPECT_EQ(set_typed(d, "SelectionNamespaces", VT_I4, 1), E_FAIL_);
        EXPECT_EQ(get(d, "SelectionNamespaces").value, "1");
        // Boolean coercion (VariantChangeType to VT_BOOL).
        EXPECT_EQ(set_typed(d, "ProhibitDTD", VT_BOOL, 1), S_OK_);
        EXPECT_EQ(get(d, "ProhibitDTD").value, "-1");
        EXPECT_EQ(set_typed(d, "ProhibitDTD", VT_I4, 0), S_OK_);
        EXPECT_EQ(get(d, "ProhibitDTD").value, "0");
        EXPECT_EQ(set_str(d, "ProhibitDTD", "true"), S_OK_);
        EXPECT_EQ(get(d, "ProhibitDTD").value, "-1");
        EXPECT_EQ(set_str(d, "ProhibitDTD", "garbage"), TYPEMISMATCH_);
        EXPECT_EQ(get(d, "ProhibitDTD").value, "-1");
        // ValidateOnParse/ResolveExternals are second-level only on 6.0 and
        // mirror the first-level properties there.
        EXPECT_EQ(set_typed(d, "ValidateOnParse", VT_BOOL, 0), v6 ? S_OK_ : E_FAIL_);
        (void)memory_.write_u16(kOut + 0x70, 7);
        (void)call(MsxmlIface::Document, GetValidateOnParse, d, {u64{kOut + 0x70}});
        EXPECT_EQ(static_cast<i16>(memory_.read_u16(kOut + 0x70).value()), v6 ? 0 : -1);
        (void)call(MsxmlIface::Document, PutValidateOnParse, d, {u64{0xFFFF}});
        EXPECT_EQ(get(d, "ValidateOnParse").hr, v6 ? S_OK_ : E_FAIL_);
        EXPECT_EQ(set_typed(d, "ResolveExternals", VT_BOOL, 1), v6 ? S_OK_ : E_FAIL_);
        (void)call(MsxmlIface::Document, PutResolveExternals, d, {u64{0}});
        if (v6) EXPECT_EQ(get(d, "ResolveExternals").value, "0");
        // Integer limits.
        EXPECT_EQ(set_typed(d, "MaxElementDepth", VT_I4, 5), S_OK_);
        EXPECT_EQ(get(d, "MaxElementDepth").value, "5");
        EXPECT_EQ(set_typed(d, "MaxElementDepth", VT_I4, -1), E_INVALIDARG_);
        EXPECT_EQ(set_str(d, "MaxElementDepth", "7"), S_OK_);
        EXPECT_EQ(set_str(d, "MaxElementDepth", "x"), TYPEMISMATCH_);
        EXPECT_EQ(set_typed(d, "MaxElementDepth", VT_BOOL, 1), E_INVALIDARG_);
        EXPECT_EQ(get(d, "MaxElementDepth").value, "7");
        EXPECT_EQ(set_typed(d, "MaxElementDepth", VT_I4, 0x7fffffff), S_OK_);
        EXPECT_EQ(get(d, "MaxElementDepth").value, "2147483647");
        EXPECT_EQ(set_typed(d, "MaxXMLSize", VT_I4, 4194304), S_OK_);
        EXPECT_EQ(set_typed(d, "MaxXMLSize", VT_I4, -1), E_INVALIDARG_);
        EXPECT_EQ(get(d, "MaxXMLSize").value, "4194304");
        // Booleans known to both versions.
        for (const char* name : {"ServerHTTPRequest", "AllowDocumentFunction", "AllowXsltScript"}) {
            EXPECT_EQ(set_typed(d, name, VT_BOOL, 1), S_OK_);
            EXPECT_EQ(get(d, name).value, "-1");
        }
        EXPECT_EQ(set_typed(d, "ForcedResync", VT_BOOL, 0), S_OK_);
        EXPECT_EQ(get(d, "ForcedResync").value, "0");
        // 6.0-only booleans.
        for (const char* name : {"NewParser", "UseInlineSchema", "MultipleErrorMessages",
                                 "NormalizeAttributeValues", "PopulateElementDefaultValues"}) {
            EXPECT_EQ(set_typed(d, name, VT_BOOL, 1), v6 ? S_OK_ : E_FAIL_);
            EXPECT_EQ(get(d, name).hr, v6 ? S_OK_ : E_FAIL_);
        }
        EXPECT_EQ(set_str(d, "SchemaLocations", "urn:a a.xsd"), E_FAIL_);
        // A NULL language string, then a NULL property name.
        EXPECT_EQ(set_str(d, "SelectionLanguage", nullptr), E_FAIL_);
        EXPECT_EQ(call(MsxmlIface::Document, SetProperty, d,
                       {u64{0}, VarArg{VT_BSTR, 0}}), E_INVALIDARG_);
        // msxml6 has no XSLPattern.
        EXPECT_EQ(set_str(d, "SelectionLanguage", "XSLPattern"), v6 ? E_FAIL_ : S_OK_);
        EXPECT_EQ(set_str(d, "SelectionLanguage", "XPath"), S_OK_);
    }
}

TEST_P(MsxmlDocument2Test, SelectionLanguageAndNamespacesDriveSelectNodes) {
    struct Exp { const char* expr; u32 hr; i32 len; u32 single_hr; const char* single; };
    const auto check = [&](GuestAddr d, const std::vector<Exp>& rows) {
        for (const auto& e : rows) {
            SCOPED_TRACE(e.expr);
            const Sel s = select(d, e.expr);
            EXPECT_EQ(s.hr, e.hr);
            EXPECT_EQ(s.len, e.len);
            EXPECT_EQ(s.single_hr, e.single_hr);
            EXPECT_EQ(s.single, e.single);
        }
    };
    // msxml3: XSLPattern default.
    GuestAddr d = new_doc(kClsidDomDocument);
    ASSERT_EQ(load(d, kSelectionDoc).hr, S_OK_);
    check(d, {{"//x", S_OK_, 0, S_FALSE_, "-"}, {"//a:x", S_OK_, 1, S_OK_, "a:x"},
              {"//q:x", S_OK_, 1, S_OK_, "q:x"}, {"//y", S_OK_, 1, S_OK_, "y"},
              {"//w", S_OK_, 1, S_OK_, "w"}, {"/r", S_OK_, 1, S_OK_, "r"}});
    ASSERT_EQ(set_str(d, "SelectionNamespaces", "xmlns:a='urn:a' xmlns:d='urn:d'"), S_OK_);
    check(d, {{"//a:x", S_OK_, 2, S_OK_, "a:x"}, {"//q:x", S_OK_, 1, S_OK_, "q:x"},
              {"//d:y", S_OK_, 1, S_OK_, "y"}, {"//y", S_OK_, 1, S_OK_, "y"},
              {"/d:r/a:x[@id='2']", S_OK_, 1, S_OK_, "q:x"},
              {"//nope:x", S_OK_, 0, S_FALSE_, "-"}});
    ASSERT_EQ(set_str(d, "SelectionNamespaces", "xmlns='urn:d'"), S_OK_);
    check(d, {{"//y", S_OK_, 1, S_OK_, "y"}});
    ASSERT_EQ(set_str(d, "SelectionLanguage", "XPath"), S_OK_);
    ASSERT_EQ(set_str(d, "SelectionNamespaces", ""), S_OK_);
    check(d, {{"//x", S_OK_, 0, S_FALSE_, "-"}, {"//y", S_OK_, 0, S_FALSE_, "-"},
              {"//a:x", E_FAIL_, -1, E_FAIL_, "-"}, {"//w", S_OK_, 1, S_OK_, "w"}});
    ASSERT_EQ(set_str(d, "SelectionNamespaces", "xmlns:a='urn:a' xmlns:d='urn:d'"), S_OK_);
    check(d, {{"//a:x", S_OK_, 2, S_OK_, "a:x"}, {"//d:y", S_OK_, 1, S_OK_, "y"},
              {"//d:*", S_OK_, 2, S_OK_, "r"}, {"//a:*", S_OK_, 2, S_OK_, "a:x"}});
    ASSERT_EQ(set_str(d, "SelectionNamespaces", "xmlns='urn:d'"), S_OK_);
    check(d, {{"//y", S_OK_, 0, S_FALSE_, "-"}});
    ASSERT_EQ(set_str(d, "SelectionLanguage", "XSLPattern"), S_OK_);
    ASSERT_EQ(set_str(d, "SelectionNamespaces", ""), S_OK_);
    check(d, {{"//x", S_OK_, 0, S_FALSE_, "-"}, {"//y", S_OK_, 1, S_OK_, "y"},
              {"//a:x", S_OK_, 1, S_OK_, "a:x"}});

    // msxml6: XPath default.
    d = new_doc(kClsid60);
    ASSERT_EQ(load(d, kSelectionDoc).hr, S_OK_);
    check(d, {{"//x", S_OK_, 0, S_FALSE_, "-"}, {"//a:x", E_FAIL_, -1, E_FAIL_, "-"},
              {"//q:x", E_FAIL_, -1, E_FAIL_, "-"}, {"//y", S_OK_, 0, S_FALSE_, "-"},
              {"//w", S_OK_, 1, S_OK_, "w"}, {"/r", S_OK_, 0, S_FALSE_, "-"}});
    ASSERT_EQ(set_str(d, "SelectionNamespaces", "xmlns:a='urn:a' xmlns:d='urn:d'"), S_OK_);
    check(d, {{"//a:x", S_OK_, 2, S_OK_, "a:x"}, {"//q:x", E_FAIL_, -1, E_FAIL_, "-"},
              {"//d:y", S_OK_, 1, S_OK_, "y"}, {"//y", S_OK_, 0, S_FALSE_, "-"},
              {"/d:r/a:x[@id='2']", S_OK_, 1, S_OK_, "q:x"},
              {"//nope:x", E_FAIL_, -1, E_FAIL_, "-"}});

    // Namespace declarations are attributes only under XSLPattern
    // ("nsdecl" in the details section).
    d = new_doc(kClsid30);
    ASSERT_EQ(set_str(d, "SelectionLanguage", "XPath"), S_OK_);
    ASSERT_EQ(load(d, "<r xmlns:p='urn:p' p:at='1' plain='x'/>").hr, S_OK_);
    check(d, {{"/*/@*", S_OK_, 2, S_OK_, "p:at"}, {"//@*", S_OK_, 2, S_OK_, "p:at"}});
    ASSERT_EQ(set_str(d, "SelectionNamespaces", "xmlns:p='urn:p'"), S_OK_);
    check(d, {{"/*/@p:at", S_OK_, 1, S_OK_, "p:at"}, {"/*/@at", S_OK_, 0, S_FALSE_, "-"}});
    ASSERT_EQ(set_str(d, "SelectionLanguage", "XSLPattern"), S_OK_);
    check(d, {{"/*/@*", S_OK_, 3, S_OK_, "xmlns:p"}, {"/*/@p:at", S_OK_, 1, S_OK_, "p:at"}});
}

TEST_P(MsxmlDocument2Test, NamespaceUrisOfElementsAndDeclarations) {
    for (const bool v6 : {false, true}) {
        const GuestAddr d = new_doc(v6 ? kClsid60 : kClsidDomDocument);
        ASSERT_EQ(load(d, "<r xmlns:p='urn:p' p:at='1' plain='a' xmlns='urn:d'><p:c/></r>").hr, S_OK_);
        const GuestAddr root = out_object(MsxmlIface::Document, DocumentElement, d);
        EXPECT_EQ(node_string(MsxmlIface::Element, NamespaceUri, root), "urn:d");
        const GuestAddr child = out_object(MsxmlIface::Element, FirstChild, root);
        EXPECT_EQ(node_string(MsxmlIface::Element, NamespaceUri, child), "urn:p");
        const GuestAddr map = out_object(MsxmlIface::Element, Attributes, root);
        const char* expect[] = {v6 ? "http://www.w3.org/2000/xmlns/" : "", "urn:p", "",
                                v6 ? "http://www.w3.org/2000/xmlns/" : ""};
        for (u64 i = 0; i < 4; ++i) {
            write_ptr(kOut + 0x58, 0);
            ASSERT_EQ(call(MsxmlIface::NamedNodeMap, MapItem, map, {i, u64{kOut + 0x58}}), S_OK_);
            const GuestAddr attr = read_ptr(kOut + 0x58);
            EXPECT_EQ(node_string(MsxmlIface::Attribute, NamespaceUri, attr), expect[i]) << i;
        }
    }
}

TEST_P(MsxmlDocument2Test, ParserLimitsFailTheLoad) {
    for (const bool v6 : {false, true}) {
        SCOPED_TRACE(v6 ? "DOMDocument60" : "DOMDocument30");
        const GuestAddr d = new_doc(v6 ? kClsid60 : kClsid30);
        const std::string dtd = "<?xml version='1.0'?>\n<!DOCTYPE r [<!ELEMENT r ANY>]><r/>";
        // ProhibitDTD default: off on 3.0, on on 6.0 ("dtd default").
        Load l = load(d, dtd);
        EXPECT_EQ(l.hr, v6 ? S_FALSE_ : S_OK_);
        ASSERT_EQ(set_typed(d, "ProhibitDTD", VT_BOOL, 1), S_OK_);
        l = load(d, dtd);
        EXPECT_EQ(l.hr, S_FALSE_);
        EXPECT_EQ(l.ok, 0);
        Err e = doc_error(d);
        EXPECT_EQ(static_cast<u32>(e.code), v6 ? 0xC00CE584u : 0xC00CE556u);
        EXPECT_EQ(e.reason, v6 ? "DTD is prohibited." : "Invalid at the top level of the document.");
        EXPECT_EQ(e.line, 2); EXPECT_EQ(e.pos, 11); EXPECT_EQ(e.fpos, 32);
        EXPECT_EQ(e.src, "<!DOCTYPE r [<!ELEMENT r ANY>]><r/>");
        ASSERT_EQ(set_typed(d, "ProhibitDTD", VT_BOOL, 0), S_OK_);
        EXPECT_EQ(load(d, dtd).hr, S_OK_);

        ASSERT_EQ(set_typed(d, "MaxElementDepth", VT_I4, 2), S_OK_);
        EXPECT_EQ(load(d, "<a><b/></a>").hr, S_OK_);
        EXPECT_EQ(load(d, "<a>\n <b><c/></b></a>").hr, S_FALSE_);
        e = doc_error(d);
        EXPECT_EQ(static_cast<u32>(e.code), v6 ? 0xC00CE586u : 0x80004004u);
        EXPECT_EQ(e.reason, v6 ? "Operation aborted: MaxElementDepth constraint violated."
                               : "System error: MaxElementDepth.");
        EXPECT_EQ(e.line, 2); EXPECT_EQ(e.pos, 6); EXPECT_EQ(e.fpos, 9);
        EXPECT_EQ(e.src, " <b><c/></b></a>");
        ASSERT_EQ(set_typed(d, "MaxElementDepth", VT_I4, 0), S_OK_);
        EXPECT_EQ(load(d, "<a><b><c><d/></c></b></a>").hr, S_OK_);

        ASSERT_EQ(set_typed(d, "MaxXMLSize", VT_I4, 1), S_OK_);
        EXPECT_EQ(load(d, "<r>" + std::string(1010, 'x') + "</r>").hr, S_OK_);
        EXPECT_EQ(load(d, "<r>" + std::string(1030, 'x') + "</r>").hr, S_FALSE_);
        e = doc_error(d);
        EXPECT_EQ(static_cast<u32>(e.code), v6 ? 0xC00CE585u : 0x80004004u);
        EXPECT_EQ(e.reason, v6 ? "Operation aborted: MaxXMLSize constraint violated."
                               : "System error: MaxXMLSize.");
        EXPECT_EQ(e.line, 1); EXPECT_EQ(e.pos, 1); EXPECT_EQ(e.fpos, 0);
        ASSERT_EQ(set_typed(d, "MaxXMLSize", VT_I4, 0), S_OK_);
    }
}

TEST_P(MsxmlDocument2Test, NormalizeAttributeValues) {
    const GuestAddr d = new_doc(kClsid60);
    const std::string xml = "<r plain='a\tb\nc&#10;d&#9;e  f'/>";
    const auto value = [&]() {
        const GuestAddr root = out_object(MsxmlIface::Document, DocumentElement, d);
        (void)call(MsxmlIface::Element, AttrValue, root, {u64{bstr("plain")}, u64{kVarOut}});
        return read_bstr(read_ptr(kVarOut + 8));
    };
    ASSERT_EQ(load(d, xml).hr, S_OK_);
    EXPECT_EQ(value(), "a\tb\nc\nd\te  f");
    ASSERT_EQ(set_typed(d, "NormalizeAttributeValues", VT_BOOL, 1), S_OK_);
    ASSERT_EQ(load(d, xml).hr, S_OK_);
    EXPECT_EQ(value(), "a b c\nd\te  f");
}

TEST_P(MsxmlDocument2Test, NamespacesCollection) {
    for (const bool v6 : {false, true}) {
        SCOPED_TRACE(v6 ? "DOMDocument60" : "DOMDocument");
        const GuestAddr d = new_doc(v6 ? kClsid60 : kClsidDomDocument);
        ASSERT_EQ(load(d, kSelectionDoc).hr, S_OK_);
        const GuestAddr sc = out_object(MsxmlIface::Document, GetNamespaces, d);
        ASSERT_NE(sc, 0u);
        const auto length = [&]() {
            EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScLength, sc, {u64{kOut + 0x60}}), S_OK_);
            return static_cast<i32>(memory_.read_u32(kOut + 0x60).value());
        };
        const auto uri = [&](u64 i) {
            EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScNamespaceUri, sc, {i, u64{kOut + 0x68}}), S_OK_);
            return read_bstr(read_ptr(kOut + 0x68));
        };
        // 6.0 lists the xmlns='' undeclaration's empty URI; 3.0 does not.
        ASSERT_EQ(length(), v6 ? 3 : 2);
        EXPECT_EQ(uri(0), "urn:d");
        EXPECT_EQ(uri(1), "urn:a");
        if (v6) EXPECT_EQ(uri(2), "");
        write_ptr(kOut + 0x68, 1);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScNamespaceUri, sc,
                       {u64(v6 ? 3 : 2), u64{kOut + 0x68}}), E_FAIL_);
        EXPECT_EQ(read_ptr(kOut + 0x68), v6 ? 0u : 1u);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScNamespaceUri, sc,
                       {u64{0xFFFFFFFFu}, u64{kOut + 0x68}}), E_FAIL_);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScNamespaceUri, sc, {u64{0}, u64{0}}), E_POINTER_);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScLength, sc, {u64{0}}), E_POINTER_);
        write_ptr(kOut + 0x68, 1);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScGet, sc, {u64{bstr("urn:a")}, u64{kOut + 0x68}}),
                  v6 ? E_NOTIMPL_ : S_OK_);
        EXPECT_EQ(read_ptr(kOut + 0x68), 0u);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScRemove, sc, {u64{bstr("urn:a")}}), E_FAIL_);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScAdd, sc,
                       {u64{bstr("urn:a")}, VarArg{VT_NULL, 0}}), E_FAIL_);
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScAddCollection, sc, {u64{0}}), E_FAIL_);
        // Unimplemented: the IEnumVARIANT enumerator (Windows: S_OK).
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, ScNewEnum, sc, {u64{kOut + 0x68}}), E_NOTIMPL_);
        EXPECT_EQ(length(), v6 ? 3 : 2);
        // Same object every time; it is live across a reload.
        EXPECT_EQ(out_object(MsxmlIface::Document, GetNamespaces, d), sc);
        ASSERT_EQ(load(d, "<r/>").hr, S_OK_);
        EXPECT_EQ(length(), 0);
        EXPECT_EQ(call(MsxmlIface::Document, GetNamespaces, d, {u64{0}}), E_POINTER_);
        // QI to IXMLDOMSchemaCollection.
        (void)memory_.write_bytes(kOut + 0x100, reinterpret_cast<const u8*>(&kIidSchemaCollection), sizeof(Guid));
        EXPECT_EQ(call(MsxmlIface::SchemaCollection, QI, sc, {u64{kOut + 0x100}, u64{kOut + 0x120}}), S_OK_);
        // Declarations, in document order, including unused ones.
        ASSERT_EQ(load(d, "<r xmlns:u='urn:unused' xmlns:p='urn:p' p:at='1'><c xmlns='urn:c'/><p:x/></r>").hr, S_OK_);
        ASSERT_EQ(length(), 3);
        EXPECT_EQ(uri(0), "urn:unused");
        EXPECT_EQ(uri(1), "urn:p");
        EXPECT_EQ(uri(2), "urn:c");
    }
}

TEST_P(MsxmlDocument2Test, SchemasAndValidate) {
    for (const bool v6 : {false, true}) {
        SCOPED_TRACE(v6 ? "DOMDocument60" : "DOMDocument");
        const GuestAddr d = new_doc(v6 ? kClsid60 : kClsidDomDocument);
        (void)memory_.write_u16(kVarOut, kPoison);
        EXPECT_EQ(call(MsxmlIface::Document, GetSchemas, d, {u64{kVarOut}}), S_FALSE_);
        EXPECT_EQ(memory_.read_u16(kVarOut).value(), VT_NULL);
        EXPECT_EQ(call(MsxmlIface::Document, GetSchemas, d, {u64{0}}), E_POINTER_);
        EXPECT_EQ(call(MsxmlIface::Document, PutrefSchemas, d, {VarArg{VT_NULL, 0}}), S_OK_);
        EXPECT_EQ(call(MsxmlIface::Document, PutrefSchemas, d, {VarArg{VT_EMPTY, 0}}), S_OK_);
        EXPECT_EQ(call(MsxmlIface::Document, PutrefSchemas, d, {VarArg{VT_I4, 3}}), E_FAIL_);

        // Empty document.
        GuestAddr err = out_object(MsxmlIface::Document, Validate, d);
        ASSERT_NE(err, 0u);
        Err e = error_of(err);
        EXPECT_EQ(static_cast<u32>(e.code), 0xC00CE223u);
        EXPECT_EQ(e.reason, "Validate failed because the document does not contain exactly one root node.");
        EXPECT_EQ(e.line, 0); EXPECT_EQ(e.pos, 0); EXPECT_EQ(e.fpos, 0);

        ASSERT_EQ(load(d, "<r><c/></r>").hr, S_OK_);
        write_ptr(kOut + 0x58, 0);
        EXPECT_EQ(call(MsxmlIface::Document, Validate, d, {u64{kOut + 0x58}}), S_FALSE_);
        e = error_of(read_ptr(kOut + 0x58));
        EXPECT_EQ(static_cast<u32>(e.code), 0xC00CE224u);
        EXPECT_EQ(e.reason, v6 ? "The node is neither valid nor invalid because no DTD/Schema declaration was found."
                               : "Validate failed because the root element had no associated DTD/schema.");
        // The document's own parseError is untouched.
        EXPECT_EQ(doc_error(d).code, 0);
        EXPECT_EQ(call(MsxmlIface::Document, Validate, d, {u64{0}}), v6 ? E_FAIL_ : S_FALSE_);

        // Unimplemented: DTD validation (Windows: S_OK for a valid document).
        ASSERT_EQ(set_typed(d, "ProhibitDTD", VT_BOOL, 0), S_OK_);
        ASSERT_EQ(load(d, "<!DOCTYPE r [<!ELEMENT r (c)><!ELEMENT c EMPTY>]><r><c/></r>").hr, S_OK_);
        EXPECT_EQ(call(MsxmlIface::Document, Validate, d, {u64{kOut + 0x58}}), E_NOTIMPL_);
    }
}

TEST_P(MsxmlDocument2Test, ValidateNodeAndImportNode) {
    const GuestAddr d = new_doc(kClsid60);
    ASSERT_EQ(load(d, "<r a='1'><c>t</c></r>").hr, S_OK_);
    const GuestAddr root = out_object(MsxmlIface::Document, DocumentElement, d);
    const GuestAddr c = out_object(MsxmlIface::Element, FirstChild, root);

    write_ptr(kOut + 0x58, 0);
    EXPECT_EQ(call(MsxmlIface::Document, ValidateNode, d, {u64{c}, u64{kOut + 0x58}}), S_FALSE_);
    Err e = error_of(read_ptr(kOut + 0x58));
    EXPECT_EQ(static_cast<u32>(e.code), 0xC00CE00Du);
    EXPECT_EQ(e.reason, "The element 'c' is used but not declared in the DTD/Schema.");
    EXPECT_EQ(call(MsxmlIface::Document, ValidateNode, d, {u64{0}, u64{kOut + 0x58}}), E_INVALIDARG_);
    EXPECT_EQ(call(MsxmlIface::Document, ValidateNode, d, {u64{root}, u64{0}}), E_FAIL_);

    const GuestAddr other = new_doc(kClsid60);
    write_ptr(kOut + 0x58, 0);
    EXPECT_EQ(call(MsxmlIface::Document, ValidateNode, other, {u64{c}, u64{kOut + 0x58}}), S_FALSE_);
    e = error_of(read_ptr(kOut + 0x58));
    EXPECT_EQ(static_cast<u32>(e.code), 0xC00CE23Eu);
    EXPECT_EQ(e.reason, "Validate failed because the node provided does not belong to this document.");

    // importNode: a parentless copy owned by the importing document.
    write_ptr(kOut + 0x58, 0);
    ASSERT_EQ(call(MsxmlIface::Document, ImportNode, other, {u64{root}, u64{0xFFFF}, u64{kOut + 0x58}}), S_OK_);
    const GuestAddr deep = read_ptr(kOut + 0x58);
    ASSERT_NE(deep, 0u);
    ASSERT_NE(deep, root);
    EXPECT_EQ(node_string(MsxmlIface::Element, Xml, deep), "<r a=\"1\"><c>t</c></r>");
    write_ptr(kOut + 0x50, 1);
    EXPECT_EQ(call(MsxmlIface::Element, ParentNode, deep, {u64{kOut + 0x50}}), S_FALSE_);
    EXPECT_EQ(read_ptr(kOut + 0x50), 0u);
    EXPECT_EQ(out_object(MsxmlIface::Element, OwnerDocument, deep), other);
    write_ptr(kOut + 0x60, 0);
    EXPECT_EQ(call(MsxmlIface::Document, AppendChild, other, {u64{deep}, u64{kOut + 0x60}}), S_OK_);

    write_ptr(kOut + 0x58, 0);
    ASSERT_EQ(call(MsxmlIface::Document, ImportNode, other, {u64{root}, u64{0}, u64{kOut + 0x58}}), S_OK_);
    EXPECT_EQ(node_string(MsxmlIface::Element, Xml, read_ptr(kOut + 0x58)), "<r a=\"1\"/>");

    write_ptr(kOut + 0x58, 1);
    EXPECT_EQ(call(MsxmlIface::Document, ImportNode, other, {u64{0}, u64{0xFFFF}, u64{kOut + 0x58}}), E_INVALIDARG_);
    EXPECT_EQ(read_ptr(kOut + 0x58), 1u);
    EXPECT_EQ(call(MsxmlIface::Document, ImportNode, other, {u64{root}, u64{0xFFFF}, u64{0}}), E_INVALIDARG_);
    EXPECT_EQ(call(MsxmlIface::Document, ImportNode, other, {u64{d}, u64{0xFFFF}, u64{kOut + 0x58}}), E_INVALIDARG_);
    // Importing from the same document still copies.
    write_ptr(kOut + 0x58, 0);
    ASSERT_EQ(call(MsxmlIface::Document, ImportNode, d, {u64{root}, u64{0xFFFF}, u64{kOut + 0x58}}), S_OK_);
    EXPECT_NE(read_ptr(kOut + 0x58), root);
}

TEST(MsxmlDocument2Slots, VtableCarriesDocument3LayoutAndStdcallBytes) {
    // IXMLDOMDocument3: 7 IDispatch + 36 IXMLDOMNode + 33 IXMLDOMDocument
    // + 6 IXMLDOMDocument2 + 2 IXMLDOMDocument3 (msxml6.h).
    EXPECT_EQ(MsxmlManager::slot_count(MsxmlIface::Document), 84u);
    EXPECT_EQ(MsxmlManager::slot_count(MsxmlIface::SchemaCollection), 14u);
    // this + {pointer | BSTR | VARIANT(16) | VARIANT_BOOL(4)} on x86.
    const u32 doc2[] = {8, 8, 20, 8, 24, 12, 12, 16};
    for (u32 i = 0; i < 8; ++i) {
        EXPECT_EQ(MsxmlManager::slot_stdcall_bytes(MsxmlIface::Document, 76 + i), doc2[i]) << 76 + i;
    }
    const u32 sc[] = {24, 12, 8, 8, 12, 8, 8};
    for (u32 i = 0; i < 7; ++i) {
        EXPECT_EQ(MsxmlManager::slot_stdcall_bytes(MsxmlIface::SchemaCollection, 7 + i), sc[i]) << 7 + i;
    }
}

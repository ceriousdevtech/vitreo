// SPDX-License-Identifier: LGPL-2.1-only

/// @file msxml_com.cpp
/// @brief IXMLDOM* COM projection of the host DOM.

#include "winapi/com/msxml/msxml_com.hpp"

#include "capsule/vfs/vfs.hpp"
#include "winapi/com/msxml/xpath.hpp"
#include "winapi/kernel32/error.hpp"
#include <vitreo/common.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>

namespace vitreo::winapi {

// ole32's "{XXXXXXXX-...}" formatter (ole_guid_internal.hpp, whose IID
// constants clash with this file's).
[[nodiscard]] std::string format_guid(const Guid& g);

namespace {

// One UNIMPLEMENTED line per (object kind, IID) MSXML does not serve, so an
// E_NOINTERFACE that ends a guest's XML handling names the missing contract.
void log_unserved_iid(const char* object, const Guid& iid) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::string key = std::string(object) + " " + format_guid(iid);
    std::lock_guard lock(mutex);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msxml: %s does not serve IID %s (E_NOINTERFACE)\n",
                 object, format_guid(iid).c_str());
}

// One "[vitreo] UNIMPLEMENTED msxml!<member>: <case>" line per (member, case):
// the member fails with its documented HRESULT instead of faking success.
void log_unimplemented(const char* member, const std::string& what) {
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::string key = std::string(member) + ": " + what;
    std::lock_guard lock(mutex);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "[vitreo] UNIMPLEMENTED msxml!%s\n", key.c_str());
}

constexpr u32 S_OK_HR                   = 0x00000000u;
constexpr u32 S_FALSE_HR                = 0x00000001u;
constexpr u32 E_NOTIMPL_HR              = 0x80004001u;
constexpr u32 E_NOINTERFACE_HR          = 0x80004002u;
constexpr u32 E_POINTER_HR              = 0x80004003u;
constexpr u32 E_FAIL_HR                 = 0x80004005u;
constexpr u32 E_INVALIDARG_HR           = 0x80070057u;
constexpr u32 E_OUTOFMEMORY_HR          = 0x8007000Eu;
constexpr u32 DISP_E_TYPEMISMATCH_HR    = 0x80020005u;
constexpr u32 DISP_E_OVERFLOW_HR        = 0x8002000Au;

// validate()/validateNode() outcomes (tools/oracle_probe/
// msxml_document2_probe.expected; 3.0 and 6.0 word the no-DTD case
// differently).
constexpr i32 kValNotOneRoot  = static_cast<i32>(0xC00CE223u);
constexpr i32 kValNoDtd       = static_cast<i32>(0xC00CE224u);
constexpr i32 kValUndeclared  = static_cast<i32>(0xC00CE00Du);
constexpr i32 kValForeignNode = static_cast<i32>(0xC00CE23Eu);

// MSXML returns this from selectNodes when the expression is not valid.
constexpr u32 XPATH_INVALID_HR          = 0x80004005u; // E_FAIL, per MSXML

constexpr u16 VT_EMPTY_T    = 0;
constexpr u16 VT_NULL_T     = 1;
constexpr u16 VT_I2_T       = 2;
constexpr u16 VT_I4_T       = 3;
constexpr u16 VT_R4_T       = 4;
constexpr u16 VT_R8_T       = 5;
constexpr u16 VT_BSTR_T     = 8;
constexpr u16 VT_DISPATCH_T = 9;
constexpr u16 VT_BOOL_T     = 11;
constexpr u16 VT_UNKNOWN_T  = 13;
constexpr u16 VT_I1_T       = 16;
constexpr u16 VT_UI1_T      = 17;
constexpr u16 VT_UI2_T      = 18;
constexpr u16 VT_UI4_T      = 19;
constexpr u16 VT_I8_T       = 20;
constexpr u16 VT_UI8_T      = 21;
constexpr u16 VT_INT_T      = 22;
constexpr u16 VT_UINT_T     = 23;

// IIDs from the Windows SDK msxml.h.
constexpr Guid IID_IUNKNOWN{0x00000000u, 0x0000u, 0x0000u,
                            {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};
constexpr Guid IID_IDISPATCH{0x00020400u, 0x0000u, 0x0000u,
                             {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

#define MSXML_IID(name, d1)                                                    \
    constexpr Guid name{d1, 0x7B36u, 0x11d2u,                                  \
                        {0xB2,0x0E,0x00,0xC0,0x4F,0x98,0x3E,0x60}}
MSXML_IID(IID_IXMLDOMNODE,       0x2933BF80u);
MSXML_IID(IID_IXMLDOMDOCUMENT,   0x2933BF81u);
MSXML_IID(IID_IXMLDOMNODELIST,   0x2933BF82u);
MSXML_IID(IID_IXMLDOMNAMEDNODEMAP, 0x2933BF83u);
MSXML_IID(IID_IXMLDOMCHARACTERDATA, 0x2933BF84u);
MSXML_IID(IID_IXMLDOMATTRIBUTE,  0x2933BF85u);
MSXML_IID(IID_IXMLDOMELEMENT,    0x2933BF86u);
MSXML_IID(IID_IXMLDOMTEXT,       0x2933BF87u);
MSXML_IID(IID_IXMLDOMCOMMENT,    0x2933BF88u);
MSXML_IID(IID_IXMLDOMPI,         0x2933BF89u);
MSXML_IID(IID_IXMLDOMCDATA,      0x2933BF8Au);
MSXML_IID(IID_IXMLDOMDOCUMENTTYPE, 0x2933BF8Bu);
MSXML_IID(IID_IXMLDOMNOTATION,   0x2933BF8Cu);
MSXML_IID(IID_IXMLDOMENTITY,     0x2933BF8Du);
MSXML_IID(IID_IXMLDOMENTITYREFERENCE, 0x2933BF8Eu);
MSXML_IID(IID_IXMLDOMDOCUMENT2,  0x2933BF95u);
MSXML_IID(IID_IXMLDOMDOCUMENT3,  0x2933BF96u);
#undef MSXML_IID

constexpr Guid IID_IXMLDOMSCHEMACOLLECTION{
    0x373984c8u, 0xb845u, 0x449bu, {0x91,0xe7,0x45,0xac,0x83,0x03,0x6a,0xde}};

// msxml2.h: IXMLDOMDocumentFragment adds no members to IXMLDOMNode.
constexpr Guid IID_IXMLDOMDOCUMENTFRAGMENT{
    0x3efaa413u, 0x272fu, 0x11d2u,
    {0x83,0x6f,0x00,0x00,0xf8,0x7a,0x77,0x82}};

constexpr Guid IID_IXMLDOMPARSEERROR{
    0x3efaa426u, 0x272fu, 0x11d2u,
    {0x83,0x6f,0x00,0x00,0xf8,0x7a,0x77,0x82}};

/// Every interface a DOM object of some node type implements (msxml2.h).
[[nodiscard]] bool is_dom_iid(const Guid& iid) {
    static const Guid* const kAll[] = {
        &IID_IXMLDOMNODE, &IID_IXMLDOMDOCUMENT, &IID_IXMLDOMDOCUMENT2,
        &IID_IXMLDOMDOCUMENT3, &IID_IXMLDOMNODELIST,
        &IID_IXMLDOMNAMEDNODEMAP, &IID_IXMLDOMCHARACTERDATA,
        &IID_IXMLDOMATTRIBUTE, &IID_IXMLDOMELEMENT, &IID_IXMLDOMTEXT,
        &IID_IXMLDOMCOMMENT, &IID_IXMLDOMPI, &IID_IXMLDOMCDATA,
        &IID_IXMLDOMDOCUMENTTYPE, &IID_IXMLDOMNOTATION, &IID_IXMLDOMENTITY,
        &IID_IXMLDOMENTITYREFERENCE, &IID_IXMLDOMDOCUMENTFRAGMENT,
        &IID_IXMLDOMPARSEERROR, &IID_IXMLDOMSCHEMACOLLECTION};
    for (const Guid* g : kAll) {
        if (std::memcmp(g, &iid, sizeof iid) == 0) return true;
    }
    return false;
}

// DOMDocument coclasses (msxml.h / msxml2.h / msxml6.h). Windows serves the
// version-independent and 3.0 ones from msxml3 and the 6.0 ones from msxml6;
// DOMDocument26/40 are not registered and fail with REGDB_E_CLASSNOTREG
// (tools/oracle_probe/msxml_document2_probe.expected).
constexpr Guid CLSID_DOMDOCUMENT{0x2933BF90u, 0x7B36u, 0x11d2u,
                                 {0xB2,0x0E,0x00,0xC0,0x4F,0x98,0x3E,0x60}};
constexpr Guid CLSID_FREETHREADEDDOMDOCUMENT{
    0x2933BF91u, 0x7B36u, 0x11d2u, {0xB2,0x0E,0x00,0xC0,0x4F,0x98,0x3E,0x60}};
constexpr Guid CLSID_DOMDOCUMENT2{0xf6d90f11u, 0x9c73u, 0x11d3u,
                                  {0xb3,0x2e,0x00,0xc0,0x4f,0x99,0x0b,0xb4}};
constexpr Guid CLSID_FREETHREADEDDOMDOCUMENT2{
    0xf6d90f12u, 0x9c73u, 0x11d3u, {0xb3,0x2e,0x00,0xc0,0x4f,0x99,0x0b,0xb4}};
constexpr Guid CLSID_DOMDOCUMENT30{0xf5078f32u, 0xc551u, 0x11d3u,
                                   {0x89,0xb9,0x00,0x00,0xf8,0x1f,0xe2,0x21}};
constexpr Guid CLSID_FREETHREADEDDOMDOCUMENT30{
    0xf5078f33u, 0xc551u, 0x11d3u, {0x89,0xb9,0x00,0x00,0xf8,0x1f,0xe2,0x21}};
constexpr Guid CLSID_DOMDOCUMENT60{0x88d96a05u, 0xf192u, 0x11d4u,
                                   {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};
constexpr Guid CLSID_FREETHREADEDDOMDOCUMENT60{
    0x88d96a06u, 0xf192u, 0x11d4u, {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};

// XSLT / schema coclasses this component does not implement.
constexpr Guid CLSID_XSLTEMPLATE60{0x88d96a08u, 0xf192u, 0x11d4u,
                                   {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};
constexpr Guid CLSID_XMLSCHEMACACHE60{0x88d96a07u, 0xf192u, 0x11d4u,
                                      {0xa6,0x5f,0x00,0x40,0x96,0x32,0x51,0xe5}};

[[nodiscard]] bool guid_eq(const Guid& a, const Guid& b) {
    return a.Data1 == b.Data1 && a.Data2 == b.Data2 && a.Data3 == b.Data3 &&
           std::equal(std::begin(a.Data4), std::end(a.Data4),
                      std::begin(b.Data4));
}

// Per-slot stdcall byte counts, derived from the SDK's Vtbl signatures. A
// VARIANT passed by value occupies four dwords on x86, which is why the
// put_nodeValue / load / save entries are wider than a pointer argument.
constexpr u32 kNodeBytes[43] = {
    12,4,4,8,16,24,36,8,8,20,8,8,8,8,8,8,8,8,28,16,12,12,8,8,12,8,8,8,8,8,8,20,
    8,8,8,12,12,12,8,8,8,8,24};
// IXMLDOMDocument slots 43..75, then IXMLDOMDocument2 get_namespaces,
// get_schemas, putref_schemas(VARIANT), validate, setProperty(BSTR, VARIANT),
// getProperty, then IXMLDOMDocument3 validateNode, importNode(node, BOOL, out).
constexpr u32 kDocumentTailBytes[41] = {
    8,8,8,8,12,8,12,12,12,16,12,12,12,32,12,24,8,8,8,8,8,4,12,20,8,8,8,8,8,8,
    20,20,20,
    8,8,20,8,24,12,
    12,16};
constexpr u32 kElementTailBytes[9]   = {8,12,12,8,12,12,12,12,4};
constexpr u32 kCharDataTailBytes[8]  = {8,8,8,16,8,12,12,16};
constexpr u32 kTextTailBytes[9]      = {8,8,8,16,8,12,12,16,12};
constexpr u32 kAttributeTailBytes[3] = {8,8,8};
constexpr u32 kPiTailBytes[3]        = {8,8,8};
constexpr u32 kNodeListBytes[12]     = {12,4,4,8,16,24,36,12,8,8,4,8};
constexpr u32 kNamedNodeMapBytes[17] = {
    12,4,4,8,16,24,36,12,12,12,12,8,16,16,8,4,8};
constexpr u32 kParseErrorBytes[14]   = {12,4,4,8,16,24,36,8,8,8,8,8,8,8};
// IXMLDOMSchemaCollection: add(BSTR, VARIANT), get, remove, get_length,
// get_namespaceURI(long, BSTR*), addCollection, get__newEnum.
constexpr u32 kSchemaCollectionBytes[14] = {12,4,4,8,16,24,36,24,12,8,8,12,8,8};

[[nodiscard]] const char* iface_tag(MsxmlIface iface) {
    switch (iface) {
        case MsxmlIface::Document:              return "Doc";
        case MsxmlIface::Node:                  return "Node";
        case MsxmlIface::Element:               return "Elem";
        case MsxmlIface::Text:                  return "Text";
        case MsxmlIface::CharacterData:         return "CData";
        case MsxmlIface::Attribute:             return "Attr";
        case MsxmlIface::ProcessingInstruction: return "Pi";
        case MsxmlIface::NodeList:              return "List";
        case MsxmlIface::NamedNodeMap:          return "Map";
        case MsxmlIface::ParseError:            return "Err";
        case MsxmlIface::SchemaCollection:      return "Schemas";
        case MsxmlIface::Count:                 break;
    }
    return "?";
}

/// UTF-8 -> UTF-16. MSXML strings are BSTRs, so a byte-wise widening would
/// corrupt every non-ASCII name or value.
[[nodiscard]] std::vector<u16> utf8_to_utf16(std::string_view text) {
    std::vector<u16> out;
    out.reserve(text.size());
    usize i = 0;
    while (i < text.size()) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        u32 cp = 0;
        usize extra = 0;
        if (b0 < 0x80u)          { cp = b0;          extra = 0; }
        else if ((b0 & 0xE0u) == 0xC0u) { cp = b0 & 0x1Fu; extra = 1; }
        else if ((b0 & 0xF0u) == 0xE0u) { cp = b0 & 0x0Fu; extra = 2; }
        else if ((b0 & 0xF8u) == 0xF0u) { cp = b0 & 0x07u; extra = 3; }
        else { cp = 0xFFFDu; extra = 0; }
        if (i + extra >= text.size()) { cp = 0xFFFDu; extra = 0; }
        for (usize k = 1; k <= extra; ++k) {
            cp = (cp << 6) |
                 (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        }
        i += extra + 1;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            out.push_back(static_cast<u16>(0xD800u + (cp >> 10)));
            out.push_back(static_cast<u16>(0xDC00u + (cp & 0x3FFu)));
        } else {
            out.push_back(static_cast<u16>(cp));
        }
    }
    return out;
}

[[nodiscard]] std::string utf16_to_utf8(const std::vector<u16>& units) {
    std::string out;
    out.reserve(units.size());
    for (usize i = 0; i < units.size(); ++i) {
        u32 cp = units[i];
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 1 < units.size() &&
            units[i + 1] >= 0xDC00u && units[i + 1] <= 0xDFFFu) {
            cp = 0x10000u + ((cp - 0xD800u) << 10) + (units[++i] - 0xDC00u);
        }
        if (cp < 0x80u) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Slot tables
// ---------------------------------------------------------------------------

u32 MsxmlManager::slot_count(MsxmlIface iface) {
    switch (iface) {
        case MsxmlIface::Document:              return 84;
        case MsxmlIface::Node:                  return 43;
        case MsxmlIface::Element:               return 52;
        case MsxmlIface::Text:                  return 52;
        case MsxmlIface::CharacterData:         return 51;
        case MsxmlIface::Attribute:             return 46;
        case MsxmlIface::ProcessingInstruction: return 46;
        case MsxmlIface::NodeList:              return 12;
        case MsxmlIface::NamedNodeMap:          return 17;
        case MsxmlIface::ParseError:            return 14;
        case MsxmlIface::SchemaCollection:      return 14;
        case MsxmlIface::Count:                 break;
    }
    return 0;
}

u32 MsxmlManager::slot_stdcall_bytes(MsxmlIface iface, u32 slot) {
    switch (iface) {
        case MsxmlIface::NodeList:
            return slot < 12 ? kNodeListBytes[slot] : 4u;
        case MsxmlIface::NamedNodeMap:
            return slot < 17 ? kNamedNodeMapBytes[slot] : 4u;
        case MsxmlIface::ParseError:
            return slot < 14 ? kParseErrorBytes[slot] : 4u;
        case MsxmlIface::SchemaCollection:
            return slot < 14 ? kSchemaCollectionBytes[slot] : 4u;
        default:
            break;
    }
    // Node-derived interfaces share slots 0..42.
    if (slot < 43) return kNodeBytes[slot];
    const u32 tail = slot - 43;
    switch (iface) {
        case MsxmlIface::Document:
            return tail < 41 ? kDocumentTailBytes[tail] : 4u;
        case MsxmlIface::Element:
            return tail < 9 ? kElementTailBytes[tail] : 4u;
        case MsxmlIface::Text:
            return tail < 9 ? kTextTailBytes[tail] : 4u;
        case MsxmlIface::CharacterData:
            return tail < 8 ? kCharDataTailBytes[tail] : 4u;
        case MsxmlIface::Attribute:
            return tail < 3 ? kAttributeTailBytes[tail] : 4u;
        case MsxmlIface::ProcessingInstruction:
            return tail < 3 ? kPiTailBytes[tail] : 4u;
        default:
            break;
    }
    return 4u;
}

std::string MsxmlManager::slot_thunk_name(MsxmlIface iface, u32 slot) {
    return std::string("Vitreo_Msxml_") + iface_tag(iface) + "_" +
           std::to_string(slot);
}

MsxmlIface MsxmlManager::iface_for(msxml::NodeType type) {
    switch (type) {
        case msxml::NodeType::Document:              return MsxmlIface::Document;
        case msxml::NodeType::Element:               return MsxmlIface::Element;
        case msxml::NodeType::Text:
        case msxml::NodeType::CDATASection:          return MsxmlIface::Text;
        case msxml::NodeType::Comment:               return MsxmlIface::CharacterData;
        case msxml::NodeType::Attribute:             return MsxmlIface::Attribute;
        case msxml::NodeType::ProcessingInstruction:
            return MsxmlIface::ProcessingInstruction;
        default:                                     return MsxmlIface::Node;
    }
}

// ---------------------------------------------------------------------------
// Object lifetime
// ---------------------------------------------------------------------------

MsxmlManager::Object* MsxmlManager::find(GuestAddr iface_ptr) {
    auto it = m_objects.find(iface_ptr);
    return it == m_objects.end() ? nullptr : &it->second;
}

GuestAddr MsxmlManager::build_vtable(Marshaler& m, MsxmlIface iface) {
    const auto index = static_cast<usize>(iface);
    if (m_vtables[index] != 0) return m_vtables[index];
    if (m_thunks == nullptr || m_next_addr == 0) return 0;

    const u32 count = slot_count(iface);
    const GuestAddr ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr vtable = m_next_addr;
    m_next_addr += ptr_size * count;

    for (u32 slot = 0; slot < count; ++slot) {
        auto addr = m_thunks->get_thunk_address("msxml6.dll",
                                                slot_thunk_name(iface, slot));
        if (addr.is_err() || addr.value() == 0) return 0;
        if (m.write_ptr(vtable + static_cast<GuestAddr>(slot) * ptr_size,
                        addr.value()).is_err()) {
            return 0;
        }
    }
    m_vtables[index] = vtable;
    return vtable;
}

bool MsxmlManager::write_object(Marshaler& m, GuestAddr object,
                                MsxmlIface iface) {
    const GuestAddr vtable = build_vtable(m, iface);
    if (vtable == 0) return false;
    return m.write_ptr(object, vtable).is_ok();
}

GuestAddr MsxmlManager::make_object(
    Marshaler& m, MsxmlIface iface,
    const std::shared_ptr<msxml::Document>& doc) {
    if (m_next_addr == 0) return 0;
    const GuestAddr ptr_size = m.cpu_is_64bit() ? 8u : 4u;
    const GuestAddr object = m_next_addr;
    m_next_addr += ptr_size;
    if (!write_object(m, object, iface)) return 0;
    Object state;
    state.iface = iface;
    state.refs  = 1;
    state.doc   = doc;
    m_objects.emplace(object, std::move(state));
    return object;
}

GuestAddr MsxmlManager::intern_node(
    Marshaler& m, const msxml::Node::Ptr& node,
    const std::shared_ptr<msxml::Document>& doc) {
    if (!node) return 0;
    if (auto it = m_node_to_iface.find(node.get());
        it != m_node_to_iface.end()) {
        if (auto* existing = find(it->second)) {
            ++existing->refs;
            return it->second;
        }
        m_node_to_iface.erase(it);
    }
    const MsxmlIface iface = iface_for(node->type());
    const GuestAddr object = make_object(m, iface, doc);
    if (object == 0) return 0;
    if (auto* state = find(object)) state->node = node;
    m_node_to_iface.emplace(node.get(), object);
    return object;
}

// ---------------------------------------------------------------------------
// Marshaling helpers
// ---------------------------------------------------------------------------

std::string MsxmlManager::read_bstr(Marshaler& m, GuestAddr bstr) const {
    if (bstr == 0) return {};
    // A BSTR's byte length lives in the DWORD immediately before the data.
    auto len = m.read_u32(bstr - 4);
    if (len.is_err()) return {};
    const u32 units = len.value() / 2u;
    std::vector<u16> buffer;
    buffer.reserve(units);
    for (u32 i = 0; i < units; ++i) {
        auto unit = m.read_u16(bstr + static_cast<GuestAddr>(i) * 2u);
        if (unit.is_err()) break;
        buffer.push_back(unit.value());
    }
    return utf16_to_utf8(buffer);
}

bool MsxmlManager::out_bstr(Marshaler& m, GuestAddr out,
                            std::string_view utf8) {
    if (out == 0) return false;
    if (!m_alloc_bstr) return false;
    const GuestAddr bstr = m_alloc_bstr(m, utf8_to_utf16(utf8));
    if (bstr == 0) return false;
    return m.write_ptr(out, bstr).is_ok();
}

bool MsxmlManager::out_node(Marshaler& m, GuestAddr out,
                            const msxml::Node::Ptr& node,
                            const std::shared_ptr<msxml::Document>& doc) {
    if (out == 0) return false;
    if (!node) return m.write_ptr(out, 0).is_ok();
    const GuestAddr object = intern_node(m, node, doc);
    if (object == 0) return false;
    return m.write_ptr(out, object).is_ok();
}

bool MsxmlManager::out_variant_bstr(Marshaler& m, GuestAddr out,
                                    std::string_view utf8) {
    if (out == 0 || !m_alloc_bstr) return false;
    const GuestAddr bstr = m_alloc_bstr(m, utf8_to_utf16(utf8));
    if (bstr == 0) return false;
    // tagVARIANT: vt at +0, payload at +8 in both bitnesses.
    if (m.write_u16(out, VT_BSTR_T).is_err()) return false;
    if (m.write_u16(out + 2, 0).is_err()) return false;
    if (m.write_u16(out + 4, 0).is_err()) return false;
    if (m.write_u16(out + 6, 0).is_err()) return false;
    return m.write_ptr(out + 8, bstr).is_ok();
}

bool MsxmlManager::out_variant_null(Marshaler& m, GuestAddr out) {
    if (out == 0) return false;
    if (m.write_u16(out, VT_NULL_T).is_err()) return false;
    if (m.write_u16(out + 2, 0).is_err()) return false;
    if (m.write_u16(out + 4, 0).is_err()) return false;
    if (m.write_u16(out + 6, 0).is_err()) return false;
    return m.write_ptr(out + 8, 0).is_ok();
}

bool MsxmlManager::out_bool(Marshaler& m, GuestAddr out, bool value) {
    if (out == 0) return false;
    // VARIANT_BOOL is a 16-bit -1 / 0.
    return m.write_u16(out, value ? 0xFFFFu : 0u).is_ok();
}

namespace {

/// Write a VARIANT header (vt, zero reserved words) and an 8-byte payload.
[[nodiscard]] bool write_variant(Marshaler& m, GuestAddr out, u16 vt,
                                 u64 payload) {
    if (out == 0) return false;
    if (m.write_u16(out, vt).is_err()) return false;
    if (m.write_u16(out + 2, 0).is_err()) return false;
    if (m.write_u16(out + 4, 0).is_err()) return false;
    if (m.write_u16(out + 6, 0).is_err()) return false;
    return m.write_u64(out + 8, payload).is_ok();
}

} // namespace

bool MsxmlManager::out_variant_bool(Marshaler& m, GuestAddr out, bool value) {
    return write_variant(m, out, VT_BOOL_T, value ? 0xFFFFu : 0u);
}

bool MsxmlManager::out_variant_i4(Marshaler& m, GuestAddr out, i32 value) {
    return write_variant(m, out, VT_I4_T, static_cast<u32>(value));
}

bool MsxmlManager::out_variant_empty(Marshaler& m, GuestAddr out) {
    return write_variant(m, out, VT_EMPTY_T, 0);
}

MsxmlManager::VariantIn MsxmlManager::read_variant_in(Marshaler& m, u32 slot) {
    VariantIn in;
    if (m.cpu_is_64bit()) {
        // x64 passes a by-value VARIANT through a hidden pointer.
        const GuestAddr addr = m.arg_ptr(slot);
        if (addr == 0) return in;
        in.vt   = m.read_u16(addr).value_or(0);
        in.bits = m.read_u64(addr + 8).value_or(0);
    } else {
        // x86 copies the 16 bytes onto the stack: vt in the low word of the
        // first dword, the payload in the third and fourth.
        in.vt   = static_cast<u16>(m.arg32(slot) & 0xFFFFu);
        in.bits = static_cast<u64>(m.arg32(slot + 2)) |
                  (static_cast<u64>(m.arg32(slot + 3)) << 32);
    }
    return in;
}

// ---------------------------------------------------------------------------
// Guest file access
// ---------------------------------------------------------------------------

std::optional<std::vector<u8>> MsxmlManager::read_guest_file(
    const std::string& path) const {
    if (m_vfs == nullptr) return std::nullopt;
    auto opened = m_vfs->open(path, capsule::AccessMode::Read,
                              capsule::ShareMode::Read,
                              capsule::CreationDisposition::OpenExisting);
    if (opened.is_err()) return std::nullopt;
    std::vector<u8> bytes;
    std::array<u8, 64 * 1024> chunk{};
    for (;;) {
        auto read = m_vfs->read(opened.value(), chunk);
        if (read.is_err() || read.value() == 0) break;
        bytes.insert(bytes.end(), chunk.begin(),
                     chunk.begin() + static_cast<std::ptrdiff_t>(read.value()));
    }
    (void)m_vfs->close(opened.value());
    return bytes;
}

bool MsxmlManager::write_guest_file(const std::string& path,
                                    std::string_view text) const {
    if (m_vfs == nullptr) return false;
    auto opened = m_vfs->open(path, capsule::AccessMode::Write,
                              capsule::ShareMode::None,
                              capsule::CreationDisposition::CreateAlways);
    if (opened.is_err()) return false;
    const std::span<const u8> bytes(
        reinterpret_cast<const u8*>(text.data()), text.size());
    const bool ok = m_vfs->write(opened.value(), bytes).is_ok();
    (void)m_vfs->close(opened.value());
    return ok;
}

// ---------------------------------------------------------------------------
// CoCreateInstance
// ---------------------------------------------------------------------------

std::optional<msxml::MsxmlVersion> MsxmlManager::dom_document_version(
    const Guid& clsid) {
    if (guid_eq(clsid, CLSID_DOMDOCUMENT) ||
        guid_eq(clsid, CLSID_FREETHREADEDDOMDOCUMENT) ||
        guid_eq(clsid, CLSID_DOMDOCUMENT2) ||
        guid_eq(clsid, CLSID_FREETHREADEDDOMDOCUMENT2) ||
        guid_eq(clsid, CLSID_DOMDOCUMENT30) ||
        guid_eq(clsid, CLSID_FREETHREADEDDOMDOCUMENT30)) {
        return msxml::MsxmlVersion::V3;
    }
    if (guid_eq(clsid, CLSID_DOMDOCUMENT60) ||
        guid_eq(clsid, CLSID_FREETHREADEDDOMDOCUMENT60)) {
        return msxml::MsxmlVersion::V6;
    }
    return std::nullopt;
}

bool MsxmlManager::is_dom_document_clsid(const Guid& clsid) {
    return dom_document_version(clsid).has_value();
}

bool MsxmlManager::is_unimplemented_msxml_clsid(const Guid& clsid) {
    return guid_eq(clsid, CLSID_XSLTEMPLATE60) ||
           guid_eq(clsid, CLSID_XMLSCHEMACACHE60);
}

u32 MsxmlManager::create_document(Marshaler& m, const Guid& clsid,
                                  const Guid& iid, GuestAddr ppv) {
    if (ppv == 0) return E_POINTER_HR;
    const auto version = dom_document_version(clsid);
    if (!version) {
        (void)m.write_ptr(ppv, 0);
        return 0x80040154u; // REGDB_E_CLASSNOTREG
    }
    const bool v6 = *version == msxml::MsxmlVersion::V6;
    // IXMLDOMDocument3 is msxml6-only: msxml3's coclasses answer
    // E_NOINTERFACE for it (msxml_document2_probe.expected).
    if (!guid_eq(iid, IID_IUNKNOWN) && !guid_eq(iid, IID_IDISPATCH) &&
        !guid_eq(iid, IID_IXMLDOMNODE) && !guid_eq(iid, IID_IXMLDOMDOCUMENT) &&
        !guid_eq(iid, IID_IXMLDOMDOCUMENT2) &&
        !(v6 && guid_eq(iid, IID_IXMLDOMDOCUMENT3))) {
        if (!guid_eq(iid, IID_IXMLDOMDOCUMENT3)) {
            log_unserved_iid("DOMDocument (CoCreateInstance)", iid);
        }
        (void)m.write_ptr(ppv, 0);
        return E_NOINTERFACE_HR;
    }
    auto doc = msxml::Document::create();
    // Per-version defaults, measured with getProperty and the first-level
    // getters on Windows 11 (msxml_document2_probe.expected). Learn's
    // "MaxElementDepth 0 in 3.0" is stale: msxml3 reports 5000.
    auto& settings = doc->settings();
    auto& props    = doc->properties();
    settings.version                    = *version;
    settings.prohibit_dtd               = v6;
    settings.max_element_depth          = v6 ? 256 : 5000;
    settings.max_xml_size_kb            = 0;
    settings.normalize_attribute_values = false;
    props.selection_language      = v6 ? "XPath" : "XSLPattern";
    props.async                   = true;
    props.validate_on_parse       = true;
    props.resolve_externals       = !v6;
    props.server_http_request     = false;
    props.allow_document_function = !v6;
    props.allow_xslt_script       = !v6;
    props.forced_resync           = true;
    const GuestAddr object = make_object(m, MsxmlIface::Document, doc);
    if (object == 0) {
        (void)m.write_ptr(ppv, 0);
        return E_OUTOFMEMORY_HR;
    }
    if (auto* state = find(object)) {
        state->node = doc;
        m_node_to_iface.emplace(doc.get(), object);
    }
    if (m.write_ptr(ppv, object).is_err()) return E_POINTER_HR;
    VITREO_TRACE("[msxml] CoCreateInstance DOMDocument (MSXML %s) -> 0x%llx\n",
                 v6 ? "6.0" : "3.0", static_cast<unsigned long long>(object));
    return S_OK_HR;
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

u32 MsxmlManager::query_interface(Marshaler& m, Object& self) {
    const GuestAddr self_ptr = m.arg_ptr(0);
    const GuestAddr iid_ptr  = m.arg_ptr(1);
    const GuestAddr ppv      = m.arg_ptr(2);
    if (ppv == 0) return E_POINTER_HR;
    auto iid = m.read_struct<Guid>(iid_ptr);
    if (iid.is_err()) {
        (void)m.write_ptr(ppv, 0);
        return E_INVALIDARG_HR;
    }
    const Guid& want = iid.value();

    bool ok = guid_eq(want, IID_IUNKNOWN) || guid_eq(want, IID_IDISPATCH);
    if (!ok) {
        switch (self.iface) {
            case MsxmlIface::Document: {
                auto doc = std::static_pointer_cast<msxml::Document>(self.node);
                const bool v6 = doc && doc->settings().version ==
                                           msxml::MsxmlVersion::V6;
                ok = guid_eq(want, IID_IXMLDOMDOCUMENT) ||
                     guid_eq(want, IID_IXMLDOMDOCUMENT2) ||
                     guid_eq(want, IID_IXMLDOMNODE) ||
                     (v6 && guid_eq(want, IID_IXMLDOMDOCUMENT3));
                if (!ok && guid_eq(want, IID_IXMLDOMDOCUMENT3)) {
                    // msxml3 does not implement IXMLDOMDocument3.
                    (void)m.write_ptr(ppv, 0);
                    return E_NOINTERFACE_HR;
                }
                break;
            }
            case MsxmlIface::Element:
                ok = guid_eq(want, IID_IXMLDOMELEMENT) ||
                     guid_eq(want, IID_IXMLDOMNODE);
                break;
            case MsxmlIface::Text:
                // msxml2.h: IXMLDOMCDATASection : IXMLDOMText, so a CDATA
                // section answers both; a plain text node is not a CDATA
                // section.
                ok = guid_eq(want, IID_IXMLDOMTEXT) ||
                     (self.node && self.node->type() ==
                                       msxml::NodeType::CDATASection &&
                      guid_eq(want, IID_IXMLDOMCDATA)) ||
                     guid_eq(want, IID_IXMLDOMCHARACTERDATA) ||
                     guid_eq(want, IID_IXMLDOMNODE);
                break;
            case MsxmlIface::CharacterData:
                ok = guid_eq(want, IID_IXMLDOMCOMMENT) ||
                     guid_eq(want, IID_IXMLDOMCHARACTERDATA) ||
                     guid_eq(want, IID_IXMLDOMNODE);
                break;
            case MsxmlIface::Attribute:
                ok = guid_eq(want, IID_IXMLDOMATTRIBUTE) ||
                     guid_eq(want, IID_IXMLDOMNODE);
                break;
            case MsxmlIface::ProcessingInstruction:
                ok = guid_eq(want, IID_IXMLDOMPI) ||
                     guid_eq(want, IID_IXMLDOMNODE);
                break;
            case MsxmlIface::Node:
                // IXMLDOMDocumentFragment and IXMLDOMEntityReference add no
                // members to IXMLDOMNode (msxml2.h), so the node vtable is
                // their full vtable.
                ok = guid_eq(want, IID_IXMLDOMNODE) ||
                     (self.node &&
                      ((self.node->type() ==
                            msxml::NodeType::DocumentFragment &&
                        guid_eq(want, IID_IXMLDOMDOCUMENTFRAGMENT)) ||
                       (self.node->type() ==
                            msxml::NodeType::EntityReference &&
                        guid_eq(want, IID_IXMLDOMENTITYREFERENCE))));
                break;
            case MsxmlIface::NodeList:
                ok = guid_eq(want, IID_IXMLDOMNODELIST);
                break;
            case MsxmlIface::NamedNodeMap:
                ok = guid_eq(want, IID_IXMLDOMNAMEDNODEMAP);
                break;
            case MsxmlIface::ParseError:
                ok = guid_eq(want, IID_IXMLDOMPARSEERROR);
                break;
            case MsxmlIface::SchemaCollection:
                ok = guid_eq(want, IID_IXMLDOMSCHEMACOLLECTION);
                break;
            case MsxmlIface::Count:
                break;
        }
    }
    if (!ok) {
        // A DOM interface the node's type does not implement is the
        // documented answer to a type probe (QI an arbitrary child for
        // IXMLDOMComment), not a missing feature: E_NOINTERFACE, no log.
        if (!is_dom_iid(want)) {
            log_unserved_iid("node object (QueryInterface)", want);
        }
        (void)m.write_ptr(ppv, 0);
        return E_NOINTERFACE_HR;
    }
    // Every supported IID is served by this same object; QueryInterface must
    // AddRef the pointer it returns.
    ++self.refs;
    return m.write_ptr(ppv, self_ptr).is_ok() ? S_OK_HR : E_POINTER_HR;
}

u32 MsxmlManager::release(Marshaler& m, GuestAddr self_ptr) {
    auto* self = find(self_ptr);
    if (self == nullptr) return 0;
    if (self->refs > 0) --self->refs;
    const u32 remaining = self->refs;
    if (remaining == 0) {
        if (self->node) m_node_to_iface.erase(self->node.get());
        if (self->iface == MsxmlIface::SchemaCollection && self->doc) {
            m_namespace_collections.erase(self->doc.get());
        }
        m_objects.erase(self_ptr);
    }
    (void)m;
    return remaining;
}

u32 MsxmlManager::dispatch(MsxmlIface iface, u32 slot, Marshaler& m) {
    const GuestAddr self_ptr = m.arg_ptr(0);
    auto* self = find(self_ptr);
    // Per-call tracing: a DOM caller that gives up part-way through is far
    // easier to place when the last slot it reached is on the record.
    VITREO_TRACE("[msxml] %s slot %u on 0x%llx%s\n", iface_tag(iface), slot,
                 static_cast<unsigned long long>(self_ptr),
                 self == nullptr ? " (unknown object)" : "");
    if (self == nullptr) return E_POINTER_HR;

    // IUnknown is common to every interface.
    switch (slot) {
        case 0: return query_interface(m, *self);
        case 1: return ++self->refs;
        case 2: return release(m, self_ptr);
        // IDispatch. Late binding would need the DISPID tables from
        // msxml2did.h; vtable callers never reach these.
        case 3: case 4: case 5: case 6:
            return E_NOTIMPL_HR;
        default:
            break;
    }

    // Every DOM method leaves the thread's last error at 0 -- success, S_FALSE
    // and failed load alike, DOMDocument/30/60, x86 and x64
    // (tools/oracle_probe/msxml_last_error_probe.expected). WiX's dutil reads
    // GetLastError() after these calls (ExitOnLastError), so a stale error
    // from an earlier failed FindFirstFile aborted VC_redist's .wxl parse.
    set_last_error(0);

    switch (iface) {
        case MsxmlIface::NodeList:     return dispatch_node_list(slot, m, *self);
        case MsxmlIface::NamedNodeMap: return dispatch_named_node_map(slot, m, *self);
        case MsxmlIface::ParseError:   return dispatch_parse_error(slot, m, *self);
        case MsxmlIface::SchemaCollection:
            return dispatch_schema_collection(slot, m, *self);
        default: break;
    }
    if (slot < 43) return dispatch_node(slot, m, *self);
    if (iface == MsxmlIface::Document && slot >= 76) {
        return dispatch_document2(
            slot, m, *self,
            std::static_pointer_cast<msxml::Document>(self->node));
    }
    switch (iface) {
        case MsxmlIface::Document: return dispatch_document(slot, m, *self);
        case MsxmlIface::Element:  return dispatch_element(slot, m, *self);
        case MsxmlIface::Text:
        case MsxmlIface::CharacterData:
            return dispatch_character_data(slot, m, *self);
        case MsxmlIface::Attribute: return dispatch_attribute(slot, m, *self);
        case MsxmlIface::ProcessingInstruction:
            return dispatch_character_data(slot, m, *self);
        default: break;
    }
    return E_NOTIMPL_HR;
}

u32 MsxmlManager::dispatch_node(u32 slot, Marshaler& m, Object& self) {
    auto& node = self.node;
    if (!node) return E_FAIL_HR;
    const auto& doc = self.doc;

    switch (slot) {
        case 7:  // get_nodeName
            return out_bstr(m, m.arg_ptr(1), node->name()) ? S_OK_HR
                                                           : E_POINTER_HR;
        case 8:  // get_nodeValue -> VARIANT
            if (node->has_null_value()) {
                return out_variant_null(m, m.arg_ptr(1)) ? S_OK_HR
                                                         : E_POINTER_HR;
            }
            return out_variant_bstr(m, m.arg_ptr(1), node->value())
                       ? S_OK_HR : E_POINTER_HR;
        case 9: { // put_nodeValue(VARIANT)
            if (node->has_null_value()) return E_FAIL_HR;
            // The VARIANT is by value: inline on x86, by hidden pointer on x64.
            GuestAddr vt_addr = 0;
            if (m.cpu_is_64bit()) {
                vt_addr = m.arg_ptr(1);
            } else {
                // Stack args start right after `this`; read the payload there.
                vt_addr = 0;
            }
            if (m.cpu_is_64bit() && vt_addr != 0) {
                auto vt = m.read_u16(vt_addr);
                if (vt.is_ok() && vt.value() == VT_BSTR_T) {
                    auto p = m.read_ptr(vt_addr + 8);
                    if (p.is_ok()) {
                        node->set_value(read_bstr(m, p.value()));
                        return S_OK_HR;
                    }
                }
                return E_INVALIDARG_HR;
            }
            // x86: VARIANT occupies arg slots 1..4; vt is the low word of 1.
            const u32 vt = static_cast<u32>(m.arg(1)) & 0xFFFFu;
            if (vt != VT_BSTR_T) return E_INVALIDARG_HR;
            node->set_value(read_bstr(m, m.arg_ptr(3)));
            return S_OK_HR;
        }
        case 10: // get_nodeType
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(node->type())).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        case 11: // get_parentNode
            return out_node(m, m.arg_ptr(1), node->parent(), doc)
                       ? (node->parent() ? S_OK_HR : S_FALSE_HR) : E_POINTER_HR;
        case 12: { // get_childNodes
            const GuestAddr out = m.arg_ptr(1);
            const GuestAddr list = make_object(m, MsxmlIface::NodeList, doc);
            if (list == 0) return E_OUTOFMEMORY_HR;
            if (auto* state = find(list)) state->list = node->children();
            return m.write_ptr(out, list).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 13: { auto c = node->first_child();
            return out_node(m, m.arg_ptr(1), c, doc)
                       ? (c ? S_OK_HR : S_FALSE_HR) : E_POINTER_HR; }
        case 14: { auto c = node->last_child();
            return out_node(m, m.arg_ptr(1), c, doc)
                       ? (c ? S_OK_HR : S_FALSE_HR) : E_POINTER_HR; }
        case 15: { auto c = node->previous_sibling();
            return out_node(m, m.arg_ptr(1), c, doc)
                       ? (c ? S_OK_HR : S_FALSE_HR) : E_POINTER_HR; }
        case 16: { auto c = node->next_sibling();
            return out_node(m, m.arg_ptr(1), c, doc)
                       ? (c ? S_OK_HR : S_FALSE_HR) : E_POINTER_HR; }
        case 17: { // get_attributes
            const GuestAddr out = m.arg_ptr(1);
            if (node->type() != msxml::NodeType::Element) {
                return m.write_ptr(out, 0).is_ok() ? S_FALSE_HR : E_POINTER_HR;
            }
            const GuestAddr map = make_object(m, MsxmlIface::NamedNodeMap, doc);
            if (map == 0) return E_OUTOFMEMORY_HR;
            if (auto* state = find(map)) {
                state->list = node->attributes();
                state->node = node;
            }
            return m.write_ptr(out, map).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 18: { // insertBefore(new, VARIANT ref, out)
            auto* child = find(m.arg_ptr(1));
            if (child == nullptr || !child->node) return E_INVALIDARG_HR;
            // The reference node arrives as a by-value VARIANT; a VT_NULL or
            // VT_EMPTY reference means "append".
            msxml::Node::Ptr ref;
            const GuestAddr out = m.cpu_is_64bit() ? m.arg_ptr(3)
                                                   : m.arg_ptr(6);
            GuestAddr ref_iface = 0;
            if (m.cpu_is_64bit()) {
                const GuestAddr vt_addr = m.arg_ptr(2);
                auto vt = m.read_u16(vt_addr);
                if (vt.is_ok() && (vt.value() == 9u || vt.value() == 13u)) {
                    auto p = m.read_ptr(vt_addr + 8);
                    if (p.is_ok()) ref_iface = p.value();
                }
            } else {
                const u32 vt = static_cast<u32>(m.arg(2)) & 0xFFFFu;
                if (vt == 9u || vt == 13u) ref_iface = m.arg_ptr(4);
            }
            if (ref_iface != 0) {
                if (auto* r = find(ref_iface)) ref = r->node;
            }
            if (!node->insert_before(child->node, ref)) return E_FAIL_HR;
            return out_node(m, out, child->node, doc) ? S_OK_HR : S_OK_HR;
        }
        case 19: { // replaceChild(new, old, out)
            auto* new_child = find(m.arg_ptr(1));
            auto* old_child = find(m.arg_ptr(2));
            if (old_child == nullptr || !old_child->node) return E_INVALIDARG_HR;
            auto replacement = new_child ? new_child->node : nullptr;
            if (!node->replace_child(replacement, old_child->node)) {
                return E_FAIL_HR;
            }
            return out_node(m, m.arg_ptr(3), old_child->node, doc) ? S_OK_HR
                                                                   : S_OK_HR;
        }
        case 20: { // removeChild(child, out)
            auto* child = find(m.arg_ptr(1));
            if (child == nullptr || !child->node) return E_INVALIDARG_HR;
            auto removed = child->node;
            if (!node->remove_child(removed)) return E_FAIL_HR;
            return out_node(m, m.arg_ptr(2), removed, doc) ? S_OK_HR : S_OK_HR;
        }
        case 21: { // appendChild(child, out)
            auto* child = find(m.arg_ptr(1));
            if (child == nullptr || !child->node) return E_INVALIDARG_HR;
            if (!node->append_child(child->node)) return E_FAIL_HR;
            return out_node(m, m.arg_ptr(2), child->node, doc) ? S_OK_HR
                                                               : S_OK_HR;
        }
        case 22: // hasChildNodes
            return out_bool(m, m.arg_ptr(1), !node->children().empty())
                       ? S_OK_HR : E_POINTER_HR;
        case 23: // get_ownerDocument
            return out_node(m, m.arg_ptr(1), doc, doc) ? S_OK_HR : E_POINTER_HR;
        case 24: { // cloneNode(deep, out)
            const bool deep = (static_cast<u32>(m.arg(1)) & 0xFFFFu) != 0;
            auto copy = node->clone(deep);
            return out_node(m, m.arg_ptr(2), copy, doc) ? S_OK_HR : E_POINTER_HR;
        }
        case 25: // get_nodeTypeString
            return out_bstr(m, m.arg_ptr(1),
                            msxml::node_type_string(node->type()))
                       ? S_OK_HR : E_POINTER_HR;
        case 26: // get_text
            return out_bstr(m, m.arg_ptr(1), node->text()) ? S_OK_HR
                                                           : E_POINTER_HR;
        case 27: // put_text
            node->set_text(read_bstr(m, m.arg_ptr(1)));
            return S_OK_HR;
        case 28: // get_specified — parsed attributes are always specified.
            return out_bool(m, m.arg_ptr(1), true) ? S_OK_HR : E_POINTER_HR;
        case 34: // get_xml
            return out_bstr(m, m.arg_ptr(1), node->xml()) ? S_OK_HR
                                                          : E_POINTER_HR;
        case 36: { // selectNodes(expr, out)
            const std::string expr = read_bstr(m, m.arg_ptr(1));
            auto result = select(self, expr);
            if (!result.supported || result.invalid) {
                (void)m.write_ptr(m.arg_ptr(2), 0);
                return XPATH_INVALID_HR;
            }
            const GuestAddr list = make_object(m, MsxmlIface::NodeList, doc);
            if (list == 0) return E_OUTOFMEMORY_HR;
            if (auto* state = find(list)) state->list = std::move(result.nodes);
            return m.write_ptr(m.arg_ptr(2), list).is_ok() ? S_OK_HR
                                                           : E_POINTER_HR;
        }
        case 37: { // selectSingleNode(expr, out)
            const std::string expr = read_bstr(m, m.arg_ptr(1));
            auto result = select(self, expr);
            if (!result.supported || result.invalid) {
                (void)m.write_ptr(m.arg_ptr(2), 0);
                return XPATH_INVALID_HR;
            }
            if (result.nodes.empty()) {
                return m.write_ptr(m.arg_ptr(2), 0).is_ok() ? S_FALSE_HR
                                                            : E_POINTER_HR;
            }
            return out_node(m, m.arg_ptr(2), result.nodes.front(), doc)
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 38: // get_parsed — this DOM is always fully parsed.
            return out_bool(m, m.arg_ptr(1), true) ? S_OK_HR : E_POINTER_HR;
        case 39: // get_namespaceURI
            return out_bstr(m, m.arg_ptr(1), node->namespace_uri())
                       ? S_OK_HR : E_POINTER_HR;
        case 40: // get_prefix
            return out_bstr(m, m.arg_ptr(1), node->prefix()) ? S_OK_HR
                                                             : E_POINTER_HR;
        case 41: // get_baseName
            return out_bstr(m, m.arg_ptr(1), node->base_name()) ? S_OK_HR
                                                                : E_POINTER_HR;
        // get_definition, nodeTypedValue, dataType and the XSLT transforms are
        // schema/XSLT features this component does not implement. Reporting
        // E_NOTIMPL is what MSXML does for an unavailable feature, and it keeps
        // callers from mistaking an empty result for a real answer.
        default:
            return E_NOTIMPL_HR;
    }
}

namespace {

// The parser skips a DOCTYPE's declarations: no DTD validation (validateOnParse)
// and no external DTD/entity loading (resolveExternals) happen, and entities
// declared there stay undefined. Say so when a loaded document relied on that.
void note_dtd_processing(const msxml::Document& doc) {
    if (!doc.has_doctype()) return;
    const auto& p = doc.properties();
    if (p.validate_on_parse) {
        log_unimplemented("IXMLDOMDocument::load",
                          "DTD validation (validateOnParse)");
    }
    if (p.resolve_externals) {
        log_unimplemented("IXMLDOMDocument::load",
                          "external DTD/entity resolution (resolveExternals)");
    }
}

} // namespace

u32 MsxmlManager::dispatch_document(u32 slot, Marshaler& m, Object& self) {
    auto doc = std::static_pointer_cast<msxml::Document>(self.node);
    if (!doc) return E_FAIL_HR;

    switch (slot) {
        case 45: { // get_documentElement
            auto root = doc->document_element();
            return out_node(m, m.arg_ptr(1), root, doc)
                       ? (root ? S_OK_HR : S_FALSE_HR) : E_POINTER_HR;
        }
        case 46: { // putref_documentElement
            auto* element = find(m.arg_ptr(1));
            if (element == nullptr || !element->node) return E_INVALIDARG_HR;
            if (auto existing = doc->document_element()) {
                (void)doc->remove_child(existing);
            }
            return doc->append_child(element->node) ? S_OK_HR : E_FAIL_HR;
        }
        case 47: // createElement
            return out_node(m, m.arg_ptr(2),
                            doc->create_element(read_bstr(m, m.arg_ptr(1))),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 48: // createDocumentFragment
            return out_node(m, m.arg_ptr(1), doc->create_document_fragment(),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 49: // createTextNode
            return out_node(m, m.arg_ptr(2),
                            doc->create_text_node(read_bstr(m, m.arg_ptr(1))),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 50: // createComment
            return out_node(m, m.arg_ptr(2),
                            doc->create_comment(read_bstr(m, m.arg_ptr(1))),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 51: // createCDATASection
            return out_node(m, m.arg_ptr(2),
                            doc->create_cdata_section(
                                read_bstr(m, m.arg_ptr(1))),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 52: // createProcessingInstruction(target, data, out)
            return out_node(m, m.arg_ptr(3),
                            doc->create_processing_instruction(
                                read_bstr(m, m.arg_ptr(1)),
                                read_bstr(m, m.arg_ptr(2))),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 53: // createAttribute
            return out_node(m, m.arg_ptr(2),
                            doc->create_attribute(read_bstr(m, m.arg_ptr(1))),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 54: // createEntityReference
            return out_node(m, m.arg_ptr(2),
                            doc->create_entity_reference(
                                read_bstr(m, m.arg_ptr(1))),
                            doc) ? S_OK_HR : E_POINTER_HR;
        case 55: { // getElementsByTagName
            const std::string tag = read_bstr(m, m.arg_ptr(1));
            std::vector<msxml::Node::Ptr> found;
            doc->collect_elements_by_tag_name(tag, found);
            const GuestAddr list = make_object(m, MsxmlIface::NodeList, doc);
            if (list == 0) return E_OUTOFMEMORY_HR;
            if (auto* state = find(list)) state->list = std::move(found);
            return m.write_ptr(m.arg_ptr(2), list).is_ok() ? S_OK_HR
                                                           : E_POINTER_HR;
        }
        case 58: { // load(VARIANT source, out success)
            // MS Learn documents load() as accepting either a URL/path
            // (VT_BSTR) or an in-memory document. Callers that already hold the
            // bytes — WiX Burn reading its manifest out of its own image is
            // one — pass a byte SAFEARRAY (VT_ARRAY | VT_UI1) instead of a
            // path, so both forms have to be decoded here.
            constexpr u16 VT_ARRAY_UI1 = 0x2011u;
            std::string path;
            std::string inline_text;
            bool        have_buffer = false;
            GuestAddr   out = 0;
            u16         vt = 0;
            GuestAddr   payload = 0;
            if (m.cpu_is_64bit()) {
                const GuestAddr vt_addr = m.arg_ptr(1);
                out = m.arg_ptr(2);
                if (auto v = m.read_u16(vt_addr); v.is_ok()) vt = v.value();
                if (auto p = m.read_ptr(vt_addr + 8); p.is_ok()) {
                    payload = p.value();
                }
            } else {
                vt  = static_cast<u16>(m.arg(1) & 0xFFFFu);
                out = m.arg_ptr(5);
                payload = m.arg_ptr(3);
            }
            VITREO_TRACE("[msxml] load: vt=0x%04x payload=0x%llx\n", vt,
                         static_cast<unsigned long long>(payload));
            if (vt == VT_BSTR_T) {
                path = read_bstr(m, payload);
                VITREO_TRACE("[msxml] load: path='%s'\n", path.c_str());
            } else if (vt == VT_ARRAY_UI1 && payload != 0) {
                // tagSAFEARRAY: cDims, fFeatures, cbElements, cLocks, pvData,
                // then rgsabound. pvData is pointer-aligned, so its offset and
                // the bound array's differ between bitnesses.
                const GuestAddr pvdata_off = m.cpu_is_64bit() ? 16u : 12u;
                const GuestAddr bound_off  = m.cpu_is_64bit() ? 24u : 16u;
                auto data = m.read_ptr(payload + pvdata_off);
                auto count = m.read_u32(payload + bound_off);
                if (data.is_ok() && count.is_ok() && data.value() != 0) {
                    inline_text.reserve(count.value());
                    for (u32 i = 0; i < count.value(); ++i) {
                        auto byte = m.read_u8(data.value() +
                                              static_cast<GuestAddr>(i));
                        if (byte.is_err()) break;
                        inline_text.push_back(static_cast<char>(byte.value()));
                    }
                    have_buffer = true;
                }
            } else {
                // IStream / IXMLDOMDocument sources are documented inputs this
                // component does not read. Fail rather than report an empty
                // document as a successful load.
                VITREO_TRACE("[msxml] load: unsupported source vt=0x%04x\n", vt);
                (void)out_bool(m, out, false);
                return E_NOTIMPL_HR;
            }

            if (have_buffer) {
                const bool ok = doc->load_xml(inline_text);
                if (ok) note_dtd_processing(*doc);
                (void)out_bool(m, out, ok);
                VITREO_TRACE("[msxml] load(buffer %zu bytes) -> %s\n",
                             inline_text.size(),
                             ok ? "ok" : doc->parse_error().reason.c_str());
                return ok ? S_OK_HR : S_FALSE_HR;
            }
            if (path.empty() || m_vfs == nullptr) {
                (void)out_bool(m, out, false);
                return S_FALSE_HR;
            }
            auto bytes = read_guest_file(path);
            if (!bytes.has_value()) {
                msxml::ParseError err;
                err.code   = static_cast<i32>(0x800C0005u); // cannot open file
                err.reason = "The system cannot locate the resource specified.";
                doc->set_parse_error(std::move(err));
                (void)out_bool(m, out, false);
                VITREO_TRACE("[msxml] load('%s') -> not found\n", path.c_str());
                return S_FALSE_HR;
            }
            const std::string text(
                reinterpret_cast<const char*>(bytes->data()), bytes->size());
            const bool ok = doc->load_xml(text);
            if (ok) note_dtd_processing(*doc);
            doc->set_url(path);
            (void)out_bool(m, out, ok);
            VITREO_TRACE("[msxml] load('%s') -> %s\n", path.c_str(),
                         ok ? "ok" : doc->parse_error().reason.c_str());
            return ok ? S_OK_HR : S_FALSE_HR;
        }
        case 59: // get_readyState — always 4 (completed); this DOM is sync.
            return m.write_u32(m.arg_ptr(1), 4u).is_ok() ? S_OK_HR
                                                         : E_POINTER_HR;
        case 60: { // get_parseError
            const GuestAddr err = make_object(m, MsxmlIface::ParseError, doc);
            if (err == 0) return E_OUTOFMEMORY_HR;
            return m.write_ptr(m.arg_ptr(1), err).is_ok() ? S_OK_HR
                                                          : E_POINTER_HR;
        }
        case 61: // get_url
            return out_bstr(m, m.arg_ptr(1), doc->url()) ? S_OK_HR
                                                         : E_POINTER_HR;
        case 62: // get_async (default true on 3.0 and 6.0)
            return out_bool(m, m.arg_ptr(1), doc->properties().async)
                       ? S_OK_HR : E_POINTER_HR;
        case 63: // put_async. Loads here complete before load() returns,
                 // which is a valid completion of an asynchronous load too.
            doc->properties().async = (m.arg32(1) & 0xFFFFu) != 0;
            return S_OK_HR;
        case 64: // abort — nothing is ever in flight.
            return S_OK_HR;
        case 65: { // loadXML(BSTR, out success)
            const std::string text = read_bstr(m, m.arg_ptr(1));
            const bool ok = doc->load_xml(text);
            if (ok) note_dtd_processing(*doc);
            (void)out_bool(m, m.arg_ptr(2), ok);
            if (!ok) {
                VITREO_TRACE("[msxml] loadXML failed: %s (line %d, pos %d)\n",
                             doc->parse_error().reason.c_str(),
                             doc->parse_error().line,
                             doc->parse_error().line_position);
            }
            return ok ? S_OK_HR : S_FALSE_HR;
        }
        case 66: { // save(VARIANT destination)
            std::string path;
            if (m.cpu_is_64bit()) {
                const GuestAddr vt_addr = m.arg_ptr(1);
                auto vt = m.read_u16(vt_addr);
                if (vt.is_ok() && vt.value() == VT_BSTR_T) {
                    auto p = m.read_ptr(vt_addr + 8);
                    if (p.is_ok()) path = read_bstr(m, p.value());
                }
            } else {
                const u32 vt = static_cast<u32>(m.arg(1)) & 0xFFFFu;
                if (vt == VT_BSTR_T) path = read_bstr(m, m.arg_ptr(3));
            }
            if (path.empty() || m_vfs == nullptr) return E_INVALIDARG_HR;
            const std::string text = doc->xml();
            const bool ok = write_guest_file(path, text);
            VITREO_TRACE("[msxml] save('%s') -> %s\n", path.c_str(),
                         ok ? "ok" : "failed");
            return ok ? S_OK_HR : E_FAIL_HR;
        }
        case 67: // get_validateOnParse (default true on 3.0 and 6.0)
            return out_bool(m, m.arg_ptr(1),
                            doc->properties().validate_on_parse)
                       ? S_OK_HR : E_POINTER_HR;
        case 68: // put_validateOnParse
            doc->properties().validate_on_parse = (m.arg32(1) & 0xFFFFu) != 0;
            return S_OK_HR;
        case 69: // get_resolveExternals (default true on 3.0, false on 6.0)
            return out_bool(m, m.arg_ptr(1),
                            doc->properties().resolve_externals)
                       ? S_OK_HR : E_POINTER_HR;
        case 70: // put_resolveExternals
            doc->properties().resolve_externals = (m.arg32(1) & 0xFFFFu) != 0;
            return S_OK_HR;
        case 71: // get_preserveWhiteSpace
            return out_bool(m, m.arg_ptr(1), doc->preserve_whitespace())
                       ? S_OK_HR : E_POINTER_HR;
        case 72: // put_preserveWhiteSpace
            doc->set_preserve_whitespace(
                (static_cast<u32>(m.arg(1)) & 0xFFFFu) != 0);
            return S_OK_HR;
        default:
            return E_NOTIMPL_HR;
    }
}

u32 MsxmlManager::dispatch_element(u32 slot, Marshaler& m, Object& self) {
    auto& node = self.node;
    if (!node) return E_FAIL_HR;
    const auto& doc = self.doc;

    switch (slot) {
        case 43: // get_tagName
            return out_bstr(m, m.arg_ptr(1), node->name()) ? S_OK_HR
                                                           : E_POINTER_HR;
        case 44: { // getAttribute(name, VARIANT* value)
            const std::string name = read_bstr(m, m.arg_ptr(1));
            auto attr = node->find_attribute(name);
            if (!attr) {
                // MSXML reports a missing attribute as VT_NULL with S_FALSE.
                return out_variant_null(m, m.arg_ptr(2)) ? S_FALSE_HR
                                                         : E_POINTER_HR;
            }
            return out_variant_bstr(m, m.arg_ptr(2), attr->value())
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 45: { // setAttribute(name, VARIANT value)
            const std::string name = read_bstr(m, m.arg_ptr(1));
            if (name.empty()) return E_INVALIDARG_HR;
            std::string value;
            if (m.cpu_is_64bit()) {
                const GuestAddr vt_addr = m.arg_ptr(2);
                auto vt = m.read_u16(vt_addr);
                if (vt.is_ok() && vt.value() == VT_BSTR_T) {
                    auto p = m.read_ptr(vt_addr + 8);
                    if (p.is_ok()) value = read_bstr(m, p.value());
                }
            } else {
                const u32 vt = static_cast<u32>(m.arg(2)) & 0xFFFFu;
                if (vt == VT_BSTR_T) value = read_bstr(m, m.arg_ptr(4));
            }
            if (!doc) return E_FAIL_HR;
            auto attr = node->find_attribute(name);
            if (attr) {
                attr->set_value(std::move(value));
                return S_OK_HR;
            }
            auto created = doc->create_attribute(name);
            created->set_value(std::move(value));
            (void)node->set_attribute_node(created);
            return S_OK_HR;
        }
        case 46: { // removeAttribute
            auto attr = node->find_attribute(read_bstr(m, m.arg_ptr(1)));
            if (!attr) return S_FALSE_HR;
            (void)node->remove_attribute_node(attr);
            return S_OK_HR;
        }
        case 47: { // getAttributeNode
            auto attr = node->find_attribute(read_bstr(m, m.arg_ptr(1)));
            return out_node(m, m.arg_ptr(2), attr, doc)
                       ? (attr ? S_OK_HR : S_FALSE_HR) : E_POINTER_HR;
        }
        case 48: { // setAttributeNode
            auto* attr = find(m.arg_ptr(1));
            if (attr == nullptr || !attr->node) return E_INVALIDARG_HR;
            auto displaced = node->set_attribute_node(attr->node);
            return out_node(m, m.arg_ptr(2), displaced, doc) ? S_OK_HR
                                                             : S_OK_HR;
        }
        case 49: { // removeAttributeNode
            auto* attr = find(m.arg_ptr(1));
            if (attr == nullptr || !attr->node) return E_INVALIDARG_HR;
            auto removed = node->remove_attribute_node(attr->node);
            if (!removed) return E_FAIL_HR;
            return out_node(m, m.arg_ptr(2), removed, doc) ? S_OK_HR : S_OK_HR;
        }
        case 50: { // getElementsByTagName
            std::vector<msxml::Node::Ptr> found;
            node->collect_elements_by_tag_name(read_bstr(m, m.arg_ptr(1)),
                                               found);
            const GuestAddr list = make_object(m, MsxmlIface::NodeList, doc);
            if (list == 0) return E_OUTOFMEMORY_HR;
            if (auto* state = find(list)) state->list = std::move(found);
            return m.write_ptr(m.arg_ptr(2), list).is_ok() ? S_OK_HR
                                                           : E_POINTER_HR;
        }
        case 51: // normalize — this parser already merges adjacent text.
            return S_OK_HR;
        default:
            return E_NOTIMPL_HR;
    }
}

u32 MsxmlManager::dispatch_character_data(u32 slot, Marshaler& m,
                                          Object& self) {
    auto& node = self.node;
    if (!node) return E_FAIL_HR;
    std::string data = node->value();

    switch (slot) {
        case 43: // get_data
            return out_bstr(m, m.arg_ptr(1), data) ? S_OK_HR : E_POINTER_HR;
        case 44: // put_data
            node->set_value(read_bstr(m, m.arg_ptr(1)));
            return S_OK_HR;
        case 45: // get_length (in UTF-16 units, which is what callers index by)
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(utf8_to_utf16(data).size()))
                       .is_ok() ? S_OK_HR : E_POINTER_HR;
        case 46: { // substringData(offset, count, out)
            const auto units = utf8_to_utf16(data);
            const auto offset = static_cast<usize>(m.arg32(1));
            const auto count  = static_cast<usize>(m.arg32(2));
            if (offset > units.size()) return E_INVALIDARG_HR;
            const usize take = std::min(count, units.size() - offset);
            std::vector<u16> slice(units.begin() + static_cast<isize>(offset),
                                   units.begin() +
                                       static_cast<isize>(offset + take));
            return out_bstr(m, m.arg_ptr(3), utf16_to_utf8(slice))
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 47: // appendData
            node->set_value(data + read_bstr(m, m.arg_ptr(1)));
            return S_OK_HR;
        case 48: { // insertData(offset, text)
            auto units = utf8_to_utf16(data);
            const auto offset = static_cast<usize>(m.arg32(1));
            if (offset > units.size()) return E_INVALIDARG_HR;
            const auto insert = utf8_to_utf16(read_bstr(m, m.arg_ptr(2)));
            units.insert(units.begin() + static_cast<isize>(offset),
                         insert.begin(), insert.end());
            node->set_value(utf16_to_utf8(units));
            return S_OK_HR;
        }
        case 49: { // deleteData(offset, count)
            auto units = utf8_to_utf16(data);
            const auto offset = static_cast<usize>(m.arg32(1));
            const auto count  = static_cast<usize>(m.arg32(2));
            if (offset > units.size()) return E_INVALIDARG_HR;
            const usize take = std::min(count, units.size() - offset);
            units.erase(units.begin() + static_cast<isize>(offset),
                        units.begin() + static_cast<isize>(offset + take));
            node->set_value(utf16_to_utf8(units));
            return S_OK_HR;
        }
        case 50: { // replaceData(offset, count, text)
            auto units = utf8_to_utf16(data);
            const auto offset = static_cast<usize>(m.arg32(1));
            const auto count  = static_cast<usize>(m.arg32(2));
            if (offset > units.size()) return E_INVALIDARG_HR;
            const usize take = std::min(count, units.size() - offset);
            units.erase(units.begin() + static_cast<isize>(offset),
                        units.begin() + static_cast<isize>(offset + take));
            const auto insert = utf8_to_utf16(read_bstr(m, m.arg_ptr(3)));
            units.insert(units.begin() + static_cast<isize>(offset),
                         insert.begin(), insert.end());
            node->set_value(utf16_to_utf8(units));
            return S_OK_HR;
        }
        case 51: { // splitText(offset, out) — IXMLDOMText only.
            auto units = utf8_to_utf16(data);
            const auto offset = static_cast<usize>(m.arg32(1));
            if (offset > units.size() || !self.doc) return E_INVALIDARG_HR;
            std::vector<u16> tail(units.begin() + static_cast<isize>(offset),
                                  units.end());
            units.resize(offset);
            node->set_value(utf16_to_utf8(units));
            auto sibling = self.doc->create_text_node(utf16_to_utf8(tail));
            if (auto parent = node->parent()) {
                (void)parent->insert_before(sibling, node->next_sibling());
            }
            return out_node(m, m.arg_ptr(2), sibling, self.doc) ? S_OK_HR
                                                                : E_POINTER_HR;
        }
        default:
            return E_NOTIMPL_HR;
    }
}

u32 MsxmlManager::dispatch_attribute(u32 slot, Marshaler& m, Object& self) {
    auto& node = self.node;
    if (!node) return E_FAIL_HR;
    switch (slot) {
        case 43: // get_name
            return out_bstr(m, m.arg_ptr(1), node->name()) ? S_OK_HR
                                                           : E_POINTER_HR;
        case 44: // get_value -> VARIANT
            return out_variant_bstr(m, m.arg_ptr(1), node->value())
                       ? S_OK_HR : E_POINTER_HR;
        case 45: { // put_value(VARIANT)
            std::string value;
            if (m.cpu_is_64bit()) {
                const GuestAddr vt_addr = m.arg_ptr(1);
                auto vt = m.read_u16(vt_addr);
                if (vt.is_ok() && vt.value() == VT_BSTR_T) {
                    auto p = m.read_ptr(vt_addr + 8);
                    if (p.is_ok()) value = read_bstr(m, p.value());
                }
            } else {
                const u32 vt = static_cast<u32>(m.arg(1)) & 0xFFFFu;
                if (vt == VT_BSTR_T) value = read_bstr(m, m.arg_ptr(3));
            }
            node->set_value(std::move(value));
            return S_OK_HR;
        }
        default:
            return E_NOTIMPL_HR;
    }
}

u32 MsxmlManager::dispatch_node_list(u32 slot, Marshaler& m, Object& self) {
    switch (slot) {
        case 7: { // get_item(index, out)
            const auto index = static_cast<usize>(m.arg32(1));
            if (index >= self.list.size()) {
                return m.write_ptr(m.arg_ptr(2), 0).is_ok() ? S_FALSE_HR
                                                            : E_POINTER_HR;
            }
            return out_node(m, m.arg_ptr(2), self.list[index], self.doc)
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 8: // get_length
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(self.list.size())).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        case 9: { // nextNode
            if (self.cursor >= self.list.size()) {
                return m.write_ptr(m.arg_ptr(1), 0).is_ok() ? S_FALSE_HR
                                                            : E_POINTER_HR;
            }
            auto node = self.list[self.cursor++];
            return out_node(m, m.arg_ptr(1), node, self.doc) ? S_OK_HR
                                                             : E_POINTER_HR;
        }
        case 10: // reset
            self.cursor = 0;
            return S_OK_HR;
        default:
            return E_NOTIMPL_HR;
    }
}

u32 MsxmlManager::dispatch_named_node_map(u32 slot, Marshaler& m,
                                          Object& self) {
    switch (slot) {
        case 7: { // getNamedItem(name, out)
            const std::string name = read_bstr(m, m.arg_ptr(1));
            for (const auto& attr : self.list) {
                if (attr->name() != name) continue;
                return out_node(m, m.arg_ptr(2), attr, self.doc) ? S_OK_HR
                                                                 : E_POINTER_HR;
            }
            return m.write_ptr(m.arg_ptr(2), 0).is_ok() ? S_FALSE_HR
                                                        : E_POINTER_HR;
        }
        case 8: { // setNamedItem(node, out)
            auto* attr = find(m.arg_ptr(1));
            if (attr == nullptr || !attr->node || !self.node) {
                return E_INVALIDARG_HR;
            }
            auto displaced = self.node->set_attribute_node(attr->node);
            self.list = self.node->attributes();
            return out_node(m, m.arg_ptr(2), displaced, self.doc) ? S_OK_HR
                                                                  : S_OK_HR;
        }
        case 9: { // removeNamedItem(name, out)
            if (!self.node) return E_INVALIDARG_HR;
            auto attr = self.node->find_attribute(read_bstr(m, m.arg_ptr(1)));
            if (!attr) {
                return m.write_ptr(m.arg_ptr(2), 0).is_ok() ? S_FALSE_HR
                                                            : E_POINTER_HR;
            }
            auto removed = self.node->remove_attribute_node(attr);
            self.list = self.node->attributes();
            return out_node(m, m.arg_ptr(2), removed, self.doc) ? S_OK_HR
                                                                : S_OK_HR;
        }
        case 10: { // get_item(index, out)
            const auto index = static_cast<usize>(m.arg32(1));
            if (index >= self.list.size()) {
                return m.write_ptr(m.arg_ptr(2), 0).is_ok() ? S_FALSE_HR
                                                            : E_POINTER_HR;
            }
            return out_node(m, m.arg_ptr(2), self.list[index], self.doc)
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 11: // get_length
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(self.list.size())).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        case 14: { // nextNode
            if (self.cursor >= self.list.size()) {
                return m.write_ptr(m.arg_ptr(1), 0).is_ok() ? S_FALSE_HR
                                                            : E_POINTER_HR;
            }
            auto node = self.list[self.cursor++];
            return out_node(m, m.arg_ptr(1), node, self.doc) ? S_OK_HR
                                                             : E_POINTER_HR;
        }
        case 15: // reset
            self.cursor = 0;
            return S_OK_HR;
        default:
            return E_NOTIMPL_HR;
    }
}

u32 MsxmlManager::dispatch_parse_error(u32 slot, Marshaler& m, Object& self) {
    if (!self.doc) return E_FAIL_HR;
    // A validate() result carries its own error and no URL.
    const auto& err = self.error ? *self.error : self.doc->parse_error();
    if (slot == 8 && self.error) {
        return out_bstr(m, m.arg_ptr(1), "") ? S_OK_HR : E_POINTER_HR;
    }
    switch (slot) {
        case 7: // get_errorCode
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(err.code)).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        case 8: // get_url
            return out_bstr(m, m.arg_ptr(1), self.doc->url()) ? S_OK_HR
                                                              : E_POINTER_HR;
        case 9: // get_reason
            return out_bstr(m, m.arg_ptr(1), err.reason) ? S_OK_HR
                                                         : E_POINTER_HR;
        case 10: // get_srcText
            return out_bstr(m, m.arg_ptr(1), err.source_text) ? S_OK_HR
                                                              : E_POINTER_HR;
        case 11: // get_line
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(err.line)).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        case 12: // get_linepos
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(err.line_position)).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        case 13: // get_filepos
            return m.write_u32(m.arg_ptr(1),
                               static_cast<u32>(err.file_position)).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        default:
            return E_NOTIMPL_HR;
    }
}

// ---------------------------------------------------------------------------
// IXMLDOMDocument2 / IXMLDOMDocument3
// ---------------------------------------------------------------------------
//
// Contract: msxml6.h vtable order; Learn's IXMLDOMDocument2 members and
// "Second-Level DOM Properties"; every case Learn leaves open measured on
// Windows 11 by tools/oracle_probe/msxml_document2_probe.c (x86 and x64 agree).

namespace {

enum class PropKind { Bool, Int, Language, Namespaces, SchemaLocations };

struct PropDesc {
    const char* name;
    PropKind    kind;
    bool        v3; ///< msxml3 knows it (msxml6 knows all of these).
};

// Names are case-sensitive and never trimmed (Learn; probe: "selectionlanguage"
// and "SELECTIONLANGUAGE" are unknown).
constexpr PropDesc kProps[] = {
    {"SelectionLanguage",            PropKind::Language,        true},
    {"SelectionNamespaces",          PropKind::Namespaces,      true},
    {"ServerHTTPRequest",            PropKind::Bool,            true},
    {"ProhibitDTD",                  PropKind::Bool,            true},
    {"AllowDocumentFunction",        PropKind::Bool,            true},
    {"AllowXsltScript",              PropKind::Bool,            true},
    {"MaxElementDepth",              PropKind::Int,             true},
    {"MaxXMLSize",                   PropKind::Int,             true},
    {"ForcedResync",                 PropKind::Bool,            true},
    {"NewParser",                    PropKind::Bool,            false},
    {"ResolveExternals",             PropKind::Bool,            false},
    {"ValidateOnParse",              PropKind::Bool,            false},
    {"UseInlineSchema",              PropKind::Bool,            false},
    {"MultipleErrorMessages",        PropKind::Bool,            false},
    {"NormalizeAttributeValues",     PropKind::Bool,            false},
    {"PopulateElementDefaultValues", PropKind::Bool,            false},
    {"SchemaLocations",              PropKind::SchemaLocations, false},
};

[[nodiscard]] const PropDesc* find_prop(std::string_view name, bool v6) {
    for (const auto& p : kProps) {
        if (name == p.name) return (v6 || p.v3) ? &p : nullptr;
    }
    return nullptr;
}

[[nodiscard]] bool* bool_slot(msxml::Document& doc, std::string_view name) {
    auto& s = doc.settings();
    auto& p = doc.properties();
    if (name == "ServerHTTPRequest")     return &p.server_http_request;
    if (name == "ProhibitDTD")           return &s.prohibit_dtd;
    if (name == "AllowDocumentFunction") return &p.allow_document_function;
    if (name == "AllowXsltScript")       return &p.allow_xslt_script;
    if (name == "ForcedResync")          return &p.forced_resync;
    if (name == "NewParser")             return &p.new_parser;
    // The first-level resolveExternals / validateOnParse are the same values.
    if (name == "ResolveExternals")      return &p.resolve_externals;
    if (name == "ValidateOnParse")       return &p.validate_on_parse;
    if (name == "UseInlineSchema")       return &p.use_inline_schema;
    if (name == "MultipleErrorMessages") return &p.multiple_error_messages;
    if (name == "NormalizeAttributeValues")
        return &s.normalize_attribute_values;
    if (name == "PopulateElementDefaultValues")
        return &p.populate_element_default_values;
    return nullptr;
}

[[nodiscard]] std::string trim(std::string_view text) {
    usize b = 0, e = text.size();
    while (b < e && (text[b] == ' ' || text[b] == '\t')) ++b;
    while (e > b && (text[e - 1] == ' ' || text[e - 1] == '\t')) --e;
    return std::string(text.substr(b, e - b));
}

/// Strict decimal/real parse of a whole string (VarR8FromStr's numeric form).
[[nodiscard]] std::optional<double> parse_number(std::string_view text) {
    const std::string t = trim(text);
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end == nullptr || *end != '\0') return std::nullopt;
    return v;
}

[[nodiscard]] bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

/// Numeric payload of a scalar VARIANT, or nullopt for a non-numeric type.
[[nodiscard]] std::optional<double> numeric_payload(u16 vt, u64 bits) {
    switch (vt) {
        case VT_EMPTY_T: return 0.0;
        case VT_I2_T:
        case VT_BOOL_T:  return static_cast<double>(static_cast<i16>(bits));
        case VT_I4_T:
        case VT_INT_T:   return static_cast<double>(static_cast<i32>(bits));
        case VT_UI4_T:
        case VT_UINT_T:  return static_cast<double>(static_cast<u32>(bits));
        case VT_I1_T:    return static_cast<double>(static_cast<i8>(bits));
        case VT_UI1_T:   return static_cast<double>(static_cast<u8>(bits));
        case VT_UI2_T:   return static_cast<double>(static_cast<u16>(bits));
        case VT_I8_T:    return static_cast<double>(static_cast<i64>(bits));
        case VT_UI8_T:   return static_cast<double>(bits);
        case VT_R4_T: {
            float f = 0;
            const u32 lo = static_cast<u32>(bits);
            std::memcpy(&f, &lo, sizeof f);
            return static_cast<double>(f);
        }
        case VT_R8_T: {
            double d = 0;
            std::memcpy(&d, &bits, sizeof d);
            return d;
        }
        default: return std::nullopt;
    }
}

[[nodiscard]] std::string vt_name(u16 vt) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "vt=0x%04x", vt);
    return buf;
}

/// Parse SelectionNamespaces: whitespace-separated `xmlns:p='uri'` /
/// `xmlns="uri"` declarations with quoted values. Anything else (another
/// attribute name, an unquoted value) is invalid: setProperty E_FAIL.
[[nodiscard]] bool parse_selection_namespaces(
    std::string_view text,
    std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    usize i = 0;
    while (true) {
        while (i < text.size() && space(text[i])) ++i;
        if (i >= text.size()) return true;
        const usize name_begin = i;
        while (i < text.size() && !space(text[i]) && text[i] != '=') ++i;
        const std::string_view name = text.substr(name_begin, i - name_begin);
        while (i < text.size() && space(text[i])) ++i;
        if (i >= text.size() || text[i] != '=') return false;
        ++i;
        while (i < text.size() && space(text[i])) ++i;
        if (i >= text.size() || (text[i] != '\'' && text[i] != '"')) return false;
        const char quote = text[i++];
        const usize value_begin = i;
        while (i < text.size() && text[i] != quote) ++i;
        if (i >= text.size()) return false;
        const std::string value(text.substr(value_begin, i - value_begin));
        ++i;
        if (name == "xmlns") continue; // default namespace: never applied
        if (name.rfind("xmlns:", 0) != 0 || name.size() == 6) return false;
        const std::string prefix(name.substr(6));
        bool replaced = false;
        for (auto& [p, uri] : out) {
            if (p == prefix) { uri = value; replaced = true; }
        }
        if (!replaced) out.emplace_back(prefix, value);
    }
}

} // namespace

msxml::XPathResult MsxmlManager::select(const Object& self,
                                        const std::string& expr) const {
    msxml::SelectionOptions options;
    if (self.doc) {
        const auto& props = self.doc->properties();
        options.xpath      = props.selection_language == "XPath";
        options.namespaces = props.selection_prefixes;
    }
    auto result = msxml::select_nodes(self.node, expr, options);
    if (!result.supported) {
        // The expression may be valid; this evaluator cannot run it. Fail
        // like an invalid one instead of answering "no matches".
        log_unimplemented("selectNodes",
                          "expression '" + expr + "' (" +
                              result.unsupported_reason + ")");
    } else if (result.invalid) {
        VITREO_TRACE("[msxml] select '%s' invalid: %s\n", expr.c_str(),
                     result.invalid_reason.c_str());
    }
    return result;
}

u32 MsxmlManager::get_property(Marshaler& m, msxml::Document& doc) {
    const std::string name = read_bstr(m, m.arg_ptr(1));
    const GuestAddr   out  = m.arg_ptr(2);
    const bool v6 = doc.settings().version == msxml::MsxmlVersion::V6;
    if (out == 0) return E_INVALIDARG_HR;
    const PropDesc* prop =
        m.arg_ptr(1) == 0 ? nullptr : find_prop(name, v6);
    if (prop == nullptr || prop->kind == PropKind::SchemaLocations) {
        // Unknown name (and a NULL one): E_FAIL. msxml6 also VariantInit's the
        // out VARIANT; msxml3 leaves it untouched. SchemaLocations reads back
        // the same way on msxml6 when nothing was set, and this component
        // never stores one (see set_property).
        if (v6) (void)out_variant_empty(m, out);
        return E_FAIL_HR;
    }
    switch (prop->kind) {
        case PropKind::Language:
            return out_variant_bstr(m, out, doc.properties().selection_language)
                       ? S_OK_HR : E_OUTOFMEMORY_HR;
        case PropKind::Namespaces:
            return out_variant_bstr(m, out,
                                    doc.properties().selection_namespaces)
                       ? S_OK_HR : E_OUTOFMEMORY_HR;
        case PropKind::Int:
            return out_variant_i4(m, out,
                                  name == "MaxElementDepth"
                                      ? doc.settings().max_element_depth
                                      : doc.settings().max_xml_size_kb)
                       ? S_OK_HR : E_POINTER_HR;
        case PropKind::Bool:
            return out_variant_bool(m, out, *bool_slot(doc, name))
                       ? S_OK_HR : E_POINTER_HR;
        case PropKind::SchemaLocations:
            break;
    }
    return E_FAIL_HR;
}

u32 MsxmlManager::set_property(Marshaler& m, msxml::Document& doc) {
    if (m.arg_ptr(1) == 0) return E_INVALIDARG_HR;
    const std::string name = read_bstr(m, m.arg_ptr(1));
    const VariantIn   value = read_variant_in(m, 2);
    const bool v6 = doc.settings().version == msxml::MsxmlVersion::V6;
    const PropDesc* prop = find_prop(name, v6);
    if (prop == nullptr) return E_FAIL_HR;

    // VariantChangeType-style coercions MSXML applies to the value.
    const auto as_string = [&](std::string& outs) -> u32 {
        if (value.vt == VT_BSTR_T) {
            outs = read_bstr(m, static_cast<GuestAddr>(
                                    m.cpu_is_64bit() ? value.bits
                                                     : value.bits & 0xFFFFFFFFu));
            return S_OK_HR;
        }
        if (value.vt == VT_EMPTY_T) { outs.clear(); return S_OK_HR; }
        if (value.vt == VT_I2_T || value.vt == VT_I4_T || value.vt == VT_INT_T ||
            value.vt == VT_UI1_T || value.vt == VT_UI2_T || value.vt == VT_UI4_T ||
            value.vt == VT_UINT_T || value.vt == VT_I1_T || value.vt == VT_I8_T ||
            value.vt == VT_UI8_T) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.0f",
                          *numeric_payload(value.vt, value.bits));
            outs = buf;
            return S_OK_HR;
        }
        if (value.vt == VT_NULL_T) return DISP_E_TYPEMISMATCH_HR;
        log_unimplemented("IXMLDOMDocument2::setProperty",
                          name + " value coercion from " + vt_name(value.vt) +
                              " to a string");
        return DISP_E_TYPEMISMATCH_HR;
    };
    const auto bstr_payload = [&]() -> std::string {
        return read_bstr(m, static_cast<GuestAddr>(
                                m.cpu_is_64bit() ? value.bits
                                                 : value.bits & 0xFFFFFFFFu));
    };

    switch (prop->kind) {
        case PropKind::Language: {
            // Exact, case-sensitive values; msxml6 dropped XSLPattern. A
            // non-string value never names a language.
            if (value.vt != VT_BSTR_T) return E_FAIL_HR;
            const std::string lang = bstr_payload();
            if (lang == "XPath" || (!v6 && lang == "XSLPattern")) {
                doc.properties().selection_language = lang;
                return S_OK_HR;
            }
            return E_FAIL_HR;
        }
        case PropKind::Namespaces: {
            std::string text;
            if (const u32 hr = as_string(text); hr != S_OK_HR) return hr;
            // msxml keeps the string even when it then rejects it (probe:
            // "garbage" and the coerced "1" read back after E_FAIL). Which
            // prefixes stay in force after a rejected value is not
            // observable through the probe; none are kept here.
            auto& props = doc.properties();
            props.selection_namespaces = text;
            if (!parse_selection_namespaces(text, props.selection_prefixes)) {
                props.selection_prefixes.clear();
                return E_FAIL_HR;
            }
            return S_OK_HR;
        }
        case PropKind::Int: {
            double number = 0;
            if (value.vt == VT_BSTR_T) {
                const auto parsed = parse_number(bstr_payload());
                if (!parsed) return DISP_E_TYPEMISMATCH_HR;
                number = *parsed;
            } else if (auto n = numeric_payload(value.vt, value.bits)) {
                number = *n;
            } else if (value.vt == VT_NULL_T) {
                return DISP_E_TYPEMISMATCH_HR;
            } else {
                log_unimplemented("IXMLDOMDocument2::setProperty",
                                  name + " value coercion from " +
                                      vt_name(value.vt) + " to VT_I4");
                return DISP_E_TYPEMISMATCH_HR;
            }
            const double rounded = std::nearbyint(number); // banker's rounding
            if (rounded < -2147483648.0 || rounded > 2147483647.0) {
                return DISP_E_OVERFLOW_HR;
            }
            const auto v = static_cast<i32>(rounded);
            // Negative limits are rejected (VARIANT_TRUE coerces to -1, which
            // is why a boolean fails with E_INVALIDARG); the documented
            // MaxXMLSize ceiling of 4194303 is not enforced by Windows.
            if (v < 0) return E_INVALIDARG_HR;
            if (name == "MaxElementDepth") {
                doc.settings().max_element_depth = v;
            } else {
                doc.settings().max_xml_size_kb = v;
            }
            return S_OK_HR;
        }
        case PropKind::Bool: {
            bool b = false;
            if (value.vt == VT_BSTR_T) {
                const std::string text = trim(bstr_payload());
                if (ieq(text, "true")) {
                    b = true;
                } else if (ieq(text, "false")) {
                    b = false;
                } else if (auto n = parse_number(text)) {
                    b = *n != 0.0;
                } else {
                    return DISP_E_TYPEMISMATCH_HR;
                }
            } else if (auto n = numeric_payload(value.vt, value.bits)) {
                b = *n != 0.0;
            } else if (value.vt == VT_NULL_T) {
                return DISP_E_TYPEMISMATCH_HR;
            } else {
                log_unimplemented("IXMLDOMDocument2::setProperty",
                                  name + " value coercion from " +
                                      vt_name(value.vt) + " to VT_BOOL");
                return DISP_E_TYPEMISMATCH_HR;
            }
            *bool_slot(doc, name) = b;
            return S_OK_HR;
        }
        case PropKind::SchemaLocations:
            // Setting it makes msxml6 locate and compile the named XSDs;
            // there is no schema compiler here.
            log_unimplemented("IXMLDOMDocument2::setProperty",
                              "SchemaLocations (XSD schema loading)");
            return E_FAIL_HR;
    }
    return E_FAIL_HR;
}

u32 MsxmlManager::out_validation_error(
    Marshaler& m, GuestAddr out, const std::shared_ptr<msxml::Document>& doc,
    msxml::ParseError error) {
    const bool v6 = doc->settings().version == msxml::MsxmlVersion::V6;
    if (out == 0) {
        // msxml3 still reports S_FALSE; msxml6 fails the call outright.
        return v6 ? E_FAIL_HR : S_FALSE_HR;
    }
    const GuestAddr err = make_object(m, MsxmlIface::ParseError, doc);
    if (err == 0) return E_OUTOFMEMORY_HR;
    if (auto* state = find(err)) state->error = std::move(error);
    return m.write_ptr(out, err).is_ok() ? S_FALSE_HR : E_POINTER_HR;
}

u32 MsxmlManager::dispatch_document2(
    u32 slot, Marshaler& m, [[maybe_unused]] Object& self,
    const std::shared_ptr<msxml::Document>& doc) {
    if (!doc) return E_FAIL_HR;
    const bool v6 = doc->settings().version == msxml::MsxmlVersion::V6;
    switch (slot) {
        case 76: { // get_namespaces(IXMLDOMSchemaCollection**)
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER_HR;
            // One live collection per document; it reflects the current tree.
            if (auto it = m_namespace_collections.find(doc.get());
                it != m_namespace_collections.end()) {
                if (auto* existing = find(it->second)) {
                    ++existing->refs;
                    return m.write_ptr(out, it->second).is_ok() ? S_OK_HR
                                                                : E_POINTER_HR;
                }
            }
            const GuestAddr coll =
                make_object(m, MsxmlIface::SchemaCollection, doc);
            if (coll == 0) return E_OUTOFMEMORY_HR;
            m_namespace_collections[doc.get()] = coll;
            return m.write_ptr(out, coll).is_ok() ? S_OK_HR : E_POINTER_HR;
        }
        case 77: { // get_schemas(VARIANT*)
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER_HR;
            // putref_schemas only ever accepts "no schemas" here.
            return out_variant_null(m, out) ? S_FALSE_HR : E_POINTER_HR;
        }
        case 78: { // putref_schemas(VARIANT)
            const VariantIn v = read_variant_in(m, 1);
            if (v.vt == VT_NULL_T || v.vt == VT_EMPTY_T) return S_OK_HR;
            if ((v.vt == VT_DISPATCH_T || v.vt == VT_UNKNOWN_T)) {
                if (v.bits == 0) return S_OK_HR;
                log_unimplemented("IXMLDOMDocument2::putref_schemas",
                                  "attaching an XMLSchemaCache");
                return E_NOTIMPL_HR;
            }
            return E_FAIL_HR;
        }
        case 79: { // validate(IXMLDOMParseError**)
            const GuestAddr out = m.arg_ptr(1);
            if (out != 0) (void)m.write_ptr(out, 0);
            msxml::ParseError err;
            if (!doc->document_element()) {
                err.code   = kValNotOneRoot;
                err.reason = "Validate failed because the document does not "
                             "contain exactly one root node.";
                return out_validation_error(m, out, doc, std::move(err));
            }
            if (doc->has_doctype()) {
                log_unimplemented("IXMLDOMDocument2::validate",
                                  "DTD validation");
                return E_NOTIMPL_HR;
            }
            // No DTD and no schema collection: neither valid nor invalid.
            err.code   = kValNoDtd;
            err.reason = v6 ? "The node is neither valid nor invalid because "
                              "no DTD/Schema declaration was found."
                            : "Validate failed because the root element had "
                              "no associated DTD/schema.";
            return out_validation_error(m, out, doc, std::move(err));
        }
        case 80: // setProperty(BSTR, VARIANT)
            return set_property(m, *doc);
        case 81: // getProperty(BSTR, VARIANT*)
            return get_property(m, *doc);
        case 82: { // validateNode(IXMLDOMNode*, IXMLDOMParseError**)
            auto* target = find(m.arg_ptr(1));
            const GuestAddr out = m.arg_ptr(2);
            if (target == nullptr || !target->node) return E_INVALIDARG_HR;
            if (out != 0) (void)m.write_ptr(out, 0);
            msxml::ParseError err;
            if (target->node->owner_document() != doc) {
                err.code   = kValForeignNode;
                err.reason = "Validate failed because the node provided does "
                             "not belong to this document.";
                return out_validation_error(m, out, doc, std::move(err));
            }
            if (doc->has_doctype()) {
                log_unimplemented("IXMLDOMDocument3::validateNode",
                                  "DTD validation");
                return E_NOTIMPL_HR;
            }
            if (target->node->type() != msxml::NodeType::Element) {
                log_unimplemented("IXMLDOMDocument3::validateNode",
                                  "non-element node without a DTD/schema");
                return E_NOTIMPL_HR;
            }
            err.code   = kValUndeclared;
            err.reason = "The element '" + target->node->name() +
                         "' is used but not declared in the DTD/Schema.";
            return out_validation_error(m, out, doc, std::move(err));
        }
        case 83: { // importNode(IXMLDOMNode*, VARIANT_BOOL deep, IXMLDOMNode**)
            auto* source = find(m.arg_ptr(1));
            const bool deep = (m.arg32(2) & 0xFFFFu) != 0;
            const GuestAddr out = m.arg_ptr(3);
            if (source == nullptr || !source->node || out == 0) {
                return E_INVALIDARG_HR;
            }
            if (source->node->type() == msxml::NodeType::Document) {
                return E_INVALIDARG_HR;
            }
            // A parentless copy owned by this document; the source is
            // untouched (importing from the same document also copies).
            auto copy = source->node->clone(deep);
            copy->adopt(doc);
            return out_node(m, out, copy, doc) ? S_OK_HR : E_OUTOFMEMORY_HR;
        }
        default:
            break;
    }
    return E_NOTIMPL_HR;
}

u32 MsxmlManager::dispatch_schema_collection(u32 slot, Marshaler& m,
                                             Object& self) {
    if (!self.doc) return E_FAIL_HR;
    const bool v6 = self.doc->settings().version == msxml::MsxmlVersion::V6;
    switch (slot) {
        case 7:  // add — the namespaces collection is read-only.
        case 9:  // remove
        case 12: // addCollection
            return E_FAIL_HR;
        case 8: { // get(BSTR uri, IXMLDOMNode** schema)
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) return E_POINTER_HR;
            (void)m.write_ptr(out, 0);
            // No schema is attached to a declared namespace: msxml3 answers
            // S_OK with NULL, msxml6 E_NOTIMPL.
            return v6 ? E_NOTIMPL_HR : S_OK_HR;
        }
        case 10: { // get_length
            const GuestAddr out = m.arg_ptr(1);
            if (out == 0) return E_POINTER_HR;
            const auto list = self.doc->declared_namespaces();
            return m.write_u32(out, static_cast<u32>(list.size())).is_ok()
                       ? S_OK_HR : E_POINTER_HR;
        }
        case 11: { // get_namespaceURI(long index, BSTR*)
            const auto index = static_cast<i32>(m.arg32(1));
            const GuestAddr out = m.arg_ptr(2);
            if (out == 0) return E_POINTER_HR;
            const auto list = self.doc->declared_namespaces();
            if (index < 0 || static_cast<usize>(index) >= list.size()) {
                if (v6) (void)m.write_ptr(out, 0);
                return E_FAIL_HR;
            }
            return out_bstr(m, out, list[static_cast<usize>(index)])
                       ? S_OK_HR : E_OUTOFMEMORY_HR;
        }
        case 13: { // get__newEnum
            const GuestAddr out = m.arg_ptr(1);
            if (out != 0) (void)m.write_ptr(out, 0);
            log_unimplemented("IXMLDOMSchemaCollection::get__newEnum",
                              "namespace enumerator");
            return E_NOTIMPL_HR;
        }
        default:
            return E_NOTIMPL_HR;
    }
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void register_msxml_thunks(DllExports& exports, MsxmlManager& mgr) {
    for (u32 i = 0; i < static_cast<u32>(MsxmlIface::Count); ++i) {
        const auto iface = static_cast<MsxmlIface>(i);
        const u32 count = MsxmlManager::slot_count(iface);
        for (u32 slot = 0; slot < count; ++slot) {
            exports.entries.push_back(ExportEntry{
                MsxmlManager::slot_thunk_name(iface, slot), 0,
                [&mgr, iface, slot](Marshaler& m) {
                    m.set_return(mgr.dispatch(iface, slot, m));
                },
                static_cast<u16>(
                    MsxmlManager::slot_stdcall_bytes(iface, slot))});
        }
    }
}

} // namespace vitreo::winapi

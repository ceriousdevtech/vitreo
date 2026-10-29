// SPDX-License-Identifier: LGPL-2.1-only

/// @file msxml_com.hpp
/// @brief Guest-visible MSXML COM objects over the host DOM.
///
/// Projects vitreo::msxml::Document / Node into the IXMLDOM* interfaces.
/// Every vtable and per-slot stdcall byte count is derived from the Windows
/// SDK's msxml.h, so slot order is contractual and must not be reordered.
///
/// The Document vtable carries the IXMLDOMDocument3 layout (84 slots): one
/// document object serves IXMLDOMDocument, IXMLDOMDocument2 (slots 76..81) and,
/// for MSXML 6.0 documents only, IXMLDOMDocument3 (82..83).
///
/// The IXMLDOM* node interfaces all inherit IXMLDOMNode, so slots 0..42 are
/// byte-identical across IXMLDOMNode, IXMLDOMDocument, IXMLDOMElement,
/// IXMLDOMCharacterData, IXMLDOMText and IXMLDOMAttribute. One shared
/// implementation therefore backs those slots for every node interface, with
/// per-interface methods appended above slot 42.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vitreo/types.hpp"
#include "winapi/com/msxml/xml_dom.hpp"
#include "winapi/com/msxml/xpath.hpp"
#include "winapi/core/marshaler.hpp"
#include "winapi/core/thunk_registry.hpp"
#include "winapi/com/ole32/com_core.hpp"  // Guid, DllExports users

namespace vitreo::capsule { class Vfs; }

namespace vitreo::winapi {

/// The interfaces this component hands out. The value doubles as the index into
/// the vtable descriptor table.
enum class MsxmlIface : u32 {
    Document = 0,
    Node,
    Element,
    Text,          ///< IXMLDOMText; also serves CDATA sections.
    CharacterData, ///< IXMLDOMComment.
    Attribute,
    ProcessingInstruction,
    NodeList,
    NamedNodeMap,
    ParseError,
    /// IXMLDOMSchemaCollection: the read-only namespaces collection
    /// IXMLDOMDocument2::get_namespaces returns.
    SchemaCollection,
    Count,
};

/// Implements the MSXML DOM component family (Msxml2.DOMDocument and friends).
class MsxmlManager {
public:
    /// Guest scratch used for interface objects and their vtables.
    static constexpr GuestAddr MSXML_SCRATCH_BASE = 0x5F400000u;
    static constexpr u32       MSXML_SCRATCH_SIZE = 0x00400000u; // 4 MiB

    void set_memory(GuestMemory* memory) noexcept { m_memory = memory; }
    void set_thunk_registry(ThunkRegistry* thunks) noexcept {
        m_thunks = thunks;
    }
    void set_vfs(capsule::Vfs* vfs) noexcept { m_vfs = vfs; }
    void set_scratch_base(GuestAddr base) noexcept { m_next_addr = base; }

    /// Supplies BSTR allocation, which lives in oleaut32. Set at registration
    /// time so this component does not depend on that manager's type.
    void set_bstr_allocator(
        std::function<GuestAddr(Marshaler&, const std::vector<u16>&)> alloc) {
        m_alloc_bstr = std::move(alloc);
    }

    /// True when @p clsid names one of the DOMDocument coclasses Windows
    /// registers (2.6 and 4.0 are not).
    [[nodiscard]] static bool is_dom_document_clsid(const Guid& clsid);
    /// The MSXML implementation behind a DOMDocument coclass.
    [[nodiscard]] static std::optional<msxml::MsxmlVersion>
    dom_document_version(const Guid& clsid);
    /// True when @p clsid names one of the XMLSchemaCache/XSLTemplate coclasses
    /// this component does not implement, so CoCreateInstance can fail with the
    /// documented HRESULT instead of "class not registered".
    [[nodiscard]] static bool is_unimplemented_msxml_clsid(const Guid& clsid);

    /// CoCreateInstance backend for a DOMDocument coclass. The document
    /// remembers @p clsid's MSXML version, which sets its property defaults
    /// and whether it serves IXMLDOMDocument3.
    [[nodiscard]] u32 create_document(Marshaler& m, const Guid& clsid,
                                      const Guid& iid, GuestAddr ppv);

    /// Vtable slot dispatch. @p iface selects the interface family.
    [[nodiscard]] u32 dispatch(MsxmlIface iface, u32 slot, Marshaler& m);

    /// Number of vtable slots for @p iface.
    [[nodiscard]] static u32 slot_count(MsxmlIface iface);
    /// stdcall bytes-to-pop for @p iface slot @p slot (x86 only).
    [[nodiscard]] static u32 slot_stdcall_bytes(MsxmlIface iface, u32 slot);
    /// Stable thunk export name for @p iface slot @p slot.
    [[nodiscard]] static std::string slot_thunk_name(MsxmlIface iface,
                                                     u32 slot);

private:
    /// One live guest interface pointer.
    struct Object {
        MsxmlIface                       iface = MsxmlIface::Node;
        u32                              refs  = 0;
        msxml::Node::Ptr                 node;   ///< Node-backed interfaces.
        std::shared_ptr<msxml::Document> doc;    ///< Keeps the tree alive.
        std::vector<msxml::Node::Ptr>    list;   ///< NodeList / NamedNodeMap.
        usize                            cursor = 0; ///< nextNode position.
        /// A ParseError object that reports a validate() result rather than
        /// the document's last load.
        std::optional<msxml::ParseError> error;
    };

    /// A by-value VARIANT argument: inline on x86, by hidden pointer on x64.
    struct VariantIn {
        u16 vt   = 0;
        u64 bits = 0;
    };
    [[nodiscard]] static VariantIn read_variant_in(Marshaler& m, u32 slot);

    [[nodiscard]] Object* find(GuestAddr iface_ptr);

    /// Allocate (or reuse) a guest interface pointer for @p node.
    [[nodiscard]] GuestAddr intern_node(Marshaler& m,
                                        const msxml::Node::Ptr& node,
                                        const std::shared_ptr<msxml::Document>& doc);
    /// Allocate a fresh collection or parse-error object.
    [[nodiscard]] GuestAddr make_object(Marshaler& m, MsxmlIface iface,
                                        const std::shared_ptr<msxml::Document>& doc);

    [[nodiscard]] GuestAddr build_vtable(Marshaler& m, MsxmlIface iface);
    [[nodiscard]] bool write_object(Marshaler& m, GuestAddr object,
                                    MsxmlIface iface);

    /// Which interface MSXML hands out for a node of this type.
    [[nodiscard]] static MsxmlIface iface_for(msxml::NodeType type);

    // -- shared IXMLDOMNode slots 0..42 --------------------------------------
    [[nodiscard]] u32 dispatch_node(u32 slot, Marshaler& m, Object& self);
    [[nodiscard]] u32 dispatch_document(u32 slot, Marshaler& m, Object& self);
    [[nodiscard]] u32 dispatch_element(u32 slot, Marshaler& m, Object& self);
    [[nodiscard]] u32 dispatch_character_data(u32 slot, Marshaler& m,
                                              Object& self);
    [[nodiscard]] u32 dispatch_attribute(u32 slot, Marshaler& m, Object& self);
    [[nodiscard]] u32 dispatch_node_list(u32 slot, Marshaler& m, Object& self);
    [[nodiscard]] u32 dispatch_named_node_map(u32 slot, Marshaler& m,
                                              Object& self);
    [[nodiscard]] u32 dispatch_parse_error(u32 slot, Marshaler& m,
                                           Object& self);
    [[nodiscard]] u32 dispatch_document2(u32 slot, Marshaler& m, Object& self,
                                         const std::shared_ptr<msxml::Document>& doc);
    [[nodiscard]] u32 dispatch_schema_collection(u32 slot, Marshaler& m,
                                                 Object& self);
    [[nodiscard]] u32 get_property(Marshaler& m, msxml::Document& doc);
    [[nodiscard]] u32 set_property(Marshaler& m, msxml::Document& doc);
    /// selectNodes / selectSingleNode with the owning document's selection
    /// language and namespaces.
    [[nodiscard]] msxml::XPathResult select(const Object& self,
                                            const std::string& expr) const;
    /// A validate()/validateNode() outcome as a fresh IXMLDOMParseError.
    [[nodiscard]] u32 out_validation_error(Marshaler& m, GuestAddr out,
                                           const std::shared_ptr<msxml::Document>& doc,
                                           msxml::ParseError error);

    [[nodiscard]] u32 query_interface(Marshaler& m, Object& self);
    [[nodiscard]] u32 release(Marshaler& m, GuestAddr self_ptr);

    // -- marshaling helpers ---------------------------------------------------
    [[nodiscard]] bool out_bstr(Marshaler& m, GuestAddr out,
                                std::string_view utf8);
    [[nodiscard]] bool out_node(Marshaler& m, GuestAddr out,
                                const msxml::Node::Ptr& node,
                                const std::shared_ptr<msxml::Document>& doc);
    [[nodiscard]] bool out_variant_bstr(Marshaler& m, GuestAddr out,
                                        std::string_view utf8);
    [[nodiscard]] bool out_variant_null(Marshaler& m, GuestAddr out);
    [[nodiscard]] bool out_bool(Marshaler& m, GuestAddr out, bool value);
    [[nodiscard]] bool out_variant_bool(Marshaler& m, GuestAddr out,
                                        bool value);
    [[nodiscard]] bool out_variant_i4(Marshaler& m, GuestAddr out, i32 value);
    [[nodiscard]] bool out_variant_empty(Marshaler& m, GuestAddr out);
    [[nodiscard]] std::string read_bstr(Marshaler& m, GuestAddr bstr) const;

    [[nodiscard]] std::optional<std::vector<u8>> read_guest_file(
        const std::string& path) const;
    [[nodiscard]] bool write_guest_file(const std::string& path,
                                        std::string_view text) const;

    GuestMemory*    m_memory  = nullptr;
    ThunkRegistry*  m_thunks  = nullptr;
    capsule::Vfs*   m_vfs     = nullptr;
    GuestAddr       m_next_addr = 0;
    GuestAddr       m_scratch_end = 0;
    std::function<GuestAddr(Marshaler&, const std::vector<u16>&)> m_alloc_bstr;

    std::unordered_map<GuestAddr, Object> m_objects;
    /// Node identity reuse: the same node asked for twice yields the same
    /// pointer with an extra reference, which bounds arena growth.
    std::unordered_map<const msxml::Node*, GuestAddr> m_node_to_iface;
    /// get_namespaces hands out one collection object per document.
    std::unordered_map<const msxml::Document*, GuestAddr> m_namespace_collections;
    /// Vtables are shared by every object of an interface family.
    GuestAddr m_vtables[static_cast<usize>(MsxmlIface::Count)] = {};
};

/// Register msxml3.dll / msxml6.dll exports and the per-slot vtable thunks.
void register_msxml_thunks(DllExports& exports, MsxmlManager& mgr);

} // namespace vitreo::winapi

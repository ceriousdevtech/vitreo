// SPDX-License-Identifier: LGPL-2.1-only

/// @file xml_dom.hpp
/// @brief Host-side XML 1.0 document model backing MSXML's IXMLDOM* interfaces.
///
/// This layer is deliberately free of guest/COM concepts: it parses XML text
/// into a DOM tree, serializes it back, and reports parse failures the way
/// MSXML's IXMLDOMParseError does (code, reason, line, line position, file
/// position). The COM plumbing in msxml_com.cpp projects this model into guest
/// memory.
///
/// Contract sources: W3C DOM Level 1 Core (node types, tree invariants) and the
/// MSXML IXMLDOMNode / IXMLDOMDocument reference on Microsoft Learn. Slot order
/// and HRESULT behaviour live with the COM layer, not here.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "vitreo/types.hpp"

namespace vitreo::msxml {

/// DOMNodeType (msxml.h). Values are guest-visible through get_nodeType, so the
/// numbering is contractual, not an implementation detail.
enum class NodeType : i32 {
    Invalid               = 0,
    Element               = 1,
    Attribute             = 2,
    Text                  = 3,
    CDATASection          = 4,
    EntityReference       = 5,
    Entity                = 6,
    ProcessingInstruction = 7,
    Comment               = 8,
    Document              = 9,
    DocumentType          = 10,
    DocumentFragment      = 11,
    Notation              = 12,
};

/// MSXML's nodeTypeString values, e.g. "element", "text". Documented per node
/// type on Learn; callers switch on these strings.
[[nodiscard]] const char* node_type_string(NodeType type);

class Document;

/// One DOM node. Children are owned; the parent link is weak so that detaching
/// a subtree cannot leave a dangling owner.
///
/// Attributes are kept in their own list rather than in @ref children because
/// the DOM does not treat them as children of their element, but they are
/// reachable through get_attributes and must carry a parent link.
class Node : public std::enable_shared_from_this<Node> {
public:
    using Ptr = std::shared_ptr<Node>;

    Node(NodeType type, std::string name, std::string value);
    virtual ~Node() = default;

    [[nodiscard]] NodeType type() const noexcept { return m_type; }

    /// Qualified name ("ns:local" when prefixed). MSXML's get_nodeName.
    [[nodiscard]] const std::string& name() const noexcept { return m_name; }

    /// get_nodeValue. Null for element/document nodes, which the COM layer
    /// reports as VT_NULL rather than an empty string.
    [[nodiscard]] const std::string& value() const noexcept { return m_value; }
    void set_value(std::string value) { m_value = std::move(value); }

    /// True when get_nodeValue must yield VT_NULL. Elements, documents,
    /// fragments and doctypes have no value; everything else does.
    [[nodiscard]] bool has_null_value() const noexcept;

    /// Namespace prefix ("ns" in "ns:local"), empty when unprefixed.
    [[nodiscard]] std::string prefix() const;
    /// Local name without the prefix. MSXML's get_baseName.
    [[nodiscard]] std::string base_name() const;
    /// Resolved namespace URI, or empty when the node is in no namespace.
    [[nodiscard]] const std::string& namespace_uri() const noexcept {
        return m_namespace_uri;
    }
    void set_namespace_uri(std::string uri) {
        m_namespace_uri = std::move(uri);
    }

    [[nodiscard]] const std::vector<Ptr>& children() const noexcept {
        return m_children;
    }
    [[nodiscard]] const std::vector<Ptr>& attributes() const noexcept {
        return m_attributes;
    }

    [[nodiscard]] Ptr parent() const { return m_parent.lock(); }
    [[nodiscard]] std::shared_ptr<Document> owner_document() const {
        return m_owner.lock();
    }
    void set_owner_document(const std::shared_ptr<Document>& doc) {
        m_owner = doc;
    }

    /// Sibling navigation. Attribute nodes report no siblings, matching MSXML.
    [[nodiscard]] Ptr previous_sibling() const;
    [[nodiscard]] Ptr next_sibling() const;

    [[nodiscard]] Ptr first_child() const {
        return m_children.empty() ? nullptr : m_children.front();
    }
    [[nodiscard]] Ptr last_child() const {
        return m_children.empty() ? nullptr : m_children.back();
    }

    /// appendChild. Detaches @p child from any previous parent first, as the
    /// DOM requires. Returns false when the insertion would build a cycle or
    /// the node type cannot hold children.
    bool append_child(const Ptr& child);
    /// insertBefore(child, ref). A null @p ref appends.
    bool insert_before(const Ptr& child, const Ptr& ref);
    /// replaceChild(new, old). Returns false when @p old is not a child.
    bool replace_child(const Ptr& new_child, const Ptr& old_child);
    /// removeChild. Returns false when @p child is not a child of this node.
    bool remove_child(const Ptr& child);
    /// Detach from whatever parent (or owning element, for attributes) holds
    /// this node.
    void detach_from_parent();

    /// Attribute access by qualified name. Elements only.
    [[nodiscard]] Ptr find_attribute(std::string_view qualified_name) const;
    /// setAttributeNode semantics: replaces a same-named attribute and returns
    /// the one displaced, if any.
    Ptr set_attribute_node(const Ptr& attr);
    Ptr remove_attribute_node(const Ptr& attr);

    /// get_text: concatenated descendant character data, skipping comments and
    /// processing instructions the way MSXML does.
    [[nodiscard]] std::string text() const;
    /// put_text: replaces all children with a single text node.
    void set_text(std::string value);

    /// get_xml: the node serialized back to XML.
    [[nodiscard]] std::string xml() const;

    /// cloneNode. @p deep also copies descendants and attributes.
    [[nodiscard]] Ptr clone(bool deep) const;

    /// Make @p doc the ownerDocument of this node, its attributes and every
    /// descendant (IXMLDOMDocument3::importNode).
    void adopt(const std::shared_ptr<Document>& doc);

    /// True for an attribute that declares a namespace (`xmlns`, `xmlns:p`).
    [[nodiscard]] bool is_namespace_declaration() const;

    /// getElementsByTagName, in document order. "*" matches every element.
    void collect_elements_by_tag_name(std::string_view tag,
                                      std::vector<Ptr>& out) const;

    /// True when this node type may contain children at all.
    [[nodiscard]] bool can_have_children() const noexcept;

private:
    friend class Document;

    /// True when @p candidate is this node or one of its ancestors, which would
    /// make an insertion cyclic.
    [[nodiscard]] bool is_inclusive_ancestor_of(const Node& candidate) const;

    NodeType    m_type;
    std::string m_name;
    std::string m_value;
    std::string m_namespace_uri;

    std::vector<Ptr>      m_children;
    std::vector<Ptr>      m_attributes;
    std::weak_ptr<Node>   m_parent;
    std::weak_ptr<Document> m_owner;
};

/// IXMLDOMParseError's payload. @ref code is 0 exactly when the parse succeeded.
struct ParseError {
    i32         code = 0;
    std::string reason;
    std::string source_text;
    i32         line = 0;
    i32         line_position = 0;
    i32         file_position = 0;

    [[nodiscard]] bool failed() const noexcept { return code != 0; }
};

/// Which MSXML implementation a document follows. Windows serves every
/// version-independent and 3.0 DOMDocument CLSID from msxml3 and the 6.0 ones
/// from msxml6 (2.6 and 4.0 are not registered;
/// tools/oracle_probe/msxml_document2_probe.expected).
enum class MsxmlVersion : u8 { V3, V6 };

/// The parser-facing second-level DOM properties (IXMLDOMDocument2
/// setProperty). Defaults here are a neutral parser; the COM layer applies the
/// per-version defaults when it creates a document.
struct ParseSettings {
    MsxmlVersion version = MsxmlVersion::V6;
    /// ProhibitDTD: any DOCTYPE fails the load.
    bool prohibit_dtd = false;
    /// MaxElementDepth: 0 = unlimited; the root element is depth 1.
    i32 max_element_depth = 0;
    /// MaxXMLSize in blocks of 1024 characters; 0 = unlimited.
    i32 max_xml_size_kb = 0;
    /// NormalizeAttributeValues (6.0): literal tab/CR/LF in an attribute
    /// value become spaces; character references are kept.
    bool normalize_attribute_values = false;
};

/// The DOM properties that do not steer the parser: the first-level
/// async/validateOnParse/resolveExternals and the remaining second-level
/// (setProperty) values. The COM layer applies per-version defaults.
struct DomProperties {
    std::string selection_language = "XPath";
    /// SelectionNamespaces exactly as last set (stored even when invalid).
    std::string selection_namespaces;
    /// The prefixed declarations parsed out of @ref selection_namespaces.
    std::vector<std::pair<std::string, std::string>> selection_prefixes;
    bool async                          = true;
    bool validate_on_parse              = true;
    bool resolve_externals              = false;
    bool server_http_request            = false;
    bool allow_document_function        = false;
    bool allow_xslt_script              = false;
    bool forced_resync                  = true;
    bool new_parser                     = false;
    bool use_inline_schema              = false;
    bool multiple_error_messages        = false;
    bool populate_element_default_values = false;
};

/// A parsed document. Owns the tree and the outcome of the last load.
///
/// A Document must be owned by a `shared_ptr` — the parser and every createXxx
/// helper take `shared_from_this()` to set each new node's ownerDocument. Use
/// @ref create rather than constructing one on the stack.
class Document : public Node {
public:
    Document();

    /// The only supported way to build a Document; see the class note.
    [[nodiscard]] static std::shared_ptr<Document> create() {
        return std::make_shared<Document>();
    }

    /// loadXML. Replaces the current tree. Returns false on a parse error, in
    /// which case @ref parse_error carries the MSXML-shaped diagnosis and the
    /// tree is left empty.
    bool load_xml(std::string_view text);

    [[nodiscard]] const ParseError& parse_error() const noexcept {
        return m_parse_error;
    }
    void set_parse_error(ParseError error) {
        m_parse_error = std::move(error);
    }

    /// The single element child, if the document has one.
    [[nodiscard]] Ptr document_element() const;

    /// The URL the document was loaded from, empty for loadXML.
    [[nodiscard]] const std::string& url() const noexcept { return m_url; }
    void set_url(std::string url) { m_url = std::move(url); }

    /// preserveWhiteSpace. When false (the MSXML default) whitespace-only text
    /// nodes between elements are dropped at parse time.
    [[nodiscard]] bool preserve_whitespace() const noexcept {
        return m_preserve_whitespace;
    }
    void set_preserve_whitespace(bool value) {
        m_preserve_whitespace = value;
    }

    [[nodiscard]] ParseSettings& settings() noexcept { return m_settings; }
    [[nodiscard]] const ParseSettings& settings() const noexcept {
        return m_settings;
    }
    [[nodiscard]] DomProperties& properties() noexcept { return m_properties; }
    [[nodiscard]] const DomProperties& properties() const noexcept {
        return m_properties;
    }

    /// True when the last successful load contained a DOCTYPE declaration.
    [[nodiscard]] bool has_doctype() const noexcept { return m_has_doctype; }
    void set_has_doctype(bool value) noexcept { m_has_doctype = value; }

    /// Namespace URIs declared anywhere in the tree, in document order without
    /// duplicates (IXMLDOMDocument2::namespaces). MSXML 3.0 omits the empty
    /// URI an `xmlns=""` undeclaration names; 6.0 lists it.
    [[nodiscard]] std::vector<std::string> declared_namespaces() const;

    /// Factory helpers backing the createXxx methods. Each returns a parentless
    /// node already owned by this document.
    [[nodiscard]] Ptr create_element(std::string name);
    [[nodiscard]] Ptr create_text_node(std::string data);
    [[nodiscard]] Ptr create_comment(std::string data);
    [[nodiscard]] Ptr create_cdata_section(std::string data);
    [[nodiscard]] Ptr create_processing_instruction(std::string target,
                                                    std::string data);
    [[nodiscard]] Ptr create_attribute(std::string name);
    [[nodiscard]] Ptr create_document_fragment();
    [[nodiscard]] Ptr create_entity_reference(std::string name);

private:
    ParseError  m_parse_error;
    std::string m_url;
    bool        m_preserve_whitespace = false;
    bool        m_has_doctype = false;
    ParseSettings m_settings;
    DomProperties m_properties;
};

/// Parse @p text into @p doc. Exposed separately so tests can drive the parser
/// without a Document, and so the COM layer can reuse it for load-from-file.
bool parse_document(std::string_view text, Document& doc);

/// XML-escape @p text for use as element content (@p in_attribute false) or as
/// an attribute value (@p in_attribute true).
[[nodiscard]] std::string escape_xml(std::string_view text, bool in_attribute);

} // namespace vitreo::msxml

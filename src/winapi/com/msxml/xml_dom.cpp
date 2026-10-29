// SPDX-License-Identifier: LGPL-2.1-only

/// @file xml_dom.cpp
/// @brief XML 1.0 parser, DOM tree, and serializer behind MSXML.
///
/// The parse-error codes, reason strings, and line/linepos/filepos conventions
/// are taken from the real MSXML6 implementation on the reference Windows host
/// (see tests/winapi/msxml/oracle_parse_errors.tsv, captured with
/// oracle_parse_error_probe.ps1). `line` and `linepos` are 1-based while
/// `filepos` is the 0-based byte offset, so linepos is not simply filepos + 1
/// once a newline has been crossed.

#include "winapi/com/msxml/xml_dom.hpp"

#include <algorithm>
#include <cctype>

namespace vitreo::msxml {

namespace {

// MSXML parse-error HRESULTs, captured from MSXML6 rather than recalled.
constexpr i32 kErrUndefinedEntity   = static_cast<i32>(0xC00CE002u);
constexpr i32 kErrMissingQuote      = static_cast<i32>(0xC00CE502u);
constexpr i32 kErrCommentSyntax     = static_cast<i32>(0xC00CE503u);
constexpr i32 kErrBadStartNameChar  = static_cast<i32>(0xC00CE504u);
constexpr i32 kErrBadNameChar       = static_cast<i32>(0xC00CE505u);
constexpr i32 kErrBadCharInString   = static_cast<i32>(0xC00CE506u);
constexpr i32 kErrMissingSemicolon  = static_cast<i32>(0xC00CE50Du);
constexpr i32 kErrBadEntityChar     = static_cast<i32>(0xC00CE50Eu);
constexpr i32 kErrMultipleColons    = static_cast<i32>(0xC00CE51Cu);
constexpr i32 kErrMultipleRoots     = static_cast<i32>(0xC00CE555u);
constexpr i32 kErrInvalidAtTopLevel = static_cast<i32>(0xC00CE556u);
constexpr i32 kErrMissingRoot       = static_cast<i32>(0xC00CE558u);
constexpr i32 kErrUnclosedCData     = static_cast<i32>(0xC00CE564u);
constexpr i32 kErrElementNotClosed  = static_cast<i32>(0xC00CE55Eu);
constexpr i32 kErrTagMismatch       = static_cast<i32>(0xC00CE56Du);

// Second-level property limits (tools/oracle_probe/msxml_document2_probe
// .expected). MSXML 3.0 reports its limits as E_ABORT "System error"s; 6.0 has
// dedicated codes, and its ProhibitDTD code differs from 3.0's top-level error.
constexpr i32 kErrAbortV3           = static_cast<i32>(0x80004004u);
constexpr i32 kErrDtdProhibited     = static_cast<i32>(0xC00CE584u);
constexpr i32 kErrMaxXmlSize        = static_cast<i32>(0xC00CE585u);
constexpr i32 kErrMaxElementDepth   = static_cast<i32>(0xC00CE586u);

constexpr const char* kXmlNamespace   = "http://www.w3.org/XML/1998/namespace";
constexpr const char* kXmlnsNamespace = "http://www.w3.org/2000/xmlns/";

[[nodiscard]] bool is_xml_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// XML 1.0 NameStartChar, restricted to the ASCII range plus any byte with the
/// high bit set. Treating all non-ASCII bytes as name characters keeps UTF-8
/// names working without decoding, and XML forbids no ASCII-adjacent byte that
/// this would wrongly admit.
[[nodiscard]] bool is_name_start_char(unsigned char c) {
    return std::isalpha(c) != 0 || c == '_' || c == ':' || c >= 0x80;
}

[[nodiscard]] bool is_name_char(unsigned char c) {
    return is_name_start_char(c) || std::isdigit(c) != 0 || c == '-' ||
           c == '.';
}

/// Walks a buffer while tracking the 1-based line/column MSXML reports.
class Cursor {
public:
    explicit Cursor(std::string_view text) : m_text(text) {}

    [[nodiscard]] bool at_end() const { return m_pos >= m_text.size(); }
    [[nodiscard]] usize offset() const { return m_pos; }
    [[nodiscard]] i32 line() const { return m_line; }
    [[nodiscard]] i32 column() const { return m_column; }

    [[nodiscard]] char peek(usize ahead = 0) const {
        const usize at = m_pos + ahead;
        return at < m_text.size() ? m_text[at] : '\0';
    }

    [[nodiscard]] bool starts_with(std::string_view s) const {
        return m_text.substr(m_pos).rfind(s, 0) == 0;
    }

    char advance() {
        if (at_end()) return '\0';
        const char c = m_text[m_pos++];
        // A CRLF pair counts as one line break; a lone CR also ends a line.
        if (c == '\n') {
            ++m_line;
            m_column = 1;
        } else if (c == '\r') {
            if (peek() != '\n') {
                ++m_line;
                m_column = 1;
            }
        } else {
            ++m_column;
        }
        return c;
    }

    void skip(usize count) {
        for (usize i = 0; i < count && !at_end(); ++i) (void)advance();
    }

    void skip_space() {
        while (!at_end() && is_xml_space(peek())) (void)advance();
    }

    [[nodiscard]] std::string_view rest() const { return m_text.substr(m_pos); }

private:
    std::string_view m_text;
    usize m_pos    = 0;
    i32   m_line   = 1;
    i32   m_column = 1;
};

/// Recursive-descent XML 1.0 parser producing DOM nodes.
class Parser {
public:
    Parser(std::string_view text, Document& doc)
        : m_cursor(text), m_doc(doc), m_text(text) {}

    bool run();

    [[nodiscard]] const ParseError& error() const { return m_error; }

private:
    /// Records the first failure and stops the parse. Later calls are ignored
    /// so the reported position stays at the original fault.
    bool fail(i32 code, std::string reason);

    /// Fails at a position captured before the offending token was consumed.
    bool fail_at(i32 code, std::string reason, i32 line, i32 column,
                 usize offset);

    [[nodiscard]] std::string current_line_text() const;

    bool parse_prolog();
    bool parse_misc(Node& parent, bool at_root);
    bool parse_element(const Node::Ptr& parent);
    bool parse_content(const Node::Ptr& element);
    bool parse_comment(const Node::Ptr& parent);
    bool parse_processing_instruction(const Node::Ptr& parent);
    bool parse_cdata(const Node::Ptr& parent);
    bool parse_doctype();
    bool parse_name(std::string& out);
    bool parse_attribute_value(std::string& out);
    /// Assign namespace URIs to @p element and its attributes from the
    /// declarations in scope (its own, then its ancestors' from @p parent).
    void resolve_namespaces(const Node::Ptr& element, const Node::Ptr& parent);
    /// Expands &amp; &lt; &gt; &apos; &quot; and numeric character references.
    bool expand_reference(std::string& out);

    Cursor           m_cursor;
    Document&        m_doc;
    std::string_view m_text;
    ParseError       m_error;
    bool             m_failed = false;
    i32              m_depth  = 0;
};

bool Parser::fail(i32 code, std::string reason) {
    return fail_at(code, std::move(reason), m_cursor.line(), m_cursor.column(),
                   m_cursor.offset());
}

bool Parser::fail_at(i32 code, std::string reason, i32 line, i32 column,
                     usize offset) {
    if (m_failed) return false;
    m_failed             = true;
    m_error.code         = code;
    m_error.reason       = std::move(reason);
    m_error.line         = line;
    m_error.line_position = column;
    m_error.file_position = static_cast<i32>(offset);
    m_error.source_text  = current_line_text();
    return false;
}

std::string Parser::current_line_text() const {
    const usize at = std::min(m_error.file_position < 0
                                  ? usize{0}
                                  : static_cast<usize>(m_error.file_position),
                              m_text.size());
    usize begin = at;
    while (begin > 0 && m_text[begin - 1] != '\n' && m_text[begin - 1] != '\r') {
        --begin;
    }
    usize end = at;
    while (end < m_text.size() && m_text[end] != '\n' && m_text[end] != '\r') {
        ++end;
    }
    return std::string(m_text.substr(begin, end - begin));
}

bool Parser::parse_name(std::string& out) {
    out.clear();
    if (m_cursor.at_end() ||
        !is_name_start_char(static_cast<unsigned char>(m_cursor.peek()))) {
        return fail(kErrBadStartNameChar,
                    "A name was started with an invalid character.");
    }
    i32   colons = 0;
    usize colon_offset = 0;
    i32   colon_line = 0, colon_column = 0;
    while (!m_cursor.at_end() &&
           is_name_char(static_cast<unsigned char>(m_cursor.peek()))) {
        if (m_cursor.peek() == ':') {
            ++colons;
            if (colons == 2) {
                colon_offset = m_cursor.offset();
                colon_line   = m_cursor.line();
                colon_column = m_cursor.column();
            }
        }
        out.push_back(m_cursor.advance());
    }
    if (colons >= 2) {
        return fail_at(kErrMultipleColons,
                       "Multiple colons are not allowed in a name.",
                       colon_line, colon_column, colon_offset);
    }
    return true;
}

bool Parser::expand_reference(std::string& out) {
    // Positioned on '&'.
    const i32   amp_line   = m_cursor.line();
    const i32   amp_column = m_cursor.column();
    const usize amp_offset = m_cursor.offset();
    (void)m_cursor.advance();

    if (m_cursor.peek() == '#') {
        (void)m_cursor.advance();
        u32  value = 0;
        bool hex   = false;
        if (m_cursor.peek() == 'x' || m_cursor.peek() == 'X') {
            hex = true;
            (void)m_cursor.advance();
        }
        bool any = false;
        while (!m_cursor.at_end() && m_cursor.peek() != ';') {
            const char c = m_cursor.peek();
            u32 digit = 0;
            if (c >= '0' && c <= '9') {
                digit = static_cast<u32>(c - '0');
            } else if (hex && c >= 'a' && c <= 'f') {
                digit = static_cast<u32>(c - 'a') + 10u;
            } else if (hex && c >= 'A' && c <= 'F') {
                digit = static_cast<u32>(c - 'A') + 10u;
            } else {
                return fail(kErrBadEntityChar,
                            "An invalid character was found inside an entity "
                            "reference.");
            }
            value = value * (hex ? 16u : 10u) + digit;
            any   = true;
            (void)m_cursor.advance();
        }
        if (!any || m_cursor.peek() != ';') {
            return fail(kErrMissingSemicolon,
                        "A semi colon character was expected.");
        }
        (void)m_cursor.advance();
        // Encode the code point as UTF-8; the DOM stores UTF-8 throughout.
        if (value < 0x80u) {
            out.push_back(static_cast<char>(value));
        } else if (value < 0x800u) {
            out.push_back(static_cast<char>(0xC0u | (value >> 6)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        } else if (value < 0x10000u) {
            out.push_back(static_cast<char>(0xE0u | (value >> 12)));
            out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        } else {
            out.push_back(static_cast<char>(0xF0u | (value >> 18)));
            out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
            out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
        }
        return true;
    }

    std::string name;
    while (!m_cursor.at_end() && m_cursor.peek() != ';' &&
           is_name_char(static_cast<unsigned char>(m_cursor.peek()))) {
        name.push_back(m_cursor.advance());
    }
    if (m_cursor.peek() != ';') {
        return fail(kErrMissingSemicolon,
                    "A semi colon character was expected.");
    }
    (void)m_cursor.advance();

    if (name == "amp")       out.push_back('&');
    else if (name == "lt")   out.push_back('<');
    else if (name == "gt")   out.push_back('>');
    else if (name == "apos") out.push_back('\'');
    else if (name == "quot") out.push_back('"');
    else {
        // Without a DTD, MSXML reports any other name as undefined and points
        // at the '&' that opened the reference.
        return fail_at(kErrUndefinedEntity,
                       "Reference to undefined entity '" + name + "'.",
                       amp_line, amp_column + 1, amp_offset + 1);
    }
    return true;
}

bool Parser::parse_attribute_value(std::string& out) {
    out.clear();
    const char quote = m_cursor.peek();
    if (quote != '"' && quote != '\'') {
        return fail(kErrMissingQuote,
                    "A string literal was expected, but no opening quote "
                    "character was found.");
    }
    (void)m_cursor.advance();
    while (true) {
        if (m_cursor.at_end()) {
            return fail(kErrMissingQuote,
                        "A string literal was expected, but no opening quote "
                        "character was found.");
        }
        const char c = m_cursor.peek();
        if (c == quote) {
            (void)m_cursor.advance();
            return true;
        }
        if (c == '<') {
            return fail(kErrBadCharInString,
                        "The character '<' cannot be used in an attribute "
                        "value.");
        }
        if (c == '&') {
            if (!expand_reference(out)) return false;
            continue;
        }
        if (m_doc.settings().normalize_attribute_values &&
            (c == '\t' || c == '\n' || c == '\r')) {
            // XML 1.0 3.3.3: a literal line break (CRLF counted once) or tab
            // becomes one space; character references were kept above.
            (void)m_cursor.advance();
            if (c == '\r' && m_cursor.peek() == '\n') (void)m_cursor.advance();
            out.push_back(' ');
            continue;
        }
        out.push_back(m_cursor.advance());
    }
}

bool Parser::parse_comment(const Node::Ptr& parent) {
    m_cursor.skip(4); // "<!--"
    std::string data;
    while (true) {
        if (m_cursor.at_end()) {
            return fail(kErrCommentSyntax,
                        "Incorrect syntax was used in a comment.");
        }
        if (m_cursor.starts_with("--")) {
            // "--" is only legal as the start of the closing "-->".
            if (m_cursor.peek(2) != '>') {
                // MSXML points at the character that follows the "--" rather
                // than at the "--" itself.
                return fail_at(kErrCommentSyntax,
                               "Incorrect syntax was used in a comment.",
                               m_cursor.line(), m_cursor.column() + 2,
                               m_cursor.offset() + 2);
            }
            m_cursor.skip(3);
            auto node = m_doc.create_comment(std::move(data));
            return parent->append_child(node);
        }
        data.push_back(m_cursor.advance());
    }
}

bool Parser::parse_processing_instruction(const Node::Ptr& parent) {
    m_cursor.skip(2); // "<?"
    std::string target;
    if (!parse_name(target)) return false;
    std::string data;
    m_cursor.skip_space();
    while (!m_cursor.at_end() && !m_cursor.starts_with("?>")) {
        data.push_back(m_cursor.advance());
    }
    if (!m_cursor.starts_with("?>")) {
        return fail(kErrElementNotClosed, "Element was not closed.");
    }
    m_cursor.skip(2);
    auto node =
        m_doc.create_processing_instruction(std::move(target), std::move(data));
    return parent->append_child(node);
}

bool Parser::parse_cdata(const Node::Ptr& parent) {
    m_cursor.skip(9); // "<![CDATA["
    // The reported position is the first content character, not the "<".
    const i32   start_line   = m_cursor.line();
    const i32   start_column = m_cursor.column();
    const usize start_offset = m_cursor.offset();
    std::string data;
    while (!m_cursor.starts_with("]]>")) {
        if (m_cursor.at_end()) {
            return fail_at(kErrUnclosedCData, "A CDATA section was not closed.",
                           start_line, start_column, start_offset);
        }
        data.push_back(m_cursor.advance());
    }
    m_cursor.skip(3);
    auto node = m_doc.create_cdata_section(std::move(data));
    return parent->append_child(node);
}

bool Parser::parse_doctype() {
    // The DTD's internal subset is skipped rather than modelled: Vitreo does
    // not resolve external entities, and MSXML's default resolveExternals is
    // false, so no declaration here can affect the resulting tree.
    m_cursor.skip(9); // "<!DOCTYPE"
    if (m_doc.settings().prohibit_dtd) {
        // Reported at the document type name, after the keyword's whitespace.
        m_cursor.skip_space();
        const bool v6 = m_doc.settings().version == MsxmlVersion::V6;
        return v6 ? fail(kErrDtdProhibited, "DTD is prohibited.")
                  : fail(kErrInvalidAtTopLevel,
                         "Invalid at the top level of the document.");
    }
    m_doc.set_has_doctype(true);
    i32 depth = 0;
    while (!m_cursor.at_end()) {
        const char c = m_cursor.peek();
        if (c == '[') ++depth;
        else if (c == ']') --depth;
        else if (c == '>' && depth <= 0) {
            (void)m_cursor.advance();
            return true;
        }
        (void)m_cursor.advance();
    }
    return fail(kErrElementNotClosed, "Element was not closed.");
}

bool Parser::parse_element(const Node::Ptr& parent) {
    const i32   open_line   = m_cursor.line();
    const i32   open_column = m_cursor.column();
    const usize open_offset = m_cursor.offset();
    (void)m_cursor.advance(); // '<'

    std::string name;
    if (!parse_name(name)) return false;

    // MaxElementDepth counts the root as depth 1 and is reported at the name
    // of the first element past the limit.
    struct DepthGuard {
        i32& depth;
        explicit DepthGuard(i32& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    } depth_guard(m_depth);
    const i32 max_depth = m_doc.settings().max_element_depth;
    if (max_depth > 0 && m_depth > max_depth) {
        const bool v6 = m_doc.settings().version == MsxmlVersion::V6;
        return fail_at(v6 ? kErrMaxElementDepth : kErrAbortV3,
                       v6 ? "Operation aborted: MaxElementDepth constraint "
                            "violated."
                          : "System error: MaxElementDepth.",
                       open_line, open_column + 1, open_offset + 1);
    }

    auto element = m_doc.create_element(name);

    // Attributes.
    while (true) {
        const bool had_space = is_xml_space(m_cursor.peek());
        m_cursor.skip_space();
        if (m_cursor.at_end()) {
            return fail_at(kErrElementNotClosed, "Element was not closed.",
                           open_line, open_column + 1, open_offset + 1);
        }
        if (m_cursor.peek() == '>') {
            (void)m_cursor.advance();
            break;
        }
        if (m_cursor.starts_with("/>")) {
            m_cursor.skip(2);
            resolve_namespaces(element, parent);
            if (!parent->append_child(element)) {
                return fail(kErrInvalidAtTopLevel,
                            "Invalid at the top level of the document.");
            }
            return true;
        }
        if (!had_space) {
            return fail(kErrBadNameChar,
                        "A name contained an invalid character.");
        }
        std::string attr_name;
        if (!parse_name(attr_name)) return false;
        m_cursor.skip_space();
        if (m_cursor.peek() != '=') {
            return fail(kErrBadNameChar,
                        "A name contained an invalid character.");
        }
        (void)m_cursor.advance();
        m_cursor.skip_space();
        std::string attr_value;
        if (!parse_attribute_value(attr_value)) return false;

        auto attr = m_doc.create_attribute(attr_name);
        attr->set_value(std::move(attr_value));
        (void)element->set_attribute_node(attr);
    }

    resolve_namespaces(element, parent);
    if (!parent->append_child(element)) {
        return fail_at(kErrMultipleRoots,
                       "Only one top level element is allowed in an XML "
                       "document.",
                       open_line, open_column + 1, open_offset + 1);
    }
    if (!parse_content(element)) return false;

    // Closing tag.
    if (!m_cursor.starts_with("</")) {
        return fail_at(kErrElementNotClosed, "Element was not closed.",
                       open_line, open_column + 1, open_offset + 1);
    }
    m_cursor.skip(2); // "</"
    const i32   close_line   = m_cursor.line();
    const i32   close_column = m_cursor.column();
    const usize close_offset = m_cursor.offset();
    std::string close_name;
    if (!parse_name(close_name)) return false;
    if (close_name != name) {
        return fail_at(kErrTagMismatch,
                       "End tag '" + close_name +
                           "' does not match the start tag '" + name + "'.",
                       close_line, close_column, close_offset);
    }
    m_cursor.skip_space();
    if (m_cursor.peek() != '>') {
        return fail(kErrBadNameChar, "A name contained an invalid character.");
    }
    (void)m_cursor.advance();
    return true;
}

bool Parser::parse_content(const Node::Ptr& element) {
    std::string pending;
    // Character data accumulates until a markup token forces it out, so that
    // "a&amp;b" becomes one text node rather than three.
    const auto flush_text = [&]() -> bool {
        if (pending.empty()) return true;
        bool only_space = true;
        for (const char c : pending) {
            if (!is_xml_space(c)) { only_space = false; break; }
        }
        if (only_space && !m_doc.preserve_whitespace()) {
            pending.clear();
            return true;
        }
        auto node = m_doc.create_text_node(std::move(pending));
        pending.clear();
        return element->append_child(node);
    };

    while (true) {
        if (m_cursor.at_end()) return flush_text();
        if (m_cursor.starts_with("</")) return flush_text();

        if (m_cursor.starts_with("<!--")) {
            if (!flush_text() || !parse_comment(element)) return false;
            continue;
        }
        if (m_cursor.starts_with("<![CDATA[")) {
            if (!flush_text() || !parse_cdata(element)) return false;
            continue;
        }
        if (m_cursor.starts_with("<?")) {
            if (!flush_text() || !parse_processing_instruction(element)) {
                return false;
            }
            continue;
        }
        if (m_cursor.peek() == '<') {
            if (!flush_text() || !parse_element(element)) return false;
            continue;
        }
        if (m_cursor.peek() == '&') {
            if (!expand_reference(pending)) return false;
            continue;
        }
        pending.push_back(m_cursor.advance());
    }
}

bool Parser::parse_prolog() {
    // The XML declaration is metadata, not a DOM node in MSXML's default view,
    // but its syntax is still validated: a bare `version=1.0` is an error.
    if (!m_cursor.starts_with("<?xml")) return true;
    const char after = m_cursor.peek(5);
    if (!is_xml_space(after)) return true; // "<?xmlfoo" is an ordinary PI.
    m_cursor.skip(5);
    while (true) {
        m_cursor.skip_space();
        if (m_cursor.at_end()) {
            return fail(kErrElementNotClosed, "Element was not closed.");
        }
        if (m_cursor.starts_with("?>")) {
            m_cursor.skip(2);
            return true;
        }
        std::string attr_name;
        if (!parse_name(attr_name)) return false;
        m_cursor.skip_space();
        if (m_cursor.peek() != '=') {
            return fail(kErrBadNameChar,
                        "A name contained an invalid character.");
        }
        (void)m_cursor.advance();
        m_cursor.skip_space();
        std::string value;
        if (!parse_attribute_value(value)) return false;
    }
}

bool Parser::parse_misc(Node& parent, bool at_root) {
    // Comments and PIs may appear before and after the root element.
    auto self = parent.shared_from_this();
    while (true) {
        m_cursor.skip_space();
        if (m_cursor.at_end()) return true;
        if (m_cursor.starts_with("<!--")) {
            if (!parse_comment(self)) return false;
            continue;
        }
        if (m_cursor.starts_with("<?")) {
            if (!parse_processing_instruction(self)) return false;
            continue;
        }
        if (at_root && m_cursor.starts_with("<!DOCTYPE")) {
            if (!parse_doctype()) return false;
            continue;
        }
        return true;
    }
}

void Parser::resolve_namespaces(const Node::Ptr& element,
                                const Node::Ptr& parent) {
    const auto lookup = [&](const std::string& prefix) -> std::string {
        if (prefix == "xml") return kXmlNamespace;
        const std::string decl = prefix.empty() ? "xmlns" : "xmlns:" + prefix;
        if (auto attr = element->find_attribute(decl)) return attr->value();
        for (auto walk = parent; walk; walk = walk->parent()) {
            if (walk->type() != NodeType::Element) continue;
            if (auto attr = walk->find_attribute(decl)) return attr->value();
        }
        return {};
    };
    element->set_namespace_uri(lookup(element->prefix()));
    const bool v6 = m_doc.settings().version == MsxmlVersion::V6;
    for (const auto& attr : element->attributes()) {
        if (attr->is_namespace_declaration()) {
            // 6.0 reports the xmlns namespace; 3.0 reports none.
            attr->set_namespace_uri(v6 ? kXmlnsNamespace : "");
        } else if (const std::string p = attr->prefix(); !p.empty()) {
            // Unprefixed attributes are in no namespace (Namespaces in XML).
            attr->set_namespace_uri(lookup(p));
        }
    }
}

bool Parser::run() {
    if (const i32 kb = m_doc.settings().max_xml_size_kb; kb > 0) {
        // The limit is in blocks of 1024 characters (UTF-16 units).
        usize units = 0;
        for (const char ch : m_text) {
            const auto b = static_cast<unsigned char>(ch);
            if ((b & 0xC0u) != 0x80u) ++units;
            if (b >= 0xF0u) ++units; // supplementary plane: a surrogate pair
        }
        if (units > static_cast<usize>(kb) * 1024u) {
            const bool v6 = m_doc.settings().version == MsxmlVersion::V6;
            return fail_at(v6 ? kErrMaxXmlSize : kErrAbortV3,
                           v6 ? "Operation aborted: MaxXMLSize constraint "
                                "violated."
                              : "System error: MaxXMLSize.",
                           1, 1, 0);
        }
    }
    if (!parse_prolog()) return false;
    if (!parse_misc(m_doc, /*at_root=*/true)) return false;

    if (m_cursor.at_end()) {
        // An empty or comment-only document has no root. MSXML reports this at
        // position 0/0 rather than at the end of the text.
        return fail_at(kErrMissingRoot,
                       "XML document must have a top level element.", 0, 0, 0);
    }
    if (m_cursor.peek() != '<') {
        return fail(kErrInvalidAtTopLevel,
                    "Invalid at the top level of the document.");
    }

    auto root = m_doc.shared_from_this();
    if (!parse_element(root)) return false;

    if (!parse_misc(m_doc, /*at_root=*/false)) return false;
    m_cursor.skip_space();
    if (!m_cursor.at_end()) {
        if (m_cursor.peek() == '<') {
            // Reported at the second root's name, one past the '<'.
            return fail_at(kErrMultipleRoots,
                           "Only one top level element is allowed in an XML "
                           "document.",
                           m_cursor.line(), m_cursor.column() + 1,
                           m_cursor.offset() + 1);
        }
        return fail(kErrInvalidAtTopLevel,
                    "Invalid at the top level of the document.");
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Node
// ---------------------------------------------------------------------------

Node::Node(NodeType type, std::string name, std::string value)
    : m_type(type), m_name(std::move(name)), m_value(std::move(value)) {}

const char* node_type_string(NodeType type) {
    switch (type) {
        case NodeType::Element:               return "element";
        case NodeType::Attribute:             return "attribute";
        case NodeType::Text:                  return "text";
        case NodeType::CDATASection:          return "cdatasection";
        case NodeType::EntityReference:       return "entityreference";
        case NodeType::Entity:                return "entity";
        case NodeType::ProcessingInstruction: return "processinginstruction";
        case NodeType::Comment:               return "comment";
        case NodeType::Document:              return "document";
        case NodeType::DocumentType:          return "documenttype";
        case NodeType::DocumentFragment:      return "documentfragment";
        case NodeType::Notation:              return "notation";
        case NodeType::Invalid:               break;
    }
    return "";
}

bool Node::has_null_value() const noexcept {
    switch (m_type) {
        case NodeType::Element:
        case NodeType::Document:
        case NodeType::DocumentFragment:
        case NodeType::DocumentType:
        case NodeType::Entity:
        case NodeType::EntityReference:
        case NodeType::Notation:
            return true;
        default:
            return false;
    }
}

bool Node::can_have_children() const noexcept {
    switch (m_type) {
        case NodeType::Element:
        case NodeType::Document:
        case NodeType::DocumentFragment:
        case NodeType::Attribute:
        case NodeType::Entity:
        case NodeType::EntityReference:
            return true;
        default:
            return false;
    }
}

std::string Node::prefix() const {
    const auto colon = m_name.find(':');
    return colon == std::string::npos ? std::string{} : m_name.substr(0, colon);
}

std::string Node::base_name() const {
    const auto colon = m_name.find(':');
    return colon == std::string::npos ? m_name : m_name.substr(colon + 1);
}

bool Node::is_inclusive_ancestor_of(const Node& candidate) const {
    const Node* walk = &candidate;
    while (walk != nullptr) {
        if (walk == this) return true;
        const auto up = walk->m_parent.lock();
        walk = up.get();
    }
    return false;
}

Node::Ptr Node::previous_sibling() const {
    auto up = parent();
    if (!up || m_type == NodeType::Attribute) return nullptr;
    const auto& sibs = up->m_children;
    for (usize i = 0; i < sibs.size(); ++i) {
        if (sibs[i].get() == this) {
            return i == 0 ? nullptr : sibs[i - 1];
        }
    }
    return nullptr;
}

Node::Ptr Node::next_sibling() const {
    auto up = parent();
    if (!up || m_type == NodeType::Attribute) return nullptr;
    const auto& sibs = up->m_children;
    for (usize i = 0; i < sibs.size(); ++i) {
        if (sibs[i].get() == this) {
            return i + 1 >= sibs.size() ? nullptr : sibs[i + 1];
        }
    }
    return nullptr;
}

void Node::detach_from_parent() {
    auto up = m_parent.lock();
    if (!up) return;
    if (m_type == NodeType::Attribute) {
        auto& attrs = up->m_attributes;
        for (auto it = attrs.begin(); it != attrs.end(); ++it) {
            if (it->get() == this) { attrs.erase(it); break; }
        }
    } else {
        auto& kids = up->m_children;
        for (auto it = kids.begin(); it != kids.end(); ++it) {
            if (it->get() == this) { kids.erase(it); break; }
        }
    }
    m_parent.reset();
}

bool Node::append_child(const Ptr& child) {
    return insert_before(child, nullptr);
}

bool Node::insert_before(const Ptr& child, const Ptr& ref) {
    if (!child || !can_have_children()) return false;
    // Inserting a node into its own subtree would build a cycle and leak the
    // whole branch, since children are owned by shared_ptr.
    if (child->is_inclusive_ancestor_of(*this)) return false;
    // A document holds at most one element child; a second is the caller's
    // "only one top level element" error.
    if (m_type == NodeType::Document && child->type() == NodeType::Element) {
        for (const auto& existing : m_children) {
            if (existing->type() == NodeType::Element) return false;
        }
    }
    child->detach_from_parent();
    child->m_parent = weak_from_this();
    if (!ref) {
        m_children.push_back(child);
        return true;
    }
    for (auto it = m_children.begin(); it != m_children.end(); ++it) {
        if (*it == ref) {
            m_children.insert(it, child);
            return true;
        }
    }
    m_children.push_back(child);
    return true;
}

bool Node::replace_child(const Ptr& new_child, const Ptr& old_child) {
    if (!old_child) return false;
    for (auto it = m_children.begin(); it != m_children.end(); ++it) {
        if (*it != old_child) continue;
        if (!new_child) {
            old_child->m_parent.reset();
            m_children.erase(it);
            return true;
        }
        if (new_child->is_inclusive_ancestor_of(*this)) return false;
        new_child->detach_from_parent();
        new_child->m_parent = weak_from_this();
        old_child->m_parent.reset();
        // detach_from_parent may have shifted the vector, so re-find the slot.
        for (auto slot = m_children.begin(); slot != m_children.end(); ++slot) {
            if (*slot == old_child) { *slot = new_child; return true; }
        }
        m_children.push_back(new_child);
        return true;
    }
    return false;
}

bool Node::remove_child(const Ptr& child) {
    if (!child) return false;
    for (auto it = m_children.begin(); it != m_children.end(); ++it) {
        if (*it == child) {
            child->m_parent.reset();
            m_children.erase(it);
            return true;
        }
    }
    return false;
}

Node::Ptr Node::find_attribute(std::string_view qualified_name) const {
    for (const auto& attr : m_attributes) {
        if (attr->name() == qualified_name) return attr;
    }
    return nullptr;
}

Node::Ptr Node::set_attribute_node(const Ptr& attr) {
    if (!attr) return nullptr;
    Ptr displaced;
    for (auto& existing : m_attributes) {
        if (existing->name() != attr->name()) continue;
        displaced = existing;
        displaced->m_parent.reset();
        attr->detach_from_parent();
        attr->m_parent = weak_from_this();
        existing = attr;
        return displaced;
    }
    attr->detach_from_parent();
    attr->m_parent = weak_from_this();
    m_attributes.push_back(attr);
    return nullptr;
}

Node::Ptr Node::remove_attribute_node(const Ptr& attr) {
    if (!attr) return nullptr;
    for (auto it = m_attributes.begin(); it != m_attributes.end(); ++it) {
        if (*it != attr) continue;
        auto removed = *it;
        removed->m_parent.reset();
        m_attributes.erase(it);
        return removed;
    }
    return nullptr;
}

std::string Node::text() const {
    switch (m_type) {
        case NodeType::Text:
        case NodeType::CDATASection:
        case NodeType::Attribute:
            return m_value;
        case NodeType::Comment:
        case NodeType::ProcessingInstruction:
            // MSXML's get_text skips comments and PIs when gathering a
            // subtree, but returns the data when asked for the node itself.
            return m_value;
        default:
            break;
    }
    std::string out;
    for (const auto& child : m_children) {
        switch (child->type()) {
            case NodeType::Comment:
            case NodeType::ProcessingInstruction:
                continue;
            default:
                out += child->text();
                break;
        }
    }
    return out;
}

void Node::set_text(std::string value) {
    switch (m_type) {
        case NodeType::Text:
        case NodeType::CDATASection:
        case NodeType::Comment:
        case NodeType::ProcessingInstruction:
        case NodeType::Attribute:
            m_value = std::move(value);
            return;
        default:
            break;
    }
    for (auto& child : m_children) child->m_parent.reset();
    m_children.clear();
    if (auto doc = owner_document()) {
        auto node = doc->create_text_node(std::move(value));
        (void)append_child(node);
    }
}

std::string escape_xml(std::string_view text, bool in_attribute) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  if (in_attribute) out += "&quot;"; else out.push_back(c);
                       break;
            case '\r': out += "&#13;";  break;
            default:   out.push_back(c); break;
        }
    }
    return out;
}

std::string Node::xml() const {
    switch (m_type) {
        case NodeType::Text:
            return escape_xml(m_value, /*in_attribute=*/false);
        case NodeType::CDATASection:
            return "<![CDATA[" + m_value + "]]>";
        case NodeType::Comment:
            return "<!--" + m_value + "-->";
        case NodeType::ProcessingInstruction:
            return m_value.empty() ? "<?" + m_name + "?>"
                                   : "<?" + m_name + " " + m_value + "?>";
        case NodeType::Attribute:
            return m_name + "=\"" + escape_xml(m_value, true) + "\"";
        case NodeType::EntityReference:
            return "&" + m_name + ";";
        case NodeType::Element: {
            std::string out = "<" + m_name;
            for (const auto& attr : m_attributes) {
                out += " " + attr->xml();
            }
            if (m_children.empty()) {
                out += "/>";
                return out;
            }
            out += ">";
            for (const auto& child : m_children) out += child->xml();
            out += "</" + m_name + ">";
            return out;
        }
        case NodeType::Document:
        case NodeType::DocumentFragment:
        default: {
            std::string out;
            for (const auto& child : m_children) out += child->xml();
            return out;
        }
    }
}

Node::Ptr Node::clone(bool deep) const {
    auto copy = std::make_shared<Node>(m_type, m_name, m_value);
    copy->m_namespace_uri = m_namespace_uri;
    copy->m_owner         = m_owner;
    for (const auto& attr : m_attributes) {
        auto attr_copy = attr->clone(true);
        attr_copy->m_parent = copy;
        copy->m_attributes.push_back(attr_copy);
    }
    if (deep) {
        for (const auto& child : m_children) {
            auto child_copy = child->clone(true);
            child_copy->m_parent = copy;
            copy->m_children.push_back(child_copy);
        }
    }
    return copy;
}

void Node::adopt(const std::shared_ptr<Document>& doc) {
    m_owner = doc;
    for (const auto& attr : m_attributes) attr->adopt(doc);
    for (const auto& child : m_children) child->adopt(doc);
}

bool Node::is_namespace_declaration() const {
    return m_type == NodeType::Attribute &&
           (m_name == "xmlns" || m_name.rfind("xmlns:", 0) == 0);
}

void Node::collect_elements_by_tag_name(std::string_view tag,
                                        std::vector<Ptr>& out) const {
    for (const auto& child : m_children) {
        if (child->type() == NodeType::Element &&
            (tag == "*" || child->name() == tag)) {
            out.push_back(child);
        }
        child->collect_elements_by_tag_name(tag, out);
    }
}

// ---------------------------------------------------------------------------
// Document
// ---------------------------------------------------------------------------

Document::Document() : Node(NodeType::Document, "#document", std::string{}) {}

Node::Ptr Document::document_element() const {
    for (const auto& child : children()) {
        if (child->type() == NodeType::Element) return child;
    }
    return nullptr;
}

namespace {

/// Strip a byte-order mark and transcode UTF-16 input to UTF-8.
///
/// XML files are routinely stored with a BOM — WiX Burn's own manifest is
/// UTF-8 with one — and MSXML honours both that and UTF-16 encodings. Without
/// this the first byte is not '<' and the whole document is rejected as
/// "Invalid at the top level of the document."
///
/// Positions reported by the parser are relative to the decoded text, which is
/// what a caller comparing against the document's characters expects.
[[nodiscard]] std::string decode_to_utf8(std::string_view text) {
    const auto byte = [&](usize i) -> unsigned char {
        return static_cast<unsigned char>(text[i]);
    };
    if (text.size() >= 3 && byte(0) == 0xEF && byte(1) == 0xBB &&
        byte(2) == 0xBF) {
        return std::string(text.substr(3));
    }
    const bool le = text.size() >= 2 && byte(0) == 0xFF && byte(1) == 0xFE;
    const bool be = text.size() >= 2 && byte(0) == 0xFE && byte(1) == 0xFF;
    if (!le && !be) return std::string(text);

    std::string out;
    out.reserve(text.size() / 2);
    for (usize i = 2; i + 1 < text.size(); i += 2) {
        u32 cp = le ? static_cast<u32>(byte(i)) |
                          (static_cast<u32>(byte(i + 1)) << 8)
                    : (static_cast<u32>(byte(i)) << 8) |
                          static_cast<u32>(byte(i + 1));
        // Combine a surrogate pair into one code point before encoding.
        if (cp >= 0xD800u && cp <= 0xDBFFu && i + 3 < text.size()) {
            const u32 lo = le ? static_cast<u32>(byte(i + 2)) |
                                    (static_cast<u32>(byte(i + 3)) << 8)
                              : (static_cast<u32>(byte(i + 2)) << 8) |
                                    static_cast<u32>(byte(i + 3));
            if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 2;
            }
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

bool parse_document(std::string_view text, Document& doc) {
    const std::string decoded = decode_to_utf8(text);
    text = decoded;
    Parser parser(text, doc);
    if (parser.run()) {
        doc.set_parse_error(ParseError{});
        return true;
    }
    doc.set_parse_error(parser.error());
    return false;
}

namespace {

void collect_declarations(const Node& node, bool keep_empty,
                          std::vector<std::string>& out) {
    for (const auto& attr : node.attributes()) {
        if (!attr->is_namespace_declaration()) continue;
        const std::string& uri = attr->value();
        if (uri.empty() && !keep_empty) continue;
        if (std::find(out.begin(), out.end(), uri) == out.end()) {
            out.push_back(uri);
        }
    }
    for (const auto& child : node.children()) {
        collect_declarations(*child, keep_empty, out);
    }
}

} // namespace

std::vector<std::string> Document::declared_namespaces() const {
    std::vector<std::string> out;
    collect_declarations(*this, m_settings.version == MsxmlVersion::V6, out);
    return out;
}

bool Document::load_xml(std::string_view text) {
    for (auto& child : m_children) child->m_parent.reset();
    m_children.clear();
    m_url.clear();
    m_has_doctype = false;
    if (parse_document(text, *this)) return true;
    // MSXML leaves the document empty after a failed parse.
    for (auto& child : m_children) child->m_parent.reset();
    m_children.clear();
    m_has_doctype = false;
    return false;
}

Node::Ptr Document::create_element(std::string name) {
    auto node = std::make_shared<Node>(NodeType::Element, std::move(name),
                                       std::string{});
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

Node::Ptr Document::create_text_node(std::string data) {
    auto node = std::make_shared<Node>(NodeType::Text, "#text", std::move(data));
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

Node::Ptr Document::create_comment(std::string data) {
    auto node =
        std::make_shared<Node>(NodeType::Comment, "#comment", std::move(data));
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

Node::Ptr Document::create_cdata_section(std::string data) {
    auto node = std::make_shared<Node>(NodeType::CDATASection,
                                       "#cdata-section", std::move(data));
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

Node::Ptr Document::create_processing_instruction(std::string target,
                                                  std::string data) {
    auto node = std::make_shared<Node>(NodeType::ProcessingInstruction,
                                       std::move(target), std::move(data));
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

Node::Ptr Document::create_attribute(std::string name) {
    auto node = std::make_shared<Node>(NodeType::Attribute, std::move(name),
                                       std::string{});
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

Node::Ptr Document::create_document_fragment() {
    auto node = std::make_shared<Node>(NodeType::DocumentFragment,
                                       "#document-fragment", std::string{});
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

Node::Ptr Document::create_entity_reference(std::string name) {
    auto node = std::make_shared<Node>(NodeType::EntityReference,
                                       std::move(name), std::string{});
    node->set_owner_document(
        std::static_pointer_cast<Document>(shared_from_this()));
    return node;
}

} // namespace vitreo::msxml

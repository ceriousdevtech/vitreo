/// @file test_xml_dom.cpp
/// @brief MSXML DOM, parser, and XPath tests.
///
/// The parse-error expectations are transcribed from a capture of the real
/// MSXML6 on the reference Windows host (oracle_parse_errors.tsv, produced by
/// oracle_parse_error_probe.ps1). They pin the guest-visible errorCode, line,
/// linepos, and filepos rather than whatever this parser happens to produce.

#include <gtest/gtest.h>

#include <winapi/com/msxml/xml_dom.hpp>
#include <winapi/com/msxml/xpath.hpp>

using namespace vitreo;
using namespace vitreo::msxml;

namespace {

std::shared_ptr<Document> parse_ok(std::string_view text) {
    auto doc = Document::create();
    EXPECT_TRUE(doc->load_xml(text))
        << "unexpected parse error: " << doc->parse_error().reason;
    return doc;
}

} // namespace

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

TEST(MsxmlDomTest, ParsesElementsAttributesAndText) {
    auto doc = parse_ok(R"(<root a="1" b='two'><child>hello</child></root>)");
    auto root = doc->document_element();
    ASSERT_TRUE(root);
    EXPECT_EQ(root->name(), "root");
    EXPECT_EQ(root->type(), NodeType::Element);
    ASSERT_EQ(root->attributes().size(), 2u);
    EXPECT_EQ(root->find_attribute("a")->value(), "1");
    EXPECT_EQ(root->find_attribute("b")->value(), "two");
    ASSERT_EQ(root->children().size(), 1u);
    EXPECT_EQ(root->children()[0]->name(), "child");
    EXPECT_EQ(root->children()[0]->text(), "hello");
}

TEST(MsxmlDomTest, DropsWhitespaceOnlyTextByDefault) {
    // preserveWhiteSpace defaults to false in MSXML, so the indentation between
    // elements must not become text nodes.
    auto doc = parse_ok("<a>\n  <b/>\n  <c/>\n</a>");
    auto root = doc->document_element();
    ASSERT_TRUE(root);
    ASSERT_EQ(root->children().size(), 2u);
    EXPECT_EQ(root->children()[0]->name(), "b");
    EXPECT_EQ(root->children()[1]->name(), "c");
}

TEST(MsxmlDomTest, PreservesWhitespaceWhenAsked) {
    auto doc = Document::create();
    doc->set_preserve_whitespace(true);
    ASSERT_TRUE(doc->load_xml("<a>\n  <b/>\n</a>"));
    EXPECT_GT(doc->document_element()->children().size(), 1u);
}

TEST(MsxmlDomTest, ExpandsBuiltinAndNumericEntities) {
    auto doc = parse_ok(
        "<a x=\"&lt;&amp;&gt;\">&apos;&quot;&#65;&#x42;</a>");
    auto root = doc->document_element();
    EXPECT_EQ(root->find_attribute("x")->value(), "<&>");
    EXPECT_EQ(root->text(), "'\"AB");
}

TEST(MsxmlDomTest, ParsesCdataCommentsAndProcessingInstructions) {
    auto doc = parse_ok(
        "<a><!--note--><![CDATA[<raw> & stuff]]><?pi data?>tail</a>");
    auto root = doc->document_element();
    ASSERT_EQ(root->children().size(), 4u);
    EXPECT_EQ(root->children()[0]->type(), NodeType::Comment);
    EXPECT_EQ(root->children()[0]->value(), "note");
    EXPECT_EQ(root->children()[1]->type(), NodeType::CDATASection);
    EXPECT_EQ(root->children()[1]->value(), "<raw> & stuff");
    EXPECT_EQ(root->children()[2]->type(), NodeType::ProcessingInstruction);
    EXPECT_EQ(root->children()[2]->name(), "pi");
    // get_text skips comments and PIs but includes CDATA.
    EXPECT_EQ(root->text(), "<raw> & stufftail");
}

TEST(MsxmlDomTest, AcceptsXmlDeclarationAndDoctype) {
    auto doc = parse_ok(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<!DOCTYPE a SYSTEM \"a.dtd\">"
        "<a/>");
    ASSERT_TRUE(doc->document_element());
    EXPECT_EQ(doc->document_element()->name(), "a");
}

TEST(MsxmlDomTest, AllowsCommentsAroundRootElement) {
    auto doc = parse_ok("<!--before--><a/><!--after-->");
    ASSERT_TRUE(doc->document_element());
    EXPECT_EQ(doc->document_element()->name(), "a");
}

TEST(MsxmlDomTest, SkipsUtf8ByteOrderMark) {
    // WiX Burn's manifest is UTF-8 with a BOM. Without BOM handling the first
    // byte is not '<' and the whole document is rejected at the top level.
    auto doc = parse_ok("\xEF\xBB\xBF<a>x</a>");
    ASSERT_TRUE(doc->document_element());
    EXPECT_EQ(doc->document_element()->name(), "a");
    EXPECT_EQ(doc->document_element()->text(), "x");
}

TEST(MsxmlDomTest, DecodesUtf16Documents) {
    // MSXML accepts UTF-16 input; both byte orders carry a BOM. Build the
    // bytes rather than hand-writing escapes so the length cannot drift.
    const std::string ascii = "<a>x</a>";
    const auto widen = [&](bool little_endian) {
        std::string out;
        out.push_back(static_cast<char>(little_endian ? 0xFF : 0xFE));
        out.push_back(static_cast<char>(little_endian ? 0xFE : 0xFF));
        for (const char c : ascii) {
            if (little_endian) { out.push_back(c); out.push_back('\0'); }
            else               { out.push_back('\0'); out.push_back(c); }
        }
        return out;
    };

    auto doc_le = parse_ok(widen(true));
    ASSERT_TRUE(doc_le->document_element());
    EXPECT_EQ(doc_le->document_element()->name(), "a");
    EXPECT_EQ(doc_le->document_element()->text(), "x");

    auto doc_be = parse_ok(widen(false));
    ASSERT_TRUE(doc_be->document_element());
    EXPECT_EQ(doc_be->document_element()->name(), "a");
    EXPECT_EQ(doc_be->document_element()->text(), "x");
}

TEST(MsxmlDomTest, DecodesNonAsciiUtf8Content) {
    // A byte-wise widening would corrupt these on the way out to a BSTR.
    auto doc = parse_ok("<a t=\"caf\xC3\xA9\">\xE6\x97\xA5</a>");
    EXPECT_EQ(doc->document_element()->find_attribute("t")->value(),
              "caf\xC3\xA9");
    EXPECT_EQ(doc->document_element()->text(), "\xE6\x97\xA5");
}

// ---------------------------------------------------------------------------
// Parse errors — values captured from MSXML6 on real Windows
// ---------------------------------------------------------------------------

namespace {

struct ErrorCase {
    const char* label;
    const char* xml;
    u32         code;
    i32         line;
    i32         linepos;
    i32         filepos;
};

const ErrorCase kOracleErrors[] = {
    {"unclosed_tag",       "<a><b></a>",             0xC00CE56Du, 1, 9,  8},
    {"no_root",            "",                       0xC00CE558u, 0, 0,  0},
    {"junk_at_root",       "hello",                  0xC00CE556u, 1, 1,  0},
    {"two_roots",          "<a/><b/>",               0xC00CE555u, 1, 6,  5},
    {"missing_quote",      "<a x=1/>",               0xC00CE502u, 1, 6,  5},
    {"bad_start_name",     "<1a/>",                  0xC00CE504u, 1, 2,  1},
    {"unterminated_tag",   "<a",                     0xC00CE55Eu, 1, 2,  1},
    {"undefined_entity",   "<a>&nope;</a>",          0xC00CE002u, 1, 5,  4},
    {"missing_semicolon",  "<a>&amp</a>",            0xC00CE50Du, 1, 8,  7},
    {"comment_syntax",     "<a><!-- x --- y --></a>",0xC00CE503u, 1, 13, 12},
    {"bad_char_in_string", "<a x='b<c'/>",           0xC00CE506u, 1, 8,  7},
    {"mismatched_close",   "<a></b>",                0xC00CE56Du, 1, 6,  5},
    {"cdata_unterminated", "<a><![CDATA[x</a>",      0xC00CE564u, 1, 13, 12},
    {"invalid_hex",        "<a>&#xZZ;</a>",          0xC00CE50Eu, 1, 7,  6},
    {"multiple_colons",    "<a:b:c/>",               0xC00CE51Cu, 1, 5,  4},
    {"two_roots_pair",     "<a></a><b></b>",         0xC00CE555u, 1, 9,  8},
    {"trailing_junk",      "<a/>junk",               0xC00CE556u, 1, 5,  4},
};

} // namespace

TEST(MsxmlDomTest, ParseErrorsMatchWindows) {
    for (const auto& c : kOracleErrors) {
        SCOPED_TRACE(c.label);
        auto doc = Document::create();
        EXPECT_FALSE(doc->load_xml(c.xml));
        const auto& err = doc->parse_error();
        EXPECT_EQ(static_cast<u32>(err.code), c.code)
            << "expected 0x" << std::hex << c.code << " got 0x" << err.code;
        EXPECT_EQ(err.line, c.line);
        EXPECT_EQ(err.line_position, c.linepos);
        EXPECT_EQ(err.file_position, c.filepos);
        EXPECT_FALSE(err.reason.empty());
    }
}

TEST(MsxmlDomTest, SuccessfulParseClearsParseError) {
    auto doc = Document::create();
    EXPECT_FALSE(doc->load_xml("<a"));
    EXPECT_NE(doc->parse_error().code, 0);
    EXPECT_TRUE(doc->load_xml("<a/>"));
    EXPECT_EQ(doc->parse_error().code, 0);
}

TEST(MsxmlDomTest, FailedParseLeavesDocumentEmpty) {
    auto doc = Document::create();
    ASSERT_TRUE(doc->load_xml("<a><b/></a>"));
    EXPECT_FALSE(doc->load_xml("<a><b></a>"));
    EXPECT_FALSE(doc->document_element());
}

TEST(MsxmlDomTest, ErrorLineTracksNewlines) {
    // linepos is 1-based within the line while filepos counts bytes, so the
    // two diverge once a newline has been crossed.
    auto doc = Document::create();
    EXPECT_FALSE(doc->load_xml("<a>\n<b></a>"));
    EXPECT_EQ(doc->parse_error().line, 2);
    EXPECT_EQ(doc->parse_error().line_position, 6);
    EXPECT_EQ(doc->parse_error().file_position, 9);
}

// ---------------------------------------------------------------------------
// Serialization and mutation
// ---------------------------------------------------------------------------

TEST(MsxmlDomTest, RoundTripsThroughXml) {
    const char* source =
        R"(<root a="1"><child>text</child><empty/><!--c--></root>)";
    auto doc = parse_ok(source);
    EXPECT_EQ(doc->xml(), source);
}

TEST(MsxmlDomTest, EscapesSpecialCharactersOnSerialize) {
    auto doc  = Document::create();
    auto root = doc->create_element("r");
    ASSERT_TRUE(doc->append_child(root));
    auto attr = doc->create_attribute("a");
    attr->set_value("<\"&>");
    (void)root->set_attribute_node(attr);
    root->set_text("a<b&c");
    EXPECT_EQ(doc->xml(), R"(<r a="&lt;&quot;&amp;&gt;">a&lt;b&amp;c</r>)");
}

TEST(MsxmlDomTest, AppendChildMovesNodeBetweenParents) {
    auto doc = parse_ok("<r><a><x/></a><b/></r>");
    auto root = doc->document_element();
    auto a    = root->children()[0];
    auto b    = root->children()[1];
    auto x    = a->children()[0];
    ASSERT_TRUE(b->append_child(x));
    EXPECT_TRUE(a->children().empty());
    ASSERT_EQ(b->children().size(), 1u);
    EXPECT_EQ(x->parent(), b);
}

TEST(MsxmlDomTest, RejectsCyclicInsertion) {
    auto doc  = parse_ok("<r><a><b/></a></r>");
    auto root = doc->document_element();
    auto a    = root->children()[0];
    auto b    = a->children()[0];
    // Making an ancestor a child of its own descendant would leak the branch.
    EXPECT_FALSE(b->append_child(a));
    EXPECT_FALSE(a->append_child(a));
}

TEST(MsxmlDomTest, DocumentAcceptsOnlyOneElementChild) {
    auto doc = Document::create();
    ASSERT_TRUE(doc->append_child(doc->create_element("a")));
    EXPECT_FALSE(doc->append_child(doc->create_element("b")));
}

TEST(MsxmlDomTest, SetAttributeNodeReplacesSameName) {
    auto doc  = parse_ok(R"(<r a="1"/>)");
    auto root = doc->document_element();
    auto attr = doc->create_attribute("a");
    attr->set_value("2");
    auto displaced = root->set_attribute_node(attr);
    ASSERT_TRUE(displaced);
    EXPECT_EQ(displaced->value(), "1");
    ASSERT_EQ(root->attributes().size(), 1u);
    EXPECT_EQ(root->find_attribute("a")->value(), "2");
}

TEST(MsxmlDomTest, CloneNodeDeepCopiesSubtree) {
    auto doc  = parse_ok(R"(<r a="1"><c>t</c></r>)");
    auto root = doc->document_element();
    auto shallow = root->clone(false);
    EXPECT_TRUE(shallow->children().empty());
    EXPECT_EQ(shallow->attributes().size(), 1u);
    auto deep = root->clone(true);
    ASSERT_EQ(deep->children().size(), 1u);
    EXPECT_EQ(deep->children()[0]->text(), "t");
    // A clone is detached from the tree it came from.
    EXPECT_FALSE(deep->parent());
}

TEST(MsxmlDomTest, NodeTypeStringsMatchMsxml) {
    EXPECT_STREQ(node_type_string(NodeType::Element), "element");
    EXPECT_STREQ(node_type_string(NodeType::Text), "text");
    EXPECT_STREQ(node_type_string(NodeType::CDATASection), "cdatasection");
    EXPECT_STREQ(node_type_string(NodeType::Document), "document");
    EXPECT_STREQ(node_type_string(NodeType::ProcessingInstruction),
                 "processinginstruction");
}

TEST(MsxmlDomTest, PrefixAndBaseNameSplitQualifiedNames) {
    auto doc  = parse_ok("<ns:root xmlns:ns='urn:x'/>");
    auto root = doc->document_element();
    EXPECT_EQ(root->name(), "ns:root");
    EXPECT_EQ(root->prefix(), "ns");
    EXPECT_EQ(root->base_name(), "root");
}

// ---------------------------------------------------------------------------
// XPath
// ---------------------------------------------------------------------------

namespace {

const char* kSample =
    "<catalog>"
    "<book id='1' lang='en'><title>Alpha</title><price>10</price></book>"
    "<book id='2'><title>Beta</title><price>20</price></book>"
    "<book id='3'><title>Gamma</title></book>"
    "</catalog>";

} // namespace

TEST(MsxmlXPathTest, SelectsByAbsoluteAndRelativePaths) {
    auto doc = parse_ok(kSample);
    auto r   = select_nodes(doc, "/catalog/book");
    ASSERT_TRUE(r.supported);
    EXPECT_EQ(r.nodes.size(), 3u);

    auto rel = select_nodes(doc->document_element(), "book/title");
    ASSERT_TRUE(rel.supported);
    ASSERT_EQ(rel.nodes.size(), 3u);
    EXPECT_EQ(rel.nodes[0]->text(), "Alpha");
}

TEST(MsxmlXPathTest, DescendantShorthandFindsNestedNodes) {
    auto doc = parse_ok(kSample);
    auto r   = select_nodes(doc, "//title");
    ASSERT_TRUE(r.supported);
    ASSERT_EQ(r.nodes.size(), 3u);
    EXPECT_EQ(r.nodes[2]->text(), "Gamma");
}

TEST(MsxmlXPathTest, SelectsAttributes) {
    auto doc = parse_ok(kSample);
    auto r   = select_nodes(doc, "//book/@id");
    ASSERT_TRUE(r.supported);
    ASSERT_EQ(r.nodes.size(), 3u);
    EXPECT_EQ(r.nodes[0]->type(), NodeType::Attribute);
    EXPECT_EQ(r.nodes[0]->value(), "1");
}

TEST(MsxmlXPathTest, AppliesPositionAndAttributePredicates) {
    auto doc = parse_ok(kSample);

    auto second = select_nodes(doc, "/catalog/book[2]");
    ASSERT_TRUE(second.supported);
    ASSERT_EQ(second.nodes.size(), 1u);
    EXPECT_EQ(second.nodes[0]->find_attribute("id")->value(), "2");

    auto by_attr = select_nodes(doc, "//book[@id='3']");
    ASSERT_TRUE(by_attr.supported);
    ASSERT_EQ(by_attr.nodes.size(), 1u);

    auto has_attr = select_nodes(doc, "//book[@lang]");
    ASSERT_TRUE(has_attr.supported);
    EXPECT_EQ(has_attr.nodes.size(), 1u);

    auto last = select_nodes(doc, "/catalog/book[last()]");
    ASSERT_TRUE(last.supported);
    ASSERT_EQ(last.nodes.size(), 1u);
    EXPECT_EQ(last.nodes[0]->find_attribute("id")->value(), "3");
}

TEST(MsxmlXPathTest, AppliesChildPredicates) {
    auto doc = parse_ok(kSample);
    auto with_price = select_nodes(doc, "//book[price]");
    ASSERT_TRUE(with_price.supported);
    EXPECT_EQ(with_price.nodes.size(), 2u);

    auto exact = select_nodes(doc, "//book[title='Beta']");
    ASSERT_TRUE(exact.supported);
    ASSERT_EQ(exact.nodes.size(), 1u);
    EXPECT_EQ(exact.nodes[0]->find_attribute("id")->value(), "2");
}

TEST(MsxmlXPathTest, HandlesWildcardsSelfParentAndUnion) {
    auto doc = parse_ok(kSample);

    auto wild = select_nodes(doc, "/catalog/*");
    ASSERT_TRUE(wild.supported);
    EXPECT_EQ(wild.nodes.size(), 3u);

    auto first_book = select_nodes(doc, "/catalog/book[1]").nodes.at(0);
    auto self = select_nodes(first_book, ".");
    ASSERT_TRUE(self.supported);
    ASSERT_EQ(self.nodes.size(), 1u);
    EXPECT_EQ(self.nodes[0], first_book);

    auto up = select_nodes(first_book, "..");
    ASSERT_TRUE(up.supported);
    ASSERT_EQ(up.nodes.size(), 1u);
    EXPECT_EQ(up.nodes[0]->name(), "catalog");

    auto both = select_nodes(doc, "//title | //price");
    ASSERT_TRUE(both.supported);
    EXPECT_EQ(both.nodes.size(), 5u);
}

TEST(MsxmlXPathTest, ReturnsResultsInDocumentOrder) {
    auto doc = parse_ok(kSample);
    // The union is written price-first, but the result must be document order.
    auto r = select_nodes(doc, "//price | //title");
    ASSERT_TRUE(r.supported);
    ASSERT_EQ(r.nodes.size(), 5u);
    EXPECT_EQ(r.nodes[0]->text(), "Alpha");
    EXPECT_EQ(r.nodes[1]->text(), "10");
    EXPECT_EQ(r.nodes[2]->text(), "Beta");
    EXPECT_EQ(r.nodes[3]->text(), "20");
    EXPECT_EQ(r.nodes[4]->text(), "Gamma");
}

TEST(MsxmlXPathTest, EmptyMatchIsSupportedNotAnError) {
    auto doc = parse_ok(kSample);
    auto r   = select_nodes(doc, "/catalog/magazine");
    EXPECT_TRUE(r.supported);
    EXPECT_TRUE(r.nodes.empty());
}

TEST(MsxmlXPathTest, RejectsUnsupportedSyntaxInsteadOfReturningEmpty) {
    // An unparsable expression must be distinguishable from "no matches";
    // reporting an empty node set would silently change caller behaviour.
    auto doc = parse_ok(kSample);
    for (const char* expr : {"//book[position() > 1]",
                             "count(//book)",
                             "child::book",
                             "//book[@id!='1']",
                             "//book[contains(@id,'1')]"}) {
        SCOPED_TRACE(expr);
        auto r = select_nodes(doc, expr);
        EXPECT_FALSE(r.supported);
        EXPECT_FALSE(r.unsupported_reason.empty());
        EXPECT_TRUE(r.nodes.empty());
    }
}

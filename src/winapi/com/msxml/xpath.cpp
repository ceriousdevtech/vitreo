// SPDX-License-Identifier: LGPL-2.1-only

/// @file xpath.cpp
/// @brief Location-path evaluator for selectNodes / selectSingleNode.

#include "winapi/com/msxml/xpath.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace vitreo::msxml {

namespace {

[[nodiscard]] bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

[[nodiscard]] bool is_name_char(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' ||
           c == ':' || c >= 0x80;
}

/// What a step matches once the axis has been applied.
enum class NodeTest {
    Name,     ///< A literal (possibly prefixed) element/attribute name.
    Any,      ///< `*`
    NamespaceAny, ///< `p:*`; @ref Step::name holds the prefix.
    Text,     ///< `text()`
    Comment,  ///< `comment()`
    AnyNode,  ///< `node()`
};

enum class Axis {
    Child,
    Descendant, ///< The `//` shorthand: descendant-or-self then child.
    DescendantOrSelf, ///< `//` before an attribute step: `//@a`.
    Attribute,
    Self,       ///< `.`
    Parent,     ///< `..`
};

/// A predicate in one of the shapes this evaluator understands.
struct Predicate {
    enum class Kind {
        Position,      ///< `[3]`
        Last,          ///< `[last()]`
        HasAttribute,  ///< `[@a]`
        AttributeEquals,///< `[@a='v']`
        HasChild,      ///< `[b]`
        ChildEquals,   ///< `[b='v']`
    };
    Kind        kind = Kind::Position;
    i32         position = 0;
    std::string name;
    std::string value;
};

struct Step {
    Axis                   axis = Axis::Child;
    NodeTest               test = NodeTest::Name;
    std::string            name;
    std::vector<Predicate> predicates;
};

/// Parses one location path (no unions) into steps.
class PathParser {
public:
    PathParser(std::string_view text) : m_text(text) {}

    /// Returns false when the expression is outside the supported grammar;
    /// @ref reason then explains why.
    bool parse(std::vector<Step>& steps, bool& absolute);

    [[nodiscard]] const std::string& reason() const { return m_reason; }

private:
    bool fail(std::string why) {
        m_reason = std::move(why);
        return false;
    }
    void skip_space() {
        while (m_pos < m_text.size() && is_space(m_text[m_pos])) ++m_pos;
    }
    [[nodiscard]] char peek(usize ahead = 0) const {
        const usize at = m_pos + ahead;
        return at < m_text.size() ? m_text[at] : '\0';
    }
    bool parse_step(Step& step);
    /// `//step`: descendant-or-self::node()/step. A child step folds into the
    /// Descendant axis; an attribute step needs the explicit node step so the
    /// context element's own attributes are included.
    static void push_descendant(std::vector<Step>& steps, Step step) {
        if (step.axis == Axis::Attribute) {
            Step any;
            any.axis = Axis::DescendantOrSelf;
            any.test = NodeTest::AnyNode;
            steps.push_back(std::move(any));
        } else {
            step.axis = Axis::Descendant;
        }
        steps.push_back(std::move(step));
    }
    bool parse_predicate(Predicate& pred);
    bool parse_name(std::string& out);
    bool parse_quoted(std::string& out);

    std::string_view m_text;
    usize            m_pos = 0;
    std::string      m_reason;
};

bool PathParser::parse_name(std::string& out) {
    out.clear();
    while (m_pos < m_text.size() &&
           is_name_char(static_cast<unsigned char>(m_text[m_pos]))) {
        // Stop before an axis separator so "child::book" is seen as an axis
        // specifier and rejected, rather than swallowed as one long name.
        if (m_text[m_pos] == ':' && m_pos + 1 < m_text.size() &&
            m_text[m_pos + 1] == ':') {
            break;
        }
        out.push_back(m_text[m_pos++]);
    }
    return !out.empty();
}

bool PathParser::parse_quoted(std::string& out) {
    const char quote = peek();
    if (quote != '\'' && quote != '"') return false;
    ++m_pos;
    out.clear();
    while (m_pos < m_text.size() && m_text[m_pos] != quote) {
        out.push_back(m_text[m_pos++]);
    }
    if (m_pos >= m_text.size()) return false;
    ++m_pos; // closing quote
    return true;
}

bool PathParser::parse_predicate(Predicate& pred) {
    skip_space();
    // [last()]
    if (m_text.substr(m_pos).rfind("last()", 0) == 0) {
        m_pos += 6;
        skip_space();
        if (peek() != ']') return fail("unsupported predicate after last()");
        pred.kind = Predicate::Kind::Last;
        return true;
    }
    // [3]
    if (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
        i32 value = 0;
        while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
            value = value * 10 + (m_text[m_pos++] - '0');
        }
        skip_space();
        if (peek() != ']') return fail("unsupported numeric predicate");
        pred.kind     = Predicate::Kind::Position;
        pred.position = value;
        return true;
    }
    const bool is_attr = peek() == '@';
    if (is_attr) ++m_pos;
    std::string name;
    if (!parse_name(name)) return fail("predicate is not a name test");
    skip_space();
    if (peek() == ']') {
        pred.kind = is_attr ? Predicate::Kind::HasAttribute
                            : Predicate::Kind::HasChild;
        pred.name = std::move(name);
        return true;
    }
    if (peek() != '=') return fail("only '=' comparisons are supported");
    ++m_pos;
    skip_space();
    std::string value;
    if (!parse_quoted(value)) {
        return fail("comparison value must be a quoted literal");
    }
    skip_space();
    if (peek() != ']') return fail("trailing tokens in predicate");
    pred.kind  = is_attr ? Predicate::Kind::AttributeEquals
                         : Predicate::Kind::ChildEquals;
    pred.name  = std::move(name);
    pred.value = std::move(value);
    return true;
}

bool PathParser::parse_step(Step& step) {
    skip_space();
    if (peek() == '@') {
        ++m_pos;
        step.axis = Axis::Attribute;
        if (peek() == '*') {
            ++m_pos;
            step.test = NodeTest::Any;
        } else if (!parse_name(step.name)) {
            return fail("expected an attribute name after '@'");
        } else if (step.name.back() == ':' && peek() == '*') {
            ++m_pos;
            step.name.pop_back();
            step.test = NodeTest::NamespaceAny;
        } else {
            step.test = NodeTest::Name;
        }
    } else if (m_text.substr(m_pos).rfind("..", 0) == 0) {
        m_pos += 2;
        step.axis = Axis::Parent;
        step.test = NodeTest::AnyNode;
    } else if (peek() == '.' && !is_name_char(
                                    static_cast<unsigned char>(peek(1)))) {
        ++m_pos;
        step.axis = Axis::Self;
        step.test = NodeTest::AnyNode;
    } else if (peek() == '*') {
        ++m_pos;
        step.test = NodeTest::Any;
    } else if (m_text.substr(m_pos).rfind("text()", 0) == 0) {
        m_pos += 6;
        step.test = NodeTest::Text;
    } else if (m_text.substr(m_pos).rfind("node()", 0) == 0) {
        m_pos += 6;
        step.test = NodeTest::AnyNode;
    } else if (m_text.substr(m_pos).rfind("comment()", 0) == 0) {
        m_pos += 9;
        step.test = NodeTest::Comment;
    } else {
        if (!parse_name(step.name)) return fail("expected a node test");
        if (step.name.back() == ':' && peek() == '*') {
            ++m_pos;
            step.name.pop_back();
            step.test = NodeTest::NamespaceAny;
            goto predicates;
        }
        // Reject an axis specifier or function call this evaluator does not
        // implement rather than mis-reading it as an element name.
        if (peek() == '(') return fail("unsupported function call in path");
        if (peek() == ':' && peek(1) == ':') {
            return fail("explicit axis specifiers are not supported");
        }
        step.test = NodeTest::Name;
    }

predicates:
    while (true) {
        skip_space();
        if (peek() != '[') break;
        ++m_pos;
        Predicate pred;
        if (!parse_predicate(pred)) return false;
        if (peek() != ']') return fail("unterminated predicate");
        ++m_pos;
        step.predicates.push_back(std::move(pred));
    }
    return true;
}

bool PathParser::parse(std::vector<Step>& steps, bool& absolute) {
    skip_space();
    absolute = false;
    if (peek() == '/') {
        absolute = true;
        ++m_pos;
        if (peek() == '/') {
            ++m_pos;
            Step step;
            if (!parse_step(step)) return false;
            push_descendant(steps, std::move(step));
        } else if (m_pos >= m_text.size()) {
            return true; // "/" selects the document root itself.
        } else {
            Step step;
            if (!parse_step(step)) return false;
            steps.push_back(std::move(step));
        }
    } else {
        Step step;
        if (!parse_step(step)) return false;
        steps.push_back(std::move(step));
    }

    while (true) {
        skip_space();
        if (m_pos >= m_text.size()) return true;
        if (peek() != '/') {
            return fail("unexpected token '" + std::string(1, peek()) + "'");
        }
        ++m_pos;
        Step step;
        if (peek() == '/') {
            ++m_pos;
            if (!parse_step(step)) return false;
            push_descendant(steps, std::move(step));
            continue;
        }
        if (!parse_step(step)) return false;
        steps.push_back(std::move(step));
    }
}

[[nodiscard]] const std::string* lookup_prefix(const SelectionOptions& o,
                                               std::string_view prefix) {
    for (const auto& [p, uri] : o.namespaces) {
        if (p == prefix) return &uri;
    }
    return nullptr;
}

/// Name test @p test (possibly prefixed) against @p node's name, per the
/// selection language (see SelectionOptions).
[[nodiscard]] bool qname_matches(const Node& node, std::string_view test,
                                 const SelectionOptions& o) {
    const usize colon = test.find(':');
    if (colon == std::string_view::npos) {
        if (o.xpath) return node.namespace_uri().empty() && node.name() == test;
        return node.name() == test;
    }
    const std::string_view prefix = test.substr(0, colon);
    const std::string_view local  = test.substr(colon + 1);
    if (const std::string* uri = lookup_prefix(o, prefix)) {
        return node.namespace_uri() == *uri && node.base_name() == local;
    }
    // XPath rejects undeclared prefixes before evaluation; XSLPattern falls
    // back to the literal qualified name.
    return !o.xpath && node.name() == test;
}

[[nodiscard]] bool namespace_wildcard_matches(const Node& node,
                                              std::string_view prefix,
                                              const SelectionOptions& o) {
    if (const std::string* uri = lookup_prefix(o, prefix)) {
        return node.namespace_uri() == *uri;
    }
    return !o.xpath && node.prefix() == prefix;
}

[[nodiscard]] bool matches_test(const Node& node, const Step& step,
                                const SelectionOptions& o) {
    // Under XPath, namespace declarations are not on the attribute axis.
    if (step.axis == Axis::Attribute && o.xpath &&
        node.is_namespace_declaration()) {
        return false;
    }
    switch (step.test) {
        case NodeTest::Any:
            return step.axis == Axis::Attribute
                       ? node.type() == NodeType::Attribute
                       : node.type() == NodeType::Element;
        case NodeTest::NamespaceAny:
            if (step.axis == Axis::Attribute
                    ? node.type() != NodeType::Attribute
                    : node.type() != NodeType::Element) {
                return false;
            }
            return namespace_wildcard_matches(node, step.name, o);
        case NodeTest::Text:
            return node.type() == NodeType::Text ||
                   node.type() == NodeType::CDATASection;
        case NodeTest::Comment:
            return node.type() == NodeType::Comment;
        case NodeTest::AnyNode:
            return true;
        case NodeTest::Name:
            break;
    }
    if (step.axis == Axis::Attribute) {
        return node.type() == NodeType::Attribute &&
               qname_matches(node, step.name, o);
    }
    return node.type() == NodeType::Element && qname_matches(node, step.name, o);
}

[[nodiscard]] Node::Ptr find_attribute_matching(const Node& node,
                                                std::string_view name,
                                                const SelectionOptions& o) {
    for (const auto& attr : node.attributes()) {
        if (o.xpath && attr->is_namespace_declaration()) continue;
        if (qname_matches(*attr, name, o)) return attr;
    }
    return nullptr;
}

/// The string value a predicate compares against for a child-element test.
[[nodiscard]] Node::Ptr find_child_element(const Node& node,
                                           std::string_view name,
                                           const SelectionOptions& o) {
    for (const auto& child : node.children()) {
        if (child->type() == NodeType::Element &&
            qname_matches(*child, name, o)) {
            return child;
        }
    }
    return nullptr;
}

[[nodiscard]] bool passes_predicate(const Node::Ptr& node, const Predicate& pred,
                                    usize index, usize count,
                                    const SelectionOptions& o) {
    switch (pred.kind) {
        case Predicate::Kind::Position:
            return static_cast<i32>(index + 1) == pred.position;
        case Predicate::Kind::Last:
            return index + 1 == count;
        case Predicate::Kind::HasAttribute:
            return find_attribute_matching(*node, pred.name, o) != nullptr;
        case Predicate::Kind::AttributeEquals: {
            auto attr = find_attribute_matching(*node, pred.name, o);
            return attr && attr->value() == pred.value;
        }
        case Predicate::Kind::HasChild:
            return find_child_element(*node, pred.name, o) != nullptr;
        case Predicate::Kind::ChildEquals: {
            auto child = find_child_element(*node, pred.name, o);
            return child && child->text() == pred.value;
        }
    }
    return false;
}

void collect_descendants(const Node::Ptr& node, std::vector<Node::Ptr>& out) {
    for (const auto& child : node->children()) {
        out.push_back(child);
        collect_descendants(child, out);
    }
}

std::vector<Node::Ptr> apply_step(const std::vector<Node::Ptr>& context,
                                  const Step& step,
                                  const SelectionOptions& o) {
    std::vector<Node::Ptr> candidates;
    for (const auto& node : context) {
        switch (step.axis) {
            case Axis::Child:
                for (const auto& child : node->children()) {
                    candidates.push_back(child);
                }
                break;
            case Axis::Descendant:
                collect_descendants(node, candidates);
                break;
            case Axis::DescendantOrSelf:
                candidates.push_back(node);
                collect_descendants(node, candidates);
                break;
            case Axis::Attribute:
                for (const auto& attr : node->attributes()) {
                    candidates.push_back(attr);
                }
                break;
            case Axis::Self:
                candidates.push_back(node);
                break;
            case Axis::Parent:
                if (auto up = node->parent()) candidates.push_back(up);
                break;
        }
    }

    std::vector<Node::Ptr> matched;
    for (const auto& node : candidates) {
        if (matches_test(*node, step, o)) matched.push_back(node);
    }

    // Predicates filter the matched set in order; positions are 1-based and
    // refer to the set produced by the preceding predicate.
    for (const auto& pred : step.predicates) {
        std::vector<Node::Ptr> kept;
        for (usize i = 0; i < matched.size(); ++i) {
            if (passes_predicate(matched[i], pred, i, matched.size(), o)) {
                kept.push_back(matched[i]);
            }
        }
        matched = std::move(kept);
    }
    return matched;
}

/// Document order for the final result. Comparing ancestor chains keeps this
/// correct for nodes from different branches without numbering the whole tree.
[[nodiscard]] std::vector<const Node*> ancestor_chain(const Node::Ptr& node) {
    std::vector<const Node*> chain;
    for (auto walk = node; walk; walk = walk->parent()) {
        chain.push_back(walk.get());
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

[[nodiscard]] usize index_in_parent(const Node* node, const Node* parent) {
    if (parent == nullptr) return 0;
    const auto& kids = parent->children();
    for (usize i = 0; i < kids.size(); ++i) {
        if (kids[i].get() == node) return i;
    }
    const auto& attrs = parent->attributes();
    for (usize i = 0; i < attrs.size(); ++i) {
        // Attributes sort before child nodes, matching document order in the
        // DOM's traversal of an element.
        if (attrs[i].get() == node) return i;
    }
    return 0;
}

[[nodiscard]] bool precedes(const Node::Ptr& a, const Node::Ptr& b) {
    const auto chain_a = ancestor_chain(a);
    const auto chain_b = ancestor_chain(b);
    const usize common = std::min(chain_a.size(), chain_b.size());
    for (usize i = 0; i < common; ++i) {
        if (chain_a[i] == chain_b[i]) continue;
        const Node* parent = i == 0 ? nullptr : chain_a[i - 1];
        const bool a_is_attr = chain_a[i]->type() == NodeType::Attribute;
        const bool b_is_attr = chain_b[i]->type() == NodeType::Attribute;
        if (a_is_attr != b_is_attr) return a_is_attr;
        return index_in_parent(chain_a[i], parent) <
               index_in_parent(chain_b[i], parent);
    }
    return chain_a.size() < chain_b.size();
}

/// Under XPath every prefix an expression uses must be declared in
/// SelectionNamespaces; returns the first undeclared one.
[[nodiscard]] std::optional<std::string> undeclared_prefix(
    const std::vector<Step>& steps, const SelectionOptions& o) {
    const auto check = [&](const std::string& name,
                           bool whole_is_prefix) -> std::optional<std::string> {
        std::string prefix;
        if (whole_is_prefix) {
            prefix = name;
        } else if (const usize colon = name.find(':');
                   colon != std::string::npos) {
            prefix = name.substr(0, colon);
        } else {
            return std::nullopt;
        }
        if (lookup_prefix(o, prefix) == nullptr) return prefix;
        return std::nullopt;
    };
    for (const auto& step : steps) {
        if (step.test == NodeTest::Name || step.test == NodeTest::NamespaceAny) {
            if (auto p = check(step.name, step.test == NodeTest::NamespaceAny)) {
                return p;
            }
        }
        for (const auto& pred : step.predicates) {
            if (pred.kind == Predicate::Kind::Position ||
                pred.kind == Predicate::Kind::Last) {
                continue;
            }
            if (auto p = check(pred.name, false)) return p;
        }
    }
    return std::nullopt;
}

} // namespace

XPathResult select_nodes(const Node::Ptr& context,
                         std::string_view expression) {
    return select_nodes(context, expression, SelectionOptions{});
}

XPathResult select_nodes(const Node::Ptr& context,
                         std::string_view expression,
                         const SelectionOptions& options) {
    XPathResult result;
    if (!context) return result;

    // Split on '|' at the top level. The supported grammar has no other use of
    // '|', so a plain scan is sufficient.
    std::vector<std::string_view> branches;
    usize begin = 0;
    bool  in_quote = false;
    char  quote = '\0';
    for (usize i = 0; i < expression.size(); ++i) {
        const char c = expression[i];
        if (in_quote) {
            if (c == quote) in_quote = false;
            continue;
        }
        if (c == '\'' || c == '"') {
            in_quote = true;
            quote    = c;
            continue;
        }
        if (c == '|') {
            branches.push_back(expression.substr(begin, i - begin));
            begin = i + 1;
        }
    }
    branches.push_back(expression.substr(begin));

    auto root = context;
    while (auto up = root->parent()) root = up;

    for (const auto& branch : branches) {
        std::vector<Step> steps;
        bool              absolute = false;
        PathParser        parser(branch);
        if (!parser.parse(steps, absolute)) {
            result.supported          = false;
            result.unsupported_reason = parser.reason();
            result.nodes.clear();
            return result;
        }
        if (options.xpath) {
            if (auto prefix = undeclared_prefix(steps, options)) {
                result.invalid        = true;
                result.invalid_reason =
                    "Reference to undeclared namespace prefix: '" + *prefix +
                    "'.";
                result.nodes.clear();
                return result;
            }
        }
        std::vector<Node::Ptr> current{absolute ? root : context};
        for (const auto& step : steps) {
            current = apply_step(current, step, options);
            if (current.empty()) break;
        }
        for (const auto& node : current) {
            if (std::find(result.nodes.begin(), result.nodes.end(), node) ==
                result.nodes.end()) {
                result.nodes.push_back(node);
            }
        }
    }

    std::sort(result.nodes.begin(), result.nodes.end(), precedes);
    return result;
}

} // namespace vitreo::msxml

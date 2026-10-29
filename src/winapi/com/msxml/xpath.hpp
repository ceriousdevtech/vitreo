// SPDX-License-Identifier: LGPL-2.1-only

/// @file xpath.hpp
/// @brief The XPath subset behind selectNodes / selectSingleNode.
///
/// MSXML evaluates the abbreviated XPath syntax against the DOM. This
/// implementation covers the location-path grammar that document-reading
/// callers actually use:
///
///   * absolute and relative paths: `/a/b`, `a/b`, `//b`, `a//b`
///   * node tests: name, `*`, `text()`, `node()`, `comment()`
///   * the attribute axis: `@name`, `@*`
///   * self and parent steps: `.`, `..`
///   * predicates: `[3]`, `[@a]`, `[@a='v']`, `[b]`, `[b='v']`, `[last()]`
///   * unions: `expr | expr`
///
/// Anything outside that grammar is **rejected**, not silently evaluated to an
/// empty node set. An empty result is a legitimate XPath answer, so quietly
/// returning one for an expression this code cannot parse would tell the caller
/// "no matches" when the truth is "not understood" — the kind of lie that makes
/// an installer skip work and report success.

#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "winapi/com/msxml/xml_dom.hpp"

namespace vitreo::msxml {

/// How selectNodes interprets names: the document's SelectionLanguage and the
/// prefixes SelectionNamespaces declares.
///
/// Measured on Windows (tools/oracle_probe/msxml_document2_probe.expected):
///   * XPath: an unprefixed name test matches only nodes in no namespace; a
///     prefix must be declared in SelectionNamespaces (otherwise the
///     expression is invalid: E_FAIL) and matches by namespace URI + local
///     name; namespace declarations are not attributes.
///   * XSLPattern (MSXML 3.0's default): an unprefixed name test matches the
///     node name whatever its namespace; a prefix declared in
///     SelectionNamespaces matches by namespace URI, an undeclared one matches
///     the qualified name literally; `xmlns` declarations are attributes.
///   * A default namespace (`xmlns='uri'`) in SelectionNamespaces never
///     applies to unprefixed name tests in either language.
struct SelectionOptions {
    bool xpath = false;
    /// prefix -> namespace URI, from SelectionNamespaces' `xmlns:p='uri'`.
    std::vector<std::pair<std::string, std::string>> namespaces;
};

struct XPathResult {
    /// Selected nodes, in document order, duplicates removed.
    std::vector<Node::Ptr> nodes;
    /// False when the expression uses syntax this evaluator does not implement.
    /// Callers must surface an error rather than treating it as "no matches".
    bool supported = true;
    /// Human-readable cause, set when @ref supported is false.
    std::string unsupported_reason;
    /// True when the expression is invalid for the selection language (an
    /// undeclared prefix under XPath). MSXML fails these with E_FAIL; this is
    /// the contract, not a gap in this evaluator.
    bool invalid = false;
    std::string invalid_reason;
};

/// Evaluate @p expression with @p context as the context node.
[[nodiscard]] XPathResult select_nodes(const Node::Ptr& context,
                                       std::string_view expression,
                                       const SelectionOptions& options);

/// XSLPattern with no SelectionNamespaces.
[[nodiscard]] XPathResult select_nodes(const Node::Ptr& context,
                                       std::string_view expression);

} // namespace vitreo::msxml

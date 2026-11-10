// SPDX-License-Identifier: LGPL-2.1-only
/// @file sxs_manifest.cpp
/// @brief Side-by-side manifest and configuration file parsing (see header).

#include "winapi/common/sxs/sxs_manifest.hpp"

#include <cctype>
#include <cstdio>
#include <map>
#include <memory>

namespace vitreo::winapi::sxs {

bool iequal(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (usize i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::optional<Version> Version::parse(std::string_view text) {
    Version v;
    usize part = 0;
    usize i = 0;
    while (part < 4) {
        if (i >= text.size() || !std::isdigit(static_cast<unsigned char>(text[i]))) return std::nullopt;
        u32 value = 0;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) {
            value = value * 10 + static_cast<u32>(text[i] - '0');
            if (value > 0xFFFF) return std::nullopt;
            ++i;
        }
        v.parts[part++] = static_cast<u16>(value);
        if (part < 4) {
            if (i >= text.size() || text[i] != '.') return std::nullopt;
            ++i;
        }
    }
    if (i != text.size()) return std::nullopt;
    return v;
}

std::string Version::str() const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", parts[0], parts[1], parts[2], parts[3]);
    return buffer;
}

namespace {

constexpr std::string_view kAsmV1 = "urn:schemas-microsoft-com:asm.v1";
constexpr std::string_view kAsmV2 = "urn:schemas-microsoft-com:asm.v2";
constexpr std::string_view kAsmV3 = "urn:schemas-microsoft-com:asm.v3";

bool assembly_namespace(std::string_view uri) {
    return uri == kAsmV1 || uri == kAsmV2 || uri == kAsmV3;
}

// ---- A minimal namespace-aware XML reader ---------------------------------
//
// Elements, attributes (single or double quoted), self-closing tags, the five
// predefined entities and numeric character references, comments, processing
// instructions, CDATA and DOCTYPE. Text content is not kept: no element the
// side-by-side schema defines carries meaning in its text.

struct Element {
    std::string ns;      // namespace URI
    std::string local;   // local name
    std::map<std::string, std::string> attributes;  // unprefixed attribute local names
    std::vector<std::unique_ptr<Element>> children;
};

void append_utf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::optional<std::string> to_utf8(std::span<const u8> bytes) {
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return std::string(reinterpret_cast<const char*>(bytes.data() + 3), bytes.size() - 3);
    }
    const bool le = bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE;
    const bool be = bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF;
    if (!le && !be) return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string out;
    for (usize i = 2; i + 1 < bytes.size(); i += 2) {
        const auto at = [&](usize k) { return static_cast<u32>(bytes[k]); };
        u32 unit = le ? (at(i) | (at(i + 1) << 8)) : ((at(i) << 8) | at(i + 1));
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
            const u32 low = le ? (at(i + 2) | (at(i + 3) << 8)) : ((at(i + 2) << 8) | at(i + 3));
            if (low < 0xDC00 || low > 0xDFFF) return std::nullopt;
            unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
            i += 2;
        }
        append_utf8(out, unit);
    }
    return out;
}

class XmlReader {
public:
    explicit XmlReader(std::string_view text) : m_text(text) {}

    std::unique_ptr<Element> parse_document() {
        std::unique_ptr<Element> root;
        std::vector<std::map<std::string, std::string>> scopes;
        std::vector<Element*> stack;
        std::vector<std::string> open_names;
        while (m_pos < m_text.size()) {
            if (m_text[m_pos] != '<') {
                ++m_pos;
                continue;
            }
            if (starts("<!--")) {
                if (!skip_to("-->")) return nullptr;
                continue;
            }
            if (starts("<![CDATA[")) {
                if (!skip_to("]]>")) return nullptr;
                continue;
            }
            if (starts("<?")) {
                if (!skip_to("?>")) return nullptr;
                continue;
            }
            if (starts("<!")) {
                if (!skip_to(">")) return nullptr;
                continue;
            }
            if (starts("</")) {
                m_pos += 2;
                const std::string name = read_name();
                skip_space();
                if (m_pos >= m_text.size() || m_text[m_pos] != '>') return nullptr;
                ++m_pos;
                if (open_names.empty() || open_names.back() != name) return nullptr;
                open_names.pop_back();
                stack.pop_back();
                scopes.pop_back();
                continue;
            }
            ++m_pos;
            const std::string qname = read_name();
            if (qname.empty()) return nullptr;
            std::vector<std::pair<std::string, std::string>> raw;
            bool self_closing = false;
            for (;;) {
                skip_space();
                if (m_pos >= m_text.size()) return nullptr;
                if (m_text[m_pos] == '>') { ++m_pos; break; }
                if (starts("/>")) { m_pos += 2; self_closing = true; break; }
                const std::string attr = read_name();
                if (attr.empty()) return nullptr;
                skip_space();
                if (m_pos >= m_text.size() || m_text[m_pos] != '=') return nullptr;
                ++m_pos;
                skip_space();
                auto value = read_quoted();
                if (!value) return nullptr;
                raw.emplace_back(attr, std::move(*value));
            }
            std::map<std::string, std::string> scope = scopes.empty() ? std::map<std::string, std::string>{} : scopes.back();
            for (const auto& [k, v] : raw) {
                if (k == "xmlns") scope[""] = v;
                else if (k.rfind("xmlns:", 0) == 0) scope[k.substr(6)] = v;
            }
            auto element = std::make_unique<Element>();
            const usize colon = qname.find(':');
            const std::string prefix = colon == std::string::npos ? std::string{} : qname.substr(0, colon);
            element->local = colon == std::string::npos ? qname : qname.substr(colon + 1);
            if (auto it = scope.find(prefix); it != scope.end()) element->ns = it->second;
            else if (!prefix.empty()) return nullptr;  // undeclared prefix
            for (const auto& [k, v] : raw) {
                if (k == "xmlns" || k.rfind("xmlns:", 0) == 0) continue;
                if (k.find(':') != std::string::npos) continue;  // qualified attributes are other schemas'
                element->attributes[k] = v;
            }
            Element* raw_ptr = element.get();
            if (stack.empty()) {
                if (root) return nullptr;  // second document element
                root = std::move(element);
            } else {
                stack.back()->children.push_back(std::move(element));
            }
            if (!self_closing) {
                stack.push_back(raw_ptr);
                scopes.push_back(std::move(scope));
                open_names.push_back(qname);
            }
        }
        if (!stack.empty()) return nullptr;
        return root;
    }

private:
    bool starts(std::string_view s) const { return m_text.substr(m_pos, s.size()) == s; }
    bool skip_to(std::string_view s) {
        const usize end = m_text.find(s, m_pos);
        if (end == std::string_view::npos) return false;
        m_pos = end + s.size();
        return true;
    }
    void skip_space() {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' ||
                                         m_text[m_pos] == '\r' || m_text[m_pos] == '\n')) {
            ++m_pos;
        }
    }
    std::string read_name() {
        const usize start = m_pos;
        while (m_pos < m_text.size()) {
            const char c = m_text[m_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/' || c == '=' ||
                c == '<' || c == '"' || c == '\'') {
                break;
            }
            ++m_pos;
        }
        return std::string(m_text.substr(start, m_pos - start));
    }
    std::optional<std::string> read_quoted() {
        if (m_pos >= m_text.size() || (m_text[m_pos] != '"' && m_text[m_pos] != '\'')) return std::nullopt;
        const char quote = m_text[m_pos++];
        const usize end = m_text.find(quote, m_pos);
        if (end == std::string_view::npos) return std::nullopt;
        std::string_view raw = m_text.substr(m_pos, end - m_pos);
        m_pos = end + 1;
        std::string out;
        for (usize i = 0; i < raw.size(); ++i) {
            if (raw[i] == '<') return std::nullopt;
            if (raw[i] != '&') {
                out.push_back(raw[i]);
                continue;
            }
            const usize semi = raw.find(';', i);
            if (semi == std::string_view::npos) return std::nullopt;
            const std::string_view entity = raw.substr(i + 1, semi - i - 1);
            if (entity == "amp") out.push_back('&');
            else if (entity == "lt") out.push_back('<');
            else if (entity == "gt") out.push_back('>');
            else if (entity == "quot") out.push_back('"');
            else if (entity == "apos") out.push_back('\'');
            else if (entity.size() > 1 && entity[0] == '#') {
                u32 cp = 0;
                const bool hex = entity[1] == 'x' || entity[1] == 'X';
                for (usize k = hex ? 2 : 1; k < entity.size(); ++k) {
                    const char c = entity[k];
                    u32 d;
                    if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
                    else if (hex && c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
                    else if (hex && c >= 'A' && c <= 'F') d = static_cast<u32>(c - 'A' + 10);
                    else return std::nullopt;
                    cp = cp * (hex ? 16u : 10u) + d;
                    if (cp > 0x10FFFF) return std::nullopt;
                }
                append_utf8(out, cp);
            } else {
                return std::nullopt;
            }
            i = semi;
        }
        return out;
    }

    std::string_view m_text;
    usize m_pos = 0;
};

const std::string* attr(const Element& e, const char* name) {
    auto it = e.attributes.find(name);
    return it == e.attributes.end() ? nullptr : &it->second;
}

bool is(const Element& e, std::string_view local) {
    return assembly_namespace(e.ns) && e.local == local;
}

std::optional<Identity> read_identity(const Element& e) {
    Identity id;
    if (const auto* v = attr(e, "type")) id.type = *v;
    if (const auto* v = attr(e, "name")) id.name = *v;
    if (const auto* v = attr(e, "version")) {
        id.version = Version::parse(*v);
        if (!id.version) return std::nullopt;
    }
    if (const auto* v = attr(e, "processorArchitecture")) id.architecture = *v;
    if (const auto* v = attr(e, "publicKeyToken")) id.public_key_token = to_lower(*v);
    if (const auto* v = attr(e, "language")) id.language = *v;
    if (id.name.empty()) return std::nullopt;
    return id;
}

std::optional<BindingRedirect> read_redirect(const Element& e) {
    const auto* old_version = attr(e, "oldVersion");
    const auto* new_version = attr(e, "newVersion");
    if (old_version == nullptr || new_version == nullptr) return std::nullopt;
    BindingRedirect r;
    const usize dash = old_version->find('-');
    const auto low = Version::parse(std::string_view(*old_version).substr(0, dash));
    const auto high = dash == std::string::npos ? low : Version::parse(std::string_view(*old_version).substr(dash + 1));
    const auto target = Version::parse(*new_version);
    if (!low || !high || !target || *high < *low) return std::nullopt;
    r.old_low = *low;
    r.old_high = *high;
    r.new_version = *target;
    return r;
}

bool publisher_policy_off(const Element& e) {
    const auto* apply = attr(e, "apply");
    return apply != nullptr && iequal(*apply, "no");
}

// A dependentAssembly: its identity, redirects and per-assembly policy switch.
std::optional<Dependency> read_dependent(const Element& e, bool optional) {
    Dependency d;
    d.optional = optional;
    bool have_identity = false;
    for (const auto& child : e.children) {
        if (is(*child, "assemblyIdentity")) {
            auto id = read_identity(*child);
            if (!id) return std::nullopt;
            d.identity = std::move(*id);
            have_identity = true;
        } else if (is(*child, "bindingRedirect")) {
            auto r = read_redirect(*child);
            if (!r) return std::nullopt;
            d.redirects.push_back(*r);
        } else if (is(*child, "publisherPolicy")) {
            d.publisher_policy_disabled = publisher_policy_off(*child);
        }
    }
    if (!have_identity) return std::nullopt;
    return d;
}

std::unique_ptr<Element> parse_xml(std::span<const u8> bytes) {
    auto text = to_utf8(bytes);
    if (!text) return nullptr;
    return XmlReader(*text).parse_document();
}

}  // namespace

std::optional<Manifest> parse_manifest(std::span<const u8> bytes) {
    const auto root = parse_xml(bytes);
    if (!root || !is(*root, "assembly")) return std::nullopt;
    Manifest m;
    for (const auto& child : root->children) {
        if (is(*child, "assemblyIdentity")) {
            if (m.identity) return std::nullopt;  // one identity per manifest
            auto id = read_identity(*child);
            if (!id) return std::nullopt;
            m.identity = std::move(*id);
        } else if (is(*child, "file")) {
            const auto* name = attr(*child, "name");
            if (name == nullptr || name->empty()) return std::nullopt;
            m.files.push_back(*name);
            for (const auto& item : child->children)
                for (const char* kind : {"comClass", "typelib", "comInterfaceProxyStub", "windowClass"})
                    if (is(*item, kind)) m.unbuilt_sections.emplace_back(kind);
        } else if (is(*child, "comInterfaceExternalProxyStub") || is(*child, "clrClass") ||
                   is(*child, "clrSurrogate")) {
            m.unbuilt_sections.push_back(child->local);
        } else if (is(*child, "noInheritable")) {
            m.no_inheritable = true;
        } else if (is(*child, "dependency")) {
            const auto* opt = attr(*child, "optional");
            const bool optional = opt != nullptr && iequal(*opt, "yes");
            for (const auto& dep : child->children) {
                if (!is(*dep, "dependentAssembly")) continue;
                auto d = read_dependent(*dep, optional);
                if (!d) return std::nullopt;
                m.dependencies.push_back(std::move(*d));
            }
        }
    }
    return m;
}

std::optional<AppConfig> parse_app_config(std::span<const u8> bytes) {
    const auto root = parse_xml(bytes);
    if (!root) return std::nullopt;
    AppConfig config;
    if (root->local != "configuration") return config;
    for (const auto& windows : root->children) {
        if (windows->local != "windows" || !windows->ns.empty()) continue;
        for (const auto& binding : windows->children) {
            if (!is(*binding, "assemblyBinding")) continue;
            for (const auto& item : binding->children) {
                if (is(*item, "publisherPolicy")) {
                    config.publisher_policy_disabled = publisher_policy_off(*item);
                } else if (is(*item, "dependentAssembly")) {
                    auto d = read_dependent(*item, false);
                    if (!d) return std::nullopt;
                    config.dependencies.push_back(std::move(*d));
                }
            }
        }
    }
    return config;
}

}  // namespace vitreo::winapi::sxs

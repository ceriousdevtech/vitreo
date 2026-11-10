// SPDX-License-Identifier: LGPL-2.1-only
/// @file crt_type_name.cpp
/// @brief msvcrt.dll type_info::name() undecoration (see crt_type_name.hpp).
///
/// The encoding is the Microsoft C++ data-type grammar: built-in letters,
/// T/U/V/W user-defined types with '@'-terminated qualified names, P/Q/R/S/A/B
/// pointers and references (E = __ptr64, I = __restrict, F = __unaligned),
/// '6' / '8' function and member-function pointees, Q-T member data pointees,
/// 'Y' arrays, ?$ templates with $0/$1/$E/$$C/$$B/$$A arguments, back-
/// references for names and argument types, and nested function symbols for
/// function-local types. Parsing builds a small tree; rendering then applies
/// the C declarator rules ("void (__cdecl**)(void)", "int (*)[3]"), because a
/// return or element type is printed around text that is parsed after it.
///
/// Every rendering rule here is pinned by an encoding in
/// tools/oracle_probe/crt_legacy_exports_probe{32,64}.expected.

#include "winapi/msvcrt/runtime/crt_type_name.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace vitreo::winapi {

namespace {

struct Node;
using NodeP = std::shared_ptr<Node>;

struct Node {
    enum class Kind { Missing, Simple, Pointer, Function, Array };
    Kind kind{Kind::Missing};
    // Simple: the base text and its cv suffix (" const").
    std::string text;
    std::string cv;
    // Pointer: "*", "&" or "Foo::*", then modifiers and its own cv.
    std::string token;
    NodeP pointee;
    // Function.
    bool unknown_convention{false};
    std::string convention;
    std::string member_class;  // member-function pointee: "Foo"
    std::string symbol_name;   // nested function symbol: "Foo::g"
    bool has_return{true};
    NodeP ret;
    std::string args;
    std::string this_cv;  // "const"
    std::string throw_spec;
    // Array.
    std::string array_cv;  // "const " (inside the declarator parentheses)
    std::string dims;      // "[3][4]"
    NodeP element;
};

/// The declarator a type is printed around: pointer tokens (innermost first)
/// and the text that follows them (an enclosing function's parameter list).
struct Decl {
    std::vector<std::string> ptrs;
    std::string tail;
};

NodeP make(Node::Kind kind) {
    auto node = std::make_shared<Node>();
    node->kind = kind;
    return node;
}

NodeP simple(std::string text) {
    auto node = make(Node::Kind::Simple);
    node->text = std::move(text);
    return node;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += sep;
        out += parts[i];
    }
    return out;
}

std::string with_declarator(const std::string& base, const Decl& decl) {
    std::string out = base;
    if (!decl.ptrs.empty()) {
        out += ' ';
        out += join(decl.ptrs, " ");
    }
    if (!decl.tail.empty()) {
        out += ' ';
        out += decl.tail;
    }
    return out;
}

std::string render(const NodeP& node, const Decl& decl) {
    switch (node->kind) {
    case Node::Kind::Missing:
        return with_declarator(" ??", decl);
    case Node::Kind::Simple:
        return with_declarator(node->text + node->cv, decl);
    case Node::Kind::Pointer: {
        Decl inner;
        inner.ptrs.reserve(decl.ptrs.size() + 1);
        inner.ptrs.push_back(node->token);
        inner.ptrs.insert(inner.ptrs.end(), decl.ptrs.begin(), decl.ptrs.end());
        inner.tail = decl.tail;
        return render(node->pointee, inner);
    }
    case Node::Kind::Function: {
        // An unrecognised calling convention ends the undecoration after the
        // return type ("P6QXXZ" prints "void").
        if (node->unknown_convention) return render(node->ret, Decl{});
        const std::string params =
            "(" + node->args + ")" + node->this_cv + node->throw_spec;
        std::string text;
        if (!node->symbol_name.empty()) {
            text = node->convention;
            if (!text.empty()) text += ' ';
            text += node->symbol_name + params;
        } else if (decl.ptrs.empty() && decl.tail.empty()) {
            text = node->convention + params;
        } else {
            // Inside a function declarator the pointer tokens run together:
            // "(__cdecl** const)", "(__cdecl*&)", "(__thiscall Foo::*)".
            std::string inner = node->convention;
            if (!node->member_class.empty()) {
                if (!inner.empty()) inner += ' ';
                inner += node->member_class + "::";
            }
            for (const auto& p : decl.ptrs) inner += p;
            inner += decl.tail;
            text = "(" + inner + ")" + params;
        }
        if (!node->has_return) return text;
        return render(node->ret, Decl{{}, text});
    }
    case Node::Kind::Array: {
        const std::string inner =
            node->array_cv + join(decl.ptrs, " ") + decl.tail;
        const std::string tail =
            inner.empty() ? node->dims : "(" + inner + ")" + node->dims;
        return render(node->element, Decl{{}, tail});
    }
    }
    return {};
}

const char* builtin(char c) {
    switch (c) {
    case 'C': return "signed char";
    case 'D': return "char";
    case 'E': return "unsigned char";
    case 'F': return "short";
    case 'G': return "unsigned short";
    case 'H': return "int";
    case 'I': return "unsigned int";
    case 'J': return "long";
    case 'K': return "unsigned long";
    case 'M': return "float";
    case 'N': return "double";
    case 'O': return "long double";
    case 'X': return "void";
    default: return nullptr;
    }
}

const char* extended_builtin(char c) {
    switch (c) {
    case 'D': return "__int8";
    case 'E': return "unsigned __int8";
    case 'F': return "__int16";
    case 'G': return "unsigned __int16";
    case 'H': return "__int32";
    case 'I': return "unsigned __int32";
    case 'J': return "__int64";
    case 'K': return "unsigned __int64";
    case 'L': return "__int128";
    case 'M': return "unsigned __int128";
    case 'N': return "bool";
    case 'W': return "wchar_t";
    default: return "UNKNOWN";
    }
}

/// cv letters A-D as a suffix; nullptr for any other letter.
const char* cv_suffix(char c) {
    switch (c) {
    case 'A': return "";
    case 'B': return " const";
    case 'C': return " volatile";
    case 'D': return " const volatile";
    default: return nullptr;
    }
}

const char* convention(char c) {
    switch (c) {
    case 'A': case 'B': return "__cdecl";
    case 'C': case 'D': return "__pascal";
    case 'E': case 'F': return "__thiscall";
    case 'G': case 'H': return "__stdcall";
    case 'I': case 'J': return "__fastcall";
    case 'K': case 'L': return "";
    case 'M': case 'N': return "__clrcall";
    default: return nullptr;
    }
}

const char* operator_name(char c) {
    switch (c) {
    case '2': return "operator new";
    case '3': return "operator delete";
    case '4': return "operator=";
    case '5': return "operator>>";
    case '6': return "operator<<";
    case '7': return "operator!";
    case '8': return "operator==";
    case '9': return "operator!=";
    case 'A': return "operator[]";
    case 'C': return "operator->";
    case 'D': return "operator*";
    case 'E': return "operator++";
    case 'F': return "operator--";
    case 'G': return "operator-";
    case 'H': return "operator+";
    case 'I': return "operator&";
    case 'J': return "operator->*";
    case 'K': return "operator/";
    case 'L': return "operator%";
    case 'M': return "operator<";
    case 'N': return "operator<=";
    case 'O': return "operator>";
    case 'P': return "operator>=";
    case 'Q': return "operator,";
    case 'R': return "operator()";
    case 'S': return "operator~";
    case 'T': return "operator^";
    case 'U': return "operator|";
    case 'V': return "operator&&";
    case 'W': return "operator||";
    case 'X': return "operator*=";
    case 'Y': return "operator+=";
    case 'Z': return "operator-=";
    default: return nullptr;
    }
}

const char* extended_operator_name(char c) {
    switch (c) {
    case '0': return "operator/=";
    case '1': return "operator%=";
    case '2': return "operator>>=";
    case '3': return "operator<<=";
    case '4': return "operator&=";
    case '5': return "operator|=";
    case '6': return "operator^=";
    case 'U': return "operator new[]";
    case 'V': return "operator delete[]";
    default: return nullptr;
    }
}

class Parser {
public:
    explicit Parser(std::string_view input) : in_(input) {}

    enum class Mode { TopLevel, Pointee, Arg };

    NodeP type(Mode mode);

    [[nodiscard]] bool invalid() const { return invalid_; }
    [[nodiscard]] bool stopped() const { return stopped_; }
    [[nodiscard]] bool truncated() const { return truncated_; }
    [[nodiscard]] bool fully_consumed() const { return pos_ >= in_.size(); }

private:
    struct Number {
        bool ok{false};
        bool negative{false};
        std::uint64_t value{0};
    };

    struct Depth {
        explicit Depth(Parser& p) : parser(p) {
            if (++parser.depth_ > 64) parser.invalid_ = true;
        }
        ~Depth() { --parser.depth_; }
        Parser& parser;
    };

    [[nodiscard]] bool done() const {
        return invalid_ || stopped_ || pos_ >= in_.size();
    }
    /// End of input where something was required: the "??" markers.
    [[nodiscard]] bool at_end() {
        if (pos_ < in_.size() && !stopped_) return false;
        if (!stopped_) truncated_ = true;
        return true;
    }
    char peek() const { return pos_ < in_.size() ? in_[pos_] : '\0'; }
    char get() { return pos_ < in_.size() ? in_[pos_++] : '\0'; }
    NodeP fail() {
        invalid_ = true;
        return make(Node::Kind::Missing);
    }
    void remember_name(const std::string& name) {
        if (names_.size() < 10) names_.push_back(name);
    }
    void remember_type(const std::string& type) {
        if (types_.size() < 10) types_.push_back(type);
    }

    NodeP pointer(char kind);
    NodeP array(std::string array_cv);
    NodeP function_type(std::string member_class, std::string this_cv);
    NodeP return_type();
    void function_tail(Node& fn);
    std::string arg_list();
    std::string qualified_name();
    std::string fragment();
    std::string plain_fragment(bool& complete);
    std::string template_name();
    std::string symbol();
    std::string symbol_body();
    Number number();
    static std::string number_text(const Number& n) {
        return (n.negative ? "-" : "") + std::to_string(n.value);
    }

    std::string_view in_;
    std::size_t pos_{0};
    int depth_{0};
    bool invalid_{false};
    bool stopped_{false};
    bool truncated_{false};
    std::vector<std::string> names_;
    std::vector<std::string> types_;
};

Parser::Number Parser::number() {
    Number n;
    if (at_end()) return n;
    if (peek() == '?') {
        n.negative = true;
        get();
        if (at_end()) return n;
    }
    const char c = get();
    if (c >= '0' && c <= '9') {
        n.value = static_cast<std::uint64_t>(c - '0') + 1;
        n.ok = true;
        return n;
    }
    if (c < 'A' || c > 'P') {
        invalid_ = true;
        return n;
    }
    n.value = static_cast<std::uint64_t>(c - 'A');
    for (;;) {
        if (at_end()) return n;
        const char h = get();
        if (h == '@') break;
        if (h < 'A' || h > 'P') {
            invalid_ = true;
            return n;
        }
        n.value = n.value * 16 + static_cast<std::uint64_t>(h - 'A');
    }
    n.ok = true;
    return n;
}

std::string Parser::plain_fragment(bool& complete) {
    std::string text;
    complete = false;
    while (pos_ < in_.size()) {
        const char c = get();
        if (c == '@') {
            complete = true;
            return text;
        }
        text += c;
    }
    truncated_ = true;
    return text;
}

std::string Parser::template_name() {
    Depth depth(*this);
    bool complete = false;
    const std::string name = plain_fragment(complete);
    if (!complete) return " ??";
    // A template argument list has its own back-reference tables, whose first
    // name is the template's own.
    auto saved_names = std::move(names_);
    auto saved_types = std::move(types_);
    names_ = {name};
    types_.clear();
    std::vector<std::string> args;
    bool ok = true;
    for (;;) {
        if (invalid_ || at_end()) {
            ok = false;
            break;
        }
        const char c = peek();
        if (c == '@') {
            get();
            break;
        }
        if (c == '$' && pos_ + 1 < in_.size() && in_[pos_ + 1] != '$') {
            get();
            const char k = get();
            if (k == '0') {
                const Number n = number();
                if (!n.ok) {
                    ok = false;
                    break;
                }
                args.push_back(number_text(n));
            } else if (k == '1') {
                args.push_back("&" + symbol());
            } else if (k == 'E') {
                args.push_back(symbol());
            } else {
                invalid_ = true;
                ok = false;
                break;
            }
            continue;
        }
        if (c >= '0' && c <= '9') {
            get();
            const auto index = static_cast<std::size_t>(c - '0');
            if (index >= types_.size()) {
                invalid_ = true;
                ok = false;
                break;
            }
            args.push_back(types_[index]);
            continue;
        }
        const std::size_t start = pos_;
        const NodeP arg = type(Mode::Arg);
        const std::string text = render(arg, Decl{});
        if (pos_ - start > 1) remember_type(text);
        args.push_back(text);
    }
    names_ = std::move(saved_names);
    types_ = std::move(saved_types);
    if (!ok) return " ??";
    std::string out = name + "<" + join(args, ",");
    out += (!out.empty() && out.back() == '>') ? " >" : ">";
    remember_name(out);
    return out;
}

std::string Parser::fragment() {
    Depth depth(*this);
    const char c = peek();
    if (c >= '0' && c <= '9') {
        get();
        const auto index = static_cast<std::size_t>(c - '0');
        if (index >= names_.size()) {
            invalid_ = true;
            return {};
        }
        return names_[index];
    }
    if (c == '?') {
        get();
        if (at_end()) return "??";
        const char k = peek();
        if (k == '$') {
            get();
            return template_name();
        }
        if (k == '?') return "`" + symbol() + "'";
        if (k == 'A') {
            bool complete = false;
            (void)plain_fragment(complete);
            const std::string name = "`anonymous namespace'";
            if (complete) remember_name(name);
            return name;
        }
        const Number n = number();
        if (!n.ok) return "??";
        return "`" + number_text(n) + "'";
    }
    bool complete = false;
    std::string name = plain_fragment(complete);
    remember_name(name);
    return name;
}

std::string Parser::qualified_name() {
    if (at_end()) return "??";
    const std::string first = fragment();
    std::vector<std::string> scopes;
    for (;;) {
        if (invalid_) return {};
        if (at_end()) {
            scopes.push_back("?? ");
            break;
        }
        if (peek() == '@') {
            get();
            break;
        }
        scopes.push_back(fragment());
    }
    std::string out;
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        out += *it;
        out += "::";
    }
    return out + first;
}

std::string Parser::symbol() {
    // A nested symbol is a complete mangled name with back-reference tables
    // of its own; the enclosing type's tables resume after it.
    auto saved_names = std::move(names_);
    auto saved_types = std::move(types_);
    names_.clear();
    types_.clear();
    std::string text = symbol_body();
    names_ = std::move(saved_names);
    types_ = std::move(saved_types);
    return text;
}

std::string Parser::symbol_body() {
    Depth depth(*this);
    if (at_end()) return "??";
    if (get() != '?') {
        invalid_ = true;
        return {};
    }
    if (at_end()) return "??";
    // The unqualified name: a plain or template name, or a special name.
    std::string name;
    enum class Special { None, Ctor, Dtor } special = Special::None;
    if (peek() == '?') {
        get();
        if (at_end()) return "??";
        const char k = get();
        if (k == '0') {
            special = Special::Ctor;
        } else if (k == '1') {
            special = Special::Dtor;
        } else if (k == '$') {
            name = template_name();
        } else if (k == '_') {
            const char* op = at_end() ? nullptr : extended_operator_name(get());
            if (op == nullptr) {
                invalid_ = true;
                return {};
            }
            name = op;
        } else if (const char* op = operator_name(k); op != nullptr) {
            name = op;
        } else {
            invalid_ = true;
            return {};
        }
    } else {
        name = fragment();
    }
    std::vector<std::string> scopes;
    for (;;) {
        if (invalid_) return {};
        if (at_end()) return "??";
        if (peek() == '@') {
            get();
            break;
        }
        scopes.push_back(fragment());
    }
    if (special != Special::None) {
        if (scopes.empty()) {
            invalid_ = true;
            return {};
        }
        name = (special == Special::Dtor ? "~" : "") + scopes.front();
    }
    std::string qualified;
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        qualified += *it;
        qualified += "::";
    }
    qualified += name;

    if (at_end()) return "??";
    const char kind = get();
    if (kind >= '0' && kind <= '4') {
        static constexpr const char* kAccess[] = {
            "private: static ", "protected: static ", "public: static ", "", ""};
        NodeP var = type(Mode::Pointee);
        while (!done() && (peek() == 'E' || peek() == 'I' || peek() == 'F')) get();
        if (at_end()) return "??";
        const char* cv = cv_suffix(get());
        if (cv == nullptr) {
            invalid_ = true;
            return {};
        }
        if (var->kind == Node::Kind::Simple) var->cv += cv;
        return kAccess[kind - '0'] + render(var, Decl{{}, qualified});
    }
    auto fn = make(Node::Kind::Function);
    fn->symbol_name = qualified;
    std::string prefix;
    if (kind == 'Y' || kind == 'Z') {
        // global function: the calling convention follows directly
    } else if (kind >= 'A' && kind <= 'X') {
        const int index = kind - 'A';
        static constexpr const char* kAccess[] = {"private: ", "protected: ",
                                                  "public: "};
        prefix = kAccess[index / 8];
        const int flavour = (index % 8) / 2;
        if (flavour == 3) {
            invalid_ = true;  // adjustor thunks carry offsets; not a type scope
            return {};
        }
        if (flavour == 1) prefix += "static ";
        if (flavour == 2) prefix += "virtual ";
        if (flavour != 1) {
            while (!done() && (peek() == 'E' || peek() == 'I' || peek() == 'F')) get();
            if (at_end()) return "??";
            const char* cv = cv_suffix(get());
            if (cv == nullptr) {
                invalid_ = true;
                return {};
            }
            fn->this_cv = *cv == ' ' ? std::string(cv + 1) : std::string(cv);
        }
    } else {
        invalid_ = true;
        return {};
    }
    function_tail(*fn);
    if (invalid_) return {};
    return prefix + render(fn, Decl{});
}

NodeP Parser::return_type() {
    if (at_end()) return make(Node::Kind::Missing);
    if (peek() == '?') {
        get();
        if (at_end()) return make(Node::Kind::Missing);
        const char* cv = cv_suffix(get());
        if (cv == nullptr) return fail();
        NodeP ret = type(Mode::Pointee);
        if (ret->kind == Node::Kind::Simple) ret->cv += cv;
        return ret;
    }
    return type(Mode::Pointee);
}

std::string Parser::arg_list() {
    if (at_end()) return " ??";
    if (peek() == 'X') {
        get();
        return "void";
    }
    std::vector<std::string> args;
    for (;;) {
        if (invalid_) return {};
        if (at_end()) {
            args.push_back(" ??");
            break;
        }
        const char c = peek();
        if (c == '@') {
            get();
            break;
        }
        if (c == 'Z') {
            get();
            args.push_back("...");
            break;
        }
        const std::size_t start = pos_;
        const NodeP arg = type(Mode::Arg);
        const std::string text = render(arg, Decl{});
        if (pos_ - start > 1) remember_type(text);
        args.push_back(text);
    }
    return join(args, ",");
}

/// Calling convention, return type, parameters and exception specification.
void Parser::function_tail(Node& fn) {
    if (at_end()) {
        fn.unknown_convention = true;
        fn.ret = make(Node::Kind::Missing);
        return;
    }
    const char* cc = convention(get());
    if (cc == nullptr) {
        fn.unknown_convention = true;
        fn.ret = return_type();
        stopped_ = true;
        return;
    }
    fn.convention = cc;
    if (!done() && peek() == '@') {
        get();
        fn.has_return = false;
    } else {
        fn.ret = return_type();
    }
    fn.args = arg_list();
    if (at_end()) {
        fn.throw_spec = " throw( ?? )";
    } else if (get() != 'Z') {
        invalid_ = true;
    }
}

NodeP Parser::function_type(std::string member_class, std::string this_cv) {
    Depth depth(*this);
    auto fn = make(Node::Kind::Function);
    fn->member_class = std::move(member_class);
    fn->this_cv = std::move(this_cv);
    function_tail(*fn);
    return fn;
}

NodeP Parser::array(std::string array_cv) {
    Depth depth(*this);
    auto node = make(Node::Kind::Array);
    node->array_cv = std::move(array_cv);
    const Number count = number();
    if (!count.ok) {
        if (invalid_) return node;
        node->dims = "[ ?? ]";
    } else {
        for (std::uint64_t i = 0; i < count.value && !invalid_; ++i) {
            const Number dim = number();
            if (!dim.ok) {
                if (invalid_) return node;
                node->dims += "[ ?? ]";
                break;
            }
            node->dims += "[" + number_text(dim) + "]";
        }
    }
    node->element = type(Mode::Pointee);
    return node;
}

NodeP Parser::pointer(char kind) {
    Depth depth(*this);
    auto node = make(Node::Kind::Pointer);
    const bool reference = kind == 'A' || kind == 'B';
    std::string own_cv;
    switch (kind) {
    case 'Q': own_cv = " const"; break;
    case 'R': own_cv = " volatile"; break;
    case 'S': own_cv = " const volatile"; break;
    case 'B': own_cv = " volatile"; break;
    default: break;
    }
    std::string mods;
    bool unaligned = false;
    while (!done()) {
        const char m = peek();
        if (m == 'E') {
            mods += " __ptr64";
        } else if (m == 'I') {
            mods += " __restrict";
        } else if (m == 'F') {
            unaligned = true;
        } else {
            break;
        }
        get();
    }
    node->token = std::string(reference ? "&" : "*") + mods + own_cv;
    if (at_end()) {
        node->pointee = make(Node::Kind::Missing);
        return node;
    }
    const char k = get();
    if (k == '6') {
        node->pointee = function_type({}, {});
        return node;
    }
    if (k == '8') {
        std::string cls = qualified_name();
        while (!done() && (peek() == 'E' || peek() == 'I' || peek() == 'F')) get();
        if (at_end()) {
            node->pointee = make(Node::Kind::Missing);
            return node;
        }
        const char* cv = cv_suffix(get());
        if (cv == nullptr) return fail();
        node->pointee =
            function_type(std::move(cls), *cv == ' ' ? std::string(cv + 1) : std::string());
        return node;
    }
    if (k >= 'Q' && k <= 'T') {
        // Pointer to data member: the class, then the member's type.
        const std::string cls = qualified_name();
        node->token = cls + "::*" + mods + own_cv;
        const char* cv = cv_suffix(static_cast<char>('A' + (k - 'Q')));
        NodeP member = type(Mode::Pointee);
        if (member->kind == Node::Kind::Simple) member->cv += cv;
        node->pointee = member;
        return node;
    }
    const char* cv = cv_suffix(k);
    if (cv == nullptr) return fail();
    if (!done() && peek() == 'Y') {
        get();
        node->pointee = array(*cv == ' ' ? std::string(cv + 1) + " " : std::string());
        return node;
    }
    NodeP pointee = type(Mode::Pointee);
    std::string suffix = cv;
    if (unaligned) suffix += " __unaligned";
    if (pointee->kind == Node::Kind::Simple) {
        pointee->cv += suffix;
    } else if (pointee->kind == Node::Kind::Pointer) {
        pointee->token += suffix;
    }
    node->pointee = pointee;
    return node;
}

NodeP Parser::type(Mode mode) {
    Depth depth(*this);
    if (invalid_) return make(Node::Kind::Missing);
    if (at_end()) return make(Node::Kind::Missing);
    const char c = get();
    if (const char* name = builtin(c); name != nullptr) return simple(name);
    switch (c) {
    case '_': {
        if (at_end()) return make(Node::Kind::Missing);
        const char k = get();
        if (k == 'X') return simple("coclass " + qualified_name());
        if (k == 'Y') return simple("cointerface " + qualified_name());
        return simple(extended_builtin(k));
    }
    case 'T':
        return simple("union " + qualified_name());
    case 'U':
        return simple("struct " + qualified_name());
    case 'V':
        return simple("class " + qualified_name());
    case 'W': {
        if (at_end()) return make(Node::Kind::Missing);
        static constexpr const char* kBase[] = {
            "char ", "unsigned char ", "short ", "unsigned short ", "",
            "unsigned int ", "long ", "unsigned long "};
        const char k = get();
        if (k < '0' || k > '7') return fail();
        return simple(std::string("enum ") + kBase[k - '0'] + qualified_name());
    }
    case 'P': case 'Q': case 'R': case 'S': case 'A': case 'B':
        return pointer(c);
    case '?': {
        if (mode != Mode::TopLevel) return fail();
        if (at_end()) return make(Node::Kind::Missing);
        const char* cv = cv_suffix(get());
        if (cv == nullptr) return fail();
        NodeP inner = type(Mode::Pointee);
        if (inner->kind == Node::Kind::Simple) inner->cv += cv;
        return inner;
    }
    case '$': {
        if (get() != '$') return fail();
        if (at_end()) return make(Node::Kind::Missing);
        const char k = get();
        if (k == 'C') {
            // A cv-qualified type argument prints its qualifier with a
            // trailing blank: "pair<int const ,int>".
            if (at_end()) return make(Node::Kind::Missing);
            const char* cv = cv_suffix(get());
            if (cv == nullptr) return fail();
            NodeP inner = type(Mode::Pointee);
            if (inner->kind == Node::Kind::Simple && *cv != '\0') {
                inner->cv += std::string(cv) + " ";
            }
            return inner;
        }
        if (k == 'B') {
            if (at_end()) return make(Node::Kind::Missing);
            if (get() != 'Y') return fail();
            return array({});
        }
        if (k == 'A') {
            if (at_end()) return make(Node::Kind::Missing);
            if (get() != '6') return fail();
            return function_type({}, {});
        }
        return fail();  // $$Q / $$R (rvalue references) and the rest
    }
    default:
        if (c >= '0' && c <= '9' && mode == Mode::Arg) {
            const auto index = static_cast<std::size_t>(c - '0');
            if (index >= types_.size()) return fail();
            return simple(types_[index]);
        }
        return fail();
    }
}

} // namespace

std::string msvcrt_type_info_name(std::string_view decorated) {
    // name() undecorates what follows the leading '.'.
    const std::string_view body =
        decorated.empty() ? std::string_view{} : decorated.substr(1);
    Parser parser(body);
    const NodeP node = parser.type(Parser::Mode::TopLevel);
    std::string out;
    if (parser.invalid() ||
        (!parser.stopped() && !parser.truncated() && !parser.fully_consumed())) {
        out = std::string(body);
    } else {
        out = render(node, Decl{});
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace vitreo::winapi

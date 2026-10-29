// SPDX-License-Identifier: LGPL-2.1-only

/// @file msvc_demangle.cpp
/// @brief See msvc_demangle.hpp.

#include "winapi/system/dbghelp/msvc_demangle.hpp"

#include <vector>

namespace vitreo::winapi {
namespace {

/// Recursive-descent reader over a decorated name.
///
/// The grammar is:  '?' <name> '@' [<scope> '@']* '@' <what>
/// where <what> is '3' <type> <storage>            for data, or
///                 'Y' <callconv> <ret> <args> 'Z' for a free function, or
///                 <access><callconv> ...          for a member function.
class Reader {
public:
    explicit Reader(const std::string& s) : m_s(s) {}

    bool eof() const { return m_i >= m_s.size(); }
    char peek() const { return eof() ? '\0' : m_s[m_i]; }
    char get() { return eof() ? '\0' : m_s[m_i++]; }
    bool consume(char c) {
        if (peek() == c) { ++m_i; return true; }
        return false;
    }
    bool ok() const { return m_ok; }
    void fail() { m_ok = false; }

    /// A name fragment runs to the next '@'.
    std::string name_fragment() {
        std::string out;
        while (!eof() && peek() != '@') out.push_back(get());
        if (!consume('@')) fail();
        return out;
    }

    std::string builtin() {
        switch (get()) {
            case 'X': return "void";
            case 'D': return "char";
            case 'C': return "signed char";
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
            case '_':
                switch (get()) {
                    case 'J': return "__int64";
                    case 'K': return "unsigned __int64";
                    case 'N': return "bool";
                    case 'W': return "wchar_t";
                    default: fail(); return {};
                }
            default: fail(); return {};
        }
    }

    /// 'A' none, 'B' const, 'C' volatile, 'D' const volatile.
    static const char* cv_for(char c) {
        switch (c) {
            case 'B': return " const";
            case 'C': return " volatile";
            case 'D': return " const volatile";
            default:  return "";
        }
    }

    std::string type() {
        const char c = peek();
        // Pointer family. 'E' between the pointer and its cv marks __ptr64.
        if (c == 'P' || c == 'Q' || c == 'R' || c == 'S') {
            get();
            const bool ptr64 = consume('E');
            const char cv = get();
            std::string inner = type();
            if (!m_ok) return {};
            std::string out = inner + cv_for(cv) + " *";
            if (ptr64) out += " __ptr64";
            return out;
        }
        if (c == 'A') {                       // reference
            get();
            const bool ptr64 = consume('E');
            const char cv = get();
            std::string inner = type();
            if (!m_ok) return {};
            std::string out = inner + cv_for(cv) + " &";
            if (ptr64) out += " __ptr64";
            return out;
        }
        if (c == 'V' || c == 'U' || c == 'T' || c == 'W') {   // class/struct/union/enum
            get();
            const char* kind = (c == 'V') ? "class " : (c == 'U') ? "struct "
                             : (c == 'T') ? "union " : "enum ";
            std::vector<std::string> parts;
            while (!eof() && peek() != '@') parts.push_back(name_fragment());
            consume('@');
            std::string q;
            for (usize i = parts.size(); i-- > 0;) {
                if (!q.empty()) q += "::";
                q += parts[i];
            }
            return std::string(kind) + q;
        }
        return builtin();
    }

    static const char* callconv(char c) {
        switch (c) {
            case 'A': case 'B': return "__cdecl";
            case 'C': case 'D': return "__pascal";
            case 'E': case 'F': return "__thiscall";
            case 'G': case 'H': return "__stdcall";
            case 'I': case 'J': return "__fastcall";
            default: return "__cdecl";
        }
    }

    usize pos() const { return m_i; }

private:
    const std::string& m_s;
    usize m_i = 0;
    bool m_ok = true;
};

} // namespace

std::string msvc_undecorate(const std::string& mangled, u32 flags) {
    if (mangled.empty()) return {};
    // Windows passes an undecorated name straight through -- and reports its
    // length, not zero.
    if (mangled[0] != '?') return mangled;

    Reader r(mangled);
    r.get();                                   // '?'
    // Operators and other '?'-prefixed special names are not modelled; hand
    // back the raw text rather than a wrong rendering.
    if (r.peek() == '?') return mangled;

    const std::string base = r.name_fragment();
    std::vector<std::string> scopes;
    while (r.ok() && !r.eof() && r.peek() != '@') {
        scopes.push_back(r.name_fragment());
    }
    r.consume('@');                            // the '@' closing the scope list

    std::string qualified;
    for (usize i = scopes.size(); i-- > 0;) qualified += scopes[i] + "::";
    qualified += base;

    if ((flags & kUndnameNameOnly) != 0) return qualified;
    if (!r.ok() || r.eof()) return qualified;

    const char what = r.get();
    if (what == '3' || what == '2' || what == '1' || what == '0') {
        // Data. The trailing storage-class letter adds nothing readable.
        const std::string t = r.type();
        if (!r.ok()) return qualified;
        return t + " " + qualified;
    }

    // Function. 'Y' is a free function; anything else in the access-specifier
    // range introduces a member, whose cv-qualifier follows the convention.
    const char* cc = nullptr;
    if (what == 'Y') {
        cc = Reader::callconv(r.get());
    } else if (what >= 'A' && what <= 'X') {
        if (what >= 'E' && what <= 'X') (void)r.get();   // member cv qualifier
        cc = Reader::callconv(r.get());
    } else {
        return qualified;
    }

    const std::string ret = r.type();
    if (!r.ok()) return qualified;

    std::vector<std::string> args;
    while (r.ok() && !r.eof() && r.peek() != 'Z' && r.peek() != '@') {
        args.push_back(r.type());
    }
    if (!r.ok()) return qualified;
    if (args.size() == 1 && args[0] == "void") args.clear();

    std::string out = ret + " " + cc + " " + qualified + "(";
    for (usize i = 0; i < args.size(); ++i) {
        if (i) out += ",";
        out += args[i];
    }
    if (args.empty()) out += "void";
    out += ")";
    return out;
}

} // namespace vitreo::winapi

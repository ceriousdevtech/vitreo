// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <string_view>
#include <string>
#include <cctype>
namespace vitreo::capsule {
// File-I/O namespaces, deliberately separate from GetFullPathName's lexical
// normalization. Source: Learn Naming Files; disk_space_namespace_probe.
struct Win32NamespacePath {
    enum class Kind { Dos, VerbatimDos, DeviceDos, NativeDos, Network, OtherDevice };
    Kind kind = Kind::Dos;
    std::string_view body;
    bool literal() const { return kind == Kind::VerbatimDos || kind == Kind::NativeDos; }
    bool prefixed_dos() const { return literal() || kind == Kind::DeviceDos; }
    static bool drive(std::string_view p) {
        return p.size() >= 2 && ((p[0] >= 'a' && p[0] <= 'z') ||
            (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':';
    }
};
inline Win32NamespacePath win32_namespace_path(std::string_view p) {
    using K = Win32NamespacePath::Kind;
    if (p.starts_with("\\\\?\\") || p.starts_with("\\??\\") || p.starts_with("\\\\.\\")) {
        auto body = p.substr(4);
        if (Win32NamespacePath::drive(body)) return {
            p.starts_with("\\\\.\\") ? K::DeviceDos : p.starts_with("\\??\\") ? K::NativeDos : K::VerbatimDos, body};
        if (body.size() >= 4 && (body[0] == 'U' || body[0] == 'u') &&
            (body[1] == 'N' || body[1] == 'n') && (body[2] == 'C' || body[2] == 'c') && body[3] == '\\')
            return {K::Network, body.substr(4)};
        return {K::OtherDevice, body};
    }
    if (p.starts_with("\\\\")) return {K::Network, p.substr(2)};
    return {K::Dos, p};
}
// GetFullPathName preserves periods on intermediate components. DOS file I/O
// removes them, while preserving intermediate spaces (native namespace probe).
inline std::string win32_dos_lookup_path(std::string path) {
    const auto ns = win32_namespace_path(path);
    if (ns.literal()) return path;
    std::string::size_type begin = ns.kind == Win32NamespacePath::Kind::DeviceDos ? 4 : 0;
    while (begin < path.size()) {
        auto end = path.find('\\', begin);
        if (end == std::string::npos) break; // final component already normalized
        auto last = end;
        while (last > begin && path[last-1] == '.') --last;
        if (last != end && !(last == begin && end-begin <= 2)) {
            path.erase(last, end-last); end = last;
        }
        begin = end+1;
    }
    return path;
}
} // namespace vitreo::capsule
